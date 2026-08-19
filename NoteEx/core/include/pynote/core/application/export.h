#pragma once

#include "pynote/core/domain/models.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace pynote::core::application
{
	enum class E_NEWLINE_FORMAT { Lf, Crlf };
	enum class E_EXPORT_RESULT { Ok, InvalidSuffix, WriteFailed };

	using DirectExportWriter = std::function<bool(
		const std::string&, const std::string&, std::string*)>;

	std::string_view NewlineCharacters(E_NEWLINE_FORMAT _eNewline) noexcept;
	std::vector<std::string> ActiveCardIdsInExportOrder(const std::vector<domain::S_CARD>& _Cards);
	std::string RenderCards(const std::vector<domain::S_CARD>& _Cards,
		E_NEWLINE_FORMAT _eNewline = E_NEWLINE_FORMAT::Lf);
	E_EXPORT_RESULT ExportCards(const std::string& _sPath,
		const std::vector<domain::S_CARD>& _Cards, E_NEWLINE_FORMAT _eNewline,
		const DirectExportWriter& _Writer, std::string* _psError);
}
