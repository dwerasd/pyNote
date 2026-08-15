from __future__ import annotations

import sqlite3


def migrate(connection: sqlite3.Connection, applied_at_us: int) -> None:
    """문서 UI 상태에 편집 분할 보기 좌우 크기 열을 추가한다."""
    connection.execute(
        "ALTER TABLE document_ui_states ADD COLUMN editor_split_left INTEGER"
    )
    connection.execute(
        "ALTER TABLE document_ui_states ADD COLUMN editor_split_right INTEGER"
    )
    connection.execute(
        """
        UPDATE schema_version
        SET version = 6, applied_at_us = ?
        WHERE id = 1
        """,
        (applied_at_us,),
    )
