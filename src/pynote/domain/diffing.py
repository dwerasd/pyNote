from __future__ import annotations

from dataclasses import dataclass
from difflib import SequenceMatcher
from enum import StrEnum


class DiffTag(StrEnum):
    """두 문자열 사이에서 한 구간이 갖는 변경 종류다."""

    EQUAL = "equal"
    INSERT = "insert"
    DELETE = "delete"
    REPLACE = "replace"


@dataclass(frozen=True, slots=True)
class CharacterDiff:
    """교체된 한 줄 안의 글자 단위 변경 구간이다."""

    tag: DiffTag
    before: str
    after: str


@dataclass(frozen=True, slots=True)
class LineDiff:
    """줄 번호와 글자 단위 변경을 포함한 한 줄의 차이다."""

    tag: DiffTag
    before_line_number: int | None
    after_line_number: int | None
    before: str
    after: str
    characters: tuple[CharacterDiff, ...] = ()


@dataclass(frozen=True, slots=True)
class TextDiff:
    """worker 경계를 안전하게 통과할 수 있는 불변 diff 결과다."""

    before: str
    after: str
    lines: tuple[LineDiff, ...]


def diff_characters(before: str, after: str) -> tuple[CharacterDiff, ...]:
    """두 줄을 글자 단위로 비교한다."""
    matcher = SequenceMatcher(a=before, b=after, autojunk=False)
    return tuple(
        CharacterDiff(
            tag=DiffTag(tag),
            before=before[before_start:before_end],
            after=after[after_start:after_end],
        )
        for tag, before_start, before_end, after_start, after_end in matcher.get_opcodes()
    )


def diff_text(before: str, after: str) -> TextDiff:
    """두 전체 문자열을 줄 단위로 비교하고 교체 줄에는 글자 diff를 붙인다."""
    before_lines = before.splitlines(keepends=True)
    after_lines = after.splitlines(keepends=True)
    matcher = SequenceMatcher(a=before_lines, b=after_lines, autojunk=False)
    lines: list[LineDiff] = []

    for tag_value, before_start, before_end, after_start, after_end in matcher.get_opcodes():
        tag = DiffTag(tag_value)
        if tag is DiffTag.EQUAL:
            for offset, (before_line, after_line) in enumerate(
                zip(
                    before_lines[before_start:before_end],
                    after_lines[after_start:after_end],
                    strict=True,
                )
            ):
                lines.append(
                    LineDiff(
                        tag=tag,
                        before_line_number=before_start + offset + 1,
                        after_line_number=after_start + offset + 1,
                        before=before_line,
                        after=after_line,
                    )
                )
            continue

        if tag is DiffTag.DELETE:
            for offset, before_line in enumerate(before_lines[before_start:before_end]):
                lines.append(
                    LineDiff(
                        tag=tag,
                        before_line_number=before_start + offset + 1,
                        after_line_number=None,
                        before=before_line,
                        after="",
                    )
                )
            continue

        if tag is DiffTag.INSERT:
            for offset, after_line in enumerate(after_lines[after_start:after_end]):
                lines.append(
                    LineDiff(
                        tag=tag,
                        before_line_number=None,
                        after_line_number=after_start + offset + 1,
                        before="",
                        after=after_line,
                    )
                )
            continue

        before_chunk = before_lines[before_start:before_end]
        after_chunk = after_lines[after_start:after_end]
        paired_count = min(len(before_chunk), len(after_chunk))
        for offset in range(paired_count):
            before_line = before_chunk[offset]
            after_line = after_chunk[offset]
            lines.append(
                LineDiff(
                    tag=DiffTag.REPLACE,
                    before_line_number=before_start + offset + 1,
                    after_line_number=after_start + offset + 1,
                    before=before_line,
                    after=after_line,
                    characters=diff_characters(before_line, after_line),
                )
            )
        for offset, before_line in enumerate(before_chunk[paired_count:]):
            lines.append(
                LineDiff(
                    tag=DiffTag.DELETE,
                    before_line_number=before_start + paired_count + offset + 1,
                    after_line_number=None,
                    before=before_line,
                    after="",
                )
            )
        for offset, after_line in enumerate(after_chunk[paired_count:]):
            lines.append(
                LineDiff(
                    tag=DiffTag.INSERT,
                    before_line_number=None,
                    after_line_number=after_start + paired_count + offset + 1,
                    before="",
                    after=after_line,
                )
            )

    return TextDiff(before=before, after=after, lines=tuple(lines))
