from __future__ import annotations

import sqlite3
from dataclasses import replace

import pytest

from pynote.domain.events import EditEvent, EventSource, EventType
from pynote.domain.models import (
    CaptureOperationSource,
    CardSource,
    Document,
    Draft,
    DraftKind,
    NewCaptureOperation,
    NewCard,
    RevisionSource,
    SplitPolicy,
)
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories, text_hash


def _create_document(repositories: Repositories) -> Document:
    document = Document(
        id="document-1",
        title="테스트 문서",
        created_at_us=1_000,
        updated_at_us=1_000,
    )
    repositories.create_document(document)
    return document


def _operation(identifier: str = "operation-1") -> NewCaptureOperation:
    return NewCaptureOperation(
        id=identifier,
        document_id="document-1",
        source=CaptureOperationSource.TYPING,
        split_policy=SplitPolicy.KEEP,
        original_text=None,
        created_at_us=2_000,
    )


def _card(
    number: int,
    body: str,
    *,
    position_key: int | None = None,
) -> NewCard:
    return NewCard(
        id=f"card-{number}",
        revision_id=f"revision-{number}",
        event_id=f"event-{number}",
        position_key=number * 1_024 if position_key is None else position_key,
        body=body,
        card_source=CardSource.TYPING,
        event_source=EventSource.TYPING,
        revision_source=RevisionSource.EDIT,
        created_at_us=2_000 + number,
    )


def _count(database: Database, table: str) -> int:
    row = database.connection.execute(f"SELECT COUNT(*) FROM {table}").fetchone()
    assert row is not None
    return int(row[0])


def test_multiline_unicode_round_trip(repositories: Repositories) -> None:
    _create_document(repositories)
    body = "첫 줄\n\n한글과 emoji 🧭\n보조 평면 문자: \U0002000b\n마지막 빈 줄\n"

    cards = repositories.create_cards(_operation(), [_card(1, body)])

    stored_card = repositories.get_card(cards[0].id)
    stored_revision = repositories.get_revision("revision-1")
    assert stored_card is not None
    assert stored_revision is not None
    assert stored_card.body == body
    assert stored_revision.body == body
    assert stored_card.body_hash == stored_revision.body_hash
    assert stored_card.current_revision_id == stored_revision.id


def test_new_cards_roll_back_every_row_and_counter_on_middle_failure(
    database: Database,
    repositories: Repositories,
) -> None:
    _create_document(repositories)
    database.connection.execute(
        """
        CREATE TRIGGER fail_second_revision
        BEFORE INSERT ON card_revisions
        WHEN NEW.id = 'revision-2'
        BEGIN
            SELECT RAISE(ABORT, '의도한 중간 실패');
        END
        """
    )

    with pytest.raises(sqlite3.IntegrityError, match="의도한 중간 실패"):
        repositories.create_cards(
            _operation(),
            [_card(1, "첫 카드"), _card(2, "두 번째 카드")],
        )

    assert _count(database, "capture_operations") == 0
    assert _count(database, "cards") == 0
    assert _count(database, "card_revisions") == 0
    assert _count(database, "edit_events") == 0
    assert repositories.get_counter("capture") == 1


def test_capture_counter_only_advances_for_successful_card_creation(
    repositories: Repositories,
) -> None:
    _create_document(repositories)
    first = repositories.create_cards(_operation(), [_card(1, "첫 카드")])
    assert first[0].capture_seq == 1
    assert repositories.get_counter("capture") == 2

    event = repositories.create_event(
        EditEvent(
            event_seq=None,
            event_id="event-update",
            operation_id=None,
            document_id="document-1",
            card_id="card-1",
            event_type=EventType.UPDATE,
            source=EventSource.EDIT,
            occurred_at_us=3_000,
            details_json="{}",
        )
    )
    assert event.event_seq is not None
    assert repositories.get_counter("capture") == 2

    second = repositories.create_cards(
        _operation("operation-2"),
        [_card(2, "두 번째 카드")],
    )
    assert second[0].capture_seq == 2
    assert repositories.get_counter("capture") == 3


def test_split_input_preserves_original_text_once(repositories: Repositories) -> None:
    _create_document(repositories)
    original_text = "첫 문단\n\n두 번째 문단"
    operation = NewCaptureOperation(
        id="operation-split",
        document_id="document-1",
        source=CaptureOperationSource.PASTE,
        split_policy=SplitPolicy.SPLIT_BY_BLANK_LINE,
        original_text=original_text,
        created_at_us=2_000,
    )

    repositories.create_cards(
        operation,
        [_card(1, "첫 문단"), _card(2, "두 번째 문단")],
    )

    stored = repositories.get_capture_operation(operation.id)
    assert stored is not None
    assert stored.original_text == original_text
    assert stored.original_hash == text_hash(original_text)


def test_keep_operation_rejects_duplicate_original_text(
    repositories: Repositories,
) -> None:
    _create_document(repositories)
    operation = NewCaptureOperation(
        id="operation-invalid",
        document_id="document-1",
        source=CaptureOperationSource.TYPING,
        split_policy=SplitPolicy.KEEP,
        original_text="중복 원문",
        created_at_us=2_000,
    )

    with pytest.raises(ValueError, match="중복 저장"):
        repositories.create_cards(operation, [_card(1, "중복 원문")])
    assert repositories.get_capture_operation(operation.id) is None


def test_partial_unique_indexes_enforce_active_card_and_draft_rules(
    repositories: Repositories,
) -> None:
    _create_document(repositories)
    first, second = repositories.create_cards(
        _operation(),
        [_card(1, "첫 카드"), _card(2, "두 번째 카드")],
    )

    with pytest.raises(sqlite3.IntegrityError):
        repositories.update_card_position(
            second.id,
            first.position_key,
            expected_revision_id=second.current_revision_id or "",
        )

    repositories.update_card_deleted_state(
        first.id,
        position_key=first.position_key,
        deleted_at_us=3_000,
        expected_revision_id=first.current_revision_id or "",
    )
    repositories.update_card_position(
        second.id,
        first.position_key,
        expected_revision_id=second.current_revision_id or "",
    )

    first_draft = Draft(
        id="draft-1",
        document_id="document-1",
        card_id=second.id,
        draft_kind=DraftKind.EDIT,
        base_revision_id=second.current_revision_id,
        draft_text="편집 중",
        draft_hash=text_hash("편집 중"),
        cursor_position_qchar=4,
        updated_at_us=4_000,
    )
    repositories.create_draft(first_draft)
    with pytest.raises(sqlite3.IntegrityError):
        repositories.create_draft(replace(first_draft, id="draft-2"))

    repositories.create_draft(
        replace(
            first_draft,
            id="draft-new-1",
            card_id=None,
            draft_kind=DraftKind.NEW,
            base_revision_id=None,
        )
    )
    repositories.create_draft(
        replace(
            first_draft,
            id="draft-new-2",
            card_id=None,
            draft_kind=DraftKind.NEW,
            base_revision_id=None,
        )
    )


def test_new_input_split_requires_original_and_existing_transform_forbids_it(
    repositories: Repositories,
) -> None:
    _create_document(repositories)
    missing_original = replace(
        _operation("operation-missing-original"),
        source=CaptureOperationSource.PASTE,
        split_policy=SplitPolicy.SPLIT_BY_BLANK_LINE,
    )
    duplicated_original = replace(
        _operation("operation-existing-split"),
        source=CaptureOperationSource.SPLIT,
        split_policy=SplitPolicy.SPLIT_BY_BLANK_LINE,
        original_text="기존 카드 원문",
    )

    with pytest.raises(ValueError, match="정확한 원문"):
        repositories.create_cards(missing_original, [_card(1, "첫 문단")])
    with pytest.raises(ValueError, match="중복 저장"):
        repositories.create_cards(duplicated_original, [_card(2, "둘째 문단")])


def test_database_rejects_cross_card_current_and_parent_revisions(
    database: Database,
    repositories: Repositories,
) -> None:
    _create_document(repositories)
    first, second = repositories.create_cards(
        _operation(),
        [_card(1, "첫 카드"), _card(2, "둘째 카드")],
    )

    with pytest.raises(sqlite3.IntegrityError, match="현재 리비전"):
        database.connection.execute(
            """
            UPDATE cards
            SET body = ?, body_hash = ?, current_revision_id = ?
            WHERE id = ?
            """,
            (
                second.body,
                second.body_hash,
                second.current_revision_id,
                first.id,
            ),
        )

    event = repositories.create_event(
        EditEvent(
            event_seq=None,
            event_id="cross-parent-event",
            operation_id=None,
            document_id=first.document_id,
            card_id=first.id,
            event_type=EventType.UPDATE,
            source=EventSource.EDIT,
            occurred_at_us=4_000,
            details_json="{}",
        )
    )
    assert event.event_seq is not None
    with pytest.raises(sqlite3.IntegrityError, match="같은 카드"):
        database.connection.execute(
            """
            INSERT INTO card_revisions(
                id, card_id, event_seq, parent_revision_id,
                body, body_hash, source, created_at_us
            ) VALUES (?, ?, ?, ?, ?, ?, 'edit', ?)
            """,
            (
                "cross-parent-revision",
                first.id,
                event.event_seq,
                second.current_revision_id,
                "교차 부모",
                text_hash("교차 부모"),
                4_000,
            ),
        )


def test_capture_counter_requires_transaction_and_cannot_go_back_or_delete(
    database: Database,
    repositories: Repositories,
) -> None:
    with pytest.raises(RuntimeError, match="카드 생성 트랜잭션"):
        repositories._issue_capture_sequence()
    with pytest.raises(sqlite3.IntegrityError, match="감소"):
        database.connection.execute(
            "UPDATE counters SET next_value = 0 WHERE name = 'capture'"
        )
    with pytest.raises(sqlite3.IntegrityError, match="삭제"):
        database.connection.execute("DELETE FROM counters WHERE name = 'capture'")
    assert repositories.get_counter("capture") == 1


def test_document_crud(repositories: Repositories) -> None:
    document = _create_document(repositories)
    updated = Document(
        id=document.id,
        title="바뀐 제목",
        created_at_us=document.created_at_us,
        updated_at_us=2_000,
        archived_at_us=2_000,
    )

    repositories.update_document(updated)
    assert repositories.get_document(document.id) == updated
    assert repositories.list_documents() == (updated,)
    repositories.delete_document(document.id)
    assert repositories.get_document(document.id) is None
