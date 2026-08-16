#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// core 계층. Win32/ATL/WTL/COM/DirectX 헤더를 포함하지 않는다(tools/gates 격리 게이트가 강제).

namespace pynote::core::domain
{
	// 파이썬 원본 domain/events.py 의 EventType(:7~14) 이식이다.
	// 아래 철자는 edit_events.event_type 의 CHECK 제약이 받는 값 그대로라 표기가 곧 계약이다.
	enum class E_EVENT_TYPE
	{
		Create,
		Update,
		Move,
		Split,
		Merge,
		Delete,
		Restore
	};

	// 파이썬 원본 domain/events.py 의 EventSource(:17~24) 이식이다.
	enum class E_EVENT_SOURCE
	{
		Typing,
		Paste,
		Import,
		Mixed,
		Edit,
		Restore,
		System
	};

	// 저장 철자를 돌려준다. 반환값은 정적 수명의 리터럴을 가리킨다.
	std::string_view ToText(E_EVENT_TYPE _eValue);
	std::string_view ToText(E_EVENT_SOURCE _eValue);

	// 모르는 철자는 실패다. 이 열들은 CHECK 제약을 달고 있어서 기본값으로 접으면
	// 스키마가 거부할 값을 쓰거나, 더 나쁘게는 스키마가 받아들이지만 뜻이 다른 값을 쓰게 된다.
	bool FromText(std::string_view _sText, E_EVENT_TYPE* _peValue);
	bool FromText(std::string_view _sText, E_EVENT_SOURCE* _peValue);

	// 파이썬 원본 domain/events.py 의 EditEvent(:27~37) 이식이다. 필드 순서와 이름은 원본 그대로다.
	// event_seq 는 SQLite 가 발급하므로 새 이벤트에서는 값이 없다.
	struct S_EDIT_EVENT
	{
		std::optional<std::int64_t> nEventSeq{};
		std::string                 sEventId;
		std::optional<std::string>  sOperationId{};
		std::string                 sDocumentId;
		std::optional<std::string>  sCardId{};
		E_EVENT_TYPE                eEventType{ E_EVENT_TYPE::Create };
		E_EVENT_SOURCE              eSource{ E_EVENT_SOURCE::Typing };
		std::int64_t                nOccurredAtUs{ 0 };
		std::string                 sDetailsJson;

		// 원본 dataclass 가 __eq__ 를 만들어 주고 시험이 그 동치를 계약으로 쓴다.
		bool operator==(const S_EDIT_EVENT&) const = default;
	};
}
