#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

// core 내부 구현 헤더. UTF-8 낱개 코덱을 담는다.
// 공개 헤더(include/pynote/core) 가 아니라 여기 있는 이유는 바이트 해석이 계층 계약이 아니라
// 구현 세부라서다. import_pipeline.cpp 의 익명 이름공간에 있던 셋을 그대로 옮겼을 뿐이고
// 동작은 한 글자도 바꾸지 않았다 - 가져오기 시험이 무수정으로 그 동작을 계속 지킨다.

namespace pynote::core::application::detail
{
	inline void append_utf8(std::uint32_t scalar, std::string* out)
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

	inline bool decode_one_utf8(std::span<const std::uint8_t> bytes, std::size_t* offset, std::uint32_t* scalar)
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

	inline bool strict_utf8(std::span<const std::uint8_t> bytes)
	{
		std::size_t offset = 0; std::uint32_t scalar = 0;
		while (offset < bytes.size()) {
			if (!decode_one_utf8(bytes, &offset, &scalar)) { return false; }
		}
		return true;
	}
}
