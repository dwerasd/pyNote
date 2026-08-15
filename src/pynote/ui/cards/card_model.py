from __future__ import annotations

import json
from collections.abc import Mapping, Sequence
from enum import IntEnum

from PySide6.QtCore import (
    QAbstractListModel,
    QByteArray,
    QDateTime,
    QMimeData,
    QModelIndex,
    QPersistentModelIndex,
    Qt,
    QTimeZone,
)

from pynote.domain.models import Card, CardSource

CARD_MIME_TYPE = "application/x-pynote-card-id"


class CardRole(IntEnum):
    """delegate와 카드 스트림이 공유하는 모델 role이다."""

    CARD = Qt.ItemDataRole.UserRole + 1
    CARD_ID = Qt.ItemDataRole.UserRole + 2
    BODY = Qt.ItemDataRole.UserRole + 3
    PREVIEW = Qt.ItemDataRole.UserRole + 4
    POSITION_NUMBER = Qt.ItemDataRole.UserRole + 5
    CAPTURE_SEQ = Qt.ItemDataRole.UserRole + 6
    CREATED_AT_US = Qt.ItemDataRole.UserRole + 7
    UPDATED_AT_US = Qt.ItemDataRole.UserRole + 8
    SOURCE = Qt.ItemDataRole.UserRole + 9
    REVISION_COUNT = Qt.ItemDataRole.UserRole + 10
    MODIFIED = Qt.ItemDataRole.UserRole + 16
    DIRTY_DRAFT = Qt.ItemDataRole.UserRole + 17
    RECONSTRUCTION_AVAILABLE = Qt.ItemDataRole.UserRole + 19
    PREVIEW_LINE_COUNT = Qt.ItemDataRole.UserRole + 20
    PREVIEW_TRUNCATED = Qt.ItemDataRole.UserRole + 21


PREVIEW_LINE_COUNT = 3
# CEILING: 줄당 예산은 화면당 최대 문자 수의 증명값이 아니라 성능 정책값이다.
# 임의로 넓은 창과 임의 Unicode 조합에서는 이론상 표시 가능한 것보다 미리보기에
# 글자가 덜 보일 수 있고, 그때도 PREVIEW_TRUNCATED가 말줄임표를 세운다. 실제로
# 부족하다는 관측이 나오면 이 값만 올린다.
PREVIEW_CODEPOINT_BUDGET_PER_LINE = 4_096
_ROOT_INDEX = QModelIndex()


class CardListModel(QAbstractListModel):
    """정렬·필터·미리보기 설정을 제공하는 카드 목록 모델이다."""

    def __init__(
        self,
        cards: Sequence[Card] = (),
        *,
        revision_counts: Mapping[str, int] | None = None,
        dirty_draft_ids: frozenset[str] = frozenset(),
        preview_line_count: int = PREVIEW_LINE_COUNT,
        reconstruction_unavailable_ids: frozenset[str] = frozenset(),
    ) -> None:
        super().__init__()
        if preview_line_count < 1:
            raise ValueError("카드 미리보기 줄 수는 1 이상이어야 합니다.")
        self._all_cards: tuple[Card, ...] = ()
        self._visible_cards: tuple[Card, ...] = ()
        self._revision_counts = dict(revision_counts or {})
        self._dirty_draft_ids = dirty_draft_ids
        self._preview_line_count = preview_line_count
        self._reconstruction_unavailable_ids = reconstruction_unavailable_ids
        self._sort_mode = "recency"
        self._source_filter: frozenset[CardSource] | None = None
        self._position_numbers: dict[str, int] = {}
        self._time_format = "yyyy-MM-dd HH:mm:ss"
        self._timezone = "system"
        self._drag_token = 0
        self._drag_body: str | None = None
        self.set_cards(cards)

    @property
    def sort_mode(self) -> str:
        """현재 정렬 기준을 반환한다."""
        return self._sort_mode

    def rowCount(
        self,
        parent: QModelIndex | QPersistentModelIndex = _ROOT_INDEX,
    ) -> int:
        """현재 필터를 통과한 카드 수를 반환한다."""
        return 0 if parent.isValid() else len(self._visible_cards)

    def data(
        self,
        index: QModelIndex | QPersistentModelIndex,
        role: int = Qt.ItemDataRole.DisplayRole,
    ) -> object:
        """delegate와 접근성 뷰에 카드 데이터를 제공한다."""
        if not index.isValid() or index.row() >= len(self._visible_cards):
            return None
        card = self._visible_cards[index.row()]
        if role == Qt.ItemDataRole.DisplayRole:
            # 접근성 뷰가 읽는 값이라 미리보기와 달리 원문을 그대로 준다.
            return card.body
        if role == CardRole.CARD:
            return card
        if role == CardRole.CARD_ID:
            return card.id
        if role == CardRole.BODY:
            return card.body
        if role == CardRole.PREVIEW:
            return self._preview(card)
        if role == CardRole.PREVIEW_TRUNCATED:
            return len(card.body) > self._preview_budget()
        if role == CardRole.POSITION_NUMBER:
            return self._position_numbers[card.id]
        if role == CardRole.CAPTURE_SEQ:
            return card.capture_seq
        if role == CardRole.CREATED_AT_US:
            return card.created_at_us
        if role == CardRole.UPDATED_AT_US:
            return card.updated_at_us
        if role == CardRole.SOURCE:
            return card.source.value
        if role == CardRole.REVISION_COUNT:
            return self._revision_counts.get(card.id, 1)
        if role == CardRole.MODIFIED:
            return card.updated_at_us > card.created_at_us
        if role == CardRole.DIRTY_DRAFT:
            return card.id in self._dirty_draft_ids
        if role == CardRole.RECONSTRUCTION_AVAILABLE:
            return card.id not in self._reconstruction_unavailable_ids
        if role == CardRole.PREVIEW_LINE_COUNT:
            return self._preview_line_count
        if role == Qt.ItemDataRole.ToolTipRole:
            position = self._position_numbers[card.id]
            source = self._source_label(card.source.value)
            created = self._time_label(card.created_at_us)
            updated = self._time_label(card.updated_at_us)
            revision_count = self._revision_counts.get(card.id, 1)
            reconstruction = (
                ""
                if card.id not in self._reconstruction_unavailable_ids
                else "\n형제 카드 purge로 작업 원문 재구성 불가"
            )
            return (
                f"위치 {position}은 문서 안의 현재 순서이며 "
                "현재 문서 순서 보기에서 이동할 수 있습니다.\n"
                f"기록 #{card.capture_seq}은 최초 생성 순서이며 바뀌지 않습니다.\n"
                f"출처 {source}\n"
                f"최초 기록 {created}\n"
                f"리비전 {revision_count}개\n"
                f"수정 {updated}"
                f"{reconstruction}"
            )
        return None

    def roleNames(self) -> dict[int, QByteArray]:
        """테스트와 향후 QML 어댑터에서 사용할 role 이름을 반환한다."""
        roles = super().roleNames()
        roles.update(
            {
                int(CardRole.CARD_ID): QByteArray(b"cardId"),
                int(CardRole.BODY): QByteArray(b"body"),
                int(CardRole.PREVIEW): QByteArray(b"preview"),
                int(CardRole.PREVIEW_TRUNCATED): QByteArray(b"previewTruncated"),
                int(CardRole.POSITION_NUMBER): QByteArray(b"positionNumber"),
                int(CardRole.CAPTURE_SEQ): QByteArray(b"captureSeq"),
            }
        )
        return roles

    def flags(
        self,
        index: QModelIndex | QPersistentModelIndex,
    ) -> Qt.ItemFlag:
        """유효 카드 행을 선택·활성·드래그 가능한 항목으로 표시한다."""
        flags = super().flags(index)
        if index.isValid():
            flags |= Qt.ItemFlag.ItemIsDragEnabled
        return flags

    def mimeTypes(self) -> list[str]:
        """내부 권한 판정 MIME과 외부 본문 전달 MIME을 함께 제공한다."""
        return [CARD_MIME_TYPE, "text/plain"]

    def set_drag_payload(self, *, token: int, body: str) -> None:
        """view가 확정한 활성 drag token과 화면 본문을 다음 MIME에 싣는다."""
        self._drag_token = token
        self._drag_body = body

    def clear_drag_payload(self) -> None:
        """완료된 drag의 token과 화면 본문을 재사용하지 않게 무효화한다."""
        self._drag_token = 0
        self._drag_body = None

    def mimeData(
        self,
        indexes: Sequence[QModelIndex | QPersistentModelIndex],
    ) -> QMimeData:
        """받은 첫 카드 index 한 장만 UTF-8 JSON과 평문으로 직렬화한다."""
        mime_data = QMimeData()
        if not indexes:
            return mime_data
        index = indexes[0]
        if not index.isValid():
            return mime_data
        card = index.data(CardRole.CARD)
        if not isinstance(card, Card):
            return mime_data
        payload = json.dumps(
            {
                "card_id": card.id,
                "revision_id": card.current_revision_id,
                "token": self._drag_token,
            },
            ensure_ascii=False,
            separators=(",", ":"),
        ).encode("utf-8")
        mime_data.setData(CARD_MIME_TYPE, QByteArray(payload))
        mime_data.setText(card.body if self._drag_body is None else self._drag_body)
        return mime_data

    def supportedDragActions(self) -> Qt.DropAction:
        """내부 Move와 외부 Copy 협상을 모두 명시한다."""
        return Qt.DropAction.CopyAction | Qt.DropAction.MoveAction

    def supportedDropActions(self) -> Qt.DropAction:
        """목록이 받는 custom MIME은 내부 Move만 허용한다."""
        return Qt.DropAction.MoveAction

    def set_cards(
        self,
        cards: Sequence[Card],
        *,
        revision_counts: Mapping[str, int] | None = None,
    ) -> None:
        """DB에서 다시 읽은 카드 스냅샷으로 모델을 교체한다."""
        self.beginResetModel()
        self._all_cards = tuple(card for card in cards if card.deleted_at_us is None)
        if revision_counts is not None:
            self._revision_counts = dict(revision_counts)
        self._rebuild_metadata()
        self._apply_sort_and_filter()
        self.endResetModel()

    def update_card(self, card: Card, *, revision_count: int) -> bool:
        """카드 하나를 갱신하고 정렬 위치가 바뀌면 해당 행만 이동한다."""
        all_index = next(
            (
                index
                for index, current in enumerate(self._all_cards)
                if current.id == card.id
            ),
            None,
        )
        if all_index is None or card.deleted_at_us is not None:
            return False
        all_cards = list(self._all_cards)
        all_cards[all_index] = card
        self._all_cards = tuple(all_cards)
        self._revision_counts[card.id] = revision_count
        old_visible_index = self.index_for_card(card.id)
        if not old_visible_index.isValid():
            return True
        old_row = old_visible_index.row()
        visible_cards = list(self._visible_cards)
        visible_cards[old_row] = card
        self._visible_cards = tuple(visible_cards)
        sorted_cards = self._sorted_and_filtered_cards()
        new_row = next(
            row for row, visible_card in enumerate(sorted_cards) if visible_card.id == card.id
        )
        if new_row != old_row:
            destination = new_row if new_row < old_row else new_row + 1
            self.beginMoveRows(
                _ROOT_INDEX,
                old_row,
                old_row,
                _ROOT_INDEX,
                destination,
            )
            self._visible_cards = sorted_cards
            self.endMoveRows()
        visible_index = self.index(new_row)
        self.dataChanged.emit(
            visible_index,
            visible_index,
            [
                int(Qt.ItemDataRole.DisplayRole),
                int(Qt.ItemDataRole.SizeHintRole),
                int(CardRole.CARD),
                int(CardRole.BODY),
                int(CardRole.PREVIEW),
                int(CardRole.PREVIEW_TRUNCATED),
                int(CardRole.UPDATED_AT_US),
                int(CardRole.REVISION_COUNT),
                int(CardRole.MODIFIED),
            ],
        )
        return True

    def add_cards(
        self,
        cards: Sequence[Card],
        *,
        revision_counts: Mapping[str, int] | None = None,
    ) -> None:
        """새 카드들을 현재 정렬 위치에 행 삽입으로 추가한다."""
        existing_ids = {card.id for card in self._all_cards}
        additions = tuple(
            card
            for card in cards
            if card.deleted_at_us is None and card.id not in existing_ids
        )
        if not additions:
            return
        self._all_cards = (*self._all_cards, *additions)
        for card in additions:
            self._revision_counts[card.id] = (
                1 if revision_counts is None else revision_counts.get(card.id, 1)
            )
        self._rebuild_metadata()
        previous_visible = self._visible_cards
        final_visible = self._sorted_and_filtered_cards()
        self._visible_cards = previous_visible
        addition_ids = {card.id for card in additions}
        visible_cards = list(previous_visible)
        for row, card in enumerate(final_visible):
            if card.id not in addition_ids:
                continue
            self.beginInsertRows(_ROOT_INDEX, row, row)
            visible_cards.insert(row, card)
            self._visible_cards = tuple(visible_cards)
            self.endInsertRows()

    def set_sort_mode(self, sort_mode: str) -> None:
        """최근 활동순·문서순·최초 기록순을 전환한다."""
        if sort_mode not in {"recency", "position", "capture"}:
            raise ValueError(f"지원하지 않는 정렬 모드입니다: {sort_mode}")
        if sort_mode == self._sort_mode:
            return
        self.beginResetModel()
        self._sort_mode = sort_mode
        self._apply_sort_and_filter()
        self.endResetModel()

    def set_source_filter(
        self,
        sources: Sequence[CardSource | str] | None,
    ) -> None:
        """표시할 입력 출처를 지정하며 None이면 전체를 표시한다."""
        parsed = (
            None
            if sources is None
            else frozenset(
                source if isinstance(source, CardSource) else CardSource(source)
                for source in sources
            )
        )
        if parsed == self._source_filter:
            return
        self.beginResetModel()
        self._source_filter = parsed
        self._apply_sort_and_filter()
        self.endResetModel()

    def set_dirty_draft_ids(self, card_ids: frozenset[str]) -> None:
        """dirty draft 표시가 달라진 카드 행만 다시 그린다."""
        changed_ids = self._dirty_draft_ids.symmetric_difference(card_ids)
        self._dirty_draft_ids = card_ids
        for card_id in changed_ids:
            index = self.index_for_card(card_id)
            if not index.isValid():
                continue
            self.dataChanged.emit(
                index,
                index,
                [int(CardRole.DIRTY_DRAFT)],
            )

    def set_card_dirty(self, card_id: str, dirty: bool) -> None:
        """카드 하나의 dirty draft 표시만 바꾼다."""
        changed_ids = set(self._dirty_draft_ids)
        if dirty:
            changed_ids.add(card_id)
        else:
            changed_ids.discard(card_id)
        self.set_dirty_draft_ids(frozenset(changed_ids))

    def set_preview_line_count(self, line_count: int) -> None:
        """모델을 reset해 설정한 고정 미리보기 줄 수를 적용한다."""
        if line_count < 1:
            raise ValueError("카드 미리보기 줄 수는 1 이상이어야 합니다.")
        if line_count == self._preview_line_count:
            return
        self.beginResetModel()
        self._preview_line_count = line_count
        self._apply_sort_and_filter()
        self.endResetModel()

    def apply_time_display(self, time_format: str, timezone: str) -> None:
        """툴팁의 시간 형식과 표시 시간대를 적용한다."""
        self._time_format = time_format
        self._timezone = timezone
        if self._visible_cards:
            self.dataChanged.emit(
                self.index(0),
                self.index(len(self._visible_cards) - 1),
                [int(Qt.ItemDataRole.ToolTipRole)],
            )

    def set_reconstruction_unavailable_ids(self, card_ids: frozenset[str]) -> None:
        """purge된 작업 원문 때문에 재구성할 수 없는 형제 카드를 표시한다."""
        self._reconstruction_unavailable_ids = card_ids
        if self._visible_cards:
            self.dataChanged.emit(
                self.index(0),
                self.index(len(self._visible_cards) - 1),
                [
                    int(CardRole.RECONSTRUCTION_AVAILABLE),
                    int(Qt.ItemDataRole.ToolTipRole),
                    int(Qt.ItemDataRole.SizeHintRole),
                ],
            )

    def card_at(self, index: QModelIndex) -> Card | None:
        """모델 인덱스의 도메인 카드를 반환한다."""
        if not index.isValid() or index.row() >= len(self._visible_cards):
            return None
        return self._visible_cards[index.row()]

    def index_for_card(self, card_id: str) -> QModelIndex:
        """현재 표시 행에서 카드 ID를 찾는다."""
        for row, card in enumerate(self._visible_cards):
            if card.id == card_id:
                return self.index(row)
        return QModelIndex()

    def _rebuild_metadata(self) -> None:
        position_order = sorted(
            self._all_cards,
            key=lambda card: (card.position_key, card.id),
        )
        self._position_numbers = {
            card.id: index + 1 for index, card in enumerate(position_order)
        }
    def _apply_sort_and_filter(self) -> None:
        self._visible_cards = self._sorted_and_filtered_cards()

    def _sorted_and_filtered_cards(self) -> tuple[Card, ...]:
        cards = self._all_cards
        if self._source_filter is not None:
            cards = tuple(card for card in cards if card.source in self._source_filter)
        if self._sort_mode == "capture":
            return tuple(sorted(cards, key=lambda card: (card.capture_seq, card.id)))
        if self._sort_mode == "recency":
            return tuple(
                sorted(
                    cards,
                    key=lambda card: (card.updated_at_us, card.capture_seq),
                    reverse=True,
                )
            )
        return tuple(sorted(cards, key=lambda card: (card.position_key, card.id)))

    def _preview_budget(self) -> int:
        # +1은 delegate가 overflow를 판정할 때 쓰는 여분 한 줄이다.
        return (self._preview_line_count + 1) * PREVIEW_CODEPOINT_BUDGET_PER_LINE

    def _preview(self, card: Card) -> str:
        # 코드 포인트 단위 슬라이싱이라 서로게이트 쌍이 쪼개지지 않는다. 원문은
        # BODY와 DisplayRole이 그대로 보존한다.
        return card.body[: self._preview_budget()]

    @staticmethod
    def _source_label(source: str) -> str:
        return {
            "typing": "직접 입력",
            "paste": "붙여넣기",
            "mixed": "혼합",
            "import": "가져오기",
            "restore": "복구",
            "split": "분할",
            "merge": "병합",
            "system": "시스템",
        }.get(source, source)

    def _time_label(self, epoch_us: int) -> str:
        date_time = QDateTime.fromMSecsSinceEpoch(epoch_us // 1_000, QTimeZone.utc())
        if self._timezone == "system":
            displayed = date_time.toLocalTime()
        elif self._timezone == "UTC":
            displayed = date_time.toTimeZone(QTimeZone.utc())
        else:
            zone = QTimeZone(self._timezone.encode("utf-8"))
            displayed = date_time.toTimeZone(zone) if zone.isValid() else date_time.toLocalTime()
        return displayed.toString(self._time_format)
