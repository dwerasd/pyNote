"""정적 게이트 자기시험용 기준 마이그레이션(fixture).

실제 스키마의 사본이 **아니다.** 일부러 다른 이름을 쓴다 - 이 파일이 진짜 스키마의
두 번째 사본으로 오인되면 단일출처가 깨진다. 대신 게이트가 다뤄야 하는 두 가지를
그대로 담는다.

**추출 함정**: 중첩 괄호, 작은따옴표 목록, 여러 줄에 걸친 CHECK, 문장 앞뒤의
개행과 네 칸 들여쓰기.

**발행 모양**: 실제 마이그레이션들이 쓰는 세 가지 모양을 한 파일에 모았다 -
인라인 `connection.execute(...)`(v0002 계열), 지역 튜플을 도는 루프(v0003),
바인드 파라미터를 함께 넘기는 문장(v0002~v0009 전건). 게이트는 소스를 파싱하지
않고 `migrate()` 가 **실제로 발행한** 문장을 잡으므로, 이 세 모양이 모두
같은 결과를 내야 한다.

문장은 순서대로 **실제 실행된다**(기록 프록시가 진짜 연결로 중계한다). 그래서
INSERT 대상 테이블이 먼저 만들어져야 하고, SQL 은 유효해야 한다.

`check_migration_sql_parity.py --self-test` 만 이 파일을 읽는다.
"""

from __future__ import annotations

import sqlite3


def migrate(connection: sqlite3.Connection, applied_at_us: int) -> None:
    """기준 fixture 스키마를 만든다. 발행 순서 자체가 시험 대상이다."""
    connection.execute(
        """
    CREATE TABLE fixture_alpha (
        id TEXT PRIMARY KEY,
        label TEXT NOT NULL,
        kind TEXT NOT NULL
            CHECK (kind IN ('alpha', 'beta', 'gamma')),
        created_at_us INTEGER NOT NULL
    )
    """
    )
    statements = (
        """
    CREATE TABLE fixture_beta (
        id TEXT PRIMARY KEY,
        alpha_id TEXT NOT NULL
            REFERENCES fixture_alpha(id) ON DELETE RESTRICT,
        state TEXT NOT NULL
            CHECK (
                state IN (
                    'draft', 'ready',
                    'done'
                )
            ),
        body TEXT NOT NULL
    )
    """,
        """
    CREATE TABLE fixture_counters (
        name TEXT PRIMARY KEY,
        next_value INTEGER NOT NULL
    )
    """,
    )
    for statement in statements:
        connection.execute(statement)
    connection.execute(
        """
    CREATE UNIQUE INDEX fixture_beta_alpha
    ON fixture_beta(alpha_id)
    WHERE state IS NOT NULL
    """
    )
    connection.execute(
        """
    INSERT INTO fixture_counters(name, next_value)
    VALUES ('fixture', 1)
    """
    )
    connection.execute(
        """
    INSERT INTO fixture_alpha(id, label, kind, created_at_us)
    VALUES ('seed', 'seed', 'alpha', ?)
    """,
        (applied_at_us,),
    )
