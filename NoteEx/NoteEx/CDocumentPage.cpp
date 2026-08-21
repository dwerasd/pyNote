#include "CDocumentPage.h"

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

#include <CommCtrl.h>
#include <Richedit.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
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
}

struct C_DOCUMENT_PAGE::S_STATE
{
	C_DOCUMENT_PAGE* pOwner{};
	HINSTANCE hInstance{};
	HWND hListHost{};
	HWND hEditorHost{};
	HWND hCardList{};
	HWND hEditor{};
	HWND hFind{};
	HWND hReplace{};
	HWND hHistory{};
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
	std::vector<std::string> ListCardIds;
	C_DOCUMENT_PAGE::LeavePrompt LeavePrompt;
	bool bSynchronizing{ false };
	bool bCleaned{ false };

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

	bool refresh_cards()
	{
		std::vector<domain::S_CARD> Cards;
		if (pCardService->ListActiveCards(sDocumentId, this->service_sort(), &Cards) !=
			app::E_CARD_SERVICE_RESULT::Ok) { return(false); }
		Projection->SetCards(Cards);
		ListCardIds.clear();
		::SendMessageW(hCardList, LB_RESETCONTENT, 0, 0);
		for (std::size_t nRow = 0; nRow < Projection->RowCount(); ++nRow)
		{
			const auto* pCard = Projection->CardAt(nRow);
			if (!pCard) { continue; }
			ListCardIds.push_back(pCard->sId);
			std::wstring Label = wide(pCard->sBody);
			if (Label.empty()) { Label = L"(빈 카드)"; }
			const auto nLine = Label.find_first_of(L"\r\n");
			if (nLine != std::wstring::npos) { Label.resize(nLine); }
			if (Label.size() > 80) { Label.resize(80); }
			::SendMessageW(hCardList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Label.c_str()));
		}
		if (sCurrentCardId)
		{
			const auto it = std::find(ListCardIds.begin(), ListCardIds.end(), *sCurrentCardId);
			if (it != ListCardIds.end())
			{
				::SendMessageW(hCardList, LB_SETCURSEL,
					static_cast<WPARAM>(std::distance(ListCardIds.begin(), it)), 0);
			}
		}
		return(true);
	}

	bool open_card(const std::string& _sCardId, bool _bReplaceEditor)
	{
		if (sDraftId && sCurrentCardId == _sCardId) { ::SetFocus(hEditor); return(true); }
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
		Projection->SetCurrentCardId(Card.sId);
		if (_bReplaceEditor) { this->set_editor_text(Opened.Session->sText); }
		this->refresh_cards();
		::SetFocus(hEditor);
		return(true);
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
		}
		if (!sDraftId) { return(true); }
		const auto Updated = pDraftCoordinator->UpdateSession(*sDraftId, sText, nCursor,
			_eSource == domain::E_CAPTURE_OPERATION_SOURCE::Paste);
		if (Updated.eOutcome != app::E_DRAFT_OUTCOME::Ok) { return(false); }
		if (sCurrentCardId) { Projection->SetCardDirty(*sCurrentCardId, Updated.Session && Updated.Session->bDirty); }
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

	static LRESULT CALLBACK ListSubclass(
		HWND _hWnd, UINT _uMessage, WPARAM _wParam, LPARAM _lParam,
		UINT_PTR, DWORD_PTR _nReference)
	{
		auto* pState = reinterpret_cast<S_STATE*>(_nReference);
		if (_uMessage == WM_NCDESTROY) { ::RemoveWindowSubclass(_hWnd, &S_STATE::ListSubclass, 2); }
		if (pState && _uMessage == WM_KEYDOWN && _wParam == VK_RETURN)
		{
			pState->pOwner->OpenSelectedCard();
			return(0);
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
	State.FirstInput = std::make_unique<app::C_FIRST_INPUT_CAPTURE>(
		_CardService, *State.Projection, State.sDocumentId);
	State.hRichEdit = ::LoadLibraryW(L"Msftedit.dll");
	if (!State.hRichEdit) { return(false); }
	State.hCardList = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
		0, 0, 1, 1, _hListHost, reinterpret_cast<HMENU>(IDC_DOCUMENT_CARD_LIST), _hInstance, nullptr);
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
	if (!State.hCardList || !State.hHistory || !State.hFind || !State.hReplace || !State.hEditor ||
		!::SetWindowSubclass(State.hEditor, &S_STATE::EditorSubclass, 1,
			reinterpret_cast<DWORD_PTR>(&State)) ||
		!::SetWindowSubclass(State.hCardList, &S_STATE::ListSubclass, 2,
			reinterpret_cast<DWORD_PTR>(&State)))
	{
		this->Cleanup();
		return(false);
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
	if (UiState.sSelectedCardId)
	{
		const auto it = std::find(State.ListCardIds.begin(), State.ListCardIds.end(), *UiState.sSelectedCardId);
		if (it != State.ListCardIds.end())
		{
			::SendMessageW(State.hCardList, LB_SETCURSEL,
				static_cast<WPARAM>(std::distance(State.ListCardIds.begin(), it)), 0);
		}
	}
	::SendMessageW(State.hCardList, LB_SETTOPINDEX,
		static_cast<WPARAM>((std::max<std::int64_t>)(0, UiState.nListScrollPosition)), 0);
	if (UiState.sEditorCardId)
	{
		if (!State.open_card(*UiState.sEditorCardId, true)) { this->Cleanup(); return(false); }
		if (UiState.nEditorCursorQchar) { State.set_editor_text(State.editor_text(), *UiState.nEditorCursorQchar); }
	}
	this->Layout();
	::SetFocus(State.hEditor);
	return(true);
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

app::E_LEAVE_RESULT C_DOCUMENT_PAGE::RequestLeave()
{
	if (!m_pState->sDraftId) { this->FocusCardList(); return(app::E_LEAVE_RESULT::ApprovedClean); }
	const std::string sDraftId = *m_pState->sDraftId;
	const auto Session = m_pState->pDraftCoordinator->Session(sDraftId);
	if (!Session) { return(app::E_LEAVE_RESULT::Denied); }
	app::E_LEAVE_RESULT Result = app::E_LEAVE_RESULT::ApprovedClean;
	bool bReleased = false;
	if (Session->bDirty)
	{
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
			Result = app::E_LEAVE_RESULT::ApprovedAfterSave;
		}
		else
		{
			if (m_pState->pDraftCoordinator->DiscardSession(sDraftId).eOutcome != app::E_DRAFT_OUTCOME::Ok)
			{
				return(app::E_LEAVE_RESULT::Denied);
			}
			bReleased = true;
		}
	}
	if (!bReleased && m_pState->pDraftCoordinator->ReleaseSession(sDraftId).eOutcome != app::E_DRAFT_OUTCOME::Ok)
	{
		return(app::E_LEAVE_RESULT::Denied);
	}
	m_pState->sDraftId.reset();
	m_pState->sCurrentCardId.reset();
	m_pState->FirstInput->ResetAfterAcceptedClose();
	m_pState->set_editor_text({});
	this->FocusCardList();
	return(Result);
}

bool C_DOCUMENT_PAGE::PersistState(const std::optional<std::pair<int, int>>& _SplitSizesDip)
{
	if (!m_pState->pDatabase || !m_pState->pRepositories) { return(false); }
	app::S_DOCUMENT_UI_STATE State;
	State.sDocumentId = m_pState->sDocumentId;
	const LRESULT nSelection = ::SendMessageW(m_pState->hCardList, LB_GETCURSEL, 0, 0);
	if (nSelection != LB_ERR && static_cast<std::size_t>(nSelection) < m_pState->ListCardIds.size())
	{
		State.sSelectedCardId = m_pState->ListCardIds[static_cast<std::size_t>(nSelection)];
	}
	State.nListScrollPosition = ::SendMessageW(m_pState->hCardList, LB_GETTOPINDEX, 0, 0);
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
	HWND* Windows[] = { &m_pState->hCardList, &m_pState->hHistory,
		&m_pState->hFind, &m_pState->hReplace, &m_pState->hEditor };
	for (HWND* pWindow : Windows)
	{
		if (::IsWindow(*pWindow)) { ::DestroyWindow(*pWindow); }
		*pWindow = nullptr;
	}
	if (m_pState->hRichEdit) { ::FreeLibrary(m_pState->hRichEdit); m_pState->hRichEdit = nullptr; }
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
	::MoveWindow(m_pState->hCardList, 0, 0, ListClient.right, ListClient.bottom, TRUE);
	::MoveWindow(m_pState->hHistory, 0, 0, ListClient.right, ListClient.bottom, TRUE);
	const bool bFind = ::IsWindowVisible(m_pState->hFind) != FALSE;
	const bool bReplace = ::IsWindowVisible(m_pState->hReplace) != FALSE;
	int nTop = 0;
	if (bFind) { ::MoveWindow(m_pState->hFind, 0, nTop, EditorClient.right, 26, TRUE); nTop += 28; }
	if (bReplace) { ::MoveWindow(m_pState->hReplace, 0, nTop, EditorClient.right, 26, TRUE); nTop += 28; }
	::MoveWindow(m_pState->hEditor, 0, nTop, EditorClient.right,
		(std::max)(1L, EditorClient.bottom - nTop), TRUE);
}

bool C_DOCUMENT_PAGE::OpenSelectedCard()
{
	const LRESULT nSelection = ::SendMessageW(m_pState->hCardList, LB_GETCURSEL, 0, 0);
	return(nSelection != LB_ERR && static_cast<std::size_t>(nSelection) < m_pState->ListCardIds.size() &&
		m_pState->open_card(m_pState->ListCardIds[static_cast<std::size_t>(nSelection)], true));
}

bool C_DOCUMENT_PAGE::Save()
{
	if (!m_pState->sDraftId) { return(true); }
	const auto Result = m_pState->pSaveCoordinator->Save(*m_pState->sDraftId);
	if (Result.eOutcome != app::E_SAVE_OUTCOME::Saved && Result.eOutcome != app::E_SAVE_OUTCOME::Unchanged)
	{
		return(false);
	}
	if (Result.Card)
	{
		m_pState->sCurrentCardId = Result.Card->sId;
		m_pState->Projection->UpdateCard(*Result.Card);
		m_pState->Projection->SetCardDirty(Result.Card->sId, false);
	}
	return(m_pState->refresh_cards());
}

void C_DOCUMENT_PAGE::FocusCardList()
{
	::ShowWindow(m_pState->hHistory, SW_HIDE);
	::ShowWindow(m_pState->hCardList, SW_SHOW);
	::SetFocus(m_pState->hCardList);
}

void C_DOCUMENT_PAGE::ShowHistory()
{
	::ShowWindow(m_pState->hCardList, SW_HIDE);
	::ShowWindow(m_pState->hHistory, SW_SHOW);
	::SetFocus(m_pState->hHistory);
}

void C_DOCUMENT_PAGE::ShowFind(bool _bReplace)
{
	::ShowWindow(m_pState->hFind, SW_SHOW);
	::ShowWindow(m_pState->hReplace, _bReplace ? SW_SHOW : SW_HIDE);
	this->Layout();
	::SetFocus(_bReplace ? m_pState->hReplace : m_pState->hFind);
}

HWND C_DOCUMENT_PAGE::CardListHwnd() const noexcept { return(m_pState->hCardList); }
HWND C_DOCUMENT_PAGE::EditorHwnd() const noexcept { return(m_pState->hEditor); }
HWND C_DOCUMENT_PAGE::FindHwnd() const noexcept { return(m_pState->hFind); }
HWND C_DOCUMENT_PAGE::ReplaceHwnd() const noexcept { return(m_pState->hReplace); }
HWND C_DOCUMENT_PAGE::HistoryHwnd() const noexcept { return(m_pState->hHistory); }
bool C_DOCUMENT_PAGE::IsHistoryVisible() const noexcept
{
	return(m_pState->hHistory && ::IsWindowVisible(m_pState->hHistory));
}
bool C_DOCUMENT_PAGE::HasDirtySession() const
{
	if (!m_pState->sDraftId) { return(false); }
	const auto Session = m_pState->pDraftCoordinator->Session(*m_pState->sDraftId);
	return(Session && Session->bDirty);
}
