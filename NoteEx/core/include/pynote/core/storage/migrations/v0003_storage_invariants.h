#pragma once

#include "pynote/core/storage/migration_runner.h"

#include <cstdint>

// core 계층. Win32/WTL/DirectWrite 헤더를 포함하지 않는다(tools/gates 격리 게이트가 강제).

namespace pynote::core::storage::migrations::v0003
{
	// 파이썬 원본 migrations/v0003_storage_invariants.py 의 migrate 이식이다.
	// 카드와 리비전 교차 불변조건, capture counter 단조성을 트리거로 강제한다.
	// 원본은 DDL 앞에서 기존 행을 검사해 위반이 있으면 예외를 올린다 - 이식은 false 다.
	bool Migrate(C_DATABASE& _database, std::int64_t _nAppliedAtUs);
}
