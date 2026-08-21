#include "CSearchDialog.h"

#include "Resource.h"

#include <algorithm>

namespace
{
	constexpr wchar_t SEARCH_CLASS[] = L"NoteExSearchDialog";
}

C_SEARCH_DIALOG::~C_SEARCH_DIALOG() { this->Destroy(); }

bool C_SEARCH_DIALOG::Initialize(HINSTANCE _hInstance)
{
	if (m_hWnd) { return(true); }
	WNDCLASSEXW Class{};
	Class.cbSize = sizeof(Class);
	Class.hInstance = _hInstance;
	Class.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
	Class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
	Class.lpfnWndProc = &C_SEARCH_DIALOG::WindowProcedure;
	Class.lpszClassName = SEARCH_CLASS;
	if (!::RegisterClassExW(&Class) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { return(false); }
	m_hWnd = ::CreateWindowExW(WS_EX_TOOLWINDOW, SEARCH_CLASS, L"문서와 카드 검색",
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 420, 120,
		nullptr, nullptr, _hInstance, this);
	if (!m_hWnd) { return(false); }
	m_hQuery = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
		12, 14, 378, 26, m_hWnd, reinterpret_cast<HMENU>(IDC_SEARCH_QUERY), _hInstance, nullptr);
	if (!m_hQuery) { this->Destroy(); return(false); }
	return(true);
}

void C_SEARCH_DIALOG::Show(HWND _hOwner)
{
	if (!m_hWnd) { return; }
	::SetWindowLongPtrW(m_hWnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(_hOwner));
	::ShowWindow(m_hWnd, SW_SHOWNORMAL);
	::SetForegroundWindow(m_hWnd);
	::SetFocus(m_hQuery);
	::SendMessageW(m_hQuery, EM_SETSEL, 0, -1);
}

void C_SEARCH_DIALOG::Hide() { if (m_hWnd) { ::ShowWindow(m_hWnd, SW_HIDE); } }

void C_SEARCH_DIALOG::Destroy()
{
	if (m_hWnd) { ::DestroyWindow(m_hWnd); }
	m_hWnd = nullptr;
	m_hQuery = nullptr;
}

bool C_SEARCH_DIALOG::IsVisible() const noexcept
{
	return(m_hWnd && ::IsWindowVisible(m_hWnd));
}

LRESULT CALLBACK C_SEARCH_DIALOG::WindowProcedure(HWND _hWnd, UINT _uMessage, WPARAM _wParam, LPARAM _lParam)
{
	auto* pDialog = reinterpret_cast<C_SEARCH_DIALOG*>(::GetWindowLongPtrW(_hWnd, GWLP_USERDATA));
	if (_uMessage == WM_NCCREATE)
	{
		const auto* pCreate = reinterpret_cast<const CREATESTRUCTW*>(_lParam);
		pDialog = static_cast<C_SEARCH_DIALOG*>(pCreate->lpCreateParams);
		pDialog->m_hWnd = _hWnd;
		::SetWindowLongPtrW(_hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pDialog));
	}
	return(pDialog ? pDialog->handle_message_(_uMessage, _wParam, _lParam) :
		::DefWindowProcW(_hWnd, _uMessage, _wParam, _lParam));
}

LRESULT C_SEARCH_DIALOG::handle_message_(UINT _uMessage, WPARAM _wParam, LPARAM _lParam)
{
	const HWND hWindow = m_hWnd;
	if (_uMessage == WM_CLOSE) { this->Hide(); return(0); }
	if (_uMessage == WM_SIZE && m_hQuery)
	{
		RECT Client{};
		::GetClientRect(m_hWnd, &Client);
		::MoveWindow(m_hQuery, 12, 14, (std::max)(1L, Client.right - 24), 26, TRUE);
		return(0);
	}
	if (_uMessage == WM_NCDESTROY)
	{
		::SetWindowLongPtrW(m_hWnd, GWLP_USERDATA, 0);
		m_hWnd = nullptr;
		m_hQuery = nullptr;
	}
	return(::DefWindowProcW(hWindow, _uMessage, _wParam, _lParam));
}
