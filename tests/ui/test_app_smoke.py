from __future__ import annotations

import json
import os
import socket
import sqlite3
import subprocess
import sys
import time
from pathlib import Path

import pytest
from PySide6.QtCore import QSettings
from PySide6.QtNetwork import QLocalSocket
from PySide6.QtWidgets import QApplication, QMessageBox
from pytest import MonkeyPatch
from pytestqt.qtbot import QtBot

from pynote.app import (
    AppContext,
    ApplicationQuitCoordinator,
    SingleInstanceGuard,
    WindowManager,
)
from pynote.application.card_service import CardService
from pynote.domain.events import EventType
from pynote.domain.models import Document
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories
from pynote.ui.editor.card_editor import CloseChoice

_SECONDARY_ACQUIRE_SCRIPT = """
import json
import sys
from pathlib import Path

from PySide6.QtCore import QCoreApplication

from pynote.app import SingleInstanceGuard

application = QCoreApplication([])
secondary = SingleInstanceGuard(Path(sys.argv[1]))
real_notify = secondary._notify_existing
attempts = 0


def transient_notify(timeout_ms: int) -> bool:
    global attempts
    attempts += 1
    if attempts <= 2:
        return False
    return real_notify(timeout_ms)


secondary._notify_existing = transient_notify
try:
    acquired = secondary.acquire(timeout_ms=100)
    print(json.dumps({"acquired": acquired, "attempts": attempts}), flush=True)
finally:
    secondary.close()
    application.processEvents()
"""


def test_root_launcher_starts_in_offscreen_smoke_mode() -> None:
    repository_root = Path(__file__).resolve().parents[2]
    environment = os.environ.copy()
    environment["QT_QPA_PLATFORM"] = "offscreen"

    result = subprocess.run(
        [sys.executable, str(repository_root / "main.py"), "--smoke"],
        cwd=repository_root,
        env=environment,
        capture_output=True,
        text=True,
        timeout=20,
        check=False,
    )

    assert result.returncode == 0, result.stderr


def test_transient_secondary_failures_keep_live_server_and_route_new_window(
    qtbot: QtBot,
    tmp_path: Path,
) -> None:
    repository_root = Path(__file__).resolve().parents[2]
    environment = os.environ.copy()
    primary = SingleInstanceGuard(tmp_path / "primary")
    portable = SingleInstanceGuard(tmp_path / "portable")
    database = Database(tmp_path / "primary" / "pynote.sqlite3")
    settings = QSettings(
        str(tmp_path / "single-instance.ini"),
        QSettings.Format.IniFormat,
    )
    settings.setValue("first_run/guide_shown", True)
    manager = WindowManager(AppContext(database, settings))
    manager.create_window()
    primary.new_window_requested.connect(manager.create_window)
    process: subprocess.Popen[str] | None = None
    try:
        assert primary.acquire(timeout_ms=100)
        process = subprocess.Popen(
            [
                sys.executable,
                "-c",
                _SECONDARY_ACQUIRE_SCRIPT,
                str(tmp_path / "primary"),
            ],
            cwd=repository_root,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        qtbot.waitUntil(lambda: process.poll() is not None, timeout=5_000)
        stdout, stderr = process.communicate()
        assert process.returncode == 0, stderr
        secondary_result = json.loads(stdout)

        assert not secondary_result["acquired"]
        qtbot.waitUntil(lambda: len(manager.windows) == 2, timeout=2_000)
        for window in manager.windows:
            window.show()
        assert secondary_result["attempts"] >= 3
        assert portable.socket_name != primary.socket_name
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait(timeout=2)
        primary.close()
        portable.close()
        manager.prepare_shutdown()
        for window in manager.windows:
            window.close()
        QApplication.processEvents()
        database.close()


@pytest.mark.skipif(os.name == "nt", reason="Unix 소켓 파일 mtime 계약")
def test_socket_file_is_removed_only_after_verifiable_stale_age(
    tmp_path: Path,
) -> None:
    guard = SingleInstanceGuard(tmp_path / "stale-socket")
    socket_path = Path(guard.socket_name)
    # AF_UNIX 는 Windows 타입스텁에 없다 — 이 시험 자체가 nt 에서 skip 된다.
    unix_family = socket.AF_UNIX  # pyright: ignore[reportAttributeAccessIssue]
    stale_endpoint = socket.socket(unix_family, socket.SOCK_STREAM)
    stale_endpoint.bind(str(socket_path))
    stale_endpoint.close()

    with pytest.raises(RuntimeError, match="stale"):
        guard.acquire(timeout_ms=10)
    assert socket_path.exists()

    old = time.time() - 10
    os.utime(socket_path, (old, old))
    try:
        assert guard.acquire(timeout_ms=10)
    finally:
        guard.close()


@pytest.mark.parametrize("cleanup", ["finish", "close"])
def test_local_clients_disconnect_signals_and_delete_later(
    cleanup: str,
    qtbot: QtBot,
    tmp_path: Path,
) -> None:
    guard = SingleInstanceGuard(tmp_path / f"client-{cleanup}")
    client = QLocalSocket(guard)
    client.readyRead.connect(lambda: None)
    client.disconnected.connect(lambda: None)
    guard._clients.add(client)
    guard._client_buffers[client] = bytearray()

    with qtbot.waitSignal(client.destroyed, timeout=2_000):
        if cleanup == "finish":
            guard._finish_client(client)
        else:
            guard.close()

    assert client not in guard._clients
    assert client not in guard._client_buffers


def test_application_quit_is_cancelled_before_about_to_quit_for_dirty_window(
    qapp: QApplication,
    qtbot: QtBot,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    database = Database(tmp_path / "quit.sqlite3")
    repositories = Repositories(database)
    document = Document(
        id="quit-document",
        title="종료 문서",
        created_at_us=1,
        updated_at_us=1,
    )
    repositories.create_document(document)
    card = CardService(database, repositories, clock=lambda: 10).create_card(
        document.id,
        "확정 본문",
    )
    settings = QSettings(
        str(tmp_path / "quit.ini"),
        QSettings.Format.IniFormat,
    )
    settings.setValue("first_run/guide_shown", True)
    manager = WindowManager(AppContext(database, settings))
    window = manager.create_window()
    window.show()
    assert manager.open_document(window, document.id)
    page = window.page_for_document(document.id)
    assert page is not None
    assert page.open_card(card.id)
    page.editor.setPlainText("앱 quit 직전 미저장 본문")
    monkeypatch.setattr(page.editor, "save_current", lambda: False)
    monkeypatch.setattr(
        page.editor,
        "_ask_close_choice",
        lambda: CloseChoice.KEEP_EDITING,
    )
    second_window = manager.create_window()
    second_window.show()
    coordinator = ApplicationQuitCoordinator(qapp, manager)
    about_to_quit: list[bool] = []
    protected_pages: list[str] = []
    for current_window in manager.windows:
        for document_id in current_window.open_document_ids:
            current_page = current_window.page_for_document(document_id)
            assert current_page is not None
            real_protect = current_page.protect_now
            monkeypatch.setattr(
                current_page,
                "protect_now",
                lambda current_id=document_id, protect=real_protect: (
                    protected_pages.append(current_id) or protect()
                ),
            )

    def record_about_to_quit() -> None:
        about_to_quit.append(True)

    qapp.aboutToQuit.connect(record_about_to_quit)

    try:
        qapp.quit()
        QApplication.processEvents()
        session = page.editor.session
        assert session is not None
        assert repositories.get_draft(session.draft_id) is not None
        assert about_to_quit == []
        assert window in manager.windows
        assert set(protected_pages) == {
            document_id
            for current_window in manager.windows
            for document_id in current_window.open_document_ids
        }
    finally:
        coordinator.uninstall()
        qapp.aboutToQuit.disconnect(record_about_to_quit)
        monkeypatch.setattr(
            page.editor,
            "_ask_close_choice",
            lambda: CloseChoice.DISCARD,
        )
        manager.prepare_shutdown()
        for current_window in manager.windows:
            current_window.close()
        QApplication.processEvents()
        database.close()


def test_application_quit_auto_saves_dirty_window_and_requests_qt_quit(
    qapp: QApplication,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    database = Database(tmp_path / "quit-auto-save.sqlite3")
    repositories = Repositories(database)
    document = Document(
        id="quit-auto-save-document",
        title="자동저장 종료 문서",
        created_at_us=1,
        updated_at_us=1,
    )
    repositories.create_document(document)
    card = CardService(database, repositories, clock=lambda: 10).create_card(
        document.id,
        "종료 전 확정 본문",
    )
    settings = QSettings(
        str(tmp_path / "quit-auto-save.ini"),
        QSettings.Format.IniFormat,
    )
    settings.setValue("first_run/guide_shown", True)
    manager = WindowManager(AppContext(database, settings))
    window = manager.create_window()
    window.show()
    assert manager.open_document(window, document.id)
    page = window.page_for_document(document.id)
    assert page is not None
    assert page.open_card(card.id)
    page.editor.setPlainText("앱 quit에서 자동 저장할 본문")
    revision_count = len(repositories.list_revisions(card.id))

    def fail_close_dialog() -> CloseChoice:
        raise AssertionError("정상 앱 종료에서 닫기 선택 대화가 호출됨")

    monkeypatch.setattr(page.editor, "_ask_close_choice", fail_close_dialog)
    coordinator = ApplicationQuitCoordinator(qapp, manager)
    qt_quit_calls: list[bool] = []
    monkeypatch.setattr(
        coordinator,
        "_qt_quit",
        lambda: qt_quit_calls.append(True),
    )

    try:
        qapp.quit()

        stored = repositories.get_card(card.id)
        assert stored is not None
        assert stored.body == "앱 quit에서 자동 저장할 본문"
        assert stored.deleted_at_us is None
        assert len(repositories.list_revisions(card.id)) == revision_count + 1
        assert not any(
            event.card_id == card.id and event.event_type is EventType.DELETE
            for event in repositories.list_events(document.id)
        )
        assert page.editor.session is not None
        assert not page.editor.session.dirty
        assert qt_quit_calls == [True]
    finally:
        coordinator.uninstall()
        manager.prepare_shutdown()
        for current_window in manager.windows:
            current_window.close()
        QApplication.processEvents()
        database.close()


def test_app_context_entrypoint_creates_pre_migration_backup(
    tmp_path: Path,
) -> None:
    database_path = tmp_path / "migration.sqlite3"
    backup_directory = tmp_path / "migration-backups"
    with sqlite3.connect(database_path) as connection:
        connection.execute(
            """
            CREATE TABLE schema_version(
                id INTEGER PRIMARY KEY,
                version INTEGER NOT NULL,
                applied_at_us INTEGER NOT NULL
            )
            """
        )
        connection.execute(
            "INSERT INTO schema_version(id, version, applied_at_us) VALUES (1, 0, 0)"
        )
    settings = QSettings(
        str(tmp_path / "migration.ini"),
        QSettings.Format.IniFormat,
    )
    settings.setValue("backup/location", str(backup_directory))

    context = AppContext.open(database_path, settings)
    try:
        backups = tuple(backup_directory.glob("*.pre-migration-*.sqlite3"))
        assert len(backups) == 1
    finally:
        context.database.close()


def test_periodic_backup_checks_integrity_first_and_reports_failure(
    qapp: QApplication,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
    caplog: pytest.LogCaptureFixture,
) -> None:
    database = Database(tmp_path / "maintenance.sqlite3")
    settings = QSettings(
        str(tmp_path / "maintenance.ini"),
        QSettings.Format.IniFormat,
    )
    context = AppContext(database, settings)
    order: list[str] = []
    notifications: list[str] = []
    user_reports: list[tuple[str, str]] = []
    context.maintenance_failed.connect(notifications.append)
    _manager = WindowManager(context)
    monkeypatch.setattr(
        QMessageBox,
        "critical",
        lambda _parent, title, message: user_reports.append((title, message)),
    )
    monkeypatch.setattr(
        context.quick_check,
        "run_if_due",
        lambda **_kwargs: order.append("quick-check") or True,
    )
    monkeypatch.setattr(
        context.backup_manager,
        "run_if_due",
        lambda **_kwargs: order.append("backup") or None,
    )

    context.start_automatic_maintenance()
    assert context.maintenance_timer.isActive()
    assert order == ["quick-check", "backup"]

    order.clear()

    def fail_quick_check(**_kwargs: object) -> bool:
        order.append("quick-check-failed")
        raise RuntimeError("무결성 실패 주입")

    monkeypatch.setattr(context.quick_check, "run_if_due", fail_quick_check)
    with caplog.at_level("ERROR"):
        context.maintenance_timer.timeout.emit()

    assert order == ["quick-check-failed"]
    assert notifications
    assert "무결성 실패 주입" in notifications[-1]
    assert user_reports == [("자동 백업 실패", notifications[-1])]
    assert "자동 백업 전 DB 무결성 검사" in caplog.text
    context.maintenance_timer.stop()
    database.close()
