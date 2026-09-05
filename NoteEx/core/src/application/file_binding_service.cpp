#include "pynote/core/application/file_binding_service.h"

#include "utf_codec_detail.h"

#include <chrono>
#include <cstddef>
#include <iterator>
#include <utility>

namespace pynote::core::application
{
	namespace
	{
		using detail::append_utf8;
		using detail::decode_one_utf8;
		using detail::strict_utf8;

		const std::uint8_t BOM_UTF8[]     = { 0xEF, 0xBB, 0xBF };
		const std::uint8_t BOM_UTF16_LE[] = { 0xFF, 0xFE };
		const std::uint8_t BOM_UTF16_BE[] = { 0xFE, 0xFF };

		// 원본 _default_clock(:324~325) 이다. time.time_ns() // 1_000 과 같은 값이다.
		std::int64_t default_clock()
		{
			const auto Since = std::chrono::system_clock::now().time_since_epoch();
			return(std::chrono::duration_cast<std::chrono::microseconds>(Since).count());
		}

		std::span<const std::uint8_t> as_bytes(std::string_view _sText)
		{
			return(std::span<const std::uint8_t>(
				reinterpret_cast<const std::uint8_t*>(_sText.data()), _sText.size()));
		}

		std::string as_text(std::span<const std::uint8_t> _Bytes)
		{
			return(std::string(reinterpret_cast<const char*>(_Bytes.data()), _Bytes.size()));
		}

		bool starts_with(std::span<const std::uint8_t> _Bytes, const std::uint8_t* _pPrefix, std::size_t _nSize)
		{
			if (_Bytes.size() < _nSize) { return(false); }
			for (std::size_t i = 0; i < _nSize; ++i)
			{
				if (_Bytes[i] != _pPrefix[i]) { return(false); }
			}
			return(true);
		}

		std::uint16_t utf16_unit(std::span<const std::uint8_t> _Bytes, std::size_t _nOffset, bool _bBigEndian)
		{
			return(static_cast<std::uint16_t>(_bBigEndian
				? (static_cast<std::uint16_t>(_Bytes[_nOffset]) << 8) | _Bytes[_nOffset + 1]
				: _Bytes[_nOffset] | (static_cast<std::uint16_t>(_Bytes[_nOffset + 1]) << 8)));
		}

		// 원본 bytes.decode("utf-16-le"/"utf-16-be") strict 와 같은 판정이다 - 홀수 꼬리와
		// 짝 없는 대리 문자를 실패로 본다. import_pipeline 의 utf16_replace 는 치환 디코더라
		// 여기에 쓸 수 없다.
		bool utf16_strict(std::span<const std::uint8_t> _Bytes, bool _bBigEndian, std::string* _psOut)
		{
			_psOut->clear();
			if ((_Bytes.size() % 2) != 0) { return(false); }

			std::size_t nOffset = 0;
			while (nOffset < _Bytes.size())
			{
				const std::uint16_t nFirst = utf16_unit(_Bytes, nOffset, _bBigEndian);
				nOffset += 2;
				if (nFirst >= 0xD800 && nFirst <= 0xDBFF)
				{
					if (nOffset + 2 > _Bytes.size()) { _psOut->clear(); return(false); }
					const std::uint16_t nSecond = utf16_unit(_Bytes, nOffset, _bBigEndian);
					if (nSecond < 0xDC00 || nSecond > 0xDFFF) { _psOut->clear(); return(false); }
					nOffset += 2;
					append_utf8(0x10000u + ((static_cast<std::uint32_t>(nFirst) - 0xD800u) << 10)
						+ (static_cast<std::uint32_t>(nSecond) - 0xDC00u), _psOut);
					continue;
				}
				if (nFirst >= 0xDC00 && nFirst <= 0xDFFF) { _psOut->clear(); return(false); }
				append_utf8(nFirst, _psOut);
			}
			return(true);
		}

		void push_utf16_unit(std::vector<std::uint8_t>* _pOut, std::uint16_t _nUnit, bool _bBigEndian)
		{
			if (_bBigEndian)
			{
				_pOut->push_back(static_cast<std::uint8_t>(_nUnit >> 8));
				_pOut->push_back(static_cast<std::uint8_t>(_nUnit & 0xFF));
				return;
			}
			_pOut->push_back(static_cast<std::uint8_t>(_nUnit & 0xFF));
			_pOut->push_back(static_cast<std::uint8_t>(_nUnit >> 8));
		}

		bool utf16_encode(std::string_view _sUtf8Text, bool _bBigEndian, std::vector<std::uint8_t>* _pOut)
		{
			_pOut->clear();
			const std::span<const std::uint8_t> Bytes = as_bytes(_sUtf8Text);
			std::size_t nOffset = 0;
			while (nOffset < Bytes.size())
			{
				std::uint32_t nScalar = 0;
				if (!decode_one_utf8(Bytes, &nOffset, &nScalar)) { _pOut->clear(); return(false); }
				if (nScalar <= 0xFFFF)
				{
					push_utf16_unit(_pOut, static_cast<std::uint16_t>(nScalar), _bBigEndian);
					continue;
				}
				const std::uint32_t nValue = nScalar - 0x10000u;
				push_utf16_unit(_pOut, static_cast<std::uint16_t>(0xD800u + (nValue >> 10)), _bBigEndian);
				push_utf16_unit(_pOut, static_cast<std::uint16_t>(0xDC00u + (nValue & 0x3FFu)), _bBigEndian);
			}
			return(true);
		}

		// 원본 text.encode(binding.encoding) 자리다. core 가 직접 다루는 셋 밖의 이름은
		// mbcs seam 이 아니면 실패이며 그것이 원본 LookupError 의 자리다.
		bool encode_text(
			std::string_view           _sUtf8Text,
			const std::string&         _sEncoding,
			const LegacyEncoder&       _Encoder,
			std::vector<std::uint8_t>* _pOut)
		{
			if (_sEncoding == "utf-8")
			{
				const std::span<const std::uint8_t> Bytes = as_bytes(_sUtf8Text);
				_pOut->assign(Bytes.begin(), Bytes.end());
				return(true);
			}
			if (_sEncoding == "utf-16-le") { return(utf16_encode(_sUtf8Text, false, _pOut)); }
			if (_sEncoding == "utf-16-be") { return(utf16_encode(_sUtf8Text, true, _pOut)); }
			if (_sEncoding == ANSI_ENCODING)
			{
				_pOut->clear();
				return(_Encoder && _Encoder(_sUtf8Text, _pOut));
			}
			return(false);
		}

		// 원본 _BOM_BY_ENCODING(:28~32). 표에 없는 인코딩에 BOM 을 요구하면 실패다.
		bool bom_for_encoding(const std::string& _sEncoding, std::vector<std::uint8_t>* _pOut)
		{
			if (_sEncoding == "utf-8")     { _pOut->assign(std::begin(BOM_UTF8), std::end(BOM_UTF8)); return(true); }
			if (_sEncoding == "utf-16-le") { _pOut->assign(std::begin(BOM_UTF16_LE), std::end(BOM_UTF16_LE)); return(true); }
			if (_sEncoding == "utf-16-be") { _pOut->assign(std::begin(BOM_UTF16_BE), std::end(BOM_UTF16_BE)); return(true); }
			return(false);
		}

		// 원본 _decode_strict(:225~237). BOM 이 있으면 그 인코딩 하나만 시도한다 - 뒤 단계로
		// 내려가면 BOM 바이트가 본문에 섞여 편집 결과가 오염된다.
		bool decode_strict(
			std::span<const std::uint8_t> _Bytes,
			const StrictLegacyDecoder&    _Decoder,
			std::string*                  _psText,
			std::string*                  _psEncoding,
			bool*                         _pbBom)
		{
			if (starts_with(_Bytes, BOM_UTF8, std::size(BOM_UTF8)))
			{
				const std::span<const std::uint8_t> Rest = _Bytes.subspan(std::size(BOM_UTF8));
				if (!strict_utf8(Rest)) { return(false); }
				*_psText = as_text(Rest);
				*_psEncoding = "utf-8";
				*_pbBom = true;
				return(true);
			}
			if (starts_with(_Bytes, BOM_UTF16_LE, std::size(BOM_UTF16_LE)))
			{
				if (!utf16_strict(_Bytes.subspan(std::size(BOM_UTF16_LE)), false, _psText)) { return(false); }
				*_psEncoding = "utf-16-le";
				*_pbBom = true;
				return(true);
			}
			if (starts_with(_Bytes, BOM_UTF16_BE, std::size(BOM_UTF16_BE)))
			{
				if (!utf16_strict(_Bytes.subspan(std::size(BOM_UTF16_BE)), true, _psText)) { return(false); }
				*_psEncoding = "utf-16-be";
				*_pbBom = true;
				return(true);
			}
			if (strict_utf8(_Bytes))
			{
				*_psText = as_text(_Bytes);
				*_psEncoding = "utf-8";
				*_pbBom = false;
				return(true);
			}
			_psText->clear();
			if (_Decoder && _Decoder(_Bytes, _psText))
			{
				*_psEncoding = std::string(ANSI_ENCODING);
				*_pbBom = false;
				return(true);
			}
			return(false);
		}

		// 원본 text.replace("\r\n", "\n").replace("\r", "\n") 과 같은 결과다.
		std::string normalise_newlines(std::string_view _sText)
		{
			std::string sResult;
			sResult.reserve(_sText.size());
			for (std::size_t i = 0; i < _sText.size(); ++i)
			{
				if (_sText[i] != '\r') { sResult.push_back(_sText[i]); continue; }
				if (i + 1 < _sText.size() && _sText[i + 1] == '\n') { ++i; }
				sResult.push_back('\n');
			}
			return(sResult);
		}

		// 원본 text.replace("\n", binding.newline.characters) 자리다.
		std::string apply_newline(std::string_view _sText, std::string_view _sNewline)
		{
			std::string sResult;
			sResult.reserve(_sText.size());
			for (const char ch : _sText)
			{
				if (ch == '\n') { sResult.append(_sNewline); continue; }
				sResult.push_back(ch);
			}
			return(sResult);
		}

		// 원본 _atomic_write(:274~293). 같은 디렉터리의 임시 파일에 쓰고 교체하며, 교체 전에
		// 실패하면 임시 파일을 반드시 지운다. 사유는 정리 호출이 덮어쓰기 전에 붙잡아 둔다.
		bool atomic_write(
			I_BINDING_FILE_SYSTEM&        _FileSystem,
			const std::string&            _sPath,
			std::span<const std::uint8_t> _Bytes,
			std::string*                  _psError)
		{
			std::string sTemporary;
			if (!_FileSystem.CreateUniqueTemporaryPathFor(_sPath, &sTemporary))
			{
				*_psError = _FileSystem.LastError();
				return(false);
			}
			if (!_FileSystem.WriteAllBytes(sTemporary, _Bytes))
			{
				*_psError = _FileSystem.LastError();
				_FileSystem.Remove(sTemporary);
				return(false);
			}
			if (!_FileSystem.Replace(sTemporary, _sPath))
			{
				*_psError = _FileSystem.LastError();
				_FileSystem.Remove(sTemporary);
				return(false);
			}
			return(true);
		}

		// 원본 _record_sync(:296~321). stat 실패는 지문을 남기지 않는 쪽으로 처리한다 -
		// 다음 저장이 (d) 분기로 떨어져 다시 기록하므로 안전한 방향이다.
		bool record_sync(
			storage::C_REPOSITORIES&      _Repositories,
			const domain::S_FILE_BINDING& _Binding,
			std::span<const std::uint8_t> _Data,
			const BindingClock&           _Clock,
			const I_BINDING_FILE_SYSTEM&  _FileSystem)
		{
			std::int64_t nSize = 0;
			std::int64_t nMtimeNs = 0;
			if (!_FileSystem.Stat(_Binding.sPath, &nSize, &nMtimeNs)) { return(true); }

			domain::S_FILE_BINDING Updated = _Binding;
			Updated.nSyncedSize    = nSize;
			Updated.nSyncedMtimeNs = nMtimeNs;
			Updated.sSyncedHash    = HashBytes(_Data);
			Updated.nSyncedAtUs    = _Clock ? _Clock() : default_clock();
			return(_Repositories.UpsertFileBinding(Updated) == storage::E_REPO_RESULT::Ok);
		}
	}

	std::string_view ToText(E_FILE_SYNC_OUTCOME _eValue)
	{
		switch (_eValue)
		{
		case E_FILE_SYNC_OUTCOME::Noop:           return("noop");
		case E_FILE_SYNC_OUTCOME::Written:        return("written");
		case E_FILE_SYNC_OUTCOME::ExternalChange: return("external_change");
		case E_FILE_SYNC_OUTCOME::Failed:         return("failed");
		}
		return(std::string_view{});
	}

	bool HasControlChars(std::string_view _sUtf8Text)
	{
		// 유효 UTF-8 에서 다바이트 문자의 모든 바이트는 0x80 이상이라 제어 문자가 될 수 없다.
		// 따라서 바이트 훑기가 원본의 코드포인트 훑기와 같은 판정이다.
		for (const char raw : _sUtf8Text)
		{
			const unsigned char ch = static_cast<unsigned char>(raw);
			if (ch >= 0x80) { continue; }
			if (ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v') { continue; }
			if (ch < 0x20 || ch == 0x7F) { return(true); }
		}
		return(false);
	}

	bool HasRoundtripHazard(std::string_view _sUtf8Text)
	{
		// UTF-8 은 자기동기적이라 아래 바이트열은 문자 경계에서만 나타난다 - 부분 문자열 검색이
		// 코드포인트 검색과 같은 판정이다. 원본 _ROUNDTRIP_HAZARDS(:40) 의 다섯이다.
		static const std::string_view HAZARDS[] = {
			"\xc2\xa0",      // U+00A0
			"\xe2\x80\xa8",  // U+2028
			"\xe2\x80\xa9",  // U+2029
			"\xef\xb7\x90",  // U+FDD0
			"\xef\xb7\x91",  // U+FDD1
		};
		for (const std::string_view sHazard : HAZARDS)
		{
			if (_sUtf8Text.find(sHazard) != std::string_view::npos) { return(true); }
		}
		return(false);
	}

	domain::E_NEWLINE_KIND DetectNewline(std::string_view _sUtf8Text)
	{
		const std::size_t nCarriage = _sUtf8Text.find('\r');
		const std::size_t nLinefeed = _sUtf8Text.find('\n');
		// 원본은 플랫폼 분기지만 포팅본은 Windows 전용이라 win32 갈래만 이식한다.
		if (nCarriage == std::string_view::npos && nLinefeed == std::string_view::npos)
		{
			return(domain::E_NEWLINE_KIND::Crlf);
		}
		if (nCarriage == std::string_view::npos
			|| (nLinefeed != std::string_view::npos && nLinefeed < nCarriage))
		{
			return(domain::E_NEWLINE_KIND::Lf);
		}
		const std::size_t nNext = nCarriage + 1;
		const bool        bCrlf = nNext < _sUtf8Text.size() && _sUtf8Text[nNext] == '\n';
		return(bCrlf ? domain::E_NEWLINE_KIND::Crlf : domain::E_NEWLINE_KIND::Cr);
	}

	bool DetectText(
		std::span<const std::uint8_t> _Bytes,
		const StrictLegacyDecoder&    _Decoder,
		S_DETECTED_TEXT*              _pOut)
	{
		std::string sDecoded;
		std::string sEncoding;
		bool        bBom = false;
		if (!decode_strict(_Bytes, _Decoder, &sDecoded, &sEncoding, &bBom)) { return(false); }
		if (HasControlChars(sDecoded) || HasRoundtripHazard(sDecoded)) { return(false); }

		// newline 과 trailing_newline 은 정규화 **전** 텍스트로 판정한다(원본 :126~127).
		_pOut->eNewline         = DetectNewline(sDecoded);
		_pOut->bTrailingNewline = !sDecoded.empty()
			&& (sDecoded.back() == '\n' || sDecoded.back() == '\r');
		_pOut->sText     = normalise_newlines(sDecoded);
		_pOut->sEncoding = std::move(sEncoding);
		_pOut->bBom      = bBom;
		return(true);
	}

	bool RenderBytes(
		std::string_view              _sUtf8Text,
		const domain::S_FILE_BINDING& _Binding,
		const LegacyEncoder&          _Encoder,
		std::vector<std::uint8_t>*    _pOut)
	{
		// 원본은 인코딩을 먼저 하고 BOM 을 뒤에 붙인다 - 미지 인코딩·표현 불가 실패가 BOM
		// 실패보다 앞선다. 순서를 바꾸면 mbcs+bom 의 실패 사유가 달라진다.
		const std::string    sBody = apply_newline(_sUtf8Text, domain::NewlineCharacters(_Binding.eNewline));
		std::vector<std::uint8_t> Encoded;
		if (!encode_text(sBody, _Binding.sEncoding, _Encoder, &Encoded)) { return(false); }

		if (!_Binding.bBom)
		{
			*_pOut = std::move(Encoded);
			return(true);
		}

		std::vector<std::uint8_t> Prefix;
		if (!bom_for_encoding(_Binding.sEncoding, &Prefix)) { return(false); }
		Prefix.insert(Prefix.end(), Encoded.begin(), Encoded.end());
		*_pOut = std::move(Prefix);
		return(true);
	}

	std::string HashBytes(std::span<const std::uint8_t> _Bytes)
	{
		return(storage::TextHash(as_text(_Bytes)));
	}

	bool ReadFileHash(
		const I_BINDING_FILE_SYSTEM& _FileSystem,
		const std::string&           _sPath,
		std::optional<std::string>*  _psHash)
	{
		std::vector<std::uint8_t> Bytes;
		bool                      bFound = false;
		if (!_FileSystem.ReadAllBytes(_sPath, &Bytes, &bFound)) { return(false); }
		*_psHash = bFound ? std::optional<std::string>(HashBytes(Bytes)) : std::nullopt;
		return(true);
	}

	bool PrepareBindingPath(
		storage::C_REPOSITORIES&   _Repositories,
		const std::string&         _sPathKey,
		S_BINDING_PATH_RESOLUTION* _pOut)
	{
		domain::S_FILE_BINDING Existing;
		const storage::E_REPO_RESULT eFind = _Repositories.FindBindingByPath(_sPathKey, &Existing);
		if (eFind == storage::E_REPO_RESULT::NotFound)
		{
			*_pOut = S_BINDING_PATH_RESOLUTION{ E_BINDING_PATH_STATUS::Free, std::nullopt };
			return(true);
		}
		if (eFind != storage::E_REPO_RESULT::Ok) { return(false); }

		domain::S_CARD Holder;
		const storage::E_REPO_RESULT eHolder = _Repositories.GetCard(Existing.sCardId, &Holder);
		if (eHolder == storage::E_REPO_RESULT::Ok && !Holder.nDeletedAtUs.has_value())
		{
			*_pOut = S_BINDING_PATH_RESOLUTION{ E_BINDING_PATH_STATUS::HeldByActiveCard, Existing.sCardId };
			return(true);
		}
		if (eHolder != storage::E_REPO_RESULT::Ok && eHolder != storage::E_REPO_RESULT::NotFound)
		{
			return(false);
		}

		if (_Repositories.DeleteFileBinding(Existing.sCardId) != storage::E_REPO_RESULT::Ok) { return(false); }
		*_pOut = S_BINDING_PATH_RESOLUTION{ E_BINDING_PATH_STATUS::Free, std::nullopt };
		return(true);
	}

	bool SyncFile(
		storage::C_REPOSITORIES&   _Repositories,
		const domain::S_CARD&      _Card,
		const S_FILE_SYNC_OPTIONS& _Options,
		const BindingClock&        _Clock,
		const LegacyEncoder&       _Encoder,
		I_BINDING_FILE_SYSTEM&     _FileSystem,
		S_FILE_SYNC_RESULT*        _pResult)
	{
		domain::S_FILE_BINDING Binding;
		const storage::E_REPO_RESULT eGet = _Repositories.GetFileBinding(_Card.sId, &Binding);
		if (eGet == storage::E_REPO_RESULT::NotFound)
		{
			*_pResult = S_FILE_SYNC_RESULT{ E_FILE_SYNC_OUTCOME::Noop, std::string{} };
			return(true);
		}
		if (eGet != storage::E_REPO_RESULT::Ok) { return(false); }

		std::vector<std::uint8_t> Rendered;
		if (!RenderBytes(_Card.sBody, Binding, _Encoder, &Rendered))
		{
			*_pResult = S_FILE_SYNC_RESULT{
				E_FILE_SYNC_OUTCOME::Failed,
				reinterpret_cast<const char*>(u8"결속 파일 인코딩에 실패했습니다: ") + Binding.sPath };
			return(true);
		}

		std::vector<std::uint8_t> Current;
		bool                      bFound = false;
		if (!_FileSystem.ReadAllBytes(Binding.sPath, &Current, &bFound))
		{
			*_pResult = S_FILE_SYNC_RESULT{ E_FILE_SYNC_OUTCOME::Failed, _FileSystem.LastError() };
			return(true);
		}

		if (bFound)
		{
			// (c) 파일 바이트 == 렌더 바이트: 되쓰지 않고 지문만 갱신한다.
			if (Current == Rendered)
			{
				if (!record_sync(_Repositories, Binding, Rendered, _Clock, _FileSystem)) { return(false); }
				*_pResult = S_FILE_SYNC_RESULT{ E_FILE_SYNC_OUTCOME::Noop, std::string{} };
				return(true);
			}
			// (e) 파일 해시 != synced_hash: 묻지 않고 덮어쓰지 않는다. bInteractive 는 원본에서도
			// 로그 수준만 바꾸므로 로거가 없는 포팅본에서는 결과값을 바꾸지 않는다.
			if (!_Options.bForce
				&& Binding.sSyncedHash.has_value()
				&& HashBytes(Current) != *Binding.sSyncedHash)
			{
				*_pResult = S_FILE_SYNC_RESULT{ E_FILE_SYNC_OUTCOME::ExternalChange, std::string{} };
				return(true);
			}
		}

		// (b) 파일 부재 재생성 / (d) 외부 변경 없음: 기록한다.
		std::string sError;
		if (!atomic_write(_FileSystem, Binding.sPath, Rendered, &sError))
		{
			*_pResult = S_FILE_SYNC_RESULT{ E_FILE_SYNC_OUTCOME::Failed, sError };
			return(true);
		}
		if (!record_sync(_Repositories, Binding, Rendered, _Clock, _FileSystem)) { return(false); }
		*_pResult = S_FILE_SYNC_RESULT{ E_FILE_SYNC_OUTCOME::Written, std::string{} };
		return(true);
	}
}
