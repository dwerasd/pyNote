#include <catch_amalgamated.hpp>

#include "pynote/core/application/card_service.h"
#include "pynote/core/application/search_result_plan.h"
#include "pynote/core/domain/paragraph_parser.h"
#include "pynote/core/storage/migration_runner.h"

#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>

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

	std::string hex(std::string_view value)
	{
		constexpr char digits[]="0123456789abcdef";std::string result;result.reserve(value.size()*2);
		for(const unsigned char byte:value){result.push_back(digits[byte>>4]);result.push_back(digits[byte&15]);}return result;
	}
	std::string state(app::E_SEARCH_RESULT_STATE value){return value==app::E_SEARCH_RESULT_STATE::NeedsQuery?"needs-query":"results";}
	std::string rows(const app::S_SEARCH_RESULT_PLAN& plan)
	{
		std::string result;
		for(const auto& row:plan.Rows){if(!result.empty())result.push_back(',');result+=row.eKind==app::E_SEARCH_RESULT_ROW_KIND::DocumentTitle?"title:":"card:";
			result+=hex(row.sDocumentId)+":"+(row.sCardId?hex(*row.sCardId):"-")+":"+hex(row.sDocumentTitle)+":"+hex(row.sMatchText);}
		return result.empty()?"-":result;
	}
	void record(std::vector<std::string>& output,std::string_view name,const app::S_SEARCH_RESULT_PLAN& plan)
	{
		output.push_back(std::string(name)+"|query="+hex(plan.sNormalizedQuery)+"|state="+state(plan.eState)+"|count="+std::to_string(plan.Rows.size())+"|rows="+rows(plan));
	}
	domain::S_DOCUMENT document(std::string id,std::string title){domain::S_DOCUMENT value;value.sId=std::move(id);value.sTitle=std::move(title);value.nCreatedAtUs=1000;value.nUpdatedAtUs=1000;return value;}
	domain::S_CARD card(std::string id,std::string documentId,std::string body){domain::S_CARD value;value.sId=std::move(id);value.sDocumentId=std::move(documentId);value.sBody=std::move(body);return value;}

	app::C_SEARCH_RESULT_PLANNER scripted(std::vector<domain::S_DOCUMENT> documents,std::vector<domain::S_CARD> cards,
		app::GetDocumentPort get={})
	{
		if(!get)get=[](const std::string&,domain::S_DOCUMENT*){return storage::E_REPO_RESULT::NotFound;};
		return app::C_SEARCH_RESULT_PLANNER(
			[documents=std::move(documents)](const std::string&,auto* out){*out=documents;return storage::E_REPO_RESULT::Ok;},
			[cards=std::move(cards)](const std::string&,auto* out){*out=cards;return storage::E_REPO_RESULT::Ok;},std::move(get));
	}

	class DbFixture
	{
	private:
		std::filesystem::path path_;storage::C_DATABASE database_;
	public:
		storage::C_REPOSITORIES repositories;
	public:
		DbFixture():path_(std::filesystem::temp_directory_path()/("noteex_w2z3_"+std::to_string(::GetCurrentProcessId())+"_"+std::to_string(++sequence_)+".db")),repositories(database_)
		{remove_();REQUIRE(database_.Open(path_.string()));storage::C_MIGRATION_RUNNER runner;runner.SetExistingDatabase(false,path_.string());REQUIRE(runner.Run(database_)==storage::E_MIGRATE_RESULT::Ok);}
		~DbFixture(){database_.Close();remove_();}
		void Create(std::string title,std::string body,bool archived)
		{auto doc=document("doc-1",std::move(title));if(archived)doc.nArchivedAtUs=1500;REQUIRE(repositories.CreateDocument(doc)==storage::E_REPO_RESULT::Ok);
			app::C_CARD_SERVICE service(database_,repositories,parser_,[]{return 2000;},[this]{return ids_[id_++];});domain::S_CARD made;REQUIRE(service.CreateCard("doc-1",body,domain::E_CAPTURE_OPERATION_SOURCE::Typing,std::nullopt,&made)==app::E_CARD_SERVICE_RESULT::Ok);}
	private:
		void remove_(){std::error_code e;std::filesystem::remove(path_,e);std::filesystem::remove(path_.string()+"-wal",e);std::filesystem::remove(path_.string()+"-shm",e);}
		domain::C_PARAGRAPH_PARSER parser_;std::size_t id_{};
		const std::vector<std::string> ids_{"operation-1","card-1","revision-1","event-1"};inline static int sequence_{};
	};
	void emit(const std::vector<std::string>& records)
	{wchar_t path[32768]={};const DWORD length=::GetEnvironmentVariableW(L"PYNOTE_SEARCH_RESULT_GOLDEN_OUT",path,32768);if(length==0||length>=32768)return;std::ofstream output(std::filesystem::path(path),std::ios::binary|std::ios::trunc);REQUIRE(output.is_open());for(const auto& line:records)output<<line<<'\n';REQUIRE(output.good());}
}

TEST_CASE("W2-Z3 search result plan","[W2-Z3][core][application][search]")
{
	std::vector<std::string> output;
	int documentCalls=0,cardCalls=0,getCalls=0;
	app::C_SEARCH_RESULT_PLANNER empty(
		[&](const std::string&,auto*){++documentCalls;return storage::E_REPO_RESULT::Ok;},
		[&](const std::string&,auto*){++cardCalls;return storage::E_REPO_RESULT::Ok;},
		[&](const std::string&,auto*){++getCalls;return storage::E_REPO_RESULT::NotFound;});
	auto plan=empty.Plan("\xE3\x80\x80\xC2\xA0\t");REQUIRE(plan.eState==app::E_SEARCH_RESULT_STATE::NeedsQuery);REQUIRE(documentCalls==0);REQUIRE(cardCalls==0);REQUIRE(getCalls==0);record(output,"empty-unicode-strip",plan);
	const auto invalidQuery=empty.Plan("\xC3\x28");REQUIRE(invalidQuery.eError==app::E_SEARCH_RESULT_ERROR::InvalidUtf8);REQUIRE(invalidQuery.Rows.empty());REQUIRE(documentCalls==0);REQUIRE(cardCalls==0);REQUIRE(getCalls==0);

	const std::string strasse="Stra\xC3\x9F" "e";
	plan=scripted({document("doc-b",strasse),document("doc-a","neutral")},{}).Plan(" \xC2\xA0STRASSE\xE3\x80\x80");
	REQUIRE(plan.Rows.size()==1);REQUIRE(plan.Rows[0].sDocumentId=="doc-b");record(output,"navigator-title-casefold",plan);

	{DbFixture fixture;fixture.Create(strasse,"STRASSE anchor",true);app::C_SEARCH_RESULT_PLANNER planner(fixture.repositories);plan=planner.Plan("STRASSE");
	REQUIRE(plan.Rows.size()==2);REQUIRE(plan.Rows[0].eKind==app::E_SEARCH_RESULT_ROW_KIND::DocumentTitle);REQUIRE(plan.Rows[1].eKind==app::E_SEARCH_RESULT_ROW_KIND::CardBody);record(output,"global-title-casefold",plan);}
	{DbFixture fixture;fixture.Create("neutral",strasse,false);app::C_SEARCH_RESULT_PLANNER planner(fixture.repositories);plan=planner.Plan("STRASSE");REQUIRE(plan.Rows.empty());record(output,"body-like-not-casefold",plan);}

	plan=scripted({document("doc-b","aB"),document("doc-a","aA")},{card("card-a2","doc-a","a2"),card("card-a1","doc-a","a1"),card("card-b","doc-b","b")}).Plan("a");
	REQUIRE(plan.Rows.size()==5);REQUIRE(plan.Rows[0].sDocumentId=="doc-b");REQUIRE(plan.Rows[1].sDocumentId=="doc-a");REQUIRE(plan.Rows[2].sCardId=="card-a2");REQUIRE(plan.Rows[4].sCardId=="card-b");record(output,"result-order",plan);

	getCalls=0;plan=scripted({}, {card("card-x","doc-x","match")},[&](const std::string& id,domain::S_DOCUMENT* out){++getCalls;REQUIRE(id=="doc-x");*out=document("doc-x","Fallback");return storage::E_REPO_RESULT::Ok;}).Plan("match");
	REQUIRE(getCalls==1);REQUIRE(plan.Rows[0].sDocumentTitle=="Fallback");record(output,"fallback-document-title",plan);
	auto invalidFallback=scripted({document("doc-good","x")},{card("card-bad","doc-missing","x")},[](const std::string&,domain::S_DOCUMENT* out){*out=document("doc-missing","\xC3\x28");return storage::E_REPO_RESULT::Ok;}).Plan("x");
	REQUIRE(invalidFallback.eError==app::E_SEARCH_RESULT_ERROR::InvalidUtf8);REQUIRE(invalidFallback.Rows.empty());REQUIRE(invalidFallback.eSourceResult==storage::E_REPO_RESULT::Invalid);

	std::string body160(156,'a');body160+="\xF0\x9F\x98\x80\n\rz";plan=scripted({}, {card("card-160","doc-p",body160)}).Plan("x");
	REQUIRE(plan.Rows[0].sMatchText.size()==body160.size());REQUIRE(plan.Rows[0].sMatchText.find('\n')==std::string::npos);REQUIRE(plan.Rows[0].sMatchText.find('\r')!=std::string::npos);record(output,"preview-160",plan);
	std::string body161(156,'a');body161+="\xF0\x9F\x98\x80" "bcde";plan=scripted({}, {card("card-161","doc-p",body161)}).Plan("x");
	const std::string ellipsis="\xE2\x80\xA6";REQUIRE(plan.Rows[0].sMatchText==std::string(156,'a')+"\xF0\x9F\x98\x80"+ellipsis);record(output,"preview-161",plan);

	app::C_SEARCH_RESULT_PLANNER failure([](const std::string&,auto*){return storage::E_REPO_RESULT::Failed;},[](const std::string&,auto*){FAIL("cards called after document failure");return storage::E_REPO_RESULT::Ok;},[](const std::string&,auto*){return storage::E_REPO_RESULT::NotFound;});
	const auto failed=failure.Plan("x");REQUIRE_FALSE(failed.Succeeded());REQUIRE(failed.Rows.empty());REQUIRE(failed.eSourceResult==storage::E_REPO_RESULT::Failed);
	REQUIRE(output.size()==8);emit(output);
}
