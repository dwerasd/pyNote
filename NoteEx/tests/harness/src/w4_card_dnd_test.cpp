#include <catch_amalgamated.hpp>

#include "CCardList.h"
#include "CDocumentPage.h"
#include "Resource.h"
#include "pynote/harness/win32_harness.h"

// windows.h 의 CreateEvent 매크로가 repositories.h 의 멤버 이름을 바꾸기 전에 걷는다 -
// CDocumentPage.cpp·w4_card_select_test.cpp 와 같은 순서 계약이어야 같은 바이너리 안에서
// 멤버 이름이 갈리지 않는다. ATL/WTL(CCardList.h)은 이 #undef 앞에서 읽어야 자기
// ::CreateEvent 호출이 식별자를 잃지 않는다.
#ifdef CreateEvent
#undef CreateEvent
#endif

#include "pynote/core/application/card_service.h"
#include "pynote/core/application/draft_coordinator.h"
#include "pynote/core/application/save_coordinator.h"
#include "pynote/core/application/workspace_state.h"
#include "pynote/core/domain/card_drag_session_registry.h"
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
#include <initializer_list>
#include <ios>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "D2DWrapp")
#pragma comment(lib, "NoteExCore")
#pragma comment(lib, "Ole32")

namespace
{
	namespace app = pynote::core::application;
	namespace domain = pynote::core::domain;
	namespace storage = pynote::core::storage;
	using pynote::harness::TestWindow;

	// 시험은 폰트를 명시 주입해 측정을 결정적으로 만든다(S2 픽스처와 같은 값).
	constexpr wchar_t TEST_FONT_FAMILY[] = L"Segoe UI";
	constexpr float TEST_FONT_SIZE_DIP = 12.0f;
	// 원본 CARD_MIME_TYPE(card_model.py:20).
	constexpr wchar_t CARD_MIME_FORMAT_NAME[] = L"application/x-pynote-card-id";

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

	CLIPFORMAT card_mime_format()
	{
		static const CLIPFORMAT s_nFormat =
			static_cast<CLIPFORMAT>(::RegisterClipboardFormatW(CARD_MIME_FORMAT_NAME));
		return(s_nFormat);
	}

	// w4_card_select_test.cpp 의 같은 이름 헬퍼와 같은 카드다(그 TU 는 무수정 계약이라 복제한다).
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

	// 키 상태는 호출 스레드의 표로만 건다 - 전경 권한도, 사용자 데스크톱 부작용도 없다
	// (keybd_event/SendInput 을 쓰지 않는 이유다).
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

	void send_right_click(HWND _hWnd, POINT _Point)
	{
		::SendMessageW(_hWnd, WM_RBUTTONDOWN, MK_RBUTTON, pack_point(_Point));
		::SendMessageW(_hWnd, WM_RBUTTONUP, 0, pack_point(_Point));
	}

	POINT to_screen(HWND _hWnd, POINT _ClientDip)
	{
		// DPI 비인식 프로세스라 DIP == 픽셀이다(픽스처가 한 번 확인한다).
		POINT Point = _ClientDip;
		REQUIRE(::ClientToScreen(_hWnd, &Point) != FALSE);
		return(Point);
	}

	// 원본 customContextMenuRequested 좌표를 그대로 싣는 자리다(WM_CONTEXTMENU 는 화면 좌표).
	void send_context_menu(HWND _hWnd, POINT _ClientDip)
	{
		const POINT Screen = to_screen(_hWnd, _ClientDip);
		::SendMessageW(_hWnd, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(_hWnd), pack_point(Screen));
	}

	// 휠 메시지의 lParam 은 화면 좌표다(클라이언트 좌표가 아니다).
	void send_wheel(HWND _hWnd, int _nDelta, WORD _nKeys = 0)
	{
		RECT Client{};
		REQUIRE(::GetClientRect(_hWnd, &Client) != FALSE);
		const POINT Centre = to_screen(_hWnd,
			POINT{ (Client.left + Client.right) / 2, (Client.top + Client.bottom) / 2 });
		::SendMessageW(_hWnd, WM_MOUSEWHEEL,
			MAKEWPARAM(_nKeys, static_cast<WORD>(static_cast<short>(_nDelta))), pack_point(Centre));
	}

	// 원본 _gesture(tests/ui/test_card_drag.py:132~137): 행 중앙 press -> 임계 초과 이동 -> 릴리스.
	void gesture(HWND _hWnd, POINT _RowCentre)
	{
		const POINT End{ _RowCentre.x + CARD_DRAG_DISTANCE_DIP + 20, _RowCentre.y };
		send_press(_hWnd, _RowCentre);
		send_move(_hWnd, End);
		send_release(_hWnd, End);
	}

	// 타이머 창을 실제로 흘려보내며 "아무 일도 없었다" 를 관측한다.
	void pump_for(int _nMilliseconds)
	{
		const DWORD nDeadline = ::GetTickCount() + static_cast<DWORD>(_nMilliseconds);
		while (::GetTickCount() < nDeadline)
		{
			pynote::harness::drain_messages();
			::Sleep(5);
		}
		pynote::harness::drain_messages();
	}

	// ---- 메뉴 관측(w4_card_select_test.cpp:109~127 과 같은 API 계열) ----
	struct S_MENU_ITEM
	{
		UINT nId{ 0 };
		std::wstring sText;
		UINT nState{ 0 };
		UINT nType{ 0 };
		bool bSubMenu{ false };
	};

	std::vector<S_MENU_ITEM> read_menu(HMENU _hMenu)
	{
		std::vector<S_MENU_ITEM> Items;
		const int nCount = ::GetMenuItemCount(_hMenu);
		for (int nIndex = 0; nIndex < nCount; ++nIndex)
		{
			S_MENU_ITEM Item;
			MENUITEMINFOW Info{};
			Info.cbSize = sizeof(Info);
			Info.fMask = MIIM_ID | MIIM_STATE | MIIM_FTYPE | MIIM_SUBMENU;
			REQUIRE(::GetMenuItemInfoW(_hMenu, static_cast<UINT>(nIndex), TRUE, &Info) != FALSE);
			Item.nId = Info.wID;
			Item.nState = Info.fState;
			Item.nType = Info.fType;
			Item.bSubMenu = Info.hSubMenu != nullptr;
			wchar_t Buffer[256]{};
			const int nCopied = ::GetMenuStringW(_hMenu, static_cast<UINT>(nIndex), Buffer,
				static_cast<int>(std::size(Buffer)), MF_BYPOSITION);
			Item.sText.assign(Buffer, static_cast<std::size_t>((std::max)(0, nCopied)));
			Items.push_back(std::move(Item));
		}
		return(Items);
	}

	std::vector<std::wstring> menu_texts(const std::vector<S_MENU_ITEM>& _Items)
	{
		std::vector<std::wstring> Texts;
		for (const S_MENU_ITEM& Item : _Items) { Texts.push_back(Item.sText); }
		return(Texts);
	}

	// ---- 클립보드(공유 자원이라 유계 재시도한다 - w4_card_select_test.cpp:535~544 선례) ----
	bool clear_clipboard(HWND _hOwner)
	{
		for (int nAttempt = 0; nAttempt < 20; ++nAttempt)
		{
			if (::OpenClipboard(_hOwner))
			{
				const bool bOk = ::EmptyClipboard() != FALSE;
				::CloseClipboard();
				if (bOk) { return(true); }
			}
			::Sleep(20);
		}
		return(false);
	}

	std::optional<std::wstring> read_clipboard_text(HWND _hOwner)
	{
		for (int nAttempt = 0; nAttempt < 20; ++nAttempt)
		{
			if (::OpenClipboard(_hOwner))
			{
				std::optional<std::wstring> Result;
				const HANDLE hData = ::GetClipboardData(CF_UNICODETEXT);
				if (hData)
				{
					const wchar_t* pText = static_cast<const wchar_t*>(::GlobalLock(hData));
					if (pText) { Result = std::wstring(pText); ::GlobalUnlock(hData); }
				}
				::CloseClipboard();
				if (Result) { return(Result); }
			}
			::Sleep(20);
		}
		return(std::nullopt);
	}

	// ---- IDataObject 판독(TYMED_HGLOBAL 전용) ----
	FORMATETC make_format(CLIPFORMAT _nFormat)
	{
		FORMATETC Format{};
		Format.cfFormat = _nFormat;
		Format.ptd = nullptr;
		Format.dwAspect = DVASPECT_CONTENT;
		Format.lindex = -1;
		Format.tymed = TYMED_HGLOBAL;
		return(Format);
	}

	std::optional<std::string> read_format_bytes(IDataObject* _pData, CLIPFORMAT _nFormat)
	{
		REQUIRE(_pData != nullptr);
		FORMATETC Format = make_format(_nFormat);
		STGMEDIUM Medium{};
		if (FAILED(_pData->GetData(&Format, &Medium))) { return(std::nullopt); }
		std::optional<std::string> Result;
		if (Medium.tymed == TYMED_HGLOBAL && Medium.hGlobal)
		{
			const SIZE_T nBytes = ::GlobalSize(Medium.hGlobal);
			const void* pBytes = ::GlobalLock(Medium.hGlobal);
			if (pBytes)
			{
				Result = std::string(static_cast<const char*>(pBytes), nBytes);
				::GlobalUnlock(Medium.hGlobal);
			}
		}
		// OLE 초기화 없이도 도는 경로다 - pUnkForRelease 가 nullptr 이라 GlobalFree 만 부른다.
		::ReleaseStgMedium(&Medium);
		return(Result);
	}

	std::string payload_json(IDataObject* _pData)
	{
		const std::optional<std::string> Bytes = read_format_bytes(_pData, card_mime_format());
		REQUIRE(Bytes.has_value());
		return(*Bytes);
	}

	std::wstring payload_text(IDataObject* _pData)
	{
		const std::optional<std::string> Bytes = read_format_bytes(_pData, CF_UNICODETEXT);
		REQUIRE(Bytes.has_value());
		REQUIRE(Bytes->size() >= sizeof(wchar_t));
		const wchar_t* pText = reinterpret_cast<const wchar_t*>(Bytes->data());
		return(std::wstring(pText));
	}

	std::vector<CLIPFORMAT> enumerate_formats(IDataObject* _pData)
	{
		CComPtr<IEnumFORMATETC> pEnum;
		REQUIRE(_pData->EnumFormatEtc(DATADIR_GET, &pEnum) == S_OK);
		REQUIRE(pEnum != nullptr);
		std::vector<CLIPFORMAT> Formats;
		FORMATETC Format{};
		ULONG nFetched = 0;
		while (pEnum->Next(1, &Format, &nFetched) == S_OK && nFetched == 1)
		{
			Formats.push_back(Format.cfFormat);
		}
		return(Formats);
	}

	domain::CardDragSourceIdentity source_identity_of(IDataObject* _pData)
	{
		CComPtr<I_CARD_DRAG_SOURCE> pSource;
		REQUIRE(SUCCEEDED(_pData->QueryInterface(__uuidof(I_CARD_DRAG_SOURCE),
			reinterpret_cast<void**>(&pSource))));
		REQUIRE(pSource != nullptr);
		return(pSource->SourceIdentity());
	}

	// 부정 벡터를 위한 대역 데이터 개체다. 커스텀 형식의 바이트와 원본 동일성을 마음대로
	// 정할 수 있어야 원본 _accepts_internal_drag 의 요소별 거절이 공허해지지 않는다.
	class C_FAKE_DATA_OBJECT final : public IDataObject, public I_CARD_DRAG_SOURCE
	{
	public:
		C_FAKE_DATA_OBJECT(domain::CardDragSourceIdentity _nSource,
			std::optional<std::string> _sJson)
			: m_nSource(_nSource), m_sJson(std::move(_sJson)) {}

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID _Riid, void** _ppObject) override
		{
			if (!_ppObject) { return(E_POINTER); }
			*_ppObject = nullptr;
			if (_Riid == IID_IUnknown || _Riid == IID_IDataObject)
			{
				*_ppObject = static_cast<IDataObject*>(this);
			}
			else if (_Riid == __uuidof(I_CARD_DRAG_SOURCE))
			{
				*_ppObject = static_cast<I_CARD_DRAG_SOURCE*>(this);
			}
			else { return(E_NOINTERFACE); }
			this->AddRef();
			return(S_OK);
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return(++m_nReference); }
		ULONG STDMETHODCALLTYPE Release() override
		{
			const ULONG nLeft = --m_nReference;
			if (nLeft == 0) { delete this; }
			return(nLeft);
		}
		domain::CardDragSourceIdentity STDMETHODCALLTYPE SourceIdentity() override
		{
			return(m_nSource);
		}
		HRESULT STDMETHODCALLTYPE GetData(FORMATETC* _pFormat, STGMEDIUM* _pMedium) override
		{
			if (!_pFormat || !_pMedium) { return(E_POINTER); }
			*_pMedium = STGMEDIUM{};
			if (!m_sJson || _pFormat->cfFormat != card_mime_format()) { return(DV_E_FORMATETC); }
			if ((_pFormat->tymed & TYMED_HGLOBAL) == 0) { return(DV_E_TYMED); }
			HGLOBAL hMemory = ::GlobalAlloc(GMEM_MOVEABLE, m_sJson->size());
			if (!hMemory) { return(E_OUTOFMEMORY); }
			void* pTarget = ::GlobalLock(hMemory);
			if (!pTarget) { ::GlobalFree(hMemory); return(E_OUTOFMEMORY); }
			std::memcpy(pTarget, m_sJson->data(), m_sJson->size());
			::GlobalUnlock(hMemory);
			_pMedium->tymed = TYMED_HGLOBAL;
			_pMedium->hGlobal = hMemory;
			_pMedium->pUnkForRelease = nullptr;
			return(S_OK);
		}
		HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return(E_NOTIMPL); }
		HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* _pFormat) override
		{
			if (!_pFormat) { return(E_POINTER); }
			return(m_sJson && _pFormat->cfFormat == card_mime_format() ? S_OK : DV_E_FORMATETC);
		}
		HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override
		{
			return(E_NOTIMPL);
		}
		HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return(E_NOTIMPL); }
		HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC** _ppOut) override
		{
			if (_ppOut) { *_ppOut = nullptr; }
			return(E_NOTIMPL);
		}
		HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override
		{
			return(OLE_E_ADVISENOTSUPPORTED);
		}
		HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return(OLE_E_ADVISENOTSUPPORTED); }
		HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override
		{
			return(OLE_E_ADVISENOTSUPPORTED);
		}

	private:
		domain::CardDragSourceIdentity m_nSource{ 0 };
		std::optional<std::string> m_sJson;
		ULONG m_nReference{ 1 };
	};

	CComPtr<IDataObject> fake_payload(domain::CardDragSourceIdentity _nSource,
		std::optional<std::string> _sJson)
	{
		CComPtr<IDataObject> pData;
		pData.Attach(static_cast<IDataObject*>(
			new C_FAKE_DATA_OBJECT(_nSource, std::move(_sJson))));
		return(pData);
	}

	// ---- 드롭 대상 구동(모달 루프 없음) ----
	struct S_DROP_OBSERVATION
	{
		DWORD nEnter{ DROPEFFECT_NONE };
		DWORD nOver{ DROPEFFECT_NONE };
		DWORD nDrop{ DROPEFFECT_NONE };

		bool AllMove() const noexcept
		{
			return(nEnter == DROPEFFECT_MOVE && nOver == DROPEFFECT_MOVE && nDrop == DROPEFFECT_MOVE);
		}
		bool AllNone() const noexcept
		{
			return(nEnter == DROPEFFECT_NONE && nOver == DROPEFFECT_NONE && nDrop == DROPEFFECT_NONE);
		}
	};

	S_DROP_OBSERVATION drive_drop(IDropTarget* _pTarget, HWND _hWnd, IDataObject* _pData,
		POINT _ClientDip, bool _bDrop = true)
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
		if (_bDrop)
		{
			nEffect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
			REQUIRE(_pTarget->Drop(_pData, MK_LBUTTON, Where, &nEffect) == S_OK);
			Observation.nDrop = nEffect;
		}
		else { REQUIRE(_pTarget->DragLeave() == S_OK); }
		return(Observation);
	}

	// ---- 오버레이 기하의 기준 사각(원본 list_pane = 컨트롤의 창 사각) ----
	struct S_PANEL_FRAME
	{
		int nWidth{ 0 };
		int nHeight{ 0 };
		int nInsetX{ 0 };
		int nInsetY{ 0 };
	};

	S_PANEL_FRAME panel_frame(HWND _hWnd)
	{
		RECT Window{};
		REQUIRE(::GetWindowRect(_hWnd, &Window) != FALSE);
		POINT Origin{ 0, 0 };
		REQUIRE(::ClientToScreen(_hWnd, &Origin) != FALSE);
		S_PANEL_FRAME Frame;
		Frame.nWidth = static_cast<int>(Window.right - Window.left);
		Frame.nHeight = static_cast<int>(Window.bottom - Window.top);
		Frame.nInsetX = static_cast<int>(Origin.x - Window.left);
		Frame.nInsetY = static_cast<int>(Origin.y - Window.top);
		return(Frame);
	}

	// 클라이언트 좌표의 오버레이 사각을 원본이 재는 패널 좌표로 되돌린다.
	S_DIP_RECT to_panel_rect(const S_DIP_RECT& _Client, const S_PANEL_FRAME& _Frame)
	{
		return(S_DIP_RECT{ _Client.nLeft + _Frame.nInsetX, _Client.nTop + _Frame.nInsetY,
			_Client.nWidth, _Client.nHeight });
	}

	// ---- D2D 프레임 판독(spec §3.3.8 의 전제를 이 빌드에서 직접 증명한다) ----
	struct S_FRAME_PIXELS
	{
		std::vector<std::uint8_t> Bytes;
		UINT nWidth{ 0 };
		UINT nHeight{ 0 };
		UINT nPitch{ 0 };
		bool bOk{ false };

		// BGRA 순서다.
		void Pixel(int _nX, int _nY, int* _pRed, int* _pGreen, int* _pBlue) const
		{
			const std::size_t nOffset =
				static_cast<std::size_t>(_nY) * nPitch + static_cast<std::size_t>(_nX) * 4;
			*_pBlue = Bytes[nOffset];
			*_pGreen = Bytes[nOffset + 1];
			*_pRed = Bytes[nOffset + 2];
		}
	};

	void capture_frame(ID2D1DeviceContext* _pDc, S_FRAME_PIXELS* _pOut)
	{
		_pOut->bOk = false;
		CComPtr<ID2D1Image> pImage;
		_pDc->GetTarget(&pImage);
		if (!pImage) { return; }
		CComPtr<ID2D1Bitmap1> pTarget;
		if (FAILED(pImage->QueryInterface(__uuidof(ID2D1Bitmap1),
			reinterpret_cast<void**>(&pTarget))) || !pTarget) { return; }
		const D2D1_SIZE_U Size = pTarget->GetPixelSize();
		if (Size.width == 0 || Size.height == 0) { return; }
		const D2D1_BITMAP_PROPERTIES1 Properties = D2D1::BitmapProperties1(
			D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
			D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
		CComPtr<ID2D1Bitmap1> pCopy;
		if (FAILED(_pDc->CreateBitmap(Size, nullptr, 0, Properties, &pCopy)) || !pCopy) { return; }
		// Flush 는 CopyFromBitmap "앞" 이어야 한다. spec §3.3.8 은 뒤로 적고 있으나 실측으로
		// 그 순서에서는 밴드가 통째로 배경색이었다(빨강 비율 0.0) - CopyFromBitmap 은 D2D
		// 명령 대기열에 들어가는 그리기 명령이 아니라 표면을 즉시 읽으므로, 앞선 그리기가
		// 아직 대기열에 있으면 그것을 보지 못한다. 뒤의 Flush 는 그대로 두어 복사까지 밀어낸다.
		_pDc->Flush();
		if (FAILED(pCopy->CopyFromBitmap(nullptr, pTarget, nullptr))) { return; }
		_pDc->Flush();
		D2D1_MAPPED_RECT Mapped{};
		if (FAILED(pCopy->Map(D2D1_MAP_OPTIONS_READ, &Mapped))) { return; }
		_pOut->nWidth = Size.width;
		_pOut->nHeight = Size.height;
		_pOut->nPitch = Mapped.pitch;
		_pOut->Bytes.assign(Mapped.bits,
			Mapped.bits + static_cast<std::size_t>(Mapped.pitch) * Size.height);
		pCopy->Unmap();
		_pOut->bOk = true;
	}

	// ---- 컨트롤 픽스처(S2 C_SELECT_FIXTURE 모양 + 제품과 같은 WS_EX_CLIENTEDGE) ----
	struct S_DND_OPTIONS
	{
		std::size_t nCards{ 5 };
		int nClientWidth{ 500 };
		int nClientHeight{ 500 };
		bool bExtended{ false };
		domain::E_CARD_LIST_SORT_MODE eSortMode{ domain::E_CARD_LIST_SORT_MODE::Position };
		std::vector<domain::S_CARD> Cards{};
	};

	class C_DND_FIXTURE
	{
	public:
		explicit C_DND_FIXTURE(const S_DND_OPTIONS& _Options = {})
			: m_Host(pynote::harness::TestWindowOptions{ L"W4 dnd", 1000, 900, true })
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
			m_Control.SetMoveCardHandler([this](const std::string& _sCardId,
				const std::optional<std::string>& _sBefore)
				{ m_Moves.emplace_back(_sCardId, _sBefore); });
			m_Control.SetDeleteDroppedHandler(
				[this](const std::string& _sCardId) { m_DeleteDropped.push_back(_sCardId); });
			m_Control.SetBrowseCardHandler([this](const std::string& _sCardId)
				{ m_Browsed.push_back(_sCardId); return(true); });
			// 제품과 같은 창 프레임이어야 오버레이 기준 사각(창 사각)이 같은 값을 낸다.
			RECT Frame{ 0, 0, _Options.nClientWidth, _Options.nClientHeight };
			REQUIRE(m_Control.Create(m_Host.hwnd(), Frame, nullptr,
				WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL, WS_EX_CLIENTEDGE,
				static_cast<UINT>(IDC_DOCUMENT_CARD_LIST)) != nullptr);
			this->ResizeClient(_Options.nClientWidth, _Options.nClientHeight);
			// DPI 비인식 프로세스라 픽셀 == DIP 다 - 좌표 계약을 한 번 못박는다.
			REQUIRE(static_cast<int>(::GetDpiForWindow(m_Control.m_hWnd)) ==
				static_cast<int>(USER_DEFAULT_SCREEN_DPI));

			std::vector<domain::S_CARD> Cards = _Options.Cards;
			if (Cards.empty())
			{
				for (std::size_t nIndex = 0; nIndex < _Options.nCards; ++nIndex)
				{
					Cards.push_back(make_card(static_cast<int>(nIndex) + 1,
						to_utf8(L"카드 " + std::to_wstring(nIndex + 1))));
				}
			}
			m_Projection.SetSortMode(_Options.eSortMode);
			m_Projection.SetMultiSelectionEnabled(_Options.bExtended);
			m_Projection.SetCards(Cards);
			m_Control.OnProjectionChanged();
			this->FocusIn();
		}

		~C_DND_FIXTURE()
		{
			if (::GetCapture() == m_Control.m_hWnd) { ::ReleaseCapture(); }
			if (m_Control.IsWindow()) { m_Control.DestroyWindow(); }
		}

		C_DND_FIXTURE(const C_DND_FIXTURE&) = delete;
		C_DND_FIXTURE& operator=(const C_DND_FIXTURE&) = delete;

		C_CARD_LIST& Control() noexcept { return(m_Control); }
		domain::C_CARD_LIST_PROJECTION& ProjectionRef() noexcept { return(m_Projection); }
		HWND Hwnd() const noexcept { return(m_Control.m_hWnd); }
		TestWindow& Host() noexcept { return(m_Host); }

		const std::vector<std::string>& Opened() const noexcept { return(m_Opened); }
		const std::vector<std::vector<std::string>>& Deleted() const noexcept { return(m_Deleted); }
		const std::vector<std::pair<std::string, std::optional<std::string>>>& Moves() const noexcept
		{
			return(m_Moves);
		}
		const std::vector<std::string>& DeleteDropped() const noexcept { return(m_DeleteDropped); }
		const std::vector<std::string>& Browsed() const noexcept { return(m_Browsed); }
		const std::vector<std::string>& Selected() const { return(m_Projection.SelectedCardIds()); }

		int CurrentRow() const
		{
			const std::optional<std::string>& sCurrent = m_Projection.CurrentCardId();
			if (!sCurrent) { return(-1); }
			const auto nRow = m_Projection.RowForCard(*sCurrent);
			return(nRow ? static_cast<int>(*nRow) : -1);
		}

		std::string CardId(std::size_t _nRow) const
		{
			const domain::S_CARD* pCard = m_Projection.CardAt(_nRow);
			REQUIRE(pCard != nullptr);
			return(pCard->sId);
		}

		POINT Pt(std::size_t _nRow) const
		{
			const S_DIP_RECT Row = m_Control.RowRectDip(_nRow);
			return(POINT{ Row.nLeft + Row.nWidth / 2, Row.nTop + Row.nHeight / 2 });
		}

		POINT EmptyPoint() const
		{
			const S_DIP_RECT Last = m_Control.RowRectDip(m_Projection.RowCount() - 1);
			return(POINT{ m_Control.ViewportWidthDip() / 2,
				(Last.Bottom() + m_Control.ViewportHeightDip()) / 2 });
		}

		POINT ZoneCentre() const
		{
			const S_DIP_RECT Zone = m_Control.DeleteZoneRectDip();
			return(POINT{ Zone.nLeft + Zone.nWidth / 2, Zone.nTop + Zone.nHeight / 2 });
		}

		void ResizeClient(int _nWidthDip, int _nHeightDip)
		{
			// 컨트롤이 WS_VSCROLL 과 WS_EX_CLIENTEDGE 를 달고 있어 창 폭과 클라이언트 폭이
			// 다르다 - 뷰포트 기준으로 맞춘다(S2 픽스처와 같은 수렴 루프).
			int nWidth = _nWidthDip;
			int nHeight = _nHeightDip;
			for (int nAttempt = 0; nAttempt < 6; ++nAttempt)
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

		// 창 사각을 정확한 값으로 잡는다(오버레이 기준 사각 = 창 사각).
		void ResizeWindow(int _nWidth, int _nHeight)
		{
			::MoveWindow(m_Control.m_hWnd, 0, 0, _nWidth, _nHeight, TRUE);
			pynote::harness::drain_messages();
		}

		void FocusIn()
		{
			::SetFocus(m_Host.hwnd());
			pynote::harness::drain_messages();
			::SetFocus(m_Control.m_hWnd);
			pynote::harness::drain_messages();
		}

	private:
		d2d::C_D2D_DEVICE m_Device;
		d2d::C_D2D_TEXT m_Text;
		d2d::C_D2D_BRUSH_CACHE m_Brushes;
		TestWindow m_Host;
		domain::C_CARD_LIST_PROJECTION m_Projection;
		C_CARD_LIST m_Control;
		std::vector<std::string> m_Opened;
		std::vector<std::vector<std::string>> m_Deleted;
		std::vector<std::pair<std::string, std::optional<std::string>>> m_Moves;
		std::vector<std::string> m_DeleteDropped;
		std::vector<std::string> m_Browsed;
		int m_nClicked{ 0 };
	};

	// ---- 페이지 픽스처(S2 C_PAGE_FIXTURE 모양) ----
	class C_PAGE_FIXTURE
	{
	public:
		explicit C_PAGE_FIXTURE(
			domain::E_CARD_LIST_SORT_MODE _eSortMode = domain::E_CARD_LIST_SORT_MODE::Recency)
			: m_Path(std::filesystem::temp_directory_path() /
				("NoteEx-W4S4-page-" + std::to_string(::GetCurrentProcessId()) + "-" +
				std::to_string(++s_nSequence) + ".db")), m_Repositories(m_Database),
			  m_DraftStore(m_Database, m_Repositories),
			  m_Parent(pynote::harness::TestWindowOptions{ L"W4 S4 page host", 1000, 780, true })
		{
			this->remove_();
			REQUIRE(m_Database.Open(m_Path.string()));
			storage::C_MIGRATION_RUNNER Runner;
			Runner.SetExistingDatabase(false, m_Path.string());
			REQUIRE(Runner.Run(m_Database) == storage::E_MIGRATE_RESULT::Ok);
			domain::S_DOCUMENT Document;
			Document.sId = DocumentId;
			Document.sTitle = "w4 s4 document";
			Document.nCreatedAtUs = 1000;
			Document.nUpdatedAtUs = 1000;
			REQUIRE(m_Repositories.CreateDocument(Document) == storage::E_REPO_RESULT::Ok);
			domain::S_WORKSPACE_WINDOW Workspace;
			REQUIRE(m_Repositories.SaveWorkspaceWindow(
				WorkspaceId, { DocumentId }, DocumentId, &Workspace) == storage::E_REPO_RESULT::Ok);
			// 정렬 모드는 페이지 밖에서 정할 수 없다 - Init 이 읽는 UI 상태에 미리 심는다.
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
			m_Page = std::make_unique<C_DOCUMENT_PAGE>();
			m_Page->SetRenderServices(&m_Device, &m_Brushes, &m_Text);
			S_CARD_LIST_DISPLAY Display;
			Display.Font.sFamily = TEST_FONT_FAMILY;
			Display.Font.fSizeDip = TEST_FONT_SIZE_DIP;
			m_Page->SetDisplaySettings(Display);
			// 원본 시험이 QMessageBox.question 을 예외로 바꿔 두는 자리의 쌍둥이다 -
			// 프롬프트가 뜨면 그 자체로 실패다(빠뜨림이 아니라 금지 단언).
			REQUIRE(m_Page->Init(::GetModuleHandleW(nullptr), m_hLeft, m_hRight,
				m_Database, m_Repositories, *m_CardService, *m_Drafts, *m_Save,
				WorkspaceId, DocumentId, [this](HWND)
				{
					if (m_bForbidPrompts) { FAIL("이탈 프롬프트가 떠서는 안 된다"); }
					return(m_LeaveChoice);
				}));
			m_Page->SetDragDeletePrompt([this](HWND)
				{
					++m_nDragDeletePrompts;
					if (m_bForbidPrompts) { FAIL("드래그 삭제 프롬프트가 떠서는 안 된다"); }
					return(m_DragDeleteChoice);
				});
			REQUIRE(::GetDpiForWindow(m_Page->CardListHwnd()) == USER_DEFAULT_SCREEN_DPI);
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

		// 소프트 삭제의 증거다 - 저장소 행은 남아 있고 삭제 시각만 찍힌다(W7 복원의 전제).
		bool CardSoftDeleted(const std::string& _sCardId)
		{
			domain::S_CARD Card;
			if (m_Repositories.GetCard(_sCardId, &Card) != storage::E_REPO_RESULT::Ok)
			{
				return(false);
			}
			return(Card.nDeletedAtUs.has_value() && *Card.nDeletedAtUs != 0);
		}

		std::optional<std::string> CardRevision(const std::string& _sCardId)
		{
			domain::S_CARD Card;
			if (m_Repositories.GetCard(_sCardId, &Card) != storage::E_REPO_RESULT::Ok)
			{
				return(std::nullopt);
			}
			return(Card.sCurrentRevisionId);
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

		std::string CardBody(std::size_t _nRow) const
		{
			const domain::S_CARD* pCard = m_Page->CardList().Projection()->CardAt(_nRow);
			REQUIRE(pCard != nullptr);
			return(pCard->sBody);
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
		storage::C_REPOSITORIES& Repositories() noexcept { return(m_Repositories); }
		TestWindow& Parent() noexcept { return(m_Parent); }

		void ClosePage() { m_Page->Cleanup(); }

		static constexpr int LIST_HOST_HEIGHT = 700;
		C_DOCUMENT_PAGE::E_LEAVE_CHOICE m_LeaveChoice{ C_DOCUMENT_PAGE::E_LEAVE_CHOICE::Save };
		C_DOCUMENT_PAGE::E_DRAG_DELETE_CHOICE m_DragDeleteChoice{
			C_DOCUMENT_PAGE::E_DRAG_DELETE_CHOICE::Cancel };
		bool m_bForbidPrompts{ false };
		int m_nDragDeletePrompts{ 0 };
		inline static const std::string DocumentId = "document-w4s4";
		inline static const std::string WorkspaceId = "window-w4s4";

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
		std::int64_t m_nClock{ 2000 };
		std::uint64_t m_nId{};
		inline static std::atomic<unsigned long> s_nSequence{};
	};
}

// ---------------------------------------------------------------------------
// 군 A — 컨텍스트 메뉴
// ---------------------------------------------------------------------------

TEST_CASE("PLAN-W4-0001 card context menu has required actions and selection conditions",
	"[W4-dnd][WTL-CAP-TI-025]")
{
	// 원본 tests/ui/test_card_context_menu.py:62~125.
	S_DND_OPTIONS Options;
	Options.nCards = 2;
	Options.nClientWidth = 500;
	Options.nClientHeight = 400;
	Options.bExtended = true;
	C_DND_FIXTURE Fixture(Options);
	const HWND hList = Fixture.Hwnd();

	std::vector<std::vector<S_MENU_ITEM>> Shown;
	Fixture.Control().SetContextMenuExecutor([&Shown](HMENU _hMenu, POINT) -> UINT
		{
			Shown.push_back(read_menu(_hMenu));
			return(0);
		});

	send_right_click(hList, Fixture.Pt(0));
	pynote::harness::drain_messages();
	REQUIRE(Shown.size() == 1);
	const std::vector<std::wstring> Expected{
		L"편집기에서 열기", L"본문 복사", L"파일로 내보내기", L"닫기" };
	REQUIRE(menu_texts(Shown[0]) == Expected);
	for (const S_MENU_ITEM& Item : Shown[0])
	{
		INFO("menu item " << Item.nId);
		// 전부 활성이고, 확인 표시도 구분선도 하위 메뉴도 없다.
		REQUIRE((Item.nState & (MFS_DISABLED | MFS_GRAYED)) == 0);
		REQUIRE((Item.nState & MFS_CHECKED) == 0);
		REQUIRE((Item.nType & MFT_SEPARATOR) == 0);
		REQUIRE_FALSE(Item.bSubMenu);
		// 단축키 표시는 탭 뒤에 붙는다 - 원본 항목에는 없다.
		REQUIRE(Item.sText.find(L'\t') == std::wstring::npos);
	}

	// 두 행을 모두 고르고 선택된 행을 우클릭하면 다중 선택이 그대로 남는다.
	Fixture.ProjectionRef().SetSelectedCardIds({ Fixture.CardId(0), Fixture.CardId(1) });
	send_right_click(hList, Fixture.Pt(0));
	pynote::harness::drain_messages();
	REQUIRE(Fixture.Selected() == std::vector<std::string>{ Fixture.CardId(0), Fixture.CardId(1) });
	REQUIRE(Shown.size() == 2);
	REQUIRE(menu_texts(Shown[1]) == Expected);

	// 무효 행(마지막 행 아래 빈 영역)에서는 실행기가 아예 불리지 않는다.
	send_context_menu(hList, Fixture.EmptyPoint());
	pynote::harness::drain_messages();
	REQUIRE(Shown.size() == 2);
}

TEST_CASE("PLAN-W4-0002 card context menu reexposes open copy and delete",
	"[W4-dnd][WTL-CAP-FI-065][WTL-CAP-FI-066][WTL-CAP-TI-014]")
{
	// 원본 tests/ui/test_card_context_menu.py:128~150.
	// 본문에 줄바꿈을 넣어 OLE 층의 CRLF 계약(spec §3.1.3b)까지 함께 본다.
	S_DND_OPTIONS Options;
	Options.nCards = 0;
	Options.Cards = { make_card(1, to_utf8(L"첫 줄\n둘째 줄")), make_card(2, to_utf8(L"두 번째")) };
	C_DND_FIXTURE Fixture(Options);
	const HWND hList = Fixture.Hwnd();
	REQUIRE(clear_clipboard(Fixture.Host().hwnd()));

	UINT nChoice = 0;
	std::vector<std::vector<S_MENU_ITEM>> Shown;
	Fixture.Control().SetContextMenuExecutor([&](HMENU _hMenu, POINT) -> UINT
		{
			Shown.push_back(read_menu(_hMenu));
			return(Shown.back().at(nChoice).nId);
		});

	// 0 = 편집기에서 열기: 누른 카드 한 장만 열린다.
	nChoice = 0;
	send_right_click(hList, Fixture.Pt(0));
	pynote::harness::drain_messages();
	REQUIRE(Fixture.Opened() == std::vector<std::string>{ Fixture.CardId(0) });

	// 1 = 본문 복사: 진짜 시스템 클립보드에 CF_UNICODETEXT 로 실리고 줄바꿈은 CRLF 다.
	nChoice = 1;
	send_right_click(hList, Fixture.Pt(0));
	pynote::harness::drain_messages();
	const std::optional<std::wstring> Copied = read_clipboard_text(Fixture.Host().hwnd());
	REQUIRE(Copied.has_value());
	REQUIRE(*Copied == std::wstring(L"첫 줄\r\n둘째 줄"));

	// 2 = 파일로 내보내기: 활성이지만 아무 관측 가능한 일도 일어나지 않는다(W7 이월).
	const std::vector<std::string> OpenedBefore = Fixture.Opened();
	const std::size_t nDeletedBefore = Fixture.Deleted().size();
	nChoice = 2;
	send_right_click(hList, Fixture.Pt(0));
	pynote::harness::drain_messages();
	REQUIRE(Fixture.Opened() == OpenedBefore);
	REQUIRE(Fixture.Deleted().size() == nDeletedBefore);
	REQUIRE(Fixture.ProjectionRef().RowCount() == 2);

	// 3 = 닫기: 삭제 요청이 누른 카드 한 장으로 나간다.
	nChoice = 3;
	send_right_click(hList, Fixture.Pt(0));
	pynote::harness::drain_messages();
	REQUIRE(Fixture.Deleted().size() == nDeletedBefore + 1);
	REQUIRE(Fixture.Deleted().back() == std::vector<std::string>{ Fixture.CardId(0) });
}

TEST_CASE("PLAN-W4-0003 right click opens menu only over a card", "[W4-dnd]")
{
	// 원본 tests/ui/test_card_context_menu.py:153~217.
	S_DND_OPTIONS Options;
	Options.nCards = 1;
	Options.nClientWidth = 500;
	Options.nClientHeight = 400;
	C_DND_FIXTURE Fixture(Options);
	const HWND hList = Fixture.Hwnd();

	int nShown = 0;
	Fixture.Control().SetContextMenuExecutor([&nShown](HMENU _hMenu, POINT) -> UINT
		{
			++nShown;
			REQUIRE(::GetMenuItemCount(_hMenu) == 4);
			return(0);
		});

	send_context_menu(hList, Fixture.Pt(0));
	pynote::harness::drain_messages();
	REQUIRE(nShown == 1);
	const std::vector<std::string> SelectedBefore = Fixture.Selected();
	const int nCurrentBefore = Fixture.CurrentRow();

	// 원본이 쓰는 빈 위치와 같은 자리다(뷰포트 하단 4 DIP 위).
	const POINT Empty{ Fixture.Control().ViewportWidthDip() / 2,
		Fixture.Control().ViewportHeightDip() - 4 };
	send_context_menu(hList, Empty);
	pynote::harness::drain_messages();
	REQUIRE(nShown == 1);
	REQUIRE(Fixture.Selected() == SelectedBefore);
	REQUIRE(Fixture.CurrentRow() == nCurrentBefore);
}

TEST_CASE("PLAN-W4-0004 context delete soft deletes without confirmation and can restore",
	"[W4-dnd]")
{
	// 원본 tests/ui/test_card_drag.py:220~252.
	C_PAGE_FIXTURE Fixture;
	Fixture.CreateCards(2);
	// 원본이 QMessageBox.question 을 예외로 바꿔 두는 자리다 - 프롬프트가 뜨면 실패다.
	Fixture.m_bForbidPrompts = true;
	REQUIRE(Fixture.RowCount() == 2);
	const std::string sTarget = Fixture.CardId(0);
	const std::string sSibling = Fixture.CardId(1);

	Fixture.List().SetContextMenuExecutor([](HMENU _hMenu, POINT) -> UINT
		{
			const std::vector<S_MENU_ITEM> Items = read_menu(_hMenu);
			REQUIRE(Items.size() == 4);
			return(Items[3].nId);
		});
	send_right_click(Fixture.ListHwnd(), Fixture.RowPoint(0));
	pynote::harness::drain_messages();

	REQUIRE(Fixture.m_nDragDeletePrompts == 0);
	// 복원 반쪽(선언 치환): 네이티브에는 복원 서비스가 없고 복원 UI 는 W7 이다. 원본의
	// restore_card + refresh 왕복 대신 "하드가 아니라 소프트로 지워졌다" 를 단언한다 -
	// 그것이 원본 복원이 기대는 전제다. 재진입 조건 = W7 복원 UI.
	REQUIRE(Fixture.CardSoftDeleted(sTarget));
	REQUIRE(Fixture.RowCount() == 1);
	REQUIRE(Fixture.CardId(0) == sSibling);
	REQUIRE_FALSE(Fixture.CardDeleted(sSibling));
}

TEST_CASE("PLAN-W4-0005 context delete applies to entire multiple selection", "[W4-dnd]")
{
	// 원본 tests/ui/test_card_drag.py:255~287.
	C_PAGE_FIXTURE Fixture;
	Fixture.Page().SetMultiSelectionEnabled(true);
	Fixture.CreateCards(2);
	Fixture.m_bForbidPrompts = true;
	REQUIRE(Fixture.RowCount() == 2);
	const std::string sFirst = Fixture.CardId(0);
	const std::string sSecond = Fixture.CardId(1);

	send_click(Fixture.ListHwnd(), Fixture.RowPoint(0));
	send_click(Fixture.ListHwnd(), Fixture.RowPoint(1), MK_CONTROL);
	pynote::harness::drain_messages();
	REQUIRE(Fixture.List().Projection()->SelectedCardIds().size() == 2);

	Fixture.List().SetContextMenuExecutor([](HMENU _hMenu, POINT) -> UINT
		{
			const std::vector<S_MENU_ITEM> Items = read_menu(_hMenu);
			REQUIRE(Items.size() == 4);
			return(Items[3].nId);
		});
	send_right_click(Fixture.ListHwnd(), Fixture.RowPoint(0));
	pynote::harness::drain_messages();

	REQUIRE(Fixture.m_nDragDeletePrompts == 0);
	REQUIRE(Fixture.RowCount() == 0);
	REQUIRE(Fixture.CardSoftDeleted(sFirst));
	REQUIRE(Fixture.CardSoftDeleted(sSecond));
}

// ---------------------------------------------------------------------------
// 군 B — 삭제 오버레이
// ---------------------------------------------------------------------------

TEST_CASE("PLAN-W4-0007 delete zone actually paints and leaves list edges open",
	"[W4-dnd][WTL-CAP-RE-015]")
{
	// 원본 tests/ui/test_card_drag.py:80~121. isVisible() 만 보면 배경이 그려지지 않는
	// 결함을 놓친다 - 픽셀로 판정한다.
	C_PAGE_FIXTURE Fixture;
	Fixture.CreateCards(3);
	C_CARD_LIST& List = Fixture.List();

	S_FRAME_PIXELS Pixels;
	List.SetFrameCaptureHook([&Pixels](ID2D1DeviceContext* _pDc) { capture_frame(_pDc, &Pixels); });
	// 원본 page._show_delete_drop_zone(cards[0].id, 1) 과 같은 조작이다 - 페이지 핸들러는
	// 이 호출로 그대로 넘긴다(카드 id 는 오버레이에 쓰이지 않는다).
	List.ArmDeleteZone(1);
	REQUIRE(List.Render());
	// spec §3.3.8 의 전제(그리는 도중 CANNOT_DRAW 사본으로 읽기)를 이 빌드에서 증명한다.
	REQUIRE(Pixels.bOk);

	const S_CARD_LIST_FRAME& Frame = List.LastFrame();
	REQUIRE(Frame.bDeleteZoneVisible);
	const S_DIP_RECT Zone = Frame.DeleteZoneRect;
	REQUIRE(Zone.nWidth > 0);
	REQUIRE(Zone.nHeight == CARD_DELETE_ZONE_HEIGHT_DIP);

	// 밴드는 오버레이 사각 안의 y in [h/8, h/4), x in [w/4, 3w/4) 다. 원본의 alpha > 0 항은
	// 뺀다 - 스왑체인 타깃이 D2D1_ALPHA_MODE_IGNORE 라 읽어 온 알파가 관측값이 아니다
	// (spec §3.3.8 의 문서화된 치환). 두 빨강 비교는 그대로 옮긴다.
	// 오버레이 사각은 DIP 이고 읽어 온 프레임은 픽셀이다 - 대상 창의 실제 DPI 로 환산한다
	// (컨트롤이 내부에서 쓰는 것과 같은 MulDiv 변환이다). 이 프로세스는 DPI 비인식이라
	// 배율이 1 이고 페이지 픽스처가 그 전제를 생성자에서 이미 단언하지만, 여기서는 전제에
	// 기대지 않고 실제 배율로 옮긴다(감사 contract-6).
	const int nDpi = static_cast<int>(::GetDpiForWindow(Fixture.ListHwnd()));
	REQUIRE(nDpi > 0);
	const auto to_pixel = [nDpi](int _nDip)
		{ return(::MulDiv(_nDip, nDpi, USER_DEFAULT_SCREEN_DPI)); };
	int nSamples = 0;
	int nRedDominant = 0;
	for (int nY = Zone.nHeight / 8; nY < Zone.nHeight / 4; ++nY)
	{
		for (int nX = Zone.nWidth / 4; nX < Zone.nWidth * 3 / 4; ++nX)
		{
			const int nPixelX = to_pixel(Zone.nLeft + nX);
			const int nPixelY = to_pixel(Zone.nTop + nY);
			REQUIRE(nPixelX >= 0);
			REQUIRE(nPixelY >= 0);
			REQUIRE(static_cast<UINT>(nPixelX) < Pixels.nWidth);
			REQUIRE(static_cast<UINT>(nPixelY) < Pixels.nHeight);
			int nRed = 0;
			int nGreen = 0;
			int nBlue = 0;
			Pixels.Pixel(nPixelX, nPixelY, &nRed, &nGreen, &nBlue);
			++nSamples;
			if (nRed > nGreen && nRed > nBlue) { ++nRedDominant; }
		}
	}
	REQUIRE(nSamples > 0);
	const double dRatio = static_cast<double>(nRedDominant) / static_cast<double>(nSamples);
	INFO("red-dominant ratio = " << dRatio);
	REQUIRE(dRatio >= 0.9);

	// 기하 부등호는 원본 그대로다(:117~121). 원본이 재는 기준은 list_pane 폭·높이이고 그
	// 네이티브 쌍둥이는 컨트롤의 창 사각이다(spec §3.3.1) - 클라이언트 폭으로 재면 좌우
	// 테두리와 상시 스크롤바만큼 좁아져 우여백이 30 으로 떨어진다(실측). 시험계획이 적은
	// clientWidth/clientHeight 표현 대신 패널 좌표로 재는 이유다.
	const S_PANEL_FRAME Panel = panel_frame(Fixture.ListHwnd());
	const S_DIP_RECT PanelZone = to_panel_rect(Zone, Panel);
	REQUIRE(PanelZone.nLeft >= 32);
	REQUIRE(Panel.nWidth - PanelZone.Right() >= 32);
	REQUIRE(Panel.nHeight - PanelZone.Bottom() >= 12);
	// 지시서가 허용하는 정확 단언 셋(폭 상한·높이·하단 간격)도 같은 좌표에서 본다.
	REQUIRE(PanelZone.nWidth <= CARD_DELETE_ZONE_MAX_WIDTH_DIP);
	REQUIRE(PanelZone.nHeight == CARD_DELETE_ZONE_HEIGHT_DIP);
	REQUIRE(Panel.nHeight - (PanelZone.nTop + PanelZone.nHeight) ==
		CARD_DELETE_ZONE_BOTTOM_GAP_DIP);

	List.DisarmDeleteZone();
	List.SetFrameCaptureHook({});
}

// ---------------------------------------------------------------------------
// 군 C — 드래그 아웃과 드롭 인
// ---------------------------------------------------------------------------

TEST_CASE("PLAN-W4-0008 position drag supports self move and external copy",
	"[W4-dnd][WTL-CAP-FI-069][WTL-CAP-FI-070][WTL-CAP-TI-015]")
{
	// 원본 tests/ui/test_card_drag.py:285~361.
	C_PAGE_FIXTURE Fixture(domain::E_CARD_LIST_SORT_MODE::Position);
	Fixture.CreateCards(2);
	C_CARD_LIST& List = Fixture.List();
	const HWND hList = Fixture.ListHwnd();
	const std::string sFirst = Fixture.CardId(0);
	const std::string sBody = Fixture.CardBody(0);

	std::vector<std::pair<std::string, std::optional<std::string>>> Moves;
	List.SetMoveCardHandler([&](const std::string& _sCardId,
		const std::optional<std::string>& _sBefore)
		{
			Moves.emplace_back(_sCardId, _sBefore);
			Fixture.Page().MoveCard(_sCardId, _sBefore);
		});

	// ---- 내부 절반 ----
	DWORD nInternalOkEffects = 0;
	DWORD nInternalProposed = 0;
	std::vector<S_DROP_OBSERVATION> Internal;
	List.SetDragRunner([&](IDataObject* _pData, IDropSource* _pSource, DWORD _nOkEffects,
		DWORD* _pEffect) -> HRESULT
		{
			REQUIRE(_pData != nullptr);
			REQUIRE(_pSource != nullptr);
			nInternalOkEffects = _nOkEffects;
			// 원본 drag.exec 의 두 번째 인자(제안 동작 = Copy)의 네이티브 자리다.
			nInternalProposed = *_pEffect;
			const S_PANEL_FRAME Panel = panel_frame(hList);
			const S_DIP_RECT Zone = List.DeleteZoneRectDip();
			const S_DIP_RECT PanelZone = to_panel_rect(Zone, Panel);
			// 원본 :303~308 의 성질 단언을 그대로 옮긴다(구현 공식을 베끼지 않는다).
			REQUIRE(PanelZone.nWidth > 0);
			REQUIRE(PanelZone.nWidth <= CARD_DELETE_ZONE_MAX_WIDTH_DIP);
			REQUIRE(PanelZone.nHeight == CARD_DELETE_ZONE_HEIGHT_DIP);
			REQUIRE(PanelZone.nLeft + PanelZone.nWidth / 2 == Panel.nWidth / 2);
			REQUIRE(PanelZone.nLeft >= 32);
			REQUIRE(Panel.nWidth - PanelZone.Right() >= 32);
			REQUIRE(Panel.nHeight - PanelZone.Bottom() - 1 >= 12);
			// 원본 panel.childAt 의 쌍둥이 - 존 안이면 존, 좌우 8 DIP 밖이면 목록 행 영역이다.
			const POINT Centre{ Zone.nLeft + Zone.nWidth / 2, Zone.nTop + Zone.nHeight / 2 };
			const POINT Left{ Zone.nLeft - 8, Centre.y };
			const POINT Right{ Zone.Right() + 8, Centre.y };
			CComPtr<IDataObject> pLive(_pData);
			REQUIRE(drive_drop(List.DropTargetForTest(), hList, pLive, Centre, false).nEnter ==
				DROPEFFECT_MOVE);
			// 좌우 8 DIP 밖은 존이 아니라 행 영역이라 행 규칙(position 정렬)으로 수락된다.
			const S_DROP_OBSERVATION Observation =
				drive_drop(List.DropTargetForTest(), hList, pLive, Left);
			REQUIRE(Observation.AllMove());
			Internal.push_back(Observation);
			const S_DROP_OBSERVATION RightSide =
				drive_drop(List.DropTargetForTest(), hList, pLive, Right, false);
			REQUIRE(RightSide.nEnter == DROPEFFECT_MOVE);
			*_pEffect = DROPEFFECT_MOVE;
			return(DRAGDROP_S_DROP);
		});

	gesture(hList, Fixture.RowPoint(0));
	pynote::harness::drain_messages();
	REQUIRE(Internal.size() == 1);
	REQUIRE(nInternalOkEffects == (DROPEFFECT_COPY | DROPEFFECT_MOVE));
	REQUIRE(nInternalProposed == DROPEFFECT_COPY);
	// 존 왼쪽 8 DIP 지점은 마지막 행보다 아래라 "문서 끝으로 이동" 이다.
	REQUIRE(Moves.size() == 1);
	REQUIRE(Moves[0].first == sFirst);
	REQUIRE_FALSE(Moves[0].second.has_value());

	// ---- 외부 절반 ----
	std::vector<std::string> Copied;
	DWORD nExternalOkEffects = 0;
	DWORD nExternalProposed = 0;
	List.SetDragRunner([&](IDataObject* _pData, IDropSource*, DWORD _nOkEffects,
		DWORD* _pEffect) -> HRESULT
		{
			nExternalOkEffects = _nOkEffects;
			nExternalProposed = *_pEffect;
			Copied.push_back(to_utf8(payload_text(_pData)));
			*_pEffect = DROPEFFECT_COPY;
			return(DRAGDROP_S_DROP);
		});
	const std::size_t nMoveRow = *Fixture.List().Projection()->RowForCard(sFirst);
	gesture(hList, Fixture.RowPoint(nMoveRow));
	pynote::harness::drain_messages();
	REQUIRE(Copied == std::vector<std::string>{ sBody });
	REQUIRE(nExternalOkEffects == (DROPEFFECT_COPY | DROPEFFECT_MOVE));
	REQUIRE(nExternalProposed == DROPEFFECT_COPY);
	// 외부가 Copy 를 돌려도, Move 를 돌려도 카드는 그대로 있다(원본은 exec 반환을 읽지 않는다).
	REQUIRE_FALSE(Fixture.CardDeleted(sFirst));
}

TEST_CASE("PLAN-W4-0009 drag uses pressed index after selection changes", "[W4-dnd]")
{
	// 원본 tests/ui/test_card_drag.py:364~428.
	C_PAGE_FIXTURE Fixture(domain::E_CARD_LIST_SORT_MODE::Position);
	Fixture.Page().SetMultiSelectionEnabled(true);
	Fixture.CreateCards(2);
	C_CARD_LIST& List = Fixture.List();
	const HWND hList = Fixture.ListHwnd();
	const std::string sFirst = Fixture.CardId(0);
	const std::string sPressed = Fixture.CardId(1);

	send_click(hList, Fixture.RowPoint(0));
	send_click(hList, Fixture.RowPoint(1), MK_CONTROL);
	pynote::harness::drain_messages();
	REQUIRE(List.Projection()->SelectedCardIds().size() == 2);

	std::vector<std::string> Observed;
	std::vector<std::pair<std::string, std::optional<std::string>>> Moves;
	std::vector<std::string> Dropped;
	List.SetMoveCardHandler([&](const std::string& _sCardId,
		const std::optional<std::string>& _sBefore) { Moves.emplace_back(_sCardId, _sBefore); });
	List.SetDeleteDroppedHandler([&](const std::string& _sCardId)
		{ Dropped.push_back(_sCardId); });

	const auto payload_card = [](IDataObject* _pData)
		{
			// {"card_id":"<id>", ... 에서 값만 꺼낸다(형식 계약은 케이스 15 가 본다).
			const std::string sJson = payload_json(_pData);
			const std::string sKey = "\"card_id\":\"";
			const std::size_t nStart = sJson.find(sKey);
			REQUIRE(nStart != std::string::npos);
			const std::size_t nFrom = nStart + sKey.size();
			const std::size_t nEnd = sJson.find('"', nFrom);
			REQUIRE(nEnd != std::string::npos);
			return(sJson.substr(nFrom, nEnd - nFrom));
		};

	// 1) 러너 안에서 선택을 첫 카드로 바꿔도 payload 는 press 한 행의 카드다.
	List.SetDragRunner([&](IDataObject* _pData, IDropSource*, DWORD, DWORD* _pEffect) -> HRESULT
		{
			const_cast<domain::C_CARD_LIST_PROJECTION*>(List.Projection())->
				SetSelectedCardIds({ sFirst });
			Observed.push_back(payload_card(_pData));
			const POINT Bottom{ 10, List.ViewportHeightDip() - 4 };
			REQUIRE(drive_drop(List.DropTargetForTest(), hList, _pData, Bottom).AllMove());
			*_pEffect = DROPEFFECT_MOVE;
			return(DRAGDROP_S_DROP);
		});
	gesture(hList, Fixture.RowPoint(1));
	pynote::harness::drain_messages();

	// 2) 선택을 건드리지 않고 취소로 끝나도 같은 카드다.
	List.SetDragRunner([&](IDataObject* _pData, IDropSource*, DWORD, DWORD*) -> HRESULT
		{
			Observed.push_back(payload_card(_pData));
			return(DRAGDROP_S_CANCEL);
		});
	const std::size_t nRow = *List.Projection()->RowForCard(sPressed);
	gesture(hList, Fixture.RowPoint(nRow));
	pynote::harness::drain_messages();

	// 3) 선택을 바꾸고 오버레이에 떨어뜨려도 지워지는 것은 press 한 카드다.
	List.SetDragRunner([&](IDataObject* _pData, IDropSource*, DWORD, DWORD* _pEffect) -> HRESULT
		{
			const_cast<domain::C_CARD_LIST_PROJECTION*>(List.Projection())->
				SetSelectedCardIds({ sFirst });
			Observed.push_back(payload_card(_pData));
			REQUIRE(drive_drop(List.DropTargetForTest(), hList, _pData,
				Fixture.ZoneCentre()).AllMove());
			*_pEffect = DROPEFFECT_MOVE;
			return(DRAGDROP_S_DROP);
		});
	const std::size_t nRow3 = *List.Projection()->RowForCard(sPressed);
	gesture(hList, Fixture.RowPoint(nRow3));
	pynote::harness::drain_messages();

	REQUIRE(Observed == std::vector<std::string>{ sPressed, sPressed, sPressed });
	REQUIRE(Moves.size() == 1);
	REQUIRE(Moves[0].first == sPressed);
	REQUIRE(Dropped == std::vector<std::string>{ sPressed });
}

TEST_CASE("PLAN-W4-0010 recency and capture modes allow drag but not internal move", "[W4-dnd]")
{
	// 원본 tests/ui/test_card_drag.py:431~450.
	for (const domain::E_CARD_LIST_SORT_MODE eMode :
		{ domain::E_CARD_LIST_SORT_MODE::Recency, domain::E_CARD_LIST_SORT_MODE::Capture })
	{
		C_PAGE_FIXTURE Fixture(eMode);
		Fixture.CreateCards(2);
		C_CARD_LIST& List = Fixture.List();
		const HWND hList = Fixture.ListHwnd();
		const std::string sFirst = Fixture.CardId(0);

		// 네이티브 치환: 등록된 드롭 대상은 "acceptDrops == false" 를 등록 수준으로 표현할
		// 수 없다 - core 술어 둘과 아래 행동 확인이 원본 세 단언의 자리다.
		REQUIRE(List.Projection()->CanDragOut());
		REQUIRE_FALSE(List.Projection()->CanInternalReorder());

		std::vector<std::string> Calls;
		std::vector<std::pair<std::string, std::optional<std::string>>> Moves;
		std::vector<std::string> Dropped;
		List.SetMoveCardHandler([&](const std::string& _sCardId,
			const std::optional<std::string>& _sBefore) { Moves.emplace_back(_sCardId, _sBefore); });
		List.SetDeleteDroppedHandler([&](const std::string& _sCardId)
			{ Dropped.push_back(_sCardId); });
		List.SetDragRunner([&](IDataObject* _pData, IDropSource*, DWORD, DWORD*) -> HRESULT
			{
				const std::string sJson = payload_json(_pData);
				REQUIRE(sJson.find("\"card_id\":\"" + sFirst + "\"") != std::string::npos);
				Calls.push_back(sFirst);
				// 행 위에서는 어떤 단계도 수락되지 않는다.
				const S_DROP_OBSERVATION Rows = drive_drop(List.DropTargetForTest(), hList,
					_pData, Fixture.RowPoint(0), false);
				REQUIRE(Rows.nEnter == DROPEFFECT_NONE);
				REQUIRE(Rows.nOver == DROPEFFECT_NONE);
				// 추가 단언(부록 A [RECENCY-DROP]): 오버레이는 정렬 모드를 보지 않는다.
				const S_DROP_OBSERVATION Zone = drive_drop(List.DropTargetForTest(), hList,
					_pData, Fixture.ZoneCentre());
				REQUIRE(Zone.AllMove());
				return(DRAGDROP_S_DROP);
			});
		gesture(hList, Fixture.RowPoint(0));
		pynote::harness::drain_messages();
		REQUIRE(Calls == std::vector<std::string>{ sFirst });
		REQUIRE(Moves.empty());
		REQUIRE(Dropped == std::vector<std::string>{ sFirst });
	}
}

TEST_CASE("PLAN-W4-0011 delete zone appears on drag and tears down on every path",
	"[W4-dnd][WTL-CAP-FI-071][WTL-CAP-NC-014]")
{
	// 원본 tests/ui/test_card_drag.py:453~493.
	{
		C_PAGE_FIXTURE Fixture(domain::E_CARD_LIST_SORT_MODE::Position);
		Fixture.CreateCards(2);
		C_CARD_LIST& List = Fixture.List();
		const HWND hList = Fixture.ListHwnd();

		// 러너가 Move / Copy / Cancel / Cancel 로 끝나는 네 갈래 전부에서 세운 뒤 걷는다.
		const HRESULT Results[]{ DRAGDROP_S_DROP, DRAGDROP_S_DROP,
			DRAGDROP_S_CANCEL, DRAGDROP_S_CANCEL };
		const DWORD Effects[]{ DROPEFFECT_MOVE, DROPEFFECT_COPY,
			DROPEFFECT_NONE, DROPEFFECT_NONE };
		for (int nIndex = 0; nIndex < 4; ++nIndex)
		{
			const HRESULT hrResult = Results[nIndex];
			const DWORD nEffect = Effects[nIndex];
			// 반복 지역 변수는 값으로 잡는다 - 참조로 잡으면 반복이 끝난 뒤에도 설치된 채로
			// 남은 러너가 사라진 지역을 가리킨다(이 케이스에서는 호출되지 않지만 위험을 없앤다).
			List.SetDragRunner([&List, hrResult, nEffect](IDataObject*, IDropSource*, DWORD,
				DWORD* _pEffect) -> HRESULT
				{
					REQUIRE(List.ArmedDeleteToken().has_value());
					REQUIRE(List.Render());
					REQUIRE(List.LastFrame().bDeleteZoneVisible);
					*_pEffect = nEffect;
					return(hrResult);
				});
			gesture(hList, Fixture.RowPoint(0));
			pynote::harness::drain_messages();
			REQUIRE_FALSE(List.ArmedDeleteToken().has_value());
			REQUIRE(List.Render());
			REQUIRE_FALSE(List.LastFrame().bDeleteZoneVisible);
		}

		// 크기 변경은 오버레이를 감추지 않고 위치만 옮긴다(실측 경로).
		List.SetDragRunner([&](IDataObject*, IDropSource*, DWORD, DWORD*) -> HRESULT
			{
				const auto nToken = List.ArmedDeleteToken();
				REQUIRE(nToken.has_value());
				const S_DIP_RECT Before = List.DeleteZoneRectDip();
				::MoveWindow(hList, 0, 0, 340, C_PAGE_FIXTURE::LIST_HOST_HEIGHT - 100, TRUE);
				pynote::harness::drain_messages();
				const S_DIP_RECT After = List.DeleteZoneRectDip();
				REQUIRE(List.ArmedDeleteToken() == nToken);
				REQUIRE(After.nTop < Before.nTop);
				REQUIRE(List.Render());
				REQUIRE(List.LastFrame().bDeleteZoneVisible);
				::MoveWindow(hList, 0, 0, 340, C_PAGE_FIXTURE::LIST_HOST_HEIGHT, TRUE);
				pynote::harness::drain_messages();
				return(DRAGDROP_S_CANCEL);
			});
		gesture(hList, Fixture.RowPoint(0));
		pynote::harness::drain_messages();
		REQUIRE_FALSE(List.ArmedDeleteToken().has_value());

		// 러너가 예외를 던지면 정리가 먼저 돌고 예외는 그 뒤에 전파된다. 실행되지 않았으므로
		// press 소비가 되돌려지고 이어지는 릴리스가 카드를 연다(spec §3.1.8 규칙 2).
		List.SetDragRunner([](IDataObject*, IDropSource*, DWORD, DWORD*) -> HRESULT
			{ throw std::runtime_error("drag failure"); });
		std::vector<std::string> Opened;
		List.SetOpenCardHandler([&Opened](const std::string& _sCardId)
			{ Opened.push_back(_sCardId); });
		const POINT Start = Fixture.RowPoint(0);
		const POINT End{ Start.x + CARD_DRAG_DISTANCE_DIP + 20, Start.y };
		// 관측 수단의 문서화된 치환: 이 한 걸음만 SendMessage 가 아니라 ATL 메시지 맵을
		// 직접 부른다. USER32 콜백 경계를 넘는 C++ 예외의 되감기는 플랫폼 속성이지 제품
		// 계약이 아니고, 실측으로 이 프로세스에서는 그 경계를 넘을 때 예외가 호출부의
		// catch 를 건너뛰어 Catch2 의 케이스 수준 처리기까지 갔다(케이스 단독 실행에서는
		// 잡히고 전 선택자 실행에서는 잡히지 않는 순서 의존까지 나왔다). 여기서 보려는
		// 것은 "예외가 begin_drag_ 밖으로 나가는가" 이므로 그 경계를 뺀다 - press 와
		// release 는 그대로 진짜 메시지다. 원본 pytest.raises(match="drag failure") 처럼
		// 형과 메시지를 함께 본다.
		std::string sThrown;
		bool bThrewUnknown = false;
		send_press(hList, Start);
		LRESULT nResult = 0;
		try
		{
			List.ProcessWindowMessage(hList, WM_MOUSEMOVE, MK_LBUTTON, pack_point(End),
				nResult, 0);
		}
		catch (const std::runtime_error& Error) { sThrown = Error.what(); }
		catch (...) { bThrewUnknown = true; }
		INFO("unknown exception = " << bThrewUnknown);
		REQUIRE_FALSE(bThrewUnknown);
		REQUIRE(sThrown == "drag failure");
		REQUIRE_FALSE(List.ArmedDeleteToken().has_value());
		REQUIRE(List.Render());
		REQUIRE_FALSE(List.LastFrame().bDeleteZoneVisible);
		send_release(hList, Start);
		pynote::harness::drain_messages();
		REQUIRE(Opened.size() == 1);

		// 시작 가드(spec §3.1.5 단계 0): press 뒤 임계 초과 이동 전에 카드가 목록에서
		// 사라지면 러너도 신호도 토큰도 없고 press 소비도 서지 않는다.
		Opened.clear();
		bool bRunnerCalled = false;
		std::vector<std::string> Started;
		List.SetDragRunner([&](IDataObject*, IDropSource*, DWORD, DWORD*) -> HRESULT
			{ bRunnerCalled = true; return(DRAGDROP_S_CANCEL); });
		List.SetDragStartedHandler([&](const std::string& _sCardId, domain::CardDragSessionToken)
			{ Started.push_back(_sCardId); });
		const std::string sVanishing = Fixture.CardId(0);
		const POINT VanishStart = Fixture.RowPoint(0);
		send_press(hList, VanishStart);
		// 다른 창이 그 사이에 지운 것과 같은 상태를 만든다.
		{
			auto* pProjection =
				const_cast<domain::C_CARD_LIST_PROJECTION*>(List.Projection());
			std::vector<domain::S_CARD> Remaining;
			for (std::size_t nRow = 0; nRow < pProjection->RowCount(); ++nRow)
			{
				const domain::S_CARD* pCard = pProjection->CardAt(nRow);
				if (pCard && pCard->sId != sVanishing) { Remaining.push_back(*pCard); }
			}
			pProjection->SetCards(Remaining);
			List.OnProjectionChanged();
		}
		send_move(hList, POINT{ VanishStart.x + CARD_DRAG_DISTANCE_DIP + 20, VanishStart.y });
		send_release(hList, VanishStart);
		pynote::harness::drain_messages();
		REQUIRE_FALSE(bRunnerCalled);
		REQUIRE(Started.empty());
		REQUIRE_FALSE(List.ArmedDeleteToken().has_value());
		REQUIRE_FALSE(List.ActiveDragToken().has_value());
		// press 소비가 서지 않았다는 관측(원본은 _drag_consumed_press 를 직접 읽는다):
		// 소비되지 않은 press 의 릴리스는 릴리스 지점의 카드를 연다(감사 coverage-2).
		REQUIRE(Opened.size() == 1);
	}
	{
		// 페이지를 닫으면 감춰지고 무장도 풀린다.
		C_PAGE_FIXTURE Fixture(domain::E_CARD_LIST_SORT_MODE::Position);
		Fixture.CreateCards(2);
		Fixture.List().ArmDeleteZone(1);
		REQUIRE(Fixture.List().ArmedDeleteToken().has_value());
		Fixture.ClosePage();
		REQUIRE_FALSE(Fixture.List().ArmedDeleteToken().has_value());
	}
	{
		// 컨트롤 창을 파괴해도 무장이 풀린다.
		C_DND_FIXTURE Fixture;
		Fixture.Control().ArmDeleteZone(7);
		REQUIRE(Fixture.Control().ArmedDeleteToken().has_value());
		Fixture.Control().DestroyWindow();
		REQUIRE_FALSE(Fixture.Control().ArmedDeleteToken().has_value());
	}
}

TEST_CASE("PLAN-W4-0012 active drag source destruction is lifetime safe",
	"[W4-dnd][WTL-CAP-NC-014]")
{
	// 원본 tests/ui/test_card_drag.py:496~523.
	C_PAGE_FIXTURE Fixture(domain::E_CARD_LIST_SORT_MODE::Position);
	Fixture.CreateCards(2);
	C_CARD_LIST& List = Fixture.List();
	const HWND hList = Fixture.ListHwnd();
	const std::string sFirst = Fixture.CardId(0);

	int nFinished = 0;
	List.SetDragFinishedHandler([&](domain::CardDragSessionToken)
		{ ++nFinished; List.DisarmDeleteZone(); });
	List.SetDragRunner([&](IDataObject*, IDropSource*, DWORD, DWORD*) -> HRESULT
		{
			REQUIRE(List.ArmedDeleteToken().has_value());
			// 원본은 페이지를 닫고 원본 위젯을 지운다 - 네이티브 쌍둥이는 창 파괴다.
			List.DestroyWindow();
			return(DRAGDROP_S_CANCEL);
		});
	gesture(hList, Fixture.RowPoint(0));
	pynote::harness::drain_messages();

	// 종료 통지는 정확히 한 번이다 - OnDestroy 와 러너 반환 뒤가 둘 다 정리를 부르지만
	// finish_drag_session_ 이 멱등이다.
	REQUIRE(nFinished == 1);
	REQUIRE_FALSE(List.ArmedDeleteToken().has_value());
	REQUIRE_FALSE(List.ActiveDragToken().has_value());
	// 정리 뒤 다시 만든 payload 는 토큰 0 을 싣는다(원본 clear_drag_payload).
	CComPtr<IDataObject> pRebuilt;
	pRebuilt.Attach(List.CreateDragDataObject(sFirst));
	REQUIRE(pRebuilt != nullptr);
	REQUIRE(payload_json(pRebuilt).find("\"token\":0") != std::string::npos);
}

TEST_CASE("PLAN-W4-0013 next card click opens after drag release is consumed", "[W4-dnd]")
{
	// 원본 tests/ui/test_card_drag.py:526~546.
	{
		S_DND_OPTIONS Options;
		Options.nCards = 2;
		C_DND_FIXTURE Fixture(Options);
		const HWND hList = Fixture.Hwnd();
		Fixture.Control().SetDragRunner([](IDataObject*, IDropSource*, DWORD, DWORD*) -> HRESULT
			{ return(DRAGDROP_S_CANCEL); });

		gesture(hList, Fixture.Pt(0));
		pynote::harness::drain_messages();
		// 소비된 press 는 릴리스에서 카드를 열지 않는다.
		REQUIRE(Fixture.Opened().empty());
		send_click(hList, Fixture.Pt(1));
		pynote::harness::drain_messages();
		REQUIRE(Fixture.Opened() == std::vector<std::string>{ Fixture.CardId(1) });
	}
	{
		// 캡처 변경 단계(spec §3.1.7): 러너 안에서 WM_CAPTURECHANGED 가 오면 press 상태는
		// 걷히지만 세션과 press 소비 표식은 남아야 한다. 주입 러너는 캡처를 잡지 않으므로
		// 이 걸음이 없으면 그 의무는 전 게이트가 초록인 채로 검증되지 않고 나간다.
		S_DND_OPTIONS Options;
		Options.nCards = 2;
		C_DND_FIXTURE Fixture(Options);
		const HWND hList = Fixture.Hwnd();
		Fixture.Control().SetDragRunner([hList](IDataObject*, IDropSource*, DWORD, DWORD*) -> HRESULT
			{
				::SendMessageW(hList, WM_CAPTURECHANGED, 0, 0);
				return(DRAGDROP_S_CANCEL);
			});

		gesture(hList, Fixture.Pt(0));
		pynote::harness::drain_messages();
		REQUIRE(Fixture.Opened().empty());
		send_click(hList, Fixture.Pt(1));
		pynote::harness::drain_messages();
		REQUIRE(Fixture.Opened() == std::vector<std::string>{ Fixture.CardId(1) });
	}
}

TEST_CASE("PLAN-W4-0014 external drop does not delete card and carries text plain",
	"[W4-dnd][WTL-CAP-NC-013]")
{
	// 원본 tests/ui/test_card_drag.py:591~605.
	C_PAGE_FIXTURE Fixture(domain::E_CARD_LIST_SORT_MODE::Position);
	Fixture.CreateCards(2);
	C_CARD_LIST& List = Fixture.List();
	const HWND hList = Fixture.ListHwnd();
	const std::string sFirst = Fixture.CardId(0);
	const std::string sBody = Fixture.CardBody(0);

	std::vector<std::string> Received;
	List.SetDragRunner([&](IDataObject* _pData, IDropSource*, DWORD, DWORD* _pEffect) -> HRESULT
		{
			Received.push_back(to_utf8(payload_text(_pData)));
			*_pEffect = DROPEFFECT_COPY;
			return(DRAGDROP_S_DROP);
		});
	gesture(hList, Fixture.RowPoint(0));
	pynote::harness::drain_messages();
	REQUIRE(Received == std::vector<std::string>{ sBody });
	REQUIRE_FALSE(Fixture.CardDeleted(sFirst));

	// CAP-NC-013 컨트롤 반쪽: 본문 조회자 seam 이 실제로 소비된다. 더티 편집 버퍼를
	// 앞세우는 조회자 본체는 W6 몫이다(PLAN-W6-0011) - 이 행은 그만큼 부분 종결이다.
	const std::string sProvided = to_utf8(L"조회자 본문");
	REQUIRE(sProvided != sBody);
	List.SetDragBodyProvider([&sProvided](const std::string&) { return(sProvided); });
	Received.clear();
	gesture(hList, Fixture.RowPoint(0));
	pynote::harness::drain_messages();
	REQUIRE(Received == std::vector<std::string>{ sProvided });
}

TEST_CASE("PLAN-W4-0015 connected card deletion focuses empty editor", "[W4-dnd]")
{
	// 원본 tests/ui/test_card_drag.py:801~814.
	C_PAGE_FIXTURE Fixture(domain::E_CARD_LIST_SORT_MODE::Position);
	Fixture.CreateCards(2);
	C_CARD_LIST& List = Fixture.List();
	const HWND hList = Fixture.ListHwnd();
	const std::string sFirst = Fixture.CardId(0);

	// 카드를 연다(연결 + 깨끗한 상태라 3지 프롬프트는 뜨지 않는다).
	send_click(hList, Fixture.RowPoint(0));
	pynote::harness::drain_messages();
	REQUIRE(Fixture.Page().HasSession());
	Fixture.m_bForbidPrompts = true;

	// 선언 치환: 원본은 press 뒤 card_delete_dropped 를 직접 발행한다(press 스냅샷이 살아
	// 있기 때문이다). 네이티브에서 삭제 통지는 오버레이에서만 나오고 오버레이는 살아 있는
	// 세션 없이는 수락하지 않으므로 진짜 드롭 경로를 몬다(spec §3.3.5 규칙 1).
	List.SetDragRunner([&](IDataObject* _pData, IDropSource*, DWORD, DWORD* _pEffect) -> HRESULT
		{
			REQUIRE(drive_drop(List.DropTargetForTest(), hList, _pData,
				Fixture.ZoneCentre()).AllMove());
			*_pEffect = DROPEFFECT_MOVE;
			return(DRAGDROP_S_DROP);
		});
	const std::size_t nRow = *List.Projection()->RowForCard(sFirst);
	gesture(hList, Fixture.RowPoint(nRow));
	pynote::harness::drain_messages();

	// Qt 의 지연 재포커스는 이식하지 않는다(위젯 해체 중 포커스 이동에 대응물이 없다) -
	// 원본 시험이 기다리는 관측은 유계 대기로 같은 값에 닿는다.
	const HWND hEditor = Fixture.Page().EditorHwnd();
	REQUIRE(pynote::harness::wait_until([hEditor]() { return(::GetFocus() == hEditor); },
		std::chrono::milliseconds(2000)));
	REQUIRE_FALSE(Fixture.Page().HasSession());
	REQUIRE(Fixture.EditorText().empty());
	REQUIRE(Fixture.CardSoftDeleted(sFirst));
}

// ---------------------------------------------------------------------------
// 군 D — 네이티브 전용(부록 A 벡터·환경 계약)
// ---------------------------------------------------------------------------

TEST_CASE("W4 card drag data object carries the custom json and unicode text formats byte exact",
	"[W4-dnd][WTL-CAP-TI-015][WTL-CAP-FI-069]")
{
	// spec §3.7.2 의 전제를 먼저 증명한다: 이 프로세스는 OLE 를 초기화하지 않으며
	// TYMED_HGLOBAL + pUnkForRelease == nullptr 왕복은 그 상태에서 성립한다.
	{
		APTTYPE eApartment{};
		APTTYPEQUALIFIER eQualifier{};
		const HRESULT hrApartment = ::CoGetApartmentType(&eApartment, &eQualifier);
		// 아파트 상태는 단언하지 않는다(spec §3.7.2 - 어떤 규칙도 그 상태에 의존하면 안 된다).
		// 어느 상태였는지는 증거로만 남기고, 아래 왕복은 두 상태 모두에서 성립해야 한다.
		INFO("CoGetApartmentType = 0x" << std::hex << static_cast<unsigned long>(hrApartment));
		(void)eApartment;
		(void)eQualifier;

		S_DND_OPTIONS Options;
		Options.nCards = 2;
		C_DND_FIXTURE Fixture(Options);
		CComPtr<IDataObject> pData;
		pData.Attach(Fixture.Control().CreateDragDataObject(Fixture.CardId(0)));
		REQUIRE(pData != nullptr);
		FORMATETC Format = make_format(card_mime_format());
		STGMEDIUM Medium{};
		REQUIRE(pData->GetData(&Format, &Medium) == S_OK);
		REQUIRE(Medium.tymed == TYMED_HGLOBAL);
		REQUIRE(Medium.hGlobal != nullptr);
		REQUIRE(Medium.pUnkForRelease == nullptr);
		const HGLOBAL hBlock = Medium.hGlobal;
		REQUIRE(::GlobalSize(hBlock) > 0);
		::ReleaseStgMedium(&Medium);
		// 감사 프로브가 잰 그대로다 - tymed 는 0 이 되고 블록은 풀린다(GlobalSize 52 -> 0).
		// hGlobal 필드 자체는 ReleaseStgMedium 이 지우지 않는다(실측).
		REQUIRE(Medium.tymed == TYMED_NULL);
		REQUIRE(::GlobalSize(hBlock) == 0);
	}

	// 형식 목록과 바이트 계약(부록 A A-1·A-5 [MIME-BYTES], spec §3.1.3).
	{
		S_DND_OPTIONS Options;
		Options.nCards = 0;
		Options.bExtended = true;
		Options.Cards = { make_card(1, to_utf8(L"첫 줄\n둘째 줄\n")), make_card(2, "second") };
		C_DND_FIXTURE Fixture(Options);
		const HWND hList = Fixture.Hwnd();
		// 두 행을 골라도 payload 는 press 한 한 장이다(card_model.py:206).
		Fixture.ProjectionRef().SetSelectedCardIds({ Fixture.CardId(0), Fixture.CardId(1) });

		std::string sJson;
		std::string sTextBytes;
		std::vector<CLIPFORMAT> Formats;
		bool bUnregisteredRejected = false;
		Fixture.Control().SetDragRunner(
			[&](IDataObject* _pData, IDropSource*, DWORD, DWORD*) -> HRESULT
			{
				Formats = enumerate_formats(_pData);
				sJson = payload_json(_pData);
				sTextBytes = *read_format_bytes(_pData, CF_UNICODETEXT);
				FORMATETC Unregistered = make_format(CF_HDROP);
				bUnregisteredRejected = _pData->QueryGetData(&Unregistered) != S_OK;
				return(DRAGDROP_S_CANCEL);
			});
		gesture(hList, Fixture.Pt(0));
		pynote::harness::drain_messages();

		// 형식 순서: 커스텀 -> CF_UNICODETEXT -> (ACP 가 UTF-8 이 아니면) CF_TEXT.
		// 개수를 상수로 박지 않고 살아 있는 ACP 에서 유도한다(spec §3.1.3c).
		std::vector<CLIPFORMAT> Expected{ card_mime_format(), CF_UNICODETEXT };
		if (::GetACP() != CP_UTF8) { Expected.push_back(static_cast<CLIPFORMAT>(CF_TEXT)); }
		REQUIRE(Formats == Expected);
		REQUIRE(bUnregisteredRejected);

		// 키 순서 고정·공백 없음. 토큰은 이 드래그의 살아 있는 값이다.
		const std::string sPrefix = "{\"card_id\":\"card-1\",\"revision_id\":\"revision-1\",\"token\":";
		INFO("json = " << sJson);
		REQUIRE(sJson.rfind(sPrefix, 0) == 0);
		REQUIRE(sJson.back() == '}');
		const std::string sToken = sJson.substr(sPrefix.size(), sJson.size() - sPrefix.size() - 1);
		REQUIRE_FALSE(sToken.empty());
		REQUIRE(sToken.find_first_not_of("0123456789") == std::string::npos);
		REQUIRE(std::stoull(sToken) > 0);

		// CF_UNICODETEXT 는 (길이+1)*2 바이트에 끝 NUL 이다. 줄바꿈은 CRLF 다.
		const std::wstring sExpectedText = L"첫 줄\r\n둘째 줄\r\n";
		const std::size_t nExpectedBytes = (sExpectedText.size() + 1) * sizeof(wchar_t);
		REQUIRE(sTextBytes.size() == nExpectedBytes);
		const wchar_t* pText = reinterpret_cast<const wchar_t*>(sTextBytes.data());
		REQUIRE(std::wstring(pText, sExpectedText.size()) == sExpectedText);
		REQUIRE(pText[sExpectedText.size()] == L'\0');

		// 정리 뒤 다시 만든 payload 는 토큰 0 이고 본문은 카드 확정 본문이다.
		CComPtr<IDataObject> pRebuilt;
		pRebuilt.Attach(Fixture.Control().CreateDragDataObject(Fixture.CardId(0)));
		REQUIRE(pRebuilt != nullptr);
		REQUIRE(payload_json(pRebuilt) ==
			"{\"card_id\":\"card-1\",\"revision_id\":\"revision-1\",\"token\":0}");
	}

	// 리비전 없음과 비 ASCII id(부록 A A-1).
	{
		domain::S_CARD NoRevision = make_card(1, "body");
		NoRevision.sId = "null-rev";
		NoRevision.sCurrentRevisionId.reset();
		domain::S_CARD NonAscii = make_card(2, "body");
		NonAscii.sId = to_utf8(L"카드-1");
		NonAscii.sCurrentRevisionId = to_utf8(L"리비전-1");
		S_DND_OPTIONS Options;
		Options.nCards = 0;
		Options.Cards = { NoRevision, NonAscii };
		C_DND_FIXTURE Fixture(Options);
		const HWND hList = Fixture.Hwnd();

		std::vector<std::string> Payloads;
		Fixture.Control().SetDragRunner(
			[&](IDataObject* _pData, IDropSource*, DWORD, DWORD*) -> HRESULT
			{ Payloads.push_back(payload_json(_pData)); return(DRAGDROP_S_CANCEL); });
		gesture(hList, Fixture.Pt(0));
		gesture(hList, Fixture.Pt(1));
		pynote::harness::drain_messages();
		REQUIRE(Payloads.size() == 2);
		REQUIRE(Payloads[0].rfind("{\"card_id\":\"null-rev\",\"revision_id\":null,\"token\":", 0) == 0);
		// 비 ASCII 는 UTF-8 원문 그대로이며 \u 이스케이프가 없다.
		const std::string sNonAsciiPrefix = "{\"card_id\":\"" + to_utf8(L"카드-1") +
			"\",\"revision_id\":\"" + to_utf8(L"리비전-1") + "\",\"token\":";
		REQUIRE(Payloads[1].rfind(sNonAsciiPrefix, 0) == 0);
		REQUIRE(Payloads[1].find("\\u") == std::string::npos);
	}
}

TEST_CASE("W4 drop row half boundary inserts before the upper half and after the lower half",
	"[W4-dnd][WTL-CAP-RE-014][WTL-CAP-FI-070]")
{
	// 부록 A A-3·A-5 [ROW-HALF]. 경계는 QRect::center().y() = top + (height-1)/2 이고
	// y == 중앙은 이미 아래쪽 반이다(상반부가 1 DIP 짧다).
	S_DND_OPTIONS Options;
	Options.nCards = 3;
	C_DND_FIXTURE Fixture(Options);
	C_CARD_LIST& List = Fixture.Control();
	const HWND hList = Fixture.Hwnd();
	const std::size_t nRows = List.RowCount();
	REQUIRE(nRows == 3);

	std::vector<std::pair<std::string, std::optional<std::string>>> Moves;
	List.SetMoveCardHandler([&](const std::string& _sCardId,
		const std::optional<std::string>& _sBefore) { Moves.emplace_back(_sCardId, _sBefore); });

	// 드래그하는 카드는 마지막 행이다 - 앞 두 행의 경계를 자기 자신과 겹치지 않고 볼 수 있다.
	const std::string sDragged = Fixture.CardId(2);
	List.SetDragRunner([&](IDataObject* _pData, IDropSource*, DWORD, DWORD*) -> HRESULT
		{
			for (std::size_t nRow = 0; nRow < nRows; ++nRow)
			{
				const S_DIP_RECT Row = List.RowRectDip(nRow);
				const int nCentre = Row.nTop + (Row.nHeight - 1) / 2;
				const int nX = Row.nLeft + Row.nWidth / 2;
				const std::string sThisRow = Fixture.CardId(nRow);
				const std::optional<std::string> sNextRow = nRow + 1 < nRows ?
					std::optional<std::string>(Fixture.CardId(nRow + 1)) : std::nullopt;
				const struct { int nY; std::optional<std::string> sExpected; } Vectors[]{
					{ Row.nTop, sThisRow },
					{ nCentre - 1, sThisRow },
					{ nCentre, sNextRow },
					{ Row.Bottom(), sNextRow } };
				for (const auto& Vector : Vectors)
				{
					if (Vector.sExpected && *Vector.sExpected == sDragged)
					{
						// 자기 앞으로의 드롭은 아래에서 따로 본다.
						continue;
					}
					Moves.clear();
					INFO("row " << nRow << " y " << Vector.nY);
					const S_DROP_OBSERVATION Observation = drive_drop(List.DropTargetForTest(),
						hList, _pData, POINT{ nX, Vector.nY });
					REQUIRE(Observation.AllMove());
					REQUIRE(Moves.size() == 1);
					REQUIRE(Moves[0].first == sDragged);
					REQUIRE(Moves[0].second == Vector.sExpected);
				}
			}
			// 마지막 행 아래·뷰포트 하단은 전부 "문서 끝으로 이동" 이다.
			const S_DIP_RECT Last = List.RowRectDip(nRows - 1);
			const int Ends[]{ Last.Bottom() + 8, List.ViewportHeightDip() - 1 };
			for (const int nY : Ends)
			{
				Moves.clear();
				INFO("end y " << nY);
				REQUIRE(drive_drop(List.DropTargetForTest(), hList, _pData,
					POINT{ 10, nY }).AllMove());
				REQUIRE(Moves.size() == 1);
				REQUIRE_FALSE(Moves[0].second.has_value());
			}
			// 자기 자신 앞으로의 드롭은 enter/move 는 통과하고 drop 만 거절된다(self_upper 110).
			Moves.clear();
			const S_DIP_RECT Own = List.RowRectDip(nRows - 1);
			const S_DROP_OBSERVATION Self = drive_drop(List.DropTargetForTest(), hList, _pData,
				POINT{ Own.nLeft + Own.nWidth / 2, Own.nTop });
			REQUIRE(Self.nEnter == DROPEFFECT_MOVE);
			REQUIRE(Self.nOver == DROPEFFECT_MOVE);
			REQUIRE(Self.nDrop == DROPEFFECT_NONE);
			REQUIRE(Moves.empty());
			return(DRAGDROP_S_CANCEL);
		});
	gesture(hList, Fixture.Pt(2));
	pynote::harness::drain_messages();
}

TEST_CASE("W4 delete zone geometry follows the panel width across the measured table",
	"[W4-dnd][WTL-CAP-RE-015]")
{
	// 부록 A A-4. 기준 사각은 원본 list_pane 위젯 = 컨트롤의 창 사각이다(클라이언트가
	// 아니다). 창 폭 = 뷰포트 폭 + 좌우 WS_EX_CLIENTEDGE(각 SM_CXEDGE) + 상시 세로
	// 스크롤바(SM_CXVSCROLL)이고, 그려지는 사각은 좌·상 테두리만큼 클라이언트 좌표로
	// 옮겨진 값이라 아래에서 그만큼 되돌려 표와 맞춘다(spec §3.3.1).
	S_DND_OPTIONS Options;
	Options.nCards = 3;
	C_DND_FIXTURE Fixture(Options);
	C_CARD_LIST& List = Fixture.Control();
	const HWND hList = Fixture.Hwnd();
	List.ArmDeleteZone(1);

	{
		const S_PANEL_FRAME Frame = panel_frame(hList);
		REQUIRE(Frame.nInsetX == ::GetSystemMetrics(SM_CXEDGE));
		REQUIRE(Frame.nWidth == List.ViewportWidthDip() +
			2 * ::GetSystemMetrics(SM_CXEDGE) + ::GetSystemMetrics(SM_CXVSCROLL));
	}

	const struct { int nPanelWidth; int nWidth; } Table[]{
		{ 96, 0 }, { 100, 4 }, { 300, 204 }, { 376, 280 },
		{ 400, 280 }, { 500, 280 }, { 900, 280 } };
	for (const auto& Row : Table)
	{
		INFO("panel width " << Row.nPanelWidth);
		Fixture.ResizeWindow(Row.nPanelWidth, 700);
		const S_PANEL_FRAME Frame = panel_frame(hList);
		REQUIRE(Frame.nWidth == Row.nPanelWidth);
		REQUIRE(Frame.nHeight == 700);
		List.Render();
		const S_DIP_RECT Zone = to_panel_rect(List.LastFrame().DeleteZoneRect, Frame);
		REQUIRE(List.LastFrame().bDeleteZoneVisible);
		REQUIRE(Zone.nWidth == Row.nWidth);
		REQUIRE(Zone.nHeight == CARD_DELETE_ZONE_HEIGHT_DIP);
		// 하단 간격 16 = y 628(패널 높이 700). 지시서가 허용하는 정확 단언 셋이다.
		REQUIRE(Frame.nHeight - (Zone.nTop + Zone.nHeight) == CARD_DELETE_ZONE_BOTTOM_GAP_DIP);
		REQUIRE(Zone.nTop == 628);
		// x 는 상수 열이 아니라 원본 형태로 본다(정확한 여백 48 단언은 지시서가 금한다).
		REQUIRE(Zone.nLeft + Zone.nWidth / 2 == Frame.nWidth / 2);
		if (Row.nWidth > 0) { REQUIRE(Zone.nLeft >= 32); }
	}

	// 패널 0x0 이면 max(0, ...) 클램프 둘이 함께 걸려 (0, 0, 0, 56)이 된다.
	// 여기서는 그린 프레임이 아니라 기하 함수를 직접 읽는다 - 0 크기 창은 스왑 타깃을
	// 세울 수 없어 render_ 가 그리기 전에 돌아가기 때문이다(그래도 프레임에는 실린다).
	Fixture.ResizeWindow(0, 0);
	{
		const S_PANEL_FRAME Frame = panel_frame(hList);
		REQUIRE(Frame.nWidth == 0);
		REQUIRE(Frame.nHeight == 0);
		const S_DIP_RECT Zone = to_panel_rect(List.DeleteZoneRectDip(), Frame);
		REQUIRE(Zone.nLeft == 0);
		REQUIRE(Zone.nTop == 0);
		REQUIRE(Zone.nWidth == 0);
		REQUIRE(Zone.nHeight == CARD_DELETE_ZONE_HEIGHT_DIP);
	}
	List.DisarmDeleteZone();
}

TEST_CASE("W4 internal drop requires the live source token card revision position mode and move action",
	"[W4-dnd][WTL-CAP-TI-018]")
{
	// 부록 A A-2 의 수락 행렬. 13 개 부정 벡터는 전부 enter/move/drop 세 단계 모두에서
	// DROPEFFECT_NONE 이고 어떤 통지도 내지 않는다(표의 14 행 = 기준선 1 + 부정 13).
	S_DND_OPTIONS Options;
	Options.nCards = 3;
	C_DND_FIXTURE Fixture(Options);
	C_CARD_LIST& List = Fixture.Control();
	const HWND hList = Fixture.Hwnd();
	const std::string sDragged = Fixture.CardId(2);

	std::vector<std::pair<std::string, std::optional<std::string>>> Moves;
	std::vector<std::string> Dropped;
	List.SetMoveCardHandler([&](const std::string& _sCardId,
		const std::optional<std::string>& _sBefore) { Moves.emplace_back(_sCardId, _sBefore); });
	List.SetDeleteDroppedHandler([&](const std::string& _sCardId)
		{ Dropped.push_back(_sCardId); });

	domain::CardDragSessionToken nUsedToken = 0;
	domain::CardDragSourceIdentity nIdentity = 0;
	List.SetDragRunner([&](IDataObject* _pData, IDropSource*, DWORD, DWORD*) -> HRESULT
		{
			nIdentity = source_identity_of(_pData);
			const auto nToken = List.ActiveDragToken();
			REQUIRE(nToken.has_value());
			nUsedToken = *nToken;
			const std::string sToken = std::to_string(nUsedToken);
			const POINT RowPoint = Fixture.Pt(0);
			// 기준선: okEffects 가 Copy|Move 여도 결과는 Move 다.
			Moves.clear();
			REQUIRE(drive_drop(List.DropTargetForTest(), hList, _pData, RowPoint).AllMove());
			REQUIRE(Moves.size() == 1);

			const std::string sGood = "{\"card_id\":\"" + sDragged +
				"\",\"revision_id\":\"revision-3\",\"token\":" + sToken + "}";
			const std::vector<std::pair<const char*, std::optional<std::string>>> Negatives{
				{ "token + 1", "{\"card_id\":\"" + sDragged + "\",\"revision_id\":\"revision-3\","
					"\"token\":" + std::to_string(nUsedToken + 1) + "}" },
				{ "token 0", "{\"card_id\":\"" + sDragged + "\",\"revision_id\":\"revision-3\","
					"\"token\":0}" },
				{ "negative token", "{\"card_id\":\"" + sDragged + "\",\"revision_id\":\"revision-3\","
					"\"token\":-1}" },
				{ "true token", "{\"card_id\":\"" + sDragged + "\",\"revision_id\":\"revision-3\","
					"\"token\":true}" },
				{ "string token", "{\"card_id\":\"" + sDragged + "\",\"revision_id\":\"revision-3\","
					"\"token\":\"" + sToken + "\"}" },
				{ "other card_id", "{\"card_id\":\"card-1\",\"revision_id\":\"revision-3\","
					"\"token\":" + sToken + "}" },
				{ "other revision_id", "{\"card_id\":\"" + sDragged + "\",\"revision_id\":\"other\","
					"\"token\":" + sToken + "}" },
				{ "null revision_id", "{\"card_id\":\"" + sDragged + "\",\"revision_id\":null,"
					"\"token\":" + sToken + "}" },
				{ "missing custom format", std::nullopt },
				{ "malformed json", std::string("{\"card_id\":") },
				{ "json array", std::string("[\"card-3\"]") } };

			// 양성 대조: 같은 합성 경로로 만든 **정상** payload 는 수락돼야 한다. 이것이
			// 없으면 아래 13 개 부정 벡터가 "각자의 사유로" 거절된 것인지 "합성 데이터 개체
			// 경로 자체가 망가져서" 거절된 것인지 구별되지 않는다(감사 discriminability-1).
			{
				INFO("positive control (synthetic good payload)");
				Moves.clear();
				CComPtr<IDataObject> pGood = fake_payload(nIdentity, sGood);
				REQUIRE(drive_drop(List.DropTargetForTest(), hList, pGood, RowPoint).AllMove());
				REQUIRE(Moves.size() == 1);
				REQUIRE(Moves[0].first == sDragged);
			}

			for (const auto& Vector : Negatives)
			{
				INFO("negative vector " << Vector.first);
				Moves.clear();
				CComPtr<IDataObject> pFake = fake_payload(nIdentity, Vector.second);
				REQUIRE(drive_drop(List.DropTargetForTest(), hList, pFake, RowPoint).AllNone());
				REQUIRE(Moves.empty());
			}
			// 12) 남의 원본 동일성(원본 event.source() is not self).
			{
				INFO("negative vector foreign source");
				Moves.clear();
				CComPtr<IDataObject> pFake = fake_payload(nIdentity + 1, sGood);
				REQUIRE(drive_drop(List.DropTargetForTest(), hList, pFake, RowPoint).AllNone());
				REQUIRE(Moves.empty());
			}
			// 13) 정렬 모드가 position 이 아니면 행 위에서는 거절, 오버레이는 수락이다.
			{
				List.ArmDeleteZone(nUsedToken);
				for (const domain::E_CARD_LIST_SORT_MODE eMode :
					{ domain::E_CARD_LIST_SORT_MODE::Recency, domain::E_CARD_LIST_SORT_MODE::Capture })
				{
					Fixture.ProjectionRef().SetSortMode(eMode);
					List.OnProjectionChanged();
					Moves.clear();
					Dropped.clear();
					REQUIRE(drive_drop(List.DropTargetForTest(), hList, _pData, RowPoint).AllNone());
					REQUIRE(Moves.empty());
					REQUIRE(drive_drop(List.DropTargetForTest(), hList, _pData,
						Fixture.ZoneCentre()).AllMove());
					REQUIRE(Dropped.size() == 1);
				}
				Fixture.ProjectionRef().SetSortMode(domain::E_CARD_LIST_SORT_MODE::Position);
				List.OnProjectionChanged();
				// 오버레이 부정 벡터 1: 토큰 불일치는 존에서도 거절이고 삭제 통지가 없다.
				Dropped.clear();
				CComPtr<IDataObject> pStaleToken = fake_payload(nIdentity,
					"{\"card_id\":\"" + sDragged + "\",\"revision_id\":\"revision-3\",\"token\":" +
					std::to_string(nUsedToken + 1) + "}");
				REQUIRE(drive_drop(List.DropTargetForTest(), hList, pStaleToken,
					Fixture.ZoneCentre()).AllNone());
				REQUIRE(Dropped.empty());
				List.DisarmDeleteZone();
			}
			return(DRAGDROP_S_CANCEL);
		});
	gesture(hList, Fixture.Pt(2));
	pynote::harness::drain_messages();
	REQUIRE(nUsedToken > 0);

	// 오버레이 부정 벡터 2: 세션이 끝난 뒤 남은 무장(stale arm)만으로는 절대 수락되지 않는다.
	// 이것이 존이 자기 무장 토큰과 원본의 살아 있는 토큰을 함께 본다는 증거다.
	Dropped.clear();
	List.ArmDeleteZone(nUsedToken);
	REQUIRE_FALSE(List.ActiveDragToken().has_value());
	{
		CComPtr<IDataObject> pFake = fake_payload(nIdentity,
			"{\"card_id\":\"" + sDragged + "\",\"revision_id\":\"revision-3\",\"token\":" +
			std::to_string(nUsedToken) + "}");
		REQUIRE(drive_drop(List.DropTargetForTest(), hList, pFake,
			Fixture.ZoneCentre()).AllNone());
	}
	REQUIRE(Dropped.empty());
	// 14) 드래그가 끝난 뒤 재사용한 토큰은 행 위에서도 거절이다.
	{
		Moves.clear();
		CComPtr<IDataObject> pFake = fake_payload(nIdentity,
			"{\"card_id\":\"" + sDragged + "\",\"revision_id\":\"revision-3\",\"token\":" +
			std::to_string(nUsedToken) + "}");
		REQUIRE(drive_drop(List.DropTargetForTest(), hList, pFake, Fixture.Pt(0)).AllNone());
		REQUIRE(Moves.empty());
	}
	List.DisarmDeleteZone();
}

TEST_CASE("W4 wheel during an active drag scrolls lines instead of browsing",
	"[W4-dnd][WTL-CAP-FI-064]")
{
	// spec §3.1.12. 오라클 = W4_wheel_contract spec 부록 A [NOTES] 2 의 줄 스크롤 식.
	S_DND_OPTIONS Options;
	Options.nCards = 40;
	C_DND_FIXTURE Fixture(Options);
	C_CARD_LIST& List = Fixture.Control();
	const HWND hList = Fixture.Hwnd();
	REQUIRE(List.ContentHeightDip() > List.ViewportHeightDip());

	UINT nLines = 3;
	REQUIRE(::SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &nLines, 0) != FALSE);
	const int nRowHeight = List.RowHeightDip();
	const int nViewport = List.ViewportHeightDip();

	List.SetDragRunner([&](IDataObject*, IDropSource*, DWORD, DWORD*) -> HRESULT
		{
			const int nCurrentBefore = Fixture.CurrentRow();
			// 1) 한 틱: 줄 스크롤이 돌고 탐색은 아예 돌지 않으며 소비된다.
			List.ScrollToPixel(0);
			Fixture.Host().clear_messages();
			send_wheel(hList, -120);
			const int nExpected = (std::min)(
				static_cast<int>(nLines) * nRowHeight, nViewport);
			REQUIRE(List.ScrollOffsetDip() == nExpected);
			REQUIRE(Fixture.CurrentRow() == nCurrentBefore);
			REQUIRE_FALSE(List.PendingBrowseCardId().has_value());
			REQUIRE(List.WheelAngleRemainder() == 0);
			REQUIRE_FALSE(Fixture.Host().received(WM_MOUSEWHEEL));

			// 2) 빠른 회전(-360)은 한 페이지로 묶인다 - 이 기하에서는 뷰포트 높이가 이긴다.
			List.ScrollToPixel(0);
			Fixture.Host().clear_messages();
			send_wheel(hList, -360);
			const int nFast = (std::min)(3 * static_cast<int>(nLines) * nRowHeight, nViewport);
			REQUIRE(nFast == nViewport);
			REQUIRE(List.ScrollOffsetDip() == nFast);
			REQUIRE(List.ScrollOffsetDip() != 3 * static_cast<int>(nLines) * nRowHeight);

			// 3) 클램프 양 끝에서는 값이 변하지 않아 소비되지 않는다 - 부모가 받는다.
			//    위쪽 끝(오프셋 0 에서 위로).
			List.ScrollToPixel(0);
			Fixture.Host().clear_messages();
			send_wheel(hList, 120);
			REQUIRE(List.ScrollOffsetDip() == 0);
			REQUIRE(Fixture.Host().received(WM_MOUSEWHEEL));
			//    아래쪽 끝(시험계획이 지목한 벡터): 이미 최대인 상태에서 -120 은 값을 바꾸지
			//    못하므로 소비되지 않는다.
			List.ScrollToPixel(List.ContentHeightDip());
			const int nMaximum = List.ScrollOffsetDip();
			REQUIRE(nMaximum == List.ContentHeightDip() - List.ViewportHeightDip());
			Fixture.Host().clear_messages();
			send_wheel(hList, -120);
			REQUIRE(List.ScrollOffsetDip() == nMaximum);
			REQUIRE(Fixture.Host().received(WM_MOUSEWHEEL));
			List.ScrollToPixel(0);

			// 4) 드래그 시작이 대기 중 탐색을 취소했으므로 타이머 창을 흘려도 조용하다.
			pump_for(3 * 120);
			REQUIRE(Fixture.Browsed().empty());
			REQUIRE_FALSE(List.PendingBrowseCardId().has_value());
			return(DRAGDROP_S_CANCEL);
		});
	gesture(hList, Fixture.Pt(0));
	pynote::harness::drain_messages();

	// 짧은 뷰포트 벡터: 뷰포트가 한 번의 줄 스크롤보다 짧으면 페이지 경계가 이긴다
	// (L2 프로브 실측 258 이 아니라 248).
	{
		S_DND_OPTIONS Short;
		Short.nCards = 40;
		Short.nClientHeight = 200;
		C_DND_FIXTURE ShortFixture(Short);
		C_CARD_LIST& ShortList = ShortFixture.Control();
		const HWND hShort = ShortFixture.Hwnd();
		const int nShortViewport = ShortList.ViewportHeightDip();
		REQUIRE(nShortViewport < static_cast<int>(nLines) * ShortList.RowHeightDip());
		ShortList.SetDragRunner([&](IDataObject*, IDropSource*, DWORD, DWORD*) -> HRESULT
			{
				ShortList.ScrollToPixel(0);
				send_wheel(hShort, -120);
				REQUIRE(ShortList.ScrollOffsetDip() == nShortViewport);
				return(DRAGDROP_S_CANCEL);
			});
		gesture(hShort, ShortFixture.Pt(0));
		pynote::harness::drain_messages();
	}

	// Meta(Win 키)는 S4 가 건드리지 않는다 - 드래그가 없으면 S3 의 비소비 분기 그대로다.
	{
		C_MODIFIER_SCOPE Meta({ VK_LWIN });
		List.ScrollToPixel(0);
		const int nCurrentBefore = Fixture.CurrentRow();
		Fixture.Host().clear_messages();
		send_wheel(hList, -120);
		REQUIRE(List.ScrollOffsetDip() == 0);
		REQUIRE(Fixture.CurrentRow() == nCurrentBefore);
		REQUIRE_FALSE(List.PendingBrowseCardId().has_value());
		REQUIRE(Fixture.Host().received(WM_MOUSEWHEEL));
	}
	List.CancelPendingBrowse();
}

TEST_CASE("W4 drop target registration is non fatal and revoked before destruction",
	"[W4-dnd][WTL-CAP-NC-014]")
{
	// spec §3.2.1·§3.7.2. 계약은 "등록 실패가 치명적이 아니고, 파괴 앞에서 해제된다" 이지
	// 특정 코드가 아니다.
	// 실측 편차(보고서 기재): 이 프로세스는 OLE 를 초기화하지 않지만 아파트가 없는 상태로
	// 남지도 않는다 - D2D 스왑체인이 서면 그 스레드에 OLE 아파트가 생기고, 그 뒤로는
	// RegisterDragDrop 이 성공한다(같은 실행 안에서 케이스 하나만 돌리면 실패, 스왑체인을
	// 세우는 케이스 뒤에 돌리면 성공). 그래서 "늘 실패" 를 단언하면 실행 순서에 흔들리는
	// 시험이 된다 - 여기서는 기록된 결과와 보유 상태가 서로 맞는지, 그리고 결과와 무관하게
	// 컨트롤이 계속 도는지를 본다. 관측된 코드는 증거로 남긴다.
	S_DND_OPTIONS Options;
	Options.nCards = 2;
	C_DND_FIXTURE Fixture(Options);
	C_CARD_LIST& List = Fixture.Control();
	const HWND hList = Fixture.Hwnd();

	const HRESULT hrRegister = List.DropRegistrationResult();
	APTTYPE eApartment{};
	APTTYPEQUALIFIER eQualifier{};
	const HRESULT hrApartment = ::CoGetApartmentType(&eApartment, &eQualifier);
	// 증거 기록은 S1 성능 줄과 같은 형태로 표준 출력에 남긴다(단언이 아니다).
	std::cout << "W4 dnd registration hr=0x" << std::hex << static_cast<unsigned long>(hrRegister)
		<< " apartment_hr=0x" << static_cast<unsigned long>(hrApartment)
		<< " apartment_type=" << std::dec << static_cast<int>(eApartment) << std::endl;
	REQUIRE(SUCCEEDED(hrRegister) == List.HasDropRegistration());
	// 등록 결과와 무관하게 컨트롤의 나머지는 그대로 돈다 - 드롭 대상도 드래그도 성립한다.
	REQUIRE(List.DropTargetForTest() != nullptr);

	// fix1: 컨트롤의 기본 러너는 **비어 있다** - ::DoDragDrop 을 부르지 않고 "실행 뒤 취소"
	// 로 끝난다. 관측 계약은 press 소비 유지(릴리스가 카드를 열지 않는다)·정리·종료 통지
	// 1회다. 진짜 러너는 셸(CMain bind_card_list)이 걸며, 그래야 읽기 전용 S1~S3 시험이
	// 실제 모달 드래그 루프에 들지 않는다(spec §3.4.6 과 같은 형태).
	int nFinished = 0;
	List.SetDragFinishedHandler([&](domain::CardDragSessionToken) { ++nFinished; });
	gesture(hList, Fixture.Pt(0));
	pynote::harness::drain_messages();
	REQUIRE(nFinished == 1);
	REQUIRE(Fixture.Opened().empty());
	REQUIRE_FALSE(List.ActiveDragToken().has_value());

	bool bRan = false;
	nFinished = 0;
	List.SetDragRunner([&](IDataObject*, IDropSource*, DWORD, DWORD*) -> HRESULT
		{ bRan = true; return(DRAGDROP_S_CANCEL); });
	gesture(hList, Fixture.Pt(0));
	pynote::harness::drain_messages();
	REQUIRE(bRan);
	REQUIRE(nFinished == 1);

	// 살아 있는 세션을 든 채 창을 파괴해도 죽지 않고, 종료 통지는 정확히 한 번이며
	// 오버레이 무장도 풀린다(revoke 를 먼저 하는 순서 덕분이다).
	nFinished = 0;
	List.SetDragRunner([&](IDataObject*, IDropSource*, DWORD, DWORD*) -> HRESULT
		{
			List.ArmDeleteZone(*List.ActiveDragToken());
			List.DestroyWindow();
			return(DRAGDROP_S_CANCEL);
		});
	gesture(hList, Fixture.Pt(0));
	pynote::harness::drain_messages();
	REQUIRE(nFinished == 1);
	REQUIRE_FALSE(List.HasDropRegistration());
	REQUIRE_FALSE(List.ArmedDeleteToken().has_value());
	REQUIRE_FALSE(List.ActiveDragToken().has_value());
	// 실제 등록 성공은 OLE 가 초기화된 제품 프로세스에서만 관측된다 - 그 반쪽은 지휘의
	// G4 수동 확인(실제 앱에서의 진짜 드래그)이 덮는다.
}
