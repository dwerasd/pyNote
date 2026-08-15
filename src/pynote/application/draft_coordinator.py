from __future__ import annotations

import json
import logging
import time
import uuid
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from enum import StrEnum
from pathlib import Path

from PySide6.QtCore import QObject, QTimer, Signal

from pynote.domain.models import Card, Draft, DraftKind
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories, text_hash

LOGGER = logging.getLogger(__name__)

Clock = Callable[[], int]
IdFactory = Callable[[], str]


class DraftDisposition(StrEnum):
    """잔존 draft에 대해 사용자가 선택할 수 있는 처분이다."""

    RECOVER = "recover"
    DISCARD = "discard"
    LATER = "later"


@dataclass(slots=True)
class DraftSession:
    """확정 카드와 분리해 메모리에서 편집하는 draft 상태다."""

    draft_id: str
    document_id: str
    card_id: str | None
    base_revision_id: str | None
    text: str
    dirty: bool
    cursor_position_qchar: int


@dataclass(frozen=True, slots=True)
class RecoveryCandidate:
    """확정본과 나란히 제시할 잔존 draft 정보다."""

    draft: Draft
    committed_text: str
    committed_revision_id: str | None
    committed_is_newer: bool = False


@dataclass(frozen=True, slots=True)
class DocumentRecoveryPlan:
    """문서 하나에 대해 시작 복구가 표시할 카드와 미룰 카드다."""

    document_id: str
    display_card_id: str
    deferred_card_ids: tuple[str, ...]


def build_recovery_plans(
    candidates: Sequence[RecoveryCandidate],
    *,
    opened_editor_cards: Mapping[str, str | None],
) -> tuple[DocumentRecoveryPlan, ...]:
    """후보 순서를 보존해 문서별 단일 표시 카드와 보류 카드를 정한다."""
    document_cards: dict[str, list[str]] = {}
    seen_cards: dict[str, set[str]] = {}
    for candidate in candidates:
        card_id = candidate.draft.card_id
        if card_id is None:
            continue
        document_id = candidate.draft.document_id
        cards = document_cards.setdefault(document_id, [])
        seen = seen_cards.setdefault(document_id, set())
        if card_id in seen:
            continue
        seen.add(card_id)
        cards.append(card_id)

    plans: list[DocumentRecoveryPlan] = []
    for document_id, cards in document_cards.items():
        opened_card_id = opened_editor_cards.get(document_id)
        display_card_id = (
            opened_card_id
            if opened_card_id is not None and opened_card_id in seen_cards[document_id]
            else cards[0]
        )
        plans.append(
            DocumentRecoveryPlan(
                document_id=document_id,
                display_card_id=display_card_id,
                deferred_card_ids=tuple(
                    card_id for card_id in cards if card_id != display_card_id
                ),
            )
        )
    return tuple(plans)


@dataclass(frozen=True, slots=True)
class DraftWriteMeasurement:
    """draft recovery 쓰기의 관찰된 지연이다."""

    draft_id: str
    text_bytes: int
    elapsed_ms: float


@dataclass(frozen=True, slots=True)
class _DraftSnapshot:
    draft_id: str
    document_id: str
    card_id: str | None
    base_revision_id: str | None
    text: str
    cursor_position_qchar: int


class DraftCoordinator(QObject):
    """DraftSession과 2초 유휴 recovery draft를 관리한다."""

    draft_protected = Signal(str, object, float)
    draft_write_failed = Signal(str, str)

    def __init__(
        self,
        database: Database,
        repositories: Repositories | None = None,
        *,
        idle_ms: int = 2_000,
        clock: Clock | None = None,
        age_clock: Clock | None = None,
        id_factory: IdFactory | None = None,
        emergency_directory: Path | None = None,
        parent: QObject | None = None,
    ) -> None:
        super().__init__(parent)
        if idle_ms < 0:
            raise ValueError("draft 유휴 시간은 0 이상이어야 합니다.")
        self._database = database
        self._repositories = repositories or Repositories(database)
        self._idle_ms = idle_ms
        self._clock = clock or (lambda: time.time_ns() // 1_000)
        self._age_clock = age_clock or (lambda: time.monotonic_ns() // 1_000)
        self._id_factory = id_factory or (lambda: str(uuid.uuid4()))
        self._emergency_directory = (
            emergency_directory
            if emergency_directory is not None
            else database.path.parent / "recovery"
        )
        self._sessions: dict[str, DraftSession] = {}
        self._committed_texts: dict[str, str] = {}
        self._paste_sessions: set[str] = set()
        self._timers: dict[str, QTimer] = {}
        self._composing: set[str] = set()
        self._write_in_progress: set[str] = set()
        self._coalesced_snapshots: dict[str, _DraftSnapshot] = {}
        self._last_protected_age_us: dict[str, int] = {}
        self._last_written_snapshot_keys: dict[str, tuple[str, int]] = {}

    def set_idle_ms(self, idle_ms: int) -> None:
        """새 입력부터 적용할 초안 자동 보호 유휴 시간을 변경한다."""
        if idle_ms < 0:
            raise ValueError("draft 유휴 시간은 0 이상이어야 합니다.")
        self._idle_ms = idle_ms
        for timer in self._timers.values():
            timer.setInterval(idle_ms)

    def open_card(
        self,
        card: Card,
        *,
        disposition: DraftDisposition | None = None,
    ) -> DraftSession | None:
        """카드 확정본 또는 사용자가 선택한 잔존 draft로 편집을 시작한다."""
        existing = self._draft_for_card(card)
        if existing is not None:
            if disposition is DraftDisposition.DISCARD:
                self.discard_draft(existing.id)
            else:
                self._require_valid_draft(existing)
                if disposition is None or disposition is DraftDisposition.LATER:
                    return None
                session = self._session_from_draft(existing, card.body)
                self._sessions[session.draft_id] = session
                self._committed_texts[session.draft_id] = card.body
                return session

        session = DraftSession(
            draft_id=self._id_factory(),
            document_id=card.document_id,
            card_id=card.id,
            base_revision_id=card.current_revision_id,
            text=card.body,
            dirty=False,
            cursor_position_qchar=0,
        )
        self._sessions[session.draft_id] = session
        self._committed_texts[session.draft_id] = card.body
        return session

    def open_new(self, document_id: str) -> DraftSession:
        """문서 입력기의 잔존 new draft 또는 새 빈 세션을 연다."""
        if self._repositories.get_document(document_id) is None:
            raise KeyError(f"존재하지 않는 문서입니다: {document_id}")
        existing_drafts = tuple(
            draft
            for draft in self._repositories.list_drafts(document_id)
            if draft.draft_kind is DraftKind.NEW
        )
        if existing_drafts:
            existing = max(
                existing_drafts,
                key=lambda draft: (draft.updated_at_us, draft.id),
            )
            self._require_valid_draft(existing)
            session = self._session_from_draft(existing, "")
            self._sessions[session.draft_id] = session
            self._committed_texts[session.draft_id] = ""
            LOGGER.info(
                "문서 입력기에 new recovery draft를 복원했습니다: "
                "document=%s draft=%s",
                document_id,
                existing.id,
            )
            if len(existing_drafts) > 1:
                LOGGER.warning(
                    "문서에 new recovery draft가 여러 건 있어 최신 항목을 복원했습니다: "
                    "document=%s count=%d",
                    document_id,
                    len(existing_drafts),
                )
            return session

        session = DraftSession(
            draft_id=self._id_factory(),
            document_id=document_id,
            card_id=None,
            base_revision_id=None,
            text="",
            dirty=False,
            cursor_position_qchar=0,
        )
        self._sessions[session.draft_id] = session
        self._committed_texts[session.draft_id] = ""
        return session

    def session(self, draft_id: str) -> DraftSession:
        """현재 메모리 draft session을 반환한다."""
        try:
            return self._sessions[draft_id]
        except KeyError:
            raise KeyError(f"열려 있지 않은 draft session입니다: {draft_id}") from None

    def update_session(
        self,
        draft_id: str,
        *,
        text: str,
        cursor_position_qchar: int,
        includes_paste: bool | None = None,
    ) -> DraftSession:
        """편집기 스냅샷을 메모리 draft에 반영하고 유휴 보호를 예약한다."""
        if cursor_position_qchar < 0:
            raise ValueError("Qt 커서 위치는 음수일 수 없습니다.")
        session = self.session(draft_id)
        session.text = text
        session.cursor_position_qchar = cursor_position_qchar
        session.dirty = text != self._committed_texts[draft_id]
        if includes_paste is not None:
            if includes_paste:
                self._paste_sessions.add(draft_id)
        if session.dirty:
            checkpoint_us = self._last_protected_age_us.get(draft_id)
            if checkpoint_us is None:
                self._last_protected_age_us[draft_id] = self._age_clock()
            else:
                now_us = self._age_clock()
                max_age_us = self._idle_ms * 1_000
                if now_us - checkpoint_us >= max_age_us:
                    self._protect_safely(draft_id)
            self._restart_idle_timer(session)
        else:
            self._stop_timer(draft_id)
            self._last_protected_age_us.pop(draft_id, None)
        return session

    def set_ime_composing(self, draft_id: str, composing: bool) -> None:
        """IME 조합 중에는 recovery 쓰기를 지연한다."""
        self.session(draft_id)
        if composing:
            self._composing.add(draft_id)
            self._stop_timer(draft_id)
            return
        was_composing = draft_id in self._composing
        self._composing.discard(draft_id)
        if was_composing and self.session(draft_id).dirty:
            self._restart_idle_timer(self.session(draft_id))

    def is_ime_composing(self, draft_id: str) -> bool:
        """해당 session에서 IME 조합이 진행 중인지 반환한다."""
        return draft_id in self._composing

    def includes_paste(self, draft_id: str) -> bool:
        """해당 편집 session에 붙여넣기가 포함됐는지 반환한다."""
        self.session(draft_id)
        return draft_id in self._paste_sessions

    def protect_now(self, draft_id: str) -> DraftWriteMeasurement | None:
        """최신 dirty draft를 즉시 보호하며 재진입 요청은 최신 한 건으로 합친다."""
        session = self.session(draft_id)
        if not session.dirty or draft_id in self._composing:
            return None
        snapshot = self._snapshot(session)
        if draft_id in self._write_in_progress:
            self._coalesced_snapshots[draft_id] = snapshot
            return None

        measurement: DraftWriteMeasurement | None = None
        self._write_in_progress.add(draft_id)
        try:
            pending: _DraftSnapshot | None = snapshot
            while pending is not None:
                pending_hash = text_hash(pending.text)
                pending_key = (pending_hash, pending.cursor_position_qchar)
                # 커서만 바뀐 snapshot도 다시 기록한다 — 복원 위치가 달라진다.
                if self._last_written_snapshot_keys.get(draft_id) == pending_key:
                    self._last_protected_age_us[draft_id] = self._age_clock()
                else:
                    measurement = self._write_snapshot(pending, pending_hash)
                pending = self._coalesced_snapshots.pop(draft_id, None)
        finally:
            self._write_in_progress.remove(draft_id)
        return measurement

    def recovery_candidates(
        self,
        document_id: str | None = None,
    ) -> tuple[RecoveryCandidate, ...]:
        """복구 대화가 필요한 잔존 edit draft를 시작 시 처분 목록으로 반환한다."""
        document_ids = (
            (document_id,)
            if document_id is not None
            else tuple(document.id for document in self._repositories.list_documents())
        )
        candidates: list[RecoveryCandidate] = []
        for current_document_id in document_ids:
            for draft in self._repositories.list_drafts(current_document_id):
                if draft.draft_kind is DraftKind.NEW:
                    continue
                if not self._draft_hash_matches(draft):
                    LOGGER.error(
                        "해시가 일치하지 않는 recovery draft를 복구 목록에서 차단합니다: %s",
                        draft.id,
                    )
                    continue
                card = (
                    None
                    if draft.card_id is None
                    else self._repositories.get_card(draft.card_id)
                )
                revision = (
                    None
                    if card is None or card.current_revision_id is None
                    else self._repositories.get_revision(card.current_revision_id)
                )
                committed_is_newer = (
                    revision is not None
                    and draft.updated_at_us <= revision.created_at_us
                )
                candidates.append(
                    RecoveryCandidate(
                        draft=draft,
                        committed_text="" if card is None else card.body,
                        committed_revision_id=(
                            None if card is None else card.current_revision_id
                        ),
                        committed_is_newer=committed_is_newer,
                    )
                )
        return tuple(candidates)

    def resolve_candidate(
        self,
        draft_id: str,
        disposition: DraftDisposition,
    ) -> DraftSession | None:
        """복구/버리기/나중에 중 선택한 처분을 적용한다."""
        draft = self._repositories.get_draft(draft_id)
        if draft is None:
            raise KeyError(f"존재하지 않는 recovery draft입니다: {draft_id}")
        if disposition is DraftDisposition.LATER:
            return None
        if disposition is DraftDisposition.DISCARD:
            self.discard_draft(draft_id)
            return None
        self._require_valid_draft(draft)
        committed_text = ""
        if draft.card_id is not None:
            card = self._repositories.get_card(draft.card_id)
            if card is None:
                raise KeyError(f"draft의 카드를 찾을 수 없습니다: {draft.card_id}")
            committed_text = card.body
        session = self._session_from_draft(draft, committed_text)
        self._sessions[draft_id] = session
        self._committed_texts[draft_id] = committed_text
        return session

    def discard_session(self, draft_id: str) -> None:
        """메모리 session과 저장된 recovery draft를 함께 버린다."""
        self._stop_timer(draft_id)
        if self._repositories.get_draft(draft_id) is not None:
            self.discard_draft(draft_id)
        self.release_session(draft_id)

    def release_session(self, draft_id: str) -> None:
        """저장된 recovery draft는 남기고 메모리 session과 타이머만 해제한다.

        해제하지 않으면 남은 유휴 타이머가 사용자가 버린 draft를 같은 ID로 다시
        기록한다.
        """
        self._stop_timer(draft_id)
        self._sessions.pop(draft_id, None)
        self._committed_texts.pop(draft_id, None)
        self._paste_sessions.discard(draft_id)
        self._composing.discard(draft_id)
        self._coalesced_snapshots.pop(draft_id, None)
        self._last_protected_age_us.pop(draft_id, None)
        self._last_written_snapshot_keys.pop(draft_id, None)

    def discard_draft(self, draft_id: str) -> None:
        """확인된 recovery draft 한 건을 삭제한다."""
        try:
            with self._database.transaction():
                self._repositories.delete_draft(draft_id)
        except BaseException:
            LOGGER.exception("recovery draft 삭제에 실패했습니다: %s", draft_id)
            raise
        self._last_written_snapshot_keys.pop(draft_id, None)

    def complete_save(
        self,
        draft_id: str,
        *,
        committed_text: str,
        revision_id: str | None,
    ) -> DraftSession:
        """저장 성공 뒤에만 session 기준 확정본과 리비전을 전진시킨다."""
        session = self.session(draft_id)
        self._stop_timer(draft_id)
        session.text = committed_text
        self._committed_texts[draft_id] = committed_text
        session.base_revision_id = revision_id
        session.dirty = False
        self._paste_sessions.discard(draft_id)
        self._last_protected_age_us.pop(draft_id, None)
        self._last_written_snapshot_keys.pop(draft_id, None)
        return session

    def write_emergency_copy(self, draft_id: str) -> Path | None:
        """DB 쓰기 실패 시 설정된 복구 디렉터리에 메모리 draft를 기록한다."""
        session = self.session(draft_id)
        self._emergency_directory.mkdir(parents=True, exist_ok=True)
        path = self._emergency_directory / f"{draft_id}.json"
        payload = {
            "draft_id": session.draft_id,
            "document_id": session.document_id,
            "card_id": session.card_id,
            "base_revision_id": session.base_revision_id,
            "draft_text": session.text,
            "cursor_position_qchar": session.cursor_position_qchar,
            "written_at_us": self._clock(),
        }
        path.write_text(
            json.dumps(payload, ensure_ascii=False, separators=(",", ":")),
            encoding="utf-8",
        )
        return path

    def _draft_for_card(self, card: Card) -> Draft | None:
        return next(
            (
                draft
                for draft in self._repositories.list_drafts(card.document_id)
                if draft.card_id == card.id
            ),
            None,
        )

    @staticmethod
    def _draft_hash_matches(draft: Draft) -> bool:
        return text_hash(draft.draft_text) == draft.draft_hash

    def _require_valid_draft(self, draft: Draft) -> None:
        if self._draft_hash_matches(draft):
            return
        LOGGER.error(
            "해시가 일치하지 않는 recovery draft의 복구·편집 진입을 차단합니다: %s",
            draft.id,
        )
        raise RuntimeError(
            f"recovery draft의 본문 해시가 일치하지 않습니다: {draft.id}"
        )

    def _restart_idle_timer(self, session: DraftSession) -> None:
        timer = self._timers.get(session.draft_id)
        if timer is None:
            timer = QTimer(self)
            timer.setSingleShot(True)
            timer.timeout.connect(
                lambda draft_id=session.draft_id: self._protect_safely(draft_id)
            )
            self._timers[session.draft_id] = timer
        timer.start(self._idle_ms)

    def _stop_timer(self, draft_id: str) -> None:
        timer = self._timers.get(draft_id)
        if timer is not None:
            timer.stop()

    def _protect_safely(self, draft_id: str) -> None:
        try:
            self.protect_now(draft_id)
        except BaseException:
            # _write_snapshot이 이미 로깅하고 UI 오류 신호와 비상 복구 경로를 처리한다.
            return

    @staticmethod
    def _snapshot(session: DraftSession) -> _DraftSnapshot:
        return _DraftSnapshot(
            draft_id=session.draft_id,
            document_id=session.document_id,
            card_id=session.card_id,
            base_revision_id=session.base_revision_id,
            text=session.text,
            cursor_position_qchar=session.cursor_position_qchar,
        )

    def _write_snapshot(
        self,
        snapshot: _DraftSnapshot,
        draft_hash: str,
    ) -> DraftWriteMeasurement:
        started_ns = time.perf_counter_ns()
        draft = Draft(
            id=snapshot.draft_id,
            document_id=snapshot.document_id,
            card_id=snapshot.card_id,
            draft_kind=DraftKind.EDIT if snapshot.card_id is not None else DraftKind.NEW,
            base_revision_id=snapshot.base_revision_id,
            draft_text=snapshot.text,
            draft_hash=draft_hash,
            cursor_position_qchar=snapshot.cursor_position_qchar,
            updated_at_us=self._clock(),
        )
        try:
            with self._database.transaction():
                if self._repositories.get_draft(draft.id) is None:
                    self._repositories.create_draft(draft)
                else:
                    self._repositories.update_draft(draft)
        except BaseException as error:
            LOGGER.exception("recovery draft 저장에 실패했습니다: %s", draft.id)
            try:
                self.write_emergency_copy(draft.id)
            except BaseException:
                LOGGER.exception("비상 recovery 파일 저장에도 실패했습니다: %s", draft.id)
            self.draft_write_failed.emit(draft.id, str(error))
            raise

        self._last_written_snapshot_keys[draft.id] = (
            draft.draft_hash,
            draft.cursor_position_qchar,
        )
        self._last_protected_age_us[draft.id] = self._age_clock()
        elapsed_ms = (time.perf_counter_ns() - started_ns) / 1_000_000
        measurement = DraftWriteMeasurement(
            draft_id=draft.id,
            text_bytes=len(snapshot.text.encode("utf-8")),
            elapsed_ms=elapsed_ms,
        )
        LOGGER.info(
            "draft recovery 쓰기 지연: draft=%s bytes=%d elapsed_ms=%.3f",
            measurement.draft_id,
            measurement.text_bytes,
            measurement.elapsed_ms,
        )
        self.draft_protected.emit(
            measurement.draft_id,
            draft.updated_at_us,
            measurement.elapsed_ms,
        )
        return measurement

    @staticmethod
    def _session_from_draft(draft: Draft, committed_text: str) -> DraftSession:
        return DraftSession(
            draft_id=draft.id,
            document_id=draft.document_id,
            card_id=draft.card_id,
            base_revision_id=draft.base_revision_id,
            text=draft.draft_text,
            dirty=draft.draft_text != committed_text,
            cursor_position_qchar=draft.cursor_position_qchar,
        )
