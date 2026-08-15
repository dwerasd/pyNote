from __future__ import annotations

import codecs
import locale
import sys
from collections.abc import Callable
from pathlib import Path
from types import TracebackType
from typing import IO, Any

import pytest
from PySide6.QtCore import (
    QItemSelectionModel,
    QMimeData,
    QModelIndex,
    QPointF,
    Qt,
    QTimer,
    QUrl,
)
from PySide6.QtGui import QDragEnterEvent, QDragMoveEvent, QDropEvent
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QAbstractItemView, QApplication, QMessageBox
from pytest import MonkeyPatch
from pytestqt.qtbot import QtBot

from pynote.application import document_service
from pynote.domain.events import EventSource, EventType
from pynote.domain.models import (
    CaptureOperationSource,
    CardSource,
    NewCaptureOperation,
    NewCard,
    RevisionSource,
    SplitPolicy,
)
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories
from pynote.ui.cards.card_model import CardRole
from pynote.ui.document_page import DocumentPage
from pynote.ui.editor.card_editor import CardEditor

_FOUR_MIB = 4 * 1024 * 1024
_CP949_TEXT = "CP949 한글 원문"
_SYSTEM_ANSI_IS_CP949 = (
    sys.platform == "win32"
    and codecs.lookup(locale.getencoding()).name == codecs.lookup("cp949").name
)


def _page(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    *,
    errors: list[tuple[str, str]] | None = None,
) -> DocumentPage:
    document = document_service.create_document(repositories, "파일 드롭")
    page = DocumentPage(
        database,
        repositories,
        document.id,
        error_reporter=None if errors is None else lambda title, message: errors.append(
            (title, message)
        ),
    )
    qtbot.addWidget(page)
    page.resize(900, 600)
    page.show()
    return page


def _file_mime(paths: list[Path], *, companion_text: str | None = None) -> QMimeData:
    mime = QMimeData()
    mime.setUrls([QUrl.fromLocalFile(str(path)) for path in paths])
    if companion_text is not None:
        mime.setText(companion_text)
    return mime


def _url_mime(urls: list[QUrl]) -> QMimeData:
    mime = QMimeData()
    mime.setUrls(urls)
    return mime


def _send_enter(editor: CardEditor, mime: QMimeData) -> QDragEnterEvent:
    event = QDragEnterEvent(
        editor.cursorRect().center(),
        Qt.DropAction.CopyAction,
        mime,
        Qt.MouseButton.LeftButton,
        Qt.KeyboardModifier.NoModifier,
    )
    QApplication.sendEvent(editor.viewport(), event)
    return event


def _send_accepted_drop(
    editor: CardEditor,
    mime: QMimeData,
) -> tuple[QDragEnterEvent, QDragMoveEvent, QDropEvent]:
    enter = _send_enter(editor, mime)
    assert enter.isAccepted()
    move = QDragMoveEvent(
        editor.cursorRect().center(),
        Qt.DropAction.CopyAction,
        mime,
        Qt.MouseButton.LeftButton,
        Qt.KeyboardModifier.NoModifier,
    )
    QApplication.sendEvent(editor.viewport(), move)
    assert move.isAccepted()
    drop = QDropEvent(
        QPointF(editor.cursorRect().center()),
        Qt.DropAction.CopyAction,
        mime,
        Qt.MouseButton.LeftButton,
        Qt.KeyboardModifier.NoModifier,
    )
    QApplication.sendEvent(editor.viewport(), drop)
    assert drop.isAccepted()
    return enter, move, drop


@pytest.mark.parametrize(
    "mime_factory",
    [
        lambda _tmp_path: _url_mime([QUrl("https://example.com/note.md")]),
        lambda tmp_path: _url_mime(
            [
                QUrl.fromLocalFile(str(tmp_path / "local.txt")),
                QUrl("https://example.com/note.md"),
            ]
        ),
    ],
    ids=["nonlocal-url", "mixed-local-and-nonlocal"],
)
def test_rejected_drag_enter_never_delivers_drop(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
    mime_factory: Callable[[Path], QMimeData],
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    dropped: list[object] = []
    page.editor.files_dropped.connect(dropped.append)
    gate_calls: list[bool] = []
    read_calls: list[Path] = []
    original_open = Path.open

    def open_spy(target: Path, *args: Any, **kwargs: Any) -> Any:
        read_calls.append(target)
        return original_open(target, *args, **kwargs)

    monkeypatch.setattr(Path, "open", open_spy)
    monkeypatch.setattr(
        page,
        "can_leave_editor",
        lambda **_kwargs: gate_calls.append(True) or True,
    )

    mime = mime_factory(tmp_path)
    enter = _send_enter(page.editor, mime)
    move = QDragMoveEvent(
        page.editor.cursorRect().center(),
        Qt.DropAction.CopyAction,
        mime,
        Qt.MouseButton.LeftButton,
        Qt.KeyboardModifier.NoModifier,
    )
    QApplication.sendEvent(page.editor.viewport(), move)
    drop = QDropEvent(
        QPointF(page.editor.cursorRect().center()),
        Qt.DropAction.CopyAction,
        mime,
        Qt.MouseButton.LeftButton,
        Qt.KeyboardModifier.NoModifier,
    )
    QApplication.sendEvent(page.editor.viewport(), drop)

    assert not enter.isAccepted()
    assert not move.isAccepted()
    assert not drop.isAccepted()
    assert dropped == []
    assert read_calls == []
    assert gate_calls == []
    assert errors == []
    assert repositories.list_cards(page.document_id) == ()


def test_drop_creates_one_card_per_file_in_url_order(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    paths = [tmp_path / "third.md", tmp_path / "first.txt", tmp_path / "second.md"]
    bodies = ["URL 첫째", "URL 둘째\r\n계속", "URL 셋째"]
    for path, body in zip(paths, bodies, strict=True):
        path.write_bytes(body.encode())
    content_changes: list[bool] = []
    history_refreshes: list[bool] = []
    add_calls: list[tuple[str, ...]] = []
    original_add_cards = page.stream.card_model.add_cards

    def add_cards_spy(cards: Any, **kwargs: Any) -> None:
        add_calls.append(tuple(card.id for card in cards))
        original_add_cards(cards, **kwargs)

    page.content_changed.connect(lambda: content_changes.append(True))
    monkeypatch.setattr(page.stream.card_model, "add_cards", add_cards_spy)
    monkeypatch.setattr(
        page.history,
        "refresh",
        lambda: history_refreshes.append(True),
    )

    _send_accepted_drop(page.editor, _file_mime(paths))

    cards = repositories.list_cards(page.document_id)
    assert [card.body for card in cards] == bodies
    assert page.editor.card_id is None
    assert page.editor.toPlainText() == ""
    assert content_changes == [True]
    assert history_refreshes == [True]
    assert len(add_calls) == 1
    assert add_calls[0] == tuple(card.id for card in cards)
    assert errors == []


def test_drop_accepts_any_extension_in_url_order(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    paths = [tmp_path / "LICENSE", tmp_path / ".gitignore", tmp_path / "manual.pdf"]
    bodies = ["확장자 없음", "닷파일", "임의 확장자"]
    for path, body in zip(paths, bodies, strict=True):
        path.write_text(body, encoding="utf-8")

    _send_accepted_drop(page.editor, _file_mime(paths))

    cards = repositories.list_cards(page.document_id)
    assert len(cards) == len(paths)
    assert [card.body for card in cards] == bodies
    assert errors == []


def test_drop_accepts_binary_bytes_and_preserves_ascii_identifier(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    image = tmp_path / "image.png"
    executable = tmp_path / "program.exe"
    image.write_bytes(b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR")
    executable.write_bytes(b"MZ\x90\x00\x03\x00\x00\x00\x04\x00\xff")

    _send_accepted_drop(page.editor, _file_mime([image, executable]))

    cards = repositories.list_cards(page.document_id)
    assert len(cards) == 2
    assert cards[0].body != ""
    assert cards[1].body.startswith("MZ")
    assert errors == []


def test_drop_decodes_utf16_bom_text(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    path = tmp_path / "utf16.data"
    original = "UTF-16 한글 원문\r\n둘째 줄"
    path.write_bytes(b"\xff\xfe" + original.encode("utf-16-le"))

    _send_accepted_drop(page.editor, _file_mime([path]))

    cards = repositories.list_cards(page.document_id)
    assert len(cards) == 1
    assert cards[0].body == original
    assert errors == []


@pytest.mark.skipif(
    not _SYSTEM_ANSI_IS_CP949,
    reason="Windows 시스템 ANSI 코드페이지가 CP949가 아닙니다.",
)
def test_drop_decodes_cp949_text_when_system_ansi_is_cp949(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    path = tmp_path / "ansi.data"
    original = _CP949_TEXT
    path.write_bytes(original.encode("cp949"))

    _send_accepted_drop(page.editor, _file_mime([path]))

    cards = repositories.list_cards(page.document_id)
    assert len(cards) == 1
    assert cards[0].body == original
    assert errors == []


def test_single_file_drop_connects_and_starts_cursor_at_zero(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    path = tmp_path / "single.md"
    path.write_text("첫 줄\n둘째 줄", encoding="utf-8")
    session_changes: list[bool] = []
    connected: list[str] = []
    modal_calls: list[bool] = []
    page.editor.session_changed.connect(session_changes.append)
    page.editor.card_connected.connect(connected.append)
    monkeypatch.setattr(
        QMessageBox,
        "exec",
        lambda _dialog: modal_calls.append(True)
        or QMessageBox.DialogCode.Rejected,
    )

    _send_accepted_drop(page.editor, _file_mime([path]))

    cards = repositories.list_cards(page.document_id)
    assert len(cards) == 1
    assert page.editor.card_id == cards[0].id
    assert page.editor.toPlainText() == "첫 줄\n둘째 줄"
    assert page.editor.textCursor().position() == 0
    assert session_changes == [True]
    assert connected == [cards[0].id]
    assert modal_calls == []
    assert errors == []


def test_dirty_card_save_failure_shows_one_real_modal_then_imports(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    QTest.keyClicks(page.editor, "base card")
    previous_card_id = page.editor.card_id
    assert previous_card_id is not None
    assert page.editor.save_current()
    QTest.keyClicks(page.editor, " dirty")
    session = page.editor.session
    assert session is not None and session.dirty
    path = tmp_path / "after-discard.md"
    path.write_text("드롭 카드", encoding="utf-8")
    modal_calls: list[QMessageBox] = []
    gate_calls: list[bool] = []
    original_gate = page.can_leave_editor

    def fail_save(_session: object) -> object:
        raise RuntimeError("주입된 저장 실패")

    def gate_spy(
        *,
        choice_provider: Any = None,
        protect_now: bool = False,
    ) -> bool:
        gate_calls.append(protect_now)
        return original_gate(
            choice_provider=choice_provider,
            protect_now=protect_now,
        )

    def discard_modal() -> None:
        modal = QApplication.activeModalWidget()
        assert isinstance(modal, QMessageBox)
        modal_calls.append(modal)
        discard = next(
            button for button in modal.buttons() if button.text() == "버리기"
        )
        discard.click()

    monkeypatch.setattr(page.editor._save_coordinator, "save", fail_save)
    monkeypatch.setattr(page, "can_leave_editor", gate_spy)
    QTimer.singleShot(0, discard_modal)

    _send_accepted_drop(page.editor, _file_mime([path]))

    imported = [
        card
        for card in repositories.list_cards(page.document_id)
        if card.source is CardSource.IMPORT
    ]
    assert gate_calls == [True]
    assert len(modal_calls) == 1
    assert len(imported) == 1
    assert imported[0].body == "드롭 카드"
    assert page.editor.card_id == imported[0].id
    assert page.editor.toPlainText() == "드롭 카드"
    assert errors == []


def test_hidden_row_import_still_refreshes_history(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    page.source_filter.setCurrentIndex(page.source_filter.findData("typing"))
    assert page.stream.card_model.rowCount() == 0
    path = tmp_path / "hidden-import.txt"
    path.write_text("필터에 숨은 카드", encoding="utf-8")
    refresh_calls: list[bool] = []
    set_card_calls: list[str] = []
    original_refresh = page.history.refresh
    original_set_card = page.history.set_card

    def refresh_spy() -> None:
        refresh_calls.append(True)
        original_refresh()

    def set_card_spy(card_id: str) -> None:
        set_card_calls.append(card_id)
        original_set_card(card_id)

    monkeypatch.setattr(page.history, "refresh", refresh_spy)
    monkeypatch.setattr(page.history, "set_card", set_card_spy)

    _send_accepted_drop(page.editor, _file_mime([path]))

    imported = [
        card
        for card in repositories.list_cards(page.document_id)
        if card.source is CardSource.IMPORT
    ]
    assert len(imported) == 1
    assert page.stream.card_model.rowCount() == 0
    assert refresh_calls == [True]
    assert set_card_calls == [imported[0].id]
    assert page.editor.card_id == imported[0].id
    assert errors == []


def test_zero_success_drop_preserves_connected_dirty_state(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    QTest.keyClicks(page.editor, "committed")
    card_id = page.editor.card_id
    assert card_id is not None
    QTest.keyClicks(page.editor, " dirty")
    session = page.editor.session
    assert session is not None and session.dirty
    path = tmp_path / "creation-failure.txt"
    path.write_text("생성 실패 본문", encoding="utf-8")
    before_text = page.editor.toPlainText()
    before_draft = (
        session.draft_id,
        session.text,
        session.cursor_position_qchar,
        session.base_revision_id,
        session.dirty,
    )
    before_revisions = repositories.list_revisions(card_id)
    gate_calls: list[bool] = []
    monkeypatch.setattr(
        page,
        "can_leave_editor",
        lambda **_kwargs: gate_calls.append(True) or True,
    )

    def fail_creation(*_args: Any, **_kwargs: Any) -> Any:
        raise RuntimeError("주입된 생성 실패")

    monkeypatch.setattr(page.card_service, "create_cards", fail_creation)

    _send_accepted_drop(page.editor, _file_mime([path]))

    after_session = page.editor.session
    assert after_session is not None
    assert page.editor.card_id == card_id
    assert page.editor.toPlainText() == before_text
    assert (
        after_session.draft_id,
        after_session.text,
        after_session.cursor_position_qchar,
        after_session.base_revision_id,
        after_session.dirty,
    ) == before_draft
    assert repositories.list_revisions(card_id) == before_revisions
    assert gate_calls == [True]
    assert len(errors) == 1
    assert "생성/연결 실패" in errors[0][1]


def test_zero_valid_bodies_never_runs_leave_gate(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    QTest.keyClicks(page.editor, "connected card")
    card_id = page.editor.card_id
    assert card_id is not None
    QTest.keyClicks(page.editor, " dirty")
    session = page.editor.session
    assert session is not None and session.dirty
    before_text = page.editor.toPlainText()
    before_session = (
        session.draft_id,
        session.text,
        session.cursor_position_qchar,
        session.base_revision_id,
        session.dirty,
    )
    empty_utf16_bom = tmp_path / "empty-utf16-bom.txt"
    empty = tmp_path / "empty.md"
    empty_utf16_bom.write_bytes(b"\xff\xfe")
    empty.write_text(" \r\n\t", encoding="utf-8")
    gate_calls: list[bool] = []
    monkeypatch.setattr(
        page,
        "can_leave_editor",
        lambda **_kwargs: gate_calls.append(True) or True,
    )

    _send_accepted_drop(page.editor, _file_mime([empty_utf16_bom, empty]))

    after_session = page.editor.session
    assert after_session is not None
    assert page.editor.card_id == card_id
    assert page.editor.toPlainText() == before_text
    assert (
        after_session.draft_id,
        after_session.text,
        after_session.cursor_position_qchar,
        after_session.base_revision_id,
        after_session.dirty,
    ) == before_session
    assert gate_calls == []
    assert len(errors) == 1
    assert "판독 실패" in errors[0][1]


def test_file_level_failures_do_not_block_valid_files(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    first = tmp_path / "first.md"
    oversized = tmp_path / "oversized.txt"
    unreadable = tmp_path / "unreadable.md"
    empty_utf16_bom = tmp_path / "empty-utf16-bom.txt"
    last = tmp_path / "last.md"
    first.write_text("첫 유효 본문", encoding="utf-8")
    oversized.write_bytes(b"x" * (_FOUR_MIB + 1))
    unreadable.write_text("읽히면 안 됨", encoding="utf-8")
    empty_utf16_bom.write_bytes(b"\xff\xfe")
    last.write_text("둘째 유효 본문", encoding="utf-8")
    paths = [first, oversized, unreadable, empty_utf16_bom, last]
    original_open = Path.open

    def open_with_failure(target: Path, *args: Any, **kwargs: Any) -> Any:
        if target == unreadable and args and args[0] == "rb":
            raise OSError("주입된 판독 실패")
        return original_open(target, *args, **kwargs)

    monkeypatch.setattr(Path, "open", open_with_failure)

    _send_accepted_drop(page.editor, _file_mime(paths))

    cards = repositories.list_cards(page.document_id)
    assert [card.body for card in cards] == ["첫 유효 본문", "둘째 유효 본문"]
    assert page.editor.card_id is None
    assert len(errors) == 1
    assert "판독 실패" in errors[0][1]
    assert "oversized.txt" in errors[0][1]
    assert "unreadable.md" in errors[0][1]
    assert "empty-utf16-bom.txt" in errors[0][1]


def test_total_limit_is_all_or_nothing(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    first = tmp_path / "limit.txt"
    last = tmp_path / "last.md"
    first.write_bytes(b"a" * _FOUR_MIB)
    last.write_bytes(b"b")
    before_counts = tuple(
        database.connection.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
        for table in ("cards", "card_revisions", "capture_operations", "edit_events")
    )
    gate_calls: list[bool] = []
    monkeypatch.setattr(
        page,
        "can_leave_editor",
        lambda **_kwargs: gate_calls.append(True) or True,
    )

    _send_accepted_drop(page.editor, _file_mime([first, last]))

    after_counts = tuple(
        database.connection.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
        for table in ("cards", "card_revisions", "capture_operations", "edit_events")
    )
    assert after_counts == before_counts
    assert gate_calls == []
    assert errors == [
        (
            "파일 드롭 실패",
            "판독 실패:\n- last.md: 드롭 전체 4 MiB 상한을 초과했습니다.",
        )
    ]


def test_import_uses_single_bounded_byte_snapshot(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    path = tmp_path / "snapshot.txt"
    path.write_text("최초 snapshot", encoding="utf-8")
    original_open = Path.open
    open_calls: list[int] = []

    class _MutatingReader:
        def __init__(self, stream: IO[bytes]) -> None:
            self._stream = stream

        def __enter__(self) -> _MutatingReader:
            self._stream.__enter__()
            return self

        def __exit__(
            self,
            exception_type: type[BaseException] | None,
            exception: BaseException | None,
            traceback: TracebackType | None,
        ) -> bool | None:
            return self._stream.__exit__(exception_type, exception, traceback)

        def read(self, size: int = -1) -> bytes:
            data = self._stream.read(size)
            with original_open(path, "w", encoding="utf-8") as target:
                target.write("두 번째 내용")
            return data

    def open_spy(target: Path, *args: Any, **kwargs: Any) -> Any:
        stream = original_open(target, *args, **kwargs)
        if target == path and args and args[0] == "rb":
            open_calls.append(1)
            return _MutatingReader(stream)
        return stream

    monkeypatch.setattr(Path, "open", open_spy)

    _send_accepted_drop(page.editor, _file_mime([path]))

    cards = repositories.list_cards(page.document_id)
    assert [card.body for card in cards] == ["최초 snapshot"]
    assert path.read_text(encoding="utf-8") == "두 번째 내용"
    assert open_calls == [1]
    assert errors == []


def test_drop_rejects_nonlocal_directory_and_missing_urls_before_gate(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    directory = tmp_path / "directory.md"
    directory.mkdir()
    missing = tmp_path / "missing.txt"
    gate_calls: list[bool] = []
    read_calls: list[Path] = []
    original_open = Path.open

    def open_spy(target: Path, *args: Any, **kwargs: Any) -> Any:
        read_calls.append(target)
        return original_open(target, *args, **kwargs)

    monkeypatch.setattr(Path, "open", open_spy)
    monkeypatch.setattr(
        page,
        "can_leave_editor",
        lambda **_kwargs: gate_calls.append(True) or True,
    )

    nonlocal_enter = _send_enter(
        page.editor,
        _url_mime([QUrl("https://example.com/note.md")]),
    )
    assert not nonlocal_enter.isAccepted()
    _send_accepted_drop(page.editor, _file_mime([directory]))
    _send_accepted_drop(page.editor, _file_mime([missing]))

    assert gate_calls == []
    assert read_calls == []
    assert repositories.list_cards(page.document_id) == ()
    assert len(errors) == 2


def test_duplicate_urls_are_deduplicated(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    page = _page(qtbot, database, repositories, errors=[])
    path = tmp_path / "duplicate.md"
    path.write_text("한 번만", encoding="utf-8")

    _send_accepted_drop(page.editor, _file_mime([path, path]))

    cards = repositories.list_cards(page.document_id)
    assert len(cards) == 1
    assert cards[0].body == "한 번만"


def test_import_source_is_recorded_in_card_operation_and_event(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    page = _page(qtbot, database, repositories, errors=[])
    path = tmp_path / "source.txt"
    path.write_text("출처 확인", encoding="utf-8")

    _send_accepted_drop(page.editor, _file_mime([path]))

    card = repositories.list_cards(page.document_id)[0]
    operation = repositories.get_capture_operation(card.operation_id)
    events = [
        event
        for event in repositories.list_events(page.document_id)
        if event.card_id == card.id and event.event_type is EventType.CREATE
    ]
    assert card.source is CardSource.IMPORT
    assert operation is not None
    assert operation.source is CaptureOperationSource.IMPORT
    assert len(events) == 1
    assert events[0].source is EventSource.IMPORT


def test_card_mime_drop_is_ignored_by_editor(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page = _page(qtbot, database, repositories, errors=[])
    mime = QMimeData()
    mime.setData("application/x-pynote-card-id", b"spoofed-card")
    dropped: list[object] = []
    page.editor.files_dropped.connect(dropped.append)

    enter = _send_enter(page.editor, mime)

    assert not enter.isAccepted()
    assert dropped == []
    assert page.editor.toPlainText() == ""
    assert repositories.list_cards(page.document_id) == ()


def test_url_with_companion_text_does_not_insert_text(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    page = _page(qtbot, database, repositories, errors=[])
    paths = [tmp_path / "one.md", tmp_path / "two.txt"]
    paths[0].write_text("파일 하나", encoding="utf-8")
    paths[1].write_text("파일 둘", encoding="utf-8")
    companion = "삽입되면 안 되는 경로 문자열"

    _send_accepted_drop(
        page.editor,
        _file_mime(paths, companion_text=companion),
    )

    assert page.editor.toPlainText() == ""
    assert [card.body for card in repositories.list_cards(page.document_id)] == [
        "파일 하나",
        "파일 둘",
    ]
    assert all(
        companion not in card.body
        for card in repositories.list_cards(page.document_id)
    )


def test_plain_text_drop_still_creates_typing_paste_card(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page = _page(qtbot, database, repositories, errors=[])
    mime = QMimeData()
    mime.setText("순수 텍스트 드롭")

    _send_accepted_drop(page.editor, mime)

    card = repositories.list_cards(page.document_id)[0]
    assert card.body == "순수 텍스트 드롭"
    assert card.source is CardSource.PASTE
    operation = repositories.get_capture_operation(card.operation_id)
    assert operation is not None
    assert operation.source is CaptureOperationSource.PASTE


def test_drop_on_empty_surface_with_text_preserves_widget_text(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    page = _page(qtbot, database, repositories, errors=[])
    QTest.keyClicks(page.editor, "   ")
    assert page.editor.card_id is None
    new_session = page.editor._new_session
    assert new_session is not None
    text = page.editor.toPlainText()
    paths = [tmp_path / "one.txt", tmp_path / "two.md"]
    paths[0].write_text("첫 파일", encoding="utf-8")
    paths[1].write_text("둘째 파일", encoding="utf-8")

    _send_accepted_drop(page.editor, _file_mime(paths))

    assert page.editor.card_id is None
    assert page.editor.toPlainText() == text
    assert page.editor._new_session is not None
    assert page.editor._new_session.draft_id == new_session.draft_id
    drafts = repositories.list_drafts(page.document_id)
    assert any(draft.id == new_session.draft_id for draft in drafts)
    assert [card.body for card in repositories.list_cards(page.document_id)] == [
        "첫 파일",
        "둘째 파일",
    ]


@pytest.mark.parametrize("failure_kind", ["return", "exception"])
def test_single_import_draft_coordinator_return_and_exception_failures(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
    failure_kind: str,
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    QTest.keyClicks(page.editor, "clean card")
    previous_card_id = page.editor.card_id
    assert previous_card_id is not None
    assert page.editor.save_current()
    previous_card = repositories.get_card(previous_card_id)
    assert previous_card is not None
    path = tmp_path / f"{failure_kind}.txt"
    path.write_text("연결 실패 카드", encoding="utf-8")
    open_calls: list[str] = []

    if failure_kind == "return":

        def fail_open(card: Any, **_kwargs: Any) -> None:
            open_calls.append(card.id)
            return None

    else:

        def fail_open(card: Any, **_kwargs: Any) -> None:
            open_calls.append(card.id)
            raise RuntimeError("주입된 연결 예외")

    monkeypatch.setattr(page.draft_coordinator, "open_card", fail_open)

    _send_accepted_drop(page.editor, _file_mime([path]))

    cards = repositories.list_cards(page.document_id)
    imported = [card for card in cards if card.source is CardSource.IMPORT]
    assert len(cards) == 2
    assert len(imported) == 1
    assert imported[0].body == "연결 실패 카드"
    assert open_calls == [imported[0].id]
    assert page.editor.card_id is None
    assert page.editor.toPlainText() == ""
    new_session = page.editor._new_session
    assert new_session is not None
    assert new_session.text == page.editor.toPlainText()
    assert page.draft_coordinator.session(new_session.draft_id) == new_session
    preserved = repositories.get_card(previous_card_id)
    assert preserved is not None
    assert preserved.body == previous_card.body
    assert len(errors) == 1
    assert "생성/연결 실패" in errors[0][1]


def test_gate_rejection_cancels_creation_after_validation(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    path = tmp_path / "valid.md"
    path.write_text("검증은 끝남", encoding="utf-8")
    gate_calls: list[bool] = []
    original_open = Path.open
    read_calls: list[int] = []

    def open_spy(target: Path, *args: Any, **kwargs: Any) -> Any:
        if target == path and args and args[0] == "rb":
            read_calls.append(1)
        return original_open(target, *args, **kwargs)

    monkeypatch.setattr(Path, "open", open_spy)

    def reject_gate(*, protect_now: bool = False, **_kwargs: Any) -> bool:
        gate_calls.append(protect_now)
        return False

    monkeypatch.setattr(page, "can_leave_editor", reject_gate)

    _send_accepted_drop(page.editor, _file_mime([path]))

    assert read_calls == [1]
    assert gate_calls == [True]
    assert repositories.list_cards(page.document_id) == ()
    assert errors == []


def test_creation_failures_are_aggregated_and_later_files_continue(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    empty_utf16_bom = tmp_path / "empty-utf16-bom.txt"
    paths = [empty_utf16_bom, tmp_path / "failure.md", tmp_path / "success.txt"]
    empty_utf16_bom.write_bytes(b"\xff\xfe")
    paths[1].write_text("생성 실패", encoding="utf-8")
    paths[2].write_text("후속 성공", encoding="utf-8")
    original_create = page.card_service.create_cards
    calls: list[str] = []

    def create_with_first_failure(
        document_id: str,
        body: str,
        **kwargs: Any,
    ) -> Any:
        calls.append(body)
        if len(calls) == 1:
            raise RuntimeError("주입된 생성 실패")
        return original_create(document_id, body, **kwargs)

    monkeypatch.setattr(
        page.card_service,
        "create_cards",
        create_with_first_failure,
    )

    _send_accepted_drop(page.editor, _file_mime(paths))

    assert calls == ["생성 실패", "후속 성공"]
    cards = repositories.list_cards(page.document_id)
    assert [card.body for card in cards] == ["후속 성공"]
    assert page.editor.card_id == cards[0].id
    assert len(errors) == 1
    assert "failure.md" in errors[0][1]
    assert "판독 실패" in errors[0][1]
    assert "생성/연결 실패" in errors[0][1]


@pytest.mark.parametrize("limit_kind", ["file-count", "individual-bytes"])
def test_drop_rejects_count_and_individual_limits_before_gate(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
    limit_kind: str,
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    if limit_kind == "file-count":
        paths = [tmp_path / f"{index}.txt" for index in range(21)]
        for path in paths:
            path.write_text("x", encoding="utf-8")
        expected_message = "한 번에 20개 파일까지만 드롭할 수 있습니다."
    else:
        paths = [tmp_path / "oversized.md"]
        paths[0].write_bytes(b"x" * (_FOUR_MIB + 1))
        expected_message = "oversized.md: 파일당 4 MiB 상한을 초과했습니다."
    gate_calls: list[bool] = []
    monkeypatch.setattr(
        page,
        "can_leave_editor",
        lambda **_kwargs: gate_calls.append(True) or True,
    )

    _send_accepted_drop(page.editor, _file_mime(paths))

    assert gate_calls == []
    assert repositories.list_cards(page.document_id) == ()
    assert errors == [("파일 드롭 실패", f"판독 실패:\n- {expected_message}")]


def _scroll_spy(
    page: DocumentPage,
    monkeypatch: MonkeyPatch,
) -> list[str]:
    """스트림의 scrollTo 대상 카드 ID를 호출 순서대로 수집한다."""
    calls: list[str] = []
    original = page.stream.scrollTo

    def spy(
        index: QModelIndex,
        hint: QAbstractItemView.ScrollHint = (
            QAbstractItemView.ScrollHint.EnsureVisible
        ),
    ) -> None:
        calls.append(str(index.data(CardRole.CARD_ID)))
        original(index, hint)

    monkeypatch.setattr(page.stream, "scrollTo", spy)
    return calls


def test_multi_card_drop_reveals_last_created_card_without_connecting(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    first = tmp_path / "first.txt"
    last = tmp_path / "last.txt"
    first.write_text("첫 카드", encoding="utf-8")
    last.write_text("마지막 카드", encoding="utf-8")
    page.editor.setFocus()
    scroll_calls = _scroll_spy(page, monkeypatch)

    _send_accepted_drop(page.editor, _file_mime([first, last]))

    cards = repositories.list_cards(page.document_id)
    assert tuple(card.body for card in cards) == ("첫 카드", "마지막 카드")
    target = cards[-1]
    current = page.stream.currentIndex()
    assert current.isValid()
    assert current.data(CardRole.CARD_ID) == target.id
    # setCurrentIndex가 Qt 내부에서 scrollTo를 한 번 더 부르므로 횟수가 아니라
    # 대상만 단언한다.
    assert scroll_calls and set(scroll_calls) == {target.id}
    assert page.editor.card_id is None
    assert page.focusWidget() is page.editor
    assert errors == []


def test_single_success_among_multiple_files_still_connects_editor(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    blank = tmp_path / "blank.txt"
    good = tmp_path / "good.txt"
    blank.write_bytes(b" \r\n\t")
    good.write_text("살아남은 카드", encoding="utf-8")

    _send_accepted_drop(page.editor, _file_mime([blank, good]))

    cards = repositories.list_cards(page.document_id)
    assert len(cards) == 1
    assert cards[0].body == "살아남은 카드"
    assert page.editor.card_id == cards[0].id
    assert page.stream.currentIndex().data(CardRole.CARD_ID) == cards[0].id
    assert errors == [
        (
            "파일 드롭 실패",
            "판독 실패:\n- blank.txt: 가져올 비어 있지 않은 문단이 없습니다.",
        )
    ]


def test_extended_whitespace_only_drop_reports_read_failure_without_side_effects(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    blank = tmp_path / "extended-blank.txt"
    blank.write_bytes(b"\v\f")
    tables = ("cards", "card_revisions", "capture_operations", "edit_events")
    before_counts = tuple(
        database.connection.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
        for table in tables
    )
    before_events = repositories.list_events(page.document_id)

    _send_accepted_drop(page.editor, _file_mime([blank]))

    after_counts = tuple(
        database.connection.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
        for table in tables
    )
    assert after_counts == before_counts
    assert repositories.list_events(page.document_id) == before_events
    assert repositories.list_cards(page.document_id) == ()
    assert errors == [
        (
            "파일 드롭 실패",
            "판독 실패:\n- extended-blank.txt: "
            "가져올 비어 있지 않은 문단이 없습니다.",
        )
    ]


def test_extended_whitespace_drop_with_valid_file_creates_only_valid_card(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    blank = tmp_path / "extended-blank.txt"
    valid = tmp_path / "valid.txt"
    blank.write_bytes(b"\v\f")
    valid.write_text("정상 카드", encoding="utf-8")

    _send_accepted_drop(page.editor, _file_mime([blank, valid]))

    cards = repositories.list_cards(page.document_id)
    assert tuple(card.body for card in cards) == ("정상 카드",)
    assert page.editor.card_id == cards[0].id
    assert errors == [
        (
            "파일 드롭 실패",
            "판독 실패:\n- extended-blank.txt: "
            "가져올 비어 있지 않은 문단이 없습니다.",
        )
    ]
    assert page.editor.request_close()


def test_extended_whitespace_on_empty_editor_skips_card_service(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    monkeypatch: MonkeyPatch,
) -> None:
    page = _page(qtbot, database, repositories, errors=[])
    original_create = page.card_service.create_cards
    create_calls: list[str] = []

    def observe_create(document_id: str, text: str, **kwargs: Any) -> Any:
        create_calls.append(text)
        return original_create(document_id, text, **kwargs)

    monkeypatch.setattr(page.card_service, "create_cards", observe_create)
    mime = QMimeData()
    mime.setText("\v\f\u00a0")

    _send_accepted_drop(page.editor, mime)

    assert create_calls == []
    assert page.editor.card_id is None
    assert repositories.list_cards(page.document_id) == ()
    assert page.editor.request_close()


def test_existing_extended_whitespace_card_survives_reopen_then_clean_exit_deletes(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    document = document_service.create_document(repositories, "기존 공백 카드")
    body = "\v\f"
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
    tables = ("cards", "card_revisions", "capture_operations", "edit_events")
    before_counts = tuple(
        database.connection.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
        for table in tables
    )
    before_events = repositories.list_events(document.id)

    with Database(database.path) as reopened_database:
        reopened_repositories = Repositories(reopened_database)
        reopened_card = reopened_repositories.get_card(card.id)
        after_counts = tuple(
            reopened_database.connection.execute(
                f"SELECT COUNT(*) FROM {table}"
            ).fetchone()[0]
            for table in tables
        )
        assert reopened_card is not None
        assert reopened_card.body == body
        assert reopened_card.deleted_at_us is None
        assert after_counts == before_counts
        assert reopened_repositories.list_events(document.id) == before_events

    page = DocumentPage(database, repositories, document.id)
    qtbot.addWidget(page)
    page.resize(900, 600)
    page.show()
    assert page.open_card(card.id)
    assert page.editor.session is not None
    assert page.editor.session.dirty is False
    assert page.editor.request_close()

    deleted = repositories.get_card(card.id)
    assert deleted is not None
    assert deleted.body == body
    assert deleted.deleted_at_us is not None
    delete_events = tuple(
        event
        for event in repositories.list_events(document.id)
        if event.card_id == card.id and event.event_type is EventType.DELETE
    )
    assert len(delete_events) == 1


def test_multi_card_drop_hidden_by_filter_keeps_selection_and_filter(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    kept = page.card_service.create_cards(page.document_id, "타이핑 카드")[0]
    page.refresh()
    page.source_filter.setCurrentIndex(page.source_filter.findData("typing"))
    page.stream.setCurrentIndex(page.stream.card_model.index_for_card(kept.id))
    first = tmp_path / "hidden-first.txt"
    last = tmp_path / "hidden-last.txt"
    first.write_text("숨은 첫 카드", encoding="utf-8")
    last.write_text("숨은 마지막 카드", encoding="utf-8")
    scroll_calls = _scroll_spy(page, monkeypatch)

    _send_accepted_drop(page.editor, _file_mime([first, last]))

    assert len(repositories.list_cards(page.document_id)) == 3
    assert page.stream.card_model.rowCount() == 1
    assert page.stream.currentIndex().data(CardRole.CARD_ID) == kept.id
    assert scroll_calls == []
    assert page.source_filter.currentData() == "typing"
    assert errors == []


def test_multi_card_drop_scrolls_offscreen_last_created_card_into_view(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    for number in range(20):
        page.card_service.create_cards(page.document_id, f"기존 카드 {number}")
    page.refresh()
    page.sort_combo.setCurrentIndex(page.sort_combo.findData("capture"))
    model = page.stream.card_model
    page.stream.scrollTo(model.index(0))
    existing_last = model.index(model.rowCount() - 1)
    qtbot.waitUntil(
        lambda: (
            page.stream.visualRect(existing_last).isValid()
            and not page.stream.visualRect(existing_last).intersects(
                page.stream.viewport().rect()
            )
        )
    )
    first = tmp_path / "visible-first.txt"
    last = tmp_path / "visible-last.txt"
    first.write_text("새 첫 카드", encoding="utf-8")
    last.write_text("새 마지막 카드", encoding="utf-8")
    pre_reveal_card_ids: list[str] = []
    original_reveal = page.reveal_card

    def reveal_spy(card_id: str) -> bool:
        index = model.index_for_card(card_id)
        assert index.isValid()
        target_rect = page.stream.visualRect(index)
        viewport_rect = page.stream.viewport().rect()
        assert target_rect.isValid()
        assert viewport_rect.isValid()
        assert not target_rect.intersects(viewport_rect)
        selection_model = page.stream.selectionModel()
        assert selection_model is not None
        # currentChanged도 자동 스크롤하므로 준비 구간만 막아 명시 호출을 격리한다.
        signals_were_blocked = selection_model.blockSignals(True)
        try:
            selection_model.setCurrentIndex(
                index,
                QItemSelectionModel.SelectionFlag.NoUpdate,
            )
        finally:
            selection_model.blockSignals(signals_were_blocked)
        pre_reveal_card_ids.append(card_id)
        return original_reveal(card_id)

    monkeypatch.setattr(page, "reveal_card", reveal_spy)

    _send_accepted_drop(page.editor, _file_mime([first, last]))

    imported = [
        card
        for card in repositories.list_cards(page.document_id)
        if card.source is CardSource.IMPORT
    ]
    assert tuple(card.body for card in imported) == ("새 첫 카드", "새 마지막 카드")
    target = imported[-1]
    assert pre_reveal_card_ids == [target.id]
    target_index = model.index_for_card(target.id)
    qtbot.waitUntil(
        lambda: page.stream.visualRect(target_index).intersects(
            page.stream.viewport().rect()
        )
    )
    assert page.stream.visualRect(target_index).intersects(
        page.stream.viewport().rect()
    )
    assert page.stream.currentIndex().data(CardRole.CARD_ID) == target.id
    assert page.editor.card_id is None
    assert errors == []


def test_multi_card_drop_selects_last_success_when_final_input_is_blank(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    first = tmp_path / "first-success.txt"
    last_success = tmp_path / "last-success.txt"
    final_blank = tmp_path / "final-blank.txt"
    first.write_text("첫 성공 카드", encoding="utf-8")
    last_success.write_text("마지막 성공 카드", encoding="utf-8")
    final_blank.write_bytes(b" \r\n\t")

    _send_accepted_drop(
        page.editor,
        _file_mime([first, last_success, final_blank]),
    )

    cards = repositories.list_cards(page.document_id)
    assert tuple(card.body for card in cards) == ("첫 성공 카드", "마지막 성공 카드")
    target = cards[-1]
    assert page.stream.currentIndex().data(CardRole.CARD_ID) == target.id
    assert page.editor.card_id is None
    assert errors == [
        (
            "파일 드롭 실패",
            "판독 실패:\n- final-blank.txt: 가져올 비어 있지 않은 문단이 없습니다.",
        )
    ]


@pytest.mark.parametrize("sort_mode", ["recency", "position", "capture"])
def test_multi_card_drop_selects_same_target_card_id_in_every_sort_mode(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    sort_mode: str,
) -> None:
    errors: list[tuple[str, str]] = []
    page = _page(qtbot, database, repositories, errors=errors)
    page.card_service.create_cards(page.document_id, "기존 첫 카드")
    page.refresh()
    page.sort_combo.setCurrentIndex(page.sort_combo.findData(sort_mode))
    first = tmp_path / "sort-first.txt"
    target_path = tmp_path / "sort-target.txt"
    first.write_text("정렬 첫 카드", encoding="utf-8")
    target_path.write_text("정렬 대상 카드", encoding="utf-8")

    _send_accepted_drop(page.editor, _file_mime([first, target_path]))

    target = next(
        card
        for card in repositories.list_cards(page.document_id)
        if card.body == "정렬 대상 카드"
    )
    target_index = page.stream.card_model.index_for_card(target.id)
    assert target_index.isValid()
    expected_row = (
        0 if sort_mode == "recency" else page.stream.card_model.rowCount() - 1
    )
    assert target_index.row() == expected_row
    assert page.stream.currentIndex().data(CardRole.CARD_ID) == target.id
    assert page.editor.card_id is None
    assert errors == []
