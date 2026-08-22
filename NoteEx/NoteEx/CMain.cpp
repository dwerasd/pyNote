#include "framework.h"
#include "CMain.h"

#include "CApplication.h"
#include "CCardList.h"
#include "pynote/platform/win32_device_settings.h"

#include <algorithm>
#include <CommCtrl.h>
#include <limits>
#include <utility>

namespace
{
	constexpr UINT LEFT_PANE_ID = 2001;
	constexpr UINT EDITOR_PANE_ID = 2002;

	std::wstring wide(const std::string& _sValue)
	{
		if (_sValue.empty()) { return(std::wstring{}); }
		const int nSize = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			_sValue.data(), static_cast<int>(_sValue.size()), nullptr, 0);
		if (nSize <= 0) { return(std::wstring{}); }
		std::wstring Result(static_cast<std::size_t>(nSize), L'\0');
		if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, _sValue.data(),
			static_cast<int>(_sValue.size()), Result.data(), nSize) != nSize)
		{
			return(std::wstring{});
		}
		return(Result);
	}

	// 카드 목록 렌더 서비스·표시 설정 배선. 디바이스·브러시 캐시·텍스트 엔진은 CApplication
	// 소유이고 창이 서기 전에 이미 초기화돼 있다(CApplication.cpp:681~688).
	void bind_card_list(C_DOCUMENT_PAGE& _Page, CApplication& _Application)
	{
		_Page.SetRenderServices(&_Application.D2DDevice(), &_Application.BrushCache(),
			&_Application.TextEngine());
		S_CARD_LIST_DISPLAY Display;
		std::wstring sValue;
		if (_Application.Settings().GetString("display/time_format", &sValue) && !sValue.empty())
		{
			Display.sTimeFormat = sValue;
		}
		if (_Application.Settings().GetString("display/timezone", &sValue) && !sValue.empty())
		{
			Display.sTimeZone = sValue;
		}
		_Page.SetDisplaySettings(Display);
	}
}

std::vector<ACCEL> C_MAIN::RuntimeAccelerators()
{
	return(pynote::shell::RuntimeAccelerators());
}

LRESULT C_MAIN_PANE_HOST::OnPaint(UINT, WPARAM, LPARAM, BOOL&)
{
	if (m_bEditor && m_pOwner)
	{
		m_pOwner->render_();
		::ValidateRect(this->m_hWnd, nullptr);
		return(0);
	}
	CPaintDC Dc(this->m_hWnd);
	RECT Frame{};
	this->GetClientRect(&Frame);
	Dc.FillSolidRect(&Frame, RGB(238, 241, 245));
	return(0);
}

bool C_MAIN::save_geometry()
{
	if (this->IsIconic() || !m_pApplication) { return(true); }
	pynote::shell::S_WINDOW_GEOMETRY Geometry;
	if (!pynote::shell::CaptureWindowGeometry(this->m_hWnd, &Geometry)) { return(false); }
	const std::vector<std::uint8_t> Bytes = pynote::shell::EncodeWindowGeometry(Geometry);
	const std::string sKey = pynote::shell::WindowGeometryKey(m_sWorkspaceId);
	if (Bytes.empty() || sKey.empty()) { return(false); }
	auto& Settings = m_pApplication->Settings();
	return(Settings.SetBytes(sKey, Bytes) && Settings.Sync());
}

bool C_MAIN::restore_geometry(bool _bAllowLegacyFallback, bool* _pbMaximized)
{
	if (!_pbMaximized || !m_pApplication) { return(false); }
	*_pbMaximized = false;
	const DWORD nStyle = static_cast<DWORD>(::GetWindowLongPtrW(this->m_hWnd, GWL_STYLE));
	const DWORD nExStyle = static_cast<DWORD>(::GetWindowLongPtrW(this->m_hWnd, GWL_EXSTYLE));
	const bool bHasMenu = ::GetMenu(this->m_hWnd) != nullptr;
	const auto WorkAreas = pynote::shell::EnumerateMonitorWorkAreas();
	RECT Frame{};
	bool bAdmitted = false;

	auto& Settings = m_pApplication->Settings();
	std::vector<std::uint8_t> Bytes;
	pynote::shell::S_WINDOW_GEOMETRY Geometry;
	const std::string sKey = pynote::shell::WindowGeometryKey(m_sWorkspaceId);
	if (!sKey.empty() && Settings.GetBytes(sKey, &Bytes) &&
		pynote::shell::DecodeWindowGeometry(Bytes, &Geometry))
	{
		const POINT SavedOrigin{ Geometry.nFrameXpx, Geometry.nFrameYpx };
		const UINT nRestoreDpi = pynote::shell::GetMonitorDpiForPoint(SavedOrigin, Geometry.nDpi);
		Frame = pynote::shell::MakeFrameRectForClientDips(
			{ Geometry.nFrameXpx, Geometry.nFrameYpx },
			{ Geometry.nClientWidthDip, Geometry.nClientHeightDip },
			nRestoreDpi, nStyle, nExStyle, bHasMenu);
		bAdmitted = pynote::shell::IntersectsMonitorWorkArea(Frame, WorkAreas);
		if (bAdmitted) { *_pbMaximized = Geometry.bMaximized; }
	}

	if (_bAllowLegacyFallback && !bAdmitted &&
		Settings.Contains("location/x") && Settings.Contains("location/y") &&
		Settings.Contains("location/w") && Settings.Contains("location/h"))
	{
		const int nUnset = (std::numeric_limits<int>::min)();
		const int nX = Settings.GetInt("location/x", nUnset);
		const int nY = Settings.GetInt("location/y", nUnset);
		const int nWidth = Settings.GetInt("location/w", nUnset);
		const int nHeight = Settings.GetInt("location/h", nUnset);
		if (nX != nUnset && nY != nUnset && nWidth > 0 && nHeight > 0)
		{
			Frame = { nX, nY, nX + nWidth, nY + nHeight };
			bAdmitted = pynote::shell::IntersectsMonitorWorkArea(Frame, WorkAreas);
		}
	}

	if (!bAdmitted)
	{
		RECT WorkArea{};
		UINT nDpi = USER_DEFAULT_SCREEN_DPI;
		if (!pynote::shell::GetMonitorWorkAreaForWindow(this->m_hWnd, &WorkArea, &nDpi)) { return(false); }
		Frame = pynote::shell::MakeCenteredDefaultFrame(WorkArea, nDpi, nStyle, nExStyle, bHasMenu);
		*_pbMaximized = false;
	}

	return(::SetWindowPos(this->m_hWnd, nullptr, Frame.left, Frame.top,
		Frame.right - Frame.left, Frame.bottom - Frame.top,
		SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER) != FALSE);
}

void C_MAIN::layout_children()
{
	if (!m_Splitter.IsWindow()) { return; }
	RECT Client{};
	this->GetClientRect(&Client);
	if (m_hStatus && ::IsWindowVisible(m_hStatus))
	{
		::SendMessageW(m_hStatus, WM_SIZE, 0, 0);
		RECT Status{};
		::GetWindowRect(m_hStatus, &Status);
		Client.bottom -= Status.bottom - Status.top;
	}
	m_Splitter.MoveWindow(&Client, TRUE);
	m_DocumentPage.Layout();
	if (m_bD2DReady && m_EditorPane.IsWindow())
	{
		RECT Editor{};
		m_EditorPane.GetClientRect(&Editor);
		m_mainTarget.Resize(
			static_cast<UINT>((std::max)(1L, Editor.right - Editor.left)),
			static_cast<UINT>((std::max)(1L, Editor.bottom - Editor.top)));
	}
}

void C_MAIN::render_()
{
	if (!m_bD2DReady) { return; }
	if (!m_mainTarget.BeginDraw()) { return; }
	ID2D1DeviceContext* pDC = m_mainTarget.GetDC();
	pDC->Clear(d2d::ToColorF(0xFFF5F5F5));
	IDWriteTextFormat* pFormat = m_pApplication->TextEngine().GetFormat(L"맑은 고딕", 16.0f);
	ID2D1SolidColorBrush* pBrush = m_pApplication->BrushCache().GetBrush(0xFF202020);
	const D2D1_SIZE_F Size = m_mainTarget.GetSizeDips();
	if (pFormat && pBrush && Size.width > 0.0f && Size.height > 0.0f)
	{
		const D2D1_RECT_F TextFrame = D2D1::RectF(
			16.0f, 16.0f, (std::max)(16.0f, Size.width - 16.0f),
			(std::max)(16.0f, Size.height - 16.0f));
		pDC->DrawTextW(m_sTitle.c_str(), static_cast<UINT32>(m_sTitle.size()),
			pFormat, TextFrame, pBrush);
	}
	if (!m_mainTarget.EndDraw(1))
	{
		m_pApplication->BrushCache().OnDeviceLost();
		if (m_pApplication->D2DDevice().HandleDeviceLost()) { m_mainTarget.RecreateAfterDeviceLost(); }
	}
}

void C_MAIN::subscribe_change_bus_()
{
	if (!m_pApplication || m_ChangeSubscription != 0) { return; }
	m_ChangeSubscription = m_pApplication->ChangeBus().Subscribe(
		[this](const std::string& _sDocumentId) { this->on_document_changed_(_sDocumentId); });
}

void C_MAIN::unsubscribe_change_bus_()
{
	if (!m_pApplication || m_ChangeSubscription == 0) { return; }
	m_pApplication->ChangeBus().Unsubscribe(m_ChangeSubscription);
	m_ChangeSubscription = 0;
}

// 원본 apply_document_change(main_window.py:543~564) + _remove_page_after_change(:584~601).
void C_MAIN::on_document_changed_(const std::string& _sDocumentId)
{
	if (m_bCleaned || !m_pApplication || !m_sDocumentId) { return; }
	if (*m_sDocumentId != _sDocumentId) { return; }
	const auto eChange = pynote::shell::ClassifyDocumentChange(
		m_pApplication->Repositories(), _sDocumentId);
	if (!eChange)
	{
		// 분류가 아니라 관측 실패다 - 살아 있는 페이지를 저장 없이 버리지 않는다.
		DBGPRINT(L"문서 변경 분류에 실패했습니다");
		return;
	}
	if (*eChange == pynote::shell::E_DOCUMENT_CHANGE::Alive)
	{
		this->update_title_();
		if (!m_bPublishingPageContentChange) { m_DocumentPage.Refresh(); }
		m_pApplication->PersistWindowState(m_Token, m_sWorkspaceId, m_sDocumentId);
		this->update_status_();
		return;
	}
	// save_ui_state 는 문서 행이 남아 있을 때만 참이다(RemovedSaveUi).
	if (*eChange == pynote::shell::E_DOCUMENT_CHANGE::RemovedSaveUi)
	{
		if (!m_DocumentPage.PersistState(m_Splitter.SplitSizesDip()))
		{
			DBGPRINT(L"소멸 문서의 UI 상태 저장에 실패했습니다");
		}
	}
	m_DocumentPage.Cleanup();
	this->refill_after_document_removal_();
}

// 원본 _handle_page_content_changed(main_window.py:1107~1115).
void C_MAIN::on_page_content_changed_()
{
	if (!m_bCleaned && m_pApplication && m_sDocumentId)
	{
		// 원본의 try/finally 등가 - 발행이 예외로 탈출해도 가드가 true 로 굳으면
		// 이 창의 페이지 재채움이 세션 내내 무력화된다.
		m_bPublishingPageContentChange = true;
		struct S_GUARD
		{
			bool& bFlag;
			~S_GUARD() { bFlag = false; }
		} Guard{ m_bPublishingPageContentChange };
		const bool bPublished = m_pApplication->PublishDocumentChange(*m_sDocumentId);
		if (!bPublished) { DBGPRINT(L"문서 변경 발행 뒤 소유 매핑 재계산에 실패했습니다"); }
	}
	this->update_status_();
}

void C_MAIN::update_title_()
{
	std::optional<std::wstring> sDocumentTitle;
	if (m_pApplication && m_sDocumentId)
	{
		const auto sTitle = m_pApplication->DocumentTitle(*m_sDocumentId);
		if (sTitle) { sDocumentTitle = wide(*sTitle); }
	}
	m_sTitle = pynote::shell::ComposeWindowTitle(sDocumentTitle);
	if (::IsWindow(this->m_hWnd)) { this->SetWindowText(m_sTitle.c_str()); }
}

// 원본 _update_status(main_window.py:711~732). 문안 조립은 전부 seam 이 한다.
void C_MAIN::update_status_()
{
	if (!m_pApplication || !m_hStatus || !::IsWindow(m_hStatus)) { return; }
	std::wstring sText;
	if (!m_sDocumentId) { sText = pynote::shell::ComposeEmptyStatusText(); }
	else
	{
		const auto Stats = pynote::shell::CountActiveCards(
			m_pApplication->Repositories(), *m_sDocumentId);
		if (!Stats)
		{
			// 계수 실패를 0 으로 접으면 실패가 정상 상태로 위장된다 - 기존 문안을 둔다.
			DBGPRINT(L"상태 바 카드 계수에 실패했습니다");
			return;
		}
		sText = pynote::shell::ComposeStatusText(Stats->nCards, Stats->nCodepoints,
			pynote::shell::ComposeSaveStateText(m_DocumentPage.HasSession(),
				m_DocumentPage.HasDirtySession(), m_DocumentPage.HasSaveFailed()));
	}
	::SendMessageW(m_hStatus, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(sText.c_str()));
}

// 원본 _refill_after_document_removal(main_window.py:603~) - 시스템 주도 회수로
// 비워진 창을 즉시 입력 가능한 문서로 다시 채운다.
// CEILING: registry 항목의 문서 식별자는 재채움 뒤에도 옛 문서를 가리킨다(core 무수정
// 계약이라 창 재결속 API 가 없다). 소유 판정은 CApplication 이 살아 있는 창의
// DocumentId() 를 함께 보아 메운다 - 재결속 API 는 W7 에서 core 와 함께 연다.
bool C_MAIN::refill_after_document_removal_()
{
	if (!m_pApplication) { return(false); }
	const auto sRefillId = m_pApplication->ChooseRefillDocument(m_Token);
	if (!sRefillId)
	{
		m_sDocumentId.reset();
		this->update_title_();
		this->update_status_();
		return(false);
	}
	m_sDocumentId = *sRefillId;
	bind_card_list(m_DocumentPage, *m_pApplication);
	if (!m_DocumentPage.Init(
		m_hInst, m_LeftPane, m_EditorPane,
		m_pApplication->Database(), m_pApplication->Repositories(), m_pApplication->CardService(),
		m_pApplication->DraftCoordinator(), m_pApplication->SaveCoordinator(),
		m_sWorkspaceId, *m_sDocumentId))
	{
		DBGPRINT(L"재채움 페이지 생성에 실패했습니다");
		return(false);
	}
	m_DocumentPage.SetChangeNotifier([this]() { this->on_page_content_changed_(); });
	m_pApplication->PersistWindowState(m_Token, m_sWorkspaceId, m_sDocumentId);
	this->update_title_();
	this->layout_children();
	// 발행 트리거 - 창의 문서 재채움이다. 발행을 시작한 창은 재진입 가드 덕에 방금 만든
	// 페이지를 다시 읽지 않고, 상태 바 갱신은 그 안에서 함께 돈다.
	this->on_page_content_changed_();
	return(true);
}

bool C_MAIN::Init(
	HINSTANCE _hInstance, CApplication* _pApplication,
	pynote::core::application::WINDOW_TOKEN _Token,
	std::string _sWorkspaceId, std::optional<std::string> _sDocumentId,
	std::wstring _sTitle, bool _bAllowLegacyGeometryFallback)
{
	m_hInst = _hInstance;
	m_pApplication = _pApplication;
	m_Token = _Token;
	m_sWorkspaceId = std::move(_sWorkspaceId);
	m_sDocumentId = std::move(_sDocumentId);
	m_sTitle = std::move(_sTitle);
	if (!m_pApplication || m_Token == 0 || m_sWorkspaceId.empty() || !m_sDocumentId) { return(false); }
	std::optional<std::pair<int, int>> SplitSizes;
	if (!m_pApplication->LoadDocumentSplit(m_sWorkspaceId, *m_sDocumentId, &SplitSizes)) { return(false); }
	m_Splitter.SetSplitSizesDip(SplitSizes);
	if (nullptr == this->CreateEx()) { return(false); }
	bool bMaximized = false;
	if (!this->restore_geometry(_bAllowLegacyGeometryFallback, &bMaximized))
	{
		this->DestroyWindow();
		return(false);
	}
	this->ShowWindow(bMaximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
	this->UpdateWindow();
	return(true);
}

bool C_MAIN::Protect()
{
	return(m_DocumentPage.Protect());
}

pynote::core::application::E_LEAVE_RESULT C_MAIN::RequestLeave()
{
	// 창 닫기·앱 종료 참가자의 이탈 단계는 승인만이다(원본 closeEvent: can_leave_open_pages 뒤
	// persist_open_page_ui_states) - 세션 해제는 persist 뒤 Cleanup 이 맡는다.
	return(m_DocumentPage.CanLeave());
}

bool C_MAIN::PersistState()
{
	const bool bPage = m_DocumentPage.PersistState(m_Splitter.SplitSizesDip());
	const bool bWorkspace = m_pApplication &&
		m_pApplication->PersistWindowState(m_Token, m_sWorkspaceId, m_sDocumentId);
	const bool bGeometry = this->save_geometry();
	return(bPage && bWorkspace && bGeometry);
}

bool C_MAIN::Cleanup()
{
	if (m_bCleaned) { return(true); }
	// 창은 파괴 전에 구독을 해제한다(PLAN-W3-0042).
	this->unsubscribe_change_bus_();
	const bool bPage = m_DocumentPage.Cleanup();
	m_DocumentListShell.Destroy();
	m_bD2DReady = false;
	m_mainTarget.Shutdown();
	if (m_hRuntimeMenu)
	{
		if (::GetMenu(this->m_hWnd) == m_hRuntimeMenu) { ::SetMenu(this->m_hWnd, nullptr); }
		::DestroyMenu(m_hRuntimeMenu);
		m_hRuntimeMenu = nullptr;
	}
	m_hStatus = nullptr;
	m_bCleaned = true;
	return(bPage);
}

void C_MAIN::DestroyNative()
{
	if (::IsWindow(this->m_hWnd)) { this->DestroyWindow(); }
}

bool C_MAIN::PreTranslateMessage(MSG* _pMessage)
{
	return(m_DocumentPage.PreTranslateMessage(_pMessage));
}

LRESULT C_MAIN::OnCreate(UINT, WPARAM, LPARAM, BOOL& _bHandled)
{
	this->SetWindowText(m_sTitle.c_str());
	const HMENU hResourceMenu = this->GetMenu();
	m_hRuntimeMenu = pynote::shell::CreateRuntimeMenu();
	if (!m_hRuntimeMenu || !::SetMenu(this->m_hWnd, m_hRuntimeMenu))
	{
		return(-1);
	}
	if (hResourceMenu && hResourceMenu != m_hRuntimeMenu) { ::DestroyMenu(hResourceMenu); }
	HICON hIcon = reinterpret_cast<HICON>(::LoadImageW(m_hInst, MAKEINTRESOURCEW(IDI_NOTEEX), IMAGE_ICON,
		::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
	if (hIcon) { this->SetIcon(hIcon, TRUE); }
	HICON hIconSmall = reinterpret_cast<HICON>(::LoadImageW(m_hInst, MAKEINTRESOURCEW(IDI_SMALL), IMAGE_ICON,
		::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
	if (hIconSmall) { this->SetIcon(hIconSmall, FALSE); }
	this->ModifyStyle(0, WS_CLIPCHILDREN);

	RECT Empty{};
	if (nullptr == m_Splitter.Create(
		this->m_hWnd, Empty, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS))
	{
		return(-1);
	}
	m_LeftPane.Initialize(this, false);
	m_EditorPane.Initialize(this, true);
	// 페이지 실자식(목록·편집기·찾기/바꾸기·이력)이 pane 을 채우므로 WS_CLIPCHILDREN 이
	// 없으면 pane 배경 페인트(D2D render_ 포함)가 자식 표면을 덮어 그린다.
	if (nullptr == m_LeftPane.Create(
		m_Splitter, Empty, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
		0, LEFT_PANE_ID) ||
		nullptr == m_EditorPane.Create(
			m_Splitter, Empty, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
			0, EDITOR_PANE_ID))
	{
		return(-1);
	}
	m_Splitter.SetSplitterPanes(m_LeftPane, m_EditorPane, false);
	m_Splitter.SetDpi(::GetDpiForWindow(this->m_hWnd), false);
	m_hStatus = ::CreateWindowExW(0, STATUSCLASSNAMEW, L"준비",
		WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
		this->m_hWnd, reinterpret_cast<HMENU>(IDC_MAIN_STATUS), m_hInst, nullptr);
	bind_card_list(m_DocumentPage, *m_pApplication);
	if (!m_hStatus || !m_DocumentPage.Init(
		m_hInst, m_LeftPane, m_EditorPane,
		m_pApplication->Database(), m_pApplication->Repositories(), m_pApplication->CardService(),
		m_pApplication->DraftCoordinator(), m_pApplication->SaveCoordinator(),
		m_sWorkspaceId, *m_sDocumentId) ||
		!m_DocumentListShell.Initialize(m_hInst, this->m_hWnd))
	{
		return(-1);
	}
	m_DocumentListShell.Show();
	// 카드 생성·저장 완료가 페이지 통지 콜백을 거쳐 앱 버스 발행으로 간다.
	m_DocumentPage.SetChangeNotifier([this]() { this->on_page_content_changed_(); });
	this->subscribe_change_bus_();
	this->update_title_();
	this->update_status_();
	this->layout_children();
	if (m_mainTarget.Initialize(&m_pApplication->D2DDevice(), m_EditorPane)) { m_bD2DReady = true; }
	_bHandled = FALSE;
	return(0);
}

LRESULT C_MAIN::OnClose(UINT, WPARAM, LPARAM, BOOL& _bHandled)
{
	_bHandled = TRUE;
	if (m_pApplication) { m_pApplication->RequestCloseWindow(m_Token); }
	return(0);
}

LRESULT C_MAIN::OnDestroy(UINT, WPARAM, LPARAM, BOOL& _bHandled)
{
	this->Cleanup();
	_bHandled = TRUE;
	return(0);
}

LRESULT C_MAIN::OnNcDestroy(UINT, WPARAM, LPARAM, BOOL& _bHandled)
{
	// 생성 실패 경로(Init 의 DestroyWindow)는 Cleanup 을 거치지 않는다 - HWND 가 죽는
	// 모든 경로가 지나는 이 지점에서 해제해야 버스에 죽은 this 캡처가 남지 않는다.
	// 정상 종료는 Cleanup 이 먼저 해제하므로 토큰 0 재해제는 무해하다(멱등).
	this->unsubscribe_change_bus_();
	if (m_pApplication) { m_pApplication->NotifyWindowNcDestroy(m_Token); }
	_bHandled = FALSE;
	return(0);
}

LRESULT C_MAIN::OnActivate(UINT, WPARAM _wParam, LPARAM, BOOL& _bHandled)
{
	if (LOWORD(_wParam) != WA_INACTIVE && m_pApplication)
	{
		m_pApplication->NotifyWindowActivated(m_Token);
	}
	_bHandled = FALSE;
	return(0);
}

LRESULT C_MAIN::OnSize(UINT, WPARAM _wParam, LPARAM, BOOL& _bHandled)
{
	if (_wParam != SIZE_MINIMIZED)
	{
		this->layout_children();
		this->render_();
	}
	_bHandled = FALSE;
	return(0);
}

LRESULT C_MAIN::OnDpiChanged(UINT, WPARAM _wParam, LPARAM _lParam, BOOL& _bHandled)
{
	const UINT nDpi = HIWORD(_wParam);
	if (m_bD2DReady) { m_mainTarget.SetDpi(static_cast<float>(nDpi)); }
	m_Splitter.SetDpi(nDpi, false);
	const auto* pSuggested = reinterpret_cast<const RECT*>(_lParam);
	if (pSuggested)
	{
		::SetWindowPos(this->m_hWnd, nullptr, pSuggested->left, pSuggested->top,
			pSuggested->right - pSuggested->left, pSuggested->bottom - pSuggested->top,
			SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER);
	}
	this->layout_children();
	_bHandled = TRUE;
	return(0);
}

LRESULT C_MAIN::OnPaint(UINT, WPARAM, LPARAM, BOOL&)
{
	CPaintDC Dc(this->m_hWnd);
	RECT Client{};
	this->GetClientRect(&Client);
	Dc.FillSolidRect(&Client, ::GetSysColor(COLOR_WINDOW));
	return(0);
}

LRESULT C_MAIN::OnResetGeometry(WORD, WORD, HWND, BOOL&)
{
	if (pynote::shell::ResetWindowGeometry(this->m_hWnd)) { this->save_geometry(); }
	return(0);
}

LRESULT C_MAIN::OnNewWindow(WORD, WORD, HWND, BOOL&)
{
	if (m_pApplication) { m_pApplication->CreateMainWindow(); }
	return(0);
}

LRESULT C_MAIN::OnDocumentList(WORD, WORD, HWND, BOOL&)
{
	if (!m_bFocusMode) { m_DocumentListShell.Show(); }
	return(0);
}

LRESULT C_MAIN::OnGlobalSearch(WORD, WORD, HWND, BOOL&)
{
	if (m_pApplication) { m_pApplication->ShowSearchDialog(this->m_hWnd); }
	return(0);
}

LRESULT C_MAIN::OnSaveCard(WORD, WORD, HWND, BOOL&)
{
	m_DocumentPage.Save();
	return(0);
}

LRESULT C_MAIN::OnFind(WORD, WORD, HWND, BOOL&)
{
	m_DocumentPage.ShowFind(false);
	return(0);
}

LRESULT C_MAIN::OnReplace(WORD, WORD, HWND, BOOL&)
{
	m_DocumentPage.ShowFind(true);
	return(0);
}

LRESULT C_MAIN::OnCardList(WORD, WORD, HWND, BOOL&)
{
	m_DocumentPage.FocusCardList();
	return(0);
}

LRESULT C_MAIN::OnHistory(WORD, WORD, HWND, BOOL&)
{
	m_DocumentPage.ShowHistory();
	return(0);
}

LRESULT C_MAIN::OnBack(WORD, WORD, HWND, BOOL&)
{
	m_DocumentPage.RequestLeave();
	return(0);
}

LRESULT C_MAIN::OnFocusMode(WORD, WORD, HWND, BOOL&)
{
	m_bFocusMode = !m_bFocusMode;
	if (!pynote::shell::ApplyFocusMode(
		this->m_hWnd, m_hRuntimeMenu, m_hStatus, m_DocumentListShell, m_bFocusMode))
	{
		m_bFocusMode = !m_bFocusMode;
		return(0);
	}
	this->layout_children();
	return(0);
}

LRESULT C_MAIN::OnMenuExit(WORD, WORD, HWND, BOOL&)
{
	if (m_pApplication) { m_pApplication->RequestApplicationQuit(); }
	return(0);
}

LRESULT C_MAIN::OnMenuAbout(WORD, WORD, HWND, BOOL&)
{
	CSimpleDialog<IDD_ABOUTBOX> dlg;
	dlg.DoModal(this->m_hWnd);
	return(0);
}
