#include "pynote/core/storage/migrations/v0004_workspace_windows.h"

namespace pynote::core::storage::migrations::v0004
{
	namespace
	{
		// 파이썬 원본 v0004_workspace_windows.py 의 migrate 가 발행하는 문장이다. 앞뒤 개행과 들여쓰기
		// 공백까지 원본 그대로이고 선언 순서가 곧 실행 순서다 - 정적 게이트가 이 리터럴을
		// 순서대로 뽑아 원본과 바이트 비교하므로 구분자는 SQL 로 고정한다. narrow 리터럴은
		// 이 기계에서 CP949 로 변환되므로 SQLite 에 UTF-8 을 넘기려면 u8 리터럴이어야 한다.
		constexpr const char8_t* SQL_CREATE_TABLE = u8R"SQL(
        CREATE TABLE workspace_windows (
            window_id TEXT PRIMARY KEY,
            open_document_ids_json TEXT NOT NULL,
            active_document_id TEXT
                REFERENCES documents(id) ON DELETE RESTRICT,
            updated_at_us INTEGER NOT NULL
        )
        )SQL";

		constexpr const char8_t* SQL_COPY_FROM_STATE = u8R"SQL(
        INSERT INTO workspace_windows(
            window_id, open_document_ids_json, active_document_id, updated_at_us
        )
        SELECT
            lower(
                hex(randomblob(4)) || '-' ||
                hex(randomblob(2)) || '-4' ||
                substr(hex(randomblob(2)), 2) || '-' ||
                substr('89ab', abs(random()) % 4 + 1, 1) ||
                substr(hex(randomblob(2)), 2) || '-' ||
                hex(randomblob(6))
            ),
            open_document_ids_json,
            active_document_id,
            updated_at_us
        FROM workspace_state
        WHERE id = 1
        )SQL";

		constexpr const char8_t* SQL_DROP_STATE = u8R"SQL(DROP TABLE workspace_state)SQL";

		constexpr const char8_t* SQL_UPDATE_VERSION = u8R"SQL(
        UPDATE schema_version
        SET version = 4, applied_at_us = ?
        WHERE id = 1
        )SQL";
	}

	bool Migrate(C_DATABASE& _database, std::int64_t _nAppliedAtUs)
	{
		if (!_database.Execute(reinterpret_cast<const char*>(SQL_CREATE_TABLE))) { return(false); }

		// workspace_state 에 id = 1 행이 없으면 아무것도 옮기지 않는다. 원본도 마찬가지로
		// INSERT ... SELECT 가 0 행을 넣고 지나간다 - 빈 결과는 실패가 아니다.
		if (!_database.Execute(reinterpret_cast<const char*>(SQL_COPY_FROM_STATE))) { return(false); }
		if (!_database.Execute(reinterpret_cast<const char*>(SQL_DROP_STATE))) { return(false); }

		return(ExecuteBoundInt64(_database, reinterpret_cast<const char*>(SQL_UPDATE_VERSION), _nAppliedAtUs));
	}
}
