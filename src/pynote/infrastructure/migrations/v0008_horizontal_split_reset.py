from __future__ import annotations

import sqlite3


def migrate(connection: sqlite3.Connection, applied_at_us: int) -> None:
    """세로 분할 시절에 저장된 편집 분할 크기를 비운다.

    기존 값은 (상단 슬롯, 하단 목록) 순서라 가로 (좌측 목록, 우측 슬롯)로
    그대로 읽으면 의미가 어긋나므로 NULL 로 되돌려 기본 비율을 다시 적용한다.
    """
    connection.execute(
        """
        UPDATE document_ui_states
        SET editor_split_left = NULL, editor_split_right = NULL
        """
    )
    connection.execute(
        """
        UPDATE schema_version
        SET version = 8, applied_at_us = ?
        WHERE id = 1
        """,
        (applied_at_us,),
    )
