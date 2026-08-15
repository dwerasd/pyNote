#pragma once

#include <cstddef>
#include <string_view>

// core 계층. Win32/WTL/DirectWrite 헤더를 포함하지 않는다(tools/gates 격리 게이트가 강제).

namespace pynote::core::text
{
	// 영속 커서 좌표와 core 내부 본문 사이의 변환.
	//
	// 계약(설계 D1): core 본문은 UTF-8 이고, 저장 스키마의 커서 열
	// (drafts.cursor_position_qchar / document_ui_states.editor_cursor_qchar)은
	// 기존 값 그대로 UTF-16 코드 단위를 유지한다. 두 표현의 변환은 영속 경계의
	// 이 함수 한 쌍으로만 한다 - 다른 곳에서 좌표를 환산하면 계약이 흩어진다.
	//
	// 두 함수 모두 범위를 벗어난 입력을 끝으로 클램프하고, 문자 경계 중간을
	// 가리키는 입력은 그 문자의 시작으로 내림한다. 커서 좌표는 사용자 DB 에서
	// 들어오는 외부 입력이므로 경계 처리를 여기서 끝낸다.

	// UTF-8 바이트 인덱스 -> UTF-16 코드 단위 오프셋.
	std::size_t utf8_index_to_utf16_offset(std::string_view _sText, std::size_t _nByteIndex);

	// UTF-16 코드 단위 오프셋 -> UTF-8 바이트 인덱스.
	// 서로게이트 쌍 중간을 가리키면 그 쌍의 시작 바이트를 돌려준다.
	std::size_t utf16_offset_to_utf8_index(std::string_view _sText, std::size_t _nOffset);
}
