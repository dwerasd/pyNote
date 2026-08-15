from __future__ import annotations

import hashlib
import logging
import os
import sqlite3
import tempfile
import time
from collections.abc import Callable, Iterable
from dataclasses import dataclass
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Never

from pynote.infrastructure.migrations import LATEST_SCHEMA_VERSION

LOGGER = logging.getLogger(__name__)

_V1_REQUIRED_TABLES = {
    "schema_version",
    "documents",
    "capture_operations",
    "cards",
    "card_revisions",
    "drafts",
    "edit_events",
    "card_lineage",
    "counters",
    "workspace_state",
    "document_ui_states",
}
_V2_REQUIRED_TABLES = _V1_REQUIRED_TABLES | {"data_policy_settings"}
_V4_REQUIRED_TABLES = (
    _V2_REQUIRED_TABLES - {"workspace_state"}
) | {"workspace_windows"}


class BackupError(RuntimeError):
    """백업 또는 복원에 실패했음을 나타낸다."""


class BackupIntegrityError(BackupError):
    """SQLite 무결성 검사를 통과하지 못한 백업 오류다."""


class UnsupportedBackupError(BackupError):
    """현재 앱보다 새로운 schema의 백업 오류다."""


@dataclass(frozen=True, slots=True)
class BackupInspection:
    """검증을 마친 백업의 불변 메타데이터다."""

    path: Path
    schema_version: int


def run_quick_check(connection: sqlite3.Connection) -> None:
    """열린 SQLite 연결에 quick_check를 실행하고 실패를 예외로 전파한다."""
    try:
        rows = connection.execute("PRAGMA quick_check").fetchall()
    except sqlite3.DatabaseError as error:
        LOGGER.exception("SQLite quick_check 실행에 실패했습니다.")
        raise BackupIntegrityError("SQLite quick_check를 실행할 수 없습니다.") from error
    messages = tuple(str(row[0]) for row in rows)
    if messages != ("ok",):
        message = "; ".join(messages) if messages else "검사 결과 없음"
        LOGGER.error("SQLite quick_check가 무결성 오류를 보고했습니다: %s", message)
        raise BackupIntegrityError(f"SQLite 무결성 검사에 실패했습니다: {message}")


def inspect_backup(path: Path) -> BackupInspection:
    """백업 파일의 SQLite 무결성과 지원 schema version을 검증한다."""
    if not path.is_file():
        raise BackupIntegrityError(f"백업 파일이 없습니다: {path}")
    try:
        connection = _open_read_only(path)
    except sqlite3.Error as error:
        LOGGER.exception("백업 파일을 SQLite로 열지 못했습니다: %s", path)
        raise BackupIntegrityError("올바른 SQLite 백업 파일이 아닙니다.") from error
    try:
        run_quick_check(connection)
        schema_version = _read_schema_version(connection)
        if 0 <= schema_version <= LATEST_SCHEMA_VERSION:
            _validate_schema_tables(connection, schema_version)
            _validate_foreign_keys(connection)
            if schema_version >= 1:
                _validate_logical_integrity(connection)
    except BackupError:
        raise
    except (TypeError, ValueError, sqlite3.DatabaseError) as error:
        LOGGER.exception("백업 무결성 정보를 검사하지 못했습니다: %s", path)
        raise BackupIntegrityError("백업 무결성 정보를 검사할 수 없습니다.") from error
    finally:
        connection.close()

    if schema_version < 0 or schema_version > LATEST_SCHEMA_VERSION:
        LOGGER.error(
            "지원하지 않는 백업 schema version입니다: %s (최대 %s)",
            schema_version,
            LATEST_SCHEMA_VERSION,
        )
        raise UnsupportedBackupError(
            f"지원하지 않는 백업 schema version입니다: {schema_version}"
        )
    return BackupInspection(path=path, schema_version=schema_version)


def create_database_backup(source: Path, destination: Path) -> BackupInspection:
    """SQLite 온라인 백업을 새 파일에 원자적으로 생성하고 검증한다."""
    if not source.is_file():
        raise BackupError(f"백업할 데이터베이스 파일이 없습니다: {source}")
    if destination.exists():
        raise FileExistsError(f"기존 백업을 덮어쓰지 않습니다: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = _temporary_database_path(destination)
    try:
        source_connection = _open_read_only(source)
        try:
            destination_connection = sqlite3.connect(temporary_path)
            try:
                source_connection.backup(destination_connection)
            finally:
                destination_connection.close()
        finally:
            source_connection.close()
        inspection = inspect_backup(temporary_path)
        os.replace(temporary_path, destination)
        return BackupInspection(
            path=destination,
            schema_version=inspection.schema_version,
        )
    except BaseException:
        LOGGER.exception("데이터베이스 백업 생성에 실패했습니다: %s", destination)
        raise
    finally:
        temporary_path.unlink(missing_ok=True)


def restore_database(
    backup_path: Path,
    destination: Path,
    *,
    overwrite: bool = False,
) -> BackupInspection:
    """검증된 백업을 닫힌 DB 경로에 원자적으로 복원한다."""
    inspect_backup(backup_path)
    database_paths = _database_file_set(destination)
    _validate_restore_targets(database_paths)
    existing_paths = tuple(path for path in database_paths if path.exists())
    if existing_paths and not overwrite:
        raise FileExistsError(f"기존 데이터베이스를 덮어쓰지 않습니다: {destination}")

    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = _temporary_database_path(destination)
    preserved_paths: dict[Path, Path] = {}
    moved_paths: list[Path] = []
    try:
        source_connection = _open_read_only(backup_path)
        try:
            destination_connection = sqlite3.connect(temporary_path)
            try:
                source_connection.backup(destination_connection)
            finally:
                destination_connection.close()
        finally:
            source_connection.close()
        restored = inspect_backup(temporary_path)
        preserved_paths = {
            path: _temporary_database_path(path) for path in existing_paths
        }
        try:
            for path in existing_paths:
                os.replace(path, preserved_paths[path])
                moved_paths.append(path)
            os.replace(temporary_path, destination)
        except BaseException as error:
            rollback_errors = _restore_preserved_database_set(
                destination=destination,
                temporary_path=temporary_path,
                moved_paths=moved_paths,
                preserved_paths=preserved_paths,
            )
            if rollback_errors:
                LOGGER.error(
                    "복원 실패 후 원본 DB 세트 롤백에도 실패했습니다: %s",
                    ", ".join(str(path) for path in rollback_errors),
                )
                raise BackupError(
                    "데이터베이스 복원과 원본 세트 롤백에 실패했습니다."
                ) from error
            raise
        _discard_preserved_database_set(preserved_paths.values())
        return BackupInspection(path=destination, schema_version=restored.schema_version)
    except BaseException:
        LOGGER.exception("데이터베이스 복원에 실패했습니다: %s", destination)
        raise
    finally:
        temporary_path.unlink(missing_ok=True)


class MigrationBackupHook:
    """Database migration 백업 훅 계약을 구현한다."""

    def __init__(
        self,
        backup_directory: Path | None = None,
        *,
        clock: Callable[[], datetime] | None = None,
    ) -> None:
        self._backup_directory = backup_directory
        self._clock = clock or (lambda: datetime.now(UTC))
        self.last_backup_path: Path | None = None

    def __call__(
        self,
        database_path: Path,
        old_version: int,
        new_version: int,
    ) -> None:
        """migration 직전 DB를 별도 백업 파일로 보존한다."""
        directory = self._backup_directory or database_path.parent / "backups"
        timestamp = self._clock().strftime("%Y%m%dT%H%M%S%fZ")
        destination = directory / (
            f"{database_path.stem}.pre-migration-v{old_version}-to-v{new_version}"
            f"-{timestamp}.sqlite3"
        )
        create_database_backup(database_path, destination)
        self.last_backup_path = destination


class AutomaticBackupManager:
    """설정된 시간 간격에 따라 자동 백업 실행 여부를 결정한다."""

    def __init__(
        self,
        database_path: Path,
        backup_directory: Path,
        *,
        interval_hours: float = 24,
        clock: Callable[[], datetime] | None = None,
    ) -> None:
        self._database_path = database_path
        self._backup_directory = backup_directory
        self._clock = clock or (lambda: datetime.now(UTC))
        self._last_backup_at: datetime | None = None
        self.set_interval_hours(interval_hours)

    def set_interval_hours(self, interval_hours: float) -> None:
        """QSettings의 백업 주기 값을 적용할 자리를 제공한다."""
        if interval_hours <= 0:
            raise ValueError("자동 백업 주기는 0시간보다 커야 합니다.")
        self._interval = timedelta(hours=interval_hours)

    def run_if_due(self, *, force: bool = False) -> Path | None:
        """백업 주기가 지났을 때만 검증된 자동 백업을 만든다."""
        now = self._clock()
        last_backup_at = self._last_backup_at or self._latest_backup_time()
        if not force and last_backup_at is not None and now - last_backup_at < self._interval:
            return None
        timestamp = now.strftime("%Y%m%dT%H%M%S%fZ")
        destination = self._backup_directory / (
            f"{self._database_path.stem}.auto-{timestamp}.sqlite3"
        )
        create_database_backup(self._database_path, destination)
        self._last_backup_at = now
        return destination

    def _latest_backup_time(self) -> datetime | None:
        pattern = f"{self._database_path.stem}.auto-*.sqlite3"
        candidates = tuple(self._backup_directory.glob(pattern))
        if not candidates:
            return None
        latest = max(path.stat().st_mtime for path in candidates)
        return datetime.fromtimestamp(latest, tz=UTC)


class PeriodicQuickCheck:
    """호출 시점에 주기가 도래했으면 DB quick_check를 수행한다."""

    def __init__(
        self,
        connection: sqlite3.Connection,
        *,
        interval_hours: float = 24,
        clock: Callable[[], float] | None = None,
    ) -> None:
        if interval_hours <= 0:
            raise ValueError("quick_check 주기는 0시간보다 커야 합니다.")
        self._connection = connection
        self._interval_seconds = interval_hours * 60 * 60
        self._clock = clock or time.monotonic
        self._last_check_at: float | None = None

    def run_if_due(self, *, force: bool = False) -> bool:
        """검사 실행 여부를 반환하고 무결성 실패는 즉시 전파한다."""
        now = self._clock()
        if (
            not force
            and self._last_check_at is not None
            and now - self._last_check_at < self._interval_seconds
        ):
            return False
        run_quick_check(self._connection)
        self._last_check_at = now
        return True


def _read_schema_version(connection: sqlite3.Connection) -> int:
    table = connection.execute(
        """
        SELECT 1
        FROM sqlite_master
        WHERE type = 'table' AND name = 'schema_version'
        """
    ).fetchone()
    if table is None:
        raise BackupIntegrityError("pyNote schema_version이 없는 백업입니다.")
    row = connection.execute(
        "SELECT version FROM schema_version WHERE id = 1"
    ).fetchone()
    if row is None:
        raise BackupIntegrityError("백업의 schema version 행이 없습니다.")
    value = row[0]
    if type(value) is not int:
        raise BackupIntegrityError("백업의 schema version이 정수가 아닙니다.")
    return value


def _validate_schema_tables(
    connection: sqlite3.Connection,
    schema_version: int,
) -> None:
    if schema_version == 0:
        return
    rows = connection.execute(
        """
        SELECT name
        FROM sqlite_master
        WHERE type = 'table'
        """
    ).fetchall()
    table_names = {str(row[0]) for row in rows}
    if schema_version >= 4:
        required = _V4_REQUIRED_TABLES
    elif schema_version >= 2:
        required = _V2_REQUIRED_TABLES
    else:
        required = _V1_REQUIRED_TABLES
    missing = sorted(required - table_names)
    if missing:
        message = ", ".join(missing)
        raise BackupIntegrityError(f"백업에 필수 테이블이 없습니다: {message}")


def _validate_foreign_keys(connection: sqlite3.Connection) -> None:
    rows = connection.execute("PRAGMA foreign_key_check").fetchall()
    if not rows:
        return
    sample = "; ".join(
        f"{row[0]} rowid={row[1]} -> {row[2]}" for row in rows[:5]
    )
    LOGGER.error("백업에서 FK 무결성 오류를 발견했습니다: %s", sample)
    raise BackupIntegrityError(f"백업의 FK 무결성 검사에 실패했습니다: {sample}")


def _validate_logical_integrity(connection: sqlite3.Connection) -> None:
    _validate_card_revision_integrity(connection)
    _validate_capture_counter(connection)
    _validate_capture_operations(connection)


def _validate_card_revision_integrity(connection: sqlite3.Connection) -> None:
    rows = connection.execute(
        """
        SELECT
            cards.id,
            cards.body,
            cards.body_hash,
            cards.current_revision_id,
            card_revisions.card_id,
            card_revisions.body,
            card_revisions.body_hash
        FROM cards
        LEFT JOIN card_revisions
          ON card_revisions.id = cards.current_revision_id
        """
    ).fetchall()
    for row in rows:
        card_id = str(row[0])
        if row[3] is None or row[4] != row[0]:
            _raise_integrity_error(
                f"카드 {card_id}의 현재 리비전 소유권이 올바르지 않습니다."
            )
        card_body = _validated_text(row[1], f"카드 {card_id} 본문")
        card_hash = _validated_text(row[2], f"카드 {card_id} 본문 해시")
        revision_body = _validated_text(row[5], f"카드 {card_id} 현재 리비전 본문")
        revision_hash = _validated_text(
            row[6],
            f"카드 {card_id} 현재 리비전 본문 해시",
        )
        if card_body != revision_body or card_hash != revision_hash:
            _raise_integrity_error(
                f"카드 {card_id}의 본문과 현재 리비전이 일치하지 않습니다."
            )
        _validate_text_hash(card_body, card_hash, f"카드 {card_id}")

    revision_rows = connection.execute(
        "SELECT id, body, body_hash FROM card_revisions"
    ).fetchall()
    for revision_id, body_value, hash_value in revision_rows:
        body = _validated_text(body_value, f"리비전 {revision_id} 본문")
        body_hash = _validated_text(hash_value, f"리비전 {revision_id} 본문 해시")
        _validate_text_hash(body, body_hash, f"리비전 {revision_id}")


def _validate_capture_counter(connection: sqlite3.Connection) -> None:
    row = connection.execute(
        "SELECT next_value FROM counters WHERE name = 'capture'"
    ).fetchone()
    if row is None or type(row[0]) is not int:
        _raise_integrity_error("capture 카운터가 없거나 정수가 아닙니다.")
    maximum_row = connection.execute(
        "SELECT COALESCE(MAX(capture_seq), 0) FROM cards"
    ).fetchone()
    maximum = 0 if maximum_row is None else maximum_row[0]
    if type(maximum) is not int or row[0] <= maximum:
        _raise_integrity_error(
            "capture 카운터가 이미 발급된 capture_seq보다 크지 않습니다."
        )


def _validate_capture_operations(connection: sqlite3.Connection) -> None:
    rows = connection.execute(
        """
        SELECT id, original_text, original_hash, original_redacted_at_us
        FROM capture_operations
        """
    ).fetchall()
    for operation_id, text_value, hash_value, redacted_at_us in rows:
        has_text = text_value is not None
        has_hash = hash_value is not None
        if has_text != has_hash:
            _raise_integrity_error(
                f"입력 작업 {operation_id}의 원문과 해시 쌍이 일치하지 않습니다."
            )
        if redacted_at_us is not None:
            if type(redacted_at_us) is not int or has_text:
                _raise_integrity_error(
                    f"입력 작업 {operation_id}의 redact 마커가 올바르지 않습니다."
                )
            continue
        if has_text:
            text = _validated_text(text_value, f"입력 작업 {operation_id} 원문")
            original_hash = _validated_text(
                hash_value,
                f"입력 작업 {operation_id} 원문 해시",
            )
            _validate_text_hash(text, original_hash, f"입력 작업 {operation_id}")


def _validated_text(value: object, label: str) -> str:
    if type(value) is not str:
        _raise_integrity_error(f"{label}가 문자열이 아닙니다.")
    return value


def _validate_text_hash(text: str, stored_hash: str, label: str) -> None:
    expected_hash = hashlib.sha256(text.encode("utf-8")).hexdigest()
    if stored_hash != expected_hash:
        _raise_integrity_error(f"{label}의 SHA-256 해시가 일치하지 않습니다.")


def _raise_integrity_error(message: str) -> Never:
    LOGGER.error("백업 논리 무결성 오류: %s", message)
    raise BackupIntegrityError(message)


def _database_file_set(destination: Path) -> tuple[Path, Path, Path]:
    return (
        destination,
        Path(f"{destination}-wal"),
        Path(f"{destination}-shm"),
    )


def _validate_restore_targets(paths: tuple[Path, ...]) -> None:
    for path in paths:
        if (path.exists() or path.is_symlink()) and (
            not path.is_file() or path.is_symlink()
        ):
            LOGGER.error("복원 대상 DB 세트 경로가 일반 파일이 아닙니다: %s", path)
            raise BackupError(f"복원 대상 DB 세트 경로가 올바르지 않습니다: {path}")


def _restore_preserved_database_set(
    *,
    destination: Path,
    temporary_path: Path,
    moved_paths: list[Path],
    preserved_paths: dict[Path, Path],
) -> tuple[Path, ...]:
    failed_paths: list[Path] = []
    if destination not in moved_paths and destination.exists():
        try:
            os.replace(destination, temporary_path)
        except OSError:
            LOGGER.exception("새 복원 DB를 롤백하지 못했습니다: %s", destination)
            failed_paths.append(destination)
    for path in reversed(moved_paths):
        try:
            os.replace(preserved_paths[path], path)
        except OSError:
            LOGGER.exception("보존한 원본 DB 파일을 복구하지 못했습니다: %s", path)
            failed_paths.append(path)
    return tuple(failed_paths)


def _discard_preserved_database_set(paths: Iterable[Path]) -> None:
    for path in paths:
        try:
            path.unlink(missing_ok=True)
        except OSError:
            LOGGER.exception("교체 완료 후 보존 DB 파일을 삭제하지 못했습니다: %s", path)


def _open_read_only(path: Path) -> sqlite3.Connection:
    return sqlite3.connect(f"{path.resolve().as_uri()}?mode=ro", uri=True)


def _temporary_database_path(destination: Path) -> Path:
    descriptor, name = tempfile.mkstemp(
        prefix=f".{destination.name}.",
        suffix=".tmp",
        dir=destination.parent,
    )
    os.close(descriptor)
    path = Path(name)
    path.unlink()
    return path
