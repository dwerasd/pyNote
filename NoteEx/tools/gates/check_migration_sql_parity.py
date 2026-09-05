#!/usr/bin/env python3
"""마이그레이션 SQL 문장 축자(逐字) 이식 게이트(v0001~v0010 전건).

파이썬 원본 `migrate()` 가 **실제로 발행하는 문장**과, 그 C++ 이식본이 담은
`u8R"SQL( ... )SQL"` 원시 문자열 리터럴을 **순서대로 바이트 단위 대조**한다.
이식 계약이 "공백 한 칸까지 그대로"이므로 이 게이트는 들여쓰기·빈 줄·문장
앞뒤의 개행을 검사 대상으로 삼는다. 다시 포맷하거나 정리하는 것은 계약 위반이고,
그런 지적의 해법은 원문 복원이지 게이트 완화가 아니다.

**왜 소스 파싱이 아니라 실행 기록인가**(T-R2 에서 바뀐 부분): `v0001` 만
모듈 상수 `STATEMENTS` 를 노출하고 나머지 여덟은 `connection.execute(...)` 를
인라인으로 부른다. `v0003` 은 지역 튜플을 만들어 돌고, DDL 앞에서 기존 행을
검사하는 `SELECT` 두 건을 먼저 발행한다. 이 모양들을 정적으로 파싱하는 것은
깨지기 쉽고, 무엇보다 **원본이 어떻게 쓰였는지**를 검사하게 된다. 기록 프록시로
`migrate()` 의 발행을 잡으면 검사 대상이 **원본이 무엇을 하는지**가 되고, 원본의
문장 표현 방식이 바뀌어도 게이트는 그대로 성립한다. 발행은 흉내가 아니라 실제
실행이라(사다리를 진짜로 올린다) `v0005` 의 `ALTER TABLE` 처럼 앞 버전 상태가
있어야 성립하는 문장도 그대로 잡힌다.

대조 대상은 `migrate()` 가 발행한 **전부**다. 꼬리의 `schema_version` 갱신도
포함이다(T-R1 의 유일한 문서화 일탈은 T-R2 의 `ExecuteBoundInt64` 로 사라졌다).
뒤집어 말하면 **발행되지 않는 SQL 에 `SQL` 구분자를 쓰면 안 된다** - 잉여
리터럴로 잡힌다.

검사 규칙:
  * 리터럴 접두는 **`u8` 가 필수**다(SPEC §1(a)). 이 기계에서 좁은 리터럴은
    CP949 로 컴파일되므로, 본문이 바이트까지 같아도 SQLite 에 넘어가는 바이트가
    UTF-8 이 아니게 된다. `v0003` 의 SQL 안에는 한국어가 들어 있다. 오늘 한국어가
    없는 파일에도 같은 규칙을 적용하는 이유는, 파일 내용에 따라 켜졌다 꺼지는
    규칙은 누군가 한국어를 처음 넣는 순간 조용히 깨지기 때문이다. 지켜야 할
    불변식은 "SQLite 에 넘기는 SQL 원문은 UTF-8"이고, 그것은 전건 성립하거나
    불변식이 아니다.
  * 정규화는 둘뿐이다. CRLF -> LF(저장소가 `* text=auto` 로 LF 를 보관하므로
    줄끝은 이 게이트가 지킬 대상이 아니다), UTF-8 BOM 제거(C++ 소스는 BOM 포함이
    프로젝트 표준이다). 그 외에는 **아무것도 정규화하지 않는다** - 들여쓰기·말미
    공백·빈 줄은 보호 대상 그 자체다.
  * 리터럴이 어떤 문맥에 놓였는지는 보지 않는다. 배열 원소든
    `reinterpret_cast<const char*>(...)` 안이든 같다.

파이썬 쪽은 원본 모듈을 적재해 `migrate` 를 **직접 호출**하고, 등록 순서는 원본
`MIGRATIONS` 튜플을 그대로 읽는다. 게이트가 검사 대상의 사본을 들고 있으면
아무것도 증명하지 못하므로, 이 파일에는 스키마 SQL 이 한 줄도 들어 있지 않다.

종료 코드:
  0  통과(등록된 짝 전건, 문장 수·내용·접두 일치)
  1  불일치 검출(또는 자기시험 기대 불일치)
  2  사용법·환경 오류(경로 없음, 적재 실패, **C++ 이식본 없음**, **추출 대상 0건**)

C++ 이식본이 아직 없는 버전이 하나라도 있으면 2 다. 있는 것만 보고 통과라고
말하지 않는다 - 판정할 수 없는 상태는 통과가 아니다.
"""

from __future__ import annotations

import argparse
import difflib
import re
import sys
import tempfile
from pathlib import Path
from typing import NamedTuple, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))

import migration_reference as reference  # noqa: E402
from migration_reference import (  # noqa: E402
    CapturedStatement,
    MigrationEntry,
    display_path,
)

# 추출 대상 원시 문자열의 구분자. SPEC §3 이 고정한 값이다.
RAW_DELIMITER = "SQL"

# 요구되는 인코딩 접두. SPEC §1(a).
REQUIRED_PREFIX = "u8"

# 원시 문자열 접두(u8R"..", LR".." 등)를 인정하되, 식별자 꼬리의 R 은 배제한다.
_RAW_PREFIX = re.compile(r"(?:u8|u|U|L)?R$")
_IDENTIFIER_TAIL = re.compile(r"[0-9A-Za-z_]")

# 구분자에 올 수 없는 문자(C++ 표준: 공백·괄호·역슬래시 등).
_DELIMITER_STOP = set(" ()\\\t\v\f\n\r")


class RawLiteral(NamedTuple):
    """C++ 소스에서 추출한 `u8R"SQL( ... )SQL"` 리터럴 1건."""

    index: int  # 소스 등장 순서(0-base)
    line: int  # 여는 따옴표가 있는 줄 번호(1-base)
    text: str  # 구분자 사이 본문. 원문 그대로다
    prefix: str  # 인코딩 접두("u8" / "" / "L" / "u" / "U")


class Mismatch(NamedTuple):
    """문장 1건의 불일치."""

    index: int
    line: int | None
    detail: list[str]


class PairResult(NamedTuple):
    """마이그레이션 1건(파이썬 <-> C++)의 판정 결과."""

    version: int
    stem: str
    cpp_path: Path | None
    statements: int
    literals: int
    mismatches: list[Mismatch]


def normalise_newlines(text: str) -> str:
    """CRLF 만 LF 로 낮춘다. 단독 CR 은 건드리지 않는다(그것은 진짜 차이다)."""
    return text.replace("\r\n", "\n")


def read_source_text(path: Path) -> str:
    """소스를 UTF-8(BOM 허용)로 읽고 줄끝만 정규화한다."""
    return normalise_newlines(path.read_bytes().decode("utf-8-sig"))


def _skip_line_comment(text: str, start: int) -> int:
    """`//` 주석 끝(개행 직전) 위치를 돌려준다. 역슬래시 줄이음을 따라간다."""
    i = start + 2
    n = len(text)
    while i < n:
        if text[i] == "\\":
            i += 1
            if i < n and text[i] == "\r":
                i += 1
            if i < n and text[i] == "\n":
                i += 1
            continue
        if text[i] == "\n":
            break
        i += 1
    return i


def _skip_block_comment(text: str, start: int) -> int:
    """`/* */` 주석 끝 다음 위치를 돌려준다. 미종결이면 파일 끝이다."""
    end = text.find("*/", start + 2)
    return len(text) if end == -1 else end + 2


def _skip_quoted(text: str, start: int, quote: str) -> int:
    """일반 문자열·문자 리터럴 끝 다음 위치를 돌려준다.

    미종결 리터럴이 파일 나머지를 통째로 삼키는 것을 막기 위해 줄 끝에서도
    상태를 푼다. 유효한 C++ 이 아닌 입력에서 추출이 폭주하지 않게 하는 방어다.
    """
    i = start + 1
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == "\\":
            i += 2
            continue
        if ch == quote:
            return i + 1
        if ch == "\n":
            return i
        i += 1
    return n


def _raw_string_prefix(text: str, quote_pos: int) -> str | None:
    """따옴표 앞이 원시 문자열 접두면 인코딩 접두를 돌려준다(없으면 None).

    반환값에서 끝의 `R` 은 뺀다 - `u8R` 이면 `"u8"`, `R` 이면 `""` 이다.
    """
    head = text[max(0, quote_pos - 3) : quote_pos]
    match = _RAW_PREFIX.search(head)
    if match is None:
        return None
    prefix = head[match.start() : match.end()]
    prefix_start = quote_pos - len(prefix)
    if prefix_start > 0 and _IDENTIFIER_TAIL.match(text[prefix_start - 1]):
        return None
    return prefix[:-1]


def _is_char_literal_open(text: str, quote_pos: int) -> bool:
    """작은따옴표가 문자 리터럴인지 숫자 구분자(1'000'000)인지 가른다."""
    if quote_pos == 0:
        return True
    return _IDENTIFIER_TAIL.match(text[quote_pos - 1]) is None


def extract_cpp_literals(text: str) -> list[RawLiteral]:
    """C++ 소스에서 `R"SQL( ... )SQL"` 본문을 등장 순서대로 뽑는다.

    주석·일반 문자열·문자 리터럴 안의 표기는 무시하고, 구분자가 `SQL` 이 아닌
    원시 문자열도 건너뛴다. 본문은 어떤 가공도 하지 않는다. 접두가 `u8` 이
    아닌 것도 **일단 뽑는다** - 접두 위반을 "리터럴이 없다"가 아니라 "이
    리터럴이 틀렸다"로 보고하기 위해서다.
    """
    literals: list[RawLiteral] = []
    i = 0
    n = len(text)

    while i < n:
        ch = text[i]

        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            i = _skip_line_comment(text, i)
            continue

        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            i = _skip_block_comment(text, i)
            continue

        if ch == '"':
            prefix = _raw_string_prefix(text, i)
            if prefix is not None:
                delimiter_chars: list[str] = []
                j = i + 1
                while j < n and text[j] not in _DELIMITER_STOP and len(delimiter_chars) <= 16:
                    delimiter_chars.append(text[j])
                    j += 1
                if j < n and text[j] == "(":
                    delimiter = "".join(delimiter_chars)
                    closer = ")" + delimiter + '"'
                    body_start = j + 1
                    end = text.find(closer, body_start)
                    stop = n if end == -1 else end + len(closer)
                    if delimiter == RAW_DELIMITER and end != -1:
                        literals.append(
                            RawLiteral(
                                index=len(literals),
                                line=text.count("\n", 0, i) + 1,
                                text=text[body_start:end],
                                prefix=prefix,
                            )
                        )
                    i = stop
                    continue
            i = _skip_quoted(text, i, '"')
            continue

        if ch == "'" and _is_char_literal_open(text, i):
            i = _skip_quoted(text, i, "'")
            continue

        i += 1

    return literals


def _visible(line: str) -> str:
    """탭·말미 공백을 눈에 보이게 만든다. 비교가 아니라 표시 전용이다."""
    shown = line.replace("\t", "\\t")
    trailing = len(line) - len(line.rstrip(" "))
    if trailing:
        shown = f"{shown}[말미 공백 {trailing}]"
    return shown


def _first_difference(left: str, right: str) -> str:
    """첫 불일치 지점의 오프셋과 앞뒤 문맥을 사람이 읽을 수 있게 만든다."""
    limit = min(len(left), len(right))
    offset = limit
    for position in range(limit):
        if left[position] != right[position]:
            offset = position
            break
    window = 24
    low = max(0, offset - window)
    return (
        f"첫 불일치 오프셋 {offset} "
        f"(파이썬 {len(left)}자 / C++ {len(right)}자)\n"
        f"      파이썬: {left[low : offset + window]!r}\n"
        f"      C++   : {right[low : offset + window]!r}"
    )


def _unified(left: str, right: str) -> list[str]:
    """문장 1건의 통합 diff. 줄 분해는 `split` 이라 말미 개행도 한 줄로 남는다."""
    return [
        line.rstrip("\n")
        for line in difflib.unified_diff(
            [_visible(part) for part in left.split("\n")],
            [_visible(part) for part in right.split("\n")],
            fromfile="python/migrate()",
            tofile='cpp/u8R"SQL(...)SQL"',
            lineterm="",
        )
    ]


def compare_statements(
    statements: Sequence[CapturedStatement], literals: Sequence[RawLiteral]
) -> list[Mismatch]:
    """발행 문장 목록과 리터럴 목록을 순서대로 바이트 대조한다."""
    mismatches: list[Mismatch] = []
    common = min(len(statements), len(literals))

    for index in range(common):
        expected = normalise_newlines(statements[index].sql)
        actual = literals[index]
        detail: list[str] = []

        if actual.prefix != REQUIRED_PREFIX:
            shown = actual.prefix if actual.prefix else "(없음)"
            detail.append(
                f'인코딩 접두가 `{shown}R"{RAW_DELIMITER}(` 다. '
                f'`{REQUIRED_PREFIX}R"{RAW_DELIMITER}(` 여야 한다(SPEC §1(a)) - '
                "이 기계에서 좁은 리터럴은 CP949 로 컴파일되므로 SQLite 에 "
                "넘어가는 바이트가 UTF-8 이 아니게 된다."
            )
        if expected != actual.text:
            detail.append(_first_difference(expected, actual.text))
            detail.extend(f"      {line}" for line in _unified(expected, actual.text))

        if detail:
            mismatches.append(Mismatch(index=index, line=actual.line, detail=detail))

    for index in range(common, len(statements)):
        mismatches.append(
            Mismatch(
                index=index,
                line=None,
                detail=[
                    "C++ 쪽에 대응 리터럴이 없다(이식 누락).",
                    f"      파이썬 원문: {statements[index].sql!r}",
                ],
            )
        )

    for index in range(common, len(literals)):
        extra = literals[index]
        mismatches.append(
            Mismatch(
                index=index,
                line=extra.line,
                detail=[
                    "파이썬 쪽에 대응 문장이 없다(잉여 리터럴).",
                    "      migrate() 가 발행하지 않는 SQL 에 구분자 "
                    f"`{RAW_DELIMITER}` 를 쓰면 안 된다 - 추출 대상이 된다.",
                    f"      C++ 원문: {extra.text!r}",
                ],
            )
        )

    return mismatches


def _report_pair(result: PairResult) -> None:
    """쌍 1건의 불일치를 stderr 로 낸다."""
    location = display_path(result.cpp_path) if result.cpp_path else "(C++ 없음)"
    print(
        f"[v{result.version:04d} {result.stem}] {location} - "
        f"파이썬 {result.statements}문장 / C++ {result.literals}리터럴",
        file=sys.stderr,
    )
    for item in result.mismatches:
        where = f"{location}:{item.line}" if item.line else "(위치 없음)"
        print(f"  [문장 {item.index}] {where}", file=sys.stderr)
        for line in item.detail:
            print(f"    {line}", file=sys.stderr)


def run_check(cpp_dir: Path | None = None) -> int:
    """등록된 짝을 전건 대조한다. 종료 코드를 돌려준다."""
    root = reference.CPP_MIGRATIONS_DIR if cpp_dir is None else cpp_dir
    try:
        entries = reference.load_registry()
    except (OSError, SyntaxError, ImportError, AttributeError, TypeError, ValueError) as exc:
        print(f"오류: 파이썬 원본 적재 실패 - {exc}", file=sys.stderr)
        return 2

    missing = [
        entry for entry in entries if reference.find_cpp_source(entry, root) is None
    ]
    if missing:
        print(
            "오류: C++ 이식본이 없는 마이그레이션이 "
            f"{len(missing)}/{len(entries)}건이다.",
            file=sys.stderr,
        )
        for entry in missing:
            print(
                f"      v{entry.version:04d} {entry.stem} -> "
                f"{display_path(root / (entry.stem + '.cpp'))}",
                file=sys.stderr,
            )
        print(
            "      아직 작성되지 않았다면 이 게이트는 판정할 수 없다(환경 오류). "
            "있는 것만 보고 통과라고 말하지 않는다.",
            file=sys.stderr,
        )
        return 2

    with tempfile.TemporaryDirectory(prefix="sqlparity_") as work:
        try:
            captured = reference.capture_statements(
                entries, Path(work) / "ladder.db", 1_700_000_000_000_000
            )
        except Exception as exc:  # 원본 실행 실패는 환경 오류다
            print(f"오류: 파이썬 사다리 실행 실패 - {exc}", file=sys.stderr)
            return 2

    results: list[PairResult] = []
    for entry in entries:
        cpp_path = reference.find_cpp_source(entry, root)
        assert cpp_path is not None  # 위에서 전건 확인했다
        statements = captured.get(entry.version, ())
        try:
            literals = extract_cpp_literals(read_source_text(cpp_path))
        except (OSError, UnicodeDecodeError) as exc:
            print(
                f"오류: C++ 이식본 읽기 실패 - {display_path(cpp_path)}: {exc}",
                file=sys.stderr,
            )
            return 2

        if not statements:
            print(
                f"오류: v{entry.version:04d} 의 발행 문장이 0건이다 - "
                f"{display_path(entry.source)}. 대조할 것이 없으므로 실패로 처리한다.",
                file=sys.stderr,
            )
            return 2
        if not literals:
            print(
                f'오류: 추출 대상 0건 - {display_path(cpp_path)} 에 '
                f'{REQUIRED_PREFIX}R"{RAW_DELIMITER}( ... ){RAW_DELIMITER}" '
                "리터럴이 없다. 게이트가 아무것도 증명하지 못하므로 실패로 처리한다.",
                file=sys.stderr,
            )
            return 2

        results.append(
            PairResult(
                version=entry.version,
                stem=entry.stem,
                cpp_path=cpp_path,
                statements=len(statements),
                literals=len(literals),
                mismatches=compare_statements(statements, literals),
            )
        )

    failed = [result for result in results if result.mismatches]
    for result in results:
        status = "FAIL" if result.mismatches else "PASS"
        print(
            f"  {status}  v{result.version:04d} {result.stem}: "
            f"문장 {result.statements}건 / 리터럴 {result.literals}건"
        )

    if failed:
        print("", file=sys.stderr)
        for result in failed:
            _report_pair(result)
        total = sum(len(result.mismatches) for result in failed)
        print(
            f"SQL 축자 이식 위반 {total}건 / 마이그레이션 {len(failed)}건 "
            f"(대조 {sum(result.statements for result in results)}문장)",
            file=sys.stderr,
        )
        return 1

    print(
        f"마이그레이션 {len(results)}건 / 문장 "
        f"{sum(result.statements for result in results)}건 전건 바이트 일치"
    )
    return 0


def _self_test_fixture_dir() -> Path:
    return Path(__file__).resolve().parent / "fixtures" / "schema_parity" / "static"


def _fixture_statements(reference_path: Path, work: Path) -> tuple[CapturedStatement, ...]:
    """fixture 마이그레이션을 기록 프록시로 돌려 발행 문장을 얻는다.

    실제 대조와 **같은 경로**를 쓴다 - 자기시험이 다른 경로를 타면 증명하는
    바가 달라진다.
    """
    import importlib.util

    spec = importlib.util.spec_from_file_location("static_fixture_migration", reference_path)
    if spec is None or spec.loader is None:
        raise ImportError(f"모듈 스펙을 만들 수 없다: {display_path(reference_path)}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    try:
        spec.loader.exec_module(module)
    finally:
        sys.modules.pop(spec.name, None)

    migrate = getattr(module, "migrate", None)
    if migrate is None or not callable(migrate):
        raise AttributeError(f"호출 가능한 migrate 가 없다: {display_path(reference_path)}")

    entry = MigrationEntry(version=1, migrate=migrate, source=reference_path)
    captured = reference.capture_statements(
        (entry,), work / "fixture.db", 1_700_000_000_000_000
    )
    return captured[1]


def run_self_test() -> int:
    """fixtures 로 게이트 자신을 양방향 검증한다. 실제 소스에 의존하지 않는다."""
    fixtures = _self_test_fixture_dir()
    reference_path = fixtures / "reference_migration.py"
    good = fixtures / "good.cpp"
    empty = fixtures / "no_literals.cpp"
    bad_files = sorted(fixtures.glob("bad_*.cpp"))

    if not fixtures.is_dir():
        print(f"오류: fixture 디렉터리 없음 - {display_path(fixtures)}", file=sys.stderr)
        return 2
    for required in (reference_path, good, empty):
        if not required.is_file():
            print(f"오류: fixture 없음 - {display_path(required)}", file=sys.stderr)
            return 2
    if not bad_files:
        print(
            "오류: known-bad fixture 가 0건이다. 잡을 것을 잡는지 증명하지 못하므로 "
            "실패로 처리한다.",
            file=sys.stderr,
        )
        return 2

    failures = 0

    with tempfile.TemporaryDirectory(prefix="sqlparity_selftest_") as work:
        work_dir = Path(work)
        try:
            expected = _fixture_statements(reference_path, work_dir)
        except Exception as exc:
            print(f"오류: fixture 기준 적재 실패 - {exc}", file=sys.stderr)
            return 2
        if not expected:
            print("오류: fixture 기준 발행 문장이 0건이다.", file=sys.stderr)
            return 2

        bound = sum(1 for statement in expected if statement.bound)
        print(
            f"[방향 1] known-good 수용 검사 - 기준 문장 {len(expected)}건"
            f"(바인드 {bound}건)"
        )
        good_literals = extract_cpp_literals(read_source_text(good))
        good_mismatches = compare_statements(expected, good_literals)
        if len(good_literals) != len(expected):
            print(
                f"  FAIL  {display_path(good)}: 추출 {len(good_literals)}건 "
                f"(기대 {len(expected)}건)"
            )
            failures += 1
        elif good_mismatches:
            print(f"  FAIL  {display_path(good)}: 오탐 {len(good_mismatches)}건")
            for item in good_mismatches:
                for line in item.detail:
                    print(f"        {line}")
            failures += 1
        else:
            print(f"  PASS  {display_path(good)}: 문장 {len(good_literals)}건 수용")

        print("[방향 2] known-bad 거부 검사")
        for path in bad_files:
            literals = extract_cpp_literals(read_source_text(path))
            mismatches = compare_statements(expected, literals)
            if mismatches:
                first = mismatches[0]
                summary = first.detail[0].splitlines()[0]
                print(
                    f"  PASS  {display_path(path)}: 거부 "
                    f"(불일치 {len(mismatches)}건, 문장 {first.index}: {summary})"
                )
            else:
                print(f"  FAIL  {display_path(path)}: 미탐 - 심어둔 결함을 잡지 못했다")
                failures += 1

        print("[방향 3] CRLF 정규화 - 줄끝만으로 거짓 실패하지 않는가")
        crlf_path = work_dir / "good_crlf.cpp"
        crlf_path.write_bytes(
            read_source_text(good).replace("\n", "\r\n").encode("utf-8-sig")
        )
        crlf_literals = extract_cpp_literals(read_source_text(crlf_path))
        crlf_mismatches = compare_statements(expected, crlf_literals)
        if len(crlf_literals) == len(expected) and not crlf_mismatches:
            print(f"  PASS  CRLF + BOM 사본 수용(문장 {len(crlf_literals)}건)")
        else:
            print(
                f"  FAIL  CRLF + BOM 사본에서 불일치 {len(crlf_mismatches)}건 "
                f"(추출 {len(crlf_literals)}건)"
            )
            failures += 1

        print("[방향 4] 추출 0건·이식본 부재 - 통과가 아니라 환경 오류인가")
        empty_literals = extract_cpp_literals(read_source_text(empty))
        if empty_literals:
            print(
                f"  FAIL  {display_path(empty)}: 리터럴이 없어야 하는데 "
                f"{len(empty_literals)}건 추출됐다"
            )
            failures += 1

        try:
            entries = reference.load_registry()
        except Exception as exc:
            print(f"  FAIL  실제 원본 적재 실패 - {exc}")
            return 2

        # 4-a) 짝이 다 있지만 전부 리터럴 0건인 디렉터리.
        zero_dir = work_dir / "cpp_zero"
        zero_dir.mkdir()
        for entry in entries:
            (zero_dir / f"{entry.stem}.cpp").write_bytes(empty.read_bytes())
        print("        (이어지는 stderr 한 줄은 이 검사가 기대하는 출력이다)")
        sys.stdout.flush()
        code = run_check(zero_dir)
        if code == 2:
            print("  PASS  리터럴 0건 디렉터리: 종료 코드 2(환경 오류)")
        else:
            print(f"  FAIL  리터럴 0건 디렉터리: 종료 코드 {code}(기대 2)")
            failures += 1

        # 4-b) 이식이 아직 일부만 된 상태(전이 중 실제로 겪는 상태).
        partial_dir = work_dir / "cpp_partial"
        partial_dir.mkdir()
        (partial_dir / f"{entries[0].stem}.cpp").write_bytes(good.read_bytes())
        print("        (이어지는 stderr 여러 줄은 이 검사가 기대하는 출력이다)")
        sys.stdout.flush()
        code = run_check(partial_dir)
        if code == 2:
            print(
                f"  PASS  {len(entries) - 1}/{len(entries)}건 이식본 부재: "
                "종료 코드 2(환경 오류)"
            )
        else:
            print(f"  FAIL  이식본 부재: 종료 코드 {code}(기대 2)")
            failures += 1

        print("[방향 5] 실제 등록된 짝의 파이썬 쪽이 적재·발행되는가")
        try:
            captured = reference.capture_statements(
                entries, work_dir / "real_ladder.db", 1_700_000_000_000_000
            )
        except Exception as exc:
            print(f"  FAIL  실제 원본 발행 실패 - {exc}")
            failures += 1
        else:
            empty_versions = [
                entry.version for entry in entries if not captured.get(entry.version)
            ]
            if empty_versions:
                print(f"  FAIL  발행 문장 0건인 버전이 있다: {empty_versions}")
                failures += 1
            else:
                summary = " ".join(
                    f"v{entry.version:04d}={len(captured[entry.version])}"
                    for entry in entries
                )
                print(
                    f"  PASS  마이그레이션 {len(entries)}건 전건 발행 - {summary} "
                    f"(합계 {sum(len(v) for v in captured.values())}문장)"
                )

    if failures:
        print(f"자기시험 실패 {failures}건", file=sys.stderr)
        return 1

    print(
        f"자기시험 PASS: good 1건 수용, bad {len(bad_files)}건 전건 거부, "
        "CRLF 수용, 0건 추출은 환경 오류, 실제 원본 발행 확인"
    )
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    reference.force_utf8_output()
    parser = argparse.ArgumentParser(
        description="마이그레이션 SQL 문장의 파이썬 <-> C++ 축자 대조(v0001~v0010)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--cpp-dir",
        default=None,
        metavar="PATH",
        help=(
            "C++ 이식본 디렉터리(기본: "
            "NoteEx/core/src/storage/migrations)"
        ),
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="fixtures 기반 자기검증을 수행한다(--cpp-dir 는 무시)",
    )
    args = parser.parse_args(argv)

    if args.self_test:
        return run_self_test()
    return run_check(Path(args.cpp_dir) if args.cpp_dir else None)


if __name__ == "__main__":
    sys.exit(main())
