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
    from pynote.application import document_service
    from pynote.application.card_service import CardService
    from pynote.domain.models import CaptureOperationSource, CardSource
    from pynote.infrastructure.database import Database
    from pynote.infrastructure.repositories import Repositories
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

    def fixture(directory: Path) -> tuple[DocumentPage, Repositories, list[int]]:
        nonlocal fixture_number
        fixture_number += 1
        database = Database(directory / f"first-input-{fixture_number}.sqlite3")
        repositories = Repositories(database)
        document = document_service.create_document(repositories, f"first-input-{fixture_number}")
        settings = QSettings(str(directory / f"first-input-{fixture_number}.ini"), QSettings.Format.IniFormat)
        initialize_device_settings(settings)
        settings.setValue("first_run/guide_shown", True)
        page = DocumentPage(database, repositories, document.id, settings=settings)
        id_values = identifiers()
        times = iter(range(2_000_000, 2_000_100))
        service = CardService(database, repositories, clock=lambda: next(times), id_factory=lambda: next(id_values))
        attempts: list[int] = []
        actual_create_cards = service.create_cards

        def observed_create_cards(*values: Any, **keywords: Any) -> tuple[Any, ...]:
            attempts.append(1)
            return actual_create_cards(*values, **keywords)

        service.create_cards = observed_create_cards  # type: ignore[method-assign]
        page.card_service = service
        page.editor._card_service = service
        page.resize(900, 600)
        page.show()
        page.editor.setFocus()
        application.processEvents()
        resources.append((page, database))
        return page, repositories, attempts

    def paste(page: DocumentPage, text: str) -> None:
        QApplication.clipboard().setText(text)
        page.editor.setFocus()
        QTest.keyClick(page.editor, Qt.Key.Key_V, Qt.KeyboardModifier.ControlModifier)
        application.processEvents()

    def type_text(page: DocumentPage, text: str) -> None:
        page.editor.setFocus()
        QTest.keyClicks(page.editor, text)
        application.processEvents()

    def operation_ids(repositories: Repositories, cards: tuple[Any, ...]) -> str:
        values = []
        for card in cards:
            operation = repositories.get_capture_operation(card.operation_id)
            if operation is None:
                raise RuntimeError(f"missing operation: {card.operation_id}")
            values.append(operation.id)
        return ",".join(values)

    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)

        page, repositories, _attempts = fixture(directory)
        for index, body in enumerate(("첫 수집", "둘째 수집", "셋째 수집")):
            paste(page, body)
            if index != 2 and not page.editor.request_close():
                raise RuntimeError("accepted close was rejected")
        cards = repositories.list_cards(page.document_id)
        lines.append(
            "WTL-W2-0077"
            f"|cards={','.join(card.id for card in cards)}"
            f"|bodies={','.join(card.body.encode().hex() for card in cards)}"
            f"|sources={','.join(card.source.value for card in cards)}"
            f"|operations={operation_ids(repositories, cards)}"
            f"|connected={page.editor.session.card_id if page.editor.session is not None else '-'}"
        )

        page, repositories, _attempts = fixture(directory)
        type_text(page, "first")
        if not page.editor.request_close():
            raise RuntimeError("accepted close was rejected")
        type_text(page, "second")
        if not page.editor.request_close():
            raise RuntimeError("accepted close was rejected")
        stored = repositories.list_cards(page.document_id)
        displayed = tuple(
            card
            for row in range(page.stream.card_model.rowCount())
            if (card := page.stream.card_model.card_at(page.stream.card_model.index(row, 0))) is not None
        )
        lines.append(
            "WTL-W2-0078"
            f"|stored={','.join(card.body for card in stored)}"
            f"|recency={','.join(card.body for card in displayed)}"
            f"|positions={','.join(str(card.position_key) for card in stored)}"
            f"|operations={operation_ids(repositories, stored)}"
        )

        page, repositories, _attempts = fixture(directory)
        page.source_filter.setCurrentIndex(page.source_filter.findData("typing"))
        paste(page, "필터 밖 붙여넣기")
        cards = repositories.list_cards(page.document_id)
        card = cards[0]
        connected = page.editor.session.card_id if page.editor.session is not None else "-"
        row = page.stream.card_model.index_for_card(card.id)
        lines.append(
            "WTL-W2-0079"
            f"|filter={page.source_filter.currentData()}|card={card.id}|source={card.source.value}"
            f"|connected={connected}|row={row.row() if row.isValid() else '-'}"
        )

        page, repositories, attempts = fixture(directory)
        type_text(page, "a")
        paste(page, "b")
        cards = repositories.list_cards(page.document_id)
        card = cards[0]
        operation = repositories.get_capture_operation(card.operation_id)
        if operation is None:
            raise RuntimeError(f"missing operation: {card.operation_id}")
        lines.append(
            "WTL-W2-0080"
            f"|card={card.id}|surface={page.editor.toPlainText().encode().hex()}"
            f"|card_source={card.source.value}|operation_source={operation.source.value}"
            f"|cards={len(cards)}|create_attempts={len(attempts)}"
        )

        if len(lines) != 4:
            raise RuntimeError(f"expected 4 golden lines, got {len(lines)}")
        args.output.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")

        for page, database in resources:
            if page.editor.session is not None:
                page.editor.request_close()
            page.close()
            database.close()
        application.processEvents()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
