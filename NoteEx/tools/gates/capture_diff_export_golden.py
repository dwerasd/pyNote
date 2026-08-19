from __future__ import annotations

import argparse
import sys
import tempfile
from pathlib import Path


def _hex(text: str) -> str:
    return text.encode("utf-8").hex()


def _number(value: int | None) -> str:
    return "-" if value is None else str(value)


def _serialize_diff(result: object) -> str:
    records: list[str] = []
    for line in result.lines:
        if line.characters:
            characters = ";".join(
                f"{part.tag.value}:{_hex(part.before) if part.before else '-'}:"
                f"{_hex(part.after) if part.after else '-'}"
                for part in line.characters
            )
        else:
            characters = "-"
        records.append(
            f"{line.tag.value},{_number(line.before_line_number)},{_number(line.after_line_number)},"
            f"{_hex(line.before)},{_hex(line.after)},{characters}"
        )
    return f"before={_hex(result.before)}|after={_hex(result.after)}|lines={'/'.join(records)}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    source_root = args.source_root.resolve()
    sys.path.insert(0, str(source_root / "src"))

    import pynote
    from pynote.domain.diffing import diff_characters, diff_text
    from pynote.domain.models import Card, CardSource
    from pynote.infrastructure.export import NewlineFormat, export_cards, render_cards

    resolved_package = Path(pynote.__file__).resolve()
    if not resolved_package.is_relative_to(source_root):
        raise RuntimeError(f"pynote import escaped source root: {resolved_package}")

    def card(identifier: str, body: str, position: int, deleted: int | None = None) -> Card:
        return Card(
            id=identifier, document_id="document-1", operation_id="operation-1",
            position_key=position, capture_seq=position, created_at_us=1, updated_at_us=1,
            source=CardSource.TYPING, body=body, body_hash="hash",
            current_revision_id=f"revision-{identifier}", deleted_at_us=deleted,
        )

    def active_ids(cards: tuple[Card, ...]) -> str:
        ordered = sorted(
            (value for value in cards if value.deleted_at_us is None),
            key=lambda value: (value.position_key, value.id),
        )
        return ",".join(_hex(value.id) for value in ordered) or "-"

    lines: list[str] = []
    before = "첫 줄\n바뀌기 전\n삭제\n"
    after = "첫 줄\n바뀐 뒤\n추가\n"
    lines.append("WTL-W2-0113|" + _serialize_diff(diff_text(before, after)))

    lines.append("WTL-W2-0114|" + _serialize_diff(diff_text("A\nB\nC", "A\n새 줄")))

    before = "한글 A🧭B"
    after = "한글 A🧭C"
    changes = diff_characters(before, after)
    result = diff_text(before, after)
    if tuple(result.lines[0].characters) != tuple(changes):
        raise RuntimeError("character diff capture disagrees with text diff")
    lines.append("WTL-W2-0115|" + _serialize_diff(result))

    cards = (card("card-1", "첫 줄\n\n마지막 줄", 1_024),)
    content = render_cards(cards)
    lines.append(
        f"WTL-W2-0116|newline=0a|active={active_ids(cards)}|calls=0|result=ok"
        f"|content={_hex(content)}|error=-"
    )

    cards = (
        card("second", "둘째\r\n줄", 2_048),
        card("deleted", "제외", 3_072, 3),
        card("first", "첫째\n줄", 1_024),
    )
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "cards.MD"
        export_cards(path, cards, newline=NewlineFormat.CRLF)
        content = path.read_bytes().decode("utf-8")
    lines.append(
        f"WTL-W2-0117|newline=0d0a|active={active_ids(cards)}|calls=1|result=ok"
        f"|content={_hex(content)}|error=-"
    )

    error = "-"
    result = "ok"
    try:
        export_cards(Path("cards.json"), ())
    except ValueError:
        result = "invalid-suffix"
        error = "invalid-suffix"
    else:
        raise RuntimeError("invalid export suffix unexpectedly succeeded")
    lines.append(
        f"WTL-W2-0118|newline=0a|active=-|calls=0|result={result}|content=-|error={error}"
    )

    if len(lines) != 6:
        raise RuntimeError(f"expected 6 golden lines, got {len(lines)}")
    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
