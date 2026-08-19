#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace pynote::core::domain
{
	enum class E_TIME_ZONE_RESOLUTION_SOURCE { System, Utc, FixedOffset, BundledTzdata };
	enum class E_TIME_ZONE_RESOLUTION_ERROR
	{
		None,
		InvalidIdentifier,
		UnavailableSystemProvider,
		OutOfRangeInstant,
		CorruptOrMissingBundledData
	};

	struct S_TIME_ZONE_RESOLUTION
	{
		std::string sRequestedId;
		std::string sCanonicalId;
		std::string sTzdataVersion;
		std::int64_t nYear{};
		int nMonth{};
		int nDay{};
		int nHour{};
		int nMinute{};
		int nSecond{};
		int nMillisecond{};
		std::int32_t nUtcOffsetSeconds{};
		bool bDst{};
		std::string sAbbreviation;
		E_TIME_ZONE_RESOLUTION_SOURCE eSource{ E_TIME_ZONE_RESOLUTION_SOURCE::BundledTzdata };
		E_TIME_ZONE_RESOLUTION_ERROR eError{ E_TIME_ZONE_RESOLUTION_ERROR::InvalidIdentifier };
		bool bValid{};
	};

	using SystemTimeZoneResolver = std::function<S_TIME_ZONE_RESOLUTION(std::int64_t)>;

	class C_TIME_ZONE_RESOLVER
	{
	public:
		explicit C_TIME_ZONE_RESOLVER(SystemTimeZoneResolver _SystemResolver = {});
		S_TIME_ZONE_RESOLUTION Resolve(std::string_view _sRequestedId, std::int64_t _nEpochUs) const;
		static const std::vector<std::string>& SupportedTimeZoneIds();

	private:
		SystemTimeZoneResolver m_SystemResolver;
	};
}
