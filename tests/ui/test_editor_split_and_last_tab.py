from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QSettings, Qt
from PySide6.QtWidgets import QApplication
from pytest import MonkeyPatch
from pytestqt.qtbot import QtBot

from pynote.app import (
    AppContext,
    SqliteWorkspaceStateStore,
    WindowManager,
    initialize_device_settings,
)
from pynote.application import document_service
from pynote.domain.models import CaptureOperationSource, Card, Document
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories
from pynote.ui.document_page import DocumentPage
from pynote.ui.editor.card_editor import (
    MIN_LIST_WIDTH,
    MIN_SLOT_WIDTH,
    CloseChoice,
)
from pynote.ui.main_window import MainWindow
from pynote.ui.panels.document_navigator import DocumentNavigator


def _window(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    *,
    ini_name: str = "split-last-tab.ini",
) -> tuple[MainWindow, QSettings, DocumentPage]:
    settings = QSettings(
        str(tmp_path / ini_name),
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
    window.resize(1_400, 800)
    window.show()
    document = document_service.create_document(repositories, "분할 검증 문서")
    assert window.open_document_local(document.id)
    page = window.active_document_page()
    assert page is not None
    return window, settings, page


def _first_card(page: DocumentPage) -> Card:
    cards = page.card_service.create_cards(
        page.document_id,
        "분할 비율 검증 본문",
        source=CaptureOperationSource.TYPING,
        split=False,
    )
    assert len(cards) == 1
    page.refresh()
    return cards[0]


def _navigator(window: MainWindow) -> DocumentNavigator:
    window._open_document_list()
    dialog = window._document_list_dialog
    assert dialog is not None
    return dialog.navigator


def test_horizontal_master_detail_order_and_editor_state_toggle_keep_split(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    window, _settings, page = _window(qtbot, database, repositories, tmp_path)
    card = _first_card(page)
    splitter = page.editor_workspace._splitter

    assert splitter.orientation() is Qt.Orientation.Horizontal
    assert splitter.widget(0) is page.list_pane
    assert splitter.widget(1) is page.editor.parentWidget()
    assert page.editor.card_id is None
    assert splitter.sizes()[1] > splitter.sizes()[0] * 1.5

    total = sum(splitter.sizes())
    splitter.setSizes([420, total - 420])
    splitter.splitterMoved.emit(420, 1)
    recorded = page.editor_workspace.editor_split_sizes()
    assert recorded is not None

    assert page.open_card(card.id)
    assert page.editor.card_id == card.id
    assert splitter.widget(1) is page.editor.parentWidget()
    assert page.editor_workspace.editor_split_sizes() == recorded

    assert page.editor.request_close()
    assert page.editor.card_id is None
    assert splitter.widget(1) is page.editor.parentWidget()
    assert page.editor_workspace.editor_split_sizes() == recorded


def test_editor_survives_history_round_trip(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    window, _settings, page = _window(qtbot, database, repositories, tmp_path)
    card = _first_card(page)
    assert page.open_card(card.id)

    page.show_history()
    page.show_cards()

    # 이력 화면 왕복은 연결을 놓지 않고 단일 편집면 점유를 유지한다.
    assert page.editor.card_id == card.id
    assert (
        page.editor_workspace._splitter.widget(1)
        is page.editor.parentWidget()
    )


def test_focus_editor_targets_single_surface_while_card_connected(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    window, _settings, page = _window(qtbot, database, repositories, tmp_path)
    card = _first_card(page)
    assert page.open_card(card.id)

    page.focus_editor()

    # 연결 상태에서도 같은 단일 편집면에 포커스를 둔다.
    # offscreen에서는 창이 활성화되지 않아 hasFocus()가 서지 않으므로 포커스
    # 대상 자체를 확인한다.
    assert page.editor.card_id == card.id
    assert page.focusWidget() is page.editor
    assert (
        page.editor_workspace._splitter.widget(1)
        is page.editor.parentWidget()
    )


def test_back_action_closes_editor_like_cancel(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    window, _settings, page = _window(qtbot, database, repositories, tmp_path)
    card = _first_card(page)
    assert page.open_card(card.id)

    # ESC·Alt+Left가 걸린 액션이다. 편집 중에는 취소 버튼과 같아야 한다.
    window.back_action.trigger()

    qtbot.waitUntil(lambda: QApplication.focusWidget() is page.editor)
    assert page.editor.card_id is None
    assert (
        page.editor_workspace._splitter.widget(1)
        is page.editor.parentWidget()
    )

    # dirty면 이탈 게이트를 거치고, 계속 편집을 고르면 세션이 남는다.
    assert page.open_card(card.id)
    page.editor.setPlainText("저장하지 않은 편집")
    monkeypatch.setattr(page.editor, "save_current", lambda: False)
    monkeypatch.setattr(
        page.editor,
        "_ask_close_choice",
        lambda: CloseChoice.KEEP_EDITING,
    )
    window.back_action.trigger()
    qtbot.waitUntil(lambda: QApplication.focusWidget() is page.editor)
    assert page.editor.card_id == card.id

    monkeypatch.setattr(
        page.editor,
        "_ask_close_choice",
        lambda: CloseChoice.DISCARD,
    )
    window.back_action.trigger()
    qtbot.waitUntil(lambda: QApplication.focusWidget() is page.editor)
    assert page.editor.card_id is None


def test_back_action_auto_saves_dirty_editor_and_adds_revision(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    window, _settings, page = _window(qtbot, database, repositories, tmp_path)
    card = _first_card(page)
    assert page.open_card(card.id)
    page.editor.setPlainText("뒤로가기 전에 자동 저장할 본문")
    revision_count = len(repositories.list_revisions(card.id))

    window.back_action.trigger()

    stored = repositories.get_card(card.id)
    assert stored is not None
    assert stored.body == "뒤로가기 전에 자동 저장할 본문"
    assert len(repositories.list_revisions(card.id)) == revision_count + 1
    qtbot.waitUntil(lambda: QApplication.focusWidget() is page.editor)
    assert page.editor.card_id is None


def test_back_action_without_session_focuses_card_list(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    window, _settings, page = _window(qtbot, database, repositories, tmp_path)
    _first_card(page)
    assert page.editor.card_id is None

    window.back_action.trigger()

    # 빈 편집면에서 카드 목록 액션은 목록 포커스로 전환한다.
    assert page.focusWidget() is page.stream
    assert page.editor.card_id is None
    assert (
        page.editor_workspace._splitter.widget(1)
        is page.editor.parentWidget()
    )


def test_splitter_rejects_collapse_and_records_clamped_widths(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    window, _settings, page = _window(qtbot, database, repositories, tmp_path)
    splitter = page.editor_workspace._splitter
    qtbot.waitUntil(lambda: splitter.sizes()[0] > 0)

    assert not splitter.childrenCollapsible()
    total = sum(splitter.sizes())
    splitter.setSizes([0, total])
    splitter.splitterMoved.emit(0, 1)
    recorded = page.editor_workspace.editor_split_sizes()
    assert recorded is not None
    assert recorded[0] >= MIN_LIST_WIDTH
    assert recorded[1] >= MIN_SLOT_WIDTH
    assert all(size > 0 for size in splitter.sizes())

    # 접힌 값이 영속되면 다음 실행에서 목록이 사라진다 — 실제 pane 폭까지 본다.
    page.editor_workspace.set_editor_split_sizes(recorded)
    qtbot.waitUntil(lambda: sum(splitter.sizes()) > 0)
    assert splitter.sizes()[0] >= MIN_LIST_WIDTH
    assert splitter.sizes()[1] >= MIN_SLOT_WIDTH
    assert page.list_pane.minimumSizeHint().width() <= MIN_LIST_WIDTH


def test_editor_split_sizes_persist_across_document_switch_and_restart(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    window, settings, page = _window(qtbot, database, repositories, tmp_path)
    assert page.editor.card_id is None
    splitter = page.editor_workspace._splitter
    qtbot.waitUntil(lambda: splitter.sizes()[0] > 0)

    total = sum(splitter.sizes())
    splitter.setSizes([420, total - 420])
    splitter.splitterMoved.emit(420, 1)
    recorded = page.editor_workspace.editor_split_sizes()
    assert recorded is not None

    document_id = page.document_id
    second = document_service.create_document(repositories, "분할 전환 문서")
    assert window.open_document_local(second.id)
    assert window.open_document_local(document_id)
    switched_page = window.active_document_page()
    assert switched_page is not None
    assert switched_page.editor.card_id is None
    assert switched_page.editor_workspace.editor_split_sizes() == recorded

    window.persist_open_page_ui_states()
    state = SqliteWorkspaceStateStore(database).load_document_ui_state(
        document_id
    )
    assert state is not None
    assert state.editor_split_sizes == recorded

    assert window.close()
    restored_window = MainWindow(
        repositories,
        SqliteWorkspaceStateStore(database),
        settings=settings,
    )
    qtbot.addWidget(restored_window)
    restored_window.restore_workspace()
    restored_page = restored_window.page_for_document(document_id)
    assert restored_page is not None
    assert restored_page.editor.card_id is None
    assert restored_page.editor_workspace.editor_split_sizes() == recorded
    restored_window.resize(1_400, 800)
    restored_window.show()
    restored_splitter = restored_page.editor_workspace._splitter
    qtbot.waitUntil(lambda: sum(restored_splitter.sizes()) > 0)
    # 캐시만이 아니라 화면에 실제로 그 비율이 적용돼야 복원이다.
    assert restored_splitter.sizes() == list(recorded)


def test_repeated_split_apply_and_record_round_trips(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    _window_widget, _settings, page = _window(
        qtbot,
        database,
        repositories,
        tmp_path,
    )
    workspace = page.editor_workspace
    splitter = workspace._splitter
    qtbot.waitUntil(lambda: splitter.sizes()[0] > 0)

    total = sum(splitter.sizes())
    splitter.setSizes([420, total - 420])
    splitter.splitterMoved.emit(420, 1)
    recorded = workspace.editor_split_sizes()
    assert recorded is not None

    # 적용 → 기록 왕복이 값을 바꾸면 문서를 오갈 때마다 비율이 이동한다.
    for _round in range(5):
        workspace.set_editor_split_sizes(recorded)
        splitter.splitterMoved.emit(splitter.sizes()[0], 1)
        assert splitter.sizes() == list(recorded)
        assert workspace.editor_split_sizes() == recorded
        assert splitter.sizes()[1] >= MIN_SLOT_WIDTH


def test_user_close_of_document_closes_window(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    window, _settings, _page = _window(qtbot, database, repositories, tmp_path)
    assert window.isVisible()
    assert len(window.open_document_ids) == 1

    assert window.close()
    assert not window.isVisible()


def test_switching_document_replaces_page_and_keeps_window_open(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    window, _settings, page = _window(qtbot, database, repositories, tmp_path)
    second = document_service.create_document(repositories, "둘째 문서")

    assert window.open_document_local(second.id)
    assert window.isVisible()
    assert window.open_document_ids == (second.id,)
    assert page.document_id != second.id


def test_system_removal_of_document_refills_with_recent_document(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    window, _settings, page = _window(qtbot, database, repositories, tmp_path)
    spare = Document(
        id="spare-document",
        title="여분 문서",
        created_at_us=1,
        updated_at_us=1,
    )
    repositories.create_document(spare)

    _navigator(window).trash_document(page.document_id)

    assert window.isVisible()
    assert window.open_document_ids == (spare.id,)


def test_system_removal_creates_document_when_none_available(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    window, _settings, page = _window(qtbot, database, repositories, tmp_path)
    before_ids = {document.id for document in repositories.list_documents()}

    _navigator(window).trash_document(page.document_id)

    assert window.isVisible()
    assert len(window.open_document_ids) == 1
    assert window.open_document_ids[0] not in before_ids


def test_manager_refill_does_not_steal_document_owned_by_other_window(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    first = Document(id="doc-a", title="가 문서", created_at_us=1, updated_at_us=1)
    second = Document(id="doc-b", title="나 문서", created_at_us=2, updated_at_us=2)
    repositories.create_document(first)
    repositories.create_document(second)
    repositories.save_workspace_window("window-a", (first.id,), first.id)
    repositories.save_workspace_window("window-b", (second.id,), second.id)
    settings = QSettings(
        str(tmp_path / "manager-refill.ini"),
        QSettings.Format.IniFormat,
    )
    settings.setValue("first_run/guide_shown", True)
    manager = WindowManager(AppContext(database, settings))
    manager.restore_windows()
    for window in manager.windows:
        qtbot.addWidget(window)
        window.show()
    windows_by_document = {
        window.open_document_ids[0]: window for window in manager.windows
    }
    window_a = windows_by_document[first.id]
    window_b = windows_by_document[second.id]

    _navigator(window_b).trash_document(first.id)

    assert len(window_a.open_document_ids) == 1
    refilled_id = window_a.open_document_ids[0]
    assert refilled_id not in {first.id, second.id}
    assert window_b.open_document_ids == (second.id,)
