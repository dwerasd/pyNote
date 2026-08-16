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

	// 최신 스키마까지 올린 데이터베이스와 저장소 한 벌. 파이썬 시험의 database/repositories
	// 픽스처(tests/integration/conftest.py) 자리다.
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

	// 저장된 원문을 그대로 읽는다. 매퍼를 거치지 않아야 저장 형식 자체를 볼 수 있다.
	std::string read_text(storage::C_DATABASE& _database, const char* _pszSql)
	{
		sqlite3_stmt* pStmt = nullptr;
		REQUIRE(::sqlite3_prepare_v2(_database.Handle(), _pszSql, -1, &pStmt, nullptr) == SQLITE_OK);
		REQUIRE(::sqlite3_step(pStmt) == SQLITE_ROW);
		const unsigned char* pText = ::sqlite3_column_text(pStmt, 0);
		const int            nSize = ::sqlite3_column_bytes(pStmt, 0);
		std::string sResult(reinterpret_cast<const char*>(pText), static_cast<std::size_t>(nSize));
		::sqlite3_finalize(pStmt);
		return(sResult);
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
}

// 대응 원본: tests/integration/test_repositories.py::test_document_crud
// (repositories.py 의 create/get/list/update/delete_document 와 _document_from_row :827~836).
TEST_CASE("문서는 행 매퍼를 거쳐 값 그대로 돌아온다", "[core][storage][repositories]")
{
	C_REPO_FIXTURE Fixture("repo_document");
	const domain::S_DOCUMENT Document = make_document();
	REQUIRE(Fixture.Repo().CreateDocument(Document) == storage::E_REPO_RESULT::Ok);

	domain::S_DOCUMENT Fetched;
	REQUIRE(Fixture.Repo().GetDocument(Document.sId, &Fetched) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fetched == Document);

	// NULL 열이 "없음"으로 오는지가 매퍼의 계약이다. 0 으로 접히면 여기서 걸린다.
	REQUIRE_FALSE(Fetched.nArchivedAtUs.has_value());
	REQUIRE_FALSE(Fetched.nTrashedAtUs.has_value());

	domain::S_DOCUMENT Updated = Document;
	Updated.sTitle        = u8s(u8"바뀐 제목");
	Updated.nUpdatedAtUs  = 2000;
	Updated.nArchivedAtUs = 2000;
	REQUIRE(Fixture.Repo().UpdateDocument(Updated) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fixture.Repo().GetDocument(Document.sId, &Fetched) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fetched == Updated);
	REQUIRE(Fetched.nArchivedAtUs.has_value());

	std::vector<domain::S_DOCUMENT> Documents;
	REQUIRE(Fixture.Repo().ListDocuments(&Documents) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Documents.size() == 1);
	REQUIRE(Documents[0] == Updated);

	REQUIRE(Fixture.Repo().DeleteDocument(Document.sId) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fixture.Repo().GetDocument(Document.sId, &Fetched) == storage::E_REPO_RESULT::NotFound);
}

// 대응 원본: repositories.py 의 touch_document (:291~300) 의 MAX(updated_at_us, ?).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("touch_document 는 시각을 앞으로만 옮긴다", "[core][storage][repositories]")
{
	C_REPO_FIXTURE Fixture("repo_touch");
	domain::S_DOCUMENT Document = make_document();
	Document.nUpdatedAtUs = 5000;
	REQUIRE(Fixture.Repo().CreateDocument(Document) == storage::E_REPO_RESULT::Ok);

	REQUIRE(Fixture.Repo().TouchDocument(Document.sId, 9000) == storage::E_REPO_RESULT::Ok);
	domain::S_DOCUMENT Fetched;
	REQUIRE(Fixture.Repo().GetDocument(Document.sId, &Fetched) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fetched.nUpdatedAtUs == 9000);

	// 더 이른 시각은 무시된다 - MAX 를 그냥 대입으로 바꾸면 여기서 걸린다.
	REQUIRE(Fixture.Repo().TouchDocument(Document.sId, 1000) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fixture.Repo().GetDocument(Document.sId, &Fetched) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fetched.nUpdatedAtUs == 9000);
}

// 대응 원본: tests/integration/test_repositories.py::test_split_input_preserves_original_text_once
// (repositories.py 의 create/get/update/delete_capture_operation 와 _capture_operation_from_row :838~849).
TEST_CASE("입력 작업은 행 매퍼를 거쳐 값 그대로 돌아온다", "[core][storage][repositories]")
{
	C_REPO_FIXTURE Fixture("repo_operation");
	REQUIRE(Fixture.Repo().CreateDocument(make_document()) == storage::E_REPO_RESULT::Ok);

	domain::S_CAPTURE_OPERATION Operation;
	Operation.sId                   = "operation-1";
	Operation.sDocumentId           = "document-1";
	Operation.eSource               = domain::E_CAPTURE_OPERATION_SOURCE::Paste;
	Operation.eSplitPolicy          = domain::E_SPLIT_POLICY::SplitByBlankLine;
	Operation.sOriginalText         = u8s(u8"첫 문단\n\n두 번째 문단");
	Operation.sOriginalHash         = storage::TextHash(*Operation.sOriginalText);
	Operation.nOriginalRedactedAtUs = std::nullopt;
	Operation.nCreatedAtUs          = 2000;
	REQUIRE(Fixture.Repo().CreateCaptureOperation(Operation) == storage::E_REPO_RESULT::Ok);

	domain::S_CAPTURE_OPERATION Fetched;
	REQUIRE(Fixture.Repo().GetCaptureOperation(Operation.sId, &Fetched) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fetched == Operation);
	REQUIRE_FALSE(Fetched.nOriginalRedactedAtUs.has_value());

	// purge 가 원문을 지우는 모양. 텍스트와 해시가 함께 NULL 이 된다.
	domain::S_CAPTURE_OPERATION Redacted = Operation;
	Redacted.sOriginalText         = std::nullopt;
	Redacted.sOriginalHash         = std::nullopt;
	Redacted.nOriginalRedactedAtUs = 3000;
	REQUIRE(Fixture.Repo().UpdateCaptureOperation(Redacted) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fixture.Repo().GetCaptureOperation(Operation.sId, &Fetched) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fetched == Redacted);
	REQUIRE_FALSE(Fetched.sOriginalText.has_value());
	REQUIRE_FALSE(Fetched.sOriginalHash.has_value());

	REQUIRE(Fixture.Repo().DeleteCaptureOperation(Operation.sId) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fixture.Repo().GetCaptureOperation(Operation.sId, &Fetched) == storage::E_REPO_RESULT::NotFound);
}

// 대응 원본: tests/integration/test_repositories.py::test_multiline_unicode_round_trip
// (repositories.py 의 _card_from_row :851~866).
TEST_CASE("카드는 여러 줄 유니코드까지 값 그대로 돌아온다", "[core][storage][repositories]")
{
	C_REPO_FIXTURE Fixture("repo_card");
	REQUIRE(Fixture.Repo().CreateDocument(make_document()) == storage::E_REPO_RESULT::Ok);

	// 보조 평면 문자와 emoji 는 CP949 밖이라 u8 로 쓴다.
	const std::string sBody = u8s(u8"첫 줄\n\n한글과 emoji \U0001F9ED\n보조 평면 문자: \U0002000B\n마지막 빈 줄\n");

	std::vector<domain::S_CARD> Created;
	REQUIRE(Fixture.Repo().CreateCards(make_operation("operation-1"), { make_new_card(1, sBody) }, &Created)
		== storage::E_REPO_RESULT::Ok);
	REQUIRE(Created.size() == 1);

	domain::S_CARD Fetched;
	REQUIRE(Fixture.Repo().GetCard("card-1", &Fetched) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fetched == Created[0]);
	REQUIRE(Fetched.sBody == sBody);
	REQUIRE(Fetched.sBodyHash == storage::TextHash(sBody));
	REQUIRE_FALSE(Fetched.nDeletedAtUs.has_value());

	domain::S_CARD_REVISION Revision;
	REQUIRE(Fixture.Repo().GetRevision("revision-1", &Revision) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Revision.sBody == sBody);
	REQUIRE(Revision.sBodyHash == Fetched.sBodyHash);
	REQUIRE(Fetched.sCurrentRevisionId.has_value());
	REQUIRE(*Fetched.sCurrentRevisionId == Revision.sId);

	std::vector<domain::S_CARD> Cards;
	REQUIRE(Fixture.Repo().ListCards("document-1", &Cards) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Cards.size() == 1);
	REQUIRE(Cards[0] == Fetched);

	REQUIRE(Fixture.Repo().GetCard("card-missing", &Fetched) == storage::E_REPO_RESULT::NotFound);
}

// 대응 원본: repositories.py 의 create/get/list_revisions 와 _revision_from_row (:868~879).
// 파이썬 시험 트리에 매퍼 단독 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("리비전은 행 매퍼를 거쳐 값 그대로 돌아오고 이력은 event_seq 순이다", "[core][storage][repositories]")
{
	C_REPO_FIXTURE Fixture("repo_revision");
	REQUIRE(Fixture.Repo().CreateDocument(make_document()) == storage::E_REPO_RESULT::Ok);

	std::vector<domain::S_CARD> Created;
	REQUIRE(Fixture.Repo().CreateCards(make_operation("operation-1"), { make_new_card(1, u8s(u8"첫 카드")) }, &Created)
		== storage::E_REPO_RESULT::Ok);

	domain::S_CARD_REVISION First;
	REQUIRE(Fixture.Repo().GetRevision("revision-1", &First) == storage::E_REPO_RESULT::Ok);
	REQUIRE_FALSE(First.sParentRevisionId.has_value());
	REQUIRE(First.nEventSeq == 1);
	REQUIRE(First.eSource == domain::E_REVISION_SOURCE::Edit);

	domain::S_EDIT_EVENT NewEvent;
	NewEvent.sEventId      = "event-update";
	NewEvent.sDocumentId   = "document-1";
	NewEvent.sCardId       = "card-1";
	NewEvent.eEventType    = domain::E_EVENT_TYPE::Update;
	NewEvent.eSource       = domain::E_EVENT_SOURCE::Edit;
	NewEvent.nOccurredAtUs = 3000;
	NewEvent.sDetailsJson  = "{}";
	domain::S_EDIT_EVENT StoredEvent;
	REQUIRE(Fixture.Repo().CreateEvent(NewEvent, &StoredEvent) == storage::E_REPO_RESULT::Ok);

	const std::string sBody = u8s(u8"고친 본문");
	domain::S_CARD_REVISION Second;
	Second.sId               = "revision-2";
	Second.sCardId           = "card-1";
	Second.nEventSeq         = *StoredEvent.nEventSeq;
	Second.sParentRevisionId = "revision-1";
	Second.sBody             = sBody;
	Second.sBodyHash         = storage::TextHash(sBody);
	Second.eSource           = domain::E_REVISION_SOURCE::Restore;
	Second.nCreatedAtUs      = 3000;
	REQUIRE(Fixture.Repo().CreateRevision(Second) == storage::E_REPO_RESULT::Ok);

	domain::S_CARD_REVISION Fetched;
	REQUIRE(Fixture.Repo().GetRevision("revision-2", &Fetched) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fetched == Second);
	REQUIRE(Fetched.sParentRevisionId.has_value());

	std::vector<domain::S_CARD_REVISION> Revisions;
	REQUIRE(Fixture.Repo().ListRevisions("card-1", &Revisions) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Revisions.size() == 2);
	REQUIRE(Revisions[0] == First);
	REQUIRE(Revisions[1] == Second);

	REQUIRE(Fixture.Repo().DeleteRevisionForPurge("revision-2") == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fixture.Repo().GetRevision("revision-2", &Fetched) == storage::E_REPO_RESULT::NotFound);
}

// 대응 원본: tests/integration/test_repositories.py::test_partial_unique_indexes_enforce_active_card_and_draft_rules
// (repositories.py 의 초안 네 연산과 _draft_from_row :881~893).
TEST_CASE("초안은 행 매퍼를 거쳐 값 그대로 돌아온다", "[core][storage][repositories]")
{
	C_REPO_FIXTURE Fixture("repo_draft");
	REQUIRE(Fixture.Repo().CreateDocument(make_document()) == storage::E_REPO_RESULT::Ok);
	std::vector<domain::S_CARD> Created;
	REQUIRE(Fixture.Repo().CreateCards(make_operation("operation-1"), { make_new_card(1, u8s(u8"첫 카드")) }, &Created)
		== storage::E_REPO_RESULT::Ok);

	const std::string sText = u8s(u8"편집 중");
	domain::S_DRAFT EditDraft;
	EditDraft.sId                  = "draft-1";
	EditDraft.sDocumentId          = "document-1";
	EditDraft.sCardId              = "card-1";
	EditDraft.eDraftKind           = domain::E_DRAFT_KIND::Edit;
	EditDraft.sBaseRevisionId      = "revision-1";
	EditDraft.sDraftText           = sText;
	EditDraft.sDraftHash           = storage::TextHash(sText);
	EditDraft.nCursorPositionQchar = 4;
	EditDraft.nUpdatedAtUs         = 4000;
	REQUIRE(Fixture.Repo().CreateDraft(EditDraft) == storage::E_REPO_RESULT::Ok);

	// 새 카드 초안은 card_id 와 base_revision_id 가 둘 다 NULL 이다.
	domain::S_DRAFT NewDraft = EditDraft;
	NewDraft.sId              = "draft-2";
	NewDraft.sCardId          = std::nullopt;
	NewDraft.eDraftKind       = domain::E_DRAFT_KIND::New;
	NewDraft.sBaseRevisionId  = std::nullopt;
	NewDraft.nUpdatedAtUs     = 5000;
	REQUIRE(Fixture.Repo().CreateDraft(NewDraft) == storage::E_REPO_RESULT::Ok);

	domain::S_DRAFT Fetched;
	REQUIRE(Fixture.Repo().GetDraft("draft-1", &Fetched) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fetched == EditDraft);
	REQUIRE(Fixture.Repo().GetDraft("draft-2", &Fetched) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fetched == NewDraft);
	REQUIRE_FALSE(Fetched.sCardId.has_value());
	REQUIRE_FALSE(Fetched.sBaseRevisionId.has_value());

	std::vector<domain::S_DRAFT> Drafts;
	REQUIRE(Fixture.Repo().ListDrafts("document-1", &Drafts) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Drafts.size() == 2);
	REQUIRE(Drafts[0] == EditDraft);
	REQUIRE(Drafts[1] == NewDraft);

	domain::S_DRAFT Updated = EditDraft;
	Updated.sDraftText           = u8s(u8"더 편집 중");
	Updated.sDraftHash           = storage::TextHash(Updated.sDraftText);
	Updated.nCursorPositionQchar = 6;
	Updated.nUpdatedAtUs         = 6000;
	REQUIRE(Fixture.Repo().UpdateDraft(Updated) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fixture.Repo().GetDraft("draft-1", &Fetched) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fetched == Updated);

	REQUIRE(Fixture.Repo().DeleteDraft("draft-1") == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fixture.Repo().GetDraft("draft-1", &Fetched) == storage::E_REPO_RESULT::NotFound);
}

// 대응 원본: repositories.py 의 create_event(:595~629), list_events(:638~648) 와
// _event_from_row(:895~907). 이벤트 순서는 자동 증가 event_seq 다.
// 파이썬 시험 트리에 이 계약만 보는 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("이벤트는 발급된 순번을 달고 돌아오고 이력은 오름차순이다", "[core][storage][repositories]")
{
	C_REPO_FIXTURE Fixture("repo_event");
	REQUIRE(Fixture.Repo().CreateDocument(make_document()) == storage::E_REPO_RESULT::Ok);

	domain::S_EDIT_EVENT NewEvent;
	NewEvent.sEventId      = "event-1";
	NewEvent.sDocumentId   = "document-1";
	NewEvent.eEventType    = domain::E_EVENT_TYPE::Move;
	NewEvent.eSource       = domain::E_EVENT_SOURCE::System;
	NewEvent.nOccurredAtUs = 3000;
	NewEvent.sDetailsJson  = u8s(u8"{\"메모\":\"값\"}");

	domain::S_EDIT_EVENT First;
	REQUIRE(Fixture.Repo().CreateEvent(NewEvent, &First) == storage::E_REPO_RESULT::Ok);
	REQUIRE(First.nEventSeq.has_value());
	REQUIRE(*First.nEventSeq == 1);

	// operation_id 와 card_id 는 NULL 로 남는다.
	REQUIRE_FALSE(First.sOperationId.has_value());
	REQUIRE_FALSE(First.sCardId.has_value());

	domain::S_EDIT_EVENT Fetched;
	REQUIRE(Fixture.Repo().GetEvent(*First.nEventSeq, &Fetched) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fetched == First);
	REQUIRE(Fetched.sDetailsJson == NewEvent.sDetailsJson);

	domain::S_EDIT_EVENT SecondInput = NewEvent;
	SecondInput.sEventId      = "event-2";
	SecondInput.nOccurredAtUs = 1000;   // 발생 시각이 더 이르지만 순번은 뒤다.
	domain::S_EDIT_EVENT Second;
	REQUIRE(Fixture.Repo().CreateEvent(SecondInput, &Second) == storage::E_REPO_RESULT::Ok);
	REQUIRE(*Second.nEventSeq == 2);

	std::vector<domain::S_EDIT_EVENT> Events;
	REQUIRE(Fixture.Repo().ListEvents("document-1", &Events) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Events.size() == 2);
	REQUIRE(Events[0] == First);
	REQUIRE(Events[1] == Second);

	// 순번을 실어 보내면 거절이다(:596~597).
	domain::S_EDIT_EVENT Preassigned = NewEvent;
	Preassigned.sEventId  = "event-3";
	Preassigned.nEventSeq = 99;
	domain::S_EDIT_EVENT Ignored;
	REQUIRE(Fixture.Repo().CreateEvent(Preassigned, &Ignored) == storage::E_REPO_RESULT::Invalid);
	REQUIRE(Fixture.Db().LastError() == "새 이벤트의 event_seq는 SQLite가 발급해야 합니다.");

	REQUIRE(Fixture.Repo().DeleteEventForPurge(2) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fixture.Repo().GetEvent(2, &Fetched) == storage::E_REPO_RESULT::NotFound);
}

// 대응 원본: repositories.py 의 create/list/delete_lineage 와 _lineage_from_row (:909~916).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("계보는 행 매퍼를 거쳐 값 그대로 돌아온다", "[core][storage][repositories]")
{
	C_REPO_FIXTURE Fixture("repo_lineage");
	REQUIRE(Fixture.Repo().CreateDocument(make_document()) == storage::E_REPO_RESULT::Ok);
	std::vector<domain::S_CARD> Created;
	REQUIRE(Fixture.Repo().CreateCards(
		make_operation("operation-1"),
		{ make_new_card(1, u8s(u8"첫 카드")), make_new_card(2, u8s(u8"둘째 카드")) },
		&Created) == storage::E_REPO_RESULT::Ok);

	domain::S_CARD_LINEAGE Lineage;
	Lineage.sParentCardId = "card-1";
	Lineage.sChildCardId  = "card-2";
	Lineage.nEventSeq     = 2;
	Lineage.eRelationType = domain::E_LINEAGE_RELATION_TYPE::Split;
	REQUIRE(Fixture.Repo().CreateLineage(Lineage) == storage::E_REPO_RESULT::Ok);

	std::vector<domain::S_CARD_LINEAGE> Rows;
	REQUIRE(Fixture.Repo().ListLineageForCard("card-1", &Rows) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Rows.size() == 1);
	REQUIRE(Rows[0] == Lineage);

	// 자식 쪽에서도 같은 행이 보인다(WHERE parent = ? OR child = ?).
	REQUIRE(Fixture.Repo().ListLineageForCard("card-2", &Rows) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Rows.size() == 1);
	REQUIRE(Rows[0] == Lineage);

	// 계약 대장 §4-19: 삭제는 PK 3열로만 한다. relation_type 을 WHERE 에 더하면 값이 다른
	// 호출에서 삭제가 조용히 0행이 되고, 영향 행수를 안 보므로 오류조차 나지 않는다.
	domain::S_CARD_LINEAGE OtherRelation = Lineage;
	OtherRelation.eRelationType = domain::E_LINEAGE_RELATION_TYPE::Merge;
	REQUIRE(Fixture.Repo().DeleteLineage(OtherRelation) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fixture.Repo().ListLineageForCard("card-1", &Rows) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Rows.empty());

	// 없는 행을 지우는 것도 조용한 no-op 이다(영향 행수 미확인).
	REQUIRE(Fixture.Repo().DeleteLineage(Lineage) == storage::E_REPO_RESULT::Ok);
}

// 대응 원본: repositories.py 의 창 상태 네 연산(:61~156). 저장 순서는 updated_at_us, window_id 다.
// 파이썬 시험 트리에 저장소 단위 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("창 상태는 탭 JSON 계약을 지키며 저장 순서대로 읽힌다", "[core][storage][repositories]")
{
	C_REPO_FIXTURE Fixture("repo_workspace");
	REQUIRE(Fixture.Repo().CreateDocument(make_document()) == storage::E_REPO_RESULT::Ok);
	domain::S_DOCUMENT Second = make_document();
	Second.sId = "document-2";
	REQUIRE(Fixture.Repo().CreateDocument(Second) == storage::E_REPO_RESULT::Ok);

	domain::S_WORKSPACE_WINDOW Saved;
	REQUIRE(Fixture.Repo().SaveWorkspaceWindow("window-1", { "document-1", "document-2" }, "document-2", &Saved)
		== storage::E_REPO_RESULT::Ok);
	REQUIRE(Saved.sWindowId == "window-1");
	REQUIRE(Saved.OpenDocumentIds.size() == 2);
	REQUIRE(Saved.sActiveDocumentId.has_value());
	REQUIRE(Saved.nUpdatedAtUs > 0);

	domain::S_WORKSPACE_WINDOW Fetched;
	REQUIRE(Fixture.Repo().GetWorkspaceWindow("window-1", &Fetched) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fetched == Saved);

	// 활성 문서 없는 창도 성립한다.
	domain::S_WORKSPACE_WINDOW Empty;
	REQUIRE(Fixture.Repo().SaveWorkspaceWindow("window-2", {}, std::nullopt, &Empty) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Empty.OpenDocumentIds.empty());
	REQUIRE(Fixture.Repo().GetWorkspaceWindow("window-2", &Fetched) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fetched == Empty);
	REQUIRE_FALSE(Fetched.sActiveDocumentId.has_value());

	std::vector<domain::S_WORKSPACE_WINDOW> Windows;
	REQUIRE(Fixture.Repo().ListWorkspaceWindows(&Windows) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Windows.size() == 2);

	// 같은 창을 다시 저장하면 갱신이다(ON CONFLICT DO UPDATE).
	REQUIRE(Fixture.Repo().SaveWorkspaceWindow("window-1", { "document-1" }, "document-1", &Saved)
		== storage::E_REPO_RESULT::Ok);
	REQUIRE(Fixture.Repo().ListWorkspaceWindows(&Windows) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Windows.size() == 2);
	REQUIRE(Fixture.Repo().GetWorkspaceWindow("window-1", &Fetched) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fetched.OpenDocumentIds.size() == 1);

	REQUIRE(Fixture.Repo().DeleteWorkspaceWindow("window-1") == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fixture.Repo().GetWorkspaceWindow("window-1", &Fetched) == storage::E_REPO_RESULT::NotFound);
}

// 대응 원본: repositories.py 의 save_workspace_window 인코딩(:100~104) 과
// list_workspace_windows 정렬(:63~70). 인코딩은 json.dumps(ensure_ascii=False,
// separators=(",", ":")) 이고 정렬은 updated_at_us 다음 window_id 다.
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("탭 JSON 은 공백 없이 그대로 저장되고 창은 시각 다음 id 로 정렬된다", "[core][storage][repositories]")
{
	C_REPO_FIXTURE Fixture("repo_workspace_encode");
	REQUIRE(Fixture.Repo().CreateDocument(make_document()) == storage::E_REPO_RESULT::Ok);
	domain::S_DOCUMENT Second = make_document();
	Second.sId = "document-2";
	REQUIRE(Fixture.Repo().CreateDocument(Second) == storage::E_REPO_RESULT::Ok);
	domain::S_DOCUMENT Korean = make_document();
	Korean.sId = u8s(u8"문서-3");
	REQUIRE(Fixture.Repo().CreateDocument(Korean) == storage::E_REPO_RESULT::Ok);

	domain::S_WORKSPACE_WINDOW Saved;
	REQUIRE(Fixture.Repo().SaveWorkspaceWindow("window-b", { "document-1", "document-2" }, "document-1", &Saved)
		== storage::E_REPO_RESULT::Ok);

	// 구분자 사이에 공백이 없다. separators=(",", ":") 를 흘리면 여기서 걸린다.
	REQUIRE(read_text(Fixture.Db(),
		"SELECT open_document_ids_json FROM workspace_windows WHERE window_id = 'window-b'")
		== "[\"document-1\",\"document-2\"]");

	// ensure_ascii=False 라 비ASCII 는 이스케이프하지 않고 UTF-8 그대로 나간다.
	REQUIRE(Fixture.Repo().SaveWorkspaceWindow("window-a", { Korean.sId }, std::nullopt, &Saved)
		== storage::E_REPO_RESULT::Ok);
	REQUIRE(read_text(Fixture.Db(),
		"SELECT open_document_ids_json FROM workspace_windows WHERE window_id = 'window-a'")
		== "[\"" + u8s(u8"문서-3") + "\"]");

	// 빈 목록은 빈 배열이다.
	REQUIRE(Fixture.Repo().SaveWorkspaceWindow("window-c", {}, std::nullopt, &Saved) == storage::E_REPO_RESULT::Ok);
	REQUIRE(read_text(Fixture.Db(),
		"SELECT open_document_ids_json FROM workspace_windows WHERE window_id = 'window-c'") == "[]");

	// 정렬 확인을 위해 시각을 손으로 고정한다. window-a 와 window-b 는 같은 시각이라 id 로 갈린다.
	REQUIRE(Fixture.Db().Execute("UPDATE workspace_windows SET updated_at_us = 100 WHERE window_id = 'window-b'"));
	REQUIRE(Fixture.Db().Execute("UPDATE workspace_windows SET updated_at_us = 100 WHERE window_id = 'window-a'"));
	REQUIRE(Fixture.Db().Execute("UPDATE workspace_windows SET updated_at_us = 50 WHERE window_id = 'window-c'"));

	std::vector<domain::S_WORKSPACE_WINDOW> Windows;
	REQUIRE(Fixture.Repo().ListWorkspaceWindows(&Windows) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Windows.size() == 3);
	REQUIRE(Windows[0].sWindowId == "window-c");
	REQUIRE(Windows[1].sWindowId == "window-a");
	REQUIRE(Windows[2].sWindowId == "window-b");
	REQUIRE(Windows[1].OpenDocumentIds.size() == 1);
	REQUIRE(Windows[1].OpenDocumentIds[0] == Korean.sId);
}

// 대응 원본: repositories.py 의 save_workspace_window 세 검증(:93~98).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("창 상태 저장은 세 계약 위반을 사유와 함께 거절한다", "[core][storage][repositories]")
{
	C_REPO_FIXTURE Fixture("repo_workspace_reject");
	REQUIRE(Fixture.Repo().CreateDocument(make_document()) == storage::E_REPO_RESULT::Ok);

	domain::S_WORKSPACE_WINDOW Ignored;
	REQUIRE(Fixture.Repo().SaveWorkspaceWindow("", { "document-1" }, std::nullopt, &Ignored)
		== storage::E_REPO_RESULT::Invalid);
	REQUIRE(Fixture.Db().LastError() == "window_id는 비어 있을 수 없습니다.");

	REQUIRE(Fixture.Repo().SaveWorkspaceWindow("window-1", { "document-1", "document-1" }, std::nullopt, &Ignored)
		== storage::E_REPO_RESULT::Invalid);
	REQUIRE(Fixture.Db().LastError() == "한 창에는 같은 문서 탭을 두 번 저장할 수 없습니다.");

	REQUIRE(Fixture.Repo().SaveWorkspaceWindow("window-1", { "document-1" }, "document-2", &Ignored)
		== storage::E_REPO_RESULT::Invalid);
	REQUIRE(Fixture.Db().LastError() == "활성 문서는 해당 창의 열린 탭 목록에 있어야 합니다.");

	// 거절된 저장은 행을 남기지 않는다.
	std::vector<domain::S_WORKSPACE_WINDOW> Windows;
	REQUIRE(Fixture.Repo().ListWorkspaceWindows(&Windows) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Windows.empty());
}

// 대응 원본: repositories.py 의 _workspace_window_from_row(:135~156) 세 거절.
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("창 상태 매퍼는 깨진 탭 JSON 을 사유별로 거절한다", "[core][storage][repositories]")
{
	C_REPO_FIXTURE Fixture("repo_workspace_json");
	REQUIRE(Fixture.Repo().CreateDocument(make_document()) == storage::E_REPO_RESULT::Ok);

	const char* const pszInsert =
		"INSERT INTO workspace_windows(window_id, open_document_ids_json, active_document_id, updated_at_us)"
		" VALUES (?, ?, ?, 1)";

	struct S_CASE
	{
		const char* pszWindowId;
		const char* pszJson;
		bool        bActive;
		const char* pszExpected;
	};
	// 파싱 실패와 형식 위반은 원본에서 서로 다른 사유다. 한 갈래로 접으면 여기서 걸린다.
	const S_CASE Cases[] = {
		{ "window-broken", "[\"document-1\"",          false, "workspace_windows 탭 JSON을 읽지 못했습니다." },
		{ "window-object", "{\"a\":1}",                false, "workspace_windows의 열린 문서 목록 형식이 잘못되었습니다." },
		{ "window-number", "[1]",                      false, "workspace_windows의 열린 문서 목록 형식이 잘못되었습니다." },
		{ "window-dup",    "[\"a\",\"a\"]",            false, "workspace_windows의 열린 문서 목록 형식이 잘못되었습니다." },
		{ "window-active", "[]",                       true,  "workspace_windows의 활성 문서가 열린 탭에 없습니다." },
	};

	for (const S_CASE& Case : Cases)
	{
		INFO("창: " << Case.pszWindowId);
		sqlite3_stmt* pStmt = nullptr;
		REQUIRE(::sqlite3_prepare_v2(Fixture.Db().Handle(), pszInsert, -1, &pStmt, nullptr) == SQLITE_OK);
		REQUIRE(::sqlite3_bind_text(pStmt, 1, Case.pszWindowId, -1, SQLITE_TRANSIENT) == SQLITE_OK);
		REQUIRE(::sqlite3_bind_text(pStmt, 2, Case.pszJson, -1, SQLITE_TRANSIENT) == SQLITE_OK);
		if (Case.bActive) { REQUIRE(::sqlite3_bind_text(pStmt, 3, "document-1", -1, SQLITE_TRANSIENT) == SQLITE_OK); }
		else              { REQUIRE(::sqlite3_bind_null(pStmt, 3) == SQLITE_OK); }
		REQUIRE(::sqlite3_step(pStmt) == SQLITE_DONE);
		::sqlite3_finalize(pStmt);

		domain::S_WORKSPACE_WINDOW Ignored;
		REQUIRE(Fixture.Repo().GetWorkspaceWindow(Case.pszWindowId, &Ignored) == storage::E_REPO_RESULT::Invalid);
		REQUIRE(Fixture.Db().LastError() == Case.pszExpected);

		REQUIRE(Fixture.Db().Execute(
			std::string("DELETE FROM workspace_windows WHERE window_id = '") + Case.pszWindowId + "'"));
	}

	// 계약 대장 §4-13: 나쁜 행 하나가 목록 조회 전체를 실패시킨다. 건너뛰고 나머지를
	// 돌려주면 사용자에게는 "창이 조용히 사라진" 것으로 보인다.
	domain::S_WORKSPACE_WINDOW Saved;
	REQUIRE(Fixture.Repo().SaveWorkspaceWindow("window-good", { "document-1" }, "document-1", &Saved)
		== storage::E_REPO_RESULT::Ok);
	REQUIRE(Fixture.Db().Execute(
		"INSERT INTO workspace_windows(window_id, open_document_ids_json, active_document_id, updated_at_us)"
		" VALUES ('window-bad', '[\"a\",\"a\"]', NULL, 1)"));

	std::vector<domain::S_WORKSPACE_WINDOW> Windows;
	REQUIRE(Fixture.Repo().ListWorkspaceWindows(&Windows) == storage::E_REPO_RESULT::Invalid);
	REQUIRE(Fixture.Db().LastError() == "workspace_windows의 열린 문서 목록 형식이 잘못되었습니다.");

	// 나쁜 행을 치우면 다시 성립한다 - 실패가 상태를 망가뜨리지는 않는다.
	REQUIRE(Fixture.Db().Execute("DELETE FROM workspace_windows WHERE window_id = 'window-bad'"));
	REQUIRE(Fixture.Repo().ListWorkspaceWindows(&Windows) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Windows.size() == 1);
}

// 대응 원본: repositories.py 의 text_hash(:30~32). 기대값은 파이썬 hashlib.sha256 실측이다.
// 파이썬 시험 트리에 다이제스트 자체를 고정하는 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("text_hash 는 UTF-8 SHA-256 소문자 16진수다", "[core][storage][repositories]")
{
	REQUIRE(storage::TextHash("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	REQUIRE(storage::TextHash("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

	// 채움 경계 셋. 55/56 바이트에서 블록 하나가 더 붙는다.
	REQUIRE(storage::TextHash(std::string(55, 'a')) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
	REQUIRE(storage::TextHash(std::string(56, 'a')) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
	REQUIRE(storage::TextHash(std::string(64, 'a')) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");

	REQUIRE(storage::TextHash(u8s(u8"첫 카드")) == "f0f533d7db1065d9e26144f27673058c6c0e3a3032cf763092c007b671825354");
	REQUIRE(storage::TextHash(u8s(u8"첫 줄\n\n한글과 emoji \U0001F9ED\n보조 평면 문자: \U0002000B\n마지막 빈 줄\n"))
		== "b88e90fe2ab4735e42b1e1ab2f0fabed6629a52e9a3dd13669004b1469ba68a6");
}

// 대응 원본: repositories.py 의 search_documents(:190~216) 와 search_cards(:218~255).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("검색은 앞뒤 공백을 걷어내고 LIKE 특수문자를 이스케이프한다", "[core][storage][repositories]")
{
	C_REPO_FIXTURE Fixture("repo_search");
	REQUIRE(Fixture.Repo().CreateDocument(make_document()) == storage::E_REPO_RESULT::Ok);

	domain::S_DOCUMENT Percent = make_document();
	Percent.sId          = "document-2";
	Percent.sTitle       = u8s(u8"100% 확실");
	Percent.nUpdatedAtUs = 500;
	REQUIRE(Fixture.Repo().CreateDocument(Percent) == storage::E_REPO_RESULT::Ok);

	std::vector<domain::S_CARD> Created;
	REQUIRE(Fixture.Repo().CreateCards(
		make_operation("operation-1"),
		{ make_new_card(1, u8s(u8"검색 대상 본문")) },
		&Created) == storage::E_REPO_RESULT::Ok);

	std::vector<domain::S_DOCUMENT> Documents;
	REQUIRE(Fixture.Repo().SearchDocuments(u8s(u8"테스트"), &Documents) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Documents.size() == 1);
	REQUIRE(Documents[0].sId == "document-1");

	// 빈 질의는 빈 결과다. 유니코드 공백만 있는 질의도 마찬가지다 - 파이썬 strip 은 ASCII 공백만
	// 걷어내지 않는다. U+3000 과 U+00A0 을 UTF-8 바이트로 직접 쓰는 이유는 U+00A0 이 CP949 밖이라
	// 소스에 글자로 넣으면 좁은 리터럴 변환에서 걸리기 때문이다(spec_TR1 §4).
	const std::string sUnicodeSpaces = "\xE3\x80\x80\xC2\xA0";
	REQUIRE(Fixture.Repo().SearchDocuments("", &Documents) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Documents.empty());
	REQUIRE(Fixture.Repo().SearchDocuments(sUnicodeSpaces + " \t", &Documents) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Documents.empty());

	// 앞뒤 유니코드 공백은 걷어내고 가운데는 남긴다.
	REQUIRE(Fixture.Repo().SearchDocuments(sUnicodeSpaces + u8s(u8"테스트 문서") + sUnicodeSpaces, &Documents)
		== storage::E_REPO_RESULT::Ok);
	REQUIRE(Documents.size() == 1);

	// LIKE 메타문자는 이스케이프된다 - '%' 질의가 전건을 긁어오면 여기서 걸린다.
	REQUIRE(Fixture.Repo().SearchDocuments("%", &Documents) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Documents.size() == 1);
	REQUIRE(Documents[0].sId == "document-2");

	// 본문 일치로도 문서가 잡힌다.
	REQUIRE(Fixture.Repo().SearchDocuments(u8s(u8"검색 대상"), &Documents) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Documents.size() == 1);
	REQUIRE(Documents[0].sId == "document-1");

	std::vector<domain::S_CARD> Cards;
	REQUIRE(Fixture.Repo().SearchCards(u8s(u8"대상"), std::nullopt, &Cards) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Cards.size() == 1);
	REQUIRE(Cards[0].sId == "card-1");

	REQUIRE(Fixture.Repo().SearchCards(u8s(u8"대상"), std::optional<std::string>("document-1"), &Cards)
		== storage::E_REPO_RESULT::Ok);
	REQUIRE(Cards.size() == 1);
	REQUIRE(Fixture.Repo().SearchCards(u8s(u8"대상"), std::optional<std::string>("document-2"), &Cards)
		== storage::E_REPO_RESULT::Ok);
	REQUIRE(Cards.empty());
	REQUIRE(Fixture.Repo().SearchCards("  ", std::nullopt, &Cards) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Cards.empty());

	// 휴지통 문서는 검색에서 빠진다.
	domain::S_DOCUMENT Trashed = make_document();
	Trashed.nTrashedAtUs = 7000;
	REQUIRE(Fixture.Repo().UpdateDocument(Trashed) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fixture.Repo().SearchDocuments(u8s(u8"테스트"), &Documents) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Documents.empty());
	REQUIRE(Fixture.Repo().SearchCards(u8s(u8"대상"), std::nullopt, &Cards) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Cards.empty());

	// 계약 대장 §4-10·§5-1: 두 분기는 휴지통 정책이 다르다. 문서 지정 분기는 documents 와
	// 조인하지 않으므로 휴지통 문서의 카드도 그대로 돌려준다. 원본의 비대칭이고 MODE A 라
	// 고치지 않는다 - 두 분기를 한 문장으로 합치면 여기서 결과가 비어 버린다.
	REQUIRE(Fixture.Repo().SearchCards(u8s(u8"대상"), std::optional<std::string>("document-1"), &Cards)
		== storage::E_REPO_RESULT::Ok);
	REQUIRE(Cards.size() == 1);
	REQUIRE(Cards[0].sId == "card-1");
}

// 대응 원본: repositories.py 의 이스케이프 사슬(:195, :228) - "/" -> "//", "%" -> "/%",
// "_" -> "/_" 이고 ESCAPE 문자는 "/" 다. 계약 대장 §4-9: 순서를 뒤집으면 방금 넣은
// 이스케이프 접두사를 다시 이스케이프해 패턴이 깨진다.
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("LIKE 메타문자 세 종류가 모두 리터럴로 검색된다", "[core][storage][repositories]")
{
	C_REPO_FIXTURE Fixture("repo_like_escape");

	const char* const IDS[]    = { "document-1", "document-2", "document-3" };
	const char* const TITLES[] = { "a/b", "a_b", "axb" };
	for (int i = 0; i < 3; ++i)
	{
		domain::S_DOCUMENT Document = make_document();
		Document.sId    = IDS[i];
		Document.sTitle = TITLES[i];
		REQUIRE(Fixture.Repo().CreateDocument(Document) == storage::E_REPO_RESULT::Ok);
	}

	std::vector<domain::S_DOCUMENT> Documents;

	// "_" 를 이스케이프하지 않으면 임의 한 글자로 읽혀 "axb" 까지 걸린다.
	REQUIRE(Fixture.Repo().SearchDocuments("a_b", &Documents) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Documents.size() == 1);
	REQUIRE(Documents[0].sTitle == "a_b");

	// "/" 는 ESCAPE 문자 자신이라 두 배로 늘려야 리터럴이 된다.
	REQUIRE(Fixture.Repo().SearchDocuments("a/b", &Documents) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Documents.size() == 1);
	REQUIRE(Documents[0].sTitle == "a/b");

	// 이스케이프 순서가 뒤집히면 이 질의가 리터럴 "/" 를 찾게 되어 0건이 된다.
	REQUIRE(Fixture.Repo().SearchDocuments("a", &Documents) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Documents.size() == 3);
}

// 대응 원본: repositories.py 의 list_documents(:184~188) 와 list_cards(:393~403).
// 계약 대장 §4-20: 둘 다 필터가 없다. "당연히 활성만"이라는 가정으로 조건을 더하면
// 휴지통 화면과 purge 경로가 대상 행을 못 찾는다.
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("목록 조회는 보관·휴지통 행을 걸러내지 않는다", "[core][storage][repositories]")
{
	C_REPO_FIXTURE Fixture("repo_no_filter");
	REQUIRE(Fixture.Repo().CreateDocument(make_document()) == storage::E_REPO_RESULT::Ok);

	std::vector<domain::S_CARD> Created;
	REQUIRE(Fixture.Repo().CreateCards(
		make_operation("operation-1"),
		{ make_new_card(1, u8s(u8"첫 카드")), make_new_card(2, u8s(u8"둘째 카드")) },
		&Created) == storage::E_REPO_RESULT::Ok);

	REQUIRE(Fixture.Repo().UpdateCardDeletedState(
		Created[0].sId, Created[0].nPositionKey, 3000, *Created[0].sCurrentRevisionId)
		== storage::E_REPO_RESULT::Ok);

	std::vector<domain::S_CARD> Cards;
	REQUIRE(Fixture.Repo().ListCards("document-1", &Cards) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Cards.size() == 2);
	REQUIRE(Cards[0].nDeletedAtUs.has_value());

	// 삭제 표시된 카드는 단건 조회로도 그대로 나온다.
	domain::S_CARD Fetched;
	REQUIRE(Fixture.Repo().GetCard(Created[0].sId, &Fetched) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fetched.nDeletedAtUs.has_value());

	domain::S_DOCUMENT Archived = make_document();
	Archived.nArchivedAtUs = 4000;
	Archived.nTrashedAtUs  = 5000;
	REQUIRE(Fixture.Repo().UpdateDocument(Archived) == storage::E_REPO_RESULT::Ok);

	std::vector<domain::S_DOCUMENT> Documents;
	REQUIRE(Fixture.Repo().ListDocuments(&Documents) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Documents.size() == 1);
	REQUIRE(Documents[0].nArchivedAtUs.has_value());
	REQUIRE(Documents[0].nTrashedAtUs.has_value());
}

// 대응 원본: repositories.py 의 operation_reconstruction_available(:257~271).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("원문 재구성 가능 여부는 소거 시각으로 갈리고 없는 카드는 사유를 남긴다", "[core][storage][repositories]")
{
	C_REPO_FIXTURE Fixture("repo_reconstruct");
	REQUIRE(Fixture.Repo().CreateDocument(make_document()) == storage::E_REPO_RESULT::Ok);
	std::vector<domain::S_CARD> Created;
	REQUIRE(Fixture.Repo().CreateCards(make_operation("operation-1"), { make_new_card(1, u8s(u8"첫 카드")) }, &Created)
		== storage::E_REPO_RESULT::Ok);

	bool bAvailable = false;
	REQUIRE(Fixture.Repo().OperationReconstructionAvailable("card-1", &bAvailable) == storage::E_REPO_RESULT::Ok);
	REQUIRE(bAvailable);

	domain::S_CAPTURE_OPERATION Operation;
	REQUIRE(Fixture.Repo().GetCaptureOperation("operation-1", &Operation) == storage::E_REPO_RESULT::Ok);
	Operation.nOriginalRedactedAtUs = 9000;
	REQUIRE(Fixture.Repo().UpdateCaptureOperation(Operation) == storage::E_REPO_RESULT::Ok);

	REQUIRE(Fixture.Repo().OperationReconstructionAvailable("card-1", &bAvailable) == storage::E_REPO_RESULT::Ok);
	REQUIRE_FALSE(bAvailable);

	// 원본이 KeyError 를 올리는 유일한 자리다. 사유 문구를 그대로 남긴다.
	REQUIRE(Fixture.Repo().OperationReconstructionAvailable("card-9", &bAvailable) == storage::E_REPO_RESULT::NotFound);
	REQUIRE(Fixture.Db().LastError() == "존재하지 않는 카드입니다: card-9");
}

// 대응 원본: tests/integration/test_repositories.py::test_capture_counter_only_advances_for_successful_card_creation
// (repositories.py 의 get_counter :656~661 과 _issue_capture_sequence :663~677).
TEST_CASE("capture counter 는 카드 생성에서만 전진한다", "[core][storage][repositories]")
{
	C_REPO_FIXTURE Fixture("repo_counter");
	REQUIRE(Fixture.Repo().CreateDocument(make_document()) == storage::E_REPO_RESULT::Ok);

	std::int64_t nCounter = 0;
	REQUIRE(Fixture.Repo().GetCounter("capture", &nCounter) == storage::E_REPO_RESULT::Ok);
	REQUIRE(nCounter == 1);

	std::vector<domain::S_CARD> First;
	REQUIRE(Fixture.Repo().CreateCards(make_operation("operation-1"), { make_new_card(1, u8s(u8"첫 카드")) }, &First)
		== storage::E_REPO_RESULT::Ok);
	REQUIRE(First[0].nCaptureSeq == 1);
	REQUIRE(Fixture.Repo().GetCounter("capture", &nCounter) == storage::E_REPO_RESULT::Ok);
	REQUIRE(nCounter == 2);

	// 카드 생성이 아닌 이벤트는 counter 를 건드리지 않는다.
	domain::S_EDIT_EVENT NewEvent;
	NewEvent.sEventId      = "event-update";
	NewEvent.sDocumentId   = "document-1";
	NewEvent.sCardId       = "card-1";
	NewEvent.eEventType    = domain::E_EVENT_TYPE::Update;
	NewEvent.eSource       = domain::E_EVENT_SOURCE::Edit;
	NewEvent.nOccurredAtUs = 3000;
	NewEvent.sDetailsJson  = "{}";
	domain::S_EDIT_EVENT StoredEvent;
	REQUIRE(Fixture.Repo().CreateEvent(NewEvent, &StoredEvent) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fixture.Repo().GetCounter("capture", &nCounter) == storage::E_REPO_RESULT::Ok);
	REQUIRE(nCounter == 2);

	std::vector<domain::S_CARD> Second;
	REQUIRE(Fixture.Repo().CreateCards(make_operation("operation-2"), { make_new_card(2, u8s(u8"둘째 카드")) }, &Second)
		== storage::E_REPO_RESULT::Ok);
	REQUIRE(Second[0].nCaptureSeq == 2);
	REQUIRE(Fixture.Repo().GetCounter("capture", &nCounter) == storage::E_REPO_RESULT::Ok);
	REQUIRE(nCounter == 3);

	// counter 는 UPDATE ... RETURNING 로 발급되므로 스키마의 단조성 트리거가 그대로 산다.
	REQUIRE_FALSE(Fixture.Db().Execute("UPDATE counters SET next_value = 0 WHERE name = 'capture'"));
	REQUIRE_FALSE(Fixture.Db().Execute("DELETE FROM counters WHERE name = 'capture'"));
	REQUIRE(Fixture.Repo().GetCounter("capture", &nCounter) == storage::E_REPO_RESULT::Ok);
	REQUIRE(nCounter == 3);

	REQUIRE(Fixture.Repo().GetCounter("missing-counter", &nCounter) == storage::E_REPO_RESULT::NotFound);
	REQUIRE(count_rows(Fixture.Db(), "counters") == 1);
}

// 대응 원본: src/pynote/infrastructure/database.py 의 transaction() 중첩 거부(:57~58) 가
// 저장소의 트랜잭션 세 연산에서 보이는 모습이다.
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("트랜잭션을 여는 연산은 중첩을 사유와 함께 거절한다", "[core][storage][repositories]")
{
	C_REPO_FIXTURE Fixture("repo_nested_tx");
	REQUIRE(Fixture.Repo().CreateDocument(make_document()) == storage::E_REPO_RESULT::Ok);

	storage::C_TRANSACTION Outer(Fixture.Db());
	REQUIRE(Outer.IsActive());

	domain::S_WORKSPACE_WINDOW Ignored;
	REQUIRE(Fixture.Repo().SaveWorkspaceWindow("window-1", { "document-1" }, std::nullopt, &Ignored)
		== storage::E_REPO_RESULT::Failed);
	REQUIRE(Fixture.Db().LastError() == "중첩 트랜잭션은 지원하지 않습니다.");

	REQUIRE(Fixture.Repo().DeleteWorkspaceWindow("window-1") == storage::E_REPO_RESULT::Failed);
	REQUIRE(Fixture.Db().LastError() == "중첩 트랜잭션은 지원하지 않습니다.");

	std::vector<domain::S_CARD> Created;
	REQUIRE(Fixture.Repo().CreateCards(make_operation("operation-1"), { make_new_card(1, u8s(u8"첫 카드")) }, &Created)
		== storage::E_REPO_RESULT::Failed);
	REQUIRE(Fixture.Db().LastError() == "중첩 트랜잭션은 지원하지 않습니다.");

	// 바깥 트랜잭션은 살아 있어야 한다 - 거절이 남의 트랜잭션을 닫으면 원자성이 깨진다.
	REQUIRE(Outer.IsActive());
	REQUIRE(Outer.Commit());
}
