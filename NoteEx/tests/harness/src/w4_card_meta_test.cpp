#include <catch_amalgamated.hpp>

#include "CCardList.h"
#include "CDocumentPage.h"
#include "Resource.h"
#include "pynote/harness/win32_harness.h"

// UIA(case 16)는 CCardList.h(ATL/WTL + ole2.h) 뒤, #undef CreateEvent 앞이다 - 제품
// CCardList.cpp 와 같은 순서 계약이다(spec §3.3.4).
#include <uiautomation.h>

// windows.h 의 CreateEvent 매크로가 repositories.h 의 멤버 이름을 바꾸기 전에 걷는다 -
// CDocumentPage.cpp·다른 W4 시험 TU 와 같은 순서 계약이어야 같은 바이너리 안에서 멤버
// 이름이 갈리지 않는다. ATL/WTL(CCardList.h)은 이 #undef 앞에서 읽어야 자기 ::CreateEvent
// 호출이 식별자를 잃지 않는다.
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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma comment(lib, "D2DWrapp")
#pragma comment(lib, "NoteExCore")
#pragma comment(lib, "Ole32")
// CUIAutomation/UiaReturnRawElementProvider 자리다(case 16, spec §3.3.4).
#pragma comment(lib, "uiautomationcore")

namespace
{
	namespace app = pynote::core::application;
	namespace domain = pynote::core::domain;
	namespace storage = pynote::core::storage;
	using pynote::harness::TestWindow;

	// 시험은 폰트를 명시 주입해 측정을 결정적으로 만든다(S1~S4 픽스처와 같은 값).
	constexpr wchar_t TEST_FONT_FAMILY[] = L"Segoe UI";
	constexpr float TEST_FONT_SIZE_DIP = 12.0f;

	// 한글은 narrow 리터럴로 쓰면 실행 문자셋(CP949)으로 접혀 UTF-8 계약이 깨진다 - 본문은
	// 전부 wide 리터럴에서 변환한다(p2_card_list_test.cpp:42 선례).
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

	std::wstring from_utf8(const std::string& _sValue)
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

	// 부록 A-5/A-10 류의 정확한 벡터를 손으로 못박기 위한 자리다 - position_key 와
	// capture_seq 를 창조 순서와 일부러 어긋나게 둔다(core 직결 픽스처 전용).
	domain::S_CARD vector_card(const std::string& _sId, std::int64_t _nPositionKey,
		std::int64_t _nCaptureSeq, std::int64_t _nUpdatedAtUs)
	{
		domain::S_CARD Card;
		Card.sId = _sId;
		Card.sDocumentId = "document-1";
		Card.sOperationId = "operation-1";
		Card.nPositionKey = _nPositionKey;
		Card.nCaptureSeq = _nCaptureSeq;
		Card.nCreatedAtUs = 1000000;
		Card.nUpdatedAtUs = _nUpdatedAtUs;
		Card.eSource = domain::E_CARD_SOURCE::Typing;
		Card.sBody = "body-" + _sId;
		return(Card);
	}

	std::vector<std::string> row_order(const domain::C_CARD_LIST_PROJECTION& _Projection)
	{
		std::vector<std::string> Result;
		for (std::size_t nRow = 0; nRow < _Projection.RowCount(); ++nRow)
		{
			Result.push_back(_Projection.CardAt(nRow)->sId);
		}
		return(Result);
	}

	std::vector<std::size_t> position_numbers(const domain::C_CARD_LIST_PROJECTION& _Projection)
	{
		std::vector<std::size_t> Result;
		for (std::size_t nRow = 0; nRow < _Projection.RowCount(); ++nRow)
		{
			const auto nPosition = _Projection.PositionNumber(_Projection.CardAt(nRow)->sId);
			Result.push_back(nPosition ? *nPosition : 0);
		}
		return(Result);
	}

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

	POINT to_screen(HWND _hWnd, POINT _ClientDip)
	{
		// DPI 비인식 프로세스라 DIP == 픽셀이다(다른 W4 픽스처와 같은 확인).
		POINT Point = _ClientDip;
		REQUIRE(::ClientToScreen(_hWnd, &Point) != FALSE);
		return(Point);
	}

	void send_wheel(HWND _hWnd, int _nDelta, WORD _nKeys = 0)
	{
		RECT Client{};
		REQUIRE(::GetClientRect(_hWnd, &Client) != FALSE);
		const POINT Centre = to_screen(_hWnd,
			POINT{ (Client.left + Client.right) / 2, (Client.top + Client.bottom) / 2 });
		::SendMessageW(_hWnd, WM_MOUSEWHEEL,
			MAKEWPARAM(_nKeys, static_cast<WORD>(static_cast<short>(_nDelta))), pack_point(Centre));
	}

	// 원본 _gesture(tests/ui/test_card_drag.py:132~137) - w4_card_dnd_test.cpp 의 같은
	// 이름 헬퍼와 같다(그 TU 는 무수정 계약이라 복제한다): 행 중앙 press -> 임계 초과 이동 ->
	// 릴리스.
	void gesture(HWND _hWnd, POINT _RowCentre)
	{
		const POINT End{ _RowCentre.x + CARD_DRAG_DISTANCE_DIP + 20, _RowCentre.y };
		send_press(_hWnd, _RowCentre);
		send_move(_hWnd, End);
		send_release(_hWnd, End);
	}

	// ---- 드롭 대상 구동(모달 루프 없음, w4_card_dnd_test.cpp 의 같은 이름 헬퍼와 같은 모양) ----
	struct S_DROP_OBSERVATION
	{
		DWORD nEnter{ DROPEFFECT_NONE };
		DWORD nOver{ DROPEFFECT_NONE };
		DWORD nDrop{ DROPEFFECT_NONE };

		bool AllMove() const noexcept
		{
			return(nEnter == DROPEFFECT_MOVE && nOver == DROPEFFECT_MOVE && nDrop == DROPEFFECT_MOVE);
		}
	};

	S_DROP_OBSERVATION drive_drop(IDropTarget* _pTarget, HWND _hWnd, IDataObject* _pData,
		POINT _ClientDip)
	{
		REQUIRE(_pTarget != nullptr);
		S_DROP_OBSERVATION Observation;
		const POINT Screen = to_screen(_hWnd, _ClientDip);
		const POINTL Where{ Screen.x, Screen.y };
		DWORD nEffect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
		REQUIRE(_pTarget->DragEnter(_pData, MK_LBUTTON, Where, &nEffect) == S_OK);
		Observation.nEnter = nEffect;
		nEffect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
		REQUIRE(_pTarget->DragOver(MK_LBUTTON, Where, &nEffect) == S_OK);
		Observation.nOver = nEffect;
		nEffect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
		REQUIRE(_pTarget->Drop(_pData, MK_LBUTTON, Where, &nEffect) == S_OK);
		Observation.nDrop = nEffect;
		return(Observation);
	}

	// 원본 CB_SETCURSEL 은 CBN_SELCHANGE 를 보내지 않는다(측정, investigation §3-6) - 시험이
	// 직접 WM_COMMAND 를 합성해야 한다(handoff.md 의 helper 설명 그대로, w4_card_dnd_test.cpp
	// 의 WM_COMMAND 합성 패턴과 같다).
	void send_combo_selection(HWND _hListHost, HWND _hCombo, UINT _nId, int _nIndex)
	{
		::SendMessageW(_hCombo, CB_SETCURSEL, static_cast<WPARAM>(_nIndex), 0);
		::SendMessageW(_hListHost, WM_COMMAND, MAKEWPARAM(_nId, CBN_SELCHANGE),
			reinterpret_cast<LPARAM>(_hCombo));
	}

	// TTM_GETTEXT 는 최대 80 문자까지 채운다(comctl32 계약) - 세 스트립 툴팁 전부 그 안에 든다.
	std::wstring read_tool_text(HWND _hTooltip, HWND _hOwner, HWND _hTarget)
	{
		wchar_t Buffer[256]{};
		TTTOOLINFOW Info{};
		// 클래식(비 v6) comctl32 는 sizeof(TTTOOLINFOW) 의 확장 필드를 모른다 - V1 크기여야
		// TTM_ADDTOOL 이 받아 준 도구를 TTM_GETTEXT 가 찾는다(제품 CDocumentPage.cpp 와 동일).
		Info.cbSize = TTTOOLINFOW_V1_SIZE;
		// TTF_IDISHWND 로 등록된 도구는 조회 쪽도 같은 플래그를 실어야 uId 가 HWND 로
		// 해석된다 - 없으면 comctl32 가 도구를 못 찾아 빈 문자열을 돌려준다.
		Info.uFlags = TTF_IDISHWND;
		Info.hwnd = _hOwner;
		Info.uId = reinterpret_cast<UINT_PTR>(_hTarget);
		Info.lpszText = Buffer;
		::SendMessageW(_hTooltip, TTM_GETTEXT, static_cast<WPARAM>(std::size(Buffer)),
			reinterpret_cast<LPARAM>(&Info));
		return(std::wstring(Buffer));
	}

	void send_hover(HWND _hList, POINT _Point)
	{
		::SendMessageW(_hList, WM_MOUSEMOVE, 0, pack_point(_Point));
		::SendMessageW(_hList, WM_MOUSEHOVER, 0, pack_point(_Point));
	}

	struct S_CHILD_INFO
	{
		UINT nId{ 0 };
		std::wstring sClassName;
	};

	// EnumChildWindows 는 콤보의 내부 팝업 리스트박스까지 재귀로 훑는다 - w4_card_list_test.cpp
	// 의 child_window_count() 와 같은 GW_CHILD/GW_HWNDNEXT 비재귀 순회로 직계 자식만 본다.
	std::vector<S_CHILD_INFO> direct_children(HWND _hParent)
	{
		std::vector<S_CHILD_INFO> Result;
		for (HWND hChild = ::GetWindow(_hParent, GW_CHILD); hChild != nullptr;
			hChild = ::GetWindow(hChild, GW_HWNDNEXT))
		{
			wchar_t ClassName[64]{};
			::GetClassNameW(hChild, ClassName, static_cast<int>(std::size(ClassName)));
			Result.push_back(S_CHILD_INFO{ static_cast<UINT>(::GetDlgCtrlID(hChild)), ClassName });
		}
		return(Result);
	}

	// 진짜 HWND + 진짜 D2DWrapp + 진짜 프로젝션 위의 컨트롤(S1 C_RENDER_FIXTURE/S2
	// C_SELECT_FIXTURE 모양). Position 정렬을 기본으로 둬 창조 순서 == 행 순서로
	// 결정적이게 한다(카드 순서 자체를 시험하는 case 3 은 SetSortMode 를 직접 다시 부른다).
	class C_META_CONTROL_FIXTURE
	{
	public:
		static constexpr int VIEW_DIP = 500;

		explicit C_META_CONTROL_FIXTURE(const std::vector<domain::S_CARD>& _Cards = {})
			: m_Host(pynote::harness::TestWindowOptions{ L"W4 meta control", 700, 700, true })
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
			RECT Frame{ 0, 0, VIEW_DIP, VIEW_DIP };
			REQUIRE(m_Control.Create(m_Host.hwnd(), Frame, nullptr,
				WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL, 0,
				static_cast<UINT>(IDC_DOCUMENT_CARD_LIST)) != nullptr);
			this->resize_client_(VIEW_DIP, VIEW_DIP);
			m_Projection.SetSortMode(domain::E_CARD_LIST_SORT_MODE::Position);
			if (!_Cards.empty()) { this->SetCards(_Cards); }
		}

		~C_META_CONTROL_FIXTURE()
		{
			if (::GetCapture() == m_Control.m_hWnd) { ::ReleaseCapture(); }
			if (m_Control.IsWindow()) { m_Control.DestroyWindow(); }
		}

		C_META_CONTROL_FIXTURE(const C_META_CONTROL_FIXTURE&) = delete;
		C_META_CONTROL_FIXTURE& operator=(const C_META_CONTROL_FIXTURE&) = delete;

		C_CARD_LIST& Control() noexcept { return(m_Control); }
		domain::C_CARD_LIST_PROJECTION& Projection() noexcept { return(m_Projection); }
		HWND Hwnd() const noexcept { return(m_Control.m_hWnd); }

		void SetCards(const std::vector<domain::S_CARD>& _Cards)
		{
			m_Projection.SetCards(_Cards);
			m_Control.OnProjectionChanged();
		}

		POINT Pt(std::size_t _nRow) const
		{
			const S_DIP_RECT Row = m_Control.RowRectDip(_nRow);
			return(POINT{ Row.nLeft + Row.nWidth / 2, Row.nTop + Row.nHeight / 2 });
		}

	private:
		void resize_client_(int _nWidthDip, int _nHeightDip)
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
	};

	// 진짜 DB·마이그레이션·서비스 위의 페이지(S2/S4 C_PAGE_FIXTURE 모양 - w4_card_dnd_test.cpp
	// 의 같은 이름 클래스를 이 슬라이스용으로 옮겨 온 것이다). eSortMode 는 Init 앞서
	// 영속 상태에 심어 둔다(케이스 2/4/7 이 정해진 정렬로 시작해야 한다).
	class C_META_PAGE_FIXTURE
	{
	public:
		explicit C_META_PAGE_FIXTURE(
			domain::E_CARD_LIST_SORT_MODE _eSortMode = domain::E_CARD_LIST_SORT_MODE::Recency)
			: m_Path(std::filesystem::temp_directory_path() /
				("NoteEx-W4S5-page-" + std::to_string(::GetCurrentProcessId()) + "-" +
				std::to_string(++s_nSequence) + ".db")), m_Repositories(m_Database),
			  m_DraftStore(m_Database, m_Repositories),
			  m_Parent(pynote::harness::TestWindowOptions{ L"W4 S5 page host", 1000, 780, true })
		{
			this->remove_();
			REQUIRE(m_Database.Open(m_Path.string()));
			storage::C_MIGRATION_RUNNER Runner;
			Runner.SetExistingDatabase(false, m_Path.string());
			REQUIRE(Runner.Run(m_Database) == storage::E_MIGRATE_RESULT::Ok);
			domain::S_DOCUMENT Document;
			Document.sId = DocumentId;
			Document.sTitle = "w4 s5 document";
			Document.nCreatedAtUs = 1000;
			Document.nUpdatedAtUs = 1000;
			REQUIRE(m_Repositories.CreateDocument(Document) == storage::E_REPO_RESULT::Ok);
			domain::S_WORKSPACE_WINDOW Workspace;
			REQUIRE(m_Repositories.SaveWorkspaceWindow(
				WorkspaceId, { DocumentId }, DocumentId, &Workspace) == storage::E_REPO_RESULT::Ok);
			// 정렬 모드는 페이지 밖에서 정할 수 없다 - Init 이 읽는 UI 상태에 미리 심는다
			// (w4_card_dnd_test.cpp 의 같은 패턴).
			app::C_WORKSPACE_STATE_STORE Store(m_Database, m_Repositories, WorkspaceId);
			app::S_DOCUMENT_UI_STATE UiState;
			UiState.sDocumentId = DocumentId;
			UiState.eSortMode = _eSortMode;
			UiState.nUpdatedAtUs = 1000;
			REQUIRE(Store.SaveDocumentUiState(UiState) == storage::E_REPO_RESULT::Ok);
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
			REQUIRE(this->create_page_());
		}

		~C_META_PAGE_FIXTURE()
		{
			m_Page.reset();
			m_Save.reset();
			m_Drafts.reset();
			m_CardService.reset();
			m_Database.Close();
			this->remove_();
		}

		C_META_PAGE_FIXTURE(const C_META_PAGE_FIXTURE&) = delete;
		C_META_PAGE_FIXTURE& operator=(const C_META_PAGE_FIXTURE&) = delete;

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

		std::string CreateCardWithSource(domain::E_CAPTURE_OPERATION_SOURCE _eSource,
			const std::wstring& _sBody)
		{
			domain::S_CARD Created;
			REQUIRE(m_CardService->CreateCard(DocumentId, to_utf8(_sBody), _eSource,
				std::nullopt, &Created) == app::E_CARD_SERVICE_RESULT::Ok);
			REQUIRE(m_Page->Refresh());
			return(Created.sId);
		}

		bool CardSoftDeleted(const std::string& _sCardId)
		{
			domain::S_CARD Card;
			if (m_Repositories.GetCard(_sCardId, &Card) != storage::E_REPO_RESULT::Ok) { return(false); }
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

		std::string CardId(std::size_t _nRow) const
		{
			const domain::S_CARD* pCard = m_Page->CardList().Projection()->CardAt(_nRow);
			REQUIRE(pCard != nullptr);
			return(pCard->sId);
		}

		std::size_t RowCount() const { return(m_Page->CardList().Projection()->RowCount()); }

		POINT RowPoint(std::size_t _nRow) const
		{
			const S_DIP_RECT Row = m_Page->CardList().RowRectDip(_nRow);
			return(POINT{ Row.nLeft + Row.nWidth / 2, Row.nTop + Row.nHeight / 2 });
		}

		POINT ZoneCentre() const
		{
			const S_DIP_RECT Zone = m_Page->CardList().DeleteZoneRectDip();
			return(POINT{ Zone.nLeft + Zone.nWidth / 2, Zone.nTop + Zone.nHeight / 2 });
		}

		C_DOCUMENT_PAGE& Page() { return(*m_Page); }
		C_CARD_LIST& List() const { return(m_Page->CardList()); }
		HWND ListHwnd() const { return(m_Page->CardListHwnd()); }
		HWND ListHostHwnd() const noexcept { return(m_hLeft); }
		storage::C_REPOSITORIES& Repositories() noexcept { return(m_Repositories); }

		bool ClosePage() { return(m_Page->Cleanup()); }
		// Cleanup() 뒤 같은 문서로 다시 Init 한다 - 원본 재채움 경로다(spec §3.1.5/CDocumentPage
		// ::Init 의 "재채움 경로" 주석 참조).
		bool ReInit() { return(this->create_page_()); }

		static constexpr int LIST_HOST_HEIGHT = 700;
		C_DOCUMENT_PAGE::E_LEAVE_CHOICE m_LeaveChoice{ C_DOCUMENT_PAGE::E_LEAVE_CHOICE::Save };
		C_DOCUMENT_PAGE::E_DRAG_DELETE_CHOICE m_DragDeleteChoice{
			C_DOCUMENT_PAGE::E_DRAG_DELETE_CHOICE::Cancel };
		bool m_bForbidPrompts{ false };
		inline static const std::string DocumentId = "document-w4s5";
		inline static const std::string WorkspaceId = "window-w4s5";

	private:
		bool create_page_()
		{
			if (!m_Page) { m_Page = std::make_unique<C_DOCUMENT_PAGE>(); }
			m_Page->SetRenderServices(&m_Device, &m_Brushes, &m_Text);
			S_CARD_LIST_DISPLAY Display;
			Display.Font.sFamily = TEST_FONT_FAMILY;
			Display.Font.fSizeDip = TEST_FONT_SIZE_DIP;
			m_Page->SetDisplaySettings(Display);
			m_Page->SetDragDeletePrompt([this](HWND)
				{
					if (m_bForbidPrompts) { FAIL("드래그 삭제 프롬프트가 떠서는 안 된다"); }
					return(m_DragDeleteChoice);
				});
			return(m_Page->Init(::GetModuleHandleW(nullptr), m_hLeft, m_hRight,
				m_Database, m_Repositories, *m_CardService, *m_Drafts, *m_Save,
				WorkspaceId, DocumentId, [this](HWND)
				{
					if (m_bForbidPrompts) { FAIL("이탈 프롬프트가 떠서는 안 된다"); }
					return(m_LeaveChoice);
				}));
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

	LRESULT CALLBACK count_selchange_subclass(HWND _hWnd, UINT _uMessage, WPARAM _wParam,
		LPARAM _lParam, UINT_PTR, DWORD_PTR _nReference)
	{
		if (_uMessage == WM_COMMAND && LOWORD(_wParam) == IDC_DOCUMENT_SORT_COMBO &&
			HIWORD(_wParam) == CBN_SELCHANGE)
		{
			++(*reinterpret_cast<int*>(_nReference));
		}
		return(::DefSubclassProc(_hWnd, _uMessage, _wParam, _lParam));
	}
}

TEST_CASE("PLAN-W4-0006 sort and source filter combos expose recency position capture and source choices while trash button and obsolete controls are absent",
	"[W4-meta][WTL-CAP-FI-056][WTL-CAP-FI-057]")
{
	// 원본 tests/ui/test_card_context_menu.py:355~392 (HEAD 좌표, spec §0).
	C_META_PAGE_FIXTURE Fixture;
	Fixture.CreateCards(1);
	C_DOCUMENT_PAGE& Page = Fixture.Page();

	// 정렬 콤보 - 항목 3개, 표시 순서 그대로.
	REQUIRE(::SendMessageW(Page.SortComboHwnd(), CB_GETCOUNT, 0, 0) == 3);
	const std::vector<std::wstring> ExpectedSort = { L"최근 활동순", L"현재 문서 순서", L"최초 기록 순서" };
	for (int nIndex = 0; nIndex < 3; ++nIndex)
	{
		wchar_t Buffer[64]{};
		::SendMessageW(Page.SortComboHwnd(), CB_GETLBTEXT, static_cast<WPARAM>(nIndex),
			reinterpret_cast<LPARAM>(Buffer));
		REQUIRE(std::wstring(Buffer) == ExpectedSort[static_cast<std::size_t>(nIndex)]);
	}
	REQUIRE(::SendMessageW(Page.SortComboHwnd(), CB_GETCURSEL, 0, 0) == 0);

	// 콤보/버튼 툴팁 - 축자.
	REQUIRE(read_tool_text(Page.TooltipHwnd(), Fixture.ListHostHwnd(), Page.SortComboHwnd()) ==
		L"카드를 최근 활동, 현재 문서 순서 또는 최초 기록 순서로 정렬");
	REQUIRE(read_tool_text(Page.TooltipHwnd(), Fixture.ListHostHwnd(), Page.SourceFilterHwnd()) ==
		L"선택한 입력 출처의 카드만 표시");
	REQUIRE(read_tool_text(Page.TooltipHwnd(), Fixture.ListHostHwnd(), Page.TrashButtonHwnd()) ==
		L"삭제한 카드를 확인하고 복구하거나 완전 삭제");

	// 출처 필터 콤보 - 항목 6개, 표시 순서 그대로.
	REQUIRE(::SendMessageW(Page.SourceFilterHwnd(), CB_GETCOUNT, 0, 0) == 6);
	const std::vector<std::wstring> ExpectedFilter =
		{ L"모든 출처", L"직접 입력", L"붙여넣기", L"혼합", L"가져오기", L"복구" };
	for (int nIndex = 0; nIndex < 6; ++nIndex)
	{
		wchar_t Buffer[64]{};
		::SendMessageW(Page.SourceFilterHwnd(), CB_GETLBTEXT, static_cast<WPARAM>(nIndex),
			reinterpret_cast<LPARAM>(Buffer));
		REQUIRE(std::wstring(Buffer) == ExpectedFilter[static_cast<std::size_t>(nIndex)]);
	}
	REQUIRE(::SendMessageW(Page.SourceFilterHwnd(), CB_GETCURSEL, 0, 0) == 0);

	// 휴지통 버튼 - 라벨.
	wchar_t TrashLabel[64]{};
	::GetWindowTextW(Page.TrashButtonHwnd(), TrashLabel, static_cast<int>(std::size(TrashLabel)));
	REQUIRE(std::wstring(TrashLabel) == L"카드 휴지통");

	// hListHost 의 직계 자식은 정확히 이 다섯 - 원본의 4개 폐지 위젯 부재 단언의 쌍둥이다
	// (findChild(...) is None 이 아니라 열거로 부재를 확인한다).
	const std::vector<S_CHILD_INFO> Children = direct_children(Fixture.ListHostHwnd());
	REQUIRE(Children.size() == 5);
	std::set<UINT> Ids;
	for (const S_CHILD_INFO& Child : Children) { Ids.insert(Child.nId); }
	REQUIRE(Ids == std::set<UINT>{ IDC_DOCUMENT_CARD_LIST, IDC_DOCUMENT_HISTORY,
		IDC_DOCUMENT_SORT_COMBO, IDC_DOCUMENT_SOURCE_FILTER, IDC_DOCUMENT_TRASH_BUTTON });
	for (const S_CHILD_INFO& Child : Children)
	{
		if (Child.nId == IDC_DOCUMENT_SORT_COMBO || Child.nId == IDC_DOCUMENT_SOURCE_FILTER)
		{
			REQUIRE(Child.sClassName == L"ComboBox");
		}
		else if (Child.nId == IDC_DOCUMENT_TRASH_BUTTON) { REQUIRE(Child.sClassName == L"Button"); }
	}
}

TEST_CASE("W4 sort combo selection change sets the core sort mode cancels pending browse and "
	"preserves scroll pixels", "[W4-meta][WTL-CAP-FI-056]")
{
	// spec §3.1.6/§3.1.8, 부록 A-1, canon §2-7/M19.
	C_META_PAGE_FIXTURE Fixture(domain::E_CARD_LIST_SORT_MODE::Position);
	Fixture.CreateCards(2);
	C_DOCUMENT_PAGE& Page = Fixture.Page();
	// 창조 순서에 의존하지 않는다 - 알려진 기준선으로 직접 잡는다.
	Page.CardList().SetCurrentRow(0);

	// 대기 중 휠 탐색을 무장한다(실제 WM_MOUSEWHEEL, position 정렬, 수식키 없음, 한 틱) -
	// 완결된 한 스텝은 §3.3.1 의 setCurrentIndex 부기까지 즉시 적용하므로(대기 열기 자체는
	// 타이머 발화까지 미룬다), "바뀌지 않는다" 의 기준선은 이 무장 *뒤* 값이다.
	send_wheel(Page.CardListHwnd(), -WHEEL_DELTA);
	REQUIRE(Page.CardList().PendingBrowseCardId().has_value());
	const std::string sCurrentBefore = *Page.CardList().Projection()->CurrentCardId();
	const std::vector<std::string> SelectedBefore = Page.CardList().Projection()->SelectedCardIds();
	const int nScrollBefore = Page.CardList().ScrollOffsetDip();

	// 실질 전이 - Capture(index 2).
	send_combo_selection(Fixture.ListHostHwnd(), Page.SortComboHwnd(), IDC_DOCUMENT_SORT_COMBO, 2);
	REQUIRE(Page.CardList().Projection()->SortMode() == domain::E_CARD_LIST_SORT_MODE::Capture);
	// 정렬 모드 전이는 보이는 카드 집합을 바꾸지 않는다 - 현재·선택은 그대로다.
	REQUIRE(Page.CardList().Projection()->CurrentCardId() == sCurrentBefore);
	REQUIRE(Page.CardList().Projection()->SelectedCardIds() == SelectedBefore);
	REQUIRE(Page.CardList().ScrollOffsetDip() == nScrollBefore);
	// 취소됐다.
	REQUIRE_FALSE(Page.CardList().PendingBrowseCardId().has_value());

	// 같은 값 재선택 - core 를 건드리기도 전에 페이지 수준 조기 반환이 선다. 관측 가능한
	// 유일한 증거는 대기 중 탐색이 취소되지 않는 것이다(spec §3.1.6). 무조건 단언한다 -
	// 재무장 자체가 이 배치에서 항상 성립한다(fix1 F5 — 감사 disc-3, 조건부면 재무장이
	// 실패하는 픽스처 변화에서 판별 없이 조용히 사라진다).
	send_wheel(Page.CardListHwnd(), -WHEEL_DELTA);
	REQUIRE(Page.CardList().PendingBrowseCardId().has_value());
	send_combo_selection(Fixture.ListHostHwnd(), Page.SortComboHwnd(), IDC_DOCUMENT_SORT_COMBO, 2);
	REQUIRE(Page.CardList().Projection()->SortMode() == domain::E_CARD_LIST_SORT_MODE::Capture);
	REQUIRE(Page.CardList().PendingBrowseCardId().has_value());
}

TEST_CASE("W4 sort combo modes reproduce the measured recency position and capture ordering "
	"including the tie break", "[W4-meta][WTL-CAP-FI-056]")
{
	// 부록 A-5, canon §2-10, test_recency_tie_uses_capture_sequence_descending.
	C_META_CONTROL_FIXTURE Fixture;
	Fixture.SetCards({
		vector_card("card-1", 5000, 1, 3000000),
		vector_card("card-2", 1000, 2, 5000000),
		vector_card("card-3", 4000, 3, 5000000),
		vector_card("card-4", 2000, 4, 1000000),
		vector_card("card-5", 3000, 5, 4000000) });

	Fixture.Projection().SetSortMode(domain::E_CARD_LIST_SORT_MODE::Recency);
	REQUIRE(row_order(Fixture.Projection()) ==
		std::vector<std::string>{ "card-3", "card-2", "card-5", "card-1", "card-4" });
	REQUIRE(position_numbers(Fixture.Projection()) == std::vector<std::size_t>{ 4, 1, 3, 5, 2 });

	Fixture.Projection().SetSortMode(domain::E_CARD_LIST_SORT_MODE::Position);
	REQUIRE(row_order(Fixture.Projection()) ==
		std::vector<std::string>{ "card-2", "card-4", "card-5", "card-3", "card-1" });
	REQUIRE(position_numbers(Fixture.Projection()) == std::vector<std::size_t>{ 1, 2, 3, 4, 5 });

	Fixture.Projection().SetSortMode(domain::E_CARD_LIST_SORT_MODE::Capture);
	REQUIRE(row_order(Fixture.Projection()) ==
		std::vector<std::string>{ "card-1", "card-2", "card-3", "card-4", "card-5" });
	REQUIRE(position_numbers(Fixture.Projection()) == std::vector<std::size_t>{ 5, 1, 4, 2, 3 });

	// 3장 완전 동률 - id/positionKey/captureSeq/updatedAt 이 셋 다 같은 position_key ·
	// 같은 updated_at, capture_seq 만 다르다.
	C_META_CONTROL_FIXTURE Tie;
	Tie.SetCards({
		vector_card("card-1", 1000, 1, 2000000),
		vector_card("card-2", 1000, 2, 2000000),
		vector_card("card-3", 1000, 3, 2000000) });

	Tie.Projection().SetSortMode(domain::E_CARD_LIST_SORT_MODE::Recency);
	// capture_seq 내림차순이다 - updated_at_us 만 뒤집고 capture_seq 는 오름차순으로 두는
	// 실수를 잡는 단언이다.
	REQUIRE(row_order(Tie.Projection()) == std::vector<std::string>{ "card-3", "card-2", "card-1" });

	Tie.Projection().SetSortMode(domain::E_CARD_LIST_SORT_MODE::Position);
	REQUIRE(row_order(Tie.Projection()) == std::vector<std::string>{ "card-1", "card-2", "card-3" });

	Tie.Projection().SetSortMode(domain::E_CARD_LIST_SORT_MODE::Capture);
	REQUIRE(row_order(Tie.Projection()) == std::vector<std::string>{ "card-1", "card-2", "card-3" });
}

TEST_CASE("W4 source filter selection change maps six items to core source sets without moving "
	"position numbers", "[W4-meta][WTL-CAP-FI-057]")
{
	// tests/ui/test_card_context_menu.py 의 필터 절반 + canon §2-8, 부록 A-4. Capture 정렬,
	// 부록 A-5 와 같은 벡터(위치·기록 순번이 정확히 겹쳐야 하는 표라 창조 순서로는
	// 재현할 수 없다) - 저장소에 직접 꽂는다(카드 서비스는 늘 창조 순서로 위치를 매긴다).
	C_META_PAGE_FIXTURE Fixture(domain::E_CARD_LIST_SORT_MODE::Capture);
	C_DOCUMENT_PAGE& Page = Fixture.Page();

	domain::S_CAPTURE_OPERATION Operation;
	Operation.sId = "operation-fixture";
	Operation.sDocumentId = C_META_PAGE_FIXTURE::DocumentId;
	Operation.eSource = domain::E_CAPTURE_OPERATION_SOURCE::Typing;
	Operation.nCreatedAtUs = 1000;
	REQUIRE(Fixture.Repositories().CreateCaptureOperation(Operation) == storage::E_REPO_RESULT::Ok);

	auto insert = [&Fixture](const std::string& _sId, std::int64_t _nPositionKey,
		std::int64_t _nCaptureSeq, domain::E_CARD_SOURCE _eSource)
	{
		domain::S_CARD Card = vector_card(_sId, _nPositionKey, _nCaptureSeq, 1000000);
		Card.sDocumentId = C_META_PAGE_FIXTURE::DocumentId;
		Card.sOperationId = "operation-fixture";
		Card.eSource = _eSource;
		Card.sBodyHash = storage::TextHash(Card.sBody);
		REQUIRE(Fixture.Repositories().CreateCard(Card) == storage::E_REPO_RESULT::Ok);
	};
	insert("card-1", 5000, 1, domain::E_CARD_SOURCE::Typing);
	insert("card-2", 1000, 2, domain::E_CARD_SOURCE::Typing);
	insert("card-3", 4000, 3, domain::E_CARD_SOURCE::Paste);
	insert("card-4", 2000, 4, domain::E_CARD_SOURCE::Import);
	insert("card-5", 3000, 5, domain::E_CARD_SOURCE::Paste);
	REQUIRE(Page.Refresh());

	// 필터 없음(index 0).
	send_combo_selection(Fixture.ListHostHwnd(), Page.SourceFilterHwnd(),
		IDC_DOCUMENT_SOURCE_FILTER, 0);
	REQUIRE(row_order(*Page.CardList().Projection()) ==
		std::vector<std::string>{ "card-1", "card-2", "card-3", "card-4", "card-5" });
	REQUIRE(position_numbers(*Page.CardList().Projection()) ==
		std::vector<std::size_t>{ 5, 1, 4, 2, 3 });
	REQUIRE_FALSE(Page.SourceFilter().has_value());

	// 붙여넣기만(index 2).
	send_combo_selection(Fixture.ListHostHwnd(), Page.SourceFilterHwnd(),
		IDC_DOCUMENT_SOURCE_FILTER, 2);
	REQUIRE(row_order(*Page.CardList().Projection()) == std::vector<std::string>{ "card-3", "card-5" });
	REQUIRE(position_numbers(*Page.CardList().Projection()) == std::vector<std::size_t>{ 4, 3 });
	REQUIRE(Page.SourceFilter() == domain::E_CARD_SOURCE::Paste);

	// 두 출처(붙여넣기+가져오기) - 콤보로는 못 닿는 core 직결 교차검증이다(콤보는 한 번에
	// 출처 하나만 고른다). Page.CardList().Projection() 은 const 라 여기서는 같은 5장
	// 벡터의 독립 core 픽스처로 SetSourceFilter 를 직접 부른다.
	{
		C_META_CONTROL_FIXTURE Cross;
		domain::S_CARD Card1 = vector_card("card-1", 5000, 1, 1000000);
		Card1.eSource = domain::E_CARD_SOURCE::Typing;
		domain::S_CARD Card2 = vector_card("card-2", 1000, 2, 1000000);
		Card2.eSource = domain::E_CARD_SOURCE::Typing;
		domain::S_CARD Card3 = vector_card("card-3", 4000, 3, 1000000);
		Card3.eSource = domain::E_CARD_SOURCE::Paste;
		domain::S_CARD Card4 = vector_card("card-4", 2000, 4, 1000000);
		Card4.eSource = domain::E_CARD_SOURCE::Import;
		domain::S_CARD Card5 = vector_card("card-5", 3000, 5, 1000000);
		Card5.eSource = domain::E_CARD_SOURCE::Paste;
		Cross.SetCards({ Card1, Card2, Card3, Card4, Card5 });
		Cross.Projection().SetSortMode(domain::E_CARD_LIST_SORT_MODE::Capture);
		Cross.Projection().SetSourceFilter(
			std::set<domain::E_CARD_SOURCE>{ domain::E_CARD_SOURCE::Paste, domain::E_CARD_SOURCE::Import });
		REQUIRE(row_order(Cross.Projection()) ==
			std::vector<std::string>{ "card-3", "card-4", "card-5" });
		REQUIRE(position_numbers(Cross.Projection()) == std::vector<std::size_t>{ 4, 2, 3 });
	}

	// 혼합만(index 3) - 아무도 없다.
	send_combo_selection(Fixture.ListHostHwnd(), Page.SourceFilterHwnd(),
		IDC_DOCUMENT_SOURCE_FILTER, 3);
	REQUIRE(Page.CardList().Projection()->RowCount() == 0);
	REQUIRE(Page.SourceFilter() == domain::E_CARD_SOURCE::Mixed);
}

TEST_CASE("W4 reveal card does not clear an active source filter or move the current card",
	"[W4-meta][WTL-CAP-NC-011]")
{
	// canon §2-9, 부록 A-12.
	C_META_PAGE_FIXTURE Fixture;
	const std::string sTyping = Fixture.CreateCardWithSource(
		domain::E_CAPTURE_OPERATION_SOURCE::Typing, L"타이핑 카드");
	const std::string sPaste = Fixture.CreateCardWithSource(
		domain::E_CAPTURE_OPERATION_SOURCE::Paste, L"붙여넣기 카드");
	C_DOCUMENT_PAGE& Page = Fixture.Page();

	Page.SetSourceFilter(domain::E_CARD_SOURCE::Paste);
	REQUIRE(Page.SourceFilter() == domain::E_CARD_SOURCE::Paste);
	const std::size_t nRowCountBefore = Page.CardList().Projection()->RowCount();
	const std::optional<std::string> sCurrentBefore = Page.CardList().Projection()->CurrentCardId();

	// 가려진 카드 - 아무것도 바뀌지 않는다.
	REQUIRE_FALSE(Page.RevealCard(sTyping));
	REQUIRE(Page.SourceFilter() == domain::E_CARD_SOURCE::Paste);
	REQUIRE(Page.CardList().Projection()->RowCount() == nRowCountBefore);
	REQUIRE(Page.CardList().Projection()->CurrentCardId() == sCurrentBefore);

	// 보이는 카드 - 성공하고 현재가 옮겨간다.
	REQUIRE(Page.RevealCard(sPaste));
	REQUIRE(Page.CardList().Projection()->CurrentCardId() == sPaste);
}

TEST_CASE("W4 history mode hides the sort filter and trash strip and restores them on return to "
	"the card list", "[W4-meta]")
{
	// spec §3.1.4, investigation risk 2.
	C_META_PAGE_FIXTURE Fixture;
	Fixture.CreateCards(1);
	C_DOCUMENT_PAGE& Page = Fixture.Page();

	REQUIRE(::IsWindowVisible(Page.SortComboHwnd()));
	REQUIRE(::IsWindowVisible(Page.SourceFilterHwnd()));
	REQUIRE(::IsWindowVisible(Page.TrashButtonHwnd()));
	REQUIRE(::IsWindowVisible(Page.CardListHwnd()));
	REQUIRE_FALSE(::IsWindowVisible(Page.HistoryHwnd()));

	Page.ShowHistory();
	REQUIRE_FALSE(::IsWindowVisible(Page.SortComboHwnd()));
	REQUIRE_FALSE(::IsWindowVisible(Page.SourceFilterHwnd()));
	REQUIRE_FALSE(::IsWindowVisible(Page.TrashButtonHwnd()));
	REQUIRE(::IsWindowVisible(Page.HistoryHwnd()));
	REQUIRE_FALSE(::IsWindowVisible(Page.CardListHwnd()));

	Page.FocusCardList();
	REQUIRE(::IsWindowVisible(Page.SortComboHwnd()));
	REQUIRE(::IsWindowVisible(Page.SourceFilterHwnd()));
	REQUIRE(::IsWindowVisible(Page.TrashButtonHwnd()));
	REQUIRE(::IsWindowVisible(Page.CardListHwnd()));
	REQUIRE_FALSE(::IsWindowVisible(Page.HistoryHwnd()));

	// 스트립 기하 - 논리 관계만 단언한다(fix1 F9 — 감사 omission-2, spec §3.1.3). 목록
	// 창의 top 이 정렬 콤보의 top 보다 아래에 있어야 스트립이 실제로 위쪽 공간을 차지한다.
	RECT ComboRect{};
	RECT ListRect{};
	REQUIRE(::GetWindowRect(Page.SortComboHwnd(), &ComboRect) != FALSE);
	REQUIRE(::GetWindowRect(Page.CardListHwnd(), &ListRect) != FALSE);
	REQUIRE(ListRect.top > ComboRect.top);
	REQUIRE(::SendMessageW(Page.SourceFilterHwnd(), CB_GETCOUNT, 0, 0) == 6);
	RECT Dropped{};
	::SendMessageW(Page.SourceFilterHwnd(), CB_GETDROPPEDCONTROLRECT, 0,
		reinterpret_cast<LPARAM>(&Dropped));
	RECT SourceFilterRect{};
	REQUIRE(::GetWindowRect(Page.SourceFilterHwnd(), &SourceFilterRect) != FALSE);
	// 열린 목록이 항목 6개를 담을 수 있어야 한다 - Layout() 이 닫힌 상자 높이로 다시
	// 눌러 드롭다운을 자르는 회귀의 게이트다(fix1 F1/F9 — 감사 parity-1).
	REQUIRE((Dropped.bottom - Dropped.top) >
		(SourceFilterRect.bottom - SourceFilterRect.top) * 3);
}

TEST_CASE("W4 persisted sort mode restores the combo display without emitting a selection change",
	"[W4-meta]")
{
	// 네이티브 CDocumentPage.cpp Init 의 정렬 복원 자리, investigation §2-6/§3-6.
	C_META_PAGE_FIXTURE Fixture;
	Fixture.CreateCards(1);
	C_DOCUMENT_PAGE& Page = Fixture.Page();
	Page.SetSortMode(domain::E_CARD_LIST_SORT_MODE::Capture);
	REQUIRE(Page.PersistState(std::nullopt));
	REQUIRE(Fixture.ClosePage());

	int nSelChangeCount = 0;
	REQUIRE(::SetWindowSubclass(Fixture.ListHostHwnd(), &count_selchange_subclass, 3,
		reinterpret_cast<DWORD_PTR>(&nSelChangeCount)) != FALSE);
	REQUIRE(Fixture.ReInit());
	::RemoveWindowSubclass(Fixture.ListHostHwnd(), &count_selchange_subclass, 3);

	REQUIRE(::SendMessageW(Page.SortComboHwnd(), CB_GETCURSEL, 0, 0) == 2);
	REQUIRE(Page.CardList().Projection()->SortMode() == domain::E_CARD_LIST_SORT_MODE::Capture);
	REQUIRE(nSelChangeCount == 0);
	// CB_SETCURSEL 단독으로는 페이지 진입점이 돌지 않는다 - 카운터는 페이지 서브클래스가
	// WM_COMMAND 를 먼저 삼키면 못 보므로(둘 다 id 2 위에 서고 나중 설치가 먼저 불린다),
	// 서브클래스 순서에 기대지 않고 모드 자체의 불변으로 비재귀를 판별한다(fix1 F4 —
	// 감사 disc-2, spec §3.1.9).
	::SendMessageW(Page.SortComboHwnd(), CB_SETCURSEL, 0, 0);
	REQUIRE(Page.CardList().Projection()->SortMode() == domain::E_CARD_LIST_SORT_MODE::Capture);
}

TEST_CASE("W4 pointer hover over a row requests the tooltip text exactly once and hides it on "
	"every exit path", "[W4-meta]")
{
	// spec §3.2.5~6, canon §3-6.
	C_META_CONTROL_FIXTURE Fixture({ make_card(1, "one"), make_card(2, "two"), make_card(3, "three") });
	C_CARD_LIST& List = Fixture.Control();
	const HWND hList = Fixture.Hwnd();
	std::vector<std::pair<std::size_t, std::wstring>> Shown;
	int nHidden = 0;
	List.SetTooltipShower([&Shown](std::size_t _nRow, const std::wstring& _sText, POINT)
		{ Shown.push_back({ _nRow, _sText }); });
	List.SetTooltipHider([&nHidden]() { ++nHidden; });

	// row0 진입(기존 가드 무장) -> WM_MOUSEHOVER 직접.
	::SendMessageW(hList, WM_MOUSEMOVE, 0, pack_point(Fixture.Pt(0)));
	REQUIRE(List.HoverArmCount() == 1);
	::SendMessageW(hList, WM_MOUSEHOVER, 0, pack_point(Fixture.Pt(0)));
	REQUIRE(Shown.size() == 1);
	REQUIRE(Shown.back().first == 0);
	REQUIRE(Shown.back().second == List.TooltipTextForRow(0));
	// TME_HOVER 는 1회성이다 - OnMouseHover 의 무조건 재무장이 카운트를 2로 올린다.
	REQUIRE(List.HoverArmCount() == 2);

	// row1 로 이동 후 다시 hover.
	::SendMessageW(hList, WM_MOUSEMOVE, 0, pack_point(Fixture.Pt(1)));
	::SendMessageW(hList, WM_MOUSEHOVER, 0, pack_point(Fixture.Pt(1)));
	REQUIRE(Shown.size() == 2);
	REQUIRE(Shown.back().first == 1);
	REQUIRE(List.HoverArmCount() == 3);

	const int nHiddenBeforeLeave = nHidden;
	::SendMessageW(hList, WM_MOUSELEAVE, 0, 0);
	REQUIRE(nHidden > nHiddenBeforeLeave);

	// row0 재진입(정상 OnMouseMove 경로) -> hover -> 좌클릭 press(숨김 트리거).
	::SendMessageW(hList, WM_MOUSEMOVE, 0, pack_point(Fixture.Pt(0)));
	::SendMessageW(hList, WM_MOUSEHOVER, 0, pack_point(Fixture.Pt(0)));
	const int nHiddenBeforePress = nHidden;
	send_press(hList, Fixture.Pt(0));
	REQUIRE(nHidden > nHiddenBeforePress);
	send_release(hList, Fixture.Pt(0));

	// 휠 - 숨김 트리거.
	send_hover(hList, Fixture.Pt(0));
	const int nHiddenBeforeWheel = nHidden;
	::SendMessageW(hList, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)),
		pack_point(Fixture.Pt(0)));
	REQUIRE(nHidden > nHiddenBeforeWheel);

	// OnProjectionChanged - 숨김 트리거.
	const int nHiddenBeforeReset = nHidden;
	Fixture.Projection().SetCards({ make_card(1, "one") });
	List.OnProjectionChanged();
	REQUIRE(nHidden > nHiddenBeforeReset);

	// OnDestroy - 숨김 트리거(창을 직접 부순다, 여섯 트리거 마지막).
	const int nHiddenBeforeDestroy = nHidden;
	List.DestroyWindow();
	REQUIRE(nHidden > nHiddenBeforeDestroy);

	// 샤워 미설치 - 조립 자체가 안 일어나지만 조회는 여전히 가능하고 죽지 않는다.
	C_META_CONTROL_FIXTURE Bare({ make_card(1, "solo") });
	::SendMessageW(Bare.Hwnd(), WM_MOUSEMOVE, 0, pack_point(Bare.Pt(0)));
	::SendMessageW(Bare.Hwnd(), WM_MOUSEHOVER, 0, pack_point(Bare.Pt(0)));
	REQUIRE_FALSE(Bare.Control().TooltipTextForRow(0).empty());
}

TEST_CASE("PLAN-W4-0048 card tooltip preserves removed metadata and the reconstruction marker",
	"[W4-meta][WTL-CAP-FI-059][WTL-CAP-RE-010]")
{
	// tests/ui/test_card_stream.py:892 인접, canon §3-1/§3-2/§3-3.
	domain::S_CARD Card1 = make_card(1, "body one");
	Card1.eSource = domain::E_CARD_SOURCE::Typing;
	domain::S_CARD Card2 = make_card(2, "body two");
	C_META_CONTROL_FIXTURE Fixture({ Card1, Card2 });
	C_CARD_LIST& List = Fixture.Control();
	List.SetRevisionCounts({ { Card1.sId, 4 } });
	List.SetReconstructionUnavailableIds({ Card1.sId });

	const std::size_t nRow1 = *Fixture.Projection().RowForCard(Card1.sId);
	const std::size_t nRow2 = *Fixture.Projection().RowForCard(Card2.sId);
	// 부록 A-6 의 고정 리터럴 그대로다 - 구현값을 보간하지 않는다(fix1 F7 — 감사
	// disc-5: 이 픽스처에서 위치·기록 번호가 어긋나는 결함도 보간이면 통과해 버린다).
	// 두 값 모두 이 픽스처(Position 정렬, Card1 이 첫 카드)에서 1 이다.
	REQUIRE(*Fixture.Projection().PositionNumber(Card1.sId) == 1);
	REQUIRE(Card1.nCaptureSeq == 1);
	const std::wstring sExpected =
		L"위치 1은 문서 안의 현재 순서이며 현재 문서 순서 보기에서 이동할 수 있습니다.\n"
		L"기록 #1은 최초 생성 순서이며 바뀌지 않습니다.\n"
		L"출처 직접 입력\n"
		L"최초 기록 " + List.TimeLabel(Card1.nCreatedAtUs) + L"\n"
		L"리비전 4개\n"
		L"수정 " + List.TimeLabel(Card1.nUpdatedAtUs) +
		L"\n형제 카드 purge로 작업 원문 재구성 불가";
	REQUIRE(List.TooltipTextForRow(nRow1) == sExpected);

	// 캐시에 없는 카드는 리비전 1개(기본값, card_model.py:141) - 7번째 줄 없음.
	const std::wstring sSecond = List.TooltipTextForRow(nRow2);
	REQUIRE(sSecond.find(L"리비전 1개") != std::wstring::npos);
	REQUIRE(sSecond.find(L"purge") == std::wstring::npos);

	// 행 자체 페인트는 영향받지 않는다(CAP-RE-010 의 "압축 카드에는 안 그린다" 절반).
	REQUIRE(List.Render());
	REQUIRE(List.LastFrame().Rows.size() >= 1);
	for (const S_CARD_LIST_ROW_FRAME& Row : List.LastFrame().Rows)
	{
		for (const S_CARD_TEXT_RUN& Run : Row.BodyLines) { REQUIRE(Run.sText.find(L"purge") == std::wstring::npos); }
		REQUIRE(Row.SuffixRun.sText.find(L"purge") == std::wstring::npos);
	}
}

TEST_CASE("PLAN-W4-0049 time display settings apply to the card row and the tooltip together",
	"[W4-meta][WTL-CAP-FI-059]")
{
	// tests/ui/test_card_stream.py 의 시간 표시 사례, canon §3-5/부록 A-8.
	domain::S_CARD Card = make_card(1, "time card");
	Card.nCreatedAtUs = 1788657825000000;
	Card.nUpdatedAtUs = 1788678489000000;
	C_META_CONTROL_FIXTURE Fixture({ Card });
	C_CARD_LIST& List = Fixture.Control();
	const std::size_t nRow = *Fixture.Projection().RowForCard(Card.sId);

	struct S_TIME_ROW { const wchar_t* pFormat; const wchar_t* pZone; const wchar_t* pCreated; const wchar_t* pUpdated; };
	const S_TIME_ROW Rows[] = {
		{ L"yyyy-MM-dd HH:mm:ss", L"system", L"2026-09-06 10:23:45", L"2026-09-06 16:08:09" },
		{ L"yyyy-MM-dd HH:mm", L"system", L"2026-09-06 10:23", L"2026-09-06 16:08" },
		{ L"yyyy-MM-dd HH:mm:ss", L"UTC", L"2026-09-06 01:23:45", L"2026-09-06 07:08:09" },
		{ L"yyyy년 M월 d일 AP h:mm", L"Asia/Seoul", L"2026년 9월 6일 AM 10:23", L"2026년 9월 6일 PM 4:08" },
		{ L"yyyy", L"UTC", L"2026", L"2026" },
		{ L"yyyy-MM-dd HH:mm:ss", L"Not/AZone", L"2026-09-06 10:23:45", L"2026-09-06 16:08:09" },
	};
	for (const S_TIME_ROW& Row : Rows)
	{
		S_CARD_LIST_DISPLAY Display;
		Display.sTimeFormat = Row.pFormat;
		Display.sTimeZone = Row.pZone;
		Display.Font.sFamily = TEST_FONT_FAMILY;
		Display.Font.fSizeDip = TEST_FONT_SIZE_DIP;
		List.SetDisplaySettings(Display);
		REQUIRE(List.Render());
		REQUIRE_FALSE(List.LastFrame().Rows.empty());
		const std::wstring& sRowTime = List.LastFrame().Rows[0].TimeRun.sText;
		REQUIRE(sRowTime == Row.pUpdated);
		const std::wstring sTooltip = List.TooltipTextForRow(nRow);
		REQUIRE(sTooltip.find(std::wstring(L"최초 기록 ") + Row.pCreated) != std::wstring::npos);
		REQUIRE(sTooltip.find(std::wstring(L"수정 ") + Row.pUpdated) != std::wstring::npos);
	}
}

TEST_CASE("W4 tooltip text is generated on request and reflects the latest revision count without "
	"caching", "[W4-meta][WTL-CAP-FI-059][WTL-CAP-RE-010]")
{
	// canon §5-1/M23, [UNCERTAIN] 4 resolved.
	domain::S_CARD Card = make_card(1, "revision card");
	C_META_CONTROL_FIXTURE Fixture({ Card });
	C_CARD_LIST& List = Fixture.Control();
	const std::size_t nRow = *Fixture.Projection().RowForCard(Card.sId);

	List.SetRevisionCounts({ { Card.sId, 2 } });
	REQUIRE(List.TooltipTextForRow(nRow).find(L"리비전 2개") != std::wstring::npos);
	List.SetRevisionCounts({ { Card.sId, 9 } });
	REQUIRE(List.TooltipTextForRow(nRow).find(L"리비전 9개") != std::wstring::npos);
}

TEST_CASE("PLAN-W4-0050 reconstruction warning change invalidates size hint", "[W4-meta][WTL-CAP-NC-010]")
{
	// 부록 A-10.
	domain::S_CARD Card1 = make_card(1, "one");
	domain::S_CARD Card2 = make_card(2, "two");
	C_META_CONTROL_FIXTURE Fixture({ Card1, Card2 });
	C_CARD_LIST& List = Fixture.Control();
	REQUIRE(List.Render());
	const int nRowHeightBefore = List.RowHeightDip();

	List.ClearInvalidationLog();
	List.SetReconstructionUnavailableIds({ Card1.sId });
	REQUIRE(List.InvalidationLog().size() == 1);
	REQUIRE(List.InvalidationLog()[0] == S_CARD_INVALIDATION_ENTRY{ 0, 1,
		{ E_CARD_INVALIDATION_ROLE::Reconstruction, E_CARD_INVALIDATION_ROLE::Tooltip,
			E_CARD_INVALIDATION_ROLE::SizeHint } });

	// 같은 집합 재호출 - 무조건 재기록(diff 없음).
	List.SetReconstructionUnavailableIds({ Card1.sId });
	REQUIRE(List.InvalidationLog().size() == 2);

	// 빈 모델 - 로그 항목 0.
	Fixture.SetCards({});
	List.ClearInvalidationLog();
	List.SetReconstructionUnavailableIds({ Card1.sId });
	REQUIRE(List.InvalidationLog().empty());

	REQUIRE(List.Render());
	REQUIRE(List.RowHeightDip() == nRowHeightBefore);
}

TEST_CASE("W4 invalidation log records the display settings dirty draft and reset entries per the "
	"decision table", "[W4-meta][WTL-CAP-NC-010]")
{
	// spec §3.4.4 전수.
	domain::S_CARD Card1 = make_card(1, "one");
	domain::S_CARD Card2 = make_card(2, "two");
	C_META_CONTROL_FIXTURE Fixture({ Card1, Card2 });
	C_CARD_LIST& List = Fixture.Control();
	domain::C_CARD_LIST_PROJECTION& Projection = Fixture.Projection();
	const std::size_t nRow1 = *Projection.RowForCard(Card1.sId);

	List.ClearInvalidationLog();
	S_CARD_LIST_DISPLAY Display;
	Display.Font.sFamily = TEST_FONT_FAMILY;
	Display.Font.fSizeDip = TEST_FONT_SIZE_DIP;
	List.SetDisplaySettings(Display);
	REQUIRE(List.InvalidationLog().size() == 1);
	REQUIRE(List.InvalidationLog()[0] ==
		S_CARD_INVALIDATION_ENTRY{ 0, 1, { E_CARD_INVALIDATION_ROLE::Tooltip } });

	Fixture.SetCards({});
	List.ClearInvalidationLog();
	List.SetDisplaySettings(Display);
	REQUIRE(List.InvalidationLog().empty());
	Fixture.SetCards({ Card1, Card2 });
	List.ClearInvalidationLog();

	// 더티마커 전이, 가드 있음 - NotifyCardDirtyChanged 자신은 무조건 재도장한다(그래서
	// 행이 있으면 항상 로그에 오른다). 값 변화 가드는 *호출부* 몫이다(spec §3.4.4) - 페이지의
	// 가드 있는 호출 자리를 여기서 그대로 흉내 낸다.
	REQUIRE_FALSE(Projection.IsCardDirty(Card1.sId));
	const std::vector<std::string> SelectedBefore = Projection.SelectedCardIds();
	{
		const bool bWasDirty = Projection.IsCardDirty(Card1.sId);
		Projection.SetCardDirty(Card1.sId, true);
		if (bWasDirty != true) { List.NotifyCardDirtyChanged(Card1.sId); }
	}
	REQUIRE(List.InvalidationLog().size() == 1);
	REQUIRE(List.InvalidationLog()[0] ==
		S_CARD_INVALIDATION_ENTRY{ nRow1, nRow1, { E_CARD_INVALIDATION_ROLE::DirtyDraft } });
	REQUIRE(Projection.SelectedCardIds() == SelectedBefore);

	// 같은 값 재호출 - 가드 자체의 회귀 검사: 값이 안 바뀌었으니 통지 자체를 안 부른다,
	// 추가 항목 없음. 가드 없이 무조건 불렀다면(버그) 여기서 항목이 하나 더 늘었을 것이다.
	{
		const bool bWasDirty = Projection.IsCardDirty(Card1.sId);
		Projection.SetCardDirty(Card1.sId, true);
		if (bWasDirty != true) { List.NotifyCardDirtyChanged(Card1.sId); }
	}
	REQUIRE(List.InvalidationLog().size() == 1);

	// 행이 없는 카드 - 크래시 없이 항목도 없다.
	List.NotifyCardDirtyChanged("card-does-not-exist");
	REQUIRE(List.InvalidationLog().size() == 1);

	// 리셋은 dataChanged 가 아니다 - 로그는 그대로다.
	List.OnProjectionChanged();
	REQUIRE(List.InvalidationLog().size() == 1);

	// 호버 행 변경 - 재도장은 실제로 일어나지만(다른 행으로 이동) 로그는 늘지 않는다.
	// 무효 영역을 비운 뒤 이동시켜 재도장이 정말 일어났음을 GetUpdateRect 로 직접
	// 확인한다(fix1 F6 — 감사 disc-4: 로그 불변만으로는 invalidate_row_ 자체가 조기
	// 반환하도록 잘못 고쳐지는 회귀도 통과해 버린다).
	const std::size_t nSizeBeforeHover = List.InvalidationLog().size();
	::SendMessageW(Fixture.Hwnd(), WM_MOUSEMOVE, 0, pack_point(Fixture.Pt(0)));
	::ValidateRect(Fixture.Hwnd(), nullptr);
	::SendMessageW(Fixture.Hwnd(), WM_MOUSEMOVE, 0, pack_point(Fixture.Pt(1)));
	RECT Update{};
	REQUIRE(::GetUpdateRect(Fixture.Hwnd(), &Update, FALSE) != FALSE);
	REQUIRE(List.InvalidationLog().size() == nSizeBeforeHover);

	// 페이지 호출부 가드의 회귀 검사 - 같은 카드에 두 번 입력해도 로그는 한 번만
	// 는다(fix1 F3 — 감사 disc-1: 이 TU 의 다른 단언은 전부 컨트롤 픽스처에서 가드를
	// 직접 흉내 내며, 제품 CDocumentPage.cpp 의 synchronize_editor 가드 자체를 편집기
	// 입력으로 몰아 부르는 케이스가 없었다). 가드를 지우면 두 번째 REQUIRE 가 첫 값보다
	// 큰 값을 보고 깨진다(spec §3.4.4).
	{
		C_META_PAGE_FIXTURE Guard;
		Guard.CreateCards(1);
		send_click(Guard.ListHwnd(), Guard.RowPoint(0));
		pynote::harness::drain_messages();
		REQUIRE(Guard.Page().HasSession());
		Guard.List().ClearInvalidationLog();
		::SendMessageW(Guard.Page().EditorHwnd(), WM_CHAR, static_cast<WPARAM>(L'a'), 1);
		pynote::harness::drain_messages();
		const std::size_t nAfterFirst = Guard.List().InvalidationLog().size();
		::SendMessageW(Guard.Page().EditorHwnd(), WM_CHAR, static_cast<WPARAM>(L'b'), 1);
		pynote::harness::drain_messages();
		REQUIRE(Guard.List().InvalidationLog().size() == nAfterFirst);
	}
}

TEST_CASE("PLAN-W4-0016 deleted card refreshes when new backing cannot be created", "[W4-meta]")
{
	// tests/ui/test_card_drag.py:817~836, spec §2 의 부분 종결 note.
	C_META_PAGE_FIXTURE Fixture;
	Fixture.CreateCards(2);
	C_CARD_LIST& List = Fixture.List();
	const HWND hList = Fixture.ListHwnd();
	const std::string sFirst = Fixture.CardId(0);

	// 연결 + 깨끗한 상태(3지 프롬프트 없음) - Python 오라클도 열기만 하고 타이핑하지 않는다.
	send_click(hList, Fixture.RowPoint(0));
	pynote::harness::drain_messages();
	REQUIRE(Fixture.Page().HasSession());
	Fixture.m_bForbidPrompts = true;

	int nChangeCount = 0;
	Fixture.Page().SetChangeNotifier([&nChangeCount]() { ++nChangeCount; });
	List.SetDragRunner([&](IDataObject* _pData, IDropSource*, DWORD, DWORD* _pEffect) -> HRESULT
		{
			REQUIRE(drive_drop(List.DropTargetForTest(), hList, _pData, Fixture.ZoneCentre()).AllMove());
			*_pEffect = DROPEFFECT_MOVE;
			return(DRAGDROP_S_DROP);
		});
	const std::size_t nRow = *List.Projection()->RowForCard(sFirst);
	gesture(hList, Fixture.RowPoint(nRow));
	pynote::harness::drain_messages();

	const HWND hEditor = Fixture.Page().EditorHwnd();
	REQUIRE(pynote::harness::wait_until([hEditor]() { return(::GetFocus() == hEditor); },
		std::chrono::milliseconds(2000)));
	// 세 단언만 닫는다 - _prepare_empty_surface 실패 사전조건은 대응물이 없어 미실시다
	// (재진입 = W6 편집기 결선).
	REQUIRE(Fixture.CardSoftDeleted(sFirst));
	REQUIRE_FALSE(List.Projection()->RowForCard(sFirst).has_value());
	REQUIRE(nChangeCount == 1);
}

TEST_CASE("PLAN-W4-0017 deleted card does not emit a duplicate delete event", "[W4-meta]")
{
	// tests/ui/test_card_drag.py:838~857.
	C_META_PAGE_FIXTURE Fixture;
	Fixture.CreateCards(2);
	C_CARD_LIST& List = Fixture.List();
	const HWND hList = Fixture.ListHwnd();
	const std::string sFirst = Fixture.CardId(0);

	send_click(hList, Fixture.RowPoint(0));
	pynote::harness::drain_messages();
	REQUIRE(Fixture.Page().HasSession());
	Fixture.m_bForbidPrompts = true;

	List.SetDragRunner([&](IDataObject* _pData, IDropSource*, DWORD, DWORD* _pEffect) -> HRESULT
		{
			REQUIRE(drive_drop(List.DropTargetForTest(), hList, _pData, Fixture.ZoneCentre()).AllMove());
			*_pEffect = DROPEFFECT_MOVE;
			return(DRAGDROP_S_DROP);
		});
	const std::size_t nRow = *List.Projection()->RowForCard(sFirst);
	gesture(hList, Fixture.RowPoint(nRow));
	pynote::harness::drain_messages();

	const HWND hEditor = Fixture.Page().EditorHwnd();
	REQUIRE(pynote::harness::wait_until([hEditor]() { return(::GetFocus() == hEditor); },
		std::chrono::milliseconds(2000)));
	REQUIRE(Fixture.CardSoftDeleted(sFirst));

	std::vector<domain::S_EDIT_EVENT> Events;
	REQUIRE(Fixture.Repositories().ListEvents(C_META_PAGE_FIXTURE::DocumentId, &Events) ==
		storage::E_REPO_RESULT::Ok);
	const std::size_t nDeleteCount = static_cast<std::size_t>(std::count_if(
		Events.begin(), Events.end(), [&sFirst](const domain::S_EDIT_EVENT& _Event)
		{
			return(_Event.sCardId && *_Event.sCardId == sFirst &&
				_Event.eEventType == domain::E_EVENT_TYPE::Delete);
		}));
	REQUIRE(nDeleteCount == 1);
}

TEST_CASE("WTL-CAP-NC-036 uia provider exposes the full card body as the accessible name",
	"[W4-meta][WTL-CAP-NC-036]")
{
	// spec §3.3.5, investigation §8.
	const std::string sLongBody = std::string(20000, L'x');
	domain::S_CARD Card1 = make_card(1, to_utf8(L"첫 줄\r\n둘째\t줄  \n  앞뒤 공백  "));
	domain::S_CARD Card2 = make_card(2, sLongBody);
	C_META_CONTROL_FIXTURE Fixture({ Card1, Card2 });
	const HWND hList = Fixture.Hwnd();
	Fixture.Projection().SetCurrentCardId(Card1.sId);

	HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	INFO("CoInitializeEx hr=0x" << std::hex << static_cast<unsigned long>(hr));
	{
		CComPtr<IUIAutomation> pAutomation;
		REQUIRE(SUCCEEDED(pAutomation.CoCreateInstance(__uuidof(CUIAutomation))));
		CComPtr<IUIAutomationElement> pElement;
		REQUIRE(SUCCEEDED(pAutomation->ElementFromHandle(hList, &pElement)));
		CComBSTR sName;
		REQUIRE(SUCCEEDED(pElement->get_CurrentName(&sName)));
		const std::wstring sActual(static_cast<BSTR>(sName), static_cast<std::size_t>(sName.Length()));
		REQUIRE(sActual == from_utf8(Card1.sBody));
		CONTROLTYPEID nControlType{};
		REQUIRE(SUCCEEDED(pElement->get_CurrentControlType(&nControlType)));
		REQUIRE(nControlType == UIA_ListControlTypeId);

		// 현재 카드를 바꾸고 재조회 - 새 본문, 여전히 바이트 그대로(공백·길이 포함).
		Fixture.Projection().SetCurrentCardId(Card2.sId);
		CComPtr<IUIAutomationElement> pElement2;
		REQUIRE(SUCCEEDED(pAutomation->ElementFromHandle(hList, &pElement2)));
		CComBSTR sName2;
		REQUIRE(SUCCEEDED(pElement2->get_CurrentName(&sName2)));
		const std::wstring sActual2(static_cast<BSTR>(sName2), static_cast<std::size_t>(sName2.Length()));
		REQUIRE(sActual2 == from_utf8(Card2.sBody));
		REQUIRE(sActual2.size() == 20000);
	}
	// RPC_E_CHANGED_MODE 는 이 스레드의 COM 참조 카운트를 올리지 않았다 - 그때는 걷지 않는다
	// (spec §3.3.5, D2D 스왑체인이 쥔 아파트를 실수로 걷지 않기 위해서다).
	if (SUCCEEDED(hr)) { ::CoUninitialize(); }
}
