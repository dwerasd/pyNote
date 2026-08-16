#pragma once

#include "pynote/core/storage/migration_runner.h"

#include <cstdint>

// core 계층. Win32/WTL/DirectWrite 헤더를 포함하지 않는다(tools/gates 격리 게이트가 강제).

namespace pynote::core::storage::migrations::v0006
{
	// 파이썬 원본 migrations/v0006_editor_split_sizes.py 의 migrate 이식이다.
	// 문서 UI 상태에 편집 분할 보기 좌우 크기 열을 추가한다. 두 열 모두 nullable 이다.
	bool Migrate(C_DATABASE& _database, std::int64_t _nAppliedAtUs);
}
