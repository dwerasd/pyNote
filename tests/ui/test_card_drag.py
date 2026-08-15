from __future__ import annotations

import json
from collections.abc import Callable, Iterator
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

import pytest
from PySide6.QtCore import (
    QEvent,
    QItemSelectionModel,
    QMimeData,
    QPoint,
    QPointF,
    Qt,
    QTimer,
)
from PySide6.QtGui import QDragEnterEvent, QDragMoveEvent, QDropEvent
from PySide6.QtTest import QTest
from PySide6.QtWidgets import (
    QAbstractItemView,
    QApplication,
    QFileDialog,
    QMessageBox,
    QWidget,
)
from pytest import MonkeyPatch
from pytestqt.qtbot import QtBot
from shiboken6 import isValid

from pynote.application.card_service import CardService
from pynote.domain.events import EventType
from pynote.domain.models import Card, Document
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories
from pynote.ui.cards.card_stream import CardStreamView
from pynote.ui.document_page import DocumentPage

_CARD_MIME = "application/x-pynote-card-id"


def _ids() -> Iterator[str]:
    number = 0
    while True:
        number += 1
        yield f"drag-{number}"


def _create_page(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    bodies: tuple[str, ...] = ("A", "B"),
) -> tuple[DocumentPage, tuple[Card, ...]]:
    document = Document(
        id="drag-document",
        title="카드 드래그",
        created_at_us=1_000_000,
        updated_at_us=1_000_000,
    )
    repositories.create_document(document)
    identifiers = _ids()
    times = iter(range(2_000_000, 2_000_000 + len(bodies) + 20))
    service = CardService(
        database,
        repositories,
        clock=lambda: next(times),
        id_factory=lambda: next(identifiers),
    )
    cards = tuple(service.create_card(document.id, body) for body in bodies)
    page = DocumentPage(database, repositories, document.id)
    qtbot.addWidget(page)
    page.resize(900, 700)
    page.show()
    qtbot.wait(20)
    return page, cards


def test_delete_zone_actually_paints_and_leaves_list_edges_open(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    """표출된 삭제 영역이 실제로 그려지고 목록 하단 좌우를 비워 두는지 본다.

    isVisible() 만 보면 스타일시트 배경이 그려지지 않는 결함(WA_StyledBackground
    누락)을 놓친다 — 픽셀로 판정한다.
    """
    page, cards = _create_page(qtbot, database, repositories, bodies=("A", "B", "C"))
    zone = page.delete_drop_zone
    page._show_delete_drop_zone(cards[0].id, 1)
    qtbot.wait(30)

    assert zone.isVisible()
    image = zone.grab().toImage()
    assert not image.isNull()
    background_band = tuple(
        image.pixelColor(x, y)
        for y in range(image.height() // 8, image.height() // 4)
        for x in range(image.width() // 4, image.width() * 3 // 4)
    )
    red_dominant_count = sum(
        color.alpha() > 0
        and color.red() > color.green()
        and color.red() > color.blue()
        for color in background_band
    )
    red_dominant_ratio = red_dominant_count / len(background_band)
    assert red_dominant_ratio >= 0.9, (
        "삭제 영역 내부 배경이 칠해지지 않았다: "
        f"red-dominant={red_dominant_ratio:.1%}"
    )

    panel_width = page.list_pane.width()
    geometry = zone.geometry()
    assert geometry.left() >= 32, "좌측 여백이 없어 목록 하단이 가려진다"
    assert panel_width - geometry.right() >= 32, "우측 여백이 없어 목록 하단이 가려진다"
    assert page.list_pane.height() - geometry.bottom() >= 12, (
        "하단 여백이 없어 autoscroll edge 가 가려진다"
    )


def _press_card(view: CardStreamView, card_id: str) -> QPoint:
    index = view.card_model.index_for_card(card_id)
    assert index.isValid()
    point = view.visualRect(index).center()
    QTest.mousePress(view.viewport(), Qt.MouseButton.LeftButton, pos=point)
    return point


def _gesture(view: CardStreamView, card_id: str) -> None:
    start = _press_card(view, card_id)
    end = start + QPoint(QApplication.startDragDistance() + 20, 0)
    QTest.mouseMove(view.viewport(), end, delay=10)
    QTest.mouseRelease(view.viewport(), Qt.MouseButton.LeftButton, pos=end)
    QApplication.processEvents()


def _append_editor_text(page: DocumentPage, text: str) -> None:
    QTest.keyClick(page.editor, Qt.Key.Key_End)
    QTest.keyClicks(page.editor, text)


class _SourceDragEnterEvent(QDragEnterEvent):
    def __init__(self, *args: object, source: QWidget) -> None:
        super().__init__(*args)  # type: ignore[arg-type]
        self._test_source = source

    def source(self) -> QWidget:
        return self._test_source


class _SourceDragMoveEvent(QDragMoveEvent):
    def __init__(self, *args: object, source: QWidget) -> None:
        super().__init__(*args)  # type: ignore[arg-type]
        self._test_source = source

    def source(self) -> QWidget:
        return self._test_source


class _SourceDropEvent(QDropEvent):
    def __init__(self, *args: object, source: QWidget) -> None:
        super().__init__(*args)  # type: ignore[arg-type]
        self._test_source = source

    def source(self) -> QWidget:
        return self._test_source


@dataclass(frozen=True)
class _DropObservation:
    accepted: tuple[bool, bool, bool]
    possible_actions: Qt.DropAction
    proposed_action: Qt.DropAction
    drop_action: Qt.DropAction


def _dispatch_drop_sequence(
    target: QWidget,
    mime_data: QMimeData,
    *,
    source: QWidget,
    position: QPoint | None = None,
) -> _DropObservation:
    position = QPoint(8, 8) if position is None else position
    actions = Qt.DropAction.CopyAction | Qt.DropAction.MoveAction
    enter = _SourceDragEnterEvent(
        position,
        actions,
        mime_data,
        Qt.MouseButton.LeftButton,
        Qt.KeyboardModifier.NoModifier,
        source=source,
    )
    QApplication.sendEvent(target, enter)
    move = _SourceDragMoveEvent(
        position,
        actions,
        mime_data,
        Qt.MouseButton.LeftButton,
        Qt.KeyboardModifier.NoModifier,
        source=source,
    )
    QApplication.sendEvent(target, move)
    drop = _SourceDropEvent(
        QPointF(position),
        actions,
        mime_data,
        Qt.MouseButton.LeftButton,
        Qt.KeyboardModifier.NoModifier,
        source=source,
    )
    QApplication.sendEvent(target, drop)
    return _DropObservation(
        accepted=(enter.isAccepted(), move.isAccepted(), drop.isAccepted()),
        possible_actions=drop.possibleActions(),
        proposed_action=drop.proposedAction(),
        drop_action=drop.dropAction(),
    )


class _RecordingCopyDropTarget(QWidget):
    def __init__(self) -> None:
        super().__init__()
        self.setAcceptDrops(True)
        self.drop_observations: list[
            tuple[Qt.DropAction, Qt.DropAction, Qt.DropAction]
        ] = []

    def dragEnterEvent(self, event: QDragEnterEvent) -> None:
        event.setDropAction(Qt.DropAction.CopyAction)
        event.accept()

    def dragMoveEvent(self, event: QDragMoveEvent) -> None:
        event.setDropAction(Qt.DropAction.CopyAction)
        event.accept()

    def dropEvent(self, event: QDropEvent) -> None:
        event.setDropAction(Qt.DropAction.CopyAction)
        event.accept()
        self.drop_observations.append(
            (
                event.possibleActions(),
                event.proposedAction(),
                event.dropAction(),
            )
        )


class _FakeDrag:
    run: Callable[[QMimeData], Qt.DropAction] = lambda _mime: Qt.DropAction.IgnoreAction
    action_arguments: list[tuple[Qt.DropAction, Qt.DropAction]] = []

    def __init__(self, _source: QWidget) -> None:
        self._mime_data = QMimeData()

    def setMimeData(self, mime_data: QMimeData) -> None:
        self._mime_data = mime_data

    def exec(
        self,
        actions: Qt.DropAction,
        default: Qt.DropAction,
    ) -> Qt.DropAction:
        self.action_arguments.append((actions, default))
        return self.run(self._mime_data)


def _install_fake_drag(
    monkeypatch: MonkeyPatch,
    run: Callable[[QMimeData], Qt.DropAction],
) -> None:
    _FakeDrag.run = staticmethod(run)
    _FakeDrag.action_arguments = []
    monkeypatch.setattr("pynote.ui.cards.card_stream.QDrag", _FakeDrag)


def _payload(mime_data: QMimeData) -> dict[str, object]:
    raw = bytes(mime_data.data(_CARD_MIME).data())
    return json.loads(raw.decode("utf-8"))


def test_position_drag_supports_self_move_and_external_copy(
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _create_page(qtbot, database, repositories)
    page.stream.set_sort_mode("position")
    moves: list[tuple[str, object]] = []
    page.stream.card_move_requested.connect(lambda card_id, before: moves.append((card_id, before)))
    copied: list[str] = []
    internal_actions: list[_DropObservation] = []

    def run_internal(mime_data: QMimeData) -> Qt.DropAction:
        panel = page.list_pane
        zone = page.delete_drop_zone
        target = page.stream.viewport()
        # 구현 공식을 베끼지 않고 계약이 요구하는 성질만 단언한다.
        assert 0 < zone.width() <= 280
        assert zone.height() == 56
        assert zone.geometry().center().x() == panel.rect().center().x()
        assert zone.geometry().left() >= 32
        assert panel.width() - zone.geometry().right() >= 32
        assert panel.height() - zone.geometry().bottom() - 1 >= 12
        hit_y = zone.geometry().center().y()
        zone_point = zone.geometry().center()
        left_point = QPoint(zone.geometry().left() - 8, hit_y)
        right_point = QPoint(zone.geometry().right() + 8, hit_y)
        zone_hit = panel.childAt(zone_point)
        assert zone_hit is not None
        assert zone_hit is zone or zone.isAncestorOf(zone_hit)
        assert panel.childAt(left_point) is target
        assert panel.childAt(right_point) is target
        observation = _dispatch_drop_sequence(
            target,
            mime_data,
            source=page.stream,
            position=target.mapFrom(panel, left_point),
        )
        assert observation.accepted == (True, True, True)
        internal_actions.append(observation)
        return Qt.DropAction.MoveAction

    _install_fake_drag(monkeypatch, run_internal)
    _gesture(page.stream, cards[0].id)
    assert moves == [(cards[0].id, None)]
    assert internal_actions == [
        _DropObservation(
            accepted=(True, True, True),
            possible_actions=Qt.DropAction.CopyAction | Qt.DropAction.MoveAction,
            proposed_action=Qt.DropAction.CopyAction,
            drop_action=Qt.DropAction.MoveAction,
        )
    ]

    external_target = _RecordingCopyDropTarget()
    qtbot.addWidget(external_target)
    external_target.show()
    def run_external(mime_data: QMimeData) -> Qt.DropAction:
        copied.append(mime_data.text())
        observation = _dispatch_drop_sequence(
            external_target,
            mime_data,
            source=page.stream,
        )
        assert observation.accepted == (True, True, True)
        return Qt.DropAction.CopyAction

    _install_fake_drag(monkeypatch, run_external)
    _gesture(page.stream, cards[0].id)
    supported = Qt.DropAction.CopyAction | Qt.DropAction.MoveAction
    assert _FakeDrag.action_arguments == [(supported, Qt.DropAction.CopyAction)]
    assert external_target.drop_observations == [
        (supported, Qt.DropAction.CopyAction, Qt.DropAction.CopyAction)
    ]
    assert copied == [cards[0].body]
    assert repositories.get_card(cards[0].id).deleted_at_us is None  # type: ignore[union-attr]


def test_drag_uses_pressed_index_after_selection_changes(
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _create_page(qtbot, database, repositories)
    page.stream.set_sort_mode("position")
    selection = page.stream.selectionModel()
    assert selection is not None
    for card in cards:
        selection.select(
            page.stream.card_model.index_for_card(card.id),
            QItemSelectionModel.SelectionFlag.Select
            | QItemSelectionModel.SelectionFlag.Rows,
        )
    observed: list[str] = []
    moved: list[str] = []
    deleted: list[str] = []
    page.stream.card_move_requested.connect(lambda card_id, _before: moved.append(card_id))
    page.stream.card_delete_dropped.connect(deleted.append)

    def select_first_and_observe(mime_data: QMimeData) -> None:
        selection.clearSelection()
        selection.select(
            page.stream.card_model.index_for_card(cards[0].id),
            QItemSelectionModel.SelectionFlag.Select,
        )
        observed.append(str(_payload(mime_data)["card_id"]))

    def run_move(mime_data: QMimeData) -> Qt.DropAction:
        select_first_and_observe(mime_data)
        target = page.stream.viewport()
        assert _dispatch_drop_sequence(
            target,
            mime_data,
            source=page.stream,
            position=QPoint(10, target.height() - 4),
        ).accepted == (True, True, True)
        return Qt.DropAction.MoveAction

    _install_fake_drag(monkeypatch, run_move)
    _gesture(page.stream, cards[1].id)

    def run_external(mime_data: QMimeData) -> Qt.DropAction:
        observed.append(str(_payload(mime_data)["card_id"]))
        return Qt.DropAction.IgnoreAction

    _install_fake_drag(monkeypatch, run_external)
    _gesture(page.stream, cards[1].id)

    def run_delete(mime_data: QMimeData) -> Qt.DropAction:
        select_first_and_observe(mime_data)
        assert _dispatch_drop_sequence(
            page.delete_drop_zone,
            mime_data,
            source=page.stream,
        ).accepted == (True, True, True)
        return Qt.DropAction.MoveAction

    _install_fake_drag(monkeypatch, run_delete)
    _gesture(page.stream, cards[1].id)
    assert observed == [cards[1].id, cards[1].id, cards[1].id]
    assert moved == [cards[1].id]
    assert deleted == [cards[1].id]


def test_recency_and_capture_modes_allow_drag_but_not_internal_move(
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _create_page(qtbot, database, repositories)
    calls: list[str] = []
    _install_fake_drag(
        monkeypatch,
        lambda mime: calls.append(str(_payload(mime)["card_id"]))
        or Qt.DropAction.CopyAction,
    )
    for mode in ("recency", "capture"):
        page.stream.set_sort_mode(mode)
        assert page.stream.dragEnabled()
        assert not page.stream.acceptDrops()
        assert page.stream.dragDropMode() is QAbstractItemView.DragDropMode.DragOnly
        _gesture(page.stream, cards[0].id)
    assert calls == [cards[0].id, cards[0].id]


def test_delete_zone_appears_on_drag_and_tears_down_on_every_path(
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _create_page(qtbot, database, repositories)
    for action in (
        Qt.DropAction.MoveAction,
        Qt.DropAction.CopyAction,
        Qt.DropAction.IgnoreAction,
        Qt.DropAction.IgnoreAction,
    ):
        def run(_mime: QMimeData, result: Qt.DropAction = action) -> Qt.DropAction:
            assert page.delete_drop_zone.isVisible()
            assert page.delete_drop_zone.armed_token is not None
            return result

        _install_fake_drag(monkeypatch, run)
        _gesture(page.stream, cards[0].id)
        assert page.delete_drop_zone.isHidden()
        assert page.delete_drop_zone.armed_token is None

    _install_fake_drag(
        monkeypatch,
        lambda _mime: (_ for _ in ()).throw(RuntimeError("drag failure")),
    )
    _press_card(page.stream, cards[0].id)
    with pytest.raises(RuntimeError, match="drag failure"):
        page.stream.startDrag(Qt.DropAction.CopyAction | Qt.DropAction.MoveAction)
    assert page.delete_drop_zone.isHidden()
    assert page.delete_drop_zone.armed_token is None

    page._show_delete_drop_zone(cards[0].id, 99)
    page.close()
    assert page.delete_drop_zone.isHidden()
    assert page.delete_drop_zone.armed_token is None
    page._show_delete_drop_zone(cards[0].id, 100)
    page.stream.deleteLater()
    QApplication.sendPostedEvents(page.stream, QEvent.Type.DeferredDelete)
    assert page.delete_drop_zone.armed_token is None


def test_active_drag_source_destruction_is_lifetime_safe(
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _create_page(qtbot, database, repositories)
    source = page.stream
    model = source.card_model
    index = model.index_for_card(cards[0].id)
    zone = page.delete_drop_zone

    def destroy_source_during_exec(_mime: QMimeData) -> Qt.DropAction:
        assert zone.isVisible()
        page.close()
        source.deleteLater()
        QApplication.sendPostedEvents(source, QEvent.Type.DeferredDelete)
        assert not isValid(source)
        return Qt.DropAction.IgnoreAction

    _install_fake_drag(monkeypatch, destroy_source_during_exec)
    _press_card(source, cards[0].id)
    source.startDrag(Qt.DropAction.CopyAction | Qt.DropAction.MoveAction)

    assert not isValid(source)
    assert zone.isHidden()
    assert zone.armed_token is None
    assert _payload(model.mimeData((index,)))["token"] == 0


def test_next_card_click_opens_after_drag_release_is_consumed(
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _create_page(qtbot, database, repositories)
    opened: list[str] = []
    page.stream.card_open_requested.connect(opened.append)
    _install_fake_drag(monkeypatch, lambda _mime: Qt.DropAction.IgnoreAction)

    _press_card(page.stream, cards[0].id)
    page.stream.startDrag(Qt.DropAction.CopyAction | Qt.DropAction.MoveAction)
    assert page.stream._drag_consumed_press

    target = page.stream.visualRect(
        page.stream.card_model.index_for_card(cards[1].id)
    ).center()
    QTest.mouseClick(page.stream.viewport(), Qt.MouseButton.LeftButton, pos=target)

    assert opened == [cards[1].id]


def test_spoofed_custom_mime_cannot_move_or_delete(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _create_page(qtbot, database, repositories)
    page.stream.set_sort_mode("position")
    _press_card(page.stream, cards[0].id)
    page.stream._active_drag_token = 7
    page.delete_drop_zone.arm(7)
    mime_data = QMimeData()
    mime_data.setData(
        _CARD_MIME,
        json.dumps(
            {
                "card_id": cards[0].id,
                "revision_id": cards[0].current_revision_id,
                "token": 7,
            }
        ).encode(),
    )
    other = QWidget()
    qtbot.addWidget(other)
    deleted: list[str] = []
    moved: list[tuple[str, object]] = []
    page.stream.card_delete_dropped.connect(deleted.append)
    page.stream.card_move_requested.connect(lambda card_id, before: moved.append((card_id, before)))

    assert _dispatch_drop_sequence(
        page.delete_drop_zone,
        mime_data,
        source=other,
    ).accepted == (False, False, False)
    assert _dispatch_drop_sequence(
        page.stream.viewport(),
        mime_data,
        source=other,
    ).accepted == (False, False, False)
    assert deleted == []
    assert moved == []


def test_external_drop_does_not_delete_card_and_carries_text_plain(
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _create_page(qtbot, database, repositories)
    received: list[str] = []
    _install_fake_drag(
        monkeypatch,
        lambda mime: received.append(mime.text()) or Qt.DropAction.CopyAction,
    )
    _gesture(page.stream, cards[0].id)
    assert received == [cards[0].body]
    assert repositories.get_card(cards[0].id).deleted_at_us is None  # type: ignore[union-attr]


def test_drag_body_provider_uses_editor_text_for_connected_dirty_card(
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _create_page(qtbot, database, repositories)
    assert page.open_card(cards[0].id)
    _append_editor_text(page, " dirty")
    received: list[str] = []
    _install_fake_drag(
        monkeypatch,
        lambda mime: received.append(mime.text()) or Qt.DropAction.CopyAction,
    )
    _gesture(page.stream, cards[0].id)
    assert received == [f"{cards[0].body} dirty"]


def test_dirty_target_shows_three_choices_with_real_modal(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _create_page(qtbot, database, repositories)
    assert page.open_card(cards[0].id)
    _append_editor_text(page, " dirty")
    _press_card(page.stream, cards[0].id)
    observed: list[set[str]] = []

    def close_dialog() -> None:
        for widget in QApplication.topLevelWidgets():
            if not isinstance(widget, QMessageBox) or not widget.isVisible():
                continue
            buttons = {button.text() for button in widget.buttons()}
            observed.append(buttons)
            for button in widget.buttons():
                if button.text() == "취소":
                    QTest.mouseClick(button, Qt.MouseButton.LeftButton)
                    return

    QTimer.singleShot(0, close_dialog)
    page.stream.card_delete_dropped.emit(cards[0].id)
    assert observed == [{"저장 후 삭제", "그대로 삭제", "취소"}]
    assert repositories.get_card(cards[0].id).deleted_at_us is None  # type: ignore[union-attr]


def test_save_then_delete_uses_post_save_revision(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _create_page(qtbot, database, repositories)
    assert page.open_card(cards[0].id)
    _append_editor_text(page, " saved")
    session = page.editor.session
    assert session is not None
    assert page.protect_now()
    assert repositories.get_draft(session.draft_id) is not None
    original_revision = cards[0].current_revision_id
    _press_card(page.stream, cards[0].id)
    clicked: list[str] = []

    def click_save() -> None:
        for widget in QApplication.topLevelWidgets():
            if not isinstance(widget, QMessageBox) or not widget.isVisible():
                continue
            for button in widget.buttons():
                if button.text() == "저장 후 삭제":
                    clicked.append(button.text())
                    QTest.mouseClick(button, Qt.MouseButton.LeftButton)
                    return

    QTimer.singleShot(0, click_save)
    page.stream.card_delete_dropped.emit(cards[0].id)

    deleted = repositories.get_card(cards[0].id)
    assert clicked == ["저장 후 삭제"]
    assert deleted is not None
    assert deleted.deleted_at_us is not None
    assert deleted.current_revision_id != original_revision
    assert repositories.get_draft(session.draft_id) is None


def test_discard_delete_leaves_no_recovery_candidate(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _create_page(qtbot, database, repositories)
    assert page.open_card(cards[0].id)
    _append_editor_text(page, " discarded")
    session = page.editor.session
    assert session is not None
    assert page.protect_now()
    assert repositories.get_draft(session.draft_id) is not None
    _press_card(page.stream, cards[0].id)
    clicked: list[str] = []

    def click_discard() -> None:
        for widget in QApplication.topLevelWidgets():
            if not isinstance(widget, QMessageBox) or not widget.isVisible():
                continue
            for button in widget.buttons():
                if button.text() == "그대로 삭제":
                    clicked.append(button.text())
                    QTest.mouseClick(button, Qt.MouseButton.LeftButton)
                    return

    QTimer.singleShot(0, click_discard)
    page.stream.card_delete_dropped.emit(cards[0].id)

    assert clicked == ["그대로 삭제"]
    assert repositories.list_drafts(page.document_id) == ()
    assert page.draft_coordinator.recovery_candidates(page.document_id) == ()


def test_clean_delete_discards_previously_protected_draft(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _create_page(qtbot, database, repositories)
    assert page.open_card(cards[0].id)
    _append_editor_text(page, " temporary")
    session = page.editor.session
    assert session is not None and session.dirty
    assert page.protect_now()
    assert repositories.get_draft(session.draft_id) is not None

    QTest.keyClick(
        page.editor,
        Qt.Key.Key_Z,
        Qt.KeyboardModifier.ControlModifier,
    )
    QApplication.processEvents()
    assert page.editor.toPlainText() == cards[0].body
    assert not session.dirty
    _press_card(page.stream, cards[0].id)

    page.stream.card_delete_dropped.emit(cards[0].id)

    assert repositories.list_drafts(page.document_id) == ()
    assert page.draft_coordinator.recovery_candidates(page.document_id) == ()


def test_discard_delete_failure_matrix(
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _create_page(qtbot, database, repositories)
    assert page.open_card(cards[0].id)
    _append_editor_text(page, " keep")
    session = page.editor.session
    assert session is not None
    assert page.protect_now()
    page._error_reporter = lambda _title, _message: None
    _press_card(page.stream, cards[0].id)
    monkeypatch.setattr(page, "_ask_drag_delete_choice", lambda: "discard")
    monkeypatch.setattr(
        page.card_service,
        "soft_delete",
        lambda *args, **kwargs: (_ for _ in ()).throw(RuntimeError("injected")),
    )

    page.stream.card_delete_dropped.emit(cards[0].id)

    card = repositories.get_card(cards[0].id)
    assert card is not None and card.deleted_at_us is None
    assert repositories.get_draft(session.draft_id) is not None
    assert page.editor.toPlainText() == f"{cards[0].body} keep"


def test_delete_other_card_only_protects_current_dirty_editor(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _create_page(qtbot, database, repositories)
    current, target = cards
    assert page.open_card(current.id)
    _append_editor_text(page, " dirty")
    revision_count = len(repositories.list_revisions(current.id))
    _press_card(page.stream, target.id)

    page.stream.card_delete_dropped.emit(target.id)

    assert len(repositories.list_revisions(current.id)) == revision_count
    assert any(draft.card_id == current.id for draft in repositories.list_drafts(page.document_id))
    assert page.editor.card_id == current.id


def test_connected_card_deletion_focuses_empty_editor(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _create_page(qtbot, database, repositories)
    assert page.open_card(cards[0].id)
    _press_card(page.stream, cards[0].id)

    page.stream.card_delete_dropped.emit(cards[0].id)
    qtbot.waitUntil(lambda: QApplication.focusWidget() is page.editor)

    assert page.editor.card_id is None
    assert page.editor.toPlainText() == ""


def test_deleted_card_refreshes_when_new_backing_cannot_be_created(
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _create_page(qtbot, database, repositories)
    assert page.open_card(cards[0].id)
    _press_card(page.stream, cards[0].id)
    changes: list[None] = []
    page.content_changed.connect(lambda: changes.append(None))
    monkeypatch.setattr(page.editor, "_prepare_empty_surface", lambda: False)

    page.stream.card_delete_dropped.emit(cards[0].id)

    deleted = repositories.get_card(cards[0].id)
    assert deleted is not None and deleted.deleted_at_us is not None
    assert not page.stream.card_model.index_for_card(cards[0].id).isValid()
    assert changes == [None]


def test_deleted_card_does_not_emit_duplicate_delete_event(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _create_page(qtbot, database, repositories)
    assert page.open_card(cards[0].id)
    _press_card(page.stream, cards[0].id)

    page.stream.card_delete_dropped.emit(cards[0].id)

    delete_events = [
        event
        for event in repositories.list_events(page.document_id)
        if event.card_id == cards[0].id and event.event_type is EventType.DELETE
    ]
    assert len(delete_events) == 1


def test_cas_mismatch_refuses_drag_delete(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _create_page(qtbot, database, repositories)
    _press_card(page.stream, cards[0].id)
    errors: list[tuple[str, str]] = []
    page._error_reporter = lambda title, message: errors.append((title, message))
    other_page = DocumentPage(database, repositories, page.document_id)
    qtbot.addWidget(other_page)
    assert other_page.open_card(cards[0].id)
    _append_editor_text(other_page, " advanced")
    assert other_page.editor.save_current()
    page.stream.card_delete_dropped.emit(cards[0].id)

    assert repositories.get_card(cards[0].id).deleted_at_us is None  # type: ignore[union-attr]
    assert errors and errors[0][0] == "카드 삭제 거부"


def test_export_menu_writes_selected_cards_to_one_file(
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
    tmp_path: Path,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _create_page(qtbot, database, repositories, ("first", "second"))
    page.stream.set_sort_mode("position")
    destinations = iter((tmp_path / "single.txt", tmp_path / "cards.txt"))
    monkeypatch.setattr(
        QFileDialog,
        "getSaveFileName",
        lambda *args, **kwargs: (
            str(next(destinations)),
            "텍스트 파일 (*.txt)",
        ),
    )
    selection = page.stream.selectionModel()
    assert selection is not None
    first_index = page.stream.card_model.index_for_card(cards[0].id)
    selection.select(
        first_index,
        QItemSelectionModel.SelectionFlag.ClearAndSelect
        | QItemSelectionModel.SelectionFlag.Rows,
    )
    single_menu = page.stream._build_context_menu(first_index)
    assert single_menu is not None
    next(
        action for action in single_menu.actions() if action.text() == "파일로 내보내기"
    ).trigger()

    for card in cards:
        selection.select(
            page.stream.card_model.index_for_card(card.id),
            QItemSelectionModel.SelectionFlag.Select
            | QItemSelectionModel.SelectionFlag.Rows,
        )
    menu = page.stream._build_context_menu(page.stream.card_model.index_for_card(cards[0].id))
    assert menu is not None
    next(action for action in menu.actions() if action.text() == "파일로 내보내기").trigger()

    assert (tmp_path / "single.txt").read_text(encoding="utf-8") == "first"
    assert (tmp_path / "cards.txt").read_text(encoding="utf-8") == "first\n\nsecond"
    assert all(repositories.get_card(card.id).deleted_at_us is None for card in cards)  # type: ignore[union-attr]


def test_export_default_filename_and_suffix_completion(
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
    tmp_path: Path,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _create_page(qtbot, database, repositories, ("export",))
    proposed: list[str] = []
    bare_path = tmp_path / "card-export"

    def choose(
        _parent: QWidget,
        _title: str,
        default_name: str,
        _filters: str,
    ) -> tuple[str, str]:
        proposed.append(default_name)
        return str(bare_path), "Markdown (*.md)"

    monkeypatch.setattr(QFileDialog, "getSaveFileName", choose)
    page._export_cards((cards[0].id,))

    local_stamp = datetime.fromtimestamp(cards[0].created_at_us / 1_000_000)
    assert proposed == [f"pyNote_카드_{local_stamp:%Y%m%d_%H%M%S}.txt"]
    assert not any(character in proposed[0] for character in '<>:"/\\|?*')
    assert bare_path.with_suffix(".md").read_text(encoding="utf-8") == "export"
