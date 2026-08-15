#include <catch_amalgamated.hpp>

#include "pynote/core/storage/database.h"

#include <sqlite3/sqlite3.h>

#include <filesystem>
#include <string>

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
		const std::filesystem::path& Path() const { return(m_Path); }

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
}

TEST_CASE("파일 데이터베이스를 열면 WAL 과 외래키가 실제로 켜져 있다", "[core][storage]")
{
	const C_TEMP_DB temp("open");
	pynote::core::storage::C_DATABASE db;

	REQUIRE(db.Open(temp.Utf8()));
	REQUIRE(db.IsOpen());

	// 계약은 PRAGMA 를 던지는 것이 아니라 실제로 켜졌는지다. 연결에 직접 되물어 확인한다.
	sqlite3_stmt* pStmt = nullptr;
	REQUIRE(::sqlite3_prepare_v2(db.Handle(), "PRAGMA journal_mode", -1, &pStmt, nullptr) == SQLITE_OK);
	REQUIRE(::sqlite3_step(pStmt) == SQLITE_ROW);
	const std::string sJournal(reinterpret_cast<const char*>(::sqlite3_column_text(pStmt, 0)));
	::sqlite3_finalize(pStmt);
	REQUIRE(sJournal == "wal");

	REQUIRE(::sqlite3_prepare_v2(db.Handle(), "PRAGMA foreign_keys", -1, &pStmt, nullptr) == SQLITE_OK);
	REQUIRE(::sqlite3_step(pStmt) == SQLITE_ROW);
	REQUIRE(::sqlite3_column_int(pStmt, 0) == 1);
	::sqlite3_finalize(pStmt);
}

TEST_CASE("schema_version 테이블이 없으면 버전은 0", "[core][storage]")
{
	const C_TEMP_DB temp("version_absent");
	pynote::core::storage::C_DATABASE db;

	REQUIRE(db.Open(temp.Utf8()));
	REQUIRE(db.SchemaVersion() == 0);
}

TEST_CASE("schema_version 값을 읽는다", "[core][storage]")
{
	const C_TEMP_DB temp("version_present");
	pynote::core::storage::C_DATABASE db;

	REQUIRE(db.Open(temp.Utf8()));
	REQUIRE(db.Execute("CREATE TABLE schema_version (id INTEGER PRIMARY KEY, version INTEGER NOT NULL)"));
	REQUIRE(db.Execute("INSERT INTO schema_version (id, version) VALUES (1, 9)"));

	REQUIRE(db.SchemaVersion() == 9);
}

TEST_CASE("커밋한 트랜잭션의 변경은 남는다", "[core][storage]")
{
	const C_TEMP_DB temp("tx_commit");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));
	REQUIRE(db.Execute("CREATE TABLE t (v INTEGER NOT NULL)"));

	{
		pynote::core::storage::C_TRANSACTION tx(db);
		REQUIRE(tx.IsActive());
		REQUIRE(db.Execute("INSERT INTO t (v) VALUES (1)"));
		REQUIRE(tx.Commit());
		REQUIRE_FALSE(tx.IsActive());
	}

	sqlite3_stmt* pStmt = nullptr;
	REQUIRE(::sqlite3_prepare_v2(db.Handle(), "SELECT COUNT(*) FROM t", -1, &pStmt, nullptr) == SQLITE_OK);
	REQUIRE(::sqlite3_step(pStmt) == SQLITE_ROW);
	REQUIRE(::sqlite3_column_int(pStmt, 0) == 1);
	::sqlite3_finalize(pStmt);
}

TEST_CASE("커밋하지 않고 범위를 벗어나면 전부 롤백된다", "[core][storage]")
{
	const C_TEMP_DB temp("tx_rollback");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));
	REQUIRE(db.Execute("CREATE TABLE t (v INTEGER NOT NULL)"));

	{
		pynote::core::storage::C_TRANSACTION tx(db);
		REQUIRE(tx.IsActive());
		REQUIRE(db.Execute("INSERT INTO t (v) VALUES (1)"));
		REQUIRE(db.Execute("INSERT INTO t (v) VALUES (2)"));
		// Commit 을 부르지 않고 나간다.
	}

	sqlite3_stmt* pStmt = nullptr;
	REQUIRE(::sqlite3_prepare_v2(db.Handle(), "SELECT COUNT(*) FROM t", -1, &pStmt, nullptr) == SQLITE_OK);
	REQUIRE(::sqlite3_step(pStmt) == SQLITE_ROW);
	REQUIRE(::sqlite3_column_int(pStmt, 0) == 0);
	::sqlite3_finalize(pStmt);
}

TEST_CASE("중첩 트랜잭션은 시작되지 않는다", "[core][storage]")
{
	const C_TEMP_DB temp("tx_nested");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));

	pynote::core::storage::C_TRANSACTION outer(db);
	REQUIRE(outer.IsActive());

	// 안쪽은 시작되지 않아야 한다. 시작됐다면 바깥 트랜잭션의 원자성이 깨진다.
	pynote::core::storage::C_TRANSACTION inner(db);
	REQUIRE_FALSE(inner.IsActive());
	REQUIRE_FALSE(inner.Commit());

	// 안쪽이 소멸해도 바깥은 살아 있어야 한다.
	REQUIRE(outer.IsActive());
}

TEST_CASE("열리지 않은 연결은 실패를 사유와 함께 보고한다", "[core][storage]")
{
	pynote::core::storage::C_DATABASE db;

	REQUIRE_FALSE(db.IsOpen());
	REQUIRE_FALSE(db.Execute("SELECT 1"));
	REQUIRE_FALSE(db.LastError().empty());
}
