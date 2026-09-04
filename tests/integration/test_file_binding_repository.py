from __future__ import annotations

import sqlite3
from dataclasses import replace
from pathlib import Path

import pytest

from pynote.application.card_service import CardService
from pynote.application.file_binding_service import (
    BindingPathStatus,
    prepare_binding_path,
)
from pynote.application.purge_service import PurgeService
from pynote.domain.models import (
    CaptureOperationSource,
    Card,
    Document,
    FileBinding,
    NewlineKind,
)
from pynote.infrastructure.database import Database
from pynote.infrastructure.migrations import LATEST_SCHEMA_VERSION
from pynote.infrastructure.repositories import Repositories

DAY_US = 24 * 60 * 60 * 1_000_000
NOW_US = 100 * DAY_US


def _document(repositories: Repositories, document_id: str = "document") -> Document:
    document = Document(
        id=document_id,
        title="결속 검증",
        created_at_us=1,
        updated_at_us=1,
    )
    repositories.create_document(document)
    return document


def _binding(card: Card, path: str, *, bound_at_us: int = 1_000) -> FileBinding:
    return FileBinding(
        card_id=card.id,
        path=path,
        path_key=path.lower(),
        encoding="utf-8",
        bom=False,
        newline=NewlineKind.LF,
        trailing_newline=True,
        bound_at_us=bound_at_us,
    )


def _binding_count(database: Database) -> int:
    row = database.connection.execute("SELECT COUNT(*) FROM card_file_bindings").fetchone()
    assert row is not None
    return int(row[0])


def test_schema_version_ten_creates_the_binding_table(database: Database) -> None:
    assert LATEST_SCHEMA_VERSION == 10
    assert database.schema_version == 10
    row = database.connection.execute(
        """
        SELECT 1 FROM sqlite_master
        WHERE type = 'table' AND name = 'card_file_bindings'
        """
    ).fetchone()
    assert row is not None


def test_binding_table_foreign_key_uses_delete_restrict(database: Database) -> None:
    foreign_keys = database.connection.execute(
        "PRAGMA foreign_key_list(card_file_bindings)"
    ).fetchall()

    assert foreign_keys != []
    assert all(str(foreign_key[6]).upper() == "RESTRICT" for foreign_key in foreign_keys)


def test_migration_is_idempotent_when_the_schema_version_is_rewound(
    database_path: Path,
) -> None:
    with Database(database_path) as created:
        created.connection.execute(
            "UPDATE schema_version SET version = 9, applied_at_us = 9 WHERE id = 1"
        )

    with Database(database_path) as migrated:
        assert migrated.schema_version == LATEST_SCHEMA_VERSION


def test_upsert_reads_back_every_column_and_replaces_on_conflict(
    database: Database,
    repositories: Repositories,
) -> None:
    document = _document(repositories)
    card = CardService(database, repositories, clock=lambda: 10).create_card(document.id, "본문")
    binding = FileBinding(
        card_id=card.id,
        path="C:\\Notes\\A.txt",
        path_key="c:\\notes\\a.txt",
        encoding="utf-16-be",
        bom=True,
        newline=NewlineKind.CRLF,
        trailing_newline=False,
        bound_at_us=1_000,
        synced_size=12,
        synced_mtime_ns=34,
        synced_hash="abc",
        synced_at_us=2_000,
    )

    repositories.upsert_file_binding(binding)
    assert repositories.get_file_binding(card.id) == binding

    moved = replace(binding, path="C:\\Notes\\B.txt", path_key="c:\\notes\\b.txt")
    repositories.upsert_file_binding(moved)

    assert repositories.get_file_binding(card.id) == moved
    assert repositories.find_binding_by_path("c:\\notes\\a.txt") is None
    assert _binding_count(database) == 1


def test_path_key_is_unique_across_cards(
    database: Database,
    repositories: Repositories,
) -> None:
    document = _document(repositories)
    service = CardService(database, repositories, clock=lambda: 10)
    first = service.create_card(document.id, "첫 카드")
    second = service.create_card(document.id, "둘째 카드")
    repositories.upsert_file_binding(_binding(first, "C:\\Notes\\A.txt"))

    with pytest.raises(sqlite3.IntegrityError):
        repositories.upsert_file_binding(_binding(second, "C:\\Notes\\A.txt"))


def test_trashed_cards_keep_the_binding_but_leave_the_active_lookup(
    database: Database,
    repositories: Repositories,
) -> None:
    document = _document(repositories)
    service = CardService(database, repositories, clock=lambda: 10)
    card = service.create_card(document.id, "휴지통으로 갈 카드")
    repositories.upsert_file_binding(_binding(card, "C:\\Notes\\A.txt"))
    assert repositories.find_active_binding_by_path("c:\\notes\\a.txt") is not None

    service.soft_delete(card.id)

    assert repositories.find_active_binding_by_path("c:\\notes\\a.txt") is None
    assert repositories.find_binding_by_path("c:\\notes\\a.txt") is not None
    assert repositories.get_file_binding(card.id) is not None


def test_delete_file_binding_removes_only_that_row(
    database: Database,
    repositories: Repositories,
) -> None:
    document = _document(repositories)
    service = CardService(database, repositories, clock=lambda: 10)
    first = service.create_card(document.id, "첫 카드")
    second = service.create_card(document.id, "둘째 카드")
    repositories.upsert_file_binding(_binding(first, "C:\\Notes\\A.txt"))
    repositories.upsert_file_binding(_binding(second, "C:\\Notes\\B.txt"))

    repositories.delete_file_binding(first.id)

    assert repositories.get_file_binding(first.id) is None
    assert repositories.get_file_binding(second.id) is not None


def test_rebinding_a_path_held_by_a_trashed_card_succeeds_with_one_active_row(
    database: Database,
    repositories: Repositories,
) -> None:
    document = _document(repositories)
    service = CardService(database, repositories, clock=lambda: 10)
    first = service.create_card(document.id, "먼저 결속한 카드")
    repositories.upsert_file_binding(_binding(first, "C:\\Notes\\A.txt"))
    service.soft_delete(first.id)
    second = service.create_card(document.id, "나중에 결속한 카드")

    resolution = prepare_binding_path(repositories, "c:\\notes\\a.txt")
    repositories.upsert_file_binding(_binding(second, "C:\\Notes\\A.txt", bound_at_us=2_000))

    assert resolution.status is BindingPathStatus.FREE
    assert repositories.get_file_binding(first.id) is None
    assert _binding_count(database) == 1
    active = repositories.find_active_binding_by_path("c:\\notes\\a.txt")
    assert active is not None
    assert active.card_id == second.id


def test_a_path_held_by_an_active_card_reports_the_holder_and_keeps_the_row(
    database: Database,
    repositories: Repositories,
) -> None:
    document = _document(repositories)
    service = CardService(database, repositories, clock=lambda: 10)
    card = service.create_card(document.id, "결속 중인 카드")
    repositories.upsert_file_binding(_binding(card, "C:\\Notes\\A.txt"))

    resolution = prepare_binding_path(repositories, "c:\\notes\\a.txt")

    assert resolution.status is BindingPathStatus.HELD_BY_ACTIVE_CARD
    assert resolution.holder_card_id == card.id
    assert _binding_count(database) == 1


def test_a_free_path_resolves_without_touching_any_row(
    database: Database,
    repositories: Repositories,
) -> None:
    resolution = prepare_binding_path(repositories, "c:\\notes\\없는파일.txt")

    assert resolution.status is BindingPathStatus.FREE
    assert resolution.holder_card_id is None
    assert _binding_count(database) == 0


def test_card_purge_removes_the_binding_row(
    database: Database,
    repositories: Repositories,
) -> None:
    document = _document(repositories)
    service = CardService(database, repositories, clock=lambda: 10)
    card = service.create_card(document.id, "purge 대상")
    repositories.upsert_file_binding(_binding(card, "C:\\Notes\\A.txt"))
    service.soft_delete(card.id)

    PurgeService(database, repositories, clock=lambda: NOW_US).purge_card(
        card.id,
        retention_days=30,
    )

    assert _binding_count(database) == 0
    assert database.connection.execute("PRAGMA foreign_key_check").fetchall() == []


def test_document_purge_removes_every_binding_row_of_that_document(
    database: Database,
    repositories: Repositories,
) -> None:
    document = _document(repositories)
    other = _document(repositories, "other-document")
    service = CardService(database, repositories, clock=lambda: 10)
    first = service.create_card(document.id, "첫 카드")
    second = service.create_card(document.id, "둘째 카드")
    kept = service.create_card(other.id, "남는 카드")
    repositories.upsert_file_binding(_binding(first, "C:\\Notes\\A.txt"))
    repositories.upsert_file_binding(_binding(second, "C:\\Notes\\B.txt"))
    repositories.upsert_file_binding(_binding(kept, "C:\\Notes\\C.txt"))
    repositories.update_document(
        replace(document, trashed_at_us=10),
    )

    PurgeService(database, repositories, clock=lambda: NOW_US).purge_document(
        document.id,
        retention_days=30,
    )

    assert _binding_count(database) == 1
    assert repositories.get_file_binding(kept.id) is not None
    assert database.connection.execute("PRAGMA foreign_key_check").fetchall() == []


def test_binding_rows_block_a_raw_card_delete(
    database: Database,
    repositories: Repositories,
) -> None:
    """FK 가 RESTRICT 이므로 결속 행을 남긴 채로는 카드 행을 지울 수 없다."""
    document = _document(repositories)
    service = CardService(database, repositories, clock=lambda: 10)
    card = service.create_card(document.id, "결속 카드")
    repositories.upsert_file_binding(_binding(card, "C:\\Notes\\A.txt"))
    repositories.update_card_deleted_state(
        card.id,
        position_key=card.position_key,
        deleted_at_us=10,
        expected_revision_id=str(card.current_revision_id),
    )

    with pytest.raises(sqlite3.IntegrityError):
        repositories.delete_card(card.id)


def test_capture_source_enum_is_untouched_by_the_binding_feature() -> None:
    """§4-5 금지: 새 CaptureOperationSource 값을 추가하지 않는다."""
    assert {source.value for source in CaptureOperationSource} == {
        "typing",
        "paste",
        "import",
        "mixed",
        "split",
        "merge",
        "system",
    }
