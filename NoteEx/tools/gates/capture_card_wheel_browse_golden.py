from __future__ import annotations

import argparse
import os
import sys
import tempfile
from collections.abc import Iterator
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
    from PySide6.QtCore import QEventLoop, QPoint, QPointF, QSettings, Qt, QTimer
    from PySide6.QtGui import QWheelEvent
    from PySide6.QtWidgets import QApplication
    from pynote.app import initialize_device_settings
    from pynote.application.card_service import CardService
    from pynote.domain.models import Document
    from pynote.infrastructure.database import Database
    from pynote.infrastructure.repositories import Repositories
    from pynote.ui.cards.card_stream import BROWSE_OPEN_DELAY_MS, CardStreamView
    from pynote.ui.document_page import DocumentPage

    resolved_package = Path(pynote.__file__).resolve()
    if not resolved_package.is_relative_to(source_root):
        raise RuntimeError(f"pynote import escaped source root: {resolved_package}")

    application = QApplication.instance() or QApplication([])
    databases: list[Database] = []
    pages: list[DocumentPage] = []
    fixture_number = 0
    lines: list[str] = []

    def identifiers() -> Iterator[str]:
        number = 0
        while True:
            number += 1
            yield f"operation-{number}"
            yield f"wheel-{number}"
            yield f"revision-{number}"
            yield f"event-{number}"

    def fixture(directory: Path, count: int) -> tuple[DocumentPage, tuple[object, ...]]:
        nonlocal fixture_number
        fixture_number += 1
        database = Database(directory / f"wheel-{fixture_number}.sqlite3")
        databases.append(database)
        repositories = Repositories(database)
        document = Document(
            id=f"wheel-document-{fixture_number}", title="휠 탐색",
            created_at_us=1_000_000, updated_at_us=1_000_000,
        )
        repositories.create_document(document)
        ids = identifiers()
        times = iter(range(2_000_000, 2_000_000 + count + 20))
        service = CardService(
            database, repositories, clock=lambda: next(times), id_factory=lambda: next(ids),
        )
        cards = tuple(service.create_card(document.id, f"카드 {number}") for number in range(1, count + 1))
        settings = QSettings(str(directory / f"wheel-{fixture_number}.ini"), QSettings.Format.IniFormat)
        initialize_device_settings(settings)
        settings.setValue("first_run/guide_shown", True)
        page = DocumentPage(database, repositories, document.id, settings=settings)
        pages.append(page)
        page.resize(900, 700)
        page.show()
        application.processEvents()
        page.stream.set_sort_mode("capture")
        return page, cards

    def wheel(stream: CardStreamView, angle: int) -> QWheelEvent:
        position = QPointF(stream.viewport().rect().center())
        event = QWheelEvent(
            position, position, QPoint(0, 0), QPoint(0, angle),
            Qt.MouseButton.NoButton, Qt.KeyboardModifier.NoModifier,
            Qt.ScrollPhase.NoScrollPhase, False,
        )
        application.sendEvent(stream.viewport(), event)
        application.processEvents()
        return event

    def wait(milliseconds: int) -> None:
        loop = QEventLoop()
        QTimer.singleShot(milliseconds, loop.quit)
        loop.exec()

    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)

        page, _cards = fixture(directory, 10)
        stream = page.stream
        stream.setCurrentIndex(stream.card_model.index(3))
        down: list[int] = []
        up: list[int] = []
        for _ in range(3):
            wheel(stream, -120)
            down.append(stream.currentIndex().row())
        down_selected = ",".join(stream.selected_card_ids())
        for _ in range(3):
            wheel(stream, 120)
            up.append(stream.currentIndex().row())
        lines.append(
            f"WTL-W2-0044|down={','.join(map(str, down))}|down_selected={down_selected}"
            f"|up={','.join(map(str, up))}|up_selected={','.join(stream.selected_card_ids())}"
        )
        stream.cancel_pending_browse()

        page, _cards = fixture(directory, 5)
        stream = page.stream
        stream.setCurrentIndex(stream.card_model.index(4))
        wheel(stream, -120)
        bottom = f"{stream.currentIndex().row()},{','.join(stream.selected_card_ids())}"
        stream.setCurrentIndex(stream.card_model.index(0))
        wheel(stream, 120)
        top = f"{stream.currentIndex().row()},{','.join(stream.selected_card_ids())}"
        lines.append(f"WTL-W2-0045|bottom={bottom}|top={top}|wrapped=0")
        stream.cancel_pending_browse()

        page, _cards = fixture(directory, 10)
        stream = page.stream
        stream.setCurrentIndex(stream.card_model.index(0))
        rows: list[int] = []
        remainders: list[int] = []
        for _ in range(6):
            wheel(stream, -40)
            rows.append(stream.currentIndex().row())
            remainders.append(stream._wheel_angle)
        lines.append(
            f"WTL-W2-0046|rows={','.join(map(str, rows))}"
            f"|remainders={','.join(map(str, remainders))}"
        )
        stream.cancel_pending_browse()

        page, _cards = fixture(directory, 10)
        stream = page.stream
        stream.setCurrentIndex(stream.card_model.index(4))
        wheel(stream, -40)
        wheel(stream, -40)
        before = f"{stream.currentIndex().row()},{stream._wheel_angle}"
        wheel(stream, 120)
        after = f"{stream.currentIndex().row()},{stream._wheel_angle}"
        lines.append(f"WTL-W2-0047|before={before}|after={after}|discarded=1")
        stream.cancel_pending_browse()

        page, _cards = fixture(directory, 12)
        stream = page.stream
        stream.setCurrentIndex(stream.card_model.index(0))
        opened: list[str] = []
        page.card_opened.connect(opened.append)
        for _ in range(4):
            wheel(stream, -120)
            wait(40)
        during = ",".join(opened) or "-"
        wait(BROWSE_OPEN_DELAY_MS + 40)
        opened_at_quiet = ",".join(opened) or "-"
        wait(BROWSE_OPEN_DELAY_MS * 3)
        lines.append(
            f"WTL-W2-0048|during={during}|opened={opened_at_quiet}"
            f"|count={len(opened)}|quiet_ms=120"
        )
        stream.cancel_pending_browse()

        page, _cards = fixture(directory, 12)
        stream = page.stream
        stream.setCurrentIndex(stream.card_model.index(0))
        opened = []
        page.card_opened.connect(opened.append)
        wheel(stream, -360)
        pending = stream._pending_browse_card_id
        page.refresh()
        wait(BROWSE_OPEN_DELAY_MS * 3)
        lines.append(
            f"WTL-W2-0049|pending={pending or '-'}|cancel=reset"
            f"|opened={','.join(opened) or '-'}|editor={page.editor.card_id or '-'}"
        )
        stream.cancel_pending_browse()

        page, cards = fixture(directory, 12)
        stream = page.stream
        first_id = str(cards[0].id)
        if not page.open_card(first_id):
            raise RuntimeError("failed to establish editor card for WTL-W2-0050")
        application.processEvents()
        page.editor_workspace.open_card = lambda *_args, **_kwargs: False  # type: ignore[method-assign]
        wheel(stream, -480)
        browse_row = stream.currentIndex().row()
        wait(BROWSE_OPEN_DELAY_MS + 40)
        focus = "editor" if page.focusWidget() is page.editor else "other"
        lines.append(
            f"WTL-W2-0050|browse_row={browse_row}|current_row={stream.currentIndex().row()}"
            f"|selected={','.join(stream.selected_card_ids())}|editor={page.editor.card_id or '-'}|focus={focus}"
        )
        stream.cancel_pending_browse()

        page, _cards = fixture(directory, 0)
        stream = page.stream
        requested: list[str] = []
        stream.card_browse_requested.connect(requested.append)
        event = wheel(stream, -360)
        wait(BROWSE_OPEN_DELAY_MS * 3)
        lines.append(
            f"WTL-W2-0051|handled={int(event.isAccepted())}"
            f"|requested={','.join(requested) or '-'}"
            f"|current={'-' if not stream.currentIndex().isValid() else stream.currentIndex().row()}"
        )

        if len(lines) != 8:
            raise RuntimeError(f"expected 8 golden lines, got {len(lines)}")
        args.output.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")

        for page in pages:
            page.stream.cancel_pending_browse()
            if page.editor.session is not None:
                page.editor.request_close()
            page.close()
        application.processEvents()
        for database in databases:
            database.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
