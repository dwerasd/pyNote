"""정적 게이트 자기시험용 기준 문장(fixture).

실제 스키마의 사본이 **아니다.** 일부러 다른 이름을 쓴다 - 이 파일이 진짜 스키마의
두 번째 사본으로 오인되면 단일출처가 깨진다. 대신 원본이 가진 추출 함정은 그대로
담는다: 중첩 괄호, 작은따옴표 목록, 여러 줄에 걸친 CHECK, 문장 앞뒤의 개행과
네 칸 들여쓰기.

`check_migration_sql_parity.py --self-test` 만 이 파일을 읽는다. 실행되지 않으므로
INSERT 대상 테이블이 먼저 만들어지는지 같은 실행 의미는 검사 대상이 아니다.
"""

from __future__ import annotations

STATEMENTS = (
    """
    CREATE TABLE fixture_alpha (
        id TEXT PRIMARY KEY,
        label TEXT NOT NULL,
        kind TEXT NOT NULL
            CHECK (kind IN ('alpha', 'beta', 'gamma')),
        created_at_us INTEGER NOT NULL
    )
    """,
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
    """
    CREATE UNIQUE INDEX fixture_beta_alpha
    ON fixture_beta(alpha_id)
    WHERE state IS NOT NULL
    """,
    """
    INSERT INTO fixture_counters(name, next_value)
    VALUES ('fixture', 1)
    """,
)
