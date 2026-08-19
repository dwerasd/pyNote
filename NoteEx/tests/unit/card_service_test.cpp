#include <catch_amalgamated.hpp>

#include "pynote/core/application/card_service.h"
#include "pynote/core/domain/paragraph_parser.h"
#include "pynote/core/storage/migration_runner.h"
#include "pynote/core/storage/repositories.h"

#include <sqlite3/sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#pragma comment(lib, "NoteExCore")

namespace
{
	namespace app = pynote::core::application;
	namespace domain = pynote::core::domain;
	namespace storage = pynote::core::storage;

	std::string u8s(const char8_t* text) { return std::string(reinterpret_cast<const char*>(text)); }

	class Fixture
	{
	public:
		Fixture()
			: path_(std::filesystem::temp_directory_path()
				/ ("noteex_w2r2_card_" + std::to_string(::GetCurrentProcessId()) + "_" + std::to_string(++sequence_) + ".db")),
			  repositories_(database_)
		{
			remove_();
			REQUIRE(database_.Open(path_.string()));
			storage::C_MIGRATION_RUNNER runner;
			runner.SetExistingDatabase(false, path_.string());
			REQUIRE(runner.Run(database_) == storage::E_MIGRATE_RESULT::Ok);
			domain::S_DOCUMENT document;
			document.sId = "document-card-service"; document.sTitle = u8s(u8"카드 서비스 테스트");
			document.nCreatedAtUs = 1000; document.nUpdatedAtUs = 1000;
			REQUIRE(repositories_.CreateDocument(document) == storage::E_REPO_RESULT::Ok);
		}
		~Fixture() { database_.Close(); remove_(); }

		app::C_CARD_SERVICE Service(std::vector<std::int64_t> times = { 2000 })
		{
			auto clockState = std::make_shared<std::pair<std::vector<std::int64_t>, std::size_t>>(std::move(times), 0);
			auto idState = std::make_shared<int>(0);
			return app::C_CARD_SERVICE(database_, repositories_, parser_,
				[clockState] { const auto index = (std::min)(clockState->second++, clockState->first.size() - 1); return clockState->first[index]; },
				[idState] { return "id-" + std::to_string((*idState)++); });
		}
		std::int64_t Count(const char* table)
		{
			const std::string sql = std::string("SELECT COUNT(*) FROM ") + table;
			sqlite3_stmt* statement = nullptr;
			REQUIRE(::sqlite3_prepare_v2(database_.Handle(), sql.c_str(), -1, &statement, nullptr) == SQLITE_OK);
			REQUIRE(::sqlite3_step(statement) == SQLITE_ROW);
			const auto count = ::sqlite3_column_int64(statement, 0); ::sqlite3_finalize(statement); return count;
		}
		storage::C_DATABASE database_;
		storage::C_REPOSITORIES repositories_;
		domain::C_PARAGRAPH_PARSER parser_;
	private:
		void remove_() { std::error_code ec; std::filesystem::remove(path_, ec); std::filesystem::remove(path_.string()+"-wal",ec); std::filesystem::remove(path_.string()+"-shm",ec); }
		std::filesystem::path path_;
		inline static int sequence_ = 0;
	};

	void emit(std::string_view id, std::string_view line)
	{
		wchar_t path[32768] = {}; const DWORD length = ::GetEnvironmentVariableW(L"PYNOTE_CARD_IMPORT_GOLDEN_OUT", path, 32768);
		if (length == 0) { return; }
		REQUIRE(length < 32768);
		std::ofstream output(std::filesystem::path(path), std::ios::binary | std::ios::app);
		REQUIRE(output.is_open()); output << id << '|' << line << '\n'; REQUIRE(output.good());
	}

	domain::S_CARD create(app::C_CARD_SERVICE& service, const std::string& body,
		domain::E_CAPTURE_OPERATION_SOURCE source = domain::E_CAPTURE_OPERATION_SOURCE::Typing,
		std::optional<std::string> before = std::nullopt)
	{
		domain::S_CARD card;
		REQUIRE(service.CreateCard("document-card-service", body, source, before, &card) == app::E_CARD_SERVICE_RESULT::Ok);
		return card;
	}

	std::vector<std::string> bodies(const std::vector<domain::S_CARD>& cards)
	{
		std::vector<std::string> result; for (const auto& card : cards) { result.push_back(card.sBody); } return result;
	}
}

#define TAGS(ID) "[W2-R2][core][domain][card-service][" ID "]"

TEST_CASE("WTL-W2-0102", TAGS("WTL-W2-0102"))
{
	Fixture f; auto service = f.Service(); std::string body;
	for (int line=1; line<=60; ++line) { if(line>1) body+='\n'; body += std::to_string(line)+u8s(u8"번째 줄"); }
	const auto card=create(service,body,domain::E_CAPTURE_OPERATION_SOURCE::Paste);
	domain::S_CAPTURE_OPERATION operation; domain::S_CARD_REVISION revision;
	REQUIRE(f.repositories_.GetCaptureOperation(card.sOperationId,&operation)==storage::E_REPO_RESULT::Ok);
	REQUIRE_FALSE(operation.sOriginalText.has_value()); REQUIRE(card.sBody==body);
	REQUIRE(f.repositories_.GetRevision(*card.sCurrentRevisionId,&revision)==storage::E_REPO_RESULT::Ok); REQUIRE(revision.sBody==body);
	emit("WTL-W2-0102","cards=1|lines=60|original=none");
}

TEST_CASE("WTL-W2-0103", TAGS("WTL-W2-0103"))
{
	Fixture f; auto service=f.Service(); const std::string original=u8s(u8" 첫 문단\r\n한 줄 더\r\n\r\n  \r\n둘째 문단\n\n셋째 문단\n");
	std::vector<domain::S_CARD> cards; REQUIRE(service.CreateCards("document-card-service",original,domain::E_CAPTURE_OPERATION_SOURCE::Paste,true,std::nullopt,&cards)==app::E_CARD_SERVICE_RESULT::Ok);
	REQUIRE(bodies(cards)==std::vector<std::string>{u8s(u8" 첫 문단\n한 줄 더"),u8s(u8"둘째 문단"),u8s(u8"셋째 문단")});
	REQUIRE(cards[0].nCaptureSeq==1); REQUIRE(cards[1].nCaptureSeq==2); REQUIRE(cards[2].nCaptureSeq==3);
	domain::S_CAPTURE_OPERATION operation; REQUIRE(f.repositories_.GetCaptureOperation(cards[0].sOperationId,&operation)==storage::E_REPO_RESULT::Ok); REQUIRE(operation.sOriginalText==std::optional<std::string>(original));
	emit("WTL-W2-0103","cards=3|capture=1,2,3|original=exact");
}

TEST_CASE("WTL-W2-0104", TAGS("WTL-W2-0104"))
{
	Fixture f; auto service=f.Service(); const auto card=create(service,u8s(u8"직접 입력 뒤 붙여넣기"),domain::E_CAPTURE_OPERATION_SOURCE::Mixed);
	domain::S_CAPTURE_OPERATION operation; std::vector<domain::S_EDIT_EVENT> events;
	REQUIRE(f.repositories_.GetCaptureOperation(card.sOperationId,&operation)==storage::E_REPO_RESULT::Ok); REQUIRE(f.repositories_.ListEvents("document-card-service",&events)==storage::E_REPO_RESULT::Ok);
	REQUIRE(operation.eSource==domain::E_CAPTURE_OPERATION_SOURCE::Mixed); REQUIRE(card.eSource==domain::E_CARD_SOURCE::Mixed); REQUIRE(events[0].eSource==domain::E_EVENT_SOURCE::Mixed);
	emit("WTL-W2-0104","operation=mixed|card=mixed|event=mixed");
}

TEST_CASE("WTL-W2-0105", TAGS("WTL-W2-0105"))
{
	Fixture f; auto service=f.Service(); REQUIRE(f.database_.Execute(u8s(u8"CREATE TRIGGER fail_second BEFORE INSERT ON card_revisions WHEN NEW.body = '둘째 문단' BEGIN SELECT RAISE(ABORT, 'fail'); END")));
	std::vector<domain::S_CARD> cards; REQUIRE(service.CreateCards("document-card-service",u8s(u8"첫 문단\n\n둘째 문단"),domain::E_CAPTURE_OPERATION_SOURCE::Paste,true,std::nullopt,&cards)==app::E_CARD_SERVICE_RESULT::Failed);
	REQUIRE(f.Count("capture_operations")==0); REQUIRE(f.Count("cards")==0); REQUIRE(f.Count("edit_events")==0); REQUIRE(f.Count("card_revisions")==0);
	std::int64_t counter=0; REQUIRE(f.repositories_.GetCounter("capture",&counter)==storage::E_REPO_RESULT::Ok); REQUIRE(counter==1);
	emit("WTL-W2-0105","rows=0,0,0,0|counter=1");
}

TEST_CASE("WTL-W2-0106", TAGS("WTL-W2-0106"))
{
	Fixture f; auto service=f.Service({2000,2000}); auto card=create(service,u8s(u8"삭제 성공"));
	domain::S_DRAFT draft; draft.sId="draft-delete"; draft.sDocumentId="document-card-service"; draft.sCardId=card.sId; draft.eDraftKind=domain::E_DRAFT_KIND::Edit; draft.sBaseRevisionId=card.sCurrentRevisionId; draft.sDraftText="edit"; draft.sDraftHash=storage::TextHash("edit"); draft.nUpdatedAtUs=3000;
	REQUIRE(f.repositories_.CreateDraft(draft)==storage::E_REPO_RESULT::Ok); domain::S_CARD deleted;
	REQUIRE(service.SoftDelete(card.sId,card.sCurrentRevisionId,false,draft.sId,&deleted)==app::E_CARD_SERVICE_RESULT::Ok);
	domain::S_DRAFT fetched; REQUIRE(f.repositories_.GetDraft(draft.sId,&fetched)==storage::E_REPO_RESULT::NotFound); REQUIRE(deleted.nDeletedAtUs.has_value());
	emit("WTL-W2-0106","draft=deleted|card=soft-deleted");
}

TEST_CASE("WTL-W2-0107", TAGS("WTL-W2-0107"))
{
	Fixture f; auto service=f.Service(); auto a=create(service,"A"); auto b=create(service,"B"); auto c=create(service,"C",domain::E_CAPTURE_OPERATION_SOURCE::Typing,b.sId);
	std::vector<domain::S_CARD> ordered; REQUIRE(service.ListActiveCards("document-card-service",app::E_CARD_SORT_MODE::Position,&ordered)==app::E_CARD_SERVICE_RESULT::Ok);
	REQUIRE(bodies(ordered)==std::vector<std::string>{"A","C","B"}); REQUIRE((std::vector<std::int64_t>{ordered[0].nCaptureSeq,ordered[1].nCaptureSeq,ordered[2].nCaptureSeq}==std::vector<std::int64_t>{1,3,2})); REQUIRE(a.nPositionKey<c.nPositionKey); REQUIRE(c.nPositionKey<b.nPositionKey);
	emit("WTL-W2-0107","bodies=41,43,42|capture=1,3,2");
}

TEST_CASE("WTL-W2-0108", TAGS("WTL-W2-0108"))
{
	Fixture f; auto service=f.Service(); auto a=create(service,"A"); auto b=create(service,"B"); auto c=create(service,"C");
	std::vector<domain::S_CARD> recent,position; REQUIRE(service.ListActiveCards("document-card-service",app::E_CARD_SORT_MODE::Recency,&recent)==app::E_CARD_SERVICE_RESULT::Ok); REQUIRE(service.ListActiveCards("document-card-service",app::E_CARD_SORT_MODE::Position,&position)==app::E_CARD_SERVICE_RESULT::Ok);
	REQUIRE((std::vector<std::string>{recent[0].sId,recent[1].sId,recent[2].sId}==std::vector<std::string>{c.sId,b.sId,a.sId})); REQUIRE(bodies(position)==std::vector<std::string>{"A","B","C"});
	emit("WTL-W2-0108","recency=3,2,1|position=1,2,3");
}

TEST_CASE("WTL-W2-0109", TAGS("WTL-W2-0109"))
{
	Fixture f; auto service=f.Service(); auto a=create(service,"A"); auto b=create(service,"B"); REQUIRE(f.repositories_.UpdateCardPosition(a.sId,1,*a.sCurrentRevisionId)==storage::E_REPO_RESULT::Ok); REQUIRE(f.repositories_.UpdateCardPosition(b.sId,2,*b.sCurrentRevisionId)==storage::E_REPO_RESULT::Ok);
	auto c=create(service,"C",domain::E_CAPTURE_OPERATION_SOURCE::Typing,b.sId); std::vector<domain::S_CARD> ordered; REQUIRE(service.ListActiveCards("document-card-service",app::E_CARD_SORT_MODE::Position,&ordered)==app::E_CARD_SERVICE_RESULT::Ok); REQUIRE(bodies(ordered)==std::vector<std::string>{"A","C","B"}); REQUIRE(c.nCaptureSeq==3); REQUIRE(ordered[0].nPositionKey!=ordered[1].nPositionKey); REQUIRE(ordered[1].nPositionKey!=ordered[2].nPositionKey);
	emit("WTL-W2-0109","bodies=41,43,42|unique_positions=3|capture=3");
}

TEST_CASE("WTL-W2-0110", TAGS("WTL-W2-0110"))
{
	Fixture f; auto service=f.Service({2000,3000,4000,5000,6000}); auto a=create(service,"A"); auto b=create(service,"B"); auto c=create(service,"C"); domain::S_CARD moved,deleted;
	REQUIRE(service.MoveCard(c.sId,b.sId,&moved)==app::E_CARD_SERVICE_RESULT::Ok); REQUIRE(service.SoftDelete(c.sId,std::nullopt,false,std::nullopt,&deleted)==app::E_CARD_SERVICE_RESULT::Ok); REQUIRE(moved.nCaptureSeq==c.nCaptureSeq); REQUIRE(deleted.nCaptureSeq==c.nCaptureSeq);
	std::vector<domain::S_CARD> active; REQUIRE(service.ListActiveCards("document-card-service",app::E_CARD_SORT_MODE::Position,&active)==app::E_CARD_SERVICE_RESULT::Ok); REQUIRE(bodies(active)==std::vector<std::string>{"A","B"}); std::vector<domain::S_EDIT_EVENT> events; REQUIRE(f.repositories_.ListEvents("document-card-service",&events)==storage::E_REPO_RESULT::Ok); REQUIRE(events.back().sDetailsJson.find(a.sId)!=std::string::npos); REQUIRE(events.back().sDetailsJson.find(b.sId)!=std::string::npos);
	emit("WTL-W2-0110","capture=3|active=41,42|neighbors=first,second");
}

TEST_CASE("WTL-W2-0111", TAGS("WTL-W2-0111"))
{
	Fixture f; auto service=f.Service({2000,3000}); auto card=create(service,u8s(u8"삭제 실패")); std::vector<domain::S_EDIT_EVENT> before; REQUIRE(f.repositories_.ListEvents("document-card-service",&before)==storage::E_REPO_RESULT::Ok);
	REQUIRE(f.database_.Execute("CREATE TRIGGER fail_delete BEFORE UPDATE OF deleted_at_us ON cards WHEN NEW.deleted_at_us IS NOT NULL BEGIN SELECT RAISE(ABORT, 'fail'); END")); domain::S_CARD deleted; REQUIRE(service.SoftDelete(card.sId,std::nullopt,false,std::nullopt,&deleted)==app::E_CARD_SERVICE_RESULT::Failed);
	domain::S_CARD stored; std::vector<domain::S_EDIT_EVENT> after; REQUIRE(f.repositories_.GetCard(card.sId,&stored)==storage::E_REPO_RESULT::Ok); REQUIRE_FALSE(stored.nDeletedAtUs.has_value()); REQUIRE(f.repositories_.ListEvents("document-card-service",&after)==storage::E_REPO_RESULT::Ok); REQUIRE(after==before);
	emit("WTL-W2-0111","deleted=0|events_unchanged=1");
}

TEST_CASE("WTL-W2-0112", TAGS("WTL-W2-0112"))
{
	Fixture f; auto service=f.Service({2000,3000}); auto card=create(service,u8s(u8"비어 있지 않은 본문")); std::vector<domain::S_EDIT_EVENT> before; REQUIRE(f.repositories_.ListEvents("document-card-service",&before)==storage::E_REPO_RESULT::Ok); domain::S_CARD deleted;
	REQUIRE(service.SoftDelete(card.sId,card.sCurrentRevisionId,true,std::nullopt,&deleted)==app::E_CARD_SERVICE_RESULT::Conflict); domain::S_CARD stored; REQUIRE(f.repositories_.GetCard(card.sId,&stored)==storage::E_REPO_RESULT::Ok); REQUIRE_FALSE(stored.nDeletedAtUs.has_value()); std::vector<domain::S_EDIT_EVENT> after; REQUIRE(f.repositories_.ListEvents("document-card-service",&after)==storage::E_REPO_RESULT::Ok); REQUIRE(after==before);
	emit("WTL-W2-0112","result=conflict|deleted=0|events_unchanged=1");
}

#undef TAGS
