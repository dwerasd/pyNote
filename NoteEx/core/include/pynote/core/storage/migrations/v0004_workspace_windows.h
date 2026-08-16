#pragma once

#include "pynote/core/storage/migration_runner.h"

#include <cstdint>

// core 계층. Win32/WTL/DirectWrite 헤더를 포함하지 않는다(tools/gates 격리 게이트가 강제).

namespace pynote::core::storage::migrations::v0004
{
	// 파이썬 원본 migrations/v0004_workspace_windows.py 의 migrate 이식이다.
	// 단일 workspace 행을 복원 대상 창 행 집합으로 이관한다.
	// window_id 는 randomblob/random() 로 만드는 UUID 라 실행마다 값이 다르다.
	bool Migrate(C_DATABASE& _database, std::int64_t _nAppliedAtUs);
}
