// WinMain.cpp : 애플리케이션에 대한 진입점을 정의합니다.
//

#include "framework.h"
#include "CMain.h"


dk::C_CONFIG* g_pConfig = nullptr;
dk::C_LOG* g_pLog = nullptr;

int APIENTRY wWinMain(_In_ HINSTANCE _hInstance, _In_opt_ HINSTANCE _hPrevInstance, _In_ LPWSTR _lpCmdLine, _In_ int _nCmdShow)
{
	UNREFERENCED_PARAMETER(_hPrevInstance);
	UNREFERENCED_PARAMETER(_lpCmdLine);
	UNREFERENCED_PARAMETER(_nCmdShow);

	C_MAIN cMain;
	if (cMain.Init(_hInstance))
	{
		cMain.Display();
	}
	cMain.Destroy();
	return(0);
}
