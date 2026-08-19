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
    from pynote.app import initialize_device_settings
    from pynote.application.card_service import CardService
    from pynote.domain.models import CaptureOperationSource, CardSource, Document, Draft, DraftKind
    from pynote.infrastructure.database import Database
    from pynote.infrastructure.repositories import Repositories, text_hash
    from pynote.ui.document_page import DocumentPage

    for name, module in tuple(sys.modules.items()):
        if name != "pynote" and not name.startswith("pynote."):
            continue
        module_file = getattr(module, "__file__", None)
        if module_file is not None and not Path(module_file).resolve().is_relative_to(source_root):
            raise RuntimeError(f"pynote import escaped source root: {name}={module_file}")
    if not Path(pynote.__file__).resolve().is_relative_to(source_root):
        raise RuntimeError(f"pynote import escaped source root: {pynote.__file__}")

    application = QApplication.instance() or QApplication([])
    lines: list[str] = []
    resources: list[tuple[DocumentPage, Database]] = []
    fixture_number = 0

    def utf16(text: str) -> int:
        return len(text.encode("utf-16-le")) // 2

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
            self.override_ids: list[str] = []

        def next_id(self) -> str:
            return self.override_ids.pop(0) if self.override_ids else next(self.ids)

        def page(self, clock: int = 2_000) -> DocumentPage:
            page = DocumentPage(self.database, self.repositories, self.document_id, settings=self.settings)
            service = CardService(
                self.database, self.repositories, clock=lambda: clock, id_factory=self.next_id,
            )
            page.card_service = service
            page.editor._card_service = service
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

    def phase(page: DocumentPage) -> str:
        if page.editor.session is not None:
            return "connected"
        return "pending-link" if page.editor._pending_card_id is not None else "awaiting"

    def active_count(cards: tuple[Any, ...]) -> int:
        return sum(card.deleted_at_us is None for card in cards)

    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)

        fixture = Fixture(directory, "0083"); page = fixture.page(); typing(page, "a")
        cards = fixture.repositories.list_cards(fixture.document_id)
        lines.append(f"WTL-W2-0083|event=insertion|source=typing|text={page.editor.toPlainText().encode().hex()}|cards={len(cards)}|active={active_count(cards)}|phase={phase(page)}|connected={page.editor.card_id or '-'}")

        fixture = Fixture(directory, "0084"); page = fixture.page(); paste(page, "붙여넣기")
        cards = fixture.repositories.list_cards(fixture.document_id)
        lines.append(f"WTL-W2-0084|event=insertion|source=paste|text={page.editor.toPlainText().encode().hex()}|cards={len(cards)}|active={active_count(cards)}|phase={phase(page)}|connected={page.editor.card_id or '-'}")

        fixture = Fixture(directory, "0085"); page = fixture.page(); paste(page, " \n\t")
        cards = fixture.repositories.list_cards(fixture.document_id)
        lines.append(f"WTL-W2-0085|event=insertion|source=paste|text={page.editor.toPlainText().encode().hex()}|cards={len(cards)}|active={active_count(cards)}|phase={phase(page)}|backing={'new' if page.editor._new_session is not None else '-'}")

        fixture = Fixture(directory, "0086"); page = fixture.page(); creates: list[int] = []
        actual_create = page.card_service.create_cards
        def observed_create(*values: Any, **keywords: Any) -> tuple[Any, ...]:
            creates.append(1); return actual_create(*values, **keywords)
        page.card_service.create_cards = observed_create  # type: ignore[method-assign]
        typing(page, "a"); typing(page, "bc"); cards = fixture.repositories.list_cards(fixture.document_id)
        lines.append(f"WTL-W2-0086|inputs=61,6263|surface={page.editor.toPlainText().encode().hex()}|cards={len(cards)}|creates={len(creates)}|phase={phase(page)}|connected={page.editor.card_id or '-'}")

        fixture = Fixture(directory, "0087")
        legacy = "legacy NEW 본문"
        fixture.repositories.create_draft(Draft(id="legacy-new", document_id=fixture.document_id, card_id=None,
            draft_kind=DraftKind.NEW, base_revision_id=None, draft_text=legacy, draft_hash=text_hash(legacy),
            cursor_position_qchar=7, updated_at_us=1_000))
        page = fixture.page(); cards = fixture.repositories.list_cards(fixture.document_id)
        lines.append(f"WTL-W2-0087|restore=draft-legacy|text={page.editor.toPlainText().encode().hex()}|cursor={page.editor.textCursor().position()}|cards={len(cards)}|phase={phase(page)}")

        fixture = Fixture(directory, "0088"); body = "복원문\n😀"
        fixture.repositories.create_draft(Draft(id="settings-new", document_id=fixture.document_id, card_id=None,
            draft_kind=DraftKind.NEW, base_revision_id=None, draft_text=body, draft_hash=text_hash(body),
            cursor_position_qchar=utf16(body), updated_at_us=2_000))
        fixture.settings.setValue("editor/line_spacing", 1.7)
        page = fixture.page(); page.apply_settings(); cards = fixture.repositories.list_cards(fixture.document_id)
        events = fixture.repositories.list_events(fixture.document_id)
        lines.append(f"WTL-W2-0088|restore=draft-settings|text={page.editor.toPlainText().encode().hex()}|cursor={page.editor.textCursor().position()}|cards={len(cards)}|events={len(events)}|phase={phase(page)}")

        for node, failure, source, body in (("0089", "return", "paste", "paste 실패 😀"), ("0090", "exception", "typing", "A")):
            fixture = Fixture(directory, node); page = fixture.page()
            def failed_create(*_values: Any, current=failure, **_keywords: Any) -> tuple[Any, ...]:
                if current == "exception": raise RuntimeError("injected create")
                return ()
            page.card_service.create_cards = failed_create  # type: ignore[method-assign]
            paste(page, body) if source == "paste" else typing(page, body)
            protected = int(page.protect_now())
            before = len(fixture.repositories.list_cards(fixture.document_id))
            restarted = fixture.page(); after = len(fixture.repositories.list_cards(fixture.document_id))
            lines.append(f"WTL-W2-{node}|failure={failure}|source={source}|text={restarted.editor.toPlainText().encode().hex()}|cards={before}|protected={protected}|restart_cards={after}|phase={phase(restarted)}")

        fixture = Fixture(directory, "0091"); page = fixture.page(); calls: list[CaptureOperationSource] = []
        actual_create = page.card_service.create_cards
        def fail_once(*values: Any, source: CaptureOperationSource, **keywords: Any) -> tuple[Any, ...]:
            calls.append(source)
            if len(calls) == 1: return ()
            return actual_create(*values, source=source, **keywords)
        page.card_service.create_cards = fail_once  # type: ignore[method-assign]
        paste(page, "ABC")
        QTest.keyClick(page.editor, Qt.Key.Key_Backspace)
        QTest.keyClick(page.editor, Qt.Key.Key_Z, Qt.KeyboardModifier.ControlModifier)
        page.editor.apply_line_spacing(1.5); calls_before = len(calls)
        typing(page, "D"); cards = fixture.repositories.list_cards(fixture.document_id)
        lines.append(f"WTL-W2-0091|first=paste-return|noninsert=delete,undo,format|calls_before_retry={calls_before}|retry=typing|surface={page.editor.toPlainText().encode().hex()}|cards={len(cards)}|source={cards[0].source.value}")

        for node, failure in (("0092", "return"), ("0093", "exception")):
            fixture = Fixture(directory, node); page = fixture.page(); attempts: list[str] = []
            actual_open = page.draft_coordinator.open_card
            def fail_link(card: Any, *, disposition: Any = None, current=failure) -> Any:
                attempts.append(str(card.id))
                if len(attempts) == 1:
                    if current == "exception": raise RuntimeError("injected link")
                    return None
                return actual_open(card, disposition=disposition)
            page.draft_coordinator.open_card = fail_link  # type: ignore[method-assign]
            typing(page, "a"); typing(page, "b"); cards = fixture.repositories.list_cards(fixture.document_id)
            lines.append(f"WTL-W2-{node}|failure={failure}|attempts={','.join(attempts)}|cards={len(cards)}|surface={page.editor.toPlainText().encode().hex()}|phase={phase(page)}|connected={page.editor.card_id or '-'}")

        fixture = Fixture(directory, "0094"); page = fixture.page(); attempts = []
        def always_fail(card: Any, *, disposition: Any = None) -> None:
            del disposition; attempts.append(str(card.id)); return None
        page.draft_coordinator.open_card = always_fail  # type: ignore[method-assign]
        typing(page, "a"); typing(page, "b"); typing(page, "c")
        pending = page.editor._pending_card_id
        if pending is None: raise RuntimeError("0094 pending card missing")
        fixture.override_ids.append("delete-event")
        page.card_service.soft_delete(pending); typing(page, "d")
        cards = fixture.repositories.list_cards(fixture.document_id)
        deleted = next(card.id for card in cards if card.deleted_at_us is not None)
        lines.append(f"WTL-W2-0094|attempts={','.join(attempts)}|cards={len(cards)}|active={active_count(cards)}|deleted={deleted}|pending={page.editor._pending_card_id or '-'}|phase={phase(page)}")

        for node, failure in (("0095", "return"), ("0096", "exception")):
            fixture = Fixture(directory, node); page = fixture.page(); base = "재기동 경계"
            def failed_link(_card: Any, *, disposition: Any = None, current=failure) -> None:
                del disposition
                if current == "exception": raise RuntimeError("injected link")
                return None
            page.draft_coordinator.open_card = failed_link  # type: ignore[method-assign]
            paste(page, base); before = len(fixture.repositories.list_cards(fixture.document_id)); protected = int(page.protect_now())
            restarted = fixture.page(); typing(restarted, "!")
            cards = fixture.repositories.list_cards(fixture.document_id)
            lines.append(f"WTL-W2-{node}|failure={failure}|before_cards={before}|protected={protected}|restart_surface={restarted.editor.toPlainText().encode().hex()}|after_cards={len(cards)}|bodies={','.join(card.body.encode().hex() for card in cards)}|phase={phase(restarted)}")

        fixture = Fixture(directory, "0097"); page = fixture.page(); prefix = " \n\t"; paste(page, prefix)
        if not page.protect_now(): raise RuntimeError("0097 protect failed")
        restarted = fixture.page(clock=88_888); typing(restarted, "A")
        card = fixture.repositories.list_cards(fixture.document_id)[0]
        lines.append(f"WTL-W2-0097|restored={prefix.encode().hex()}|promoted={card.body.encode().hex()}|created_at={card.created_at_us}|capture_seq={card.capture_seq}|source={card.source.value}|cursor={restarted.editor.textCursor().position()}|cards=1")

        if len(lines) != 15:
            raise RuntimeError(f"expected 15 golden lines, got {len(lines)}")
        args.output.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")

        closed: set[int] = set()
        for page, database in resources:
            page.close(); application.processEvents()
            identity = id(database)
            if identity not in closed:
                database.close(); closed.add(identity)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
