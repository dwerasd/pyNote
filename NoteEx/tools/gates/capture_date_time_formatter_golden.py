from __future__ import annotations

import argparse
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    source_root = args.source_root.resolve()
    sys.path.insert(0, str(source_root / "src"))

    import pynote
    from PySide6.QtCore import QDate, QDateTime, QLocale, QTime, QTimeZone

    def verify_imports() -> None:
        for name, module in tuple(sys.modules.items()):
            if name != "pynote" and not name.startswith("pynote."):
                continue
            module_file = getattr(module, "__file__", None)
            if module_file is not None and not Path(module_file).resolve().is_relative_to(source_root):
                raise RuntimeError(f"pynote import escaped source root: {name}={module_file}")

    verify_imports()
    if not Path(pynote.__file__).resolve().is_relative_to(source_root):
        raise RuntimeError(f"pynote import escaped source root: {pynote.__file__}")

    utc = QTimeZone.fromSecondsAheadOfUtc(0)
    seoul = QTimeZone(b"Asia/Seoul")
    if not utc.isValid() or not seoul.isValid():
        raise RuntimeError("required Qt time zone is unavailable")

    def value(hour: int = 12, millisecond: int = 120) -> QDateTime:
        return QDateTime(QDate(2024, 1, 2), QTime(hour, 4, 5, millisecond), utc)

    def seoul_value() -> QDateTime:
        return QDateTime(QDate(2024, 1, 2), QTime(12, 4, 5, 120), seoul)

    records: list[str] = []

    def record(label: str, date_time: QDateTime, format_text: str) -> None:
        output = date_time.toString(format_text)
        records.append(
            f"{label}|format={format_text.encode('utf-8').hex()}|output={output.encode('utf-8').hex()}"
        )

    record("preset", value(), "yyyy-MM-dd HH:mm")
    record("date-tokens", value(), "d|dd|ddd|dddd|M|MM|MMM|MMMM|yy|yyyy")

    record("clock24-midnight", value(0), "h|hh|H|HH")
    record("clock24-afternoon", value(13), "h|hh|H|HH")
    record("clock12-midnight", value(0), "h|hh|H|HH|AP|A|ap|a|aP|Ap")
    record("clock12-afternoon", value(13), "h|hh|H|HH|AP|A|ap|a|aP|Ap")

    for millisecond in (0, 7, 40, 120, 250, 999):
        record(f"milliseconds-{millisecond:03d}", value(12, millisecond), "z|zz|zzz|zzzz|zzzzz")

    record("quote-paired", value(), "'Date:' yyyy")
    record("quote-doubled", value(), "'it''s' ''yyyy''")
    record("quote-unmatched", value(), "'open yyyy")
    record("literal-utf8", value(), "Q/X λ 한글")

    record("greedy", seoul_value(), "yyyyy|MMMMM|dddddd|HHHHH|zzzz|ttttt")
    record("timezone-seoul", seoul_value(), "t|tt|ttt|tttt")

    original_locale = QLocale()
    try:
        for locale_name in ("C", "ko_KR", "de_DE", "ar_EG"):
            QLocale.setDefault(QLocale(locale_name))
            record(f"locale-{locale_name}", value(13), "ddd|dddd|MMM|MMMM|AP|ap")
    finally:
        QLocale.setDefault(original_locale)

    record("empty-format", value(), "")
    record("invalid-flag", QDateTime(), "yyyy-MM-dd HH:mm")
    record(
        "invalid-fields",
        QDateTime(QDate(2024, 1, 32), QTime(12, 4, 5, 120), utc),
        "yyyy-MM-dd HH:mm",
    )

    verify_imports()
    if len(records) != 25:
        raise RuntimeError(f"expected 25 records, got {len(records)}")
    args.output.write_text("\n".join(records) + "\n", encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
