#include "pynote/core/storage/migrations/v0005_recency_sort_mode.h"

namespace pynote::core::storage::migrations::v0005
{
	namespace
	{
		// 파이썬 원본 v0005_recency_sort_mode.py 의 migrate 가 발행하는 문장이다. 앞뒤 개행과 들여쓰기
		// 공백까지 원본 그대로이고 선언 순서가 곧 실행 순서다 - 정적 게이트가 이 리터럴을
		// 순서대로 뽑아 원본과 바이트 비교하므로 구분자는 SQL 로 고정한다. narrow 리터럴은
		// 이 기계에서 CP949 로 변환되므로 SQLite 에 UTF-8 을 넘기려면 u8 리터럴이어야 한다.
		constexpr const char8_t* SQL_RENAME_OLD = u8R"SQL(ALTER TABLE document_ui_states RENAME TO document_ui_states_v4)SQL";

		constexpr const char8_t* SQL_CREATE_TABLE = u8R"SQL(
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
        )SQL";

		constexpr const char8_t* SQL_COPY_ROWS = u8R"SQL(
        INSERT INTO document_ui_states(
            document_id, selected_card_id, list_scroll_position, sort_mode,
            editor_card_id, editor_base_revision_id, editor_cursor_qchar,
            updated_at_us
        )
        SELECT
            document_id, selected_card_id, list_scroll_position, sort_mode,
            editor_card_id, editor_base_revision_id, editor_cursor_qchar,
            updated_at_us
        FROM document_ui_states_v4
        )SQL";

		constexpr const char8_t* SQL_DROP_OLD = u8R"SQL(DROP TABLE document_ui_states_v4)SQL";

		constexpr const char8_t* SQL_UPDATE_VERSION = u8R"SQL(
        UPDATE schema_version
        SET version = 5, applied_at_us = ?
        WHERE id = 1
        )SQL";
	}

	bool Migrate(C_DATABASE& _database, std::int64_t _nAppliedAtUs)
	{
		if (!_database.Execute(reinterpret_cast<const char*>(SQL_RENAME_OLD))) { return(false); }
		if (!_database.Execute(reinterpret_cast<const char*>(SQL_CREATE_TABLE))) { return(false); }
		if (!_database.Execute(reinterpret_cast<const char*>(SQL_COPY_ROWS))) { return(false); }
		if (!_database.Execute(reinterpret_cast<const char*>(SQL_DROP_OLD))) { return(false); }

		return(ExecuteBoundInt64(_database, reinterpret_cast<const char*>(SQL_UPDATE_VERSION), _nAppliedAtUs));
	}
}
