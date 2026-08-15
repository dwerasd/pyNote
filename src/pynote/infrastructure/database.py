from __future__ import annotations

import logging
import sqlite3
import time
from collections.abc import Callable, Iterator
from contextlib import contextmanager
from pathlib import Path
from types import TracebackType

from pynote.infrastructure.migrations import LATEST_SCHEMA_VERSION, MIGRATIONS

LOGGER = logging.getLogger(__name__)

BackupHook = Callable[[Path, int, int], None]


class Database:
    """WAL과 FK 검증을 포함하는 SQLite 연결 수명 주기를 관리한다."""

    def __init__(self, path: Path, backup_hook: BackupHook | None = None) -> None:
        self.path = path
        self._backup_hook = backup_hook
        self._had_database = path.is_file() and path.stat().st_size > 0
        self._connection = self._open()
        try:
            self._migrate()
        except BaseException:
            self._connection.close()
            raise

    @property
    def connection(self) -> sqlite3.Connection:
        return self._connection

    @property
    def schema_version(self) -> int:
        return self._read_schema_version()

    def close(self) -> None:
        self._connection.close()

    def __enter__(self) -> Database:
        return self

    def __exit__(
        self,
        exception_type: type[BaseException] | None,
        exception: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        self.close()

    @contextmanager
    def transaction(self) -> Iterator[None]:
        """즉시 쓰기 잠금을 잡고 실패 시 전체 변경을 롤백한다."""
        if self._connection.in_transaction:
            raise RuntimeError("중첩 트랜잭션은 지원하지 않습니다.")
        self._connection.execute("BEGIN IMMEDIATE")
        try:
            yield
            self._connection.commit()
        except BaseException:
            LOGGER.exception("SQLite 쓰기 트랜잭션이 실패하여 롤백합니다.")
            self._connection.rollback()
            raise

    def _open(self) -> sqlite3.Connection:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        connection = sqlite3.connect(self.path, isolation_level=None)
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA foreign_keys = ON")
        foreign_keys = connection.execute("PRAGMA foreign_keys").fetchone()
        if foreign_keys is None or foreign_keys[0] != 1:
            connection.close()
            raise RuntimeError("SQLite foreign_keys 활성화에 실패했습니다.")

        journal_mode = connection.execute("PRAGMA journal_mode = WAL").fetchone()
        if journal_mode is None or str(journal_mode[0]).lower() != "wal":
            connection.close()
            raise RuntimeError("SQLite WAL 모드 활성화 검증에 실패했습니다.")
        return connection

    def _read_schema_version(self) -> int:
        table = self._connection.execute(
            """
            SELECT 1
            FROM sqlite_master
            WHERE type = 'table' AND name = 'schema_version'
            """
        ).fetchone()
        if table is None:
            return 0
        row = self._connection.execute("SELECT version FROM schema_version WHERE id = 1").fetchone()
        return 0 if row is None else int(row[0])

    def _migrate(self) -> None:
        current_version = self._read_schema_version()
        if current_version > LATEST_SCHEMA_VERSION:
            raise RuntimeError(f"지원하지 않는 schema version입니다: {current_version}")
        pending = [
            (version, migration) for version, migration in MIGRATIONS if version > current_version
        ]
        if not pending:
            return

        if self._had_database and self._backup_hook is not None:
            try:
                self._backup_hook(self.path, current_version, LATEST_SCHEMA_VERSION)
            except BaseException:
                LOGGER.exception("schema migration 전 백업 훅이 실패했습니다.")
                raise

        try:
            with self.transaction():
                for _, migration in pending:
                    migration(self._connection, time.time_ns() // 1_000)
        except BaseException:
            LOGGER.exception(
                "schema migration에 실패했습니다: %s -> %s",
                current_version,
                LATEST_SCHEMA_VERSION,
            )
            raise
