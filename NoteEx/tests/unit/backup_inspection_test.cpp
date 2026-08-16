#include "backup_test_support.h"

#include <string>

#pragma comment(lib, "NoteExCore")

namespace
{
	using namespace backup_support;

	// 백업 검사만 보는 시험이라 파일시스템은 실제 Win32 구현 그대로다.
	class C_INSPECTION_FIXTURE
	{
	public:
		explicit C_INSPECTION_FIXTURE(const std::string& _sName)
			: m_Tree(_sName)
			, m_Service(m_FileSystem)
		{
		}

		C_TEMP_TREE&              Tree() { return(m_Tree); }
		storage::C_BACKUP_SERVICE& Service() { return(m_Service); }

	private:
		C_TEMP_TREE                   m_Tree;
		platform::C_WIN32_FILE_SYSTEM m_FileSystem;
		storage::C_BACKUP_SERVICE     m_Service;
	};

	bool contains(const std::string& _sText, const char* _pszNeedle)
	{
		return(_sText.find(_pszNeedle) != std::string::npos);
	}

}

// 대응 원본: src/pynote/infrastructure/backup.py 의 inspect_backup (:74~75 - 파일이 없으면
// BackupIntegrityError). 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("없는 백업 파일은 사유와 함께 거절한다", "[core][storage][backup]")
{
	C_INSPECTION_FIXTURE Fixture("inspect_missing");
	const std::string    sPath = Fixture.Tree().Child("absent.sqlite3");

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Fixture.Service().Inspect(sPath, &Inspection) == storage::E_BACKUP_RESULT::Integrity);
	REQUIRE(contains(Fixture.Service().LastError(), "백업 파일이 없습니다"));
}

// 대응 원본: tests/integration/test_backup.py::test_corrupt_backup_is_rejected_before_destination_changes
// (backup.py 의 run_quick_check :60~64 - PRAGMA 자체가 실패하는 갈래).
TEST_CASE("SQLite 파일이 아닌 백업은 quick_check 실행 실패로 거절한다", "[core][storage][backup]")
{
	C_INSPECTION_FIXTURE Fixture("inspect_not_sqlite");
	const std::string    sPath = Fixture.Tree().Child("broken.sqlite3");
	write_bytes(sPath, "not sqlite");

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Fixture.Service().Inspect(sPath, &Inspection) == storage::E_BACKUP_RESULT::Integrity);
	REQUIRE(contains(Fixture.Service().LastError(), "SQLite quick_check를 실행할 수 없습니다."));
}

// 대응 원본: backup.py 의 run_quick_check (:65~69 - PRAGMA 는 돌았지만 결과가 ok 가 아닌 갈래).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("quick_check 가 손상을 보고하면 그 문구를 달고 거절한다", "[core][storage][backup]")
{
	C_INSPECTION_FIXTURE Fixture("inspect_quick_check");
	const std::string    sPath = Fixture.Tree().Child("damaged.sqlite3");

	make_quick_check_violation(sPath);

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Fixture.Service().Inspect(sPath, &Inspection) == storage::E_BACKUP_RESULT::Integrity);
	REQUIRE(contains(Fixture.Service().LastError(), "SQLite 무결성 검사에 실패했습니다:"));
}

// 대응 원본: backup.py 의 _read_schema_version (:316~318).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("schema_version 테이블이 없는 백업은 거절한다", "[core][storage][backup]")
{
	C_INSPECTION_FIXTURE Fixture("inspect_no_table");
	const std::string    sPath = Fixture.Tree().Child("no-table.sqlite3");
	execute_sql(sPath, "CREATE TABLE other (v INTEGER)");

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Fixture.Service().Inspect(sPath, &Inspection) == storage::E_BACKUP_RESULT::Integrity);
	REQUIRE(contains(Fixture.Service().LastError(), "pyNote schema_version이 없는 백업입니다."));
}

// 대응 원본: backup.py 의 _read_schema_version (:319~323).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("schema_version 행이 없는 백업은 거절한다", "[core][storage][backup]")
{
	C_INSPECTION_FIXTURE Fixture("inspect_no_row");
	const std::string    sPath = Fixture.Tree().Child("no-row.sqlite3");
	execute_sql(sPath, "CREATE TABLE schema_version (id INTEGER PRIMARY KEY, version INTEGER)");

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Fixture.Service().Inspect(sPath, &Inspection) == storage::E_BACKUP_RESULT::Integrity);
	REQUIRE(contains(Fixture.Service().LastError(), "백업의 schema version 행이 없습니다."));
}

// 대응 원본: backup.py 의 _read_schema_version (:324~326 - type(value) is not int).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("schema version 이 정수가 아닌 백업은 거절한다", "[core][storage][backup]")
{
	C_INSPECTION_FIXTURE Fixture("inspect_version_text");
	const std::string    sPath = Fixture.Tree().Child("text-version.sqlite3");
	execute_sql(sPath, "CREATE TABLE schema_version (id INTEGER PRIMARY KEY, version TEXT)");
	execute_sql(sPath, "INSERT INTO schema_version (id, version) VALUES (1, 'nine')");

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Fixture.Service().Inspect(sPath, &Inspection) == storage::E_BACKUP_RESULT::Integrity);
	REQUIRE(contains(Fixture.Service().LastError(), "백업의 schema version이 정수가 아닙니다."));
}

// 대응 원본: tests/integration/test_backup.py::test_newer_schema_backup_is_rejected_before_destination_changes
// (backup.py :97~105 - 지원 범위 밖 버전은 나머지 검사를 건너뛰고 UnsupportedBackupError 다).
TEST_CASE("앱보다 새로운 schema version 은 지원하지 않는 백업으로 거절한다", "[core][storage][backup]")
{
	C_INSPECTION_FIXTURE Fixture("inspect_future");
	const std::string    sPath = Fixture.Tree().Child("future.sqlite3");
	const int            nFuture = storage::migrations::LatestSchemaVersion() + 1;

	execute_sql(sPath, "CREATE TABLE schema_version (id INTEGER PRIMARY KEY, version INTEGER)");
	execute_sql(sPath,
		("INSERT INTO schema_version (id, version) VALUES (1, " + std::to_string(nFuture) + ")").c_str());

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Fixture.Service().Inspect(sPath, &Inspection) == storage::E_BACKUP_RESULT::Unsupported);
	REQUIRE(contains(Fixture.Service().LastError(), "지원하지 않는 백업 schema version입니다"));
}

// 대응 원본: tests/integration/test_backup.py::test_incomplete_current_schema_is_rejected_before_destination_changes
// (backup.py 의 _validate_schema_tables :330~353).
TEST_CASE("필수 테이블이 빠진 백업은 빠진 이름을 달고 거절한다", "[core][storage][backup]")
{
	C_INSPECTION_FIXTURE Fixture("inspect_incomplete");
	const std::string    sPath = Fixture.Tree().Child("not-pynote.sqlite3");
	const int            nLatest = storage::migrations::LatestSchemaVersion();

	execute_sql(sPath, "CREATE TABLE schema_version (id INTEGER PRIMARY KEY, version INTEGER)");
	execute_sql(sPath,
		("INSERT INTO schema_version (id, version) VALUES (1, " + std::to_string(nLatest) + ")").c_str());

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Fixture.Service().Inspect(sPath, &Inspection) == storage::E_BACKUP_RESULT::Integrity);
	REQUIRE(contains(Fixture.Service().LastError(), "백업에 필수 테이블이 없습니다:"));
	// 이름은 오름차순이다 - "card_" 의 밑줄이 "cards" 의 s 보다 앞이라 순서가 이렇게 된다.
	REQUIRE(contains(Fixture.Service().LastError(),
		"없습니다: capture_operations, card_lineage, card_revisions, cards, counters"));
}

// 대응 원본: tests/integration/test_backup.py::test_v4_backup_requires_workspace_windows_instead_of_workspace_state
TEST_CASE("v4 이상 백업은 workspace_state 가 아니라 workspace_windows 를 요구한다", "[core][storage][backup]")
{
	C_INSPECTION_FIXTURE Fixture("inspect_v4_tables");
	const std::string    sPath = Fixture.Tree().Child("missing-workspace-windows.sqlite3");
	seed_database(sPath);

	execute_sql(sPath, "DROP TABLE workspace_windows");
	execute_sql(sPath,
		"CREATE TABLE workspace_state ("
		"  id INTEGER PRIMARY KEY,"
		"  open_document_ids_json TEXT NOT NULL,"
		"  active_document_id TEXT,"
		"  updated_at_us INTEGER NOT NULL)");

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Fixture.Service().Inspect(sPath, &Inspection) == storage::E_BACKUP_RESULT::Integrity);
	REQUIRE(contains(Fixture.Service().LastError(), "workspace_windows"));
}

// 대응 원본: tests/integration/test_backup.py::test_v3_backup_still_requires_and_accepts_workspace_state
TEST_CASE("v3 백업은 workspace_state 를 요구하고 받아들인다", "[core][storage][backup]")
{
	C_INSPECTION_FIXTURE Fixture("inspect_v3_tables");
	const std::string    sPath = Fixture.Tree().Child("v3-backup.sqlite3");
	seed_database(sPath);

	execute_sql(sPath, "DROP TABLE workspace_windows");
	execute_sql(sPath,
		"CREATE TABLE workspace_state ("
		"  id INTEGER PRIMARY KEY,"
		"  open_document_ids_json TEXT NOT NULL,"
		"  active_document_id TEXT,"
		"  updated_at_us INTEGER NOT NULL)");
	execute_sql(sPath, "UPDATE schema_version SET version = 3, applied_at_us = 3 WHERE id = 1");

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Fixture.Service().Inspect(sPath, &Inspection) == storage::E_BACKUP_RESULT::Ok);
	REQUIRE(Inspection.nSchemaVersion == 3);
}

// 대응 원본: tests/integration/test_backup.py::test_dangling_foreign_key_backup_is_rejected_before_destination_changes
// (backup.py 의 _validate_foreign_keys :356~364).
TEST_CASE("FK 무결성이 깨진 백업은 표본을 달고 거절한다", "[core][storage][backup]")
{
	C_INSPECTION_FIXTURE Fixture("inspect_dangling_fk");
	const std::string    sPath = Fixture.Tree().Child("dangling.sqlite3");
	seed_database(sPath);

	// 원본 시험처럼 외래키 강제를 끈 연결로 부모 행만 지운다 - quick_check 는 통과하고
	// foreign_key_check 만 걸리는 상태다.
	execute_sql(sPath, "DELETE FROM capture_operations WHERE id = 'operation-1'");

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Fixture.Service().Inspect(sPath, &Inspection) == storage::E_BACKUP_RESULT::Integrity);
	REQUIRE(contains(Fixture.Service().LastError(), "백업의 FK 무결성 검사에 실패했습니다:"));
	REQUIRE(contains(Fixture.Service().LastError(), "cards rowid="));
}

// 대응 원본: backup.py 의 _validate_card_revision_integrity (:391~394 - 현재 리비전 소유권).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("현재 리비전이 비어 있는 카드는 소유권 오류로 거절한다", "[core][storage][backup]")
{
	C_INSPECTION_FIXTURE Fixture("inspect_owner");
	const std::string    sPath = Fixture.Tree().Child("owner.sqlite3");
	seed_database(sPath);

	execute_sql(sPath, "UPDATE cards SET current_revision_id = NULL WHERE id = 'card-0'");

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Fixture.Service().Inspect(sPath, &Inspection) == storage::E_BACKUP_RESULT::Integrity);
	REQUIRE(contains(Fixture.Service().LastError(), "카드 card-0의 현재 리비전 소유권이 올바르지 않습니다."));
}

// 대응 원본: backup.py 의 _validate_card_revision_integrity (:402~405 - 본문/해시 일치).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("카드 본문이 현재 리비전과 다르면 거절한다", "[core][storage][backup]")
{
	C_INSPECTION_FIXTURE Fixture("inspect_body_mismatch");
	const std::string    sPath = Fixture.Tree().Child("body.sqlite3");
	seed_database(sPath);

	// 이 상태는 v0003 의 불변조건 트리거가 막는다. 검사 계층이 있는 이유가 이 스키마를 거치지
	// 않은 파일(외부에서 받은 백업, 옛 버전이 만든 파일)까지 받기 때문이므로 트리거를 떼고 만든다.
	execute_sql(sPath, "DROP TRIGGER cards_current_revision_update");
	execute_sql(sPath, "UPDATE cards SET body = 'tampered' WHERE id = 'card-1'");

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Fixture.Service().Inspect(sPath, &Inspection) == storage::E_BACKUP_RESULT::Integrity);
	REQUIRE(contains(Fixture.Service().LastError(), "카드 card-1의 본문과 현재 리비전이 일치하지 않습니다."));
}

// 대응 원본: backup.py 의 _validate_text_hash (:468~471 - SHA-256 재계산).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("본문과 저장된 해시가 어긋나면 재계산으로 잡아 거절한다", "[core][storage][backup]")
{
	C_INSPECTION_FIXTURE Fixture("inspect_hash");
	const std::string    sPath = Fixture.Tree().Child("hash.sqlite3");
	seed_database(sPath);

	// 카드와 리비전을 같은 값으로 바꿔 두 행의 일치 검사는 통과시키고 해시 재계산만 실패시킨다.
	// 여기도 v0003 트리거가 막는 상태라 트리거를 떼고 만든다.
	execute_sql(sPath, "DROP TRIGGER cards_current_revision_update");
	execute_sql(sPath, "DROP TRIGGER card_revisions_current_update");
	execute_sql(sPath, "UPDATE cards SET body_hash = 'deadbeef' WHERE id = 'card-2'");
	execute_sql(sPath, "UPDATE card_revisions SET body_hash = 'deadbeef' WHERE id = 'revision-2'");

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Fixture.Service().Inspect(sPath, &Inspection) == storage::E_BACKUP_RESULT::Integrity);
	REQUIRE(contains(Fixture.Service().LastError(), "카드 card-2의 SHA-256 해시가 일치하지 않습니다."));
}

// 대응 원본: backup.py 의 _validate_capture_counter (:417~430).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("capture 카운터가 이미 발급한 순번보다 크지 않으면 거절한다", "[core][storage][backup]")
{
	C_INSPECTION_FIXTURE Fixture("inspect_counter");
	const std::string    sPath = Fixture.Tree().Child("counter.sqlite3");
	seed_database(sPath);

	// 감소 자체를 v0003 트리거가 막으므로 떼고 만든다.
	execute_sql(sPath, "DROP TRIGGER capture_counter_no_decrease");
	execute_sql(sPath, "UPDATE counters SET next_value = 1 WHERE name = 'capture'");

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Fixture.Service().Inspect(sPath, &Inspection) == storage::E_BACKUP_RESULT::Integrity);
	REQUIRE(contains(Fixture.Service().LastError(),
		"capture 카운터가 이미 발급된 capture_seq보다 크지 않습니다."));
}

// 대응 원본: backup.py 의 _validate_capture_operations (:443~446 - 원문/해시 쌍).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("입력 작업의 원문과 해시가 짝을 이루지 않으면 거절한다", "[core][storage][backup]")
{
	C_INSPECTION_FIXTURE Fixture("inspect_operation_pair");
	const std::string    sPath = Fixture.Tree().Child("pair.sqlite3");
	seed_database(sPath);

	execute_sql(sPath, "UPDATE capture_operations SET original_hash = NULL WHERE id = 'operation-1'");

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Fixture.Service().Inspect(sPath, &Inspection) == storage::E_BACKUP_RESULT::Integrity);
	REQUIRE(contains(Fixture.Service().LastError(),
		"입력 작업 operation-1의 원문과 해시 쌍이 일치하지 않습니다."));
}

// 대응 원본: backup.py 의 _validate_capture_operations (:447~452 - redact 마커).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("원문이 남아 있는데 redact 마커가 찍혀 있으면 거절한다", "[core][storage][backup]")
{
	C_INSPECTION_FIXTURE Fixture("inspect_redact");
	const std::string    sPath = Fixture.Tree().Child("redact.sqlite3");
	seed_database(sPath);

	execute_sql(sPath,
		"UPDATE capture_operations SET original_redacted_at_us = 5 WHERE id = 'operation-1'");

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Fixture.Service().Inspect(sPath, &Inspection) == storage::E_BACKUP_RESULT::Integrity);
	REQUIRE(contains(Fixture.Service().LastError(),
		"입력 작업 operation-1의 redact 마커가 올바르지 않습니다."));
}

// 대응 원본: backup.py 의 _validate_capture_operations (:447~452 - 소거를 마친 정상 상태).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("원문을 지우고 마커만 남긴 입력 작업은 받아들인다", "[core][storage][backup]")
{
	C_INSPECTION_FIXTURE Fixture("inspect_redacted_ok");
	const std::string    sPath = Fixture.Tree().Child("redacted.sqlite3");
	seed_database(sPath);

	execute_sql(sPath,
		"UPDATE capture_operations"
		" SET original_text = NULL, original_hash = NULL, original_redacted_at_us = 5"
		" WHERE id = 'operation-1'");

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Fixture.Service().Inspect(sPath, &Inspection) == storage::E_BACKUP_RESULT::Ok);
	REQUIRE(Inspection.nSchemaVersion == storage::migrations::LatestSchemaVersion());
}

// 대응 원본: backup.py 의 inspect_backup (:84~88 - 버전 0 은 테이블 검사와 논리 검사를 건너뛴다).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("버전 0 백업은 테이블 검사 없이 받아들인다", "[core][storage][backup]")
{
	C_INSPECTION_FIXTURE Fixture("inspect_v0");
	const std::string    sPath = Fixture.Tree().Child("v0.sqlite3");
	execute_sql(sPath, "CREATE TABLE schema_version (id INTEGER PRIMARY KEY, version INTEGER)");
	execute_sql(sPath, "INSERT INTO schema_version (id, version) VALUES (1, 0)");

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Fixture.Service().Inspect(sPath, &Inspection) == storage::E_BACKUP_RESULT::Ok);
	REQUIRE(Inspection.nSchemaVersion == 0);
	REQUIRE(Inspection.sPath == sPath);
}
