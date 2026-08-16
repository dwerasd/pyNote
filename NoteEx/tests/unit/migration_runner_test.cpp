#include <catch_amalgamated.hpp>

#include "pynote/core/storage/migration_runner.h"
#include "pynote/core/storage/migrations/registry.h"

#include <sqlite3/sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <span>
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

	// 주입용 가짜 마이그레이션. MigrateFn 이 함수 포인터라 호출 기록은 파일 범위 상태에 남긴다.
	int                       g_nAlphaCalls = 0;
	int                       g_nBetaCalls  = 0;
	int                       g_nFailCalls  = 0;
	std::vector<std::int64_t> g_AppliedAtUs;

	void reset_fakes()
	{
		g_nAlphaCalls = 0;
		g_nBetaCalls  = 0;
		g_nFailCalls  = 0;
		g_AppliedAtUs.clear();
	}

	bool fake_alpha(pynote::core::storage::C_DATABASE& _database, std::int64_t _nAppliedAtUs)
	{
		++g_nAlphaCalls;
		g_AppliedAtUs.push_back(_nAppliedAtUs);
		return(_database.Execute("CREATE TABLE alpha (v INTEGER NOT NULL)"));
	}

	bool fake_beta(pynote::core::storage::C_DATABASE& _database, std::int64_t _nAppliedAtUs)
	{
		++g_nBetaCalls;
		g_AppliedAtUs.push_back(_nAppliedAtUs);
		return(_database.Execute("CREATE TABLE beta (v INTEGER NOT NULL)"));
	}

	bool fake_fails(pynote::core::storage::C_DATABASE&, std::int64_t)
	{
		++g_nFailCalls;
		return(false);
	}

	// 단일 정수 결과를 읽는다. 행이 없으면 시험을 실패시킨다.
	int scalar_int(pynote::core::storage::C_DATABASE& _database, const std::string& _sSql)
	{
		sqlite3_stmt* pStmt = nullptr;
		REQUIRE(::sqlite3_prepare_v2(_database.Handle(), _sSql.c_str(), -1, &pStmt, nullptr) == SQLITE_OK);
		REQUIRE(::sqlite3_step(pStmt) == SQLITE_ROW);
		const int nValue = ::sqlite3_column_int(pStmt, 0);
		::sqlite3_finalize(pStmt);
		return(nValue);
	}

	// sqlite_master 에 그 이름의 테이블이 있는지 센다.
	int table_count(pynote::core::storage::C_DATABASE& _database, const std::string& _sName)
	{
		return(scalar_int(_database,
			"SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = '" + _sName + "'"));
	}

	// 원본 fixture 와 같은 v0 데이터베이스를 만든다
	// (tests/integration/test_database.py::test_previous_schema_fixture_migrates_after_backup_hook).
	void seed_schema_version(pynote::core::storage::C_DATABASE& _database, int _nVersion)
	{
		REQUIRE(_database.Execute(
			"CREATE TABLE schema_version ("
			" id INTEGER PRIMARY KEY CHECK (id = 1),"
			" version INTEGER NOT NULL CHECK (version >= 0),"
			" applied_at_us INTEGER NOT NULL)"));
		REQUIRE(_database.Execute(
			"INSERT INTO schema_version(id, version, applied_at_us) VALUES (1, "
			+ std::to_string(_nVersion) + ", 0)"));
	}
}

// 대응 원본: tests/integration/test_database.py::test_new_v0_database_migrates_to_latest
// (원본은 v9 까지 올리지만 T-R1 레지스트리의 LATEST 는 1 이다.)
TEST_CASE("신규 데이터베이스는 백업 훅을 부르지 않고 최신 버전까지 올린다", "[core][storage][migration]")
{
	const C_TEMP_DB temp("migrate_new");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));

	int nHookCalls = 0;
	pynote::core::storage::C_MIGRATION_RUNNER runner;
	runner.SetExistingDatabase(false, temp.Utf8());
	runner.SetBackupHook([&nHookCalls](const std::string&, int, int) { ++nHookCalls; return(true); });

	REQUIRE(runner.Run(db) == pynote::core::storage::E_MIGRATE_RESULT::Ok);

	// _had_database 가 false 면 훅이 주입돼 있어도 부르지 않는다(database.py:107).
	REQUIRE(nHookCalls == 0);
	REQUIRE(db.SchemaVersion() == pynote::core::storage::migrations::LatestSchemaVersion());
}

// 대응 원본: tests/integration/test_database.py::test_previous_schema_fixture_migrates_after_backup_hook
TEST_CASE("기존 v0 데이터베이스는 백업 훅을 거쳐 올라간다", "[core][storage][migration]")
{
	const C_TEMP_DB temp("migrate_existing_v0");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));
	seed_schema_version(db, 0);

	std::vector<std::string> HookPaths;
	std::vector<int>         HookFrom;
	std::vector<int>         HookTo;
	pynote::core::storage::C_MIGRATION_RUNNER runner;
	runner.SetExistingDatabase(true, temp.Utf8());
	runner.SetBackupHook([&](const std::string& _sPath, int _nCurrent, int _nLatest)
		{
			HookPaths.push_back(_sPath);
			HookFrom.push_back(_nCurrent);
			HookTo.push_back(_nLatest);
			return(true);
		});

	REQUIRE(runner.Run(db) == pynote::core::storage::E_MIGRATE_RESULT::Ok);

	REQUIRE(HookPaths.size() == 1);
	REQUIRE(HookPaths[0] == temp.Utf8());
	REQUIRE(HookFrom[0] == 0);
	// 세 번째 인자는 적용될 pending 의 최대 버전이 아니라 항상 LATEST 다(database.py:109).
	REQUIRE(HookTo[0] == pynote::core::storage::migrations::LatestSchemaVersion());
	REQUIRE(db.SchemaVersion() == pynote::core::storage::migrations::LatestSchemaVersion());

	// pending 이 없으면 훅을 부르기 전에 조기 반환한다(:104~107). 두 번째 실행은 훅을 부르지 않는다.
	REQUIRE(runner.Run(db) == pynote::core::storage::E_MIGRATE_RESULT::Ok);
	REQUIRE(HookPaths.size() == 1);
}

// 대응 원본: src/pynote/infrastructure/database.py 의 Database._migrate (:99~100).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("현재 버전이 LATEST 보다 크면 거부한다", "[core][storage][migration]")
{
	const C_TEMP_DB temp("migrate_unsupported");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));
	seed_schema_version(db, 99);

	reset_fakes();
	const pynote::core::storage::S_MIGRATION Fakes[] = { { 1, &fake_alpha } };

	pynote::core::storage::C_MIGRATION_RUNNER runner;
	runner.SetExistingDatabase(true, temp.Utf8());

	REQUIRE(runner.Run(db, std::span<const pynote::core::storage::S_MIGRATION>(Fakes), 1)
		== pynote::core::storage::E_MIGRATE_RESULT::UnsupportedVersion);
	REQUIRE(g_nAlphaCalls == 0);
	REQUIRE_FALSE(runner.LastError().empty());
}

// 대응 원본: src/pynote/infrastructure/database.py 의 Database._migrate (:107~112 - 훅 실패 전파).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("백업 훅이 실패하면 마이그레이션을 하나도 적용하지 않는다", "[core][storage][migration]")
{
	const C_TEMP_DB temp("migrate_hook_failed");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));
	seed_schema_version(db, 0);

	reset_fakes();
	const pynote::core::storage::S_MIGRATION Fakes[] = { { 1, &fake_alpha } };

	pynote::core::storage::C_MIGRATION_RUNNER runner;
	runner.SetExistingDatabase(true, temp.Utf8());
	runner.SetBackupHook([](const std::string&, int, int) { return(false); });

	REQUIRE(runner.Run(db, std::span<const pynote::core::storage::S_MIGRATION>(Fakes), 1)
		== pynote::core::storage::E_MIGRATE_RESULT::BackupHookFailed);
	REQUIRE(g_nAlphaCalls == 0);
	REQUIRE(table_count(db, "alpha") == 0);
	REQUIRE(db.SchemaVersion() == 0);
}

// 대응 원본: src/pynote/infrastructure/database.py 의 transaction() 과 _migrate 적용 루프
// (:54~66, :114~124). 파이썬 시험 트리에 대응 케이스가 없어 node ID 는 W0 T4 역보강 대기다.
TEST_CASE("중도 실패하면 앞선 마이그레이션까지 전부 롤백한다", "[core][storage][migration]")
{
	const C_TEMP_DB temp("migrate_rollback");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));

	reset_fakes();
	const pynote::core::storage::S_MIGRATION Fakes[] = {
		{ 1, &fake_alpha },
		{ 2, &fake_beta },
		{ 3, &fake_fails }
	};

	pynote::core::storage::C_MIGRATION_RUNNER runner;
	runner.SetExistingDatabase(false, temp.Utf8());

	REQUIRE(runner.Run(db, std::span<const pynote::core::storage::S_MIGRATION>(Fakes), 3)
		== pynote::core::storage::E_MIGRATE_RESULT::MigrationFailed);

	// 앞선 두 본은 실제로 돌았다.
	REQUIRE(g_nAlphaCalls == 1);
	REQUIRE(g_nBetaCalls == 1);
	REQUIRE(g_nFailCalls == 1);

	// 그래도 하나의 트랜잭션이므로 성공했던 두 본의 결과도 남지 않는다.
	REQUIRE(table_count(db, "alpha") == 0);
	REQUIRE(table_count(db, "beta") == 0);
	REQUIRE_FALSE(runner.LastError().empty());
}

// 대응 원본: src/pynote/infrastructure/database.py 의 _migrate 적용 루프 (:117,
// time.time_ns() // 1_000). 파이썬 시험 트리에 대응 케이스가 없어 node ID 는 W0 T4 역보강 대기다.
TEST_CASE("applied_at_us 는 본마다 다시 읽은 epoch 마이크로초다", "[core][storage][migration]")
{
	const C_TEMP_DB temp("migrate_clock");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));

	reset_fakes();
	const pynote::core::storage::S_MIGRATION Fakes[] = {
		{ 1, &fake_alpha },
		{ 2, &fake_beta }
	};

	pynote::core::storage::C_MIGRATION_RUNNER runner;
	runner.SetExistingDatabase(false, temp.Utf8());

	REQUIRE(runner.Run(db, std::span<const pynote::core::storage::S_MIGRATION>(Fakes), 2)
		== pynote::core::storage::E_MIGRATE_RESULT::Ok);

	REQUIRE(g_AppliedAtUs.size() == 2);
	for (const std::int64_t nAppliedAtUs : g_AppliedAtUs)
	{
		// 2020-01-01 과 2100-01-01 사이면 초도 밀리초도 나노초도 아닌 마이크로초다.
		REQUIRE(nAppliedAtUs > 1577836800000000LL);
		REQUIRE(nAppliedAtUs < 4102444800000000LL);
	}
	// 시계 해상도가 두 호출을 같은 값으로 접을 수 있으므로 등호를 허용한다.
	// 본마다 다시 읽는다는 사실 자체는 이 단언이 아니라 러너 구현이 보증한다.
	REQUIRE(g_AppliedAtUs[1] >= g_AppliedAtUs[0]);
}

// 대응 원본 없음. 반환값 오류 규약 때문에 이식에서 새로 생긴 경로다 - 파이썬은 같은 상황에서
// sqlite3 예외를 올린다. 계약 정의는 migration_runner.h 의 E_MIGRATE_RESULT::VersionReadFailed 이고
// 구별해야 하는 이유는 database.py:84~95 가 조회 실패를 버전 0 으로 접지 않기 때문이다.
TEST_CASE("스키마 버전 조회 실패는 버전 0 과 구별해 보고한다", "[core][storage][migration]")
{
	pynote::core::storage::C_DATABASE db;
	pynote::core::storage::C_MIGRATION_RUNNER runner;

	REQUIRE_FALSE(db.IsOpen());
	REQUIRE(runner.Run(db) == pynote::core::storage::E_MIGRATE_RESULT::VersionReadFailed);
	REQUIRE_FALSE(runner.LastError().empty());
}

// 대응 원본: src/pynote/infrastructure/database.py 의 transaction() 중첩 거부 (:57~58).
// 파이썬 시험 트리에 대응 케이스가 없어 node ID 는 W0 T4 역보강 대기다.
TEST_CASE("이미 트랜잭션 중이면 마이그레이션을 시작하지 않는다", "[core][storage][migration]")
{
	const C_TEMP_DB temp("migrate_nested_tx");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));

	pynote::core::storage::C_TRANSACTION outer(db);
	REQUIRE(outer.IsActive());

	reset_fakes();
	const pynote::core::storage::S_MIGRATION Fakes[] = { { 1, &fake_alpha } };

	pynote::core::storage::C_MIGRATION_RUNNER runner;
	runner.SetExistingDatabase(false, temp.Utf8());

	REQUIRE(runner.Run(db, std::span<const pynote::core::storage::S_MIGRATION>(Fakes), 1)
		== pynote::core::storage::E_MIGRATE_RESULT::TransactionFailed);
	REQUIRE(g_nAlphaCalls == 0);
	REQUIRE(outer.IsActive());
}
