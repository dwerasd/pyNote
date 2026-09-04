from __future__ import annotations

import base64
import inspect
import json
import os
import subprocess
import sys
from collections.abc import Iterator
from pathlib import Path

import pytest
from PySide6.QtCore import QSettings
from PySide6.QtWidgets import QApplication, QMessageBox, QWidget
from pytest import MonkeyPatch
from pytestqt.qtbot import QtBot

from pynote import app as app_module
from pynote.app import (
    AppContext,
    SingleInstanceGuard,
    WindowManager,
    _parse_arguments,
)
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories
from pynote.ui.document_page import DocumentPage
from pynote.ui.editor.card_editor import CardEditor, CloseChoice
from pynote.ui.main_window import MainWindow

_SECONDARY_OPEN_FILE_SCRIPT = """
import json
import sys
from pathlib import Path

from PySide6.QtCore import QCoreApplication

from pynote.app import SingleInstanceGuard

application = QCoreApplication([])
secondary = SingleInstanceGuard(
    Path(sys.argv[1]),
    paths=tuple(Path(argument) for argument in sys.argv[2:]),
)
try:
    acquired = secondary.acquire(timeout_ms=2000)
    print(json.dumps({"acquired": acquired}), flush=True)
finally:
    secondary.close()
    application.processEvents()
"""


@pytest.fixture(autouse=True)
def _non_blocking_close_dialog(monkeypatch: MonkeyPatch) -> None:
    """teardown 의 창 닫기가 모달을 띄우면 시험 실행이 그 자리에서 멈춘다."""
    monkeypatch.setattr(
        CardEditor,
        "_ask_close_choice",
        lambda _self: CloseChoice.DISCARD,
    )


@pytest.fixture
def manager(
    database: Database,
    tmp_path: Path,
) -> Iterator[WindowManager]:
    settings = QSettings(
        str(tmp_path / "launch.ini"),
        QSettings.Format.IniFormat,
    )
    settings.setValue("first_run/guide_shown", True)
    instance = WindowManager(AppContext(database, settings))
    yield instance
    instance.prepare_shutdown()
    for window in instance.windows:
        window.close()
    QApplication.processEvents()


def _activation_log(monkeypatch: MonkeyPatch) -> list[MainWindow]:
    """활성화 대상 창을 순서대로 기록한다 — `_activate_window` 와 같은 seam."""
    activated: list[MainWindow] = []
    monkeypatch.setattr(
        app_module,
        "_activate_window",
        lambda window: activated.append(window),
    )
    return activated


def _dialog_log(monkeypatch: MonkeyPatch) -> list[tuple[str, str]]:
    """오류 대화상자 대신 제목·본문을 기록한다.

    `critical` 까지 잡는 이유는 창을 먼저 만드는 변이가 페이지의 오류 모달을
    띄워 시험 실행을 멈추게 하기 때문이다 — 그 변이도 실패로 관측돼야 한다.
    """
    dialogs: list[tuple[str, str]] = []

    def record(
        _parent: QWidget | None,
        title: str,
        text: str,
        *_arguments: object,
    ) -> QMessageBox.StandardButton:
        dialogs.append((title, text))
        return QMessageBox.StandardButton.Ok

    monkeypatch.setattr(QMessageBox, "warning", record)
    monkeypatch.setattr(QMessageBox, "critical", record)
    return dialogs


def _show(manager: WindowManager, qtbot: QtBot) -> None:
    for window in manager.windows:
        qtbot.addWidget(window)
        window.show()


def _touch(path: Path, text: str) -> Path:
    path.write_bytes(text.encode("utf-8"))
    return path


def _bound_card_id(window: MainWindow) -> str | None:
    page = window.active_document_page()
    return None if page is None else page.editor.card_id


def _decoded_lines(payload: bytes) -> list[str]:
    """소켓으로 나간 열기 명령 줄에서 경로 문자열만 뽑는다."""
    decoded: list[str] = []
    for line in payload.split(b"\n"):
        if not line:
            continue
        prefix, separator, encoded = line.partition(b"\t")
        assert prefix == b"open-file"
        assert separator == b"\t"
        decoded.append(base64.urlsafe_b64decode(encoded).decode("utf-8"))
    return decoded


def test_launch_arguments_collect_positional_paths() -> None:
    options = _parse_arguments(["--database", "db.sqlite3", "첫.txt", "둘째.txt"])

    assert options.paths == [Path("첫.txt"), Path("둘째.txt")]
    assert options.database == Path("db.sqlite3")

    assert _parse_arguments([]).paths == []


def test_first_run_opens_each_path_in_a_new_window_and_activates_the_last(
    qtbot: QtBot,
    repositories: Repositories,
    manager: WindowManager,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    restored = manager.restore_windows()
    _show(manager, qtbot)
    activated = _activation_log(monkeypatch)
    first = _touch(tmp_path / "첫.txt", "첫 본문\n")
    second = _touch(tmp_path / "둘째.txt", "둘째 본문\n")

    assert manager.open_path(first)
    assert manager.open_path(second)
    _show(manager, qtbot)

    assert len(manager.windows) == len(restored) + 2
    first_window, second_window = manager.windows[-2:]
    first_card = _bound_card_id(first_window)
    second_card = _bound_card_id(second_window)
    assert first_card is not None and second_card is not None
    first_binding = repositories.get_file_binding(first_card)
    second_binding = repositories.get_file_binding(second_card)
    assert first_binding is not None
    assert second_binding is not None
    assert Path(first_binding.path) == first.resolve()
    assert Path(second_binding.path) == second.resolve()
    assert activated == [first_window, second_window]


def test_already_bound_path_reuses_the_owning_window(
    qtbot: QtBot,
    manager: WindowManager,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    manager.restore_windows()
    path = _touch(tmp_path / "재사용.txt", "본문\n")
    assert manager.open_path(path)
    _show(manager, qtbot)
    owner = manager.windows[-1]
    owner_card = _bound_card_id(owner)
    window_count = len(manager.windows)
    activated = _activation_log(monkeypatch)

    assert manager.open_path(path)

    assert len(manager.windows) == window_count
    assert _bound_card_id(owner) == owner_card
    # 결속 카드 라우팅도 소유 창을 활성화하므로 호출 수가 아니라 대상만 못박는다.
    assert activated and all(window is owner for window in activated)


def test_bound_path_routes_to_the_owning_window_across_windows(
    qtbot: QtBot,
    manager: WindowManager,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    manager.restore_windows()
    path = _touch(tmp_path / "교차.txt", "본문\n")
    assert manager.open_path(path)
    owner = manager.windows[-1]
    owner_documents = owner.open_document_ids
    latest = manager.create_window()
    _show(manager, qtbot)
    window_count = len(manager.windows)
    activated = _activation_log(monkeypatch)

    assert manager.open_path(path)

    assert len(manager.windows) == window_count
    assert owner.open_document_ids == owner_documents
    assert owner_documents[0] not in latest.open_document_ids
    assert activated and all(window is owner for window in activated)


def test_missing_path_reports_an_error_and_the_app_keeps_running(
    qtbot: QtBot,
    manager: WindowManager,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    manager.restore_windows()
    _show(manager, qtbot)
    window_count = len(manager.windows)
    dialogs = _dialog_log(monkeypatch)

    assert not manager.open_path(tmp_path / "없는파일.txt")

    assert len(manager.windows) == window_count
    assert dialogs == [("파일 열기", "없는파일.txt: 파일을 찾을 수 없습니다.")]

    present = _touch(tmp_path / "있는파일.txt", "본문\n")
    assert manager.open_path(present)
    _show(manager, qtbot)
    assert len(manager.windows) == window_count + 1


def test_rejected_file_argument_reclaims_the_window_it_created(
    qtbot: QtBot,
    manager: WindowManager,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    """결속 불가 파일(제어 문자)은 새 창을 남기지 않는다 — S8 창 총수 술어."""
    manager.restore_windows()
    _show(manager, qtbot)
    window_count = len(manager.windows)
    monkeypatch.setattr(DocumentPage, "_ask_copy_import", lambda _self, _path: False)
    binary = tmp_path / "그림.png"
    binary.write_bytes(b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR")

    assert not manager.open_path(binary)

    QApplication.processEvents()
    assert len(manager.windows) == window_count


def test_directory_argument_is_reported_without_creating_a_window(
    qtbot: QtBot,
    manager: WindowManager,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    manager.restore_windows()
    _show(manager, qtbot)
    window_count = len(manager.windows)
    dialogs = _dialog_log(monkeypatch)
    directory = tmp_path / "폴더"
    directory.mkdir()

    assert not manager.open_path(directory)

    assert len(manager.windows) == window_count
    assert dialogs == [("파일 열기", "폴더: 디렉터리는 열 수 없습니다.")]


def test_notify_existing_keeps_its_single_argument_signature() -> None:
    """경로는 생성자 상태로 받는다 — 기존 시험의 대역 함수가 이 서명에 결속돼 있다."""
    parameters = inspect.signature(SingleInstanceGuard._notify_existing).parameters

    assert list(parameters) == ["self", "timeout_ms"]


def test_second_instance_sends_open_file_lines_instead_of_new_window(
    tmp_path: Path,
) -> None:
    first = tmp_path / "첫.txt"
    second = tmp_path / "둘째.txt"
    guard = SingleInstanceGuard(tmp_path / "data", paths=(first, second))

    payload = guard.launch_message()

    assert b"new-window" not in payload
    assert payload.count(b"\n") == 2
    assert _decoded_lines(payload) == [str(first.resolve()), str(second.resolve())]
    assert SingleInstanceGuard(tmp_path / "data").launch_message() == b"new-window\n"


def test_relative_path_is_resolved_against_the_caller_cwd_before_sending(
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    monkeypatch.chdir(tmp_path)
    guard = SingleInstanceGuard(tmp_path / "data", paths=(Path("상대.txt"),))

    assert guard.paths == ((tmp_path / "상대.txt").resolve(),)
    assert _decoded_lines(guard.launch_message()) == [
        str((tmp_path / "상대.txt").resolve())
    ]


def test_second_instance_delivers_paths_to_the_running_server(
    qtbot: QtBot,
    tmp_path: Path,
) -> None:
    repository_root = Path(__file__).resolve().parents[2]
    data_directory = tmp_path / "instance"
    # 탭이 든 파일명은 줄 프로토콜을 깨는 반례다 — base64 로 감싼 계약을 못박는다.
    paths = (tmp_path / "평범한.txt", tmp_path / "탭\t들어간.txt")
    primary = SingleInstanceGuard(data_directory)
    requested: list[Path] = []
    new_windows: list[object] = []
    primary.open_file_requested.connect(requested.append)
    primary.new_window_requested.connect(lambda: new_windows.append(None))
    process: subprocess.Popen[str] | None = None
    try:
        assert primary.acquire(timeout_ms=200)
        process = subprocess.Popen(
            [
                sys.executable,
                "-c",
                _SECONDARY_OPEN_FILE_SCRIPT,
                str(data_directory),
                *(str(path) for path in paths),
            ],
            cwd=repository_root,
            env=os.environ.copy(),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        qtbot.waitUntil(lambda: len(requested) == len(paths), timeout=5_000)
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 0, stderr

        assert json.loads(stdout)["acquired"] is False
        assert requested == [path.resolve() for path in paths]
        assert new_windows == []
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait(timeout=2)
        primary.close()
        QApplication.processEvents()


def test_undecodable_open_file_command_is_ignored(
    tmp_path: Path,
) -> None:
    guard = SingleInstanceGuard(tmp_path / "data")
    requested: list[Path] = []
    guard.open_file_requested.connect(requested.append)

    guard._handle_command(b"open-file\t%%%\n")
    guard._handle_command(b"open-file\t\n")
    guard._handle_command(b"open-file\tYQ\n")  # 패딩 불량 — binascii.Error
    guard._handle_command(b"open-file\t__4=\n")  # 해독은 되나 UTF-8 아님
    guard._handle_command(b"unknown-command\n")

    assert requested == []
