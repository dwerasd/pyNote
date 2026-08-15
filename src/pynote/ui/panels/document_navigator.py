from __future__ import annotations

import logging
import time
from collections.abc import Callable
from dataclasses import dataclass, replace
from enum import StrEnum

from PySide6.QtCore import QDateTime, Qt, QTimeZone, Signal
from PySide6.QtWidgets import (
    QComboBox,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMessageBox,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from pynote.application import document_service
from pynote.domain.models import Document
from pynote.infrastructure.repositories import Repositories

LOGGER = logging.getLogger(__name__)


class DocumentView(StrEnum):
    """문서 탐색기에 표시할 문서 상태다."""

    ACTIVE = "active"
    ARCHIVED = "archived"
    TRASHED = "trashed"


@dataclass(frozen=True, slots=True)
class DocumentSummary:
    """문서 목록 한 행에 필요한 문서 및 카드 집계다."""

    document: Document
    card_count: int
    character_count: int


class DocumentNavigator(QWidget):
    """문서 CRUD와 상태별 목록을 제공하는 왼쪽 탐색기다."""

    document_open_requested = Signal(str)
    document_created = Signal(str)
    document_state_changed = Signal(str)
    documents_changed = Signal()
    document_purge_requested = Signal(str)

    def __init__(
        self,
        repositories: Repositories,
        *,
        clock_us: Callable[[], int] | None = None,
        time_format: str = "yyyy-MM-dd HH:mm",
        destructive_preflight: Callable[[str], bool] | None = None,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._repositories = repositories
        self._clock_us = clock_us or (lambda: time.time_ns() // 1_000)
        self._time_format = time_format
        self._timezone = "system"
        self._summaries: dict[str, DocumentSummary] = {}
        self._destructive_preflight = destructive_preflight

        self.setObjectName("documentNavigator")
        self.setMinimumWidth(260)

        title = QLabel("문서", self)
        title.setObjectName("documentNavigatorTitle")

        self.search_edit = QLineEdit(self)
        self.search_edit.setObjectName("documentSearchEdit")
        self.search_edit.setPlaceholderText("문서 제목 검색")
        self.search_edit.setClearButtonEnabled(True)
        self.search_edit.textChanged.connect(self.refresh)

        self.view_combo = QComboBox(self)
        self.view_combo.setObjectName("documentViewCombo")
        self.view_combo.addItem("최근 문서", DocumentView.ACTIVE.value)
        self.view_combo.addItem("보관함", DocumentView.ARCHIVED.value)
        self.view_combo.addItem("휴지통", DocumentView.TRASHED.value)
        self.view_combo.currentIndexChanged.connect(self.refresh)

        filter_layout = QHBoxLayout()
        filter_layout.addWidget(self.search_edit, 1)
        filter_layout.addWidget(self.view_combo)

        self.document_list = QListWidget(self)
        self.document_list.setObjectName("documentList")
        self.document_list.setAlternatingRowColors(True)
        self.document_list.itemActivated.connect(self._request_item_open)
        self.document_list.itemDoubleClicked.connect(self._request_item_open)

        self.new_button = QPushButton("새 문서", self)
        self.new_button.setObjectName("newDocumentButton")
        self.new_button.clicked.connect(self._create_from_dialog)

        self.rename_button = QPushButton("이름 변경", self)
        self.rename_button.setObjectName("renameDocumentButton")
        self.rename_button.clicked.connect(self._rename_from_dialog)

        self.archive_button = QPushButton("보관", self)
        self.archive_button.setObjectName("archiveDocumentButton")
        self.archive_button.clicked.connect(self._archive_selected)

        self.trash_button = QPushButton("휴지통", self)
        self.trash_button.setObjectName("trashDocumentButton")
        self.trash_button.clicked.connect(self._trash_selected)

        self.restore_button = QPushButton("복구", self)
        self.restore_button.setObjectName("restoreDocumentButton")
        self.restore_button.clicked.connect(self._restore_selected)
        self.purge_button = QPushButton("완전 삭제…", self)
        self.purge_button.setObjectName("purgeDocumentButton")
        self.purge_button.clicked.connect(self._request_purge_selected)

        command_layout = QHBoxLayout()
        command_layout.addWidget(self.new_button)
        command_layout.addWidget(self.rename_button)

        state_layout = QHBoxLayout()
        state_layout.addWidget(self.archive_button)
        state_layout.addWidget(self.trash_button)
        state_layout.addWidget(self.restore_button)
        state_layout.addWidget(self.purge_button)

        layout = QVBoxLayout(self)
        layout.addWidget(title)
        layout.addLayout(filter_layout)
        layout.addWidget(self.document_list, 1)
        layout.addLayout(command_layout)
        layout.addLayout(state_layout)

        self.view_combo.currentIndexChanged.connect(self._update_command_visibility)
        self.refresh()
        self._update_command_visibility()

    @property
    def current_view(self) -> DocumentView:
        """현재 선택된 문서 상태 보기를 반환한다."""
        value = self.view_combo.currentData()
        return DocumentView(str(value))

    def current_document_id(self) -> str | None:
        """현재 선택 문서 ID를 반환한다."""
        item = self.document_list.currentItem()
        if item is None:
            return None
        value = item.data(Qt.ItemDataRole.UserRole)
        return None if value is None else str(value)

    def visible_document_ids(self) -> tuple[str, ...]:
        """현재 목록의 문서 ID를 화면 순서대로 반환한다."""
        return tuple(
            str(self.document_list.item(index).data(Qt.ItemDataRole.UserRole))
            for index in range(self.document_list.count())
        )

    def summary(self, document_id: str) -> DocumentSummary | None:
        """마지막으로 읽은 문서 요약을 반환한다."""
        return self._summaries.get(document_id)

    def set_view(self, view: DocumentView) -> None:
        """문서 상태 보기를 전환한다."""
        index = self.view_combo.findData(view.value)
        if index >= 0:
            self.view_combo.setCurrentIndex(index)

    def apply_display_settings(self, time_format: str, timezone: str) -> None:
        """문서 메타데이터의 시간 형식과 표시 시간대를 즉시 갱신한다."""
        self._time_format = time_format
        self._timezone = timezone
        self.refresh()

    def refresh(self, *_arguments: object) -> None:
        """저장소에서 문서와 카드 집계를 다시 읽어 목록을 갱신한다."""
        selected_id = self.current_document_id()
        query = self.search_edit.text().strip().casefold()
        documents = self._repositories.list_documents()
        summaries = [self._build_summary(document) for document in documents]
        summaries = [
            summary
            for summary in summaries
            if self._matches_view(summary.document)
            and (not query or query in summary.document.title.casefold())
        ]
        summaries.sort(
            key=lambda summary: (
                -summary.document.updated_at_us,
                -summary.document.created_at_us,
                summary.document.id,
            )
        )
        self._summaries = {summary.document.id: summary for summary in summaries}

        self.document_list.clear()
        selected_item: QListWidgetItem | None = None
        for summary in summaries:
            item = QListWidgetItem(self._summary_text(summary))
            item.setData(Qt.ItemDataRole.UserRole, summary.document.id)
            item.setToolTip(summary.document.title)
            self.document_list.addItem(item)
            if summary.document.id == selected_id:
                selected_item = item
        if selected_item is not None:
            self.document_list.setCurrentItem(selected_item)

    def create_document(self, title: str | None = None) -> Document:
        """새 문서를 저장하고 생성 완료를 알린다."""
        document = document_service.create_document(
            self._repositories,
            title,
            clock_us=self._clock_us,
        )
        self.set_view(DocumentView.ACTIVE)
        self.refresh()
        self._select_document(document.id)
        self.documents_changed.emit()
        self.document_state_changed.emit(document.id)
        self.document_created.emit(document.id)
        return document

    def rename_document(self, document_id: str, title: str) -> Document:
        """문서 제목을 변경한다."""
        normalized_title = title.strip()
        if not normalized_title:
            raise ValueError("문서 제목은 비어 있을 수 없습니다.")
        document = self._require_document(document_id)
        updated = replace(
            document,
            title=normalized_title,
            updated_at_us=self._clock_us(),
        )
        self._store_document(updated, "문서 이름 변경")
        return updated

    def archive_document(self, document_id: str) -> Document:
        """문서를 보관함으로 이동한다."""
        self._require_destructive_preflight(document_id)
        document = self._require_document(document_id)
        now_us = self._clock_us()
        updated = replace(document, archived_at_us=now_us, updated_at_us=now_us)
        self._store_document(updated, "문서 보관")
        return updated

    def trash_document(self, document_id: str) -> Document:
        """문서를 휴지통으로 이동한다."""
        self._require_destructive_preflight(document_id)
        document = self._require_document(document_id)
        now_us = self._clock_us()
        updated = replace(document, trashed_at_us=now_us, updated_at_us=now_us)
        self._store_document(updated, "문서 휴지통 이동")
        return updated

    def restore_document(self, document_id: str) -> Document:
        """휴지통 문서를 이전 보관 상태로 복구한다."""
        document = self._require_document(document_id)
        updated = replace(
            document,
            trashed_at_us=None,
            updated_at_us=self._clock_us(),
        )
        self._store_document(updated, "문서 복구")
        return updated

    def unarchive_document(self, document_id: str) -> Document:
        """보관 문서를 최근 문서 목록으로 되돌린다."""
        document = self._require_document(document_id)
        updated = replace(
            document,
            archived_at_us=None,
            updated_at_us=self._clock_us(),
        )
        self._store_document(updated, "문서 보관 해제")
        return updated

    def _build_summary(self, document: Document) -> DocumentSummary:
        cards = [
            card
            for card in self._repositories.list_cards(document.id)
            if card.deleted_at_us is None
        ]
        return DocumentSummary(
            document=document,
            card_count=len(cards),
            character_count=sum(len(card.body) for card in cards),
        )

    def _matches_view(self, document: Document) -> bool:
        if self.current_view is DocumentView.TRASHED:
            return document.trashed_at_us is not None
        if document.trashed_at_us is not None:
            return False
        if self.current_view is DocumentView.ARCHIVED:
            return document.archived_at_us is not None
        return document.archived_at_us is None

    def _summary_text(self, summary: DocumentSummary) -> str:
        document = summary.document
        created = self._format_time(document.created_at_us)
        updated = self._format_time(document.updated_at_us)
        return (
            f"{document.title}\n"
            f"생성 {created}\n"
            f"수정 {updated}\n"
            f"{summary.card_count}개 카드 · {summary.character_count}자"
        )

    def _format_time(self, timestamp_us: int) -> str:
        date_time = QDateTime.fromMSecsSinceEpoch(
            timestamp_us // 1_000,
            QTimeZone.utc(),
        )
        if self._timezone == "system":
            displayed = date_time.toLocalTime()
        elif self._timezone == "UTC":
            displayed = date_time.toTimeZone(QTimeZone.utc())
        else:
            zone = QTimeZone(self._timezone.encode("utf-8"))
            displayed = date_time.toTimeZone(zone) if zone.isValid() else date_time.toLocalTime()
        return displayed.toString(self._time_format)

    def _require_document(self, document_id: str) -> Document:
        document = self._repositories.get_document(document_id)
        if document is None:
            raise KeyError(f"존재하지 않는 문서입니다: {document_id}")
        return document

    def _store_document(self, document: Document, action: str) -> None:
        try:
            self._repositories.update_document(document)
        except BaseException:
            LOGGER.exception("%s 저장에 실패했습니다.", action)
            raise
        self.refresh()
        self._select_document(document.id)
        self.documents_changed.emit()
        self.document_state_changed.emit(document.id)

    def _require_destructive_preflight(self, document_id: str) -> None:
        if (
            self._destructive_preflight is not None
            and not self._destructive_preflight(document_id)
        ):
            raise RuntimeError("편집을 계속하도록 선택해 문서 작업을 취소했습니다.")

    def _select_document(self, document_id: str) -> None:
        for index in range(self.document_list.count()):
            item = self.document_list.item(index)
            if str(item.data(Qt.ItemDataRole.UserRole)) == document_id:
                self.document_list.setCurrentItem(item)
                return

    def _request_item_open(self, item: QListWidgetItem) -> None:
        document_id = item.data(Qt.ItemDataRole.UserRole)
        if document_id is not None:
            self.document_open_requested.emit(str(document_id))

    def _create_from_dialog(self) -> None:
        title, accepted = QInputDialog.getText(self, "새 문서", "문서 제목")
        if accepted:
            self._show_write_error(lambda: self.create_document(title))

    def _rename_from_dialog(self) -> None:
        document_id = self.current_document_id()
        if document_id is None:
            return
        document = self._require_document(document_id)
        title, accepted = QInputDialog.getText(
            self,
            "문서 이름 변경",
            "문서 제목",
            text=document.title,
        )
        if accepted:
            self._show_write_error(lambda: self.rename_document(document_id, title))

    def _archive_selected(self) -> None:
        document_id = self.current_document_id()
        if document_id is not None:
            self._show_write_error(lambda: self.archive_document(document_id))

    def _trash_selected(self) -> None:
        document_id = self.current_document_id()
        if document_id is not None:
            self._show_write_error(lambda: self.trash_document(document_id))

    def _restore_selected(self) -> None:
        document_id = self.current_document_id()
        if document_id is not None:
            if self.current_view is DocumentView.ARCHIVED:
                self._show_write_error(lambda: self.unarchive_document(document_id))
            else:
                self._show_write_error(lambda: self.restore_document(document_id))

    def _request_purge_selected(self) -> None:
        document_id = self.current_document_id()
        if document_id is not None and self.current_view is DocumentView.TRASHED:
            self.document_purge_requested.emit(document_id)

    def _show_write_error(self, command: Callable[[], object]) -> None:
        try:
            command()
        except (KeyError, ValueError) as error:
            QMessageBox.warning(self, "문서 작업 실패", str(error))
        except BaseException as error:
            QMessageBox.critical(self, "문서 저장 실패", str(error))

    def _update_command_visibility(self, *_arguments: object) -> None:
        is_active = self.current_view is DocumentView.ACTIVE
        is_trashed = self.current_view is DocumentView.TRASHED
        self.restore_button.setVisible(not is_active)
        self.restore_button.setText("복구" if is_trashed else "보관 해제")
        self.rename_button.setVisible(not is_trashed)
        self.archive_button.setVisible(is_active)
        self.trash_button.setVisible(not is_trashed)
        self.purge_button.setVisible(is_trashed)
