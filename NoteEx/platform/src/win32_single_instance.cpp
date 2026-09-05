#include "pynote/platform/win32_single_instance.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <shlobj.h>

// windows.h 의 CreateEvent 매크로는 repositories.h 의 멤버 이름을 바꾼다 - 결속 경로 확정을
// 위해 core 를 끌고 오는 아래 헤더보다 먼저 걷는다(CDocumentPage.cpp·w3_shell_consumer_test.cpp
// 의 순서 계약과 같다). 이 TU 는 ATL/WTL 을 읽지 않으므로 앞에서 걷어도 잃을 식별자가 없다.
#ifdef CreateEvent
#undef CreateEvent
#endif

#include "pynote/platform/win32_file_binding_support.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
	constexpr wchar_t MUTEX_PREFIX[] = L"Local\\pyNote.NoteEx.";
	constexpr wchar_t PIPE_PREFIX[] = L"\\\\.\\pipe\\pyNote.NoteEx.";
	constexpr char NEW_WINDOW_FRAME[] = "new-window";
	constexpr char NEW_WINDOW_COMMAND[] = "new-window\n";
	// 원본 _OPEN_FILE_PREFIX(app.py:60). 탭·개행이 든 파일명이 줄 프로토콜을 깨지 않게
	// 뒤에 base64url 을 싣는다.
	constexpr std::string_view OPEN_FILE_PREFIX = "open-file\t";
	// 경로 32767자 x UTF-8 최대 3바이트 -> base64 약 131 KiB 라 한 프레임을 그만큼 받는다.
	// 명명 파이프 버퍼(CreateNamedPipeW 의 4096)는 바이트 모드의 힌트일 뿐이라 함께 올리지 않는다.
	constexpr std::size_t MAX_FRAME_BYTES = 140000;
	constexpr char BASE64URL_ALPHABET[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

	// 원본 base64.urlsafe_b64encode(app.py:167). 패딩 '=' 를 유지한다.
	std::string base64url_encode(const std::string& _sBytes)
	{
		std::string sResult;
		sResult.reserve((_sBytes.size() + 2) / 3 * 4);
		std::size_t i = 0;
		while (i + 2 < _sBytes.size())
		{
			const std::uint32_t nGroup =
				(static_cast<std::uint8_t>(_sBytes[i]) << 16) |
				(static_cast<std::uint8_t>(_sBytes[i + 1]) << 8) |
				static_cast<std::uint8_t>(_sBytes[i + 2]);
			sResult.push_back(BASE64URL_ALPHABET[(nGroup >> 18) & 0x3F]);
			sResult.push_back(BASE64URL_ALPHABET[(nGroup >> 12) & 0x3F]);
			sResult.push_back(BASE64URL_ALPHABET[(nGroup >> 6) & 0x3F]);
			sResult.push_back(BASE64URL_ALPHABET[nGroup & 0x3F]);
			i += 3;
		}
		const std::size_t nRemaining = _sBytes.size() - i;
		if (nRemaining == 0) { return(sResult); }
		std::uint32_t nGroup = static_cast<std::uint8_t>(_sBytes[i]) << 16;
		if (nRemaining == 2) { nGroup |= static_cast<std::uint8_t>(_sBytes[i + 1]) << 8; }
		sResult.push_back(BASE64URL_ALPHABET[(nGroup >> 18) & 0x3F]);
		sResult.push_back(BASE64URL_ALPHABET[(nGroup >> 12) & 0x3F]);
		sResult.push_back(nRemaining == 2 ? BASE64URL_ALPHABET[(nGroup >> 6) & 0x3F] : '=');
		sResult.push_back('=');
		return(sResult);
	}

	int base64url_value(const char _ch)
	{
		if (_ch >= 'A' && _ch <= 'Z') { return(_ch - 'A'); }
		if (_ch >= 'a' && _ch <= 'z') { return(_ch - 'a' + 26); }
		if (_ch >= '0' && _ch <= '9') { return(_ch - '0' + 52); }
		if (_ch == '-') { return(62); }
		if (_ch == '_') { return(63); }
		return(-1);
	}

	// 원본 base64.urlsafe_b64decode(app.py:270). 알파벳 밖 문자를 버리는 것까지는 원본
	// (validate=False)과 같고 - "%%%" 가 오류가 아니라 빈 경로가 되는 갈래가 이 순서에서
	// 나온다(원본은 그 빈 값을 :271 에서 걸러낸다) - 패딩 판정은 원본보다 엄격하다(원본은 유효
	// 패딩열에서 조기 종료한다). 제품 클라이언트가 만들지 않는 이형이라 좁게 둔다(P2 감사 1-2).
	bool base64url_decode(const std::string_view _sEncoded, std::string* _psBytes)
	{
		std::string sFiltered;
		sFiltered.reserve(_sEncoded.size());
		for (const char ch : _sEncoded)
		{
			if (ch == '=' || base64url_value(ch) >= 0) { sFiltered.push_back(ch); }
		}
		if (sFiltered.size() % 4 != 0) { return(false); }
		_psBytes->clear();
		for (std::size_t i = 0; i < sFiltered.size(); i += 4)
		{
			int Values[4] = { 0, 0, 0, 0 };
			int nData = 0;
			for (int j = 0; j < 4; ++j)
			{
				const char ch = sFiltered[i + j];
				if (ch == '=')
				{
					// 패딩은 마지막 그룹의 꼬리에서만 성립한다.
					if (i + 4 != sFiltered.size() || j < 2) { return(false); }
					continue;
				}
				if (nData != j) { return(false); }
				Values[j] = base64url_value(ch);
				++nData;
			}
			if (nData < 2) { return(false); }
			const std::uint32_t nGroup = (static_cast<std::uint32_t>(Values[0]) << 18) |
				(static_cast<std::uint32_t>(Values[1]) << 12) |
				(static_cast<std::uint32_t>(Values[2]) << 6) |
				static_cast<std::uint32_t>(Values[3]);
			_psBytes->push_back(static_cast<char>((nGroup >> 16) & 0xFF));
			if (nData >= 3) { _psBytes->push_back(static_cast<char>((nGroup >> 8) & 0xFF)); }
			if (nData == 4) { _psBytes->push_back(static_cast<char>(nGroup & 0xFF)); }
		}
		return(true);
	}

	std::wstring win32_error(const std::wstring_view _sAction, const DWORD _nCode)
	{
		return(std::wstring(_sAction) + L" (Win32 " + std::to_wstring(_nCode) + L")");
	}

	bool lower_invariant(const std::wstring& _sSource, std::wstring* _psResult)
	{
		if (_sSource.empty()) { _psResult->clear(); return(true); }
		if (_sSource.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) { return(false); }
		const int nSource = static_cast<int>(_sSource.size());
		const int nRequired = ::LCMapStringEx(
			LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, _sSource.data(), nSource,
			nullptr, 0, nullptr, nullptr, 0);
		if (nRequired <= 0) { return(false); }
		_psResult->assign(static_cast<std::size_t>(nRequired), L'\0');
		return(::LCMapStringEx(
			LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, _sSource.data(), nSource,
			_psResult->data(), nRequired, nullptr, nullptr, 0) == nRequired);
	}

	bool utf8(const std::wstring& _sSource, std::string* _psResult)
	{
		if (_sSource.empty()) { _psResult->clear(); return(true); }
		if (_sSource.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) { return(false); }
		const int nSource = static_cast<int>(_sSource.size());
		const int nRequired = ::WideCharToMultiByte(
			CP_UTF8, WC_ERR_INVALID_CHARS, _sSource.data(), nSource, nullptr, 0, nullptr, nullptr);
		if (nRequired <= 0) { return(false); }
		_psResult->assign(static_cast<std::size_t>(nRequired), '\0');
		return(::WideCharToMultiByte(
			CP_UTF8, WC_ERR_INVALID_CHARS, _sSource.data(), nSource,
			_psResult->data(), nRequired, nullptr, nullptr) == nRequired);
	}

	// 원본 bytes.decode("utf-8") strict 자리다(app.py:270). 실패하면 프레임을 버린다.
	bool wide_strict(const std::string& _sSource, std::wstring* _psResult)
	{
		if (_sSource.empty()) { _psResult->clear(); return(true); }
		if (_sSource.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) { return(false); }
		const int nSource = static_cast<int>(_sSource.size());
		const int nRequired = ::MultiByteToWideChar(
			CP_UTF8, MB_ERR_INVALID_CHARS, _sSource.data(), nSource, nullptr, 0);
		if (nRequired <= 0) { return(false); }
		_psResult->assign(static_cast<std::size_t>(nRequired), L'\0');
		return(::MultiByteToWideChar(
			CP_UTF8, MB_ERR_INVALID_CHARS, _sSource.data(), nSource,
			_psResult->data(), nRequired) == nRequired);
	}

	bool full_path(const std::wstring& _sPath, std::wstring* _psResult, std::wstring* _psError)
	{
		if (_sPath.empty()) { *_psError = L"database path is empty"; return(false); }
		const DWORD nRequired = ::GetFullPathNameW(_sPath.c_str(), 0, nullptr, nullptr);
		if (nRequired == 0) { *_psError = win32_error(L"database path resolution failed", ::GetLastError()); return(false); }
		std::wstring sBuffer(static_cast<std::size_t>(nRequired), L'\0');
		const DWORD nLength = ::GetFullPathNameW(_sPath.c_str(), nRequired, sBuffer.data(), nullptr);
		if (nLength == 0 || nLength >= nRequired)
		{
			*_psError = win32_error(L"database path resolution failed", ::GetLastError());
			return(false);
		}
		sBuffer.resize(nLength);
		*_psResult = std::move(sBuffer);
		return(true);
	}

	bool sha256_prefix(const std::string& _sBytes, std::string* _psIdentity, std::wstring* _psError)
	{
		BCRYPT_ALG_HANDLE hAlgorithm = nullptr;
		BCRYPT_HASH_HANDLE hHash = nullptr;
		std::vector<UCHAR> Object;
		std::array<UCHAR, 32> Digest{};
		bool bResult = false;
		do
		{
			if (::BCryptOpenAlgorithmProvider(&hAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
			{
				*_psError = L"SHA-256 provider open failed";
				break;
			}
			DWORD nObject = 0;
			DWORD nRead = 0;
			if (::BCryptGetProperty(
				hAlgorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&nObject), sizeof(nObject), &nRead, 0) < 0)
			{
				*_psError = L"SHA-256 object query failed";
				break;
			}
			Object.resize(nObject);
			if (::BCryptCreateHash(hAlgorithm, &hHash, Object.data(), nObject, nullptr, 0, 0) < 0)
			{
				*_psError = L"SHA-256 hash creation failed";
				break;
			}
			if (_sBytes.size() > static_cast<std::size_t>(std::numeric_limits<ULONG>::max()))
			{
				*_psError = L"database parent is too long to hash";
				break;
			}
			if (::BCryptHashData(
				hHash, reinterpret_cast<PUCHAR>(const_cast<char*>(_sBytes.data())),
				static_cast<ULONG>(_sBytes.size()), 0) < 0 ||
				::BCryptFinishHash(hHash, Digest.data(), static_cast<ULONG>(Digest.size()), 0) < 0)
			{
				*_psError = L"SHA-256 hashing failed";
				break;
			}
			static constexpr char DIGITS[] = "0123456789abcdef";
			_psIdentity->clear();
			_psIdentity->reserve(24);
			for (std::size_t i = 0; i < 12; ++i)
			{
				_psIdentity->push_back(DIGITS[Digest[i] >> 4]);
				_psIdentity->push_back(DIGITS[Digest[i] & 0x0f]);
			}
			bResult = true;
		} while (false);
		if (hHash) { ::BCryptDestroyHash(hHash); }
		if (hAlgorithm) { ::BCryptCloseAlgorithmProvider(hAlgorithm, 0); }
		return(bResult);
	}

	class C_CURRENT_USER_SECURITY
	{
	public:
		~C_CURRENT_USER_SECURITY() { if (m_pAcl) { ::LocalFree(m_pAcl); } }

		bool Initialize(std::wstring* _psError)
		{
			if (m_pAcl) { ::LocalFree(m_pAcl); m_pAcl = nullptr; }
			m_TokenUser.clear();
			m_Descriptor = {};
			m_Attributes = {};
			HANDLE hToken = nullptr;
			if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &hToken) == FALSE)
			{
				*_psError = win32_error(L"process token open failed", ::GetLastError());
				return(false);
			}
			DWORD nRequired = 0;
			::GetTokenInformation(hToken, TokenUser, nullptr, 0, &nRequired);
			if (nRequired == 0)
			{
				const DWORD nCode = ::GetLastError();
				::CloseHandle(hToken);
				*_psError = win32_error(L"current user SID size query failed", nCode);
				return(false);
			}
			m_TokenUser.resize(nRequired);
			if (::GetTokenInformation(hToken, TokenUser, m_TokenUser.data(), nRequired, &nRequired) == FALSE)
			{
				const DWORD nCode = ::GetLastError();
				::CloseHandle(hToken);
				*_psError = win32_error(L"current user SID query failed", nCode);
				return(false);
			}
			::CloseHandle(hToken);

			auto* pTokenUser = reinterpret_cast<TOKEN_USER*>(m_TokenUser.data());
			EXPLICIT_ACCESSW Access{};
			Access.grfAccessPermissions = GENERIC_ALL;
			Access.grfAccessMode = SET_ACCESS;
			Access.grfInheritance = NO_INHERITANCE;
			Access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
			Access.Trustee.TrusteeType = TRUSTEE_IS_USER;
			Access.Trustee.ptstrName = static_cast<LPWSTR>(pTokenUser->User.Sid);
			const DWORD nAcl = ::SetEntriesInAclW(1, &Access, nullptr, &m_pAcl);
			if (nAcl != ERROR_SUCCESS)
			{
				*_psError = win32_error(L"current-user DACL creation failed", nAcl);
				return(false);
			}
			if (::InitializeSecurityDescriptor(&m_Descriptor, SECURITY_DESCRIPTOR_REVISION) == FALSE ||
				::SetSecurityDescriptorDacl(&m_Descriptor, TRUE, m_pAcl, FALSE) == FALSE ||
				::SetSecurityDescriptorControl(
					&m_Descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED) == FALSE)
			{
				*_psError = win32_error(L"protected security descriptor creation failed", ::GetLastError());
				return(false);
			}
			m_Attributes.nLength = sizeof(m_Attributes);
			m_Attributes.lpSecurityDescriptor = &m_Descriptor;
			m_Attributes.bInheritHandle = FALSE;
			return(true);
		}

		SECURITY_ATTRIBUTES* Attributes() { return(&m_Attributes); }

	private:
		std::vector<std::uint8_t> m_TokenUser;
		PACL m_pAcl{ nullptr };
		SECURITY_DESCRIPTOR m_Descriptor{};
		SECURITY_ATTRIBUTES m_Attributes{};
	};

	enum class E_NOTIFY_RESULT
	{
		Notified,
		Unavailable,
		Denied,
	};

	E_NOTIFY_RESULT notify_pipe_once(
		const std::wstring& _sPipeName, const std::string& _sCommand, DWORD* _pnError)
	{
		const HANDLE hPipe = ::CreateFileW(
			_sPipeName.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		if (hPipe == INVALID_HANDLE_VALUE)
		{
			*_pnError = ::GetLastError();
			if (*_pnError == ERROR_FILE_NOT_FOUND || *_pnError == ERROR_PIPE_BUSY)
			{
				return(E_NOTIFY_RESULT::Unavailable);
			}
			return(E_NOTIFY_RESULT::Denied);
		}
		DWORD nWritten = 0;
		const BOOL bWritten = ::WriteFile(
			hPipe, _sCommand.data(), static_cast<DWORD>(_sCommand.size()), &nWritten, nullptr);
		const bool bCompleteWrite = bWritten != FALSE &&
			nWritten == static_cast<DWORD>(_sCommand.size());
		BOOL bFlushed = FALSE;
		if (bCompleteWrite) { bFlushed = ::FlushFileBuffers(hPipe); }
		if (bWritten == FALSE) { *_pnError = ::GetLastError(); }
		else if (!bCompleteWrite) { *_pnError = ERROR_WRITE_FAULT; }
		else if (bFlushed == FALSE) { *_pnError = ::GetLastError(); }
		else { *_pnError = ERROR_SUCCESS; }
		::CloseHandle(hPipe);
		return(bCompleteWrite && bFlushed != FALSE
			? E_NOTIFY_RESULT::Notified : E_NOTIFY_RESULT::Denied);
	}

	bool wait_overlapped(HANDLE _hStop, HANDLE _hObject, OVERLAPPED* _pOverlapped, DWORD* _pnBytes)
	{
		HANDLE Handles[] = { _hStop, _pOverlapped->hEvent };
		const DWORD nWait = ::WaitForMultipleObjects(2, Handles, FALSE, INFINITE);
		if (nWait != WAIT_OBJECT_0 + 1)
		{
			::CancelIoEx(_hObject, _pOverlapped);
			DWORD nCancelled = 0;
			::GetOverlappedResult(_hObject, _pOverlapped, &nCancelled, TRUE);
			return(false);
		}
		return(::GetOverlappedResult(_hObject, _pOverlapped, _pnBytes, FALSE) != FALSE);
	}
}

namespace pynote::platform
{
	struct C_WIN32_SINGLE_INSTANCE::S_STATE
	{
		std::wstring sDatabaseParent;
		std::string sIdentity;
		std::wstring sMutexName;
		std::wstring sPipeName;

		mutable std::mutex ErrorMutex;
		std::wstring sLastError;
		std::mutex HandlerMutex;
		NEW_WINDOW_HANDLER Handler;
		std::size_t nPendingRequests{ 0 };
		OPEN_FILE_HANDLER OpenFileHandler;
		std::vector<std::wstring> PendingPaths;

		C_CURRENT_USER_SECURITY Security;
		HANDLE hMutex{ nullptr };
		HANDLE hStop{ nullptr };
		HANDLE hInitialPipe{ INVALID_HANDLE_VALUE };
		std::thread ServerThread;

		void SetError(std::wstring _sError)
		{
			const std::lock_guard Lock(ErrorMutex);
			sLastError = std::move(_sError);
		}

		HANDLE CreatePipe()
		{
			return(::CreateNamedPipeW(
				sPipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
				PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
				PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, Security.Attributes()));
		}

		void DispatchNewWindow()
		{
			NEW_WINDOW_HANDLER Copy;
			{
				const std::lock_guard Lock(HandlerMutex);
				if (!Handler) { ++nPendingRequests; return; }
				Copy = Handler;
			}
			try { Copy(); }
			catch (...) { SetError(L"new-window handler threw an exception"); }
		}

		void DispatchOpenFile(std::wstring _sPath)
		{
			OPEN_FILE_HANDLER Copy;
			{
				const std::lock_guard Lock(HandlerMutex);
				if (!OpenFileHandler) { PendingPaths.push_back(std::move(_sPath)); return; }
				Copy = OpenFileHandler;
			}
			try { Copy(std::move(_sPath)); }
			catch (...) { SetError(L"open-file handler threw an exception"); }
		}

		// 원본 _handle_command(app.py:258~272). 미지 프레임은 무시하고, base64 해독 실패·
		// UTF-8 해독 실패·빈 경로도 무시한다(원본은 ValueError 한 갈래가 두 오류를 함께 삼킨다).
		void DispatchFrame(const std::string& _sFrame)
		{
			if (_sFrame == NEW_WINDOW_FRAME) { this->DispatchNewWindow(); return; }
			const std::string_view sFrame(_sFrame);
			if (!sFrame.starts_with(OPEN_FILE_PREFIX)) { return; }
			std::string sDecoded;
			if (!base64url_decode(sFrame.substr(OPEN_FILE_PREFIX.size()), &sDecoded)) { return; }
			if (sDecoded.empty()) { return; }
			std::wstring sPath;
			if (!wide_strict(sDecoded, &sPath) || sPath.empty()) { return; }
			this->DispatchOpenFile(std::move(sPath));
		}

		void ServeClient(HANDLE _hPipe)
		{
			std::string sFrame;
			// 상한에 닿은 프레임은 그 프레임만 버리고 다음 개행까지 소비한다 - 한 연결에 여러
			// 줄이 실리므로 연결을 통째로 접으면 앞의 과대 프레임이 뒤의 정상 경로를 죽인다.
			bool bDropping = false;
			std::array<char, 4> Buffer{};
			for (;;)
			{
				OVERLAPPED Overlapped{};
				Overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
				if (!Overlapped.hEvent) { SetError(win32_error(L"pipe read event creation failed", ::GetLastError())); return; }
				DWORD nRead = 0;
				const BOOL bImmediate = ::ReadFile(
					_hPipe, Buffer.data(), static_cast<DWORD>(Buffer.size()), &nRead, &Overlapped);
				bool bRead = (bImmediate != FALSE);
				if (!bRead && ::GetLastError() == ERROR_IO_PENDING)
				{
					bRead = wait_overlapped(hStop, _hPipe, &Overlapped, &nRead);
				}
				::CloseHandle(Overlapped.hEvent);
				if (!bRead || nRead == 0) { return; }
				for (DWORD i = 0; i < nRead; ++i)
				{
					const char ch = Buffer[i];
					if (ch == '\n')
					{
						// 원본 _read_client(app.py:248~256) - 연결의 모든 줄을 처리한다.
						if (!bDropping) { this->DispatchFrame(sFrame); }
						sFrame.clear();
						bDropping = false;
						continue;
					}
					if (bDropping) { continue; }
					if (sFrame.size() == MAX_FRAME_BYTES)
					{
						bDropping = true;
						sFrame.clear();
						continue;
					}
					sFrame.push_back(ch);
				}
			}
		}

		void ServerLoop()
		{
			HANDLE hPipe = hInitialPipe;
			hInitialPipe = INVALID_HANDLE_VALUE;
			while (::WaitForSingleObject(hStop, 0) != WAIT_OBJECT_0)
			{
				if (hPipe == INVALID_HANDLE_VALUE)
				{
					hPipe = CreatePipe();
					if (hPipe == INVALID_HANDLE_VALUE)
					{
						SetError(win32_error(L"named pipe recreation failed", ::GetLastError()));
						return;
					}
				}

				OVERLAPPED Overlapped{};
				Overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
				if (!Overlapped.hEvent)
				{
					SetError(win32_error(L"pipe connection event creation failed", ::GetLastError()));
					::CloseHandle(hPipe);
					return;
				}
				DWORD nTransferred = 0;
				BOOL bConnected = ::ConnectNamedPipe(hPipe, &Overlapped);
				if (!bConnected)
				{
					const DWORD nCode = ::GetLastError();
					if (nCode == ERROR_PIPE_CONNECTED) { bConnected = TRUE; }
					else if (nCode == ERROR_IO_PENDING)
					{
						bConnected = wait_overlapped(hStop, hPipe, &Overlapped, &nTransferred) ? TRUE : FALSE;
					}
				}
				::CloseHandle(Overlapped.hEvent);
				if (::WaitForSingleObject(hStop, 0) == WAIT_OBJECT_0)
				{
					::CloseHandle(hPipe);
					return;
				}
				if (bConnected) { ServeClient(hPipe); }
				::DisconnectNamedPipe(hPipe);
				::CloseHandle(hPipe);
				hPipe = INVALID_HANDLE_VALUE;
			}
			if (hPipe != INVALID_HANDLE_VALUE) { ::CloseHandle(hPipe); }
		}
	};

	bool ResolveWin32DefaultDatabasePath(std::wstring* _psDatabasePath, std::wstring* _psError)
	{
		if (!_psDatabasePath || !_psError) { return(false); }
		PWSTR pRoaming = nullptr;
		const HRESULT nResult = ::SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &pRoaming);
		if (FAILED(nResult) || !pRoaming)
		{
			*_psError = L"RoamingAppData path resolution failed";
			return(false);
		}
		try
		{
			*_psDatabasePath = (std::filesystem::path(pRoaming) / L"pyNote" / L"pyNote" / L"pynote.sqlite3").native();
		}
		catch (...)
		{
			::CoTaskMemFree(pRoaming);
			*_psError = L"default database path construction failed";
			return(false);
		}
		::CoTaskMemFree(pRoaming);
		_psError->clear();
		return(true);
	}

	bool ParseWin32StartupOptions(
		const int _nArgumentCount, wchar_t* const* _ppArguments,
		S_WIN32_STARTUP_OPTIONS* _pOptions, std::wstring* _psError)
	{
		if (!_pOptions || !_psError || _nArgumentCount < 1 || !_ppArguments)
		{
			if (_psError) { *_psError = L"invalid command line input"; }
			return(false);
		}
		_pOptions->sDatabasePath.clear();
		// 기존 W3-I1-001 이 옵션 구조체 하나를 여섯 번 재사용하므로 진입 시 비운다
		// (sDatabasePath 와 같은 관례).
		_pOptions->Paths.clear();
		_psError->clear();
		bool bDatabaseSeen = false;
		for (int i = 1; i < _nArgumentCount; ++i)
		{
			const std::wstring_view sArgument(_ppArguments[i] ? _ppArguments[i] : L"");
			// 원본 argparse 의 positional paths 다. 하이픈으로 시작하는 토큰은 여전히 옵션
			// 자리이므로(app.py:1143~1162) --database 계열이 아니면 오류로 남는다.
			if (!sArgument.starts_with(L'-'))
			{
				// 원본은 경로 확정 실패를 기동 실패로 올리지 않는다(빈 인자도 창을 세우고 경고만
				// 낸다, app.py:96·:702~711) - 변환·확정에 실패한 인자만 버리고 나머지를 연다
				// (P2 감사 1-4).
				std::string sUtf8Argument;
				if (!utf8(std::wstring(sArgument), &sUtf8Argument)) { continue; }
				std::string sResolved;
				std::string sPathKey;
				if (!ResolveBindingPath(sUtf8Argument, &sResolved, &sPathKey)) { continue; }
				std::wstring sWideResolved;
				if (!wide_strict(sResolved, &sWideResolved)) { continue; }
				_pOptions->Paths.push_back(std::move(sWideResolved));
				continue;
			}
			std::wstring sValue;
			if (sArgument == L"--database")
			{
				if (bDatabaseSeen || i + 1 >= _nArgumentCount || !_ppArguments[i + 1] ||
					_ppArguments[i + 1][0] == L'\0' || std::wstring_view(_ppArguments[i + 1]).starts_with(L"--"))
				{
					*_psError = L"--database requires exactly one non-empty path";
					return(false);
				}
				sValue = _ppArguments[++i];
			}
			else if (sArgument.starts_with(L"--database="))
			{
				if (bDatabaseSeen || sArgument.size() == std::wstring_view(L"--database=").size())
				{
					*_psError = L"--database requires exactly one non-empty path";
					return(false);
				}
				sValue.assign(sArgument.substr(std::wstring_view(L"--database=").size()));
			}
			else
			{
				*_psError = L"unknown command-line argument";
				return(false);
			}
			bDatabaseSeen = true;
			_pOptions->sDatabasePath = std::move(sValue);
		}
		if (!bDatabaseSeen)
		{
			return(ResolveWin32DefaultDatabasePath(&_pOptions->sDatabasePath, _psError));
		}
		return(true);
	}

	bool MakeWin32InstanceIdentity(
		const std::wstring& _sDatabasePath, std::wstring* _psNormalizedParent,
		std::string* _psIdentity, std::wstring* _psError)
	{
		if (!_psNormalizedParent || !_psIdentity || !_psError) { return(false); }
		std::wstring sFullPath;
		if (!full_path(_sDatabasePath, &sFullPath, _psError)) { return(false); }
		std::wstring sParent;
		try { sParent = std::filesystem::path(sFullPath).parent_path().lexically_normal().native(); }
		catch (...) { *_psError = L"database parent normalization failed"; return(false); }
		if (sParent.empty()) { *_psError = L"database path has no parent"; return(false); }
		if (!lower_invariant(sParent, _psNormalizedParent))
		{
			*_psError = win32_error(L"database parent case normalization failed", ::GetLastError());
			return(false);
		}
		std::string sUtf8;
		if (!utf8(*_psNormalizedParent, &sUtf8))
		{
			*_psError = win32_error(L"database parent UTF-8 conversion failed", ::GetLastError());
			return(false);
		}
		return(sha256_prefix(sUtf8, _psIdentity, _psError));
	}

	C_WIN32_SINGLE_INSTANCE::C_WIN32_SINGLE_INSTANCE() : m_pState(std::make_unique<S_STATE>()) {}
	C_WIN32_SINGLE_INSTANCE::~C_WIN32_SINGLE_INSTANCE() { this->Close(); }

	C_WIN32_SINGLE_INSTANCE::E_ACQUIRE_RESULT C_WIN32_SINGLE_INSTANCE::Acquire(
		const std::wstring& _sDatabasePath, const std::vector<std::wstring>& _Paths)
	{
		this->Close();
		m_pState->SetError(L"");
		std::wstring sIdentityError;
		if (!MakeWin32InstanceIdentity(
			_sDatabasePath, &m_pState->sDatabaseParent, &m_pState->sIdentity, &sIdentityError))
		{
			m_pState->SetError(std::move(sIdentityError));
			return(E_ACQUIRE_RESULT::Failure);
		}
		const std::wstring sIdentity(m_pState->sIdentity.begin(), m_pState->sIdentity.end());
		m_pState->sMutexName = std::wstring(MUTEX_PREFIX) + sIdentity + L".mutex";
		m_pState->sPipeName = std::wstring(PIPE_PREFIX) + sIdentity;

		// 원본 launch_message(app.py:162~170) - 경로가 있으면 경로마다 한 줄, 없으면 새 창 한 줄이다.
		std::string sCommand;
		for (const std::wstring& sPath : _Paths)
		{
			std::string sUtf8Path;
			if (!utf8(sPath, &sUtf8Path))
			{
				m_pState->SetError(L"launch path is not representable in UTF-8");
				return(E_ACQUIRE_RESULT::Failure);
			}
			sCommand.append(OPEN_FILE_PREFIX);
			sCommand.append(base64url_encode(sUtf8Path));
			sCommand.push_back('\n');
		}
		if (sCommand.empty()) { sCommand = NEW_WINDOW_COMMAND; }

		DWORD nNotifyError = ERROR_SUCCESS;
		E_NOTIFY_RESULT eNotify = notify_pipe_once(m_pState->sPipeName, sCommand, &nNotifyError);
		if (eNotify == E_NOTIFY_RESULT::Notified) { return(E_ACQUIRE_RESULT::SecondaryNotified); }
		if (eNotify == E_NOTIFY_RESULT::Denied)
		{
			m_pState->SetError(win32_error(L"primary notification failed", nNotifyError));
			return(E_ACQUIRE_RESULT::Failure);
		}

		std::wstring sSecurityError;
		if (!m_pState->Security.Initialize(&sSecurityError))
		{
			m_pState->SetError(std::move(sSecurityError));
			return(E_ACQUIRE_RESULT::Failure);
		}
		m_pState->hMutex = ::CreateMutexW(m_pState->Security.Attributes(), TRUE, m_pState->sMutexName.c_str());
		const DWORD nMutexResult = ::GetLastError();
		if (!m_pState->hMutex)
		{
			m_pState->SetError(win32_error(L"instance mutex creation failed", nMutexResult));
			return(E_ACQUIRE_RESULT::Failure);
		}
		if (nMutexResult == ERROR_ALREADY_EXISTS)
		{
			::CloseHandle(m_pState->hMutex);
			m_pState->hMutex = nullptr;
			for (const DWORD nDelay : { 25UL, 50UL })
			{
				::Sleep(nDelay);
				eNotify = notify_pipe_once(m_pState->sPipeName, sCommand, &nNotifyError);
				if (eNotify == E_NOTIFY_RESULT::Notified) { return(E_ACQUIRE_RESULT::SecondaryNotified); }
				if (eNotify == E_NOTIFY_RESULT::Denied) { break; }
			}
			m_pState->SetError(win32_error(L"live instance pipe is unavailable", nNotifyError));
			return(E_ACQUIRE_RESULT::Failure);
		}

		m_pState->hStop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!m_pState->hStop)
		{
			m_pState->SetError(win32_error(L"single-instance stop event creation failed", ::GetLastError()));
			this->Close();
			return(E_ACQUIRE_RESULT::Failure);
		}
		m_pState->hInitialPipe = m_pState->CreatePipe();
		if (m_pState->hInitialPipe == INVALID_HANDLE_VALUE)
		{
			m_pState->SetError(win32_error(L"named pipe creation failed", ::GetLastError()));
			this->Close();
			return(E_ACQUIRE_RESULT::Failure);
		}
		try { m_pState->ServerThread = std::thread([this]() { m_pState->ServerLoop(); }); }
		catch (...)
		{
			m_pState->SetError(L"named pipe server thread creation failed");
			this->Close();
			return(E_ACQUIRE_RESULT::Failure);
		}
		return(E_ACQUIRE_RESULT::Primary);
	}

	void C_WIN32_SINGLE_INSTANCE::SetNewWindowHandler(NEW_WINDOW_HANDLER _Handler)
	{
		std::size_t nPending = 0;
		NEW_WINDOW_HANDLER Copy;
		{
			const std::lock_guard Lock(m_pState->HandlerMutex);
			m_pState->Handler = std::move(_Handler);
			if (m_pState->Handler)
			{
				Copy = m_pState->Handler;
				nPending = std::exchange(m_pState->nPendingRequests, 0);
			}
		}
		for (std::size_t i = 0; i < nPending; ++i)
		{
			try { Copy(); }
			catch (...) { m_pState->SetError(L"new-window handler threw an exception"); }
		}
	}

	void C_WIN32_SINGLE_INSTANCE::SetOpenFileHandler(OPEN_FILE_HANDLER _Handler)
	{
		std::vector<std::wstring> Pending;
		OPEN_FILE_HANDLER Copy;
		{
			const std::lock_guard Lock(m_pState->HandlerMutex);
			m_pState->OpenFileHandler = std::move(_Handler);
			if (m_pState->OpenFileHandler)
			{
				Copy = m_pState->OpenFileHandler;
				Pending = std::exchange(m_pState->PendingPaths, {});
			}
		}
		for (std::wstring& sPath : Pending)
		{
			try { Copy(std::move(sPath)); }
			catch (...) { m_pState->SetError(L"open-file handler threw an exception"); }
		}
	}

	void C_WIN32_SINGLE_INSTANCE::Close()
	{
		if (!m_pState) { return; }
		if (m_pState->hStop) { ::SetEvent(m_pState->hStop); }
		if (m_pState->ServerThread.joinable()) { m_pState->ServerThread.join(); }
		if (m_pState->hInitialPipe != INVALID_HANDLE_VALUE)
		{
			::CloseHandle(m_pState->hInitialPipe);
			m_pState->hInitialPipe = INVALID_HANDLE_VALUE;
		}
		if (m_pState->hStop) { ::CloseHandle(m_pState->hStop); m_pState->hStop = nullptr; }
		if (m_pState->hMutex) { ::CloseHandle(m_pState->hMutex); m_pState->hMutex = nullptr; }
		const std::lock_guard Lock(m_pState->HandlerMutex);
		m_pState->Handler = {};
		m_pState->nPendingRequests = 0;
		m_pState->OpenFileHandler = {};
		m_pState->PendingPaths.clear();
	}

	std::wstring C_WIN32_SINGLE_INSTANCE::LastError() const
	{
		const std::lock_guard Lock(m_pState->ErrorMutex);
		return(m_pState->sLastError);
	}
	const std::wstring& C_WIN32_SINGLE_INSTANCE::DatabaseParent() const { return(m_pState->sDatabaseParent); }
	const std::string& C_WIN32_SINGLE_INSTANCE::Identity() const { return(m_pState->sIdentity); }
	const std::wstring& C_WIN32_SINGLE_INSTANCE::MutexName() const { return(m_pState->sMutexName); }
	const std::wstring& C_WIN32_SINGLE_INSTANCE::PipeName() const { return(m_pState->sPipeName); }
}
