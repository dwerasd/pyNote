#include "pynote/core/storage/migrations/v0001_initial.h"

#include <iterator>
#include <string>

namespace pynote::core::storage::migrations::v0001
{
	namespace
	{
		// 파이썬 원본 v0001_initial.py 의 STATEMENTS 튜플이다. 앞뒤 개행과 들여쓰기 공백까지
		// 원본 그대로다 - 정렬·개명·재들여쓰기는 계약 위반이고, 정적 게이트가 이 리터럴을 뽑아
		// 원본과 바이트 비교한다. 그래서 구분자는 SQL 로 고정한다.
		constexpr const char* STATEMENTS[] = {
			R"SQL(
    CREATE TABLE IF NOT EXISTS schema_version (
        id INTEGER PRIMARY KEY CHECK (id = 1),
        version INTEGER NOT NULL CHECK (version >= 0),
        applied_at_us INTEGER NOT NULL
    )
    )SQL",
			R"SQL(
    CREATE TABLE documents (
        id TEXT PRIMARY KEY,
        title TEXT NOT NULL,
        created_at_us INTEGER NOT NULL,
        updated_at_us INTEGER NOT NULL,
        archived_at_us INTEGER,
        trashed_at_us INTEGER
    )
    )SQL",
			R"SQL(
    CREATE TABLE capture_operations (
        id TEXT PRIMARY KEY,
        document_id TEXT NOT NULL
            REFERENCES documents(id) ON DELETE RESTRICT,
        source TEXT NOT NULL
            CHECK (source IN ('typing', 'paste', 'import', 'mixed', 'split', 'merge', 'system')),
        split_policy TEXT NOT NULL
            CHECK (split_policy IN ('keep', 'split_by_blank_line')),
        original_text TEXT,
        original_hash TEXT,
        original_redacted_at_us INTEGER,
        created_at_us INTEGER NOT NULL
    )
    )SQL",
			R"SQL(
    CREATE TABLE cards (
        id TEXT PRIMARY KEY,
        document_id TEXT NOT NULL
            REFERENCES documents(id) ON DELETE RESTRICT,
        operation_id TEXT NOT NULL
            REFERENCES capture_operations(id) ON DELETE RESTRICT,
        position_key INTEGER NOT NULL,
        capture_seq INTEGER NOT NULL UNIQUE,
        created_at_us INTEGER NOT NULL,
        updated_at_us INTEGER NOT NULL,
        source TEXT NOT NULL
            CHECK (
                source IN (
                    'typing', 'paste', 'import', 'mixed',
                    'restore', 'split', 'merge', 'system'
                )
            ),
        body TEXT NOT NULL,
        body_hash TEXT NOT NULL,
        current_revision_id TEXT
            REFERENCES card_revisions(id) ON DELETE RESTRICT
            DEFERRABLE INITIALLY DEFERRED,
        deleted_at_us INTEGER
    )
    )SQL",
			R"SQL(
    CREATE TABLE edit_events (
        event_seq INTEGER PRIMARY KEY AUTOINCREMENT,
        event_id TEXT NOT NULL UNIQUE,
        operation_id TEXT
            REFERENCES capture_operations(id) ON DELETE RESTRICT,
        document_id TEXT NOT NULL
            REFERENCES documents(id) ON DELETE RESTRICT,
        card_id TEXT
            REFERENCES cards(id) ON DELETE RESTRICT
            DEFERRABLE INITIALLY DEFERRED,
        event_type TEXT NOT NULL
            CHECK (
                event_type IN (
                    'create', 'update', 'move', 'split',
                    'merge', 'delete', 'restore'
                )
            ),
        source TEXT NOT NULL
            CHECK (source IN ('typing', 'paste', 'import', 'mixed', 'edit', 'restore', 'system')),
        occurred_at_us INTEGER NOT NULL,
        details_json TEXT NOT NULL
    )
    )SQL",
			R"SQL(
    CREATE TABLE card_revisions (
        id TEXT PRIMARY KEY,
        card_id TEXT NOT NULL
            REFERENCES cards(id) ON DELETE RESTRICT,
        event_seq INTEGER NOT NULL
            REFERENCES edit_events(event_seq) ON DELETE RESTRICT,
        parent_revision_id TEXT
            REFERENCES card_revisions(id) ON DELETE RESTRICT,
        body TEXT NOT NULL,
        body_hash TEXT NOT NULL,
        source TEXT NOT NULL
            CHECK (source IN ('edit', 'restore', 'split', 'merge')),
        created_at_us INTEGER NOT NULL
    )
    )SQL",
			R"SQL(
    CREATE TABLE drafts (
        id TEXT PRIMARY KEY,
        document_id TEXT NOT NULL
            REFERENCES documents(id) ON DELETE RESTRICT,
        card_id TEXT
            REFERENCES cards(id) ON DELETE RESTRICT,
        draft_kind TEXT NOT NULL
            CHECK (draft_kind IN ('new', 'edit')),
        base_revision_id TEXT
            REFERENCES card_revisions(id) ON DELETE RESTRICT,
        draft_text TEXT NOT NULL,
        draft_hash TEXT NOT NULL,
        cursor_position_qchar INTEGER NOT NULL,
        updated_at_us INTEGER NOT NULL
    )
    )SQL",
			R"SQL(
    CREATE TABLE card_lineage (
        parent_card_id TEXT NOT NULL
            REFERENCES cards(id) ON DELETE RESTRICT,
        child_card_id TEXT NOT NULL
            REFERENCES cards(id) ON DELETE RESTRICT,
        event_seq INTEGER NOT NULL
            REFERENCES edit_events(event_seq) ON DELETE RESTRICT,
        relation_type TEXT NOT NULL
            CHECK (relation_type IN ('split', 'merge')),
        PRIMARY KEY (parent_card_id, child_card_id, event_seq)
    )
    )SQL",
			R"SQL(
    CREATE TABLE counters (
        name TEXT PRIMARY KEY,
        next_value INTEGER NOT NULL
    )
    )SQL",
			R"SQL(
    CREATE TABLE workspace_state (
        id INTEGER PRIMARY KEY CHECK (id = 1),
        open_document_ids_json TEXT NOT NULL,
        active_document_id TEXT
            REFERENCES documents(id) ON DELETE RESTRICT,
        updated_at_us INTEGER NOT NULL
    )
    )SQL",
			R"SQL(
    CREATE TABLE document_ui_states (
        document_id TEXT PRIMARY KEY
            REFERENCES documents(id) ON DELETE RESTRICT,
        selected_card_id TEXT
            REFERENCES cards(id) ON DELETE RESTRICT,
        list_scroll_position INTEGER NOT NULL,
        sort_mode TEXT NOT NULL
            CHECK (sort_mode IN ('recency', 'position', 'capture')),
        editor_card_id TEXT
            REFERENCES cards(id) ON DELETE RESTRICT,
        editor_base_revision_id TEXT
            REFERENCES card_revisions(id) ON DELETE RESTRICT,
        editor_cursor_qchar INTEGER,
        updated_at_us INTEGER NOT NULL
    )
    )SQL",
			R"SQL(
    CREATE UNIQUE INDEX active_card_position
    ON cards(document_id, position_key)
    WHERE deleted_at_us IS NULL
    )SQL",
			R"SQL(
    CREATE UNIQUE INDEX active_card_draft
    ON drafts(card_id)
    WHERE card_id IS NOT NULL
    )SQL",
			R"SQL(
    INSERT INTO counters(name, next_value)
    VALUES ('capture', 1)
    )SQL",
		};

		// 원본 튜플의 항목 수다. CREATE TABLE 11, CREATE UNIQUE INDEX 2, INSERT 1 이다.
		// v0001 에는 트리거가 없다 - 여덟 트리거는 v0003_storage_invariants 소속이다.
		static_assert(std::size(STATEMENTS) == 14, "STATEMENTS 는 원본과 같은 14 문장이어야 한다.");
	}

	bool Migrate(C_DATABASE& _database, std::int64_t _nAppliedAtUs)
	{
		for (const char* pszStatement : STATEMENTS)
		{
			if (!_database.Execute(pszStatement)) { return(false); }
		}

		// 이식의 유일한 문서화된 편차. 원본은 applied_at_us 를 바인드 파라미터(?)로 넘기지만
		// C_DATABASE::Execute 는 단문 문자열만 받으므로 int64 값을 문장 본문에 리터럴로 넣는다.
		// 결과 데이터베이스 상태와 sqlite_master 는 원본과 같다. 이 문장은 STATEMENTS 소속이
		// 아니므로 게이트가 뽑는 SQL 구분자를 쓰지 않는다.
		const std::string sSchemaVersionSql =
			R"UPSERT(
        INSERT INTO schema_version(id, version, applied_at_us)
        VALUES (1, 1, )UPSERT"
			+ std::to_string(_nAppliedAtUs)
			+ R"UPSERT()
        ON CONFLICT(id) DO UPDATE SET
            version = excluded.version,
            applied_at_us = excluded.applied_at_us
        )UPSERT";

		return(_database.Execute(sSchemaVersionSql));
	}
}
