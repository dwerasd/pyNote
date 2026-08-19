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
    from PySide6.QtCore import QSettings, Qt
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QApplication
    from pynote.app import SqliteWorkspaceStateStore, initialize_device_settings
    from pynote.application import document_service
    from pynote.application.card_service import CardService
    from pynote.domain.events import EventType
    from pynote.domain.models import CaptureOperationSource, Document
    from pynote.infrastructure.database import Database
    from pynote.infrastructure.repositories import Repositories
    from pynote.ui.document_page import DocumentPage
    from pynote.ui.main_window import MainWindow

    def verify_imports() -> None:
        for name, module in tuple(sys.modules.items()):
            if name != "pynote" and not name.startswith("pynote."):
                continue
            module_file = getattr(module, "__file__", None)
            if module_file is not None and not Path(module_file).resolve().is_relative_to(source_root):
                raise RuntimeError(f"pynote import escaped source root: {name}={module_file}")

    verify_imports()
    if not Path(pynote.__file__).resolve().is_relative_to(source_root):
        raise RuntimeError(f"pynote import escaped source root: {pynote.__file__}")

    application = QApplication.instance() or QApplication([])
    lines: list[str] = []
    resources: list[tuple[Any, Database]] = []
    fixture_number = 0

    def identifiers() -> Iterator[str]:
        number = 0
        while True:
            number += 1
            yield f"operation-{number}"
            yield f"card-{number}"
            yield f"revision-{number}"
            yield f"event-{number}"

    class Fixture:
        def __init__(self, directory: Path, name: str) -> None:
            nonlocal fixture_number
            fixture_number += 1
            self.database = Database(directory / f"{name}-{fixture_number}.sqlite3")
            self.repositories = Repositories(self.database)
            self.document_id = f"document-{name}"
            self.repositories.create_document(Document(
                id=self.document_id, title=name, created_at_us=1_000, updated_at_us=1_000,
            ))
            self.settings = QSettings(str(directory / f"{name}-{fixture_number}.ini"), QSettings.Format.IniFormat)
            initialize_device_settings(self.settings)
            self.settings.setValue("first_run/guide_shown", True)
            self.ids = identifiers()

        def next_id(self) -> str:
            return next(self.ids)

        def install_service(self, page: DocumentPage) -> None:
            service = CardService(
                self.database, self.repositories, clock=lambda: 2_000, id_factory=self.next_id,
            )
            page.card_service = service
            page.editor._card_service = service

        def page(self) -> DocumentPage:
            page = DocumentPage(
                self.database, self.repositories, self.document_id, settings=self.settings,
            )
            self.install_service(page)
            page.resize(900, 600)
            page.show(); page.editor.setFocus(); application.processEvents()
            resources.append((page, self.database))
            return page

    def paste(page: DocumentPage, text: str) -> None:
        QApplication.clipboard().setText(text)
        page.editor.setFocus()
        QTest.keyClick(page.editor, Qt.Key.Key_V, Qt.KeyboardModifier.ControlModifier)
        application.processEvents()

    def typing(page: DocumentPage, text: str) -> None:
        page.editor.setFocus(); QTest.keyClicks(page.editor, text); application.processEvents()

    def provenance(fixture: Fixture, card: Any) -> tuple[str, str, str]:
        operation = fixture.repositories.get_capture_operation(card.operation_id)
        if operation is None:
            raise RuntimeError("capture operation missing")
        events = tuple(event for event in fixture.repositories.list_events(fixture.document_id)
                       if event.card_id == card.id and event.event_type is EventType.CREATE)
        if len(events) != 1:
            raise RuntimeError(f"expected one create event, got {len(events)}")
        return card.source.value, operation.source.value, events[0].source.value

    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)

        fixture = Fixture(directory, "0098"); page = fixture.page()
        typing(page, "a")
        cards_before_later = fixture.repositories.list_cards(fixture.document_id)
        paste(page, "b")
        cards = fixture.repositories.list_cards(fixture.document_id)
        card_source, operation_source, event_source = provenance(fixture, cards[0])
        later_effects = max(0, len(cards) - len(cards_before_later)) * 7
        lines.append(
            f"WTL-W2-0098|first=typing|later=paste|card_source={card_source}|operation_source={operation_source}|event_source={event_source}|surface={page.editor.toPlainText().encode().hex()}|cards={len(cards)}|later_effects={later_effects}"
        )

        fixture = Fixture(directory, "0099"); page = fixture.page()
        attempts: list[CaptureOperationSource] = []
        actual_create = page.card_service.create_cards
        def fail_paste_once(*values: Any, source: CaptureOperationSource, **keywords: Any) -> tuple[Any, ...]:
            attempts.append(source)
            if len(attempts) == 1:
                return ()
            return actual_create(*values, source=source, **keywords)
        page.card_service.create_cards = fail_paste_once  # type: ignore[method-assign]
        paste(page, "p"); typing(page, "t")
        card = fixture.repositories.list_cards(fixture.document_id)[0]
        card_source, operation_source, event_source = provenance(fixture, card)
        lines.append(
            f"WTL-W2-0099|failed=paste|retry=typing|attempts={','.join(value.value for value in attempts)}|body={card.body.encode().hex()}|card_source={card_source}|operation_source={operation_source}|event_source={event_source}|scoped={int(attempts == [CaptureOperationSource.PASTE, CaptureOperationSource.TYPING])}"
        )

        fixture = Fixture(directory, "0100"); page = fixture.page()
        attempts = []
        actual_create = page.card_service.create_cards
        def fail_typing_once(*values: Any, source: CaptureOperationSource, **keywords: Any) -> tuple[Any, ...]:
            attempts.append(source)
            if len(attempts) == 1:
                return ()
            return actual_create(*values, source=source, **keywords)
        page.card_service.create_cards = fail_typing_once  # type: ignore[method-assign]
        typing(page, "t"); paste(page, "p")
        card = fixture.repositories.list_cards(fixture.document_id)[0]
        card_source, operation_source, event_source = provenance(fixture, card)
        lines.append(
            f"WTL-W2-0100|failed=typing|retry=paste|attempts={','.join(value.value for value in attempts)}|body={card.body.encode().hex()}|card_source={card_source}|operation_source={operation_source}|event_source={event_source}|scoped={int(attempts == [CaptureOperationSource.TYPING, CaptureOperationSource.PASTE])}"
        )

        fixture = Fixture(directory, "0101")
        published: list[str] = []
        window = MainWindow(
            fixture.repositories, SqliteWorkspaceStateStore(fixture.database),
            settings=fixture.settings, document_change_publisher=published.append,
        )
        window.resize(1_000, 700); window.show()
        if not window.open_document_local(fixture.document_id):
            raise RuntimeError("failed to open consumer document")
        page = window.active_document_page()
        if page is None:
            raise RuntimeError("consumer page missing")
        fixture.install_service(page)
        resources.append((window, fixture.database))
        created: list[Any] = []
        connected: list[str] = []
        content_changes: list[bool] = []
        opened: list[str] = []
        history_refreshes: list[bool] = []
        history_cards: list[str] = []
        page.editor.card_created.connect(created.append)
        page.editor.card_connected.connect(connected.append)
        page.content_changed.connect(lambda: content_changes.append(True))
        page.card_opened.connect(opened.append)
        page.history.refresh = lambda: history_refreshes.append(True)  # type: ignore[method-assign]
        page.history.set_card = history_cards.append  # type: ignore[method-assign]
        page.history.set_pending_card = history_cards.append  # type: ignore[method-assign]
        paste(page, "소비자 갱신")
        cards = fixture.repositories.list_cards(fixture.document_id)
        card = cards[0]
        projection_count = int(page.stream.card_model.index_for_card(card.id).isValid())
        initial_counts = tuple(map(len, (created, connected, content_changes, opened,
                                         history_refreshes, history_cards, published)))
        typing(page, "!")
        repeat_counts = tuple(map(len, (created, connected, content_changes, opened,
                                        history_refreshes, history_cards, published)))
        repeat_effects = sum(after - before for before, after in zip(initial_counts, repeat_counts, strict=True))
        if "1개 카드" not in window.statusBar().currentMessage():
            raise RuntimeError("status consumer did not update")
        lines.append(
            f"WTL-W2-0101|card={card.id}|body={card.body.encode().hex()}|created={initial_counts[0]}|connected={initial_counts[1]}|content_changed={initial_counts[2]}|opened={initial_counts[3]}|history_refresh={initial_counts[4]}|history_card={initial_counts[5]}|published={initial_counts[6]}|projection={projection_count}|repeat_effects={repeat_effects}"
        )

        verify_imports()
        if len(lines) != 4:
            raise RuntimeError(f"expected 4 golden lines, got {len(lines)}")
        args.output.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")

        closed: set[int] = set()
        for widget, database in resources:
            widget.close(); application.processEvents()
            identity = id(database)
            if identity not in closed:
                database.close(); closed.add(identity)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
