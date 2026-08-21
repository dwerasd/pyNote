#pragma once

#include <windows.h>

#include <vector>

class C_DOCUMENT_LIST_SHELL;

namespace pynote::shell
{
	HMENU CreateRuntimeMenu();
	std::vector<ACCEL> RuntimeAccelerators();
	bool ApplyFocusMode(HWND _hMain, HMENU _hRuntimeMenu, HWND _hStatus,
		::C_DOCUMENT_LIST_SHELL& _DocumentShell, bool _bEnabled);
}

class C_DOCUMENT_LIST_SHELL final
{
public:
	C_DOCUMENT_LIST_SHELL() = default;
	~C_DOCUMENT_LIST_SHELL();
	C_DOCUMENT_LIST_SHELL(const C_DOCUMENT_LIST_SHELL&) = delete;
	C_DOCUMENT_LIST_SHELL& operator=(const C_DOCUMENT_LIST_SHELL&) = delete;

	bool Initialize(HINSTANCE _hInstance, HWND _hOwner);
	void Show();
	void Hide();
	void Destroy();
	bool IsVisible() const noexcept;
	HWND Hwnd() const noexcept { return m_hWnd; }
	HWND ListHwnd() const noexcept { return m_hList; }

private:
	static LRESULT CALLBACK WindowProcedure(HWND, UINT, WPARAM, LPARAM);
	LRESULT handle_message_(UINT, WPARAM, LPARAM);
	HWND m_hWnd{ nullptr };
	HWND m_hList{ nullptr };
};
