from __future__ import annotations

import sqlite3


def migrate(connection: sqlite3.Connection, applied_at_us: int) -> None:
    """가로 분할 시절에 저장된 편집 분할 크기를 비운다.

    편집기가 입력기와 같은 슬롯을 쓰면서 분할이 세로로 바뀌었고,
    editor_split_left/right 는 이제 좌우가 아니라 상단 슬롯·하단 목록의 크기를
    담는다(명명 부채 — 열 이름은 repository·상태 계층 수정을 피하려고 유지한다).
    기존 값은 (목록, 편집기) 순서라 그대로 읽으면 편집기가 작아지는 방향으로
    의미가 뒤집히므로 NULL 로 되돌려 기본 비율을 다시 적용받게 한다.
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
        SET version = 7, applied_at_us = ?
        WHERE id = 1
        """,
        (applied_at_us,),
    )
