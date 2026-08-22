#include <catch_amalgamated.hpp>

#include "CCardList.h"
#include "CDocumentListShell.h"
#include "CDocumentPage.h"
#include "Resource.h"
#include "pynote/harness/win32_harness.h"

// windows.h 의 CreateEvent 매크로가 repositories.h 의 멤버 이름을 바꾸기 전에 걷는다 -
// CDocumentPage.cpp·w4_card_list_test.cpp 와 같은 순서 계약이어야 같은 바이너리 안에서
// 멤버 이름이 갈리지 않는다. ATL/WTL(CCardList.h)은 이 #undef 앞에서 읽어야 자기
// ::CreateEvent 호출이 식별자를 잃지 않는다.
#ifdef CreateEvent
#undef CreateEvent
#endif

#include "pynote/core/application/card_service.h"
#include "pynote/core/application/draft_coordinator.h"
#include "pynote/core/application/save_coordinator.h"
#include "pynote/core/application/workspace_state.h"
#include "pynote/core/domain/card_list_projection.h"
#include "pynote/core/domain/paragraph_parser.h"
#include "pynote/core/storage/migration_runner.h"
#include "pynote/core/storage/repositories.h"
#include "pynote/platform/win32_device_settings.h"
#include "pynote/platform/win32_file_system.h"

#include <D2DWrapp/D2DBrushCache.h>
#include <D2DWrapp/D2DDevice.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "D2DWrapp")
#pragma comment(lib, "NoteExCore")

namespace
{
	namespace app = pynote::core::application;
	namespace domain = pynote::core::domain;
	namespace shell = pynote::shell;
	namespace storage = pynote::core::storage;
	using pynote::harness::TestWindow;
	using pynote::platform::C_WIN32_DEVICE_SETTINGS;
	using pynote::platform::C_WIN32_FILE_SYSTEM;

	// 시험은 폰트를 명시 주입해 측정을 결정적으로 만든다(시스템 메시지 폰트 의존 제거).
	constexpr wchar_t TEST_FONT_FAMILY[] = L"Segoe UI";
	constexpr float TEST_FONT_SIZE_DIP = 12.0f;
	// 오라클 [ENV] 의 QApplication.startDragDistance() 값이다.
	constexpr int DRAG_THRESHOLD_DIP = 10;
	constexpr char MULTI_SELECTION_KEY[] = "cards/multi_selection_enabled";

	// 한글은 narrow 리터럴로 쓰면 실행 문자셋(CP949)으로 접혀 UTF-8 계약이 깨진다 -
	// 본문은 전부 wide 리터럴에서 변환한다.
	std::string to_utf8(const std::wstring& _sValue)
	{
		if (_sValue.empty()) { return(std::string{}); }
		const int nRequired = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
			_sValue.data(), static_cast<int>(_sValue.size()), nullptr, 0, nullptr, nullptr);
		if (nRequired <= 0) { throw std::runtime_error("WideCharToMultiByte size query failed"); }
		std::string Result(static_cast<std::size_t>(nRequired), '\0');
		if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, _sValue.data(),
			static_cast<int>(_sValue.size()), Result.data(), nRequired, nullptr, nullptr) != nRequired)
		{
			throw std::runtime_error("WideCharToMultiByte conversion failed");
		}
		return(Result);
	}

	// w4_card_list_test.cpp 의 같은 이름 헬퍼와 같은 카드다(그 TU 는 무수정 계약이라 복제한다).
	domain::S_CARD make_card(int _nNumber, const std::string& _sBody)
	{
		domain::S_CARD Card;
		Card.sId = "card-" + std::to_string(_nNumber);
		Card.sDocumentId = "document-1";
		Card.sOperationId = "operation-" + std::to_string(_nNumber);
		Card.nPositionKey = static_cast<std::int64_t>(_nNumber) * 1024;
		Card.nCaptureSeq = _nNumber;
		Card.nCreatedAtUs = 1000000 + _nNumber;
		Card.nUpdatedAtUs = 1000000 + _nNumber;
		Card.eSource = domain::E_CARD_SOURCE::Typing;
		Card.sBody = _sBody;
		Card.sCurrentRevisionId = "revision-" + std::to_string(_nNumber);
		return(Card);
	}

	// 정렬이 문서순이므로 행 r 의 카드 id 는 card-(r+1) 이다.
	std::vector<std::string> cards_at(std::initializer_list<int> _Rows)
	{
		std::vector<std::string> Ids;
		for (const int nRow : _Rows) { Ids.push_back("card-" + std::to_string(nRow + 1)); }
		return(Ids);
	}

	// w3_shell_consumer_test.cpp:204 의 메뉴 상태 조회와 같은 계약이다.
	UINT command_state(HMENU _hMenu, UINT _nCommand)
	{
		const int nCount = ::GetMenuItemCount(_hMenu);
		for (int nIndex = 0; nIndex < nCount; ++nIndex)
		{
			MENUITEMINFOW Item{};
			Item.cbSize = sizeof(Item);
			Item.fMask = MIIM_ID | MIIM_STATE | MIIM_SUBMENU;
			if (!::GetMenuItemInfoW(_hMenu, static_cast<UINT>(nIndex), TRUE, &Item)) { continue; }
			if (Item.wID == _nCommand) { return(Item.fState); }
			if (Item.hSubMenu)
			{
				const UINT nState = command_state(Item.hSubMenu, _nCommand);
				if (nState != static_cast<UINT>(-1)) { return(nState); }
			}
		}
		return(static_cast<UINT>(-1));
	}

	class C_MENU_SCOPE
	{
	public:
		C_MENU_SCOPE() : m_hMenu(shell::CreateRuntimeMenu()) { REQUIRE(m_hMenu != nullptr); }
		~C_MENU_SCOPE() { if (m_hMenu) { ::DestroyMenu(m_hMenu); } }
		C_MENU_SCOPE(const C_MENU_SCOPE&) = delete;
		C_MENU_SCOPE& operator=(const C_MENU_SCOPE&) = delete;
		HMENU Get() const noexcept { return(m_hMenu); }

	private:
		HMENU m_hMenu{};
	};

	// 키 메시지의 수식키는 호출 스레드의 키 상태 표로 건다 - GetKeyState 가 읽는 바로 그 표이며
	// 전경 권한도, 사용자 데스크톱 부작용도 없다(keybd_event/SendInput 을 쓰지 않는 이유다).
	class C_MODIFIER_SCOPE
	{
	public:
		explicit C_MODIFIER_SCOPE(std::initializer_list<int> _Keys)
		{
			REQUIRE(::GetKeyboardState(m_Saved) != FALSE);
			BYTE State[256]{};
			std::memcpy(State, m_Saved, sizeof(State));
			for (const int nKey : _Keys) { State[nKey] = 0x80; }
			REQUIRE(::SetKeyboardState(State) != FALSE);
		}
		~C_MODIFIER_SCOPE() { ::SetKeyboardState(m_Saved); }
		C_MODIFIER_SCOPE(const C_MODIFIER_SCOPE&) = delete;
		C_MODIFIER_SCOPE& operator=(const C_MODIFIER_SCOPE&) = delete;

	private:
		BYTE m_Saved[256]{};
	};

	// 음수 클라이언트 좌표까지 실어야 하므로 부호 있는 WORD 로 담는다.
	LPARAM pack_point(POINT _Point)
	{
		return(MAKELPARAM(static_cast<WORD>(static_cast<short>(_Point.x)),
			static_cast<WORD>(static_cast<short>(_Point.y))));
	}

	void send_press(HWND _hWnd, POINT _Point, WPARAM _nModifiers = 0)
	{
		::SendMessageW(_hWnd, WM_LBUTTONDOWN, MK_LBUTTON | _nModifiers, pack_point(_Point));
	}

	void send_move(HWND _hWnd, POINT _Point, WPARAM _nModifiers = 0)
	{
		::SendMessageW(_hWnd, WM_MOUSEMOVE, MK_LBUTTON | _nModifiers, pack_point(_Point));
	}

	void send_release(HWND _hWnd, POINT _Point, WPARAM _nModifiers = 0)
	{
		::SendMessageW(_hWnd, WM_LBUTTONUP, _nModifiers, pack_point(_Point));
	}

	void send_click(HWND _hWnd, POINT _Point, WPARAM _nModifiers = 0)
	{
		send_press(_hWnd, _Point, _nModifiers);
		send_release(_hWnd, _Point, _nModifiers);
	}

	void send_double_click(HWND _hWnd, POINT _Point)
	{
		::SendMessageW(_hWnd, WM_LBUTTONDBLCLK, MK_LBUTTON, pack_point(_Point));
	}

	void send_right_click(HWND _hWnd, POINT _Point)
	{
		::SendMessageW(_hWnd, WM_RBUTTONDOWN, MK_RBUTTON, pack_point(_Point));
		::SendMessageW(_hWnd, WM_RBUTTONUP, 0, pack_point(_Point));
	}

	void send_key(HWND _hWnd, int _nKey, std::initializer_list<int> _Modifiers = {},
		LPARAM _lParam = 1)
	{
		C_MODIFIER_SCOPE Scope(_Modifiers);
		::SendMessageW(_hWnd, WM_KEYDOWN, static_cast<WPARAM>(_nKey), _lParam);
	}

	void send_char(HWND _hWnd, wchar_t _Char, std::initializer_list<int> _Modifiers = {})
	{
		C_MODIFIER_SCOPE Scope(_Modifiers);
		::SendMessageW(_hWnd, WM_CHAR, static_cast<WPARAM>(_Char), 1);
	}

	int manhattan(POINT _First, POINT _Second)
	{
		return(std::abs(_First.x - _Second.x) + std::abs(_First.y - _Second.y));
	}

	// 진짜 HWND + 진짜 D2DWrapp + 진짜 프로젝션 위의 컨트롤(S1 C_RENDER_FIXTURE 모양).
	// 뷰 크기는 오라클 [ENV] 의 500x500 이며 전 행이 한 화면에 들어간다(N1).
	class C_SELECT_FIXTURE
	{
	public:
		static constexpr int VIEW_DIP = 500;

		explicit C_SELECT_FIXTURE(bool _bExtended = false,
			const std::vector<std::wstring>& _Bodies = {})
			: m_Host(pynote::harness::TestWindowOptions{ L"W4 select", 700, 700, true })
		{
			REQUIRE(m_Device.Initialize());
			REQUIRE(m_Text.Initialize(&m_Device));
			REQUIRE(m_Brushes.Initialize(&m_Device));
			m_Control.AttachRenderServices(&m_Device, &m_Brushes, &m_Text);
			m_Control.Bind(m_Projection);
			S_CARD_LIST_DISPLAY Display;
			Display.Font.sFamily = TEST_FONT_FAMILY;
			Display.Font.fSizeDip = TEST_FONT_SIZE_DIP;
			m_Control.SetDisplaySettings(Display);
			m_Control.SetOpenCardHandler(
				[this](const std::string& _sCardId) { m_Opened.push_back(_sCardId); });
			m_Control.SetEmptyAreaClickHandler([this]() { ++m_nClicked; });
			m_Control.SetDeleteHandler(
				[this](std::vector<std::string> _Ids) { m_Deleted.push_back(std::move(_Ids)); });
			m_Control.SetActivateHandler([this]() { ++m_nActivated; });
			RECT Frame{ 0, 0, VIEW_DIP, VIEW_DIP };
			REQUIRE(m_Control.Create(m_Host.hwnd(), Frame, nullptr,
				WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL, 0,
				static_cast<UINT>(IDC_DOCUMENT_CARD_LIST)) != nullptr);
			this->ResizeClient(VIEW_DIP, VIEW_DIP);
			// DPI 비인식 프로세스라 픽셀 == DIP 다 - 좌표 계약을 한 번 못박는다.
			REQUIRE(static_cast<int>(::GetDpiForWindow(m_Control.m_hWnd)) ==
				static_cast<int>(USER_DEFAULT_SCREEN_DPI));

			const std::size_t nCount = _Bodies.empty() ? 5 : _Bodies.size();
			std::vector<domain::S_CARD> Cards;
			for (std::size_t nIndex = 0; nIndex < nCount; ++nIndex)
			{
				const std::wstring sBody = _Bodies.empty() ?
					(L"카드 " + std::to_wstring(nIndex + 1)) : _Bodies[nIndex];
				Cards.push_back(make_card(static_cast<int>(nIndex) + 1, to_utf8(sBody)));
			}
			m_Projection.SetSortMode(domain::E_CARD_LIST_SORT_MODE::Position);
			m_Projection.SetMultiSelectionEnabled(_bExtended);
			m_Projection.SetCards(Cards);
			m_Control.OnProjectionChanged();
			// 오라클 N1: 전 행이 한 화면에 들어가야 empty_pt 와 PageUp/Down 값이 성립한다.
			REQUIRE(m_Control.RowHeightDip() * static_cast<int>(nCount) <
				m_Control.ViewportHeightDip());
			this->FocusIn();
			REQUIRE_FALSE(m_Control.HitTestRow(this->EmptyPoint()).has_value());
		}

		~C_SELECT_FIXTURE()
		{
			if (::GetCapture() == m_Control.m_hWnd) { ::ReleaseCapture(); }
			if (m_Control.IsWindow()) { m_Control.DestroyWindow(); }
		}

		C_SELECT_FIXTURE(const C_SELECT_FIXTURE&) = delete;
		C_SELECT_FIXTURE& operator=(const C_SELECT_FIXTURE&) = delete;

		C_CARD_LIST& Control() noexcept { return(m_Control); }
		const C_CARD_LIST& Control() const noexcept { return(m_Control); }
		HWND Hwnd() const noexcept { return(m_Control.m_hWnd); }

		// 오라클의 "fresh view" 자리다 - 선택·현재·누적기를 되돌리고 포커스를 다시 넣는다.
		void Reset()
		{
			::SendMessageW(m_Control.m_hWnd, WM_CAPTURECHANGED, 0, 0);
			if (::GetCapture() == m_Control.m_hWnd) { ::ReleaseCapture(); }
			m_Projection.SetSelectedCardIds({});
			m_Projection.SetCurrentCardId(std::nullopt);
			// Bind 가 앵커·Shift 기준·"현재를 본 적 있는가" 를 되돌린다.
			m_Control.Bind(m_Projection);
			m_Opened.clear();
			m_Deleted.clear();
			m_nClicked = 0;
			m_nActivated = 0;
			this->FocusIn();
		}

		const std::vector<std::string>& Selected() const { return(m_Projection.SelectedCardIds()); }

		int CurrentRow() const
		{
			const std::optional<std::string>& sCurrent = m_Projection.CurrentCardId();
			if (!sCurrent) { return(-1); }
			const auto nRow = m_Projection.RowForCard(*sCurrent);
			return(nRow ? static_cast<int>(*nRow) : -1);
		}

		const std::vector<std::string>& Opened() const noexcept { return(m_Opened); }
		const std::vector<std::vector<std::string>>& Deleted() const noexcept { return(m_Deleted); }
		int Clicked() const noexcept { return(m_nClicked); }
		int Activated() const noexcept { return(m_nActivated); }
		E_CARD_LIST_VIEW_STATE State() const noexcept { return(m_Control.ViewState()); }

		POINT Pt(std::size_t _nRow) const
		{
			const S_DIP_RECT Row = m_Control.RowRectDip(_nRow);
			return(POINT{ Row.nLeft + Row.nWidth / 2, Row.nTop + Row.nHeight / 2 });
		}

		POINT Below(std::size_t _nRow, int _nDeltaY) const
		{
			const S_DIP_RECT Row = m_Control.RowRectDip(_nRow);
			return(POINT{ Row.nLeft + Row.nWidth / 2, Row.Bottom() + _nDeltaY });
		}

		POINT EmptyPoint() const
		{
			const S_DIP_RECT Last = m_Control.RowRectDip(m_Projection.RowCount() - 1);
			return(POINT{ m_Control.ViewportWidthDip() / 2,
				(Last.Bottom() + m_Control.ViewportHeightDip()) / 2 });
		}

		POINT Offset(POINT _Point, int _nDeltaX, int _nDeltaY) const
		{
			return(POINT{ _Point.x + _nDeltaX, _Point.y + _nDeltaY });
		}

		int HitRow(POINT _Point) const
		{
			const auto nRow = m_Control.HitTestRow(_Point);
			return(nRow ? static_cast<int>(*nRow) : -1);
		}

	private:
		void FocusIn()
		{
			::SetFocus(m_Host.hwnd());
			pynote::harness::drain_messages();
			::SetFocus(m_Control.m_hWnd);
			pynote::harness::drain_messages();
			// 오라클 N3: 포커스가 들어오면 선택 없이 현재 행만 0 이 된다.
			REQUIRE(this->CurrentRow() == 0);
			REQUIRE(this->Selected().empty());
		}

		// 컨트롤이 WS_VSCROLL 을 달고 있어 창 폭과 클라이언트 폭이 다르다 - 뷰포트 기준으로 맞춘다.
		void ResizeClient(int _nWidthDip, int _nHeightDip)
		{
			int nWidth = _nWidthDip;
			int nHeight = _nHeightDip;
			for (int nAttempt = 0; nAttempt < 4; ++nAttempt)
			{
				::MoveWindow(m_Control.m_hWnd, 0, 0, nWidth, nHeight, TRUE);
				pynote::harness::drain_messages();
				const int nActualWidth = m_Control.ViewportWidthDip();
				const int nActualHeight = m_Control.ViewportHeightDip();
				if (nActualWidth == _nWidthDip && nActualHeight == _nHeightDip) { break; }
				nWidth += _nWidthDip - nActualWidth;
				nHeight += _nHeightDip - nActualHeight;
			}
			REQUIRE(m_Control.ViewportWidthDip() == _nWidthDip);
			REQUIRE(m_Control.ViewportHeightDip() == _nHeightDip);
		}

		d2d::C_D2D_DEVICE m_Device;
		d2d::C_D2D_TEXT m_Text;
		d2d::C_D2D_BRUSH_CACHE m_Brushes;
		TestWindow m_Host;
		domain::C_CARD_LIST_PROJECTION m_Projection;
		C_CARD_LIST m_Control;
		std::vector<std::string> m_Opened;
		std::vector<std::vector<std::string>> m_Deleted;
		int m_nClicked{ 0 };
		int m_nActivated{ 0 };
	};

	// 한 걸음 뒤 상태를 오라클 표의 한 행과 대조한다.
	void require_step(const C_SELECT_FIXTURE& _Fixture, const char* _pszStep,
		const std::vector<std::string>& _Selected, int _nCurrentRow,
		const std::vector<std::string>& _Opened, int _nClicked, E_CARD_LIST_VIEW_STATE _eState)
	{
		INFO("step " << _pszStep);
		REQUIRE(_Fixture.Selected() == _Selected);
		REQUIRE(_Fixture.CurrentRow() == _nCurrentRow);
		REQUIRE(_Fixture.Opened() == _Opened);
		REQUIRE(_Fixture.Clicked() == _nClicked);
		REQUIRE(_Fixture.State() == _eState);
	}

	// 임시 LOCALAPPDATA + 임시 레지스트리 루트 격리(unit/win32_device_settings_test.cpp 와 같은 형태).
	std::atomic<unsigned long> g_nSequence{ 0 };

	std::wstring unique_suffix()
	{
		return(L"w4s2_" + std::to_wstring(::GetCurrentProcessId()) + L"_" +
			std::to_wstring(::GetTickCount64()) + L"_" + std::to_wstring(++g_nSequence));
	}

	class C_TEMP_LOCAL_APP_DATA
	{
	public:
		C_TEMP_LOCAL_APP_DATA()
		{
			wchar_t TempPath[32768] = {};
			const DWORD nLength = ::GetTempPathW(static_cast<DWORD>(std::size(TempPath)), TempPath);
			m_Root = std::filesystem::path(std::wstring(TempPath, nLength)) /
				(L"NoteEx-W4-S2-settings-" + unique_suffix());
			std::filesystem::create_directories(m_Root);

			const DWORD nOldRequired = ::GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
			if (nOldRequired != 0)
			{
				m_sOld.resize(nOldRequired);
				const DWORD nOldLength =
					::GetEnvironmentVariableW(L"LOCALAPPDATA", m_sOld.data(), nOldRequired);
				m_sOld.resize(nOldLength);
				m_bHadOld = true;
			}
			::SetEnvironmentVariableW(L"LOCALAPPDATA", m_Root.c_str());
		}

		~C_TEMP_LOCAL_APP_DATA()
		{
			::SetEnvironmentVariableW(L"LOCALAPPDATA", m_bHadOld ? m_sOld.c_str() : nullptr);
			std::error_code Error;
			std::filesystem::remove_all(m_Root, Error);
		}

		C_TEMP_LOCAL_APP_DATA(const C_TEMP_LOCAL_APP_DATA&) = delete;
		C_TEMP_LOCAL_APP_DATA& operator=(const C_TEMP_LOCAL_APP_DATA&) = delete;

	private:
		std::filesystem::path m_Root;
		std::wstring m_sOld;
		bool m_bHadOld{ false };
	};

	class C_REGISTRY_SCOPE
	{
	public:
		C_REGISTRY_SCOPE() : m_sRoot(L"Software\\pyNote\\W4S2Tests\\" + unique_suffix()) {}
		~C_REGISTRY_SCOPE()
		{
			::RegDeleteTreeW(HKEY_CURRENT_USER, m_sRoot.c_str());
			::RegDeleteKeyW(HKEY_CURRENT_USER, m_sRoot.c_str());
		}
		C_REGISTRY_SCOPE(const C_REGISTRY_SCOPE&) = delete;
		C_REGISTRY_SCOPE& operator=(const C_REGISTRY_SCOPE&) = delete;
		const std::wstring& Root() const noexcept { return(m_sRoot); }

	private:
		std::wstring m_sRoot;
	};

	// 진짜 DB·마이그레이션·서비스 위의 페이지(S1 C_PAGE_FIXTURE 모양). 좌 pane 은 여러 행이
	// 한 화면에 들어가도록 높게 잡는다 - 마우스로 행을 짚어야 하기 때문이다.
	class C_PAGE_FIXTURE
	{
	public:
		C_PAGE_FIXTURE()
			: m_Path(std::filesystem::temp_directory_path() /
				("NoteEx-W4S2-page-" + std::to_string(::GetCurrentProcessId()) + "-" +
				std::to_string(++s_nSequence) + ".db")), m_Repositories(m_Database),
			  m_DraftStore(m_Database, m_Repositories),
			  m_Parent(pynote::harness::TestWindowOptions{ L"W4 S2 page host", 1000, 780, true })
		{
			this->remove_();
			REQUIRE(m_Database.Open(m_Path.string()));
			storage::C_MIGRATION_RUNNER Runner;
			Runner.SetExistingDatabase(false, m_Path.string());
			REQUIRE(Runner.Run(m_Database) == storage::E_MIGRATE_RESULT::Ok);
			domain::S_DOCUMENT Document;
			Document.sId = DocumentId;
			Document.sTitle = "w4 s2 document";
			Document.nCreatedAtUs = 1000;
			Document.nUpdatedAtUs = 1000;
			REQUIRE(m_Repositories.CreateDocument(Document) == storage::E_REPO_RESULT::Ok);
			domain::S_WORKSPACE_WINDOW Workspace;
			REQUIRE(m_Repositories.SaveWorkspaceWindow(
				WorkspaceId, { DocumentId }, DocumentId, &Workspace) == storage::E_REPO_RESULT::Ok);
			m_CardService = std::make_unique<app::C_CARD_SERVICE>(m_Database, m_Repositories, m_Parser,
				[this]() { return(++m_nClock); }, [this]() { return(this->next_id_("card-data")); });
			m_Drafts = std::make_unique<app::C_DRAFT_COORDINATOR>(m_DraftStore, 2000,
				[this]() { return(++m_nClock); }, [this]() { return(++m_nClock); },
				[this]() { return(++m_nClock * 1000); }, [this]() { return(this->next_id_("draft")); });
			m_Save = std::make_unique<app::C_SAVE_COORDINATOR>(m_Database, m_Repositories, *m_Drafts,
				[this]() { return(++m_nClock); }, [this]() { return(this->next_id_("save")); });
			REQUIRE(m_Device.Initialize());
			REQUIRE(m_Text.Initialize(&m_Device));
			REQUIRE(m_Brushes.Initialize(&m_Device));
			m_hLeft = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
				0, 0, 340, LIST_HOST_HEIGHT, m_Parent.hwnd(), reinterpret_cast<HMENU>(3001),
				::GetModuleHandleW(nullptr), nullptr);
			m_hRight = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
				340, 0, 640, LIST_HOST_HEIGHT, m_Parent.hwnd(), reinterpret_cast<HMENU>(3002),
				::GetModuleHandleW(nullptr), nullptr);
			REQUIRE(m_hLeft != nullptr);
			REQUIRE(m_hRight != nullptr);
			this->create_page_();
		}

		~C_PAGE_FIXTURE()
		{
			m_Page.reset();
			m_Save.reset();
			m_Drafts.reset();
			m_CardService.reset();
			m_Database.Close();
			this->remove_();
		}

		C_PAGE_FIXTURE(const C_PAGE_FIXTURE&) = delete;
		C_PAGE_FIXTURE& operator=(const C_PAGE_FIXTURE&) = delete;

		// 클립보드는 프로세스 밖(클립보드 관리자·OS 클립보드 기록)이 잠깐 잡을 수 있는 공유
		// 자원이다 - OpenClipboard 는 그 사이 실패하고, 열리더라도 붙여넣기 직전에 내용이
		// 바뀌면 편집면에 글자가 실리지 않는다(실측: OpenClipboard=0 과 무동작 붙여넣기 각 1회).
		// MSDN 권고대로 유계 재시도하고, "글자가 실제로 늘었는가" 까지 확인한 뒤에야 돌아간다.
		// 단언을 무르게 한 것이 아니라 입력 전달을 사실로 만든 것이다 - 제품 계약 단언은
		// 이 함수 밖에서 종전 그대로 한 번만 돈다.
		void Paste(const std::wstring& _sText)
		{
			const std::wstring sBefore = this->EditorText();
			for (int nAttempt = 0; nAttempt < 20; ++nAttempt)
			{
				if (this->try_paste_(_sText) && this->EditorText() != sBefore) { return; }
				::Sleep(20);
			}
			FAIL("클립보드 붙여넣기가 20회 시도 안에 편집면에 글자를 싣지 못했습니다");
		}

		// 붙여넣기는 한 세션에 카드 한 장만 만든다 - 여러 장이 필요한 시퀀스는 서비스로 직접 만든다.
		void CreateCards(int _nCount)
		{
			for (int nIndex = 1; nIndex <= _nCount; ++nIndex)
			{
				domain::S_CARD Created;
				REQUIRE(m_CardService->CreateCard(DocumentId,
					to_utf8(L"카드 " + std::to_wstring(nIndex)),
					domain::E_CAPTURE_OPERATION_SOURCE::Typing, std::nullopt, &Created) ==
					app::E_CARD_SERVICE_RESULT::Ok);
			}
			REQUIRE(m_Page->Refresh());
		}

		bool CardDeleted(const std::string& _sCardId)
		{
			domain::S_CARD Card;
			const auto eResult = m_Repositories.GetCard(_sCardId, &Card);
			if (eResult == storage::E_REPO_RESULT::NotFound) { return(true); }
			REQUIRE(eResult == storage::E_REPO_RESULT::Ok);
			return(Card.nDeletedAtUs.has_value() && *Card.nDeletedAtUs != 0);
		}

		std::wstring EditorText() const
		{
			std::wstring Text(
				static_cast<std::size_t>(::GetWindowTextLengthW(m_Page->EditorHwnd())) + 1, L'\0');
			Text.resize(static_cast<std::size_t>(::GetWindowTextW(
				m_Page->EditorHwnd(), Text.data(), static_cast<int>(Text.size()))));
			return(Text);
		}

		POINT RowPoint(std::size_t _nRow) const
		{
			const S_DIP_RECT Row = m_Page->CardList().RowRectDip(_nRow);
			return(POINT{ Row.nLeft + Row.nWidth / 2, Row.nTop + Row.nHeight / 2 });
		}

		POINT EmptyPoint() const
		{
			const C_CARD_LIST& List = m_Page->CardList();
			const S_DIP_RECT Last = List.RowRectDip(List.RowCount() - 1);
			return(POINT{ List.ViewportWidthDip() / 2,
				(Last.Bottom() + List.ViewportHeightDip()) / 2 });
		}

		C_DOCUMENT_PAGE& Page() { return(*m_Page); }

		static constexpr int LIST_HOST_HEIGHT = 700;
		C_DOCUMENT_PAGE::E_LEAVE_CHOICE LeaveChoice{ C_DOCUMENT_PAGE::E_LEAVE_CHOICE::Save };
		inline static const std::string DocumentId = "document-w4s2";
		inline static const std::string WorkspaceId = "window-w4s2";

	private:
		bool try_paste_(const std::wstring& _sText)
		{
			if (!::OpenClipboard(m_Parent.hwnd())) { return(false); }
			bool bOk = ::EmptyClipboard() != FALSE;
			if (bOk)
			{
				const SIZE_T nBytes = (_sText.size() + 1) * sizeof(wchar_t);
				HGLOBAL hText = ::GlobalAlloc(GMEM_MOVEABLE, nBytes);
				bOk = hText != nullptr;
				if (bOk)
				{
					void* pText = ::GlobalLock(hText);
					bOk = pText != nullptr;
					if (bOk)
					{
						std::memcpy(pText, _sText.c_str(), nBytes);
						::GlobalUnlock(hText);
						bOk = ::SetClipboardData(CF_UNICODETEXT, hText) == hText;
					}
					// 클립보드가 소유권을 가져가지 못했으면 우리가 되돌린다.
					if (!bOk) { ::GlobalFree(hText); }
				}
			}
			if (!::CloseClipboard()) { return(false); }
			if (!bOk) { return(false); }
			::SetFocus(m_Page->EditorHwnd());
			::SendMessageW(m_Page->EditorHwnd(), WM_PASTE, 0, 0);
			return(true);
		}

		void create_page_()
		{
			m_Page = std::make_unique<C_DOCUMENT_PAGE>();
			m_Page->SetRenderServices(&m_Device, &m_Brushes, &m_Text);
			S_CARD_LIST_DISPLAY Display;
			Display.Font.sFamily = TEST_FONT_FAMILY;
			Display.Font.fSizeDip = TEST_FONT_SIZE_DIP;
			m_Page->SetDisplaySettings(Display);
			REQUIRE(m_Page->Init(::GetModuleHandleW(nullptr), m_hLeft, m_hRight,
				m_Database, m_Repositories, *m_CardService, *m_Drafts, *m_Save,
				WorkspaceId, DocumentId, [this](HWND) { return(LeaveChoice); }));
		}

		std::string next_id_(const char* _pszPrefix)
		{
			return(std::string(_pszPrefix) + "-" + std::to_string(++m_nId));
		}

		void remove_() const
		{
			std::error_code Error;
			std::filesystem::remove(m_Path, Error);
			std::filesystem::remove(m_Path.string() + "-wal", Error);
			std::filesystem::remove(m_Path.string() + "-shm", Error);
		}

		std::filesystem::path m_Path;
		storage::C_DATABASE m_Database;
		storage::C_REPOSITORIES m_Repositories;
		app::C_REPOSITORY_DRAFT_STORE m_DraftStore;
		domain::C_PARAGRAPH_PARSER m_Parser;
		std::unique_ptr<app::C_CARD_SERVICE> m_CardService;
		std::unique_ptr<app::C_DRAFT_COORDINATOR> m_Drafts;
		std::unique_ptr<app::C_SAVE_COORDINATOR> m_Save;
		d2d::C_D2D_DEVICE m_Device;
		d2d::C_D2D_TEXT m_Text;
		d2d::C_D2D_BRUSH_CACHE m_Brushes;
		TestWindow m_Parent;
		HWND m_hLeft{};
		HWND m_hRight{};
		std::unique_ptr<C_DOCUMENT_PAGE> m_Page;
		std::int64_t m_nClock{ 2000 };
		std::uint64_t m_nId{};
		inline static std::atomic<unsigned long> s_nSequence{};
	};
}

// ---------------------------------------------------------------------------
// 군 D — 보기 메뉴 토글과 설정 소비자
// ---------------------------------------------------------------------------

TEST_CASE("PLAN-W4-0018 view menu toggle persists and applies to the open list",
	"[W4-select][WTL-CAP-FI-029]")
{
	// 원본 tests/ui/test_card_multi_selection.py:333~364.
	C_TEMP_LOCAL_APP_DATA Local;
	C_REGISTRY_SCOPE Registry;
	C_WIN32_FILE_SYSTEM FileSystem;
	C_WIN32_DEVICE_SETTINGS Settings(FileSystem, Registry.Root());
	REQUIRE(Settings.Initialize());
	C_PAGE_FIXTURE Fixture;
	C_DOCUMENT_PAGE& Page = Fixture.Page();
	C_MENU_SCOPE Menu;

	bool bValue = true;
	REQUIRE(Settings.GetBool(MULTI_SELECTION_KEY, &bValue));
	REQUIRE_FALSE(bValue);

	const std::optional<bool> First = shell::ToggleMultiSelection(Settings, Menu.Get(), Page);
	REQUIRE(First.has_value());
	REQUIRE(*First);
	bValue = false;
	REQUIRE(Settings.GetBool(MULTI_SELECTION_KEY, &bValue));
	REQUIRE(bValue);
	REQUIRE((command_state(Menu.Get(), IDM_MULTI_SELECTION) & MFS_CHECKED) != 0);
	REQUIRE(Page.MultiSelectionEnabled());
	REQUIRE(Page.CardList().Projection() != nullptr);
	REQUIRE(Page.CardList().Projection()->SelectionMode() ==
		domain::E_CARD_SELECTION_MODE::Extended);

	// 값이 INI 파일에 실렸는가 - 같은 루트를 여는 새 객체가 읽어낸다.
	{
		C_WIN32_DEVICE_SETTINGS Fresh(FileSystem, Registry.Root());
		REQUIRE(Fresh.Initialize());
		bool bPersisted = false;
		REQUIRE(Fresh.GetBool(MULTI_SELECTION_KEY, &bPersisted));
		REQUIRE(bPersisted);
	}

	const std::optional<bool> Second = shell::ToggleMultiSelection(Settings, Menu.Get(), Page);
	REQUIRE(Second.has_value());
	REQUIRE_FALSE(*Second);
	bValue = true;
	REQUIRE(Settings.GetBool(MULTI_SELECTION_KEY, &bValue));
	REQUIRE_FALSE(bValue);
	REQUIRE((command_state(Menu.Get(), IDM_MULTI_SELECTION) & MFS_CHECKED) == 0);
	REQUIRE_FALSE(Page.MultiSelectionEnabled());
	REQUIRE(Page.CardList().Projection()->SelectionMode() ==
		domain::E_CARD_SELECTION_MODE::Single);
}

TEST_CASE("PLAN-W4-0019 disabled selection command snapshot carries only the one selected card",
	"[W4-select][WTL-CAP-FI-029]")
{
	// 원본 tests/ui/test_card_multi_selection.py:448~487 의 선택 절반이다.
	// 파일 절반(내보내기 .txt 내용)은 PLAN-W7-0021/0022 이 닫는다.
	C_PAGE_FIXTURE Fixture;
	C_DOCUMENT_PAGE& Page = Fixture.Page();
	Fixture.CreateCards(6);
	REQUIRE(::SendMessageW(Page.CardListHwnd(), LB_GETCOUNT, 0, 0) == 6);

	Page.SetMultiSelectionEnabled(true);
	const HWND hList = Page.CardListHwnd();
	::SetFocus(hList);
	send_click(hList, Fixture.RowPoint(1));
	send_click(hList, Fixture.RowPoint(2), MK_CONTROL);
	send_click(hList, Fixture.RowPoint(3), MK_CONTROL);
	REQUIRE(Page.CardList().Projection()->SelectedCardIds().size() == 3);

	Page.SetMultiSelectionEnabled(false);
	REQUIRE(Page.CardList().Projection()->SelectedCardIds().size() == 1);
	REQUIRE(Page.CardList().Projection()->CopySelectionForCommand().size() == 1);

	// 원본 _select_context_index + 명령 스냅샷: 우클릭이 메뉴 대상만 그 행으로 바꾼다.
	const domain::S_CARD* pRowThree = Page.CardList().Projection()->CardAt(3);
	REQUIRE(pRowThree != nullptr);
	const std::string sRowThreeId = pRowThree->sId;
	send_right_click(hList, Fixture.RowPoint(3));
	REQUIRE(Page.CardList().Projection()->CopySelectionForCommand() ==
		std::vector<std::string>{ sRowThreeId });
}

TEST_CASE("PLAN-W3-0009 settings change syncs the view menu check and list selection mode",
	"[W4-select][WTL-CAP-FI-029]")
{
	// 원본 tests/ui/test_card_multi_selection.py:367~389 (sync_device_settings 소비자 절반).
	C_TEMP_LOCAL_APP_DATA Local;
	C_REGISTRY_SCOPE Registry;
	C_WIN32_FILE_SYSTEM FileSystem;
	C_WIN32_DEVICE_SETTINGS Settings(FileSystem, Registry.Root());
	REQUIRE(Settings.Initialize());
	C_PAGE_FIXTURE Fixture;
	C_DOCUMENT_PAGE& Page = Fixture.Page();
	C_MENU_SCOPE Menu;

	Settings.SetBool(MULTI_SELECTION_KEY, true);
	REQUIRE(shell::SyncMultiSelection(Settings, Menu.Get(), Page));
	REQUIRE((command_state(Menu.Get(), IDM_MULTI_SELECTION) & MFS_CHECKED) != 0);
	REQUIRE(Page.CardList().Projection()->SelectionMode() ==
		domain::E_CARD_SELECTION_MODE::Extended);

	Settings.SetBool(MULTI_SELECTION_KEY, false);
	REQUIRE(shell::SyncMultiSelection(Settings, Menu.Get(), Page));
	REQUIRE((command_state(Menu.Get(), IDM_MULTI_SELECTION) & MFS_CHECKED) == 0);
	REQUIRE(Page.CardList().Projection()->SelectionMode() ==
		domain::E_CARD_SELECTION_MODE::Single);
}

TEST_CASE("PLAN-W3-0013 activating a window picks up another windows multi selection change",
	"[W4-select][WTL-CAP-FI-029]")
{
	// 원본 tests/ui/test_card_multi_selection.py:517~542. 단일 인스턴스라 두 창이 설정 객체
	// 하나를 공유한다 - 파일 재읽기 seam 은 없고 활성화 때의 Sync 가 그 자리다(spec §3.3.3).
	// 창 A/B 는 각자의 임시 DB 를 쓰는 페이지 fixture 두 개로 모델링한다.
	C_TEMP_LOCAL_APP_DATA Local;
	C_REGISTRY_SCOPE Registry;
	C_WIN32_FILE_SYSTEM FileSystem;
	C_WIN32_DEVICE_SETTINGS Settings(FileSystem, Registry.Root());
	REQUIRE(Settings.Initialize());
	C_PAGE_FIXTURE FixtureA;
	C_PAGE_FIXTURE FixtureB;
	C_MENU_SCOPE MenuA;
	C_MENU_SCOPE MenuB;

	const std::optional<bool> Toggled =
		shell::ToggleMultiSelection(Settings, MenuB.Get(), FixtureB.Page());
	REQUIRE(Toggled.has_value());
	REQUIRE(*Toggled);
	REQUIRE((command_state(MenuB.Get(), IDM_MULTI_SELECTION) & MFS_CHECKED) != 0);
	REQUIRE(FixtureB.Page().CardList().Projection()->SelectionMode() ==
		domain::E_CARD_SELECTION_MODE::Extended);
	// A 는 아직 동기화하지 않았다.
	REQUIRE((command_state(MenuA.Get(), IDM_MULTI_SELECTION) & MFS_CHECKED) == 0);
	REQUIRE(FixtureA.Page().CardList().Projection()->SelectionMode() ==
		domain::E_CARD_SELECTION_MODE::Single);

	// OnActivate 의 쌍둥이. Initialize() 재읽기는 쓰지 않는다(INI 를 다시 쓰기까지 한다).
	REQUIRE(shell::SyncMultiSelection(Settings, MenuA.Get(), FixtureA.Page()));
	REQUIRE((command_state(MenuA.Get(), IDM_MULTI_SELECTION) & MFS_CHECKED) != 0);
	REQUIRE(FixtureA.Page().CardList().Projection()->SelectionMode() ==
		domain::E_CARD_SELECTION_MODE::Extended);
}

// ---------------------------------------------------------------------------
// 군 E — 클릭·빈 영역 인식
// ---------------------------------------------------------------------------

TEST_CASE("PLAN-W4-0032 ctrl click selects multiple cards without opening second",
	"[W4-select][WTL-CAP-FI-060]")
{
	// 원본 tests/ui/test_card_stream.py:514~539.
	C_SELECT_FIXTURE Fixture(true);
	send_click(Fixture.Hwnd(), Fixture.Pt(0));
	send_click(Fixture.Hwnd(), Fixture.Pt(1), MK_CONTROL);
	REQUIRE(Fixture.Selected() == cards_at({ 0, 1 }));
	REQUIRE(Fixture.Opened() == cards_at({ 0 }));
}

TEST_CASE("PLAN-W4-0033 empty area clean left click emits signal once",
	"[W4-select][WTL-CAP-FI-061][WTL-CAP-TI-024]")
{
	// 원본 tests/ui/test_card_stream.py:563~598.
	C_SELECT_FIXTURE Fixture;
	const POINT Empty = Fixture.EmptyPoint();
	send_click(Fixture.Hwnd(), Empty);
	REQUIRE(Fixture.Clicked() == 1);

	const POINT Jitter = Fixture.Offset(Empty, 2, 0);
	REQUIRE(Fixture.HitRow(Jitter) == -1);
	send_press(Fixture.Hwnd(), Empty);
	send_move(Fixture.Hwnd(), Jitter);
	REQUIRE(Fixture.State() == E_CARD_LIST_VIEW_STATE::NoState);
	send_release(Fixture.Hwnd(), Jitter);
	REQUIRE(Fixture.Clicked() == 2);
}

TEST_CASE("PLAN-W4-0034 subthreshold card boundary rubber band does not emit empty click",
	"[W4-select][WTL-CAP-TI-024]")
{
	// 원본 tests/ui/test_card_stream.py:601~626, 오라클 M13. 원본 fixture 는 카드 한 장이라
	// 그 행 0 이 마지막 행이다 - 5장 fixture 에서는 마지막 행이 r4 다.
	C_SELECT_FIXTURE Fixture;
	const POINT Start = Fixture.Below(4, 2);
	const POINT Crossed = Fixture.Below(4, -2);
	REQUIRE(Fixture.HitRow(Start) == -1);
	REQUIRE(Fixture.HitRow(Crossed) == 4);
	REQUIRE(manhattan(Start, Crossed) < DRAG_THRESHOLD_DIP);

	send_press(Fixture.Hwnd(), Start);
	send_move(Fixture.Hwnd(), Crossed);
	REQUIRE(Fixture.State() == E_CARD_LIST_VIEW_STATE::DragSelecting);
	REQUIRE(Fixture.Selected() == cards_at({ 4 }));
	REQUIRE(Fixture.CurrentRow() == 4);
	send_release(Fixture.Hwnd(), Start);
	REQUIRE(Fixture.Clicked() == 0);
	REQUIRE(Fixture.Selected() == cards_at({ 4 }));
}

TEST_CASE("PLAN-W4-0035 round trip rubber band does not emit empty area click",
	"[W4-select][WTL-CAP-TI-024]")
{
	// 원본 tests/ui/test_card_stream.py:629~650, 오라클 M14 계열.
	C_SELECT_FIXTURE Fixture;
	const POINT Empty = Fixture.EmptyPoint();
	const POINT Far = Fixture.Offset(Empty, DRAG_THRESHOLD_DIP + 5, 0);
	const POINT Near = Fixture.Offset(Empty, 1, 0);
	REQUIRE(Fixture.HitRow(Far) == -1);
	REQUIRE(Fixture.HitRow(Near) == -1);

	send_press(Fixture.Hwnd(), Empty);
	send_move(Fixture.Hwnd(), Far);
	REQUIRE(Fixture.State() == E_CARD_LIST_VIEW_STATE::NoState);
	send_move(Fixture.Hwnd(), Near);
	REQUIRE(Fixture.State() == E_CARD_LIST_VIEW_STATE::NoState);
	send_release(Fixture.Hwnd(), Near);
	REQUIRE(Fixture.Clicked() == 0);
	REQUIRE(Fixture.Selected().empty());
}

TEST_CASE("PLAN-W4-0036 empty area release with modifier does not emit signal",
	"[W4-select][WTL-CAP-TI-024]")
{
	// 원본 tests/ui/test_card_stream.py:653~668, 오라클 M17b.
	C_SELECT_FIXTURE Fixture;
	const POINT Empty = Fixture.EmptyPoint();
	send_press(Fixture.Hwnd(), Empty);
	send_release(Fixture.Hwnd(), Empty, MK_CONTROL);
	REQUIRE(Fixture.Clicked() == 0);
}

TEST_CASE("PLAN-W4-0037 empty area ctrl press then unmodified release does not emit signal",
	"[W4-select][WTL-CAP-TI-024]")
{
	// 원본 tests/ui/test_card_stream.py:671~688, 오라클 M17a.
	C_SELECT_FIXTURE Fixture;
	const POINT Empty = Fixture.EmptyPoint();
	send_press(Fixture.Hwnd(), Empty, MK_CONTROL);
	send_release(Fixture.Hwnd(), Empty);
	REQUIRE(Fixture.Clicked() == 0);
}

TEST_CASE("PLAN-W4-0038 empty area shift press then unmodified release does not emit signal",
	"[W4-select][WTL-CAP-TI-024]")
{
	// 원본 tests/ui/test_card_stream.py:691~707.
	C_SELECT_FIXTURE Fixture;
	const POINT Empty = Fixture.EmptyPoint();
	send_press(Fixture.Hwnd(), Empty, MK_SHIFT);
	send_release(Fixture.Hwnd(), Empty);
	REQUIRE(Fixture.Clicked() == 0);
}

TEST_CASE("PLAN-W4-0039 empty area shift release does not emit signal",
	"[W4-select][WTL-CAP-TI-024]")
{
	// 원본 tests/ui/test_card_stream.py:710~724.
	C_SELECT_FIXTURE Fixture;
	const POINT Empty = Fixture.EmptyPoint();
	send_press(Fixture.Hwnd(), Empty);
	send_release(Fixture.Hwnd(), Empty, MK_SHIFT);
	REQUIRE(Fixture.Clicked() == 0);
}

TEST_CASE("PLAN-W4-0040 card click opens card without empty area signal",
	"[W4-select][WTL-CAP-FI-060]")
{
	// 원본 tests/ui/test_card_stream.py:727~738, 오라클 M1.
	C_SELECT_FIXTURE Fixture;
	send_click(Fixture.Hwnd(), Fixture.Pt(0));
	REQUIRE(Fixture.Clicked() == 0);
	REQUIRE(Fixture.Opened() == cards_at({ 0 }));
}

TEST_CASE("PLAN-W4-0041 empty press then card release does not emit empty area signal",
	"[W4-select][WTL-CAP-TI-024]")
{
	// 원본 tests/ui/test_card_stream.py:741~757. 여는 카드는 release 지점의 행이다
	// (spec §3.1.9 7, 오라클 M15/M20 의 release 규칙).
	C_SELECT_FIXTURE Fixture;
	send_press(Fixture.Hwnd(), Fixture.EmptyPoint());
	send_release(Fixture.Hwnd(), Fixture.Pt(0));
	REQUIRE(Fixture.Clicked() == 0);
	REQUIRE(Fixture.Opened() == cards_at({ 0 }));
}

TEST_CASE("PLAN-W4-0042 card press then empty release does not emit empty area signal",
	"[W4-select][WTL-CAP-TI-024]")
{
	// 원본 tests/ui/test_card_stream.py:760~776.
	C_SELECT_FIXTURE Fixture;
	send_press(Fixture.Hwnd(), Fixture.Pt(0));
	send_release(Fixture.Hwnd(), Fixture.EmptyPoint());
	REQUIRE(Fixture.Clicked() == 0);
}

TEST_CASE("PLAN-W4-0043 empty area right click does not emit signal",
	"[W4-select][WTL-CAP-TI-024]")
{
	// 원본 tests/ui/test_card_stream.py:779~787, 오라클 M18.
	C_SELECT_FIXTURE Fixture;
	send_right_click(Fixture.Hwnd(), Fixture.EmptyPoint());
	REQUIRE(Fixture.Clicked() == 0);
	REQUIRE(Fixture.Selected().empty());
	REQUIRE(Fixture.CurrentRow() == 0);
}

TEST_CASE("PLAN-W4-0044 empty area release outside viewport does not emit signal",
	"[W4-select][WTL-CAP-TI-024]")
{
	// 원본 tests/ui/test_card_stream.py:790~810. 캡처가 잡고 있으므로 뷰포트 밖 좌표가
	// 그대로 실려 온다 - 음수 좌표는 GET_X_LPARAM 의 부호 판독을 본다.
	C_SELECT_FIXTURE Fixture;
	const int nWidth = Fixture.Control().ViewportWidthDip();
	const int nEmptyY = Fixture.EmptyPoint().y;

	send_press(Fixture.Hwnd(), POINT{ nWidth - 1, nEmptyY });
	send_release(Fixture.Hwnd(), POINT{ nWidth + 1, nEmptyY });
	REQUIRE(Fixture.Clicked() == 0);

	send_press(Fixture.Hwnd(), POINT{ 1, nEmptyY });
	send_release(Fixture.Hwnd(), POINT{ -5, nEmptyY });
	REQUIRE(Fixture.Clicked() == 0);
}

TEST_CASE("PLAN-W4-0045 card right click preserves context selection without empty signal",
	"[W4-select][WTL-CAP-FI-060]")
{
	// 원본 tests/ui/test_card_stream.py:813~824, 오라클 M9a/M9b.
	{
		C_SELECT_FIXTURE Fixture;
		send_right_click(Fixture.Hwnd(), Fixture.Pt(0));
		REQUIRE(Fixture.Clicked() == 0);
		REQUIRE(Fixture.Selected() == cards_at({ 0 }));
	}
	{
		C_SELECT_FIXTURE Fixture(true);
		// M9a: 미선택 행 우클릭은 다중 선택을 깬다.
		send_click(Fixture.Hwnd(), Fixture.Pt(1));
		send_click(Fixture.Hwnd(), Fixture.Pt(3), MK_CONTROL);
		send_right_click(Fixture.Hwnd(), Fixture.Pt(2));
		REQUIRE(Fixture.Selected() == cards_at({ 2 }));
		REQUIRE(Fixture.CurrentRow() == 2);

		// M9b: 선택된 행 우클릭은 선택을 유지한다.
		Fixture.Reset();
		send_click(Fixture.Hwnd(), Fixture.Pt(1));
		send_click(Fixture.Hwnd(), Fixture.Pt(3), MK_CONTROL);
		send_right_click(Fixture.Hwnd(), Fixture.Pt(3));
		REQUIRE(Fixture.Selected() == cards_at({ 1, 3 }));
		REQUIRE(Fixture.CurrentRow() == 3);
	}
}

TEST_CASE("PLAN-W4-0046 clicking card bottom opens card without expand toggle",
	"[W4-select][WTL-CAP-FI-060]")
{
	// 원본 tests/ui/test_card_stream.py:827~844. 카드 인셋은 행 사각 안이라 아래쪽 여백을
	// 눌러도 그 행이 맞는다(Qt visualRect = 행 사각).
	std::wstring sBody;
	for (int nLine = 1; nLine <= 10; ++nLine)
	{
		if (nLine > 1) { sBody += L"\n"; }
		sBody += std::to_wstring(nLine) + L"줄";
	}
	C_SELECT_FIXTURE Fixture(false, { sBody });
	const S_DIP_RECT Row = Fixture.Control().RowRectDip(0);
	const POINT Point{ Row.Right() - 20, Row.Bottom() - 16 };
	REQUIRE(Fixture.HitRow(Point) == 0);
	send_click(Fixture.Hwnd(), Point);
	REQUIRE(Fixture.Opened() == cards_at({ 0 }));
}

// ---------------------------------------------------------------------------
// 순수 해석기 표 — 시퀀스 단계 -> 입력 튜플 -> 명령
// ---------------------------------------------------------------------------

TEST_CASE("W4 selection command resolver matches the measured Qt oracle in single and extended modes",
	"[W4-select][WTL-CAP-FI-060]")
{
	using Mode = domain::E_CARD_SELECTION_MODE;
	using Phase = E_CARD_INPUT_PHASE;
	using Command = E_CARD_SELECTION_COMMAND;

	struct S_ROW
	{
		const char* pszStep;   // 오라클 시퀀스의 어느 단계가 이 튜플을 만드는가
		Mode eMode;
		Phase ePhase;
		bool bRowValid;
		bool bRowSelected;
		bool bPressedAlreadySelected;
		bool bSamePressedRow;
		bool bCtrl;
		bool bShift;
		bool bDragSelecting;
		int nKey;
		Command eExpected;
	};

	// M9/M18/M19 는 해석기에 닿지 않는다(우클릭은 _select_context_index 가, 더블클릭은
	// press 와 같은 경로가 처리한다). 아래 표는 M1~M20 과 K1~K7 이 만드는 튜플이다.
	const S_ROW Rows[] = {
		// --- single ---
		{ "M1 press r1", Mode::Single, Phase::Press, true, false, false, true, false, false, false, 0, Command::ClearAndSelect },
		{ "M1 release r1", Mode::Single, Phase::Release, true, true, false, true, false, false, false, 0, Command::NoUpdate },
		{ "M2 ctrl press unselected r3", Mode::Single, Phase::Press, true, false, false, true, true, false, false, 0, Command::ClearAndSelect },
		{ "M3 ctrl press selected r1", Mode::Single, Phase::Press, true, true, true, true, true, false, false, 0, Command::Deselect },
		{ "M5 shift press r3", Mode::Single, Phase::Press, true, false, false, true, false, true, false, 0, Command::ClearAndSelect },
		{ "M10 press selected r1", Mode::Single, Phase::Press, true, true, true, true, false, false, false, 0, Command::ClearAndSelect },
		{ "M12 press empty", Mode::Single, Phase::Press, false, false, false, true, false, false, false, 0, Command::ClearAndSelect },
		{ "M12 release empty", Mode::Single, Phase::Release, false, false, false, true, false, false, false, 0, Command::NoUpdate },
		{ "M13 move into r4", Mode::Single, Phase::Move, true, false, false, false, false, false, true, 0, Command::BandReplace },
		{ "M15 move onto r2", Mode::Single, Phase::Move, true, false, false, false, false, false, true, 0, Command::BandReplace },
		{ "M16 release empty with selection", Mode::Single, Phase::Release, false, false, false, true, false, false, false, 0, Command::NoUpdate },
		{ "K1 Down", Mode::Single, Phase::Key, true, false, false, false, false, false, false, VK_DOWN, Command::ClearAndSelect },
		{ "K3 Ctrl+Down", Mode::Single, Phase::Key, true, false, false, false, true, false, false, VK_DOWN, Command::ClearAndSelect },
		{ "K3 Ctrl+Space on selected", Mode::Single, Phase::Key, true, true, false, false, true, false, false, VK_SPACE, Command::Deselect },
		{ "K3 Space on unselected", Mode::Single, Phase::Key, true, false, false, false, false, false, false, VK_SPACE, Command::ClearAndSelect },
		{ "K7 Shift+End", Mode::Single, Phase::Key, true, false, false, false, false, true, false, VK_END, Command::ClearAndSelect },
		// --- extended ---
		{ "M1 press r1", Mode::Extended, Phase::Press, true, false, false, true, false, false, false, 0, Command::ClearAndSelect },
		{ "M10 press selected r1", Mode::Extended, Phase::Press, true, true, true, true, false, false, false, 0, Command::NoUpdate },
		{ "M10 release same selected row", Mode::Extended, Phase::Release, true, true, true, true, false, false, false, 0, Command::ClearAndSelect },
		{ "M2 ctrl press unselected r3", Mode::Extended, Phase::Press, true, false, false, true, true, false, false, 0, Command::Toggle },
		{ "M4 ctrl press selected r1", Mode::Extended, Phase::Press, true, true, true, true, true, false, false, 0, Command::NoUpdate },
		{ "M4 ctrl release same selected row", Mode::Extended, Phase::Release, true, true, true, true, true, false, false, 0, Command::Toggle },
		{ "M5 shift press r3", Mode::Extended, Phase::Press, true, false, false, true, false, true, false, 0, Command::SelectCurrent },
		{ "M5 shift release", Mode::Extended, Phase::Release, true, true, false, true, false, true, false, 0, Command::NoUpdate },
		{ "M12 press empty", Mode::Extended, Phase::Press, false, false, false, true, false, false, false, 0, Command::NoUpdate },
		{ "M16 release empty", Mode::Extended, Phase::Release, false, false, false, true, false, false, false, 0, Command::ClearAndSelect },
		{ "M13 release during band", Mode::Extended, Phase::Release, false, false, false, true, false, false, true, 0, Command::NoUpdate },
		{ "M15 move onto row", Mode::Extended, Phase::Move, true, false, false, false, false, false, true, 0, Command::BandReplace },
		{ "ctrl band move", Mode::Extended, Phase::Move, true, false, false, false, true, false, true, 0, Command::BandToggle },
		{ "shift band move", Mode::Extended, Phase::Move, true, false, false, false, false, true, true, 0, Command::SelectCurrent },
		{ "K3 Ctrl+Down", Mode::Extended, Phase::Key, true, false, false, false, true, false, false, VK_DOWN, Command::NoUpdate },
		{ "K2 Shift+Down", Mode::Extended, Phase::Key, true, false, false, false, false, true, false, VK_DOWN, Command::SelectCurrent },
		{ "K3 Space on unselected", Mode::Extended, Phase::Key, true, false, false, false, false, false, false, VK_SPACE, Command::Select },
		{ "K3 Ctrl+Space on selected", Mode::Extended, Phase::Key, true, true, false, false, true, false, false, VK_SPACE, Command::Toggle },
		{ "K1 Down", Mode::Extended, Phase::Key, true, false, false, false, false, false, false, VK_DOWN, Command::ClearAndSelect },
	};

	for (const S_ROW& Row : Rows)
	{
		INFO("row " << Row.pszStep << (Row.eMode == Mode::Single ? " (single)" : " (extended)"));
		S_CARD_SELECTION_INPUT Input{};
		Input.eMode = Row.eMode;
		Input.ePhase = Row.ePhase;
		Input.bRowValid = Row.bRowValid;
		Input.bRowSelected = Row.bRowSelected;
		Input.bPressedAlreadySelected = Row.bPressedAlreadySelected;
		Input.bSamePressedRow = Row.bSamePressedRow;
		Input.bCtrl = Row.bCtrl;
		Input.bShift = Row.bShift;
		Input.bDragSelecting = Row.bDragSelecting;
		Input.nKey = Row.nKey;
		REQUIRE(ResolveSelectionCommand(Input) == Row.eExpected);
	}
}

// ---------------------------------------------------------------------------
// 오라클 마우스 시퀀스 M1~M20
// ---------------------------------------------------------------------------

TEST_CASE("W4 mouse selection sequences reproduce the Qt oracle in single mode",
	"[W4-select][WTL-CAP-FI-060]")
{
	constexpr auto NO_STATE = E_CARD_LIST_VIEW_STATE::NoState;
	C_SELECT_FIXTURE Fixture(false);
	const HWND hList = Fixture.Hwnd();

	// M1 -> M2 -> M4 -> M3 (앞 단계의 끝 상태가 다음 시퀀스의 시작 상태와 같다).
	send_click(hList, Fixture.Pt(1));
	require_step(Fixture, "M1 click r1", cards_at({ 1 }), 1, cards_at({ 1 }), 0, NO_STATE);
	send_click(hList, Fixture.Pt(3), MK_CONTROL);
	require_step(Fixture, "M2 ctrl+click r3", cards_at({ 3 }), 3, cards_at({ 1 }), 0, NO_STATE);
	send_click(hList, Fixture.Pt(1), MK_CONTROL);
	require_step(Fixture, "M4 ctrl+click r1", cards_at({ 1 }), 1, cards_at({ 1 }), 0, NO_STATE);
	send_click(hList, Fixture.Pt(1), MK_CONTROL);
	require_step(Fixture, "M3 ctrl+click selected r1", {}, 1, cards_at({ 1 }), 0, NO_STATE);

	// M5 - Shift 클릭도 열기를 억제하지 않는다.
	Fixture.Reset();
	send_click(hList, Fixture.Pt(1));
	send_click(hList, Fixture.Pt(3), MK_SHIFT);
	require_step(Fixture, "M5 shift r3", cards_at({ 3 }), 3, cards_at({ 1, 3 }), 0, NO_STATE);
	send_click(hList, Fixture.Pt(0), MK_SHIFT);
	require_step(Fixture, "M5 shift r0", cards_at({ 0 }), 0, cards_at({ 1, 3, 0 }), 0, NO_STATE);
	send_click(hList, Fixture.Pt(4), MK_SHIFT);
	require_step(Fixture, "M5 shift r4", cards_at({ 4 }), 4, cards_at({ 1, 3, 0, 4 }), 0, NO_STATE);

	// M6
	Fixture.Reset();
	send_click(hList, Fixture.Pt(1));
	send_click(hList, Fixture.Pt(3), MK_CONTROL);
	send_click(hList, Fixture.Pt(4), MK_SHIFT);
	require_step(Fixture, "M6 shift r4", cards_at({ 4 }), 4, cards_at({ 1, 4 }), 0, NO_STATE);

	// M7
	Fixture.Reset();
	send_click(hList, Fixture.Pt(1), MK_CONTROL);
	require_step(Fixture, "M7 ctrl+click r1", cards_at({ 1 }), 1, {}, 0, NO_STATE);

	// M8
	Fixture.Reset();
	send_click(hList, Fixture.Pt(2), MK_SHIFT);
	require_step(Fixture, "M8 shift r2", cards_at({ 2 }), 2, cards_at({ 2 }), 0, NO_STATE);

	// M9a
	Fixture.Reset();
	send_click(hList, Fixture.Pt(1));
	send_click(hList, Fixture.Pt(3), MK_CONTROL);
	send_right_click(hList, Fixture.Pt(2));
	REQUIRE(Fixture.Selected() == cards_at({ 2 }));
	REQUIRE(Fixture.CurrentRow() == 2);

	// M9b
	Fixture.Reset();
	send_click(hList, Fixture.Pt(1));
	send_click(hList, Fixture.Pt(3), MK_CONTROL);
	send_right_click(hList, Fixture.Pt(3));
	REQUIRE(Fixture.Selected() == cards_at({ 3 }));
	REQUIRE(Fixture.CurrentRow() == 3);

	// M10
	Fixture.Reset();
	send_click(hList, Fixture.Pt(1));
	send_click(hList, Fixture.Pt(3), MK_CONTROL);
	send_press(hList, Fixture.Pt(1));
	require_step(Fixture, "M10 press r1", cards_at({ 1 }), 1, cards_at({ 1 }), 0, NO_STATE);
	send_release(hList, Fixture.Pt(1));
	require_step(Fixture, "M10 release r1", cards_at({ 1 }), 1, cards_at({ 1, 1 }), 0, NO_STATE);

	// M11 - 임계 미달 이동도 DraggingState 로 들어가고 release 는 그대로 연다.
	Fixture.Reset();
	send_click(hList, Fixture.Pt(1));
	send_click(hList, Fixture.Pt(3), MK_CONTROL);
	send_press(hList, Fixture.Pt(1));
	{
		const POINT Jitter = Fixture.Offset(Fixture.Pt(1), 3, 3);
		send_move(hList, Jitter);
		require_step(Fixture, "M11 move 3,3", cards_at({ 1 }), 1, cards_at({ 1 }), 0,
			E_CARD_LIST_VIEW_STATE::Dragging);
		send_release(hList, Jitter);
		require_step(Fixture, "M11 release jitter", cards_at({ 1 }), 1, cards_at({ 1, 1 }), 0, NO_STATE);
	}

	// M12
	Fixture.Reset();
	send_press(hList, Fixture.EmptyPoint());
	require_step(Fixture, "M12 press empty", {}, 0, {}, 0, NO_STATE);
	send_release(hList, Fixture.EmptyPoint());
	require_step(Fixture, "M12 release empty", {}, 0, {}, 1, NO_STATE);

	// M13
	Fixture.Reset();
	send_press(hList, Fixture.Below(4, 2));
	require_step(Fixture, "M13 press below r4", {}, 0, {}, 0, NO_STATE);
	send_move(hList, Fixture.Below(4, -2));
	require_step(Fixture, "M13 move into r4", cards_at({ 4 }), 4, {}, 0,
		E_CARD_LIST_VIEW_STATE::DragSelecting);
	send_release(hList, Fixture.Below(4, 2));
	require_step(Fixture, "M13 release at start", cards_at({ 4 }), 4, {}, 0, NO_STATE);

	// M14 - 빈 영역 안에 머무르면 임계를 넘겨도 상태·선택이 그대로다.
	Fixture.Reset();
	send_press(hList, Fixture.EmptyPoint());
	send_move(hList, Fixture.Offset(Fixture.EmptyPoint(), DRAG_THRESHOLD_DIP + 5, 0));
	require_step(Fixture, "M14 move far", {}, 0, {}, 0, NO_STATE);
	send_release(hList, Fixture.Offset(Fixture.EmptyPoint(), DRAG_THRESHOLD_DIP + 5, 0));
	require_step(Fixture, "M14 release far", {}, 0, {}, 0, NO_STATE);

	// M15 - 단일 선택의 띠는 커서 아래 한 행만 따라간다.
	Fixture.Reset();
	send_press(hList, Fixture.EmptyPoint());
	send_move(hList, Fixture.Pt(4));
	require_step(Fixture, "M15 move r4", cards_at({ 4 }), 4, {}, 0,
		E_CARD_LIST_VIEW_STATE::DragSelecting);
	send_move(hList, Fixture.Pt(2));
	require_step(Fixture, "M15 move r2", cards_at({ 2 }), 2, {}, 0,
		E_CARD_LIST_VIEW_STATE::DragSelecting);
	send_release(hList, Fixture.Pt(2));
	require_step(Fixture, "M15 release r2", cards_at({ 2 }), 2, cards_at({ 2 }), 0, NO_STATE);

	// M16 - 단일 선택은 빈 영역 클릭에도 선택을 유지한다.
	Fixture.Reset();
	send_click(hList, Fixture.Pt(1));
	send_press(hList, Fixture.EmptyPoint());
	require_step(Fixture, "M16 press empty", cards_at({ 1 }), 1, cards_at({ 1 }), 0, NO_STATE);
	send_release(hList, Fixture.EmptyPoint());
	require_step(Fixture, "M16 release empty", cards_at({ 1 }), 1, cards_at({ 1 }), 1, NO_STATE);

	// M17a / M17b
	Fixture.Reset();
	send_press(hList, Fixture.EmptyPoint(), MK_CONTROL);
	send_release(hList, Fixture.EmptyPoint());
	require_step(Fixture, "M17a", {}, 0, {}, 0, NO_STATE);
	Fixture.Reset();
	send_press(hList, Fixture.EmptyPoint());
	send_release(hList, Fixture.EmptyPoint(), MK_CONTROL);
	require_step(Fixture, "M17b", {}, 0, {}, 0, NO_STATE);

	// M18
	Fixture.Reset();
	send_right_click(hList, Fixture.EmptyPoint());
	require_step(Fixture, "M18", {}, 0, {}, 0, NO_STATE);

	// M19 - 오라클 N5: QTest.mouseDClick 이 실제로 보낸 것은 더블클릭 이벤트 하나뿐이라
	// release 가 없었고 그래서 opened 가 늘지 않았다.
	Fixture.Reset();
	send_click(hList, Fixture.Pt(1));
	send_double_click(hList, Fixture.Pt(1));
	require_step(Fixture, "M19 lone dblclk", cards_at({ 1 }), 1, cards_at({ 1 }), 0, NO_STATE);

	// 실기의 더블클릭은 press/release/dblclk/release 라 release 가 2회다 - 열기도 2회다.
	Fixture.Reset();
	send_press(hList, Fixture.Pt(1));
	send_release(hList, Fixture.Pt(1));
	send_double_click(hList, Fixture.Pt(1));
	send_release(hList, Fixture.Pt(1));
	REQUIRE(Fixture.Opened() == cards_at({ 1, 1 }));

	// M20 - 여는 카드는 release 지점의 행이다.
	Fixture.Reset();
	send_click(hList, Fixture.Pt(1));
	send_press(hList, Fixture.Pt(3));
	require_step(Fixture, "M20 press r3", cards_at({ 3 }), 3, cards_at({ 1 }), 0, NO_STATE);
	send_release(hList, Fixture.Pt(3));
	require_step(Fixture, "M20 release r3", cards_at({ 3 }), 3, cards_at({ 1, 3 }), 0, NO_STATE);
}

TEST_CASE("W4 mouse selection sequences reproduce the Qt oracle in extended mode",
	"[W4-select][WTL-CAP-FI-060][WTL-CAP-FI-029]")
{
	constexpr auto NO_STATE = E_CARD_LIST_VIEW_STATE::NoState;
	C_SELECT_FIXTURE Fixture(true);
	const HWND hList = Fixture.Hwnd();

	// M1 -> M2 -> M4
	send_click(hList, Fixture.Pt(1));
	require_step(Fixture, "M1 click r1", cards_at({ 1 }), 1, cards_at({ 1 }), 0, NO_STATE);
	send_click(hList, Fixture.Pt(3), MK_CONTROL);
	require_step(Fixture, "M2 ctrl+click r3", cards_at({ 1, 3 }), 3, cards_at({ 1 }), 0, NO_STATE);
	send_click(hList, Fixture.Pt(1), MK_CONTROL);
	require_step(Fixture, "M4 ctrl+click r1", cards_at({ 3 }), 1, cards_at({ 1 }), 0, NO_STATE);

	// M3
	Fixture.Reset();
	send_click(hList, Fixture.Pt(1));
	send_click(hList, Fixture.Pt(1), MK_CONTROL);
	require_step(Fixture, "M3 ctrl+click selected r1", {}, 1, cards_at({ 1 }), 0, NO_STATE);

	// M5 - 앵커는 평범한 클릭 지점(r1)에 고정된다.
	Fixture.Reset();
	send_click(hList, Fixture.Pt(1));
	send_click(hList, Fixture.Pt(3), MK_SHIFT);
	require_step(Fixture, "M5 shift r3", cards_at({ 1, 2, 3 }), 3, cards_at({ 1, 3 }), 0, NO_STATE);
	send_click(hList, Fixture.Pt(0), MK_SHIFT);
	require_step(Fixture, "M5 shift r0", cards_at({ 0, 1 }), 0, cards_at({ 1, 3, 0 }), 0, NO_STATE);
	send_click(hList, Fixture.Pt(4), MK_SHIFT);
	require_step(Fixture, "M5 shift r4", cards_at({ 1, 2, 3, 4 }), 4,
		cards_at({ 1, 3, 0, 4 }), 0, NO_STATE);

	// M6 - Ctrl+클릭이 새 앵커가 되고 뒤이은 Shift 는 기존 선택을 유지한 채 범위를 더한다.
	Fixture.Reset();
	send_click(hList, Fixture.Pt(1));
	send_click(hList, Fixture.Pt(3), MK_CONTROL);
	send_click(hList, Fixture.Pt(4), MK_SHIFT);
	require_step(Fixture, "M6 shift r4", cards_at({ 1, 3, 4 }), 4, cards_at({ 1, 4 }), 0, NO_STATE);

	// M7
	Fixture.Reset();
	send_click(hList, Fixture.Pt(1), MK_CONTROL);
	require_step(Fixture, "M7 ctrl+click r1", cards_at({ 1 }), 1, {}, 0, NO_STATE);

	// M8 - 포커스 진입 현재 행(0)이 앵커가 된다.
	Fixture.Reset();
	send_click(hList, Fixture.Pt(2), MK_SHIFT);
	require_step(Fixture, "M8 shift r2", cards_at({ 0, 1, 2 }), 2, cards_at({ 2 }), 0, NO_STATE);

	// M9a
	Fixture.Reset();
	send_click(hList, Fixture.Pt(1));
	send_click(hList, Fixture.Pt(3), MK_CONTROL);
	send_right_click(hList, Fixture.Pt(2));
	REQUIRE(Fixture.Selected() == cards_at({ 2 }));
	REQUIRE(Fixture.CurrentRow() == 2);

	// M9b
	Fixture.Reset();
	send_click(hList, Fixture.Pt(1));
	send_click(hList, Fixture.Pt(3), MK_CONTROL);
	send_right_click(hList, Fixture.Pt(3));
	REQUIRE(Fixture.Selected() == cards_at({ 1, 3 }));
	REQUIRE(Fixture.CurrentRow() == 3);

	// M10 - 이미 선택된 행의 press 는 선택을 바꾸지 않고 release 에서 한 장으로 접힌다.
	Fixture.Reset();
	send_click(hList, Fixture.Pt(1));
	send_click(hList, Fixture.Pt(3), MK_CONTROL);
	send_press(hList, Fixture.Pt(1));
	require_step(Fixture, "M10 press r1", cards_at({ 1, 3 }), 1, cards_at({ 1 }), 0, NO_STATE);
	send_release(hList, Fixture.Pt(1));
	require_step(Fixture, "M10 release r1", cards_at({ 1 }), 1, cards_at({ 1, 1 }), 0, NO_STATE);

	// M11
	Fixture.Reset();
	send_click(hList, Fixture.Pt(1));
	send_click(hList, Fixture.Pt(3), MK_CONTROL);
	send_press(hList, Fixture.Pt(1));
	{
		const POINT Jitter = Fixture.Offset(Fixture.Pt(1), 3, 3);
		send_move(hList, Jitter);
		require_step(Fixture, "M11 move 3,3", cards_at({ 1, 3 }), 1, cards_at({ 1 }), 0,
			E_CARD_LIST_VIEW_STATE::Dragging);
		send_release(hList, Jitter);
		require_step(Fixture, "M11 release jitter", cards_at({ 1 }), 1, cards_at({ 1, 1 }), 0, NO_STATE);
	}

	// M12
	Fixture.Reset();
	send_press(hList, Fixture.EmptyPoint());
	require_step(Fixture, "M12 press empty", {}, 0, {}, 0, NO_STATE);
	send_release(hList, Fixture.EmptyPoint());
	require_step(Fixture, "M12 release empty", {}, 0, {}, 1, NO_STATE);

	// M13
	Fixture.Reset();
	send_press(hList, Fixture.Below(4, 2));
	require_step(Fixture, "M13 press below r4", {}, 0, {}, 0, NO_STATE);
	send_move(hList, Fixture.Below(4, -2));
	require_step(Fixture, "M13 move into r4", cards_at({ 4 }), 4, {}, 0,
		E_CARD_LIST_VIEW_STATE::DragSelecting);
	send_release(hList, Fixture.Below(4, 2));
	require_step(Fixture, "M13 release at start", cards_at({ 4 }), 4, {}, 0, NO_STATE);

	// M14
	Fixture.Reset();
	send_press(hList, Fixture.EmptyPoint());
	send_move(hList, Fixture.Offset(Fixture.EmptyPoint(), DRAG_THRESHOLD_DIP + 5, 0));
	require_step(Fixture, "M14 move far", {}, 0, {}, 0, NO_STATE);
	send_release(hList, Fixture.Offset(Fixture.EmptyPoint(), DRAG_THRESHOLD_DIP + 5, 0));
	require_step(Fixture, "M14 release far", {}, 0, {}, 0, NO_STATE);

	// M15 - 확장 선택의 띠는 지나온 범위를 누적한다.
	Fixture.Reset();
	send_press(hList, Fixture.EmptyPoint());
	send_move(hList, Fixture.Pt(4));
	require_step(Fixture, "M15 move r4", cards_at({ 4 }), 4, {}, 0,
		E_CARD_LIST_VIEW_STATE::DragSelecting);
	send_move(hList, Fixture.Pt(2));
	require_step(Fixture, "M15 move r2", cards_at({ 2, 3, 4 }), 2, {}, 0,
		E_CARD_LIST_VIEW_STATE::DragSelecting);
	send_release(hList, Fixture.Pt(2));
	require_step(Fixture, "M15 release r2", cards_at({ 2, 3, 4 }), 2, cards_at({ 2 }), 0, NO_STATE);

	// M16 - 확장 선택은 빈 영역 클릭에서 선택을 비운다(현재는 그대로).
	Fixture.Reset();
	send_click(hList, Fixture.Pt(1));
	send_press(hList, Fixture.EmptyPoint());
	require_step(Fixture, "M16 press empty", cards_at({ 1 }), 1, cards_at({ 1 }), 0, NO_STATE);
	send_release(hList, Fixture.EmptyPoint());
	require_step(Fixture, "M16 release empty", {}, 1, cards_at({ 1 }), 1, NO_STATE);

	// M17a / M17b
	Fixture.Reset();
	send_press(hList, Fixture.EmptyPoint(), MK_CONTROL);
	send_release(hList, Fixture.EmptyPoint());
	require_step(Fixture, "M17a", {}, 0, {}, 0, NO_STATE);
	Fixture.Reset();
	send_press(hList, Fixture.EmptyPoint());
	send_release(hList, Fixture.EmptyPoint(), MK_CONTROL);
	require_step(Fixture, "M17b", {}, 0, {}, 0, NO_STATE);

	// M18
	Fixture.Reset();
	send_right_click(hList, Fixture.EmptyPoint());
	require_step(Fixture, "M18", {}, 0, {}, 0, NO_STATE);

	// M19
	Fixture.Reset();
	send_click(hList, Fixture.Pt(1));
	send_double_click(hList, Fixture.Pt(1));
	require_step(Fixture, "M19 lone dblclk", cards_at({ 1 }), 1, cards_at({ 1 }), 0, NO_STATE);

	Fixture.Reset();
	send_press(hList, Fixture.Pt(1));
	send_release(hList, Fixture.Pt(1));
	send_double_click(hList, Fixture.Pt(1));
	send_release(hList, Fixture.Pt(1));
	REQUIRE(Fixture.Opened() == cards_at({ 1, 1 }));

	// M20
	Fixture.Reset();
	send_click(hList, Fixture.Pt(1));
	send_press(hList, Fixture.Pt(3));
	require_step(Fixture, "M20 press r3", cards_at({ 3 }), 3, cards_at({ 1 }), 0, NO_STATE);
	send_release(hList, Fixture.Pt(3));
	require_step(Fixture, "M20 release r3", cards_at({ 3 }), 3, cards_at({ 1, 3 }), 0, NO_STATE);

	// L2 P1 - Shift press 뒤 평범한 release 는 범위를 그대로 둔다(release 명령이 적용되지 않는다).
	Fixture.Reset();
	send_click(hList, Fixture.Pt(1));
	send_press(hList, Fixture.Pt(3), MK_SHIFT);
	send_release(hList, Fixture.Pt(3));
	REQUIRE(Fixture.Selected() == cards_at({ 1, 2, 3 }));

	// L2 P2 - Ctrl press(미선택 행) 뒤 평범한 release 도 다중 선택을 유지한다.
	Fixture.Reset();
	send_click(hList, Fixture.Pt(1));
	send_press(hList, Fixture.Pt(3), MK_CONTROL);
	send_release(hList, Fixture.Pt(3));
	REQUIRE(Fixture.Selected() == cards_at({ 1, 3 }));

	// L2 P4 - 빈 영역 Ctrl press 뒤 평범한 release 는 선택을 비우되 신호는 내지 않는다.
	Fixture.Reset();
	send_click(hList, Fixture.Pt(1));
	send_press(hList, Fixture.EmptyPoint(), MK_CONTROL);
	send_release(hList, Fixture.EmptyPoint());
	REQUIRE(Fixture.Selected().empty());
	REQUIRE(Fixture.CurrentRow() == 1);
	REQUIRE(Fixture.Clicked() == 0);

	// L2 P9 - 임계를 넘긴 드래그는 다중 선택을 접지 않는다.
	Fixture.Reset();
	send_click(hList, Fixture.Pt(1));
	send_click(hList, Fixture.Pt(3), MK_CONTROL);
	{
		const std::vector<std::string> Opened = Fixture.Opened();
		send_press(hList, Fixture.Pt(1));
		send_move(hList, Fixture.Offset(Fixture.Pt(1), 8, 8));
		REQUIRE(Fixture.State() == NO_STATE);
		send_release(hList, Fixture.Pt(1));
		REQUIRE(Fixture.Selected() == cards_at({ 1, 3 }));
		REQUIRE(Fixture.Opened() == Opened);
		REQUIRE(Fixture.State() == NO_STATE);
	}
}

// ---------------------------------------------------------------------------
// 오라클 키보드 시퀀스 K1~K7
// ---------------------------------------------------------------------------

TEST_CASE("W4 keyboard navigation reproduces the Qt oracle in both modes",
	"[W4-select][WTL-CAP-FI-062]")
{
	for (const bool bExtended : { false, true })
	{
		INFO("mode " << (bExtended ? "extended" : "single"));
		C_SELECT_FIXTURE Fixture(bExtended);
		const HWND hList = Fixture.Hwnd();

		// K1 - 평범한 방향키는 현재와 선택을 함께 옮긴다. 전 행이 한 화면이라
		// PageDown = 마지막 행, PageUp = 첫 행이다(오라클 N1).
		send_click(hList, Fixture.Pt(1));
		send_key(hList, VK_DOWN);
		REQUIRE(Fixture.Selected() == cards_at({ 2 }));
		REQUIRE(Fixture.CurrentRow() == 2);
		send_key(hList, VK_DOWN);
		REQUIRE(Fixture.Selected() == cards_at({ 3 }));
		send_key(hList, VK_UP);
		REQUIRE(Fixture.Selected() == cards_at({ 2 }));
		send_key(hList, VK_HOME);
		REQUIRE(Fixture.Selected() == cards_at({ 0 }));
		send_key(hList, VK_END);
		REQUIRE(Fixture.Selected() == cards_at({ 4 }));
		send_key(hList, VK_NEXT);
		REQUIRE(Fixture.Selected() == cards_at({ 4 }));
		REQUIRE(Fixture.CurrentRow() == 4);
		send_key(hList, VK_PRIOR);
		REQUIRE(Fixture.Selected() == cards_at({ 0 }));
		REQUIRE(Fixture.CurrentRow() == 0);

		// K1b - 포커스 진입이 이미 행 0 을 현재로 잡았으므로 Up 은 아무것도 바꾸지 않는다.
		Fixture.Reset();
		send_key(hList, VK_UP);
		REQUIRE(Fixture.Selected().empty());
		REQUIRE(Fixture.CurrentRow() == 0);
		send_key(hList, VK_DOWN);
		REQUIRE(Fixture.Selected() == cards_at({ 1 }));
		REQUIRE(Fixture.CurrentRow() == 1);
		send_key(hList, VK_END);
		REQUIRE(Fixture.Selected() == cards_at({ 4 }));
		REQUIRE(Fixture.CurrentRow() == 4);

		// K2 - Shift 는 앵커에서 범위를 뻗고, 평범한 이동이 앵커를 옮긴다.
		Fixture.Reset();
		send_click(hList, Fixture.Pt(1));
		send_key(hList, VK_DOWN, { VK_SHIFT });
		REQUIRE(Fixture.Selected() == (bExtended ? cards_at({ 1, 2 }) : cards_at({ 2 })));
		send_key(hList, VK_DOWN, { VK_SHIFT });
		REQUIRE(Fixture.Selected() == (bExtended ? cards_at({ 1, 2, 3 }) : cards_at({ 3 })));
		send_key(hList, VK_DOWN);
		REQUIRE(Fixture.Selected() == cards_at({ 4 }));
		send_key(hList, VK_UP, { VK_SHIFT });
		REQUIRE(Fixture.Selected() == (bExtended ? cards_at({ 3, 4 }) : cards_at({ 3 })));
		REQUIRE(Fixture.CurrentRow() == 3);

		// K3 - Ctrl+방향키는 현재만 옮기고(확장), Space 는 현재 행을 더하거나 토글한다.
		Fixture.Reset();
		send_click(hList, Fixture.Pt(1));
		send_key(hList, VK_DOWN, { VK_CONTROL });
		REQUIRE(Fixture.Selected() == (bExtended ? cards_at({ 1 }) : cards_at({ 2 })));
		REQUIRE(Fixture.CurrentRow() == 2);
		send_key(hList, VK_DOWN, { VK_CONTROL });
		REQUIRE(Fixture.Selected() == (bExtended ? cards_at({ 1 }) : cards_at({ 3 })));
		REQUIRE(Fixture.CurrentRow() == 3);
		send_key(hList, VK_SPACE, { VK_CONTROL });
		REQUIRE(Fixture.Selected() ==
			(bExtended ? cards_at({ 1, 3 }) : std::vector<std::string>{}));
		REQUIRE(Fixture.CurrentRow() == 3);
		send_key(hList, VK_SPACE);
		REQUIRE(Fixture.Selected() == (bExtended ? cards_at({ 1, 3 }) : cards_at({ 3 })));
		REQUIRE(Fixture.CurrentRow() == 3);

		// K4 - Delete 는 선택 전부를, Enter 는 현재 카드를 대상으로 한다(선택은 유지된다).
		Fixture.Reset();
		send_click(hList, Fixture.Pt(1));
		send_click(hList, Fixture.Pt(3), MK_CONTROL);
		send_key(hList, VK_DELETE);
		REQUIRE(Fixture.Deleted().size() == 1);
		REQUIRE(Fixture.Deleted().front() == (bExtended ? cards_at({ 1, 3 }) : cards_at({ 3 })));

		Fixture.Reset();
		send_click(hList, Fixture.Pt(1));
		send_key(hList, VK_RETURN);
		REQUIRE(Fixture.Activated() == 1);
		REQUIRE(Fixture.Selected() == cards_at({ 1 }));

		Fixture.Reset();
		send_click(hList, Fixture.Pt(1));
		// 키패드 Enter 는 lParam 의 확장 키 비트(0x01000000)만 다르다.
		send_key(hList, VK_RETURN, {}, 0x01000001);
		REQUIRE(Fixture.Activated() == 1);
		REQUIRE(Fixture.Selected() == cards_at({ 1 }));

		// K5 - 전 폭 행의 ListMode 에서 좌우는 현재를 그대로 둔다.
		Fixture.Reset();
		send_click(hList, Fixture.Pt(1));
		send_key(hList, VK_LEFT);
		REQUIRE(Fixture.Selected() == cards_at({ 1 }));
		REQUIRE(Fixture.CurrentRow() == 1);
		send_key(hList, VK_RIGHT);
		REQUIRE(Fixture.Selected() == cards_at({ 1 }));
		REQUIRE(Fixture.CurrentRow() == 1);

		// K7 - Shift/Ctrl 을 붙인 Home/End/PageDown.
		Fixture.Reset();
		send_click(hList, Fixture.Pt(1));
		send_key(hList, VK_END, { VK_SHIFT });
		REQUIRE(Fixture.Selected() == (bExtended ? cards_at({ 1, 2, 3, 4 }) : cards_at({ 4 })));
		REQUIRE(Fixture.CurrentRow() == 4);

		Fixture.Reset();
		send_click(hList, Fixture.Pt(3));
		send_key(hList, VK_HOME, { VK_SHIFT });
		REQUIRE(Fixture.Selected() == (bExtended ? cards_at({ 0, 1, 2, 3 }) : cards_at({ 0 })));
		REQUIRE(Fixture.CurrentRow() == 0);

		Fixture.Reset();
		send_click(hList, Fixture.Pt(1));
		send_key(hList, VK_END, { VK_CONTROL });
		REQUIRE(Fixture.Selected() == (bExtended ? cards_at({ 1 }) : cards_at({ 4 })));
		REQUIRE(Fixture.CurrentRow() == 4);

		Fixture.Reset();
		send_click(hList, Fixture.Pt(1));
		send_key(hList, VK_NEXT, { VK_SHIFT });
		REQUIRE(Fixture.Selected() == (bExtended ? cards_at({ 1, 2, 3, 4 }) : cards_at({ 4 })));
		REQUIRE(Fixture.CurrentRow() == 4);

		// K6 - 타입어헤드 검색. 표시 문자열은 카드 본문 전체다.
		{
			C_SELECT_FIXTURE Search(bExtended,
				{ L"alpha", L"bravo", L"charlie", L"delta", L"echo" });
			send_click(Search.Hwnd(), Search.Pt(0));
			send_char(Search.Hwnd(), L'c');
			REQUIRE(Search.CurrentRow() == 2);
			REQUIRE(Search.Selected() == cards_at({ 2 }));
			// 400ms 안의 두 번째 글자는 접두사를 잇는다 - charlie 가 그대로 남는다.
			send_char(Search.Hwnd(), L'h');
			REQUIRE(Search.CurrentRow() == 2);
		}
		{
			C_SELECT_FIXTURE Search(bExtended,
				{ L"alpha", L"avocado", L"bravo", L"charlie", L"delta" });
			// 새 검색은 현재 행 다음 칸부터 찾는다(skipRow) - 행 0 에서 'a' 는 avocado(행 1)다.
			send_click(Search.Hwnd(), Search.Pt(0));
			send_char(Search.Hwnd(), L'a');
			REQUIRE(Search.CurrentRow() == 1);
			// 400ms 안의 같은 글자는 sameKey 라 시작 행만 밀 뿐 찾는 문자열은 "aa"·"aaa" 로
			// 이어진다 - 맞는 행이 없으므로 행 1 에 그대로 머문다(실측 오라클).
			send_char(Search.Hwnd(), L'a');
			REQUIRE(Search.CurrentRow() == 1);
			send_char(Search.Hwnd(), L'a');
			REQUIRE(Search.CurrentRow() == 1);

			// Ctrl 이 눌린 문자는 검색에 실리지 않는다.
			send_char(Search.Hwnd(), L'c', { VK_CONTROL });
			REQUIRE(Search.CurrentRow() == 1);

			// 간격이 지나면 새 검색이고 현재(행 1) 다음 행부터 찾아 alpha(행 0)로 감긴다.
			::Sleep(450);
			send_char(Search.Hwnd(), L'a');
			REQUIRE(Search.CurrentRow() == 0);

			// VK_SPACE 뒤에 오는 WM_CHAR ' ' 도 검색에 실린다 - 맞는 행이 없어 움직이지 않는다.
			::Sleep(450);
			const int nBeforeSpace = Search.CurrentRow();
			send_key(Search.Hwnd(), VK_SPACE);
			send_char(Search.Hwnd(), L' ');
			REQUIRE(Search.CurrentRow() == nBeforeSpace);
		}
	}
}

TEST_CASE("W4 enter opens current card and delete trashes every selected card",
	"[W4-select][WTL-CAP-FI-062]")
{
	C_PAGE_FIXTURE Fixture;
	C_DOCUMENT_PAGE& Page = Fixture.Page();
	Fixture.CreateCards(3);
	const HWND hList = Page.CardListHwnd();
	Page.SetMultiSelectionEnabled(true);

	// (1) Enter 는 현재 카드를 열고 편집기로 포커스를 준다(S1 계약).
	REQUIRE(::SendMessageW(hList, LB_SETCURSEL, 0, 0) == 0);
	const domain::S_CARD* pFirst = Page.CardList().Projection()->CardAt(0);
	REQUIRE(pFirst != nullptr);
	const std::string sFirstId = pFirst->sId;
	::SendMessageW(hList, WM_KEYDOWN, VK_RETURN, 1);
	REQUIRE(Page.HasSession());
	REQUIRE(::GetFocus() == Page.EditorHwnd());

	// (2) 목록으로 돌아가 두 장을 고른다.
	::SetFocus(hList);
	send_click(hList, Fixture.RowPoint(0));
	send_click(hList, Fixture.RowPoint(2), MK_CONTROL);
	const domain::S_CARD* pThird = Page.CardList().Projection()->CardAt(2);
	REQUIRE(pThird != nullptr);
	const std::string sThirdId = pThird->sId;
	REQUIRE(Page.CardList().Projection()->SelectedCardIds().size() == 2);
	REQUIRE(::GetFocus() == hList);

	// (3) Delete 는 선택 전부를 휴지통으로 보내고 소멸한 카드의 세션을 푼다.
	::SendMessageW(hList, WM_KEYDOWN, VK_DELETE, 1);
	REQUIRE(Fixture.CardDeleted(sFirstId));
	REQUIRE(Fixture.CardDeleted(sThirdId));
	REQUIRE(::SendMessageW(hList, LB_GETCOUNT, 0, 0) == 1);
	for (const std::string& sId : Page.CardList().Projection()->SelectedCardIds())
	{
		REQUIRE(Page.CardList().Projection()->RowForCard(sId).has_value());
	}
	REQUIRE_FALSE(Page.HasSession());
	REQUIRE(Fixture.EditorText().empty());
	// 원본 _delete_cards 는 포커스를 옮기지 않는다.
	REQUIRE(::GetFocus() == hList);

	// 단일 모드에서도 Enter 의 W3 관측 계약은 그대로다.
	Page.SetMultiSelectionEnabled(false);
	REQUIRE(::SendMessageW(hList, LB_SETCURSEL, 0, 0) == 0);
	::SendMessageW(hList, WM_KEYDOWN, VK_RETURN, 1);
	REQUIRE(::GetFocus() == Page.EditorHwnd());

	// 선택이 비면 Delete 는 아무것도 하지 않는다("소비하지 않음" 은 seam 없이 관측할 수 없어
	// 단언하지 않는다).
	REQUIRE(::SendMessageW(hList, LB_SETCURSEL, static_cast<WPARAM>(-1), 0) == LB_ERR);
	::SetFocus(hList);
	const LRESULT nCount = ::SendMessageW(hList, LB_GETCOUNT, 0, 0);
	::SendMessageW(hList, WM_KEYDOWN, VK_DELETE, 1);
	REQUIRE(::SendMessageW(hList, LB_GETCOUNT, 0, 0) == nCount);
}

// ---------------------------------------------------------------------------
// 군 F — 목록 포커스 명령과 빈 영역 클릭의 편집기 처분
// ---------------------------------------------------------------------------

TEST_CASE("W4 card list command returns focus or closes the editor session",
	"[W4-select][WTL-CAP-FI-024]")
{
	using Command = shell::E_CARD_LIST_COMMAND;
	REQUIRE(shell::ResolveCardListCommand(false, false) == Command::FocusCardList);
	REQUIRE(shell::ResolveCardListCommand(false, true) == Command::RequestLeave);
	REQUIRE(shell::ResolveCardListCommand(true, false) == Command::FocusCardList);
	REQUIRE(shell::ResolveCardListCommand(true, true) == Command::FocusCardList);

	C_PAGE_FIXTURE Fixture;
	C_DOCUMENT_PAGE& Page = Fixture.Page();
	Page.ShowHistory();
	REQUIRE(Page.IsHistoryVisible());
	Page.FocusCardList();
	REQUIRE_FALSE(Page.IsHistoryVisible());
	REQUIRE(::GetFocus() == Page.CardListHwnd());

	Fixture.Paste(L"w4 s2 focus body");
	REQUIRE(Page.HasSession());
	REQUIRE(Page.Save());
	REQUIRE(Page.RequestLeave() == app::E_LEAVE_RESULT::ApprovedClean);
	REQUIRE_FALSE(Page.HasSession());
	REQUIRE(::GetFocus() == Page.CardListHwnd());
}

TEST_CASE("W4 empty area click closes the editor only for a genuine click and refocuses the editor",
	"[W4-select][WTL-CAP-FI-061]")
{
	C_PAGE_FIXTURE Fixture;
	C_DOCUMENT_PAGE& Page = Fixture.Page();
	Fixture.CreateCards(1);
	const HWND hList = Page.CardListHwnd();

	// (1) 세션이 없으면 닫기 게이트를 돌리지 않고 포커스만 편집면으로 되돌린다.
	REQUIRE_FALSE(Page.HasSession());
	REQUIRE(Page.OnEmptyAreaClicked());
	// 지연 호출이 실제로 돌았는지는 포커스를 목록으로 빼앗은 뒤 펌프해서 본다.
	::SetFocus(hList);
	REQUIRE(::GetFocus() == hList);
	pynote::harness::drain_messages();
	REQUIRE(::GetFocus() == Page.EditorHwnd());

	// (2) 진짜 빈 영역 클릭은 깨끗한 세션을 닫고 편집면으로 포커스를 되돌린다.
	Fixture.Paste(L"w4 s2 empty click body");
	REQUIRE(Page.Save());
	REQUIRE(Page.HasSession());
	::SetFocus(hList);
	send_click(hList, Fixture.EmptyPoint());
	pynote::harness::drain_messages();
	REQUIRE_FALSE(Page.HasSession());
	REQUIRE(Fixture.EditorText().empty());
	REQUIRE(::GetFocus() == Page.EditorHwnd());

	// (3) 수식키가 붙은 빈 영역 클릭은 신호를 내지 않으므로 세션이 그대로 남는다.
	Fixture.Paste(L"w4 s2 kept session body");
	REQUIRE(Page.Save());
	REQUIRE(Page.HasSession());
	::SetFocus(hList);
	send_press(hList, Fixture.EmptyPoint(), MK_CONTROL);
	send_release(hList, Fixture.EmptyPoint(), MK_CONTROL);
	pynote::harness::drain_messages();
	REQUIRE(Page.HasSession());
}

TEST_CASE("W4 drag threshold is manhattan ten dip and a card press past threshold consumes the release",
	"[W4-select][WTL-CAP-TI-024]")
{
	{
		C_SELECT_FIXTURE Fixture;
		const HWND hList = Fixture.Hwnd();

		// 맨해튼 9 - 임계 미달이라 여전히 열린다.
		send_press(hList, Fixture.Pt(0));
		send_move(hList, Fixture.Offset(Fixture.Pt(0), 5, 4));
		REQUIRE(Fixture.State() == E_CARD_LIST_VIEW_STATE::Dragging);
		send_release(hList, Fixture.Offset(Fixture.Pt(0), 5, 4));
		REQUIRE(Fixture.Opened() == cards_at({ 0 }));

		// 맨해튼 10 - Qt 는 "초과" 를 요구하므로 아직 소비되지 않는다.
		Fixture.Reset();
		send_press(hList, Fixture.Pt(0));
		send_move(hList, Fixture.Offset(Fixture.Pt(0), 6, 4));
		send_release(hList, Fixture.Pt(0));
		REQUIRE(Fixture.Opened() == cards_at({ 0 }));

		// 맨해튼 11 - press 가 소비되고 상태가 NoState 로 돌아온다.
		Fixture.Reset();
		send_press(hList, Fixture.Pt(0));
		send_move(hList, Fixture.Offset(Fixture.Pt(0), 7, 4));
		REQUIRE(Fixture.State() == E_CARD_LIST_VIEW_STATE::NoState);
		send_release(hList, Fixture.Pt(0));
		REQUIRE(Fixture.Opened().empty());
		REQUIRE(Fixture.Selected() == cards_at({ 0 }));

		// 다른 행으로 끌어도 열지 않고 선택도 그대로다.
		Fixture.Reset();
		send_press(hList, Fixture.Pt(0));
		send_move(hList, Fixture.Pt(2));
		send_release(hList, Fixture.Pt(2));
		REQUIRE(Fixture.Opened().empty());
		REQUIRE(Fixture.Selected() == cards_at({ 0 }));
	}
	{
		// 확장 모드(= L2 P9): 임계를 넘긴 드래그는 다중 선택을 접지 않는다.
		C_SELECT_FIXTURE Fixture(true);
		const HWND hList = Fixture.Hwnd();
		send_click(hList, Fixture.Pt(1));
		send_click(hList, Fixture.Pt(3), MK_CONTROL);
		const std::vector<std::string> Opened = Fixture.Opened();
		send_press(hList, Fixture.Pt(1));
		send_move(hList, Fixture.Offset(Fixture.Pt(1), 8, 8));
		send_release(hList, Fixture.Pt(1));
		REQUIRE(Fixture.Selected() == cards_at({ 1, 3 }));
		REQUIRE(Fixture.Opened() == Opened);
	}
	{
		// 빈 영역 press 의 비대칭: 원본은 >= 로 재므로 맨해튼 10 이면 이미 clean click 이 아니다.
		C_SELECT_FIXTURE Fixture;
		const HWND hList = Fixture.Hwnd();
		send_press(hList, Fixture.EmptyPoint());
		send_move(hList, Fixture.Offset(Fixture.EmptyPoint(), DRAG_THRESHOLD_DIP, 0));
		send_release(hList, Fixture.EmptyPoint());
		REQUIRE(Fixture.Clicked() == 0);

		Fixture.Reset();
		send_press(hList, Fixture.EmptyPoint());
		send_move(hList, Fixture.Offset(Fixture.EmptyPoint(), DRAG_THRESHOLD_DIP - 1, 0));
		send_release(hList, Fixture.EmptyPoint());
		REQUIRE(Fixture.Clicked() == 1);
	}
}
