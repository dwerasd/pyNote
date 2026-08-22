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

# 마이그레이션 SQL 축자 이식 게이트 (v0001~v0009)

`check_migration_sql_parity.py` 는 파이썬 원본 `migrate()` 가 **실제로 발행하는
문장**과 그 C++ 이식본의 `u8R"SQL( ... )SQL"` 리터럴을 **순서대로 바이트 단위**로
대조한다. 이식 계약이 "공백 한 칸까지 그대로"이므로, 지켜지는지를 리뷰가 아니라
게이트로 확인한다.

## 무엇을 증명하나

문장 수가 같고, 각 문장의 본문이 **완전히 같다**는 것. 들여쓰기·빈 줄·말미 공백·
문장 앞뒤의 개행이 전부 대조 대상이다. 다시 포맷하거나 정리한 코드는 실패하며,
그 지적의 해법은 원문 복원이지 게이트 완화가 아니다.

## 왜 소스 파싱이 아니라 실행 기록인가 (T-R2 에서 바뀐 부분)

`v0001` 만 모듈 상수 `STATEMENTS` 를 노출하고 나머지 여덟은 `connection.execute(...)`
를 인라인으로 부른다. `v0003` 은 지역 튜플을 돌며 DDL 앞에 `SELECT` 두 건을 먼저
발행한다. 이 모양들을 정적으로 파싱하면 깨지기 쉽고, 무엇보다 **원본이 어떻게
쓰였는지**를 검사하게 된다. 기록 프록시로 발행을 잡으면 검사 대상이 **원본이 무엇을
하는지**가 되고, 표현 방식이 바뀌어도 게이트는 그대로 성립한다.

원본 모듈은 경로로 적재해 직접 호출하므로 패키지 설치나 가상환경이 필요 없고,
게이트가 검사 대상의 사본을 들지 않는다 - 사본을 든 게이트는 아무것도 증명하지 못한다.

## 리터럴 접두 `u8` 는 필수다

이 기계에서 좁은 리터럴은 CP949 로 컴파일된다. 본문이 바이트까지 같아도 SQLite 로
넘어가는 바이트가 UTF-8 이 아니게 되고, `v0003` 의 SQL 안에는 한국어가 들어 있다
(`RAISE(ABORT, '카드와 현재 리비전이 일치하지 않습니다')` 등). 오늘 한국어가 없는
파일에도 같은 규칙을 적용하는 이유는, 파일 내용에 따라 켜졌다 꺼지는 규칙은 누군가
한국어를 처음 넣는 순간 조용히 깨지기 때문이다. 지켜야 할 불변식은 "SQLite 에 넘기는
SQL 원문은 UTF-8"이고, 그것은 전건 성립하거나 불변식이 아니다.

정규화는 **CRLF -> LF 와 UTF-8 BOM 제거, 이 둘뿐**이다. 저장소가 `* text=auto` 로
LF 를 보관하므로 줄끝은 이 게이트가 지킬 대상이 아니고, 나머지 공백은 보호 대상
그 자체라 손대지 않는다.

원시 문자열 구분자는 `SQL` 로 고정이다. 주석·일반 문자열 안의 표기와 구분자가 다른
원시 문자열(`R"NOTE(...)NOTE"`)은 추출되지 않는다. 뒤집어 말하면 **발행되지 않는 SQL
에 `SQL` 구분자를 쓰면 안 된다** - 잉여 리터럴로 잡힌다.

## 실행

```bash
# 1) 게이트 자신을 검증한다(fixtures 기반, --python/--cpp 무시)
python tools/gates/check_migration_sql_parity.py --self-test

# 2) 실제 두 소스를 대조한다(기본 경로면 인자 없이)
python tools/gates/check_migration_sql_parity.py
# 3) C++ 이식본이 다른 곳에 있으면 디렉터리를 지정한다
python tools/gates/check_migration_sql_parity.py \
  --cpp-dir NoteEx/core/src/storage/migrations
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

`fixtures/schema_parity/static/` 의 기준 마이그레이션(`reference_migration.py`)과 C++
모양 fixture 로 다섯 방향을 본다. 기준 마이그레이션은 실제 스키마의 사본이 **아니고**,
추출 함정(중첩 괄호·작은따옴표 목록·여러 줄 CHECK)만 같게 만든 별개의 문장이다.

1. `good.cpp` 수용 — 주석·일반 문자열·다른 구분자 원시 문자열 함정 포함
2. `bad_*.cpp` 전건 거부 — 들여쓰기 변경, CHECK 누락, 열 순서 뒤바뀜, 말미 공백
   1칸, 리터럴 누락, 잉여 리터럴, **좁은 리터럴**(`u8` 누락), **발행 순서 뒤바뀜**
3. CRLF + BOM 사본 수용 — 줄끝만으로 거짓 실패하지 않는가
4. `no_literals.cpp` 는 통과가 아니라 **종료 코드 2**
5. 실제 원본 9본이 문장을 발행하는지 확인 — 기록 프록시가 조용히 0건을 잡고
   "전건 일치"를 내는 경로를 닫는다

규칙을 추가하면 `bad_*.cpp` 를 한 개 늘리고 다시 돌린다. 시험되지 않은 규칙은
규칙이 아니다.

---

# 스키마 동등성 게이트 (최신 버전 신규 생성)

`check_schema_parity.py` 는 파이썬 러너가 빈 파일에서 만든 데이터베이스와 C++ 러너가
같은 자리에서 만든 데이터베이스를 각각 `sqlite_master` 로 덤프해 **완전히 같은지**
본다. 위의 정적 게이트가 소스를 보는 반면 이쪽은 그 소스가 실제로 만들어 내는 결과를
본다. 등록된 마이그레이션 전건을 돌리므로 대상 버전은 그때의 최신 버전이다(현재 v9).

기존 데이터베이스가 갱신되는 경로와 행 데이터 대조는 이 게이트가 보지 않는다 —
아래 사다리 게이트가 그쪽을 맡는다.

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

---

# 사다리 동등성 게이트 (v1~v9 전 구간)

`check_migration_ladder_parity.py` 는 두 러너가 **같은 사다리를 올랐을 때 같은 곳에
도착하는지** 본다. 위의 두 게이트가 각각 소스와 신규 생성 결과를 보는 데 비해, 이쪽은
**기존 사용자 데이터베이스가 갱신되는 실제 경로**와 **행 데이터**를 본다.

경로 A 는 빈 데이터베이스에서 한 번에 v9 까지 간다. 경로 B 는 N = 1..8 각각에 대해
정확히 버전 N 에 머무는 데이터베이스를 두 벌 복사해 한 벌은 파이썬이, 한 벌은 C++ 이
v9 로 올린다.

## 왜 fixture 에 행이 들어 있어야 하나

빈 표 위의 사다리는 `v0004` 의 행 복사, `v0005` 의 이름변경-복사-삭제, `v0007`/`v0008`
의 초기화, `v0009` 의 조건부 갱신을 **전부 건너뛴다**. 이식이 틀렸어도 결과가 같아지므로
게이트가 아무것도 증명하지 못한다. fixture 는 `make_ladder_fixtures.py` 가 만들며 각
버전의 스키마에서 성립하는 실제 행을 담는다.

## 무엇을 비교하나

스키마 규칙은 위 동등성 게이트와 같다. 행 데이터는 **사용자 표 전부와
`sqlite_sequence`** 를 대조하며, 표 목록을 고정으로 적지 않는다 - 나중에 마이그레이션이
새 표를 만들면 그것도 자동으로 대조 대상이 되어야 한다. 값은 형까지 드러나게 적어
(`INT:` `REAL:` `TEXT:` `BLOB:` `NULL`) 형 변화도 발산으로 잡는다.

## 정규화 - 비결정 열 세 개

`schema_version.applied_at_us`, `data_policy_settings.updated_at_us`(v0002),
`workspace_windows.window_id`(v0004 의 `randomblob`/`random`)는 러너가 스스로 값을
만들기 때문에 양쪽이 같을 수 없다. 다만 **자리표시자로 바꾸는 것은 그 마이그레이션이
이번 대조 구간 `(N, 9]` 에서 실제로 실행될 때뿐**이다. 실행되지 않았다면 그 값은
fixture 가 들고 온 것이므로 그대로 대조한다 - 무조건 가리면 fixture 값이 유실되는
회귀를 놓친다. 규칙의 정본은 게이트 모듈 docstring 이다.

## 실행

```bash
# 1) C++ 빌드 없이 게이트 자신을 검증한다(--exe 무시)
python tools/gates/check_migration_ladder_parity.py --self-test

# 2) 실제 두 러너로 9경로를 올린다
python tools/gates/check_migration_ladder_parity.py
python tools/gates/check_migration_ladder_parity.py \
  --exe NoteEx/x64/ReleaseMD/NoteExTests.exe --timeout 120
```

종료 코드는 위 게이트들과 같다(0 일치 / 1 발산 / 2 환경 오류). 시험 실행 파일이 없거나
`[parity-upgrade]` 태그가 없으면 2 다 - 이 게이트는 **빌드하지 않는다.** 공유 출력
디렉터리를 건드리지 않기 위한 의도된 제약이다.

## C++ 러너 계약 (고정)

| 태그 | 대상 파일 | 하는 일 |
|---|---|---|
| `[parity-emit]` | 있으면 지운다(사이드카 포함) | 빈 자리에서 최신 버전까지 새로 만든다 |
| `[parity-upgrade]` | **반드시 있어야 한다** | 기존 데이터베이스로 열어 최신 버전까지 올린다 |

둘 다 경로를 `NOTEEX_PARITY_DB` 환경변수로 받고 성공 시 0 으로 끝난다. 숨김 태그라
인자 없는 `NoteExTests.exe` 실행에는 잡히지 않는다.

---

# 사다리 fixture 생성기

`make_ladder_fixtures.py` 는 게이트가 아니라 **입력 생성기**다. `v0001.db` ~ `v0008.db`
를 만들고 입력·산출 SHA-256 을 기록한다. 입력 목록에는 마이그레이션 원본 아홉 본과 이
도구 자신, 공용 모듈이 들어간다 - 그중 무엇이 바뀌어도 fixture 를 다시 만들어야 한다는
뜻이다.

```bash
python tools/gates/make_ladder_fixtures.py --self-test   # 산출물 자체 검증
python tools/gates/make_ladder_fixtures.py --out fixtures/ladder_parity
```

`--self-test` 는 만들어진 fixture 가 **의도한 버전에 머물고 의도한 행을 담는지** 본다.
버전만 맞고 표가 비어 있으면 사다리가 아무것도 시험하지 못하므로 그것도 실패다.

---

# 저장소 계층 SQL 축자 이식 게이트

`check_repository_sql_parity.py` 는 `repositories.py` 가 `execute(...)` 로 넘기는 SQL
문장과 그 C++ 이식본의 `u8R"SQL( ... )SQL"` 리터럴을 바이트로 대조한다. 위 마이그레이션
게이트와 같은 계약을 저장소 계층에 건 것이다 — 한쪽만 지키면 다음에 SQL 을 손댄 사람이
어느 쪽이 보호받는지 알 수 없다.

## 마이그레이션 게이트와 다른 두 가지

**순서를 대조하지 않는다.** 마이그레이션은 문장 발행 순서가 그 자체로 계약이지만
(`create_cards` 의 강제 순서가 그 예다) 저장소의 메서드 배치 순서는 계약이 아니다.
그래서 **다중집합**을 대조한다 — 같은 문장이 두 번 나오면 양쪽 다 두 번이어야 한다.

**추출 방식이 다르다.** 마이그레이션 게이트는 `migrate()` 를 실제로 실행해 발행을
기록하지만, 저장소 메서드는 살아 있는 데이터베이스와 인자가 있어야 부를 수 있다.
그래서 이쪽은 파이썬 AST 에서 `*.execute(<문자열 리터럴>)` 의 첫 인자를 뽑는다.
리터럴이 아닌 방식으로 조립된 문장은 파이썬 쪽에서 안 잡히지만, 그런 문장이 C++ 에
있으면 "파이썬 쪽에 없는 문장"으로 크게 실패한다.

`u8` 접두는 여기서도 필수이고 이유도 같다. 정규화도 CRLF -> LF 와 BOM 제거 둘뿐이다.

## 실행

```bash
python tools/gates/check_repository_sql_parity.py --self-test
python tools/gates/check_repository_sql_parity.py
python tools/gates/check_repository_sql_parity.py \
  --python src/pynote/infrastructure/repositories.py \
  --cpp NoteEx/core/src/storage/repositories.cpp
```

종료 코드는 다른 게이트와 같다(0 일치 / 1 불일치 / 2 환경 오류). 자기시험은 정상 1건
수용과 결함 4종(리터럴 누락, 들여쓰기 변형, `u8` 접두 누락, 잉여 리터럴) 거부에 더해
**실제 원본에서 문장이 실제로 뽑히는지**까지 본다. 0건 추출이 "전건 일치"로 보고되는
경로를 닫는 것이다 — 아무것도 비교하지 않은 게이트는 통과가 아니다.

---

# 바인드 순서 게이트

`check_bind_order.py` 는 위 두 SQL 게이트가 못 보는 구멍 하나를 맡는다. **본문이
바이트까지 같아도 `?` 자리에 넣는 값의 순서가 어긋날 수 있다.** 같은 형의 파라미터가
연달아 붙은 문장 — `UpdateCaptureOperation` 8 개, `CreateCard` 12 개, `UpdateDraft`
9 개 — 에서 두 자리를 맞바꾸면 SQL 은 멀쩡하고 컴파일도 되며, 시험 값이 서로 비슷하면
왕복 시험까지 통과한다. 대상은 저장소 계층 한 쌍과 마이그레이션 아홉 쌍이다(백업
계층은 범위 밖 — `--python/--cpp` 로 따로 걸 수 있다).

## 무엇을 증명하나

1. **개수·인덱스** — `?` 개수와 바인드 호출 수가 같고 인덱스가 빠짐도 중복도 없이
   정확히 1..N 이다.
2. **오름차순 배치** — 바인드가 소스에 인덱스 순으로 놓인다(읽는 사람 몫의 계약이다).
3. **값 대응** — 파이썬이 넘기는 **튜플 순서**와 C++ 이 **인덱스로 지정한 순서**가 같은
   값을 가리킨다.
4. **센서스** — 양쪽 모두, `?` 를 담은 SQL 이 인식된 바인드 자리에 붙어 있지 않으면
   실패다. 게이트가 모르는 방식으로 바인딩하면 조용히 넘어가는 대신 실패로 보고한다.

## 3 번만 추정이다 (그리고 모르면 실패한다)

파이썬은 위치로 말하고 C++ 은 이름(인덱스)으로 말하므로 둘을 이으려면
`card.updated_at_us` <-> `_Card.nUpdatedAtUs` 같은 이름 사상이 필요하다. 규칙은
`domain::ToText(...)`·열거형 `.value` 를 벗기고, 헝가리안 접두를 떼고, CamelCase 를
snake_case 로 접는 것이다. **접을 수 없는 식은 건너뛰지 않고 위반이다** — 함수 호출,
첨자, 연산식, 헝가리안도 snake 도 아닌 식별자가 나오면 그 자리를 실패로 보고한다.
모르는 것을 조용히 넘기는 검사기는 없느니만 못하다. 통과라고 보고하기 때문이다.

사상은 many-to-one 이라 `_Card.sId` 와 `_Draft.sId` 는 둘 다 `id` 로 접힌다. 즉
**이름이 다르면 확실히 잡고, 이름이 같으면 아마 맞다** 까지다. 못 보는 것의 전체
목록은 게이트 docstring 의 "무엇을 증명하지 않나" 절이 소유한다.

## 실행

```bash
# 1) 게이트 자신을 양방향 검증한다(fixtures 기반, --python/--cpp 무시)
python tools/gates/check_bind_order.py --self-test
# 2) 실제 트리 열 쌍을 전부 검사한다
python tools/gates/check_bind_order.py
# 3) 한 쌍만 검사한다(백업 계층처럼 기본 목록에 없는 것)
python tools/gates/check_bind_order.py \
  --python src/pynote/infrastructure/repositories.py \
  --cpp NoteEx/core/src/storage/repositories.cpp
```

종료 코드는 다른 게이트와 같다(0 통과 / 1 위반 / 2 환경 오류). 마이그레이션 C++
이식본이 하나라도 없으면 2 다 — 있는 것만 보고 통과라고 말하지 않는다. 대조 대상이
0 건이어도 2 다.

## 자기시험

정상 표본 1 건 수용에 더해 **결함 8 종을 기대 사유까지 맞춰** 거부한다 — 인덱스 중복,
인덱스 건너뜀, 이웃한 동형 바인드 뒤바뀜, 바인드 수 불일치, 인덱스 역순 배치, 바인드에
안 붙은 `?` 리터럴, 사상 불가 C++ 식, 사상 불가 파이썬 파라미터. "무엇이든 걸렸다" 가
아니라 **심은 결함 때문에 거부됐는지**를 보는 이유는, 엉뚱한 이유로 통과한 자기시험이
곧 미검증 계측기이기 때문이다. 표본은 `fixtures/bind_order/` 에 있고 결함본은 정상본에서
한 곳만 비튼 것이다. 여기에 실제 이식본에서 바인드 문장이 실제로 뽑히는지까지 본다.

---

# 시험 추적성 게이트

`check_test_traceability.py` 는 W1 지시서의 **주석 규약을 기계로 굳힌다.** 규약은
"새로 쓰는 시험마다 대응하는 파이썬 원본을 주석으로 적는다" 이고, W0 T4 가 시험 이식
대장을 만들 때 그 주석의 pytest node ID 를 역보강한다. 지금까지 이 규약은 관례로만
지켜졌다 — 이식 워커들이 실제로 적었을 뿐 아무도 검사하지 않았다.

검사하지 않는 관례가 위험한 이유는 **썩는 방식이 조용하기 때문**이다. 주석 없이 추가된
시험은 누가 파일을 열어 읽기 전까지 주석 있는 시험과 겉보기가 같다. 그보다 나쁜 것이
**없는 node ID 를 적은 주석**이다. 역보강 때는 그 문자열을 믿고 대장에 옮겨 적을 텐데,
가리키는 함수가 없으면 대장 한 줄이 통째로 거짓이 되고 다시 확인하는 사람은 없다.

## 대장 시대 개정 (2026-08-21)

T4a 가 시험 이식 대장을 동결한 뒤 W2 이후 이식 시험은 주석 대신 **추적 ID 를 시험
이름·태그에 실었다**(대장 native_id `WTL-W#-####`·`PLAN-W#-####`, T4a uncertain
`T4A-UNC-###`, capability `CAP-XX-###`·`WTL-CAP-XX-###`, 동결 조각 계약 SC
`W#-D8-###` 류·`W#-Z#` 류, 또는 조각 계약 태그 `[W#-...]`). 게이트는 이 관례를 그대로
굳힌다 — 아래 "주석 존재" 요구는 **추적 ID·태그가 없는 시험에만** 적용되고, ID 형식이
아닌 이름(소문자·자릿수 미달·문중 삽입)은 여전히 위반이다. 이름·태그는 형식만 본다 —
실재는 각각 `check_test_port_register.py`·`check_capability_matrix.py`·조각 게이트
소관이다(docs/ 는 gitignored 라 기본 실행이 저장소 밖에 의존하면 안 된다).
`--register <대장 경로>` 를 준 실행에서는 대장 계열 제목 ID 의 실재까지 추가 대조한다.

## 무엇을 강제하나

1. **주석 존재** — `NoteEx/tests/unit/` 의 모든 `TEST_CASE` 바로 위에 `//` 주석 블록이
   빈 줄 없이 붙어 있고, 그 블록에 `대응 원본` 표기가 있다(위 개정대로 추적 ID·태그가
   있는 시험은 제외).
2. **claim 해소** — 블록이 pytest node ID 를 주장하면 그 파일이 저장소에 있고 그 이름의
   시험 함수가 파일 안에 **정의**되어 있다. 판정은 `ast` 로 한다 — 호출부나 주석에 이름이
   스쳐 지나가는 것은 정의가 아니다.
3. **부재의 명시** — `대응 원본 없음` 과 `... node ID 는 W0 T4 역보강 대기다` 는 **유효한
   형태로 받는다.** 그것은 구멍이 아니라 정직한 신고다. 다만 명시적이어야 한다 — 침묵은
   부재 선언이 아니므로, 아무것도 주장하지 않는 블록은 위반이다.

## 주석 배치는 강제하지 않는다

이식 워커들은 node ID 를 다섯 가지 자리에 적었다 — 대응 원본 줄에 직접, `pytest node
ID:` 별도 줄, 들여쓴 목록, 산문 괄호 안, 그리고 앞줄 경로를 잇는 `::이름`. 그중 하나만
정본으로 골랐다면 나머지 네 가지를 전부 고쳐야 한다. **게이트의 일은 있는 관례를 기계로
굳히는 것이지 새 관례를 강요하는 것이 아니므로** 블록 안 어디에 있든 받는다.

강제하는 것은 둘뿐이다. node ID 는 `tests/` 로 시작하는 저장소 상대 경로에 붙어야 하고
(그래야 저장소 루트에서 pytest 로 그대로 돌릴 수 있다 — `v0001_initial.py::migrate` 처럼
원본 모듈 함수를 가리키는 산문은 claim 이 아니다), 경로를 생략한 `::이름` 은 **같은 블록의
앞줄**에 경로가 나와 있어야 한다. 앞이 비어 있으면 어느 파일의 시험인지 아무도 모른다.

## 실행

```bash
# 1) 게이트 자신을 양방향 검증한다(fixtures 기반, --root 무시)
python tools/gates/check_test_traceability.py --self-test
# 2) 실제 시험 트리를 검사한다
python tools/gates/check_test_traceability.py
# 3) 다른 디렉터리를 검사한다(node ID 해소 기준도 함께 옮길 수 있다)
python tools/gates/check_test_traceability.py \
  --root tests/unit --resolve-root ..
```

종료 코드는 다른 게이트와 같다(0 통과 / 1 위반 / 2 환경 오류). `TEST_CASE` 를 하나도 못
찾으면 2 다 — 아무것도 검사하지 않은 게이트는 통과가 아니다. 파일이 반쯤 쓰인 채로 읽혀
주석·문자열이 닫히지 않으면 특별 취급하지 않고 **그 순간 본 것을 위반으로 보고한다.**

`NoteEx/tests/harness/` 는 기본 대상이 아니다. 그쪽은 이식본이 아니라 하네스 자체의 연기
시험이라 대응할 파이썬 원본이 없다.

## 이 게이트의 초록이 뜻하지 않는 것

**C++ 시험이 주석에 적힌 파이썬 시험과 같은 동작을 보는지는 전혀 확인하지 않는다.**
엉뚱한 시험을 가리켜도 그 이름이 실재하기만 하면 통과다. 초록은 "주석이 거짓말을 하지
않는다" 까지이지 "이식이 옳다" 가 아니다. 마찬가지로 역보강 대기 선언의 진위 — 정말
대응 케이스가 없는지, 찾기 귀찮았는지 — 도 구별하지 않는다. 대기 선언은 전수 조사가
아니라 신고이고 그 신고를 대조하는 것이 W0 T4 의 일이다. 못 보는 것의 전체 목록은
게이트 docstring 의 "무엇을 증명하지 않나" 절이 소유한다.

## 자기시험

정상 표본 1 건(트리에 실재하는 주석 배치 일곱 가지를 한 파일에 모은 것) 수용에 더해
**결함 5 종을 기대 사유까지 맞춰** 거부한다 — 주석 없는 `TEST_CASE`, 파일은 있고 함수가
없는 node ID, 파일 자체가 없는 node ID, 아무것도 주장하지 않는 주석, 경로 없는 `::이름`
표기. 정상 표본에서는 위반 0 건에 더해 **`TEST_CASE` 7 건과 node ID 7 건을 정확히 셌는지**
까지 본다. 개수를 안 보면 주석·문자열 안의 `TEST_CASE` 를 세거나 진짜를 빠뜨리는 훑기
결함이 "위반 0 건" 뒤에 숨는다. 표본은 `fixtures/test_traceability/` 에 있고, node ID 해소
기준도 그 안의 `tests/integration/test_sample.py` 다 — 진짜 시험 트리를 가리키면 파이썬
시험 이름 하나가 바뀔 때마다 게이트와 상관없는 이유로 자기시험이 깨진다.

---

# 기존 사용자 DB 세트 개방 게이트

`check_sidecar_open.py` 는 사용자 디스크에 실제로 있는 것 — 본체와 `-wal` 과 `-shm`
세 파일 — 을 그대로 열 수 있는지 본다. 위 사다리 게이트가 데이터베이스 **한 파일**을
승급시키는 데 비해 이쪽은 세트를 연다.

## 왜 WAL 에만 있는 행을 심나

사이드카를 만들어 두기만 하면 아무것도 증명하지 못한다. WAL 이 비어 있으면 본체만
열어도 결과가 같기 때문이다. 그래서 `wal_autocheckpoint = 0` 으로 자동 체크포인트를
끄고 커밋한 뒤 **연결이 열려 있는 채로** 세 파일을 복사한다. 복사본의 본체에는 그 행이
없고 WAL 에만 있으므로, 이식본이 사이드카를 버리거나 무시하면 행이 사라지고 게이트가
잡는다. 자기시험이 그 음성 대조군을 고정한다 — 사이드카를 지우면 표식이 실제로
사라지는지 확인하고, 안 사라지면 판정이 공허하다고 실패시킨다.

연결을 닫고 복사하면 안 된다. SQLite 가 닫는 순간 체크포인트하고 사이드카를 지워
시험하려던 상태가 사라진다.

## 실행

```bash
# fixture 가 먼저 필요하다
python tools/gates/make_ladder_fixtures.py --out <디렉터리>

# 1) C++ 빌드 없이 전제를 검증한다
python tools/gates/check_sidecar_open.py --self-test --fixtures <디렉터리>

# 2) 실제 러너로 전 버전을 연다
python tools/gates/check_sidecar_open.py --fixtures <디렉터리>
```

각 버전마다 세 가지를 본다. C++ 러너가 세트를 열어 최신 버전까지 올리는가,
결과가 `quick_check` 을 통과하고 `schema_version` 이 최신인가, 그리고 **WAL 에만 있던
행이 살아남았는가**. 종료 코드는 다른 게이트와 같다.

**증명하지 않는 것**: 스키마와 행 전량의 동등성(사다리 게이트 소관), 강제 종료 복구의
모든 형태(여기서 만드는 것은 정상 연결이 열린 상태의 복사본이지 프로세스가 죽은 순간의
스냅샷이 아니다 — 그쪽은 W6 손실 프로브 소관), SHM 의 내용(복사하되 해석하지 않는다).

---

# 연결 상태 동등성 게이트

`check_connection_parity.py` 는 파이썬 연결과 C++ 연결의 `PRAGMA` 실효값을 각각 실측해
대조한다. **특정 값을 강제하지 않는다** — 현행 파이썬은 `synchronous` 를 명시 설정하지
않으므로 옳은 값이 무엇인지는 이 게이트의 관심사가 아니고, 두 연결이 같은 상태에 있는지가
관심사다. 파이썬이 2 이고 C++ 이 2 면 통과, 둘 다 1 이어도 통과, 갈리면 실패다.

## 왜 파일을 열어 보는 방식으로는 안 되나

여기서 보는 값은 대부분 **연결 속성이라 데이터베이스 파일에 영속되지 않는다.** 방출된
DB 를 나중에 파이썬으로 열어 읽으면 그것은 파이썬 연결의 값이지 C++ 연결의 값이 아니다.
그래서 C++ 쪽은 살아 있는 연결이 스스로 보고해야 하고, `NoteExTests.exe "[pragma-emit]"`
가 `NOTEEX_PRAGMA_OUT` 에 기록한다.

SQLite 라이브러리 버전은 나란히 적되 **판정하지 않는다.** 파이썬 3.49.1 과 house 3.50.4 로
원래 다르고, 그 차이가 실효값을 흔드는지가 질문이지 버전 자체가 질문이 아니다. 흔들면
값 비교에서 잡힌다.

```bash
python tools/gates/check_connection_parity.py --self-test   # C++ 빌드 없이
python tools/gates/check_connection_parity.py
```

---

# 시험 추적성 게이트의 사거리 (재확인)

초록은 **주석이 거짓말을 하지 않는다**까지다. 주장된 node ID 가 실재하는 함수를 가리킨다는
뜻이지, 그 C++ 시험이 가리킨 파이썬 시험과 같은 계약을 본다는 뜻이 아니다. 그 대응은
사람이 판정하거나 다른 수단이 해야 한다.

---

# 공용 지원 모듈

`migration_reference.py` 는 게이트가 아니다. 네 게이트가 공유하는 원본 적재와 덤프
구현만 모았고 **판정하지 않는다.** 이 모듈에도 스키마 SQL 은 한 줄도 없다 -
마이그레이션은 원본을 적재해 직접 호출하고 등록 순서는 원본 `migrations/__init__.py`
를 그대로 읽는다. 적재된 모듈이 이 저장소의 것인지 `__file__` 로 확인하므로 다른 곳에
설치된 동명 패키지가 가로채면 오류로 멈춘다.

정규화 규칙은 이 모듈이 아니라 **각 게이트의 docstring** 이 소유한다. 읽는 사람이 규칙을
찾을 자리는 게이트 문서이지 구현이 아니다.

# 셸 실기동 게이트 (shell_smoke.ps1 — T0 일괄 실행기 09~12)

`shell_smoke.ps1` 은 실제 `NoteEx.exe` 를 띄워 **창이 뜨고(제목 일치), 정상 종료되고(CloseMainWindow → rc 0),
INI 가 재생성되는지**를 보는 양방향 프로브다. `run_t0_gates.ps1` 이 09(known-good)·10(창 없음)·11(INI 미기록)·
12(실행 파일 부재) 네 번 부른다.

## W3 계약 (2026-08-21 개정, 사용자 A 확정)

W0 계약(인자 없는 기동 → 제목 `NoteEx` → exe 옆 `.ini`)은 W3 셸과 어긋났다 — 제목은 `<문서 제목> — pyNote`, INI 는
`%LOCALAPPDATA%\pyNote\pyNote\NoteEx.ini`, 그리고 **인자 없는 기동은 사용자 기본 DB(`%APPDATA%\pyNote\pyNote\pynote.sqlite3`)를 연다.**
게이트가 실데이터를 여는 구조를 끊기 위해 두 인자를 추가했다.

- `-Database <path>`: 앱에 `--database="<path>"` 로 넘기는 격리 DB. 부모 디렉터리를 만들어 준다.
- `-LocalAppData <dir>`: 자식 프로세스의 `LOCALAPPDATA` 를 이 임시 루트로 바꾼다(앱 INI·로컬 상태가 여기로 간다).
- `-IniPath` 는 종전대로 **명시**한다(격리 루트 안 `pyNote\pyNote\NoteEx.ini`). `-ExpectedTitle` 은 빈 DB 첫 문서 제목 `Note 1 — pyNote`.
- 둘 다 비우면 W0 계약 그대로 돈다(하위 호환) — 단 T0 실행기는 항상 격리 인자를 넘긴다.

실행기는 실행마다 `%TEMP%\NoteEx-T0-shell-<guid>\{db,local}` 을 만들고 끝에 지운다(트랜스크립트에 정리 결과 기록).
10번(`-Exe NoteExTests.exe`)은 창이 없으니 격리 인자 없이 rc=1, 11번은 격리 DB 로 **앱은 정상 기동·종료**하되
`-IniPath` 를 없는 경로로 줘 INI 술어에서만 rc=1 — 이제야 INI 분기 자체의 판별력을 시험한다(종전엔 제목 불일치로
먼저 떨어져 공허했다). 12번은 실행 파일 부재 rc=2.

## 실행

```powershell
pwsh -NoProfile -File tools/gates/shell_smoke.ps1 -Database $env:TEMP\x\db\p.sqlite3 -LocalAppData $env:TEMP\x\local -IniPath $env:TEMP\x\local\pyNote\pyNote\NoteEx.ini -ExpectedTitle 'Note 1 — pyNote'
pwsh -NoProfile -File tools/gates/run_t0_gates.ps1   # 기본 13게이트 / -WithSmoke 14게이트(대화형 전용, W-단위 aggregate 종결 판정에 필수; -SmokeIdleSeconds 60 권장), 셸 프로브 격리 루트 자동
```

## 실패는 무슨 뜻인가

- 09 rc=1 `창 생성 전 종료` — 기동 즉사(2026-08-21 실측: 초안 잔존 시 `OpenCard`→`NoOp` 오판이 원인이었다). 먼저 `--database` 를 새 임시 경로로 바꿔 재현 여부를 가른다.
- 09 rc=1 `창 제목 불일치` — 제목 조립(`ComposeWindowTitle`) 또는 첫 문서 제목 규칙이 바뀐 것. `-ExpectedTitle` 을 바꾸는 것은 계약 개정이다(이 절 갱신 + 사용자 확인).
- 09 rc=1 `INI 가 재생성되지 않았다` — INI 위치 계약이 바뀐 것(`-IniPath` 와 앱 D8 설정 경로 대조).
- 11 rc=0 — INI 분기 판별력 상실(없는 경로를 줬는데 통과) — 게이트 결함.

---

# 다중 창 수명주기 스모크 게이트 (multiwindow_lifecycle_smoke.ps1 — T0 14, 옵트인)

`shell_smoke.ps1` 이 창 하나의 기동·제목·종료·INI 를 보는 데 반해, 이 스모크는 실제 `NoteEx.exe` 를
여러 번 띄워 **다중 창 수명주기 전체**를 37개 술어로 본다. 덮는 범위는 단일 인스턴스 라우팅, 비마지막
닫기와 마지막 닫기, 재시작 복원(1창·2창), 강제 종료 후 재획득, D8 INI·사용자 기본 DB·레지스트리 불변,
프로세스·임시 자산 잔여 0, PerMonitorV2 와 DPI, geometry 재설정·화면 밖 교정·창별 키, 첫 붙여넣기와
카드 열기·저장, 찾기/바꾸기·이력·모달리스 검색·포커스 모드, 재시작 UI 상태 복원, 상태 표시줄·창 제목,
기동 유지보수 백업이다. 출력 끝에 `PREDICATE <이름>=True|False` 37줄이 나오고 하나라도 False 면 rc=1 이다.
`-Executable` 은 유일한 필수 인자다.

## 왜 기본 목록이 아니라 옵트인인가

이 스모크는 **전경 창을 잡아 실제 키 입력을 넣는다.** 같은 데스크톱에서 사람이 키보드를 만지고 있으면
전경을 뺏겨 술어가 무더기로 False 가 된다 — 제품 결함이 아니라 관측 장치의 경합이다. 그래서 T0 일괄
실행기의 기본 13게이트에는 넣지 않고 `-WithSmoke` 로만 켠다. 원격 세션·잠금 화면은
`SetForegroundWindow` 권한이 없어 실행해도 의미가 없다(대화형 데스크톱 전용).

`-SmokeIdleSeconds <초>` 를 주면 14번 실행 **직전** `GetLastInputInfo` 기준 사용자 유휴가 그 값에 이를
때까지 5초 간격으로 기다린다(상한 `-SmokeIdleWaitMinutes`, 기본 10분). 상한을 넘겨도 **스킵하지 않고
그대로 실행**하고 트랜스크립트에 `유휴 대기 상한 초과` 를 남긴다. 14번 상세에는 실행 직전 유휴 초,
`query session` 활성 세션, 전경 창 hwnd·제목 3줄이 남는다 — 사후에 전경 경합인지 제품 결함인지를 이 3줄로
가른다(유휴 조건 2/2 통과 실측 2026-08-22).

## 실행

```powershell
pwsh -NoProfile -File tools\gates\multiwindow_lifecycle_smoke.ps1 -Executable x64\ReleaseMD\NoteEx.exe
pwsh -NoProfile -File tools/gates/run_t0_gates.ps1 -WithSmoke -SmokeIdleSeconds 60   # 14게이트 일괄
```

## 실패는 무슨 뜻인가

- 전경·포커스 계열 술어(`RuntimeMenuCommandsAndAcceleratorDispatch`·`ModelessSearchQueryFocus`·
  `FindReplaceVisibilityAndFocus` 등)만 무더기 False — 전경 경합을 먼저 의심한다. 상세 3줄의 유휴 초와
  전경 창을 보고 유휴를 확보해 재실행한다.
- 술어 1~2개만 False — 그 계약의 제품 결함으로 다룬다. 술어 이름이 곧 계약 이름이다.
- 술어를 지우거나 기대를 낮추는 것은 게이트 약화다. 37 술어 집합의 변경은 계약 개정이다(이 절 갱신 +
  사용자 확인).

---

# capability 추적표 gate 명령 열 계약 (check_capability_matrix.py)

추적표의 `gate 명령` 열은 **그 행 하나를 다시 닫는 명령**이다. 검사기는 행 ID 에서 파생한
probe(`WTL-CAP-XX-NNN`)를 기준으로 아래 **3형만** 받고 그 밖은 `GATE_MISMATCH` 다.

| 형 | 명령 | 언제 쓰나 |
|---|---|---|
| 이름형 | `x64\ReleaseMD\NoteExTests.exe "WTL-CAP-XX-NNN"` | 케이스 **이름 자체가** probe ID 일 때(W2 관례) |
| 태그형 | `x64\ReleaseMD\NoteExTests.exe "[WTL-CAP-XX-NNN]"` | 닫는 케이스에 probe ID **태그**가 붙어 있을 때(W3 관례) |
| 스모크형 | `pwsh -NoProfile -File tools\gates\multiwindow_lifecycle_smoke.ps1 -Executable x64\ReleaseMD\NoteEx.exe` | Catch2 케이스 없이 스모크 술어로만 닫는 행 |

Catch2 는 `[` 로 시작하지 않는 토큰을 태그가 아니라 **이름 패턴**으로 받고, 와일드카드가 없으면 이름
완전 일치다. 그래서 태그로만 닫는 행에 이름형을 적으면 0건을 골라 rc=2 로 끝난다 — 초록도 빨강도 아닌
공허한 게이트다. 3형 분리가 그 공허를 막는다. 스모크형 행은 어느 술어로 닫았는지를 완료 증거 열에
`PREDICATE <이름>` 으로 명시한다(명령 자체는 37 술어 전체를 돌린다).

## 태그 부여 규약

- probe ID 태그는 **그 capability 를 닫는 케이스**에 붙인다. 기존 태그·케이스 이름·본문은 그대로 두고
  태그만 덧붙인다 — **케이스 이름 변경은 금지**다(이름이 다른 게이트·대장의 선택자다).
- 한 케이스가 여러 행을 닫으면 태그를 여러 개 붙인다(예
  `[W3-multi-window-lifecycle][WTL-CAP-FI-121][WTL-CAP-PL-021][WTL-CAP-PL-023][WTL-CAP-NC-032]`).
- 한 행을 여러 케이스가 닫으면 그중 한 케이스에만 붙여도 선택자는 성립한다.
- 두 capability 가 한 태그를 공유하면 한쪽이 회귀해도 다른 쪽 셀이 초록으로 남는다 — 행마다 자기 태그를 준다.

## `--tests-exe` 교차 검증

정적 검사는 열의 **문자열 형태**만 본다. 그 선택자가 실제로 케이스를 고르는지는 실행본이 있어야 안다.

```powershell
# 정적 — 행·순서·내용·owner·probe·필수 열·gate 형태
python NoteEx/tools/gates/check_capability_matrix.py --source "docs/20260819_2123_Sol_max_WTL포팅_F_a01_errata-01.md" --matrix "docs/20260819_2026_Sol_max_WTL포팅_capability추적표-01.md"
# 정적 + 선택자 실재 교차 검증
python NoteEx/tools/gates/check_capability_matrix.py --source "docs/20260819_2123_Sol_max_WTL포팅_F_a01_errata-01.md" --matrix "docs/20260819_2026_Sol_max_WTL포팅_capability추적표-01.md" --tests-exe NoteEx\x64\ReleaseMD\NoteExTests.exe
```

- 대상 = **완료 증거 열이 `PASS` 로 시작하는 행**만. 미실시·이월 행은 아직 닫히지 않았으므로 제외한다.
- 태그형은 `--list-tags` 에 `[probe]` 가, 이름형은 `--list-tests --verbosity quiet` 에 probe 와 같은 줄이
  있어야 한다(기본 `--list-tests` 는 긴 이름을 80열에서 접어 오탐을 낸다). 스모크형은 Catch2 선택자가
  아니므로 검사 대상이 아니다 — 술어 실재는 스모크 실행이 소유한다.
- 실패 = `GATE_SELECTOR_EMPTY: gate 선택자가 케이스를 고르지 못한다: <행 ID>` (rc=1). 뜻은 "완료 증거는
  PASS 라는데 그 명령으로는 아무 케이스도 돌지 않는다"이다.
- `--tests-exe` 를 주지 않으면 종전과 같은 정적 검사만 하고 rc 의미도 그대로다.
- **실행본이 낡으면 이 검사도 낡는다.** 태그를 소스에 넣고 빌드하지 않은 채 돌리면 새 태그가 전건
  `GATE_SELECTOR_EMPTY` 로 뜬다 — 검사기 결함이 아니라 실행본과 소스의 시차다.

## 이 열을 고치려면

허용 목록 개정은 **게이트 의미 변경**이다. `check_capability_matrix.py` 의 `allowed_gates()` 를 고치는
일은 이 절 갱신과 사용자 확인을 함께 거친다(이번 3형 허용의 근거 = 2026-08-22 사용자 `1A` 확정).
`--self-test` 는 정상 표본 수용(`허용 형식 수용: 태그형`·`스모크형`)과 `gate 명령 변경 → GATE_MISMATCH`
돌연변이 거부를 **양방향으로** 지킨다 — 형을 추가하면 그 두 방향을 함께 늘린다. 이름형은 W2 행이 쓰고
있으므로 제거하지 않는다.

---

# 다중 창 수명주기 스모크 — `ActualPageChildWindows` 카드 목록 클래스명 기대값 개정 (W4 S1, 2026-08-22)

- 변경: `multiwindow_lifecycle_smoke.ps1` 의 `ActualPageChildWindows` 술어에서 카드 목록(컨트롤 ID 2101)의
  클래스명 기대값을 `ListBox` → `NoteExCardList` 로 바꿨다(853행 토큰 1건). 이력 목록(2105)의 `ListBox`
  기대값과 나머지 36 술어는 불변이다. 술어의 의미(자식 2101~2106 실재·클래스·가시성)는 그대로이고
  기대값만 바뀐다(37 술어 집합 불변).
- 근거: W4 S1 이 W3 의 `LISTBOX` 자리표시자를 자작 `CWindowImpl` 컨트롤 `C_CARD_LIST`(등록 클래스
  `NoteExCardList`, `NoteEx/NoteEx/CCardList.h`)로 교체했다 — 자작 컨트롤은 시스템 클래스명 `ListBox` 로
  등록될 수 없다. 기대값 변경은 게이트 의미 변경이므로 사용자 확인을 거쳤다(2026-08-22 08:11 전용 봇 ask
  회신 `승인 NoteExCardList + 1A 태그형(추천)`; W4 지시서 §2 결정 2).
- LB 호환: 자작 컨트롤이 `LB_GETCOUNT`·`LB_GETCURSEL`·`LB_SETCURSEL`·`LB_GETTOPINDEX`·`LB_SETTOPINDEX` 를
  같은 메시지 번호·정수 반환 규약(`LB_ERR` 포함)으로 구현하므로 프로세스 밖 메시지 프로브 술어 3건
  (`FirstPasteCreatesConnectedCard`·`ListEnterOpensStoredCard`·`RestartRestoresPageUiState`)은 개정 없이
  통과한다(W4 지시서 §2 결정 1; 전용 프로브 메시지로의 이관은 W5 이후 별건 게이트 개정).
- 실패는 무슨 뜻인가: `ActualPageChildWindows=False` 이고 다른 술어가 True 면 카드 목록 컨트롤의 클래스
  등록(`DECLARE_WND_CLASS_EX(L"NoteExCardList", …)`)이나 페이지의 컨트롤 생성(ID 2101·가시)이 깨진 것이다.
  메시지 프로브 3건만 False 면 LB 호환 메시지 구현의 회귀다.
