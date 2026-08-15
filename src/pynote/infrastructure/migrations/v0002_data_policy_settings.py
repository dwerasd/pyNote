from __future__ import annotations

import sqlite3


def migrate(connection: sqlite3.Connection, applied_at_us: int) -> None:
    """장치와 공유되지 않는 데이터 운용 정책 단일 행을 추가한다."""
    connection.execute(
        """
        CREATE TABLE data_policy_settings (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            draft_idle_ms INTEGER NOT NULL CHECK (draft_idle_ms >= 0),
            split_policy TEXT NOT NULL
                CHECK (split_policy IN ('keep', 'split_by_blank_line')),
            preview_lines INTEGER NOT NULL CHECK (preview_lines >= 1),
            backup_interval_hours REAL NOT NULL CHECK (backup_interval_hours > 0),
            trash_retention_days INTEGER NOT NULL CHECK (trash_retention_days >= 0),
            updated_at_us INTEGER NOT NULL
        )
        """
    )
    connection.execute(
        """
        INSERT INTO data_policy_settings(
            id, draft_idle_ms, split_policy, preview_lines,
            backup_interval_hours, trash_retention_days, updated_at_us
        ) VALUES (1, 2000, 'keep', 6, 24, 30, ?)
        """,
        (applied_at_us,),
    )
    connection.execute(
        """
        UPDATE schema_version
        SET version = 2, applied_at_us = ?
        WHERE id = 1
        """,
        (applied_at_us,),
    )
