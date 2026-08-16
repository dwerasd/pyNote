// 정적 게이트 자기시험용 known-good fixture.
// reference_statements.py 의 STATEMENTS 와 바이트 단위로 같아야 한다.
// 빌드 대상이 아니다 - 어떤 vcxproj 에도 등록하지 않는다.

#include <cstddef>

namespace fixture::good
{
	// 함정 1: 줄 주석 안의 R"SQL( 표기는 추출되면 안 된다.
	/* 함정 2: 블록 주석 안의 R"SQL( 표기도 마찬가지다. */

	// 함정 3: 일반 문자열 안의 표기(이스케이프된 따옴표 포함).
	static const char* const g_szTrap = "R\"SQL(문자열은 문장이 아니다)SQL\"";

	// 함정 4: 구분자가 다르면 추출 대상이 아니다.
	static const char* const g_szNote = R"NOTE(
    CREATE TABLE not_extracted (
        id TEXT PRIMARY KEY
    )
    )NOTE";

	static const char* const g_szStatements[] =
	{
		R"SQL(
    CREATE TABLE fixture_alpha (
        id TEXT PRIMARY KEY,
        label TEXT NOT NULL,
        kind TEXT NOT NULL
            CHECK (kind IN ('alpha', 'beta', 'gamma')),
        created_at_us INTEGER NOT NULL
    )
    )SQL",
		R"SQL(
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
    )SQL",
		R"SQL(
    CREATE TABLE fixture_counters (
        name TEXT PRIMARY KEY,
        next_value INTEGER NOT NULL
    )
    )SQL",
		R"SQL(
    CREATE UNIQUE INDEX fixture_beta_alpha
    ON fixture_beta(alpha_id)
    WHERE state IS NOT NULL
    )SQL",
		R"SQL(
    INSERT INTO fixture_counters(name, next_value)
    VALUES ('fixture', 1)
    )SQL",
	};
}
