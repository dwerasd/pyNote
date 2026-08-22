#pragma once

#include <windows.h>

#include <functional>
#include <optional>
#include <vector>

class C_DOCUMENT_LIST_SHELL;
// 전역 선언만 둔다 - 이 헤더를 읽는 W3/W4 시험 TU 의 include 순서(CreateEvent 매크로
// 계약)를 건드리지 않기 위해 실제 헤더는 .cpp 에서만 읽는다.
class C_DOCUMENT_PAGE;
namespace pynote::platform { class C_WIN32_DEVICE_SETTINGS; }

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

	// ---- 카드 다중 선택(군 D). 원본 MainWindow 의 설정-메뉴-목록 3자 배선에 대응한다. ----
	// 원본 _multi_selection_setting(main_window.py:1328~1335).
	bool ReadMultiSelectionSetting(const pynote::platform::C_WIN32_DEVICE_SETTINGS& _Settings);
	// 원본 multi_selection_action.setChecked(신호 차단) - 표시만 맞춘다.
	bool ApplyMultiSelectionMenuState(HMENU _hRuntimeMenu, bool _bEnabled);
	// 원본 sync_device_settings 의 소비자 절반: 설정값 -> 메뉴 체크 + 목록 선택 모드.
	bool SyncMultiSelection(const pynote::platform::C_WIN32_DEVICE_SETTINGS& _Settings,
		HMENU _hRuntimeMenu, ::C_DOCUMENT_PAGE& _Page);
	// 원본 _set_multi_selection: 값을 뒤집어 저장·sync 한 뒤 이 창에 적용한다.
	std::optional<bool> ToggleMultiSelection(pynote::platform::C_WIN32_DEVICE_SETTINGS& _Settings,
		HMENU _hRuntimeMenu, ::C_DOCUMENT_PAGE& _Page);

	// 원본 _focus_card_list(main_window.py:1176~1188): 편집기가 입력기 자리를 차지하고 있으면
	// "목록으로 돌아가기" 는 곧 편집기 닫기다.
	enum class E_CARD_LIST_COMMAND { RequestLeave, FocusCardList };
	E_CARD_LIST_COMMAND ResolveCardListCommand(bool _bHistoryVisible, bool _bHasSession);
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
