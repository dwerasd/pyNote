from __future__ import annotations

import argparse
import os
import sys
from dataclasses import replace
from pathlib import Path


def _hex(text: str) -> str:
    return text.encode("utf-8").hex()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    source_root = args.source_root.resolve()
    os.environ["QT_QPA_PLATFORM"] = "offscreen"
    sys.path.insert(0, str(source_root / "src"))

    import pynote
    from PySide6.QtCore import QRect, Qt
    from PySide6.QtWidgets import QApplication, QAbstractItemView, QStyleOptionViewItem
    from pynote.domain.models import Card, CardSource
    from pynote.infrastructure.repositories import text_hash
    from pynote.ui.cards.card_delegate import CardDelegate
    from pynote.ui.cards.card_model import (
        PREVIEW_CODEPOINT_BUDGET_PER_LINE,
        CardListModel,
        CardRole,
    )
    from pynote.ui.cards.card_stream import CardStreamView

    resolved_package = Path(pynote.__file__).resolve()
    if not resolved_package.is_relative_to(source_root):
        raise RuntimeError(f"pynote import escaped source root: {resolved_package}")
    application = QApplication.instance() or QApplication([])

    def card(number: int, body: str | None = None, source: CardSource = CardSource.TYPING) -> Card:
        value = f"카드 {number}" if body is None else body
        return Card(
            id=f"card-{number}", document_id="document-1", operation_id=f"operation-{number}",
            position_key=number * 1_024, capture_seq=number,
            created_at_us=1_000_000 + number, updated_at_us=1_000_000 + number,
            source=source, body=value, body_hash=text_hash(value),
            current_revision_id=f"revision-{number}",
        )

    def ids(model: CardListModel) -> str:
        return ",".join(str(model.index(row).data(CardRole.CARD_ID)) for row in range(model.rowCount())) or "-"

    def budget(model: CardListModel) -> int:
        count = int(model.index(0).data(CardRole.PREVIEW_LINE_COUNT))
        return (count + 1) * PREVIEW_CODEPOINT_BUDGET_PER_LINE

    def fields(roles: list[int]) -> str:
        result: list[str] = []
        for role, name in (
            (int(CardRole.CARD), "card"),
            (int(CardRole.PREVIEW), "preview"),
            (int(CardRole.PREVIEW_TRUNCATED), "previewTruncated"),
            (int(CardRole.DIRTY_DRAFT), "dirtyDraft"),
        ):
            if role in roles:
                result.append(name)
        return ",".join(result) or "-"

    lines: list[str] = []
    body = "첫 줄\n둘째 줄"
    model = CardListModel([card(1, body)])
    index = model.index(0)
    preview = str(index.data(CardRole.PREVIEW))
    lines.append(
        f"WTL-W2-0029|full={_hex(str(index.data(CardRole.BODY)))}|preview={_hex(preview)}"
        f"|truncated={int(bool(index.data(CardRole.PREVIEW_TRUNCATED)))}|expanded=absent"
    )

    first = card(1, source=CardSource.PASTE)
    middle = replace(card(3, source=CardSource.IMPORT), position_key=2_048, capture_seq=3, updated_at_us=2_000_000)
    last = replace(card(2), position_key=3_072, capture_seq=2)
    deleted = replace(card(4, source=CardSource.IMPORT), deleted_at_us=3, updated_at_us=3_000_000)
    model = CardListModel([first, middle, last, deleted])
    recency = ids(model)
    model.set_sort_mode("position")
    position = ids(model)
    model.set_sort_mode("capture")
    capture = ids(model)
    middle_position = int(model.index_for_card(middle.id).data(CardRole.POSITION_NUMBER))
    model.set_source_filter([CardSource.IMPORT])
    lines.append(
        f"WTL-W2-0030|recency={recency}|position={position}|capture={capture}"
        f"|middle_position={middle_position}|filter={ids(model)}"
    )

    cards = tuple(replace(card(number), updated_at_us=2_000_000) for number in range(1, 4))
    model = CardListModel(cards)
    capture_order = ",".join(str(model.index(row).data(CardRole.CAPTURE_SEQ)) for row in range(3))
    lines.append(f"WTL-W2-0031|capture={capture_order}")

    first, second = card(1), card(2)
    model = CardListModel([first, second])
    view = CardStreamView(model)
    view.setCurrentIndex(model.index_for_card(first.id))
    changes: list[str] = []
    model.rowsMoved.connect(lambda _s, start, _e, _d, row: changes.append(f"move:{start}:{row}:card"))
    model.dataChanged.connect(lambda top, bottom, roles: changes.append(f"update:{top.row()}:{bottom.row()}:{fields(roles)}"))
    model.update_card(replace(first, updated_at_us=2_000_000, body="수정된 첫 카드"), revision_count=2)
    current = str(view.currentIndex().data(CardRole.CARD_ID))
    selected_ids = ",".join(str(value.data(CardRole.CARD_ID)) for value in view.selectionModel().selectedIndexes())
    lines.append(
        f"WTL-W2-0032|order={ids(model)}|deltas={';'.join(changes)}"
        f"|current={current}|selected={selected_ids}|reset=0"
    )

    model = CardListModel([card(1)])
    changes = []
    model.rowsInserted.connect(lambda _p, start, end: changes.append(f"insert:-:{start}:card"))
    model.add_cards([card(2)], revision_counts={"card-2": 1})
    lines.append(f"WTL-W2-0033|order={ids(model)}|deltas={';'.join(changes)}|reset=0")

    view = CardStreamView(CardListModel([card(1)]))
    policies: list[str] = []
    for mode in ("recency", "capture", "position"):
        view.set_sort_mode(mode)
        internal = view.dragDropMode() is QAbstractItemView.DragDropMode.InternalMove
        policies.append(f"{int(view.dragEnabled())},{int(internal)},{int(view.acceptDrops())}")
    lines.append(f"WTL-W2-0034|policy={';'.join(policies)}")

    model = CardListModel([card(1), card(2), card(3)])
    changes = []
    model.dataChanged.connect(lambda top, bottom, roles: changes.append(f"update:{top.row()}:{bottom.row()}:{fields(roles)}"))
    model.set_card_dirty("card-2", True)
    model.set_card_dirty("card-2", True)
    model.set_card_dirty("card-2", False)
    lines.append(f"WTL-W2-0035|deltas={';'.join(changes)}")

    lines.append(f"WTL-W2-0036|per_line={PREVIEW_CODEPOINT_BUDGET_PER_LINE}")
    role_name = bytes(model.roleNames()[int(CardRole.PREVIEW_TRUNCATED)]).decode("ascii")
    lines.append(f"WTL-W2-0037|trace={role_name}")

    model = CardListModel([card(1, "가")])
    preview_budget = budget(model)
    body = "머리" + "가" * preview_budget + "꼬리"
    model = CardListModel([card(1, body)])
    index = model.index(0)
    preview = str(index.data(CardRole.PREVIEW))
    lines.append(
        f"WTL-W2-0038|budget={preview_budget}|preview={_hex(preview)}|truncated="
        f"{int(bool(index.data(CardRole.PREVIEW_TRUNCATED)))}|examined={len(preview)}|bytes={len(preview.encode())}"
    )

    body = "한 줄\n두 줄"
    model = CardListModel([card(1, body)])
    preview = str(model.index(0).data(CardRole.PREVIEW))
    lines.append(f"WTL-W2-0039|preview={_hex(preview)}|truncated=0|examined={len(preview)}")

    value = card(1, "짧은 본문")
    model = CardListModel([value])
    before_truncated = int(bool(model.index(0).data(CardRole.PREVIEW_TRUNCATED)))
    changes = []
    model.dataChanged.connect(lambda top, bottom, roles: changes.append(f"update:{top.row()}:{bottom.row()}:{fields(roles)}"))
    body = "가" * (budget(model) + 1)
    model.update_card(replace(value, body=body, body_hash=text_hash(body)), revision_count=2)
    after_truncated = int(bool(model.index(0).data(CardRole.PREVIEW_TRUNCATED)))
    lines.append(f"WTL-W2-0040|before={before_truncated}|after={after_truncated}|deltas={';'.join(changes)}")

    body = "가" * (10 * PREVIEW_CODEPOINT_BUDGET_PER_LINE)
    model = CardListModel([card(1, body)])
    three = str(model.index(0).data(CardRole.PREVIEW))
    three_truncated = int(bool(model.index(0).data(CardRole.PREVIEW_TRUNCATED)))
    resets: list[str] = []
    model.modelReset.connect(lambda: resets.append("reset:-:-:-"))
    model.set_preview_line_count(9)
    nine = str(model.index(0).data(CardRole.PREVIEW))
    nine_truncated = int(bool(model.index(0).data(CardRole.PREVIEW_TRUNCATED)))
    lines.append(
        f"WTL-W2-0041|three={len(three)},{three_truncated}|nine={len(nine)},{nine_truncated}"
        f"|deltas={';'.join(resets)}|preview={_hex(nine)}"
    )

    class TripwireBody(str):
        def __getattribute__(self, name: str) -> object:
            operations.append(name)
            return str.__getattribute__(self, name)

        def __str__(self) -> str:
            operations.append("__str__")
            return str.__str__(self)

    class RecordingPainter:
        def __init__(self) -> None:
            self.texts: list[str] = []

        def save(self) -> None: pass
        def restore(self) -> None: pass
        def setPen(self, _pen: object) -> None: pass
        def setBrush(self, _brush: object) -> None: pass
        def setFont(self, _font: object) -> None: pass
        def drawRoundedRect(self, *_args: object) -> None: pass

        def drawText(self, *_args: object) -> None:
            self.texts.append(str(_args[-1]))

    model = CardListModel([card(1, "가")])
    preview_budget = budget(model)
    tail = "감시 대상 꼬리"
    operations: list[str] = []
    body = TripwireBody("머리\n" + "가" * preview_budget + "\n" + tail)
    model = CardListModel([replace(card(1, str.__str__(body)), body=body)])
    index = model.index(0)
    delegate = CardDelegate()
    option = QStyleOptionViewItem()
    option.rect = QRect(0, 0, 400, 200)
    painter = RecordingPainter()
    delegate.paint(painter, option, index)
    preview = str.__str__(index.data(CardRole.PREVIEW))
    if operations or tail in preview or any(tail in text for text in painter.texts):
        raise RuntimeError("bounded preview tripwire observed whole-body work")
    lines.append(
        f"WTL-W2-0042|budget={preview_budget}|preview={_hex(preview)}|truncated=1"
        f"|examined={len(preview)}|bytes={len(preview.encode())}|tail=absent"
    )

    model = CardListModel([card(1, "가")])
    preview_budget = budget(model)
    body = "가" * (preview_budget - 1) + "😀" + "꼬리"
    model = CardListModel([card(1, body)])
    preview = str(model.index(0).data(CardRole.PREVIEW))
    if not preview.endswith("😀") or "�" in preview:
        raise RuntimeError("preview split a Unicode scalar")
    lines.append(
        f"WTL-W2-0043|budget={preview_budget}|preview={_hex(preview)}|truncated=1"
        f"|examined={len(preview)}|bytes={len(preview.encode())}|emoji=final"
    )

    if len(lines) != 15:
        raise RuntimeError(f"expected 15 golden lines, got {len(lines)}")
    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    application.processEvents()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
