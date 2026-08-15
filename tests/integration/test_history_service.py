from __future__ import annotations

import json
import sqlite3
from collections.abc import Iterator

import pytest

from pynote.application.draft_coordinator import DraftCoordinator
from pynote.application.history_service import HistoryService
from pynote.application.save_coordinator import SaveCoordinator, SaveOutcome
from pynote.domain.events import EventSource, EventType
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
from pynote.infrastructure.repositories import Repositories, text_hash


def _ids(prefix: str) -> Iterator[str]:
    number = 0
    while True:
        number += 1
        yield f"{prefix}-{number}"


def _create_card(repositories: Repositories, body: str = "첫 본문") -> Card:
    repositories.create_document(
        Document(
            id="document-history",
            title="이력 테스트",
            created_at_us=1_000,
            updated_at_us=1_000,
        )
    )
    return repositories.create_cards(
        NewCaptureOperation(
            id="operation-history",
            document_id="document-history",
            source=CaptureOperationSource.TYPING,
            split_policy=SplitPolicy.KEEP,
            original_text=None,
            created_at_us=2_000,
        ),
        [
            NewCard(
                id="card-history",
                revision_id="revision-initial",
                event_id="event-initial",
                position_key=1_024,
                body=body,
                card_source=CardSource.TYPING,
                event_source=EventSource.TYPING,
                revision_source=RevisionSource.EDIT,
                created_at_us=2_000,
            )
        ],
    )[0]


def _save_body(
    database: Database,
    repositories: Repositories,
    card: Card,
    body: str,
) -> Card:
    draft = DraftCoordinator(
        database,
        repositories,
        clock=lambda: 3_000,
        id_factory=lambda: f"draft-{body}",
    )
    identifiers = _ids(f"save-{body}")
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
        text=body,
        cursor_position_qchar=0,
    )
    result = save.save(session)
    assert result.outcome is SaveOutcome.SAVED
    return result.card


def test_history_lists_every_previous_string_and_document_events_newest_first(
    database: Database,
    repositories: Repositories,
) -> None:
    initial = _create_card(repositories)
    second = _save_body(database, repositories, initial, "둘째 본문")
    _save_body(database, repositories, second, "셋째 본문")
    history = HistoryService(database, repositories)

    revisions = history.list_card_revisions(initial.id)
    events = history.list_document_events(initial.document_id)

    assert [revision.body for revision in revisions] == [
        "셋째 본문",
        "둘째 본문",
        "첫 본문",
    ]
    assert [event.event_seq for event in events] == sorted(
        (event.event_seq for event in events if event.event_seq is not None),
        reverse=True,
    )
    assert [event.event_type for event in events] == [
        EventType.UPDATE,
        EventType.UPDATE,
        EventType.CREATE,
    ]


def test_restore_creates_child_of_current_and_preserves_both_sides(
    database: Database,
    repositories: Repositories,
) -> None:
    initial = _create_card(repositories)
    second = _save_body(database, repositories, initial, "복구 직전 본문")
    identifiers = _ids("restore")
    history = HistoryService(
        database,
        repositories,
        clock=lambda: 5_000,
        id_factory=lambda: next(identifiers),
    )

    result = history.restore(initial.id, "revision-initial")

    assert result.card.body == "첫 본문"
    assert result.card.body_hash == text_hash("첫 본문")
    assert result.card.source is CardSource.RESTORE
    assert result.revision.parent_revision_id == second.current_revision_id
    assert result.revision.body == "첫 본문"
    assert result.revision.source is RevisionSource.RESTORE
    assert result.event.event_type is EventType.RESTORE
    assert result.event.source is EventSource.RESTORE
    assert json.loads(result.event.details_json) == {
        "target_revision_id": "revision-initial",
        "previous_revision_id": second.current_revision_id,
    }
    assert [revision.body for revision in history.list_card_revisions(initial.id)] == [
        "첫 본문",
        "복구 직전 본문",
        "첫 본문",
    ]
    history.assert_card_revision_invariant(initial.id)


def test_restore_rejects_current_revision_without_creating_history(
    database: Database,
    repositories: Repositories,
) -> None:
    card = _create_card(repositories)
    history = HistoryService(database, repositories)

    with pytest.raises(ValueError, match="현재 리비전"):
        history.restore(card.id, "revision-initial")

    assert len(repositories.list_revisions(card.id)) == 1
    assert len(repositories.list_events(card.document_id)) == 1


def test_restore_failure_rolls_back_event_revision_and_card(
    database: Database,
    repositories: Repositories,
) -> None:
    initial = _create_card(repositories)
    current = _save_body(database, repositories, initial, "복구 전 본문")
    identifiers = _ids("failed-restore")
    history = HistoryService(
        database,
        repositories,
        clock=lambda: 5_000,
        id_factory=lambda: next(identifiers),
    )
    database.connection.execute(
        """
        CREATE TRIGGER fail_restore_card_update
        BEFORE UPDATE ON cards
        WHEN NEW.body = '첫 본문'
        BEGIN
            SELECT RAISE(ABORT, '의도한 복구 실패');
        END
        """
    )

    with pytest.raises(sqlite3.IntegrityError, match="의도한 복구 실패"):
        history.restore(initial.id, "revision-initial")

    assert repositories.get_card(initial.id) == current
    assert len(repositories.list_revisions(initial.id)) == 2
    assert len(repositories.list_events(initial.document_id)) == 2
    history.assert_card_revision_invariant(initial.id)


def test_restore_makes_existing_draft_stale_and_save_stops_with_comparison(
    database: Database,
    repositories: Repositories,
) -> None:
    initial = _create_card(repositories)
    current = _save_body(database, repositories, initial, "복구 전 본문")
    draft = DraftCoordinator(
        database,
        repositories,
        id_factory=lambda: "draft-before-restore",
    )
    session = draft.open_card(current)
    assert session is not None
    draft.update_session(
        session.draft_id,
        text="잔존 draft",
        cursor_position_qchar=0,
    )
    draft.protect_now(session.draft_id)
    identifiers = _ids("stale-restore")
    history = HistoryService(
        database,
        repositories,
        id_factory=lambda: next(identifiers),
    )
    restored = history.restore(initial.id, "revision-initial")

    result = SaveCoordinator(database, draft, repositories).save(session)

    assert result.outcome is SaveOutcome.CONFLICT
    assert result.conflict is not None
    assert result.conflict.committed_text == restored.card.body
    assert result.conflict.draft_text == "잔존 draft"
    assert repositories.get_card(initial.id) == restored.card
    stored_draft = repositories.get_draft(session.draft_id)
    assert stored_draft is not None
    assert stored_draft.draft_text == "잔존 draft"


def test_invariant_detects_card_body_hash_mismatch(
    database: Database,
    repositories: Repositories,
) -> None:
    card = _create_card(repositories)
    history = HistoryService(database, repositories)
    with pytest.raises(sqlite3.IntegrityError, match="현재 리비전"):
        database.connection.execute(
            "UPDATE cards SET body = ? WHERE id = ?",
            ("손상된 본문", card.id),
        )
    history.assert_card_revision_invariant(card.id)


def test_revision_diff_requires_same_card(
    database: Database,
    repositories: Repositories,
) -> None:
    first = _create_card(repositories)
    second = repositories.create_cards(
        NewCaptureOperation(
            id="operation-second",
            document_id=first.document_id,
            source=CaptureOperationSource.TYPING,
            split_policy=SplitPolicy.KEEP,
            original_text=None,
            created_at_us=3_000,
        ),
        [
            NewCard(
                id="card-second",
                revision_id="revision-second",
                event_id="event-second",
                position_key=2_048,
                body="다른 카드",
                card_source=CardSource.TYPING,
                event_source=EventSource.TYPING,
                revision_source=RevisionSource.EDIT,
                created_at_us=3_000,
            )
        ],
    )[0]
    history = HistoryService(database, repositories)

    with pytest.raises(ValueError, match="서로 다른 카드"):
        history.diff_revisions(
            first.current_revision_id or "",
            second.current_revision_id or "",
        )
