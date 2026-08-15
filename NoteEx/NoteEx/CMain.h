#pragma once


#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>
#include <atlframe.h>
#include <atlcrack.h>
#include <atlgdi.h>
#include <atluser.h>
#include <atldlgs.h>

// D2D 렌더 백엔드(자가 라이브러리). 카드 목록/본문 페인팅의 페인트 백엔드다.
#include <D2DWrapp/D2DDevice.h>
#include <D2DWrapp/D2DSwapTarget.h>
#include <D2DWrapp/D2DBrushCache.h>
#include <D2DWrapp/D2DText.h>
#pragma comment(lib, "D2DWrapp")

#include "Resource.h"


// 메인 프레임 창. DBGView 의 C_MAIN 스타일(C_SINGLETON + WTL CAppModule/CMessageLoop +
// Init/Display/Destroy)을 그대로 따르는 초기 골격이다.
// 본문은 D2D 로 그린다 - 카드 목록/에디터는 이 렌더 경로 위에 올린다.
class C_MAIN
	: public dk::C_SINGLETON<C_MAIN>
	, public CFrameWindowImpl<C_MAIN>
{
private:
	HINSTANCE		m_hInst{ nullptr };

	CAppModule		m_Module;
	CMessageLoop	m_MsgLoop;

	// D2D 자원. 디바이스/브러시/텍스트는 창 간 공유 대상이고, 스왑타겟은 창별로 하나다.
	d2d::C_D2D_DEVICE		m_d2dDevice;
	d2d::C_D2D_BRUSH_CACHE	m_brushCache;
	d2d::C_D2D_TEXT			m_textEngine;
	d2d::C_D2D_SWAP_TARGET	m_mainTarget;
	bool					m_bD2DReady{ false };

	void save_rect();	// 창 위치/크기 INI 영속(최소화 상태 제외)
	void move_rect();	// INI 위치/크기 복원(값이 없으면 화면 중앙)
	void render_();		// D2D 프레임 1회(BeginDraw - 본문 - EndDraw, 디바이스 로스트 복구 포함)

public:
	DECLARE_FRAME_WND_CLASS(L"NoteExMainWindow", IDC_NOTEEX)

	C_MAIN();
	~C_MAIN();

	bool Init(HINSTANCE _hInstance);
	int  Display();
	void Destroy();

	BEGIN_MSG_MAP(C_MAIN)
		MESSAGE_HANDLER(WM_CREATE, OnCreate)
		MESSAGE_HANDLER(WM_CLOSE, OnClose)
		MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
		MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBkgnd)
		MESSAGE_HANDLER(WM_SIZE, OnSize)
		MESSAGE_HANDLER(WM_PAINT, OnPaint)
		COMMAND_ID_HANDLER(IDM_EXIT, OnMenuExit)
		COMMAND_ID_HANDLER(IDM_ABOUT, OnMenuAbout)
		CHAIN_MSG_MAP(CFrameWindowImpl<C_MAIN>)
	END_MSG_MAP()

	LRESULT OnCreate(UINT /*_uMsg*/, WPARAM /*_wParam*/, LPARAM /*_lParam*/, BOOL& _bHandled);
	LRESULT OnClose(UINT /*_uMsg*/, WPARAM /*_wParam*/, LPARAM /*_lParam*/, BOOL& _bHandled);
	LRESULT OnDestroy(UINT /*_uMsg*/, WPARAM /*_wParam*/, LPARAM /*_lParam*/, BOOL& _bHandled);
	// 배경 지우기 차단 - D2D 가 본문 전체를 매 프레임 덮으므로 GDI 지우기는 깜박임만 만든다.
	LRESULT OnEraseBkgnd(UINT /*_uMsg*/, WPARAM /*_wParam*/, LPARAM /*_lParam*/, BOOL& /*_bHandled*/) { return(1); }
	LRESULT OnSize(UINT /*_uMsg*/, WPARAM /*_wParam*/, LPARAM /*_lParam*/, BOOL& _bHandled);
	LRESULT OnPaint(UINT /*_uMsg*/, WPARAM /*_wParam*/, LPARAM /*_lParam*/, BOOL& /*_bHandled*/);
	LRESULT OnMenuExit(WORD /*_wNotifyCode*/, WORD /*_wID*/, HWND /*_hWndCtl*/, BOOL& /*_bHandled*/);
	LRESULT OnMenuAbout(WORD /*_wNotifyCode*/, WORD /*_wID*/, HWND /*_hWndCtl*/, BOOL& /*_bHandled*/);
};
