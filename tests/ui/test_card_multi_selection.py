from __future__ import annotations

from collections.abc import Iterator
from pathlib import Path

from PySide6.QtCore import QEvent, QPoint, QSettings, Qt
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QAbstractItemView, QApplication, QFormLayout
from pytest import MonkeyPatch
from pytestqt.qtbot import QtBot

from pynote.app import SqliteWorkspaceStateStore, initialize_device_settings
from pynote.application import document_service
from pynote.application.card_service import CardService
from pynote.domain.models import Card, Document
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories
from pynote.ui import document_page as document_page_module
from pynote.ui.cards.card_model import CardListModel
from pynote.ui.cards.card_stream import CardStreamView
from pynote.ui.document_page import DocumentPage
from pynote.ui.main_window import MainWindow
from pynote.ui.settings_dialog import SettingsDialog

MULTI_SELECTION_KEY = "cards/multi_selection_enabled"


def _ids() -> Iterator[str]:
    number = 0
    while True:
        number += 1
        yield f"multi-{number}"


def _settings(tmp_path: Path) -> QSettings:
    settings = QSettings(
        str(tmp_path / "multi-selection.ini"),
        QSettings.Format.IniFormat,
    )
    initialize_device_settings(settings)
    settings.setValue("first_run/guide_shown", True)
    return settings


def _page(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    count: int,
    settings: QSettings | None = None,
) -> tuple[DocumentPage, tuple[Card, ...]]:
    document = Document(
        id="multi-document",
        title="다중 선택",
        created_at_us=1_000_000,
        updated_at_us=1_000_000,
    )
    repositories.create_document(document)
    identifiers = _ids()
    times = iter(range(2_000_000, 2_000_000 + count + 20))
    service = CardService(
        database,
        repositories,
        clock=lambda: next(times),
        id_factory=lambda: next(identifiers),
    )
    cards = tuple(
        service.create_card(document.id, f"카드 {number}")
        for number in range(1, count + 1)
    )
    page = DocumentPage(database, repositories, document.id, settings=settings)
    qtbot.addWidget(page)
    page.resize(900, 700)
    page.show()
    qtbot.waitExposed(page)
    page.stream.set_sort_mode("capture")
    return page, cards


def _window(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    settings: QSettings,
    title: str,
) -> MainWindow:
    window = MainWindow(
        repositories,
        SqliteWorkspaceStateStore(database),
        settings=settings,
    )
    qtbot.addWidget(window)
    window.resize(1_000, 700)
    window.show()
    document = document_service.create_document(repositories, title)

    assert window.open_document_local(document.id)

    return window


def _quiesce(page: DocumentPage) -> None:
    """대기 타이머와 열린 편집 세션을 남기지 않고 시험을 끝낸다."""
    page.stream.cancel_pending_browse()
    if page.editor.session is not None:
        page.editor.request_close()


def _select_rows(page: DocumentPage, rows: tuple[int, ...]) -> None:
    """Ctrl 클릭으로 여러 행을 실제 입력 경로대로 선택한다."""
    stream = page.stream
    for position, row in enumerate(rows):
        index = stream.card_model.index(row)
        stream.scrollTo(index)
        QTest.mouseClick(
            stream.viewport(),
            Qt.MouseButton.LeftButton,
            (
                Qt.KeyboardModifier.NoModifier
                if position == 0
                else Qt.KeyboardModifier.ControlModifier
            ),
            stream.visualRect(index).center(),
        )


def test_single_selection_is_the_default(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    settings = _settings(tmp_path)

    # 값이 없을 때도 False 로 읽히므로, 장치 설정에 기본값이 실제로 기록되는지
    # 까지 확인해야 기본값 등재가 빠진 것을 잡는다.
    assert settings.contains(MULTI_SELECTION_KEY)
    assert settings.value(MULTI_SELECTION_KEY, type=bool) is False

    page, cards = _page(qtbot, database, repositories, 6, settings)

    assert (
        page.stream.selectionMode()
        is QAbstractItemView.SelectionMode.SingleSelection
    )

    _select_rows(page, (1, 2, 3))

    assert page.stream.selected_card_ids() == (cards[3].id,)


def test_a_bare_card_stream_starts_in_single_selection(qtbot: QtBot) -> None:
    view = CardStreamView(CardListModel([]))
    qtbot.addWidget(view)

    assert (
        view.selectionMode() is QAbstractItemView.SelectionMode.SingleSelection
    )


def test_a_page_without_device_settings_is_also_single_selection(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _page(qtbot, database, repositories, 6)

    assert (
        page.stream.selectionMode()
        is QAbstractItemView.SelectionMode.SingleSelection
    )

    _select_rows(page, (1, 2))

    assert page.stream.selected_card_ids() == (cards[2].id,)


def test_enabling_restores_multi_selection(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _page(qtbot, database, repositories, 6)
    stream = page.stream
    stream.set_multi_selection_enabled(True)

    assert (
        stream.selectionMode()
        is QAbstractItemView.SelectionMode.ExtendedSelection
    )

    _select_rows(page, (1, 2, 3))

    assert stream.selected_card_ids() == (cards[1].id, cards[2].id, cards[3].id)


def test_disabling_switches_to_single_selection_and_keeps_the_current_card(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _page(qtbot, database, repositories, 6)
    stream = page.stream
    stream.set_multi_selection_enabled(True)
    _select_rows(page, (1, 2, 3))

    assert stream.selected_card_ids() == (cards[1].id, cards[2].id, cards[3].id)

    selection_model = stream.selectionModel()

    assert selection_model is not None

    # setCurrentIndex 는 선택까지 한 장으로 바꾼다 — 여기서 시험하려는 것은
    # "여러 장이 잡힌 채로 끄면" 이므로 선택은 두고 current 만 옮긴다.
    selection_model.setCurrentIndex(
        stream.card_model.index(2),
        selection_model.SelectionFlag.NoUpdate,
    )

    stream.set_multi_selection_enabled(False)

    assert (
        stream.selectionMode() is QAbstractItemView.SelectionMode.SingleSelection
    )
    assert stream.selected_card_ids() == (cards[2].id,)
    assert stream.currentIndex().row() == 2


def test_disabling_without_a_selected_current_keeps_the_first_row(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _page(qtbot, database, repositories, 6)
    stream = page.stream
    stream.set_multi_selection_enabled(True)
    _select_rows(page, (2, 4))
    selection_model = stream.selectionModel()

    assert selection_model is not None

    selection_model.setCurrentIndex(
        stream.card_model.index(5),
        selection_model.SelectionFlag.NoUpdate,
    )

    stream.set_multi_selection_enabled(False)

    assert stream.selected_card_ids() == (cards[2].id,)


def test_disabled_list_never_selects_more_than_one_card(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _page(qtbot, database, repositories, 6)
    stream = page.stream

    _select_rows(page, (1, 2, 3))

    assert stream.selected_card_ids() == (cards[3].id,)

    index = stream.card_model.index(0)
    stream.scrollTo(index)
    QTest.mouseClick(
        stream.viewport(),
        Qt.MouseButton.LeftButton,
        Qt.KeyboardModifier.ShiftModifier,
        stream.visualRect(index).center(),
    )

    assert len(stream.selected_card_ids()) == 1


def test_disabled_delete_request_carries_exactly_one_card(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    page, cards = _page(qtbot, database, repositories, 6)
    stream = page.stream
    requested: list[tuple[str, ...]] = []
    stream.cards_delete_requested.connect(requested.append)
    stream.set_multi_selection_enabled(True)
    _select_rows(page, (1, 2, 3))

    assert len(stream.selected_card_ids()) == 3

    stream.set_multi_selection_enabled(False)
    stream.setFocus()

    QTest.keyClick(stream, Qt.Key.Key_Delete)

    assert requested == [(cards[3].id,)]


def test_settings_dialog_saves_and_cancels_the_option(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    settings = _settings(tmp_path)
    dialog = SettingsDialog(settings)
    qtbot.addWidget(dialog)

    assert not dialog.multi_selection.isChecked()

    dialog.multi_selection.setChecked(True)
    dialog.apply_settings()

    assert settings.value(MULTI_SELECTION_KEY, type=bool) is True

    reopened = SettingsDialog(settings)
    qtbot.addWidget(reopened)

    assert reopened.multi_selection.isChecked()

    reopened.multi_selection.setChecked(False)
    reopened.reject()

    assert settings.value(MULTI_SELECTION_KEY, type=bool) is True

    page, _cards = _page(qtbot, database, repositories, 6, settings)

    assert (
        page.stream.selectionMode()
        is QAbstractItemView.SelectionMode.ExtendedSelection
    )


def test_view_menu_toggle_persists_and_applies_to_the_open_list(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    settings = _settings(tmp_path)
    window = _window(qtbot, database, repositories, settings, "메뉴 토글")
    page = window.active_document_page()

    assert page is not None
    assert not window.multi_selection_action.isChecked()
    assert (
        page.stream.selectionMode()
        is QAbstractItemView.SelectionMode.SingleSelection
    )

    window.multi_selection_action.setChecked(True)

    assert settings.value(MULTI_SELECTION_KEY, type=bool) is True
    assert (
        page.stream.selectionMode()
        is QAbstractItemView.SelectionMode.ExtendedSelection
    )

    window.multi_selection_action.setChecked(False)

    assert settings.value(MULTI_SELECTION_KEY, type=bool) is False
    assert (
        page.stream.selectionMode()
        is QAbstractItemView.SelectionMode.SingleSelection
    )


def test_settings_change_syncs_the_view_menu_check(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    settings = _settings(tmp_path)
    window = _window(qtbot, database, repositories, settings, "동기화")

    assert not window.multi_selection_action.isChecked()

    settings.setValue(MULTI_SELECTION_KEY, True)
    window._apply_settings()

    assert window.multi_selection_action.isChecked()

    page = window.active_document_page()

    assert page is not None
    assert (
        page.stream.selectionMode()
        is QAbstractItemView.SelectionMode.ExtendedSelection
    )


def test_opening_settings_wires_the_dialog_back_to_the_window(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    settings = _settings(tmp_path)
    window = _window(qtbot, database, repositories, settings, "대화상자 배선")
    opened: list[SettingsDialog] = []

    def capture(dialog: SettingsDialog) -> int:
        opened.append(dialog)
        return 0

    monkeypatch.setattr(SettingsDialog, "exec", capture)
    window._open_settings()

    assert len(opened) == 1

    dialog = opened[0]
    dialog.multi_selection.setChecked(True)
    dialog.apply_settings()

    assert window.multi_selection_action.isChecked()

    page = window.active_document_page()

    assert page is not None
    assert (
        page.stream.selectionMode()
        is QAbstractItemView.SelectionMode.ExtendedSelection
    )


def test_new_window_starts_from_the_persisted_option(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    settings = _settings(tmp_path)
    settings.setValue(MULTI_SELECTION_KEY, True)
    window = _window(qtbot, database, repositories, settings, "재시작")

    assert window.multi_selection_action.isChecked()

    page = window.active_document_page()

    assert page is not None
    assert (
        page.stream.selectionMode()
        is QAbstractItemView.SelectionMode.ExtendedSelection
    )


def test_disabled_export_writes_only_the_one_selected_card(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    monkeypatch: MonkeyPatch,
) -> None:
    page, cards = _page(qtbot, database, repositories, 6)
    stream = page.stream
    target = tmp_path / "내보내기.txt"
    # 실제 소비자를 태우되 파일 대화상자만 가로챈다 — 가로채지 않으면 시험이
    # 모달에서 멈춘다.
    monkeypatch.setattr(
        document_page_module.QFileDialog,
        "getSaveFileName",
        staticmethod(
            lambda *_args, **_kwargs: (str(target), "텍스트 파일 (*.txt)")
        ),
    )
    requested: list[tuple[str, ...]] = []
    stream.cards_export_requested.connect(requested.append)
    stream.set_multi_selection_enabled(True)
    _select_rows(page, (1, 2, 3))

    assert len(stream.selected_card_ids()) == 3

    stream.set_multi_selection_enabled(False)
    menu = stream._build_context_menu(stream.card_model.index(3))

    assert menu is not None

    export_action = next(
        action for action in menu.actions() if action.text() == "파일로 내보내기"
    )
    export_action.trigger()

    assert requested == [(cards[3].id,)]
    assert cards[3].body in target.read_text(encoding="utf-8")
    assert cards[1].body not in target.read_text(encoding="utf-8")
    _quiesce(page)


def test_both_entry_points_are_actually_reachable_in_the_ui(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    settings = _settings(tmp_path)
    window = _window(qtbot, database, repositories, settings, "진입점 노출")

    assert window.multi_selection_action in window.view_menu.actions()

    dialog = SettingsDialog(settings)
    qtbot.addWidget(dialog)
    form = dialog.findChild(QFormLayout)

    assert form is not None

    fields = [
        form.itemAt(row, QFormLayout.ItemRole.FieldRole)
        for row in range(form.rowCount())
    ]

    assert dialog.multi_selection in [
        item.widget() for item in fields if item is not None
    ]


def test_activating_a_window_picks_up_another_windows_change(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    settings = _settings(tmp_path)
    first = _window(qtbot, database, repositories, settings, "창 A")
    second = _window(qtbot, database, repositories, settings, "창 B")
    second.multi_selection_action.setChecked(True)

    # 값을 바꾼 창만 즉시 반영된다 — 다른 창은 아직 옛 표시를 들고 있다.
    assert not first.multi_selection_action.isChecked()

    QApplication.sendEvent(first, QEvent(QEvent.Type.WindowActivate))

    assert first.multi_selection_action.isChecked()

    page = first.active_document_page()

    assert page is not None
    assert (
        page.stream.selectionMode()
        is QAbstractItemView.SelectionMode.ExtendedSelection
    )


def test_the_option_survives_a_fresh_settings_instance(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    settings = _settings(tmp_path)
    window = _window(qtbot, database, repositories, settings, "재시작 영속")
    window.multi_selection_action.setChecked(True)

    # 시험이 sync() 를 대신 부르면 제품이 저장을 확정하지 않아도 통과한다.
    # 같은 객체를 다시 읽는 것도 메모리 값만 보는 것이라, 파일에서 새로 읽어야
    # 재시작 뒤에도 유지된다는 계약이 증명된다.
    reopened = QSettings(
        str(tmp_path / "multi-selection.ini"),
        QSettings.Format.IniFormat,
    )

    assert reopened.value(MULTI_SELECTION_KEY, type=bool) is True

    page, _cards = _page(qtbot, database, repositories, 4, reopened)

    assert (
        page.stream.selectionMode()
        is QAbstractItemView.SelectionMode.ExtendedSelection
    )


def test_wheel_browsing_still_moves_one_card_when_disabled(
    qtbot: QtBot,
    database: Database,
    repositories: Repositories,
) -> None:
    from PySide6.QtCore import QPointF
    from PySide6.QtGui import QWheelEvent

    page, cards = _page(qtbot, database, repositories, 8)
    stream = page.stream

    assert (
        stream.selectionMode() is QAbstractItemView.SelectionMode.SingleSelection
    )

    stream.setCurrentIndex(stream.card_model.index(0))
    position = QPointF(stream.viewport().rect().center())
    event = QWheelEvent(
        position,
        position,
        QPoint(0, 0),
        QPoint(0, -120),
        Qt.MouseButton.NoButton,
        Qt.KeyboardModifier.NoModifier,
        Qt.ScrollPhase.NoScrollPhase,
        False,
    )
    QApplication.sendEvent(stream.viewport(), event)

    assert stream.currentIndex().row() == 1
    assert stream.selected_card_ids() == (cards[1].id,)

    stream.cancel_pending_browse()
