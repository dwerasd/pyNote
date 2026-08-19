from __future__ import annotations

import argparse
import json
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
    from PySide6.QtCore import QItemSelectionModel, QMimeData, QPoint, QPointF, QSettings, Qt
    from PySide6.QtGui import QDragEnterEvent, QDragMoveEvent, QDropEvent, QWheelEvent
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QAbstractItemView, QApplication, QWidget
    from pynote.app import initialize_device_settings
    from pynote.application.card_service import CardService
    from pynote.domain.models import Document
    from pynote.infrastructure.database import Database
    from pynote.infrastructure.repositories import Repositories
    from pynote.ui.cards.card_model import CardListModel, CardRole
    from pynote.ui.cards.card_stream import CardStreamView
    from pynote.ui.document_page import DocumentPage

    resolved_package = Path(pynote.__file__).resolve()
    if not resolved_package.is_relative_to(source_root):
        raise RuntimeError(f"pynote import escaped source root: {resolved_package}")

    application = QApplication.instance() or QApplication([])
    card_mime = "application/x-pynote-card-id"
    lines: list[str] = []
    databases: list[Database] = []
    pages: list[DocumentPage] = []
    fixture_number = 0

    def identifiers() -> Iterator[str]:
        number = 0
        while True:
            number += 1
            yield f"operation-{number}"
            yield f"multi-{number}"
            yield f"revision-{number}"
            yield f"event-{number}"

    def page_fixture(directory: Path, count: int, settings: QSettings | None = None) -> tuple[DocumentPage, tuple[object, ...]]:
        nonlocal fixture_number
        fixture_number += 1
        database = Database(directory / f"selection-{fixture_number}.sqlite3")
        databases.append(database)
        repositories = Repositories(database)
        document = Document(
            id=f"multi-document-{fixture_number}", title="다중 선택",
            created_at_us=1_000_000, updated_at_us=1_000_000,
        )
        repositories.create_document(document)
        ids = identifiers()
        times = iter(range(2_000_000, 2_000_000 + count + 20))
        service = CardService(
            database, repositories, clock=lambda: next(times), id_factory=lambda: next(ids),
        )
        cards = tuple(service.create_card(document.id, f"카드 {number}") for number in range(1, count + 1))
        page = DocumentPage(database, repositories, document.id, settings=settings)
        pages.append(page)
        page.resize(900, 700)
        page.show()
        application.processEvents()
        page.stream.set_sort_mode("capture")
        return page, cards

    def select_rows(page: DocumentPage, rows: tuple[int, ...]) -> None:
        for position, row in enumerate(rows):
            index = page.stream.card_model.index(row)
            page.stream.scrollTo(index)
            QTest.mouseClick(
                page.stream.viewport(), Qt.MouseButton.LeftButton,
                Qt.KeyboardModifier.NoModifier if position == 0 else Qt.KeyboardModifier.ControlModifier,
                page.stream.visualRect(index).center(),
            )
            application.processEvents()

    class SourceDragEnterEvent(QDragEnterEvent):
        def __init__(self, *event_args: object, source: QWidget) -> None:
            super().__init__(*event_args)  # type: ignore[arg-type]
            self._capture_source = source

        def source(self) -> QWidget:
            return self._capture_source

    class SourceDragMoveEvent(QDragMoveEvent):
        def __init__(self, *event_args: object, source: QWidget) -> None:
            super().__init__(*event_args)  # type: ignore[arg-type]
            self._capture_source = source

        def source(self) -> QWidget:
            return self._capture_source

    class SourceDropEvent(QDropEvent):
        def __init__(self, *event_args: object, source: QWidget) -> None:
            super().__init__(*event_args)  # type: ignore[arg-type]
            self._capture_source = source

        def source(self) -> QWidget:
            return self._capture_source

    def dispatch_drop(target: QWidget, mime_data: QMimeData, source: QWidget) -> tuple[bool, bool, bool]:
        position = QPoint(8, 8)
        actions = Qt.DropAction.CopyAction | Qt.DropAction.MoveAction
        enter = SourceDragEnterEvent(
            position, actions, mime_data, Qt.MouseButton.LeftButton,
            Qt.KeyboardModifier.NoModifier, source=source,
        )
        application.sendEvent(target, enter)
        move = SourceDragMoveEvent(
            position, actions, mime_data, Qt.MouseButton.LeftButton,
            Qt.KeyboardModifier.NoModifier, source=source,
        )
        application.sendEvent(target, move)
        drop = SourceDropEvent(
            QPointF(position), actions, mime_data, Qt.MouseButton.LeftButton,
            Qt.KeyboardModifier.NoModifier, source=source,
        )
        application.sendEvent(target, drop)
        return enter.isAccepted(), move.isAccepted(), drop.isAccepted()

    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)

        page, cards = page_fixture(directory, 2)
        first = cards[0]
        first_id = str(first.id)
        index = page.stream.card_model.index_for_card(first_id)
        QTest.mousePress(page.stream.viewport(), Qt.MouseButton.LeftButton, pos=page.stream.visualRect(index).center())
        page.stream._active_drag_token = 7
        page.delete_drop_zone.arm(7)
        mime_data = QMimeData()
        mime_data.setData(card_mime, json.dumps({
            "card_id": first_id,
            "revision_id": first.current_revision_id,
            "token": 7,
        }).encode())
        spoof_source = QWidget()
        deleted: list[str] = []
        moved: list[tuple[str, object]] = []
        page.stream.card_delete_dropped.connect(deleted.append)
        page.stream.card_move_requested.connect(lambda card_id, before: moved.append((card_id, before)))
        trash = dispatch_drop(page.delete_drop_zone, mime_data, spoof_source)
        reorder = dispatch_drop(page.stream.viewport(), mime_data, spoof_source)
        QTest.mouseRelease(page.stream.viewport(), Qt.MouseButton.LeftButton, pos=page.stream.visualRect(index).center())
        lines.append(
            "WTL-W2-0019|trash=" + ",".join(str(int(value)) for value in trash)
            + "|reorder=" + ",".join(str(int(value)) for value in reorder)
            + f"|deleted={','.join(deleted) or '-'}|moved={','.join(value[0] for value in moved) or '-'}"
        )

        settings = QSettings(str(directory / "multi-selection.ini"), QSettings.Format.IniFormat)
        initialize_device_settings(settings)
        settings.setValue("first_run/guide_shown", True)
        page, cards = page_fixture(directory, 6, settings)
        select_rows(page, (1, 2, 3))
        lines.append(
            "WTL-W2-0020"
            f"|default_key={int(settings.contains('cards/multi_selection_enabled'))}"
            f"|enabled={int(settings.value('cards/multi_selection_enabled', type=bool))}"
            f"|mode={'single' if page.stream.selectionMode() is QAbstractItemView.SelectionMode.SingleSelection else 'extended'}"
            f"|selected={','.join(page.stream.selected_card_ids())}"
        )

        bare = CardStreamView(CardListModel([]))
        mode = "single" if bare.selectionMode() is QAbstractItemView.SelectionMode.SingleSelection else "extended"
        lines.append(f"WTL-W2-0021|mode={mode}")
        bare.close()

        page, _cards = page_fixture(directory, 6)
        select_rows(page, (1, 2))
        lines.append(f"WTL-W2-0022|mode=single|selected={','.join(page.stream.selected_card_ids())}")

        page, _cards = page_fixture(directory, 6)
        page.stream.set_multi_selection_enabled(True)
        select_rows(page, (1, 2, 3))
        lines.append(f"WTL-W2-0023|mode=extended|selected={','.join(page.stream.selected_card_ids())}")

        page, _cards = page_fixture(directory, 6)
        page.stream.set_multi_selection_enabled(True)
        select_rows(page, (1, 2, 3))
        selection_model = page.stream.selectionModel()
        if selection_model is None:
            raise RuntimeError("selection model unavailable")
        selection_model.setCurrentIndex(page.stream.card_model.index(2), QItemSelectionModel.SelectionFlag.NoUpdate)
        page.stream.set_multi_selection_enabled(False)
        lines.append(
            f"WTL-W2-0024|mode=single|current={page.stream.currentIndex().data(CardRole.CARD_ID)}"
            f"|selected={','.join(page.stream.selected_card_ids())}"
        )

        page, _cards = page_fixture(directory, 6)
        page.stream.set_multi_selection_enabled(True)
        select_rows(page, (2, 4))
        selection_model = page.stream.selectionModel()
        if selection_model is None:
            raise RuntimeError("selection model unavailable")
        selection_model.setCurrentIndex(page.stream.card_model.index(5), QItemSelectionModel.SelectionFlag.NoUpdate)
        page.stream.set_multi_selection_enabled(False)
        lines.append(
            f"WTL-W2-0025|mode=single|current={page.stream.currentIndex().data(CardRole.CARD_ID)}"
            f"|selected={','.join(page.stream.selected_card_ids())}"
        )

        page, _cards = page_fixture(directory, 6)
        stream = page.stream
        index = stream.card_model.index(1)
        QTest.mouseClick(stream.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.ControlModifier, stream.visualRect(index).center())
        after_ctrl = len(stream.selected_card_ids())
        index = stream.card_model.index(0)
        QTest.mouseClick(stream.viewport(), Qt.MouseButton.LeftButton, Qt.KeyboardModifier.ShiftModifier, stream.visualRect(index).center())
        after_shift = len(stream.selected_card_ids())
        lines.append(f"WTL-W2-0026|after_ctrl_count={after_ctrl}|after_shift_count={after_shift}")

        page, _cards = page_fixture(directory, 6)
        page.stream.set_multi_selection_enabled(True)
        select_rows(page, (1, 2, 3))
        page.stream.set_multi_selection_enabled(False)
        requests: list[tuple[str, ...]] = []
        page.stream.cards_delete_requested.connect(requests.append)
        page.stream.setFocus()
        QTest.keyClick(page.stream, Qt.Key.Key_Delete)
        application.processEvents()
        deleted_ids = requests[0] if requests else ()
        lines.append(f"WTL-W2-0027|delete={','.join(deleted_ids) or '-'}|count={len(deleted_ids)}")

        page, _cards = page_fixture(directory, 8)
        stream = page.stream
        stream.setCurrentIndex(stream.card_model.index(0))
        position = QPointF(stream.viewport().rect().center())
        wheel = QWheelEvent(
            position, position, QPoint(0, 0), QPoint(0, -120),
            Qt.MouseButton.NoButton, Qt.KeyboardModifier.NoModifier,
            Qt.ScrollPhase.NoScrollPhase, False,
        )
        application.sendEvent(stream.viewport(), wheel)
        lines.append(
            f"WTL-W2-0028|row={stream.currentIndex().row()}"
            f"|current={stream.currentIndex().data(CardRole.CARD_ID)}"
            f"|selected={','.join(stream.selected_card_ids())}"
        )
        stream.cancel_pending_browse()

        if len(lines) != 10:
            raise RuntimeError(f"expected 10 golden lines, got {len(lines)}")
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
