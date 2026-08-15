from __future__ import annotations

from collections.abc import Iterator

import pytest
from pytestqt.qtbot import QtBot

import pynote.ui.panels.history_view as history_view_module
from pynote.application.draft_coordinator import DraftCoordinator
from pynote.application.history_service import HistoryService
from pynote.application.save_coordinator import SaveConflict, SaveCoordinator
from pynote.domain.events import EventSource
from pynote.domain.models import (
    CaptureOperationSource,
    Card,
    CardSource,
    Document,
    NewCaptureOperation,
    NewCard,
    RevisionSource,
    SplitPolicy,
)
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories
from pynote.ui.document_page import DocumentPage
from pynote.ui.editor.card_editor import CardEditor, CloseChoice
from pynote.ui.panels.history_view import HistoryView


def _ids(prefix: str) -> Iterator[str]:
    number = 0
    while True:
        number += 1
        yield f"{prefix}-{number}"


def _create_card(repositories: Repositories) -> Card:
    repositories.create_document(
        Document(
            id="document-ui-history",
            title="이력 UI",
            created_at_us=1_000,
            updated_at_us=1_000,
        )
    )
    return repositories.create_cards(
        NewCaptureOperation(
            id="operation-ui-history",
            document_id="document-ui-history",
            source=CaptureOperationSource.TYPING,
            split_policy=SplitPolicy.KEEP,
            original_text=None,
            created_at_us=2_000,
        ),
        [
            NewCard(
                id="card-ui-history",
                revision_id="revision-ui-initial",
                event_id="event-ui-initial",
                position_key=1_024,
                body="첫 줄\n예전 문장",
                card_source=CardSource.TYPING,
                event_source=EventSource.TYPING,
                revision_source=RevisionSource.EDIT,
                created_at_us=2_000,
            )
        ],
    )[0]


def _save_changed_card(
    database: Database,
    repositories: Repositories,
    card: Card,
) -> Card:
    draft = DraftCoordinator(
        database,
        repositories,
        id_factory=lambda: "draft-ui-history",
    )
    identifiers = _ids("save-ui-history")
    save = SaveCoordinator(
        database,
        draft,
        repositories,
        clock=lambda: 3_000,
        id_factory=lambda: next(identifiers),
    )
    session = draft.open_card(card)
    assert session is not None
    draft.update_session(
        session.draft_id,
        text="첫 줄\n새 문장",
        cursor_position_qchar=0,
    )
    return save.save(session).card


def test_history_view_switches_timeline_and_revision_diff_and_restores(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
) -> None:
    initial = _create_card(repositories)
    changed = _save_changed_card(database, repositories, initial)
    identifiers = _ids("restore-ui")
    history = HistoryService(
        database,
        repositories,
        clock=lambda: 4_000,
        id_factory=lambda: next(identifiers),
    )
    view = HistoryView(history)
    qtbot.addWidget(view)
    view.set_document(initial.document_id)
    view.set_card(initial.id)

    assert view.mode_tabs.tabText(0) == "문서 이벤트"
    assert view.mode_tabs.tabText(1) == "카드 리비전"
    assert view.event_list.count() == 2
    assert view.revision_list.count() == 2
    assert "실행 취소" in view.role_label.text()
    assert "영구 이력" in view.role_label.text()

    current_item = view.revision_list.item(0)
    previous_item = view.revision_list.item(1)
    current_item.setSelected(True)
    previous_item.setSelected(True)
    assert "- 예전 문장" in view.diff_browser.toPlainText()
    assert "+ 새 문장" in view.diff_browser.toPlainText()

    view.revision_list.clearSelection()
    previous_item.setSelected(True)
    view.revision_list.setCurrentItem(previous_item)
    assert view.restore_button.isEnabled()
    result = view.restore_selected()

    assert result.revision.parent_revision_id == changed.current_revision_id
    assert result.card.body == "첫 줄\n예전 문장"
    assert view.revision_list.count() == 3


def test_history_restore_stops_when_destructive_preflight_keeps_editing(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    initial = _create_card(repositories)
    _save_changed_card(database, repositories, initial)
    page: DocumentPage
    page = DocumentPage(
        database,
        repositories,
        initial.document_id,
        destructive_preflight=lambda _document_id: page.can_leave_editor(
            protect_now=True
        ),
    )
    qtbot.addWidget(page)
    assert page.open_card(initial.id)
    page.editor.setPlainText("계속 편집할 미저장 본문")
    monkeypatch.setattr(page.editor, "save_current", lambda: False)
    monkeypatch.setattr(
        page.editor,
        "_ask_close_choice",
        lambda: CloseChoice.KEEP_EDITING,
    )
    page.history.set_card(initial.id)
    previous_item = page.history.revision_list.item(1)
    assert previous_item is not None
    page.history.revision_list.clearSelection()
    previous_item.setSelected(True)
    page.history.revision_list.setCurrentItem(previous_item)
    revision_count = len(repositories.list_revisions(initial.id))

    with pytest.raises(RuntimeError, match="취소"):
        page.history.restore_selected()

    assert len(repositories.list_revisions(initial.id)) == revision_count


def test_history_restore_auto_saves_dirty_editor_before_restore(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    initial = _create_card(repositories)
    _save_changed_card(database, repositories, initial)
    page: DocumentPage
    page = DocumentPage(
        database,
        repositories,
        initial.document_id,
        destructive_preflight=lambda _document_id: page.can_leave_editor(
            protect_now=True
        ),
    )
    qtbot.addWidget(page)
    assert page.open_card(initial.id)
    page.editor.setPlainText("이력 복원 전에 자동 저장할 본문")
    page.history.set_card(initial.id)
    previous_item = page.history.revision_list.item(1)
    assert previous_item is not None
    page.history.revision_list.clearSelection()
    previous_item.setSelected(True)
    page.history.revision_list.setCurrentItem(previous_item)
    revision_count = len(repositories.list_revisions(initial.id))

    def fail_close_dialog() -> CloseChoice:
        raise AssertionError("정상 이력 복원 preflight에서 닫기 선택 대화가 호출됨")

    monkeypatch.setattr(page.editor, "_ask_close_choice", fail_close_dialog)

    result = page.history.restore_selected()

    revisions = repositories.list_revisions(initial.id)
    assert len(revisions) == revision_count + 2
    assert any(
        revision.body == "이력 복원 전에 자동 저장할 본문"
        for revision in revisions
    )
    assert result.card.body == initial.body
    assert result.revision.parent_revision_id is not None
    assert page.history.revision_list.count() == revision_count + 2


def test_save_conflict_signal_uses_same_committed_draft_comparison_area(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(history_view_module, "_ASYNC_DIFF_BYTES", 1)
    card = _create_card(repositories)
    draft = DraftCoordinator(
        database,
        repositories,
        id_factory=lambda: "draft-conflict-ui",
    )
    save = SaveCoordinator(database, draft, repositories)
    editor = CardEditor(repositories, draft, save)
    history = HistoryService(database, repositories)
    view = HistoryView(history)
    qtbot.addWidget(editor)
    qtbot.addWidget(view)
    view.set_document(card.document_id)
    view.set_card(card.id)
    view.bind_save_conflict_source(editor)
    conflict = SaveConflict(
        card_id=card.id,
        base_revision_id="revision-ui-initial",
        current_revision_id="revision-newer",
        base_text="첫 줄\n예전 문장",
        committed_text="현재 확정본",
        draft_text="잔존 draft",
    )

    editor.save_conflict.emit(conflict)
    qtbot.waitUntil(
        lambda: "+ 잔존 draft" in view.diff_browser.toPlainText(),
        timeout=2_000,
    )

    assert view.notice_label.isVisibleTo(view)
    assert "저장을 중단" in view.notice_label.text()
    assert "[현재 확정본]" in view.preview_browser.toPlainText()
    assert "- 현재 확정본" in view.diff_browser.toPlainText()
    assert "+ 잔존 draft" in view.diff_browser.toPlainText()
    assert view.mode_tabs.currentWidget() is view.revision_list
    assert not view.restore_button.isEnabled()
