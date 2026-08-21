#pragma once

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>
#include <atlframe.h>
#include <atlcrack.h>
#include <atlgdi.h>
#include <atluser.h>
#include <atldlgs.h>

#include <D2DWrapp/D2DDevice.h>
#include <D2DWrapp/D2DSwapTarget.h>
#include <D2DWrapp/D2DBrushCache.h>
#include <D2DWrapp/D2DText.h>
#pragma comment(lib, "D2DWrapp")

#include "Resource.h"
#include "CChangeBus.h"
#include "CDocumentListShell.h"
#include "CDocumentPage.h"
#include "CWindowLayout.h"
#include "pynote/core/application/window_lifecycle.h"

#include <optional>
#include <string>
#include <vector>

class CApplication;
class C_MAIN;

class C_MAIN_PANE_HOST final : public CWindowImpl<C_MAIN_PANE_HOST>
{
public:
	DECLARE_WND_CLASS_EX(L"NoteExPaneHost", CS_HREDRAW | CS_VREDRAW, COLOR_WINDOW)

	void Initialize(C_MAIN* _pOwner, bool _bEditor) noexcept
	{
		m_pOwner = _pOwner;
		m_bEditor = _bEditor;
	}

	BEGIN_MSG_MAP(C_MAIN_PANE_HOST)
		MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBkgnd)
		MESSAGE_HANDLER(WM_PAINT, OnPaint)
	END_MSG_MAP()

	LRESULT OnEraseBkgnd(UINT, WPARAM, LPARAM, BOOL&) { return(1); }
	LRESULT OnPaint(UINT, WPARAM, LPARAM, BOOL&);

private:
	C_MAIN* m_pOwner{ nullptr };
	bool m_bEditor{ false };
};

// 한 인스턴스는 한 HWND와 한 document/workspace identity만 소유한다. 프로세스 자원과
// 생명주기 판정은 CApplication이 소유하고 native callback은 stable token만 전달한다.
class C_MAIN : public CFrameWindowImpl<C_MAIN>
{
private:
	HINSTANCE m_hInst{ nullptr };
	CApplication* m_pApplication{ nullptr };
	pynote::core::application::WINDOW_TOKEN m_Token{ 0 };
	std::string m_sWorkspaceId;
	std::optional<std::string> m_sDocumentId{};
	std::wstring m_sTitle;
	d2d::C_D2D_SWAP_TARGET m_mainTarget;
	pynote::shell::C_WINDOW_SPLITTER m_Splitter;
	C_MAIN_PANE_HOST m_LeftPane;
	C_MAIN_PANE_HOST m_EditorPane;
	C_DOCUMENT_PAGE m_DocumentPage;
	C_DOCUMENT_LIST_SHELL m_DocumentListShell;
	HWND m_hStatus{ nullptr };
	HMENU m_hRuntimeMenu{ nullptr };
	pynote::shell::SUBSCRIPTION_TOKEN m_ChangeSubscription{ 0 };
	// 원본 _publishing_page_content_change(main_window.py:1110~1114) 의 재진입 가드다.
	// 발행을 시작한 창은 자기 페이지를 다시 채우지 않는다 - 제목·상태 바는 무관하게 돈다.
	bool m_bPublishingPageContentChange{ false };
	bool m_bFocusMode{ false };
	bool m_bD2DReady{ false };
	bool m_bCleaned{ false };

	bool save_geometry();
	bool restore_geometry(bool _bAllowLegacyFallback, bool* _pbMaximized);
	void layout_children();
	void render_();
	void subscribe_change_bus_();
	void unsubscribe_change_bus_();
	void on_document_changed_(const std::string& _sDocumentId);
	void on_page_content_changed_();
	void update_title_();
	void update_status_();
	bool refill_after_document_removal_();
	friend class C_MAIN_PANE_HOST;

public:
	DECLARE_FRAME_WND_CLASS(L"NoteExMainWindow", IDC_NOTEEX)

	C_MAIN() = default;
	~C_MAIN() = default;

	bool Init(
		HINSTANCE _hInstance, CApplication* _pApplication,
		pynote::core::application::WINDOW_TOKEN _Token,
		std::string _sWorkspaceId, std::optional<std::string> _sDocumentId,
		std::wstring _sTitle, bool _bAllowLegacyGeometryFallback);
	bool Protect();
	pynote::core::application::E_LEAVE_RESULT RequestLeave();
	bool PersistState();
	bool Cleanup();
	void DestroyNative();
	bool PreTranslateMessage(MSG* _pMessage);
	static std::vector<ACCEL> RuntimeAccelerators();

	C_DOCUMENT_PAGE& DocumentPage() noexcept { return(m_DocumentPage); }
	const C_DOCUMENT_PAGE& DocumentPage() const noexcept { return(m_DocumentPage); }
	HWND StatusHwnd() const noexcept { return(m_hStatus); }
	// 소유 문서는 외부 소멸 뒤 재채움으로 바뀐다 - 소유 판정은 이 살아 있는 값을 본다.
	const std::optional<std::string>& DocumentId() const noexcept { return(m_sDocumentId); }
	HMENU RuntimeMenu() const noexcept { return(m_hRuntimeMenu); }
	bool FocusMode() const noexcept { return(m_bFocusMode); }
	C_DOCUMENT_LIST_SHELL& DocumentListShell() noexcept { return(m_DocumentListShell); }

	BEGIN_MSG_MAP(C_MAIN)
		MESSAGE_HANDLER(WM_CREATE, OnCreate)
		MESSAGE_HANDLER(WM_CLOSE, OnClose)
		MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
		MESSAGE_HANDLER(WM_NCDESTROY, OnNcDestroy)
		MESSAGE_HANDLER(WM_ACTIVATE, OnActivate)
		MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBkgnd)
		MESSAGE_HANDLER(WM_SIZE, OnSize)
		MESSAGE_HANDLER(WM_DPICHANGED, OnDpiChanged)
		MESSAGE_HANDLER(WM_PAINT, OnPaint)
		COMMAND_ID_HANDLER(IDM_RESET_GEOMETRY, OnResetGeometry)
		COMMAND_ID_HANDLER(IDM_NEW_WINDOW, OnNewWindow)
		COMMAND_ID_HANDLER(IDM_DOCUMENT_LIST, OnDocumentList)
		COMMAND_ID_HANDLER(IDM_GLOBAL_SEARCH, OnGlobalSearch)
		COMMAND_ID_HANDLER(IDM_SAVE_CARD, OnSaveCard)
		COMMAND_ID_HANDLER(IDM_FIND, OnFind)
		COMMAND_ID_HANDLER(IDM_REPLACE, OnReplace)
		COMMAND_ID_HANDLER(IDM_CARD_LIST, OnCardList)
		COMMAND_ID_HANDLER(IDM_HISTORY, OnHistory)
		COMMAND_ID_HANDLER(IDM_BACK, OnBack)
		COMMAND_ID_HANDLER(IDM_FOCUS_MODE, OnFocusMode)
		COMMAND_ID_HANDLER(IDM_EXIT, OnMenuExit)
		COMMAND_ID_HANDLER(IDM_ABOUT, OnMenuAbout)
		CHAIN_MSG_MAP(CFrameWindowImpl<C_MAIN>)
	END_MSG_MAP()

	LRESULT OnCreate(UINT, WPARAM, LPARAM, BOOL& _bHandled);
	LRESULT OnClose(UINT, WPARAM, LPARAM, BOOL& _bHandled);
	LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL& _bHandled);
	LRESULT OnNcDestroy(UINT, WPARAM, LPARAM, BOOL& _bHandled);
	LRESULT OnActivate(UINT, WPARAM _wParam, LPARAM, BOOL& _bHandled);
	LRESULT OnEraseBkgnd(UINT, WPARAM, LPARAM, BOOL&) { return(1); }
	LRESULT OnSize(UINT, WPARAM, LPARAM, BOOL& _bHandled);
	LRESULT OnDpiChanged(UINT, WPARAM _wParam, LPARAM _lParam, BOOL& _bHandled);
	LRESULT OnPaint(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnResetGeometry(WORD, WORD, HWND, BOOL&);
	LRESULT OnNewWindow(WORD, WORD, HWND, BOOL&);
	LRESULT OnDocumentList(WORD, WORD, HWND, BOOL&);
	LRESULT OnGlobalSearch(WORD, WORD, HWND, BOOL&);
	LRESULT OnSaveCard(WORD, WORD, HWND, BOOL&);
	LRESULT OnFind(WORD, WORD, HWND, BOOL&);
	LRESULT OnReplace(WORD, WORD, HWND, BOOL&);
	LRESULT OnCardList(WORD, WORD, HWND, BOOL&);
	LRESULT OnHistory(WORD, WORD, HWND, BOOL&);
	LRESULT OnBack(WORD, WORD, HWND, BOOL&);
	LRESULT OnFocusMode(WORD, WORD, HWND, BOOL&);
	LRESULT OnMenuExit(WORD, WORD, HWND, BOOL&);
	LRESULT OnMenuAbout(WORD, WORD, HWND, BOOL&);
};
