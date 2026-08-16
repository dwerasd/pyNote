// 자기시험용 정상 표본. 이식 트리에 실제로 있는 일곱 가지 주석 배치를 한 파일에 모았다.
// 여기 있는 배치를 게이트가 하나라도 거절하면, 게이트가 관례를 추인하는 대신 새 관례를
// 강요하고 있다는 뜻이다.
//
// TEST_CASE 를 세는 훑기도 함께 시험한다. 아래에 주석·문자열·원시 문자열 안의
// TEST_CASE( 표기를 심어 두었고, 게이트가 세야 하는 것은 정확히 일곱 건이다.

#include <catch2/catch_test_macros.hpp>

/*
	블록 주석 안의 TEST_CASE("이것은 대상이 아니다", "[x]") 표기.
*/

namespace
{
	// 문자열 리터럴 안의 TEST_CASE( 표기. 따옴표와 역슬래시가 섞여 있다.
	constexpr const char* K_DECOY = "TEST_CASE(\"세면 안 된다\", \"[x]\")";
	// 원시 문자열 안에는 따옴표와 // 와 TEST_CASE( 가 전부 들어 있다.
	constexpr const char8_t* K_RAW_DECOY = u8R"SQL(
		SELECT "TEST_CASE(" FROM t -- // 여기도 아니다
		)SQL";
}

// 배치 1: 한 줄에 직접.
// 대응 원본: tests/integration/test_sample.py::test_alpha
TEST_CASE("한 줄 직접 표기", "[fixture]")
{
	REQUIRE(K_DECOY != nullptr);
}

// 배치 2: 설명은 대응 원본 줄에, node ID 는 별도 줄에.
// 대응 원본: sample.py 의 alpha 갈래(:10~20).
// pytest node ID: tests/integration/test_sample.py::test_beta
TEST_CASE("node ID 별도 줄 표기", "[fixture]")
{
	REQUIRE(K_RAW_DECOY != nullptr);
}

// 배치 3: 들여쓴 목록. 클래스 마디가 붙은 node ID 도 같은 목록에 섞인다.
// 대응 원본: tests/integration/test_sample.py::test_alpha
//            tests/integration/test_sample.py::test_beta
//            tests/integration/test_sample.py::TestGroup::test_gamma
TEST_CASE("들여쓴 목록 표기", "[fixture]")
{
	REQUIRE(true);
}

// 배치 4: 산문 괄호 안의 node ID 와, 앞 경로를 잇는 `::이름` 표기.
// 대응 원본: dataclass 동치 비교(tests/integration/test_sample.py::test_alpha 의 값 비교)
// 와 ::test_beta (sample.py:30~40).
TEST_CASE("산문 괄호와 경로 잇기 표기", "[fixture]")
{
	REQUIRE(true);
}

// 배치 5: 역보강 대기 선언. 파이썬 쪽에 대응 시험이 없다는 신고다.
// 대응 원본: sample.py 의 gamma 갈래(:50~60).
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("역보강 대기 선언", "[fixture]")
{
	REQUIRE(true);
}

// 배치 6: 부재 선언(콜론 있음).
// 대응 원본: 없음. 이식본에서 새로 생긴 계약이다. pytest node ID 는 존재하지 않는다.
TEST_CASE("부재 선언 - 콜론 있음", "[fixture]")
{
	REQUIRE(true);
}

// 배치 7: 부재 선언(콜론 없음). 트리에 이 표기도 있다.
// 대응 원본 없음. 반환값 오류 규약 때문에 이식에서 새로 생긴 경로다.
TEST_CASE("부재 선언 - 콜론 없음", "[fixture]")
{
	REQUIRE(true);
}
