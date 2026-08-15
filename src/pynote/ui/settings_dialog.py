from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QSettings, Signal
from PySide6.QtGui import QFont
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QDoubleSpinBox,
    QFileDialog,
    QFontComboBox,
    QFormLayout,
    QHBoxLayout,
    QLineEdit,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from pynote.infrastructure.settings import DataPolicySettingsStore


class SettingsDialog(QDialog):
    """장치별 설정을 편집하고 즉시 QSettings에 반영한다."""

    settings_applied = Signal()

    def __init__(
        self,
        settings: QSettings,
        *,
        policy_store: DataPolicySettingsStore | None = None,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._settings = settings
        self._policy_store = policy_store
        self._split_policy: str | None = None
        self.setObjectName("settingsDialog")
        self.setWindowTitle("설정")
        self.setMinimumWidth(520)

        self.time_format = QComboBox(self)
        self.time_format.setEditable(True)
        self.time_format.addItems(
            ("yyyy-MM-dd HH:mm", "yyyy-MM-dd HH:mm:ss", "yyyy년 M월 d일 AP h:mm")
        )
        self.timezone = QComboBox(self)
        self.timezone.setEditable(True)
        self.timezone.addItems(("system", "UTC", "Asia/Seoul"))

        self.font_family = QFontComboBox(self)
        self.font_size = QSpinBox(self)
        self.font_size.setRange(8, 72)
        self.line_spacing = QDoubleSpinBox(self)
        self.line_spacing.setRange(0.8, 3.0)
        self.line_spacing.setSingleStep(0.1)

        self.draft_idle_seconds = QDoubleSpinBox(self)
        self.draft_idle_seconds.setRange(0.1, 60.0)
        self.draft_idle_seconds.setSingleStep(0.5)
        self.preview_lines = QSpinBox(self)
        self.preview_lines.setRange(1, 100)
        self.multi_selection = QCheckBox("여러 카드를 함께 선택", self)
        self.multi_selection.setObjectName("cardMultiSelectionCheck")
        self.multi_selection.setToolTip(
            "끄면 카드 목록에서 한 번에 한 장만 선택합니다."
        )

        self.backup_location = QLineEdit(self)
        backup_browse = QPushButton("찾아보기…", self)
        backup_browse.clicked.connect(self._choose_backup_location)
        backup_row = QWidget(self)
        backup_layout = QHBoxLayout(backup_row)
        backup_layout.setContentsMargins(0, 0, 0, 0)
        backup_layout.addWidget(self.backup_location, 1)
        backup_layout.addWidget(backup_browse)
        self.backup_interval = QDoubleSpinBox(self)
        self.backup_interval.setRange(0.25, 24 * 365)
        self.backup_interval.setSuffix(" 시간")
        self.trash_retention_days = QSpinBox(self)
        self.trash_retention_days.setRange(0, 3650)
        self.trash_retention_days.setSuffix(" 일")

        form = QFormLayout()
        form.addRow("시간 표시 형식", self.time_format)
        form.addRow("시간대", self.timezone)
        form.addRow("글꼴", self.font_family)
        form.addRow("글자 크기", self.font_size)
        form.addRow("줄 간격", self.line_spacing)
        form.addRow("초안 보호 유휴 시간", self.draft_idle_seconds)
        form.addRow("카드 미리보기 줄 수", self.preview_lines)
        form.addRow("카드 다중 선택", self.multi_selection)
        form.addRow("백업 위치", backup_row)
        form.addRow("백업 주기", self.backup_interval)
        form.addRow("휴지통 보존 기간", self.trash_retention_days)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Save
            | QDialogButtonBox.StandardButton.Cancel,
            self,
        )
        buttons.button(QDialogButtonBox.StandardButton.Save).setText("저장")
        buttons.accepted.connect(self.apply_settings)
        buttons.rejected.connect(self.reject)

        layout = QVBoxLayout(self)
        layout.addLayout(form)
        layout.addWidget(buttons)
        self._load()

    def apply_settings(self) -> None:
        """설정값을 저장하고 적용 신호를 보낸다."""
        self._settings.setValue("display/time_format", self.time_format.currentText())
        self._settings.setValue("display/timezone", self.timezone.currentText())
        self._settings.setValue(
            "editor/font_family",
            self.font_family.currentFont().family(),
        )
        self._settings.setValue("editor/font_size", self.font_size.value())
        self._settings.setValue("editor/line_spacing", self.line_spacing.value())
        self._settings.setValue("backup/location", self.backup_location.text().strip())
        self._settings.setValue(
            "cards/multi_selection_enabled",
            self.multi_selection.isChecked(),
        )
        if self._policy_store is None:
            self._settings.setValue(
                "draft/idle_seconds",
                self.draft_idle_seconds.value(),
            )
            self._settings.setValue("cards/preview_lines", self.preview_lines.value())
            self._settings.setValue(
                "backup/interval_hours",
                self.backup_interval.value(),
            )
            self._settings.setValue(
                "trash/retention_days",
                self.trash_retention_days.value(),
            )
        else:
            split_policy = self._split_policy
            if split_policy is None:
                raise RuntimeError("현재 문단 분리 정책을 불러오지 못했습니다.")
            self._policy_store.save(
                draft_idle_ms=round(self.draft_idle_seconds.value() * 1_000),
                split_policy=split_policy,
                preview_lines=self.preview_lines.value(),
                backup_interval_hours=self.backup_interval.value(),
                trash_retention_days=self.trash_retention_days.value(),
            )
        self._settings.sync()
        if self._settings.status() is not QSettings.Status.NoError:
            raise RuntimeError("설정을 저장하지 못했습니다.")
        self.settings_applied.emit()
        self.accept()

    def selected_font(self) -> QFont:
        """현재 설정 화면의 글꼴과 크기를 QFont로 반환한다."""
        font = self.font_family.currentFont()
        font.setPointSize(self.font_size.value())
        return font

    def _load(self) -> None:
        self.time_format.setCurrentText(
            str(self._settings.value("display/time_format", "yyyy-MM-dd HH:mm"))
        )
        self.timezone.setCurrentText(
            str(self._settings.value("display/timezone", "system"))
        )
        family = str(self._settings.value("editor/font_family", ""))
        if family:
            self.font_family.setCurrentFont(QFont(family))
        self.font_size.setValue(
            int(str(self._settings.value("editor/font_size", 11)))
        )
        self.line_spacing.setValue(
            float(str(self._settings.value("editor/line_spacing", 1.0)))
        )
        self.multi_selection.setChecked(
            bool(
                self._settings.value(
                    "cards/multi_selection_enabled",
                    False,
                    type=bool,
                )
            )
        )
        policy = None if self._policy_store is None else self._policy_store.load()
        if policy is not None:
            self._split_policy = policy.split_policy
        self.draft_idle_seconds.setValue(
            float(str(self._settings.value("draft/idle_seconds", 2)))
            if policy is None
            else policy.draft_idle_ms / 1_000
        )
        self.preview_lines.setValue(
            int(str(self._settings.value("cards/preview_lines", 6)))
            if policy is None
            else policy.preview_lines
        )
        self.backup_location.setText(
            str(self._settings.value("backup/location", ""))
        )
        self.backup_interval.setValue(
            float(str(self._settings.value("backup/interval_hours", 24)))
            if policy is None
            else policy.backup_interval_hours
        )
        self.trash_retention_days.setValue(
            int(str(self._settings.value("trash/retention_days", 30)))
            if policy is None
            else policy.trash_retention_days
        )
    def _choose_backup_location(self) -> None:
        start = self.backup_location.text() or str(Path.home())
        path = QFileDialog.getExistingDirectory(self, "백업 위치", start)
        if path:
            self.backup_location.setText(path)
