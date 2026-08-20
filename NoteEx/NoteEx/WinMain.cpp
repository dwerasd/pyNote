// WinMain.cpp : 애플리케이션에 대한 진입점을 정의합니다.
//

#include "framework.h"
#include "CMain.h"

#include "pynote/platform/win32_device_settings.h"
#include "pynote/platform/win32_file_system.h"
#include "pynote/platform/win32_single_instance.h"

#include <shellapi.h>


dk::C_CONFIG* g_pConfig = nullptr;
dk::C_LOG* g_pLog = nullptr;

int APIENTRY wWinMain(_In_ HINSTANCE _hInstance, _In_opt_ HINSTANCE _hPrevInstance, _In_ LPWSTR _lpCmdLine, _In_ int _nCmdShow)
{
	UNREFERENCED_PARAMETER(_hPrevInstance);
	UNREFERENCED_PARAMETER(_lpCmdLine);
	UNREFERENCED_PARAMETER(_nCmdShow);

	int nArgumentCount = 0;
	LPWSTR* ppArguments = ::CommandLineToArgvW(::GetCommandLineW(), &nArgumentCount);
	if (!ppArguments) { return(1); }
	pynote::platform::S_WIN32_STARTUP_OPTIONS Options;
	std::wstring sArgumentError;
	const bool bArguments = pynote::platform::ParseWin32StartupOptions(
		nArgumentCount, ppArguments, &Options, &sArgumentError);
	::LocalFree(ppArguments);
	if (!bArguments) { return(1); }

	pynote::platform::C_WIN32_SINGLE_INSTANCE Instance;
	const auto eAcquire = Instance.Acquire(Options.sDatabasePath);
	if (eAcquire == pynote::platform::C_WIN32_SINGLE_INSTANCE::E_ACQUIRE_RESULT::SecondaryNotified)
	{
		return(0);
	}
	if (eAcquire != pynote::platform::C_WIN32_SINGLE_INSTANCE::E_ACQUIRE_RESULT::Primary)
	{
		return(1);
	}

	pynote::platform::C_WIN32_FILE_SYSTEM FileSystem;
	pynote::platform::C_WIN32_DEVICE_SETTINGS Settings(FileSystem);
	if (!Settings.Initialize()) { Instance.Close(); return(1); }

	C_MAIN cMain;
	const bool bInitialized = cMain.Init(_hInstance, &Settings);
	if (bInitialized)
	{
		Instance.SetNewWindowHandler([&cMain]() { cMain.RequestNewWindow(); });
		cMain.Display();
	}
	Instance.SetNewWindowHandler({});
	Instance.Close();
	cMain.Destroy();
	return(bInitialized ? 0 : 1);
}
