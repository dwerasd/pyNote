#include <catch_amalgamated.hpp>

#include "CWindowLayout.h"
#include "pynote/core/application/workspace_state.h"
#include "pynote/core/storage/database.h"
#include "pynote/core/storage/migration_runner.h"
#include "pynote/core/storage/repositories.h"
#include "pynote/platform/win32_device_settings.h"
#include "pynote/platform/win32_file_system.h"
#include "pynote/harness/win32_harness.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "NoteExCore")

namespace
{
	namespace app = pynote::core::application;
	namespace domain = pynote::core::domain;
	namespace shell = pynote::shell;
	namespace storage = pynote::core::storage;
	using pynote::harness::TestWindow;

	class C_THREAD_DPI_CONTEXT
	{
	public:
		C_THREAD_DPI_CONTEXT()
			: m_Previous(::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
		~C_THREAD_DPI_CONTEXT()
		{
			if (m_Previous) { ::SetThreadDpiAwarenessContext(m_Previous); }
		}
	private:
		DPI_AWARENESS_CONTEXT m_Previous{ nullptr };
	};

	class C_SPLITTER_FIXTURE
	{
	public:
		explicit C_SPLITTER_FIXTURE(int _nWidth = 964, int _nHeight = 640)
		{
			RECT Frame{ 0, 0, _nWidth, _nHeight };
			REQUIRE(nullptr != Splitter.Create(Parent.hwnd(), Frame, nullptr,
				WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS));
			Left = ::CreateWindowExW(0, L"STATIC", L"left", WS_CHILD | WS_VISIBLE,
				0, 0, 1, 1, Splitter, reinterpret_cast<HMENU>(1), ::GetModuleHandleW(nullptr), nullptr);
			Right = ::CreateWindowExW(0, L"STATIC", L"right", WS_CHILD | WS_VISIBLE,
				0, 0, 1, 1, Splitter, reinterpret_cast<HMENU>(2), ::GetModuleHandleW(nullptr), nullptr);
			REQUIRE(Left != nullptr);
			REQUIRE(Right != nullptr);
			Splitter.SetSplitterPanes(Left, Right, false);
			Splitter.SetDpi(USER_DEFAULT_SCREEN_DPI, false);
			Splitter.MoveWindow(&Frame, TRUE);
		}

		~C_SPLITTER_FIXTURE()
		{
			if (Splitter.IsWindow()) { Splitter.DestroyWindow(); }
		}

		std::pair<int, int> PaneWidths() const
		{
			RECT LeftFrame{};
			RECT RightFrame{};
			REQUIRE(::GetWindowRect(Left, &LeftFrame));
			REQUIRE(::GetWindowRect(Right, &RightFrame));
			return(std::pair<int, int>{
				static_cast<int>(LeftFrame.right - LeftFrame.left),
				static_cast<int>(RightFrame.right - RightFrame.left) });
		}

		TestWindow Parent;
		shell::C_WINDOW_SPLITTER Splitter;
		HWND Left{ nullptr };
		HWND Right{ nullptr };
	};

	class C_DATABASE_FIXTURE
	{
	public:
		C_DATABASE_FIXTURE()
			: m_Path(std::filesystem::temp_directory_path() /
				("NoteEx-W3-layout-" + std::to_string(::GetCurrentProcessId()) + "-" +
				std::to_string(++s_nSequence) + ".db"))
		{
			this->Remove();
			this->Open(false);
		}

		~C_DATABASE_FIXTURE()
		{
			m_Repositories.reset();
			m_Database.Close();
			this->Remove();
		}

		void AddDocument(const std::string& _sId)
		{
			domain::S_DOCUMENT Document;
			Document.sId = _sId;
			Document.sTitle = _sId;
			Document.nCreatedAtUs = 100;
			Document.nUpdatedAtUs = 100;
			REQUIRE(m_Repositories->CreateDocument(Document) == storage::E_REPO_RESULT::Ok);
		}

		app::C_WORKSPACE_STATE_STORE Store(const std::string& _sWindowId)
		{
			return(app::C_WORKSPACE_STATE_STORE(m_Database, *m_Repositories, _sWindowId));
		}

		void Reopen()
		{
			m_Repositories.reset();
			m_Database.Close();
			this->Open(true);
		}

	private:
		void Open(bool _bExisting)
		{
			REQUIRE(m_Database.Open(m_Path.string()));
			storage::C_MIGRATION_RUNNER Runner;
			Runner.SetExistingDatabase(_bExisting, m_Path.string());
			REQUIRE(Runner.Run(m_Database) == storage::E_MIGRATE_RESULT::Ok);
			m_Repositories = std::make_unique<storage::C_REPOSITORIES>(m_Database);
		}

		void Remove() const
		{
			std::error_code Error;
			std::filesystem::remove(m_Path, Error);
			std::filesystem::remove(m_Path.string() + "-wal", Error);
			std::filesystem::remove(m_Path.string() + "-shm", Error);
		}

		std::filesystem::path m_Path;
		storage::C_DATABASE m_Database;
		std::unique_ptr<storage::C_REPOSITORIES> m_Repositories;
		inline static std::atomic<unsigned long> s_nSequence{ 0 };
	};

	class C_TEMP_LOCAL_APP_DATA
	{
	public:
		C_TEMP_LOCAL_APP_DATA()
		{
			wchar_t Temp[32768]{};
			const DWORD nLength = ::GetTempPathW(_countof(Temp), Temp);
			m_Root = std::filesystem::path(std::wstring(Temp, nLength)) /
				(L"NoteEx-W3-layout-settings-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
				std::to_wstring(++s_nSequence));
			std::filesystem::create_directories(m_Root);
			const DWORD nRequired = ::GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
			if (nRequired)
			{
				m_sPrevious.resize(nRequired);
				m_sPrevious.resize(::GetEnvironmentVariableW(
					L"LOCALAPPDATA", m_sPrevious.data(), nRequired));
				m_bHadPrevious = true;
			}
			REQUIRE(::SetEnvironmentVariableW(L"LOCALAPPDATA", m_Root.c_str()));
		}

		~C_TEMP_LOCAL_APP_DATA()
		{
			::SetEnvironmentVariableW(L"LOCALAPPDATA", m_bHadPrevious ? m_sPrevious.c_str() : nullptr);
			std::error_code Error;
			std::filesystem::remove_all(m_Root, Error);
		}

	private:
		std::filesystem::path m_Root;
		std::wstring m_sPrevious;
		bool m_bHadPrevious{ false };
		inline static std::atomic<unsigned long> s_nSequence{ 0 };
	};

	app::S_DOCUMENT_UI_STATE split_state(
		const std::string& _sDocumentId, std::pair<int, int> _Sizes, std::int64_t _nUpdated)
	{
		app::S_DOCUMENT_UI_STATE State;
		State.sDocumentId = _sDocumentId;
		State.EditorSplitSizes = _Sizes;
		State.nUpdatedAtUs = _nUpdated;
		return(State);
	}
}

TEST_CASE("PLAN-W3-0016 permanent left/right WTL hosts keep initial one-to-two order",
	"[W3-geometry-layout][PLAN-W3-0016][WTL-CAP-FI-099]")
{
	C_THREAD_DPI_CONTEXT DpiContext;
	C_SPLITTER_FIXTURE Fixture;
	Fixture.Splitter.SetSplitSizesDip(std::nullopt);
	const auto Initial = Fixture.PaneWidths();
	const HWND hEditor = Fixture.Right;
	REQUIRE(Fixture.Splitter.GetSplitterPane(SPLIT_PANE_LEFT) == Fixture.Left);
	REQUIRE(Fixture.Splitter.GetSplitterPane(SPLIT_PANE_RIGHT) == Fixture.Right);
	REQUIRE(Initial.first > 0);
	REQUIRE(Initial.second > Initial.first);
	REQUIRE(std::abs((Initial.first * 2) - Initial.second) <= 2);
	Fixture.Splitter.SetSplitSizesDip(std::pair(300, 660));
	REQUIRE(Fixture.Right == hEditor);
}

TEST_CASE("PLAN-W3-0017 actual WTL splitter clamps both extremes without collapse",
	"[W3-geometry-layout][PLAN-W3-0017][WTL-CAP-FI-099]")
{
	C_THREAD_DPI_CONTEXT DpiContext;
	C_SPLITTER_FIXTURE Fixture;
	Fixture.Splitter.SetSplitterPos(1, true);
	const auto LeftExtreme = Fixture.PaneWidths();
	REQUIRE(LeftExtreme.first == shell::SPLITTER_LEFT_MIN_DIP);
	REQUIRE(LeftExtreme.second >= shell::SPLITTER_RIGHT_MIN_DIP);
	Fixture.Splitter.SetSplitterPos(100000, true);
	const auto RightExtreme = Fixture.PaneWidths();
	REQUIRE(RightExtreme.first >= shell::SPLITTER_LEFT_MIN_DIP);
	REQUIRE(RightExtreme.second == shell::SPLITTER_RIGHT_MIN_DIP);
	RECT Client{};
	REQUIRE(::GetClientRect(Fixture.Splitter, &Client));
	REQUIRE(LeftExtreme.first + LeftExtreme.second + shell::SPLITTER_HANDLE_DIP == Client.right);
	REQUIRE(RightExtreme.first + RightExtreme.second + shell::SPLITTER_HANDLE_DIP == Client.right);
	const auto Narrow = shell::ClampPaneSizesDip(0, 320);
	REQUIRE(Narrow.first > 0);
	REQUIRE(Narrow.second > 0);
	REQUIRE(Narrow.first + Narrow.second == 320);
}

TEST_CASE("PLAN-W3-0018 document split saves before switch and reopens exactly",
	"[W3-geometry-layout][PLAN-W3-0018][WTL-CAP-PL-006]")
{
	C_DATABASE_FIXTURE Database;
	Database.AddDocument("document-a");
	Database.AddDocument("document-b");
	auto Store = Database.Store("window-a");
	REQUIRE(Store.SaveDocumentUiState(split_state("document-a", { 312, 648 }, 101)) ==
		storage::E_REPO_RESULT::Ok);
	REQUIRE(Store.SaveDocumentUiState(split_state("document-b", { 401, 559 }, 102)) ==
		storage::E_REPO_RESULT::Ok);
	Database.Reopen();
	auto Reopened = Database.Store("window-a");
	app::S_DOCUMENT_UI_STATE A;
	app::S_DOCUMENT_UI_STATE B;
	REQUIRE(Reopened.LoadDocumentUiState("document-a", &A) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Reopened.LoadDocumentUiState("document-b", &B) == storage::E_REPO_RESULT::Ok);
	REQUIRE(A.EditorSplitSizes == std::optional(std::pair<std::int64_t, std::int64_t>(312, 648)));
	REQUIRE(B.EditorSplitSizes == std::optional(std::pair<std::int64_t, std::int64_t>(401, 559)));
}

TEST_CASE("PLAN-W3-0019 pending apply and five 96 144 192 DPI cycles do not drift",
	"[W3-geometry-layout][PLAN-W3-0019][WTL-CAP-RE-018]")
{
	C_THREAD_DPI_CONTEXT DpiContext;
	C_SPLITTER_FIXTURE Fixture(1, 640);
	Fixture.Splitter.SetSplitSizesDip(std::pair(320, 640));
	for (int nCycle = 0; nCycle < 5; ++nCycle)
	{
		for (const UINT nDpi : { 96u, 144u, 192u })
		{
			Fixture.Splitter.SetDpi(nDpi, false);
			RECT Frame{ 0, 0, shell::DipsToPixels(964, nDpi), shell::DipsToPixels(640, nDpi) };
			Fixture.Splitter.MoveWindow(&Frame, TRUE);
			const int nPosition = Fixture.Splitter.GetSplitterPos();
			Fixture.Splitter.SetSplitterPos(nPosition, true);
			REQUIRE(Fixture.Splitter.SplitSizesDip() == std::optional(std::pair(320, 640)));
		}
	}
	RECT Suggested{ 20, 30, 20 + shell::DipsToPixels(964, 144), 30 + shell::DipsToPixels(640, 144) };
	Fixture.Parent.set_handler([&](UINT _nMessage, WPARAM _wParam, LPARAM _lParam, LRESULT& _nResult) {
		if (_nMessage != WM_DPICHANGED) { return(false); }
		Fixture.Splitter.SetDpi(HIWORD(_wParam), false);
		const auto* pFrame = reinterpret_cast<const RECT*>(_lParam);
		::SetWindowPos(Fixture.Parent.hwnd(), nullptr, pFrame->left, pFrame->top,
			pFrame->right - pFrame->left, pFrame->bottom - pFrame->top,
			SWP_NOACTIVATE | SWP_NOZORDER);
		_nResult = 0;
		return(true);
	});
	pynote::harness::send_message(Fixture.Parent.hwnd(), WM_DPICHANGED,
		MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(&Suggested));
	REQUIRE(Fixture.Splitter.Dpi() == 144);
}

TEST_CASE("PLAN-W3-0027 maximized HWND reset is normal 960 by 640 DIP and centered",
	"[W3-geometry-layout][PLAN-W3-0027][WTL-CAP-FI-030]")
{
	C_THREAD_DPI_CONTEXT DpiContext;
	TestWindow Window;
	::ShowWindow(Window.hwnd(), SW_MAXIMIZE);
	REQUIRE(shell::ResetWindowGeometry(Window.hwnd()));
	REQUIRE_FALSE(::IsZoomed(Window.hwnd()));
	const UINT nDpi = ::GetDpiForWindow(Window.hwnd());
	RECT Client{};
	RECT Frame{};
	RECT Work{};
	REQUIRE(::GetClientRect(Window.hwnd(), &Client));
	REQUIRE(::GetWindowRect(Window.hwnd(), &Frame));
	REQUIRE(shell::GetMonitorWorkAreaForWindow(Window.hwnd(), &Work));
	REQUIRE(std::abs(shell::PixelsToDips(Client.right, nDpi) - shell::DEFAULT_CLIENT_WIDTH_DIP) <= 1);
	REQUIRE(std::abs(shell::PixelsToDips(Client.bottom, nDpi) - shell::DEFAULT_CLIENT_HEIGHT_DIP) <= 1);
	REQUIRE(std::abs((Frame.left + Frame.right) - (Work.left + Work.right)) <= 2);
	REQUIRE(std::abs((Frame.top + Frame.bottom) - (Work.top + Work.bottom)) <= 2);
	shell::S_WINDOW_GEOMETRY Geometry;
	REQUIRE(shell::CaptureWindowGeometry(Window.hwnd(), &Geometry));
	REQUIRE_FALSE(Geometry.bMaximized);
}

TEST_CASE("PLAN-W3-0028 monitor admission accepts one pixel and rejects wholly offscreen",
	"[W3-geometry-layout][PLAN-W3-0028][WTL-CAP-FI-034][WTL-CAP-NC-040]")
{
	const std::vector<RECT> Positive{ RECT{ 0, 0, 1920, 1040 }, RECT{ 1920, 0, 3840, 1040 } };
	const std::vector<RECT> Negative{ RECT{ -1600, -900, 0, 0 }, RECT{ 0, 0, 1920, 1040 } };
	REQUIRE(shell::IntersectsMonitorWorkArea(RECT{ 1919, 100, 2200, 500 }, Positive));
	REQUIRE(shell::IntersectsMonitorWorkArea(RECT{ -1, -1, 100, 100 }, Negative));
	REQUIRE(shell::IntersectsMonitorWorkArea(RECT{ -1600, -900, -1599, -899 }, Negative));
	REQUIRE_FALSE(shell::IntersectsMonitorWorkArea(RECT{ 5000, 5000, 5600, 5400 }, Positive));
	REQUIRE_FALSE(shell::IntersectsMonitorWorkArea(RECT{ -3000, -2000, -2000, -1000 }, Negative));
}

TEST_CASE("PLAN-W3-0038 two geometry keys and two DB document splits stay independent",
	"[W3-geometry-layout][PLAN-W3-0038][WTL-CAP-PL-006]")
{
	C_TEMP_LOCAL_APP_DATA Local;
	pynote::platform::C_WIN32_FILE_SYSTEM FileSystem;
	pynote::platform::C_WIN32_DEVICE_SETTINGS Settings(
		FileSystem, L"Software\\pyNote\\W3LayoutMissingRegistry");
	REQUIRE(Settings.Initialize());
	const auto A = shell::EncodeWindowGeometry({ 10, 20, 900, 600, 96, false });
	const auto B = shell::EncodeWindowGeometry({ 2100, -400, 1000, 700, 144, true });
	REQUIRE(Settings.SetBytes(shell::WindowGeometryKey("window-a"), A));
	REQUIRE(Settings.SetBytes(shell::WindowGeometryKey("window-b"), B));
	REQUIRE(Settings.Sync());
	std::vector<std::uint8_t> ReadA;
	std::vector<std::uint8_t> ReadB;
	REQUIRE(Settings.GetBytes(shell::WindowGeometryKey("window-a"), &ReadA));
	REQUIRE(Settings.GetBytes(shell::WindowGeometryKey("window-b"), &ReadB));
	REQUIRE(ReadA == A);
	REQUIRE(ReadB == B);

	C_DATABASE_FIXTURE Database;
	Database.AddDocument("document-a");
	Database.AddDocument("document-b");
	auto Store = Database.Store("window-a");
	REQUIRE(Store.SaveDocumentUiState(split_state("document-a", { 280, 680 }, 1)) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Store.SaveDocumentUiState(split_state("document-b", { 440, 520 }, 2)) == storage::E_REPO_RESULT::Ok);
	app::S_DOCUMENT_UI_STATE StateA;
	app::S_DOCUMENT_UI_STATE StateB;
	REQUIRE(Store.LoadDocumentUiState("document-a", &StateA) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Store.LoadDocumentUiState("document-b", &StateB) == storage::E_REPO_RESULT::Ok);
	REQUIRE(StateA.EditorSplitSizes != StateB.EditorSplitSizes);
}

TEST_CASE("PLAN-W3-0048 legacy bytes migrate once target wins and Qt blob falls back safely",
	"[W3-geometry-layout][PLAN-W3-0048][WTL-CAP-PL-007]")
{
	C_TEMP_LOCAL_APP_DATA Local;
	pynote::platform::C_WIN32_FILE_SYSTEM FileSystem;
	pynote::platform::C_WIN32_DEVICE_SETTINGS Settings(
		FileSystem, L"Software\\pyNote\\W3LayoutMissingRegistry");
	REQUIRE(Settings.Initialize());
	const auto Legacy = shell::EncodeWindowGeometry({ -120, 80, 960, 640, 96, false });
	const auto Existing = shell::EncodeWindowGeometry({ 40, 50, 800, 500, 144, true });
	const std::string First = shell::WindowGeometryKey("first");
	REQUIRE(Settings.SetBytes("window/geometry", Legacy));
	REQUIRE(Settings.Sync());
	bool bMigrated = false;
	REQUIRE(Settings.MigrateBytes("window/geometry", First, &bMigrated));
	REQUIRE(bMigrated);
	REQUIRE_FALSE(Settings.Contains("window/geometry"));
	std::vector<std::uint8_t> Value;
	REQUIRE(Settings.GetBytes(First, &Value));
	REQUIRE(Value == Legacy);

	REQUIRE(Settings.SetBytes("window/geometry", Legacy));
	REQUIRE(Settings.SetBytes(First, Existing));
	REQUIRE(Settings.Sync());
	REQUIRE(Settings.MigrateBytes("window/geometry", First, &bMigrated));
	REQUIRE_FALSE(bMigrated);
	REQUIRE_FALSE(Settings.Contains("window/geometry"));
	REQUIRE(Settings.GetBytes(First, &Value));
	REQUIRE(Value == Existing);

	const std::vector<std::uint8_t> QtBlob{ 0x01, 0xd9, 0xd0, 0xcb, 0x00, 0x03 };
	REQUIRE(Settings.SetBytes(shell::WindowGeometryKey("qt"), QtBlob));
	REQUIRE(Settings.Sync());
	shell::S_WINDOW_GEOMETRY Geometry;
	REQUIRE_FALSE(shell::DecodeWindowGeometry(QtBlob, &Geometry));
	REQUIRE(Settings.GetBytes(shell::WindowGeometryKey("qt"), &Value));
	REQUIRE(Value == QtBlob);
}
