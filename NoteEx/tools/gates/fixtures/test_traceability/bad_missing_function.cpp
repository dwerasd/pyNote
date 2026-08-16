// 결함 표본: 파일은 실재하는데 그 이름의 시험 함수가 없는 node ID.
//
// 주석이 없는 것보다 나쁘다. 역보강 때는 이 문자열을 믿고 대장에 옮겨 적을 텐데,
// 대장 한 줄이 통째로 거짓이 되고 아무도 그것을 다시 확인하지 않는다.

#include <catch2/catch_test_macros.hpp>

// 대응 원본: tests/integration/test_sample.py::test_delta
TEST_CASE("없는 시험 함수를 가리킨다", "[fixture]")
{
	REQUIRE(true);
}

// 이름이 파일 안에 문자열로는 나오지만 정의는 아니다. 문자열 검색으로 해소하는
// 게이트라면 이 줄을 통과시킨다.
// 대응 원본: tests/integration/test_sample.py::test_ghost
TEST_CASE("정의가 아니라 언급된 이름을 가리킨다", "[fixture]")
{
	REQUIRE(true);
}

// 클래스 마디가 붙었지만 그 클래스에 그런 메서드가 없다.
// 대응 원본: tests/integration/test_sample.py::TestGroup::test_delta
TEST_CASE("없는 메서드를 가리킨다", "[fixture]")
{
	REQUIRE(true);
}
