#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pynote::core::domain
{
	enum class E_DIFF_TAG { Equal, Insert, Delete, Replace };

	struct S_CHARACTER_DIFF
	{
		E_DIFF_TAG eTag{ E_DIFF_TAG::Equal };
		std::string sBefore;
		std::string sAfter;
		bool operator==(const S_CHARACTER_DIFF&) const = default;
	};

	struct S_LINE_DIFF
	{
		E_DIFF_TAG eTag{ E_DIFF_TAG::Equal };
		std::optional<std::size_t> nBeforeLineNumber{};
		std::optional<std::size_t> nAfterLineNumber{};
		std::string sBefore;
		std::string sAfter;
		std::vector<S_CHARACTER_DIFF> Characters;
		bool operator==(const S_LINE_DIFF&) const = default;
	};

	struct S_TEXT_DIFF
	{
		std::string sBefore;
		std::string sAfter;
		std::vector<S_LINE_DIFF> Lines;
		bool operator==(const S_TEXT_DIFF&) const = default;
	};

	std::string_view ToText(E_DIFF_TAG _eTag) noexcept;
	std::vector<S_CHARACTER_DIFF> DiffCharacters(std::string_view _sBefore, std::string_view _sAfter);
	S_TEXT_DIFF DiffText(std::string_view _sBefore, std::string_view _sAfter);
}
