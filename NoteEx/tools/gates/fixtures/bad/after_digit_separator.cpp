// C++14 숫자 구분자를 문자 리터럴 시작으로 오인하면 아래 include 가 통째로 삼켜진다.
// kMask 쪽은 어포스트로피가 홀수 개라, 리터럴 상태를 줄 끝에서 풀지 않는 스캐너는
// 파일 나머지를 전부 문자 리터럴로 먹어 버린다.
#include <cstdint>

namespace pynote::core {

const long kBudget = 1'000'000;
const std::uint32_t kMask = 0xFF'FF;

}  // namespace pynote::core

#include <objbase.h>
