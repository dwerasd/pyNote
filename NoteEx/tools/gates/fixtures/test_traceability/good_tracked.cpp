// 대장 시대의 ID·태그 추적 표기를 모은 표본. 전건 주석 없이 통과해야 한다.
// 트리에 실재하는 여섯 계열: 대장 native_id / planned id / T4A uncertain /
// capability(WTL- 접두 포함) / 조각 계약 SC / 조각 계약 태그.
#include <catch_amalgamated.hpp>

TEST_CASE("WTL-W2-0052", WORKSPACE_TAGS("WTL-W2-0052")) { REQUIRE(true); }

TEST_CASE("PLAN-W3-0025 actual page opens saves and navigates", "[W3-shell-consumer]")
{
	REQUIRE(true);
}

// 대장 대조 표본 - register_sample.md 에 없는 ID 다. 대장 없이 돌면 통과,
// `--register` 를 주면 이 한 건만 걸려야 한다.
TEST_CASE("WTL-W2-9999", "[unit]") { REQUIRE(true); }

TEST_CASE("T4A-UNC-005", "[T4A-UNC-005][W2-CAP]") { REQUIRE(true); }

TEST_CASE("CAP-FI-026 both back paths keep rejected dirty editor", "[W3-shell-consumer]")
{
	REQUIRE(true);
}

// 추적 표지가 아닌 일반 설명 주석이 붙은 경우 - ID 추적이므로 표지를 요구하지 않는다.
TEST_CASE("W3-D8-001 LocalAppData path and seven defaults", "[W3-D8-settings]")
{
	REQUIRE(true);
}

TEST_CASE(
	"registry enforces one document owner and active transitions",  // 인자 속 꼬리 주석
	"[W3-multi-window-lifecycle]")
{
	REQUIRE(true);
}
