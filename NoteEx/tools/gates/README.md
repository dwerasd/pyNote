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

---

# v0001 마이그레이션 SQL 축자 이식 게이트

`check_migration_sql_parity.py` 는 파이썬 원본 `migrations/v0001_initial.py` 의
`STATEMENTS` 와 그 C++ 이식본의 `R"SQL( ... )SQL"` 리터럴을 **순서대로 바이트 단위**로
대조한다. 이식 계약이 "공백 한 칸까지 그대로"이므로, 지켜지는지를 리뷰가 아니라
게이트로 확인한다.

## 무엇을 증명하나

문장 수가 같고, 각 문장의 본문이 **완전히 같다**는 것. 들여쓰기·빈 줄·말미 공백·
문장 앞뒤의 개행이 전부 대조 대상이다. 다시 포맷하거나 정리한 코드는 실패하며,
그 지적의 해법은 원문 복원이지 게이트 완화가 아니다.

파이썬 쪽은 원본 모듈을 **경로로 적재**해 `STATEMENTS` 를 그대로 읽는다. 패키지
설치나 가상환경이 필요 없고(원본이 표준 라이브러리만 가져온다), 무엇보다 게이트가
검사 대상의 사본을 들지 않는다 - 사본을 든 게이트는 아무것도 증명하지 못한다.

정규화는 **CRLF -> LF 와 UTF-8 BOM 제거, 이 둘뿐**이다. 저장소가 `* text=auto` 로
LF 를 보관하므로 줄끝은 이 게이트가 지킬 대상이 아니고, 나머지 공백은 보호 대상
그 자체라 손대지 않는다.

원시 문자열 구분자는 `SQL` 로 고정이다. 주석·일반 문자열 안의 `R"SQL(` 표기와
구분자가 다른 원시 문자열(`R"NOTE(...)NOTE"`)은 추출되지 않는다. 뒤집어 말하면
**`STATEMENTS` 밖의 SQL 에 `SQL` 구분자를 쓰면 안 된다** - `schema_version` upsert
처럼 이식 대상이 아닌 문장이 추출되어 문장 수 불일치로 잡힌다.

## 실행

```bash
# 1) 게이트 자신을 검증한다(fixtures 기반, --python/--cpp 무시)
python tools/gates/check_migration_sql_parity.py --self-test

# 2) 실제 두 소스를 대조한다(기본 경로면 인자 없이)
python tools/gates/check_migration_sql_parity.py
python tools/gates/check_migration_sql_parity.py \
  --python src/pynote/infrastructure/migrations/v0001_initial.py \
  --cpp NoteEx/core/src/storage/migrations/v0001_initial.cpp
```

종료 코드:

| 코드 | 뜻 |
|---|---|
| 0 | 문장 수·내용 전건 일치 |
| 1 | 불일치 검출(자기시험이면 기대 불일치) |
| 2 | 사용법·환경 오류 — 경로 없음, 모듈 적재 실패, **추출 대상 0건** |

C++ 이식본이 아직 없으면 2 다. 판정할 수 없는 상태를 통과로 부르지 않는다.

## 실패는 무슨 뜻인가

문장별로 `[문장 N] 파일:줄` 과 **첫 불일치 오프셋**, 양쪽 원문의 `repr` 창,
통합 diff 가 stderr 로 나온다. 탭은 `\t` 로, 말미 공백은 `[말미 공백 N]` 으로
표시된다 - 눈에 보이지 않는 차이가 diff 에서 읽히게 하려는 것이다.

## 자기시험

`fixtures/schema_parity/static/` 의 기준 문장(`reference_statements.py`)과 C++ 모양
fixture 로 네 방향을 본다. 기준 문장은 실제 스키마의 사본이 **아니고**, 추출 함정
(중첩 괄호·작은따옴표 목록·여러 줄 CHECK)만 같게 만든 별개의 문장이다.

1. `good.cpp` 수용 — 주석·일반 문자열·다른 구분자 원시 문자열 함정 포함
2. `bad_*.cpp` 전건 거부 — 들여쓰기 변경, CHECK 누락, 열 순서 뒤바뀜, 말미 공백
   1칸, 리터럴 누락, 잉여 리터럴
3. CRLF + BOM 사본 수용 — 줄끝만으로 거짓 실패하지 않는가
4. `no_literals.cpp` 는 통과가 아니라 **종료 코드 2**

규칙을 추가하면 `bad_*.cpp` 를 한 개 늘리고 다시 돌린다. 시험되지 않은 규칙은
규칙이 아니다.

---

# v0001 스키마 동등성 게이트

`check_schema_parity.py` 는 파이썬 러너가 만든 v0001 데이터베이스와 C++ 러너가
만든 데이터베이스를 각각 `sqlite_master` 로 덤프해 **완전히 같은지** 본다. 위의
정적 게이트가 소스를 보는 반면 이쪽은 그 소스가 실제로 만들어 내는 결과를 본다.

## 두 게이트의 사거리 - 대체 관계가 아니다

| 결함 | 정적 게이트 | 동적 게이트 |
|---|---|---|
| 문장 내부의 공백·제약·열 순서 변화 | 잡는다 | 잡는다 |
| 문장 앞뒤의 개행·들여쓰기 변화 | 잡는다 | **못 잡는다**(SQLite 가 저장 시 버린다) |
| `INSERT` 시드 문장 누락 | 잡는다 | **못 잡는다**(스키마 객체가 아니다) |
| 문장 수 증감 | 잡는다 | 객체가 늘거나 줄면 잡는다 |
| C++ 이 실제로 만드는 스키마 | 못 본다(소스만 본다) | 잡는다 |

`IF NOT EXISTS` 가 저장 시 제거되는 것도 같은 이유로 동적 게이트에 보이지 않는다.
두 게이트를 함께 돌려야 이식이 증명된다. 동적 게이트의 자기시험은 이 사각을
**음성 대조군**으로 고정해 둔다 - 사거리가 바뀌면 자기시험이 깨지고, 그때는 이 표를
함께 고쳐야 한다.

## 정규화 규칙 (계약이므로 여기 적어 둔다)

- 대상은 `sqlite_master` 의 `type` `name` `tbl_name` `sql` 네 열.
- **`rootpage` 는 제외.** 물리 페이지 할당 번호라 같은 스키마여도 달라질 수 있고
  스키마 동등성의 일부가 아니다.
- 정렬은 `ORDER BY type, name, tbl_name`(SQLite 기본 BINARY 조합). 삽입 순서·페이지
  배치와 무관하게 결정적이다.
- **`sql` 본문은 어떤 공백 정규화도 하지 않는다.** 줄끝 차이(CRLF 대 LF)도 데이터
  수준의 실제 발산이므로 실패다.
- 표시 이스케이프: 덤프에 실을 때 역슬래시·CR·LF·탭을 `\\` `\r` `\n` `\t` 로 바꾼다.
  양쪽에 똑같이 적용하는 단사 변환이라 서로 다른 원문이 같은 덤프가 되지 않는다 -
  **정규화가 아니라 표기**이며, 보이지 않는 CR 차이를 diff 에서 읽으려는 것이다.
- `sqlite_autoindex_*` 와 `sqlite_sequence` 는 제외하지 않는다. UNIQUE·PRIMARY KEY·
  AUTOINCREMENT 선언의 결과라 스키마 정체성의 일부다.
- 행 데이터는 비교하지 않는다. `applied_at_us` 처럼 양쪽이 각자 만드는 값은 관심사가
  아니며, 자기시험이 서로 다른 값으로도 덤프가 같음을 보인다.

파이썬 쪽은 `database.py` 의 연결 수명주기를 그대로 재현한다(`_open` 68~82행,
트랜잭션 54~66행, 마이그레이션 호출 117행): autocommit 연결, `foreign_keys` 를 켠 뒤
되읽어 검증, `journal_mode = WAL` 을 반환값으로 검증, `BEGIN IMMEDIATE` 후 원본
`migrate` 호출, commit. 스키마는 원본 모듈을 경로로 적재해 **직접 호출**하므로 이
게이트에도 SQL 사본이 없다.

## C++ 방출기 계약 (고정)

| 항목 | 값 |
|---|---|
| 환경 변수 | `NOTEEX_PARITY_DB` = 만들 데이터베이스의 UTF-8 경로 |
| 명령 | `NoteExTests.exe "[parity-emit]"` |
| 성공 | 종료 코드 0, 그 경로에 데이터베이스 생성 |

## 실행

```bash
# 1) C++ 빌드 없이 게이트 자신을 검증한다(--exe 무시)
python tools/gates/check_schema_parity.py --self-test

# 2) 실제 두 러너를 대조한다
python tools/gates/check_schema_parity.py
python tools/gates/check_schema_parity.py --exe NoteEx/x64/ReleaseMD/NoteExTests.exe
```

종료 코드:

| 코드 | 뜻 |
|---|---|
| 0 | 두 덤프가 완전히 같음 |
| 1 | 스키마 발산 검출(자기시험이면 기대 불일치) |
| 2 | 사용법·환경 오류 — 실행 파일 없음, 방출기 비정상 종료·시간 초과, DB 미생성, **스키마 객체 0건** |

**이 게이트는 빌드하지 않는다.** 출력 디렉터리가 공유 자원이라 게이트가 멋대로
빌드하면 동시에 작업 중인 빌드를 망가뜨린다. 실행 파일이 없거나 낡았으면 2 로
멈추고 사실을 보고한다. 실행 파일의 크기·수정 시각을 매 실행마다 찍는 것도
낡음 판단을 사람이 하라는 뜻이다.

**스키마 객체 0건은 통과가 아니라 오류다.** 빈 데이터베이스 둘은 언제나 같으므로,
그것을 통과로 부르면 `[parity-emit]` 태그가 없는 낡은 실행 파일이 조용히 통과한다.

## 실패는 무슨 뜻인가

두 덤프의 통합 diff 가 stderr 로 나온다. `### type=... name=...` 이 객체 경계이고
`  |` 로 시작하는 줄이 저장된 SQL 원문이다. 차이가 나온 객체를 찾아 C++ 쪽 문장을
원본 문면으로 되돌린다 - 실패한 게이트를 고치는 것이 아니라 이식을 고친다.

## 자기시험

`fixtures/schema_parity/dynamic/perturbations.py` 의 교란 목록으로 세 방향을 본다.
C++ 빌드에 의존하지 않고 파이썬 러너만으로 돈다.

1. **수용** — 같은 스키마를 `applied_at_us` 만 달리해 두 번 만들면 diff 0줄
2. **거부** — 교란 4종: `NOT NULL` 제거, 문장 내부 들여쓰기 1칸 변화, 잉여 인덱스,
   그리고 **음성 대조군**인 시드 `INSERT` 누락(설계상 비탐지)
3. **환경 오류** — 스키마 객체 0건은 통과가 아니다

교란은 원본 문장을 문자열 수술로 바꾼다. 원본 문면이 바뀌어 교란이 무효가 되면
"교란이 아무것도 바꾸지 않았다"로 **실패한다** - 조용히 통과하지 않는다.
