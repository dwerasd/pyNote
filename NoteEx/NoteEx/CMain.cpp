#include "framework.h"
#include "CMain.h"

#include "pynote/platform/win32_device_settings.h"


C_MAIN::C_MAIN()
{
}

C_MAIN::~C_MAIN()
{
}

void C_MAIN::save_rect()
{
	// 최소화 상태의 좌표는 복원에 쓸 수 없는 값이라 저장하지 않는다.
	if (this->IsIconic()) { return; }
	if (!m_pSettings) { return; }

	RECT rc{};
	this->GetWindowRect(&rc);

	m_pSettings->SetInt("location/x", rc.left);
	m_pSettings->SetInt("location/y", rc.top);
	m_pSettings->SetInt("location/w", rc.right - rc.left);
	m_pSettings->SetInt("location/h", rc.bottom - rc.top);
	if (!m_pSettings->Sync()) { DBGPRINT(L"창 위치 저장 실패"); }
}

void C_MAIN::move_rect()
{
	RECT rc{};
	this->GetWindowRect(&rc);

	const int nDefaultW = rc.right - rc.left;
	const int nDefaultH = rc.bottom - rc.top;
	const int nW = m_pSettings ? m_pSettings->GetInt("location/w", nDefaultW) : nDefaultW;
	const int nH = m_pSettings ? m_pSettings->GetInt("location/h", nDefaultH) : nDefaultH;
	const int nX = m_pSettings
		? m_pSettings->GetInt("location/x", (::GetSystemMetrics(SM_CXSCREEN) - nW) / 2)
		: (::GetSystemMetrics(SM_CXSCREEN) - nW) / 2;
	const int nY = m_pSettings
		? m_pSettings->GetInt("location/y", (::GetSystemMetrics(SM_CYSCREEN) - nH) / 2)
		: (::GetSystemMetrics(SM_CYSCREEN) - nH) / 2;

	this->MoveWindow(nX, nY, nW, nH);
}

void C_MAIN::render_()
{
	if (!m_bD2DReady) { return; }
	if (!m_mainTarget.BeginDraw()) { return; }

	ID2D1DeviceContext* pDC = m_mainTarget.GetDC();
	pDC->Clear(d2d::ToColorF(0xFFF5F5F5));

	// 골격 확인용 표시. 카드 목록/에디터가 올라오면 이 블록이 교체된다.
	IDWriteTextFormat* pFormat = m_textEngine.GetFormat(L"맑은 고딕", 16.0f);
	ID2D1SolidColorBrush* pBrush = m_brushCache.GetBrush(0xFF202020);
	if (pFormat && pBrush)
	{
		const D2D1_RECT_F rcText = D2D1::RectF(16.0f, 16.0f
			, static_cast<float>(m_mainTarget.GetWidth()) - 16.0f
			, static_cast<float>(m_mainTarget.GetHeight()) - 16.0f);
		pDC->DrawTextW(L"NoteEx", 6, pFormat, rcText, pBrush);
	}

	// EndDraw 실패 = 디바이스 로스트. 브러시는 디바이스 종속이라 먼저 버리고 스왑체인을 다시 만든다.
	if (!m_mainTarget.EndDraw(1))
	{
		m_brushCache.OnDeviceLost();
		if (m_d2dDevice.HandleDeviceLost()) { m_mainTarget.RecreateAfterDeviceLost(); }
	}
}

bool C_MAIN::Init(HINSTANCE _hInstance, pynote::platform::C_WIN32_DEVICE_SETTINGS* _pSettings)
{
	bool bResult = false;
	do
	{
		DBGPRINT(L"C_MAIN::Init(start)");
		m_hInst = _hInstance;
		m_pSettings = _pSettings;
		if (!m_pSettings) { break; }

		if (!g_pLog) { g_pLog = new dk::C_LOG(L"log-NoteEx"); }

		// WTL 초기화
		this->m_Module.Init(nullptr, m_hInst);
		this->m_Module.AddMessageLoop(&m_MsgLoop);

		// D2D 공유 자원은 창 생성 전에 만든다. 스왑타겟은 HWND 가 필요하므로 OnCreate 에서.
		if (!m_d2dDevice.Initialize()) { DBGPRINT(L"D2D 디바이스 초기화 실패"); break; }
		m_brushCache.Initialize(&m_d2dDevice);
		m_textEngine.Initialize(&m_d2dDevice);

		// 메인 프레임 창 생성. 메뉴/아이콘/단축키는 DECLARE_FRAME_WND_CLASS 의 IDC_NOTEEX 리소스에서 온다.
		if (nullptr == this->CreateEx())
		{
			DBGPRINT(L"메인 창 생성 실패 - GetLastError=%u", ::GetLastError());
			break;
		}

		this->move_rect();
		this->ShowWindow(SW_SHOWDEFAULT);
		this->UpdateWindow();

		bResult = true;
	} while (false);

	DBGPRINT(L"C_MAIN::Init(end) - %d", bResult ? 1 : 0);
	return(bResult);
}

int C_MAIN::Display()
{
	return(m_MsgLoop.Run());
}

void C_MAIN::Destroy()
{
	DBGPRINT(L"C_MAIN::Destroy(start)");

	// 스왑타겟은 OnDestroy 에서 이미 내려갔다. 공유 자원은 디바이스보다 먼저 정리한다.
	m_brushCache.Shutdown();
	m_textEngine.Shutdown();
	m_d2dDevice.Shutdown();

	this->m_Module.RemoveMessageLoop();
	this->m_Module.Term();

	if (g_pConfig) { delete g_pConfig; g_pConfig = nullptr; }
	// g_pLog 는 종료 직전까지 로깅에 쓰이므로 유지한다(프로세스 종료 시 OS 회수).

	DBGPRINT(L"C_MAIN::Destroy(end)");
}

LRESULT C_MAIN::OnCreate(UINT /*_uMsg*/, WPARAM /*_wParam*/, LPARAM /*_lParam*/, BOOL& _bHandled)
{
	DBGPRINT(L"C_MAIN::OnCreate()");
	this->SetWindowText(L"NoteEx");

	// 아이콘 설정(대/소)
	HICON hIcon = (HICON)::LoadImageW(m_hInst, MAKEINTRESOURCEW(IDI_NOTEEX), IMAGE_ICON
		, ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
	if (hIcon) { this->SetIcon(hIcon, TRUE); }
	HICON hIconSmall = (HICON)::LoadImageW(m_hInst, MAKEINTRESOURCEW(IDI_SMALL), IMAGE_ICON
		, ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
	if (hIconSmall) { this->SetIcon(hIconSmall, FALSE); }

	this->ModifyStyle(0, WS_CLIPCHILDREN);	// 자식 영역을 프레임이 덮어그리지 않게

	// 창별 스왑타겟. 실패하면 GDI 폴백으로 계속 뜬다(창 자체는 살린다).
	if (m_mainTarget.Initialize(&m_d2dDevice, this->m_hWnd)) { m_bD2DReady = true; }
	else { DBGPRINT(L"D2D 스왑타겟 초기화 실패 - GDI 폴백"); }

	_bHandled = FALSE;	// 프레임 기본 처리 유지
	return(0);
}

LRESULT C_MAIN::OnClose(UINT /*_uMsg*/, WPARAM /*_wParam*/, LPARAM /*_lParam*/, BOOL& _bHandled)
{
	this->save_rect();	// 창이 파괴되기 전에 위치를 영속한다.

	_bHandled = FALSE;
	return(0);
}

LRESULT C_MAIN::OnDestroy(UINT /*_uMsg*/, WPARAM /*_wParam*/, LPARAM /*_lParam*/, BOOL& _bHandled)
{
	DBGPRINT(L"C_MAIN::OnDestroy()");

	m_bD2DReady = false;
	m_mainTarget.Shutdown();	// 스왑체인은 HWND 가 살아 있을 때 내린다.

	::PostQuitMessage(0);

	_bHandled = FALSE;
	return(0);
}

LRESULT C_MAIN::OnSize(UINT /*_uMsg*/, WPARAM /*_wParam*/, LPARAM /*_lParam*/, BOOL& _bHandled)
{
	if (m_bD2DReady)
	{
		RECT rc{};
		this->GetClientRect(&rc);
		m_mainTarget.Resize(static_cast<UINT>(rc.right - rc.left), static_cast<UINT>(rc.bottom - rc.top));
		this->render_();
	}

	_bHandled = FALSE;
	return(0);
}

LRESULT C_MAIN::OnPaint(UINT /*_uMsg*/, WPARAM /*_wParam*/, LPARAM /*_lParam*/, BOOL& /*_bHandled*/)
{
	if (m_bD2DReady)
	{
		this->render_();
		::ValidateRect(this->m_hWnd, nullptr);
		return(0);
	}

	// D2D 미준비 폴백 - 빈 셸이라도 흰 배경으로 그린다.
	CPaintDC dc(this->m_hWnd);
	RECT rc{};
	this->GetClientRect(&rc);
	dc.FillSolidRect(&rc, ::GetSysColor(COLOR_WINDOW));

	return(0);
}

LRESULT C_MAIN::OnMenuExit(WORD /*_wNotifyCode*/, WORD /*_wID*/, HWND /*_hWndCtl*/, BOOL& /*_bHandled*/)
{
	this->PostMessageW(WM_CLOSE);
	return(0);
}

LRESULT C_MAIN::OnMenuAbout(WORD /*_wNotifyCode*/, WORD /*_wID*/, HWND /*_hWndCtl*/, BOOL& /*_bHandled*/)
{
	CSimpleDialog<IDD_ABOUTBOX> dlg;
	dlg.DoModal(this->m_hWnd);
	return(0);
}
