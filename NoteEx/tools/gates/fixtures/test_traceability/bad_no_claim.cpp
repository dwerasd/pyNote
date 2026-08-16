// 결함 표본: 주석은 있는데 아무것도 주장하지 않는다.
//
// node ID 도 없고, 역보강 대기 선언도 없고, 부재 선언도 없다. 겉보기에는 규약을
// 지킨 것처럼 보이지만 역보강 시점에 쓸 것이 아무것도 없다. 침묵은 부재 선언이 아니다.

#include <catch2/catch_test_macros.hpp>

// 대응 원본: backup.py 의 create_database_backup 언저리.
TEST_CASE("설명만 있고 node ID 도 선언도 없다", "[fixture]")
{
	REQUIRE(true);
}

// 대응 원본: repositories.py:722~737 의 검증 네 갈래. 행 번호는 node ID 가 아니다.
TEST_CASE("행 번호만 적었다", "[fixture]")
{
	REQUIRE(true);
}

// 대응 원본: v0001_initial.py::migrate (:187~196) 를 이식한 자리다.
// 원본 모듈의 함수를 가리키는 표기는 pytest node ID 가 아니다 - `tests/` 로 시작하지
// 않으므로 저장소 루트에서 pytest 로 돌릴 수 없다.
TEST_CASE("원본 모듈 함수를 node ID 처럼 적었다", "[fixture]")
{
	REQUIRE(true);
}

// 주석 블록은 붙어 있지만 대응 원본 표기가 없다.
// 이 시험은 나중에 채운다.
TEST_CASE("대응 원본 표기 자체가 없다", "[fixture]")
{
	REQUIRE(true);
}
