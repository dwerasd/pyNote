#include <catch_amalgamated.hpp>

#include "pynote/core/storage/migration_runner.h"
#include "pynote/core/storage/migrations/registry.h"

#include <sqlite3/sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#pragma comment(lib, "NoteExCore")

namespace
{
	namespace storage = pynote::core::storage;

	// 시험용 임시 데이터베이스 경로. 소멸 시 본체와 WAL/SHM 사이드카까지 지운다.
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

	int scalar_int(storage::C_DATABASE& _database, const std::string& _sSql)
	{
		sqlite3_stmt* pStmt = nullptr;
		REQUIRE(::sqlite3_prepare_v2(_database.Handle(), _sSql.c_str(), -1, &pStmt, nullptr) == SQLITE_OK);
		REQUIRE(::sqlite3_step(pStmt) == SQLITE_ROW);
		const int nValue = ::sqlite3_column_int(pStmt, 0);
		::sqlite3_finalize(pStmt);
		return(nValue);
	}

	std::int64_t scalar_int64(storage::C_DATABASE& _database, const std::string& _sSql)
	{
		sqlite3_stmt* pStmt = nullptr;
		REQUIRE(::sqlite3_prepare_v2(_database.Handle(), _sSql.c_str(), -1, &pStmt, nullptr) == SQLITE_OK);
		REQUIRE(::sqlite3_step(pStmt) == SQLITE_ROW);
		const std::int64_t nValue = ::sqlite3_column_int64(pStmt, 0);
		::sqlite3_finalize(pStmt);
		return(nValue);
	}

	// PRAGMA 결과의 문자열 열 하나를 모은다.
	std::vector<std::string> text_column(storage::C_DATABASE& _database, const std::string& _sSql, int _nColumn)
	{
		sqlite3_stmt* pStmt = nullptr;
		REQUIRE(::sqlite3_prepare_v2(_database.Handle(), _sSql.c_str(), -1, &pStmt, nullptr) == SQLITE_OK);

		std::vector<std::string> Values;
		while (::sqlite3_step(pStmt) == SQLITE_ROW)
		{
			const unsigned char* pText = ::sqlite3_column_text(pStmt, _nColumn);
			Values.emplace_back(pText == nullptr ? "" : reinterpret_cast<const char*>(pText));
		}
		::sqlite3_finalize(pStmt);
		return(Values);
	}

	bool run_sql(storage::C_DATABASE& _database, const std::string& _sSql)
	{
		return(_database.Execute(_sSql));
	}

	// 새 데이터베이스를 최신 스키마까지 올린다.
	void migrate_to_latest(storage::C_DATABASE& _database, const std::string& _sPath)
	{
		REQUIRE(_database.Open(_sPath));
		storage::C_MIGRATION_RUNNER Runner;
		Runner.SetExistingDatabase(false, _sPath);
		REQUIRE(Runner.Run(_database) == storage::E_MIGRATE_RESULT::Ok);
	}

	// 결속 행 하나를 넣는 최소 준비물(문서·입력 작업·카드)을 원시 SQL 로 세운다.
	void seed_card(storage::C_DATABASE& _database)
	{
		REQUIRE(run_sql(_database,
			"INSERT INTO documents(id, title, created_at_us, updated_at_us) VALUES ('doc', 'T', 1, 1)"));
		REQUIRE(run_sql(_database,
			"INSERT INTO capture_operations(id, document_id, source, split_policy, original_text,"
			" original_hash, original_redacted_at_us, created_at_us)"
			" VALUES ('op', 'doc', 'typing', 'keep', NULL, NULL, NULL, 1)"));
		REQUIRE(run_sql(_database,
			"INSERT INTO cards(id, document_id, operation_id, position_key, capture_seq, created_at_us,"
			" updated_at_us, source, body, body_hash, current_revision_id, deleted_at_us)"
			" VALUES ('card', 'doc', 'op', 1024, 1, 1, 1, 'typing', 'b', 'h', NULL, NULL)"));
	}

	std::string insert_binding(const std::string& _sCardId, const std::string& _sPathKey)
	{
		return("INSERT INTO card_file_bindings(card_id, path, path_key, encoding, bom, newline,"
			" trailing_newline, synced_size, synced_mtime_ns, synced_hash, bound_at_us, synced_at_us)"
			" VALUES ('" + _sCardId + "', 'C:\\N\\a.txt', '" + _sPathKey
			+ "', 'utf-8', 0, 'lf', 1, NULL, NULL, NULL, 1000, NULL)");
	}
}

TEST_CASE("스키마 v10 이 결속 표와 제약을 만든다", "[W1-file-binding][FS-port][WTL-CAP-FB-018]")
{
	C_TEMP_DB           Temp("fb_migration_v0010");
	storage::C_DATABASE Database;
	migrate_to_latest(Database, Temp.Utf8());

	REQUIRE(storage::migrations::LatestSchemaVersion() == 10);
	REQUIRE(Database.SchemaVersion() == 10);
	REQUIRE(scalar_int(Database,
		"SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'card_file_bindings'") == 1);

	// 외래 키는 RESTRICT 다. 원본 시험과 같은 PRAGMA 로 본다.
	const std::vector<std::string> Deletes = text_column(
		Database, "PRAGMA foreign_key_list(card_file_bindings)", 6);
	REQUIRE_FALSE(Deletes.empty());
	for (const std::string& sRule : Deletes)
	{
		REQUIRE(sRule == "RESTRICT");
	}

	// applied_at_us 가 기록된다.
	REQUIRE(scalar_int64(Database, "SELECT applied_at_us FROM schema_version WHERE id = 1") > 0);
}

TEST_CASE("결속 표의 path_key 는 카드가 달라도 유일하다", "[W1-file-binding][FS-port]")
{
	C_TEMP_DB           Temp("fb_migration_unique");
	storage::C_DATABASE Database;
	migrate_to_latest(Database, Temp.Utf8());
	seed_card(Database);
	REQUIRE(run_sql(Database,
		"INSERT INTO cards(id, document_id, operation_id, position_key, capture_seq, created_at_us,"
		" updated_at_us, source, body, body_hash, current_revision_id, deleted_at_us)"
		" VALUES ('card2', 'doc', 'op', 2048, 2, 1, 1, 'typing', 'b', 'h', NULL, NULL)"));

	REQUIRE(run_sql(Database, insert_binding("card", "c:\\n\\a.txt")));
	REQUIRE_FALSE(run_sql(Database, insert_binding("card2", "c:\\n\\a.txt")));
}

TEST_CASE("결속 표의 CHECK 셋이 bom·newline·trailing_newline 값을 거른다", "[W1-file-binding][FS-port]")
{
	C_TEMP_DB           Temp("fb_migration_checks");
	storage::C_DATABASE Database;
	migrate_to_latest(Database, Temp.Utf8());
	seed_card(Database);

	const std::string sBase =
		"INSERT INTO card_file_bindings(card_id, path, path_key, encoding, bom, newline,"
		" trailing_newline, synced_size, synced_mtime_ns, synced_hash, bound_at_us, synced_at_us)"
		" VALUES ('card', 'p', 'k', 'utf-8', ";
	REQUIRE_FALSE(run_sql(Database, sBase + "2, 'lf', 1, NULL, NULL, NULL, 1, NULL)"));
	REQUIRE_FALSE(run_sql(Database, sBase + "0, 'crlfx', 1, NULL, NULL, NULL, 1, NULL)"));
	REQUIRE_FALSE(run_sql(Database, sBase + "0, 'lf', 7, NULL, NULL, NULL, 1, NULL)"));
	// 셋 다 허용 값이면 들어간다.
	REQUIRE(run_sql(Database, sBase + "1, 'cr', 0, NULL, NULL, NULL, 1, NULL)"));
}

TEST_CASE("결속 표의 외래 키는 카드 원시 삭제를 막는다", "[W1-file-binding][FS-port]")
{
	C_TEMP_DB           Temp("fb_migration_restrict");
	storage::C_DATABASE Database;
	migrate_to_latest(Database, Temp.Utf8());
	seed_card(Database);
	REQUIRE(run_sql(Database, insert_binding("card", "c:\\n\\a.txt")));

	REQUIRE_FALSE(run_sql(Database, "DELETE FROM cards WHERE id = 'card'"));
	REQUIRE(scalar_int(Database, "SELECT COUNT(*) FROM cards WHERE id = 'card'") == 1);
}

TEST_CASE("v10 되감기는 표와 행을 지키며 멱등하게 다시 오른다", "[W1-file-binding][FS-port][WTL-CAP-FB-018]")
{
	C_TEMP_DB Temp("fb_migration_rewind");
	{
		storage::C_DATABASE Database;
		migrate_to_latest(Database, Temp.Utf8());
		seed_card(Database);
		REQUIRE(run_sql(Database, insert_binding("card", "c:\\n\\a.txt")));
		// 되감기 시험은 schema_version 만 9 로 낮춘다(원본 시험과 같은 조작이다).
		REQUIRE(run_sql(Database, "UPDATE schema_version SET version = 9, applied_at_us = 9 WHERE id = 1"));
	}

	storage::C_DATABASE Reopened;
	REQUIRE(Reopened.Open(Temp.Utf8()));
	storage::C_MIGRATION_RUNNER Runner;
	Runner.SetExistingDatabase(true, Temp.Utf8());
	REQUIRE(Runner.Run(Reopened) == storage::E_MIGRATE_RESULT::Ok);

	REQUIRE(Reopened.SchemaVersion() == 10);
	REQUIRE(scalar_int(Reopened, "SELECT COUNT(*) FROM card_file_bindings") == 1);
	REQUIRE(text_column(Reopened, "SELECT path_key FROM card_file_bindings", 0).front() == "c:\\n\\a.txt");
}
