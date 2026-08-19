#!/usr/bin/env python3
"""Validate the frozen Python-to-native test port register."""

from __future__ import annotations

import argparse
from dataclasses import dataclass, replace
from pathlib import Path
import re
import subprocess
import sys
from typing import Sequence


EXPECTED_BASELINE_COUNT = 630
BASELINE_PROVENANCE = "baseline-disposition:6ccb3c7"
POST_BASELINE_PROVENANCE = "post-baseline-reference:6ccb3c7..23d7134"
BASELINE_BEGIN = "<!-- T4A_BASELINE_REGISTER_BEGIN -->"
BASELINE_END = "<!-- T4A_BASELINE_REGISTER_END -->"
POST_BEGIN = "<!-- T4A_POST_BASELINE_BEGIN -->"
POST_END = "<!-- T4A_POST_BASELINE_END -->"
DISPOSITIONS = {"A직역", "B강등", "C재작성", "폐기"}
OWNERS = {f"W{number}" for number in range(1, 8)}
PLANNED_ID = re.compile(r"PLAN-(W[1-7])-\d{4}$")
TEST_CASE = re.compile(r'\bTEST_CASE\s*\(\s*"((?:\\.|[^"\\])*)"')
NODE_CLAIM = re.compile(
    r"(?<![\w./-])(tests/[\w./-]*\.py)((?:::[A-Za-z_]\w*)+)(\[[^\]\s]*\])?"
)
POST_BASELINE_IDS = (
    "tests/integration/test_backup.py::test_restore_keeps_original_database_when_first_move_aside_fails",
    "tests/integration/test_backup.py::test_interrupted_move_aside_keeps_original_in_reservation",
    "tests/integration/test_backup.py::test_interrupted_install_unpublishes_new_database",
    "tests/integration/test_backup.py::test_reservation_cleanup_failure_keeps_original_error",
    "tests/integration/test_backup.py::test_temporary_database_path_reserves_name_until_replaced",
)


class EnvironmentError(RuntimeError):
    """The gate cannot make a trustworthy decision."""


@dataclass(frozen=True)
class BaselineRow:
    node_id: str
    disposition: str
    native_id: str
    owner: str
    status: str
    provenance: str


@dataclass(frozen=True)
class PostBaselineRow:
    node_id: str
    native_id: str
    owner: str
    status: str
    provenance: str


def _plain(cell: str) -> str:
    value = cell.strip()
    if len(value) >= 2 and value.startswith("`") and value.endswith("`"):
        value = value[1:-1]
    return value


def _table_lines(text: str, begin: str, end: str) -> list[list[str]]:
    lines = text.replace("\r\n", "\n").replace("\r", "\n").splitlines()
    try:
        start = lines.index(begin)
        stop = lines.index(end, start + 1)
    except ValueError as error:
        raise EnvironmentError(f"register marker is missing: {begin} / {end}") from error
    body = [line for line in lines[start + 1 : stop] if line.strip()]
    if len(body) < 2:
        raise EnvironmentError(f"register table is empty: {begin}")
    rows: list[list[str]] = []
    for line in body[2:]:
        if not line.startswith("|") or not line.endswith("|"):
            raise EnvironmentError(f"malformed register row: {line}")
        rows.append([_plain(cell) for cell in line[1:-1].split("|")])
    return rows


def read_register(path: Path) -> tuple[list[BaselineRow], list[PostBaselineRow]]:
    try:
        text = path.read_text(encoding="utf-8-sig")
    except OSError as error:
        raise EnvironmentError(f"cannot read register: {error}") from error
    baseline_cells = _table_lines(text, BASELINE_BEGIN, BASELINE_END)
    post_cells = _table_lines(text, POST_BEGIN, POST_END)
    if any(len(cells) != 6 for cells in baseline_cells):
        raise EnvironmentError("baseline register must have exactly 6 columns")
    if any(len(cells) != 5 for cells in post_cells):
        raise EnvironmentError("post-baseline register must have exactly 5 columns")
    return (
        [BaselineRow(*cells) for cells in baseline_cells],
        [PostBaselineRow(*cells) for cells in post_cells],
    )


def read_manifest(path: Path) -> list[str]:
    try:
        nodes = path.read_text(encoding="utf-8-sig").splitlines()
    except OSError as error:
        raise EnvironmentError(f"cannot read manifest: {error}") from error
    if len(nodes) != EXPECTED_BASELINE_COUNT:
        raise EnvironmentError(
            f"manifest count is {len(nodes)}, expected {EXPECTED_BASELINE_COUNT}"
        )
    if len(set(nodes)) != len(nodes):
        raise EnvironmentError("manifest itself contains duplicate node IDs")
    return nodes


def _source_test_names(native_root: Path) -> set[str]:
    names: set[str] = set()
    for path in sorted(native_root.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in {".cpp", ".cc", ".cxx"}:
            continue
        try:
            text = path.read_text(encoding="utf-8-sig")
        except OSError as error:
            raise EnvironmentError(f"cannot read native source {path}: {error}") from error
        for match in TEST_CASE.finditer(text):
            name = match.group(1)
            if name in names:
                raise EnvironmentError(f"duplicate TEST_CASE name in source: {name}")
            names.add(name)
    if not names:
        raise EnvironmentError(f"no TEST_CASE names found below {native_root}")
    return names


def _listed_test_names(native_exe: Path) -> set[str]:
    try:
        result = subprocess.run(
            [str(native_exe.resolve()), "--list-tests"],
            cwd=native_exe.resolve().parents[2],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except OSError as error:
        raise EnvironmentError(f"cannot execute native registry: {error}") from error
    if result.returncode != 0:
        detail = result.stderr.decode("cp949", errors="replace").strip()
        raise EnvironmentError(f"native registry rc={result.returncode}: {detail}")
    try:
        decoded = result.stdout.decode("cp949")
    except UnicodeDecodeError as error:
        raise EnvironmentError(f"native registry is not valid CP949: {error}") from error
    names = {
        line[2:]
        for line in decoded.replace("\r\n", "\n").replace("\r", "\n").splitlines()
        if line.startswith("  ") and not line.startswith("    ") and line[2:].strip()
    }
    if not names:
        raise EnvironmentError("native registry returned zero test IDs")
    return names


def check_rows(
    manifest: Sequence[str],
    baseline_rows: Sequence[BaselineRow],
    post_rows: Sequence[PostBaselineRow],
    source_ids: set[str],
    listed_ids: set[str],
) -> list[str]:
    problems: list[str] = []
    manifest_set = set(manifest)
    row_nodes = [row.node_id for row in baseline_rows]

    if len(baseline_rows) != EXPECTED_BASELINE_COUNT:
        problems.append(
            f"본표 행 수 위반: {len(baseline_rows)} != {EXPECTED_BASELINE_COUNT}"
        )
    missing = [node for node in manifest if node not in row_nodes]
    if missing:
        problems.append(f"누락 baseline node ID {len(missing)}건: {missing[0]}")
    duplicates = sorted({node for node in row_nodes if row_nodes.count(node) > 1})
    if duplicates:
        problems.append(f"중복 baseline node ID {len(duplicates)}건: {duplicates[0]}")
    outside = [node for node in row_nodes if node not in manifest_set]
    if outside:
        problems.append(f"manifest 밖 node ID {len(outside)}건: {outside[0]}")

    planned_ids: set[str] = set()
    for row in baseline_rows:
        if row.disposition not in DISPOSITIONS:
            problems.append(f"허용되지 않은 처분: {row.node_id} -> {row.disposition!r}")
        if row.disposition == "폐기":
            problems.append(f"미승인 폐기: {row.node_id}")
        if row.owner not in OWNERS:
            problems.append(f"허용되지 않은 owner wave: {row.node_id} -> {row.owner!r}")
        if not row.provenance:
            problems.append(f"필수 provenance 공란: {row.node_id}")
        elif row.provenance != BASELINE_PROVENANCE:
            problems.append(f"baseline provenance 오분류: {row.node_id} -> {row.provenance!r}")
        if row.status == "planned":
            matched = PLANNED_ID.fullmatch(row.native_id)
            if matched is None or matched.group(1) != row.owner:
                problems.append(f"planned 예약 ID 위반: {row.node_id} -> {row.native_id!r}")
            elif row.native_id in planned_ids:
                problems.append(f"planned 예약 ID 중복: {row.native_id}")
            else:
                planned_ids.add(row.native_id)
        elif row.status == "implemented":
            if row.native_id not in source_ids or row.native_id not in listed_ids:
                problems.append(f"존재하지 않는 신규 ID: {row.node_id} -> {row.native_id!r}")
        else:
            problems.append(f"허용되지 않은 상태: {row.node_id} -> {row.status!r}")

    expected_post = set(POST_BASELINE_IDS)
    actual_post = [row.node_id for row in post_rows]
    if set(actual_post) != expected_post or len(actual_post) != len(expected_post):
        problems.append("post-baseline 5건 목록 불일치")
    for row in post_rows:
        if row.node_id in manifest_set:
            problems.append(f"post-baseline ID가 baseline 본표와 겹침: {row.node_id}")
        if row.owner not in OWNERS:
            problems.append(f"post-baseline owner wave 위반: {row.node_id} -> {row.owner!r}")
        if not row.provenance:
            problems.append(f"필수 provenance 공란: {row.node_id}")
        elif row.provenance != POST_BASELINE_PROVENANCE:
            problems.append(f"post-baseline의 baseline 오분류: {row.node_id}")
        if row.status == "referenced":
            if row.native_id not in source_ids or row.native_id not in listed_ids:
                problems.append(f"존재하지 않는 신규 ID: {row.node_id} -> {row.native_id!r}")
        elif row.status == "unmapped":
            if row.native_id != "-":
                problems.append(f"unmapped post-baseline ID는 '-'여야 함: {row.node_id}")
        else:
            problems.append(f"post-baseline 상태 위반: {row.node_id} -> {row.status!r}")
    return problems


def _synthetic_good() -> tuple[list[str], list[BaselineRow], list[PostBaselineRow]]:
    manifest = [f"tests/unit/test_sample.py::test_case_{index:04d}" for index in range(630)]
    baseline = [
        BaselineRow(node, "C재작성", f"PLAN-W2-{index:04d}", "W2", "planned", BASELINE_PROVENANCE)
        for index, node in enumerate(manifest, 1)
    ]
    post = [
        PostBaselineRow(node, "-", "W1", "unmapped", POST_BASELINE_PROVENANCE)
        for node in POST_BASELINE_IDS
    ]
    return manifest, baseline, post


def run_self_test() -> int:
    manifest, good_rows, good_post = _synthetic_good()
    source_ids = {"native-present"}
    listed_ids = {"native-present"}
    if check_rows(manifest, good_rows, good_post, source_ids, listed_ids):
        print("FAIL known-good was rejected")
        return 1
    print("PASS known-good accepted (630 baseline + 5 post-baseline rows)")

    cases: list[tuple[str, list[BaselineRow], list[PostBaselineRow], str]] = []
    cases.append(("누락", good_rows[:-1], good_post, "누락 baseline"))
    duplicate = list(good_rows)
    duplicate[-1] = replace(duplicate[-1], node_id=duplicate[0].node_id)
    cases.append(("중복", duplicate, good_post, "중복 baseline"))
    nonexistent = list(good_rows)
    nonexistent[0] = replace(nonexistent[0], native_id="native-missing", status="implemented")
    cases.append(("존재하지 않는 신규 ID", nonexistent, good_post, "존재하지 않는 신규 ID"))
    discarded = list(good_rows)
    discarded[0] = replace(discarded[0], disposition="폐기")
    cases.append(("미승인 폐기", discarded, good_post, "미승인 폐기"))
    extra = list(good_rows) + [
        BaselineRow(
            "tests/unit/test_extra.py::test_extra",
            "C재작성",
            "PLAN-W2-9999",
            "W2",
            "planned",
            BASELINE_PROVENANCE,
        )
    ]
    cases.append(("본표 행 수", extra, good_post, "본표 행 수 위반"))
    outside = list(good_rows)
    outside[-1] = replace(outside[-1], node_id="tests/unit/test_extra.py::test_extra")
    cases.append(("manifest 밖 node ID", outside, good_post, "manifest 밖 node ID"))
    blank = list(good_rows)
    blank[0] = replace(blank[0], provenance="")
    cases.append(("필수 provenance 공란", blank, good_post, "필수 provenance 공란"))
    misclassified_post = list(good_post)
    misclassified_post[0] = replace(misclassified_post[0], provenance=BASELINE_PROVENANCE)
    cases.append(("post-baseline baseline 오분류", good_rows, misclassified_post, "post-baseline의 baseline 오분류"))

    failures = 0
    for name, rows, post, expected in cases:
        found = check_rows(manifest, rows, post, source_ids, listed_ids)
        if any(expected in problem for problem in found):
            print(f"PASS seeded known-bad rejected: {name}")
        else:
            print(f"FAIL seeded known-bad not rejected for expected reason: {name}: {found}")
            failures += 1
    print("self-test complete: 8 independent failure conditions rejected" if not failures else f"self-test failures: {failures}")
    return 1 if failures else 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--register", type=Path)
    parser.add_argument("--native-root", type=Path)
    parser.add_argument("--native-exe", type=Path)
    args = parser.parse_args(argv)
    if args.self_test:
        return run_self_test()
    required = (args.manifest, args.register, args.native_root, args.native_exe)
    if any(value is None for value in required):
        parser.error("--manifest, --register, --native-root, and --native-exe are required")
    try:
        manifest = read_manifest(args.manifest)
        baseline_rows, post_rows = read_register(args.register)
        source_ids = _source_test_names(args.native_root)
        listed_ids = _listed_test_names(args.native_exe)
        problems = check_rows(manifest, baseline_rows, post_rows, source_ids, listed_ids)
    except EnvironmentError as error:
        print(f"environment error: {error}", file=sys.stderr)
        return 2
    if problems:
        for problem in problems:
            print(f"violation: {problem}", file=sys.stderr)
        print(f"register rejected: {len(problems)} violation(s)", file=sys.stderr)
        return 1
    implemented = sum(row.status == "implemented" for row in baseline_rows)
    print(
        f"register accepted: baseline {len(baseline_rows)}, implemented {implemented}, "
        f"planned {len(baseline_rows) - implemented}, post-baseline {len(post_rows)}; "
        f"native source/list registry {len(source_ids)}/{len(listed_ids)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
