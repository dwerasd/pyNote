#pragma once

#include "pynote/core/storage/migration_runner.h"

#include <cstdint>

// core 계층. Win32/WTL/DirectWrite 헤더를 포함하지 않는다(tools/gates 격리 게이트가 강제).

namespace pynote::core::storage::migrations::v0005
{
	// 파이썬 원본 migrations/v0005_recency_sort_mode.py 의 migrate 이식이다.
	// 문서 UI 정렬 상태에 최근 활동순을 추가하며 기존 값을 보존한다.
	// 개명 - 재생성 - 복사 - 삭제 네 단계이고 INSERT/SELECT 의 열 순서가 계약이다.
	bool Migrate(C_DATABASE& _database, std::int64_t _nAppliedAtUs);
}
