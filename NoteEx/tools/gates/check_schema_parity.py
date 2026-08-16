#!/usr/bin/env python3
"""빈 데이터베이스 스키마 동등성 게이트(파이썬 러너 <-> C++ 러너).

파이썬 원본이 빈 파일에서 최신 버전까지 올린 데이터베이스와, C++ 이식본이 같은
일을 한 데이터베이스를 각각 `sqlite_master` 로 덤프해 **문자열이 완전히 같은지**
본다. 축자 이식이 지켜졌는지는 정적 게이트(`check_migration_sql_parity.py`)가 소스
수준에서 보고, 이 게이트는 그 소스가 실제로 만들어 내는 **결과 스키마**를 본다.
둘은 대체 관계가 아니다 - `INSERT` 는 스키마 객체를 만들지 않아 이 게이트에
보이지 않고, 문장 앞뒤의 개행·들여쓰기는 SQLite 가 저장 시 버리므로 역시 보이지
않는다. 그 사각은 정적 게이트가 덮는다.

**사거리 주의**: 이 게이트는 빈 데이터베이스 한 경로만, 그것도 스키마만 본다.
기존 데이터베이스를 갱신하는 경로와 **행 데이터**는 사다리 게이트
(`check_migration_ladder_parity.py`)가 본다. 사다리 게이트의 경로 A 가 이 게이트를
포함하므로, 이쪽은 fixture 없이 빠르게 스키마만 확인하고 싶을 때 쓰는 가벼운
게이트다.

(T-R2 갱신: `[parity-emit]` 의 계약은 그대로지만 등록 목록이 v9 까지 늘어 이제
v9 데이터베이스가 나온다. 그래서 파이썬 쪽도 `MIGRATIONS` 전건을 적용한다 -
v0001 만 적용하던 T-R1 판 그대로 두면 이식이 옳아도 실패한다.)

정규화 규칙(버전 관리되는 계약이므로 여기 적어 둔다):
  * 대상 = `sqlite_master` 의 `type` `name` `tbl_name` `sql` 네 열.
  * `rootpage` 는 **제외**한다. 물리 페이지 할당 번호라 같은 스키마라도 문장
    실행 순서·페이지 재사용에 따라 달라지는 값이고, 스키마 동등성의 일부가 아니다.
  * 정렬은 `ORDER BY type, name, tbl_name`(SQLite 기본 BINARY 조합). 삽입 순서·
    페이지 배치와 무관하게 결정적이다.
  * `sql` 본문은 **어떤 공백 정규화도 하지 않는다.** 줄끝 차이(CRLF 대 LF)도
    데이터 수준의 실제 발산이므로 실패다. 저장된 스키마 원문은 그 자체가 계약이다.
  * 표시 이스케이프: 덤프에 실을 때 역슬래시·CR·LF·탭을 `\\\\` `\\r` `\\n` `\\t`
    로 바꾼다. 양쪽에 똑같이 적용하는 단사(injective) 변환이라 서로 다른 원문이
    같은 덤프가 되는 일이 없다 - **정규화가 아니라 표기**이며, 눈에 보이지 않는
    CR 차이를 diff 에서 읽을 수 있게 하려는 것이다.
  * 자동 생성 인덱스(`sqlite_autoindex_*`)와 `sqlite_sequence` 는 제외하지 않는다.
    UNIQUE·PRIMARY KEY·AUTOINCREMENT 선언의 결과라 스키마 정체성의 일부다.
  * 행 데이터는 비교 대상이 아니다. `schema_version.applied_at_us` 처럼 양쪽이
    각자 생성하는 값은 이 게이트의 관심사가 아니며, 자기시험이 서로 다른
    `applied_at_us` 로도 덤프가 같음을 보여 이를 증명한다. 행을 보는 것은 사다리
    게이트의 몫이다.

파이썬 쪽은 `database.py` 의 연결 수명주기와 트랜잭션 구조를 그대로 재현하고
(`migration_reference` 모듈), 스키마는 원본 모듈을 적재해 `migrate` 를 **직접
호출**한다. 이 파일에는 스키마 SQL 사본이 한 줄도 없다.

C++ 쪽 계약(고정):
  * 환경 변수 `NOTEEX_PARITY_DB` = 만들 데이터베이스의 UTF-8 경로
  * 명령 `NoteExTests.exe "[parity-emit]"`
  * 성공 시 종료 코드 0

종료 코드:
  0  통과(두 덤프가 완전히 같음)
  1  스키마 발산 검출(또는 자기시험 기대 불일치)
  2  사용법·환경 오류(실행 파일 없음·**태그 없음**·비정상 종료·**스키마 객체 0건** 등)

스키마 객체 0건을 통과가 아니라 오류로 두는 것은 의도된 선택이다. 빈 데이터베이스
둘은 언제나 "같으므로", 그것을 통과로 부르면 게이트가 아무것도 증명하지 못한다.
같은 이유로 태그 존재를 먼저 확인한다 - 태그를 모르는 실행 파일은 그 태그를 주면
아무것도 하지 않고 **종료 코드 0** 을 낸다(2026-08-16 실측).
"""

from __future__ import annotations

import argparse
import difflib
import importlib.util
import os
import sqlite3
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import NamedTuple, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))

import migration_reference as reference  # noqa: E402
from migration_reference import SchemaDump, display_path, tail  # noqa: E402

DEFAULT_PYTHON_MIGRATION = (
    reference.PYTHON_MIGRATIONS_DIR / "v0001_initial.py"
)
DEFAULT_TEST_EXE = reference.DEFAULT_TEST_EXE

# C++ 방출기 계약(SPEC 고정값).
PARITY_DB_ENV = "NOTEEX_PARITY_DB"
PARITY_EMIT_TAG = "[parity-emit]"
EMIT_TIMEOUT_SECONDS = 180.0


class EmitResult(NamedTuple):
    """C++ 방출기 실행 결과."""

    returncode: int
    stdout: str
    stderr: str


def load_reference_statements(path: Path) -> tuple[str, ...]:
    """원본 `STATEMENTS` 를 그대로 돌려준다. 자기시험의 교란 대상이다."""
    spec = importlib.util.spec_from_file_location("pynote_reference_statements", path)
    if spec is None or spec.loader is None:
        raise ImportError(f"모듈 스펙을 만들 수 없다: {display_path(path)}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    try:
        spec.loader.exec_module(module)
    finally:
        sys.modules.pop(spec.name, None)

    statements = getattr(module, "STATEMENTS", None)
    if not isinstance(statements, tuple) or not statements:
        raise AttributeError(f"STATEMENTS 가 없거나 비었다: {display_path(path)}")
    return statements


def build_database_from_statements(
    db_path: Path, statements: Sequence[str], applied_at_us: int
) -> None:
    """교란된 문장 목록으로 데이터베이스를 만든다. 자기시험 전용이다.

    `schema_version` upsert 는 원본 `migrate` 의 꼬리와 같은 역할이지만, 자기시험은
    스키마 발산 탐지력만 보므로 문장 목록 실행까지만 한다.
    """
    connection = reference.open_connection(db_path)
    try:
        connection.execute("BEGIN IMMEDIATE")
        try:
            for statement in statements:
                connection.execute(statement)
            connection.commit()
        except BaseException:
            connection.rollback()
            raise
    finally:
        connection.close()


def diff_dumps(
    left: SchemaDump, right: SchemaDump, left_label: str, right_label: str
) -> list[str]:
    """두 덤프의 통합 diff. 빈 목록이 곧 동등성 증명이다."""
    return [
        line.rstrip("\n")
        for line in difflib.unified_diff(
            left.text.splitlines(),
            right.text.splitlines(),
            fromfile=left_label,
            tofile=right_label,
            lineterm="",
        )
    ]


def emit_cpp_database(exe: Path, db_path: Path, timeout: float) -> EmitResult:
    """C++ 방출기를 계약대로 호출한다."""
    env = os.environ.copy()
    env[PARITY_DB_ENV] = str(db_path)
    completed = subprocess.run(
        [str(exe), PARITY_EMIT_TAG],
        cwd=str(exe.parent),
        env=env,
        capture_output=True,
        timeout=timeout,
    )
    return EmitResult(
        returncode=completed.returncode,
        stdout=reference.decode_process_output(completed.stdout),
        stderr=reference.decode_process_output(completed.stderr),
    )


def run_check(exe: Path, timeout: float = EMIT_TIMEOUT_SECONDS) -> int:
    """실제 두 러너의 결과 스키마를 대조한다. 종료 코드를 돌려준다."""
    if not exe.is_file():
        print(
            f"오류: 시험 실행 파일이 없다 - {display_path(exe)}\n"
            "      ReleaseMD 산출물이 필요하다. 이 게이트는 빌드를 수행하지 않는다"
            "(공유 출력 디렉터리를 건드리지 않기 위한 의도된 제약이다).",
            file=sys.stderr,
        )
        return 2

    stat = exe.stat()
    # flush 는 로그 순서 보장용이다 - stderr 는 버퍼링되지 않아 이 줄이 뒤로 밀린다.
    print(
        f"C++ 방출기: {display_path(exe)} "
        f"({stat.st_size}바이트, "
        f"{time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(stat.st_mtime))})",
        flush=True,
    )

    try:
        listing = reference.count_matching_tests(exe, PARITY_EMIT_TAG, timeout)
    except (subprocess.TimeoutExpired, OSError) as exc:
        print(f"오류: 태그 조회 실패 - {PARITY_EMIT_TAG}: {exc}", file=sys.stderr)
        return 2
    if listing.matched != 1:
        print(
            f"오류: 실행 파일이 {PARITY_EMIT_TAG} 태그를 모른다"
            f"(일치 {listing.matched}건, 기대 1건). 낡은 실행 파일이다 - "
            "태그를 모르면 종료 코드 0 과 함께 아무것도 하지 않는다.",
            file=sys.stderr,
        )
        return 2

    try:
        entries = reference.load_registry()
    except Exception as exc:
        print(f"오류: 파이썬 원본 적재 실패 - {exc}", file=sys.stderr)
        return 2
    latest = reference.latest_version(entries)
    print(f"  등록 마이그레이션 {len(entries)}건, 최신 버전 {latest}")

    with tempfile.TemporaryDirectory(prefix="schema_parity_") as work:
        work_dir = Path(work)
        python_db = work_dir / "python_latest.db"
        cpp_db = work_dir / "cpp_latest.db"

        try:
            reference.build_database(
                python_db, entries, time.time_ns() // 1_000
            )
        except (sqlite3.Error, RuntimeError) as exc:
            print(f"오류: 파이썬 러너 실행 실패 - {exc}", file=sys.stderr)
            return 2

        try:
            emitted = emit_cpp_database(exe, cpp_db, timeout)
        except subprocess.TimeoutExpired:
            print(
                f"오류: C++ 방출기가 {timeout:.0f}초 안에 끝나지 않았다.",
                file=sys.stderr,
            )
            return 2
        except OSError as exc:
            print(f"오류: C++ 방출기 실행 실패 - {exc}", file=sys.stderr)
            return 2

        if emitted.returncode != 0:
            print(
                f"오류: C++ 방출기 종료 코드 {emitted.returncode} "
                f"(기대 0). 명령: {display_path(exe)} \"{PARITY_EMIT_TAG}\"\n"
                f"    stdout:\n{tail(emitted.stdout)}\n"
                f"    stderr:\n{tail(emitted.stderr)}",
                file=sys.stderr,
            )
            return 2

        if not cpp_db.is_file():
            print(
                f"오류: C++ 방출기가 종료 코드 0 을 냈지만 "
                f"{PARITY_DB_ENV} 경로에 데이터베이스가 없다 - {display_path(cpp_db)}\n"
                f"    stdout:\n{tail(emitted.stdout)}",
                file=sys.stderr,
            )
            return 2

        python_dump = reference.dump_schema(python_db)
        cpp_dump = reference.dump_schema(cpp_db)

        if python_dump.objects == 0 or cpp_dump.objects == 0:
            print(
                f"오류: 스키마 객체 0건(파이썬 {python_dump.objects}건 / "
                f"C++ {cpp_dump.objects}건). 빈 데이터베이스끼리는 언제나 같으므로 "
                "통과로 부를 수 없다.",
                file=sys.stderr,
            )
            return 2

        cpp_version = reference.database_version(cpp_db)
        diff = diff_dumps(python_dump, cpp_dump, f"python/v{latest}", f"cpp/v{cpp_version}")
        if diff:
            for line in diff:
                print(line, file=sys.stderr)
            print(
                f"스키마 발산: diff {len(diff)}줄 "
                f"(파이썬 v{latest} {python_dump.objects}객체 / "
                f"C++ v{cpp_version} {cpp_dump.objects}객체)",
                file=sys.stderr,
            )
            return 1

        if cpp_version != latest:
            print(
                f"오류: 스키마는 같은데 C++ schema version 이 {cpp_version} 이다"
                f"(기대 {latest}).",
                file=sys.stderr,
            )
            return 1

        print(
            f"스키마 동등: sqlite_master {python_dump.objects}객체 전건 일치 "
            f"(v{latest}, diff 0줄)"
        )
        return 0


def _fixture_dir() -> Path:
    return Path(__file__).resolve().parent / "fixtures" / "schema_parity" / "dynamic"


def _load_perturbations(path: Path) -> Sequence[object]:
    """교란 fixture 를 경로로 적재한다."""
    spec = importlib.util.spec_from_file_location("schema_parity_perturbations", path)
    if spec is None or spec.loader is None:
        raise ImportError(f"모듈 스펙을 만들 수 없다: {display_path(path)}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    try:
        spec.loader.exec_module(module)
    finally:
        sys.modules.pop(spec.name, None)

    perturbations = getattr(module, "PERTURBATIONS", None)
    if not perturbations:
        raise AttributeError(f"PERTURBATIONS 가 없거나 비었다: {display_path(path)}")
    return perturbations


def run_self_test(python_migration: Path) -> int:
    """C++ 빌드 없이 게이트 자신을 양방향 검증한다.

    파이썬 러너만으로 known-good 쌍(같은 스키마)과 known-bad 쌍(교란된 스키마)을
    만들어, 통과해야 할 것이 통과하고 잡아야 할 것이 잡히는지 본다.
    """
    fixture_path = _fixture_dir() / "perturbations.py"
    if not python_migration.is_file():
        print(f"오류: 파이썬 원본이 없다 - {display_path(python_migration)}", file=sys.stderr)
        return 2
    if not fixture_path.is_file():
        print(f"오류: fixture 없음 - {display_path(fixture_path)}", file=sys.stderr)
        return 2

    try:
        entries = reference.load_registry()
        statements = load_reference_statements(python_migration)
        perturbations = _load_perturbations(fixture_path)
    except Exception as exc:
        print(f"오류: 적재 실패 - {exc}", file=sys.stderr)
        return 2

    failures = 0

    with tempfile.TemporaryDirectory(prefix="schema_parity_selftest_") as work:
        work_dir = Path(work)

        print(
            f"[방향 1] known-good 수용 - 같은 사다리(v1~v{reference.latest_version(entries)}), "
            "다른 applied_at_us"
        )
        left_db = work_dir / "good_left.db"
        right_db = work_dir / "good_right.db"
        try:
            reference.build_database(left_db, entries, 1_700_000_000_000_000)
            reference.build_database(right_db, entries, 1_900_000_000_999_999)
        except (sqlite3.Error, RuntimeError) as exc:
            print(f"  ERROR 기준 데이터베이스 생성 실패 - {exc}", file=sys.stderr)
            return 2

        left_dump = reference.dump_schema(left_db)
        right_dump = reference.dump_schema(right_db)
        if left_dump.objects == 0:
            print("  ERROR 기준 덤프의 스키마 객체가 0건이다.", file=sys.stderr)
            return 2

        good_diff = diff_dumps(left_dump, right_dump, "python/a", "python/b")
        if good_diff:
            print(f"  FAIL  같은 스키마인데 diff {len(good_diff)}줄 - 오탐")
            for line in good_diff[:20]:
                print(f"        {line}")
            failures += 1
        else:
            print(f"  PASS  sqlite_master {left_dump.objects}객체 일치, diff 0줄")

        # 기준 문장만으로 만든 데이터베이스(교란 대조군의 짝).
        base_db = work_dir / "base.db"
        try:
            build_database_from_statements(base_db, statements, 1_700_000_000_000_000)
        except sqlite3.Error as exc:
            print(f"  ERROR 기준 문장 실행 실패 - {exc}", file=sys.stderr)
            return 2
        base_dump = reference.dump_schema(base_db)

        print(f"[방향 2] known-bad 거부 - 교란 {len(perturbations)}종")
        for order, perturbation in enumerate(perturbations):
            name = getattr(perturbation, "name", f"perturbation_{order}")
            describe = getattr(perturbation, "describe", "")
            detected = bool(getattr(perturbation, "detected", True))
            apply = getattr(perturbation, "apply", None)
            if apply is None:
                print(f"  FAIL  {name}: apply 가 없다")
                failures += 1
                continue

            try:
                mutated = tuple(apply(statements))
            except Exception as exc:  # fixture 결함을 자기시험 실패로 드러낸다
                print(f"  FAIL  {name}: 교란 함수가 예외를 냈다 - {exc}")
                failures += 1
                continue

            if tuple(mutated) == tuple(statements):
                print(
                    f"  FAIL  {name}: 교란이 아무것도 바꾸지 않았다"
                    "(원본 문면이 바뀌어 무효가 된 fixture 다)"
                )
                failures += 1
                continue

            mutated_db = work_dir / f"mutated_{order}.db"
            try:
                build_database_from_statements(
                    mutated_db, mutated, 1_700_000_000_000_000
                )
            except sqlite3.Error as exc:
                print(f"  FAIL  {name}: 교란된 스키마를 만들 수 없다 - {exc}")
                failures += 1
                continue

            mutated_dump = reference.dump_schema(mutated_db)
            diff = diff_dumps(base_dump, mutated_dump, "python/base", f"python/{name}")

            if detected and diff:
                print(f"  PASS  {name}: 거부 (diff {len(diff)}줄) - {describe}")
            elif detected and not diff:
                print(f"  FAIL  {name}: 미탐 - {describe}")
                failures += 1
            elif not detected and not diff:
                print(
                    f"  PASS  {name}: 설계상 비탐지 확인(diff 0줄) - {describe}"
                )
            else:
                print(
                    f"  FAIL  {name}: 비탐지 대조군인데 diff {len(diff)}줄이 나왔다. "
                    "게이트의 사거리가 바뀌었으니 문서를 갱신해야 한다."
                )
                failures += 1

        print("[방향 3] 스키마 객체 0건 - 통과가 아니라 환경 오류인가")
        empty_db = work_dir / "empty.db"
        empty_connection = reference.open_connection(empty_db)
        empty_connection.close()
        empty_dump = reference.dump_schema(empty_db)
        empty_diff = diff_dumps(empty_dump, empty_dump, "empty", "empty")
        if empty_dump.objects == 0 and not empty_diff:
            print(
                "  PASS  빈 데이터베이스끼리는 diff 0줄이지만 객체 0건이라 "
                "run_check 가 종료 코드 2 로 막는다"
            )
        else:
            print(
                f"  FAIL  빈 데이터베이스 가정이 깨졌다"
                f"(객체 {empty_dump.objects}건, diff {len(empty_diff)}줄)"
            )
            failures += 1

    if failures:
        print(f"자기시험 실패 {failures}건", file=sys.stderr)
        return 1

    print(
        f"자기시험 PASS: 동일 쌍 수용, 교란 {len(perturbations)}종 기대대로 판정, "
        "객체 0건은 환경 오류"
    )
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    reference.force_utf8_output()
    parser = argparse.ArgumentParser(
        description="빈 데이터베이스 결과 스키마의 파이썬 <-> C++ 동등성 대조",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--python-migration",
        default=str(DEFAULT_PYTHON_MIGRATION),
        metavar="PATH",
        help=(
            "자기시험 교란의 대상이 되는 원본 모듈"
            "(기본: src/pynote/.../v0001_initial.py). 실제 대조는 MIGRATIONS 전건을 쓴다"
        ),
    )
    parser.add_argument(
        "--exe",
        default=str(DEFAULT_TEST_EXE),
        metavar="PATH",
        help="C++ 시험 실행 파일(기본: NoteEx/x64/ReleaseMD/NoteExTests.exe)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=EMIT_TIMEOUT_SECONDS,
        metavar="SEC",
        help=f"C++ 방출기 제한 시간(기본 {EMIT_TIMEOUT_SECONDS:.0f}초)",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="C++ 빌드 없이 자기검증을 수행한다(--exe 는 무시)",
    )
    args = parser.parse_args(argv)

    if args.self_test:
        return run_self_test(Path(args.python_migration))
    return run_check(Path(args.exe), args.timeout)


if __name__ == "__main__":
    sys.exit(main())
