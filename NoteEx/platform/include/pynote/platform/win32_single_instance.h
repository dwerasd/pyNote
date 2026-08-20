#pragma once

#include <functional>
#include <memory>
#include <string>

namespace pynote::platform
{
	struct S_WIN32_STARTUP_OPTIONS
	{
		std::wstring sDatabasePath;
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

		C_WIN32_SINGLE_INSTANCE();
		~C_WIN32_SINGLE_INSTANCE();

		C_WIN32_SINGLE_INSTANCE(const C_WIN32_SINGLE_INSTANCE&) = delete;
		C_WIN32_SINGLE_INSTANCE& operator=(const C_WIN32_SINGLE_INSTANCE&) = delete;

		E_ACQUIRE_RESULT Acquire(const std::wstring& _sDatabasePath);
		void SetNewWindowHandler(NEW_WINDOW_HANDLER _Handler);
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
