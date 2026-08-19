from __future__ import annotations

import argparse
import sys
from pathlib import Path
def _hex(text: str) -> str:
    return text.encode("utf-8").hex()


def _line(stable_id: str, parser: object, text: str, *, keep: bool = False) -> str:
    split = parser.split(text)
    fields = [
        stable_id,
        f"input={_hex(text)}",
        f"split={','.join(_hex(part) for part in split)}",
        f"zero={int(parser.is_zero_paragraph_input(text))}",
    ]
    if keep:
        fields.append(f"keep={_hex(parser.keep(text))}")
    return "|".join(fields)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    source_root = args.source_root.resolve()
    sys.path.insert(0, str(source_root / "src"))

    import pynote
    from pynote.domain.paragraph_parser import ParagraphParser

    resolved_package = Path(pynote.__file__).resolve()
    if not resolved_package.is_relative_to(source_root):
        raise RuntimeError(f"pynote import escaped source root: {resolved_package}")

    default = ParagraphParser()
    cases = [
        "", " ", "\t", " \t \t", "\r", "\r\r", "\n", "\r\n",
        " \t\r\n\t \n", " \r\r\n\t", "\v", "\f", "\v\f", "\x1c",
        "\x1d", "\x1e", "\x1f", "\u00a0", "\u2003",
        " \t\v\f\x1c\x1d\x1e\x1f\u00a0\u2003\r\n",
    ]
    lines = [_line(f"WTL-W2-{119 + index:04d}", default, text) for index, text in enumerate(cases)]

    all_whitespace = "".join(chr(codepoint) for codepoint in range(0x110000) if chr(codepoint).isspace())
    lines.append(_line("WTL-W2-0139", default, all_whitespace))
    lines.append(_line("WTL-W2-0140", default, "첫 문단의 첫 줄\n첫 문단의 둘째 줄"))
    lines.append(_line("WTL-W2-0141", default, "첫 문단\n\n둘째 문단\n\n\n\n셋째 문단"))
    lines.append(_line("WTL-W2-0142", default, "첫 줄\r\n둘째 줄\r\n\r\n다음 문단"))
    lines.append(_line("WTL-W2-0143", default, "첫 줄\n둘째 줄\n\n다음 문단"))
    lines.append(_line("WTL-W2-0144", default, "첫 줄\r\n둘째 줄\n\r\n다음 문단"))
    lines.append(_line("WTL-W2-0145", default, "첫 문단\n \t \n둘째 문단\n\t\n셋째 문단"))
    lines.append(_line("WTL-W2-0146", default, "첫 문단\n\v\f\n둘째 문단"))
    lines.append(_line("WTL-W2-0147", default, "첫 문단\n\u00a0\u2003\n둘째 문단"))
    lines.append(_line("WTL-W2-0148", default, "\v첫 줄\f"))
    nul_first = "\x00"
    nul_second = "\x00\n\n본문"
    lines.append(
        "WTL-W2-0149"
        f"|input0={_hex(nul_first)}|split0={_hex(nul_first)}|zero0=0"
        f"|input1={_hex(nul_second)}|split1={_hex(nul_first)},{_hex('본문')}|zero1=0"
    )
    lines.append(_line("WTL-W2-0150", default, "첫\r줄\n \r \n둘째 문단\r"))
    lines.append(_line("WTL-W2-0151", default, "한글 😀 e\u0301\n두 번째 줄\n\n👩🏽‍💻과 값"))
    for offset, suffix in enumerate(("", "\n", "\r\n", "\n\n", "\r\n\r\n", "\n \t\n")):
        lines.append(_line(f"WTL-W2-{152 + offset:04d}", default, f"마지막 문단{suffix}"))
    lines.append(_line("WTL-W2-0158", default, "\r\n \t\n\n첫 문단"))
    lines.append(_line("WTL-W2-0159", default, "\r\n 첫 줄 \r\n\t\r\n둘째 문단\n\n", keep=True))
    lines.append(_line("WTL-W2-0160", default, "\r\n첫 줄\r\n둘째 줄\r\n \t\r\n\r\n둘째 문단\n\n\n", keep=True))

    class PipePolicy:
        def split(self, text: str) -> tuple[str, ...]:
            return tuple(part for part in text.split("|") if part)

    lines.append(_line("WTL-W2-0161", ParagraphParser(PipePolicy()), "첫 문단|둘째 문단"))
    performance_text = "\r\n \t\r\n".join(f"문단 {index} 😀" for index in range(10_000))
    performance_result = default.split(performance_text)
    lines.append(
        "WTL-W2-0162"
        f"|count={len(performance_result)}"
        f"|first={_hex(performance_result[0])}"
        f"|last={_hex(performance_result[-1])}"
    )

    if len(lines) != 44:
        raise RuntimeError(f"expected 44 golden lines, got {len(lines)}")
    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
