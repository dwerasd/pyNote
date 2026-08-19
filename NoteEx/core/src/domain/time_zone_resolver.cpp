#include "pynote/core/domain/time_zone_resolver.h"

#include "bundled_tzdata_2026c.h"
#include "cctz/time_zone.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

namespace
{
	using namespace pynote::core::domain;

	std::optional<int> fixed_offset(std::string_view id)
	{
		if (id.size() != 9 || id.substr(0, 3) != "UTC" || (id[3] != '+' && id[3] != '-') ||
			id[6] != ':' || id[4] < '0' || id[4] > '9' || id[5] < '0' || id[5] > '9' ||
			id[7] < '0' || id[7] > '9' || id[8] < '0' || id[8] > '9') { return std::nullopt; }
		const auto& supported = C_TIME_ZONE_RESOLVER::SupportedTimeZoneIds();
		if (!std::binary_search(supported.begin(), supported.end(), id)) { return std::nullopt; }
		const int hours = (id[4] - '0') * 10 + id[5] - '0';
		const int minutes = (id[7] - '0') * 10 + id[8] - '0';
		const int seconds = hours * 3600 + minutes * 60;
		return id[3] == '-' ? -seconds : seconds;
	}

	S_TIME_ZONE_RESOLUTION invalid(std::string_view id, E_TIME_ZONE_RESOLUTION_ERROR error,
		E_TIME_ZONE_RESOLUTION_SOURCE source = E_TIME_ZONE_RESOLUTION_SOURCE::BundledTzdata)
	{
		S_TIME_ZONE_RESOLUTION result; result.sRequestedId = std::string(id);
		result.eError = error; result.eSource = source; return result;
	}

	S_TIME_ZONE_RESOLUTION lookup(std::string_view requested, std::string canonical,
		std::int64_t epochUs, const cctz::time_zone& zone, E_TIME_ZONE_RESOLUTION_SOURCE source,
		std::string version)
	{
		std::int64_t seconds = epochUs / 1'000'000;
		std::int64_t remainder = epochUs % 1'000'000;
		if (remainder < 0) { --seconds; remainder += 1'000'000; }
		const auto instant = cctz::time_point<cctz::seconds>(cctz::seconds(seconds));
		const auto value = zone.lookup(instant);
		S_TIME_ZONE_RESOLUTION result;
		result.sRequestedId = std::string(requested); result.sCanonicalId = std::move(canonical);
		result.sTzdataVersion = std::move(version); result.nYear = value.cs.year();
		result.nMonth = value.cs.month(); result.nDay = value.cs.day(); result.nHour = value.cs.hour();
		result.nMinute = value.cs.minute(); result.nSecond = value.cs.second();
		result.nMillisecond = static_cast<int>(remainder / 1000); result.nUtcOffsetSeconds = value.offset;
		result.bDst = value.is_dst; result.sAbbreviation = value.abbr == nullptr ? "" : value.abbr;
		result.eSource = source; result.eError = E_TIME_ZONE_RESOLUTION_ERROR::None; result.bValid = true;
		return result;
	}
}

namespace pynote::core::domain
{
	C_TIME_ZONE_RESOLVER::C_TIME_ZONE_RESOLVER(SystemTimeZoneResolver _SystemResolver)
		: m_SystemResolver(std::move(_SystemResolver)) {}

	S_TIME_ZONE_RESOLUTION C_TIME_ZONE_RESOLVER::Resolve(std::string_view _sRequestedId,
		std::int64_t _nEpochUs) const
	{
		if (_sRequestedId == "system") {
			if (!m_SystemResolver) {
				return invalid(_sRequestedId, E_TIME_ZONE_RESOLUTION_ERROR::UnavailableSystemProvider,
					E_TIME_ZONE_RESOLUTION_SOURCE::System);
			}
			auto result = m_SystemResolver(_nEpochUs);
			result.sRequestedId = std::string(_sRequestedId); result.eSource = E_TIME_ZONE_RESOLUTION_SOURCE::System;
			if (!result.bValid && result.eError == E_TIME_ZONE_RESOLUTION_ERROR::None) {
				result.eError = E_TIME_ZONE_RESOLUTION_ERROR::UnavailableSystemProvider;
			}
			return result;
		}
		if (_sRequestedId == "UTC") {
			return lookup(_sRequestedId, "UTC", _nEpochUs, cctz::utc_time_zone(),
				E_TIME_ZONE_RESOLUTION_SOURCE::Utc, "2026c");
		}
		if (const auto offset = fixed_offset(_sRequestedId)) {
			return lookup(_sRequestedId, std::string(_sRequestedId), _nEpochUs,
				cctz::fixed_time_zone(cctz::seconds(*offset)), E_TIME_ZONE_RESOLUTION_SOURCE::FixedOffset, "2026c");
		}
		const auto& supported = SupportedTimeZoneIds();
		if (!std::binary_search(supported.begin(), supported.end(), _sRequestedId)) {
			return invalid(_sRequestedId, E_TIME_ZONE_RESOLUTION_ERROR::InvalidIdentifier);
		}
		detail::S_BUNDLED_TZDATA_VIEW payload;
		if (!detail::FindBundledTzdata(_sRequestedId, &payload)) {
			return invalid(_sRequestedId, E_TIME_ZONE_RESOLUTION_ERROR::CorruptOrMissingBundledData);
		}
		cctz::time_zone zone;
		if (!cctz::load_time_zone(std::string(_sRequestedId), &zone)) {
			return invalid(_sRequestedId, E_TIME_ZONE_RESOLUTION_ERROR::CorruptOrMissingBundledData);
		}
		return lookup(_sRequestedId, std::string(_sRequestedId), _nEpochUs, zone,
			E_TIME_ZONE_RESOLUTION_SOURCE::BundledTzdata, std::string(detail::BundledTzdataVersion()));
	}

	const std::vector<std::string>& C_TIME_ZONE_RESOLVER::SupportedTimeZoneIds()
	{
		static const std::vector<std::string> ids = [] {
			const auto values = detail::BundledSupportedTimeZoneIds();
			return std::vector<std::string>(values.begin(), values.end());
		}();
		return ids;
	}
}
