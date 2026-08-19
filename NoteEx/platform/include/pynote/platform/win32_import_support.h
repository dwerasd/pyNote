#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pynote::platform
{
	bool ReadFileBounded(
		const std::string& _sUtf8Path,
		std::size_t _nMaximumBytes,
		std::vector<std::uint8_t>* _pOut,
		std::string* _psError);
	std::string DecodeSystemAnsi(std::span<const std::uint8_t> _Bytes);
	std::string DecodeWindowsCodePage(std::span<const std::uint8_t> _Bytes, unsigned int _nCodePage);
}
