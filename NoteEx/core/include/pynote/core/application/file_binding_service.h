#pragma once

#include "pynote/core/domain/models.h"
#include "pynote/core/storage/repositories.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// core 계층. Win32/ATL/WTL/COM/DirectX 헤더를 포함하지 않는다(tools/gates 격리 게이트가 강제).
// 파이썬 원본 application/file_binding_service.py 이식이다. 경로 문법과 ANSI 코덱은 core 가
// 알 수 없으므로 seam 으로 빼고, 그 구현은 platform/win32_file_binding_support 가 가진다.

namespace pynote::core::application
{
	// 원본 _ANSI_ENCODING(:25) 의 win32 갈래다. 이 문자열이 그대로 DB 의 encoding 열에 들어가고
	// 두 앱이 같은 DB 를 읽으므로 철자 자체가 계약이다.
	inline constexpr std::string_view ANSI_ENCODING = "mbcs";

	// ANSI 코덱 seam. import_pipeline.h 의 LegacyDecoder 와 같은 계열이되 strict 판정이 필요해
	// 서명이 다르다 - 실패하면 false 이고 치환 문자를 만들지 않는다(원본 UnicodeDecodeError 자리).
	using StrictLegacyDecoder = std::function<bool(std::span<const std::uint8_t>, std::string*)>;

	// 표현할 수 없는 문자가 하나라도 있으면 false 다(원본 UnicodeEncodeError 자리).
	using LegacyEncoder = std::function<bool(std::string_view, std::vector<std::uint8_t>*)>;

	// 원본 Clock = Callable[[], int](:21). epoch 마이크로초다.
	using BindingClock = std::function<std::int64_t()>;

	// 원본 DetectedText(:59~67).
	struct S_DETECTED_TEXT
	{
		std::string            sText;
		std::string            sEncoding;
		bool                   bBom{ false };
		domain::E_NEWLINE_KIND eNewline{ domain::E_NEWLINE_KIND::Crlf };
		bool                   bTrailingNewline{ false };

		bool operator==(const S_DETECTED_TEXT&) const = default;
	};

	// 원본 PendingFileBinding(:70~82). 카드가 아직 없는 동안 들고 있는 결속 예약이다.
	// 소비자가 편집기라 결선은 W6 이며 여기서는 데이터 구조만 둔다.
	struct S_PENDING_FILE_BINDING
	{
		std::string            sPath;
		std::string            sEncoding;
		bool                   bBom{ false };
		domain::E_NEWLINE_KIND eNewline{ domain::E_NEWLINE_KIND::Crlf };
		bool                   bTrailingNewline{ false };

		bool operator==(const S_PENDING_FILE_BINDING&) const = default;
	};

	// 원본 FileSyncOutcome(:43~49).
	enum class E_FILE_SYNC_OUTCOME
	{
		Noop,
		Written,
		ExternalChange,
		Failed
	};

	// 원본이 StrEnum 이라 값 철자가 곧 표현이다. 골든 직렬화가 이 철자를 쓴다.
	std::string_view ToText(E_FILE_SYNC_OUTCOME _eValue);

	// 원본 FileSyncResult(:85~90). sError 는 UI 문구라 원문 parity 대상이 아니되 실패 원인이
	// Win32 오류면 그 숫자를 포함한다 - 읽기 전용 대상의 5번을 사용자가 구분할 수 있어야 한다.
	struct S_FILE_SYNC_RESULT
	{
		E_FILE_SYNC_OUTCOME eOutcome{ E_FILE_SYNC_OUTCOME::Noop };
		std::string         sError;
	};

	// 원본 sync_file 의 키워드 인자 force/interactive(:177~184) 자리다.
	struct S_FILE_SYNC_OPTIONS
	{
		bool bForce{ false };
		bool bInteractive{ false };
	};

	// 원본 BindingPathStatus(:52~56).
	enum class E_BINDING_PATH_STATUS
	{
		Free,
		HeldByActiveCard
	};

	// 원본 BindingPathResolution(:93~98).
	struct S_BINDING_PATH_RESOLUTION
	{
		E_BINDING_PATH_STATUS      eStatus{ E_BINDING_PATH_STATUS::Free };
		std::optional<std::string> sHolderCardId{};

		bool operator==(const S_BINDING_PATH_RESOLUTION&) const = default;
	};

	// 결속 되쓰기가 파일을 만지는 유일한 통로다. 원본이 부르는 연산만 담는다.
	//
	// CEILING: I_BINDING_FILE_SYSTEM 의 Replace·Remove·CreateUniqueTemporaryPath·LastError
	// 는 I_FILE_SYSTEM 과 의미가 같은 중복 선언이다 - 백업 계층 시험이 대역을 갱신할 수 있는
	// wave 에서 공통 기반 인터페이스로 합친다.
	class I_BINDING_FILE_SYSTEM
	{
	public:
		virtual ~I_BINDING_FILE_SYSTEM() = default;

		// 원본 _read_bytes(:267~271). 부재는 _pFound false + **true 반환**이고, 그 밖의 오류는
		// false 반환이라 호출부가 Failed 로 접는다(원본은 FileNotFoundError 만 삼킨다).
		virtual bool ReadAllBytes(
			const std::string&         _sPath,
			std::vector<std::uint8_t>* _pBytes,
			bool*                      _pFound) const = 0;

		// Path.stat(). _pnMtimeNs 는 원본 st_mtime_ns 와 같은 단위(나노초)다.
		virtual bool Stat(const std::string& _sPath, std::int64_t* _pnSize, std::int64_t* _pnMtimeNs) const = 0;

		// 원본 mkstemp(dir=path.parent, prefix=".<name>.", suffix=".tmp"). 부모·파일명을 잘라내는
		// 것은 platform 이다 - core 는 경로 문법을 알지 못하고, 부모를 잘못 잡으면 임시 파일이
		// 다른 볼륨에 생겨 교체의 원자성이 깨진다.
		virtual bool CreateUniqueTemporaryPathFor(const std::string& _sTargetPath, std::string* _psPath) = 0;

		virtual bool WriteAllBytes(const std::string& _sPath, std::span<const std::uint8_t> _Bytes) = 0;

		// os.replace. 대상이 없어도 성공해야 한다.
		virtual bool Replace(const std::string& _sFrom, const std::string& _sTo) = 0;

		// Path.unlink(missing_ok=True). 대상이 없으면 성공이다.
		virtual bool Remove(const std::string& _sPath) = 0;

		// 마지막 실패 사유. 원본이 OSError 메시지로 남기는 자리다.
		virtual const std::string& LastError() const = 0;
	};

	// 원본 has_control_chars(:101~106). 판정은 UTF-8 바이트열 위에서 한다.
	bool HasControlChars(std::string_view _sUtf8Text);

	// 원본 has_roundtrip_hazard(:109~111). 편집기 왕복이 치환하는 다섯 문자를 잡는다.
	bool HasRoundtripHazard(std::string_view _sUtf8Text);

	// 원본 _detect_newline(:247~256) 의 win32 갈래다. 줄끝이 하나도 없으면 Crlf 다.
	domain::E_NEWLINE_KIND DetectNewline(std::string_view _sUtf8Text);

	// 원본 detect_text(:114~128). 결속 불가면 false 이고 _pOut 은 손대지 않는다.
	bool DetectText(
		std::span<const std::uint8_t> _Bytes,
		const StrictLegacyDecoder&    _Decoder,
		S_DETECTED_TEXT*              _pOut);

	// 원본 render_bytes(:131~139). 인코딩·BOM·표현 불가 실패를 전부 false 로 접는다.
	bool RenderBytes(
		std::string_view              _sUtf8Text,
		const domain::S_FILE_BINDING& _Binding,
		const LegacyEncoder&          _Encoder,
		std::vector<std::uint8_t>*    _pOut);

	// 원본 hash_bytes(:148~150). sha256 소문자 16진수 64자다.
	std::string HashBytes(std::span<const std::uint8_t> _Bytes);

	// 원본 read_file_hash(:153~156). 파일이 없으면 값 없음이고, 그 밖의 읽기 오류는 false 다.
	bool ReadFileHash(
		const I_BINDING_FILE_SYSTEM& _FileSystem,
		const std::string&           _sPath,
		std::optional<std::string>*  _psHash);

	// 원본 prepare_binding_path(:159~174). 휴지통 카드가 쥔 행은 지우고 Free 로 돌려준다.
	// 저장소 오류가 나면 false 다(원본이 sqlite3.Error 를 올리는 자리).
	bool PrepareBindingPath(
		storage::C_REPOSITORIES&   _Repositories,
		const std::string&         _sPathKey,
		S_BINDING_PATH_RESOLUTION* _pOut);

	// 원본 sync_file(:177~222). 분기는 설계지시서 §2-5 표 (a)~(e) 그대로다.
	// 저장소 오류가 나면 false 이고 그때 _pResult 는 의미가 없다(원본이 예외를 올리는 자리).
	bool SyncFile(
		storage::C_REPOSITORIES&     _Repositories,
		const domain::S_CARD&        _Card,
		const S_FILE_SYNC_OPTIONS&   _Options,
		const BindingClock&          _Clock,
		const LegacyEncoder&         _Encoder,
		I_BINDING_FILE_SYSTEM&       _FileSystem,
		S_FILE_SYNC_RESULT*          _pResult);
}
