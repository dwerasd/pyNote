from __future__ import annotations

import sys
from pathlib import Path


def _bootstrap_src() -> None:
    source_path = str(Path(__file__).resolve().parent / "src")
    if source_path not in sys.path:
        sys.path.insert(0, source_path)


if __name__ == "__main__":
    _bootstrap_src()

    from pynote.app import main

    raise SystemExit(main())
