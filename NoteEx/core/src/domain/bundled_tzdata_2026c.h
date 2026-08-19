#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace pynote::core::domain::detail
{
	struct S_BUNDLED_TZDATA_VIEW
	{
		const unsigned char* pData{};
		std::size_t nSize{};
	};

	bool FindBundledTzdata(std::string_view _sId, S_BUNDLED_TZDATA_VIEW* _pOut) noexcept;
	std::span<const std::string_view> BundledSupportedTimeZoneIds() noexcept;
	std::string_view BundledTzdataVersion() noexcept;
}
