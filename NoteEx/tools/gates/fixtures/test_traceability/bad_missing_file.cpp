// 결함 표본: 가리키는 파일 자체가 저장소에 없는 node ID.
//
// 파이썬 쪽 시험 파일을 개명하거나 합치면 이렇게 된다. C++ 은 컴파일되고 시험은
// 초록이므로 주석만 조용히 낡는다.

#include <catch2/catch_test_macros.hpp>

// 대응 원본: tests/integration/test_absent.py::test_alpha
TEST_CASE("없는 파일을 가리킨다", "[fixture]")
{
	REQUIRE(true);
}

// 파일 이름은 그럴듯하지만 디렉터리가 틀렸다.
// 대응 원본: tests/unit/test_sample.py::test_alpha
TEST_CASE("디렉터리가 틀린 경로를 가리킨다", "[fixture]")
{
	REQUIRE(true);
}
