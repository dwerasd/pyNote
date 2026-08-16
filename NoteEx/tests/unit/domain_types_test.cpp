#include <catch_amalgamated.hpp>

#include "pynote/core/domain/events.h"
#include "pynote/core/domain/models.h"

#include <string>
#include <string_view>

#pragma comment(lib, "NoteExCore")

namespace
{
	namespace domain = pynote::core::domain;

	// 철자와 열거값을 한 쌍씩 왕복시킨다. 저장 철자가 곧 CHECK 제약이 받는 값이라
	// 한 글자만 틀려도 INSERT 자체가 거부되므로 여기서 전건 대조한다.
	template <typename T_ENUM>
	void check_round_trip(std::string_view _sText, T_ENUM _eValue)
	{
		INFO("철자: " << std::string(_sText));
		REQUIRE(domain::ToText(_eValue) == _sText);

		T_ENUM eParsed{};
		REQUIRE(domain::FromText(_sText, &eParsed));
		REQUIRE(eParsed == _eValue);
	}
}

// 대응 원본: src/pynote/domain/events.py 의 EventType(:7~14).
// 파이썬 시험 트리에 철자 자체를 확인하는 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("EventType 철자는 저장 값 그대로다", "[core][domain]")
{
	check_round_trip("create", domain::E_EVENT_TYPE::Create);
	check_round_trip("update", domain::E_EVENT_TYPE::Update);
	check_round_trip("move", domain::E_EVENT_TYPE::Move);
	check_round_trip("split", domain::E_EVENT_TYPE::Split);
	check_round_trip("merge", domain::E_EVENT_TYPE::Merge);
	check_round_trip("delete", domain::E_EVENT_TYPE::Delete);
	check_round_trip("restore", domain::E_EVENT_TYPE::Restore);
}

// 대응 원본: src/pynote/domain/events.py 의 EventSource(:17~24).
// pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("EventSource 철자는 저장 값 그대로다", "[core][domain]")
{
	check_round_trip("typing", domain::E_EVENT_SOURCE::Typing);
	check_round_trip("paste", domain::E_EVENT_SOURCE::Paste);
	check_round_trip("import", domain::E_EVENT_SOURCE::Import);
	check_round_trip("mixed", domain::E_EVENT_SOURCE::Mixed);
	check_round_trip("edit", domain::E_EVENT_SOURCE::Edit);
	check_round_trip("restore", domain::E_EVENT_SOURCE::Restore);
	check_round_trip("system", domain::E_EVENT_SOURCE::System);
}

// 대응 원본: src/pynote/domain/models.py 의 여섯 StrEnum(:9~49).
// pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("models 열거형 철자는 저장 값 그대로다", "[core][domain]")
{
	check_round_trip("typing", domain::E_CAPTURE_OPERATION_SOURCE::Typing);
	check_round_trip("paste", domain::E_CAPTURE_OPERATION_SOURCE::Paste);
	check_round_trip("import", domain::E_CAPTURE_OPERATION_SOURCE::Import);
	check_round_trip("mixed", domain::E_CAPTURE_OPERATION_SOURCE::Mixed);
	check_round_trip("split", domain::E_CAPTURE_OPERATION_SOURCE::Split);
	check_round_trip("merge", domain::E_CAPTURE_OPERATION_SOURCE::Merge);
	check_round_trip("system", domain::E_CAPTURE_OPERATION_SOURCE::System);

	check_round_trip("keep", domain::E_SPLIT_POLICY::Keep);
	check_round_trip("split_by_blank_line", domain::E_SPLIT_POLICY::SplitByBlankLine);

	check_round_trip("typing", domain::E_CARD_SOURCE::Typing);
	check_round_trip("paste", domain::E_CARD_SOURCE::Paste);
	check_round_trip("import", domain::E_CARD_SOURCE::Import);
	check_round_trip("mixed", domain::E_CARD_SOURCE::Mixed);
	check_round_trip("restore", domain::E_CARD_SOURCE::Restore);
	check_round_trip("split", domain::E_CARD_SOURCE::Split);
	check_round_trip("merge", domain::E_CARD_SOURCE::Merge);
	check_round_trip("system", domain::E_CARD_SOURCE::System);

	check_round_trip("edit", domain::E_REVISION_SOURCE::Edit);
	check_round_trip("restore", domain::E_REVISION_SOURCE::Restore);
	check_round_trip("split", domain::E_REVISION_SOURCE::Split);
	check_round_trip("merge", domain::E_REVISION_SOURCE::Merge);

	check_round_trip("new", domain::E_DRAFT_KIND::New);
	check_round_trip("edit", domain::E_DRAFT_KIND::Edit);

	check_round_trip("split", domain::E_LINEAGE_RELATION_TYPE::Split);
	check_round_trip("merge", domain::E_LINEAGE_RELATION_TYPE::Merge);
}

// 대응 원본: StrEnum 생성자가 모르는 값에 ValueError 를 올리는 자리
// (repositories.py 의 행 매퍼 :843, :861, :877, :887, :903~904, :915).
// pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("모르는 철자는 기본값으로 접히지 않고 실패한다", "[core][domain]")
{
	// 이 열들은 CHECK 제약을 달고 있다. 실패 대신 첫 열거값으로 접으면 스키마가 거부할 값을
	// 쓰거나, 더 나쁘게는 스키마가 받아들이지만 뜻이 다른 값을 쓰게 된다.
	domain::E_EVENT_TYPE eEventType{ domain::E_EVENT_TYPE::Restore };
	REQUIRE_FALSE(domain::FromText("created", &eEventType));
	REQUIRE(eEventType == domain::E_EVENT_TYPE::Restore);

	domain::E_CARD_SOURCE eCardSource{};
	REQUIRE_FALSE(domain::FromText("", &eCardSource));
	REQUIRE_FALSE(domain::FromText("TYPING", &eCardSource));
	REQUIRE_FALSE(domain::FromText("typing ", &eCardSource));

	domain::E_SPLIT_POLICY eSplitPolicy{};
	REQUIRE_FALSE(domain::FromText("split", &eSplitPolicy));
	REQUIRE(domain::FromText("split_by_blank_line", &eSplitPolicy));

	// 카드와 리비전은 철자 집합이 다르다. edit 은 리비전에만, restore 는 양쪽에 있다.
	domain::E_REVISION_SOURCE eRevisionSource{};
	REQUIRE_FALSE(domain::FromText("typing", &eRevisionSource));
	REQUIRE(domain::FromText("edit", &eRevisionSource));
	domain::E_CARD_SOURCE eCardEdit{};
	REQUIRE_FALSE(domain::FromText("edit", &eCardEdit));

	domain::E_DRAFT_KIND eDraftKind{};
	REQUIRE_FALSE(domain::FromText("draft", &eDraftKind));

	domain::E_LINEAGE_RELATION_TYPE eRelation{};
	REQUIRE_FALSE(domain::FromText("keep", &eRelation));

	domain::E_EVENT_SOURCE eEventSource{};
	REQUIRE_FALSE(domain::FromText("split", &eEventSource));

	domain::E_CAPTURE_OPERATION_SOURCE eOperationSource{};
	REQUIRE_FALSE(domain::FromText("edit", &eOperationSource));
}

// 대응 원본: dataclass 가 만들어 주는 __eq__ 를 쓰는 비교
// (tests/integration/test_repositories.py::test_document_crud 의 get_document(...) == updated).
TEST_CASE("도메인 구조체 동치는 필드 전건 비교다", "[core][domain]")
{
	domain::S_DOCUMENT Left;
	Left.sId           = "document-1";
	Left.sTitle        = "제목";
	Left.nCreatedAtUs  = 1000;
	Left.nUpdatedAtUs  = 1000;

	domain::S_DOCUMENT Right = Left;
	REQUIRE(Left == Right);

	// nullable 열은 "없음"과 "0" 이 다른 값이다.
	Right.nArchivedAtUs = 0;
	REQUIRE_FALSE(Left == Right);
	Right.nArchivedAtUs = std::nullopt;
	REQUIRE(Left == Right);
}
