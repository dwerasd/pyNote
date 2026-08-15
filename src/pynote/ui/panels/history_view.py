from __future__ import annotations

import html
import logging
from collections.abc import Callable
from concurrent.futures import Future, ThreadPoolExecutor
from datetime import datetime

from PySide6.QtCore import Qt, QTimer, Signal
from PySide6.QtGui import QCloseEvent
from PySide6.QtWidgets import (
    QAbstractItemView,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QMessageBox,
    QPushButton,
    QTabWidget,
    QTextBrowser,
    QVBoxLayout,
    QWidget,
)

from pynote.application.history_service import HistoryService, RestoreResult
from pynote.application.save_coordinator import SaveConflict
from pynote.domain.diffing import CharacterDiff, DiffTag, LineDiff, TextDiff, diff_text
from pynote.domain.events import EditEvent, EventSource, EventType
from pynote.domain.models import CardRevision, RevisionSource

LOGGER = logging.getLogger(__name__)

_ASYNC_DIFF_BYTES = 512_000

_EVENT_NAMES = {
    EventType.CREATE: "생성",
    EventType.UPDATE: "수정",
    EventType.MOVE: "이동",
    EventType.SPLIT: "분할",
    EventType.MERGE: "병합",
    EventType.DELETE: "삭제",
    EventType.RESTORE: "복구",
}
_EVENT_SOURCE_NAMES = {
    EventSource.TYPING: "직접 입력",
    EventSource.PASTE: "붙여넣기",
    EventSource.IMPORT: "가져오기",
    EventSource.MIXED: "혼합",
    EventSource.EDIT: "편집",
    EventSource.RESTORE: "복구",
    EventSource.SYSTEM: "시스템",
}
_REVISION_SOURCE_NAMES = {
    RevisionSource.EDIT: "편집",
    RevisionSource.RESTORE: "복구",
    RevisionSource.SPLIT: "분할",
    RevisionSource.MERGE: "병합",
}


class HistoryView(QWidget):
    """문서 이벤트, 카드 리비전, draft 비교와 복구를 한 화면에 제공한다."""

    card_restored = Signal(object)
    restore_failed = Signal(str)

    def __init__(
        self,
        history_service: HistoryService,
        *,
        destructive_preflight: Callable[[], bool] | None = None,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._history_service = history_service
        self._destructive_preflight = destructive_preflight
        self._document_id: str | None = None
        self._card_id: str | None = None
        self._diff_executor = ThreadPoolExecutor(
            max_workers=1,
            thread_name_prefix="pynote-diff",
        )
        self._pending_diff: tuple[int, Future[TextDiff]] | None = None
        self._diff_generation = 0

        self.setObjectName("historyView")
        layout = QVBoxLayout(self)
        self.notice_label = QLabel(self)
        self.notice_label.setObjectName("historyNotice")
        self.notice_label.setWordWrap(True)
        self.notice_label.hide()
        layout.addWidget(self.notice_label)

        self.role_label = QLabel(
            "실행 취소는 현재 편집 세션만 되돌립니다. "
            "영구 이력 복구는 새 리비전으로 기록됩니다.",
            self,
        )
        self.role_label.setObjectName("historyRoleNotice")
        self.role_label.setWordWrap(True)
        layout.addWidget(self.role_label)

        self.mode_tabs = QTabWidget(self)
        self.mode_tabs.setObjectName("historyModeTabs")
        self.event_list = QListWidget(self.mode_tabs)
        self.event_list.setObjectName("historyEventList")
        self.revision_list = QListWidget(self.mode_tabs)
        self.revision_list.setObjectName("historyRevisionList")
        self.revision_list.setSelectionMode(
            QAbstractItemView.SelectionMode.ExtendedSelection
        )
        self.mode_tabs.addTab(self.event_list, "문서 이벤트")
        self.mode_tabs.addTab(self.revision_list, "카드 리비전")
        layout.addWidget(self.mode_tabs, 1)

        selection_layout = QHBoxLayout()
        self.selection_label = QLabel("이벤트 또는 리비전을 선택하세요.", self)
        self.restore_button = QPushButton("선택 리비전 복구", self)
        self.restore_button.setObjectName("restoreRevisionButton")
        self.restore_button.setEnabled(False)
        selection_layout.addWidget(self.selection_label)
        selection_layout.addStretch(1)
        selection_layout.addWidget(self.restore_button)
        layout.addLayout(selection_layout)

        self.preview_browser = QTextBrowser(self)
        self.preview_browser.setObjectName("historyPreview")
        self.preview_browser.setPlaceholderText("선택한 리비전의 전체 문자열 미리보기")
        self.diff_browser = QTextBrowser(self)
        self.diff_browser.setObjectName("historyDiff")
        self.diff_browser.setPlaceholderText("두 리비전을 선택하면 차이를 표시합니다.")
        layout.addWidget(self.preview_browser, 1)
        layout.addWidget(self.diff_browser, 2)

        self._diff_timer = QTimer(self)
        self._diff_timer.setInterval(20)
        self._diff_timer.timeout.connect(self._poll_diff)
        self.event_list.currentItemChanged.connect(self._event_selected)
        self.revision_list.itemSelectionChanged.connect(self._revision_selection_changed)
        self.revision_list.currentItemChanged.connect(
            lambda _current, _previous: self._revision_selection_changed()
        )
        self.restore_button.clicked.connect(self._confirm_restore)

    def set_document(self, document_id: str) -> None:
        """문서 전체 이벤트 타임라인을 최신순으로 표시한다."""
        self._document_id = document_id
        events = self._history_service.list_document_events(document_id)
        self.event_list.clear()
        for event in events:
            item = QListWidgetItem(self._event_text(event))
            item.setData(Qt.ItemDataRole.UserRole, event)
            self.event_list.addItem(item)

    def set_card(self, card_id: str) -> None:
        """선택 카드의 영구 리비전 목록과 현재 미리보기를 표시한다."""
        self._card_id = card_id
        revisions = self._history_service.list_card_revisions(card_id)
        self.revision_list.clear()
        for index, revision in enumerate(revisions):
            marker = "현재 · " if index == 0 else ""
            item = QListWidgetItem(
                f"{marker}r{revision.event_seq} · "
                f"{_REVISION_SOURCE_NAMES[revision.source]} · "
                f"{self._format_time(revision.created_at_us)}"
            )
            item.setData(Qt.ItemDataRole.UserRole, revision)
            self.revision_list.addItem(item)
        if self.revision_list.count():
            self.revision_list.setCurrentRow(0)

    def set_pending_card(self, card_id: str) -> None:
        """화면이 숨어 있는 동안 바뀐 선택 카드를 기록만 해 둔다."""
        self._card_id = card_id

    def refresh(self) -> None:
        """현재 문서와 카드의 이력을 DB 확정 상태로 다시 읽는다."""
        if self._document_id is not None:
            self.set_document(self._document_id)
        if self._card_id is not None:
            self.set_card(self._card_id)

    def bind_save_conflict_source(self, source: object) -> None:
        """CardEditor 계열의 save_conflict 신호를 이 비교 화면에 연결한다."""
        signal = getattr(source, "save_conflict", None)
        if signal is None or not hasattr(signal, "connect"):
            raise TypeError("save_conflict 신호를 제공하는 객체가 필요합니다.")
        signal.connect(self.show_save_conflict)

    def show_save_conflict(self, value: object) -> None:
        """base revision 불일치를 전용 화면 없이 확정본/draft 비교로 표시한다."""
        if not isinstance(value, SaveConflict):
            raise TypeError("저장 충돌 비교에는 SaveConflict가 필요합니다.")
        if self._card_id != value.card_id:
            self.set_card(value.card_id)
        self.mode_tabs.setCurrentWidget(self.revision_list)
        self.notice_label.setText(
            "기준 리비전이 현재 리비전과 달라 저장을 중단했습니다. "
            "아래에서 현재 확정본과 recovery draft를 비교하세요."
        )
        self.notice_label.show()
        self.selection_label.setText(
            f"draft 기준: {value.base_revision_id or '없음'} · "
            f"현재: {value.current_revision_id or '없음'}"
        )
        self.preview_browser.setPlainText(
            f"[draft 기준 리비전]\n{value.base_text}\n\n"
            f"[현재 확정본]\n{value.committed_text}"
        )
        self.restore_button.setEnabled(False)
        self._request_diff(value.committed_text, value.draft_text)

    def restore_selected(self) -> RestoreResult:
        """현재 선택한 과거 리비전을 새 restore 리비전으로 확정한다."""
        if self._card_id is None:
            raise RuntimeError("복구할 카드가 선택되지 않았습니다.")
        revision = self._current_revision()
        if revision is None:
            raise RuntimeError("복구할 리비전이 선택되지 않았습니다.")
        if (
            self._destructive_preflight is not None
            and not self._destructive_preflight()
        ):
            raise RuntimeError("계속 편집을 선택해 이력 복구를 취소했습니다.")
        result = self._history_service.restore(self._card_id, revision.id)
        self.notice_label.hide()
        self.refresh()
        self.card_restored.emit(result.card)
        return result

    def closeEvent(self, event: QCloseEvent) -> None:
        """닫힐 때 대기 중 diff 작업을 취소하고 worker를 정리한다."""
        self._diff_timer.stop()
        self._diff_executor.shutdown(wait=False, cancel_futures=True)
        super().closeEvent(event)

    def _revision_selection_changed(self) -> None:
        revisions = [
            revision
            for item in self.revision_list.selectedItems()
            if isinstance(
                revision := item.data(Qt.ItemDataRole.UserRole),
                CardRevision,
            )
        ]
        current = self._current_revision()
        if current is not None:
            self.preview_browser.setPlainText(current.body)
        current_revision = (
            None
            if self._card_id is None
            else self._history_service.list_card_revisions(self._card_id)[0]
        )
        self.restore_button.setEnabled(
            current is not None
            and current_revision is not None
            and current.id != current_revision.id
        )

        if len(revisions) != 2:
            self.selection_label.setText(
                "리비전 두 개를 선택하면 줄·글자 단위 차이를 표시합니다."
            )
            self.diff_browser.clear()
            return
        before, after = sorted(revisions, key=lambda revision: revision.event_seq)
        self.selection_label.setText(f"r{before.event_seq} → r{after.event_seq}")
        self._request_diff(before.body, after.body)

    def _event_selected(
        self,
        current: QListWidgetItem | None,
        _previous: QListWidgetItem | None,
    ) -> None:
        if current is None:
            return
        event = current.data(Qt.ItemDataRole.UserRole)
        if not isinstance(event, EditEvent):
            raise TypeError("이벤트 목록 항목에 EditEvent가 없습니다.")
        self.selection_label.setText(self._event_text(event))
        if event.card_id is None:
            self.preview_browser.setPlainText(event.details_json)
            self.diff_browser.clear()
            return
        revisions = self._history_service.list_card_revisions(event.card_id)
        revision = next(
            (
                candidate
                for candidate in revisions
                if candidate.event_seq == event.event_seq
            ),
            None,
        )
        if revision is None:
            self.preview_browser.setPlainText(event.details_json)
            self.diff_browser.clear()
            return
        self.preview_browser.setPlainText(revision.body)
        parent = (
            None
            if revision.parent_revision_id is None
            else self._history_service.get_revision(revision.parent_revision_id)
        )
        self._request_diff("" if parent is None else parent.body, revision.body)

    def _request_diff(self, before: str, after: str) -> None:
        self._diff_generation += 1
        generation = self._diff_generation
        if len(before.encode("utf-8")) + len(after.encode("utf-8")) < _ASYNC_DIFF_BYTES:
            self._show_diff(diff_text(before, after))
            return
        self.diff_browser.setPlainText("대용량 차이를 계산하는 중…")
        future = self._diff_executor.submit(diff_text, before, after)
        self._pending_diff = (generation, future)
        self._diff_timer.start()

    def _poll_diff(self) -> None:
        pending = self._pending_diff
        if pending is None:
            return
        generation, future = pending
        if not future.done():
            return
        self._pending_diff = None
        self._diff_timer.stop()
        if generation != self._diff_generation:
            return
        try:
            result = future.result()
        except BaseException as error:
            LOGGER.exception("대용량 리비전 diff 계산에 실패했습니다.")
            self.diff_browser.setPlainText(f"차이 계산 실패: {error}")
            return
        self._show_diff(result)

    def _show_diff(self, result: TextDiff) -> None:
        self.diff_browser.setHtml(self._diff_html(result))

    def _confirm_restore(self) -> None:
        answer = QMessageBox.question(
            self,
            "과거 리비전 복구",
            "현재 상태를 보존하고 선택한 본문을 새 리비전으로 복구할까요?",
        )
        if answer is not QMessageBox.StandardButton.Yes:
            return
        try:
            self.restore_selected()
        except BaseException as error:
            LOGGER.exception("이력 화면에서 카드 복구에 실패했습니다.")
            self.restore_failed.emit(str(error))
            QMessageBox.critical(self, "복구 실패", str(error))

    def _current_revision(self) -> CardRevision | None:
        item = self.revision_list.currentItem()
        if item is None:
            return None
        value = item.data(Qt.ItemDataRole.UserRole)
        return value if isinstance(value, CardRevision) else None

    @staticmethod
    def _event_text(event: EditEvent) -> str:
        card_text = "문서" if event.card_id is None else f"카드 {event.card_id}"
        return (
            f"{HistoryView._format_time(event.occurred_at_us)}  "
            f"{card_text} {_EVENT_NAMES[event.event_type]} · "
            f"{_EVENT_SOURCE_NAMES[event.source]}"
        )

    @staticmethod
    def _format_time(epoch_us: int) -> str:
        return datetime.fromtimestamp(epoch_us / 1_000_000).strftime(
            "%Y-%m-%d %H:%M:%S"
        )

    @staticmethod
    def _diff_html(result: TextDiff) -> str:
        rows: list[str] = [
            "<html><body><pre style='white-space:pre-wrap; font-family:monospace'>"
        ]
        for line in result.lines:
            rows.extend(HistoryView._line_html(line))
        rows.append("</pre></body></html>")
        return "".join(rows)

    @staticmethod
    def _line_html(line: LineDiff) -> tuple[str, ...]:
        if line.tag is DiffTag.EQUAL:
            return (f"  {html.escape(line.after)}",)
        if line.tag is DiffTag.DELETE:
            return (
                "<span style='background:#ffd7d5;color:#8b0000'>- "
                f"{html.escape(line.before)}</span>",
            )
        if line.tag is DiffTag.INSERT:
            return (
                "<span style='background:#d8f5d0;color:#145a14'>+ "
                f"{html.escape(line.after)}</span>",
            )
        return (
            "- " + HistoryView._character_html(line.characters, before=True),
            "+ " + HistoryView._character_html(line.characters, before=False),
        )

    @staticmethod
    def _character_html(
        characters: tuple[CharacterDiff, ...],
        *,
        before: bool,
    ) -> str:
        pieces: list[str] = []
        for change in characters:
            text = change.before if before else change.after
            if not text:
                continue
            escaped = html.escape(text)
            changed = (
                change.tag in {DiffTag.DELETE, DiffTag.REPLACE}
                if before
                else change.tag in {DiffTag.INSERT, DiffTag.REPLACE}
            )
            if changed:
                color = "#ffd7d5" if before else "#d8f5d0"
                pieces.append(f"<span style='background:{color}'>{escaped}</span>")
            else:
                pieces.append(escaped)
        return "".join(pieces)
