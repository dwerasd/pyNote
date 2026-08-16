// known-bad fixture: STATEMENTS 밖의 SQL 을 같은 `SQL` 구분자로 적어 문장 수가 늘었다.
// 실제로 벌어질 수 있는 사고다 - schema_version upsert 는 STATEMENTS 가 아닌데
// 습관대로 R"SQL( 를 쓰면 이 게이트의 추출 대상이 되어 버린다.

namespace fixture::bad_extra_literal
{
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

	static const char* const g_szUpsert = R"SQL(
    INSERT INTO fixture_version(id, version)
    VALUES (1, 1)
    )SQL";
}
