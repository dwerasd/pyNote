from __future__ import annotations

from collections.abc import Iterator
from pathlib import Path

import pytest

from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories


@pytest.fixture
def database_path(tmp_path: Path) -> Path:
    return tmp_path / "pynote-test.sqlite3"


@pytest.fixture
def database(database_path: Path) -> Iterator[Database]:
    instance = Database(database_path)
    yield instance
    instance.close()


@pytest.fixture
def repositories(database: Database) -> Repositories:
    return Repositories(database)
