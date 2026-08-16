// known-bad fixture: 문장 0 의 id 와 label 열 순서가 뒤바뀌었다.
// 열 순서는 sqlite_master 원문과 INSERT 의미에 모두 영향을 준다.

namespace fixture::bad_reordered_column
{
	static const char* const g_szStatements[] =
	{
		R"SQL(
    CREATE TABLE fixture_alpha (
        label TEXT NOT NULL,
        id TEXT PRIMARY KEY,
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
