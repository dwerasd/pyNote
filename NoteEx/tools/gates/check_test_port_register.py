#!/usr/bin/env python3
"""Validate the frozen Python-to-native test port register."""

from __future__ import annotations

import argparse
import hashlib
import html
import re
import subprocess
import sys
from collections.abc import Sequence
from dataclasses import dataclass, replace
from pathlib import Path

EXPECTED_BASELINE_COUNT = 630
MANIFEST_SHA256 = "66815f3eeae8bc032a67acf6ce7be2f1e889b72833eb35cb415bc7ca7a3b2cc4"
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
NODE_CLAIM = re.compile(r"(?<![\w./-])(tests/[\w./-]*\.py)((?:::[A-Za-z_]\w*)+)(\[[^\]\s]*\])?")
POST_BASELINE_IDS = (
    "tests/integration/test_backup.py::test_restore_keeps_original_database_when_first_move_aside_fails",
    "tests/integration/test_backup.py::test_interrupted_move_aside_keeps_original_in_reservation",
    "tests/integration/test_backup.py::test_interrupted_install_unpublishes_new_database",
    "tests/integration/test_backup.py::test_reservation_cleanup_failure_keeps_original_error",
    "tests/integration/test_backup.py::test_temporary_database_path_reserves_name_until_replaced",
)


def _expand_ranges(*ranges: tuple[int, int]) -> frozenset[int]:
    return frozenset(index for first, last in ranges for index in range(first, last + 1))


# Design R3 §9 is the semantic oracle.  The 174 integration/unit cases are
# A-direct ports.  UI cases default to B core/state-machine demotions; only
# the exact irreducible focus, IME, event-propagation, DPI/geometry, painting,
# and real UI-wiring cases below remain C harness rewrites.  Indices are safe
# stable coordinates only because read_manifest() first checks the frozen raw
# manifest SHA-256 and check_rows() checks its exact order.
A_INDICES = _expand_ranges((1, 94), (551, 630))
C_INDICES = _expand_ranges(
    (95, 95),
    (104, 106),
    (111, 113),
    (116, 116),
    (118, 118),
    (120, 120),
    (122, 122),
    (128, 128),
    (132, 133),
    (135, 139),
    (149, 152),
    (155, 155),
    (164, 164),
    (167, 167),
    (170, 170),
    (175, 186),
    (191, 206),
    (208, 210),
    (220, 221),
    (226, 226),
    (228, 231),
    (234, 234),
    (244, 245),
    (251, 252),
    (262, 265),
    (272, 273),
    (279, 279),
    (284, 287),
    (289, 290),
    (293, 293),
    (298, 298),
    (302, 303),
    (309, 313),
    (315, 318),
    (348, 349),
    (352, 353),
    (367, 367),
    (369, 370),
    (377, 377),
    (385, 386),
    (389, 390),
    (455, 455),
    (459, 462),
    (466, 467),
    (479, 481),
    (507, 530),
    (534, 534),
    (542, 544),
    (546, 550),
)


DEFAULT_OWNER_BY_FILE = {
    "tests/integration/test_backup.py": "W1",
    "tests/integration/test_database.py": "W1",
    "tests/integration/test_history_service.py": "W7",
    "tests/integration/test_import.py": "W2",
    "tests/integration/test_legacy_split_graph.py": "W1",
    "tests/integration/test_purge_service.py": "W1",
    "tests/integration/test_repositories.py": "W1",
    "tests/integration/test_save_coordinator.py": "W6",
    "tests/integration/test_stabilization.py": "W6",
    "tests/integration/test_storage_concurrency.py": "W1",
    "tests/ui/test_app_smoke.py": "W3",
    "tests/ui/test_card_context_menu.py": "W4",
    "tests/ui/test_card_drag.py": "W4",
    "tests/ui/test_card_editor.py": "W5",
    "tests/ui/test_card_multi_selection.py": "W4",
    "tests/ui/test_card_stream.py": "W4",
    "tests/ui/test_card_wheel_browse.py": "W4",
    "tests/ui/test_document_navigator.py": "W7",
    "tests/ui/test_editor_file_drop.py": "W7",
    "tests/ui/test_editor_split_and_last_tab.py": "W3",
    "tests/ui/test_history_view.py": "W7",
    "tests/ui/test_immediate_paste_and_composer_draft.py": "W6",
    "tests/ui/test_main_window_integration.py": "W3",
    "tests/ui/test_multiwindow.py": "W3",
    "tests/ui/test_notepad_start.py": "W3",
    "tests/ui/test_single_editor_surface.py": "W6",
    "tests/ui/test_stabilization_performance.py": "W4",
    "tests/unit/test_card_service.py": "W2",
    "tests/unit/test_diffing.py": "W2",
    "tests/unit/test_draft_coordinator.py": "W6",
    "tests/unit/test_export.py": "W2",
    "tests/unit/test_paragraph_parser.py": "W2",
}


def _owner_overrides() -> dict[int, str]:
    result: dict[int, str] = {}

    def assign(owner: str, *ranges: tuple[int, int]) -> None:
        for index in _expand_ranges(*ranges):
            if index in result:
                raise RuntimeError(f"duplicate test owner override: {index}")
            result[index] = owner

    assign("W1", (102, 103))
    assign(
        "W2",
        (35, 52),
        (119, 119),
        (156, 163),
        (173, 174),
        (187, 190),
        (207, 207),
        (211, 219),
        (222, 225),
        (227, 227),
        (235, 235),
        (237, 240),
        (247, 250),
        (255, 261),
        (268, 271),
        (274, 276),
        (278, 278),
        (280, 283),
        (304, 306),
        (314, 314),
        (391, 392),
        (463, 465),
        (468, 478),
        (482, 482),
        (531, 533),
        (545, 545),
    )
    assign("W3", (164, 164), (166, 168), (170, 172), (243, 243))
    assign(
        "W5",
        (136, 139),
        (149, 150),
        (152, 152),
        (155, 155),
        (244, 245),
        (251, 251),
        (262, 265),
        (273, 273),
        (286, 287),
        (289, 289),
        (302, 303),
        (349, 349),
        (459, 462),
        (466, 467),
        (479, 481),
        (507, 530),
        (534, 534),
        (542, 544),
        (546, 549),
    )
    assign(
        "W6",
        (109, 110),
        (121, 127),
        (131, 131),
        (134, 134),
        (140, 148),
        (151, 151),
        (153, 154),
        (252, 252),
        (254, 254),
        (266, 267),
        (277, 277),
        (288, 288),
        (299, 301),
        (319, 348),
        (350, 351),
        (379, 384),
        (389, 389),
        (428, 453),
    )
    assign(
        "W7",
        (28, 34),
        (53, 64),
        (72, 72),
        (132, 133),
        (236, 236),
        (279, 279),
        (285, 285),
        (353, 368),
        (385, 388),
        (393, 393),
        (396, 422),
    )
    return result


OWNER_OVERRIDES = _owner_overrides()


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
    return html.unescape(value)


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
        raw = path.read_bytes()
    except OSError as error:
        raise EnvironmentError(f"cannot read manifest: {error}") from error
    digest = hashlib.sha256(raw).hexdigest()
    if digest != MANIFEST_SHA256:
        raise EnvironmentError(f"manifest SHA-256 is {digest}, expected {MANIFEST_SHA256}")
    try:
        nodes = raw.decode("utf-8-sig").splitlines()
    except UnicodeDecodeError as error:
        raise EnvironmentError(f"manifest is not valid UTF-8: {error}") from error
    if len(nodes) != EXPECTED_BASELINE_COUNT:
        raise EnvironmentError(
            f"manifest count is {len(nodes)}, expected {EXPECTED_BASELINE_COUNT}"
        )
    if len(set(nodes)) != len(nodes):
        raise EnvironmentError("manifest itself contains duplicate node IDs")
    return nodes


def expected_assignments(manifest: Sequence[str]) -> dict[str, tuple[str, str]]:
    """Return the frozen design §9 disposition and §11 implementation owner."""

    if len(manifest) != EXPECTED_BASELINE_COUNT:
        raise EnvironmentError(
            f"cannot assign {len(manifest)} rows; expected {EXPECTED_BASELINE_COUNT}"
        )
    result: dict[str, tuple[str, str]] = {}
    for index, node_id in enumerate(manifest, 1):
        source_file = node_id.split("::", 1)[0]
        owner = OWNER_OVERRIDES.get(index)
        if owner is None:
            try:
                owner = DEFAULT_OWNER_BY_FILE[source_file]
            except KeyError as error:
                raise EnvironmentError(
                    f"no semantic owner oracle for manifest row {index}: {source_file}"
                ) from error
        if index in A_INDICES:
            disposition = "A직역"
        elif index in C_INDICES:
            disposition = "C재작성"
        else:
            disposition = "B강등"
        if node_id in result:
            raise EnvironmentError(f"duplicate assignment node ID: {node_id}")
        result[node_id] = (disposition, owner)

    if len(A_INDICES) != 174:
        raise RuntimeError(f"A oracle count changed: {len(A_INDICES)}")
    if len(C_INDICES) != 151:
        raise RuntimeError(f"C oracle count changed: {len(C_INDICES)}")
    if A_INDICES & C_INDICES:
        raise RuntimeError("A/C semantic oracle overlap")
    if not C_INDICES <= frozenset(range(95, 551)):
        raise RuntimeError("C oracle escaped the 456 UI cases")
    return result


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
            # 기본 출력은 긴 이름을 80열에서 줄바꿈해 첫 줄만 이름처럼 보인다(2026-08-21 실측: 79자 초과
            # 케이스 9행이 "존재하지 않는 신규 ID" 오탐). --verbosity quiet 는 한 줄에 이름 하나를 줄바꿈
            # 없이 낸다.
            [str(native_exe.resolve()), "--list-tests", "--verbosity", "quiet"],
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
        line.strip()
        for line in decoded.replace("\r\n", "\n").replace("\r", "\n").splitlines()
        if line.strip()
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
    assignment_by_node = expected_assignments(manifest)
    row_nodes = [row.node_id for row in baseline_rows]

    if len(baseline_rows) != EXPECTED_BASELINE_COUNT:
        problems.append(f"본표 행 수 위반: {len(baseline_rows)} != {EXPECTED_BASELINE_COUNT}")
    missing = [node for node in manifest if node not in row_nodes]
    if missing:
        problems.append(f"누락 baseline node ID {len(missing)}건: {missing[0]}")
    duplicates = sorted({node for node in row_nodes if row_nodes.count(node) > 1})
    if duplicates:
        problems.append(f"중복 baseline node ID {len(duplicates)}건: {duplicates[0]}")
    outside = [node for node in row_nodes if node not in manifest_set]
    if outside:
        problems.append(f"manifest 밖 node ID {len(outside)}건: {outside[0]}")
    if len(row_nodes) == len(manifest) and row_nodes != list(manifest):
        problems.append("baseline 원수집 순서 위반")

    planned_ids: set[str] = set()
    for row in baseline_rows:
        if row.disposition not in DISPOSITIONS:
            problems.append(f"허용되지 않은 처분: {row.node_id} -> {row.disposition!r}")
        if row.disposition == "폐기":
            problems.append(f"미승인 폐기: {row.node_id}")
        if row.owner not in OWNERS:
            problems.append(f"허용되지 않은 owner wave: {row.node_id} -> {row.owner!r}")
        expected = assignment_by_node.get(row.node_id)
        if expected is not None:
            expected_disposition, expected_owner = expected
            if row.disposition != expected_disposition:
                problems.append(
                    "설계 §9 semantic 처분 불일치: "
                    f"{row.node_id} -> {row.disposition!r}, 기대 {expected_disposition!r}"
                )
            if row.owner != expected_owner:
                problems.append(
                    "설계 §11 semantic owner 불일치: "
                    f"{row.node_id} -> {row.owner!r}, 기대 {expected_owner!r}"
                )
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


def _synthetic_good(
    manifest: list[str],
) -> tuple[list[BaselineRow], list[PostBaselineRow]]:
    assignments = expected_assignments(manifest)
    counters = {owner: 0 for owner in OWNERS}
    baseline: list[BaselineRow] = []
    for node in manifest:
        disposition, owner = assignments[node]
        counters[owner] += 1
        baseline.append(
            BaselineRow(
                node,
                disposition,
                f"PLAN-{owner}-{counters[owner]:04d}",
                owner,
                "planned",
                BASELINE_PROVENANCE,
            )
        )
    post = [
        PostBaselineRow(node, "-", "W1", "unmapped", POST_BASELINE_PROVENANCE)
        for node in POST_BASELINE_IDS
    ]
    return baseline, post


def run_self_test() -> int:
    manifest_path = (
        Path(__file__).resolve().parent / "fixtures" / "pytest_baseline_6ccb3c7" / "node_ids.txt"
    )
    try:
        manifest = read_manifest(manifest_path)
        good_rows, good_post = _synthetic_good(manifest)
    except EnvironmentError as error:
        print(f"FAIL self-test fixture: {error}")
        return 1
    source_ids = {"native-present"}
    listed_ids = {"native-present"}
    if check_rows(manifest, good_rows, good_post, source_ids, listed_ids):
        print("FAIL known-good was rejected")
        return 1
    counts = {
        disposition: sum(row.disposition == disposition for row in good_rows)
        for disposition in ("A직역", "B강등", "C재작성")
    }
    print(
        "PASS known-good accepted "
        f"(630 baseline: A={counts['A직역']}, B={counts['B강등']}, "
        f"C={counts['C재작성']}; 5 post-baseline rows)"
    )

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
    cases.append(
        (
            "post-baseline baseline 오분류",
            good_rows,
            misclassified_post,
            "post-baseline의 baseline 오분류",
        )
    )
    reordered = list(good_rows)
    reordered[0], reordered[1] = reordered[1], reordered[0]
    cases.append(("baseline 원순서", reordered, good_post, "baseline 원수집 순서 위반"))
    wrong_disposition = list(good_rows)
    wrong_disposition[0] = replace(wrong_disposition[0], disposition="C재작성")
    cases.append(("semantic 처분", wrong_disposition, good_post, "semantic 처분 불일치"))
    wrong_owner = list(good_rows)
    wrong_owner[0] = replace(wrong_owner[0], owner="W2", native_id="PLAN-W2-9999")
    cases.append(("semantic owner", wrong_owner, good_post, "semantic owner 불일치"))

    failures = 0
    for name, rows, post, expected in cases:
        found = check_rows(manifest, rows, post, source_ids, listed_ids)
        if any(expected in problem for problem in found):
            print(f"PASS seeded known-bad rejected: {name}")
        else:
            print(f"FAIL seeded known-bad not rejected for expected reason: {name}: {found}")
            failures += 1
    print(
        "self-test complete: 11 independent failure conditions rejected"
        if not failures
        else f"self-test failures: {failures}"
    )
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
