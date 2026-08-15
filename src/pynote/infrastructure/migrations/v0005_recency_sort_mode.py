from __future__ import annotations

import sqlite3


def migrate(connection: sqlite3.Connection, applied_at_us: int) -> None:
    """문서 UI 정렬 상태에 최근 활동순을 추가하며 기존 값을 보존한다."""
    connection.execute("ALTER TABLE document_ui_states RENAME TO document_ui_states_v4")
    connection.execute(
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
        """
    )
    connection.execute(
        """
        INSERT INTO document_ui_states(
            document_id, selected_card_id, list_scroll_position, sort_mode,
            editor_card_id, editor_base_revision_id, editor_cursor_qchar,
            updated_at_us
        )
        SELECT
            document_id, selected_card_id, list_scroll_position, sort_mode,
            editor_card_id, editor_base_revision_id, editor_cursor_qchar,
            updated_at_us
        FROM document_ui_states_v4
        """
    )
    connection.execute("DROP TABLE document_ui_states_v4")
    connection.execute(
        """
        UPDATE schema_version
        SET version = 5, applied_at_us = ?
        WHERE id = 1
        """,
        (applied_at_us,),
    )
