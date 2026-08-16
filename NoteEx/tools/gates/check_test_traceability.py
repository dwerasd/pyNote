#!/usr/bin/env python3
"""시험 추적성 게이트 - 이식 시험이 어느 파이썬 원본에서 왔는지 기계로 확인한다.

W1 지시서는 새로 쓰는 시험마다 대응하는 파이썬 원본을 주석으로 적으라고 요구한다.
W0 T4 가 시험 이식 대장을 만들 때 그 주석의 pytest node ID 를 역보강하기 때문이다.
지금까지 그 요구는 **관례**로만 지켜졌다 - 이식 워커들이 실제로 적었을 뿐, 아무도
검사하지 않았다. 검사하지 않는 관례는 조용히 썩는다. 주석 없이 추가된 시험은 다음
달에 누가 파일을 열어 읽기 전까지 주석 있는 시험과 똑같아 보인다.

그보다 나쁜 것이 **없는 node ID 를 적은 주석**이다. 역보강 시점에는 그 문자열을
믿고 대장에 옮겨 적을 텐데, 가리키는 함수가 없으면 대장 한 줄이 통째로 거짓이
된다. 표본 네 건을 손으로 확인해 전부 실재함을 봤지만 네 건은 전부가 아니다.

## 무엇을 증명하나

1. **주석 존재**: 검사 대상 디렉터리의 모든 `TEST_CASE` 바로 위에 `//` 주석 블록이
   붙어 있고, 그 블록에 `대응 원본` 표기가 있다. 빈 줄로 끊긴 주석은 그 시험의
   것이 아니므로 없는 것으로 본다.
2. **claim 해소**: 블록이 pytest node ID 를 주장하면 그 node ID 가 **실재한다**.
   파일이 저장소에 있고, 그 파일 안에 그 이름의 시험 함수가 **정의**되어 있어야
   한다. 판정은 `ast` 로 한다 - 호출부나 주석에 이름이 스쳐 지나가는 것은 정의가
   아니다.
3. **부재의 명시**: 대응하는 파이썬 시험이 없다는 선언(`대응 원본 없음`)과 node ID
   역보강 대기 선언(`... node ID 는 W0 T4 역보강 대기다`)은 **유효한 형태로 받는다.**
   그것은 구멍이 아니라 정직한 신고다. 다만 명시적이어야 한다 - 침묵은 부재 선언이
   아니다. 아무 말도 없는 블록은 위반이다.

## 받아들이는 주석 형태 (트리에 이미 있는 것을 추인한다)

주석 블록 안 **어디에** node ID 가 있는지는 강제하지 않는다. 이식 워커들이 다섯
가지 배치를 썼고, 그중 하나만 정본으로 고르면 나머지 네 가지를 전부 고쳐야 한다.
게이트의 일은 있는 관례를 기계로 굳히는 것이지 새 관례를 강요하는 것이 아니다.

    // 대응 원본: tests/integration/test_backup.py::test_x        (한 줄 직접)
    // 대응 원본: <설명>
    // pytest node ID: tests/integration/test_database.py::test_x (별도 줄)
    // 대응 원본: tests/integration/test_database.py::test_a
    //            tests/integration/test_database.py::test_b      (들여쓴 목록)
    // 대응 원본: <설명> (tests/integration/test_repositories.py::test_x 의 ...)
    //                                                            (산문 괄호 안)
    // 대응 원본: tests/integration/test_repositories.py::test_a
    // 와 ::test_b (...)                                          (앞 경로를 잇는 표기)

    // 대응 원본: <설명>. 파이썬 시험 트리에 대응 케이스가 없어
    // pytest node ID 는 W0 T4 역보강 대기다.                     (역보강 대기)
    // 대응 원본: 없음. ... pytest node ID 는 존재하지 않는다.     (부재)
    // 대응 원본 없음. ...                                        (부재, 콜론 없음)

**강제하는 것은 두 가지뿐이다.** node ID 는 `tests/` 로 시작하는 저장소 상대 경로에
붙어야 하고(그래야 저장소 루트에서 pytest 로 그대로 돌릴 수 있다), 경로를 생략한
`::이름` 표기는 **같은 블록의 앞줄**에 경로가 나와 있어야 한다. 앞이 비어 있으면
그 표기는 어느 파일의 시험인지 아무도 모르므로 위반이다.

## 무엇을 증명하지 않나 (여기 적힌 것은 이 게이트가 못 보는 것이다)

* **시험이 같은 동작을 보는지.** 이것이 가장 큰 구멍이다. 이 게이트는 주석이 가리키는
  node ID 가 실재하는지만 본다. C++ 시험이 그 파이썬 시험과 **같은 계약을 검사하는지**
  는 전혀 보지 않는다 - 엉뚱한 시험을 가리켜도 그 이름이 실재하기만 하면 통과다.
  이 게이트의 초록은 "주석이 거짓말을 하지 않는다"까지이지 "이식이 옳다"가 아니다.
* **역보강 대기 선언의 진위.** "파이썬 시험 트리에 대응 케이스가 없다"는 주장을
  확인하지 않는다. 정말 없는지, 찾기 귀찮았는지는 구별되지 않는다. 대기 선언은
  전수 조사가 아니라 신고이고, 그 신고를 대조하는 것이 W0 T4 의 일이다.
* **설명 부분의 행 번호.** `(backup.py :109~137)` 같은 표기가 실제 그 줄을 가리키는지
  보지 않는다. 파이썬 원본이 바뀌면 이 숫자는 조용히 낡는다.
* **주석이 붙은 시험이 맞는지.** 바로 위에 붙은 블록을 그 `TEST_CASE` 의 것으로 본다.
  시험을 재배열하면서 주석을 두고 가면 잘못 붙은 채로 통과한다.
* **`/* */` 블록 주석.** 추적 주석으로 인식하지 않는다. 트리 전체가 `//` 를 쓴다.
* **harness 디렉터리.** `NoteEx/tests/harness/` 는 검사하지 않는다. 그쪽은 이식본이
  아니라 하네스 자체의 연기 시험이라 대응할 파이썬 원본이 없다.

## 종료 코드

  0  검사 대상 전건 통과
  1  위반 검출(자기시험이면 기대 위반). 파일이 반쯤 쓰인 채로 읽혀 주석·문자열이
     닫히지 않은 경우도 여기로 보고한다 - 그 순간 본 것을 그대로 적는다.
  2  사용법·환경 오류(경로 없음, 읽기 실패, 파이썬 원본 파싱 실패, **검사 대상 0건**)

검사 대상 0건을 오류로 두는 것은 다른 게이트와 같은 선택이다. `TEST_CASE` 를 하나도
못 찾은 게이트는 아무것도 증명하지 못하는데, 원인은 대개 경로 오타나 디렉터리 개편이다.
"""

from __future__ import annotations

import argparse
import ast
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Sequence

REPO_ROOT = Path(__file__).resolve().parents[3]
FIXTURE_DIR = Path(__file__).resolve().parent / "fixtures" / "test_traceability"

DEFAULT_ROOT = REPO_ROOT / "NoteEx" / "tests" / "unit"

# check_core_isolation.py 와 같은 확장자 집합을 쓴다. 지금 대상은 .cpp 뿐이지만
# 확장자마다 다른 게이트가 다른 집합을 보는 것이 더 나쁜 함정이다.
SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ixx")

# 추적 주석의 표지. 이 문자열이 없는 블록은 추적 주석이 아니다.
ORIGIN_MARKER = re.compile(r"대응\s*원본")
# 부재 선언. `대응 원본: 없음` 과 `대응 원본 없음` 둘 다 트리에 있다.
ABSENT_MARKER = re.compile(r"대응\s*원본\s*:?\s*없음")
# node ID 를 말하는 자리. 뒤의 두 표지는 이 표지와 함께 있을 때만 뜻이 산다.
NODE_WORD = re.compile(r"node\s*ID", re.IGNORECASE)
PENDING_WORD = re.compile(r"역보강")
NONEXISTENT_WORD = re.compile(r"존재하지\s*않는다")

# 경로가 붙은 node ID. `tests/` 로 시작하는 저장소 상대 경로만 claim 으로 본다.
# `v0001_initial.py::migrate` 처럼 원본 모듈의 함수를 가리키는 산문은 claim 이 아니다.
NODE_FULL = re.compile(r"(?<![\w./-])(tests/[\w./-]*\.py)((?:::[A-Za-z_]\w*)+)(\[[^\]\s]*\])?")
# 경로를 생략하고 앞줄 경로를 잇는 표기. `E_MIGRATE_RESULT::VersionReadFailed` 같은
# C++ 범위 연산자는 `::` 앞에 식별자 문자가 있으므로 여기 걸리지 않는다.
NODE_BARE = re.compile(r"(?<![\w.\])/])::([A-Za-z_]\w*)(\[[^\]\s]*\])?")

TEST_MACRO = re.compile(r"\bTEST_CASE\s*\(")
RAW_PREFIX = re.compile(r"(?:u8|u|U|L)?R$")


class GateEnvironmentError(RuntimeError):
    """판정을 시작할 수 없는 상태. 위반이 아니라 종료 코드 2 다."""


def normalise(text: str) -> str:
    return text.replace("\r\n", "\n").replace("\r", "\n")


# ---------------------------------------------------------------------------------------
# C++ 소스 훑기
# ---------------------------------------------------------------------------------------
@dataclass
class Scan:
    """한 파일에서 뽑은 것. 줄 번호는 1 부터다."""

    test_lines: list[int] = field(default_factory=list)
    comment_lines: dict[int, str] = field(default_factory=dict)
    unterminated: str | None = None


def scan_source(source: str) -> Scan:
    """`TEST_CASE` 자리와 온전한 `//` 주석 줄을 뽑는다.

    주석·문자열·원시 문자열 안의 `TEST_CASE` 는 세지 않는다. 시험 이름이 한국어
    문자열이고 SQL 원시 리터럴이 섞여 있으므로, 정규식만으로는 문자열 안의 따옴표와
    `//` 에 걸려 뒤쪽 줄을 통째로 주석으로 오인한다.

    주석 줄로 기록하는 것은 **줄 전체가 주석인 줄**뿐이다. 코드 뒤에 붙은 꼬리 주석은
    추적 주석 블록의 일부가 아니다.
    """
    scan = Scan()
    size = len(source)
    index = 0
    line = 1
    line_start = 0

    while index < size:
        char = source[index]

        if char == "\n":
            line += 1
            index += 1
            line_start = index
            continue

        if char == "/" and index + 1 < size and source[index + 1] == "/":
            head = source[line_start:index]
            end = source.find("\n", index)
            if end == -1:
                end = size
            if head.strip() == "":
                scan.comment_lines[line] = source[index + 2 : end]
            index = end
            continue

        if char == "/" and index + 1 < size and source[index + 1] == "*":
            end = source.find("*/", index + 2)
            if end == -1:
                scan.unterminated = f"{line} 줄에서 시작한 블록 주석이 닫히지 않았다"
                break
            line += source.count("\n", index, end)
            line_start = source.rfind("\n", index, end) + 1 or line_start
            index = end + 2
            continue

        if char == '"':
            prefix = RAW_PREFIX.search(source[max(0, index - 3) : index])
            raw = prefix is not None and (
                index - len(prefix.group(0)) == 0
                or not source[index - len(prefix.group(0)) - 1].isalnum()
            )
            if raw:
                opener = source.find("(", index)
                if opener == -1:
                    scan.unterminated = f"{line} 줄의 원시 문자열 구분자를 읽지 못했다"
                    break
                closer = f"){source[index + 1 : opener]}\""
                end = source.find(closer, opener)
                if end == -1:
                    scan.unterminated = f"{line} 줄에서 시작한 원시 문자열이 닫히지 않았다"
                    break
                line += source.count("\n", index, end)
                index = end + len(closer)
                line_start = index
                continue
            end = _skip_quoted(source, index, '"')
            if end is None:
                scan.unterminated = f"{line} 줄에서 시작한 문자열이 닫히지 않았다"
                break
            line += source.count("\n", index, end)
            index = end
            continue

        if char == "'":
            end = _skip_quoted(source, index, "'")
            if end is None:
                scan.unterminated = f"{line} 줄에서 시작한 문자 리터럴이 닫히지 않았다"
                break
            line += source.count("\n", index, end)
            index = end
            continue

        if char == "T":
            # `\b` 는 pos 앞 글자를 본다 - `MY_TEST_CASE(` 는 걸리지 않는다.
            matched = TEST_MACRO.match(source, index)
            if matched is not None:
                scan.test_lines.append(line)
                index = matched.end()
                continue

        index += 1

    return scan


def _skip_quoted(source: str, start: int, quote: str) -> int | None:
    """여는 따옴표 위치에서 닫는 따옴표 **다음** 위치를 돌려준다. 못 닫으면 None."""
    index = start + 1
    size = len(source)
    while index < size:
        char = source[index]
        if char == "\\":
            index += 2
            continue
        if char == quote:
            return index + 1
        index += 1
    return None


def comment_block(scan: Scan, test_line: int) -> list[tuple[int, str]]:
    """`TEST_CASE` 바로 위에 빈 줄 없이 붙은 주석 줄을 위에서 아래 순서로 돌려준다."""
    block: list[tuple[int, str]] = []
    cursor = test_line - 1
    while cursor >= 1 and cursor in scan.comment_lines:
        block.append((cursor, scan.comment_lines[cursor]))
        cursor -= 1
    block.reverse()
    return block


# ---------------------------------------------------------------------------------------
# 주석 블록 해석
# ---------------------------------------------------------------------------------------
@dataclass
class Claim:
    path: str
    parts: tuple[str, ...]
    line: int

    def text(self) -> str:
        return self.path + "::" + "::".join(self.parts)


@dataclass
class Block:
    claims: list[Claim] = field(default_factory=list)
    unanchored: list[tuple[int, str]] = field(default_factory=list)
    has_origin: bool = False
    declares_absent: bool = False
    declares_pending: bool = False


def read_block(block: Sequence[tuple[int, str]]) -> Block:
    """주석 블록에서 claim 과 선언을 뽑는다."""
    parsed = Block()
    joined = " ".join(text for _line, text in block)
    parsed.has_origin = ORIGIN_MARKER.search(joined) is not None
    parsed.declares_absent = ABSENT_MARKER.search(joined) is not None or (
        NODE_WORD.search(joined) is not None and NONEXISTENT_WORD.search(joined) is not None
    )
    parsed.declares_pending = (
        NODE_WORD.search(joined) is not None and PENDING_WORD.search(joined) is not None
    )

    last_path: str | None = None
    for line, text in block:
        for matched in NODE_FULL.finditer(text):
            last_path = matched.group(1)
            parts = tuple(part for part in matched.group(2).split("::") if part)
            parsed.claims.append(Claim(path=last_path, parts=parts, line=line))
        for matched in NODE_BARE.finditer(text):
            if last_path is None:
                parsed.unanchored.append((line, matched.group(1)))
                continue
            parsed.claims.append(Claim(path=last_path, parts=(matched.group(1),), line=line))
    return parsed


# ---------------------------------------------------------------------------------------
# node ID 해소
# ---------------------------------------------------------------------------------------
@dataclass
class Definitions:
    functions: set[str]
    methods: set[tuple[str, str]]


def python_definitions(path: Path) -> Definitions:
    """모듈 최상위 함수와 클래스 메서드의 이름을 모은다."""
    try:
        tree = ast.parse(path.read_text(encoding="utf-8"))
    except OSError as error:
        raise GateEnvironmentError(f"파이썬 원본을 읽지 못했다 - {error}") from error
    except SyntaxError as error:
        raise GateEnvironmentError(f"파이썬 원본을 파싱하지 못했다 - {error}") from error

    functions: set[str] = set()
    methods: set[tuple[str, str]] = set()
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            functions.add(node.name)
        elif isinstance(node, ast.ClassDef):
            for member in node.body:
                if isinstance(member, (ast.FunctionDef, ast.AsyncFunctionDef)):
                    methods.add((node.name, member.name))
    return Definitions(functions=functions, methods=methods)


class Resolver:
    """node ID 를 해소한다. 같은 파일을 여러 시험이 가리키므로 파싱 결과를 남긴다."""

    def __init__(self, root: Path) -> None:
        self._root = root
        self._cache: dict[str, Definitions] = {}

    def problem(self, claim: Claim) -> str | None:
        """해소되면 None, 아니면 사유."""
        if len(claim.parts) > 2:
            return f"node ID 마디가 너무 많다: {claim.text()}"
        target = self._root / claim.path
        if not target.is_file():
            return f"가리키는 파일이 없다: {claim.text()}"
        definitions = self._cache.get(claim.path)
        if definitions is None:
            definitions = python_definitions(target)
            self._cache[claim.path] = definitions
        if len(claim.parts) == 1:
            if claim.parts[0] in definitions.functions:
                return None
            return f"그 이름의 시험 함수가 파일에 정의되어 있지 않다: {claim.text()}"
        if (claim.parts[0], claim.parts[1]) in definitions.methods:
            return None
        return f"그 클래스의 메서드가 파일에 정의되어 있지 않다: {claim.text()}"


# ---------------------------------------------------------------------------------------
# 검사
# ---------------------------------------------------------------------------------------
def display_path(path: Path) -> str:
    try:
        return path.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return str(path)


def check_source(source: str, label: str, resolver: Resolver) -> tuple[list[str], int, int]:
    """(위반 목록, 검사한 `TEST_CASE` 수, 해소한 node ID 수).

    node ID 수를 따로 세는 것은 "주장한 것은 전부 실재한다" 가 주장이 0 건이어도
    참이기 때문이다. 전건이 역보강 대기 선언이면 이 게이트는 실재성을 하나도
    증명하지 않은 것이고, 그 사실이 보고에 드러나야 한다.
    """
    problems: list[str] = []
    resolved = 0
    scan = scan_source(normalise(source))
    if scan.unterminated is not None:
        problems.append(
            f"{label}:{scan.unterminated} - 읽는 도중의 파일일 수 있다. 본 그대로 보고한다"
        )

    for test_line in scan.test_lines:
        block = comment_block(scan, test_line)
        if not block:
            problems.append(f"{label}:{test_line} TEST_CASE 바로 위에 추적 주석이 없다")
            continue
        parsed = read_block(block)
        if not parsed.has_origin:
            problems.append(
                f"{label}:{test_line} 주석 블록에 `대응 원본` 표기가 없다"
                f"(블록 {block[0][0]}~{block[-1][0]} 줄)"
            )
            continue
        for line, name in parsed.unanchored:
            problems.append(
                f"{label}:{line} 앞줄에 파일 경로가 없는 `::{name}` 표기 - "
                f"어느 파일의 시험인지 알 수 없다"
            )
        for claim in parsed.claims:
            reason = resolver.problem(claim)
            if reason is None:
                resolved += 1
            else:
                problems.append(f"{label}:{claim.line} {reason}")
        if not parsed.claims and not parsed.declares_pending and not parsed.declares_absent:
            problems.append(
                f"{label}:{test_line} `대응 원본` 은 있으나 node ID 도, 역보강 대기 선언도, "
                f"부재 선언도 없다(블록 {block[0][0]}~{block[-1][0]} 줄)"
            )
    return problems, len(scan.test_lines), resolved


def source_files(root: Path) -> list[Path]:
    return sorted(
        path
        for path in root.rglob("*")
        if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES
    )


def run_root(root: Path, resolver: Resolver) -> tuple[list[str], int, int, int]:
    """(위반 목록, 검사한 `TEST_CASE` 수, 해소한 node ID 수, 읽은 파일 수)."""
    problems: list[str] = []
    total = 0
    resolved = 0
    files = source_files(root)
    for path in files:
        try:
            source = path.read_text(encoding="utf-8-sig")
        except OSError as error:
            raise GateEnvironmentError(f"읽지 못했다 - {error}") from error
        found, count, hits = check_source(source, display_path(path), resolver)
        problems.extend(found)
        total += count
        resolved += hits
    return problems, total, resolved, len(files)


# ---------------------------------------------------------------------------------------
# 자기시험
# ---------------------------------------------------------------------------------------
def run_self_test() -> int:
    """통과해야 할 것이 통과하고 잡아야 할 것이 잡히는지 본다."""
    required = [
        "good.cpp",
        "bad_no_comment.cpp",
        "bad_missing_function.cpp",
        "bad_missing_file.cpp",
        "bad_no_claim.cpp",
        "bad_unanchored_ref.cpp",
        "tests/integration/test_sample.py",
    ]
    absent = [name for name in required if not (FIXTURE_DIR / name).is_file()]
    if absent:
        print(f"  FAIL  fixture 가 없다: {absent}", file=sys.stderr)
        return 2

    resolver = Resolver(FIXTURE_DIR)
    failures = 0

    # 정상 표본. 트리에 실재하는 일곱 가지 주석 배치를 한 파일에 모아 둔 것이다.
    good = (FIXTURE_DIR / "good.cpp").read_text(encoding="utf-8-sig")
    accepted, counted, resolved = check_source(good, "good.cpp", resolver)
    if accepted:
        print(f"  FAIL  정상 표본이 거절됐다: {accepted}")
        failures += 1
    elif counted != 7 or resolved != 7:
        # 개수까지 본다. 주석·문자열 안의 `TEST_CASE` 를 세거나 진짜를 빠뜨리는
        # 훑기 결함도, node ID 를 통째로 못 뽑는 결함도 "위반 0건" 으로는 드러나지 않는다.
        print(
            f"  FAIL  정상 표본에서 TEST_CASE 7 / node ID 7 이 아니라 "
            f"{counted} / {resolved} 를 셌다"
        )
        failures += 1
    else:
        print(f"  PASS  정상 표본 수용(TEST_CASE {counted} 건, node ID {resolved} 건 해소)")

    # 기대 사유까지 맞춘다. "무엇이든 걸렸다" 로는 엉뚱한 이유로 통과한 자기시험을
    # 가려낼 수 없다 - 결함 표본은 심은 결함 때문에 거부돼야 한다.
    cases = [
        ("주석 없는 TEST_CASE", "bad_no_comment.cpp", "추적 주석이 없다"),
        ("파일은 있고 함수가 없는 node ID", "bad_missing_function.cpp", "정의되어 있지 않다"),
        ("파일 자체가 없는 node ID", "bad_missing_file.cpp", "가리키는 파일이 없다"),
        ("아무것도 주장하지 않는 주석", "bad_no_claim.cpp", "부재 선언도 없다"),
        ("경로 없는 `::이름` 표기", "bad_unanchored_ref.cpp", "어느 파일의 시험인지"),
    ]
    for name, fixture, expected in cases:
        source = (FIXTURE_DIR / fixture).read_text(encoding="utf-8-sig")
        found, _count, _resolved = check_source(source, fixture, resolver)
        hit = [line for line in found if expected in line]
        if hit:
            print(f"  PASS  결함 표본 거부: {name} ({hit[0]})")
        else:
            print(f"  FAIL  결함 표본({name})을 기대 사유로 못 잡았다: {found}")
            failures += 1

    # 실제 트리에서 대상이 실제로 뽑히는지도 본다. 0 건 추출이 "전건 통과" 로
    # 보고되는 경로를 닫는다 - 아무것도 검사하지 않은 게이트는 통과가 아니다.
    if DEFAULT_ROOT.is_dir():
        real = 0
        for path in source_files(DEFAULT_ROOT):
            real += len(scan_source(normalise(path.read_text(encoding="utf-8-sig"))).test_lines)
        if real > 0:
            print(f"  PASS  실제 시험 트리에서 TEST_CASE {real} 건 추출")
        else:
            print("  FAIL  실제 시험 트리에서 TEST_CASE 를 하나도 못 뽑았다")
            failures += 1

    print(
        "자기시험 PASS: 정상 1건 수용, 결함 5종 전건 거부, 실제 트리 추출 확인"
        if failures == 0
        else f"자기시험 실패 {failures} 건"
    )
    return 1 if failures else 0


# ---------------------------------------------------------------------------------------
# 실행
# ---------------------------------------------------------------------------------------
def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--self-test", action="store_true", help="게이트 자신을 양방향 검증한다")
    parser.add_argument(
        "--root",
        type=Path,
        default=DEFAULT_ROOT,
        help="검사할 시험 디렉터리(기본: NoteEx/tests/unit)",
    )
    parser.add_argument(
        "--resolve-root",
        type=Path,
        default=REPO_ROOT,
        help="node ID 를 해소할 기준 디렉터리(기본: 저장소 루트)",
    )
    args = parser.parse_args(argv)

    if args.self_test:
        return run_self_test()

    if not args.root.is_dir():
        print(f"오류: 디렉터리가 없다 - {args.root}", file=sys.stderr)
        return 2
    if not args.resolve_root.is_dir():
        print(f"오류: 디렉터리가 없다 - {args.resolve_root}", file=sys.stderr)
        return 2

    try:
        problems, total, resolved, files = run_root(args.root, Resolver(args.resolve_root))
    except GateEnvironmentError as error:
        print(f"오류: {error}", file=sys.stderr)
        return 2

    # 위반을 먼저 게워 낸다. 대상 0건이어도 위반이 있으면 그것이 보고할 내용이지,
    # 환경 오류로 접어 숨길 것이 아니다.
    if problems:
        for line in problems:
            print("위반:", line, file=sys.stderr)
        print(
            f"위반 {len(problems)} 건 / TEST_CASE {total} 건 검사, node ID {resolved} 건 해소",
            file=sys.stderr,
        )
        return 1

    if total == 0:
        print(
            f"오류: 검사 대상 0건 - {display_path(args.root)} 의 파일 {files} 개에서 "
            "TEST_CASE 를 하나도 찾지 못했다.",
            file=sys.stderr,
        )
        return 2

    print(
        f"통과: TEST_CASE {total} 건 전건이 추적 주석을 달고 있고, "
        f"주장한 node ID {resolved} 건은 전부 실재한다"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
