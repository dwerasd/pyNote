from __future__ import annotations

from time import perf_counter

import pytest

from pynote.domain.paragraph_parser import ParagraphParser


@pytest.fixture
def parser() -> ParagraphParser:
    return ParagraphParser()


@pytest.mark.parametrize(
    "text",
    [
        "",
        " ",
        "\t",
        " \t \t",
        "\r",
        "\r\r",
        "\n",
        "\r\n",
        " \t\r\n\t \n",
        " \r\r\n\t",
        pytest.param("\v", id="vertical-tab"),
        pytest.param("\f", id="form-feed"),
        pytest.param("\v\f", id="vertical-tab-form-feed"),
        pytest.param("\x1c", id="file-separator"),
        pytest.param("\x1d", id="group-separator"),
        pytest.param("\x1e", id="record-separator"),
        pytest.param("\x1f", id="unit-separator"),
        pytest.param("\u00a0", id="no-break-space"),
        pytest.param("\u2003", id="em-space"),
        pytest.param(
            " \t\v\f\x1c\x1d\x1e\x1f\u00a0\u2003\r\n",
            id="mixed-runtime-whitespace",
        ),
    ],
)
def test_empty_or_whitespace_only_input_has_zero_paragraphs(
    parser: ParagraphParser,
    text: str,
) -> None:
    assert parser.split(text) == ()
    assert parser.is_zero_paragraph_input(text)


def test_all_runtime_whitespace_characters_have_zero_paragraphs(
    parser: ParagraphParser,
) -> None:
    all_whitespace = "".join(
        chr(codepoint)
        for codepoint in range(0x110000)
        if chr(codepoint).isspace()
    )

    assert all_whitespace
    assert parser.split(all_whitespace) == ()
    assert parser.is_zero_paragraph_input(all_whitespace) is True


def test_single_paragraph_preserves_internal_single_line_break(
    parser: ParagraphParser,
) -> None:
    text = "첫 문단의 첫 줄\n첫 문단의 둘째 줄"

    assert parser.split(text) == (text,)
    assert not parser.is_zero_paragraph_input(text)


def test_multiple_paragraphs_are_split_on_one_or_more_blank_lines(
    parser: ParagraphParser,
) -> None:
    text = "첫 문단\n\n둘째 문단\n\n\n\n셋째 문단"

    assert parser.split(text) == ("첫 문단", "둘째 문단", "셋째 문단")


@pytest.mark.parametrize(
    ("text", "expected"),
    [
        ("첫 줄\r\n둘째 줄\r\n\r\n다음 문단", ("첫 줄\n둘째 줄", "다음 문단")),
        ("첫 줄\n둘째 줄\n\n다음 문단", ("첫 줄\n둘째 줄", "다음 문단")),
        ("첫 줄\r\n둘째 줄\n\r\n다음 문단", ("첫 줄\n둘째 줄", "다음 문단")),
    ],
)
def test_crlf_lf_and_mixed_line_breaks_are_normalized(
    parser: ParagraphParser,
    text: str,
    expected: tuple[str, ...],
) -> None:
    assert parser.split(text) == expected


def test_space_and_tab_only_lines_are_paragraph_boundaries(
    parser: ParagraphParser,
) -> None:
    text = "첫 문단\n \t \n둘째 문단\n\t\n셋째 문단"

    assert parser.split(text) == ("첫 문단", "둘째 문단", "셋째 문단")


@pytest.mark.parametrize(
    "boundary_line",
    [
        pytest.param("\v\f", id="vertical-tab-form-feed"),
        pytest.param("\u00a0\u2003", id="unicode-whitespace"),
    ],
)
def test_extended_whitespace_only_lines_are_paragraph_boundaries(
    parser: ParagraphParser,
    boundary_line: str,
) -> None:
    text = f"첫 문단\n{boundary_line}\n둘째 문단"

    assert parser.split(text) == ("첫 문단", "둘째 문단")


def test_nonblank_line_whitespace_is_preserved(parser: ParagraphParser) -> None:
    text = "\v첫 줄\f"

    assert parser.split(text) == (text,)


def test_nul_remains_nonblank_content(parser: ParagraphParser) -> None:
    assert parser.split("\x00") == ("\x00",)
    assert parser.is_zero_paragraph_input("\x00") is False
    assert parser.split("\x00\n\n본문") == ("\x00", "본문")
    assert parser.is_zero_paragraph_input("\x00\n\n본문") is False


def test_lone_cr_blank_line_preserves_nonblank_paragraph_text(
    parser: ParagraphParser,
) -> None:
    text = "첫\r줄\n \r \n둘째 문단\r"

    assert parser.split(text) == ("첫\r줄", "둘째 문단\r")


def test_unicode_text_is_preserved(parser: ParagraphParser) -> None:
    combined = "한글 😀 e\u0301"
    text = f"{combined}\n두 번째 줄\n\n👩🏽‍💻과 값"

    assert parser.split(text) == (f"{combined}\n두 번째 줄", "👩🏽‍💻과 값")


@pytest.mark.parametrize(
    "suffix",
    [
        "",
        "\n",
        "\r\n",
        "\n\n",
        "\r\n\r\n",
        "\n \t\n",
    ],
)
def test_trailing_line_breaks_are_excluded_from_paragraph(
    parser: ParagraphParser,
    suffix: str,
) -> None:
    assert parser.split(f"마지막 문단{suffix}") == ("마지막 문단",)


def test_leading_boundary_lines_are_excluded(parser: ParagraphParser) -> None:
    text = "\r\n \t\n\n첫 문단"

    assert parser.split(text) == ("첫 문단",)


def test_keep_returns_the_exact_original_text(parser: ParagraphParser) -> None:
    original_text = "\r\n 첫 줄 \r\n\t\r\n둘째 문단\n\n"

    kept_text = parser.keep(original_text)

    assert kept_text == original_text
    assert kept_text.encode() == original_text.encode()


def _restore_original_text(
    parser: ParagraphParser,
    paragraphs: tuple[str, ...],
    original_text: str,
) -> str:
    if parser.split(original_text) != paragraphs:
        raise ValueError("분리 결과가 저장된 원문과 일치하지 않습니다.")
    return original_text


def test_split_result_and_stored_original_text_round_trip(
    parser: ParagraphParser,
) -> None:
    original_text = "\r\n첫 줄\r\n둘째 줄\r\n \t\r\n\r\n둘째 문단\n\n\n"
    paragraphs = parser.split(original_text)

    restored_text = _restore_original_text(parser, paragraphs, original_text)

    assert paragraphs == ("첫 줄\n둘째 줄", "둘째 문단")
    assert restored_text == original_text
    assert restored_text.encode() == original_text.encode()


def test_split_policy_is_replaceable() -> None:
    class PipeParagraphPolicy:
        def split(self, text: str) -> tuple[str, ...]:
            return tuple(part for part in text.split("|") if part)

    parser = ParagraphParser(PipeParagraphPolicy())

    assert parser.split("첫 문단|둘째 문단") == ("첫 문단", "둘째 문단")


def test_ten_thousand_paragraphs_finish_within_five_seconds(
    parser: ParagraphParser,
) -> None:
    text = "\r\n \t\r\n".join(f"문단 {index} 😀" for index in range(10_000))

    started_at = perf_counter()
    paragraphs = parser.split(text)
    elapsed_seconds = perf_counter() - started_at

    assert len(paragraphs) == 10_000
    assert paragraphs[0] == "문단 0 😀"
    assert paragraphs[-1] == "문단 9999 😀"
    assert elapsed_seconds < 5.0
