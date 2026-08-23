#include <catch_amalgamated.hpp>

#include "CCardList.h"
#include "CDocumentPage.h"
#include "Resource.h"
#include "pynote/harness/win32_harness.h"

// windows.h 의 CreateEvent 매크로가 repositories.h 의 멤버 이름을 바꾸기 전에 걷는다 -
// CDocumentPage.cpp·w4_card_list_test.cpp·w4_card_select_test.cpp 와 같은 순서 계약이어야
// 같은 바이너리 안에서 멤버 이름이 갈리지 않는다. ATL/WTL(CCardList.h)은 이 #undef 앞에서
// 읽어야 자기 ::CreateEvent 호출이 식별자를 잃지 않는다.
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

#include <D2DWrapp/D2DBrushCache.h>
#include <D2DWrapp/D2DDevice.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
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
	namespace storage = pynote::core::storage;
	using pynote::harness::TestWindow;

	// 시험은 폰트를 명시 주입해 측정을 결정적으로 만든다(시스템 메시지 폰트 의존 제거).
	constexpr wchar_t TEST_FONT_FAMILY[] = L"Segoe UI";
	constexpr float TEST_FONT_SIZE_DIP = 12.0f;
	// Qt 표준 휠 한 틱의 각도. 제품 상수를 가져다 쓰면 상수를 바꿔도 시험 입력이 함께 바뀌어
	// "틱당 한 장" 계약을 스스로 확인하지 못한다(원본 시험의 WHEEL_TICK_ANGLE 과 같은 이유다).
	constexpr int WHEEL_TICK_ANGLE = 120;
	// 정숙 열기 지연(120ms)의 세 배. "이 안에 열리지 않는다" 를 보는 부정 관측 구간이다
	// (원본 qtbot.wait(BROWSE_OPEN_DELAY_MS * 3)).
	constexpr int QUIET_WINDOW_MS = 360;
	// 굴림 도중을 흉내 내는 이벤트 간격. 지연보다 짧아야 대기가 계속 미뤄진다.
	constexpr int BURST_INTERVAL_MS = 40;
	// 열림을 기다리는 상한(원본 qtbot.waitUntil(..., timeout=2_000)).
	constexpr int OPEN_TIMEOUT_MS = 2000;

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

	std::wstring to_wide(const std::string& _sValue)
	{
		if (_sValue.empty()) { return(std::wstring{}); }
		const int nRequired = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			_sValue.data(), static_cast<int>(_sValue.size()), nullptr, 0);
		if (nRequired <= 0) { throw std::runtime_error("MultiByteToWideChar size query failed"); }
		std::wstring Result(static_cast<std::size_t>(nRequired), L'\0');
		if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, _sValue.data(),
			static_cast<int>(_sValue.size()), Result.data(), nRequired) != nRequired)
		{
			throw std::runtime_error("MultiByteToWideChar conversion failed");
		}
		return(Result);
	}

	// w4_card_list_test.cpp·w4_card_select_test.cpp 의 같은 이름 헬퍼와 같은 카드다
	// (두 TU 모두 무수정 계약이라 복제한다).
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

	// 수식키는 호출 스레드의 키 상태 표로 건다 - GetKeyState 가 읽는 바로 그 표이며 전경
	// 권한도, 사용자 데스크톱 부작용도 없다(keybd_event/SendInput 을 쓰지 않는 이유다).
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

	// 휠 메시지의 lParam 은 화면 좌표다(클라이언트 좌표가 아니다).
	LPARAM screen_centre_lparam(HWND _hWnd)
	{
		RECT Client{};
		REQUIRE(::GetClientRect(_hWnd, &Client) != FALSE);
		POINT Centre{ (Client.left + Client.right) / 2, (Client.top + Client.bottom) / 2 };
		REQUIRE(::ClientToScreen(_hWnd, &Centre) != FALSE);
		return(pack_point(Centre));
	}

	void send_wheel(HWND _hWnd, int _nDelta, WORD _nKeys = 0)
	{
		::SendMessageW(_hWnd, WM_MOUSEWHEEL,
			MAKEWPARAM(_nKeys, static_cast<WORD>(static_cast<short>(_nDelta))),
			screen_centre_lparam(_hWnd));
	}

	void send_hwheel(HWND _hWnd, int _nDelta)
	{
		::SendMessageW(_hWnd, WM_MOUSEHWHEEL,
			MAKEWPARAM(0, static_cast<WORD>(static_cast<short>(_nDelta))),
			screen_centre_lparam(_hWnd));
	}

	void send_press(HWND _hWnd, POINT _Point, WPARAM _nModifiers = 0)
	{
		::SendMessageW(_hWnd, WM_LBUTTONDOWN, MK_LBUTTON | _nModifiers, pack_point(_Point));
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

	// 하네스의 wait_until 은 PeekMessage 루프 + MsgWaitForMultipleObjectsEx(QS_ALLINPUT) 라
	// WM_TIMER 도 배수한다. SendMessage 는 큐에 든 타이머 메시지를 배수하지 않으므로 실
	// 타이머 관측은 반드시 이 두 함수를 거친다.
	bool pump_until(const std::function<bool()>& _Predicate, int _nMilliseconds)
	{
		return(pynote::harness::wait_until(_Predicate, std::chrono::milliseconds(_nMilliseconds)));
	}

	// 부정 관측 구간: 술어가 참이 되지 않으므로 예산을 다 쓰고 돌아온다.
	void pump_for(int _nMilliseconds)
	{
		pynote::harness::wait_until([]() { return(false); },
			std::chrono::milliseconds(_nMilliseconds));
	}

	// 진짜 HWND + 진짜 D2DWrapp + 진짜 프로젝션 위의 컨트롤(S2 C_SELECT_FIXTURE 모양).
	// S2 와 달리 "전 행이 한 화면에 들어간다" 를 요구하지 않는다 - 휠 탐색은 스크롤되는
	// 목록이 주제이기 때문이다.
	class C_WHEEL_FIXTURE
	{
	public:
		static constexpr int VIEW_DIP = 500;

		explicit C_WHEEL_FIXTURE(std::size_t _nCount = 12, bool _bExtended = false)
			: m_Host(pynote::harness::TestWindowOptions{ L"W4 wheel", 700, 700, true })
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
			m_Control.SetBrowseCardHandler([this](const std::string& _sCardId)
				{ m_Browsed.push_back(_sCardId); return(BrowseResult); });
			m_Control.SetEditorCardProvider([this]() { return(EditorCard); });
			RECT Frame{ 0, 0, VIEW_DIP, VIEW_DIP };
			REQUIRE(m_Control.Create(m_Host.hwnd(), Frame, nullptr,
				WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL, 0,
				static_cast<UINT>(IDC_DOCUMENT_CARD_LIST)) != nullptr);
			this->ResizeClient(VIEW_DIP, VIEW_DIP);
			// DPI 비인식 프로세스라 픽셀 == DIP 다 - 좌표 계약을 한 번 못박는다.
			REQUIRE(static_cast<int>(::GetDpiForWindow(m_Control.m_hWnd)) ==
				static_cast<int>(USER_DEFAULT_SCREEN_DPI));

			std::vector<domain::S_CARD> Cards;
			for (std::size_t nIndex = 0; nIndex < _nCount; ++nIndex)
			{
				Cards.push_back(make_card(static_cast<int>(nIndex) + 1,
					to_utf8(L"카드 " + std::to_wstring(nIndex + 1))));
			}
			m_Projection.SetSortMode(domain::E_CARD_LIST_SORT_MODE::Position);
			m_Projection.SetMultiSelectionEnabled(_bExtended);
			m_Projection.SetCards(Cards);
			m_Control.OnProjectionChanged();
			this->FocusIn();
		}

		~C_WHEEL_FIXTURE()
		{
			// 시험이 남긴 타이머가 fixture 보다 오래 살면 다음 케이스의 펌프에서 떨어진다.
			m_Control.CancelPendingBrowse();
			if (::GetCapture() == m_Control.m_hWnd) { ::ReleaseCapture(); }
			if (m_Control.IsWindow()) { m_Control.DestroyWindow(); }
		}

		C_WHEEL_FIXTURE(const C_WHEEL_FIXTURE&) = delete;
		C_WHEEL_FIXTURE& operator=(const C_WHEEL_FIXTURE&) = delete;

		C_CARD_LIST& Control() noexcept { return(m_Control); }
		const C_CARD_LIST& Control() const noexcept { return(m_Control); }
		HWND Hwnd() const noexcept { return(m_Control.m_hWnd); }
		TestWindow& Host() noexcept { return(m_Host); }
		domain::C_CARD_LIST_PROJECTION& ProjectionRef() noexcept { return(m_Projection); }

		const std::vector<std::string>& Selected() const { return(m_Projection.SelectedCardIds()); }
		const std::vector<std::string>& Opened() const noexcept { return(m_Opened); }
		const std::vector<std::string>& Browsed() const noexcept { return(m_Browsed); }

		int CurrentRow() const
		{
			const std::optional<std::string>& sCurrent = m_Projection.CurrentCardId();
			if (!sCurrent) { return(-1); }
			const auto nRow = m_Projection.RowForCard(*sCurrent);
			return(nRow ? static_cast<int>(*nRow) : -1);
		}

		int AnchorRow() const
		{
			const auto nRow = m_Control.AnchorRow();
			return(nRow ? static_cast<int>(*nRow) : -1);
		}

		// 대기 카드 id 를 문자열로 평탄화한다("-" = 대기 없음). 단언 실패 메시지가 읽힌다.
		std::string Pending() const
		{
			const auto sCardId = m_Control.PendingBrowseCardId();
			return(sCardId ? *sCardId : std::string("-"));
		}

		int Remainder() const { return(m_Control.WheelAngleRemainder()); }
		int Offset() const { return(m_Control.ScrollOffsetDip()); }

		POINT Pt(std::size_t _nRow) const
		{
			const S_DIP_RECT Row = m_Control.RowRectDip(_nRow);
			return(POINT{ Row.nLeft + Row.nWidth / 2, Row.nTop + Row.nHeight / 2 });
		}

		// 마지막 행 아래의 지점이다. 내용이 뷰포트보다 높으면 이 값은 뷰포트 밖으로 나가는데,
		// 그때도 "행 위가 아니다" 라는 성질은 같다(컨트롤은 뷰포트 밖을 행 없음으로 읽는다).
		POINT EmptyPoint() const
		{
			const S_DIP_RECT Last = m_Control.RowRectDip(m_Projection.RowCount() - 1);
			return(POINT{ m_Control.ViewportWidthDip() / 2,
				(Last.Bottom() + m_Control.ViewportHeightDip()) / 2 });
		}

		// 탐색 열기 핸들러의 반환값(원본 _open_card 의 결과 자리)과 편집면 카드 제공자의 값.
		bool BrowseResult{ true };
		std::optional<std::string> EditorCard{};

	private:
		void FocusIn()
		{
			::SetFocus(m_Host.hwnd());
			pynote::harness::drain_messages();
			::SetFocus(m_Control.m_hWnd);
			pynote::harness::drain_messages();
			// 오라클 N3: 행이 있으면 포커스 진입에 선택 없이 현재 행만 0 이 된다.
			REQUIRE(this->CurrentRow() == (m_Projection.RowCount() > 0 ? 0 : -1));
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
		std::vector<std::string> m_Browsed;
	};
}

TEST_CASE("PLAN-W4-0055 wheel input we cannot use is not swallowed", "[W4-wheel][WTL-CAP-FI-064]")
{
	// 원본 test_wheel_input_we_cannot_use_is_not_swallowed(:266~285). 카드 이동으로 바꿀 수
	// 없는 입력을 소비해 버리면 상위 위젯이 처리 기회를 잃는다.
	C_WHEEL_FIXTURE Fixture(40);
	::SendMessageW(Fixture.Hwnd(), LB_SETCURSEL, 0, 0);

	Fixture.Host().clear_messages();
	// Win32 에는 픽셀 전용 휠 메시지가 없다 - 0 각이 원본의 "pixelDelta 만 실은 이벤트" 와
	// "0 각 이벤트" 두 벡터의 네이티브 쌍둥이다(오라클 P8).
	send_wheel(Fixture.Hwnd(), 0);

	REQUIRE(Fixture.CurrentRow() == 0);
	REQUIRE(Fixture.Pending() == "-");
	REQUIRE(Fixture.Remainder() == 0);
	// 미소비 -> DefWindowProc 이 부모로 올린다. 이것이 원본 not isAccepted() 의 관측 수단이다.
	REQUIRE(Fixture.Host().received(WM_MOUSEWHEEL));

	Fixture.Host().clear_messages();
	const int nOffset = Fixture.Offset();
	send_hwheel(Fixture.Hwnd(), -WHEEL_TICK_ANGLE);

	REQUIRE(Fixture.CurrentRow() == 0);
	REQUIRE(Fixture.Offset() == nOffset);
	// WM_MOUSEHWHEEL 은 아예 처리하지 않는다(메시지 맵에 없다).
	REQUIRE(Fixture.Host().received(WM_MOUSEHWHEEL));

	Fixture.Control().CancelPendingBrowse();
}

namespace
{
	// 진짜 DB·마이그레이션·서비스 위의 페이지(S2 C_PAGE_FIXTURE 모양). 좌 pane 은 여러 행이
	// 한 화면에 들어가도록 높게 잡는다 - 마우스로 행을 짚어야 하기 때문이다.
	// S2 의 Paste 는 옮기지 않았다 - 이 조각의 어느 케이스도 붙여넣기를 쓰지 않는다.
	class C_PAGE_FIXTURE
	{
	public:
		C_PAGE_FIXTURE()
			: m_Path(std::filesystem::temp_directory_path() /
				("NoteEx-W4S3-page-" + std::to_string(::GetCurrentProcessId()) + "-" +
				std::to_string(++s_nSequence) + ".db")), m_Repositories(m_Database),
			  m_DraftStore(m_Database, m_Repositories),
			  m_Parent(pynote::harness::TestWindowOptions{ L"W4 S3 page host", 1000, 780, true })
		{
			this->remove_();
			REQUIRE(m_Database.Open(m_Path.string()));
			storage::C_MIGRATION_RUNNER Runner;
			Runner.SetExistingDatabase(false, m_Path.string());
			REQUIRE(Runner.Run(m_Database) == storage::E_MIGRATE_RESULT::Ok);
			domain::S_DOCUMENT Document;
			Document.sId = DocumentId;
			Document.sTitle = "w4 s3 document";
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
			m_Page = std::make_unique<C_DOCUMENT_PAGE>();
			m_Page->SetRenderServices(&m_Device, &m_Brushes, &m_Text);
			S_CARD_LIST_DISPLAY Display;
			Display.Font.sFamily = TEST_FONT_FAMILY;
			Display.Font.fSizeDip = TEST_FONT_SIZE_DIP;
			m_Page->SetDisplaySettings(Display);
			REQUIRE(m_Page->Init(::GetModuleHandleW(nullptr), m_hLeft, m_hRight,
				m_Database, m_Repositories, *m_CardService, *m_Drafts, *m_Save,
				WorkspaceId, DocumentId, [this](HWND) { return(LeaveChoice); }));
			m_pProjection = m_Page->CardList().Projection();
			REQUIRE(m_pProjection != nullptr);
		}

		~C_PAGE_FIXTURE()
		{
			if (m_Page) { m_Page->CardList().CancelPendingBrowse(); }
			m_Page.reset();
			m_Save.reset();
			m_Drafts.reset();
			m_CardService.reset();
			m_Database.Close();
			this->remove_();
		}

		C_PAGE_FIXTURE(const C_PAGE_FIXTURE&) = delete;
		C_PAGE_FIXTURE& operator=(const C_PAGE_FIXTURE&) = delete;

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

		C_DOCUMENT_PAGE& Page() { return(*m_Page); }
		C_CARD_LIST& CardList() const { return(m_Page->CardList()); }
		HWND CardListHwnd() const { return(m_Page->CardListHwnd()); }
		HWND EditorHwnd() const { return(m_Page->EditorHwnd()); }

		std::wstring EditorText() const
		{
			std::wstring Text(
				static_cast<std::size_t>(::GetWindowTextLengthW(m_Page->EditorHwnd())) + 1, L'\0');
			Text.resize(static_cast<std::size_t>(::GetWindowTextW(
				m_Page->EditorHwnd(), Text.data(), static_cast<int>(Text.size()))));
			return(Text);
		}

		// 페이지의 정렬 모드는 UI 상태 기본값이다 - 행 <-> 카드 매핑은 늘 프로젝션에서 읽는다.
		// 포인터는 Init 이 Bind 한 뒤 바뀌지 않으므로 생성자에서 한 번만 확인한다 - 펌프 술어가
		// 이 접근자를 부르므로 여기에 단언을 두면 단언 수가 대기 시간에 따라 흔들린다.
		const domain::C_CARD_LIST_PROJECTION& Projection() const { return(*m_pProjection); }

		std::string CardOfRow(std::size_t _nRow) const
		{
			const domain::S_CARD* pCard = this->Projection().CardAt(_nRow);
			REQUIRE(pCard != nullptr);
			return(pCard->sId);
		}

		std::wstring BodyOfRow(std::size_t _nRow) const
		{
			const domain::S_CARD* pCard = this->Projection().CardAt(_nRow);
			REQUIRE(pCard != nullptr);
			return(to_wide(pCard->sBody));
		}

		int CurrentRow() const
		{
			const std::optional<std::string>& sCurrent = this->Projection().CurrentCardId();
			if (!sCurrent) { return(-1); }
			const auto nRow = this->Projection().RowForCard(*sCurrent);
			return(nRow ? static_cast<int>(*nRow) : -1);
		}

		const std::vector<std::string>& Selected() const
		{
			return(this->Projection().SelectedCardIds());
		}

		std::string Pending() const
		{
			const auto sCardId = m_Page->CardList().PendingBrowseCardId();
			return(sCardId ? *sCardId : std::string("-"));
		}

		POINT RowPoint(std::size_t _nRow) const
		{
			const S_DIP_RECT Row = m_Page->CardList().RowRectDip(_nRow);
			return(POINT{ Row.nLeft + Row.nWidth / 2, Row.nTop + Row.nHeight / 2 });
		}

		// 세션을 더티로 만드는 유일한 실입력 경로다(W3 fixture 의 Type 과 같다). 편집면 서브클래스는
		// WM_PASTE·WM_CHAR 만 가로채 synchronize_editor 를 부르므로 EM_REPLACESEL 로는 더티가 되지 않는다.
		void Type(wchar_t _Char)
		{
			::SetFocus(m_Page->EditorHwnd());
			::SendMessageW(m_Page->EditorHwnd(), WM_CHAR, static_cast<WPARAM>(_Char), 1);
		}

		static constexpr int LIST_HOST_HEIGHT = 700;
		C_DOCUMENT_PAGE::E_LEAVE_CHOICE LeaveChoice{ C_DOCUMENT_PAGE::E_LEAVE_CHOICE::Save };
		inline static const std::string DocumentId = "document-w4s3";
		inline static const std::string WorkspaceId = "window-w4s3";

	private:
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
		const domain::C_CARD_LIST_PROJECTION* m_pProjection{ nullptr };
		std::int64_t m_nClock{ 2000 };
		std::uint64_t m_nId{};
		inline static std::atomic<unsigned long> s_nSequence{};
	};

	// 현재 행이 뷰포트 안에 온전히 들어왔는가 - Qt viewport().rect().contains(visualRect) 와 같은
	// 닫힌 정수 구간 판정이다.
	void require_row_visible(const C_CARD_LIST& _Control, std::size_t _nRow)
	{
		const S_DIP_RECT Row = _Control.RowRectDip(_nRow);
		REQUIRE(Row.nTop >= 0);
		REQUIRE(Row.Bottom() <= _Control.ViewportHeightDip() - 1);
	}
}

TEST_CASE("PLAN-W4-0053 wheeling keeps every visited card in view",
	"[W4-wheel][WTL-CAP-RE-011][WTL-CAP-FI-063]")
{
	// 원본 test_wheeling_keeps_every_visited_card_in_view(:195~214). CAP-RE-011 의 휠 절반이
	// 여기서 닫힌다 - S1 은 픽셀 스크롤·reveal 절반만 닫았다.
	C_WHEEL_FIXTURE Fixture(40);
	::SendMessageW(Fixture.Hwnd(), LB_SETCURSEL, 0, 0);
	REQUIRE(Fixture.Control().ContentHeightDip() > Fixture.Control().ViewportHeightDip());
	REQUIRE(Fixture.Offset() == 0);
	// 마지막 행이 처음에는 화면 밖이어야 "보이게 하기" 가 판별력을 갖는다.
	REQUIRE(Fixture.Control().RowRectDip(39).nTop >= Fixture.Control().ViewportHeightDip());
	Fixture.Host().clear_messages();

	for (int nStep = 1; nStep <= 39; ++nStep)
	{
		INFO("down step " << nStep);
		send_wheel(Fixture.Hwnd(), -WHEEL_TICK_ANGLE);
		REQUIRE(Fixture.CurrentRow() == nStep);
		require_row_visible(Fixture.Control(), static_cast<std::size_t>(nStep));
	}
	REQUIRE(Fixture.CurrentRow() == 39);
	REQUIRE(Fixture.Pending() == "card-40");

	for (int nStep = 38; nStep >= 0; --nStep)
	{
		INFO("up step " << nStep);
		send_wheel(Fixture.Hwnd(), WHEEL_TICK_ANGLE);
		REQUIRE(Fixture.CurrentRow() == nStep);
		require_row_visible(Fixture.Control(), static_cast<std::size_t>(nStep));
	}
	REQUIRE(Fixture.CurrentRow() == 0);
	REQUIRE(Fixture.Pending() == "card-1");
	// 탐색이 소비한 메시지는 부모로 올라가지 않는다.
	REQUIRE_FALSE(Fixture.Host().received(WM_MOUSEWHEEL));

	Fixture.Control().CancelPendingBrowse();
	REQUIRE(Fixture.Pending() == "-");
}

TEST_CASE("PLAN-W4-0054 modifier wheel keeps the plain scrollbar behaviour", "[W4-wheel][WTL-CAP-FI-064]")
{
	// 원본 test_modifier_wheel_keeps_the_plain_scrollbar_behaviour(:239~263) + 오라클 P1/P2/P3/P5.
	C_WHEEL_FIXTURE Fixture(40);
	::SendMessageW(Fixture.Hwnd(), LB_SETCURSEL, 0, 0);
	const int nViewport = Fixture.Control().ViewportHeightDip();
	const int nBefore = Fixture.Offset();
	Fixture.Host().clear_messages();

	send_wheel(Fixture.Hwnd(), -3 * WHEEL_TICK_ANGLE, MK_CONTROL);

	REQUIRE(Fixture.CurrentRow() == 0);
	REQUIRE(Fixture.Offset() > nBefore);
	// P2: 세 틱이어도 한 페이지다(clamp(-pageStep, .., pageStep)).
	REQUIRE(Fixture.Offset() - nBefore == nViewport);
	REQUIRE_FALSE(Fixture.Host().received(WM_MOUSEWHEEL));
	pump_for(QUIET_WINDOW_MS);
	REQUIRE(Fixture.Browsed().empty());
	REQUIRE(Fixture.Pending() == "-");

	// P5: Shift 도 Ctrl 과 같은 페이지 스크롤이고 수평 전환이 아니다.
	const int nAfterCtrl = Fixture.Offset();
	send_wheel(Fixture.Hwnd(), -WHEEL_TICK_ANGLE, MK_SHIFT);
	REQUIRE(Fixture.Offset() - nAfterCtrl == nViewport);

	// P3: 한 틱을 못 채운 각은 trunc(angle/120 x pageStep) 만큼이다.
	const int nAfterShift = Fixture.Offset();
	send_wheel(Fixture.Hwnd(), -40, MK_CONTROL);
	REQUIRE(Fixture.Offset() - nAfterShift == 40 * nViewport / 120);

	const int nAfterPartial = Fixture.Offset();
	send_wheel(Fixture.Hwnd(), WHEEL_TICK_ANGLE, MK_CONTROL);
	REQUIRE(nAfterPartial - Fixture.Offset() == nViewport);

	REQUIRE(Fixture.CurrentRow() == 0);
	Fixture.Control().CancelPendingBrowse();
}

TEST_CASE("PLAN-W4-0056 wheel browse opens the card and leaves focus on the list",
	"[W4-wheel][WTL-CAP-TI-022][WTL-CAP-FI-063]")
{
	// 원본 test_wheel_browse_opens_the_card_and_leaves_focus_on_the_list(:288~310).
	C_PAGE_FIXTURE Fixture;
	Fixture.CreateCards(8);
	const std::wstring sBody2 = Fixture.BodyOfRow(2);
	const std::wstring sBody3 = Fixture.BodyOfRow(3);
	const std::string sCard3 = Fixture.CardOfRow(3);
	::SendMessageW(Fixture.CardListHwnd(), LB_SETCURSEL, 0, 0);
	::SetFocus(Fixture.CardListHwnd());

	// 한 이벤트에 두 틱(원본 steps=2).
	send_wheel(Fixture.CardListHwnd(), -2 * WHEEL_TICK_ANGLE);
	{
		const bool bOpened = pump_until([&Fixture, &sBody2]()
			{ return(Fixture.Page().HasSession() && Fixture.EditorText() == sBody2); }, OPEN_TIMEOUT_MS);
		// 대기가 실패하면 그 시점의 상태가 원인을 가른다(열기 실패 / 타이머 미도달 / 본문 불일치).
		INFO("open wait: session=" << Fixture.Page().HasSession()
			<< " pending=" << Fixture.Pending() << " current=" << Fixture.CurrentRow()
			<< " textlen=" << Fixture.EditorText().size() << " wantlen=" << sBody2.size()
			<< " match=" << (Fixture.EditorText() == sBody2)
			<< " remainder=" << Fixture.CardList().WheelAngleRemainder());
		REQUIRE(bOpened);
	}
	REQUIRE(::GetFocus() == Fixture.CardListHwnd());

	send_wheel(Fixture.CardListHwnd(), -WHEEL_TICK_ANGLE);
	{
		const bool bOpened = pump_until([&Fixture, &sBody3]()
			{ return(Fixture.EditorText() == sBody3); }, OPEN_TIMEOUT_MS);
		INFO("open wait 2: session=" << Fixture.Page().HasSession()
			<< " pending=" << Fixture.Pending() << " current=" << Fixture.CurrentRow()
			<< " textlen=" << Fixture.EditorText().size() << " wantlen=" << sBody3.size());
		REQUIRE(bOpened);
	}
	REQUIRE(::GetFocus() == Fixture.CardListHwnd());
	REQUIRE(Fixture.Selected() == std::vector<std::string>{ sCard3 });

	Fixture.CardList().CancelPendingBrowse();
}

TEST_CASE("PLAN-W4-0057 click during the wheel delay wins and keeps editor focus",
	"[W4-wheel][WTL-CAP-RE-013][WTL-CAP-TI-022]")
{
	// 원본 test_click_during_the_wheel_delay_wins_and_keeps_editor_focus(:313~338).
	C_PAGE_FIXTURE Fixture;
	Fixture.CreateCards(12);
	const std::string sCard3 = Fixture.CardOfRow(3);
	const std::wstring sBody1 = Fixture.BodyOfRow(1);
	::SendMessageW(Fixture.CardListHwnd(), LB_SETCURSEL, 0, 0);
	::SetFocus(Fixture.CardListHwnd());

	send_wheel(Fixture.CardListHwnd(), -3 * WHEEL_TICK_ANGLE);
	REQUIRE(Fixture.CurrentRow() == 3);
	REQUIRE(Fixture.Pending() == sCard3);

	// 원본 scrollTo(target) - 클릭할 행을 화면에 들인다(대기는 취소하지 않는다).
	Fixture.CardList().EnsureVisible(1);
	send_click(Fixture.CardListHwnd(), Fixture.RowPoint(1));
	// press 가 대기를 즉시 폐기하고 release 가 행 1 을 연다.
	REQUIRE(Fixture.Pending() == "-");

	pump_for(QUIET_WINDOW_MS);
	REQUIRE(Fixture.Page().HasSession());
	// 페이지가 자기 열기 핸들러를 들고 있어 열기 "횟수" 는 관측할 수 없다 - 행 3 의 탐색
	// 열기가 일어나지 않았다는 것이 계약이고, 편집면 본문이 그것을 드러낸다.
	REQUIRE(Fixture.EditorText() == sBody1);
	REQUIRE(::GetFocus() == Fixture.EditorHwnd());

	Fixture.CardList().CancelPendingBrowse();
}

TEST_CASE("PLAN-W4-0058 reveal card during the wheel delay keeps the editor unconnected",
	"[W4-wheel][WTL-CAP-RE-013]")
{
	// 원본 test_reveal_card_during_the_wheel_delay_keeps_the_editor_unconnected(:341~361).
	C_PAGE_FIXTURE Fixture;
	Fixture.CreateCards(12);
	const std::string sCard2 = Fixture.CardOfRow(2);
	const std::string sCard7 = Fixture.CardOfRow(7);
	::SendMessageW(Fixture.CardListHwnd(), LB_SETCURSEL, 0, 0);

	send_wheel(Fixture.CardListHwnd(), -2 * WHEEL_TICK_ANGLE);
	REQUIRE(Fixture.Pending() == sCard2);

	REQUIRE(Fixture.Page().RevealCard(sCard7));
	REQUIRE(Fixture.Pending() == "-");
	REQUIRE(Fixture.CurrentRow() == 7);

	pump_for(QUIET_WINDOW_MS);
	REQUIRE_FALSE(Fixture.Page().HasSession());
	REQUIRE(Fixture.EditorText().empty());
	REQUIRE(Fixture.CurrentRow() == 7);

	// 알 수 없는 카드는 false 를 돌리고 아무것도 바꾸지 않는다(원본은 취소 앞에서 돌아간다).
	const std::vector<std::string> Selected = Fixture.Selected();
	REQUIRE_FALSE(Fixture.Page().RevealCard("no-such-card"));
	REQUIRE(Fixture.CurrentRow() == 7);
	REQUIRE(Fixture.Selected() == Selected);
	REQUIRE_FALSE(Fixture.Page().HasSession());

	Fixture.CardList().CancelPendingBrowse();
}

TEST_CASE("PLAN-W4-0059 selection moving elsewhere during the delay cancels the open",
	"[W4-wheel][WTL-CAP-NC-012][WTL-CAP-RE-013]")
{
	// 원본 test_selection_moving_elsewhere_during_the_delay_cancels_the_open(:364~381).
	C_PAGE_FIXTURE Fixture;
	Fixture.CreateCards(8);
	const std::string sCard2 = Fixture.CardOfRow(2);
	::SendMessageW(Fixture.CardListHwnd(), LB_SETCURSEL, 0, 0);

	send_wheel(Fixture.CardListHwnd(), -2 * WHEEL_TICK_ANGLE);
	REQUIRE(Fixture.Pending() == sCard2);

	// 원본 setCurrentIndex 는 취소하지 않는다 - 대기는 그대로 남고, 만료 시점의 동일성 검사가
	// 열기를 막는다(이 케이스가 "취소" 가 아니라 "동일성" 을 보는 이유다).
	::SendMessageW(Fixture.CardListHwnd(), LB_SETCURSEL, 6, 0);
	REQUIRE(Fixture.Pending() == sCard2);

	pump_for(QUIET_WINDOW_MS);
	REQUIRE_FALSE(Fixture.Page().HasSession());
	REQUIRE(Fixture.EditorText().empty());
	REQUIRE(Fixture.CurrentRow() == 6);
	REQUIRE(Fixture.Pending() == "-");

	Fixture.CardList().CancelPendingBrowse();
}

TEST_CASE("PLAN-W4-0060 empty area click during the delay does not reopen the editor",
	"[W4-wheel][WTL-CAP-RE-013]")
{
	// 원본 test_empty_area_click_during_the_delay_does_not_reopen_the_editor(:384~408).
	C_PAGE_FIXTURE Fixture;
	Fixture.CreateCards(4);
	const std::string sCard0 = Fixture.CardOfRow(0);
	const std::string sCard2 = Fixture.CardOfRow(2);

	// 앱 주도 열기(원본 page.open_card).
	REQUIRE(Fixture.Page().RevealCard(sCard0));
	REQUIRE(Fixture.Page().OpenSelectedCard());
	REQUIRE(Fixture.Page().HasSession());

	send_wheel(Fixture.CardListHwnd(), -2 * WHEEL_TICK_ANGLE);
	REQUIRE(Fixture.Pending() == sCard2);

	// 원본 viewport().rect().bottomRight() - QPoint(10,10). HitTestRow 는 클라이언트 픽셀을
	// 받아 DIP 로 바꾸므로 96 DPI(픽셀 == DIP)를 한 번 못박는다.
	REQUIRE(::GetDpiForWindow(Fixture.CardListHwnd()) == USER_DEFAULT_SCREEN_DPI);
	RECT Client{};
	REQUIRE(::GetClientRect(Fixture.CardListHwnd(), &Client) != FALSE);
	const POINT Point{ Client.right - 11, Client.bottom - 11 };
	REQUIRE_FALSE(Fixture.CardList().HitTestRow(Point).has_value());

	send_click(Fixture.CardListHwnd(), Point);
	pump_for(QUIET_WINDOW_MS);

	// 빈 영역 클릭이 깨끗한 세션을 RequestLeave 로 닫았고, press 가 대기 열기를 폐기했다.
	REQUIRE_FALSE(Fixture.Page().HasSession());
	REQUIRE(Fixture.EditorText().empty());
	REQUIRE(Fixture.Pending() == "-");
	// S2 빈 영역 클릭 계약: 지연 재포커스까지 끝나면 포커스는 편집면이다.
	REQUIRE(::GetFocus() == Fixture.EditorHwnd());

	Fixture.CardList().CancelPendingBrowse();
}

TEST_CASE("W4 wheel ticks move the current card with core arithmetic through real messages",
	"[W4-wheel][WTL-CAP-RE-012][WTL-CAP-FI-063]")
{
	// core 골든(WTL-W2-0044~0047)과 같은 모양을 진짜 WM_MOUSEWHEEL 로 다시 본다
	// (원본 test_card_wheel_browse.py:111~192).
	C_WHEEL_FIXTURE Fixture(10);
	::SendMessageW(Fixture.Hwnd(), LB_SETCURSEL, 3, 0);
	Fixture.Host().clear_messages();

	std::vector<int> Rows;
	for (int nIndex = 0; nIndex < 3; ++nIndex)
	{
		send_wheel(Fixture.Hwnd(), -WHEEL_TICK_ANGLE);
		Rows.push_back(Fixture.CurrentRow());
	}
	REQUIRE(Rows == std::vector<int>{ 4, 5, 6 });
	REQUIRE(Fixture.Selected() == std::vector<std::string>{ "card-7" });

	Rows.clear();
	for (int nIndex = 0; nIndex < 3; ++nIndex)
	{
		send_wheel(Fixture.Hwnd(), WHEEL_TICK_ANGLE);
		Rows.push_back(Fixture.CurrentRow());
	}
	REQUIRE(Rows == std::vector<int>{ 5, 4, 3 });
	REQUIRE(Fixture.Selected() == std::vector<std::string>{ "card-4" });

	// 양 끝에서 감기지 않는다.
	::SendMessageW(Fixture.Hwnd(), LB_SETCURSEL, 9, 0);
	send_wheel(Fixture.Hwnd(), -WHEEL_TICK_ANGLE);
	REQUIRE(Fixture.CurrentRow() == 9);
	REQUIRE(Fixture.Pending() == "card-10");
	::SendMessageW(Fixture.Hwnd(), LB_SETCURSEL, 0, 0);
	send_wheel(Fixture.Hwnd(), WHEEL_TICK_ANGLE);
	REQUIRE(Fixture.CurrentRow() == 0);
	REQUIRE(Fixture.Pending() == "card-1");

	// 한 틱을 못 채운 각은 누적된다.
	Fixture.Control().CancelPendingBrowse();
	std::vector<int> PartialRows;
	std::vector<int> PartialRemainders;
	for (int nIndex = 0; nIndex < 6; ++nIndex)
	{
		send_wheel(Fixture.Hwnd(), -40);
		PartialRows.push_back(Fixture.CurrentRow());
		PartialRemainders.push_back(Fixture.Remainder());
	}
	REQUIRE(PartialRows == std::vector<int>{ 0, 0, 1, 1, 1, 2 });
	REQUIRE(PartialRemainders == std::vector<int>{ -40, -80, 0, -40, -80, 0 });

	// 방향을 바꾸면 이전 방향의 잔여 각을 버린다.
	::SendMessageW(Fixture.Hwnd(), LB_SETCURSEL, 4, 0);
	Fixture.Control().CancelPendingBrowse();
	send_wheel(Fixture.Hwnd(), -40);
	send_wheel(Fixture.Hwnd(), -40);
	REQUIRE(Fixture.CurrentRow() == 4);
	REQUIRE(Fixture.Remainder() == -80);
	send_wheel(Fixture.Hwnd(), WHEEL_TICK_ANGLE);
	REQUIRE(Fixture.CurrentRow() == 3);
	REQUIRE(Fixture.Remainder() == 0);

	// 탐색 경로의 메시지는 전부 소비된다.
	REQUIRE_FALSE(Fixture.Host().received(WM_MOUSEWHEEL));
	Fixture.Control().CancelPendingBrowse();

	// Qt setCurrentIndex 는 currentSelectionStartIndex 도 옮긴다 - 휠 직후 Shift+클릭 범위가
	// 그것을 드러낸다(확장 모드).
	C_WHEEL_FIXTURE Extended(10, true);
	::SendMessageW(Extended.Hwnd(), LB_SETCURSEL, 0, 0);
	send_wheel(Extended.Hwnd(), -2 * WHEEL_TICK_ANGLE);
	REQUIRE(Extended.CurrentRow() == 2);
	REQUIRE(Extended.AnchorRow() == 2);
	REQUIRE(Extended.Control().HitTestRow(Extended.Pt(4)) == std::optional<std::size_t>(4));
	send_click(Extended.Hwnd(), Extended.Pt(4), MK_SHIFT);
	REQUIRE(Extended.Selected() == std::vector<std::string>{ "card-3", "card-4", "card-5" });
	Extended.Control().CancelPendingBrowse();
}

TEST_CASE("W4 wheel open fires once after the quiet delay on a real timer and is cancelled by press key reveal and reset",
	"[W4-wheel][WTL-CAP-RE-013][WTL-CAP-FI-063]")
{
	C_WHEEL_FIXTURE Fixture(12);

	// (a) 굴리는 도중에는 열리지 않고, 멈춘 뒤 정확히 한 번만 열린다(원본 :217~236).
	for (int nIndex = 0; nIndex < 4; ++nIndex)
	{
		send_wheel(Fixture.Hwnd(), -WHEEL_TICK_ANGLE);
		pump_for(BURST_INTERVAL_MS);
	}
	REQUIRE(Fixture.Browsed().empty());
	REQUIRE(pump_until([&Fixture]()
		{ return(Fixture.Browsed() == std::vector<std::string>{ "card-5" }); }, OPEN_TIMEOUT_MS));
	pump_for(QUIET_WINDOW_MS);
	REQUIRE(Fixture.Browsed() == std::vector<std::string>{ "card-5" });

	// (b) 한 틱을 못 채운 각도 대기를 다시 미룬다(core 는 대기 카드가 있으면 무조건 재무장한다).
	send_wheel(Fixture.Hwnd(), -WHEEL_TICK_ANGLE);
	REQUIRE(Fixture.Pending() == "card-6");
	pump_for(80);
	send_wheel(Fixture.Hwnd(), -40);
	REQUIRE(Fixture.Pending() == "card-6");
	REQUIRE(Fixture.Remainder() == -40);
	pump_for(60);
	// 첫 무장이 살아 있었다면 이 시점(약 140ms)에 이미 열렸어야 한다.
	REQUIRE(Fixture.Browsed().size() == 1);
	REQUIRE(pump_until([&Fixture]() { return(Fixture.Browsed().size() == 2); }, OPEN_TIMEOUT_MS));
	REQUIRE(Fixture.Browsed().back() == "card-6");

	// (c) press 가 대기를 폐기한다. 열리는 것은 release 지점 행이고 그것은 browsed 가 아니라 opened 다.
	::SendMessageW(Fixture.Hwnd(), LB_SETCURSEL, 0, 0);
	Fixture.Control().CancelPendingBrowse();
	std::size_t nBrowsed = Fixture.Browsed().size();
	send_wheel(Fixture.Hwnd(), -3 * WHEEL_TICK_ANGLE);
	REQUIRE(Fixture.Pending() != "-");
	REQUIRE(Fixture.Control().HitTestRow(Fixture.Pt(1)) == std::optional<std::size_t>(1));
	send_press(Fixture.Hwnd(), Fixture.Pt(1));
	REQUIRE(Fixture.Pending() == "-");
	send_release(Fixture.Hwnd(), Fixture.Pt(1));
	REQUIRE(Fixture.Opened() == std::vector<std::string>{ "card-2" });
	pump_for(QUIET_WINDOW_MS);
	REQUIRE(Fixture.Browsed().size() == nBrowsed);

	// (d) 우클릭·가운데 클릭도 취소한다. 가운데 버튼은 컨트롤이 처리하지 않으므로 그 밖에는
	// 아무것도 바뀌지 않는다(버튼 메시지는 DefWindowProc 이 부모로 올리지 않아 수신 관측이 없다).
	::SendMessageW(Fixture.Hwnd(), LB_SETCURSEL, 0, 0);
	Fixture.Control().CancelPendingBrowse();
	// 내용이 뷰포트보다 높아 화면 안에는 빈 영역이 없다 - EmptyPoint 는 뷰포트 아래를 가리키며
	// 어느 행 위도 아니라는 성질만 쓴다.
	REQUIRE_FALSE(Fixture.Control().HitTestRow(Fixture.EmptyPoint()).has_value());
	send_wheel(Fixture.Hwnd(), -WHEEL_TICK_ANGLE);
	REQUIRE(Fixture.Pending() != "-");
	::SendMessageW(Fixture.Hwnd(), WM_RBUTTONDOWN, MK_RBUTTON, pack_point(Fixture.EmptyPoint()));
	REQUIRE(Fixture.Pending() == "-");
	send_wheel(Fixture.Hwnd(), -WHEEL_TICK_ANGLE);
	REQUIRE(Fixture.Pending() != "-");
	const std::vector<std::string> SelectedBefore = Fixture.Selected();
	const int nCurrentBefore = Fixture.CurrentRow();
	const int nAnchorBefore = Fixture.AnchorRow();
	::SendMessageW(Fixture.Hwnd(), WM_MBUTTONDOWN, MK_MBUTTON, pack_point(Fixture.EmptyPoint()));
	REQUIRE(Fixture.Pending() == "-");
	REQUIRE(Fixture.Selected() == SelectedBefore);
	REQUIRE(Fixture.CurrentRow() == nCurrentBefore);
	REQUIRE(Fixture.AnchorRow() == nAnchorBefore);

	// (e) 키는 소비하지 않는 것까지 전부 취소한다(WM_KEYDOWN 과 WM_CHAR 양쪽).
	::SendMessageW(Fixture.Hwnd(), LB_SETCURSEL, 0, 0);
	Fixture.Control().CancelPendingBrowse();
	nBrowsed = Fixture.Browsed().size();
	send_wheel(Fixture.Hwnd(), -WHEEL_TICK_ANGLE);
	REQUIRE(Fixture.Pending() != "-");
	const int nBeforeKey = Fixture.CurrentRow();
	::SendMessageW(Fixture.Hwnd(), WM_KEYDOWN, VK_DOWN, 1);
	REQUIRE(Fixture.Pending() == "-");
	REQUIRE(Fixture.CurrentRow() == nBeforeKey + 1);
	send_wheel(Fixture.Hwnd(), -WHEEL_TICK_ANGLE);
	REQUIRE(Fixture.Pending() != "-");
	::SendMessageW(Fixture.Hwnd(), WM_CHAR, static_cast<WPARAM>(L'x'), 1);
	REQUIRE(Fixture.Pending() == "-");
	pump_for(QUIET_WINDOW_MS);
	REQUIRE(Fixture.Browsed().size() == nBrowsed);

	// (f) reveal 이 취소한다(원본 reveal_card 의 cancel_pending_browse).
	::SendMessageW(Fixture.Hwnd(), LB_SETCURSEL, 0, 0);
	Fixture.Control().CancelPendingBrowse();
	nBrowsed = Fixture.Browsed().size();
	send_wheel(Fixture.Hwnd(), -WHEEL_TICK_ANGLE);
	REQUIRE(Fixture.Pending() != "-");
	Fixture.Control().RevealRow(7);
	REQUIRE(Fixture.Pending() == "-");
	REQUIRE(Fixture.CurrentRow() == 7);
	pump_for(QUIET_WINDOW_MS);
	REQUIRE(Fixture.Browsed().size() == nBrowsed);

	// (g) 모델 리셋이 취소한다(원본 modelAboutToBeReset -> cancel_pending_browse).
	C_PAGE_FIXTURE Page;
	Page.CreateCards(12);
	::SendMessageW(Page.CardListHwnd(), LB_SETCURSEL, 0, 0);
	send_wheel(Page.CardListHwnd(), -3 * WHEEL_TICK_ANGLE);
	REQUIRE(Page.Pending() != "-");
	REQUIRE(Page.Page().Refresh());
	REQUIRE(Page.Pending() == "-");
	pump_for(QUIET_WINDOW_MS);
	REQUIRE_FALSE(Page.Page().HasSession());

	Fixture.Control().CancelPendingBrowse();
}

TEST_CASE("W4 wheel fallthrough leaves modifier zero angle horizontal alt and empty list input to the parent",
	"[W4-wheel][WTL-CAP-FI-064]")
{
	// CAP-FI-064 의 파이썬 6갈래 중 5개를 닫는다. 6번째(드래그 중 휠)는 네이티브 드래그 세션이
	// 아직 없으므로 S4 [W4-dnd] 몫이다.
	C_WHEEL_FIXTURE Fixture(40);
	const int nViewport = Fixture.Control().ViewportHeightDip();

	// P11: 아래 끝에서 Ctrl 휠은 값이 바뀌지 않아 수락되지 않는다.
	Fixture.Control().ScrollToPixel(INT_MAX);
	const int nBottom = Fixture.Offset();
	REQUIRE(nBottom > 0);
	Fixture.Host().clear_messages();
	send_wheel(Fixture.Hwnd(), -WHEEL_TICK_ANGLE, MK_CONTROL);
	REQUIRE(Fixture.Offset() == nBottom);
	REQUIRE(Fixture.Host().received(WM_MOUSEWHEEL));

	// P4: 위 끝에서 반대 방향도 같다.
	Fixture.Control().ScrollToPixel(0);
	Fixture.Host().clear_messages();
	send_wheel(Fixture.Hwnd(), WHEEL_TICK_ANGLE, MK_CONTROL);
	REQUIRE(Fixture.Offset() == 0);
	REQUIRE(Fixture.Host().received(WM_MOUSEWHEEL));

	// P6: Ctrl+Shift 는 Ctrl 단독과 같은 한 페이지이고 소비된다.
	Fixture.Host().clear_messages();
	send_wheel(Fixture.Hwnd(), -WHEEL_TICK_ANGLE, MK_CONTROL | MK_SHIFT);
	REQUIRE(Fixture.Offset() == nViewport);
	REQUIRE_FALSE(Fixture.Host().received(WM_MOUSEWHEEL));

	Fixture.Control().ScrollToPixel(0);
	::SendMessageW(Fixture.Hwnd(), LB_SETCURSEL, 0, 0);
	Fixture.Control().CancelPendingBrowse();

	// P7r: Windows QPA 는 Alt+세로 휠을 수평 각으로 바꿔 넣고, 가로 범위가 0 인 이 뷰에서는
	// 값이 바뀌지 않아 수락되지 않는다. 네이티브는 Alt 를 비소비 분기로 옮겨 같은 관측을 만든다.
	{
		C_MODIFIER_SCOPE Alt({ VK_MENU, VK_LMENU });
		Fixture.Host().clear_messages();
		send_wheel(Fixture.Hwnd(), -WHEEL_TICK_ANGLE);
		REQUIRE(Fixture.CurrentRow() == 0);
		REQUIRE(Fixture.Offset() == 0);
		REQUIRE(Fixture.Pending() == "-");
		REQUIRE(Fixture.Remainder() == 0);
		REQUIRE(Fixture.Host().received(WM_MOUSEWHEEL));

		// Ctrl+Alt 는 Alt 가 이긴다 - 페이지 스크롤이 아니다.
		Fixture.Host().clear_messages();
		send_wheel(Fixture.Hwnd(), -WHEEL_TICK_ANGLE, MK_CONTROL);
		REQUIRE(Fixture.Offset() == 0);
		REQUIRE(Fixture.CurrentRow() == 0);
		REQUIRE(Fixture.Host().received(WM_MOUSEWHEEL));
	}

	// Meta(Win 키): 원본은 Qt 줄 스크롤로 흘려보낸다. S3 은 "탐색이 소비하지 않는다" 까지만
	// 보장하고 줄 스크롤 양은 재현하지 않는다(CAP-FI-064 의 기록된 잔여 - S4 재진입).
	{
		C_MODIFIER_SCOPE Meta({ VK_LWIN });
		Fixture.Host().clear_messages();
		send_wheel(Fixture.Hwnd(), -WHEEL_TICK_ANGLE);
		REQUIRE(Fixture.CurrentRow() == 0);
		REQUIRE(Fixture.Offset() == 0);
		REQUIRE(Fixture.Pending() == "-");
		REQUIRE(Fixture.Host().received(WM_MOUSEWHEEL));
	}

	// 0 각(케이스 PLAN-W4-0055 와 같은 관측 - 이 선택자에서도 한 번 본다).
	Fixture.Host().clear_messages();
	send_wheel(Fixture.Hwnd(), 0);
	REQUIRE(Fixture.Host().received(WM_MOUSEWHEEL));

	// P10: 빈 목록은 탐색 경로가 성립하지 않아 그대로 부모로 간다. 휠이 행 0 을 현재로 만들지 않는다.
	{
		C_WHEEL_FIXTURE Empty(0);
		Empty.Host().clear_messages();
		send_wheel(Empty.Hwnd(), -3 * WHEEL_TICK_ANGLE);
		REQUIRE(Empty.Host().received(WM_MOUSEWHEEL));
		REQUIRE_FALSE(Empty.Control().Projection()->CurrentCardId().has_value());
		REQUIRE(Empty.Pending() == "-");
	}

	Fixture.Control().CancelPendingBrowse();
}

TEST_CASE("W4 wheel open request is identity checked at firing time", "[W4-wheel][WTL-CAP-NC-012]")
{
	// 원본 _request_browse_open(:364~373) + core WTL-W2-0049.
	C_WHEEL_FIXTURE Fixture(12);
	::SendMessageW(Fixture.Hwnd(), LB_SETCURSEL, 0, 0);
	send_wheel(Fixture.Hwnd(), -3 * WHEEL_TICK_ANGLE);
	REQUIRE(Fixture.Pending() == "card-4");

	::SendMessageW(Fixture.Hwnd(), LB_SETCURSEL, 6, 0);
	REQUIRE(Fixture.Pending() == "card-4");
	pump_for(QUIET_WINDOW_MS);
	REQUIRE(Fixture.Browsed().empty());
	REQUIRE(Fixture.Pending() == "-");

	// 요청은 행이 아니라 카드 id 를 들고 간다 - 정렬이 바뀌어 행이 옮겨져도 같은 카드를 연다.
	::SendMessageW(Fixture.Hwnd(), LB_SETCURSEL, 0, 0);
	send_wheel(Fixture.Hwnd(), -WHEEL_TICK_ANGLE);
	REQUIRE(Fixture.Pending() == "card-2");
	REQUIRE(Fixture.CurrentRow() == 1);
	Fixture.ProjectionRef().SetSortMode(domain::E_CARD_LIST_SORT_MODE::Recency);
	Fixture.Control().OnProjectionChanged();
	REQUIRE(Fixture.CurrentRow() == 10);
	REQUIRE(pump_until([&Fixture]() { return(Fixture.Browsed().size() == 1); }, OPEN_TIMEOUT_MS));
	REQUIRE(Fixture.Browsed().back() == "card-2");
	// 이것은 컨트롤 수준의 동일성 프로브(행이 아니라 id)다. 제품에서 정렬 변경은 refresh_cards()
	// 를 거쳐 취소되므로(원본 set_sort_mode -> beginResetModel -> cancel) "정렬이 바뀌어도 대기
	// 열기가 살아남는다" 의 선례가 아니다.

	Fixture.Control().CancelPendingBrowse();
}

TEST_CASE("W4 wheel open success keeps list focus and failure restores the editor card and focus",
	"[W4-wheel][WTL-CAP-TI-022]")
{
	// 페이지 절반 - 원본 _browse_card(document_page.py:443~456) 와
	// test_failed_open_returns_the_selection_to_the_editor_card(:456~483).
	{
		C_PAGE_FIXTURE Fixture;
		Fixture.CreateCards(6);
		const std::wstring sBody2 = Fixture.BodyOfRow(2);
		const std::string sCard2 = Fixture.CardOfRow(2);
		const std::string sCard4 = Fixture.CardOfRow(4);
		::SendMessageW(Fixture.CardListHwnd(), LB_SETCURSEL, 0, 0);
		::SetFocus(Fixture.CardListHwnd());

		send_wheel(Fixture.CardListHwnd(), -2 * WHEEL_TICK_ANGLE);
		{
			const bool bOpened = pump_until([&Fixture, &sBody2]()
				{ return(Fixture.Page().HasSession() && Fixture.EditorText() == sBody2); }, OPEN_TIMEOUT_MS);
			INFO("success open wait: session=" << Fixture.Page().HasSession()
				<< " pending=" << Fixture.Pending() << " current=" << Fixture.CurrentRow()
				<< " textlen=" << Fixture.EditorText().size() << " wantlen=" << sBody2.size()
				<< " match=" << (Fixture.EditorText() == sBody2)
				<< " remainder=" << Fixture.CardList().WheelAngleRemainder());
			REQUIRE(bOpened);
		}
		REQUIRE(::GetFocus() == Fixture.CardListHwnd());
		REQUIRE(Fixture.Selected() == std::vector<std::string>{ sCard2 });

		// 같은 카드 재열기(spec §3.4.4): 편집면이 든 카드로 되돌아온 채 정숙 시간이 만료되면
		// open_card 의 조기 반환이 걸린다. 원본은 그 경우에도 card_connected 를 다시 emit 해
		// (card_editor.py:192~196) reveal_card -> cancel_pending_browse 로 잔여 각을 0 으로 만든다.
		const std::string sCard3 = Fixture.CardOfRow(3);
		send_wheel(Fixture.CardListHwnd(), -WHEEL_TICK_ANGLE);
		REQUIRE(Fixture.CurrentRow() == 3);
		REQUIRE(Fixture.Pending() == sCard3);
		send_wheel(Fixture.CardListHwnd(), WHEEL_TICK_ANGLE);
		REQUIRE(Fixture.CurrentRow() == 2);
		REQUIRE(Fixture.Pending() == sCard2);
		// 마지막 입력이 한 틱을 못 채워 잔여 각을 남긴 채 재무장한다.
		send_wheel(Fixture.CardListHwnd(), -40);
		REQUIRE(Fixture.CardList().WheelAngleRemainder() == -40);
		{
			// 타이머 발화는 조기 반환의 취소와 **무관한** 관측이다(core OnTimer 가 대기 카드를
			// 먼저 비운다). 그것을 기다린 뒤 잔여 각을 동기로 본다 - 그래야 "취소가 있었는가" 를
			// 경합 없이 가른다. 계약이 지정한 pump_until 형태도 그대로 이어서 둔다.
			const bool bFired = pump_until([&Fixture]()
				{ return(Fixture.Pending() == "-"); }, OPEN_TIMEOUT_MS);
			INFO("re-open wait: session=" << Fixture.Page().HasSession()
				<< " pending=" << Fixture.Pending() << " current=" << Fixture.CurrentRow()
				<< " remainder=" << Fixture.CardList().WheelAngleRemainder()
				<< " match=" << (Fixture.EditorText() == sBody2));
			REQUIRE(bFired);
			REQUIRE(Fixture.CardList().WheelAngleRemainder() == 0);
			REQUIRE(pump_until([&Fixture]()
				{ return(Fixture.CardList().WheelAngleRemainder() == 0); }, OPEN_TIMEOUT_MS));
		}
		REQUIRE(Fixture.Page().HasSession());
		REQUIRE(Fixture.EditorText() == sBody2);
		REQUIRE(Fixture.CurrentRow() == 2);
		// 핸들러가 true 를 돌려받았으므로 포커스는 목록에 남는다.
		REQUIRE(::GetFocus() == Fixture.CardListHwnd());
		REQUIRE(Fixture.Pending() == "-");

		// 실패 경로: 더티 세션 + 이탈 거부. 편집면 서브클래스가 WM_CHAR 만 가로채므로
		// 더티는 실제 타건으로만 만든다(EM_REPLACESEL 은 synchronize_editor 에 닿지 않는다).
		Fixture.Type(L'x');
		REQUIRE(Fixture.Page().HasDirtySession());
		Fixture.LeaveChoice = C_DOCUMENT_PAGE::E_LEAVE_CHOICE::Cancel;
		::SetFocus(Fixture.CardListHwnd());

		send_wheel(Fixture.CardListHwnd(), -2 * WHEEL_TICK_ANGLE);
		REQUIRE(Fixture.CurrentRow() == 4);
		REQUIRE(Fixture.Pending() == sCard4);
		// 마지막 입력이 한 틱을 못 채우면 잔여 각이 남는다 - 복원의 취소가 그것을 0 으로 만든다.
		send_wheel(Fixture.CardListHwnd(), -40);
		REQUIRE(Fixture.CardList().WheelAngleRemainder() == -40);

		{
			const bool bRestored = pump_until([&Fixture]()
				{ return(Fixture.CurrentRow() == 2); }, OPEN_TIMEOUT_MS);
			INFO("restore wait: current=" << Fixture.CurrentRow()
				<< " pending=" << Fixture.Pending() << " session=" << Fixture.Page().HasSession()
				<< " dirty=" << Fixture.Page().HasDirtySession()
				<< " remainder=" << Fixture.CardList().WheelAngleRemainder());
			REQUIRE(bRestored);
		}
		REQUIRE(Fixture.Selected() == std::vector<std::string>{ sCard2 });
		REQUIRE(::GetFocus() == Fixture.EditorHwnd());
		REQUIRE(Fixture.Page().HasSession());
		REQUIRE(Fixture.EditorText() == sBody2 + L"x");
		REQUIRE(Fixture.Pending() == "-");
		REQUIRE(Fixture.CardList().WheelAngleRemainder() == 0);
		require_row_visible(Fixture.CardList(), 2);

		Fixture.CardList().CancelPendingBrowse();
	}

	// 컨트롤 절반 - core CompleteOpen 의 두 갈래를 열기 핸들러 반환값으로 직접 가른다.
	C_WHEEL_FIXTURE Fixture(12);
	Fixture.BrowseResult = false;
	Fixture.EditorCard = std::string("card-1");
	::SendMessageW(Fixture.Hwnd(), LB_SETCURSEL, 0, 0);
	send_wheel(Fixture.Hwnd(), -3 * WHEEL_TICK_ANGLE);
	send_wheel(Fixture.Hwnd(), -40);
	REQUIRE(Fixture.CurrentRow() == 3);
	REQUIRE(Fixture.Pending() == "card-4");
	REQUIRE(Fixture.Remainder() == -40);

	REQUIRE(pump_until([&Fixture]() { return(Fixture.Browsed().size() == 1); }, OPEN_TIMEOUT_MS));
	REQUIRE(Fixture.Selected() == std::vector<std::string>{ "card-1" });
	REQUIRE(Fixture.CurrentRow() == 0);
	REQUIRE(Fixture.AnchorRow() == 0);
	// 복원은 RevealRow(= 취소 + SetCurrentRow)라 잔여 각이 0 이 된다(원본 reveal_card 와 같다).
	REQUIRE(Fixture.Remainder() == 0);

	// 편집면에 카드가 없으면 원본도 reveal_card 에 닿지 않는다 - 선택도 잔여 각도 그대로다.
	Fixture.EditorCard.reset();
	send_wheel(Fixture.Hwnd(), -3 * WHEEL_TICK_ANGLE);
	send_wheel(Fixture.Hwnd(), -40);
	REQUIRE(Fixture.CurrentRow() == 3);
	REQUIRE(Fixture.Remainder() == -40);
	REQUIRE(pump_until([&Fixture]() { return(Fixture.Browsed().size() == 2); }, OPEN_TIMEOUT_MS));
	REQUIRE(Fixture.Selected() == std::vector<std::string>{ "card-4" });
	REQUIRE(Fixture.CurrentRow() == 3);
	REQUIRE(Fixture.Remainder() == -40);

	Fixture.Control().CancelPendingBrowse();
}
