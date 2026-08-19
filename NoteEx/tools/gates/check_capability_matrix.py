#!/usr/bin/env python3
"""Validate the frozen WTL capability matrix against the F_a01 errata.

The matrix is deliberately project-specific. T4b rebinds the T4a rows and
stable native test IDs to the corrected canonical source.
"""

from __future__ import annotations

import argparse
import hashlib
import html
import platform
import re
import sys
from collections.abc import Callable, Iterable
from dataclasses import dataclass, replace
from pathlib import Path

SOURCE_SHA256 = "b9d08d92f014d77e42556156a6f5c25b4c1f331c8b15e794cb3b1425ecf98499"
SECTION_SPECS = (
    ("[FEATURE INVENTORY]", "FI", 122),
    ("[TEXT AND INPUT]", "TI", 27),
    ("[RENDERING]", "RE", 19),
    ("[PERSISTENCE AND DURABILITY]", "PD", 40),
    ("[PROCESS AND LIFECYCLE]", "PL", 26),
    ("[NON-FEATURE CONTRACTS]", "NC", 40),
)
EXPECTED_TOTAL = sum(spec[2] for spec in SECTION_SPECS)
MATRIX_BEGIN = "<!-- CAPABILITY-MATRIX-BEGIN -->"
MATRIX_END = "<!-- CAPABILITY-MATRIX-END -->"
UNCERTAIN_BEGIN = "<!-- UNCERTAIN-LINKS-BEGIN -->"
UNCERTAIN_END = "<!-- UNCERTAIN-LINKS-END -->"
VALID_OWNER = re.compile(r"W[0-7]\Z")


@dataclass(frozen=True)
class SourceRow:
    row_id: str
    section: str
    text: str


@dataclass(frozen=True)
class MatrixRow:
    row_id: str
    section: str
    text: str
    owner: str
    artifact: str
    probe_id: str
    gate: str
    evidence: str


@dataclass(frozen=True)
class UncertainLink:
    link_id: str
    qt_row: str
    owner: str
    prerequisite: str
    probe_id: str
    predicate: str
    fallback: str


UNCERTAIN_LINKS = (
    UncertainLink(
        "UNC-001",
        "QDateTime",
        "W0",
        "T1 / D6",
        "T4A-UNC-001",
        "기존 Qt 날짜 형식 저장값 전건을 해석하고 동일 표시 fixture를 재현한다.",
        "W7에 Qt-token 호환 formatter를 두고 미지원 token은 명시적 이관 판정으로 보낸다.",
    ),
    UncertainLink(
        "UNC-002",
        "QSettings",
        "W0",
        "T1 / D8",
        "T4A-UNC-002",
        "HKCU 기존 경로·값 형식·geometry blob 전수와 typed device-key schema가 일대일 대응한다.",
        "일회성 registry 이관 어댑터 범위를 확대하고 해석 불가 원시값을 보존한다.",
    ),
    UncertainLink(
        "UNC-003",
        "QStandardPaths",
        "W0",
        "T1 / D8",
        "T4A-UNC-003",
        "실측한 기존 DB 절대 경로와 LocalAppData 기반 새 경로가 동일 데이터 위치를 가리킨다.",
        "기존 경로 탐색·명시 이관 gate를 W3 시작 차단 조건으로 둔다.",
    ),
    UncertainLink(
        "UNC-004",
        "QTimeZone",
        "W0",
        "T1 / D6",
        "T4A-UNC-004",
        "기존 저장 timezone ID 전건이 원본 ID를 보존한 bundled IANA tzdata 또는 system 동적 시간대로 해석되고 대표 시각 fixture가 일치한다.",
        "bundled IANA tzdata를 정본으로 사용하고 실패 ID는 원문을 보존해 차단하며 Windows key·local·system으로 조용히 치환하지 않는다.",
    ),
    UncertainLink(
        "UNC-005",
        "QInputMethodEvent",
        "W0",
        "T2 / P1",
        "T4A-UNC-005",
        "한국어 IME의 preedit/commit/cancel 순서와 조합 중 이탈 거부가 P1 trace와 일치한다.",
        "Rich Edit를 기각하고 DirectWrite+IMM32, 이어 TSF 후보 순으로 승급한다.",
    ),
    UncertainLink(
        "UNC-006",
        "QPalette",
        "W4",
        "W0 D9 및 T3 / P2",
        "T4A-UNC-006",
        "light/dark/high-contrast fixture에서 선택·hover·본문·placeholder 대비와 의미가 보존된다.",
        "system color 역할 매핑을 보정하고 owner-draw 색 정책을 W4에서 재동결한다.",
    ),
    UncertainLink(
        "UNC-007",
        "QTextDocument",
        "W0",
        "T2 / P1",
        "T4A-UNC-007",
        "LF·CRLF·non-BMP·삭제·formatting 알림의 position/removed/added trace가 UTF-16 계약과 일치한다.",
        "Rich Edit를 기각하고 다음 P1 후보로 승급한다.",
    ),
    UncertainLink(
        "UNC-008",
        "QApplication",
        "W0",
        "T1 / D9",
        "T4A-UNC-008",
        "PerMonitorV2 요구 API 전건과 P1·P2·smoke가 선택한 Windows 하한에서 실행된다.",
        "지원 하한을 재판정하며 더 낮은 하한이 필요하면 DPI 전략 변경을 사용자 결정으로 올린다.",
    ),
    UncertainLink(
        "UNC-009",
        "QPlainTextEdit",
        "W0",
        "T2 / P1",
        "T4A-UNC-009",
        "exact LF·UTF-16 cursor·IME·첫 입력 undo의 P1 7기준 전건이 같은 HWND에서 통과한다.",
        "DirectWrite+IMM32, 이어 TSF 후보 순으로 승급한다.",
    ),
    UncertainLink(
        "UNC-010",
        "QPlainTextEdit 내부 undo stack",
        "W0",
        "T2 / P1",
        "T4A-UNC-010",
        "첫 의미 입력이 정확히 한 undo 단위이며 load 시 clear와 replace-all 단일 undo가 재현된다.",
        "현재 P1 후보를 기각하고 다음 후보의 undo 모델을 시험한다.",
    ),
    UncertainLink(
        "UNC-011",
        "QFontMetrics",
        "W0",
        "T3 / P2",
        "T4A-UNC-011",
        "한국어·non-BMP advance와 line metric fixture가 허용 오차와 줄 수 계약 안에서 일치한다.",
        "D2DWrapp text-layout 보정층을 추가하고 보정 불가 시 렌더 후보를 재판정한다.",
    ),
    UncertainLink(
        "UNC-012",
        "QTextLayout",
        "W0",
        "T3 / P2",
        "T4A-UNC-012",
        "CR/LF·긴 무공백·surrogate pair wrap과 final-line ellipsis fixture가 전건 일치한다.",
        "D2DWrapp line-break 보정 후 재시험하고 실패하면 P2 렌더 후보를 재판정한다.",
    ),
)


class MatrixError(Exception):
    """A deterministic input or schema error."""


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_exact_text(path: Path) -> tuple[str, str]:
    data = path.read_bytes()
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise MatrixError(f"UTF-8 파일이 아니다: {path}: {exc}") from exc
    return text, sha256_bytes(data)


def source_rows(source_text: str) -> list[SourceRow]:
    lines = source_text.splitlines()
    heading_positions = {
        line: index
        for index, line in enumerate(lines)
        if line.startswith("[") and line.endswith("]")
    }
    rows: list[SourceRow] = []
    for heading, prefix, expected_count in SECTION_SPECS:
        if heading not in heading_positions:
            raise MatrixError(f"SOURCE_SECTION: 정본 절이 없다: {heading}")
        start = heading_positions[heading] + 1
        end = next(
            (
                i
                for i in range(start, len(lines))
                if lines[i].startswith("[") and lines[i].endswith("]")
            ),
            len(lines),
        )
        section_rows: list[str] = []
        for line in lines[start:end]:
            if line.startswith("- "):
                section_rows.append(line)
            elif line.startswith("|"):
                if re.match(r"^\|[- ]+\|", line):
                    continue
                if line.startswith("| Feature |") or line.startswith("| Requirement |"):
                    continue
                section_rows.append(line)
        if len(section_rows) != expected_count:
            raise MatrixError(
                f"SOURCE_COUNT: {heading} {len(section_rows)}행, 기대 {expected_count}행"
            )
        for number, text in enumerate(section_rows, 1):
            rows.append(SourceRow(f"CAP-{prefix}-{number:03d}", heading[1:-1], text))
    if len(rows) != EXPECTED_TOTAL:
        raise MatrixError(f"SOURCE_COUNT: 전체 {len(rows)}행, 기대 {EXPECTED_TOTAL}행")
    return rows


def _build_owner_map() -> dict[str, str]:
    """Freeze semantic ownership by stable row ID, never by incidental wording.

    The groups follow design §11: W1 storage, W2 pure core/domain, W3 shell and
    lifecycle, W4 card list/rendering, W5 editor control, W6 edit protection and
    recovery integration, and W7 peripheral document/search/history/import UI.
    """

    result: dict[str, str] = {}

    def add(prefix: str, owner: str, numbers: Iterable[int]) -> None:
        for number in numbers:
            row_id = f"CAP-{prefix}-{number:03d}"
            if row_id in result:
                raise RuntimeError(f"owner 중복 동결: {row_id}")
            result[row_id] = owner

    # FEATURE INVENTORY: classify the named feature itself, not words in its
    # description/capability columns.
    add("FI", "W1", (8,))
    add("FI", "W2", (118, 119))
    add("FI", "W3", (*range(1, 8), *range(9, 13), 15, 16, 26, 27, 30, 34, 35, 36, 38, 99, 120, 121))
    add("FI", "W4", (24, 29, *range(56, 67), 69, 70, 71))
    add("FI", "W5", (28, 80, 82, *range(90, 99)))
    add("FI", "W6", (13, 37, 72, 81, *range(83, 90), 100))
    add(
        "FI",
        "W7",
        (
            14,
            *range(17, 24),
            25,
            *range(31, 34),
            *range(39, 56),
            67,
            68,
            *range(73, 80),
            *range(101, 118),
            122,
        ),
    )

    # TEXT AND INPUT: core text coordinates/provenance, editor input, card-list
    # gestures, save integration, and peripheral file import remain distinct.
    add("TI", "W2", (1, 2, 11, 27))
    add("TI", "W4", (14, 15, 18, 22, 24, 25))
    add("TI", "W5", (*range(3, 9), 12, 13, 16, 17, 19, 20, 21, 23))
    add("TI", "W6", (9, 10))
    add("TI", "W7", (26,))

    # RENDERING is the W4 card renderer except the explicitly named history,
    # editor-formatting, and shell-DPI contracts.
    add("RE", "W3", (18,))
    add("RE", "W4", (*range(1, 16), 19))
    add("RE", "W5", (17,))
    add("RE", "W7", (16,))

    add("PD", "W1", range(1, 41))

    # PROCESS AND LIFECYCLE: shell ordering stays W3; protection gates are W6;
    # import/history worker behavior is owned with its W7 feature.
    add("PL", "W3", (*range(1, 10), 12, *range(21, 27)))
    add("PL", "W6", (10, 11, 20))
    add("PL", "W7", range(13, 20))

    # NON-FEATURE CONTRACTS are assigned to the wave that implements the
    # invariant, even where the sentence mentions a consumer owned elsewhere.
    add("NC", "W1", (24, 25, 38, 39))
    add("NC", "W2", (6, 7, 8, 9))
    add("NC", "W3", (32, 33, 35, 40))
    add("NC", "W4", (*range(10, 15), 36))
    add("NC", "W5", (1, 5))
    add("NC", "W6", (2, 3, 4, *range(15, 24), 34))
    add("NC", "W7", (*range(26, 32), 37))

    expected = {
        f"CAP-{prefix}-{number:03d}"
        for _, prefix, count in SECTION_SPECS
        for number in range(1, count + 1)
    }
    if set(result) != expected:
        missing = sorted(expected - set(result))
        extra = sorted(set(result) - expected)
        raise RuntimeError(f"owner 동결 coverage 불일치: missing={missing}, extra={extra}")
    return result


OWNER_BY_ID = _build_owner_map()


def owner_for(row: SourceRow) -> str:
    return OWNER_BY_ID[row.row_id]


ARTIFACTS = {
    "W0": "W0 실측·P1/P2 판정 증거",
    "W1": "NoteEx core/storage 구현과 저장 시험",
    "W2": "NoteEx core/domain 구현과 순수 C++ 시험",
    "W3": "NoteEx shell·수명주기 구현과 shell 시험",
    "W4": "NoteEx 카드 목록·DirectWrite 구현과 렌더 시험",
    "W5": "NoteEx 편집기 구현과 입력 시험",
    "W6": "NoteEx 보호·저장·복구 결선과 손실 시험",
    "W7": "NoteEx 주변 기능 UI 구현과 통합 시험",
}


def default_matrix(rows: Iterable[SourceRow]) -> list[MatrixRow]:
    matrix: list[MatrixRow] = []
    for row in rows:
        owner = owner_for(row)
        probe = row.row_id.replace("CAP-", "WTL-CAP-")
        matrix.append(
            MatrixRow(
                row.row_id,
                row.section,
                row.text,
                owner,
                ARTIFACTS[owner],
                probe,
                f'x64\\ReleaseMD\\NoteExTests.exe "{probe}"',
                "미실시 — T4c backfill 전",
            )
        )
    return matrix


def encode_cell(value: str) -> str:
    return html.escape(value, quote=False).replace("|", "&#124;")


def decode_cell(value: str) -> str:
    return html.unescape(value.strip())


def render_table_row(values: Iterable[str]) -> str:
    return "| " + " | ".join(encode_cell(value) for value in values) + " |"


def parse_table_row(line: str, expected_fields: int, label: str) -> list[str]:
    if not line.startswith("|") or not line.endswith("|"):
        raise MatrixError(f"SCHEMA: {label} 표 행 형식이 아니다")
    fields = [decode_cell(value) for value in line[1:-1].split("|")]
    if len(fields) != expected_fields:
        raise MatrixError(f"SCHEMA: {label} 열 {len(fields)}개, 기대 {expected_fields}개")
    return fields


def marked_lines(text: str, begin: str, end: str) -> list[str]:
    lines = text.splitlines()
    try:
        start = lines.index(begin) + 1
        finish = lines.index(end, start)
    except ValueError as exc:
        raise MatrixError(f"SCHEMA: marker가 없다: {begin} / {end}") from exc
    return lines[start:finish]


def parse_matrix(matrix_text: str) -> list[MatrixRow]:
    lines = marked_lines(matrix_text, MATRIX_BEGIN, MATRIX_END)
    data = [line for line in lines if line.startswith("|")]
    if len(data) < 2:
        raise MatrixError("SCHEMA: capability 표 header가 없다")
    result: list[MatrixRow] = []
    for index, line in enumerate(data[2:], 1):
        values = parse_table_row(line, 8, f"capability {index}")
        result.append(MatrixRow(*values))
    return result


def parse_uncertain(matrix_text: str) -> list[UncertainLink]:
    lines = marked_lines(matrix_text, UNCERTAIN_BEGIN, UNCERTAIN_END)
    data = [line for line in lines if line.startswith("|")]
    if len(data) < 2:
        raise MatrixError("SCHEMA: uncertain 표 header가 없다")
    result: list[UncertainLink] = []
    for index, line in enumerate(data[2:], 1):
        values = parse_table_row(line, 7, f"uncertain {index}")
        result.append(UncertainLink(*values))
    return result


def validate_rows(expected: list[SourceRow], actual: list[MatrixRow]) -> list[str]:
    problems: list[str] = []
    expected_by_id = {row.row_id: row for row in expected}
    ids = [row.row_id for row in actual]
    duplicates = sorted({row_id for row_id in ids if ids.count(row_id) > 1})
    for row_id in duplicates:
        problems.append(f"DUPLICATE: capability ID가 중복이다: {row_id}")
    actual_ids = set(ids)
    for row in expected:
        if row.row_id not in actual_ids:
            problems.append(f"MISSING: 정본 행이 없다: {row.row_id}")
    for row_id in sorted(actual_ids - set(expected_by_id)):
        problems.append(f"EXTRA: 정본에 없는 행이다: {row_id}")
    if not duplicates and actual_ids == set(expected_by_id):
        if ids != [row.row_id for row in expected]:
            problems.append("ORDER: 정본 행 원순서가 바뀌었다")
    probe_ids = [row.probe_id for row in actual if row.probe_id]
    duplicate_probes = sorted({probe_id for probe_id in probe_ids if probe_ids.count(probe_id) > 1})
    for probe_id in duplicate_probes:
        problems.append(f"PROBE_DUPLICATE: probe/시험 ID가 중복이다: {probe_id}")
    seen: set[str] = set()
    for row in actual:
        if row.row_id in seen or row.row_id not in expected_by_id:
            continue
        seen.add(row.row_id)
        source = expected_by_id[row.row_id]
        if row.section != source.section or row.text != source.text:
            problems.append(f"CHANGED: 정본 의미/내용이 바뀌었다: {row.row_id}")
        if not row.owner:
            problems.append(f"OWNER_UNASSIGNED: owner가 비었다: {row.row_id}")
        elif row.owner == "W8":
            problems.append(f"OWNER_W8: W8은 구현 owner가 아니다: {row.row_id}")
        elif not VALID_OWNER.fullmatch(row.owner):
            problems.append(
                f"OWNER_MULTIPLE: owner는 W0~W7 하나여야 한다: {row.row_id}={row.owner}"
            )
        elif row.owner != OWNER_BY_ID[row.row_id]:
            problems.append(
                f"OWNER_MISMATCH: semantic owner가 다르다: {row.row_id}="
                f"{row.owner}, 기대 {OWNER_BY_ID[row.row_id]}"
            )
        for field_name, value in (
            ("artifact", row.artifact),
            ("probe_id", row.probe_id),
            ("gate", row.gate),
            ("evidence", row.evidence),
        ):
            if not value:
                problems.append(f"REQUIRED_BLANK: 필수 열이 비었다: {row.row_id}.{field_name}")
        expected_artifact = ARTIFACTS[OWNER_BY_ID[row.row_id]]
        if row.artifact and row.artifact != expected_artifact:
            problems.append(f"ARTIFACT_MISMATCH: owner 산출물이 다르다: {row.row_id}")
        expected_probe = row.row_id.replace("CAP-", "WTL-CAP-")
        if row.probe_id and row.probe_id != expected_probe:
            problems.append(f"PROBE_MISMATCH: 안정 probe/시험 ID가 다르다: {row.row_id}")
        expected_gate = f'x64\\ReleaseMD\\NoteExTests.exe "{expected_probe}"'
        if row.gate and row.gate != expected_gate:
            problems.append(f"GATE_MISMATCH: gate 명령이 다르다: {row.row_id}")
    return problems


def validate_uncertain(actual: list[UncertainLink]) -> list[str]:
    problems: list[str] = []
    expected_ids = [row.link_id for row in UNCERTAIN_LINKS]
    ids = [row.link_id for row in actual]
    if ids != expected_ids:
        problems.append(f"UNCERTAIN_SET: 연결 ID/순서가 다르다: {ids}")
    expected_by_id = {row.link_id: row for row in UNCERTAIN_LINKS}
    for row in actual:
        expected = expected_by_id.get(row.link_id)
        if expected is None:
            continue
        if row.owner == "W8" or (row.owner and not VALID_OWNER.fullmatch(row.owner)):
            problems.append(f"UNCERTAIN_OWNER: owner는 W0~W7 하나여야 한다: {row.link_id}")
        required = (row.owner, row.prerequisite, row.probe_id, row.predicate, row.fallback)
        if row.owner == "W0" and any(not value for value in required):
            problems.append(f"UNCERTAIN_W0_INCOMPLETE: W0 연결이 불완전하다: {row.link_id}")
        elif any(not value for value in required):
            problems.append(f"UNCERTAIN_INCOMPLETE: 연결이 불완전하다: {row.link_id}")
        elif row != expected:
            problems.append(f"UNCERTAIN_CHANGED: 동결 연결 의미가 바뀌었다: {row.link_id}")
    return problems


def validate(
    expected: list[SourceRow], matrix: list[MatrixRow], uncertain: list[UncertainLink]
) -> list[str]:
    return validate_rows(expected, matrix) + validate_uncertain(uncertain)


def render_document(source_path: str, rows: list[SourceRow]) -> str:
    matrix = default_matrix(rows)
    lines = [
        "# pyNote WTL 포팅 capability 추적표 — T4b errata 재결속 01",
        "",
        "- MODE A / T4b errata 정본 재결속본. T4a max 행 ID·owner·probe·gate 동결은 유지한다.",
        "- known-good은 anchor checker를 통과한 아래 F_a01 errata 정본이다.",
        f"- source: `{source_path}`",
        f"- source SHA-256: `{SOURCE_SHA256}`",
        f"- 행 수: `{len(rows)}` (기능 122 + TEXT 27 + RENDERING 19 + PERSISTENCE 40 + PROCESS 26 + NON-FEATURE 40)",
        "- checker는 정본 원순서·내용뿐 아니라 exact semantic owner·owner 산출물·유일 probe ID·exact gate와 `[UNCERTAIN]` 12건의 전체 연결 의미를 검사한다.",
        "- 시험 명령 작업 디렉터리: `D:\\Sources\\python\\pyNote\\NoteEx`",
        "- 완료 증거는 T4c가 실측 결과를 backfill하기 전까지 전건 `미실시`다. 예정 산출물·시험 ID·gate는 완료 증거가 아니다.",
        "",
        "## Capability matrix",
        "",
        MATRIX_BEGIN,
        "| 행 ID | 정본 절 | 원문 행 내용 | 구현 owner wave | 구현 산출물 | 안정 probe/시험 ID | gate 명령 | 완료 증거 |",
        "|---|---|---|---|---|---|---|---|",
    ]
    lines.extend(
        render_table_row(
            (
                row.row_id,
                row.section,
                row.text,
                row.owner,
                row.artifact,
                row.probe_id,
                row.gate,
                row.evidence,
            )
        )
        for row in matrix
    )
    lines.extend(
        [
            MATRIX_END,
            "",
            "## Appendix A `[UNCERTAIN]` linkage",
            "",
            "설계서 §14의 `[UNCERTAIN]` 행 12건을 연결한다. W0 owner는 T1·T2·T3에서 닫고, 후속 wave owner는 해당 wave의 착수 차단 조건으로 유지한다.",
            "",
            UNCERTAIN_BEGIN,
            "| 연결 ID | §14 Qt 행 | owner | 선행 wave/태스크 | probe/시험 ID | PASS 술어 | 실패 시 대안 |",
            "|---|---|---|---|---|---|---|",
        ]
    )
    lines.extend(
        render_table_row(
            (
                row.link_id,
                row.qt_row,
                row.owner,
                row.prerequisite,
                row.probe_id,
                row.predicate,
                row.fallback,
            )
        )
        for row in UNCERTAIN_LINKS
    )
    lines.extend([UNCERTAIN_END, ""])
    return "\n".join(lines)


def _replace_matrix_row(rows: list[MatrixRow], index: int, **changes: str) -> list[MatrixRow]:
    changed = list(rows)
    changed[index] = replace(changed[index], **changes)
    return changed


def run_self_test(source: Path) -> int:
    source_text, digest = read_exact_text(source)
    if digest != SOURCE_SHA256:
        print(f"FAIL  source SHA-256 불일치: {digest}")
        return 1
    expected = source_rows(source_text)
    good = default_matrix(expected)
    links = list(UNCERTAIN_LINKS)
    good_problems = validate(expected, good, links)
    if good_problems:
        print("FAIL  정상 표본 거부")
        for problem in good_problems:
            print(f"  {problem}")
        return 1
    print(f"PASS  정상 표본 수용({len(good)}행, uncertain {len(links)}행)")

    mutations: list[tuple[str, str, Callable[[], tuple[list[MatrixRow], list[UncertainLink]]]]] = [
        ("누락", "MISSING:", lambda: (good[1:], links)),
        ("중복", "DUPLICATE:", lambda: ([good[0], *good], links)),
        ("무단 추가", "EXTRA:", lambda: ([*good, replace(good[-1], row_id="CAP-XX-999")], links)),
        ("원순서 변경", "ORDER:", lambda: ([good[1], good[0], *good[2:]], links)),
        (
            "승인되지 않은 의미 변경",
            "CHANGED:",
            lambda: (_replace_matrix_row(good, 0, text=good[0].text + " altered"), links),
        ),
        (
            "owner 미배정",
            "OWNER_UNASSIGNED:",
            lambda: (_replace_matrix_row(good, 0, owner=""), links),
        ),
        (
            "owner 중복",
            "OWNER_MULTIPLE:",
            lambda: (_replace_matrix_row(good, 0, owner="W3,W4"), links),
        ),
        (
            "semantic owner 변경",
            "OWNER_MISMATCH:",
            lambda: (_replace_matrix_row(good, 0, owner="W1"), links),
        ),
        (
            "필수 열 공란",
            "REQUIRED_BLANK:",
            lambda: (_replace_matrix_row(good, 0, artifact=""), links),
        ),
        (
            "owner 산출물 변경",
            "ARTIFACT_MISMATCH:",
            lambda: (_replace_matrix_row(good, 0, artifact="altered artifact"), links),
        ),
        (
            "probe ID 변경",
            "PROBE_MISMATCH:",
            lambda: (_replace_matrix_row(good, 0, probe_id="WTL-CAP-WRONG-001"), links),
        ),
        (
            "probe ID 중복",
            "PROBE_DUPLICATE:",
            lambda: (_replace_matrix_row(good, 1, probe_id=good[0].probe_id), links),
        ),
        (
            "gate 명령 변경",
            "GATE_MISMATCH:",
            lambda: (_replace_matrix_row(good, 0, gate="wrong gate"), links),
        ),
        ("W8 owner", "OWNER_W8:", lambda: (_replace_matrix_row(good, 0, owner="W8"), links)),
        (
            "W0 UNCERTAIN 연결 불완전",
            "UNCERTAIN_W0_INCOMPLETE:",
            lambda: (good, [replace(links[0], predicate=""), *links[1:]]),
        ),
        (
            "UNCERTAIN 의미 변경",
            "UNCERTAIN_CHANGED:",
            lambda: (
                good,
                [replace(links[0], predicate=links[0].predicate + " altered"), *links[1:]],
            ),
        ),
    ]
    failed = False
    for name, expected_reason, make_bad in mutations:
        bad_rows, bad_links = make_bad()
        problems = validate(expected, bad_rows, bad_links)
        matching = [problem for problem in problems if problem.startswith(expected_reason)]
        allowed_related = {
            "중복": ("ORDER:", "PROBE_DUPLICATE:"),
            "무단 추가": ("PROBE_DUPLICATE:",),
            "probe ID 중복": ("PROBE_MISMATCH:",),
        }.get(name, ())
        unrelated = [
            problem
            for problem in problems
            if not problem.startswith(expected_reason)
            and not any(problem.startswith(prefix) for prefix in allowed_related)
        ]
        if len(matching) == 1 and not unrelated:
            print(f"PASS  seeded known-bad 거부: {name} -> {matching[0]}")
        else:
            failed = True
            print(f"FAIL  seeded known-bad 판별: {name}; problems={problems}")
    return 1 if failed else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--source", type=Path)
    parser.add_argument("--matrix", type=Path)
    args = parser.parse_args(argv)

    if args.self_test:
        if args.source or args.matrix:
            parser.error("--self-test는 --source/--matrix와 함께 쓰지 않는다")
        repo_root = Path(__file__).resolve().parents[3]
        return run_self_test(repo_root / "docs/20260819_2123_Sol_max_WTL포팅_F_a01_errata-01.md")
    if args.source is None or args.matrix is None:
        parser.error("검사에는 --source와 --matrix가 모두 필요하다")

    try:
        source_text, digest = read_exact_text(args.source)
        if digest != SOURCE_SHA256:
            raise MatrixError(f"SOURCE_SHA: {digest}, 기대 {SOURCE_SHA256}")
        expected = source_rows(source_text)
        matrix_text, _ = read_exact_text(args.matrix)
        matrix = parse_matrix(matrix_text)
        uncertain = parse_uncertain(matrix_text)
        problems = validate(expected, matrix, uncertain)
    except (OSError, MatrixError) as exc:
        print(f"환경/입력 오류: {exc}", file=sys.stderr)
        return 2

    if problems:
        print(f"위반 {len(problems)}건")
        for problem in problems:
            print(f"  {problem}")
        return 1
    print(
        f"통과: source SHA-256={digest}, capability {len(matrix)}행 원순서·내용·owner·필수 열 일치, "
        f"uncertain {len(uncertain)}행 연결 완비"
    )
    print(f"runtime: Python {platform.python_version()} / {platform.platform()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
