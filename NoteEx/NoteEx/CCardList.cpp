#include "CCardList.h"

#include <D2DWrapp/D2DBrushCache.h>
#include <D2DWrapp/D2DDevice.h>

#include "pynote/core/domain/card_list_projection.h"
#include "pynote/core/domain/card_wheel_browse.h"
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

	// Win32 한 틱(WHEEL_DELTA)과 core 의 각 단위가 같아야 "틱당 한 장" 이 성립한다.
	static_assert(WHEEL_DELTA == domain::CARD_WHEEL_VERTICAL_ANGLE_STEP,
		"WHEEL_DELTA 와 CARD_WHEEL_VERTICAL_ANGLE_STEP 이 어긋나면 틱당 한 장 계약이 깨진다");

	// 말줄임표는 U+2026 한 글자다(원본 f"{displayed[-1]}…", card_delegate.py:216).
	constexpr wchar_t ELLIPSIS[] = L"…";
	// 미리보기 레이아웃의 높이 한도. D2DWrapp 기본값과 같은 "사실상 무제한"이다.
	constexpr float PREVIEW_LAYOUT_MAX_HEIGHT_DIP = 100000.0f;

	// Qt manhattanLength() 등가. 정수 DIP 로만 잰다.
	int manhattan_dip(POINT _First, POINT _Second) noexcept
	{
		const int nX = _First.x - _Second.x;
		const int nY = _First.y - _Second.y;
		return((nX < 0 ? -nX : nX) + (nY < 0 ? -nY : nY));
	}

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

E_CARD_SELECTION_COMMAND ResolveSelectionCommand(const S_CARD_SELECTION_INPUT& _Input) noexcept
{
	// Qt 6.9 QAbstractItemViewPrivate::selectionCommand / extendedSelectionCommand 의 규칙이다.
	// 단언은 오라클 표(spec 부록 A)이고 이 함수는 그 표를 재생산한다.
	const bool bNavigationKey =
		_Input.nKey == VK_UP || _Input.nKey == VK_DOWN || _Input.nKey == VK_LEFT ||
		_Input.nKey == VK_RIGHT || _Input.nKey == VK_HOME || _Input.nKey == VK_END ||
		_Input.nKey == VK_PRIOR || _Input.nKey == VK_NEXT;

	if (_Input.eMode == domain::E_CARD_SELECTION_MODE::Single)
	{
		if (_Input.ePhase == E_CARD_INPUT_PHASE::Release) { return(E_CARD_SELECTION_COMMAND::NoUpdate); }
		// Ctrl+클릭·Ctrl+Space 로 선택된 카드를 누르면 선택만 풀린다(현재는 그대로 - M3/K3 single).
		if (_Input.bCtrl && _Input.bRowSelected && _Input.ePhase != E_CARD_INPUT_PHASE::Move)
		{
			return(E_CARD_SELECTION_COMMAND::Deselect);
		}
		if (_Input.ePhase == E_CARD_INPUT_PHASE::Move && _Input.bDragSelecting)
		{
			return(E_CARD_SELECTION_COMMAND::BandReplace);
		}
		return(E_CARD_SELECTION_COMMAND::ClearAndSelect);
	}

	switch (_Input.ePhase)
	{
	case E_CARD_INPUT_PHASE::Press:
		// 이미 선택된 행의 평범한 press 는 release 로 미룬다 - 드래그가 시작될 수 있다(M10).
		if (!_Input.bShift && !_Input.bCtrl && _Input.bRowSelected) { return(E_CARD_SELECTION_COMMAND::NoUpdate); }
		// Qt 는 빈 영역 press 에 Clear 를 돌려주지만 press 시점에 적용하지 않는다 - 지우는 것은 release 다.
		if (!_Input.bRowValid) { return(E_CARD_SELECTION_COMMAND::NoUpdate); }
		if (_Input.bCtrl && _Input.bPressedAlreadySelected) { return(E_CARD_SELECTION_COMMAND::NoUpdate); }
		break;
	case E_CARD_INPUT_PHASE::Release:
		if (((_Input.bSamePressedRow && _Input.bRowSelected) || !_Input.bRowValid) &&
			!_Input.bDragSelecting && !_Input.bShift && !_Input.bCtrl)
		{
			return(E_CARD_SELECTION_COMMAND::ClearAndSelect);
		}
		if (_Input.bSamePressedRow && _Input.bCtrl && _Input.bPressedAlreadySelected)
		{
			return(E_CARD_SELECTION_COMMAND::Toggle);
		}
		return(E_CARD_SELECTION_COMMAND::NoUpdate);
	case E_CARD_INPUT_PHASE::Move:
		if (_Input.bDragSelecting)
		{
			if (_Input.bCtrl) { return(E_CARD_SELECTION_COMMAND::BandToggle); }
			if (_Input.bShift) { return(E_CARD_SELECTION_COMMAND::SelectCurrent); }
			return(E_CARD_SELECTION_COMMAND::BandReplace);
		}
		break;
	case E_CARD_INPUT_PHASE::Key:
		// Ctrl+방향키는 현재만 옮기고 선택은 그대로다(K3/K7 extended).
		if (bNavigationKey && _Input.bCtrl) { return(E_CARD_SELECTION_COMMAND::NoUpdate); }
		if (_Input.nKey == VK_SPACE)
		{
			return(_Input.bCtrl ? E_CARD_SELECTION_COMMAND::Toggle : E_CARD_SELECTION_COMMAND::Select);
		}
		break;
	}
	if (_Input.bShift) { return(E_CARD_SELECTION_COMMAND::SelectCurrent); }
	if (_Input.bCtrl) { return(E_CARD_SELECTION_COMMAND::Toggle); }
	return(E_CARD_SELECTION_COMMAND::ClearAndSelect);
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
	// 새 프로젝션은 새 상태 기계다 - 옛 타이머부터 걷고 만든다(떠난 세대의 WM_TIMER 방지).
	if (this->IsWindow()) { ::KillTimer(this->m_hWnd, CARD_LIST_WHEEL_TIMER_ID); }
	m_nWheelTimerGeneration = 0;
	m_pWheel.reset();
	m_pProjection = &_Projection;
	// 새 프로젝션은 새 목록이다 - 앵커·Shift 기준·"현재를 본 적 있는가" 를 모두 되돌린다.
	m_bCurrentObserved = false;
	m_sAnchorCardId.reset();
	m_ShiftBase.clear();
	// 시계는 단조 ms, 스케줄러는 SetTimer/KillTimer 다. 둘 다 비어 있지 않으므로 core
	// 생성자의 invalid_argument 는 발생할 수 없다(그래서 이 함수는 noexcept 로 남는다).
	m_pWheel = std::make_unique<domain::C_CARD_WHEEL_BROWSE>(_Projection,
		[]() { return(static_cast<std::int64_t>(::GetTickCount64())); },
		[this](const domain::S_CARD_WHEEL_TIMER_COMMAND& _Command)
			{ this->schedule_wheel_timer_(_Command); });
}

void C_CARD_LIST::SetActivateHandler(ActivateHandler _Handler)
{
	m_Activate = std::move(_Handler);
}

void C_CARD_LIST::SetOpenCardHandler(OpenCardHandler _Handler)
{
	m_OpenCard = std::move(_Handler);
}

void C_CARD_LIST::SetEmptyAreaClickHandler(EmptyAreaClickHandler _Handler)
{
	m_EmptyAreaClick = std::move(_Handler);
}

void C_CARD_LIST::SetDeleteHandler(DeleteHandler _Handler)
{
	m_Delete = std::move(_Handler);
}

void C_CARD_LIST::SetBrowseCardHandler(BrowseCardHandler _Handler)
{
	m_BrowseCard = std::move(_Handler);
}

void C_CARD_LIST::SetEditorCardProvider(EditorCardProvider _Provider)
{
	m_EditorCard = std::move(_Provider);
}

std::optional<std::string> C_CARD_LIST::PendingBrowseCardId() const
{
	if (!m_pWheel) { return(std::nullopt); }
	return(m_pWheel->PendingCardId());
}

int C_CARD_LIST::WheelAngleRemainder() const
{
	return(m_pWheel ? m_pWheel->AngleRemainder() : 0);
}

void C_CARD_LIST::CancelPendingBrowse()
{
	// core 가 대기 카드·잔여 각·진행 중 열기를 지우고 세대를 올린 뒤 Cancel 명령을 낸다.
	if (m_pWheel) { m_pWheel->Cancel(); }
}

void C_CARD_LIST::SetCurrentRow(std::size_t _nRow)
{
	if (!m_pProjection || _nRow >= m_pProjection->RowCount()) { return; }
	const domain::S_CARD* pCard = m_pProjection->CardAt(_nRow);
	if (!pCard) { return; }
	m_pProjection->SelectVisibleCard(pCard->sId, domain::E_CARD_SELECTION_INTENT::Replace);
	// Qt setCurrentIndex -> ClearAndSelect 경로와 같게 앵커·Shift 기준도 이 행으로 간다.
	this->set_anchor_(_nRow);
	m_ShiftBase = { pCard->sId };
	this->observe_current_();
	this->EnsureVisible(_nRow);
	if (this->IsWindow()) { this->Invalidate(FALSE); }
}

void C_CARD_LIST::RevealRow(std::size_t _nRow)
{
	// 원본 reveal_card 순서: 유효성 확인 -> 대기 취소 -> setCurrentIndex -> scrollTo.
	// 범위 밖이면 취소도 하지 않는다(원본이 취소 앞에서 돌아간다).
	if (!m_pProjection || _nRow >= m_pProjection->RowCount()) { return; }
	this->CancelPendingBrowse();
	this->SetCurrentRow(_nRow);
}

void C_CARD_LIST::schedule_wheel_timer_(const domain::S_CARD_WHEEL_TIMER_COMMAND& _Command)
{
	if (_Command.eOperation == domain::E_CARD_WHEEL_TIMER_OPERATION::Cancel)
	{
		if (this->IsWindow()) { ::KillTimer(this->m_hWnd, CARD_LIST_WHEEL_TIMER_ID); }
		m_nWheelTimerGeneration = 0;
		return;
	}
	// 창이 없으면 타이머가 떨어질 곳이 없다 - Arm 은 통째로 무동작이다.
	if (!this->IsWindow() || !_Command.nDeadlineMs) { return; }
	m_nWheelTimerGeneration = _Command.nGeneration;
	const std::int64_t nElapse =
		*_Command.nDeadlineMs - static_cast<std::int64_t>(::GetTickCount64());
	// 같은 id 는 이전 타이머를 대체한다 - Arm 마다 정숙 구간이 처음부터 다시 흐른다.
	::SetTimer(this->m_hWnd, CARD_LIST_WHEEL_TIMER_ID,
		static_cast<UINT>((std::max<std::int64_t>)(USER_TIMER_MINIMUM, nElapse)), nullptr);
}

void C_CARD_LIST::PostDeferred(std::function<void()> _Callable)
{
	if (!_Callable || !this->IsWindow()) { return; }
	m_Deferred.push_back(std::move(_Callable));
	::PostMessageW(this->m_hWnd, CARD_LIST_DEFERRED_MESSAGE, 0, 0);
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
	this->observe_current_();
	// 앵커는 카드 id 로 들고 있다 - 정렬·필터·삭제로 행을 잃으면 Shift 기준째 버린다.
	if (m_sAnchorCardId && (!m_pProjection || !m_pProjection->RowForCard(*m_sAnchorCardId)))
	{
		m_sAnchorCardId.reset();
		m_ShiftBase.clear();
	}
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

POINT C_CARD_LIST::point_from_lparam_(LPARAM _lParam) const noexcept
{
	// 캡처된 릴리스는 음수 클라이언트 좌표를 실어 온다 - LOWORD 로 읽으면 -5 가 65531 이 된다.
	return(POINT{ this->client_dip_(GET_X_LPARAM(_lParam)), this->client_dip_(GET_Y_LPARAM(_lParam)) });
}

bool C_CARD_LIST::inside_viewport_(POINT _Point) const
{
	// 세로 스크롤바는 클라이언트 밖이라 클라이언트 사각이 곧 뷰포트다(Qt viewport().rect()).
	return(_Point.x >= 0 && _Point.y >= 0 &&
		_Point.x < this->ViewportWidthDip() && _Point.y < this->ViewportHeightDip());
}

std::optional<std::size_t> C_CARD_LIST::row_at_point_(POINT _Point) const
{
	if (!this->inside_viewport_(_Point)) { return(std::nullopt); }
	return(this->row_at_dip_(_Point.y));
}

std::optional<std::size_t> C_CARD_LIST::HitTestRow(POINT _ClientPx) const
{
	return(this->row_at_point_(
		POINT{ this->client_dip_(_ClientPx.x), this->client_dip_(_ClientPx.y) }));
}

bool C_CARD_LIST::IsRowSelected(std::size_t _nRow) const
{
	if (!m_pProjection) { return(false); }
	const domain::S_CARD* pCard = m_pProjection->CardAt(_nRow);
	if (!pCard) { return(false); }
	const std::vector<std::string>& Selected = m_pProjection->SelectedCardIds();
	return(std::find(Selected.begin(), Selected.end(), pCard->sId) != Selected.end());
}

std::optional<std::size_t> C_CARD_LIST::AnchorRow() const
{
	if (!m_pProjection || !m_sAnchorCardId) { return(std::nullopt); }
	return(m_pProjection->RowForCard(*m_sAnchorCardId));
}

void C_CARD_LIST::set_anchor_(std::optional<std::size_t> _nRow)
{
	m_sAnchorCardId.reset();
	if (!_nRow || !m_pProjection) { return; }
	const domain::S_CARD* pCard = m_pProjection->CardAt(*_nRow);
	if (pCard) { m_sAnchorCardId = pCard->sId; }
}

void C_CARD_LIST::observe_current_()
{
	if (m_pProjection && m_pProjection->CurrentCardId()) { m_bCurrentObserved = true; }
}

void C_CARD_LIST::select_row_additive_(std::size_t _nRow)
{
	const domain::S_CARD* pCard = m_pProjection->CardAt(_nRow);
	if (!pCard) { return; }
	m_pProjection->SelectVisibleCard(pCard->sId, domain::E_CARD_SELECTION_INTENT::Additive);
	this->set_anchor_(_nRow);
	m_ShiftBase = m_pProjection->SelectedCardIds();
}

void C_CARD_LIST::deselect_row_(std::size_t _nRow)
{
	const domain::S_CARD* pCard = m_pProjection->CardAt(_nRow);
	if (!pCard) { return; }
	const std::string sCardId = pCard->sId;
	std::vector<std::string> Remaining = m_pProjection->SelectedCardIds();
	Remaining.erase(std::remove(Remaining.begin(), Remaining.end(), sCardId), Remaining.end());
	m_pProjection->SetSelectedCardIds(std::move(Remaining));
	// 선택만 풀고 현재는 대상 행에 남긴다(Qt Deselect + 현재 유지 - M3).
	m_pProjection->SetCurrentCardId(sCardId);
	this->set_anchor_(_nRow);
	m_ShiftBase = m_pProjection->SelectedCardIds();
}

std::vector<std::string> C_CARD_LIST::band_rows_(int _nCursorYdip) const
{
	std::vector<std::string> Rows;
	if (!m_pProjection) { return(Rows); }
	const std::size_t nCount = this->RowCount();
	const int nRowHeight = this->RowHeightDip();
	if (nCount == 0 || nRowHeight <= 0) { return(Rows); }
	if (m_pProjection->SelectionMode() == domain::E_CARD_SELECTION_MODE::Single)
	{
		// 단일 선택의 Qt 는 1x1 사각을 쓴다 - 띠가 아니라 커서 아래 한 행이다(M13/M15 single).
		const std::optional<std::size_t> nRow = this->row_at_dip_(_nCursorYdip);
		const domain::S_CARD* pCard = nRow ? m_pProjection->CardAt(*nRow) : nullptr;
		if (pCard) { Rows.push_back(pCard->sId); }
		return(Rows);
	}
	// 닫힌 정수 구간의 교차다. press 와 커서는 뷰포트 좌표라 내용 좌표로 올려서 잰다.
	const int nPress = m_PressPoint.y + m_nScrollOffsetDip;
	const int nCursor = _nCursorYdip + m_nScrollOffsetDip;
	const int nLow = (std::min)(nPress, nCursor);
	const int nHigh = (std::max)(nPress, nCursor);
	for (std::size_t nRow = 0; nRow < nCount; ++nRow)
	{
		const int nTop = static_cast<int>(nRow) * nRowHeight;
		const int nBottom = nTop + nRowHeight - 1;
		if (nBottom < nLow || nTop > nHigh) { continue; }
		const domain::S_CARD* pCard = m_pProjection->CardAt(nRow);
		if (pCard) { Rows.push_back(pCard->sId); }
	}
	return(Rows);
}

std::size_t C_CARD_LIST::page_row_(std::size_t _nCurrentRow, bool _bDown) const
{
	// CEILING: 스크롤되는 목록의 PageUp/Down 행 선택은 Qt closestIndex 미이식, 근사다
	// (전 행이 한 화면에 보이는 오라클 K1/K7 조건에서는 정확히 마지막/첫 행이 된다).
	const std::size_t nCount = this->RowCount();
	const int nRowHeight = this->RowHeightDip();
	if (nCount == 0 || nRowHeight <= 0) { return(0); }
	const long long nTop = static_cast<long long>(_nCurrentRow) * nRowHeight;
	const long long nStep = (std::max)(0, this->ViewportHeightDip() - 1);
	const long long nPoint = nTop + (_bDown ? nStep : -nStep);
	if (nPoint < 0) { return(0); }
	if (nPoint >= static_cast<long long>(nCount) * nRowHeight) { return(nCount - 1); }
	return(static_cast<std::size_t>(nPoint / nRowHeight));
}

void C_CARD_LIST::apply_selection_command_(E_CARD_SELECTION_COMMAND _eCommand,
	E_CARD_INPUT_PHASE _ePhase, std::optional<std::size_t> _nTargetRow,
	std::optional<std::size_t> _nOldCurrentRow, const std::vector<std::string>& _Band)
{
	if (!m_pProjection || _eCommand == E_CARD_SELECTION_COMMAND::NoUpdate) { return; }
	// Qt mousePressEvent 는 유효하지 않은 행의 press 명령을 적용하지 않는다(M12/M16 single).
	if (_ePhase == E_CARD_INPUT_PHASE::Press && !_nTargetRow) { return; }
	const domain::S_CARD* pTarget = _nTargetRow ? m_pProjection->CardAt(*_nTargetRow) : nullptr;
	const std::string sTargetId = pTarget ? pTarget->sId : std::string{};
	switch (_eCommand)
	{
	case E_CARD_SELECTION_COMMAND::ClearAndSelect:
		if (!pTarget)
		{
			// release/키 단계의 유효하지 않은 행은 선택을 비운다(현재는 그대로 - M16 extended).
			m_pProjection->SetSelectedCardIds({});
			break;
		}
		m_pProjection->SelectVisibleCard(sTargetId, domain::E_CARD_SELECTION_INTENT::Replace);
		this->set_anchor_(_nTargetRow);
		m_ShiftBase = { sTargetId };
		break;
	case E_CARD_SELECTION_COMMAND::Select:
		if (pTarget) { this->select_row_additive_(*_nTargetRow); }
		break;
	case E_CARD_SELECTION_COMMAND::Deselect:
		if (pTarget) { this->deselect_row_(*_nTargetRow); }
		break;
	case E_CARD_SELECTION_COMMAND::Toggle:
		if (!pTarget) { break; }
		if (this->IsRowSelected(*_nTargetRow)) { this->deselect_row_(*_nTargetRow); }
		else { this->select_row_additive_(*_nTargetRow); }
		break;
	case E_CARD_SELECTION_COMMAND::SelectCurrent:
	{
		if (!pTarget) { break; }
		std::optional<std::size_t> nAnchor = this->AnchorRow();
		if (!nAnchor)
		{
			// Qt currentSelectionStartIndex = currentIndex() 폴백이다 - 이 입력이 현재를
			// 옮기기 전의 행이 앵커가 되고 그때의 선택이 Shift 기준이 된다(M8 extended).
			nAnchor = _nOldCurrentRow ? _nOldCurrentRow : _nTargetRow;
			this->set_anchor_(nAnchor);
			m_ShiftBase = m_pProjection->SelectedCardIds();
		}
		const std::size_t nFirst = (std::min)(*nAnchor, *_nTargetRow);
		const std::size_t nLast = (std::max)(*nAnchor, *_nTargetRow);
		std::vector<std::string> Ids;
		for (std::size_t nRow = 0; nRow < this->RowCount(); ++nRow)
		{
			const domain::S_CARD* pCard = m_pProjection->CardAt(nRow);
			if (!pCard) { continue; }
			const bool bInRange = nRow >= nFirst && nRow <= nLast;
			const bool bInBase =
				std::find(m_ShiftBase.begin(), m_ShiftBase.end(), pCard->sId) != m_ShiftBase.end();
			if (bInRange || bInBase) { Ids.push_back(pCard->sId); }
		}
		m_pProjection->SetSelectedCardIds(std::move(Ids));
		m_pProjection->SetCurrentCardId(sTargetId);
		// 앵커는 그대로다 - 이어지는 Shift 클릭도 같은 기준에서 뻗는다(M5 extended).
		break;
	}
	case E_CARD_SELECTION_COMMAND::BandReplace:
		// core 의 SetSelectedCardIds 는 단일 선택에서 현재 카드로 접는다 - 현재를 먼저 옮긴다.
		if (pTarget) { m_pProjection->SetCurrentCardId(sTargetId); }
		m_pProjection->SetSelectedCardIds(_Band);
		m_ShiftBase = m_pProjection->SelectedCardIds();
		break;
	case E_CARD_SELECTION_COMMAND::BandToggle:
	{
		if (pTarget) { m_pProjection->SetCurrentCardId(sTargetId); }
		std::vector<std::string> Ids = m_pProjection->SelectedCardIds();
		for (const std::string& sId : _Band)
		{
			const auto It = std::find(Ids.begin(), Ids.end(), sId);
			if (m_bCtrlDragSelect) { if (It == Ids.end()) { Ids.push_back(sId); } }
			else if (It != Ids.end()) { Ids.erase(It); }
		}
		m_pProjection->SetSelectedCardIds(std::move(Ids));
		break;
	}
	default:
		break;
	}
	this->observe_current_();
	// 행 단위 무효화는 S5 의 몫이다 - 여기서는 정확성을 먼저 둔다.
	if (this->IsWindow()) { this->Invalidate(FALSE); }
}

void C_CARD_LIST::reset_press_state_()
{
	m_bPressActive = false;
	m_PressedRow.reset();
	m_bPressOnEmpty = false;
	m_bPressedAlreadySelected = false;
	m_bDragConsumedPress = false;
	m_bEmptyPress = false;
	m_bEmptyPressMoved = false;
	m_bNoSelectionOnMousePress = false;
	m_eViewState = E_CARD_LIST_VIEW_STATE::NoState;
	m_DragSnapshot.reset();
}

void C_CARD_LIST::handle_navigation_key_(int _nKey)
{
	if (!m_pProjection) { return; }
	const std::size_t nCount = this->RowCount();
	if (nCount == 0) { return; }
	const std::optional<std::size_t> nOld = this->current_row_();
	std::size_t nNew = 0;
	if (nOld)
	{
		switch (_nKey)
		{
		case VK_DOWN: nNew = (std::min)(*nOld + 1, nCount - 1); break;
		case VK_UP: nNew = *nOld > 0 ? *nOld - 1 : 0; break;
		case VK_HOME: nNew = 0; break;
		case VK_END: nNew = nCount - 1; break;
		case VK_NEXT: nNew = this->page_row_(*nOld, true); break;
		default: nNew = this->page_row_(*nOld, false); break;
		}
		// Qt 는 newCurrent != oldCurrent 일 때만 움직인다(K1b 의 행 0 Up).
		if (nNew == *nOld) { return; }
	}
	S_CARD_SELECTION_INPUT Input{};
	Input.eMode = m_pProjection->SelectionMode();
	Input.ePhase = E_CARD_INPUT_PHASE::Key;
	Input.bRowValid = true;
	Input.bRowSelected = this->IsRowSelected(nNew);
	Input.bCtrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
	Input.bShift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
	Input.nKey = _nKey;
	const E_CARD_SELECTION_COMMAND eCommand = ResolveSelectionCommand(Input);
	if (eCommand == E_CARD_SELECTION_COMMAND::NoUpdate)
	{
		// Qt 는 Current 플래그가 없는 분기에서 NoUpdate 여도 앵커를 새 현재로 옮긴다.
		const domain::S_CARD* pCard = m_pProjection->CardAt(nNew);
		if (pCard) { m_pProjection->SetCurrentCardId(pCard->sId); }
		this->set_anchor_(nNew);
		m_ShiftBase = m_pProjection->SelectedCardIds();
		if (this->IsWindow()) { this->Invalidate(FALSE); }
	}
	else { this->apply_selection_command_(eCommand, E_CARD_INPUT_PHASE::Key, nNew, nOld, {}); }
	this->EnsureVisible(nNew);
	this->observe_current_();
}

void C_CARD_LIST::handle_space_key_()
{
	if (!m_pProjection) { return; }
	const std::optional<std::size_t> nCurrent = this->current_row_();
	if (!nCurrent) { return; }
	S_CARD_SELECTION_INPUT Input{};
	Input.eMode = m_pProjection->SelectionMode();
	Input.ePhase = E_CARD_INPUT_PHASE::Key;
	Input.bRowValid = true;
	Input.bRowSelected = this->IsRowSelected(*nCurrent);
	Input.bCtrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
	Input.bShift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
	Input.nKey = VK_SPACE;
	this->apply_selection_command_(
		ResolveSelectionCommand(Input), E_CARD_INPUT_PHASE::Key, nCurrent, nCurrent, {});
}

bool C_CARD_LIST::prefix_match_(std::size_t _nRow, const std::wstring& _sPrefix) const
{
	if (_sPrefix.empty() || !m_pProjection) { return(false); }
	const domain::S_CARD* pCard = m_pProjection->CardAt(_nRow);
	if (!pCard) { return(false); }
	const std::optional<std::string_view> sBody = m_pProjection->FullBodyForCard(pCard->sId);
	if (!sBody) { return(false); }
	const std::wstring sWide = wide(std::string(*sBody));
	if (sWide.size() < _sPrefix.size()) { return(false); }
	// CEILING: Qt 는 유니코드 대소문자 접기를 쓴다 - 라틴 밖 특수 케이스에서 서수 비교와 갈릴 수 있다.
	return(::CompareStringOrdinal(sWide.c_str(), static_cast<int>(_sPrefix.size()),
		_sPrefix.c_str(), static_cast<int>(_sPrefix.size()), TRUE) == CSTR_EQUAL);
}

void C_CARD_LIST::keyboard_search_(wchar_t _Char)
{
	// QAbstractItemView::keyboardSearch 이식. 표시 문자열은 카드 본문 전체다(card_model.py:103~105).
	if (!m_pProjection) { return; }
	const std::size_t nCount = this->RowCount();
	if (nCount == 0) { return; }
	const std::optional<std::size_t> nCurrent = this->current_row_();
	const std::uint64_t nNow = ::GetTickCount64();
	bool bSkipRow = false;
	if (m_sSearchInput.empty() || nNow - m_nSearchTickMs > CARD_KEYBOARD_SEARCH_INTERVAL_MS)
	{
		m_sSearchInput.assign(1, _Char);
		bSkipRow = nCurrent.has_value();
	}
	else { m_sSearchInput.push_back(_Char); }
	m_nSearchTickMs = nNow;

	if (m_sSearchInput.size() > 1)
	{
		// 같은 글자만 이어지는 입력은 시작 행만 한 칸 민다 - 찾는 문자열은 줄이지 않고 누적된
		// 접두사 그대로다. 실측(본문 alpha/avocado/bravo/charlie/delta, 행 0 에서 'a' 연타):
		// 첫 'a' 는 avocado(행 1), 400ms 안의 둘째·셋째 'a' 는 "aa"/"aaa" 로 찾아 맞는 행이
		// 없으므로 행 1 에 그대로 머문다.
		const bool bSameKey =
			static_cast<std::size_t>(std::count(m_sSearchInput.begin(), m_sSearchInput.end(),
				m_sSearchInput.back())) == m_sSearchInput.size();
		if (bSameKey) { bSkipRow = true; }
	}
	std::size_t nStart = nCurrent ? *nCurrent : 0;
	if (bSkipRow) { nStart = nStart + 1 < nCount ? nStart + 1 : 0; }
	for (std::size_t nOffset = 0; nOffset < nCount; ++nOffset)
	{
		const std::size_t nRow = (nStart + nOffset) % nCount;
		if (!this->prefix_match_(nRow, m_sSearchInput)) { continue; }
		this->apply_selection_command_(
			E_CARD_SELECTION_COMMAND::ClearAndSelect, E_CARD_INPUT_PHASE::Key, nRow, nCurrent, {});
		this->EnsureVisible(nRow);
		break;
	}
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
	this->observe_current_();
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

LRESULT C_CARD_LIST::OnMouseMove(UINT, WPARAM _wParam, LPARAM _lParam, BOOL&)
{
	if (!m_bTrackingMouse)
	{
		TRACKMOUSEEVENT Track{};
		Track.cbSize = sizeof(Track);
		Track.dwFlags = TME_LEAVE;
		Track.hwndTrack = this->m_hWnd;
		m_bTrackingMouse = ::TrackMouseEvent(&Track) != FALSE;
	}
	const POINT Point = this->point_from_lparam_(_lParam);
	const std::optional<std::size_t> nRow = this->row_at_dip_(Point.y);
	if (nRow != m_nHoverRow)
	{
		this->invalidate_row_(m_nHoverRow);
		m_nHoverRow = nRow;
		this->invalidate_row_(m_nHoverRow);
	}
	if ((_wParam & MK_LBUTTON) == 0 || !m_bPressActive || m_bDragConsumedPress) { return(0); }

	const int nManhattan = manhattan_dip(Point, m_PressPoint);
	if (m_bPressOnEmpty)
	{
		// 원본 mouseMoveEvent 의 두 갈래다: 임계 도달과 고무줄 진입이 각각 clean click 을 깬다.
		if (m_bEmptyPress && nManhattan >= CARD_DRAG_DISTANCE_DIP) { m_bEmptyPressMoved = true; }
		const std::optional<std::size_t> nTarget = this->row_at_point_(Point);
		// Qt selectionAllowed: 커서가 유효한 행 위에 있을 때만 고무줄이 선다(M14 는 NoState 로 남는다).
		if (!nTarget || !m_pProjection) { return(0); }
		m_eViewState = E_CARD_LIST_VIEW_STATE::DragSelecting;
		if (m_bEmptyPress) { m_bEmptyPressMoved = true; }
		S_CARD_SELECTION_INPUT Input{};
		Input.eMode = m_pProjection->SelectionMode();
		Input.ePhase = E_CARD_INPUT_PHASE::Move;
		Input.bRowValid = true;
		Input.bRowSelected = this->IsRowSelected(*nTarget);
		Input.bPressedAlreadySelected = m_bPressedAlreadySelected;
		Input.bSamePressedRow = nTarget == m_PressedRow;
		Input.bCtrl = (_wParam & MK_CONTROL) != 0;
		Input.bShift = (_wParam & MK_SHIFT) != 0;
		Input.bDragSelecting = true;
		this->apply_selection_command_(ResolveSelectionCommand(Input), E_CARD_INPUT_PHASE::Move,
			nTarget, this->current_row_(), this->band_rows_(Point.y));
		return(0);
	}
	if (m_PressedRow)
	{
		// Qt 는 버튼을 쥔 첫 이동에서 DraggingState 로 들어가고(임계와 무관 - M11),
		// 임계를 "넘어선" 이동에서 pressedIndex 를 비우고 startDrag 뒤 NoState 로 돌아온다.
		if (m_eViewState == E_CARD_LIST_VIEW_STATE::NoState)
		{
			m_eViewState = E_CARD_LIST_VIEW_STATE::Dragging;
		}
		if (nManhattan > CARD_DRAG_DISTANCE_DIP)
		{
			// pressedIndex 를 비우지 않으면 이어지는 release 명령이 다중 선택을 한 장으로 접는다.
			m_bDragConsumedPress = true;
			m_PressedRow.reset();
			m_bPressedAlreadySelected = false;
			m_eViewState = E_CARD_LIST_VIEW_STATE::NoState;
		}
	}
	return(0);
}

LRESULT C_CARD_LIST::OnLButtonDown(UINT, WPARAM _wParam, LPARAM _lParam, BOOL&)
{
	// 원본 mousePressEvent(:248)는 어떤 버튼이든 기록 앞에서 먼저 취소한다.
	this->CancelPendingBrowse();
	// WM_LBUTTONDBLCLK 도 이 핸들러로 온다 - Qt 는 mouseDoubleClickEvent 를 재정의하지 않으므로
	// 더블클릭의 두 번째 press 도 평범한 press 이고 열기는 release 에서만 난다(오라클 N5).
	this->observe_current_();
	m_bFocusByMouse = true;
	::SetFocus(this->m_hWnd);
	m_bFocusByMouse = false;
	::SetCapture(this->m_hWnd);
	if (!m_pProjection) { return(0); }

	const POINT Point = this->point_from_lparam_(_lParam);
	const std::optional<std::size_t> nRow = this->row_at_point_(Point);
	const bool bCtrl = (_wParam & MK_CONTROL) != 0;
	const bool bShift = (_wParam & MK_SHIFT) != 0;
	// 원본의 "수식키 없음" 은 Qt NoModifier 라 Alt 도 수식키로 센다(MK_* 에는 Alt 비트가 없다).
	const bool bNoModifier = !bCtrl && !bShift && (::GetKeyState(VK_MENU) & 0x8000) == 0;
	const bool bRowSelected = nRow && this->IsRowSelected(*nRow);
	const std::optional<std::size_t> nOldCurrent = this->current_row_();

	m_bPressActive = true;
	m_PressPoint = Point;
	m_PressedRow = nRow;
	m_bPressOnEmpty = !nRow.has_value();
	m_bPressedAlreadySelected = bRowSelected;
	m_bCtrlDragSelect = !bRowSelected;
	m_bDragConsumedPress = false;
	m_bEmptyPress = !nRow && this->inside_viewport_(Point) && bNoModifier;
	m_bEmptyPressMoved = false;
	m_DragSnapshot.reset();
	const domain::S_CARD* pCard = nRow ? m_pProjection->CardAt(*nRow) : nullptr;
	if (pCard)
	{
		// S4 가 소비할 CAS 스냅샷이다 - S2 는 기록만 한다.
		m_DragSnapshot = S_CARD_DRAG_SNAPSHOT{ pCard->sId, pCard->sCurrentRevisionId, Point };
		// Qt setCurrentIndex(index, NoUpdate): press 는 현재만 옮기고 자동 스크롤은 끈다.
		m_pProjection->SetCurrentCardId(pCard->sId);
	}

	S_CARD_SELECTION_INPUT Input{};
	Input.eMode = m_pProjection->SelectionMode();
	Input.ePhase = E_CARD_INPUT_PHASE::Press;
	Input.bRowValid = nRow.has_value();
	Input.bRowSelected = bRowSelected;
	Input.bPressedAlreadySelected = bRowSelected;
	Input.bSamePressedRow = true;
	Input.bCtrl = bCtrl;
	Input.bShift = bShift;
	Input.bDragSelecting = m_eViewState == E_CARD_LIST_VIEW_STATE::DragSelecting;
	const E_CARD_SELECTION_COMMAND eCommand = ResolveSelectionCommand(Input);
	// Qt noSelectionOnMousePress - release 명령을 적용할지 가르는 문이다(spec §3.1.9 1).
	m_bNoSelectionOnMousePress = eCommand == E_CARD_SELECTION_COMMAND::NoUpdate || !nRow;
	this->apply_selection_command_(eCommand, E_CARD_INPUT_PHASE::Press, nRow, nOldCurrent, {});
	this->observe_current_();
	if (this->IsWindow()) { this->Invalidate(FALSE); }
	return(0);
}

LRESULT C_CARD_LIST::OnLButtonUp(UINT, WPARAM _wParam, LPARAM _lParam, BOOL&)
{
	this->observe_current_();
	const POINT Point = this->point_from_lparam_(_lParam);
	const std::optional<std::size_t> nRow = this->row_at_point_(Point);
	const bool bCtrl = (_wParam & MK_CONTROL) != 0;
	const bool bShift = (_wParam & MK_SHIFT) != 0;
	const bool bNoModifierNow = !bCtrl && !bShift && (::GetKeyState(VK_MENU) & 0x8000) == 0;

	// 1. release 선택 명령은 press 가 아무 선택도 적용하지 않았을 때만 돈다. setState(NoState)
	//    는 Qt 도 이 뒤라 고무줄 상태가 그대로 보인다(M13/M15 의 선택 유지).
	if (m_pProjection && m_bPressActive && m_bNoSelectionOnMousePress)
	{
		S_CARD_SELECTION_INPUT Input{};
		Input.eMode = m_pProjection->SelectionMode();
		Input.ePhase = E_CARD_INPUT_PHASE::Release;
		Input.bRowValid = nRow.has_value();
		Input.bRowSelected = nRow && this->IsRowSelected(*nRow);
		Input.bPressedAlreadySelected = m_bPressedAlreadySelected;
		Input.bSamePressedRow = nRow == m_PressedRow;
		Input.bCtrl = bCtrl;
		Input.bShift = bShift;
		Input.bDragSelecting = m_eViewState == E_CARD_LIST_VIEW_STATE::DragSelecting;
		this->apply_selection_command_(ResolveSelectionCommand(Input), E_CARD_INPUT_PHASE::Release,
			nRow, this->current_row_(), {});
	}
	// 2. 빈 영역 클릭 판정(원본 mouseReleaseEvent 의 술어 그대로).
	const bool bEmptyAreaClicked = this->inside_viewport_(Point) && !nRow &&
		m_bEmptyPress && !m_bEmptyPressMoved && bNoModifierNow;
	const bool bDragConsumed = m_bDragConsumedPress;
	const domain::S_CARD* pCard = (m_pProjection && nRow) ? m_pProjection->CardAt(*nRow) : nullptr;
	const std::string sCardId = pCard ? pCard->sId : std::string{};

	// 3. 상태 초기화 + 캡처 해제.
	this->reset_press_state_();
	if (::GetCapture() == this->m_hWnd) { ::ReleaseCapture(); }
	if (this->IsWindow()) { this->Invalidate(FALSE); }

	// 4~7. 빈 영역 신호 -> 드래그 소비 -> Ctrl/무효 행 -> 열기.
	if (bEmptyAreaClicked && m_EmptyAreaClick) { m_EmptyAreaClick(); }
	if (bDragConsumed || !pCard || bCtrl) { return(0); }
	// 여는 카드는 press 한 행이 아니라 release 지점의 행이다(M15/M20).
	if (m_OpenCard) { m_OpenCard(sCardId); }
	return(0);
}

LRESULT C_CARD_LIST::OnRButtonDown(UINT, WPARAM, LPARAM _lParam, BOOL&)
{
	// 원본 mousePressEvent 는 버튼을 가리지 않고 먼저 취소한다.
	this->CancelPendingBrowse();
	// 원본 _select_context_index: 미선택 행이면 그 행만 남기고, 선택된 행이면 선택을 유지한 채
	// 현재만 옮긴다. 컨텍스트 메뉴 자체는 S4 소유라 여기서는 선택 조정만 한다.
	this->observe_current_();
	m_bFocusByMouse = true;
	::SetFocus(this->m_hWnd);
	m_bFocusByMouse = false;
	if (!m_pProjection) { return(0); }
	const std::optional<std::size_t> nRow = this->row_at_point_(this->point_from_lparam_(_lParam));
	const domain::S_CARD* pCard = nRow ? m_pProjection->CardAt(*nRow) : nullptr;
	if (!pCard) { return(0); }
	const std::string sCardId = pCard->sId;
	if (!this->IsRowSelected(*nRow))
	{
		m_pProjection->SelectVisibleCard(sCardId, domain::E_CARD_SELECTION_INTENT::Replace);
	}
	m_pProjection->SetCurrentCardId(sCardId);
	this->set_anchor_(nRow);
	m_ShiftBase = m_pProjection->SelectedCardIds();
	this->observe_current_();
	if (this->IsWindow()) { this->Invalidate(FALSE); }
	return(0);
}

LRESULT C_CARD_LIST::OnCaptureChanged(UINT, WPARAM, LPARAM, BOOL& _bHandled)
{
	this->reset_press_state_();
	_bHandled = FALSE;
	return(0);
}

LRESULT C_CARD_LIST::OnMouseWheel(UINT, WPARAM _wParam, LPARAM, BOOL& _bHandled)
{
	// 고해상도 장치는 WHEEL_DELTA 보다 작은 각을 보낸다 - 부호 있는 short 로 읽는다.
	const int nDelta = GET_WHEEL_DELTA_WPARAM(_wParam);
	const WORD nKeys = GET_KEYSTATE_WPARAM(_wParam);
	const bool bCtrl = (nKeys & MK_CONTROL) != 0;
	const bool bShift = (nKeys & MK_SHIFT) != 0;
	// MK_* 에는 Alt·Win 비트가 없다 - 키 상태 표에서 읽는다(S2 §3.1.3 과 같은 규칙).
	const bool bAlt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
	const bool bMeta = ((::GetKeyState(VK_LWIN) | ::GetKeyState(VK_RWIN)) & 0x8000) != 0;
	// 비소비 분기(원본 wheelEvent 의 네 이접지): 빈 목록·0 각·수식키 중 Alt/Win.
	// Alt 는 Windows QPA 가 세로 휠을 수평으로 바꿔 넣어(오라클 P13) 원본이 가로 스크롤바
	// (범위 0)로 흘려보내고 미수락으로 끝난다. Win 키는 원본이 줄 스크롤로 흘리는데,
	// 여기서는 "탐색이 소비하지 않는다" 까지만 보장한다(줄 스크롤 잔여 = CAP-FI-064 부분).
	if (!m_pProjection || !m_pWheel || this->RowCount() == 0 || nDelta == 0 || bAlt || bMeta)
	{
		// DefWindowProc 이 WM_MOUSEWHEEL 을 부모로 올린다 - Qt "미수락 -> 부모에게" 의 쌍둥이다.
		_bHandled = FALSE;
		return(0);
	}
	if (bCtrl || bShift)
	{
		// Qt QAbstractSliderPrivate::scrollByDelta 의 Ctrl/Shift 갈래는 델타와 무관하게
		// 한 페이지다(오라클 P1/P2: -120 도 -360 도 636). pageStep = viewport 높이다.
		const int nViewport = this->ViewportHeightDip();
		const int nBefore = m_nScrollOffsetDip;
		// Qt 는 int(offset * pageStep) 로 절사한다(L2 감사 프로브: -13 -> 75, -17 -> 98).
		const int nBy = (std::clamp)(static_cast<int>(
			static_cast<double>(nDelta) * nViewport / 120.0), -nViewport, nViewport);
		this->ScrollToPixel(m_nScrollOffsetDip - nBy);
		// 값이 바뀌지 않으면 Qt 는 수락하지 않는다(P4/P11) - 부모가 처리 기회를 갖는다.
		if (m_nScrollOffsetDip == nBefore) { _bHandled = FALSE; }
		return(0);
	}
	const int nBefore = m_pWheel->AngleRemainder();
	const domain::S_CARD_WHEEL_RESULT Result = m_pWheel->OnVerticalAngle(nDelta);
	// CEILING: core 결과에 steps 가 없어 잔여각 전후로 역산한다, core 가 nSteps 를 노출하면 교체
	// (card_wheel_browse.cpp:17~23 의 대수적 역함수 - 방향 반전 시 폐기되는 잔여 각까지 반영한다).
	const int nAdjusted = (nBefore != 0 && (nDelta > 0) != (nBefore > 0)) ? 0 : nBefore;
	const int nSteps = (nAdjusted + nDelta - Result.nAngleRemainder) /
		domain::CARD_WHEEL_VERTICAL_ANGLE_STEP;
	// 원본은 steps 가 0 이 아닐 때만 setCurrentIndex + scrollTo 를 부른다(card_stream.py:342~351).
	// 한 틱을 못 채운 각은 행도 스크롤도 건드리지 않는다(오라클 P12b).
	if (nSteps != 0 && Result.nCurrentRow) { this->SetCurrentRow(*Result.nCurrentRow); }
	this->observe_current_();
	if (this->IsWindow()) { this->Invalidate(FALSE); }
	// 탐색 경로는 행 이동 여부와 무관하게 늘 소비한다(원본 event.accept(), 오라클 [NOTES] 6).
	return(0);
}

LRESULT C_CARD_LIST::OnTimer(UINT, WPARAM _wParam, LPARAM, BOOL& _bHandled)
{
	if (_wParam != CARD_LIST_WHEEL_TIMER_ID) { _bHandled = FALSE; return(0); }
	// 단발 타이머다 - 처리 전에 먼저 걷는다.
	if (this->IsWindow()) { ::KillTimer(this->m_hWnd, CARD_LIST_WHEEL_TIMER_ID); }
	if (!m_pWheel) { return(0); }
	// core 는 세대·대기 카드·현재 카드가 모두 맞을 때만 요청을 낸다(CAP-NC-012).
	const std::optional<domain::S_CARD_WHEEL_OPEN_REQUEST> Request =
		m_pWheel->OnTimer(m_nWheelTimerGeneration);
	if (!Request) { return(0); }
	const bool bOpened = m_BrowseCard ? m_BrowseCard(Request->sCardId) : false;
	// 원본은 편집면 카드를 실패 뒤에 읽는다(document_page.py:453) - 미리 캐시하면 열기가
	// 바꿔 놓은 값을 놓친다.
	m_pWheel->SetEditorCardId(m_EditorCard ? m_EditorCard() : std::nullopt);
	const domain::E_CARD_WHEEL_FOCUS_EFFECT eEffect = m_pWheel->CompleteOpen(*Request, bOpened);
	if (eEffect == domain::E_CARD_WHEEL_FOCUS_EFFECT::FocusEditor)
	{
		// 원본 reveal_card 는 취소까지 한다 - core OnTimer 는 대기 카드·데드라인만 지우므로
		// 잔여 각은 여기서 RevealRow 의 취소로 0 이 된다(L2 프로브 [D]).
		std::optional<std::size_t> nRow;
		const std::optional<std::string>& sEditorCard = m_pWheel->EditorCardId();
		if (m_pProjection && sEditorCard) { nRow = m_pProjection->RowForCard(*sEditorCard); }
		if (nRow) { this->RevealRow(*nRow); }
	}
	this->observe_current_();
	if (this->IsWindow()) { this->Invalidate(FALSE); }
	return(0);
}

LRESULT C_CARD_LIST::OnOtherButtonDown(UINT, WPARAM, LPARAM, BOOL& _bHandled)
{
	// Qt 는 가운데·X 버튼에도 mousePressEvent 를 주고 원본은 그 앞에서 취소한다.
	// 컨트롤은 이 버튼들을 처리하지 않으므로 취소만 하고 흘려보낸다.
	this->CancelPendingBrowse();
	_bHandled = FALSE;
	return(0);
}

LRESULT C_CARD_LIST::OnSysKey(UINT, WPARAM, LPARAM, BOOL& _bHandled)
{
	// Alt 조합은 Win32 가 WM_SYSKEYDOWN/WM_SYSCHAR 로 돌리지만 Qt 에는 같은 keyPressEvent 다.
	// 메뉴 활성화 기본 동작은 그대로 두고 취소만 얹는다.
	this->CancelPendingBrowse();
	_bHandled = FALSE;
	return(0);
}

LRESULT C_CARD_LIST::OnDeferred(UINT, WPARAM, LPARAM, BOOL&)
{
	if (m_Deferred.empty()) { return(0); }
	std::function<void()> Callable = std::move(m_Deferred.front());
	m_Deferred.erase(m_Deferred.begin());
	if (Callable) { Callable(); }
	return(0);
}

LRESULT C_CARD_LIST::OnChar(UINT, WPARAM _wParam, LPARAM, BOOL& _bHandled)
{
	// Win32 는 한 번의 타건을 WM_KEYDOWN + WM_CHAR 로 쪼갠다 - 원본은 keyPressEvent 한 번에서
	// 취소하므로 양쪽 다 취소해야 합성 WM_CHAR 단독도 Qt 와 같게 움직인다. 검색 가드보다 앞이다.
	this->CancelPendingBrowse();
	// Qt keyboardSearch 의 modified 가드: 제어 문자와 Ctrl/Alt 조합은 검색에 실리지 않는다.
	if (_wParam < 0x20 || (::GetKeyState(VK_CONTROL) & 0x8000) != 0 ||
		(::GetKeyState(VK_MENU) & 0x8000) != 0)
	{
		_bHandled = FALSE;
		return(0);
	}
	this->observe_current_();
	this->keyboard_search_(static_cast<wchar_t>(_wParam));
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
	// 원본 keyPressEvent(:302)는 소비하지 않는 키에도 먼저 취소한다.
	this->CancelPendingBrowse();
	this->observe_current_();
	switch (static_cast<int>(_wParam))
	{
	case VK_RETURN:
		// 관측 계약: CardListHwnd() 에 WM_KEYDOWN(VK_RETURN) 을 보내면 카드가 열린다.
		// 현재 카드가 없어도 소비한다(원본도 emit 뒤 accept 하고, 없으면 super 가 무시한다).
		if (m_Activate) { m_Activate(); }
		return(0);
	case VK_DELETE:
	{
		std::vector<std::string> Ids =
			m_pProjection ? m_pProjection->CopySelectionForCommand() : std::vector<std::string>{};
		// 선택이 비었으면 원본도 super 로 흘린다 - 여기서는 소비하지 않는다.
		if (Ids.empty()) { _bHandled = FALSE; return(0); }
		if (m_Delete) { m_Delete(std::move(Ids)); }
		return(0);
	}
	case VK_SPACE:
		this->handle_space_key_();
		return(0);
	case VK_UP:
	case VK_DOWN:
	case VK_HOME:
	case VK_END:
	case VK_PRIOR:
	case VK_NEXT:
		this->handle_navigation_key_(static_cast<int>(_wParam));
		return(0);
	default:
		// 좌우 방향키는 전 폭 행의 ListMode 에서 현재 인덱스를 그대로 돌려준다(K5) - 무동작이다.
		_bHandled = FALSE;
		return(0);
	}
}

LRESULT C_CARD_LIST::OnFocusChanged(UINT _uMessage, WPARAM, LPARAM, BOOL& _bHandled)
{
	// Qt QAbstractItemView::focusInEvent: 자기 마우스 press 가 아닌 포커스 진입에서 현재 행이
	// 없으면 행 0 을 현재로 잡는다(선택은 건드리지 않는다 - 오라클 N3 의 start 행).
	if (_uMessage == WM_SETFOCUS && !m_bFocusByMouse && m_pProjection &&
		!m_bCurrentObserved && this->RowCount() > 0 && !this->current_row_())
	{
		const domain::S_CARD* pCard = m_pProjection->CardAt(0);
		if (pCard)
		{
			m_pProjection->SetCurrentCardId(pCard->sId);
			m_bCurrentObserved = true;
		}
	}
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
	// 창이 사라지면 타이머를 먼저 걷고 상태 기계를 놓는다(떠난 창으로 WM_TIMER 가 떨어지지 않게).
	::KillTimer(this->m_hWnd, CARD_LIST_WHEEL_TIMER_ID);
	m_nWheelTimerGeneration = 0;
	m_pWheel.reset();
	m_Target.Shutdown();
	m_bTargetReady = false;
	m_bRecoveringDevice = false;
	m_bTrackingMouse = false;
	m_nHoverRow.reset();
	if (::GetCapture() == this->m_hWnd) { ::ReleaseCapture(); }
	this->reset_press_state_();
	m_Deferred.clear();
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
		m_sAnchorCardId.reset();
		m_ShiftBase.clear();
		if (this->IsWindow()) { this->Invalidate(FALSE); }
		return(LB_ERR);
	}
	if (nRow < 0 || static_cast<std::size_t>(nRow) >= m_pProjection->RowCount()) { return(LB_ERR); }
	if (!m_pProjection->CardAt(static_cast<std::size_t>(nRow))) { return(LB_ERR); }
	// Qt setCurrentIndex 와 같은 자리다 - 선택·앵커·Shift 기준·EnsureVisible 을 한 곳에서 쓴다.
	this->SetCurrentRow(static_cast<std::size_t>(nRow));
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
