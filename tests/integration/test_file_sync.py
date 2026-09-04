from __future__ import annotations

import logging
import os
import stat
import sys
from dataclasses import dataclass, replace
from pathlib import Path

import pytest

from pynote.application.file_binding_service import (
    FileSyncOutcome,
    detect_text,
    hash_bytes,
    render_bytes,
    resolve_path,
    sync_file,
)
from pynote.domain.events import EventSource
from pynote.domain.models import (
    CaptureOperationSource,
    Card,
    CardSource,
    Document,
    FileBinding,
    NewCaptureOperation,
    NewCard,
    NewlineKind,
    RevisionSource,
    SplitPolicy,
)
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories
from tests.unit.test_file_binding_service import (
    _ANSI_ENCODING,
    golden_cases,
    source_bytes,
)


@dataclass(frozen=True, slots=True)
class BoundCard:
    """결속을 마친 카드와 그 파일이다."""

    card: Card
    binding: FileBinding
    path: Path


def _create_card(repositories: Repositories, body: str) -> Card:
    """가져오기와 같은 경로로 카드 한 장을 만든다(본문은 정규화된 그대로다)."""
    document_id = "document-1"
    repositories.create_document(
        Document(
            id=document_id,
            title="결속 문서",
            created_at_us=1,
            updated_at_us=1,
        )
    )
    created = repositories.create_cards(
        NewCaptureOperation(
            id="operation-1",
            document_id=document_id,
            source=CaptureOperationSource.IMPORT,
            split_policy=SplitPolicy.KEEP,
            original_text=None,
            created_at_us=10,
        ),
        [
            NewCard(
                id="card-1",
                revision_id="revision-1",
                event_id="event-1",
                position_key=1_024,
                body=body,
                card_source=CardSource.IMPORT,
                event_source=EventSource.IMPORT,
                revision_source=RevisionSource.EDIT,
                created_at_us=10,
            )
        ],
    )
    return created[0]


def _bind(
    repositories: Repositories,
    tmp_path: Path,
    data: bytes,
    *,
    name: str = "note.txt",
    synced: bool = True,
) -> BoundCard:
    """파일을 만들고 열기 직후와 같은 결속 상태를 세운다."""
    path = tmp_path / name
    path.write_bytes(data)
    detected = detect_text(path.read_bytes())
    assert detected is not None
    card = _create_card(repositories, detected.text)
    path_string, path_key = resolve_path(path)
    stat_result = path.stat()
    binding = FileBinding(
        card_id=card.id,
        path=path_string,
        path_key=path_key,
        encoding=detected.encoding,
        bom=detected.bom,
        newline=detected.newline,
        trailing_newline=detected.trailing_newline,
        bound_at_us=1_000,
        synced_size=stat_result.st_size if synced else None,
        synced_mtime_ns=stat_result.st_mtime_ns if synced else None,
        synced_hash=hash_bytes(data) if synced else None,
        synced_at_us=1_000 if synced else None,
    )
    repositories.upsert_file_binding(binding)
    return BoundCard(card=card, binding=binding, path=path)


def _temp_files(directory: Path) -> list[Path]:
    return [entry for entry in directory.iterdir() if entry.name.endswith(".tmp")]


@pytest.mark.parametrize(("body", "encoding", "bom", "newline", "trailing"), golden_cases())
def test_unedited_save_leaves_the_file_byte_identical(
    repositories: Repositories,
    tmp_path: Path,
    body: str,
    encoding: str,
    bom: bool,
    newline: NewlineKind,
    trailing: bool,
) -> None:
    data = source_bytes(
        body,
        encoding=encoding,
        bom=bom,
        newline=newline,
        trailing_newline=trailing,
    )
    bound = _bind(repositories, tmp_path, data)

    result = sync_file(repositories, bound.card, clock=lambda: 2_000)

    assert result.outcome is FileSyncOutcome.NOOP
    assert result.error is None
    assert bound.path.read_bytes() == data


@pytest.mark.parametrize(("body", "encoding", "bom", "newline", "trailing"), golden_cases())
def test_edited_save_rewrites_the_file_in_the_original_format(
    repositories: Repositories,
    tmp_path: Path,
    body: str,
    encoding: str,
    bom: bool,
    newline: NewlineKind,
    trailing: bool,
) -> None:
    data = source_bytes(
        body,
        encoding=encoding,
        bom=bom,
        newline=newline,
        trailing_newline=trailing,
    )
    bound = _bind(repositories, tmp_path, data)
    edited = replace(bound.card, body=bound.card.body + "\n덧붙인 줄")

    result = sync_file(repositories, edited, clock=lambda: 2_000)

    assert result.outcome is FileSyncOutcome.WRITTEN
    expected = source_bytes(
        body + ("\n" if trailing else "") + "\n덧붙인 줄",
        encoding=encoding,
        bom=bom,
        newline=newline,
        trailing_newline=False,
    )
    assert bound.path.read_bytes() == expected
    stored = repositories.get_file_binding(bound.card.id)
    assert stored is not None
    assert stored.synced_hash == hash_bytes(expected)
    assert stored.synced_size == len(expected)
    assert stored.synced_at_us == 2_000


def test_missing_file_is_recreated(
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    bound = _bind(repositories, tmp_path, "본문\n".encode())
    bound.path.unlink()

    result = sync_file(repositories, bound.card, clock=lambda: 2_000)

    assert result.outcome is FileSyncOutcome.WRITTEN
    assert bound.path.read_bytes() == "본문\n".encode()


def test_a_binding_that_never_synced_writes_without_asking(
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    bound = _bind(repositories, tmp_path, "본문\n".encode(), synced=False)
    assert bound.binding.synced_hash is None
    edited = replace(bound.card, body="다른 본문\n")

    result = sync_file(repositories, edited, clock=lambda: 2_000)

    assert result.outcome is FileSyncOutcome.WRITTEN
    assert bound.path.read_bytes() == "다른 본문\n".encode()


def test_external_change_is_not_overwritten_without_a_decision(
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    bound = _bind(repositories, tmp_path, "본문\n".encode())
    bound.path.write_bytes("바깥에서 바꾼 본문\n".encode())
    edited = replace(bound.card, body="편집한 본문\n")

    result = sync_file(repositories, edited, interactive=False, clock=lambda: 2_000)

    assert result.outcome is FileSyncOutcome.EXTERNAL_CHANGE
    assert bound.path.read_bytes() == "바깥에서 바꾼 본문\n".encode()
    stored = repositories.get_file_binding(bound.card.id)
    assert stored is not None
    assert stored.synced_hash == bound.binding.synced_hash


def test_an_interactive_save_also_refuses_before_the_user_answers(
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    bound = _bind(repositories, tmp_path, "본문\n".encode())
    bound.path.write_bytes("바깥에서 바꾼 본문\n".encode())
    edited = replace(bound.card, body="편집한 본문\n")

    result = sync_file(repositories, edited, interactive=True, clock=lambda: 2_000)

    assert result.outcome is FileSyncOutcome.EXTERNAL_CHANGE
    assert bound.path.read_bytes() == "바깥에서 바꾼 본문\n".encode()


def test_cancelling_leaves_the_next_save_asking_again(
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    bound = _bind(repositories, tmp_path, "본문\n".encode())
    bound.path.write_bytes("바깥에서 바꾼 본문\n".encode())
    edited = replace(bound.card, body="편집한 본문\n")

    first = sync_file(repositories, edited, interactive=True, clock=lambda: 2_000)
    second = sync_file(repositories, edited, interactive=True, clock=lambda: 3_000)

    assert first.outcome is FileSyncOutcome.EXTERNAL_CHANGE
    assert second.outcome is FileSyncOutcome.EXTERNAL_CHANGE
    stored = repositories.get_file_binding(bound.card.id)
    assert stored is not None
    assert stored.synced_hash == bound.binding.synced_hash
    assert stored.synced_at_us == bound.binding.synced_at_us


def test_force_overwrites_the_external_change(
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    bound = _bind(repositories, tmp_path, "본문\n".encode())
    bound.path.write_bytes("바깥에서 바꾼 본문\n".encode())
    edited = replace(bound.card, body="편집한 본문\n")

    result = sync_file(repositories, edited, force=True, clock=lambda: 2_000)

    assert result.outcome is FileSyncOutcome.WRITTEN
    assert bound.path.read_bytes() == "편집한 본문\n".encode()
    stored = repositories.get_file_binding(bound.card.id)
    assert stored is not None
    assert stored.synced_hash == hash_bytes("편집한 본문\n".encode())


def test_an_external_change_that_matches_the_card_is_a_noop(
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    bound = _bind(repositories, tmp_path, "본문\n".encode())
    edited = replace(bound.card, body="바깥에서 바꾼 본문\n")
    bound.path.write_bytes("바깥에서 바꾼 본문\n".encode())

    result = sync_file(repositories, edited, clock=lambda: 2_000)

    assert result.outcome is FileSyncOutcome.NOOP
    stored = repositories.get_file_binding(bound.card.id)
    assert stored is not None
    assert stored.synced_hash == hash_bytes("바깥에서 바꾼 본문\n".encode())


def test_a_card_without_a_binding_never_touches_a_file(
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    bound = _bind(repositories, tmp_path, "본문\n".encode())
    repositories.delete_file_binding(bound.card.id)
    edited = replace(bound.card, body="편집한 본문\n")

    result = sync_file(repositories, edited, clock=lambda: 2_000)

    assert result.outcome is FileSyncOutcome.NOOP
    assert bound.path.read_bytes() == "본문\n".encode()


def test_a_failed_replace_reports_failure_and_leaves_no_temporary_file(
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    bound = _bind(repositories, tmp_path, "본문\n".encode())
    edited = replace(bound.card, body="편집한 본문\n")

    def refuse(source: object, destination: object) -> None:
        raise OSError(13, "교체 거부 주입")

    monkeypatch.setattr(os, "replace", refuse)
    result = sync_file(repositories, edited, clock=lambda: 2_000)

    assert result.outcome is FileSyncOutcome.FAILED
    assert result.error is not None
    assert "교체 거부 주입" in result.error
    assert _temp_files(tmp_path) == []
    assert bound.path.read_bytes() == "본문\n".encode()
    stored = repositories.get_file_binding(bound.card.id)
    assert stored is not None
    assert stored.synced_hash == bound.binding.synced_hash


@pytest.mark.skipif(sys.platform != "win32", reason="winerror 는 Windows 전용 실패 신호다")
def test_a_read_only_target_fails_with_the_windows_error_code(
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    bound = _bind(repositories, tmp_path, "본문\n".encode())
    edited = replace(bound.card, body="편집한 본문\n")
    os.chmod(bound.path, stat.S_IREAD)
    try:
        result = sync_file(repositories, edited, clock=lambda: 2_000)
    finally:
        os.chmod(bound.path, stat.S_IWRITE)

    assert result.outcome is FileSyncOutcome.FAILED
    assert result.error is not None
    assert "WinError 5" in result.error
    assert _temp_files(tmp_path) == []
    assert bound.path.read_bytes() == "본문\n".encode()


def test_an_unrepresentable_character_fails_instead_of_being_replaced(
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    original = "본문\n".encode(_ANSI_ENCODING)
    bound = _bind(repositories, tmp_path, original, name="ansi.txt")
    stored_binding = repositories.get_file_binding(bound.card.id)
    assert stored_binding is not None
    repositories.upsert_file_binding(replace(stored_binding, encoding="cp949"))
    edited = replace(bound.card, body="이모지 🙂\n")

    result = sync_file(repositories, edited, clock=lambda: 2_000)

    assert result.outcome is FileSyncOutcome.FAILED
    assert result.error is not None
    assert bound.path.read_bytes() == original
    assert _temp_files(tmp_path) == []


@pytest.mark.parametrize(
    "data",
    [
        b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR",
        ("앞" + chr(0x00A0) + "뒤").encode("utf-8"),
        ("앞" + chr(0x2028) + "뒤").encode("utf-8"),
        bytes(range(0x80, 0x100)),
    ],
)
def test_rejected_files_are_never_read_into_a_binding_and_stay_untouched(
    tmp_path: Path,
    data: bytes,
) -> None:
    path = tmp_path / "reject.bin"
    path.write_bytes(data)

    assert detect_text(path.read_bytes()) is None
    assert path.read_bytes() == data


def test_render_and_sync_agree_on_the_bytes_written(
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    data = source_bytes(
        "첫 줄\n둘째 줄",
        encoding="utf-8",
        bom=True,
        newline=NewlineKind.CRLF,
        trailing_newline=True,
    )
    bound = _bind(repositories, tmp_path, data)
    edited = replace(bound.card, body="바뀐 첫 줄\n바뀐 둘째 줄\n")

    result = sync_file(repositories, edited, clock=lambda: 2_000)

    assert result.outcome is FileSyncOutcome.WRITTEN
    assert bound.path.read_bytes() == render_bytes(edited.body, bound.binding)


def test_database_stays_consistent_after_syncing(
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    bound = _bind(repositories, tmp_path, "본문\n".encode())
    edited = replace(bound.card, body="편집한 본문\n")

    sync_file(repositories, edited, clock=lambda: 2_000)

    assert database.connection.execute("PRAGMA foreign_key_check").fetchall() == []


def test_the_interactive_flag_changes_the_recorded_message(
    repositories: Repositories,
    tmp_path: Path,
    caplog: pytest.LogCaptureFixture,
) -> None:
    """(e) 분기의 대화형/비대화형 차이는 로그 수준·문구뿐이므로 그것을 단언한다(§2-5)."""
    bound = _bind(repositories, tmp_path, "본문\n".encode())
    bound.path.write_bytes("바깥에서 바꾼 본문\n".encode())
    edited = replace(bound.card, body="편집한 본문\n")
    logger_name = "pynote.application.file_binding_service"

    with caplog.at_level(logging.INFO, logger=logger_name):
        sync_file(repositories, edited, interactive=True, clock=lambda: 2_000)
    interactive_records = list(caplog.records)
    caplog.clear()
    with caplog.at_level(logging.INFO, logger=logger_name):
        sync_file(repositories, edited, interactive=False, clock=lambda: 3_000)
    quiet_records = list(caplog.records)

    assert [record.levelno for record in interactive_records] == [logging.INFO]
    assert "확인이 필요합니다" in interactive_records[0].getMessage()
    assert [record.levelno for record in quiet_records] == [logging.WARNING]
    assert "파일이 외부에서 변경되어 되쓰지 않았습니다" in quiet_records[0].getMessage()


def test_stat_failure_after_replace_is_sealed_and_leaves_no_fingerprint(
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """기록 뒤 stat 이 실패해도 예외가 새지 않고 지문만 비운다(다음 저장이 (d) 로 재기록)."""
    bound = _bind(repositories, tmp_path, "본문\n".encode())
    edited = replace(bound.card, body="편집한 본문\n")
    real_stat = Path.stat

    def failing_stat(self: Path, *args: object, **kwargs: object) -> os.stat_result:
        if self == bound.path:
            raise OSError("stat 실패 주입")
        return real_stat(self, *args, **kwargs)  # type: ignore[arg-type]

    monkeypatch.setattr(Path, "stat", failing_stat)
    result = sync_file(repositories, edited, clock=lambda: 2_000)
    monkeypatch.undo()

    assert result.outcome is FileSyncOutcome.WRITTEN
    assert bound.path.read_bytes() == "편집한 본문\n".encode()
    binding = repositories.get_file_binding(bound.card.id)
    assert binding is not None
    assert binding.synced_hash == bound.binding.synced_hash
