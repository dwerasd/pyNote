from __future__ import annotations

import argparse
import os
import sqlite3
import sys
import tempfile
from collections.abc import Callable, Iterator
from dataclasses import replace
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    source_root = args.source_root.resolve()
    sys.path.insert(0, str(source_root / "src"))

    import pynote
    from pynote.application.draft_coordinator import DraftCoordinator
    from pynote.application.save_coordinator import (
        ImeCompositionInProgressError,
        SaveCoordinator,
    )
    from pynote.domain.events import EditEvent, EventSource, EventType
    from pynote.domain.models import (
        CaptureOperationSource,
        Card,
        CardRevision,
        CardSource,
        Document,
        NewCaptureOperation,
        NewCard,
        RevisionSource,
        SplitPolicy,
    )
    from pynote.infrastructure.database import Database
    from pynote.infrastructure.repositories import Repositories, text_hash

    package = Path(pynote.__file__).resolve()
    if not package.is_relative_to(source_root):
        raise RuntimeError(f"pynote import escaped source root: {package}")

    lines: list[str] = []

    def identifiers(values: tuple[str, ...]) -> Iterator[str]:
        yield from values

    def create_card(repositories: Repositories) -> Card:
        repositories.create_document(Document(
            id="document-1", title="save test", created_at_us=1_000, updated_at_us=1_000,
        ))
        return repositories.create_cards(
            NewCaptureOperation(
                id="operation-1",
                document_id="document-1",
                source=CaptureOperationSource.TYPING,
                split_policy=SplitPolicy.KEEP,
                original_text=None,
                created_at_us=2_000,
            ),
            [NewCard(
                id="card-1",
                revision_id="revision-1",
                event_id="event-1",
                position_key=1_024,
                body="기존 확정 본문",
                card_source=CardSource.TYPING,
                event_source=EventSource.TYPING,
                revision_source=RevisionSource.EDIT,
                created_at_us=2_000,
            )],
        )[0]

    def commit_body(
        database: Database,
        repositories: Repositories,
        body: str,
        suffix: str,
        at: int,
    ) -> Card:
        card = repositories.get_card("card-1")
        if card is None or card.current_revision_id is None:
            raise RuntimeError("commit fixture card missing")
        with database.transaction():
            event = repositories.create_event(EditEvent(
                event_seq=None,
                event_id=f"event-{suffix}",
                operation_id=None,
                document_id=card.document_id,
                card_id=card.id,
                event_type=EventType.UPDATE,
                source=EventSource.EDIT,
                occurred_at_us=at,
                details_json="{}",
            ))
            if event.event_seq is None:
                raise RuntimeError("commit fixture event sequence missing")
            revision = CardRevision(
                id=f"revision-{suffix}",
                card_id=card.id,
                event_seq=event.event_seq,
                parent_revision_id=card.current_revision_id,
                body=body,
                body_hash=text_hash(body),
                source=RevisionSource.EDIT,
                created_at_us=at,
            )
            repositories.create_revision(revision)
            saved = replace(
                card,
                body=body,
                body_hash=revision.body_hash,
                current_revision_id=revision.id,
                updated_at_us=at,
            )
            repositories.advance_card_revision(saved, expected_revision_id=card.current_revision_id)
            repositories.touch_document(card.document_id, at)
        return saved

    class CommitAfterReadRepositories(Repositories):
        def __init__(self, database: Database, callback: Callable[[], object]) -> None:
            super().__init__(database)
            self.callback = callback
            self.fired = False

        def get_card(self, card_id: str) -> Card | None:
            card = super().get_card(card_id)
            if not self.database.connection.in_transaction and not self.fired:
                self.fired = True
                self.callback()
            return card

    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)

        database = Database(directory / "0001.sqlite3")
        repositories = Repositories(database)
        card = create_card(repositories)
        drafts = DraftCoordinator(
            database, repositories, clock=lambda: 3_000, age_clock=lambda: 0,
            id_factory=lambda: "draft-1",
        )
        id_order: list[str] = []
        save_ids = identifiers(("revision-save", "event-save"))

        def next_save_id() -> str:
            value = next(save_ids)
            id_order.append(value)
            return value

        clock_calls = [0]

        def save_clock() -> int:
            clock_calls[0] += 1
            return 4_000

        save = SaveCoordinator(database, drafts, repositories, clock=save_clock, id_factory=next_save_id)
        session = drafts.open_card(card)
        if session is None:
            raise RuntimeError("0001 session missing")
        drafts.update_session(
            session.draft_id, text="새 확정 본문", cursor_position_qchar=7, includes_paste=True,
        )
        result = save.save(session)
        events = repositories.list_events(card.document_id)
        revisions = repositories.list_revisions(card.id)
        document = repositories.get_document(card.document_id)
        stored_draft = repositories.get_draft(session.draft_id)
        immutable = (
            result.card.operation_id == card.operation_id
            and result.card.position_key == card.position_key
            and result.card.capture_seq == card.capture_seq
            and result.card.created_at_us == card.created_at_us
            and result.card.source == card.source
            and result.card.document_id == card.document_id
            and result.card.deleted_at_us == card.deleted_at_us
        )
        if document is None:
            raise RuntimeError("0001 document missing")
        lines.append(
            f"W2-Z5-0001|outcome={result.outcome.value}|body={result.card.body.encode().hex()}"
            f"|time={result.card.updated_at_us}|ids={','.join(id_order)}"
            f"|details={events[-1].details_json.encode().hex()}|parent={revisions[-1].parent_revision_id}"
            f"|counts={len(revisions)},{len(events)}|draft={'present' if stored_draft else 'missing'}"
            f"|session={'clean' if not session.dirty else 'dirty'}|immutable={'true' if immutable else 'false'}"
            f"|document={document.updated_at_us}"
        )
        database.close()

        database = Database(directory / "0002.sqlite3")
        repositories = Repositories(database)
        card = create_card(repositories)
        drafts = DraftCoordinator(
            database, repositories, clock=lambda: 3_000, age_clock=lambda: 0,
            id_factory=lambda: "draft-failure",
        )
        save_ids = identifiers(("revision-failure", "event-failure"))
        save = SaveCoordinator(
            database, drafts, repositories, clock=lambda: 4_000, id_factory=lambda: next(save_ids),
        )
        session = drafts.open_card(card)
        if session is None:
            raise RuntimeError("0002 session missing")
        drafts.update_session(session.draft_id, text="rollback-body", cursor_position_qchar=6)
        database.connection.execute(
            "CREATE TRIGGER fail_card_update BEFORE UPDATE ON cards "
            "WHEN NEW.body = 'rollback-body' BEGIN SELECT RAISE(ABORT, 'save failure'); END"
        )
        save_outcome = "saved"
        reason = "other"
        try:
            save.save(session)
        except sqlite3.IntegrityError:
            save_outcome = "failed"
            reason = "repository"
        stored = repositories.get_draft(session.draft_id)
        stored_card = repositories.get_card(card.id)
        document = repositories.get_document(card.document_id)
        if stored is None or stored_card is None or document is None:
            raise RuntimeError("0002 rollback observations missing")
        lines.append(
            f"W2-Z5-0002|outcome={save_outcome}|reason={reason}|card={stored_card.body.encode().hex()}"
            f"|counts={len(repositories.list_revisions(card.id))},{len(repositories.list_events(card.document_id))}"
            f"|document={document.updated_at_us}|draft={stored.draft_text.encode().hex()}"
            f"|dirty={'true' if session.dirty else 'false'}"
        )
        database.close()

        database = Database(directory / "0003.sqlite3")
        repositories = Repositories(database)
        card = create_card(repositories)
        drafts = DraftCoordinator(
            database, repositories, clock=lambda: 3_000, age_clock=lambda: 0,
            id_factory=lambda: "draft-same",
        )
        save = SaveCoordinator(database, drafts, repositories)
        session = drafts.open_card(card)
        if session is None:
            raise RuntimeError("0003 session missing")
        drafts.update_session(session.draft_id, text="temporary", cursor_position_qchar=3)
        drafts.protect_now(session.draft_id)
        drafts.update_session(session.draft_id, text=card.body, cursor_position_qchar=0)
        result = save.save(session)
        clean_after_save = not session.dirty
        missing_after_save = repositories.get_draft(session.draft_id) is None
        drafts.update_session(session.draft_id, text="temporary", cursor_position_qchar=3)
        drafts.protect_now(session.draft_id)
        rewritten = repositories.get_draft(session.draft_id)
        if rewritten is None:
            raise RuntimeError("0003 dedup rewrite missing")
        stored_card = repositories.get_card(card.id)
        document = repositories.get_document(card.document_id)
        lines.append(
            f"W2-Z5-0003|outcome={result.outcome.value}"
            f"|counts={len(repositories.list_revisions(card.id))},{len(repositories.list_events(card.document_id))}"
            f"|card_same={'true' if stored_card == card else 'false'}"
            f"|document_same={'true' if document is not None and document.updated_at_us == 1_000 else 'false'}"
            f"|draft={'missing' if missing_after_save else 'present'}"
            f"|session={'clean' if clean_after_save else 'dirty'}|rewrite={rewritten.draft_text.encode().hex()}"
        )
        database.close()

        database = Database(directory / "0004.sqlite3")
        repositories = Repositories(database)
        card = create_card(repositories)
        first = DraftCoordinator(database, repositories, clock=lambda: 3_000, id_factory=lambda: "draft-first")
        second = DraftCoordinator(database, repositories, clock=lambda: 3_000, id_factory=lambda: "draft-second")
        first_session = first.open_card(card)
        second_session = second.open_card(card)
        if first_session is None or second_session is None:
            raise RuntimeError("0004 session missing")
        first.update_session(first_session.draft_id, text="오래된 편집", cursor_position_qchar=5)
        second.update_session(second_session.draft_id, text="먼저 저장된 편집", cursor_position_qchar=5)
        second_ids = identifiers(("revision-second", "event-second"))
        SaveCoordinator(
            database, second, repositories, clock=lambda: 4_000, id_factory=lambda: next(second_ids),
        ).save(second_session)
        result = SaveCoordinator(database, first, repositories).save(first_session)
        if result.conflict is None:
            raise RuntimeError("0004 conflict missing")
        durable = repositories.get_draft(first_session.draft_id)
        if durable is None:
            raise RuntimeError("0004 durable draft missing")
        lines.append(
            f"W2-Z5-0004|outcome={result.outcome.value}|base={result.conflict.base_revision_id}"
            f"|current={result.conflict.current_revision_id}|base_text={result.conflict.base_text.encode().hex()}"
            f"|committed={result.conflict.committed_text.encode().hex()}|draft={result.conflict.draft_text.encode().hex()}"
            f"|counts={len(repositories.list_revisions(card.id))},{len(repositories.list_events(card.document_id))}"
            f"|durable={durable.draft_text.encode().hex()}|dirty={'true' if first_session.dirty else 'false'}"
        )
        database.close()

        database = Database(directory / "0005.sqlite3")
        repositories = Repositories(database)
        card = create_card(repositories)
        drafts = DraftCoordinator(database, repositories, clock=lambda: 3_000, id_factory=lambda: "draft-race")
        session = drafts.open_card(card)
        if session is None:
            raise RuntimeError("0005 session missing")
        drafts.update_session(session.draft_id, text="stale-save", cursor_position_qchar=0)
        with Database(database.path) as other_database:
            other_repositories = Repositories(other_database)
            racing_repositories = CommitAfterReadRepositories(
                database,
                lambda: commit_body(other_database, other_repositories, "racing-save", "race", 10_000),
            )
            result = SaveCoordinator(database, drafts, racing_repositories).save(session)
        stored = repositories.get_card(card.id)
        durable = repositories.get_draft(session.draft_id)
        if stored is None or durable is None:
            raise RuntimeError("0005 race observation missing")
        lines.append(
            f"W2-Z5-0005|outcome={result.outcome.value}|body={stored.body.encode().hex()}"
            f"|revision={stored.current_revision_id}"
            f"|counts={len(repositories.list_revisions(card.id))},{len(repositories.list_events(card.document_id))}"
            f"|draft={durable.draft_text.encode().hex()}"
        )
        database.close()

        database = Database(directory / "0006.sqlite3")
        repositories = Repositories(database)
        card = create_card(repositories)
        draft_ids = identifiers(("draft-ime", "draft-new", "draft-inactive"))
        drafts = DraftCoordinator(database, repositories, clock=lambda: 3_000, id_factory=lambda: next(draft_ids))
        clock_calls = [0]
        id_calls = [0]

        def guard_clock() -> int:
            clock_calls[0] += 1
            return 4_000

        def guard_id() -> str:
            id_calls[0] += 1
            return "unused"

        save = SaveCoordinator(database, drafts, repositories, clock=guard_clock, id_factory=guard_id)
        ime_session = drafts.open_card(card)
        if ime_session is None:
            raise RuntimeError("0006 IME session missing")
        drafts.set_ime_composing(ime_session.draft_id, True)
        try:
            save.save(ime_session)
            ime = "other"
        except ImeCompositionInProgressError:
            ime = "ime_composing"
        drafts.set_ime_composing(ime_session.draft_id, False)
        drafts.release_session(ime_session.draft_id)
        new_session = drafts.open_new(card.document_id)
        try:
            save.save(new_session)
            missing = "other"
        except ValueError:
            missing = "missing_card_identity"
        inactive_session = drafts.open_card(card)
        if inactive_session is None or card.current_revision_id is None:
            raise RuntimeError("0006 inactive session missing")
        repositories.update_card_deleted_state(
            card.id,
            position_key=card.position_key,
            deleted_at_us=9_000,
            expected_revision_id=card.current_revision_id,
        )
        before = repositories.get_card(card.id)
        try:
            save.save(inactive_session)
            inactive = "other"
        except KeyError:
            inactive = "inactive_card"
        after = repositories.get_card(card.id)
        if before != after:
            raise RuntimeError("0006 guard mutated inactive card")
        lines.append(
            f"W2-Z5-0006|ime={ime}|new={missing}|inactive={inactive}"
            f"|history={len(repositories.list_revisions(card.id))},{len(repositories.list_events(card.document_id))}"
            f"|clock={clock_calls[0]}|ids={id_calls[0]}"
        )
        database.close()

        expected = [f"W2-Z5-{number:04d}" for number in range(1, 7)]
        actual = [line.split("|", 1)[0] for line in lines]
        if actual != expected:
            raise RuntimeError(f"ordered labels mismatch: {actual!r}")

        source_package = source_root / "src"
        for name, module in tuple(sys.modules.items()):
            if name != "pynote" and not name.startswith("pynote."):
                continue
            module_file = getattr(module, "__file__", None)
            if module_file is None:
                continue
            resolved = Path(module_file).resolve()
            if not resolved.is_relative_to(source_package):
                raise RuntimeError(f"pynote import escaped source root: {name} -> {resolved}")

        args.output.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
