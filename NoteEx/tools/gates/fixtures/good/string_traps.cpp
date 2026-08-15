// 문자열·문자 리터럴 안의 금지 include 는 위반이 아니다.

#include <string>

namespace pynote::core {

// 일반 문자열 리터럴
const char* kPlain = "#include <windows.h>";

// 이스케이프된 따옴표가 섞인 문자열
const char* kEscaped = "\"#include <atlwin.h>\" 는 문자열이다";

// 백슬래시로 이어지는 문자열
const char* kJoined = "앞줄 \
#include <dwrite.h> 뒷줄";

// 원시 문자열. 줄머리에 지시문 모양이 그대로 들어간다.
const char* kRaw = R"(
#include <windows.h>
#include "atlbase.h"
)";

// 사용자 지정 구분자를 쓴 원시 문자열
const char* kRawDelim = R"sql(
#include <d2d1.h>
)sql";

// 숫자 구분자. 문자 리터럴로 오인하면 이 줄 뒤가 통째로 삼켜진다.
const long kLimit = 1'000'000;
const char kQuote = '\'';
const char kHash = '#';

}  // namespace pynote::core
