from __future__ import annotations

import argparse
import hashlib
import logging
import os
import stat
import sys
import tempfile
import time
import uuid
from collections.abc import Callable, Sequence
from pathlib import Path
from typing import cast

from PySide6.QtCore import QLockFile, QObject, QSettings, QStandardPaths, QTimer, Signal
from PySide6.QtNetwork import QLocalServer, QLocalSocket
from PySide6.QtWidgets import QApplication, QMessageBox, QPushButton

from pynote.application import document_service
from pynote.application.card_service import CardService
from pynote.application.draft_coordinator import (
    DraftCoordinator,
    DraftDisposition,
    RecoveryCandidate,
    build_recovery_plans,
)
from pynote.domain.models import Card
from pynote.infrastructure.backup import (
    AutomaticBackupManager,
    MigrationBackupHook,
    PeriodicQuickCheck,
)
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories, WorkspaceWindow
from pynote.infrastructure.settings import (
    DataPolicySettingsStore,
    migrate_legacy_window_geometry,
    window_geometry_key,
)
from pynote.ui.import_dialog import ImportController
from pynote.ui.main_window import (
    DocumentUiState,
    MainWindow,
    RecoveryChoiceProvider,
    WorkspaceState,
)

LOGGER = logging.getLogger(__name__)

DEVICE_SETTING_DEFAULTS: dict[str, object] = {
    "display/time_format": "yyyy-MM-dd HH:mm",
    "display/timezone": "system",
    "editor/font_family": "",
    "editor/font_size": 11,
    "editor/line_spacing": 1.0,
    "backup/location": "",
    "cards/multi_selection_enabled": False,
}
_NEW_WINDOW_MESSAGE = b"new-window\n"
_INSTANCE_RETRY_DELAYS_SECONDS = (0.025, 0.05)
_STALE_SOCKET_MIN_AGE_SECONDS = 1.0


def instance_socket_name(data_directory: Path) -> str:
    """데이터 디렉터리별 단일 인스턴스 소켓 이름을 만든다."""
    normalized = os.path.normcase(str(data_directory.resolve(strict=False)))
    digest = hashlib.sha256(normalized.encode("utf-8")).hexdigest()[:24]
    name = f"pynote-{digest}"
    if os.name == "nt":
        return name
    return str(Path(tempfile.gettempdir()) / name)


class SingleInstanceGuard(QObject):
    """같은 데이터 디렉터리의 두 번째 실행을 새 창 명령으로 바꾼다."""

    new_window_requested = Signal()
    activation_requested = Signal()

    def __init__(
        self,
        data_directory: Path,
        *,
        parent: QObject | None = None,
    ) -> None:
        super().__init__(parent)
        self.socket_name = instance_socket_name(data_directory)
        self._server: QLocalServer | None = None
        self._lock_file: QLockFile | None = None
        self._clients: set[QLocalSocket] = set()
        self._client_buffers: dict[QLocalSocket, bytearray] = {}

    def acquire(self, *, timeout_ms: int = 500) -> bool:
        """첫 실행이면 서버를 열고, 두 번째 실행이면 새 창 명령 후 False를 반환한다."""
        if timeout_ms < 1:
            raise ValueError("단일 인스턴스 접속 제한 시간은 1ms 이상이어야 합니다.")
        if self._notify_existing(timeout_ms):
            return False

        lock_path = Path(tempfile.gettempdir()) / f"{Path(self.socket_name).name}.lock"
        lock_file = QLockFile(str(lock_path))
        lock_file.setStaleLockTime(0)
        if not lock_file.tryLock(0):
            for delay_seconds in _INSTANCE_RETRY_DELAYS_SECONDS:
                time.sleep(delay_seconds)
                if self._notify_existing(timeout_ms):
                    return False
            raise RuntimeError(
                "기존 인스턴스 소유권 잠금이 유지 중이지만 "
                "새 창 명령을 전달하지 못했습니다."
            )

        self._lock_file = lock_file
        server = QLocalServer(self)
        try:
            if self._listen_with_backoff(server):
                self._start_server(server)
                return True
            if not self._socket_is_verifiably_stale():
                message = server.errorString()
                raise RuntimeError(
                    "단일 인스턴스 서버를 열지 못했고 소켓을 stale로 "
                    f"검증하지 못했습니다: {message}"
                )
            LOGGER.warning("검증된 stale 단일 인스턴스 소켓을 제거합니다.")
            if not QLocalServer.removeServer(self.socket_name):
                raise RuntimeError("검증된 stale 단일 인스턴스 소켓을 제거하지 못했습니다.")
            if not server.listen(self.socket_name):
                message = server.errorString()
                raise RuntimeError(f"단일 인스턴스 서버를 열지 못했습니다: {message}")
            self._start_server(server)
            return True
        except BaseException:
            server.close()
            server.deleteLater()
            lock_file.unlock()
            self._lock_file = None
            raise

    def close(self) -> None:
        """소유한 로컬 서버와 대기 중 연결을 닫는다."""
        for client in tuple(self._clients):
            self._dispose_client(client, abort=True)
        if self._server is not None:
            self._server.close()
            QLocalServer.removeServer(self.socket_name)
            self._server.deleteLater()
            self._server = None
        if self._lock_file is not None:
            self._lock_file.unlock()
            self._lock_file = None

    def _notify_existing(self, timeout_ms: int) -> bool:
        socket = QLocalSocket(self)
        try:
            socket.connectToServer(self.socket_name)
            if not socket.waitForConnected(timeout_ms):
                socket.abort()
                return False
            if socket.write(_NEW_WINDOW_MESSAGE) < 0:
                message = socket.errorString()
                socket.abort()
                raise RuntimeError(
                    f"기존 인스턴스에 새 창 명령을 보내지 못했습니다: {message}"
                )
            socket.flush()
            if socket.bytesToWrite() > 0 and not socket.waitForBytesWritten(timeout_ms):
                message = socket.errorString()
                socket.abort()
                raise RuntimeError(
                    f"기존 인스턴스 새 창 명령을 완료하지 못했습니다: {message}"
                )
            socket.disconnectFromServer()
            return True
        finally:
            socket.abort()
            socket.deleteLater()

    def _listen_with_backoff(self, server: QLocalServer) -> bool:
        if server.listen(self.socket_name):
            return True
        for delay_seconds in _INSTANCE_RETRY_DELAYS_SECONDS:
            time.sleep(delay_seconds)
            if server.listen(self.socket_name):
                return True
        return False

    def _socket_is_verifiably_stale(self) -> bool:
        if os.name == "nt":
            return self._lock_file is not None and self._lock_file.isLocked()
        socket_path = Path(self.socket_name)
        try:
            metadata = socket_path.stat()
        except FileNotFoundError:
            return False
        except OSError:
            LOGGER.exception("단일 인스턴스 소켓 메타데이터를 읽지 못했습니다.")
            return False
        age_seconds = time.time() - metadata.st_mtime
        return (
            self._lock_file is not None
            and self._lock_file.isLocked()
            and stat.S_ISSOCK(metadata.st_mode)
            and age_seconds >= _STALE_SOCKET_MIN_AGE_SECONDS
        )

    def _start_server(self, server: QLocalServer) -> None:
        self._server = server
        server.newConnection.connect(self._accept_connections)

    def _accept_connections(self) -> None:
        if self._server is None:
            return
        while self._server.hasPendingConnections():
            client = self._server.nextPendingConnection()
            if client is None:
                continue
            self._clients.add(client)
            self._client_buffers[client] = bytearray()
            client.readyRead.connect(
                lambda current=client: self._read_client(current)
            )
            client.disconnected.connect(
                lambda current=client: self._finish_client(current)
            )
            self._read_client(client)

    def _read_client(self, client: QLocalSocket) -> None:
        buffer = self._client_buffers.get(client)
        if buffer is None:
            return
        buffer.extend(client.readAll().data())
        while b"\n" in buffer:
            command, _, remainder = buffer.partition(b"\n")
            buffer[:] = remainder
            self._handle_command(bytes(command) + b"\n")

    def _handle_command(self, command: bytes) -> None:
        if command != _NEW_WINDOW_MESSAGE:
            return
        self.new_window_requested.emit()
        self.activation_requested.emit()

    def _finish_client(self, client: QLocalSocket) -> None:
        self._read_client(client)
        self._dispose_client(client, abort=False)

    def _dispose_client(self, client: QLocalSocket, *, abort: bool) -> None:
        client.readyRead.disconnect()
        client.disconnected.disconnect()
        self._clients.discard(client)
        self._client_buffers.pop(client, None)
        if abort:
            client.abort()
        client.deleteLater()


class SqliteWorkspaceStateStore:
    """창 한 개의 workspace_windows와 문서 UI 상태를 잇는 어댑터다."""

    def __init__(
        self,
        database: Database,
        window_id: str | None = None,
    ) -> None:
        self._database = database
        self._connection = database.connection
        self._repositories = Repositories(database)
        self.window_id = window_id or self._first_or_new_window_id()

    def load_workspace(self) -> WorkspaceState:
        """이 창의 마지막 workspace 상태를 읽고 계약을 검증한다."""
        workspace = self._repositories.get_workspace_window(self.window_id)
        if workspace is None:
            return WorkspaceState((), None, 0)
        return WorkspaceState(
            open_document_ids=workspace.open_document_ids,
            active_document_id=workspace.active_document_id,
            updated_at_us=workspace.updated_at_us,
        )

    def save_workspace(
        self,
        open_document_ids: tuple[str, ...],
        active_document_id: str | None,
    ) -> WorkspaceState:
        """현재 문서와 활성 문서를 이 창 행으로 저장한다."""
        try:
            workspace = self._repositories.save_workspace_window(
                self.window_id,
                open_document_ids,
                active_document_id,
            )
        except BaseException:
            LOGGER.exception("workspace_windows 저장에 실패했습니다.")
            raise
        return WorkspaceState(
            open_document_ids=workspace.open_document_ids,
            active_document_id=workspace.active_document_id,
            updated_at_us=workspace.updated_at_us,
        )

    def _first_or_new_window_id(self) -> str:
        windows = self._repositories.list_workspace_windows()
        if windows:
            return windows[0].window_id
        window_id = str(uuid.uuid4())
        self._repositories.save_workspace_window(window_id, (), None)
        return window_id

    def load_document_ui_state(self, document_id: str) -> DocumentUiState | None:
        """문서별 선택·스크롤·정렬·편집 상태를 읽는다."""
        row = self._connection.execute(
            """
            SELECT *
            FROM document_ui_states
            WHERE document_id = ?
            """,
            (document_id,),
        ).fetchone()
        if row is None:
            return None
        split_left = row["editor_split_left"]
        split_right = row["editor_split_right"]
        return DocumentUiState(
            document_id=str(row["document_id"]),
            selected_card_id=row["selected_card_id"],
            list_scroll_position=int(row["list_scroll_position"]),
            sort_mode=str(row["sort_mode"]),
            editor_card_id=row["editor_card_id"],
            editor_base_revision_id=row["editor_base_revision_id"],
            editor_cursor_qchar=row["editor_cursor_qchar"],
            editor_split_sizes=(
                (int(split_left), int(split_right))
                if split_left is not None and split_right is not None
                else None
            ),
            updated_at_us=int(row["updated_at_us"]),
        )

    def save_document_ui_state(self, state: DocumentUiState) -> None:
        """문서별 UI 상태를 생성하거나 갱신한다."""
        if state.sort_mode not in {"recency", "position", "capture"}:
            raise ValueError(f"지원하지 않는 정렬 모드입니다: {state.sort_mode}")
        try:
            with self._database.transaction():
                self._connection.execute(
                    """
                    INSERT INTO document_ui_states(
                        document_id, selected_card_id, list_scroll_position,
                        sort_mode, editor_card_id, editor_base_revision_id,
                        editor_cursor_qchar, editor_split_left,
                        editor_split_right, updated_at_us
                    )
                    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                    ON CONFLICT(document_id) DO UPDATE SET
                        selected_card_id = excluded.selected_card_id,
                        list_scroll_position = excluded.list_scroll_position,
                        sort_mode = excluded.sort_mode,
                        editor_card_id = excluded.editor_card_id,
                        editor_base_revision_id = excluded.editor_base_revision_id,
                        editor_cursor_qchar = excluded.editor_cursor_qchar,
                        editor_split_left = excluded.editor_split_left,
                        editor_split_right = excluded.editor_split_right,
                        updated_at_us = excluded.updated_at_us
                    """,
                    (
                        state.document_id,
                        state.selected_card_id,
                        state.list_scroll_position,
                        state.sort_mode,
                        state.editor_card_id,
                        state.editor_base_revision_id,
                        state.editor_cursor_qchar,
                        None
                        if state.editor_split_sizes is None
                        else state.editor_split_sizes[0],
                        None
                        if state.editor_split_sizes is None
                        else state.editor_split_sizes[1],
                        state.updated_at_us,
                    ),
                )
        except BaseException:
            LOGGER.exception("document_ui_states 저장에 실패했습니다.")
            raise


def initialize_device_settings(settings: QSettings) -> None:
    """장치별 설정 키의 기본 골격을 QSettings에 만든다."""
    for key, default_value in DEVICE_SETTING_DEFAULTS.items():
        if not settings.contains(key):
            settings.setValue(key, default_value)
    settings.sync()
    if settings.status() is not QSettings.Status.NoError:
        raise RuntimeError("장치별 설정을 저장하지 못했습니다.")


class DocumentChangeBus(QObject):
    """문서 수준 변경을 모든 창에 전달한다."""

    document_changed = Signal(str)


class AppContext(QObject):
    """앱 단위 DB·저장소·점검·설정·변경 버스를 한 번 소유한다."""

    maintenance_failed = Signal(str)

    @classmethod
    def open(
        cls,
        database_path: Path,
        settings: QSettings | None = None,
        *,
        parent: QObject | None = None,
    ) -> AppContext:
        """정상 진입점에서 migration 백업 훅과 함께 DB를 연다."""
        device_settings = settings or QSettings("pyNote", "pyNote")
        backup_value = str(device_settings.value("backup/location", "")).strip()
        backup_directory = Path(backup_value) if backup_value else None
        database = Database(
            database_path,
            backup_hook=MigrationBackupHook(backup_directory),
        )
        try:
            return cls(database, device_settings, parent=parent)
        except BaseException:
            database.close()
            raise

    def __init__(
        self,
        database: Database,
        settings: QSettings | None = None,
        *,
        parent: QObject | None = None,
    ) -> None:
        super().__init__(parent)
        self.database = database
        self.settings = settings or QSettings("pyNote", "pyNote")
        initialize_device_settings(self.settings)
        self.repositories = Repositories(database)
        self.change_bus = DocumentChangeBus(self)
        backup_value = str(self.settings.value("backup/location", "")).strip()
        backup_directory = (
            Path(backup_value) if backup_value else database.path.parent / "backups"
        )
        backup_interval_hours = DataPolicySettingsStore(
            database
        ).load().backup_interval_hours
        self.backup_manager = AutomaticBackupManager(
            database.path,
            backup_directory,
            interval_hours=backup_interval_hours,
        )
        self.quick_check = PeriodicQuickCheck(
            database.connection,
            interval_hours=backup_interval_hours,
        )
        self.maintenance_timer = QTimer(self)
        interval_ms = min(
            round(backup_interval_hours * 60 * 60 * 1_000),
            2_147_483_647,
        )
        self.maintenance_timer.setInterval(interval_ms)
        self.maintenance_timer.timeout.connect(self.run_automatic_maintenance)

    def start_automatic_maintenance(self) -> None:
        """시작 즉시 점검·백업을 실행하고 같은 주기의 타이머를 시작한다."""
        if self.maintenance_timer.isActive():
            return
        self.run_automatic_maintenance()
        self.maintenance_timer.start()

    def run_automatic_maintenance(self) -> bool:
        """자동 백업 직전 quick_check를 실행하고 실패 시 백업을 중단한다."""
        try:
            self.quick_check.run_if_due(force=True)
            self.backup_manager.run_if_due()
        except BaseException as error:
            message = f"자동 백업 전 DB 무결성 검사 또는 백업에 실패했습니다: {error}"
            LOGGER.exception("자동 백업 전 DB 무결성 검사 또는 백업에 실패했습니다.")
            self.maintenance_failed.emit(message)
            return False
        return True


class WindowManager(QObject):
    """창 수명·문서 소유권·전역 recovery·destructive 게이트를 관리한다.

    앱 수명 가져오기도 여기가 소유한다 — 전용 `CardService` 와 단일
    `ImportController` 를 만들어 모든 창에 주입하고, 완료·실패 라우팅과 종료
    경계를 담당한다. 요청 창이 닫혀도 작업이 살아남아야 하므로 컨트롤러 수명을
    개별 창에 묶지 않는다.
    """

    def __init__(
        self,
        context: AppContext,
        *,
        recovery_choice_provider: RecoveryChoiceProvider | None = None,
        parent: QObject | None = None,
    ) -> None:
        super().__init__(parent)
        self.context = context
        self._recovery_choice_provider = recovery_choice_provider
        self._windows: dict[str, MainWindow] = {}
        self._document_windows: dict[str, MainWindow] = {}
        self._document_change_handlers: dict[str, Callable[[str], None]] = {}
        self._recovery_dispositions: dict[str, DraftDisposition] = {}
        self._later_suppressed_card_ids: set[str] = set()
        self._recovered_candidates: tuple[RecoveryCandidate, ...] = ()
        self._recovery_completed = False
        self._shutting_down = False
        import_service = CardService(
            self.context.database,
            self.context.repositories,
        )
        self.import_controller = ImportController(
            import_service,
            self._route_import_completed,
            self._route_import_failure,
            parent=self,
        )
        self.context.maintenance_failed.connect(self._report_maintenance_failure)

    @property
    def windows(self) -> tuple[MainWindow, ...]:
        """현재 생존 창을 생성 순서대로 반환한다."""
        return tuple(self._windows.values())

    def restore_windows(self) -> tuple[MainWindow, ...]:
        """전역 recovery 후 저장된 전 창을 복원하고 없으면 한 창을 만든다."""
        self._resolve_startup_recovery()
        records = self._validated_restore_records(
            self.context.repositories.list_workspace_windows()
        )
        if records:
            migrate_legacy_window_geometry(
                self.context.settings,
                records[0].window_id,
            )
            for record in records:
                self._create_window(record.window_id, create_row=False)
            for window in self.windows:
                self._prepare_window_for_input(window)
        else:
            self.create_window()
        self._resume_recoveries()
        self.context.start_automatic_maintenance()
        return self.windows

    def create_window(
        self,
        initial_document_id: str | None = None,
    ) -> MainWindow:
        """빈 복원 행과 창별 UI를 만들어 새 창을 표시한다."""
        self._protect_windows_quietly()
        window_id = str(uuid.uuid4())
        self.context.repositories.save_workspace_window(window_id, (), None)
        migrate_legacy_window_geometry(self.context.settings, window_id)
        window = self._create_window(window_id, create_row=False)
        if initial_document_id is None or not window.open_document_local(
            initial_document_id
        ):
            # 지정 문서를 열지 못하면 빈 창이 남는다 — 기본 채움으로 되돌린다.
            self._prepare_window_for_input(window)
        window.show()
        return window

    def open_document(self, requesting_window: MainWindow, document_id: str) -> bool:
        """이미 열린 문서는 소유 창을 활성화하고 아니면 요청 창에 연다."""
        owner = self._document_windows.get(document_id)
        if owner is not None and owner is not requesting_window:
            owner.open_document_local(document_id)
            self._protect_windows_quietly()
            _activate_window(owner)
            return True
        opened = requesting_window.open_document_local(document_id)
        if opened:
            self._refresh_document_mapping()
        return opened

    def open_document_in_new_window(
        self,
        requesting_window: MainWindow,
        document_id: str,
    ) -> bool:
        """소유 창이 있으면 활성화하고, 없을 때만 새 창을 만든다."""
        owner = self._document_windows.get(document_id)
        if owner is not None:
            self._protect_windows_quietly()
            _activate_window(owner)
            return True
        document = self.context.repositories.get_document(document_id)
        if (
            document is None
            or document.archived_at_us is not None
            or document.trashed_at_us is not None
        ):
            # 보관·휴지통 문서는 페이지를 만들 수 없다 — 창을 만들면 요청한
            # 문서가 아니라 엉뚱한 문서가 든 창이 남는다.
            return False
        self.create_window(initial_document_id=document_id)
        return True

    def open_search_result(
        self,
        requesting_window: MainWindow,
        document_id: str,
        card_id: str | None,
    ) -> bool:
        """검색 결과를 문서 소유 창에 열고 그 창을 활성화한다."""
        if not self.open_document(requesting_window, document_id):
            return False
        owner = self._document_windows.get(document_id)
        if owner is None:
            return False
        self._protect_windows_quietly()
        _activate_window(owner)
        if card_id is None:
            return True
        page = owner.page_for_document(document_id)
        return page is not None and page.open_card(card_id)

    def can_quit_application(self) -> bool:
        """전 창 초안을 보호한 뒤 편집기 이탈 승인을 순서대로 받는다."""
        windows = self.windows
        protected = True
        for window in windows:
            if not window.protect_open_pages():
                protected = False
        if not protected:
            return False
        if any(not window.can_leave_open_pages() for window in windows):
            return False
        for window in windows:
            window.persist_open_page_ui_states()
        for window in windows:
            try:
                window.cleanup_empty_card_before_exit()
            except BaseException:
                LOGGER.exception(
                    "앱 종료 전 빈 카드 정리에 실패했지만 종료 승인을 유지합니다: "
                    "window=%s",
                    window.window_id,
                )
        return True

    def _protect_windows_quietly(self) -> None:
        """앱 주도 창 전환 전에 현행 생존 창의 초안 보호를 시도한다."""
        for window in self.windows:
            try:
                window.protect_open_pages_quietly()
            except BaseException:
                LOGGER.exception(
                    "앱 주도 전환 전 창의 recovery draft 보호에 실패했습니다: "
                    "window=%s",
                    window.window_id,
                )

    def destructive_preflight(self, document_id: str) -> bool:
        """영향 문서 소유 창의 dirty 편집기가 이탈을 허용하는지 확인한다."""
        owner = self._document_windows.get(document_id)
        if owner is None:
            return True
        page = owner.page_for_document(document_id)
        return page is None or page.can_leave_editor(protect_now=True)

    def publish_document_change(self, document_id: str) -> None:
        """문서 변경을 모든 창에 동기 전파한다."""
        self.context.change_bus.document_changed.emit(document_id)
        self._refresh_document_mapping()

    def _route_import_completed(
        self,
        request_window_id: str,
        value: object,
    ) -> None:
        """확정된 가져오기를 버스로 발행하고 생존 요청 창에 공개한다."""
        if not isinstance(value, tuple) or not all(
            isinstance(card, Card) for card in value
        ):
            raise TypeError("가져오기 완료 신호에는 Card 튜플이 필요합니다.")
        created = cast(tuple[Card, ...], value)
        target_document_id = created[-1].document_id
        requester = self._windows.get(request_window_id)
        plan = (
            None
            if requester is None
            else requester._handle_import_finished(created)
        )
        self.publish_document_change(target_document_id)
        if requester is None or plan is None:
            return
        if self._windows.get(request_window_id) is not requester:
            return
        requester._execute_import_reveal(plan)

    def _route_import_failure(
        self,
        request_window_id: str,
        message: str,
    ) -> None:
        """실패 모달을 레지스트리에 남은 요청 창으로만 보낸다."""
        requester = self._windows.get(request_window_id)
        if requester is None:
            LOGGER.error(
                "닫힌 요청 창의 가져오기 실패를 기록합니다: window=%s, error=%s",
                request_window_id,
                message,
            )
            return
        QMessageBox.critical(requester, "가져오기 실패", message)

    def _deliver_document_change(
        self,
        window: MainWindow,
        document_id: str,
    ) -> None:
        try:
            window.apply_document_change(document_id)
        except BaseException:
            LOGGER.exception(
                "문서 변경 소비자 처리에 실패했습니다: window=%s, document=%s",
                window.window_id,
                document_id,
            )

    def handle_window_close(self, window: MainWindow) -> None:
        """비마지막 창만 복원 행과 geometry를 삭제하고 매핑에서 제거한다."""
        is_non_last = len(self._windows) > 1 and not self._shutting_down
        if not is_non_last and not self._shutting_down:
            self.import_controller.begin_shutdown()
            self._shutting_down = True
        if is_non_last:
            self.context.repositories.delete_workspace_window(window.window_id)
            self.context.settings.remove(window_geometry_key(window.window_id))
            self.context.settings.sync()
            if self.context.settings.status() is not QSettings.Status.NoError:
                raise RuntimeError("닫힌 창의 geometry를 삭제하지 못했습니다.")
        handler = self._document_change_handlers.pop(window.window_id, None)
        if handler is not None:
            self.context.change_bus.document_changed.disconnect(handler)
        window.workspace_changed.disconnect(self._refresh_document_mapping)
        self._windows.pop(window.window_id, None)
        self._refresh_document_mapping()
        window.deleteLater()

    def prepare_shutdown(self) -> None:
        """앱 종료에서는 전 창의 복원 행과 geometry를 보존한다."""
        self.import_controller.begin_shutdown()
        self._shutting_down = True
        for window in self.windows:
            window.persist_window_state()

    def _create_window(
        self,
        window_id: str,
        *,
        create_row: bool,
    ) -> MainWindow:
        if create_row:
            self.context.repositories.save_workspace_window(window_id, (), None)
        time_format = str(
            self.context.settings.value(
                "display/time_format",
                "yyyy-MM-dd HH:mm",
            )
        )
        window = MainWindow(
            self.context.repositories,
            SqliteWorkspaceStateStore(self.context.database, window_id),
            time_format=time_format,
            settings=self.context.settings,
            window_id=window_id,
            document_open_router=self.open_document,
            search_result_router=self.open_search_result,
            destructive_preflight=self.destructive_preflight,
            document_change_publisher=self.publish_document_change,
            import_controller=self.import_controller,
            new_window_callback=self.create_window,
            open_in_new_window_callback=self.open_document_in_new_window,
            window_close_callback=self.handle_window_close,
            window_refill_callback=self._prepare_window_for_input,
            startup_recovery_dispositions=self._recovery_dispositions,
            startup_suppressed_card_ids=self._later_suppressed_card_ids,
        )
        self._windows[window_id] = window
        window.workspace_changed.connect(self._refresh_document_mapping)

        def change_handler(document_id: str) -> None:
            self._deliver_document_change(window, document_id)

        self._document_change_handlers[window_id] = change_handler
        self.context.change_bus.document_changed.connect(change_handler)
        window.destroyed.connect(
            lambda _object=None, current_id=window_id: self._window_destroyed(
                current_id
            )
        )
        self._refresh_document_mapping()
        return window

    def _window_destroyed(self, window_id: str) -> None:
        self._windows.pop(window_id, None)
        self._document_change_handlers.pop(window_id, None)
        self._refresh_document_mapping()

    def _validated_restore_records(
        self,
        records: tuple[WorkspaceWindow, ...],
    ) -> tuple[WorkspaceWindow, ...]:
        seen_documents: set[str] = set()
        validated: list[WorkspaceWindow] = []
        for record in records:
            eligible = tuple(
                document_id
                for document_id in record.open_document_ids
                if (
                    (document := self.context.repositories.get_document(document_id))
                    is not None
                    and document.archived_at_us is None
                    and document.trashed_at_us is None
                    and document_id not in seen_documents
                )
            )
            retained = (
                record.active_document_id
                if record.active_document_id in eligible
                else next(iter(eligible), None)
            )
            if retained is not None:
                seen_documents.add(retained)
            document_ids = () if retained is None else (retained,)
            if (
                document_ids != record.open_document_ids
                or retained != record.active_document_id
            ):
                record = self.context.repositories.save_workspace_window(
                    record.window_id,
                    document_ids,
                    retained,
                )
            validated.append(record)
        return tuple(validated)

    def _refresh_document_mapping(self) -> None:
        self._document_windows.clear()
        for window in self.windows:
            for document_id in window.open_document_ids:
                if document_id in self._document_windows:
                    raise RuntimeError(
                        f"문서가 둘 이상의 창에 열렸습니다: {document_id}"
                    )
                self._document_windows[document_id] = window

    def _prepare_window_for_input(self, window: MainWindow) -> None:
        if window.open_document_ids:
            window.focus_active_editor()
            return
        available = [
            document
            for document in self.context.repositories.list_documents()
            if document.archived_at_us is None
            and document.trashed_at_us is None
            and document.id not in self._document_windows
        ]
        available.sort(
            key=lambda document: (
                -document.updated_at_us,
                -document.created_at_us,
                document.id,
            )
        )
        if available:
            if not self.open_document(window, available[0].id):
                LOGGER.error(
                    "빈 창에 최근 문서를 자동으로 열지 못했습니다: %s",
                    available[0].id,
                )
                raise RuntimeError("빈 창에 최근 문서를 자동으로 열지 못했습니다.")
        else:
            new_document = document_service.create_document(
                self.context.repositories
            )
            self.open_document(window, new_document.id)
            self.publish_document_change(new_document.id)
        window.focus_active_editor()

    def _resolve_startup_recovery(self) -> None:
        if self._recovery_completed:
            return
        coordinator = DraftCoordinator(
            self.context.database,
            self.context.repositories,
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
            elif choice is DraftDisposition.RECOVER:
                recovered.append(candidate)
                if candidate.draft.card_id is not None:
                    self._recovery_dispositions[
                        candidate.draft.card_id
                    ] = DraftDisposition.RECOVER
            elif (
                choice is DraftDisposition.LATER
                and candidate.draft.card_id is not None
            ):
                self._later_suppressed_card_ids.add(candidate.draft.card_id)
        self._recovered_candidates = tuple(recovered)
        self._recovery_completed = True

    def _resume_recoveries(self) -> None:
        plans = build_recovery_plans(
            self._recovered_candidates,
            opened_editor_cards=self._opened_editor_cards(),
        )
        for plan in plans:
            document = self.context.repositories.get_document(plan.document_id)
            if (
                document is None
                or document.archived_at_us is not None
                or document.trashed_at_us is not None
            ):
                LOGGER.warning(
                    "복구 배치 대상 문서가 열 수 없는 상태라 건너뜁니다: "
                    "document=%s",
                    plan.document_id,
                )
                continue
            owner = self._document_windows.get(plan.document_id)
            if owner is None:
                owner = self.create_window(initial_document_id=plan.document_id)
            if owner.active_document_id != plan.document_id:
                LOGGER.warning(
                    "복구 배치 대상 문서를 소유 창에서 열지 못했습니다: document=%s",
                    plan.document_id,
                )
                continue
            page = owner.active_document_page()
            candidate_card_ids = (
                plan.display_card_id,
                *plan.deferred_card_ids,
            )
            session = None if page is None else page.editor.session
            if session is not None and session.card_id in candidate_card_ids:
                continue
            if not owner.resume_recovery_card(plan.display_card_id):
                LOGGER.warning(
                    "복구 배치 대상 카드를 열지 못했습니다: document=%s card=%s",
                    plan.document_id,
                    plan.display_card_id,
                )

    def _opened_editor_cards(self) -> dict[str, str | None]:
        opened: dict[str, str | None] = {}
        for window in self.windows:
            page = window.active_document_page()
            if page is None:
                continue
            session = page.editor.session
            opened[page.document_id] = None if session is None else session.card_id
        return opened

    def _report_maintenance_failure(self, message: str) -> None:
        LOGGER.error("자동 유지보수 실패를 사용자에게 알립니다: %s", message)
        parent = self.windows[0] if self.windows else None
        QMessageBox.critical(parent, "자동 백업 실패", message)

    @staticmethod
    def _ask_recovery_choice(candidate: RecoveryCandidate) -> DraftDisposition:
        box = QMessageBox()
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


class ApplicationQuitCoordinator(QObject):
    """QApplication.quit 호출을 취소 가능한 전 창 이탈 게이트로 감싼다."""

    def __init__(
        self,
        application: QApplication,
        manager: WindowManager,
        *,
        parent: QObject | None = None,
    ) -> None:
        super().__init__(parent or application)
        self._application = application
        self._manager = manager
        self._qt_quit: Callable[[], None] = application.quit
        self._installed = True
        application.quit = self.quit

    def quit(self) -> None:
        """모든 창이 이탈을 승인한 경우에만 실제 Qt 종료를 요청한다."""
        if self._manager.can_quit_application():
            self._qt_quit()

    def uninstall(self) -> None:
        """테스트·수명 종료 시 원래 QApplication.quit 메서드를 복원한다."""
        if not self._installed:
            return
        self._application.quit = self._qt_quit
        self._installed = False


def create_main_window(
    database: Database,
    settings: QSettings | None = None,
) -> MainWindow:
    """호환 진입점에서 앱 자원과 첫 관리 창을 만든다."""
    context = AppContext(database, settings)
    manager = WindowManager(context)
    windows = manager.restore_windows()
    window = windows[0]
    window.retain_application_owners(context, manager)
    return window


def _parse_arguments(arguments: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(prog="pyNote")
    parser.add_argument(
        "--smoke",
        action="store_true",
        help="창 생성 직후 종료하여 기동 가능 여부만 확인합니다.",
    )
    parser.add_argument(
        "--database",
        type=Path,
        help="명시한 SQLite 파일을 사용합니다.",
    )
    return parser.parse_args(arguments)


def _default_database_path() -> Path:
    data_location = QStandardPaths.writableLocation(
        QStandardPaths.StandardLocation.AppDataLocation
    )
    if not data_location:
        raise RuntimeError("운영체제 사용자 데이터 경로를 찾지 못했습니다.")
    return Path(data_location) / "pynote.sqlite3"


def _activate_window(window: MainWindow) -> None:
    """두 번째 실행 요청을 받으면 기존 창을 현재 데스크톱 앞으로 가져온다."""
    if window.isMinimized():
        window.showNormal()
    else:
        window.show()
    window.raise_()
    window.activateWindow()


def main(arguments: Sequence[str] | None = None) -> int:
    """pyNote의 Qt 이벤트 루프를 시작한다."""
    raw_arguments = list(sys.argv[1:] if arguments is None else arguments)
    options = _parse_arguments(raw_arguments)
    application = QApplication([sys.argv[0], *raw_arguments])
    application.setOrganizationName("pyNote")
    application.setApplicationName("pyNote")

    temporary_directory: tempfile.TemporaryDirectory[str] | None = None
    if options.database is not None:
        database_path = options.database
    elif options.smoke:
        temporary_directory = tempfile.TemporaryDirectory(prefix="pynote-smoke-")
        database_path = Path(temporary_directory.name) / "pynote.sqlite3"
    else:
        database_path = _default_database_path()

    instance_guard = SingleInstanceGuard(database_path.parent)
    try:
        if not options.smoke and not instance_guard.acquire():
            return 0
        settings = (
            QSettings(
                str(Path(temporary_directory.name) / "settings.ini"),
                QSettings.Format.IniFormat,
            )
            if temporary_directory is not None
            else None
        )
        context = AppContext.open(database_path, settings)
        quit_coordinator: ApplicationQuitCoordinator | None = None
        try:
            manager = WindowManager(context)
            windows = manager.restore_windows()
            instance_guard.new_window_requested.connect(manager.create_window)
            quit_coordinator = ApplicationQuitCoordinator(application, manager)
            application.aboutToQuit.connect(manager.prepare_shutdown)
            for window in windows:
                window.show()
            if options.smoke:
                QTimer.singleShot(0, application.quit)
            return application.exec()
        finally:
            if quit_coordinator is not None:
                quit_coordinator.uninstall()
            context.database.close()
    finally:
        instance_guard.close()
        if temporary_directory is not None:
            temporary_directory.cleanup()
