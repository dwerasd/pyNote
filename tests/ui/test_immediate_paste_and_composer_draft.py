from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QMimeData, QPoint, QSettings, Qt
from PySide6.QtGui import QInputMethodEvent
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QMessageBox
from pytest import MonkeyPatch
from pytestqt.qtbot import QtBot

from pynote.app import SqliteWorkspaceStateStore, initialize_device_settings
from pynote.application import document_service
from pynote.domain.models import CardSource, DraftKind
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories
from pynote.ui.document_page import DocumentPage
from pynote.ui.main_window import MainWindow


def _window(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> tuple[MainWindow, QSettings, DocumentPage]:
    settings = QSettings(
        str(tmp_path / "single-surface.ini"),
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
    document = document_service.create_document(repositories, "단일 편집면")
    assert window.open_document_local(document.id)
    page = window.active_document_page()
    assert page is not None
    return window, settings, page


def _paste(page: DocumentPage, text: str) -> None:
    source = QMimeData()
    source.setText(text)
    page.editor.insertFromMimeData(source)


def _empty_stream_point(page: DocumentPage) -> QPoint:
    point = page.stream.viewport().rect().bottomRight() - QPoint(10, 10)
    assert not page.stream.indexAt(point).isValid()
    return point


def test_empty_click_auto_saves_and_returns_focus_to_editor(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    window, _settings, page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )
    window.activateWindow()
    page.editor.setFocus()
    _paste(page, "자동저장 원본")
    session = page.editor.session
    assert session is not None
    card_id = session.card_id
    assert card_id is not None
    page.editor.setPlainText("빈 공간 클릭으로 저장할 본문")

    QTest.mouseClick(
        page.stream.viewport(),
        Qt.MouseButton.LeftButton,
        pos=_empty_stream_point(page),
    )
    qtbot.waitUntil(lambda: QApplication.focusWidget() is page.editor)

    stored = repositories.get_card(card_id)
    assert stored is not None
    assert stored.body == "빈 공간 클릭으로 저장할 본문"
    assert page.editor.session is None


def test_empty_click_without_session_is_handler_noop(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    _window_widget, _settings, page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )

    def fail_request_close() -> bool:
        raise AssertionError("빈 편집면에서 닫기 게이트를 호출했습니다.")

    monkeypatch.setattr(page.editor, "request_close", fail_request_close)
    QTest.mouseClick(
        page.stream.viewport(),
        Qt.MouseButton.LeftButton,
        pos=_empty_stream_point(page),
    )

    assert page.editor.session is None
    assert repositories.list_cards(page.document_id) == ()


def test_consecutive_pastes_create_separate_operations(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    _window_widget, _settings, page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )
    for body in ("첫 수집", "둘째 수집", "셋째 수집"):
        _paste(page, body)
        if body != "셋째 수집":
            assert page.editor.request_close()

    cards = repositories.list_cards(page.document_id)
    assert [card.body for card in cards] == ["첫 수집", "둘째 수집", "셋째 수집"]
    assert {card.source for card in cards} == {CardSource.PASTE}
    assert len({card.operation_id for card in cards}) == 3


def test_new_cards_append_position_and_newest_appears_at_recency_top(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    _window_widget, _settings, page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )
    QTest.keyClicks(page.editor, "first")
    assert page.editor.request_close()
    QTest.keyClicks(page.editor, "second")
    assert page.editor.request_close()

    stored = repositories.list_cards(page.document_id)
    displayed = tuple(
        card
        for row in range(page.stream.card_model.rowCount())
        if (
            card := page.stream.card_model.card_at(
                page.stream.card_model.index(row, 0)
            )
        )
        is not None
    )
    assert [card.body for card in stored] == ["first", "second"]
    assert [card.body for card in displayed] == ["second", "first"]


def test_filtered_first_paste_connects_without_visible_row(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    _window_widget, _settings, page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )
    page.source_filter.setCurrentIndex(page.source_filter.findData("typing"))
    _paste(page, "필터 밖 붙여넣기")

    card = repositories.list_cards(page.document_id)[0]
    assert card.source is CardSource.PASTE
    assert page.editor.session is not None
    assert page.editor.session.card_id == card.id
    assert not page.stream.card_model.index_for_card(card.id).isValid()


def test_failed_creation_new_draft_restores_without_recovery_dialog(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    window, settings, page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )
    create_calls: list[bool] = []

    def fail_creation(*_args: object, **_kwargs: object) -> tuple[object, ...]:
        create_calls.append(True)
        return ()

    monkeypatch.setattr(page.card_service, "create_cards", fail_creation)
    body = "생성 실패 뒤 복원할 NEW 초안 😀"
    _paste(page, body)
    assert create_calls == [True]
    assert page.protect_now()
    assert repositories.list_cards(page.document_id) == ()
    window.hide()

    restarted = MainWindow(
        repositories,
        SqliteWorkspaceStateStore(database),
        settings=settings,
        recovery_choice_provider=lambda _candidate: (_ for _ in ()).throw(
            AssertionError("NEW draft에 카드 복구 대화가 열렸습니다.")
        ),
    )
    qtbot.addWidget(restarted)
    restored = restarted.active_document_page()
    assert restored is not None
    assert restored.editor.toPlainText() == body
    assert restored.editor.textCursor().position() == (
        len(body.encode("utf-16-le")) // 2
    )
    assert restored.editor.session is None
    assert repositories.list_cards(page.document_id) == ()


def test_window_close_protects_unconnected_text_without_confirmation(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    window, _settings, page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )
    create_calls: list[bool] = []

    def fail_creation(*_args: object, **_kwargs: object) -> tuple[object, ...]:
        create_calls.append(True)
        return ()

    monkeypatch.setattr(page.card_service, "create_cards", fail_creation)
    _paste(page, "카드 없는 보호 본문")
    assert create_calls == [True]

    assert window.close()
    assert any(
        draft.draft_kind is DraftKind.NEW
        and draft.draft_text == "카드 없는 보호 본문"
        for draft in repositories.list_drafts(page.document_id)
    )


def test_preedit_only_empty_surface_refuses_leave_until_composition_ends(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    _window_widget, _settings, page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )
    QApplication.sendEvent(page.editor, QInputMethodEvent("ㅎ", []))

    assert not page.can_leave_editor()
    assert repositories.list_cards(page.document_id) == ()

    QApplication.sendEvent(page.editor, QInputMethodEvent())
    assert page.can_leave_editor()


def test_preedit_replacing_selection_keeps_new_session_and_stored_draft(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    _window_widget, _settings, page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )
    create_calls: list[bool] = []

    def fail_creation(*_args: object, **_kwargs: object) -> tuple[object, ...]:
        create_calls.append(True)
        return ()

    monkeypatch.setattr(page.card_service, "create_cards", fail_creation)
    _paste(page, "기존 NEW 초안")
    assert create_calls == [True]
    assert page.protect_now()
    session = page.editor._new_session
    assert session is not None
    draft_id = session.draft_id

    QApplication.sendEvent(page.editor, QInputMethodEvent("ㅎ", []))
    page.editor.clear()

    assert page.editor._new_session is not None
    assert page.editor._new_session.draft_id == draft_id
    assert repositories.get_draft(draft_id) is not None
    assert not page.can_leave_editor()

    QApplication.sendEvent(page.editor, QInputMethodEvent())


def test_connected_editor_protect_now_is_refused_while_ime_composing(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    _window_widget, _settings, page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )
    _paste(page, "편집할 카드")
    session = page.editor.session
    assert session is not None
    assert not session.dirty
    QApplication.sendEvent(page.editor, QInputMethodEvent("ㅎ", []))

    assert not page.editor.protect_now()
    assert page.draft_coordinator.is_ime_composing(session.draft_id)

    QApplication.sendEvent(page.editor, QInputMethodEvent())
    assert page.editor.protect_now()
    assert not page.draft_coordinator.is_ime_composing(session.draft_id)


def test_ime_commit_then_empty_click_clears_composing_and_runs_gate(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    _window_widget, _settings, page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )
    _paste(page, "IME 전환을 확인할 카드")
    session = page.editor.session
    assert session is not None
    card_id = session.card_id
    assert card_id is not None
    revision_count = len(repositories.list_revisions(card_id))

    QApplication.sendEvent(page.editor, QInputMethodEvent("ㅎ", []))
    assert page.draft_coordinator.is_ime_composing(session.draft_id)
    point = _empty_stream_point(page)
    QTest.mousePress(
        page.stream.viewport(),
        Qt.MouseButton.LeftButton,
        pos=point,
    )
    commit = QInputMethodEvent()
    commit.setCommitString("한")
    QApplication.sendEvent(page.editor, commit)

    committed_body = "IME 전환을 확인할 카드한"
    assert not page.draft_coordinator.is_ime_composing(session.draft_id)
    assert page.editor.toPlainText() == committed_body
    assert session.dirty
    QTest.mouseRelease(
        page.stream.viewport(),
        Qt.MouseButton.LeftButton,
        pos=point,
    )
    qtbot.waitUntil(lambda: QApplication.focusWidget() is page.editor)

    stored = repositories.get_card(card_id)
    assert stored is not None
    assert stored.body == committed_body
    assert len(repositories.list_revisions(card_id)) == revision_count + 1
    assert page.editor.session is None
    assert page.editor.toPlainText() == ""
    assert QApplication.focusWidget() is page.editor


def test_window_close_is_refused_while_connected_ime_composing(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    window, _settings, page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )
    _paste(page, "창 종료 IME 카드")
    session = page.editor.session
    assert session is not None
    assert not session.dirty
    failures: list[str] = []
    monkeypatch.setattr(
        QMessageBox,
        "critical",
        lambda *_args, **_kwargs: failures.append("critical")
        or QMessageBox.StandardButton.Ok,
    )

    QApplication.sendEvent(page.editor, QInputMethodEvent("ㅎ", []))

    assert not window.close()
    assert failures == ["critical"]
    assert page.editor.session is session
    assert page.draft_coordinator.is_ime_composing(session.draft_id)

    QApplication.sendEvent(page.editor, QInputMethodEvent())
    assert window.close()


def test_typing_first_event_fixes_source_even_after_paste(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    _window_widget, _settings, page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )
    QTest.keyClicks(page.editor, "a")
    _paste(page, "b")

    card = repositories.list_cards(page.document_id)[0]
    assert card.source is CardSource.TYPING
    assert page.editor.toPlainText() == "ab"
