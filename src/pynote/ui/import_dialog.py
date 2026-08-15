from __future__ import annotations

import codecs
import locale
import logging
import sys
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from PySide6.QtCore import QObject, QRunnable, QThreadPool, Signal, Slot
from PySide6.QtWidgets import (
    QDialog,
    QDialogButtonBox,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from pynote.application.card_service import CardService
from pynote.domain.models import CaptureOperationSource
from pynote.domain.paragraph_parser import ParagraphParser

LOGGER = logging.getLogger(__name__)

MAX_IMPORT_FILE_BYTES = 4 * 1024 * 1024

# 메모장의 최종 폴백과 같은 인코딩: Windows는 시스템 ANSI 코드페이지, 그 밖은 로케일 기본값이다.
_ANSI_ENCODING = "mbcs" if sys.platform == "win32" else locale.getpreferredencoding(False)


@dataclass(frozen=True, slots=True)
class ImportRequest:
    """가져오기 시작 시점의 대상과 입력을 고정한 요청이다."""

    request_window_id: str
    document_id: str
    path: Path


@dataclass(frozen=True, slots=True)
class ImportPreparation:
    """worker에서 준비해 GUI 스레드로 전달하는 불변 가져오기 데이터다."""

    path: Path
    text: str


def prepare_import(path: Path) -> ImportPreparation:
    """확장자와 무관하게 파일을 읽어 카드 본문 후보를 준비한다."""
    with path.open("rb") as source:
        data = source.read(MAX_IMPORT_FILE_BYTES + 1)
    if len(data) > MAX_IMPORT_FILE_BYTES:
        raise ValueError("파일당 4 MiB 상한을 초과했습니다.")
    return prepare_import_from_bytes(path, data)


def prepare_import_from_bytes(
    path: Path,
    data: bytes,
) -> ImportPreparation:
    """고정된 byte snapshot을 카드 본문 후보로 준비한다."""
    text = decode_import_bytes(data)

    parser = ParagraphParser()
    paragraphs = parser.split(text)
    if not paragraphs:
        raise ValueError("가져올 비어 있지 않은 문단이 없습니다.")
    return ImportPreparation(path=path, text=text)


def decode_import_bytes(data: bytes) -> str:
    """메모장과 같은 순서(BOM → UTF-8 → ANSI)로 어떤 바이트든 텍스트로 만든다."""
    if data.startswith(codecs.BOM_UTF8):
        return data.decode("utf-8-sig", errors="replace")
    if data.startswith((codecs.BOM_UTF16_LE, codecs.BOM_UTF16_BE)):
        return data.decode("utf-16", errors="replace")
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return data.decode(_ANSI_ENCODING, errors="replace")


class ImportDialog(QDialog):
    """한 카드로 가져올 파일을 선택한다."""

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setWindowTitle("파일 가져오기")

        self._path_edit = QLineEdit(self)
        self._path_edit.setReadOnly(True)
        browse_button = QPushButton("파일 선택…", self)
        browse_button.clicked.connect(self._choose_file)

        path_layout = QHBoxLayout()
        path_layout.addWidget(self._path_edit)
        path_layout.addWidget(browse_button)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel,
            parent=self,
        )
        buttons.accepted.connect(self._accept_if_valid)
        buttons.rejected.connect(self.reject)

        layout = QVBoxLayout(self)
        layout.addWidget(QLabel("가져올 파일을 선택해 주세요.", self))
        layout.addLayout(path_layout)
        layout.addWidget(buttons)

    @property
    def selected_path(self) -> Path | None:
        """사용자가 선택한 경로를 반환한다."""
        value = self._path_edit.text()
        return None if not value else Path(value)

    @Slot()
    def _choose_file(self) -> None:
        filename, _ = QFileDialog.getOpenFileName(
            self,
            "파일 가져오기",
            "",
            "모든 파일 (*)",
        )
        if filename:
            self._path_edit.setText(filename)

    @Slot()
    def _accept_if_valid(self) -> None:
        path = self.selected_path
        if path is None or not path.is_file():
            QMessageBox.warning(self, "가져오기", "가져올 파일을 선택해 주세요.")
            return
        self.accept()


class ImportWorkerSignals(QObject):
    """가져오기 준비 worker의 완료 신호다."""

    prepared = Signal(object)
    failed = Signal(str)


class ImportPreparationWorker(QRunnable):
    """파일 읽기와 문단 파싱만 작업 스레드에서 수행한다."""

    def __init__(self, request: ImportRequest) -> None:
        super().__init__()
        self._request = request
        self.signals = ImportWorkerSignals()

    @Slot()
    def run(self) -> None:
        """불변 입력만 사용해 가져오기 데이터를 준비한다."""
        try:
            preparation = prepare_import(self._request.path)
        except ValueError as error:
            LOGGER.exception("가져오기 파일 준비에 실패했습니다: %s", self._request.path)
            self.signals.failed.emit(str(error))
            return
        except OSError:
            LOGGER.exception("가져오기 파일 준비에 실패했습니다: %s", self._request.path)
            self.signals.failed.emit(
                "파일을 가져올 수 없습니다. "
                "파일을 읽을 수 있는지, 가져올 내용이 비어 있지 않은지 확인하세요."
            )
            return
        self.signals.prepared.emit(preparation)


class ImportController(QObject):
    """앱 수명 동안 가져오기 준비와 GUI 스레드 DB 확정을 수행한다.

    `imported`·`failed` 는 창별 스트림이 아니라 **앱 전역 관찰 스트림**이다.
    인스턴스가 하나뿐이므로 이 신호만 보는 소비자는 어느 창이 요청했는지
    구분할 수 없다. 요청자와의 상관은 `request_window_id` 를 받는 router
    계층에서만 성립한다.
    """

    imported = Signal(object)
    failed = Signal(str)

    def __init__(
        self,
        service: CardService,
        completion_router: Callable[[str, object], None],
        failure_router: Callable[[str, str], None],
        *,
        parent: QObject | None = None,
        thread_pool: QThreadPool | None = None,
    ) -> None:
        super().__init__(parent)
        self._service = service
        self._completion_router = completion_router
        self._failure_router = failure_router
        self._thread_pool = thread_pool or QThreadPool.globalInstance()
        self._workers: set[ImportPreparationWorker] = set()
        self._shutting_down = False

    def start_import(
        self,
        request_window_id: str,
        document_id: object,
        path: Path,
    ) -> None:
        """고정된 요청 창·문서·경로로 가져오기 준비를 시작한다."""
        if self._shutting_down:
            LOGGER.warning(
                "종료 중인 가져오기 요청을 시작하지 않습니다: window=%s",
                request_window_id,
            )
            return
        try:
            if not isinstance(document_id, str) or not document_id:
                raise ValueError("가져올 활성 문서가 없습니다.")
        except BaseException as error:
            LOGGER.exception("가져오기 대상 문서 확인에 실패했습니다.")
            self._report_failure(request_window_id, str(error))
            return
        request = ImportRequest(
            request_window_id=request_window_id,
            document_id=document_id,
            path=path,
        )
        worker = ImportPreparationWorker(request)
        self._workers.add(worker)
        worker.signals.prepared.connect(
            lambda value, active=worker, captured=request: self._commit_preparation(
                active,
                captured,
                value,
            )
        )
        worker.signals.failed.connect(
            lambda message, active=worker, captured=request: (
                self._report_worker_failure(active, captured, message)
            )
        )
        self._thread_pool.start(worker)

    def begin_shutdown(self) -> None:
        """신규 요청과 아직 DB 확정 전인 준비 결과를 폐기한다."""
        self._shutting_down = True

    def _commit_preparation(
        self,
        worker: ImportPreparationWorker,
        request: ImportRequest,
        value: object,
    ) -> None:
        self._workers.discard(worker)
        if self._shutting_down:
            LOGGER.warning(
                "종료 중인 가져오기 준비 결과를 폐기합니다: window=%s",
                request.request_window_id,
            )
            return
        if not isinstance(value, ImportPreparation):
            self._report_failure(
                request.request_window_id,
                "가져오기 준비 결과 형식이 잘못되었습니다.",
            )
            return
        try:
            cards = self._service.create_cards(
                request.document_id,
                value.text,
                source=CaptureOperationSource.IMPORT,
                split=False,
            )
        except BaseException as error:
            LOGGER.exception("가져온 카드의 DB 저장에 실패했습니다.")
            self._report_failure(request.request_window_id, str(error))
            return
        self._completion_router(request.request_window_id, cards)
        self.imported.emit(cards)

    def _report_worker_failure(
        self,
        worker: ImportPreparationWorker,
        request: ImportRequest,
        message: str,
    ) -> None:
        self._workers.discard(worker)
        if self._shutting_down:
            LOGGER.warning(
                "종료 중인 가져오기 실패 결과를 폐기합니다: window=%s",
                request.request_window_id,
            )
            return
        self._report_failure(request.request_window_id, message)

    def _report_failure(self, request_window_id: str, message: str) -> None:
        self.failed.emit(message)
        self._failure_router(request_window_id, message)
