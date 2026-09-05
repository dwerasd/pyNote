#pragma once

#include "pynote/core/domain/events.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// core 계층. Win32/ATL/WTL/COM/DirectX 헤더를 포함하지 않는다(tools/gates 격리 게이트가 강제).

namespace pynote::core::domain
{
	// 파이썬 원본 domain/models.py 의 열거형 이식이다(:9~49). 각 철자는 저장 열의 CHECK
	// 제약이 받는 값 그대로이므로 표기 자체가 계약이다 - events.h 의 같은 규칙이 여기에도 걸린다.
	enum class E_CAPTURE_OPERATION_SOURCE
	{
		Typing,
		Paste,
		Import,
		Mixed,
		Split,
		Merge,
		System
	};

	enum class E_SPLIT_POLICY
	{
		Keep,
		SplitByBlankLine
	};

	enum class E_CARD_SOURCE
	{
		Typing,
		Paste,
		Import,
		Mixed,
		Restore,
		Split,
		Merge,
		System
	};

	enum class E_REVISION_SOURCE
	{
		Edit,
		Restore,
		Split,
		Merge
	};

	enum class E_DRAFT_KIND
	{
		New,
		Edit
	};

	enum class E_LINEAGE_RELATION_TYPE
	{
		Split,
		Merge
	};

	// domain/models.py 의 NewlineKind(:52~64). 철자는 card_file_bindings.newline 의 CHECK 값 그대로다.
	enum class E_NEWLINE_KIND
	{
		Lf,
		Crlf,
		Cr
	};

	std::string_view ToText(E_CAPTURE_OPERATION_SOURCE _eValue);
	std::string_view ToText(E_SPLIT_POLICY _eValue);
	std::string_view ToText(E_CARD_SOURCE _eValue);
	std::string_view ToText(E_REVISION_SOURCE _eValue);
	std::string_view ToText(E_DRAFT_KIND _eValue);
	std::string_view ToText(E_LINEAGE_RELATION_TYPE _eValue);
	std::string_view ToText(E_NEWLINE_KIND _eValue);

	bool FromText(std::string_view _sText, E_CAPTURE_OPERATION_SOURCE* _peValue);
	bool FromText(std::string_view _sText, E_SPLIT_POLICY* _peValue);
	bool FromText(std::string_view _sText, E_CARD_SOURCE* _peValue);
	bool FromText(std::string_view _sText, E_REVISION_SOURCE* _peValue);
	bool FromText(std::string_view _sText, E_DRAFT_KIND* _peValue);
	bool FromText(std::string_view _sText, E_LINEAGE_RELATION_TYPE* _peValue);
	bool FromText(std::string_view _sText, E_NEWLINE_KIND* _peValue);

	// 원본 NewlineKind.characters(:59~64) 다. 결속 파일에 기록할 줄바꿈 문자열을 돌려준다.
	std::string_view NewlineCharacters(E_NEWLINE_KIND _eValue);

	// 아래 구조체는 파이썬 원본 dataclass 의 필드 순서와 이름을 그대로 옮긴 것이다.
	// nullable 열은 std::optional 이고 텍스트는 UTF-8 을 담은 std::string 이다.

	// domain/models.py 의 Document(:52~59).
	struct S_DOCUMENT
	{
		std::string                 sId;
		std::string                 sTitle;
		std::int64_t                nCreatedAtUs{ 0 };
		std::int64_t                nUpdatedAtUs{ 0 };
		std::optional<std::int64_t> nArchivedAtUs{};
		std::optional<std::int64_t> nTrashedAtUs{};

		// 원본 dataclass 가 __eq__ 를 만들어 주고 시험이 그 동치를 계약으로 쓴다.
		bool operator==(const S_DOCUMENT&) const = default;
	};

	// domain/models.py 의 CaptureOperation(:62~71).
	struct S_CAPTURE_OPERATION
	{
		std::string                 sId;
		std::string                 sDocumentId;
		E_CAPTURE_OPERATION_SOURCE  eSource{ E_CAPTURE_OPERATION_SOURCE::Typing };
		E_SPLIT_POLICY              eSplitPolicy{ E_SPLIT_POLICY::Keep };
		std::optional<std::string>  sOriginalText{};
		std::optional<std::string>  sOriginalHash{};
		std::optional<std::int64_t> nOriginalRedactedAtUs{};
		std::int64_t                nCreatedAtUs{ 0 };

		// 원본 dataclass 가 __eq__ 를 만들어 주고 시험이 그 동치를 계약으로 쓴다.
		bool operator==(const S_CAPTURE_OPERATION&) const = default;
	};

	// domain/models.py 의 Card(:74~87).
	struct S_CARD
	{
		std::string                 sId;
		std::string                 sDocumentId;
		std::string                 sOperationId;
		std::int64_t                nPositionKey{ 0 };
		std::int64_t                nCaptureSeq{ 0 };
		std::int64_t                nCreatedAtUs{ 0 };
		std::int64_t                nUpdatedAtUs{ 0 };
		E_CARD_SOURCE               eSource{ E_CARD_SOURCE::Typing };
		std::string                 sBody;
		std::string                 sBodyHash;
		std::optional<std::string>  sCurrentRevisionId{};
		std::optional<std::int64_t> nDeletedAtUs{};

		// 원본 dataclass 가 __eq__ 를 만들어 주고 시험이 그 동치를 계약으로 쓴다.
		bool operator==(const S_CARD&) const = default;
	};

	// domain/models.py 의 CardRevision(:90~99).
	struct S_CARD_REVISION
	{
		std::string                sId;
		std::string                sCardId;
		std::int64_t               nEventSeq{ 0 };
		std::optional<std::string> sParentRevisionId{};
		std::string                sBody;
		std::string                sBodyHash;
		E_REVISION_SOURCE          eSource{ E_REVISION_SOURCE::Edit };
		std::int64_t               nCreatedAtUs{ 0 };

		// 원본 dataclass 가 __eq__ 를 만들어 주고 시험이 그 동치를 계약으로 쓴다.
		bool operator==(const S_CARD_REVISION&) const = default;
	};

	// domain/models.py 의 Draft(:102~112).
	struct S_DRAFT
	{
		std::string                sId;
		std::string                sDocumentId;
		std::optional<std::string> sCardId{};
		E_DRAFT_KIND               eDraftKind{ E_DRAFT_KIND::New };
		std::optional<std::string> sBaseRevisionId{};
		std::string                sDraftText;
		std::string                sDraftHash;
		std::int64_t               nCursorPositionQchar{ 0 };
		std::int64_t               nUpdatedAtUs{ 0 };

		// 원본 dataclass 가 __eq__ 를 만들어 주고 시험이 그 동치를 계약으로 쓴다.
		bool operator==(const S_DRAFT&) const = default;
	};

	// domain/models.py 의 FileBinding(:130~145). 카드 한 장과 디스크 파일 한 개의 결속 상태다.
	struct S_FILE_BINDING
	{
		std::string                 sCardId;
		std::string                 sPath;
		std::string                 sPathKey;
		std::string                 sEncoding;
		bool                        bBom{ false };
		E_NEWLINE_KIND              eNewline{ E_NEWLINE_KIND::Lf };
		bool                        bTrailingNewline{ false };
		std::int64_t                nBoundAtUs{ 0 };
		std::optional<std::int64_t> nSyncedSize{};
		std::optional<std::int64_t> nSyncedMtimeNs{};
		std::optional<std::string>  sSyncedHash{};
		std::optional<std::int64_t> nSyncedAtUs{};

		// 원본 dataclass 가 __eq__ 를 만들어 주고 시험이 그 동치를 계약으로 쓴다.
		bool operator==(const S_FILE_BINDING&) const = default;
	};

	// domain/models.py 의 CardLineage(:115~120).
	struct S_CARD_LINEAGE
	{
		std::string             sParentCardId;
		std::string             sChildCardId;
		std::int64_t            nEventSeq{ 0 };
		E_LINEAGE_RELATION_TYPE eRelationType{ E_LINEAGE_RELATION_TYPE::Split };

		// 원본 dataclass 가 __eq__ 를 만들어 주고 시험이 그 동치를 계약으로 쓴다.
		bool operator==(const S_CARD_LINEAGE&) const = default;
	};

	// domain/models.py 의 NewCaptureOperation(:123~130). 저장 전 입력이라 해시와 소거 시각이 없다.
	struct S_NEW_CAPTURE_OPERATION
	{
		std::string                sId;
		std::string                sDocumentId;
		E_CAPTURE_OPERATION_SOURCE eSource{ E_CAPTURE_OPERATION_SOURCE::Typing };
		E_SPLIT_POLICY             eSplitPolicy{ E_SPLIT_POLICY::Keep };
		std::optional<std::string> sOriginalText{};
		std::int64_t               nCreatedAtUs{ 0 };

		// 원본 dataclass 가 __eq__ 를 만들어 주고 시험이 그 동치를 계약으로 쓴다.
		bool operator==(const S_NEW_CAPTURE_OPERATION&) const = default;
	};

	// domain/models.py 의 NewCard(:133~144). event_details_json 의 기본값 "{}" 도 원본 그대로다.
	struct S_NEW_CARD
	{
		std::string       sId;
		std::string       sRevisionId;
		std::string       sEventId;
		std::int64_t      nPositionKey{ 0 };
		std::string       sBody;
		E_CARD_SOURCE     eCardSource{ E_CARD_SOURCE::Typing };
		E_EVENT_SOURCE    eEventSource{ E_EVENT_SOURCE::Typing };
		E_REVISION_SOURCE eRevisionSource{ E_REVISION_SOURCE::Edit };
		std::int64_t      nCreatedAtUs{ 0 };
		std::string       sEventDetailsJson{ "{}" };

		// 원본 dataclass 가 __eq__ 를 만들어 주고 시험이 그 동치를 계약으로 쓴다.
		bool operator==(const S_NEW_CARD&) const = default;
	};

	// 파이썬 원본은 이 구조체만 infrastructure/repositories.py(:39~46) 에 두지만, 저장소 서명이
	// 서는 도메인 값이라 다른 아홉과 같은 자리에 둔다. 필드 순서와 이름은 원본 그대로다.
	struct S_WORKSPACE_WINDOW
	{
		std::string                sWindowId;
		std::vector<std::string>   OpenDocumentIds;
		std::optional<std::string> sActiveDocumentId{};
		std::int64_t               nUpdatedAtUs{ 0 };

		// 원본 dataclass 가 __eq__ 를 만들어 주고 시험이 그 동치를 계약으로 쓴다.
		bool operator==(const S_WORKSPACE_WINDOW&) const = default;
	};
}
