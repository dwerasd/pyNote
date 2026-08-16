#pragma once

#include "pynote/core/storage/file_system.h"

#include <cstdint>
#include <random>
#include <string>
#include <vector>

// platform 계층. core 가 선언한 파일 연산을 Win32 로 구현한다 - core 는 이 헤더를 포함하지
// 않으며(격리 게이트가 강제) 이 헤더도 windows.h 를 끌고 오지 않는다. 소비 프로젝트가
// 원치 않는 Win32 매크로를 물려받지 않도록 구현 세부는 전부 .cpp 에 둔다.

namespace pynote::platform
{
	// 경로는 전부 UTF-8 이고 내부에서 UTF-16 으로 옮긴다. 파이썬 원본이 다루는 경로도
	// 유니코드라 인코딩을 좁히면 한글 경로에서 동작이 갈린다.
	class C_WIN32_FILE_SYSTEM : public core::storage::I_FILE_SYSTEM
	{
	public:
		C_WIN32_FILE_SYSTEM() = default;

		C_WIN32_FILE_SYSTEM(const C_WIN32_FILE_SYSTEM&) = delete;
		C_WIN32_FILE_SYSTEM& operator=(const C_WIN32_FILE_SYSTEM&) = delete;

		bool Exists(const std::string& _sPath) const override;
		bool IsRegularFile(const std::string& _sPath) const override;
		bool IsSymlink(const std::string& _sPath) const override;
		bool CreateDirectories(const std::string& _sPath) override;
		bool Replace(const std::string& _sFrom, const std::string& _sTo) override;
		bool Remove(const std::string& _sPath) override;
		bool CreateUniqueTemporaryPath(
			const std::string& _sDirectory,
			const std::string& _sPrefix,
			const std::string& _sSuffix,
			std::string*       _psPath) override;
		bool ModifiedTimeUs(const std::string& _sPath, std::int64_t* _pnValueUs) const override;
		bool ListDirectory(const std::string& _sDirectory, std::vector<std::string>* _pNames) const override;

		const std::string& LastError() const override { return(m_sLastError); }

	private:
		void set_error_(const std::string& _sOperation, const std::string& _sPath, unsigned long _nCode) const;

		mutable std::string m_sLastError;

		// 임시 이름 발생기. 파이썬 tempfile 도 프로세스마다 한 벌을 들고 이름을 뽑는다.
		std::mt19937 m_Random{ std::random_device{}() };
	};
}
