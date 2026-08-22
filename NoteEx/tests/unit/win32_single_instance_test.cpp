#include <catch_amalgamated.hpp>

#include "pynote/platform/win32_single_instance.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
	using C_SINGLE_INSTANCE = pynote::platform::C_WIN32_SINGLE_INSTANCE;
	using E_ACQUIRE_RESULT = C_SINGLE_INSTANCE::E_ACQUIRE_RESULT;

	std::atomic<unsigned long> g_nSequence{ 0 };

	std::wstring unique_suffix()
	{
		return(L"w3_i1_" + std::to_wstring(::GetCurrentProcessId()) + L"_" +
			std::to_wstring(::GetTickCount64()) + L"_" + std::to_wstring(++g_nSequence));
	}

	std::filesystem::path database_path(const std::wstring& _sParent, const std::wstring& _sName = L"one.sqlite3")
	{
		wchar_t TempPath[32768] = {};
		const DWORD nLength = ::GetTempPathW(static_cast<DWORD>(std::size(TempPath)), TempPath);
		return(std::filesystem::path(std::wstring(TempPath, nLength)) /
			(L"NoteEx-W3-I1-test-" + unique_suffix()) / _sParent / _sName);
	}

	bool wait_count(const std::atomic<int>& _nCount, const int _nExpected, const DWORD _nMilliseconds = 1000)
	{
		const ULONGLONG nDeadline = ::GetTickCount64() + _nMilliseconds;
		while (::GetTickCount64() < nDeadline)
		{
			if (_nCount.load() == _nExpected) { return(true); }
			::Sleep(5);
		}
		return(_nCount.load() == _nExpected);
	}

	HANDLE open_pipe(const std::wstring& _sPipeName, const DWORD _nAccess = GENERIC_WRITE)
	{
		const ULONGLONG nDeadline = ::GetTickCount64() + 500;
		for (;;)
		{
			const HANDLE hPipe = ::CreateFileW(
				_sPipeName.c_str(), _nAccess, 0, nullptr, OPEN_EXISTING, 0, nullptr);
			if (hPipe != INVALID_HANDLE_VALUE) { return(hPipe); }
			if (::GetTickCount64() >= nDeadline) { return(INVALID_HANDLE_VALUE); }
			::Sleep(5);
		}
	}

	bool send_fragments(const std::wstring& _sPipeName, const std::vector<std::string_view>& _Fragments)
	{
		const HANDLE hPipe = open_pipe(_sPipeName);
		if (hPipe == INVALID_HANDLE_VALUE) { return(false); }
		bool bResult = true;
		for (const std::string_view sFragment : _Fragments)
		{
			DWORD nWritten = 0;
			if (::WriteFile(
				hPipe, sFragment.data(), static_cast<DWORD>(sFragment.size()), &nWritten, nullptr) == FALSE ||
				nWritten != static_cast<DWORD>(sFragment.size()))
			{
				bResult = false;
				break;
			}
		}
		::CloseHandle(hPipe);
		return(bResult);
	}

	std::vector<std::uint8_t> current_user_sid()
	{
		HANDLE hToken = nullptr;
		if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &hToken) == FALSE)
		{
			return(std::vector<std::uint8_t>{});
		}
		DWORD nRequired = 0;
		::GetTokenInformation(hToken, TokenUser, nullptr, 0, &nRequired);
		std::vector<std::uint8_t> TokenBuffer(nRequired);
		if (nRequired == 0 ||
			::GetTokenInformation(hToken, TokenUser, TokenBuffer.data(), nRequired, &nRequired) == FALSE)
		{
			::CloseHandle(hToken);
			return(std::vector<std::uint8_t>{});
		}
		::CloseHandle(hToken);
		const auto* pTokenUser = reinterpret_cast<const TOKEN_USER*>(TokenBuffer.data());
		const DWORD nSid = ::GetLengthSid(pTokenUser->User.Sid);
		std::vector<std::uint8_t> Sid(nSid);
		if (::CopySid(nSid, Sid.data(), pTokenUser->User.Sid) == FALSE)
		{
			return(std::vector<std::uint8_t>{});
		}
		return(Sid);
	}

	bool has_current_user_only_protected_dacl(const HANDLE _hObject, const SE_OBJECT_TYPE _eObjectType)
	{
		PACL pDacl = nullptr;
		PSECURITY_DESCRIPTOR pDescriptor = nullptr;
		const DWORD nStatus = ::GetSecurityInfo(
			_hObject, _eObjectType, DACL_SECURITY_INFORMATION,
			nullptr, nullptr, &pDacl, nullptr, &pDescriptor);
		if (nStatus != ERROR_SUCCESS || !pDescriptor || !pDacl)
		{
			if (pDescriptor) { ::LocalFree(pDescriptor); }
			return(false);
		}

		SECURITY_DESCRIPTOR_CONTROL nControl = 0;
		DWORD nRevision = 0;
		const bool bProtected = ::GetSecurityDescriptorControl(
			pDescriptor, &nControl, &nRevision) != FALSE && (nControl & SE_DACL_PROTECTED) != 0;
		ACL_SIZE_INFORMATION AclInformation{};
		const bool bOneAce = ::GetAclInformation(
			pDacl, &AclInformation, sizeof(AclInformation), AclSizeInformation) != FALSE &&
			AclInformation.AceCount == 1;
		void* pAce = nullptr;
		const std::vector<std::uint8_t> Sid = current_user_sid();
		bool bCurrentUser = false;
		if (bOneAce && !Sid.empty() && ::GetAce(pDacl, 0, &pAce) != FALSE)
		{
			const auto* pAllowed = static_cast<const ACCESS_ALLOWED_ACE*>(pAce);
			bCurrentUser = pAllowed->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
				::EqualSid(const_cast<std::uint8_t*>(Sid.data()),
					const_cast<DWORD*>(&pAllowed->SidStart)) != FALSE;
		}
		::LocalFree(pDescriptor);
		return(bProtected && bOneAce && bCurrentUser);
	}

	std::pair<std::wstring, std::wstring> instance_names(const std::filesystem::path& _Database)
	{
		C_SINGLE_INSTANCE Probe;
		if (Probe.Acquire(_Database.native()) != E_ACQUIRE_RESULT::Primary)
		{
			return(std::pair<std::wstring, std::wstring>{});
		}
		const std::pair<std::wstring, std::wstring> Names{ Probe.MutexName(), Probe.PipeName() };
		Probe.Close();
		return(Names);
	}
}

TEST_CASE("W3-I1-001 startup database arguments and parent identity are exact",
	"[W3-single-instance][WTL-CAP-FI-002][WTL-CAP-FI-004][WTL-CAP-PL-001]")
{
	const auto Database = database_path(L"유니코드-부모");
	std::wstring sDatabase = Database.native();
	wchar_t sProgram[] = L"NoteEx.exe";
	wchar_t sDatabaseOption[] = L"--database";
	std::vector<wchar_t> DatabaseBuffer(sDatabase.begin(), sDatabase.end());
	DatabaseBuffer.push_back(L'\0');
	wchar_t* Arguments[] = { sProgram, sDatabaseOption, DatabaseBuffer.data() };
	pynote::platform::S_WIN32_STARTUP_OPTIONS Options;
	std::wstring sError;
	REQUIRE(pynote::platform::ParseWin32StartupOptions(3, Arguments, &Options, &sError));
	REQUIRE(Options.sDatabasePath == sDatabase);
	std::wstring sEqualsArgument = L"--database=" + sDatabase;
	std::vector<wchar_t> EqualsBuffer(sEqualsArgument.begin(), sEqualsArgument.end());
	EqualsBuffer.push_back(L'\0');
	wchar_t* EqualsArguments[] = { sProgram, EqualsBuffer.data() };
	REQUIRE(pynote::platform::ParseWin32StartupOptions(2, EqualsArguments, &Options, &sError));
	REQUIRE(Options.sDatabasePath == sDatabase);

	std::wstring sParentOne;
	std::wstring sParentTwo;
	std::wstring sParentOther;
	std::string sIdentityOne;
	std::string sIdentityTwo;
	std::string sIdentityOther;
	REQUIRE(pynote::platform::MakeWin32InstanceIdentity(
		Database.native(), &sParentOne, &sIdentityOne, &sError));
	REQUIRE(pynote::platform::MakeWin32InstanceIdentity(
		(Database.parent_path() / L"two.sqlite3").native(), &sParentTwo, &sIdentityTwo, &sError));
	REQUIRE(pynote::platform::MakeWin32InstanceIdentity(
		(Database.parent_path().parent_path() / L"other" / L"one.sqlite3").native(),
		&sParentOther, &sIdentityOther, &sError));
	REQUIRE(sParentOne == sParentTwo);
	REQUIRE(sIdentityOne == sIdentityTwo);
	REQUIRE(sIdentityOne != sIdentityOther);
	std::wstring sUpperDatabase = Database.native();
	std::transform(sUpperDatabase.begin(), sUpperDatabase.end(), sUpperDatabase.begin(),
		[](const wchar_t ch) { return(static_cast<wchar_t>(::towupper(ch))); });
	std::wstring sUpperParent;
	std::string sUpperIdentity;
	REQUIRE(pynote::platform::MakeWin32InstanceIdentity(
		sUpperDatabase, &sUpperParent, &sUpperIdentity, &sError));
	REQUIRE(sUpperIdentity == sIdentityOne);
	REQUIRE(sIdentityOne.size() == 24);
	REQUIRE(std::all_of(sIdentityOne.begin(), sIdentityOne.end(), [](const char ch)
	{
		return((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'));
	}));
	std::wstring sKnownParent;
	std::string sKnownIdentity;
	REQUIRE(pynote::platform::MakeWin32InstanceIdentity(
		L"C:\\W3-I1\\Parent\\one.sqlite3", &sKnownParent, &sKnownIdentity, &sError));
	REQUIRE(sKnownParent == L"c:\\w3-i1\\parent");
	REQUIRE(sKnownIdentity == "dc88454b6d79373dd38572b2");

	std::wstring sDefault;
	REQUIRE(pynote::platform::ResolveWin32DefaultDatabasePath(&sDefault, &sError));
	REQUIRE(std::filesystem::path(sDefault).filename() == L"pynote.sqlite3");
	REQUIRE(std::filesystem::path(sDefault).parent_path().filename() == L"pyNote");
	wchar_t* DefaultArguments[] = { sProgram };
	REQUIRE(pynote::platform::ParseWin32StartupOptions(1, DefaultArguments, &Options, &sError));
	REQUIRE(Options.sDatabasePath == sDefault);

	wchar_t sUnknown[] = L"--unknown";
	wchar_t* Unknown[] = { sProgram, sUnknown };
	REQUIRE_FALSE(pynote::platform::ParseWin32StartupOptions(2, Unknown, &Options, &sError));
	wchar_t* Missing[] = { sProgram, sDatabaseOption };
	REQUIRE_FALSE(pynote::platform::ParseWin32StartupOptions(2, Missing, &Options, &sError));
	wchar_t* Repeated[] = { sProgram, sDatabaseOption, DatabaseBuffer.data(), sDatabaseOption, DatabaseBuffer.data() };
	REQUIRE_FALSE(pynote::platform::ParseWin32StartupOptions(5, Repeated, &Options, &sError));
}

TEST_CASE("W3-I1-002 mutex and pipe use the current-user protected DACL", "[W3-single-instance]")
{
	C_SINGLE_INSTANCE Primary;
	REQUIRE(Primary.Acquire(database_path(L"security").native()) == E_ACQUIRE_RESULT::Primary);
	const HANDLE hMutex = ::OpenMutexW(READ_CONTROL, FALSE, Primary.MutexName().c_str());
	REQUIRE(hMutex != nullptr);
	REQUIRE(has_current_user_only_protected_dacl(hMutex, SE_KERNEL_OBJECT));
	::CloseHandle(hMutex);

	const HANDLE hPipe = open_pipe(Primary.PipeName(), GENERIC_WRITE | READ_CONTROL);
	REQUIRE(hPipe != INVALID_HANDLE_VALUE);
	REQUIRE(has_current_user_only_protected_dacl(hPipe, SE_FILE_OBJECT));
	::CloseHandle(hPipe);
	Primary.Close();
}

TEST_CASE("W3-I1-003 one primary routes a secondary new-window request", "[W3-single-instance][WTL-CAP-FI-005]")
{
	const auto Database = database_path(L"routing");
	C_SINGLE_INSTANCE Primary;
	REQUIRE(Primary.Acquire(Database.native()) == E_ACQUIRE_RESULT::Primary);

	C_SINGLE_INSTANCE Secondary;
	REQUIRE(Secondary.Acquire((Database.parent_path() / L"two.sqlite3").native()) ==
		E_ACQUIRE_RESULT::SecondaryNotified);
	std::atomic<int> nRequests{ 0 };
	Primary.SetNewWindowHandler([&nRequests]() { ++nRequests; });
	REQUIRE(wait_count(nRequests, 1));
	REQUIRE(Primary.LastError().empty());
	Secondary.Close();
	Primary.Close();
}

TEST_CASE("W3-I1-004 partial and unknown newline frames are handled exactly", "[W3-single-instance][WTL-CAP-PL-003]")
{
	C_SINGLE_INSTANCE Primary;
	REQUIRE(Primary.Acquire(database_path(L"framing").native()) == E_ACQUIRE_RESULT::Primary);
	std::atomic<int> nRequests{ 0 };
	Primary.SetNewWindowHandler([&nRequests]() { ++nRequests; });
	REQUIRE(send_fragments(Primary.PipeName(), { "new-window-extra\n" }));
	REQUIRE(send_fragments(Primary.PipeName(), { "new-", "win", "dow\n" }));
	REQUIRE(wait_count(nRequests, 1));
	REQUIRE(nRequests.load() == 1);
	Primary.Close();
}

TEST_CASE("W3-I1-005 slow live owner retries while unavailable live owner fails closed",
	"[W3-single-instance][WTL-CAP-FI-007][WTL-CAP-PL-002]")
{
	const auto SlowDatabase = database_path(L"slow-owner");
	const auto SlowNames = instance_names(SlowDatabase);
	REQUIRE_FALSE(SlowNames.first.empty());
	const HANDLE hSlowMutex = ::CreateMutexW(nullptr, TRUE, SlowNames.first.c_str());
	REQUIRE(hSlowMutex != nullptr);
	std::atomic<bool> bSlowServerRead{ false };
	std::thread SlowServer([&]()
	{
		::Sleep(40);
		const HANDLE hPipe = ::CreateNamedPipeW(
			SlowNames.second.c_str(), PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
			1, 0, 128, 0, nullptr);
		if (hPipe == INVALID_HANDLE_VALUE) { return; }
		const ULONGLONG nDeadline = ::GetTickCount64() + 1000;
		bool bConnected = false;
		while (::GetTickCount64() < nDeadline)
		{
			const BOOL bConnectResult = ::ConnectNamedPipe(hPipe, nullptr);
			const DWORD nConnectError = bConnectResult ? ERROR_SUCCESS : ::GetLastError();
			if (bConnectResult != FALSE || nConnectError == ERROR_PIPE_CONNECTED)
			{
				bConnected = true;
				break;
			}
			if (nConnectError != ERROR_PIPE_LISTENING) { break; }
			::Sleep(5);
		}
		std::array<char, 32> Buffer{};
		DWORD nRead = 0;
		while (bConnected && ::GetTickCount64() < nDeadline)
		{
			const BOOL bReadResult = ::ReadFile(
				hPipe, Buffer.data(), static_cast<DWORD>(Buffer.size()), &nRead, nullptr);
			const DWORD nReadError = bReadResult ? ERROR_SUCCESS : ::GetLastError();
			if (bReadResult != FALSE)
			{
				bSlowServerRead = std::string_view(Buffer.data(), nRead) == "new-window\n";
				break;
			}
			if (nReadError != ERROR_NO_DATA) { break; }
			::Sleep(5);
		}
		::DisconnectNamedPipe(hPipe);
		::CloseHandle(hPipe);
	});
	C_SINGLE_INSTANCE SlowSecondary;
	const E_ACQUIRE_RESULT eSlow = SlowSecondary.Acquire(SlowDatabase.native());
	SlowServer.join();
	REQUIRE(eSlow == E_ACQUIRE_RESULT::SecondaryNotified);
	REQUIRE(bSlowServerRead.load());
	::CloseHandle(hSlowMutex);

	const auto UnavailableDatabase = database_path(L"unavailable-owner");
	const auto UnavailableNames = instance_names(UnavailableDatabase);
	REQUIRE_FALSE(UnavailableNames.first.empty());
	const HANDLE hUnavailableMutex = ::CreateMutexW(nullptr, TRUE, UnavailableNames.first.c_str());
	REQUIRE(hUnavailableMutex != nullptr);
	C_SINGLE_INSTANCE Unavailable;
	const ULONGLONG nStarted = ::GetTickCount64();
	REQUIRE(Unavailable.Acquire(UnavailableDatabase.native()) == E_ACQUIRE_RESULT::Failure);
	REQUIRE(::GetTickCount64() - nStarted < 1000);
	REQUIRE_FALSE(Unavailable.LastError().empty());
	const HANDLE hStillOwned = ::OpenMutexW(SYNCHRONIZE, FALSE, UnavailableNames.first.c_str());
	REQUIRE(hStillOwned != nullptr);
	::CloseHandle(hStillOwned);
	::CloseHandle(hUnavailableMutex);
	C_SINGLE_INSTANCE Reacquired;
	REQUIRE(Reacquired.Acquire(UnavailableDatabase.native()) == E_ACQUIRE_RESULT::Primary);
	Reacquired.Close();
}

TEST_CASE("W3-I1-006 close releases server and mutex for reacquisition", "[W3-single-instance][WTL-CAP-PL-026]")
{
	const auto Database = database_path(L"reacquire");
	C_SINGLE_INSTANCE First;
	REQUIRE(First.Acquire(Database.native()) == E_ACQUIRE_RESULT::Primary);
	const HANDLE hIdleClient = open_pipe(First.PipeName());
	REQUIRE(hIdleClient != INVALID_HANDLE_VALUE);
	const ULONGLONG nStarted = ::GetTickCount64();
	First.Close();
	REQUIRE(::GetTickCount64() - nStarted < 1000);
	First.Close();
	::CloseHandle(hIdleClient);

	C_SINGLE_INSTANCE Second;
	REQUIRE(Second.Acquire(Database.native()) == E_ACQUIRE_RESULT::Primary);
	Second.Close();
}
