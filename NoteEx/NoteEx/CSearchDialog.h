#pragma once

#include <windows.h>

class C_SEARCH_DIALOG final
{
public:
	C_SEARCH_DIALOG() = default;
	~C_SEARCH_DIALOG();
	C_SEARCH_DIALOG(const C_SEARCH_DIALOG&) = delete;
	C_SEARCH_DIALOG& operator=(const C_SEARCH_DIALOG&) = delete;

	bool Initialize(HINSTANCE _hInstance);
	void Show(HWND _hOwner);
	void Hide();
	void Destroy();
	bool IsVisible() const noexcept;
	HWND Hwnd() const noexcept { return m_hWnd; }
	HWND QueryHwnd() const noexcept { return m_hQuery; }

private:
	static LRESULT CALLBACK WindowProcedure(HWND, UINT, WPARAM, LPARAM);
	LRESULT handle_message_(UINT, WPARAM, LPARAM);
	HWND m_hWnd{ nullptr };
	HWND m_hQuery{ nullptr };
};
