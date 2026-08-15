@echo off
chcp 65001 >nul
rem pyNote 빌드 — 저장소 루트에 pyNote.exe (단일 파일) 산출.
rem 이 파일은 UTF-8(BOM 없음) + CRLF 개행이다. chcp 가 한국어 메시지보다 먼저 와야
rem 하고, 개행이 LF 면 cmd.exe 가 파싱에 실패한다.
rem 실행 중이던 pyNote 는 정상 종료 후 빌드하고, 실행 중이었으면 끝나고 재실행한다.
setlocal EnableExtensions
cd /d "%~dp0"

set PY=.venv\Scripts\python.exe

echo [1/4] 가상환경 확인
if not exist "%PY%" (
  echo   오류: .venv 가 없다. 먼저 실행하라: python -m venv .venv
  exit /b 1
)
"%PY%" --version

echo [2/4] pyNote.exe 실행 여부 확인
set WAS_RUNNING=0
tasklist /FI "IMAGENAME eq pyNote.exe" 2>nul | find /I "pyNote.exe" >nul
if errorlevel 1 goto :not_running
set WAS_RUNNING=1
echo   실행 중 — 정상 종료를 요청한다.
taskkill /IM pyNote.exe >nul 2>&1
for /L %%I in (1,1,10) do (
  tasklist /FI "IMAGENAME eq pyNote.exe" 2>nul | find /I "pyNote.exe" >nul
  if errorlevel 1 goto :not_running
  ping -n 2 127.0.0.1 >nul
)
echo   응답 없음 — 강제 종료한다.
taskkill /F /IM pyNote.exe >nul 2>&1
ping -n 3 127.0.0.1 >nul
:not_running

echo [3/4] 의존성 확인
"%PY%" -c "import PySide6" 2>nul
if errorlevel 1 (
  echo   PySide6 설치 중...
  rem 버전 단일 출처는 requirements.txt 다.
  "%PY%" -m pip install -r requirements.txt
  if errorlevel 1 (
    echo   오류: PySide6 설치 실패
    exit /b 3
  )
)
"%PY%" -c "import PyInstaller" 2>nul
if errorlevel 1 (
  echo   PyInstaller 설치 중...
  "%PY%" -m pip install pyinstaller
  if errorlevel 1 (
    echo   오류: PyInstaller 설치 실패
    exit /b 4
  )
)

echo [4/4] PyInstaller 패키징
rem --distpath . : dist\ 대신 main.py 옆에 pyNote.exe 를 만든다
"%PY%" -m PyInstaller --noconfirm --clean --onefile --windowed --name pyNote --paths src --distpath . main.py
if errorlevel 1 (
  echo   오류: PyInstaller 실패
  exit /b 5
)
if not exist "%~dp0pyNote.exe" (
  echo   오류: pyNote.exe 가 생성되지 않았다.
  exit /b 6
)

if "%WAS_RUNNING%"=="1" (
  echo 빌드 완료 — pyNote 를 다시 실행한다.
  start "" "%~dp0pyNote.exe"
)

echo 완료: %~dp0pyNote.exe
exit /b 0
