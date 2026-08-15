// 허용되는 include 만 모아 둔 헤더. 표준 라이브러리·SQLite·프로젝트 헤더는 통과한다.
#pragma once

#include <sqlite3.h>
#include <sqlite3ext.h>
#  include <vector>
#	include <string>
	#include <memory>
#include "pynote/core/note.h"
#include "../core/card_id.h"

namespace pynote::core {

struct AllowedFixture {
    std::vector<std::string> tags;
};

}  // namespace pynote::core
