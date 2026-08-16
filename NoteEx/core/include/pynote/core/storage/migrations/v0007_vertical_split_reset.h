#pragma once

#include "pynote/core/storage/migration_runner.h"

#include <cstdint>

// core 계층. Win32/WTL/DirectWrite 헤더를 포함하지 않는다(tools/gates 격리 게이트가 강제).

namespace pynote::core::storage::migrations::v0007
{
	// 파이썬 원본 migrations/v0007_vertical_split_reset.py 의 migrate 이식이다.
	// 가로 분할 시절에 저장된 편집 분할 크기를 비운다.
	// 기존 값은 (목록, 편집기) 순서라 세로 배치에서 그대로 읽으면 의미가 뒤집힌다.
	// v0008 과 문장이 같은 것은 실수가 아니다 - 사유가 다를 뿐 같은 열을 되돌린다.
	bool Migrate(C_DATABASE& _database, std::int64_t _nAppliedAtUs);
}
