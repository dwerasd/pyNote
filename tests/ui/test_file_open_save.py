from __future__ import annotations

import codecs
from pathlib import Path
from typing import Any

import pytest
from PySide6.QtCore import QEvent, QSettings, Qt
from PySide6.QtGui import QInputMethodEvent, QKeyEvent, QKeySequence
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QFileDialog
from pytest import MonkeyPatch
from pytestqt.qtbot import QtBot

from pynote.app import SqliteWorkspaceStateStore
from pynote.application import document_service, file_binding_service
from pynote.application.save_coordinator import SaveCoordinator, SaveOutcome
from pynote.domain.models import CaptureOperationSource
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories
from pynote.ui.document_page import DocumentPage
from pynote.ui.editor import card_editor as card_editor_module
from pynote.ui.editor.card_editor import (
    CardEditor,
    CloseChoice,
    EditorStatus,
    ExternalChangeChoice,
)
from pynote.ui.main_window import MainWindow

_PNG_HEADER = b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR"


@pytest.fixture(autouse=True)
def _non_blocking_close_dialog(monkeypatch: MonkeyPatch) -> None:
    """teardown 의 창 닫기가 모달을 띄우면 시험 실행이 그 자리에서 멈춘다."""
    monkeypatch.setattr(
        CardEditor,
        "_ask_close_choice",
        lambda _self: CloseChoice.DISCARD,
    )


def _window(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    *,
    title: str = "결속 문서",
    window_id: str = "binding-window",
    settings_name: str = "settings.ini",
) -> tuple[MainWindow, DocumentPage]:
    settings = QSettings(
        str(tmp_path / settings_name),
        QSettings.Format.IniFormat,
    )
    settings.setValue("first_run/guide_shown", True)
    window = MainWindow(
        repositories,
        SqliteWorkspaceStateStore(database),
        settings=settings,
        window_id=window_id,
    )
    qtbot.addWidget(window)
    window.show()
    document = document_service.create_document(repositories, title)
    assert window.open_document_local(document.id)
    page = window.active_document_page()
    assert page is not None
    return window, page


def _write_file(
    path: Path,
    text: str,
    *,
    encoding: str = "utf-8",
    bom: bytes = b"",
    newline: str = "\n",
) -> bytes:
    data = bom + text.replace("\n", newline).encode(encoding)
    path.write_bytes(data)
    return data


def _touch(path: Path, text: str) -> Path:
    _write_file(path, text)
    return path


def _save_spy(monkeypatch: MonkeyPatch) -> list[str | None]:
    """SaveCoordinator.save 를 지나간 카드 ID 를 순서대로 기록한다."""
    saves: list[str | None] = []
    real_save = SaveCoordinator.save

    def spy(self: SaveCoordinator, session: Any) -> Any:
        saves.append(session.card_id)
        return real_save(self, session)

    monkeypatch.setattr(SaveCoordinator, "save", spy)
    return saves


def _commit_text(editor: CardEditor, text: str) -> None:
    event = QInputMethodEvent()
    event.setCommitString(text)
    QApplication.sendEvent(editor, event)


def _press_ctrl_s(editor: CardEditor) -> None:
    QTest.keyClick(
        editor,
        Qt.Key.Key_S,
        Qt.KeyboardModifier.ControlModifier,
    )


def _sync_spy(monkeypatch: MonkeyPatch) -> list[dict[str, Any]]:
    """편집기가 부르는 sync_file 호출을 기록하고 원본 동작은 유지한다."""
    calls: list[dict[str, Any]] = []
    real = card_editor_module.sync_file

    def spy(
        repositories: Repositories,
        card: Any,
        **kwargs: Any,
    ) -> Any:
        calls.append(dict(kwargs))
        return real(repositories, card, **kwargs)

    monkeypatch.setattr(card_editor_module, "sync_file", spy)
    return calls


def _answer_external_change(
    monkeypatch: MonkeyPatch,
    choice: ExternalChangeChoice,
) -> list[str]:
    asked: list[str] = []

    def ask(_self: CardEditor, path: str) -> ExternalChangeChoice:
        asked.append(path)
        return choice

    monkeypatch.setattr(CardEditor, "_ask_external_change_choice", ask)
    return asked


def _forbid_external_change_prompt(monkeypatch: MonkeyPatch) -> None:
    def ask(_self: CardEditor, path: str) -> ExternalChangeChoice:
        raise AssertionError(f"비대화형 경로에서 모달을 띄웠습니다: {path}")

    monkeypatch.setattr(CardEditor, "_ask_external_change_choice", ask)


def _revision_count(repositories: Repositories, card_id: str) -> int:
    return len(repositories.list_revisions(card_id))


def _discard_draft(page: DocumentPage) -> None:
    """dirty 초안을 남긴 채 시험을 끝내면 창 닫기 자동 저장이 모달을 띄운다."""
    assert page.editor.can_leave_editor(
        choice_provider=lambda _session: CloseChoice.DISCARD
    )


def test_opened_file_binds_one_card_and_unedited_save_keeps_the_bytes(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    window, page = _window(qtbot, database, repositories, tmp_path)
    path = tmp_path / "메모.txt"
    original = _write_file(path, "첫 줄\n둘째 줄\n", newline="\r\n")
    calls = _sync_spy(monkeypatch)

    assert page.open_file(path)

    card_id = page.editor.card_id
    assert card_id is not None
    binding = repositories.get_file_binding(card_id)
    assert binding is not None
    assert binding.path == str(path.resolve())
    assert binding.encoding == "utf-8"
    assert binding.newline.characters == "\r\n"
    assert page.editor.toPlainText() == "첫 줄\n둘째 줄\n"
    assert window.windowTitle().startswith("메모.txt — ")

    revisions = _revision_count(repositories, card_id)
    _press_ctrl_s(page.editor)

    assert path.read_bytes() == original
    assert _revision_count(repositories, card_id) == revisions
    assert calls == [{"interactive": True}]
    assert page.editor.status is EditorStatus.SAVED
    assert page.editor.status_text.endswith("· 파일 메모.txt")


def test_edited_save_rewrites_the_file_in_its_original_encoding_and_newline(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    _window_unused, page = _window(qtbot, database, repositories, tmp_path)
    path = tmp_path / "utf16.txt"
    _write_file(
        path,
        "처음\n다음\n",
        encoding="utf-16-le",
        bom=codecs.BOM_UTF16_LE,
        newline="\r\n",
    )
    assert page.open_file(path)
    card_id = page.editor.card_id
    assert card_id is not None
    revisions = _revision_count(repositories, card_id)

    page.editor.setPlainText("처음\n다음\n마지막\n")
    _press_ctrl_s(page.editor)

    assert _revision_count(repositories, card_id) == revisions + 1
    expected = codecs.BOM_UTF16_LE + "처음\r\n다음\r\n마지막\r\n".encode("utf-16-le")
    assert path.read_bytes() == expected
    card = repositories.get_card(card_id)
    assert card is not None
    assert card.body == "처음\n다음\n마지막\n"


def test_database_save_failure_leaves_the_bound_file_untouched(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    _window_unused, page = _window(qtbot, database, repositories, tmp_path)
    path = tmp_path / "실패.txt"
    original = _write_file(path, "원본\n")
    assert page.open_file(path)
    calls = _sync_spy(monkeypatch)

    def failing_save(_self: SaveCoordinator, _session: object) -> None:
        raise RuntimeError("DB 저장 실패")

    monkeypatch.setattr(SaveCoordinator, "save", failing_save)
    page.editor.setPlainText("바뀐 본문\n")

    assert not page.editor.save_current(interactive=True)
    assert path.read_bytes() == original
    assert calls == []
    assert page.editor.status is EditorStatus.SAVE_FAILED
    _discard_draft(page)


def test_file_write_failure_keeps_the_revision_and_retries_on_the_next_save(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    _window_unused, page = _window(qtbot, database, repositories, tmp_path)
    path = tmp_path / "쓰기실패.txt"
    original = _write_file(path, "원본\n")
    assert page.open_file(path)
    card_id = page.editor.card_id
    assert card_id is not None
    calls = _sync_spy(monkeypatch)

    real_atomic_write = file_binding_service._atomic_write
    failing = {"active": True}

    def failing_write(path: Path, data: bytes) -> None:
        if failing["active"]:
            raise OSError(5, "액세스가 거부되었습니다")
        real_atomic_write(path, data)

    monkeypatch.setattr(file_binding_service, "_atomic_write", failing_write)
    page.editor.setPlainText("새 본문\n")
    revisions_before = _revision_count(repositories, card_id)
    _press_ctrl_s(page.editor)

    assert _revision_count(repositories, card_id) == revisions_before + 1
    assert path.read_bytes() == original
    assert page.editor.status is EditorStatus.FILE_WRITE_FAILED
    assert page.editor.status_text.startswith("카드 저장됨 · 파일 쓰기 실패")
    assert len(calls) == 1

    # 본문을 고치지 않아도(UNCHANGED 경로) 다음 저장이 파일 되쓰기를 재시도한다.
    failing["active"] = False
    _press_ctrl_s(page.editor)

    assert len(calls) == 2
    assert path.read_bytes() == "새 본문\n".encode()
    assert page.editor.status is EditorStatus.SAVED


def test_external_change_overwrite_rewrites_the_file_after_the_prompt(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    _window_unused, page = _window(qtbot, database, repositories, tmp_path)
    path = tmp_path / "외부변경.txt"
    _write_file(path, "원본\n")
    assert page.open_file(path)
    asked = _answer_external_change(monkeypatch, ExternalChangeChoice.OVERWRITE)

    path.write_bytes("밖에서 바꾼 내용\n".encode())
    page.editor.setPlainText("편집기 본문\n")
    _press_ctrl_s(page.editor)

    assert asked == [str(path.resolve())]
    assert path.read_bytes() == "편집기 본문\n".encode()


def test_external_change_cancel_keeps_the_file_and_asks_again(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    _window_unused, page = _window(qtbot, database, repositories, tmp_path)
    path = tmp_path / "취소.txt"
    assert page.open_file(_touch(path, "원본\n"))
    card_id = page.editor.card_id
    assert card_id is not None
    asked = _answer_external_change(monkeypatch, ExternalChangeChoice.CANCEL)

    external = "밖에서 바꾼 내용\n".encode()
    path.write_bytes(external)
    page.editor.setPlainText("편집기 본문\n")
    revisions_before = _revision_count(repositories, card_id)
    _press_ctrl_s(page.editor)

    # DB 저장은 이미 끝났고 파일만 건드리지 않는다.
    assert _revision_count(repositories, card_id) == revisions_before + 1
    assert path.read_bytes() == external
    assert "외부에서 변경" in page.editor.status_text

    _press_ctrl_s(page.editor)

    assert len(asked) == 2
    assert path.read_bytes() == external


def test_external_change_reload_replaces_the_session_text_without_writing(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    _window_unused, page = _window(qtbot, database, repositories, tmp_path)
    path = tmp_path / "다시읽기.txt"
    assert page.open_file(_touch(path, "원본\n"))
    _answer_external_change(monkeypatch, ExternalChangeChoice.RELOAD)

    external = "밖에서 바꾼 내용\n".encode()
    path.write_bytes(external)
    page.editor.setPlainText("편집기 본문\n")
    _press_ctrl_s(page.editor)

    assert path.read_bytes() == external
    assert page.editor.toPlainText() == "밖에서 바꾼 내용\n"
    session = page.editor.session
    assert session is not None
    assert session.dirty
    _discard_draft(page)


def test_leaving_the_editor_never_prompts_on_external_change(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    _window_unused, page = _window(qtbot, database, repositories, tmp_path)
    path = tmp_path / "이탈.txt"
    assert page.open_file(_touch(path, "원본\n"))
    card_id = page.editor.card_id
    assert card_id is not None
    _forbid_external_change_prompt(monkeypatch)

    external = "밖에서 바꾼 내용\n".encode()
    path.write_bytes(external)
    page.editor.setPlainText("편집기 본문\n")
    revisions_before = _revision_count(repositories, card_id)

    assert page.can_leave_editor(protect_now=True)
    assert path.read_bytes() == external
    assert _revision_count(repositories, card_id) == revisions_before + 1
    assert "외부에서 변경" in page.editor.status_text


@pytest.mark.parametrize(
    ("choice", "expected_body"),
    [
        (ExternalChangeChoice.RELOAD, "밖에서 바꾼 내용\n"),
        (ExternalChangeChoice.CANCEL, "원본\n"),
    ],
)
def test_reopening_a_card_offers_to_reload_the_changed_file(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
    choice: ExternalChangeChoice,
    expected_body: str,
) -> None:
    _window_unused, page = _window(qtbot, database, repositories, tmp_path)
    path = tmp_path / "열기변경.txt"
    assert page.open_file(_touch(path, "원본\n"))
    card_id = page.editor.card_id
    assert card_id is not None
    assert page.editor.request_close()

    external = "밖에서 바꾼 내용\n".encode()
    path.write_bytes(external)
    asked: list[str] = []

    def ask(_self: CardEditor, asked_path: str) -> ExternalChangeChoice:
        asked.append(asked_path)
        return choice

    monkeypatch.setattr(CardEditor, "_ask_open_external_change_choice", ask)

    assert page.open_card(card_id)

    assert asked == [str(path.resolve())]
    assert page.editor.toPlainText() == expected_body
    # 어느 선택도 파일을 건드리지 않는다 — 되쓰기는 다음 저장이 한다.
    assert path.read_bytes() == external
    _discard_draft(page)


def test_save_as_writes_the_new_path_and_leaves_the_previous_file(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    window, page = _window(qtbot, database, repositories, tmp_path)
    source = tmp_path / "원본.txt"
    original = _write_file(source, "본문\n")
    assert page.open_file(source)
    card_id = page.editor.card_id
    assert card_id is not None

    target = tmp_path / "확장자없음"
    monkeypatch.setattr(
        QFileDialog,
        "getSaveFileName",
        lambda *args, **kwargs: (str(target), "모든 파일 (*)"),
    )
    page.editor.setPlainText("바뀐 본문\n")

    assert page.save_card_as()

    # 확장자를 강제하지 않고 즉시 기록한다.
    assert target.read_bytes() == "바뀐 본문\n".encode()
    assert source.read_bytes() == original
    binding = repositories.get_file_binding(card_id)
    assert binding is not None
    assert binding.path == str(target.resolve())
    assert window.windowTitle().startswith("확장자없음 — ")


def test_save_as_rejects_a_path_already_bound_to_another_card(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    _window_unused, page = _window(qtbot, database, repositories, tmp_path)
    taken = tmp_path / "선점.txt"
    taken_bytes = _write_file(taken, "선점 본문\n")
    assert page.open_file(taken)

    other = tmp_path / "다른.txt"
    assert page.open_file(_touch(other, "다른 본문\n"))
    errors: list[tuple[str, str]] = []
    monkeypatch.setattr(
        page,
        "_error_reporter",
        lambda title, message: errors.append((title, message)),
    )
    monkeypatch.setattr(
        QFileDialog,
        "getSaveFileName",
        lambda *args, **kwargs: (str(taken), "모든 파일 (*)"),
    )

    assert not page.save_card_as()
    assert errors and "이미 다른 카드에 결속된 파일" in errors[0][1]
    assert taken.read_bytes() == taken_bytes


def test_reopening_the_same_path_reuses_the_bound_card(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    window, page = _window(qtbot, database, repositories, tmp_path)
    path = tmp_path / "재열기.txt"
    assert window.open_file_path(_touch(path, "본문\n"))
    first_card_id = page.editor.card_id
    assert first_card_id is not None

    assert window.open_file_path(path)

    active = [
        card
        for card in repositories.list_cards(page.document_id)
        if card.deleted_at_us is None
    ]
    assert len(active) == 1
    assert page.editor.card_id == first_card_id


def test_reopening_from_another_document_routes_back_to_the_owner(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    window, page = _window(qtbot, database, repositories, tmp_path)
    owner_document_id = page.document_id
    path = tmp_path / "교차.txt"
    assert window.open_file_path(_touch(path, "본문\n"))
    card_id = page.editor.card_id
    assert card_id is not None

    other = document_service.create_document(repositories, "다른 문서")
    assert window.open_document_local(other.id)
    assert window.active_document_id == other.id

    assert window.open_file_path(path)

    assert window.active_document_id == owner_document_id
    owner_page = window.active_document_page()
    assert owner_page is not None
    assert owner_page.editor.card_id == card_id


def test_empty_file_waits_for_the_first_card_and_writes_on_the_first_save(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    _window_unused, page = _window(qtbot, database, repositories, tmp_path)
    path = tmp_path / "빈파일.txt"
    path.write_bytes(b"")

    assert page.open_file(path)

    assert page.pending_binding is not None
    assert page.editor.card_id is None
    assert path.read_bytes() == b""

    page.editor.setFocus()
    _commit_text(page.editor, "첫 카드")
    card_id = page.editor.card_id
    assert card_id is not None
    assert page.pending_binding is None
    binding = repositories.get_file_binding(card_id)
    assert binding is not None
    assert binding.synced_hash is None
    assert path.read_bytes() == b""

    _press_ctrl_s(page.editor)

    assert path.read_bytes() == "첫 카드".encode()


def test_whitespace_only_file_shows_the_replacement_notice(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    _window_unused, page = _window(qtbot, database, repositories, tmp_path)
    path = tmp_path / "공백.txt"
    _write_file(path, "   \n\n")

    assert page.open_file(path)

    assert page.pending_binding is not None
    assert page.editor_workspace.status_label.text() == (
        "원본 공백 내용은 첫 저장 때 대체됩니다"
    )


def test_pending_binding_is_promoted_only_once(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    _window_unused, page = _window(qtbot, database, repositories, tmp_path)
    path = tmp_path / "한번만.txt"
    path.write_bytes(b"")
    assert page.open_file(path)

    page.editor.setFocus()
    _commit_text(page.editor, "첫 카드")
    first_card_id = page.editor.card_id
    assert first_card_id is not None
    _press_ctrl_s(page.editor)

    assert page.editor.request_close()
    page.editor.setFocus()
    _commit_text(page.editor, "둘째 카드")
    second_card_id = page.editor.card_id

    assert second_card_id is not None
    assert second_card_id != first_card_id
    assert repositories.get_file_binding(second_card_id) is None
    _resolved, path_key = file_binding_service.resolve_path(path)
    binding = repositories.find_binding_by_path(path_key)
    assert binding is not None
    assert binding.card_id == first_card_id


def test_pending_binding_attaches_to_the_first_created_card_only(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    _window_unused, page = _window(qtbot, database, repositories, tmp_path)
    path = tmp_path / "승격.txt"
    path.write_bytes(b"")
    assert page.open_file(path)

    # 카드 연결(card_connected)을 거치지 않고 생성 신호만 두 번 보내, 승격 훅 자체가
    # 대기를 한 번만 소비하는지 본다.
    first = page.card_service.create_cards(
        page.document_id,
        "첫 카드",
        source=CaptureOperationSource.IMPORT,
        split=False,
    )[0]
    page.editor.card_created.emit(first)
    second = page.card_service.create_cards(
        page.document_id,
        "둘째 카드",
        source=CaptureOperationSource.IMPORT,
        split=False,
    )[0]
    page.editor.card_created.emit(second)

    assert page.pending_binding is None
    assert repositories.get_file_binding(first.id) is not None
    assert repositories.get_file_binding(second.id) is None


def test_pending_binding_is_discarded_when_leaving_the_page(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    _window_unused, page = _window(qtbot, database, repositories, tmp_path)
    path = tmp_path / "폐기.txt"
    path.write_bytes(b"")
    assert page.open_file(path)
    assert page.pending_binding is not None

    assert page.can_leave_editor(protect_now=True)

    assert page.pending_binding is None
    assert path.read_bytes() == b""


def test_unbound_card_save_does_not_touch_the_file(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    window, page = _window(qtbot, database, repositories, tmp_path)
    path = tmp_path / "해제.txt"
    original = _write_file(path, "본문\n")
    assert page.open_file(path)
    card_id = page.editor.card_id
    assert card_id is not None

    assert page.unbind_file()

    assert repositories.get_file_binding(card_id) is None
    assert not window.windowTitle().startswith("해제.txt")
    page.editor.setPlainText("바뀐 본문\n")
    _press_ctrl_s(page.editor)

    assert path.read_bytes() == original
    card = repositories.get_card(card_id)
    assert card is not None
    assert card.body == "바뀐 본문\n"


def test_rejected_file_is_not_bound_and_keeps_its_bytes(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    _window_unused, page = _window(qtbot, database, repositories, tmp_path)
    path = tmp_path / "이진.png"
    path.write_bytes(_PNG_HEADER)
    monkeypatch.setattr(DocumentPage, "_ask_copy_import", lambda _self, _path: False)

    assert not page.open_file(path)

    assert path.read_bytes() == _PNG_HEADER
    assert not repositories.list_cards(page.document_id)


def test_rejected_file_can_be_imported_as_a_copy_without_binding(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    _window_unused, page = _window(qtbot, database, repositories, tmp_path)
    path = tmp_path / "제어문자.txt"
    data = "본문\x00 뒤\n".encode()
    path.write_bytes(data)
    monkeypatch.setattr(DocumentPage, "_ask_copy_import", lambda _self, _path: True)

    assert page.open_file(path)

    card_id = page.editor.card_id
    assert card_id is not None
    assert repositories.get_file_binding(card_id) is None
    assert path.read_bytes() == data


def test_editor_accepts_the_ctrl_s_shortcut_override_only_with_a_session(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    _window_unused, page = _window(qtbot, database, repositories, tmp_path)

    empty_surface_event = QKeyEvent(
        QEvent.Type.ShortcutOverride,
        Qt.Key.Key_S,
        Qt.KeyboardModifier.ControlModifier,
    )
    QApplication.sendEvent(page.editor, empty_surface_event)
    assert not empty_surface_event.isAccepted()

    path = tmp_path / "override.txt"
    assert page.open_file(_touch(path, "본문\n"))
    session_event = QKeyEvent(
        QEvent.Type.ShortcutOverride,
        Qt.Key.Key_S,
        Qt.KeyboardModifier.ControlModifier,
    )
    QApplication.sendEvent(page.editor, session_event)

    assert session_event.isAccepted()


def test_window_shortcut_saves_the_active_card_from_the_card_list(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    window, page = _window(qtbot, database, repositories, tmp_path)
    path = tmp_path / "단축키.txt"
    assert page.open_file(_touch(path, "본문\n"))
    window.raise_()
    window.activateWindow()
    qtbot.waitUntil(window.isActiveWindow, timeout=3_000)
    page.stream.setFocus()
    saves = _save_spy(monkeypatch)

    handle = window.windowHandle()
    assert handle is not None
    QTest.keyClick(handle, Qt.Key.Key_S, Qt.KeyboardModifier.ControlModifier)

    assert saves == [page.editor.card_id]


def test_only_the_active_window_saves_on_the_window_shortcut(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    first_window, first_page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
        title="첫 문서",
        window_id="window-a",
        settings_name="a.ini",
    )
    second_window, second_page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
        title="둘째 문서",
        window_id="window-b",
        settings_name="b.ini",
    )
    assert first_page.open_file(_touch(tmp_path / "첫.txt", "첫 본문\n"))
    assert second_page.open_file(_touch(tmp_path / "둘째.txt", "둘째 본문\n"))
    second_window.raise_()
    second_window.activateWindow()
    qtbot.waitUntil(second_window.isActiveWindow, timeout=3_000)
    second_page.stream.setFocus()
    saves = _save_spy(monkeypatch)

    handle = second_window.windowHandle()
    assert handle is not None
    QTest.keyClick(handle, Qt.Key.Key_S, Qt.KeyboardModifier.ControlModifier)

    assert saves == [second_page.editor.card_id]
    assert first_page.editor.card_id not in saves
    assert not first_window.isActiveWindow()


def test_file_actions_own_ctrl_o_and_use_window_shortcut_context(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    window, _page = _window(qtbot, database, repositories, tmp_path)
    actions = {action.objectName(): action for action in window.actions()}

    assert actions["openFileAction"].shortcut() == QKeySequence("Ctrl+O")
    assert actions["documentListAction"].shortcut() == QKeySequence("Ctrl+Shift+O")
    assert actions["saveCardAction"].shortcut() == QKeySequence("Ctrl+S")
    assert actions["saveCardAsAction"].shortcut() == QKeySequence("Ctrl+Shift+S")
    for object_name in ("openFileAction", "saveCardAction", "saveCardAsAction"):
        assert (
            actions[object_name].shortcutContext()
            is Qt.ShortcutContext.WindowShortcut
        )
    assert actions["documentListAction"].shortcutContext() is (
        Qt.ShortcutContext.ApplicationShortcut
    )
    file_menu_actions = {
        action.objectName() for action in window.file_menu.actions()
    }
    assert {
        "openFileAction",
        "saveCardAction",
        "saveCardAsAction",
        "unbindFileAction",
    } <= file_menu_actions


def test_open_action_routes_the_chosen_path_through_the_binding_lookup(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    window, page = _window(qtbot, database, repositories, tmp_path)
    path = tmp_path / "액션.txt"
    _touch(path, "본문\n")
    monkeypatch.setattr(
        QFileDialog,
        "getOpenFileName",
        lambda *args, **kwargs: (str(path), "모든 파일 (*)"),
    )

    window.open_file_action.trigger()

    card_id = page.editor.card_id
    assert card_id is not None
    binding = repositories.get_file_binding(card_id)
    assert binding is not None
    assert binding.path == str(path.resolve())


def test_title_drops_the_file_name_when_the_editor_closes(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    window, page = _window(qtbot, database, repositories, tmp_path)
    path = tmp_path / "제목.txt"
    assert page.open_file(_touch(path, "본문\n"))
    assert window.windowTitle().startswith("제목.txt — ")

    assert page.editor.request_close()

    assert window.windowTitle() == "결속 문서 — pyNote"


@pytest.mark.parametrize(
    "outcome",
    [SaveOutcome.SAVED, SaveOutcome.UNCHANGED],
)
def test_both_database_outcomes_reach_the_file_sync_hook(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
    outcome: SaveOutcome,
) -> None:
    _window_unused, page = _window(qtbot, database, repositories, tmp_path)
    path = tmp_path / f"{outcome.value}.txt"
    assert page.open_file(_touch(path, "본문\n"))
    calls = _sync_spy(monkeypatch)
    if outcome is SaveOutcome.SAVED:
        page.editor.setPlainText("바뀐 본문\n")

    _press_ctrl_s(page.editor)

    assert len(calls) == 1


def test_save_as_restores_the_previous_binding_when_the_commit_fails(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    """확정 실패(IME 조합·충돌·예외)면 새 파일을 만들지 않고 결속을 이전 상태로 되돌린다."""
    _window_unused, page = _window(qtbot, database, repositories, tmp_path)
    source = tmp_path / "원본.txt"
    original = _write_file(source, "본문\n")
    assert page.open_file(source)
    card_id = page.editor.card_id
    assert card_id is not None
    before = repositories.get_file_binding(card_id)
    assert before is not None

    target = tmp_path / "새이름.txt"
    monkeypatch.setattr(
        QFileDialog,
        "getSaveFileName",
        lambda *args, **kwargs: (str(target), "모든 파일 (*)"),
    )
    errors: list[tuple[str, str]] = []
    monkeypatch.setattr(
        page,
        "_error_reporter",
        lambda title, message: errors.append((title, message)),
    )
    monkeypatch.setattr(page.editor, "save_current", lambda **kwargs: False)
    page.editor.setPlainText("바뀐 본문\n")

    assert not page.save_card_as()

    assert not target.exists()
    assert source.read_bytes() == original
    after = repositories.get_file_binding(card_id)
    assert after is not None and after.path == before.path
    assert errors and "확정하지 못해" in errors[0][1]
    _discard_draft(page)


def test_opening_a_bound_path_routes_through_the_search_result_router(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    """S6 교차 창 갈래 — 라우터가 있으면 (창, 문서, 카드) 로 정확히 1회 위임한다."""
    routed: list[tuple[MainWindow, str, str | None]] = []

    def router(window: MainWindow, document_id: str, card_id: str | None) -> bool:
        routed.append((window, document_id, card_id))
        return True

    settings = QSettings(str(tmp_path / "router.ini"), QSettings.Format.IniFormat)
    settings.setValue("first_run/guide_shown", True)
    window = MainWindow(
        repositories,
        SqliteWorkspaceStateStore(database),
        settings=settings,
        window_id="router-window",
        search_result_router=router,
    )
    qtbot.addWidget(window)
    window.show()
    document = document_service.create_document(repositories, "라우터 문서")
    assert window.open_document_local(document.id)
    page = window.active_document_page()
    assert page is not None
    path = _touch(tmp_path / "라우팅.txt", "본문\n")
    assert page.open_file(path)
    card_id = page.editor.card_id
    assert card_id is not None

    assert window.open_file_path(path)

    assert routed == [(window, document.id, card_id)]
