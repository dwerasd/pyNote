from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
from collections.abc import Iterator
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    source_root = args.source_root.resolve()
    os.environ["QT_QPA_PLATFORM"] = "offscreen"
    sys.path.insert(0, str(source_root / "src"))

    import pynote
    from PySide6.QtCore import QCoreApplication, QEventLoop, QTimer
    from pynote.application import draft_coordinator as draft_module
    from pynote.application.draft_coordinator import (
        DraftCoordinator,
        DraftDisposition,
        RecoveryCandidate,
        build_recovery_plans,
    )
    from pynote.application.save_coordinator import SaveCoordinator
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

    package = Path(pynote.__file__).resolve()
    if not package.is_relative_to(source_root):
        raise RuntimeError(f"pynote import escaped source root: {package}")

    application = QCoreApplication.instance() or QCoreApplication([])
    lines: list[str] = []

    def wait(milliseconds: int) -> None:
        loop = QEventLoop()
        QTimer.singleShot(milliseconds, loop.quit)
        loop.exec()

    def add_document(repositories: Repositories, document_id: str = "document-1") -> None:
        repositories.create_document(Document(
            id=document_id,
            title="draft test",
            created_at_us=1_000,
            updated_at_us=1_000,
        ))

    def create_card(repositories: Repositories, number: int = 1) -> Card:
        if repositories.get_document("document-1") is None:
            add_document(repositories)
        return repositories.create_cards(
            NewCaptureOperation(
                id=f"operation-{number}",
                document_id="document-1",
                source=CaptureOperationSource.TYPING,
                split_policy=SplitPolicy.KEEP,
                original_text=None,
                created_at_us=2_000 + number,
            ),
            [NewCard(
                id=f"card-{number}",
                revision_id=f"revision-{number}",
                event_id=f"event-{number}",
                position_key=number * 1_024,
                body=f"committed-{number}",
                card_source=CardSource.TYPING,
                event_source=EventSource.TYPING,
                revision_source=RevisionSource.EDIT,
                created_at_us=2_000 + number,
            )],
        )[0]

    def make_draft(
        repositories: Repositories,
        draft_id: str,
        card_id: str | None,
        text: str,
        updated_at_us: int,
        cursor: int = 2,
    ) -> Draft:
        draft = Draft(
            id=draft_id,
            document_id="document-1",
            card_id=card_id,
            draft_kind=DraftKind.EDIT if card_id is not None else DraftKind.NEW,
            base_revision_id=None,
            draft_text=text,
            draft_hash=draft_module.text_hash(text),
            cursor_position_qchar=cursor,
            updated_at_us=updated_at_us,
        )
        repositories.create_draft(draft)
        return draft

    def ids(prefix: str) -> Iterator[str]:
        number = 0
        while True:
            number += 1
            yield f"{prefix}-{number}"

    def recovery_candidate(document_id: str, card_id: str | None, draft_id: str) -> RecoveryCandidate:
        draft = Draft(
            id=draft_id,
            document_id=document_id,
            card_id=card_id,
            draft_kind=DraftKind.NEW if card_id is None else DraftKind.EDIT,
            base_revision_id=None,
            draft_text=draft_id,
            draft_hash=draft_module.text_hash(draft_id),
            cursor_position_qchar=2,
            updated_at_us=10,
        )
        return RecoveryCandidate(draft=draft, committed_text="committed", committed_revision_id=None)

    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)

        plans = build_recovery_plans(
            (
                recovery_candidate("document-b", "card-b1", "draft-b1"),
                recovery_candidate("document-a", "card-a1", "draft-a1"),
                recovery_candidate("document-b", "card-b2", "draft-b2"),
                recovery_candidate("document-b", "card-b1", "copy"),
                recovery_candidate("document-a", None, "new"),
                recovery_candidate("document-a", "card-a2", "draft-a2"),
            ),
            opened_editor_cards={"document-b": "card-b2", "document-a": "not-a-candidate"},
        )
        plan_text = ";".join(
            f"{plan.document_id}:{plan.display_card_id}>{','.join(plan.deferred_card_ids)}"
            for plan in plans
        )
        lines.append(f"W2-Z4-0144|plans={plan_text}")

        for label, updated, expected in (
            ("W2-Z4-0145", 2_002, False),
            ("W2-Z4-0146", 2_001, True),
            ("W2-Z4-0147", 2_000, True),
        ):
            database = Database(directory / f"{label[-4:]}.sqlite3")
            repositories = Repositories(database)
            card = create_card(repositories)
            make_draft(repositories, "draft-time", card.id, "draft-time", updated)
            candidates = DraftCoordinator(database, repositories).recovery_candidates()
            if len(candidates) != 1 or candidates[0].committed_is_newer is not expected:
                raise RuntimeError(f"{label} timestamp relation mismatch")
            lines.append(f"{label}|newer={'true' if expected else 'false'}")
            database.close()

        database = Database(directory / "0148.sqlite3")
        repositories = Repositories(database)
        card = create_card(repositories)
        database.connection.execute("UPDATE cards SET current_revision_id = NULL WHERE id = ?", (card.id,))
        make_draft(repositories, "draft-revisionless", card.id, "revisionless", 1)
        before = make_draft(repositories, "draft-new", None, "new", 2)
        candidates = DraftCoordinator(database, repositories).recovery_candidates()
        after = repositories.get_draft("draft-new")
        if [candidate.draft.id for candidate in candidates] != ["draft-revisionless"]:
            raise RuntimeError("0148 candidate mismatch")
        lines.append(
            "W2-Z4-0148|candidates=draft-revisionless"
            f"|new_unchanged={'true' if after == before else 'false'}"
        )
        database.close()

        database = Database(directory / "0149.sqlite3")
        repositories = Repositories(database)
        card = create_card(repositories)
        coordinator = DraftCoordinator(
            database, repositories, idle_ms=10, clock=lambda: 5_000, id_factory=lambda: "draft-ime",
        )
        session = coordinator.open_card(card)
        if session is None:
            raise RuntimeError("0149 session missing")
        coordinator.update_session(session.draft_id, text="ㅎ", cursor_position_qchar=1)
        coordinator.set_ime_composing(session.draft_id, True)
        wait(30)
        during = repositories.get_draft(session.draft_id)
        coordinator.set_ime_composing(session.draft_id, False)
        for _ in range(20):
            wait(5)
            stored = repositories.get_draft(session.draft_id)
            if stored is not None:
                break
        else:
            raise RuntimeError("0149 idle timer did not protect")
        lines.append(
            f"W2-Z4-0149|during={'missing' if during is None else 'present'}"
            f"|after={stored.draft_text.encode().hex()}"
        )
        database.close()

        database = Database(directory / "0150.sqlite3")
        repositories = Repositories(database)
        card = create_card(repositories)
        wall = [500_000]
        age = [0]
        coordinator = DraftCoordinator(
            database, repositories, idle_ms=100, clock=lambda: wall[0], age_clock=lambda: age[0],
            id_factory=lambda: "draft-max-age",
        )
        session = coordinator.open_card(card)
        if session is None:
            raise RuntimeError("0150 session missing")
        revision_count = len(repositories.list_revisions(card.id))
        for elapsed, text in ((0, "continuous-1"), (40_000, "continuous-12"), (80_000, "continuous-123")):
            age[0] = elapsed
            wall[0] = 500_000 - elapsed
            coordinator.update_session(session.draft_id, text=text, cursor_position_qchar=1)
        age[0] = 120_000
        wall[0] = 380_000
        coordinator.update_session(session.draft_id, text="continuous-1234", cursor_position_qchar=10)
        stored = repositories.get_draft(session.draft_id)
        if stored is None:
            raise RuntimeError("0150 max age row missing")
        lines.append(
            f"W2-Z4-0150|text={stored.draft_text.encode().hex()}|updated={stored.updated_at_us}"
            f"|revisions={revision_count}"
        )
        database.close()

        database = Database(directory / "0151.sqlite3")
        repositories = Repositories(database)
        card = create_card(repositories)
        age = [0]
        coordinator = DraftCoordinator(
            database, repositories, idle_ms=100, clock=lambda: 7_000, age_clock=lambda: age[0],
            id_factory=lambda: "draft-first-anchor",
        )
        session = coordinator.open_card(card)
        if session is None:
            raise RuntimeError("0151 session missing")
        age[0] = 10_000_000
        coordinator.update_session(session.draft_id, text="first-dirty", cursor_position_qchar=11)
        lines.append(
            f"W2-Z4-0151|stored={'true' if repositories.get_draft(session.draft_id) else 'false'}"
        )
        database.close()

        database = Database(directory / "0152.sqlite3")
        repositories = Repositories(database)
        card = create_card(repositories)
        age = [0]
        coordinator = DraftCoordinator(
            database, repositories, idle_ms=100, clock=lambda: 8_000, age_clock=lambda: age[0],
            id_factory=lambda: "draft-reset",
        )
        session = coordinator.open_card(card)
        if session is None:
            raise RuntimeError("0152 session missing")
        coordinator.update_session(session.draft_id, text="dirty-one", cursor_position_qchar=8)
        age[0] = 90_000
        coordinator.update_session(session.draft_id, text=card.body, cursor_position_qchar=4)
        age[0] = 1_000_000
        coordinator.update_session(session.draft_id, text="dirty-two", cursor_position_qchar=9)
        age[0] = 1_099_999
        coordinator.update_session(session.draft_id, text="dirty-two-2", cursor_position_qchar=11)
        before = repositories.get_draft(session.draft_id)
        age[0] = 1_100_000
        coordinator.update_session(session.draft_id, text="dirty-two-3", cursor_position_qchar=11)
        after = repositories.get_draft(session.draft_id)
        if after is None:
            raise RuntimeError("0152 reset row missing")
        lines.append(
            f"W2-Z4-0152|at99999={'true' if before else 'false'}"
            f"|at100000={after.draft_text.encode().hex()}"
        )
        database.close()

        database = Database(directory / "0153.sqlite3")
        repositories = Repositories(database)
        card = create_card(repositories)
        coordinator = DraftCoordinator(
            database, repositories, idle_ms=10, clock=lambda: 5_000, id_factory=lambda: "draft-dedup",
        )
        protected: list[str] = []
        coordinator.draft_protected.connect(lambda draft_id, _at, _elapsed: protected.append(draft_id))
        session = coordinator.open_card(card)
        if session is None:
            raise RuntimeError("0153 session missing")
        coordinator.update_session(session.draft_id, text="once", cursor_position_qchar=5)
        coordinator.protect_now(session.draft_id)
        coordinator.protect_now(session.draft_id)
        coordinator.protect_now(session.draft_id)
        wait(30)
        timer_count = len(protected)
        coordinator.discard_draft(session.draft_id)
        coordinator.protect_now(session.draft_id)
        lines.append(
            f"W2-Z4-0153|writes=1,{len(protected)}|timer={timer_count}"
            f"|revisions={len(repositories.list_revisions(card.id))}"
        )
        database.close()

        database = Database(directory / "0154.sqlite3")
        repositories = Repositories(database)
        card = create_card(repositories)
        coordinator = DraftCoordinator(database, repositories, id_factory=lambda: "draft-one-hash")
        session = coordinator.open_card(card)
        if session is None:
            raise RuntimeError("0154 session missing")
        coordinator.update_session(session.draft_id, text="hash-once", cursor_position_qchar=3)
        original_hash = draft_module.text_hash
        hash_calls: list[str] = []

        def observed_hash(text: str) -> str:
            hash_calls.append(text)
            return original_hash(text)

        draft_module.text_hash = observed_hash
        try:
            coordinator.protect_now(session.draft_id)
        finally:
            draft_module.text_hash = original_hash
        lines.append(f"W2-Z4-0154|hash_calls={len(hash_calls)}")
        database.close()

        database = Database(directory / "0155.sqlite3")
        repositories = Repositories(database)
        card = create_card(repositories)
        coordinator = DraftCoordinator(
            database, repositories, clock=lambda: 5_000, age_clock=lambda: 6_000,
            id_factory=lambda: "draft-save",
        )
        session = coordinator.open_card(card)
        if session is None:
            raise RuntimeError("0155 session missing")
        coordinator.update_session(session.draft_id, text="before-save", cursor_position_qchar=10)
        coordinator.protect_now(session.draft_id)
        coordinator.update_session(session.draft_id, text=card.body, cursor_position_qchar=len(card.body))
        SaveCoordinator(database, coordinator, repositories, clock=lambda: 7_000).save(session)
        coordinator.update_session(session.draft_id, text="before-save", cursor_position_qchar=10)
        coordinator.protect_now(session.draft_id)
        stored = repositories.get_draft(session.draft_id)
        if stored is None:
            raise RuntimeError("0155 rewritten row missing")
        lines.append(f"W2-Z4-0155|restored={stored.draft_text.encode().hex()}")
        database.close()

        database = Database(directory / "0156.sqlite3")
        repositories = Repositories(database)
        card = create_card(repositories)
        coordinator = DraftCoordinator(
            database, repositories, clock=lambda: 5_000, age_clock=lambda: 6_000,
            id_factory=lambda: "draft-reused",
        )
        session = coordinator.open_card(card)
        if session is None:
            raise RuntimeError("0156 session missing")
        coordinator.update_session(session.draft_id, text="before-release", cursor_position_qchar=10)
        coordinator.protect_now(session.draft_id)
        coordinator.release_session(session.draft_id)
        with database.transaction():
            repositories.delete_draft(session.draft_id)
        reopened = coordinator.open_card(card)
        if reopened is None:
            raise RuntimeError("0156 reopened session missing")
        coordinator.update_session(reopened.draft_id, text="before-release", cursor_position_qchar=10)
        coordinator.protect_now(reopened.draft_id)
        stored = repositories.get_draft(reopened.draft_id)
        if stored is None:
            raise RuntimeError("0156 rewritten row missing")
        lines.append(f"W2-Z4-0156|restored={stored.draft_text.encode().hex()}")
        database.close()

        database = Database(directory / "0157.sqlite3")
        repositories = Repositories(database)
        card = create_card(repositories)
        age = [0]
        coordinator = DraftCoordinator(
            database, repositories, idle_ms=100, clock=lambda: 5_000, age_clock=lambda: age[0],
            id_factory=lambda: "draft-retry",
        )
        failed: list[str] = []
        coordinator.draft_write_failed.connect(lambda draft_id, _message: failed.append(draft_id))
        session = coordinator.open_card(card)
        if session is None:
            raise RuntimeError("0157 session missing")
        coordinator.update_session(session.draft_id, text="before-failure", cursor_position_qchar=6)
        database.connection.execute(
            "CREATE TRIGGER fail_draft BEFORE INSERT ON drafts BEGIN SELECT RAISE(ABORT, 'fail'); END"
        )
        age[0] = 100_000
        coordinator.update_session(session.draft_id, text="first-failure", cursor_position_qchar=8)
        before = repositories.get_draft(session.draft_id)
        database.connection.execute("DROP TRIGGER fail_draft")
        age[0] = 110_000
        coordinator.update_session(session.draft_id, text="next-retry", cursor_position_qchar=9)
        after = repositories.get_draft(session.draft_id)
        if after is None:
            raise RuntimeError("0157 retry row missing")
        lines.append(
            f"W2-Z4-0157|failures={len(failed)}|before={'true' if before else 'false'}"
            f"|after={after.draft_text.encode().hex()}"
        )
        database.close()

        restart_path = directory / "0158.sqlite3"
        database = Database(restart_path)
        repositories = Repositories(database)
        cards = tuple(create_card(repositories, number) for number in range(1, 4))
        for card, draft_id in zip(cards, ("draft-recover", "draft-discard", "draft-later"), strict=True):
            make_draft(repositories, draft_id, card.id, card.body + "+unsaved", 9_000 + int(card.id[-1]), 4)
        database.close()
        database = Database(restart_path)
        repositories = Repositories(database)
        restarted = DraftCoordinator(database, repositories)
        candidates = restarted.recovery_candidates()
        recovered = restarted.resolve_candidate("draft-recover", DraftDisposition.RECOVER)
        restarted.resolve_candidate("draft-discard", DraftDisposition.DISCARD)
        restarted.resolve_candidate("draft-later", DraftDisposition.LATER)
        open_later = restarted.open_card(cards[2])
        if recovered is None:
            raise RuntimeError("0158 recovered session missing")
        lines.append(
            "W2-Z4-0158|candidates=" + ",".join(candidate.draft.id for candidate in candidates)
            + f"|recover={recovered.text.encode().hex()}"
            + f"|discarded={'true' if repositories.get_draft('draft-discard') is None else 'false'}"
            + f"|later={'true' if repositories.get_draft('draft-later') is not None else 'false'}"
            + f"|open={'deferred' if open_later is None else 'opened'}"
        )
        database.close()

        database = Database(directory / "0159.sqlite3")
        repositories = Repositories(database)
        card = create_card(repositories)
        make_draft(repositories, "draft-corrupt", card.id, "valid", 9_000)
        database.connection.execute(
            "UPDATE drafts SET draft_text = ? WHERE id = ?", ("corrupt", "draft-corrupt")
        )
        coordinator = DraftCoordinator(database, repositories)
        candidates = coordinator.recovery_candidates()
        resolve = "none"
        opened = "none"
        try:
            coordinator.resolve_candidate("draft-corrupt", DraftDisposition.RECOVER)
        except RuntimeError:
            resolve = "corrupt"
        try:
            coordinator.open_card(card, disposition=DraftDisposition.RECOVER)
        except RuntimeError:
            opened = "corrupt"
        lines.append(f"W2-Z4-0159|candidates={len(candidates)}|resolve={resolve}|open={opened}")
        database.close()

        class ReentrantRepositories(Repositories):
            coordinator: DraftCoordinator | None = None
            reentered = False

            def create_draft(self, draft: object) -> None:
                super().create_draft(draft)  # type: ignore[arg-type]
                if self.reentered or self.coordinator is None:
                    return
                self.reentered = True
                self.coordinator.update_session("draft-coalesced", text="latest", cursor_position_qchar=9)
                self.coordinator.protect_now("draft-coalesced")

        database = Database(directory / "0160.sqlite3")
        repositories = ReentrantRepositories(database)
        card = create_card(repositories)
        coordinator = DraftCoordinator(database, repositories, clock=lambda: 5_000, id_factory=lambda: "draft-coalesced")
        repositories.coordinator = coordinator
        session = coordinator.open_card(card)
        if session is None:
            raise RuntimeError("0160 session missing")
        coordinator.update_session(session.draft_id, text="old", cursor_position_qchar=5)
        coordinator.protect_now(session.draft_id)
        stored = repositories.get_draft(session.draft_id)
        if stored is None:
            raise RuntimeError("0160 coalesced row missing")
        lines.append(f"W2-Z4-0160|text={stored.draft_text.encode().hex()}|cursor={stored.cursor_position_qchar}")
        database.close()

        database = Database(directory / "0161.sqlite3")
        repositories = Repositories(database)
        card = create_card(repositories)
        emergency_directory = directory / "emergency"
        coordinator = DraftCoordinator(
            database, repositories, clock=lambda: 7_000, id_factory=lambda: "draft-emergency",
            emergency_directory=emergency_directory,
        )
        failed = []
        coordinator.draft_write_failed.connect(lambda draft_id, _message: failed.append(draft_id))
        session = coordinator.open_card(card)
        if session is None:
            raise RuntimeError("0161 session missing")
        coordinator.update_session(session.draft_id, text="preserve-me", cursor_position_qchar=5, includes_paste=True)
        database.connection.execute(
            "CREATE TRIGGER fail_emergency BEFORE INSERT ON drafts BEGIN SELECT RAISE(ABORT, 'fail'); END"
        )
        outcome = "ok"
        try:
            coordinator.protect_now(session.draft_id)
        except Exception:
            outcome = "storage_failure"
        payload = json.loads((emergency_directory / "draft-emergency.json").read_text(encoding="utf-8"))
        lines.append(
            f"W2-Z4-0161|result={outcome}|failed={len(failed)}|emergency="
            f"{payload['draft_id']},{payload['document_id']},{payload['card_id']},{payload['base_revision_id']},"
            f"{payload['draft_text'].encode().hex()},{payload['cursor_position_qchar']},{payload['written_at_us']}"
        )
        database.close()

        database = Database(directory / "0162.sqlite3")
        repositories = Repositories(database)
        card = create_card(repositories)
        identifiers = ids("draft-perf")
        coordinator = DraftCoordinator(database, repositories, id_factory=lambda: next(identifiers))
        protected = []
        coordinator.draft_protected.connect(lambda draft_id, _at, _elapsed: protected.append(draft_id))
        measurements = []
        for text in ("가" * 1_024, "a" * (1_024 * 1_024), "a" * (10 * 1_024 * 1_024)):
            session = coordinator.open_card(card)
            if session is None:
                raise RuntimeError("0162 session missing")
            coordinator.update_session(session.draft_id, text=text, cursor_position_qchar=0)
            measurement = coordinator.protect_now(session.draft_id)
            if measurement is None:
                raise RuntimeError("0162 measurement missing")
            measurements.append(measurement)
            coordinator.discard_session(session.draft_id)
        lines.append(
            "W2-Z4-0162|bytes=" + ",".join(str(value.text_bytes) for value in measurements)
            + f"|nonnegative={'true' if all(value.elapsed_ms >= 0 for value in measurements) else 'false'}"
            + f"|protected={len(protected)}"
        )

        expected_labels = [f"W2-Z4-{number:04d}" for number in range(144, 163)]
        actual_labels = [line.split("|", 1)[0] for line in lines]
        if actual_labels != expected_labels:
            raise RuntimeError(f"ordered labels mismatch: {actual_labels!r}")

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
        database.close()

    application.processEvents()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
