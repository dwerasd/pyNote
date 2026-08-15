from __future__ import annotations

import logging
import time
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Protocol, cast

from PySide6.QtCore import QByteArray, QEvent, QSettings, QSize, Qt, Signal
from PySide6.QtGui import QAction, QCloseEvent, QGuiApplication, QKeySequence
from PySide6.QtWidgets import (
    QDialog,
    QFileDialog,
    QInputDialog,
    QLabel,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from pynote.application import document_service
from pynote.application.draft_coordinator import (
    DraftCoordinator,
    DraftDisposition,
    RecoveryCandidate,
    build_recovery_plans,
)
from pynote.application.purge_service import PurgeService
from pynote.domain.models import Card
from pynote.infrastructure.backup import (
    create_database_backup,
    inspect_backup,
    restore_database,
)
from pynote.infrastructure.export import NewlineFormat, export_document
from pynote.infrastructure.repositories import Repositories
from pynote.infrastructure.settings import (
    DataPolicySettingsStore,
    window_geometry_key,
)
from pynote.ui.dialogs.document_list_dialog import DocumentListDialog
from pynote.ui.document_page import DocumentPage
from pynote.ui.editor.card_editor import EditorStatus
from pynote.ui.import_dialog import ImportController, ImportDialog
from pynote.ui.search_dialog import SearchDialog
from pynote.ui.settings_dialog import SettingsDialog

LOGGER = logging.getLogger(__name__)
# 좌측 카드 목록과 우측 입력·편집 슬롯의 최소폭을 기본 창에서 함께 확보한다.
DEFAULT_WINDOW_SIZE = QSize(960, 640)
RecoveryChoiceProvider = Callable[[RecoveryCandidate], DraftDisposition]
DocumentOpenRouter = Callable[["MainWindow", str], bool]
SearchResultRouter = Callable[["MainWindow", str, str | None], bool]
DocumentPreflight = Callable[[str], bool]
DocumentChangePublisher = Callable[[str], None]
WindowCallback = Callable[["MainWindow"], None]
OpenInNewWindowCallback = Callable[["MainWindow", str], bool]


@dataclass(frozen=True, slots=True)
class WorkspaceState:
    """창에 저장된 문서 ID와 활성 문서를 나타낸다."""

    open_document_ids: tuple[str, ...]
    active_document_id: str | None
    updated_at_us: int


@dataclass(frozen=True, slots=True)
class DocumentUiState:
    """카드 UI가 연결될 때 사용할 문서별 복원 상태다."""

    document_id: str
    selected_card_id: str | None
    list_scroll_position: int
    sort_mode: str
    editor_card_id: str | None
    editor_base_revision_id: str | None
    editor_cursor_qchar: int | None
    # editor_split_left/right 는 각각 좌측 목록과 우측 슬롯의 폭이다.
    editor_split_sizes: tuple[int, int] | None
    updated_at_us: int


@dataclass(frozen=True, slots=True)
class ImportRevealPlan:
    """버스 발행 뒤 요청 창에서 실행할 생성 카드 공개 계획이다."""

    page: DocumentPage
    document_id: str
    card_id: str
    previous_view: tuple[str | None, int]


class WorkspaceStateStore(Protocol):
    """메인 창이 사용하는 DB 작업 상태 저장 계약이다."""

    def load_workspace(self) -> WorkspaceState:
        """마지막 workspace 상태를 읽는다."""
        ...

    def save_workspace(
        self,
        open_document_ids: tuple[str, ...],
        active_document_id: str | None,
    ) -> WorkspaceState:
        """현재 workspace 상태를 저장한다."""
        ...

    def load_document_ui_state(self, document_id: str) -> DocumentUiState | None:
        """문서별 UI 상태를 읽는다."""
        ...

    def save_document_ui_state(self, state: DocumentUiState) -> None:
        """문서별 UI 상태를 저장한다."""
        ...


class MainWindow(QMainWindow):
    """문서 하나의 카드 작업 페이지를 제공한다."""

    workspace_save_failed = Signal(str)
    workspace_changed = Signal()

    def __init__(
        self,
        repositories: Repositories,
        state_store: WorkspaceStateStore,
        *,
        time_format: str = "yyyy-MM-dd HH:mm",
        settings: QSettings | None = None,
        recovery_choice_provider: RecoveryChoiceProvider | None = None,
        window_id: str = "legacy-window",
        document_open_router: DocumentOpenRouter | None = None,
        search_result_router: SearchResultRouter | None = None,
        destructive_preflight: DocumentPreflight | None = None,
        document_change_publisher: DocumentChangePublisher | None = None,
        import_controller: ImportController | None = None,
        new_window_callback: Callable[[], object] | None = None,
        open_in_new_window_callback: OpenInNewWindowCallback | None = None,
        window_close_callback: WindowCallback | None = None,
        window_refill_callback: WindowCallback | None = None,
        startup_recovery_dispositions: dict[str, DraftDisposition] | None = None,
        startup_suppressed_card_ids: set[str] | None = None,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._repositories = repositories
        self._state_store = state_store
        self._restoring_workspace = False
        self._document_ui_states: dict[str, DocumentUiState] = {}
        self._settings = settings
        self._time_format = time_format
        self.window_id = window_id
        self._document_open_router = document_open_router
        self._search_result_router = search_result_router
        self._destructive_preflight = destructive_preflight
        self._document_change_publisher = document_change_publisher
        self.import_controller = import_controller
        self._new_window_callback = new_window_callback
        self._open_in_new_window_callback = open_in_new_window_callback
        self._window_close_callback = window_close_callback
        self._window_refill_callback = window_refill_callback
        self._recovery_choice_provider = recovery_choice_provider
        self._startup_recovery_dispositions = dict(
            startup_recovery_dispositions or {}
        )
        # 처분 맵은 창별 1회성이고 억제 집합은 실행 수명 전역 상태라 공유한다.
        self._startup_suppressed_card_ids = (
            startup_suppressed_card_ids
            if startup_suppressed_card_ids is not None
            else set()
        )
        self._policy_store = DataPolicySettingsStore(repositories.database)
        self._focus_mode = False
        self._publishing_page_content_change = False
        self._document_list_dialog: DocumentListDialog | None = None
        self._automatic_backup_manager: object | None = None
        self._application_owners: tuple[object, ...] = ()

        self.setObjectName("mainWindow")
        self.setWindowTitle("pyNote")
        self.resize(DEFAULT_WINDOW_SIZE)

        root = QWidget(self)
        root_layout = QVBoxLayout(root)
        root_layout.setContentsMargins(0, 0, 0, 0)

        self.first_run_banner = QLabel(root)
        self.first_run_banner.setObjectName("firstRunNotice")
        self.first_run_banner.setWordWrap(True)
        self.first_run_banner.setStyleSheet(
            "padding: 8px; background: palette(alternate-base);"
        )
        self.first_run_banner.hide()
        root_layout.addWidget(self.first_run_banner)

        self._page: DocumentPage | None = None
        self._page_host = QWidget(root)
        self._page_host.setObjectName("documentPageHost")
        self._page_host_layout = QVBoxLayout(self._page_host)
        self._page_host_layout.setContentsMargins(0, 0, 0, 0)
        root_layout.addWidget(self._page_host, 1)
        self.setCentralWidget(root)

        self._create_actions()
        self._create_menus()
        self._restore_window_geometry()
        self.statusBar().showMessage("문서를 선택하거나 새 문서를 만드세요.")
        recovered_candidates = (
            self._prompt_recovery_drafts()
            if recovery_choice_provider is not None
            else ()
        )
        self.restore_workspace()
        self._resume_recovery_draft(recovered_candidates)
        self._show_first_run_notice()

    @property
    def open_document_ids(self) -> tuple[str, ...]:
        """현재 창에 열린 문서 ID를 반환한다."""
        return () if self._page is None else (self._page.document_id,)

    @property
    def active_document_id(self) -> str | None:
        """현재 활성 문서 ID를 반환한다."""
        return None if self._page is None else self._page.document_id

    def document_ui_state(self, document_id: str) -> DocumentUiState | None:
        """시작 시 읽었거나 현재 저장한 문서별 UI 상태를 반환한다."""
        return self._document_ui_states.get(document_id)

    def restore_workspace(self) -> None:
        """시작 시 1회 DB 상태를 복원하며 live page 교체는 지원하지 않는다.

        최신 draft 보호가 선행되지 않으므로 실행 중 다시 호출하는 경로가 아니다.
        """
        workspace = self._state_store.load_workspace()
        self._restoring_workspace = True
        try:
            document_id = (
                workspace.active_document_id
                if workspace.active_document_id in workspace.open_document_ids
                else next(iter(workspace.open_document_ids), None)
            )
            page = (
                None
                if document_id is None
                else self._create_page(document_id)
            )
            if page is None:
                document_id = None
            elif document_id is not None:
                state = self._state_store.load_document_ui_state(document_id)
                if state is not None:
                    self._document_ui_states[document_id] = state
                    self._apply_ui_state_to_page(page, state)
            previous = self._install_page(page)
            if previous is not None:
                previous.deleteLater()
        finally:
            self._restoring_workspace = False

        retained_ids = () if document_id is None else (document_id,)
        if (
            retained_ids != workspace.open_document_ids
            or document_id != workspace.active_document_id
        ):
            self.save_workspace()
        self._update_status()
        self.focus_active_editor()

    def save_workspace(self) -> WorkspaceState:
        """현재 문서와 활성 문서를 DB에 저장한다."""
        return self._state_store.save_workspace(
            self.open_document_ids,
            self.active_document_id,
        )

    def persist_window_state(self) -> None:
        """앱 종료 전 이 창의 작업 상태와 geometry를 보존한다."""
        self.save_workspace()
        self._save_window_geometry()

    def protect_open_pages(self) -> bool:
        """앱 종료 요청 전에 이 창의 모든 최신 초안을 즉시 보호한다."""
        results = tuple(page.protect_now() for page in self._document_pages())
        if all(results):
            return True
        QMessageBox.critical(
            self,
            "초안 보호 실패",
            "종료 전 recovery draft를 보호하지 못했습니다. "
            "편집 내용을 확인한 뒤 다시 시도하세요.",
        )
        return False

    def protect_open_pages_quietly(self) -> None:
        """창 전환을 막지 않고 모든 최신 초안의 보호를 시도한다."""
        for page in self._document_pages():
            try:
                protected = page.protect_now()
            except BaseException:
                LOGGER.exception(
                    "창 전환 전 recovery draft 보호 중 예외가 발생했습니다: "
                    "document=%s",
                    page.document_id,
                )
                continue
            if not protected:
                LOGGER.warning(
                    "창 전환 전 recovery draft를 보호하지 못했습니다: document=%s",
                    page.document_id,
                )

    def can_leave_open_pages(self) -> bool:
        """이 창의 모든 편집기가 앱 종료 이탈을 승인하는지 확인한다."""
        return all(page.can_leave_editor() for page in self._document_pages())

    def persist_open_page_ui_states(self) -> None:
        """승인된 앱 종료 직전에 모든 문서 UI 상태를 저장한다."""
        for page in self._document_pages():
            self._save_page_ui_state(page)

    def cleanup_empty_card_before_exit(self) -> None:
        """종료를 막지 않고 모든 문서 페이지의 빈 확정 카드를 정리한다."""
        for page in self._document_pages():
            try:
                page.cleanup_empty_card_before_exit()
            except BaseException:
                LOGGER.exception(
                    "창 종료 전 빈 카드 정리 중 예외가 발생했습니다: document=%s",
                    page.document_id,
                )

    def retain_application_owners(self, *owners: object) -> None:
        """호환 진입점에서 앱 단위 QObject의 수명을 창과 함께 유지한다."""
        self._application_owners = owners

    def save_document_ui_state(
        self,
        document_id: str,
        *,
        selected_card_id: str | None = None,
        list_scroll_position: int = 0,
        sort_mode: str = "recency",
        editor_card_id: str | None = None,
        editor_base_revision_id: str | None = None,
        editor_cursor_qchar: int | None = None,
        editor_split_sizes: tuple[int, int] | None = None,
    ) -> DocumentUiState:
        """카드 UI가 전달한 문서별 상태를 DB에 저장한다."""
        if sort_mode not in {"recency", "position", "capture"}:
            raise ValueError(f"지원하지 않는 정렬 모드입니다: {sort_mode}")
        if editor_split_sizes is not None and (
            len(editor_split_sizes) != 2
            or any(size < 0 for size in editor_split_sizes)
        ):
            raise ValueError(f"잘못된 분할 보기 크기입니다: {editor_split_sizes}")
        if self._repositories.get_document(document_id) is None:
            raise KeyError(f"존재하지 않는 문서입니다: {document_id}")
        state = DocumentUiState(
            document_id=document_id,
            selected_card_id=selected_card_id,
            list_scroll_position=list_scroll_position,
            sort_mode=sort_mode,
            editor_card_id=editor_card_id,
            editor_base_revision_id=editor_base_revision_id,
            editor_cursor_qchar=editor_cursor_qchar,
            editor_split_sizes=editor_split_sizes,
            updated_at_us=time.time_ns() // 1_000,
        )
        self._state_store.save_document_ui_state(state)
        self._document_ui_states[document_id] = state
        return state

    def open_document(self, document_id: str) -> bool:
        """문서를 현재 창 또는 소유 창에 연다."""
        if self._document_open_router is not None:
            return self._document_open_router(self, document_id)
        return self.open_document_local(document_id)

    def open_document_local(
        self,
        document_id: str,
        *,
        app_driven: bool = False,
    ) -> bool:
        """WindowManager가 중복 개방을 확인한 문서를 이 창에 연다."""
        if self.active_document_id == document_id:
            self.focus_active_editor()
            return True
        if app_driven:
            page = self.active_document_page()
            if page is not None:
                if not page.protect_now():
                    return False
                if not page.detach_editor_session_quietly():
                    return False
        elif not self._can_leave_active_editor(protect_now=True):
            return False
        page = self._create_page(document_id)
        if page is None:
            return False
        state = self._state_store.load_document_ui_state(document_id)
        if state is not None:
            self._document_ui_states[document_id] = state
            self._apply_ui_state_to_page(page, state)
        previous = self._install_page(page)
        if previous is not None:
            self._save_page_ui_state(previous)
            previous.deleteLater()
        self._save_workspace_from_ui()
        self.focus_active_editor()
        self._update_status()
        return True

    def closeEvent(self, event: QCloseEvent) -> None:
        """창 종료 전 workspace 상태 저장을 보장한다."""
        try:
            if not self.protect_open_pages():
                event.ignore()
                return
            if not self.can_leave_open_pages():
                event.ignore()
                return
            self.persist_open_page_ui_states()
            self.save_workspace()
            self._save_window_geometry()
            try:
                self.cleanup_empty_card_before_exit()
            except BaseException:
                LOGGER.exception(
                    "창 종료 전 빈 카드 정리에 실패했지만 종료를 계속합니다: "
                    "window=%s",
                    self.window_id,
                )
            if self._window_close_callback is not None:
                self._window_close_callback(self)
        except BaseException as error:
            LOGGER.exception("창 종료 전 workspace 상태 저장에 실패했습니다.")
            QMessageBox.critical(self, "작업 상태 저장 실패", str(error))
            event.ignore()
            return
        event.accept()

    def event(self, event: QEvent) -> bool:
        """창 비활성화 직전에 최신 초안 보호를 비차단으로 시도한다."""
        if event.type() == QEvent.Type.WindowDeactivate:
            try:
                self.protect_open_pages_quietly()
            except BaseException:
                LOGGER.exception("창 비활성화 전 recovery draft 보호에 실패했습니다.")
        if event.type() == QEvent.Type.WindowActivate:
            self.sync_device_settings()
        return super().event(event)

    def sync_device_settings(self) -> None:
        """다른 창이 바꾼 장치 설정을 이 창의 메뉴 표시와 카드 목록에 맞춘다."""
        enabled = self._multi_selection_setting()
        # 되돌아온 toggled 가 같은 값을 다시 저장하지 않도록 표시만 맞춘다.
        blocked = self.multi_selection_action.blockSignals(True)
        self.multi_selection_action.setChecked(enabled)
        self.multi_selection_action.blockSignals(blocked)
        if self._page is not None:
            self._page.stream.set_multi_selection_enabled(enabled)

    def _create_page(self, document_id: str) -> DocumentPage | None:
        """문서가 열 수 있는 상태면 페이지를 만들어 반환한다."""
        document = self._repositories.get_document(document_id)
        if (
            document is None
            or document.trashed_at_us is not None
            or document.archived_at_us is not None
        ):
            return None

        page = DocumentPage(
            self._repositories.database,
            self._repositories,
            document_id,
            settings=self._settings,
            policy_store=self._policy_store,
            destructive_preflight=self._destructive_preflight,
            parent=self._page_host,
        )
        page.setObjectName(f"documentPage_{document_id}")
        for card_id, disposition in self._startup_recovery_dispositions.items():
            card = self._repositories.get_card(card_id)
            if card is not None and card.document_id == document_id:
                page.set_recovery_disposition(card_id, disposition)
        page.card_opened.connect(self._startup_suppressed_card_ids.discard)
        page.content_changed.connect(self._handle_page_content_changed)
        page.editor.status_changed.connect(
            lambda _status, _text: self._update_status()
        )
        return page

    def _install_page(
        self,
        page: DocumentPage | None,
    ) -> DocumentPage | None:
        """새 페이지를 호스트에 걸고 떼어낸 구 페이지를 반환한다."""
        previous = self._page
        if previous is not None:
            self._page_host_layout.removeWidget(previous)
            previous.setParent(None)
        self._page = page
        if page is not None:
            self._page_host_layout.addWidget(page)
            page.show()
            document = self._repositories.get_document(page.document_id)
            self.setWindowTitle(
                "pyNote" if document is None else f"{document.title} — pyNote"
            )
        else:
            self.setWindowTitle("pyNote")
        return previous

    def _handle_document_state_changed(self, document_id: str) -> None:
        document = self._repositories.get_document(document_id)
        if self._document_list_dialog is not None:
            self._document_list_dialog.refresh()
        if (
            document is None
            or document.trashed_at_us is not None
            or document.archived_at_us is not None
        ):
            if self.active_document_id == document_id:
                self._remove_page_after_change(
                    document_id,
                    save_ui_state=document is not None,
                )
            return
        if self.active_document_id == document_id:
            self.setWindowTitle(f"{document.title} — pyNote")
            self._save_workspace_from_ui()
            self._update_status()

    def apply_document_change(self, document_id: str) -> None:
        """앱 변경 버스의 문서 변경을 목록 대화상자와 페이지에 반영한다."""
        if self._document_list_dialog is not None:
            self._document_list_dialog.refresh()
        document = self._repositories.get_document(document_id)
        if (
            document is None
            or document.trashed_at_us is not None
            or document.archived_at_us is not None
        ):
            if self.active_document_id == document_id:
                self._remove_page_after_change(
                    document_id,
                    save_ui_state=document is not None,
                )
            return
        if self.active_document_id == document_id:
            self.setWindowTitle(f"{document.title} — pyNote")
            if self._page is not None and not self._publishing_page_content_change:
                self._page.refresh()
            self._save_workspace_from_ui()
            self._update_status()

    def page_for_document(self, document_id: str) -> DocumentPage | None:
        """지정 문서가 현재 열렸으면 작업 페이지를 반환한다."""
        return self._page if self.active_document_id == document_id else None

    def resume_recovery_card(self, card_id: str) -> bool:
        """복구 배치가 이 창에 연 문서의 카드 초안을 확정 없이 연다."""
        card = self._repositories.get_card(card_id)
        if card is None:
            return False
        page = self.page_for_document(card.document_id)
        if page is None:
            LOGGER.warning(
                "복구 배치 대상 문서가 이 창에 열려 있지 않습니다: document=%s",
                card.document_id,
            )
            return False
        return page.open_card(card_id, app_driven=True)

    def _remove_page_after_change(
        self,
        document_id: str,
        *,
        save_ui_state: bool,
    ) -> None:
        page = self._page
        if page is None or page.document_id != document_id:
            return
        if save_ui_state:
            self._save_page_ui_state(page)
        removed = self._install_page(None)
        if removed is not None:
            removed.deleteLater()
        self._document_ui_states.pop(document_id, None)
        self._save_workspace_from_ui()
        self._update_status()
        self._refill_after_document_removal()

    def _refill_after_document_removal(self) -> None:
        """시스템 주도 회수로 비워진 창을 즉시 입력 가능한 문서로 채운다."""
        if self._window_refill_callback is not None:
            self._window_refill_callback(self)
            return
        available = [
            document
            for document in self._repositories.list_documents()
            if document.archived_at_us is None and document.trashed_at_us is None
        ]
        available.sort(
            key=lambda document: (
                -document.updated_at_us,
                -document.created_at_us,
                document.id,
            )
        )
        if available:
            if self.open_document_local(available[0].id):
                return
            LOGGER.error(
                "빈 창에 최근 문서를 자동으로 열지 못했습니다: %s",
                available[0].id,
            )
        new_document = document_service.create_document(self._repositories)
        self.open_document_local(new_document.id)
        self._publish_document_change(new_document.id)

    def _publish_document_change(self, document_id: str) -> None:
        if self._document_change_publisher is None:
            self._handle_document_state_changed(document_id)
            return
        self._document_change_publisher(document_id)

    def _can_leave_active_editor(self, *, protect_now: bool) -> bool:
        page = self.active_document_page()
        return page is None or page.can_leave_editor(protect_now=protect_now)

    def _document_pages(self) -> tuple[DocumentPage, ...]:
        return () if self._page is None else (self._page,)

    def reset_window_geometry(self, _checked: bool = False) -> None:
        """창을 기본 크기로 되돌리고 현재 화면 중앙에 배치한다."""
        self.showNormal()
        self._place_default_window()

    def _restore_window_geometry(self) -> None:
        stored = (
            None
            if self._settings is None
            else self._settings.value(window_geometry_key(self.window_id))
        )
        geometry = (
            stored
            if isinstance(stored, QByteArray)
            else QByteArray(stored)
            if isinstance(stored, bytes)
            else None
        )
        restored = geometry is not None and self.restoreGeometry(geometry)
        if restored and self._window_is_on_screen():
            return
        self.setWindowState(Qt.WindowState.WindowNoState)
        self._place_default_window()

    def _save_window_geometry(self) -> None:
        if self._settings is None:
            return
        self._settings.setValue(
            window_geometry_key(self.window_id),
            self.saveGeometry(),
        )
        self._settings.sync()
        if self._settings.status() is not QSettings.Status.NoError:
            raise RuntimeError("창 위치와 크기를 저장하지 못했습니다.")

    def _place_default_window(self) -> None:
        self.resize(DEFAULT_WINDOW_SIZE)
        screen = (
            QGuiApplication.screenAt(self.frameGeometry().center())
            or self.screen()
            or QGuiApplication.primaryScreen()
        )
        if screen is None:
            return
        frame = self.frameGeometry()
        frame.moveCenter(screen.availableGeometry().center())
        self.move(frame.topLeft())

    def _window_is_on_screen(self) -> bool:
        frame = self.frameGeometry()
        return any(
            screen.availableGeometry().intersects(frame)
            for screen in QGuiApplication.screens()
        )

    def _save_workspace_from_ui(self) -> None:
        if self._restoring_workspace:
            return
        try:
            self.save_workspace()
            self.workspace_changed.emit()
        except BaseException as error:
            LOGGER.exception("workspace 상태 저장에 실패했습니다.")
            message = f"작업 상태 저장 실패: {error}"
            self.statusBar().showMessage(message)
            self.workspace_save_failed.emit(message)

    def _update_status(self) -> None:
        document_id = self.active_document_id
        if document_id is None:
            self.statusBar().showMessage("문서를 선택하거나 새 문서를 만드세요.")
            return
        cards = [
            card
            for card in self._repositories.list_cards(document_id)
            if card.deleted_at_us is None
        ]
        character_count = sum(len(card.body) for card in cards)
        page = self.active_document_page()
        save_status = "모든 변경 저장됨"
        if page is not None:
            session = page.editor.session
            if session is not None and (
                session.dirty or page.editor.status is EditorStatus.SAVE_FAILED
            ):
                save_status = page.editor.status_text
        self.statusBar().showMessage(
            f"{len(cards)}개 카드 · {character_count}자 · {save_status} · 로컬 DB"
        )

    def active_document_page(self) -> DocumentPage | None:
        """현재 문서의 실제 카드 작업 페이지를 반환한다."""
        return self._page

    def focus_active_editor(self) -> None:
        """활성 문서의 단일 편집면으로 작업 초점을 옮긴다."""
        page = self.active_document_page()
        if page is not None:
            page.focus_editor()

    def _create_new_document(self) -> None:
        document = document_service.create_document(self._repositories)
        self._handle_document_created(document.id)

    def _handle_document_created(self, document_id: str) -> None:
        if self._open_document_in_new_window(document_id):
            self._close_document_list()
        self._publish_document_change(document_id)

    def _open_document_in_new_window(self, document_id: str) -> bool:
        if self._open_in_new_window_callback is not None:
            return self._open_in_new_window_callback(self, document_id)
        return self.open_document_local(document_id)

    def _open_document_from_list(self, document_id: str) -> None:
        if self.open_document(document_id):
            self._close_document_list()

    def _open_document_from_list_in_new_window(self, document_id: str) -> None:
        if self._open_document_in_new_window(document_id):
            self._close_document_list()

    def _close_document_list(self) -> None:
        if self._document_list_dialog is not None:
            self._document_list_dialog.accept()

    def _open_document_list(self) -> None:
        if self._document_list_dialog is None:
            dialog = DocumentListDialog(
                self._repositories,
                time_format=self._time_format,
                destructive_preflight=self._destructive_preflight,
                parent=self,
            )
            dialog.document_open_requested.connect(self._open_document_from_list)
            dialog.document_created.connect(self._handle_document_created)
            dialog.open_in_new_window_requested.connect(
                self._open_document_from_list_in_new_window
            )
            dialog.document_purge_requested.connect(self._purge_document)
            dialog.document_state_changed.connect(self._publish_document_change)
            self._document_list_dialog = dialog
        if self._settings is not None:
            # 목록을 처음 여는 시점이 설정 변경 뒤일 수 있다.
            self._document_list_dialog.apply_display_settings(
                str(
                    self._settings.value(
                        "display/time_format",
                        "yyyy-MM-dd HH:mm",
                    )
                ),
                str(self._settings.value("display/timezone", "system")),
            )
        self._document_list_dialog.refresh()
        self._document_list_dialog.show()
        self._document_list_dialog.raise_()
        self._document_list_dialog.activateWindow()

    def _create_actions(self) -> None:
        self.new_window_action = QAction("새 창", self)
        self.new_window_action.setObjectName("newWindowAction")
        self.new_window_action.setShortcut(QKeySequence("Ctrl+Shift+N"))
        if self._new_window_callback is not None:
            self.new_window_action.triggered.connect(self._new_window_callback)

        self.new_document_action = QAction("새 문서", self)
        self.new_document_action.setObjectName("newDocumentAction")
        self.new_document_action.setShortcut(QKeySequence("Ctrl+N"))
        self.new_document_action.triggered.connect(self._create_new_document)

        self.document_list_action = QAction("문서 목록…", self)
        self.document_list_action.setObjectName("documentListAction")
        self.document_list_action.setShortcut(QKeySequence("Ctrl+O"))
        self.document_list_action.triggered.connect(self._open_document_list)

        self.search_action = QAction("문서와 카드 검색…", self)
        self.search_action.setObjectName("globalSearchAction")
        self.search_action.setShortcut(QKeySequence("Ctrl+P"))
        self.search_dialog = SearchDialog(self._repositories, parent=self)
        self.search_action.triggered.connect(self.search_dialog.focus_search)
        self.search_dialog.result_activated.connect(self._open_search_result)

        self.import_action = QAction("파일 가져오기…", self)
        self.import_action.setObjectName("importTextAction")
        self.import_action.setShortcut(QKeySequence("Ctrl+Shift+I"))
        if self.import_controller is None:
            self.import_action.setEnabled(False)
        else:
            self.import_action.triggered.connect(self._open_import_dialog)

        self.export_action = QAction("TXT/Markdown 내보내기…", self)
        self.export_action.setObjectName("exportTextAction")
        self.export_action.setShortcut(QKeySequence("Ctrl+Shift+E"))
        self.export_action.triggered.connect(self._export_active_document)
        self.backup_action = QAction("DB 백업 만들기…", self)
        self.backup_action.setObjectName("createBackupAction")
        self.backup_action.setShortcut(QKeySequence("Ctrl+Alt+B"))
        self.backup_action.triggered.connect(self._create_backup)
        self.restore_backup_action = QAction("DB 백업을 파일로 복원…", self)
        self.restore_backup_action.setObjectName("restoreBackupAction")
        self.restore_backup_action.setShortcut(QKeySequence("Ctrl+Alt+R"))
        self.restore_backup_action.triggered.connect(self._restore_backup_to_file)

        self.card_list_action = QAction("카드 목록", self)
        self.card_list_action.setObjectName("showCardListAction")
        self.card_list_action.setShortcut(QKeySequence("Ctrl+Shift+P"))
        self.card_list_action.triggered.connect(self._focus_card_list)
        self.history_action = QAction("변경 이력", self)
        self.history_action.setObjectName("showHistoryAction")
        self.history_action.setShortcut(QKeySequence("Ctrl+Shift+H"))
        self.history_action.triggered.connect(self._show_history)
        self.back_action = QAction("카드 목록으로 돌아가기", self)
        self.back_action.setObjectName("backToCardListAction")
        self.back_action.setShortcuts(
            [QKeySequence("Esc"), QKeySequence("Alt+Left")]
        )
        self.back_action.triggered.connect(self._focus_card_list)
        self.focus_action = QAction("집중 모드", self)
        self.focus_action.setObjectName("focusModeAction")
        self.focus_action.setShortcut(QKeySequence("F11"))
        self.focus_action.setCheckable(True)
        self.focus_action.toggled.connect(self._set_focus_mode)
        self.wrap_action = QAction("편집기 줄 바꿈 표시", self)
        self.wrap_action.setObjectName("editorLineWrapAction")
        self.wrap_action.setCheckable(True)
        self.wrap_action.setChecked(True)
        self.wrap_action.toggled.connect(self._set_editor_wrap)
        self.multi_selection_action = QAction("카드 다중 선택", self)
        self.multi_selection_action.setObjectName("cardMultiSelectionAction")
        self.multi_selection_action.setCheckable(True)
        self.multi_selection_action.setChecked(self._multi_selection_setting())
        self.multi_selection_action.toggled.connect(self._set_multi_selection)
        self.reset_window_action = QAction("원래 크기로", self)
        self.reset_window_action.setObjectName("resetWindowGeometryAction")
        self.reset_window_action.triggered.connect(self.reset_window_geometry)

        self.settings_action = QAction("설정…", self)
        self.settings_action.setObjectName("settingsAction")
        self.settings_action.triggered.connect(self._open_settings)
        self.data_location_action = QAction("데이터 위치 표시", self)
        self.data_location_action.setObjectName("showDataLocationAction")
        self.data_location_action.triggered.connect(self._show_data_location)
        self.license_action = QAction("오픈소스 라이선스", self)
        self.license_action.setObjectName("openSourceLicensesAction")
        self.license_action.triggered.connect(self._show_licenses)
        self.first_run_action = QAction("처음 사용 안내", self)
        self.first_run_action.setObjectName("firstRunGuideAction")
        self.first_run_action.triggered.connect(
            lambda: self._show_first_run_notice(force=True)
        )

        for action in (
            self.new_window_action,
            self.new_document_action,
            self.document_list_action,
            self.search_action,
            self.import_action,
            self.export_action,
            self.backup_action,
            self.restore_backup_action,
            self.card_list_action,
            self.history_action,
            self.back_action,
            self.focus_action,
        ):
            action.setShortcutContext(Qt.ShortcutContext.ApplicationShortcut)
            self.addAction(action)

    def _create_menus(self) -> None:
        self.window_menu = self.menuBar().addMenu("창")
        self.window_menu.addAction(self.new_window_action)

        self.file_menu = self.menuBar().addMenu("파일")
        self.file_menu.addAction(self.new_document_action)
        self.file_menu.addAction(self.document_list_action)
        self.file_menu.addSeparator()
        self.file_menu.addAction(self.import_action)
        self.file_menu.addAction(self.export_action)
        self.file_menu.addSeparator()
        self.file_menu.addAction(self.backup_action)
        self.file_menu.addAction(self.restore_backup_action)

        self.edit_menu = self.menuBar().addMenu("편집")
        self.edit_menu.addAction(self.search_action)
        self.edit_menu.addAction(self.settings_action)

        self.view_menu = self.menuBar().addMenu("보기")
        self.view_menu.addAction(self.reset_window_action)
        self.view_menu.addSeparator()
        self.view_menu.addAction(self.card_list_action)
        self.view_menu.addAction(self.history_action)
        self.view_menu.addAction(self.multi_selection_action)
        self.view_menu.addAction(self.wrap_action)
        self.view_menu.addAction(self.focus_action)

        self.help_menu = self.menuBar().addMenu("도움말")
        self.help_menu.addAction(self.first_run_action)
        self.help_menu.addAction(self.data_location_action)
        self.help_menu.addAction(self.license_action)

    def _save_page_ui_state(self, page: DocumentPage) -> None:
        current = page.stream.currentIndex()
        card_id_value = current.data(Qt.ItemDataRole.UserRole + 2)
        session = page.editor.session
        scroll_position = page.stream.verticalScrollBar().value()
        split_sizes = page.editor_workspace.editor_split_sizes()
        existing = self._document_ui_states.get(page.document_id)
        if (
            existing is not None
            and not isinstance(card_id_value, str)
            and session is None
            and scroll_position == 0
            and page.stream.card_model.rowCount() == 0
            and page.stream.card_model.sort_mode == existing.sort_mode
            and split_sizes == existing.editor_split_sizes
        ):
            return
        self.save_document_ui_state(
            page.document_id,
            selected_card_id=(
                card_id_value if isinstance(card_id_value, str) else None
            ),
            list_scroll_position=scroll_position,
            sort_mode=page.stream.card_model.sort_mode,
            editor_card_id=None if session is None else session.card_id,
            editor_base_revision_id=(
                None if session is None else session.base_revision_id
            ),
            editor_cursor_qchar=(
                None if session is None else page.editor.textCursor().position()
            ),
            editor_split_sizes=split_sizes,
        )

    def _apply_ui_state_to_page(
        self,
        page: DocumentPage,
        state: DocumentUiState,
    ) -> None:
        page.editor_workspace.set_editor_split_sizes(state.editor_split_sizes)
        sort_index = page.sort_combo.findData(state.sort_mode)
        if sort_index >= 0:
            page.sort_combo.setCurrentIndex(sort_index)
        if state.selected_card_id is not None:
            index = page.stream.card_model.index_for_card(state.selected_card_id)
            if index.isValid():
                page.stream.setCurrentIndex(index)
        page.stream.verticalScrollBar().setValue(state.list_scroll_position)
        if (
            state.editor_card_id is not None
            and state.editor_card_id not in self._startup_suppressed_card_ids
        ):
            card = self._repositories.get_card(state.editor_card_id)
            if card is not None and card.deleted_at_us is None:
                if page.open_card(state.editor_card_id, app_driven=True):
                    maximum = max(0, page.editor.document().characterCount() - 1)
                    position = min(
                        max(0, state.editor_cursor_qchar or 0),
                        maximum,
                    )
                    cursor = page.editor.textCursor()
                    cursor.setPosition(position)
                    page.editor.setTextCursor(cursor)

    def _prompt_recovery_drafts(self) -> tuple[RecoveryCandidate, ...]:
        coordinator = DraftCoordinator(
            self._repositories.database,
            self._repositories,
            parent=self,
        )
        recovered: list[RecoveryCandidate] = []
        for candidate in coordinator.recovery_candidates():
            choice = (
                self._recovery_choice_provider(candidate)
                if self._recovery_choice_provider is not None
                else self._ask_recovery_choice(candidate)
            )
            if choice is DraftDisposition.DISCARD:
                coordinator.resolve_candidate(
                    candidate.draft.id,
                    DraftDisposition.DISCARD,
                )
                continue
            if choice is DraftDisposition.RECOVER:
                recovered.append(candidate)
                if candidate.draft.card_id is not None:
                    self._startup_recovery_dispositions[
                        candidate.draft.card_id
                    ] = DraftDisposition.RECOVER
            elif (
                choice is DraftDisposition.LATER
                and candidate.draft.card_id is not None
            ):
                self._startup_suppressed_card_ids.add(candidate.draft.card_id)
        return tuple(recovered)

    def _resume_recovery_draft(
        self,
        candidates: tuple[RecoveryCandidate, ...],
    ) -> None:
        page = self.active_document_page()
        opened: dict[str, str | None] = {}
        if page is not None:
            session = page.editor.session
            opened[page.document_id] = None if session is None else session.card_id
        plans = build_recovery_plans(candidates, opened_editor_cards=opened)
        if not plans:
            return
        plan = next(
            (
                value
                for value in plans
                if page is not None and value.document_id == page.document_id
            ),
            plans[0],
        )
        if page is not None and page.document_id != plan.document_id:
            if not self.open_document_local(plan.document_id, app_driven=True):
                return
            page = self.active_document_page()
        if page is None:
            return
        candidate_card_ids = (
            plan.display_card_id,
            *plan.deferred_card_ids,
        )
        session = page.editor.session
        if session is not None and session.card_id in candidate_card_ids:
            return
        page.open_card(plan.display_card_id, app_driven=True)

    def _ask_recovery_choice(
        self,
        candidate: RecoveryCandidate,
    ) -> DraftDisposition:
        box = QMessageBox(self)
        box.setWindowTitle("미저장 초안 발견")
        box.setText(
            "확정본이 recovery draft보다 새롭습니다."
            if candidate.committed_is_newer
            else "확정본보다 새로운 recovery draft가 있습니다."
        )
        box.setInformativeText(
            "복구하면 초안 편집을 계속하고, 나중에는 초안을 보존합니다."
        )
        box.setDetailedText(
            f"[확정본]\n{candidate.committed_text}\n\n"
            f"[recovery draft]\n{candidate.draft.draft_text}"
        )
        recover = box.addButton("복구", QMessageBox.ButtonRole.AcceptRole)
        discard = box.addButton("버리기", QMessageBox.ButtonRole.DestructiveRole)
        later = box.addButton("나중에", QMessageBox.ButtonRole.RejectRole)
        box.setDefaultButton(cast(QPushButton, recover))
        box.exec()
        clicked = box.clickedButton()
        if clicked is recover:
            return DraftDisposition.RECOVER
        if clicked is discard:
            return DraftDisposition.DISCARD
        if clicked is later:
            return DraftDisposition.LATER
        return DraftDisposition.LATER

    def _handle_page_content_changed(self) -> None:
        document_id = self.active_document_id
        if document_id is not None:
            self._publishing_page_content_change = True
            try:
                self._publish_document_change(document_id)
            finally:
                self._publishing_page_content_change = False
        self._update_status()

    def _refresh_active_page(self, _value: object = None) -> None:
        page = self.active_document_page()
        if page is not None:
            page.refresh()
        self._handle_page_content_changed()

    def _open_import_dialog(self) -> None:
        controller = self.import_controller
        if controller is None:
            return
        dialog = ImportDialog(self)
        if dialog.exec() != QDialog.DialogCode.Accepted:
            return
        path = dialog.selected_path
        if path is not None:
            controller.start_import(
                self.window_id,
                self.active_document_id,
                path,
            )

    def _handle_import_finished(
        self,
        created: tuple[Card, ...],
    ) -> ImportRevealPlan | None:
        """버스 발행 전에 현재 요청 창의 공개 계획만 캡처한다."""
        target_document_id = created[-1].document_id
        page = self.active_document_page()
        if page is None or page.document_id != target_document_id:
            return None
        return ImportRevealPlan(
            page=page,
            document_id=target_document_id,
            card_id=created[-1].id,
            previous_view=page.view_state(),
        )

    def _execute_import_reveal(self, plan: ImportRevealPlan) -> None:
        """버스 발행 뒤에도 같은 페이지일 때만 생성 카드를 공개한다."""
        page = self.active_document_page()
        if page is None:
            return
        if page is not plan.page or page.document_id != plan.document_id:
            return
        if not page.reveal_created_card(plan.card_id):
            page.restore_view(plan.previous_view)

    def _open_search_result(self, document_id: str, card_id: object) -> None:
        routed_card_id = card_id if isinstance(card_id, str) else None
        self.search_dialog.hide()
        if self._search_result_router is not None:
            self._search_result_router(self, document_id, routed_card_id)
            return
        if not self.open_document(document_id):
            return
        page = self.active_document_page()
        if page is not None and routed_card_id is not None:
            page.open_card(routed_card_id)

    def _focus_card_list(self) -> None:
        page = self.active_document_page()
        if page is None:
            return
        if (
            page.mode_stack.currentWidget() is page.editor_workspace
            and page.editor.session is not None
        ):
            # 편집기가 입력기 자리를 차지하고 있으므로 "목록으로 돌아가기"는 곧
            # 편집기 닫기다. 취소 버튼과 같은 이탈 게이트를 태운다. 이력 화면에
            # 있을 때는 먼저 카드 화면으로 돌아가야 하므로 이 분기를 타지 않는다.
            page.editor.request_close()
            return
        page.focus_card_list()

    def _show_history(self) -> None:
        page = self.active_document_page()
        if page is not None:
            page.show_history()

    def _export_active_document(self) -> None:
        document_id = self.active_document_id
        if document_id is None:
            QMessageBox.warning(self, "내보내기", "활성 문서가 없습니다.")
            return
        filename, selected_filter = QFileDialog.getSaveFileName(
            self,
            "TXT/Markdown 내보내기",
            "",
            "텍스트 파일 (*.txt);;Markdown (*.md)",
        )
        if not filename:
            return
        path = Path(filename)
        if not path.suffix:
            path = path.with_suffix(".md" if "Markdown" in selected_filter else ".txt")
        newline_name, accepted = QInputDialog.getItem(
            self,
            "줄바꿈 형식",
            "내보내기 줄바꿈",
            ("LF", "CRLF"),
            editable=False,
        )
        if not accepted:
            return
        try:
            export_document(
                path,
                self._repositories,
                document_id,
                newline=(
                    NewlineFormat.CRLF if newline_name == "CRLF" else NewlineFormat.LF
                ),
            )
        except BaseException as error:
            LOGGER.exception("문서 내보내기에 실패했습니다.")
            QMessageBox.critical(self, "내보내기 실패", str(error))

    def _create_backup(self) -> None:
        filename, _ = QFileDialog.getSaveFileName(
            self,
            "DB 백업 만들기",
            str(self._repositories.database.path.with_suffix(".backup.sqlite3")),
            "SQLite DB (*.sqlite3)",
        )
        if not filename:
            return
        try:
            create_database_backup(
                self._repositories.database.path,
                Path(filename),
            )
        except BaseException as error:
            LOGGER.exception("사용자 요청 DB 백업에 실패했습니다.")
            QMessageBox.critical(self, "백업 실패", str(error))

    def _restore_backup_to_file(self) -> None:
        backup_name, _ = QFileDialog.getOpenFileName(
            self,
            "복원할 DB 백업",
            "",
            "SQLite DB (*.sqlite3)",
        )
        if not backup_name:
            return
        try:
            inspect_backup(Path(backup_name))
        except BaseException as error:
            LOGGER.exception("복원할 DB 백업 검증에 실패했습니다.")
            QMessageBox.critical(self, "백업 검증 실패", str(error))
            return
        destination_name, _ = QFileDialog.getSaveFileName(
            self,
            "검증된 백업을 새 DB 파일로 복원",
            "",
            "SQLite DB (*.sqlite3)",
        )
        if not destination_name:
            return
        try:
            restore_database(Path(backup_name), Path(destination_name))
        except BaseException as error:
            LOGGER.exception("DB 백업 복원에 실패했습니다.")
            QMessageBox.critical(self, "백업 복원 실패", str(error))

    def _purge_document(self, document_id: str) -> None:
        first = QMessageBox.warning(
            self,
            "비가역 문서 완전 삭제",
            "문서의 카드·초안·리비전·이력을 모두 물리 삭제합니다. 계속할까요?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No,
        )
        if first is not QMessageBox.StandardButton.Yes:
            return
        text, accepted = QInputDialog.getText(
            self,
            "문서 완전 삭제 재확인",
            "되돌릴 수 없습니다. PURGE를 입력하세요.",
        )
        if not accepted or text != "PURGE":
            return
        if (
            self._destructive_preflight is not None
            and not self._destructive_preflight(document_id)
        ):
            return
        retention_days = self._policy_store.load().trash_retention_days
        try:
            PurgeService(
                self._repositories.database,
                self._repositories,
            ).purge_document(document_id, retention_days=retention_days)
        except BaseException as error:
            LOGGER.exception("문서 purge에 실패했습니다.")
            QMessageBox.critical(self, "문서 완전 삭제 실패", str(error))
            return
        self._document_ui_states.pop(document_id, None)
        self._publish_document_change(document_id)

    def _open_settings(self) -> None:
        if self._settings is None:
            QMessageBox.warning(self, "설정", "이 창에는 설정 저장소가 연결되지 않았습니다.")
            return
        dialog = SettingsDialog(
            self._settings,
            policy_store=self._policy_store,
            parent=self,
        )
        dialog.settings_applied.connect(self._apply_settings)
        dialog.exec()

    def _apply_settings(self) -> None:
        if self._settings is not None:
            if self._document_list_dialog is not None:
                self._document_list_dialog.apply_display_settings(
                    str(
                        self._settings.value(
                            "display/time_format",
                            "yyyy-MM-dd HH:mm",
                        )
                    ),
                    str(self._settings.value("display/timezone", "system")),
                )
            self.multi_selection_action.setChecked(self._multi_selection_setting())
        if self._page is not None:
            self._page.apply_settings()

    def _multi_selection_setting(self) -> bool:
        """카드 다중 선택 허용 여부를 장치 설정에서 읽는다."""
        if self._settings is None:
            return False
        return bool(
            self._settings.value("cards/multi_selection_enabled", False, type=bool)
        )

    def _set_multi_selection(self, enabled: bool) -> None:
        """보기 메뉴 토글을 장치 설정에 저장하고 현재 카드 목록에 적용한다."""
        if self._settings is not None:
            self._settings.setValue("cards/multi_selection_enabled", enabled)
            # 다른 창은 활성화될 때 파일에서 다시 읽는다 — 지금 쓰지 않으면 그때
            # 옛 값을 읽는다.
            self._settings.sync()
        if self._page is not None:
            self._page.stream.set_multi_selection_enabled(enabled)

    def _set_focus_mode(self, enabled: bool) -> None:
        self._focus_mode = enabled
        self.menuBar().setVisible(not enabled)
        self.statusBar().setVisible(not enabled)
        if enabled and self._document_list_dialog is not None:
            # 탐색기 패널 시절 집중 모드가 문서 목록을 감췄던 동작의 등가물이다.
            self._document_list_dialog.hide()

    def _set_editor_wrap(self, enabled: bool) -> None:
        if self._page is not None:
            self._page.editor.set_wrap_visible(enabled)

    def _show_first_run_notice(self, *, force: bool = False) -> None:
        if (
            not force
            and self._settings is not None
            and bool(self._settings.value("first_run/guide_shown", False, type=bool))
        ):
            return
        message = (
            "처음 사용 안내 — ‘위치’는 문서 안에서 이동 가능한 현재 순서이고, "
            "‘기록 #’은 바뀌지 않는 최초 생성 순번입니다. "
            f"사용자 데이터: {self._repositories.database.path}"
        )
        if force:
            QMessageBox.information(self, "pyNote 처음 사용 안내", message)
        else:
            self.first_run_banner.setText(message)
            self.first_run_banner.show()
        if self._settings is not None:
            self._settings.setValue("first_run/guide_shown", True)
            self._settings.sync()

    def _show_data_location(self) -> None:
        QMessageBox.information(
            self,
            "사용자 데이터 위치",
            str(self._repositories.database.path),
        )

    def _show_licenses(self) -> None:
        QMessageBox.information(
            self,
            "오픈소스 라이선스",
            "pyNote는 PySide6(Qt for Python)를 사용합니다 — LGPL-3.0-only, "
            "GPL-2.0-only, GPL-3.0-only 중 하나 또는 유효한 Qt 상용 라이선스.\n"
            "라이선스 고지는 THIRD_PARTY_NOTICES.md를 확인하세요.",
        )
