from __future__ import annotations

from pynote.domain.diffing import DiffTag, diff_characters, diff_text


def test_diff_text_preserves_line_endings_and_adds_character_diff() -> None:
    result = diff_text(
        "첫 줄\n바뀌기 전\n삭제\n",
        "첫 줄\n바뀐 뒤\n추가\n",
    )

    assert [line.tag for line in result.lines] == [
        DiffTag.EQUAL,
        DiffTag.REPLACE,
        DiffTag.REPLACE,
    ]
    changed = result.lines[1]
    assert changed.before_line_number == 2
    assert changed.after_line_number == 2
    assert changed.before == "바뀌기 전\n"
    assert changed.after == "바뀐 뒤\n"
    assert {part.tag for part in changed.characters} >= {
        DiffTag.EQUAL,
        DiffTag.REPLACE,
    }


def test_diff_text_reports_unpaired_inserted_and_deleted_lines() -> None:
    result = diff_text("A\nB\nC", "A\n새 줄")

    assert result.lines[0].tag is DiffTag.EQUAL
    assert result.lines[1].tag is DiffTag.REPLACE
    assert result.lines[2].tag is DiffTag.DELETE
    assert result.lines[2].before_line_number == 3
    assert result.lines[2].after_line_number is None


def test_diff_characters_handles_unicode_without_changing_inputs() -> None:
    before = "한글 A🧭B"
    after = "한글 A🧭C"

    changes = diff_characters(before, after)

    assert "".join(change.before for change in changes) == before
    assert "".join(change.after for change in changes) == after
    assert changes[-1].tag is DiffTag.REPLACE
