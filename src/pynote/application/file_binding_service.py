from __future__ import annotations

import codecs
import hashlib
import locale
import logging
import os
import sys
import tempfile
import time
from collections.abc import Callable
from dataclasses import dataclass, replace
from enum import StrEnum
from pathlib import Path

from pynote.domain.models import Card, FileBinding, NewlineKind
from pynote.infrastructure.repositories import Repositories

LOGGER = logging.getLogger(__name__)

Clock = Callable[[], int]

# ui.import_dialog._ANSI_ENCODING 과 같은 규칙이다. application 층은 ui 를 import 하지 않으므로
# 상수를 여기에 다시 둔다 — 한쪽 규칙을 바꾸면 다른 쪽도 함께 바꾼다.
_ANSI_ENCODING = "mbcs" if sys.platform == "win32" else locale.getpreferredencoding(False)

# BOM 을 함께 기록하는 인코딩과 그 BOM 바이트열이다.
_BOM_BY_ENCODING = {
    "utf-8": codecs.BOM_UTF8,
    "utf-16-le": codecs.BOM_UTF16_LE,
    "utf-16-be": codecs.BOM_UTF16_BE,
}

# 결속을 막는 제어 문자에서 제외할 공백류다.
_ALLOWED_CONTROL_CHARS = frozenset("\t\n\r\f\v")

# QPlainTextEdit 이 편집기 왕복에서 치환하는 문자다(실측 PySide6 6.9.1 offscreen:
# U+00A0 은 U+0020 으로, 나머지 넷은 개행으로 바뀐다). 되쓰기가 원본을 파괴한다.
# CEILING: 왕복 위험 문자 파일은 결속 거부 — 허용하려면 편집기 치환을 우회하는 표현 보존층이 필요
_ROUNDTRIP_HAZARDS = frozenset("   ﷐﷑")


class FileSyncOutcome(StrEnum):
    """결속 파일 되쓰기 한 번의 결과 종류다."""

    NOOP = "noop"
    WRITTEN = "written"
    EXTERNAL_CHANGE = "external_change"
    FAILED = "failed"


class BindingPathStatus(StrEnum):
    """새 결속을 만들기 전 경로 점유 판정 결과다."""

    FREE = "free"
    HELD_BY_ACTIVE_CARD = "held_by_active_card"


@dataclass(frozen=True, slots=True)
class DetectedText:
    """결속 가능한 파일에서 읽어낸 본문과 되쓰기 형식이다."""

    text: str
    encoding: str
    bom: bool
    newline: NewlineKind
    trailing_newline: bool


@dataclass(frozen=True, slots=True)
class PendingFileBinding:
    """카드가 아직 없는 동안 들고 있는 결속 예약이다."""

    path: str
    encoding: str
    bom: bool
    newline: NewlineKind
    trailing_newline: bool

    @property
    def path_key(self) -> str:
        return os.path.normcase(self.path)


@dataclass(frozen=True, slots=True)
class FileSyncResult:
    """되쓰기 결과와 실패 사유를 함께 전달한다."""

    outcome: FileSyncOutcome
    error: str | None = None


@dataclass(frozen=True, slots=True)
class BindingPathResolution:
    """경로 점유 해소 뒤의 상태와 점유 카드다."""

    status: BindingPathStatus
    holder_card_id: str | None = None


def has_control_chars(text: str) -> bool:
    """되쓰기를 막는 제어 문자가 있는지 판정한다."""
    return any(
        (character < "\x20" or character == "\x7f") and character not in _ALLOWED_CONTROL_CHARS
        for character in text
    )


def has_roundtrip_hazard(text: str) -> bool:
    """편집기 왕복에서 치환되는 문자가 있는지 판정한다."""
    return any(character in _ROUNDTRIP_HAZARDS for character in text)


def detect_text(data: bytes) -> DetectedText | None:
    """파일 바이트에서 카드 본문과 되쓰기 형식을 읽어낸다. 결속 불가면 None 이다."""
    decoded = _decode_strict(data)
    if decoded is None:
        return None
    text, encoding, bom = decoded
    if has_control_chars(text) or has_roundtrip_hazard(text):
        return None
    return DetectedText(
        text=text.replace("\r\n", "\n").replace("\r", "\n"),
        encoding=encoding,
        bom=bom,
        newline=_detect_newline(text),
        trailing_newline=text.endswith(("\n", "\r")),
    )


def render_bytes(text: str, binding: FileBinding) -> bytes:
    """카드 본문을 결속 형식 그대로의 파일 바이트로 만든다."""
    body = text.replace("\n", binding.newline.characters).encode(binding.encoding)
    if not binding.bom:
        return body
    prefix = _BOM_BY_ENCODING.get(binding.encoding)
    if prefix is None:
        raise ValueError(f"BOM을 기록할 수 없는 인코딩입니다: {binding.encoding}")
    return prefix + body


def resolve_path(path: Path | str) -> tuple[str, str]:
    """절대 경로 문자열과 대소문자 무시 비교용 키를 만든다."""
    resolved = str(Path(path).resolve())
    return resolved, os.path.normcase(resolved)


def hash_bytes(data: bytes) -> str:
    """파일 바이트의 sha256 hex 를 반환한다."""
    return hashlib.sha256(data).hexdigest()


def read_file_hash(path: Path) -> str | None:
    """파일이 있으면 현재 바이트의 sha256 hex, 없으면 None 을 반환한다."""
    data = _read_bytes(path)
    return None if data is None else hash_bytes(data)


def prepare_binding_path(
    repositories: Repositories,
    path_key: str,
) -> BindingPathResolution:
    """새 결속을 만들기 전 경로 점유를 해소한다. 휴지통 카드의 행은 지운다."""
    existing = repositories.find_binding_by_path(path_key)
    if existing is None:
        return BindingPathResolution(BindingPathStatus.FREE)
    holder = repositories.get_card(existing.card_id)
    if holder is not None and holder.deleted_at_us is None:
        return BindingPathResolution(
            BindingPathStatus.HELD_BY_ACTIVE_CARD,
            holder_card_id=existing.card_id,
        )
    repositories.delete_file_binding(existing.card_id)
    return BindingPathResolution(BindingPathStatus.FREE)


def sync_file(
    repositories: Repositories,
    card: Card,
    *,
    force: bool = False,
    interactive: bool = False,
    clock: Clock | None = None,
) -> FileSyncResult:
    """DB 확정이 끝난 카드 본문을 결속 파일에 되쓴다."""
    binding = repositories.get_file_binding(card.id)
    if binding is None:
        return FileSyncResult(FileSyncOutcome.NOOP)

    path = Path(binding.path)
    try:
        rendered = render_bytes(card.body, binding)
    except (UnicodeEncodeError, LookupError, ValueError) as error:
        LOGGER.warning("결속 파일 인코딩에 실패했습니다: %s", binding.path)
        return FileSyncResult(FileSyncOutcome.FAILED, str(error))

    try:
        current = _read_bytes(path)
    except OSError as error:
        LOGGER.warning("결속 파일을 읽지 못했습니다: %s", binding.path)
        return FileSyncResult(FileSyncOutcome.FAILED, str(error))

    if current is not None:
        if current == rendered:
            _record_sync(repositories, binding, path, rendered, clock)
            return FileSyncResult(FileSyncOutcome.NOOP)
        if (
            not force
            and binding.synced_hash is not None
            and hash_bytes(current) != binding.synced_hash
        ):
            _log_external_change(binding.path, interactive=interactive)
            return FileSyncResult(FileSyncOutcome.EXTERNAL_CHANGE)

    try:
        _atomic_write(path, rendered)
    except OSError as error:
        LOGGER.warning("결속 파일 기록에 실패했습니다: %s", binding.path)
        return FileSyncResult(FileSyncOutcome.FAILED, str(error))

    _record_sync(repositories, binding, path, rendered, clock)
    return FileSyncResult(FileSyncOutcome.WRITTEN)


def _decode_strict(data: bytes) -> tuple[str, str, bool] | None:
    """가져오기와 같은 순서로, 다만 치환 없이 디코딩한다.

    BOM 이 있으면 그 인코딩 하나만 시도하고 실패 시 결속 불가로 판정한다 — 뒤 단계로 내려가면
    BOM 바이트가 ANSI 문자로 본문에 섞여 편집 결과가 오염된다.
    """
    if data.startswith(codecs.BOM_UTF8):
        return _try_decode(data[len(codecs.BOM_UTF8) :], "utf-8", bom=True)
    if data.startswith(codecs.BOM_UTF16_LE):
        return _try_decode(data[len(codecs.BOM_UTF16_LE) :], "utf-16-le", bom=True)
    if data.startswith(codecs.BOM_UTF16_BE):
        return _try_decode(data[len(codecs.BOM_UTF16_BE) :], "utf-16-be", bom=True)
    return _try_decode(data, "utf-8", bom=False) or _try_decode(data, _ANSI_ENCODING, bom=False)


def _try_decode(data: bytes, encoding: str, *, bom: bool) -> tuple[str, str, bool] | None:
    try:
        return data.decode(encoding), encoding, bom
    except (UnicodeDecodeError, LookupError):
        return None


def _detect_newline(text: str) -> NewlineKind:
    """가장 먼저 나타나는 줄끝을 채택한다. 줄끝이 없으면 플랫폼 기본값이다."""
    # CEILING: 혼합 줄끝은 첫 줄끝 하나로 수렴한다 — 보존이 필요하면 줄별 줄끝 맵을 결속에 추가
    carriage = text.find("\r")
    linefeed = text.find("\n")
    if carriage < 0 and linefeed < 0:
        return NewlineKind.CRLF if sys.platform == "win32" else NewlineKind.LF
    if carriage < 0 or (linefeed >= 0 and linefeed < carriage):
        return NewlineKind.LF
    return NewlineKind.CRLF if text.startswith("\n", carriage + 1) else NewlineKind.CR


def _log_external_change(path: str, *, interactive: bool) -> None:
    """대화형 저장은 질의로 이어지고, 그 밖의 경로는 경고만 남긴다."""
    if interactive:
        LOGGER.info("결속 파일이 외부에서 변경되어 확인이 필요합니다: %s", path)
    else:
        LOGGER.warning("파일이 외부에서 변경되어 되쓰지 않았습니다: %s", path)


def _read_bytes(path: Path) -> bytes | None:
    try:
        return path.read_bytes()
    except FileNotFoundError:
        return None


def _atomic_write(path: Path, data: bytes) -> None:
    """같은 디렉터리의 임시 파일에 쓰고 os.replace 로 교체한다."""
    # CEILING: os.replace 는 파일 신원(하드링크·명시 ACL·대체 스트림)을 교체한다 — 보존이 필요하면
    # 제자리 쓰기 + 사전 백업본으로 전환
    descriptor, temporary_name = tempfile.mkstemp(
        dir=path.parent,
        prefix=f".{path.name}.",
        suffix=".tmp",
    )
    temporary_path = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
        os.replace(temporary_path, path)
    except BaseException:
        try:
            temporary_path.unlink(missing_ok=True)
        except OSError:
            LOGGER.exception("결속 임시 파일 정리에 실패했습니다: %s", temporary_path)
        raise


def _record_sync(
    repositories: Repositories,
    binding: FileBinding,
    path: Path,
    data: bytes,
    clock: Clock | None,
) -> None:
    """기록이 끝난 뒤에만 마지막 동기 지문을 갱신한다.

    stat 실패는 지문을 남기지 않는 쪽으로 처리한다 — 다음 저장이 (d) 분기로 떨어져 다시 기록하므로
    안전한 방향이고, 파일 계층의 예외가 저장 흐름을 관통하지 않는다.
    """
    try:
        stat_result = path.stat()
    except OSError:
        LOGGER.warning("결속 파일 상태를 읽지 못해 동기 지문을 남기지 않습니다: %s", binding.path)
        return
    repositories.upsert_file_binding(
        replace(
            binding,
            synced_size=stat_result.st_size,
            synced_mtime_ns=stat_result.st_mtime_ns,
            synced_hash=hash_bytes(data),
            synced_at_us=(clock or _default_clock)(),
        )
    )


def _default_clock() -> int:
    return time.time_ns() // 1_000
