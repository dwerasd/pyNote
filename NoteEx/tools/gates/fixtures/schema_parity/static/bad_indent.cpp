// known-bad fixture: 문장 0 의 label 줄 들여쓰기가 8칸에서 6칸으로 바뀌었다.
// 눈으로는 거의 같아 보이는 종류의 결함이라 게이트가 잡아야 한다.

namespace fixture::bad_indent
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
