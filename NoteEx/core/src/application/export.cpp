#include "pynote/core/application/export.h"

#include <algorithm>
#include <cctype>

namespace pynote::core::application
{
	namespace
	{
		std::vector<const domain::S_CARD*> ordered_active(const std::vector<domain::S_CARD>& cards)
		{
			std::vector<const domain::S_CARD*> result;
			for (const auto& card : cards) { if (!card.nDeletedAtUs.has_value()) { result.push_back(&card); } }
			std::sort(result.begin(), result.end(), [](const auto* left, const auto* right) {
				return left->nPositionKey != right->nPositionKey
					? left->nPositionKey < right->nPositionKey : left->sId < right->sId;
			});
			return result;
		}

		std::string convert_newlines(std::string_view text, std::string_view newline)
		{
			std::string normalized; normalized.reserve(text.size());
			for (std::size_t index=0; index<text.size(); ++index) {
				if (text[index]=='\r') {
					if (index+1<text.size() && text[index+1]=='\n') { ++index; }
					normalized.push_back('\n');
				}
				else { normalized.push_back(text[index]); }
			}
			if (newline == "\n") { return normalized; }
			std::string result; result.reserve(normalized.size());
			for (const char value : normalized) { if (value=='\n') { result += newline; } else { result.push_back(value); } }
			return result;
		}

		bool valid_suffix(std::string_view path)
		{
			const std::size_t separator = path.find_last_of("/\\");
			const std::size_t dot = path.find_last_of('.');
			if (dot == std::string_view::npos || (separator != std::string_view::npos && dot < separator)) { return false; }
			std::string suffix(path.substr(dot));
			std::transform(suffix.begin(),suffix.end(),suffix.begin(),[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
			return suffix == ".txt" || suffix == ".md";
		}
	}

	std::string_view NewlineCharacters(E_NEWLINE_FORMAT newline) noexcept
	{
		return newline == E_NEWLINE_FORMAT::Lf ? "\n" : "\r\n";
	}

	std::vector<std::string> ActiveCardIdsInExportOrder(const std::vector<domain::S_CARD>& cards)
	{
		std::vector<std::string> result;
		for (const auto* card : ordered_active(cards)) { result.push_back(card->sId); }
		return result;
	}

	std::string RenderCards(const std::vector<domain::S_CARD>& cards, E_NEWLINE_FORMAT newline)
	{
		const std::string_view characters = NewlineCharacters(newline);
		const std::string separator = std::string(characters) + std::string(characters);
		std::string result; bool first = true;
		for (const auto* card : ordered_active(cards)) {
			if (!first) { result += separator; }
			result += convert_newlines(card->sBody, characters);
			first = false;
		}
		return result;
	}

	E_EXPORT_RESULT ExportCards(const std::string& path, const std::vector<domain::S_CARD>& cards,
		E_NEWLINE_FORMAT newline, const DirectExportWriter& writer, std::string* error)
	{
		if (!valid_suffix(path)) { if (error != nullptr) { *error = "invalid-suffix"; } return E_EXPORT_RESULT::InvalidSuffix; }
		const std::string content = RenderCards(cards,newline);
		if (!writer(path,content,error)) { return E_EXPORT_RESULT::WriteFailed; }
		return E_EXPORT_RESULT::Ok;
	}
}
