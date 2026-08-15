from __future__ import annotations

import json
import logging
import time
import uuid
from collections.abc import Callable
from dataclasses import dataclass, replace

from pynote.domain.diffing import TextDiff, diff_text
from pynote.domain.events import EditEvent, EventSource, EventType
from pynote.domain.models import Card, CardRevision, CardSource, RevisionSource
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import (
    CardCompareAndSwapError,
    Repositories,
    text_hash,
)

LOGGER = logging.getLogger(__name__)

Clock = Callable[[], int]
IdFactory = Callable[[], str]


@dataclass(frozen=True, slots=True)
class RestoreResult:
    """복구 트랜잭션이 확정한 카드, 이벤트와 새 리비전이다."""

    card: Card
    event: EditEvent
    revision: CardRevision


class HistoryService:
    """문서 이벤트와 영구 카드 리비전의 조회·비교·복구를 제공한다."""

    def __init__(
        self,
        database: Database,
        repositories: Repositories | None = None,
        *,
        clock: Clock | None = None,
        id_factory: IdFactory | None = None,
    ) -> None:
        self._database = database
        self._repositories = repositories or Repositories(database)
        self._clock = clock or (lambda: time.time_ns() // 1_000)
        self._id_factory = id_factory or (lambda: str(uuid.uuid4()))

    def list_document_events(self, document_id: str) -> tuple[EditEvent, ...]:
        """문서 전체 이벤트를 최신 확정 순번부터 반환한다."""
        if self._repositories.get_document(document_id) is None:
            raise KeyError(f"존재하지 않는 문서입니다: {document_id}")
        return tuple(reversed(self._repositories.list_events(document_id)))

    def list_card_revisions(self, card_id: str) -> tuple[CardRevision, ...]:
        """카드 리비전을 최신 확정 순번부터 반환한다."""
        if self._repositories.get_card(card_id) is None:
            raise KeyError(f"존재하지 않는 카드입니다: {card_id}")
        return tuple(reversed(self._repositories.list_revisions(card_id)))

    def get_revision(self, revision_id: str) -> CardRevision:
        """리비전 전체 문자열을 조회한다."""
        revision = self._repositories.get_revision(revision_id)
        if revision is None:
            raise KeyError(f"존재하지 않는 리비전입니다: {revision_id}")
        return revision

    def diff_revisions(
        self,
        before_revision_id: str,
        after_revision_id: str,
    ) -> TextDiff:
        """같은 카드의 두 영구 리비전을 줄·글자 단위로 비교한다."""
        before = self.get_revision(before_revision_id)
        after = self.get_revision(after_revision_id)
        if before.card_id != after.card_id:
            raise ValueError("서로 다른 카드의 리비전은 비교할 수 없습니다.")
        return diff_text(before.body, after.body)

    def restore(self, card_id: str, target_revision_id: str) -> RestoreResult:
        """현재 리비전을 부모로 과거 본문을 복사한 새 restore 리비전을 만든다."""
        card = self._repositories.get_card(card_id)
        if card is None or card.deleted_at_us is not None:
            raise KeyError(f"활성 카드가 아닙니다: {card_id}")
        target = self.get_revision(target_revision_id)
        if target.card_id != card.id:
            raise ValueError("다른 카드의 리비전으로 복구할 수 없습니다.")
        if target.id == card.current_revision_id:
            raise ValueError("현재 리비전은 복구 대상이 될 수 없습니다.")
        self.assert_card_revision_invariant(card.id)
        if text_hash(target.body) != target.body_hash:
            raise RuntimeError(f"복구 대상 리비전의 해시가 일치하지 않습니다: {target.id}")
        expected_revision_id = card.current_revision_id
        if expected_revision_id is None:
            raise RuntimeError(f"카드의 현재 리비전이 없습니다: {card.id}")

        restored_at_us = self._clock()
        revision_id = self._id_factory()
        try:
            with self._database.transaction():
                card = self._repositories.get_card(card_id)
                if card is None or card.deleted_at_us is not None:
                    raise KeyError(f"활성 카드가 아닙니다: {card_id}")
                if card.current_revision_id != expected_revision_id:
                    raise CardCompareAndSwapError(
                        f"복구 기준 리비전이 변경되었습니다: {card_id}"
                    )
                target = self.get_revision(target_revision_id)
                if target.card_id != card.id:
                    raise ValueError("다른 카드의 리비전으로 복구할 수 없습니다.")
                if target.id == card.current_revision_id:
                    raise ValueError("현재 리비전은 복구 대상이 될 수 없습니다.")
                self.assert_card_revision_invariant(card.id)
                if text_hash(target.body) != target.body_hash:
                    raise RuntimeError(
                        f"복구 대상 리비전의 해시가 일치하지 않습니다: {target.id}"
                    )
                event = self._repositories.create_event(
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
                                "target_revision_id": target.id,
                                "previous_revision_id": card.current_revision_id,
                            },
                            ensure_ascii=False,
                            separators=(",", ":"),
                        ),
                    )
                )
                if event.event_seq is None:
                    raise RuntimeError("restore 이벤트의 event_seq가 없습니다.")
                revision = CardRevision(
                    id=revision_id,
                    card_id=card.id,
                    event_seq=event.event_seq,
                    parent_revision_id=card.current_revision_id,
                    body=target.body,
                    body_hash=target.body_hash,
                    source=RevisionSource.RESTORE,
                    created_at_us=restored_at_us,
                )
                self._repositories.create_revision(revision)
                restored_card = replace(
                    card,
                    source=CardSource.RESTORE,
                    body=revision.body,
                    body_hash=revision.body_hash,
                    current_revision_id=revision.id,
                    updated_at_us=restored_at_us,
                )
                self._repositories.advance_card_revision(
                    restored_card,
                    expected_revision_id=expected_revision_id,
                )
                self._repositories.touch_document(card.document_id, restored_at_us)
        except BaseException:
            LOGGER.exception(
                "카드 리비전 복구에 실패했습니다: card=%s revision=%s",
                card_id,
                target_revision_id,
            )
            raise

        self.assert_card_revision_invariant(card.id)
        return RestoreResult(card=restored_card, event=event, revision=revision)

    def assert_card_revision_invariant(self, card_id: str) -> None:
        """카드 본문·현재 리비전 본문·해시 일치 불변조건을 검증한다."""
        card = self._repositories.get_card(card_id)
        if card is None:
            raise KeyError(f"존재하지 않는 카드입니다: {card_id}")
        if card.current_revision_id is None:
            raise RuntimeError(f"카드의 현재 리비전이 없습니다: {card_id}")
        revision = self._repositories.get_revision(card.current_revision_id)
        if revision is None or revision.card_id != card.id:
            raise RuntimeError(f"카드의 현재 리비전 연결이 올바르지 않습니다: {card_id}")
        actual_hash = text_hash(card.body)
        if (
            card.body != revision.body
            or card.body_hash != revision.body_hash
            or card.body_hash != actual_hash
        ):
            raise RuntimeError(f"카드와 현재 리비전의 본문·해시가 일치하지 않습니다: {card_id}")
