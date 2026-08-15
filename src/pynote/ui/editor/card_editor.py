from __future__ import annotations

import logging
from collections.abc import Callable
from datetime import datetime
from enum import StrEnum
from pathlib import Path
from typing import Protocol, cast

from PySide6.QtCore import QEvent, QMimeData, Qt, QTimer, Signal
from PySide6.QtGui import (
    QCloseEvent,
    QDragEnterEvent,
    QDragMoveEvent,
    QDropEvent,
    QFont,
    QInputMethodEvent,
    QKeyEvent,
    QMouseEvent,
    QResizeEvent,
    QShowEvent,
    QTextBlockFormat,
    QTextCursor,
    QTextDocument,
)
from PySide6.QtWidgets import (
    QCheckBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QSplitter,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

from pynote.application.card_service import CardService
from pynote.application.draft_coordinator import (
    DraftCoordinator,
    DraftDisposition,
    DraftSession,
    RecoveryCandidate,
)
from pynote.application.save_coordinator import (
    ImeCompositionInProgressError,
    SaveCoordinator,
    SaveOutcome,
)
from pynote.domain.models import CaptureOperationSource, Card
from pynote.domain.paragraph_parser import ParagraphParser
from pynote.infrastructure.repositories import Repositories
from pynote.ui.cards.card_model import CARD_MIME_TYPE, CardListModel

LOGGER = logging.getLogger(__name__)

RecoveryChoiceProvider = Callable[[RecoveryCandidate], DraftDisposition]

# 목록과 슬롯이 이 아래로 줄면 각 pane을 사용하기 어렵다.
MIN_LIST_WIDTH = 260
MIN_SLOT_WIDTH = 380


class CloseChoice(StrEnum):
    """dirty 편집기를 떠날 때 선택할 수 있는 동작이다."""

    SAVE = "save"
    DISCARD = "discard"
    KEEP_EDITING = "keep_editing"


CloseChoiceProvider = Callable[[DraftSession], CloseChoice]


class EditorStatus(StrEnum):
    """UX 설계의 카드 편집 상태 5단계다."""

    EDITING = "editing"
    DRAFT_PROTECTED = "draft_protected"
    SAVING = "saving"
    SAVED = "saved"
    SAVE_FAILED = "save_failed"


class CardOpenSource(Protocol):
    """카드 스트림이 편집기 열기 신호를 제공하는 최소 계약이다."""

    card_open_requested: object


class CardEditor(QPlainTextEdit):
    """확정 카드와 분리된 DraftSession만 수정하는 장문 평문 편집기다."""

    status_changed = Signal(str, str)
    card_committed = Signal(object)
    card_created = Signal(object)
    card_connected = Signal(str)
    card_deleted = Signal(object)
    files_dropped = Signal(object)
    draft_dirty_changed = Signal(str, bool)
    session_changed = Signal(bool)
    save_conflict = Signal(object)
    close_accepted = Signal()
    find_requested = Signal()
    replace_requested = Signal()
    find_next_requested = Signal()
    find_previous_requested = Signal()

    def __init__(
        self,
        repositories: Repositories,
        draft_coordinator: DraftCoordinator,
        save_coordinator: SaveCoordinator,
        *,
        document_id: str | None = None,
        card_service: CardService | None = None,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._repositories = repositories
        self._draft_coordinator = draft_coordinator
        self._save_coordinator = save_coordinator
        if (document_id is None) != (card_service is None):
            raise ValueError("빈 편집면에는 document_id와 card_service가 함께 필요합니다.")
        self._document_id = document_id
        self._card_service = card_service
        self._paragraph_parser = ParagraphParser()
        self._session: DraftSession | None = None
        self._new_session: DraftSession | None = None
        self._pending_card_id: str | None = None
        self._input_source: CaptureOperationSource | None = None
        self._loading = False
        self._status = EditorStatus.SAVED
        self._status_text = "저장됨"
        # 카드 저장 실패만 latch 한다 — 초안 쓰기 실패는 다음 보호 성공으로
        # 회복돼야 하므로 같은 상태값을 공유하되 latch 대상이 아니다.
        self._card_save_failed = False
        self._default_font = self.font()
        self._line_spacing = 1.0
        self._recovery_dispositions: dict[str, DraftDisposition] = {}

        self.setObjectName("cardEditor")
        self.setReadOnly(False)
        self.setPlaceholderText("내용을 입력하면 새 카드가 만들어집니다.")
        self.setUndoRedoEnabled(True)
        self.setLineWrapMode(QPlainTextEdit.LineWrapMode.WidgetWidth)
        self.document().setDocumentMargin(16)
        self.textChanged.connect(self._sync_session)
        self.cursorPositionChanged.connect(self._sync_cursor)
        self.document().contentsChange.connect(self._handle_contents_change)
        self._draft_coordinator.draft_protected.connect(self._handle_draft_protected)
        self._draft_coordinator.draft_write_failed.connect(
            self._handle_draft_write_failed
        )
        if self._document_id is not None:
            self._open_empty_surface()

    @property
    def session(self) -> DraftSession | None:
        """현재 편집 중인 DraftSession을 반환한다."""
        return self._session

    @property
    def card_id(self) -> str | None:
        """빈 편집면과 연결됨 상태를 구분하는 공개 카드 ID를 반환한다."""
        return None if self._session is None else self._session.card_id

    @property
    def status(self) -> EditorStatus:
        """현재 저장 UX 상태를 반환한다."""
        return self._status

    @property
    def status_text(self) -> str:
        """상태 표시줄에 사용할 한국어 문구를 반환한다."""
        return self._status_text

    def open_card(
        self,
        card_id: str,
        *,
        disposition: DraftDisposition | None = None,
        recovery_choice_provider: RecoveryChoiceProvider | None = None,
        app_driven: bool = False,
    ) -> bool:
        """확정 카드를 draft buffer로 복사해 연다."""
        card = self._repositories.get_card(card_id)
        if card is None or card.deleted_at_us is not None:
            raise KeyError(f"활성 카드가 아닙니다: {card_id}")
        if self._session is not None and self._session.card_id == card_id:
            # 같은 카드를 다시 여는 요청도 "열기"다. 목록 press가 가져간 포커스를
            # 되돌리지 않으면 이어지는 타이핑이 목록으로 들어가 사라진다.
            self.card_connected.emit(card_id)
            return True
        if self._session is not None:
            if app_driven:
                if not self.detach_session_quietly():
                    return False
            else:
                if not self.can_leave_editor(protect_now=True):
                    return False
                if not self._release_clean_session():
                    return False
        elif self._new_session is not None and not self._protect_empty_surface():
            return False
        effective_disposition = (
            disposition
            if disposition is not None
            else self._recovery_dispositions.pop(card_id, None)
        )
        session = self._draft_coordinator.open_card(
            card,
            disposition=effective_disposition,
        )
        if session is None and effective_disposition is None:
            candidate = self._candidate_for_card(card_id)
            if candidate is None:
                raise RuntimeError("카드 draft의 복구 후보를 찾지 못했습니다.")
            choice = (
                recovery_choice_provider(candidate)
                if recovery_choice_provider is not None
                else self._ask_recovery_choice(candidate)
            )
            if choice is DraftDisposition.LATER:
                return False
            session = self._draft_coordinator.open_card(card, disposition=choice)
        if session is None:
            return False

        self._release_new_backing(discard=False)
        self._session = session
        self._loading = True
        try:
            self.setPlainText(session.text)
            self._apply_line_spacing_to_document()
            self.document().clearUndoRedoStacks()
            self._restore_cursor_qchar(session.cursor_position_qchar)
        finally:
            self._loading = False
        self._set_status(
            EditorStatus.EDITING if session.dirty else EditorStatus.SAVED,
            "편집 중" if session.dirty else "저장됨",
        )
        self.session_changed.emit(True)
        self.card_connected.emit(card_id)
        if session.card_id is not None:
            self.draft_dirty_changed.emit(session.card_id, session.dirty)
        return True

    def set_recovery_disposition(
        self,
        card_id: str,
        disposition: DraftDisposition,
    ) -> None:
        """시작 시 확정한 recovery 처분을 해당 카드의 첫 진입에 적용한다."""
        self._recovery_dispositions[card_id] = disposition

    def save_current(self) -> bool:
        """현재 draft를 동기 저장하고 성공 뒤에만 카드 갱신 신호를 낸다."""
        session = self._session
        if session is None:
            return True
        self._sync_session()
        was_dirty = session.dirty
        self._set_status(EditorStatus.SAVING, "저장 중…")
        try:
            result = self._save_coordinator.save(session)
        except ImeCompositionInProgressError as error:
            self._card_save_failed = True
            self._set_status(
                EditorStatus.SAVE_FAILED,
                f"저장 실패 — 다시 시도: {error}",
            )
            return False
        except BaseException as error:
            LOGGER.exception("편집기에서 카드 저장 요청이 실패했습니다.")
            self._card_save_failed = True
            self._set_status(
                EditorStatus.SAVE_FAILED,
                f"저장 실패 — 다시 시도: {error}",
            )
            return False

        if result.outcome is SaveOutcome.CONFLICT:
            if result.conflict is None:
                raise RuntimeError("충돌 결과에 비교 정보가 없습니다.")
            self._card_save_failed = True
            self._set_status(
                EditorStatus.SAVE_FAILED,
                "저장 실패 — 기준 리비전이 변경됨, 비교 후 다시 시도",
            )
            self.save_conflict.emit(result.conflict)
            return False

        self._card_save_failed = False
        self._set_status(
            EditorStatus.SAVED,
            f"저장됨 {self._format_time(result.card.updated_at_us)}",
        )
        if was_dirty and session.card_id is not None and not session.dirty:
            self.draft_dirty_changed.emit(session.card_id, False)
        if result.outcome is SaveOutcome.SAVED:
            self.card_committed.emit(result.card)
        return True

    def can_leave_editor(
        self,
        *,
        choice_provider: CloseChoiceProvider | None = None,
        protect_now: bool = False,
    ) -> bool:
        """편집기를 떠나는 모든 경로의 저장·버리기·계속 편집 선택을 적용한다."""
        session = self._session
        if session is None:
            try:
                return self._protect_empty_surface()
            except BaseException:
                LOGGER.exception("빈 편집면 이탈 전 보호에 실패했습니다.")
                return False
        if self._draft_coordinator.is_ime_composing(session.draft_id):
            self._set_status(
                EditorStatus.SAVE_FAILED,
                "저장 실패 — 한글 IME 조합을 확정한 뒤 다시 시도",
            )
            return False
        if not session.dirty:
            return True
        if protect_now and not self.protect_now():
            return False
        if choice_provider is None:
            if self.save_current():
                return True
            choice = self._ask_close_choice()
        else:
            choice = choice_provider(session)
        if choice is CloseChoice.KEEP_EDITING:
            return False
        if choice is CloseChoice.SAVE:
            if not self.save_current():
                return False
        else:
            if not self._prepare_empty_surface():
                return False
            self._draft_coordinator.discard_session(session.draft_id)
            if session.card_id is not None:
                self.draft_dirty_changed.emit(session.card_id, False)
            self._clear_editor()
        return True

    def request_close(
        self,
        *,
        choice_provider: CloseChoiceProvider | None = None,
    ) -> bool:
        """사용자 취소·뒤로 이동에서 단일 이탈 게이트를 거쳐 편집기를 닫는다."""
        if not self.can_leave_editor(
            choice_provider=choice_provider,
            protect_now=True,
        ):
            self._restore_focus_after_close()
            return False
        if not self._release_clean_session():
            self._restore_focus_after_close()
            return False
        self.close_accepted.emit()
        self._restore_focus_after_close()
        return True

    def protect_now(self) -> bool:
        """현재 메모리 draft를 종료·전환 전에 즉시 recovery 저장한다."""
        session = self._session
        if session is None:
            return self._protect_empty_surface()
        self._sync_session()
        if self._draft_coordinator.is_ime_composing(session.draft_id):
            # 조합 중에는 protect_now가 쓰기 없이 반환하므로 보호 성공이 아니다.
            self._set_status(
                EditorStatus.SAVE_FAILED,
                "저장 실패 — 한글 IME 조합을 확정한 뒤 다시 시도",
            )
            return False
        if not session.dirty:
            return True
        try:
            self._draft_coordinator.protect_now(session.draft_id)
        except BaseException as error:
            LOGGER.exception("편집기 이탈 전 recovery draft 보호에 실패했습니다.")
            self._set_status(
                EditorStatus.SAVE_FAILED,
                f"저장 실패 — 다시 시도: {error}",
            )
            return False
        return True

    def detach_session_quietly(self) -> bool:
        """앱 주도 전환에서 최신 초안을 보호한 뒤 확정 없이 세션만 해제한다."""
        session = self._session
        if session is None:
            if not self._protect_empty_surface():
                return False
            self._release_new_backing(discard=False)
            return True
        if not self.protect_now():
            return False
        if not self._prepare_empty_surface():
            return False
        self._draft_coordinator.release_session(session.draft_id)
        self._clear_editor()
        return True

    def cleanup_empty_card_before_exit(self) -> None:
        """종료 직전 clean 빈 확정 카드를 휴지통으로 옮긴다."""
        deleted_card = self._cleanup_empty_card()
        if deleted_card is not None:
            self.card_deleted.emit(deleted_card)

    def release_session_for_removed_card(self) -> None:
        """소멸한 원본 카드의 편집 세션을 recovery draft와 분리해 해제한다."""
        session = self._session
        if session is None:
            return
        if session.dirty and not self.protect_now():
            LOGGER.warning(
                "소멸한 카드의 recovery draft를 보호하지 못한 채 "
                "편집 세션을 해제합니다."
            )
        if not self._prepare_empty_surface():
            LOGGER.error(
                "NEW backing을 확보하지 못해 삭제된 카드의 연결을 유지합니다."
            )
            return
        self._draft_coordinator.release_session(session.draft_id)
        self._clear_editor(cleanup_empty=False)

    def discard_session_for_deleted_card(self) -> None:
        """이미 삭제된 카드 세션을 draft 재생성이나 중복 DELETE 없이 해제한다."""
        session = self._session
        if session is None:
            return
        if not self._prepare_empty_surface():
            LOGGER.error(
                "NEW backing을 확보하지 못해 삭제된 카드의 연결을 유지합니다."
            )
            return
        self._draft_coordinator.release_session(session.draft_id)
        self._clear_editor(cleanup_empty=False)
        self.setFocus()
        QTimer.singleShot(0, self, self.setFocus)

    def set_wrap_visible(self, visible: bool) -> None:
        """화면 폭 기준 줄 바꿈 표시를 켜거나 끈다."""
        mode = (
            QPlainTextEdit.LineWrapMode.WidgetWidth
            if visible
            else QPlainTextEdit.LineWrapMode.NoWrap
        )
        self.setLineWrapMode(mode)

    def apply_editor_font(self, font: QFont) -> None:
        """장문 편집기에 선택한 글꼴을 적용한다."""
        self._default_font = QFont(font)
        self.setFont(font)

    def apply_line_spacing(self, multiplier: float) -> None:
        """전체 평문 블록에 설정한 줄 간격 배율을 적용한다."""
        if multiplier <= 0:
            raise ValueError("줄 간격은 0보다 커야 합니다.")
        self._line_spacing = multiplier
        was_loading = self._loading
        self._loading = True
        try:
            self._apply_line_spacing_to_document()
        finally:
            self._loading = was_loading

    def find_text(
        self,
        text: str,
        *,
        backwards: bool = False,
        case_sensitive: bool = False,
    ) -> bool:
        """현재 커서부터 평문을 찾아 선택한다."""
        if not text:
            return False
        flags = QTextDocument.FindFlag(0)
        if backwards:
            flags |= QTextDocument.FindFlag.FindBackward
        if case_sensitive:
            flags |= QTextDocument.FindFlag.FindCaseSensitively
        if self.find(text, flags):
            return True
        cursor = QTextCursor(self.document())
        cursor.movePosition(
            QTextCursor.MoveOperation.End
            if backwards
            else QTextCursor.MoveOperation.Start
        )
        self.setTextCursor(cursor)
        return self.find(text, flags)

    def replace_one(
        self,
        find_text: str,
        replacement: str,
        *,
        case_sensitive: bool = False,
    ) -> bool:
        """현재 선택이 일치하면 바꾸고 아니면 다음 항목을 찾아 바꾼다."""
        if not find_text:
            return False
        cursor = self.textCursor()
        selected = cursor.selectedText()
        matches = (
            selected == find_text
            if case_sensitive
            else selected.casefold() == find_text.casefold()
        )
        if not matches and not self.find_text(
            find_text,
            case_sensitive=case_sensitive,
        ):
            return False
        self.textCursor().insertText(replacement)
        return True

    def replace_all(
        self,
        find_text: str,
        replacement: str,
        *,
        case_sensitive: bool = False,
    ) -> int:
        """문서 전체에서 일치하는 평문을 QTextCursor로 바꾼다."""
        if not find_text:
            return 0
        flags = QTextDocument.FindFlag(0)
        if case_sensitive:
            flags |= QTextDocument.FindFlag.FindCaseSensitively
        count = 0
        cursor = QTextCursor(self.document())
        cursor.beginEditBlock()
        try:
            while True:
                found = self.document().find(find_text, cursor, flags)
                if found.isNull():
                    break
                found.insertText(replacement)
                cursor = found
                count += 1
        finally:
            cursor.endEditBlock()
        return count

    def inputMethodEvent(self, event: QInputMethodEvent) -> None:
        """IME preedit 동안 recovery와 영구 저장 경계를 닫는다."""
        session = self._session or self._new_session
        composing = bool(event.preeditString())
        if session is not None:
            self._draft_coordinator.set_ime_composing(session.draft_id, composing)
        previous_source = self._input_source
        if event.commitString():
            self._input_source = CaptureOperationSource.TYPING
        try:
            super().inputMethodEvent(event)
        finally:
            self._input_source = previous_source

    def insertFromMimeData(self, source: QMimeData) -> None:
        """붙여넣기 포함 여부를 draft 편집 세션에 기록한다."""
        if source.hasUrls() and not source.hasFormat("text/plain"):
            return
        previous_source = self._input_source
        self._input_source = CaptureOperationSource.PASTE
        try:
            super().insertFromMimeData(source)
        finally:
            self._input_source = previous_source

    def dragEnterEvent(self, event: QDragEnterEvent) -> None:
        """카드 MIME은 거부하고 로컬 파일 URL만 드롭 후보로 받는다."""
        mime = event.mimeData()
        if mime.hasFormat(CARD_MIME_TYPE):
            event.ignore()
            return
        if mime.hasUrls():
            if self._file_paths_from_mime(mime) is None:
                event.ignore()
            else:
                event.acceptProposedAction()
            return
        super().dragEnterEvent(event)

    def dragMoveEvent(self, event: QDragMoveEvent) -> None:
        """drag-enter와 같은 우선순위로 move 단계의 수락을 판정한다."""
        mime = event.mimeData()
        if mime.hasFormat(CARD_MIME_TYPE):
            event.ignore()
            return
        if mime.hasUrls():
            if self._file_paths_from_mime(mime) is None:
                event.ignore()
            else:
                event.acceptProposedAction()
            return
        super().dragMoveEvent(event)

    def dropEvent(self, event: QDropEvent) -> None:
        """텍스트 drop의 삭제·삽입 단계 전체에 paste 삽입 토큰을 한정한다."""
        mime = event.mimeData()
        if mime.hasFormat(CARD_MIME_TYPE):
            event.ignore()
            return
        if mime.hasUrls():
            paths = self._file_paths_from_mime(mime)
            if paths is None:
                event.ignore()
                return
            self.files_dropped.emit(paths)
            event.acceptProposedAction()
            return
        if not mime.hasText():
            super().dropEvent(event)
            return
        previous_source = self._input_source
        self._input_source = CaptureOperationSource.PASTE
        try:
            super().dropEvent(event)
        finally:
            self._input_source = previous_source

    @staticmethod
    def _file_paths_from_mime(mime: QMimeData) -> tuple[Path, ...] | None:
        urls = mime.urls()
        if not urls:
            return None
        paths: list[Path] = []
        for url in urls:
            if not url.isLocalFile():
                return None
            paths.append(Path(url.toLocalFile()))
        return tuple(paths)

    def _closes_by_shortcut(self) -> bool:
        """ESC·뒤로가기를 편집기 닫기로 다뤄도 되는 상태인지 판정한다."""
        session = self._session
        if session is None:
            return False
        # 조합 중 ESC 는 조합 취소여야 하므로 편집기를 닫지 않는다.
        return not self._draft_coordinator.is_ime_composing(session.draft_id)

    def event(self, event: QEvent) -> bool:
        """선택이 있는 ESC 는 back_action 단축키보다 먼저 이 위젯이 가져간다."""
        if (
            event.type() is QEvent.Type.ShortcutOverride
            and cast(QKeyEvent, event).key() == Qt.Key.Key_Escape
            and self.textCursor().hasSelection()
        ):
            event.accept()
            return True
        return super().event(event)

    def keyPressEvent(self, event: QKeyEvent) -> None:
        """Ctrl+S/F/H·F3 계열과 확대·축소 단축키를 처리한다.

        ESC 는 MainWindow 의 back_action 이 ApplicationShortcut 으로 소유하되,
        선택이 있으면 event()의 ShortcutOverride 수락으로 여기 도달해 선택
        해제만 하고 포커스와 편집 세션을 유지한다.
        """
        modifiers = event.modifiers()
        if event.key() == Qt.Key.Key_Escape and self.textCursor().hasSelection():
            cursor = self.textCursor()
            cursor.clearSelection()
            self.setTextCursor(cursor)
            event.accept()
            return
        if event.key() == Qt.Key.Key_F3:
            if modifiers == Qt.KeyboardModifier.ShiftModifier:
                self.find_previous_requested.emit()
                event.accept()
                return
            if modifiers == Qt.KeyboardModifier.NoModifier:
                self.find_next_requested.emit()
                event.accept()
                return
        if modifiers & Qt.KeyboardModifier.ControlModifier:
            if event.key() in {Qt.Key.Key_Return, Qt.Key.Key_Enter}:
                self.insertPlainText("\n")
                event.accept()
                return
            if event.key() == Qt.Key.Key_S:
                self.save_current()
                event.accept()
                return
            if event.key() == Qt.Key.Key_F:
                self.find_requested.emit()
                event.accept()
                return
            if event.key() == Qt.Key.Key_H:
                self.replace_requested.emit()
                event.accept()
                return
            if event.key() in {Qt.Key.Key_Plus, Qt.Key.Key_Equal}:
                self.zoomIn(1)
                event.accept()
                return
            if event.key() == Qt.Key.Key_Minus:
                self.zoomOut(1)
                event.accept()
                return
            if event.key() == Qt.Key.Key_0:
                self.setFont(self._default_font)
                event.accept()
                return
        text = event.text()
        insertion_modifiers = (
            Qt.KeyboardModifier.ControlModifier
            | Qt.KeyboardModifier.AltModifier
            | Qt.KeyboardModifier.MetaModifier
        )
        if (
            text
            and text.isprintable()
            and not modifiers & insertion_modifiers
        ):
            previous_source = self._input_source
            self._input_source = CaptureOperationSource.TYPING
            try:
                super().keyPressEvent(event)
            finally:
                self._input_source = previous_source
            return
        super().keyPressEvent(event)

    def mousePressEvent(self, event: QMouseEvent) -> None:
        """마우스 뒤로가기 버튼을 취소 버튼과 같게 다룬다."""
        if (
            event.button() is Qt.MouseButton.BackButton
            and self._closes_by_shortcut()
        ):
            self.request_close()
            event.accept()
            return
        super().mousePressEvent(event)

    def closeEvent(self, event: QCloseEvent) -> None:
        """위젯이 직접 닫힐 때도 dirty 종료 계약을 적용한다."""
        if self.request_close():
            event.accept()
        else:
            event.ignore()

    def _sync_session(self) -> None:
        if self._loading:
            return
        if self._session is None:
            self._sync_empty_backing()
            return
        was_dirty = self._session.dirty
        session = self._draft_coordinator.update_session(
            self._session.draft_id,
            text=self.toPlainText(),
            cursor_position_qchar=self.textCursor().position(),
            includes_paste=(
                self._input_source is CaptureOperationSource.PASTE
            ),
        )
        if was_dirty != session.dirty and session.card_id is not None:
            self.draft_dirty_changed.emit(session.card_id, session.dirty)
        if session.dirty and self._status not in {
            EditorStatus.SAVING,
            EditorStatus.SAVE_FAILED,
        }:
            self._set_status(EditorStatus.EDITING, "편집 중")
        elif not session.dirty and self._card_save_failed:
            # 확정본과 같아졌으면 저장할 것이 없다 — 실패 latch 를 푼다.
            self._card_save_failed = False
            self._set_status(EditorStatus.SAVED, "저장됨")

    def _release_clean_session(self) -> bool:
        session = self._session
        if session is None:
            return True
        if session.dirty:
            raise RuntimeError("dirty draft는 이탈 게이트 없이 닫을 수 없습니다.")
        if not self._prepare_empty_surface():
            return False
        self._draft_coordinator.discard_session(session.draft_id)
        self._clear_editor()
        return True

    def _sync_empty_backing(self) -> None:
        session = self._new_session
        if session is None:
            return
        if (
            self._draft_coordinator.is_ime_composing(session.draft_id)
            and self._input_source is not CaptureOperationSource.PASTE
        ):
            return
        session = self._draft_coordinator.update_session(
            session.draft_id,
            text=self.toPlainText(),
            cursor_position_qchar=self.textCursor().position(),
            includes_paste=(
                self._input_source is CaptureOperationSource.PASTE
            ),
        )
        self._new_session = session

    def _handle_contents_change(
        self,
        _position: int,
        _chars_removed: int,
        chars_added: int,
    ) -> None:
        """사용자 삽입 토큰과 실제 내용 추가가 겹칠 때만 생성을 시도한다."""
        source = self._input_source
        if (
            self._loading
            or self._session is not None
            or source is None
            or chars_added <= 0
        ):
            return
        self._sync_empty_backing()
        self._create_or_connect_from_insertion(source)

    def _create_or_connect_from_insertion(
        self,
        source: CaptureOperationSource,
    ) -> None:
        session = self._new_session
        if session is None:
            return
        if self._pending_card_id is not None:
            self._connect_pending_card()
            if self._pending_card_id is not None or self._session is not None:
                return
        if self._paragraph_parser.is_zero_paragraph_input(session.text):
            return
        if self._card_service is None or self._document_id is None:
            return
        try:
            cards = self._card_service.create_cards(
                self._document_id,
                session.text,
                source=source,
                split=False,
            )
        except BaseException:
            LOGGER.exception("빈 편집면의 첫 입력으로 카드를 만들지 못했습니다.")
            return
        if not cards:
            LOGGER.warning("빈 편집면의 첫 입력 카드 생성이 결과 없이 끝났습니다.")
            return
        if len(cards) != 1:
            LOGGER.error(
                "split=False 카드 생성이 %d장을 반환했습니다.",
                len(cards),
            )
        for card in cards:
            self.card_created.emit(card)
        self._pending_card_id = cards[0].id
        self._connect_pending_card()

    def _sync_cursor(self) -> None:
        if self._loading:
            return
        if self._session is not None:
            self._sync_session()
            return
        session = self._new_session
        if session is None:
            return
        if (
            self._session is None
            and self._draft_coordinator.is_ime_composing(session.draft_id)
        ):
            return
        self._new_session = self._draft_coordinator.update_session(
            session.draft_id,
            text=self.toPlainText(),
            cursor_position_qchar=self.textCursor().position(),
        )

    def _connect_pending_card(self) -> bool:
        card_id = self._pending_card_id
        if card_id is None:
            return False
        card = self._repositories.get_card(card_id)
        if card is None or card.deleted_at_us is not None:
            LOGGER.error("생성된 카드를 연결할 수 없습니다: card=%s", card_id)
            self._pending_card_id = None
            return False
        try:
            session = self._draft_coordinator.open_card(card)
        except BaseException:
            LOGGER.exception("생성된 카드를 편집면에 연결하지 못했습니다.")
            return False
        if session is None:
            LOGGER.warning(
                "생성된 카드의 편집 세션을 열지 못했습니다: card=%s",
                card_id,
            )
            return False

        new_session = self._new_session
        was_composing = (
            new_session is not None
            and self._draft_coordinator.is_ime_composing(new_session.draft_id)
        )
        self._session = session
        self._session = self._draft_coordinator.update_session(
            session.draft_id,
            text=self.toPlainText(),
            cursor_position_qchar=self.textCursor().position(),
        )
        self._pending_card_id = None
        self._new_session = None
        if was_composing:
            self._draft_coordinator.set_ime_composing(
                self._session.draft_id,
                True,
            )
        self._set_status(
            EditorStatus.EDITING if self._session.dirty else EditorStatus.SAVED,
            "편집 중" if self._session.dirty else "저장됨",
        )
        self.session_changed.emit(True)
        self.card_connected.emit(card_id)
        self.draft_dirty_changed.emit(card_id, self._session.dirty)
        if new_session is not None:
            try:
                self._draft_coordinator.discard_session(new_session.draft_id)
            except BaseException:
                LOGGER.exception(
                    "카드 연결 뒤 NEW recovery draft 폐기에 실패했습니다."
                )
                self._draft_coordinator.release_session(new_session.draft_id)
        return True

    def _open_empty_surface(self) -> None:
        if self._document_id is None:
            return
        if self._new_session is None:
            self._new_session = self._draft_coordinator.open_new(self._document_id)
        self._load_empty_surface()

    def _prepare_empty_surface(self) -> bool:
        if self._document_id is None or self._new_session is not None:
            return True
        try:
            self._new_session = self._draft_coordinator.open_new(self._document_id)
        except BaseException as error:
            LOGGER.exception("빈 편집면 전환 전 NEW backing 확보에 실패했습니다.")
            self._set_status(
                EditorStatus.SAVE_FAILED,
                f"저장 실패 — 빈 편집면 보호 준비 실패: {error}",
            )
            return False
        return True

    def _load_empty_surface(self) -> None:
        session = self._new_session
        if session is None:
            raise RuntimeError("빈 편집면에는 NEW backing이 필요합니다.")
        self._loading = True
        try:
            self.setPlainText(session.text)
            self._apply_line_spacing_to_document()
            self.document().clearUndoRedoStacks()
            self._restore_cursor_qchar(session.cursor_position_qchar)
        finally:
            self._loading = False

    def _release_new_backing(self, *, discard: bool) -> None:
        session = self._new_session
        if session is None:
            return
        if discard:
            self._draft_coordinator.discard_session(session.draft_id)
        else:
            self._draft_coordinator.release_session(session.draft_id)
        self._new_session = None

    def _protect_empty_surface(self) -> bool:
        session = self._new_session
        if session is None:
            return self._document_id is None
        if self._draft_coordinator.is_ime_composing(session.draft_id):
            self._set_status(
                EditorStatus.SAVE_FAILED,
                "저장 실패 — 한글 IME 조합을 확정한 뒤 다시 시도",
            )
            return False
        try:
            session = self._draft_coordinator.update_session(
                session.draft_id,
                text=self.toPlainText(),
                cursor_position_qchar=self.textCursor().position(),
            )
            self._new_session = session
            if not session.dirty:
                return True
            self._draft_coordinator.protect_now(session.draft_id)
        except BaseException as error:
            LOGGER.exception("빈 편집면의 NEW recovery draft 보호에 실패했습니다.")
            self._set_status(
                EditorStatus.SAVE_FAILED,
                f"저장 실패 — 다시 시도: {error}",
            )
            return False
        return True

    def _restore_focus_after_close(self) -> None:
        self.setFocus()
        QTimer.singleShot(0, self, self.setFocus)

    def _clear_editor(self, *, cleanup_empty: bool = True) -> None:
        deleted_card = self._cleanup_empty_card() if cleanup_empty else None
        self._session = None
        self._card_save_failed = False
        self._loading = True
        try:
            self.clear()
            self.document().clearUndoRedoStacks()
        finally:
            self._loading = False
        if self._new_session is None and self._document_id is not None:
            raise RuntimeError("연결 해제 전에 NEW backing을 확보해야 합니다.")
        if self._new_session is not None:
            self._load_empty_surface()
        self._set_status(EditorStatus.SAVED, "새 카드를 입력하세요.")
        self.session_changed.emit(False)
        if deleted_card is not None:
            self.card_deleted.emit(deleted_card)

    def _cleanup_empty_card(self) -> Card | None:
        session = self._session
        if (
            session is None
            or session.card_id is None
            or session.dirty
            or self._card_service is None
            or self._draft_coordinator.is_ime_composing(session.draft_id)
        ):
            return None
        try:
            card = self._repositories.get_card(session.card_id)
            if (
                card is None
                or card.deleted_at_us is not None
                or card.body.strip()
            ):
                return None
            deleted = cast(
                object,
                self._card_service.soft_delete(
                    card.id,
                    expected_revision_id=card.current_revision_id,
                    require_empty_body=True,
                ),
            )
        except BaseException:
            LOGGER.exception(
                "빈 카드 정리에 실패했지만 편집기 이탈을 계속합니다: card=%s",
                session.card_id,
            )
            return None
        if deleted is False:
            LOGGER.warning(
                "빈 카드 정리가 거부됐지만 편집기 이탈을 계속합니다: card=%s",
                session.card_id,
            )
            return None
        if not isinstance(deleted, Card):
            LOGGER.error(
                "빈 카드 정리가 올바른 Card를 반환하지 않았지만 "
                "편집기 이탈을 계속합니다: card=%s",
                session.card_id,
            )
            return None
        return deleted

    def _restore_cursor_qchar(self, stored_position: int) -> None:
        document_end = max(0, self.document().characterCount() - 1)
        position = min(max(0, stored_position), document_end)
        cursor = QTextCursor(self.document())
        cursor.setPosition(position)
        self.setTextCursor(cursor)

    def _apply_line_spacing_to_document(self) -> None:
        cursor = QTextCursor(self.document())
        cursor.select(QTextCursor.SelectionType.Document)
        block_format = QTextBlockFormat()
        block_format.setLineHeight(
            self._line_spacing * 100,
            QTextBlockFormat.LineHeightTypes.ProportionalHeight.value,
        )
        cursor.mergeBlockFormat(block_format)

    def _candidate_for_card(self, card_id: str) -> RecoveryCandidate | None:
        return next(
            (
                candidate
                for candidate in self._draft_coordinator.recovery_candidates()
                if candidate.draft.card_id == card_id
            ),
            None,
        )

    def _handle_draft_protected(
        self,
        draft_id: str,
        updated_at_us: object,
        _elapsed_ms: float,
    ) -> None:
        session = self._session or self._new_session
        if session is None or session.draft_id != draft_id:
            return
        if not isinstance(updated_at_us, int):
            raise TypeError("draft 보호 시각은 microsecond 정수여야 합니다.")
        if self._status is EditorStatus.SAVING or self._card_save_failed:
            return
        self._set_status(
            EditorStatus.DRAFT_PROTECTED,
            f"초안 보호됨 {self._format_time(updated_at_us)}",
        )

    def _handle_draft_write_failed(self, draft_id: str, message: str) -> None:
        session = self._session or self._new_session
        if session is None or session.draft_id != draft_id:
            return
        self._set_status(
            EditorStatus.SAVE_FAILED,
            f"저장 실패 — 다시 시도: {message}",
        )

    def _set_status(self, status: EditorStatus, text: str) -> None:
        self._status = status
        self._status_text = text
        self.status_changed.emit(status.value, text)

    def _require_session(self) -> DraftSession:
        if self._session is None:
            raise RuntimeError("열린 카드가 없습니다.")
        return self._session

    @staticmethod
    def _format_time(epoch_us: int) -> str:
        return datetime.fromtimestamp(epoch_us / 1_000_000).strftime("%H:%M:%S")

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
        box.setInformativeText("복구하면 초안으로 편집을 계속합니다.")
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

    def _ask_close_choice(self) -> CloseChoice:
        box = QMessageBox(self)
        box.setWindowTitle("미저장 초안")
        box.setText("저장하지 않은 변경이 있습니다.")
        save = box.addButton("저장", QMessageBox.ButtonRole.AcceptRole)
        discard = box.addButton("버리기", QMessageBox.ButtonRole.DestructiveRole)
        keep = box.addButton("계속 편집", QMessageBox.ButtonRole.RejectRole)
        box.setDefaultButton(cast(QPushButton, save))
        box.exec()
        clicked = box.clickedButton()
        if clicked is save:
            return CloseChoice.SAVE
        if clicked is discard:
            return CloseChoice.DISCARD
        if clicked is keep:
            return CloseChoice.KEEP_EDITING
        return CloseChoice.KEEP_EDITING


class FindReplaceBar(QWidget):
    """CardEditor의 찾기·바꾸기·전체 바꾸기 컨트롤이다."""

    def __init__(self, editor: CardEditor, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._editor = editor
        self._replace_mode = False
        self.setObjectName("findReplaceBar")
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        self.find_input = QLineEdit(self)
        self.find_input.setPlaceholderText("찾기")
        self.replace_input = QLineEdit(self)
        self.replace_input.setPlaceholderText("바꾸기")
        self.case_sensitive = QCheckBox("대/소문자", self)
        previous_button = QPushButton("이전", self)
        next_button = QPushButton("다음", self)
        replace_button = QPushButton("바꾸기", self)
        replace_all_button = QPushButton("전체 바꾸기", self)
        close_button = QToolButton(self)
        close_button.setText("닫기")
        for widget in (
            self.find_input,
            self.replace_input,
            self.case_sensitive,
            previous_button,
            next_button,
            replace_button,
            replace_all_button,
            close_button,
        ):
            layout.addWidget(widget)
        previous_button.clicked.connect(self.find_previous)
        next_button.clicked.connect(self.find_next)
        replace_button.clicked.connect(
            lambda: editor.replace_one(
                self.find_input.text(),
                self.replace_input.text(),
                case_sensitive=self.case_sensitive.isChecked(),
            )
        )
        replace_all_button.clicked.connect(
            lambda: editor.replace_all(
                self.find_input.text(),
                self.replace_input.text(),
                case_sensitive=self.case_sensitive.isChecked(),
            )
        )
        close_button.clicked.connect(self.hide)
        self.find_input.returnPressed.connect(next_button.click)
        self.hide()

    def keyPressEvent(self, event: QKeyEvent) -> None:
        """찾기 막대 안에서도 F3·Shift+F3 로 다음·이전 찾기를 잇는다."""
        if event.key() == Qt.Key.Key_F3:
            modifiers = event.modifiers()
            if modifiers == Qt.KeyboardModifier.ShiftModifier:
                self.find_previous()
                event.accept()
                return
            if modifiers == Qt.KeyboardModifier.NoModifier:
                self.find_next()
                event.accept()
                return
        super().keyPressEvent(event)

    def show_find(self, *, include_replace: bool) -> None:
        """찾기 또는 바꾸기 모드로 막대를 표시한다."""
        self._replace_mode = include_replace
        self.replace_input.setVisible(include_replace)
        self.show()
        self.find_input.setFocus()
        self.find_input.selectAll()

    def find_next(self) -> None:
        """마지막 검색어의 다음 일치로 이동한다. 검색어가 없으면 막대를 연다."""
        self._find(backwards=False)

    def find_previous(self) -> None:
        """마지막 검색어의 이전 일치로 이동한다. 검색어가 없으면 막대를 연다."""
        self._find(backwards=True)

    def _find(self, *, backwards: bool) -> None:
        text = self.find_input.text()
        if not text:
            self.show_find(include_replace=self._replace_mode)
            return
        self._editor.find_text(
            text,
            backwards=backwards,
            case_sensitive=self.case_sensitive.isChecked(),
        )


class CardEditorWorkspace(QWidget):
    """단일 편집면과 카드 목록을 가로 분할로 함께 표시한다."""

    card_committed = Signal(object)
    save_conflict = Signal(object)

    def __init__(
        self,
        list_pane: QWidget,
        editor: CardEditor,
        *,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._card_list = list_pane
        self.editor = editor
        self._open_split_sizes: tuple[int, int] | None = None
        self._applying_split = False
        self._split_apply_pending = True

        self.setObjectName("cardEditorWorkspace")
        root_layout = QVBoxLayout(self)
        root_layout.setContentsMargins(4, 4, 4, 4)
        self._splitter = QSplitter(Qt.Orientation.Horizontal, self)
        self._splitter.setObjectName("cardEditorSplitter")
        self._splitter.setChildrenCollapsible(False)
        self._splitter.setHandleWidth(4)
        root_layout.addWidget(self._splitter)

        self._editor_shell = QWidget()
        editor_layout = QVBoxLayout(self._editor_shell)
        editor_layout.setContentsMargins(0, 0, 0, 0)
        self.find_bar = FindReplaceBar(editor, self._editor_shell)
        editor_layout.addWidget(self.find_bar)
        editor_layout.addWidget(editor, 1)
        footer = QHBoxLayout()
        self.status_label = QLabel(editor.status_text, self._editor_shell)
        self.status_label.setObjectName("editorStatus")
        self.cancel_button = QPushButton("취소", self._editor_shell)
        self.cancel_button.setToolTip(
            "편집기를 닫으며 변경을 자동 저장한다. "
            "저장 실패 시에만 저장/버리기/계속 편집을 묻는다"
        )
        self.save_button = QPushButton("저장 Ctrl+S", self._editor_shell)
        self.save_button.setToolTip("초안을 카드에 확정하고 새 리비전을 만든다")
        footer.addWidget(self.status_label)
        footer.addStretch(1)
        footer.addWidget(self.cancel_button)
        footer.addWidget(self.save_button)
        editor_layout.addLayout(footer)

        self._splitter.addWidget(list_pane)
        self._splitter.addWidget(self._editor_shell)
        self._splitter.setStretchFactor(0, 0)
        self._splitter.setStretchFactor(1, 1)

        editor.status_changed.connect(
            lambda _status, text: self.status_label.setText(text)
        )
        editor.session_changed.connect(self._set_editor_enabled)
        editor.session_changed.connect(self._handle_session_changed)
        self._splitter.splitterMoved.connect(self._handle_splitter_moved)
        editor.draft_dirty_changed.connect(self._handle_draft_dirty_changed)
        editor.card_committed.connect(self._handle_card_committed)
        editor.save_conflict.connect(self.save_conflict.emit)
        editor.find_requested.connect(
            lambda: self.find_bar.show_find(include_replace=False)
        )
        editor.replace_requested.connect(
            lambda: self.find_bar.show_find(include_replace=True)
        )
        editor.find_next_requested.connect(self.find_bar.find_next)
        editor.find_previous_requested.connect(self.find_bar.find_previous)
        self.save_button.clicked.connect(editor.save_current)
        self.cancel_button.clicked.connect(editor.request_close)
        self._set_editor_enabled(False)
        self._handle_session_changed(False)

    def editor_split_sizes(self) -> tuple[int, int] | None:
        """사용자가 조정했거나 복원된 편집 분할 보기 크기를 반환한다.

        (목록, 슬롯) 순서다.
        """
        return self._open_split_sizes

    def set_editor_split_sizes(self, sizes: tuple[int, int] | None) -> None:
        """복원된 문서 UI 상태의 (목록, 슬롯) 분할 크기를 적용한다."""
        self._open_split_sizes = sizes
        self._apply_current_split()

    def bind_card_open_signal(self, source: object) -> None:
        """CardStreamView의 card_open_requested 같은 신호를 연결한다."""
        signal = getattr(source, "card_open_requested", None)
        if signal is None or not hasattr(signal, "connect"):
            raise TypeError("카드 열기 신호를 제공하는 객체가 필요합니다.")
        signal.connect(self.open_card)

    def open_card(self, card_id: str, *, app_driven: bool = False) -> bool:
        """클릭 또는 Enter 요청으로 카드를 연다. 슬롯 전환은 세션 신호가 맡는다."""
        return self.editor.open_card(card_id, app_driven=app_driven)

    def resizeEvent(self, event: QResizeEvent) -> None:
        """첫 표시로 폭이 정해지면 보류해 둔 분할 적용을 마친다."""
        super().resizeEvent(event)
        if self._split_apply_pending:
            self._apply_current_split()

    def showEvent(self, event: QShowEvent) -> None:
        """숨은 동안 지정돼 보류된 분할을 다시 보일 때 적용한다.

        크기가 그대로면 resizeEvent 가 나지 않아 보류가 풀리지 않는다.
        """
        super().showEvent(event)
        if self._split_apply_pending:
            self._apply_current_split()

    def _handle_session_changed(self, has_session: bool) -> None:
        self.status_label.setText(
            self.editor.status_text if has_session else "새 카드를 입력하세요."
        )

    def _apply_current_split(self) -> None:
        """저장된 가로 분할 또는 기본 목록 1:슬롯 2 비율을 적용한다."""
        # 핸들 폭을 빼야 기록(pane 폭 합)과 좌표계가 같아진다 — 섞으면 적용할
        # 때마다 Qt 가 요청치를 비례 축소해 비율이 최소폭 쪽으로 표류한다.
        # 핸들 폭을 빼야 기록(pane 폭 합)과 좌표계가 같아진다 — 섞으면 클램프
        # 상한이 그만큼 어긋나 슬롯이 MIN_SLOT_WIDTH 아래로 내려간다.
        total = self._splitter.width() - self._splitter.handleWidth()
        if total <= 0 or not self.isVisible():
            self._split_apply_pending = True
            return
        self._split_apply_pending = False
        saved = self._open_split_sizes
        requested = saved[0] if saved is not None else total // 3
        list_width = self._clamp_list_width(requested, total)
        sizes = (list_width, total - list_width)
        self._applying_split = True
        try:
            self._splitter.setSizes(list(sizes))
        finally:
            self._applying_split = False

    @staticmethod
    def _clamp_list_width(requested: int, total: int) -> int:
        """목록·슬롯 어느 쪽도 사라지지 않도록 목록 폭을 가둔다.

        접힌 0이나 창보다 큰 저장값을 그대로 적용하면 편집기 또는 카드 목록이
        화면에서 없어지고, 그 값이 다시 영속돼 다음 실행까지 이어진다.
        """
        minimum_total = MIN_LIST_WIDTH + MIN_SLOT_WIDTH
        if total <= minimum_total:
            return total * MIN_LIST_WIDTH // minimum_total
        return min(max(requested, MIN_LIST_WIDTH), total - MIN_SLOT_WIDTH)

    def _handle_splitter_moved(self, _position: int, _index: int) -> None:
        if self._applying_split:
            return
        sizes = self._splitter.sizes()
        if len(sizes) != 2:
            return
        total = sizes[0] + sizes[1]
        if total <= 0:
            return
        list_width = self._clamp_list_width(sizes[0], total)
        self._open_split_sizes = (list_width, total - list_width)

    def _handle_card_committed(self, value: object) -> None:
        if not isinstance(value, Card):
            raise TypeError("확정 카드 신호에는 Card가 필요합니다.")
        model = getattr(self._card_list, "card_model", None)
        if isinstance(model, CardListModel):
            model.update_card(
                value,
                revision_count=len(
                    self.editor._repositories.list_revisions(value.id)
                ),
            )
        self.card_committed.emit(value)

    def _handle_draft_dirty_changed(self, card_id: str, dirty: bool) -> None:
        model = getattr(self._card_list, "card_model", None)
        if isinstance(model, CardListModel):
            model.set_card_dirty(card_id, dirty)

    def _set_editor_enabled(self, enabled: bool) -> None:
        self.save_button.setEnabled(enabled)
        self.cancel_button.setEnabled(enabled)
