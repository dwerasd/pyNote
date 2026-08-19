#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace pynote::core::domain
{
	struct S_DATE_TIME_VIEW
	{
		int nYear{ 0 };
		int nMonth{ 0 };
		int nDay{ 0 };
		int nHour{ 0 };
		int nMinute{ 0 };
		int nSecond{ 0 };
		int nMillisecond{ 0 };
		std::int32_t nUtcOffsetSeconds{ 0 };
		std::string sTimeZoneAbbreviation;
		std::string sTimeZoneLongName;
		bool bValid{ false };

		bool operator==(const S_DATE_TIME_VIEW&) const = default;
	};

	std::string FormatDateTime(const S_DATE_TIME_VIEW& _View, std::string_view _sFormat);
}
