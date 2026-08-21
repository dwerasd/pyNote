// 추적 ID 처럼 보이지만 문법에 어긋나는 표기들. 전건 "추적 주석이 없다" 로 걸려야 한다.
#include <catch_amalgamated.hpp>

TEST_CASE("WTL-W2-52 digits too short", "[unit]") { REQUIRE(true); }

TEST_CASE("w3-d8-001 lowercase id", "[unit]") { REQUIRE(true); }

TEST_CASE("prose then WTL-W2-0002 midway is not a title id", "[unit][core]") { REQUIRE(true); }

TEST_CASE("W2 plain milestone word", "[unit]") { REQUIRE(true); }

TEST_CASE("plain title with lookalike tag", "[Windows-10]") { REQUIRE(true); }

TEST_CASE("plain title with lowercase tag", "[w3-d8-settings]") { REQUIRE(true); }
