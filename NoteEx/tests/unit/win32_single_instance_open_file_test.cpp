#include <catch_amalgamated.hpp>

#include "pynote/platform/win32_single_instance.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// P2 [FS-launch-port] 의 프로토콜·인자 시험이다. 기존 win32_single_instance_test.cpp 의
// 여섯 케이스는 무수정이며 여기의 대역·골격은 그 파일에서 그대로 가져왔다.
namespace
{
	using C_SINGLE_INSTANCE = pynote::platform::C_WIN32_SINGLE_INSTANCE;
	using E_ACQUIRE_RESULT = C_SINGLE_INSTANCE::E_ACQUIRE_RESULT;

	std::atomic<unsigned long> g_nSequence{ 0 };

	std::wstring unique_suffix()
	{
		return(L"w3_fb_" + std::to_wstring(::GetCurrentProcessId()) + L"_" +
			std::to_wstring(::GetTickCount64()) + L"_" + std::to_wstring(++g_nSequence));
	}

	std::filesystem::path database_path(
		const std::wstring& _sParent, const std::wstring& _sName = L"one.sqlite3")
	{
		wchar_t TempPath[32768] = {};
		const DWORD nLength = ::GetTempPathW(static_cast<DWORD>(std::size(TempPath)), TempPath);
		return(std::filesystem::path(std::wstring(TempPath, nLength)) /
			(L"NoteEx-W3-FB-test-" + unique_suffix()) / _sParent / _sName);
	}

	HANDLE open_pipe(const std::wstring& _sPipeName)
	{
		const ULONGLONG nDeadline = ::GetTickCount64() + 500;
		for (;;)
		{
			const HANDLE hPipe = ::CreateFileW(
				_sPipeName.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
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
		// 서버가 다 읽을 때까지 막는다 - 닫기가 접속보다 빠르면 ConnectNamedPipe 가
		// ERROR_NO_DATA 로 떨어져 그 연결의 프레임이 통째로 사라진다. 실제 보조 프로세스도
		// 같은 이유로 FlushFileBuffers 뒤에 닫는다(notify_pipe_once).
		if (bResult) { bResult = ::FlushFileBuffers(hPipe) != FALSE; }
		::CloseHandle(hPipe);
		return(bResult);
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

	template <typename PREDICATE>
	bool wait_for(const PREDICATE& _Predicate, const DWORD _nMilliseconds = 1000)
	{
		const ULONGLONG nDeadline = ::GetTickCount64() + _nMilliseconds;
		while (::GetTickCount64() < nDeadline)
		{
			if (_Predicate()) { return(true); }
			::Sleep(5);
		}
		return(_Predicate());
	}

	std::vector<wchar_t> argument_buffer(const std::wstring& _sValue)
	{
		std::vector<wchar_t> Buffer(_sValue.begin(), _sValue.end());
		Buffer.push_back(L'\0');
		return(Buffer);
	}

	// 살아 있는 소유자를 흉내 내 보조 프로세스가 실제로 쓴 바이트를 회수한다(기존
	// W3-I1-005 의 SlowServer 와 같은 골격이되 여기서는 페이로드를 그대로 돌려준다).
	std::string capture_launch_payload(
		const std::filesystem::path& _Database, const std::vector<std::wstring>& _Paths)
	{
		const auto Names = instance_names(_Database);
		if (Names.second.empty()) { return(std::string{}); }
		const HANDLE hPipe = ::CreateNamedPipeW(
			Names.second.c_str(), PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
			1, 0, 4096, 0, nullptr);
		if (hPipe == INVALID_HANDLE_VALUE) { return(std::string{}); }
		std::string sPayload;
		std::thread Server([&]()
		{
			const BOOL bConnected = ::ConnectNamedPipe(hPipe, nullptr);
			if (bConnected == FALSE && ::GetLastError() != ERROR_PIPE_CONNECTED) { return; }
			std::array<char, 4096> Buffer{};
			for (;;)
			{
				DWORD nRead = 0;
				if (::ReadFile(hPipe, Buffer.data(), static_cast<DWORD>(Buffer.size()), &nRead, nullptr) == FALSE ||
					nRead == 0)
				{
					return;
				}
				sPayload.append(Buffer.data(), nRead);
			}
		});
		C_SINGLE_INSTANCE Secondary;
		const E_ACQUIRE_RESULT eResult = Secondary.Acquire(_Database.native(), _Paths);
		Server.join();
		::DisconnectNamedPipe(hPipe);
		::CloseHandle(hPipe);
		Secondary.Close();
		if (eResult != E_ACQUIRE_RESULT::SecondaryNotified) { return(std::string{}); }
		return(sPayload);
	}

	// 시험이 스스로 계산하는 base64url 이다 - 구현과 다른 경로로 만든 기대값이어야 인코더
	// 결함을 잡는다. 패딩은 원본(base64.urlsafe_b64encode)대로 유지한다.
	std::string expected_base64url(const std::string& _sBytes)
	{
		static constexpr char ALPHABET[] =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
		std::string sBits;
		for (const char ch : _sBytes)
		{
			for (int nShift = 7; nShift >= 0; --nShift)
			{
				sBits.push_back(((static_cast<std::uint8_t>(ch) >> nShift) & 1) ? '1' : '0');
			}
		}
		std::string sResult;
		for (std::size_t i = 0; i < sBits.size(); i += 6)
		{
			std::string sChunk = sBits.substr(i, 6);
			while (sChunk.size() < 6) { sChunk.push_back('0'); }
			int nValue = 0;
			for (const char ch : sChunk) { nValue = nValue * 2 + (ch == '1' ? 1 : 0); }
			sResult.push_back(ALPHABET[nValue]);
		}
		while (sResult.size() % 4 != 0) { sResult.push_back('='); }
		return(sResult);
	}

	std::string utf8_of(const std::wstring& _sValue)
	{
		const int nRequired = ::WideCharToMultiByte(
			CP_UTF8, 0, _sValue.data(), static_cast<int>(_sValue.size()), nullptr, 0, nullptr, nullptr);
		std::string sResult(static_cast<std::size_t>(nRequired), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, _sValue.data(), static_cast<int>(_sValue.size()),
			sResult.data(), nRequired, nullptr, nullptr);
		return(sResult);
	}

	std::string open_file_line(const std::wstring& _sPath)
	{
		return("open-file\t" + expected_base64url(utf8_of(_sPath)) + "\n");
	}

	// 파이프 스레드가 채우고 시험 스레드가 읽으므로 잠금 없이 벡터를 공유하지 않는다.
	class C_PATH_RECORDER
	{
	public:
		void Record(std::wstring _sPath)
		{
			const std::lock_guard Lock(m_Mutex);
			m_Paths.push_back(std::move(_sPath));
		}

		std::size_t Count() const
		{
			const std::lock_guard Lock(m_Mutex);
			return(m_Paths.size());
		}

		std::vector<std::wstring> Snapshot() const
		{
			const std::lock_guard Lock(m_Mutex);
			return(m_Paths);
		}

	private:
		mutable std::mutex m_Mutex;
		std::vector<std::wstring> m_Paths;
	};
}

TEST_CASE("W3-FB-101 startup arguments collect positional paths and keep hyphen tokens as options",
	"[W3-file-binding][FS-port]")
{
	const auto Database = database_path(L"인자");
	const auto First = Database.parent_path() / L"첫.txt";
	const auto Second = Database.parent_path() / L"둘째.txt";

	wchar_t sProgram[] = L"NoteEx.exe";
	wchar_t sDatabaseOption[] = L"--database";
	auto DatabaseBuffer = argument_buffer(Database.native());
	auto FirstBuffer = argument_buffer(First.native());
	auto SecondBuffer = argument_buffer(Second.native());
	wchar_t* Arguments[] = {
		sProgram, sDatabaseOption, DatabaseBuffer.data(), FirstBuffer.data(), SecondBuffer.data() };
	pynote::platform::S_WIN32_STARTUP_OPTIONS Options;
	std::wstring sError;
	REQUIRE(pynote::platform::ParseWin32StartupOptions(5, Arguments, &Options, &sError));
	REQUIRE(Options.sDatabasePath == Database.native());
	REQUIRE(Options.Paths.size() == 2);
	// 순서 보존 + 결속 경로 확정(절대 경로)이다.
	REQUIRE(std::filesystem::path(Options.Paths[0]).filename() == L"첫.txt");
	REQUIRE(std::filesystem::path(Options.Paths[1]).filename() == L"둘째.txt");
	REQUIRE(std::filesystem::path(Options.Paths[0]).is_absolute());

	// 진입 시 clear() 한다 - 같은 구조체를 재사용해도 앞 실행의 경로가 남지 않는다.
	wchar_t* DefaultArguments[] = { sProgram, sDatabaseOption, DatabaseBuffer.data() };
	REQUIRE(pynote::platform::ParseWin32StartupOptions(3, DefaultArguments, &Options, &sError));
	REQUIRE(Options.Paths.empty());

	// 상대 경로는 호출자의 현재 디렉터리 기준으로 확정된다(원본 app.py:167~168).
	wchar_t sRelative[] = L"상대.txt";
	wchar_t* Relative[] = { sProgram, sDatabaseOption, DatabaseBuffer.data(), sRelative };
	REQUIRE(pynote::platform::ParseWin32StartupOptions(4, Relative, &Options, &sError));
	REQUIRE(Options.Paths.size() == 1);
	REQUIRE(std::filesystem::path(Options.Paths[0]).is_absolute());
	REQUIRE(std::filesystem::path(Options.Paths[0]).filename() == L"상대.txt");

	// 확정 실패 인자(빈 문자열)는 그 인자만 버리고 나머지를 연다 - 기동 전체를 실패로 올리지
	// 않는다(원본 app.py:96·:702~711, P2 감사 1-4).
	wchar_t sEmptyArgument[] = L"";
	wchar_t* WithEmpty[] = { sProgram, sDatabaseOption, DatabaseBuffer.data(), sEmptyArgument, FirstBuffer.data() };
	REQUIRE(pynote::platform::ParseWin32StartupOptions(5, WithEmpty, &Options, &sError));
	REQUIRE(Options.Paths.size() == 1);
	REQUIRE(std::filesystem::path(Options.Paths[0]).filename() == L"첫.txt");

	// 하이픈으로 시작하는 토큰은 여전히 옵션 자리다(원본 argparse, app.py:1143~1162).
	wchar_t sShortOption[] = L"-x";
	wchar_t* Short[] = { sProgram, sShortOption };
	REQUIRE_FALSE(pynote::platform::ParseWin32StartupOptions(2, Short, &Options, &sError));
	wchar_t sUnknown[] = L"--unknown";
	wchar_t* Unknown[] = { sProgram, sUnknown };
	REQUIRE_FALSE(pynote::platform::ParseWin32StartupOptions(2, Unknown, &Options, &sError));
}

TEST_CASE("W3-FB-102 launch frames carry base64url paths with padding one line per path",
	"[W3-file-binding][FS-port][WTL-CAP-FB-013]")
{
	// 고정 벡터 - 패딩을 지우면 이 기대값이 깨진다.
	REQUIRE(expected_base64url("C:\\a.txt") == "QzpcYS50eHQ=");

	const auto Database = database_path(L"프레임");
	// 탭이 든 파일명은 줄 프로토콜을 깨는 반례다 - base64 로 감싼 계약을 못박는다.
	const std::vector<std::wstring> Paths{ L"C:\\a.txt", L"C:\\탭\t들어간.txt" };
	const std::string sPayload = capture_launch_payload(Database, Paths);
	REQUIRE(sPayload == open_file_line(Paths[0]) + open_file_line(Paths[1]));
	REQUIRE(sPayload.find("new-window") == std::string::npos);

	// 경로가 없으면 기존 새 창 명령 그대로다.
	REQUIRE(capture_launch_payload(database_path(L"프레임-없음"), {}) == "new-window\n");
}

TEST_CASE("W3-FB-103 one connection delivers every line and an over-limit frame drops only itself",
	"[W3-file-binding][FS-port]")
{
	C_SINGLE_INSTANCE Primary;
	REQUIRE(Primary.Acquire(database_path(L"연결").native()) == E_ACQUIRE_RESULT::Primary);
	C_PATH_RECORDER Recorder;
	std::atomic<int> nNewWindows{ 0 };
	Primary.SetOpenFileHandler([&Recorder](std::wstring _sPath) { Recorder.Record(std::move(_sPath)); });
	Primary.SetNewWindowHandler([&nNewWindows]() { ++nNewWindows; });

	const std::string sFirst = open_file_line(L"C:\\하나.txt");
	const std::string sSecond = open_file_line(L"C:\\둘.txt");
	// 상한(140000 바이트)에 닿는 프레임은 그 프레임만 버리고 뒤따르는 줄은 정상 처리한다.
	const std::string sOverLimit = "open-file\t" + std::string(140000, 'A') + "\n";
	const std::string sUnknown = "unknown-command\n";
	const std::string sNewWindow = "new-window\n";
	REQUIRE(send_fragments(Primary.PipeName(),
		{ sFirst, sOverLimit, sSecond, sUnknown, sNewWindow }));
	REQUIRE(wait_for([&Recorder]() { return(Recorder.Count() == 2); }));
	REQUIRE(wait_for([&nNewWindows]() { return(nNewWindows.load() == 1); }));
	REQUIRE(Recorder.Snapshot() == std::vector<std::wstring>{ L"C:\\하나.txt", L"C:\\둘.txt" });
	REQUIRE(nNewWindows.load() == 1);

	// base64 해독 실패·UTF-8 해독 실패·빈 경로는 무시한다(원본 app.py:265~272).
	const std::string sBadPadding = "open-file\tYQ\n";
	const std::string sBadAlphabet = "open-file\t%%%\n";
	const std::string sEmptyPayload = "open-file\t\n";
	const std::string sBadUtf8 = "open-file\t__4=\n";
	REQUIRE(send_fragments(Primary.PipeName(),
		{ sBadAlphabet, sEmptyPayload, sBadPadding, sBadUtf8 }));
	// 조각난 프레임은 한 줄로 이어 붙는다.
	const std::string sFragmented = open_file_line(L"C:\\조각.txt");
	const std::string sHead = sFragmented.substr(0, 5);
	const std::string sMiddle = sFragmented.substr(5, 8);
	const std::string sTail = sFragmented.substr(13);
	REQUIRE(send_fragments(Primary.PipeName(), { sHead, sMiddle, sTail }));
	REQUIRE(wait_for([&Recorder]() { return(Recorder.Count() == 3); }));
	const auto Opened = Recorder.Snapshot();
	REQUIRE(Opened.size() == 3);
	REQUIRE(Opened.back() == L"C:\\조각.txt");
	REQUIRE(Primary.LastError().empty());
	Primary.Close();
}

TEST_CASE("W3-FB-104 paths delivered before the handler are replayed in order",
	"[W3-file-binding][FS-port]")
{
	C_SINGLE_INSTANCE Primary;
	REQUIRE(Primary.Acquire(database_path(L"재생").native()) == E_ACQUIRE_RESULT::Primary);
	const std::vector<std::wstring> Paths{ L"C:\\가.txt", L"C:\\나.txt", L"C:\\다.txt" };
	std::string sPayload;
	for (const std::wstring& sPath : Paths) { sPayload += open_file_line(sPath); }
	REQUIRE(send_fragments(Primary.PipeName(), { sPayload }));

	// 핸들러가 붙기 전에 도착한 경로는 큐에 쌓였다 등록 시 순서대로 재생된다. 등록을 늦춰
	// 큐 경로를 실제로 태운다 - 여기서 재지 않으면 살아 있는 배달과 구분되지 않는다.
	::Sleep(100);
	C_PATH_RECORDER Recorder;
	Primary.SetOpenFileHandler([&Recorder](std::wstring _sPath) { Recorder.Record(std::move(_sPath)); });
	REQUIRE(Recorder.Count() == Paths.size());
	REQUIRE(Recorder.Snapshot() == Paths);

	// 등록 뒤에는 큐가 비어 재등록이 같은 경로를 두 번 열지 않는다.
	C_PATH_RECORDER Second;
	Primary.SetOpenFileHandler([&Second](std::wstring _sPath) { Second.Record(std::move(_sPath)); });
	::Sleep(50);
	REQUIRE(Second.Count() == 0);
	Primary.Close();
}
