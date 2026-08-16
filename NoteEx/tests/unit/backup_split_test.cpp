#include "backup_test_support.h"

#include <string>
#include <vector>

#pragma comment(lib, "NoteExCore")

// 이 파일은 W1 지시서가 기록한 이 파동의 유일한 의도적 개선을 닫는다. 파이썬 원본은
// Connection.backup 한 번 호출이고 이식본은 sqlite3_backup_step 을 나눠 부른다. 나눈 루프는
// 동시 쓰기·취소·실패에서만 원본과 다른 것을 만들 수 있으므로 세 상황 각각에서 "어떤
// 스냅숏이 남는가"와 "무엇이 게시되는가"를 단언한다. 동시성 없이 도는 분할 루프는 문장이
// 늘어난 단일 호출과 같아서 아무것도 증명하지 않는다.

namespace
{
	using namespace backup_support;

	bool contains(const std::string& _sText, const char* _pszNeedle)
	{
		return(_sText.find(_pszNeedle) != std::string::npos);
	}

	// 살아 있는 다른 연결이 카드 한 장을 커밋한다. 커밋을 마치고 돌아오므로 다음 step 시점에
	// 잠금은 남아 있지 않고, 원본이 바뀐 사실만 남는다.
	void commit_one_card(storage::C_REPOSITORIES& _repositories, const std::string& _sSuffix)
	{
		domain::S_NEW_CAPTURE_OPERATION Operation;
		Operation.sId          = "operation-" + _sSuffix;
		Operation.sDocumentId  = "document-1";
		Operation.eSource      = domain::E_CAPTURE_OPERATION_SOURCE::Typing;
		Operation.eSplitPolicy = domain::E_SPLIT_POLICY::Keep;
		Operation.nCreatedAtUs = 8000;

		domain::S_NEW_CARD Card;
		Card.sId             = "card-" + _sSuffix;
		Card.sRevisionId     = "revision-" + _sSuffix;
		Card.sEventId        = "event-" + _sSuffix;
		Card.nPositionKey    = 50 * 1024;
		Card.sBody           = u8s(u8"백업 도중 커밋한 카드");
		Card.eCardSource     = domain::E_CARD_SOURCE::Typing;
		Card.eEventSource    = domain::E_EVENT_SOURCE::Typing;
		Card.eRevisionSource = domain::E_REVISION_SOURCE::Edit;
		Card.nCreatedAtUs    = 8000;

		std::vector<domain::S_CARD> Created;
		REQUIRE(_repositories.CreateCards(Operation, { Card }, &Created) == storage::E_REPO_RESULT::Ok);
	}
}

// 대응 원본: 없음. 파이썬 원본에는 응용 계층의 증분 루프가 없어 이 계약 자체가 이식본에서
// 새로 생긴 것이다(SPEC §2 가 허용한 유일한 개선). pytest node ID 는 존재하지 않는다.
TEST_CASE("분할 백업 도중 다른 연결이 커밋하면 그 커밋까지 담은 스냅숏을 게시한다", "[core][storage][backup]")
{
	C_TEMP_TREE               Tree("split_concurrent");
	C_PROBE_FILE_SYSTEM       FileSystem;
	storage::C_BACKUP_SERVICE Service(FileSystem);

	const std::string sSource      = Tree.Child("pynote.sqlite3");
	const std::string sDestination = Tree.Child("backup.sqlite3");
	seed_database(sSource);

	// 백업이 도는 동안 살아 있는 앱 연결이다. WAL 이 붙은 상태의 원본을 읽기 전용으로 여는
	// 경로까지 이 시험이 덮는다.
	storage::C_DATABASE Live;
	REQUIRE(Live.Open(sSource));
	storage::C_REPOSITORIES Repositories(Live);

	int  nCalls   = 0;
	bool bWritten = false;

	storage::S_BACKUP_STEP_OPTIONS Options;
	Options.nPagesPerStep     = 1;
	Options.dBusyRetrySeconds = 0.01;
	Options.fnShouldContinue  = [&](int, int)
		{
			++nCalls;
			// 첫 step 이 페이지를 옮긴 뒤에 커밋해야 "도중"이 된다.
			if (nCalls == 2 && !bWritten)
			{
				bWritten = true;
				commit_one_card(Repositories, "concurrent");
			}
			return(true);
		};
	Service.SetStepOptions(Options);

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Service.Create(sSource, sDestination, &Inspection) == storage::E_BACKUP_RESULT::Ok);
	Live.Close();

	REQUIRE(bWritten);
	// 한 번에 한 페이지씩 옮겼으므로 루프가 실제로 나뉘어 돌았다.
	REQUIRE(nCalls > 1);

	// 게시된 것: 대상 하나뿐이고 임시 이름은 남지 않았다.
	REQUIRE(FileSystem.Exists(sDestination));
	REQUIRE(FileSystem.NoTemporaryLeftBehind());

	// 남은 스냅숏: SQLite 는 원본이 다른 연결에서 바뀌면 백업을 다시 시작하므로 마지막
	// 커밋까지 담긴 한 시점의 상태다. 찢긴 중간 상태가 아니라는 것은 검사 사슬이 증명한다.
	REQUIRE(query_int(sDestination, "SELECT COUNT(*) FROM cards") == 4);
	REQUIRE(query_int(sDestination, "SELECT COUNT(*) FROM cards WHERE id = 'card-concurrent'") == 1);
	REQUIRE(query_int(sSource, "SELECT COUNT(*) FROM cards") == 4);

	storage::S_BACKUP_INSPECTION Reinspected;
	REQUIRE(Service.Inspect(sDestination, &Reinspected) == storage::E_BACKUP_RESULT::Ok);
	REQUIRE(Reinspected.nSchemaVersion == storage::migrations::LatestSchemaVersion());
}

// 대응 원본: 없음. 취소는 분할 루프에만 있는 경로다(SPEC §2). pytest node ID 는 존재하지 않는다.
TEST_CASE("분할 백업을 취소하면 아무것도 게시하지 않고 임시 파일도 남기지 않는다", "[core][storage][backup]")
{
	C_TEMP_TREE               Tree("split_cancel");
	C_PROBE_FILE_SYSTEM       FileSystem;
	storage::C_BACKUP_SERVICE Service(FileSystem);

	const std::string sSource      = Tree.Child("pynote.sqlite3");
	const std::string sDestination = Tree.Child("backup.sqlite3");
	seed_database(sSource);

	const std::string sSourceBefore = read_bytes(sSource);

	int nCalls = 0;
	storage::S_BACKUP_STEP_OPTIONS Options;
	Options.nPagesPerStep     = 1;
	Options.dBusyRetrySeconds = 0.01;
	Options.fnShouldContinue  = [&nCalls](int, int)
		{
			++nCalls;
			// 두 페이지를 옮긴 뒤 세 번째 step 직전에 그만둔다.
			return(nCalls <= 2);
		};
	Service.SetStepOptions(Options);

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Service.Create(sSource, sDestination, &Inspection) == storage::E_BACKUP_RESULT::Cancelled);
	REQUIRE(nCalls == 3);
	REQUIRE(contains(Service.LastError(), "온라인 백업이 취소되었습니다."));

	// 게시된 것: 없음. 부분 스냅숏은 대상 이름으로 나가지 않는다.
	REQUIRE_FALSE(FileSystem.Exists(sDestination));
	REQUIRE(FileSystem.TemporaryPaths.size() == 1);
	REQUIRE(FileSystem.NoTemporaryLeftBehind());

	// 원본은 그대로다.
	REQUIRE(read_bytes(sSource) == sSourceBefore);
}

// 대응 원본: 없음. 분할 루프의 중도 실패 경로다(SPEC §2). pytest node ID 는 존재하지 않는다.
TEST_CASE("분할 백업이 원본에서 실패하면 아무것도 게시하지 않는다", "[core][storage][backup]")
{
	C_TEMP_TREE               Tree("split_source_failed");
	C_PROBE_FILE_SYSTEM       FileSystem;
	storage::C_BACKUP_SERVICE Service(FileSystem);

	const std::string sSource      = Tree.Child("pynote.sqlite3");
	const std::string sDestination = Tree.Child("backup.sqlite3");

	// SQLite 파일이 아닌 원본이다. 읽기 전용 열기는 성공하고 백업 루프에서 실패한다 -
	// 원본 검사(is_file)만으로는 걸러지지 않는 자리다.
	write_bytes(sSource, "not a sqlite database at all");

	int nCalls = 0;
	storage::S_BACKUP_STEP_OPTIONS Options;
	Options.nPagesPerStep     = 1;
	Options.dBusyRetrySeconds = 0.01;
	Options.fnShouldContinue  = [&nCalls](int, int) { ++nCalls; return(true); };
	Service.SetStepOptions(Options);

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Service.Create(sSource, sDestination, &Inspection) == storage::E_BACKUP_RESULT::Failed);

	// 게시된 것: 없음. 임시 파일도 남지 않는다.
	REQUIRE_FALSE(FileSystem.Exists(sDestination));
	REQUIRE(FileSystem.TemporaryPaths.size() == 1);
	REQUIRE(FileSystem.NoTemporaryLeftBehind());
	REQUIRE_FALSE(Service.LastError().empty());
}

// 대응 원본: 없음. 같은 실패를 대상 쪽에서 본 것이다(SPEC §2). pytest node ID 는 존재하지 않는다.
TEST_CASE("분할 백업이 대상에서 실패하면 아무것도 게시하지 않는다", "[core][storage][backup]")
{
	C_TEMP_TREE               Tree("split_destination_failed");
	C_PROBE_FILE_SYSTEM       FileSystem;
	storage::C_BACKUP_SERVICE Service(FileSystem);

	const std::string sSource      = Tree.Child("pynote.sqlite3");
	const std::string sDestination = Tree.Child("backup.sqlite3");
	seed_database(sSource);

	// 임시 이름 자리에 SQLite 가 아닌 바이트가 이미 놓여 있는 상황이다.
	FileSystem.bCorruptTemporary = true;

	storage::S_BACKUP_STEP_OPTIONS Options;
	Options.nPagesPerStep     = 1;
	Options.dBusyRetrySeconds = 0.01;
	Service.SetStepOptions(Options);

	storage::S_BACKUP_INSPECTION Inspection;
	REQUIRE(Service.Create(sSource, sDestination, &Inspection) == storage::E_BACKUP_RESULT::Failed);

	REQUIRE_FALSE(FileSystem.Exists(sDestination));
	REQUIRE(FileSystem.TemporaryPaths.size() == 1);
	REQUIRE(FileSystem.NoTemporaryLeftBehind());
	REQUIRE_FALSE(Service.LastError().empty());
}
