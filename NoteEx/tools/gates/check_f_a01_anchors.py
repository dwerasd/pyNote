#!/usr/bin/env python3
"""Validate the F_a01 errata and every baseline path:line anchor."""

from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


BASELINE_COMMIT = "6ccb3c7176f9ef40123a4567d80ddf61a38558f2"
ORIGINAL_SHA256 = "78ba556a0b878da06052084466bcdb8b6d7ccc77ddd8fa087ded65056352a3bd"
ERRATA_SHA256 = "b9d08d92f014d77e42556156a6f5c25b4c1f331c8b15e794cb3b1425ecf98499"
CANONICAL_BEGIN = "<!-- F_A01_CANONICAL_BEGIN -->"
CANONICAL_END = "<!-- F_A01_CANONICAL_END -->"
SECTION_SPECS = (
    ("[FEATURE INVENTORY]", "FI", 122),
    ("[TEXT AND INPUT]", "TI", 27),
    ("[RENDERING]", "RE", 19),
    ("[PERSISTENCE AND DURABILITY]", "PD", 40),
    ("[PROCESS AND LIFECYCLE]", "PL", 26),
    ("[NON-FEATURE CONTRACTS]", "NC", 40),
)
ANCHOR_RE = re.compile(
    r"(?P<path>(?:[A-Za-z0-9_.-]+/)*[A-Za-z0-9_.-]+\.(?:py|bat))"
    r":(?P<line>\d+)"
)


class GateError(ValueError):
    """The errata or an anchor violates the frozen T4b contract."""


@dataclass(frozen=True)
class CapabilityRow:
    row_id: str
    section: str
    text: str


@dataclass(frozen=True)
class Replacement:
    before: str
    after: str
    count: int


AUTHORIZED_REPLACEMENTS = (
    Replacement("`main.py:7`,", "`main.py:18`,", 1),
    Replacement("migrations/v1_initial.py", "migrations/v0001_initial.py", 9),
    Replacement(
        "`src/pynote/infrastructure/repositories.py:37`, `src/pynote/infrastructure/export.py:23`",
        "`src/pynote/infrastructure/repositories.py:30`, `src/pynote/infrastructure/export.py:23`",
        1,
    ),
    Replacement(
        "`src/pynote/infrastructure/repositories.py:37`, `src/pynote/infrastructure/backup.py:367`",
        "`src/pynote/infrastructure/repositories.py:30`, `src/pynote/infrastructure/backup.py:367`",
        1,
    ),
    Replacement(
        "`src/pynote/infrastructure/database.py:35` |\n| WAL mode",
        "`src/pynote/infrastructure/database.py:72` |\n| WAL mode",
        1,
    ),
    Replacement(
        "configuration. `src/pynote/infrastructure/database.py:35` |",
        "configuration. `src/pynote/infrastructure/database.py:68` |",
        1,
    ),
    Replacement(
        "`src/pynote/infrastructure/migrations/v3_storage_invariants.py:75`",
        "`src/pynote/infrastructure/migrations/v0003_storage_invariants.py:49`, "
        "`src/pynote/infrastructure/migrations/v0003_storage_invariants.py:65`",
        1,
    ),
    Replacement(
        "`src/pynote/infrastructure/migrations/v3_storage_invariants.py:127`",
        "`src/pynote/infrastructure/migrations/v0003_storage_invariants.py:81`, "
        "`src/pynote/infrastructure/migrations/v0003_storage_invariants.py:95`",
        1,
    ),
    Replacement(
        "`src/pynote/infrastructure/migrations/v3_storage_invariants.py:197`",
        "`src/pynote/infrastructure/migrations/v0003_storage_invariants.py:126`, "
        "`src/pynote/infrastructure/migrations/v0003_storage_invariants.py:134`, "
        "`src/pynote/infrastructure/migrations/v0003_storage_invariants.py:142`",
        1,
    ),
    Replacement(
        "Capture operation, cards, initial revisions, events, lineage, and counter updates succeed or fail together.",
        "Capture operation, cards, initial revisions, events, and counter updates succeed or fail together. "
        "At `6ccb3c7`, `card_lineage` remains schema/API/test-only because no production creation path writes it.",
        1,
    ),
    Replacement(
        "Instance identity is derived from a hash of the resolved, case-normalized data directory, "
        "so two databases may have different application instances.",
        "Instance identity is the hash of the resolved, case-normalized database parent directory: "
        "databases in the same parent share one application instance, while different parents produce different instances.",
        1,
    ),
)


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _extract_canonical(errata_text: str) -> str:
    if errata_text.count(CANONICAL_BEGIN) != 1 or errata_text.count(CANONICAL_END) != 1:
        raise GateError("errata must contain exactly one canonical begin/end marker")
    before_end, tail = errata_text.split(CANONICAL_END, 1)
    head, canonical = before_end.split(CANONICAL_BEGIN, 1)
    del head, tail
    return canonical.strip("\n")


def _authorized_transform(original: str) -> str:
    transformed = original.strip("\n")
    for replacement in AUTHORIZED_REPLACEMENTS:
        actual_count = transformed.count(replacement.before)
        if actual_count != replacement.count:
            raise GateError(
                "authorized source fragment count mismatch: "
                f"expected={replacement.count} actual={actual_count} fragment={replacement.before!r}"
            )
        transformed = transformed.replace(
            replacement.before, replacement.after, replacement.count
        )
    return transformed


def _source_rows(text: str) -> tuple[CapabilityRow, ...]:
    lines = text.splitlines()
    starts: list[tuple[int, str, str, int]] = []
    for heading, prefix, count in SECTION_SPECS:
        try:
            starts.append((lines.index(heading), heading, prefix, count))
        except ValueError as exc:
            raise GateError(f"missing section heading: {heading}") from exc

    rows: list[CapabilityRow] = []
    for start, heading, prefix, expected_count in starts:
        end = next(
            (
                line_index
                for line_index in range(start + 1, len(lines))
                if lines[line_index].startswith("[") and lines[line_index].endswith("]")
            ),
            len(lines),
        )
        section_rows: list[str] = []
        for line in lines[start + 1 : end]:
            if line.startswith("- "):
                section_rows.append(line)
            elif line.startswith("|"):
                if re.match(r"^\|[- ]+\|", line):
                    continue
                if line.startswith("| Feature |") or line.startswith("| Requirement |"):
                    continue
                section_rows.append(line)
        if len(section_rows) != expected_count:
            raise GateError(
                f"{heading} row count mismatch: expected={expected_count} actual={len(section_rows)}"
            )
        rows.extend(
            CapabilityRow(
                row_id=f"CAP-{prefix}-{row_index:03d}",
                section=heading,
                text=row,
            )
            for row_index, row in enumerate(section_rows, start=1)
        )
    return tuple(rows)


def validate_relationship(original_text: str, errata_text: str) -> str:
    canonical = _extract_canonical(errata_text)
    expected = _authorized_transform(original_text)

    original_rows = _source_rows(original_text)
    expected_rows = _source_rows(expected)
    canonical_rows = _source_rows(canonical)
    expected_total = sum(spec[2] for spec in SECTION_SPECS)
    if len(original_rows) != expected_total or len(canonical_rows) != expected_total:
        raise GateError("capability total changed")
    if [row.row_id for row in canonical_rows] != [row.row_id for row in original_rows]:
        raise GateError("capability row ID order changed")
    if canonical_rows != expected_rows:
        raise GateError("capability row content differs from the authorized transform")
    if canonical.splitlines() != expected.splitlines():
        raise GateError("non-row content differs from the authorized transform")
    return canonical


def _git_file(repo_root: Path, commit: str, path: str) -> list[str]:
    result = subprocess.run(
        ["git", "-C", str(repo_root), "show", f"{commit}:{path}"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise GateError(f"anchor path absent at {commit[:7]}: {path}: {detail}")
    try:
        return result.stdout.decode("utf-8").splitlines()
    except UnicodeDecodeError as exc:
        raise GateError(f"anchor source is not UTF-8: {path}") from exc


def validate_anchors(canonical: str, repo_root: Path) -> tuple[int, int]:
    matches = list(ANCHOR_RE.finditer(canonical))
    if not matches:
        raise GateError("no path:line anchors found")

    cache: dict[str, list[str]] = {}
    for match in matches:
        path = match.group("path")
        line_number = int(match.group("line"))
        if path not in cache:
            cache[path] = _git_file(repo_root, BASELINE_COMMIT, path)
        lines = cache[path]
        if line_number < 1 or line_number > len(lines):
            raise GateError(
                f"anchor line out of range: {path}:{line_number} file_lines={len(lines)}"
            )
        if not lines[line_number - 1].strip():
            raise GateError(f"anchor points to a blank line: {path}:{line_number}")
    return len(matches), len(cache)


def validate_all(original: Path, errata: Path, repo_root: Path) -> tuple[int, int, int]:
    if _sha256(original) != ORIGINAL_SHA256:
        raise GateError("original F_a01 SHA-256 mismatch")
    if _sha256(errata) != ERRATA_SHA256:
        raise GateError("errata SHA-256 mismatch")
    result = subprocess.run(
        ["git", "-C", str(repo_root), "cat-file", "-e", f"{BASELINE_COMMIT}^{{commit}}"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        raise GateError(f"baseline commit is unavailable: {BASELINE_COMMIT}")

    original_text = original.read_text(encoding="utf-8")
    errata_text = errata.read_text(encoding="utf-8")
    canonical = validate_relationship(original_text, errata_text)
    anchors, paths = validate_anchors(canonical, repo_root)
    return len(_source_rows(canonical)), anchors, paths


def _expect_rejected(label: str, operation: object) -> None:
    try:
        assert callable(operation)
        operation()
    except GateError as exc:
        print(f"PASS seeded-bad [{label}]: {exc}")
        return
    raise GateError(f"seeded-bad [{label}] was accepted")


def run_self_test(original: Path, errata: Path, repo_root: Path) -> int:
    rows, anchors, paths = validate_all(original, errata, repo_root)
    original_text = original.read_text(encoding="utf-8")
    errata_text = errata.read_text(encoding="utf-8")
    canonical = _extract_canonical(errata_text)
    first_row = next(line for line in canonical.splitlines() if line.startswith("| Direct/package"))

    _expect_rejected(
        "missing row",
        lambda: validate_relationship(original_text, errata_text.replace(first_row + "\n", "", 1)),
    )
    _expect_rejected(
        "duplicate row",
        lambda: validate_relationship(
            original_text, errata_text.replace(first_row, first_row + "\n" + first_row, 1)
        ),
    )
    _expect_rejected(
        "extra row",
        lambda: validate_relationship(
            original_text,
            errata_text.replace(
                first_row,
                first_row + "\n| Seeded extra | `main.py:18` | bad | bad |",
                1,
            ),
        ),
    )
    _expect_rejected(
        "unauthorized meaning change",
        lambda: validate_relationship(
            original_text,
            errata_text.replace("Starts the desktop application", "Starts a different application", 1),
        ),
    )
    _expect_rejected(
        "missing anchor path",
        lambda: validate_anchors(canonical.replace("main.py:18", "missing.py:18", 1), repo_root),
    )
    _expect_rejected(
        "out-of-range anchor",
        lambda: validate_anchors(canonical.replace("main.py:18", "main.py:999999", 1), repo_root),
    )
    _expect_rejected(
        "blank-line anchor",
        lambda: validate_anchors(canonical.replace("main.py:18", "main.py:6", 1), repo_root),
    )

    print(
        f"PASS known-good: rows={rows} anchors={anchors} unique_paths={paths} "
        f"baseline={BASELINE_COMMIT[:7]}"
    )
    print("PASS F_a01 anchor self-test: known-good accepted; 7 seeded conditions rejected")
    return 0


def main(argv: list[str] | None = None) -> int:
    repo_root = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--original",
        type=Path,
        default=repo_root / "scratchpad/orchestration/port-feasibility/out/F_a01.md",
    )
    parser.add_argument(
        "--errata",
        type=Path,
        default=repo_root / "docs/20260819_2123_Sol_max_WTL포팅_F_a01_errata-01.md",
    )
    parser.add_argument("--repo-root", type=Path, default=repo_root)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)

    try:
        if args.self_test:
            return run_self_test(args.original, args.errata, args.repo_root)
        rows, anchors, paths = validate_all(args.original, args.errata, args.repo_root)
    except (OSError, UnicodeError, GateError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    print(f"PASS F_a01 errata: rows={rows} anchors={anchors} unique_paths={paths}")
    print(f"baseline={BASELINE_COMMIT}")
    print(f"original_sha256={ORIGINAL_SHA256}")
    print(f"errata_sha256={ERRATA_SHA256}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
