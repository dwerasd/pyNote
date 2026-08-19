#pragma once

#include "pynote/core/domain/events.h"
#include "pynote/core/domain/models.h"
#include "pynote/core/storage/database.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// core 계층. Win32/ATL/WTL/COM/DirectX 헤더를 포함하지 않는다(tools/gates 격리 게이트가 강제).

namespace pynote::core::storage
{
	// 파이썬 원본 infrastructure/repositories.py 의 text_hash(:30~32) 이식이다.
	// UTF-8 바이트열의 SHA-256 을 소문자 16진수 64자로 돌려준다. body_hash 와 original_hash 가
	// 이 값이라 다이제스트나 인코딩이 달라지면 저장된 해시가 파이썬 원본과 어긋난다.
	std::string TextHash(const std::string& _sText);

	// 저장소 연산의 결과. 원본이 예외 종류로 구별하던 실패를 반환값으로 옮긴 것이다.
	enum class E_REPO_RESULT
	{
		Ok,
		NotFound,     // 조회 대상이 없다. 파이썬 원본이 None 을 돌려주는 자리다.
		CasConflict,  // 영향 행수가 1 이 아니다. 원본의 CardCompareAndSwapError 자리다.
		Invalid,      // 호출부가 계약을 어겼다. 원본의 ValueError 자리다.
		Failed        // SQLite 오류. 원본이 sqlite3.Error 를 올리는 자리다.
	};

	// 같은 SQLite 연결 위에서 도메인 행과 원자적 저장 흐름을 제공한다.
	// 파이썬 원본 infrastructure/repositories.py 의 Repositories(:49) 이식이다.
	//
	// 보존하는 계약(MODE A - 원본 repositories.py 실측):
	//  1. 실패 사유는 연결의 LastError() 에 남는다. CasConflict 는 Failed 와 반드시 구별된다 -
	//     호출부가 그 분기로 재시도를 결정하므로 접으면 낙관적 동시성 자체가 사라진다.
	//  2. 카드 CAS 넷(LinkInitialRevision :405, AdvanceCardRevision :417, UpdateCardPosition :443,
	//     UpdateCardDeletedState :461)은 영향 행수가 **정확히 1** 이어야 한다(_require_card_cas :820).
	//     1 이상이 아니라 1 이다.
	//  3. CreateCards(:716) 는 트랜잭션을 열기 **전에** 네 가지 거절을 판정하고, 그 뒤 한 트랜잭션이
	//     operation 행과 카드마다 이벤트 -> capture 순번 -> 카드 -> 리비전 -> 최초 리비전 연결을 덮는다.
	//  4. capture 순번은 UPDATE ... RETURNING 으로 발급한다(:663). 읽고 쓰는 두 문장으로 바꾸지 않는다.
	//  5. 이벤트 순서는 자동 증가하는 event_seq 이고 이력 조회는 오름차순이다(:638).
	//  6. 조회는 행이 없으면 NotFound 다. 원본이 None 을 돌려주는 자리와 같다.
	class C_REPOSITORIES
	{
	public:
		explicit C_REPOSITORIES(C_DATABASE& _database);

		C_REPOSITORIES(const C_REPOSITORIES&) = delete;
		C_REPOSITORIES& operator=(const C_REPOSITORIES&) = delete;

		// 이 저장소 묶음이 쓰는 연결. 원본의 database 프로퍼티(:56~59) 자리다.
		C_DATABASE& Database() const noexcept { return(m_Database); }

		// workspace_windows -------------------------------------------------------------------
		E_REPO_RESULT ListWorkspaceWindows(std::vector<domain::S_WORKSPACE_WINDOW>* _pOut);
		E_REPO_RESULT GetWorkspaceWindow(const std::string& _sWindowId, domain::S_WORKSPACE_WINDOW* _pOut);
		E_REPO_RESULT SaveWorkspaceWindow(
			const std::string& _sWindowId,
			const std::vector<std::string>& _OpenDocumentIds,
			const std::optional<std::string>& _sActiveDocumentId,
			domain::S_WORKSPACE_WINDOW* _pOut);
		E_REPO_RESULT DeleteWorkspaceWindow(const std::string& _sWindowId);

		// documents ---------------------------------------------------------------------------
		E_REPO_RESULT CreateDocument(const domain::S_DOCUMENT& _Document);
		E_REPO_RESULT GetDocument(const std::string& _sDocumentId, domain::S_DOCUMENT* _pOut);
		E_REPO_RESULT ListDocuments(std::vector<domain::S_DOCUMENT>* _pOut);
		E_REPO_RESULT SearchDocuments(const std::string& _sQuery, std::vector<domain::S_DOCUMENT>* _pOut);
		E_REPO_RESULT UpdateDocument(const domain::S_DOCUMENT& _Document);
		E_REPO_RESULT TouchDocument(const std::string& _sDocumentId, std::int64_t _nUpdatedAtUs);
		E_REPO_RESULT DeleteDocument(const std::string& _sDocumentId);

		// cards 조회 --------------------------------------------------------------------------
		E_REPO_RESULT SearchCards(
			const std::string& _sQuery,
			const std::optional<std::string>& _sDocumentId,
			std::vector<domain::S_CARD>* _pOut);

		// 카드의 입력 작업 원문이 purge 로 소거되지 않았는지 돌려준다. 카드가 없으면 NotFound 다 -
		// 원본은 이 자리에서만 KeyError 를 올리므로(:270) 사유 문구를 LastError 에 그대로 남긴다.
		E_REPO_RESULT OperationReconstructionAvailable(const std::string& _sCardId, bool* _pAvailable);

		// capture_operations ------------------------------------------------------------------
		E_REPO_RESULT CreateCaptureOperation(const domain::S_CAPTURE_OPERATION& _Operation);
		E_REPO_RESULT GetCaptureOperation(const std::string& _sOperationId, domain::S_CAPTURE_OPERATION* _pOut);
		E_REPO_RESULT UpdateCaptureOperation(const domain::S_CAPTURE_OPERATION& _Operation);
		E_REPO_RESULT DeleteCaptureOperation(const std::string& _sOperationId);

		// cards -------------------------------------------------------------------------------
		E_REPO_RESULT CreateCard(const domain::S_CARD& _Card);
		E_REPO_RESULT GetCard(const std::string& _sCardId, domain::S_CARD* _pOut);
		E_REPO_RESULT ListCards(const std::string& _sDocumentId, std::vector<domain::S_CARD>* _pOut);

		// 아래 넷이 카드 CAS 다. 영향 행수가 1 이 아니면 CasConflict 다.
		E_REPO_RESULT LinkInitialRevision(const std::string& _sCardId, const std::string& _sRevisionId);
		E_REPO_RESULT AdvanceCardRevision(const domain::S_CARD& _Card, const std::string& _sExpectedRevisionId);
		E_REPO_RESULT UpdateCardPosition(
			const std::string& _sCardId,
			std::int64_t _nPositionKey,
			const std::string& _sExpectedRevisionId);
		E_REPO_RESULT UpdateCardDeletedState(
			const std::string& _sCardId,
			std::int64_t _nPositionKey,
			const std::optional<std::int64_t>& _nDeletedAtUs,
			const std::string& _sExpectedRevisionId);

		E_REPO_RESULT DeleteCard(const std::string& _sCardId);

		// card_revisions ----------------------------------------------------------------------
		E_REPO_RESULT CreateRevision(const domain::S_CARD_REVISION& _Revision);
		E_REPO_RESULT GetRevision(const std::string& _sRevisionId, domain::S_CARD_REVISION* _pOut);
		E_REPO_RESULT ListRevisions(const std::string& _sCardId, std::vector<domain::S_CARD_REVISION>* _pOut);
		E_REPO_RESULT DeleteRevisionForPurge(const std::string& _sRevisionId);

		// drafts ------------------------------------------------------------------------------
		E_REPO_RESULT CreateDraft(const domain::S_DRAFT& _Draft);
		E_REPO_RESULT GetDraft(const std::string& _sDraftId, domain::S_DRAFT* _pOut);
		E_REPO_RESULT ListDrafts(const std::string& _sDocumentId, std::vector<domain::S_DRAFT>* _pOut);
		E_REPO_RESULT UpdateDraft(const domain::S_DRAFT& _Draft);
		E_REPO_RESULT DeleteDraft(const std::string& _sDraftId);

		// edit_events -------------------------------------------------------------------------
		// 새 이벤트의 event_seq 는 SQLite 가 발급한다. 값이 실려 오면 Invalid 다(:596~597).
		// 성공하면 _pOut 에 발급된 순번까지 채운 이벤트를 넣는다.
		E_REPO_RESULT CreateEvent(const domain::S_EDIT_EVENT& _Event, domain::S_EDIT_EVENT* _pOut);
		E_REPO_RESULT GetEvent(std::int64_t _nEventSeq, domain::S_EDIT_EVENT* _pOut);
		E_REPO_RESULT ListEvents(const std::string& _sDocumentId, std::vector<domain::S_EDIT_EVENT>* _pOut);
		E_REPO_RESULT DeleteEventForPurge(std::int64_t _nEventSeq);

		// counters ----------------------------------------------------------------------------
		E_REPO_RESULT GetCounter(const std::string& _sName, std::int64_t* _pValue);

		// card_lineage ------------------------------------------------------------------------
		E_REPO_RESULT CreateLineage(const domain::S_CARD_LINEAGE& _Lineage);
		E_REPO_RESULT ListLineageForCard(const std::string& _sCardId, std::vector<domain::S_CARD_LINEAGE>* _pOut);
		E_REPO_RESULT DeleteLineage(const domain::S_CARD_LINEAGE& _Lineage);

		// 원자적 카드 생성 ---------------------------------------------------------------------
		// 네 거절은 트랜잭션 밖에서 판정하고 사유를 LastError 에 남긴다. 성공하면 _pOut 에는
		// 방금 연결된 리비전 id 까지 담긴 카드가 들어간다 - 트랜잭션 안에서 만든 값에는 없던 값이다.
		E_REPO_RESULT CreateCards(
			const domain::S_NEW_CAPTURE_OPERATION& _Operation,
			const std::vector<domain::S_NEW_CARD>& _Cards,
			std::vector<domain::S_CARD>* _pOut);
		// 호출자가 연 활성 트랜잭션 안에서만 카드 생성 행을 쓴다. 커밋은 호출자 소유다.
		E_REPO_RESULT CreateCardsInActiveTransaction(
			const domain::S_NEW_CAPTURE_OPERATION& _Operation,
			const std::vector<domain::S_NEW_CARD>& _Cards,
			std::vector<domain::S_CARD>* _pOut);

	private:
		// 활성 카드 생성 트랜잭션 안에서 capture 순번 하나를 발급한다(:663~677).
		// 원본과 같은 자리에서 트랜잭션 여부를 먼저 확인한다.
		E_REPO_RESULT issue_capture_sequence_(std::int64_t* _pValue);
		E_REPO_RESULT validate_create_cards_(
			const domain::S_NEW_CAPTURE_OPERATION& _Operation,
			const std::vector<domain::S_NEW_CARD>& _Cards);

		// SQLite 실패를 LastError 로 옮기고 Failed 를 돌려준다.
		E_REPO_RESULT fail_();

		// 쓰기 트랜잭션을 열지 못한 사유를 중첩과 SQLite 오류로 가른다.
		E_REPO_RESULT begin_failed_();

		// 카드 CAS 넷이 공유하는 판정(_require_card_cas :820~825).
		E_REPO_RESULT require_card_cas_(int _nChanges, const std::string& _sCardId);

		C_DATABASE& m_Database;
	};
}
