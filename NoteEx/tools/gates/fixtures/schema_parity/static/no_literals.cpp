// 추출 대상 0건 fixture.
// SQL 구분자를 쓴 원시 문자열이 하나도 없다. 이 입력에서 게이트는 "통과"가 아니라
// 환경 오류(종료 코드 2)를 내야 한다 - 대조할 것이 없는 게이트는 아무것도
// 증명하지 못하기 때문이다. 주석과 일반 문자열에 표기만 있는 함정을 함께 담는다.

namespace fixture::no_literals
{
	// R"SQL( 는 주석 안이라 추출 대상이 아니다.
	static const char* const g_szTrap = "R\"SQL(문자열이다)SQL\"";

	static const char* const g_szOther = R"NOTE(
    CREATE TABLE not_extracted (
        id TEXT PRIMARY KEY
    )
    )NOTE";
}
