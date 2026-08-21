#!/usr/bin/env python3
"""core/ 계층 헤더 격리 게이트.

core/ 는 순수 C++20 도메인 로직이므로 Win32·ATL/WTL·COM·Direct2D/DirectWrite 등
플랫폼 헤더를 포함할 수 없다. 이 스크립트는 주석·문자열 리터럴을 걷어낸 뒤
`#include` 지시문만 골라 금지 계열 헤더를 찾는다.

종료 코드:
  0  통과
  1  위반 검출(또는 자기시험 기대 불일치)
  2  사용법·환경 오류(경로 없음, 읽기 실패, **스캔 대상 0건**)

스캔 대상 0건을 통과가 아니라 오류로 두는 것은 의도된 선택이다 — 대상이 없는
게이트는 아무것도 증명하지 못하며, 실제로는 경로 오타·디렉터리 개편이 원인인
경우가 압도적이다. 조용한 통과보다 붉은 실패가 싸다.
"""

from __future__ import annotations

import argparse
import fnmatch
import os
import re
import sys
from pathlib import Path
from typing import NamedTuple, Sequence

# 스캔 대상 확장자(C/C++ 소스·헤더·MSVC 모듈 인터페이스).
SOURCE_SUFFIXES: frozenset[str] = frozenset(
    {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ixx"}
)

# 금지 계열. 개별 헤더 나열이 아니라 접두 계열 글롭이 정본이다 — SDK 가 새 헤더를
# 추가해도 같은 계열이면 자동으로 걸린다. include 표기의 마지막 경로 요소를
# 소문자로 낮춰 대조한다.
FORBIDDEN_PATTERNS: tuple[str, ...] = (
    # Win32 기본. windows.h/windowsx.h/winuser.h/wingdi.h/winbase.h/wincodec.h 를
    # 포함해 winreg.h·winnt.h·winsock2.h 같은 미래 헤더까지 한 패턴이 덮는다.
    "win*.h",
    "shellapi.h",
    "shlobj*.h",
    "commctrl.h",
    # COM
    "objbase.h",
    "combaseapi.h",
    "unknwn.h",
    "ole*.h",
    # DirectX 계열
    "d2d*.h",
    "d3d*.h",
    "dwrite*.h",
    "dxgi*.h",
    # 입력기·텍스트 서비스. imm*.h 로 넓히면 도메인 헤더 immutable.h 를 잡으므로
    # 이 계열만 정확한 이름으로 고정한다.
    "imm.h",
    "msctf.h",
    "richedit.h",
    # ATL 과 WTL 은 헤더 접두가 같다(atlbase.h, atlapp.h, atlcrack.h, atlmisc.h ...).
    "atl*.h",
    "wtl*.h",
    # 기타 Win32 유틸
    "tchar.h",
    "strsafe.h",
)

# 허용 목록은 금지 패턴보다 **먼저** 판정한다 — 넓은 접두 계열이 정당한 헤더를
# 잡을 때의 유일한 탈출구다. SQLite 는 이식 가능한 저장 엔진이라 core 에서 쓴다.
# window_lifecycle.h 는 core 자신의 헤더인데 `win*.h` 글롭에 걸리는 이름 충돌이라
# 허용한다 — Win32 SDK 에 같은 이름의 헤더는 없다.
ALLOWED_HEADERS: frozenset[str] = frozenset(
    {"sqlite3.h", "sqlite3ext.h", "window_lifecycle.h"}
)

_FORBIDDEN_RE: tuple[re.Pattern[str], ...] = tuple(
    re.compile(fnmatch.translate(pattern), re.IGNORECASE)
    for pattern in FORBIDDEN_PATTERNS
)

# 지시문 자체(`include`)는 대소문자를 가리지만 헤더 이름은 가리지 않는다.
_INCLUDE_HEAD = re.compile(r"#[ \t]*include[ \t]*")
_INCLUDE_ARG = re.compile(r"<[^>\n]*>|\"[^\"\n]*\"")
_INCLUDE_LINE = re.compile(r"^[ \t]*#[ \t]*include[ \t]*(<[^>\n]*>|\"[^\"\n]*\")")


class Violation(NamedTuple):
    """금지 헤더 1건. spelling 은 원문 표기(꺾쇠·따옴표 포함)."""

    path: Path
    line: int
    spelling: str


def _display(path: Path) -> str:
    """경로를 슬래시로 정규화해 셸·로그 어디서나 같은 문자열이 되게 한다."""
    return str(path).replace("\\", "/")


def _blank(chunk: str) -> str:
    """개행만 남기고 나머지를 공백으로 치환한다(줄 번호·열 위치 보존)."""
    return "".join("\n" if ch == "\n" else " " for ch in chunk)


def strip_comments_and_literals(text: str) -> str:
    """주석·문자열/문자 리터럴을 공백으로 지운다. 줄 구조는 보존한다.

    `#include "foo.h"` 의 따옴표 토큰은 문자열 리터럴이 아니라 헤더 이름이므로
    지우지 않고 원문 그대로 남긴다. 블록 주석은 표준대로 공백 취급이라
    `/* c */ #include <x>` 는 여전히 지시문으로 인식된다.
    """
    out: list[str] = []
    i = 0
    n = len(text)
    at_line_start = True  # 이번 줄에서 아직 공백·주석만 지나왔는가

    while i < n:
        ch = text[i]

        if ch == "\n":
            out.append(ch)
            i += 1
            at_line_start = True
            continue

        if ch in " \t\r\f\v":
            out.append(ch)
            i += 1
            continue

        # 줄의 첫 토큰이 #include 면 헤더 이름 토큰까지 원문 보존
        if at_line_start and ch == "#":
            head = _INCLUDE_HEAD.match(text, i)
            at_line_start = False
            if head is not None:
                out.append(text[i : head.end()])
                i = head.end()
                arg = _INCLUDE_ARG.match(text, i)
                if arg is not None:
                    out.append(arg.group(0))
                    i = arg.end()
                continue
            out.append(ch)
            i += 1
            continue

        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            # 줄 주석. 백슬래시 줄이음이면 다음 줄까지 이어진다.
            out.append("  ")
            i += 2
            while i < n:
                cur = text[i]
                if cur == "\\":
                    out.append(" ")
                    i += 1
                    if i < n and text[i] == "\r":
                        out.append(" ")
                        i += 1
                    if i < n and text[i] == "\n":
                        out.append("\n")
                        i += 1
                    continue
                if cur == "\n":
                    break
                out.append(" ")
                i += 1
            at_line_start = False
            continue

        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            stop = n if end == -1 else end + 2
            chunk = text[i:stop]
            out.append(_blank(chunk))
            # 블록 주석은 공백 취급이라 at_line_start 를 내리지 않는다. 다만 주석이
            # 여러 줄을 삼켰으면 마지막 개행 이후가 새 줄이다.
            if "\n" in chunk:
                at_line_start = True
            i = stop
            continue

        if ch == '"':
            # 원시 문자열 R"delim( ... )delim" 은 개행을 담을 수 있다.
            if i > 0 and text[i - 1] == "R":
                j = i + 1
                delim: list[str] = []
                while j < n and text[j] not in "( )\\\t\n":
                    delim.append(text[j])
                    j += 1
                if j < n and text[j] == "(":
                    closer = ")" + "".join(delim) + '"'
                    end = text.find(closer, j + 1)
                    stop = n if end == -1 else end + len(closer)
                    out.append(_blank(text[i:stop]))
                    i = stop
                    at_line_start = False
                    continue
            out.append(" ")
            i += 1
            while i < n:
                cur = text[i]
                if cur == "\\":
                    out.append(" ")
                    i += 1
                    if i < n:
                        out.append("\n" if text[i] == "\n" else " ")
                        i += 1
                    continue
                if cur == '"':
                    out.append(" ")
                    i += 1
                    break
                if cur == "\n":
                    # 유효한 C++ 이 아니다. 줄 끝에서 리터럴 상태를 풀어, 미종결
                    # 따옴표가 파일 나머지를 통째로 삼키는 미탐을 막는다.
                    break
                out.append(" ")
                i += 1
            at_line_start = False
            continue

        if ch == "'" and not (i > 0 and (text[i - 1].isalnum() or text[i - 1] == "_")):
            # 앞이 식별자·숫자면 문자 리터럴이 아니라 숫자 구분자(1'000'000)다.
            out.append(" ")
            i += 1
            while i < n:
                cur = text[i]
                if cur == "\\":
                    out.append(" ")
                    i += 1
                    if i < n:
                        out.append("\n" if text[i] == "\n" else " ")
                        i += 1
                    continue
                if cur == "'":
                    out.append(" ")
                    i += 1
                    break
                if cur == "\n":
                    break
                out.append(" ")
                i += 1
            at_line_start = False
            continue

        out.append(ch)
        i += 1
        at_line_start = False

    return "".join(out)


def is_forbidden(spelling: str) -> bool:
    """include 표기(꺾쇠·따옴표 포함)가 금지 계열인지 판정한다."""
    name = spelling[1:-1] if len(spelling) >= 2 else spelling
    base = name.replace("\\", "/").rsplit("/", 1)[-1].strip().lower()
    if base in ALLOWED_HEADERS:
        return False
    return any(rx.match(base) is not None for rx in _FORBIDDEN_RE)


def find_violations(text: str, path: Path) -> list[Violation]:
    """소스 본문에서 금지 include 를 찾는다."""
    violations: list[Violation] = []
    for lineno, line in enumerate(
        strip_comments_and_literals(text).splitlines(), start=1
    ):
        match = _INCLUDE_LINE.match(line)
        if match is None:
            continue
        spelling = match.group(1)
        if is_forbidden(spelling):
            violations.append(Violation(path, lineno, spelling))
    return violations


def scan_file(path: Path) -> list[Violation]:
    """파일 1개를 검사한다. 소스가 UTF-8 BOM 으로 저장돼도 첫 줄을 놓치지 않는다."""
    text = path.read_text(encoding="utf-8-sig", errors="replace")
    return find_violations(text, path)


def iter_source_files(roots: Sequence[Path]) -> list[Path]:
    """roots 아래 C/C++ 소스·헤더를 재귀 수집한다(점으로 시작하는 디렉터리 제외)."""
    found: list[Path] = []
    for root in roots:
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
            for name in sorted(filenames):
                if Path(name).suffix.lower() in SOURCE_SUFFIXES:
                    found.append(Path(dirpath) / name)
    return found


def scan_files(paths: Sequence[Path]) -> tuple[list[Violation], list[str]]:
    """파일 목록을 검사해 (위반, 읽기 오류 메시지) 를 돌려준다."""
    violations: list[Violation] = []
    errors: list[str] = []
    for path in paths:
        try:
            violations.extend(scan_file(path))
        except OSError as exc:
            errors.append(f"{_display(path)}: 읽기 실패 - {exc}")
    return violations, errors


def run_scan(root_args: Sequence[str]) -> int:
    """--roots 모드. 종료 코드를 돌려준다."""
    roots: list[Path] = []
    for raw in root_args:
        root = Path(raw)
        if not root.is_dir():
            print(
                f"오류: 스캔 루트가 디렉터리가 아니다 - {_display(root)}",
                file=sys.stderr,
            )
            return 2
        roots.append(root)

    files = iter_source_files(roots)
    if not files:
        print(
            "오류: 스캔 대상 0건. 게이트가 아무것도 증명하지 못하므로 실패로 처리한다"
            f"(루트: {', '.join(_display(r) for r in roots)}).",
            file=sys.stderr,
        )
        return 2

    violations, errors = scan_files(files)
    for message in errors:
        print(f"오류: {message}", file=sys.stderr)
    if errors:
        return 2

    if violations:
        for item in violations:
            print(
                f"{_display(item.path)}:{item.line}: {item.spelling}", file=sys.stderr
            )
        print(
            f"core 헤더 격리 위반 {len(violations)}건 / 스캔 {len(files)}개 파일",
            file=sys.stderr,
        )
        return 1

    print(f"스캔 {len(files)}개 파일: 위반 없음")
    return 0


def run_self_test() -> int:
    """fixtures 로 게이트 자신을 양방향 검증한다. --roots 는 무시한다."""
    fixtures = Path(__file__).resolve().parent / "fixtures"
    good_dir = fixtures / "good"
    bad_dir = fixtures / "bad"

    for directory in (good_dir, bad_dir):
        if not directory.is_dir():
            print(
                f"오류: fixture 디렉터리 없음 - {_display(directory)}", file=sys.stderr
            )
            return 2

    good_files = iter_source_files([good_dir])
    bad_files = iter_source_files([bad_dir])
    if not good_files or not bad_files:
        print(
            "오류: fixture 가 비었다. 공허한 통과를 막기 위해 실패로 처리한다"
            f"(good {len(good_files)}건, bad {len(bad_files)}건).",
            file=sys.stderr,
        )
        return 2

    failures = 0

    print(f"[방향 1] known-good 수용 검사 - {len(good_files)}건")
    for path in good_files:
        try:
            violations = scan_file(path)
        except OSError as exc:
            print(f"  ERROR {_display(path)}: 읽기 실패 - {exc}", file=sys.stderr)
            failures += 1
            continue
        if violations:
            detail = ", ".join(f"{v.line}행 {v.spelling}" for v in violations)
            print(f"  FAIL  {_display(path)}: 오탐 {len(violations)}건 ({detail})")
            failures += 1
        else:
            print(f"  PASS  {_display(path)}: 수용")

    print(f"[방향 2] known-bad 거부 검사 - {len(bad_files)}건")
    for path in bad_files:
        try:
            violations = scan_file(path)
        except OSError as exc:
            print(f"  ERROR {_display(path)}: 읽기 실패 - {exc}", file=sys.stderr)
            failures += 1
            continue
        if violations:
            detail = ", ".join(f"{v.line}행 {v.spelling}" for v in violations)
            print(f"  PASS  {_display(path)}: 거부 ({detail})")
        else:
            print(f"  FAIL  {_display(path)}: 미탐 - 금지 include 를 잡지 못했다")
            failures += 1

    if failures:
        print(
            f"자기시험 실패 {failures}건 (good {len(good_files)}건 / bad {len(bad_files)}건)",
            file=sys.stderr,
        )
        return 1

    print(
        f"자기시험 PASS: good {len(good_files)}건 전건 수용, bad {len(bad_files)}건 전건 거부"
    )
    return 0


def _force_utf8_output() -> None:
    """콘솔 로케일(Windows 기본 cp949)과 무관하게 UTF-8 로 출력한다.

    파일 리다이렉션·CTest 캡처가 소스와 같은 인코딩으로 남고, 로케일에 없는
    문자가 경로·헤더 표기에 섞여도 UnicodeEncodeError 로 게이트가 거짓 실패하지
    않는다.
    """
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="backslashreplace")


def main(argv: Sequence[str] | None = None) -> int:
    _force_utf8_output()
    parser = argparse.ArgumentParser(
        description="core/ 계층의 플랫폼 헤더 격리 검사",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--roots",
        nargs="+",
        metavar="DIR",
        help="재귀 스캔할 디렉터리(1개 이상)",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="fixtures 기반 양방향 자기검증을 수행한다(--roots 는 무시)",
    )
    args = parser.parse_args(argv)

    if args.self_test:
        return run_self_test()
    if not args.roots:
        print("오류: --roots 또는 --self-test 중 하나가 필요하다.", file=sys.stderr)
        return 2
    return run_scan(args.roots)


if __name__ == "__main__":
    sys.exit(main())
