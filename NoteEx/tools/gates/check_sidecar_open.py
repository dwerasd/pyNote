#!/usr/bin/env python3
"""기존 사용자 DB 세트 개방 게이트 (DB + `-wal` + `-shm`, v1~v9 전 구간).

지시서 T-R5 가 요구하는 개방 시험이다. 사다리 게이트는 데이터베이스 **한 파일**을
승급시키지만, 실제 사용자의 디스크에 있는 것은 세 파일이다 — 앱이 살아 있거나 강제
종료된 뒤에는 WAL 에 아직 본체로 넘어가지 않은 커밋이 남아 있고 SHM 이 그 인덱스를
들고 있다. 그 세트를 그대로 여는 것이 이 게이트의 대상이다.

**커밋됐지만 본체에 없는 행이 이 시험의 핵심이다.** 사이드카를 그냥 만들어 두는 것만
으로는 아무것도 증명하지 못한다 - WAL 이 비어 있으면 본체만 열어도 결과가 같기
때문이다. 그래서 `wal_autocheckpoint=0` 으로 자동 체크포인트를 끄고, 연결이 열려
있는 상태에서 세 파일을 복사한다. 복사본의 본체에는 그 행이 없고 WAL 에만 있다.
이식본이 WAL 을 무시하거나 사이드카를 버리면 그 행이 사라지고 이 게이트가 잡는다.

**격리 프로필만 쓴다.** 실사용 데이터베이스를 기동하지 않는다(W0 지시서 R2 T1).
입력은 `make_ladder_fixtures.py` 가 만든 버전별 fixture 이고, 세트는 임시 디렉터리에
복제해 거기서만 연다.

## 판정

각 버전 N 에 대해 세 가지를 본다.

1. C++ 러너가 그 세트를 열어 최신 버전까지 올린다(`[parity-upgrade]`).
2. 올린 결과가 `quick_check` 을 통과하고 `schema_version` 이 최신이다.
3. **WAL 에만 있던 행이 살아 있다.** 본체에 없고 사이드카에만 있던 커밋이 승급 뒤에도
   읽히는지가 무손실 개방의 실질이다.

## 무엇을 증명하지 않나

- **스키마·행 전량의 동등성.** 그건 사다리 게이트 소관이다. 이쪽은 세트 개방과 WAL
  잔여분 보존만 본다.
- **강제 종료 복구의 모든 형태.** 여기서 만드는 것은 정상 연결이 열려 있는 상태의
  복사본이지 프로세스가 죽은 순간의 스냅샷이 아니다. 후자는 W6 손실 프로브 소관이다.
- **SHM 의 내용.** 복사하되 해석하지 않는다. SQLite 가 필요하면 다시 만든다.

종료 코드:
  0  전 버전 개방·보존 성공
  1  개방 실패 또는 WAL 잔여분 소실
  2  사용법·환경 오류 — 실행 파일 없음, fixture 없음, 대상 0건
"""

from __future__ import annotations

import argparse
import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))

REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_EXE = REPO_ROOT / "NoteEx" / "x64" / "ReleaseMD" / "NoteExTests.exe"
DEFAULT_FIXTURES = Path(__file__).resolve().parent / "fixtures" / "ladder_parity"

# WAL 에만 남길 표식. 승급 뒤에도 읽혀야 한다.
MARKER_ID = "sidecar-probe-marker"


def _display(path: Path) -> str:
    return str(path).replace("\\", "/")


def build_live_set(source_db: Path, target_dir: Path) -> tuple[Path, bool]:
    """열린 연결 상태의 3종 세트를 target_dir 에 복제하고 (경로, WAL 비어있지 않음) 을 준다.

    자동 체크포인트를 끄고 커밋한 뒤 **연결을 닫지 않은 채** 복사한다. 닫으면 SQLite 가
    체크포인트하고 사이드카를 지워 버려 이 게이트가 시험하려는 상태가 사라진다.
    """
    target_dir.mkdir(parents=True, exist_ok=True)
    staging = target_dir / "staging.db"
    shutil.copy2(source_db, staging)

    connection = sqlite3.connect(staging, isolation_level=None)
    try:
        connection.execute("PRAGMA journal_mode = WAL")
        connection.execute("PRAGMA wal_autocheckpoint = 0")
        connection.execute("PRAGMA foreign_keys = ON")
        # 어느 버전에나 있는 표에 남긴다. documents 는 v0001 부터 존재한다.
        connection.execute("BEGIN IMMEDIATE")
        connection.execute(
            "INSERT INTO documents(id, title, created_at_us, updated_at_us)"
            " VALUES (?, ?, ?, ?)",
            (MARKER_ID, "sidecar probe", 1, 1),
        )
        connection.commit()

        wal = Path(str(staging) + "-wal")
        wal_has_content = wal.is_file() and wal.stat().st_size > 0

        # 연결이 살아 있는 동안 세 파일을 그대로 옮긴다.
        profile = target_dir / "profile.db"
        shutil.copy2(staging, profile)
        for suffix in ("-wal", "-shm"):
            side = Path(str(staging) + suffix)
            if side.is_file():
                shutil.copy2(side, Path(str(profile) + suffix))
        return profile, wal_has_content
    finally:
        connection.close()


def body_lacks_marker(profile: Path) -> bool:
    """복사된 본체만으로는 표식이 안 보이는지 확인한다(WAL 에만 있다는 증거)."""
    sidecars = [Path(str(profile) + suffix) for suffix in ("-wal", "-shm")]
    moved = []
    try:
        for side in sidecars:
            if side.is_file():
                hidden = Path(str(side) + ".hidden")
                side.rename(hidden)
                moved.append((hidden, side))
        connection = sqlite3.connect(profile)
        try:
            row = connection.execute(
                "SELECT COUNT(*) FROM documents WHERE id = ?", (MARKER_ID,)
            ).fetchone()
            return row is not None and row[0] == 0
        finally:
            connection.close()
    finally:
        for hidden, original in moved:
            hidden.rename(original)


def run_upgrade(exe: Path, profile: Path, timeout: float) -> None:
    environment = dict(os.environ)
    environment["NOTEEX_PARITY_DB"] = str(profile)
    completed = subprocess.run(
        [str(exe), "[parity-upgrade]"],
        cwd=str(exe.parent),
        env=environment,
        capture_output=True,
        timeout=timeout,
        check=False,
    )
    if completed.returncode != 0:
        tail = completed.stdout.decode("cp949", errors="replace").strip().splitlines()[-4:]
        raise RuntimeError(f"승급이 종료 코드 {completed.returncode}: {' / '.join(tail)}")


def verify_after(profile: Path, latest: int) -> list[str]:
    problems: list[str] = []
    connection = sqlite3.connect(profile)
    try:
        row = connection.execute("PRAGMA quick_check").fetchone()
        if row is None or str(row[0]).lower() != "ok":
            problems.append(f"quick_check 이 ok 가 아니다: {row[0] if row else '<없음>'}")

        row = connection.execute("SELECT version FROM schema_version WHERE id = 1").fetchone()
        if row is None or int(row[0]) != latest:
            problems.append(f"schema_version 이 {latest} 가 아니다: {row[0] if row else '<없음>'}")

        row = connection.execute(
            "SELECT COUNT(*) FROM documents WHERE id = ?", (MARKER_ID,)
        ).fetchone()
        if row is None or row[0] != 1:
            problems.append(
                "WAL 에만 있던 행이 승급 뒤 사라졌다 - 사이드카가 버려졌거나 무시됐다"
            )
    finally:
        connection.close()
    return problems


def run_self_test(fixtures: Path) -> int:
    """C++ 빌드 없이 이 게이트의 전제를 검증한다."""
    failures = 0
    sources = sorted(fixtures.glob("v*.db"))
    if not sources:
        print(f"  FAIL  fixture 가 없다 - {_display(fixtures)}", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory() as directory:
        profile, wal_has_content = build_live_set(sources[0], Path(directory) / "probe")

        # 방향 1: 세트가 실제로 세 파일인가.
        present = [s for s in ("", "-wal", "-shm") if Path(str(profile) + s).is_file()]
        if len(present) == 3:
            print("  PASS  3종 세트 복제(본체·WAL·SHM)")
        else:
            print(f"  FAIL  세트가 {len(present)} 파일뿐이다: {present}")
            failures += 1

        # 방향 2: WAL 이 비어 있지 않은가. 비면 이 게이트는 아무것도 증명하지 못한다.
        if wal_has_content:
            print("  PASS  WAL 에 내용이 있다")
        else:
            print("  FAIL  WAL 이 비었다 - 자동 체크포인트가 꺼지지 않았다")
            failures += 1

        # 방향 3: 본체만으로는 표식이 안 보이는가. 이것이 성립해야 보존 판정에 의미가 있다.
        if body_lacks_marker(profile):
            print("  PASS  본체 단독으로는 표식이 보이지 않는다(WAL 전용 커밋)")
        else:
            print("  FAIL  표식이 본체에 이미 있다 - WAL 잔여분을 시험하지 못한다")
            failures += 1

        # 방향 4: 사이드카를 버리면 표식이 사라지는가(음성 대조군).
        for suffix in ("-wal", "-shm"):
            side = Path(str(profile) + suffix)
            if side.is_file():
                side.unlink()
        connection = sqlite3.connect(profile)
        lost = connection.execute(
            "SELECT COUNT(*) FROM documents WHERE id = ?", (MARKER_ID,)
        ).fetchone()[0] == 0
        connection.close()
        if lost:
            print("  PASS  사이드카를 버리면 표식이 사라진다(음성 대조군)")
        else:
            print("  FAIL  사이드카 없이도 표식이 보인다 - 판정이 공허하다")
            failures += 1

    print(
        "자기시험 PASS: 3종 세트 복제, WAL 비어있지 않음, 본체 단독 미검출, 사이드카 폐기 시 소실"
        if failures == 0
        else f"자기시험 실패 {failures} 건"
    )
    return 1 if failures else 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--self-test", action="store_true", help="C++ 빌드 없이 전제를 검증한다")
    parser.add_argument("--exe", type=Path, default=DEFAULT_EXE, help="시험 실행 파일 경로")
    parser.add_argument("--fixtures", type=Path, default=DEFAULT_FIXTURES, help="버전별 fixture 디렉터리")
    parser.add_argument("--timeout", type=float, default=180.0, help="승급 제한 시간(초)")
    args = parser.parse_args(argv)

    if not args.fixtures.is_dir():
        print(f"오류: fixture 디렉터리가 없다 - {_display(args.fixtures)}", file=sys.stderr)
        print("      make_ladder_fixtures.py 를 먼저 돌린다.", file=sys.stderr)
        return 2

    if args.self_test:
        return run_self_test(args.fixtures)

    if not args.exe.is_file():
        print(f"오류: 시험 실행 파일이 없다 - {_display(args.exe)}", file=sys.stderr)
        print("      이 게이트는 빌드하지 않는다.", file=sys.stderr)
        return 2

    import migration_reference  # noqa: E402  (경로 삽입 후에만 적재 가능)

    latest = migration_reference.latest_schema_version()
    sources = sorted(args.fixtures.glob("v*.db"))
    if not sources:
        print(f"오류: 대상 0건 - {_display(args.fixtures)} 에 fixture 가 없다.", file=sys.stderr)
        return 2

    print(f"C++ 러너: {_display(args.exe)} / 최신 버전 {latest}")
    failures = 0
    with tempfile.TemporaryDirectory() as directory:
        for index, source in enumerate(sources):
            label = source.stem
            try:
                profile, wal_has_content = build_live_set(source, Path(directory) / f"p{index}")
                if not wal_has_content:
                    print(f"  FAIL  {label}: WAL 이 비어 개방 시험이 성립하지 않는다")
                    failures += 1
                    continue
                run_upgrade(args.exe, profile, args.timeout)
                problems = verify_after(profile, latest)
            except subprocess.TimeoutExpired:
                print(f"  FAIL  {label}: 승급이 {args.timeout} 초 안에 끝나지 않았다")
                failures += 1
                continue
            except (RuntimeError, sqlite3.Error) as error:
                print(f"  FAIL  {label}: {error}")
                failures += 1
                continue

            if problems:
                for line in problems:
                    print(f"  FAIL  {label}: {line}")
                failures += len(problems)
            else:
                print(f"  PASS  {label}: 3종 세트 개방 -> v{latest}, WAL 잔여분 보존")

    if failures:
        print(f"실패: {failures} 건", file=sys.stderr)
        return 1
    print(f"세트 개방 동등: fixture {len(sources)} 건 전건 개방·보존")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
