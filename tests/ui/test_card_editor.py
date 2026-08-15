from __future__ import annotations

from collections.abc import Iterator

from PySide6.QtCore import QEvent, QObject, Qt, Signal
from PySide6.QtGui import QInputMethodEvent, QKeyEvent
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication
from pytest import MonkeyPatch
from pytestqt.qtbot import QtBot

from pynote.application.draft_coordinator import (
    DraftCoordinator,
    DraftDisposition,
)
from pynote.application.save_coordinator import SaveCoordinator
from pynote.domain.events import EventSource
from pynote.domain.models import (
    CaptureOperationSource,
    Card,
    CardSource,
    Document,
    Draft,
    DraftKind,
    NewCaptureOperation,
    NewCard,
    RevisionSource,
    SplitPolicy,
)
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories, text_hash
from pynote.ui.cards.card_model import CardListModel, CardRole
from pynote.ui.cards.card_stream import CardStreamView
from pynote.ui.document_page import DocumentPage
from pynote.ui.editor.card_editor import (
    CardEditor,
    CardEditorWorkspace,
    CloseChoice,
    EditorStatus,
)


def _ids(prefix: str) -> Iterator[str]:
    number = 0
    while True:
        number += 1
        yield f"{prefix}-{number}"


def _create_card(repositories: Repositories, body: str = "기존 본문") -> Card:
    repositories.create_document(
        Document(
            id="document-1",
            title="편집기 테스트",
            created_at_us=1_000,
            updated_at_us=1_000,
        )
    )
    return repositories.create_cards(
        NewCaptureOperation(
            id="operation-1",
            document_id="document-1",
            source=CaptureOperationSource.TYPING,
            split_policy=SplitPolicy.KEEP,
            original_text=None,
            created_at_us=2_000,
        ),
        [
            NewCard(
                id="card-1",
                revision_id="revision-1",
                event_id="event-1",
                position_key=1_024,
                body=body,
                card_source=CardSource.TYPING,
                event_source=EventSource.TYPING,
                revision_source=RevisionSource.EDIT,
                created_at_us=2_000,
            )
        ],
    )[0]


def _editor(
    database: Database,
    repositories: Repositories,
    *,
    draft_id: str = "draft-ui",
) -> tuple[CardEditor, DraftCoordinator]:
    draft = DraftCoordinator(
        database,
        repositories,
        clock=lambda: 3_000,
        id_factory=lambda: draft_id,
    )
    identifiers = _ids("ui-save")
    save = SaveCoordinator(
        database,
        draft,
        repositories,
        clock=lambda: 4_000,
        id_factory=lambda: next(identifiers),
    )
    return CardEditor(repositories, draft, save), draft


def test_editor_keeps_committed_card_unchanged_until_ctrl_s(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
) -> None:
    card = _create_card(repositories)
    editor, _draft = _editor(database, repositories)
    qtbot.addWidget(editor)
    editor.show()
    committed: list[Card] = []
    editor.card_committed.connect(committed.append)
    assert editor.open_card(card.id)

    editor.selectAll()
    QTest.keyClicks(editor, "changed")

    assert repositories.get_card(card.id) == card
    assert committed == []
    assert editor.status is EditorStatus.EDITING
    QTest.keyClick(editor, Qt.Key.Key_S, Qt.KeyboardModifier.ControlModifier)

    stored = repositories.get_card(card.id)
    assert stored is not None
    assert stored.body == "changed"
    assert committed == [stored]
    assert editor.status is EditorStatus.SAVED


def test_ime_preedit_cannot_become_permanent_revision(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
) -> None:
    card = _create_card(repositories, body="")
    editor, draft = _editor(database, repositories)
    qtbot.addWidget(editor)
    editor.show()
    assert editor.open_card(card.id)
    preedit = QInputMethodEvent("ㅎ", [])
    QApplication.sendEvent(editor, preedit)

    assert editor.save_current() is False
    assert repositories.get_card(card.id) == card
    assert len(repositories.list_revisions(card.id)) == 1
    assert editor.session is not None
    assert draft.is_ime_composing(editor.session.draft_id)

    commit = QInputMethodEvent()
    commit.setCommitString("한")
    QApplication.sendEvent(editor, commit)
    assert editor.save_current() is True
    stored = repositories.get_card(card.id)
    assert stored is not None
    assert stored.body == "한"
    assert len(repositories.list_revisions(card.id)) == 2


def test_recovered_cursor_is_clamped_with_qtextcursor_position(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
) -> None:
    card = _create_card(repositories, body="A🧭B")
    repositories.create_draft(
        Draft(
            id="draft-cursor",
            document_id=card.document_id,
            card_id=card.id,
            draft_kind=DraftKind.EDIT,
            base_revision_id=card.current_revision_id,
            draft_text="A🧭B + 초안",
            draft_hash=text_hash("A🧭B + 초안"),
            cursor_position_qchar=999,
            updated_at_us=3_000,
        )
    )
    editor, _draft = _editor(database, repositories, draft_id="unused")
    qtbot.addWidget(editor)

    assert editor.open_card(card.id, disposition=DraftDisposition.RECOVER)

    assert editor.textCursor().position() == editor.document().characterCount() - 1


def test_find_replace_wrap_font_zoom_and_dirty_close(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
) -> None:
    card = _create_card(repositories, body="alpha beta alpha")
    editor, _draft = _editor(database, repositories)
    qtbot.addWidget(editor)
    editor.show()
    assert editor.open_card(card.id)

    assert editor.find_text("beta")
    assert editor.textCursor().selectedText() == "beta"
    assert editor.replace_one("beta", "감마")
    assert editor.replace_all("alpha", "A") == 2
    editor.set_wrap_visible(False)
    assert editor.lineWrapMode() is editor.LineWrapMode.NoWrap
    old_size = editor.font().pointSizeF()
    QTest.keyClick(editor, Qt.Key.Key_Plus, Qt.KeyboardModifier.ControlModifier)
    assert editor.font().pointSizeF() > old_size

    assert editor.request_close(
        choice_provider=lambda _session: CloseChoice.KEEP_EDITING
    ) is False
    assert editor.request_close(
        choice_provider=lambda _session: CloseChoice.DISCARD
    ) is True
    assert repositories.get_card(card.id) == card
    assert editor.session is None
    assert editor.toPlainText() == ""
    assert not editor.isReadOnly()


def test_escape_with_selection_clears_only_selection(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
) -> None:
    card = _create_card(repositories, body="alpha beta")
    editor, _draft = _editor(database, repositories)
    qtbot.addWidget(editor)
    editor.show()
    assert editor.open_card(card.id)
    editor.selectAll()

    # 선택이 있으면 ShortcutOverride 를 수락해 back_action 발동을 막는다.
    override = QKeyEvent(
        QEvent.Type.ShortcutOverride,
        Qt.Key.Key_Escape,
        Qt.KeyboardModifier.NoModifier,
    )
    override.ignore()
    QApplication.sendEvent(editor, override)
    assert override.isAccepted()

    QTest.keyClick(editor, Qt.Key.Key_Escape)
    assert not editor.textCursor().hasSelection()
    assert editor.textCursor().position() == len("alpha beta")
    assert editor.session is not None
    assert editor.toPlainText() == "alpha beta"

    # 선택이 없으면 ESC 소유권은 back_action 에 남는다.
    override = QKeyEvent(
        QEvent.Type.ShortcutOverride,
        Qt.Key.Key_Escape,
        Qt.KeyboardModifier.NoModifier,
    )
    override.ignore()
    QApplication.sendEvent(editor, override)
    assert not override.isAccepted()


def test_f3_repeats_search_like_find_next(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
) -> None:
    card = _create_card(repositories, body="alpha beta alpha")
    editor, _draft = _editor(database, repositories)
    workspace = CardEditorWorkspace(CardStreamView(), editor)
    qtbot.addWidget(workspace)
    workspace.resize(900, 500)
    workspace.show()
    assert workspace.open_card(card.id)
    find_bar = workspace.find_bar

    # 검색어가 없으면 F3 는 찾기 막대를 연다.
    QTest.keyClick(editor, Qt.Key.Key_F3)
    assert find_bar.isVisible()
    assert not find_bar.replace_input.isVisible()

    find_bar.find_input.setText("alpha")
    QTest.keyClick(editor, Qt.Key.Key_F3)
    assert editor.textCursor().selectedText() == "alpha"
    assert editor.textCursor().selectionStart() == 0

    QTest.keyClick(editor, Qt.Key.Key_F3)
    assert editor.textCursor().selectionStart() == 11

    QTest.keyClick(editor, Qt.Key.Key_F3, Qt.KeyboardModifier.ShiftModifier)
    assert editor.textCursor().selectionStart() == 0

    # 찾기 막대 안에서 눌러도 같은 다음 찾기가 이어진다.
    QTest.keyClick(find_bar.find_input, Qt.Key.Key_F3)
    assert editor.textCursor().selectionStart() == 11


def test_default_close_auto_saves_without_close_dialog(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
) -> None:
    card = _create_card(repositories)
    editor, _draft = _editor(database, repositories)
    qtbot.addWidget(editor)
    assert editor.open_card(card.id)
    editor.setPlainText("대화 없이 자동 저장할 본문")
    revision_count = len(repositories.list_revisions(card.id))

    def fail_close_dialog() -> CloseChoice:
        raise AssertionError("정상 자동저장 경로에서 닫기 선택 대화가 호출됨")

    monkeypatch.setattr(editor, "_ask_close_choice", fail_close_dialog)

    assert editor.request_close()
    assert editor.session is None
    stored = repositories.get_card(card.id)
    assert stored is not None
    assert stored.body == "대화 없이 자동 저장할 본문"
    assert len(repositories.list_revisions(card.id)) == revision_count + 1


def test_choice_provider_preserves_save_discard_and_keep_editing(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
) -> None:
    card = _create_card(repositories)
    editor, _draft = _editor(database, repositories)
    qtbot.addWidget(editor)
    assert editor.open_card(card.id)
    editor.setPlainText("명시 SAVE 본문")
    revision_count = len(repositories.list_revisions(card.id))

    assert editor.request_close(
        choice_provider=lambda _session: CloseChoice.SAVE
    )
    saved = repositories.get_card(card.id)
    assert saved is not None
    assert saved.body == "명시 SAVE 본문"
    assert len(repositories.list_revisions(card.id)) == revision_count + 1

    assert editor.open_card(card.id)
    editor.setPlainText("명시 DISCARD 본문")
    assert editor.request_close(
        choice_provider=lambda _session: CloseChoice.DISCARD
    )
    discarded = repositories.get_card(card.id)
    assert discarded == saved

    assert editor.open_card(card.id)
    editor.setPlainText("명시 KEEP_EDITING 본문")
    assert not editor.request_close(
        choice_provider=lambda _session: CloseChoice.KEEP_EDITING
    )
    assert editor.session is not None
    assert editor.toPlainText() == "명시 KEEP_EDITING 본문"
    assert editor.request_close(
        choice_provider=lambda _session: CloseChoice.DISCARD
    )


def test_leave_asks_once_only_after_protection_succeeds_and_save_fails(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
) -> None:
    card = _create_card(repositories)
    editor, _draft = _editor(database, repositories)
    qtbot.addWidget(editor)
    assert editor.open_card(card.id)
    editor.setPlainText("보호 뒤 저장에 실패할 본문")
    session = editor.session
    assert session is not None
    ask_calls: list[bool] = []
    monkeypatch.setattr(editor, "save_current", lambda: False)
    monkeypatch.setattr(
        editor,
        "_ask_close_choice",
        lambda: ask_calls.append(True) or CloseChoice.KEEP_EDITING,
    )

    assert not editor.can_leave_editor(protect_now=True)
    assert ask_calls == [True]
    protected = repositories.get_draft(session.draft_id)
    assert protected is not None
    assert protected.draft_text == "보호 뒤 저장에 실패할 본문"
    assert editor.session is session


def test_leave_rejects_without_dialog_when_ime_protection_fails(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
) -> None:
    card = _create_card(repositories)
    editor, draft = _editor(database, repositories)
    qtbot.addWidget(editor)
    assert editor.open_card(card.id)
    editor.setPlainText("보호할 수 없는 본문")
    QApplication.sendEvent(editor, QInputMethodEvent("ㅎ", []))
    session = editor.session
    assert session is not None
    assert draft.is_ime_composing(session.draft_id)
    ask_calls: list[bool] = []
    save_calls: list[bool] = []
    monkeypatch.setattr(
        editor,
        "save_current",
        lambda: save_calls.append(True) or False,
    )
    monkeypatch.setattr(
        editor,
        "_ask_close_choice",
        lambda: ask_calls.append(True) or CloseChoice.DISCARD,
    )

    assert not editor.can_leave_editor(protect_now=True)
    assert save_calls == []
    assert ask_calls == []
    assert editor.session is not None
    assert editor.status is EditorStatus.SAVE_FAILED

    QApplication.sendEvent(editor, QInputMethodEvent())
    assert editor.request_close(
        choice_provider=lambda _session: CloseChoice.DISCARD
    )


def test_leave_rejects_without_save_or_dialog_when_protection_write_fails(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
) -> None:
    card = _create_card(repositories)
    editor, draft = _editor(database, repositories)
    qtbot.addWidget(editor)
    assert editor.open_card(card.id)
    editor.setPlainText("보호 쓰기에 실패할 본문")
    session = editor.session
    assert session is not None
    assert session.dirty
    save_calls: list[bool] = []
    ask_calls: list[bool] = []

    def fail_protection(_draft_id: str) -> None:
        raise RuntimeError("주입된 recovery 쓰기 실패")

    monkeypatch.setattr(draft, "protect_now", fail_protection)
    monkeypatch.setattr(
        editor,
        "save_current",
        lambda: save_calls.append(True) or False,
    )
    monkeypatch.setattr(
        editor,
        "_ask_close_choice",
        lambda: ask_calls.append(True) or CloseChoice.DISCARD,
    )

    assert not editor.can_leave_editor(protect_now=True)
    assert editor.session is session
    assert session.dirty
    assert editor.status is EditorStatus.SAVE_FAILED
    assert "주입된 recovery 쓰기 실패" in editor.status_text
    assert save_calls == []
    assert ask_calls == []

    assert editor.can_leave_editor(
        choice_provider=lambda _session: CloseChoice.DISCARD,
    )


def test_idle_draft_protection_does_not_create_revision_until_leave(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
) -> None:
    card = _create_card(repositories)
    editor, draft = _editor(database, repositories)
    qtbot.addWidget(editor)
    draft.set_idle_ms(10)
    assert editor.open_card(card.id)
    revision_count = len(repositories.list_revisions(card.id))

    with qtbot.waitSignal(draft.draft_protected, timeout=2_000):
        editor.setPlainText("유휴 보호 뒤 이탈할 본문")

    session = editor.session
    assert session is not None
    protected = repositories.get_draft(session.draft_id)
    assert protected is not None
    assert protected.draft_text == "유휴 보호 뒤 이탈할 본문"
    assert len(repositories.list_revisions(card.id)) == revision_count
    assert session.dirty

    def fail_close_dialog() -> CloseChoice:
        raise AssertionError("정상 이탈 자동저장에서 닫기 선택 대화가 호출됨")

    monkeypatch.setattr(editor, "_ask_close_choice", fail_close_dialog)

    assert editor.request_close()
    stored = repositories.get_card(card.id)
    assert stored is not None
    assert stored.body == "유휴 보호 뒤 이탈할 본문"
    assert len(repositories.list_revisions(card.id)) == revision_count + 1
    assert editor.session is None


def test_save_failed_status_survives_later_draft_protected_signal(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
) -> None:
    card = _create_card(repositories)
    editor, draft = _editor(database, repositories)
    qtbot.addWidget(editor)
    assert editor.open_card(card.id)
    editor.setPlainText("저장 실패 상태를 유지할 본문")
    session = editor.session
    assert session is not None

    def fail_save(_session: object) -> object:
        raise RuntimeError("주입된 저장 실패")

    monkeypatch.setattr(editor._save_coordinator, "save", fail_save)
    monkeypatch.setattr(
        editor,
        "_ask_close_choice",
        lambda: CloseChoice.KEEP_EDITING,
    )

    assert not editor.can_leave_editor()
    assert editor.status is EditorStatus.SAVE_FAILED
    failed_text = editor.status_text

    draft.draft_protected.emit(session.draft_id, 5_000, 0.0)

    assert editor.status is EditorStatus.SAVE_FAILED
    assert editor.status_text == failed_text


def test_card_switch_uses_leave_gate_and_protects_before_choice(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
) -> None:
    first = _create_card(repositories)
    second = repositories.create_cards(
        NewCaptureOperation(
            id="operation-2",
            document_id=first.document_id,
            source=CaptureOperationSource.TYPING,
            split_policy=SplitPolicy.KEEP,
            original_text=None,
            created_at_us=3_000,
        ),
        [
            NewCard(
                id="card-2",
                revision_id="revision-2",
                event_id="event-2",
                position_key=2_048,
                body="둘째 본문",
                card_source=CardSource.TYPING,
                event_source=EventSource.TYPING,
                revision_source=RevisionSource.EDIT,
                created_at_us=3_000,
            )
        ],
    )[0]
    editor, _draft = _editor(database, repositories)
    qtbot.addWidget(editor)
    assert editor.open_card(first.id)
    editor.setPlainText("미저장 변경")

    monkeypatch.setattr(editor, "save_current", lambda: False)
    monkeypatch.setattr(
        editor,
        "_ask_close_choice",
        lambda: CloseChoice.KEEP_EDITING,
    )
    assert not editor.open_card(second.id)
    assert editor.session is not None
    assert editor.session.card_id == first.id
    assert repositories.get_draft(editor.session.draft_id) is not None

    monkeypatch.setattr(
        editor,
        "_ask_close_choice",
        lambda: CloseChoice.DISCARD,
    )
    assert editor.open_card(second.id)
    assert editor.session is not None
    assert editor.session.card_id == second.id
    assert editor.toPlainText() == second.body


def test_card_switch_auto_saves_revision_before_opening_next_card(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
) -> None:
    first = _create_card(repositories)
    second = repositories.create_cards(
        NewCaptureOperation(
            id="operation-2",
            document_id=first.document_id,
            source=CaptureOperationSource.TYPING,
            split_policy=SplitPolicy.KEEP,
            original_text=None,
            created_at_us=3_000,
        ),
        [
            NewCard(
                id="card-2",
                revision_id="revision-2",
                event_id="event-2",
                position_key=2_048,
                body="둘째 본문",
                card_source=CardSource.TYPING,
                event_source=EventSource.TYPING,
                revision_source=RevisionSource.EDIT,
                created_at_us=3_000,
            )
        ],
    )[0]
    editor, _draft = _editor(database, repositories)
    qtbot.addWidget(editor)
    assert editor.open_card(first.id)
    editor.setPlainText("전환 전에 자동 저장할 본문")
    revision_count = len(repositories.list_revisions(first.id))

    assert editor.open_card(second.id)

    stored = repositories.get_card(first.id)
    assert stored is not None
    assert stored.body == "전환 전에 자동 저장할 본문"
    assert len(repositories.list_revisions(first.id)) == revision_count + 1
    assert editor.session is not None
    assert editor.session.card_id == second.id


class _OpenSource(QObject):
    card_open_requested = Signal(str)


def test_workspace_keeps_one_editable_surface_without_input_stack(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
) -> None:
    card = _create_card(repositories)
    editor, _draft = _editor(database, repositories)
    list_pane = CardStreamView()
    workspace = CardEditorWorkspace(list_pane, editor)
    source = _OpenSource()
    workspace.bind_card_open_signal(source)
    qtbot.addWidget(workspace)
    workspace.resize(600, 500)
    workspace.show()
    qtbot.wait(20)
    # 빈 상태와 연결 상태 모두 같은 편집면과 목록을 쓴다.
    assert workspace.findChild(QObject, "cardEditorInputStack") is None
    assert editor.isVisible()
    assert not editor.isReadOnly()
    assert not workspace.save_button.isEnabled()
    assert list_pane.isVisible()

    source.card_open_requested.emit(card.id)
    assert editor.session is not None
    assert editor.session.card_id == card.id
    assert editor.isVisible()
    assert workspace.save_button.isEnabled()
    assert list_pane.isVisible()

    workspace.resize(1_000, 500)
    qtbot.wait(20)
    assert editor.isVisible()

    assert editor.request_close()
    assert editor.isVisible()
    assert not workspace.save_button.isEnabled()
    assert list_pane.isVisible()


def test_back_button_closes_editor_like_cancel(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
) -> None:
    card = _create_card(repositories)
    editor, _draft = _editor(database, repositories)
    workspace = CardEditorWorkspace(CardStreamView(), editor)
    qtbot.addWidget(workspace)
    workspace.resize(600, 500)
    workspace.show()
    assert workspace.open_card(card.id)
    QTest.mouseClick(editor.viewport(), Qt.MouseButton.BackButton)

    assert editor.session is None
    assert editor.isVisible()
    assert not editor.isReadOnly()


def test_back_button_on_dirty_editor_runs_leave_gate(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
) -> None:
    card = _create_card(repositories)
    editor, _draft = _editor(database, repositories)
    workspace = CardEditorWorkspace(CardStreamView(), editor)
    qtbot.addWidget(workspace)
    workspace.show()
    assert workspace.open_card(card.id)
    editor.setPlainText("저장하지 않은 편집")
    assert editor.session is not None
    assert editor.session.dirty

    # 계속 편집을 고르면 취소 버튼과 똑같이 세션이 유지되어야 한다.
    monkeypatch.setattr(editor, "save_current", lambda: False)
    monkeypatch.setattr(
        editor,
        "_ask_close_choice",
        lambda: CloseChoice.KEEP_EDITING,
    )
    QTest.mouseClick(editor.viewport(), Qt.MouseButton.BackButton)
    assert editor.session is not None

    monkeypatch.setattr(
        editor,
        "_ask_close_choice",
        lambda: CloseChoice.DISCARD,
    )
    QTest.mouseClick(editor.viewport(), Qt.MouseButton.BackButton)
    assert editor.session is None


def test_back_button_without_session_is_left_to_default_handling(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
) -> None:
    editor, _draft = _editor(database, repositories)
    closed: list[bool] = []
    editor.close_accepted.connect(lambda: closed.append(True))
    qtbot.addWidget(editor)
    editor.show()

    QTest.mouseClick(editor.viewport(), Qt.MouseButton.BackButton)

    assert closed == []


def test_workspace_emits_model_data_changed_only_after_successful_save(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
) -> None:
    card = _create_card(repositories)
    model = CardListModel([card])
    card_list = CardStreamView(model)
    editor, _draft = _editor(database, repositories)
    workspace = CardEditorWorkspace(card_list, editor)
    qtbot.addWidget(workspace)
    workspace.resize(900, 500)
    workspace.show()
    qtbot.wait(20)
    card_list.setCurrentIndex(model.index(0))
    scroll_anchor = card_list.verticalScrollBar().value()
    changes: list[tuple[int, list[int]]] = []
    model_resets: list[bool] = []
    model.dataChanged.connect(
        lambda top_left, _bottom_right, roles: changes.append(
            (top_left.row(), roles)
        )
    )
    model.modelReset.connect(lambda: model_resets.append(True))
    assert editor.open_card(card.id)

    editor.selectAll()
    QTest.keyClicks(editor, "committed")

    assert model.index(0).data(CardRole.BODY) == card.body
    assert changes == [(0, [int(CardRole.DIRTY_DRAFT)])]
    assert model.index(0).data(CardRole.DIRTY_DRAFT) is True
    assert editor.save_current()
    assert model.index(0).data(CardRole.BODY) == "committed"
    assert model.index(0).data(CardRole.REVISION_COUNT) == 2
    assert model.index(0).data(CardRole.DIRTY_DRAFT) is False
    assert card_list.currentIndex().data(CardRole.CARD_ID) == card.id
    assert card_list.verticalScrollBar().value() == scroll_anchor
    assert model_resets == []
    assert all(row == 0 for row, _roles in changes)
    assert sum(int(CardRole.BODY) in roles for _row, roles in changes) == 1


def test_document_page_updates_card_preview_revision_and_dirty_after_save(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
) -> None:
    card = _create_card(repositories)
    page = DocumentPage(database, repositories, card.document_id)
    qtbot.addWidget(page)
    assert page._open_card(card.id)

    page.editor.setPlainText("목록에 즉시 반영할 본문")

    index = page.stream.card_model.index_for_card(card.id)
    assert index.data(CardRole.DIRTY_DRAFT) is True
    assert index.data(CardRole.PREVIEW) == card.body
    assert index.data(Qt.ItemDataRole.DisplayRole) == card.body

    assert page.editor.save_current()

    index = page.stream.card_model.index_for_card(card.id)
    assert index.data(CardRole.PREVIEW) == "목록에 즉시 반영할 본문"
    assert index.data(Qt.ItemDataRole.DisplayRole) == "목록에 즉시 반영할 본문"
    assert index.data(CardRole.REVISION_COUNT) == 2
    assert index.data(CardRole.DIRTY_DRAFT) is False


def test_workspace_save_and_cancel_buttons_have_korean_tooltips(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
) -> None:
    editor, _draft = _editor(database, repositories)
    workspace = CardEditorWorkspace(CardStreamView(), editor)
    qtbot.addWidget(workspace)

    assert (
        workspace.save_button.toolTip()
        == "초안을 카드에 확정하고 새 리비전을 만든다"
    )
    assert (
        workspace.cancel_button.toolTip()
        == "편집기를 닫으며 변경을 자동 저장한다. "
        "저장 실패 시에만 저장/버리기/계속 편집을 묻는다"
    )
