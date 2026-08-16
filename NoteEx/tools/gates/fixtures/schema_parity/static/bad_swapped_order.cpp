// known-bad fixture: 문장 2 와 3 의 순서가 뒤바뀌었다(본문은 전건 동일).
// 발행 순서는 계약이다 - 인덱스를 대상 테이블보다 먼저 만들면 실행이 깨지고,
// 내용만 집합으로 비교하는 게이트는 이 결함을 통과시킨다.

#include <cstddef>

namespace fixture::bad_swapped_order
{
	// 함정 1: 줄 주석 안의 u8R"SQL( 표기는 추출되면 안 된다.
	/* 함정 2: 블록 주석 안의 u8R"SQL( 표기도 마찬가지다. */

	// 함정 3: 일반 문자열 안의 표기(이스케이프된 따옴표 포함).
	static const char* const g_szTrap = "u8R\"SQL(문자열은 문장이 아니다)SQL\"";

	// 함정 4: 구분자가 다르면 추출 대상이 아니다.
	static const char* const g_szNote = u8R"NOTE(
    CREATE TABLE not_extracted (
        id TEXT PRIMARY KEY
    )
    )NOTE";

	static const char* const g_szStatements[] =
	{
		reinterpret_cast<const char*>(u8R"SQL(
    CREATE TABLE fixture_alpha (
        id TEXT PRIMARY KEY,
        label TEXT NOT NULL,
        kind TEXT NOT NULL
            CHECK (kind IN ('alpha', 'beta', 'gamma')),
        created_at_us INTEGER NOT NULL
    )
    )SQL"),
		reinterpret_cast<const char*>(u8R"SQL(
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
    )SQL"),
		u8R"SQL(
    CREATE UNIQUE INDEX fixture_beta_alpha
    ON fixture_beta(alpha_id)
    WHERE state IS NOT NULL
    )SQL",
		u8R"SQL(
    CREATE TABLE fixture_counters (
        name TEXT PRIMARY KEY,
        next_value INTEGER NOT NULL
    )
    )SQL",
		u8R"SQL(
    INSERT INTO fixture_counters(name, next_value)
    VALUES ('fixture', 1)
    )SQL",
	};

	// 바인드 파라미터를 쓰는 문장도 같은 구분자로 적는다(T-R2 부터 이식 대상이다).
	static const char* const g_szSeedAlpha = reinterpret_cast<const char*>(u8R"SQL(
    INSERT INTO fixture_alpha(id, label, kind, created_at_us)
    VALUES ('seed', 'seed', 'alpha', ?)
    )SQL");
}
