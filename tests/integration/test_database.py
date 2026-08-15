from __future__ import annotations

import re
import sqlite3
from pathlib import Path

from pynote.domain.events import EventSource
from pynote.domain.models import (
    CaptureOperationSource,
    CardSource,
    RevisionSource,
)
from pynote.infrastructure.database import Database
from pynote.infrastructure.migrations import LATEST_SCHEMA_VERSION
from pynote.infrastructure.settings import DataPolicySettings

EXPECTED_TABLES = {
    "documents",
    "capture_operations",
    "cards",
    "card_revisions",
    "drafts",
    "edit_events",
    "card_lineage",
    "counters",
    "workspace_windows",
    "document_ui_states",
    "data_policy_settings",
}

EXPECTED_SOURCES = {
    "capture_operations": {
        "typing",
        "paste",
        "import",
        "mixed",
        "split",
        "merge",
        "system",
    },
    "cards": {
        "typing",
        "paste",
        "import",
        "mixed",
        "restore",
        "split",
        "merge",
        "system",
    },
    "card_revisions": {"edit", "restore", "split", "merge"},
    "edit_events": {
        "typing",
        "paste",
        "import",
        "mixed",
        "edit",
        "restore",
        "system",
    },
}


def test_data_policy_object_defaults_preview_lines_to_three() -> None:
    policy = DataPolicySettings(
        draft_idle_ms=2_000,
        split_policy="keep",
        backup_interval_hours=24.0,
        trash_retention_days=30,
        updated_at_us=1,
    )

    assert policy.preview_lines == 3


def _table_names(database: Database) -> set[str]:
    rows = database.connection.execute(
        """
        SELECT name
        FROM sqlite_master
        WHERE type = 'table' AND name NOT LIKE 'sqlite_%'
        """
    ).fetchall()
    return {str(row[0]) for row in rows}


def _source_check_values(database: Database, table: str) -> set[str]:
    row = database.connection.execute(
        "SELECT sql FROM sqlite_master WHERE type = 'table' AND name = ?",
        (table,),
    ).fetchone()
    assert row is not None
    match = re.search(
        r"source\s+TEXT\s+NOT\s+NULL\s+CHECK\s*\(\s*source\s+IN\s*\((.*?)\)\s*\)",
        str(row[0]),
        flags=re.DOTALL | re.IGNORECASE,
    )
    assert match is not None
    return set(re.findall(r"'([^']+)'", match.group(1)))


def _replace_workspace_windows_with_v3_fixture(database: Database) -> None:
    database.connection.execute("DROP TABLE workspace_windows")
    database.connection.execute(
        """
        CREATE TABLE workspace_state (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            open_document_ids_json TEXT NOT NULL,
            active_document_id TEXT
                REFERENCES documents(id) ON DELETE RESTRICT,
            updated_at_us INTEGER NOT NULL
        )
        """
    )


def test_new_v0_database_migrates_to_latest(database_path: Path) -> None:
    with Database(database_path) as database:
        assert database.schema_version == LATEST_SCHEMA_VERSION
        assert EXPECTED_TABLES <= _table_names(database)
        assert database.connection.execute("PRAGMA foreign_keys").fetchone()[0] == 1
        assert database.connection.execute("PRAGMA journal_mode").fetchone()[0] == "wal"
        assert (
            database.connection.execute(
                "SELECT next_value FROM counters WHERE name = 'capture'"
            ).fetchone()[0]
            == 1
        )
        assert (
            database.connection.execute(
                "SELECT preview_lines FROM data_policy_settings WHERE id = 1"
            ).fetchone()[0]
            == 3
        )


def test_previous_schema_fixture_migrates_after_backup_hook(
    database_path: Path,
) -> None:
    fixture = sqlite3.connect(database_path)
    fixture.execute(
        """
        CREATE TABLE schema_version (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            version INTEGER NOT NULL CHECK (version >= 0),
            applied_at_us INTEGER NOT NULL
        )
        """
    )
    fixture.execute("INSERT INTO schema_version(id, version, applied_at_us) VALUES (1, 0, 0)")
    fixture.commit()
    fixture.close()
    calls: list[tuple[Path, int, int]] = []

    with Database(
        database_path,
        backup_hook=lambda path, old, new: calls.append((path, old, new)),
    ) as database:
        assert database.schema_version == LATEST_SCHEMA_VERSION
        assert EXPECTED_TABLES <= _table_names(database)

    assert calls == [(database_path, 0, LATEST_SCHEMA_VERSION)]


def test_v1_database_fixture_migrates_data_policy_after_backup_hook(
    database_path: Path,
) -> None:
    with Database(database_path) as created:
        connection = created.connection
        _replace_workspace_windows_with_v3_fixture(created)
        connection.execute("DROP TABLE data_policy_settings")
        connection.execute(
            "UPDATE schema_version SET version = 1, applied_at_us = 1 WHERE id = 1"
        )
    calls: list[tuple[Path, int, int]] = []

    with Database(
        database_path,
        backup_hook=lambda path, old, new: calls.append((path, old, new)),
    ) as migrated:
        row = migrated.connection.execute(
            """
            SELECT draft_idle_ms, split_policy, preview_lines,
                   backup_interval_hours, trash_retention_days
            FROM data_policy_settings
            """
        ).fetchone()
        assert tuple(row) == (2000, "keep", 3, 24.0, 30)

    assert calls == [(database_path, 1, LATEST_SCHEMA_VERSION)]


def test_v2_database_fixture_adds_storage_invariant_triggers(
    database_path: Path,
) -> None:
    trigger_names = {
        "cards_current_revision_insert",
        "cards_current_revision_update",
        "card_revisions_parent_insert",
        "card_revisions_parent_update",
        "card_revisions_current_update",
        "capture_counter_no_decrease",
        "capture_counter_no_rename",
        "capture_counter_no_delete",
    }
    with Database(database_path) as created:
        _replace_workspace_windows_with_v3_fixture(created)
        for trigger_name in trigger_names:
            created.connection.execute(f"DROP TRIGGER {trigger_name}")
        created.connection.execute(
            "UPDATE schema_version SET version = 2, applied_at_us = 1 WHERE id = 1"
        )

    with Database(database_path) as migrated:
        rows = migrated.connection.execute(
            "SELECT name FROM sqlite_master WHERE type = 'trigger'"
        ).fetchall()
        assert trigger_names <= {str(row[0]) for row in rows}
        assert migrated.schema_version == LATEST_SCHEMA_VERSION


def test_v3_workspace_fixture_migrates_rows_without_data_loss(
    database_path: Path,
) -> None:
    with Database(database_path) as created:
        _replace_workspace_windows_with_v3_fixture(created)
        created.connection.execute(
            """
            INSERT INTO documents(id, title, created_at_us, updated_at_us)
            VALUES ('document-v3', 'v3 문서', 1, 2)
            """
        )
        created.connection.execute(
            """
            INSERT INTO workspace_state(
                id, open_document_ids_json, active_document_id, updated_at_us
            )
            VALUES (1, '["document-v3"]', 'document-v3', 3)
            """
        )
        created.connection.execute(
            "UPDATE schema_version SET version = 3, applied_at_us = 3 WHERE id = 1"
        )

    with Database(database_path) as migrated:
        row = migrated.connection.execute(
            """
            SELECT open_document_ids_json, active_document_id, updated_at_us
            FROM workspace_windows
            """
        ).fetchone()
        assert tuple(row) == ('["document-v3"]', "document-v3", 3)
        assert (
            migrated.connection.execute(
                """
                SELECT 1 FROM sqlite_master
                WHERE type = 'table' AND name = 'workspace_state'
                """
            ).fetchone()
            is None
        )


def test_v4_sort_mode_fixture_preserves_values_and_accepts_recency(
    database_path: Path,
) -> None:
    with Database(database_path) as created:
        connection = created.connection
        connection.execute("DROP TABLE document_ui_states")
        connection.execute(
            """
            CREATE TABLE document_ui_states (
                document_id TEXT PRIMARY KEY
                    REFERENCES documents(id) ON DELETE RESTRICT,
                selected_card_id TEXT
                    REFERENCES cards(id) ON DELETE RESTRICT,
                list_scroll_position INTEGER NOT NULL,
                sort_mode TEXT NOT NULL
                    CHECK (sort_mode IN ('position', 'capture')),
                editor_card_id TEXT
                    REFERENCES cards(id) ON DELETE RESTRICT,
                editor_base_revision_id TEXT
                    REFERENCES card_revisions(id) ON DELETE RESTRICT,
                editor_cursor_qchar INTEGER,
                updated_at_us INTEGER NOT NULL
            )
            """
        )
        connection.execute(
            """
            INSERT INTO documents(id, title, created_at_us, updated_at_us)
            VALUES
                ('position-document', '위치순', 1, 1),
                ('capture-document', '기록순', 1, 1),
                ('recency-document', '최근순', 1, 1)
            """
        )
        connection.execute(
            """
            INSERT INTO document_ui_states(
                document_id, selected_card_id, list_scroll_position, sort_mode,
                editor_card_id, editor_base_revision_id, editor_cursor_qchar,
                updated_at_us
            )
            VALUES
                ('position-document', NULL, 0, 'position', NULL, NULL, NULL, 2),
                ('capture-document', NULL, 0, 'capture', NULL, NULL, NULL, 2)
            """
        )
        connection.execute(
            "UPDATE schema_version SET version = 4, applied_at_us = 4 WHERE id = 1"
        )

    with Database(database_path) as migrated:
        rows = migrated.connection.execute(
            """
            SELECT document_id, sort_mode
            FROM document_ui_states
            ORDER BY document_id
            """
        ).fetchall()
        assert [tuple(row) for row in rows] == [
            ("capture-document", "capture"),
            ("position-document", "position"),
        ]
        migrated.connection.execute(
            """
            INSERT INTO document_ui_states(
                document_id, selected_card_id, list_scroll_position, sort_mode,
                editor_card_id, editor_base_revision_id, editor_cursor_qchar,
                updated_at_us
            )
            VALUES ('recency-document', NULL, 0, 'recency', NULL, NULL, NULL, 3)
            """
        )
        assert migrated.schema_version == LATEST_SCHEMA_VERSION


def test_v5_fixture_gains_editor_split_size_columns(
    database_path: Path,
) -> None:
    with Database(database_path) as created:
        connection = created.connection
        connection.execute("DROP TABLE document_ui_states")
        connection.execute(
            """
            CREATE TABLE document_ui_states (
                document_id TEXT PRIMARY KEY
                    REFERENCES documents(id) ON DELETE RESTRICT,
                selected_card_id TEXT
                    REFERENCES cards(id) ON DELETE RESTRICT,
                list_scroll_position INTEGER NOT NULL,
                sort_mode TEXT NOT NULL
                    CHECK (sort_mode IN ('recency', 'position', 'capture')),
                editor_card_id TEXT
                    REFERENCES cards(id) ON DELETE RESTRICT,
                editor_base_revision_id TEXT
                    REFERENCES card_revisions(id) ON DELETE RESTRICT,
                editor_cursor_qchar INTEGER,
                updated_at_us INTEGER NOT NULL
            )
            """
        )
        connection.execute(
            """
            INSERT INTO documents(id, title, created_at_us, updated_at_us)
            VALUES ('split-document', '분할 문서', 1, 1)
            """
        )
        connection.execute(
            """
            INSERT INTO document_ui_states(
                document_id, selected_card_id, list_scroll_position, sort_mode,
                editor_card_id, editor_base_revision_id, editor_cursor_qchar,
                updated_at_us
            )
            VALUES ('split-document', NULL, 0, 'recency', NULL, NULL, NULL, 2)
            """
        )
        connection.execute(
            "UPDATE schema_version SET version = 5, applied_at_us = 5 WHERE id = 1"
        )

    with Database(database_path) as migrated:
        columns = {
            str(row[1])
            for row in migrated.connection.execute(
                "PRAGMA table_info(document_ui_states)"
            )
        }
        assert {"editor_split_left", "editor_split_right"} <= columns
        row = migrated.connection.execute(
            """
            SELECT editor_split_left, editor_split_right
            FROM document_ui_states
            WHERE document_id = 'split-document'
            """
        ).fetchone()
        assert tuple(row) == (None, None)
        assert migrated.schema_version == LATEST_SCHEMA_VERSION


def test_v6_fixture_clears_horizontal_split_sizes(
    database_path: Path,
) -> None:
    with Database(database_path) as created:
        connection = created.connection
        connection.execute(
            """
            INSERT INTO documents(id, title, created_at_us, updated_at_us)
            VALUES ('vertical-document', '세로 전환 문서', 1, 1)
            """
        )
        connection.execute(
            """
            INSERT INTO document_ui_states(
                document_id, list_scroll_position, sort_mode,
                editor_split_left, editor_split_right, updated_at_us
            )
            VALUES ('vertical-document', 3, 'position', 400, 800, 2)
            """
        )
        connection.execute(
            "UPDATE schema_version SET version = 6, applied_at_us = 6 WHERE id = 1"
        )

    with Database(database_path) as migrated:
        row = migrated.connection.execute(
            """
            SELECT editor_split_left, editor_split_right,
                   sort_mode, list_scroll_position
            FROM document_ui_states
            WHERE document_id = 'vertical-document'
            """
        ).fetchone()
        # 가로 기준 (목록, 편집기) 값은 세로로 읽으면 의미가 반전된다.
        assert (row[0], row[1]) == (None, None)
        # 나머지 UI 상태는 건드리지 않는다.
        assert (row[2], row[3]) == ("position", 3)
        assert migrated.schema_version == LATEST_SCHEMA_VERSION


def test_v7_fixture_clears_vertical_split_sizes(
    database_path: Path,
) -> None:
    with Database(database_path) as created:
        connection = created.connection
        connection.execute(
            """
            INSERT INTO documents(id, title, created_at_us, updated_at_us)
            VALUES ('horizontal-document', '가로 전환 문서', 1, 1)
            """
        )
        connection.execute(
            """
            INSERT INTO document_ui_states(
                document_id, list_scroll_position, sort_mode,
                editor_split_left, editor_split_right, updated_at_us
            )
            VALUES ('horizontal-document', 4, 'capture', 500, 300, 2)
            """
        )
        connection.execute(
            "UPDATE schema_version SET version = 7, applied_at_us = 7 WHERE id = 1"
        )

    with Database(database_path) as migrated:
        row = migrated.connection.execute(
            """
            SELECT editor_split_left, editor_split_right,
                   sort_mode, list_scroll_position
            FROM document_ui_states
            WHERE document_id = 'horizontal-document'
            """
        ).fetchone()
        assert (row[0], row[1]) == (None, None)
        assert (row[2], row[3]) == ("capture", 4)
        assert migrated.schema_version == LATEST_SCHEMA_VERSION


def test_v8_fixture_normalizes_old_preview_line_default(
    database_path: Path,
) -> None:
    with Database(database_path) as created:
        created.connection.execute(
            "UPDATE data_policy_settings SET preview_lines = 6 WHERE id = 1"
        )
        created.connection.execute(
            "UPDATE schema_version SET version = 8, applied_at_us = 8 WHERE id = 1"
        )

    with Database(database_path) as migrated:
        assert (
            migrated.connection.execute(
                "SELECT preview_lines FROM data_policy_settings WHERE id = 1"
            ).fetchone()[0]
            == 3
        )
        assert migrated.schema_version == LATEST_SCHEMA_VERSION


def test_v8_fixture_preserves_explicit_preview_line_value(
    database_path: Path,
) -> None:
    with Database(database_path) as created:
        created.connection.execute(
            "UPDATE data_policy_settings SET preview_lines = 9 WHERE id = 1"
        )
        created.connection.execute(
            "UPDATE schema_version SET version = 8, applied_at_us = 8 WHERE id = 1"
        )

    with Database(database_path) as migrated:
        assert (
            migrated.connection.execute(
                "SELECT preview_lines FROM data_policy_settings WHERE id = 1"
            ).fetchone()[0]
            == 9
        )
        assert migrated.schema_version == LATEST_SCHEMA_VERSION


def test_all_foreign_keys_use_delete_restrict(database: Database) -> None:
    for table in EXPECTED_TABLES:
        foreign_keys = database.connection.execute(f"PRAGMA foreign_key_list({table})").fetchall()
        assert all(str(foreign_key[6]).upper() == "RESTRICT" for foreign_key in foreign_keys)


def test_source_checks_and_domain_enums_match_data_model(
    database: Database,
) -> None:
    actual = {table: _source_check_values(database, table) for table in EXPECTED_SOURCES}
    assert actual == EXPECTED_SOURCES
    assert {source.value for source in CaptureOperationSource} == EXPECTED_SOURCES[
        "capture_operations"
    ]
    assert {source.value for source in CardSource} == EXPECTED_SOURCES["cards"]
    assert {source.value for source in RevisionSource} == EXPECTED_SOURCES["card_revisions"]
    assert {source.value for source in EventSource} == EXPECTED_SOURCES["edit_events"]


def test_required_partial_unique_indexes_exist(database: Database) -> None:
    rows = database.connection.execute(
        """
        SELECT name, sql
        FROM sqlite_master
        WHERE type = 'index' AND name IN ('active_card_position', 'active_card_draft')
        """
    ).fetchall()
    indexes = {str(row[0]): str(row[1]).lower() for row in rows}
    assert "where deleted_at_us is null" in indexes["active_card_position"]
    assert "where card_id is not null" in indexes["active_card_draft"]
