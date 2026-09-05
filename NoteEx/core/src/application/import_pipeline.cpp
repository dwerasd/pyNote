#include "pynote/core/application/import_pipeline.h"

#include "pynote/core/domain/paragraph_parser.h"

#include "utf_codec_detail.h"

#include <utility>

namespace pynote::core::application
{
	namespace
	{
		using detail::append_utf8;
		using detail::decode_one_utf8;
		using detail::strict_utf8;

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
