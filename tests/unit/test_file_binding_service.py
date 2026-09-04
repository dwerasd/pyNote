from __future__ import annotations

import codecs
import os
import sys
from pathlib import Path

import pytest

from pynote.application.file_binding_service import (
    _ANSI_ENCODING,
    DetectedText,
    PendingFileBinding,
    detect_text,
    has_control_chars,
    has_roundtrip_hazard,
    hash_bytes,
    read_file_hash,
    render_bytes,
    resolve_path,
)
from pynote.domain.models import FileBinding, NewlineKind

BOM_BY_ENCODING = {
    "utf-8": codecs.BOM_UTF8,
    "utf-16-le": codecs.BOM_UTF16_LE,
    "utf-16-be": codecs.BOM_UTF16_BE,
}

# 인코딩 5종: utf-8 / utf-8+BOM / utf-16 LE+BOM / utf-16 BE+BOM / ANSI(win32는 cp949).
GOLDEN_ENCODINGS = (
    ("utf-8", False),
    ("utf-8", True),
    ("utf-16-le", True),
    ("utf-16-be", True),
    (_ANSI_ENCODING, False),
)
GOLDEN_NEWLINES = (NewlineKind.LF, NewlineKind.CRLF)
GOLDEN_TRAILING = (True, False)
GOLDEN_BODY = "첫 줄 한글\n두 번째 줄 ASCII\n세 번째"
JSON_BODY = '{\n  "이름": "값",\n  "목록": [1, 2, 3]\n}'


def binding(
    *,
    encoding: str,
    bom: bool,
    newline: NewlineKind,
    trailing_newline: bool = True,
) -> FileBinding:
    return FileBinding(
        card_id="card-1",
        path="C:\\notes\\sample.txt",
        path_key="c:\\notes\\sample.txt",
        encoding=encoding,
        bom=bom,
        newline=newline,
        trailing_newline=trailing_newline,
        bound_at_us=1_000,
    )


def source_bytes(
    body: str,
    *,
    encoding: str,
    bom: bool,
    newline: NewlineKind,
    trailing_newline: bool,
) -> bytes:
    text = body.replace("\n", newline.characters)
    if trailing_newline:
        text += newline.characters
    prefix = BOM_BY_ENCODING[encoding] if bom else b""
    return prefix + text.encode(encoding)


def golden_cases() -> list[tuple[str, str, bool, NewlineKind, bool]]:
    cases = [
        (body, encoding, bom, newline, trailing)
        for body in (GOLDEN_BODY,)
        for encoding, bom in GOLDEN_ENCODINGS
        for newline in GOLDEN_NEWLINES
        for trailing in GOLDEN_TRAILING
    ]
    cases.append((JSON_BODY, "utf-8", False, NewlineKind.CRLF, True))
    return cases


@pytest.mark.parametrize(("body", "encoding", "bom", "newline", "trailing"), golden_cases())
def test_detect_then_render_reproduces_original_bytes(
    body: str,
    encoding: str,
    bom: bool,
    newline: NewlineKind,
    trailing: bool,
) -> None:
    data = source_bytes(
        body,
        encoding=encoding,
        bom=bom,
        newline=newline,
        trailing_newline=trailing,
    )

    detected = detect_text(data)

    assert detected is not None
    assert detected.encoding == encoding
    assert detected.bom is bom
    assert detected.newline is newline
    assert detected.trailing_newline is trailing
    assert detected.text == (body + "\n" if trailing else body)
    restored = render_bytes(
        detected.text,
        binding(
            encoding=detected.encoding,
            bom=detected.bom,
            newline=detected.newline,
            trailing_newline=detected.trailing_newline,
        ),
    )
    assert restored == data


def test_ansi_fixture_is_not_valid_utf8_so_the_fallback_is_exercised() -> None:
    data = GOLDEN_BODY.encode(_ANSI_ENCODING)

    with pytest.raises(UnicodeDecodeError):
        data.decode("utf-8")
    detected = detect_text(data)
    assert detected is not None
    assert detected.encoding == _ANSI_ENCODING


def test_utf16_endianness_comes_from_the_bom_not_from_the_platform() -> None:
    little = detect_text(codecs.BOM_UTF16_LE + "가\n".encode("utf-16-le"))
    big = detect_text(codecs.BOM_UTF16_BE + "가\n".encode("utf-16-be"))

    assert little is not None
    assert big is not None
    assert little.encoding == "utf-16-le"
    assert big.encoding == "utf-16-be"
    assert little.text == big.text == "가\n"


def test_empty_file_detects_as_empty_text_without_trailing_newline() -> None:
    platform_default = NewlineKind.CRLF if sys.platform == "win32" else NewlineKind.LF

    assert detect_text(b"") == DetectedText(
        text="",
        encoding="utf-8",
        bom=False,
        newline=platform_default,
        trailing_newline=False,
    )


@pytest.mark.parametrize(
    ("text", "expected"),
    [
        ("a\nb", NewlineKind.LF),
        ("a\r\nb", NewlineKind.CRLF),
        ("a\rb", NewlineKind.CR),
        # 혼합 줄끝은 가장 먼저 나온 것으로 수렴한다.
        ("a\nb\r\nc", NewlineKind.LF),
        ("a\r\nb\nc", NewlineKind.CRLF),
        ("a\rb\nc", NewlineKind.CR),
    ],
)
def test_newline_detection_takes_the_first_line_ending(
    text: str,
    expected: NewlineKind,
) -> None:
    detected = detect_text(text.encode("utf-8"))

    assert detected is not None
    assert detected.newline is expected
    assert "\r" not in detected.text


def test_card_body_normalizes_every_line_ending_to_lf() -> None:
    detected = detect_text("첫\r\n둘\r셋\n".encode())

    assert detected is not None
    assert detected.text == "첫\n둘\n셋\n"


def test_png_header_passes_ansi_decoding_but_the_control_gate_rejects_it() -> None:
    data = b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR"

    assert has_control_chars(data.decode(_ANSI_ENCODING)) is True
    assert detect_text(data) is None


@pytest.mark.parametrize("code_point", [0x00A0, 0x2028, 0x2029, 0xFDD0, 0xFDD1])
def test_roundtrip_hazard_characters_block_binding(code_point: int) -> None:
    text = "앞" + chr(code_point) + "뒤"

    assert has_roundtrip_hazard(text) is True
    assert detect_text(text.encode("utf-8")) is None


def test_lone_high_bytes_fail_strict_decoding() -> None:
    assert detect_text(bytes(range(0x80, 0x100))) is None


@pytest.mark.parametrize("code_point", [0x00, 0x01, 0x1F, 0x7F])
def test_control_characters_block_binding(code_point: int) -> None:
    text = "앞" + chr(code_point) + "뒤"

    assert has_control_chars(text) is True
    assert detect_text(text.encode("utf-8")) is None


@pytest.mark.parametrize("character", ["\t", "\n", "\r", "\f", "\v"])
def test_whitespace_control_characters_stay_allowed(character: str) -> None:
    assert has_control_chars("앞" + character + "뒤") is False


def test_render_does_not_replace_characters_the_encoding_cannot_represent() -> None:
    target = binding(encoding="cp949", bom=False, newline=NewlineKind.LF)

    with pytest.raises(UnicodeEncodeError):
        render_bytes("이모지 🙂", target)


def test_render_rejects_a_bom_on_an_encoding_that_has_none() -> None:
    target = binding(encoding="cp949", bom=True, newline=NewlineKind.LF)

    with pytest.raises(ValueError, match="BOM"):
        render_bytes("본문", target)


def test_render_uses_the_binding_newline_for_every_line() -> None:
    target = binding(encoding="utf-8", bom=False, newline=NewlineKind.CRLF)

    assert render_bytes("a\nb\n", target) == b"a\r\nb\r\n"


def test_resolve_path_returns_an_absolute_resolved_path_and_its_normcase_key(
    tmp_path: Path,
) -> None:
    target = tmp_path / "Sub" / ".." / "Note.TXT"

    path, path_key = resolve_path(target)

    assert Path(path).is_absolute()
    assert ".." not in path
    assert path.endswith("Note.TXT")
    assert path_key == os.path.normcase(path)


@pytest.mark.skipif(sys.platform != "win32", reason="대소문자 무시 경로 비교는 Windows 규칙이다")
def test_path_key_ignores_case_on_windows(tmp_path: Path) -> None:
    _, lower_key = resolve_path(tmp_path / "note.txt")
    _, upper_key = resolve_path(tmp_path / "NOTE.TXT")

    assert lower_key == upper_key


def test_pending_binding_derives_its_path_key_from_the_path() -> None:
    pending = PendingFileBinding(
        path="C:\\Notes\\A.TXT",
        encoding="utf-8",
        bom=False,
        newline=NewlineKind.CRLF,
        trailing_newline=True,
    )

    assert pending.path_key == os.path.normcase("C:\\Notes\\A.TXT")


def test_read_file_hash_matches_the_bytes_on_disk(tmp_path: Path) -> None:
    path = tmp_path / "note.txt"
    path.write_bytes("본문\n".encode())

    assert read_file_hash(path) == hash_bytes("본문\n".encode())
    assert read_file_hash(tmp_path / "없는파일.txt") is None
