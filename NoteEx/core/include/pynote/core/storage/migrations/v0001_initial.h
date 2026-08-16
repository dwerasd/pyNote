#pragma once

#include "pynote/core/storage/migration_runner.h"

#include <cstdint>

// core 계층. Win32/WTL/DirectWrite 헤더를 포함하지 않는다(tools/gates 격리 게이트가 강제).

namespace pynote::core::storage::migrations::v0001
{
	// 파이썬 원본 migrations/v0001_initial.py 의 migrate 이식이다.
	// 스키마 문장은 원본 STATEMENTS 튜플을 공백까지 그대로 옮긴 것이며 정리·개선은 계약 위반이다.
	bool Migrate(C_DATABASE& _database, std::int64_t _nAppliedAtUs);
}
