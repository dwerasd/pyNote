#pragma once

#include <windows.h>

#include <functional>
#include <vector>

class C_DOCUMENT_LIST_SHELL;

namespace pynote::shell
{
	HMENU CreateRuntimeMenu();
	std::vector<ACCEL> RuntimeAccelerators();
	bool ApplyFocusMode(HWND _hMain, HMENU _hRuntimeMenu, HWND _hStatus,
		::C_DOCUMENT_LIST_SHELL& _DocumentShell, bool _bEnabled);
	// 활성 메인 창 프레임의 키 라우팅 계약: 프레임 소속 메시지만 대상으로, 페이지
	// 사전 번역이 액셀러레이터보다 먼저다(소유 모델리스 셸은 원본 Qt 의 별도 창
	// 의미대로 창 수준 단축키가 닿지 않는다).
	bool RouteFrameMessage(MSG* _pMessage, HWND _hFrame,
		const std::function<bool(MSG*)>& _PreTranslate, HACCEL _hAccelerator);
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
