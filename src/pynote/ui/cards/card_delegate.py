from __future__ import annotations

from PySide6.QtCore import (
    QDateTime,
    QModelIndex,
    QObject,
    QPersistentModelIndex,
    QPointF,
    QRect,
    QSize,
    Qt,
    QTimeZone,
)
from PySide6.QtGui import QFont, QFontMetrics, QPainter, QPalette, QTextLayout, QTextOption
from PySide6.QtWidgets import QStyle, QStyledItemDelegate, QStyleOptionViewItem

from pynote.ui.cards.card_model import CardRole


class CardDelegate(QStyledItemDelegate):
    """카드마다 QWidget을 만들지 않고 본문과 시간 중심으로 그린다."""

    _horizontal_margin = 14
    _vertical_margin = 10
    _card_horizontal_inset = 4
    _card_vertical_inset = 3
    _auxiliary_row_padding = 4

    def __init__(self, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self._time_format = "yyyy-MM-dd HH:mm:ss"
        self._timezone = "system"

    def apply_time_display(self, time_format: str, timezone: str) -> None:
        """카드 시간 형식과 표시 시간대를 적용한다."""
        self._time_format = time_format
        self._timezone = timezone

    def paint(
        self,
        painter: QPainter,
        option: QStyleOptionViewItem,
        index: QModelIndex | QPersistentModelIndex,
    ) -> None:
        """선택·hover 상태를 포함한 카드 한 행을 그린다."""
        painter.save()
        try:
            state = option.state  # pyright: ignore[reportAttributeAccessIssue]
            selected = bool(state & QStyle.StateFlag.State_Selected)
            hovered = bool(state & QStyle.StateFlag.State_MouseOver)
            palette = option.palette  # pyright: ignore[reportAttributeAccessIssue]
            painter.setFont(option.font)  # pyright: ignore[reportAttributeAccessIssue]
            if selected:
                background = palette.color(QPalette.ColorRole.Highlight)
                foreground = palette.color(QPalette.ColorRole.HighlightedText)
            else:
                background = palette.color(QPalette.ColorRole.Base)
                foreground = palette.color(QPalette.ColorRole.Text)
                if hovered:
                    background = background.lighter(104)

            card_rect = option.rect.adjusted(  # pyright: ignore[reportAttributeAccessIssue]
                self._card_horizontal_inset,
                self._card_vertical_inset,
                -self._card_horizontal_inset,
                -self._card_vertical_inset,
            )
            painter.setPen(palette.color(QPalette.ColorRole.Mid))
            painter.setBrush(background)
            painter.drawRoundedRect(card_rect, 6, 6)

            content_rect = card_rect.adjusted(
                self._horizontal_margin,
                self._vertical_margin,
                -self._horizontal_margin,
                -self._vertical_margin,
            )
            font_metrics = option.fontMetrics  # pyright: ignore[reportAttributeAccessIssue]
            line_spacing = max(1, font_metrics.lineSpacing())
            auxiliary_height = self._auxiliary_row_height(font_metrics)
            y = content_rect.top()

            painter.setPen(foreground if selected else palette.color(QPalette.ColorRole.Text))
            body = str(index.data(CardRole.PREVIEW) or "")
            preview_lines = max(
                1,
                int(index.data(CardRole.PREVIEW_LINE_COUNT) or 1),
            )
            display_lines = self._preview_lines(
                body,
                option.font,  # pyright: ignore[reportAttributeAccessIssue]
                font_metrics,
                max(1, content_rect.width()),
                preview_lines,
                truncated=bool(index.data(CardRole.PREVIEW_TRUNCATED)),
            )
            for line_number, text in enumerate(display_lines):
                painter.drawText(
                    QRect(
                        content_rect.left(),
                        y + line_number * line_spacing,
                        content_rect.width(),
                        line_spacing,
                    ),
                    Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
                    text,
                )
            y += preview_lines * line_spacing

            updated_at_us = int(index.data(CardRole.UPDATED_AT_US) or 0)
            suffix = self._time_suffix(
                modified=bool(index.data(CardRole.MODIFIED)),
                dirty=bool(index.data(CardRole.DIRTY_DRAFT)),
            )
            time_text, suffix_text, suffix_left = self._time_parts(
                self._time_label(updated_at_us),
                suffix,
                font_metrics,
                content_rect,
            )
            painter.setPen(
                foreground if selected else palette.color(QPalette.ColorRole.PlaceholderText)
            )
            painter.drawText(
                QRect(
                    content_rect.left(),
                    y,
                    max(0, suffix_left - content_rect.left()),
                    auxiliary_height,
                ),
                Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
                time_text,
            )
            if suffix_text:
                painter.drawText(
                    QRect(
                        suffix_left,
                        y,
                        font_metrics.horizontalAdvance(suffix_text),
                        auxiliary_height,
                    ),
                    Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
                    suffix_text,
                )
        finally:
            painter.restore()

    def sizeHint(
        self,
        option: QStyleOptionViewItem,
        index: QModelIndex | QPersistentModelIndex,
    ) -> QSize:
        """설정 줄 수와 폰트 메트릭만으로 본문 고정 높이를 반환한다."""
        font_metrics = option.fontMetrics  # pyright: ignore[reportAttributeAccessIssue]
        line_spacing = max(1, font_metrics.lineSpacing())
        preview_lines = max(
            1,
            int(index.data(CardRole.PREVIEW_LINE_COUNT) or 1),
        )
        height = (
            self._card_vertical_inset * 2
            + self._vertical_margin * 2
            + preview_lines * line_spacing
            + self._auxiliary_row_height(font_metrics)
        )
        width = option.rect.width()  # pyright: ignore[reportAttributeAccessIssue]
        return QSize(width, height)

    def _preview_lines(
        self,
        text: str,
        font: QFont,
        font_metrics: QFontMetrics,
        width: int,
        maximum_lines: int,
        *,
        truncated: bool = False,
    ) -> tuple[str, ...]:
        layout_text = text.replace("\r\n", "\n").replace("\r", "\n").replace("\n", "\u2028")
        layout_utf16 = layout_text.encode("utf-16-le")
        layout = QTextLayout(layout_text, font)
        text_option = QTextOption()
        text_option.setWrapMode(QTextOption.WrapMode.WrapAtWordBoundaryOrAnywhere)
        layout.setTextOption(text_option)
        visual_lines: list[str] = []
        layout.beginLayout()
        try:
            while True:
                line = layout.createLine()
                if not line.isValid():
                    break
                line.setLineWidth(width)
                line.setPosition(QPointF(0, len(visual_lines) * font_metrics.lineSpacing()))
                start = line.textStart()
                length = line.textLength()
                visual_lines.append(
                    layout_utf16[start * 2 : (start + length) * 2]
                    .decode("utf-16-le")
                    .rstrip("\u2028")
                )
                if len(visual_lines) > maximum_lines:
                    break
        finally:
            layout.endLayout()

        if not visual_lines:
            visual_lines.append("")
        # 모델이 이미 잘라 보낸 본문은 layout이 줄 수를 넘기지 않아도 뒷부분이
        # 없으므로 말줄임표가 필요하다.
        overflow = len(visual_lines) > maximum_lines or truncated
        displayed = visual_lines[:maximum_lines]
        if overflow:
            with_ellipsis = f"{displayed[-1]}…"
            displayed[-1] = (
                font_metrics.elidedText(
                    with_ellipsis,
                    Qt.TextElideMode.ElideRight,
                    width,
                )
                if font_metrics.horizontalAdvance(with_ellipsis) > width
                else with_ellipsis
            )
        return tuple(displayed)

    @classmethod
    def _auxiliary_row_height(cls, font_metrics: QFontMetrics) -> int:
        return max(1, font_metrics.lineSpacing()) + cls._auxiliary_row_padding

    @staticmethod
    def _time_suffix(*, modified: bool, dirty: bool) -> str:
        return (" (수정됨)" if modified else "") + (" · 편집 중" if dirty else "")

    @staticmethod
    def _time_parts(
        time_text: str,
        suffix: str,
        font_metrics: QFontMetrics,
        content_rect: QRect,
    ) -> tuple[str, str, int]:
        suffix_width = font_metrics.horizontalAdvance(suffix)
        if suffix_width > content_rect.width():
            elided_suffix = font_metrics.elidedText(
                suffix,
                Qt.TextElideMode.ElideRight,
                content_rect.width(),
            )
            return "", elided_suffix, content_rect.left()

        time_width = max(0, content_rect.width() - suffix_width)
        elided_time = font_metrics.elidedText(
            time_text,
            Qt.TextElideMode.ElideRight,
            time_width,
        )
        suffix_left = content_rect.left() + font_metrics.horizontalAdvance(elided_time)
        return elided_time, suffix, suffix_left

    def _time_label(self, epoch_us: int) -> str:
        date_time = QDateTime.fromMSecsSinceEpoch(epoch_us // 1_000, QTimeZone.utc())
        if self._timezone == "system":
            displayed = date_time.toLocalTime()
        elif self._timezone == "UTC":
            displayed = date_time.toTimeZone(QTimeZone.utc())
        else:
            zone = QTimeZone(self._timezone.encode("utf-8"))
            displayed = date_time.toTimeZone(zone) if zone.isValid() else date_time.toLocalTime()
        return displayed.toString(self._time_format)
