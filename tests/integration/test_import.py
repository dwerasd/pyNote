from __future__ import annotations

import codecs
from pathlib import Path
from types import TracebackType
from typing import IO, Any, cast

import pytest
from PySide6.QtCore import QRunnable, QThreadPool
from pytest import MonkeyPatch
from pytestqt.qtbot import QtBot

from pynote.application.card_service import CardService
from pynote.domain.models import CaptureOperationSource, CardSource, Document, SplitPolicy
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories
from pynote.ui import import_dialog as import_dialog_module
from pynote.ui.import_dialog import ImportController, prepare_import


class _ManualThreadPool:
    def __init__(self) -> None:
        self._workers: list[QRunnable] = []

    def start(self, worker: QRunnable) -> None:
        self._workers.append(worker)

    @property
    def pending_count(self) -> int:
        return len(self._workers)

    def complete(self) -> None:
        assert self._workers
        self._workers.pop(0).run()


def _create_document(
    repositories: Repositories,
    *,
    document_id: str = "document-1",
) -> Document:
    document = Document(
        id=document_id,
        title="가져오기",
        created_at_us=1,
        updated_at_us=1,
    )
    repositories.create_document(document)
    return document


def _import_side_effect_counts(database: Database) -> tuple[int, ...]:
    # 가져오기 한 건이 만들 수 있는 카드·리비전·작업·이벤트 행의 수를 함께 센다.
    tables = ("cards", "card_revisions", "capture_operations", "edit_events")
    return tuple(
        database.connection.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
        for table in tables
    )


def test_one_card_import_preserves_decoded_text(
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    document = _create_document(repositories)
    path = tmp_path / "note.txt"
    original = "첫 줄\r\n\r\n둘째 줄"
    path.write_bytes(b"\xef\xbb\xbf" + original.encode())

    preparation = prepare_import(path)
    cards = CardService(database, repositories).create_cards(
        document.id,
        preparation.text,
        source=CaptureOperationSource.IMPORT,
        split=False,
    )

    assert tuple(card.body for card in cards) == (original,)


def test_import_file_limit_constant_and_exact_boundary_passes(
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    document = _create_document(repositories)
    assert import_dialog_module.MAX_IMPORT_FILE_BYTES == 4 * 1024 * 1024
    path = tmp_path / "boundary.data"
    path.write_bytes(b"x" * import_dialog_module.MAX_IMPORT_FILE_BYTES)

    preparation = prepare_import(path)
    cards = CardService(database, repositories).create_cards(
        document.id,
        preparation.text,
        source=CaptureOperationSource.IMPORT,
        split=False,
    )

    assert len(cards) == 1
    assert len(cards[0].body) == import_dialog_module.MAX_IMPORT_FILE_BYTES


def test_import_rejects_one_byte_over_limit_after_bounded_read(
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
) -> None:
    limit = import_dialog_module.MAX_IMPORT_FILE_BYTES
    path = tmp_path / "oversized.data"
    path.write_bytes(b"x" * (limit + 1))
    original_open = Path.open
    read_calls: list[tuple[int, int]] = []

    class _ObservedReader:
        def __init__(self, stream: IO[bytes]) -> None:
            self._stream = stream

        def __enter__(self) -> _ObservedReader:
            self._stream.__enter__()
            return self

        def __exit__(
            self,
            exception_type: type[BaseException] | None,
            exception: BaseException | None,
            traceback: TracebackType | None,
        ) -> bool | None:
            return self._stream.__exit__(exception_type, exception, traceback)

        def read(self, size: int = -1) -> bytes:
            data = self._stream.read(size)
            read_calls.append((size, len(data)))
            return data

    def open_spy(target: Path, *args: Any, **kwargs: Any) -> Any:
        stream = original_open(target, *args, **kwargs)
        mode = args[0] if args else kwargs.get("mode")
        if target == path and mode == "rb":
            return _ObservedReader(stream)
        return stream

    monkeypatch.setattr(Path, "open", open_spy)

    with pytest.raises(ValueError) as error:
        prepare_import(path)

    assert str(error.value) == "파일당 4 MiB 상한을 초과했습니다."
    assert read_calls == [(limit + 1, limit + 1)]

    document = _create_document(repositories)
    thread_pool = _ManualThreadPool()
    controller = ImportController(
        CardService(database, repositories),
        lambda _window_id, _value: None,
        lambda _window_id, _message: None,
        thread_pool=cast(QThreadPool, thread_pool),
    )
    controller.start_import("import-window", document.id, path)

    with qtbot.waitSignal(controller.failed, timeout=1_000) as blocker:
        thread_pool.complete()

    assert blocker.args == ["파일당 4 MiB 상한을 초과했습니다."]
    assert read_calls == [(limit + 1, limit + 1), (limit + 1, limit + 1)]


def test_prepare_import_from_bytes_has_no_file_limit(
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    document = _create_document(repositories)
    data = b"x" * (import_dialog_module.MAX_IMPORT_FILE_BYTES + 1)

    preparation = import_dialog_module.prepare_import_from_bytes(
        tmp_path / "memory.data",
        data,
    )
    cards = CardService(database, repositories).create_cards(
        document.id,
        preparation.text,
        source=CaptureOperationSource.IMPORT,
        split=False,
    )

    assert len(cards) == 1
    assert cards[0].body == data.decode()


def test_decode_import_bytes_honors_bom_utf8_and_fixed_ansi_fallback(
    monkeypatch: MonkeyPatch,
) -> None:
    monkeypatch.setattr(import_dialog_module, "_ANSI_ENCODING", "cp949")
    utf8_bom_text = "UTF-8 BOM 한글"
    utf16_le_text = "UTF-16 LE 한글"
    utf16_be_text = "UTF-16 BE 한글"
    utf8_text = "순수 UTF-8 한글"
    ansi_text = "고정 ANSI 한글"

    assert import_dialog_module.decode_import_bytes(
        codecs.BOM_UTF8 + utf8_bom_text.encode("utf-8")
    ) == utf8_bom_text
    assert import_dialog_module.decode_import_bytes(
        codecs.BOM_UTF16_LE + utf16_le_text.encode("utf-16-le")
    ) == utf16_le_text
    assert import_dialog_module.decode_import_bytes(
        codecs.BOM_UTF16_BE + utf16_be_text.encode("utf-16-be")
    ) == utf16_be_text
    assert import_dialog_module.decode_import_bytes(utf8_text.encode("utf-8")) == utf8_text
    assert import_dialog_module.decode_import_bytes(ansi_text.encode("cp949")) == ansi_text


@pytest.mark.parametrize(
    "data",
    [bytes(range(256)), b"\x81", b"\xf0\x9f"],
    ids=["전체-바이트값", "잘린-cp949", "잘린-utf8"],
)
def test_decode_import_bytes_ansi_fallback_matches_replacement_decoding_for_corpus(
    monkeypatch: MonkeyPatch,
    data: bytes,
) -> None:
    ansi_encoding = "cp949"
    monkeypatch.setattr(import_dialog_module, "_ANSI_ENCODING", ansi_encoding)

    assert import_dialog_module.decode_import_bytes(data) == data.decode(
        ansi_encoding,
        errors="replace",
    )


def test_paragraph_import_has_import_source_and_one_operation(
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    document = _create_document(repositories)
    path = tmp_path / "note.md"
    original = "첫 문단\r\n계속\r\n\r\n둘째 문단"
    path.write_bytes(original.encode())
    preparation = prepare_import(path)
    service = CardService(
        database,
        repositories,
        clock=lambda: 10,
        id_factory=(f"id-{number}" for number in range(20)).__next__,
    )

    cards = service.create_cards(
        document.id,
        preparation.text,
        source=CaptureOperationSource.IMPORT,
        split=True,
    )

    assert tuple(card.body for card in cards) == ("첫 문단\n계속", "둘째 문단")
    assert {card.source for card in cards} == {CardSource.IMPORT}
    assert len({card.operation_id for card in cards}) == 1
    operation = repositories.get_capture_operation(cards[0].operation_id)
    assert operation is not None
    assert operation.source is CaptureOperationSource.IMPORT
    assert operation.split_policy is SplitPolicy.SPLIT_BY_BLANK_LINE
    assert operation.original_text == original


def test_import_decodes_invalid_utf8_into_card(
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    document = _create_document(repositories)
    path = tmp_path / "broken.data"
    path.write_bytes(b"\x80notepad")

    preparation = prepare_import(path)
    cards = CardService(database, repositories).create_cards(
        document.id,
        preparation.text,
        source=CaptureOperationSource.IMPORT,
        split=False,
    )

    assert len(cards) == 1
    assert "notepad" in cards[0].body


def test_card_service_rejects_extended_whitespace_without_side_effects(
    database: Database,
    repositories: Repositories,
) -> None:
    document = _create_document(repositories)
    tables = ("cards", "card_revisions", "capture_operations", "edit_events")
    before_counts = tuple(
        database.connection.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
        for table in tables
    )
    before_events = repositories.list_events(document.id)

    with pytest.raises(ValueError) as error:
        CardService(database, repositories).create_cards(
            document.id,
            "\v\f\x1c\x1f\u00a0\u2003",
        )

    after_counts = tuple(
        database.connection.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
        for table in tables
    )
    assert str(error.value) == "빈 문자열 또는 공백만 있는 입력은 저장할 수 없습니다."
    assert after_counts == before_counts
    assert repositories.list_events(document.id) == before_events


@pytest.mark.parametrize(
    "blank_bytes",
    [
        pytest.param(b" \r\n\t", id="mixed-whitespace"),
        pytest.param(b"\r", id="single-carriage-return"),
        pytest.param(b"\v", id="vertical-tab"),
        pytest.param(b"\f", id="form-feed"),
        pytest.param(b"\v\f", id="vertical-tab-form-feed"),
        pytest.param(b"\x1c\x1d\x1e\x1f", id="information-separators"),
        pytest.param("\u00a0\u2003".encode(), id="unicode-whitespace"),
    ],
)
def test_import_rejects_blank_text(tmp_path: Path, blank_bytes: bytes) -> None:
    blank = tmp_path / "blank.data"
    blank.write_bytes(blank_bytes)

    try:
        prepare_import(blank)
    except ValueError as error:
        assert str(error) == "가져올 비어 있지 않은 문단이 없습니다."
    else:
        raise AssertionError("공백뿐인 파일이 거부되어야 합니다.")


def test_import_controller_rejects_extended_whitespace_without_side_effects(
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    qtbot: QtBot,
) -> None:
    document = _create_document(repositories)
    path = tmp_path / "extended-whitespace.txt"
    path.write_bytes(b"\v\f")
    thread_pool = _ManualThreadPool()
    controller = ImportController(
        CardService(database, repositories),
        lambda _window_id, _value: None,
        lambda _window_id, _message: None,
        thread_pool=cast(QThreadPool, thread_pool),
    )
    imported: list[object] = []
    controller.imported.connect(imported.append)
    tables = ("cards", "card_revisions", "capture_operations", "edit_events")
    before_counts = tuple(
        database.connection.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
        for table in tables
    )
    before_events = repositories.list_events(document.id)

    controller.start_import("import-window", document.id, path)
    with qtbot.waitSignal(controller.failed, timeout=1_000) as blocker:
        thread_pool.complete()

    after_counts = tuple(
        database.connection.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
        for table in tables
    )
    assert blocker.args == ["가져올 비어 있지 않은 문단이 없습니다."]
    assert imported == []
    assert after_counts == before_counts
    assert repositories.list_events(document.id) == before_events


def test_async_import_stays_with_document_selected_at_start(
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    qtbot: QtBot,
) -> None:
    original_document = _create_document(
        repositories,
        document_id="document-original",
    )
    switched_document = _create_document(
        repositories,
        document_id="document-switched",
    )
    thread_pool = _ManualThreadPool()
    completion_calls: list[tuple[str, object]] = []
    failure_calls: list[tuple[str, str]] = []
    service = CardService(
        database,
        repositories,
        clock=lambda: 10,
        id_factory=(f"id-{number}" for number in range(20)).__next__,
    )
    controller = ImportController(
        service,
        lambda window_id, value: completion_calls.append((window_id, value)),
        lambda window_id, message: failure_calls.append((window_id, message)),
        thread_pool=cast(QThreadPool, thread_pool),
    )
    original_path = tmp_path / "original.txt"
    original_path.write_text("원래 문서의 카드", encoding="utf-8")
    switched_path = tmp_path / "switched.txt"
    switched_path.write_text("전환된 문서의 카드", encoding="utf-8")

    controller.start_import("import-window", original_document.id, original_path)
    controller.start_import("import-window", switched_document.id, switched_path)
    # 같은 요청 창의 후속 B 요청이 있어도 먼저 시작한 A 요청의 FIFO worker를 완료한다.
    assert thread_pool.pending_count == 2
    with qtbot.waitSignal(controller.imported, timeout=1_000):
        thread_pool.complete()

    # 1은 B worker를 미완료로 남겨 아래 단언이 첫 A 요청의 결과만 본다는 뜻이다.
    assert thread_pool.pending_count == 1
    original_cards = repositories.list_cards(original_document.id)
    assert tuple(card.body for card in original_cards) == (
        "원래 문서의 카드",
    )
    assert repositories.list_cards(switched_document.id) == ()
    assert completion_calls == [("import-window", original_cards)]
    assert failure_calls == []


@pytest.mark.parametrize(
    "document_id",
    [
        pytest.param("", id="empty-string"),
        pytest.param(None, id="none"),
        pytest.param(7, id="integer"),
    ],
)
def test_import_controller_rejects_invalid_document_id_synchronously_without_worker(
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
    document_id: object,
) -> None:
    document = _create_document(repositories)
    path = tmp_path / "invalid-document-id.txt"
    path.write_text("시작되면 안 되는 가져오기", encoding="utf-8")
    thread_pool = _ManualThreadPool()
    completion_calls: list[tuple[str, object]] = []
    failure_calls: list[tuple[str, str]] = []
    imported: list[object] = []
    failed: list[str] = []
    modal_calls: list[tuple[object, ...]] = []
    controller = ImportController(
        CardService(database, repositories),
        lambda window_id, value: completion_calls.append((window_id, value)),
        lambda window_id, message: failure_calls.append((window_id, message)),
        thread_pool=cast(QThreadPool, thread_pool),
    )
    controller.imported.connect(imported.append)
    controller.failed.connect(failed.append)
    monkeypatch.setattr(
        import_dialog_module.QMessageBox,
        "critical",
        lambda *args: modal_calls.append(args),
    )
    before_counts = _import_side_effect_counts(database)

    controller.start_import("invalid-id-window", document_id, path)

    expected_message = "가져올 활성 문서가 없습니다."
    # 0은 start_import()가 thread_pool.start()를 호출하지 않았다는 뜻이다.
    assert thread_pool.pending_count == 0
    assert failed == [expected_message]
    assert failure_calls == [("invalid-id-window", expected_message)]
    assert imported == []
    assert completion_calls == []
    assert modal_calls == []
    assert _import_side_effect_counts(database) == before_counts
    assert repositories.list_cards(document.id) == ()


def test_import_controller_silently_ignores_new_request_during_shutdown(
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
    caplog: pytest.LogCaptureFixture,
) -> None:
    document = _create_document(repositories)
    path = tmp_path / "shutdown-new-request.txt"
    path.write_text("종료 뒤에는 시작되지 않는다", encoding="utf-8")
    thread_pool = _ManualThreadPool()
    completion_calls: list[tuple[str, object]] = []
    failure_calls: list[tuple[str, str]] = []
    imported: list[object] = []
    failed: list[str] = []
    modal_calls: list[tuple[object, ...]] = []
    controller = ImportController(
        CardService(database, repositories),
        lambda window_id, value: completion_calls.append((window_id, value)),
        lambda window_id, message: failure_calls.append((window_id, message)),
        thread_pool=cast(QThreadPool, thread_pool),
    )
    controller.imported.connect(imported.append)
    controller.failed.connect(failed.append)
    monkeypatch.setattr(
        import_dialog_module.QMessageBox,
        "critical",
        lambda *args: modal_calls.append(args),
    )
    before_counts = _import_side_effect_counts(database)

    controller.begin_shutdown()
    with caplog.at_level("WARNING", logger=import_dialog_module.__name__):
        controller.start_import("shutdown-window", document.id, path)

    warning_records = [
        (record.levelname, record.getMessage())
        for record in caplog.records
        if record.name == import_dialog_module.__name__
    ]
    # 0은 종료 뒤 신규 요청에서 worker 기동과 모든 관찰 통지가 없었다는 뜻이다.
    assert thread_pool.pending_count == 0
    assert imported == []
    assert failed == []
    assert completion_calls == []
    assert failure_calls == []
    assert modal_calls == []
    assert warning_records == [
        (
            "WARNING",
            "종료 중인 가져오기 요청을 시작하지 않습니다: window=shutdown-window",
        )
    ]
    assert _import_side_effect_counts(database) == before_counts


def test_shutdown_discards_prepared_import_before_database_commit(
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    qtbot: QtBot,
) -> None:
    document = _create_document(repositories)
    path = tmp_path / "shutdown-before-commit.txt"
    path.write_text("확정 전에 폐기할 카드", encoding="utf-8")
    thread_pool = _ManualThreadPool()
    completion_calls: list[tuple[str, object]] = []
    failure_calls: list[tuple[str, str]] = []
    imported: list[object] = []
    failed: list[str] = []
    controller = ImportController(
        CardService(database, repositories),
        lambda window_id, value: completion_calls.append((window_id, value)),
        lambda window_id, message: failure_calls.append((window_id, message)),
        thread_pool=cast(QThreadPool, thread_pool),
    )
    controller.imported.connect(imported.append)
    controller.failed.connect(failed.append)
    before_counts = _import_side_effect_counts(database)

    controller.start_import("pre-commit-window", document.id, path)
    # 1은 종료 플래그보다 먼저 worker가 실제로 pool에 들어갔다는 뜻이다.
    assert thread_pool.pending_count == 1
    controller.begin_shutdown()
    thread_pool.complete()

    assert thread_pool.pending_count == 0
    assert repositories.list_cards(document.id) == ()
    assert _import_side_effect_counts(database) == before_counts
    assert completion_calls == []
    assert failure_calls == []
    assert imported == []
    assert failed == []


def test_shutdown_discards_worker_failure_without_signal_or_router(
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
    caplog: pytest.LogCaptureFixture,
) -> None:
    document = _create_document(repositories)
    missing_path = tmp_path / "shutdown-worker-failure-missing.txt"
    assert not missing_path.exists()
    thread_pool = _ManualThreadPool()
    completion_calls: list[tuple[str, object]] = []
    failure_calls: list[tuple[str, str]] = []
    imported: list[object] = []
    failed: list[str] = []
    modal_calls: list[tuple[object, ...]] = []
    controller = ImportController(
        CardService(database, repositories),
        lambda window_id, value: completion_calls.append((window_id, value)),
        lambda window_id, message: failure_calls.append((window_id, message)),
        thread_pool=cast(QThreadPool, thread_pool),
    )
    controller.imported.connect(imported.append)
    controller.failed.connect(failed.append)
    monkeypatch.setattr(
        import_dialog_module.QMessageBox,
        "critical",
        lambda *args: modal_calls.append(args),
    )
    before_counts = _import_side_effect_counts(database)

    controller.start_import("shutdown-failure-window", document.id, missing_path)
    assert thread_pool.pending_count == 1
    assert len(controller._workers) == 1
    controller.begin_shutdown()
    with caplog.at_level("WARNING", logger=import_dialog_module.__name__):
        thread_pool.complete()

    discard_records = [
        (record.levelname, record.getMessage())
        for record in caplog.records
        if record.name == import_dialog_module.__name__
        and "종료 중인 가져오기 실패 결과를 폐기" in record.getMessage()
    ]
    assert thread_pool.pending_count == 0
    assert controller._workers == set()
    assert failed == []
    assert failure_calls == []
    assert completion_calls == []
    assert imported == []
    assert modal_calls == []
    assert repositories.list_cards(document.id) == ()
    assert _import_side_effect_counts(database) == before_counts
    assert discard_records == [
        (
            "WARNING",
            "종료 중인 가져오기 실패 결과를 폐기합니다: "
            "window=shutdown-failure-window",
        )
    ]


def test_committed_import_finishes_routing_after_shutdown_begins(
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
) -> None:
    document = _create_document(repositories)
    path = tmp_path / "shutdown-after-commit.txt"
    path.write_text("확정 뒤에는 끝까지 라우팅할 카드", encoding="utf-8")
    thread_pool = _ManualThreadPool()
    order: list[str] = []
    completion_calls: list[tuple[str, object]] = []
    published_document_ids: list[str] = []
    failure_calls: list[tuple[str, str]] = []
    imported: list[object] = []
    failed: list[str] = []
    service = CardService(database, repositories)

    def completion_router(request_window_id: str, value: object) -> None:
        order.append("completion-router")
        completion_calls.append((request_window_id, value))
        cards = cast(tuple[Any, ...], value)
        order.append("bus")
        published_document_ids.append(cards[-1].document_id)

    controller = ImportController(
        service,
        completion_router,
        lambda window_id, message: failure_calls.append((window_id, message)),
        thread_pool=cast(QThreadPool, thread_pool),
    )

    def record_imported(value: object) -> None:
        order.append("imported")
        imported.append(value)

    controller.imported.connect(record_imported)
    controller.failed.connect(failed.append)
    original_create_cards = service.create_cards

    def commit_then_begin_shutdown(
        document_id: str,
        text: str,
        **kwargs: Any,
    ) -> tuple[Any, ...]:
        cards = original_create_cards(document_id, text, **kwargs)
        order.append("db-commit")
        controller.begin_shutdown()
        return cards

    monkeypatch.setattr(service, "create_cards", commit_then_begin_shutdown)

    controller.start_import("post-commit-window", document.id, path)
    with qtbot.waitSignal(controller.imported, timeout=1_000):
        thread_pool.complete()

    created_cards = repositories.list_cards(document.id)
    assert tuple(card.body for card in created_cards) == (
        "확정 뒤에는 끝까지 라우팅할 카드",
    )
    assert order == ["db-commit", "completion-router", "bus", "imported"]
    assert completion_calls == [("post-commit-window", created_cards)]
    assert published_document_ids == [document.id]
    assert imported == [created_cards]
    assert failure_calls == []
    assert failed == []


def test_import_controller_routes_database_commit_failure_once_without_modal(
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
) -> None:
    path = tmp_path / "missing-document.txt"
    path.write_text("존재하지 않는 문서로 갈 카드", encoding="utf-8")
    thread_pool = _ManualThreadPool()
    completion_calls: list[tuple[str, object]] = []
    failure_calls: list[tuple[str, str]] = []
    imported: list[object] = []
    failed: list[str] = []
    modal_calls: list[tuple[object, ...]] = []
    controller = ImportController(
        CardService(database, repositories),
        lambda window_id, value: completion_calls.append((window_id, value)),
        lambda window_id, message: failure_calls.append((window_id, message)),
        thread_pool=cast(QThreadPool, thread_pool),
    )
    controller.imported.connect(imported.append)
    controller.failed.connect(failed.append)
    monkeypatch.setattr(
        import_dialog_module.QMessageBox,
        "critical",
        lambda *args: modal_calls.append(args),
    )
    before_counts = _import_side_effect_counts(database)

    controller.start_import("db-failure-window", "missing-document", path)
    with qtbot.waitSignal(controller.failed, timeout=1_000) as blocker:
        thread_pool.complete()

    assert len(failed) == 1
    assert blocker.args == failed
    assert "존재하지 않는 문서입니다: missing-document" in failed[0]
    assert failure_calls == [("db-failure-window", failed[0])]
    assert imported == []
    assert completion_calls == []
    assert modal_calls == []
    assert _import_side_effect_counts(database) == before_counts


def test_import_worker_generalizes_oserror(
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
    qtbot: QtBot,
    monkeypatch: MonkeyPatch,
) -> None:
    document = _create_document(repositories)
    thread_pool = _ManualThreadPool()
    service = CardService(database, repositories)
    completion_calls: list[tuple[str, object]] = []
    failure_calls: list[tuple[str, str]] = []
    imported: list[object] = []
    failed: list[str] = []
    modal_calls: list[tuple[object, ...]] = []
    controller = ImportController(
        service,
        lambda window_id, value: completion_calls.append((window_id, value)),
        lambda window_id, message: failure_calls.append((window_id, message)),
        thread_pool=cast(QThreadPool, thread_pool),
    )
    controller.imported.connect(imported.append)
    controller.failed.connect(failed.append)
    monkeypatch.setattr(
        import_dialog_module.QMessageBox,
        "critical",
        lambda *args: modal_calls.append(args),
    )
    before_counts = _import_side_effect_counts(database)

    controller.start_import(
        "import-window",
        document.id,
        tmp_path / "존재하지-않는-파일.data",
    )

    with qtbot.waitSignal(controller.failed, timeout=1_000) as blocker:
        thread_pool.complete()

    expected_message = (
        "파일을 가져올 수 없습니다. "
        "파일을 읽을 수 있는지, 가져올 내용이 비어 있지 않은지 확인하세요."
    )
    assert blocker.args == [expected_message]
    assert failed == [expected_message]
    assert failure_calls == [("import-window", expected_message)]
    assert imported == []
    assert completion_calls == []
    assert modal_calls == []
    assert _import_side_effect_counts(database) == before_counts
    assert repositories.list_cards(document.id) == ()
