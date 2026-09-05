#include "pynote/platform/win32_file_binding_support.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>

namespace
{
	// FILETIME 의 epoch(1601-01-01) 와 Unix epoch 의 차이를 100나노초 단위로 센 값이다.
	const std::int64_t FILETIME_EPOCH_DELTA = 116444736000000000LL;

	std::wstring widen(std::string_view _sText)
	{
		if (_sText.empty()) { return(std::wstring{}); }
		const int nSize = ::MultiByteToWideChar(
			CP_UTF8, MB_ERR_INVALID_CHARS, _sText.data(), static_cast<int>(_sText.size()), nullptr, 0);
		if (nSize <= 0) { return(std::wstring{}); }

		std::wstring sResult(static_cast<std::size_t>(nSize), L'\0');
		::MultiByteToWideChar(
			CP_UTF8, MB_ERR_INVALID_CHARS, _sText.data(), static_cast<int>(_sText.size()), sResult.data(), nSize);
		return(sResult);
	}

	std::string narrow(const std::wstring& _sText)
	{
		if (_sText.empty()) { return(std::string{}); }
		const int nSize = ::WideCharToMultiByte(
			CP_UTF8, 0, _sText.data(), static_cast<int>(_sText.size()), nullptr, 0, nullptr, nullptr);
		if (nSize <= 0) { return(std::string{}); }

		std::string sResult(static_cast<std::size_t>(nSize), '\0');
		::WideCharToMultiByte(
			CP_UTF8, 0, _sText.data(), static_cast<int>(_sText.size()), sResult.data(), nSize, nullptr, nullptr);
		return(sResult);
	}

	bool is_separator(char _ch)
	{
		return(_ch == '\\' || _ch == '/');
	}

	// 대상 경로에서 부모 디렉터리를 잘라낸다. 임시 파일은 대상과 같은 볼륨에 있어야 교체가
	// 원자적이므로 이 판정이 platform 소유다. 부모가 없으면 현재 디렉터리다.
	std::string parent_of(const std::string& _sPath)
	{
		std::size_t nEnd = _sPath.size();
		while (nEnd > 0 && is_separator(_sPath[nEnd - 1])) { --nEnd; }

		for (std::size_t i = nEnd; i > 0; --i)
		{
			if (!is_separator(_sPath[i - 1])) { continue; }
			std::string sParent = _sPath.substr(0, i - 1);
			// "D:" 나 "\\\\server" 처럼 더 오를 수 없는 자리는 구분자를 남긴다.
			if (sParent.empty() || (sParent.size() == 2 && sParent[1] == ':'))
			{
				return(_sPath.substr(0, i));
			}
			return(sParent);
		}
		return(std::string("."));
	}

	std::string name_of(const std::string& _sPath)
	{
		std::size_t nEnd = _sPath.size();
		while (nEnd > 0 && is_separator(_sPath[nEnd - 1])) { --nEnd; }

		for (std::size_t i = nEnd; i > 0; --i)
		{
			if (is_separator(_sPath[i - 1])) { return(_sPath.substr(i, nEnd - i)); }
		}
		return(_sPath.substr(0, nEnd));
	}

	// GetFinalPathNameByHandleW 로 디스크의 실제 표기를 얻는다. 파이썬 realpath 가 부르는
	// _getfinalpathname 과 같은 API 다.
	bool final_path_of(const std::wstring& _sPath, std::wstring* _psOut)
	{
		const HANDLE hFile = ::CreateFileW(
			_sPath.c_str(),
			0,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS,
			nullptr);
		if (hFile == INVALID_HANDLE_VALUE) { return(false); }

		const DWORD nNeeded = ::GetFinalPathNameByHandleW(hFile, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
		if (nNeeded == 0) { ::CloseHandle(hFile); return(false); }

		std::wstring sBuffer(static_cast<std::size_t>(nNeeded), L'\0');
		const DWORD  nWritten = ::GetFinalPathNameByHandleW(
			hFile, sBuffer.data(), nNeeded, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
		::CloseHandle(hFile);
		if (nWritten == 0 || nWritten >= nNeeded) { return(false); }
		sBuffer.resize(static_cast<std::size_t>(nWritten));

		// 원본도 `\\?\` 접두를 벗겨서 돌려준다(ntpath.realpath).
		if (sBuffer.rfind(L"\\\\?\\UNC\\", 0) == 0)
		{
			*_psOut = L"\\\\" + sBuffer.substr(8);
			return(true);
		}
		if (sBuffer.rfind(L"\\\\?\\", 0) == 0)
		{
			*_psOut = sBuffer.substr(4);
			return(true);
		}
		*_psOut = sBuffer;
		return(true);
	}

	// 원본 _getfinalpathname_nonstrict 의 되감기 루프다 - 존재하는 가장 긴 접두를 실제 표기로
	// 접고, 남은 꼬리는 입력 표기 그대로 붙인다.
	std::wstring resolve_wide(const std::wstring& _sFullPath)
	{
		std::wstring sHead = _sFullPath;
		std::wstring sTail;
		while (!sHead.empty())
		{
			std::wstring sResolved;
			if (final_path_of(sHead, &sResolved))
			{
				if (sTail.empty()) { return(sResolved); }
				if (!sResolved.empty() && sResolved.back() != L'\\') { sResolved.push_back(L'\\'); }
				return(sResolved + sTail);
			}

			const std::size_t nPos = sHead.find_last_of(L'\\');
			if (nPos == std::wstring::npos) { break; }
			const std::wstring sName = sHead.substr(nPos + 1);
			if (sName.empty()) { break; }
			sTail = sTail.empty() ? sName : sName + L"\\" + sTail;
			sHead = sHead.substr(0, nPos);
			if (sHead.size() == 2 && sHead[1] == L':') { sHead.push_back(L'\\'); }
		}
		return(_sFullPath);
	}

	// 원본 os.path.normcase 의 Windows 구현이다(ntpath: '/' -> '\\' 뒤
	// LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE)).
	std::wstring normcase_wide(const std::wstring& _sText)
	{
		std::wstring sReplaced = _sText;
		for (wchar_t& ch : sReplaced)
		{
			if (ch == L'/') { ch = L'\\'; }
		}
		if (sReplaced.empty()) { return(sReplaced); }

		const int nNeeded = ::LCMapStringEx(
			LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
			sReplaced.data(), static_cast<int>(sReplaced.size()), nullptr, 0, nullptr, nullptr, 0);
		if (nNeeded <= 0) { return(sReplaced); }

		std::wstring sLowered(static_cast<std::size_t>(nNeeded), L'\0');
		const int nWritten = ::LCMapStringEx(
			LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
			sReplaced.data(), static_cast<int>(sReplaced.size()),
			sLowered.data(), nNeeded, nullptr, nullptr, 0);
		if (nWritten <= 0) { return(sReplaced); }
		sLowered.resize(static_cast<std::size_t>(nWritten));
		return(sLowered);
	}
}

namespace pynote::platform
{
	bool DecodeSystemAnsiStrict(std::span<const std::uint8_t> _Bytes, std::string* _psUtf8)
	{
		_psUtf8->clear();
		if (_Bytes.empty()) { return(true); }

		const char* pData = reinterpret_cast<const char*>(_Bytes.data());
		const int   nSize = static_cast<int>(_Bytes.size());
		const int   nWide = ::MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, pData, nSize, nullptr, 0);
		if (nWide <= 0) { return(false); }

		std::wstring sWide(static_cast<std::size_t>(nWide), L'\0');
		if (::MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, pData, nSize, sWide.data(), nWide) <= 0)
		{
			return(false);
		}
		*_psUtf8 = narrow(sWide);
		return(true);
	}

	bool EncodeSystemAnsiStrict(std::string_view _sUtf8Text, std::vector<std::uint8_t>* _pOut)
	{
		_pOut->clear();
		if (_sUtf8Text.empty()) { return(true); }

		const std::wstring sWide = widen(_sUtf8Text);
		if (sWide.empty()) { return(false); }

		const int nSize = ::WideCharToMultiByte(
			CP_ACP, WC_NO_BEST_FIT_CHARS, sWide.data(), static_cast<int>(sWide.size()),
			nullptr, 0, nullptr, nullptr);
		if (nSize <= 0) { return(false); }

		std::string sNarrow(static_cast<std::size_t>(nSize), '\0');
		BOOL        bUsedDefault = FALSE;
		const int   nWritten = ::WideCharToMultiByte(
			CP_ACP, WC_NO_BEST_FIT_CHARS, sWide.data(), static_cast<int>(sWide.size()),
			sNarrow.data(), nSize, nullptr, &bUsedDefault);
		if (nWritten <= 0) { return(false); }
		// 기본 문자가 한 번이라도 쓰였으면 표현할 수 없는 문자가 있었다는 뜻이다.
		if (bUsedDefault != FALSE) { return(false); }

		_pOut->assign(sNarrow.begin(), sNarrow.begin() + nWritten);
		return(true);
	}

	bool ResolveBindingPath(const std::string& _sUtf8Path, std::string* _psPath, std::string* _psPathKey)
	{
		const std::wstring sInput = widen(_sUtf8Path);
		if (sInput.empty()) { return(false); }

		// 원본은 상대 경로를 현재 디렉터리에 붙여 절대화한다. GetFullPathNameW 가 그 자리이며
		// '..' 축약과 '/' -> '\\' 변환까지 함께 한다.
		const DWORD nNeeded = ::GetFullPathNameW(sInput.c_str(), 0, nullptr, nullptr);
		if (nNeeded == 0) { return(false); }

		std::wstring sFull(static_cast<std::size_t>(nNeeded), L'\0');
		const DWORD  nWritten = ::GetFullPathNameW(sInput.c_str(), nNeeded, sFull.data(), nullptr);
		if (nWritten == 0 || nWritten >= nNeeded) { return(false); }
		sFull.resize(static_cast<std::size_t>(nWritten));

		const std::wstring sResolved = resolve_wide(sFull);
		*_psPath    = narrow(sResolved);
		*_psPathKey = narrow(normcase_wide(sResolved));
		return(true);
	}

	void C_WIN32_BINDING_FILE_SYSTEM::set_error_(
		const std::string& _sOperation, const std::string& _sPath, unsigned long _nCode) const
	{
		m_sLastError = _sOperation + reinterpret_cast<const char*>(u8" 실패(Win32 오류 ")
			+ std::to_string(_nCode) + "): " + _sPath;
	}

	void C_WIN32_BINDING_FILE_SYSTEM::take_error_() const
	{
		m_sLastError = m_Files.LastError();
	}

	bool C_WIN32_BINDING_FILE_SYSTEM::ReadAllBytes(
		const std::string& _sPath, std::vector<std::uint8_t>* _pBytes, bool* _pFound) const
	{
		_pBytes->clear();
		*_pFound = false;

		const HANDLE hFile = ::CreateFileW(
			widen(_sPath).c_str(),
			GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (hFile == INVALID_HANDLE_VALUE)
		{
			const DWORD nCode = ::GetLastError();
			// 원본 _read_bytes 는 FileNotFoundError 만 삼키고 나머지는 그대로 올린다.
			if (nCode == ERROR_FILE_NOT_FOUND || nCode == ERROR_PATH_NOT_FOUND) { return(true); }
			this->set_error_(reinterpret_cast<const char*>(u8"파일 읽기"), _sPath, nCode);
			return(false);
		}

		std::uint8_t buffer[64 * 1024];
		for (;;)
		{
			DWORD nRead = 0;
			if (::ReadFile(hFile, buffer, static_cast<DWORD>(sizeof(buffer)), &nRead, nullptr) == FALSE)
			{
				const DWORD nCode = ::GetLastError();
				::CloseHandle(hFile);
				this->set_error_(reinterpret_cast<const char*>(u8"파일 읽기"), _sPath, nCode);
				_pBytes->clear();
				return(false);
			}
			if (nRead == 0) { break; }
			_pBytes->insert(_pBytes->end(), buffer, buffer + nRead);
		}
		::CloseHandle(hFile);
		*_pFound = true;
		return(true);
	}

	bool C_WIN32_BINDING_FILE_SYSTEM::Stat(
		const std::string& _sPath, std::int64_t* _pnSize, std::int64_t* _pnMtimeNs) const
	{
		WIN32_FILE_ATTRIBUTE_DATA Data = {};
		if (::GetFileAttributesExW(widen(_sPath).c_str(), GetFileExInfoStandard, &Data) == FALSE)
		{
			this->set_error_(reinterpret_cast<const char*>(u8"파일 상태 조회"), _sPath, ::GetLastError());
			return(false);
		}

		*_pnSize = (static_cast<std::int64_t>(Data.nFileSizeHigh) << 32)
			| static_cast<std::int64_t>(Data.nFileSizeLow);

		const std::int64_t nFileTime = (static_cast<std::int64_t>(Data.ftLastWriteTime.dwHighDateTime) << 32)
			| static_cast<std::int64_t>(Data.ftLastWriteTime.dwLowDateTime);
		// 원본 st_mtime_ns 와 같은 단위다. FILETIME 은 100나노초 눈금이다.
		*_pnMtimeNs = (nFileTime - FILETIME_EPOCH_DELTA) * 100;
		return(true);
	}

	bool C_WIN32_BINDING_FILE_SYSTEM::CreateUniqueTemporaryPathFor(
		const std::string& _sTargetPath, std::string* _psPath)
	{
		// 원본 mkstemp(dir=path.parent, prefix=".<name>.", suffix=".tmp") 그대로다.
		const std::string sDirectory = parent_of(_sTargetPath);
		const std::string sPrefix    = "." + name_of(_sTargetPath) + ".";
		if (!m_Files.CreateUniqueTemporaryPath(sDirectory, sPrefix, ".tmp", _psPath))
		{
			this->take_error_();
			return(false);
		}
		return(true);
	}

	bool C_WIN32_BINDING_FILE_SYSTEM::WriteAllBytes(
		const std::string& _sPath, std::span<const std::uint8_t> _Bytes)
	{
		const HANDLE hFile = ::CreateFileW(
			widen(_sPath).c_str(),
			GENERIC_WRITE,
			0,
			nullptr,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (hFile == INVALID_HANDLE_VALUE)
		{
			this->set_error_(reinterpret_cast<const char*>(u8"파일 기록"), _sPath, ::GetLastError());
			return(false);
		}

		std::size_t nOffset = 0;
		while (nOffset < _Bytes.size())
		{
			const DWORD nWanted = static_cast<DWORD>(
				(_Bytes.size() - nOffset) > 0x10000000u ? 0x10000000u : (_Bytes.size() - nOffset));
			DWORD nWritten = 0;
			if (::WriteFile(hFile, _Bytes.data() + nOffset, nWanted, &nWritten, nullptr) == FALSE || nWritten == 0)
			{
				const DWORD nCode = ::GetLastError();
				::CloseHandle(hFile);
				this->set_error_(reinterpret_cast<const char*>(u8"파일 기록"), _sPath, nCode);
				return(false);
			}
			nOffset += nWritten;
		}
		::CloseHandle(hFile);
		return(true);
	}

	bool C_WIN32_BINDING_FILE_SYSTEM::Replace(const std::string& _sFrom, const std::string& _sTo)
	{
		if (!m_Files.Replace(_sFrom, _sTo)) { this->take_error_(); return(false); }
		return(true);
	}

	bool C_WIN32_BINDING_FILE_SYSTEM::Remove(const std::string& _sPath)
	{
		if (!m_Files.Remove(_sPath)) { this->take_error_(); return(false); }
		return(true);
	}
}
