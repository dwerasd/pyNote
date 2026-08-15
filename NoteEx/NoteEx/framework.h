// framework.h: 표준 시스템 포함 파일
// 또는 프로젝트 특정 포함 파일이 들어 있는 포함 파일입니다.
//

#pragma once

#include "targetver.h"
#define WIN32_LEAN_AND_MEAN             // 거의 사용되지 않는 내용을 Windows 헤더에서 제외합니다.
#define NOMINMAX                        // min/max 매크로 차단(std::min/max 충돌 방지)
// Windows 헤더 파일
#include <windows.h>
// C 런타임 헤더 파일입니다.
#include <stdlib.h>
#include <malloc.h>
#include <memory.h>
#include <tchar.h>

// DarkCore(자가 라이브러리) - 로그/설정/싱글톤/디버그 출력.
// 링크는 pragma 로만 건다. .sln 빌드 시 DarkCore.lib 가 NoteEx.exe 와 같은 $(OutDir) 에 생성되고
// NoteEx 의 AdditionalLibraryDirectories=$(OutDir) 가 그것을 찾는다.
#include <DarkCore/DDef.h>
#include <DarkCore/DPrint.h>
#include <DarkCore/DConfig.h>
#include <DarkCore/DLog.h>
#include <DarkCore/DSingleton.h>
#pragma comment(lib, "DarkCore")

extern dk::C_CONFIG* g_pConfig;
extern dk::C_LOG* g_pLog;
