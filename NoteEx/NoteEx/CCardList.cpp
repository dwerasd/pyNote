#include "CCardList.h"

// UIA 는 CCardList.h(ATL/WTL + ole2.h) 뒤, core 도메인 헤더 앞이다 - 이 파일은
// CDocumentPage.cpp 와 달리 #undef CreateEvent 를 하지 않는다(어느 core 헤더도 여기서
// 충돌하는 CreateEvent 를 선언하지 않는다, spec §3.3.4).
#include <uiautomation.h>

#include <D2DWrapp/D2DBrushCache.h>
#include <D2DWrapp/D2DDevice.h>

#include "pynote/core/domain/card_drag_session_registry.h"
#include "pynote/core/domain/card_list_projection.h"
#include "pynote/core/domain/card_wheel_browse.h"
#include "pynote/core/domain/date_time_formatter.h"
#include "pynote/platform/win32_time_zone_resolver.h"

#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <utility>

#pragma comment(lib, "D2DWrapp")
// RegisterDragDrop·DoDragDrop·ReleaseStgMedium 은 ole32 다(기본 의존 목록에 있으나 명시한다).
#pragma comment(lib, "Ole32")
// UiaReturnRawElementProvider·UiaHostProviderFromHwnd 자리다(spec §3.3.4).
#pragma comment(lib, "uiautomationcore")

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

	// =====================================================================================
	// S4 - 드래그 앤 드롭 payload·COM 개체·컨텍스트 메뉴 상수
	// =====================================================================================

	// 원본 CARD_MIME_TYPE(card_model.py:20)이고 지시서 §2 의 MIME 정본이다.
	constexpr wchar_t CARD_MIME_FORMAT_NAME[] = L"application/x-pynote-card-id";
	// 원본 스타일시트(card_stream.py:598~614): 배경 rgba(178,45,45,220), 테두리
	// 1px rgba(255,255,255,110), 라벨 흰색. 알파는 D2D 가 그대로 합성한다.
	constexpr d2d::Color DELETE_ZONE_FILL_COLOR = 0xDCB22D2Du;
	constexpr d2d::Color DELETE_ZONE_BORDER_COLOR = 0x6EFFFFFFu;
	constexpr d2d::Color DELETE_ZONE_LABEL_COLOR = 0xFFFFFFFFu;
	constexpr wchar_t DELETE_ZONE_LABEL_TEXT[] = L"여기에 놓으면 휴지통으로 이동";
	// 런타임 메뉴의 파일 지역 명령 id 다. TPM_RETURNCMD 가 값을 직접 돌리므로
	// Resource.h 에 자리를 만들지 않는다(원본도 메뉴 리소스를 쓰지 않는다).
	constexpr UINT CARD_MENU_OPEN = 0x7101;
	constexpr UINT CARD_MENU_COPY = 0x7102;
	constexpr UINT CARD_MENU_EXPORT = 0x7103;
	constexpr UINT CARD_MENU_DELETE = 0x7104;
	// 클립보드는 프로세스 밖이 잠깐 잡을 수 있는 공유 자원이다 - MSDN 권고대로 유계 재시도한다.
	constexpr int CLIPBOARD_ATTEMPTS = 20;
	constexpr DWORD CLIPBOARD_RETRY_MS = 10;

	CLIPFORMAT card_mime_format()
	{
		// RegisterClipboardFormatW 는 같은 이름에 늘 같은 값을 돌려준다 - 한 번만 잡는다.
		static const CLIPFORMAT s_nFormat =
			static_cast<CLIPFORMAT>(::RegisterClipboardFormatW(CARD_MIME_FORMAT_NAME));
		return(s_nFormat);
	}

	// Qt QWindowsMimeText::convertFromMime(qwindowsmimeregistry.cpp:413~448)과 같은 규칙이다 -
	// 홑 LF 에만 CR 을 붙이고 이미 CRLF 인 곳은 겹치지 않는다.
	std::wstring to_crlf(const std::wstring& _sValue)
	{
		std::wstring Result;
		Result.reserve(_sValue.size() + 8);
		for (std::size_t nIndex = 0; nIndex < _sValue.size(); ++nIndex)
		{
			if (_sValue[nIndex] == L'\n' && (nIndex == 0 || _sValue[nIndex - 1] != L'\r'))
			{
				Result.push_back(L'\r');
			}
			Result.push_back(_sValue[nIndex]);
		}
		return(Result);
	}

	struct S_CARD_MIME_PAYLOAD
	{
		std::string sCardId;
		std::optional<std::string> sRevisionId{};
		domain::CardDragSessionToken nToken{ 0 };
	};

	// ---- 최소 JSON 판독기 ----------------------------------------------------------------
	// 원본 _card_mime_payload(card_stream.py:64~85)의 거절 조건(객체가 아님·형 불일치·
	// bool 토큰·0 이하 토큰·디코드 실패)을 재현하는 데 필요한 만큼만 갖춘다.
	enum class E_JSON_KIND { Null, Bool, Number, String, Array, Object };

	void json_skip_ws(std::string_view _sText, std::size_t& _nIndex) noexcept
	{
		while (_nIndex < _sText.size())
		{
			const char ch = _sText[_nIndex];
			if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') { break; }
			++_nIndex;
		}
	}

	void utf8_append(std::string& _sOut, unsigned int _nCodePoint)
	{
		if (_nCodePoint < 0x80) { _sOut.push_back(static_cast<char>(_nCodePoint)); }
		else if (_nCodePoint < 0x800)
		{
			_sOut.push_back(static_cast<char>(0xC0 | (_nCodePoint >> 6)));
			_sOut.push_back(static_cast<char>(0x80 | (_nCodePoint & 0x3F)));
		}
		else if (_nCodePoint < 0x10000)
		{
			_sOut.push_back(static_cast<char>(0xE0 | (_nCodePoint >> 12)));
			_sOut.push_back(static_cast<char>(0x80 | ((_nCodePoint >> 6) & 0x3F)));
			_sOut.push_back(static_cast<char>(0x80 | (_nCodePoint & 0x3F)));
		}
		else
		{
			_sOut.push_back(static_cast<char>(0xF0 | (_nCodePoint >> 18)));
			_sOut.push_back(static_cast<char>(0x80 | ((_nCodePoint >> 12) & 0x3F)));
			_sOut.push_back(static_cast<char>(0x80 | ((_nCodePoint >> 6) & 0x3F)));
			_sOut.push_back(static_cast<char>(0x80 | (_nCodePoint & 0x3F)));
		}
	}

	bool json_read_hex4(std::string_view _sText, std::size_t& _nIndex, unsigned int* _pOut)
	{
		if (_nIndex + 4 > _sText.size()) { return(false); }
		unsigned int nValue = 0;
		for (int nDigit = 0; nDigit < 4; ++nDigit)
		{
			const char ch = _sText[_nIndex + static_cast<std::size_t>(nDigit)];
			unsigned int nNibble = 0;
			if (ch >= '0' && ch <= '9') { nNibble = static_cast<unsigned int>(ch - '0'); }
			else if (ch >= 'a' && ch <= 'f') { nNibble = static_cast<unsigned int>(ch - 'a') + 10; }
			else if (ch >= 'A' && ch <= 'F') { nNibble = static_cast<unsigned int>(ch - 'A') + 10; }
			else { return(false); }
			nValue = nValue * 16 + nNibble;
		}
		_nIndex += 4;
		*_pOut = nValue;
		return(true);
	}

	bool json_read_string(std::string_view _sText, std::size_t& _nIndex, std::string* _pOut)
	{
		if (_nIndex >= _sText.size() || _sText[_nIndex] != '"') { return(false); }
		++_nIndex;
		std::string Result;
		while (_nIndex < _sText.size())
		{
			const char ch = _sText[_nIndex];
			if (ch == '"')
			{
				++_nIndex;
				if (_pOut) { *_pOut = std::move(Result); }
				return(true);
			}
			if (ch == '\\')
			{
				++_nIndex;
				if (_nIndex >= _sText.size()) { return(false); }
				const char chEscape = _sText[_nIndex++];
				switch (chEscape)
				{
				case '"': Result.push_back('"'); break;
				case '\\': Result.push_back('\\'); break;
				case '/': Result.push_back('/'); break;
				case 'b': Result.push_back('\b'); break;
				case 'f': Result.push_back('\f'); break;
				case 'n': Result.push_back('\n'); break;
				case 'r': Result.push_back('\r'); break;
				case 't': Result.push_back('\t'); break;
				case 'u':
				{
					unsigned int nFirst = 0;
					if (!json_read_hex4(_sText, _nIndex, &nFirst)) { return(false); }
					unsigned int nCodePoint = nFirst;
					if (nFirst >= 0xD800 && nFirst <= 0xDBFF &&
						_nIndex + 1 < _sText.size() && _sText[_nIndex] == '\\' && _sText[_nIndex + 1] == 'u')
					{
						_nIndex += 2;
						unsigned int nSecond = 0;
						if (!json_read_hex4(_sText, _nIndex, &nSecond)) { return(false); }
						nCodePoint = 0x10000u + ((nFirst - 0xD800u) << 10) + (nSecond - 0xDC00u);
					}
					utf8_append(Result, nCodePoint);
					break;
				}
				default: return(false);
				}
				continue;
			}
			// 원본 json 은 따옴표 안의 제어문자를 거절한다.
			if (static_cast<unsigned char>(ch) < 0x20) { return(false); }
			Result.push_back(ch);
			++_nIndex;
		}
		return(false);
	}

	bool json_read_number(std::string_view _sText, std::size_t& _nIndex, std::string* _pOut)
	{
		const std::size_t nStart = _nIndex;
		if (_nIndex < _sText.size() && _sText[_nIndex] == '-') { ++_nIndex; }
		if (_nIndex >= _sText.size() || _sText[_nIndex] < '0' || _sText[_nIndex] > '9') { return(false); }
		if (_sText[_nIndex] == '0') { ++_nIndex; }
		else { while (_nIndex < _sText.size() && _sText[_nIndex] >= '0' && _sText[_nIndex] <= '9') { ++_nIndex; } }
		if (_nIndex < _sText.size() && _sText[_nIndex] == '.')
		{
			++_nIndex;
			if (_nIndex >= _sText.size() || _sText[_nIndex] < '0' || _sText[_nIndex] > '9') { return(false); }
			while (_nIndex < _sText.size() && _sText[_nIndex] >= '0' && _sText[_nIndex] <= '9') { ++_nIndex; }
		}
		if (_nIndex < _sText.size() && (_sText[_nIndex] == 'e' || _sText[_nIndex] == 'E'))
		{
			++_nIndex;
			if (_nIndex < _sText.size() && (_sText[_nIndex] == '+' || _sText[_nIndex] == '-')) { ++_nIndex; }
			if (_nIndex >= _sText.size() || _sText[_nIndex] < '0' || _sText[_nIndex] > '9') { return(false); }
			while (_nIndex < _sText.size() && _sText[_nIndex] >= '0' && _sText[_nIndex] <= '9') { ++_nIndex; }
		}
		if (_pOut) { *_pOut = std::string(_sText.substr(nStart, _nIndex - nStart)); }
		return(true);
	}

	bool json_read_value(std::string_view _sText, std::size_t& _nIndex,
		E_JSON_KIND* _pKind, std::string* _pValue);

	// 중첩 값은 형만 확인하고 건너뛴다 - 이 payload 에는 중첩이 없지만 문법 위반은 걸러야 한다.
	bool json_skip_container(std::string_view _sText, std::size_t& _nIndex, bool _bObject)
	{
		const char chOpen = _bObject ? '{' : '[';
		const char chClose = _bObject ? '}' : ']';
		if (_nIndex >= _sText.size() || _sText[_nIndex] != chOpen) { return(false); }
		++_nIndex;
		json_skip_ws(_sText, _nIndex);
		if (_nIndex < _sText.size() && _sText[_nIndex] == chClose) { ++_nIndex; return(true); }
		for (;;)
		{
			json_skip_ws(_sText, _nIndex);
			if (_bObject)
			{
				if (!json_read_string(_sText, _nIndex, nullptr)) { return(false); }
				json_skip_ws(_sText, _nIndex);
				if (_nIndex >= _sText.size() || _sText[_nIndex] != ':') { return(false); }
				++_nIndex;
			}
			if (!json_read_value(_sText, _nIndex, nullptr, nullptr)) { return(false); }
			json_skip_ws(_sText, _nIndex);
			if (_nIndex < _sText.size() && _sText[_nIndex] == ',') { ++_nIndex; continue; }
			if (_nIndex < _sText.size() && _sText[_nIndex] == chClose) { ++_nIndex; return(true); }
			return(false);
		}
	}

	bool json_read_value(std::string_view _sText, std::size_t& _nIndex,
		E_JSON_KIND* _pKind, std::string* _pValue)
	{
		json_skip_ws(_sText, _nIndex);
		if (_nIndex >= _sText.size()) { return(false); }
		const char ch = _sText[_nIndex];
		if (ch == '"')
		{
			if (_pKind) { *_pKind = E_JSON_KIND::String; }
			return(json_read_string(_sText, _nIndex, _pValue));
		}
		if (ch == '{')
		{
			if (_pKind) { *_pKind = E_JSON_KIND::Object; }
			return(json_skip_container(_sText, _nIndex, true));
		}
		if (ch == '[')
		{
			if (_pKind) { *_pKind = E_JSON_KIND::Array; }
			return(json_skip_container(_sText, _nIndex, false));
		}
		if (_sText.compare(_nIndex, 4, "null") == 0)
		{
			_nIndex += 4;
			if (_pKind) { *_pKind = E_JSON_KIND::Null; }
			return(true);
		}
		if (_sText.compare(_nIndex, 4, "true") == 0)
		{
			_nIndex += 4;
			if (_pKind) { *_pKind = E_JSON_KIND::Bool; }
			return(true);
		}
		if (_sText.compare(_nIndex, 5, "false") == 0)
		{
			_nIndex += 5;
			if (_pKind) { *_pKind = E_JSON_KIND::Bool; }
			return(true);
		}
		if (_pKind) { *_pKind = E_JSON_KIND::Number; }
		return(json_read_number(_sText, _nIndex, _pValue));
	}

	std::optional<S_CARD_MIME_PAYLOAD> parse_card_payload(std::string_view _sJson)
	{
		std::size_t nIndex = 0;
		json_skip_ws(_sJson, nIndex);
		// 원본은 dict 가 아니면 거절한다(배열·수·문자열 전부).
		if (nIndex >= _sJson.size() || _sJson[nIndex] != '{') { return(std::nullopt); }
		++nIndex;
		S_CARD_MIME_PAYLOAD Payload;
		bool bHasCardId = false;
		bool bHasToken = false;
		json_skip_ws(_sJson, nIndex);
		if (nIndex < _sJson.size() && _sJson[nIndex] == '}') { ++nIndex; }
		else
		{
			for (;;)
			{
				json_skip_ws(_sJson, nIndex);
				std::string sKey;
				if (!json_read_string(_sJson, nIndex, &sKey)) { return(std::nullopt); }
				json_skip_ws(_sJson, nIndex);
				if (nIndex >= _sJson.size() || _sJson[nIndex] != ':') { return(std::nullopt); }
				++nIndex;
				E_JSON_KIND eKind = E_JSON_KIND::Null;
				std::string sValue;
				if (!json_read_value(_sJson, nIndex, &eKind, &sValue)) { return(std::nullopt); }
				if (sKey == "card_id")
				{
					if (eKind != E_JSON_KIND::String) { return(std::nullopt); }
					Payload.sCardId = std::move(sValue);
					bHasCardId = true;
				}
				else if (sKey == "revision_id")
				{
					if (eKind == E_JSON_KIND::String) { Payload.sRevisionId = std::move(sValue); }
					else if (eKind != E_JSON_KIND::Null) { return(std::nullopt); }
				}
				else if (sKey == "token")
				{
					// 원본은 bool 을 int 로 받지 않고(파이썬 bool 은 int 의 부분형이라 명시 배제),
					// 실수도 int 가 아니다. JSON 에서 true 는 수가 아니고 소수·지수는 실수다.
					if (eKind != E_JSON_KIND::Number) { return(std::nullopt); }
					if (sValue.find_first_of(".eE") != std::string::npos) { return(std::nullopt); }
					if (sValue.empty() || sValue[0] == '-') { return(std::nullopt); }
					errno = 0;
					char* pEnd = nullptr;
					const unsigned long long nToken = std::strtoull(sValue.c_str(), &pEnd, 10);
					if (errno == ERANGE || pEnd != sValue.c_str() + sValue.size() || nToken == 0)
					{
						return(std::nullopt);
					}
					Payload.nToken = static_cast<domain::CardDragSessionToken>(nToken);
					bHasToken = true;
				}
				json_skip_ws(_sJson, nIndex);
				if (nIndex < _sJson.size() && _sJson[nIndex] == ',') { ++nIndex; continue; }
				if (nIndex < _sJson.size() && _sJson[nIndex] == '}') { ++nIndex; break; }
				return(std::nullopt);
			}
		}
		json_skip_ws(_sJson, nIndex);
		if (nIndex != _sJson.size()) { return(std::nullopt); }
		// 없는 키는 원본 .get() 이 None 을 주므로 card_id·token 은 형 검사에서 걸리고
		// revision_id 는 None(리비전 없음)으로 성립한다.
		if (!bHasCardId || !bHasToken) { return(std::nullopt); }
		return(Payload);
	}

	// 원본 json.dumps(ensure_ascii=False, separators=(",", ":")) 와 같은 바이트를 낸다 -
	// 비 ASCII 는 UTF-8 원문 그대로이고 제어문자만 이스케이프한다.
	void json_append_escaped(std::string& _sOut, const std::string& _sValue)
	{
		for (const char ch : _sValue)
		{
			const unsigned char nByte = static_cast<unsigned char>(ch);
			switch (ch)
			{
			case '"': _sOut += "\\\""; continue;
			case '\\': _sOut += "\\\\"; continue;
			case '\b': _sOut += "\\b"; continue;
			case '\f': _sOut += "\\f"; continue;
			case '\n': _sOut += "\\n"; continue;
			case '\r': _sOut += "\\r"; continue;
			case '\t': _sOut += "\\t"; continue;
			default: break;
			}
			if (nByte < 0x20)
			{
				char Buffer[8]{};
				::sprintf_s(Buffer, "\\u%04x", static_cast<unsigned int>(nByte));
				_sOut += Buffer;
			}
			else { _sOut.push_back(ch); }
		}
	}

	std::string build_card_payload(const std::string& _sCardId,
		const std::optional<std::string>& _sRevisionId, domain::CardDragSessionToken _nToken)
	{
		// 키 순서는 삽입 순서 고정이고 공백은 없다(부록 A [MIME-BYTES]).
		std::string sJson = "{\"card_id\":\"";
		json_append_escaped(sJson, _sCardId);
		sJson += "\",\"revision_id\":";
		if (_sRevisionId)
		{
			sJson += '"';
			json_append_escaped(sJson, *_sRevisionId);
			sJson += '"';
		}
		else { sJson += "null"; }
		sJson += ",\"token\":";
		sJson += std::to_string(_nToken);
		sJson += "}";
		return(sJson);
	}

	// ---- OLE 개체(TYMED_HGLOBAL 전용) ----------------------------------------------------
	HGLOBAL clone_global(const void* _pData, SIZE_T _nBytes)
	{
		HGLOBAL hMemory = ::GlobalAlloc(GMEM_MOVEABLE, _nBytes);
		if (!hMemory) { return(nullptr); }
		void* pTarget = ::GlobalLock(hMemory);
		if (!pTarget) { ::GlobalFree(hMemory); return(nullptr); }
		if (_nBytes > 0) { std::memcpy(pTarget, _pData, _nBytes); }
		::GlobalUnlock(hMemory);
		return(hMemory);
	}

	FORMATETC make_format(CLIPFORMAT _nFormat) noexcept
	{
		FORMATETC Format{};
		Format.cfFormat = _nFormat;
		Format.ptd = nullptr;
		Format.dwAspect = DVASPECT_CONTENT;
		Format.lindex = -1;
		Format.tymed = TYMED_HGLOBAL;
		return(Format);
	}

	class C_CARD_FORMAT_ENUM final : public IEnumFORMATETC
	{
	public:
		C_CARD_FORMAT_ENUM(std::vector<FORMATETC> _Formats, ULONG _nIndex)
			: m_Formats(std::move(_Formats)), m_nIndex(_nIndex) {}

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID _Riid, void** _ppObject) override
		{
			if (!_ppObject) { return(E_POINTER); }
			*_ppObject = nullptr;
			if (_Riid == IID_IUnknown || _Riid == IID_IEnumFORMATETC)
			{
				*_ppObject = static_cast<IEnumFORMATETC*>(this);
				this->AddRef();
				return(S_OK);
			}
			return(E_NOINTERFACE);
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return(++m_nReference); }
		ULONG STDMETHODCALLTYPE Release() override
		{
			const ULONG nLeft = --m_nReference;
			if (nLeft == 0) { delete this; }
			return(nLeft);
		}
		HRESULT STDMETHODCALLTYPE Next(ULONG _nCount, FORMATETC* _pOut, ULONG* _pFetched) override
		{
			if (_nCount > 0 && !_pOut) { return(E_POINTER); }
			ULONG nCopied = 0;
			while (nCopied < _nCount && m_nIndex < static_cast<ULONG>(m_Formats.size()))
			{
				_pOut[nCopied] = m_Formats[m_nIndex];
				++nCopied;
				++m_nIndex;
			}
			if (_pFetched) { *_pFetched = nCopied; }
			return(nCopied == _nCount ? S_OK : S_FALSE);
		}
		HRESULT STDMETHODCALLTYPE Skip(ULONG _nCount) override
		{
			const ULONG nLeft = static_cast<ULONG>(m_Formats.size()) - m_nIndex;
			m_nIndex += (std::min)(_nCount, nLeft);
			return(_nCount <= nLeft ? S_OK : S_FALSE);
		}
		HRESULT STDMETHODCALLTYPE Reset() override { m_nIndex = 0; return(S_OK); }
		HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC** _ppOut) override
		{
			if (!_ppOut) { return(E_POINTER); }
			*_ppOut = new C_CARD_FORMAT_ENUM(m_Formats, m_nIndex);
			return(S_OK);
		}

	private:
		std::vector<FORMATETC> m_Formats;
		ULONG m_nIndex{ 0 };
		ULONG m_nReference{ 1 };
	};

	// SHCreateStdEnumFmtEtc 를 쓰지 않는다 - COM 할당자 경로를 끌어들이기 때문이다(spec §3.7.2).
	class C_CARD_DATA_OBJECT final : public IDataObject, public I_CARD_DRAG_SOURCE
	{
	public:
		C_CARD_DATA_OBJECT(domain::CardDragSourceIdentity _nSource, std::string _sJson,
			std::wstring _sText)
			: m_nSource(_nSource), m_sJson(std::move(_sJson)), m_sText(std::move(_sText))
		{
			m_Formats.push_back(make_format(card_mime_format()));
			m_Formats.push_back(make_format(CF_UNICODETEXT));
			// Qt 와 같은 조건부 노출이다(qwindowsmimeregistry.cpp:476~483) - 이 기계의 ACP 는
			// 949 라 형식이 셋이고, ANSI 전용 외부 대상도 본문을 받는다(spec §3.1.3c).
			if (::GetACP() != CP_UTF8) { m_Formats.push_back(make_format(CF_TEXT)); }
		}

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

		pynote::core::domain::CardDragSourceIdentity STDMETHODCALLTYPE SourceIdentity() override
		{
			return(m_nSource);
		}

		HRESULT STDMETHODCALLTYPE GetData(FORMATETC* _pFormat, STGMEDIUM* _pMedium) override
		{
			if (!_pFormat || !_pMedium) { return(E_POINTER); }
			*_pMedium = STGMEDIUM{};
			if ((_pFormat->tymed & TYMED_HGLOBAL) == 0) { return(DV_E_TYMED); }
			if (_pFormat->dwAspect != DVASPECT_CONTENT) { return(DV_E_DVASPECT); }
			if (!this->supports_(_pFormat->cfFormat)) { return(DV_E_FORMATETC); }
			HGLOBAL hMemory = this->allocate_(_pFormat->cfFormat);
			if (!hMemory) { return(E_OUTOFMEMORY); }
			_pMedium->tymed = TYMED_HGLOBAL;
			_pMedium->hGlobal = hMemory;
			// pUnkForRelease == nullptr 이면 ReleaseStgMedium 이 GlobalFree 만 부른다 -
			// OLE 초기화 없는 시험 프로세스에서도 성립하는 경로다(spec §3.7.2).
			_pMedium->pUnkForRelease = nullptr;
			return(S_OK);
		}
		HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return(E_NOTIMPL); }
		HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* _pFormat) override
		{
			if (!_pFormat) { return(E_POINTER); }
			if ((_pFormat->tymed & TYMED_HGLOBAL) == 0) { return(DV_E_TYMED); }
			if (_pFormat->dwAspect != DVASPECT_CONTENT) { return(DV_E_DVASPECT); }
			return(this->supports_(_pFormat->cfFormat) ? S_OK : DV_E_FORMATETC);
		}
		HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* _pOut) override
		{
			if (_pOut) { *_pOut = FORMATETC{}; _pOut->ptd = nullptr; }
			return(E_NOTIMPL);
		}
		HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return(E_NOTIMPL); }
		HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD _nDirection, IEnumFORMATETC** _ppOut) override
		{
			if (!_ppOut) { return(E_POINTER); }
			*_ppOut = nullptr;
			if (_nDirection != DATADIR_GET) { return(E_NOTIMPL); }
			*_ppOut = new C_CARD_FORMAT_ENUM(m_Formats, 0);
			return(S_OK);
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
		bool supports_(CLIPFORMAT _nFormat) const
		{
			for (const FORMATETC& Format : m_Formats)
			{
				if (Format.cfFormat == _nFormat) { return(true); }
			}
			return(false);
		}

		HGLOBAL allocate_(CLIPFORMAT _nFormat) const
		{
			if (_nFormat == card_mime_format())
			{
				// 커스텀 형식은 끝 NUL 이 값의 일부가 아니다(부록 A [MIME-BYTES] 51 바이트).
				return(clone_global(m_sJson.data(), m_sJson.size()));
			}
			if (_nFormat == CF_UNICODETEXT)
			{
				// Qt 는 (길이+1)*2 바이트에 끝 NUL 을 넣는다(qwindowsmimeregistry.cpp:454~467).
				return(clone_global(m_sText.c_str(), (m_sText.size() + 1) * sizeof(wchar_t)));
			}
			const int nRequired = ::WideCharToMultiByte(CP_ACP, 0, m_sText.c_str(),
				static_cast<int>(m_sText.size()), nullptr, 0, nullptr, nullptr);
			if (nRequired < 0) { return(nullptr); }
			std::string Ansi(static_cast<std::size_t>(nRequired) + 1, '\0');
			if (nRequired > 0 && ::WideCharToMultiByte(CP_ACP, 0, m_sText.c_str(),
				static_cast<int>(m_sText.size()), Ansi.data(), nRequired, nullptr, nullptr) != nRequired)
			{
				return(nullptr);
			}
			return(clone_global(Ansi.data(), Ansi.size()));
		}

		domain::CardDragSourceIdentity m_nSource{ 0 };
		std::string m_sJson;
		std::wstring m_sText;
		std::vector<FORMATETC> m_Formats;
		ULONG m_nReference{ 1 };
	};

	class C_CARD_DROP_SOURCE final : public IDropSource
	{
	public:
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID _Riid, void** _ppObject) override
		{
			if (!_ppObject) { return(E_POINTER); }
			*_ppObject = nullptr;
			if (_Riid == IID_IUnknown || _Riid == IID_IDropSource)
			{
				*_ppObject = static_cast<IDropSource*>(this);
				this->AddRef();
				return(S_OK);
			}
			return(E_NOINTERFACE);
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return(++m_nReference); }
		ULONG STDMETHODCALLTYPE Release() override
		{
			const ULONG nLeft = --m_nReference;
			if (nLeft == 0) { delete this; }
			return(nLeft);
		}
		HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL _bEscape, DWORD _nKeyState) override
		{
			// OLE 표준 계약이다 - Esc 는 취소, 시작 버튼(좌)이 떨어지면 드롭이다.
			if (_bEscape) { return(DRAGDROP_S_CANCEL); }
			if ((_nKeyState & MK_LBUTTON) == 0) { return(DRAGDROP_S_DROP); }
			return(S_OK);
		}
		HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override
		{
			// 원본은 pixmap 을 걸지 않는다(실측 pixmap().isNull()) - 기본 커서 그대로다.
			return(DRAGDROP_S_USEDEFAULTCURSORS);
		}

	private:
		ULONG m_nReference{ 1 };
	};

	class C_CARD_DROP_TARGET final : public IDropTarget
	{
	public:
		// 컨트롤의 비공개 판정을 부르는 자리다 - 소유 컨트롤 안에서 만든 람다를 받는다.
		using DropCallback = std::function<DWORD(IDataObject*, POINTL, bool)>;

		explicit C_CARD_DROP_TARGET(DropCallback _Callback) : m_Callback(std::move(_Callback)) {}

		// 컨트롤이 죽어도 OLE 가 참조를 들고 있을 수 있다 - 그때는 아무것도 받지 않는다.
		void Detach() noexcept { m_Callback = {}; m_pData.Release(); }

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID _Riid, void** _ppObject) override
		{
			if (!_ppObject) { return(E_POINTER); }
			*_ppObject = nullptr;
			if (_Riid == IID_IUnknown || _Riid == IID_IDropTarget)
			{
				*_ppObject = static_cast<IDropTarget*>(this);
				this->AddRef();
				return(S_OK);
			}
			return(E_NOINTERFACE);
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return(++m_nReference); }
		ULONG STDMETHODCALLTYPE Release() override
		{
			const ULONG nLeft = --m_nReference;
			if (nLeft == 0) { delete this; }
			return(nLeft);
		}
		HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* _pData, DWORD, POINTL _Point,
			DWORD* _pEffect) override
		{
			if (!_pEffect) { return(E_POINTER); }
			m_pData = _pData;
			*_pEffect = m_Callback ? m_Callback(_pData, _Point, false) : DROPEFFECT_NONE;
			return(S_OK);
		}
		HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL _Point, DWORD* _pEffect) override
		{
			if (!_pEffect) { return(E_POINTER); }
			*_pEffect = m_Callback ? m_Callback(m_pData, _Point, false) : DROPEFFECT_NONE;
			return(S_OK);
		}
		HRESULT STDMETHODCALLTYPE DragLeave() override { m_pData.Release(); return(S_OK); }
		HRESULT STDMETHODCALLTYPE Drop(IDataObject* _pData, DWORD, POINTL _Point,
			DWORD* _pEffect) override
		{
			if (!_pEffect) { return(E_POINTER); }
			*_pEffect = m_Callback ? m_Callback(_pData, _Point, true) : DROPEFFECT_NONE;
			m_pData.Release();
			return(S_OK);
		}

	private:
		DropCallback m_Callback;
		CComPtr<IDataObject> m_pData;
		ULONG m_nReference{ 1 };
	};

	// 데이터 개체가 밝힌 원본 동일성(원본 event.source() is self 의 쌍둥이).
	domain::CardDragSourceIdentity source_identity_of(IDataObject* _pData)
	{
		if (!_pData) { return(0); }
		CComPtr<I_CARD_DRAG_SOURCE> pSource;
		if (FAILED(_pData->QueryInterface(__uuidof(I_CARD_DRAG_SOURCE),
			reinterpret_cast<void**>(&pSource))) || !pSource) { return(0); }
		return(pSource->SourceIdentity());
	}

	// TYMED_HGLOBAL 한 형식을 문자열로 읽는다. 실패는 빈 optional 이다.
	std::optional<std::string> read_global_bytes(IDataObject* _pData, CLIPFORMAT _nFormat)
	{
		if (!_pData) { return(std::nullopt); }
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
		::ReleaseStgMedium(&Medium);
		return(Result);
	}

	// WM_GETOBJECT 마다 새로 만들고 호출이 끝나면 곧바로 놓는다(decision H-6, spec §3.3.1/§3.3.3).
	// 원시 C_CARD_LIST* 를 들지 않고 공유 원자 플래그만 들어 - 창이 죽은 뒤 살아남은 참조도
	// nullptr 을 읽고 VT_EMPTY/UIA_E_ELEMENTNOTAVAILABLE 로 답한다(dangling 역참조 없음).
	// CEILING: 행별 IRawElementProviderFragment 트리는 W7 이후 별건(상향 경로 =
	// FragmentRoot + 행 fragment 구현)
	class C_CARD_LIST_UIA_PROVIDER final : public IRawElementProviderSimple
	{
	public:
		explicit C_CARD_LIST_UIA_PROVIDER(
			std::shared_ptr<std::atomic<C_CARD_LIST*>> _pLiveness)
			: m_pLiveness(std::move(_pLiveness)) {}

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID _Riid, void** _ppObject) override
		{
			if (!_ppObject) { return(E_POINTER); }
			*_ppObject = nullptr;
			if (_Riid == IID_IUnknown || _Riid == __uuidof(IRawElementProviderSimple))
			{
				*_ppObject = static_cast<IRawElementProviderSimple*>(this);
				this->AddRef();
				return(S_OK);
			}
			return(E_NOINTERFACE);
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return(++m_nReference); }
		ULONG STDMETHODCALLTYPE Release() override
		{
			const ULONG nLeft = --m_nReference;
			if (nLeft == 0) { delete this; }
			return(nLeft);
		}
		HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* _pOptions) override
		{
			if (!_pOptions) { return(E_POINTER); }
			*_pOptions = ProviderOptions_ServerSideProvider;
			return(S_OK);
		}
		HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID, IUnknown** _ppOut) override
		{
			// 이 슬라이스는 어떤 UIA 패턴도 구현하지 않는다(§3.3의 선언된 상한).
			if (_ppOut) { *_ppOut = nullptr; }
			return(S_OK);
		}
		HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID _nPropertyId, VARIANT* _pValue) override
		{
			if (!_pValue) { return(E_POINTER); }
			::VariantInit(_pValue);
			C_CARD_LIST* pControl = m_pLiveness ? m_pLiveness->load() : nullptr;
			// 창이 죽은 뒤(또는 아직 붙지 않은 채) 들어온 질의는 전부 VT_EMPTY 다(spec §3.3.3).
			if (!pControl) { return(S_OK); }
			if (_nPropertyId == UIA_ControlTypePropertyId)
			{
				_pValue->vt = VT_I4;
				_pValue->lVal = UIA_ListControlTypeId;
			}
			else if (_nPropertyId == UIA_NamePropertyId)
			{
				std::wstring sName;
				const domain::C_CARD_LIST_PROJECTION* pProjection = pControl->Projection();
				if (pProjection && pProjection->CurrentCardId())
				{
					const std::optional<std::string_view> Body =
						pProjection->FullBodyForCard(*pProjection->CurrentCardId());
					// 정규화·자르기·트리밍 없음 - CR·탭·앞뒤 공백·2만자 본문을 바이트 그대로 싣는다.
					if (Body) { sName = wide(std::string(*Body)); }
				}
				_pValue->vt = VT_BSTR;
				_pValue->bstrVal = ::SysAllocString(sName.c_str());
			}
			else if (_nPropertyId == UIA_IsControlElementPropertyId ||
				_nPropertyId == UIA_IsContentElementPropertyId)
			{
				_pValue->vt = VT_BOOL;
				_pValue->boolVal = VARIANT_TRUE;
			}
			// 그 밖의 모든 속성은 VT_EMPTY 로 남는다(VariantInit 이 이미 그렇게 했다).
			return(S_OK);
		}
		HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(
			IRawElementProviderSimple** _ppOut) override
		{
			if (!_ppOut) { return(E_POINTER); }
			*_ppOut = nullptr;
			C_CARD_LIST* pControl = m_pLiveness ? m_pLiveness->load() : nullptr;
			if (!pControl || !pControl->IsWindow()) { return(S_OK); }
			return(::UiaHostProviderFromHwnd(pControl->m_hWnd, _ppOut));
		}

	private:
		std::shared_ptr<std::atomic<C_CARD_LIST*>> m_pLiveness;
		ULONG m_nReference{ 1 };
	};
}

// 등록소는 컨트롤 인스턴스마다 하나다 - 토큰이 인스턴스 안에서만 단조 증가하고 창끼리
// 섞이지 않는 것이 §3.1.5 와 §3.2.2 의 창간 거절이 서는 근거다.
struct S_CARD_DRAG_INTERNAL
{
	pynote::core::domain::C_CARD_DRAG_SESSION_REGISTRY Registry;
	pynote::core::domain::C_CARD_DRAG_SESSION_REGISTRY::C_SOURCE_REGISTRATION Registration;
	CComPtr<IDropTarget> pDropTarget;
	HRESULT hrRegister{ E_FAIL };
	bool bSourceRegistered{ false };
	bool bDropRegistered{ false };
};

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
	: m_TimeZone(pynote::platform::MakeWin32SystemTimeZoneResolver()),
	  m_pDragInternal(std::make_unique<S_CARD_DRAG_INTERNAL>())
{
	m_sTimeFormatUtf8 = narrow(m_Display.sTimeFormat);
	m_sTimeZoneUtf8 = narrow(m_Display.sTimeZone);
	this->resolve_font_();
	this->capture_palette_();
	// 드래그 러너와 메뉴 실행기의 컨트롤 기본값은 **둘 다 비어 있다**. 진짜 ::DoDragDrop 과
	// 진짜 TrackPopupMenu 는 셸(CMain bind_card_list)이 건다 - 시험 프로세스는 D2D 스왑체인이
	// 만든 OLE 아파트를 갖게 되므로 기본값이 진짜 러너면 읽기 전용 S1~S3 시험이 실제 모달
	// 드래그 루프에 들어가 데스크톱 상태(물리 버튼·커서 위치)에 좌우된다(fix1, spec §3.4.6 형태).
}

C_CARD_LIST::~C_CARD_LIST()
{
	// 창이 살아 있는 채로 객체가 죽으면 ATL thunk 가 떠난 자리를 가리킨다.
	if (this->IsWindow()) { this->DestroyWindow(); }
	m_Target.Shutdown();
	// OLE 가 드롭 대상을 더 들고 있어도 죽은 컨트롤을 부르지 않게 끊는다.
	if (m_pDragInternal && m_pDragInternal->pDropTarget)
	{
		static_cast<C_CARD_DROP_TARGET*>(m_pDragInternal->pDropTarget.p)->Detach();
	}
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
	// 원본 등록이 살아 있어야 BeginSession 이 토큰을 준다. 이미 들고 있으면 다시 잡지
	// 않는다 - 이동 대입은 새 등록을 만든 뒤 옛 것을 지워 원본을 오히려 뺀다.
	if (m_pDragInternal && !m_pDragInternal->bSourceRegistered)
	{
		m_pDragInternal->Registration =
			m_pDragInternal->Registry.RegisterSource(this->source_identity_());
		m_pDragInternal->bSourceRegistered = true;
	}
	// Bind 는 Create 앞뒤 어느 쪽에서도 불린다 - 창이 이미 있으면 여기서 등록한다.
	this->ensure_drop_registration_();
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

void C_CARD_LIST::SetDragRunner(DragRunner _Runner)
{
	m_DragRunner = std::move(_Runner);
}

void C_CARD_LIST::SetContextMenuExecutor(MenuExecutor _Executor)
{
	m_MenuExecutor = std::move(_Executor);
}

void C_CARD_LIST::SetFrameCaptureHook(FrameCaptureHook _Hook)
{
	m_FrameCapture = std::move(_Hook);
}

void C_CARD_LIST::SetDragBodyProvider(DragBodyProvider _Provider)
{
	m_DragBody = std::move(_Provider);
}

void C_CARD_LIST::SetMoveCardHandler(MoveCardHandler _Handler)
{
	m_MoveCard = std::move(_Handler);
}

void C_CARD_LIST::SetDeleteDroppedHandler(DeleteDroppedHandler _Handler)
{
	m_DeleteDropped = std::move(_Handler);
}

void C_CARD_LIST::SetDragStartedHandler(DragStartedHandler _Handler)
{
	m_DragStarted = std::move(_Handler);
}

void C_CARD_LIST::SetDragFinishedHandler(DragFinishedHandler _Handler)
{
	m_DragFinished = std::move(_Handler);
}

domain::CardDragSourceIdentity C_CARD_LIST::source_identity_() const noexcept
{
	// 컨트롤 인스턴스 하나가 곧 하나의 드래그 원본이다(원본 event.source() is self).
	return(reinterpret_cast<domain::CardDragSourceIdentity>(this));
}

void C_CARD_LIST::ensure_drop_registration_()
{
	if (!m_pDragInternal || !this->IsWindow() || !m_pProjection) { return; }
	if (m_pDragInternal->bDropRegistered) { return; }
	if (!m_pDragInternal->pDropTarget)
	{
		m_pDragInternal->pDropTarget.Attach(new C_CARD_DROP_TARGET(
			[this](IDataObject* _pData, POINTL _Screen, bool _bDrop)
			{
				return(_bDrop ? this->handle_drop_(_pData, _Screen) :
					this->handle_drag_over_(_pData, _Screen));
			}));
	}
	m_pDragInternal->hrRegister = ::RegisterDragDrop(this->m_hWnd, m_pDragInternal->pDropTarget);
	// 실패는 치명적이 아니다 - 아파트 없는 프로세스에서는 늘 실패하고(실측 E_OUTOFMEMORY)
	// 컨트롤의 나머지는 그대로 돈다(spec §3.2.1). 원본이 로그를 남기는 세 자리는 이 뷰
	// 계층의 무로깅 관례를 따르며 각각 다른 방식으로 관측된다 - 여기(등록 결과)는 값을
	// 멤버로 남겨 접근자로 읽고, 러너 반환 실패 코드는 남기지도 알리지도 않으며(실행됨·
	// 취소 결과로만 관측된다), 정리의 종료 통지 예외는 삼킨다. S1~S3 도 같은 관례다.
	m_pDragInternal->bDropRegistered = SUCCEEDED(m_pDragInternal->hrRegister);
}

HRESULT C_CARD_LIST::DropRegistrationResult() const noexcept
{
	return(m_pDragInternal ? m_pDragInternal->hrRegister : E_FAIL);
}

bool C_CARD_LIST::HasDropRegistration() const noexcept
{
	return(m_pDragInternal && m_pDragInternal->bDropRegistered);
}

IDropTarget* C_CARD_LIST::DropTargetForTest() const noexcept
{
	return(m_pDragInternal ? m_pDragInternal->pDropTarget.p : nullptr);
}

std::optional<std::string> C_CARD_LIST::ActiveDragRevision(const std::string& _sCardId) const
{
	// 원본은 press 스냅샷에서 읽지만(card_stream.py:205~210) 네이티브 스냅샷은 드래그 시작
	// 시점의 캡처 변경으로 사라진다 - 값이 같은 세션 기록에서 읽는다(spec §3.3.5 선언 편차).
	if (!m_DragSession || m_DragSession->sCardId != _sCardId) { return(std::nullopt); }
	return(m_DragSession->sRevisionId);
}

std::optional<domain::CardDragSessionToken> C_CARD_LIST::ActiveDragToken() const
{
	if (!m_DragSession) { return(std::nullopt); }
	return(m_DragSession->nToken);
}

void C_CARD_LIST::ArmDeleteZone(domain::CardDragSessionToken _nToken)
{
	// 원본 zone.arm(token) + show() 다. 무장 토큰이 곧 표시 여부다.
	m_nArmedDeleteToken = _nToken;
	if (this->IsWindow()) { this->Invalidate(FALSE); }
}

void C_CARD_LIST::DisarmDeleteZone()
{
	m_nArmedDeleteToken.reset();
	if (this->IsWindow()) { this->Invalidate(FALSE); }
}

std::optional<domain::CardDragSessionToken> C_CARD_LIST::ArmedDeleteToken() const
{
	return(m_nArmedDeleteToken);
}

S_DIP_RECT C_CARD_LIST::DeleteZoneRectDip() const
{
	// CEILING: 드래그 중 자동 스크롤 없음 — Qt QAbstractItemView.autoScroll 등가 미이식(추적표 행 없음), 삭제 존 여백 단언의 근거만 기록
	// (원본 주석 document_page.py:958~959 는 좌우·하단 여백을 autoscroll edge 를 열어 두기
	// 위한 것으로 설명한다 - 여백 값은 이식되고 그 근거는 이식되지 않는다.)
	// 원본 _position_delete_drop_zone(document_page.py:953~967)의 축자 이식이다. 기준 사각은
	// list_pane 위젯 = 이 컨트롤의 "창" 사각이며 클라이언트가 아니다 - WS_EX_CLIENTEDGE
	// 테두리와 상시 세로 스크롤바가 파이썬 위젯 폭 안에 들어 있다(spec §3.3.1).
	if (!this->IsWindow()) { return(S_DIP_RECT{ 0, 0, 0, CARD_DELETE_ZONE_HEIGHT_DIP }); }
	RECT Window{};
	::GetWindowRect(this->m_hWnd, &Window);
	POINT ClientOrigin{ 0, 0 };
	::ClientToScreen(this->m_hWnd, &ClientOrigin);
	const int nPanelWidth = this->client_dip_(static_cast<int>(Window.right - Window.left));
	const int nPanelHeight = this->client_dip_(static_cast<int>(Window.bottom - Window.top));
	// max(0, ...) 클램프 2 건은 축자 이식 대상이다(부록 A A-4).
	const int nWidth = (std::max)(0,
		(std::min)(CARD_DELETE_ZONE_MAX_WIDTH_DIP, nPanelWidth - CARD_DELETE_ZONE_SIDE_RESERVE_DIP));
	const int nHeight = CARD_DELETE_ZONE_HEIGHT_DIP;
	const int nX = (std::max)(0, (nPanelWidth - nWidth) / 2);
	const int nY = (std::max)(0, nPanelHeight - nHeight - CARD_DELETE_ZONE_BOTTOM_GAP_DIP);
	// 창 좌표에서 잰 값을 클라이언트 좌표로 옮긴다(왼쪽·위 테두리 두께만큼).
	const int nInsetX = this->client_dip_(static_cast<int>(ClientOrigin.x - Window.left));
	const int nInsetY = this->client_dip_(static_cast<int>(ClientOrigin.y - Window.top));
	return(S_DIP_RECT{ nX - nInsetX, nY - nInsetY, nWidth, nHeight });
}

int C_CARD_LIST::ScrollLinesForWheel(int _nDelta) const
{
	UINT nLines = 3;
	if (!::SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &nLines, 0)) { nLines = 3; }
	const int nViewport = this->ViewportHeightDip();
	if (nViewport <= 0) { return(0); }
	// Qt QAbstractSliderPrivate::scrollByDelta 의 줄 갈래다 - singleStep 은 행 높이다.
	const double dDelta = static_cast<double>(nLines) *
		(-static_cast<double>(_nDelta) / 120.0) * static_cast<double>(this->RowHeightDip());
	// Qt 는 절사한 뒤 한 페이지(pageStep = 뷰포트 높이)로 묶는다. SPI 가 돌려주는
	// WHEEL_PAGESCROLL(0xFFFFFFFF)도 여기서 한 화면으로 접힌다(spec §3.1.12).
	const double dBounded = (std::clamp)(dDelta,
		-static_cast<double>(nViewport), static_cast<double>(nViewport));
	return(static_cast<int>(dBounded));
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
	if (this->IsWindow()) { this->update_scroll_bar_(); }
	// 무조건이다(원본 apply_time_display 의 무조건 dataChanged) - invalidate_all_rows_ 자체가
	// 로그(RowCount()>0)와 실제 다시 그리기(IsWindow())를 독립적으로 가른다(spec §3.4.4).
	this->invalidate_all_rows_({ E_CARD_INVALIDATION_ROLE::Tooltip });
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
	// 여섯 숨김 트리거 중 하나 - 리셋은 dataChanged 가 아니므로 로그에는 오르지 않는다
	// (spec §3.4.3/§3.4.4 다섯째 항목).
	if (m_TooltipHider) { m_TooltipHider(); }
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

bool C_CARD_LIST::roles_loggable_(const std::vector<E_CARD_INVALIDATION_ROLE>& _Roles) const noexcept
{
	// Hover·Selection 만으로 이루어진 역할 목록은 절대 로그에 오르지 않는다(spec §3.4.3) -
	// Hover 는 마우스 이동마다 나는 무한 증가 생산자이고 Selection 은 현재 발생원이 없다.
	for (const E_CARD_INVALIDATION_ROLE eRole : _Roles)
	{
		if (eRole != E_CARD_INVALIDATION_ROLE::Hover &&
			eRole != E_CARD_INVALIDATION_ROLE::Selection) { return(true); }
	}
	return(false);
}

void C_CARD_LIST::invalidate_row_(std::optional<std::size_t> _nRow,
	std::vector<E_CARD_INVALIDATION_ROLE> _Roles)
{
	if (!_nRow || !this->IsWindow()) { return; }
	// 로그 append 는 이 이른 반환을 지난 뒤에만, 그리고 역할이 loggable 할 때만 일어난다 -
	// 행이 실제로 무효화될 때만 기록한다(spec §3.4.3 첫 항목).
	if (this->roles_loggable_(_Roles))
	{
		m_InvalidationLog.push_back(S_CARD_INVALIDATION_ENTRY{ *_nRow, *_nRow, std::move(_Roles) });
	}
	const S_DIP_RECT Row = this->RowRectDip(*_nRow);
	const int nDpi = static_cast<int>((std::max<UINT>)(USER_DEFAULT_SCREEN_DPI, m_nDpi));
	RECT Invalid{
		0,
		::MulDiv(Row.nTop, nDpi, USER_DEFAULT_SCREEN_DPI),
		::MulDiv(Row.nLeft + Row.nWidth, nDpi, USER_DEFAULT_SCREEN_DPI),
		::MulDiv(Row.nTop + Row.nHeight, nDpi, USER_DEFAULT_SCREEN_DPI) };
	::InvalidateRect(this->m_hWnd, &Invalid, FALSE);
}

void C_CARD_LIST::invalidate_all_rows_(std::vector<E_CARD_INVALIDATION_ROLE> _Roles)
{
	// 두 조건은 독립이다 - 빈 모델도 창이 있으면 무조건 다시 그리지만(기존 SetDisplaySettings
	// 의 무조건 Invalidate(FALSE) 와 동일), 로그 항목만 행 수에 따라 남긴다(spec §3.4.3).
	const std::size_t nRows = this->RowCount();
	if (nRows > 0)
	{
		m_InvalidationLog.push_back(
			S_CARD_INVALIDATION_ENTRY{ 0, nRows - 1, std::move(_Roles) });
	}
	if (this->IsWindow()) { this->Invalidate(FALSE); }
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

void C_CARD_LIST::arm_hover_tracking_()
{
	TRACKMOUSEEVENT Track{};
	Track.cbSize = sizeof(Track);
	// TME_HOVER 는 1회성이다 - WM_MOUSEHOVER 가 뜨면 취소되고 TME_LEAVE 추적만 남는다.
	// OnMouseHover 가 매번 무조건 이 함수를 다시 불러 재무장한다(spec §3.2.5).
	// CEILING: Qt 의 되살아나기 창(마지막 툴팁 표시 뒤 2000 ms 안이면 즉시 재표시)과
	// 표시 지속 시간 자체는 이식하지 않았다 - HOVER_DEFAULT(시스템 hover 시간)만
	// 재현한다(spec §3.2.5의 declared deviation, canon [UNCERTAIN] 1)
	Track.dwFlags = TME_LEAVE | TME_HOVER;
	Track.hwndTrack = this->m_hWnd;
	Track.dwHoverTime = HOVER_DEFAULT;
	m_bTrackingMouse = ::TrackMouseEvent(&Track) != FALSE;
	++m_nHoverArmCount;
}

std::wstring C_CARD_LIST::source_label_(domain::E_CARD_SOURCE _eSource) const
{
	// S5 소유 상수 표다(card_model.py:477~488) - core 는 이 라벨을 모른다(spec §3.2.3).
	// 앞 5개만 콤보 항목을 갖고(§3.1.2), Split/Merge/System 은 툴팁 전용 라벨이다.
	switch (_eSource)
	{
	case domain::E_CARD_SOURCE::Typing: return(L"직접 입력");
	case domain::E_CARD_SOURCE::Paste: return(L"붙여넣기");
	case domain::E_CARD_SOURCE::Mixed: return(L"혼합");
	case domain::E_CARD_SOURCE::Import: return(L"가져오기");
	case domain::E_CARD_SOURCE::Restore: return(L"복구");
	case domain::E_CARD_SOURCE::Split: return(L"분할");
	case domain::E_CARD_SOURCE::Merge: return(L"병합");
	case domain::E_CARD_SOURCE::System: return(L"시스템");
	default: return(std::wstring{});
	}
}

void C_CARD_LIST::SetRevisionCounts(std::unordered_map<std::string, int> _Counts)
{
	// 조용하다 - 자체 무효화가 없고 뒤따르는 리셋(SetCards/OnProjectionChanged)에 실려
	// 간다(원본 set_cards(cards, revision_counts=...) 와 같은 접힘, spec §3.4.1/§3.4.2).
	m_RevisionCounts = std::move(_Counts);
}

void C_CARD_LIST::SetReconstructionUnavailableIds(std::set<std::string> _Ids)
{
	m_ReconstructionUnavailable = std::move(_Ids);
	// 무조건이다 - 집합이 이전과 바이트 단위로 같아도 다시 로그·무효화한다. 원본
	// set_reconstruction_unavailable_ids 에는 변경 없음 조기 반환이 없다(spec §3.4.4 첫 항목).
	this->invalidate_all_rows_({ E_CARD_INVALIDATION_ROLE::Reconstruction,
		E_CARD_INVALIDATION_ROLE::Tooltip, E_CARD_INVALIDATION_ROLE::SizeHint });
}

std::wstring C_CARD_LIST::TooltipTextForRow(std::size_t _nRow) const
{
	// 순수 조회다 - 절대 캐시하지 않는다(spec §3.2.4, canon M23: Qt 의 update_card 9역할
	// 목록에 ToolTipRole 이 없는데도 표시 시점에 재조회해 최신값을 보여 준다).
	if (!m_pProjection) { return(std::wstring{}); }
	const domain::S_CARD* pCard = m_pProjection->CardAt(_nRow);
	if (!pCard) { return(std::wstring{}); }
	const std::optional<std::size_t> nPosition = m_pProjection->PositionNumber(pCard->sId);
	const std::wstring sSource = this->source_label_(pCard->eSource);
	const std::wstring sCreated = this->TimeLabel(pCard->nCreatedAtUs);
	const std::wstring sUpdated = this->TimeLabel(pCard->nUpdatedAtUs);
	const auto ItRevision = m_RevisionCounts.find(pCard->sId);
	// 캐시에 없는 카드는 리비전 1(원본 .get(card.id, 1), card_model.py:141).
	const int nRevision = ItRevision != m_RevisionCounts.end() ? ItRevision->second : 1;
	const bool bUnavailable = m_ReconstructionUnavailable.count(pCard->sId) != 0;
	// 부록 A-6 축자 템플릿이다 - 구분자는 단일 \n, 첫 두 문장 조각은 공백으로 잇고,
	// 정상(6줄) 카드는 끝에 개행이 없다. "은" 조사는 받침과 무관하게 고정이다.
	std::wstring sResult;
	sResult += L"위치 " + std::to_wstring(nPosition ? *nPosition : 0) +
		L"은 문서 안의 현재 순서이며 현재 문서 순서 보기에서 이동할 수 있습니다.\n";
	sResult += L"기록 #" + std::to_wstring(pCard->nCaptureSeq) +
		L"은 최초 생성 순서이며 바뀌지 않습니다.\n";
	sResult += L"출처 " + sSource + L"\n";
	sResult += L"최초 기록 " + sCreated + L"\n";
	sResult += L"리비전 " + std::to_wstring(nRevision) + L"개\n";
	sResult += L"수정 " + sUpdated;
	if (bUnavailable) { sResult += L"\n형제 카드 purge로 작업 원문 재구성 불가"; }
	return(sResult);
}

void C_CARD_LIST::NotifyCardDirtyChanged(const std::string& _sCardId)
{
	// 호출부(페이지)가 IsCardDirty 값 변화를 이미 확인했다 - 여기서는 행이 아직 있는지만
	// 본다. 행이 없으면(필터에 가려짐) 로그도 재도장도 없다(spec §3.4.4).
	if (!m_pProjection) { return; }
	const std::optional<std::size_t> nRow = m_pProjection->RowForCard(_sCardId);
	if (!nRow) { return; }
	this->invalidate_row_(nRow, { E_CARD_INVALIDATION_ROLE::DirtyDraft });
}

void C_CARD_LIST::SetTooltipShower(TooltipShower _Shower)
{
	m_TooltipShower = std::move(_Shower);
}

void C_CARD_LIST::SetTooltipHider(TooltipHider _Hider)
{
	m_TooltipHider = std::move(_Hider);
}

std::string C_CARD_LIST::drag_body_(const std::string& _sCardId) const
{
	// 원본은 provider 가 없으면 모델의 BODY 를 읽는다(card_stream.py:456~460).
	if (m_DragBody) { return(m_DragBody(_sCardId)); }
	if (!m_pProjection) { return(std::string{}); }
	const std::optional<std::size_t> nRow = m_pProjection->RowForCard(_sCardId);
	const domain::S_CARD* pCard = nRow ? m_pProjection->CardAt(*nRow) : nullptr;
	return(pCard ? pCard->sBody : std::string{});
}

IDataObject* C_CARD_LIST::CreateDragDataObject(const std::string& _sCardId) const
{
	// 원본 CardListModel.mimeData((index,)) 다 - 행이 없으면 빈 QMimeData 라 여기서는 nullptr 다.
	if (!m_pProjection) { return(nullptr); }
	const std::optional<std::size_t> nRow = m_pProjection->RowForCard(_sCardId);
	const domain::S_CARD* pCard = nRow ? m_pProjection->CardAt(*nRow) : nullptr;
	if (!pCard) { return(nullptr); }
	const std::string sJson =
		build_card_payload(pCard->sId, pCard->sCurrentRevisionId, m_nDragPayloadToken);
	// 본문은 payload 에 실린 화면 본문이 있으면 그것, 없으면 확정 본문이다(card_model.py:222).
	const std::string sBody = m_sDragPayloadBody ? *m_sDragPayloadBody : pCard->sBody;
	// OLE 층의 줄바꿈은 CRLF 다(spec §3.1.3b) - QMimeData 층의 LF 와 갈리는 자리다.
	return(static_cast<IDataObject*>(
		new C_CARD_DATA_OBJECT(this->source_identity_(), sJson, to_crlf(wide(sBody)))));
}

void C_CARD_LIST::begin_drag_()
{
	// CEILING: 관리자 권한(UIPI) — 승격 프로세스에서 비승격 외부 창으로의 실사용 드래그는 차단된다
	// (NoteEx.vcxproj ReleaseMD|x64 와 NoteExTests.vcxproj 가 둘 다 RequireAdministrator 다 -
	// 프로세스 안 드롭은 그대로 성립하고 실제 외부 드래그만 막힌다.)
	// 원본 startDrag(card_stream.py:439~488)의 순서를 그대로 돈다.
	if (!m_pProjection || !m_DragSnapshot || !m_pDragInternal) { return; }
	const S_CARD_DRAG_SNAPSHOT Snapshot = *m_DragSnapshot;
	// 0) 스냅샷의 카드가 아직 목록에 있어야 한다. 없으면 스냅샷만 지우고 토큰도 신호도
	//    press 소비도 없이 끝난다(다른 창이 그 사이에 지운 경우가 여기로 온다).
	if (!m_pProjection->RowForCard(Snapshot.sCardId))
	{
		m_DragSnapshot.reset();
		return;
	}
	// 1) 토큰 발급. 등록이 살아 있지 않으면 아무 상태도 게시하지 않는다(원본에는 없는 방어
	//    갈래이며 등록 결함이 드래그를 반쯤 시작시키지 못하게 한다).
	const std::optional<domain::CardDragSessionToken> nToken =
		m_pDragInternal->Registry.BeginSession(
			this->source_identity_(), Snapshot.sCardId, Snapshot.sRevisionId);
	if (!nToken) { return; }
	// 2) 세션 게시.
	m_DragSession = S_CARD_DRAG_SESSION{ *nToken, Snapshot.sCardId, Snapshot.sRevisionId, false };
	// 3) press 소비.
	m_bDragConsumedPress = true;
	// 드래그가 도는 동안 정숙 열기 타이머가 떨어지면 안 된다(spec §3.7.3).
	this->CancelPendingBrowse();
	// 4) 드래그 시작 통지(원본 drag_started).
	if (m_DragStarted) { m_DragStarted(Snapshot.sCardId, *nToken); }
	// 5) 본문 -> payload -> 데이터 개체 -> 러너.
	std::exception_ptr Pending;
	try
	{
		m_nDragPayloadToken = *nToken;
		m_sDragPayloadBody = this->drag_body_(Snapshot.sCardId);
		CComPtr<IDataObject> pData;
		pData.Attach(this->CreateDragDataObject(Snapshot.sCardId));
		CComPtr<IDropSource> pSource;
		pSource.Attach(static_cast<IDropSource*>(new C_CARD_DROP_SOURCE()));
		// 원본 drag.exec(Copy|Move, Copy) 의 두 번째 인자가 "제안 동작" 이다 - 러너에 들어가는
		// pEffect 초기값이 그 자리이며, 협상 결과는 읽지 않는다(원본도 반환값을 버린다).
		DWORD nEffect = DROPEFFECT_COPY;
		// 러너가 없으면 ::DoDragDrop 을 부르지 않고 "실행 뒤 취소" 로 끝낸다(fix1) - press
		// 소비는 남고 정리와 종료 통지는 그대로 돈다. 반환값을 읽지 않으므로 이 상수는
		// 아래 "실행" 판정의 근거를 문서로 남기는 자리다.
		const HRESULT hrRun = m_DragRunner ?
			m_DragRunner(pData, pSource, DROPEFFECT_COPY | DROPEFFECT_MOVE, &nEffect) :
			DRAGDROP_S_CANCEL;
		// 반환된 HRESULT 는 실패 코드까지 전부 "실행" 이다(spec §3.1.8) - 예외만 되돌린다.
		if (m_DragSession) { m_DragSession->bExecuted = true; }
		(void)hrRun;
	}
	catch (...) { Pending = std::current_exception(); }
	// 원본 finally 와 같은 자리다 - 정리가 먼저이고 예외는 그 뒤에 전파한다.
	this->finish_drag_session_();
	if (Pending) { std::rethrow_exception(Pending); }
}

void C_CARD_LIST::finish_drag_session_()
{
	// 멱등이다 - 세션이 없으면 즉시 돌아가고 아무 신호도 내지 않는다. 러너 안에서 창이
	// 죽으면 OnDestroy 와 러너 반환 뒤가 둘 다 이 함수를 부른다(spec §3.1.7).
	if (!m_DragSession) { return; }
	const S_CARD_DRAG_SESSION Session = *m_DragSession;
	m_DragSession.reset();
	// 1) payload 정리(토큰 0, 본문 없음).
	m_nDragPayloadToken = 0;
	m_sDragPayloadBody.reset();
	// 2) 활성 토큰·스냅샷 정리. 실행되지 않았으면 press 소비를 되돌린다.
	if (m_pDragInternal) { m_pDragInternal->Registry.EndSession(Session.nToken); }
	m_DragSnapshot.reset();
	if (!Session.bExecuted) { m_bDragConsumedPress = false; }
	// 3) 종료 통지. 원본은 세 블록을 각각 감싸 예외를 삼키고 기록한다 - 여기서 밖으로
	//    나갈 수 있는 것은 소비자 콜백뿐이라 그 자리만 감싼다.
	if (m_DragFinished)
	{
		try { m_DragFinished(Session.nToken); }
		catch (...) { /* 종료 통지 실패는 드래그 정리를 막지 않는다(원본 로깅 자리) */ }
	}
}

bool C_CARD_LIST::zone_hit_(POINT _PointDip) const
{
	if (!m_nArmedDeleteToken) { return(false); }
	const S_DIP_RECT Zone = this->DeleteZoneRectDip();
	if (Zone.nWidth <= 0 || Zone.nHeight <= 0) { return(false); }
	// S_DIP_RECT 는 QRect 와 같은 닫힌 구간이다.
	return(_PointDip.x >= Zone.nLeft && _PointDip.x <= Zone.Right() &&
		_PointDip.y >= Zone.nTop && _PointDip.y <= Zone.Bottom());
}

POINT C_CARD_LIST::client_point_from_screen_(POINTL _Screen) const
{
	POINT Point{ _Screen.x, _Screen.y };
	if (this->IsWindow()) { ::ScreenToClient(this->m_hWnd, &Point); }
	return(POINT{ this->client_dip_(Point.x), this->client_dip_(Point.y) });
}

std::optional<std::string> C_CARD_LIST::drop_before_card_(POINT _PointDip) const
{
	// 원본 _drop_before_card_id(card_stream.py:548~560). 판정만 옮기고 그림은 그리지 않는다.
	// CEILING: 드롭 인디케이터 선은 그리지 않는다 — Qt 스타일 페인트의 등가는 추적표 행이 없어 W4 aggregate 재판정
	// (원본 모델의 flags() 에 ItemIsDropEnabled 가 없어 실측 인디케이터는 늘 OnItem 이고
	// 위치에 따른 픽셀 변화가 없다 - 판정만 이식하면 관측이 같다, spec §3.2.5).
	if (!m_pProjection) { return(std::nullopt); }
	const std::optional<std::size_t> nRow = this->row_at_point_(_PointDip);
	// 무효 index = 문서 끝으로 이동이다(마지막 행 아래 빈 영역·뷰포트 하단).
	if (!nRow) { return(std::nullopt); }
	const S_DIP_RECT Row = this->RowRectDip(*nRow);
	// QRect::center().y() = top + (height - 1) / 2 다 - y == 중앙은 이미 아래쪽 반이다.
	const int nCentreY = Row.nTop + (Row.nHeight - 1) / 2;
	std::size_t nTarget = *nRow;
	if (_PointDip.y >= nCentreY) { ++nTarget; }
	if (nTarget >= this->RowCount()) { return(std::nullopt); }
	const domain::S_CARD* pCard = m_pProjection->CardAt(nTarget);
	if (!pCard) { return(std::nullopt); }
	return(pCard->sId);
}

bool C_CARD_LIST::accepts_session_payload_(IDataObject* _pData) const
{
	// 원본 _accepts_internal_drag(card_stream.py:562~577)의 2~6 번 요소다. 3~6 은 core
	// Validate 가 소유하고(토큰·원본·카드·리비전) 여기서는 구문 해석과 원본 동일성을 본다.
	if (!m_pDragInternal || !m_DragSession) { return(false); }
	if (source_identity_of(_pData) != this->source_identity_()) { return(false); }
	const std::optional<std::string> sJson = read_global_bytes(_pData, card_mime_format());
	if (!sJson) { return(false); }
	const std::optional<S_CARD_MIME_PAYLOAD> Payload = parse_card_payload(*sJson);
	if (!Payload) { return(false); }
	if (Payload->nToken != m_DragSession->nToken) { return(false); }
	return(m_pDragInternal->Registry.Validate(Payload->nToken, this->source_identity_(),
		Payload->sCardId, Payload->sRevisionId));
}

bool C_CARD_LIST::accepts_row_drag_(IDataObject* _pData) const
{
	// 1) 내부 재정렬은 position 정렬에서만 가능하다(core 가 소유한 술어다).
	if (!m_pProjection || !m_pProjection->CanInternalReorder()) { return(false); }
	return(this->accepts_session_payload_(_pData));
}

bool C_CARD_LIST::accepts_zone_drag_(IDataObject* _pData) const
{
	// 원본 CardDeleteDropZone._accepts(card_stream.py:653~667). 정렬 모드를 보지 않으므로
	// 휴지통 드래그는 모든 정렬에서 된다(부록 A [RECENCY-DROP]). 무장 토큰과 원본의 살아
	// 있는 세션 토큰이 함께 맞아야 하므로 무장만 남은 상태로는 절대 수락되지 않는다.
	if (!m_nArmedDeleteToken || !m_DragSession) { return(false); }
	if (m_DragSession->nToken != *m_nArmedDeleteToken) { return(false); }
	const std::optional<std::string> sJson = read_global_bytes(_pData, card_mime_format());
	if (!sJson) { return(false); }
	const std::optional<S_CARD_MIME_PAYLOAD> Payload = parse_card_payload(*sJson);
	if (!Payload || Payload->nToken != *m_nArmedDeleteToken) { return(false); }
	return(this->accepts_session_payload_(_pData));
}

DWORD C_CARD_LIST::handle_drag_over_(IDataObject* _pData, POINTL _Screen) const
{
	const POINT Point = this->client_point_from_screen_(_Screen);
	// 오버레이 위에서는 행 반쪽 판정도 행 수락 규칙도 돌지 않는다 - 원본에서 목록이
	// dragMove 를 아예 받지 못하는 상태의 쌍둥이다(spec §3.3.1).
	if (this->zone_hit_(Point))
	{
		return(this->accepts_zone_drag_(_pData) ? DROPEFFECT_MOVE : DROPEFFECT_NONE);
	}
	return(this->accepts_row_drag_(_pData) ? DROPEFFECT_MOVE : DROPEFFECT_NONE);
}

DWORD C_CARD_LIST::handle_drop_(IDataObject* _pData, POINTL _Screen)
{
	const POINT Point = this->client_point_from_screen_(_Screen);
	if (this->zone_hit_(Point))
	{
		if (!this->accepts_zone_drag_(_pData)) { return(DROPEFFECT_NONE); }
		const std::string sCardId = m_DragSession->sCardId;
		if (m_DeleteDropped) { m_DeleteDropped(sCardId); }
		return(DROPEFFECT_MOVE);
	}
	// 원본 dropEvent(card_stream.py:393~428)의 순서다 - enter 경로와 다른 차례로 다시 본다.
	// 1) 정렬 모드가 먼저이고, 2)3) payload·원본·세션·토큰·카드/리비전이 그 다음이다.
	// 원본 4)(세션 기록이 사라졌을 때의 선택 첫 항목 폴백)에 대응하는 갈래는 두지 않는다 -
	// §3.1.7 이 토큰과 스냅샷을 한 세션 기록으로 합치므로 "토큰은 살아 있고 기록만 없다"
	// 라는 상태가 네이티브에서는 구조적으로 성립하지 않는다(spec §3.2.3 선언 편차).
	if (!m_pProjection || !m_pProjection->CanInternalReorder()) { return(DROPEFFECT_NONE); }
	if (!this->accepts_session_payload_(_pData)) { return(DROPEFFECT_NONE); }
	const std::string sCardId = m_DragSession->sCardId;
	const std::optional<std::string> sBefore = this->drop_before_card_(Point);
	// 5) 자기 앞으로의 드롭은 드롭 시점에만 거절한다(부록 A self_upper = 110).
	if (sBefore && *sBefore == sCardId) { return(DROPEFFECT_NONE); }
	if (m_MoveCard) { m_MoveCard(sCardId, sBefore); }
	return(DROPEFFECT_MOVE);
}

void C_CARD_LIST::draw_delete_zone_(ID2D1DeviceContext* _pDc)
{
	if (!m_Frame.bDeleteZoneVisible || !m_pBrushCache || !_pDc) { return; }
	const S_DIP_RECT Zone = m_Frame.DeleteZoneRect;
	if (Zone.nWidth <= 0 || Zone.nHeight <= 0) { return; }
	const D2D1_RECT_F Area = D2D1::RectF(static_cast<float>(Zone.nLeft),
		static_cast<float>(Zone.nTop), static_cast<float>(Zone.nLeft + Zone.nWidth),
		static_cast<float>(Zone.nTop + Zone.nHeight));
	_pDc->FillRoundedRectangle(
		D2D1::RoundedRect(Area, CARD_DELETE_ZONE_CORNER_RADIUS_DIP, CARD_DELETE_ZONE_CORNER_RADIUS_DIP),
		m_pBrushCache->GetBrush(DELETE_ZONE_FILL_COLOR));
	// 1 DIP 선은 경계 위에 중심이 놓이므로 0.5 안으로 당긴다(카드 테두리와 같은 규칙).
	_pDc->DrawRoundedRectangle(
		D2D1::RoundedRect(D2D1::RectF(Area.left + 0.5f, Area.top + 0.5f,
			Area.right - 0.5f, Area.bottom - 0.5f),
			CARD_DELETE_ZONE_CORNER_RADIUS_DIP, CARD_DELETE_ZONE_CORNER_RADIUS_DIP),
		m_pBrushCache->GetBrush(DELETE_ZONE_BORDER_COLOR));
	// 원본 QLabel: AlignCenter, 사방 여백 8, 흰 글자(card_stream.py:608~613).
	IDWriteTextFormat* pFormat = this->text_format_();
	if (!pFormat || !m_pText) { return; }
	const int nLabelWidth = Zone.nWidth - 2 * CARD_DELETE_ZONE_LABEL_MARGIN_DIP;
	const int nLabelHeight = Zone.nHeight - 2 * CARD_DELETE_ZONE_LABEL_MARGIN_DIP;
	if (nLabelWidth <= 0 || nLabelHeight <= 0) { return; }
	d2d::C_D2D_TEXT_LAYOUT Layout = m_pText->CreateLayout(DELETE_ZONE_LABEL_TEXT, pFormat,
		static_cast<float>(nLabelWidth), static_cast<float>(nLabelHeight), 0);
	if (!Layout.IsValid()) { return; }
	if (IDWriteTextLayout* pLayout = Layout.Get())
	{
		pLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
		pLayout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	}
	Layout.Draw(_pDc, m_pBrushCache->GetBrush(DELETE_ZONE_LABEL_COLOR),
		D2D1::Point2F(static_cast<float>(Zone.nLeft + CARD_DELETE_ZONE_LABEL_MARGIN_DIP),
			static_cast<float>(Zone.nTop + CARD_DELETE_ZONE_LABEL_MARGIN_DIP)));
}

bool C_CARD_LIST::copy_body_to_clipboard_(const std::string& _sBody) const
{
	// 원본 QApplication.clipboard().setText(body) 다. OleSetClipboard 가 아니라 raw Win32 를
	// 쓰는 것은 의도한 선택이다 - 아파트 없는 프로세스에서 OLE 판이 실패하기 때문이다.
	const std::wstring sText = to_crlf(wide(_sBody));
	for (int nAttempt = 0; nAttempt < CLIPBOARD_ATTEMPTS; ++nAttempt)
	{
		if (::OpenClipboard(this->IsWindow() ? this->m_hWnd : nullptr))
		{
			bool bOk = ::EmptyClipboard() != FALSE;
			if (bOk)
			{
				const SIZE_T nBytes = (sText.size() + 1) * sizeof(wchar_t);
				HGLOBAL hText = ::GlobalAlloc(GMEM_MOVEABLE, nBytes);
				bOk = hText != nullptr;
				if (bOk)
				{
					void* pText = ::GlobalLock(hText);
					bOk = pText != nullptr;
					if (bOk)
					{
						std::memcpy(pText, sText.c_str(), nBytes);
						::GlobalUnlock(hText);
						bOk = ::SetClipboardData(CF_UNICODETEXT, hText) == hText;
					}
					// 클립보드가 소유권을 가져가지 못했으면 우리가 되돌린다.
					if (!bOk) { ::GlobalFree(hText); }
				}
			}
			::CloseClipboard();
			if (bOk) { return(true); }
		}
		::Sleep(CLIPBOARD_RETRY_MS);
	}
	return(false);
}

void C_CARD_LIST::show_context_menu_(POINT _ClientDip, POINT _Screen)
{
	if (!m_pProjection) { return; }
	const std::optional<std::size_t> nRow = this->row_at_point_(_ClientDip);
	const domain::S_CARD* pCard = nRow ? m_pProjection->CardAt(*nRow) : nullptr;
	// 원본 _build_context_menu 는 무효 index 에 None 을 돌린다 - 메뉴도 선택 변화도 없다.
	if (!pCard) { return; }
	const std::string sCardId = pCard->sId;
	const std::string sBody = pCard->sBody;
	// 원본 _select_context_index: 미선택 행이면 그 행만 남기고, 선택된 행이면 선택을 유지한
	// 채 현재만 옮긴다. 우버튼 press 가 이미 한 것과 같은 조작이라 멱등이고, 키보드 컨텍스트
	// 키 경로에서는 여기서만 돈다(원본도 press 와 메뉴 구성에서 두 번 부른다).
	if (!this->IsRowSelected(*nRow))
	{
		m_pProjection->SelectVisibleCard(sCardId, domain::E_CARD_SELECTION_INTENT::Replace);
	}
	m_pProjection->SetCurrentCardId(sCardId);
	this->set_anchor_(nRow);
	m_ShiftBase = m_pProjection->SelectedCardIds();
	this->observe_current_();
	if (this->IsWindow()) { this->Invalidate(FALSE); }
	// 대상은 메뉴를 만들 때 확정된다 - 원본 람다가 build 시점 값을 붙잡는다(:505~526).
	const std::vector<std::string> Selected = m_pProjection->SelectedCardIds();
	HMENU hMenu = ::CreatePopupMenu();
	if (!hMenu) { return; }
	// 항목은 정확히 넷·고정 순서·전부 활성이며 선택 장수에 따라 달라지지 않는다.
	::AppendMenuW(hMenu, MF_STRING, CARD_MENU_OPEN, L"편집기에서 열기");
	::AppendMenuW(hMenu, MF_STRING, CARD_MENU_COPY, L"본문 복사");
	::AppendMenuW(hMenu, MF_STRING, CARD_MENU_EXPORT, L"파일로 내보내기");
	::AppendMenuW(hMenu, MF_STRING, CARD_MENU_DELETE, L"닫기");
	const UINT nCommand = m_MenuExecutor ? m_MenuExecutor(hMenu, _Screen) : 0;
	::DestroyMenu(hMenu);
	switch (nCommand)
	{
	case CARD_MENU_OPEN:
		// 여는 카드는 선택 전체가 아니라 누른 카드 한 장이다.
		if (m_OpenCard) { m_OpenCard(sCardId); }
		break;
	case CARD_MENU_COPY:
		// 확정 본문이다 - 더티 편집 버퍼가 아니다(드래그 규칙과 반대다, spec §3.4.5).
		this->copy_body_to_clipboard_(sBody);
		break;
	case CARD_MENU_EXPORT:
		// CEILING: 파일로 내보내기 항목은 활성이되 미배선 - 실행(저장 대화상자·순서·UTF-8
		// 출력)은 W7 CAP-FI-067 이다.
		break;
	case CARD_MENU_DELETE:
		if (m_Delete) { m_Delete(Selected); }
		break;
	default:
		break;
	}
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
	// 오버레이 기하는 그리기 성공 여부와 무관하게 싣는다 - 0x0 클라이언트처럼 타깃을
	// 세울 수 없는 상태에서도 클램프 결과를 읽을 수 있어야 한다(부록 A A-4 마지막 행).
	m_Frame.bDeleteZoneVisible = m_nArmedDeleteToken.has_value();
	m_Frame.DeleteZoneRect = this->DeleteZoneRectDip();
	m_Frame.nDeleteZoneColor = DELETE_ZONE_FILL_COLOR;
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

	// 삭제 오버레이는 행 뒤에 그린다(원본에서도 목록 위에 얹힌 별도 위젯이다).
	this->draw_delete_zone_(pDc);
	// 캡처 훅은 EndDraw 앞이어야 한다 - EndDraw 가 present 하고 스왑체인이
	// FLIP_DISCARD 라 그 뒤 백버퍼 내용은 정의되지 않는다(spec §3.3.8).
	if (m_FrameCapture) { m_FrameCapture(pDc); }
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
	// TME_HOVER 는 arm_hover_tracking_ 안으로 옮겨졌다(S5) - 여기서는 기존 그대로
	// !m_bTrackingMouse 가드로만 첫 진입에서 무장한다(spec §3.2.5).
	if (!m_bTrackingMouse) { this->arm_hover_tracking_(); }
	const POINT Point = this->point_from_lparam_(_lParam);
	const std::optional<std::size_t> nRow = this->row_at_dip_(Point.y);
	if (nRow != m_nHoverRow)
	{
		this->invalidate_row_(m_nHoverRow, { E_CARD_INVALIDATION_ROLE::Hover });
		m_nHoverRow = nRow;
		this->invalidate_row_(m_nHoverRow, { E_CARD_INVALIDATION_ROLE::Hover });
		// 호버 행이 바뀌면(사라짐 포함) 켜져 있던 툴팁을 숨긴다 - 여섯 숨김 트리거 중 하나
		// (spec §3.2.6).
		if (m_TooltipHider) { m_TooltipHider(); }
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
			m_PressedRow.reset();
			m_bPressedAlreadySelected = false;
			m_eViewState = E_CARD_LIST_VIEW_STATE::NoState;
			// S4: 여기가 Qt 가 startDrag 를 부르는 자리다. press 소비 표식은 원본과 같이
			// startDrag 안에서만 선다 - 시작 가드에 걸리면 원본처럼 아무 일도 없다(spec §3.1.5).
			this->begin_drag_();
		}
	}
	return(0);
}

LRESULT C_CARD_LIST::OnMouseHover(UINT, WPARAM, LPARAM _lParam, BOOL&)
{
	// 조립은 여기서만 한다 - OnMouseMove 는 픽셀마다 돌므로 문자열을 만들지 않는다
	// (spec §3.2.5 투자 위험 8). 샤워가 설치돼 있지 않으면 조립도 하지 않는다.
	if (m_nHoverRow && m_TooltipShower)
	{
		POINT ScreenPoint{ GET_X_LPARAM(_lParam), GET_Y_LPARAM(_lParam) };
		::ClientToScreen(this->m_hWnd, &ScreenPoint);
		m_TooltipShower(*m_nHoverRow, this->TooltipTextForRow(*m_nHoverRow), ScreenPoint);
	}
	// TME_HOVER 는 1회성이라 무조건 재무장한다 - 안 하면 마우스 진입당 한 번만 뜬다
	// (spec §3.2.5, BLOCKER-1 의 자기 회귀 가드).
	this->arm_hover_tracking_();
	return(0);
}

LRESULT C_CARD_LIST::OnLButtonDown(UINT, WPARAM _wParam, LPARAM _lParam, BOOL&)
{
	// 원본 mousePressEvent(:248)는 어떤 버튼이든 기록 앞에서 먼저 취소한다.
	this->CancelPendingBrowse();
	// 여섯 숨김 트리거 중 하나 - 어떤 마우스 press 든 켜져 있던 툴팁을 숨긴다(spec §3.2.6).
	if (m_TooltipHider) { m_TooltipHider(); }
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
	// 여섯 숨김 트리거 중 하나(spec §3.2.6).
	if (m_TooltipHider) { m_TooltipHider(); }
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

LRESULT C_CARD_LIST::OnRButtonUp(UINT, WPARAM, LPARAM, BOOL& _bHandled)
{
	// 더는 삼키지 않는다 - DefWindowProc 이 WM_CONTEXTMENU 를 합성하고 그것이 원본
	// CustomContextMenu 정책의 관측 쌍둥이다. 키보드 컨텍스트 키도 같이 얻는다(spec §3.4.1).
	_bHandled = FALSE;
	return(0);
}

LRESULT C_CARD_LIST::OnContextMenu(UINT, WPARAM, LPARAM _lParam, BOOL&)
{
	POINT Screen{ GET_X_LPARAM(_lParam), GET_Y_LPARAM(_lParam) };
	POINT ClientDip{};
	if (Screen.x == -1 && Screen.y == -1)
	{
		// 키보드 컨텍스트 키(VK_APPS·Shift+F10). 현재 행 사각의 중앙에서 연다 -
		// Win32 관례이며 원본은 이 경로를 측정한 적이 없다(지휘 판단, 미실측).
		const std::optional<std::size_t> nRow = this->current_row_();
		if (!nRow) { return(0); }
		const S_DIP_RECT Row = this->RowRectDip(*nRow);
		ClientDip = POINT{ Row.nLeft + Row.nWidth / 2, Row.nTop + Row.nHeight / 2 };
		const int nDpi = static_cast<int>((std::max<UINT>)(USER_DEFAULT_SCREEN_DPI, m_nDpi));
		Screen = POINT{ ::MulDiv(ClientDip.x, nDpi, USER_DEFAULT_SCREEN_DPI),
			::MulDiv(ClientDip.y, nDpi, USER_DEFAULT_SCREEN_DPI) };
		if (this->IsWindow()) { ::ClientToScreen(this->m_hWnd, &Screen); }
	}
	else
	{
		POINT Client = Screen;
		if (this->IsWindow()) { ::ScreenToClient(this->m_hWnd, &Client); }
		ClientDip = POINT{ this->client_dip_(Client.x), this->client_dip_(Client.y) };
	}
	this->show_context_menu_(ClientDip, Screen);
	return(0);
}

LRESULT C_CARD_LIST::OnCaptureChanged(UINT, WPARAM, LPARAM, BOOL& _bHandled)
{
	// 드래그가 도는 동안 DoDragDrop 이 자기 캡처를 잡아 이 메시지가 온다. 세션은 press
	// 상태가 아니라 남기고, press 소비 표식도 세션이 살아 있는 동안은 세션 상태로 다룬다 -
	// 지우면 이어지는 릴리스가 카드를 열어 원본과 갈린다(spec §3.1.7).
	const bool bSessionLive = m_DragSession.has_value();
	const bool bConsumedPress = m_bDragConsumedPress;
	this->reset_press_state_();
	if (bSessionLive) { m_bDragConsumedPress = bConsumedPress; }
	_bHandled = FALSE;
	return(0);
}

LRESULT C_CARD_LIST::OnMouseWheel(UINT, WPARAM _wParam, LPARAM, BOOL& _bHandled)
{
	// 여섯 숨김 트리거 중 하나 - 소비 여부와 무관하게 휠은 툴팁을 숨긴다(spec §3.2.6).
	if (m_TooltipHider) { m_TooltipHider(); }
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
	if (m_DragSession)
	{
		// 원본 wheelEvent 의 네 번째 이접지(_active_drag_token is not None)다 - 수식키·0 각·
		// 빈 목록 뒤, 탐색 앞이다. super().wheelEvent 가 Qt 줄 스크롤로 흘린다(spec §3.1.12).
		// 탐색 기계는 아예 돌지 않으므로 현재 행·대기 카드·잔여 각이 그대로 남는다.
		const int nBeforeOffset = m_nScrollOffsetDip;
		this->ScrollToPixel(m_nScrollOffsetDip + this->ScrollLinesForWheel(nDelta));
		// 값이 바뀌지 않으면 Qt 는 수락하지 않는다(S3 §3.1.3.2 와 같은 규칙).
		if (m_nScrollOffsetDip == nBeforeOffset) { _bHandled = FALSE; }
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
	// 여섯 숨김 트리거 중 하나 - "어떤 마우스 press 든"(spec §3.2.6).
	if (m_TooltipHider) { m_TooltipHider(); }
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
	this->invalidate_row_(nRow, { E_CARD_INVALIDATION_ROLE::Hover });
	// 여섯 숨김 트리거 중 하나(spec §3.2.6).
	if (m_TooltipHider) { m_TooltipHider(); }
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

LRESULT C_CARD_LIST::OnCreate(UINT, WPARAM, LPARAM, BOOL& _bHandled)
{
	// 창이 서면 드롭 대상을 등록한다(프로젝션은 Bind 가 이미 걸어 두었다). 실패는 치명적이
	// 아니며 기본 상태의 시험 프로세스에서는 늘 실패한다(spec §3.2.1).
	this->ensure_drop_registration_();
	_bHandled = FALSE;
	return(0);
}

LRESULT C_CARD_LIST::OnGetObject(UINT, WPARAM _wParam, LPARAM _lParam, BOOL& _bHandled)
{
	if (_lParam != static_cast<LPARAM>(UiaRootObjectId))
	{
		_bHandled = FALSE;
		return(0);
	}
	// 지연 생성 - 첫 요청에서만 만들고 이후 모든 제공자 개체가 이 하나를 공유한다.
	if (!m_pUiaLiveness) { m_pUiaLiveness = std::make_shared<std::atomic<C_CARD_LIST*>>(this); }
	CComPtr<IRawElementProviderSimple> pProvider;
	pProvider.Attach(new C_CARD_LIST_UIA_PROVIDER(m_pUiaLiveness));
	return(::UiaReturnRawElementProvider(this->m_hWnd, _wParam, _lParam, pProvider));
}

LRESULT C_CARD_LIST::OnDestroy(UINT, WPARAM, LPARAM, BOOL& _bHandled)
{
	// RevokeDragDrop 이 가장 먼저다 - 창이 사라진 뒤 OLE 가 대상을 부르지 않게 한다.
	if (m_pDragInternal && m_pDragInternal->bDropRegistered)
	{
		::RevokeDragDrop(this->m_hWnd);
		m_pDragInternal->bDropRegistered = false;
	}
	// UIA 클라이언트가 창 파괴 뒤에도 들고 있을 수 있는 참조를 무해화한다 - RevokeDragDrop
	// 바로 다음 줄이다(교체가 아니라 추가, spec §3.3.3).
	if (m_pUiaLiveness)
	{
		::UiaReturnRawElementProvider(this->m_hWnd, 0, 0, nullptr);
		m_pUiaLiveness->store(nullptr);
	}
	if (m_TooltipHider) { m_TooltipHider(); }
	// 살아 있는 세션은 여기서 무효가 된다 - 토큰을 걷고 종료를 정확히 한 번 알린다.
	this->finish_drag_session_();
	// 세션 없이 무장만 남은 오버레이(직접 Arm 한 경우)도 여기서 걷는다.
	m_nArmedDeleteToken.reset();
	if (m_pDragInternal)
	{
		m_pDragInternal->Registration.Reset();
		m_pDragInternal->bSourceRegistered = false;
	}
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
