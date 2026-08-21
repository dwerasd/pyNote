#include "framework.h"
#include "CMain.h"

#include "CApplication.h"
#include "pynote/platform/win32_device_settings.h"

#include <algorithm>
#include <CommCtrl.h>
#include <limits>
#include <utility>

namespace
{
	constexpr UINT LEFT_PANE_ID = 2001;
	constexpr UINT EDITOR_PANE_ID = 2002;

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
	return(m_DocumentPage.RequestLeave());
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
	if (nullptr == m_LeftPane.Create(
		m_Splitter, Empty, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, LEFT_PANE_ID) ||
		nullptr == m_EditorPane.Create(
			m_Splitter, Empty, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, EDITOR_PANE_ID))
	{
		return(-1);
	}
	m_Splitter.SetSplitterPanes(m_LeftPane, m_EditorPane, false);
	m_Splitter.SetDpi(::GetDpiForWindow(this->m_hWnd), false);
	m_hStatus = ::CreateWindowExW(0, STATUSCLASSNAMEW, L"준비",
		WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
		this->m_hWnd, reinterpret_cast<HMENU>(IDC_MAIN_STATUS), m_hInst, nullptr);
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
