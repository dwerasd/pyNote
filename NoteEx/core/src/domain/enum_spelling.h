#pragma once

#include <cstddef>
#include <string_view>

// core 내부 구현 헤더. 열거형 철자표를 양방향으로 훑는 도우미만 담는다.
// 공개 헤더(include/pynote/core/domain) 가 아니라 여기 있는 이유는 저장 철자 대조가
// 도메인 계약이 아니라 구현 세부라서다.

namespace pynote::core::domain::detail
{
	template <typename T_ENUM>
	struct S_SPELLING
	{
		std::string_view sText;
		T_ENUM           eValue;
	};

	// 표에 없는 값은 빈 문자열이다. 열거형 값이 표에서 빠지는 것은 구현 결함이므로
	// 여기서 기본 철자로 접지 않는다 - 그렇게 접으면 잘못된 값이 조용히 저장된다.
	template <typename T_ENUM, std::size_t N>
	std::string_view SpellingToText(const S_SPELLING<T_ENUM> (&_Table)[N], T_ENUM _eValue)
	{
		for (const S_SPELLING<T_ENUM>& Entry : _Table)
		{
			if (Entry.eValue == _eValue) { return(Entry.sText); }
		}
		return(std::string_view{});
	}

	template <typename T_ENUM, std::size_t N>
	bool SpellingFromText(const S_SPELLING<T_ENUM> (&_Table)[N], std::string_view _sText, T_ENUM* _peValue)
	{
		for (const S_SPELLING<T_ENUM>& Entry : _Table)
		{
			if (Entry.sText == _sText)
			{
				*_peValue = Entry.eValue;
				return(true);
			}
		}
		return(false);
	}
}
