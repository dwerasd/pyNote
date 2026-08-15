from __future__ import annotations

import logging
import sqlite3
from collections.abc import Iterator
from pathlib import Path

import pytest
from pytestqt.qtbot import QtBot

from pynote.application import draft_coordinator as draft_coordinator_module
from pynote.application.draft_coordinator import (
    DraftCoordinator,
    DraftDisposition,
    RecoveryCandidate,
)
from pynote.application.save_coordinator import SaveCoordinator, SaveOutcome
from pynote.domain.events import EventSource
from pynote.domain.models import (
    CaptureOperationSource,
    Card,
    CardSource,
    Document,
    Draft,
    DraftKind,
    NewCaptureOperation,
    NewCard,
    RevisionSource,
    SplitPolicy,
)
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories


def _create_card(repositories: Repositories, number: int = 1) -> Card:
    if repositories.get_document("document-1") is None:
        repositories.create_document(
            Document(
                id="document-1",
                title="초안 테스트",
                created_at_us=1_000,
                updated_at_us=1_000,
            )
        )
    return repositories.create_cards(
        NewCaptureOperation(
            id=f"operation-{number}",
            document_id="document-1",
            source=CaptureOperationSource.TYPING,
            split_policy=SplitPolicy.KEEP,
            original_text=None,
            created_at_us=2_000 + number,
        ),
        [
            NewCard(
                id=f"card-{number}",
                revision_id=f"revision-{number}",
                event_id=f"event-{number}",
                position_key=number * 1_024,
                body=f"확정 본문 {number}",
                card_source=CardSource.TYPING,
                event_source=EventSource.TYPING,
                revision_source=RevisionSource.EDIT,
                created_at_us=2_000 + number,
            )
        ],
    )[0]


def _ids(prefix: str) -> Iterator[str]:
    number = 0
    while True:
        number += 1
        yield f"{prefix}-{number}"


def _recovery_candidate(
    document_id: str,
    card_id: str | None,
    *,
    draft_id: str,
) -> RecoveryCandidate:
    text = f"draft text {draft_id}"
    return RecoveryCandidate(
        draft=Draft(
            id=draft_id,
            document_id=document_id,
            card_id=card_id,
            draft_kind=DraftKind.NEW if card_id is None else DraftKind.EDIT,
            base_revision_id=None,
            draft_text=text,
            draft_hash=draft_coordinator_module.text_hash(text),
            cursor_position_qchar=2,
            updated_at_us=10,
        ),
        committed_text="committed",
        committed_revision_id=None,
    )


def test_build_recovery_plans_preserves_order_and_opened_nonfirst_candidate() -> None:
    candidates = (
        _recovery_candidate("document-b", "card-b1", draft_id="draft-b1"),
        _recovery_candidate("document-a", "card-a1", draft_id="draft-a1"),
        _recovery_candidate("document-b", "card-b2", draft_id="draft-b2"),
        _recovery_candidate("document-b", "card-b1", draft_id="draft-b1-copy"),
        _recovery_candidate("document-a", None, draft_id="draft-new"),
        _recovery_candidate("document-a", "card-a2", draft_id="draft-a2"),
    )

    plans = draft_coordinator_module.build_recovery_plans(
        candidates,
        opened_editor_cards={
            "document-b": "card-b2",
            "document-a": "not-a-candidate",
        },
    )

    assert [
        (plan.document_id, plan.display_card_id, plan.deferred_card_ids)
        for plan in plans
    ] == [
        ("document-b", "card-b2", ("card-b1",)),
        ("document-a", "card-a1", ("card-a2",)),
    ]


@pytest.mark.parametrize(
    ("draft_updated_at_us", "committed_is_newer"),
    [(2_002, False), (2_001, True), (2_000, True)],
)
def test_recovery_candidates_keep_all_timestamp_relations(
    draft_updated_at_us: int,
    committed_is_newer: bool,
    database: Database,
    repositories: Repositories,
) -> None:
    card = _create_card(repositories)
    text = "시각과 무관하게 처분할 초안"
    repositories.create_draft(
        Draft(
            id="draft-time-relation",
            document_id=card.document_id,
            card_id=card.id,
            draft_kind=DraftKind.EDIT,
            base_revision_id=card.current_revision_id,
            draft_text=text,
            draft_hash=draft_coordinator_module.text_hash(text),
            cursor_position_qchar=4,
            updated_at_us=draft_updated_at_us,
        )
    )

    candidates = DraftCoordinator(database, repositories).recovery_candidates()

    assert len(candidates) == 1
    assert candidates[0].draft.id == "draft-time-relation"
    assert candidates[0].committed_is_newer is committed_is_newer


def test_recovery_candidates_cover_revisionless_and_exclude_new_draft(
    database: Database,
    repositories: Repositories,
) -> None:
    card = _create_card(repositories)
    database.connection.execute(
        "UPDATE cards SET current_revision_id = NULL WHERE id = ?",
        (card.id,),
    )
    edit_text = "리비전 없는 카드 초안"
    new_text = "새 카드 입력기 초안"
    repositories.create_draft(
        Draft(
            id="draft-revisionless",
            document_id=card.document_id,
            card_id=card.id,
            draft_kind=DraftKind.EDIT,
            base_revision_id=None,
            draft_text=edit_text,
            draft_hash=draft_coordinator_module.text_hash(edit_text),
            cursor_position_qchar=3,
            updated_at_us=1,
        )
    )
    repositories.create_draft(
        Draft(
            id="draft-new-card",
            document_id=card.document_id,
            card_id=None,
            draft_kind=DraftKind.NEW,
            base_revision_id=None,
            draft_text=new_text,
            draft_hash=draft_coordinator_module.text_hash(new_text),
            cursor_position_qchar=2,
            updated_at_us=2,
        )
    )

    stored_new_before = repositories.get_draft("draft-new-card")
    candidates = DraftCoordinator(database, repositories).recovery_candidates()
    stored_new_after = repositories.get_draft("draft-new-card")

    assert [candidate.draft.id for candidate in candidates] == [
        "draft-revisionless"
    ]
    assert not candidates[0].committed_is_newer
    assert stored_new_before is not None
    assert stored_new_after == stored_new_before


def test_idle_recovery_waits_for_ime_commit(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
) -> None:
    card = _create_card(repositories)
    coordinator = DraftCoordinator(
        database,
        repositories,
        idle_ms=10,
        clock=lambda: 5_000,
        id_factory=lambda: "draft-ime",
    )
    session = coordinator.open_card(card)
    assert session is not None
    coordinator.update_session(
        session.draft_id,
        text="ㅎ",
        cursor_position_qchar=1,
    )
    coordinator.set_ime_composing(session.draft_id, True)

    qtbot.wait(30)

    assert repositories.get_draft(session.draft_id) is None
    coordinator.set_ime_composing(session.draft_id, False)
    qtbot.waitUntil(lambda: repositories.get_draft(session.draft_id) is not None)
    stored = repositories.get_draft(session.draft_id)
    assert stored is not None
    assert stored.draft_text == "ㅎ"


def test_continuous_updates_force_draft_protection_at_max_age(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
) -> None:
    card = _create_card(repositories)
    wall_us = [500_000]
    age_us = [0]
    coordinator = DraftCoordinator(
        database,
        repositories,
        clock=lambda: wall_us[0],
        age_clock=lambda: age_us[0],
        id_factory=lambda: "draft-max-age",
    )
    coordinator.set_idle_ms(100)
    session = coordinator.open_card(card)
    assert session is not None
    revision_count = len(repositories.list_revisions(card.id))

    for elapsed_us, text in (
        (0, "연속 입력 1"),
        (40_000, "연속 입력 12"),
        (80_000, "연속 입력 123"),
    ):
        wall_us[0] = 500_000 - elapsed_us
        age_us[0] = elapsed_us
        coordinator.update_session(
            session.draft_id,
            text=text,
            cursor_position_qchar=len(text),
        )
        assert repositories.get_draft(session.draft_id) is None

    wall_us[0] = 380_000
    age_us[0] = 120_000
    coordinator.update_session(
        session.draft_id,
        text="연속 입력 1234",
        cursor_position_qchar=10,
    )

    stored = repositories.get_draft(session.draft_id)
    assert stored is not None
    assert stored.draft_text == "연속 입력 1234"
    assert stored.updated_at_us == 380_000
    assert len(repositories.list_revisions(card.id)) == revision_count


def test_first_dirty_input_anchors_age_after_long_clean_session(
    database: Database,
    repositories: Repositories,
) -> None:
    card = _create_card(repositories)
    age_us = [0]
    coordinator = DraftCoordinator(
        database,
        repositories,
        idle_ms=100,
        clock=lambda: 7_000,
        age_clock=lambda: age_us[0],
        id_factory=lambda: "draft-first-dirty-anchor",
    )
    session = coordinator.open_card(card)
    assert session is not None

    age_us[0] = 10_000_000
    coordinator.update_session(
        session.draft_id,
        text="오래 열린 뒤 첫 입력",
        cursor_position_qchar=11,
    )

    assert repositories.get_draft(session.draft_id) is None


def test_dirty_clean_dirty_resets_max_age_interval(
    database: Database,
    repositories: Repositories,
) -> None:
    card = _create_card(repositories)
    age_us = [0]
    coordinator = DraftCoordinator(
        database,
        repositories,
        idle_ms=100,
        clock=lambda: 8_000,
        age_clock=lambda: age_us[0],
        id_factory=lambda: "draft-dirty-reset",
    )
    session = coordinator.open_card(card)
    assert session is not None
    coordinator.update_session(
        session.draft_id,
        text="첫 dirty 구간",
        cursor_position_qchar=8,
    )
    age_us[0] = 90_000
    coordinator.update_session(
        session.draft_id,
        text=card.body,
        cursor_position_qchar=len(card.body),
    )

    age_us[0] = 1_000_000
    coordinator.update_session(
        session.draft_id,
        text="둘째 dirty 구간",
        cursor_position_qchar=9,
    )
    age_us[0] = 1_099_999
    coordinator.update_session(
        session.draft_id,
        text="둘째 dirty 구간 2",
        cursor_position_qchar=11,
    )
    assert repositories.get_draft(session.draft_id) is None

    age_us[0] = 1_100_000
    coordinator.update_session(
        session.draft_id,
        text="둘째 dirty 구간 3",
        cursor_position_qchar=11,
    )

    stored = repositories.get_draft(session.draft_id)
    assert stored is not None
    assert stored.draft_text == "둘째 dirty 구간 3"


def test_duplicate_protection_writes_once_and_rewrites_after_discard(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    card = _create_card(repositories)
    coordinator = DraftCoordinator(
        database,
        repositories,
        idle_ms=10,
        clock=lambda: 5_000,
        id_factory=lambda: "draft-deduplicated",
    )
    protected_ids: list[str] = []
    coordinator.draft_protected.connect(
        lambda draft_id, _updated_at_us, _elapsed_ms: protected_ids.append(draft_id)
    )
    session = coordinator.open_card(card)
    assert session is not None
    revision_count = len(repositories.list_revisions(card.id))
    coordinator.update_session(
        session.draft_id,
        text="한 번만 기록할 본문",
        cursor_position_qchar=5,
    )
    timer_calls: list[str] = []
    protect_safely = coordinator._protect_safely

    def observe_timer_protection(draft_id: str) -> None:
        timer_calls.append(draft_id)
        protect_safely(draft_id)

    monkeypatch.setattr(coordinator, "_protect_safely", observe_timer_protection)

    coordinator.protect_now(session.draft_id)
    coordinator.protect_now(session.draft_id)
    coordinator.protect_now(session.draft_id)
    qtbot.waitUntil(lambda: bool(timer_calls))

    assert timer_calls == [session.draft_id]
    assert protected_ids == [session.draft_id]
    coordinator.discard_draft(session.draft_id)
    assert repositories.get_draft(session.draft_id) is None

    coordinator.protect_now(session.draft_id)

    stored = repositories.get_draft(session.draft_id)
    assert stored is not None
    assert stored.draft_text == "한 번만 기록할 본문"
    assert protected_ids == [session.draft_id, session.draft_id]
    assert len(repositories.list_revisions(card.id)) == revision_count


def test_single_protection_hashes_body_once(
    database: Database,
    repositories: Repositories,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    card = _create_card(repositories)
    coordinator = DraftCoordinator(
        database,
        repositories,
        id_factory=lambda: "draft-single-hash",
    )
    session = coordinator.open_card(card)
    assert session is not None
    text = "한 번만 해시할 본문"
    coordinator.update_session(
        session.draft_id,
        text=text,
        cursor_position_qchar=10,
    )
    hash_calls: list[str] = []
    text_hash = draft_coordinator_module.text_hash

    def observe_text_hash(value: str) -> str:
        hash_calls.append(value)
        return text_hash(value)

    monkeypatch.setattr(draft_coordinator_module, "text_hash", observe_text_hash)

    coordinator.protect_now(session.draft_id)

    assert hash_calls == [text]


def test_save_coordinator_invalidates_deduplication_after_unchanged_save(
    database: Database,
    repositories: Repositories,
) -> None:
    card = _create_card(repositories)
    coordinator = DraftCoordinator(
        database,
        repositories,
        clock=lambda: 5_000,
        age_clock=lambda: 6_000,
        id_factory=lambda: "draft-save-invalidation",
    )
    session = coordinator.open_card(card)
    assert session is not None
    protected_text = "저장 전에 보호한 본문"
    coordinator.update_session(
        session.draft_id,
        text=protected_text,
        cursor_position_qchar=10,
    )
    coordinator.protect_now(session.draft_id)
    coordinator.update_session(
        session.draft_id,
        text=card.body,
        cursor_position_qchar=len(card.body),
    )
    saver = SaveCoordinator(
        database,
        coordinator,
        repositories,
        clock=lambda: 7_000,
    )

    result = saver.save(session)

    assert result.outcome is SaveOutcome.UNCHANGED
    assert repositories.get_draft(session.draft_id) is None
    coordinator.update_session(
        session.draft_id,
        text=protected_text,
        cursor_position_qchar=10,
    )
    coordinator.protect_now(session.draft_id)
    stored = repositories.get_draft(session.draft_id)
    assert stored is not None
    assert stored.draft_text == protected_text


def test_release_session_invalidates_deduplication_for_reused_id(
    database: Database,
    repositories: Repositories,
) -> None:
    card = _create_card(repositories)
    coordinator = DraftCoordinator(
        database,
        repositories,
        clock=lambda: 5_000,
        age_clock=lambda: 6_000,
        id_factory=lambda: "draft-released-id",
    )
    session = coordinator.open_card(card)
    assert session is not None
    protected_text = "해제 전에 보호한 본문"
    coordinator.update_session(
        session.draft_id,
        text=protected_text,
        cursor_position_qchar=10,
    )
    coordinator.protect_now(session.draft_id)

    coordinator.release_session(session.draft_id)
    with database.transaction():
        repositories.delete_draft(session.draft_id)
    reopened = coordinator.open_card(card)
    assert reopened is not None
    assert reopened.draft_id == session.draft_id
    coordinator.update_session(
        reopened.draft_id,
        text=protected_text,
        cursor_position_qchar=10,
    )
    coordinator.protect_now(reopened.draft_id)

    stored = repositories.get_draft(reopened.draft_id)
    assert stored is not None
    assert stored.draft_text == protected_text


def test_max_age_write_failure_is_swallowed_and_next_input_retries(
    database: Database,
    repositories: Repositories,
    qtbot: QtBot,
) -> None:
    card = _create_card(repositories)
    now_us = [0]
    coordinator = DraftCoordinator(
        database,
        repositories,
        idle_ms=100,
        clock=lambda: 5_000,
        age_clock=lambda: now_us[0],
        id_factory=lambda: "draft-max-age-retry",
    )
    failed_ids: list[str] = []
    coordinator.draft_write_failed.connect(
        lambda draft_id, _message: failed_ids.append(draft_id)
    )
    session = coordinator.open_card(card)
    assert session is not None
    coordinator.update_session(
        session.draft_id,
        text="실패 전 본문",
        cursor_position_qchar=6,
    )
    database.connection.execute(
        """
        CREATE TRIGGER fail_max_age_draft_insert
        BEFORE INSERT ON drafts
        BEGIN
            SELECT RAISE(ABORT, '의도한 max age 실패');
        END
        """
    )

    now_us[0] = 100_000
    coordinator.update_session(
        session.draft_id,
        text="첫 보호 실패 본문",
        cursor_position_qchar=8,
    )

    assert failed_ids == [session.draft_id]
    assert repositories.get_draft(session.draft_id) is None
    database.connection.execute("DROP TRIGGER fail_max_age_draft_insert")
    now_us[0] = 110_000
    coordinator.update_session(
        session.draft_id,
        text="다음 입력 재시도 본문",
        cursor_position_qchar=9,
    )

    stored = repositories.get_draft(session.draft_id)
    assert stored is not None
    assert stored.draft_text == "다음 입력 재시도 본문"


def test_restart_recovery_supports_recover_discard_and_later(
    database_path: Path,
) -> None:
    first_database = Database(database_path)
    first_repositories = Repositories(first_database)
    cards = tuple(_create_card(first_repositories, number) for number in range(1, 4))
    draft_ids = iter(("draft-recover", "draft-discard", "draft-later"))
    first = DraftCoordinator(
        first_database,
        first_repositories,
        clock=lambda: 9_000,
        id_factory=lambda: next(draft_ids),
    )
    for card in cards:
        session = first.open_card(card)
        assert session is not None
        first.update_session(
            session.draft_id,
            text=f"{card.body} + 미저장",
            cursor_position_qchar=4,
        )
        first.protect_now(session.draft_id)
    first_database.close()

    restarted_database = Database(database_path)
    try:
        restarted_repositories = Repositories(restarted_database)
        restarted = DraftCoordinator(restarted_database, restarted_repositories)
        candidates = restarted.recovery_candidates()
        assert {candidate.draft.id for candidate in candidates} == {
            "draft-recover",
            "draft-discard",
            "draft-later",
        }

        recovered = restarted.resolve_candidate(
            "draft-recover",
            DraftDisposition.RECOVER,
        )
        assert recovered is not None
        assert recovered.text == "확정 본문 1 + 미저장"

        assert (
            restarted.resolve_candidate("draft-discard", DraftDisposition.DISCARD)
            is None
        )
        assert restarted_repositories.get_draft("draft-discard") is None

        assert (
            restarted.resolve_candidate("draft-later", DraftDisposition.LATER)
            is None
        )
        assert restarted_repositories.get_draft("draft-later") is not None
        assert restarted.open_card(cards[2]) is None
    finally:
        restarted_database.close()


def test_corrupted_draft_is_logged_and_blocked_from_listing_and_recovery(
    database: Database,
    repositories: Repositories,
    caplog: pytest.LogCaptureFixture,
) -> None:
    card = _create_card(repositories)
    coordinator = DraftCoordinator(
        database,
        repositories,
        clock=lambda: 9_000,
        id_factory=lambda: "draft-corrupted",
    )
    session = coordinator.open_card(card)
    assert session is not None
    coordinator.update_session(
        session.draft_id,
        text="정상 초안",
        cursor_position_qchar=4,
    )
    coordinator.protect_now(session.draft_id)
    database.connection.execute(
        "UPDATE drafts SET draft_text = ? WHERE id = ?",
        ("손상된 초안", session.draft_id),
    )
    restarted = DraftCoordinator(database, repositories)

    with caplog.at_level(logging.ERROR):
        assert restarted.recovery_candidates() == ()
        with pytest.raises(RuntimeError, match="본문 해시"):
            restarted.resolve_candidate(
                session.draft_id,
                DraftDisposition.RECOVER,
            )
        with pytest.raises(RuntimeError, match="본문 해시"):
            restarted.open_card(card, disposition=DraftDisposition.RECOVER)

    messages = [record.message for record in caplog.records]
    assert any("복구 목록에서 차단" in message for message in messages)
    assert any("복구·편집 진입을 차단" in message for message in messages)


class _ReentrantRepositories(Repositories):
    coordinator: DraftCoordinator | None = None
    reentered = False

    def create_draft(self, draft: object) -> None:
        super().create_draft(draft)  # pyright: ignore[reportArgumentType]
        if self.reentered or self.coordinator is None:
            return
        self.reentered = True
        self.coordinator.update_session(
            "draft-coalesced",
            text="가장 최신 스냅샷",
            cursor_position_qchar=9,
        )
        assert self.coordinator.protect_now("draft-coalesced") is None


def test_reentrant_write_coalesces_to_latest_snapshot(database: Database) -> None:
    repositories = _ReentrantRepositories(database)
    card = _create_card(repositories)
    coordinator = DraftCoordinator(
        database,
        repositories,
        clock=lambda: 5_000,
        id_factory=lambda: "draft-coalesced",
    )
    repositories.coordinator = coordinator
    session = coordinator.open_card(card)
    assert session is not None
    coordinator.update_session(
        session.draft_id,
        text="오래된 스냅샷",
        cursor_position_qchar=5,
    )

    coordinator.protect_now(session.draft_id)

    stored = repositories.get_draft(session.draft_id)
    assert stored is not None
    assert stored.draft_text == "가장 최신 스냅샷"
    assert stored.cursor_position_qchar == 9


def test_draft_write_failure_creates_emergency_copy(
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    card = _create_card(repositories)
    emergency_directory = tmp_path / "recovery"
    coordinator = DraftCoordinator(
        database,
        repositories,
        id_factory=lambda: "draft-emergency",
        emergency_directory=emergency_directory,
    )
    session = coordinator.open_card(card)
    assert session is not None
    coordinator.update_session(
        session.draft_id,
        text="DB 실패에도 보존할 본문",
        cursor_position_qchar=5,
    )
    database.connection.execute(
        """
        CREATE TRIGGER fail_draft_insert
        BEFORE INSERT ON drafts
        BEGIN
            SELECT RAISE(ABORT, '의도한 draft 실패');
        END
        """
    )

    with pytest.raises(sqlite3.IntegrityError, match="의도한 draft 실패"):
        coordinator.protect_now(session.draft_id)

    emergency = emergency_directory / "draft-emergency.json"
    assert emergency.is_file()
    assert "DB 실패에도 보존할 본문" in emergency.read_text(encoding="utf-8")


def test_draft_write_latency_samples_are_logged(
    database: Database,
    repositories: Repositories,
    caplog: pytest.LogCaptureFixture,
) -> None:
    card = _create_card(repositories)
    identifiers = _ids("draft-perf")
    coordinator = DraftCoordinator(
        database,
        repositories,
        id_factory=lambda: next(identifiers),
    )
    samples = (
        ("소형", "가" * 1_024),
        ("1MB", "a" * (1024 * 1024)),
        ("10MB", "a" * (10 * 1024 * 1024)),
    )
    measurements = []
    with caplog.at_level(logging.INFO):
        for _label, text in samples:
            session = coordinator.open_card(card)
            assert session is not None
            coordinator.update_session(
                session.draft_id,
                text=text,
                cursor_position_qchar=0,
            )
            measurement = coordinator.protect_now(session.draft_id)
            assert measurement is not None
            measurements.append(measurement)
            coordinator.discard_session(session.draft_id)

    assert [measurement.text_bytes for measurement in measurements] == [
        len(text.encode("utf-8")) for _, text in samples
    ]
    assert all(measurement.elapsed_ms >= 0 for measurement in measurements)
    messages = [record.message for record in caplog.records]
    assert sum("draft recovery 쓰기 지연" in message for message in messages) == 3
