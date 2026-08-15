from __future__ import annotations

import logging
import time

from PySide6.QtWidgets import QApplication, QPlainTextEdit
from pytestqt.qtbot import QtBot

from pynote.domain.models import Card, CardSource
from pynote.infrastructure.repositories import text_hash
from pynote.ui.cards.card_model import CardListModel
from pynote.ui.cards.card_stream import CardStreamView

LOGGER = logging.getLogger(__name__)


def _card(number: int) -> Card:
    body = f"성능 카드 {number}"
    return Card(
        id=f"performance-{number}",
        document_id="performance-document",
        operation_id=f"performance-operation-{number}",
        position_key=number * 1_024,
        capture_seq=number,
        created_at_us=number,
        updated_at_us=number,
        source=CardSource.SYSTEM,
        body=body,
        body_hash=text_hash(body),
        current_revision_id=f"performance-revision-{number}",
    )


def test_1mb_and_10mb_plain_text_edit_load_latency(qtbot: QtBot) -> None:
    editor = QPlainTextEdit()
    qtbot.addWidget(editor)
    editor.show()

    for label, text in (
        ("1MB", ("a" * 79 + "\n") * (1024 * 1024 // 80)),
        ("10MB", ("a" * 79 + "\n") * (10 * 1024 * 1024 // 80)),
    ):
        started = time.perf_counter()
        editor.setPlainText(text)
        QApplication.processEvents()
        elapsed = time.perf_counter() - started
        LOGGER.info("%s 편집기 로드 elapsed_ms=%.3f", label, elapsed * 1_000)
        assert editor.toPlainText() == text
        assert elapsed < 10


def test_10000_card_model_creation_and_scroll_latency(qtbot: QtBot) -> None:
    started = time.perf_counter()
    model = CardListModel(tuple(_card(number) for number in range(1, 10_001)))
    view = CardStreamView(model)
    qtbot.addWidget(view)
    view.resize(640, 480)
    view.show()
    view.scrollTo(model.index(model.rowCount() - 1))
    QApplication.processEvents()
    elapsed = time.perf_counter() - started

    LOGGER.info("10,000 카드 모델·스크롤 elapsed_ms=%.3f", elapsed * 1_000)
    assert model.rowCount() == 10_000
    assert elapsed < 10
