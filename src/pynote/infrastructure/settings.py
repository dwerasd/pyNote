from __future__ import annotations

import logging
import time
from dataclasses import dataclass, field

from PySide6.QtCore import QSettings

from pynote.infrastructure.database import Database

LOGGER = logging.getLogger(__name__)
LEGACY_WINDOW_GEOMETRY_KEY = "window/geometry"
IMMEDIATE_PASTE_CAPTURE_KEY = "composer/immediate_paste_capture"


def window_geometry_key(window_id: str) -> str:
    """창 ID에 대응하는 장치별 geometry 키를 반환한다."""
    if not window_id:
        raise ValueError("window_id는 비어 있을 수 없습니다.")
    return f"windows/{window_id}/geometry"


def migrate_legacy_window_geometry(settings: QSettings, window_id: str) -> None:
    """단일 창 시절의 geometry를 v4 첫 창 키로 한 번 이관한다."""
    target_key = window_geometry_key(window_id)
    if settings.contains(LEGACY_WINDOW_GEOMETRY_KEY):
        if not settings.contains(target_key):
            settings.setValue(
                target_key,
                settings.value(LEGACY_WINDOW_GEOMETRY_KEY),
            )
        settings.remove(LEGACY_WINDOW_GEOMETRY_KEY)
        settings.sync()
        if settings.status() is not QSettings.Status.NoError:
            raise RuntimeError("기존 창 위치와 크기를 이관하지 못했습니다.")


@dataclass(frozen=True, slots=True)
class DataPolicySettings:
    """DB와 함께 이동해야 하는 데이터 운용 정책이다."""

    draft_idle_ms: int
    split_policy: str
    preview_lines: int = field(default=3, kw_only=True)
    backup_interval_hours: float
    trash_retention_days: int
    updated_at_us: int


class DataPolicySettingsStore:
    """data_policy_settings 단일 행을 원자적으로 읽고 저장한다."""

    def __init__(self, database: Database) -> None:
        self._database = database
        self._connection = database.connection

    def load(self) -> DataPolicySettings:
        """현재 데이터 정책 단일 행을 반환한다."""
        row = self._connection.execute(
            "SELECT * FROM data_policy_settings WHERE id = 1"
        ).fetchone()
        if row is None:
            raise RuntimeError("data_policy_settings 단일 행이 없습니다.")
        return DataPolicySettings(
            draft_idle_ms=int(row["draft_idle_ms"]),
            split_policy=str(row["split_policy"]),
            preview_lines=int(row["preview_lines"]),
            backup_interval_hours=float(row["backup_interval_hours"]),
            trash_retention_days=int(row["trash_retention_days"]),
            updated_at_us=int(row["updated_at_us"]),
        )

    def save(
        self,
        *,
        draft_idle_ms: int,
        split_policy: str,
        preview_lines: int,
        backup_interval_hours: float,
        trash_retention_days: int,
    ) -> DataPolicySettings:
        """검증된 데이터 정책 전체를 한 트랜잭션으로 갱신한다."""
        if draft_idle_ms < 0:
            raise ValueError("draft 유휴 시간은 0 이상이어야 합니다.")
        if split_policy not in {"keep", "split_by_blank_line"}:
            raise ValueError(f"지원하지 않는 문단 분리 정책입니다: {split_policy}")
        if preview_lines < 1:
            raise ValueError("미리보기 줄 수는 1 이상이어야 합니다.")
        if backup_interval_hours <= 0:
            raise ValueError("백업 주기는 0시간보다 커야 합니다.")
        if trash_retention_days < 0:
            raise ValueError("휴지통 보존 기간은 0일 이상이어야 합니다.")
        updated_at_us = time.time_ns() // 1_000
        try:
            with self._database.transaction():
                self._connection.execute(
                    """
                    UPDATE data_policy_settings
                    SET draft_idle_ms = ?,
                        split_policy = ?,
                        preview_lines = ?,
                        backup_interval_hours = ?,
                        trash_retention_days = ?,
                        updated_at_us = ?
                    WHERE id = 1
                    """,
                    (
                        draft_idle_ms,
                        split_policy,
                        preview_lines,
                        backup_interval_hours,
                        trash_retention_days,
                        updated_at_us,
                    ),
                )
        except BaseException:
            LOGGER.exception("데이터 정책 설정 저장에 실패했습니다.")
            raise
        return self.load()
