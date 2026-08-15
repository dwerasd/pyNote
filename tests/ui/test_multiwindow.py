from __future__ import annotations

import logging
import sqlite3
from collections.abc import Callable
from dataclasses import replace
from pathlib import Path
from typing import cast

import pytest
from PySide6.QtCore import (
    QByteArray,
    QEvent,
    QRunnable,
    QSettings,
    Qt,
    QThreadPool,
)
from PySide6.QtWidgets import QApplication, QInputDialog, QMessageBox
from pytest import MonkeyPatch
from pytestqt.qtbot import QtBot
from shiboken6 import isValid

from pynote import app as app_module
from pynote.app import AppContext, SqliteWorkspaceStateStore, WindowManager
from pynote.application.card_service import CardService
from pynote.application.draft_coordinator import (
    DraftDisposition,
    RecoveryCandidate,
)
from pynote.application.purge_service import PurgeService
from pynote.domain.events import EventType
from pynote.domain.models import (
    CaptureOperationSource,
    Card,
    CardSource,
    Document,
    Draft,
    DraftKind,
)
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories, text_hash
from pynote.infrastructure.settings import (
    LEGACY_WINDOW_GEOMETRY_KEY,
    migrate_legacy_window_geometry,
    window_geometry_key,
)
from pynote.ui import main_window as main_window_module
from pynote.ui.cards.card_model import CardRole
from pynote.ui.document_page import DocumentPage
from pynote.ui.editor.card_editor import CardEditor, CloseChoice
from pynote.ui.main_window import DocumentUiState, MainWindow, WorkspaceState
from pynote.ui.panels.document_navigator import DocumentNavigator, DocumentView


def _document(
    repositories: Repositories,
    document_id: str,
    title: str,
) -> Document:
    document = Document(
        id=document_id,
        title=title,
        created_at_us=1,
        updated_at_us=1,
    )
    repositories.create_document(document)
    return document


def _context(
    database: Database,
    tmp_path: Path,
) -> tuple[AppContext, QSettings]:
    settings = QSettings(
        str(tmp_path / "multiwindow.ini"),
        QSettings.Format.IniFormat,
    )
    settings.setValue("first_run/guide_shown", True)
    return AppContext(database, settings), settings


def _register_windows(qtbot: QtBot, manager: WindowManager) -> None:
    for window in manager.windows:
        qtbot.addWidget(window)
        window.show()


class _ManualThreadPool:
    def __init__(self) -> None:
        self._workers: list[QRunnable] = []
        self.wait_calls: list[int] = []

    def start(self, worker: QRunnable) -> None:
        self._workers.append(worker)

    @property
    def pending_count(self) -> int:
        return len(self._workers)

    def complete(self, index: int = 0) -> None:
        assert self._workers
        self._workers.pop(index).run()

    def waitForDone(self, msecs: int = -1) -> bool:
        self.wait_calls.append(msecs)
        return not self._workers


def _shutdown_manager(manager: WindowManager) -> None:
    manager.prepare_shutdown()
    for window in manager.windows:
        window.close()
    QApplication.processEvents()


def _navigator(window: MainWindow) -> DocumentNavigator:
    window._open_document_list()
    dialog = window._document_list_dialog
    assert dialog is not None
    return dialog.navigator


def _dirty_card(
    manager: WindowManager,
    owner: MainWindow,
    database: Database,
    repositories: Repositories,
    document_id: str,
) -> Card:
    card = CardService(database, repositories, clock=lambda: 10).create_card(
        document_id,
        "확정 본문",
    )
    assert manager.open_document(owner, document_id)
    page = owner.page_for_document(document_id)
    assert page is not None
    assert page.open_card(card.id)
    page.editor.setPlainText("미저장 본문")
    return card


def _save_empty_connected_card(
    page: DocumentPage,
    repositories: Repositories,
) -> Card:
    card = page.card_service.create_card(page.document_id, "비울 확정 본문")
    assert page.open_card(card.id)
    page.editor.setPlainText("")
    assert page.editor.save_current()
    stored = repositories.get_card(card.id)
    assert stored is not None
    assert stored.body == ""
    assert stored.deleted_at_us is None
    return stored


def _delete_event_count(repositories: Repositories, card: Card) -> int:
    return sum(
        event.card_id == card.id and event.event_type is EventType.DELETE
        for event in repositories.list_events(card.document_id)
    )


def _recovery_card(
    database: Database,
    repositories: Repositories,
    document_id: str,
    number: int,
) -> tuple[Card, Draft]:
    card = CardService(database, repositories, clock=lambda: 10 + number).create_card(
        document_id,
        f"확정 본문 {number}",
    )
    draft_text = f"미저장 본문 {number}"
    draft = Draft(
        id=f"batched-recovery-draft-{number}",
        document_id=document_id,
        card_id=card.id,
        draft_kind=DraftKind.EDIT,
        base_revision_id=card.current_revision_id,
        draft_text=draft_text,
        draft_hash=text_hash(draft_text),
        cursor_position_qchar=3,
        updated_at_us=20 + number,
    )
    repositories.create_draft(draft)
    return card, draft


def test_real_window_close_cleans_empty_after_state_save_and_restarts_blank(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    document = _document(repositories, "exit-window-document", "창 종료 문서")
    context, settings = _context(database, tmp_path)
    manager = WindowManager(context)
    window = manager.create_window(initial_document_id=document.id)
    window.show()
    page = window.page_for_document(document.id)
    assert page is not None
    empty = _save_empty_connected_card(page, repositories)
    retained = page.card_service.create_card(document.id, "남아야 하는 본문")
    state_store = SqliteWorkspaceStateStore(database, window.window_id)
    sequence: list[str] = []
    persist_ui = window.persist_open_page_ui_states
    save_workspace = window.save_workspace
    cleanup = window.cleanup_empty_card_before_exit

    def observe_persist_ui() -> None:
        sequence.append("persist-ui")
        persist_ui()

    def observe_save_workspace() -> WorkspaceState:
        sequence.append("save-workspace")
        return save_workspace()

    def observe_cleanup() -> None:
        sequence.append("cleanup")
        cleanup()

    monkeypatch.setattr(window, "persist_open_page_ui_states", observe_persist_ui)
    monkeypatch.setattr(window, "save_workspace", observe_save_workspace)
    monkeypatch.setattr(window, "cleanup_empty_card_before_exit", observe_cleanup)

    assert window.close()
    assert not window.isVisible()
    assert sequence[:3] == ["persist-ui", "save-workspace", "cleanup"]
    deleted = repositories.get_card(empty.id)
    assert deleted is not None
    assert deleted.deleted_at_us is not None
    assert _delete_event_count(repositories, empty) == 1
    remaining = repositories.get_card(retained.id)
    assert remaining is not None
    assert remaining.deleted_at_us is None
    assert _delete_event_count(repositories, retained) == 0
    saved_state = state_store.load_document_ui_state(document.id)
    assert saved_state is not None
    assert saved_state.editor_card_id == empty.id

    restarted = MainWindow(
        repositories,
        state_store,
        settings=settings,
        window_id=window.window_id,
    )
    qtbot.addWidget(restarted)
    restarted.show()
    restarted_page = restarted.page_for_document(document.id)
    assert restarted_page is not None
    assert restarted_page.editor.session is None
    assert restarted_page.editor.card_id is None
    assert restarted_page.editor.toPlainText() == ""


def test_app_quit_approval_cleans_all_windows_after_all_state_saves(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    documents = tuple(
        _document(
            repositories,
            f"app-exit-document-{number}",
            f"앱 종료 문서 {number}",
        )
        for number in range(1, 4)
    )
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    windows = tuple(
        manager.create_window(initial_document_id=document.id)
        for document in documents
    )
    _register_windows(qtbot, manager)
    empty_cards: list[Card] = []
    for window, document in zip(windows[:2], documents[:2], strict=True):
        page = window.page_for_document(document.id)
        assert page is not None
        empty_cards.append(_save_empty_connected_card(page, repositories))
    nonempty_page = windows[2].page_for_document(documents[2].id)
    assert nonempty_page is not None
    nonempty = nonempty_page.card_service.create_card(
        documents[2].id,
        "종료 뒤에도 남을 본문",
    )
    assert nonempty_page.open_card(nonempty.id)
    sequence: list[str] = []

    def observe(action: Callable[[], None], label: str) -> Callable[[], None]:
        def wrapped() -> None:
            sequence.append(label)
            action()

        return wrapped

    for number, window in enumerate(windows, start=1):
        monkeypatch.setattr(
            window,
            "persist_open_page_ui_states",
            observe(window.persist_open_page_ui_states, f"persist-{number}"),
        )
        monkeypatch.setattr(
            window,
            "cleanup_empty_card_before_exit",
            observe(window.cleanup_empty_card_before_exit, f"cleanup-{number}"),
        )

    assert manager.can_quit_application()
    assert sequence == [
        "persist-1",
        "persist-2",
        "persist-3",
        "cleanup-1",
        "cleanup-2",
        "cleanup-3",
    ]
    for empty in empty_cards:
        deleted = repositories.get_card(empty.id)
        assert deleted is not None
        assert deleted.deleted_at_us is not None
        assert _delete_event_count(repositories, empty) == 1
    remaining = repositories.get_card(nonempty.id)
    assert remaining is not None
    assert remaining.deleted_at_us is None
    assert _delete_event_count(repositories, nonempty) == 0


@pytest.mark.parametrize("failure_kind", ["false", "exception"])
@pytest.mark.parametrize("exit_path", ["window", "application"])
def test_empty_cleanup_failure_never_blocks_window_or_app_exit(
    failure_kind: str,
    exit_path: str,
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
    caplog: pytest.LogCaptureFixture,
) -> None:
    document = _document(
        repositories,
        f"{exit_path}-{failure_kind}-document",
        "정리 실패 종료 문서",
    )
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    window = manager.create_window(initial_document_id=document.id)
    window.show()
    if exit_path == "application":
        qtbot.addWidget(window)
    page = window.page_for_document(document.id)
    assert page is not None
    empty = _save_empty_connected_card(page, repositories)

    if failure_kind == "false":
        monkeypatch.setattr(
            page.card_service,
            "soft_delete",
            lambda _card_id, **_kwargs: False,
        )
    else:

        def raise_delete(_card_id: str, **_kwargs: object) -> Card:
            raise RuntimeError("주입된 종료 정리 예외")

        monkeypatch.setattr(page.card_service, "soft_delete", raise_delete)

    with caplog.at_level("WARNING"):
        if exit_path == "window":
            assert window.close()
            assert not window.isVisible()
        else:
            assert manager.can_quit_application()
            assert window.isVisible()

    remaining = repositories.get_card(empty.id)
    assert remaining is not None
    assert remaining.deleted_at_us is None
    assert _delete_event_count(repositories, empty) == 0
    assert "편집기 이탈을 계속합니다" in caplog.text


def test_window_close_continues_when_page_cleanup_entrypoint_raises(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
    caplog: pytest.LogCaptureFixture,
) -> None:
    document = _document(
        repositories,
        "window-page-cleanup-exception",
        "페이지 정리 예외 문서",
    )
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    window = manager.create_window(initial_document_id=document.id)
    window.show()
    page = window.page_for_document(document.id)
    assert page is not None
    empty = _save_empty_connected_card(page, repositories)

    def raise_cleanup() -> None:
        raise RuntimeError("주입된 페이지 정리 진입점 예외")

    monkeypatch.setattr(page, "cleanup_empty_card_before_exit", raise_cleanup)

    with caplog.at_level("ERROR"):
        assert window.close()

    assert not window.isVisible()
    remaining = repositories.get_card(empty.id)
    assert remaining is not None
    assert remaining.deleted_at_us is None
    assert _delete_event_count(repositories, empty) == 0
    assert "창 종료 전 빈 카드 정리 중 예외" in caplog.text


def test_app_quit_continues_after_window_cleanup_entrypoint_raises(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
    caplog: pytest.LogCaptureFixture,
) -> None:
    documents = (
        _document(repositories, "app-window-cleanup-failure", "실패 창 문서"),
        _document(repositories, "app-window-cleanup-next", "후속 창 문서"),
    )
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    windows = tuple(
        manager.create_window(initial_document_id=document.id)
        for document in documents
    )
    _register_windows(qtbot, manager)
    cards: list[Card] = []
    for window, document in zip(windows, documents, strict=True):
        page = window.page_for_document(document.id)
        assert page is not None
        cards.append(_save_empty_connected_card(page, repositories))

    def raise_cleanup() -> None:
        raise RuntimeError("주입된 창 정리 진입점 예외")

    monkeypatch.setattr(
        windows[0],
        "cleanup_empty_card_before_exit",
        raise_cleanup,
    )

    with caplog.at_level("ERROR"):
        assert manager.can_quit_application()

    first = repositories.get_card(cards[0].id)
    second = repositories.get_card(cards[1].id)
    assert first is not None
    assert second is not None
    assert first.deleted_at_us is None
    assert second.deleted_at_us is not None
    assert _delete_event_count(repositories, cards[0]) == 0
    assert _delete_event_count(repositories, cards[1]) == 1
    assert "앱 종료 전 빈 카드 정리에 실패했지만 종료 승인을 유지" in caplog.text


def test_two_windows_restore_documents_and_independent_geometry(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    first_document = _document(repositories, "document-1", "첫 문서")
    second_document = _document(repositories, "document-2", "둘째 문서")
    repositories.save_workspace_window(
        "window-1",
        (first_document.id,),
        first_document.id,
    )
    repositories.save_workspace_window(
        "window-2",
        (second_document.id,),
        second_document.id,
    )
    context, settings = _context(database, tmp_path)
    manager = WindowManager(context)
    manager.restore_windows()
    for window in manager.windows:
        window.show()
    first, second = manager.windows
    first.resize(1_048, 501)
    second.resize(1_048, 602)
    qtbot.wait(20)
    first_size = first.size()
    second_size = second.size()
    assert first_size != second_size
    first.persist_window_state()
    second.persist_window_state()
    assert settings.contains(window_geometry_key("window-1"))
    assert settings.contains(window_geometry_key("window-2"))

    manager.prepare_shutdown()
    assert first.close()
    assert second.close()

    restarted = WindowManager(context)
    restarted.restore_windows()
    _register_windows(qtbot, restarted)
    restored = {window.window_id: window for window in restarted.windows}
    assert restored["window-1"].open_document_ids == (first_document.id,)
    assert restored["window-2"].open_document_ids == (second_document.id,)
    assert restored["window-1"].size().height() == first_size.height()
    assert restored["window-2"].size().height() == second_size.height()


def test_open_document_activates_existing_owner_without_duplicate_page(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    document = _document(repositories, "owned-document", "소유 문서")
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    first = manager.create_window()
    second = manager.create_window()
    second_document_ids = second.open_document_ids
    _register_windows(qtbot, manager)
    activated: list[MainWindow] = []
    monkeypatch.setattr(
        app_module,
        "_activate_window",
        lambda window: activated.append(window),
    )

    assert manager.open_document(first, document.id)
    assert manager.open_document(second, document.id)

    assert first.open_document_ids == (document.id,)
    assert len(second_document_ids) == 1
    assert second.open_document_ids == second_document_ids
    assert document.id not in second.open_document_ids
    assert activated == [first]


def test_create_window_protects_live_windows_before_creation(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    document = _document(repositories, "create-protect", "새 창 전환 보호")
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    first = manager.create_window(initial_document_id=document.id)
    qtbot.addWidget(first)
    _dirty_card(manager, first, database, repositories, document.id)
    page = first.page_for_document(document.id)
    assert page is not None
    session = page.editor.session
    assert session is not None
    assert repositories.get_draft(session.draft_id) is None
    observed_window_counts: list[int] = []
    protect_quietly = first.protect_open_pages_quietly

    def observe_protection() -> None:
        observed_window_counts.append(len(manager.windows))
        protect_quietly()

    monkeypatch.setattr(first, "protect_open_pages_quietly", observe_protection)

    created = manager.create_window()
    qtbot.addWidget(created)

    stored = repositories.get_draft(session.draft_id)
    assert observed_window_counts
    assert observed_window_counts[0] == 1
    assert stored is not None
    assert stored.draft_text == "미저장 본문"


@pytest.mark.parametrize(
    "activation_path",
    ["open_document", "open_document_in_new_window", "open_search_result"],
)
def test_existing_window_activation_paths_protect_all_live_windows(
    activation_path: str,
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    owned = _document(repositories, "activation-owned", "활성화 소유 문서")
    requesting_document = _document(
        repositories,
        "activation-requesting",
        "활성화 요청 문서",
    )
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    owner = manager.create_window(initial_document_id=owned.id)
    requesting = manager.create_window(initial_document_id=requesting_document.id)
    _register_windows(qtbot, manager)
    _dirty_card(
        manager,
        requesting,
        database,
        repositories,
        requesting_document.id,
    )
    page = requesting.page_for_document(requesting_document.id)
    assert page is not None
    session = page.editor.session
    assert session is not None
    assert repositories.get_draft(session.draft_id) is None
    sequence: list[str] = []
    protect_quietly = requesting.protect_open_pages_quietly

    def observe_protection() -> None:
        sequence.append("protect")
        protect_quietly()

    monkeypatch.setattr(
        requesting,
        "protect_open_pages_quietly",
        observe_protection,
    )
    monkeypatch.setattr(
        app_module,
        "_activate_window",
        lambda _window: sequence.append("activate"),
    )

    if activation_path == "open_document":
        assert manager.open_document(requesting, owned.id)
    elif activation_path == "open_document_in_new_window":
        assert manager.open_document_in_new_window(requesting, owned.id)
    else:
        assert manager.open_search_result(owner, owned.id, None)

    stored = repositories.get_draft(session.draft_id)
    assert sequence == ["protect", "activate"]
    assert stored is not None
    assert stored.draft_text == "미저장 본문"


def test_create_window_continues_when_quiet_protection_raises(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    document = _document(repositories, "create-failure", "새 창 실패 경계")
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    first = manager.create_window(initial_document_id=document.id)
    second = manager.create_window()
    qtbot.addWidget(first)
    qtbot.addWidget(second)
    card = _dirty_card(manager, first, database, repositories, document.id)
    revision_count = len(repositories.list_revisions(card.id))
    protected_after_failure: list[MainWindow] = []

    def fail_dialog(*_args: object, **_kwargs: object) -> None:
        raise AssertionError("새 창 보호 실패에서 대화상자가 호출됨")

    def fail_protection() -> None:
        raise sqlite3.OperationalError("주입된 draft write 실패")

    with monkeypatch.context() as patch:
        patch.setattr(main_window_module, "QMessageBox", fail_dialog)
        patch.setattr(first, "protect_open_pages_quietly", fail_protection)
        patch.setattr(
            second,
            "protect_open_pages_quietly",
            lambda: protected_after_failure.append(second),
        )
        created = manager.create_window()

    qtbot.addWidget(created)
    assert len(manager.windows) == 3
    assert protected_after_failure == [second]
    assert created in manager.windows
    assert len(repositories.list_revisions(card.id)) == revision_count


def test_existing_window_activation_continues_when_page_protection_raises(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    owned = _document(repositories, "failure-owned", "실패 경계 소유 문서")
    requesting_document = _document(
        repositories,
        "failure-requesting",
        "실패 경계 요청 문서",
    )
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    owner = manager.create_window(initial_document_id=owned.id)
    requesting = manager.create_window(initial_document_id=requesting_document.id)
    _register_windows(qtbot, manager)
    card = _dirty_card(
        manager,
        requesting,
        database,
        repositories,
        requesting_document.id,
    )
    page = requesting.page_for_document(requesting_document.id)
    assert page is not None
    revision_count = len(repositories.list_revisions(card.id))
    activated: list[MainWindow] = []

    def fail_dialog(*_args: object, **_kwargs: object) -> None:
        raise AssertionError("기존 창 활성화 보호 실패에서 대화상자가 호출됨")

    def fail_protection() -> bool:
        raise sqlite3.OperationalError("주입된 draft write 실패")

    with monkeypatch.context() as patch:
        patch.setattr(main_window_module, "QMessageBox", fail_dialog)
        patch.setattr(page, "protect_now", fail_protection)
        patch.setattr(
            app_module,
            "_activate_window",
            lambda window: activated.append(window),
        )
        assert manager.open_document(requesting, owned.id)

    assert activated == [owner]
    assert len(repositories.list_revisions(card.id)) == revision_count


def test_dialog_open_activates_existing_owner_without_creating_window(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    owned = _document(repositories, "dialog-owned", "소유 문서")
    requesting = _document(repositories, "dialog-requesting", "요청 문서")
    repositories.save_workspace_window("dialog-owner", (owned.id,), owned.id)
    repositories.save_workspace_window(
        "dialog-requester",
        (requesting.id,),
        requesting.id,
    )
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    manager.restore_windows()
    for window in manager.windows:
        window.show()
    owner, requester = manager.windows
    activated: list[MainWindow] = []
    monkeypatch.setattr(
        app_module,
        "_activate_window",
        lambda window: activated.append(window),
    )
    dialog = requester._document_list_dialog
    assert dialog is None
    navigator = _navigator(requester)
    dialog = requester._document_list_dialog
    assert dialog is not None
    navigator._select_document(owned.id)
    item = navigator.document_list.currentItem()
    assert item is not None
    window_count = len(manager.windows)

    navigator._request_item_open(item)

    assert len(manager.windows) == window_count
    assert requester.open_document_ids == (requesting.id,)
    assert owner.open_document_ids == (owned.id,)
    assert activated == [owner]
    assert not dialog.isVisible()


def test_dialog_new_window_open_checks_existing_owner_before_creation(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    owned = _document(repositories, "new-window-owned", "소유 문서")
    requesting = _document(repositories, "new-window-requesting", "요청 문서")
    repositories.save_workspace_window("new-window-owner", (owned.id,), owned.id)
    repositories.save_workspace_window(
        "new-window-requester",
        (requesting.id,),
        requesting.id,
    )
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    manager.restore_windows()
    _register_windows(qtbot, manager)
    owner, requester = manager.windows
    activated: list[MainWindow] = []
    monkeypatch.setattr(
        app_module,
        "_activate_window",
        lambda window: activated.append(window),
    )
    navigator = _navigator(requester)
    dialog = requester._document_list_dialog
    assert dialog is not None
    navigator._select_document(owned.id)
    window_count = len(manager.windows)

    dialog.open_in_new_window_button.click()

    assert len(manager.windows) == window_count
    assert requester.open_document_ids == (requesting.id,)
    assert owner.open_document_ids == (owned.id,)
    assert activated == [owner]
    assert not dialog.isVisible()


def test_new_document_action_opens_created_document_in_new_window(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    requesting = manager.create_window()
    _register_windows(qtbot, manager)
    original_document_ids = requesting.open_document_ids
    existing_ids = {
        document.id for document in repositories.list_documents()
    }
    window_count = len(manager.windows)

    requesting.new_document_action.trigger()

    created_ids = {
        document.id for document in repositories.list_documents()
    } - existing_ids
    assert len(manager.windows) == window_count + 1
    assert len(created_ids) == 1
    assert requesting.open_document_ids == original_document_ids
    assert manager.windows[-1].open_document_ids == tuple(created_ids)


def test_dialog_new_window_open_rejects_archived_document_without_window(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    requesting = manager.create_window()
    _register_windows(qtbot, manager)
    archived = _document(repositories, "archived-target", "보관 문서")
    repositories.update_document(replace(archived, archived_at_us=2))
    original_document_ids = requesting.open_document_ids
    navigator = _navigator(requesting)
    dialog = requesting._document_list_dialog
    assert dialog is not None
    navigator.set_view(DocumentView.ARCHIVED)
    navigator._select_document(archived.id)
    window_count = len(manager.windows)

    dialog.open_in_new_window_button.click()

    assert len(manager.windows) == window_count
    assert requesting.open_document_ids == original_document_ids
    assert dialog.isVisible()


def test_dialog_stays_open_when_leave_gate_refuses_document_switch(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    window = manager.create_window()
    _register_windows(qtbot, manager)
    current_document_ids = window.open_document_ids
    other = _document(repositories, "switch-target", "전환 대상")
    page = window.active_document_page()
    assert page is not None
    monkeypatch.setattr(
        page,
        "can_leave_editor",
        lambda *, choice_provider=None, protect_now=False: False,
    )
    navigator = _navigator(window)
    dialog = window._document_list_dialog
    assert dialog is not None
    navigator.refresh()
    item = next(
        navigator.document_list.item(index)
        for index in range(navigator.document_list.count())
        if str(navigator.document_list.item(index).data(Qt.ItemDataRole.UserRole))
        == other.id
    )

    navigator._request_item_open(item)

    assert window.open_document_ids == current_document_ids
    assert dialog.isVisible()


def test_focus_mode_hides_open_document_list(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    window = manager.create_window()
    _register_windows(qtbot, manager)
    _navigator(window)
    dialog = window._document_list_dialog
    assert dialog is not None
    assert dialog.isVisible()

    window.focus_action.trigger()

    assert not dialog.isVisible()


def test_legacy_restore_retains_active_then_claims_document_in_next_record(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    first = _document(repositories, "legacy-a", "문서 A")
    second = _document(repositories, "legacy-b", "문서 B")
    repositories.save_workspace_window(
        "legacy-window-1",
        (first.id, second.id),
        first.id,
    )
    repositories.save_workspace_window(
        "legacy-window-2",
        (second.id,),
        second.id,
    )
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)

    manager.restore_windows()
    _register_windows(qtbot, manager)

    assert tuple(window.open_document_ids for window in manager.windows) == (
        (first.id,),
        (second.id,),
    )


def test_legacy_restore_uses_first_unclaimed_when_active_was_claimed(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    first = _document(repositories, "claimed-a", "문서 A")
    second = _document(repositories, "unclaimed-b", "문서 B")
    repositories.save_workspace_window(
        "claimed-window-1",
        (first.id,),
        first.id,
    )
    repositories.save_workspace_window(
        "claimed-window-2",
        (first.id, second.id),
        first.id,
    )
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)

    manager.restore_windows()
    _register_windows(qtbot, manager)

    assert tuple(window.open_document_ids for window in manager.windows) == (
        (first.id,),
        (second.id,),
    )


def test_search_result_opens_card_in_document_owner_window(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    owned = _document(repositories, "search-owned", "검색 소유 문서")
    requesting = _document(repositories, "search-requesting", "검색 요청 문서")
    service = CardService(database, repositories, clock=lambda: 10)
    dirty_card = service.create_card(
        owned.id,
        "검색 전에 편집할 카드",
    )
    result_card = service.create_card(
        owned.id,
        "소유 창에서만 열릴 검색 결과",
    )
    repositories.save_workspace_window("search-owner-window", (owned.id,), owned.id)
    repositories.save_workspace_window(
        "search-request-window",
        (requesting.id,),
        requesting.id,
    )
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    manager.restore_windows()
    _register_windows(qtbot, manager)
    owner, requester = manager.windows
    owner_page = owner.page_for_document(owned.id)
    assert owner_page is not None
    assert owner_page.open_card(dirty_card.id)
    edited_text = "검색 결과 activation 직전 미저장 본문"
    owner_page.editor.setPlainText(edited_text)
    revision_count = len(repositories.list_revisions(dirty_card.id))

    requester.search_dialog.search("소유 창에서만")
    result = requester.search_dialog.result_tree.topLevelItem(0)
    assert result is not None
    requester.search_dialog._activate(result)

    requester_page = requester.page_for_document(requesting.id)
    assert owner_page is not None
    assert requester_page is not None
    assert owner_page.editor.session is not None
    assert owner_page.editor.session.card_id == result_card.id
    stored_dirty = repositories.get_card(dirty_card.id)
    assert stored_dirty is not None
    assert stored_dirty.body == edited_text
    assert len(repositories.list_revisions(dirty_card.id)) == revision_count + 1
    assert requester_page.editor.session is None
    assert not requester_page.open_card(result_card.id)


def test_non_last_close_deletes_state_but_last_close_preserves_restore(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    context, settings = _context(database, tmp_path)
    manager = WindowManager(context)
    first = manager.create_window()
    second = manager.create_window()
    first.show()
    second.show()
    first_id = first.window_id
    second_id = second.window_id
    first.persist_window_state()
    second.persist_window_state()

    assert first.close()
    assert repositories.get_workspace_window(first_id) is None
    assert not settings.contains(window_geometry_key(first_id))
    assert repositories.get_workspace_window(second_id) is not None

    assert second.close()
    assert repositories.get_workspace_window(second_id) is not None
    assert settings.contains(window_geometry_key(second_id))

    restarted = WindowManager(context)
    restarted.restore_windows()
    _register_windows(qtbot, restarted)
    assert tuple(window.window_id for window in restarted.windows) == (second_id,)


def test_closed_non_last_window_disconnects_change_bus_before_publish(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    first_document = _document(repositories, "closed-window-document", "닫을 문서")
    second_document = _document(repositories, "live-window-document", "생존 문서")
    repositories.save_workspace_window(
        "closed-window",
        (first_document.id,),
        first_document.id,
    )
    repositories.save_workspace_window(
        "live-window",
        (second_document.id,),
        second_document.id,
    )
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    manager.restore_windows()
    for window in manager.windows:
        window.show()
    closed, live = manager.windows
    handlers_before = dict(manager._document_change_handlers)
    assert set(handlers_before) == {closed.window_id, live.window_id}
    assert len({id(handler) for handler in handlers_before.values()}) == 2
    deliveries: list[str] = []

    monkeypatch.setattr(
        closed,
        "apply_document_change",
        lambda _document_id: deliveries.append(closed.window_id),
    )
    monkeypatch.setattr(
        live,
        "apply_document_change",
        lambda _document_id: deliveries.append(live.window_id),
    )

    manager.publish_document_change(first_document.id)
    # 각 창별 handler 하나가 발행 한 번을 정확히 한 번씩 전달한다.
    assert deliveries == [closed.window_id, live.window_id]

    assert closed.close()
    assert repositories.get_workspace_window("closed-window") is None
    assert manager._document_change_handlers == {
        live.window_id: handlers_before[live.window_id]
    }
    manager.publish_document_change(first_document.id)

    assert repositories.get_workspace_window("closed-window") is None
    assert tuple(window.window_id for window in manager.windows) == ("live-window",)
    # 닫힌 창에는 추가 전달이 없고 생존 창 handler만 한 번 더 호출된다.
    assert deliveries == [closed.window_id, live.window_id, live.window_id]


def test_async_import_routes_payload_document_to_new_owner_without_tracking(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    started = _document(repositories, "import-started", "가져오기 대상")
    receiver_initial = _document(repositories, "receiver-initial", "수신 창 기존")
    source_active = _document(repositories, "source-active", "소스 창 활성")
    service = CardService(database, repositories)
    started_anchor = service.create_card(started.id, "A 기준 카드")
    service.create_card(receiver_initial.id, "B 기준 카드")
    service.create_card(source_active.id, "C 긴 기준 카드 하나")
    service.create_card(source_active.id, "C 긴 기준 카드 둘")
    service.create_card(source_active.id, "C 긴 기준 카드 셋")
    repositories.save_workspace_window("import-source", (started.id,), started.id)
    repositories.save_workspace_window(
        "import-receiver",
        (receiver_initial.id,),
        receiver_initial.id,
    )
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    manager.restore_windows()
    _register_windows(qtbot, manager)
    source, receiver = manager.windows
    manual_pool = _ManualThreadPool()
    manager.import_controller._thread_pool = cast(QThreadPool, manual_pool)
    payloads: list[object] = []
    failures: list[str] = []
    manager.import_controller.imported.connect(payloads.append)
    manager.import_controller.failed.connect(failures.append)
    path = tmp_path / "cross-window-import.txt"
    path.write_text("교차 창으로 도착한 카드", encoding="utf-8")

    manager.import_controller.start_import(source.window_id, started.id, path)
    assert manager.open_document(source, source_active.id)
    assert manager.open_document(receiver, started.id)
    source_page = source.page_for_document(source_active.id)
    receiver_page = receiver.page_for_document(started.id)
    assert source_page is not None
    assert receiver_page is not None
    assert receiver_page.open_card(started_anchor.id)
    receiver_page.show_cards()
    before_selected = receiver_page.stream.currentIndex().data(CardRole.CARD_ID)
    before_editor_card = receiver_page.editor.card_id
    before_mode = receiver_page.mode_stack.currentWidget()
    receiver_status = receiver.statusBar().currentMessage()
    source_status = source.statusBar().currentMessage()
    started_cards_before = repositories.list_cards(started.id)
    source_cards_before = repositories.list_cards(source_active.id)
    assert before_selected == started_anchor.id
    assert before_editor_card == started_anchor.id
    assert receiver_status == (
        f"{len(started_cards_before)}개 카드 · "
        f"{sum(len(card.body) for card in started_cards_before)}자 · "
        "모든 변경 저장됨 · 로컬 DB"
    )
    assert source_status == (
        f"{len(source_cards_before)}개 카드 · "
        f"{sum(len(card.body) for card in source_cards_before)}자 · "
        "모든 변경 저장됨 · 로컬 DB"
    )
    source_refresh_calls: list[None] = []
    source_refresh = source_page.refresh

    def observe_source_refresh() -> None:
        source_refresh_calls.append(None)
        source_refresh()

    monkeypatch.setattr(source_page, "refresh", observe_source_refresh)
    published: list[str] = []
    publish = manager.publish_document_change

    def observe_publish(document_id: str) -> None:
        published.append(document_id)
        publish(document_id)

    monkeypatch.setattr(manager, "publish_document_change", observe_publish)

    manual_pool.complete()
    qtbot.waitUntil(lambda: bool(payloads or failures))

    assert failures == []
    assert len(payloads) == 1
    created = cast(tuple[Card, ...], payloads[0])
    assert published == [started.id]
    assert source_refresh_calls == []
    assert receiver_page.stream.card_model.index_for_card(created[-1].id).isValid()
    assert source_page.stream.card_model.index_for_card(created[-1].id).isValid() is False
    # 일반 문서 refresh의 모델 reset은 기존 선택을 해제하지만 생성 카드를 추적하지 않는다.
    assert receiver_page.stream.currentIndex().data(CardRole.CARD_ID) is None
    assert receiver_page.stream.currentIndex().data(CardRole.CARD_ID) != created[-1].id
    assert receiver_page.editor.card_id == before_editor_card
    assert receiver_page.editor.card_id != created[-1].id
    assert receiver_page.mode_stack.currentWidget() is before_mode
    assert source.statusBar().currentMessage().encode() == source_status.encode()
    started_cards = repositories.list_cards(started.id)
    source_cards = repositories.list_cards(source_active.id)
    assert len(started_cards) == 2
    assert len(source_cards) == 3
    assert (len(started_cards), sum(len(card.body) for card in started_cards)) != (
        len(source_cards),
        sum(len(card.body) for card in source_cards),
    )
    assert receiver.statusBar().currentMessage() == (
        f"{len(started_cards)}개 카드 · "
        f"{sum(len(card.body) for card in started_cards)}자 · "
        "모든 변경 저장됨 · 로컬 DB"
    )


def test_pending_import_survives_destroyed_request_window_and_refreshes_new_owner(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    target = _document(repositories, "pending-import-target", "가져오기 대상")
    receiver_initial = _document(
        repositories,
        "pending-import-receiver",
        "수신 창 기존 문서",
    )
    CardService(database, repositories).create_card(target.id, "기존 카드")
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    source = manager.create_window(initial_document_id=target.id)
    receiver = manager.create_window(initial_document_id=receiver_initial.id)
    source.show()
    qtbot.addWidget(receiver)
    receiver.show()
    manual_pool = _ManualThreadPool()
    manager.import_controller._thread_pool = cast(QThreadPool, manual_pool)
    payloads: list[object] = []
    failures: list[str] = []
    manager.import_controller.imported.connect(payloads.append)
    manager.import_controller.failed.connect(failures.append)
    published: list[str] = []
    publish = manager.publish_document_change

    def observe_publish(document_id: str) -> None:
        published.append(document_id)
        publish(document_id)

    monkeypatch.setattr(manager, "publish_document_change", observe_publish)
    path = tmp_path / "pending-import.txt"
    path.write_text("닫힌 요청 창 뒤에도 저장될 카드", encoding="utf-8")

    try:
        manager.import_controller.start_import(source.window_id, target.id, path)
        assert source.close()
        QApplication.sendPostedEvents(source, QEvent.Type.DeferredDelete)
        QApplication.processEvents()
        assert source not in manager.windows
        assert not isValid(source)
        assert isValid(manager.import_controller)
        assert manager.import_controller.parent() is manager
        assert manager.open_document(receiver, target.id)
        receiver_page = receiver.page_for_document(target.id)
        assert receiver_page is not None

        with qtbot.captureExceptions() as exceptions:
            manual_pool.complete()
            QApplication.processEvents()

        assert exceptions == []
        assert failures == []
        assert len(payloads) == 1
        created = cast(tuple[Card, ...], payloads[0])
        assert len(created) == 1
        assert published == [target.id]
        assert tuple(
            card.id
            for card in repositories.list_cards(target.id)
            if card.source is CardSource.IMPORT
        ) == (created[0].id,)
        assert receiver_page.stream.card_model.index_for_card(
            created[0].id
        ).isValid()
        assert receiver_page.stream.currentIndex().data(CardRole.CARD_ID) != created[0].id
        assert receiver_page.editor.card_id != created[0].id
    finally:
        manager.prepare_shutdown()


def test_pending_import_without_document_owner_finishes_without_ui_error(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    target = _document(repositories, "ownerless-import-target", "소유자 없는 대상")
    survivor_document = _document(
        repositories,
        "ownerless-import-survivor",
        "무관한 생존 문서",
    )
    service = CardService(database, repositories)
    survivor_anchor = service.create_card(survivor_document.id, "생존 창 기준 카드")
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    source = manager.create_window(initial_document_id=target.id)
    survivor = manager.create_window(initial_document_id=survivor_document.id)
    source.show()
    survivor.show()
    survivor_page = survivor.page_for_document(survivor_document.id)
    assert survivor_page is not None
    assert survivor_page.open_card(survivor_anchor.id)
    before_model = tuple(
        survivor_page.stream.card_model.index(row, 0).data(CardRole.CARD_ID)
        for row in range(survivor_page.stream.card_model.rowCount())
    )
    before_selected = survivor_page.stream.currentIndex().data(CardRole.CARD_ID)
    before_editor_card = survivor_page.editor.card_id
    before_status = survivor.statusBar().currentMessage()
    manual_pool = _ManualThreadPool()
    manager.import_controller._thread_pool = cast(QThreadPool, manual_pool)
    imported: list[object] = []
    failed: list[str] = []
    published: list[str] = []
    critical_calls: list[tuple[object, ...]] = []
    manager.import_controller.imported.connect(imported.append)
    manager.import_controller.failed.connect(failed.append)
    publish = manager.publish_document_change

    def observe_publish(document_id: str) -> None:
        published.append(document_id)
        publish(document_id)

    monkeypatch.setattr(manager, "publish_document_change", observe_publish)
    monkeypatch.setattr(
        QMessageBox,
        "critical",
        lambda *args, **_kwargs: critical_calls.append(args),
    )
    path = tmp_path / "ownerless-import.txt"
    path.write_text("소유 창 없이 완료될 카드", encoding="utf-8")

    try:
        manager.import_controller.start_import(source.window_id, target.id, path)
        assert source.close()
        QApplication.sendPostedEvents(source, QEvent.Type.DeferredDelete)
        QApplication.processEvents()
        assert source not in manager.windows
        assert not isValid(source)
        assert survivor.active_document_id == survivor_document.id

        with qtbot.captureExceptions() as exceptions:
            manual_pool.complete()
            QApplication.processEvents()

        assert exceptions == []
        assert failed == []
        assert len(imported) == 1
        created = cast(tuple[Card, ...], imported[0])
        assert len(created) == 1
        assert created[0].source is CardSource.IMPORT
        assert published == [target.id]
        assert tuple(
            card.id
            for card in repositories.list_cards(target.id)
            if card.source is CardSource.IMPORT
        ) == (created[0].id,)
        assert critical_calls == []
        assert survivor.active_document_id == survivor_document.id
        after_model = tuple(
            survivor_page.stream.card_model.index(row, 0).data(CardRole.CARD_ID)
            for row in range(survivor_page.stream.card_model.rowCount())
        )
        assert after_model == before_model
        assert (
            survivor_page.stream.currentIndex().data(CardRole.CARD_ID)
            == before_selected
        )
        assert survivor_page.editor.card_id == before_editor_card
        assert survivor.statusBar().currentMessage() == before_status
    finally:
        _shutdown_manager(manager)


@pytest.mark.parametrize(
    "request_state",
    ["live", "dead_deferred_pending", "dead_deferred_done"],
)
@pytest.mark.parametrize(
    "failure_origin",
    ["file_preparation", "document_id_validation", "database_commit"],
)
def test_import_failure_routes_modal_only_to_live_request_window(
    request_state: str,
    failure_origin: str,
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
    caplog: pytest.LogCaptureFixture,
) -> None:
    target = _document(
        repositories,
        f"failure-{request_state}-{failure_origin}",
        "실패 가져오기 대상",
    )
    survivor_document = _document(
        repositories,
        f"failure-survivor-{request_state}-{failure_origin}",
        "실패와 무관한 생존 문서",
    )
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    requester = manager.create_window(initial_document_id=target.id)
    survivor = manager.create_window(initial_document_id=survivor_document.id)
    requester.show()
    survivor.show()
    requester_id = requester.window_id
    manual_pool = _ManualThreadPool()
    manager.import_controller._thread_pool = cast(QThreadPool, manual_pool)
    failed: list[str] = []
    critical_calls: list[tuple[object, ...]] = []
    manager.import_controller.failed.connect(failed.append)
    monkeypatch.setattr(
        QMessageBox,
        "critical",
        lambda *args, **_kwargs: critical_calls.append(args),
    )
    path = tmp_path / f"failure-{request_state}-{failure_origin}.txt"
    if failure_origin != "file_preparation":
        path.write_text("DB 확정까지 갈 가져오기", encoding="utf-8")
    if failure_origin == "database_commit":

        def fail_database_commit(*_args: object, **_kwargs: object) -> tuple[Card, ...]:
            raise sqlite3.IntegrityError("주입된 가져오기 DB 확정 실패")

        monkeypatch.setattr(
            manager.import_controller._service,
            "create_cards",
            fail_database_commit,
        )

    def remove_requester_from_registry() -> None:
        if request_state == "live":
            return
        assert requester.close()
        assert requester not in manager.windows
        if request_state == "dead_deferred_pending":
            assert isValid(requester)
            return
        QApplication.sendPostedEvents(requester, QEvent.Type.DeferredDelete)
        QApplication.processEvents()
        assert not isValid(requester)

    try:
        caplog.clear()
        with caplog.at_level(logging.ERROR), qtbot.captureExceptions() as exceptions:
            if failure_origin == "document_id_validation":
                remove_requester_from_registry()
                manager.import_controller.start_import(requester_id, object(), path)
                assert manual_pool.pending_count == 0
            else:
                manager.import_controller.start_import(requester_id, target.id, path)
                assert manual_pool.pending_count == 1
                remove_requester_from_registry()
                manual_pool.complete()
            QApplication.processEvents()

        assert exceptions == []
        assert len(failed) == 1
        manager_failure_logs = [
            record
            for record in caplog.records
            if record.name == "pynote.app"
            and "닫힌 요청 창의 가져오기 실패" in record.getMessage()
        ]
        if request_state == "live":
            assert len(critical_calls) == 1
            assert critical_calls[0][0] is requester
            assert critical_calls[0][0] is not survivor
            assert manager_failure_logs == []
        else:
            assert critical_calls == []
            assert len(manager_failure_logs) == 1
            assert requester_id in manager_failure_logs[0].getMessage()
    finally:
        _shutdown_manager(manager)


def test_shutdown_does_not_wait_for_or_commit_pending_import(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
    caplog: pytest.LogCaptureFixture,
) -> None:
    target = _document(repositories, "shutdown-pending-import", "종료 폐기 대상")
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    window = manager.create_window(initial_document_id=target.id)
    window.show()
    manual_pool = _ManualThreadPool()
    manager.import_controller._thread_pool = cast(QThreadPool, manual_pool)
    imported: list[object] = []
    failed: list[str] = []
    completion_routes: list[tuple[str, object]] = []
    published: list[str] = []
    critical_calls: list[tuple[object, ...]] = []
    real_completion_router = manager.import_controller._completion_router
    publish = manager.publish_document_change
    manager.import_controller.imported.connect(imported.append)
    manager.import_controller.failed.connect(failed.append)

    def observe_completion(request_window_id: str, value: object) -> None:
        completion_routes.append((request_window_id, value))
        real_completion_router(request_window_id, value)

    def observe_publish(document_id: str) -> None:
        published.append(document_id)
        publish(document_id)

    monkeypatch.setattr(
        manager.import_controller,
        "_completion_router",
        observe_completion,
    )
    monkeypatch.setattr(manager, "publish_document_change", observe_publish)
    monkeypatch.setattr(
        QMessageBox,
        "critical",
        lambda *args, **_kwargs: critical_calls.append(args),
    )
    path = tmp_path / "shutdown-pending-import.txt"
    path.write_text("종료 뒤에는 저장되면 안 되는 카드", encoding="utf-8")

    try:
        manager.import_controller.start_import(window.window_id, target.id, path)
        assert manual_pool.pending_count == 1
        caplog.clear()
        with caplog.at_level(logging.WARNING), qtbot.captureExceptions() as exceptions:
            manager.prepare_shutdown()
            # 미완료 worker가 그대로인데 반환했으므로 GUI 종료 승인을 보류하지 않았다.
            assert manual_pool.pending_count == 1
            assert manual_pool.wait_calls == []
            manual_pool.complete()
            QApplication.processEvents()

        assert exceptions == []
        assert repositories.list_cards(target.id) == ()
        assert published == []
        assert critical_calls == []
        # 완료 결과 0회는 manager router와 두 공개 결과 스트림을 각각 뜻한다.
        assert completion_routes == []
        assert imported == []
        assert failed == []
        discard_logs = [
            record
            for record in caplog.records
            if record.name == "pynote.ui.import_dialog"
            and "종료 중인 가져오기 준비 결과를 폐기" in record.getMessage()
        ]
        assert len(discard_logs) == 1
        assert window.window_id in discard_logs[0].getMessage()
    finally:
        _shutdown_manager(manager)


def test_start_import_after_shutdown_is_ignored_without_worker_or_signals(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
    caplog: pytest.LogCaptureFixture,
) -> None:
    target = _document(repositories, "post-shutdown-import", "종료 뒤 신규 요청")
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    window = manager.create_window(initial_document_id=target.id)
    window.show()
    manual_pool = _ManualThreadPool()
    manager.import_controller._thread_pool = cast(QThreadPool, manual_pool)
    imported: list[object] = []
    failed: list[str] = []
    critical_calls: list[tuple[object, ...]] = []
    manager.import_controller.imported.connect(imported.append)
    manager.import_controller.failed.connect(failed.append)
    monkeypatch.setattr(
        QMessageBox,
        "critical",
        lambda *args, **_kwargs: critical_calls.append(args),
    )
    path = tmp_path / "post-shutdown-import.txt"
    path.write_text("시작되면 안 되는 가져오기", encoding="utf-8")

    try:
        manager.import_controller.begin_shutdown()
        caplog.clear()
        with caplog.at_level(logging.WARNING), qtbot.captureExceptions() as exceptions:
            manager.import_controller.start_import(window.window_id, target.id, path)
            QApplication.processEvents()

        assert exceptions == []
        assert manual_pool.pending_count == 0
        assert imported == []
        assert failed == []
        assert critical_calls == []
        assert repositories.list_cards(target.id) == ()
        ignored_logs = [
            record
            for record in caplog.records
            if record.name == "pynote.ui.import_dialog"
            and "종료 중인 가져오기 요청을 시작하지 않습니다" in record.getMessage()
        ]
        assert len(ignored_logs) == 1
        assert window.window_id in ignored_logs[0].getMessage()
    finally:
        _shutdown_manager(manager)


def test_live_request_window_publishes_original_document_without_tracking_it(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    started = _document(repositories, "live-import-started", "원래 요청 문서")
    receiver_initial = _document(
        repositories,
        "live-import-receiver-initial",
        "수신 창 기존 문서",
    )
    source_active = _document(
        repositories,
        "live-import-source-active",
        "요청 창 이동 문서",
    )
    service = CardService(database, repositories)
    receiver_anchor = service.create_card(started.id, "수신 창 기준 카드")
    service.create_card(receiver_initial.id, "수신 창 초기 카드")
    source_anchor = service.create_card(source_active.id, "요청 창 기준 카드")
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    source = manager.create_window(initial_document_id=started.id)
    receiver = manager.create_window(initial_document_id=receiver_initial.id)
    source.show()
    receiver.show()
    manual_pool = _ManualThreadPool()
    manager.import_controller._thread_pool = cast(QThreadPool, manual_pool)
    imported: list[object] = []
    failed: list[str] = []
    published: list[str] = []
    manager.import_controller.imported.connect(imported.append)
    manager.import_controller.failed.connect(failed.append)
    publish = manager.publish_document_change

    def observe_publish(document_id: str) -> None:
        published.append(document_id)
        publish(document_id)

    monkeypatch.setattr(manager, "publish_document_change", observe_publish)
    path = tmp_path / "live-request-original-document.txt"
    path.write_text("원래 문서에 들어갈 카드", encoding="utf-8")

    try:
        manager.import_controller.start_import(source.window_id, started.id, path)
        assert manager.open_document(source, source_active.id)
        assert manager.open_document(receiver, started.id)
        source_page = source.page_for_document(source_active.id)
        receiver_page = receiver.page_for_document(started.id)
        assert source_page is not None
        assert receiver_page is not None
        assert source_page.open_card(source_anchor.id)
        assert receiver_page.open_card(receiver_anchor.id)
        source_refresh_calls: list[None] = []
        source_refresh = source_page.refresh

        def observe_source_refresh() -> None:
            source_refresh_calls.append(None)
            source_refresh()

        monkeypatch.setattr(source_page, "refresh", observe_source_refresh)
        source_model_before = tuple(
            source_page.stream.card_model.index(row, 0).data(CardRole.CARD_ID)
            for row in range(source_page.stream.card_model.rowCount())
        )
        source_selected_before = source_page.stream.currentIndex().data(
            CardRole.CARD_ID
        )
        source_editor_before = source_page.editor.card_id
        source_mode_before = source_page.mode_stack.currentWidget()
        source_status_before = source.statusBar().currentMessage()

        manual_pool.complete()
        QApplication.processEvents()

        assert failed == []
        assert len(imported) == 1
        created = cast(tuple[Card, ...], imported[0])
        assert published == [started.id]
        assert source_refresh_calls == []
        assert tuple(
            source_page.stream.card_model.index(row, 0).data(CardRole.CARD_ID)
            for row in range(source_page.stream.card_model.rowCount())
        ) == source_model_before
        assert (
            source_page.stream.currentIndex().data(CardRole.CARD_ID)
            == source_selected_before
        )
        assert source_page.editor.card_id == source_editor_before
        assert source_page.mode_stack.currentWidget() is source_mode_before
        assert source.statusBar().currentMessage() == source_status_before
        assert receiver_page.stream.card_model.index_for_card(
            created[-1].id
        ).isValid()
        assert (
            receiver_page.stream.currentIndex().data(CardRole.CARD_ID)
            != created[-1].id
        )
        assert receiver_page.editor.card_id == receiver_anchor.id
        assert receiver_page.editor.card_id != created[-1].id
    finally:
        _shutdown_manager(manager)


def test_reverse_order_import_completions_keep_requester_and_document_correlation(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    first_document = _document(repositories, "reverse-import-first", "첫 요청 문서")
    second_document = _document(repositories, "reverse-import-second", "둘째 요청 문서")
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    first = manager.create_window(initial_document_id=first_document.id)
    second = manager.create_window(initial_document_id=second_document.id)
    first.show()
    second.show()
    manual_pool = _ManualThreadPool()
    manager.import_controller._thread_pool = cast(QThreadPool, manual_pool)
    routed: list[tuple[str, str]] = []
    public_payloads: list[object] = []
    failures: list[str] = []
    published: list[str] = []
    completion_router = manager.import_controller._completion_router
    publish = manager.publish_document_change

    def observe_completion(request_window_id: str, value: object) -> None:
        cards = cast(tuple[Card, ...], value)
        routed.append((request_window_id, cards[-1].document_id))
        completion_router(request_window_id, value)

    def observe_publish(document_id: str) -> None:
        published.append(document_id)
        publish(document_id)

    monkeypatch.setattr(
        manager.import_controller,
        "_completion_router",
        observe_completion,
    )
    monkeypatch.setattr(manager, "publish_document_change", observe_publish)
    manager.import_controller.imported.connect(public_payloads.append)
    manager.import_controller.failed.connect(failures.append)
    first_path = tmp_path / "reverse-first.txt"
    second_path = tmp_path / "reverse-second.txt"
    first_path.write_text("첫 요청에서 만든 카드", encoding="utf-8")
    second_path.write_text("둘째 요청에서 만든 카드", encoding="utf-8")

    try:
        manager.import_controller.start_import(
            first.window_id,
            first_document.id,
            first_path,
        )
        manager.import_controller.start_import(
            second.window_id,
            second_document.id,
            second_path,
        )
        assert manual_pool.pending_count == 2

        manual_pool.complete(-1)
        manual_pool.complete()
        QApplication.processEvents()

        assert failures == []
        assert routed == [
            (second.window_id, second_document.id),
            (first.window_id, first_document.id),
        ]
        assert published == [second_document.id, first_document.id]
        assert len(public_payloads) == 2
        public_cards = [cast(tuple[Card, ...], value) for value in public_payloads]
        assert [cards[-1].document_id for cards in public_cards] == [
            second_document.id,
            first_document.id,
        ]
        # 공개 imported payload에는 요청 창 ID가 없고 router 계층에만 상관키가 있다.
        assert all(not hasattr(value, "request_window_id") for value in public_payloads)
        assert all(
            not hasattr(card, "request_window_id")
            for cards in public_cards
            for card in cards
        )
        first_page = first.page_for_document(first_document.id)
        second_page = second.page_for_document(second_document.id)
        assert first_page is not None
        assert second_page is not None
        assert (
            first_page.stream.currentIndex().data(CardRole.CARD_ID)
            == public_cards[1][-1].id
        )
        assert (
            second_page.stream.currentIndex().data(CardRole.CARD_ID)
            == public_cards[0][-1].id
        )
    finally:
        _shutdown_manager(manager)


@pytest.mark.parametrize(
    "last_window",
    [False, True],
    ids=["non-last-window", "last-window"],
)
def test_last_window_close_discards_pending_import_but_non_last_close_preserves_it(
    last_window: bool,
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
    caplog: pytest.LogCaptureFixture,
) -> None:
    target = _document(
        repositories,
        f"last-window-target-{last_window}",
        "마지막 창 비대칭 대상",
    )
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    source = manager.create_window(initial_document_id=target.id)
    if not last_window:
        survivor_document = _document(
            repositories,
            "last-window-survivor",
            "비마지막 경계 생존 문서",
        )
        manager.create_window(initial_document_id=survivor_document.id)
    for window in manager.windows:
        window.show()
    manual_pool = _ManualThreadPool()
    manager.import_controller._thread_pool = cast(QThreadPool, manual_pool)
    imported: list[object] = []
    failed: list[str] = []
    published: list[str] = []
    critical_calls: list[tuple[object, ...]] = []
    manager.import_controller.imported.connect(imported.append)
    manager.import_controller.failed.connect(failed.append)
    publish = manager.publish_document_change

    def observe_publish(document_id: str) -> None:
        published.append(document_id)
        publish(document_id)

    monkeypatch.setattr(manager, "publish_document_change", observe_publish)
    monkeypatch.setattr(
        QMessageBox,
        "critical",
        lambda *args, **_kwargs: critical_calls.append(args),
    )
    path = tmp_path / f"last-window-{last_window}.txt"
    path.write_text("창 수에 따라 결과가 갈릴 카드", encoding="utf-8")

    try:
        manager.import_controller.start_import(source.window_id, target.id, path)
        assert manual_pool.pending_count == 1
        caplog.clear()
        with caplog.at_level(logging.WARNING), qtbot.captureExceptions() as exceptions:
            assert source.close()
            assert source not in manager.windows
            manual_pool.complete()
            QApplication.processEvents()

        assert exceptions == []
        assert failed == []
        assert critical_calls == []
        imported_cards = tuple(
            card
            for card in repositories.list_cards(target.id)
            if card.source is CardSource.IMPORT
        )
        discard_logs = [
            record
            for record in caplog.records
            if record.name == "pynote.ui.import_dialog"
            and "종료 중인 가져오기 준비 결과를 폐기" in record.getMessage()
        ]
        if last_window:
            assert imported_cards == ()
            assert imported == []
            assert published == []
            assert len(discard_logs) == 1
        else:
            assert len(imported_cards) == 1
            assert len(imported) == 1
            assert published == [target.id]
            assert discard_logs == []
    finally:
        _shutdown_manager(manager)


@pytest.mark.parametrize(
    "disruption",
    ["close", "switch-document", "replace-page-same-document"],
)
def test_import_bus_disruption_keeps_publish_and_skips_stale_reveal(
    disruption: str,
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    target = _document(repositories, f"bus-disruption-target-{disruption}", "발행 대상")
    survivor_document = _document(
        repositories,
        f"bus-disruption-survivor-{disruption}",
        "발행 중 생존 문서",
    )
    replacement = _document(
        repositories,
        f"bus-disruption-replacement-{disruption}",
        "발행 중 전환 문서",
    )
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    requester = manager.create_window(initial_document_id=target.id)
    manager.create_window(initial_document_id=survivor_document.id)
    for window in manager.windows:
        window.show()
    original_page = requester.page_for_document(target.id)
    assert original_page is not None
    manual_pool = _ManualThreadPool()
    manager.import_controller._thread_pool = cast(QThreadPool, manual_pool)
    imported: list[object] = []
    failed: list[str] = []
    published: list[str] = []
    disruptions: list[str] = []
    reveal_calls: list[tuple[str, str]] = []
    publish = manager.publish_document_change
    reveal_created_card = DocumentPage.reveal_created_card
    manager.import_controller.imported.connect(imported.append)
    manager.import_controller.failed.connect(failed.append)

    def observe_publish(document_id: str) -> None:
        published.append(document_id)
        publish(document_id)

    def observe_reveal(page: DocumentPage, card_id: str) -> bool:
        reveal_calls.append((page.document_id, card_id))
        return reveal_created_card(page, card_id)

    def disrupt_during_publish(document_id: str) -> None:
        if document_id != target.id:
            return
        disruptions.append(document_id)
        if disruption == "close":
            assert requester.close()
        elif disruption == "switch-document":
            assert manager.open_document(requester, replacement.id)
        else:
            assert manager.open_document(requester, replacement.id)
            assert manager.open_document(requester, target.id)

    monkeypatch.setattr(manager, "publish_document_change", observe_publish)
    monkeypatch.setattr(DocumentPage, "reveal_created_card", observe_reveal)
    context.change_bus.document_changed.connect(disrupt_during_publish)
    path = tmp_path / f"bus-disruption-{disruption}.txt"
    path.write_text("발행은 끝나되 공개되면 안 되는 카드", encoding="utf-8")

    try:
        manager.import_controller.start_import(requester.window_id, target.id, path)
        manual_pool.complete()
        QApplication.processEvents()

        assert failed == []
        assert len(imported) == 1
        created = cast(tuple[Card, ...], imported[0])
        assert published == [target.id]
        assert disruptions == [target.id]
        assert reveal_calls == []
        assert tuple(
            card.id
            for card in repositories.list_cards(target.id)
            if card.source is CardSource.IMPORT
        ) == (created[-1].id,)
        if disruption == "close":
            assert requester not in manager.windows
        elif disruption == "switch-document":
            replacement_page = requester.page_for_document(replacement.id)
            assert requester in manager.windows
            assert replacement_page is not None
            assert replacement_page is not original_page
            assert requester.active_document_id == replacement.id
            assert not replacement_page.stream.card_model.index_for_card(
                created[-1].id
            ).isValid()
        else:
            replacement_target_page = requester.page_for_document(target.id)
            assert requester in manager.windows
            assert replacement_target_page is not None
            assert replacement_target_page is not original_page
            assert requester.active_document_id == target.id
            assert (
                replacement_target_page.stream.currentIndex().data(CardRole.CARD_ID)
                != created[-1].id
            )
    finally:
        context.change_bus.document_changed.disconnect(disrupt_during_publish)
        _shutdown_manager(manager)


def test_document_change_consumer_exception_is_logged_and_does_not_block_reveal(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
    caplog: pytest.LogCaptureFixture,
) -> None:
    failing_document = _document(repositories, "consumer-failing", "실패 소비자 문서")
    healthy_document = _document(repositories, "consumer-healthy", "후속 소비자 문서")
    target = _document(repositories, "consumer-target", "요청 창 문서")
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    failing = manager.create_window(initial_document_id=failing_document.id)
    healthy = manager.create_window(initial_document_id=healthy_document.id)
    requester = manager.create_window(initial_document_id=target.id)
    for window in manager.windows:
        window.show()
    requester_page = requester.page_for_document(target.id)
    assert requester_page is not None
    manual_pool = _ManualThreadPool()
    manager.import_controller._thread_pool = cast(QThreadPool, manual_pool)
    imported: list[object] = []
    failed: list[str] = []
    deliveries: list[str] = []
    published: list[str] = []
    healthy_apply = healthy.apply_document_change
    requester_apply = requester.apply_document_change
    publish = manager.publish_document_change
    manager.import_controller.imported.connect(imported.append)
    manager.import_controller.failed.connect(failed.append)

    def fail_delivery(_document_id: str) -> None:
        deliveries.append(failing.window_id)
        raise RuntimeError("주입된 문서 변경 소비자 예외")

    def observe_healthy(document_id: str) -> None:
        deliveries.append(healthy.window_id)
        healthy_apply(document_id)

    def observe_requester(document_id: str) -> None:
        deliveries.append(requester.window_id)
        requester_apply(document_id)

    def observe_publish(document_id: str) -> None:
        published.append(document_id)
        publish(document_id)

    monkeypatch.setattr(failing, "apply_document_change", fail_delivery)
    monkeypatch.setattr(healthy, "apply_document_change", observe_healthy)
    monkeypatch.setattr(requester, "apply_document_change", observe_requester)
    monkeypatch.setattr(manager, "publish_document_change", observe_publish)
    path = tmp_path / "consumer-exception-import.txt"
    path.write_text("소비자 하나가 실패해도 공개될 카드", encoding="utf-8")

    try:
        manager.import_controller.start_import(requester.window_id, target.id, path)
        caplog.clear()
        with caplog.at_level(logging.ERROR), qtbot.captureExceptions() as exceptions:
            manual_pool.complete()
            QApplication.processEvents()

        assert exceptions == []
        assert failed == []
        assert len(imported) == 1
        created = cast(tuple[Card, ...], imported[0])
        assert published == [target.id]
        assert deliveries == [failing.window_id, healthy.window_id, requester.window_id]
        consumer_logs = [
            record
            for record in caplog.records
            if record.name == "pynote.app"
            and "문서 변경 소비자 처리에 실패" in record.getMessage()
        ]
        assert len(consumer_logs) == 1
        assert failing.window_id in consumer_logs[0].getMessage()
        assert target.id in consumer_logs[0].getMessage()
        assert consumer_logs[0].exc_info is not None
        assert (
            requester_page.stream.currentIndex().data(CardRole.CARD_ID)
            == created[-1].id
        )
    finally:
        _shutdown_manager(manager)


def test_general_active_document_refresh_resets_selection_without_tracking_created_card(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    document = _document(repositories, "general-refresh", "일반 refresh")
    service = CardService(database, repositories)
    for number in range(24):
        service.create_card(document.id, f"일반 통지 카드 {number:02d}")
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    window = manager.create_window(initial_document_id=document.id)
    qtbot.addWidget(window)
    page = window.page_for_document(document.id)
    assert page is not None
    page.sort_combo.setCurrentIndex(page.sort_combo.findData("capture"))
    anchor = repositories.list_cards(document.id)[12]
    assert page.open_card(anchor.id)
    assert page.stream.verticalScrollBar().value() > 0
    before_selected = page.stream.currentIndex().data(CardRole.CARD_ID)
    before_scroll = page.stream.verticalScrollBar().value()
    before_editor_card = page.editor.card_id
    before_mode = page.mode_stack.currentWidget()
    created = service.create_card(document.id, "일반 경로로 추가된 최신 카드")
    assert before_selected == anchor.id
    assert before_editor_card == anchor.id

    manager.publish_document_change(document.id)

    # 일반 문서 refresh의 모델 reset은 기존 선택을 해제하지만 생성 카드를 추적하지 않는다.
    assert page.stream.currentIndex().data(CardRole.CARD_ID) is None
    assert page.stream.currentIndex().data(CardRole.CARD_ID) != created.id
    assert page.stream.verticalScrollBar().value() == before_scroll
    assert page.editor.card_id == before_editor_card
    assert page.editor.card_id != created.id
    assert page.mode_stack.currentWidget() is before_mode


def test_active_import_echo_guard_suppresses_self_echo_and_recovers(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    """활성 대상 가져오기 완료가 버스 refresh 한 번으로 끝나는지 센다.

    W1 이후 가져오기 완료는 `_publishing_page_content_change` 를 지나지 않으므로
    이 시험은 이름과 달리 guard 자체를 구동하지 않는다. guard 의 설정·복구는
    `test_page_content_change_echo_guard_suppresses_self_echo_and_recovers`
    (`tests/ui/test_main_window_integration.py`)가 별도로 센다. 이름은 이관
    추적을 위해 유지한다.
    """
    document = _document(repositories, "echo-guard", "에코 가드")
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    window = manager.create_window(initial_document_id=document.id)
    qtbot.addWidget(window)
    page = window.page_for_document(document.id)
    assert page is not None
    created = CardService(database, repositories).create_cards(
        document.id,
        "에코 가드 가져오기",
        source=CaptureOperationSource.IMPORT,
    )
    refresh_calls: list[None] = []
    refresh = page.refresh

    def observe_refresh() -> None:
        refresh_calls.append(None)
        refresh()

    monkeypatch.setattr(page, "refresh", observe_refresh)

    published: list[str] = []
    publish = manager.publish_document_change

    def observe_publish(document_id: str) -> None:
        published.append(document_id)
        publish(document_id)

    monkeypatch.setattr(manager, "publish_document_change", observe_publish)

    manager._route_import_completed(window.window_id, created)
    # 가져오기 완료는 버스에서 refresh 한 번을 만들고 reveal은 같은 모델을 쓴다.
    assert published == [document.id]
    assert len(refresh_calls) == 1

    manager.publish_document_change(document.id)
    # 이후 독립 버스 통지가 두 번째 refresh를 만든다.
    assert len(refresh_calls) == 2


@pytest.mark.parametrize("state", ["archive", "trash"])
def test_import_completion_for_ineligible_document_creates_card_without_reopening(
    state: str,
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    target = _document(repositories, f"{state}-import-target", "상태 변경 대상")
    fallback = _document(repositories, f"{state}-import-fallback", "대체 문서")
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    window = manager.create_window(initial_document_id=target.id)
    qtbot.addWidget(window)
    manual_pool = _ManualThreadPool()
    manager.import_controller._thread_pool = cast(QThreadPool, manual_pool)
    payloads: list[object] = []
    failures: list[str] = []
    manager.import_controller.imported.connect(payloads.append)
    manager.import_controller.failed.connect(failures.append)
    published: list[str] = []
    publish = manager.publish_document_change

    def observe_publish(document_id: str) -> None:
        published.append(document_id)
        publish(document_id)

    manager.publish_document_change = observe_publish
    path = tmp_path / f"{state}-import.txt"
    path.write_text("상태 변경 전에 시작한 가져오기", encoding="utf-8")
    manager.import_controller.start_import(window.window_id, target.id, path)
    command = (
        _navigator(window).archive_document
        if state == "archive"
        else _navigator(window).trash_document
    )
    command(target.id)
    assert target.id not in window.open_document_ids
    assert window.active_document_id == fallback.id
    published.clear()

    manual_pool.complete()
    qtbot.waitUntil(lambda: bool(payloads or failures))

    assert failures == []
    assert len(payloads) == 1
    assert len(repositories.list_cards(target.id)) == 1
    assert published == [target.id]
    assert target.id not in window.open_document_ids
    assert window.active_document_id == fallback.id


def test_import_completion_after_purge_fails_without_payload_or_bus_publish(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    target = _document(repositories, "purged-import-target", "purge 가져오기")
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    window = manager.create_window(initial_document_id=target.id)
    qtbot.addWidget(window)
    manual_pool = _ManualThreadPool()
    manager.import_controller._thread_pool = cast(QThreadPool, manual_pool)
    imported: list[object] = []
    failed: list[str] = []
    published: list[str] = []
    manager.import_controller.imported.connect(imported.append)
    manager.import_controller.failed.connect(failed.append)
    publish = manager.publish_document_change

    def observe_publish(document_id: str) -> None:
        published.append(document_id)
        publish(document_id)

    monkeypatch.setattr(manager, "publish_document_change", observe_publish)
    monkeypatch.setattr(QMessageBox, "critical", lambda *args, **kwargs: None)
    path = tmp_path / "purged-import.txt"
    path.write_text("purge 뒤 실패할 본문", encoding="utf-8")
    manager.import_controller.start_import(window.window_id, target.id, path)
    repositories.update_document(replace(target, updated_at_us=2, trashed_at_us=1))
    PurgeService(database, repositories, clock=lambda: 3).purge_document(
        target.id,
        retention_days=0,
    )

    manual_pool.complete()
    qtbot.waitUntil(lambda: bool(imported or failed))

    assert len(failed) == 1
    assert imported == []
    assert published == []
    assert repositories.get_document(target.id) is None
    assert repositories.list_cards(target.id) == ()


def test_document_creation_publish_path_updates_active_status_bar(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    initial = _document(repositories, "create-status-initial", "생성 전 문서")
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    window = manager.create_window(initial_document_id=initial.id)
    _register_windows(qtbot, manager)
    window.statusBar().showMessage("갱신 전 상태")

    created = _navigator(window).create_document("상태바 생성 문서")

    owner = next(
        candidate
        for candidate in manager.windows
        if candidate.active_document_id == created.id
    )
    qtbot.addWidget(owner)
    assert owner.statusBar().currentMessage() == (
        "0개 카드 · 0자 · 모든 변경 저장됨 · 로컬 DB"
    )


def test_document_list_state_publish_path_updates_active_status_bar(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    document = _document(repositories, "state-status", "상태 변경 전")
    CardService(database, repositories).create_card(document.id, "상태바 본문")
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    window = manager.create_window(initial_document_id=document.id)
    qtbot.addWidget(window)
    window.statusBar().showMessage("갱신 전 상태")

    _navigator(window).rename_document(document.id, "상태 변경 후")

    assert window.statusBar().currentMessage() == (
        f"1개 카드 · {len('상태바 본문')}자 · "
        "모든 변경 저장됨 · 로컬 DB"
    )


def test_page_content_publish_path_updates_active_status_bar(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    document = _document(repositories, "content-status", "페이지 변경")
    body = "페이지 변경 상태바"
    CardService(database, repositories).create_card(document.id, body)
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    window = manager.create_window(initial_document_id=document.id)
    qtbot.addWidget(window)
    page = window.page_for_document(document.id)
    assert page is not None
    window.statusBar().showMessage("갱신 전 상태")

    page.content_changed.emit()

    assert window.statusBar().currentMessage() == (
        f"1개 카드 · {len(body)}자 · 모든 변경 저장됨 · 로컬 DB"
    )


def test_purge_publish_path_updates_refilled_active_status_bar(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    target = _document(repositories, "purge-status-target", "purge 대상")
    fallback = _document(repositories, "purge-status-fallback", "purge 대체")
    fallback_body = "대체 문서 상태바 본문"
    CardService(database, repositories).create_card(fallback.id, fallback_body)
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    window = manager.create_window(initial_document_id=target.id)
    qtbot.addWidget(window)
    repositories.update_document(replace(target, updated_at_us=2, trashed_at_us=1))
    monkeypatch.setattr(
        app_module.QMessageBox,
        "warning",
        lambda *args, **kwargs: QMessageBox.StandardButton.Yes,
    )
    monkeypatch.setattr(
        QInputDialog,
        "getText",
        lambda *args, **kwargs: ("PURGE", True),
    )
    window.statusBar().showMessage("갱신 전 상태")

    window._purge_document(target.id)

    assert window.active_document_id == fallback.id
    assert window.statusBar().currentMessage() == (
        f"1개 카드 · {len(fallback_body)}자 · "
        "모든 변경 저장됨 · 로컬 DB"
    )


def test_empty_window_refill_publish_path_updates_active_status_bar(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    window = manager._create_window("empty-refill-status", create_row=True)
    qtbot.addWidget(window)
    window.show()
    assert window.active_document_id is None

    manager._prepare_window_for_input(window)

    assert window.active_document_id is not None
    assert window.statusBar().currentMessage() == (
        "0개 카드 · 0자 · 모든 변경 저장됨 · 로컬 DB"
    )


@pytest.mark.parametrize("operation", ["archive", "trash"])
def test_other_window_document_change_honors_dirty_preflight_and_broadcasts(
    operation: str,
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    document = _document(repositories, f"{operation}-document", "변경 문서")
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    owner = manager.create_window()
    requesting = manager.create_window()
    _register_windows(qtbot, manager)
    _dirty_card(manager, owner, database, repositories, document.id)
    page = owner.page_for_document(document.id)
    assert page is not None

    def fail_save(_session: object) -> object:
        raise RuntimeError("주입된 타 창 저장 실패")

    monkeypatch.setattr(page.editor._save_coordinator, "save", fail_save)
    monkeypatch.setattr(
        page.editor,
        "_ask_close_choice",
        lambda: CloseChoice.KEEP_EDITING,
    )
    command = (
        _navigator(requesting).archive_document
        if operation == "archive"
        else _navigator(requesting).trash_document
    )

    with pytest.raises(RuntimeError, match="문서 작업을 취소"):
        command(document.id)
    unchanged = repositories.get_document(document.id)
    assert unchanged is not None
    assert unchanged.archived_at_us is None
    assert unchanged.trashed_at_us is None
    assert "모든 변경 저장됨" not in owner.statusBar().currentMessage()
    assert "저장 실패" in owner.statusBar().currentMessage()

    monkeypatch.setattr(
        page.editor,
        "_ask_close_choice",
        lambda: CloseChoice.DISCARD,
    )
    command(document.id)
    changed = repositories.get_document(document.id)
    assert changed is not None
    expected_timestamp = (
        changed.archived_at_us if operation == "archive" else changed.trashed_at_us
    )
    assert expected_timestamp is not None
    assert document.id not in owner.open_document_ids
    assert _navigator(requesting).current_document_id() != document.id


@pytest.mark.parametrize("operation", ["archive", "trash"])
def test_other_window_document_change_auto_saves_owner_before_broadcast(
    operation: str,
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    document = _document(
        repositories,
        f"{operation}-auto-save-document",
        "자동저장 변경 문서",
    )
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    owner = manager.create_window()
    requesting = manager.create_window()
    _register_windows(qtbot, manager)
    _dirty_card(manager, owner, database, repositories, document.id)
    page = owner.page_for_document(document.id)
    assert page is not None
    session = page.editor.session
    assert session is not None
    card_id = session.card_id
    assert card_id is not None
    page.editor.setPlainText(f"타 창 {operation} 전에 자동 저장할 본문")
    revision_count = len(repositories.list_revisions(card_id))

    def fail_close_dialog() -> CloseChoice:
        raise AssertionError(f"정상 타 창 {operation}에서 닫기 선택 대화가 호출됨")

    monkeypatch.setattr(page.editor, "_ask_close_choice", fail_close_dialog)
    command = (
        _navigator(requesting).archive_document
        if operation == "archive"
        else _navigator(requesting).trash_document
    )

    changed = command(document.id)

    stored_card = repositories.get_card(card_id)
    assert stored_card is not None
    assert stored_card.body == f"타 창 {operation} 전에 자동 저장할 본문"
    assert len(repositories.list_revisions(card_id)) == revision_count + 1
    if operation == "archive":
        assert changed.archived_at_us is not None
        assert changed.trashed_at_us is None
    else:
        assert changed.trashed_at_us is not None
    assert document.id not in owner.open_document_ids
    assert _navigator(requesting).current_document_id() != document.id


def test_other_window_trash_refills_owner_with_most_recent_document(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    target = _document(repositories, "trash-target", "휴지통 대상")
    recent = _document(repositories, "recent-refill", "최근 문서")
    requester_document = _document(
        repositories,
        "trash-requester",
        "요청 창 문서",
    )
    repositories.update_document(
        replace(target, updated_at_us=30)
    )
    repositories.update_document(
        replace(recent, updated_at_us=20)
    )
    repositories.update_document(
        replace(requester_document, updated_at_us=10)
    )
    repositories.save_workspace_window("trash-owner", (target.id,), target.id)
    repositories.save_workspace_window(
        "trash-requester-window",
        (requester_document.id,),
        requester_document.id,
    )
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    manager.restore_windows()
    _register_windows(qtbot, manager)
    owner, requester = manager.windows

    _navigator(requester).trash_document(target.id)

    assert owner.open_document_ids == (recent.id,)
    assert requester.open_document_ids == (requester_document.id,)


@pytest.mark.parametrize("operation", ["archive", "trash"])
def test_broadcast_removal_saves_latest_document_ui_state(
    operation: str,
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    document = _document(
        repositories,
        f"{operation}-ui-state-document",
        "UI 상태 문서",
    )
    card = CardService(database, repositories, clock=lambda: 10).create_card(
        document.id,
        "커서 상태를 저장할 본문",
    )
    repositories.save_workspace_window("ui-owner", (document.id,), document.id)
    repositories.save_workspace_window("ui-requester", (), None)
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    manager.restore_windows()
    _register_windows(qtbot, manager)
    owner, requester = manager.windows
    page = owner.page_for_document(document.id)
    assert page is not None
    assert page.open_card(card.id)
    cursor = page.editor.textCursor()
    cursor.setPosition(5)
    page.editor.setTextCursor(cursor)

    command = (
        _navigator(requester).archive_document
        if operation == "archive"
        else _navigator(requester).trash_document
    )
    command(document.id)

    state = SqliteWorkspaceStateStore(
        database,
        owner.window_id,
    ).load_document_ui_state(document.id)
    assert state is not None
    assert state.selected_card_id == card.id
    assert state.editor_card_id == card.id
    assert state.editor_cursor_qchar == 5


def test_other_window_purge_honors_dirty_preflight_and_broadcasts(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    document = _document(repositories, "purge-document", "삭제 문서")
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    owner = manager.create_window()
    requesting = manager.create_window()
    _register_windows(qtbot, manager)
    _dirty_card(manager, owner, database, repositories, document.id)
    repositories.update_document(
        replace(document, updated_at_us=2, trashed_at_us=1)
    )
    page = owner.page_for_document(document.id)
    assert page is not None
    monkeypatch.setattr(
        app_module.QMessageBox,
        "warning",
        lambda *args, **kwargs: QMessageBox.StandardButton.Yes,
    )
    monkeypatch.setattr(
        QInputDialog,
        "getText",
        lambda *args, **kwargs: ("PURGE", True),
    )
    monkeypatch.setattr(page.editor, "save_current", lambda: False)
    monkeypatch.setattr(
        page.editor,
        "_ask_close_choice",
        lambda: CloseChoice.KEEP_EDITING,
    )

    requesting._purge_document(document.id)
    assert repositories.get_document(document.id) is not None

    monkeypatch.setattr(
        page.editor,
        "_ask_close_choice",
        lambda: CloseChoice.DISCARD,
    )
    requesting._purge_document(document.id)
    assert repositories.get_document(document.id) is None
    assert document.id not in owner.open_document_ids


def test_other_window_purge_auto_saves_owner_before_physical_delete(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    document = _document(
        repositories,
        "purge-auto-save-document",
        "자동저장 완전 삭제 문서",
    )
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(context)
    owner = manager.create_window()
    requesting = manager.create_window()
    _register_windows(qtbot, manager)
    _dirty_card(manager, owner, database, repositories, document.id)
    repositories.update_document(
        replace(document, updated_at_us=2, trashed_at_us=1)
    )
    page = owner.page_for_document(document.id)
    assert page is not None
    session = page.editor.session
    assert session is not None
    card_id = session.card_id
    assert card_id is not None
    page.editor.setPlainText("타 창 purge 전에 자동 저장할 본문")
    revision_count = len(repositories.list_revisions(card_id))

    def fail_close_dialog() -> CloseChoice:
        raise AssertionError("정상 타 창 purge에서 닫기 선택 대화가 호출됨")

    monkeypatch.setattr(page.editor, "_ask_close_choice", fail_close_dialog)
    monkeypatch.setattr(
        app_module.QMessageBox,
        "warning",
        lambda *args, **kwargs: QMessageBox.StandardButton.Yes,
    )
    monkeypatch.setattr(
        QInputDialog,
        "getText",
        lambda *args, **kwargs: ("PURGE", True),
    )
    critical_messages: list[tuple[str, str]] = []
    monkeypatch.setattr(
        app_module.QMessageBox,
        "critical",
        lambda _parent, title, message: critical_messages.append(
            (str(title), str(message))
        ),
    )
    real_preflight = requesting._destructive_preflight
    assert real_preflight is not None
    observed_preflight: list[tuple[bool, str, int]] = []

    def observe_preflight(document_id: str) -> bool:
        allowed = real_preflight(document_id)
        stored = repositories.get_card(card_id)
        assert stored is not None
        observed_preflight.append(
            (
                allowed,
                stored.body,
                len(repositories.list_revisions(card_id)),
            )
        )
        return allowed

    monkeypatch.setattr(
        requesting,
        "_destructive_preflight",
        observe_preflight,
    )

    requesting._purge_document(document.id)

    assert critical_messages == []
    assert observed_preflight == [
        (
            True,
            "타 창 purge 전에 자동 저장할 본문",
            revision_count + 1,
        )
    ]
    assert repositories.get_document(document.id) is None
    assert repositories.get_card(card_id) is None
    assert document.id not in owner.open_document_ids


@pytest.mark.parametrize(
    "choice",
    [DraftDisposition.RECOVER, DraftDisposition.DISCARD],
)
def test_global_recovery_batches_choice_before_restoring_multiple_windows(
    choice: DraftDisposition,
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    cards = []
    for number in (1, 2):
        document = _document(
            repositories,
            f"recovery-document-{number}",
            f"복구 문서 {number}",
        )
        card = CardService(
            database,
            repositories,
            clock=lambda: 10,
        ).create_card(document.id, f"확정 본문 {number}")
        cards.append(card)
        draft_text = f"미저장 본문 {number}"
        repositories.create_draft(
            Draft(
                id=f"recovery-draft-{number}",
                document_id=document.id,
                card_id=card.id,
                draft_kind=DraftKind.EDIT,
                base_revision_id=card.current_revision_id,
                draft_text=draft_text,
                draft_hash=text_hash(draft_text),
                cursor_position_qchar=3,
                updated_at_us=20,
            )
        )
        repositories.save_workspace_window(
            f"recovery-window-{number}",
            (document.id,),
            document.id,
        )
    context, _settings = _context(database, tmp_path)
    observations: list[tuple[str, int]] = []
    manager: WindowManager

    def choose(candidate: RecoveryCandidate) -> DraftDisposition:
        observations.append((candidate.draft.id, len(manager.windows)))
        return choice

    manager = WindowManager(context, recovery_choice_provider=choose)
    manager.restore_windows()
    _register_windows(qtbot, manager)
    close_choice_calls: list[bool] = []
    for window in manager.windows:
        page = window.active_document_page()
        if page is not None and page.editor.session is not None:
            monkeypatch.setattr(page.editor, "save_current", lambda: False)
            monkeypatch.setattr(
                page.editor,
                "_ask_close_choice",
                lambda: close_choice_calls.append(True) or CloseChoice.DISCARD,
            )

    assert len(manager.windows) == 2
    assert observations == [
        ("recovery-draft-1", 0),
        ("recovery-draft-2", 0),
    ]
    if choice is DraftDisposition.RECOVER:
        sessions = {
            page.editor.session.card_id: page.editor.toPlainText()
            for window in manager.windows
            if (page := window.active_document_page()) is not None
            and page.editor.session is not None
        }
        assert sessions == {
            cards[0].id: "미저장 본문 1",
            cards[1].id: "미저장 본문 2",
        }
    else:
        assert repositories.list_drafts(cards[0].document_id) == ()
        assert repositories.list_drafts(cards[1].document_id) == ()
    for window in manager.windows:
        assert window.can_leave_open_pages()
    assert close_choice_calls == (
        [True, True] if choice is DraftDisposition.RECOVER else []
    )


def test_global_later_suppression_expires_when_window_manager_restarts(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    document = _document(
        repositories,
        "later-restart-document",
        "재시작 LATER 문서",
    )
    card, draft = _recovery_card(
        database,
        repositories,
        document.id,
        90,
    )
    window_id = "later-restart-window"
    repositories.save_workspace_window(window_id, (document.id,), document.id)
    SqliteWorkspaceStateStore(database, window_id).save_document_ui_state(
        DocumentUiState(
            document_id=document.id,
            selected_card_id=card.id,
            list_scroll_position=0,
            sort_mode="position",
            editor_card_id=card.id,
            editor_base_revision_id=card.current_revision_id,
            editor_cursor_qchar=draft.cursor_position_qchar,
            editor_split_sizes=None,
            updated_at_us=100,
        )
    )
    app_driven_questions: list[str] = []

    def fail_app_driven_prompt(
        _editor: CardEditor,
        candidate: RecoveryCandidate,
    ) -> DraftDisposition:
        app_driven_questions.append(candidate.draft.id)
        raise AssertionError("LATER 카드가 workspace 복원에서 다시 질문됐습니다.")

    monkeypatch.setattr(
        CardEditor,
        "_ask_recovery_choice",
        fail_app_driven_prompt,
    )
    monkeypatch.setattr(QMessageBox, "critical", lambda *_args, **_kwargs: None)
    startup_questions: list[str] = []

    def choose(candidate: RecoveryCandidate) -> DraftDisposition:
        startup_questions.append(candidate.draft.id)
        return DraftDisposition.LATER

    settings_path = tmp_path / "multiwindow-restart.ini"
    first_settings = QSettings(
        str(settings_path),
        QSettings.Format.IniFormat,
    )
    first_settings.setValue("first_run/guide_shown", True)
    first_context = AppContext(database, first_settings)
    first = WindowManager(
        first_context,
        recovery_choice_provider=choose,
    )
    first.restore_windows()
    for window in first.windows:
        window.show()

    assert first._later_suppressed_card_ids == {card.id}
    assert first._recovered_candidates == ()
    first_page = first.windows[0].active_document_page()
    assert first_page is not None
    assert first_page.editor.session is None
    first.prepare_shutdown()
    for window in first.windows:
        assert window.close()
    assert first.windows == ()
    first_context.maintenance_timer.stop()
    first.deleteLater()
    first_context.deleteLater()
    first_settings.sync()
    del first, first_context, first_settings
    qtbot.wait(0)

    restarted_settings = QSettings(
        str(settings_path),
        QSettings.Format.IniFormat,
    )
    assert Path(restarted_settings.fileName()) == settings_path
    restarted = WindowManager(
        AppContext(database, restarted_settings),
        recovery_choice_provider=choose,
    )
    restarted.restore_windows()
    _register_windows(qtbot, restarted)

    assert startup_questions == [draft.id, draft.id]
    assert app_driven_questions == []
    assert restarted._later_suppressed_card_ids == {card.id}
    assert restarted._recovered_candidates == ()
    restarted_page = restarted.windows[0].active_document_page()
    assert restarted_page is not None
    assert restarted_page.editor.session is None
    assert repositories.get_draft(draft.id) == draft


@pytest.mark.parametrize("destination", ["existing_window", "new_window"])
def test_global_later_suppression_stays_released_after_document_moves(
    destination: str,
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    recovery_document = _document(
        repositories,
        f"later-move-{destination}",
        "LATER 이동 문서",
    )
    other_document = _document(
        repositories,
        f"later-move-other-{destination}",
        "기존 다른 창 문서",
    )
    replacement_document = _document(
        repositories,
        f"later-move-replacement-{destination}",
        "소유권 해제 문서",
    )
    card, draft = _recovery_card(
        database,
        repositories,
        recovery_document.id,
        91,
    )
    source_window_id = f"later-move-source-{destination}"
    other_window_id = f"later-move-other-window-{destination}"
    repositories.save_workspace_window(
        source_window_id,
        (recovery_document.id,),
        recovery_document.id,
    )
    repositories.save_workspace_window(
        other_window_id,
        (other_document.id,),
        other_document.id,
    )
    SqliteWorkspaceStateStore(
        database,
        source_window_id,
    ).save_document_ui_state(
        DocumentUiState(
            document_id=recovery_document.id,
            selected_card_id=card.id,
            list_scroll_position=0,
            sort_mode="position",
            editor_card_id=card.id,
            editor_base_revision_id=card.current_revision_id,
            editor_cursor_qchar=draft.cursor_position_qchar,
            editor_split_sizes=None,
            updated_at_us=100,
        )
    )
    user_questions: list[str] = []

    def discard_on_user_click(
        _editor: CardEditor,
        candidate: RecoveryCandidate,
    ) -> DraftDisposition:
        user_questions.append(candidate.draft.id)
        return DraftDisposition.DISCARD

    monkeypatch.setattr(
        CardEditor,
        "_ask_recovery_choice",
        discard_on_user_click,
    )
    monkeypatch.setattr(QMessageBox, "critical", lambda *_args, **_kwargs: None)
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(
        context,
        recovery_choice_provider=lambda _candidate: DraftDisposition.LATER,
    )
    manager.restore_windows()
    _register_windows(qtbot, manager)
    source, existing_destination = manager.windows
    source_page = source.active_document_page()
    assert source_page is not None
    assert source_page.editor.session is None
    target = source_page.stream.card_model.index_for_card(card.id)
    assert target.isValid()
    source_page.stream.scrollTo(target)
    qtbot.mouseClick(
        source_page.stream.viewport(),
        Qt.MouseButton.LeftButton,
        pos=source_page.stream.visualRect(target).center(),
    )

    assert user_questions == [draft.id]
    assert source_page.editor.session is not None
    assert source_page.editor.session.card_id == card.id
    assert repositories.get_draft(draft.id) is None
    assert manager.open_document(source, replacement_document.id)
    assert recovery_document.id not in source.open_document_ids

    if destination == "existing_window":
        assert manager.open_document(
            existing_destination,
            recovery_document.id,
        )
        destination_window = existing_destination
    else:
        previous_windows = manager.windows
        assert manager.open_document_in_new_window(
            existing_destination,
            recovery_document.id,
        )
        assert len(manager.windows) == len(previous_windows) + 1
        destination_window = manager.windows[-1]
        qtbot.addWidget(destination_window)

    restored_page = destination_window.active_document_page()
    assert restored_page is not None
    assert restored_page.editor.session is not None
    assert restored_page.editor.session.card_id == card.id
    assert card.id not in manager._later_suppressed_card_ids
    assert all(
        card.id not in window._startup_suppressed_card_ids
        for window in manager.windows
    )


def test_global_recovery_keeps_workspace_nonfirst_card_and_defers_the_rest(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    document = _document(
        repositories,
        "batched-owned-document",
        "복구 배치 소유 문서",
    )
    first_card, first_draft = _recovery_card(
        database,
        repositories,
        document.id,
        1,
    )
    second_card, second_draft = _recovery_card(
        database,
        repositories,
        document.id,
        2,
    )
    window_id = "batched-owned-window"
    repositories.save_workspace_window(window_id, (document.id,), document.id)
    SqliteWorkspaceStateStore(database, window_id).save_document_ui_state(
        DocumentUiState(
            document_id=document.id,
            selected_card_id=second_card.id,
            list_scroll_position=0,
            sort_mode="position",
            editor_card_id=second_card.id,
            editor_base_revision_id=second_card.current_revision_id,
            editor_cursor_qchar=second_draft.cursor_position_qchar,
            editor_split_sizes=None,
            updated_at_us=30,
        )
    )
    revision_counts = {
        card.id: len(repositories.list_revisions(card.id))
        for card in (first_card, second_card)
    }
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(
        context,
        recovery_choice_provider=lambda _candidate: DraftDisposition.RECOVER,
    )

    manager.restore_windows()
    _register_windows(qtbot, manager)

    assert len(manager.windows) == 1
    owner = manager.windows[0]
    page = owner.page_for_document(document.id)
    assert page is not None
    assert page.editor.session is not None
    assert page.editor.session.card_id == second_card.id
    assert page.editor.toPlainText() == second_draft.draft_text
    for card, draft in (
        (first_card, first_draft),
        (second_card, second_draft),
    ):
        assert len(repositories.list_revisions(card.id)) == revision_counts[card.id]
        stored = repositories.get_draft(draft.id)
        assert stored is not None
        assert stored.draft_text == draft.draft_text
        assert stored.cursor_position_qchar == draft.cursor_position_qchar
    assert page.editor.textCursor().position() == second_draft.cursor_position_qchar
    first_index = page.stream.card_model.index_for_card(first_card.id)
    assert first_index.isValid()
    assert first_index.data(CardRole.DIRTY_DRAFT) is True


def test_global_recovery_creates_distinct_owner_for_each_unclaimed_document(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    documents = (
        _document(repositories, "unclaimed-recovery-1", "미소유 복구 문서 1"),
        _document(repositories, "unclaimed-recovery-2", "미소유 복구 문서 2"),
    )
    pairs = tuple(
        _recovery_card(database, repositories, document.id, number)
        for number, document in enumerate(documents, start=11)
    )
    revision_counts = {
        card.id: len(repositories.list_revisions(card.id))
        for card, _draft in pairs
    }
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(
        context,
        recovery_choice_provider=lambda _candidate: DraftDisposition.RECOVER,
    )

    manager.restore_windows()
    _register_windows(qtbot, manager)

    assert len(manager.windows) == 2
    owners = {
        window.active_document_id: window
        for window in manager.windows
    }
    assert set(owners) == {document.id for document in documents}
    for card, draft in pairs:
        owner = owners[card.document_id]
        page = owner.page_for_document(card.document_id)
        assert page is not None
        assert page.editor.session is not None
        assert page.editor.session.card_id == card.id
        assert page.editor.toPlainText() == draft.draft_text
        assert page.editor.textCursor().position() == draft.cursor_position_qchar
        assert len(repositories.list_revisions(card.id)) == revision_counts[card.id]
        stored = repositories.get_draft(draft.id)
        assert stored is not None
        assert stored.draft_text == draft.draft_text
        index = page.stream.card_model.index_for_card(card.id)
        assert index.isValid()
        assert index.data(CardRole.DIRTY_DRAFT) is True


def test_global_recovery_new_owner_honors_nonfirst_workspace_candidate(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    retained = _document(repositories, "retained-owner", "기존 소유 문서")
    recovery_document = _document(
        repositories,
        "new-owner-recovery",
        "신규 소유 복구 문서",
    )
    first_card, first_draft = _recovery_card(
        database,
        repositories,
        recovery_document.id,
        21,
    )
    second_card, second_draft = _recovery_card(
        database,
        repositories,
        recovery_document.id,
        22,
    )
    retained_window_id = "retained-owner-window"
    repositories.save_workspace_window(
        retained_window_id,
        (retained.id,),
        retained.id,
    )
    ui_cursor = second_draft.cursor_position_qchar - 1
    SqliteWorkspaceStateStore(
        database,
        retained_window_id,
    ).save_document_ui_state(
        DocumentUiState(
            document_id=recovery_document.id,
            selected_card_id=second_card.id,
            list_scroll_position=0,
            sort_mode="position",
            editor_card_id=second_card.id,
            editor_base_revision_id=second_card.current_revision_id,
            editor_cursor_qchar=ui_cursor,
            editor_split_sizes=None,
            updated_at_us=50,
        )
    )
    revision_counts = {
        card.id: len(repositories.list_revisions(card.id))
        for card in (first_card, second_card)
    }
    choices: list[str] = []
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(
        context,
        recovery_choice_provider=lambda candidate: (
            choices.append(candidate.draft.id) or DraftDisposition.RECOVER
        ),
    )

    manager.restore_windows()
    _register_windows(qtbot, manager)

    assert choices == [first_draft.id, second_draft.id]
    owners = {
        window.active_document_id: window
        for window in manager.windows
    }
    assert set(owners) == {retained.id, recovery_document.id}
    owner = owners[recovery_document.id]
    page = owner.page_for_document(recovery_document.id)
    assert page is not None
    assert page.editor.session is not None
    assert page.editor.session.card_id == second_card.id
    assert page.editor.toPlainText() == second_draft.draft_text
    assert page.editor.textCursor().position() == ui_cursor
    for card, draft in (
        (first_card, first_draft),
        (second_card, second_draft),
    ):
        assert len(repositories.list_revisions(card.id)) == revision_counts[card.id]
        stored = repositories.get_draft(draft.id)
        assert stored is not None
        assert stored.draft_text == draft.draft_text
        assert stored.cursor_position_qchar == draft.cursor_position_qchar
    first_index = page.stream.card_model.index_for_card(first_card.id)
    second_index = page.stream.card_model.index_for_card(second_card.id)
    assert first_index.isValid()
    assert second_index.isValid()
    assert first_index.data(CardRole.DIRTY_DRAFT) is True
    assert second_index.data(CardRole.DIRTY_DRAFT) is True
    repeated_questions: list[str] = []
    monkeypatch.setattr(
        page.editor,
        "_ask_recovery_choice",
        lambda candidate: (
            repeated_questions.append(candidate.draft.id)
            or DraftDisposition.LATER
        ),
    )
    assert page.open_card(second_card.id)
    assert repeated_questions == []


@pytest.mark.parametrize("document_state", ["archived", "trashed"])
def test_global_recovery_skips_ineligible_document_without_creating_window(
    document_state: str,
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    retained = _document(
        repositories,
        f"ineligible-retained-{document_state}",
        "유지할 소유 문서",
    )
    ineligible = _document(
        repositories,
        f"ineligible-recovery-{document_state}",
        "열 수 없는 복구 문서",
    )
    card, draft = _recovery_card(
        database,
        repositories,
        ineligible.id,
        31 if document_state == "archived" else 32,
    )
    repositories.update_document(
        replace(
            ineligible,
            archived_at_us=100 if document_state == "archived" else None,
            trashed_at_us=100 if document_state == "trashed" else None,
        )
    )
    repositories.save_workspace_window(
        f"ineligible-window-{document_state}",
        (retained.id,),
        retained.id,
    )
    document_ids_before = {
        document.id for document in repositories.list_documents()
    }
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(
        context,
        recovery_choice_provider=lambda _candidate: DraftDisposition.RECOVER,
    )

    restored = manager.restore_windows()
    _register_windows(qtbot, manager)

    assert restored == manager.windows
    assert len(manager.windows) == 1
    assert manager.windows[0].active_document_id == retained.id
    assert {
        document.id for document in repositories.list_documents()
    } == document_ids_before
    assert repositories.get_draft(draft.id) == draft
    assert len(repositories.list_revisions(card.id)) == 1


@pytest.mark.parametrize("owner_present", [True, False])
@pytest.mark.parametrize("document_count", [1, 2])
def test_global_recovery_owner_document_matrix(
    owner_present: bool,
    document_count: int,
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    recovery_documents = tuple(
        _document(
            repositories,
            f"matrix-document-{owner_present}-{number}",
            f"복구 행렬 문서 {number}",
        )
        for number in range(document_count)
    )
    retained = None
    retained_window_id = "matrix-retained-window"
    if not owner_present:
        retained = _document(
            repositories,
            "matrix-retained-document",
            "미소유 조합 유지 문서",
        )
        repositories.save_workspace_window(
            retained_window_id,
            (retained.id,),
            retained.id,
        )
    pairs_by_document: dict[str, tuple[tuple[Card, Draft], ...]] = {}
    revision_counts: dict[str, int] = {}
    for document_number, document in enumerate(recovery_documents, start=1):
        pairs = tuple(
            _recovery_card(
                database,
                repositories,
                document.id,
                document_number * 100 + card_number,
            )
            for card_number in (1, 2)
        )
        pairs_by_document[document.id] = pairs
        window_id = (
            f"matrix-owner-window-{document_number}"
            if owner_present
            else retained_window_id
        )
        if owner_present:
            repositories.save_workspace_window(
                window_id,
                (document.id,),
                document.id,
            )
        displayed_card, displayed_draft = pairs[1]
        SqliteWorkspaceStateStore(database, window_id).save_document_ui_state(
            DocumentUiState(
                document_id=document.id,
                selected_card_id=displayed_card.id,
                list_scroll_position=0,
                sort_mode="position",
                editor_card_id=displayed_card.id,
                editor_base_revision_id=displayed_card.current_revision_id,
                editor_cursor_qchar=displayed_draft.cursor_position_qchar,
                editor_split_sizes=None,
                updated_at_us=50 + document_number,
            )
        )
        for card, _draft in pairs:
            revision_counts[card.id] = len(
                repositories.list_revisions(card.id)
            )
    context, _settings = _context(database, tmp_path)
    manager = WindowManager(
        context,
        recovery_choice_provider=lambda _candidate: DraftDisposition.RECOVER,
    )

    manager.restore_windows()
    _register_windows(qtbot, manager)

    expected_window_count = document_count + (0 if owner_present else 1)
    assert len(manager.windows) == expected_window_count
    owners = {
        window.active_document_id: window
        for window in manager.windows
    }
    expected_document_ids = {document.id for document in recovery_documents}
    if retained is not None:
        expected_document_ids.add(retained.id)
    assert set(owners) == expected_document_ids
    for document in recovery_documents:
        page = owners[document.id].page_for_document(document.id)
        assert page is not None
        first_pair, second_pair = pairs_by_document[document.id]
        displayed_card, displayed_draft = second_pair
        assert page.editor.session is not None
        assert page.editor.session.card_id == displayed_card.id
        assert page.editor.toPlainText() == displayed_draft.draft_text
        assert (
            page.editor.textCursor().position()
            == displayed_draft.cursor_position_qchar
        )
        for card, draft in (first_pair, second_pair):
            assert (
                len(repositories.list_revisions(card.id))
                == revision_counts[card.id]
            )
            stored = repositories.get_draft(draft.id)
            assert stored is not None
            assert stored.draft_text == draft.draft_text
            assert stored.cursor_position_qchar == draft.cursor_position_qchar
        deferred_index = page.stream.card_model.index_for_card(first_pair[0].id)
        assert deferred_index.isValid()
        assert deferred_index.data(CardRole.DIRTY_DRAFT) is True


@pytest.mark.parametrize("failure_mode", ["false", "exception", "ime"])
def test_recovery_protection_failure_skips_only_failed_session(
    failure_mode: str,
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    documents = (
        _document(repositories, "failure-document-1", "보호 실패 문서"),
        _document(repositories, "failure-document-2", "계속 복구 문서"),
    )
    workspace_cards: list[Card] = []
    recovery_pairs: list[tuple[Card, Draft]] = []
    revision_counts: dict[str, int] = {}
    for number, document in enumerate(documents, start=1):
        service = CardService(database, repositories, clock=lambda: 10)
        workspace_card = service.create_card(
            document.id,
            f"workspace 카드 {number}",
        )
        recovery_card, recovery_draft = _recovery_card(
            database,
            repositories,
            document.id,
            400 + number,
        )
        workspace_cards.append(workspace_card)
        recovery_pairs.append((recovery_card, recovery_draft))
        window_id = f"failure-window-{number}"
        repositories.save_workspace_window(
            window_id,
            (document.id,),
            document.id,
        )
        SqliteWorkspaceStateStore(database, window_id).save_document_ui_state(
            DocumentUiState(
                document_id=document.id,
                selected_card_id=workspace_card.id,
                list_scroll_position=0,
                sort_mode="position",
                editor_card_id=workspace_card.id,
                editor_base_revision_id=workspace_card.current_revision_id,
                editor_cursor_qchar=0,
                editor_split_sizes=None,
                updated_at_us=50 + number,
            )
        )
        revision_counts[workspace_card.id] = len(
            repositories.list_revisions(workspace_card.id)
        )
        revision_counts[recovery_card.id] = len(
            repositories.list_revisions(recovery_card.id)
        )
    failed_session_ids: list[str] = []
    release_calls: list[str] = []
    modal_calls: list[str] = []
    ime_sessions: list[tuple[DocumentPage, str]] = []
    original_resume = MainWindow.resume_recovery_card

    def resume_with_one_failure(window: MainWindow, card_id: str) -> bool:
        if window.active_document_id != documents[0].id:
            return original_resume(window, card_id)
        page = window.active_document_page()
        assert page is not None
        page.editor.setPlainText(f"{failure_mode} 보호 실패 직전 본문")
        session = page.editor.session
        assert session is not None
        failed_session_ids.append(session.draft_id)
        release_session = page.draft_coordinator.release_session

        def observe_release(draft_id: str) -> None:
            release_calls.append(draft_id)
            release_session(draft_id)

        restore_patch.setattr(
            page.draft_coordinator,
            "release_session",
            observe_release,
        )
        if failure_mode == "false":
            restore_patch.setattr(page.editor, "protect_now", lambda: False)
        elif failure_mode == "exception":
            def raise_protection_error(_draft_id: str) -> None:
                raise RuntimeError("restore 루프에 주입한 보호 실패")

            restore_patch.setattr(
                page.draft_coordinator,
                "protect_now",
                raise_protection_error,
            )
        else:
            ime_sessions.append((page, session.draft_id))
            page.draft_coordinator.set_ime_composing(
                session.draft_id,
                True,
            )
        return original_resume(window, card_id)

    context, _settings = _context(database, tmp_path)
    manager = WindowManager(
        context,
        recovery_choice_provider=lambda _candidate: DraftDisposition.RECOVER,
    )

    try:
        with monkeypatch.context() as restore_patch:
            restore_patch.setattr(
                MainWindow,
                "resume_recovery_card",
                resume_with_one_failure,
            )
            restore_patch.setattr(
                QMessageBox,
                "critical",
                lambda *_args, **_kwargs: modal_calls.append("critical"),
            )
            restore_patch.setattr(
                QMessageBox,
                "warning",
                lambda *_args, **_kwargs: modal_calls.append("warning"),
            )
            restored = manager.restore_windows()
        _register_windows(qtbot, manager)

        assert restored == manager.windows
        owners = {
            window.active_document_id: window
            for window in manager.windows
        }
        failed_page = owners[documents[0].id].active_document_page()
        successful_page = owners[documents[1].id].active_document_page()
        assert failed_page is not None
        assert successful_page is not None
        assert failed_page.editor.session is not None
        assert failed_page.editor.session.card_id == workspace_cards[0].id
        assert failed_session_ids == [failed_page.editor.session.draft_id]
        assert (
            failed_page.draft_coordinator.session(failed_session_ids[0])
            is failed_page.editor.session
        )
        assert release_calls == []
        assert successful_page.editor.session is not None
        assert successful_page.editor.session.card_id == recovery_pairs[1][0].id
        assert successful_page.editor.toPlainText() == recovery_pairs[1][1].draft_text
        for card in (*workspace_cards, *(pair[0] for pair in recovery_pairs)):
            assert len(repositories.list_revisions(card.id)) == revision_counts[card.id]
        assert modal_calls == []
    finally:
        for ime_page, draft_id in ime_sessions:
            ime_page.draft_coordinator.set_ime_composing(
                draft_id,
                False,
            )


def test_legacy_geometry_migrates_only_to_first_window_key(
    tmp_path: Path,
) -> None:
    settings = QSettings(
        str(tmp_path / "legacy-geometry.ini"),
        QSettings.Format.IniFormat,
    )
    legacy_geometry = QByteArray(b"legacy-geometry")
    settings.setValue(LEGACY_WINDOW_GEOMETRY_KEY, legacy_geometry)

    migrate_legacy_window_geometry(settings, "first-window")
    migrate_legacy_window_geometry(settings, "second-window")

    assert settings.value(window_geometry_key("first-window")) == legacy_geometry
    assert not settings.contains(window_geometry_key("second-window"))
    assert not settings.contains(LEGACY_WINDOW_GEOMETRY_KEY)
