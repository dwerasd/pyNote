from __future__ import annotations

import json
import logging
import time
import uuid
from collections.abc import Callable
from dataclasses import dataclass, replace
from enum import StrEnum

from pynote.application.draft_coordinator import DraftCoordinator, DraftSession
from pynote.domain.events import EditEvent, EventSource, EventType
from pynote.domain.models import Card, CardRevision, RevisionSource
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories, text_hash

LOGGER = logging.getLogger(__name__)

Clock = Callable[[], int]
IdFactory = Callable[[], str]


class SaveOutcome(StrEnum):
    """명시적 카드 저장의 결과 종류다."""

    SAVED = "saved"
    UNCHANGED = "unchanged"
    CONFLICT = "conflict"


@dataclass(frozen=True, slots=True)
class SaveConflict:
    """비교 화면에 표시할 base revision 불일치 정보다."""

    card_id: str
    base_revision_id: str | None
    current_revision_id: str | None
    base_text: str
    committed_text: str
    draft_text: str


@dataclass(frozen=True, slots=True)
class SaveResult:
    """DB 확정 완료 여부와 비교 안내를 함께 반환한다."""

    outcome: SaveOutcome
    card: Card
    conflict: SaveConflict | None = None


class ImeCompositionInProgressError(RuntimeError):
    """IME 조합 중 명시적 저장을 거부했음을 나타낸다."""


class SaveCoordinator:
    """base revision 검사와 카드 수정 트랜잭션을 동기 실행한다."""

    def __init__(
        self,
        database: Database,
        draft_coordinator: DraftCoordinator,
        repositories: Repositories | None = None,
        *,
        clock: Clock | None = None,
        id_factory: IdFactory | None = None,
    ) -> None:
        self._database = database
        self._draft_coordinator = draft_coordinator
        self._repositories = repositories or Repositories(database)
        self._clock = clock or (lambda: time.time_ns() // 1_000)
        self._id_factory = id_factory or (lambda: str(uuid.uuid4()))

    def save(self, session: DraftSession) -> SaveResult:
        """Ctrl+S 경계에서 변경된 카드만 새 리비전으로 확정한다."""
        if self._draft_coordinator.is_ime_composing(session.draft_id):
            raise ImeCompositionInProgressError(
                "한글 IME 조합을 확정한 뒤 다시 저장해 주세요."
            )
        if session.card_id is None:
            raise ValueError("카드 편집 저장에는 card_id가 필요합니다.")
        initial_card = self._repositories.get_card(session.card_id)
        if initial_card is None or initial_card.deleted_at_us is not None:
            raise KeyError(f"활성 카드가 아닙니다: {session.card_id}")
        self._draft_coordinator.protect_now(session.draft_id)

        draft_hash = text_hash(session.text)
        conflict: SaveConflict | None = None
        saved_card: Card | None = None
        outcome = SaveOutcome.SAVED
        try:
            with self._database.transaction():
                card = self._repositories.get_card(session.card_id)
                if card is None or card.deleted_at_us is not None:
                    raise KeyError(f"활성 카드가 아닙니다: {session.card_id}")
                if session.base_revision_id != card.current_revision_id:
                    conflict = self._conflict(session, card)
                    outcome = SaveOutcome.CONFLICT
                    saved_card = card
                elif draft_hash == card.body_hash:
                    if self._repositories.get_draft(session.draft_id) is not None:
                        self._repositories.delete_draft(session.draft_id)
                    outcome = SaveOutcome.UNCHANGED
                    saved_card = card
                else:
                    if card.current_revision_id is None:
                        raise RuntimeError(f"카드의 현재 리비전이 없습니다: {card.id}")
                    saved_at_us = self._clock()
                    revision_id = self._id_factory()
                    event = self._repositories.create_event(
                        EditEvent(
                            event_seq=None,
                            event_id=self._id_factory(),
                            operation_id=None,
                            document_id=card.document_id,
                            card_id=card.id,
                            event_type=EventType.UPDATE,
                            source=EventSource.EDIT,
                            occurred_at_us=saved_at_us,
                            details_json=json.dumps(
                                {
                                    "base_revision_id": session.base_revision_id,
                                    "includes_paste": (
                                        self._draft_coordinator.includes_paste(
                                            session.draft_id
                                        )
                                    ),
                                },
                                ensure_ascii=False,
                                separators=(",", ":"),
                            ),
                        )
                    )
                    if event.event_seq is None:
                        raise RuntimeError("update 이벤트의 event_seq가 없습니다.")
                    self._repositories.create_revision(
                        CardRevision(
                            id=revision_id,
                            card_id=card.id,
                            event_seq=event.event_seq,
                            parent_revision_id=card.current_revision_id,
                            body=session.text,
                            body_hash=draft_hash,
                            source=RevisionSource.EDIT,
                            created_at_us=saved_at_us,
                        )
                    )
                    saved_card = replace(
                        card,
                        body=session.text,
                        body_hash=draft_hash,
                        current_revision_id=revision_id,
                        updated_at_us=saved_at_us,
                    )
                    self._repositories.advance_card_revision(
                        saved_card,
                        expected_revision_id=card.current_revision_id,
                    )
                    if self._repositories.get_draft(session.draft_id) is not None:
                        self._repositories.delete_draft(session.draft_id)
                    self._repositories.touch_document(card.document_id, saved_at_us)
        except BaseException:
            LOGGER.exception("카드 저장에 실패했습니다: %s", session.card_id)
            raise

        if saved_card is None:
            raise RuntimeError("카드 저장 결과가 없습니다.")
        if outcome is SaveOutcome.CONFLICT:
            return SaveResult(outcome=outcome, card=saved_card, conflict=conflict)
        self._draft_coordinator.complete_save(
            session.draft_id,
            committed_text=saved_card.body,
            revision_id=saved_card.current_revision_id,
        )
        return SaveResult(outcome=outcome, card=saved_card)

    def _conflict(self, session: DraftSession, card: Card) -> SaveConflict:
        base_revision = (
            None
            if session.base_revision_id is None
            else self._repositories.get_revision(session.base_revision_id)
        )
        return SaveConflict(
            card_id=card.id,
            base_revision_id=session.base_revision_id,
            current_revision_id=card.current_revision_id,
            base_text="" if base_revision is None else base_revision.body,
            committed_text=card.body,
            draft_text=session.text,
        )
