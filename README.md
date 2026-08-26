# pyNote

로컬에서 동작하는 **시간 추적형 장문 카드 메모 편집기**. 여러 줄 텍스트를 카드로
쌓고, 각 카드가 언제 처음 기록됐는지·몇 번째 기록인지·언제 어떻게 수정됐는지를
자동 보존한다. 원본은 로컬 SQLite 데이터베이스에 저장된다.

## 주요 기능

- **기록 카드 스트림**: 빈 입력기에 입력하거나 붙여넣으면 그 자리에서 카드가
  만들어진다. 긴 카드는 설정한 미리보기 줄 수만큼 줄여 보여주고, 카드를 클릭하면
  전용 편집기가 열린다
- **기록 순서 보존**: 카드를 옮겨도 최초 기록 순번(`기록 #N`)은 변하지 않는다 —
  현재 배치 순서와 최초 기록 순서를 따로 정렬해 조회한다
- **수정 이력(리비전)**: 내용이 바뀐 저장마다 복구 가능한 리비전을 만들고,
  리비전 사이의 diff 를 보여주며, 과거 버전으로 되돌릴 수 있다(되돌리기 직전
  상태도 리비전으로 남는다)
- **출처 추적**: 직접 입력·붙여넣기·가져오기 중 어느 경로로 들어온 내용인지
  자동 기록
- **초안 보호**: 편집 중인 내용을 주기적으로 보존해 비정상 종료 뒤 복구를 제안한다
- **가져오기·내보내기·백업**: 확장자를 가리지 않는 파일 가져오기(BOM 있는
  UTF-8·UTF-16, UTF-8, 시스템 기본 인코딩 순서로 해석하며 파일당 4 MiB 까지),
  TXT·Markdown 내보내기, 자동 DB 백업과 백업을 새 DB 파일로 복원
- **문서 관리**: 문서·카드 검색, 보관함과 휴지통, 재시작 시 작업 상태 복원
- **클립보드 수집**: 빈 입력기에 붙여넣으면 즉시 카드로 수집
- **다중 창**: 프로그램을 다시 실행하면 새 창이 열린다. 창마다 문서 하나를 맡고
  위치·크기를 따로 저장한다
- **실행 즉시 입력**: 창이 열리면 바로 타이핑·붙여넣기할 수 있다

## 데이터 위치

데이터베이스는 운영체제의 애플리케이션 데이터 디렉터리 아래 `pynote.sqlite3` 이고,
자동 백업은 그 옆 `backups/`, 비상 초안 사본은 `recovery/` 에 쌓인다.
휴지통의 카드는 설정한 보관 기간(기본 30일)이 지나야 완전 삭제할 수 있다.

## 요구 사항

- Windows (우선 지원 대상)
- Python 3.12 이상
- PySide6 6.9.1

## 사용법

### 가상환경에서 직접 실행

```bat
python -m venv .venv
.venv\Scripts\activate
pip install -r requirements.txt
python main.py
```

### 실행 파일로 빌드

```bat
python -m venv .venv
build.bat
```

`build.bat` 은 `.venv` 의 인터프리터로 빌드하므로 가상환경이 없으면 오류로
끝난다. 빌드가 끝나면 저장소 루트의 `pyNote.exe` 를 실행한다. PySide6 나
PyInstaller 가 없으면 스크립트가 설치한다.

### 개발

[uv](https://docs.astral.sh/uv/) 를 쓰는 경우:

```bash
uv sync
uv run ruff check src tests
uv run pyright
uv run pytest
```

pip 만 쓰는 경우:

```bash
pip install -r requirements-dev.txt
ruff check src tests
pyright
pytest
```

`requirements.txt` 와 `requirements-dev.txt` 는 `uv.lock` 에서 생성한다 — 각 파일
머리에 생성 명령이 적혀 있다.

## NoteEx — Windows 네이티브 포팅 (진행 중)

`NoteEx/` 디렉터리는 위 파이썬/PySide6 앱을 WTL(Windows Template Library)
기반 C++ 네이티브 앱으로 이식하는 별도 서브프로젝트다. 파이썬 원본을 동작
기준(golden)으로 삼아 카드 목록·휠 탐색·드래그 선택·가져오기·검색 등 기능을
단계적으로 포팅 중이며, 이 저장소 시점 기준 이식이 완료되지 않았다.

- **구조**: `core/`(Win32 비의존 순수 도메인 로직) · `NoteEx/`(WTL 앱 셸) ·
  `tests/`(Catch2 네이티브 테스트) · `tools/gates/`(파이썬/PowerShell 검증
  게이트 — core 계층이 Win32 헤더를 끌어오지 않는지, DB 스키마·마이그레이션·
  SQL 이 파이썬 원본과 일치하는지 등을 기계로 강제)
- **빌드**: `NoteEx/NoteEx.sln` (Visual Studio, x64). `core/`·`NoteEx/`·
  `tests/` 각각 별도 `.vcxproj`이며, `_Library`(솔루션 기준 4단 상위 외부
  의존: DarkCore·D2DWrapp·DOpenSources·sqlite3)가 별도로 필요하다 — 이
  저장소에는 포함돼 있지 않다.
- **테스트/검증**: `NoteExTests.exe`(Catch2)와 `tools/gates/` 아래 다수의
  파이썬 기반 골든/파리티 게이트로 파이썬 원본과의 동작 일치를 검증한다.
- **상태**: Windows 전용, 활성 개발 중인 포팅 작업(완결 여부는 커밋 로그
  기준 미확정). 일반 사용자는 위 파이썬 앱을 사용하면 되고, 이 서브프로젝트는
  Windows 네이티브 빌드에 관심 있는 개발자를 위한 것이다.

## 라이선스

[MIT](LICENSE)
