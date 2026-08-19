from __future__ import annotations

import argparse
import os
import sys
import tempfile
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    source_root = args.source_root.resolve()
    os.environ["QT_QPA_PLATFORM"] = "offscreen"
    sys.path.insert(0, str(source_root / "src"))

    import pynote
    from PySide6.QtCore import QSettings
    from PySide6.QtWidgets import QApplication
    from pynote.app import AppContext, SqliteWorkspaceStateStore, WindowManager, initialize_device_settings
    from pynote.domain.models import Document
    from pynote.infrastructure.database import Database
    from pynote.infrastructure.repositories import Repositories, WorkspaceWindow
    from pynote.ui.main_window import MainWindow

    package = Path(pynote.__file__).resolve()
    if not package.is_relative_to(source_root):
        raise RuntimeError(f"pynote import escaped source root: {package}")

    application = QApplication.instance() or QApplication([])
    lines: list[str] = []

    def settings_for(directory: Path, name: str) -> QSettings:
        settings = QSettings(str(directory / f"{name}.ini"), QSettings.Format.IniFormat)
        initialize_device_settings(settings)
        settings.setValue("first_run/guide_shown", True)
        return settings

    def add_document(repositories: Repositories, document_id: str, time: int) -> None:
        repositories.create_document(Document(
            id=document_id, title=document_id, created_at_us=time, updated_at_us=time,
        ))

    def optional(value: object | None) -> str:
        return "-" if value is None else str(value)

    def record_text(records: tuple[WorkspaceWindow, ...]) -> str:
        return ";".join(
            f"{record.window_id}:{','.join(record.open_document_ids)}@{record.active_document_id or '-'}"
            for record in records
        )

    def manager_case(directory: Path, name: str, documents: tuple[str, str],
                     rows: tuple[tuple[str, tuple[str, ...], str], ...]) -> tuple[str, str, str]:
        database = Database(directory / f"{name}.sqlite3")
        repositories = Repositories(database)
        for time, document_id in enumerate(documents, start=1):
            add_document(repositories, document_id, time)
        for window_id, open_ids, active_id in rows:
            repositories.save_workspace_window(window_id, open_ids, active_id)
        before = repositories.list_workspace_windows()
        settings = settings_for(directory, name)
        context = AppContext(database, settings)
        manager = WindowManager(context)
        manager.restore_windows()
        application.processEvents()
        output = ";".join(
            f"{window.window_id}:{','.join(window.open_document_ids)}@{window.active_document_id or '-'}"
            for window in manager.windows
        )
        after = repositories.list_workspace_windows()
        before_by_id = {record.window_id: record for record in before}
        rewrites = ",".join(
            record.window_id for record in after
            if (
                record.open_document_ids != before_by_id[record.window_id].open_document_ids
                or record.active_document_id != before_by_id[record.window_id].active_document_id
            )
        ) or "-"
        manager.prepare_shutdown()
        for window in manager.windows:
            if not window.close():
                raise RuntimeError(f"manager window close rejected: {window.window_id}")
        context.maintenance_timer.stop()
        application.processEvents()
        database.close()
        return record_text(before), output, rewrites

    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)

        database = Database(directory / "0052.sqlite3")
        repositories = Repositories(database)
        add_document(repositories, "first", 1)
        repositories.save_workspace_window("window-first", ("first",), "first")
        settings = settings_for(directory, "0052")
        store = SqliteWorkspaceStateStore(database, "window-first")
        first = MainWindow(repositories, store, settings=settings, window_id="window-first")
        first.show(); application.processEvents()
        page = first.active_document_page()
        if page is None:
            raise RuntimeError("0052 page was not restored")
        page.sort_combo.setCurrentIndex(page.sort_combo.findData("capture"))
        first.save_document_ui_state(
            "first", list_scroll_position=137, sort_mode="capture", editor_cursor_qchar=9,
        )
        if not first.close():
            raise RuntimeError("0052 accepted close was rejected")
        restarted = MainWindow(repositories, store, settings=settings, window_id="window-first")
        restarted.show(); application.processEvents()
        workspace = store.load_workspace()
        state = restarted.document_ui_state("first")
        if state is None:
            raise RuntimeError("0052 UI state was not restored")
        lines.append(
            "WTL-W2-0052"
            f"|workspace={','.join(workspace.open_document_ids)}|active={optional(workspace.active_document_id)}"
            f"|selected={optional(state.selected_card_id)}|scroll={state.list_scroll_position}"
            f"|sort={state.sort_mode}|editor={optional(state.editor_card_id)}"
            f"|base={optional(state.editor_base_revision_id)}|cursor={optional(state.editor_cursor_qchar)}"
            f"|split={optional(state.editor_split_sizes)}"
        )
        if not restarted.close():
            raise RuntimeError("0052 restart close was rejected")
        application.processEvents(); database.close()

        database = Database(directory / "0053.sqlite3")
        repositories = Repositories(database)
        add_document(repositories, "sort-document", 1)
        repositories.save_workspace_window("sort-window", ("sort-document",), "sort-document")
        settings = settings_for(directory, "0053")
        store = SqliteWorkspaceStateStore(database, "sort-window")
        first = MainWindow(repositories, store, settings=settings, window_id="sort-window")
        page = first.active_document_page()
        if page is None:
            raise RuntimeError("0053 initial page missing")
        initial = page.stream.card_model.sort_mode
        page.sort_combo.setCurrentIndex(page.sort_combo.findData("position"))
        first.persist_open_page_ui_states()
        stored = store.load_document_ui_state("sort-document")
        if stored is None:
            raise RuntimeError("0053 position state missing")
        restarted = MainWindow(repositories, store, settings=settings, window_id="sort-window")
        restarted_page = restarted.active_document_page()
        if restarted_page is None:
            raise RuntimeError("0053 restart page missing")
        restart = restarted_page.stream.card_model.sort_mode
        restarted_page.sort_combo.setCurrentIndex(restarted_page.sort_combo.findData("recency"))
        restarted.persist_open_page_ui_states()
        resaved = store.load_document_ui_state("sort-document")
        if resaved is None:
            raise RuntimeError("0053 recency state missing")
        second_restart = MainWindow(repositories, store, settings=settings, window_id="sort-window")
        second_page = second_restart.active_document_page()
        if second_page is None:
            raise RuntimeError("0053 second restart page missing")
        lines.append(
            f"WTL-W2-0053|initial={initial}|stored={stored.sort_mode}|restart={restart}"
            f"|resaved={resaved.sort_mode}|second_restart={second_page.stream.card_model.sort_mode}"
        )
        for window in (first, restarted, second_restart):
            if not window.close():
                raise RuntimeError("0053 accepted close was rejected")
        application.processEvents(); database.close()

        input_text, output_text, rewrite_text = manager_case(
            directory, "0081", ("legacy-a", "legacy-b"),
            (("legacy-window-1", ("legacy-a", "legacy-b"), "legacy-a"),
             ("legacy-window-2", ("legacy-b",), "legacy-b")),
        )
        lines.append(f"WTL-W2-0081|input={input_text}|output={output_text}|rewrites={rewrite_text}")

        input_text, output_text, rewrite_text = manager_case(
            directory, "0082", ("claimed-a", "unclaimed-b"),
            (("claimed-window-1", ("claimed-a",), "claimed-a"),
             ("claimed-window-2", ("claimed-a", "unclaimed-b"), "claimed-a")),
        )
        lines.append(f"WTL-W2-0082|input={input_text}|output={output_text}|rewrites={rewrite_text}")

        if len(lines) != 4:
            raise RuntimeError(f"expected 4 golden lines, got {len(lines)}")
        args.output.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
