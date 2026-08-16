#pragma once

#include "pynote/core/storage/migration_runner.h"

#include <cstdint>

// core 계층. Win32/WTL/DirectWrite 헤더를 포함하지 않는다(tools/gates 격리 게이트가 강제).

namespace pynote::core::storage::migrations::v0009
{
	// 파이썬 원본 migrations/v0009_preview_lines_default.py 의 migrate 이식이다.
	// 이전 기본 미리보기 줄 수 6을 새 기본값 3으로 한 번 정규화한다.
	// 6 이 아닌 값은 사용자가 고른 값이므로 건드리지 않는다.
	bool Migrate(C_DATABASE& _database, std::int64_t _nAppliedAtUs);
}
