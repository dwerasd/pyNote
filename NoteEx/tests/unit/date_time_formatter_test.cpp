#include <catch_amalgamated.hpp>

#include "pynote/core/domain/date_time_formatter.h"

#include <filesystem>
#include <fstream>
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

	domain::S_DATE_TIME_VIEW view(int hour = 12, int millisecond = 120)
	{
		return { 2024, 1, 2, hour, 4, 5, millisecond, 0, "UTC", "Coordinated Universal Time", true };
	}

	domain::S_DATE_TIME_VIEW seoul_view()
	{
		return { 2024, 1, 2, 12, 4, 5, 120, 9 * 3600, "GMT+9", "Korean Standard Time", true };
	}

	std::string hex(std::string_view value)
	{
		constexpr char digits[] = "0123456789abcdef";
		std::string result; result.reserve(value.size() * 2);
		for (const unsigned char byte : value) {
			result.push_back(digits[byte >> 4]); result.push_back(digits[byte & 0x0f]);
		}
		return result;
	}

	void check(std::vector<std::string>& records, std::string_view label,
		const domain::S_DATE_TIME_VIEW& value, std::string_view format, std::string_view expected)
	{
		const auto actual = domain::FormatDateTime(value, format);
		REQUIRE(actual == expected);
		records.push_back(std::string(label) + "|format=" + hex(format) + "|output=" + hex(actual));
	}

	void emit(const std::vector<std::string>& records)
	{
		wchar_t path[32768] = {};
		const DWORD length = ::GetEnvironmentVariableW(L"PYNOTE_DATETIME_GOLDEN_OUT", path, 32768);
		if (length == 0 || length >= 32768) { return; }
		std::ofstream output(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
		REQUIRE(output.is_open());
		for (const auto& record : records) { output << record << '\n'; }
		REQUIRE(output.good());
	}
}

TEST_CASE("T4A-UNC-001", "[W2-Z1][core][domain][date-time][T4A-UNC-001]")
{
	std::vector<std::string> records;
	check(records, "preset", view(), "yyyy-MM-dd HH:mm", "2024-01-02 12:04");
	check(records, "date-tokens", view(), "d|dd|ddd|dddd|M|MM|MMM|MMMM|yy|yyyy",
		"2|02|Tue|Tuesday|1|01|Jan|January|24|2024");

	check(records, "clock24-midnight", view(0), "h|hh|H|HH", "0|00|0|00");
	check(records, "clock24-afternoon", view(13), "h|hh|H|HH", "13|13|13|13");
	check(records, "clock12-midnight", view(0), "h|hh|H|HH|AP|A|ap|a|aP|Ap",
		"12|12|0|00|AM|AM|am|am|AM|AM");
	check(records, "clock12-afternoon", view(13), "h|hh|H|HH|AP|A|ap|a|aP|Ap",
		"1|01|13|13|PM|PM|pm|pm|PM|PM");

	check(records, "milliseconds-000", view(12, 0), "z|zz|zzz|zzzz|zzzzz", "0|0|000|0000|0000");
	check(records, "milliseconds-007", view(12, 7), "z|zz|zzz|zzzz|zzzzz", "007|007|007|007007|007007");
	check(records, "milliseconds-040", view(12, 40), "z|zz|zzz|zzzz|zzzzz", "04|04|040|04004|04004");
	check(records, "milliseconds-120", view(12, 120), "z|zz|zzz|zzzz|zzzzz", "12|12|120|12012|12012");
	check(records, "milliseconds-250", view(12, 250), "z|zz|zzz|zzzz|zzzzz", "25|25|250|25025|25025");
	check(records, "milliseconds-999", view(12, 999), "z|zz|zzz|zzzz|zzzzz", "999|999|999|999999|999999");

	check(records, "quote-paired", view(), "'Date:' yyyy", "Date: 2024");
	check(records, "quote-doubled", view(), "'it''s' ''yyyy''", "it's '2024'");
	check(records, "quote-unmatched", view(), "'open yyyy", "open yyyy");
	check(records, "literal-utf8", view(), "Q/X \xCE\xBB \xED\x95\x9C\xEA\xB8\x80",
		"Q/X \xCE\xBB \xED\x95\x9C\xEA\xB8\x80");

	check(records, "greedy", seoul_view(), "yyyyy|MMMMM|dddddd|HHHHH|zzzz|ttttt",
		"2024y|January1|Tuesday02|121212|12012|Korean Standard TimeGMT+9");
	check(records, "timezone-seoul", seoul_view(), "t|tt|ttt|tttt",
		"GMT+9|+0900|+09:00|Korean Standard Time");

	for (const std::string_view locale : { "C", "ko_KR", "de_DE", "ar_EG" }) {
		check(records, std::string("locale-") + std::string(locale), view(13),
			"ddd|dddd|MMM|MMMM|AP|ap", "Tue|Tuesday|Jan|January|PM|pm");
	}

	check(records, "empty-format", view(), "", "");
	auto invalid = view(); invalid.bValid = false;
	check(records, "invalid-flag", invalid, "yyyy-MM-dd HH:mm", "");
	invalid = view(); invalid.nDay = 32;
	check(records, "invalid-fields", invalid, "yyyy-MM-dd HH:mm", "");
	REQUIRE(records.size() == 25);
	emit(records);
}
