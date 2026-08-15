from __future__ import annotations

from pathlib import Path
from time import monotonic

import pytest
from PySide6.QtCore import QMimeData, QPoint, QPointF, QSettings, Qt, QTimer, QUrl
from PySide6.QtGui import QDragEnterEvent, QDropEvent, QInputMethodEvent
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QMessageBox, QStackedWidget
from pytest import MonkeyPatch
from pytestqt.qtbot import QtBot

from pynote.app import SqliteWorkspaceStateStore, initialize_device_settings
from pynote.application import document_service
from pynote.application.draft_coordinator import (
    DraftCoordinator,
    DraftDisposition,
    DraftSession,
)
from pynote.application.save_coordinator import (
    SaveConflict,
    SaveCoordinator,
    SaveOutcome,
    SaveResult,
)
from pynote.domain.events import EventSource, EventType
from pynote.domain.models import (
    CaptureOperationSource,
    Card,
    CardSource,
    Draft,
    DraftKind,
)
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories, text_hash
from pynote.ui.document_page import DocumentPage
from pynote.ui.editor.card_editor import CardEditor, CloseChoice
from pynote.ui.main_window import MainWindow


def _page(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> DocumentPage:
    document = document_service.create_document(repositories, "단일 편집면")
    page = DocumentPage(database, repositories, document.id)
    qtbot.addWidget(page)
    page.resize(900, 600)
    page.show()
    return page


def _window(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> tuple[MainWindow, DocumentPage]:
    settings = QSettings(
        str(tmp_path / "single-editor.ini"),
        QSettings.Format.IniFormat,
    )
    initialize_device_settings(settings)
    settings.setValue("first_run/guide_shown", True)
    window = MainWindow(
        repositories,
        SqliteWorkspaceStateStore(database),
        settings=settings,
    )
    qtbot.addWidget(window)
    window.resize(1_000, 700)
    window.show()
    document = document_service.create_document(repositories, "포커스")
    assert window.open_document_local(document.id)
    page = window.active_document_page()
    assert page is not None
    window.activateWindow()
    qtbot.waitUntil(lambda: QApplication.activeWindow() is window)
    page.editor.setFocus()
    qtbot.waitUntil(lambda: QApplication.focusWidget() is page.editor)
    return window, page


def _paste(editor: CardEditor, text: str) -> None:
    mime = QMimeData()
    mime.setText(text)
    editor.insertFromMimeData(mime)


def _commit(editor: CardEditor, text: str) -> None:
    event = QInputMethodEvent()
    event.setCommitString(text)
    QApplication.sendEvent(editor, event)


def _empty_point(page: DocumentPage) -> QPoint:
    point = page.stream.viewport().rect().bottomRight() - QPoint(10, 10)
    assert not page.stream.indexAt(point).isValid()
    return point


def _utf16_units(text: str) -> int:
    return len(text.encode("utf-16-le")) // 2


def _assert_creation_sources(
    repositories: Repositories,
    card: Card,
    *,
    card_source: CardSource,
    operation_source: CaptureOperationSource,
    event_source: EventSource,
) -> None:
    operation = repositories.get_capture_operation(card.operation_id)
    assert operation is not None
    events = tuple(
        event
        for event in repositories.list_events(card.document_id)
        if event.card_id == card.id and event.event_type is EventType.CREATE
    )
    assert card.source is card_source
    assert operation.source is operation_source
    assert len(events) == 1
    assert events[0].source is event_source


def _save_empty_connected_card(
    page: DocumentPage,
    repositories: Repositories,
) -> Card:
    _paste(page.editor, "지울 본문")
    card_id = page.editor.card_id
    assert card_id is not None
    page.editor.selectAll()
    QTest.keyClick(page.editor, Qt.Key.Key_Backspace)
    assert page.editor.session is not None
    assert page.editor.session.dirty
    assert page.editor.save_current()
    card = repositories.get_card(card_id)
    assert card is not None
    assert card.body == ""
    assert card.deleted_at_us is None
    assert not page.editor.session.dirty
    return card


def _delete_events(
    repositories: Repositories,
    card: Card,
) -> tuple[object, ...]:
    return tuple(
        event
        for event in repositories.list_events(card.document_id)
        if event.card_id == card.id and event.event_type is EventType.DELETE
    )


@pytest.mark.parametrize(
    ("event_kind", "expected_count", "expected_source", "expected_text"),
    (
        pytest.param("preedit", 0, None, "", id="preedit-start"),
        pytest.param("ime-commit", 1, CardSource.TYPING, "한", id="ime-commit"),
        pytest.param("empty-commit", 0, None, "", id="ime-empty-commit"),
        pytest.param("ime-cancel", 0, None, "", id="ime-cancel"),
        pytest.param("typing", 1, CardSource.TYPING, "a", id="typing"),
        pytest.param("paste", 1, CardSource.PASTE, "붙여넣기", id="paste"),
        pytest.param("whitespace", 0, None, " \n\t", id="whitespace"),
    ),
)
def test_first_input_state_rows(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    event_kind: str,
    expected_count: int,
    expected_source: CardSource | None,
    expected_text: str,
) -> None:
    page = _page(qtbot, database, repositories)

    if event_kind == "preedit":
        QApplication.sendEvent(page.editor, QInputMethodEvent("ㅎ", []))
    elif event_kind == "ime-commit":
        _commit(page.editor, "한")
    elif event_kind == "empty-commit":
        QApplication.sendEvent(page.editor, QInputMethodEvent())
    elif event_kind == "ime-cancel":
        QApplication.sendEvent(page.editor, QInputMethodEvent("ㅎ", []))
        QApplication.sendEvent(page.editor, QInputMethodEvent())
    elif event_kind == "typing":
        QTest.keyClicks(page.editor, "a")
    elif event_kind == "paste":
        _paste(page.editor, "붙여넣기")
    else:
        _paste(page.editor, " \n\t")

    cards = repositories.list_cards(page.document_id)
    assert len(cards) == expected_count
    assert page.editor.toPlainText() == expected_text
    assert (page.editor.session is not None) is bool(expected_count)
    if expected_source is not None:
        assert cards[0].source is expected_source


def test_text_mime_drop_uses_paste_source(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page = _page(qtbot, database, repositories)
    _paste(page.editor, "드롭 본문")

    card = repositories.list_cards(page.document_id)[0]
    assert card.source is CardSource.PASTE
    assert card.body == "드롭 본문"


def test_preedit_paste_commit_stays_on_one_card(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page = _page(qtbot, database, repositories)
    QApplication.sendEvent(page.editor, QInputMethodEvent("ㅎ", []))
    _paste(page.editor, "붙여넣기")
    cards_after_paste = repositories.list_cards(page.document_id)
    assert len(cards_after_paste) == 1
    assert cards_after_paste[0].source is CardSource.PASTE

    _commit(page.editor, "한")

    assert len(repositories.list_cards(page.document_id)) == 1
    assert page.editor.toPlainText() == "붙여넣기한"
    assert page.editor.session is not None
    assert page.editor.session.card_id == cards_after_paste[0].id
    _assert_creation_sources(
        repositories,
        cards_after_paste[0],
        card_source=CardSource.PASTE,
        operation_source=CaptureOperationSource.PASTE,
        event_source=EventSource.PASTE,
    )


def test_connected_input_never_creates_second_card(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page = _page(qtbot, database, repositories)
    QTest.keyClicks(page.editor, "a")
    QTest.keyClicks(page.editor, "bc")

    assert len(repositories.list_cards(page.document_id)) == 1
    assert page.editor.toPlainText() == "abc"


def test_legacy_new_draft_restore_does_not_create_card(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    document = document_service.create_document(repositories, "legacy")
    repositories.create_draft(
        Draft(
            id="legacy-new-draft",
            document_id=document.id,
            card_id=None,
            draft_kind=DraftKind.NEW,
            base_revision_id=None,
            draft_text="legacy NEW 본문",
            draft_hash=text_hash("legacy NEW 본문"),
            cursor_position_qchar=7,
            updated_at_us=1_000,
        )
    )

    restored = DocumentPage(database, repositories, document.id)
    qtbot.addWidget(restored)

    assert restored.editor.toPlainText() == "legacy NEW 본문"
    assert restored.editor.session is None
    assert repositories.list_cards(document.id) == ()


def test_legacy_new_restore_and_settings_do_not_create_card(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    document = document_service.create_document(repositories, "legacy settings")
    body = "복원문\n😀"
    repositories.create_draft(
        Draft(
            id="legacy-settings-new",
            document_id=document.id,
            card_id=None,
            draft_kind=DraftKind.NEW,
            base_revision_id=None,
            draft_text=body,
            draft_hash=text_hash(body),
            cursor_position_qchar=_utf16_units(body),
            updated_at_us=2_000,
        )
    )
    settings = QSettings(
        str(tmp_path / "legacy-settings.ini"),
        QSettings.Format.IniFormat,
    )
    settings.setValue("editor/line_spacing", 1.7)

    page = DocumentPage(
        database,
        repositories,
        document.id,
        settings=settings,
    )
    qtbot.addWidget(page)
    page.apply_settings()

    assert page.editor.card_id is None
    assert page.editor.toPlainText() == body
    assert page.editor.textCursor().position() == _utf16_units(body)
    assert repositories.list_cards(document.id) == ()
    assert repositories.list_events(document.id) == ()
    assert page.editor._new_session is not None


@pytest.mark.parametrize(
    ("failure_kind", "input_kind", "body"),
    (
        pytest.param(
            "return",
            "paste",
            "paste 실패 😀",
            id="return-after-real-paste",
        ),
        pytest.param(
            "exception",
            "typing",
            "A",
            id="exception-after-real-typing",
        ),
    ),
)
def test_create_failure_keeps_text_and_new_backing(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    monkeypatch: MonkeyPatch,
    failure_kind: str,
    input_kind: str,
    body: str,
) -> None:
    page = _page(qtbot, database, repositories)
    create_calls: list[CaptureOperationSource] = []

    def fail(
        _document_id: str,
        _text: str,
        *,
        source: CaptureOperationSource,
        split: bool,
    ) -> tuple[Card, ...]:
        del split
        create_calls.append(source)
        if failure_kind == "exception":
            raise RuntimeError("주입된 생성 실패")
        return ()

    monkeypatch.setattr(page.card_service, "create_cards", fail)

    if input_kind == "paste":
        _paste(page.editor, body)
        expected_source = CaptureOperationSource.PASTE
    else:
        QTest.keyClicks(page.editor, body)
        expected_source = CaptureOperationSource.TYPING

    assert create_calls == [expected_source]
    assert repositories.list_cards(page.document_id) == ()
    assert page.editor.session is None
    assert page.editor.toPlainText() == body
    assert page.protect_now()
    assert any(
        draft.draft_kind is DraftKind.NEW
        and draft.draft_text == body
        for draft in repositories.list_drafts(page.document_id)
    )

    restored = DocumentPage(database, repositories, page.document_id)
    qtbot.addWidget(restored)

    assert restored.editor.toPlainText() == body
    assert restored.editor.textCursor().position() == _utf16_units(body)
    assert restored.editor.session is None
    assert repositories.list_cards(page.document_id) == ()


def test_failed_creation_deletion_undo_and_formatting_do_not_retry(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    monkeypatch: MonkeyPatch,
) -> None:
    page = _page(qtbot, database, repositories)
    real_create = page.card_service.create_cards
    calls: list[CaptureOperationSource] = []

    def fail_once(
        document_id: str,
        text: str,
        *,
        source: CaptureOperationSource,
        split: bool,
    ) -> tuple[Card, ...]:
        calls.append(source)
        if len(calls) == 1:
            return ()
        return real_create(document_id, text, source=source, split=split)

    monkeypatch.setattr(page.card_service, "create_cards", fail_once)
    _paste(page.editor, "ABC")
    assert calls == [CaptureOperationSource.PASTE]

    QTest.keyClick(page.editor, Qt.Key.Key_Backspace)
    QTest.keyClick(
        page.editor,
        Qt.Key.Key_Z,
        Qt.KeyboardModifier.ControlModifier,
    )
    page.editor.apply_line_spacing(1.5)

    assert calls == [CaptureOperationSource.PASTE]
    assert repositories.list_cards(page.document_id) == ()

    QTest.keyClicks(page.editor, "D")

    assert calls == [
        CaptureOperationSource.PASTE,
        CaptureOperationSource.TYPING,
    ]
    assert repositories.list_cards(page.document_id)[0].body == "ABCD"


@pytest.mark.parametrize("failure_kind", ("return", "exception"))
def test_connect_failure_retries_without_second_card(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    monkeypatch: MonkeyPatch,
    failure_kind: str,
) -> None:
    page = _page(qtbot, database, repositories)
    real_open = page.draft_coordinator.open_card
    attempted_ids: list[str] = []

    def fail_once(
        card: Card,
        *,
        disposition: DraftDisposition | None = None,
    ) -> DraftSession | None:
        attempted_ids.append(card.id)
        if len(attempted_ids) == 1:
            if failure_kind == "exception":
                raise RuntimeError("주입된 연결 실패")
            return None
        return real_open(card, disposition=disposition)

    monkeypatch.setattr(page.draft_coordinator, "open_card", fail_once)
    QTest.keyClicks(page.editor, "a")
    assert len(repositories.list_cards(page.document_id)) == 1
    assert page.editor.session is None

    QTest.keyClicks(page.editor, "b")

    assert len(repositories.list_cards(page.document_id)) == 1
    assert page.editor.session is not None
    assert page.editor.toPlainText() == "ab"
    assert len(set(attempted_ids)) == 1


def test_link_repeated_failures_reuse_id_and_inactive_pending_is_cleared(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    monkeypatch: MonkeyPatch,
) -> None:
    page = _page(qtbot, database, repositories)
    attempted_ids: list[str] = []

    def fail_link(
        card: Card,
        *,
        disposition: DraftDisposition | None = None,
    ) -> None:
        del disposition
        attempted_ids.append(card.id)
        return None

    monkeypatch.setattr(page.draft_coordinator, "open_card", fail_link)
    QTest.keyClicks(page.editor, "a")
    QTest.keyClicks(page.editor, "b")
    QTest.keyClicks(page.editor, "c")

    assert len(repositories.list_cards(page.document_id)) == 1
    assert len(set(attempted_ids)) == 1
    pending_id = attempted_ids[0]

    page.card_service.soft_delete(pending_id)
    QTest.keyClicks(page.editor, "d")

    cards = repositories.list_cards(page.document_id)
    assert len(cards) == 2
    deleted = next(card for card in cards if card.id == pending_id)
    replacement = next(card for card in cards if card.id != pending_id)
    assert deleted.deleted_at_us is not None
    assert page.editor._pending_card_id == replacement.id


@pytest.mark.parametrize("failure_kind", ("return", "exception"))
def test_link_failure_restart_allows_one_bounded_duplicate(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    monkeypatch: MonkeyPatch,
    failure_kind: str,
) -> None:
    page = _page(qtbot, database, repositories)

    def fail_link(
        _card: Card,
        *,
        disposition: DraftDisposition | None = None,
    ) -> None:
        del disposition
        if failure_kind == "exception":
            raise RuntimeError("주입된 연결 예외")
        return None

    monkeypatch.setattr(page.draft_coordinator, "open_card", fail_link)
    _paste(page.editor, "재기동 경계")
    assert len(repositories.list_cards(page.document_id)) == 1
    assert page.editor.card_id is None
    assert page.protect_now()

    restored = DocumentPage(database, repositories, page.document_id)
    qtbot.addWidget(restored)
    assert restored.editor.toPlainText() == "재기동 경계"
    QTest.keyClicks(restored.editor, "!")

    cards = repositories.list_cards(page.document_id)
    assert len(cards) == 2
    assert [card.body for card in cards] == ["재기동 경계", "재기동 경계!"]


@pytest.mark.parametrize(
    ("input_kind", "text"),
    (
        pytest.param("typing", "x", id="first-character"),
        pytest.param("paste", "첫 붙여넣기", id="first-paste"),
    ),
)
def test_first_input_undo_is_preserved(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    input_kind: str,
    text: str,
) -> None:
    page = _page(qtbot, database, repositories)
    if input_kind == "typing":
        QTest.keyClicks(page.editor, text)
    else:
        _paste(page.editor, text)
    assert page.editor.session is not None

    QTest.keyClick(
        page.editor,
        Qt.Key.Key_Z,
        Qt.KeyboardModifier.ControlModifier,
    )

    assert page.editor.toPlainText() == ""
    assert len(repositories.list_cards(page.document_id)) == 1


def test_attach_keeps_clean_baseline_cursor_and_undo(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page = _page(qtbot, database, repositories)
    text = "첫 문단\n😀 끝"
    _paste(page.editor, text)

    card = repositories.list_cards(page.document_id)[0]
    session = page.editor.session
    assert session is not None
    assert session.card_id == card.id
    assert session.base_revision_id == card.current_revision_id
    assert not session.dirty
    assert session.cursor_position_qchar == _utf16_units(text)
    assert page.editor.textCursor().position() == _utf16_units(text)

    QTest.keyClick(
        page.editor,
        Qt.Key.Key_Z,
        Qt.KeyboardModifier.ControlModifier,
    )
    assert page.editor.toPlainText() == ""


def test_whitespace_restart_and_exact_promotion_contract(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    monkeypatch: MonkeyPatch,
) -> None:
    page = _page(qtbot, database, repositories)
    monkeypatch.setattr(page.card_service, "_clock", lambda: 77_777)
    prefix = " \n\t"
    _paste(page.editor, prefix)

    assert repositories.list_cards(page.document_id) == ()
    assert page.editor.toPlainText() == prefix
    assert page.protect_now()

    restored = DocumentPage(database, repositories, page.document_id)
    qtbot.addWidget(restored)
    monkeypatch.setattr(restored.card_service, "_clock", lambda: 88_888)
    assert restored.editor.toPlainText() == prefix
    assert restored.editor.textCursor().position() == _utf16_units(prefix)

    QTest.keyClicks(restored.editor, "A")

    card = repositories.list_cards(page.document_id)[0]
    promoted_body = f"{prefix}A"
    assert card.body == promoted_body
    assert card.created_at_us == 88_888
    assert card.capture_seq == 1
    assert restored.editor.textCursor().position() == _utf16_units(promoted_body)
    _assert_creation_sources(
        repositories,
        card,
        card_source=CardSource.TYPING,
        operation_source=CaptureOperationSource.TYPING,
        event_source=EventSource.TYPING,
    )


@pytest.mark.parametrize("save_path", ("shortcut", "button"))
def test_empty_surface_save_never_calls_coordinator(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    monkeypatch: MonkeyPatch,
    save_path: str,
) -> None:
    page = _page(qtbot, database, repositories)
    calls: list[object] = []
    monkeypatch.setattr(
        page.editor._save_coordinator,
        "save",
        lambda session: calls.append(session),
    )

    if save_path == "shortcut":
        QTest.keyClick(
            page.editor,
            Qt.Key.Key_S,
            Qt.KeyboardModifier.ControlModifier,
        )
    else:
        page.editor_workspace.save_button.click()

    assert calls == []
    assert page.editor.session is None
    assert page.editor.status_text == "저장됨"


def test_empty_card_cleanup_switches_to_other_card_and_is_recoverable(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page = _page(qtbot, database, repositories)
    empty = _save_empty_connected_card(page, repositories)
    other = page.card_service.create_card(page.document_id, "다른 카드")

    assert page.open_card(other.id)

    deleted = repositories.get_card(empty.id)
    assert deleted is not None
    assert deleted.deleted_at_us is not None
    assert len(_delete_events(repositories, empty)) == 1
    assert page.editor.card_id == other.id
    restored = page.card_service.restore_card(empty.id)
    assert restored.deleted_at_us is None


@pytest.mark.parametrize(
    "close_path",
    ("escape", "empty-click", "cancel", "mouse-back"),
)
def test_empty_card_cleanup_covers_every_connected_close_path(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    close_path: str,
) -> None:
    window, page = _window(qtbot, database, repositories, tmp_path)
    empty = _save_empty_connected_card(page, repositories)

    if close_path == "escape":
        window.back_action.trigger()
    elif close_path == "empty-click":
        QTest.mouseClick(
            page.stream.viewport(),
            Qt.MouseButton.LeftButton,
            pos=_empty_point(page),
        )
    elif close_path == "cancel":
        page.editor_workspace.cancel_button.click()
    else:
        QTest.mouseClick(page.editor.viewport(), Qt.MouseButton.BackButton)
    qtbot.waitUntil(lambda: page.editor.session is None)

    deleted = repositories.get_card(empty.id)
    assert deleted is not None
    assert deleted.deleted_at_us is not None
    assert len(_delete_events(repositories, empty)) == 1
    assert page.stream.card_model.rowCount() == 0
    assert page.editor.toPlainText() == ""
    assert QApplication.focusWidget() is page.editor
    assert "0개 카드 · 0자 · 모든 변경 저장됨" in (
        window.statusBar().currentMessage()
    )


def test_app_driven_document_switch_cleans_empty_card(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    window, page = _window(qtbot, database, repositories, tmp_path)
    empty = _save_empty_connected_card(page, repositories)
    other = document_service.create_document(repositories, "전환할 문서")

    assert window.open_document_local(other.id, app_driven=True)

    deleted = repositories.get_card(empty.id)
    assert deleted is not None
    assert deleted.deleted_at_us is not None
    assert len(_delete_events(repositories, empty)) == 1


def test_quiet_detach_cleans_empty_card(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page = _page(qtbot, database, repositories)
    empty = _save_empty_connected_card(page, repositories)

    assert page.detach_editor_session_quietly()

    deleted = repositories.get_card(empty.id)
    assert deleted is not None
    assert deleted.deleted_at_us is not None
    assert len(_delete_events(repositories, empty)) == 1


def test_empty_card_cleanup_refreshes_consumers_once(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    monkeypatch: MonkeyPatch,
) -> None:
    page = _page(qtbot, database, repositories)
    empty = _save_empty_connected_card(page, repositories)
    content_changes: list[bool] = []
    history_refreshes: list[bool] = []
    real_history_refresh = page.history.refresh
    page.content_changed.connect(lambda: content_changes.append(True))

    def record_history_refresh() -> None:
        history_refreshes.append(True)
        real_history_refresh()

    monkeypatch.setattr(page.history, "refresh", record_history_refresh)

    assert page.editor.request_close()

    assert repositories.get_card(empty.id) is not None
    assert page.stream.card_model.rowCount() == 0
    assert content_changes == [True]
    assert history_refreshes == [True]
    assert page.editor.session is None
    assert page.editor.toPlainText() == ""


def test_first_input_delete_leave_keeps_capture_sequence_gap(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page = _page(qtbot, database, repositories)
    _paste(page.editor, "A")
    first_id = page.editor.card_id
    assert first_id is not None
    page.editor.selectAll()
    QTest.keyClick(page.editor, Qt.Key.Key_Backspace)

    assert page.editor.request_close()

    first = repositories.get_card(first_id)
    assert first is not None
    assert first.deleted_at_us is not None
    assert len(repositories.list_revisions(first.id)) == 2
    assert len(_delete_events(repositories, first)) == 1
    _paste(page.editor, "B")
    second_id = page.editor.card_id
    assert second_id is not None
    second = repositories.get_card(second_id)
    assert second is not None
    assert first.capture_seq == 1
    assert second.capture_seq == 2


def test_empty_cleanup_reads_committed_body_instead_of_widget(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page = _page(qtbot, database, repositories)
    _paste(page.editor, "DB에는 남는 본문")
    card_id = page.editor.card_id
    session = page.editor.session
    assert card_id is not None
    assert session is not None
    assert not session.dirty
    page.editor._loading = True
    try:
        page.editor.clear()
    finally:
        page.editor._loading = False
    assert page.editor.toPlainText() == ""
    assert not session.dirty

    assert page.editor.request_close()

    card = repositories.get_card(card_id)
    assert card is not None
    assert card.body == "DB에는 남는 본문"
    assert card.deleted_at_us is None
    assert _delete_events(repositories, card) == ()


def test_empty_cleanup_rejects_concurrent_nonempty_commit_before_delete(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    monkeypatch: MonkeyPatch,
) -> None:
    page = _page(qtbot, database, repositories)
    empty = _save_empty_connected_card(page, repositories)
    other = page.card_service.create_card(page.document_id, "전환 대상")
    real_soft_delete = page.card_service.soft_delete
    delete_calls: list[tuple[str | None, bool]] = []

    with Database(database.path) as competing_database:
        competing_repositories = Repositories(competing_database)

        def race_then_delete(
            card_id: str,
            *,
            expected_revision_id: str | None = None,
            require_empty_body: bool = False,
        ) -> Card:
            delete_calls.append((expected_revision_id, require_empty_body))
            competing_card = competing_repositories.get_card(card_id)
            assert competing_card is not None
            coordinator = DraftCoordinator(
                competing_database,
                competing_repositories,
            )
            competing_session = coordinator.open_card(competing_card)
            assert competing_session is not None
            coordinator.update_session(
                competing_session.draft_id,
                text="정리 직전 경쟁 저장",
                cursor_position_qchar=0,
            )
            result = SaveCoordinator(
                competing_database,
                coordinator,
                competing_repositories,
            ).save(competing_session)
            assert result.outcome is SaveOutcome.SAVED
            return real_soft_delete(
                card_id,
                expected_revision_id=expected_revision_id,
                require_empty_body=require_empty_body,
            )

        monkeypatch.setattr(page.card_service, "soft_delete", race_then_delete)
        assert page.open_card(other.id)

    assert delete_calls == [(empty.current_revision_id, True)]
    remaining = repositories.get_card(empty.id)
    assert remaining is not None
    assert remaining.body == "정리 직전 경쟁 저장"
    assert remaining.deleted_at_us is None
    assert _delete_events(repositories, empty) == ()
    assert page.editor.card_id == other.id


def test_empty_cleanup_skips_nonempty_history_ime_and_removed_cards(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page = _page(qtbot, database, repositories)

    _paste(page.editor, "비어 있지 않음")
    nonempty_id = page.editor.card_id
    assert nonempty_id is not None
    assert page.editor.request_close()
    nonempty = repositories.get_card(nonempty_id)
    assert nonempty is not None
    assert nonempty.deleted_at_us is None

    empty = _save_empty_connected_card(page, repositories)
    session = page.editor.session
    assert session is not None
    page.show_history()
    page.show_cards()
    assert page.editor.session is session
    assert _delete_events(repositories, empty) == ()

    QApplication.sendEvent(page.editor, QInputMethodEvent("ㅎ", []))
    assert not page.editor.request_close()
    assert repositories.get_card(empty.id) == empty
    assert _delete_events(repositories, empty) == ()
    QApplication.sendEvent(page.editor, QInputMethodEvent())

    page._delete_cards((empty.id,))
    removed = repositories.get_card(empty.id)
    assert removed is not None
    assert removed.deleted_at_us is not None
    assert len(_delete_events(repositories, empty)) == 1


@pytest.mark.parametrize("failure_kind", ("conflict_return", "exception"))
@pytest.mark.parametrize(
    "choice",
    (CloseChoice.DISCARD, CloseChoice.KEEP_EDITING),
    ids=("discard", "keep-editing"),
)
def test_dirty_save_failure_never_cleans_empty_card(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    monkeypatch: MonkeyPatch,
    failure_kind: str,
    choice: CloseChoice,
) -> None:
    page = _page(qtbot, database, repositories)
    empty = _save_empty_connected_card(page, repositories)
    draft_text = "저장 실패 뒤에도 보존할 본문"
    page.editor.setPlainText(draft_text)
    session = page.editor.session
    assert session is not None
    assert session.dirty
    save_calls: list[str] = []

    def fail_save(current_session: DraftSession) -> SaveResult:
        save_calls.append(current_session.draft_id)
        if failure_kind == "exception":
            raise RuntimeError("주입된 저장 예외")
        return SaveResult(
            outcome=SaveOutcome.CONFLICT,
            card=empty,
            conflict=SaveConflict(
                card_id=empty.id,
                base_revision_id=current_session.base_revision_id,
                current_revision_id="주입된-경쟁-리비전",
                base_text=empty.body,
                committed_text=empty.body,
                draft_text=current_session.text,
            ),
        )

    monkeypatch.setattr(page.editor._save_coordinator, "save", fail_save)
    monkeypatch.setattr(page.editor, "_ask_close_choice", lambda: choice)

    left_editor = page.editor.request_close()

    assert save_calls == [session.draft_id]
    remaining = repositories.get_card(empty.id)
    assert remaining is not None
    assert remaining.deleted_at_us is None
    assert remaining.body == ""
    assert _delete_events(repositories, empty) == ()
    if choice is CloseChoice.DISCARD:
        assert left_editor
        assert page.editor.session is None
    else:
        assert not left_editor
        assert page.editor.session is session
        assert session.dirty
        assert page.editor.card_id == empty.id
        assert page.editor.toPlainText() == draft_text


@pytest.mark.parametrize("failure_kind", ("false", "exception"))
@pytest.mark.parametrize("leave_path", ("switch", "close", "quiet-exit"))
def test_empty_cleanup_failure_never_blocks_leave(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    monkeypatch: MonkeyPatch,
    caplog: pytest.LogCaptureFixture,
    failure_kind: str,
    leave_path: str,
) -> None:
    page = _page(qtbot, database, repositories)
    empty = _save_empty_connected_card(page, repositories)
    other = page.card_service.create_card(page.document_id, "전환 대상")

    if failure_kind == "false":
        monkeypatch.setattr(
            page.card_service,
            "soft_delete",
            lambda _card_id, **_kwargs: False,
        )
    else:
        def raise_delete(_card_id: str, **_kwargs: object) -> Card:
            raise RuntimeError("주입된 정리 예외")

        monkeypatch.setattr(page.card_service, "soft_delete", raise_delete)

    with caplog.at_level("WARNING"):
        if leave_path == "switch":
            assert page.open_card(other.id)
            assert page.editor.card_id == other.id
        elif leave_path == "close":
            assert page.editor.request_close()
            assert page.editor.session is None
        else:
            assert page.detach_editor_session_quietly()
            assert page.editor.session is None

    remaining = repositories.get_card(empty.id)
    assert remaining is not None
    assert remaining.deleted_at_us is None
    assert _delete_events(repositories, empty) == ()
    assert "편집기 이탈을 계속합니다" in caplog.text


@pytest.mark.parametrize(
    "open_path",
    ("external", "click", "enter", "context"),
)
def test_card_open_focuses_single_editor(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    open_path: str,
) -> None:
    _window_widget, page = _window(qtbot, database, repositories, tmp_path)
    _paste(page.editor, "열 카드")
    assert page.editor.request_close()
    card = repositories.list_cards(page.document_id)[0]

    if open_path == "external":
        assert page.open_card(card.id)
    else:
        index = page.stream.card_model.index_for_card(card.id)
        assert index.isValid()
        page.stream.setCurrentIndex(index)
        page.stream.setFocus()
        if open_path == "click":
            QTest.mouseClick(
                page.stream.viewport(),
                Qt.MouseButton.LeftButton,
                pos=page.stream.visualRect(index).center(),
            )
        elif open_path == "enter":
            QTest.keyClick(page.stream, Qt.Key.Key_Return)
        else:
            menu = page.stream._build_context_menu(index)
            assert menu is not None
            menu.actions()[0].trigger()
    qtbot.waitUntil(lambda: QApplication.focusWidget() is page.editor)

    assert page.editor.session is not None
    assert page.editor.session.card_id == card.id
    assert QApplication.focusWidget() is page.editor


def test_empty_click_close_focuses_editor(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    _window_widget, page = _window(qtbot, database, repositories, tmp_path)
    _paste(page.editor, "닫을 카드")

    QTest.mouseClick(
        page.stream.viewport(),
        Qt.MouseButton.LeftButton,
        pos=_empty_point(page),
    )
    qtbot.waitUntil(lambda: QApplication.focusWidget() is page.editor)

    assert page.editor.session is None


def test_first_input_creation_keeps_existing_editor_focus(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    _window_widget, page = _window(qtbot, database, repositories, tmp_path)

    QTest.keyClicks(page.editor, "a")

    assert page.editor.card_id is not None
    assert QApplication.focusWidget() is page.editor


def test_close_then_paste_creates_next_card(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    _window_widget, page = _window(qtbot, database, repositories, tmp_path)
    _paste(page.editor, "첫 카드")
    QTest.mouseClick(
        page.stream.viewport(),
        Qt.MouseButton.LeftButton,
        pos=_empty_point(page),
    )
    qtbot.waitUntil(lambda: QApplication.focusWidget() is page.editor)

    _paste(page.editor, "둘째 카드")

    assert [card.body for card in repositories.list_cards(page.document_id)] == [
        "첫 카드",
        "둘째 카드",
    ]


@pytest.mark.parametrize(
    "close_path",
    ("cancel", "mouse-back", "escape-action", "alt-left-action"),
)
def test_connected_close_paths_focus_editor(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    close_path: str,
) -> None:
    window, page = _window(qtbot, database, repositories, tmp_path)
    _paste(page.editor, close_path)

    if close_path == "cancel":
        page.editor_workspace.cancel_button.click()
    elif close_path == "mouse-back":
        QTest.mouseClick(page.editor.viewport(), Qt.MouseButton.BackButton)
    elif close_path == "escape-action":
        window.back_action.trigger()
    else:
        for widget in QApplication.topLevelWidgets():
            if isinstance(widget, MainWindow) and widget is not window:
                widget.back_action.setEnabled(False)
        QTest.keyClick(
            page.editor,
            Qt.Key.Key_Left,
            Qt.KeyboardModifier.AltModifier,
        )
    qtbot.waitUntil(lambda: QApplication.focusWidget() is page.editor)

    assert page.editor.session is None


@pytest.mark.parametrize("list_path", ("escape-action", "card-list-action"))
def test_empty_surface_list_actions_focus_list(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    list_path: str,
) -> None:
    window, page = _window(qtbot, database, repositories, tmp_path)
    assert page.editor.session is None

    if list_path == "escape-action":
        window.back_action.trigger()
    else:
        window.card_list_action.trigger()

    assert QApplication.focusWidget() is page.stream


@pytest.mark.parametrize("noop_path", ("empty-click", "cancel", "mouse-back"))
def test_empty_surface_close_gestures_keep_editor_focus(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    noop_path: str,
) -> None:
    _window_widget, page = _window(qtbot, database, repositories, tmp_path)
    assert page.editor.session is None

    if noop_path == "empty-click":
        QTest.mouseClick(
            page.stream.viewport(),
            Qt.MouseButton.LeftButton,
            pos=_empty_point(page),
        )
    elif noop_path == "cancel":
        page.editor_workspace.cancel_button.click()
    else:
        QTest.mouseClick(page.editor.viewport(), Qt.MouseButton.BackButton)

    qtbot.waitUntil(lambda: QApplication.focusWidget() is page.editor)
    assert QApplication.focusWidget() is page.editor
    assert page.editor.session is None


def test_real_failure_modal_keep_editing_restores_focus(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    _window_widget, page = _window(qtbot, database, repositories, tmp_path)
    _paste(page.editor, "모달 원본")
    page.editor.setPlainText("저장 실패 본문")

    def fail_save(_session: object) -> object:
        raise RuntimeError("주입된 저장 실패")

    monkeypatch.setattr(page.editor._save_coordinator, "save", fail_save)
    modal_seen: list[bool] = []
    deadline = monotonic() + 2.0

    def click_keep() -> None:
        for widget in QApplication.allWidgets():
            if isinstance(widget, QMessageBox) and widget.isVisible():
                for button in widget.buttons():
                    if widget.buttonRole(button) is QMessageBox.ButtonRole.RejectRole:
                        modal_seen.append(True)
                        button.click()
                        return
        if monotonic() < deadline:
            QTimer.singleShot(5, click_keep)

    QTimer.singleShot(0, click_keep)
    try:
        QTest.mouseClick(
            page.stream.viewport(),
            Qt.MouseButton.LeftButton,
            pos=_empty_point(page),
        )
        qtbot.wait(100)

        assert modal_seen == [True]
        assert page.editor.session is not None
        assert page.focusWidget() is page.editor
    finally:
        if page.editor.session is not None:
            assert page.editor.request_close(
                choice_provider=lambda _session: CloseChoice.DISCARD
            )


def test_search_result_activation_hides_dialog_and_focuses_editor(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    window, page = _window(qtbot, database, repositories, tmp_path)
    _paste(page.editor, "검색할 본문")
    assert page.editor.request_close()
    card = repositories.list_cards(page.document_id)[0]
    window.search_dialog.focus_search()
    window.search_dialog.search("검색할")
    item = window.search_dialog.result_tree.topLevelItem(0)
    assert item is not None

    window.search_dialog._activate(item)
    qtbot.waitUntil(lambda: QApplication.focusWidget() is page.editor)

    assert window.search_dialog.isHidden()
    assert page.editor.card_id == card.id
    assert QApplication.focusWidget() is page.editor


@pytest.mark.parametrize(
    "leave_path",
    (
        "escape",
        "empty-click",
        "cancel",
        "mouse-back",
        "other-card",
        "other-document",
    ),
)
def test_clean_connected_preedit_blocks_every_leave_entrypoint(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
    leave_path: str,
) -> None:
    window, page = _window(qtbot, database, repositories, tmp_path)
    _paste(page.editor, "clean IME 카드")
    session = page.editor.session
    card_id = page.editor.card_id
    assert session is not None
    assert card_id is not None
    assert not session.dirty
    revision_ids = tuple(
        revision.id for revision in repositories.list_revisions(card_id)
    )
    monkeypatch.setattr(
        page.editor,
        "_ask_close_choice",
        lambda: (_ for _ in ()).throw(
            AssertionError("IME 즉시 거부 경로에서 이탈 대화를 열었습니다.")
        ),
    )
    QApplication.sendEvent(page.editor, QInputMethodEvent("ㅎ", []))
    assert page.draft_coordinator.is_ime_composing(session.draft_id)
    assert not session.dirty

    if leave_path == "escape":
        window.back_action.trigger()
    elif leave_path == "empty-click":
        QTest.mouseClick(
            page.stream.viewport(),
            Qt.MouseButton.LeftButton,
            pos=_empty_point(page),
        )
    elif leave_path == "cancel":
        page.editor_workspace.cancel_button.click()
    elif leave_path == "mouse-back":
        QTest.mouseClick(page.editor.viewport(), Qt.MouseButton.BackButton)
    elif leave_path == "other-card":
        other_card = page.card_service.create_cards(
            page.document_id,
            "다른 카드",
            source=CaptureOperationSource.TYPING,
            split=False,
        )[0]
        assert not page.open_card(other_card.id)
    else:
        other_document = document_service.create_document(
            repositories,
            "다른 문서",
        )
        assert not window.open_document_local(other_document.id)
        assert window.active_document_id == page.document_id

    assert page.editor.session is session
    assert page.editor.card_id == card_id
    assert page.draft_coordinator.is_ime_composing(session.draft_id)
    assert tuple(
        revision.id for revision in repositories.list_revisions(card_id)
    ) == revision_ids
    QApplication.sendEvent(page.editor, QInputMethodEvent())
    assert not page.draft_coordinator.is_ime_composing(session.draft_id)


def test_first_event_source_does_not_become_mixed(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page = _page(qtbot, database, repositories)
    QTest.keyClicks(page.editor, "a")
    _paste(page.editor, "b")

    card = repositories.list_cards(page.document_id)[0]
    assert card.source is CardSource.TYPING
    assert page.editor.toPlainText() == "ab"


@pytest.mark.parametrize(
    ("first_kind", "expected_source"),
    (
        pytest.param("paste", CardSource.TYPING, id="failed-paste-then-typing"),
        pytest.param("typing", CardSource.PASTE, id="failed-typing-then-paste"),
    ),
)
def test_source_flag_is_scoped_after_failed_insertion(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    monkeypatch: MonkeyPatch,
    first_kind: str,
    expected_source: CardSource,
) -> None:
    page = _page(qtbot, database, repositories)
    real_create = page.card_service.create_cards
    sources: list[CaptureOperationSource] = []

    def fail_once(
        document_id: str,
        text: str,
        *,
        source: CaptureOperationSource,
        split: bool,
    ) -> tuple[Card, ...]:
        sources.append(source)
        if len(sources) == 1:
            return ()
        return real_create(document_id, text, source=source, split=split)

    monkeypatch.setattr(page.card_service, "create_cards", fail_once)
    if first_kind == "paste":
        _paste(page.editor, "p")
        QTest.keyClicks(page.editor, "t")
        expected_operation = CaptureOperationSource.TYPING
        expected_event = EventSource.TYPING
    else:
        QTest.keyClicks(page.editor, "t")
        _paste(page.editor, "p")
        expected_operation = CaptureOperationSource.PASTE
        expected_event = EventSource.PASTE

    card = repositories.list_cards(page.document_id)[0]
    _assert_creation_sources(
        repositories,
        card,
        card_source=expected_source,
        operation_source=expected_operation,
        event_source=expected_event,
    )
    assert page.editor._input_source is None


def test_ctrl_enter_is_plain_line_break(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page = _page(qtbot, database, repositories)
    QTest.keyClicks(page.editor, "a")

    QTest.keyClick(
        page.editor,
        Qt.Key.Key_Return,
        Qt.KeyboardModifier.ControlModifier,
    )

    assert page.editor.toPlainText() == "a\n"
    assert len(repositories.list_cards(page.document_id)) == 1


def test_successful_creation_discards_new_backing(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page = _page(qtbot, database, repositories)
    _paste(page.editor, "성공 본문")

    assert page.editor.session is not None
    assert all(
        draft.draft_kind is not DraftKind.NEW
        for draft in repositories.list_drafts(page.document_id)
    )


def test_new_backing_is_reopened_after_connected_card_close(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    monkeypatch: MonkeyPatch,
) -> None:
    page = _page(qtbot, database, repositories)
    first_new = page.editor._new_session
    assert first_new is not None
    _paste(page.editor, "연결 카드")
    assert page.editor.request_close()

    reopened = page.editor._new_session
    assert reopened is not None
    assert reopened.draft_id != first_new.draft_id
    monkeypatch.setattr(page.card_service, "create_cards", lambda *_a, **_k: ())
    _paste(page.editor, "실패 뒤 보호")
    assert page.protect_now()

    restored = DocumentPage(database, repositories, page.document_id)
    qtbot.addWidget(restored)
    assert restored.editor.toPlainText() == "실패 뒤 보호"
    assert restored.editor.card_id is None


def test_connected_to_empty_refuses_when_new_backing_open_fails(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    monkeypatch: MonkeyPatch,
) -> None:
    page = _page(qtbot, database, repositories)
    _paste(page.editor, "연결 유지")
    card_id = page.editor.card_id
    assert card_id is not None

    def fail_open_new(_document_id: str) -> DraftSession:
        raise RuntimeError("주입된 NEW 확보 실패")

    monkeypatch.setattr(page.draft_coordinator, "open_new", fail_open_new)

    assert not page.editor.request_close()
    assert page.editor.card_id == card_id
    assert page.editor.toPlainText() == "연결 유지"


def test_new_draft_discard_failure_preserves_bounded_duplicate_contract(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    monkeypatch: MonkeyPatch,
) -> None:
    page = _page(qtbot, database, repositories)
    _paste(page.editor, " ")
    assert page.protect_now()
    new_session = page.editor._new_session
    assert new_session is not None
    stale_id = new_session.draft_id
    real_discard = page.draft_coordinator.discard_session

    def fail_exact_discard(draft_id: str) -> None:
        if draft_id == stale_id:
            raise RuntimeError("주입된 NEW 폐기 실패")
        real_discard(draft_id)

    monkeypatch.setattr(
        page.draft_coordinator,
        "discard_session",
        fail_exact_discard,
    )
    QTest.keyClicks(page.editor, "A")

    first = repositories.list_cards(page.document_id)
    assert len(first) == 1
    assert first[0].body == " A"
    stale = repositories.get_draft(stale_id)
    assert stale is not None
    assert stale.draft_text == " "
    assert page.editor.card_id == first[0].id

    restored = DocumentPage(database, repositories, page.document_id)
    qtbot.addWidget(restored)
    assert restored.editor.toPlainText() == " "
    QTest.keyClicks(restored.editor, "A")

    cards = repositories.list_cards(page.document_id)
    assert len(cards) == 2
    assert [card.body for card in cards] == [" A", " A"]


def test_multiple_new_drafts_restore_latest_and_preserve_others(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    caplog: pytest.LogCaptureFixture,
) -> None:
    document = document_service.create_document(repositories, "multiple NEW")
    for draft_id, body, updated_at_us in (
        ("new-a", "older", 1_000),
        ("new-z", "same-time-winner", 2_000),
        ("new-b", "same-time-loser", 2_000),
    ):
        repositories.create_draft(
            Draft(
                id=draft_id,
                document_id=document.id,
                card_id=None,
                draft_kind=DraftKind.NEW,
                base_revision_id=None,
                draft_text=body,
                draft_hash=text_hash(body),
                cursor_position_qchar=_utf16_units(body),
                updated_at_us=updated_at_us,
            )
        )

    with caplog.at_level("WARNING"):
        page = DocumentPage(database, repositories, document.id)
    qtbot.addWidget(page)

    assert page.editor.toPlainText() == "same-time-winner"
    assert {draft.id for draft in repositories.list_drafts(document.id)} == {
        "new-a",
        "new-z",
        "new-b",
    }
    assert page.draft_coordinator.recovery_candidates(document.id) == ()
    assert sum("new recovery draft가 여러 건" in record.message for record in caplog.records) == 1


@pytest.mark.parametrize("failure_kind", ("false", "exception"))
def test_empty_surface_leave_blocks_on_protect_false_and_exception(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    monkeypatch: MonkeyPatch,
    failure_kind: str,
) -> None:
    page = _page(qtbot, database, repositories)
    page.editor.setPlainText("이탈 거부 본문")

    if failure_kind == "false":
        monkeypatch.setattr(page.editor, "_protect_empty_surface", lambda: False)
    else:
        def raise_protection() -> bool:
            raise RuntimeError("주입된 보호 예외")

        monkeypatch.setattr(
            page.editor,
            "_protect_empty_surface",
            raise_protection,
        )

    assert not page.can_leave_editor()
    assert page.editor.card_id is None
    assert page.editor.toPlainText() == "이탈 거부 본문"


def test_internal_text_move_creates_once_from_drop_insert_phase(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page = _page(qtbot, database, repositories)
    page.editor.setPlainText("ABC")
    cursor = page.editor.textCursor()
    cursor.setPosition(0)
    cursor.setPosition(1, cursor.MoveMode.KeepAnchor)
    cursor.removeSelectedText()
    assert page.editor.toPlainText() == "BC"
    assert repositories.list_cards(page.document_id) == ()

    cursor.movePosition(cursor.MoveOperation.End)
    page.editor.setTextCursor(cursor)
    mime = QMimeData()
    mime.setText("A")
    drop = QDropEvent(
        QPointF(page.editor.cursorRect().center()),
        Qt.DropAction.MoveAction,
        mime,
        Qt.MouseButton.LeftButton,
        Qt.KeyboardModifier.NoModifier,
    )
    page.editor.dropEvent(drop)

    cards = repositories.list_cards(page.document_id)
    assert len(cards) == 1
    assert cards[0].body == page.editor.toPlainText()
    _assert_creation_sources(
        repositories,
        cards[0],
        card_source=CardSource.PASTE,
        operation_source=CaptureOperationSource.PASTE,
        event_source=EventSource.PASTE,
    )


def test_file_url_with_companion_text_imports_file_without_inserting_path(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    page = _page(qtbot, database, repositories)
    path = tmp_path / "note.pdf"
    path.write_text("확장자와 무관한 파일 본문", encoding="utf-8")
    companion = str(path)
    mime = QMimeData()
    mime.setUrls([QUrl.fromLocalFile(companion)])
    mime.setText(companion)
    drag_enter = QDragEnterEvent(
        page.editor.cursorRect().center(),
        Qt.DropAction.CopyAction,
        mime,
        Qt.MouseButton.LeftButton,
        Qt.KeyboardModifier.NoModifier,
    )

    QApplication.sendEvent(page.editor.viewport(), drag_enter)
    drop = QDropEvent(
        QPointF(page.editor.cursorRect().center()),
        Qt.DropAction.CopyAction,
        mime,
        Qt.MouseButton.LeftButton,
        Qt.KeyboardModifier.NoModifier,
    )
    QApplication.sendEvent(page.editor.viewport(), drop)

    assert drag_enter.isAccepted()
    assert drop.isAccepted()
    cards = repositories.list_cards(page.document_id)
    assert len(cards) == 1
    assert cards[0].body == "확장자와 무관한 파일 본문"
    assert companion not in cards[0].body
    assert page.editor.toPlainText() == cards[0].body
    assert page.editor.card_id == cards[0].id


def test_url_only_mime_paste_does_not_insert_text(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    page = _page(qtbot, database, repositories)
    mime = QMimeData()
    mime.setUrls([QUrl.fromLocalFile(str(tmp_path / "note.md"))])
    assert not mime.hasFormat("text/plain")

    page.editor.insertFromMimeData(mime)

    assert page.editor.toPlainText() == ""
    assert page.editor.card_id is None
    assert repositories.list_cards(page.document_id) == ()
    assert repositories.list_events(page.document_id) == ()


def test_creation_updates_all_consumers_once(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    published: list[str] = []
    settings = QSettings(
        str(tmp_path / "consumer-updates.ini"),
        QSettings.Format.IniFormat,
    )
    initialize_device_settings(settings)
    settings.setValue("first_run/guide_shown", True)
    window = MainWindow(
        repositories,
        SqliteWorkspaceStateStore(database),
        settings=settings,
        document_change_publisher=published.append,
    )
    qtbot.addWidget(window)
    document = document_service.create_document(repositories, "consumers")
    assert window.open_document_local(document.id)
    page = window.active_document_page()
    assert page is not None
    created: list[Card] = []
    connected: list[str] = []
    content_changes: list[bool] = []
    opened: list[str] = []
    history_refreshes: list[bool] = []
    history_cards: list[str] = []
    page.editor.card_created.connect(created.append)
    page.editor.card_connected.connect(connected.append)
    page.content_changed.connect(lambda: content_changes.append(True))
    page.card_opened.connect(opened.append)
    monkeypatch.setattr(
        page.history,
        "refresh",
        lambda: history_refreshes.append(True),
    )
    monkeypatch.setattr(
        page.history,
        "set_card",
        history_cards.append,
    )
    # 카드 화면에서는 숨은 이력 갱신이 대상 기록으로 미뤄진다 — 어느 경로든
    # 소비자 갱신은 그 카드 한 번이어야 한다는 계약은 같다.
    monkeypatch.setattr(
        page.history,
        "set_pending_card",
        history_cards.append,
    )

    _paste(page.editor, "소비자 갱신")

    card = repositories.list_cards(document.id)[0]
    assert [value.id for value in created] == [card.id]
    assert connected == [card.id]
    assert content_changes == [True]
    assert opened == [card.id]
    assert history_refreshes == [True]
    assert history_cards == [card.id]
    assert published == [document.id]
    assert page.stream.card_model.index_for_card(card.id).isValid()
    assert "1개 카드" in window.statusBar().currentMessage()


def test_single_surface_structure_and_state_contract(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page = _page(qtbot, database, repositories)
    workspace = page.editor_workspace
    workspace.show()
    qtbot.wait(20)

    assert workspace.findChild(QStackedWidget, "cardEditorInputStack") is None
    assert workspace._splitter.indexOf(workspace._editor_shell) == 1
    assert page.editor.parentWidget() is workspace._editor_shell
    assert not page.editor.isReadOnly()
    assert page.editor.card_id is None
    assert not workspace.save_button.isEnabled()
    assert page.editor.placeholderText()
    assert workspace.status_label.text() == "새 카드를 입력하세요."
    assert not workspace._editor_shell.isHidden()

    QTest.keyClicks(page.editor, "x")

    assert page.editor.card_id is not None
    assert workspace.save_button.isEnabled()
    assert workspace.status_label.text() == "저장됨"
    assert not workspace._editor_shell.isHidden()


def test_reopening_connected_card_has_bounded_signal_side_effects(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    _window_widget, page = _window(qtbot, database, repositories, tmp_path)
    _paste(page.editor, "이미 연결된 카드")
    card_id = page.editor.card_id
    assert card_id is not None
    index = page.stream.card_model.index_for_card(card_id)
    assert index.isValid()
    assert page.stream.currentIndex() == index
    revision_ids = tuple(
        revision.id for revision in repositories.list_revisions(card_id)
    )
    scroll_anchor = page.stream.verticalScrollBar().value()
    opened: list[str] = []
    content_changes: list[bool] = []
    page.card_opened.connect(opened.append)
    page.content_changed.connect(lambda: content_changes.append(True))

    page.stream.setFocus()
    qtbot.waitUntil(lambda: QApplication.focusWidget() is page.stream)
    QTest.keyClick(page.stream, Qt.Key.Key_Return)
    qtbot.waitUntil(lambda: QApplication.focusWidget() is page.editor)

    assert page.editor.card_id == card_id
    assert QApplication.focusWidget() is page.editor
    assert opened == [card_id]
    assert content_changes == []
    assert tuple(
        revision.id for revision in repositories.list_revisions(card_id)
    ) == revision_ids
    assert page.stream.currentIndex() == index
    assert page.stream.verticalScrollBar().value() == scroll_anchor


def test_empty_surface_empty_click_creates_no_card(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    _window_widget, page = _window(qtbot, database, repositories, tmp_path)
    assert page.editor.card_id is None

    QTest.mouseClick(
        page.stream.viewport(),
        Qt.MouseButton.LeftButton,
        pos=_empty_point(page),
    )
    qtbot.waitUntil(lambda: QApplication.focusWidget() is page.editor)

    assert page.editor.card_id is None
    assert repositories.list_cards(page.document_id) == ()
