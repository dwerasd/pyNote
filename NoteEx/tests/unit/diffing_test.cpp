#include <catch_amalgamated.hpp>

#include "pynote/core/domain/diffing.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#pragma comment(lib, "NoteExCore")

namespace
{
	namespace domain = pynote::core::domain;
	std::string u8s(const char8_t* value) { return std::string(reinterpret_cast<const char*>(value)); }
	std::string hex(std::string_view text)
	{
		std::ostringstream stream; stream << std::hex << std::setfill('0');
		for (const unsigned char value : text) { stream << std::setw(2) << static_cast<unsigned int>(value); }
		return stream.str();
	}
	std::string number(const std::optional<std::size_t>& value) { return value ? std::to_string(*value) : "-"; }
	std::string serialize(const domain::S_TEXT_DIFF& diff)
	{
		std::string lines;
		for (const auto& line : diff.Lines) {
			if (!lines.empty()) { lines += '/'; }
			lines += std::string(domain::ToText(line.eTag)) + ',' + number(line.nBeforeLineNumber) + ','
				+ number(line.nAfterLineNumber) + ',' + hex(line.sBefore) + ',' + hex(line.sAfter) + ',';
			if (line.Characters.empty()) { lines += '-'; continue; }
			bool first = true;
			for (const auto& part : line.Characters) {
				if (!first) { lines += ';'; }
				lines += std::string(domain::ToText(part.eTag)) + ':'
					+ (part.sBefore.empty() ? "-" : hex(part.sBefore)) + ':'
					+ (part.sAfter.empty() ? "-" : hex(part.sAfter));
				first = false;
			}
		}
		return "before=" + hex(diff.sBefore) + "|after=" + hex(diff.sAfter) + "|lines=" + lines;
	}
	void emit(std::string_view id, std::string_view payload)
	{
		wchar_t path[32768] = {};
		const DWORD length = ::GetEnvironmentVariableW(L"PYNOTE_DIFF_EXPORT_GOLDEN_OUT",path,32768);
		if (length == 0 || length >= 32768) { return; }
		std::ofstream output(std::filesystem::path(path),std::ios::binary
			| (id == "WTL-W2-0113" ? std::ios::trunc : std::ios::app));
		REQUIRE(output.is_open()); output << id << '|' << payload << '\n'; REQUIRE(output.good());
	}
}

#define TAGS(ID) "[W2-R3][core][domain][diffing][" ID "]"

TEST_CASE("WTL-W2-0113", TAGS("WTL-W2-0113"))
{
	const std::string before=u8s(u8"첫 줄\n바뀌기 전\n삭제\n");
	const std::string after=u8s(u8"첫 줄\n바뀐 뒤\n추가\n");
	const auto result=domain::DiffText(before,after);
	REQUIRE(result.sBefore==before); REQUIRE(result.sAfter==after); REQUIRE(result.Lines.size()==3);
	REQUIRE(result.Lines[0].eTag==domain::E_DIFF_TAG::Equal);
	REQUIRE(result.Lines[1].eTag==domain::E_DIFF_TAG::Replace);
	REQUIRE(result.Lines[1].nBeforeLineNumber==2); REQUIRE(result.Lines[1].nAfterLineNumber==2);
	REQUIRE(result.Lines[1].sBefore==u8s(u8"바뀌기 전\n")); REQUIRE(result.Lines[1].sAfter==u8s(u8"바뀐 뒤\n"));
	const std::string boundaries=u8s(u8"a\r\nb\rc\nd\ve\ff\u001cg\u001dh\u001ei\u0085j\u2028k\u2029z");
	const auto boundaryResult=domain::DiffText(boundaries,boundaries); REQUIRE(boundaryResult.Lines.size()==12);
	emit("WTL-W2-0113",serialize(result));
}

TEST_CASE("WTL-W2-0114", TAGS("WTL-W2-0114"))
{
	const auto result=domain::DiffText("A\nB\nC",u8s(u8"A\n새 줄"));
	REQUIRE(result.Lines.size()==3); REQUIRE(result.Lines[0].eTag==domain::E_DIFF_TAG::Equal);
	REQUIRE(result.Lines[1].eTag==domain::E_DIFF_TAG::Replace); REQUIRE(result.Lines[2].eTag==domain::E_DIFF_TAG::Delete);
	REQUIRE(result.Lines[2].nBeforeLineNumber==3); REQUIRE_FALSE(result.Lines[2].nAfterLineNumber.has_value());
	emit("WTL-W2-0114",serialize(result));
}

TEST_CASE("WTL-W2-0115", TAGS("WTL-W2-0115"))
{
	const std::string before=u8s(u8"한글 A🧭B"); const std::string after=u8s(u8"한글 A🧭C");
	const auto changes=domain::DiffCharacters(before,after); std::string reconstructedBefore,reconstructedAfter;
	for (const auto& part : changes) { reconstructedBefore+=part.sBefore; reconstructedAfter+=part.sAfter; }
	REQUIRE(reconstructedBefore==before); REQUIRE(reconstructedAfter==after);
	REQUIRE(changes.back().eTag==domain::E_DIFF_TAG::Replace);
	const std::string expectedPrefix=u8s(u8"한글 A🧭"); REQUIRE(changes.front().sBefore==expectedPrefix);
	const auto tie=domain::DiffCharacters("ab","ba"); REQUIRE(tie.front().eTag==domain::E_DIFF_TAG::Insert);
	const auto repeated=domain::DiffCharacters(std::string(300,'x')+"A",std::string(300,'x')+"B");
	REQUIRE(repeated.front().eTag==domain::E_DIFF_TAG::Equal); REQUIRE(repeated.front().sBefore.size()==300);
	domain::S_TEXT_DIFF capture{before,after,{{domain::E_DIFF_TAG::Replace,1,1,before,after,changes}}};
	emit("WTL-W2-0115",serialize(capture));
}

#undef TAGS
