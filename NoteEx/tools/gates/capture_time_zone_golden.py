from __future__ import annotations

import argparse
import re
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path, PurePosixPath
from zoneinfo import ZoneInfo


EPOCH = datetime(1970, 1, 1, tzinfo=timezone.utc)
FIXED_RE = re.compile(r"UTC([+-])(\d{2}):(\d{2})\Z")


def epoch_us(year: int, month: int, day: int, hour: int, minute: int, second: int, millisecond: int = 0) -> int:
    return int((datetime(year, month, day, hour, minute, second, millisecond * 1000, tzinfo=timezone.utc) - EPOCH) // timedelta(microseconds=1))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--zoneinfo-root", required=True, type=Path)
    parser.add_argument("--ids", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    source_root = args.source_root.resolve()
    sys.path.insert(0, str(source_root / "src"))
    import pynote
    from PySide6.QtCore import QDateTime, QTimeZone

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
    ids = args.ids.read_text(encoding="ascii").splitlines()
    if len(ids) != 488 or len(set(ids)) != 488 or ids != sorted(ids, key=lambda value: value.encode()):
        raise RuntimeError("tracked ID surface is not the exact sorted 488-ID set")

    links: dict[str, str] = {}
    for line in args.zoneinfo_root.joinpath("tzdata.zi").read_text(encoding="ascii").splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[0] == "L":
            links[fields[2]] = fields[1]

    def payload(identifier: str) -> Path:
        relative = PurePosixPath(identifier)
        if relative.is_absolute() or ".." in relative.parts or "." in relative.parts:
            raise RuntimeError(f"unsafe zone ID: {identifier!r}")
        current = identifier
        visited: set[str] = set()
        while True:
            path = args.zoneinfo_root.joinpath(*PurePosixPath(current).parts)
            try:
                if path.is_file() and path.read_bytes().startswith(b"TZif"):
                    return path
            except OSError:
                pass
            if current in visited or current not in links:
                raise RuntimeError(f"unreadable TZif: {identifier}")
            visited.add(current); current = links[current]

    system_zone = QTimeZone.systemTimeZone()
    if not system_zone.isValid():
        raise RuntimeError("Qt system time zone is unavailable")
    cache: dict[str, ZoneInfo] = {}

    def resolve(identifier: str, instant_us: int) -> tuple[bool, str, int | None]:
        utc = EPOCH + timedelta(microseconds=instant_us)
        if identifier == "system":
            qt_value = QDateTime.fromMSecsSinceEpoch(instant_us // 1000, system_zone)
            if not qt_value.isValid():
                return False, "-", None
            local = qt_value.date().toString("yyyy-MM-dd") + "T" + qt_value.time().toString("HH:mm:ss.zzz")
            return True, local, qt_value.offsetFromUtc()
        if identifier == "UTC":
            zone = timezone.utc
        elif identifier in ids and (match := FIXED_RE.fullmatch(identifier)):
            hours, minutes = int(match.group(2)), int(match.group(3))
            zone = timezone((1 if match.group(1) == "+" else -1) * timedelta(hours=hours, minutes=minutes))
        elif identifier in ids:
            if identifier not in cache:
                with payload(identifier).open("rb") as source:
                    cache[identifier] = ZoneInfo.from_file(source, key=identifier)
            zone = cache[identifier]
        else:
            return False, "-", None
        local_value = utc.astimezone(zone)
        offset = local_value.utcoffset()
        if offset is None:
            return False, "-", None
        return True, local_value.strftime("%Y-%m-%dT%H:%M:%S.") + f"{local_value.microsecond // 1000:03d}", int(offset.total_seconds())

    rows: list[str] = []
    def row(name: str, identifier: str, instant_us: int) -> None:
        valid, local, offset = resolve(identifier, instant_us)
        rows.append(f"T4A-UNC-004|case={name}|id={identifier.encode().hex()}|epoch_us={instant_us}|valid={int(valid)}|local={local}|offset={offset if valid else '-'}")

    winter = epoch_us(2024, 1, 15, 12, 0, 0, 123)
    summer = epoch_us(2024, 7, 15, 12, 0, 0, 456)
    row("system-winter", "system", winter); row("system-summer", "system", summer)
    for index, identifier in enumerate(ids):
        row(f"id-{index:03d}-winter", identifier, winter)
        row(f"id-{index:03d}-summer", identifier, summer)
    boundaries = (
        ("ny-spring-before", "America/New_York", epoch_us(2024,3,10,6,59,59)), ("ny-spring-at", "America/New_York", epoch_us(2024,3,10,7,0,0)),
        ("ny-fall-before", "America/New_York", epoch_us(2024,11,3,5,59,59)), ("ny-fall-at", "America/New_York", epoch_us(2024,11,3,6,0,0)),
        ("juarez-spring-before", "America/Ciudad_Juarez", epoch_us(2024,3,10,8,59,59)), ("juarez-spring-at", "America/Ciudad_Juarez", epoch_us(2024,3,10,9,0,0)),
        ("juarez-fall-before", "America/Ciudad_Juarez", epoch_us(2024,11,3,7,59,59)), ("juarez-fall-at", "America/Ciudad_Juarez", epoch_us(2024,11,3,8,0,0)),
        ("vostok-before", "Antarctica/Vostok", epoch_us(2023,12,17,18,59,59)), ("vostok-at", "Antarctica/Vostok", epoch_us(2023,12,17,19,0,0)),
        ("urumqi-winter", "Asia/Urumqi", winter), ("urumqi-summer", "Asia/Urumqi", summer),
        ("seoul-winter", "Asia/Seoul", winter), ("seoul-summer", "Asia/Seoul", summer),
    )
    for values in boundaries:
        row(*values)
    invalid_ids = ("", "utc", "Asia/Seoul ", "Invalid/Zone", "UTC+9:00", "UTC+03:15", "UTC+15:00", "UTC+14:30", "UTC-00")
    for index, identifier in enumerate(invalid_ids):
        row(f"invalid-{index}", identifier, winter)
    if len(rows) != 2 + 976 + 14 + len(invalid_ids):
        raise RuntimeError(f"unexpected row count: {len(rows)}")
    verify_imports()
    args.output.write_text("\n".join(rows) + "\n", encoding="ascii", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
