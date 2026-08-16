#!/usr/bin/env python3
"""연결 상태 동등성 게이트 (파이썬 연결 <-> C++ 연결).

지시서 T-R5 가 요구하는 것을 그대로 옮긴 게이트다 — 현행 파이썬 연결은
`PRAGMA synchronous` 를 명시 설정하지 않으므로 **특정 값을 강제하지 않는다.**
양쪽 연결의 실효값을 각각 실측해 **대조 일치**만 본다. 값이 무엇이어야 하는지는
이 게이트의 관심사가 아니고, 두 연결이 같은 상태에 있는지가 관심사다.

## 왜 파일을 열어 보는 방식으로는 안 되나

여기서 보는 값들은 대부분 **연결 속성이라 데이터베이스 파일에 영속되지 않는다.**
방출된 DB 를 나중에 파이썬으로 열어 읽으면 그것은 파이썬 연결의 값이지 C++ 연결의
값이 아니다. 그래서 C++ 쪽은 살아 있는 연결이 스스로 보고해야 하고,
`NoteExTests.exe "[pragma-emit]"` 가 그 역할을 한다(`NOTEEX_PRAGMA_OUT` 에 기록).

`journal_mode`·`page_size`·`encoding` 은 파일에 영속되지만 같은 목록에 함께 둔다 —
나누면 어느 것이 어느 쪽인지 읽는 사람이 매번 다시 판단해야 한다.

## 파이썬 쪽을 어떻게 재나

`src/pynote/infrastructure/database.py` 의 `_open` 경로를 재현한다: autocommit
연결, `PRAGMA foreign_keys = ON` 후 되읽어 검증, `PRAGMA journal_mode = WAL` 을
반환값으로 검증. 그 뒤 같은 PRAGMA 목록을 같은 순서로 읽는다. 원본 모듈을 적재해
`Database` 를 쓰지 않는 이유는 그쪽이 마이그레이션까지 돌려 v9 스키마를 만들기
때문이다 - 연결 상태를 재는 데 스키마는 필요 없고, 없는 편이 변수가 적다.

## 무엇을 증명하지 않나

- **값의 적절성.** `synchronous=2` 가 옳은 값인지 판단하지 않는다. 파이썬이 2 이고
  C++ 이 2 이면 통과이고, 둘 다 1 이어도 통과다. 갈리면 실패다.
- **SQLite 라이브러리 버전 일치.** 두 버전을 나란히 적지만 다르다고 실패시키지
  않는다(파이썬 3.49.1 / C++ house 3.50.4 로 원래 다르다). 버전 차이가 실효값을
  흔드는지가 이 게이트가 답하는 질문이고, 흔들면 값 비교에서 잡힌다.
- **연결 수명 동안의 변화.** 열린 직후 한 번 잰다. 이후 누가 바꾸면 보지 못한다.

종료 코드:
  0  전건 일치
  1  발산 검출(자기시험이면 기대 불일치)
  2  사용법·환경 오류 — 실행 파일 없음, 방출기 비정상 종료, 방출 항목 0건
"""

from __future__ import annotations

import argparse
import os
import sqlite3
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Sequence

REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_EXE = REPO_ROOT / "NoteEx" / "x64" / "ReleaseMD" / "NoteExTests.exe"

# C++ 방출기와 같은 목록·같은 순서. 한쪽만 고치면 항목 수 불일치로 실패한다.
PRAGMAS: tuple[str, ...] = (
    "synchronous",
    "journal_mode",
    "foreign_keys",
    "page_size",
    "encoding",
    "auto_vacuum",
    "temp_store",
)


def _display(path: Path) -> str:
    return str(path).replace("\\", "/")


def measure_python(db_path: Path) -> dict[str, str]:
    """database.py 의 _open 경로를 재현한 뒤 실효값을 읽는다."""
    connection = sqlite3.connect(db_path, isolation_level=None)
    try:
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA foreign_keys = ON")
        row = connection.execute("PRAGMA foreign_keys").fetchone()
        if row is None or row[0] != 1:
            raise RuntimeError("파이썬 연결에서 foreign_keys 활성화에 실패했다.")
        row = connection.execute("PRAGMA journal_mode = WAL").fetchone()
        if row is None or str(row[0]).lower() != "wal":
            raise RuntimeError("파이썬 연결에서 WAL 활성화 검증에 실패했다.")

        values: dict[str, str] = {}
        for name in PRAGMAS:
            row = connection.execute(f"PRAGMA {name}").fetchone()
            values[name] = "" if row is None else str(row[0])
        values["sqlite_version"] = sqlite3.sqlite_version
        return values
    finally:
        connection.close()


def measure_cpp(exe: Path, timeout: float) -> dict[str, str]:
    """`NoteExTests.exe "[pragma-emit]"` 를 돌려 방출 파일을 읽는다."""
    with tempfile.TemporaryDirectory() as directory:
        out_path = Path(directory) / "pragma.txt"
        environment = dict(os.environ)
        environment["NOTEEX_PRAGMA_OUT"] = str(out_path)
        completed = subprocess.run(
            [str(exe), "[pragma-emit]"],
            cwd=str(exe.parent),
            env=environment,
            capture_output=True,
            timeout=timeout,
            check=False,
        )
        if completed.returncode != 0:
            tail = completed.stdout.decode("cp949", errors="replace").strip().splitlines()[-3:]
            raise RuntimeError(
                f"방출기가 종료 코드 {completed.returncode} 로 끝났다: {' / '.join(tail)}"
            )
        if not out_path.is_file():
            raise RuntimeError("방출기가 종료 코드 0 이지만 출력 파일이 없다.")

        values: dict[str, str] = {}
        for line in out_path.read_text(encoding="utf-8").splitlines():
            if not line.strip():
                continue
            name, _, value = line.partition("=")
            values[name.strip()] = value.strip()
        return values


def compare(left: dict[str, str], right: dict[str, str]) -> list[str]:
    problems: list[str] = []
    for name in PRAGMAS:
        if name not in left:
            problems.append(f"파이썬 쪽에 {name} 이 없다")
            continue
        if name not in right:
            problems.append(f"C++ 쪽에 {name} 이 없다 - 방출기 목록이 어긋났다")
            continue
        if left[name] != right[name]:
            problems.append(f"{name}: 파이썬 {left[name]!r} vs C++ {right[name]!r}")
    return problems


def run_self_test() -> int:
    """C++ 빌드 없이 게이트 자신을 검증한다."""
    failures = 0
    base = {name: "x" for name in PRAGMAS}
    base["sqlite_version"] = "3.49.1"

    if compare(dict(base), dict(base)):
        print("  FAIL  같은 값끼리 거절됐다")
        failures += 1
    else:
        print("  PASS  같은 값 수용")

    for name in ("synchronous", "journal_mode", "foreign_keys"):
        perturbed = dict(base)
        perturbed[name] = "different"
        if compare(dict(base), perturbed):
            print(f"  PASS  발산 거부: {name}")
        else:
            print(f"  FAIL  {name} 발산을 못 잡았다")
            failures += 1

    missing = dict(base)
    del missing["synchronous"]
    if compare(dict(base), missing):
        print("  PASS  방출기 목록 결손 거부")
    else:
        print("  FAIL  방출기 목록 결손을 못 잡았다")
        failures += 1

    # 버전 차이는 실패가 아니다 - 두 라이브러리는 원래 다르다.
    version_only = dict(base)
    version_only["sqlite_version"] = "3.50.4"
    if compare(dict(base), version_only):
        print("  FAIL  버전 차이만으로 실패했다 - 그건 이 게이트의 판정 대상이 아니다")
        failures += 1
    else:
        print("  PASS  버전 차이만으로는 실패하지 않는다")

    # 파이썬 쪽 실측이 실제로 값을 돌려주는지. 0건을 통과로 보고하는 경로를 닫는다.
    with tempfile.TemporaryDirectory() as directory:
        measured = measure_python(Path(directory) / "probe.db")
    if len(measured) >= len(PRAGMAS):
        print(f"  PASS  파이썬 실측 {len(measured)} 항목 회수")
    else:
        print(f"  FAIL  파이썬 실측이 {len(measured)} 항목만 돌려줬다")
        failures += 1

    print(
        "자기시험 PASS: 동일 수용, 발산 4종 거부, 버전 차이 비판정, 파이썬 실측 확인"
        if failures == 0
        else f"자기시험 실패 {failures} 건"
    )
    return 1 if failures else 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--self-test", action="store_true", help="C++ 빌드 없이 게이트 자신을 검증한다")
    parser.add_argument("--exe", type=Path, default=DEFAULT_EXE, help="시험 실행 파일 경로")
    parser.add_argument("--timeout", type=float, default=120.0, help="방출기 제한 시간(초)")
    args = parser.parse_args(argv)

    if args.self_test:
        return run_self_test()

    if not args.exe.is_file():
        print(f"오류: 시험 실행 파일이 없다 - {_display(args.exe)}", file=sys.stderr)
        print("      ReleaseMD 산출물이 필요하다. 이 게이트는 빌드하지 않는다.", file=sys.stderr)
        return 2

    try:
        with tempfile.TemporaryDirectory() as directory:
            python_values = measure_python(Path(directory) / "probe.db")
        cpp_values = measure_cpp(args.exe, args.timeout)
    except subprocess.TimeoutExpired:
        print(f"오류: 방출기가 {args.timeout} 초 안에 끝나지 않았다.", file=sys.stderr)
        return 2
    except (RuntimeError, sqlite3.Error) as error:
        print(f"오류: {error}", file=sys.stderr)
        return 2

    if not cpp_values:
        print("오류: 방출 항목 0건 - 비교할 것이 없다.", file=sys.stderr)
        return 2

    problems = compare(python_values, cpp_values)
    print(f"C++ 방출기: {_display(args.exe)} ({args.exe.stat().st_size}바이트)")
    print(f"  SQLite 라이브러리: 파이썬 {python_values.get('sqlite_version', '?')}"
          f" / C++ {cpp_values.get('sqlite_version', '?')} (판정 대상 아님)")
    for name in PRAGMAS:
        print(f"  {name:14} 파이썬 {python_values.get(name, '<없음>')!r}"
              f"  C++ {cpp_values.get(name, '<없음>')!r}")

    if problems:
        for line in problems:
            print("발산:", line, file=sys.stderr)
        return 1

    print(f"연결 상태 동등: PRAGMA {len(PRAGMAS)} 항 전건 일치")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
