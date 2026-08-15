from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace
from typing import cast

import pytest
from PySide6.QtCore import QByteArray, QEvent, QModelIndex, QSettings, QSize, Qt
from PySide6.QtGui import QInputMethodEvent, QKeySequence
from PySide6.QtTest import QTest
from PySide6.QtWidgets import (
    QAbstractItemView,
    QApplication,
    QDialog,
    QMenu,
    QMessageBox,
)
from pytest import MonkeyPatch
from pytestqt.qtbot import QtBot
from shiboken6 import isValid

from pynote import app as app_module
from pynote.app import (
    AppContext,
    SqliteWorkspaceStateStore,
    WindowManager,
    initialize_device_settings,
)
from pynote.application import document_service
from pynote.application.card_service import CardService
from pynote.application.draft_coordinator import (
    DraftDisposition,
    RecoveryCandidate,
)
from pynote.domain.events import EventSource, EventType
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
from pynote.infrastructure.settings import DataPolicySettingsStore, window_geometry_key
from pynote.ui import main_window as main_window_module
from pynote.ui.cards.card_model import CardRole
from pynote.ui.document_page import DocumentPage
from pynote.ui.editor.card_editor import CardEditor, CloseChoice, EditorStatus
from pynote.ui.import_dialog import ImportController
from pynote.ui.main_window import DEFAULT_WINDOW_SIZE, DocumentUiState, MainWindow
from pynote.ui.settings_dialog import SettingsDialog


def _window(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> tuple[MainWindow, QSettings, DocumentPage]:
    settings = QSettings(
        str(tmp_path / "settings.ini"),
        QSettings.Format.IniFormat,
    )
    initialize_device_settings(settings)
    settings.setValue("first_run/guide_shown", True)
    window = MainWindow(
        repositories,
        SqliteWorkspaceStateStore(database),
        settings=settings,
    )
    qtbot.addWidget(window)
    window.show()
    document = document_service.create_document(repositories, "통합 문서")
    assert window.open_document_local(document.id)
    page = window.active_document_page()
    assert page is not None
    return window, settings, page


def _create_recovery_card(repositories: Repositories) -> Card:
    document = Document(
        id="recovery-document",
        title="복구 문서",
        created_at_us=1_000,
        updated_at_us=1_000,
    )
    repositories.create_document(document)
    return repositories.create_cards(
        NewCaptureOperation(
            id="recovery-operation",
            document_id=document.id,
            source=CaptureOperationSource.TYPING,
            split_policy=SplitPolicy.KEEP,
            original_text=None,
            created_at_us=2_000,
        ),
        [
            NewCard(
                id="recovery-card",
                revision_id="recovery-revision",
                event_id="recovery-event",
                position_key=1_024,
                body="확정 본문",
                card_source=CardSource.TYPING,
                event_source=EventSource.TYPING,
                revision_source=RevisionSource.EDIT,
                created_at_us=2_000,
            )
        ],
    )[0]


def _commit_text(editor: CardEditor, text: str) -> None:
    event = QInputMethodEvent()
    event.setCommitString(text)
    QApplication.sendEvent(editor, event)


def test_real_document_page_adds_opens_saves_and_navigates_with_shortcuts(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    window, _settings, page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )
    page.editor.setFocus()
    _commit_text(page.editor, "한글 😀 카드")

    assert page.editor.card_id is not None
    assert page.stream.card_model.rowCount() == 1
    card = page.stream.card_model.card_at(page.stream.card_model.index(0))
    assert card is not None
    assert card.id == page.editor.card_id
    assert page.editor.request_close()
    qtbot.wait(1)
    assert page.editor.card_id is None
    page.stream.setFocus()
    page.stream.setCurrentIndex(page.stream.card_model.index(0))
    QTest.keyClick(page.stream, Qt.Key.Key_Return)
    assert page.editor.card_id == card.id
    qtbot.waitUntil(lambda: QApplication.focusWidget() is page.editor)
    page.editor.moveCursor(page.editor.textCursor().MoveOperation.End)

    QTest.keyClick(
        page.editor,
        Qt.Key.Key_Return,
        Qt.KeyboardModifier.ControlModifier,
    )
    assert page.editor.toPlainText() == "한글 😀 카드\n"
    QTest.keyClicks(page.editor, " edited")
    QTest.keyClick(
        page.editor,
        Qt.Key.Key_S,
        Qt.KeyboardModifier.ControlModifier,
    )
    stored = repositories.get_card(card.id)
    assert stored is not None
    assert stored.body == "한글 😀 카드\n edited"

    QTest.keyClick(
        page.editor,
        Qt.Key.Key_F,
        Qt.KeyboardModifier.ControlModifier,
    )
    assert page.editor_workspace.find_bar.isVisible()
    QTest.keyClick(
        page.editor,
        Qt.Key.Key_H,
        Qt.KeyboardModifier.ControlModifier,
    )
    assert page.editor_workspace.find_bar.replace_input.isVisible()

    window.history_action.trigger()
    assert page.mode_stack.currentWidget() is page.history
    window.card_list_action.trigger()
    qtbot.wait(10)
    assert page.mode_stack.currentWidget() is page.editor_workspace
    assert page.stream.hasFocus()


def test_all_main_shortcuts_and_t7_t8_actions_are_wired(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    window, _settings, _page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )
    expected = {
        "newDocumentAction": "Ctrl+N",
        "documentListAction": "Ctrl+O",
        "globalSearchAction": "Ctrl+P",
        "importTextAction": "Ctrl+Shift+I",
        "exportTextAction": "Ctrl+Shift+E",
        "createBackupAction": "Ctrl+Alt+B",
        "restoreBackupAction": "Ctrl+Alt+R",
        "showCardListAction": "Ctrl+Shift+P",
        "showHistoryAction": "Ctrl+Shift+H",
        "focusModeAction": "F11",
    }
    actions = {action.objectName(): action for action in window.actions()}
    for object_name, shortcut in expected.items():
        assert object_name in actions
        assert actions[object_name].shortcut() == QKeySequence(shortcut)

    file_menu = window.file_menu
    assert isinstance(file_menu, QMenu)
    file_menu_actions = {
        action.objectName()
        for action in file_menu.actions()
    }
    assert {
        "importTextAction",
        "documentListAction",
        "exportTextAction",
        "createBackupAction",
        "restoreBackupAction",
    } <= file_menu_actions

    assert window.menuBar().isVisible()
    window.focus_action.trigger()
    assert not window.menuBar().isVisible()
    window.focus_action.trigger()
    assert window.menuBar().isVisible()

    window.search_action.trigger()
    qtbot.wait(10)
    assert window.search_dialog.isVisible()
    assert window.search_dialog.focusWidget() is window.search_dialog.query_edit


def test_view_menu_reset_geometry_is_first_and_geometry_persists(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    settings = QSettings(
        str(tmp_path / "geometry.ini"),
        QSettings.Format.IniFormat,
    )
    settings.setValue("first_run/guide_shown", True)
    store = SqliteWorkspaceStateStore(database)
    window = MainWindow(repositories, store, settings=settings)
    qtbot.addWidget(window)
    window.show()

    assert window.view_menu.actions()[0] is window.reset_window_action
    window.resize(700, 500)
    window.move(40, 50)
    qtbot.wait(20)
    assert window.close()

    restarted = MainWindow(repositories, store, settings=settings)
    qtbot.addWidget(restarted)
    restarted.show()
    assert restarted.size() == QSize(700, 500)
    assert restarted.pos() == window.pos()

    restarted.resize(320, 240)
    restarted.reset_window_action.trigger()
    assert restarted.size() == DEFAULT_WINDOW_SIZE
    assert restarted.frameGeometry().center() == (
        restarted.screen().availableGeometry().center()
    )


def test_offscreen_saved_geometry_is_corrected_to_default_screen(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    settings = QSettings(
        str(tmp_path / "offscreen-geometry.ini"),
        QSettings.Format.IniFormat,
    )
    settings.setValue("first_run/guide_shown", True)
    store = SqliteWorkspaceStateStore(database)
    source = MainWindow(repositories, store, settings=settings)
    qtbot.addWidget(source)
    source.move(100_000, 100_000)
    source.resize(333, 222)
    stored_geometry = source.saveGeometry()
    settings.setValue(window_geometry_key(source.window_id), stored_geometry)
    settings.sync()
    restored_values: list[QByteArray] = []
    real_restore = MainWindow.restoreGeometry

    def observe_restore(window: MainWindow, geometry: QByteArray) -> bool:
        restored_values.append(geometry)
        return real_restore(window, geometry)

    monkeypatch.setattr(MainWindow, "restoreGeometry", observe_restore)

    corrected = MainWindow(repositories, store, settings=settings)
    qtbot.addWidget(corrected)
    corrected.show()

    assert corrected.screen().availableGeometry().intersects(
        corrected.frameGeometry()
    )
    assert restored_values == [stored_geometry]


@pytest.mark.parametrize(
    "choice",
    [
        DraftDisposition.RECOVER,
        DraftDisposition.DISCARD,
        DraftDisposition.LATER,
    ],
)
def test_startup_recovery_precedes_workspace_and_applies_three_choices(
    choice: DraftDisposition,
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    monkeypatch: MonkeyPatch,
) -> None:
    card = _create_recovery_card(repositories)
    draft = Draft(
        id="startup-draft",
        document_id=card.document_id,
        card_id=card.id,
        draft_kind=DraftKind.EDIT,
        base_revision_id=card.current_revision_id,
        draft_text="복구할 미저장 본문",
        draft_hash=text_hash("복구할 미저장 본문"),
        cursor_position_qchar=4,
        updated_at_us=3_000,
    )
    repositories.create_draft(draft)
    store = SqliteWorkspaceStateStore(database)
    store.save_workspace((card.document_id,), card.document_id)
    events: list[str] = []
    original_load_workspace = store.load_workspace
    monkeypatch.setattr(
        store,
        "load_workspace",
        lambda: events.append("workspace") or original_load_workspace(),
    )

    def choose(candidate: RecoveryCandidate) -> DraftDisposition:
        events.append("recovery")
        assert candidate.committed_text == card.body
        assert candidate.draft.draft_text == draft.draft_text
        return choice

    window = MainWindow(
        repositories,
        store,
        recovery_choice_provider=choose,
    )
    qtbot.addWidget(window)
    assert events[:2] == ["recovery", "workspace"]
    close_choice_calls: list[bool] = []

    if choice is DraftDisposition.DISCARD:
        assert repositories.get_draft(draft.id) is None
    elif choice is DraftDisposition.RECOVER:
        page = window.active_document_page()
        assert page is not None
        assert page.editor.session is not None
        assert page.editor.toPlainText() == draft.draft_text
        # 이 patch는 startup 복구가 끝난 뒤 설치된다. 아래 호출 횟수는 복구
        # 자체가 아니라 테스트 정리용 window.close()의 실패 fallback을 센다.
        monkeypatch.setattr(page.editor, "save_current", lambda: False)
        monkeypatch.setattr(
            page.editor,
            "_ask_close_choice",
            lambda: close_choice_calls.append(True) or CloseChoice.DISCARD,
        )
    else:
        assert repositories.get_draft(draft.id) == draft
        assert window.open_document(card.document_id)
        page = window.active_document_page()
        assert page is not None
        asked: list[str] = []

        def discard_later_candidate(
            candidate: RecoveryCandidate,
        ) -> DraftDisposition:
            asked.append(candidate.draft.id)
            return DraftDisposition.DISCARD

        monkeypatch.setattr(
            page.editor,
            "_ask_recovery_choice",
            discard_later_candidate,
        )
        assert page.open_card(card.id)
        assert asked == [draft.id]
        assert repositories.get_draft(draft.id) is None
    assert window.close()
    assert close_choice_calls == (
        [True] if choice is DraftDisposition.RECOVER else []
    )


@pytest.mark.parametrize("entrypoint", ["manager", "direct"])
@pytest.mark.parametrize(
    "choice",
    [DraftDisposition.RECOVER, DraftDisposition.LATER],
)
def test_startup_entrypoints_share_recovery_and_later_policy(
    entrypoint: str,
    choice: DraftDisposition,
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    document = Document(
        id=f"startup-parity-{entrypoint}-{choice.value}",
        title="시작 경로 정책 일치",
        created_at_us=1,
        updated_at_us=1,
    )
    repositories.create_document(document)
    service = CardService(database, repositories, clock=lambda: 10)
    cards = (
        service.create_card(document.id, "첫 확정 본문"),
        service.create_card(document.id, "둘째 확정 본문"),
    )
    drafts = tuple(
        Draft(
            id=f"startup-parity-draft-{entrypoint}-{choice.value}-{number}",
            document_id=document.id,
            card_id=card.id,
            draft_kind=DraftKind.EDIT,
            base_revision_id=card.current_revision_id,
            draft_text=f"미저장 본문 {number}",
            draft_hash=text_hash(f"미저장 본문 {number}"),
            cursor_position_qchar=number,
            updated_at_us=20 + number,
        )
        for number, card in enumerate(cards, start=1)
    )
    for draft in drafts:
        repositories.create_draft(draft)
    revision_counts = {
        card.id: len(repositories.list_revisions(card.id)) for card in cards
    }
    window_id = (
        f"startup-parity-window-{choice.value}"
        if entrypoint == "manager"
        else "legacy-window"
    )
    store = SqliteWorkspaceStateStore(database, window_id)
    if entrypoint == "manager":
        repositories.save_workspace_window(
            window_id,
            (document.id,),
            document.id,
        )
    else:
        store.save_workspace((document.id,), document.id)
    state = DocumentUiState(
        document_id=document.id,
        selected_card_id=cards[1].id,
        list_scroll_position=0,
        sort_mode="position",
        editor_card_id=cards[1].id,
        editor_base_revision_id=cards[1].current_revision_id,
        editor_cursor_qchar=drafts[1].cursor_position_qchar,
        editor_split_sizes=None,
        updated_at_us=30,
    )
    store.save_document_ui_state(state)
    settings = QSettings(
        str(tmp_path / f"startup-parity-{entrypoint}-{choice.value}.ini"),
        QSettings.Format.IniFormat,
    )
    settings.setValue("first_run/guide_shown", True)
    startup_questions: list[str] = []
    app_driven_questions: list[str] = []
    recovery_plan_inputs: list[tuple[str, ...]] = []

    def choose(candidate: RecoveryCandidate) -> DraftDisposition:
        startup_questions.append(candidate.draft.id)
        return choice

    def fail_app_driven_prompt(
        _editor: CardEditor,
        candidate: RecoveryCandidate,
    ) -> DraftDisposition:
        app_driven_questions.append(candidate.draft.id)
        raise AssertionError("LATER 카드를 앱 주도 경로에서 다시 물었습니다.")

    monkeypatch.setattr(
        CardEditor,
        "_ask_recovery_choice",
        fail_app_driven_prompt,
    )
    monkeypatch.setattr(QMessageBox, "critical", lambda *_args, **_kwargs: None)
    recovery_module = (
        app_module if entrypoint == "manager" else main_window_module
    )
    original_build_recovery_plans = recovery_module.build_recovery_plans

    def observe_recovery_plans(
        candidates: tuple[RecoveryCandidate, ...],
        *,
        opened_editor_cards: dict[str, str | None],
    ) -> object:
        recovery_plan_inputs.append(
            tuple(candidate.draft.id for candidate in candidates)
        )
        return original_build_recovery_plans(
            candidates,
            opened_editor_cards=opened_editor_cards,
        )

    monkeypatch.setattr(
        recovery_module,
        "build_recovery_plans",
        observe_recovery_plans,
    )
    manager = None
    if entrypoint == "manager":
        manager = WindowManager(
            AppContext(database, settings),
            recovery_choice_provider=choose,
        )
        manager.restore_windows()
        window = manager.windows[0]
    else:
        window = MainWindow(
            repositories,
            store,
            settings=settings,
            recovery_choice_provider=choose,
        )
    qtbot.addWidget(window)
    window.show()

    assert startup_questions == [draft.id for draft in drafts]
    assert app_driven_questions == []
    assert recovery_plan_inputs == [
        (
            tuple(draft.id for draft in drafts)
            if choice is DraftDisposition.RECOVER
            else ()
        )
    ]
    page = window.active_document_page()
    assert page is not None
    expected_suppressed = (
        {card.id for card in cards}
        if choice is DraftDisposition.LATER
        else set()
    )
    assert window._startup_suppressed_card_ids == expected_suppressed
    if manager is not None:
        if choice is DraftDisposition.LATER:
            assert manager._recovered_candidates == ()
        assert {
            candidate.draft.id for candidate in manager._recovered_candidates
        } == (
            {draft.id for draft in drafts}
            if choice is DraftDisposition.RECOVER
            else set()
        )
    if choice is DraftDisposition.RECOVER:
        assert page.editor.session is not None
        assert page.editor.session.card_id == cards[1].id
        assert page.editor.toPlainText() == drafts[1].draft_text
    else:
        assert page.editor.session is None
    for card, draft in zip(cards, drafts, strict=True):
        assert len(repositories.list_revisions(card.id)) == revision_counts[card.id]
        assert repositories.get_draft(draft.id) == draft
        index = page.stream.card_model.index_for_card(card.id)
        assert index.isValid()
        assert index.data(CardRole.DIRTY_DRAFT) is True


@pytest.mark.parametrize(
    "choice",
    [
        DraftDisposition.LATER,
        DraftDisposition.RECOVER,
        DraftDisposition.DISCARD,
    ],
)
def test_later_suppression_user_reselection_follows_three_choices(
    choice: DraftDisposition,
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    recovery_card = _create_recovery_card(repositories)
    other_document = Document(
        id="later-switch-document",
        title="전환 대상 문서",
        created_at_us=4_000,
        updated_at_us=4_000,
    )
    repositories.create_document(other_document)
    draft = Draft(
        id="later-switch-draft",
        document_id=recovery_card.document_id,
        card_id=recovery_card.id,
        draft_kind=DraftKind.EDIT,
        base_revision_id=recovery_card.current_revision_id,
        draft_text="나중에 처분할 본문",
        draft_hash=text_hash("나중에 처분할 본문"),
        cursor_position_qchar=4,
        updated_at_us=3_000,
    )
    repositories.create_draft(draft)
    store = SqliteWorkspaceStateStore(database)
    store.save_workspace((recovery_card.document_id,), recovery_card.document_id)
    state = DocumentUiState(
        document_id=recovery_card.document_id,
        selected_card_id=recovery_card.id,
        list_scroll_position=0,
        sort_mode="position",
        editor_card_id=recovery_card.id,
        editor_base_revision_id=recovery_card.current_revision_id,
        editor_cursor_qchar=draft.cursor_position_qchar,
        editor_split_sizes=None,
        updated_at_us=5_000,
    )
    store.save_document_ui_state(state)
    app_driven_questions: list[str] = []

    def fail_app_driven_prompt(
        _editor: CardEditor,
        candidate: RecoveryCandidate,
    ) -> DraftDisposition:
        app_driven_questions.append(candidate.draft.id)
        raise AssertionError("LATER 억제가 앱 주도 복원 사이에 소비됐습니다.")

    monkeypatch.setattr(
        CardEditor,
        "_ask_recovery_choice",
        fail_app_driven_prompt,
    )
    monkeypatch.setattr(QMessageBox, "critical", lambda *_args, **_kwargs: None)
    settings = QSettings(
        str(tmp_path / "later-nonconsuming.ini"),
        QSettings.Format.IniFormat,
    )
    settings.setValue("first_run/guide_shown", True)
    window = MainWindow(
        repositories,
        store,
        settings=settings,
        recovery_choice_provider=lambda _candidate: DraftDisposition.LATER,
    )
    qtbot.addWidget(window)
    window.show()

    for _attempt in range(2):
        assert window.open_document_local(other_document.id, app_driven=True)
        store.save_document_ui_state(state)
        assert window.open_document_local(
            recovery_card.document_id,
            app_driven=True,
        )
        page = window.active_document_page()
        assert page is not None
        assert page.editor.session is None
        assert recovery_card.id in window._startup_suppressed_card_ids
    assert app_driven_questions == []
    assert repositories.get_draft(draft.id) == draft

    user_questions: list[str] = []

    def choose_on_user_click(
        _editor: CardEditor,
        candidate: RecoveryCandidate,
    ) -> DraftDisposition:
        user_questions.append(candidate.draft.id)
        return choice

    monkeypatch.setattr(
        CardEditor,
        "_ask_recovery_choice",
        choose_on_user_click,
    )
    page = window.active_document_page()
    assert page is not None
    opened_cards: list[str] = []
    page.card_opened.connect(opened_cards.append)
    target = page.stream.card_model.index_for_card(recovery_card.id)
    assert target.isValid()
    page.stream.scrollTo(target)

    def click_target() -> None:
        qtbot.mouseClick(
            page.stream.viewport(),
            Qt.MouseButton.LeftButton,
            pos=page.stream.visualRect(target).center(),
        )

    click_target()

    assert user_questions == [draft.id]
    if choice is DraftDisposition.LATER:
        assert opened_cards == []
        assert page.editor.session is None
        assert repositories.get_draft(draft.id) == draft
        assert recovery_card.id in window._startup_suppressed_card_ids
        click_target()
        assert user_questions == [draft.id, draft.id]
        assert opened_cards == []
        assert page.editor.session is None
        assert repositories.get_draft(draft.id) == draft
        assert recovery_card.id in window._startup_suppressed_card_ids
        return

    assert opened_cards == [recovery_card.id]
    assert page.editor.session is not None
    assert page.editor.session.card_id == recovery_card.id
    assert recovery_card.id not in window._startup_suppressed_card_ids
    if choice is DraftDisposition.RECOVER:
        assert repositories.get_draft(draft.id) == draft
        assert page.editor.toPlainText() == draft.draft_text
        return
    assert repositories.get_draft(draft.id) is None

    assert window.open_document_local(other_document.id, app_driven=True)
    store.save_document_ui_state(state)
    assert window.open_document_local(
        recovery_card.document_id,
        app_driven=True,
    )
    restored_page = window.active_document_page()
    assert restored_page is not None
    assert restored_page.editor.session is not None
    assert restored_page.editor.session.card_id == recovery_card.id
    assert user_questions == [draft.id]
    assert opened_cards == [recovery_card.id]
    assert recovery_card.id not in window._startup_suppressed_card_ids


def test_direct_main_window_recovery_preserves_live_non_candidate_edit(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    monkeypatch: MonkeyPatch,
) -> None:
    document = Document(
        id="direct-batch-document",
        title="직접 시작 복구 문서",
        created_at_us=1,
        updated_at_us=1,
    )
    repositories.create_document(document)
    service = CardService(database, repositories, clock=lambda: 10)
    workspace_card = service.create_card(document.id, "workspace 확정 본문")
    cards = (
        service.create_card(document.id, "첫 복구 확정 본문"),
        service.create_card(document.id, "둘째 복구 확정 본문"),
    )
    drafts = []
    for number, card in enumerate(cards, start=1):
        text = f"직접 시작 미저장 본문 {number}"
        draft = Draft(
            id=f"direct-batch-draft-{number}",
            document_id=document.id,
            card_id=card.id,
            draft_kind=DraftKind.EDIT,
            base_revision_id=card.current_revision_id,
            draft_text=text,
            draft_hash=text_hash(text),
            cursor_position_qchar=number + 1,
            updated_at_us=20 + number,
        )
        repositories.create_draft(draft)
        drafts.append(draft)
    all_cards = (workspace_card, *cards)
    revision_counts = {
        card.id: len(repositories.list_revisions(card.id))
        for card in all_cards
    }
    store = SqliteWorkspaceStateStore(database)
    store.save_workspace((document.id,), document.id)
    store.save_document_ui_state(
        DocumentUiState(
            document_id=document.id,
            selected_card_id=workspace_card.id,
            list_scroll_position=0,
            sort_mode="position",
            editor_card_id=workspace_card.id,
            editor_base_revision_id=workspace_card.current_revision_id,
            editor_cursor_qchar=2,
            editor_split_sizes=None,
            updated_at_us=30,
        )
    )
    live_text = "UI state 적용 직후 아직 보호되지 않은 본문"
    live_cursor = 9
    live_draft_ids: list[str] = []
    apply_ui_state = MainWindow._apply_ui_state_to_page

    def apply_ui_state_then_edit(
        window: MainWindow,
        page: DocumentPage,
        state: DocumentUiState,
    ) -> None:
        apply_ui_state(window, page, state)
        session = page.editor.session
        assert session is not None
        assert session.card_id == workspace_card.id
        live_draft_ids.append(session.draft_id)
        page.editor.setPlainText(live_text)
        cursor = page.editor.textCursor()
        cursor.setPosition(live_cursor)
        page.editor.setTextCursor(cursor)

    monkeypatch.setattr(
        MainWindow,
        "_apply_ui_state_to_page",
        apply_ui_state_then_edit,
    )

    window = MainWindow(
        repositories,
        store,
        recovery_choice_provider=lambda _candidate: DraftDisposition.RECOVER,
    )
    qtbot.addWidget(window)

    page = window.active_document_page()
    assert page is not None
    assert page.editor.session is not None
    assert page.editor.session.card_id == cards[0].id
    assert page.editor.toPlainText() == drafts[0].draft_text
    assert live_draft_ids
    live_draft = repositories.get_draft(live_draft_ids[0])
    assert live_draft is not None
    assert live_draft.draft_text == live_text
    assert live_draft.cursor_position_qchar == live_cursor
    assert len(repositories.list_revisions(workspace_card.id)) == revision_counts[
        workspace_card.id
    ]
    for card, draft in zip(cards, drafts, strict=True):
        assert len(repositories.list_revisions(card.id)) == revision_counts[card.id]
        stored = repositories.get_draft(draft.id)
        assert stored is not None
        assert stored.draft_text == draft.draft_text
        assert stored.cursor_position_qchar == draft.cursor_position_qchar
    second_index = page.stream.card_model.index_for_card(cards[1].id)
    assert second_index.isValid()
    assert second_index.data(CardRole.DIRTY_DRAFT) is True


@pytest.mark.parametrize("empty_surface_ime", [False, True])
def test_direct_recovery_document_switch_quietly_preserves_empty_surface(
    empty_surface_ime: bool,
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    monkeypatch: MonkeyPatch,
) -> None:
    workspace_document = Document(
        id="direct-empty-surface-workspace",
        title="빈 편집면 복원 문서",
        created_at_us=1,
        updated_at_us=1,
    )
    recovery_document = Document(
        id="direct-empty-surface-recovery",
        title="카드 복구 문서",
        created_at_us=2,
        updated_at_us=2,
    )
    repositories.create_document(workspace_document)
    repositories.create_document(recovery_document)
    empty_surface_text = "모달 없이 보존할 빈 편집면 초안"
    empty_surface_draft = Draft(
        id="direct-empty-surface-new-draft",
        document_id=workspace_document.id,
        card_id=None,
        draft_kind=DraftKind.NEW,
        base_revision_id=None,
        draft_text=empty_surface_text,
        draft_hash=text_hash(empty_surface_text),
        cursor_position_qchar=6,
        updated_at_us=20,
    )
    repositories.create_draft(empty_surface_draft)
    recovery_card = CardService(
        database,
        repositories,
        clock=lambda: 10,
    ).create_card(recovery_document.id, "복구 대상 확정 본문")
    recovery_text = "복구 대상 미저장 본문"
    recovery_draft = Draft(
        id="direct-recovery-edit-draft",
        document_id=recovery_document.id,
        card_id=recovery_card.id,
        draft_kind=DraftKind.EDIT,
        base_revision_id=recovery_card.current_revision_id,
        draft_text=recovery_text,
        draft_hash=text_hash(recovery_text),
        cursor_position_qchar=4,
        updated_at_us=21,
    )
    repositories.create_draft(recovery_draft)
    store = SqliteWorkspaceStateStore(database)
    store.save_workspace((workspace_document.id,), workspace_document.id)
    restored_pages: list[DocumentPage] = []
    modal_calls: list[tuple[str, str]] = []
    restore_workspace = MainWindow.restore_workspace

    def restore_then_inject(window: MainWindow) -> None:
        restore_workspace(window)
        page = window.active_document_page()
        assert page is not None
        restored_pages.append(page)
        page._error_reporter = (
            lambda title, message: modal_calls.append((title, message))
        )
        page.editor._ask_close_choice = lambda: pytest.fail(
            "앱 주도 시작 복구가 사용자 이탈 확인 모달을 호출했습니다."
        )
        if empty_surface_ime:
            QApplication.sendEvent(
                page.editor,
                QInputMethodEvent("ㅎ", []),
            )

    monkeypatch.setattr(MainWindow, "restore_workspace", restore_then_inject)

    window = MainWindow(
        repositories,
        store,
        recovery_choice_provider=lambda _candidate: DraftDisposition.RECOVER,
    )
    qtbot.addWidget(window)

    restored_page = restored_pages[0]
    stored_empty_surface = repositories.get_draft(empty_surface_draft.id)
    assert stored_empty_surface is not None
    assert stored_empty_surface.draft_text == empty_surface_text
    assert (
        stored_empty_surface.cursor_position_qchar
        == empty_surface_draft.cursor_position_qchar
    )
    assert modal_calls == []
    if empty_surface_ime:
        assert window.active_document_id == workspace_document.id
        assert restored_page.editor.card_id is None
        assert restored_page.editor.toPlainText() == empty_surface_text
        assert repositories.get_draft(recovery_draft.id) == recovery_draft
        QApplication.sendEvent(
            restored_page.editor,
            QInputMethodEvent(),
        )
        restored_page.editor.clear()
    else:
        assert window.active_document_id == recovery_document.id
        page = window.active_document_page()
        assert page is not None
        assert page.editor.card_id == recovery_card.id
        assert page.editor.toPlainText() == recovery_text


def test_app_driven_card_switch_protects_latest_widget_without_revision(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    _window_instance, _settings, page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )
    service = CardService(database, repositories, clock=lambda: 10)
    first = service.create_card(page.document_id, "앱 주도 첫 카드")
    second = service.create_card(page.document_id, "앱 주도 둘째 카드")
    page.refresh()
    assert page.open_card(first.id)
    page.editor.setPlainText("교체 직전 최신 위젯 본문")
    cursor = page.editor.textCursor()
    cursor.setPosition(3)
    page.editor.setTextCursor(cursor)
    session = page.editor.session
    assert session is not None
    assert page.editor.protect_now()
    cursor = page.editor.textCursor()
    cursor.setPosition(7)
    page.editor.setTextCursor(cursor)
    revision_count = len(repositories.list_revisions(first.id))

    assert page.open_card(second.id, app_driven=True)

    stored = repositories.get_draft(session.draft_id)
    assert stored is not None
    assert stored.draft_text == "교체 직전 최신 위젯 본문"
    assert stored.cursor_position_qchar == 7
    assert len(repositories.list_revisions(first.id)) == revision_count
    assert page.editor.session is not None
    assert page.editor.session.card_id == second.id


@pytest.mark.parametrize("failure_mode", ["false", "exception", "ime"])
def test_app_driven_card_switch_keeps_session_when_protection_fails(
    failure_mode: str,
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    _window_instance, _settings, page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )
    service = CardService(database, repositories, clock=lambda: 10)
    first = service.create_card(page.document_id, f"보호 실패 첫 카드 {failure_mode}")
    second = service.create_card(page.document_id, f"보호 실패 둘째 카드 {failure_mode}")
    page.refresh()
    assert page.open_card(first.id)
    page.editor.setPlainText(f"보호 실패 직전 본문 {failure_mode}")
    session = page.editor.session
    assert session is not None
    revision_count = len(repositories.list_revisions(first.id))
    errors: list[tuple[str, str]] = []
    monkeypatch.setattr(
        page,
        "_error_reporter",
        lambda title, message: errors.append((title, message)),
    )
    if failure_mode == "false":
        monkeypatch.setattr(page.editor, "protect_now", lambda: False)
    elif failure_mode == "exception":
        def raise_protection_error(_draft_id: str) -> None:
            raise RuntimeError("주입한 draft 보호 실패")

        monkeypatch.setattr(
            page.draft_coordinator,
            "protect_now",
            raise_protection_error,
        )
    else:
        page.draft_coordinator.set_ime_composing(session.draft_id, True)

    try:
        assert not page.open_card(second.id, app_driven=True)

        current_session = page.editor.session
        assert current_session is not None
        assert current_session is session
        assert current_session.card_id == first.id
        assert len(repositories.list_revisions(first.id)) == revision_count
        assert errors == []
    finally:
        if failure_mode == "ime":
            page.draft_coordinator.set_ime_composing(session.draft_id, False)
        monkeypatch.undo()
        if page.editor.session is session:
            page.editor.setPlainText(first.body)


def test_user_card_switch_still_auto_saves_while_app_driven_switch_detaches(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    _window_instance, _settings, page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )
    service = CardService(database, repositories, clock=lambda: 10)
    first = service.create_card(page.document_id, "사용자 첫 카드")
    second = service.create_card(page.document_id, "사용자 둘째 카드")
    page.refresh()
    assert page.open_card(first.id)
    page.editor.setPlainText("사용자 카드 전환 자동 저장 본문")
    first_revision_count = len(repositories.list_revisions(first.id))

    assert page.open_card(second.id)

    stored_first = repositories.get_card(first.id)
    assert stored_first is not None
    assert stored_first.body == "사용자 카드 전환 자동 저장 본문"
    assert len(repositories.list_revisions(first.id)) == first_revision_count + 1
    page.editor.setPlainText("앱 주도 전환 보호 본문")
    second_session = page.editor.session
    assert second_session is not None
    second_revision_count = len(repositories.list_revisions(second.id))

    assert page.open_card(first.id, app_driven=True)

    assert len(repositories.list_revisions(second.id)) == second_revision_count
    stored_second_draft = repositories.get_draft(second_session.draft_id)
    assert stored_second_draft is not None
    assert stored_second_draft.draft_text == "앱 주도 전환 보호 본문"


@pytest.mark.parametrize("activation", ["mouse", "enter"])
def test_user_card_click_and_enter_auto_save_dirty_session(
    activation: str,
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    _window_instance, _settings, page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )
    service = CardService(database, repositories, clock=lambda: 10)
    first = service.create_card(page.document_id, f"{activation} 이전 카드")
    second = service.create_card(page.document_id, f"{activation} 대상 카드")
    page.refresh()
    assert page.open_card(first.id)
    edited_text = f"{activation} 실제 소비자 경로 자동 저장"
    page.editor.setPlainText(edited_text)
    revision_count = len(repositories.list_revisions(first.id))
    target = page.stream.card_model.index_for_card(second.id)
    assert target.isValid()

    if activation == "mouse":
        page.stream.scrollTo(target)
        qtbot.mouseClick(
            page.stream.viewport(),
            Qt.MouseButton.LeftButton,
            pos=page.stream.visualRect(target).center(),
        )
    else:
        page.stream.setCurrentIndex(target)
        page.stream.setFocus()
        QTest.keyClick(page.stream, Qt.Key.Key_Return)

    assert page.editor.session is not None
    assert page.editor.session.card_id == second.id
    stored_first = repositories.get_card(first.id)
    assert stored_first is not None
    assert stored_first.body == edited_text
    assert len(repositories.list_revisions(first.id)) == revision_count + 1


def test_stale_recovery_draft_opens_then_protects_and_ctrl_s_safely(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    card = _create_recovery_card(repositories)
    draft_text = "확정본보다 오래됐지만 보존할 초안"
    draft = Draft(
        id="stale-recovery-draft",
        document_id=card.document_id,
        card_id=card.id,
        draft_kind=DraftKind.EDIT,
        base_revision_id=card.current_revision_id,
        draft_text=draft_text,
        draft_hash=text_hash(draft_text),
        cursor_position_qchar=5,
        updated_at_us=1_000,
    )
    repositories.create_draft(draft)
    window = MainWindow(
        repositories,
        SqliteWorkspaceStateStore(database),
        settings=QSettings(
            str(tmp_path / "stale-recovery.ini"),
            QSettings.Format.IniFormat,
        ),
    )
    qtbot.addWidget(window)
    assert window.open_document_local(card.document_id)
    page = window.active_document_page()
    assert page is not None
    asked: list[RecoveryCandidate] = []
    errors: list[tuple[str, str]] = []
    monkeypatch.setattr(
        page.editor,
        "_ask_recovery_choice",
        lambda candidate: asked.append(candidate) or DraftDisposition.RECOVER,
    )
    monkeypatch.setattr(
        page,
        "_error_reporter",
        lambda title, message: errors.append((title, message)),
    )

    assert page.open_card(card.id)
    assert len(asked) == 1
    assert asked[0].committed_is_newer
    assert page.editor.session is not None
    assert page.editor.session.draft_id == draft.id
    assert page.editor.toPlainText() == draft_text
    assert page.editor.textCursor().position() == draft.cursor_position_qchar
    revision_count = len(repositories.list_revisions(card.id))
    page.editor.setPlainText("stale 처분 뒤 새 편집")
    assert page.editor.protect_now()
    page.editor.setFocus()
    QTest.keyClick(
        page.editor,
        Qt.Key.Key_S,
        Qt.KeyboardModifier.ControlModifier,
    )

    stored_card = repositories.get_card(card.id)
    assert stored_card is not None
    assert stored_card.body == "stale 처분 뒤 새 편집"
    assert len(repositories.list_revisions(card.id)) == revision_count + 1
    assert repositories.get_draft(draft.id) is None
    assert errors == []


def test_equal_timestamp_discard_opens_committed_then_saves_safely(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    card = _create_recovery_card(repositories)
    revision = repositories.get_revision(card.current_revision_id or "")
    assert revision is not None
    draft_text = "같은 시각이지만 버릴 recovery draft"
    draft = Draft(
        id="equal-timestamp-discard-draft",
        document_id=card.document_id,
        card_id=card.id,
        draft_kind=DraftKind.EDIT,
        base_revision_id=card.current_revision_id,
        draft_text=draft_text,
        draft_hash=text_hash(draft_text),
        cursor_position_qchar=5,
        updated_at_us=revision.created_at_us,
    )
    repositories.create_draft(draft)
    window = MainWindow(
        repositories,
        SqliteWorkspaceStateStore(database),
        settings=QSettings(
            str(tmp_path / "equal-discard.ini"),
            QSettings.Format.IniFormat,
        ),
    )
    qtbot.addWidget(window)
    assert window.open_document_local(card.document_id)
    page = window.active_document_page()
    assert page is not None
    asked: list[RecoveryCandidate] = []
    monkeypatch.setattr(
        page.editor,
        "_ask_recovery_choice",
        lambda candidate: asked.append(candidate) or DraftDisposition.DISCARD,
    )
    revision_count = len(repositories.list_revisions(card.id))

    assert page.open_card(card.id)
    assert len(asked) == 1
    assert asked[0].committed_is_newer
    assert page.editor.session is not None
    assert page.editor.session.draft_id != draft.id
    assert page.editor.toPlainText() == card.body
    assert page.editor.textCursor().position() == 0
    assert repositories.get_draft(draft.id) is None
    page.editor.setPlainText("같은 시각 DISCARD 뒤 편집")
    assert page.editor.protect_now()
    page.editor.setFocus()
    QTest.keyClick(
        page.editor,
        Qt.Key.Key_S,
        Qt.KeyboardModifier.ControlModifier,
    )

    stored_card = repositories.get_card(card.id)
    assert stored_card is not None
    assert stored_card.body == "같은 시각 DISCARD 뒤 편집"
    assert len(repositories.list_revisions(card.id)) == revision_count + 1
    assert repositories.list_drafts(card.document_id) == ()


@pytest.mark.parametrize("entrypoint", ["manager", "window", "editor"])
@pytest.mark.parametrize("committed_is_newer", [False, True])
def test_recovery_dialog_text_matches_timestamp_relation(
    entrypoint: str,
    committed_is_newer: bool,
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    window, _settings, page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )
    text = "문구 확인 recovery draft"
    candidate = RecoveryCandidate(
        draft=Draft(
            id=f"dialog-{entrypoint}-{committed_is_newer}",
            document_id=page.document_id,
            card_id="dialog-card",
            draft_kind=DraftKind.EDIT,
            base_revision_id=None,
            draft_text=text,
            draft_hash=text_hash(text),
            cursor_position_qchar=0,
            updated_at_us=10,
        ),
        committed_text="문구 확인 확정본",
        committed_revision_id=None,
        committed_is_newer=committed_is_newer,
    )
    shown_texts: list[str] = []
    monkeypatch.setattr(
        QMessageBox,
        "setText",
        lambda _box, value: shown_texts.append(value),
    )
    monkeypatch.setattr(QMessageBox, "exec", lambda _box: 0)
    monkeypatch.setattr(QMessageBox, "clickedButton", lambda _box: None)

    if entrypoint == "manager":
        choice = WindowManager._ask_recovery_choice(candidate)
    elif entrypoint == "window":
        choice = window._ask_recovery_choice(candidate)
    else:
        choice = page.editor._ask_recovery_choice(candidate)

    assert choice is DraftDisposition.LATER
    assert shown_texts == [
        (
            "확정본이 recovery draft보다 새롭습니다."
            if committed_is_newer
            else "확정본보다 새로운 recovery draft가 있습니다."
        )
    ]


def test_document_switch_and_app_exit_share_leave_gate_and_protect_draft(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    window, _settings, first_page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )
    _commit_text(first_page.editor, "첫 카드")
    assert first_page.editor.card_id is not None
    card = first_page.stream.card_model.card_at(
        first_page.stream.card_model.index(0)
    )
    assert card is not None
    second_document = document_service.create_document(repositories, "둘째 문서")
    assert first_page.open_card(card.id)
    first_page.editor.setPlainText("미저장")
    monkeypatch.setattr(first_page.editor, "save_current", lambda: False)
    monkeypatch.setattr(
        first_page.editor,
        "_ask_close_choice",
        lambda: CloseChoice.KEEP_EDITING,
    )

    assert not window.open_document_local(second_document.id)
    assert window.active_document_id == first_page.document_id
    session = first_page.editor.session
    assert session is not None
    assert repositories.get_draft(session.draft_id) is not None
    assert not window.close()

    monkeypatch.setattr(
        first_page.editor,
        "_ask_close_choice",
        lambda: CloseChoice.DISCARD,
    )
    assert window.close()


def test_window_deactivate_event_protects_dirty_draft_without_revision(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    window, _settings, page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )
    _commit_text(page.editor, "비활성화할 카드")
    assert page.editor.card_id is not None
    card = repositories.list_cards(page.document_id)[0]
    assert page.open_card(card.id)
    page.editor.setPlainText("비활성화 직전 미저장 본문")
    session = page.editor.session
    assert session is not None
    revision_count = len(repositories.list_revisions(card.id))
    assert repositories.get_draft(session.draft_id) is None

    handled = QApplication.sendEvent(
        window,
        QEvent(QEvent.Type.WindowDeactivate),
    )

    stored = repositories.get_draft(session.draft_id)
    assert handled
    assert stored is not None
    assert stored.draft_text == "비활성화 직전 미저장 본문"
    assert len(repositories.list_revisions(card.id)) == revision_count
    assert page.editor.save_current()


def test_window_deactivate_during_ime_never_opens_dialog_or_blocks_event(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    window, _settings, page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )
    _commit_text(page.editor, "IME 전환 카드")
    assert page.editor.card_id is not None
    card = repositories.list_cards(page.document_id)[0]
    assert page.open_card(card.id)
    page.editor.setPlainText("조합 중 전환할 미저장 본문")
    session = page.editor.session
    assert session is not None
    revision_count = len(repositories.list_revisions(card.id))
    QApplication.sendEvent(page.editor, QInputMethodEvent("ㅎ", []))
    assert page.draft_coordinator.is_ime_composing(session.draft_id)

    def fail_dialog(*_args: object, **_kwargs: object) -> None:
        raise AssertionError("창 비활성화 보호에서 대화상자가 호출됨")

    monkeypatch.setattr(main_window_module, "QMessageBox", fail_dialog)

    handled = QApplication.sendEvent(
        window,
        QEvent(QEvent.Type.WindowDeactivate),
    )

    assert handled
    assert repositories.get_draft(session.draft_id) is None
    assert page.editor.status is EditorStatus.SAVE_FAILED
    assert len(repositories.list_revisions(card.id)) == revision_count
    QApplication.sendEvent(page.editor, QInputMethodEvent("", []))
    assert page.editor.save_current()


def test_document_switch_and_window_close_auto_save_dirty_sessions(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    window, _settings, first_page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )
    _commit_text(first_page.editor, "전환할 카드")
    assert first_page.editor.card_id is not None
    first_card = first_page.stream.card_model.card_at(
        first_page.stream.card_model.index(0)
    )
    assert first_card is not None
    assert first_page.open_card(first_card.id)
    first_page.editor.setPlainText("문서 전환에서 자동 저장할 본문")
    first_revision_count = len(repositories.list_revisions(first_card.id))

    def fail_switch_dialog() -> CloseChoice:
        raise AssertionError("정상 문서 전환에서 닫기 선택 대화가 호출됨")

    monkeypatch.setattr(first_page.editor, "_ask_close_choice", fail_switch_dialog)
    second_document = document_service.create_document(repositories, "종료할 문서")

    assert window.open_document_local(second_document.id)
    assert window.active_document_id == second_document.id
    switched = repositories.get_card(first_card.id)
    assert switched is not None
    assert switched.body == "문서 전환에서 자동 저장할 본문"
    assert (
        len(repositories.list_revisions(first_card.id))
        == first_revision_count + 1
    )

    second_page = window.active_document_page()
    assert second_page is not None
    _commit_text(second_page.editor, "종료할 카드")
    assert second_page.editor.card_id is not None
    second_card = second_page.stream.card_model.card_at(
        second_page.stream.card_model.index(0)
    )
    assert second_card is not None
    assert second_page.open_card(second_card.id)
    second_page.editor.setPlainText("창 닫기에서 자동 저장할 본문")
    second_revision_count = len(repositories.list_revisions(second_card.id))

    def fail_close_dialog() -> CloseChoice:
        raise AssertionError("정상 창 닫기에서 닫기 선택 대화가 호출됨")

    monkeypatch.setattr(second_page.editor, "_ask_close_choice", fail_close_dialog)

    assert window.close()
    closed = repositories.get_card(second_card.id)
    assert closed is not None
    assert closed.body == "창 닫기에서 자동 저장할 본문"
    assert closed.deleted_at_us is None
    assert (
        len(repositories.list_revisions(second_card.id))
        == second_revision_count + 1
    )
    assert not any(
        event.card_id == second_card.id and event.event_type is EventType.DELETE
        for event in repositories.list_events(second_card.document_id)
    )
    assert not window.isVisible()


def test_status_bar_never_reports_all_saved_for_dirty_or_failed_editor(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    window, _settings, page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )
    _commit_text(page.editor, "상태줄 원본")
    assert page.editor.card_id is not None
    card = page.stream.card_model.card_at(page.stream.card_model.index(0))
    assert card is not None
    assert page.open_card(card.id)

    page.editor.setPlainText("상태줄 dirty 본문")

    assert page.editor.session is not None
    assert page.editor.session.dirty
    assert "모든 변경 저장됨" not in window.statusBar().currentMessage()
    assert "편집 중" in window.statusBar().currentMessage()

    def fail_save(_session: object) -> object:
        raise RuntimeError("주입된 저장 실패")

    monkeypatch.setattr(page.editor._save_coordinator, "save", fail_save)
    monkeypatch.setattr(
        page.editor,
        "_ask_close_choice",
        lambda: CloseChoice.KEEP_EDITING,
    )

    assert not page.editor.request_close()
    assert page.editor.status is EditorStatus.SAVE_FAILED
    assert "모든 변경 저장됨" not in window.statusBar().currentMessage()
    assert "저장 실패" in window.statusBar().currentMessage()

    assert page.editor.request_close(
        choice_provider=lambda _session: CloseChoice.DISCARD
    )
    assert window.close()


def test_settings_dialog_persists_every_p0_value_and_applies_to_page(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    window, settings, page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )
    policy_store = DataPolicySettingsStore(database)
    initial_split_policy = policy_store.load().split_policy
    dialog = SettingsDialog(
        settings,
        policy_store=policy_store,
        parent=window,
    )
    qtbot.addWidget(dialog)
    dialog.time_format.setCurrentText("yyyy-MM-dd HH:mm:ss")
    dialog.timezone.setCurrentText("UTC")
    dialog.font_size.setValue(15)
    dialog.line_spacing.setValue(1.5)
    dialog.draft_idle_seconds.setValue(3.5)
    dialog.preview_lines.setValue(9)
    dialog.backup_location.setText(str(tmp_path / "backups"))
    dialog.backup_interval.setValue(12)
    dialog.trash_retention_days.setValue(45)
    dialog.settings_applied.connect(window._apply_settings)
    idle_ms_calls: list[int] = []
    set_idle_ms = page.draft_coordinator.set_idle_ms

    def observe_idle_ms(idle_ms: int) -> None:
        idle_ms_calls.append(idle_ms)
        set_idle_ms(idle_ms)

    monkeypatch.setattr(page.draft_coordinator, "set_idle_ms", observe_idle_ms)

    dialog.apply_settings()

    assert settings.value("display/time_format") == "yyyy-MM-dd HH:mm:ss"
    assert settings.value("display/timezone") == "UTC"
    assert int(str(settings.value("editor/font_size"))) == 15
    assert float(str(settings.value("editor/line_spacing"))) == 1.5
    assert settings.value("backup/location") == str(tmp_path / "backups")
    policy = policy_store.load()
    assert policy.draft_idle_ms == 3_500
    assert idle_ms_calls == [3_500]
    assert policy.split_policy == initial_split_policy
    assert policy.preview_lines == 9
    assert policy.backup_interval_hours == 12
    assert policy.trash_retention_days == 45
    assert page.editor.font().pointSize() == 15


def test_import_action_and_dialog_are_owned_by_request_window(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    first_document = document_service.create_document(repositories, "첫 요청 문서")
    second_document = document_service.create_document(repositories, "둘째 요청 문서")
    settings = QSettings(
        str(tmp_path / "action-owner-settings.ini"),
        QSettings.Format.IniFormat,
    )
    initialize_device_settings(settings)
    settings.setValue("first_run/guide_shown", True)
    manager = WindowManager(AppContext(database, settings))
    first = manager.create_window(initial_document_id=first_document.id)
    second = manager.create_window(initial_document_id=second_document.id)
    qtbot.addWidget(first)
    qtbot.addWidget(second)
    first_window_id = first.window_id
    second_window_id = second.window_id
    approved_path = tmp_path / "승인된-가져오기.txt"
    dialog_requests: list[tuple[int, Path]] = []
    import_requests: list[tuple[str, object, Path]] = []

    class FakeImportDialog:
        def __init__(self, parent: MainWindow) -> None:
            self._parent = parent
            dialog_requests.append((id(parent), approved_path))

        @property
        def selected_path(self) -> Path:
            return approved_path

        def exec(self) -> QDialog.DialogCode:
            return QDialog.DialogCode.Accepted

    def observe_start_import(
        request_window_id: str,
        document_id: object,
        path: Path,
    ) -> None:
        import_requests.append((request_window_id, document_id, path))

    monkeypatch.setattr(main_window_module, "ImportDialog", FakeImportDialog)
    monkeypatch.setattr(manager.import_controller, "start_import", observe_start_import)
    first_action = first.import_action
    second_action = second.import_action

    try:
        assert first_action is not second_action
        assert first_action.parent() is first
        assert second_action.parent() is second
        assert first.import_controller is manager.import_controller
        assert second.import_controller is manager.import_controller

        first_action.trigger()

        assert dialog_requests == [(id(first), approved_path)]
        assert import_requests == [
            (first_window_id, first_document.id, approved_path)
        ]

        assert first.close()
        QApplication.sendPostedEvents(first, QEvent.Type.DeferredDelete)
        QApplication.processEvents()
        assert not isValid(first)
        assert first not in manager.windows
        assert isValid(second_action)
        assert isValid(manager.import_controller)
        assert manager.import_controller.parent() is manager

        second_action.trigger()

        assert len(dialog_requests) == 2
        assert dialog_requests[0][1] == approved_path
        assert dialog_requests[1] == (id(second), approved_path)
        assert import_requests == [
            (first_window_id, first_document.id, approved_path),
            (second_window_id, second_document.id, approved_path),
        ]
    finally:
        manager.prepare_shutdown()
        for window in tuple(manager.windows):
            window.close()
        QApplication.processEvents()


def test_direct_main_window_has_no_import_controller_fallback(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    window, _settings, _page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )

    assert window.import_controller is None
    assert not window.import_action.isEnabled()
    assert window.findChildren(ImportController) == []


def _watch_scroll(page: DocumentPage, monkeypatch: MonkeyPatch) -> list[str]:
    """스트림의 scrollTo 대상 카드 ID를 호출 순서대로 수집한다."""
    calls: list[str] = []
    original = page.stream.scrollTo

    def spy(
        index: QModelIndex,
        hint: QAbstractItemView.ScrollHint = (
            QAbstractItemView.ScrollHint.EnsureVisible
        ),
    ) -> None:
        calls.append(str(index.data(CardRole.CARD_ID)))
        original(index, hint)

    monkeypatch.setattr(page.stream, "scrollTo", spy)
    return calls


def _import_setup(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> tuple[WindowManager, MainWindow, DocumentPage, CardService, Card, list[str]]:
    """가져오기 완료 시험용 창을 준비한다.

    목록이 뷰포트를 넘도록 카드를 채우고 중간 카드를 연결해 스크롤을 0이 아닌
    위치에 둔다. 정렬은 최초 기록순이라 새 카드가 마지막 행에 붙으므로, 드러내기
    없이는 새 카드가 화면 밖에 남는다.
    """
    settings = QSettings(
        str(tmp_path / "import-settings.ini"),
        QSettings.Format.IniFormat,
    )
    initialize_device_settings(settings)
    settings.setValue("first_run/guide_shown", True)
    manager = WindowManager(AppContext(database, settings))
    window = manager.create_window()
    qtbot.addWidget(window)
    window.show()
    page = window.active_document_page()
    assert page is not None
    service = CardService(database, repositories)
    for number in range(24):
        service.create_cards(page.document_id, f"먼저 있던 카드 {number:02d}")
    page.refresh()
    page.sort_combo.setCurrentIndex(page.sort_combo.findData("capture"))
    anchor = repositories.list_cards(page.document_id)[12]
    assert page.open_card(anchor.id)
    assert page.stream.verticalScrollBar().value() > 0
    return manager, window, page, service, anchor, _watch_scroll(page, monkeypatch)


def test_menu_import_reveals_created_card_without_changing_edit_session(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    manager, window, page, service, anchor, scroll_calls = _import_setup(
        qtbot,
        database,
        repositories,
        tmp_path,
        monkeypatch,
    )
    _commit_text(page.editor, " 편집 중")
    page.editor.setFocus()
    session = page.editor.session
    assert session is not None and session.dirty
    before_body = page.editor.toPlainText()
    imported = service.create_cards(
        page.document_id,
        "가져온 카드",
        source=CaptureOperationSource.IMPORT,
    )

    manager._route_import_completed(window.window_id, imported)

    target = page.stream.card_model.index_for_card(imported[-1].id)
    assert page.stream.currentIndex().data(CardRole.CARD_ID) == imported[-1].id
    assert scroll_calls and set(scroll_calls) == {imported[-1].id}
    # 호출 관측만으로는 화면에 들어왔음을 증명하지 못하므로 행 사각형이 실제로
    # 뷰포트와 겹치는지 본다.
    assert page.stream.viewport().rect().intersects(page.stream.visualRect(target))
    assert page.editor.card_id == anchor.id
    current_session = page.editor.session
    assert current_session is not None
    assert current_session is session and current_session.dirty
    assert page.editor.toPlainText() == before_body
    assert page.focusWidget() is page.editor
    # dirty 세션을 그대로 두면 teardown의 창 닫기가 저장 경로를 태우고 지연 Qt
    # 이벤트가 다음 시험으로 새어 나간다.
    assert page.editor.request_close()


def test_menu_import_hidden_by_filter_keeps_selection_and_filter(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    manager, window, page, service, anchor, scroll_calls = _import_setup(
        qtbot,
        database,
        repositories,
        tmp_path,
        monkeypatch,
    )
    # 필터 전환이 모델을 reset하므로 보존 대상 선택을 그 뒤에 다시 만든다.
    page.source_filter.setCurrentIndex(page.source_filter.findData("typing"))
    assert page.reveal_card(anchor.id)
    before_id = page.stream.currentIndex().data(CardRole.CARD_ID)
    before_scroll = page.stream.verticalScrollBar().value()
    assert before_id == anchor.id
    assert before_scroll > 0
    scroll_calls.clear()
    imported = service.create_cards(
        page.document_id,
        "필터에 숨은 가져오기",
        source=CaptureOperationSource.IMPORT,
    )

    manager._route_import_completed(window.window_id, imported)

    assert page.stream.card_model.rowCount() == 24
    assert page.stream.card_model.index_for_card(imported[-1].id).isValid() is False
    assert page.stream.currentIndex().data(CardRole.CARD_ID) == before_id
    assert page.stream.verticalScrollBar().value() == before_scroll
    assert page.source_filter.currentData() == "typing"
    assert page.editor.card_id == anchor.id


def test_menu_import_into_other_document_is_not_tracked(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    manager, window, page, service, _anchor, _scroll_calls = _import_setup(
        qtbot,
        database,
        repositories,
        tmp_path,
        monkeypatch,
    )
    # 가져오기를 시작한 문서를 떠나 다른 문서를 활성으로 만든 뒤 완료가 도착하는
    # 실제 순서를 구성한다.
    started_document_id = page.document_id
    other = document_service.create_document(repositories, "완료 전에 옮겨간 문서")
    for number in range(24):
        service.create_cards(other.id, f"다른 문서 카드 {number:02d}")
    assert window.open_document_local(other.id)
    active = window.active_document_page()
    assert active is not None and active.document_id == other.id
    active.sort_combo.setCurrentIndex(active.sort_combo.findData("capture"))
    other_anchor = repositories.list_cards(other.id)[12]
    assert active.open_card(other_anchor.id)
    before_id = active.stream.currentIndex().data(CardRole.CARD_ID)
    before_scroll = active.stream.verticalScrollBar().value()
    before_status = window.statusBar().currentMessage()
    assert before_scroll > 0
    active_scroll_calls = _watch_scroll(active, monkeypatch)
    refresh_calls: list[None] = []
    refresh = active.refresh

    def observe_refresh() -> None:
        refresh_calls.append(None)
        refresh()

    monkeypatch.setattr(active, "refresh", observe_refresh)
    published: list[str] = []
    publish = manager.publish_document_change

    def observe_publish(document_id: str) -> None:
        published.append(document_id)
        publish(document_id)

    monkeypatch.setattr(manager, "publish_document_change", observe_publish)
    late = service.create_cards(
        started_document_id,
        "떠난 뒤에 도착한 카드",
        source=CaptureOperationSource.IMPORT,
    )

    manager._route_import_completed(window.window_id, late)

    assert refresh_calls == []
    assert active_scroll_calls == []
    assert published == [started_document_id]
    assert active.stream.card_model.index_for_card(late[-1].id).isValid() is False
    assert active.stream.currentIndex().data(CardRole.CARD_ID) == before_id
    assert active.stream.verticalScrollBar().value() == before_scroll
    assert active.editor.card_id == other_anchor.id
    assert window.statusBar().currentMessage() == before_status
    assert late[-1].document_id == started_document_id


def test_inactive_document_change_preserves_workspace_save_failure_message(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    window, _settings, page = _window(qtbot, database, repositories, tmp_path)
    inactive_document_id = page.document_id
    active_document = document_service.create_document(repositories, "활성 문서")
    assert window.open_document_local(active_document.id)
    save_workspace = window.save_workspace

    def fail_workspace_save() -> None:
        raise RuntimeError("바이트 보존 저장 실패")

    monkeypatch.setattr(window, "save_workspace", fail_workspace_save)
    window._save_workspace_from_ui()
    failure_message = window.statusBar().currentMessage()
    assert failure_message == "작업 상태 저장 실패: 바이트 보존 저장 실패"

    window.apply_document_change(inactive_document_id)

    observed_message = window.statusBar().currentMessage()
    monkeypatch.setattr(window, "save_workspace", save_workspace)
    assert window.close()
    assert observed_message.encode() == failure_message.encode()


def test_page_content_change_echo_guard_suppresses_self_echo_and_recovers(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    document = document_service.create_document(repositories, "내용 변경 에코 가드")
    settings = QSettings(
        str(tmp_path / "content-change-echo-settings.ini"),
        QSettings.Format.IniFormat,
    )
    initialize_device_settings(settings)
    settings.setValue("first_run/guide_shown", True)
    manager = WindowManager(AppContext(database, settings))
    window = manager.create_window(initial_document_id=document.id)
    qtbot.addWidget(window)
    page = window.page_for_document(document.id)
    assert page is not None
    refresh_calls: list[None] = []
    refresh = page.refresh

    def observe_refresh() -> None:
        refresh_calls.append(None)
        refresh()

    monkeypatch.setattr(page, "refresh", observe_refresh)

    try:
        page.content_changed.emit()

        # 0회는 페이지의 자기 발행이 같은 페이지 refresh로 echo되지 않았다는 뜻이다.
        assert refresh_calls == []
        assert window._publishing_page_content_change is False

        manager.publish_document_change(document.id)

        # 1회는 finally가 guard를 복구해 이후 독립 발행을 다시 받았다는 뜻이다.
        assert refresh_calls == [None]
    finally:
        manager.prepare_shutdown()
        for live_window in tuple(manager.windows):
            live_window.close()
        QApplication.processEvents()


@pytest.mark.parametrize(
    "payload_kind",
    ["non-tuple", "tuple-with-non-card", "empty-tuple"],
    ids=["non-tuple", "tuple-with-non-card", "empty-tuple"],
)
@pytest.mark.parametrize("request_alive", [True, False], ids=["live", "dead"])
def test_import_finished_rejects_invalid_payload_with_type_error(
    payload_kind: str,
    request_alive: bool,
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    settings = QSettings(
        str(tmp_path / "invalid-import-settings.ini"),
        QSettings.Format.IniFormat,
    )
    initialize_device_settings(settings)
    settings.setValue("first_run/guide_shown", True)
    manager = WindowManager(AppContext(database, settings))
    window = manager.create_window()
    qtbot.addWidget(window)
    survivor = manager.create_window()
    qtbot.addWidget(survivor)
    page = window.active_document_page()
    assert page is not None
    request_window_id = window.window_id
    if payload_kind == "non-tuple":
        invalid_payload: object = []
    elif payload_kind == "tuple-with-non-card":
        invalid_payload = (
            SimpleNamespace(
                id="not-a-card",
                document_id=page.document_id,
            ),
        )
    else:
        invalid_payload = ()

    class RejectingWindowRegistry:
        def get(self, _key: str, _default: object = None) -> MainWindow | None:
            raise AssertionError("payload 형식 검사보다 요청 창 조회가 먼저 실행됨")

    registry = manager._windows
    try:
        if not request_alive:
            assert window.close()
            QApplication.sendPostedEvents(window, QEvent.Type.DeferredDelete)
            QApplication.processEvents()
            assert not isValid(window)
            assert window not in manager.windows
            manager._windows = cast(
                dict[str, MainWindow],
                RejectingWindowRegistry(),
            )

        if payload_kind == "empty-tuple":
            with pytest.raises(IndexError):
                manager._route_import_completed(request_window_id, invalid_payload)
        else:
            with pytest.raises(TypeError, match="Card 튜플"):
                manager._route_import_completed(request_window_id, invalid_payload)
    finally:
        manager._windows = registry
        manager.prepare_shutdown()
        for live_window in tuple(manager.windows):
            live_window.close()
        QApplication.processEvents()


def test_menu_import_without_active_page_publishes_payload_document(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    document = document_service.create_document(repositories, "무페이지 가져오기")
    created = CardService(database, repositories).create_cards(
        document.id,
        "무페이지 완료 카드",
        source=CaptureOperationSource.IMPORT,
    )
    published: list[str] = []
    settings = QSettings(
        str(tmp_path / "no-page-settings.ini"),
        QSettings.Format.IniFormat,
    )
    initialize_device_settings(settings)
    settings.setValue("first_run/guide_shown", True)
    repositories.save_workspace_window("no-page-window", (), None)
    manager = WindowManager(AppContext(database, settings))
    window = manager._create_window("no-page-window", create_row=False)
    qtbot.addWidget(window)
    window.show()
    assert window.active_document_page() is None
    before_status = window.statusBar().currentMessage()
    publish = manager.publish_document_change

    def observe_publish(document_id: str) -> None:
        published.append(document_id)
        publish(document_id)

    monkeypatch.setattr(manager, "publish_document_change", observe_publish)

    manager._route_import_completed(window.window_id, created)

    assert published == [document.id]
    assert window.active_document_page() is None
    assert before_status == "문서를 선택하거나 새 문서를 만드세요."
    assert window.statusBar().currentMessage() == before_status


def test_menu_import_from_history_view_returns_to_card_surface(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    manager, window, page, service, anchor, scroll_calls = _import_setup(
        qtbot,
        database,
        repositories,
        tmp_path,
        monkeypatch,
    )
    page.show_history()
    assert page.mode_stack.currentWidget() is page.history
    imported = service.create_cards(
        page.document_id,
        "이력 화면에서 가져온 카드",
        source=CaptureOperationSource.IMPORT,
    )

    manager._route_import_completed(window.window_id, imported)

    target = page.stream.card_model.index_for_card(imported[-1].id)
    assert page.mode_stack.currentWidget() is page.editor_workspace
    assert page.stream.currentIndex().data(CardRole.CARD_ID) == imported[-1].id
    assert scroll_calls and set(scroll_calls) == {imported[-1].id}
    assert page.stream.viewport().rect().intersects(page.stream.visualRect(target))
    assert page.editor.card_id == anchor.id


def test_menu_import_from_real_controller_reaches_the_window_consumer(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    """생산자(ImportController)와 소비자(MainWindow)를 한 시험에서 연결한다.

    payload를 합성해 emit하는 시험만 있으면 신호가 내주는 실제 형태가 바뀌어도
    양쪽이 초록으로 남는다. 여기서는 파일 선택 이후의 실제 경로를 그대로 태운다.
    """
    manager, window, page, service, anchor, scroll_calls = _import_setup(
        qtbot,
        database,
        repositories,
        tmp_path,
        monkeypatch,
    )
    del service
    _commit_text(page.editor, " 실제 경로 편집 중")
    page.editor.setFocus()
    session = page.editor.session
    assert session is not None and session.dirty
    before_body = page.editor.toPlainText()
    path = tmp_path / "end-to-end.txt"
    path.write_text("실제 경로로 가져온 카드", encoding="utf-8")
    payloads: list[object] = []
    failures: list[str] = []
    published: list[str] = []
    publish = manager.publish_document_change

    def observe_publish(document_id: str) -> None:
        published.append(document_id)
        publish(document_id)

    monkeypatch.setattr(manager, "publish_document_change", observe_publish)
    manager.import_controller.imported.connect(payloads.append)
    manager.import_controller.failed.connect(failures.append)
    scroll_calls.clear()

    manager.import_controller.start_import(window.window_id, page.document_id, path)

    qtbot.waitUntil(lambda: bool(payloads or failures))
    assert failures == []
    created = payloads[0]
    # 소비자가 tuple을 전제로 payload를 해석하므로 형태 자체가 계약이다.
    assert isinstance(created, tuple)
    assert len(created) == 1
    card = created[0]
    assert isinstance(card, Card)
    assert card.body == "실제 경로로 가져온 카드"
    assert card.document_id == page.document_id
    stored = repositories.get_card(card.id)
    assert stored is not None
    assert stored.document_id == page.document_id
    assert stored.body == card.body
    assert published == [page.document_id]
    index = page.stream.card_model.index_for_card(card.id)
    assert page.stream.currentIndex().data(CardRole.CARD_ID) == card.id
    assert scroll_calls and set(scroll_calls) == {card.id}
    assert page.stream.viewport().rect().intersects(page.stream.visualRect(index))
    assert page.editor.card_id == anchor.id
    current_session = page.editor.session
    assert current_session is not None
    assert current_session is session and current_session.dirty
    assert page.editor.toPlainText() == before_body
    assert page.focusWidget() is page.editor
    assert page.editor.request_close()
