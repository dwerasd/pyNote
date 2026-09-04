from __future__ import annotations

import sqlite3
from datetime import UTC, datetime, timedelta
from pathlib import Path

import pytest

from pynote.application.card_service import CardService
from pynote.domain.events import EventSource
from pynote.domain.models import (
    CaptureOperationSource,
    CardSource,
    Document,
    NewCaptureOperation,
    NewCard,
    RevisionSource,
    SplitPolicy,
)
from pynote.infrastructure import backup as backup_module
from pynote.infrastructure.backup import (
    AutomaticBackupManager,
    BackupError,
    BackupIntegrityError,
    MigrationBackupHook,
    PeriodicQuickCheck,
    UnsupportedBackupError,
    create_database_backup,
    inspect_backup,
    restore_database,
)
from pynote.infrastructure.database import Database
from pynote.infrastructure.migrations import LATEST_SCHEMA_VERSION
from pynote.infrastructure.repositories import Repositories


def _seed_database(path: Path) -> None:
    with Database(path) as database:
        repositories = Repositories(database)
        repositories.create_document(
            Document(
                id="document-1",
                title="백업 문서",
                created_at_us=1,
                updated_at_us=1,
            )
        )
        identifiers = (f"id-{number}" for number in range(30))
        service = CardService(
            database,
            repositories,
            clock=lambda: 20,
            id_factory=identifiers.__next__,
        )
        service.create_cards(
            "document-1",
            "첫 카드\n\n둘째 카드",
            source=CaptureOperationSource.IMPORT,
            split=True,
        )


def _content_snapshot(path: Path) -> tuple[tuple[object, ...], ...]:
    # sqlite3 커넥션의 컨텍스트 매니저는 트랜잭션만 관리하고 커넥션을 닫지
    # 않는다. 남은 핸들이 있으면 Windows가 이 파일의 교체를 거부해 뒤따르는
    # 복원이 PermissionError로 실패한다.
    connection = sqlite3.connect(path)
    try:
        rows: list[tuple[object, ...]] = []
        for table, order in (
            ("cards", "capture_seq"),
            ("card_revisions", "event_seq"),
            ("counters", "name"),
        ):
            table_rows = connection.execute(
                f"SELECT * FROM {table} ORDER BY {order}"
            ).fetchall()
            rows.extend(tuple(row) for row in table_rows)
        return tuple(rows)
    finally:
        connection.close()


def test_backup_restore_round_trip_preserves_cards_revisions_and_sequences(
    tmp_path: Path,
) -> None:
    database_path = tmp_path / "pynote.sqlite3"
    backup_path = tmp_path / "backup.sqlite3"
    _seed_database(database_path)
    expected = _content_snapshot(database_path)
    create_database_backup(database_path, backup_path)

    with Database(database_path) as database:
        repositories = Repositories(database)
        CardService(database, repositories).create_card("document-1", "백업 뒤 변경")

    restore_database(backup_path, database_path, overwrite=True)

    assert _content_snapshot(database_path) == expected
    # 성공 경로에서도 임시 본체와 비켜두기 예약 파일이 남지 않는다.
    assert not tuple(tmp_path.glob("*.tmp"))


def test_backup_restore_preserves_existing_extended_whitespace_card_exactly(
    tmp_path: Path,
) -> None:
    database_path = tmp_path / "pynote.sqlite3"
    backup_path = tmp_path / "backup.sqlite3"
    document = Document(
        id="existing-whitespace-document",
        title="기존 공백 카드",
        created_at_us=1,
        updated_at_us=1,
    )
    body = "\v\f"
    tables = ("cards", "card_revisions", "capture_operations", "edit_events")

    with Database(database_path) as database:
        repositories = Repositories(database)
        repositories.create_document(document)
        card = repositories.create_cards(
            NewCaptureOperation(
                id="existing-whitespace-operation",
                document_id=document.id,
                source=CaptureOperationSource.TYPING,
                split_policy=SplitPolicy.KEEP,
                original_text=None,
                created_at_us=2_000,
            ),
            [
                NewCard(
                    id="existing-whitespace-card",
                    revision_id="existing-whitespace-revision",
                    event_id="existing-whitespace-event",
                    position_key=1_024,
                    body=body,
                    card_source=CardSource.TYPING,
                    event_source=EventSource.TYPING,
                    revision_source=RevisionSource.EDIT,
                    created_at_us=2_000,
                )
            ],
        )[0]
        before_rows = tuple(
            (
                table,
                tuple(
                    tuple(row)
                    for row in database.connection.execute(
                        f"SELECT * FROM {table} ORDER BY rowid"
                    ).fetchall()
                ),
            )
            for table in tables
        )
        before_body_bytes = (
            tuple(
                tuple(row)
                for row in database.connection.execute(
                    "SELECT id, CAST(body AS BLOB) FROM cards ORDER BY capture_seq"
                ).fetchall()
            ),
            tuple(
                tuple(row)
                for row in database.connection.execute(
                    "SELECT id, CAST(body AS BLOB) FROM card_revisions ORDER BY event_seq"
                ).fetchall()
            ),
        )
        before_deleted_at = tuple(
            tuple(row)
            for row in database.connection.execute(
                "SELECT id, deleted_at_us FROM cards ORDER BY capture_seq"
            ).fetchall()
        )
        before_events = repositories.list_events(document.id)
        assert card.body == body
        assert card.deleted_at_us is None

    create_database_backup(database_path, backup_path)
    restore_database(backup_path, database_path, overwrite=True)

    with Database(database_path) as database:
        repositories = Repositories(database)
        after_rows = tuple(
            (
                table,
                tuple(
                    tuple(row)
                    for row in database.connection.execute(
                        f"SELECT * FROM {table} ORDER BY rowid"
                    ).fetchall()
                ),
            )
            for table in tables
        )
        after_body_bytes = (
            tuple(
                tuple(row)
                for row in database.connection.execute(
                    "SELECT id, CAST(body AS BLOB) FROM cards ORDER BY capture_seq"
                ).fetchall()
            ),
            tuple(
                tuple(row)
                for row in database.connection.execute(
                    "SELECT id, CAST(body AS BLOB) FROM card_revisions ORDER BY event_seq"
                ).fetchall()
            ),
        )
        after_deleted_at = tuple(
            tuple(row)
            for row in database.connection.execute(
                "SELECT id, deleted_at_us FROM cards ORDER BY capture_seq"
            ).fetchall()
        )
        after_events = repositories.list_events(document.id)

    assert after_rows == before_rows
    assert after_body_bytes == before_body_bytes
    assert after_deleted_at == before_deleted_at
    assert after_events == before_events


def test_corrupt_backup_is_rejected_before_destination_changes(tmp_path: Path) -> None:
    backup_path = tmp_path / "broken.sqlite3"
    destination = tmp_path / "live.sqlite3"
    backup_path.write_bytes(b"not sqlite")
    destination.write_bytes(b"keep me")

    with pytest.raises(BackupIntegrityError):
        restore_database(backup_path, destination, overwrite=True)

    assert destination.read_bytes() == b"keep me"


def test_newer_schema_backup_is_rejected_before_destination_changes(
    tmp_path: Path,
) -> None:
    backup_path = tmp_path / "future.sqlite3"
    destination = tmp_path / "live.sqlite3"
    with sqlite3.connect(backup_path) as connection:
        connection.execute(
            "CREATE TABLE schema_version(id INTEGER PRIMARY KEY, version INTEGER)"
        )
        connection.execute(
            "INSERT INTO schema_version(id, version) VALUES (1, ?)",
            (LATEST_SCHEMA_VERSION + 1,),
        )
    destination.write_bytes(b"keep me")

    with pytest.raises(UnsupportedBackupError):
        restore_database(backup_path, destination, overwrite=True)

    assert destination.read_bytes() == b"keep me"


def test_incomplete_current_schema_is_rejected_before_destination_changes(
    tmp_path: Path,
) -> None:
    backup_path = tmp_path / "not-pynote.sqlite3"
    destination = tmp_path / "live.sqlite3"
    with sqlite3.connect(backup_path) as connection:
        connection.execute(
            "CREATE TABLE schema_version(id INTEGER PRIMARY KEY, version INTEGER)"
        )
        connection.execute(
            "INSERT INTO schema_version(id, version) VALUES (1, ?)",
            (LATEST_SCHEMA_VERSION,),
        )
    destination.write_bytes(b"keep me")

    with pytest.raises(BackupIntegrityError):
        restore_database(backup_path, destination, overwrite=True)

    assert destination.read_bytes() == b"keep me"


def test_v4_backup_requires_workspace_windows_instead_of_workspace_state(
    tmp_path: Path,
) -> None:
    backup_path = tmp_path / "missing-workspace-windows.sqlite3"
    _seed_database(backup_path)
    with sqlite3.connect(backup_path) as connection:
        connection.execute("DROP TABLE workspace_windows")
        connection.execute(
            """
            CREATE TABLE workspace_state(
                id INTEGER PRIMARY KEY,
                open_document_ids_json TEXT NOT NULL,
                active_document_id TEXT,
                updated_at_us INTEGER NOT NULL
            )
            """
        )

    with pytest.raises(BackupIntegrityError, match="workspace_windows"):
        inspect_backup(backup_path)


def test_v3_backup_still_requires_and_accepts_workspace_state(
    tmp_path: Path,
) -> None:
    backup_path = tmp_path / "v3-backup.sqlite3"
    _seed_database(backup_path)
    with sqlite3.connect(backup_path) as connection:
        connection.execute("DROP TABLE workspace_windows")
        connection.execute(
            """
            CREATE TABLE workspace_state(
                id INTEGER PRIMARY KEY,
                open_document_ids_json TEXT NOT NULL,
                active_document_id TEXT,
                updated_at_us INTEGER NOT NULL
            )
            """
        )
        connection.execute(
            "UPDATE schema_version SET version = 3, applied_at_us = 3 WHERE id = 1"
        )

    assert inspect_backup(backup_path).schema_version == 3


def test_restore_rejects_sidecar_directory_before_database_set_changes(
    tmp_path: Path,
) -> None:
    source = tmp_path / "source.sqlite3"
    backup_path = tmp_path / "backup.sqlite3"
    destination = tmp_path / "live.sqlite3"
    wal_path = Path(f"{destination}-wal")
    shm_path = Path(f"{destination}-shm")
    _seed_database(source)
    create_database_backup(source, backup_path)
    destination.write_bytes(b"original database")
    wal_path.write_bytes(b"original wal")
    shm_path.mkdir()

    with pytest.raises(BackupError, match="DB 세트 경로"):
        restore_database(backup_path, destination, overwrite=True)

    assert destination.read_bytes() == b"original database"
    assert wal_path.read_bytes() == b"original wal"
    assert shm_path.is_dir()


def test_restore_rolls_back_database_set_when_publish_fails(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    source = tmp_path / "source.sqlite3"
    backup_path = tmp_path / "backup.sqlite3"
    destination = tmp_path / "live.sqlite3"
    wal_path = Path(f"{destination}-wal")
    shm_path = Path(f"{destination}-shm")
    _seed_database(source)
    create_database_backup(source, backup_path)
    destination.write_bytes(b"original database")
    wal_path.write_bytes(b"original wal")
    shm_path.write_bytes(b"original shm")
    real_replace = backup_module.os.replace
    failed = False

    def fail_first_publish(source_path: Path, destination_path: Path) -> None:
        nonlocal failed
        if not failed and Path(destination_path) == destination:
            failed = True
            raise OSError("게시 실패 주입")
        real_replace(source_path, destination_path)

    monkeypatch.setattr(backup_module.os, "replace", fail_first_publish)

    with pytest.raises(OSError, match="게시 실패 주입"):
        restore_database(backup_path, destination, overwrite=True)

    assert destination.read_bytes() == b"original database"
    assert wal_path.read_bytes() == b"original wal"
    assert shm_path.read_bytes() == b"original shm"


def test_restore_keeps_original_database_when_first_move_aside_fails(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    source = tmp_path / "source.sqlite3"
    backup_path = tmp_path / "backup.sqlite3"
    destination = tmp_path / "live.sqlite3"
    wal_path = Path(f"{destination}-wal")
    shm_path = Path(f"{destination}-shm")
    _seed_database(source)
    create_database_backup(source, backup_path)
    destination.write_bytes(b"original database")
    wal_path.write_bytes(b"original wal")
    shm_path.write_bytes(b"original shm")
    real_replace = backup_module.os.replace
    calls = 0

    def fail_first_move_aside(source_path: Path, destination_path: Path) -> None:
        nonlocal calls
        calls += 1
        if calls == 1:
            raise OSError("비켜두기 실패 주입")
        real_replace(source_path, destination_path)

    monkeypatch.setattr(backup_module.os, "replace", fail_first_move_aside)

    with pytest.raises(OSError, match="비켜두기 실패 주입"):
        restore_database(backup_path, destination, overwrite=True)

    # 구 롤백 1단계 분기는 정확히 이 상태에서 원본 DB 를 임시 이름으로 옮겨
    # finally 가 지우게 했다. 이제 원본 세트는 제자리에 남고 예약 파일도 남지 않는다.
    assert calls == 1
    assert destination.read_bytes() == b"original database"
    assert wal_path.read_bytes() == b"original wal"
    assert shm_path.read_bytes() == b"original shm"
    assert not tuple(tmp_path.glob("*.tmp"))


def test_interrupted_move_aside_keeps_original_in_reservation(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    source = tmp_path / "source.sqlite3"
    backup_path = tmp_path / "backup.sqlite3"
    destination = tmp_path / "live.sqlite3"
    _seed_database(source)
    create_database_backup(source, backup_path)
    destination.write_bytes(b"original database")
    real_replace = backup_module.os.replace
    armed = {"value": True}

    def move_then_interrupt(source_path: Path, destination_path: Path) -> None:
        # 효과는 내고 경계에서 시그널 예외가 드는 창을 재현한다 - moved_paths 기록 전이다.
        real_replace(source_path, destination_path)
        if armed["value"]:
            armed["value"] = False
            raise KeyboardInterrupt("이동 직후 중단 주입")

    monkeypatch.setattr(backup_module.os, "replace", move_then_interrupt)

    with pytest.raises(KeyboardInterrupt):
        restore_database(backup_path, destination, overwrite=True)

    # 원본은 기록 없는 예약 파일 안에 살아남아야 한다 - 정리 루프가 지우면 안 된다.
    leftovers = tuple(tmp_path.glob("*.tmp"))
    assert not destination.exists()
    assert len(leftovers) == 1
    assert leftovers[0].read_bytes() == b"original database"


def test_interrupted_install_unpublishes_new_database(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    source = tmp_path / "source.sqlite3"
    backup_path = tmp_path / "backup.sqlite3"
    destination = tmp_path / "live.sqlite3"
    _seed_database(source)
    create_database_backup(source, backup_path)
    real_replace = backup_module.os.replace
    armed = {"value": True}

    def install_then_interrupt(source_path: Path, destination_path: Path) -> None:
        real_replace(source_path, destination_path)
        if armed["value"] and Path(destination_path) == destination:
            armed["value"] = False
            raise KeyboardInterrupt("설치 직후 중단 주입")

    monkeypatch.setattr(backup_module.os, "replace", install_then_interrupt)

    with pytest.raises(KeyboardInterrupt):
        restore_database(backup_path, destination, overwrite=True)

    # 진입 시점에 없던 destination 이 남아 있으면 실패 보고와 게시가 모순된다.
    assert not destination.exists()
    assert not tuple(tmp_path.glob("*.tmp"))


def test_reservation_cleanup_failure_keeps_original_error(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    source = tmp_path / "source.sqlite3"
    backup_path = tmp_path / "backup.sqlite3"
    destination = tmp_path / "live.sqlite3"
    _seed_database(source)
    create_database_backup(source, backup_path)
    destination.write_bytes(b"original database")
    real_replace = backup_module.os.replace
    real_unlink = Path.unlink
    replace_calls = {"value": 0}
    unlink_armed = {"value": True}

    def fail_first_move_aside(source_path: Path, destination_path: Path) -> None:
        replace_calls["value"] += 1
        if replace_calls["value"] == 1:
            raise OSError("비켜두기 실패 주입")
        real_replace(source_path, destination_path)

    def fail_first_reservation_unlink(self: Path, missing_ok: bool = False) -> None:
        if unlink_armed["value"] and self.suffix == ".tmp":
            unlink_armed["value"] = False
            raise PermissionError("예약 정리 거부 주입")
        real_unlink(self, missing_ok=missing_ok)

    monkeypatch.setattr(backup_module.os, "replace", fail_first_move_aside)
    monkeypatch.setattr(Path, "unlink", fail_first_reservation_unlink)

    # 정리 실패가 원래 오류(비켜두기 실패)를 가리면 안 된다.
    with pytest.raises(OSError, match="비켜두기 실패 주입"):
        restore_database(backup_path, destination, overwrite=True)

    assert destination.read_bytes() == b"original database"
    # 지우지 못한 예약 파일 하나는 잔존을 허용한다 - 오류 보존이 우선이다.
    assert len(tuple(tmp_path.glob("*.tmp"))) <= 1


def test_temporary_database_path_reserves_name_until_replaced(
    tmp_path: Path,
) -> None:
    destination = tmp_path / "live.sqlite3"

    first = backup_module._temporary_database_path(destination)
    second = backup_module._temporary_database_path(destination)

    # 예약 파일이 남아 있는 동안 배타 생성은 같은 이름을 다시 내줄 수 없다.
    assert first != second
    assert first.is_file()
    assert second.is_file()
    assert first.parent == destination.parent

    # 예약 파일은 os.replace 의 대상이 된다 - 비켜두기가 그 위로 교체한다.
    payload = tmp_path / "payload.bin"
    payload.write_bytes(b"payload")
    backup_module.os.replace(payload, first)
    assert first.read_bytes() == b"payload"


def test_dangling_foreign_key_backup_is_rejected_before_destination_changes(
    tmp_path: Path,
) -> None:
    source = tmp_path / "source.sqlite3"
    backup_path = tmp_path / "backup.sqlite3"
    destination = tmp_path / "live.sqlite3"
    _seed_database(source)
    create_database_backup(source, backup_path)
    with sqlite3.connect(backup_path) as connection:
        operation_id = connection.execute(
            "SELECT operation_id FROM cards LIMIT 1"
        ).fetchone()[0]
        connection.execute(
            "DELETE FROM capture_operations WHERE id = ?",
            (operation_id,),
        )
        assert connection.execute("PRAGMA quick_check").fetchone() == ("ok",)
        assert connection.execute("PRAGMA foreign_key_check").fetchall()
    destination.write_bytes(b"keep me")

    with pytest.raises(BackupIntegrityError, match="FK 무결성"):
        inspect_backup(backup_path)
    with pytest.raises(BackupIntegrityError, match="FK 무결성"):
        restore_database(backup_path, destination, overwrite=True)

    assert destination.read_bytes() == b"keep me"


def test_migration_hook_creates_valid_pre_migration_backup(tmp_path: Path) -> None:
    database_path = tmp_path / "previous.sqlite3"
    backup_directory = tmp_path / "backups"
    with sqlite3.connect(database_path) as connection:
        connection.execute(
            """
            CREATE TABLE schema_version(
                id INTEGER PRIMARY KEY,
                version INTEGER NOT NULL,
                applied_at_us INTEGER NOT NULL
            )
            """
        )
        connection.execute(
            "INSERT INTO schema_version(id, version, applied_at_us) VALUES (1, 0, 0)"
        )
    hook = MigrationBackupHook(
        backup_directory,
        clock=lambda: datetime(2026, 1, 2, tzinfo=UTC),
    )

    with Database(database_path, backup_hook=hook):
        pass

    assert hook.last_backup_path is not None
    assert inspect_backup(hook.last_backup_path).schema_version == 0


def test_automatic_backup_obeys_interval_and_quick_check_period(
    tmp_path: Path,
) -> None:
    database_path = tmp_path / "pynote.sqlite3"
    _seed_database(database_path)
    now = datetime(2026, 1, 1, tzinfo=UTC)
    times = iter((now, now + timedelta(hours=1), now + timedelta(hours=25)))
    manager = AutomaticBackupManager(
        database_path,
        tmp_path / "backups",
        interval_hours=24,
        clock=times.__next__,
    )

    first = manager.run_if_due()
    second = manager.run_if_due()
    third = manager.run_if_due()

    assert first is not None
    assert second is None
    assert third is not None

    with sqlite3.connect(database_path) as connection:
        ticks = iter((0.0, 1.0, 3_601.0))
        checker = PeriodicQuickCheck(
            connection,
            interval_hours=1,
            clock=ticks.__next__,
        )
        assert checker.run_if_due()
        assert not checker.run_if_due()
        assert checker.run_if_due()
