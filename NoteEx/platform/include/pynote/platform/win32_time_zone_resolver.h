#pragma once

#include "pynote/core/domain/time_zone_resolver.h"

#include <cstdint>

namespace pynote::platform
{
	core::domain::S_TIME_ZONE_RESOLUTION ResolveSystemTimeZone(std::int64_t _nEpochUs);
	core::domain::SystemTimeZoneResolver MakeWin32SystemTimeZoneResolver();
}
