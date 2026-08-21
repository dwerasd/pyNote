#include "CDocumentListShell.h"

#include "Resource.h"

#include <algorithm>

namespace
{
	constexpr wchar_t DOCUMENT_SHELL_CLASS[] = L"NoteExDocumentListShell";

	bool append(HMENU _hMenu, UINT _nFlags, UINT_PTR _nId, const wchar_t* _pszText)
	{
		return(::AppendMenuW(_hMenu, _nFlags, _nId, _pszText) != FALSE);
	}

	HMENU command_menu(HMENU _hMenu, UINT _nCommand)
	{
		const int nCount = ::GetMenuItemCount(_hMenu);
		for (int nIndex = 0; nIndex < nCount; ++nIndex)
		{
			MENUITEMINFOW Item{};
			Item.cbSize = sizeof(Item);
			Item.fMask = MIIM_ID | MIIM_SUBMENU;
			if (!::GetMenuItemInfoW(_hMenu, static_cast<UINT>(nIndex), TRUE, &Item)) { continue; }
			if (Item.wID == _nCommand) { return(_hMenu); }
			if (Item.hSubMenu)
			{
				if (HMENU hFound = command_menu(Item.hSubMenu, _nCommand)) { return(hFound); }
			}
		}
		return(nullptr);
	}
}

HMENU pynote::shell::CreateRuntimeMenu()
{
	const HMENU hRoot = ::CreateMenu();
	const HMENU hWindow = ::CreatePopupMenu();
	const HMENU hFile = ::CreatePopupMenu();
	const HMENU hEdit = ::CreatePopupMenu();
	const HMENU hView = ::CreatePopupMenu();
	const HMENU hHelp = ::CreatePopupMenu();
	if (!hRoot || !hWindow || !hFile || !hEdit || !hView || !hHelp) { goto failed; }
	if (!append(hWindow, MF_STRING, IDM_NEW_WINDOW, L"새 창\tCtrl+Shift+N") ||
		!append(hFile, MF_GRAYED, IDM_NEW_DOCUMENT, L"새 문서\tCtrl+N") ||
		!append(hFile, MF_STRING, IDM_DOCUMENT_LIST, L"문서 목록…\tCtrl+O") ||
		!append(hFile, MF_SEPARATOR, 0, nullptr) ||
		!append(hFile, MF_GRAYED, IDM_IMPORT_TEXT, L"파일 가져오기…\tCtrl+Shift+I") ||
		!append(hFile, MF_GRAYED, IDM_EXPORT_TEXT, L"TXT/Markdown 내보내기…\tCtrl+Shift+E") ||
		!append(hFile, MF_SEPARATOR, 0, nullptr) ||
		!append(hFile, MF_GRAYED, IDM_CREATE_BACKUP, L"DB 백업 만들기…\tCtrl+Alt+B") ||
		!append(hFile, MF_GRAYED, IDM_RESTORE_BACKUP, L"DB 백업을 파일로 복원…\tCtrl+Alt+R") ||
		!append(hFile, MF_SEPARATOR, 0, nullptr) ||
		!append(hFile, MF_STRING, IDM_EXIT, L"끝내기") ||
		!append(hEdit, MF_STRING, IDM_SAVE_CARD, L"저장\tCtrl+S") ||
		!append(hEdit, MF_STRING, IDM_FIND, L"찾기\tCtrl+F") ||
		!append(hEdit, MF_STRING, IDM_REPLACE, L"바꾸기\tCtrl+H") ||
		!append(hEdit, MF_STRING, IDM_GLOBAL_SEARCH, L"문서와 카드 검색…\tCtrl+P") ||
		!append(hEdit, MF_SEPARATOR, 0, nullptr) ||
		!append(hEdit, MF_GRAYED, IDM_SETTINGS, L"설정…") ||
		!append(hView, MF_STRING, IDM_RESET_GEOMETRY, L"창 위치와 크기 초기화") ||
		!append(hView, MF_SEPARATOR, 0, nullptr) ||
		!append(hView, MF_STRING, IDM_CARD_LIST, L"카드 목록\tCtrl+Shift+P") ||
		!append(hView, MF_STRING, IDM_HISTORY, L"변경 이력\tCtrl+Shift+H") ||
		!append(hView, MF_STRING | MF_UNCHECKED, IDM_FOCUS_MODE, L"집중 모드\tF11") ||
		!append(hHelp, MF_GRAYED, IDM_FIRST_RUN_GUIDE, L"처음 사용 안내") ||
		!append(hHelp, MF_GRAYED, IDM_DATA_LOCATION, L"데이터 위치 표시") ||
		!append(hHelp, MF_GRAYED, IDM_LICENSES, L"오픈소스 라이선스") ||
		!append(hHelp, MF_STRING, IDM_ABOUT, L"NoteEx 정보") ||
		!append(hRoot, MF_POPUP, reinterpret_cast<UINT_PTR>(hWindow), L"창") ||
		!append(hRoot, MF_POPUP, reinterpret_cast<UINT_PTR>(hView), L"보기") ||
		!append(hRoot, MF_POPUP, reinterpret_cast<UINT_PTR>(hFile), L"파일") ||
		!append(hRoot, MF_POPUP, reinterpret_cast<UINT_PTR>(hEdit), L"편집") ||
		!append(hRoot, MF_POPUP, reinterpret_cast<UINT_PTR>(hHelp), L"도움말")) { goto failed; }
	return(hRoot);
failed:
	if (hRoot) { ::DestroyMenu(hRoot); }
	else
	{
		if (hWindow) { ::DestroyMenu(hWindow); }
		if (hFile) { ::DestroyMenu(hFile); }
		if (hEdit) { ::DestroyMenu(hEdit); }
		if (hView) { ::DestroyMenu(hView); }
		if (hHelp) { ::DestroyMenu(hHelp); }
	}
	return(nullptr);
}

std::vector<ACCEL> pynote::shell::RuntimeAccelerators()
{
	return {
		{ FCONTROL | FSHIFT | FVIRTKEY, 'N', IDM_NEW_WINDOW },
		{ FCONTROL | FVIRTKEY, 'O', IDM_DOCUMENT_LIST },
		{ FCONTROL | FVIRTKEY, 'P', IDM_GLOBAL_SEARCH },
		{ FCONTROL | FVIRTKEY, 'S', IDM_SAVE_CARD },
		{ FCONTROL | FVIRTKEY, 'F', IDM_FIND },
		{ FCONTROL | FVIRTKEY, 'H', IDM_REPLACE },
		{ FCONTROL | FSHIFT | FVIRTKEY, 'P', IDM_CARD_LIST },
		{ FCONTROL | FSHIFT | FVIRTKEY, 'H', IDM_HISTORY },
		{ FALT | FVIRTKEY, VK_LEFT, IDM_BACK },
		{ FVIRTKEY, VK_ESCAPE, IDM_BACK },
		{ FVIRTKEY, VK_F11, IDM_FOCUS_MODE },
	};
}

bool pynote::shell::RouteFrameMessage(MSG* _pMessage, HWND _hFrame,
	const std::function<bool(MSG*)>& _PreTranslate, HACCEL _hAccelerator)
{
	if (!_pMessage || !::IsWindow(_hFrame)) { return(false); }
	if (_pMessage->hwnd != _hFrame && !::IsChild(_hFrame, _pMessage->hwnd)) { return(false); }
	if (_PreTranslate && _PreTranslate(_pMessage)) { return(true); }
	return(_hAccelerator && ::TranslateAcceleratorW(_hFrame, _hAccelerator, _pMessage) != FALSE);
}

bool pynote::shell::ApplyFocusMode(HWND _hMain, HMENU _hRuntimeMenu, HWND _hStatus,
	::C_DOCUMENT_LIST_SHELL& _DocumentShell, bool _bEnabled)
{
	if (!::IsWindow(_hMain) || !_hRuntimeMenu || !::IsWindow(_hStatus)) { return(false); }
	if (!::SetMenu(_hMain, _bEnabled ? nullptr : _hRuntimeMenu)) { return(false); }
	::ShowWindow(_hStatus, _bEnabled ? SW_HIDE : SW_SHOW);
	if (_bEnabled) { _DocumentShell.Hide(); }
	else { _DocumentShell.Show(); }
	const HMENU hViewMenu = command_menu(_hRuntimeMenu, IDM_FOCUS_MODE);
	if (!hViewMenu) { return(false); }
	::CheckMenuItem(hViewMenu, IDM_FOCUS_MODE,
		MF_BYCOMMAND | (_bEnabled ? MF_CHECKED : MF_UNCHECKED));
	::DrawMenuBar(_hMain);
	return(true);
}

C_DOCUMENT_LIST_SHELL::~C_DOCUMENT_LIST_SHELL() { this->Destroy(); }

bool C_DOCUMENT_LIST_SHELL::Initialize(HINSTANCE _hInstance, HWND _hOwner)
{
	if (m_hWnd) { return(true); }
	WNDCLASSEXW Class{};
	Class.cbSize = sizeof(Class);
	Class.hInstance = _hInstance;
	Class.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
	Class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
	Class.lpfnWndProc = &C_DOCUMENT_LIST_SHELL::WindowProcedure;
	Class.lpszClassName = DOCUMENT_SHELL_CLASS;
	if (!::RegisterClassExW(&Class) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { return(false); }
	m_hWnd = ::CreateWindowExW(WS_EX_TOOLWINDOW, DOCUMENT_SHELL_CLASS, L"문서 목록",
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 300, 360,
		_hOwner, nullptr, _hInstance, this);
	if (!m_hWnd) { return(false); }
	m_hList = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
		WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
		8, 8, 260, 300, m_hWnd, reinterpret_cast<HMENU>(IDC_DOCUMENT_SHELL_LIST), _hInstance, nullptr);
	if (!m_hList) { this->Destroy(); return(false); }
	::SendMessageW(m_hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"현재 문서"));
	return(true);
}

void C_DOCUMENT_LIST_SHELL::Show()
{
	if (m_hWnd) { ::ShowWindow(m_hWnd, SW_SHOWNOACTIVATE); }
}

void C_DOCUMENT_LIST_SHELL::Hide() { if (m_hWnd) { ::ShowWindow(m_hWnd, SW_HIDE); } }

void C_DOCUMENT_LIST_SHELL::Destroy()
{
	if (m_hWnd) { ::DestroyWindow(m_hWnd); }
	m_hWnd = nullptr;
	m_hList = nullptr;
}

bool C_DOCUMENT_LIST_SHELL::IsVisible() const noexcept
{
	return(m_hWnd && ::IsWindowVisible(m_hWnd));
}

LRESULT CALLBACK C_DOCUMENT_LIST_SHELL::WindowProcedure(
	HWND _hWnd, UINT _uMessage, WPARAM _wParam, LPARAM _lParam)
{
	auto* pShell = reinterpret_cast<C_DOCUMENT_LIST_SHELL*>(::GetWindowLongPtrW(_hWnd, GWLP_USERDATA));
	if (_uMessage == WM_NCCREATE)
	{
		const auto* pCreate = reinterpret_cast<const CREATESTRUCTW*>(_lParam);
		pShell = static_cast<C_DOCUMENT_LIST_SHELL*>(pCreate->lpCreateParams);
		pShell->m_hWnd = _hWnd;
		::SetWindowLongPtrW(_hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pShell));
	}
	return(pShell ? pShell->handle_message_(_uMessage, _wParam, _lParam) :
		::DefWindowProcW(_hWnd, _uMessage, _wParam, _lParam));
}

LRESULT C_DOCUMENT_LIST_SHELL::handle_message_(UINT _uMessage, WPARAM _wParam, LPARAM _lParam)
{
	const HWND hWindow = m_hWnd;
	if (_uMessage == WM_CLOSE) { this->Hide(); return(0); }
	if (_uMessage == WM_SIZE && m_hList)
	{
		RECT Client{};
		::GetClientRect(m_hWnd, &Client);
		::MoveWindow(m_hList, 8, 8, (std::max)(1L, Client.right - 16),
			(std::max)(1L, Client.bottom - 16), TRUE);
		return(0);
	}
	if (_uMessage == WM_NCDESTROY)
	{
		::SetWindowLongPtrW(m_hWnd, GWLP_USERDATA, 0);
		m_hWnd = nullptr;
		m_hList = nullptr;
	}
	return(::DefWindowProcW(hWindow, _uMessage, _wParam, _lParam));
}
