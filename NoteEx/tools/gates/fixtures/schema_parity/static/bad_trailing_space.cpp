// known-bad fixture: 문장 2 의 name 줄 끝에 공백 한 칸이 붙었다.
// 편집기의 자동 정리·수동 재입력이 만드는 가장 흔한 표류이며 눈으로는 보이지 않는다.

namespace fixture::bad_trailing_space
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
}
