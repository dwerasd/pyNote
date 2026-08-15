from __future__ import annotations

from dataclasses import replace

from PySide6.QtCore import QPoint, QRect, Qt
from PySide6.QtGui import QFont, QFontMetrics
from PySide6.QtTest import QTest
from PySide6.QtWidgets import (
    QAbstractItemView,
    QApplication,
    QStyleOptionViewItem,
    QWidget,
)
from pytest import MonkeyPatch
from pytestqt.qtbot import QtBot

from pynote.domain.models import Card, CardSource
from pynote.infrastructure.repositories import text_hash
from pynote.ui.cards import card_model as card_model_module
from pynote.ui.cards.card_delegate import CardDelegate
from pynote.ui.cards.card_model import (
    PREVIEW_CODEPOINT_BUDGET_PER_LINE,
    CardListModel,
    CardRole,
)
from pynote.ui.cards.card_stream import CardStreamView


def _card(
    number: int,
    *,
    body: str | None = None,
    operation_id: str | None = None,
    source: CardSource = CardSource.TYPING,
) -> Card:
    card_body = f"카드 {number}" if body is None else body
    return Card(
        id=f"card-{number}",
        document_id="document-1",
        operation_id=operation_id or f"operation-{number}",
        position_key=number * 1_024,
        capture_seq=number,
        created_at_us=1_000_000 + number,
        updated_at_us=1_000_000 + number,
        source=source,
        body=card_body,
        body_hash=text_hash(card_body),
        current_revision_id=f"revision-{number}",
    )


class _RecordingPainter:
    def __init__(self) -> None:
        self.drawn_text: list[tuple[QRect, str]] = []

    @property
    def texts(self) -> list[str]:
        return [text for _rect, text in self.drawn_text]

    def save(self) -> None:
        pass

    def restore(self) -> None:
        pass

    def setPen(self, _pen: object) -> None:
        pass

    def setBrush(self, _brush: object) -> None:
        pass

    def setFont(self, _font: object) -> None:
        pass

    def drawRoundedRect(self, *_args: object) -> None:
        pass

    def drawText(self, *_args: object) -> None:
        rect = _args[0]
        assert isinstance(rect, QRect)
        self.drawn_text.append((QRect(rect), str(_args[-1])))


def test_model_preview_and_display_roles_keep_the_full_body_string() -> None:
    body = "\n".join(f"{line}줄" for line in range(1, 51))
    model = CardListModel([_card(1, body=body)])
    index = model.index(0)

    assert index.data(Qt.ItemDataRole.DisplayRole) == body
    assert index.data(CardRole.PREVIEW) == body
    assert index.data(CardRole.BODY) == body
    assert b"expanded" not in model.roleNames().values()


def test_delegate_paints_body_then_time_without_removed_metadata(
    qapp: QApplication,
) -> None:
    assert qapp is QApplication.instance()
    model = CardListModel(
        [_card(1, body="본문")],
        revision_counts={"card-1": 7},
        reconstruction_unavailable_ids=frozenset({"card-1"}),
    )
    delegate = CardDelegate()
    option = QStyleOptionViewItem()
    option.rect = QRect(0, 0, 500, delegate.sizeHint(option, model.index(0)).height())  # pyright: ignore[reportAttributeAccessIssue]
    painter = _RecordingPainter()

    delegate.paint(
        painter,  # pyright: ignore[reportArgumentType]
        option,
        model.index(0),
    )

    body_rect, body_text = painter.drawn_text[0]
    time_rect, _time_text = painter.drawn_text[1]
    content_rect = option.rect.adjusted(  # pyright: ignore[reportAttributeAccessIssue]
        delegate._card_horizontal_inset + delegate._horizontal_margin,
        delegate._card_vertical_inset + delegate._vertical_margin,
        -delegate._card_horizontal_inset - delegate._horizontal_margin,
        -delegate._card_vertical_inset - delegate._vertical_margin,
    )

    assert body_text == "본문"
    assert body_rect.top() < time_rect.top()
    assert time_rect.bottom() == content_rect.bottom()
    assert all("재구성 불가" not in text for text in painter.texts)
    assert all("위치 " not in text for text in painter.texts)
    assert all("기록 #" not in text for text in painter.texts)
    assert all("출처" not in text for text in painter.texts)
    assert all("리비전" not in text for text in painter.texts)
    assert all("더 보기" not in text for text in painter.texts)


def test_delegate_time_row_places_suffix_immediately_after_time_when_wide() -> None:
    card = replace(
        _card(1),
        created_at_us=1_000_000,
        updated_at_us=2_000_000,
    )
    model = CardListModel([card], dirty_draft_ids=frozenset({card.id}))
    delegate = CardDelegate()
    option = QStyleOptionViewItem()
    option.rect = QRect(0, 0, 600, 100)  # pyright: ignore[reportAttributeAccessIssue]
    painter = _RecordingPainter()

    delegate.paint(
        painter,  # pyright: ignore[reportArgumentType]
        option,
        model.index(0),
    )

    time_rect, time_text = painter.drawn_text[-2]
    suffix_rect, suffix_text = painter.drawn_text[-1]
    assert time_text == delegate._time_label(card.updated_at_us)
    assert suffix_text == " (수정됨) · 편집 중"
    assert time_rect.right() + 1 == suffix_rect.left()


def test_delegate_time_row_keeps_modified_and_dirty_suffix_when_narrow() -> None:
    card = replace(
        _card(1),
        created_at_us=1_000_000,
        updated_at_us=2_000_000,
    )
    model = CardListModel([card], dirty_draft_ids=frozenset({card.id}))
    delegate = CardDelegate()
    option = QStyleOptionViewItem()
    option.rect = QRect(0, 0, 220, 100)  # pyright: ignore[reportAttributeAccessIssue]
    painter = _RecordingPainter()

    delegate.paint(
        painter,  # pyright: ignore[reportArgumentType]
        option,
        model.index(0),
    )

    assert " (수정됨) · 편집 중" in painter.texts
    time_rect, time_text = painter.drawn_text[-2]
    suffix_rect, suffix_text = painter.drawn_text[-1]
    content_rect = option.rect.adjusted(  # pyright: ignore[reportAttributeAccessIssue]
        delegate._card_horizontal_inset + delegate._horizontal_margin,
        delegate._card_vertical_inset + delegate._vertical_margin,
        -delegate._card_horizontal_inset - delegate._horizontal_margin,
        -delegate._card_vertical_inset - delegate._vertical_margin,
    )

    assert time_text.endswith("…")
    assert suffix_text == " (수정됨) · 편집 중"
    assert time_rect.right() < suffix_rect.left()
    assert content_rect.contains(suffix_rect)


def test_delegate_time_row_elides_oversized_suffix_inside_content() -> None:
    card = replace(
        _card(1),
        created_at_us=1_000_000,
        updated_at_us=2_000_000,
    )
    model = CardListModel([card], dirty_draft_ids=frozenset({card.id}))
    delegate = CardDelegate()
    option = QStyleOptionViewItem()
    option.rect = QRect(0, 0, 80, 100)  # pyright: ignore[reportAttributeAccessIssue]
    painter = _RecordingPainter()

    delegate.paint(
        painter,  # pyright: ignore[reportArgumentType]
        option,
        model.index(0),
    )

    _time_rect, time_text = painter.drawn_text[-2]
    suffix_rect, suffix_text = painter.drawn_text[-1]
    content_rect = option.rect.adjusted(  # pyright: ignore[reportAttributeAccessIssue]
        delegate._card_horizontal_inset + delegate._horizontal_margin,
        delegate._card_vertical_inset + delegate._vertical_margin,
        -delegate._card_horizontal_inset - delegate._horizontal_margin,
        -delegate._card_vertical_inset - delegate._vertical_margin,
    )

    assert time_text == ""
    assert suffix_text.endswith("…")
    assert suffix_rect.left() == content_rect.left()
    assert content_rect.contains(suffix_rect)


def test_delegate_ellipsizes_source_newline_and_wrapped_overflow() -> None:
    delegate = CardDelegate()
    option = QStyleOptionViewItem()
    font = option.font  # pyright: ignore[reportAttributeAccessIssue]
    metrics = option.fontMetrics  # pyright: ignore[reportAttributeAccessIssue]

    source_lines = delegate._preview_lines(
        "첫째\n둘째\n셋째\n넷째",
        font,
        metrics,
        500,
        3,
    )
    wrapped_lines = delegate._preview_lines(
        "자동 감김을 만드는 긴 본문 " * 20,
        font,
        metrics,
        90,
        3,
    )

    assert source_lines == ("첫째", "둘째", "셋째…")
    assert len(wrapped_lines) == 3
    assert wrapped_lines[-1].endswith("…")
    assert delegate._preview_lines("첫째\n둘째", font, metrics, 500, 1) == ("첫째…",)
    unbroken_lines = delegate._preview_lines("가" * 200, font, metrics, 90, 3)
    assert len(unbroken_lines) == 3
    assert unbroken_lines[-1].endswith("…")


def test_delegate_preview_lines_keep_non_bmp_source_newline_boundaries(
    qapp: QApplication,
) -> None:
    assert qapp is QApplication.instance()
    delegate = CardDelegate()
    option = QStyleOptionViewItem()
    font = option.font  # pyright: ignore[reportAttributeAccessIssue]
    metrics = option.fontMetrics  # pyright: ignore[reportAttributeAccessIssue]

    lines = delegate._preview_lines(
        "😀😀😀\n다음 줄\n셋째 줄\n넷째 줄",
        font,
        metrics,
        400,
        3,
    )

    assert lines == ("😀😀😀", "다음 줄", "셋째 줄…")
    assert all("\u2028" not in line for line in lines)


def test_delegate_preview_lines_keep_non_bmp_wrapped_boundaries(
    qapp: QApplication,
) -> None:
    assert qapp is QApplication.instance()
    delegate = CardDelegate()
    option = QStyleOptionViewItem()
    font = option.font  # pyright: ignore[reportAttributeAccessIssue]
    metrics = option.fontMetrics  # pyright: ignore[reportAttributeAccessIssue]
    width = metrics.horizontalAdvance("😀😀가가")
    body = "😀😀가가나나다다라마"

    lines = delegate._preview_lines(
        body,
        font,
        metrics,
        width,
        3,
    )

    assert "".join(lines) == body
    assert all(
        not (
            left
            and right
            and 0xD800 <= ord(left[-1]) <= 0xDBFF
            and 0xDC00 <= ord(right[0]) <= 0xDFFF
        )
        for left, right in zip(lines, lines[1:], strict=False)
    )
    assert len(lines) <= 3
    assert all("\u2028" not in line for line in lines)


def test_delegate_paint_ellipsizes_at_actual_content_width() -> None:
    model = CardListModel([_card(1, body="공백없는초장문" * 50)])
    delegate = CardDelegate()
    option = QStyleOptionViewItem()
    option.rect = QRect(0, 0, 220, 100)  # pyright: ignore[reportAttributeAccessIssue]
    painter = _RecordingPainter()

    delegate.paint(
        painter,  # pyright: ignore[reportArgumentType]
        option,
        model.index(0),
    )

    assert painter.drawn_text[2][1].endswith("…")


def test_delegate_handles_empty_and_whitespace_only_bodies() -> None:
    delegate = CardDelegate()
    option = QStyleOptionViewItem()
    font = option.font  # pyright: ignore[reportAttributeAccessIssue]
    metrics = option.fontMetrics  # pyright: ignore[reportAttributeAccessIssue]

    assert delegate._preview_lines("", font, metrics, 100, 3) == ("",)
    assert delegate._preview_lines("   ", font, metrics, 100, 3) == ("   ",)


def test_delegate_size_hint_is_constant_for_short_and_hundred_line_bodies() -> None:
    option = QStyleOptionViewItem()
    option.rect = QRect(0, 0, 500, 100)  # pyright: ignore[reportAttributeAccessIssue]
    delegate = CardDelegate()
    short = CardListModel([_card(1, body="한 줄")])
    long = CardListModel([_card(1, body="\n".join(["긴 본문"] * 100))])

    wide_height = delegate.sizeHint(option, short.index(0)).height()
    assert (
        wide_height
        == delegate.sizeHint(
            option,
            long.index(0),
        ).height()
    )

    option.rect = QRect(0, 0, 240, 100)  # pyright: ignore[reportAttributeAccessIssue]
    assert delegate.sizeHint(option, short.index(0)).height() == wide_height
    assert (
        delegate.sizeHint(option, short.index(0)).height()
        == delegate.sizeHint(
            option,
            long.index(0),
        ).height()
    )


def test_delegate_height_budgets_follow_two_font_metrics_independently() -> None:
    delegate = CardDelegate()
    plain = CardListModel([_card(1)])
    grouped = CardListModel([_card(1), _card(2)])
    unavailable = CardListModel(
        [_card(1)],
        reconstruction_unavailable_ids=frozenset({"card-1"}),
    )

    default_option = QStyleOptionViewItem()
    default_font = QFont(default_option.font)  # pyright: ignore[reportAttributeAccessIssue]
    large_font = QFont(default_font)
    large_font.setPointSize(max(1, default_font.pointSize()) * 2)

    measured_heights: list[int] = []
    for font in (default_font, large_font):
        option = QStyleOptionViewItem()
        option.rect = QRect(0, 0, 500, 100)  # pyright: ignore[reportAttributeAccessIssue]
        option.font = font  # pyright: ignore[reportAttributeAccessIssue]
        option.fontMetrics = QFontMetrics(font)  # pyright: ignore[reportAttributeAccessIssue]
        line_spacing = max(1, option.fontMetrics.lineSpacing())  # pyright: ignore[reportAttributeAccessIssue]
        expected_plain = 2 * 3 + 2 * 10 + 3 * line_spacing + (line_spacing + 4)

        plain_height = delegate.sizeHint(option, plain.index(0)).height()
        grouped_height = delegate.sizeHint(option, grouped.index(0)).height()
        unavailable_height = delegate.sizeHint(option, unavailable.index(0)).height()

        assert plain_height == expected_plain
        assert grouped_height == expected_plain
        assert unavailable_height == expected_plain
        measured_heights.append(plain_height)

    assert measured_heights[1] > measured_heights[0]


def test_card_stream_keeps_size_hint_height_when_width_changes(
    qtbot: QtBot,
) -> None:
    body = ("폭에 따라 감기는 본문은 목록 너비가 달라지면 필요한 높이도 달라집니다. " * 8).strip()
    view = CardStreamView(CardListModel([_card(1, body=body)]))
    qtbot.addWidget(view)
    view.resize(900, 500)
    view.show()
    qtbot.wait(20)
    index = view.card_model.index(0)
    wide_height = view.visualRect(index).height()

    view.resize(320, 500)
    qtbot.wait(20)

    assert view.visualRect(index).height() == wide_height


def test_model_switches_sort_and_filters_without_changing_position_number() -> None:
    first = _card(1, source=CardSource.PASTE)
    middle = replace(
        _card(3, source=CardSource.IMPORT),
        position_key=2_048,
        capture_seq=3,
        updated_at_us=2_000_000,
    )
    last = replace(
        _card(2, source=CardSource.TYPING),
        position_key=3_072,
        capture_seq=2,
    )
    model = CardListModel([first, middle, last])

    assert [model.index(row).data(CardRole.CARD_ID) for row in range(3)] == [
        middle.id,
        last.id,
        first.id,
    ]
    assert model.index(0).data(CardRole.POSITION_NUMBER) == 2
    model.set_sort_mode("position")
    assert [model.index(row).data(CardRole.CARD_ID) for row in range(3)] == [
        first.id,
        middle.id,
        last.id,
    ]
    model.set_sort_mode("capture")
    assert [model.index(row).data(CardRole.CARD_ID) for row in range(3)] == [
        first.id,
        last.id,
        middle.id,
    ]
    assert model.index(2).data(CardRole.POSITION_NUMBER) == 2
    model.set_source_filter([CardSource.IMPORT])
    assert model.rowCount() == 1


def test_recency_tie_uses_capture_sequence_descending() -> None:
    cards = tuple(
        replace(_card(number), updated_at_us=2_000_000)
        for number in range(1, 4)
    )

    model = CardListModel(cards)

    assert [model.index(row).data(CardRole.CAPTURE_SEQ) for row in range(3)] == [
        3,
        2,
        1,
    ]


def test_recency_update_moves_only_the_saved_row_and_keeps_selection(
    qtbot: QtBot,
) -> None:
    first = _card(1)
    second = _card(2)
    model = CardListModel([first, second])
    view = CardStreamView(model)
    qtbot.addWidget(view)
    first_index = model.index_for_card(first.id)
    view.setCurrentIndex(first_index)
    moves: list[tuple[int, int]] = []
    resets: list[bool] = []
    model.rowsMoved.connect(
        lambda _source, start, _end, _destination, row: moves.append((start, row))
    )
    model.modelReset.connect(lambda: resets.append(True))

    updated = replace(first, updated_at_us=2_000_000, body="수정된 첫 카드")
    assert model.update_card(updated, revision_count=2)

    assert model.index(0).data(CardRole.CARD_ID) == first.id
    assert view.currentIndex().data(CardRole.CARD_ID) == first.id
    assert moves == [(1, 0)]
    assert resets == []


def test_new_card_is_inserted_at_recency_top_without_model_reset() -> None:
    first = _card(1)
    second = _card(2)
    model = CardListModel([first])
    inserted: list[tuple[int, int]] = []
    resets: list[bool] = []
    model.rowsInserted.connect(
        lambda _parent, start, end: inserted.append((start, end))
    )
    model.modelReset.connect(lambda: resets.append(True))

    model.add_cards([second], revision_counts={second.id: 1})

    assert model.index(0).data(CardRole.CARD_ID) == second.id
    assert inserted == [(0, 0)]
    assert resets == []


def test_ctrl_click_selects_multiple_cards_without_opening_second(
    qtbot: QtBot,
) -> None:
    view = CardStreamView(CardListModel([_card(1), _card(2)]))
    view.set_sort_mode("position")
    # 다중 선택은 기본이 꺼짐이다 — 이 계약은 사용자가 켠 상태의 것이다.
    view.set_multi_selection_enabled(True)
    qtbot.addWidget(view)
    view.resize(500, 400)
    view.show()
    qtbot.wait(20)
    opened: list[str] = []
    view.card_open_requested.connect(opened.append)
    first_point = view.visualRect(view.card_model.index(0)).center()
    second_point = view.visualRect(view.card_model.index(1)).center()

    QTest.mouseClick(view.viewport(), Qt.MouseButton.LeftButton, pos=first_point)
    QTest.mouseClick(
        view.viewport(),
        Qt.MouseButton.LeftButton,
        Qt.KeyboardModifier.ControlModifier,
        pos=second_point,
    )

    assert view.selected_card_ids() == ("card-1", "card-2")
    assert opened == ["card-1"]


def _shown_empty_area_view(qtbot: QtBot) -> CardStreamView:
    view = CardStreamView(CardListModel([_card(1)]))
    qtbot.addWidget(view)
    view.resize(500, 400)
    view.show()
    last_index = view.card_model.index(view.card_model.rowCount() - 1)
    qtbot.waitUntil(
        lambda: (
            view.viewport().rect().isValid()
            and view.visualRect(last_index).isValid()
            and view.visualRect(last_index).bottom()
            < view.viewport().rect().bottom()
        )
    )
    return view


def _empty_area_point(view: CardStreamView) -> QPoint:
    viewport_rect = view.viewport().rect()
    last_rect = view.visualRect(
        view.card_model.index(view.card_model.rowCount() - 1)
    )
    point = QPoint(
        viewport_rect.center().x(),
        (last_rect.bottom() + viewport_rect.bottom() + 1) // 2,
    )
    assert viewport_rect.contains(point)
    assert point.y() > last_rect.bottom()
    assert not view.indexAt(point).isValid()
    return point


def test_empty_area_clean_left_click_emits_signal_once(qtbot: QtBot) -> None:
    view = _shown_empty_area_view(qtbot)
    clicked: list[bool] = []
    view.empty_area_clicked.connect(lambda: clicked.append(True))
    point = _empty_area_point(view)

    QTest.mouseClick(view.viewport(), Qt.MouseButton.LeftButton, pos=point)

    assert clicked == [True]

    jittered_point = point + QPoint(2, 0)
    assert view.viewport().rect().contains(jittered_point)
    assert not view.indexAt(jittered_point).isValid()
    QTest.mousePress(view.viewport(), Qt.MouseButton.LeftButton, pos=point)
    QTest.mouseMove(view.viewport(), jittered_point)
    assert view.state() == QAbstractItemView.State.NoState
    QTest.mouseRelease(
        view.viewport(),
        Qt.MouseButton.LeftButton,
        pos=jittered_point,
    )

    assert clicked == [True, True]


def test_subthreshold_card_boundary_rubber_band_does_not_emit_empty_click(
    qtbot: QtBot,
) -> None:
    view = _shown_empty_area_view(qtbot)
    clicked: list[bool] = []
    view.empty_area_clicked.connect(lambda: clicked.append(True))
    card_rect = view.visualRect(view.card_model.index(0))
    start = QPoint(card_rect.center().x(), card_rect.bottom() + 2)
    crossed = QPoint(card_rect.center().x(), card_rect.bottom() - 2)
    assert _empty_area_point(view).y() > start.y()
    assert view.viewport().rect().contains(start)
    assert not view.indexAt(start).isValid()
    assert view.indexAt(crossed).isValid()
    assert (crossed - start).manhattanLength() < QApplication.startDragDistance()

    QTest.mousePress(view.viewport(), Qt.MouseButton.LeftButton, pos=start)
    QTest.mouseMove(view.viewport(), crossed)
    assert view.state() == QAbstractItemView.State.DragSelectingState
    QTest.mouseRelease(
        view.viewport(),
        Qt.MouseButton.LeftButton,
        pos=start,
    )

    assert clicked == []
    assert view.selected_card_ids() == ("card-1",)


def test_round_trip_rubber_band_does_not_emit_empty_area_click(
    qtbot: QtBot,
) -> None:
    view = _shown_empty_area_view(qtbot)
    clicked: list[bool] = []
    view.empty_area_clicked.connect(lambda: clicked.append(True))
    start = _empty_area_point(view)
    far = start + QPoint(QApplication.startDragDistance() + 5, 0)
    near_start = start + QPoint(1, 0)
    assert view.viewport().rect().contains(far)
    assert not view.indexAt(far).isValid()

    QTest.mousePress(view.viewport(), Qt.MouseButton.LeftButton, pos=start)
    QTest.mouseMove(view.viewport(), far)
    QTest.mouseMove(view.viewport(), near_start)
    QTest.mouseRelease(
        view.viewport(),
        Qt.MouseButton.LeftButton,
        pos=near_start,
    )

    assert clicked == []


def test_empty_area_release_with_modifier_does_not_emit_signal(
    qtbot: QtBot,
) -> None:
    view = _shown_empty_area_view(qtbot)
    clicked: list[bool] = []
    view.empty_area_clicked.connect(lambda: clicked.append(True))
    point = _empty_area_point(view)

    QTest.mousePress(view.viewport(), Qt.MouseButton.LeftButton, pos=point)
    QTest.mouseRelease(
        view.viewport(),
        Qt.MouseButton.LeftButton,
        Qt.KeyboardModifier.ControlModifier,
        pos=point,
    )

    assert clicked == []


def test_empty_area_ctrl_press_then_unmodified_release_does_not_emit_signal(
    qtbot: QtBot,
) -> None:
    view = _shown_empty_area_view(qtbot)
    clicked: list[bool] = []
    view.empty_area_clicked.connect(lambda: clicked.append(True))
    point = _empty_area_point(view)

    QTest.mousePress(
        view.viewport(),
        Qt.MouseButton.LeftButton,
        Qt.KeyboardModifier.ControlModifier,
        pos=point,
    )
    QTest.mouseRelease(view.viewport(), Qt.MouseButton.LeftButton, pos=point)

    assert clicked == []


def test_empty_area_shift_press_then_unmodified_release_does_not_emit_signal(
    qtbot: QtBot,
) -> None:
    view = _shown_empty_area_view(qtbot)
    clicked: list[bool] = []
    view.empty_area_clicked.connect(lambda: clicked.append(True))
    point = _empty_area_point(view)

    QTest.mousePress(
        view.viewport(),
        Qt.MouseButton.LeftButton,
        Qt.KeyboardModifier.ShiftModifier,
        pos=point,
    )
    QTest.mouseRelease(view.viewport(), Qt.MouseButton.LeftButton, pos=point)

    assert clicked == []


def test_empty_area_shift_release_does_not_emit_signal(qtbot: QtBot) -> None:
    view = _shown_empty_area_view(qtbot)
    clicked: list[bool] = []
    view.empty_area_clicked.connect(lambda: clicked.append(True))
    point = _empty_area_point(view)

    QTest.mousePress(view.viewport(), Qt.MouseButton.LeftButton, pos=point)
    QTest.mouseRelease(
        view.viewport(),
        Qt.MouseButton.LeftButton,
        Qt.KeyboardModifier.ShiftModifier,
        pos=point,
    )

    assert clicked == []


def test_card_click_opens_card_without_empty_area_signal(qtbot: QtBot) -> None:
    view = _shown_empty_area_view(qtbot)
    clicked: list[bool] = []
    opened: list[str] = []
    view.empty_area_clicked.connect(lambda: clicked.append(True))
    view.card_open_requested.connect(opened.append)
    point = view.visualRect(view.card_model.index(0)).center()

    QTest.mouseClick(view.viewport(), Qt.MouseButton.LeftButton, pos=point)

    assert clicked == []
    assert opened == ["card-1"]


def test_empty_press_then_card_release_does_not_emit_empty_area_signal(
    qtbot: QtBot,
) -> None:
    view = _shown_empty_area_view(qtbot)
    clicked: list[bool] = []
    view.empty_area_clicked.connect(lambda: clicked.append(True))
    empty_point = _empty_area_point(view)
    card_point = view.visualRect(view.card_model.index(0)).center()

    QTest.mousePress(view.viewport(), Qt.MouseButton.LeftButton, pos=empty_point)
    QTest.mouseRelease(
        view.viewport(),
        Qt.MouseButton.LeftButton,
        pos=card_point,
    )

    assert clicked == []


def test_card_press_then_empty_release_does_not_emit_empty_area_signal(
    qtbot: QtBot,
) -> None:
    view = _shown_empty_area_view(qtbot)
    clicked: list[bool] = []
    view.empty_area_clicked.connect(lambda: clicked.append(True))
    card_point = view.visualRect(view.card_model.index(0)).center()
    empty_point = _empty_area_point(view)

    QTest.mousePress(view.viewport(), Qt.MouseButton.LeftButton, pos=card_point)
    QTest.mouseRelease(
        view.viewport(),
        Qt.MouseButton.LeftButton,
        pos=empty_point,
    )

    assert clicked == []


def test_empty_area_right_click_does_not_emit_signal(qtbot: QtBot) -> None:
    view = _shown_empty_area_view(qtbot)
    clicked: list[bool] = []
    view.empty_area_clicked.connect(lambda: clicked.append(True))
    point = _empty_area_point(view)

    QTest.mouseClick(view.viewport(), Qt.MouseButton.RightButton, pos=point)

    assert clicked == []


def test_empty_area_release_outside_viewport_does_not_emit_signal(
    qtbot: QtBot,
) -> None:
    view = _shown_empty_area_view(qtbot)
    clicked: list[bool] = []
    view.empty_area_clicked.connect(lambda: clicked.append(True))
    empty_point = _empty_area_point(view)
    inside = QPoint(view.viewport().rect().right() - 1, empty_point.y())
    outside = QPoint(view.viewport().rect().right() + 1, empty_point.y())
    assert view.viewport().rect().contains(inside)
    assert not view.viewport().rect().contains(outside)
    assert not view.indexAt(inside).isValid()

    QTest.mousePress(view.viewport(), Qt.MouseButton.LeftButton, pos=inside)
    QTest.mouseRelease(
        view.viewport(),
        Qt.MouseButton.LeftButton,
        pos=outside,
    )

    assert clicked == []


def test_card_right_click_preserves_context_selection_without_empty_signal(
    qtbot: QtBot,
) -> None:
    view = _shown_empty_area_view(qtbot)
    clicked: list[bool] = []
    view.empty_area_clicked.connect(lambda: clicked.append(True))
    point = view.visualRect(view.card_model.index(0)).center()

    QTest.mouseClick(view.viewport(), Qt.MouseButton.RightButton, pos=point)

    assert clicked == []
    assert view.selected_card_ids() == ("card-1",)


def test_clicking_card_bottom_opens_card_without_expand_toggle(qtbot: QtBot) -> None:
    body = "\n".join(f"{line}줄" for line in range(1, 11))
    view = CardStreamView(CardListModel([_card(1, body=body)]))
    qtbot.addWidget(view)
    view.resize(500, 400)
    view.show()
    qtbot.wait(20)
    opened: list[str] = []
    view.card_open_requested.connect(opened.append)
    index = view.card_model.index(0)
    rect = view.visualRect(index)
    bottom_point = rect.bottomRight() - view.rect().topLeft()
    bottom_point.setX(rect.right() - 20)
    bottom_point.setY(rect.bottom() - 16)

    QTest.mouseClick(view.viewport(), Qt.MouseButton.LeftButton, pos=bottom_point)

    assert opened == ["card-1"]


def test_ten_thousand_cards_use_delegate_without_per_card_widgets(
    qtbot: QtBot,
) -> None:
    cards = tuple(_card(number) for number in range(1, 10_001))
    model = CardListModel(cards)
    view = CardStreamView(model)
    qtbot.addWidget(view)
    view.resize(600, 500)
    view.show()
    qtbot.wait(20)

    assert model.rowCount() == 10_000
    assert all(view.indexWidget(model.index(row)) is None for row in range(10_000))
    assert len(view.findChildren(QWidget)) < 100


def test_drag_move_is_enabled_only_for_position_sort() -> None:
    view = CardStreamView(CardListModel([_card(1)]))

    assert view.dragEnabled()
    assert not view.acceptDrops()
    assert (
        view.dragDropMode()
        is QAbstractItemView.DragDropMode.DragOnly
    )

    view.set_sort_mode("capture")
    assert view.dragEnabled()
    assert not view.acceptDrops()
    assert view.dragDropMode() is QAbstractItemView.DragDropMode.DragOnly

    view.set_sort_mode("position")
    assert view.dragEnabled()
    assert view.acceptDrops()
    assert (
        view.dragDropMode()
        is QAbstractItemView.DragDropMode.InternalMove
    )


def test_card_tooltip_preserves_removed_metadata_and_reconstruction_marker() -> None:
    model = CardListModel(
        [_card(1)],
        revision_counts={"card-1": 4},
        reconstruction_unavailable_ids=frozenset({"card-1"}),
    )

    tooltip = str(model.index(0).data(Qt.ItemDataRole.ToolTipRole))

    assert "위치 1" in tooltip
    assert "기록 #1" in tooltip
    assert "출처 직접 입력" in tooltip
    assert "최초 기록 " in tooltip
    assert "리비전 4개" in tooltip
    assert "수정 " in tooltip
    assert "재구성 불가" in tooltip


def test_time_display_settings_apply_to_card_row_and_tooltip() -> None:
    model = CardListModel([_card(1)])
    view = CardStreamView(model)

    view.apply_time_display("yyyy", "UTC")

    delegate = view.itemDelegate()
    assert isinstance(delegate, CardDelegate)
    assert delegate._time_label(1_000_000) == "1970"
    tooltip = str(model.index(0).data(Qt.ItemDataRole.ToolTipRole))
    assert "최초 기록 1970" in tooltip
    assert "수정 1970" in tooltip


def test_reconstruction_warning_change_invalidates_size_hint() -> None:
    model = CardListModel([_card(1), _card(2)])
    changed: list[tuple[int, int, list[int]]] = []
    model.dataChanged.connect(
        lambda top, bottom, roles: changed.append(
            (top.row(), bottom.row(), roles)
        )
    )

    model.set_reconstruction_unavailable_ids(frozenset({"card-1"}))

    assert changed == [
        (
            0,
            1,
            [
                int(CardRole.RECONSTRUCTION_AVAILABLE),
                int(Qt.ItemDataRole.ToolTipRole),
                int(Qt.ItemDataRole.SizeHintRole),
            ],
        )
    ]


def test_dirty_draft_changes_emit_only_for_affected_card_rows() -> None:
    model = CardListModel([_card(1), _card(2), _card(3)])
    changed: list[tuple[int, int, list[int]]] = []
    model.dataChanged.connect(
        lambda top, bottom, roles: changed.append((top.row(), bottom.row(), roles))
    )

    model.set_card_dirty("card-2", True)
    model.set_card_dirty("card-2", True)
    model.set_card_dirty("card-2", False)

    assert changed == [
        (1, 1, [int(CardRole.DIRTY_DRAFT)]),
        (1, 1, [int(CardRole.DIRTY_DRAFT)]),
    ]


def _tripwire_body(text: str, log: list[str]) -> str:
    """본문 전체를 대상으로 한 메서드 접근과 문자열 변환을 기록한다.

    슬라이싱 결과는 일반 str이므로 잘라낸 뒤의 연산은 걸리지 않는다. 길이 단언만
    으로는 "전체를 가공한 뒤 잘라내는" 구현을 잡지 못해 이 감시자가 따로 필요하다.
    예외를 던지면 Qt의 paint 경계에서 프로세스가 죽어 진단이 사라지므로 기록만
    남긴다.
    """

    class _TripwireBody(str):
        __slots__ = ()

        def __getattribute__(self, name: str) -> object:
            log.append(name)
            return str.__getattribute__(self, name)

        def __str__(self) -> str:
            log.append("__str__")
            return str.__str__(self)

    return _TripwireBody(text)


def _preview_budget(model: CardListModel) -> int:
    line_count = model.data(model.index(0), CardRole.PREVIEW_LINE_COUNT)
    assert isinstance(line_count, int)
    return (line_count + 1) * card_model_module.PREVIEW_CODEPOINT_BUDGET_PER_LINE


def test_preview_codepoint_budget_per_line_is_fixed_policy() -> None:
    assert PREVIEW_CODEPOINT_BUDGET_PER_LINE == 4_096


def test_preview_truncated_role_is_publicly_named() -> None:
    model = CardListModel()

    assert (
        model.roleNames()[int(CardRole.PREVIEW_TRUNCATED)]
        == b"previewTruncated"
    )


def test_model_preview_is_bounded_and_flags_truncation() -> None:
    model = CardListModel([_card(1, body="가")])
    budget = _preview_budget(model)
    body = "머리" + "가" * budget + "꼬리"
    model = CardListModel([_card(1, body=body)])
    index = model.index(0)

    assert len(index.data(CardRole.PREVIEW)) == budget
    assert index.data(CardRole.PREVIEW) == body[:budget]
    assert index.data(CardRole.PREVIEW_TRUNCATED) is True
    # 원문은 두 role이 그대로 보존한다.
    assert index.data(CardRole.BODY) == body
    assert index.data(Qt.ItemDataRole.DisplayRole) == body


def test_model_short_body_preview_is_whole_body_without_truncation() -> None:
    body = "한 줄\n두 줄"
    model = CardListModel([_card(1, body=body)])
    index = model.index(0)

    assert index.data(CardRole.PREVIEW) == body
    assert index.data(CardRole.PREVIEW_TRUNCATED) is False


def test_update_card_invalidates_preview_and_truncation_roles() -> None:
    card = _card(1, body="짧은 본문")
    model = CardListModel([card])
    changed_roles: list[list[int]] = []
    model.dataChanged.connect(
        lambda _top, _bottom, roles: changed_roles.append(roles)
    )
    body = "가" * (_preview_budget(model) + 1)
    updated = replace(card, body=body, body_hash=text_hash(body))

    assert model.index(0).data(CardRole.PREVIEW_TRUNCATED) is False
    assert model.update_card(updated, revision_count=2)

    assert len(changed_roles) == 1
    assert int(CardRole.PREVIEW) in changed_roles[0]
    assert int(CardRole.PREVIEW_TRUNCATED) in changed_roles[0]
    assert model.index(0).data(CardRole.PREVIEW_TRUNCATED) is True


def test_model_preview_budget_follows_configured_line_count() -> None:
    body = "가" * (10 * PREVIEW_CODEPOINT_BUDGET_PER_LINE)
    model = CardListModel([_card(1, body=body)])
    index = model.index(0)
    three_line_length = len(index.data(CardRole.PREVIEW))
    assert index.data(CardRole.PREVIEW_TRUNCATED) is True

    model.set_preview_line_count(9)
    index = model.index(0)

    assert three_line_length == 4 * PREVIEW_CODEPOINT_BUDGET_PER_LINE
    assert len(index.data(CardRole.PREVIEW)) == 10 * PREVIEW_CODEPOINT_BUDGET_PER_LINE
    assert index.data(CardRole.PREVIEW_TRUNCATED) is False


def test_preview_path_never_operates_on_the_whole_body(qapp: QApplication) -> None:
    model = CardListModel([_card(1, body="가")])
    budget = _preview_budget(model)
    plain = _card(1, body="머리\n" + "가" * budget + "\n감시 대상 꼬리")
    whole_body_operations: list[str] = []
    # 카드 생성의 본문 해시 계산은 감시 대상이 아니므로 그 뒤에 씌운다.
    watched = _tripwire_body(plain.body, whole_body_operations)
    model = CardListModel([replace(plain, body=watched)])
    index = model.index(0)
    delegate = CardDelegate()
    option = QStyleOptionViewItem()
    option.rect = QRect(0, 0, 400, 200)  # pyright: ignore[reportAttributeAccessIssue]
    painter = _RecordingPainter()

    # 감시자가 살아 있는 상태로 실제 paint 경로를 태운다.
    delegate.paint(painter, option, index)  # pyright: ignore[reportArgumentType]

    assert whole_body_operations == []
    assert "감시 대상 꼬리" not in index.data(CardRole.PREVIEW)
    assert all("감시 대상 꼬리" not in text for text in painter.texts)


def test_model_preview_never_splits_a_surrogate_pair() -> None:
    model = CardListModel([_card(1, body="가")])
    budget = _preview_budget(model)
    # 절단 경계에 비-BMP 문자가 걸리도록 앞을 정확히 채운다. UTF-16으로 잘랐다면
    # 이 자리에서 서로게이트 한 짝만 남는다.
    body = "가" * (budget - 1) + "😀" + "꼬리"
    model = CardListModel([_card(1, body=body)])

    preview = model.index(0).data(CardRole.PREVIEW)

    assert len(preview) == budget
    assert preview[-1] == "😀"
    assert preview.encode("utf-16-le").decode("utf-16-le") == preview
    assert "�" not in preview.encode("utf-16", "surrogatepass").decode(
        "utf-16",
        "replace",
    )


def test_delegate_ellipsizes_truncated_preview_that_fits_the_line_budget() -> None:
    delegate = CardDelegate()
    option = QStyleOptionViewItem()
    font = option.font  # pyright: ignore[reportAttributeAccessIssue]
    metrics = option.fontMetrics  # pyright: ignore[reportAttributeAccessIssue]

    within_budget = delegate._preview_lines("첫째\n둘째", font, metrics, 500, 3)
    truncated = delegate._preview_lines(
        "첫째\n둘째",
        font,
        metrics,
        500,
        3,
        truncated=True,
    )

    # 줄 수는 넘지 않지만 모델이 뒷부분을 잘라냈으면 말줄임표가 서야 한다.
    assert within_budget == ("첫째", "둘째")
    assert truncated == ("첫째", "둘째…")


def test_delegate_paints_ellipsis_for_body_beyond_preview_budget(
    qapp: QApplication,
    monkeypatch: MonkeyPatch,
) -> None:
    assert qapp is QApplication.instance()
    monkeypatch.setattr(
        card_model_module,
        "PREVIEW_CODEPOINT_BUDGET_PER_LINE",
        8,
    )
    model = CardListModel([_card(1, body="a" * 33)])
    budget = _preview_budget(model)
    delegate = CardDelegate()
    option = QStyleOptionViewItem()
    option.rect = QRect(0, 0, 400, 200)  # pyright: ignore[reportAttributeAccessIssue]
    painter = _RecordingPainter()
    index = model.index(0)
    preview = index.data(CardRole.PREVIEW)
    assert isinstance(preview, str)
    content_width = option.rect.width() - 2 * (  # pyright: ignore[reportAttributeAccessIssue]
        delegate._card_horizontal_inset + delegate._horizontal_margin
    )

    assert budget == 32
    assert delegate._preview_lines(
        preview,
        option.font,  # pyright: ignore[reportAttributeAccessIssue]
        option.fontMetrics,  # pyright: ignore[reportAttributeAccessIssue]
        content_width,
        3,
    ) == (preview,)

    delegate.paint(painter, option, index)  # pyright: ignore[reportArgumentType]

    assert painter.texts[0] == f"{preview}…"
    assert any(text.endswith("…") for text in painter.texts)
