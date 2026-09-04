from __future__ import annotations

import logging
import sys
import time
from collections.abc import Callable
from datetime import datetime
from pathlib import Path

from PySide6.QtCore import QModelIndex, QSettings, Qt, QTimer, Signal
from PySide6.QtGui import QCloseEvent, QResizeEvent
from PySide6.QtWidgets import (
    QComboBox,
    QDialog,
    QFileDialog,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QMessageBox,
    QPushButton,
    QStackedWidget,
    QVBoxLayout,
    QWidget,
)
from shiboken6 import isValid

from pynote.application.card_service import CardService
from pynote.application.draft_coordinator import (
    DraftCoordinator,
    DraftDisposition,
)
from pynote.application.file_binding_service import (
    BindingPathStatus,
    DetectedText,
    FileSyncOutcome,
    PendingFileBinding,
    detect_text,
    hash_bytes,
    prepare_binding_path,
    resolve_path,
    sync_file,
)
from pynote.application.history_service import HistoryService
from pynote.application.purge_service import PurgeService
from pynote.application.save_coordinator import SaveCoordinator
from pynote.domain.models import (
    CaptureOperationSource,
    Card,
    FileBinding,
    NewlineKind,
)
from pynote.domain.paragraph_parser import ParagraphParser
from pynote.infrastructure.database import Database
from pynote.infrastructure.export import NewlineFormat, export_cards
from pynote.infrastructure.repositories import CardCompareAndSwapError, Repositories
from pynote.infrastructure.settings import DataPolicySettingsStore
from pynote.ui.cards.card_model import CardRole
from pynote.ui.cards.card_stream import CardDeleteDropZone, CardStreamView
from pynote.ui.editor.card_editor import (
    CardEditor,
    CardEditorWorkspace,
    CloseChoiceProvider,
)
from pynote.ui.import_dialog import (
    MAX_IMPORT_FILE_BYTES,
    prepare_import_from_bytes,
)
from pynote.ui.panels.history_view import HistoryView

LOGGER = logging.getLogger(__name__)

_DROP_MAX_FILES = 20
_DROP_MAX_TOTAL_BYTES = 4 * 1024 * 1024


def _now_us() -> int:
    return time.time_ns() // 1_000


class DocumentPage(QWidget):
    """문서 한 탭의 카드 입력·목록·편집·이력·휴지통을 통합한다."""

    content_changed = Signal()
    card_opened = Signal(str)
    binding_changed = Signal()

    def __init__(
        self,
        database: Database,
        repositories: Repositories,
        document_id: str,
        *,
        settings: QSettings | None = None,
        policy_store: DataPolicySettingsStore | None = None,
        destructive_preflight: Callable[[str], bool] | None = None,
        error_reporter: Callable[[str, str], None] | None = None,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.document_id = document_id
        self._database = database
        self._repositories = repositories
        self._settings = settings
        self._policy_store = policy_store or DataPolicySettingsStore(database)
        self._destructive_preflight = destructive_preflight
        self._error_reporter = error_reporter or self._show_error
        self._deferred_open_errors: list[str] | None = None
        self._paragraph_parser = ParagraphParser()
        self._pending_binding: PendingFileBinding | None = None

        policy = self._policy_store.load()
        idle_seconds = policy.draft_idle_ms / 1_000
        preview_lines = policy.preview_lines
        self.card_service = CardService(database, repositories)
        self.purge_service = PurgeService(database, repositories)
        self.draft_coordinator = DraftCoordinator(
            database,
            repositories,
            idle_ms=max(0, round(idle_seconds * 1_000)),
            parent=self,
        )
        save_coordinator = SaveCoordinator(
            database,
            self.draft_coordinator,
            repositories,
        )
        self.editor = CardEditor(
            repositories,
            self.draft_coordinator,
            save_coordinator,
            document_id=document_id,
            card_service=self.card_service,
        )
        self.stream = CardStreamView()
        self.stream.card_model.set_preview_line_count(preview_lines)
        self.history = HistoryView(
            HistoryService(database, repositories),
            destructive_preflight=self._can_run_destructive_command,
        )
        self.history.set_document(document_id)

        self.list_pane = QWidget(self)
        card_layout = QVBoxLayout(self.list_pane)
        card_layout.setContentsMargins(0, 0, 0, 0)

        primary_controls = QHBoxLayout()
        secondary_controls = QHBoxLayout()
        self.sort_combo = QComboBox(self.list_pane)
        self.sort_combo.setObjectName("cardSortCombo")
        self.sort_combo.setToolTip(
            "카드를 최근 활동, 현재 문서 순서 또는 최초 기록 순서로 정렬"
        )
        self.sort_combo.addItem("최근 활동순", "recency")
        self.sort_combo.addItem("현재 문서 순서", "position")
        self.sort_combo.addItem("최초 기록 순서", "capture")
        self.source_filter = QComboBox(self.list_pane)
        self.source_filter.setObjectName("cardSourceFilter")
        self.source_filter.setToolTip("선택한 입력 출처의 카드만 표시")
        self.source_filter.addItem("모든 출처", None)
        for label, value in (
            ("직접 입력", "typing"),
            ("붙여넣기", "paste"),
            ("혼합", "mixed"),
            ("가져오기", "import"),
            ("복구", "restore"),
        ):
            self.source_filter.addItem(label, value)
        self.trash_button = QPushButton("카드 휴지통", self.list_pane)
        self.trash_button.setObjectName("cardTrashButton")
        self.trash_button.setToolTip("삭제한 카드를 확인하고 복구하거나 완전 삭제")
        primary_controls.addWidget(self.sort_combo, 1)
        primary_controls.addWidget(self.source_filter, 1)
        secondary_controls.addWidget(self.trash_button)
        card_layout.addLayout(primary_controls)
        card_layout.addLayout(secondary_controls)
        card_layout.addWidget(self.stream, 1)
        self.delete_drop_zone = CardDeleteDropZone(
            self.stream,
            parent=self.list_pane,
        )
        self.delete_drop_zone.raise_()

        self.editor_workspace = CardEditorWorkspace(
            self.list_pane,
            self.editor,
        )
        self.mode_stack = QStackedWidget(self)
        self.mode_stack.setObjectName("documentModeStack")
        self.mode_stack.addWidget(self.editor_workspace)
        self.mode_stack.addWidget(self.history)
        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.addWidget(self.mode_stack)

        self.stream.card_open_requested.connect(self._open_card)
        self.stream.card_browse_requested.connect(self._browse_card)
        self.stream.card_move_requested.connect(self._move_card)
        self.stream.cards_delete_requested.connect(self._delete_cards)
        self.stream.cards_export_requested.connect(self._export_cards)
        self.stream.drag_started.connect(self._show_delete_drop_zone)
        self.stream.drag_finished.connect(self._hide_delete_drop_zone)
        self.delete_drop_zone.card_delete_dropped.connect(
            self.stream.card_delete_dropped.emit
        )
        self.stream.card_delete_dropped.connect(self._delete_dragged_card)
        self.stream.destroyed.connect(self._source_destroyed)
        self.stream.set_drag_body_provider(self._body_for_drag)
        self.stream.empty_area_clicked.connect(self._close_editor_on_empty_click)
        self.stream.selectionModel().currentChanged.connect(self._selection_changed)
        self.sort_combo.currentIndexChanged.connect(self._sort_changed)
        self.source_filter.currentIndexChanged.connect(self._filter_changed)
        self.trash_button.clicked.connect(self.open_card_trash)
        self.editor_workspace.card_committed.connect(self._card_committed)
        self.editor.card_created.connect(self._card_created)
        self.editor.card_connected.connect(self._card_connected)
        self.editor.card_deleted.connect(self._card_deleted)
        self.editor.files_dropped.connect(self._handle_files_dropped)
        self.editor.draft_dirty_changed.connect(
            self.stream.card_model.set_card_dirty
        )
        self.editor_workspace.save_conflict.connect(self.history.show_save_conflict)
        self.history.card_restored.connect(self._card_restored)
        self.apply_settings()
        self.refresh()

    def resizeEvent(self, event: QResizeEvent) -> None:
        """목록 패널 하단 중앙의 delete pill을 56px 높이로 맞춘다."""
        super().resizeEvent(event)
        self._position_delete_drop_zone()

    def closeEvent(self, event: QCloseEvent) -> None:
        """창 닫기 중에는 활성 drag overlay와 token을 수명 안전하게 해제한다."""
        self._hide_delete_drop_zone()
        super().closeEvent(event)

    def refresh(self) -> None:
        """DB의 카드·리비전·purge 복원 상태를 스트림과 이력에 반영한다."""
        cards = self._repositories.list_cards(self.document_id)
        active_cards = tuple(card for card in cards if card.deleted_at_us is None)
        revision_counts = {
            card.id: len(self._repositories.list_revisions(card.id))
            for card in active_cards
        }
        self.stream.card_model.set_cards(
            active_cards,
            revision_counts=revision_counts,
        )
        self.stream.card_model.set_dirty_draft_ids(
            frozenset(
                draft.card_id
                for draft in self._repositories.list_drafts(self.document_id)
                if draft.card_id is not None
            )
        )
        unavailable = frozenset(
            card.id
            for card in active_cards
            if not self._repositories.operation_reconstruction_available(card.id)
        )
        self.stream.card_model.set_reconstruction_unavailable_ids(unavailable)
        self.history.refresh()

    def show_cards(self) -> None:
        """카드 작업 화면으로 전환한다."""
        self.mode_stack.setCurrentWidget(self.editor_workspace)

    def show_history(self) -> None:
        """문서 이력 화면으로 전환한다."""
        self.history.refresh()
        self.mode_stack.setCurrentWidget(self.history)

    def focus_card_list(self) -> None:
        """카드 화면으로 돌아가 목록에 키보드 포커스를 둔다."""
        self.show_cards()
        self.stream.setFocus()

    def focus_editor(self) -> None:
        """카드 화면으로 돌아가 단일 편집면에 키보드 포커스를 둔다."""
        self.show_cards()
        self._focus_editor_slot()
        if not self.isVisible():
            QTimer.singleShot(0, self._focus_editor_slot)

    def _focus_editor_slot(self) -> None:
        self.editor.setFocus()

    def _close_editor_on_empty_click(self) -> None:
        if self.editor.session is None:
            # 빈 편집면에서는 닫기 게이트를 돌리지 않는다. 다만 목록 press가
            # 가져간 포커스는 편집면으로 되돌린다 — 그러지 않으면 이어지는
            # 붙여넣기가 목록으로 들어가 조용히 사라진다.
            self._focus_editor_slot()
            QTimer.singleShot(0, self, self._focus_editor_slot)
            return
        self.editor.request_close()
        self._focus_editor_slot()
        # 저장 실패 모달은 닫히며 진입 전 목록 포커스를 복원하므로 한 틱 뒤
        # 연결 성공/거부와 무관하게 단일 편집면 포커스를 다시 확정한다.
        QTimer.singleShot(0, self, self._focus_editor_slot)

    def open_card(self, card_id: str, *, app_driven: bool = False) -> bool:
        """검색 결과 등 외부 요청으로 카드를 선택하고 편집기를 연다."""
        card = self._repositories.get_card(card_id)
        if card is None or card.document_id != self.document_id:
            return False
        self.show_cards()
        self.reveal_card(card_id)
        return self._open_card(card_id, app_driven=app_driven)

    @property
    def pending_binding(self) -> PendingFileBinding | None:
        """카드가 생기면 부착할 결속 대기를 반환한다."""
        return self._pending_binding

    def open_file(self, path: Path) -> bool:
        """파일을 카드 한 장에 결속해 연다 — 같은 문서 안의 새 결속만 만든다(2-8).

        이미 결속된 경로의 조회와 교차 문서 라우팅은 액션 소유자인 MainWindow가 한다.
        """
        try:
            with path.open("rb") as source:
                data = source.read(MAX_IMPORT_FILE_BYTES + 1)
        except OSError as error:
            self._error_reporter("파일 열기 실패", f"{path.name}: {error}")
            return False
        if len(data) > MAX_IMPORT_FILE_BYTES:
            self._error_reporter(
                "파일 열기 실패",
                f"{path.name}: 파일당 4 MiB 상한을 초과했습니다.",
            )
            return False

        detected = detect_text(data)
        if detected is None:
            return self._import_rejected_file(path, data)

        # 점유 판정을 이탈 게이트보다 먼저 한다 — 거절될 요청 때문에 현재 세션을 정리하지
        # 않는다(2-8 3·4).
        resolved, path_key = resolve_path(path)
        resolution = prepare_binding_path(self._repositories, path_key)
        if resolution.status is BindingPathStatus.HELD_BY_ACTIVE_CARD:
            self._error_reporter(
                "파일 열기 실패",
                f"{path.name}: 이미 다른 카드에 결속된 파일입니다.",
            )
            return False
        if not self.can_leave_editor(protect_now=True):
            return False

        if self._paragraph_parser.is_zero_paragraph_input(detected.text):
            return self._start_pending_binding(resolved, detected, has_bytes=bool(data))

        card = self._create_card_from_file(path, detected.text)
        if card is None:
            return False
        self._repositories.upsert_file_binding(
            self._binding_for_opened_file(card.id, resolved, path_key, detected, data)
        )
        if not self.open_card(card.id):
            return False
        self.binding_changed.emit()
        return True

    def save_card_as(self) -> bool:
        """편집 중인 카드를 새 경로에 결속하고 즉시 기록한다(2-7)."""
        if self.editor.card_id is None:
            # 카드가 아직 없는 새 입력 세션이면 먼저 확정해 카드를 만든다.
            self.editor.save_current(interactive=True)
        card_id = self.editor.card_id
        if card_id is None:
            self._error_reporter("다른 이름으로 저장", "저장할 카드가 없습니다.")
            return False
        current = self._repositories.get_file_binding(card_id)
        filename, _selected_filter = QFileDialog.getSaveFileName(
            self,
            "다른 이름으로 저장",
            "" if current is None else Path(current.path).name,
            "모든 파일 (*);;텍스트 (*.txt);;Markdown (*.md);;JSON (*.json)",
        )
        if not filename:
            return False
        # 확장자를 강제하지 않는다 — 내보내기의 _require_text_suffix 와 다른 계약이다.
        resolved, path_key = resolve_path(Path(filename))
        holder = self._repositories.find_active_binding_by_path(path_key)
        if holder is not None and holder.card_id != card_id:
            self._error_reporter(
                "다른 이름으로 저장 실패",
                f"{Path(resolved).name}: 이미 다른 카드에 결속된 파일입니다.",
            )
            return False
        prepare_binding_path(self._repositories, path_key)
        self._repositories.upsert_file_binding(
            FileBinding(
                card_id=card_id,
                path=resolved,
                path_key=path_key,
                encoding="utf-8" if current is None else current.encoding,
                bom=False if current is None else current.bom,
                newline=self._default_newline() if current is None else current.newline,
                trailing_newline=(
                    False if current is None else current.trailing_newline
                ),
                bound_at_us=_now_us(),
            )
        )
        # 결속을 먼저 옮긴 뒤에 확정한다 — 그래야 편집 중인 본문이 이전 파일이 아니라
        # 새 경로에만 기록된다(S5 의 "이전 파일 불변").
        if not self.editor.save_current(interactive=True):
            # 확정 실패(IME 조합 중·리비전 충돌·예외)면 아무것도 바뀌지 않은 상태로 되돌린다 —
            # 확정 전 본문을 새 파일에 굳히지 않고 결속도 이전 것으로 복원한다.
            if current is None:
                self._repositories.delete_file_binding(card_id)
            else:
                self._repositories.upsert_file_binding(current)
            self.binding_changed.emit()
            self._error_reporter(
                "다른 이름으로 저장 실패",
                f"{Path(resolved).name}: 카드를 확정하지 못해 기록하지 않았습니다.",
            )
            return False
        card = self._repositories.get_card(card_id)
        if card is None:
            self._error_reporter("다른 이름으로 저장 실패", "카드를 찾지 못했습니다.")
            return False
        # QFileDialog 가 덮어쓰기를 이미 확인했으므로 외부 변경 질의를 겹치지 않는다.
        result = sync_file(
            self._repositories,
            card,
            force=True,
            interactive=True,
        )
        self.binding_changed.emit()
        if result.outcome is FileSyncOutcome.FAILED:
            self._error_reporter(
                "다른 이름으로 저장 실패",
                f"{Path(resolved).name}: {result.error}",
            )
            return False
        return True

    def unbind_file(self) -> bool:
        """결속 행만 지운다 — 카드와 파일은 건드리지 않는다(S10)."""
        card_id = self.editor.card_id
        if card_id is not None and self._repositories.get_file_binding(card_id):
            self._repositories.delete_file_binding(card_id)
            self._pending_binding = None
            self.binding_changed.emit()
            return True
        if self._pending_binding is not None:
            self._pending_binding = None
            return True
        return False

    def reveal_card(self, card_id: str) -> bool:
        """카드 행을 선택하고 화면에 보이게 하되 편집면에는 연결하지 않는다."""
        index = self.stream.card_model.index_for_card(card_id)
        if not index.isValid():
            # 필터로 가려진 행은 필터를 풀지 않고 기존 선택을 그대로 둔다.
            return False
        # 선택만 옮기는 이 계약이 대기 중인 휠 탐색보다 최신 의도다 — 놔두면
        # 곧이어 만료된 탐색이 이 카드를 편집면에 연결한다.
        self.stream.cancel_pending_browse()
        self.stream.setCurrentIndex(index)
        self.stream.scrollTo(index)
        return True

    def reveal_created_card(self, card_id: str) -> bool:
        """생성 카드를 카드 화면으로 돌아와 선택·표시한다."""
        if not self.stream.card_model.index_for_card(card_id).isValid():
            return False
        # 이력 화면에서 가져오기를 실행했으면 카드 화면으로 돌아와야 실제로
        # 보인다. 편집 세션은 건드리지 않는다.
        self.show_cards()
        return self.reveal_card(card_id)

    def view_state(self) -> tuple[str | None, int]:
        """모델 reset 전에 되돌릴 선택 카드와 세로 스크롤 위치를 캡처한다."""
        card_id = self.stream.currentIndex().data(CardRole.CARD_ID)
        return (
            card_id if isinstance(card_id, str) else None,
            self.stream.verticalScrollBar().value(),
        )

    def restore_view(self, state: tuple[str | None, int]) -> None:
        """캡처한 선택과 스크롤 위치를 되돌린다."""
        card_id, scroll_value = state
        if card_id is not None:
            self.reveal_card(card_id)
        # reveal_card의 scrollTo가 위치를 바꾸므로 스크롤 복원이 뒤에 온다.
        self.stream.verticalScrollBar().setValue(scroll_value)

    def can_leave_editor(
        self,
        *,
        choice_provider: CloseChoiceProvider | None = None,
        protect_now: bool = False,
    ) -> bool:
        """문서·탭·앱 전환이 공유하는 단일 편집면 이탈 게이트를 실행한다."""
        left = self.editor.can_leave_editor(
            choice_provider=choice_provider,
            protect_now=protect_now,
        )
        if left:
            # 실제로 떠날 때만 결속 대기를 폐기한다 — 계속 편집을 고르면 대기는 유지되고
            # 파일은 불변으로 남는다(2-8 5).
            self._pending_binding = None
        return left

    def protect_now(self) -> bool:
        """단일 편집면의 최신 dirty draft를 즉시 보호한다."""
        return self.editor.protect_now()

    def detach_editor_session_quietly(self) -> bool:
        """앱 주도 문서 전환 전에 카드 편집 세션을 확정 없이 분리한다."""
        return self.editor.detach_session_quietly()

    def cleanup_empty_card_before_exit(self) -> None:
        """종료 직전 편집기의 clean 빈 확정 카드 정리를 위임한다."""
        self.editor.cleanup_empty_card_before_exit()

    def set_recovery_disposition(
        self,
        card_id: str,
        disposition: DraftDisposition,
    ) -> None:
        """시작 UI에서 확정된 카드 recovery 처분을 첫 편집 진입에 전달한다."""
        self.editor.set_recovery_disposition(card_id, disposition)

    def open_card_trash(self) -> None:
        """삭제 카드를 조회하고 복구 또는 이중 확인 purge를 제공한다."""
        dialog = QDialog(self)
        dialog.setObjectName("cardTrashDialog")
        dialog.setWindowTitle("카드 휴지통")
        listing = QListWidget(dialog)
        listing.setObjectName("trashedCardList")
        for card in self._repositories.list_cards(self.document_id):
            if card.deleted_at_us is None:
                continue
            item = QListWidgetItem(f"기록 #{card.capture_seq} · {card.body[:120]}")
            item.setData(Qt.ItemDataRole.UserRole, card.id)
            listing.addItem(item)
        restore = QPushButton("복구", dialog)
        purge = QPushButton("완전 삭제…", dialog)
        close = QPushButton("닫기", dialog)
        buttons = QHBoxLayout()
        buttons.addWidget(restore)
        buttons.addWidget(purge)
        buttons.addStretch(1)
        buttons.addWidget(close)
        layout = QVBoxLayout(dialog)
        layout.addWidget(
            QLabel("완전 삭제는 보존 기간이 지난 카드만 가능하며 되돌릴 수 없습니다.", dialog)
        )
        layout.addWidget(listing)
        layout.addLayout(buttons)
        restore.clicked.connect(lambda: self._restore_trash_item(listing, dialog))
        purge.clicked.connect(lambda: self._purge_trash_item(listing, dialog))
        close.clicked.connect(dialog.accept)
        dialog.resize(560, 380)
        dialog.exec()

    def apply_settings(self) -> None:
        """설정 화면에서 바뀐 미리보기와 편집기 글꼴을 현재 페이지에 적용한다."""
        self.stream.card_model.set_preview_line_count(
            self._policy_store.load().preview_lines
        )
        self.stream.set_multi_selection_enabled(
            self._settings is not None
            and bool(
                self._settings.value(
                    "cards/multi_selection_enabled",
                    False,
                    type=bool,
                )
            )
        )
        self.draft_coordinator.set_idle_ms(
            self._policy_store.load().draft_idle_ms
        )
        if self._settings is not None:
            from PySide6.QtGui import QFont

            family = str(self._settings.value("editor/font_family", ""))
            size = self._setting_int("editor/font_size", 11)
            font = QFont(family) if family else self.editor.font()
            font.setPointSize(size)
            self.editor.apply_editor_font(font)
            self.editor.apply_line_spacing(
                self._setting_float("editor/line_spacing", 1.0)
            )
            self.stream.apply_time_display(
                str(
                    self._settings.value(
                        "display/time_format",
                        "yyyy-MM-dd HH:mm:ss",
                    )
                ),
                str(self._settings.value("display/timezone", "system")),
            )

    def _open_card(self, card_id: str, *, app_driven: bool = False) -> bool:
        try:
            opened = self.editor_workspace.open_card(
                card_id,
                app_driven=app_driven,
            )
        except BaseException as error:
            LOGGER.exception("문서 페이지에서 카드 편집기를 열지 못했습니다.")
            if self._deferred_open_errors is None:
                self._error_reporter("카드 열기 실패", str(error))
            else:
                self._deferred_open_errors.append(str(error))
            return False
        return opened

    def _browse_card(self, card_id: str) -> None:
        """휠로 옮긴 카드를 편집면에 띄우되 키보드 포커스는 목록에 남긴다."""
        if self._open_card(card_id):
            # 열기 성공 경로가 편집면으로 포커스를 가져가므로 되돌린다 — 그러지
            # 않으면 이어지는 방향키 탐색이 편집기 커서 이동으로 새어 나간다.
            self.stream.setFocus()
            return
        # IME 조합 중이거나 저장이 거부되면 편집면은 이전 카드를 그대로 들고
        # 있다. 선택만 앞서가면 사용자가 보는 본문과 Delete 대상이 어긋나므로
        # 선택을 편집면 쪽으로 되돌리고 포커스도 넘기지 않는다.
        opened_card_id = self.editor.card_id
        if opened_card_id is not None:
            self.reveal_card(opened_card_id)
        self._focus_editor_slot()

    def _handle_files_dropped(self, value: object) -> None:
        paths, validation_errors = self._validated_drop_paths(value)
        if validation_errors:
            self._report_drop_errors(validation_errors, ())
            return
        if not paths:
            return

        preparations: list[tuple[Path, str]] = []
        read_errors: list[str] = []
        total_limit_error: str | None = None
        total_bytes = 0
        for path in paths:
            remaining_bytes = _DROP_MAX_TOTAL_BYTES - total_bytes
            try:
                with path.open("rb") as source:
                    data = source.read(MAX_IMPORT_FILE_BYTES + 1)
            except OSError as error:
                read_errors.append(f"{path.name}: {error}")
                continue
            if len(data) > MAX_IMPORT_FILE_BYTES:
                read_errors.append(
                    f"{path.name}: 파일당 4 MiB 상한을 초과했습니다."
                )
                continue
            if len(data) > remaining_bytes:
                total_limit_error = (
                    f"{path.name}: 드롭 전체 4 MiB 상한을 초과했습니다."
                )
                break
            total_bytes += len(data)
            try:
                preparation = prepare_import_from_bytes(
                    path,
                    data,
                )
            except ValueError as error:
                read_errors.append(f"{path.name}: {error}")
                continue
            preparations.append((path, preparation.text))

        if total_limit_error is not None:
            self._report_drop_errors((*read_errors, total_limit_error), ())
            return
        if not preparations:
            self._report_drop_errors(read_errors, ())
            return
        if not self.can_leave_editor(protect_now=True):
            if read_errors:
                self._report_drop_errors(read_errors, ())
            return

        cards: list[Card] = []
        creation_errors: list[str] = []
        for path, body in preparations:
            try:
                created = self.card_service.create_cards(
                    self.document_id,
                    body,
                    source=CaptureOperationSource.IMPORT,
                    split=False,
                )
            except BaseException as error:
                LOGGER.exception("드롭 파일로 카드를 만들지 못했습니다: %s", path)
                creation_errors.append(f"{path.name}: {error}")
                continue
            if len(created) != 1:
                creation_errors.append(
                    f"{path.name}: 카드가 정확히 한 장 생성되지 않았습니다."
                )
                continue
            cards.append(created[0])

        if cards:
            self.stream.card_model.add_cards(
                cards,
                revision_counts={card.id: 1 for card in cards},
            )
            self.history.refresh()
            self.content_changed.emit()

        connection_errors: list[str] = []
        if len(cards) == 1:
            previous_errors = self._deferred_open_errors
            self._deferred_open_errors = connection_errors
            try:
                try:
                    opened = self.open_card(cards[0].id)
                except BaseException as error:
                    LOGGER.exception("드롭으로 만든 카드를 연결하지 못했습니다.")
                    connection_errors.append(str(error))
                else:
                    if not opened and not connection_errors:
                        connection_errors.append("생성한 카드를 편집면에 연결하지 못했습니다.")
            finally:
                self._deferred_open_errors = previous_errors
        elif len(cards) > 1:
            # 여러 장을 만든 드롭은 편집면에 연결하지 않는 계약이라 선택·표시만
            # 바꾼다. 대상은 행 위치가 아니라 마지막 성공 카드의 ID로 잡는다.
            self.reveal_card(cards[-1].id)

        if read_errors or creation_errors or connection_errors:
            self._report_drop_errors(
                read_errors,
                (*creation_errors, *connection_errors),
            )

    def _validated_drop_paths(
        self,
        value: object,
    ) -> tuple[tuple[Path, ...], tuple[str, ...]]:
        if not isinstance(value, tuple) or not all(
            isinstance(path, Path) for path in value
        ):
            return (), ("드롭된 파일 경로 형식이 올바르지 않습니다.",)
        if not value:
            return (), ("드롭된 파일이 없습니다.",)
        if len(value) > _DROP_MAX_FILES:
            return (), (f"한 번에 {_DROP_MAX_FILES}개 파일까지만 드롭할 수 있습니다.",)

        structural_errors: list[str] = []
        for path in value:
            if not path.exists():
                structural_errors.append(f"{path.name}: 파일이 존재하지 않습니다.")
            elif path.is_dir():
                structural_errors.append(f"{path.name}: 디렉터리는 가져올 수 없습니다.")
            elif not path.is_file():
                structural_errors.append(f"{path.name}: 일반 파일이 아닙니다.")
        if structural_errors:
            return (), tuple(structural_errors)

        unique_paths: list[Path] = []
        seen: set[Path] = set()
        for path in value:
            if path in seen:
                continue
            seen.add(path)
            unique_paths.append(path)
        return tuple(unique_paths), ()

    def _report_drop_errors(
        self,
        read_errors: tuple[str, ...] | list[str],
        creation_errors: tuple[str, ...] | list[str],
    ) -> None:
        sections: list[str] = []
        if read_errors:
            sections.append("판독 실패:\n" + "\n".join(f"- {item}" for item in read_errors))
        if creation_errors:
            sections.append(
                "생성/연결 실패:\n"
                + "\n".join(f"- {item}" for item in creation_errors)
            )
        self._error_reporter("파일 드롭 실패", "\n\n".join(sections))

    def _import_rejected_file(self, path: Path, data: bytes) -> bool:
        """결속 불가 파일은 사본 가져오기만 허용한다 — 파일 바이트는 건드리지 않는다."""
        if not self._ask_copy_import(path):
            return False
        try:
            preparation = prepare_import_from_bytes(path, data)
        except ValueError as error:
            self._error_reporter("파일 열기 실패", f"{path.name}: {error}")
            return False
        if not self.can_leave_editor(protect_now=True):
            return False
        card = self._create_card_from_file(path, preparation.text)
        return card is not None and self.open_card(card.id)

    def _ask_copy_import(self, path: Path) -> bool:
        box = QMessageBox(self)
        box.setIcon(QMessageBox.Icon.Warning)
        box.setWindowTitle("결속할 수 없는 파일")
        box.setText(
            f"{path.name} 파일은 텍스트로 해석되지 않아 편집 결과를 되쓸 수 없습니다."
        )
        box.setInformativeText("사본으로만 가져오시겠습니까?")
        copy_button = box.addButton("사본 가져오기", QMessageBox.ButtonRole.AcceptRole)
        cancel_button = box.addButton("취소", QMessageBox.ButtonRole.RejectRole)
        box.setDefaultButton(cancel_button)
        box.exec()
        return box.clickedButton() is copy_button

    def _start_pending_binding(
        self,
        resolved: str,
        detected: DetectedText,
        *,
        has_bytes: bool,
    ) -> bool:
        """문단이 0개인 파일은 빈 입력기 + 결속 대기로 연다(2-8 5)."""
        # 열려 있던 카드는 이탈 게이트를 거쳐 닫아야 빈 입력기가 드러난다.
        if self.editor.card_id is not None and not self.editor.request_close():
            return False
        self._pending_binding = PendingFileBinding(
            path=resolved,
            encoding=detected.encoding,
            bom=detected.bom,
            newline=detected.newline,
            trailing_newline=detected.trailing_newline,
        )
        self.focus_editor()
        if has_bytes:
            self.editor_workspace.show_notice(
                "원본 공백 내용은 첫 저장 때 대체됩니다"
            )
        return True

    def _create_card_from_file(self, path: Path, text: str) -> Card | None:
        try:
            created = self.card_service.create_cards(
                self.document_id,
                text,
                source=CaptureOperationSource.IMPORT,
                split=False,
            )
        except BaseException as error:
            LOGGER.exception("연 파일로 카드를 만들지 못했습니다: %s", path)
            self._error_reporter("파일 열기 실패", f"{path.name}: {error}")
            return None
        if len(created) != 1:
            self._error_reporter(
                "파일 열기 실패",
                f"{path.name}: 카드가 정확히 한 장 생성되지 않았습니다.",
            )
            return None
        card = created[0]
        self.stream.card_model.add_cards((card,), revision_counts={card.id: 1})
        self.history.refresh()
        self.content_changed.emit()
        return card

    def _binding_for_opened_file(
        self,
        card_id: str,
        resolved: str,
        path_key: str,
        detected: DetectedText,
        data: bytes,
    ) -> FileBinding:
        try:
            mtime_ns = Path(resolved).stat().st_mtime_ns
        except OSError:
            LOGGER.warning("결속 파일 상태를 읽지 못했습니다: %s", resolved)
            mtime_ns = None
        now_us = _now_us()
        return FileBinding(
            card_id=card_id,
            path=resolved,
            path_key=path_key,
            encoding=detected.encoding,
            bom=detected.bom,
            newline=detected.newline,
            trailing_newline=detected.trailing_newline,
            bound_at_us=now_us,
            synced_size=len(data),
            synced_mtime_ns=mtime_ns,
            synced_hash=hash_bytes(data),
            synced_at_us=now_us,
        )

    def _promote_pending_binding(self, card: Card) -> None:
        """첫 카드가 생기면 결속 대기를 부착한다 — 파일은 아직 쓰지 않는다(2-8)."""
        pending = self._pending_binding
        if pending is None:
            return
        # 대기를 먼저 비워 같은 카드에 결속이 두 번 만들어지지 않게 한다.
        self._pending_binding = None
        self._repositories.upsert_file_binding(
            FileBinding(
                card_id=card.id,
                path=pending.path,
                path_key=pending.path_key,
                encoding=pending.encoding,
                bom=pending.bom,
                newline=pending.newline,
                trailing_newline=pending.trailing_newline,
                bound_at_us=_now_us(),
            )
        )
        self.binding_changed.emit()

    @staticmethod
    def _default_newline() -> NewlineKind:
        return NewlineKind.CRLF if sys.platform == "win32" else NewlineKind.LF

    def _move_card(self, card_id: str, before_card_id: object) -> None:
        if not self._can_run_destructive_command():
            return
        try:
            self.card_service.move_card(
                card_id,
                before_card_id=before_card_id if isinstance(before_card_id, str) else None,
            )
        except BaseException as error:
            LOGGER.exception("문서 페이지에서 카드 이동에 실패했습니다.")
            self._error_reporter("카드 이동 실패", str(error))
            return
        self.refresh()
        self.content_changed.emit()

    def _show_delete_drop_zone(self, _card_id: str, token: int) -> None:
        zone = self.delete_drop_zone
        if not isValid(zone):
            return
        zone.arm(token)
        self._position_delete_drop_zone()
        zone.raise_()
        zone.show()

    def _hide_delete_drop_zone(self, _token: int | None = None) -> None:
        zone = self.delete_drop_zone
        if not isValid(zone):
            return
        zone.disarm()
        zone.hide()

    def _source_destroyed(self, _source: object | None = None) -> None:
        self._hide_delete_drop_zone()

    def _position_delete_drop_zone(self) -> None:
        zone = self.delete_drop_zone
        if not isValid(zone) or not isValid(self.list_pane):
            return
        panel_width = self.list_pane.width()
        # 좌우 여백은 목록 하단 좌우와 autoscroll edge 를 열어 두기 위한 것이다 —
        # 전폭 바로 만들면 마지막 행 드롭과 스크롤이 막힌다.
        width = max(0, min(280, panel_width - 96))
        height = 56
        zone.setGeometry(
            max(0, (panel_width - width) // 2),
            max(0, self.list_pane.height() - height - 16),
            width,
            height,
        )

    def _body_for_drag(self, card_id: str) -> str:
        if self.editor.card_id == card_id:
            return self.editor.toPlainText()
        index = self.stream.card_model.index_for_card(card_id)
        if index.isValid():
            return str(index.data(CardRole.BODY))
        card = self._repositories.get_card(card_id)
        return "" if card is None else card.body

    def _delete_dragged_card(self, card_id: str) -> None:
        expected_revision_id = self.stream.active_drag_revision(card_id)
        if expected_revision_id is None:
            self._error_reporter(
                "카드 삭제 실패",
                "드래그 시작 시점의 카드 리비전을 확인할 수 없습니다.",
            )
            return
        session = self.editor.session
        connected = session is not None and session.card_id == card_id
        discard_draft_id = session.draft_id if connected and session is not None else None

        if not connected:
            if not self.protect_now():
                self._error_reporter(
                    "카드 삭제 중단",
                    "현재 편집 중인 초안을 보호하지 못해 카드를 삭제하지 않았습니다.",
                )
                return
        elif session is not None and session.dirty:
            choice = self._ask_drag_delete_choice()
            if choice == "cancel":
                return
            if choice == "save":
                if not self.editor.save_current():
                    self._error_reporter(
                        "카드 삭제 중단",
                        "변경 내용을 저장하지 못해 카드를 삭제하지 않았습니다.",
                    )
                    return
                saved = self._repositories.get_card(card_id)
                if saved is None or saved.deleted_at_us is not None:
                    self._error_reporter(
                        "카드 삭제 중단",
                        "저장 뒤 카드 상태를 다시 확인하지 못했습니다.",
                    )
                    return
                expected_revision_id = saved.current_revision_id
        try:
            self.card_service.soft_delete(
                card_id,
                expected_revision_id=expected_revision_id,
                discard_draft_id=discard_draft_id,
            )
        except CardCompareAndSwapError as error:
            self._error_reporter(
                "카드 삭제 거부",
                f"다른 창에서 카드가 변경되어 삭제하지 않았습니다.\n{error}",
            )
            return
        except BaseException as error:
            LOGGER.exception("드래그한 카드 삭제에 실패했습니다.")
            self._error_reporter("카드 삭제 실패", str(error))
            return

        if connected:
            self.editor.discard_session_for_deleted_card()
        self.refresh()
        self.content_changed.emit()

    def _ask_drag_delete_choice(self) -> str:
        dialog = QMessageBox(self)
        dialog.setIcon(QMessageBox.Icon.Warning)
        dialog.setWindowTitle("편집 중인 카드 삭제")
        dialog.setText("저장하지 않은 변경이 있습니다.")
        dialog.setInformativeText("변경 내용을 저장한 뒤 삭제하거나 그대로 삭제할 수 있습니다.")
        save_button = dialog.addButton(
            "저장 후 삭제",
            QMessageBox.ButtonRole.AcceptRole,
        )
        discard_button = dialog.addButton(
            "그대로 삭제",
            QMessageBox.ButtonRole.DestructiveRole,
        )
        cancel_button = dialog.addButton(
            "취소",
            QMessageBox.ButtonRole.RejectRole,
        )
        dialog.setDefaultButton(cancel_button)
        dialog.exec()
        clicked = dialog.clickedButton()
        if clicked is save_button:
            return "save"
        if clicked is discard_button:
            return "discard"
        return "cancel"

    def _export_cards(self, card_ids_value: object) -> None:
        if not isinstance(card_ids_value, tuple):
            return
        cards: list[Card] = []
        for card_id in card_ids_value:
            if not isinstance(card_id, str):
                continue
            card = self._repositories.get_card(card_id)
            if (
                card is not None
                and card.document_id == self.document_id
                and card.deleted_at_us is None
            ):
                cards.append(card)
        if not cards:
            return
        first = min(cards, key=lambda card: (card.position_key, card.id))
        timestamp = datetime.fromtimestamp(first.created_at_us / 1_000_000)
        default_name = f"pyNote_카드_{timestamp:%Y%m%d_%H%M%S}.txt"
        filename, selected_filter = QFileDialog.getSaveFileName(
            self,
            "카드 파일로 내보내기",
            default_name,
            "텍스트 파일 (*.txt);;Markdown (*.md)",
        )
        if not filename:
            return
        path = Path(filename)
        if not path.suffix:
            path = path.with_suffix(".md" if "Markdown" in selected_filter else ".txt")
        try:
            export_cards(path, cards, newline=NewlineFormat.LF)
        except BaseException as error:
            LOGGER.exception("카드 파일 내보내기에 실패했습니다.")
            self._error_reporter("카드 내보내기 실패", str(error))

    def _delete_cards(self, card_ids_value: object) -> None:
        if not isinstance(card_ids_value, tuple):
            return
        if not self._can_run_destructive_command():
            return
        try:
            for card_id in card_ids_value:
                if isinstance(card_id, str):
                    self.card_service.soft_delete(card_id)
        except BaseException as error:
            LOGGER.exception("문서 페이지에서 카드 삭제에 실패했습니다.")
            self._error_reporter("카드 삭제 실패", str(error))
            # 카드별 독립 트랜잭션이라 실패 전까지 지워진 분이 남는다.
            self._release_editor_session_if_card_removed()
            self.refresh()
            self.content_changed.emit()
            return
        self._release_editor_session_if_card_removed()
        self.refresh()
        self.content_changed.emit()

    def _restore_trash_item(self, listing: QListWidget, dialog: QDialog) -> None:
        card_id = self._selected_trash_card_id(listing)
        if card_id is None:
            return
        if not self._can_run_destructive_command():
            return
        try:
            self.card_service.restore_card(card_id)
        except BaseException as error:
            LOGGER.exception("카드 휴지통 복구에 실패했습니다.")
            self._error_reporter("카드 복구 실패", str(error))
            return
        dialog.accept()
        self.refresh()
        self.content_changed.emit()

    def _purge_trash_item(self, listing: QListWidget, dialog: QDialog) -> None:
        card_id = self._selected_trash_card_id(listing)
        if card_id is None:
            return
        first = QMessageBox.warning(
            self,
            "비가역 완전 삭제",
            "이 카드의 리비전과 계보 일부가 물리 삭제됩니다. 계속할까요?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No,
        )
        if first is not QMessageBox.StandardButton.Yes:
            return
        text, accepted = QInputDialog.getText(
            self,
            "완전 삭제 재확인",
            "되돌릴 수 없습니다. PURGE를 입력하세요.",
        )
        if not accepted or text != "PURGE":
            return
        if not self._can_run_destructive_command():
            return
        try:
            self.purge_service.purge_card(
                card_id,
                retention_days=self._policy_store.load().trash_retention_days,
            )
        except BaseException as error:
            LOGGER.exception("카드 purge에 실패했습니다.")
            self._error_reporter("카드 완전 삭제 실패", str(error))
            return
        dialog.accept()
        self.refresh()
        self.content_changed.emit()

    def _card_committed(self, value: object) -> None:
        if not isinstance(value, Card):
            raise TypeError("확정 카드 신호에는 Card가 필요합니다.")
        self.stream.card_model.update_card(
            value,
            revision_count=len(self._repositories.list_revisions(value.id)),
        )
        self.history.refresh()
        self.content_changed.emit()

    def _card_created(self, value: object) -> None:
        if not isinstance(value, Card):
            raise TypeError("생성 카드 신호에는 Card가 필요합니다.")
        self.stream.card_model.add_cards(
            (value,),
            revision_counts={value.id: 1},
        )
        self._promote_pending_binding(value)
        self.history.refresh()
        self.content_changed.emit()

    def _card_connected(self, card_id: str) -> None:
        # 대기 승격은 card_created 가 먼저 처리한다 — 여기 남은 대기는 다른 카드를
        # 열어 버려진 것이다.
        self._pending_binding = None
        if not self.reveal_card(card_id):
            # 필터로 목록 행이 가려진 생성 카드도 이력 대상은 갱신한다.
            self.history.set_card(card_id)
        self.editor.setFocus()
        self.card_opened.emit(card_id)

    def _card_deleted(self, value: object) -> None:
        if not isinstance(value, Card):
            raise TypeError("삭제 카드 신호에는 Card가 필요합니다.")
        self.refresh()
        self.content_changed.emit()

    def _card_restored(self, _value: object) -> None:
        self.refresh()
        self.content_changed.emit()

    def _selection_changed(
        self,
        current: QModelIndex,
        _previous: QModelIndex,
    ) -> None:
        card_id = current.data(Qt.ItemDataRole.UserRole + 2)
        if not isinstance(card_id, str):
            return
        if self.mode_stack.currentWidget() is self.history:
            self.history.set_card(card_id)
            return
        # 숨은 이력 화면의 리비전 목록을 선택할 때마다 다시 만들면 휠 탐색 한
        # 번에 지나친 카드 수만큼 리비전 조회가 쌓인다. 대상만 기록해 두고
        # show_history()의 refresh()가 실제 구축을 맡는다.
        self.history.set_pending_card(card_id)

    def _sort_changed(self) -> None:
        mode = self.sort_combo.currentData()
        if isinstance(mode, str):
            self.stream.set_sort_mode(mode)

    def _filter_changed(self) -> None:
        source = self.source_filter.currentData()
        self.stream.set_source_filter(None if source is None else (str(source),))

    def _release_editor_session_if_card_removed(self) -> None:
        session = self.editor.session
        if session is None or session.card_id is None:
            return
        card = self._repositories.get_card(session.card_id)
        if card is not None and card.deleted_at_us is None:
            return
        self.editor.release_session_for_removed_card()

    @staticmethod
    def _selected_trash_card_id(listing: QListWidget) -> str | None:
        item = listing.currentItem()
        if item is None:
            return None
        value = item.data(Qt.ItemDataRole.UserRole)
        return value if isinstance(value, str) else None

    def _setting_int(self, key: str, default: int) -> int:
        if self._settings is None:
            return default
        return int(str(self._settings.value(key, default)))

    def _setting_float(self, key: str, default: float) -> float:
        if self._settings is None:
            return default
        return float(str(self._settings.value(key, default)))

    def _can_run_destructive_command(self) -> bool:
        if self._destructive_preflight is not None:
            return self._destructive_preflight(self.document_id)
        return self.can_leave_editor(protect_now=True)

    def _show_error(self, title: str, message: str) -> None:
        QMessageBox.critical(self, title, message)
