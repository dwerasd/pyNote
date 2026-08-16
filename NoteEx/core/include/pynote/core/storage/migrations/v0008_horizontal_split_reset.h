#pragma once

#include "pynote/core/storage/migration_runner.h"

#include <cstdint>

// core 계층. Win32/WTL/DirectWrite 헤더를 포함하지 않는다(tools/gates 격리 게이트가 강제).

namespace pynote::core::storage::migrations::v0008
{
	// 파이썬 원본 migrations/v0008_horizontal_split_reset.py 의 migrate 이식이다.
	// 세로 분할 시절에 저장된 편집 분할 크기를 비운다.
	// 기존 값은 (상단 슬롯, 하단 목록) 순서라 가로 배치에서 읽으면 어긋난다.
	// v0007 과 문장이 같은 것은 실수가 아니다 - 사유가 다를 뿐 같은 열을 되돌린다.
	bool Migrate(C_DATABASE& _database, std::int64_t _nAppliedAtUs);
}
