#include "pynote/core/storage/migrations/v0010_card_file_bindings.h"

namespace pynote::core::storage::migrations::v0010
{
	namespace
	{
		// 파이썬 원본 v0010_card_file_bindings.py 의 migrate 가 발행하는 문장이다. 들여쓰기와
		// 선행 개행까지 원본 삼중따옴표 리터럴 그대로이며 SQL 축자 게이트가 바이트로 대조한다.
		// 좁은 리터럴은 이 기계에서 CP949 로 변환되므로 SQLite 에 UTF-8 을 넘기려면 u8 이어야 한다.
		constexpr const char8_t* SQL_CREATE_BINDINGS = u8R"SQL(
        CREATE TABLE IF NOT EXISTS card_file_bindings (
            card_id TEXT PRIMARY KEY
                REFERENCES cards(id) ON DELETE RESTRICT,
            path TEXT NOT NULL,
            path_key TEXT NOT NULL UNIQUE,
            encoding TEXT NOT NULL,
            bom INTEGER NOT NULL CHECK(bom IN (0, 1)),
            newline TEXT NOT NULL CHECK(newline IN ('lf', 'crlf', 'cr')),
            trailing_newline INTEGER NOT NULL CHECK(trailing_newline IN (0, 1)),
            synced_size INTEGER,
            synced_mtime_ns INTEGER,
            synced_hash TEXT,
            bound_at_us INTEGER NOT NULL,
            synced_at_us INTEGER
        )
        )SQL";

		constexpr const char8_t* SQL_UPDATE_VERSION = u8R"SQL(
        UPDATE schema_version
        SET version = 10, applied_at_us = ?
        WHERE id = 1
        )SQL";
	}

	bool Migrate(C_DATABASE& _database, std::int64_t _nAppliedAtUs)
	{
		if (!_database.Execute(reinterpret_cast<const char*>(SQL_CREATE_BINDINGS))) { return(false); }

		return(ExecuteBoundInt64(_database, reinterpret_cast<const char*>(SQL_UPDATE_VERSION), _nAppliedAtUs));
	}
}
