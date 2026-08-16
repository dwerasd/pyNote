#include "pynote/platform/win32_file_system.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace
{
	// tempfile._RandomNameSequence 가 쓰는 글자표와 길이를 그대로 따른다. 이름 모양이 같아야
	// 임시 파일을 지우는 운영 스크립트나 백신 예외 규칙이 원본과 같은 것을 본다.
	const char* const TEMPORARY_NAME_CHARACTERS = "abcdefghijklmnopqrstuvwxyz0123456789_";
	const int         TEMPORARY_NAME_LENGTH     = 8;

	// 파이썬 tempfile 이 이름 충돌에 재시도하는 횟수(TMP_MAX)와 같다.
	const int TEMPORARY_NAME_ATTEMPTS = 10000;

	std::wstring widen(const std::string& _sText)
	{
		if (_sText.empty()) { return(std::wstring{}); }
		const int nSize = ::MultiByteToWideChar(
			CP_UTF8, 0, _sText.data(), static_cast<int>(_sText.size()), nullptr, 0);
		if (nSize <= 0) { return(std::wstring{}); }

		std::wstring sResult(static_cast<std::size_t>(nSize), L'\0');
		::MultiByteToWideChar(
			CP_UTF8, 0, _sText.data(), static_cast<int>(_sText.size()), sResult.data(), nSize);
		return(sResult);
	}

	std::string narrow(const wchar_t* _pszText)
	{
		if (_pszText == nullptr || _pszText[0] == L'\0') { return(std::string{}); }
		const int nSize = ::WideCharToMultiByte(CP_UTF8, 0, _pszText, -1, nullptr, 0, nullptr, nullptr);
		if (nSize <= 1) { return(std::string{}); }

		// 반환 크기에 종단 널이 포함되므로 한 글자를 뺀다.
		std::string sResult(static_cast<std::size_t>(nSize - 1), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, _pszText, -1, sResult.data(), nSize, nullptr, nullptr);
		return(sResult);
	}

	bool is_separator(char _ch)
	{
		return(_ch == '\\' || _ch == '/');
	}

	// 상위 디렉터리. 더 오를 곳이 없으면 빈 문자열이다(드라이브 루트, UNC 루트, 상대 이름 하나).
	std::string parent_of(const std::string& _sPath)
	{
		std::size_t nEnd = _sPath.size();
		while (nEnd > 0 && is_separator(_sPath[nEnd - 1])) { --nEnd; }

		std::size_t nPos = std::string::npos;
		for (std::size_t i = nEnd; i > 0; --i)
		{
			if (is_separator(_sPath[i - 1])) { nPos = i - 1; break; }
		}
		if (nPos == std::string::npos) { return(std::string{}); }

		std::string sParent = _sPath.substr(0, nPos);
		if (sParent.empty()) { return(std::string{}); }
		// "D:" 나 "\\\\server" 처럼 더 오를 수 없는 자리는 부모가 없는 것으로 본다.
		if (sParent.size() == 2 && sParent[1] == ':') { return(std::string{}); }
		return(sParent);
	}

	bool is_directory(const std::wstring& _sPath)
	{
		const DWORD nAttributes = ::GetFileAttributesW(_sPath.c_str());
		return(nAttributes != INVALID_FILE_ATTRIBUTES && (nAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
	}
}

namespace pynote::platform
{
	void C_WIN32_FILE_SYSTEM::set_error_(
		const std::string& _sOperation, const std::string& _sPath, unsigned long _nCode) const
	{
		m_sLastError = _sOperation + " 실패(Win32 오류 " + std::to_string(_nCode) + "): " + _sPath;
	}

	bool C_WIN32_FILE_SYSTEM::Exists(const std::string& _sPath) const
	{
		if (_sPath.empty()) { return(false); }
		return(::GetFileAttributesW(widen(_sPath).c_str()) != INVALID_FILE_ATTRIBUTES);
	}

	bool C_WIN32_FILE_SYSTEM::IsRegularFile(const std::string& _sPath) const
	{
		if (_sPath.empty()) { return(false); }
		const DWORD nAttributes = ::GetFileAttributesW(widen(_sPath).c_str());
		if (nAttributes == INVALID_FILE_ATTRIBUTES) { return(false); }
		return((nAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0);
	}

	bool C_WIN32_FILE_SYSTEM::IsSymlink(const std::string& _sPath) const
	{
		// GetFileAttributes 의 재해석 지점 판정은 링크를 따라가는지가 문서상 모호하므로
		// 링크 자신을 여는 경로로 확정한다. 파이썬 os.lstat 도 이름 대리(name surrogate)
		// 태그만 심볼릭 링크로 보므로(마운트 지점 포함) 같은 술어를 쓴다.
		if (_sPath.empty()) { return(false); }

		const HANDLE hFile = ::CreateFileW(
			widen(_sPath).c_str(),
			FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
			nullptr);
		if (hFile == INVALID_HANDLE_VALUE) { return(false); }

		FILE_ATTRIBUTE_TAG_INFO Info = {};
		const BOOL bQueried = ::GetFileInformationByHandleEx(hFile, FileAttributeTagInfo, &Info, sizeof(Info));
		::CloseHandle(hFile);
		if (bQueried == FALSE) { return(false); }

		if ((Info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) { return(false); }
		return(IsReparseTagNameSurrogate(Info.ReparseTag) != FALSE);
	}

	bool C_WIN32_FILE_SYSTEM::CreateDirectories(const std::string& _sPath)
	{
		// Path.mkdir(parents=True, exist_ok=True) 이식. 이미 디렉터리면 성공이고 같은 자리에
		// 일반 파일이 있으면 실패다 - 원본도 exist_ok 를 디렉터리일 때만 봐준다.
		if (_sPath.empty() || _sPath == ".") { return(true); }

		const std::wstring sWide = widen(_sPath);
		if (is_directory(sWide)) { return(true); }

		const std::string sParent = parent_of(_sPath);
		if (!sParent.empty() && !this->CreateDirectories(sParent)) { return(false); }

		if (::CreateDirectoryW(sWide.c_str(), nullptr) != FALSE) { return(true); }

		const DWORD nCode = ::GetLastError();
		if (nCode == ERROR_ALREADY_EXISTS && is_directory(sWide)) { return(true); }
		this->set_error_("디렉터리 생성", _sPath, nCode);
		return(false);
	}

	bool C_WIN32_FILE_SYSTEM::Replace(const std::string& _sFrom, const std::string& _sTo)
	{
		// 파이썬 os.replace 는 Windows 에서 MoveFileExW(MOVEFILE_REPLACE_EXISTING) 이다.
		// 실측(2026-08-16): 대상이 없어도 성공하고(ReplaceFileW 는 여기서 실패한다), 대상이
		// 다른 핸들로 열려 있으면 오류 5, 원본이 열려 있으면 오류 32 다. 플래그를 늘리면
		// (MOVEFILE_WRITE_THROUGH 등) 내구성 동작이 원본과 달라지므로 이 조합을 유지한다.
		if (::MoveFileExW(widen(_sFrom).c_str(), widen(_sTo).c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE)
		{
			return(true);
		}
		this->set_error_("원자적 교체", _sFrom + " -> " + _sTo, ::GetLastError());
		return(false);
	}

	bool C_WIN32_FILE_SYSTEM::Remove(const std::string& _sPath)
	{
		if (::DeleteFileW(widen(_sPath).c_str()) != FALSE) { return(true); }

		const DWORD nCode = ::GetLastError();
		// missing_ok=True 라 없는 것은 성공이다.
		if (nCode == ERROR_FILE_NOT_FOUND || nCode == ERROR_PATH_NOT_FOUND) { return(true); }
		this->set_error_("삭제", _sPath, nCode);
		return(false);
	}

	bool C_WIN32_FILE_SYSTEM::CreateUniqueTemporaryPath(
		const std::string& _sDirectory,
		const std::string& _sPrefix,
		const std::string& _sSuffix,
		std::string*       _psPath)
	{
		// tempfile.mkstemp 뒤에 close + unlink 까지가 원본의 한 연산이다(:531~540). 배타 생성이
		// 성공한 이름만 돌려주므로 그 시점까지의 유일성은 확인된 것이고, 돌려준 뒤에 다른
		// 프로세스가 같은 이름을 쓸 수 있다는 점도 원본과 같다.
		const std::string sDirectory = (_sDirectory.empty() || _sDirectory == ".") ? std::string(".") : _sDirectory;
		const bool        bNeedsSeparator = !is_separator(sDirectory.back());
		const std::size_t nCharacters = std::char_traits<char>::length(TEMPORARY_NAME_CHARACTERS);
		std::uniform_int_distribution<std::size_t> Distribution(0, nCharacters - 1);

		for (int nAttempt = 0; nAttempt < TEMPORARY_NAME_ATTEMPTS; ++nAttempt)
		{
			std::string sName = _sPrefix;
			for (int i = 0; i < TEMPORARY_NAME_LENGTH; ++i)
			{
				sName.push_back(TEMPORARY_NAME_CHARACTERS[Distribution(m_Random)]);
			}
			sName += _sSuffix;

			const std::string sCandidate = sDirectory + (bNeedsSeparator ? "\\" : "") + sName;
			const HANDLE      hFile      = ::CreateFileW(
				widen(sCandidate).c_str(),
				GENERIC_WRITE,
				0,
				nullptr,
				CREATE_NEW,
				FILE_ATTRIBUTE_NORMAL,
				nullptr);
			if (hFile == INVALID_HANDLE_VALUE)
			{
				const DWORD nCode = ::GetLastError();
				if (nCode == ERROR_FILE_EXISTS || nCode == ERROR_ALREADY_EXISTS) { continue; }
				this->set_error_("임시 파일 생성", sCandidate, nCode);
				return(false);
			}

			::CloseHandle(hFile);
			if (::DeleteFileW(widen(sCandidate).c_str()) == FALSE)
			{
				this->set_error_("임시 파일 삭제", sCandidate, ::GetLastError());
				return(false);
			}

			*_psPath = sCandidate;
			return(true);
		}

		this->set_error_("임시 파일 이름 확보", sDirectory, ERROR_FILE_EXISTS);
		return(false);
	}

	bool C_WIN32_FILE_SYSTEM::ModifiedTimeUs(const std::string& _sPath, std::int64_t* _pnValueUs) const
	{
		WIN32_FILE_ATTRIBUTE_DATA Data = {};
		if (::GetFileAttributesExW(widen(_sPath).c_str(), GetFileExInfoStandard, &Data) == FALSE)
		{
			this->set_error_("수정 시각 조회", _sPath, ::GetLastError());
			return(false);
		}

		ULARGE_INTEGER Value = {};
		Value.LowPart  = Data.ftLastWriteTime.dwLowDateTime;
		Value.HighPart = Data.ftLastWriteTime.dwHighDateTime;

		// FILETIME 은 1601-01-01 부터의 100 나노초다. 원본은 st_mtime 실수를 datetime 으로
		// 바꿔 쓰므로 계약 정밀도는 마이크로초이고, 여기서 잘리는 것은 그 아래 자리뿐이다.
		const std::int64_t n100Nanoseconds =
			static_cast<std::int64_t>(Value.QuadPart) - 116444736000000000LL;
		*_pnValueUs = n100Nanoseconds / 10;
		return(true);
	}

	bool C_WIN32_FILE_SYSTEM::ListDirectory(
		const std::string& _sDirectory, std::vector<std::string>* _pNames) const
	{
		_pNames->clear();

		const std::string sDirectory = (_sDirectory.empty() || _sDirectory == ".") ? std::string(".") : _sDirectory;
		const std::string sPattern   = sDirectory + (is_separator(sDirectory.back()) ? "" : "\\") + "*";

		WIN32_FIND_DATAW Found = {};
		const HANDLE     hFind = ::FindFirstFileW(widen(sPattern).c_str(), &Found);
		if (hFind == INVALID_HANDLE_VALUE)
		{
			const DWORD nCode = ::GetLastError();
			// Path.glob 은 없는 디렉터리에서 아무것도 내지 않고 오류도 올리지 않는다.
			if (nCode == ERROR_FILE_NOT_FOUND || nCode == ERROR_PATH_NOT_FOUND) { return(true); }
			this->set_error_("디렉터리 열거", sDirectory, nCode);
			return(false);
		}

		do
		{
			const std::string sName = narrow(Found.cFileName);
			if (sName == "." || sName == "..") { continue; }
			_pNames->push_back(sName);
		} while (::FindNextFileW(hFind, &Found) != FALSE);

		const DWORD nCode = ::GetLastError();
		::FindClose(hFind);
		if (nCode != ERROR_NO_MORE_FILES)
		{
			this->set_error_("디렉터리 열거", sDirectory, nCode);
			return(false);
		}
		return(true);
	}
}
