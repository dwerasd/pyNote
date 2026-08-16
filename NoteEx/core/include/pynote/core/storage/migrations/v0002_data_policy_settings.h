#pragma once

#include "pynote/core/storage/migration_runner.h"

#include <cstdint>

// core 계층. Win32/WTL/DirectWrite 헤더를 포함하지 않는다(tools/gates 격리 게이트가 강제).

namespace pynote::core::storage::migrations::v0002
{
	// 파이썬 원본 migrations/v0002_data_policy_settings.py 의 migrate 이식이다.
	// 장치와 공유되지 않는 데이터 운용 정책 단일 행을 추가한다.
	bool Migrate(C_DATABASE& _database, std::int64_t _nAppliedAtUs);
}
