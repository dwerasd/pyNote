# core 헤더 격리 게이트

`check_core_isolation.py` 는 3계층 분리(core / platform / shell) 중 **core 의 순수성**을
기계로 강제한다. 표준 라이브러리만 필요한 도메인 로직이 Win32 에 물들면 이식성과
테스트 가능성이 동시에 무너지므로, 리뷰가 아니라 게이트로 막는다.

## 무엇을 강제하나

`core/` 아래 C/C++ 소스·헤더(`.c .cc .cpp .cxx .h .hh .hpp .hxx .inl .ixx`)의
`#include` 지시문에 아래 계열이 등장하면 실패한다.

| 계열 | 패턴 | 예 |
|---|---|---|
| Win32 기본 | `win*.h` | `windows.h` `winuser.h` `winbase.h` `wincodec.h` |
| 셸·공통 컨트롤 | `shellapi.h` `shlobj*.h` `commctrl.h` | |
| COM | `objbase.h` `combaseapi.h` `unknwn.h` `ole*.h` | `ole2.h` `oleauto.h` |
| DirectX | `d2d*.h` `d3d*.h` `dwrite*.h` `dxgi*.h` | `d2d1_3.h` `dwrite_3.h` |
| 입력기·텍스트 | `imm.h` `msctf.h` `richedit.h` | |
| ATL / WTL | `atl*.h` `wtl*.h` | `atlbase.h` `atlapp.h` `atlcrack.h` |
| 기타 | `tchar.h` `strsafe.h` | |

접두 계열 글롭이 정본이라 SDK·WTL 이 새 헤더를 추가해도 같은 계열이면 자동으로
걸린다. 판정은 include 표기의 마지막 경로 요소를 소문자로 낮춰 하므로 `<Windows.h>`
같은 혼용 표기와 `"atlbase.h"` 같은 따옴표 표기도 잡힌다.

허용: 표준 라이브러리, 프로젝트 헤더, 그리고 명시 허용 목록인 `sqlite3.h`·
`sqlite3ext.h`(SQLite 는 이식 가능한 저장 엔진이라 core 에서 쓴다).

주석(`//`, `/* */`)·문자열 리터럴·원시 문자열 안의 `#include` 는 위반이 아니다.
반대로 들여쓴 지시문, `#  include` 같은 이상 공백, UTF-8 BOM 뒤 첫 줄 지시문은
전부 위반으로 잡힌다.

## 실행

```bash
# 1) 게이트 자신을 양방향 검증한다(fixtures 기반, --roots 무시)
python tools/gates/check_core_isolation.py --self-test

# 2) 실제 트리를 검사한다(디렉터리 여러 개 지정 가능)
python tools/gates/check_core_isolation.py --roots core
```

종료 코드:

| 코드 | 뜻 |
|---|---|
| 0 | 통과 |
| 1 | 위반 검출(자기시험이면 기대 불일치) |
| 2 | 사용법·환경 오류 — 경로 없음, 파일 읽기 실패, **스캔 대상 0건** |

**스캔 대상 0건을 통과가 아니라 오류로 두는 것은 의도된 선택이다.** 대상이 없는
게이트는 아무것도 증명하지 못하고, 실제 원인은 대개 경로 오타나 디렉터리 개편이다.
조용한 통과보다 붉은 실패가 싸다.

## 금지 헤더를 추가하려면

1. `check_core_isolation.py` 의 `FORBIDDEN_PATTERNS` 에 글롭 1줄을 넣는다. 개별
   이름보다 계열 접두를 우선한다(`d3d*.h` > `d3d11.h`).
2. `fixtures/bad/` 에 그 헤더를 include 하는 파일 1개를 추가한다. 시험되지 않은
   규칙은 규칙이 아니다.
3. `--self-test` 로 양방향을 다시 돌린다.

넓은 접두 패턴이 정당한 프로젝트 헤더를 잡으면(예: `atl*.h` 가 도메인 헤더
`atlas.h` 를 잡는 경우) 헤더 이름을 바꾸거나 `ALLOWED_HEADERS` 에 넣는다. 허용
목록은 금지 패턴보다 먼저 판정한다.

## 실패는 무슨 뜻인가

`path:line: <헤더>` 가 stderr 로 한 줄씩 나오고 마지막에 건수 요약이 붙는다.
core 파일이 걸렸다면 그 코드는 core 에 있으면 안 되는 코드다. 억제하지 말고
플랫폼 의존부를 `platform/` 인터페이스 뒤로 옮긴 뒤 core 는 그 인터페이스만
쓰게 고친다.

`fixtures/bad/` 는 **의도적으로 비준수**다. 게이트의 스캔 루트에 넣지 않는다.
