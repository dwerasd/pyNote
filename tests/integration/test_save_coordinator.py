from __future__ import annotations

import sqlite3
from collections.abc import Iterator

import pytest

from pynote.application.draft_coordinator import DraftCoordinator
from pynote.application.save_coordinator import SaveCoordinator, SaveOutcome
from pynote.domain.events import EventSource
from pynote.domain.models import (
    CaptureOperationSource,
    Card,
    CardSource,
    Document,
    NewCaptureOperation,
    NewCard,
    RevisionSource,
    SplitPolicy,
)
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories


def _ids(prefix: str) -> Iterator[str]:
    number = 0
    while True:
        number += 1
        yield f"{prefix}-{number}"


def _create_card(repositories: Repositories) -> Card:
    repositories.create_document(
        Document(
            id="document-1",
            title="저장 테스트",
            created_at_us=1_000,
            updated_at_us=1_000,
        )
    )
    return repositories.create_cards(
        NewCaptureOperation(
            id="operation-1",
            document_id="document-1",
            source=CaptureOperationSource.TYPING,
            split_policy=SplitPolicy.KEEP,
            original_text=None,
            created_at_us=2_000,
        ),
        [
            NewCard(
                id="card-1",
                revision_id="revision-1",
                event_id="event-1",
                position_key=1_024,
                body="기존 확정 본문",
                card_source=CardSource.TYPING,
                event_source=EventSource.TYPING,
                revision_source=RevisionSource.EDIT,
                created_at_us=2_000,
            )
        ],
    )[0]


def test_save_updates_card_only_after_atomic_commit(
    database: Database,
    repositories: Repositories,
) -> None:
    card = _create_card(repositories)
    draft = DraftCoordinator(
        database,
        repositories,
        clock=lambda: 3_000,
        id_factory=lambda: "draft-1",
    )
    identifiers = _ids("save")
    save = SaveCoordinator(
        database,
        draft,
        repositories,
        clock=lambda: 4_000,
        id_factory=lambda: next(identifiers),
    )
    session = draft.open_card(card)
    assert session is not None
    draft.update_session(
        session.draft_id,
        text="새 확정 본문",
        cursor_position_qchar=7,
    )
    draft.protect_now(session.draft_id)

    before = repositories.get_card(card.id)
    assert before == card
    result = save.save(session)

    assert result.outcome is SaveOutcome.SAVED
    assert result.card.body == "새 확정 본문"
    assert result.card.created_at_us == card.created_at_us
    assert result.card.capture_seq == card.capture_seq
    assert result.card.updated_at_us == 4_000
    assert repositories.get_card(card.id) == result.card
    assert len(repositories.list_revisions(card.id)) == 2
    assert len(repositories.list_events(card.document_id)) == 2
    assert repositories.get_draft(session.draft_id) is None
    assert session.dirty is False


def test_save_failure_preserves_committed_card_and_recovery_draft(
    database: Database,
    repositories: Repositories,
) -> None:
    card = _create_card(repositories)
    draft = DraftCoordinator(
        database,
        repositories,
        clock=lambda: 3_000,
        id_factory=lambda: "draft-failure",
    )
    identifiers = _ids("failure")
    save = SaveCoordinator(
        database,
        draft,
        repositories,
        clock=lambda: 4_000,
        id_factory=lambda: next(identifiers),
    )
    session = draft.open_card(card)
    assert session is not None
    draft.update_session(
        session.draft_id,
        text="실패시킬 본문",
        cursor_position_qchar=6,
    )
    database.connection.execute(
        """
        CREATE TRIGGER fail_card_update
        BEFORE UPDATE ON cards
        WHEN NEW.body = '실패시킬 본문'
        BEGIN
            SELECT RAISE(ABORT, '의도한 저장 실패');
        END
        """
    )

    with pytest.raises(sqlite3.IntegrityError, match="의도한 저장 실패"):
        save.save(session)

    assert repositories.get_card(card.id) == card
    assert len(repositories.list_revisions(card.id)) == 1
    assert len(repositories.list_events(card.document_id)) == 1
    stored_draft = repositories.get_draft(session.draft_id)
    assert stored_draft is not None
    assert stored_draft.draft_text == "실패시킬 본문"
    assert session.dirty is True


def test_same_body_creates_no_revision_or_event_and_removes_draft(
    database: Database,
    repositories: Repositories,
) -> None:
    card = _create_card(repositories)
    draft = DraftCoordinator(
        database,
        repositories,
        clock=lambda: 3_000,
        id_factory=lambda: "draft-same",
    )
    save = SaveCoordinator(database, draft, repositories)
    session = draft.open_card(card)
    assert session is not None
    draft.update_session(
        session.draft_id,
        text="잠깐 변경",
        cursor_position_qchar=3,
    )
    draft.protect_now(session.draft_id)
    draft.update_session(
        session.draft_id,
        text=card.body,
        cursor_position_qchar=0,
    )

    result = save.save(session)

    assert result.outcome is SaveOutcome.UNCHANGED
    assert repositories.get_card(card.id) == card
    assert len(repositories.list_revisions(card.id)) == 1
    assert len(repositories.list_events(card.document_id)) == 1
    assert repositories.get_draft(session.draft_id) is None


def test_base_revision_conflict_returns_comparison_and_stops_save(
    database: Database,
    repositories: Repositories,
) -> None:
    card = _create_card(repositories)
    first_draft = DraftCoordinator(
        database,
        repositories,
        id_factory=lambda: "draft-first",
    )
    second_draft = DraftCoordinator(
        database,
        repositories,
        id_factory=lambda: "draft-second",
    )
    first_session = first_draft.open_card(card)
    second_session = second_draft.open_card(card)
    assert first_session is not None
    assert second_session is not None
    first_draft.update_session(
        first_session.draft_id,
        text="오래된 편집",
        cursor_position_qchar=5,
    )
    second_draft.update_session(
        second_session.draft_id,
        text="먼저 저장된 편집",
        cursor_position_qchar=5,
    )
    second_ids = _ids("second-save")
    second_save = SaveCoordinator(
        database,
        second_draft,
        repositories,
        clock=lambda: 4_000,
        id_factory=lambda: next(second_ids),
    )
    assert second_save.save(second_session).outcome is SaveOutcome.SAVED

    first_save = SaveCoordinator(database, first_draft, repositories)
    result = first_save.save(first_session)

    assert result.outcome is SaveOutcome.CONFLICT
    assert result.conflict is not None
    assert result.conflict.base_text == "기존 확정 본문"
    assert result.conflict.committed_text == "먼저 저장된 편집"
    assert result.conflict.draft_text == "오래된 편집"
    assert len(repositories.list_revisions(card.id)) == 2
    stale_draft = repositories.get_draft(first_session.draft_id)
    assert stale_draft is not None
    assert stale_draft.draft_text == "오래된 편집"
