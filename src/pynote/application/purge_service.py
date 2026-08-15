from __future__ import annotations

import json
import logging
import time
from collections.abc import Callable
from dataclasses import dataclass

from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories

LOGGER = logging.getLogger(__name__)

Clock = Callable[[], int]
REDACTED_MARKER = "[purge로 소거됨]"


@dataclass(frozen=True, slots=True)
class PurgeResult:
    """완전 삭제 트랜잭션의 대상과 삭제 수를 기록한다."""

    target_id: str
    target_kind: str
    purged_card_count: int
    redacted_event_count: int
    redacted_operation_count: int


class PurgeService:
    """보존 기간이 지난 휴지통 항목만 FK 제약 순서를 지켜 완전 삭제한다."""

    def __init__(
        self,
        database: Database,
        repositories: Repositories | None = None,
        *,
        clock: Clock | None = None,
    ) -> None:
        self._database = database
        self._connection = database.connection
        self._repositories = repositories or Repositories(database)
        self._clock = clock or (lambda: time.time_ns() // 1_000)

    def purge_card(self, card_id: str, *, retention_days: int) -> PurgeResult:
        """휴지통 보존 기간이 지난 카드 한 장을 물리 삭제한다."""
        card = self._repositories.get_card(card_id)
        if card is None:
            raise KeyError(f"존재하지 않는 카드입니다: {card_id}")
        self._require_expired(card.deleted_at_us, retention_days, "카드")
        now_us = self._clock()

        revision_rows = self._connection.execute(
            "SELECT id FROM card_revisions WHERE card_id = ?",
            (card_id,),
        ).fetchall()
        revision_ids = tuple(str(row[0]) for row in revision_rows)
        lineage_rows = self._connection.execute(
            """
            SELECT event_seq, parent_card_id, child_card_id
            FROM card_lineage
            WHERE parent_card_id = ? OR child_card_id = ?
            """,
            (card_id, card_id),
        ).fetchall()
        shared_event_seqs = {
            int(row["event_seq"])
            for row in lineage_rows
            if str(row["parent_card_id"]) != card_id
            or str(row["child_card_id"]) != card_id
        }

        try:
            with self._database.transaction():
                # 카드↔리비전 상호 참조를 먼저 끊어야 FK 제약을 어기지 않는다.
                self._connection.execute(
                    "UPDATE cards SET current_revision_id = NULL WHERE id = ?",
                    (card_id,),
                )
                self._connection.execute(
                    "DELETE FROM drafts WHERE card_id = ?",
                    (card_id,),
                )
                self._connection.execute(
                    """
                    DELETE FROM card_lineage
                    WHERE parent_card_id = ? OR child_card_id = ?
                    """,
                    (card_id, card_id),
                )
                self._clear_card_ui_references(card_id, revision_ids, now_us)
                # parent_revision_id 는 같은 표를 RESTRICT 로 참조하므로 체인을
                # 먼저 끊어야 한 문장 삭제가 FK 에 걸리지 않는다. 부모 체인은
                # 카드 안에서만 이어진다(분할 자식은 parent 가 NULL).
                self._connection.execute(
                    "UPDATE card_revisions SET parent_revision_id = NULL WHERE card_id = ?",
                    (card_id,),
                )
                self._connection.execute(
                    "DELETE FROM card_revisions WHERE card_id = ?",
                    (card_id,),
                )

                redacted_events = self._redact_shared_events(
                    shared_event_seqs,
                    card_id=card_id,
                    body=card.body,
                    revision_ids=revision_ids,
                    redacted_at_us=now_us,
                )
                if shared_event_seqs:
                    placeholders = ",".join("?" for _ in shared_event_seqs)
                    self._connection.execute(
                        f"""
                        DELETE FROM edit_events
                        WHERE card_id = ? AND event_seq NOT IN ({placeholders})
                        """,
                        (card_id, *sorted(shared_event_seqs)),
                    )
                else:
                    self._connection.execute(
                        "DELETE FROM edit_events WHERE card_id = ?",
                        (card_id,),
                    )
                self._connection.execute("DELETE FROM cards WHERE id = ?", (card_id,))
                redacted_operations = self._redact_operation(
                    card.operation_id,
                    redacted_at_us=now_us,
                )
        except BaseException:
            LOGGER.exception("카드 purge에 실패했습니다: %s", card_id)
            raise

        return PurgeResult(
            target_id=card_id,
            target_kind="card",
            purged_card_count=1,
            redacted_event_count=redacted_events,
            redacted_operation_count=redacted_operations,
        )

    def purge_document(
        self,
        document_id: str,
        *,
        retention_days: int,
    ) -> PurgeResult:
        """휴지통 보존 기간이 지난 문서와 소속 그래프 전체를 물리 삭제한다."""
        document = self._repositories.get_document(document_id)
        if document is None:
            raise KeyError(f"존재하지 않는 문서입니다: {document_id}")
        self._require_expired(document.trashed_at_us, retention_days, "문서")
        card_rows = self._connection.execute(
            "SELECT id FROM cards WHERE document_id = ?",
            (document_id,),
        ).fetchall()
        card_ids = tuple(str(row[0]) for row in card_rows)

        try:
            with self._database.transaction():
                self._connection.execute(
                    """
                    UPDATE cards
                    SET current_revision_id = NULL
                    WHERE document_id = ?
                    """,
                    (document_id,),
                )
                self._connection.execute(
                    "DELETE FROM drafts WHERE document_id = ?",
                    (document_id,),
                )
                if card_ids:
                    placeholders = ",".join("?" for _ in card_ids)
                    self._connection.execute(
                        f"""
                        DELETE FROM card_lineage
                        WHERE parent_card_id IN ({placeholders})
                           OR child_card_id IN ({placeholders})
                        """,
                        (*card_ids, *card_ids),
                    )
                # 실제 행 삭제는 뒤에서 하고 여기서는 리비전 FK만 먼저 푼다.
                self._connection.execute(
                    """
                    UPDATE document_ui_states
                    SET selected_card_id = NULL,
                        editor_card_id = NULL,
                        editor_base_revision_id = NULL,
                        updated_at_us = ?
                    WHERE document_id = ?
                    """,
                    (self._clock(), document_id),
                )
                self._connection.execute(
                    """
                    UPDATE card_revisions
                    SET parent_revision_id = NULL
                    WHERE card_id IN (
                        SELECT id FROM cards WHERE document_id = ?
                    )
                    """,
                    (document_id,),
                )
                self._connection.execute(
                    """
                    DELETE FROM card_revisions
                    WHERE card_id IN (
                        SELECT id FROM cards WHERE document_id = ?
                    )
                    """,
                    (document_id,),
                )
                self._connection.execute(
                    "DELETE FROM edit_events WHERE document_id = ?",
                    (document_id,),
                )
                self._connection.execute(
                    "DELETE FROM cards WHERE document_id = ?",
                    (document_id,),
                )
                self._connection.execute(
                    "DELETE FROM capture_operations WHERE document_id = ?",
                    (document_id,),
                )
                self._connection.execute(
                    "DELETE FROM document_ui_states WHERE document_id = ?",
                    (document_id,),
                )
                self._remove_document_from_workspace(document_id)
                self._connection.execute(
                    "DELETE FROM documents WHERE id = ?",
                    (document_id,),
                )
        except BaseException:
            LOGGER.exception("문서 purge에 실패했습니다: %s", document_id)
            raise

        return PurgeResult(
            target_id=document_id,
            target_kind="document",
            purged_card_count=len(card_ids),
            redacted_event_count=0,
            redacted_operation_count=0,
        )

    def _require_expired(
        self,
        deleted_at_us: int | None,
        retention_days: int,
        label: str,
    ) -> None:
        if retention_days < 0:
            raise ValueError("휴지통 보존 기간은 0일 이상이어야 합니다.")
        if deleted_at_us is None:
            raise ValueError(f"휴지통에 있는 {label}만 purge할 수 있습니다.")
        retention_us = retention_days * 24 * 60 * 60 * 1_000_000
        if deleted_at_us > self._clock() - retention_us:
            raise ValueError(f"휴지통 보존 기간이 지나지 않은 {label}입니다.")

    def _clear_card_ui_references(
        self,
        card_id: str,
        revision_ids: tuple[str, ...],
        updated_at_us: int,
    ) -> None:
        self._connection.execute(
            """
            UPDATE document_ui_states
            SET selected_card_id = CASE
                    WHEN selected_card_id = ? THEN NULL ELSE selected_card_id END,
                editor_card_id = CASE
                    WHEN editor_card_id = ? THEN NULL ELSE editor_card_id END,
                updated_at_us = ?
            WHERE selected_card_id = ? OR editor_card_id = ?
            """,
            (card_id, card_id, updated_at_us, card_id, card_id),
        )
        if revision_ids:
            placeholders = ",".join("?" for _ in revision_ids)
            self._connection.execute(
                f"""
                UPDATE document_ui_states
                SET editor_base_revision_id = NULL, updated_at_us = ?
                WHERE editor_base_revision_id IN ({placeholders})
                """,
                (updated_at_us, *revision_ids),
            )

    def _redact_shared_events(
        self,
        event_seqs: set[int],
        *,
        card_id: str,
        body: str,
        revision_ids: tuple[str, ...],
        redacted_at_us: int,
    ) -> int:
        redacted = 0
        for event_seq in sorted(event_seqs):
            row = self._connection.execute(
                """
                SELECT card_id, details_json
                FROM edit_events
                WHERE event_seq = ?
                """,
                (event_seq,),
            ).fetchone()
            if row is None:
                continue
            details = json.loads(str(row["details_json"]))
            scrubbed = self._scrub_json(details, (card_id, body, *revision_ids))
            if not isinstance(scrubbed, dict):
                scrubbed = {"details": scrubbed}
            scrubbed["purge_redacted"] = True
            scrubbed["redacted_at_us"] = redacted_at_us
            self._connection.execute(
                """
                UPDATE edit_events
                SET card_id = CASE WHEN card_id = ? THEN NULL ELSE card_id END,
                    details_json = ?
                WHERE event_seq = ?
                """,
                (
                    card_id,
                    json.dumps(scrubbed, ensure_ascii=False, separators=(",", ":")),
                    event_seq,
                ),
            )
            redacted += 1
        return redacted

    def _redact_operation(self, operation_id: str, *, redacted_at_us: int) -> int:
        row = self._connection.execute(
            """
            SELECT original_text
            FROM capture_operations
            WHERE id = ?
            """,
            (operation_id,),
        ).fetchone()
        if row is None or row["original_text"] is None:
            return 0
        self._connection.execute(
            """
            UPDATE capture_operations
            SET original_text = NULL,
                original_hash = NULL,
                original_redacted_at_us = ?
            WHERE id = ?
            """,
            (redacted_at_us, operation_id),
        )
        return 1

    def _remove_document_from_workspace(self, document_id: str) -> None:
        rows = self._connection.execute(
            """
            SELECT window_id, open_document_ids_json, active_document_id
            FROM workspace_windows
            """
        ).fetchall()
        for row in rows:
            decoded = json.loads(str(row["open_document_ids_json"]))
            active = row["active_document_id"]
            if (
                not isinstance(decoded, list)
                or any(not isinstance(value, str) for value in decoded)
                or len(set(decoded)) != len(decoded)
            ):
                raise ValueError(
                    "workspace_windows의 열린 문서 목록 형식이 잘못되었습니다."
                )
            if active is not None and str(active) not in decoded:
                raise ValueError(
                    "workspace_windows의 활성 문서가 열린 탭에 없습니다."
                )
            remaining = [value for value in decoded if value != document_id]
            self._connection.execute(
                """
                UPDATE workspace_windows
                SET open_document_ids_json = ?,
                    active_document_id = ?,
                    updated_at_us = ?
                WHERE window_id = ?
                """,
                (
                    json.dumps(
                        remaining,
                        ensure_ascii=False,
                        separators=(",", ":"),
                    ),
                    None if active == document_id else active,
                    self._clock(),
                    str(row["window_id"]),
                ),
            )

    @classmethod
    def _scrub_json(cls, value: object, secrets: tuple[str, ...]) -> object:
        if isinstance(value, dict):
            return {
                str(key): cls._scrub_json(item, secrets)
                for key, item in value.items()
            }
        if isinstance(value, list):
            return [cls._scrub_json(item, secrets) for item in value]
        if isinstance(value, str):
            scrubbed = value
            for secret in secrets:
                if secret:
                    scrubbed = scrubbed.replace(secret, REDACTED_MARKER)
            return scrubbed
        return value
