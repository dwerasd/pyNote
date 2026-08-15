from __future__ import annotations

import json
import logging
import time
from typing import Any

import pytest

from pynote.application.card_service import CardService
from pynote.application.purge_service import REDACTED_MARKER, PurgeService
from pynote.domain.models import (
    CaptureOperationSource,
    Document,
    LineageRelationType,
)
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories

DAY_US = 24 * 60 * 60 * 1_000_000
NOW_US = 100 * DAY_US
LOGGER = logging.getLogger(__name__)


def _document(repositories: Repositories, document_id: str = "document") -> Document:
    document = Document(
        id=document_id,
        title="purge 검증",
        created_at_us=1,
        updated_at_us=1,
    )
    repositories.create_document(document)
    return document


def test_card_purge_redacts_shared_operation_and_never_reuses_sequences(
    database: Database,
    repositories: Repositories,
) -> None:
    document = _document(repositories)
    ids = iter(f"id-{number}" for number in range(100))
    service = CardService(
        database,
        repositories,
        clock=lambda: 10,
        id_factory=lambda: next(ids),
    )
    first, sibling = service.create_cards(
        document.id,
        "개인정보 원문\n\n남길 문단",
        source=CaptureOperationSource.PASTE,
        split=True,
    )
    deleted = service.soft_delete(first.id)
    assert deleted.deleted_at_us == 10
    max_event_before = database.connection.execute(
        "SELECT MAX(event_seq) FROM edit_events"
    ).fetchone()[0]

    result = PurgeService(
        database,
        repositories,
        clock=lambda: NOW_US,
    ).purge_card(first.id, retention_days=30)

    assert result.redacted_operation_count == 1
    assert repositories.get_card(first.id) is None
    assert repositories.get_card(sibling.id) is not None
    operation = repositories.get_capture_operation(sibling.operation_id)
    assert operation is not None
    assert operation.original_text is None
    assert operation.original_hash is None
    assert operation.original_redacted_at_us == NOW_US
    assert not repositories.operation_reconstruction_available(sibling.id)
    assert database.connection.execute("PRAGMA foreign_key_check").fetchall() == []

    created_after = service.create_card(document.id, "purge 이후")
    new_event = max(event.event_seq or 0 for event in repositories.list_events(document.id))
    assert created_after.capture_seq > sibling.capture_seq
    assert new_event > max_event_before


def test_purge_split_parent_preserves_and_redacts_shared_event(
    database: Database,
    repositories: Repositories,
    legacy_graph_factory: Any,
) -> None:
    document = _document(repositories)
    graph = legacy_graph_factory(
        document.id,
        "purge-split",
        LineageRelationType.SPLIT,
    )
    parent = graph.parents[0]

    result = PurgeService(
        database,
        repositories,
        clock=lambda: NOW_US,
    ).purge_card(parent.id, retention_days=30)

    assert result.redacted_event_count == 1
    event = repositories.get_event(graph.transformation_event.event_seq or 0)
    assert event is not None
    assert event.card_id is None
    assert parent.id not in event.details_json
    assert parent.current_revision_id is not None
    assert parent.current_revision_id not in event.details_json
    details = json.loads(event.details_json)
    assert details["purge_redacted"] is True
    assert REDACTED_MARKER in event.details_json
    assert all(repositories.get_card(card.id) is not None for card in graph.children)
    assert database.connection.execute("PRAGMA foreign_key_check").fetchall() == []


def test_document_purge_cleans_workspace_ui_and_entire_graph(
    database: Database,
    repositories: Repositories,
) -> None:
    document = _document(repositories)
    service = CardService(database, repositories, clock=lambda: 10)
    card = service.create_card(document.id, "문서 전체 삭제")
    repositories.update_document(
        Document(
            id=document.id,
            title=document.title,
            created_at_us=document.created_at_us,
            updated_at_us=document.updated_at_us,
            trashed_at_us=10,
        )
    )
    database.connection.execute(
        """
        INSERT INTO workspace_windows(
            window_id, open_document_ids_json, active_document_id, updated_at_us
        ) VALUES ('window-1', ?, ?, 1)
        """,
        (json.dumps([document.id]), document.id),
    )
    database.connection.execute(
        """
        INSERT INTO workspace_windows(
            window_id, open_document_ids_json, active_document_id, updated_at_us
        ) VALUES ('window-2', ?, ?, 2)
        """,
        (json.dumps([document.id]), document.id),
    )
    database.connection.execute(
        """
        INSERT INTO document_ui_states(
            document_id, selected_card_id, list_scroll_position, sort_mode,
            editor_card_id, editor_base_revision_id, editor_cursor_qchar,
            updated_at_us
        ) VALUES (?, ?, 3, 'position', ?, ?, 4, 1)
        """,
        (document.id, card.id, card.id, card.current_revision_id),
    )

    result = PurgeService(
        database,
        repositories,
        clock=lambda: NOW_US,
    ).purge_document(document.id, retention_days=30)

    assert result.purged_card_count == 1
    assert repositories.get_document(document.id) is None
    assert repositories.get_card(card.id) is None
    workspaces = database.connection.execute(
        """
        SELECT open_document_ids_json, active_document_id
        FROM workspace_windows
        ORDER BY window_id
        """
    ).fetchall()
    assert [json.loads(row[0]) for row in workspaces] == [[], []]
    assert [row[1] for row in workspaces] == [None, None]
    for table in (
        "capture_operations",
        "cards",
        "card_revisions",
        "drafts",
        "edit_events",
        "card_lineage",
        "document_ui_states",
    ):
        assert database.connection.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0] == 0
    assert database.connection.execute("PRAGMA foreign_key_check").fetchall() == []


def test_purge_rejects_active_or_unexpired_targets(
    database: Database,
    repositories: Repositories,
) -> None:
    document = _document(repositories)
    card = CardService(database, repositories, clock=lambda: NOW_US).create_card(
        document.id,
        "보존",
    )
    purge = PurgeService(database, repositories, clock=lambda: NOW_US)

    with pytest.raises(ValueError, match="휴지통"):
        purge.purge_card(card.id, retention_days=30)

    CardService(database, repositories, clock=lambda: NOW_US).soft_delete(card.id)
    with pytest.raises(ValueError, match="보존 기간"):
        purge.purge_card(card.id, retention_days=30)


def test_card_soft_delete_is_recoverable(
    database: Database,
    repositories: Repositories,
) -> None:
    document = _document(repositories)
    service = CardService(database, repositories)
    first = service.create_card(document.id, "첫 카드")
    second = service.create_card(document.id, "둘째 카드")

    service.soft_delete(first.id)
    restored = service.restore_card(first.id)

    assert restored.deleted_at_us is None
    assert restored.position_key < second.position_key
    assert repositories.get_card(first.id) == restored
    assert database.connection.execute("PRAGMA foreign_key_check").fetchall() == []


def test_like_search_matches_titles_and_card_bodies_at_10000_cards(
    database: Database,
    repositories: Repositories,
) -> None:
    target = _document(repositories, "search-target")
    other = _document(repositories, "other")
    connection = database.connection
    now = time.time_ns() // 1_000
    with database.transaction():
        connection.execute(
            """
            INSERT INTO capture_operations(
                id, document_id, source, split_policy, original_text,
                original_hash, original_redacted_at_us, created_at_us
            ) VALUES ('bulk-operation', ?, 'system', 'keep', NULL, NULL, NULL, ?)
            """,
            (target.id, now),
        )
        connection.executemany(
            """
            INSERT INTO cards(
                id, document_id, operation_id, position_key, capture_seq,
                created_at_us, updated_at_us, source, body, body_hash,
                current_revision_id, deleted_at_us
            ) VALUES (?, ?, 'bulk-operation', ?, ?, ?, ?, 'system', ?, '', NULL, NULL)
            """,
            (
                (
                    f"bulk-{index}",
                    target.id,
                    (index + 1) * 1_024,
                    index + 1,
                    now,
                    now,
                    "검색 바늘" if index == 9_999 else f"카드 {index}",
                )
                for index in range(10_000)
            ),
        )

    started = time.perf_counter()
    cards = repositories.search_cards("검색 바늘", document_id=target.id)
    elapsed = time.perf_counter() - started
    LOGGER.info("LIKE 검색 10,000 카드 elapsed_ms=%.3f", elapsed * 1_000)

    assert [card.id for card in cards] == ["bulk-9999"]
    assert target.id in {document.id for document in repositories.search_documents("검색 바늘")}
    assert other.id in {document.id for document in repositories.search_documents("purge 검증")}
    assert elapsed < 5
