from __future__ import annotations

import sqlite3


def migrate(connection: sqlite3.Connection, applied_at_us: int) -> None:
    """단일 workspace 행을 복원 대상 창 행 집합으로 이관한다."""
    connection.execute(
        """
        CREATE TABLE workspace_windows (
            window_id TEXT PRIMARY KEY,
            open_document_ids_json TEXT NOT NULL,
            active_document_id TEXT
                REFERENCES documents(id) ON DELETE RESTRICT,
            updated_at_us INTEGER NOT NULL
        )
        """
    )
    connection.execute(
        """
        INSERT INTO workspace_windows(
            window_id, open_document_ids_json, active_document_id, updated_at_us
        )
        SELECT
            lower(
                hex(randomblob(4)) || '-' ||
                hex(randomblob(2)) || '-4' ||
                substr(hex(randomblob(2)), 2) || '-' ||
                substr('89ab', abs(random()) % 4 + 1, 1) ||
                substr(hex(randomblob(2)), 2) || '-' ||
                hex(randomblob(6))
            ),
            open_document_ids_json,
            active_document_id,
            updated_at_us
        FROM workspace_state
        WHERE id = 1
        """
    )
    connection.execute("DROP TABLE workspace_state")
    connection.execute(
        """
        UPDATE schema_version
        SET version = 4, applied_at_us = ?
        WHERE id = 1
        """,
        (applied_at_us,),
    )
