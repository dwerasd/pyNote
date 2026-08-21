#include <catch_amalgamated.hpp>

#include "CDocumentListShell.h"
#include "CDocumentPage.h"
#include "CSearchDialog.h"
#include "Resource.h"
#include "pynote/harness/win32_harness.h"

// windows.h 의 CreateEvent 매크로가 repositories.h 의 멤버 이름을 바꾸기 전에 걷는다 -
// 다른 TU(CDocumentPage.cpp·save_coordinator_core_test.cpp)와 같은 순서 계약이어야
// 같은 바이너리 안에서 멤버 이름이 갈리지 않는다.
#ifdef CreateEvent
#undef CreateEvent
#endif

#include "pynote/core/application/card_service.h"
#include "pynote/core/application/draft_coordinator.h"
#include "pynote/core/application/save_coordinator.h"
#include "pynote/core/application/workspace_state.h"
#include "pynote/core/domain/paragraph_parser.h"
#include "pynote/core/storage/migration_runner.h"
#include "pynote/core/storage/repositories.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#pragma comment(lib, "NoteExCore")

namespace
{
	namespace app = pynote::core::application;
	namespace domain = pynote::core::domain;
	namespace shell = pynote::shell;
	namespace storage = pynote::core::storage;
	using pynote::harness::TestWindow;

	class C_PAGE_FIXTURE
	{
	public:
		C_PAGE_FIXTURE()
			: m_Path(std::filesystem::temp_directory_path() /
				("NoteEx-W3-shell-" + std::to_string(::GetCurrentProcessId()) + "-" +
				std::to_string(++s_nSequence) + ".db")), m_Repositories(m_Database),
			  m_DraftStore(m_Database, m_Repositories),
			  m_Parent(pynote::harness::TestWindowOptions{ L"W3 shell page", 960, 480, true })
		{
			this->remove_();
			REQUIRE(m_Database.Open(m_Path.string()));
			storage::C_MIGRATION_RUNNER Runner;
			Runner.SetExistingDatabase(false, m_Path.string());
			REQUIRE(Runner.Run(m_Database) == storage::E_MIGRATE_RESULT::Ok);
			domain::S_DOCUMENT Document;
			Document.sId = DocumentId;
			Document.sTitle = "shell document";
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
			m_hLeft = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
				0, 0, 320, 480, m_Parent.hwnd(), reinterpret_cast<HMENU>(3001),
				::GetModuleHandleW(nullptr), nullptr);
			m_hRight = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
				320, 0, 640, 480, m_Parent.hwnd(), reinterpret_cast<HMENU>(3002),
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

		void RecreatePage()
		{
			m_Page.reset();
			this->create_page_();
		}

		pynote::harness::TestWindow& Parent() { return(m_Parent); }

		void Type(wchar_t _ch)
		{
			::SetFocus(m_Page->EditorHwnd());
			::SendMessageW(m_Page->EditorHwnd(), WM_CHAR, _ch, 1);
		}

		void Paste(const std::wstring& _sText)
		{
			REQUIRE(::OpenClipboard(m_Parent.hwnd()));
			REQUIRE(::EmptyClipboard());
			const SIZE_T nBytes = (_sText.size() + 1) * sizeof(wchar_t);
			HGLOBAL hText = ::GlobalAlloc(GMEM_MOVEABLE, nBytes);
			REQUIRE(hText != nullptr);
			void* pText = ::GlobalLock(hText);
			REQUIRE(pText != nullptr);
			std::memcpy(pText, _sText.c_str(), nBytes);
			::GlobalUnlock(hText);
			REQUIRE(::SetClipboardData(CF_UNICODETEXT, hText) == hText);
			REQUIRE(::CloseClipboard());
			::SetFocus(m_Page->EditorHwnd());
			::SendMessageW(m_Page->EditorHwnd(), WM_PASTE, 0, 0);
		}

		std::vector<domain::S_CARD> Cards()
		{
			std::vector<domain::S_CARD> Cards;
			REQUIRE(m_Repositories.ListCards(DocumentId, &Cards) == storage::E_REPO_RESULT::Ok);
			return(Cards);
		}

		domain::S_CARD Card()
		{
			const auto Values = this->Cards();
			REQUIRE(Values.size() == 1);
			return(Values.front());
		}

		C_DOCUMENT_PAGE& Page() { return(*m_Page); }
		storage::C_DATABASE& Database() { return(m_Database); }
		storage::C_REPOSITORIES& Repositories() { return(m_Repositories); }
		C_DOCUMENT_PAGE::E_LEAVE_CHOICE LeaveChoice{ C_DOCUMENT_PAGE::E_LEAVE_CHOICE::Save };
		inline static const std::string DocumentId = "document-shell";
		inline static const std::string WorkspaceId = "window-shell";

	private:
		void create_page_()
		{
			m_Page = std::make_unique<C_DOCUMENT_PAGE>();
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
		TestWindow m_Parent;
		HWND m_hLeft{};
		HWND m_hRight{};
		std::unique_ptr<C_DOCUMENT_PAGE> m_Page;
		std::int64_t m_nClock{ 2000 };
		std::uint64_t m_nId{};
		inline static std::atomic<unsigned long> s_nSequence{};
	};

	bool menu_contains(HMENU _hMenu, UINT _nCommand, bool* _pbEnabled = nullptr)
	{
		const int nCount = ::GetMenuItemCount(_hMenu);
		for (int nIndex = 0; nIndex < nCount; ++nIndex)
		{
			MENUITEMINFOW Item{};
			Item.cbSize = sizeof(Item);
			Item.fMask = MIIM_ID | MIIM_STATE | MIIM_SUBMENU;
			if (!::GetMenuItemInfoW(_hMenu, static_cast<UINT>(nIndex), TRUE, &Item)) { continue; }
			if (Item.wID == _nCommand)
			{
				if (_pbEnabled) { *_pbEnabled = (Item.fState & (MFS_DISABLED | MFS_GRAYED)) == 0; }
				return(true);
			}
			if (Item.hSubMenu && menu_contains(Item.hSubMenu, _nCommand, _pbEnabled)) { return(true); }
		}
		return(false);
	}

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

	std::wstring window_text(HWND _hWnd)
	{
		std::wstring Text(static_cast<std::size_t>(::GetWindowTextLengthW(_hWnd)) + 1, L'\0');
		Text.resize(static_cast<std::size_t>(::GetWindowTextW(_hWnd, Text.data(), static_cast<int>(Text.size()))));
		return(Text);
	}
}

TEST_CASE("PLAN-W3-0025 actual document page creates opens saves and navigates",
	"[W3-shell-spine][PLAN-W3-0025]")
{
	C_PAGE_FIXTURE Fixture;
	auto& Page = Fixture.Page();
	REQUIRE(::IsWindow(Page.CardListHwnd()));
	REQUIRE(::IsWindow(Page.EditorHwnd()));
	REQUIRE(::IsWindow(Page.FindHwnd()));
	REQUIRE(::IsWindow(Page.ReplaceHwnd()));
	REQUIRE(::IsWindow(Page.HistoryHwnd()));
	REQUIRE(::GetFocus() == Page.EditorHwnd());
	Fixture.Type(L'a');
	REQUIRE(Fixture.Cards().size() == 1);
	REQUIRE(Fixture.Card().sBody == "a");
	Fixture.Type(L'b');
	REQUIRE(Page.HasDirtySession());
	// GetKeyState 는 스레드 키 상태를 본다 - keybd_event(전역 입력 큐 주입)는 펌프
	// 전까지 반영이 보장되지 않고 실제 키보드 상태도 오염한다. SetKeyboardState 로
	// 호출 스레드의 Ctrl 상태를 결정적으로 만든다.
	BYTE PreviousKeys[256]{};
	REQUIRE(::GetKeyboardState(PreviousKeys) != FALSE);
	BYTE ControlKeys[256]{};
	std::memcpy(ControlKeys, PreviousKeys, sizeof(ControlKeys));
	ControlKeys[VK_CONTROL] |= 0x80;
	REQUIRE(::SetKeyboardState(ControlKeys) != FALSE);
	MSG Message{ Page.EditorHwnd(), WM_KEYDOWN, VK_RETURN, 1, 0, { 0, 0 } };
	const bool bTranslated = Page.PreTranslateMessage(&Message);
	REQUIRE(::SetKeyboardState(PreviousKeys) != FALSE);
	REQUIRE(bTranslated);
	Fixture.Type(L'c');
	// 계약은 "Ctrl+Enter = LF 하나 삽입"이다. 편집기 내부 표현(CR)과 무관하게
	// CR/CRLF/LF 를 접어 단락 구분이 정확히 하나임을 센다.
	{
		const std::wstring sEditorText = window_text(Page.EditorHwnd());
		std::size_t nBreaks = 0;
		for (std::size_t nIndex = 0; nIndex < sEditorText.size(); ++nIndex)
		{
			if (sEditorText[nIndex] == L'\r')
			{
				if (nIndex + 1 < sEditorText.size() && sEditorText[nIndex + 1] == L'\n') { ++nIndex; }
				++nBreaks;
			}
			else if (sEditorText[nIndex] == L'\n') { ++nBreaks; }
		}
		REQUIRE(nBreaks == 1);
	}
	REQUIRE(Page.Save());
	REQUIRE_FALSE(Page.HasDirtySession());
	REQUIRE(Fixture.Card().sBody == "ab\nc");
	Page.ShowFind(false);
	REQUIRE(::IsWindowVisible(Page.FindHwnd()));
	REQUIRE_FALSE(::IsWindowVisible(Page.ReplaceHwnd()));
	Page.ShowFind(true);
	REQUIRE(::IsWindowVisible(Page.ReplaceHwnd()));
	Page.ShowHistory();
	REQUIRE(Page.IsHistoryVisible());
	Page.FocusCardList();
	REQUIRE_FALSE(Page.IsHistoryVisible());
	REQUIRE(::GetFocus() == Page.CardListHwnd());
	::SendMessageW(Page.CardListHwnd(), LB_SETCURSEL, 0, 0);
	::SendMessageW(Page.CardListHwnd(), WM_KEYDOWN, VK_RETURN, 1);
	REQUIRE(::GetFocus() == Page.EditorHwnd());
}

TEST_CASE("PLAN-W3-0026 runtime menu accelerators and modeless search are wired",
	"[W3-shell-spine][PLAN-W3-0026]")
{
	HMENU hMenu = shell::CreateRuntimeMenu();
	REQUIRE(hMenu != nullptr);
	bool bEnabled = false;
	REQUIRE(menu_contains(hMenu, IDM_NEW_WINDOW, &bEnabled)); REQUIRE(bEnabled);
	REQUIRE(menu_contains(hMenu, IDM_GLOBAL_SEARCH, &bEnabled)); REQUIRE(bEnabled);
	REQUIRE(menu_contains(hMenu, IDM_SAVE_CARD, &bEnabled)); REQUIRE(bEnabled);
	REQUIRE(menu_contains(hMenu, IDM_FOCUS_MODE, &bEnabled)); REQUIRE(bEnabled);
	REQUIRE(menu_contains(hMenu, IDM_IMPORT_TEXT, &bEnabled)); REQUIRE_FALSE(bEnabled);
	REQUIRE(menu_contains(hMenu, IDM_CREATE_BACKUP, &bEnabled)); REQUIRE_FALSE(bEnabled);
	const auto Table = shell::RuntimeAccelerators();
	REQUIRE(Table.size() == 11);
	REQUIRE(Table[0].cmd == IDM_NEW_WINDOW);
	REQUIRE(Table[2].cmd == IDM_GLOBAL_SEARCH);
	REQUIRE(Table[3].cmd == IDM_SAVE_CARD);
	REQUIRE(Table[8].cmd == IDM_BACK);
	HACCEL hAccelerator = ::CreateAcceleratorTableW(
		const_cast<LPACCEL>(Table.data()), static_cast<int>(Table.size()));
	REQUIRE(hAccelerator != nullptr);
	C_SEARCH_DIALOG Search;
	REQUIRE(Search.Initialize(::GetModuleHandleW(nullptr)));
	TestWindow Owner;
	Search.Show(Owner.hwnd());
	REQUIRE(Search.IsVisible());
	REQUIRE(::IsWindow(Search.QueryHwnd()));
	REQUIRE(::GetFocus() == Search.QueryHwnd());
	::SetWindowTextW(Search.QueryHwnd(), L"검색어");
	Search.Show(Owner.hwnd());
	DWORD nStart = 1, nEnd = 1;
	::SendMessageW(Search.QueryHwnd(), EM_GETSEL,
		reinterpret_cast<WPARAM>(&nStart), reinterpret_cast<LPARAM>(&nEnd));
	REQUIRE(nStart == 0);
	REQUIRE(nEnd == 3);
	// 라우팅 계약: 프레임 소속 메시지만 대상이고, 사전 번역이 액셀러레이터보다 먼저다.
	{
		TestWindow Frame(pynote::harness::TestWindowOptions{ L"route frame", 200, 100, true });
		int nCommands = 0;
		Frame.set_handler([&](UINT _uMessage, WPARAM _wParam, LPARAM, LRESULT& _nResult) {
			if (_uMessage != WM_COMMAND || LOWORD(_wParam) != IDM_BACK) { return(false); }
			++nCommands;
			_nResult = 0;
			return(true);
		});
		MSG Escape{ Frame.hwnd(), WM_KEYDOWN, VK_ESCAPE, 1, 0, { 0, 0 } };
		int nPreTranslated = 0;
		REQUIRE(shell::RouteFrameMessage(&Escape, Frame.hwnd(),
			[&](MSG*) { ++nPreTranslated; return(true); }, hAccelerator));
		REQUIRE(nPreTranslated == 1);
		REQUIRE(nCommands == 0);
		REQUIRE(shell::RouteFrameMessage(&Escape, Frame.hwnd(),
			[&](MSG*) { ++nPreTranslated; return(false); }, hAccelerator));
		REQUIRE(nPreTranslated == 2);
		REQUIRE(nCommands == 1);
		// 소유 모델리스 셸(검색 창) 메시지는 어느 층에도 닿지 않는다.
		MSG Outside{ Search.QueryHwnd(), WM_KEYDOWN, VK_ESCAPE, 1, 0, { 0, 0 } };
		REQUIRE_FALSE(shell::RouteFrameMessage(&Outside, Frame.hwnd(),
			[&](MSG*) { ++nPreTranslated; return(true); }, hAccelerator));
		REQUIRE(nPreTranslated == 2);
		REQUIRE(nCommands == 1);
		Frame.set_handler({});
	}
	Search.Hide();
	REQUIRE_FALSE(Search.IsVisible());
	Search.Destroy();
	::DestroyAcceleratorTable(hAccelerator);
	::DestroyMenu(hMenu);
}

TEST_CASE("PLAN-W3-0040 focus mode hides and restores actual shell windows",
	"[W3-shell-spine][PLAN-W3-0040]")
{
	TestWindow Main(pynote::harness::TestWindowOptions{ L"focus mode", 400, 300, true });
	HMENU hMenu = shell::CreateRuntimeMenu();
	REQUIRE(hMenu != nullptr);
	REQUIRE(::SetMenu(Main.hwnd(), hMenu));
	HWND hStatus = ::CreateWindowExW(0, L"STATIC", L"status", WS_CHILD | WS_VISIBLE,
		0, 0, 100, 20, Main.hwnd(), reinterpret_cast<HMENU>(4001), ::GetModuleHandleW(nullptr), nullptr);
	REQUIRE(hStatus != nullptr);
	C_DOCUMENT_LIST_SHELL DocumentShell;
	REQUIRE(DocumentShell.Initialize(::GetModuleHandleW(nullptr), Main.hwnd()));
	DocumentShell.Show();
	REQUIRE(DocumentShell.IsVisible());
	REQUIRE(shell::ApplyFocusMode(Main.hwnd(), hMenu, hStatus, DocumentShell, true));
	REQUIRE(::GetMenu(Main.hwnd()) == nullptr);
	REQUIRE_FALSE(::IsWindowVisible(hStatus));
	REQUIRE_FALSE(DocumentShell.IsVisible());
	REQUIRE((command_state(hMenu, IDM_FOCUS_MODE) & MFS_CHECKED) != 0);
	REQUIRE(shell::ApplyFocusMode(Main.hwnd(), hMenu, hStatus, DocumentShell, false));
	REQUIRE(::GetMenu(Main.hwnd()) == hMenu);
	REQUIRE(::IsWindowVisible(hStatus));
	REQUIRE(DocumentShell.IsVisible());
	REQUIRE((command_state(hMenu, IDM_FOCUS_MODE) & MFS_CHECKED) == 0);
	DocumentShell.Destroy();
	::SetMenu(Main.hwnd(), nullptr);
	::DestroyMenu(hMenu);
}

TEST_CASE("PLAN-W3-0049 empty page first paste creates one connected paste card",
	"[W3-shell-spine][PLAN-W3-0049]")
{
	C_PAGE_FIXTURE Fixture;
	auto& Page = Fixture.Page();
	REQUIRE(Fixture.Cards().empty());
	REQUIRE(::GetFocus() == Page.EditorHwnd());
	Fixture.Paste(L"첫 붙여넣기");
	const auto Cards = Fixture.Cards();
	REQUIRE(Cards.size() == 1);
	REQUIRE(Cards[0].sBody == "\xec\xb2\xab \xeb\xb6\x99\xec\x97\xac\xeb\x84\xa3\xea\xb8\xb0");
	REQUIRE(Cards[0].eSource == domain::E_CARD_SOURCE::Paste);
	REQUIRE(Cards[0].sDocumentId == C_PAGE_FIXTURE::DocumentId);
	domain::S_CAPTURE_OPERATION Operation;
	REQUIRE(Fixture.Repositories().GetCaptureOperation(Cards[0].sOperationId, &Operation) ==
		storage::E_REPO_RESULT::Ok);
	REQUIRE(Operation.eSource == domain::E_CAPTURE_OPERATION_SOURCE::Paste);
	REQUIRE(Page.HasDirtySession() == false);
	Fixture.Paste(L" 둘째");
	REQUIRE(Fixture.Cards().size() == 1);
	REQUIRE(Page.HasDirtySession());
	REQUIRE(Page.Save());
	REQUIRE(Fixture.Card().sBody == "\xec\xb2\xab \xeb\xb6\x99\xec\x97\xac\xeb\x84\xa3\xea\xb8\xb0 \xeb\x91\x98\xec\xa7\xb8");
}

TEST_CASE("CAP-FI-026 both Back paths keep rejected dirty editor",
	"[W3-shell-spine][WTL-CAP-FI-026]")
{
	C_PAGE_FIXTURE Fixture;
	auto& Page = Fixture.Page();
	REQUIRE(Page.RequestLeave() == app::E_LEAVE_RESULT::ApprovedClean);
	REQUIRE(::GetFocus() == Page.CardListHwnd());
	Fixture.Type(L'x');
	Fixture.Type(L'y');
	REQUIRE(Page.HasDirtySession());
	Fixture.LeaveChoice = C_DOCUMENT_PAGE::E_LEAVE_CHOICE::Cancel;
	REQUIRE(Page.RequestLeave() == app::E_LEAVE_RESULT::Denied);
	REQUIRE(Page.HasDirtySession());
	REQUIRE(::GetFocus() == Page.EditorHwnd());
	// Back 의 정본 경로는 액셀러레이터(IDM_BACK)다 - 페이지가 Esc/Alt+Left 를 소비하면
	// 거부된 leave 가 액셀러레이터로 흘러 프롬프트가 두 번 뜬다. 페이지 미소비를 먼저
	// 고정하고, 두 Back 키 전부 실제 테이블로 IDM_BACK 에 결속됨을 구동으로 확인한다.
	MSG Escape{ Page.EditorHwnd(), WM_KEYDOWN, VK_ESCAPE, 1, 0, { 0, 0 } };
	REQUIRE_FALSE(Page.PreTranslateMessage(&Escape));
	MSG AltLeft{ Page.EditorHwnd(), WM_SYSKEYDOWN, VK_LEFT, 0x20000001, 0, { 0, 0 } };
	REQUIRE_FALSE(Page.PreTranslateMessage(&AltLeft));
	// 핸들러 탈출 예외는 하네스가 삼키므로(win32_harness 계약) 결과를 기록해 밖에서 단언한다.
	std::vector<app::E_LEAVE_RESULT> BackResults;
	Fixture.Parent().set_handler([&](UINT _uMessage, WPARAM _wParam, LPARAM, LRESULT& _nResult) {
		if (_uMessage != WM_COMMAND || LOWORD(_wParam) != IDM_BACK) { return(false); }
		BackResults.push_back(Page.RequestLeave());
		_nResult = 0;
		return(true);
	});
	const auto BackTable = shell::RuntimeAccelerators();
	HACCEL hBackAccelerator = ::CreateAcceleratorTableW(
		const_cast<LPACCEL>(BackTable.data()), static_cast<int>(BackTable.size()));
	REQUIRE(hBackAccelerator != nullptr);
	MSG EscapeKey{ Fixture.Parent().hwnd(), WM_KEYDOWN, VK_ESCAPE, 1, 0, { 0, 0 } };
	REQUIRE(::TranslateAcceleratorW(Fixture.Parent().hwnd(), hBackAccelerator, &EscapeKey));
	// FALT 매칭은 번역 시점의 스레드 키 상태를 본다 - SetKeyboardState 로 결정화한다.
	BYTE PreviousKeys[256]{};
	REQUIRE(::GetKeyboardState(PreviousKeys) != FALSE);
	BYTE MenuKeys[256]{};
	std::memcpy(MenuKeys, PreviousKeys, sizeof(MenuKeys));
	MenuKeys[VK_MENU] |= 0x80;
	REQUIRE(::SetKeyboardState(MenuKeys) != FALSE);
	MSG AltLeftKey{ Fixture.Parent().hwnd(), WM_SYSKEYDOWN, VK_LEFT, 0x20000001, 0, { 0, 0 } };
	const BOOL bAltLeftTranslated =
		::TranslateAcceleratorW(Fixture.Parent().hwnd(), hBackAccelerator, &AltLeftKey);
	REQUIRE(::SetKeyboardState(PreviousKeys) != FALSE);
	REQUIRE(bAltLeftTranslated);
	REQUIRE(BackResults ==
		std::vector<app::E_LEAVE_RESULT>{ app::E_LEAVE_RESULT::Denied, app::E_LEAVE_RESULT::Denied });
	::DestroyAcceleratorTable(hBackAccelerator);
	Fixture.Parent().set_handler({});
	REQUIRE(Page.HasDirtySession());
	::SetFocus(Page.EditorHwnd());
	REQUIRE(::GetFocus() == Page.EditorHwnd());
	Fixture.LeaveChoice = C_DOCUMENT_PAGE::E_LEAVE_CHOICE::Save;
	REQUIRE(Page.RequestLeave() == app::E_LEAVE_RESULT::ApprovedAfterSave);
	REQUIRE_FALSE(Page.HasDirtySession());
	REQUIRE(::GetFocus() == Page.CardListHwnd());
	REQUIRE(Fixture.Card().sBody == "xy");
}

TEST_CASE("CAP-FI-027 F11 command preserves shell lifetime across reactivation",
	"[W3-shell-spine][WTL-CAP-FI-027]")
{
	TestWindow Main(pynote::harness::TestWindowOptions{ L"focus accelerator", 400, 300, true });
	HMENU hMenu = shell::CreateRuntimeMenu();
	REQUIRE(hMenu != nullptr);
	REQUIRE(::SetMenu(Main.hwnd(), hMenu));
	HWND hStatus = ::CreateWindowExW(0, L"STATIC", L"status", WS_CHILD | WS_VISIBLE,
		0, 0, 100, 20, Main.hwnd(), reinterpret_cast<HMENU>(4002), ::GetModuleHandleW(nullptr), nullptr);
	C_DOCUMENT_LIST_SHELL DocumentShell;
	REQUIRE(DocumentShell.Initialize(::GetModuleHandleW(nullptr), Main.hwnd()));
	DocumentShell.Show();
	const HWND hShell = DocumentShell.Hwnd();
	bool bFocus = false;
	Main.set_handler([&](UINT _uMessage, WPARAM _wParam, LPARAM, LRESULT& _nResult) {
		if (_uMessage != WM_COMMAND || LOWORD(_wParam) != IDM_FOCUS_MODE) { return(false); }
		bFocus = !bFocus;
		REQUIRE(shell::ApplyFocusMode(Main.hwnd(), hMenu, hStatus, DocumentShell, bFocus));
		_nResult = 0;
		return(true);
	});
	const auto Table = shell::RuntimeAccelerators();
	HACCEL hAccelerator = ::CreateAcceleratorTableW(
		const_cast<LPACCEL>(Table.data()), static_cast<int>(Table.size()));
	REQUIRE(hAccelerator != nullptr);
	MSG F11{ Main.hwnd(), WM_KEYDOWN, VK_F11, 1, 0, { 0, 0 } };
	REQUIRE(::TranslateAcceleratorW(Main.hwnd(), hAccelerator, &F11));
	REQUIRE(bFocus);
	REQUIRE_FALSE(DocumentShell.IsVisible());
	REQUIRE(::IsWindow(hShell));
	REQUIRE(::TranslateAcceleratorW(Main.hwnd(), hAccelerator, &F11));
	REQUIRE_FALSE(bFocus);
	REQUIRE(DocumentShell.IsVisible());
	REQUIRE(DocumentShell.Hwnd() == hShell);
	::SetActiveWindow(Main.hwnd());
	REQUIRE(DocumentShell.IsVisible());
	REQUIRE((command_state(hMenu, IDM_FOCUS_MODE) & MFS_CHECKED) == 0);
	DocumentShell.Destroy();
	::DestroyAcceleratorTable(hAccelerator);
	::SetMenu(Main.hwnd(), nullptr);
	::DestroyMenu(hMenu);
}

TEST_CASE("CAP-FI-035 structured page state survives actual reopen",
	"[W3-shell-spine][WTL-CAP-FI-035]")
{
	C_PAGE_FIXTURE Fixture;
	auto& Page = Fixture.Page();
	Fixture.Paste(L"workspace body");
	Fixture.Type(L'!');
	REQUIRE(Page.HasDirtySession());
	REQUIRE(Page.Save());
	const auto Card = Fixture.Card();
	REQUIRE(Card.sCurrentRevisionId.has_value());
	REQUIRE(Page.PersistState(std::pair(321, 643)));
	app::C_WORKSPACE_STATE_STORE Store(
		Fixture.Database(), Fixture.Repositories(), C_PAGE_FIXTURE::WorkspaceId);
	app::S_DOCUMENT_UI_STATE State;
	REQUIRE(Store.LoadDocumentUiState(C_PAGE_FIXTURE::DocumentId, &State) == storage::E_REPO_RESULT::Ok);
	REQUIRE(State.sSelectedCardId == Card.sId);
	REQUIRE(State.sEditorCardId == Card.sId);
	REQUIRE(State.sEditorBaseRevisionId == Card.sCurrentRevisionId);
	REQUIRE(State.nEditorCursorQchar.has_value());
	REQUIRE(State.EditorSplitSizes == std::optional(std::pair<std::int64_t, std::int64_t>(321, 643)));
	REQUIRE(State.eSortMode == domain::E_CARD_LIST_SORT_MODE::Recency);
	Fixture.RecreatePage();
	auto& Reopened = Fixture.Page();
	REQUIRE(::GetFocus() == Reopened.EditorHwnd());
	REQUIRE(window_text(Reopened.EditorHwnd()) == L"workspace body!");
	REQUIRE_FALSE(Reopened.HasDirtySession());
	REQUIRE(::SendMessageW(Reopened.CardListHwnd(), LB_GETCURSEL, 0, 0) == 0);
	REQUIRE(Reopened.PersistState(std::pair(321, 643)));
}
