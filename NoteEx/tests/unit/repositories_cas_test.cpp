#include <catch_amalgamated.hpp>

#include "pynote/core/storage/migration_runner.h"
#include "pynote/core/storage/repositories.h"

#include <sqlite3/sqlite3.h>

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#pragma comment(lib, "NoteExCore")

namespace
{
	namespace domain = pynote::core::domain;
	namespace storage = pynote::core::storage;

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

	class C_REPO_FIXTURE
	{
	public:
		explicit C_REPO_FIXTURE(const std::string& _sName)
			: m_Temp(_sName)
			, m_Repositories(m_Database)
		{
			REQUIRE(m_Database.Open(m_Temp.Utf8()));

			storage::C_MIGRATION_RUNNER Runner;
			Runner.SetExistingDatabase(false, m_Temp.Utf8());
			REQUIRE(Runner.Run(m_Database) == storage::E_MIGRATE_RESULT::Ok);
		}

		storage::C_DATABASE&     Db() { return(m_Database); }
		storage::C_REPOSITORIES& Repo() { return(m_Repositories); }

	private:
		C_TEMP_DB               m_Temp;
		storage::C_DATABASE     m_Database;
		storage::C_REPOSITORIES m_Repositories;
	};

	// 좁은 리터럴은 이 기계에서 CP949 로 컴파일되므로(spec_TR2 §1(a)) 저장할 한국어는 u8 로 쓴다.
	std::string u8s(const char8_t* _pszText)
	{
		return(std::string(reinterpret_cast<const char*>(_pszText)));
	}

	domain::S_DOCUMENT make_document()
	{
		domain::S_DOCUMENT Document;
		Document.sId          = "document-1";
		Document.sTitle       = u8s(u8"테스트 문서");
		Document.nCreatedAtUs = 1000;
		Document.nUpdatedAtUs = 1000;
		return(Document);
	}

	domain::S_NEW_CAPTURE_OPERATION make_operation(const std::string& _sId)
	{
		domain::S_NEW_CAPTURE_OPERATION Operation;
		Operation.sId          = _sId;
		Operation.sDocumentId  = "document-1";
		Operation.eSource      = domain::E_CAPTURE_OPERATION_SOURCE::Typing;
		Operation.eSplitPolicy = domain::E_SPLIT_POLICY::Keep;
		Operation.nCreatedAtUs = 2000;
		return(Operation);
	}

	domain::S_NEW_CARD make_new_card(int _nNumber, const std::string& _sBody)
	{
		domain::S_NEW_CARD Card;
		Card.sId             = "card-" + std::to_string(_nNumber);
		Card.sRevisionId     = "revision-" + std::to_string(_nNumber);
		Card.sEventId        = "event-" + std::to_string(_nNumber);
		Card.nPositionKey    = _nNumber * 1024;
		Card.sBody           = _sBody;
		Card.eCardSource     = domain::E_CARD_SOURCE::Typing;
		Card.eEventSource    = domain::E_EVENT_SOURCE::Typing;
		Card.eRevisionSource = domain::E_REVISION_SOURCE::Edit;
		Card.nCreatedAtUs    = 2000 + _nNumber;
		return(Card);
	}

	std::int64_t count_rows(storage::C_DATABASE& _database, const char* _pszTable)
	{
		const std::string sSql = std::string("SELECT COUNT(*) FROM ") + _pszTable;
		sqlite3_stmt* pStmt = nullptr;
		REQUIRE(::sqlite3_prepare_v2(_database.Handle(), sSql.c_str(), -1, &pStmt, nullptr) == SQLITE_OK);
		REQUIRE(::sqlite3_step(pStmt) == SQLITE_ROW);
		const std::int64_t nCount = ::sqlite3_column_int64(pStmt, 0);
		::sqlite3_finalize(pStmt);
		return(nCount);
	}

	// 문서 하나와 카드 하나를 만들고 그 카드를 돌려준다.
	domain::S_CARD seed_one_card(C_REPO_FIXTURE& _Fixture, const std::string& _sBody)
	{
		REQUIRE(_Fixture.Repo().CreateDocument(make_document()) == storage::E_REPO_RESULT::Ok);
		std::vector<domain::S_CARD> Created;
		REQUIRE(_Fixture.Repo().CreateCards(make_operation("operation-1"), { make_new_card(1, _sBody) }, &Created)
			== storage::E_REPO_RESULT::Ok);
		REQUIRE(Created.size() == 1);
		return(Created[0]);
	}

	// 카드에 새 리비전 한 개를 더 붙이고 그 id 를 돌려준다. 전진 CAS 의 준비물이다.
	std::string add_revision(C_REPO_FIXTURE& _Fixture, const domain::S_CARD& _Card, const std::string& _sBody)
	{
		domain::S_EDIT_EVENT NewEvent;
		NewEvent.sEventId      = "event-advance";
		NewEvent.sDocumentId   = _Card.sDocumentId;
		NewEvent.sCardId       = _Card.sId;
		NewEvent.eEventType    = domain::E_EVENT_TYPE::Update;
		NewEvent.eSource       = domain::E_EVENT_SOURCE::Edit;
		NewEvent.nOccurredAtUs = 5000;
		NewEvent.sDetailsJson  = "{}";

		domain::S_EDIT_EVENT StoredEvent;
		REQUIRE(_Fixture.Repo().CreateEvent(NewEvent, &StoredEvent) == storage::E_REPO_RESULT::Ok);

		domain::S_CARD_REVISION Revision;
		Revision.sId               = "revision-next";
		Revision.sCardId           = _Card.sId;
		Revision.nEventSeq         = *StoredEvent.nEventSeq;
		Revision.sParentRevisionId = _Card.sCurrentRevisionId;
		Revision.sBody             = _sBody;
		Revision.sBodyHash         = storage::TextHash(_sBody);
		Revision.eSource           = domain::E_REVISION_SOURCE::Edit;
		Revision.nCreatedAtUs      = 5000;
		REQUIRE(_Fixture.Repo().CreateRevision(Revision) == storage::E_REPO_RESULT::Ok);
		return(Revision.sId);
	}
}

// 대응 원본: repositories.py 의 link_initial_revision(:405~415)과 _require_card_cas(:820~825).
// 파이썬 시험 트리에 저장소 단위 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("최초 리비전 연결은 NULL 일 때만 성립한다", "[core][storage][repositories][cas]")
{
	C_REPO_FIXTURE Fixture("cas_link_initial");
	REQUIRE(Fixture.Repo().CreateDocument(make_document()) == storage::E_REPO_RESULT::Ok);

	// 연결 전 상태를 만들려면 create_cards 를 거치지 않고 손으로 쌓아야 한다.
	domain::S_CAPTURE_OPERATION Operation;
	Operation.sId          = "operation-1";
	Operation.sDocumentId  = "document-1";
	Operation.eSource      = domain::E_CAPTURE_OPERATION_SOURCE::Typing;
	Operation.eSplitPolicy = domain::E_SPLIT_POLICY::Keep;
	Operation.nCreatedAtUs = 2000;
	REQUIRE(Fixture.Repo().CreateCaptureOperation(Operation) == storage::E_REPO_RESULT::Ok);

	domain::S_EDIT_EVENT NewEvent;
	NewEvent.sEventId      = "event-1";
	NewEvent.sOperationId  = "operation-1";
	NewEvent.sDocumentId   = "document-1";
	NewEvent.eEventType    = domain::E_EVENT_TYPE::Create;
	NewEvent.eSource       = domain::E_EVENT_SOURCE::Typing;
	NewEvent.nOccurredAtUs = 2001;
	NewEvent.sDetailsJson  = "{}";
	domain::S_EDIT_EVENT StoredEvent;
	REQUIRE(Fixture.Repo().CreateEvent(NewEvent, &StoredEvent) == storage::E_REPO_RESULT::Ok);

	const std::string sBody = u8s(u8"손으로 쌓은 카드");
	domain::S_CARD Card;
	Card.sId                = "card-1";
	Card.sDocumentId        = "document-1";
	Card.sOperationId       = "operation-1";
	Card.nPositionKey       = 1024;
	Card.nCaptureSeq        = 100;
	Card.nCreatedAtUs       = 2001;
	Card.nUpdatedAtUs       = 2001;
	Card.eSource            = domain::E_CARD_SOURCE::Typing;
	Card.sBody              = sBody;
	Card.sBodyHash          = storage::TextHash(sBody);
	Card.sCurrentRevisionId = std::nullopt;
	REQUIRE(Fixture.Repo().CreateCard(Card) == storage::E_REPO_RESULT::Ok);

	domain::S_CARD_REVISION Revision;
	Revision.sId          = "revision-1";
	Revision.sCardId      = "card-1";
	Revision.nEventSeq    = *StoredEvent.nEventSeq;
	Revision.sBody        = sBody;
	Revision.sBodyHash    = Card.sBodyHash;
	Revision.eSource      = domain::E_REVISION_SOURCE::Edit;
	Revision.nCreatedAtUs = 2001;
	REQUIRE(Fixture.Repo().CreateRevision(Revision) == storage::E_REPO_RESULT::Ok);

	// 성공 경로.
	REQUIRE(Fixture.Repo().LinkInitialRevision("card-1", "revision-1") == storage::E_REPO_RESULT::Ok);
	domain::S_CARD Fetched;
	REQUIRE(Fixture.Repo().GetCard("card-1", &Fetched) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fetched.sCurrentRevisionId.has_value());
	REQUIRE(*Fetched.sCurrentRevisionId == "revision-1");

	// 충돌 경로. 이미 연결된 카드는 술어가 빗나가 영향 행수가 0 이다.
	REQUIRE(Fixture.Repo().LinkInitialRevision("card-1", "revision-1") == storage::E_REPO_RESULT::CasConflict);
	REQUIRE(Fixture.Db().LastError() == "카드가 예상한 현재 리비전에서 변경되었습니다: card-1");

	// 없는 카드도 같은 갈래다 - 영향 행수 0.
	REQUIRE(Fixture.Repo().LinkInitialRevision("card-missing", "revision-1") == storage::E_REPO_RESULT::CasConflict);
}

// 대응 원본: tests/integration/test_storage_concurrency.py::test_save_rereads_base_after_begin_and_preserves_competing_commit
// (그 시험의 _commit_competing_edit 가 advance_card_revision 성공 경로를 탄다. repositories.py:417~441).
// 충돌 경로는 파이썬 시험 트리에 저장소 단위 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("리비전 전진은 예상 리비전이 맞을 때만 성립한다", "[core][storage][repositories][cas]")
{
	C_REPO_FIXTURE Fixture("cas_advance");
	const domain::S_CARD Card = seed_one_card(Fixture, u8s(u8"첫 본문"));
	const std::string sNewBody = u8s(u8"고친 본문");
	const std::string sNextRevisionId = add_revision(Fixture, Card, sNewBody);

	domain::S_CARD Advanced = Card;
	Advanced.sBody              = sNewBody;
	Advanced.sBodyHash          = storage::TextHash(sNewBody);
	Advanced.sCurrentRevisionId = sNextRevisionId;
	Advanced.nUpdatedAtUs       = 5000;
	Advanced.eSource            = domain::E_CARD_SOURCE::Mixed;

	// 충돌 먼저 - 남이 앞질러 간 상태를 흉내내려고 틀린 예상값을 준다.
	REQUIRE(Fixture.Repo().AdvanceCardRevision(Advanced, "revision-missing") == storage::E_REPO_RESULT::CasConflict);
	REQUIRE(Fixture.Db().LastError() == "카드가 예상한 현재 리비전에서 변경되었습니다: card-1");

	// 충돌은 아무것도 바꾸지 않는다.
	domain::S_CARD Fetched;
	REQUIRE(Fixture.Repo().GetCard("card-1", &Fetched) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fetched == Card);

	REQUIRE(Fixture.Repo().AdvanceCardRevision(Advanced, *Card.sCurrentRevisionId) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fixture.Repo().GetCard("card-1", &Fetched) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fetched == Advanced);

	// 같은 전진을 한 번 더 부르면 예상값이 이미 낡았다.
	REQUIRE(Fixture.Repo().AdvanceCardRevision(Advanced, *Card.sCurrentRevisionId)
		== storage::E_REPO_RESULT::CasConflict);
}

// 대응 원본: tests/integration/test_repositories.py::test_partial_unique_indexes_enforce_active_card_and_draft_rules
// (repositories.py 의 update_card_position :443~459).
TEST_CASE("카드 위치 변경은 예상 리비전이 맞을 때만 성립한다", "[core][storage][repositories][cas]")
{
	C_REPO_FIXTURE Fixture("cas_position");
	const domain::S_CARD Card = seed_one_card(Fixture, u8s(u8"첫 본문"));

	REQUIRE(Fixture.Repo().UpdateCardPosition("card-1", 4096, "revision-missing") == storage::E_REPO_RESULT::CasConflict);
	domain::S_CARD Fetched;
	REQUIRE(Fixture.Repo().GetCard("card-1", &Fetched) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fetched.nPositionKey == Card.nPositionKey);

	REQUIRE(Fixture.Repo().UpdateCardPosition("card-1", 4096, *Card.sCurrentRevisionId) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fixture.Repo().GetCard("card-1", &Fetched) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fetched.nPositionKey == 4096);

	// 위치 변경은 현재 리비전을 바꾸지 않으므로 같은 예상값으로 다시 부를 수 있다.
	REQUIRE(Fixture.Repo().UpdateCardPosition("card-1", 8192, *Card.sCurrentRevisionId) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fixture.Repo().GetCard("card-1", &Fetched) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fetched.nPositionKey == 8192);
}

// 대응 원본: tests/integration/test_repositories.py::test_partial_unique_indexes_enforce_active_card_and_draft_rules
// (repositories.py 의 update_card_deleted_state :461~478).
TEST_CASE("카드 휴지통 상태 변경은 예상 리비전이 맞을 때만 성립한다", "[core][storage][repositories][cas]")
{
	C_REPO_FIXTURE Fixture("cas_deleted");
	const domain::S_CARD Card = seed_one_card(Fixture, u8s(u8"첫 본문"));

	REQUIRE(Fixture.Repo().UpdateCardDeletedState("card-1", Card.nPositionKey, 3000, "revision-missing")
		== storage::E_REPO_RESULT::CasConflict);
	domain::S_CARD Fetched;
	REQUIRE(Fixture.Repo().GetCard("card-1", &Fetched) == storage::E_REPO_RESULT::Ok);
	REQUIRE_FALSE(Fetched.nDeletedAtUs.has_value());

	REQUIRE(Fixture.Repo().UpdateCardDeletedState("card-1", Card.nPositionKey, 3000, *Card.sCurrentRevisionId)
		== storage::E_REPO_RESULT::Ok);
	REQUIRE(Fixture.Repo().GetCard("card-1", &Fetched) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fetched.nDeletedAtUs.has_value());
	REQUIRE(*Fetched.nDeletedAtUs == 3000);

	// 복원은 같은 연산에 NULL 을 넣는 것이다.
	REQUIRE(Fixture.Repo().UpdateCardDeletedState("card-1", Card.nPositionKey, std::nullopt, *Card.sCurrentRevisionId)
		== storage::E_REPO_RESULT::Ok);
	REQUIRE(Fixture.Repo().GetCard("card-1", &Fetched) == storage::E_REPO_RESULT::Ok);
	REQUIRE_FALSE(Fetched.nDeletedAtUs.has_value());
}

// 대응 원본: tests/integration/test_repositories.py::test_partial_unique_indexes_enforce_active_card_and_draft_rules
// (:198 - 점유된 활성 위치로 옮기면 CAS 실패가 아니라 sqlite3.IntegrityError 다).
// 계약 대장 §4-4: 파이썬은 execute() 가 예외를 던져 _require_card_cas 에 도달조차 하지 않는다.
// 이식본이 sqlite3_changes 를 step 결과보다 먼저 보면 제약 위반(변경 0행)을 CAS 충돌로 오분류하고,
// CasConflict 를 재시도 신호로 읽는 W2 조정자가 성공할 수 없는 요청을 무한 재시도한다.
TEST_CASE("제약 위반은 CAS 충돌이 아니라 실패로 분류된다", "[core][storage][repositories][cas]")
{
	C_REPO_FIXTURE Fixture("cas_constraint_class");
	REQUIRE(Fixture.Repo().CreateDocument(make_document()) == storage::E_REPO_RESULT::Ok);

	std::vector<domain::S_CARD> Created;
	REQUIRE(Fixture.Repo().CreateCards(
		make_operation("operation-1"),
		{ make_new_card(1, u8s(u8"첫 카드")), make_new_card(2, u8s(u8"둘째 카드")) },
		&Created) == storage::E_REPO_RESULT::Ok);
	const domain::S_CARD& First = Created[0];
	const domain::S_CARD& Second = Created[1];

	// 예상 리비전은 맞다. 걸리는 것은 active_card_position 부분 유니크 인덱스다.
	const storage::E_REPO_RESULT eResult =
		Fixture.Repo().UpdateCardPosition(Second.sId, First.nPositionKey, *Second.sCurrentRevisionId);
	REQUIRE(eResult == storage::E_REPO_RESULT::Failed);
	REQUIRE_FALSE(eResult == storage::E_REPO_RESULT::CasConflict);
	REQUIRE(Fixture.Db().LastError() != "카드가 예상한 현재 리비전에서 변경되었습니다: card-2");

	domain::S_CARD Fetched;
	REQUIRE(Fixture.Repo().GetCard(Second.sId, &Fetched) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fetched.nPositionKey == Second.nPositionKey);

	// 첫 카드를 휴지통으로 보내면 활성 인덱스에서 빠져 같은 이동이 성립한다 - 재시도가
	// 의미를 갖는 CasConflict 와 달리 위 실패는 상태가 바뀌어야만 풀린다.
	REQUIRE(Fixture.Repo().UpdateCardDeletedState(
		First.sId, First.nPositionKey, 3000, *First.sCurrentRevisionId) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fixture.Repo().UpdateCardPosition(Second.sId, First.nPositionKey, *Second.sCurrentRevisionId)
		== storage::E_REPO_RESULT::Ok);
}

// 대응 원본: repositories.py 의 advance_card_revision(:417~441) SET 절 5개 열.
// 계약 대장 §4-6: 구조체를 통째로 UPDATE 하면 휴지통 카드가 되살아나고 위치가 되돌아간다.
// 파이썬 시험 트리에 이 조합을 고정하는 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("리비전 전진은 다섯 열만 쓰고 나머지 열은 건드리지 않는다", "[core][storage][repositories][cas]")
{
	C_REPO_FIXTURE Fixture("cas_advance_columns");
	const domain::S_CARD Card = seed_one_card(Fixture, u8s(u8"첫 본문"));

	// 카드를 휴지통에 보낸 뒤, 그 사실을 모르는 낡은 구조체로 편집 저장을 건다.
	REQUIRE(Fixture.Repo().UpdateCardDeletedState(
		Card.sId, Card.nPositionKey, 3000, *Card.sCurrentRevisionId) == storage::E_REPO_RESULT::Ok);

	const std::string sNewBody = u8s(u8"고친 본문");
	const std::string sNextRevisionId = add_revision(Fixture, Card, sNewBody);

	domain::S_CARD Stale = Card;                 // nDeletedAtUs 는 여전히 없음이다.
	Stale.nPositionKey       = 9999;             // 아래 넷은 기록되면 안 되는 값이다.
	Stale.nCaptureSeq        = 777;
	Stale.nCreatedAtUs       = 111;
	Stale.sDocumentId        = "document-1";
	Stale.sBody              = sNewBody;
	Stale.sBodyHash          = storage::TextHash(sNewBody);
	Stale.sCurrentRevisionId = sNextRevisionId;
	Stale.nUpdatedAtUs       = 5000;

	REQUIRE(Fixture.Repo().AdvanceCardRevision(Stale, *Card.sCurrentRevisionId) == storage::E_REPO_RESULT::Ok);

	domain::S_CARD Fetched;
	REQUIRE(Fixture.Repo().GetCard(Card.sId, &Fetched) == storage::E_REPO_RESULT::Ok);

	// 쓴 다섯 열.
	REQUIRE(Fetched.nUpdatedAtUs == 5000);
	REQUIRE(Fetched.sBody == sNewBody);
	REQUIRE(Fetched.sBodyHash == Stale.sBodyHash);
	REQUIRE(Fetched.sCurrentRevisionId.has_value());
	REQUIRE(*Fetched.sCurrentRevisionId == sNextRevisionId);

	// 쓰지 않는 열. 휴지통 카드가 되살아나면 여기서 걸린다.
	REQUIRE(Fetched.nDeletedAtUs.has_value());
	REQUIRE(*Fetched.nDeletedAtUs == 3000);
	REQUIRE(Fetched.nPositionKey == Card.nPositionKey);
	REQUIRE(Fetched.nCaptureSeq == Card.nCaptureSeq);
	REQUIRE(Fetched.nCreatedAtUs == Card.nCreatedAtUs);
}

// 대응 원본: tests/integration/test_repositories.py::test_keep_operation_rejects_duplicate_original_text
// 와 ::test_new_input_split_requires_original_and_existing_transform_forbids_it (repositories.py:722~737).
TEST_CASE("create_cards 는 네 계약 위반을 트랜잭션 전에 거절한다", "[core][storage][repositories]")
{
	C_REPO_FIXTURE Fixture("create_cards_reject");
	REQUIRE(Fixture.Repo().CreateDocument(make_document()) == storage::E_REPO_RESULT::Ok);

	std::vector<domain::S_CARD> Created;

	// 1. 카드가 없다.
	REQUIRE(Fixture.Repo().CreateCards(make_operation("operation-empty"), {}, &Created)
		== storage::E_REPO_RESULT::Invalid);
	REQUIRE(Fixture.Db().LastError() == "새 카드 저장에는 카드가 한 개 이상 필요합니다.");

	// 2. 기존 카드 분할/병합이 원문을 싣고 왔다.
	domain::S_NEW_CAPTURE_OPERATION Transform = make_operation("operation-split");
	Transform.eSource       = domain::E_CAPTURE_OPERATION_SOURCE::Split;
	Transform.eSplitPolicy  = domain::E_SPLIT_POLICY::SplitByBlankLine;
	Transform.sOriginalText = u8s(u8"기존 카드 원문");
	REQUIRE(Fixture.Repo().CreateCards(Transform, { make_new_card(1, u8s(u8"첫 문단")) }, &Created)
		== storage::E_REPO_RESULT::Invalid);
	REQUIRE(Fixture.Db().LastError() == "기존 카드 분할·병합 작업은 원문을 중복 저장하지 않습니다.");

	// 3. keep 작업이 원문을 싣고 왔다.
	domain::S_NEW_CAPTURE_OPERATION Keep = make_operation("operation-keep");
	Keep.sOriginalText = u8s(u8"중복 원문");
	REQUIRE(Fixture.Repo().CreateCards(Keep, { make_new_card(1, u8s(u8"중복 원문")) }, &Created)
		== storage::E_REPO_RESULT::Invalid);
	REQUIRE(Fixture.Db().LastError() == "keep 작업은 카드 본문과 같은 원문을 중복 저장하지 않습니다.");

	// 4. 새 입력 분할인데 원문이 없다.
	domain::S_NEW_CAPTURE_OPERATION Missing = make_operation("operation-missing");
	Missing.eSource      = domain::E_CAPTURE_OPERATION_SOURCE::Paste;
	Missing.eSplitPolicy = domain::E_SPLIT_POLICY::SplitByBlankLine;
	REQUIRE(Fixture.Repo().CreateCards(Missing, { make_new_card(1, u8s(u8"첫 문단")) }, &Created)
		== storage::E_REPO_RESULT::Invalid);
	REQUIRE(Fixture.Db().LastError() == "새 입력 분할 작업은 정확한 원문이 필요합니다.");

	// 거절은 트랜잭션 전이라 행이 하나도 생기지 않고 counter 도 그대로다.
	REQUIRE(count_rows(Fixture.Db(), "capture_operations") == 0);
	REQUIRE(count_rows(Fixture.Db(), "cards") == 0);
	REQUIRE(count_rows(Fixture.Db(), "edit_events") == 0);
	std::int64_t nCounter = 0;
	REQUIRE(Fixture.Repo().GetCounter("capture", &nCounter) == storage::E_REPO_RESULT::Ok);
	REQUIRE(nCounter == 1);

	// 계약 대장 §4-24: 조건 2와 3은 동시에 참일 수 있고(SPLIT 소스 + KEEP 정책 + 원문 존재)
	// 그때는 2번 메시지가 나간다. 부분 문자열("중복 저장")만 보는 시험은 뒤바뀜을 못 잡으므로
	// 여기서는 전문을 대조한다.
	domain::S_NEW_CAPTURE_OPERATION Overlap = make_operation("operation-overlap");
	Overlap.eSource       = domain::E_CAPTURE_OPERATION_SOURCE::Split;
	Overlap.eSplitPolicy  = domain::E_SPLIT_POLICY::Keep;
	Overlap.sOriginalText = u8s(u8"겹치는 원문");
	REQUIRE(Fixture.Repo().CreateCards(Overlap, { make_new_card(1, u8s(u8"첫 문단")) }, &Created)
		== storage::E_REPO_RESULT::Invalid);
	REQUIRE(Fixture.Db().LastError() == "기존 카드 분할·병합 작업은 원문을 중복 저장하지 않습니다.");

	// 기존 카드 분할이 원문 없이 오는 것은 정상 경로다 - 3번 술어가 4번을 삼키면 여기서 걸린다.
	domain::S_NEW_CAPTURE_OPERATION Allowed = make_operation("operation-ok");
	Allowed.eSource      = domain::E_CAPTURE_OPERATION_SOURCE::Split;
	Allowed.eSplitPolicy = domain::E_SPLIT_POLICY::SplitByBlankLine;
	REQUIRE(Fixture.Repo().CreateCards(Allowed, { make_new_card(1, u8s(u8"첫 문단")) }, &Created)
		== storage::E_REPO_RESULT::Ok);
	REQUIRE(Created.size() == 1);
}

// 대응 원본: tests/integration/test_repositories.py::test_new_cards_roll_back_every_row_and_counter_on_middle_failure
// (repositories.py 의 create_cards 트랜잭션 :754~816).
TEST_CASE("create_cards 는 중도 실패에서 모든 행과 counter 를 되돌린다", "[core][storage][repositories]")
{
	C_REPO_FIXTURE Fixture("create_cards_atomic");
	REQUIRE(Fixture.Repo().CreateDocument(make_document()) == storage::E_REPO_RESULT::Ok);

	// 두 번째 리비전 삽입만 중단시킨다. 중단 문구는 저장되는 SQL 이라 ASCII 로 쓴다.
	REQUIRE(Fixture.Db().Execute(
		"CREATE TRIGGER fail_second_revision"
		" BEFORE INSERT ON card_revisions"
		" WHEN NEW.id = 'revision-2'"
		" BEGIN"
		"     SELECT RAISE(ABORT, 'intended mid-sequence failure');"
		" END"));

	std::vector<domain::S_CARD> Created;
	REQUIRE(Fixture.Repo().CreateCards(
		make_operation("operation-1"),
		{ make_new_card(1, u8s(u8"첫 카드")), make_new_card(2, u8s(u8"두 번째 카드")) },
		&Created) == storage::E_REPO_RESULT::Failed);

	// 첫 카드까지는 트랜잭션 안에서 성공했지만 전부 되돌아가야 한다.
	REQUIRE(count_rows(Fixture.Db(), "capture_operations") == 0);
	REQUIRE(count_rows(Fixture.Db(), "cards") == 0);
	REQUIRE(count_rows(Fixture.Db(), "card_revisions") == 0);
	REQUIRE(count_rows(Fixture.Db(), "edit_events") == 0);

	std::int64_t nCounter = 0;
	REQUIRE(Fixture.Repo().GetCounter("capture", &nCounter) == storage::E_REPO_RESULT::Ok);
	REQUIRE(nCounter == 1);

	// 쓰기 잠금도 함께 풀려 있어야 다음 트랜잭션이 열린다.
	REQUIRE(::sqlite3_get_autocommit(Fixture.Db().Handle()) != 0);

	REQUIRE(Fixture.Db().Execute("DROP TRIGGER fail_second_revision"));
	REQUIRE(Fixture.Repo().CreateCards(make_operation("operation-2"), { make_new_card(1, u8s(u8"첫 카드")) }, &Created)
		== storage::E_REPO_RESULT::Ok);
	REQUIRE(Created.size() == 1);
	REQUIRE(Created[0].nCaptureSeq == 1);
}

// 대응 원본: repositories.py 의 create_cards 반환값(:802~818) - 돌려주는 카드는
// 트랜잭션 안에서 만든 값에 없던 current_revision_id 를 달고 있다.
// 파이썬 시험 트리에 이 계약만 보는 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("create_cards 가 돌려주는 카드는 연결된 리비전 id 를 달고 있다", "[core][storage][repositories]")
{
	C_REPO_FIXTURE Fixture("create_cards_return");
	REQUIRE(Fixture.Repo().CreateDocument(make_document()) == storage::E_REPO_RESULT::Ok);

	std::vector<domain::S_CARD> Created;
	REQUIRE(Fixture.Repo().CreateCards(
		make_operation("operation-1"),
		{ make_new_card(1, u8s(u8"첫 카드")), make_new_card(2, u8s(u8"둘째 카드")) },
		&Created) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Created.size() == 2);

	for (const domain::S_CARD& Card : Created)
	{
		INFO("카드: " << Card.sId);
		REQUIRE(Card.sCurrentRevisionId.has_value());

		domain::S_CARD Fetched;
		REQUIRE(Fixture.Repo().GetCard(Card.sId, &Fetched) == storage::E_REPO_RESULT::Ok);
		REQUIRE(Fetched == Card);

		domain::S_CARD_REVISION Revision;
		REQUIRE(Fixture.Repo().GetRevision(*Card.sCurrentRevisionId, &Revision) == storage::E_REPO_RESULT::Ok);
		REQUIRE(Revision.sCardId == Card.sId);
		REQUIRE(Revision.sBodyHash == Card.sBodyHash);
		REQUIRE_FALSE(Revision.sParentRevisionId.has_value());
	}

	// capture 순번은 카드 순서대로 발급된다.
	REQUIRE(Created[0].nCaptureSeq == 1);
	REQUIRE(Created[1].nCaptureSeq == 2);

	// 이벤트는 카드마다 하나씩, 발급 순서대로다.
	std::vector<domain::S_EDIT_EVENT> Events;
	REQUIRE(Fixture.Repo().ListEvents("document-1", &Events) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Events.size() == 2);
	REQUIRE(Events[0].sEventId == "event-1");
	REQUIRE(Events[1].sEventId == "event-2");
	REQUIRE(Events[0].eEventType == domain::E_EVENT_TYPE::Create);
	REQUIRE(Events[0].sOperationId.has_value());
	REQUIRE(*Events[0].sOperationId == "operation-1");

	// 저장된 작업 행은 원문 없이 해시도 NULL 이다(keep 정책).
	domain::S_CAPTURE_OPERATION Operation;
	REQUIRE(Fixture.Repo().GetCaptureOperation("operation-1", &Operation) == storage::E_REPO_RESULT::Ok);
	REQUIRE_FALSE(Operation.sOriginalText.has_value());
	REQUIRE_FALSE(Operation.sOriginalHash.has_value());
	REQUIRE_FALSE(Operation.nOriginalRedactedAtUs.has_value());
}

// 대응 원본: tests/integration/test_repositories.py::test_split_input_preserves_original_text_once
// (repositories.py 의 create_cards 원문 해시 계산 :739~751).
TEST_CASE("create_cards 는 원문을 한 번만 저장하고 해시를 함께 남긴다", "[core][storage][repositories]")
{
	C_REPO_FIXTURE Fixture("create_cards_original");
	REQUIRE(Fixture.Repo().CreateDocument(make_document()) == storage::E_REPO_RESULT::Ok);

	const std::string sOriginal = u8s(u8"첫 문단\n\n두 번째 문단");
	domain::S_NEW_CAPTURE_OPERATION Operation = make_operation("operation-split");
	Operation.eSource       = domain::E_CAPTURE_OPERATION_SOURCE::Paste;
	Operation.eSplitPolicy  = domain::E_SPLIT_POLICY::SplitByBlankLine;
	Operation.sOriginalText = sOriginal;

	std::vector<domain::S_CARD> Created;
	REQUIRE(Fixture.Repo().CreateCards(
		Operation,
		{ make_new_card(1, u8s(u8"첫 문단")), make_new_card(2, u8s(u8"두 번째 문단")) },
		&Created) == storage::E_REPO_RESULT::Ok);

	domain::S_CAPTURE_OPERATION Stored;
	REQUIRE(Fixture.Repo().GetCaptureOperation("operation-split", &Stored) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Stored.sOriginalText.has_value());
	REQUIRE(*Stored.sOriginalText == sOriginal);
	REQUIRE(Stored.sOriginalHash.has_value());
	REQUIRE(*Stored.sOriginalHash == storage::TextHash(sOriginal));
	REQUIRE_FALSE(Stored.nOriginalRedactedAtUs.has_value());
	REQUIRE(count_rows(Fixture.Db(), "capture_operations") == 1);
}

// 대응 원본: repositories.py 의 create_cards 루프(:756~801).
// 계약 대장 §4-17(last_insert_rowid 채취 시점), §4-22(시각 출처), §4-23(서로 다른 세 source).
// 파이썬 시험 트리에 이 셋을 함께 고정하는 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("create_cards 는 카드별 시각·순번·세 source 를 각자 자리에 넣는다", "[core][storage][repositories]")
{
	C_REPO_FIXTURE Fixture("create_cards_fields");
	REQUIRE(Fixture.Repo().CreateDocument(make_document()) == storage::E_REPO_RESULT::Ok);

	// 세 열거형은 값 집합이 다르다. import 는 리비전에 없고 merge 는 이벤트에 없으므로,
	// 하나로 합친 이식본은 CHECK 제약에 걸린다.
	domain::S_NEW_CARD FirstInput = make_new_card(1, u8s(u8"첫 카드"));
	FirstInput.eCardSource     = domain::E_CARD_SOURCE::Paste;
	FirstInput.eEventSource    = domain::E_EVENT_SOURCE::Import;
	FirstInput.eRevisionSource = domain::E_REVISION_SOURCE::Merge;
	domain::S_NEW_CARD SecondInput = make_new_card(2, u8s(u8"둘째 카드"));
	SecondInput.sEventDetailsJson = u8s(u8"{\"메모\":\"둘째\"}");

	std::vector<domain::S_CARD> Created;
	REQUIRE(Fixture.Repo().CreateCards(make_operation("operation-1"), { FirstInput, SecondInput }, &Created)
		== storage::E_REPO_RESULT::Ok);
	REQUIRE(Created.size() == 2);

	// 작업 행의 시각은 operation.created_at_us 다. 카드 시각으로 덮이면 여기서 걸린다.
	domain::S_CAPTURE_OPERATION Operation;
	REQUIRE(Fixture.Repo().GetCaptureOperation("operation-1", &Operation) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Operation.nCreatedAtUs == 2000);

	// 카드·이벤트·리비전의 시각은 카드마다 new_card.created_at_us 다.
	REQUIRE(Created[0].nCreatedAtUs == 2001);
	REQUIRE(Created[0].nUpdatedAtUs == 2001);
	REQUIRE(Created[1].nCreatedAtUs == 2002);
	REQUIRE(Created[1].nUpdatedAtUs == 2002);

	std::vector<domain::S_EDIT_EVENT> Events;
	REQUIRE(Fixture.Repo().ListEvents("document-1", &Events) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Events.size() == 2);
	REQUIRE(Events[0].nOccurredAtUs == 2001);
	REQUIRE(Events[1].nOccurredAtUs == 2002);
	REQUIRE(Events[1].sDetailsJson == SecondInput.sEventDetailsJson);

	// 리비전이 자기 카드의 이벤트를 참조해야 한다. last_insert_rowid 를 카드·리비전 INSERT
	// 뒤에 읽으면 여기서 어긋난다 - FK 는 존재하는 이벤트면 통과하므로 조용히 틀린 이력이 남는다.
	domain::S_CARD_REVISION FirstRevision;
	domain::S_CARD_REVISION SecondRevision;
	REQUIRE(Fixture.Repo().GetRevision("revision-1", &FirstRevision) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fixture.Repo().GetRevision("revision-2", &SecondRevision) == storage::E_REPO_RESULT::Ok);
	REQUIRE(FirstRevision.nEventSeq == *Events[0].nEventSeq);
	REQUIRE(SecondRevision.nEventSeq == *Events[1].nEventSeq);
	REQUIRE(FirstRevision.nEventSeq != SecondRevision.nEventSeq);
	REQUIRE(FirstRevision.nCreatedAtUs == 2001);
	REQUIRE(SecondRevision.nCreatedAtUs == 2002);

	// 세 source 는 각자 다른 열에 들어간다.
	REQUIRE(Created[0].eSource == domain::E_CARD_SOURCE::Paste);
	REQUIRE(Events[0].eSource == domain::E_EVENT_SOURCE::Import);
	REQUIRE(FirstRevision.eSource == domain::E_REVISION_SOURCE::Merge);
	REQUIRE(Events[0].eEventType == domain::E_EVENT_TYPE::Create);
}
