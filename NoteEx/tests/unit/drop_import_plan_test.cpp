#include <catch_amalgamated.hpp>

#include "pynote/core/application/card_service.h"
#include "pynote/core/application/drop_import_plan.h"
#include "pynote/core/domain/events.h"
#include "pynote/core/domain/paragraph_parser.h"
#include "pynote/core/storage/migration_runner.h"
#include "pynote/core/storage/repositories.h"
#include "pynote/platform/win32_import_support.h"

#include <sqlite3/sqlite3.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
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
	namespace platform = pynote::platform;
	namespace storage = pynote::core::storage;

	std::string u8s(const char8_t* value) { return std::string(reinterpret_cast<const char*>(value)); }
	std::vector<std::uint8_t> bytes(std::string_view value) { return {value.begin(), value.end()}; }
	std::string hex(std::string_view value)
	{
		std::ostringstream output; output << std::hex << std::setfill('0');
		for (const unsigned char byte : value) { output << std::setw(2) << static_cast<unsigned int>(byte); }
		return output.str();
	}

	void emit(std::string_view id, std::string_view payload)
	{
		wchar_t path[32768] = {};
		const DWORD length = ::GetEnvironmentVariableW(L"PYNOTE_DROP_IMPORT_GOLDEN_OUT", path, 32768);
		if (length == 0 || length >= 32768) { return; }
		std::ofstream output(std::filesystem::path(path), std::ios::binary |
			(id == "WTL-W2-0054" ? std::ios::trunc : std::ios::app));
		REQUIRE(output.is_open()); output << id << '|' << payload << '\n'; REQUIRE(output.good());
	}

	app::S_DROP_IMPORT_CANDIDATE candidate(std::string path, std::string key = {})
	{
		if (key.empty()) { key = path; }
		return {std::move(path), std::move(key)};
	}

	struct Harness
	{
		domain::C_PARAGRAPH_PARSER parser;
		app::C_IMPORT_PIPELINE pipeline{parser, [](std::span<const std::uint8_t> input) { return platform::DecodeSystemAnsi(input); }};
		std::map<std::string, std::vector<std::uint8_t>> snapshots;
		std::map<std::string, app::E_DROP_PATH_KIND> kinds;
		std::set<std::string> readFailures;
		std::set<std::string> createFailures;
		std::vector<std::string> inspected;
		std::vector<std::string> read;
		std::vector<std::size_t> limits;
		std::vector<bool> gates;
		std::vector<std::string> createBodies;
		std::vector<domain::E_CAPTURE_OPERATION_SOURCE> createSources;
		std::vector<std::string> createDocuments;
		bool gateResult{true};

		app::C_DROP_IMPORT_COORDINATOR Coordinator()
		{
			return app::C_DROP_IMPORT_COORDINATOR(pipeline,
				[this](const auto& item) { inspected.push_back(item.sPath); const auto found=kinds.find(item.sPath); return found==kinds.end()?app::E_DROP_PATH_KIND::LocalRegularFile:found->second; },
				[this](const std::string& path, std::size_t limit, std::vector<std::uint8_t>* out, std::string* error) {
					read.push_back(path); limits.push_back(limit); if(readFailures.contains(path)){*error="read-failed";return false;} *out=snapshots[path];return true;
				},
				[this](bool protectNow) { gates.push_back(protectNow); return gateResult; },
				[this](const std::string& document, const std::string& body, domain::E_CAPTURE_OPERATION_SOURCE source, std::string* cardId, std::string* error) {
					createDocuments.push_back(document); createBodies.push_back(body); createSources.push_back(source);
					if(createFailures.contains(body)){*error="create-failed";return false;} *cardId="card-"+std::to_string(createBodies.size());return true;
				});
		}

		app::S_DROP_IMPORT_REQUEST Request(std::vector<app::S_DROP_IMPORT_CANDIDATE> candidates) const
		{
			return {std::string("window\0exact", 12), "document-1",
				std::string("correlation\0exact", 17), std::move(candidates)};
		}
	};

	std::string failure_names(const std::vector<app::S_DROP_IMPORT_FAILURE>& failures)
	{
		std::string result;
		for (const auto& failure : failures) {
			if (!result.empty()) { result += ','; }
			result += failure.sPath + ':';
			switch (failure.eKind) {
			case app::E_DROP_IMPORT_FAILURE_KIND::FileTooLarge: result += "file-too-large"; break;
			case app::E_DROP_IMPORT_FAILURE_KIND::ReadFailed: result += "read-failed"; break;
			case app::E_DROP_IMPORT_FAILURE_KIND::Blank: result += "blank"; break;
			case app::E_DROP_IMPORT_FAILURE_KIND::TotalTooLarge: result += "total-too-large"; break;
			case app::E_DROP_IMPORT_FAILURE_KIND::CreateFailed: result += "create-failed"; break;
			default: result += "structural-reject"; break;
			}
		}
		return result.empty()?"-":result;
	}

	class DatabaseFixture
	{
	public:
		DatabaseFixture()
			: path_(std::filesystem::temp_directory_path() / ("noteex_w2r7_" + std::to_string(::GetCurrentProcessId()) + "_" + std::to_string(++sequence_) + ".db")), repositories(database)
		{
			remove(); REQUIRE(database.Open(path_.string())); storage::C_MIGRATION_RUNNER runner; runner.SetExistingDatabase(false,path_.string()); REQUIRE(runner.Run(database)==storage::E_MIGRATE_RESULT::Ok);
			domain::S_DOCUMENT document; document.sId="document-1";document.sTitle="drop";document.nCreatedAtUs=1;document.nUpdatedAtUs=1;REQUIRE(repositories.CreateDocument(document)==storage::E_REPO_RESULT::Ok);
		}
		~DatabaseFixture(){database.Close();remove();}
		app::C_CARD_SERVICE Service(){auto ids=std::make_shared<int>(0);return app::C_CARD_SERVICE(database,repositories,parser,[]{return std::int64_t{2};},[ids]{return "drop-id-"+std::to_string((*ids)++);});}
		std::int64_t Count(const char* table){sqlite3_stmt* statement=nullptr;const std::string sql=std::string("SELECT COUNT(*) FROM ")+table;REQUIRE(::sqlite3_prepare_v2(database.Handle(),sql.c_str(),-1,&statement,nullptr)==SQLITE_OK);REQUIRE(::sqlite3_step(statement)==SQLITE_ROW);const auto value=::sqlite3_column_int64(statement,0);::sqlite3_finalize(statement);return value;}
		domain::C_PARAGRAPH_PARSER parser;storage::C_DATABASE database;storage::C_REPOSITORIES repositories;
	private:
		void remove(){std::error_code error;std::filesystem::remove(path_,error);std::filesystem::remove(path_.string()+"-wal",error);std::filesystem::remove(path_.string()+"-shm",error);}
		std::filesystem::path path_;inline static int sequence_=0;
	};

	#define DROP_TAGS(ID) "[W2-R7][core][application][drop-import-plan][" ID "]"
}

TEST_CASE("WTL-W2-0054", DROP_TAGS("WTL-W2-0054"))
{
	Harness h;h.snapshots["LICENSE"]=bytes(u8s(u8"확장자 없음"));h.snapshots[".gitignore"]=bytes(u8s(u8"닷파일"));h.snapshots["manual.pdf"]=bytes(u8s(u8"임의 확장자"));auto c=h.Coordinator();const auto e=c.Execute(c.Prepare(h.Request({candidate("LICENSE"),candidate(".gitignore"),candidate("manual.pdf")})));REQUIRE(e.CreatedCardIds.size()==3);REQUIRE(h.createBodies.size()==3);REQUIRE(e.ePostAction==app::E_DROP_IMPORT_POST_ACTION::RevealLastCreated);REQUIRE(e.sWindowId==std::string("window\0exact",12));REQUIRE(e.sCorrelationId==std::string("correlation\0exact",17));emit("WTL-W2-0054","paths=LICENSE,.gitignore,manual.pdf|bodies=ed9995ec9ea5ec9e9020ec9786ec9d8c,eb8bb7ed8c8cec9dbc,ec9e84ec9d9820ed9995ec9ea5ec9e90|post=reveal-last");
}

TEST_CASE("WTL-W2-0055", DROP_TAGS("WTL-W2-0055"))
{
	Harness h;h.snapshots["image.png"]={0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0,0,0,0x0d,0x49,0x48,0x44,0x52};h.snapshots["program.exe"]={0x4d,0x5a,0x90,0,3,0,0,0,4,0,0xff};auto c=h.Coordinator();const auto e=c.Execute(c.Prepare(h.Request({candidate("image.png"),candidate("program.exe")})));REQUIRE(e.CreatedCardIds.size()==2);REQUIRE_FALSE(h.createBodies[0].empty());REQUIRE(h.createBodies[1].starts_with("MZ"));REQUIRE(e.ReadFailures.empty());emit("WTL-W2-0055","image_input=89504e470d0a1a0a0000000d49484452|exe_input=4d5a9000030000000400ff|cards=2|image_nonempty=1|exe_prefix=4d5a|errors=0|post=reveal-last");
}

TEST_CASE("WTL-W2-0056", DROP_TAGS("WTL-W2-0056"))
{
	Harness h;h.snapshots["utf16.data"]={0xff,0xfe,0x55,0,0x54,0,0x46,0,0x2d,0,0x31,0,0x36,0,0x20,0,0x5c,0xd5,0,0xae,0x20,0,0xd0,0xc6,0x38,0xbb,0x0d,0,0x0a,0,0x58,0xb4,0xf8,0xc9,0x20,0,0x04,0xc9};auto c=h.Coordinator();const auto e=c.Execute(c.Prepare(h.Request({candidate("utf16.data")})));REQUIRE(h.createBodies==std::vector<std::string>{u8s(u8"UTF-16 한글 원문\r\n둘째 줄")});REQUIRE(e.ePostAction==app::E_DROP_IMPORT_POST_ACTION::ConnectOnlyCreated);emit("WTL-W2-0056","input=fffe5500540046002d003100360020005cd500ae2000d0c638bb0d000a0058b4f8c9200004c9|body=5554462d313620ed959ceab88020ec9b90ebacb80d0aeb9198eca7b820eca484|post=connect-one");
}

TEST_CASE("WTL-W2-0057", DROP_TAGS("WTL-W2-0057"))
{
	REQUIRE(::GetACP()==949);Harness h;h.snapshots["ansi.data"]={0x43,0x50,0x39,0x34,0x39,0x20,0xc7,0xd1,0xb1,0xdb,0x20,0xbf,0xf8,0xb9,0xae};auto c=h.Coordinator();const auto e=c.Execute(c.Prepare(h.Request({candidate("ansi.data")})));REQUIRE(e.CreatedCardIds.size()==1);REQUIRE(h.createBodies==std::vector<std::string>{u8s(u8"CP949 한글 원문")});emit("WTL-W2-0057","acp=949|applicable=1|input=435039343920c7d1b1db20bff8b9ae|body=435039343920ed959ceab88020ec9b90ebacb8|post=connect-one");
}

TEST_CASE("WTL-W2-0058", DROP_TAGS("WTL-W2-0058"))
{
	Harness h;h.snapshots["bom"]={0xff,0xfe};h.snapshots["space"]={0x20,9,13,10};auto c=h.Coordinator();const auto p=c.Prepare(h.Request({candidate("bom"),candidate("space")}));const auto e=c.Execute(p);REQUIRE(p.PreparedItems.empty());REQUIRE(p.ReadFailures.size()==2);REQUIRE(h.gates.empty());REQUIRE(h.createBodies.empty());REQUIRE(e.ePostAction==app::E_DROP_IMPORT_POST_ACTION::None);emit("WTL-W2-0058","inputs=fffe,20090d0a|prepared=0|read_failures=2|gate=0|create=0|editor_state=preserved");
}

TEST_CASE("WTL-W2-0059", DROP_TAGS("WTL-W2-0059"))
{
	Harness h;h.snapshots["first.txt"]=bytes(u8s(u8"첫 유효 본문"));h.snapshots["oversized.txt"]=std::vector<std::uint8_t>(app::MAX_IMPORT_FILE_BYTES+1,'x');h.readFailures.insert("unreadable.md");h.snapshots["empty-utf16-bom.txt"]={0xff,0xfe};h.snapshots["last.txt"]=bytes(u8s(u8"둘째 유효 본문"));auto c=h.Coordinator();const auto p=c.Prepare(h.Request({candidate("first.txt"),candidate("oversized.txt"),candidate("unreadable.md"),candidate("empty-utf16-bom.txt"),candidate("last.txt")}));const auto e=c.Execute(p);REQUIRE(e.CreatedCardIds.size()==2);REQUIRE(failure_names(p.ReadFailures)=="oversized.txt:file-too-large,unreadable.md:read-failed,empty-utf16-bom.txt:blank");emit("WTL-W2-0059","created=ecb2ab20ec9ca0ed9aa820ebb3b8ebacb8,eb9198eca7b820ec9ca0ed9aa820ebb3b8ebacb8|failures=oversized.txt:file-too-large,unreadable.md:read-failed,empty-utf16-bom.txt:blank|post=reveal-last|editor=-");
}

TEST_CASE("WTL-W2-0060", DROP_TAGS("WTL-W2-0060"))
{
	Harness h;h.snapshots["first.md"]=std::vector<std::uint8_t>(app::MAX_DROP_IMPORT_TOTAL_BYTES,'x');h.snapshots["last.md"]={'y'};auto c=h.Coordinator();const auto p=c.Prepare(h.Request({candidate("first.md"),candidate("last.md")}));const auto e=c.Execute(p);REQUIRE(p.bBatchFatal);REQUIRE(p.PreparedItems.empty());REQUIRE(failure_names(p.ReadFailures)=="last.md:total-too-large");REQUIRE_FALSE(e.bLeaveGateCalled);emit("WTL-W2-0060","first_bytes=4194304|next_bytes=1|batch_fatal=last.md:total-too-large|discarded_prepared=1|gate=0|created=0|db_delta=0,0,0,0");
}

TEST_CASE("WTL-W2-0061", DROP_TAGS("WTL-W2-0061"))
{
	Harness h;h.snapshots["snapshot.txt"]=bytes(u8s(u8"최초 snapshot"));auto coordinator=h.Coordinator();auto plan=coordinator.Prepare(h.Request({candidate("snapshot.txt")}));h.snapshots["snapshot.txt"]=bytes(u8s(u8"두 번째 내용"));const auto e=coordinator.Execute(plan);REQUIRE(e.CreatedCardIds.size()==1);REQUIRE(h.createBodies==std::vector<std::string>{u8s(u8"최초 snapshot")});REQUIRE(h.read.size()==1);REQUIRE(h.limits==std::vector<std::size_t>{4194305});emit("WTL-W2-0061","initial=ecb59cecb48820736e617073686f74|created=ecb59cecb48820736e617073686f74|path_after=eb919020ebb288eca7b820eb82b4ec9aa9|read_calls=1|read_limit=4194305");
}

TEST_CASE("WTL-W2-0062", DROP_TAGS("WTL-W2-0062"))
{
	Harness h;h.kinds["remote"]=app::E_DROP_PATH_KIND::NonLocal;h.kinds["directory"]=app::E_DROP_PATH_KIND::Directory;h.kinds["missing"]=app::E_DROP_PATH_KIND::Missing;auto c=h.Coordinator();const auto remote=c.Prepare(h.Request({candidate("remote")}));const auto directory=c.Prepare(h.Request({candidate("directory")}));const auto missing=c.Prepare(h.Request({candidate("missing")}));const auto combined=c.Prepare(h.Request({candidate("directory"),candidate("missing")}));REQUIRE(remote.bBatchFatal);REQUIRE(directory.bBatchFatal);REQUIRE(missing.bBatchFatal);REQUIRE(combined.ReadFailures.size()==2);REQUIRE(h.read.empty());REQUIRE(h.gates.empty());REQUIRE(h.createBodies.empty());emit("WTL-W2-0062","nonlocal=enter-reject|directory=structural-reject|missing=structural-reject|reads=0|gate=0|created=0|reports=2");
}

TEST_CASE("WTL-W2-0063", DROP_TAGS("WTL-W2-0063"))
{
	Harness h;h.snapshots["duplicate.md"]=bytes(u8s(u8"한 번만"));auto c=h.Coordinator();const auto p=c.Prepare(h.Request({candidate("duplicate.md","same"),candidate("duplicate.md","same")}));const auto e=c.Execute(p);REQUIRE(p.UniqueCandidates.size()==1);REQUIRE(h.read.size()==1);REQUIRE(e.CreatedCardIds.size()==1);emit("WTL-W2-0063","input_paths=duplicate.md,duplicate.md|unique=1|body=ed959c20ebb288eba78c|post=connect-one");
}

TEST_CASE("WTL-W2-0064", DROP_TAGS("WTL-W2-0064"))
{
	DatabaseFixture db;app::C_IMPORT_PIPELINE pipeline(db.parser,[](std::span<const std::uint8_t> input){return platform::DecodeSystemAnsi(input);});auto service=db.Service();app::C_DROP_IMPORT_COORDINATOR c(pipeline,[](const auto&){return app::E_DROP_PATH_KIND::LocalRegularFile;},[](const std::string&,std::size_t,std::vector<std::uint8_t>* out,std::string*){*out=bytes(u8s(u8"출처 확인"));return true;},[](bool){return true;},[&](const std::string& document,const std::string& body,domain::E_CAPTURE_OPERATION_SOURCE source,std::string* id,std::string*){domain::S_CARD card;const auto result=service.CreateCard(document,body,source,std::nullopt,&card);*id=card.sId;return result==app::E_CARD_SERVICE_RESULT::Ok;});const auto e=c.Execute(c.Prepare({"window","document-1","correlation",{candidate("source.txt")}}));REQUIRE(e.CreatedCardIds.size()==1);std::vector<domain::S_CARD> cards;REQUIRE(db.repositories.ListCards("document-1",&cards)==storage::E_REPO_RESULT::Ok);domain::S_CAPTURE_OPERATION operation;REQUIRE(db.repositories.GetCaptureOperation(cards[0].sOperationId,&operation)==storage::E_REPO_RESULT::Ok);std::vector<domain::S_EDIT_EVENT> events;REQUIRE(db.repositories.ListEvents("document-1",&events)==storage::E_REPO_RESULT::Ok);REQUIRE(cards[0].eSource==domain::E_CARD_SOURCE::Import);REQUIRE(operation.eSource==domain::E_CAPTURE_OPERATION_SOURCE::Import);REQUIRE(events.size()==1);REQUIRE(events[0].eSource==domain::E_EVENT_SOURCE::Import);emit("WTL-W2-0064","card=import|operation=import|event=import|create_events=1|post=connect-one");
}

TEST_CASE("WTL-W2-0065", DROP_TAGS("WTL-W2-0065"))
{
	Harness h;h.snapshots["valid.md"]=bytes(u8s(u8"검증은 끝남"));h.gateResult=false;auto c=h.Coordinator();const auto p=c.Prepare(h.Request({candidate("valid.md")}));const auto e=c.Execute(p);REQUIRE(h.read.size()==1);REQUIRE(p.PreparedItems.size()==1);REQUIRE(h.gates==std::vector<bool>{true});REQUIRE(h.createBodies.empty());REQUIRE(e.ReadFailures.empty());emit("WTL-W2-0065","reads=1|prepared=1|gate=protect-now:reject|created=0|errors=0|post=none");
}

TEST_CASE("WTL-W2-0066", DROP_TAGS("WTL-W2-0066"))
{
	Harness h;h.snapshots["empty-utf16-bom.txt"]={0xff,0xfe};const auto failed=u8s(u8"생성 실패"),success=u8s(u8"후속 성공");h.snapshots["failure.md"]=bytes(failed);h.snapshots["success.txt"]=bytes(success);h.createFailures.insert(failed);auto c=h.Coordinator();const auto p=c.Prepare(h.Request({candidate("empty-utf16-bom.txt"),candidate("failure.md"),candidate("success.txt")}));const auto e=c.Execute(p);REQUIRE((h.createBodies==std::vector<std::string>{failed,success}));REQUIRE(e.CreatedCardIds.size()==1);REQUIRE(failure_names(p.ReadFailures)=="empty-utf16-bom.txt:blank");REQUIRE(failure_names(e.CreateFailures)=="failure.md:create-failed");emit("WTL-W2-0066","blank_input=fffe|create_calls=ec839dec84b120ec8ba4ed8ca8,ed9b84ec868d20ec84b1eab3b5|created=ed9b84ec868d20ec84b1eab3b5|read_failures=empty-utf16-bom.txt:blank|create_failures=failure.md:create-failed|post=connect-one");
}

TEST_CASE("WTL-W2-0067", DROP_TAGS("WTL-W2-0067"))
{
	Harness h;std::vector<app::S_DROP_IMPORT_CANDIDATE> inputs;for(int i=0;i<21;++i)inputs.push_back(candidate(std::to_string(i)));auto c=h.Coordinator();const auto p=c.Prepare(h.Request(std::move(inputs)));const auto e=c.Execute(p);REQUIRE(p.bBatchFatal);REQUIRE(p.ReadFailures[0].eKind==app::E_DROP_IMPORT_FAILURE_KIND::TooManyFiles);REQUIRE(h.inspected.empty());REQUIRE(h.read.empty());REQUIRE(h.gates.empty());REQUIRE(h.createBodies.empty());emit("WTL-W2-0067","count=21|max=20|batch_fatal=too-many-files|gate=0|created=0");
}

TEST_CASE("WTL-W2-0068", DROP_TAGS("WTL-W2-0068"))
{
	Harness h;h.snapshots["oversized.md"]=std::vector<std::uint8_t>(app::MAX_IMPORT_FILE_BYTES+1,'x');auto c=h.Coordinator();const auto p=c.Prepare(h.Request({candidate("oversized.md")}));const auto e=c.Execute(p);REQUIRE_FALSE(p.bBatchFatal);REQUIRE(p.PreparedItems.empty());REQUIRE(failure_names(p.ReadFailures)=="oversized.md:file-too-large");REQUIRE(h.limits==std::vector<std::size_t>{4194305});REQUIRE_FALSE(e.bLeaveGateCalled);emit("WTL-W2-0068","bytes=4194305|read_limit=4194305|failures=oversized.md:file-too-large|prepared=0|gate=0|created=0");
}

TEST_CASE("WTL-W2-0069", DROP_TAGS("WTL-W2-0069"))
{
	Harness h;h.snapshots["extended-blank.txt"]={0x0b,0x0c};auto c=h.Coordinator();const auto p=c.Prepare(h.Request({candidate("extended-blank.txt")}));const auto e=c.Execute(p);REQUIRE(p.ReadFailures.size()==1);REQUIRE(p.ReadFailures[0].eKind==app::E_DROP_IMPORT_FAILURE_KIND::Blank);REQUIRE(h.createBodies.empty());REQUIRE(e.ePostAction==app::E_DROP_IMPORT_POST_ACTION::None);emit("WTL-W2-0069","input=0b0c|result=blank|db_delta=0,0,0,0|created=0|post=none");
}

TEST_CASE("WTL-W2-0070", DROP_TAGS("WTL-W2-0070"))
{
	Harness h;h.snapshots["extended-blank.txt"]={0x0b,0x0c};h.snapshots["valid.txt"]=bytes(u8s(u8"정상 카드"));auto c=h.Coordinator();const auto p=c.Prepare(h.Request({candidate("extended-blank.txt"),candidate("valid.txt")}));const auto e=c.Execute(p);REQUIRE(e.CreatedCardIds.size()==1);REQUIRE(h.createBodies==std::vector<std::string>{u8s(u8"정상 카드")});REQUIRE(failure_names(p.ReadFailures)=="extended-blank.txt:blank");emit("WTL-W2-0070","blank_input=0b0c|valid_input=eca095ec838120ecb9b4eb939c|created=eca095ec838120ecb9b4eb939c|read_failures=extended-blank.txt:blank|post=connect-one");
}

TEST_CASE("WTL-W2-0071", DROP_TAGS("WTL-W2-0071"))
{
	Harness h;h.snapshots["blank.txt"]={0x0b,0x0c,0xc2,0xa0};auto c=h.Coordinator();const auto p=c.Prepare(h.Request({candidate("blank.txt")}));const auto e=c.Execute(p);REQUIRE(p.ReadFailures.size()==1);REQUIRE(p.ReadFailures[0].eKind==app::E_DROP_IMPORT_FAILURE_KIND::Blank);REQUIRE(h.createBodies.empty());REQUIRE(e.CreatedCardIds.empty());emit("WTL-W2-0071","input=0b0cc2a0|result=blank|create_calls=0|cards=0|post=none");
}

#undef DROP_TAGS
