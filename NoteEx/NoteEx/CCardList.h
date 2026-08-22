#pragma once

// 카드 목록 자작 가상 컨트롤(W4 S1 [W4-render]). 원본 대조 정본은
// src/pynote/ui/cards/card_delegate.py(그리기·측정·말줄임)와 card_stream.py(픽셀 스크롤)다.
// 미리보기 예산·정렬·필터·선택 축소는 core C_CARD_LIST_PROJECTION 이 소유하므로 여기서
// 재구현하지 않는다.

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>

#include <D2DWrapp/D2DDef.h>
#include <D2DWrapp/D2DSwapTarget.h>
#include <D2DWrapp/D2DText.h>

#include "pynote/core/domain/time_zone_resolver.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace d2d
{
	class C_D2D_DEVICE;
	class C_D2D_BRUSH_CACHE;
}
namespace pynote::core::domain { class C_CARD_LIST_PROJECTION; }

// 원본 CardDelegate 의 클래스 상수를 그대로 옮긴 것이다(card_delegate.py:23~27, :70).
inline constexpr int CARD_HORIZONTAL_INSET_DIP = 4;
inline constexpr int CARD_VERTICAL_INSET_DIP = 3;
inline constexpr int CARD_CONTENT_HORIZONTAL_MARGIN_DIP = 14;
inline constexpr int CARD_CONTENT_VERTICAL_MARGIN_DIP = 10;
inline constexpr int CARD_AUXILIARY_ROW_PADDING_DIP = 4;
inline constexpr float CARD_CORNER_RADIUS_DIP = 6.0f;

// QRect 와 같은 닫힌 정수 구간이다(Right = Left + Width - 1). Win32 RECT 의 열린 구간과
// 섞으면 1 DIP 오차가 조용히 생기므로 별도 타입으로 못박는다 - 파이썬 단언이 QRect
// 의미의 right/bottom/contains 를 직접 본다(test_card_stream.py:157·:191).
struct S_DIP_RECT
{
	int nLeft{ 0 };
	int nTop{ 0 };
	int nWidth{ 0 };
	int nHeight{ 0 };

	int Right() const noexcept { return(nLeft + nWidth - 1); }
	int Bottom() const noexcept { return(nTop + nHeight - 1); }

	S_DIP_RECT Adjusted(int _nLeft, int _nTop, int _nRight, int _nBottom) const noexcept
	{
		return(S_DIP_RECT{ nLeft + _nLeft, nTop + _nTop,
			nWidth - _nLeft + _nRight, nHeight - _nTop + _nBottom });
	}

	bool Contains(const S_DIP_RECT& _Other) const noexcept
	{
		return(_Other.nLeft >= nLeft && _Other.nTop >= nTop &&
			_Other.Right() <= this->Right() && _Other.Bottom() <= this->Bottom());
	}

	bool operator==(const S_DIP_RECT&) const = default;
};

// ResolveCardPalette 의 입력. Win32 호출은 컨트롤이 하고 해석기는 순수 함수로 남는다
// (UNC-006 - 3모드 fixture 가 이 구조체만 채워 넣는다).
struct S_SYSTEM_COLORS
{
	COLORREF nWindow{ 0 };
	COLORREF nWindowText{ 0 };
	COLORREF nHighlight{ 0 };
	COLORREF nHighlightText{ 0 };
	COLORREF nBtnShadow{ 0 };
	COLORREF nGrayText{ 0 };

	bool operator==(const S_SYSTEM_COLORS&) const = default;
};

// D2DWrapp 브러시는 ARGB(0xAARRGGBB)를 받으므로 COLORREF(0x00BBGGRR)에서 여기서 뒤집는다.
struct S_CARD_PALETTE
{
	d2d::Color nBase{ 0 };
	d2d::Color nHoverBase{ 0 };
	d2d::Color nText{ 0 };
	d2d::Color nHighlight{ 0 };
	d2d::Color nHighlightText{ 0 };
	d2d::Color nBorder{ 0 };
	d2d::Color nPlaceholder{ 0 };

	bool operator==(const S_CARD_PALETTE&) const = default;
};

// Qt 역할 -> Win32 시스템 색 매핑. 고대비 여부는 매핑을 바꾸지 않는다(Qt 동등).
S_CARD_PALETTE ResolveCardPalette(const S_SYSTEM_COLORS& _Colors, bool _bHighContrast) noexcept;

// 원본 CardDelegate._preview_lines(card_delegate.py:169~222)의 네이티브 쌍둥이다.
// _bTruncated 는 core 의 판정값이며 파이썬처럼 인자로 받는다(줄 수가 남아도 모델이
// 뒷부분을 잘랐으면 말줄임표가 선다).
std::vector<std::wstring> ComputeDisplayLines(
	std::wstring_view _sText, bool _bTruncated, d2d::C_D2D_TEXT& _Text,
	IDWriteTextFormat* _pFormat, int _nContentWidthDip, std::size_t _nMaxLines);

// 빈 sFamily / 0 이하 fSizeDip 은 "시스템 메시지 폰트로 해석하라"는 뜻이다.
struct S_CARD_LIST_FONT
{
	std::wstring sFamily;
	float fSizeDip{ 0.0f };
	DWRITE_FONT_WEIGHT eWeight{ DWRITE_FONT_WEIGHT_NORMAL };
};

struct S_CARD_LIST_DISPLAY
{
	std::wstring sTimeFormat{ L"yyyy-MM-dd HH:mm" };
	std::wstring sTimeZone{ L"system" };
	S_CARD_LIST_FONT Font{};
};

// 프레임 관측 seam(P2 last_frame() 선례). 그리기에 쓴 변수를 그대로 기록하며 그리기
// 자체를 바꾸지 않는다 - 파이썬 시험의 _RecordingPainter.drawn_text 에 대응한다.
struct S_CARD_TEXT_RUN
{
	S_DIP_RECT Rect{};
	std::wstring sText;
	bool bDrawn{ false };
};

struct S_CARD_LIST_ROW_FRAME
{
	std::size_t nRow{ 0 };
	std::string sCardId;
	S_DIP_RECT RowRect{};
	S_DIP_RECT CardRect{};
	S_DIP_RECT ContentRect{};
	d2d::Color nFillColor{ 0 };
	d2d::Color nTextColor{ 0 };
	d2d::Color nBorderColor{ 0 };
	bool bHovered{ false };
	bool bSelected{ false };
	std::vector<S_CARD_TEXT_RUN> BodyLines;
	S_CARD_TEXT_RUN TimeRun{};
	S_CARD_TEXT_RUN SuffixRun{};
};

struct S_CARD_LIST_FRAME
{
	std::size_t nFirstVisibleRow{ 0 };
	std::size_t nLastVisibleRow{ 0 };
	// 이 프레임에서 실제로 그린 텍스트 런 수다(카드당 본문 N + 보조행 1~2).
	std::size_t nLayoutCount{ 0 };
	bool bPresented{ false };
	std::vector<S_CARD_LIST_ROW_FRAME> Rows;
};

class C_CARD_LIST final : public CWindowImpl<C_CARD_LIST>
{
public:
	DECLARE_WND_CLASS_EX(L"NoteExCardList", CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW, -1)

	using ActivateHandler = std::function<void()>;

	C_CARD_LIST();
	~C_CARD_LIST();
	C_CARD_LIST(const C_CARD_LIST&) = delete;
	C_CARD_LIST& operator=(const C_CARD_LIST&) = delete;

	void Bind(pynote::core::domain::C_CARD_LIST_PROJECTION& _Projection) noexcept;
	void SetActivateHandler(ActivateHandler _Handler);
	// 디바이스·브러시 캐시·텍스트 엔진은 CApplication 소유다. 붙지 않으면 그리지 않고
	// 나머지(행·LB 메시지·Enter·스크롤 산술)는 그대로 동작한다.
	void AttachRenderServices(d2d::C_D2D_DEVICE* _pDevice,
		d2d::C_D2D_BRUSH_CACHE* _pBrushCache, d2d::C_D2D_TEXT* _pText) noexcept;
	void SetDisplaySettings(const S_CARD_LIST_DISPLAY& _Display);

	// 프로젝션 행 집합이 바뀐 뒤 호출한다 - 내용 높이·스크롤 클램프·스크롤바를 다시 잡는다.
	void OnProjectionChanged();

	std::size_t RowCount() const noexcept;
	int LineSpacingDip() const;
	int RowHeightDip() const;
	int ViewportWidthDip() const;
	int ViewportHeightDip() const;
	int ContentHeightDip() const;
	S_DIP_RECT RowRectDip(std::size_t _nRow) const;

	int ScrollOffsetDip() const noexcept { return(m_nScrollOffsetDip); }
	void ScrollToPixel(int _nOffsetDip);
	void EnsureVisible(std::size_t _nRow);

	// 동기 페인트 1회. 반환값은 present 성공 여부다.
	bool Render();
	const S_CARD_LIST_FRAME& LastFrame() const noexcept { return(m_Frame); }
	const S_CARD_PALETTE& Palette() const noexcept { return(m_Palette); }
	std::wstring TimeLabel(std::int64_t _nEpochUs) const;

	BEGIN_MSG_MAP(C_CARD_LIST)
		MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBkgnd)
		MESSAGE_HANDLER(WM_PAINT, OnPaint)
		MESSAGE_HANDLER(WM_SIZE, OnSize)
		MESSAGE_HANDLER(WM_DPICHANGED_AFTERPARENT, OnDpiChangedAfterParent)
		MESSAGE_HANDLER(WM_VSCROLL, OnVScroll)
		MESSAGE_HANDLER(WM_MOUSEMOVE, OnMouseMove)
		MESSAGE_HANDLER(WM_MOUSELEAVE, OnMouseLeave)
		MESSAGE_HANDLER(WM_KEYDOWN, OnKeyDown)
		MESSAGE_HANDLER(WM_SETFOCUS, OnFocusChanged)
		MESSAGE_HANDLER(WM_KILLFOCUS, OnFocusChanged)
		MESSAGE_HANDLER(WM_SYSCOLORCHANGE, OnPaletteChanged)
		MESSAGE_HANDLER(WM_THEMECHANGED, OnPaletteChanged)
		MESSAGE_HANDLER(WM_SETTINGCHANGE, OnPaletteChanged)
		MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
		MESSAGE_HANDLER(LB_GETCOUNT, OnListGetCount)
		MESSAGE_HANDLER(LB_GETCURSEL, OnListGetCurSel)
		MESSAGE_HANDLER(LB_SETCURSEL, OnListSetCurSel)
		MESSAGE_HANDLER(LB_GETTOPINDEX, OnListGetTopIndex)
		MESSAGE_HANDLER(LB_SETTOPINDEX, OnListSetTopIndex)
	END_MSG_MAP()

	LRESULT OnEraseBkgnd(UINT, WPARAM, LPARAM, BOOL&) { return(1); }
	LRESULT OnPaint(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnSize(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnDpiChangedAfterParent(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnVScroll(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnMouseMove(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnMouseLeave(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnKeyDown(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnFocusChanged(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnPaletteChanged(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnListGetCount(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnListGetCurSel(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnListSetCurSel(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnListGetTopIndex(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnListSetTopIndex(UINT, WPARAM, LPARAM, BOOL&);

private:
	int client_dip_(int _nPixels) const noexcept;
	bool ensure_target_();
	void handle_device_lost_();
	void capture_palette_();
	void resolve_font_();
	IDWriteTextFormat* text_format_() const;
	int max_scroll_offset_dip_() const;
	void update_scroll_bar_();
	void invalidate_row_(std::optional<std::size_t> _nRow);
	std::optional<std::size_t> row_at_dip_(int _nYdip) const;
	std::optional<std::size_t> current_row_() const;
	bool render_();
	bool draw_text_(ID2D1DeviceContext* _pDc, IDWriteTextFormat* _pFormat,
		ID2D1Brush* _pBrush, const S_DIP_RECT& _Rect, const std::wstring& _sText);
	void draw_row_(ID2D1DeviceContext* _pDc, std::size_t _nRow, IDWriteTextFormat* _pFormat);

	pynote::core::domain::C_CARD_LIST_PROJECTION* m_pProjection{ nullptr };
	ActivateHandler m_Activate;
	d2d::C_D2D_DEVICE* m_pDevice{ nullptr };
	d2d::C_D2D_BRUSH_CACHE* m_pBrushCache{ nullptr };
	d2d::C_D2D_TEXT* m_pText{ nullptr };
	d2d::C_D2D_SWAP_TARGET m_Target;
	bool m_bTargetReady{ false };
	bool m_bRecoveringDevice{ false };
	S_CARD_LIST_DISPLAY m_Display;
	std::string m_sTimeFormatUtf8;
	std::string m_sTimeZoneUtf8;
	S_CARD_LIST_FONT m_Font;
	pynote::core::domain::C_TIME_ZONE_RESOLVER m_TimeZone;
	S_CARD_PALETTE m_Palette;
	S_CARD_LIST_FRAME m_Frame;
	int m_nScrollOffsetDip{ 0 };
	UINT m_nDpi{ USER_DEFAULT_SCREEN_DPI };
	// 줄 높이는 폰트·DPI 가 바뀔 때만 달라지는데 스크롤 산술이 매번 부르므로 캐시한다.
	mutable int m_nLineSpacingDip{ 0 };
	std::optional<std::size_t> m_nHoverRow{};
	bool m_bTrackingMouse{ false };
};
