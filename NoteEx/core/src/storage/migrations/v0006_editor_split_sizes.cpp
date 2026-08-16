#include "pynote/core/storage/migrations/v0006_editor_split_sizes.h"

namespace pynote::core::storage::migrations::v0006
{
	namespace
	{
		// 파이썬 원본 v0006_editor_split_sizes.py 의 migrate 가 발행하는 문장이다. 앞뒤 개행과 들여쓰기
		// 공백까지 원본 그대로이고 선언 순서가 곧 실행 순서다 - 정적 게이트가 이 리터럴을
		// 순서대로 뽑아 원본과 바이트 비교하므로 구분자는 SQL 로 고정한다. narrow 리터럴은
		// 이 기계에서 CP949 로 변환되므로 SQLite 에 UTF-8 을 넘기려면 u8 리터럴이어야 한다.
		constexpr const char8_t* SQL_ADD_SPLIT_LEFT = u8R"SQL(ALTER TABLE document_ui_states ADD COLUMN editor_split_left INTEGER)SQL";

		constexpr const char8_t* SQL_ADD_SPLIT_RIGHT = u8R"SQL(ALTER TABLE document_ui_states ADD COLUMN editor_split_right INTEGER)SQL";

		constexpr const char8_t* SQL_UPDATE_VERSION = u8R"SQL(
        UPDATE schema_version
        SET version = 6, applied_at_us = ?
        WHERE id = 1
        )SQL";
	}

	bool Migrate(C_DATABASE& _database, std::int64_t _nAppliedAtUs)
	{
		if (!_database.Execute(reinterpret_cast<const char*>(SQL_ADD_SPLIT_LEFT))) { return(false); }
		if (!_database.Execute(reinterpret_cast<const char*>(SQL_ADD_SPLIT_RIGHT))) { return(false); }

		return(ExecuteBoundInt64(_database, reinterpret_cast<const char*>(SQL_UPDATE_VERSION), _nAppliedAtUs));
	}
}
