#include <catch_amalgamated.hpp>

#include "pynote/core/storage/migration_runner.h"
#include "pynote/core/storage/migrations/registry.h"
#include "pynote/core/storage/migrations/v0001_initial.h"

#include <sqlite3/sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#pragma comment(lib, "NoteExCore")

namespace
{
	// 시험용 임시 데이터베이스 경로. 소멸 시 본체와 WAL/SHM 사이드카까지 지운다.
	// 사이드카를 남기면 다음 시험이 이전 상태를 물려받아 통과 여부가 순서에 의존하게 된다.
	class C_TEMP_DB
	{
	public:
		explicit C_TEMP_DB(const std::string& _sName)
		{
			m_Path = std::filesystem::temp_directory_path() / ("noteex_test_" + _sName + ".db");
			this->remove_all_();
		}

		~C_TEMP_DB()
		{
			this->remove_all_();
		}

		C_TEMP_DB(const C_TEMP_DB&) = delete;
		C_TEMP_DB& operator=(const C_TEMP_DB&) = delete;

		std::string Utf8() const { return(m_Path.string()); }

	private:
		void remove_all_()
		{
			std::error_code ec;
			std::filesystem::remove(m_Path, ec);
			std::filesystem::remove(m_Path.string() + "-wal", ec);
			std::filesystem::remove(m_Path.string() + "-shm", ec);
		}

		std::filesystem::path m_Path;
	};

	// 원본 STATEMENTS 가 만드는 테이블 전부다(CREATE TABLE 11 건, 등록 순서 그대로).
	const char* const TABLE_NAMES[] = {
		"schema_version",
		"documents",
		"capture_operations",
		"cards",
		"edit_events",
		"card_revisions",
		"drafts",
		"card_lineage",
		"counters",
		"workspace_state",
		"document_ui_states"
	};

	int scalar_int(pynote::core::storage::C_DATABASE& _database, const std::string& _sSql)
	{
		sqlite3_stmt* pStmt = nullptr;
		REQUIRE(::sqlite3_prepare_v2(_database.Handle(), _sSql.c_str(), -1, &pStmt, nullptr) == SQLITE_OK);
		REQUIRE(::sqlite3_step(pStmt) == SQLITE_ROW);
		const int nValue = ::sqlite3_column_int(pStmt, 0);
		::sqlite3_finalize(pStmt);
		return(nValue);
	}

	std::int64_t scalar_int64(pynote::core::storage::C_DATABASE& _database, const std::string& _sSql)
	{
		sqlite3_stmt* pStmt = nullptr;
		REQUIRE(::sqlite3_prepare_v2(_database.Handle(), _sSql.c_str(), -1, &pStmt, nullptr) == SQLITE_OK);
		REQUIRE(::sqlite3_step(pStmt) == SQLITE_ROW);
		const std::int64_t nValue = ::sqlite3_column_int64(pStmt, 0);
		::sqlite3_finalize(pStmt);
		return(nValue);
	}

	std::string scalar_text(pynote::core::storage::C_DATABASE& _database, const std::string& _sSql)
	{
		sqlite3_stmt* pStmt = nullptr;
		REQUIRE(::sqlite3_prepare_v2(_database.Handle(), _sSql.c_str(), -1, &pStmt, nullptr) == SQLITE_OK);
		REQUIRE(::sqlite3_step(pStmt) == SQLITE_ROW);
		const unsigned char* pText = ::sqlite3_column_text(pStmt, 0);
		const std::string sValue(pText ? reinterpret_cast<const char*>(pText) : "");
		::sqlite3_finalize(pStmt);
		return(sValue);
	}

	// PRAGMA foreign_key_list 의 ON DELETE 동작 열(인덱스 6)만 모은다.
	std::vector<std::string> delete_actions(pynote::core::storage::C_DATABASE& _database, const std::string& _sTable)
	{
		std::vector<std::string> Actions;
		const std::string sSql = "PRAGMA foreign_key_list(" + _sTable + ")";
		sqlite3_stmt* pStmt = nullptr;
		REQUIRE(::sqlite3_prepare_v2(_database.Handle(), sSql.c_str(), -1, &pStmt, nullptr) == SQLITE_OK);
		while (::sqlite3_step(pStmt) == SQLITE_ROW)
		{
			const unsigned char* pText = ::sqlite3_column_text(pStmt, 6);
			Actions.emplace_back(pText ? reinterpret_cast<const char*>(pText) : "");
		}
		::sqlite3_finalize(pStmt);
		return(Actions);
	}
}

// 대응 원본: tests/integration/test_database.py::test_new_v0_database_migrates_to_latest
//            tests/integration/test_database.py::test_required_partial_unique_indexes_exist
//            tests/integration/test_database.py::test_all_foreign_keys_use_delete_restrict
// (원본 시험은 v9 스키마를 보므로 기대 집합이 다르다. 여기서는 v0001 이 만드는 것만 본다.)
TEST_CASE("v0001 은 원본 STATEMENTS 가 만드는 스키마 객체를 그대로 만든다", "[core][storage][migration]")
{
	const C_TEMP_DB temp("v0001_schema");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));

	pynote::core::storage::C_MIGRATION_RUNNER runner;
	runner.SetExistingDatabase(false, temp.Utf8());
	REQUIRE(runner.Run(db) == pynote::core::storage::E_MIGRATE_RESULT::Ok);

	for (const char* pszName : TABLE_NAMES)
	{
		INFO(pszName);
		REQUIRE(scalar_int(db,
			std::string("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = '")
			+ pszName + "'") == 1);
	}

	// sqlite_sequence 는 edit_events 의 AUTOINCREMENT 때문에 SQLite 가 스스로 만든다.
	REQUIRE(scalar_int(db,
		"SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'") == 11);

	// v0001 에는 트리거가 없다 - 여덟 트리거는 v0003_storage_invariants 소속이다.
	REQUIRE(scalar_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'trigger'") == 0);

	// 부분 유니크 인덱스 두 건은 WHERE 절까지 원본 그대로여야 한다.
	const std::string sPositionIndex = scalar_text(db,
		"SELECT sql FROM sqlite_master WHERE type = 'index' AND name = 'active_card_position'");
	REQUIRE(sPositionIndex.find("ON cards(document_id, position_key)") != std::string::npos);
	REQUIRE(sPositionIndex.find("WHERE deleted_at_us IS NULL") != std::string::npos);

	const std::string sDraftIndex = scalar_text(db,
		"SELECT sql FROM sqlite_master WHERE type = 'index' AND name = 'active_card_draft'");
	REQUIRE(sDraftIndex.find("ON drafts(card_id)") != std::string::npos);
	REQUIRE(sDraftIndex.find("WHERE card_id IS NOT NULL") != std::string::npos);

	// 모든 외래키는 ON DELETE RESTRICT 다.
	for (const char* pszName : TABLE_NAMES)
	{
		INFO(pszName);
		for (const std::string& sAction : delete_actions(db, pszName))
		{
			REQUIRE(sAction == "RESTRICT");
		}
	}
}

// 대응 원본: v0001_initial.py 의 STATEMENTS 마지막 항목 (INSERT INTO counters, :176~179).
// pytest node ID: tests/integration/test_database.py::test_new_v0_database_migrates_to_latest
// (원본 시험이 같은 시드를 확인한다).
TEST_CASE("v0001 은 counters 에 ('capture', 1) 하나만 시드한다", "[core][storage][migration]")
{
	const C_TEMP_DB temp("v0001_counters");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));

	pynote::core::storage::C_MIGRATION_RUNNER runner;
	runner.SetExistingDatabase(false, temp.Utf8());
	REQUIRE(runner.Run(db) == pynote::core::storage::E_MIGRATE_RESULT::Ok);

	REQUIRE(scalar_int(db, "SELECT COUNT(*) FROM counters") == 1);
	REQUIRE(scalar_int(db, "SELECT next_value FROM counters WHERE name = 'capture'") == 1);
}

// 대응 원본: v0001_initial.py 의 migrate() 말미 schema_version upsert (:187~196).
// pytest node ID: tests/integration/test_database.py::test_new_v0_database_migrates_to_latest
// (원본 시험은 버전만 보고 applied_at_us 는 보지 않는다 - 그쪽은 W0 T4 역보강 대상이다.)
TEST_CASE("v0001 은 schema_version 을 (1, 1, epoch 마이크로초) 로 남긴다", "[core][storage][migration]")
{
	const C_TEMP_DB temp("v0001_schema_version");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));

	pynote::core::storage::C_MIGRATION_RUNNER runner;
	runner.SetExistingDatabase(false, temp.Utf8());
	REQUIRE(runner.Run(db) == pynote::core::storage::E_MIGRATE_RESULT::Ok);

	REQUIRE(scalar_int(db, "SELECT COUNT(*) FROM schema_version") == 1);
	REQUIRE(scalar_int(db, "SELECT id FROM schema_version") == 1);
	REQUIRE(scalar_int(db, "SELECT version FROM schema_version WHERE id = 1") == 1);

	const std::int64_t nAppliedAtUs = scalar_int64(db, "SELECT applied_at_us FROM schema_version WHERE id = 1");
	// 2020-01-01 과 2100-01-01 사이면 초도 밀리초도 나노초도 아닌 마이크로초다.
	REQUIRE(nAppliedAtUs > 1577836800000000LL);
	REQUIRE(nAppliedAtUs < 4102444800000000LL);
}

// 대응 원본: v0001_initial.py::migrate (:187~196). 이식의 유일한 문서화된 편차 - 바인드
// 파라미터가 리터럴로 바뀐 자리를 확인한다. 파이썬 시험 트리에 대응 케이스가 없어
// pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("문서화된 편차: applied_at_us 리터럴은 준 int64 를 그대로 저장한다", "[core][storage][migration]")
{
	const C_TEMP_DB temp("v0001_applied_literal");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));

	// 2^53 + 1 은 double 을 거치면 값이 바뀐다. 정수 경로로 그대로 실려야 한다.
	const std::int64_t nAppliedAtUs = 9007199254740993LL;
	REQUIRE(pynote::core::storage::migrations::v0001::Migrate(db, nAppliedAtUs));

	REQUIRE(scalar_int64(db, "SELECT applied_at_us FROM schema_version WHERE id = 1") == nAppliedAtUs);
	REQUIRE(scalar_int(db, "SELECT version FROM schema_version WHERE id = 1") == 1);
}
