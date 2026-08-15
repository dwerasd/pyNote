from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QSettings
from pytestqt.qtbot import QtBot

from pynote.app import (
    DEVICE_SETTING_DEFAULTS,
    SqliteWorkspaceStateStore,
    initialize_device_settings,
)
from pynote.domain.events import EventSource
from pynote.domain.models import (
    CaptureOperationSource,
    CardSource,
    NewCaptureOperation,
    NewCard,
    RevisionSource,
    SplitPolicy,
)
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories
from pynote.ui.main_window import MainWindow
from pynote.ui.panels.document_navigator import DocumentNavigator, DocumentView


def _navigator(window: MainWindow) -> DocumentNavigator:
    window._open_document_list()
    dialog = window._document_list_dialog
    assert dialog is not None
    return dialog.navigator


def _create_card(
    repositories: Repositories,
    *,
    document_id: str,
    body: str,
) -> None:
    repositories.create_cards(
        NewCaptureOperation(
            id="operation-1",
            document_id=document_id,
            source=CaptureOperationSource.TYPING,
            split_policy=SplitPolicy.KEEP,
            original_text=None,
            created_at_us=20_000,
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
                created_at_us=20_000,
            )
        ],
    )


def test_workspace_document_and_ui_state_restore_after_restart(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    state_store = SqliteWorkspaceStateStore(database)
    first_window = MainWindow(repositories, state_store)
    qtbot.addWidget(first_window)
    first_window.show()

    first = _navigator(first_window).create_document("첫 문서")
    page = first_window.active_document_page()
    assert page is not None
    page.sort_combo.setCurrentIndex(page.sort_combo.findData("capture"))
    first_window.save_document_ui_state(
        first.id,
        list_scroll_position=137,
        sort_mode="capture",
        editor_cursor_qchar=9,
    )

    assert not hasattr(first_window, "document_tabs")
    assert not hasattr(first_window, "document_navigator")
    assert first_window.windowTitle() == "첫 문서 — pyNote"
    assert first_window.open_document_ids == (first.id,)
    assert first_window.active_document_id == first.id
    assert first_window.close()

    restarted_window = MainWindow(repositories, state_store)
    qtbot.addWidget(restarted_window)
    restarted_window.show()

    assert restarted_window.open_document_ids == (first.id,)
    assert restarted_window.active_document_id == first.id
    assert restarted_window.windowTitle() == "첫 문서 — pyNote"
    restored_ui_state = restarted_window.document_ui_state(first.id)
    assert restored_ui_state is not None
    assert restored_ui_state.list_scroll_position == 137
    assert restored_ui_state.sort_mode == "capture"
    assert restored_ui_state.editor_cursor_qchar == 9


def test_new_document_defaults_to_recency_and_restores_selected_sort_mode(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    state_store = SqliteWorkspaceStateStore(database)
    first_window = MainWindow(repositories, state_store)
    qtbot.addWidget(first_window)
    document = _navigator(first_window).create_document("정렬 복원")
    page = first_window.active_document_page()
    assert page is not None
    assert page.sort_combo.currentData() == "recency"
    assert page.stream.card_model.sort_mode == "recency"

    page.sort_combo.setCurrentIndex(page.sort_combo.findData("position"))
    first_window.persist_open_page_ui_states()
    stored = state_store.load_document_ui_state(document.id)
    assert stored is not None
    assert stored.sort_mode == "position"

    restarted = MainWindow(repositories, state_store)
    qtbot.addWidget(restarted)
    restarted_page = restarted.active_document_page()
    assert restarted_page is not None
    assert restarted_page.stream.card_model.sort_mode == "position"

    restarted_page.sort_combo.setCurrentIndex(
        restarted_page.sort_combo.findData("recency")
    )
    restarted.persist_open_page_ui_states()
    restored = state_store.load_document_ui_state(document.id)
    assert restored is not None
    assert restored.sort_mode == "recency"

    recency_restarted = MainWindow(repositories, state_store)
    qtbot.addWidget(recency_restarted)
    recency_page = recency_restarted.active_document_page()
    assert recency_page is not None
    assert recency_page.stream.card_model.sort_mode == "recency"


def test_trashed_document_is_hidden_and_can_be_restored(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    window = MainWindow(repositories, SqliteWorkspaceStateStore(database))
    qtbot.addWidget(window)
    navigator = _navigator(window)
    document = navigator.create_document("복구할 문서")

    navigator.trash_document(document.id)

    stored = repositories.get_document(document.id)
    assert stored is not None
    assert stored.trashed_at_us is not None
    assert document.id not in navigator.visible_document_ids()
    assert document.id not in window.open_document_ids
    assert len(window.open_document_ids) == 1
    refill_id = window.open_document_ids[0]
    assert refill_id != document.id

    navigator.set_view(DocumentView.TRASHED)
    assert navigator.visible_document_ids() == (document.id,)
    navigator.restore_document(document.id)
    navigator.set_view(DocumentView.ACTIVE)

    restored = repositories.get_document(document.id)
    assert restored is not None
    assert restored.trashed_at_us is None
    assert set(navigator.visible_document_ids()) == {document.id, refill_id}


def test_archive_rename_title_filter_and_metadata(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    window = MainWindow(repositories, SqliteWorkspaceStateStore(database))
    qtbot.addWidget(window)
    navigator = _navigator(window)
    document = navigator.create_document("자료 조사")
    navigator.create_document("아이디어")
    body = "한글과 emoji 🧭"
    _create_card(repositories, document_id=document.id, body=body)
    navigator.rename_document(document.id, "조사 자료")

    navigator.search_edit.setText("조사")
    assert navigator.visible_document_ids() == (document.id,)
    summary = navigator.summary(document.id)
    assert summary is not None
    assert summary.card_count == 1
    assert summary.character_count == len(body)
    assert "1개 카드" in navigator.document_list.item(0).text()
    assert f"{len(body)}자" in navigator.document_list.item(0).text()

    navigator.search_edit.clear()
    assert navigator.visible_document_ids()[0] == document.id
    navigator.archive_document(document.id)
    assert document.id not in navigator.visible_document_ids()
    navigator.set_view(DocumentView.ARCHIVED)
    assert navigator.visible_document_ids() == (document.id,)
    navigator.unarchive_document(document.id)
    navigator.set_view(DocumentView.ACTIVE)
    assert document.id in navigator.visible_document_ids()


def test_device_settings_skeleton_excludes_p1_theme(tmp_path: Path) -> None:
    settings = QSettings(str(tmp_path / "settings.ini"), QSettings.Format.IniFormat)

    initialize_device_settings(settings)

    for key in DEVICE_SETTING_DEFAULTS:
        assert settings.contains(key)
    assert settings.value("display/time_format") == "yyyy-MM-dd HH:mm"
    assert not settings.contains("display/theme")
