#include "pynote/core/domain/paragraph_parser.h"

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace pynote::core::domain
{
	namespace
	{
		std::uint32_t next_scalar(std::string_view _sText, std::size_t* _pnOffset)
		{
			const auto lead = static_cast<unsigned char>(_sText[*_pnOffset]);
			if (lead < 0x80) {
				++(*_pnOffset);
				return lead;
			}

			std::size_t nLength = 0;
			std::uint32_t nScalar = 0;
			if ((lead & 0xE0) == 0xC0) {
				nLength = 2;
				nScalar = lead & 0x1F;
			}
			else if ((lead & 0xF0) == 0xE0) {
				nLength = 3;
				nScalar = lead & 0x0F;
			}
			else {
				nLength = 4;
				nScalar = lead & 0x07;
			}

			for (std::size_t nIndex = 1; nIndex < nLength; ++nIndex) {
				nScalar = (nScalar << 6)
					| (static_cast<unsigned char>(_sText[*_pnOffset + nIndex]) & 0x3F);
			}
			*_pnOffset += nLength;
			return nScalar;
		}

		bool is_python_whitespace(std::uint32_t _nScalar) noexcept
		{
			return (_nScalar >= 0x0009 && _nScalar <= 0x000D)
				|| (_nScalar >= 0x001C && _nScalar <= 0x001F)
				|| _nScalar == 0x0020
				|| _nScalar == 0x0085
				|| _nScalar == 0x00A0
				|| _nScalar == 0x1680
				|| (_nScalar >= 0x2000 && _nScalar <= 0x200A)
				|| _nScalar == 0x2028
				|| _nScalar == 0x2029
				|| _nScalar == 0x202F
				|| _nScalar == 0x205F
				|| _nScalar == 0x3000;
		}

		bool is_blank_line(std::string_view _sLine)
		{
			std::size_t nOffset = 0;
			while (nOffset < _sLine.size()) {
				if (!is_python_whitespace(next_scalar(_sLine, &nOffset))) {
					return false;
				}
			}
			return true;
		}

		std::string normalize_crlf(std::string_view _sText)
		{
			std::string sNormalized;
			sNormalized.reserve(_sText.size());
			for (std::size_t nIndex = 0; nIndex < _sText.size(); ++nIndex) {
				if (_sText[nIndex] == '\r' && nIndex + 1 < _sText.size() && _sText[nIndex + 1] == '\n') {
					sNormalized.push_back('\n');
					++nIndex;
				}
				else {
					sNormalized.push_back(_sText[nIndex]);
				}
			}
			return sNormalized;
		}
	}

	std::vector<std::string> C_BLANK_LINE_PARAGRAPH_POLICY::Split(std::string_view _sText) const
	{
		const std::string sNormalized = normalize_crlf(_sText);
		std::vector<std::string> Paragraphs;
		std::string sCurrent;
		std::size_t nLineStart = 0;

		while (nLineStart <= sNormalized.size()) {
			const std::size_t nLineEnd = sNormalized.find('\n', nLineStart);
			const std::size_t nEnd = nLineEnd == std::string::npos ? sNormalized.size() : nLineEnd;
			const std::string_view sLine(sNormalized.data() + nLineStart, nEnd - nLineStart);

			if (is_blank_line(sLine)) {
				if (!sCurrent.empty()) {
					Paragraphs.push_back(std::move(sCurrent));
					sCurrent.clear();
				}
			}
			else {
				if (!sCurrent.empty()) {
					sCurrent.push_back('\n');
				}
				sCurrent.append(sLine);
			}

			if (nLineEnd == std::string::npos) {
				break;
			}
			nLineStart = nLineEnd + 1;
		}

		if (!sCurrent.empty()) {
			Paragraphs.push_back(std::move(sCurrent));
		}
		return Paragraphs;
	}

	C_PARAGRAPH_PARSER::C_PARAGRAPH_PARSER()
		: m_pPolicy(std::make_shared<C_BLANK_LINE_PARAGRAPH_POLICY>())
	{
	}

	C_PARAGRAPH_PARSER::C_PARAGRAPH_PARSER(std::shared_ptr<const I_PARAGRAPH_SPLIT_POLICY> _pPolicy)
		: m_pPolicy(std::move(_pPolicy))
	{
		if (!m_pPolicy) {
			throw std::invalid_argument("paragraph split policy is null");
		}
	}

	std::vector<std::string> C_PARAGRAPH_PARSER::Split(std::string_view _sText) const
	{
		return m_pPolicy->Split(_sText);
	}

	bool C_PARAGRAPH_PARSER::IsZeroParagraphInput(std::string_view _sText) const
	{
		return this->Split(_sText).empty();
	}

	std::string C_PARAGRAPH_PARSER::Keep(std::string_view _sText) const
	{
		return std::string(_sText);
	}
}
