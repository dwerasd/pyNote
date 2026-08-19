#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace pynote::core::domain { class C_PARAGRAPH_PARSER; }

namespace pynote::core::application
{
	inline constexpr std::size_t MAX_IMPORT_FILE_BYTES = 4u * 1024u * 1024u;

	using LegacyDecoder = std::function<std::string(std::span<const std::uint8_t>)>;
	using BoundedFileReader = std::function<bool(
		const std::string&, std::size_t, std::vector<std::uint8_t>*, std::string*)>;

	enum class E_IMPORT_RESULT { Ok, ReadFailed, FileTooLarge, Blank };

	struct S_IMPORT_PREPARATION
	{
		std::string sPath;
		std::string sText;
	};

	std::string DecodeImportBytes(std::span<const std::uint8_t> _Bytes, const LegacyDecoder& _LegacyDecoder);

	class C_IMPORT_PIPELINE
	{
	public:
		C_IMPORT_PIPELINE(const domain::C_PARAGRAPH_PARSER& _Parser, LegacyDecoder _LegacyDecoder);

		E_IMPORT_RESULT PrepareFromBytes(
			const std::string& _sPath,
			std::span<const std::uint8_t> _Bytes,
			S_IMPORT_PREPARATION* _pOut) const;
		E_IMPORT_RESULT PrepareFile(
			const std::string& _sPath,
			const BoundedFileReader& _Reader,
			S_IMPORT_PREPARATION* _pOut,
			std::string* _psError) const;

	private:
		const domain::C_PARAGRAPH_PARSER& m_Parser;
		LegacyDecoder m_LegacyDecoder;
	};
}
