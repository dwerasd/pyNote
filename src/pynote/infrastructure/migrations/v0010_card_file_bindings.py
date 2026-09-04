from __future__ import annotations

import sqlite3


def migrate(connection: sqlite3.Connection, applied_at_us: int) -> None:
    """카드 한 장과 파일 한 개를 잇는 결속 표를 추가한다."""
    # 되감기 시험이 최신 스키마 DB의 schema_version만 낮춘 뒤 재개봉하므로 DDL은 멱등이어야 한다.
    connection.execute(
        """
        CREATE TABLE IF NOT EXISTS card_file_bindings (
            card_id TEXT PRIMARY KEY
                REFERENCES cards(id) ON DELETE RESTRICT,
            path TEXT NOT NULL,
            path_key TEXT NOT NULL UNIQUE,
            encoding TEXT NOT NULL,
            bom INTEGER NOT NULL CHECK(bom IN (0, 1)),
            newline TEXT NOT NULL CHECK(newline IN ('lf', 'crlf', 'cr')),
            trailing_newline INTEGER NOT NULL CHECK(trailing_newline IN (0, 1)),
            synced_size INTEGER,
            synced_mtime_ns INTEGER,
            synced_hash TEXT,
            bound_at_us INTEGER NOT NULL,
            synced_at_us INTEGER
        )
        """
    )
    connection.execute(
        """
        UPDATE schema_version
        SET version = 10, applied_at_us = ?
        WHERE id = 1
        """,
        (applied_at_us,),
    )
