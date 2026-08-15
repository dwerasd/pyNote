from __future__ import annotations

import os
import sqlite3
import subprocess
import sys
from pathlib import Path

import pytest

from pynote.application.card_service import CardService
from pynote.application.draft_coordinator import DraftCoordinator
from pynote.application.history_service import HistoryService
from pynote.application.save_coordinator import SaveCoordinator, SaveOutcome
from pynote.domain.models import Document
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories


def _document(repositories: Repositories) -> Document:
    document = Document(
        id="stabilization-document",
        title="안정화",
        created_at_us=1,
        updated_at_us=1,
    )
    repositories.create_document(document)
    return document


def test_real_database_lock_preserves_committed_card_and_emergency_draft(
    database: Database,
    repositories: Repositories,
    tmp_path: Path,
) -> None:
    document = _document(repositories)
    card = CardService(database, repositories).create_card(document.id, "확정 본문")
    emergency_directory = tmp_path / "recovery"
    drafts = DraftCoordinator(
        database,
        repositories,
        emergency_directory=emergency_directory,
        id_factory=lambda: "locked-draft",
    )
    session = drafts.open_card(card)
    assert session is not None
    drafts.update_session(
        session.draft_id,
        text="DB lock 중에도 잃지 않을 draft",
        cursor_position_qchar=4,
    )
    blocker = sqlite3.connect(database.path, isolation_level=None, timeout=0)
    blocker.execute("BEGIN IMMEDIATE")
    try:
        with pytest.raises(sqlite3.OperationalError, match="locked"):
            drafts.protect_now(session.draft_id)
    finally:
        blocker.rollback()
        blocker.close()

    stored = repositories.get_card(card.id)
    assert stored is not None
    assert stored.body == "확정 본문"
    emergency = emergency_directory / "locked-draft.json"
    assert emergency.is_file()
    assert "잃지 않을 draft" in emergency.read_text(encoding="utf-8")


def test_forced_process_exit_restores_committed_state_and_separate_draft(
    database_path: Path,
) -> None:
    repository_root = Path(__file__).resolve().parents[2]
    code = """
import os
from pathlib import Path
from pynote.application.card_service import CardService
from pynote.application.draft_coordinator import DraftCoordinator
from pynote.domain.models import Document
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories

database = Database(Path(os.environ["PYNOTE_FORCE_EXIT_DB"]))
repositories = Repositories(database)
repositories.create_document(Document(
    id="forced-document",
    title="강제 종료",
    created_at_us=1,
    updated_at_us=1,
))
card = CardService(database, repositories).create_card(
    "forced-document",
    "강제 종료 전 확정본",
)
drafts = DraftCoordinator(
    database,
    repositories,
    id_factory=lambda: "forced-draft",
)
session = drafts.open_card(card)
drafts.update_session(
    session.draft_id,
    text="강제 종료 시 복구할 draft",
    cursor_position_qchar=3,
)
drafts.protect_now(session.draft_id)
os._exit(17)
"""
    # env= 는 추가가 아니라 자식 환경 전체를 대체한다 — PATH 가 빠지면 PySide6 import 가 죽는다.
    environment = os.environ.copy()
    environment.update(
        {
            "PYNOTE_FORCE_EXIT_DB": str(database_path),
            "PYTHONPATH": str(repository_root / "src"),
        }
    )
    result = subprocess.run(
        [sys.executable, "-c", code],
        cwd=repository_root,
        env=environment,
        check=False,
        capture_output=True,
        text=True,
        timeout=20,
    )
    assert result.returncode == 17, result.stderr

    with Database(database_path) as restarted:
        repositories = Repositories(restarted)
        cards = repositories.list_cards("forced-document")
        drafts = repositories.list_drafts("forced-document")
        assert len(cards) == 1
        assert cards[0].body == "강제 종료 전 확정본"
        assert len(drafts) == 1
        assert drafts[0].draft_text == "강제 종료 시 복구할 draft"


def test_repeated_paste_save_and_cancel_keeps_revision_boundaries_atomic(
    database: Database,
    repositories: Repositories,
) -> None:
    document = _document(repositories)
    card = CardService(database, repositories).create_card(document.id, "초기")
    drafts = DraftCoordinator(database, repositories)
    saver = SaveCoordinator(database, drafts, repositories)

    for number in range(20):
        session = drafts.open_card(card)
        assert session is not None
        drafts.update_session(
            session.draft_id,
            text=f"붙여넣기 저장 {number} 😀",
            cursor_position_qchar=0,
            includes_paste=True,
        )
        result = saver.save(session)
        assert result.outcome is SaveOutcome.SAVED
        card = result.card

        cancelled = drafts.open_card(card)
        assert cancelled is not None
        drafts.update_session(
            cancelled.draft_id,
            text=f"취소할 본문 {number}",
            cursor_position_qchar=0,
        )
        drafts.discard_session(cancelled.draft_id)
        assert repositories.get_card(card.id) == card

    assert len(repositories.list_revisions(card.id)) == 21
    assert database.connection.execute("PRAGMA foreign_key_check").fetchall() == []


def test_every_non_purge_destructive_path_has_a_recovery_state(
    database: Database,
    repositories: Repositories,
) -> None:
    document = _document(repositories)
    card_service = CardService(database, repositories)
    original = card_service.create_card(document.id, "첫 문단\n\n둘째 문단")

    trashed = card_service.soft_delete(original.id)
    assert trashed.deleted_at_us is not None
    original = card_service.restore_card(original.id)
    assert original.deleted_at_us is None

    history = HistoryService(database, repositories)
    current = repositories.get_card(original.id)
    assert current is not None
    drafts = DraftCoordinator(database, repositories)
    session = drafts.open_card(current)
    assert session is not None
    drafts.update_session(
        session.draft_id,
        text=f"{current.body} 수정",
        cursor_position_qchar=0,
    )
    saved = SaveCoordinator(database, drafts, repositories).save(session)
    assert saved.outcome is SaveOutcome.SAVED
    current = saved.card
    revisions_before = repositories.list_revisions(current.id)
    restored_revision = history.restore(current.id, revisions_before[0].id)
    revisions_after = repositories.list_revisions(current.id)
    assert restored_revision.revision.parent_revision_id == current.current_revision_id
    assert len(revisions_after) == len(revisions_before) + 1
    assert database.connection.execute("PRAGMA foreign_key_check").fetchall() == []
