from __future__ import annotations

from collections.abc import Iterator

from PySide6.QtCore import QItemSelectionModel, QModelIndex, QPoint, Qt
from PySide6.QtGui import QAction, QContextMenuEvent
from PySide6.QtTest import QTest
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QMenu,
    QMessageBox,
    QPushButton,
)
from pytest import MonkeyPatch, raises
from pytestqt.qtbot import QtBot

from pynote.application.card_service import CardService
from pynote.domain.models import Card, CardSource, Document
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories
from pynote.ui.cards.card_model import CardListModel
from pynote.ui.cards.card_stream import CardStreamView
from pynote.ui.document_page import DocumentPage


def _ids() -> Iterator[str]:
    number = 0
    while True:
        number += 1
        yield f"context-{number}"


def _create_cards(
    database: Database,
    repositories: Repositories,
    bodies: tuple[str, ...],
) -> tuple[Card, ...]:
    document = Document(
        id="context-document",
        title="컨텍스트 메뉴",
        created_at_us=1_000,
        updated_at_us=1_000,
    )
    repositories.create_document(document)
    identifiers = _ids()
    service = CardService(
        database,
        repositories,
        clock=lambda: 2_000,
        id_factory=lambda: next(identifiers),
    )
    return tuple(service.create_card(document.id, body) for body in bodies)


def _menu_actions(view: CardStreamView, row: int) -> dict[str, QAction]:
    menu = view._build_context_menu(view.card_model.index(row))
    assert menu is not None
    return {action.text(): action for action in menu.actions()}


def test_card_context_menu_has_required_actions_and_selection_conditions(
    qtbot: QtBot,
) -> None:
    model = CardListModel(
        [
            Card(
                id=f"card-{number}",
                document_id="document",
                operation_id=f"operation-{number}",
                position_key=number * 1_024,
                capture_seq=number,
                created_at_us=1_000,
                updated_at_us=1_000,
                source=CardSource.TYPING,
                body=f"본문 {number}",
                body_hash="hash",
                current_revision_id=f"revision-{number}",
            )
            for number in (1, 2)
        ]
    )
    view = CardStreamView(model)
    view.set_sort_mode("position")
    qtbot.addWidget(view)

    single_actions = _menu_actions(view, 0)

    assert list(single_actions) == [
        "편집기에서 열기",
        "본문 복사",
        "파일로 내보내기",
        "삭제",
    ]

    selection_model = view.selectionModel()
    assert selection_model is not None
    selection_model.select(
        view.card_model.index(1),
        QItemSelectionModel.SelectionFlag.Select
        | QItemSelectionModel.SelectionFlag.Rows,
    )
    view.resize(500, 400)
    view.show()
    qtbot.wait(20)
    QTest.mousePress(
        view.viewport(),
        Qt.MouseButton.RightButton,
        pos=view.visualRect(view.card_model.index(0)).center(),
    )
    QTest.mouseRelease(
        view.viewport(),
        Qt.MouseButton.RightButton,
        pos=view.visualRect(view.card_model.index(0)).center(),
    )

    assert view.selected_card_ids() == ("card-1", "card-2")

    assert list(_menu_actions(view, 0)) == [
        "편집기에서 열기",
        "본문 복사",
        "파일로 내보내기",
        "삭제",
    ]
    assert view._build_context_menu(QModelIndex()) is None


def test_card_context_menu_reexposes_open_copy_and_delete(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    first, second = _create_cards(database, repositories, ("첫 본문", "둘째 본문"))
    view = CardStreamView(CardListModel((first, second)))
    view.set_sort_mode("position")
    qtbot.addWidget(view)
    opened: list[str] = []
    deleted: list[tuple[str, ...]] = []
    view.card_open_requested.connect(opened.append)
    view.cards_delete_requested.connect(deleted.append)
    QApplication.clipboard().clear()

    single_actions = _menu_actions(view, 0)
    single_actions["편집기에서 열기"].trigger()
    single_actions["본문 복사"].trigger()
    single_actions["삭제"].trigger()

    assert opened == [first.id]
    assert QApplication.clipboard().text() == first.body
    assert deleted == [(first.id,)]


def test_right_click_opens_menu_only_over_a_card(
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
) -> None:
    model = CardListModel(
        [
            Card(
                id="card",
                document_id="document",
                operation_id="operation",
                position_key=1_024,
                capture_seq=1,
                created_at_us=1_000,
                updated_at_us=1_000,
                source=CardSource.TYPING,
                body="우클릭할 카드",
                body_hash="hash",
                current_revision_id="revision",
            )
        ]
    )
    view = CardStreamView(model)
    qtbot.addWidget(view)
    view.resize(500, 400)
    view.show()
    qtbot.wait(20)
    shown: list[QMenu] = []
    monkeypatch.setattr(
        view,
        "_execute_context_menu",
        lambda menu, _position: shown.append(menu),
    )
    card_position = view.visualRect(model.index(0)).center()

    QApplication.sendEvent(
        view.viewport(),
        QContextMenuEvent(
            QContextMenuEvent.Reason.Mouse,
            card_position,
            view.viewport().mapToGlobal(card_position),
        ),
    )

    assert len(shown) == 1
    assert [action.text() for action in shown[0].actions()] == [
        "편집기에서 열기",
        "본문 복사",
        "파일로 내보내기",
        "삭제",
    ]

    empty_position = QPoint(
        view.viewport().width() // 2,
        view.viewport().height() - 4,
    )
    QApplication.sendEvent(
        view.viewport(),
        QContextMenuEvent(
            QContextMenuEvent.Reason.Mouse,
            empty_position,
            view.viewport().mapToGlobal(empty_position),
        ),
    )

    assert len(shown) == 1


def test_context_delete_soft_deletes_without_confirmation_and_can_restore(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    monkeypatch: MonkeyPatch,
) -> None:
    first, second = _create_cards(database, repositories, ("첫 카드", "둘째 카드"))
    page = DocumentPage(database, repositories, first.document_id)
    qtbot.addWidget(page)
    monkeypatch.setattr(
        QMessageBox,
        "question",
        lambda *args, **kwargs: (_ for _ in ()).throw(
            AssertionError("soft-delete 확인 대화상자를 열면 안 됩니다.")
        ),
    )

    actions = _menu_actions(page.stream, 0)
    actions["삭제"].trigger()

    deleted = repositories.get_card(second.id)
    assert deleted is not None
    assert deleted.deleted_at_us is not None
    assert page.stream.card_model.rowCount() == 1

    page.card_service.restore_card(second.id)
    page.refresh()

    restored = repositories.get_card(second.id)
    assert restored is not None
    assert restored.deleted_at_us is None
    assert page.stream.card_model.rowCount() == 2
    assert repositories.get_card(first.id) is not None


def test_context_delete_applies_to_entire_multiple_selection(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    monkeypatch: MonkeyPatch,
) -> None:
    first, second = _create_cards(database, repositories, ("첫 카드", "둘째 카드"))
    page = DocumentPage(database, repositories, first.document_id)
    qtbot.addWidget(page)
    selection_model = page.stream.selectionModel()
    assert selection_model is not None
    for row in range(2):
        selection_model.select(
            page.stream.card_model.index(row),
            QItemSelectionModel.SelectionFlag.Select
            | QItemSelectionModel.SelectionFlag.Rows,
        )
    monkeypatch.setattr(
        QMessageBox,
        "question",
        lambda *args, **kwargs: (_ for _ in ()).throw(
            AssertionError("soft-delete 확인 대화상자를 열면 안 됩니다.")
        ),
    )

    actions = _menu_actions(page.stream, 0)
    actions["삭제"].trigger()

    assert page.stream.card_model.rowCount() == 0
    for card_id in (first.id, second.id):
        card = repositories.get_card(card_id)
        assert card is not None
        assert card.deleted_at_us is not None


def test_delete_dirty_open_card_releases_editor_and_keeps_recovery_draft(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    monkeypatch: MonkeyPatch,
) -> None:
    (card,) = _create_cards(database, repositories, ("삭제할 편집 카드",))
    page = DocumentPage(database, repositories, card.document_id)
    qtbot.addWidget(page)
    assert page._open_card(card.id)
    page.editor.setPlainText("삭제 뒤에도 남겨야 할 복구 초안")
    session = page.editor.session
    assert session is not None
    assert session.dirty
    leave_calls: list[bool] = []
    monkeypatch.setattr(
        page,
        "can_leave_editor",
        lambda *, choice_provider=None, protect_now=False: (
            leave_calls.append(protect_now) or True
        ),
    )

    page._delete_cards((card.id,))

    assert leave_calls == [True]
    assert page.editor.card_id is None
    drafts = repositories.list_drafts(card.document_id)
    assert any(
        draft.id == session.draft_id
        and draft.card_id == card.id
        and draft.draft_text == "삭제 뒤에도 남겨야 할 복구 초안"
        for draft in drafts
    )
    with raises(KeyError):
        page.draft_coordinator.session(session.draft_id)


def test_delete_dirty_open_card_auto_saves_before_soft_delete(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    monkeypatch: MonkeyPatch,
) -> None:
    (card,) = _create_cards(database, repositories, ("자동저장 뒤 삭제할 카드",))
    page = DocumentPage(database, repositories, card.document_id)
    qtbot.addWidget(page)
    assert page._open_card(card.id)
    page.editor.setPlainText("삭제 직전에 자동 저장할 본문")
    revision_count = len(repositories.list_revisions(card.id))

    def fail_close_dialog() -> object:
        raise AssertionError("정상 카드 삭제에서 닫기 선택 대화가 호출됨")

    monkeypatch.setattr(page.editor, "_ask_close_choice", fail_close_dialog)

    page._delete_cards((card.id,))

    deleted = repositories.get_card(card.id)
    assert deleted is not None
    assert deleted.body == "삭제 직전에 자동 저장할 본문"
    assert deleted.deleted_at_us is not None
    assert len(repositories.list_revisions(card.id)) == revision_count + 1
    assert page.editor.session is None
    assert page.stream.card_model.rowCount() == 0


def test_card_list_controls_remove_obsolete_filters_and_have_tooltips(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    (card,) = _create_cards(database, repositories, ("본문",))
    page = DocumentPage(database, repositories, card.document_id)
    qtbot.addWidget(page)

    assert page.findChild(QCheckBox, "modifiedCardsOnly") is None
    assert page.findChild(QCheckBox, "insertBeforeSelectedCard") is None
    assert [page.sort_combo.itemText(index) for index in range(3)] == [
        "최근 활동순",
        "현재 문서 순서",
        "최초 기록 순서",
    ]
    assert (
        page.sort_combo.toolTip()
        == "카드를 최근 활동, 현재 문서 순서 또는 최초 기록 순서로 정렬"
    )
    assert page.source_filter.toolTip() == "선택한 입력 출처의 카드만 표시"
    assert [
        page.source_filter.itemText(index)
        for index in range(page.source_filter.count())
    ] == [
        "모든 출처",
        "직접 입력",
        "붙여넣기",
        "혼합",
        "가져오기",
        "복구",
    ]
    assert page.findChild(QPushButton, "toggleOperationGroupButton") is None
    assert page.findChild(QPushButton, "selectOperationGroupButton") is None
    assert (
        page.trash_button.toolTip()
        == "삭제한 카드를 확인하고 복구하거나 완전 삭제"
    )
