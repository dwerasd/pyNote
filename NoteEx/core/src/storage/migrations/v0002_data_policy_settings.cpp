#include "pynote/core/storage/migrations/v0002_data_policy_settings.h"

namespace pynote::core::storage::migrations::v0002
{
	namespace
	{
		// 파이썬 원본 v0002_data_policy_settings.py 의 migrate 가 발행하는 문장이다. 앞뒤 개행과 들여쓰기
		// 공백까지 원본 그대로이고 선언 순서가 곧 실행 순서다 - 정적 게이트가 이 리터럴을
		// 순서대로 뽑아 원본과 바이트 비교하므로 구분자는 SQL 로 고정한다. narrow 리터럴은
		// 이 기계에서 CP949 로 변환되므로 SQLite 에 UTF-8 을 넘기려면 u8 리터럴이어야 한다.
		constexpr const char8_t* SQL_CREATE_TABLE = u8R"SQL(
        CREATE TABLE data_policy_settings (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            draft_idle_ms INTEGER NOT NULL CHECK (draft_idle_ms >= 0),
            split_policy TEXT NOT NULL
                CHECK (split_policy IN ('keep', 'split_by_blank_line')),
            preview_lines INTEGER NOT NULL CHECK (preview_lines >= 1),
            backup_interval_hours REAL NOT NULL CHECK (backup_interval_hours > 0),
            trash_retention_days INTEGER NOT NULL CHECK (trash_retention_days >= 0),
            updated_at_us INTEGER NOT NULL
        )
        )SQL";

		constexpr const char8_t* SQL_INSERT_DEFAULTS = u8R"SQL(
        INSERT INTO data_policy_settings(
            id, draft_idle_ms, split_policy, preview_lines,
            backup_interval_hours, trash_retention_days, updated_at_us
        ) VALUES (1, 2000, 'keep', 6, 24, 30, ?)
        )SQL";

		constexpr const char8_t* SQL_UPDATE_VERSION = u8R"SQL(
        UPDATE schema_version
        SET version = 2, applied_at_us = ?
        WHERE id = 1
        )SQL";
	}

	bool Migrate(C_DATABASE& _database, std::int64_t _nAppliedAtUs)
	{
		if (!_database.Execute(reinterpret_cast<const char*>(SQL_CREATE_TABLE))) { return(false); }
		if (!ExecuteBoundInt64(_database, reinterpret_cast<const char*>(SQL_INSERT_DEFAULTS), _nAppliedAtUs))
		{
			return(false);
		}

		return(ExecuteBoundInt64(_database, reinterpret_cast<const char*>(SQL_UPDATE_VERSION), _nAppliedAtUs));
	}
}
