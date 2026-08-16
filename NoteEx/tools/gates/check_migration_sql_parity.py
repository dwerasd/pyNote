#!/usr/bin/env python3
"""v0001 마이그레이션 SQL 문장 축자(逐字) 이식 게이트.

파이썬 원본 `migrations/v0001_initial.py` 의 `STATEMENTS` 튜플과, 그 C++ 이식본이
담은 `R"SQL( ... )SQL"` 원시 문자열 리터럴을 **순서대로 바이트 단위 대조**한다.
이식 계약이 "공백 한 칸까지 그대로"이므로 이 게이트는 들여쓰기·빈 줄·문장
앞뒤의 개행을 검사 대상으로 삼는다. 다시 포맷하거나 정리하는 것은 계약 위반이고,
그런 지적의 해법은 원문 복원이지 게이트 완화가 아니다.

정규화 규칙(이것만 한다):
  * CRLF -> LF. 저장소는 `* text=auto` 로 LF 를 보관하므로 줄끝은 이 게이트가
    지킬 대상이 아니다. 체크아웃 설정 때문에 생긴 CR 로 거짓 실패를 내지 않는다.
  * UTF-8 BOM 제거(C++ 소스는 BOM 포함이 프로젝트 표준이다).
그 외에는 **아무것도 정규화하지 않는다.** 들여쓰기·말미 공백·빈 줄은 보호 대상
그 자체다.

파이썬 쪽 문장은 원본 모듈을 경로로 적재해 `STATEMENTS` 를 **그대로** 읽는다.
게이트가 검사 대상의 사본을 들고 있으면 아무것도 증명하지 못하므로, 이 파일에는
스키마 SQL 이 한 줄도 들어 있지 않다.

원시 문자열 구분자는 `SQL` 로 고정이다(SPEC §3). 구분자가 다른 원시 문자열은
추출 대상이 아니며, 주석·일반 문자열 안의 `R"SQL(` 도 걸리지 않는다.

종료 코드:
  0  통과(문장 수·내용 전건 일치)
  1  불일치 검출(또는 자기시험 기대 불일치)
  2  사용법·환경 오류(경로 없음, 적재 실패, **추출 대상 0건**)

추출 대상 0건을 통과가 아니라 오류로 두는 것은 의도된 선택이다 - 대조할 것이
없는 게이트는 아무것도 증명하지 못하며, 실제 원인은 대개 경로 오타이거나 아직
작성되지 않은 이식본이다. 조용한 통과보다 붉은 실패가 싸다.
"""

from __future__ import annotations

import argparse
import difflib
import importlib.util
import re
import sys
import tempfile
from pathlib import Path
from typing import NamedTuple, Sequence

# 저장소 루트. 이 파일은 <루트>/NoteEx/tools/gates/ 에 있다.
REPO_ROOT = Path(__file__).resolve().parents[3]

DEFAULT_PYTHON_SOURCE = (
    REPO_ROOT / "src" / "pynote" / "infrastructure" / "migrations" / "v0001_initial.py"
)
DEFAULT_CPP_SOURCE = (
    REPO_ROOT
    / "NoteEx"
    / "core"
    / "src"
    / "storage"
    / "migrations"
    / "v0001_initial.cpp"
)

# 추출 대상 원시 문자열의 구분자. SPEC §3 이 고정한 값이다.
RAW_DELIMITER = "SQL"

# 원시 문자열 접두(u8R"..", LR".." 등)를 인정하되, 식별자 꼬리의 R 은 배제한다.
_RAW_PREFIX = re.compile(r"(?:u8|u|U|L)?R$")
_IDENTIFIER_TAIL = re.compile(r"[0-9A-Za-z_]")

# 구분자에 올 수 없는 문자(C++ 표준: 공백·괄호·역슬래시 등).
_DELIMITER_STOP = set(" ()\\\t\v\f\n\r")


class RawLiteral(NamedTuple):
    """C++ 소스에서 추출한 `R"SQL( ... )SQL"` 리터럴 1건."""

    index: int  # 소스 등장 순서(0-base)
    line: int  # 여는 `R"SQL(` 이 있는 줄 번호(1-base)
    text: str  # 구분자 사이 본문. 원문 그대로다


class Mismatch(NamedTuple):
    """문장 1건의 불일치."""

    index: int
    line: int | None
    detail: list[str]


def _display(path: Path) -> str:
    """경로를 슬래시로 정규화해 셸·로그 어디서나 같은 문자열이 되게 한다."""
    return str(path).replace("\\", "/")


def normalise_newlines(text: str) -> str:
    """CRLF 만 LF 로 낮춘다. 단독 CR 은 건드리지 않는다(그것은 진짜 차이다)."""
    return text.replace("\r\n", "\n")


def read_source_text(path: Path) -> str:
    """소스를 UTF-8(BOM 허용)로 읽고 줄끝만 정규화한다."""
    return normalise_newlines(path.read_bytes().decode("utf-8-sig"))


def load_python_statements(path: Path) -> tuple[str, ...]:
    """원본 마이그레이션 모듈을 경로로 적재해 `STATEMENTS` 를 그대로 돌려준다.

    패키지 설치나 가상환경 활성화에 의존하지 않는다 - 원본이 표준 라이브러리만
    가져오기 때문에 경로 적재로 충분하다.
    """
    spec = importlib.util.spec_from_file_location("pynote_reference_migration", path)
    if spec is None or spec.loader is None:
        raise ImportError(f"모듈 스펙을 만들 수 없다: {_display(path)}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    try:
        spec.loader.exec_module(module)
    finally:
        sys.modules.pop(spec.name, None)

    statements = getattr(module, "STATEMENTS", None)
    if statements is None:
        raise AttributeError(f"STATEMENTS 가 없다: {_display(path)}")
    if not isinstance(statements, tuple):
        raise TypeError(
            f"STATEMENTS 가 튜플이 아니다({type(statements).__name__}): {_display(path)}"
        )
    for position, statement in enumerate(statements):
        if not isinstance(statement, str):
            raise TypeError(
                f"STATEMENTS[{position}] 이 문자열이 아니다({type(statement).__name__})."
            )
    return tuple(normalise_newlines(statement) for statement in statements)


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


def _is_raw_string_open(text: str, quote_pos: int) -> bool:
    """따옴표 앞이 원시 문자열 접두(R, u8R, LR ...)인지 판정한다."""
    head = text[max(0, quote_pos - 3) : quote_pos]
    match = _RAW_PREFIX.search(head)
    if match is None:
        return False
    prefix_start = quote_pos - (match.end() - match.start())
    if prefix_start > 0 and _IDENTIFIER_TAIL.match(text[prefix_start - 1]):
        return False
    return True


def _is_char_literal_open(text: str, quote_pos: int) -> bool:
    """작은따옴표가 문자 리터럴인지 숫자 구분자(1'000'000)인지 가른다."""
    if quote_pos == 0:
        return True
    return _IDENTIFIER_TAIL.match(text[quote_pos - 1]) is None


def extract_cpp_statements(text: str) -> list[RawLiteral]:
    """C++ 소스에서 `R"SQL( ... )SQL"` 본문을 등장 순서대로 뽑는다.

    주석·일반 문자열·문자 리터럴 안의 표기는 무시하고, 구분자가 `SQL` 이 아닌
    원시 문자열도 건너뛴다. 본문은 어떤 가공도 하지 않는다.
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
            if _is_raw_string_open(text, i):
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
            fromfile="python/STATEMENTS",
            tofile="cpp/R\"SQL(...)SQL\"",
            lineterm="",
        )
    ]


def compare_statements(
    python_statements: Sequence[str], cpp_literals: Sequence[RawLiteral]
) -> list[Mismatch]:
    """두 문장 목록을 순서대로 바이트 대조한다."""
    mismatches: list[Mismatch] = []
    common = min(len(python_statements), len(cpp_literals))

    for index in range(common):
        expected = python_statements[index]
        actual = cpp_literals[index]
        if expected == actual.text:
            continue
        detail = [_first_difference(expected, actual.text)]
        detail.extend(f"      {line}" for line in _unified(expected, actual.text))
        mismatches.append(Mismatch(index=index, line=actual.line, detail=detail))

    for index in range(common, len(python_statements)):
        mismatches.append(
            Mismatch(
                index=index,
                line=None,
                detail=[
                    "C++ 쪽에 대응 리터럴이 없다(이식 누락).",
                    f"      파이썬 원문: {python_statements[index]!r}",
                ],
            )
        )

    for index in range(common, len(cpp_literals)):
        extra = cpp_literals[index]
        mismatches.append(
            Mismatch(
                index=index,
                line=extra.line,
                detail=[
                    "파이썬 쪽에 대응 문장이 없다(잉여 리터럴).",
                    "      STATEMENTS 밖의 SQL(예: schema_version upsert)은 구분자 "
                    f'`{RAW_DELIMITER}` 를 쓰면 안 된다 - 이 게이트의 추출 대상이 된다.',
                    f"      C++ 원문: {extra.text!r}",
                ],
            )
        )

    return mismatches


def run_check(python_path: Path, cpp_path: Path) -> int:
    """실제 두 소스를 대조한다. 종료 코드를 돌려준다."""
    if not python_path.is_file():
        print(
            f"오류: 파이썬 원본이 없다 - {_display(python_path)}",
            file=sys.stderr,
        )
        return 2
    if not cpp_path.is_file():
        print(
            f"오류: C++ 이식본이 없다 - {_display(cpp_path)}\n"
            "      아직 작성되지 않았다면 이 게이트는 판정할 수 없다(환경 오류).",
            file=sys.stderr,
        )
        return 2

    try:
        python_statements = load_python_statements(python_path)
    except (OSError, SyntaxError, ImportError, AttributeError, TypeError) as exc:
        print(
            f"오류: 파이썬 원본 적재 실패 - {_display(python_path)}: {exc}",
            file=sys.stderr,
        )
        return 2

    try:
        cpp_text = read_source_text(cpp_path)
    except (OSError, UnicodeDecodeError) as exc:
        print(
            f"오류: C++ 이식본 읽기 실패 - {_display(cpp_path)}: {exc}",
            file=sys.stderr,
        )
        return 2

    cpp_literals = extract_cpp_statements(cpp_text)

    if not python_statements:
        print(
            f"오류: STATEMENTS 가 비었다 - {_display(python_path)}. 대조할 것이 "
            "없으므로 실패로 처리한다.",
            file=sys.stderr,
        )
        return 2
    if not cpp_literals:
        print(
            f'오류: 추출 대상 0건 - {_display(cpp_path)} 에 R"{RAW_DELIMITER}( ... '
            f'){RAW_DELIMITER}" 리터럴이 없다. 게이트가 아무것도 증명하지 못하므로 '
            "실패로 처리한다.",
            file=sys.stderr,
        )
        return 2

    mismatches = compare_statements(python_statements, cpp_literals)
    count_differs = len(python_statements) != len(cpp_literals)

    if count_differs:
        print(
            f"문장 수 불일치: 파이썬 {len(python_statements)}건 / "
            f"C++ {len(cpp_literals)}건",
            file=sys.stderr,
        )

    if mismatches:
        for item in mismatches:
            location = f"{_display(cpp_path)}:{item.line}" if item.line else "(위치 없음)"
            print(f"[문장 {item.index}] {location}", file=sys.stderr)
            for line in item.detail:
                print(f"  {line}", file=sys.stderr)
        print(
            f"SQL 축자 이식 위반 {len(mismatches)}건 / 대조 "
            f"{max(len(python_statements), len(cpp_literals))}문장",
            file=sys.stderr,
        )
        return 1

    if count_differs:
        return 1

    print(
        f"문장 {len(python_statements)}건 전건 바이트 일치 "
        f"({_display(python_path)} <-> {_display(cpp_path)})"
    )
    return 0


def _self_test_fixture_dir() -> Path:
    return Path(__file__).resolve().parent / "fixtures" / "schema_parity" / "static"


def run_self_test() -> int:
    """fixtures 로 게이트 자신을 양방향 검증한다. 실제 소스에 의존하지 않는다."""
    fixtures = _self_test_fixture_dir()
    reference = fixtures / "reference_statements.py"
    good = fixtures / "good.cpp"
    empty = fixtures / "no_literals.cpp"
    bad_files = sorted(fixtures.glob("bad_*.cpp"))

    if not fixtures.is_dir():
        print(f"오류: fixture 디렉터리 없음 - {_display(fixtures)}", file=sys.stderr)
        return 2
    for required in (reference, good, empty):
        if not required.is_file():
            print(f"오류: fixture 없음 - {_display(required)}", file=sys.stderr)
            return 2
    if not bad_files:
        print(
            "오류: known-bad fixture 가 0건이다. 잡을 것을 잡는지 증명하지 못하므로 "
            "실패로 처리한다.",
            file=sys.stderr,
        )
        return 2

    try:
        expected = load_python_statements(reference)
    except (OSError, SyntaxError, ImportError, AttributeError, TypeError) as exc:
        print(f"오류: fixture 기준 적재 실패 - {exc}", file=sys.stderr)
        return 2
    if not expected:
        print("오류: fixture 기준 STATEMENTS 가 비었다.", file=sys.stderr)
        return 2

    failures = 0

    print(f"[방향 1] known-good 수용 검사 - 기준 문장 {len(expected)}건")
    good_literals = extract_cpp_statements(read_source_text(good))
    good_mismatches = compare_statements(expected, good_literals)
    if len(good_literals) != len(expected):
        print(
            f"  FAIL  {_display(good)}: 추출 {len(good_literals)}건 "
            f"(기대 {len(expected)}건)"
        )
        failures += 1
    elif good_mismatches:
        print(f"  FAIL  {_display(good)}: 오탐 {len(good_mismatches)}건")
        for item in good_mismatches:
            for line in item.detail:
                print(f"        {line}")
        failures += 1
    else:
        print(f"  PASS  {_display(good)}: 문장 {len(good_literals)}건 수용")

    print("[방향 2] known-bad 거부 검사")
    for path in bad_files:
        literals = extract_cpp_statements(read_source_text(path))
        mismatches = compare_statements(expected, literals)
        if mismatches:
            first = mismatches[0]
            summary = first.detail[0].splitlines()[0]
            print(
                f"  PASS  {_display(path)}: 거부 "
                f"(불일치 {len(mismatches)}건, 문장 {first.index}: {summary})"
            )
        else:
            print(f"  FAIL  {_display(path)}: 미탐 - 심어둔 결함을 잡지 못했다")
            failures += 1

    print("[방향 3] CRLF 정규화 - 줄끝만으로 거짓 실패하지 않는가")
    with tempfile.TemporaryDirectory(prefix="sqlparity_selftest_") as work:
        crlf_path = Path(work) / "good_crlf.cpp"
        crlf_path.write_bytes(
            read_source_text(good).replace("\n", "\r\n").encode("utf-8-sig")
        )
        crlf_literals = extract_cpp_statements(read_source_text(crlf_path))
        crlf_mismatches = compare_statements(expected, crlf_literals)
        if len(crlf_literals) == len(expected) and not crlf_mismatches:
            print(f"  PASS  CRLF + BOM 사본 수용(문장 {len(crlf_literals)}건)")
        else:
            print(
                f"  FAIL  CRLF + BOM 사본에서 불일치 {len(crlf_mismatches)}건 "
                f"(추출 {len(crlf_literals)}건)"
            )
            failures += 1

    print("[방향 4] 추출 0건 - 통과가 아니라 환경 오류인가")
    empty_literals = extract_cpp_statements(read_source_text(empty))
    if empty_literals:
        print(
            f"  FAIL  {_display(empty)}: 리터럴이 없어야 하는데 "
            f"{len(empty_literals)}건 추출됐다"
        )
        failures += 1
    else:
        print("        (이어지는 stderr 한 줄은 이 검사가 기대하는 출력이다)")
        sys.stdout.flush()
        code = run_check(reference, empty)
        if code == 2:
            print(f"  PASS  {_display(empty)}: 종료 코드 2(환경 오류)")
        else:
            print(f"  FAIL  {_display(empty)}: 종료 코드 {code}(기대 2)")
            failures += 1

    if failures:
        print(f"자기시험 실패 {failures}건", file=sys.stderr)
        return 1

    print(
        f"자기시험 PASS: good 1건 수용, bad {len(bad_files)}건 전건 거부, "
        "CRLF 수용, 0건 추출은 환경 오류"
    )
    return 0


def _force_utf8_output() -> None:
    """콘솔 로케일(Windows 기본 cp949)과 무관하게 UTF-8 로 출력한다."""
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="backslashreplace")


def main(argv: Sequence[str] | None = None) -> int:
    _force_utf8_output()
    parser = argparse.ArgumentParser(
        description="v0001 마이그레이션 SQL 문장의 파이썬 <-> C++ 축자 대조",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--python",
        dest="python_source",
        default=str(DEFAULT_PYTHON_SOURCE),
        metavar="PATH",
        help="파이썬 원본 마이그레이션 모듈(기본: src/pynote/.../v0001_initial.py)",
    )
    parser.add_argument(
        "--cpp",
        dest="cpp_source",
        default=str(DEFAULT_CPP_SOURCE),
        metavar="PATH",
        help="C++ 이식본(기본: NoteEx/core/src/storage/migrations/v0001_initial.cpp)",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="fixtures 기반 자기검증을 수행한다(--python/--cpp 는 무시)",
    )
    args = parser.parse_args(argv)

    if args.self_test:
        return run_self_test()
    return run_check(Path(args.python_source), Path(args.cpp_source))


if __name__ == "__main__":
    sys.exit(main())
