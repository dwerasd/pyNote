#!/usr/bin/env python3
"""사다리 동등성 게이트(파이썬 러너 <-> C++ 러너, v1~v9 전 구간).

두 경로를 본다.

  경로 A - 빈 데이터베이스에서 한 번에 v9 까지. 양쪽이 각자 처음부터 만든다.
  경로 B - N = 1..8 각각에 대해, **정확히 버전 N 에 머무는** 데이터베이스를
           똑같이 두 벌 복사해 한 벌은 파이썬 러너가, 한 벌은 C++ 러너가 v9 까지
           올린다. 기존 사용자 데이터베이스가 갱신되는 실제 상황이 이쪽이다.

경로 B 의 fixture 는 `make_ladder_fixtures.py` 가 만들며 **실제 행이 들어 있다.**
빈 표 위의 사다리는 v0004 의 행 복사, v0005 의 이름변경-복사-삭제, v0007/v0008 의
초기화, v0009 의 조건부 갱신을 전부 건너뛴다 - 이식이 틀렸어도 결과가 같아진다.

## 무엇을 비교하나

**스키마**(T-R1 게이트와 같은 규칙):
  * 대상 = `sqlite_master` 의 `type` `name` `tbl_name` `sql` 네 열.
  * `rootpage` 는 **제외**. 물리 페이지 할당 번호라 같은 스키마라도 실행 순서·페이지
    재사용에 따라 달라지며 스키마 동등성의 일부가 아니다.
  * 정렬은 `ORDER BY type, name, tbl_name`(SQLite 기본 BINARY 조합).
  * `sql` 본문은 **어떤 공백 정규화도 하지 않는다.** 줄끝 차이도 실제 발산이다.
  * `sqlite_autoindex_*` 와 `sqlite_sequence` 는 제외하지 않는다.

**행 데이터**(T-R1 게이트가 전혀 보지 않던 부분이며 이 게이트의 존재 이유다):
  * 대상 = 사용자 표 전부 + `sqlite_sequence`. `schema_version`, `counters`,
    `data_policy_settings`, `document_ui_states`, `workspace_windows` 가 여기 든다.
    표를 고정 목록으로 적지 않는 이유는, 마이그레이션이 새 표를 만들면 그것도
    자동으로 대조 대상이 되어야 하기 때문이다.
  * 행 순서는 인코딩된 문자열로 정렬해 결정적으로 만든다(삽입 순서 무관).
  * 값은 형까지 드러나게 적는다(`INT:` `REAL:` `TEXT:` `BLOB:` `NULL`). 형이
    바뀌는 것도 발산이다.

## 정규화 규칙 - 비결정 열 (계약이므로 여기 적어 둔다)

세 열은 러너가 **스스로 값을 만들기 때문에** 양쪽이 같을 수 없다.

  | 열 | 만드는 마이그레이션 | 처리 |
  |---|---|---|
  | `schema_version.applied_at_us` | 전건(매 마이그레이션이 다시 쓴다) | 언제나 자리표시자 |
  | `data_policy_settings.updated_at_us` | v0002 | 구간 안이면 자리표시자 |
  | `workspace_windows.window_id` | v0004 (`randomblob`/`random`) | 구간 안이면 자리표시자 |

**"구간 안"은 대조 구간 `(N, 9]` 를 뜻한다.** 그 마이그레이션이 이번 대조에서
실제로 실행되면 값은 러너가 만든 것이라 대조할 수 없고, 실행되지 않으면 fixture 가
준 값을 양쪽이 그대로 나르는 것이므로 **축자 대조 대상**이다. 예를 들어 N=5 경로에서
`window_id` 는 v0004 가 이미 지난 뒤라 두 러너가 똑같이 보존해야 하고, 값이 달라지면
실패다. 이 구분이 없으면 "언제나 제외"가 되어 v0004 이후 구간에서 창 행이 통째로
사라져도 게이트가 통과한다.

자리표시자로 바꾼 값도 **검사는 하고 넘어간다.** `applied_at_us` 계열은 양의 정수여야
하고, `window_id` 는 v0004 의 식이 만드는 UUIDv4 모양이어야 한다. 양쪽이 똑같이
망가지면 diff 가 비어 통과해 버리는 구멍을 이 검사가 막는다.

`workspace_windows.window_id` 를 **말없이 빼지 않는 이유**가 이것이다. 비결정이라는
사실은 값 대조를 포기할 근거이지 열의 존재·행 수·모양까지 포기할 근거가 아니다.

## C++ 러너 계약 (고정, SPEC §3)

| 태그 | 환경 변수 | 뜻 |
|---|---|---|
| `[parity-emit]` | `NOTEEX_PARITY_DB` | 대상과 사이드카를 지우고 처음부터 만든다 |
| `[parity-upgrade]` | `NOTEEX_PARITY_DB` | **이미 있는** 데이터베이스를 열어 최신까지 올린다 |

**태그 존재를 먼저 확인한다.** 태그를 모르는 낡은 실행 파일에
`NoteExTests.exe "[parity-upgrade]"` 를 주면 "No test cases matched" 를 찍고
**종료 코드 0** 을 낸다(2026-08-16 실측). 종료 코드만 보면 낡은 실행 파일이 조용히
통과한다. 그래서 `--list-tests <태그> --reporter xml` 로 `<TestCase>` 수를 먼저 센다.

## 종료 코드

  0  통과(모든 경로에서 스키마·데이터 diff 0줄)
  1  발산 검출(또는 자기시험 기대 불일치)
  2  사용법·환경 오류 - 실행 파일 없음·**태그 없음**·비정상 종료·시간 초과·
     fixture 생성 실패·**스키마 객체 0건**·**행 0건**

**이 게이트는 빌드하지 않는다.** 출력 디렉터리가 공유 자원이라 게이트가 멋대로
빌드하면 동시에 작업 중인 빌드를 망가뜨린다. 실행 파일이 없거나 낡았으면 2 로 멈추고
사실을 보고한다.
"""

from __future__ import annotations

import argparse
import difflib
import importlib.util
import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Callable, NamedTuple, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))

import make_ladder_fixtures as fixtures  # noqa: E402
import migration_reference as reference  # noqa: E402
from migration_reference import MigrationEntry, display_path, tail  # noqa: E402

PARITY_DB_ENV = "NOTEEX_PARITY_DB"
PARITY_EMIT_TAG = "[parity-emit]"
PARITY_UPGRADE_TAG = "[parity-upgrade]"
RUN_TIMEOUT_SECONDS = 180.0

# 실패 경로 1건당 인용할 diff 줄 수 상한. 전문 덤프는 읽히지 않는다.
DIFF_LINE_LIMIT = 80

# 사다리 러너가 쓰는 applied_at_us. fixture 상수와 겹치지 않게 크게 둔다.
PYTHON_APPLIED_AT_US = 1_800_000_000_000_000


class PathResult(NamedTuple):
    """경로 1건의 판정 결과."""

    label: str
    start_version: int
    schema_diff: list[str]
    data_diff: list[str]
    violations: list[str]
    objects: int
    rows: int
    notes: list[str]

    @property
    def failed(self) -> bool:
        return bool(self.schema_diff or self.data_diff or self.violations or self.notes)


class RunResult(NamedTuple):
    """C++ 러너 실행 결과."""

    returncode: int
    stdout: str
    stderr: str


def _diff(left: str, right: str, left_label: str, right_label: str) -> list[str]:
    return [
        line.rstrip("\n")
        for line in difflib.unified_diff(
            left.splitlines(),
            right.splitlines(),
            fromfile=left_label,
            tofile=right_label,
            lineterm="",
        )
    ]


def compare_databases(
    label: str,
    start_version: int,
    reference_db: Path,
    candidate_db: Path,
    expected_version: int,
    reference_label: str = "python",
    candidate_label: str = "cpp",
) -> PathResult:
    """두 데이터베이스의 스키마·행 데이터를 대조한다."""
    notes: list[str] = []

    reference_schema = reference.dump_schema(reference_db)
    candidate_schema = reference.dump_schema(candidate_db)
    reference_data = reference.dump_data(reference_db, start_version)
    candidate_data = reference.dump_data(candidate_db, start_version)

    if reference_schema.objects == 0 or candidate_schema.objects == 0:
        notes.append(
            f"스키마 객체 0건({reference_label} {reference_schema.objects}건 / "
            f"{candidate_label} {candidate_schema.objects}건). 빈 데이터베이스끼리는 "
            "언제나 같으므로 통과로 부를 수 없다."
        )
    if reference_data.rows == 0 or candidate_data.rows == 0:
        notes.append(
            f"행 0건({reference_label} {reference_data.rows}건 / "
            f"{candidate_label} {candidate_data.rows}건). 행 불변식을 보는 것이 이 "
            "게이트의 목적이므로 행이 없으면 판정할 수 없다."
        )

    candidate_version = reference.database_version(candidate_db)
    if candidate_version != expected_version:
        notes.append(
            f"{candidate_label} 최종 schema version 이 {candidate_version} 이다"
            f"(기대 {expected_version})."
        )

    violations = [
        f"{reference_label}: {item}" for item in reference_data.violations
    ] + [f"{candidate_label}: {item}" for item in candidate_data.violations]

    return PathResult(
        label=label,
        start_version=start_version,
        schema_diff=_diff(
            reference_schema.text,
            candidate_schema.text,
            f"{reference_label}/{label}",
            f"{candidate_label}/{label}",
        ),
        data_diff=_diff(
            reference_data.text,
            candidate_data.text,
            f"{reference_label}/{label}",
            f"{candidate_label}/{label}",
        ),
        violations=violations,
        objects=reference_schema.objects,
        rows=reference_data.rows,
        notes=notes,
    )


def run_cpp(exe: Path, tag: str, db_path: Path, timeout: float) -> RunResult:
    """C++ 러너를 계약대로 호출한다."""
    env = os.environ.copy()
    env[PARITY_DB_ENV] = str(db_path)
    completed = subprocess.run(
        [str(exe), tag],
        cwd=str(exe.parent),
        env=env,
        capture_output=True,
        timeout=timeout,
    )
    return RunResult(
        returncode=completed.returncode,
        stdout=reference.decode_process_output(completed.stdout),
        stderr=reference.decode_process_output(completed.stderr),
    )


def _copy_database(source: Path, target: Path) -> None:
    """fixture 를 한 벌 복사한다. 사이드카가 있으면 함께 옮긴다."""
    shutil.copy2(source, target)
    for sidecar in ("-wal", "-shm"):
        extra = Path(str(source) + sidecar)
        if extra.exists():
            shutil.copy2(extra, Path(str(target) + sidecar))


def _preflight(exe: Path, timeout: float) -> int:
    """실행 파일이 있고 두 태그를 아는지 본다. 0 이면 진행 가능."""
    if not exe.is_file():
        print(
            f"오류: 시험 실행 파일이 없다 - {display_path(exe)}\n"
            "      ReleaseMD 산출물이 필요하다. 이 게이트는 빌드를 수행하지 않는다"
            "(공유 출력 디렉터리를 건드리지 않기 위한 의도된 제약이다).",
            file=sys.stderr,
        )
        return 2

    stat = exe.stat()
    print(
        f"C++ 러너: {display_path(exe)} "
        f"({stat.st_size}바이트, "
        f"{time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(stat.st_mtime))})",
        flush=True,
    )

    for tag in (PARITY_EMIT_TAG, PARITY_UPGRADE_TAG):
        try:
            listing = reference.count_matching_tests(exe, tag, timeout)
        except subprocess.TimeoutExpired:
            print(f"오류: 태그 조회가 {timeout:.0f}초 안에 끝나지 않았다 - {tag}", file=sys.stderr)
            return 2
        except OSError as exc:
            print(f"오류: 태그 조회 실패 - {tag}: {exc}", file=sys.stderr)
            return 2
        if listing.matched != 1:
            print(
                f"오류: 실행 파일이 {tag} 태그를 모른다(일치 {listing.matched}건, "
                f"기대 1건).\n"
                "      낡은 실행 파일이거나 아직 그 시험이 없다. 태그를 모르는 "
                "실행 파일은 이 태그를 주면 종료 코드 0 과 함께 아무것도 하지 "
                "않으므로, 종료 코드만으로는 통과와 구별되지 않는다. 이 게이트는 "
                "빌드하지 않는다.",
                file=sys.stderr,
            )
            return 2
        print(f"  태그 확인: {tag} 1건")
    return 0


def run_check(exe: Path, timeout: float = RUN_TIMEOUT_SECONDS) -> int:
    """경로 A 와 경로 B(N=1..8)를 전건 대조한다."""
    code = _preflight(exe, timeout)
    if code != 0:
        return code

    try:
        entries = reference.load_registry()
    except Exception as exc:
        print(f"오류: 파이썬 원본 적재 실패 - {exc}", file=sys.stderr)
        return 2
    latest = reference.latest_version(entries)
    print(f"  등록 마이그레이션 {len(entries)}건, 최신 버전 {latest}")

    results: list[PathResult] = []

    with tempfile.TemporaryDirectory(prefix="ladder_parity_") as work:
        work_dir = Path(work)

        # 경로 A - 빈 데이터베이스에서 양쪽이 각자 만든다.
        python_db = work_dir / "pathA_python.db"
        cpp_db = work_dir / "pathA_cpp.db"
        try:
            reference.build_database(python_db, entries, PYTHON_APPLIED_AT_US)
        except (sqlite3.Error, RuntimeError) as exc:
            print(f"오류: 파이썬 러너 실행 실패(경로 A) - {exc}", file=sys.stderr)
            return 2

        try:
            emitted = run_cpp(exe, PARITY_EMIT_TAG, cpp_db, timeout)
        except subprocess.TimeoutExpired:
            print(f"오류: C++ 러너가 {timeout:.0f}초 안에 끝나지 않았다(경로 A).", file=sys.stderr)
            return 2
        except OSError as exc:
            print(f"오류: C++ 러너 실행 실패(경로 A) - {exc}", file=sys.stderr)
            return 2
        if emitted.returncode != 0 or not cpp_db.is_file():
            print(
                f"오류: {PARITY_EMIT_TAG} 종료 코드 {emitted.returncode}, "
                f"데이터베이스 생성 {'됨' if cpp_db.is_file() else '안 됨'}.\n"
                f"    stdout:\n{tail(emitted.stdout)}\n"
                f"    stderr:\n{tail(emitted.stderr)}",
                file=sys.stderr,
            )
            return 2

        results.append(
            compare_databases("경로A(빈 DB -> v9)", 0, python_db, cpp_db, latest)
        )

        # 경로 B - 버전 N fixture 를 두 벌 복사해 각자 v9 까지 올린다.
        fixture_dir = work_dir / "fixtures"
        for version in fixtures.FIXTURE_VERSIONS:
            try:
                fixture = fixtures.build_fixture(
                    fixture_dir / f"v{version:04d}.db", version, entries
                )
            except (sqlite3.Error, RuntimeError, OSError) as exc:
                print(f"오류: fixture 생성 실패(v{version:04d}) - {exc}", file=sys.stderr)
                return 2
            actual = reference.database_version(fixture)
            if actual != version:
                print(
                    f"오류: fixture 버전이 {actual} 이다(기대 {version}). "
                    "fixture 도구가 어긋났다.",
                    file=sys.stderr,
                )
                return 2

            python_side = work_dir / f"pathB{version}_python.db"
            cpp_side = work_dir / f"pathB{version}_cpp.db"
            _copy_database(fixture, python_side)
            _copy_database(fixture, cpp_side)

            try:
                start = reference.upgrade_database(python_side, entries, PYTHON_APPLIED_AT_US)
            except (sqlite3.Error, RuntimeError) as exc:
                print(
                    f"오류: 파이썬 러너 실행 실패(경로 B v{version:04d}) - {exc}",
                    file=sys.stderr,
                )
                return 2
            if start != version:
                print(
                    f"오류: 파이썬 러너가 읽은 시작 버전이 {start} 다"
                    f"(기대 {version}).",
                    file=sys.stderr,
                )
                return 2

            try:
                upgraded = run_cpp(exe, PARITY_UPGRADE_TAG, cpp_side, timeout)
            except subprocess.TimeoutExpired:
                print(
                    f"오류: C++ 러너가 {timeout:.0f}초 안에 끝나지 않았다"
                    f"(경로 B v{version:04d}).",
                    file=sys.stderr,
                )
                return 2
            except OSError as exc:
                print(
                    f"오류: C++ 러너 실행 실패(경로 B v{version:04d}) - {exc}",
                    file=sys.stderr,
                )
                return 2
            if upgraded.returncode != 0:
                print(
                    f"오류: {PARITY_UPGRADE_TAG} 종료 코드 {upgraded.returncode} "
                    f"(경로 B v{version:04d}, 기대 0).\n"
                    f"    stdout:\n{tail(upgraded.stdout)}\n"
                    f"    stderr:\n{tail(upgraded.stderr)}",
                    file=sys.stderr,
                )
                return 2

            results.append(
                compare_databases(
                    f"경로B(v{version} -> v9)", version, python_side, cpp_side, latest
                )
            )

        return _report(results)


def _report(results: Sequence[PathResult]) -> int:
    """경로별 판정을 찍고 종료 코드를 돌려준다."""
    for result in results:
        status = "FAIL" if result.failed else "PASS"
        print(
            f"  {status}  {result.label}: 스키마 {result.objects}객체 / "
            f"행 {result.rows}건 / diff 스키마 {len(result.schema_diff)}줄 "
            f"데이터 {len(result.data_diff)}줄"
        )

    failed = [result for result in results if result.failed]
    if not failed:
        print(
            f"사다리 동등: 경로 {len(results)}건 전건 일치"
            "(스키마·행 데이터 diff 0줄)"
        )
        return 0

    print("", file=sys.stderr)
    for result in failed:
        print(f"=== {result.label} (대조 구간 v{result.start_version} -> 최신)", file=sys.stderr)
        for note in result.notes:
            print(f"  환경/불변식: {note}", file=sys.stderr)
        for item in result.violations:
            print(f"  정규화 검증 위반: {item}", file=sys.stderr)
        for name, diff in (("스키마", result.schema_diff), ("데이터", result.data_diff)):
            if not diff:
                continue
            print(f"  --- {name} diff {len(diff)}줄", file=sys.stderr)
            for line in diff[:DIFF_LINE_LIMIT]:
                print(f"  {line}", file=sys.stderr)
            if len(diff) > DIFF_LINE_LIMIT:
                print(
                    f"  ... (앞 {DIFF_LINE_LIMIT}줄만 인용, 총 {len(diff)}줄)",
                    file=sys.stderr,
                )
    print(f"사다리 발산: 경로 {len(failed)}/{len(results)}건 실패", file=sys.stderr)
    return 1


def _load_perturbations(path: Path) -> Sequence[object]:
    """교란 fixture 를 경로로 적재한다."""
    spec = importlib.util.spec_from_file_location("ladder_parity_perturbations", path)
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


def _ladder_paths(
    work_dir: Path,
    entries: Sequence[MigrationEntry],
    candidate: Callable[[Path], None],
    tag: str,
) -> list[PathResult]:
    """경로 A + 경로 B 전건을, C++ 자리에 임의의 파이썬 러너를 세워 돌린다."""
    latest = reference.latest_version(entries)
    results: list[PathResult] = []

    left = work_dir / f"{tag}_A_ref.db"
    right = work_dir / f"{tag}_A_cand.db"
    reference.build_database(left, entries, PYTHON_APPLIED_AT_US)
    candidate(right)
    results.append(
        compare_databases(
            "경로A(빈 DB -> v9)", 0, left, right, latest, "reference", "candidate"
        )
    )

    for version in fixtures.FIXTURE_VERSIONS:
        fixture = fixtures.build_fixture(
            work_dir / f"{tag}_fixture_v{version:04d}.db", version, entries
        )
        left = work_dir / f"{tag}_B{version}_ref.db"
        right = work_dir / f"{tag}_B{version}_cand.db"
        _copy_database(fixture, left)
        _copy_database(fixture, right)
        reference.upgrade_database(left, entries, PYTHON_APPLIED_AT_US)
        candidate(right)
        results.append(
            compare_databases(
                f"경로B(v{version} -> v9)",
                version,
                left,
                right,
                latest,
                "reference",
                "candidate",
            )
        )
    return results


def run_self_test() -> int:
    """C++ 빌드 없이 게이트 자신을 양방향 검증한다.

    C++ 러너 자리에 **일부러 망가뜨린 파이썬 러너**를 세운다. 통과해야 할 것이
    통과하고 잡아야 할 것이 잡히는지를 실제 판정 경로 그대로 본다.
    """
    fixture_path = Path(__file__).resolve().parent / "fixtures" / "ladder_parity" / "perturbations.py"
    if not fixture_path.is_file():
        print(f"오류: fixture 없음 - {display_path(fixture_path)}", file=sys.stderr)
        return 2

    try:
        entries = reference.load_registry()
        perturbations = _load_perturbations(fixture_path)
    except Exception as exc:
        print(f"오류: 적재 실패 - {exc}", file=sys.stderr)
        return 2

    failures = 0
    with tempfile.TemporaryDirectory(prefix="ladder_parity_selftest_") as work:
        work_dir = Path(work)

        print("[방향 1] known-good 수용 - 같은 러너를 다른 applied_at_us 로 두 번")

        def good(target: Path) -> None:
            """기준과 같은 러너. 시각은 다르고 window_id 도 따로 만들어진다."""
            if target.exists():
                reference.upgrade_database(target, entries, PYTHON_APPLIED_AT_US + 999_999)
            else:
                reference.build_database(target, entries, PYTHON_APPLIED_AT_US + 999_999)

        good_results = _ladder_paths(work_dir, entries, good, "good")
        good_failed = [result for result in good_results if result.failed]
        if good_failed:
            print(f"  FAIL  같은 러너인데 {len(good_failed)}경로에서 발산 - 오탐")
            for result in good_failed:
                print(
                    f"        {result.label}: 스키마 {len(result.schema_diff)}줄 "
                    f"데이터 {len(result.data_diff)}줄 "
                    f"위반 {result.violations} 비고 {result.notes}"
                )
                for line in (result.schema_diff + result.data_diff)[:12]:
                    print(f"          {line}")
            failures += 1
        else:
            print(
                f"  PASS  경로 {len(good_results)}건 전건 diff 0줄 "
                "(applied_at_us·window_id 정규화가 실제로 동작한다)"
            )

        print(f"[방향 2] known-bad 거부 - 교란 {len(perturbations)}종 x 경로 9건")
        for perturbation in perturbations:
            name = getattr(perturbation, "name", "?")
            describe = getattr(perturbation, "describe", "")
            expect_detected = bool(getattr(perturbation, "detected", True))
            apply = getattr(perturbation, "apply", None)
            if apply is None:
                print(f"  FAIL  {name}: apply 가 없다")
                failures += 1
                continue

            try:
                mutated = tuple(apply(entries))
            except Exception as exc:
                print(f"  FAIL  {name}: 교란 함수가 예외를 냈다 - {exc}")
                failures += 1
                continue
            if mutated == tuple(entries):
                print(
                    f"  FAIL  {name}: 교란이 아무것도 바꾸지 않았다"
                    "(원본이 바뀌어 무효가 된 fixture 다)"
                )
                failures += 1
                continue

            def candidate(target: Path, mutated: Sequence[MigrationEntry] = mutated) -> None:
                if target.exists():
                    reference.upgrade_database(target, mutated, PYTHON_APPLIED_AT_US)
                else:
                    reference.build_database(target, mutated, PYTHON_APPLIED_AT_US)

            try:
                results = _ladder_paths(work_dir, entries, candidate, name)
            except Exception as exc:
                # 교란이 러너를 깨뜨려 예외가 나는 것도 "잡혔다"의 한 형태다.
                print(f"  PASS  {name}: 러너가 예외로 멈춘다 - {exc}")
                continue

            detected = [result.label for result in results if result.failed]
            if expect_detected and detected:
                print(
                    f"  PASS  {name}: {len(detected)}/{len(results)}경로에서 거부 "
                    f"({detected[0]} 외) - {describe}"
                )
            elif expect_detected and not detected:
                print(f"  FAIL  {name}: 미탐 - {describe}")
                failures += 1
            elif not expect_detected and not detected:
                print(f"  PASS  {name}: 설계상 비탐지 확인(전 경로 diff 0줄) - {describe}")
            else:
                print(
                    f"  FAIL  {name}: 비탐지 대조군인데 "
                    f"{len(detected)}경로에서 잡혔다({detected}). 게이트의 사거리가 "
                    "바뀌었으니 문서를 갱신해야 한다."
                )
                failures += 1

        print("[방향 3] 정규화 검증 - 망가진 비결정 값이 자리표시자 뒤에 숨는가")
        broken_left = work_dir / "broken_left.db"
        broken_right = work_dir / "broken_right.db"
        for target in (broken_left, broken_right):
            reference.build_database(target, entries, PYTHON_APPLIED_AT_US)
            connection = reference.open_connection(target)
            try:
                # 양쪽 **똑같이** 망가뜨린다. diff 로는 절대 드러나지 않는다.
                connection.execute("UPDATE schema_version SET applied_at_us = 0 WHERE id = 1")
            finally:
                connection.close()
        broken = compare_databases(
            "정규화 검증", 0, broken_left, broken_right, reference.latest_version(entries),
            "left", "right",
        )
        if broken.violations and not broken.schema_diff and not broken.data_diff:
            print(
                f"  PASS  diff 는 0줄인데 위반 {len(broken.violations)}건으로 실패한다 "
                f"({broken.violations[0]})"
            )
        else:
            print(
                f"  FAIL  위반 {len(broken.violations)}건 / 스키마 "
                f"{len(broken.schema_diff)}줄 / 데이터 {len(broken.data_diff)}줄 "
                "- 자리표시자 뒤에 숨는 결함을 잡지 못한다"
            )
            failures += 1

        print("[방향 4] 빈 데이터베이스 - 통과가 아니라 환경 오류인가")
        empty_left = work_dir / "empty_left.db"
        empty_right = work_dir / "empty_right.db"
        for target in (empty_left, empty_right):
            reference.open_connection(target).close()
        empty = compare_databases(
            "빈 DB", 0, empty_left, empty_right, reference.latest_version(entries),
            "left", "right",
        )
        if empty.notes and not empty.schema_diff and not empty.data_diff:
            print(
                f"  PASS  빈 DB 끼리는 diff 0줄이지만 비고 {len(empty.notes)}건으로 "
                "실패한다"
            )
        else:
            print(
                f"  FAIL  빈 DB 가정이 깨졌다(비고 {len(empty.notes)}건, "
                f"diff {len(empty.schema_diff) + len(empty.data_diff)}줄)"
            )
            failures += 1

    if failures:
        print(f"자기시험 실패 {failures}건", file=sys.stderr)
        return 1
    print(
        f"자기시험 PASS: 동일 러너 9경로 수용, 교란 {len(perturbations)}종 기대대로 "
        "판정, 정규화 뒤 위반 검출, 빈 DB 는 환경 오류"
    )
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    reference.force_utf8_output()
    parser = argparse.ArgumentParser(
        description="사다리 동등성 대조(빈 DB -> v9, v1..v8 -> v9)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--exe",
        default=str(reference.DEFAULT_TEST_EXE),
        metavar="PATH",
        help="C++ 시험 실행 파일(기본: NoteEx/x64/ReleaseMD/NoteExTests.exe)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=RUN_TIMEOUT_SECONDS,
        metavar="SEC",
        help=f"C++ 러너 1회 제한 시간(기본 {RUN_TIMEOUT_SECONDS:.0f}초)",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="C++ 빌드 없이 자기검증을 수행한다(--exe 는 무시)",
    )
    args = parser.parse_args(argv)

    if args.self_test:
        return run_self_test()
    return run_check(Path(args.exe), args.timeout)


if __name__ == "__main__":
    sys.exit(main())
