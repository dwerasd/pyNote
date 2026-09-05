#pragma once

#include "pynote/core/storage/migration_runner.h"

#include <cstdint>

// core 계층. Win32/WTL/DirectWrite 헤더를 포함하지 않는다(tools/gates 격리 게이트가 강제).

namespace pynote::core::storage::migrations::v0010
{
	// 파이썬 원본 migrations/v0010_card_file_bindings.py 의 migrate 이식이다.
	// 카드 한 장과 파일 한 개를 잇는 결속 표를 만든다. 되감기 시험이 최신 스키마 DB 의
	// schema_version 만 낮춘 뒤 재적용하므로 DDL 은 멱등이어야 한다.
	bool Migrate(C_DATABASE& _database, std::int64_t _nAppliedAtUs);
}
