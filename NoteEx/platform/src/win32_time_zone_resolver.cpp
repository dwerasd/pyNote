#include "pynote/platform/win32_time_zone_resolver.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cwchar>
#include <limits>
#include <string>
#include <utility>

namespace
{
	using namespace pynote::core::domain;

	S_TIME_ZONE_RESOLUTION failure(E_TIME_ZONE_RESOLUTION_ERROR error)
	{
		S_TIME_ZONE_RESOLUTION result; result.sRequestedId = "system";
		result.eSource = E_TIME_ZONE_RESOLUTION_SOURCE::System; result.eError = error; return result;
	}

	bool utf8(const wchar_t* value, std::string* output)
	{
		if (value == nullptr || output == nullptr) { return false; }
		const int length = static_cast<int>(wcslen(value));
		if (length == 0) { output->clear(); return true; }
		const int required = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, length,
			nullptr, 0, nullptr, nullptr);
		if (required <= 0) { return false; }
		output->resize(static_cast<std::size_t>(required));
		return ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, length,
			output->data(), required, nullptr, nullptr) == required;
	}

	std::uint64_t filetime_ticks(const FILETIME& value) noexcept
	{
		return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
	}
}

namespace pynote::platform
{
	core::domain::S_TIME_ZONE_RESOLUTION ResolveSystemTimeZone(std::int64_t _nEpochUs)
	{
		constexpr std::int64_t EpochDeltaUs = 11'644'473'600'000'000;
		constexpr std::int64_t MaximumEpochUs =
			static_cast<std::int64_t>((std::numeric_limits<std::uint64_t>::max)() / 10) - EpochDeltaUs;
		if (_nEpochUs < -EpochDeltaUs || _nEpochUs > MaximumEpochUs) {
			return failure(core::domain::E_TIME_ZONE_RESOLUTION_ERROR::OutOfRangeInstant);
		}
		const auto ticks = static_cast<std::uint64_t>(_nEpochUs + EpochDeltaUs) * 10;
		FILETIME utcFile{ static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32) };
		SYSTEMTIME utc{};
		if (!::FileTimeToSystemTime(&utcFile, &utc)) {
			return failure(core::domain::E_TIME_ZONE_RESOLUTION_ERROR::OutOfRangeInstant);
		}
		DYNAMIC_TIME_ZONE_INFORMATION dynamic{};
		if (::GetDynamicTimeZoneInformation(&dynamic) == TIME_ZONE_ID_INVALID) {
			return failure(core::domain::E_TIME_ZONE_RESOLUTION_ERROR::UnavailableSystemProvider);
		}
		SYSTEMTIME local{};
		if (!::SystemTimeToTzSpecificLocalTimeEx(&dynamic, &utc, &local)) {
			return failure(core::domain::E_TIME_ZONE_RESOLUTION_ERROR::UnavailableSystemProvider);
		}
		FILETIME localAsUtc{};
		if (!::SystemTimeToFileTime(&local, &localAsUtc)) {
			return failure(core::domain::E_TIME_ZONE_RESOLUTION_ERROR::OutOfRangeInstant);
		}
		const auto localTicks = filetime_ticks(localAsUtc);
		const auto offsetTicks = localTicks >= ticks
			? static_cast<std::int64_t>(localTicks - ticks)
			: -static_cast<std::int64_t>(ticks - localTicks);
		const auto offsetSeconds = static_cast<std::int32_t>(offsetTicks / 10'000'000);
		TIME_ZONE_INFORMATION yearly{};
		if (!::GetTimeZoneInformationForYear(local.wYear, &dynamic, &yearly)) {
			return failure(core::domain::E_TIME_ZONE_RESOLUTION_ERROR::UnavailableSystemProvider);
		}
		const auto standardOffset = -(yearly.Bias + yearly.StandardBias) * 60;
		std::string canonical;
		if (!utf8(dynamic.TimeZoneKeyName, &canonical)) {
			return failure(core::domain::E_TIME_ZONE_RESOLUTION_ERROR::UnavailableSystemProvider);
		}
		std::string abbreviation;
		if (!utf8(offsetSeconds == standardOffset ? yearly.StandardName : yearly.DaylightName, &abbreviation)) {
			return failure(core::domain::E_TIME_ZONE_RESOLUTION_ERROR::UnavailableSystemProvider);
		}
		core::domain::S_TIME_ZONE_RESOLUTION result;
		result.sRequestedId = "system"; result.sCanonicalId = canonical.empty() ? "system" : std::move(canonical);
		result.nYear = local.wYear; result.nMonth = local.wMonth; result.nDay = local.wDay;
		result.nHour = local.wHour; result.nMinute = local.wMinute; result.nSecond = local.wSecond;
		result.nMillisecond = local.wMilliseconds; result.nUtcOffsetSeconds = offsetSeconds;
		result.bDst = offsetSeconds != standardOffset; result.sAbbreviation = std::move(abbreviation);
		result.eSource = core::domain::E_TIME_ZONE_RESOLUTION_SOURCE::System;
		result.eError = core::domain::E_TIME_ZONE_RESOLUTION_ERROR::None; result.bValid = true;
		return result;
	}

	core::domain::SystemTimeZoneResolver MakeWin32SystemTimeZoneResolver()
	{
		return [](std::int64_t epochUs) { return ResolveSystemTimeZone(epochUs); };
	}
}
