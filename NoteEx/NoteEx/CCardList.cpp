#include "CCardList.h"

#include <D2DWrapp/D2DBrushCache.h>
#include <D2DWrapp/D2DDevice.h>

#include "pynote/core/domain/card_list_projection.h"
#include "pynote/core/domain/date_time_formatter.h"
#include "pynote/platform/win32_time_zone_resolver.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <utility>

#pragma comment(lib, "D2DWrapp")

namespace
{
	namespace domain = pynote::core::domain;

	// 말줄임표는 U+2026 한 글자다(원본 f"{displayed[-1]}…", card_delegate.py:216).
	constexpr wchar_t ELLIPSIS[] = L"…";
	// 미리보기 레이아웃의 높이 한도. D2DWrapp 기본값과 같은 "사실상 무제한"이다.
	constexpr float PREVIEW_LAYOUT_MAX_HEIGHT_DIP = 100000.0f;

	bool is_high_surrogate(wchar_t _ch) noexcept { return(_ch >= 0xD800 && _ch <= 0xDBFF); }
	bool is_low_surrogate(wchar_t _ch) noexcept { return(_ch >= 0xDC00 && _ch <= 0xDFFF); }

	// 마지막 코드포인트가 차지하는 UTF-16 코드유닛 수. 서로게이트 쌍은 통째로 센다.
	std::size_t trailing_units(const std::wstring& _sValue) noexcept
	{
		if (_sValue.empty()) { return(0); }
		if (_sValue.size() >= 2 && is_low_surrogate(_sValue.back()) &&
			is_high_surrogate(_sValue[_sValue.size() - 2])) { return(2); }
		return(1);
	}

	// CDocumentPage.cpp 의 wide() 와 같은 계약이다 - 불량 UTF-8 은 빈 문자열로 접는다.
	std::wstring wide(const std::string& _sValue)
	{
		if (_sValue.empty()) { return(std::wstring{}); }
		const int nSize = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			_sValue.data(), static_cast<int>(_sValue.size()), nullptr, 0);
		if (nSize <= 0) { return(std::wstring{}); }
		std::wstring Result(static_cast<std::size_t>(nSize), L'\0');
		return(::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, _sValue.data(),
			static_cast<int>(_sValue.size()), Result.data(), nSize) == nSize ? Result : std::wstring{});
	}

	std::string narrow(const std::wstring& _sValue)
	{
		if (_sValue.empty()) { return(std::string{}); }
		const int nSize = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
			_sValue.data(), static_cast<int>(_sValue.size()), nullptr, 0, nullptr, nullptr);
		if (nSize <= 0) { return(std::string{}); }
		std::string Result(static_cast<std::size_t>(nSize), '\0');
		return(::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, _sValue.data(),
			static_cast<int>(_sValue.size()), Result.data(), nSize, nullptr, nullptr) == nSize ?
			Result : std::string{});
	}

	// Qt QFontMetrics::horizontalAdvance 가 정수를 돌려주므로 여기서도 정수로 접는다 -
	// 카드 사각 산술 전체가 정수 DIP 다(card_delegate.py:233~256).
	int advance_dip(d2d::C_D2D_TEXT& _Text, IDWriteTextFormat* _pFormat, const std::wstring& _sValue)
	{
		if (_sValue.empty() || !_pFormat) { return(0); }
		return(static_cast<int>(std::lround(_Text.Measure(_sValue.c_str(), _pFormat, 0.0f).width)));
	}

	// Qt QFontMetrics::elidedText(_, ElideRight, width) 등가. 들어온 문자열이 이미 폭 안이면
	// 그대로 돌려주고, 넘치면 코드포인트 단위로 줄이며 뒤에 말줄임표를 붙인다.
	std::wstring elide_right(d2d::C_D2D_TEXT& _Text, IDWriteTextFormat* _pFormat,
		const std::wstring& _sValue, int _nWidthDip)
	{
		if (advance_dip(_Text, _pFormat, _sValue) <= _nWidthDip) { return(_sValue); }
		std::wstring Prefix = _sValue;
		while (!Prefix.empty())
		{
			Prefix.resize(Prefix.size() - trailing_units(Prefix));
			std::wstring Candidate = Prefix + ELLIPSIS;
			if (advance_dip(_Text, _pFormat, Candidate) <= _nWidthDip) { return(Candidate); }
		}
		return(std::wstring(ELLIPSIS));
	}

	// COLORREF 는 0x00BBGGRR 이고 D2DWrapp 브러시는 0xAARRGGBB 다.
	d2d::Color to_argb(COLORREF _nColor) noexcept
	{
		return(0xFF000000u |
			(static_cast<d2d::Color>(GetRValue(_nColor)) << 16) |
			(static_cast<d2d::Color>(GetGValue(_nColor)) << 8) |
			static_cast<d2d::Color>(GetBValue(_nColor)));
	}

	// Qt QColor::lighter(104) 이식(card_delegate.py:60 background.lighter(104)).
	// HSV 로 옮겨 명도만 104% 로 올리고, 255 를 넘으면 넘친 만큼 채도를 깎는다.
	d2d::Color lighter_104(d2d::Color _argb) noexcept
	{
		const int nRed = static_cast<int>((_argb >> 16) & 0xFF);
		const int nGreen = static_cast<int>((_argb >> 8) & 0xFF);
		const int nBlue = static_cast<int>(_argb & 0xFF);
		const int nMax = (std::max)(nRed, (std::max)(nGreen, nBlue));
		const int nMin = (std::min)(nRed, (std::min)(nGreen, nBlue));
		const int nDelta = nMax - nMin;
		int nValue = nMax;
		int nSaturation = nMax == 0 ? 0 : (nDelta * 255 + nMax / 2) / nMax;
		double dHue = 0.0;
		if (nDelta != 0)
		{
			const double dSpan = static_cast<double>(nDelta);
			if (nMax == nRed) { dHue = (nGreen - nBlue) / dSpan; }
			else if (nMax == nGreen) { dHue = 2.0 + (nBlue - nRed) / dSpan; }
			else { dHue = 4.0 + (nRed - nGreen) / dSpan; }
			dHue *= 60.0;
			if (dHue < 0.0) { dHue += 360.0; }
		}
		nValue = nValue * 104 / 100;
		if (nValue > 255)
		{
			nSaturation -= nValue - 255;
			if (nSaturation < 0) { nSaturation = 0; }
			nValue = 255;
		}
		const d2d::Color nAlpha = _argb & 0xFF000000u;
		if (nDelta == 0 || nSaturation == 0)
		{
			const d2d::Color nGray = static_cast<d2d::Color>(nValue);
			return(nAlpha | (nGray << 16) | (nGray << 8) | nGray);
		}
		const double dSaturation = nSaturation / 255.0;
		const double dValue = nValue / 255.0;
		const double dSector = dHue / 60.0;
		const double dFraction = dSector - std::floor(dSector);
		const int nSector = static_cast<int>(std::floor(dSector)) % 6;
		const double dLow = dValue * (1.0 - dSaturation);
		const double dFalling = dValue * (1.0 - dSaturation * dFraction);
		const double dRising = dValue * (1.0 - dSaturation * (1.0 - dFraction));
		double dRed = dValue;
		double dGreen = dRising;
		double dBlue = dLow;
		switch (nSector)
		{
		case 0: dRed = dValue; dGreen = dRising; dBlue = dLow; break;
		case 1: dRed = dFalling; dGreen = dValue; dBlue = dLow; break;
		case 2: dRed = dLow; dGreen = dValue; dBlue = dRising; break;
		case 3: dRed = dLow; dGreen = dFalling; dBlue = dValue; break;
		case 4: dRed = dRising; dGreen = dLow; dBlue = dValue; break;
		default: dRed = dValue; dGreen = dLow; dBlue = dFalling; break;
		}
		const auto channel = [](double _dValue) noexcept
		{
			return(static_cast<d2d::Color>((std::min<long>)(255, (std::max<long>)(0, std::lround(_dValue * 255.0)))));
		};
		return(nAlpha | (channel(dRed) << 16) | (channel(dGreen) << 8) | channel(dBlue));
	}
}

S_CARD_PALETTE ResolveCardPalette(const S_SYSTEM_COLORS& _Colors, bool) noexcept
{
	// 고대비 모드는 역할 매핑을 바꾸지 않는다 - Qt 도 팔레트 역할만 읽고 모드로 분기하지
	// 않는다(파이썬 동등). 플래그는 호출자가 모드를 실제로 관측했다는 계약으로만 받는다.
	// Base/Text/Highlight/HighlightedText 는 시스템 색끼리의 대응이고, Mid(테두리)=BTNSHADOW,
	// PlaceholderText(시각행)=GRAYTEXT 는 Qt 파생색의 가장 가까운 Win32 등가다(적응 기록).
	S_CARD_PALETTE Palette;
	Palette.nBase = to_argb(_Colors.nWindow);
	Palette.nText = to_argb(_Colors.nWindowText);
	Palette.nHighlight = to_argb(_Colors.nHighlight);
	Palette.nHighlightText = to_argb(_Colors.nHighlightText);
	Palette.nBorder = to_argb(_Colors.nBtnShadow);
	Palette.nPlaceholder = to_argb(_Colors.nGrayText);
	Palette.nHoverBase = lighter_104(Palette.nBase);
	return(Palette);
}

std::vector<std::wstring> ComputeDisplayLines(
	std::wstring_view _sText, bool _bTruncated, d2d::C_D2D_TEXT& _Text,
	IDWriteTextFormat* _pFormat, int _nContentWidthDip, std::size_t _nMaxLines)
{
	// 원본 호출부는 항상 max(1, ...) 를 넘긴다(card_delegate.py:82~85·:159~163).
	const std::size_t nMaxLines = (std::max<std::size_t>)(1, _nMaxLines);
	const int nWidth = (std::max)(1, _nContentWidthDip);

	// 원본은 개행을 U+2028 로 접었다가 줄마다 다시 벗긴다. DirectWrite 는 LF 에서 직접
	// 줄을 끊으므로 CRLF/CR 만 LF 로 정규화하면 같은 줄 경계가 나오고 U+2028 도 남지 않는다.
	std::wstring sLayout;
	sLayout.reserve(_sText.size());
	for (std::size_t nIndex = 0; nIndex < _sText.size(); ++nIndex)
	{
		if (_sText[nIndex] == L'\r')
		{
			if (nIndex + 1 < _sText.size() && _sText[nIndex + 1] == L'\n') { ++nIndex; }
			sLayout.push_back(L'\n');
		}
		else { sLayout.push_back(_sText[nIndex]); }
	}

	std::vector<std::wstring> Visual;
	if (_pFormat)
	{
		// maxLines=0 이다 - 마지막 줄 말줄임을 D2DWrapp trimming 에 맡기지 않는다(지시서 §2).
		d2d::C_D2D_TEXT_LAYOUT Layout = _Text.CreateLayout(sLayout.c_str(), _pFormat,
			static_cast<float>(nWidth), PREVIEW_LAYOUT_MAX_HEIGHT_DIP, 0);
		if (Layout.IsValid())
		{
			UINT32 uStart = 0;
			for (const DWRITE_LINE_METRICS& Line : Layout.GetLineMetrics())
			{
				Visual.push_back(Layout.Slice(uStart, Line.length - Line.newlineLength));
				uStart += Line.length;
				// 원본 루프는 한도를 한 줄 넘긴 자리에서 멈춘다 - 그 한 줄은 overflow 판정에만 쓴다.
				if (Visual.size() > nMaxLines) { break; }
			}
		}
	}
	if (Visual.empty()) { Visual.emplace_back(); }

	const bool bOverflow = Visual.size() > nMaxLines || _bTruncated;
	if (Visual.size() > nMaxLines) { Visual.resize(nMaxLines); }
	if (bOverflow) { Visual.back() = elide_right(_Text, _pFormat, Visual.back() + ELLIPSIS, nWidth); }
	return(Visual);
}

C_CARD_LIST::C_CARD_LIST()
	: m_TimeZone(pynote::platform::MakeWin32SystemTimeZoneResolver())
{
	m_sTimeFormatUtf8 = narrow(m_Display.sTimeFormat);
	m_sTimeZoneUtf8 = narrow(m_Display.sTimeZone);
	this->resolve_font_();
	this->capture_palette_();
}

C_CARD_LIST::~C_CARD_LIST()
{
	// 창이 살아 있는 채로 객체가 죽으면 ATL thunk 가 떠난 자리를 가리킨다.
	if (this->IsWindow()) { this->DestroyWindow(); }
	m_Target.Shutdown();
}

void C_CARD_LIST::Bind(domain::C_CARD_LIST_PROJECTION& _Projection) noexcept
{
	m_pProjection = &_Projection;
}

void C_CARD_LIST::SetActivateHandler(ActivateHandler _Handler)
{
	m_Activate = std::move(_Handler);
}

void C_CARD_LIST::AttachRenderServices(d2d::C_D2D_DEVICE* _pDevice,
	d2d::C_D2D_BRUSH_CACHE* _pBrushCache, d2d::C_D2D_TEXT* _pText) noexcept
{
	m_pDevice = _pDevice;
	m_pBrushCache = _pBrushCache;
	m_pText = _pText;
	m_nLineSpacingDip = 0;
}

void C_CARD_LIST::SetDisplaySettings(const S_CARD_LIST_DISPLAY& _Display)
{
	m_Display = _Display;
	m_sTimeFormatUtf8 = narrow(m_Display.sTimeFormat);
	m_sTimeZoneUtf8 = narrow(m_Display.sTimeZone);
	this->resolve_font_();
	m_nLineSpacingDip = 0;
	if (this->IsWindow())
	{
		this->update_scroll_bar_();
		this->Invalidate(FALSE);
	}
}

void C_CARD_LIST::OnProjectionChanged()
{
	m_nHoverRow.reset();
	this->ScrollToPixel(m_nScrollOffsetDip);
	this->update_scroll_bar_();
	if (this->IsWindow()) { this->Invalidate(FALSE); }
}

std::size_t C_CARD_LIST::RowCount() const noexcept
{
	return(m_pProjection ? m_pProjection->RowCount() : 0);
}

int C_CARD_LIST::LineSpacingDip() const
{
	if (m_nLineSpacingDip > 0) { return(m_nLineSpacingDip); }
	IDWriteTextFormat* pFormat = this->text_format_();
	if (pFormat)
	{
		m_nLineSpacingDip = (std::max)(1,
			static_cast<int>(std::ceil(m_pText->Measure(L"Ag", pFormat, 0.0f).height)));
	}
	else
	{
		// CEILING: 렌더 서비스가 없으면 DirectWrite 로 줄 높이를 잴 수 없다. 스크롤 산술이
		// 0 으로 나누지 않도록 폰트 em 크기를 쓴다 - 그리지 않는 소비자(W3 페이지 시험)만
		// 이 경로를 타고 어떤 게이트도 이 값을 단언하지 않는다. 서비스가 붙으면 실측값으로
		// 대체된다(AttachRenderServices 가 캐시를 무효화한다).
		m_nLineSpacingDip = (std::max)(1, static_cast<int>(std::lround(m_Font.fSizeDip)));
	}
	return(m_nLineSpacingDip);
}

int C_CARD_LIST::RowHeightDip() const
{
	// 원본 CardDelegate.sizeHint(card_delegate.py:150~167) 의 공식 그대로다.
	const int nLine = this->LineSpacingDip();
	const std::size_t nPreview = m_pProjection ? m_pProjection->PreviewLineCount() : 1;
	return(2 * CARD_VERTICAL_INSET_DIP + 2 * CARD_CONTENT_VERTICAL_MARGIN_DIP +
		static_cast<int>(nPreview) * nLine + (nLine + CARD_AUXILIARY_ROW_PADDING_DIP));
}

int C_CARD_LIST::client_dip_(int _nPixels) const noexcept
{
	return(::MulDiv(_nPixels, USER_DEFAULT_SCREEN_DPI,
		static_cast<int>((std::max<UINT>)(USER_DEFAULT_SCREEN_DPI, m_nDpi))));
}

int C_CARD_LIST::ViewportWidthDip() const
{
	if (!this->IsWindow()) { return(0); }
	RECT Client{};
	::GetClientRect(this->m_hWnd, &Client);
	return((std::max)(0, this->client_dip_(static_cast<int>(Client.right - Client.left))));
}

int C_CARD_LIST::ViewportHeightDip() const
{
	if (!this->IsWindow()) { return(0); }
	RECT Client{};
	::GetClientRect(this->m_hWnd, &Client);
	return((std::max)(0, this->client_dip_(static_cast<int>(Client.bottom - Client.top))));
}

int C_CARD_LIST::ContentHeightDip() const
{
	return(static_cast<int>(this->RowCount()) * this->RowHeightDip());
}

S_DIP_RECT C_CARD_LIST::RowRectDip(std::size_t _nRow) const
{
	const int nRowHeight = this->RowHeightDip();
	return(S_DIP_RECT{ 0, static_cast<int>(_nRow) * nRowHeight - m_nScrollOffsetDip,
		this->ViewportWidthDip(), nRowHeight });
}

int C_CARD_LIST::max_scroll_offset_dip_() const
{
	return((std::max)(0, this->ContentHeightDip() - this->ViewportHeightDip()));
}

void C_CARD_LIST::ScrollToPixel(int _nOffsetDip)
{
	const int nTarget = (std::min)(this->max_scroll_offset_dip_(), (std::max)(0, _nOffsetDip));
	if (nTarget == m_nScrollOffsetDip) { this->update_scroll_bar_(); return; }
	m_nScrollOffsetDip = nTarget;
	this->update_scroll_bar_();
	if (this->IsWindow()) { this->Invalidate(FALSE); }
}

void C_CARD_LIST::EnsureVisible(std::size_t _nRow)
{
	// Qt QAbstractItemView.EnsureVisible: 위로 벗어나면 행 위쪽을, 아래로 벗어나면 행
	// 아래쪽을 뷰포트에 맞춘다. 이미 보이면 그대로 둔다.
	if (_nRow >= this->RowCount()) { return; }
	const int nRowHeight = this->RowHeightDip();
	const int nTop = static_cast<int>(_nRow) * nRowHeight;
	if (nTop < m_nScrollOffsetDip) { this->ScrollToPixel(nTop); return; }
	const int nBottom = nTop + nRowHeight;
	const int nViewport = this->ViewportHeightDip();
	if (nBottom > m_nScrollOffsetDip + nViewport) { this->ScrollToPixel(nBottom - nViewport); }
}

void C_CARD_LIST::update_scroll_bar_()
{
	if (!this->IsWindow()) { return; }
	SCROLLINFO Info{};
	Info.cbSize = sizeof(Info);
	// 스크롤바를 감추면 클라이언트 폭이 흔들려 행 폭이 프레임마다 달라진다 - 항상 두고 비활성만 한다.
	Info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
	Info.nMin = 0;
	Info.nMax = (std::max)(0, this->ContentHeightDip() - 1);
	Info.nPage = static_cast<UINT>((std::max)(0, this->ViewportHeightDip()));
	Info.nPos = m_nScrollOffsetDip;
	::SetScrollInfo(this->m_hWnd, SB_VERT, &Info, TRUE);
}

void C_CARD_LIST::invalidate_row_(std::optional<std::size_t> _nRow)
{
	if (!_nRow || !this->IsWindow()) { return; }
	const S_DIP_RECT Row = this->RowRectDip(*_nRow);
	const int nDpi = static_cast<int>((std::max<UINT>)(USER_DEFAULT_SCREEN_DPI, m_nDpi));
	RECT Invalid{
		0,
		::MulDiv(Row.nTop, nDpi, USER_DEFAULT_SCREEN_DPI),
		::MulDiv(Row.nLeft + Row.nWidth, nDpi, USER_DEFAULT_SCREEN_DPI),
		::MulDiv(Row.nTop + Row.nHeight, nDpi, USER_DEFAULT_SCREEN_DPI) };
	::InvalidateRect(this->m_hWnd, &Invalid, FALSE);
}

std::optional<std::size_t> C_CARD_LIST::row_at_dip_(int _nYdip) const
{
	const int nRowHeight = this->RowHeightDip();
	if (nRowHeight <= 0 || _nYdip < 0) { return(std::nullopt); }
	const long long nIndex = (static_cast<long long>(_nYdip) + m_nScrollOffsetDip) / nRowHeight;
	if (nIndex < 0 || static_cast<unsigned long long>(nIndex) >= this->RowCount()) { return(std::nullopt); }
	return(static_cast<std::size_t>(nIndex));
}

std::optional<std::size_t> C_CARD_LIST::current_row_() const
{
	if (!m_pProjection) { return(std::nullopt); }
	const auto& sCurrent = m_pProjection->CurrentCardId();
	// 현재 카드가 행을 잃었으면(필터·삭제) 선택 없음으로 읽는다.
	return(sCurrent ? m_pProjection->RowForCard(*sCurrent) : std::nullopt);
}

void C_CARD_LIST::capture_palette_()
{
	S_SYSTEM_COLORS Colors;
	Colors.nWindow = ::GetSysColor(COLOR_WINDOW);
	Colors.nWindowText = ::GetSysColor(COLOR_WINDOWTEXT);
	Colors.nHighlight = ::GetSysColor(COLOR_HIGHLIGHT);
	Colors.nHighlightText = ::GetSysColor(COLOR_HIGHLIGHTTEXT);
	Colors.nBtnShadow = ::GetSysColor(COLOR_BTNSHADOW);
	Colors.nGrayText = ::GetSysColor(COLOR_GRAYTEXT);
	HIGHCONTRASTW Contrast{};
	Contrast.cbSize = sizeof(Contrast);
	const bool bHighContrast =
		::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(Contrast), &Contrast, 0) != FALSE &&
		(Contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
	m_Palette = ResolveCardPalette(Colors, bHighContrast);
}

void C_CARD_LIST::resolve_font_()
{
	m_Font = m_Display.Font;
	if (!m_Font.sFamily.empty() && m_Font.fSizeDip > 0.0f) { return; }
	NONCLIENTMETRICSW Metrics{};
	Metrics.cbSize = sizeof(Metrics);
	if (::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(Metrics), &Metrics, 0,
		USER_DEFAULT_SCREEN_DPI) != FALSE)
	{
		if (m_Font.sFamily.empty()) { m_Font.sFamily = Metrics.lfMessageFont.lfFaceName; }
		if (m_Font.fSizeDip <= 0.0f)
		{
			const LONG nHeight = Metrics.lfMessageFont.lfHeight;
			m_Font.fSizeDip = static_cast<float>((std::max<LONG>)(1, nHeight < 0 ? -nHeight : nHeight));
		}
	}
	// 시스템 메트릭 조회가 실패하면 포맷 생성이 실패해 목록 전체가 조용히 비어 버린다.
	if (m_Font.sFamily.empty()) { m_Font.sFamily = L"Segoe UI"; }
	if (m_Font.fSizeDip <= 0.0f) { m_Font.fSizeDip = 12.0f; }
}

IDWriteTextFormat* C_CARD_LIST::text_format_() const
{
	return(m_pText ? m_pText->GetFormat(m_Font.sFamily.c_str(), m_Font.fSizeDip, m_Font.eWeight) : nullptr);
}

std::wstring C_CARD_LIST::TimeLabel(std::int64_t _nEpochUs) const
{
	domain::S_TIME_ZONE_RESOLUTION Resolution = m_TimeZone.Resolve(m_sTimeZoneUtf8, _nEpochUs);
	// 원본 _time_label 은 유효하지 않은 시간대 식별자를 로컬로 되돌린다(card_delegate.py:264~266).
	if (!Resolution.bValid) { Resolution = m_TimeZone.Resolve("system", _nEpochUs); }
	if (!Resolution.bValid) { return(std::wstring{}); }
	domain::S_DATE_TIME_VIEW View;
	View.nYear = static_cast<int>(Resolution.nYear);
	View.nMonth = Resolution.nMonth;
	View.nDay = Resolution.nDay;
	View.nHour = Resolution.nHour;
	View.nMinute = Resolution.nMinute;
	View.nSecond = Resolution.nSecond;
	View.nMillisecond = Resolution.nMillisecond;
	View.nUtcOffsetSeconds = Resolution.nUtcOffsetSeconds;
	View.sTimeZoneAbbreviation = Resolution.sAbbreviation;
	View.bValid = true;
	return(wide(domain::FormatDateTime(View, m_sTimeFormatUtf8)));
}

bool C_CARD_LIST::ensure_target_()
{
	if (!m_pDevice || !m_pBrushCache || !m_pText || !this->IsWindow()) { return(false); }
	if (m_bTargetReady) { return(true); }
	if (!m_Target.Initialize(m_pDevice, this->m_hWnd)) { return(false); }
	m_Target.SetDpi(static_cast<float>(m_nDpi));
	m_bTargetReady = true;
	return(true);
}

void C_CARD_LIST::handle_device_lost_()
{
	m_pDevice->HandleDeviceLost();
	m_Target.RecreateAfterDeviceLost();
	m_pBrushCache->OnDeviceLost();
	::InvalidateRect(this->m_hWnd, nullptr, FALSE);
}

bool C_CARD_LIST::draw_text_(ID2D1DeviceContext* _pDc, IDWriteTextFormat* _pFormat,
	ID2D1Brush* _pBrush, const S_DIP_RECT& _Rect, const std::wstring& _sText)
{
	if (_sText.empty() || !_pBrush || !m_pText) { return(false); }
	d2d::C_D2D_TEXT_LAYOUT Layout = m_pText->CreateLayout(_sText.c_str(), _pFormat,
		static_cast<float>((std::max)(1, _Rect.nWidth)),
		static_cast<float>((std::max)(1, _Rect.nHeight)), 0);
	if (!Layout.IsValid()) { return(false); }
	// 표시 줄은 이미 확정된 한 줄이다. 여기서 다시 감기면 원본 drawText(감김 없음)와 갈린다.
	if (IDWriteTextLayout* pLayout = Layout.Get()) { pLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
	// 원본은 AlignLeft|AlignVCenter 로 rect 안에 그린다(card_delegate.py:97~107·:124~144).
	const float fHeight = Layout.Measure().height;
	Layout.Draw(_pDc, _pBrush, D2D1::Point2F(static_cast<float>(_Rect.nLeft),
		static_cast<float>(_Rect.nTop) + (static_cast<float>(_Rect.nHeight) - fHeight) * 0.5f));
	++m_Frame.nLayoutCount;
	return(true);
}

void C_CARD_LIST::draw_row_(ID2D1DeviceContext* _pDc, std::size_t _nRow, IDWriteTextFormat* _pFormat)
{
	const domain::S_CARD* pCard = m_pProjection->CardAt(_nRow);
	if (!pCard) { return; }

	S_CARD_LIST_ROW_FRAME Row;
	Row.nRow = _nRow;
	Row.sCardId = pCard->sId;
	Row.RowRect = this->RowRectDip(_nRow);
	Row.CardRect = Row.RowRect.Adjusted(CARD_HORIZONTAL_INSET_DIP, CARD_VERTICAL_INSET_DIP,
		-CARD_HORIZONTAL_INSET_DIP, -CARD_VERTICAL_INSET_DIP);
	Row.ContentRect = Row.CardRect.Adjusted(
		CARD_CONTENT_HORIZONTAL_MARGIN_DIP, CARD_CONTENT_VERTICAL_MARGIN_DIP,
		-CARD_CONTENT_HORIZONTAL_MARGIN_DIP, -CARD_CONTENT_VERTICAL_MARGIN_DIP);

	const std::vector<std::string>& Selected = m_pProjection->SelectedCardIds();
	Row.bSelected = std::find(Selected.begin(), Selected.end(), pCard->sId) != Selected.end();
	Row.bHovered = m_nHoverRow.has_value() && *m_nHoverRow == _nRow;
	Row.nFillColor = Row.bSelected ? m_Palette.nHighlight :
		(Row.bHovered ? m_Palette.nHoverBase : m_Palette.nBase);
	Row.nTextColor = Row.bSelected ? m_Palette.nHighlightText : m_Palette.nText;
	Row.nBorderColor = m_Palette.nBorder;
	const d2d::Color nTimeColor = Row.bSelected ? m_Palette.nHighlightText : m_Palette.nPlaceholder;

	// 둥근 사각은 D2DWrapp 에 없는 프리미티브라 노출된 ID2D1DeviceContext 로 직접 그린다(P2 선례).
	const D2D1_RECT_F Area = D2D1::RectF(
		static_cast<float>(Row.CardRect.nLeft), static_cast<float>(Row.CardRect.nTop),
		static_cast<float>(Row.CardRect.nLeft + Row.CardRect.nWidth),
		static_cast<float>(Row.CardRect.nTop + Row.CardRect.nHeight));
	_pDc->FillRoundedRectangle(
		D2D1::RoundedRect(Area, CARD_CORNER_RADIUS_DIP, CARD_CORNER_RADIUS_DIP),
		m_pBrushCache->GetBrush(Row.nFillColor));
	// 1 DIP 선은 경계 위에 중심이 놓이므로 0.5 안으로 당겨야 카드 안에 들어온다.
	_pDc->DrawRoundedRectangle(
		D2D1::RoundedRect(D2D1::RectF(Area.left + 0.5f, Area.top + 0.5f, Area.right - 0.5f, Area.bottom - 0.5f),
			CARD_CORNER_RADIUS_DIP, CARD_CORNER_RADIUS_DIP),
		m_pBrushCache->GetBrush(Row.nBorderColor));

	const int nLine = this->LineSpacingDip();
	const std::size_t nPreviewLines = m_pProjection->PreviewLineCount();
	const std::optional<domain::S_CARD_PREVIEW> Preview = m_pProjection->PreviewForCard(pCard->sId);
	const std::vector<std::wstring> Lines = ComputeDisplayLines(
		Preview ? wide(Preview->sText) : std::wstring{},
		Preview.has_value() && Preview->bTruncated,
		*m_pText, _pFormat, Row.ContentRect.nWidth, nPreviewLines);

	ID2D1SolidColorBrush* pBodyBrush = m_pBrushCache->GetBrush(Row.nTextColor);
	for (std::size_t nIndex = 0; nIndex < Lines.size(); ++nIndex)
	{
		S_CARD_TEXT_RUN Run;
		Run.Rect = S_DIP_RECT{ Row.ContentRect.nLeft,
			Row.ContentRect.nTop + static_cast<int>(nIndex) * nLine, Row.ContentRect.nWidth, nLine };
		Run.sText = Lines[nIndex];
		Run.bDrawn = this->draw_text_(_pDc, _pFormat, pBodyBrush, Run.Rect, Run.sText);
		Row.BodyLines.push_back(std::move(Run));
	}

	// 보조 시각행(원본 card_delegate.py:109~144). y 는 표시 줄 수가 아니라 설정 줄 수로 내린다.
	const int nAuxTop = Row.ContentRect.nTop + static_cast<int>(nPreviewLines) * nLine;
	const int nAuxHeight = nLine + CARD_AUXILIARY_ROW_PADDING_DIP;
	std::wstring sSuffix;
	if (pCard->nUpdatedAtUs > pCard->nCreatedAtUs) { sSuffix += L" (수정됨)"; }
	if (m_pProjection->IsCardDirty(pCard->sId)) { sSuffix += L" · 편집 중"; }
	std::wstring sTime = this->TimeLabel(pCard->nUpdatedAtUs);
	int nSuffixLeft = Row.ContentRect.nLeft;
	const int nSuffixWidth = advance_dip(*m_pText, _pFormat, sSuffix);
	if (nSuffixWidth > Row.ContentRect.nWidth)
	{
		// 접미사가 통째로 넘치면 시각을 버리고 접미사를 말줄임한다(_time_parts 의 첫 분기).
		sTime.clear();
		sSuffix = elide_right(*m_pText, _pFormat, sSuffix, Row.ContentRect.nWidth);
	}
	else
	{
		sTime = elide_right(*m_pText, _pFormat, sTime,
			(std::max)(0, Row.ContentRect.nWidth - nSuffixWidth));
		nSuffixLeft = Row.ContentRect.nLeft + advance_dip(*m_pText, _pFormat, sTime);
	}

	ID2D1SolidColorBrush* pTimeBrush = m_pBrushCache->GetBrush(nTimeColor);
	Row.TimeRun.Rect = S_DIP_RECT{ Row.ContentRect.nLeft, nAuxTop,
		(std::max)(0, nSuffixLeft - Row.ContentRect.nLeft), nAuxHeight };
	Row.TimeRun.sText = sTime;
	Row.TimeRun.bDrawn = this->draw_text_(_pDc, _pFormat, pTimeBrush, Row.TimeRun.Rect, sTime);
	if (!sSuffix.empty())
	{
		Row.SuffixRun.Rect = S_DIP_RECT{ nSuffixLeft, nAuxTop,
			advance_dip(*m_pText, _pFormat, sSuffix), nAuxHeight };
		Row.SuffixRun.sText = sSuffix;
		Row.SuffixRun.bDrawn = this->draw_text_(_pDc, _pFormat, pTimeBrush, Row.SuffixRun.Rect, sSuffix);
	}
	m_Frame.Rows.push_back(std::move(Row));
}

bool C_CARD_LIST::render_()
{
	m_Frame = S_CARD_LIST_FRAME{};
	if (!this->ensure_target_() || !m_Target.BeginDraw()) { return(false); }
	ID2D1DeviceContext* pDc = m_Target.GetDC();
	pDc->Clear(d2d::ToColorF(m_Palette.nBase));

	IDWriteTextFormat* pFormat = this->text_format_();
	const std::size_t nRows = this->RowCount();
	const int nRowHeight = this->RowHeightDip();
	if (pFormat && nRows > 0 && nRowHeight > 0)
	{
		const int nViewport = this->ViewportHeightDip();
		const std::size_t nFirst = (std::min)(nRows - 1,
			static_cast<std::size_t>(m_nScrollOffsetDip / nRowHeight));
		const std::size_t nLast = (std::min)(nRows - 1,
			static_cast<std::size_t>((std::max)(0, m_nScrollOffsetDip + nViewport - 1) / nRowHeight));
		m_Frame.nFirstVisibleRow = nFirst;
		m_Frame.nLastVisibleRow = nFirst;
		for (std::size_t nRow = nFirst; nRow <= nLast; ++nRow)
		{
			this->draw_row_(pDc, nRow, pFormat);
			m_Frame.nLastVisibleRow = nRow;
		}
	}

	m_Frame.bPresented = m_Target.EndDraw(0);
	// 로스가 계속되면 무한 무효화로 돌지 않는다 - 한 번만 복구를 걸고 다음 성공에서 푼다.
	if (m_Frame.bPresented) { m_bRecoveringDevice = false; }
	else if (!m_bRecoveringDevice) { m_bRecoveringDevice = true; this->handle_device_lost_(); }
	return(m_Frame.bPresented);
}

bool C_CARD_LIST::Render()
{
	if (this->IsWindow()) { ::ValidateRect(this->m_hWnd, nullptr); }
	return(this->render_());
}

LRESULT C_CARD_LIST::OnPaint(UINT, WPARAM, LPARAM, BOOL&)
{
	PAINTSTRUCT Paint{};
	::BeginPaint(this->m_hWnd, &Paint);
	::EndPaint(this->m_hWnd, &Paint);
	// 스왑체인 present 는 BeginPaint 영역과 무관하다(P2 선례). 렌더 서비스가 없으면 그리지 않는다.
	this->render_();
	return(0);
}

LRESULT C_CARD_LIST::OnSize(UINT, WPARAM, LPARAM _lParam, BOOL&)
{
	if (m_bTargetReady) { m_Target.Resize(LOWORD(_lParam), HIWORD(_lParam)); }
	this->ScrollToPixel(m_nScrollOffsetDip);
	this->update_scroll_bar_();
	this->Invalidate(FALSE);
	return(0);
}

LRESULT C_CARD_LIST::OnDpiChangedAfterParent(UINT, WPARAM, LPARAM, BOOL&)
{
	// 자식 창은 WM_DPICHANGED 를 받지 않는다 - AFTERPARENT 가 이 컨트롤의 갱신 지점이다.
	m_nDpi = (std::max<UINT>)(USER_DEFAULT_SCREEN_DPI, ::GetDpiForWindow(this->m_hWnd));
	if (m_bTargetReady) { m_Target.SetDpi(static_cast<float>(m_nDpi)); }
	m_nLineSpacingDip = 0;
	this->ScrollToPixel(m_nScrollOffsetDip);
	this->update_scroll_bar_();
	this->Invalidate(FALSE);
	return(0);
}

LRESULT C_CARD_LIST::OnVScroll(UINT, WPARAM _wParam, LPARAM, BOOL&)
{
	int nTarget = m_nScrollOffsetDip;
	switch (LOWORD(_wParam))
	{
	case SB_LINEUP: nTarget -= this->LineSpacingDip(); break;
	case SB_LINEDOWN: nTarget += this->LineSpacingDip(); break;
	case SB_PAGEUP: nTarget -= this->ViewportHeightDip(); break;
	case SB_PAGEDOWN: nTarget += this->ViewportHeightDip(); break;
	case SB_TOP: nTarget = 0; break;
	case SB_BOTTOM: nTarget = this->max_scroll_offset_dip_(); break;
	case SB_THUMBTRACK:
	case SB_THUMBPOSITION:
	{
		SCROLLINFO Info{};
		Info.cbSize = sizeof(Info);
		Info.fMask = SIF_TRACKPOS;
		if (::GetScrollInfo(this->m_hWnd, SB_VERT, &Info) != FALSE) { nTarget = Info.nTrackPos; }
		break;
	}
	default: return(0);
	}
	this->ScrollToPixel(nTarget);
	return(0);
}

LRESULT C_CARD_LIST::OnMouseMove(UINT, WPARAM, LPARAM _lParam, BOOL&)
{
	if (!m_bTrackingMouse)
	{
		TRACKMOUSEEVENT Track{};
		Track.cbSize = sizeof(Track);
		Track.dwFlags = TME_LEAVE;
		Track.hwndTrack = this->m_hWnd;
		m_bTrackingMouse = ::TrackMouseEvent(&Track) != FALSE;
	}
	const std::optional<std::size_t> nRow = this->row_at_dip_(this->client_dip_(GET_Y_LPARAM(_lParam)));
	if (nRow != m_nHoverRow)
	{
		this->invalidate_row_(m_nHoverRow);
		m_nHoverRow = nRow;
		this->invalidate_row_(m_nHoverRow);
	}
	return(0);
}

LRESULT C_CARD_LIST::OnMouseLeave(UINT, WPARAM, LPARAM, BOOL&)
{
	m_bTrackingMouse = false;
	const std::optional<std::size_t> nRow = m_nHoverRow;
	m_nHoverRow.reset();
	this->invalidate_row_(nRow);
	return(0);
}

LRESULT C_CARD_LIST::OnKeyDown(UINT, WPARAM _wParam, LPARAM, BOOL& _bHandled)
{
	// 관측 계약: CardListHwnd() 에 WM_KEYDOWN(VK_RETURN) 을 보내면 카드가 열린다.
	if (_wParam != VK_RETURN) { _bHandled = FALSE; return(0); }
	if (m_Activate) { m_Activate(); }
	return(0);
}

LRESULT C_CARD_LIST::OnFocusChanged(UINT, WPARAM, LPARAM, BOOL& _bHandled)
{
	if (this->IsWindow()) { this->Invalidate(FALSE); }
	_bHandled = FALSE;
	return(0);
}

LRESULT C_CARD_LIST::OnPaletteChanged(UINT, WPARAM, LPARAM, BOOL& _bHandled)
{
	this->capture_palette_();
	if (this->IsWindow()) { this->Invalidate(FALSE); }
	_bHandled = FALSE;
	return(0);
}

LRESULT C_CARD_LIST::OnDestroy(UINT, WPARAM, LPARAM, BOOL& _bHandled)
{
	m_Target.Shutdown();
	m_bTargetReady = false;
	m_bRecoveringDevice = false;
	m_bTrackingMouse = false;
	m_nHoverRow.reset();
	_bHandled = FALSE;
	return(0);
}

LRESULT C_CARD_LIST::OnListGetCount(UINT, WPARAM, LPARAM, BOOL&)
{
	return(static_cast<LRESULT>(this->RowCount()));
}

LRESULT C_CARD_LIST::OnListGetCurSel(UINT, WPARAM, LPARAM, BOOL&)
{
	const std::optional<std::size_t> nRow = this->current_row_();
	return(nRow ? static_cast<LRESULT>(*nRow) : LB_ERR);
}

LRESULT C_CARD_LIST::OnListSetCurSel(UINT, WPARAM _wParam, LPARAM, BOOL&)
{
	if (!m_pProjection) { return(LB_ERR); }
	const int nRow = static_cast<int>(_wParam);
	if (nRow == -1)
	{
		m_pProjection->SetCurrentCardId(std::nullopt);
		m_pProjection->SetSelectedCardIds({});
		if (this->IsWindow()) { this->Invalidate(FALSE); }
		return(LB_ERR);
	}
	if (nRow < 0 || static_cast<std::size_t>(nRow) >= m_pProjection->RowCount()) { return(LB_ERR); }
	const domain::S_CARD* pCard = m_pProjection->CardAt(static_cast<std::size_t>(nRow));
	if (!pCard) { return(LB_ERR); }
	m_pProjection->SelectVisibleCard(pCard->sId, domain::E_CARD_SELECTION_INTENT::Replace);
	this->EnsureVisible(static_cast<std::size_t>(nRow));
	if (this->IsWindow()) { this->Invalidate(FALSE); }
	return(nRow);
}

LRESULT C_CARD_LIST::OnListGetTopIndex(UINT, WPARAM, LPARAM, BOOL&)
{
	const int nRowHeight = this->RowHeightDip();
	if (this->RowCount() == 0 || nRowHeight <= 0) { return(0); }
	return(m_nScrollOffsetDip / nRowHeight);
}

LRESULT C_CARD_LIST::OnListSetTopIndex(UINT, WPARAM _wParam, LPARAM, BOOL&)
{
	const int nRow = static_cast<int>(_wParam);
	if (!m_pProjection || nRow < 0 ||
		static_cast<std::size_t>(nRow) >= m_pProjection->RowCount()) { return(LB_ERR); }
	this->ScrollToPixel(nRow * this->RowHeightDip());
	return(0);
}
