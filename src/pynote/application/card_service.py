from __future__ import annotations

import json
import logging
import time
import uuid
from collections.abc import Callable, Sequence
from dataclasses import replace

from pynote.domain.events import EditEvent, EventSource, EventType
from pynote.domain.models import (
    CaptureOperation,
    CaptureOperationSource,
    Card,
    CardRevision,
    CardSource,
    RevisionSource,
    SplitPolicy,
)
from pynote.domain.paragraph_parser import ParagraphParser
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import (
    CardCompareAndSwapError,
    Repositories,
    text_hash,
)

LOGGER = logging.getLogger(__name__)

POSITION_STEP = 1_024

Clock = Callable[[], int]
IdFactory = Callable[[], str]


class CardService:
    """새 카드 생성과 현재 문서 순서 변경을 원자적으로 처리한다."""

    def __init__(
        self,
        database: Database,
        repositories: Repositories | None = None,
        *,
        parser: ParagraphParser | None = None,
        clock: Clock | None = None,
        id_factory: IdFactory | None = None,
    ) -> None:
        self._database = database
        self._repositories = repositories or Repositories(database)
        self._parser = parser or ParagraphParser()
        self._clock = clock or (lambda: time.time_ns() // 1_000)
        self._id_factory = id_factory or (lambda: str(uuid.uuid4()))

    def create_cards(
        self,
        document_id: str,
        text: str,
        *,
        source: CaptureOperationSource = CaptureOperationSource.TYPING,
        split: bool = False,
        before_card_id: str | None = None,
    ) -> tuple[Card, ...]:
        """입력을 한 카드 또는 빈 줄 기준 여러 카드로 확정한다."""
        paragraphs = self._parser.split(text)
        if not paragraphs:
            raise ValueError("빈 문자열 또는 공백만 있는 입력은 저장할 수 없습니다.")
        if self._repositories.get_document(document_id) is None:
            raise KeyError(f"존재하지 않는 문서입니다: {document_id}")
        if source not in {
            CaptureOperationSource.TYPING,
            CaptureOperationSource.PASTE,
            CaptureOperationSource.IMPORT,
            CaptureOperationSource.MIXED,
            CaptureOperationSource.SYSTEM,
        }:
            raise ValueError(f"새 입력에 사용할 수 없는 출처입니다: {source.value}")

        should_split = split and len(paragraphs) >= 2
        bodies = paragraphs if should_split else (self._parser.keep(text),)
        split_policy = (
            SplitPolicy.SPLIT_BY_BLANK_LINE if should_split else SplitPolicy.KEEP
        )
        created_at_us = self._clock()
        operation_id = self._id_factory()
        card_source = CardSource(source.value)
        event_source = EventSource(source.value)

        try:
            with self._database.transaction():
                active_cards = self._active_cards(document_id)
                insert_index = self._insertion_index(
                    active_cards,
                    before_card_id=before_card_id,
                )
                position_keys = self._allocate_position_keys(
                    active_cards,
                    insert_index,
                    len(bodies),
                )
                operation = CaptureOperation(
                    id=operation_id,
                    document_id=document_id,
                    source=source,
                    split_policy=split_policy,
                    original_text=text if should_split else None,
                    original_hash=text_hash(text) if should_split else None,
                    original_redacted_at_us=None,
                    created_at_us=created_at_us,
                )
                self._repositories.create_capture_operation(operation)

                created: list[Card] = []
                for body, position_key in zip(bodies, position_keys, strict=True):
                    created.append(
                        self._create_card_rows(
                            document_id=document_id,
                            operation_id=operation_id,
                            body=body,
                            source=card_source,
                            event_source=event_source,
                            position_key=position_key,
                            created_at_us=created_at_us,
                        )
                    )
                self._repositories.touch_document(document_id, created_at_us)
                return tuple(created)
        except BaseException:
            LOGGER.exception("새 카드 저장에 실패했습니다.")
            raise

    def create_card(
        self,
        document_id: str,
        text: str,
        *,
        source: CaptureOperationSource = CaptureOperationSource.TYPING,
        before_card_id: str | None = None,
    ) -> Card:
        """분할하지 않은 새 카드 한 장을 확정한다."""
        return self.create_cards(
            document_id,
            text,
            source=source,
            split=False,
            before_card_id=before_card_id,
        )[0]

    def move_card(self, card_id: str, *, before_card_id: str | None = None) -> Card:
        """카드를 대상 카드 앞 또는 문서 끝으로 옮기고 move 이벤트를 남긴다."""
        initial_card = self._require_active_card(card_id)
        expected_revision_id = self._revision_id(initial_card)
        if before_card_id == card_id:
            return initial_card

        occurred_at_us = self._clock()
        try:
            with self._database.transaction():
                card = self._require_active_card(card_id)
                if card.current_revision_id != expected_revision_id:
                    raise CardCompareAndSwapError(
                        f"이동 기준 리비전이 변경되었습니다: {card_id}"
                    )
                active_cards = list(self._active_cards(card.document_id))
                original_order = [item.id for item in active_cards]
                remaining = [item for item in active_cards if item.id != card_id]
                insert_index = self._insertion_index(
                    remaining,
                    before_card_id=before_card_id,
                )
                final_order = [item.id for item in remaining]
                final_order.insert(insert_index, card_id)
                if final_order == original_order:
                    return card
                temporary_key = min(item.position_key for item in active_cards) - POSITION_STEP
                self._repositories.update_card_position(
                    card.id,
                    temporary_key,
                    expected_revision_id=expected_revision_id,
                )
                position_key = self._allocate_position_keys(remaining, insert_index, 1)[0]
                left_id = remaining[insert_index - 1].id if insert_index else None
                right_id = (
                    remaining[insert_index].id if insert_index < len(remaining) else None
                )
                moved = replace(card, position_key=position_key)
                self._repositories.create_event(
                    EditEvent(
                        event_seq=None,
                        event_id=self._id_factory(),
                        operation_id=None,
                        document_id=card.document_id,
                        card_id=card.id,
                        event_type=EventType.MOVE,
                        source=EventSource.SYSTEM,
                        occurred_at_us=occurred_at_us,
                        details_json=json.dumps(
                            {
                                "old_position_key": card.position_key,
                                "new_position_key": position_key,
                                "left_neighbor_id": left_id,
                                "right_neighbor_id": right_id,
                            },
                            ensure_ascii=False,
                        ),
                    )
                )
                self._repositories.update_card_position(
                    card.id,
                    position_key,
                    expected_revision_id=expected_revision_id,
                )
                self._repositories.touch_document(card.document_id, occurred_at_us)
                return moved
        except BaseException:
            LOGGER.exception("카드 이동에 실패했습니다: %s", card_id)
            raise

    def soft_delete(
        self,
        card_id: str,
        *,
        expected_revision_id: str | None = None,
        require_empty_body: bool = False,
        discard_draft_id: str | None = None,
    ) -> Card:
        """카드를 휴지통 상태로 바꾸고 삭제 시점의 좌우 이웃을 기록한다."""
        initial_card = self._require_active_card(card_id)
        delete_revision_id = (
            self._revision_id(initial_card)
            if expected_revision_id is None
            else expected_revision_id
        )
        deleted_at_us = self._clock()

        try:
            with self._database.transaction():
                card = self._require_active_card(card_id)
                if card.current_revision_id != delete_revision_id:
                    raise CardCompareAndSwapError(
                        f"삭제 기준 리비전이 변경되었습니다: {card_id}"
                    )
                if require_empty_body and card.body.strip():
                    raise CardCompareAndSwapError(
                        f"삭제 기준 본문이 비어 있지 않습니다: {card_id}"
                    )
                active_cards = self._active_cards(card.document_id)
                index = next(
                    index for index, item in enumerate(active_cards) if item.id == card_id
                )
                left_id = active_cards[index - 1].id if index else None
                right_id = (
                    active_cards[index + 1].id
                    if index + 1 < len(active_cards)
                    else None
                )
                deleted = replace(card, deleted_at_us=deleted_at_us)
                self._repositories.create_event(
                    EditEvent(
                        event_seq=None,
                        event_id=self._id_factory(),
                        operation_id=None,
                        document_id=card.document_id,
                        card_id=card.id,
                        event_type=EventType.DELETE,
                        source=EventSource.SYSTEM,
                        occurred_at_us=deleted_at_us,
                        details_json=json.dumps(
                            {
                                "position_key": card.position_key,
                                "left_neighbor_id": left_id,
                                "right_neighbor_id": right_id,
                            },
                            ensure_ascii=False,
                        ),
                    )
                )
                self._repositories.update_card_deleted_state(
                    card.id,
                    position_key=card.position_key,
                    deleted_at_us=deleted_at_us,
                    expected_revision_id=delete_revision_id,
                )
                if discard_draft_id is not None:
                    self._repositories.delete_draft(discard_draft_id)
                self._repositories.touch_document(card.document_id, deleted_at_us)
                return deleted
        except BaseException:
            LOGGER.exception("카드 soft-delete에 실패했습니다: %s", card_id)
            raise

    def restore_card(self, card_id: str) -> Card:
        """휴지통 카드를 이웃 힌트 기준으로 복구하고 restore 이벤트를 남긴다."""
        card = self._repositories.get_card(card_id)
        if card is None or card.deleted_at_us is None:
            raise KeyError(f"휴지통 카드가 아닙니다: {card_id}")
        expected_revision_id = self._revision_id(card)
        restored_at_us = self._clock()
        try:
            with self._database.transaction():
                card = self._repositories.get_card(card_id)
                if card is None or card.deleted_at_us is None:
                    raise KeyError(f"휴지통 카드가 아닙니다: {card_id}")
                if card.current_revision_id != expected_revision_id:
                    raise CardCompareAndSwapError(
                        f"휴지통 복구 기준 리비전이 변경되었습니다: {card_id}"
                    )
                active_cards = list(self._active_cards(card.document_id))
                event_row = self._database.connection.execute(
                    """
                    SELECT details_json
                    FROM edit_events
                    WHERE card_id = ? AND event_type = 'delete'
                    ORDER BY event_seq DESC
                    LIMIT 1
                    """,
                    (card_id,),
                ).fetchone()
                details = {} if event_row is None else json.loads(str(event_row[0]))
                right_id = details.get("right_neighbor_id")
                left_id = details.get("left_neighbor_id")
                insert_index = len(active_cards)
                if isinstance(right_id, str):
                    right_index = next(
                        (
                            index
                            for index, item in enumerate(active_cards)
                            if item.id == right_id
                        ),
                        None,
                    )
                    if right_index is not None:
                        insert_index = right_index
                if insert_index == len(active_cards) and isinstance(left_id, str):
                    left_index = next(
                        (
                            index
                            for index, item in enumerate(active_cards)
                            if item.id == left_id
                        ),
                        None,
                    )
                    if left_index is not None:
                        insert_index = left_index + 1
                position_key = self._allocate_position_keys(
                    active_cards,
                    insert_index,
                    1,
                )[0]
                restored = replace(card, position_key=position_key, deleted_at_us=None)
                self._repositories.create_event(
                    EditEvent(
                        event_seq=None,
                        event_id=self._id_factory(),
                        operation_id=None,
                        document_id=card.document_id,
                        card_id=card.id,
                        event_type=EventType.RESTORE,
                        source=EventSource.RESTORE,
                        occurred_at_us=restored_at_us,
                        details_json=json.dumps(
                            {
                                "restored_position_key": position_key,
                                "left_neighbor_id": left_id,
                                "right_neighbor_id": right_id,
                            },
                            ensure_ascii=False,
                        ),
                    )
                )
                self._repositories.update_card_deleted_state(
                    card.id,
                    position_key=position_key,
                    deleted_at_us=None,
                    expected_revision_id=expected_revision_id,
                )
                self._repositories.touch_document(card.document_id, restored_at_us)
        except BaseException:
            LOGGER.exception("카드 휴지통 복구에 실패했습니다: %s", card_id)
            raise
        return restored

    def list_active_cards(
        self,
        document_id: str,
        *,
        sort_mode: str = "recency",
    ) -> tuple[Card, ...]:
        """삭제되지 않은 카드를 지정한 보기 순서로 반환한다."""
        if sort_mode not in {"recency", "position", "capture"}:
            raise ValueError(f"지원하지 않는 정렬 모드입니다: {sort_mode}")
        cards = self._active_cards(document_id)
        if sort_mode == "capture":
            return tuple(sorted(cards, key=lambda card: (card.capture_seq, card.id)))
        if sort_mode == "recency":
            return tuple(
                sorted(
                    cards,
                    key=lambda card: (card.updated_at_us, card.capture_seq),
                    reverse=True,
                )
            )
        return cards

    def _create_card_rows(
        self,
        *,
        document_id: str,
        operation_id: str,
        body: str,
        source: CardSource,
        event_source: EventSource,
        position_key: int,
        created_at_us: int,
    ) -> Card:
        card_id = self._id_factory()
        revision_id = self._id_factory()
        event = self._repositories.create_event(
            EditEvent(
                event_seq=None,
                event_id=self._id_factory(),
                operation_id=operation_id,
                document_id=document_id,
                card_id=card_id,
                event_type=EventType.CREATE,
                source=event_source,
                occurred_at_us=created_at_us,
                details_json=json.dumps(
                    {"position_key": position_key},
                    ensure_ascii=False,
                ),
            )
        )
        if event.event_seq is None:
            raise RuntimeError("create 이벤트의 event_seq가 없습니다.")

        capture_seq = self._repositories._issue_capture_sequence()
        body_hash = text_hash(body)
        pending = Card(
            id=card_id,
            document_id=document_id,
            operation_id=operation_id,
            position_key=position_key,
            capture_seq=capture_seq,
            created_at_us=created_at_us,
            updated_at_us=created_at_us,
            source=source,
            body=body,
            body_hash=body_hash,
            current_revision_id=None,
        )
        self._repositories.create_card(pending)
        self._repositories.create_revision(
            CardRevision(
                id=revision_id,
                card_id=card_id,
                event_seq=event.event_seq,
                parent_revision_id=None,
                body=body,
                body_hash=body_hash,
                source=RevisionSource.EDIT,
                created_at_us=created_at_us,
            )
        )
        stored = replace(pending, current_revision_id=revision_id)
        self._repositories.link_initial_revision(card_id, revision_id)
        return stored

    def _active_cards(self, document_id: str) -> tuple[Card, ...]:
        return tuple(
            card
            for card in self._repositories.list_cards(document_id)
            if card.deleted_at_us is None
        )

    def _require_active_card(self, card_id: str) -> Card:
        card = self._repositories.get_card(card_id)
        if card is None or card.deleted_at_us is not None:
            raise KeyError(f"활성 카드가 아닙니다: {card_id}")
        return card

    @staticmethod
    def _revision_id(card: Card) -> str:
        if card.current_revision_id is None:
            raise RuntimeError(f"카드의 현재 리비전이 없습니다: {card.id}")
        return card.current_revision_id

    @staticmethod
    def _insertion_index(
        cards: Sequence[Card],
        *,
        before_card_id: str | None,
    ) -> int:
        if before_card_id is None:
            return len(cards)
        for index, card in enumerate(cards):
            if card.id == before_card_id:
                return index
        raise KeyError(f"삽입 기준 카드가 현재 문서에 없습니다: {before_card_id}")

    def _allocate_position_keys(
        self,
        cards: Sequence[Card],
        insert_index: int,
        count: int,
    ) -> tuple[int, ...]:
        if count < 1:
            raise ValueError("position key는 한 개 이상 할당해야 합니다.")
        working_cards = list(cards)
        if not working_cards:
            return tuple(POSITION_STEP * (index + 1) for index in range(count))

        left = working_cards[insert_index - 1].position_key if insert_index else 0
        if insert_index < len(working_cards):
            right = working_cards[insert_index].position_key
        else:
            right = left + POSITION_STEP * (count + 1)
        step = (right - left) // (count + 1)
        if step < 1:
            self._rebalance(working_cards)
            left = working_cards[insert_index - 1].position_key if insert_index else 0
            if insert_index < len(working_cards):
                right = working_cards[insert_index].position_key
            else:
                right = left + POSITION_STEP * (count + 1)
            step = (right - left) // (count + 1)
        return tuple(left + step * (index + 1) for index in range(count))

    def _rebalance(self, cards: list[Card]) -> None:
        if not cards:
            return
        minimum = min(card.position_key for card in cards)
        temporary_start = minimum - len(cards) - 1
        for index, card in enumerate(cards):
            self._repositories.update_card_position(
                card.id,
                temporary_start + index,
                expected_revision_id=self._revision_id(card),
            )
        for index, card in enumerate(cards):
            new_position = POSITION_STEP * (index + 1)
            self._repositories.update_card_position(
                card.id,
                new_position,
                expected_revision_id=self._revision_id(card),
            )
            cards[index] = replace(card, position_key=new_position)
