#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pynote::platform
{
	struct S_WIN32_STARTUP_OPTIONS
	{
		std::wstring sDatabasePath;
		// 원본 _parse_arguments 의 positional paths(app.py:1156~1161). 하이픈으로 시작하지 않는
		// 인자를 순서대로 담는다. 두 프로세스의 현재 디렉터리가 다르므로 파싱 직후 결속 경로로
		// 확정해 넣는다 - 원본도 보내기 전에 resolve 한다(app.py:167~168).
		std::vector<std::wstring> Paths;
	};

	bool ResolveWin32DefaultDatabasePath(std::wstring* _psDatabasePath, std::wstring* _psError);
	bool ParseWin32StartupOptions(
		int _nArgumentCount, wchar_t* const* _ppArguments,
		S_WIN32_STARTUP_OPTIONS* _pOptions, std::wstring* _psError);
	bool MakeWin32InstanceIdentity(
		const std::wstring& _sDatabasePath, std::wstring* _psNormalizedParent,
		std::string* _psIdentity, std::wstring* _psError);

	class C_WIN32_SINGLE_INSTANCE
	{
	public:
		enum class E_ACQUIRE_RESULT
		{
			Primary,
			SecondaryNotified,
			Failure,
		};

		using NEW_WINDOW_HANDLER = std::function<void()>;
		// 원본 open_file_requested 신호(app.py:271)의 자리다. 경로는 이미 확정된 절대 경로다.
		using OPEN_FILE_HANDLER = std::function<void(std::wstring)>;

		C_WIN32_SINGLE_INSTANCE();
		~C_WIN32_SINGLE_INSTANCE();

		C_WIN32_SINGLE_INSTANCE(const C_WIN32_SINGLE_INSTANCE&) = delete;
		C_WIN32_SINGLE_INSTANCE& operator=(const C_WIN32_SINGLE_INSTANCE&) = delete;

		// 경로가 있으면 보조 프로세스가 그 목록을 한 연결에 실어 보낸다(원본 launch_message,
		// app.py:162~170). 기본값이 있으므로 경로를 쓰지 않는 호출부는 그대로다.
		E_ACQUIRE_RESULT Acquire(
			const std::wstring& _sDatabasePath, const std::vector<std::wstring>& _Paths = {});
		void SetNewWindowHandler(NEW_WINDOW_HANDLER _Handler);
		// 핸들러가 붙기 전에 도착한 경로는 큐에 쌓였다 등록 시 순서대로 재생된다
		// (nPendingRequests 와 같은 원리).
		void SetOpenFileHandler(OPEN_FILE_HANDLER _Handler);
		void Close();

		std::wstring LastError() const;
		const std::wstring& DatabaseParent() const;
		const std::string& Identity() const;
		const std::wstring& MutexName() const;
		const std::wstring& PipeName() const;

	private:
		struct S_STATE;
		std::unique_ptr<S_STATE> m_pState;
	};
}
