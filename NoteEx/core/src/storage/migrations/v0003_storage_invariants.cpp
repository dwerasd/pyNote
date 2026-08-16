#include "pynote/core/storage/migrations/v0003_storage_invariants.h"

#include <sqlite3/sqlite3.h>

#include <iterator>
#include <string>

#pragma comment(lib, "sqlite3")

namespace pynote::core::storage::migrations::v0003
{
	namespace
	{
		// 파이썬 원본 v0003_storage_invariants.py 의 migrate 가 발행하는 문장이다. 앞뒤 개행과 들여쓰기
		// 공백까지 원본 그대로이고 선언 순서가 곧 실행 순서다 - 정적 게이트가 이 리터럴을
		// 순서대로 뽑아 원본과 바이트 비교하므로 구분자는 SQL 로 고정한다. narrow 리터럴은
		// 이 기계에서 CP949 로 변환되므로 SQLite 에 UTF-8 을 넘기려면 u8 리터럴이어야 한다.
		constexpr const char8_t* SQL_SELECT_INVALID_CURRENT = u8R"SQL(
        SELECT cards.id
        FROM cards
        LEFT JOIN card_revisions
          ON card_revisions.id = cards.current_revision_id
        WHERE cards.current_revision_id IS NOT NULL
          AND (
              card_revisions.id IS NULL
              OR card_revisions.card_id != cards.id
              OR card_revisions.body != cards.body
              OR card_revisions.body_hash != cards.body_hash
          )
        LIMIT 1
        )SQL";

		constexpr const char8_t* SQL_SELECT_INVALID_PARENT = u8R"SQL(
        SELECT child.id
        FROM card_revisions AS child
        JOIN card_revisions AS parent
          ON parent.id = child.parent_revision_id
        WHERE child.card_id != parent.card_id
        LIMIT 1
        )SQL";

		// 원본 migrate() 안의 지역 statements 튜플이다(:47~149). 여덟 트리거 전부
		// CREATE TRIGGER IF NOT EXISTS 이고 RAISE(ABORT, ...) 문구는 한국어다.
		constexpr const char8_t* TRIGGERS[] = {
			u8R"SQL(
        CREATE TRIGGER IF NOT EXISTS cards_current_revision_insert
        BEFORE INSERT ON cards
        WHEN NEW.current_revision_id IS NOT NULL
          AND NOT EXISTS (
              SELECT 1
              FROM card_revisions
              WHERE id = NEW.current_revision_id
                AND card_id = NEW.id
                AND body = NEW.body
                AND body_hash = NEW.body_hash
          )
        BEGIN
            SELECT RAISE(ABORT, '카드와 현재 리비전이 일치하지 않습니다');
        END
        )SQL",
			u8R"SQL(
        CREATE TRIGGER IF NOT EXISTS cards_current_revision_update
        BEFORE UPDATE OF current_revision_id, body, body_hash ON cards
        WHEN NEW.current_revision_id IS NOT NULL
          AND NOT EXISTS (
              SELECT 1
              FROM card_revisions
              WHERE id = NEW.current_revision_id
                AND card_id = NEW.id
                AND body = NEW.body
                AND body_hash = NEW.body_hash
          )
        BEGIN
            SELECT RAISE(ABORT, '카드와 현재 리비전이 일치하지 않습니다');
        END
        )SQL",
			u8R"SQL(
        CREATE TRIGGER IF NOT EXISTS card_revisions_parent_insert
        BEFORE INSERT ON card_revisions
        WHEN NEW.parent_revision_id IS NOT NULL
          AND NOT EXISTS (
              SELECT 1
              FROM card_revisions
              WHERE id = NEW.parent_revision_id
                AND card_id = NEW.card_id
          )
        BEGIN
            SELECT RAISE(ABORT, '부모 리비전은 같은 카드에 속해야 합니다');
        END
        )SQL",
			u8R"SQL(
        CREATE TRIGGER IF NOT EXISTS card_revisions_parent_update
        BEFORE UPDATE OF card_id, parent_revision_id ON card_revisions
        WHEN NEW.parent_revision_id IS NOT NULL
          AND NOT EXISTS (
              SELECT 1
              FROM card_revisions
              WHERE id = NEW.parent_revision_id
                AND card_id = NEW.card_id
          )
        BEGIN
            SELECT RAISE(ABORT, '부모 리비전은 같은 카드에 속해야 합니다');
        END
        )SQL",
			u8R"SQL(
        CREATE TRIGGER IF NOT EXISTS card_revisions_current_update
        BEFORE UPDATE OF card_id, body, body_hash ON card_revisions
        WHEN EXISTS (
            SELECT 1
            FROM cards
            WHERE current_revision_id = OLD.id
              AND (
                  id != NEW.card_id
                  OR body != NEW.body
                  OR body_hash != NEW.body_hash
              )
        )
        BEGIN
            SELECT RAISE(ABORT, '현재 리비전과 카드가 일치해야 합니다');
        END
        )SQL",
			u8R"SQL(
        CREATE TRIGGER IF NOT EXISTS capture_counter_no_decrease
        BEFORE UPDATE OF next_value ON counters
        WHEN OLD.name = 'capture' AND NEW.next_value < OLD.next_value
        BEGIN
            SELECT RAISE(ABORT, 'capture counter는 감소시킬 수 없습니다');
        END
        )SQL",
			u8R"SQL(
        CREATE TRIGGER IF NOT EXISTS capture_counter_no_rename
        BEFORE UPDATE OF name ON counters
        WHEN OLD.name = 'capture' AND NEW.name != OLD.name
        BEGIN
            SELECT RAISE(ABORT, 'capture counter는 이름을 바꿀 수 없습니다');
        END
        )SQL",
			u8R"SQL(
        CREATE TRIGGER IF NOT EXISTS capture_counter_no_delete
        BEFORE DELETE ON counters
        WHEN OLD.name = 'capture'
        BEGIN
            SELECT RAISE(ABORT, 'capture counter는 삭제할 수 없습니다');
        END
        )SQL"
		};

		static_assert(std::size(TRIGGERS) == 8, "원본 statements 튜플과 같은 여덟 트리거여야 한다.");

		constexpr const char8_t* SQL_UPDATE_VERSION = u8R"SQL(
        UPDATE schema_version
        SET version = 3, applied_at_us = ?
        WHERE id = 1
        )SQL";

		// 첫 행이 물려 나오는지만 본다. 조회 자체가 실패하면 false 이고 그때 _pFound 는 의미가 없다.
		// 원본은 위반 행의 id 를 예외 메시지에 담으므로(:25, :40) 존재 여부만으로는 부족하다.
		// 행을 물면 첫 열을 _pOffendingId 에 담는다.
		bool first_offending_id(
			C_DATABASE& _database, const char8_t* _pszSql, bool* _pFound, std::string* _pOffendingId)
		{
			sqlite3_stmt* pStmt = nullptr;
			if (::sqlite3_prepare_v2(
				_database.Handle(), reinterpret_cast<const char*>(_pszSql), -1, &pStmt, nullptr) != SQLITE_OK)
			{
				_database.SetLastError(::sqlite3_errmsg(_database.Handle()));
				return(false);
			}

			const int nStep = ::sqlite3_step(pStmt);
			*_pFound = (nStep == SQLITE_ROW);
			if (*_pFound)
			{
				const unsigned char* pText = ::sqlite3_column_text(pStmt, 0);
				_pOffendingId->assign(pText ? reinterpret_cast<const char*>(pText) : "");
			}
			::sqlite3_finalize(pStmt);

			if (nStep != SQLITE_ROW && nStep != SQLITE_DONE)
			{
				_database.SetLastError(::sqlite3_errmsg(_database.Handle()));
				return(false);
			}
			return(true);
		}

		// 원본 _validate_existing_rows(:6~41) 이식이다. 두 조회 중 하나라도 행을 물면 위반이라
		// DDL 에 진입하지 않는다. 원본은 위반 행 id 를 담은 RuntimeError 를 올리므로(:24~26,
		// :38~41) 이식도 같은 문구를 LastError 에 남긴다 - 반환값 규약으로 바꾸는 것과
		// 사유를 잃는 것은 다른 이야기이고, 뒤엣것은 동작 손실이다.
		bool validate_existing_rows(C_DATABASE& _database)
		{
			bool        bFound = false;
			std::string sOffendingId;

			if (!first_offending_id(_database, SQL_SELECT_INVALID_CURRENT, &bFound, &sOffendingId))
			{
				return(false);
			}
			if (bFound)
			{
				_database.SetLastError("카드와 현재 리비전 불변조건이 손상되어 있습니다: " + sOffendingId);
				return(false);
			}

			if (!first_offending_id(_database, SQL_SELECT_INVALID_PARENT, &bFound, &sOffendingId))
			{
				return(false);
			}
			if (bFound)
			{
				_database.SetLastError("리비전 부모 카드 불변조건이 손상되어 있습니다: " + sOffendingId);
				return(false);
			}
			return(true);
		}
	}

	bool Migrate(C_DATABASE& _database, std::int64_t _nAppliedAtUs)
	{
		// 원본은 트리거를 만들기 전에 기존 행부터 검사한다(:46). 순서가 계약이다 -
		// 트리거는 기존 행을 건드리지 않으므로 이 검사를 뒤로 미루면 손상된 행이 살아남는다.
		if (!validate_existing_rows(_database)) { return(false); }

		for (const char8_t* pszTrigger : TRIGGERS)
		{
			if (!_database.Execute(reinterpret_cast<const char*>(pszTrigger))) { return(false); }
		}

		return(ExecuteBoundInt64(_database, reinterpret_cast<const char*>(SQL_UPDATE_VERSION), _nAppliedAtUs));
	}
}
