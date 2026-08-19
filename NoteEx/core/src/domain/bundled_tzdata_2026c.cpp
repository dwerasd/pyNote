#include "bundled_tzdata_2026c.h"

#include "cctz/zone_info_source.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <string>

namespace
{
	struct S_BUNDLED_ZONE_ENTRY
	{
		std::string_view sId;
		std::size_t nOffset;
		std::size_t nLength;
	};

#include "bundled_tzdata_2026c.inc"

	class C_MEMORY_ZONE_INFO_SOURCE final : public cctz::ZoneInfoSource
	{
	public:
		C_MEMORY_ZONE_INFO_SOURCE(const unsigned char* data, std::size_t size) : data_(data), size_(size) {}

		std::size_t Read(void* target, std::size_t size) override
		{
			const auto available = size_ - position_;
			const auto count = (std::min)(available, size);
			if (count != 0) { std::memcpy(target, data_ + position_, count); position_ += count; }
			return count;
		}

		int Skip(std::size_t offset) override
		{
			if (offset > size_ - position_) { return -1; }
			position_ += offset; return 0;
		}

		std::string Version() const override { return "2026c"; }

	private:
		const unsigned char* data_{};
		std::size_t size_{};
		std::size_t position_{};
	};

	std::unique_ptr<cctz::ZoneInfoSource> EmbeddedFactory(
		const std::string& name,
		const std::function<std::unique_ptr<cctz::ZoneInfoSource>(const std::string&)>&)
	{
		pynote::core::domain::detail::S_BUNDLED_TZDATA_VIEW view;
		if (!pynote::core::domain::detail::FindBundledTzdata(name, &view)) { return nullptr; }
		return std::make_unique<C_MEMORY_ZONE_INFO_SOURCE>(view.pData, view.nSize);
	}
}

namespace cctz_extension
{
	ZoneInfoSourceFactory zone_info_source_factory = EmbeddedFactory;
}

namespace pynote::core::domain::detail
{
	bool FindBundledTzdata(std::string_view _sId, S_BUNDLED_TZDATA_VIEW* _pOut) noexcept
	{
		if (_pOut == nullptr) { return false; }
		const auto begin = std::begin(kBundledZones);
		const auto end = std::end(kBundledZones);
		const auto found = std::lower_bound(begin, end, _sId,
			[](const S_BUNDLED_ZONE_ENTRY& entry, std::string_view id) { return entry.sId < id; });
		if (found == end || found->sId != _sId) { return false; }
		*_pOut = { kBundledTzdataBytes + found->nOffset, found->nLength };
		return true;
	}

	std::span<const std::string_view> BundledSupportedTimeZoneIds() noexcept
	{
		return kSupportedTimeZoneIds;
	}

	std::string_view BundledTzdataVersion() noexcept { return "2026c"; }
}
