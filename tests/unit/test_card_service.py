from __future__ import annotations

import json
import sqlite3
from collections.abc import Callable

import pytest
from pytest import MonkeyPatch

from pynote.application.card_service import CardService
from pynote.domain.events import EventType
from pynote.domain.models import CaptureOperationSource, Document, Draft, DraftKind
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import (
    CardCompareAndSwapError,
    Repositories,
    text_hash,
)


def _create_document(repositories: Repositories) -> str:
    document_id = "document-card-service"
    repositories.create_document(
        Document(
            id=document_id,
            title="카드 서비스 테스트",
            created_at_us=1_000,
            updated_at_us=1_000,
        )
    )
    return document_id


def test_keep_saves_fifty_lines_as_one_card_without_original_text(
    database: Database,
    repositories: Repositories,
) -> None:
    document_id = _create_document(repositories)
    service = CardService(database, repositories, clock=lambda: 2_000)
    body = "\n".join(f"{line}번째 줄" for line in range(1, 61))

    (card,) = service.create_cards(
        document_id,
        body,
        source=CaptureOperationSource.PASTE,
    )

    operation = repositories.get_capture_operation(card.operation_id)
    revision = repositories.get_revision(card.current_revision_id or "")
    assert card.body == body
    assert len(card.body.splitlines()) == 60
    assert operation is not None
    assert operation.original_text is None
    assert operation.original_hash is None
    assert revision is not None
    assert revision.body == body


def test_split_creates_one_operation_and_preserves_original_only_there(
    database: Database,
    repositories: Repositories,
) -> None:
    document_id = _create_document(repositories)
    service = CardService(database, repositories, clock=lambda: 2_000)
    original = " 첫 문단\r\n한 줄 더\r\n\r\n  \r\n둘째 문단\n\n셋째 문단\n"

    cards = service.create_cards(
        document_id,
        original,
        source=CaptureOperationSource.PASTE,
        split=True,
    )

    assert [card.body for card in cards] == [
        " 첫 문단\n한 줄 더",
        "둘째 문단",
        "셋째 문단",
    ]
    assert len({card.operation_id for card in cards}) == 1
    assert [card.capture_seq for card in cards] == [1, 2, 3]
    operation = repositories.get_capture_operation(cards[0].operation_id)
    assert operation is not None
    assert operation.original_text == original
    assert operation.original_hash is not None


def test_mixed_source_is_stored_on_operation_card_and_event(
    database: Database,
    repositories: Repositories,
) -> None:
    document_id = _create_document(repositories)
    service = CardService(database, repositories, clock=lambda: 2_000)

    card = service.create_card(
        document_id,
        "직접 입력 뒤 붙여넣기",
        source=CaptureOperationSource.MIXED,
    )

    operation = repositories.get_capture_operation(card.operation_id)
    event = repositories.list_events(document_id)[0]
    assert operation is not None
    assert operation.source is CaptureOperationSource.MIXED
    assert card.source.value == "mixed"
    assert event.source.value == "mixed"


def test_split_failure_rolls_back_operation_cards_events_revisions_and_counter(
    database: Database,
    repositories: Repositories,
) -> None:
    document_id = _create_document(repositories)
    service = CardService(database, repositories, clock=lambda: 2_000)
    database.connection.execute(
        """
        CREATE TRIGGER fail_second_body_revision
        BEFORE INSERT ON card_revisions
        WHEN NEW.body = '둘째 문단'
        BEGIN
            SELECT RAISE(ABORT, '의도한 분리 실패');
        END
        """
    )

    with pytest.raises(sqlite3.IntegrityError, match="의도한 분리 실패"):
        service.create_cards(
            document_id,
            "첫 문단\n\n둘째 문단",
            source=CaptureOperationSource.PASTE,
            split=True,
        )

    for table in ("capture_operations", "cards", "edit_events", "card_revisions"):
        row = database.connection.execute(f"SELECT COUNT(*) FROM {table}").fetchone()
        assert row is not None
        assert int(row[0]) == 0
    assert repositories.get_counter("capture") == 1


def test_soft_delete_discards_draft_in_one_transaction(
    monkeypatch: MonkeyPatch,
    database: Database,
    repositories: Repositories,
) -> None:
    document_id = _create_document(repositories)
    service = CardService(database, repositories, clock=lambda: 2_000)
    deleted_card = service.create_card(document_id, "삭제 성공")
    stale_card = service.create_card(document_id, "CAS 실패")
    rollback_card = service.create_card(document_id, "draft 삭제 뒤 실패")
    for card, draft_id in (
        (deleted_card, "draft-delete"),
        (stale_card, "draft-stale"),
        (rollback_card, "draft-rollback"),
    ):
        draft_text = f"{card.body} 편집"
        repositories.create_draft(
            Draft(
                id=draft_id,
                document_id=document_id,
                card_id=card.id,
                draft_kind=DraftKind.EDIT,
                base_revision_id=card.current_revision_id,
                draft_text=draft_text,
                draft_hash=text_hash(draft_text),
                cursor_position_qchar=len(draft_text),
                updated_at_us=3_000,
            )
        )

    service.soft_delete(
        deleted_card.id,
        expected_revision_id=deleted_card.current_revision_id,
        discard_draft_id="draft-delete",
    )

    assert repositories.get_draft("draft-delete") is None
    assert repositories.get_card(deleted_card.id).deleted_at_us is not None  # type: ignore[union-attr]

    with pytest.raises(CardCompareAndSwapError):
        service.soft_delete(
            stale_card.id,
            expected_revision_id="stale-revision",
            discard_draft_id="draft-stale",
        )

    assert repositories.get_draft("draft-stale") is not None
    assert repositories.get_card(stale_card.id).deleted_at_us is None  # type: ignore[union-attr]

    delete_draft: Callable[[str], None] = repositories.delete_draft

    def fail_after_draft_delete(draft_id: str) -> None:
        delete_draft(draft_id)
        raise RuntimeError("draft 삭제 직후 실패")

    monkeypatch.setattr(repositories, "delete_draft", fail_after_draft_delete)
    with pytest.raises(RuntimeError, match="draft 삭제 직후 실패"):
        service.soft_delete(
            rollback_card.id,
            expected_revision_id=rollback_card.current_revision_id,
            discard_draft_id="draft-rollback",
        )

    assert repositories.get_draft("draft-rollback") is not None
    assert repositories.get_card(rollback_card.id).deleted_at_us is None  # type: ignore[union-attr]
    assert not any(
        event.card_id == rollback_card.id and event.event_type is EventType.DELETE
        for event in repositories.list_events(document_id)
    )


def test_middle_insertion_position_is_independent_from_capture_sequence(
    database: Database,
    repositories: Repositories,
) -> None:
    document_id = _create_document(repositories)
    service = CardService(database, repositories, clock=lambda: 2_000)
    first = service.create_card(document_id, "A")
    second = service.create_card(document_id, "B")

    middle = service.create_card(document_id, "C", before_card_id=second.id)

    ordered = service.list_active_cards(document_id, sort_mode="position")
    assert [card.body for card in ordered] == ["A", "C", "B"]
    assert [card.capture_seq for card in ordered] == [1, 3, 2]
    assert first.position_key < middle.position_key < second.position_key


def test_default_active_card_order_is_recency_with_capture_tie_breaker(
    database: Database,
    repositories: Repositories,
) -> None:
    document_id = _create_document(repositories)
    service = CardService(database, repositories, clock=lambda: 2_000)
    created = tuple(
        service.create_card(document_id, body)
        for body in ("A", "B", "C")
    )

    assert [
        card.id for card in service.list_active_cards(document_id)
    ] == [created[2].id, created[1].id, created[0].id]
    assert [
        card.id
        for card in service.list_active_cards(
            document_id,
            sort_mode="position",
        )
    ] == [created[0].id, created[1].id, created[2].id]


def test_exhausted_position_gap_is_rebalanced_in_creation_transaction(
    database: Database,
    repositories: Repositories,
) -> None:
    document_id = _create_document(repositories)
    service = CardService(database, repositories, clock=lambda: 2_000)
    first = service.create_card(document_id, "A")
    second = service.create_card(document_id, "B")
    repositories.update_card_position(
        first.id,
        1,
        expected_revision_id=first.current_revision_id or "",
    )
    repositories.update_card_position(
        second.id,
        2,
        expected_revision_id=second.current_revision_id or "",
    )

    middle = service.create_card(document_id, "C", before_card_id=second.id)

    ordered = service.list_active_cards(document_id, sort_mode="position")
    assert [card.body for card in ordered] == ["A", "C", "B"]
    assert len({card.position_key for card in ordered}) == 3
    assert middle.capture_seq == 3


def test_move_and_soft_delete_keep_capture_sequence_and_record_neighbors(
    database: Database,
    repositories: Repositories,
) -> None:
    document_id = _create_document(repositories)
    times = iter((2_000, 3_000, 4_000, 5_000, 6_000))
    service = CardService(database, repositories, clock=lambda: next(times))
    first = service.create_card(document_id, "A")
    second = service.create_card(document_id, "B")
    third = service.create_card(document_id, "C")

    moved = service.move_card(third.id, before_card_id=second.id)
    deleted = service.soft_delete(moved.id)

    assert moved.capture_seq == third.capture_seq
    assert deleted.capture_seq == third.capture_seq
    assert [
        card.body
        for card in service.list_active_cards(document_id, sort_mode="position")
    ] == ["A", "B"]
    delete_event = repositories.list_events(document_id)[-1]
    details = json.loads(delete_event.details_json)
    assert details["left_neighbor_id"] == first.id
    assert details["right_neighbor_id"] == second.id


def test_soft_delete_failure_rolls_back_event_and_card_state(
    database: Database,
    repositories: Repositories,
) -> None:
    document_id = _create_document(repositories)
    service = CardService(database, repositories, clock=lambda: 2_000)
    card = service.create_card(document_id, "삭제 실패")
    events_before = repositories.list_events(document_id)
    database.connection.execute(
        """
        CREATE TRIGGER fail_soft_delete
        BEFORE UPDATE OF deleted_at_us ON cards
        WHEN NEW.deleted_at_us IS NOT NULL
        BEGIN
            SELECT RAISE(ABORT, '의도한 삭제 실패');
        END
        """
    )

    with pytest.raises(sqlite3.IntegrityError, match="의도한 삭제 실패"):
        service.soft_delete(card.id)

    stored = repositories.get_card(card.id)
    assert stored is not None
    assert stored.deleted_at_us is None
    assert repositories.list_events(document_id) == events_before


def test_soft_delete_empty_body_precondition_rejects_nonempty_card(
    database: Database,
    repositories: Repositories,
) -> None:
    document_id = _create_document(repositories)
    service = CardService(database, repositories, clock=lambda: 2_000)
    card = service.create_card(document_id, "비어 있지 않은 본문")
    events_before = repositories.list_events(document_id)

    with pytest.raises(CardCompareAndSwapError, match="본문이 비어 있지 않습니다"):
        service.soft_delete(
            card.id,
            expected_revision_id=card.current_revision_id,
            require_empty_body=True,
        )

    stored = repositories.get_card(card.id)
    assert stored is not None
    assert stored.deleted_at_us is None
    assert repositories.list_events(document_id) == events_before
