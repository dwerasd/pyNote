#include "pynote/core/application/import_pipeline.h"

#include "pynote/core/domain/paragraph_parser.h"

#include <utility>

namespace pynote::core::application
{
	namespace
	{
		void append_utf8(std::uint32_t scalar, std::string* out)
		{
			if (scalar <= 0x7F) { out->push_back(static_cast<char>(scalar)); }
			else if (scalar <= 0x7FF) {
				out->push_back(static_cast<char>(0xC0 | (scalar >> 6)));
				out->push_back(static_cast<char>(0x80 | (scalar & 0x3F)));
			}
			else if (scalar <= 0xFFFF) {
				out->push_back(static_cast<char>(0xE0 | (scalar >> 12)));
				out->push_back(static_cast<char>(0x80 | ((scalar >> 6) & 0x3F)));
				out->push_back(static_cast<char>(0x80 | (scalar & 0x3F)));
			}
			else {
				out->push_back(static_cast<char>(0xF0 | (scalar >> 18)));
				out->push_back(static_cast<char>(0x80 | ((scalar >> 12) & 0x3F)));
				out->push_back(static_cast<char>(0x80 | ((scalar >> 6) & 0x3F)));
				out->push_back(static_cast<char>(0x80 | (scalar & 0x3F)));
			}
		}

		bool decode_one_utf8(std::span<const std::uint8_t> bytes, std::size_t* offset, std::uint32_t* scalar)
		{
			const std::size_t start = *offset;
			const std::uint8_t lead = bytes[start];
			if (lead < 0x80) { *scalar = lead; *offset = start + 1; return true; }
			std::size_t length = 0; std::uint32_t value = 0; std::uint32_t minimum = 0;
			if (lead >= 0xC2 && lead <= 0xDF) { length = 2; value = lead & 0x1F; minimum = 0x80; }
			else if (lead >= 0xE0 && lead <= 0xEF) { length = 3; value = lead & 0x0F; minimum = 0x800; }
			else if (lead >= 0xF0 && lead <= 0xF4) { length = 4; value = lead & 0x07; minimum = 0x10000; }
			else { return false; }
			if (start + length > bytes.size()) { return false; }
			for (std::size_t index = 1; index < length; ++index) {
				if ((bytes[start + index] & 0xC0) != 0x80) { return false; }
				value = (value << 6) | (bytes[start + index] & 0x3F);
			}
			if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) { return false; }
			*scalar = value; *offset = start + length; return true;
		}

		bool strict_utf8(std::span<const std::uint8_t> bytes)
		{
			std::size_t offset = 0; std::uint32_t scalar = 0;
			while (offset < bytes.size()) {
				if (!decode_one_utf8(bytes, &offset, &scalar)) { return false; }
			}
			return true;
		}

		std::string utf8_replace(std::span<const std::uint8_t> bytes)
		{
			std::string result; std::size_t offset = 0;
			while (offset < bytes.size()) {
				const std::size_t start = offset; std::uint32_t scalar = 0;
				if (decode_one_utf8(bytes, &offset, &scalar)) { append_utf8(scalar, &result); }
				else { append_utf8(0xFFFD, &result); offset = start + 1; }
			}
			return result;
		}

		std::string utf16_replace(std::span<const std::uint8_t> bytes, bool bigEndian)
		{
			auto unit = [&](std::size_t index) {
				return static_cast<std::uint16_t>(bigEndian
					? (static_cast<std::uint16_t>(bytes[index]) << 8) | bytes[index + 1]
					: bytes[index] | (static_cast<std::uint16_t>(bytes[index + 1]) << 8));
			};
			std::string result; std::size_t offset = 0;
			while (offset + 1 < bytes.size()) {
				const std::uint16_t first = unit(offset); offset += 2;
				if (first >= 0xD800 && first <= 0xDBFF) {
					if (offset + 1 < bytes.size()) {
						const std::uint16_t second = unit(offset);
						if (second >= 0xDC00 && second <= 0xDFFF) {
							offset += 2;
							append_utf8(0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00), &result);
							continue;
						}
					}
					append_utf8(0xFFFD, &result);
				}
				else if (first >= 0xDC00 && first <= 0xDFFF) { append_utf8(0xFFFD, &result); }
				else { append_utf8(first, &result); }
			}
			if (offset < bytes.size()) { append_utf8(0xFFFD, &result); }
			return result;
		}
	}

	std::string DecodeImportBytes(std::span<const std::uint8_t> _Bytes, const LegacyDecoder& _LegacyDecoder)
	{
		if (_Bytes.empty()) { return {}; }
		if (_Bytes.size() >= 3 && _Bytes[0] == 0xEF && _Bytes[1] == 0xBB && _Bytes[2] == 0xBF) {
			return utf8_replace(_Bytes.subspan(3));
		}
		if (_Bytes.size() >= 2 && _Bytes[0] == 0xFF && _Bytes[1] == 0xFE) {
			return utf16_replace(_Bytes.subspan(2), false);
		}
		if (_Bytes.size() >= 2 && _Bytes[0] == 0xFE && _Bytes[1] == 0xFF) {
			return utf16_replace(_Bytes.subspan(2), true);
		}
		if (strict_utf8(_Bytes)) {
			return std::string(reinterpret_cast<const char*>(_Bytes.data()), _Bytes.size());
		}
		return _LegacyDecoder(_Bytes);
	}

	C_IMPORT_PIPELINE::C_IMPORT_PIPELINE(const domain::C_PARAGRAPH_PARSER& _Parser,
		LegacyDecoder _LegacyDecoder)
		: m_Parser(_Parser), m_LegacyDecoder(std::move(_LegacyDecoder))
	{
	}

	E_IMPORT_RESULT C_IMPORT_PIPELINE::PrepareFromBytes(const std::string& _sPath,
		std::span<const std::uint8_t> _Bytes, S_IMPORT_PREPARATION* _pOut) const
	{
		const std::string text = DecodeImportBytes(_Bytes, m_LegacyDecoder);
		if (m_Parser.IsZeroParagraphInput(text)) { return E_IMPORT_RESULT::Blank; }
		_pOut->sPath = _sPath; _pOut->sText = text;
		return E_IMPORT_RESULT::Ok;
	}

	E_IMPORT_RESULT C_IMPORT_PIPELINE::PrepareFile(const std::string& _sPath,
		const BoundedFileReader& _Reader, S_IMPORT_PREPARATION* _pOut, std::string* _psError) const
	{
		std::vector<std::uint8_t> bytes;
		if (!_Reader(_sPath, MAX_IMPORT_FILE_BYTES + 1, &bytes, _psError)) { return E_IMPORT_RESULT::ReadFailed; }
		if (bytes.size() > MAX_IMPORT_FILE_BYTES) { return E_IMPORT_RESULT::FileTooLarge; }
		return this->PrepareFromBytes(_sPath, bytes, _pOut);
	}
}
