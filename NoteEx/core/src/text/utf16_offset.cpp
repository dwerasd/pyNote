#include "pynote/core/text/utf16_offset.h"

namespace
{
	// UTF-8 선두 바이트에서 시퀀스 길이를 얻는다.
	// 불정 바이트는 1 로 다룬다 - 손상된 본문에서도 전진을 보장해 루프가 멈추지 않게 한다.
	std::size_t sequence_length(unsigned char _byLead)
	{
		if (_byLead < 0x80) { return(1); }
		if ((_byLead & 0xE0) == 0xC0) { return(2); }
		if ((_byLead & 0xF0) == 0xE0) { return(3); }
		if ((_byLead & 0xF8) == 0xF0) { return(4); }
		return(1);
	}

	// UTF-8 4바이트 시퀀스만 BMP 밖이라 UTF-16 에서 서로게이트 쌍 2 단위가 된다.
	std::size_t utf16_units(std::size_t _nSequenceLength)
	{
		return((_nSequenceLength == 4) ? 2 : 1);
	}
}

namespace pynote::core::text
{
	std::size_t utf8_index_to_utf16_offset(std::string_view _sText, std::size_t _nByteIndex)
	{
		const std::size_t nLimit = (_nByteIndex < _sText.size()) ? _nByteIndex : _sText.size();

		std::size_t nByte = 0;
		std::size_t nUnits = 0;
		while (nByte < nLimit)
		{
			const std::size_t nSeq = sequence_length(static_cast<unsigned char>(_sText[nByte]));
			// 경계를 넘어서는 시퀀스는 세지 않는다 - 문자 중간 입력을 시작으로 내리는 지점이다.
			if (nByte + nSeq > nLimit) { break; }

			nByte += nSeq;
			nUnits += utf16_units(nSeq);
		}

		return(nUnits);
	}

	std::size_t utf16_offset_to_utf8_index(std::string_view _sText, std::size_t _nOffset)
	{
		std::size_t nByte = 0;
		std::size_t nUnits = 0;
		while (nByte < _sText.size() && nUnits < _nOffset)
		{
			const std::size_t nSeq = sequence_length(static_cast<unsigned char>(_sText[nByte]));
			if (nByte + nSeq > _sText.size()) { break; }	// 잘린 시퀀스 - 여기서 멈춘다

			const std::size_t nAdd = utf16_units(nSeq);
			// 서로게이트 쌍 중간을 가리키는 오프셋은 쌍의 시작으로 내린다.
			if (nUnits + nAdd > _nOffset) { break; }

			nByte += nSeq;
			nUnits += nAdd;
		}

		return(nByte);
	}
}
