from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from pynote.application.history_service import HistoryService
from pynote.application.purge_service import REDACTED_MARKER, PurgeService
from pynote.domain.events import EventType
from pynote.domain.models import Document, LineageRelationType
from pynote.infrastructure.backup import create_database_backup, inspect_backup
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories

DAY_US = 24 * 60 * 60 * 1_000_000
NOW_US = 100 * DAY_US


def _document(
    repositories: Repositories,
    document_id: str,
) -> Document:
    document = Document(
        id=document_id,
        title="legacy graph 검증",
        created_at_us=1,
        updated_at_us=1,
    )
    repositories.create_document(document)
    return document


def test_history_displays_legacy_split_graph_without_service(
    database: Database,
    repositories: Repositories,
    legacy_graph_factory: Any,
) -> None:
    document = _document(repositories, "legacy-history")
    graph = legacy_graph_factory(
        document.id,
        "history-split",
        LineageRelationType.SPLIT,
    )

    history = HistoryService(database, repositories)
    revisions = history.list_card_revisions(graph.children[0].id)
    events = history.list_document_events(document.id)

    assert [revision.body for revision in revisions] == ["민감 A"]
    assert graph.transformation_event in events
    assert any(event.event_type is EventType.SPLIT for event in events)
    assert repositories.list_lineage_for_card(graph.children[0].id)


def test_backup_validator_accepts_legacy_split_and_merge_graphs(
    database: Database,
    repositories: Repositories,
    legacy_graph_factory: Any,
    tmp_path: Path,
) -> None:
    split_document = _document(repositories, "legacy-backup-split")
    merge_document = _document(repositories, "legacy-backup-merge")
    legacy_graph_factory(
        split_document.id,
        "backup-split",
        LineageRelationType.SPLIT,
    )
    legacy_graph_factory(
        merge_document.id,
        "backup-merge",
        LineageRelationType.MERGE,
    )
    backup_path = tmp_path / "legacy-graphs.sqlite3"

    created = create_database_backup(database.path, backup_path)
    inspected = inspect_backup(backup_path)

    assert created.schema_version == inspected.schema_version
    assert inspected.path == backup_path


def test_purge_handles_legacy_merge_lineage_and_redacts_shared_event(
    database: Database,
    repositories: Repositories,
    legacy_graph_factory: Any,
) -> None:
    document = _document(repositories, "legacy-purge")
    graph = legacy_graph_factory(
        document.id,
        "purge-merge",
        LineageRelationType.MERGE,
    )
    purged_parent = graph.parents[0]

    result = PurgeService(
        database,
        repositories,
        clock=lambda: NOW_US,
    ).purge_card(purged_parent.id, retention_days=30)

    assert result.redacted_event_count == 1
    event = repositories.get_event(graph.transformation_event.event_seq or 0)
    assert event is not None
    assert purged_parent.id not in event.details_json
    assert purged_parent.current_revision_id is not None
    assert purged_parent.current_revision_id not in event.details_json
    assert json.loads(event.details_json)["purge_redacted"] is True
    assert REDACTED_MARKER in event.details_json
    assert repositories.get_card(graph.parents[1].id) is not None
    assert repositories.get_card(graph.children[0].id) is not None
    remaining_lineage = repositories.list_lineage_for_card(graph.children[0].id)
    assert [lineage.parent_card_id for lineage in remaining_lineage] == [graph.parents[1].id]
    assert database.connection.execute("PRAGMA foreign_key_check").fetchall() == []
