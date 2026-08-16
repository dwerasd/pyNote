#!/usr/bin/env python3
"""저장소 계층 SQL 문장 축자(逐字) 이식 게이트.

파이썬 원본 `repositories.py` 가 `execute(...)` 로 넘기는 SQL 문장과 그 C++ 이식본의
`u8R"SQL( ... )SQL"` 리터럴을 **바이트 단위**로 대조한다. 마이그레이션 쪽의
`check_migration_sql_parity.py` 와 같은 계약을 저장소 계층에 건 것이다 - 한쪽만
지키면 다음에 SQL 을 손댄 사람이 어느 쪽이 보호받는지 알 수 없다.

## 마이그레이션 게이트와 무엇이 다른가

**순서를 대조하지 않는다.** 마이그레이션은 문장 발행 순서 자체가 계약이라
(`create_cards` 의 강제 순서처럼) 순서까지 봐야 하지만, 저장소의 메서드 배치 순서는
계약이 아니다. 그래서 여기서는 **다중집합**(문장별 개수)을 대조한다. 같은 문장이 두
번 나오면 양쪽 모두 두 번이어야 한다.

**추출 방식도 다르다.** 마이그레이션 게이트는 `migrate()` 를 실제로 실행해 발행을
기록하지만, 저장소 메서드는 살아 있는 데이터베이스와 인자가 있어야 부를 수 있다.
그래서 이쪽은 파이썬 AST 에서 `*.execute(<문자열 리터럴>)` 의 첫 인자를 뽑는다.
리터럴이 아닌 방식으로 조립된 문장은 파이썬 쪽에서 잡히지 않으므로, 그런 문장이
C++ 에 있으면 "파이썬 쪽에 없는 문장"으로 **크게 실패한다** - 조용히 넘어가지 않는다.

## 규칙

정규화는 둘뿐이다. CRLF -> LF(저장소가 `* text=auto` 로 LF 를 보관한다),
UTF-8 BOM 제거(C++ 소스는 BOM 포함이 프로젝트 표준이다). 그 외에는 아무것도
정규화하지 않는다 - 들여쓰기·말미 공백·빈 줄이 보호 대상 그 자체다.

리터럴 접두 `u8` 는 필수다. 이 기계에서 좁은 리터럴은 CP949 로 컴파일되므로 본문이
바이트까지 같아도 SQLite 에 넘어가는 바이트가 UTF-8 이 아니게 된다. 접두가 빠진
리터럴은 그 자체로 실패다.

종료 코드:
  0  문장 집합·개수 전건 일치
  1  불일치 검출(자기시험이면 기대 불일치)
  2  사용법·환경 오류(경로 없음, 읽기 실패, 추출 대상 0건)
"""

from __future__ import annotations

import argparse
import ast
import re
import sys
from collections import Counter
from pathlib import Path
from typing import Sequence

REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_PY = REPO_ROOT / "src" / "pynote" / "infrastructure" / "repositories.py"
DEFAULT_CPP = REPO_ROOT / "NoteEx" / "core" / "src" / "storage" / "repositories.cpp"

LITERAL_RE = re.compile(r'(u8)?R"SQL\((.*?)\)SQL"', re.DOTALL)


def normalise(text: str) -> str:
    return text.replace("\r\n", "\n").replace("\r", "\n")


def python_statements(source: str) -> list[str]:
    """`*.execute(<문자열 리터럴>)` 의 첫 인자를 소스 순서와 무관하게 모은다."""
    tree = ast.parse(source)
    found: list[str] = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        func = node.func
        if not isinstance(func, ast.Attribute) or func.attr != "execute":
            continue
        if not node.args:
            continue
        first = node.args[0]
        if isinstance(first, ast.Constant) and isinstance(first.value, str):
            found.append(normalise(first.value))
    return found


def cpp_literals(source: str) -> tuple[list[str], list[str]]:
    """(u8 리터럴 본문, u8 접두가 빠진 리터럴 본문)"""
    good: list[str] = []
    narrow: list[str] = []
    for prefix, body in LITERAL_RE.findall(source):
        (good if prefix == "u8" else narrow).append(normalise(body))
    return good, narrow


def compare(py_text: str, cpp_text: str) -> list[str]:
    problems: list[str] = []
    py_statements = python_statements(py_text)
    good, narrow = cpp_literals(cpp_text)

    if narrow:
        problems.append(f"u8 접두가 없는 SQL 리터럴 {len(narrow)} 건")

    py_counter = Counter(py_statements)
    cpp_counter = Counter(good)

    if len(py_statements) != len(good):
        problems.append(f"문장 수 불일치: 파이썬 {len(py_statements)} vs C++ {len(good)}")

    for statement, count in sorted(py_counter.items()):
        if cpp_counter.get(statement, 0) != count:
            problems.append(f"C++ 쪽에 없거나 개수가 다른 문장: {statement.strip()[:60]!r}")
    for statement, count in sorted(cpp_counter.items()):
        if py_counter.get(statement, 0) != count:
            problems.append(f"파이썬 쪽에 없거나 개수가 다른 문장: {statement.strip()[:60]!r}")
    return problems


def run_self_test() -> int:
    """통과해야 할 것이 통과하고 잡아야 할 것이 잡히는지 본다."""
    py_fixture = (
        "class R:\n"
        "    def a(self):\n"
        "        self._connection.execute(\n"
        '            """\n'
        "            SELECT 1\n"
        "            FROM t\n"
        '            """\n'
        "        )\n"
        "    def b(self):\n"
        '        self._connection.execute("DELETE FROM t WHERE id = ?", (1,))\n'
    )
    good_cpp = (
        'C_STATEMENT A(h, u8R"SQL(\n            SELECT 1\n            FROM t\n            )SQL");\n'
        'C_STATEMENT B(h, u8R"SQL(DELETE FROM t WHERE id = ?)SQL");\n'
    )
    bad_missing = 'C_STATEMENT A(h, u8R"SQL(\n            SELECT 1\n            FROM t\n            )SQL");\n'
    bad_indent = (
        'C_STATEMENT A(h, u8R"SQL(\n        SELECT 1\n        FROM t\n        )SQL");\n'
        'C_STATEMENT B(h, u8R"SQL(DELETE FROM t WHERE id = ?)SQL");\n'
    )
    bad_narrow = (
        'C_STATEMENT A(h, u8R"SQL(\n            SELECT 1\n            FROM t\n            )SQL");\n'
        'C_STATEMENT B(h, R"SQL(DELETE FROM t WHERE id = ?)SQL");\n'
    )
    bad_extra = good_cpp + 'C_STATEMENT C(h, u8R"SQL(SELECT 2)SQL");\n'

    failures = 0
    accepted = compare(py_fixture, good_cpp)
    if accepted:
        print(f"  FAIL  정상 표본이 거절됐다: {accepted}")
        failures += 1
    else:
        print("  PASS  정상 표본 수용")

    for name, fixture in (
        ("리터럴 누락", bad_missing),
        ("들여쓰기 변형", bad_indent),
        ("u8 접두 누락", bad_narrow),
        ("잉여 리터럴", bad_extra),
    ):
        if compare(py_fixture, fixture):
            print(f"  PASS  결함 표본 거부: {name}")
        else:
            print(f"  FAIL  결함 표본({name})을 못 잡았다")
            failures += 1

    # 실제 원본에서 문장이 실제로 뽑히는지도 본다. 0건 추출이 "전건 일치" 로
    # 보고되는 경로를 닫는다 - 아무것도 비교하지 않은 게이트는 통과가 아니다.
    if DEFAULT_PY.is_file():
        real = len(python_statements(DEFAULT_PY.read_text(encoding="utf-8")))
        if real > 0:
            print(f"  PASS  실제 원본에서 문장 {real} 건 추출")
        else:
            print("  FAIL  실제 원본에서 문장을 하나도 못 뽑았다")
            failures += 1

    print(
        "자기시험 PASS: 정상 1건 수용, 결함 4종 전건 거부, 실제 원본 추출 확인"
        if failures == 0
        else f"자기시험 실패 {failures} 건"
    )
    return 1 if failures else 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--self-test", action="store_true", help="게이트 자신을 양방향 검증한다")
    parser.add_argument("--python", type=Path, default=DEFAULT_PY, help="파이썬 원본 경로")
    parser.add_argument("--cpp", type=Path, default=DEFAULT_CPP, help="C++ 이식본 경로")
    args = parser.parse_args(argv)

    if args.self_test:
        return run_self_test()

    for path in (args.python, args.cpp):
        if not path.is_file():
            print(f"오류: 경로가 없다 - {path}", file=sys.stderr)
            return 2

    py_text = args.python.read_text(encoding="utf-8")
    cpp_text = args.cpp.read_text(encoding="utf-8-sig")

    statements = python_statements(py_text)
    if not statements:
        print(f"오류: 추출 대상 0건 - {args.python} 에서 SQL 문장을 뽑지 못했다.", file=sys.stderr)
        return 2

    problems = compare(py_text, cpp_text)
    if problems:
        for line in problems:
            print("불일치:", line, file=sys.stderr)
        return 1

    print(f"통과: 저장소 SQL {len(statements)} 문장 전건 바이트 일치")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
