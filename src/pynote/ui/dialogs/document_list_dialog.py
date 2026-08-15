from __future__ import annotations

from collections.abc import Callable

from PySide6.QtCore import Signal
from PySide6.QtWidgets import QDialog, QPushButton, QVBoxLayout, QWidget

from pynote.infrastructure.repositories import Repositories
from pynote.ui.panels.document_navigator import DocumentNavigator


class DocumentListDialog(QDialog):
    """문서 CRUD와 열기 요청을 제공하는 모달리스 대화상자다."""

    document_open_requested = Signal(str)
    document_created = Signal(str)
    open_in_new_window_requested = Signal(str)
    document_purge_requested = Signal(str)
    document_state_changed = Signal(str)

    def __init__(
        self,
        repositories: Repositories,
        *,
        time_format: str = "yyyy-MM-dd HH:mm",
        destructive_preflight: Callable[[str], bool] | None = None,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.setObjectName("documentListDialog")
        self.setWindowTitle("문서 목록")
        self.setModal(False)

        self.navigator = DocumentNavigator(
            repositories,
            time_format=time_format,
            destructive_preflight=destructive_preflight,
            parent=self,
        )
        self.open_in_new_window_button = QPushButton("새 창에서 열기", self)
        self.open_in_new_window_button.setObjectName("openInNewWindowButton")

        layout = QVBoxLayout(self)
        layout.addWidget(self.navigator, 1)
        layout.addWidget(self.open_in_new_window_button)

        self.navigator.document_open_requested.connect(
            self._request_document_open
        )
        self.navigator.document_created.connect(self._notify_document_created)
        self.navigator.document_purge_requested.connect(
            self.document_purge_requested
        )
        self.navigator.document_state_changed.connect(
            self.document_state_changed
        )
        self.open_in_new_window_button.clicked.connect(
            self._request_open_in_new_window
        )

    def refresh(self) -> None:
        """문서 목록을 저장소에서 다시 읽는다."""
        self.navigator.refresh()

    def apply_display_settings(self, time_format: str, timezone: str) -> None:
        """문서 메타데이터 표시 설정을 갱신한다."""
        self.navigator.apply_display_settings(time_format, timezone)

    def _request_document_open(self, document_id: str) -> None:
        # 닫기는 라우팅 결과를 아는 쪽이 한다 — 이탈 게이트가 교체를 거부하면
        # 목록이 그대로 남아야 한다.
        self.document_open_requested.emit(document_id)

    def _notify_document_created(self, document_id: str) -> None:
        self.document_created.emit(document_id)

    def _request_open_in_new_window(self) -> None:
        document_id = self.navigator.current_document_id()
        if document_id is None:
            return
        self.open_in_new_window_requested.emit(document_id)
