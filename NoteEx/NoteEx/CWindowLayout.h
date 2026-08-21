#pragma once

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>
#include <atlsplit.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pynote::shell
{
	constexpr int DEFAULT_CLIENT_WIDTH_DIP = 960;
	constexpr int DEFAULT_CLIENT_HEIGHT_DIP = 640;
	constexpr int SPLITTER_HANDLE_DIP = 4;
	constexpr int SPLITTER_LEFT_MIN_DIP = 260;
	constexpr int SPLITTER_RIGHT_MIN_DIP = 380;

	struct S_WINDOW_GEOMETRY
	{
		std::int32_t nFrameXpx{ 0 };
		std::int32_t nFrameYpx{ 0 };
		std::int32_t nClientWidthDip{ DEFAULT_CLIENT_WIDTH_DIP };
		std::int32_t nClientHeightDip{ DEFAULT_CLIENT_HEIGHT_DIP };
		std::uint32_t nDpi{ USER_DEFAULT_SCREEN_DPI };
		bool bMaximized{ false };
		bool operator==(const S_WINDOW_GEOMETRY&) const = default;
	};

	std::string WindowGeometryKey(const std::string& _sStableWindowId);
	std::vector<std::uint8_t> EncodeWindowGeometry(const S_WINDOW_GEOMETRY& _Geometry);
	bool DecodeWindowGeometry(
		const std::vector<std::uint8_t>& _Bytes, S_WINDOW_GEOMETRY* _pGeometry);

	int DipsToPixels(int _nDips, UINT _nDpi);
	int PixelsToDips(int _nPixels, UINT _nDpi);
	std::pair<int, int> ClampPaneSizesDip(int _nRequestedLeftDip, int _nAvailableSpanDip);

	bool IntersectsMonitorWorkArea(const RECT& _Frame, const std::vector<RECT>& _WorkAreas);
	std::vector<RECT> EnumerateMonitorWorkAreas();
	UINT GetMonitorDpiForPoint(POINT _Point, UINT _nFallback = USER_DEFAULT_SCREEN_DPI);
	bool GetMonitorWorkAreaForWindow(HWND _hWnd, RECT* _pWorkArea, UINT* _pnDpi = nullptr);
	RECT MakeFrameRectForClientDips(
		POINT _OriginPx, SIZE _ClientDips, UINT _nDpi,
		DWORD _nStyle, DWORD _nExStyle, bool _bHasMenu);
	RECT MakeCenteredDefaultFrame(
		const RECT& _WorkArea, UINT _nDpi, DWORD _nStyle, DWORD _nExStyle, bool _bHasMenu);
	bool CaptureWindowGeometry(HWND _hWnd, S_WINDOW_GEOMETRY* _pGeometry);
	bool ResetWindowGeometry(HWND _hWnd);

	class C_WINDOW_SPLITTER final :
		public WTL::CSplitterWindowImpl<C_WINDOW_SPLITTER>
	{
	public:
		DECLARE_WND_CLASS_EX(L"NoteExWindowSplitter", CS_DBLCLKS, COLOR_WINDOW)

		C_WINDOW_SPLITTER();

		void SetDpi(UINT _nDpi, bool _bUpdate);
		UINT Dpi() const noexcept { return(m_nDpi); }
		void SetSplitSizesDip(const std::optional<std::pair<int, int>>& _Sizes);
		std::optional<std::pair<int, int>> SplitSizesDip() const noexcept { return(m_SplitSizesDip); }
		void ApplyPendingSplit();

		// CSplitterImpl calls this through CRTP for both programmatic and drag paths.
		// Keeping the override here is what prevents either pane from collapsing.
		bool SetSplitterPos(int _nPosition = -1, bool _bUpdate = true);
		bool SetSinglePaneMode(int = SPLIT_PANE_NONE) = delete;

		BEGIN_MSG_MAP(C_WINDOW_SPLITTER)
			MESSAGE_HANDLER(WM_SIZE, OnSize)
			CHAIN_MSG_MAP(WTL::CSplitterWindowImpl<C_WINDOW_SPLITTER>)
		END_MSG_MAP()

		LRESULT OnSize(UINT, WPARAM _wParam, LPARAM, BOOL& _bHandled);

	private:
		int available_span_px_() const;
		int split_position_px_() const;

		UINT m_nDpi{ USER_DEFAULT_SCREEN_DPI };
		std::optional<std::pair<int, int>> m_SplitSizesDip{};
		bool m_bProgrammaticApply{ false };
	};
}
