#include <catch_amalgamated.hpp>

#include "pynote/core/domain/time_zone_resolver.h"
#include "pynote/platform/win32_time_zone_resolver.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#pragma comment(lib, "NoteExCore")

namespace
{
	namespace domain = pynote::core::domain;

	std::int64_t epoch_us(int year, unsigned month, unsigned day, int hour, int minute, int second, int millisecond = 0)
	{
		using namespace std::chrono;
		return duration_cast<microseconds>(sys_days{ std::chrono::year{year}/month/day }.time_since_epoch() +
			hours(hour) + minutes(minute) + seconds(second) + milliseconds(millisecond)).count();
	}

	void append_number(std::string& output, std::int64_t value, int width)
	{
		std::string digits = std::to_string(value);
		if (static_cast<int>(digits.size()) < width) output.append(width - digits.size(), '0');
		output += digits;
	}

	std::string local(const domain::S_TIME_ZONE_RESOLUTION& value)
	{
		if (!value.bValid) return "-";
		std::string result; append_number(result, value.nYear, 4); result.push_back('-');
		append_number(result, value.nMonth, 2); result.push_back('-'); append_number(result, value.nDay, 2);
		result.push_back('T'); append_number(result, value.nHour, 2); result.push_back(':');
		append_number(result, value.nMinute, 2); result.push_back(':'); append_number(result, value.nSecond, 2);
		result.push_back('.'); append_number(result, value.nMillisecond, 3); return result;
	}

	std::string hex(std::string_view value)
	{
		constexpr char digits[] = "0123456789abcdef"; std::string result; result.reserve(value.size() * 2);
		for (const unsigned char byte : value) { result.push_back(digits[byte >> 4]); result.push_back(digits[byte & 15]); }
		return result;
	}

	void row(std::vector<std::string>& rows, std::string_view name, std::string_view id,
		std::int64_t epoch, const domain::S_TIME_ZONE_RESOLUTION& value)
	{
		rows.push_back("T4A-UNC-004|case=" + std::string(name) + "|id=" + hex(id) +
			"|epoch_us=" + std::to_string(epoch) + "|valid=" + (value.bValid ? "1" : "0") +
			"|local=" + local(value) + "|offset=" + (value.bValid ? std::to_string(value.nUtcOffsetSeconds) : "-"));
	}

	void emit(const std::vector<std::string>& rows)
	{
		wchar_t path[32768] = {};
		const DWORD length = ::GetEnvironmentVariableW(L"PYNOTE_TIMEZONE_GOLDEN_OUT", path, 32768);
		if (length == 0 || length >= 32768) return;
		std::ofstream output(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
		REQUIRE(output.is_open()); for (const auto& value : rows) output << value << '\n'; REQUIRE(output.good());
	}

	void require_offset(const domain::C_TIME_ZONE_RESOLVER& resolver, std::string_view id,
		std::int64_t epoch, int offset)
	{
		const auto value = resolver.Resolve(id, epoch); REQUIRE(value.bValid); REQUIRE(value.nUtcOffsetSeconds == offset);
	}
}

TEST_CASE("T4A-UNC-004", "[W2-Z2][core][platform][time-zone][T4A-UNC-004]")
{
	const domain::C_TIME_ZONE_RESOLVER resolver(pynote::platform::MakeWin32SystemTimeZoneResolver());
	const auto& ids = domain::C_TIME_ZONE_RESOLVER::SupportedTimeZoneIds();
	REQUIRE(ids.size() == 488); REQUIRE(std::set<std::string>(ids.begin(), ids.end()).size() == 488);
	REQUIRE(std::is_sorted(ids.begin(), ids.end())); REQUIRE(std::find(ids.begin(), ids.end(), "system") == ids.end());
	const auto winter = epoch_us(2024, 1, 15, 12, 0, 0, 123);
	const auto summer = epoch_us(2024, 7, 15, 12, 0, 0, 456);
	std::vector<std::string> rows;
	for (const auto [name, epoch] : { std::pair{"system-winter", winter}, std::pair{"system-summer", summer} }) {
		const auto value = resolver.Resolve("system", epoch); REQUIRE(value.bValid);
		REQUIRE(value.eSource == domain::E_TIME_ZONE_RESOLUTION_SOURCE::System); row(rows, name, "system", epoch, value);
	}
	std::size_t fixed = 0, bundledOrUtc = 0;
	for (std::size_t index = 0; index < ids.size(); ++index) {
		for (const auto [suffix, epoch] : { std::pair{"winter", winter}, std::pair{"summer", summer} }) {
			const auto value = resolver.Resolve(ids[index], epoch); REQUIRE(value.bValid); REQUIRE(value.sRequestedId == ids[index]);
			REQUIRE(value.eError == domain::E_TIME_ZONE_RESOLUTION_ERROR::None);
			if (suffix == std::string_view("winter")) {
				if (value.eSource == domain::E_TIME_ZONE_RESOLUTION_SOURCE::FixedOffset) ++fixed;
				else if (value.eSource == domain::E_TIME_ZONE_RESOLUTION_SOURCE::BundledTzdata ||
					value.eSource == domain::E_TIME_ZONE_RESOLUTION_SOURCE::Utc) ++bundledOrUtc;
				if (value.eSource == domain::E_TIME_ZONE_RESOLUTION_SOURCE::BundledTzdata) REQUIRE(value.sTzdataVersion == "2026c");
			}
			const auto name = "id-" + std::to_string(1000 + index).substr(1) + "-" + suffix;
			row(rows, name, ids[index], epoch, value);
		}
	}
	REQUIRE(fixed == 43); REQUIRE(bundledOrUtc == 445);
	const auto plusZero = resolver.Resolve("UTC+00:00", winter), minusZero = resolver.Resolve("UTC-00:00", winter);
	REQUIRE(plusZero.bValid); REQUIRE(minusZero.bValid); REQUIRE(plusZero.nUtcOffsetSeconds == 0);
	REQUIRE(minusZero.nUtcOffsetSeconds == 0); REQUIRE(plusZero.sRequestedId != minusZero.sRequestedId);

	struct Boundary { const char* name; const char* id; std::int64_t epoch; };
	const std::vector<Boundary> boundaries{
		{"ny-spring-before","America/New_York",epoch_us(2024,3,10,6,59,59)}, {"ny-spring-at","America/New_York",epoch_us(2024,3,10,7,0,0)},
		{"ny-fall-before","America/New_York",epoch_us(2024,11,3,5,59,59)}, {"ny-fall-at","America/New_York",epoch_us(2024,11,3,6,0,0)},
		{"juarez-spring-before","America/Ciudad_Juarez",epoch_us(2024,3,10,8,59,59)}, {"juarez-spring-at","America/Ciudad_Juarez",epoch_us(2024,3,10,9,0,0)},
		{"juarez-fall-before","America/Ciudad_Juarez",epoch_us(2024,11,3,7,59,59)}, {"juarez-fall-at","America/Ciudad_Juarez",epoch_us(2024,11,3,8,0,0)},
		{"vostok-before","Antarctica/Vostok",epoch_us(2023,12,17,18,59,59)}, {"vostok-at","Antarctica/Vostok",epoch_us(2023,12,17,19,0,0)},
		{"urumqi-winter","Asia/Urumqi",winter}, {"urumqi-summer","Asia/Urumqi",summer},
		{"seoul-winter","Asia/Seoul",winter}, {"seoul-summer","Asia/Seoul",summer},
	};
	for (const auto& boundary : boundaries) { const auto value=resolver.Resolve(boundary.id,boundary.epoch);REQUIRE(value.bValid);row(rows,boundary.name,boundary.id,boundary.epoch,value); }
	require_offset(resolver,"America/New_York",epoch_us(2024,3,10,6,59,59),-18000);require_offset(resolver,"America/New_York",epoch_us(2024,3,10,7,0,0),-14400);
	require_offset(resolver,"America/Ciudad_Juarez",epoch_us(2024,3,10,8,59,59),-25200);require_offset(resolver,"America/Ciudad_Juarez",epoch_us(2024,3,10,9,0,0),-21600);
	require_offset(resolver,"Antarctica/Vostok",epoch_us(2023,12,17,18,59,59),25200);require_offset(resolver,"Antarctica/Vostok",epoch_us(2023,12,17,19,0,0),18000);
	require_offset(resolver,"Asia/Urumqi",winter,21600);require_offset(resolver,"Asia/Urumqi",summer,21600);require_offset(resolver,"Asia/Seoul",winter,32400);require_offset(resolver,"Asia/Seoul",summer,32400);

	const std::vector<std::string> invalidIds{"", "utc", "Asia/Seoul ", "Invalid/Zone", "UTC+9:00", "UTC+03:15", "UTC+15:00", "UTC+14:30", "UTC-00"};
	for (std::size_t index=0; index<invalidIds.size(); ++index) { const auto value=resolver.Resolve(invalidIds[index],winter);REQUIRE_FALSE(value.bValid);REQUIRE(value.sRequestedId==invalidIds[index]);REQUIRE(value.eError==domain::E_TIME_ZONE_RESOLUTION_ERROR::InvalidIdentifier);row(rows,"invalid-"+std::to_string(index),invalidIds[index],winter,value); }
	REQUIRE(rows.size() == 2 + 976 + 14 + invalidIds.size()); emit(rows);
}
