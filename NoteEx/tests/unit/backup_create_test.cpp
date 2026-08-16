#include "backup_test_support.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#pragma comment(lib, "NoteExCore")

namespace
{
	using namespace backup_support;

	// 2026-01-02T00:00:00Z 의 epoch 마이크로초다. 파이썬 시험이 훅에 물리는 고정 시각과 같다
	// (tests/integration/test_backup.py:425).
	const std::int64_t CLOCK_20260102 = 1767312000LL * 1000000LL;

	bool contains(const std::string& _sText, const char* _pszNeedle)
	{
		return(_sText.find(_pszNeedle) != std::string::npos);
	}

	// 정해진 순서대로 값을 돌려주는 시계. 파이썬 시험이 iter(...).__next__ 로 하는 일이다.
	class C_SCRIPTED_CLOCK
	{
	public:
		explicit C_SCRIPTED_CLOCK(std::vector<std::int64_t> _Values)
			: m_Values(std::move(_Values))
		{
		}

		std::int64_t operator()()
		{
			REQUIRE(m_nIndex < m_Values.size());
			return(m_Values[m_nIndex++]);
		}

		std::size_t Consumed() const { return(m_nIndex); }

	private:
		std::vector<std::int64_t> m_Values;
		std::size_t               m_nIndex{ 0 };
	};
}

// 대응 원본: tests/integration/test_backup.py::test_backup_restore_round_trip_preserves_cards_revisions_and_sequences
// 의 앞단(backup.py 의 create_database_backup :109~137) - 임시 파일에 쓰고 검사한 뒤에만 게시하며
// 임시 파일은 성공 경로에서도 지운다.
TEST_CASE("백업 생성은 검사를 통과한 파일만 게시하고 임시 파일을 남기지 않는다", "[core][storage][backup]")
{
	C_TEMP_TREE          Tree("create_ok");
	C_PROBE_FILE_SYSTEM  FileSystem;
	storage::C_BACKUP_SERVICE Service(FileSystem);

	const std::string sSource      = Tree.Child("pynote.sqlite3");
	const std::string sDestination = Tree.Child("out\\backup.sqlite3");
	seed_database(sSource);

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Service.Create(sSource, sDestination, &Inspection) == storage::E_BACKUP_RESULT::Ok);

	REQUIRE(Inspection.sPath == sDestination);
	REQUIRE(Inspection.nSchemaVersion == storage::migrations::LatestSchemaVersion());
	REQUIRE(FileSystem.Exists(sDestination));

	// 부모 디렉터리가 없으면 만든다(:115).
	REQUIRE(FileSystem.Exists(Tree.Child("out")));

	// 임시 이름은 정확히 하나 썼고 남아 있지 않다.
	REQUIRE(FileSystem.TemporaryPaths.size() == 1);
	REQUIRE(FileSystem.NoTemporaryLeftBehind());

	// 게시한 백업은 그 자체로 다시 검사를 통과한다.
	storage::S_BACKUP_INSPECTION Reinspected;
	REQUIRE(Service.Inspect(sDestination, &Reinspected) == storage::E_BACKUP_RESULT::Ok);
	REQUIRE(query_int(sDestination, "SELECT COUNT(*) FROM cards") == 3);
}

// 대응 원본: backup.py 의 create_database_backup (:113~114 - 기존 대상을 덮어쓰지 않는다).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("백업 생성은 기존 대상을 덮어쓰지 않는다", "[core][storage][backup]")
{
	C_TEMP_TREE          Tree("create_exists");
	C_PROBE_FILE_SYSTEM  FileSystem;
	storage::C_BACKUP_SERVICE Service(FileSystem);

	const std::string sSource      = Tree.Child("pynote.sqlite3");
	const std::string sDestination = Tree.Child("backup.sqlite3");
	seed_database(sSource);
	write_bytes(sDestination, "keep me");

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Service.Create(sSource, sDestination, &Inspection)
		== storage::E_BACKUP_RESULT::DestinationExists);
	REQUIRE(contains(Service.LastError(), "기존 백업을 덮어쓰지 않습니다"));

	// 거절은 임시 파일을 만들기도 전이다.
	REQUIRE(FileSystem.TemporaryPaths.empty());
	REQUIRE(read_bytes(sDestination) == "keep me");
}

// 대응 원본: backup.py 의 create_database_backup (:111~112 - 원본이 없으면 BackupError).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("원본 데이터베이스가 없으면 백업 생성은 사유와 함께 실패한다", "[core][storage][backup]")
{
	C_TEMP_TREE          Tree("create_no_source");
	C_PROBE_FILE_SYSTEM  FileSystem;
	storage::C_BACKUP_SERVICE Service(FileSystem);

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Service.Create(Tree.Child("absent.sqlite3"), Tree.Child("backup.sqlite3"), &Inspection)
		== storage::E_BACKUP_RESULT::SourceMissing);
	REQUIRE(contains(Service.LastError(), "백업할 데이터베이스 파일이 없습니다"));
	REQUIRE_FALSE(FileSystem.Exists(Tree.Child("backup.sqlite3")));
}

// 대응 원본: backup.py 의 create_database_backup finally (:136~137). 파이썬에서 finally 안의
// 예외는 진행 중이던 반환을 덮으므로, 임시 파일 삭제가 실패하면 게시를 마쳤어도 호출은 실패다.
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("임시 파일을 지우지 못하면 게시를 마쳤어도 실패로 보고한다", "[core][storage][backup]")
{
	C_TEMP_TREE          Tree("create_temp_undeletable");
	C_PROBE_FILE_SYSTEM  FileSystem;
	storage::C_BACKUP_SERVICE Service(FileSystem);

	const std::string sSource      = Tree.Child("pynote.sqlite3");
	const std::string sDestination = Tree.Child("backup.sqlite3");
	seed_database(sSource);

	FileSystem.fnFailRemove = [](const std::string& _sPath)
		{
			return(_sPath.size() >= 4 && _sPath.compare(_sPath.size() - 4, 4, ".tmp") == 0);
		};

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Service.Create(sSource, sDestination, &Inspection) == storage::E_BACKUP_RESULT::Failed);
	REQUIRE(contains(Service.LastError(), "주입한 삭제 실패"));

	// 게시 자체는 이미 끝난 뒤라 대상은 남는다. 원본도 같은 자리에서 파일을 남긴 채 예외를 올린다.
	REQUIRE(FileSystem.Exists(sDestination));
}

// 대응 원본: tests/integration/test_backup.py::test_migration_hook_creates_valid_pre_migration_backup
// (backup.py 의 MigrationBackupHook :202~229, database.py 의 훅 호출 계약).
TEST_CASE("마이그레이션 훅은 사전 백업을 만들고 러너 계약을 만족한다", "[core][storage][backup]")
{
	C_TEMP_TREE          Tree("hook_ok");
	C_PROBE_FILE_SYSTEM  FileSystem;
	storage::C_BACKUP_SERVICE Service(FileSystem);

	const std::string sDatabase        = Tree.Child("previous.sqlite3");
	const std::string sBackupDirectory = Tree.Child("backups");

	// 파이썬 시험과 같은 출발점 - schema_version 만 있는 v0 데이터베이스다.
	execute_sql(sDatabase,
		"CREATE TABLE schema_version ("
		"  id INTEGER PRIMARY KEY,"
		"  version INTEGER NOT NULL,"
		"  applied_at_us INTEGER NOT NULL)");
	execute_sql(sDatabase, "INSERT INTO schema_version (id, version, applied_at_us) VALUES (1, 0, 0)");

	storage::C_MIGRATION_BACKUP_HOOK Hook(Service, sBackupDirectory,
		[]() { return(CLOCK_20260102); });

	storage::C_DATABASE Database;
	REQUIRE(Database.Open(sDatabase));

	storage::C_MIGRATION_RUNNER Runner;
	Runner.SetExistingDatabase(true, sDatabase);
	// std::ref 로 감싸야 러너가 사본이 아니라 이 훅을 부른다 - LastBackupPath 가 여기 남는다.
	Runner.SetBackupHook(std::ref(Hook));
	REQUIRE(Runner.Run(Database) == storage::E_MIGRATE_RESULT::Ok);
	Database.Close();

	// 이름은 원본 f-string 그대로다: {stem}.pre-migration-v{old}-to-v{new}-{timestamp}.sqlite3
	const std::string sExpected = Tree.Child("backups\\previous.pre-migration-v0-to-v"
		+ std::to_string(storage::migrations::LatestSchemaVersion())
		+ "-20260102T000000000000Z.sqlite3");
	REQUIRE(Hook.LastBackupPath() == sExpected);
	REQUIRE(FileSystem.Exists(sExpected));

	// 백업은 마이그레이션 **전** 상태다(:107~112 - 훅은 트랜잭션 전에 부른다).
	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Service.Inspect(Hook.LastBackupPath(), &Inspection) == storage::E_BACKUP_RESULT::Ok);
	REQUIRE(Inspection.nSchemaVersion == 0);

	// 원본 데이터베이스는 최신까지 올라갔다.
	REQUIRE(query_int(sDatabase, "SELECT version FROM schema_version WHERE id = 1")
		== storage::migrations::LatestSchemaVersion());
}

// 대응 원본: backup.py 의 MigrationBackupHook (:222 - 디렉터리를 주지 않으면 DB 옆의 "backups").
// 판정은 None 인가이지 빈 경로인가가 아니므로 이식본도 optional 의 부재로만 이 갈래에 든다.
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("훅에 디렉터리를 주지 않으면 데이터베이스 옆 backups 에 만든다", "[core][storage][backup]")
{
	C_TEMP_TREE          Tree("hook_default_directory");
	C_PROBE_FILE_SYSTEM  FileSystem;
	storage::C_BACKUP_SERVICE Service(FileSystem);

	// 부모가 트리 루트가 아니어야 "DB 옆"이 실제로 판정된 것인지 보인다.
	const std::string sDatabase = Tree.Child("data\\previous.sqlite3");
	REQUIRE(FileSystem.CreateDirectories(Tree.Child("data")));

	execute_sql(sDatabase,
		"CREATE TABLE schema_version ("
		"  id INTEGER PRIMARY KEY,"
		"  version INTEGER NOT NULL,"
		"  applied_at_us INTEGER NOT NULL)");
	execute_sql(sDatabase, "INSERT INTO schema_version (id, version, applied_at_us) VALUES (1, 0, 0)");

	storage::C_MIGRATION_BACKUP_HOOK Hook(Service, std::nullopt, []() { return(CLOCK_20260102); });

	// 러너를 거치지 않고 훅 계약만 직접 본다. 인자는 (경로, 현재 버전, 최종 버전) 이다.
	REQUIRE(Hook(sDatabase, 0, 9));

	const std::string sExpected =
		Tree.Child("data\\backups\\previous.pre-migration-v0-to-v9-20260102T000000000000Z.sqlite3");
	REQUIRE(Hook.LastBackupPath() == sExpected);
	REQUIRE(FileSystem.Exists(sExpected));

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Service.Inspect(sExpected, &Inspection) == storage::E_BACKUP_RESULT::Ok);
	REQUIRE(Inspection.nSchemaVersion == 0);
}

// 대응 원본: backup.py 의 MigrationBackupHook (:228 - create_database_backup 실패는 그대로 전파된다)
// 와 database.py 의 훅 실패 계약(:107~112). 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는
// W0 T4 역보강 대기다.
TEST_CASE("훅이 백업에 실패하면 마이그레이션을 진행하지 않는다", "[core][storage][backup]")
{
	C_TEMP_TREE          Tree("hook_failed");
	C_PROBE_FILE_SYSTEM  FileSystem;
	storage::C_BACKUP_SERVICE Service(FileSystem);

	const std::string sDatabase        = Tree.Child("previous.sqlite3");
	const std::string sBackupDirectory = Tree.Child("backups");

	execute_sql(sDatabase,
		"CREATE TABLE schema_version ("
		"  id INTEGER PRIMARY KEY,"
		"  version INTEGER NOT NULL,"
		"  applied_at_us INTEGER NOT NULL)");
	execute_sql(sDatabase, "INSERT INTO schema_version (id, version, applied_at_us) VALUES (1, 0, 0)");

	// 백업 디렉터리 자리에 일반 파일을 두면 mkdir 이 실패한다 - 원본도 같은 자리에서 OSError 다.
	write_bytes(sBackupDirectory, "not a directory");

	storage::C_MIGRATION_BACKUP_HOOK Hook(Service, sBackupDirectory,
		[]() { return(CLOCK_20260102); });

	storage::C_DATABASE Database;
	REQUIRE(Database.Open(sDatabase));

	storage::C_MIGRATION_RUNNER Runner;
	Runner.SetExistingDatabase(true, sDatabase);
	Runner.SetBackupHook(std::ref(Hook));
	REQUIRE(Runner.Run(Database) == storage::E_MIGRATE_RESULT::BackupHookFailed);
	Database.Close();

	REQUIRE(Hook.LastBackupPath().empty());
	REQUIRE(query_int(sDatabase, "SELECT version FROM schema_version WHERE id = 1") == 0);
}

// 대응 원본: tests/integration/test_backup.py::test_automatic_backup_obeys_interval_and_quick_check_period
// 의 앞 절반(backup.py 의 AutomaticBackupManager :232~267 - 벽시계로 재는 주기).
TEST_CASE("자동 백업은 벽시계 주기가 지났을 때만 만든다", "[core][storage][backup]")
{
	C_TEMP_TREE          Tree("auto_interval");
	C_PROBE_FILE_SYSTEM  FileSystem;
	storage::C_BACKUP_SERVICE Service(FileSystem);

	const std::string sDatabase = Tree.Child("pynote.sqlite3");
	seed_database(sDatabase);

	const std::int64_t nHour = 3600LL * 1000000LL;
	C_SCRIPTED_CLOCK   Clock({ CLOCK_20260102, CLOCK_20260102 + nHour, CLOCK_20260102 + 25 * nHour });

	storage::C_AUTOMATIC_BACKUP_MANAGER Manager(
		Service, FileSystem, sDatabase, Tree.Child("backups"), 24.0,
		[&Clock]() { return(Clock()); });
	REQUIRE(Manager.IsValid());

	bool        bCreated = false;
	std::string sFirst;
	REQUIRE(Manager.RunIfDue(false, &bCreated, &sFirst) == storage::E_BACKUP_RESULT::Ok);
	REQUIRE(bCreated);

	std::string sSecond;
	REQUIRE(Manager.RunIfDue(false, &bCreated, &sSecond) == storage::E_BACKUP_RESULT::Ok);
	REQUIRE_FALSE(bCreated);
	REQUIRE(sSecond.empty());

	std::string sThird;
	REQUIRE(Manager.RunIfDue(false, &bCreated, &sThird) == storage::E_BACKUP_RESULT::Ok);
	REQUIRE(bCreated);

	REQUIRE(Clock.Consumed() == 3);
	REQUIRE(sFirst == Tree.Child("backups\\pynote.auto-20260102T000000000000Z.sqlite3"));
	REQUIRE(sThird == Tree.Child("backups\\pynote.auto-20260103T010000000000Z.sqlite3"));
	REQUIRE(FileSystem.Exists(sFirst));
	REQUIRE(FileSystem.Exists(sThird));
}

// 대응 원본: backup.py 의 AutomaticBackupManager._latest_backup_time (:269~275 - 기억한 시각이
// 없으면 디렉터리에 남은 가장 최근 백업의 수정 시각을 본다).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("자동 백업은 기억한 시각이 없으면 남아 있는 백업의 수정 시각을 본다", "[core][storage][backup]")
{
	C_TEMP_TREE          Tree("auto_latest");
	C_PROBE_FILE_SYSTEM  FileSystem;
	storage::C_BACKUP_SERVICE Service(FileSystem);

	const std::string sDatabase  = Tree.Child("pynote.sqlite3");
	const std::string sDirectory = Tree.Child("backups");
	seed_database(sDatabase);

	{
		C_SCRIPTED_CLOCK Clock({ CLOCK_20260102 });
		storage::C_AUTOMATIC_BACKUP_MANAGER Seeder(
			Service, FileSystem, sDatabase, sDirectory, 24.0, [&Clock]() { return(Clock()); });

		bool        bCreated = false;
		std::string sPath;
		REQUIRE(Seeder.RunIfDue(false, &bCreated, &sPath) == storage::E_BACKUP_RESULT::Ok);
		REQUIRE(bCreated);
	}

	// 파일의 수정 시각은 실제 시각이라 판정 기준은 그 값이다 - 시계를 그 값에 맞춰 세운다.
	std::int64_t nModified = 0;
	REQUIRE(FileSystem.ModifiedTimeUs(
		Tree.Child("backups\\pynote.auto-20260102T000000000000Z.sqlite3"), &nModified));

	const std::int64_t nHour = 3600LL * 1000000LL;

	// 새 관리자는 기억한 시각이 없지만 디스크에 후보가 있으므로 주기 안이면 만들지 않는다.
	{
		C_SCRIPTED_CLOCK Clock({ nModified + nHour });
		storage::C_AUTOMATIC_BACKUP_MANAGER Manager(
			Service, FileSystem, sDatabase, sDirectory, 24.0, [&Clock]() { return(Clock()); });

		bool        bCreated = false;
		std::string sPath;
		REQUIRE(Manager.RunIfDue(false, &bCreated, &sPath) == storage::E_BACKUP_RESULT::Ok);
		REQUIRE_FALSE(bCreated);
	}

	// 주기를 넘기면 만든다.
	{
		C_SCRIPTED_CLOCK Clock({ nModified + 25 * nHour });
		storage::C_AUTOMATIC_BACKUP_MANAGER Manager(
			Service, FileSystem, sDatabase, sDirectory, 24.0, [&Clock]() { return(Clock()); });

		bool        bCreated = false;
		std::string sPath;
		REQUIRE(Manager.RunIfDue(false, &bCreated, &sPath) == storage::E_BACKUP_RESULT::Ok);
		REQUIRE(bCreated);
		REQUIRE(FileSystem.Exists(sPath));
	}
}

// 대응 원본: backup.py 의 run_if_due 의 force 갈래(:259) 와 set_interval_hours (:249~253).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("자동 백업은 force 면 주기를 무시하고 0 이하 주기는 거절한다", "[core][storage][backup]")
{
	C_TEMP_TREE          Tree("auto_force");
	C_PROBE_FILE_SYSTEM  FileSystem;
	storage::C_BACKUP_SERVICE Service(FileSystem);

	const std::string sDatabase = Tree.Child("pynote.sqlite3");
	seed_database(sDatabase);

	const std::int64_t nMinute = 60LL * 1000000LL;
	C_SCRIPTED_CLOCK   Clock({ CLOCK_20260102, CLOCK_20260102 + nMinute, CLOCK_20260102 + 2 * nMinute });

	storage::C_AUTOMATIC_BACKUP_MANAGER Manager(
		Service, FileSystem, sDatabase, Tree.Child("backups"), 24.0,
		[&Clock]() { return(Clock()); });

	bool        bCreated = false;
	std::string sPath;
	REQUIRE(Manager.RunIfDue(false, &bCreated, &sPath) == storage::E_BACKUP_RESULT::Ok);
	REQUIRE(bCreated);

	// 1 분 뒤라도 force 면 만든다.
	REQUIRE(Manager.RunIfDue(true, &bCreated, &sPath) == storage::E_BACKUP_RESULT::Ok);
	REQUIRE(bCreated);
	REQUIRE(sPath == Tree.Child("backups\\pynote.auto-20260102T000100000000Z.sqlite3"));

	// 0 이하는 거절한다. 원본은 예외를 올리고 _interval 에 손대지 않으므로 관리자는 계속
	// 쓸 수 있어야 한다 - 거절이 관리자를 못 쓰게 만들면 그것이 동작 편차다.
	REQUIRE_FALSE(Manager.SetIntervalHours(0.0));
	REQUIRE(contains(Manager.LastError(), "자동 백업 주기는 0시간보다 커야 합니다."));
	REQUIRE(Manager.IsValid());

	REQUIRE(Manager.RunIfDue(false, &bCreated, &sPath) == storage::E_BACKUP_RESULT::Ok);
	REQUIRE_FALSE(bCreated);
	REQUIRE(Clock.Consumed() == 3);
}

// 대응 원본: backup.py 의 AutomaticBackupManager.__init__ (:247 - 생성 시점의 주기 검증).
// 파이썬은 생성자에서 ValueError 를 올려 객체가 아예 만들어지지 않지만 이식본의 생성자는
// 실패를 알릴 수 없으므로 그 거절을 IsValid 와 RunIfDue 결과로 옮겼다.
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("생성 시점에 거절된 주기를 가진 관리자는 백업하지 않는다", "[core][storage][backup]")
{
	C_TEMP_TREE          Tree("auto_invalid_interval");
	C_PROBE_FILE_SYSTEM  FileSystem;
	storage::C_BACKUP_SERVICE Service(FileSystem);

	const std::string sDatabase = Tree.Child("pynote.sqlite3");
	seed_database(sDatabase);

	storage::C_AUTOMATIC_BACKUP_MANAGER Manager(
		Service, FileSystem, sDatabase, Tree.Child("backups"), 0.0,
		[]() { return(CLOCK_20260102); });

	REQUIRE_FALSE(Manager.IsValid());

	bool        bCreated = false;
	std::string sPath;
	REQUIRE(Manager.RunIfDue(false, &bCreated, &sPath) == storage::E_BACKUP_RESULT::Failed);
	REQUIRE_FALSE(bCreated);
	REQUIRE_FALSE(FileSystem.Exists(Tree.Child("backups")));

	// 주기를 제대로 넣으면 그때부터 쓸 수 있다.
	REQUIRE(Manager.SetIntervalHours(24.0));
	REQUIRE(Manager.IsValid());
	REQUIRE(Manager.RunIfDue(false, &bCreated, &sPath) == storage::E_BACKUP_RESULT::Ok);
	REQUIRE(bCreated);
}

// 대응 원본: tests/integration/test_backup.py::test_automatic_backup_obeys_interval_and_quick_check_period
// 의 뒤 절반(backup.py 의 PeriodicQuickCheck :278~306 - 단조시계로 재는 요율 제한).
TEST_CASE("주기적 quick_check 는 단조시계 주기가 지났을 때만 검사한다", "[core][storage][backup]")
{
	C_TEMP_TREE Tree("quick_check_period");
	const std::string sDatabase = Tree.Child("pynote.sqlite3");
	seed_database(sDatabase);

	sqlite3* pConnection = nullptr;
	REQUIRE(::sqlite3_open_v2(sDatabase.c_str(), &pConnection, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);

	// 파이썬 시험과 같은 눈금이다: 0.0, 1.0, 3601.0 에 주기 1 시간.
	std::vector<double> Ticks{ 0.0, 1.0, 3601.0 };
	std::size_t         nIndex = 0;

	storage::C_PERIODIC_QUICK_CHECK Checker(pConnection, 1.0,
		[&Ticks, &nIndex]()
		{
			REQUIRE(nIndex < Ticks.size());
			return(Ticks[nIndex++]);
		});
	REQUIRE(Checker.IsValid());

	REQUIRE(Checker.RunIfDue(false) == storage::E_QUICK_CHECK_RESULT::Passed);
	REQUIRE(Checker.RunIfDue(false) == storage::E_QUICK_CHECK_RESULT::Skipped);
	REQUIRE(Checker.RunIfDue(false) == storage::E_QUICK_CHECK_RESULT::Passed);
	REQUIRE(nIndex == 3);

	::sqlite3_close(pConnection);
}

// 대응 원본: backup.py 의 PeriodicQuickCheck (:288~289 주기 검증, :304~305 실패 시 시각 미갱신).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("quick_check 실패는 마지막 검사 시각을 갱신하지 않는다", "[core][storage][backup]")
{
	C_TEMP_TREE       Tree("quick_check_failed");
	const std::string sDatabase = Tree.Child("damaged.sqlite3");

	make_quick_check_violation(sDatabase);

	sqlite3* pConnection = nullptr;
	REQUIRE(::sqlite3_open_v2(sDatabase.c_str(), &pConnection, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);

	double nNow = 0.0;
	storage::C_PERIODIC_QUICK_CHECK Checker(pConnection, 1.0, [&nNow]() { return(nNow); });

	REQUIRE(Checker.RunIfDue(false) == storage::E_QUICK_CHECK_RESULT::Failed);
	REQUIRE(contains(Checker.LastError(), "SQLite 무결성 검사에 실패했습니다:"));

	// 시각을 갱신했다면 같은 눈금의 두 번째 호출이 Skipped 가 됐을 것이다.
	REQUIRE(Checker.RunIfDue(false) == storage::E_QUICK_CHECK_RESULT::Failed);

	// 0 이하 주기는 거절하고 그 상태의 호출은 실패다.
	storage::C_PERIODIC_QUICK_CHECK Invalid(pConnection, 0.0, [&nNow]() { return(nNow); });
	REQUIRE_FALSE(Invalid.IsValid());
	REQUIRE(Invalid.RunIfDue(false) == storage::E_QUICK_CHECK_RESULT::Failed);
	REQUIRE(contains(Invalid.LastError(), "quick_check 주기는 0시간보다 커야 합니다."));

	::sqlite3_close(pConnection);
}
