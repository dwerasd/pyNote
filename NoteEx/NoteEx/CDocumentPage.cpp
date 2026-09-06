#include "CDocumentPage.h"

// ATL/WTL 은 CreateEvent 매크로가 살아 있을 때 읽어야 한다 - 아래 #undef 뒤에 두면
// atlbase.h·atlapp.h 의 ::CreateEvent 호출이 식별자를 잃는다(실측 C3861/C2039).
#include "CCardList.h"

#ifdef CreateEvent
#undef CreateEvent
#endif

#include "Resource.h"
#include "pynote/core/application/card_service.h"
#include "pynote/core/application/draft_coordinator.h"
#include "pynote/core/application/first_input_capture.h"
#include "pynote/core/application/save_coordinator.h"
#include "pynote/core/application/workspace_state.h"
#include "pynote/core/domain/card_list_projection.h"
#include "pynote/core/storage/database.h"
#include "pynote/core/storage/repositories.h"

#include "CChangeBus.h"

#include <CommCtrl.h>
#include <Richedit.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma comment(lib, "Comctl32")

namespace
{
	namespace app = pynote::core::application;
	namespace domain = pynote::core::domain;
	namespace storage = pynote::core::storage;

	std::wstring wide(const std::string& _sValue)
	{
		if (_sValue.empty()) { return(std::wstring{}); }
		const int nSize = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			_sValue.data(), static_cast<int>(_sValue.size()), nullptr, 0);
		if (nSize <= 0) { return(std::wstring{}); }
		std::wstring Result(static_cast<std::size_t>(nSize), L'\0');
		return(::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, _sValue.data(),
			static_cast<int>(_sValue.size()), Result.data(), nSize) == nSize ? Result : std::wstring{});
	}

	std::string utf8(std::wstring _sValue)
	{
		std::wstring Normalized;
		Normalized.reserve(_sValue.size());
		for (std::size_t nIndex = 0; nIndex < _sValue.size(); ++nIndex)
		{
			// RichEdit(WM_GETTEXT, GT_DEFAULT 등가)는 단락 구분을 단독 CR 로 돌려준다 -
			// P1 프로브 normalize_lf 와 같은 규칙으로 CR/CRLF 를 전부 LF 로 접는다.
			if (_sValue[nIndex] == L'\r')
			{
				if (nIndex + 1 < _sValue.size() && _sValue[nIndex + 1] == L'\n') { ++nIndex; }
				Normalized.push_back(L'\n');
			}
			else { Normalized.push_back(_sValue[nIndex]); }
		}
		if (Normalized.empty()) { return(std::string{}); }
		const int nSize = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
			Normalized.data(), static_cast<int>(Normalized.size()), nullptr, 0, nullptr, nullptr);
		if (nSize <= 0) { return(std::string{}); }
		std::string Result(static_cast<std::size_t>(nSize), '\0');
		return(::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Normalized.data(),
			static_cast<int>(Normalized.size()), Result.data(), nSize, nullptr, nullptr) == nSize ?
			Result : std::string{});
	}

	std::int64_t now_us()
	{
		return(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count());
	}

	// 부록 A-1 순서 그대로다(index = enum order) - S5 정렬 콤보의 유일한 사실 출처.
	int sort_combo_index(domain::E_CARD_LIST_SORT_MODE _eMode) noexcept
	{
		switch (_eMode)
		{
		case domain::E_CARD_LIST_SORT_MODE::Position: return(1);
		case domain::E_CARD_LIST_SORT_MODE::Capture: return(2);
		default: return(0);
		}
	}

	// 부록 A-2 순서 그대로다(index 0 = 필터 없음) - S5 출처 필터 콤보의 유일한 사실 출처.
	std::optional<domain::E_CARD_SOURCE> source_filter_for_index(int _nIndex) noexcept
	{
		switch (_nIndex)
		{
		case 1: return(domain::E_CARD_SOURCE::Typing);
		case 2: return(domain::E_CARD_SOURCE::Paste);
		case 3: return(domain::E_CARD_SOURCE::Mixed);
		case 4: return(domain::E_CARD_SOURCE::Import);
		case 5: return(domain::E_CARD_SOURCE::Restore);
		default: return(std::nullopt);
		}
	}
}

struct C_DOCUMENT_PAGE::S_STATE
{
	C_DOCUMENT_PAGE* pOwner{};
	HINSTANCE hInstance{};
	HWND hListHost{};
	HWND hEditorHost{};
	HWND hEditor{};
	HWND hFind{};
	HWND hReplace{};
	HWND hHistory{};
	// S5 정렬/출처 필터/휴지통 스트립과 카드 목록 소유 tooltips 창(spec §3.1.1/§3.2.8).
	HWND hSortCombo{};
	HWND hSourceFilter{};
	HWND hTrashButton{};
	HWND hTooltip{};
	HMODULE hRichEdit{};
	storage::C_DATABASE* pDatabase{};
	storage::C_REPOSITORIES* pRepositories{};
	app::C_CARD_SERVICE* pCardService{};
	app::C_DRAFT_COORDINATOR* pDraftCoordinator{};
	app::C_SAVE_COORDINATOR* pSaveCoordinator{};
	std::unique_ptr<domain::C_CARD_LIST_PROJECTION> Projection;
	std::unique_ptr<app::C_FIRST_INPUT_CAPTURE> FirstInput;
	std::string sWorkspaceId;
	std::string sDocumentId;
	std::optional<std::string> sDraftId{};
	std::optional<std::string> sCurrentCardId{};
	// 행 <-> 카드 매핑과 선택은 프로젝션이 단독 소유한다(구 ListCardIds 중복 소유 제거).
	C_CARD_LIST CardList;
	C_DOCUMENT_PAGE::LeavePrompt LeavePrompt;
	C_DOCUMENT_PAGE::DragDeletePrompt DragDelete;
	C_DOCUMENT_PAGE::ChangeNotifier Notifier;
	// S5: 필터의 "현재 선택값" 은 core 가 아니라 페이지가 든다(선례 bMultiSelectionEnabled,
	// spec §3.1.7) - core 는 값을 소유할 getter 를 얻지 않는다. 비영속(canon §2-6/native §2-7):
	// S_DOCUMENT_UI_STATE 에 열이 없고 재오픈은 항상 "모든 출처" 로 보인다.
	std::optional<domain::E_CARD_SOURCE> eSourceFilter{};
	// 보기 메뉴로 켠 다중 선택. Init 이 프로젝션을 만든 직후 다시 건다(새 창도 영속값에서 시작).
	bool bMultiSelectionEnabled{ false };
	bool bSynchronizing{ false };
	bool bCleaned{ false };
	bool bCardSaveFailed{ false };

	// 카드 생성·저장 완료 지점에서만 부른다. 통지 대상(창)이 다시 이 페이지를
	// 건드릴 수 있으므로 호출부의 상태 갱신이 끝난 자리에서 부른다.
	void notify_change() const
	{
		if (Notifier) { Notifier(); }
	}

	std::string editor_text() const
	{
		const int nLength = ::GetWindowTextLengthW(hEditor);
		std::wstring Text(static_cast<std::size_t>((std::max)(0, nLength)) + 1, L'\0');
		const int nCopied = ::GetWindowTextW(hEditor, Text.data(), static_cast<int>(Text.size()));
		Text.resize(static_cast<std::size_t>((std::max)(0, nCopied)));
		return(utf8(std::move(Text)));
	}

	std::int64_t cursor_position() const
	{
		CHARRANGE Range{};
		::SendMessageW(hEditor, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&Range));
		return((std::max)(0L, Range.cpMax));
	}

	void set_editor_text(const std::string& _sText, std::int64_t _nCursor = -1)
	{
		bSynchronizing = true;
		const std::wstring Text = wide(_sText);
		::SetWindowTextW(hEditor, Text.c_str());
		const LONG nLength = static_cast<LONG>(::GetWindowTextLengthW(hEditor));
		const LONG nCursor = _nCursor < 0 ? nLength : static_cast<LONG>((std::min<std::int64_t>)
			((std::max<std::int64_t>)(0, _nCursor), nLength));
		::SendMessageW(hEditor, EM_SETSEL, nCursor, nCursor);
		bSynchronizing = false;
	}

	app::E_CARD_SORT_MODE service_sort() const noexcept
	{
		switch (Projection->SortMode())
		{
		case domain::E_CARD_LIST_SORT_MODE::Position: return(app::E_CARD_SORT_MODE::Position);
		case domain::E_CARD_LIST_SORT_MODE::Capture: return(app::E_CARD_SORT_MODE::Capture);
		default: return(app::E_CARD_SORT_MODE::Recency);
		}
	}

	// 원본 DocumentPage.refresh(): 목록만 다시 읽는다. 선택·현재 카드는 프로젝션이 들고
	// 있으므로 되돌려 놓을 것이 없고, 스크롤도 새 내용 높이로 클램프만 된다(결정 5 의 S1 몫).
	bool refresh_cards()
	{
		std::vector<domain::S_CARD> Cards;
		if (pCardService->ListActiveCards(sDocumentId, this->service_sort(), &Cards) !=
			app::E_CARD_SERVICE_RESULT::Ok) { return(false); }
		// S5: 원본 refresh() 의 두 메타 캐시를 같은 순서로 재계산한다(document_page.py:
		// 238~262) - 방금 읽은 카드 목록 기준, ListActiveCards 성공 뒤·CancelPendingBrowse
		// 앞이다(spec §3.4.2). 저장소 호출은 여기 한 곳뿐이고 그리기 경로에는 닿지 않는다
		// (PLAN-W4-0047/0061 의 1만 장 타이밍 게이트 보호). list_drafts 기반 dirty 재구축
		// 셋째 단계는 이식하지 않는다 - 재진입은 W6 편집기 결선(spec §3.4.2 선언된 이탈).
		std::unordered_map<std::string, int> RevisionCounts;
		std::set<std::string> ReconstructionUnavailable;
		for (const domain::S_CARD& Card : Cards)
		{
			std::vector<domain::S_CARD_REVISION> Revisions;
			pRepositories->ListRevisions(Card.sId, &Revisions);
			RevisionCounts[Card.sId] = static_cast<int>(Revisions.size());
			bool bAvailable = true;
			pRepositories->OperationReconstructionAvailable(Card.sId, &bAvailable);
			if (!bAvailable) { ReconstructionUnavailable.insert(Card.sId); }
		}
		// CEILING: 10000장 갱신 = SQL 20000문(ListRevisions 가 본문까지 적재) — 파이썬
		// 동일, 상향 경로 = COUNT 전용 저장소 API(core 재개방, 별건)
		// CEILING: 결정 5 의 리셋→TakeDeltas 소비 전환은 F-04 가 트리거 자리를 대체하면서
		// 최적화 잔여로 남았다 — CAP-NC-010 은 이미 §3.4.2·§3.4.4 로 닫히므로 이 잔여는
		// 성능 개선 별건(W4 aggregate 이연, 이연검증대장 §3-9 S5 항 기록)
		// 조용하다 - 자체 무효화가 없고 바로 뒤따르는 리셋에 실린다(원본
		// set_cards(cards, revision_counts=...) 와 같은 접힘).
		CardList.SetRevisionCounts(std::move(RevisionCounts));
		// 원본은 modelAboutToBeReset 에서 취소한다 - 읽기가 성공한 뒤, 리셋 바로 앞이다.
		// 읽기가 실패하면 원본은 예외로 빠져나가 리셋에 닿지 않으므로 여기서도 취소하지 않는다.
		CardList.CancelPendingBrowse();
		Projection->SetCards(Cards);
		CardList.OnProjectionChanged();
		// 리셋 뒤 - 원본의 넷째, 별도 set_reconstruction_unavailable_ids 호출과 같다.
		CardList.SetReconstructionUnavailableIds(std::move(ReconstructionUnavailable));
		return(true);
	}

	// 원본 reveal_card(document_page.py:286~296) 등가 - 보이는 카드면 선택까지 옮기고
	// 화면에 들인다. 필터에 가려 행이 없으면 현재 카드만 잡는다.
	void select_current_card_()
	{
		if (!sCurrentCardId) { return; }
		// 원본과 같은 reveal_card 한 자리로 모은다 - 대기 중 휠 탐색 취소와 앵커 이동까지
		// 포함된다. 행이 없으면(필터에 가림) 현재 카드만 잡던 S1/W3 갈래를 그대로 둔다.
		if (!pOwner->RevealCard(*sCurrentCardId)) { Projection->SetCurrentCardId(*sCurrentCardId); }
	}

	// 기동 복구 처분의 W3 자리(원본 app.py:893 _resolve_startup_recovery → main_window.py:994
	// _startup_suppressed_card_ids): 복구 프롬프트(CAP-FI-013)는 W6 소유라 아직 없으므로 후보
	// 전건을 LATER 로 처분한 것과 같게 — 후보 카드의 편집기 복원을 억제해 초안을 DB 에 보존한다.
	// nullopt = 후보 조회 저장소 실패(원본은 예외로 기동을 닫는다).
	// CEILING: W6 에서 프롬프트 결과(RECOVER/DISCARD/LATER)의 앱 수준 억제 집합으로 대체.
	std::optional<bool> editor_restore_suppressed(const std::string& _sCardId)
	{
		const auto Candidates = pDraftCoordinator->RecoveryCandidates(sDocumentId);
		if (Candidates.eOutcome != app::E_DRAFT_OUTCOME::Ok) { return(std::nullopt); }
		return(std::any_of(Candidates.Candidates.begin(), Candidates.Candidates.end(),
			[&_sCardId](const app::S_DRAFT_RECOVERY_CANDIDATE& _Candidate) { return(_Candidate.Draft.sCardId == _sCardId); }));
	}

	// _bReveal=false 는 원본 card_open_requested -> _open_card 경로다(선택을 다시 잡지 않는다).
	// 앱 주도 open_card/reveal_card 는 true 로 부른다.
	bool open_card(const std::string& _sCardId, bool _bReplaceEditor, bool _bReveal = true)
	{
		// 같은 카드를 다시 여는 것도 "열기" 다 - 원본은 이 경우에도 card_connected 를 다시 emit 해
		// (card_editor.py:192~196) _card_connected -> reveal_card -> cancel_pending_browse 로
		// 잔여 각이 0 이 된다. 이 갈래는 refresh_cards() 도 아래 명시 취소도 지나가지 않으므로
		// 여기서 직접 부른다(spec §3.4.4).
		if (sDraftId && sCurrentCardId == _sCardId)
		{
			CardList.CancelPendingBrowse();
			::SetFocus(hEditor);
			return(true);
		}
		if (sDraftId && pOwner->RequestLeave() == app::E_LEAVE_RESULT::Denied) { return(false); }
		domain::S_CARD Card;
		if (pRepositories->GetCard(_sCardId, &Card) != storage::E_REPO_RESULT::Ok || Card.nDeletedAtUs)
		{
			return(false);
		}
		const auto Opened = pDraftCoordinator->OpenCard(Card);
		if (Opened.eOutcome != app::E_DRAFT_OUTCOME::Ok || !Opened.Session) { return(false); }
		sDraftId = Opened.Session->sDraftId;
		sCurrentCardId = Card.sId;
		if (_bReplaceEditor) { this->set_editor_text(Opened.Session->sText); }
		// 새로 만들어진 카드는 이 갱신 뒤에야 프로젝션에 들어온다 - 선택은 그 다음이다.
		this->refresh_cards();
		// 마우스 press·Enter 는 이미 현재 카드를 잡아 두었다 - 여기서 다시 선택하면 방금 만든
		// 다중 선택이 한 장으로 접힌다(오라클 M5/M10 이 열기 뒤에도 선택을 유지한다).
		if (_bReveal) { this->select_current_card_(); }
		// 원본은 성공한 열기의 끝에서 잔여 각이 0 이 된다 - card_connected -> _card_connected ->
		// reveal_card -> cancel_pending_browse 사슬이다(card_editor.py:247, document_page.py:887~892).
		// 네이티브에는 그 신호 사슬이 없으므로 여기서 명시로 부른다(refresh_cards 가 S5 에서
		// 델타 소비로 바뀌어도 이 관측은 그대로 남는다).
		CardList.CancelPendingBrowse();
		::SetFocus(hEditor);
		return(true);
	}

	// 원본 _release_editor_session_if_card_removed(document_page.py:929~936) +
	// card_editor.release_session_for_removed_card(:419~435).
	void release_if_removed_()
	{
		if (!sDraftId) { return; }
		const auto Session = pDraftCoordinator->Session(*sDraftId);
		if (!Session || !Session->sCardId) { return; }
		domain::S_CARD Card;
		const auto eResult = pRepositories->GetCard(*Session->sCardId, &Card);
		if (eResult == storage::E_REPO_RESULT::Ok && !Card.nDeletedAtUs) { return; }
		// 원본은 보호 실패를 경고만 하고 계속한다.
		if (Session->bDirty) { pOwner->Protect(); }
		// CEILING: 원본은 _prepare_empty_surface() 실패 시 연결을 유지하지만 W3 첫 입력 모델에는
		// NEW backing 초안이 없어 대응물이 없다 - 해제는 무조건 진행한다.
		pDraftCoordinator->ReleaseSession(*sDraftId);
		sDraftId.reset();
		sCurrentCardId.reset();
		FirstInput->ResetAfterAcceptedClose();
		this->set_editor_text({});
	}

	// 원본 card_editor.discard_session_for_deleted_card(:619~632)의 쌍둥이다. 위
	// release_if_removed_ 의 해제·비움 꼬리를 그대로 쓰되 더티 보호(:249)는 없다 - 초안은
	// 이미 SoftDelete 에 discard 로 넘겼고, 여기서 다시 보호하면 사용자가 버리기로 한 초안이
	// 복구 후보로 남는다(W4 S4 spec §3.3.7).
	// CEILING: 원본은 _prepare_empty_surface() 실패 시 연결을 유지하지만 W3 첫 입력 모델에는
	// NEW backing 초안이 없어 대응물이 없다 - release_if_removed_ 와 같은 처분이다.
	void discard_session_for_deleted_card_()
	{
		if (!sDraftId) { return; }
		pDraftCoordinator->ReleaseSession(*sDraftId);
		sDraftId.reset();
		sCurrentCardId.reset();
		FirstInput->ResetAfterAcceptedClose();
		this->set_editor_text({});
	}

	// 원본 _body_for_drag(document_page.py:969~976). 1) 편집기에 연결된 카드면 편집기 평문
	// (더티 여부를 보지 않는다) 2) 프로젝션 본문 3) 저장소 조회 4) 빈 문자열.
	// 1) 이 더티 버퍼를 실제로 돌려주게 만드는 편집기 쪽 반쪽은 W6 다(PLAN-W6-0011).
	std::string body_for_drag_(const std::string& _sCardId)
	{
		if (sDraftId && sCurrentCardId && *sCurrentCardId == _sCardId)
		{
			return(this->editor_text());
		}
		const std::optional<std::size_t> nRow = Projection->RowForCard(_sCardId);
		const domain::S_CARD* pCard = nRow ? Projection->CardAt(*nRow) : nullptr;
		if (pCard) { return(pCard->sBody); }
		domain::S_CARD Card;
		if (pRepositories->GetCard(_sCardId, &Card) != storage::E_REPO_RESULT::Ok)
		{
			return(std::string{});
		}
		return(Card.sBody);
	}

	bool synchronize_editor(domain::E_CAPTURE_OPERATION_SOURCE _eSource)
	{
		if (bSynchronizing || bCleaned) { return(true); }
		const std::string sText = this->editor_text();
		const std::int64_t nCursor = this->cursor_position();
		if (!sDraftId)
		{
			const auto Captured = FirstInput->OnMeaningfulInsertion(sText, _eSource);
			if (Captured.eEffect == app::E_FIRST_INPUT_EFFECT::CreationFailed) { return(false); }
			if (Captured.sConnectedCardId && !this->open_card(*Captured.sConnectedCardId, false)) { return(false); }
			if (Captured.sConnectedCardId) { this->notify_change(); }
		}
		if (!sDraftId) { return(true); }
		const auto Updated = pDraftCoordinator->UpdateSession(*sDraftId, sText, nCursor,
			_eSource == domain::E_CAPTURE_OPERATION_SOURCE::Paste);
		if (Updated.eOutcome != app::E_DRAFT_OUTCOME::Ok) { return(false); }
		if (sCurrentCardId)
		{
			// S5: 값이 실제로 바뀔 때만 통지한다 - synchronize_editor 는 카드가 더티인 동안
			// 매 키 입력마다 같은 true 를 재도장하므로, 가드가 없으면 매 키 입력마다 로그·
			// 재도장이 뜬다(spec §3.4.4, "이 가드는 선택이 아니다"). core 자체 가드
			// (SetCardDirty 의 if(before==dirty)return;) 는 *값*만 지키지 이 통지를 대신
			// 못 낸다.
			const bool bDirty = Updated.Session && Updated.Session->bDirty;
			const bool bWasDirty = Projection->IsCardDirty(*sCurrentCardId);
			Projection->SetCardDirty(*sCurrentCardId, bDirty);
			if (bWasDirty != bDirty) { CardList.NotifyCardDirtyChanged(*sCurrentCardId); }
		}
		return(true);
	}

	static LRESULT CALLBACK EditorSubclass(
		HWND _hWnd, UINT _uMessage, WPARAM _wParam, LPARAM _lParam,
		UINT_PTR, DWORD_PTR _nReference)
	{
		auto* pState = reinterpret_cast<S_STATE*>(_nReference);
		if (_uMessage == WM_NCDESTROY) { ::RemoveWindowSubclass(_hWnd, &S_STATE::EditorSubclass, 1); }
		if (!pState || pState->bSynchronizing) { return(::DefSubclassProc(_hWnd, _uMessage, _wParam, _lParam)); }
		if (_uMessage == WM_PASTE)
		{
			const LRESULT nResult = ::DefSubclassProc(_hWnd, _uMessage, _wParam, _lParam);
			pState->synchronize_editor(domain::E_CAPTURE_OPERATION_SOURCE::Paste);
			return(nResult);
		}
		if (_uMessage == WM_CHAR)
		{
			const LRESULT nResult = ::DefSubclassProc(_hWnd, _uMessage, _wParam, _lParam);
			pState->synchronize_editor(domain::E_CAPTURE_OPERATION_SOURCE::Typing);
			return(nResult);
		}
		return(::DefSubclassProc(_hWnd, _uMessage, _wParam, _lParam));
	}

	// hListHost 는 CMain 이 만들고 여러 문서를 거쳐 재사용하는 장수 pane 이다 - 페이지가
	// 만들지도 부수지도 않으므로 WM_NCDESTROY 자동 해제(EditorSubclass 의 방식)를 쓸 수
	// 없다. 제거는 Cleanup() 이 명시로 한다(spec §3.1.5, ratified departure). 서브클래스 id
	// 2 다(EditorSubclass 가 이미 id 1 을 hEditor 에 쓴다). 자식 컨트롤의 WM_COMMAND 만
	// 가로채고 나머지는 그대로 DefSubclassProc 로 흘려보낸다.
	static LRESULT CALLBACK ListHostSubclass(
		HWND _hWnd, UINT _uMessage, WPARAM _wParam, LPARAM _lParam,
		UINT_PTR, DWORD_PTR _nReference)
	{
		auto* pState = reinterpret_cast<S_STATE*>(_nReference);
		if (pState && _uMessage == WM_COMMAND)
		{
			const UINT nId = LOWORD(_wParam);
			const UINT nCode = HIWORD(_wParam);
			if (nId == IDC_DOCUMENT_SORT_COMBO && nCode == CBN_SELCHANGE)
			{
				static constexpr domain::E_CARD_LIST_SORT_MODE Modes[] = {
					domain::E_CARD_LIST_SORT_MODE::Recency,
					domain::E_CARD_LIST_SORT_MODE::Position,
					domain::E_CARD_LIST_SORT_MODE::Capture };
				const int nIndex = static_cast<int>(
					::SendMessageW(pState->hSortCombo, CB_GETCURSEL, 0, 0));
				if (nIndex >= 0 && static_cast<std::size_t>(nIndex) < std::size(Modes))
				{
					pState->pOwner->SetSortMode(Modes[nIndex]);
				}
				return(0);
			}
			if (nId == IDC_DOCUMENT_SOURCE_FILTER && nCode == CBN_SELCHANGE)
			{
				const int nIndex = static_cast<int>(
					::SendMessageW(pState->hSourceFilter, CB_GETCURSEL, 0, 0));
				pState->pOwner->SetSourceFilter(source_filter_for_index(nIndex));
				return(0);
			}
			if (nId == IDC_DOCUMENT_TRASH_BUTTON && nCode == BN_CLICKED)
			{
				// 지어졌지만 배선되지 않았다 - CEILING: W7 휴지통 대화상자·복원 UI, 지금은
				// 눌러도 아무 일도 없다(spec §3.1.11).
				return(0);
			}
		}
		return(::DefSubclassProc(_hWnd, _uMessage, _wParam, _lParam));
	}
};

C_DOCUMENT_PAGE::C_DOCUMENT_PAGE() : m_pState(std::make_unique<S_STATE>())
{
	m_pState->pOwner = this;
}

C_DOCUMENT_PAGE::~C_DOCUMENT_PAGE() { this->Cleanup(); }

bool C_DOCUMENT_PAGE::Init(
	HINSTANCE _hInstance, HWND _hListHost, HWND _hEditorHost,
	storage::C_DATABASE& _Database, storage::C_REPOSITORIES& _Repositories,
	app::C_CARD_SERVICE& _CardService, app::C_DRAFT_COORDINATOR& _DraftCoordinator,
	app::C_SAVE_COORDINATOR& _SaveCoordinator, std::string _sWorkspaceId,
	std::string _sDocumentId, LeavePrompt _LeavePrompt)
{
	if (!_hInstance || !::IsWindow(_hListHost) || !::IsWindow(_hEditorHost) ||
		_sWorkspaceId.empty() || _sDocumentId.empty()) { return(false); }
	m_sDocumentId = _sDocumentId;
	auto& State = *m_pState;
	// 재채움 경로(외부 문서 소멸 뒤)는 Cleanup 한 페이지를 같은 객체로 다시 연다.
	// Cleanup 이 남긴 표식과 잔여 식별자를 여기서 걷지 않으면 Layout·Cleanup 이
	// 즉시 반환하고 이전 문서의 선택이 새 문서로 새어 든다.
	State.bCleaned = false;
	State.bSynchronizing = false;
	State.bCardSaveFailed = false;
	State.sDraftId.reset();
	State.sCurrentCardId.reset();
	State.hInstance = _hInstance;
	State.hListHost = _hListHost;
	State.hEditorHost = _hEditorHost;
	State.pDatabase = &_Database;
	State.pRepositories = &_Repositories;
	State.pCardService = &_CardService;
	State.pDraftCoordinator = &_DraftCoordinator;
	State.pSaveCoordinator = &_SaveCoordinator;
	State.sWorkspaceId = std::move(_sWorkspaceId);
	State.sDocumentId = std::move(_sDocumentId);
	State.LeavePrompt = std::move(_LeavePrompt);
	State.Projection = std::make_unique<domain::C_CARD_LIST_PROJECTION>();
	State.CardList.Bind(*State.Projection);
	// 원본은 생성 시점에 apply_settings() 로 장치 설정을 건다 - 새 창도 영속된 값에서 시작한다.
	State.Projection->SetMultiSelectionEnabled(State.bMultiSelectionEnabled);
	State.FirstInput = std::make_unique<app::C_FIRST_INPUT_CAPTURE>(
		_CardService, *State.Projection, State.sDocumentId);
	// 원본은 생성 시점에 정책 미리보기 줄 수를 모델에 건다(document_page.py:89~111).
	// 정책 행이 없거나 CHECK 를 어기면 기동을 닫는 것은 앱 수준(CApplication::validate_policy)
	// 의 일이라 여기서는 프로젝션 기본값(3)으로 계속한다.
	const auto Policy = pynote::shell::LoadDataPolicy(_Database);
	if (Policy)
	{
		State.Projection->SetPreviewLineCount(
			static_cast<std::size_t>((std::max<std::int64_t>)(1, Policy->nPreviewLines)));
	}
	State.hRichEdit = ::LoadLibraryW(L"Msftedit.dll");
	if (!State.hRichEdit) { return(false); }
	RECT CardListRect{ 0, 0, 1, 1 };
	if (!State.CardList.Create(_hListHost, CardListRect, nullptr,
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL, WS_EX_CLIENTEDGE,
		static_cast<UINT>(IDC_DOCUMENT_CARD_LIST)))
	{
		this->Cleanup();
		return(false);
	}
	State.CardList.SetActivateHandler([&State]() { State.pOwner->OpenSelectedCard(); });
	// 원본 card_open_requested -> _open_card: 신호 경로는 선택을 다시 잡지 않는다.
	State.CardList.SetOpenCardHandler([&State](const std::string& _sCardId)
		{ State.open_card(_sCardId, true, false); });
	State.CardList.SetEmptyAreaClickHandler([&State]() { State.pOwner->OnEmptyAreaClicked(); });
	State.CardList.SetDeleteHandler([&State](std::vector<std::string> _CardIds)
		{ State.pOwner->DeleteCards(_CardIds); });
	// 원본 _browse_card(document_page.py:443~456).
	State.CardList.SetBrowseCardHandler([&State](const std::string& _sCardId)
		{
			// 성공하면 편집면이 가져간 포커스를 목록으로 되돌린다 - 그러지 않으면 이어지는
			// 방향키 탐색이 편집기 커서 이동으로 새어 나간다.
			if (State.open_card(_sCardId, true, false))
			{
				::SetFocus(State.CardList.m_hWnd);
				return(true);
			}
			// 실패(이탈 거부·카드 소멸·초안 열기 실패)면 선택 복원은 core CompleteOpen 몫이고
			// 여기서는 포커스만 편집면에 둔다(원본 _focus_editor_slot 은 무조건이다).
			::SetFocus(State.hEditor);
			return(false);
		});
	// 원본 self.editor.card_id 자리. 네이티브는 초안 세션이 늘 카드를 들고 있어
	// (sDraftId 와 sCurrentCardId 가 함께 서고 함께 진다) 세션 유무로 판정한다.
	State.CardList.SetEditorCardProvider([&State]() -> std::optional<std::string>
		{ return(State.sDraftId ? State.sCurrentCardId : std::nullopt); });
	// ---- W4 S4 드래그 앤 드롭 배선 ----
	// 원본 card_move_requested -> _move_card.
	State.CardList.SetMoveCardHandler([&State](const std::string& _sCardId,
		const std::optional<std::string>& _sBeforeCardId)
		{ State.pOwner->MoveCard(_sCardId, _sBeforeCardId); });
	// 원본 card_delete_dropped -> _delete_dragged_card.
	State.CardList.SetDeleteDroppedHandler([&State](const std::string& _sCardId)
		{ State.pOwner->DeleteDraggedCard(_sCardId); });
	// 원본 drag_started -> _show_delete_drop_zone(_card_id, token). 첫 인자는 원본 시그니처
	// 그대로 받아 두고 쓰지 않는다 - 오버레이는 카드마다 달라지지 않는다.
	State.CardList.SetDragStartedHandler([&State](const std::string&,
		domain::CardDragSessionToken _nToken) { State.CardList.ArmDeleteZone(_nToken); });
	// 원본 drag_finished -> _hide_delete_drop_zone(token).
	State.CardList.SetDragFinishedHandler([&State](domain::CardDragSessionToken)
		{ State.CardList.DisarmDeleteZone(); });
	// 원본 set_drag_body_provider(self._body_for_drag)(document_page.py:209).
	State.CardList.SetDragBodyProvider([&State](const std::string& _sCardId)
		{ return(State.body_for_drag_(_sCardId)); });
	// S5 정렬/출처 필터/휴지통 스트립 - hListHost 의 자식이다(CardList/hHistory 와 같은
	// 자리, spec §3.1.1). 높이는 일부러 넉넉하다 - CBS_DROPDOWNLIST 의 높이 인자는 열린
	// 드롭다운의 최대 폭이지 닫힌 상자만이 아니다(spec §3.1.3). Layout() 이 실제 위치를
	// 잡는다.
	State.hSortCombo = ::CreateWindowExW(0, L"COMBOBOX", L"",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
		0, 0, 1, 24 + 3 * 20, _hListHost,
		reinterpret_cast<HMENU>(IDC_DOCUMENT_SORT_COMBO), _hInstance, nullptr);
	State.hSourceFilter = ::CreateWindowExW(0, L"COMBOBOX", L"",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
		0, 0, 1, 24 + 6 * 20, _hListHost,
		reinterpret_cast<HMENU>(IDC_DOCUMENT_SOURCE_FILTER), _hInstance, nullptr);
	State.hTrashButton = ::CreateWindowExW(0, L"BUTTON", L"카드 휴지통",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
		0, 0, 1, 1, _hListHost,
		reinterpret_cast<HMENU>(IDC_DOCUMENT_TRASH_BUTTON), _hInstance, nullptr);
	State.hHistory = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
		WS_CHILD | WS_TABSTOP | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
		0, 0, 1, 1, _hListHost, reinterpret_cast<HMENU>(IDC_DOCUMENT_HISTORY), _hInstance, nullptr);
	State.hFind = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
		WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
		0, 0, 1, 1, _hEditorHost, reinterpret_cast<HMENU>(IDC_DOCUMENT_FIND), _hInstance, nullptr);
	State.hReplace = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
		WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
		0, 0, 1, 1, _hEditorHost, reinterpret_cast<HMENU>(IDC_DOCUMENT_REPLACE), _hInstance, nullptr);
	State.hEditor = ::CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
		0, 0, 1, 1, _hEditorHost, reinterpret_cast<HMENU>(IDC_DOCUMENT_EDITOR), _hInstance, nullptr);
	// 카드 목록 소유 tooltips 창 - 스트립 3개는 서브클래스 자동 표시, 카드 목록은 트래킹
	// 형이다(spec §3.2.7~8, decision H-4: 진짜 shower/hider 를 이 Init 이 직접 건다).
	State.hTooltip = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASS, nullptr,
		TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, _hListHost, nullptr, _hInstance, nullptr);
	if (!State.hSortCombo || !State.hSourceFilter || !State.hTrashButton ||
		!State.hHistory || !State.hFind || !State.hReplace || !State.hEditor ||
		!::SetWindowSubclass(State.hEditor, &S_STATE::EditorSubclass, 1,
			reinterpret_cast<DWORD_PTR>(&State)) ||
		// hListHost 는 CMain 이 만든 장수 pane 이다 - 이 컨트롤들의 WM_COMMAND 를 가로챌
		// 서브클래스는 페이지가 직접 건다(spec §3.1.5). id 2 다(EditorSubclass 가 hEditor
		// 에 id 1 을 이미 쓴다). 재-Init 에서 다시 걸어도 무해하다(SetWindowSubclass 가
		// 참조 데이터를 그 자리에서 갱신한다).
		!::SetWindowSubclass(State.hListHost, &S_STATE::ListHostSubclass, 2,
			reinterpret_cast<DWORD_PTR>(&State)))
	{
		this->Cleanup();
		return(false);
	}
	// 항목 순서는 부록 A-1/A-2 그대로다(index = enum order, 기본 선택은 둘 다 0).
	for (const wchar_t* pItem : { L"최근 활동순", L"현재 문서 순서", L"최초 기록 순서" })
	{
		::SendMessageW(State.hSortCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(pItem));
	}
	for (const wchar_t* pItem :
		{ L"모든 출처", L"직접 입력", L"붙여넣기", L"혼합", L"가져오기", L"복구" })
	{
		::SendMessageW(State.hSourceFilter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(pItem));
	}
	::SendMessageW(State.hSortCombo, CB_SETCURSEL, 0, 0);
	::SendMessageW(State.hSourceFilter, CB_SETCURSEL, 0, 0);
	if (State.hTooltip)
	{
		::SetWindowPos(State.hTooltip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
		// 6~7줄 툴팁이 \n 에서 실제로 줄바꿈하려면 필요하다 - 없으면 한 줄로 접힌다.
		::SendMessageW(State.hTooltip, TTM_SETMAXTIPWIDTH, 0, 400);
		auto add_subclass_tool = [&State](HWND _hControl, const wchar_t* _pText)
			{
				TTTOOLINFOW Info{};
				// 이 앱은 Common Controls v6 매니페스트 의존이 없다(구 클래식 comctl32 로드,
				// spec §3.2.8) - sizeof(TTTOOLINFOW) 는 v6 이후 늘어난 필드(lParam 등)까지
				// 포함해 구 DLL 이 TTM_ADDTOOL/TTM_GETTOOLINFO 를 조용히 실패시킨다. V1
				// 크기만 쓴다(fix1 F10 — 들여쓰기 정리).
				Info.cbSize = TTTOOLINFOW_V1_SIZE;
				Info.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
				Info.hwnd = State.hListHost;
				Info.uId = reinterpret_cast<UINT_PTR>(_hControl);
				Info.lpszText = const_cast<wchar_t*>(_pText);
				::SendMessageW(State.hTooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&Info));
			};
		add_subclass_tool(State.hSortCombo,
			L"카드를 최근 활동, 현재 문서 순서 또는 최초 기록 순서로 정렬");
		add_subclass_tool(State.hSourceFilter, L"선택한 입력 출처의 카드만 표시");
		add_subclass_tool(State.hTrashButton, L"삭제한 카드를 확인하고 복구하거나 완전 삭제");
		// 카드 목록 자신의 툴팁은 트래킹 형이다 - hover 콜백이 위치·본문을 직접 민다.
		TTTOOLINFOW Tracked{};
		Tracked.cbSize = TTTOOLINFOW_V1_SIZE;
		Tracked.uFlags = TTF_TRACK | TTF_ABSOLUTE;
		Tracked.hwnd = State.hListHost;
		Tracked.uId = reinterpret_cast<UINT_PTR>(State.CardList.m_hWnd);
		Tracked.lpszText = const_cast<wchar_t*>(L"");
		::SendMessageW(State.hTooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&Tracked));
		// 진짜 shower/hider - 이 Init 이 직접 건다(decision H-4: comctl32 툴팁은 비모달이라
		// TrackPopupMenu/DoDragDrop 과 달리 어떤 페이지 픽스처에서도 안전하다, spec §3.2.7).
		State.CardList.SetTooltipShower(
			[&State](std::size_t, const std::wstring& _sText, POINT _Screen)
			{
				TTTOOLINFOW Info{};
				// 이 앱은 Common Controls v6 매니페스트 의존이 없다(구 클래식 comctl32 로드,
				// spec §3.2.8) - sizeof(TTTOOLINFOW) 는 v6 이후 늘어난 필드(lParam 등)까지
				// 포함해 구 DLL 이 TTM_ADDTOOL/TTM_GETTOOLINFO 를 조용히 실패시킨다. V1
				// 크기만 쓴다(fix1 F10 — 들여쓰기 정리).
				Info.cbSize = TTTOOLINFOW_V1_SIZE;
				Info.hwnd = State.hListHost;
				Info.uId = reinterpret_cast<UINT_PTR>(State.CardList.m_hWnd);
				std::wstring sText = _sText;
				Info.lpszText = sText.data();
				::SendMessageW(State.hTooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&Info));
				::SendMessageW(State.hTooltip, TTM_TRACKPOSITION, 0, MAKELPARAM(_Screen.x, _Screen.y));
				::SendMessageW(State.hTooltip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&Info));
			});
		State.CardList.SetTooltipHider(
			[&State]()
			{
				TTTOOLINFOW Info{};
				// 이 앱은 Common Controls v6 매니페스트 의존이 없다(구 클래식 comctl32 로드,
				// spec §3.2.8) - sizeof(TTTOOLINFOW) 는 v6 이후 늘어난 필드(lParam 등)까지
				// 포함해 구 DLL 이 TTM_ADDTOOL/TTM_GETTOOLINFO 를 조용히 실패시킨다. V1
				// 크기만 쓴다(fix1 F10 — 들여쓰기 정리).
				Info.cbSize = TTTOOLINFOW_V1_SIZE;
				Info.hwnd = State.hListHost;
				Info.uId = reinterpret_cast<UINT_PTR>(State.CardList.m_hWnd);
				::SendMessageW(State.hTooltip, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&Info));
			});
	}
	if (!State.refresh_cards()) { this->Cleanup(); return(false); }
	app::C_WORKSPACE_STATE_STORE Store(_Database, _Repositories, State.sWorkspaceId);
	app::S_DOCUMENT_UI_STATE UiState;
	if (Store.LoadDocumentUiState(State.sDocumentId, &UiState) != storage::E_REPO_RESULT::Ok)
	{
		this->Cleanup();
		return(false);
	}
	State.Projection->SetSortMode(UiState.eSortMode);
	if (!State.refresh_cards()) { this->Cleanup(); return(false); }
	// 복원된 모드를 콤보 표시에 맞춘다 - CB_SETCURSEL 은 CBN_SELCHANGE 를 보내지 않으므로
	// 재귀 호출이 없다(측정: investigation §3-6, spec §3.1.9).
	::SendMessageW(State.hSortCombo, CB_SETCURSEL,
		static_cast<WPARAM>(sort_combo_index(UiState.eSortMode)), 0);
	if (UiState.sSelectedCardId)
	{
		const auto nRow = State.Projection->RowForCard(*UiState.sSelectedCardId);
		if (nRow)
		{
			State.Projection->SelectVisibleCard(*UiState.sSelectedCardId,
				domain::E_CARD_SELECTION_INTENT::Replace);
			State.CardList.EnsureVisible(*nRow);
		}
	}
	// 결정 4: 영속 좌표계는 파이썬과 같은 픽셀이다. 선택 복원 뒤에 덮어쓰는 순서까지
	// 원본과 같다(main_window.py:985~991).
	State.CardList.ScrollToPixel(static_cast<int>((std::min<std::int64_t>)(
		(std::numeric_limits<int>::max)(),
		(std::max<std::int64_t>)(0, UiState.nListScrollPosition))));
	if (UiState.sEditorCardId)
	{
		const auto Suppressed = State.editor_restore_suppressed(*UiState.sEditorCardId);
		if (!Suppressed) { this->Cleanup(); return(false); }
		domain::S_CARD EditorCard;
		const auto eCard = State.pRepositories->GetCard(*UiState.sEditorCardId, &EditorCard);
		if (eCard != storage::E_REPO_RESULT::Ok && eCard != storage::E_REPO_RESULT::NotFound)
		{
			this->Cleanup();
			return(false);
		}
		// 원본 main_window.py:992~998: LATER 억제 카드가 아니고 카드가 살아 있을 때만 편집기를
		// 복원한다. 사라진·삭제된 카드는 복원만 건너뛴다(창 생성 실패가 아니다).
		if (!*Suppressed && eCard == storage::E_REPO_RESULT::Ok && !EditorCard.nDeletedAtUs)
		{
			if (!State.open_card(*UiState.sEditorCardId, true)) { this->Cleanup(); return(false); }
			if (UiState.nEditorCursorQchar) { State.set_editor_text(State.editor_text(), *UiState.nEditorCursorQchar); }
		}
	}
	this->Layout();
	::SetFocus(State.hEditor);
	return(true);
}

void C_DOCUMENT_PAGE::SetChangeNotifier(ChangeNotifier _Notifier)
{
	m_pState->Notifier = std::move(_Notifier);
}

void C_DOCUMENT_PAGE::SetDragDeletePrompt(DragDeletePrompt _Prompt)
{
	m_pState->DragDelete = std::move(_Prompt);
}

void C_DOCUMENT_PAGE::SetContextMenuExecutor(std::function<UINT(HMENU, POINT)> _Executor)
{
	// 컨트롤로 그대로 넘긴다 - 페이지는 스스로 설치하지 않고 셸만 이 함수를 부른다.
	m_pState->CardList.SetContextMenuExecutor(std::move(_Executor));
}

void C_DOCUMENT_PAGE::SetDragRunner(
	std::function<HRESULT(IDataObject*, IDropSource*, DWORD, DWORD*)> _Runner)
{
	// 메뉴 실행기와 같은 전달자다 - 페이지는 스스로 설치하지 않는다(fix1).
	m_pState->CardList.SetDragRunner(std::move(_Runner));
}

void C_DOCUMENT_PAGE::SetRenderServices(d2d::C_D2D_DEVICE* _pDevice,
	d2d::C_D2D_BRUSH_CACHE* _pBrushCache, d2d::C_D2D_TEXT* _pText)
{
	m_pState->CardList.AttachRenderServices(_pDevice, _pBrushCache, _pText);
}

void C_DOCUMENT_PAGE::SetDisplaySettings(const S_CARD_LIST_DISPLAY& _Display)
{
	m_pState->CardList.SetDisplaySettings(_Display);
}

bool C_DOCUMENT_PAGE::PreTranslateMessage(MSG* _pMessage)
{
	if (!_pMessage || !m_pState->hEditor) { return(false); }
	if (_pMessage->message == WM_KEYDOWN && _pMessage->hwnd == m_pState->hEditor &&
		_pMessage->wParam == VK_RETURN && (::GetKeyState(VK_CONTROL) & 0x8000))
	{
		::SendMessageW(m_pState->hEditor, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"\n"));
		return(m_pState->synchronize_editor(domain::E_CAPTURE_OPERATION_SOURCE::Typing));
	}
	// Esc/Alt+Left 는 여기서 처리하지 않는다 - 원본(main_window.py:855~860 back_action)이
	// 창 수준 단축키라 액셀러레이터(IDM_BACK -> OnBack -> RequestLeave) 단일 경로가 정본이다.
	// 여기서도 처리하면 거부된 leave 가 액셀러레이터로 흘러 프롬프트가 두 번 뜬다.
	return(false);
}

bool C_DOCUMENT_PAGE::Protect()
{
	if (!m_pState->sDraftId) { return(true); }
	const auto Session = m_pState->pDraftCoordinator->Session(*m_pState->sDraftId);
	if (!Session || !Session->bDirty) { return(true); }
	const auto Result = m_pState->pDraftCoordinator->ProtectNow(*m_pState->sDraftId);
	return(Result.eOutcome == app::E_DRAFT_OUTCOME::Ok || Result.eOutcome == app::E_DRAFT_OUTCOME::NoOp);
}

app::E_LEAVE_RESULT C_DOCUMENT_PAGE::CanLeave()
{
	if (!m_pState->sDraftId) { return(app::E_LEAVE_RESULT::ApprovedClean); }
	const std::string sDraftId = *m_pState->sDraftId;
	const auto Session = m_pState->pDraftCoordinator->Session(sDraftId);
	if (!Session) { return(app::E_LEAVE_RESULT::Denied); }
	// 깨끗한 세션은 승인만 한다(원본 can_leave_editor: session.dirty 가 아니면 True) - 세션을 여기서
	// 풀면 앱 종료의 persist(승인 뒤)가 편집기 카드를 잃는다(실측 2026-08-21 D-04: 재시작 복원 실패).
	if (!Session->bDirty) { return(app::E_LEAVE_RESULT::ApprovedClean); }
	E_LEAVE_CHOICE eChoice = E_LEAVE_CHOICE::Cancel;
	if (m_pState->LeavePrompt) { eChoice = m_pState->LeavePrompt(m_pState->hEditor); }
	else
	{
		const int nChoice = ::MessageBoxW(m_pState->hEditor,
			L"변경 내용을 저장하고 편집을 닫으시겠습니까?", L"NoteEx",
			MB_YESNOCANCEL | MB_ICONQUESTION);
		eChoice = nChoice == IDYES ? E_LEAVE_CHOICE::Save :
			nChoice == IDNO ? E_LEAVE_CHOICE::Discard : E_LEAVE_CHOICE::Cancel;
	}
	if (eChoice == E_LEAVE_CHOICE::Cancel) { ::SetFocus(m_pState->hEditor); return(app::E_LEAVE_RESULT::Denied); }
	if (eChoice == E_LEAVE_CHOICE::Save)
	{
		if (!this->Save()) { ::SetFocus(m_pState->hEditor); return(app::E_LEAVE_RESULT::Denied); }
		return(app::E_LEAVE_RESULT::ApprovedAfterSave);
	}
	// 버리기는 원본도 승인 단계에서 세션을 버리고 편집면을 비운다(can_leave_editor 의 else 분기).
	if (m_pState->pDraftCoordinator->DiscardSession(sDraftId).eOutcome != app::E_DRAFT_OUTCOME::Ok)
	{
		return(app::E_LEAVE_RESULT::Denied);
	}
	m_pState->sDraftId.reset();
	m_pState->sCurrentCardId.reset();
	m_pState->FirstInput->ResetAfterAcceptedClose();
	m_pState->set_editor_text({});
	return(app::E_LEAVE_RESULT::ApprovedClean);
}

app::E_LEAVE_RESULT C_DOCUMENT_PAGE::RequestLeave()
{
	// 원본 request_close → can_leave_editor(protect_now=True): 더티 세션은 프롬프트 전에 초안을 보호하고
	// 보호 실패면 이탈을 거부한다(깨끗한 세션은 ProtectNow 가 NoOp). 참가자 승인(CanLeave)에는 이 단계가
	// 없다(원본 can_leave_open_pages 는 protect_now 기본값 False).
	if (m_pState->sDraftId && !this->Protect()) { ::SetFocus(m_pState->hEditor); return(app::E_LEAVE_RESULT::Denied); }
	const auto Result = this->CanLeave();
	if (Result == app::E_LEAVE_RESULT::Denied) { return(Result); }
	if (m_pState->sDraftId)
	{
		if (m_pState->pDraftCoordinator->ReleaseSession(*m_pState->sDraftId).eOutcome != app::E_DRAFT_OUTCOME::Ok)
		{
			return(app::E_LEAVE_RESULT::Denied);
		}
		m_pState->sDraftId.reset();
		m_pState->sCurrentCardId.reset();
		m_pState->FirstInput->ResetAfterAcceptedClose();
		m_pState->set_editor_text({});
	}
	this->FocusCardList();
	return(Result);
}

bool C_DOCUMENT_PAGE::PersistState(const std::optional<std::pair<int, int>>& _SplitSizesDip)
{
	if (!m_pState->pDatabase || !m_pState->pRepositories) { return(false); }
	app::S_DOCUMENT_UI_STATE State;
	State.sDocumentId = m_pState->sDocumentId;
	// 현재 카드가 행을 잃었으면 선택 없음으로 적는다(원본 main_window.py:945~946 은
	// 유효하지 않은 현재 인덱스에 None 을 남긴다).
	const std::optional<std::string>& sCurrent = m_pState->Projection->CurrentCardId();
	if (sCurrent && m_pState->Projection->RowForCard(*sCurrent))
	{
		State.sSelectedCardId = *sCurrent;
	}
	State.nListScrollPosition = m_pState->CardList.ScrollOffsetDip();
	State.eSortMode = m_pState->Projection->SortMode();
	if (m_pState->sDraftId)
	{
		const auto Session = m_pState->pDraftCoordinator->Session(*m_pState->sDraftId);
		if (Session)
		{
			State.sEditorCardId = Session->sCardId;
			State.sEditorBaseRevisionId = Session->sBaseRevisionId;
			State.nEditorCursorQchar = m_pState->cursor_position();
		}
	}
	State.EditorSplitSizes = _SplitSizesDip;
	State.nUpdatedAtUs = now_us();
	app::C_WORKSPACE_STATE_STORE Store(
		*m_pState->pDatabase, *m_pState->pRepositories, m_pState->sWorkspaceId);
	return(Store.SaveDocumentUiState(State) == storage::E_REPO_RESULT::Ok);
}

bool C_DOCUMENT_PAGE::Cleanup()
{
	if (!m_pState || m_pState->bCleaned) { return(true); }
	bool bOk = this->Protect();
	if (m_pState->sDraftId)
	{
		const auto Released = m_pState->pDraftCoordinator->ReleaseSession(*m_pState->sDraftId);
		bOk = bOk && Released.eOutcome == app::E_DRAFT_OUTCOME::Ok;
		m_pState->sDraftId.reset();
	}
	// hListHost 는 페이지가 만들지도 부수지도 않는다(CMain 이 재활용하는 장수 pane 이다) -
	// 서브클래스만 여기서 명시로 걷는다. 최종 Cleanup() 은 소멸자 안에서 도는데, 그 시점에
	// 도 hListHost 는 보통 살아 있다(CMain 소유). WM_NCDESTROY 에 맡기면(EditorSubclass 의
	// 방식) 페이지 객체가 죽은 뒤에도 pane 의 서브클래스 사슬에 죽은 &State 를 가리키는
	// 항목이 남는다(spec §3.1.5, ratified departure).
	if (::IsWindow(m_pState->hListHost))
	{
		::RemoveWindowSubclass(m_pState->hListHost, &S_STATE::ListHostSubclass, 2);
	}
	if (m_pState->CardList.IsWindow()) { m_pState->CardList.DestroyWindow(); }
	HWND* Windows[] = { &m_pState->hSortCombo, &m_pState->hSourceFilter,
		&m_pState->hTrashButton, &m_pState->hTooltip, &m_pState->hHistory,
		&m_pState->hFind, &m_pState->hReplace, &m_pState->hEditor };
	for (HWND* pWindow : Windows)
	{
		if (::IsWindow(*pWindow)) { ::DestroyWindow(*pWindow); }
		*pWindow = nullptr;
	}
	if (m_pState->hRichEdit) { ::FreeLibrary(m_pState->hRichEdit); m_pState->hRichEdit = nullptr; }
	// 정리된 페이지가 창에 통지하면 이미 떼어진 자식 HWND 를 다시 읽게 된다.
	m_pState->Notifier = {};
	m_pState->bCleaned = true;
	return(bOk);
}

void C_DOCUMENT_PAGE::Layout()
{
	if (!m_pState || m_pState->bCleaned) { return; }
	RECT ListClient{};
	RECT EditorClient{};
	::GetClientRect(m_pState->hListHost, &ListClient);
	::GetClientRect(m_pState->hEditorHost, &EditorClient);
	// S5 정렬/필터 위 · 휴지통 아래, 두 행 24 DIP 씩(spec §3.1.3, "DPI-scaled") - 두
	// 번째 반이 홀수 폭의 나머지를 흡수한다(QHBoxLayout 1:1 stretch 와 같다). 96 DPI 에서는
	// 지금과 같은 24 다(fix1 F2 — 감사 parity-2).
	const LONG STRIP_ROW_HEIGHT_DIP = ::MulDiv(24,
		static_cast<int>(::GetDpiForWindow(m_pState->hListHost)), USER_DEFAULT_SCREEN_DPI);
	// 콤보의 창 높이는 열린 드롭다운의 최대 높이를 정한다(spec §3.1.3) - 닫힌 상자는
	// 콤보가 스스로 표준 높이로 줄이므로, 여기서도 생성 때(Init 의 24 + itemCount*20)와
	// 같은 넉넉한 높이를 줘야 목록이 잘리지 않는다. 스트립의 논리 행 높이는 여전히
	// STRIP_ROW_HEIGHT_DIP 다(fix1 F1 — 감사 parity-1: Layout() 이 24 로 다시 누르면
	// Init 의 예방책이 무효화됐었다).
	const LONG SORT_COMBO_HEIGHT_DIP = STRIP_ROW_HEIGHT_DIP + 3 * 20;
	const LONG FILTER_COMBO_HEIGHT_DIP = STRIP_ROW_HEIGHT_DIP + 6 * 20;
	const LONG nHalfWidth = ListClient.right / 2;
	::MoveWindow(m_pState->hSortCombo, 0, 0, nHalfWidth, SORT_COMBO_HEIGHT_DIP, TRUE);
	::MoveWindow(m_pState->hSourceFilter, nHalfWidth, 0,
		ListClient.right - nHalfWidth, FILTER_COMBO_HEIGHT_DIP, TRUE);
	::MoveWindow(m_pState->hTrashButton, 0, STRIP_ROW_HEIGHT_DIP,
		ListClient.right, STRIP_ROW_HEIGHT_DIP, TRUE);
	const LONG nStripHeight = 2 * STRIP_ROW_HEIGHT_DIP;
	// 짧은 fixture 픽스처(예: w4_card_list_test.cpp 의 48 DIP pane)가 스트립 높이와
	// 정확히 같으면 바닥으로 1 DIP 까지 눌린다 - 기존 편집면 한 줄 아래의
	// (std::max)(1L, ...) 관용구와 같다(spec §3.1.3).
	const LONG nListHeight = (std::max)(1L, ListClient.bottom - nStripHeight);
	::MoveWindow(m_pState->CardList.m_hWnd, 0, nStripHeight, ListClient.right, nListHeight, TRUE);
	::MoveWindow(m_pState->hHistory, 0, nStripHeight, ListClient.right, nListHeight, TRUE);
	const bool bFind = ::IsWindowVisible(m_pState->hFind) != FALSE;
	const bool bReplace = ::IsWindowVisible(m_pState->hReplace) != FALSE;
	int nTop = 0;
	if (bFind) { ::MoveWindow(m_pState->hFind, 0, nTop, EditorClient.right, 26, TRUE); nTop += 28; }
	if (bReplace) { ::MoveWindow(m_pState->hReplace, 0, nTop, EditorClient.right, 26, TRUE); nTop += 28; }
	::MoveWindow(m_pState->hEditor, 0, nTop, EditorClient.right,
		(std::max)(1L, EditorClient.bottom - nTop), TRUE);
}

void C_DOCUMENT_PAGE::SetMultiSelectionEnabled(bool _bEnabled)
{
	m_pState->bMultiSelectionEnabled = _bEnabled;
	if (!m_pState->Projection) { return; }
	// 끌 때의 축소(현재가 선택돼 있으면 현재, 아니면 첫 선택)는 core 규칙이다(W2-0024/0025/0027).
	m_pState->Projection->SetMultiSelectionEnabled(_bEnabled);
	// 행을 잃은 앵커 정리와 다시 그리기를 함께 돈다.
	m_pState->CardList.OnProjectionChanged();
}

bool C_DOCUMENT_PAGE::MultiSelectionEnabled() const
{
	return(m_pState->Projection && m_pState->Projection->MultiSelectionEnabled());
}

bool C_DOCUMENT_PAGE::OnEmptyAreaClicked()
{
	auto& State = *m_pState;
	const HWND hEditor = State.hEditor;
	if (!State.sDraftId)
	{
		// 빈 편집면에서는 닫기 게이트를 돌리지 않는다. 목록 press 가 가져간 포커스만 되돌린다.
		::SetFocus(hEditor);
		State.CardList.PostDeferred([hEditor]() { ::SetFocus(hEditor); });
		return(true);
	}
	const auto Result = this->RequestLeave();
	// 원본은 거부돼도 편집면 포커스를 확정한다(거부 경로는 CanLeave 안에서 이미 되돌렸다).
	::SetFocus(hEditor);
	State.CardList.PostDeferred([hEditor]() { ::SetFocus(hEditor); });
	return(Result != app::E_LEAVE_RESULT::Denied);
}

bool C_DOCUMENT_PAGE::DeleteCards(const std::vector<std::string>& _CardIds)
{
	// 원본 뷰는 빈 튜플을 발행하지 않는다.
	if (_CardIds.empty()) { return(false); }
	auto& State = *m_pState;
	// 원본 _can_run_destructive_command -> can_leave_editor(protect_now=True) 다.
	// Protect() 는 세션이 없거나 깨끗하면 참을 돌려주고, CanLeave() 도 세션이 없으면 승인이다.
	// CEILING: 원본은 여기서 빈 편집면 초안(_protect_empty_surface)도 보호하지만 W3 첫 입력
	// 모델에는 연결 전 초안이 없어 대응물이 없다 - W6 보호/복구 조각에서 재판정한다.
	if (!this->Protect()) { return(false); }
	if (this->CanLeave() == app::E_LEAVE_RESULT::Denied) { return(false); }
	for (const std::string& sCardId : _CardIds)
	{
		domain::S_CARD Deleted;
		if (State.pCardService->SoftDelete(sCardId, std::nullopt, false, std::nullopt, &Deleted) !=
			app::E_CARD_SERVICE_RESULT::Ok)
		{
			// 카드별 독립 트랜잭션이라 실패 전까지 지워진 분은 남는다.
			// CEILING: 오류 대화상자는 W7 주변 UI(error reporter seam) 몫이다 - 여기서는 false 만 돌린다.
			State.release_if_removed_();
			State.refresh_cards();
			State.notify_change();
			return(false);
		}
	}
	State.release_if_removed_();
	// 프로젝션이 지워진 id 를 버리고 core 정규화가 선택에서도 뺀다 - 다시 선택하지 않는다.
	State.refresh_cards();
	State.notify_change();
	// 원본은 삭제 뒤 포커스를 옮기지 않는다.
	return(true);
}

bool C_DOCUMENT_PAGE::MoveCard(const std::string& _sCardId,
	const std::optional<std::string>& _sBeforeCardId)
{
	// 원본 _move_card(document_page.py:919~932).
	auto& State = *m_pState;
	if (State.bCleaned) { return(false); }
	// 원본 _can_run_destructive_command -> can_leave_editor(protect_now=True) 다.
	if (!this->Protect()) { return(false); }
	if (this->CanLeave() == app::E_LEAVE_RESULT::Denied) { return(false); }
	domain::S_CARD Moved;
	// 낙관적 동시성은 서비스 안에서 트랜잭션과 함께 돈다 - 드래그 시점 리비전을 넘기지
	// 않는다(삭제 경로와 다른 자리다). before == card 도 서비스가 무동작으로 접는다.
	if (State.pCardService->MoveCard(_sCardId, _sBeforeCardId, &Moved) !=
		app::E_CARD_SERVICE_RESULT::Ok)
	{
		// CEILING: 오류 대화상자("카드 이동 실패")는 W7 주변 UI(error reporter seam) 몫이다 -
		// 원본처럼 목록을 다시 읽지 않고 false 만 돌린다.
		return(false);
	}
	State.refresh_cards();
	State.notify_change();
	return(true);
}

bool C_DOCUMENT_PAGE::DeleteDraggedCard(const std::string& _sCardId)
{
	// 원본 _delete_dragged_card(document_page.py:978~1036).
	auto& State = *m_pState;
	if (State.bCleaned) { return(false); }
	// 1) 기대 리비전은 드래그 시작 시점의 값이다. 없으면 지우지 않고 끝난다.
	// CEILING: 오류 보고("카드 삭제 실패" / "드래그 시작 시점의 카드 리비전을 확인할 수
	// 없습니다.")는 W7 주변 UI(error reporter seam) 몫이다 - 여기서는 false 만 돌린다.
	std::optional<std::string> sExpectedRevisionId = State.CardList.ActiveDragRevision(_sCardId);
	if (!sExpectedRevisionId) { return(false); }
	// 2) 편집기 세션이 이 카드에 연결돼 있으면 그 초안이 discard 대상이다.
	const bool bConnected =
		State.sDraftId && State.sCurrentCardId && *State.sCurrentCardId == _sCardId;
	const std::optional<std::string> sDiscardDraftId =
		bConnected ? State.sDraftId : std::optional<std::string>{};
	bool bDirty = false;
	if (bConnected)
	{
		const auto Session = State.pDraftCoordinator->Session(*State.sDraftId);
		bDirty = Session && Session->bDirty;
	}
	if (!bConnected)
	{
		// 3) 연결돼 있지 않으면 편집 중인 초안을 먼저 보호한다. 실패하면 지우지 않는다.
		if (!this->Protect()) { return(false); }
	}
	else if (bDirty)
	{
		// 4) 연결 + 더티면 3지 선택이다. 연결 + 깨끗하면 프롬프트 없이 바로 지운다.
		E_DRAG_DELETE_CHOICE eChoice = E_DRAG_DELETE_CHOICE::Cancel;
		if (State.DragDelete) { eChoice = State.DragDelete(State.hEditor); }
		else
		{
			// CEILING: 원본 세 버튼 라벨(저장 후 삭제 / 그대로 삭제 / 취소)은 comctl32 v6
			// TaskDialog 가 있어야 하고 이 앱은 v6 매니페스트를 요구하지 않는다 - CanLeave 의
			// 기본 대화상자와 같은 MB_YESNOCANCEL 근사이며 기본 단추는 취소다.
			const int nChoice = ::MessageBoxW(State.hEditor,
				L"저장하지 않은 변경이 있습니다.\n"
				L"예: 저장 후 삭제 / 아니요: 그대로 삭제 / 취소: 삭제하지 않음",
				L"편집 중인 카드 삭제",
				MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON3);
			eChoice = nChoice == IDYES ? E_DRAG_DELETE_CHOICE::Save :
				nChoice == IDNO ? E_DRAG_DELETE_CHOICE::Discard : E_DRAG_DELETE_CHOICE::Cancel;
		}
		if (eChoice == E_DRAG_DELETE_CHOICE::Cancel) { return(false); }
		if (eChoice == E_DRAG_DELETE_CHOICE::Save)
		{
			if (!this->Save()) { return(false); }
			// 저장 뒤에는 리비전이 올라간다 - 기대 리비전을 다시 읽는다.
			domain::S_CARD Saved;
			if (State.pRepositories->GetCard(_sCardId, &Saved) != storage::E_REPO_RESULT::Ok ||
				Saved.nDeletedAtUs) { return(false); }
			sExpectedRevisionId = Saved.sCurrentRevisionId;
		}
	}
	// 5) soft delete. 초안은 discard 로 넘어가므로 아래 해제 경로가 다시 보호하지 않는다.
	domain::S_CARD Deleted;
	if (State.pCardService->SoftDelete(_sCardId, sExpectedRevisionId, false, sDiscardDraftId,
		&Deleted) != app::E_CARD_SERVICE_RESULT::Ok)
	{
		// CEILING: CAS 거부("카드 삭제 거부")와 그 밖의 실패("카드 삭제 실패")를 나누어
		// 알리는 자리는 W7 주변 UI 몫이다.
		return(false);
	}
	// 6) 연결돼 있었으면 삭제된 카드의 세션을 놓고 편집면으로 포커스를 옮긴다.
	if (bConnected)
	{
		State.discard_session_for_deleted_card_();
		::SetFocus(State.hEditor);
	}
	State.refresh_cards();
	State.notify_change();
	return(true);
}

bool C_DOCUMENT_PAGE::RevealCard(const std::string& _sCardId)
{
	if (!m_pState || !m_pState->Projection) { return(false); }
	// 원본은 index 가 유효하지 않으면 취소 앞에서 돌아간다 - 선택도 대기도 그대로다.
	const auto nRow = m_pState->Projection->RowForCard(_sCardId);
	if (!nRow) { return(false); }
	m_pState->CardList.RevealRow(*nRow);
	return(true);
}

bool C_DOCUMENT_PAGE::OpenSelectedCard()
{
	const std::optional<std::string>& sCurrent = m_pState->Projection->CurrentCardId();
	if (!sCurrent || !m_pState->Projection->RowForCard(*sCurrent)) { return(false); }
	// open_card 가 프로젝션의 현재 카드 문자열을 다시 쓰므로 참조로 넘기지 않는다.
	const std::string sCardId = *sCurrent;
	// 원본 Enter 도 card_open_requested 를 거치므로 선택을 다시 잡지 않는다(K4 는 선택을 유지한다).
	return(m_pState->open_card(sCardId, true, false));
}

bool C_DOCUMENT_PAGE::Save()
{
	if (!m_pState->sDraftId) { return(true); }
	const auto Result = m_pState->pSaveCoordinator->Save(*m_pState->sDraftId);
	if (Result.eOutcome != app::E_SAVE_OUTCOME::Saved && Result.eOutcome != app::E_SAVE_OUTCOME::Unchanged)
	{
		// 원본은 카드 저장 실패만 latch 한다(card_editor.py:137~139).
		m_pState->bCardSaveFailed = true;
		return(false);
	}
	m_pState->bCardSaveFailed = false;
	if (Result.Card)
	{
		m_pState->sCurrentCardId = Result.Card->sId;
		m_pState->Projection->UpdateCard(*Result.Card);
		// S5: 저장 경로의 대칭 가드 - false 와 비교한다(spec §3.4.4).
		const bool bWasDirty = m_pState->Projection->IsCardDirty(Result.Card->sId);
		m_pState->Projection->SetCardDirty(Result.Card->sId, false);
		if (bWasDirty) { m_pState->CardList.NotifyCardDirtyChanged(Result.Card->sId); }
	}
	if (!m_pState->refresh_cards()) { return(false); }
	m_pState->notify_change();
	return(true);
}

bool C_DOCUMENT_PAGE::Refresh()
{
	if (!m_pState || m_pState->bCleaned) { return(false); }
	return(m_pState->refresh_cards());
}

void C_DOCUMENT_PAGE::FocusCardList()
{
	::ShowWindow(m_pState->hHistory, SW_HIDE);
	::ShowWindow(m_pState->CardList.m_hWnd, SW_SHOW);
	// S5: 이력 화면에서 카드 화면으로 돌아오면 스트립도 함께 되돌아온다(spec §3.1.4).
	::ShowWindow(m_pState->hSortCombo, SW_SHOW);
	::ShowWindow(m_pState->hSourceFilter, SW_SHOW);
	::ShowWindow(m_pState->hTrashButton, SW_SHOW);
	::SetFocus(m_pState->CardList.m_hWnd);
}

void C_DOCUMENT_PAGE::ShowHistory()
{
	::ShowWindow(m_pState->CardList.m_hWnd, SW_HIDE);
	::ShowWindow(m_pState->hHistory, SW_SHOW);
	// S5: 원본은 mode_stack 전체를 바꿔치기해 스트립이 화면에서 통째로 사라진다 - 네이티브
	// 는 같은 pane 을 공유하므로 명시로 숨긴다(spec §3.1.4, investigation risk 2).
	::ShowWindow(m_pState->hSortCombo, SW_HIDE);
	::ShowWindow(m_pState->hSourceFilter, SW_HIDE);
	::ShowWindow(m_pState->hTrashButton, SW_HIDE);
	::SetFocus(m_pState->hHistory);
}

void C_DOCUMENT_PAGE::ShowFind(bool _bReplace)
{
	::ShowWindow(m_pState->hFind, SW_SHOW);
	::ShowWindow(m_pState->hReplace, _bReplace ? SW_SHOW : SW_HIDE);
	this->Layout();
	::SetFocus(_bReplace ? m_pState->hReplace : m_pState->hFind);
}

HWND C_DOCUMENT_PAGE::CardListHwnd() const noexcept { return(m_pState->CardList.m_hWnd); }
C_CARD_LIST& C_DOCUMENT_PAGE::CardList() const noexcept { return(m_pState->CardList); }
HWND C_DOCUMENT_PAGE::EditorHwnd() const noexcept { return(m_pState->hEditor); }
HWND C_DOCUMENT_PAGE::FindHwnd() const noexcept { return(m_pState->hFind); }
HWND C_DOCUMENT_PAGE::ReplaceHwnd() const noexcept { return(m_pState->hReplace); }
HWND C_DOCUMENT_PAGE::HistoryHwnd() const noexcept { return(m_pState->hHistory); }
HWND C_DOCUMENT_PAGE::SortComboHwnd() const noexcept { return(m_pState->hSortCombo); }
HWND C_DOCUMENT_PAGE::SourceFilterHwnd() const noexcept { return(m_pState->hSourceFilter); }
HWND C_DOCUMENT_PAGE::TrashButtonHwnd() const noexcept { return(m_pState->hTrashButton); }
HWND C_DOCUMENT_PAGE::TooltipHwnd() const noexcept { return(m_pState->hTooltip); }

void C_DOCUMENT_PAGE::SetSortMode(domain::E_CARD_LIST_SORT_MODE _eMode)
{
	auto& State = *m_pState;
	// 같은 값이면 core 를 건드리기 전에 여기서 먼저 돌아간다 - core 자체 조기 반환은
	// CancelPendingBrowse()/OnProjectionChanged() 같은 페이지 수준 부작용을 못 막는다
	// (spec §3.1.6).
	if (State.Projection->SortMode() == _eMode) { return; }
	State.CardList.CancelPendingBrowse();
	State.Projection->SetSortMode(_eMode);
	State.CardList.OnProjectionChanged();
}

void C_DOCUMENT_PAGE::SetSourceFilter(std::optional<domain::E_CARD_SOURCE> _eSource)
{
	auto& State = *m_pState;
	// 원본 card_model.py:362~363 의 같은 값 가드 쌍둥이 - 이유는 SetSortMode 와 같다
	// (spec §3.1.7).
	if (State.eSourceFilter == _eSource) { return; }
	State.eSourceFilter = _eSource;
	State.CardList.CancelPendingBrowse();
	// 콤보는 언제나 원소 1개뿐인 집합 또는 빈 필터만 만든다 - UI 경로는 다중 출처를
	// 만들지 않는다(spec §2 의 CAP-FI-057 정정).
	State.Projection->SetSourceFilter(_eSource ?
		std::optional<std::set<domain::E_CARD_SOURCE>>{ std::set<domain::E_CARD_SOURCE>{ *_eSource } } :
		std::nullopt);
	State.CardList.OnProjectionChanged();
}

std::optional<domain::E_CARD_SOURCE> C_DOCUMENT_PAGE::SourceFilter() const noexcept
{
	return(m_pState->eSourceFilter);
}
bool C_DOCUMENT_PAGE::IsHistoryVisible() const noexcept
{
	return(m_pState->hHistory && ::IsWindowVisible(m_pState->hHistory));
}
bool C_DOCUMENT_PAGE::HasSession() const noexcept
{
	return(m_pState && m_pState->sDraftId.has_value());
}
bool C_DOCUMENT_PAGE::HasDirtySession() const
{
	if (!m_pState->sDraftId) { return(false); }
	const auto Session = m_pState->pDraftCoordinator->Session(*m_pState->sDraftId);
	return(Session && Session->bDirty);
}
bool C_DOCUMENT_PAGE::HasSaveFailed() const noexcept
{
	return(m_pState && m_pState->bCardSaveFailed);
}
