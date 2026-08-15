from __future__ import annotations

import json
import logging
from collections.abc import Callable, Sequence
from dataclasses import dataclass

from PySide6.QtCore import (
    QItemSelectionModel,
    QMimeData,
    QModelIndex,
    QPoint,
    Qt,
    QTimer,
    Signal,
)
from PySide6.QtGui import (
    QDrag,
    QDragEnterEvent,
    QDragMoveEvent,
    QDropEvent,
    QKeyEvent,
    QMouseEvent,
    QWheelEvent,
)
from PySide6.QtWidgets import (
    QAbstractItemView,
    QApplication,
    QLabel,
    QListView,
    QMenu,
    QVBoxLayout,
    QWidget,
)
from shiboken6 import isValid

from pynote.domain.models import Card, CardSource
from pynote.ui.cards.card_delegate import CardDelegate
from pynote.ui.cards.card_model import CARD_MIME_TYPE, CardListModel, CardRole

LOGGER = logging.getLogger(__name__)

# Qt 표준 휠 한 틱은 15도(각 단위 120)다. 카드 탐색은 틱당 한 장으로 고정한다.
WHEEL_STEP_ANGLE = 120
# 굴리는 도중에는 열지 않고 멈춘 뒤 한 번만 연다. 틱마다 열면 dirty 카드가
# 매번 이탈 게이트를 통과하며 저장돼 탐색 한 번에 리비전이 수십 개 쌓인다.
BROWSE_OPEN_DELAY_MS = 120


@dataclass(frozen=True, slots=True)
class _DragSnapshot:
    card_id: str
    revision_id: str | None
    press_position: QPoint


@dataclass(frozen=True, slots=True)
class _CardMimePayload:
    card_id: str
    revision_id: str | None
    token: int


def _card_mime_payload(mime_data: QMimeData) -> _CardMimePayload | None:
    if not mime_data.hasFormat(CARD_MIME_TYPE):
        return None
    try:
        raw = bytes(mime_data.data(CARD_MIME_TYPE).data())
        value = json.loads(raw.decode("utf-8"))
    except (AttributeError, TypeError, ValueError, UnicodeDecodeError, json.JSONDecodeError):
        return None
    if not isinstance(value, dict):
        return None
    card_id = value.get("card_id")
    revision_id = value.get("revision_id")
    token = value.get("token")
    if (
        not isinstance(card_id, str)
        or (revision_id is not None and not isinstance(revision_id, str))
        or not isinstance(token, int)
        or isinstance(token, bool)
        or token <= 0
    ):
        return None
    return _CardMimePayload(card_id, revision_id, token)


class CardStreamView(QListView):
    """delegate 렌더링과 카드 선택·열기·이동 요청을 제공하는 1열 스트림이다."""

    card_open_requested = Signal(str)
    card_browse_requested = Signal(str)
    card_move_requested = Signal(str, object)
    cards_delete_requested = Signal(object)
    cards_export_requested = Signal(object)
    card_delete_dropped = Signal(str)
    drag_started = Signal(str, int)
    drag_finished = Signal(int)
    empty_area_clicked = Signal()

    def __init__(
        self,
        model: CardListModel | None = None,
        *,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.setObjectName("cardStreamView")
        self.setModel(model or CardListModel())
        self.setItemDelegate(CardDelegate(self))
        self.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self.setDefaultDropAction(Qt.DropAction.MoveAction)
        self.setMouseTracking(True)
        self.setUniformItemSizes(False)
        self.setResizeMode(QListView.ResizeMode.Adjust)
        self.setVerticalScrollMode(QAbstractItemView.ScrollMode.ScrollPerPixel)
        self.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.customContextMenuRequested.connect(self._show_context_menu)
        self._empty_press_position: QPoint | None = None
        self._empty_press_moved = False
        self._drag_snapshot: _DragSnapshot | None = None
        self._active_drag_token: int | None = None
        self._last_drag_token = 0
        self._drag_body_provider: Callable[[str], str] | None = None
        self._drag_consumed_press = False
        self._wheel_angle = 0
        self._pending_browse_card_id: str | None = None
        self._browse_timer = QTimer(self)
        self._browse_timer.setSingleShot(True)
        self._browse_timer.setInterval(BROWSE_OPEN_DELAY_MS)
        self._browse_timer.timeout.connect(self._request_browse_open)
        self.card_model.modelAboutToBeReset.connect(self.cancel_pending_browse)
        self.card_model.rowsAboutToBeRemoved.connect(self.cancel_pending_browse)
        self._apply_drag_state()

    @property
    def card_model(self) -> CardListModel:
        """현재 카드 전용 모델을 반환한다."""
        model = self.model()
        if not isinstance(model, CardListModel):
            raise TypeError("CardStreamView에는 CardListModel이 필요합니다.")
        return model

    def set_cards(self, cards: Sequence[Card]) -> None:
        """DB에서 읽은 카드 목록으로 현재 스트림을 갱신한다."""
        self.card_model.set_cards(cards)

    def set_sort_mode(self, sort_mode: str) -> None:
        """정렬 표시를 전환하고 문서순에서만 드래그 이동을 허용한다."""
        self.card_model.set_sort_mode(sort_mode)
        self._apply_drag_state()

    def set_multi_selection_enabled(self, enabled: bool) -> None:
        """카드 다중 선택을 켜거나 끄고, 끌 때는 선택을 한 장으로 줄인다."""
        self.setSelectionMode(
            QAbstractItemView.SelectionMode.ExtendedSelection
            if enabled
            else QAbstractItemView.SelectionMode.SingleSelection
        )
        selection_model = self.selectionModel()
        if enabled or selection_model is None:
            return
        # setSelectionMode 는 이미 잡혀 있는 선택을 줄이지 않는다 — 그대로 두면
        # 끈 뒤에도 삭제·내보내기가 여러 장을 받는다.
        indexes = sorted(self.selectedIndexes(), key=QModelIndex.row)
        if len(indexes) <= 1:
            return
        current = self.currentIndex()
        survivor = current if current in indexes else indexes[0]
        selection_model.select(
            survivor,
            QItemSelectionModel.SelectionFlag.ClearAndSelect
            | QItemSelectionModel.SelectionFlag.Rows,
        )
        selection_model.setCurrentIndex(
            survivor,
            QItemSelectionModel.SelectionFlag.NoUpdate,
        )

    def set_source_filter(
        self,
        sources: Sequence[CardSource | str] | None,
    ) -> None:
        """출처 필터를 모델에 적용한다."""
        self.card_model.set_source_filter(sources)

    def apply_time_display(self, time_format: str, timezone: str) -> None:
        """delegate와 툴팁의 카드 시간 표시를 갱신한다."""
        self.card_model.apply_time_display(time_format, timezone)
        delegate = self.itemDelegate()
        if isinstance(delegate, CardDelegate):
            delegate.apply_time_display(time_format, timezone)
            self.viewport().update()

    def selected_card_ids(self) -> tuple[str, ...]:
        """Ctrl+클릭을 포함해 현재 선택된 카드 ID를 행 순서로 반환한다."""
        indexes = sorted(self.selectedIndexes(), key=QModelIndex.row)
        return tuple(str(index.data(CardRole.CARD_ID)) for index in indexes)

    def set_drag_body_provider(self, provider: Callable[[str], str]) -> None:
        """외부 drag payload에 실을 현재 화면 본문 조회자를 설치한다."""
        self._drag_body_provider = provider

    def active_drag_revision(self, card_id: str) -> str | None:
        """현재 drag 대상이 맞을 때 press 시점의 CAS 리비전을 반환한다."""
        snapshot = self._drag_snapshot
        if snapshot is None or snapshot.card_id != card_id:
            return None
        return snapshot.revision_id

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:
        """일반 클릭은 열고 Ctrl+클릭은 다중 선택에만 사용한다."""
        position = event.position().toPoint()
        index = self.indexAt(position)
        super().mouseReleaseEvent(event)
        empty_area_clicked = (
            event.button() == Qt.MouseButton.LeftButton
            and self.viewport().rect().contains(position)
            and not index.isValid()
            and self._empty_press_position is not None
            and not self._empty_press_moved
            and event.modifiers() == Qt.KeyboardModifier.NoModifier
        )
        self._empty_press_position = None
        self._empty_press_moved = False
        if empty_area_clicked:
            self.empty_area_clicked.emit()
        if self._drag_consumed_press:
            self._drag_consumed_press = False
            self._drag_snapshot = None
            return
        if (
            event.button() != Qt.MouseButton.LeftButton
            or not index.isValid()
            or event.modifiers() & Qt.KeyboardModifier.ControlModifier
        ):
            if event.button() == Qt.MouseButton.LeftButton:
                self._drag_snapshot = None
            return
        card_id = index.data(CardRole.CARD_ID)
        if card_id is not None:
            self.card_open_requested.emit(str(card_id))
        self._drag_snapshot = None

    def mousePressEvent(self, event: QMouseEvent) -> None:
        """우클릭은 기존 다중 선택을 유지하며 메뉴 대상만 현재 카드로 바꾼다."""
        self.cancel_pending_browse()
        position = event.position().toPoint()
        index = self.indexAt(position)
        if event.button() == Qt.MouseButton.LeftButton:
            self._drag_consumed_press = False
        if event.button() == Qt.MouseButton.LeftButton and index.isValid():
            card = index.data(CardRole.CARD)
            self._drag_snapshot = (
                _DragSnapshot(
                    card_id=card.id,
                    revision_id=card.current_revision_id,
                    press_position=position,
                )
                if isinstance(card, Card)
                else None
            )
        elif event.button() == Qt.MouseButton.LeftButton:
            self._drag_snapshot = None
        if (
            event.button() == Qt.MouseButton.LeftButton
            and self.viewport().rect().contains(position)
            and not index.isValid()
            and event.modifiers() == Qt.KeyboardModifier.NoModifier
        ):
            self._empty_press_position = position
            self._empty_press_moved = False
        else:
            self._empty_press_position = None
        if event.button() == Qt.MouseButton.RightButton:
            if index.isValid():
                self._select_context_index(index)
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        """빈 영역 press 뒤 드래그 임계 도달을 release까지 유지한다."""
        super().mouseMoveEvent(event)
        if (
            self._empty_press_position is not None
            and (
                event.position().toPoint() - self._empty_press_position
            ).manhattanLength()
            >= QApplication.startDragDistance()
        ):
            self._empty_press_moved = True
        if (
            self._empty_press_position is not None
            and self.state() == QAbstractItemView.State.DragSelectingState
        ):
            self._empty_press_moved = True

    def keyPressEvent(self, event: QKeyEvent) -> None:
        """Enter로 선택 카드를 열고 Delete로 soft-delete 요청을 보낸다."""
        self.cancel_pending_browse()
        if event.key() in {Qt.Key.Key_Return, Qt.Key.Key_Enter}:
            index = self.currentIndex()
            card_id = index.data(CardRole.CARD_ID)
            if card_id is not None:
                self.card_open_requested.emit(str(card_id))
                event.accept()
                return
        if event.key() == Qt.Key.Key_Delete:
            card_ids = self.selected_card_ids()
            if card_ids:
                self.cards_delete_requested.emit(card_ids)
                event.accept()
                return
        super().keyPressEvent(event)

    def wheelEvent(self, event: QWheelEvent) -> None:
        """휠을 스크롤 대신 카드 단위 선택 이동에 쓰고 스크롤은 따라오게 한다."""
        angle = event.angleDelta().y()
        row_count = self.card_model.rowCount()
        if (
            event.modifiers() != Qt.KeyboardModifier.NoModifier
            or angle == 0
            or row_count == 0
            or self._active_drag_token is not None
        ):
            # 카드 이동으로 소비할 수 없는 입력은 삼키지 않는다 — 수평 휠,
            # pixelDelta 만 싣는 장치, gesture 경계의 0 각 이벤트가 여기로 온다.
            super().wheelEvent(event)
            return
        event.accept()
        if self._wheel_angle != 0 and (angle > 0) != (self._wheel_angle > 0):
            # 방향을 바꾸면 이전 방향의 잔여 각을 버린다 — 남겨 두면 반대 방향
            # 한 틱이 상쇄돼 카드가 움직이지 않는다.
            self._wheel_angle = 0
        self._wheel_angle += angle
        # 고해상도 휠·터치패드는 한 틱보다 작은 각을 여러 번 보내므로 남은 각을
        # 누적해야 이동 속도가 표준 휠과 같아진다.
        steps = int(self._wheel_angle / WHEEL_STEP_ANGLE)
        self._wheel_angle -= steps * WHEEL_STEP_ANGLE
        if steps != 0:
            current = self.currentIndex()
            if current.isValid():
                row = min(max(current.row() - steps, 0), row_count - 1)
            else:
                row = 0 if steps < 0 else row_count - 1
            index = self.card_model.index(row)
            self.setCurrentIndex(index)
            self.scrollTo(index, QAbstractItemView.ScrollHint.EnsureVisible)
            card_id = index.data(CardRole.CARD_ID)
            self._pending_browse_card_id = None if card_id is None else str(card_id)
        if self._pending_browse_card_id is not None:
            # 한 틱을 못 채운 각과 관성 이벤트도 대기를 미뤄야 굴리는 도중에
            # 열리지 않는다.
            self._browse_timer.start()

    def cancel_pending_browse(self) -> None:
        """더 최신 의도가 들어오면 대기 중인 휠 탐색 열기를 폐기한다."""
        self._browse_timer.stop()
        self._pending_browse_card_id = None
        self._wheel_angle = 0

    def _request_browse_open(self) -> None:
        card_id = self._pending_browse_card_id
        self._pending_browse_card_id = None
        if card_id is None:
            return
        index = self.card_model.index_for_card(card_id)
        # 대기 중 다른 의도가 선택을 옮겼으면 그 선택이 이긴다 — 만료 시점의
        # 현재 행을 다시 읽으면 목록만 옮기는 계약까지 편집면에 연결해 버린다.
        if index.isValid() and index == self.currentIndex():
            self.card_browse_requested.emit(card_id)

    def dragEnterEvent(self, event: QDragEnterEvent) -> None:
        """활성 self drag의 일치하는 custom MIME만 내부 이동 입구에서 받는다."""
        if not self._accepts_internal_drag(event):
            event.ignore()
            return
        super().dragEnterEvent(event)
        event.setDropAction(Qt.DropAction.MoveAction)
        event.accept()

    def dragMoveEvent(self, event: QDragMoveEvent) -> None:
        """drag-enter와 같은 신뢰 조건을 이동 전 구간에도 유지한다."""
        if not self._accepts_internal_drag(event):
            event.ignore()
            return
        super().dragMoveEvent(event)
        event.setDropAction(Qt.DropAction.MoveAction)
        event.accept()

    def dropEvent(self, event: QDropEvent) -> None:
        """내부 드래그를 서비스 계층의 before-card 이동 요청으로 변환한다."""
        if self.card_model.sort_mode != "position":
            event.ignore()
            return
        payload = _card_mime_payload(event.mimeData())
        if (
            event.source() is not self
            or payload is None
            or self._active_drag_token is None
            or payload.token != self._active_drag_token
        ):
            event.ignore()
            return
        snapshot = self._drag_snapshot
        if snapshot is not None:
            if (
                payload.card_id != snapshot.card_id
                or payload.revision_id != snapshot.revision_id
            ):
                event.ignore()
                return
            card_id = snapshot.card_id
        else:
            selected_ids = self.selected_card_ids()
            if not selected_ids or payload.card_id != selected_ids[0]:
                event.ignore()
                return
            card_id = selected_ids[0]
        before_card_id = self._drop_before_card_id(event.position().toPoint())
        if before_card_id == card_id:
            event.ignore()
            return
        self.card_move_requested.emit(card_id, before_card_id)
        event.setDropAction(Qt.DropAction.MoveAction)
        event.accept()

    def _apply_drag_state(self) -> None:
        position = self.card_model.sort_mode == "position"
        self.setDragDropMode(
            QAbstractItemView.DragDropMode.InternalMove
            if position
            else QAbstractItemView.DragDropMode.DragOnly
        )
        self.setDropIndicatorShown(position)

    def startDrag(self, _supported_actions: Qt.DropAction) -> None:
        """press snapshot 한 장으로 내부 Move와 외부 Copy drag를 직접 실행한다."""
        snapshot = self._drag_snapshot
        if snapshot is None:
            return
        model = self.card_model
        index = model.index_for_card(snapshot.card_id)
        if not index.isValid():
            self._drag_snapshot = None
            return
        self._last_drag_token += 1
        token = self._last_drag_token
        self._active_drag_token = token
        self._drag_consumed_press = True
        self.drag_started.emit(snapshot.card_id, token)
        drag_executed = False
        try:
            body = (
                str(index.data(CardRole.BODY))
                if self._drag_body_provider is None
                else self._drag_body_provider(snapshot.card_id)
            )
            model.set_drag_payload(token=token, body=body)
            mime_data = model.mimeData((index,))
            drag = QDrag(self)
            drag.setMimeData(mime_data)
            drag.exec(
                Qt.DropAction.CopyAction | Qt.DropAction.MoveAction,
                Qt.DropAction.CopyAction,
            )
            drag_executed = True
        finally:
            try:
                if isValid(model):
                    model.clear_drag_payload()
            except BaseException:
                LOGGER.exception("카드 drag model teardown에 실패했습니다.")
            try:
                if isValid(self):
                    self._active_drag_token = None
                    self._drag_snapshot = None
                    if not drag_executed:
                        self._drag_consumed_press = False
            except BaseException:
                LOGGER.exception("카드 drag view teardown에 실패했습니다.")
            try:
                if isValid(self):
                    self.drag_finished.emit(token)
            except BaseException:
                LOGGER.exception("카드 drag 종료 신호 발행에 실패했습니다.")

    def _show_context_menu(self, position: QPoint) -> None:
        menu = self._build_context_menu(self.indexAt(position))
        if menu is not None:
            self._execute_context_menu(menu, self.viewport().mapToGlobal(position))

    @staticmethod
    def _execute_context_menu(menu: QMenu, global_position: QPoint) -> None:
        menu.exec(global_position)

    def _build_context_menu(self, index: QModelIndex) -> QMenu | None:
        if not index.isValid():
            return None
        selection_model = self._select_context_index(index)
        if selection_model is None:
            return None
        card_id = str(index.data(CardRole.CARD_ID))
        body = str(index.data(CardRole.BODY))
        selected_ids = self.selected_card_ids()

        menu = QMenu(self)
        open_action = menu.addAction("편집기에서 열기")
        copy_action = menu.addAction("본문 복사")
        export_action = menu.addAction("파일로 내보내기")
        delete_action = menu.addAction("삭제")

        open_action.triggered.connect(
            lambda _checked=False: self.card_open_requested.emit(card_id)
        )
        copy_action.triggered.connect(
            lambda _checked=False: QApplication.clipboard().setText(body)
        )
        export_action.triggered.connect(
            lambda _checked=False: self.cards_export_requested.emit(selected_ids)
        )
        delete_action.triggered.connect(
            lambda _checked=False: self.cards_delete_requested.emit(selected_ids)
        )
        return menu

    def _select_context_index(
        self,
        index: QModelIndex,
    ) -> QItemSelectionModel | None:
        selection_model = self.selectionModel()
        if selection_model is None:
            return None
        if not selection_model.isSelected(index):
            selection_model.select(
                index,
                QItemSelectionModel.SelectionFlag.ClearAndSelect
                | QItemSelectionModel.SelectionFlag.Rows,
            )
        selection_model.setCurrentIndex(
            index,
            QItemSelectionModel.SelectionFlag.NoUpdate,
        )
        return selection_model

    def _drop_before_card_id(self, position: QPoint) -> str | None:
        index = self.indexAt(position)
        if not index.isValid():
            return None
        rect = self.visualRect(index)
        row = index.row()
        if position.y() >= rect.center().y():
            row += 1
        if row >= self.card_model.rowCount():
            return None
        target = self.card_model.index(row)
        value = target.data(CardRole.CARD_ID)
        return None if value is None else str(value)

    def _accepts_internal_drag(
        self,
        event: QDragEnterEvent | QDragMoveEvent,
    ) -> bool:
        payload = _card_mime_payload(event.mimeData())
        snapshot = self._drag_snapshot
        return (
            self.card_model.sort_mode == "position"
            and event.source() is self
            and payload is not None
            and self._active_drag_token is not None
            and payload.token == self._active_drag_token
            and snapshot is not None
            and payload.card_id == snapshot.card_id
            and payload.revision_id == snapshot.revision_id
        )


class CardDeleteDropZone(QWidget):
    """활성 card drag만 받는 목록 하단 중앙의 56px soft-delete pill이다."""

    card_delete_dropped = Signal(str)

    def __init__(
        self,
        source: CardStreamView,
        *,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._source = source
        self._token: int | None = None
        self.setObjectName("cardDeleteDropZone")
        self.setAcceptDrops(True)
        self.setFixedHeight(56)
        self.setToolTip("여기에 놓으면 카드를 휴지통으로 이동합니다.")
        # 순수 QWidget 은 WA_StyledBackground 없이는 스타일시트 배경을 그리지 않는다 —
        # 없으면 영역이 투명해 드래그 중 아무 표시도 남지 않는다.
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self.setStyleSheet(
            "#cardDeleteDropZone {"
            "background: rgba(178, 45, 45, 220);"
            "border: 1px solid rgba(255, 255, 255, 110);"
            "border-radius: 8px;"
            "}"
            "#cardDeleteDropZoneLabel { color: white; }"
        )
        label = QLabel("여기에 놓으면 휴지통으로 이동", self)
        label.setObjectName("cardDeleteDropZoneLabel")
        label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.addWidget(label)
        self.hide()

    @property
    def armed_token(self) -> int | None:
        """시험과 수명 점검에서 현재 활성 token을 확인한다."""
        return self._token

    def arm(self, token: int) -> None:
        """현재 source drag token 한 건만 받도록 준비한다."""
        self._token = token

    def disarm(self) -> None:
        """이전 drag token을 즉시 재사용 불가로 만든다."""
        self._token = None

    def dragEnterEvent(self, event: QDragEnterEvent) -> None:
        if self._accepts(event):
            event.setDropAction(Qt.DropAction.MoveAction)
            event.accept()
        else:
            event.ignore()

    def dragMoveEvent(self, event: QDragMoveEvent) -> None:
        if self._accepts(event):
            event.setDropAction(Qt.DropAction.MoveAction)
            event.accept()
        else:
            event.ignore()

    def dropEvent(self, event: QDropEvent) -> None:
        payload = _card_mime_payload(event.mimeData())
        if not self._accepts(event) or payload is None:
            event.ignore()
            return
        self.card_delete_dropped.emit(payload.card_id)
        event.setDropAction(Qt.DropAction.MoveAction)
        event.accept()

    def _accepts(
        self,
        event: QDragEnterEvent | QDragMoveEvent | QDropEvent,
    ) -> bool:
        payload = _card_mime_payload(event.mimeData())
        return (
            self._token is not None
            and event.source() is self._source
            and payload is not None
            and payload.token == self._token
            and self._source._active_drag_token == self._token
            and self._source._drag_snapshot is not None
            and payload.card_id == self._source._drag_snapshot.card_id
            and payload.revision_id == self._source._drag_snapshot.revision_id
        )
