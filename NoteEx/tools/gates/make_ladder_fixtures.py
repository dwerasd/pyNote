#!/usr/bin/env python3
"""사다리 시험용 중간 버전 데이터베이스 생성기.

버전 1~9 각각에 대해 **그 버전에 정확히 머무는** 데이터베이스를 만든다. 사다리
게이트가 이것을 두 벌 복사해 한 벌은 파이썬 러너로, 한 벌은 C++ 러너로 최신 버전까지
올린 뒤 결과를 대조한다.

**빈 테이블 위의 사다리는 재미있는 것을 전부 건너뛴다.** 그래서 이 도구가 만드는
fixture 에는 실제 행이 들어 있다 - `v0004` 의 행 복사, `v0005` 의 이름변경-복사-삭제,
`v0007`/`v0008` 의 초기화, `v0009` 의 조건부 갱신은 대상 행이 있어야 비로소 동작한다.
행이 없으면 그 마이그레이션들은 아무 일도 하지 않고, 이식이 틀렸어도 결과가 같다.

버전별로 다음을 보장한다:
  * v1~v3: `workspace_state` 에 id=1 행이 있다 -> `v0004` 의 INSERT ... SELECT 가
    실제로 행을 옮기고 UUID 를 만든다.
  * v4~v8: `workspace_windows` 에 결정적 id 를 가진 행이 둘 있다 -> `v0004` 가
    이미 지난 구간에서는 그 행들이 그대로 보존되는지가 대조 대상이 된다.
  * v1~v5: `document_ui_states` 에 행이 둘 있다 -> `v0005` 의 복사와 `v0006` 의
    ADD COLUMN 이 실제 행 위에서 일어난다.
  * v6~v8: `editor_split_left`/`right` 가 NULL 이 아니다 -> `v0007`/`v0008` 의
    초기화가 관측 가능하다. v8 fixture 는 그 뒤로 초기화하는 마이그레이션이 없으므로
    **값이 보존되어야** 한다(없는 초기화를 하는 이식을 잡는다).
  * v2~v4: `preview_lines` 가 6 이다 -> `v0009` 의 조건이 **참**인 갈래.
    v5~v8: 4 다 -> 조건이 **거짓**인 갈래(건드리면 안 된다). v1 fixture 는
    `v0002` 가 구간 안에서 6 을 넣으므로 참 갈래를 탄다.
  * 전 버전: 카드와 현재 리비전이 서로 정합하다 -> `v0003` 의 사전 검사 SELECT 두
    건이 **행이 있는 상태에서** 돌고, 그 검사를 통과한다.
  * 전 버전: 본문·제목에 한국어가 들어 있다 -> 행 복사 경로가 UTF-8 을 보존하는지가
    대조에 걸린다.

값은 전부 상수다. `applied_at_us` 도 고정값이라 같은 입력에서 같은 파일이 나온다
(재현성은 자기시험이 실측한다 - 주장하지 않는다).

이 도구는 게이트가 아니지만 `--self-test` 를 가진다. fixture 가 의도한 버전에
의도한 행을 담고 있지 않으면 사다리 게이트의 판정 전체가 무의미해지기 때문이다.

종료 코드:
  0  성공
  1  자기시험 기대 불일치
  2  사용법·환경 오류
"""

from __future__ import annotations

import argparse
import hashlib
import sqlite3
import sys
import tempfile
from pathlib import Path
from typing import Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))

import migration_reference as reference  # noqa: E402
from migration_reference import MigrationEntry, display_path  # noqa: E402

# fixture 안의 모든 시각 값은 상수다. 러너가 만드는 값과 섞이지 않게 충분히 작다.
FIXTURE_APPLIED_AT_US = 1_600_000_000_000_000

# v0004 가 만드는 UUID 와 같은 모양이되 결정적인 값. v4 이후 fixture 의 창 id 다.
FIXTURE_WINDOW_IDS = (
    "00000000-0000-4000-8000-000000000001",
    "00000000-0000-4000-8000-000000000002",
)

# fixture 가 만들 수 있는 버전 범위(사다리 게이트가 쓰는 구간).
FIXTURE_VERSIONS = tuple(range(1, 10))


def _seed_base(connection: sqlite3.Connection) -> None:
    """v1 스키마에 존재하는 표에 행을 넣는다. 모든 버전 공통이다."""
    connection.execute(
        "INSERT INTO documents(id, title, created_at_us, updated_at_us, "
        "archived_at_us, trashed_at_us) VALUES (?, ?, ?, ?, ?, ?)",
        ("doc-1", "첫째 문서", 1000, 2000, None, None),
    )
    connection.execute(
        "INSERT INTO documents(id, title, created_at_us, updated_at_us, "
        "archived_at_us, trashed_at_us) VALUES (?, ?, ?, ?, ?, ?)",
        ("doc-2", "둘째 문서", 1100, 2100, None, 2500),
    )
    connection.execute(
        "INSERT INTO capture_operations(id, document_id, source, split_policy, "
        "original_text, original_hash, original_redacted_at_us, created_at_us) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        ("op-1", "doc-1", "typing", "keep", "원본 입력 텍스트", "hash-원본", None, 1200),
    )
    connection.execute(
        "INSERT INTO edit_events(event_id, operation_id, document_id, card_id, "
        "event_type, source, occurred_at_us, details_json) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        ("evt-1", "op-1", "doc-1", None, "create", "typing", 1300, "{}"),
    )
    connection.execute(
        "INSERT INTO cards(id, document_id, operation_id, position_key, capture_seq, "
        "created_at_us, updated_at_us, source, body, body_hash, current_revision_id, "
        "deleted_at_us) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        ("card-1", "doc-1", "op-1", 1, 1, 1400, 1400, "typing", "첫째 카드 본문", "hash-1", None, None),
    )
    connection.execute(
        "INSERT INTO cards(id, document_id, operation_id, position_key, capture_seq, "
        "created_at_us, updated_at_us, source, body, body_hash, current_revision_id, "
        "deleted_at_us) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        ("card-2", "doc-1", "op-1", 2, 2, 1500, 1500, "typing", "둘째 카드 본문", "hash-2", None, None),
    )
    connection.execute(
        "INSERT INTO card_revisions(id, card_id, event_seq, parent_revision_id, body, "
        "body_hash, source, created_at_us) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        ("rev-1", "card-1", 1, None, "첫째 카드 본문", "hash-1", "edit", 1600),
    )
    connection.execute(
        "INSERT INTO card_revisions(id, card_id, event_seq, parent_revision_id, body, "
        "body_hash, source, created_at_us) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        ("rev-2", "card-2", 1, None, "둘째 카드 본문", "hash-2", "edit", 1700),
    )
    # 본문·해시가 리비전과 일치해야 v0003 의 사전 검사와 트리거를 함께 통과한다.
    connection.execute(
        "UPDATE cards SET current_revision_id = 'rev-1' WHERE id = 'card-1'"
    )
    connection.execute(
        "UPDATE cards SET current_revision_id = 'rev-2' WHERE id = 'card-2'"
    )
    connection.execute(
        "INSERT INTO drafts(id, document_id, card_id, draft_kind, base_revision_id, "
        "draft_text, draft_hash, cursor_position_qchar, updated_at_us) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
        ("draft-1", "doc-1", "card-1", "edit", "rev-1", "편집 중 초안", "hash-draft", 3, 1800),
    )
    connection.execute(
        "INSERT INTO card_lineage(parent_card_id, child_card_id, event_seq, relation_type) "
        "VALUES (?, ?, ?, ?)",
        ("card-1", "card-2", 1, "split"),
    )
    # capture counter 는 감소가 금지되어 있으므로 늘리기만 한다(v0003 트리거).
    connection.execute("UPDATE counters SET next_value = 3 WHERE name = 'capture'")
    # 선택 카드와 편집 카드를 **다르게** 둔다. 같은 값이면 v0005 의 복사에서 두 열이
    # 뒤바뀌어도 결과가 같아 보여 열 순서 결함이 관측되지 않는다.
    connection.execute(
        "INSERT INTO document_ui_states(document_id, selected_card_id, "
        "list_scroll_position, sort_mode, editor_card_id, editor_base_revision_id, "
        "editor_cursor_qchar, updated_at_us) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        ("doc-1", "card-1", 12, "position", "card-2", "rev-2", 7, 2200),
    )
    connection.execute(
        "INSERT INTO document_ui_states(document_id, selected_card_id, "
        "list_scroll_position, sort_mode, editor_card_id, editor_base_revision_id, "
        "editor_cursor_qchar, updated_at_us) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        ("doc-2", None, 0, "recency", None, None, None, 2300),
    )


def _seed_version(connection: sqlite3.Connection, version: int) -> None:
    """버전별 추가 시드. 그 버전의 스키마에서만 성립하는 것들이다."""
    if version <= 3:
        # v0004 가 옮길 원본 행. v4 부터는 이 표가 없다.
        connection.execute(
            "INSERT INTO workspace_state(id, open_document_ids_json, "
            "active_document_id, updated_at_us) VALUES (?, ?, ?, ?)",
            (1, '["doc-1", "doc-2"]', "doc-1", 1900),
        )
    else:
        # v0004 는 이미 지났고 원본이 비어 있었으므로 창 행이 없다. 결정적 id 로 넣는다.
        connection.execute(
            "INSERT INTO workspace_windows(window_id, open_document_ids_json, "
            "active_document_id, updated_at_us) VALUES (?, ?, ?, ?)",
            (FIXTURE_WINDOW_IDS[0], '["doc-1"]', "doc-1", 2000),
        )
        connection.execute(
            "INSERT INTO workspace_windows(window_id, open_document_ids_json, "
            "active_document_id, updated_at_us) VALUES (?, ?, ?, ?)",
            (FIXTURE_WINDOW_IDS[1], '["doc-2"]', None, 2100),
        )

    if version >= 2:
        # v0009 의 두 갈래를 버전으로 나눠 덮는다(모듈 docstring 참조).
        preview_lines = 6 if version <= 4 else 4
        connection.execute(
            "UPDATE data_policy_settings SET draft_idle_ms = ?, split_policy = ?, "
            "preview_lines = ?, backup_interval_hours = ?, trash_retention_days = ?, "
            "updated_at_us = ? WHERE id = 1",
            (1500, "split_by_blank_line", preview_lines, 12.0, 15, 2400),
        )

    if version >= 6:
        # v0007/v0008 의 초기화가 관측 가능하도록 값을 채워 둔다.
        connection.execute(
            "UPDATE document_ui_states SET editor_split_left = 320, "
            "editor_split_right = 680 WHERE document_id = 'doc-1'"
        )
        connection.execute(
            "UPDATE document_ui_states SET editor_split_left = 400, "
            "editor_split_right = 600 WHERE document_id = 'doc-2'"
        )


def build_fixture(
    db_path: Path, version: int, entries: Sequence[MigrationEntry]
) -> Path:
    """빈 파일에서 시작해 `version` 까지 올리고 그 버전용 행을 채운다."""
    if db_path.exists():
        db_path.unlink()
    for sidecar in ("-wal", "-shm"):
        extra = Path(str(db_path) + sidecar)
        if extra.exists():
            extra.unlink()

    reference.build_database(db_path, entries, FIXTURE_APPLIED_AT_US, upto=version)

    connection = reference.open_connection(db_path)
    try:
        connection.execute("BEGIN IMMEDIATE")
        try:
            _seed_base(connection)
            _seed_version(connection, version)
            connection.commit()
        except BaseException:
            connection.rollback()
            raise
        # 사이드카를 남기지 않아야 파일 하나를 복사하는 것으로 fixture 이관이 끝난다.
        connection.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    finally:
        connection.close()
    return db_path


def build_all(out_dir: Path, entries: Sequence[MigrationEntry]) -> list[Path]:
    """v0001.db ~ v0009.db 를 만든다."""
    out_dir.mkdir(parents=True, exist_ok=True)
    produced: list[Path] = []
    for version in FIXTURE_VERSIONS:
        produced.append(
            build_fixture(out_dir / f"v{version:04d}.db", version, entries)
        )
    return produced


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _input_files(entries: Sequence[MigrationEntry]) -> list[Path]:
    """산출물을 결정하는 입력 파일 목록(마이그레이션 원본 + 이 도구 + 공용 모듈)."""
    here = Path(__file__).resolve()
    return [entry.source for entry in entries] + [
        here,
        here.parent / "migration_reference.py",
    ]


def run_build(out_dir: Path) -> int:
    """fixture 를 만들고 입력·산출 SHA-256 을 기록한다."""
    try:
        entries = reference.load_registry()
    except Exception as exc:
        print(f"오류: 파이썬 원본 적재 실패 - {exc}", file=sys.stderr)
        return 2

    print("입력 SHA-256:")
    for path in _input_files(entries):
        print(f"  {sha256(path)}  {display_path(path)}")

    try:
        produced = build_all(out_dir, entries)
    except (sqlite3.Error, RuntimeError, OSError) as exc:
        print(f"오류: fixture 생성 실패 - {exc}", file=sys.stderr)
        return 2

    print(f"산출 SHA-256 ({display_path(out_dir)}):")
    for path in produced:
        version = reference.database_version(path)
        print(
            f"  {sha256(path)}  {path.name}  "
            f"(schema version {version}, {path.stat().st_size}바이트)"
        )
    return 0


def _row_count(db_path: Path, table: str, where: str = "1") -> int:
    connection = sqlite3.connect(db_path)
    try:
        row = connection.execute(f'SELECT count(*) FROM "{table}" WHERE {where}').fetchone()
        return int(row[0])
    finally:
        connection.close()


def _table_exists(db_path: Path, table: str) -> bool:
    connection = sqlite3.connect(db_path)
    try:
        row = connection.execute(
            "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?", (table,)
        ).fetchone()
        return row is not None
    finally:
        connection.close()


def run_self_test() -> int:
    """fixture 가 의도한 버전에 의도한 행을 담는지 본다.

    사다리 게이트는 이 fixture 를 근거로 판정하므로, fixture 가 조용히 비어 있으면
    게이트가 초록인 채로 아무것도 증명하지 못한다. 여기서 막는다.
    """
    try:
        entries = reference.load_registry()
    except Exception as exc:
        print(f"오류: 파이썬 원본 적재 실패 - {exc}", file=sys.stderr)
        return 2

    failures = 0
    with tempfile.TemporaryDirectory(prefix="ladder_fixture_selftest_") as work:
        work_dir = Path(work)
        first = work_dir / "first"
        second = work_dir / "second"

        print(f"[방향 1] 버전 {FIXTURE_VERSIONS[0]}~{FIXTURE_VERSIONS[-1]} fixture 생성")
        try:
            produced = build_all(first, entries)
        except (sqlite3.Error, RuntimeError, OSError) as exc:
            print(f"  ERROR fixture 생성 실패 - {exc}", file=sys.stderr)
            return 2

        for version, path in zip(FIXTURE_VERSIONS, produced):
            checks: list[tuple[str, bool]] = []
            actual_version = reference.database_version(path)
            checks.append((f"schema version == {version}", actual_version == version))
            checks.append(("documents 2행", _row_count(path, "documents") == 2))
            checks.append(("cards 2행", _row_count(path, "cards") == 2))
            checks.append(
                (
                    "cards.current_revision_id 전건 채움",
                    _row_count(path, "cards", "current_revision_id IS NOT NULL") == 2,
                )
            )
            checks.append(
                ("document_ui_states 2행", _row_count(path, "document_ui_states") == 2)
            )
            if version <= 3:
                checks.append(
                    (
                        "workspace_state id=1 행(v0004 의 복사 대상)",
                        _row_count(path, "workspace_state", "id = 1") == 1,
                    )
                )
                checks.append(
                    ("workspace_windows 없음", not _table_exists(path, "workspace_windows"))
                )
            else:
                checks.append(
                    ("workspace_state 없음", not _table_exists(path, "workspace_state"))
                )
                checks.append(
                    ("workspace_windows 2행", _row_count(path, "workspace_windows") == 2)
                )
            if version >= 2:
                expected_preview = 6 if version <= 4 else 4
                checks.append(
                    (
                        f"preview_lines == {expected_preview}"
                        f" (v0009 조건 {'참' if expected_preview == 6 else '거짓'} 갈래)",
                        _row_count(
                            path, "data_policy_settings", f"preview_lines = {expected_preview}"
                        )
                        == 1,
                    )
                )
            else:
                checks.append(
                    (
                        "data_policy_settings 없음",
                        not _table_exists(path, "data_policy_settings"),
                    )
                )
            if version >= 6:
                checks.append(
                    (
                        "editor_split_left/right 전건 채움(v0007/v0008 관측용)",
                        _row_count(
                            path,
                            "document_ui_states",
                            "editor_split_left IS NOT NULL AND editor_split_right IS NOT NULL",
                        )
                        == 2,
                    )
                )
            for sidecar in ("-wal", "-shm"):
                checks.append(
                    (
                        f"사이드카 {sidecar} 없음",
                        not Path(str(path) + sidecar).exists(),
                    )
                )

            bad = [name for name, ok in checks if not ok]
            if bad:
                print(f"  FAIL  {path.name}: {len(bad)}건 - {'; '.join(bad)}")
                failures += 1
            else:
                print(f"  PASS  {path.name}: {len(checks)}개 항목(schema version {actual_version})")

        print("[방향 2] 재현성 - 같은 입력에서 같은 파일이 나오는가")
        repeated = build_all(second, entries)
        identical = 0
        differing: list[str] = []
        for left, right in zip(produced, repeated):
            if sha256(left) == sha256(right):
                identical += 1
            else:
                differing.append(left.name)
        if differing:
            print(
                f"  주의  파일 SHA-256 이 다른 fixture {len(differing)}건: "
                f"{', '.join(differing)}"
            )
            print(
                "        (사다리 게이트는 매 실행마다 fixture 를 새로 만들므로 "
                "판정에는 영향이 없다)"
            )
        else:
            print(f"  PASS  {identical}건 전건 바이트 동일")

        print("[방향 3] 덤프 재현성 - 같은 버전 fixture 의 덤프가 같은가")
        for left, right in zip(produced, repeated):
            version = reference.database_version(left)
            left_dump = reference.dump_data(left, version)
            right_dump = reference.dump_data(right, version)
            if left_dump.text != right_dump.text:
                print(f"  FAIL  {left.name}: 덤프가 다르다")
                failures += 1
                break
            if left_dump.violations:
                print(f"  FAIL  {left.name}: 정규화 검증 위반 {left_dump.violations}")
                failures += 1
                break
        else:
            print(f"  PASS  fixture {len(produced)}건 덤프 동일·정규화 위반 0건")

        print("[방향 4] 시드 없는 사다리와 다른가 - fixture 가 실제로 비어 있지 않은가")
        bare = work_dir / "bare.db"
        reference.build_database(bare, entries, FIXTURE_APPLIED_AT_US, upto=1)
        bare_rows = reference.dump_data(bare, 1).rows
        seeded_rows = reference.dump_data(produced[0], 1).rows
        if seeded_rows > bare_rows:
            print(
                f"  PASS  v0001 fixture 행 {seeded_rows}건 > 시드 없는 사다리 "
                f"{bare_rows}건"
            )
        else:
            print(
                f"  FAIL  v0001 fixture 가 비어 있다(행 {seeded_rows}건, "
                f"시드 없음 {bare_rows}건)"
            )
            failures += 1

    if failures:
        print(f"자기시험 실패 {failures}건", file=sys.stderr)
        return 1
    print(
        f"자기시험 PASS: fixture {len(FIXTURE_VERSIONS)}건이 의도한 버전·행을 담고 "
        "덤프가 재현된다"
    )
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    reference.force_utf8_output()
    parser = argparse.ArgumentParser(
        description="사다리 시험용 중간 버전 데이터베이스 생성기",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--out",
        metavar="DIR",
        default=None,
        help="fixture 를 쓸 디렉터리(지정하면 생성하고 SHA-256 을 찍는다)",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="임시 디렉터리에 만들어 내용·재현성을 검증한다(--out 무시)",
    )
    args = parser.parse_args(argv)

    if args.self_test:
        return run_self_test()
    if args.out is None:
        parser.error("--out 또는 --self-test 가 필요하다")
    return run_build(Path(args.out))


if __name__ == "__main__":
    sys.exit(main())
