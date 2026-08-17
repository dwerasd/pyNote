#include "backup_test_support.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#pragma comment(lib, "NoteExCore")

namespace
{
	using namespace backup_support;

	bool contains(const std::string& _sText, const char* _pszNeedle)
	{
		return(_sText.find(_pszNeedle) != std::string::npos);
	}

	// 카드/리비전/카운터의 내용 지문. 파이썬 시험의 _content_snapshot(:63~81) 자리다.
	std::string content_snapshot(const std::string& _sPath)
	{
		sqlite3* pConnection = nullptr;
		REQUIRE(::sqlite3_open_v2(_sPath.c_str(), &pConnection, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);

		// 정렬은 문장마다 걸어야 한다 - SQLite 는 복합 SELECT 의 각 갈래에 ORDER BY 를 받지 않는다.
		const char* const QUERIES[] = {
			"SELECT id || '|' || capture_seq || '|' || body || '|' || body_hash FROM cards ORDER BY capture_seq",
			"SELECT id || '|' || event_seq || '|' || body || '|' || body_hash FROM card_revisions ORDER BY event_seq",
			"SELECT name || '|' || next_value FROM counters ORDER BY name",
		};

		std::string sSnapshot;
		for (const char* pszSql : QUERIES)
		{
			sqlite3_stmt* pStmt = nullptr;
			REQUIRE(::sqlite3_prepare_v2(pConnection, pszSql, -1, &pStmt, nullptr) == SQLITE_OK);
			while (::sqlite3_step(pStmt) == SQLITE_ROW)
			{
				const unsigned char* pText = ::sqlite3_column_text(pStmt, 0);
				const int            nSize = ::sqlite3_column_bytes(pStmt, 0);
				if (pText != nullptr)
				{
					sSnapshot.append(reinterpret_cast<const char*>(pText), static_cast<std::size_t>(nSize));
				}
				sSnapshot.push_back('\n');
			}
			::sqlite3_finalize(pStmt);
		}
		::sqlite3_close(pConnection);
		return(sSnapshot);
	}

	// 백업 뒤에 데이터베이스를 바꿔 둔다. 복원이 되돌리는 대상이 실제로 생기게 하는 자리다.
	void add_card(const std::string& _sPath)
	{
		storage::C_DATABASE Database;
		REQUIRE(Database.Open(_sPath));
		storage::C_REPOSITORIES Repositories(Database);

		domain::S_NEW_CAPTURE_OPERATION Operation;
		Operation.sId          = "operation-after";
		Operation.sDocumentId  = "document-1";
		Operation.eSource      = domain::E_CAPTURE_OPERATION_SOURCE::Typing;
		Operation.eSplitPolicy = domain::E_SPLIT_POLICY::Keep;
		Operation.nCreatedAtUs = 9000;

		domain::S_NEW_CARD Card;
		Card.sId             = "card-after";
		Card.sRevisionId     = "revision-after";
		Card.sEventId        = "event-after";
		Card.nPositionKey    = 99 * 1024;
		Card.sBody           = u8s(u8"백업 뒤 변경");
		Card.eCardSource     = domain::E_CARD_SOURCE::Typing;
		Card.eEventSource    = domain::E_EVENT_SOURCE::Typing;
		Card.eRevisionSource = domain::E_REVISION_SOURCE::Edit;
		Card.nCreatedAtUs    = 9000;

		std::vector<domain::S_CARD> Created;
		REQUIRE(Repositories.CreateCards(Operation, { Card }, &Created) == storage::E_REPO_RESULT::Ok);
		Database.Close();
	}

	// 복원 대상 세트를 알아볼 수 있는 바이트로 채운다. 파이썬 시험이 쓰는 방식 그대로다
	// (tests/integration/test_backup.py:333~335, :356~358).
	void write_original_set(const std::string& _sDestination)
	{
		write_bytes(_sDestination, "original database");
		write_bytes(_sDestination + "-wal", "original wal");
		write_bytes(_sDestination + "-shm", "original shm");
	}
}

// 대응 원본: tests/integration/test_backup.py::test_backup_restore_round_trip_preserves_cards_revisions_and_sequences
TEST_CASE("백업 왕복은 카드와 리비전과 순번을 그대로 되돌린다", "[core][storage][backup]")
{
	C_TEMP_TREE               Tree("restore_round_trip");
	C_PROBE_FILE_SYSTEM       FileSystem;
	storage::C_BACKUP_SERVICE Service(FileSystem);

	const std::string sDatabase = Tree.Child("pynote.sqlite3");
	const std::string sBackup   = Tree.Child("backup.sqlite3");
	seed_database(sDatabase);

	const std::string sExpected = content_snapshot(sDatabase);

	storage::S_BACKUP_INSPECTION Created;
	REQUIRE(Service.Create(sDatabase, sBackup, &Created) == storage::E_BACKUP_RESULT::Ok);

	add_card(sDatabase);
	REQUIRE(content_snapshot(sDatabase) != sExpected);

	storage::S_BACKUP_INSPECTION Restored;
	REQUIRE(Service.Restore(sBackup, sDatabase, true, &Restored) == storage::E_BACKUP_RESULT::Ok);

	REQUIRE(Restored.sPath == sDatabase);
	REQUIRE(Restored.nSchemaVersion == storage::migrations::LatestSchemaVersion());
	REQUIRE(content_snapshot(sDatabase) == sExpected);
	REQUIRE(FileSystem.NoTemporaryLeftBehind());
}

// 대응 원본: backup.py 의 restore_database (:150~152 - overwrite 없이 기존 세트를 덮어쓰지 않는다).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("복원은 overwrite 없이 기존 데이터베이스를 덮어쓰지 않는다", "[core][storage][backup]")
{
	C_TEMP_TREE               Tree("restore_no_overwrite");
	C_PROBE_FILE_SYSTEM       FileSystem;
	storage::C_BACKUP_SERVICE Service(FileSystem);

	const std::string sSource      = Tree.Child("source.sqlite3");
	const std::string sBackup      = Tree.Child("backup.sqlite3");
	const std::string sDestination = Tree.Child("live.sqlite3");
	seed_database(sSource);

	storage::S_BACKUP_INSPECTION Created;
	REQUIRE(Service.Create(sSource, sBackup, &Created) == storage::E_BACKUP_RESULT::Ok);
	write_bytes(sDestination, "keep me");

	storage::S_BACKUP_INSPECTION Restored;
	REQUIRE(Service.Restore(sBackup, sDestination, false, &Restored)
		== storage::E_BACKUP_RESULT::DestinationExists);
	REQUIRE(contains(Service.LastError(), "기존 데이터베이스를 덮어쓰지 않습니다"));
	REQUIRE(read_bytes(sDestination) == "keep me");
}

// 대응 원본: backup.py 의 restore_database (:150~151 - 기존 세트가 하나도 없으면 overwrite 없이도
// 진행한다). 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("기존 세트가 없는 자리로는 overwrite 없이 복원한다", "[core][storage][backup]")
{
	C_TEMP_TREE               Tree("restore_fresh");
	C_PROBE_FILE_SYSTEM       FileSystem;
	storage::C_BACKUP_SERVICE Service(FileSystem);

	const std::string sSource      = Tree.Child("source.sqlite3");
	const std::string sBackup      = Tree.Child("backup.sqlite3");
	const std::string sDestination = Tree.Child("fresh\\live.sqlite3");
	seed_database(sSource);

	storage::S_BACKUP_INSPECTION Created;
	REQUIRE(Service.Create(sSource, sBackup, &Created) == storage::E_BACKUP_RESULT::Ok);
	const std::size_t nTemporaryBefore = FileSystem.TemporaryPaths.size();

	storage::S_BACKUP_INSPECTION Restored;
	REQUIRE(Service.Restore(sBackup, sDestination, false, &Restored) == storage::E_BACKUP_RESULT::Ok);

	REQUIRE(Restored.sPath == sDestination);
	REQUIRE(FileSystem.Exists(sDestination));
	REQUIRE(content_snapshot(sDestination) == content_snapshot(sSource));

	// 비켜 둘 대상이 없으므로 임시 이름은 새 본체용 하나뿐이고 그마저 남지 않는다.
	// (사이드카 부재는 여기서 단언하지 않는다 - WAL 모드 DB 를 읽기 전용으로 여는 것만으로
	//  -wal/-shm 이 생기고 닫아도 남는다. 실측 2026-08-16, 파이썬 sqlite3 도 같다.)
	REQUIRE(FileSystem.TemporaryPaths.size() == nTemporaryBefore + 1);
	REQUIRE(FileSystem.NoTemporaryLeftBehind());
}

// 대응 원본: tests/integration/test_backup.py::test_corrupt_backup_is_rejected_before_destination_changes
TEST_CASE("손상된 백업으로는 복원하지 않고 대상도 건드리지 않는다", "[core][storage][backup]")
{
	C_TEMP_TREE               Tree("restore_corrupt");
	C_PROBE_FILE_SYSTEM       FileSystem;
	storage::C_BACKUP_SERVICE Service(FileSystem);

	const std::string sBackup      = Tree.Child("broken.sqlite3");
	const std::string sDestination = Tree.Child("live.sqlite3");
	write_bytes(sBackup, "not sqlite");
	write_bytes(sDestination, "keep me");

	storage::S_BACKUP_INSPECTION Restored;
	REQUIRE(Service.Restore(sBackup, sDestination, true, &Restored) == storage::E_BACKUP_RESULT::Integrity);
	REQUIRE(read_bytes(sDestination) == "keep me");

	// 검사에서 막혔으므로 임시 파일을 만들지도 않았다.
	REQUIRE(FileSystem.TemporaryPaths.empty());
}

// 대응 원본: tests/integration/test_backup.py::test_restore_rejects_sidecar_directory_before_database_set_changes
// (backup.py 의 _validate_restore_targets :487~493).
TEST_CASE("복원 대상 세트에 일반 파일이 아닌 경로가 있으면 아무것도 옮기기 전에 거절한다", "[core][storage][backup]")
{
	C_TEMP_TREE               Tree("restore_sidecar_directory");
	C_PROBE_FILE_SYSTEM       FileSystem;
	storage::C_BACKUP_SERVICE Service(FileSystem);

	const std::string sSource      = Tree.Child("source.sqlite3");
	const std::string sBackup      = Tree.Child("backup.sqlite3");
	const std::string sDestination = Tree.Child("live.sqlite3");
	seed_database(sSource);

	storage::S_BACKUP_INSPECTION Created;
	REQUIRE(Service.Create(sSource, sBackup, &Created) == storage::E_BACKUP_RESULT::Ok);

	write_bytes(sDestination, "original database");
	write_bytes(sDestination + "-wal", "original wal");
	std::error_code ec;
	REQUIRE(std::filesystem::create_directory(sDestination + "-shm", ec));

	// 백업을 만드느라 이미 쓴 임시 이름이 있으므로 기준은 "이 시점 이후로 늘지 않았는가" 다.
	const std::size_t nTemporaryBefore = FileSystem.TemporaryPaths.size();

	storage::S_BACKUP_INSPECTION Restored;
	REQUIRE(Service.Restore(sBackup, sDestination, true, &Restored)
		== storage::E_BACKUP_RESULT::TargetInvalid);
	REQUIRE(contains(Service.LastError(), "복원 대상 DB 세트 경로가 올바르지 않습니다"));

	REQUIRE(read_bytes(sDestination) == "original database");
	REQUIRE(read_bytes(sDestination + "-wal") == "original wal");
	REQUIRE(std::filesystem::is_directory(sDestination + "-shm"));
	REQUIRE(FileSystem.TemporaryPaths.size() == nTemporaryBefore);
}

// 대응 원본: backup.py 의 _validate_restore_targets (:489~490 - 심볼릭 링크는 대상 자체가 일반
// 파일이어도 거절한다). 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("복원 대상 세트의 심볼릭 링크는 거절한다", "[core][storage][backup]")
{
	C_TEMP_TREE               Tree("restore_symlink");
	C_PROBE_FILE_SYSTEM       FileSystem;
	storage::C_BACKUP_SERVICE Service(FileSystem);

	const std::string sSource      = Tree.Child("source.sqlite3");
	const std::string sBackup      = Tree.Child("backup.sqlite3");
	const std::string sDestination = Tree.Child("live.sqlite3");
	const std::string sTarget      = Tree.Child("link-target.bin");
	seed_database(sSource);

	storage::S_BACKUP_INSPECTION Created;
	REQUIRE(Service.Create(sSource, sBackup, &Created) == storage::E_BACKUP_RESULT::Ok);

	write_bytes(sDestination, "original database");
	write_bytes(sTarget, "link target");

	// 심볼릭 링크 생성은 권한이 필요하다. 만들 수 없으면 이 계약은 이 실행에서 미검증이다.
	std::error_code ec;
	std::filesystem::create_symlink(sTarget, sDestination + "-wal", ec);
	if (ec)
	{
		SKIP("심볼릭 링크를 만들 권한이 없어 이 실행에서는 검증하지 못했다: " + ec.message());
	}

	REQUIRE(FileSystem.IsSymlink(sDestination + "-wal"));
	// 링크가 가리키는 대상은 일반 파일이지만 그래도 거절한다 - 판정은 링크 자신에 걸린다.
	REQUIRE(FileSystem.IsRegularFile(sDestination + "-wal"));

	const std::size_t nTemporaryBefore = FileSystem.TemporaryPaths.size();

	storage::S_BACKUP_INSPECTION Restored;
	REQUIRE(Service.Restore(sBackup, sDestination, true, &Restored)
		== storage::E_BACKUP_RESULT::TargetInvalid);
	REQUIRE(read_bytes(sDestination) == "original database");
	REQUIRE(FileSystem.TemporaryPaths.size() == nTemporaryBefore);
}

// 대응 원본: tests/integration/test_backup.py::test_restore_rolls_back_database_set_when_publish_fails
// (backup.py 의 :177~192 와 _restore_preserved_database_set :496~516).
TEST_CASE("게시가 실패하면 원본 DB 세트를 통째로 되돌린다", "[core][storage][backup]")
{
	C_TEMP_TREE               Tree("restore_rollback");
	C_PROBE_FILE_SYSTEM       FileSystem;
	storage::C_BACKUP_SERVICE Service(FileSystem);

	const std::string sSource      = Tree.Child("source.sqlite3");
	const std::string sBackup      = Tree.Child("backup.sqlite3");
	const std::string sDestination = Tree.Child("live.sqlite3");
	seed_database(sSource);

	storage::S_BACKUP_INSPECTION Created;
	REQUIRE(Service.Create(sSource, sBackup, &Created) == storage::E_BACKUP_RESULT::Ok);
	write_original_set(sDestination);

	// 파이썬 시험이 monkeypatch 로 하는 일과 같다 - 첫 게시만 실패시킨다. 뒤이은 롤백의
	// 교체까지 실패시키면 다른 계약(롤백 실패)을 시험하게 된다.
	bool bAlreadyFailed = false;
	FileSystem.fnFailReplace = [&bAlreadyFailed, &sDestination](const std::string&, const std::string& _sTo)
		{
			if (!bAlreadyFailed && _sTo == sDestination) { bAlreadyFailed = true; return(true); }
			return(false);
		};

	storage::S_BACKUP_INSPECTION Restored;
	REQUIRE(Service.Restore(sBackup, sDestination, true, &Restored) == storage::E_BACKUP_RESULT::Failed);
	REQUIRE(bAlreadyFailed);
	REQUIRE(Service.RollbackFailedPaths().empty());
	REQUIRE(contains(Service.LastError(), "주입한 교체 실패"));

	// 세 파일 전부 원래 자리에 원래 바이트로 돌아와 있다.
	REQUIRE(read_bytes(sDestination) == "original database");
	REQUIRE(read_bytes(sDestination + "-wal") == "original wal");
	REQUIRE(read_bytes(sDestination + "-shm") == "original shm");
	REQUIRE(FileSystem.NoTemporaryLeftBehind());
}

// 대응 원본: tests/integration/test_backup.py::test_restore_keeps_original_database_when_first_move_aside_fails
// (backup.py 의 _restore_preserved_database_set :496~516 와 finally :199). 구 롤백 1단계 분기는
// 정확히 이 상태에서 원본 DB 를 임시 이름으로 옮겨 말미 정리가 지우게 했다 - W1 계약 대장 §6-1
// 이 지목한 데이터 손실 경로였고, 제품 판단으로 파이썬 원본과 함께 제거했다.
TEST_CASE("첫 비켜두기가 실패해도 원본 DB 세트는 제자리에 남는다", "[core][storage][backup]")
{
	C_TEMP_TREE               Tree("restore_stage1_keeps_original");
	C_PROBE_FILE_SYSTEM       FileSystem;
	storage::C_BACKUP_SERVICE Service(FileSystem);

	const std::string sSource      = Tree.Child("source.sqlite3");
	const std::string sBackup      = Tree.Child("backup.sqlite3");
	const std::string sDestination = Tree.Child("live.sqlite3");
	seed_database(sSource);

	storage::S_BACKUP_INSPECTION Created;
	REQUIRE(Service.Create(sSource, sBackup, &Created) == storage::E_BACKUP_RESULT::Ok);
	write_original_set(sDestination);

	// 구 분기의 도달 조건이던 상태 그대로다: 대상이 존재하고 첫 비켜두기 교체가 실패해
	// 옮긴 것이 없다. 첫 교체 하나만 실패시키면 정확히 그 상태가 된다.
	int nReplaceCalls = 0;
	FileSystem.fnFailReplace = [&nReplaceCalls](const std::string&, const std::string&)
		{
			++nReplaceCalls;
			return(nReplaceCalls == 1);
		};

	storage::S_BACKUP_INSPECTION Restored;
	REQUIRE(Service.Restore(sBackup, sDestination, true, &Restored) == storage::E_BACKUP_RESULT::Failed);

	// 옮긴 것이 없으므로 롤백은 빈 성공이고 원래 오류가 그대로 나간다.
	REQUIRE(Service.RollbackFailedPaths().empty());
	REQUIRE(contains(Service.LastError(), "주입한 교체 실패"));

	// 원본 세트 세 파일 전부 원래 자리에 원래 바이트로 남고, 비켜두기 예약 파일과
	// 새 본체 임시 파일도 남지 않는다.
	REQUIRE(read_bytes(sDestination) == "original database");
	REQUIRE(read_bytes(sDestination + "-wal") == "original wal");
	REQUIRE(read_bytes(sDestination + "-shm") == "original shm");
	REQUIRE(FileSystem.NoTemporaryLeftBehind());
}

// 대응 원본: tests/integration/test_backup.py::test_temporary_database_path_reserves_name_until_replaced
// (backup.py 의 _temporary_database_path :531~540 - 배타 생성한 0바이트 예약 파일을 남긴다).
TEST_CASE("임시 이름 예약은 파일로 남아 원자 교체의 대상이 된다", "[core][storage][backup]")
{
	C_TEMP_TREE                   Tree("temporary_reservation");
	platform::C_WIN32_FILE_SYSTEM FileSystem;

	std::string sFirst;
	std::string sSecond;
	REQUIRE(FileSystem.CreateUniqueTemporaryPath(Tree.Utf8(), ".live.sqlite3.", ".tmp", &sFirst));
	REQUIRE(FileSystem.CreateUniqueTemporaryPath(Tree.Utf8(), ".live.sqlite3.", ".tmp", &sSecond));

	// 예약 파일이 남아 있는 동안 배타 생성은 같은 이름을 다시 내줄 수 없다.
	REQUIRE(sFirst != sSecond);
	REQUIRE(FileSystem.Exists(sFirst));
	REQUIRE(FileSystem.Exists(sSecond));

	// 닫힌 0바이트 예약 파일은 원자 교체의 대상이 된다 - 공유 위반은 열린 핸들의 성질이다.
	write_bytes(Tree.Child("payload.bin"), "payload");
	REQUIRE(FileSystem.Replace(Tree.Child("payload.bin"), sFirst));
	REQUIRE(read_bytes(sFirst) == "payload");
}

// 대응 원본: backup.py 의 restore_database (:184~191 - 롤백까지 실패하면 원래 실패와 다른 오류다)
// 와 _restore_preserved_database_set 의 실패 경로 보고(:503~516).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("롤백까지 실패하면 되돌리지 못한 경로를 달고 다른 오류로 보고한다", "[core][storage][backup]")
{
	C_TEMP_TREE               Tree("restore_rollback_failed");
	C_PROBE_FILE_SYSTEM       FileSystem;
	storage::C_BACKUP_SERVICE Service(FileSystem);

	const std::string sSource      = Tree.Child("source.sqlite3");
	const std::string sBackup      = Tree.Child("backup.sqlite3");
	const std::string sDestination = Tree.Child("live.sqlite3");
	seed_database(sSource);

	storage::S_BACKUP_INSPECTION Created;
	REQUIRE(Service.Create(sSource, sBackup, &Created) == storage::E_BACKUP_RESULT::Ok);
	write_original_set(sDestination);

	// -shm 을 비켜 두는 교체가 실패해 롤백이 시작되고, 그 롤백 중 본체를 되돌리는 교체도 실패한다.
	// 게시는 일어나지 않으므로 대상 이름으로 가는 교체는 롤백의 그것뿐이다.
	const std::string sShm = sDestination + "-shm";
	FileSystem.fnFailReplace = [&sShm, &sDestination](const std::string& _sFrom, const std::string& _sTo)
		{
			return(_sFrom == sShm || _sTo == sDestination);
		};

	storage::S_BACKUP_INSPECTION Restored;
	REQUIRE(Service.Restore(sBackup, sDestination, true, &Restored)
		== storage::E_BACKUP_RESULT::RollbackFailed);
	REQUIRE(contains(Service.LastError(), "데이터베이스 복원과 원본 세트 롤백에 실패했습니다."));

	// 되돌리지 못한 경로는 본체 하나다 - 역순 롤백이라 -wal 은 먼저 제자리로 돌아갔다.
	REQUIRE(Service.RollbackFailedPaths().size() == 1);
	REQUIRE(Service.RollbackFailedPaths()[0] == sDestination);

	REQUIRE(read_bytes(sDestination + "-wal") == "original wal");
	REQUIRE(read_bytes(sShm) == "original shm");

	// 본체는 비켜 둔 이름에 남아 있다. 그것이 이 오류가 다른 오류인 이유다.
	REQUIRE_FALSE(FileSystem.Exists(sDestination));
}
