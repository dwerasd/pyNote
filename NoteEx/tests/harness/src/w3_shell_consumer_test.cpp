#include <catch_amalgamated.hpp>

#include "CDocumentListShell.h"
#include "CDocumentPage.h"
#include "CSearchDialog.h"
#include "Resource.h"
#include "pynote/core/application/card_service.h"
#include "pynote/core/application/draft_coordinator.h"
#include "pynote/core/application/save_coordinator.h"
#include "pynote/core/application/workspace_state.h"
#include "pynote/core/domain/paragraph_parser.h"
#include "pynote/core/storage/migration_runner.h"
#include "pynote/core/storage/repositories.h"
#include "pynote/harness/win32_harness.h"

#ifdef CreateEvent
#undef CreateEvent
#endif

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
	::keybd_event(VK_CONTROL, 0, 0, 0);
	MSG Message{ Page.EditorHwnd(), WM_KEYDOWN, VK_RETURN, 1, 0, { 0, 0 } };
	REQUIRE(Page.PreTranslateMessage(&Message));
	::keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
	Fixture.Type(L'c');
	REQUIRE(window_text(Page.EditorHwnd()).find(L'\n') != std::wstring::npos ||
		window_text(Page.EditorHwnd()).find(L'\r') != std::wstring::npos);
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
	MSG Escape{ Page.EditorHwnd(), WM_KEYDOWN, VK_ESCAPE, 1, 0, { 0, 0 } };
	REQUIRE_FALSE(Page.PreTranslateMessage(&Escape));
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
