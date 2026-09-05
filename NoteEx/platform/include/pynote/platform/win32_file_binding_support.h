#pragma once

#include "pynote/core/application/file_binding_service.h"
#include "pynote/platform/win32_file_system.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// platform 계층. core 가 선언한 결속 seam 을 Win32 로 구현한다 - core 는 이 헤더를 포함하지
// 않으며(격리 게이트가 강제) 이 헤더도 windows.h 를 끌고 오지 않는다.

namespace pynote::platform
{
	// 원본 bytes.decode("mbcs") strict 자리다. 치환 문자를 만들지 않고 실패하면 false 다 -
	// 가져오기용 DecodeWindowsCodePage 의 0x80 -> U+FFFD 특례는 치환 디코더 전용이라
	// 여기로 승계하지 않는다(단독 0x80 은 U+0080 으로 디코딩돼 결속 가능이다).
	bool DecodeSystemAnsiStrict(std::span<const std::uint8_t> _Bytes, std::string* _psUtf8);

	// 원본 str.encode("mbcs") strict 자리다. WC_NO_BEST_FIT_CHARS 로 최적 대응 치환을 막고
	// 기본 문자가 쓰였으면 실패로 돌린다 - 플래그를 빼면 U+00C0 이 'A' 로 조용히 저장된다.
	bool EncodeSystemAnsiStrict(std::string_view _sUtf8Text, std::vector<std::uint8_t>* _pOut);

	// 원본 resolve_path(:142~145). 절대 경로와 대소문자 무시 비교용 키를 함께 만든다.
	// 존재하는 접두는 디스크의 실제 표기로 접히고 부재 꼬리는 입력 표기가 보존된다.
	// 선언 편차: 빈 경로는 원본(현재 디렉터리를 돌려준다)과 달리 실패다 - 소비자가 argv·
	// 대화상자 경로뿐이라 현재 도달하지 않는다(P1 감사 lens1-1).
	bool ResolveBindingPath(const std::string& _sUtf8Path, std::string* _psPath, std::string* _psPathKey);

	// 결속 되쓰기가 쓰는 파일 연산의 Win32 구현이다. 원자 교체와 임시 이름 확보는
	// C_WIN32_FILE_SYSTEM 이 이미 쓰는 API 를 그대로 재사용한다 - 다른 원자 교체 API 로
	// 바꾸면 내구성 동작이 파이썬 원본과 달라진다.
	// CEILING: LastError() 의 문구 인코딩은 프로젝트의 /utf-8 여부에 종속된다 - 시험 바이너리
	// (NoteExTests, /utf-8 없음)에서 take_error_ 갈래(교체·삭제·임시 이름)는 CP949 이고
	// set_error_ 갈래는 UTF-8 이다. UI 소비가 생기는 wave(W6)에서 좁은 리터럴을 u8 로 통일한다.
	class C_WIN32_BINDING_FILE_SYSTEM : public core::application::I_BINDING_FILE_SYSTEM
	{
	public:
		C_WIN32_BINDING_FILE_SYSTEM() = default;

		C_WIN32_BINDING_FILE_SYSTEM(const C_WIN32_BINDING_FILE_SYSTEM&) = delete;
		C_WIN32_BINDING_FILE_SYSTEM& operator=(const C_WIN32_BINDING_FILE_SYSTEM&) = delete;

		bool ReadAllBytes(
			const std::string&         _sPath,
			std::vector<std::uint8_t>* _pBytes,
			bool*                      _pFound) const override;
		bool Stat(const std::string& _sPath, std::int64_t* _pnSize, std::int64_t* _pnMtimeNs) const override;
		bool CreateUniqueTemporaryPathFor(const std::string& _sTargetPath, std::string* _psPath) override;
		bool WriteAllBytes(const std::string& _sPath, std::span<const std::uint8_t> _Bytes) override;
		bool Replace(const std::string& _sFrom, const std::string& _sTo) override;
		bool Remove(const std::string& _sPath) override;

		const std::string& LastError() const override { return(m_sLastError); }

	private:
		void set_error_(const std::string& _sOperation, const std::string& _sPath, unsigned long _nCode) const;
		void take_error_() const;

		mutable std::string m_sLastError;

		// 원자 교체·임시 이름·삭제는 백업 계층과 같은 구현을 쓴다.
		C_WIN32_FILE_SYSTEM m_Files;
	};
}
