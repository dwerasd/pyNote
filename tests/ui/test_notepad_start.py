from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QSettings, Qt
from PySide6.QtWidgets import QApplication
from pytestqt.qtbot import QtBot

from pynote.app import AppContext, WindowManager
from pynote.domain.models import Document
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories


def _context(database: Database, tmp_path: Path) -> AppContext:
    settings = QSettings(
        str(tmp_path / "notepad-start.ini"),
        QSettings.Format.IniFormat,
    )
    settings.setValue("first_run/guide_shown", True)
    return AppContext(database, settings)


def _show_windows(qtbot: QtBot, manager: WindowManager) -> None:
    for window in manager.windows:
        qtbot.addWidget(window)
        window.show()


def _document(
    repositories: Repositories,
    document_id: str,
    title: str,
    *,
    created_at_us: int,
    updated_at_us: int,
) -> Document:
    document = Document(
        id=document_id,
        title=title,
        created_at_us=created_at_us,
        updated_at_us=updated_at_us,
    )
    repositories.create_document(document)
    return document


def test_empty_database_focuses_empty_editor_and_first_paste_connects_card(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    manager = WindowManager(_context(database, tmp_path))

    windows = manager.restore_windows()
    _show_windows(qtbot, manager)

    assert len(windows) == 1
    window = windows[0]
    documents = repositories.list_documents()
    assert len(documents) == 1
    assert window.open_document_ids == (documents[0].id,)
    page = window.active_document_page()
    assert page is not None
    assert page.editor.card_id is None
    qtbot.waitUntil(lambda: QApplication.focusWidget() is page.editor)

    QApplication.clipboard().setText("실행 직후 붙여넣은 첫 카드")
    qtbot.keyClick(
        page.editor,
        Qt.Key.Key_V,
        modifier=Qt.KeyboardModifier.ControlModifier,
    )

    cards = repositories.list_cards(documents[0].id)
    assert len(cards) == 1
    assert cards[0].body == "실행 직후 붙여넣은 첫 카드"
    assert cards[0].source.value == "paste"
    assert page.editor.card_id == cards[0].id


def test_empty_restored_workspace_opens_most_recent_unowned_document(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    older = _document(
        repositories,
        "older-document",
        "이전 문서",
        created_at_us=1,
        updated_at_us=10,
    )
    recent = _document(
        repositories,
        "recent-document",
        "최근 문서",
        created_at_us=2,
        updated_at_us=20,
    )
    repositories.save_workspace_window("empty-window", (), None)
    manager = WindowManager(_context(database, tmp_path))

    manager.restore_windows()
    _show_windows(qtbot, manager)

    assert manager.windows[0].open_document_ids == (recent.id,)
    assert tuple(document.id for document in repositories.list_documents()) == (
        older.id,
        recent.id,
    )


def test_new_windows_use_unowned_recent_documents_then_create_one(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    older = _document(
        repositories,
        "older-document",
        "이전 문서",
        created_at_us=1,
        updated_at_us=10,
    )
    recent = _document(
        repositories,
        "recent-document",
        "최근 문서",
        created_at_us=2,
        updated_at_us=20,
    )
    manager = WindowManager(_context(database, tmp_path))

    first = manager.create_window()
    second = manager.create_window()
    third = manager.create_window()
    _show_windows(qtbot, manager)

    assert first.open_document_ids == (recent.id,)
    assert second.open_document_ids == (older.id,)
    assert len(set(first.open_document_ids + second.open_document_ids)) == 2
    assert len(repositories.list_documents()) == 3
    assert len(third.open_document_ids) == 1
    assert third.open_document_ids[0] not in {
        older.id,
        recent.id,
    }


def test_existing_restored_document_does_not_open_or_create_another_document(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    opened = _document(
        repositories,
        "opened-document",
        "열린 문서",
        created_at_us=1,
        updated_at_us=10,
    )
    _document(
        repositories,
        "newer-unowned-document",
        "더 최근인 미점유 문서",
        created_at_us=2,
        updated_at_us=20,
    )
    repositories.save_workspace_window(
        "restored-window",
        (opened.id,),
        opened.id,
    )
    document_count = len(repositories.list_documents())
    manager = WindowManager(_context(database, tmp_path))

    manager.restore_windows()
    _show_windows(qtbot, manager)

    assert manager.windows[0].open_document_ids == (opened.id,)
    assert len(repositories.list_documents()) == document_count
