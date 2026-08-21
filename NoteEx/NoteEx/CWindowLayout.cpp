#include "targetver.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "CWindowLayout.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>

namespace pynote::shell
{
	namespace
	{
		constexpr std::array<std::uint8_t, 4> GEOMETRY_MAGIC{ 'P', 'N', 'G', '1' };
		constexpr std::uint16_t GEOMETRY_VERSION = 1;
		constexpr std::size_t GEOMETRY_BYTES = 28;
		constexpr std::int32_t MAX_CLIENT_DIP = 32768;
		constexpr std::int32_t MAX_FRAME_COORDINATE = 1000000;
		constexpr std::uint32_t MIN_DPI = 48;
		constexpr std::uint32_t MAX_DPI = 768;

		void append_u16(std::vector<std::uint8_t>* _pBytes, std::uint16_t _nValue)
		{
			_pBytes->push_back(static_cast<std::uint8_t>(_nValue));
			_pBytes->push_back(static_cast<std::uint8_t>(_nValue >> 8));
		}

		void append_u32(std::vector<std::uint8_t>* _pBytes, std::uint32_t _nValue)
		{
			for (unsigned int i = 0; i < 4; ++i)
			{
				_pBytes->push_back(static_cast<std::uint8_t>(_nValue >> (i * 8)));
			}
		}

		std::uint16_t read_u16(const std::uint8_t* _pBytes)
		{
			return(static_cast<std::uint16_t>(_pBytes[0]) |
				(static_cast<std::uint16_t>(_pBytes[1]) << 8));
		}

		std::uint32_t read_u32(const std::uint8_t* _pBytes)
		{
			std::uint32_t nValue = 0;
			for (unsigned int i = 0; i < 4; ++i)
			{
				nValue |= static_cast<std::uint32_t>(_pBytes[i]) << (i * 8);
			}
			return(nValue);
		}

		bool valid_geometry(const S_WINDOW_GEOMETRY& _Geometry)
		{
			return(_Geometry.nFrameXpx >= -MAX_FRAME_COORDINATE &&
				_Geometry.nFrameXpx <= MAX_FRAME_COORDINATE &&
				_Geometry.nFrameYpx >= -MAX_FRAME_COORDINATE &&
				_Geometry.nFrameYpx <= MAX_FRAME_COORDINATE &&
				_Geometry.nClientWidthDip > 0 && _Geometry.nClientWidthDip <= MAX_CLIENT_DIP &&
				_Geometry.nClientHeightDip > 0 && _Geometry.nClientHeightDip <= MAX_CLIENT_DIP &&
				_Geometry.nDpi >= MIN_DPI && _Geometry.nDpi <= MAX_DPI);
		}

		BOOL CALLBACK collect_monitor(HMONITOR _hMonitor, HDC, LPRECT, LPARAM _nParameter)
		{
			auto* pAreas = reinterpret_cast<std::vector<RECT>*>(_nParameter);
			MONITORINFO Information{};
			Information.cbSize = sizeof(Information);
			if (::GetMonitorInfoW(_hMonitor, &Information)) { pAreas->push_back(Information.rcWork); }
			return(TRUE);
		}

		UINT dpi_for_monitor(HMONITOR _hMonitor, UINT _nFallback)
		{
			using GET_DPI_FOR_MONITOR = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
			const HMODULE hShcore = ::LoadLibraryW(L"shcore.dll");
			if (!hShcore) { return(_nFallback); }
			const auto GetDpiForMonitor = reinterpret_cast<GET_DPI_FOR_MONITOR>(
				::GetProcAddress(hShcore, "GetDpiForMonitor"));
			UINT nDpiX = 0;
			UINT nDpiY = 0;
			const bool bOk = GetDpiForMonitor &&
				SUCCEEDED(GetDpiForMonitor(_hMonitor, 0, &nDpiX, &nDpiY)) && nDpiX != 0;
			::FreeLibrary(hShcore);
			return(bOk ? nDpiX : _nFallback);
		}

		RECT normal_frame_rect(HWND _hWnd)
		{
			RECT Frame{};
			if (!::IsIconic(_hWnd) && !::IsZoomed(_hWnd) && ::GetWindowRect(_hWnd, &Frame))
			{
				return(Frame);
			}
			WINDOWPLACEMENT Placement{};
			Placement.length = sizeof(Placement);
			if (!::GetWindowPlacement(_hWnd, &Placement)) { return(Frame); }
			Frame = Placement.rcNormalPosition;
			const HMONITOR hMonitor = ::MonitorFromWindow(_hWnd, MONITOR_DEFAULTTONEAREST);
			MONITORINFO Information{};
			Information.cbSize = sizeof(Information);
			if (hMonitor && ::GetMonitorInfoW(hMonitor, &Information))
			{
				::OffsetRect(&Frame, Information.rcWork.left, Information.rcWork.top);
			}
			return(Frame);
		}
	}

	std::string WindowGeometryKey(const std::string& _sStableWindowId)
	{
		if (_sStableWindowId.empty() || _sStableWindowId.find('/') != std::string::npos ||
			_sStableWindowId.find_first_of("=\r\n") != std::string::npos)
		{
			return(std::string{});
		}
		return("windows/" + _sStableWindowId + "/geometry");
	}

	std::vector<std::uint8_t> EncodeWindowGeometry(const S_WINDOW_GEOMETRY& _Geometry)
	{
		if (!valid_geometry(_Geometry)) { return(std::vector<std::uint8_t>{}); }
		std::vector<std::uint8_t> Bytes;
		Bytes.reserve(GEOMETRY_BYTES);
		Bytes.insert(Bytes.end(), GEOMETRY_MAGIC.begin(), GEOMETRY_MAGIC.end());
		append_u16(&Bytes, GEOMETRY_VERSION);
		append_u16(&Bytes, _Geometry.bMaximized ? 1 : 0);
		append_u32(&Bytes, static_cast<std::uint32_t>(_Geometry.nFrameXpx));
		append_u32(&Bytes, static_cast<std::uint32_t>(_Geometry.nFrameYpx));
		append_u32(&Bytes, static_cast<std::uint32_t>(_Geometry.nClientWidthDip));
		append_u32(&Bytes, static_cast<std::uint32_t>(_Geometry.nClientHeightDip));
		append_u32(&Bytes, _Geometry.nDpi);
		return(Bytes);
	}

	bool DecodeWindowGeometry(
		const std::vector<std::uint8_t>& _Bytes, S_WINDOW_GEOMETRY* _pGeometry)
	{
		if (!_pGeometry || _Bytes.size() != GEOMETRY_BYTES ||
			!std::equal(GEOMETRY_MAGIC.begin(), GEOMETRY_MAGIC.end(), _Bytes.begin()) ||
			read_u16(_Bytes.data() + 4) != GEOMETRY_VERSION)
		{
			return(false);
		}
		const std::uint16_t nFlags = read_u16(_Bytes.data() + 6);
		if ((nFlags & ~1u) != 0) { return(false); }
		S_WINDOW_GEOMETRY Geometry;
		Geometry.bMaximized = (nFlags & 1u) != 0;
		Geometry.nFrameXpx = static_cast<std::int32_t>(read_u32(_Bytes.data() + 8));
		Geometry.nFrameYpx = static_cast<std::int32_t>(read_u32(_Bytes.data() + 12));
		Geometry.nClientWidthDip = static_cast<std::int32_t>(read_u32(_Bytes.data() + 16));
		Geometry.nClientHeightDip = static_cast<std::int32_t>(read_u32(_Bytes.data() + 20));
		Geometry.nDpi = read_u32(_Bytes.data() + 24);
		if (!valid_geometry(Geometry)) { return(false); }
		*_pGeometry = Geometry;
		return(true);
	}

	int DipsToPixels(int _nDips, UINT _nDpi)
	{
		return(::MulDiv(_nDips, static_cast<int>((std::max)(1u, _nDpi)), USER_DEFAULT_SCREEN_DPI));
	}

	int PixelsToDips(int _nPixels, UINT _nDpi)
	{
		return(::MulDiv(_nPixels, USER_DEFAULT_SCREEN_DPI, static_cast<int>((std::max)(1u, _nDpi))));
	}

	std::pair<int, int> ClampPaneSizesDip(int _nRequestedLeftDip, int _nAvailableSpanDip)
	{
		if (_nAvailableSpanDip < 2)
		{
			return(std::pair<int, int>{ 0, (std::max)(0, _nAvailableSpanDip) });
		}
		const int nMinimumTotal = SPLITTER_LEFT_MIN_DIP + SPLITTER_RIGHT_MIN_DIP;
		int nLeft = 0;
		if (_nAvailableSpanDip <= nMinimumTotal)
		{
			nLeft = ::MulDiv(_nAvailableSpanDip, SPLITTER_LEFT_MIN_DIP, nMinimumTotal);
			nLeft = (std::clamp)(nLeft, 1, _nAvailableSpanDip - 1);
		}
		else
		{
			nLeft = (std::clamp)(
				_nRequestedLeftDip, SPLITTER_LEFT_MIN_DIP,
				_nAvailableSpanDip - SPLITTER_RIGHT_MIN_DIP);
		}
		return(std::pair<int, int>{ nLeft, _nAvailableSpanDip - nLeft });
	}

	bool IntersectsMonitorWorkArea(const RECT& _Frame, const std::vector<RECT>& _WorkAreas)
	{
		for (const RECT& Work : _WorkAreas)
		{
			RECT Intersection{};
			if (::IntersectRect(&Intersection, &_Frame, &Work) &&
				Intersection.right > Intersection.left && Intersection.bottom > Intersection.top)
			{
				return(true);
			}
		}
		return(false);
	}

	std::vector<RECT> EnumerateMonitorWorkAreas()
	{
		std::vector<RECT> Areas;
		::EnumDisplayMonitors(nullptr, nullptr, &collect_monitor, reinterpret_cast<LPARAM>(&Areas));
		return(Areas);
	}

	UINT GetMonitorDpiForPoint(POINT _Point, UINT _nFallback)
	{
		return(dpi_for_monitor(
			::MonitorFromPoint(_Point, MONITOR_DEFAULTTONEAREST),
			_nFallback ? _nFallback : USER_DEFAULT_SCREEN_DPI));
	}

	bool GetMonitorWorkAreaForWindow(HWND _hWnd, RECT* _pWorkArea, UINT* _pnDpi)
	{
		if (!_pWorkArea) { return(false); }
		const HMONITOR hMonitor = ::MonitorFromWindow(_hWnd, MONITOR_DEFAULTTONEAREST);
		MONITORINFO Information{};
		Information.cbSize = sizeof(Information);
		if (!hMonitor || !::GetMonitorInfoW(hMonitor, &Information)) { return(false); }
		*_pWorkArea = Information.rcWork;
		if (_pnDpi)
		{
			const UINT nWindowDpi = _hWnd ? ::GetDpiForWindow(_hWnd) : USER_DEFAULT_SCREEN_DPI;
			*_pnDpi = dpi_for_monitor(hMonitor, nWindowDpi ? nWindowDpi : USER_DEFAULT_SCREEN_DPI);
		}
		return(true);
	}

	RECT MakeFrameRectForClientDips(
		POINT _OriginPx, SIZE _ClientDips, UINT _nDpi,
		DWORD _nStyle, DWORD _nExStyle, bool _bHasMenu)
	{
		RECT Frame{ 0, 0, DipsToPixels(_ClientDips.cx, _nDpi), DipsToPixels(_ClientDips.cy, _nDpi) };
		::AdjustWindowRectExForDpi(&Frame, _nStyle, _bHasMenu, _nExStyle, _nDpi);
		::OffsetRect(&Frame, _OriginPx.x - Frame.left, _OriginPx.y - Frame.top);
		return(Frame);
	}

	RECT MakeCenteredDefaultFrame(
		const RECT& _WorkArea, UINT _nDpi, DWORD _nStyle, DWORD _nExStyle, bool _bHasMenu)
	{
		RECT Frame = MakeFrameRectForClientDips(
			{ 0, 0 }, { DEFAULT_CLIENT_WIDTH_DIP, DEFAULT_CLIENT_HEIGHT_DIP },
			_nDpi, _nStyle, _nExStyle, _bHasMenu);
		const int nWidth = Frame.right - Frame.left;
		const int nHeight = Frame.bottom - Frame.top;
		Frame.left = _WorkArea.left + ((_WorkArea.right - _WorkArea.left) - nWidth) / 2;
		Frame.top = _WorkArea.top + ((_WorkArea.bottom - _WorkArea.top) - nHeight) / 2;
		Frame.right = Frame.left + nWidth;
		Frame.bottom = Frame.top + nHeight;
		return(Frame);
	}

	bool CaptureWindowGeometry(HWND _hWnd, S_WINDOW_GEOMETRY* _pGeometry)
	{
		if (!_pGeometry || !::IsWindow(_hWnd) || ::IsIconic(_hWnd)) { return(false); }
		const RECT Frame = normal_frame_rect(_hWnd);
		if (Frame.right <= Frame.left || Frame.bottom <= Frame.top) { return(false); }
		UINT nDpi = ::GetDpiForWindow(_hWnd);
		if (nDpi == 0) { nDpi = USER_DEFAULT_SCREEN_DPI; }
		const HMONITOR hNormalMonitor = ::MonitorFromRect(&Frame, MONITOR_DEFAULTTONEAREST);
		nDpi = dpi_for_monitor(hNormalMonitor, nDpi);
		RECT Decoration{ 0, 0, 0, 0 };
		const DWORD nStyle = static_cast<DWORD>(::GetWindowLongPtrW(_hWnd, GWL_STYLE));
		const DWORD nExStyle = static_cast<DWORD>(::GetWindowLongPtrW(_hWnd, GWL_EXSTYLE));
		if (!::AdjustWindowRectExForDpi(
			&Decoration, nStyle, ::GetMenu(_hWnd) != nullptr, nExStyle, nDpi))
		{
			return(false);
		}
		const int nClientWidthPx = (Frame.right - Frame.left) - (Decoration.right - Decoration.left);
		const int nClientHeightPx = (Frame.bottom - Frame.top) - (Decoration.bottom - Decoration.top);
		S_WINDOW_GEOMETRY Geometry;
		Geometry.nFrameXpx = Frame.left;
		Geometry.nFrameYpx = Frame.top;
		Geometry.nClientWidthDip = PixelsToDips(nClientWidthPx, nDpi);
		Geometry.nClientHeightDip = PixelsToDips(nClientHeightPx, nDpi);
		Geometry.nDpi = nDpi;
		Geometry.bMaximized = ::IsZoomed(_hWnd) != FALSE;
		if (!valid_geometry(Geometry)) { return(false); }
		*_pGeometry = Geometry;
		return(true);
	}

	bool ResetWindowGeometry(HWND _hWnd)
	{
		if (!::IsWindow(_hWnd)) { return(false); }
		RECT WorkArea{};
		UINT nDpi = USER_DEFAULT_SCREEN_DPI;
		if (!GetMonitorWorkAreaForWindow(_hWnd, &WorkArea, &nDpi)) { return(false); }
		const DWORD nStyle = static_cast<DWORD>(::GetWindowLongPtrW(_hWnd, GWL_STYLE));
		const DWORD nExStyle = static_cast<DWORD>(::GetWindowLongPtrW(_hWnd, GWL_EXSTYLE));
		const RECT Frame = MakeCenteredDefaultFrame(
			WorkArea, nDpi, nStyle, nExStyle, ::GetMenu(_hWnd) != nullptr);
		::ShowWindow(_hWnd, SW_RESTORE);
		return(::SetWindowPos(_hWnd, nullptr, Frame.left, Frame.top,
			Frame.right - Frame.left, Frame.bottom - Frame.top,
			SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER) != FALSE);
	}

	C_WINDOW_SPLITTER::C_WINDOW_SPLITTER()
		: WTL::CSplitterWindowImpl<C_WINDOW_SPLITTER>(true)
	{
		m_cxyMin = 1;
		this->SetSplitterExtendedStyle(0, SPLIT_PROPORTIONAL | SPLIT_RIGHTALIGNED);
		m_cxySplitBar = DipsToPixels(SPLITTER_HANDLE_DIP, m_nDpi);
	}

	void C_WINDOW_SPLITTER::SetDpi(UINT _nDpi, bool _bUpdate)
	{
		m_nDpi = (std::clamp)(_nDpi, MIN_DPI, MAX_DPI);
		m_cxySplitBar = (std::max)(1, DipsToPixels(SPLITTER_HANDLE_DIP, m_nDpi));
		if (_bUpdate) { this->ApplyPendingSplit(); }
	}

	void C_WINDOW_SPLITTER::SetSplitSizesDip(
		const std::optional<std::pair<int, int>>& _Sizes)
	{
		m_SplitSizesDip = _Sizes;
		this->ApplyPendingSplit();
	}

	int C_WINDOW_SPLITTER::available_span_px_() const
	{
		const std::int64_t nSpan =
			static_cast<std::int64_t>(m_rcSplitter.right) -
			static_cast<std::int64_t>(m_rcSplitter.left) -
			static_cast<std::int64_t>(m_cxySplitBar) -
			static_cast<std::int64_t>(m_cxyBarEdge);
		return(static_cast<int>((std::clamp)(
			nSpan, std::int64_t{ 0 },
			static_cast<std::int64_t>((std::numeric_limits<int>::max)()))));
	}

	int C_WINDOW_SPLITTER::split_position_px_() const
	{
		const int nAvailablePx = this->available_span_px_();
		const int nAvailableDip = PixelsToDips(nAvailablePx, m_nDpi);
		const int nRequestedLeft = m_SplitSizesDip ? m_SplitSizesDip->first : nAvailableDip / 3;
		const auto Sizes = ClampPaneSizesDip(nRequestedLeft, nAvailableDip);
		return((std::clamp)(DipsToPixels(Sizes.first, m_nDpi), 1, (std::max)(1, nAvailablePx - 1)));
	}

	void C_WINDOW_SPLITTER::ApplyPendingSplit()
	{
		if (!this->IsWindow() || this->available_span_px_() < 2) { return; }
		m_bProgrammaticApply = true;
		this->SetSplitterPos(this->split_position_px_(), false);
		m_bProgrammaticApply = false;
		this->UpdateSplitterLayout();
	}

	bool C_WINDOW_SPLITTER::SetSplitterPos(int _nPosition, bool _bUpdate)
	{
		const int nAvailablePx = this->available_span_px_();
		if (nAvailablePx < 2)
		{
			return(WTL::CSplitterImpl<C_WINDOW_SPLITTER>::SetSplitterPos(_nPosition, _bUpdate));
		}
		if (_nPosition == -1) { _nPosition = this->split_position_px_(); }
		const int nAvailableDip = PixelsToDips(nAvailablePx, m_nDpi);
		const int nRequestedDip = PixelsToDips(_nPosition, m_nDpi);
		const auto Sizes = ClampPaneSizesDip(nRequestedDip, nAvailableDip);
		const int nCanonicalPosition = (std::clamp)(
			DipsToPixels(Sizes.first, m_nDpi), 1, nAvailablePx - 1);
		const bool bChanged = WTL::CSplitterImpl<C_WINDOW_SPLITTER>::SetSplitterPos(
			nCanonicalPosition, _bUpdate);
		if (!m_bProgrammaticApply) { m_SplitSizesDip = Sizes; }
		return(bChanged);
	}

	LRESULT C_WINDOW_SPLITTER::OnSize(UINT, WPARAM _wParam, LPARAM, BOOL& _bHandled)
	{
		if (_wParam != SIZE_MINIMIZED)
		{
			this->SetSplitterRect(nullptr, false);
			this->ApplyPendingSplit();
		}
		_bHandled = TRUE;
		return(0);
	}
}
