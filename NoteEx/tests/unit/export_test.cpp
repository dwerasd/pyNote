#include <catch_amalgamated.hpp>

#include "pynote/core/application/export.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
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
	namespace app=pynote::core::application; namespace domain=pynote::core::domain;
	std::string u8s(const char8_t* value) { return std::string(reinterpret_cast<const char*>(value)); }
	std::string hex(std::string_view text)
	{
		std::ostringstream stream; stream<<std::hex<<std::setfill('0');
		for(const unsigned char value:text){stream<<std::setw(2)<<static_cast<unsigned int>(value);} return stream.str();
	}
	domain::S_CARD card(std::string id,std::string body,std::int64_t position,std::optional<std::int64_t> deleted=std::nullopt)
	{
		domain::S_CARD result; result.sId=std::move(id); result.sDocumentId="document-1"; result.sOperationId="operation-1";
		result.nPositionKey=position; result.nCaptureSeq=position; result.nCreatedAtUs=1; result.nUpdatedAtUs=1;
		result.eSource=domain::E_CARD_SOURCE::Typing; result.sBody=std::move(body); result.sBodyHash="hash";
		result.sCurrentRevisionId="revision-"+result.sId; result.nDeletedAtUs=deleted; return result;
	}
	std::string join(const std::vector<std::string>& values)
	{
		std::string result; for(const auto& value:values){if(!result.empty())result+=','; result+=hex(value);} return result.empty()?"-":result;
	}
	void emit(std::string_view id,std::string_view payload)
	{
		wchar_t path[32768]={}; const DWORD length=::GetEnvironmentVariableW(L"PYNOTE_DIFF_EXPORT_GOLDEN_OUT",path,32768);
		if(length==0||length>=32768)return; std::ofstream output(std::filesystem::path(path),std::ios::binary|std::ios::app);
		REQUIRE(output.is_open()); output<<id<<'|'<<payload<<'\n'; REQUIRE(output.good());
	}
}

#define TAGS(ID) "[W2-R3][core][application][export][" ID "]"

TEST_CASE("WTL-W2-0116", TAGS("WTL-W2-0116"))
{
	const std::vector<domain::S_CARD> cards={card("card-1",u8s(u8"첫 줄\n\n마지막 줄"),1024)};
	const std::string content=app::RenderCards(cards); REQUIRE(content==cards[0].sBody);
	emit("WTL-W2-0116","newline=0a|active="+join(app::ActiveCardIdsInExportOrder(cards))+"|calls=0|result=ok|content="+hex(content)+"|error=-");
}

TEST_CASE("WTL-W2-0117", TAGS("WTL-W2-0117"))
{
	const std::vector<domain::S_CARD> cards={card("second",u8s(u8"둘째\r\n줄"),2048),card("deleted",u8s(u8"제외"),3072,3),card("first",u8s(u8"첫째\n줄"),1024)};
	const auto path=std::filesystem::temp_directory_path()/"noteex_w2r3_direct_export.MD"; std::error_code ec; std::filesystem::remove(path,ec);
	int calls=0; std::string captured; std::string error;
	const app::DirectExportWriter writer=[&](const std::string& supplied,const std::string& content,std::string*){
		++calls; captured=content; REQUIRE_FALSE(supplied.empty()); std::ofstream output(path,std::ios::binary|std::ios::trunc);
		output.write(content.data(),static_cast<std::streamsize>(content.size())); return output.good(); };
	const auto path8=path.u8string(); const std::string supplied(reinterpret_cast<const char*>(path8.data()),path8.size());
	const auto result=app::ExportCards(supplied,cards,app::E_NEWLINE_FORMAT::Crlf,writer,&error);
	REQUIRE(result==app::E_EXPORT_RESULT::Ok); REQUIRE(calls==1);
	std::ifstream input(path,std::ios::binary); const std::string actual((std::istreambuf_iterator<char>(input)),{});
	const std::string expected=u8s(u8"첫째\r\n줄\r\n\r\n둘째\r\n줄"); REQUIRE(actual==expected); REQUIRE(captured==expected);
	std::filesystem::remove(path,ec);
	emit("WTL-W2-0117","newline=0d0a|active="+join(app::ActiveCardIdsInExportOrder(cards))+"|calls="+std::to_string(calls)+"|result=ok|content="+hex(captured)+"|error=-");
}

TEST_CASE("WTL-W2-0118", TAGS("WTL-W2-0118"))
{
	int calls=0; std::string error; const app::DirectExportWriter writer=[&](const std::string&,const std::string&,std::string*){++calls;return true;};
	const auto result=app::ExportCards("cards.json",{},app::E_NEWLINE_FORMAT::Lf,writer,&error);
	REQUIRE(result==app::E_EXPORT_RESULT::InvalidSuffix); REQUIRE(calls==0); REQUIRE(error=="invalid-suffix");
	emit("WTL-W2-0118","newline=0a|active=-|calls=0|result=invalid-suffix|content=-|error=invalid-suffix");
}

#undef TAGS
