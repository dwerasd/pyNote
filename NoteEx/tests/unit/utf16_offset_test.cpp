#include <catch_amalgamated.hpp>

#include "pynote/core/text/utf16_offset.h"

#pragma comment(lib, "NoteExCore")

namespace
{
	// 본문 바이트열은 소스 인코딩에 의존하지 않도록 16진 이스케이프로 고정한다.
	// MSVC 는 narrow 리터럴을 실행 문자 집합(CP949)으로 변환하므로, 한글을 그대로 적으면
	// UTF-8 이 아니라 CP949 바이트가 되어 이 시험의 전제가 깨진다. 비BMP 문자는 CP949 에
	// 대응이 없어 C4566 도 난다.
	constexpr const char* K_ASCII = "Hello";							// 5바이트 / 5단위
	constexpr const char* K_HANGUL = "\xEA\xB0\x80\xEB\x82\x98\xEB\x8B\xA4";	// "가나다" 9바이트 / 3단위
	constexpr const char* K_EMOJI = "a\xF0\x9F\x98\x80" "b";		// "a" + U+1F600 + "b" 6바이트 / 4단위
}

TEST_CASE("빈 본문은 어느 방향이든 0", "[core][text][utf16]")
{
	REQUIRE(pynote::core::text::utf8_index_to_utf16_offset("", 0) == 0);
	REQUIRE(pynote::core::text::utf8_index_to_utf16_offset("", 7) == 0);
	REQUIRE(pynote::core::text::utf16_offset_to_utf8_index("", 0) == 0);
	REQUIRE(pynote::core::text::utf16_offset_to_utf8_index("", 7) == 0);
}

TEST_CASE("ASCII 는 바이트와 코드 단위가 1대1", "[core][text][utf16]")
{
	for (std::size_t i = 0; i <= 5; ++i)
	{
		REQUIRE(pynote::core::text::utf8_index_to_utf16_offset(K_ASCII, i) == i);
		REQUIRE(pynote::core::text::utf16_offset_to_utf8_index(K_ASCII, i) == i);
	}
}

TEST_CASE("한글은 3바이트가 1 코드 단위", "[core][text][utf16]")
{
	REQUIRE(pynote::core::text::utf8_index_to_utf16_offset(K_HANGUL, 0) == 0);
	REQUIRE(pynote::core::text::utf8_index_to_utf16_offset(K_HANGUL, 3) == 1);
	REQUIRE(pynote::core::text::utf8_index_to_utf16_offset(K_HANGUL, 9) == 3);

	REQUIRE(pynote::core::text::utf16_offset_to_utf8_index(K_HANGUL, 0) == 0);
	REQUIRE(pynote::core::text::utf16_offset_to_utf8_index(K_HANGUL, 1) == 3);
	REQUIRE(pynote::core::text::utf16_offset_to_utf8_index(K_HANGUL, 3) == 9);
}

TEST_CASE("비BMP 문자는 UTF-16 에서 2 단위", "[core][text][utf16]")
{
	// "a"(1바이트/1단위) + U+1F600(4바이트/2단위) + "b"(1바이트/1단위)
	REQUIRE(pynote::core::text::utf8_index_to_utf16_offset(K_EMOJI, 1) == 1);
	REQUIRE(pynote::core::text::utf8_index_to_utf16_offset(K_EMOJI, 5) == 3);
	REQUIRE(pynote::core::text::utf8_index_to_utf16_offset(K_EMOJI, 6) == 4);

	REQUIRE(pynote::core::text::utf16_offset_to_utf8_index(K_EMOJI, 1) == 1);
	REQUIRE(pynote::core::text::utf16_offset_to_utf8_index(K_EMOJI, 3) == 5);
	REQUIRE(pynote::core::text::utf16_offset_to_utf8_index(K_EMOJI, 4) == 6);
}

TEST_CASE("문자 중간을 가리키면 그 문자의 시작으로 내린다", "[core][text][utf16]")
{
	// 한글 첫 글자 안쪽 바이트 1, 2 는 모두 문자 시작(0 단위)으로 내려간다.
	REQUIRE(pynote::core::text::utf8_index_to_utf16_offset(K_HANGUL, 1) == 0);
	REQUIRE(pynote::core::text::utf8_index_to_utf16_offset(K_HANGUL, 2) == 0);

	// 서로게이트 쌍 중간(오프셋 2)은 쌍의 시작 바이트 1 로 내려간다.
	REQUIRE(pynote::core::text::utf16_offset_to_utf8_index(K_EMOJI, 2) == 1);
}

TEST_CASE("범위를 넘는 입력은 끝으로 클램프", "[core][text][utf16]")
{
	REQUIRE(pynote::core::text::utf8_index_to_utf16_offset(K_HANGUL, 100) == 3);
	REQUIRE(pynote::core::text::utf16_offset_to_utf8_index(K_HANGUL, 100) == 9);
	REQUIRE(pynote::core::text::utf8_index_to_utf16_offset(K_EMOJI, 100) == 4);
	REQUIRE(pynote::core::text::utf16_offset_to_utf8_index(K_EMOJI, 100) == 6);
}

TEST_CASE("문자 경계에서는 두 함수가 서로의 역함수", "[core][text][utf16]")
{
	const char* pSamples[] = { K_ASCII, K_HANGUL, K_EMOJI };
	for (const char* pText : pSamples)
	{
		const std::string_view sText(pText);
		// 모든 바이트 위치에서 왕복시키면 문자 시작 위치로 정규화된 값이 나오고,
		// 그 값은 다시 왕복해도 변하지 않아야 한다.
		for (std::size_t i = 0; i <= sText.size(); ++i)
		{
			const std::size_t nUnits = pynote::core::text::utf8_index_to_utf16_offset(sText, i);
			const std::size_t nBack = pynote::core::text::utf16_offset_to_utf8_index(sText, nUnits);
			REQUIRE(pynote::core::text::utf8_index_to_utf16_offset(sText, nBack) == nUnits);
			REQUIRE(nBack <= i);
		}
	}
}
