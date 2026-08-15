from __future__ import annotations

from collections.abc import Callable, Iterator
from dataclasses import replace

import pytest

from pynote.application.card_service import CardService
from pynote.application.draft_coordinator import DraftCoordinator
from pynote.application.history_service import HistoryService
from pynote.application.save_coordinator import SaveCoordinator, SaveOutcome
from pynote.domain.events import EditEvent, EventSource, EventType
from pynote.domain.models import Card, CardRevision, Document, RevisionSource
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import (
    CardCompareAndSwapError,
    Repositories,
    text_hash,
)


def _ids(prefix: str) -> Iterator[str]:
    number = 0
    while True:
        number += 1
        yield f"{prefix}-{number}"


def _create_cards(
    database: Database,
    repositories: Repositories,
    *bodies: str,
) -> tuple[Card, ...]:
    repositories.create_document(
        Document(
            id="document-race",
            title="동시성 테스트",
            created_at_us=1_000,
            updated_at_us=1_000,
        )
    )
    identifiers = _ids("create")
    service = CardService(
        database,
        repositories,
        clock=lambda: 2_000,
        id_factory=lambda: next(identifiers),
    )
    return tuple(service.create_card("document-race", body) for body in bodies)


def _commit_body(
    database: Database,
    repositories: Repositories,
    card_id: str,
    body: str,
    *,
    suffix: str,
) -> Card:
    with database.transaction():
        card = repositories.get_card(card_id)
        assert card is not None
        assert card.current_revision_id is not None
        event = repositories.create_event(
            EditEvent(
                event_seq=None,
                event_id=f"event-{suffix}",
                operation_id=None,
                document_id=card.document_id,
                card_id=card.id,
                event_type=EventType.UPDATE,
                source=EventSource.EDIT,
                occurred_at_us=10_000,
                details_json="{}",
            )
        )
        assert event.event_seq is not None
        revision_id = f"revision-{suffix}"
        repositories.create_revision(
            CardRevision(
                id=revision_id,
                card_id=card.id,
                event_seq=event.event_seq,
                parent_revision_id=card.current_revision_id,
                body=body,
                body_hash=text_hash(body),
                source=RevisionSource.EDIT,
                created_at_us=10_000,
            )
        )
        updated = replace(
            card,
            body=body,
            body_hash=text_hash(body),
            current_revision_id=revision_id,
            updated_at_us=10_000,
        )
        repositories.advance_card_revision(
            updated,
            expected_revision_id=card.current_revision_id,
        )
        return updated


class _CommitAfterReadRepositories(Repositories):
    def __init__(self, database: Database, callback: Callable[[], object]) -> None:
        super().__init__(database)
        self._callback = callback
        self._fired = False

    def get_card(self, card_id: str) -> Card | None:
        card = super().get_card(card_id)
        if not self.database.connection.in_transaction and not self._fired:
            self._fired = True
            self._callback()
        return card


def test_save_rereads_base_after_begin_and_preserves_competing_commit(
    database: Database,
    repositories: Repositories,
) -> None:
    (card,) = _create_cards(database, repositories, "초기 본문")
    draft = DraftCoordinator(
        database,
        repositories,
        id_factory=lambda: "draft-race",
    )
    session = draft.open_card(card)
    assert session is not None
    draft.update_session(
        session.draft_id,
        text="오래된 저장",
        cursor_position_qchar=0,
    )

    with Database(database.path) as other_database:
        other_repositories = Repositories(other_database)
        racing_repositories = _CommitAfterReadRepositories(
            database,
            lambda: _commit_body(
                other_database,
                other_repositories,
                card.id,
                "경쟁 저장",
                suffix="save-race",
            ),
        )
        result = SaveCoordinator(
            database,
            draft,
            racing_repositories,
        ).save(session)

    assert result.outcome is SaveOutcome.CONFLICT
    stored = repositories.get_card(card.id)
    assert stored is not None
    assert stored.body == "경쟁 저장"
    assert len(repositories.list_revisions(card.id)) == 2


def test_history_restore_rejects_base_changed_by_separate_connection(
    database: Database,
    repositories: Repositories,
) -> None:
    (initial,) = _create_cards(database, repositories, "복구 대상")
    current = _commit_body(
        database,
        repositories,
        initial.id,
        "현재 본문",
        suffix="before-restore",
    )

    with Database(database.path) as other_database:
        other_repositories = Repositories(other_database)
        history = HistoryService(
            database,
            repositories,
            clock=lambda: (
                _commit_body(
                    other_database,
                    other_repositories,
                    current.id,
                    "경쟁 저장",
                    suffix="restore-race",
                ),
                20_000,
            )[1],
        )
        with pytest.raises(CardCompareAndSwapError, match="복구 기준"):
            history.restore(initial.id, initial.current_revision_id or "")

    stored = repositories.get_card(initial.id)
    assert stored is not None
    assert stored.body == "경쟁 저장"
    assert not any(
        event.event_type is EventType.RESTORE
        for event in repositories.list_events(initial.document_id)
    )


def test_move_rejects_base_changed_by_separate_connection(
    database: Database,
    repositories: Repositories,
) -> None:
    first, second = _create_cards(database, repositories, "A", "B")

    with Database(database.path) as other_database:
        other_repositories = Repositories(other_database)
        service = CardService(
            database,
            repositories,
            clock=lambda: (
                _commit_body(
                    other_database,
                    other_repositories,
                    second.id,
                    "B 경쟁 저장",
                    suffix="move-race",
                ),
                20_000,
            )[1],
        )
        with pytest.raises(CardCompareAndSwapError, match="이동 기준"):
            service.move_card(second.id, before_card_id=first.id)

    assert [
        card.id
        for card in service.list_active_cards(
            first.document_id,
            sort_mode="position",
        )
    ] == [
        first.id,
        second.id,
    ]
    assert not any(
        event.event_type is EventType.MOVE for event in repositories.list_events(first.document_id)
    )


def test_empty_cleanup_delete_rejects_concurrent_nonempty_commit(
    database: Database,
    repositories: Repositories,
) -> None:
    (initial,) = _create_cards(database, repositories, "비울 본문")
    empty = _commit_body(
        database,
        repositories,
        initial.id,
        "",
        suffix="empty-before-cleanup",
    )
    assert empty.current_revision_id is not None

    with Database(database.path) as other_database:
        competing = _commit_body(
            other_database,
            Repositories(other_database),
            empty.id,
            "정리 직전 경쟁 저장",
            suffix="cleanup-race",
        )
        with pytest.raises(CardCompareAndSwapError, match="삭제 기준 리비전"):
            CardService(database, repositories).soft_delete(
                empty.id,
                expected_revision_id=empty.current_revision_id,
                require_empty_body=True,
            )

    stored = repositories.get_card(empty.id)
    assert stored is not None
    assert stored.body == competing.body
    assert stored.current_revision_id == competing.current_revision_id
    assert stored.deleted_at_us is None
    assert not any(
        event.event_type is EventType.DELETE
        for event in repositories.list_events(initial.document_id)
    )
