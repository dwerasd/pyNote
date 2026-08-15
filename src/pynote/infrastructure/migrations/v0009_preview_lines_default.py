from __future__ import annotations

import sqlite3


def migrate(connection: sqlite3.Connection, applied_at_us: int) -> None:
    """이전 기본 미리보기 줄 수 6을 새 기본값 3으로 한 번 정규화한다."""
    connection.execute(
        """
        UPDATE data_policy_settings
        SET preview_lines = 3
        WHERE id = 1 AND preview_lines = 6
        """
    )
    connection.execute(
        """
        UPDATE schema_version
        SET version = 9, applied_at_us = ?
        WHERE id = 1
        """,
        (applied_at_us,),
    )
