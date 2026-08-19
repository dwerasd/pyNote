from __future__ import annotations

import argparse
import os
import sys
import tempfile
from collections.abc import Iterator
from pathlib import Path
from typing import Any


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    source_root = args.source_root.resolve()
    os.environ["QT_QPA_PLATFORM"] = "offscreen"
    sys.path.insert(0, str(source_root / "src"))

    import pynote
    from PySide6.QtCore import QMimeData, QPointF, QSettings, Qt, QUrl
    from PySide6.QtGui import QDragEnterEvent, QDragMoveEvent, QDropEvent
    from PySide6.QtWidgets import QApplication
    from pynote.app import initialize_device_settings
    from pynote.application import document_service
    from pynote.application.card_service import CardService
    from pynote.domain.models import CaptureOperationSource, CardSource
    from pynote.infrastructure.database import Database
    from pynote.infrastructure.repositories import Repositories
    from pynote.ui.cards.card_model import CardRole
    from pynote.ui.document_page import DocumentPage

    package = Path(pynote.__file__).resolve()
    if not package.is_relative_to(source_root):
        raise RuntimeError(f"pynote import escaped source root: {package}")
    application = QApplication.instance() or QApplication([])
    resources: list[tuple[DocumentPage, Database]] = []
    fixture_number = 0
    lines: list[str] = []

    def identifiers() -> Iterator[str]:
        number = 0
        while True:
            number += 1
            yield f"operation-{number}"
            yield f"card-{number}"
            yield f"revision-{number}"
            yield f"event-{number}"

    def fixture(directory: Path, with_base: bool, base_body: str = "기존 카드") -> tuple[DocumentPage, Repositories, list[tuple[str, str]]]:
        nonlocal fixture_number
        fixture_number += 1
        database = Database(directory / f"completion-{fixture_number}.sqlite3")
        repositories = Repositories(database)
        document = document_service.create_document(repositories, f"completion-{fixture_number}")
        settings = QSettings(str(directory / f"completion-{fixture_number}.ini"), QSettings.Format.IniFormat)
        initialize_device_settings(settings)
        settings.setValue("first_run/guide_shown", True)
        errors: list[tuple[str, str]] = []
        page = DocumentPage(database, repositories, document.id, settings=settings, error_reporter=lambda title, message: errors.append((title, message)))
        ids = identifiers()
        times = iter(range(2_000_000, 2_000_100))
        page.card_service = CardService(database, repositories, clock=lambda: next(times), id_factory=lambda: next(ids))
        if with_base:
            base = page.card_service.create_card(document.id, base_body, source=CaptureOperationSource.TYPING)
            if base.id != "card-1":
                raise RuntimeError(f"deterministic base ID mismatch: {base.id}")
            page.refresh()
        page.resize(900, 600)
        page.show()
        application.processEvents()
        resources.append((page, database))
        return page, repositories, errors

    def drop(page: DocumentPage, paths: list[Path]) -> None:
        mime = QMimeData()
        mime.setUrls([QUrl.fromLocalFile(str(path)) for path in paths])
        point = page.editor.cursorRect().center()
        entered = QDragEnterEvent(point, Qt.DropAction.CopyAction, mime, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier)
        application.sendEvent(page.editor.viewport(), entered)
        moved = QDragMoveEvent(point, Qt.DropAction.CopyAction, mime, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier)
        application.sendEvent(page.editor.viewport(), moved)
        dropped = QDropEvent(QPointF(point), Qt.DropAction.CopyAction, mime, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier)
        application.sendEvent(page.editor.viewport(), dropped)
        if not entered.isAccepted() or not moved.isAccepted() or not dropped.isAccepted():
            raise RuntimeError("actual Qt drop path rejected")
        application.processEvents()

    def imported(repositories: Repositories, document_id: str) -> tuple[Any, ...]:
        return tuple(card for card in repositories.list_cards(document_id) if card.source is CardSource.IMPORT)

    def current_id(page: DocumentPage) -> str:
        value = page.stream.currentIndex().data(CardRole.CARD_ID)
        return str(value) if value is not None else "-"

    def run_visible(directory: Path, node: str, mode: str) -> str:
        page, repositories, _errors = fixture(directory, True, "기존 첫 카드")
        page.stream.set_sort_mode(mode)
        first, second = directory / f"{node}-first.txt", directory / f"{node}-second.txt"
        first.write_text("정렬 첫 카드", encoding="utf-8")
        second.write_text("정렬 대상 카드", encoding="utf-8")
        scroll_targets: list[str] = []
        original_scroll = page.stream.scrollTo

        def scroll(index: Any, *values: Any) -> None:
            scroll_targets.append(str(index.data(CardRole.CARD_ID)))
            original_scroll(index, *values)

        page.stream.scrollTo = scroll  # type: ignore[method-assign]
        drop(page, [first, second])
        created = imported(repositories, page.document_id)
        target = created[-1].id
        row = page.stream.card_model.index_for_card(target).row()
        reveal = int(target in scroll_targets)
        return f"sort={mode}|created={','.join(card.id for card in created)}|target={target}|target_row={row}|current={current_id(page)}|reveal={reveal}|editor={page.editor.card_id or '-'}"

    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)

        page, repositories, _errors = fixture(directory, True, "타이핑 카드")
        page.stream.set_sort_mode("recency")
        typing_index = page.source_filter.findData("typing")
        page.source_filter.setCurrentIndex(typing_index)
        base_index = page.stream.card_model.index_for_card("card-1")
        page.stream.setCurrentIndex(base_index)
        first, second = directory / "hidden-first.txt", directory / "hidden-second.txt"
        first.write_text("숨은 첫 카드", encoding="utf-8")
        second.write_text("숨은 마지막 카드", encoding="utf-8")
        scroll_targets: list[str] = []
        original_scroll = page.stream.scrollTo
        def hidden_scroll(index: Any, *values: Any) -> None:
            scroll_targets.append(str(index.data(CardRole.CARD_ID))); original_scroll(index, *values)
        page.stream.scrollTo = hidden_scroll  # type: ignore[method-assign]
        drop(page, [first, second])
        created = imported(repositories, page.document_id); target = created[-1].id
        lines.append(
            f"WTL-W2-0072|created={','.join(card.id for card in created)}|target={target}"
            f"|target_visible={int(page.stream.card_model.index_for_card(target).isValid())}"
            f"|current={current_id(page)}|visible_rows={page.stream.card_model.rowCount()}"
            f"|filter={page.source_filter.currentData()}|reveal={int(target in scroll_targets)}|editor={page.editor.card_id or '-'}"
        )

        page, repositories, errors = fixture(directory, False)
        page.stream.set_sort_mode("recency")
        first, second, blank = directory / "blank-last-first.txt", directory / "blank-last-second.txt", directory / "blank-last.txt"
        first.write_text("첫 성공 카드", encoding="utf-8"); second.write_text("마지막 성공 카드", encoding="utf-8"); blank.write_bytes(b" \r\n\t")
        scroll_targets = []; original_scroll = page.stream.scrollTo
        def blank_scroll(index: Any, *values: Any) -> None:
            scroll_targets.append(str(index.data(CardRole.CARD_ID))); original_scroll(index, *values)
        page.stream.scrollTo = blank_scroll  # type: ignore[method-assign]
        drop(page, [first, second, blank]); created = imported(repositories, page.document_id); target = created[-1].id; row = page.stream.card_model.index_for_card(target).row()
        lines.append(
            f"WTL-W2-0073|created={','.join(card.id for card in created)}|final_input=blank|target={target}"
            f"|target_row={row}|current={current_id(page)}|read_failures={len(errors)}"
            f"|reveal={int(target in scroll_targets)}|editor={page.editor.card_id or '-'}"
        )

        lines.append("WTL-W2-0074|" + run_visible(directory, "0074", "recency"))
        lines.append("WTL-W2-0075|" + run_visible(directory, "0075", "position"))
        lines.append("WTL-W2-0076|" + run_visible(directory, "0076", "capture"))

        if len(lines) != 5:
            raise RuntimeError(f"expected 5 golden lines, got {len(lines)}")
        args.output.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
        for page, database in resources:
            if page.editor.session is not None:
                page.editor.request_close()
            page.close(); database.close()
        application.processEvents()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
