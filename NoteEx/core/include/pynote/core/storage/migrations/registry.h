#pragma once

#include "pynote/core/storage/migration_runner.h"

#include <span>

// core 계층. Win32/WTL/DirectWrite 헤더를 포함하지 않는다(tools/gates 격리 게이트가 강제).

namespace pynote::core::storage::migrations
{
	// 파이썬 원본 migrations/__init__.py 의 MIGRATIONS 튜플 이식이다.
	// 등록 순서가 계약이다 - 러너의 pending 적용 순서가 이 목록 순서에 의존한다
	// (__init__.py:33~42, database.py:101~103).
	std::span<const S_MIGRATION> Registry() noexcept;

	// 원본 LATEST_SCHEMA_VERSION = MIGRATIONS[-1][0] 과 같은 값이다(__init__.py:44).
	// v10 은 없다 - 스키마 무변경이 설계 확정 사항이라 이 값의 상한은 9 다.
	int LatestSchemaVersion() noexcept;
}
