from __future__ import annotations

import sqlite3

STATEMENTS = (
    """
    CREATE TABLE IF NOT EXISTS schema_version (
        id INTEGER PRIMARY KEY CHECK (id = 1),
        version INTEGER NOT NULL CHECK (version >= 0),
        applied_at_us INTEGER NOT NULL
    )
    """,
    """
    CREATE TABLE documents (
        id TEXT PRIMARY KEY,
        title TEXT NOT NULL,
        created_at_us INTEGER NOT NULL,
        updated_at_us INTEGER NOT NULL,
        archived_at_us INTEGER,
        trashed_at_us INTEGER
    )
    """,
    """
    CREATE TABLE capture_operations (
        id TEXT PRIMARY KEY,
        document_id TEXT NOT NULL
            REFERENCES documents(id) ON DELETE RESTRICT,
        source TEXT NOT NULL
            CHECK (source IN ('typing', 'paste', 'import', 'mixed', 'split', 'merge', 'system')),
        split_policy TEXT NOT NULL
            CHECK (split_policy IN ('keep', 'split_by_blank_line')),
        original_text TEXT,
        original_hash TEXT,
        original_redacted_at_us INTEGER,
        created_at_us INTEGER NOT NULL
    )
    """,
    """
    CREATE TABLE cards (
        id TEXT PRIMARY KEY,
        document_id TEXT NOT NULL
            REFERENCES documents(id) ON DELETE RESTRICT,
        operation_id TEXT NOT NULL
            REFERENCES capture_operations(id) ON DELETE RESTRICT,
        position_key INTEGER NOT NULL,
        capture_seq INTEGER NOT NULL UNIQUE,
        created_at_us INTEGER NOT NULL,
        updated_at_us INTEGER NOT NULL,
        source TEXT NOT NULL
            CHECK (
                source IN (
                    'typing', 'paste', 'import', 'mixed',
                    'restore', 'split', 'merge', 'system'
                )
            ),
        body TEXT NOT NULL,
        body_hash TEXT NOT NULL,
        current_revision_id TEXT
            REFERENCES card_revisions(id) ON DELETE RESTRICT
            DEFERRABLE INITIALLY DEFERRED,
        deleted_at_us INTEGER
    )
    """,
    """
    CREATE TABLE edit_events (
        event_seq INTEGER PRIMARY KEY AUTOINCREMENT,
        event_id TEXT NOT NULL UNIQUE,
        operation_id TEXT
            REFERENCES capture_operations(id) ON DELETE RESTRICT,
        document_id TEXT NOT NULL
            REFERENCES documents(id) ON DELETE RESTRICT,
        card_id TEXT
            REFERENCES cards(id) ON DELETE RESTRICT
            DEFERRABLE INITIALLY DEFERRED,
        event_type TEXT NOT NULL
            CHECK (
                event_type IN (
                    'create', 'update', 'move', 'split',
                    'merge', 'delete', 'restore'
                )
            ),
        source TEXT NOT NULL
            CHECK (source IN ('typing', 'paste', 'import', 'mixed', 'edit', 'restore', 'system')),
        occurred_at_us INTEGER NOT NULL,
        details_json TEXT NOT NULL
    )
    """,
    """
    CREATE TABLE card_revisions (
        id TEXT PRIMARY KEY,
        card_id TEXT NOT NULL
            REFERENCES cards(id) ON DELETE RESTRICT,
        event_seq INTEGER NOT NULL
            REFERENCES edit_events(event_seq) ON DELETE RESTRICT,
        parent_revision_id TEXT
            REFERENCES card_revisions(id) ON DELETE RESTRICT,
        body TEXT NOT NULL,
        body_hash TEXT NOT NULL,
        source TEXT NOT NULL
            CHECK (source IN ('edit', 'restore', 'split', 'merge')),
        created_at_us INTEGER NOT NULL
    )
    """,
    """
    CREATE TABLE drafts (
        id TEXT PRIMARY KEY,
        document_id TEXT NOT NULL
            REFERENCES documents(id) ON DELETE RESTRICT,
        card_id TEXT
            REFERENCES cards(id) ON DELETE RESTRICT,
        draft_kind TEXT NOT NULL
            CHECK (draft_kind IN ('new', 'edit')),
        base_revision_id TEXT
            REFERENCES card_revisions(id) ON DELETE RESTRICT,
        draft_text TEXT NOT NULL,
        draft_hash TEXT NOT NULL,
        cursor_position_qchar INTEGER NOT NULL,
        updated_at_us INTEGER NOT NULL
    )
    """,
    """
    CREATE TABLE card_lineage (
        parent_card_id TEXT NOT NULL
            REFERENCES cards(id) ON DELETE RESTRICT,
        child_card_id TEXT NOT NULL
            REFERENCES cards(id) ON DELETE RESTRICT,
        event_seq INTEGER NOT NULL
            REFERENCES edit_events(event_seq) ON DELETE RESTRICT,
        relation_type TEXT NOT NULL
            CHECK (relation_type IN ('split', 'merge')),
        PRIMARY KEY (parent_card_id, child_card_id, event_seq)
    )
    """,
    """
    CREATE TABLE counters (
        name TEXT PRIMARY KEY,
        next_value INTEGER NOT NULL
    )
    """,
    """
    CREATE TABLE workspace_state (
        id INTEGER PRIMARY KEY CHECK (id = 1),
        open_document_ids_json TEXT NOT NULL,
        active_document_id TEXT
            REFERENCES documents(id) ON DELETE RESTRICT,
        updated_at_us INTEGER NOT NULL
    )
    """,
    """
    CREATE TABLE document_ui_states (
        document_id TEXT PRIMARY KEY
            REFERENCES documents(id) ON DELETE RESTRICT,
        selected_card_id TEXT
            REFERENCES cards(id) ON DELETE RESTRICT,
        list_scroll_position INTEGER NOT NULL,
        sort_mode TEXT NOT NULL
            CHECK (sort_mode IN ('recency', 'position', 'capture')),
        editor_card_id TEXT
            REFERENCES cards(id) ON DELETE RESTRICT,
        editor_base_revision_id TEXT
            REFERENCES card_revisions(id) ON DELETE RESTRICT,
        editor_cursor_qchar INTEGER,
        updated_at_us INTEGER NOT NULL
    )
    """,
    """
    CREATE UNIQUE INDEX active_card_position
    ON cards(document_id, position_key)
    WHERE deleted_at_us IS NULL
    """,
    """
    CREATE UNIQUE INDEX active_card_draft
    ON drafts(card_id)
    WHERE card_id IS NOT NULL
    """,
    """
    INSERT INTO counters(name, next_value)
    VALUES ('capture', 1)
    """,
)


def migrate(connection: sqlite3.Connection, applied_at_us: int) -> None:
    """초기 스키마를 생성한다."""
    for statement in STATEMENTS:
        connection.execute(statement)
    connection.execute(
        """
        INSERT INTO schema_version(id, version, applied_at_us)
        VALUES (1, 1, ?)
        ON CONFLICT(id) DO UPDATE SET
            version = excluded.version,
            applied_at_us = excluded.applied_at_us
        """,
        (applied_at_us,),
    )
