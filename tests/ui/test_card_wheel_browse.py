from __future__ import annotations

from collections.abc import Iterator

from PySide6.QtCore import QPoint, QPointF, Qt
from PySide6.QtGui import QWheelEvent
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication
from pytest import MonkeyPatch
from pytestqt.qtbot import QtBot

from pynote.application.card_service import CardService
from pynote.domain.models import Card, Document
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories
from pynote.ui.cards.card_stream import BROWSE_OPEN_DELAY_MS, CardStreamView
from pynote.ui.document_page import DocumentPage

# Qt 표준 휠 한 틱의 각도. 제품 상수를 가져다 쓰면 상수를 바꿔도 시험 입력이
# 함께 바뀌어 "틱당 한 장" 계약을 스스로 확인하지 못한다.
WHEEL_TICK_ANGLE = 120
# 굴림 도중을 흉내 내는 이벤트 간격. 지연보다 짧아야 대기가 계속 미뤄진다.
BURST_INTERVAL_MS = 40


def _ids() -> Iterator[str]:
    number = 0
    while True:
        number += 1
        yield f"wheel-{number}"


def _page(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    count: int,
) -> tuple[DocumentPage, tuple[Card, ...]]:
    document = Document(
        id="wheel-document",
        title="휠 탐색",
        created_at_us=1_000_000,
        updated_at_us=1_000_000,
    )
    repositories.create_document(document)
    identifiers = _ids()
    times = iter(range(2_000_000, 2_000_000 + count + 20))
    service = CardService(
        database,
        repositories,
        clock=lambda: next(times),
        id_factory=lambda: next(identifiers),
    )
    cards = tuple(
        service.create_card(document.id, f"카드 {number}")
        for number in range(1, count + 1)
    )
    page = DocumentPage(database, repositories, document.id)
    qtbot.addWidget(page)
    page.resize(900, 700)
    page.show()
    qtbot.waitExposed(page)
    # 기본 recency 정렬은 새 카드가 늘 첫 행이라 행 순서 단언의 판별력이 없다.
    page.stream.set_sort_mode("capture")
    return page, cards


def _quiesce(page: DocumentPage) -> None:
    """대기 타이머와 열린 편집 세션을 남기지 않고 시험을 끝낸다."""
    page.stream.cancel_pending_browse()
    if page.editor.session is not None:
        page.editor.request_close()


def _wheel(
    stream: CardStreamView,
    *,
    steps: int = 1,
    angle: int | None = None,
    angle_x: int = 0,
    pixel_delta: QPoint | None = None,
    modifiers: Qt.KeyboardModifier = Qt.KeyboardModifier.NoModifier,
) -> QWheelEvent:
    """아래로 steps 칸 굴린 휠 한 번을 목록에 보내고 그 이벤트를 돌려준다."""
    delta_y = -WHEEL_TICK_ANGLE * steps if angle is None else angle
    position = QPointF(stream.viewport().rect().center())
    event = QWheelEvent(
        position,
        position,
        QPoint(0, 0) if pixel_delta is None else pixel_delta,
        QPoint(angle_x, delta_y),
        Qt.MouseButton.NoButton,
        modifiers,
        Qt.ScrollPhase.NoScrollPhase,
        False,
    )
    # QAbstractScrollArea 는 viewport 로 온 휠만 wheelEvent 로 넘긴다 — 뷰 자신에게
    # 보내면 sendEvent 가 False 로 떨어져 아무 경로도 타지 않는다.
    QApplication.sendEvent(stream.viewport(), event)
    return event


def test_wheel_tick_moves_the_current_card_one_row(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _page(qtbot, database, repositories, 10)
    stream = page.stream
    stream.setCurrentIndex(stream.card_model.index(3))
    rows: list[int] = []

    for _ in range(3):
        _wheel(stream)
        rows.append(stream.currentIndex().row())

    assert rows == [4, 5, 6]
    assert stream.selected_card_ids() == (cards[6].id,)

    for _ in range(3):
        _wheel(stream, steps=-1)
        rows.append(stream.currentIndex().row())

    assert rows[3:] == [5, 4, 3]
    assert stream.selected_card_ids() == (cards[3].id,)
    _quiesce(page)


def test_wheel_stops_at_both_ends_without_wrapping(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _page(qtbot, database, repositories, 5)
    stream = page.stream
    stream.setCurrentIndex(stream.card_model.index(4))

    _wheel(stream)

    assert stream.currentIndex().row() == 4
    assert stream.selected_card_ids() == (cards[4].id,)

    stream.setCurrentIndex(stream.card_model.index(0))

    _wheel(stream, steps=-1)

    assert stream.currentIndex().row() == 0
    assert stream.selected_card_ids() == (cards[0].id,)
    _quiesce(page)


def test_partial_wheel_angles_accumulate_into_one_card_step(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, _cards = _page(qtbot, database, repositories, 10)
    stream = page.stream
    stream.setCurrentIndex(stream.card_model.index(0))
    rows: list[int] = []

    for _ in range(6):
        _wheel(stream, angle=-40)
        rows.append(stream.currentIndex().row())

    assert rows == [0, 0, 1, 1, 1, 2]
    _quiesce(page)


def test_reversing_direction_discards_the_leftover_angle(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, _cards = _page(qtbot, database, repositories, 10)
    stream = page.stream
    stream.setCurrentIndex(stream.card_model.index(4))

    _wheel(stream, angle=-40)
    _wheel(stream, angle=-40)

    assert stream.currentIndex().row() == 4

    _wheel(stream, angle=WHEEL_TICK_ANGLE)

    assert stream.currentIndex().row() == 3
    _quiesce(page)


def test_wheeling_keeps_every_visited_card_in_view(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, _cards = _page(qtbot, database, repositories, 40)
    stream = page.stream
    stream.setCurrentIndex(stream.card_model.index(0))
    assert not stream.viewport().rect().intersects(
        stream.visualRect(stream.card_model.index(39))
    )

    for _ in range(39):
        _wheel(stream)
        index = stream.currentIndex()
        assert stream.viewport().rect().contains(stream.visualRect(index))

    assert stream.currentIndex().row() == 39

    for _ in range(39):
        _wheel(stream, steps=-1)
        index = stream.currentIndex()
        assert stream.viewport().rect().contains(stream.visualRect(index))

    assert stream.currentIndex().row() == 0
    _quiesce(page)


def test_wheel_defers_the_actual_open_until_the_wheel_stops(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _page(qtbot, database, repositories, 12)
    stream = page.stream
    stream.setCurrentIndex(stream.card_model.index(0))
    opened: list[str] = []
    page.card_opened.connect(opened.append)

    for _ in range(4):
        _wheel(stream)
        qtbot.wait(BURST_INTERVAL_MS)

    assert opened == []

    qtbot.waitUntil(lambda: opened == [cards[4].id], timeout=2_000)
    qtbot.wait(BROWSE_OPEN_DELAY_MS * 3)

    assert opened == [cards[4].id]
    assert page.editor.card_id == cards[4].id
    _quiesce(page)


def test_modifier_wheel_keeps_the_plain_scrollbar_behaviour(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, _cards = _page(qtbot, database, repositories, 40)
    stream = page.stream
    stream.setCurrentIndex(stream.card_model.index(0))
    before = stream.verticalScrollBar().value()
    opened: list[str] = []
    page.card_opened.connect(opened.append)

    _wheel(stream, steps=3, modifiers=Qt.KeyboardModifier.ControlModifier)

    assert stream.currentIndex().row() == 0
    assert stream.verticalScrollBar().value() > before

    qtbot.wait(BROWSE_OPEN_DELAY_MS * 3)

    assert opened == []
    _quiesce(page)


def test_wheel_input_we_cannot_use_is_not_swallowed(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, _cards = _page(qtbot, database, repositories, 40)
    stream = page.stream
    stream.setCurrentIndex(stream.card_model.index(0))

    # 세로 각이 없는 입력은 카드 이동으로 바꿀 수 없다. 우리가 accept 해 버리면
    # 상위 위젯도 처리 기회를 잃는다.
    pixel_only = _wheel(stream, angle=0, pixel_delta=QPoint(0, -240))

    assert stream.currentIndex().row() == 0
    assert not pixel_only.isAccepted()

    horizontal = _wheel(stream, angle=0, angle_x=-WHEEL_TICK_ANGLE)

    assert stream.currentIndex().row() == 0
    assert not horizontal.isAccepted()
    _quiesce(page)


def test_wheel_browse_opens_the_card_and_leaves_focus_on_the_list(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _page(qtbot, database, repositories, 8)
    stream = page.stream
    stream.setCurrentIndex(stream.card_model.index(0))
    stream.setFocus()

    _wheel(stream, steps=2)
    qtbot.waitUntil(lambda: page.editor.card_id == cards[2].id, timeout=2_000)

    assert page.editor.toPlainText() == cards[2].body
    assert page.focusWidget() is stream

    _wheel(stream)
    qtbot.waitUntil(lambda: page.editor.card_id == cards[3].id, timeout=2_000)

    assert page.editor.toPlainText() == cards[3].body
    assert page.focusWidget() is stream
    _quiesce(page)


def test_click_during_the_wheel_delay_wins_and_keeps_editor_focus(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _page(qtbot, database, repositories, 12)
    stream = page.stream
    stream.setCurrentIndex(stream.card_model.index(0))
    stream.setFocus()
    opened: list[str] = []
    page.card_opened.connect(opened.append)

    _wheel(stream, steps=3)
    target = stream.card_model.index(1)
    stream.scrollTo(target)
    QTest.mouseClick(
        stream.viewport(),
        Qt.MouseButton.LeftButton,
        pos=stream.visualRect(target).center(),
    )
    qtbot.wait(BROWSE_OPEN_DELAY_MS * 3)

    assert opened == [cards[1].id]
    assert page.editor.card_id == cards[1].id
    assert page.focusWidget() is page.editor
    _quiesce(page)


def test_reveal_card_during_the_wheel_delay_keeps_the_editor_unconnected(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _page(qtbot, database, repositories, 12)
    stream = page.stream
    stream.setCurrentIndex(stream.card_model.index(0))
    opened: list[str] = []
    page.card_opened.connect(opened.append)

    _wheel(stream, steps=2)

    assert page.reveal_card(cards[7].id)

    qtbot.wait(BROWSE_OPEN_DELAY_MS * 3)

    assert opened == []
    assert page.editor.card_id is None
    assert stream.currentIndex().row() == 7
    _quiesce(page)


def test_selection_moving_elsewhere_during_the_delay_cancels_the_open(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, _cards = _page(qtbot, database, repositories, 8)
    stream = page.stream
    stream.setCurrentIndex(stream.card_model.index(0))
    opened: list[str] = []
    page.card_opened.connect(opened.append)

    _wheel(stream, steps=2)
    stream.setCurrentIndex(stream.card_model.index(6))
    qtbot.wait(BROWSE_OPEN_DELAY_MS * 3)

    assert opened == []
    assert page.editor.card_id is None
    _quiesce(page)


def test_empty_area_click_during_the_delay_does_not_reopen_the_editor(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _page(qtbot, database, repositories, 4)
    stream = page.stream

    assert page.open_card(cards[0].id)

    qtbot.waitUntil(lambda: page.editor.card_id == cards[0].id, timeout=2_000)
    opened: list[str] = []
    page.card_opened.connect(opened.append)

    _wheel(stream, steps=2)
    point = stream.viewport().rect().bottomRight() - QPoint(10, 10)

    assert not stream.indexAt(point).isValid()

    QTest.mouseClick(stream.viewport(), Qt.MouseButton.LeftButton, pos=point)
    qtbot.wait(BROWSE_OPEN_DELAY_MS * 3)

    assert page.editor.session is None
    assert opened == []
    _quiesce(page)


def test_model_reset_during_the_wheel_delay_cancels_the_open(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, _cards = _page(qtbot, database, repositories, 12)
    stream = page.stream
    stream.setCurrentIndex(stream.card_model.index(0))
    opened: list[str] = []
    page.card_opened.connect(opened.append)

    _wheel(stream, steps=3)
    page.refresh()
    qtbot.wait(BROWSE_OPEN_DELAY_MS * 3)

    assert opened == []
    assert page.editor.card_id is None
    _quiesce(page)


def test_browsing_does_not_rebuild_the_hidden_history_view(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    monkeypatch: MonkeyPatch,
) -> None:
    page, cards = _page(qtbot, database, repositories, 12)
    stream = page.stream
    stream.setCurrentIndex(stream.card_model.index(0))
    built: list[str] = []
    monkeypatch.setattr(page.history, "set_card", built.append)

    for _ in range(6):
        _wheel(stream)
        qtbot.wait(BURST_INTERVAL_MS)
    qtbot.wait(BROWSE_OPEN_DELAY_MS * 3)

    assert built == []

    page.show_history()

    assert built == [cards[6].id]
    _quiesce(page)


def test_failed_open_returns_the_selection_to_the_editor_card(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    monkeypatch: MonkeyPatch,
) -> None:
    page, cards = _page(qtbot, database, repositories, 12)
    stream = page.stream

    assert page.open_card(cards[0].id)

    qtbot.waitUntil(lambda: page.editor.card_id == cards[0].id, timeout=2_000)
    monkeypatch.setattr(
        page.editor_workspace,
        "open_card",
        lambda *_args, **_kwargs: False,
    )

    _wheel(stream, steps=4)

    assert stream.currentIndex().row() == 4

    qtbot.waitUntil(lambda: stream.currentIndex().row() == 0, timeout=2_000)

    assert page.editor.card_id == cards[0].id
    assert stream.selected_card_ids() == (cards[0].id,)
    assert page.focusWidget() is page.editor
    _quiesce(page)


def test_wheel_on_an_empty_list_does_not_request_any_card(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, _cards = _page(qtbot, database, repositories, 0)
    stream = page.stream
    requested: list[str] = []
    stream.card_browse_requested.connect(requested.append)

    _wheel(stream, steps=3)
    qtbot.wait(BROWSE_OPEN_DELAY_MS * 3)

    assert requested == []
    assert not stream.currentIndex().isValid()
    _quiesce(page)
