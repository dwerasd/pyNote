// 결함 표본: 추적 주석이 아예 없는 TEST_CASE.
//
// 실제로 가장 자주 나는 실패다. 다음 달에 시험을 하나 더 붙이는 사람은 주석 규약을
// 모르고, 주석 없는 시험은 있는 시험과 겉보기가 같다.

#include <catch2/catch_test_macros.hpp>

// 대응 원본: tests/integration/test_sample.py::test_alpha
TEST_CASE("앞의 시험은 주석을 달고 있다", "[fixture]")
{
	REQUIRE(true);
}

TEST_CASE("뒤에 붙인 이 시험은 주석이 없다", "[fixture]")
{
	REQUIRE(true);
}

// 주석은 있지만 빈 줄로 끊겨 있다. 이 블록은 아래 시험의 것이 아니다.
// 대응 원본: tests/integration/test_sample.py::test_beta

TEST_CASE("빈 줄로 끊긴 주석은 붙은 것이 아니다", "[fixture]")
{
	REQUIRE(true);
}
