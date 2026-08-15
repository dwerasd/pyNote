from __future__ import annotations

import hashlib
import json
import sqlite3
import time
from collections.abc import Sequence
from dataclasses import dataclass

from pynote.domain.events import EditEvent, EventSource, EventType
from pynote.domain.models import (
    CaptureOperation,
    CaptureOperationSource,
    Card,
    CardLineage,
    CardRevision,
    CardSource,
    Document,
    Draft,
    DraftKind,
    LineageRelationType,
    NewCaptureOperation,
    NewCard,
    RevisionSource,
    SplitPolicy,
)
from pynote.infrastructure.database import Database


def text_hash(text: str) -> str:
    """문자열의 UTF-8 SHA-256 해시를 반환한다."""
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


class CardCompareAndSwapError(RuntimeError):
    """카드가 예상한 현재 리비전에서 이미 변경됐음을 나타낸다."""


@dataclass(frozen=True, slots=True)
class WorkspaceWindow:
    """복원 대상 창 한 개의 탭 순서와 활성 문서를 나타낸다."""

    window_id: str
    open_document_ids: tuple[str, ...]
    active_document_id: str | None
    updated_at_us: int


class Repositories:
    """동일 SQLite 연결 위에서 도메인 행과 원자적 저장 흐름을 제공한다."""

    def __init__(self, database: Database) -> None:
        self._database = database
        self._connection = database.connection

    @property
    def database(self) -> Database:
        """이 저장소 묶음이 사용하는 DB 수명 주기를 반환한다."""
        return self._database

    def list_workspace_windows(self) -> tuple[WorkspaceWindow, ...]:
        """복원 대상 창을 저장 순서대로 읽고 탭 JSON 계약을 검증한다."""
        rows = self._connection.execute(
            """
            SELECT window_id, open_document_ids_json,
                   active_document_id, updated_at_us
            FROM workspace_windows
            ORDER BY updated_at_us, window_id
            """
        ).fetchall()
        return tuple(self._workspace_window_from_row(row) for row in rows)

    def get_workspace_window(self, window_id: str) -> WorkspaceWindow | None:
        """지정 창의 작업 상태를 읽는다."""
        row = self._connection.execute(
            """
            SELECT window_id, open_document_ids_json,
                   active_document_id, updated_at_us
            FROM workspace_windows
            WHERE window_id = ?
            """,
            (window_id,),
        ).fetchone()
        return None if row is None else self._workspace_window_from_row(row)

    def save_workspace_window(
        self,
        window_id: str,
        open_document_ids: tuple[str, ...],
        active_document_id: str | None,
    ) -> WorkspaceWindow:
        """창별 탭 상태를 검증하고 생성하거나 갱신한다."""
        if not window_id:
            raise ValueError("window_id는 비어 있을 수 없습니다.")
        if len(set(open_document_ids)) != len(open_document_ids):
            raise ValueError("한 창에는 같은 문서 탭을 두 번 저장할 수 없습니다.")
        if active_document_id is not None and active_document_id not in open_document_ids:
            raise ValueError("활성 문서는 해당 창의 열린 탭 목록에 있어야 합니다.")
        updated_at_us = time.time_ns() // 1_000
        encoded_ids = json.dumps(
            open_document_ids,
            ensure_ascii=False,
            separators=(",", ":"),
        )
        with self._database.transaction():
            self._connection.execute(
                """
                INSERT INTO workspace_windows(
                    window_id, open_document_ids_json,
                    active_document_id, updated_at_us
                )
                VALUES (?, ?, ?, ?)
                ON CONFLICT(window_id) DO UPDATE SET
                    open_document_ids_json = excluded.open_document_ids_json,
                    active_document_id = excluded.active_document_id,
                    updated_at_us = excluded.updated_at_us
                """,
                (window_id, encoded_ids, active_document_id, updated_at_us),
            )
        return WorkspaceWindow(
            window_id=window_id,
            open_document_ids=open_document_ids,
            active_document_id=active_document_id,
            updated_at_us=updated_at_us,
        )

    def delete_workspace_window(self, window_id: str) -> None:
        """명시적으로 닫힌 비마지막 창의 복원 행을 삭제한다."""
        with self._database.transaction():
            self._connection.execute(
                "DELETE FROM workspace_windows WHERE window_id = ?",
                (window_id,),
            )

    @staticmethod
    def _workspace_window_from_row(row: sqlite3.Row) -> WorkspaceWindow:
        try:
            decoded = json.loads(str(row["open_document_ids_json"]))
        except (TypeError, json.JSONDecodeError):
            raise ValueError("workspace_windows 탭 JSON을 읽지 못했습니다.") from None
        if (
            not isinstance(decoded, list)
            or any(not isinstance(value, str) for value in decoded)
            or len(set(decoded)) != len(decoded)
        ):
            raise ValueError("workspace_windows의 열린 문서 목록 형식이 잘못되었습니다.")
        active_value = row["active_document_id"]
        active_document_id = None if active_value is None else str(active_value)
        if active_document_id is not None and active_document_id not in decoded:
            raise ValueError("workspace_windows의 활성 문서가 열린 탭에 없습니다.")
        return WorkspaceWindow(
            window_id=str(row["window_id"]),
            open_document_ids=tuple(decoded),
            active_document_id=active_document_id,
            updated_at_us=int(row["updated_at_us"]),
        )

    def create_document(self, document: Document) -> None:
        self._connection.execute(
            """
            INSERT INTO documents(
                id, title, created_at_us, updated_at_us,
                archived_at_us, trashed_at_us
            )
            VALUES (?, ?, ?, ?, ?, ?)
            """,
            (
                document.id,
                document.title,
                document.created_at_us,
                document.updated_at_us,
                document.archived_at_us,
                document.trashed_at_us,
            ),
        )

    def get_document(self, document_id: str) -> Document | None:
        row = self._connection.execute(
            "SELECT * FROM documents WHERE id = ?",
            (document_id,),
        ).fetchone()
        return None if row is None else self._document_from_row(row)

    def list_documents(self) -> tuple[Document, ...]:
        rows = self._connection.execute(
            "SELECT * FROM documents ORDER BY created_at_us, id"
        ).fetchall()
        return tuple(self._document_from_row(row) for row in rows)

    def search_documents(self, query: str) -> tuple[Document, ...]:
        """제목 또는 활성 카드 본문에 LIKE로 일치하는 문서를 반환한다."""
        normalized = query.strip()
        if not normalized:
            return ()
        escaped = normalized.replace("/", "//").replace("%", "/%").replace("_", "/_")
        pattern = f"%{escaped}%"
        rows = self._connection.execute(
            """
            SELECT DISTINCT documents.*
            FROM documents
            WHERE documents.trashed_at_us IS NULL
              AND (
                  documents.title LIKE ? ESCAPE '/'
                  OR EXISTS (
                      SELECT 1
                      FROM cards
                      WHERE cards.document_id = documents.id
                        AND cards.deleted_at_us IS NULL
                        AND cards.body LIKE ? ESCAPE '/'
                  )
              )
            ORDER BY documents.updated_at_us DESC, documents.id
            """,
            (pattern, pattern),
        ).fetchall()
        return tuple(self._document_from_row(row) for row in rows)

    def search_cards(
        self,
        query: str,
        *,
        document_id: str | None = None,
    ) -> tuple[Card, ...]:
        """활성 카드 본문을 LIKE로 검색하고 위치순으로 반환한다."""
        normalized = query.strip()
        if not normalized:
            return ()
        escaped = normalized.replace("/", "//").replace("%", "/%").replace("_", "/_")
        pattern = f"%{escaped}%"
        if document_id is None:
            rows = self._connection.execute(
                """
                SELECT cards.*
                FROM cards
                JOIN documents ON documents.id = cards.document_id
                WHERE cards.deleted_at_us IS NULL
                  AND documents.trashed_at_us IS NULL
                  AND cards.body LIKE ? ESCAPE '/'
                ORDER BY cards.document_id, cards.position_key, cards.id
                """,
                (pattern,),
            ).fetchall()
        else:
            rows = self._connection.execute(
                """
                SELECT *
                FROM cards
                WHERE document_id = ?
                  AND deleted_at_us IS NULL
                  AND body LIKE ? ESCAPE '/'
                ORDER BY position_key, id
                """,
                (document_id, pattern),
            ).fetchall()
        return tuple(self._card_from_row(row) for row in rows)

    def operation_reconstruction_available(self, card_id: str) -> bool:
        """카드의 입력 작업 원문이 purge로 소거되지 않았는지 반환한다."""
        row = self._connection.execute(
            """
            SELECT capture_operations.original_redacted_at_us
            FROM cards
            JOIN capture_operations
              ON capture_operations.id = cards.operation_id
            WHERE cards.id = ?
            """,
            (card_id,),
        ).fetchone()
        if row is None:
            raise KeyError(f"존재하지 않는 카드입니다: {card_id}")
        return row["original_redacted_at_us"] is None

    def update_document(self, document: Document) -> None:
        self._connection.execute(
            """
            UPDATE documents
            SET title = ?, created_at_us = ?, updated_at_us = ?,
                archived_at_us = ?, trashed_at_us = ?
            WHERE id = ?
            """,
            (
                document.title,
                document.created_at_us,
                document.updated_at_us,
                document.archived_at_us,
                document.trashed_at_us,
                document.id,
            ),
        )

    def touch_document(self, document_id: str, updated_at_us: int) -> None:
        """카드 확정 작업 시 문서의 최근 수정 시각을 전진시킨다."""
        self._connection.execute(
            """
            UPDATE documents
            SET updated_at_us = MAX(updated_at_us, ?)
            WHERE id = ?
            """,
            (updated_at_us, document_id),
        )

    def delete_document(self, document_id: str) -> None:
        self._connection.execute("DELETE FROM documents WHERE id = ?", (document_id,))

    def create_capture_operation(self, operation: CaptureOperation) -> None:
        self._connection.execute(
            """
            INSERT INTO capture_operations(
                id, document_id, source, split_policy, original_text,
                original_hash, original_redacted_at_us, created_at_us
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                operation.id,
                operation.document_id,
                operation.source.value,
                operation.split_policy.value,
                operation.original_text,
                operation.original_hash,
                operation.original_redacted_at_us,
                operation.created_at_us,
            ),
        )

    def get_capture_operation(self, operation_id: str) -> CaptureOperation | None:
        row = self._connection.execute(
            "SELECT * FROM capture_operations WHERE id = ?",
            (operation_id,),
        ).fetchone()
        return None if row is None else self._capture_operation_from_row(row)

    def update_capture_operation(self, operation: CaptureOperation) -> None:
        self._connection.execute(
            """
            UPDATE capture_operations
            SET document_id = ?, source = ?, split_policy = ?,
                original_text = ?, original_hash = ?,
                original_redacted_at_us = ?, created_at_us = ?
            WHERE id = ?
            """,
            (
                operation.document_id,
                operation.source.value,
                operation.split_policy.value,
                operation.original_text,
                operation.original_hash,
                operation.original_redacted_at_us,
                operation.created_at_us,
                operation.id,
            ),
        )

    def delete_capture_operation(self, operation_id: str) -> None:
        self._connection.execute(
            "DELETE FROM capture_operations WHERE id = ?",
            (operation_id,),
        )

    def create_card(self, card: Card) -> None:
        self._connection.execute(
            """
            INSERT INTO cards(
                id, document_id, operation_id, position_key, capture_seq,
                created_at_us, updated_at_us, source, body, body_hash,
                current_revision_id, deleted_at_us
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                card.id,
                card.document_id,
                card.operation_id,
                card.position_key,
                card.capture_seq,
                card.created_at_us,
                card.updated_at_us,
                card.source.value,
                card.body,
                card.body_hash,
                card.current_revision_id,
                card.deleted_at_us,
            ),
        )

    def get_card(self, card_id: str) -> Card | None:
        row = self._connection.execute(
            "SELECT * FROM cards WHERE id = ?",
            (card_id,),
        ).fetchone()
        return None if row is None else self._card_from_row(row)

    def list_cards(self, document_id: str) -> tuple[Card, ...]:
        rows = self._connection.execute(
            """
            SELECT *
            FROM cards
            WHERE document_id = ?
            ORDER BY position_key, id
            """,
            (document_id,),
        ).fetchall()
        return tuple(self._card_from_row(row) for row in rows)

    def link_initial_revision(self, card_id: str, revision_id: str) -> None:
        """생성 중인 카드의 NULL 현재 리비전을 최초 리비전으로 연결한다."""
        cursor = self._connection.execute(
            """
            UPDATE cards
            SET current_revision_id = ?
            WHERE id = ? AND current_revision_id IS NULL
            """,
            (revision_id, card_id),
        )
        self._require_card_cas(cursor, card_id)

    def advance_card_revision(
        self,
        card: Card,
        *,
        expected_revision_id: str,
    ) -> None:
        """예상 현재 리비전일 때만 본문과 새 현재 리비전을 함께 전진시킨다."""
        cursor = self._connection.execute(
            """
            UPDATE cards
            SET updated_at_us = ?, source = ?, body = ?, body_hash = ?,
                current_revision_id = ?
            WHERE id = ? AND current_revision_id = ?
            """,
            (
                card.updated_at_us,
                card.source.value,
                card.body,
                card.body_hash,
                card.current_revision_id,
                card.id,
                expected_revision_id,
            ),
        )
        self._require_card_cas(cursor, card.id)

    def update_card_position(
        self,
        card_id: str,
        position_key: int,
        *,
        expected_revision_id: str,
    ) -> None:
        """예상 현재 리비전일 때만 카드 위치를 변경한다."""
        cursor = self._connection.execute(
            """
            UPDATE cards
            SET position_key = ?
            WHERE id = ? AND current_revision_id = ?
            """,
            (position_key, card_id, expected_revision_id),
        )
        self._require_card_cas(cursor, card_id)

    def update_card_deleted_state(
        self,
        card_id: str,
        *,
        position_key: int,
        deleted_at_us: int | None,
        expected_revision_id: str,
    ) -> None:
        """예상 현재 리비전일 때만 카드 위치와 휴지통 상태를 변경한다."""
        cursor = self._connection.execute(
            """
            UPDATE cards
            SET position_key = ?, deleted_at_us = ?
            WHERE id = ? AND current_revision_id = ?
            """,
            (position_key, deleted_at_us, card_id, expected_revision_id),
        )
        self._require_card_cas(cursor, card_id)

    def delete_card(self, card_id: str) -> None:
        self._connection.execute("DELETE FROM cards WHERE id = ?", (card_id,))

    def create_revision(self, revision: CardRevision) -> None:
        self._connection.execute(
            """
            INSERT INTO card_revisions(
                id, card_id, event_seq, parent_revision_id,
                body, body_hash, source, created_at_us
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                revision.id,
                revision.card_id,
                revision.event_seq,
                revision.parent_revision_id,
                revision.body,
                revision.body_hash,
                revision.source.value,
                revision.created_at_us,
            ),
        )

    def get_revision(self, revision_id: str) -> CardRevision | None:
        row = self._connection.execute(
            "SELECT * FROM card_revisions WHERE id = ?",
            (revision_id,),
        ).fetchone()
        return None if row is None else self._revision_from_row(row)

    def list_revisions(self, card_id: str) -> tuple[CardRevision, ...]:
        rows = self._connection.execute(
            """
            SELECT *
            FROM card_revisions
            WHERE card_id = ?
            ORDER BY event_seq, id
            """,
            (card_id,),
        ).fetchall()
        return tuple(self._revision_from_row(row) for row in rows)

    def delete_revision_for_purge(self, revision_id: str) -> None:
        self._connection.execute(
            "DELETE FROM card_revisions WHERE id = ?",
            (revision_id,),
        )

    def create_draft(self, draft: Draft) -> None:
        self._connection.execute(
            """
            INSERT INTO drafts(
                id, document_id, card_id, draft_kind, base_revision_id,
                draft_text, draft_hash, cursor_position_qchar, updated_at_us
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                draft.id,
                draft.document_id,
                draft.card_id,
                draft.draft_kind.value,
                draft.base_revision_id,
                draft.draft_text,
                draft.draft_hash,
                draft.cursor_position_qchar,
                draft.updated_at_us,
            ),
        )

    def get_draft(self, draft_id: str) -> Draft | None:
        row = self._connection.execute(
            "SELECT * FROM drafts WHERE id = ?",
            (draft_id,),
        ).fetchone()
        return None if row is None else self._draft_from_row(row)

    def list_drafts(self, document_id: str) -> tuple[Draft, ...]:
        rows = self._connection.execute(
            """
            SELECT *
            FROM drafts
            WHERE document_id = ?
            ORDER BY updated_at_us, id
            """,
            (document_id,),
        ).fetchall()
        return tuple(self._draft_from_row(row) for row in rows)

    def update_draft(self, draft: Draft) -> None:
        self._connection.execute(
            """
            UPDATE drafts
            SET document_id = ?, card_id = ?, draft_kind = ?,
                base_revision_id = ?, draft_text = ?, draft_hash = ?,
                cursor_position_qchar = ?, updated_at_us = ?
            WHERE id = ?
            """,
            (
                draft.document_id,
                draft.card_id,
                draft.draft_kind.value,
                draft.base_revision_id,
                draft.draft_text,
                draft.draft_hash,
                draft.cursor_position_qchar,
                draft.updated_at_us,
                draft.id,
            ),
        )

    def delete_draft(self, draft_id: str) -> None:
        self._connection.execute("DELETE FROM drafts WHERE id = ?", (draft_id,))

    def create_event(self, event: EditEvent) -> EditEvent:
        if event.event_seq is not None:
            raise ValueError("새 이벤트의 event_seq는 SQLite가 발급해야 합니다.")
        cursor = self._connection.execute(
            """
            INSERT INTO edit_events(
                event_id, operation_id, document_id, card_id,
                event_type, source, occurred_at_us, details_json
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                event.event_id,
                event.operation_id,
                event.document_id,
                event.card_id,
                event.event_type.value,
                event.source.value,
                event.occurred_at_us,
                event.details_json,
            ),
        )
        if cursor.lastrowid is None:
            raise RuntimeError("event_seq 발급에 실패했습니다.")
        return EditEvent(
            event_seq=int(cursor.lastrowid),
            event_id=event.event_id,
            operation_id=event.operation_id,
            document_id=event.document_id,
            card_id=event.card_id,
            event_type=event.event_type,
            source=event.source,
            occurred_at_us=event.occurred_at_us,
            details_json=event.details_json,
        )

    def get_event(self, event_seq: int) -> EditEvent | None:
        row = self._connection.execute(
            "SELECT * FROM edit_events WHERE event_seq = ?",
            (event_seq,),
        ).fetchone()
        return None if row is None else self._event_from_row(row)

    def list_events(self, document_id: str) -> tuple[EditEvent, ...]:
        rows = self._connection.execute(
            """
            SELECT *
            FROM edit_events
            WHERE document_id = ?
            ORDER BY event_seq
            """,
            (document_id,),
        ).fetchall()
        return tuple(self._event_from_row(row) for row in rows)

    def delete_event_for_purge(self, event_seq: int) -> None:
        self._connection.execute(
            "DELETE FROM edit_events WHERE event_seq = ?",
            (event_seq,),
        )

    def get_counter(self, name: str) -> int | None:
        row = self._connection.execute(
            "SELECT next_value FROM counters WHERE name = ?",
            (name,),
        ).fetchone()
        return None if row is None else int(row[0])

    def _issue_capture_sequence(self) -> int:
        """활성 카드 생성 트랜잭션 안에서 capture 순번 하나를 발급한다."""
        if not self._connection.in_transaction:
            raise RuntimeError("capture 순번은 카드 생성 트랜잭션 안에서만 발급합니다.")
        row = self._connection.execute(
            """
            UPDATE counters
            SET next_value = next_value + 1
            WHERE name = 'capture'
            RETURNING next_value - 1
            """,
        ).fetchone()
        if row is None:
            raise KeyError("capture counter가 존재하지 않습니다.")
        return int(row[0])

    def create_lineage(self, lineage: CardLineage) -> None:
        self._connection.execute(
            """
            INSERT INTO card_lineage(
                parent_card_id, child_card_id, event_seq, relation_type
            )
            VALUES (?, ?, ?, ?)
            """,
            (
                lineage.parent_card_id,
                lineage.child_card_id,
                lineage.event_seq,
                lineage.relation_type.value,
            ),
        )

    def list_lineage_for_card(self, card_id: str) -> tuple[CardLineage, ...]:
        rows = self._connection.execute(
            """
            SELECT *
            FROM card_lineage
            WHERE parent_card_id = ? OR child_card_id = ?
            ORDER BY event_seq, parent_card_id, child_card_id
            """,
            (card_id, card_id),
        ).fetchall()
        return tuple(self._lineage_from_row(row) for row in rows)

    def delete_lineage(self, lineage: CardLineage) -> None:
        self._connection.execute(
            """
            DELETE FROM card_lineage
            WHERE parent_card_id = ? AND child_card_id = ? AND event_seq = ?
            """,
            (lineage.parent_card_id, lineage.child_card_id, lineage.event_seq),
        )

    def create_cards(
        self,
        operation: NewCaptureOperation,
        cards: Sequence[NewCard],
    ) -> tuple[Card, ...]:
        """operation과 새 카드들의 이벤트·순번·리비전을 원자적으로 저장한다."""
        if not cards:
            raise ValueError("새 카드 저장에는 카드가 한 개 이상 필요합니다.")
        existing_card_transform = operation.source in {
            CaptureOperationSource.SPLIT,
            CaptureOperationSource.MERGE,
        }
        if existing_card_transform and operation.original_text is not None:
            raise ValueError("기존 카드 분할·병합 작업은 원문을 중복 저장하지 않습니다.")
        if operation.split_policy is SplitPolicy.KEEP and operation.original_text is not None:
            raise ValueError("keep 작업은 카드 본문과 같은 원문을 중복 저장하지 않습니다.")
        if (
            not existing_card_transform
            and operation.split_policy is SplitPolicy.SPLIT_BY_BLANK_LINE
            and operation.original_text is None
        ):
            raise ValueError("새 입력 분할 작업은 정확한 원문이 필요합니다.")

        original_hash = (
            None if operation.original_text is None else text_hash(operation.original_text)
        )
        stored_operation = CaptureOperation(
            id=operation.id,
            document_id=operation.document_id,
            source=operation.source,
            split_policy=operation.split_policy,
            original_text=operation.original_text,
            original_hash=original_hash,
            original_redacted_at_us=None,
            created_at_us=operation.created_at_us,
        )
        created_cards: list[Card] = []

        with self._database.transaction():
            self.create_capture_operation(stored_operation)
            for new_card in cards:
                event = self.create_event(
                    EditEvent(
                        event_seq=None,
                        event_id=new_card.event_id,
                        operation_id=operation.id,
                        document_id=operation.document_id,
                        card_id=new_card.id,
                        event_type=EventType.CREATE,
                        source=new_card.event_source,
                        occurred_at_us=new_card.created_at_us,
                        details_json=new_card.event_details_json,
                    )
                )
                if event.event_seq is None:
                    raise RuntimeError("create 이벤트의 event_seq가 없습니다.")

                capture_seq = self._issue_capture_sequence()
                body_hash = text_hash(new_card.body)
                card = Card(
                    id=new_card.id,
                    document_id=operation.document_id,
                    operation_id=operation.id,
                    position_key=new_card.position_key,
                    capture_seq=capture_seq,
                    created_at_us=new_card.created_at_us,
                    updated_at_us=new_card.created_at_us,
                    source=new_card.card_source,
                    body=new_card.body,
                    body_hash=body_hash,
                    current_revision_id=None,
                )
                self.create_card(card)
                self.create_revision(
                    CardRevision(
                        id=new_card.revision_id,
                        card_id=new_card.id,
                        event_seq=event.event_seq,
                        parent_revision_id=None,
                        body=new_card.body,
                        body_hash=body_hash,
                        source=new_card.revision_source,
                        created_at_us=new_card.created_at_us,
                    )
                )
                self.link_initial_revision(new_card.id, new_card.revision_id)
                created_cards.append(
                    Card(
                        id=card.id,
                        document_id=card.document_id,
                        operation_id=card.operation_id,
                        position_key=card.position_key,
                        capture_seq=card.capture_seq,
                        created_at_us=card.created_at_us,
                        updated_at_us=card.updated_at_us,
                        source=card.source,
                        body=card.body,
                        body_hash=card.body_hash,
                        current_revision_id=new_card.revision_id,
                    )
                )

        return tuple(created_cards)

    @staticmethod
    def _require_card_cas(cursor: sqlite3.Cursor, card_id: str) -> None:
        if cursor.rowcount != 1:
            raise CardCompareAndSwapError(
                f"카드가 예상한 현재 리비전에서 변경되었습니다: {card_id}"
            )

    @staticmethod
    def _document_from_row(row: sqlite3.Row) -> Document:
        return Document(
            id=str(row["id"]),
            title=str(row["title"]),
            created_at_us=int(row["created_at_us"]),
            updated_at_us=int(row["updated_at_us"]),
            archived_at_us=row["archived_at_us"],
            trashed_at_us=row["trashed_at_us"],
        )

    @staticmethod
    def _capture_operation_from_row(row: sqlite3.Row) -> CaptureOperation:
        return CaptureOperation(
            id=str(row["id"]),
            document_id=str(row["document_id"]),
            source=CaptureOperationSource(row["source"]),
            split_policy=SplitPolicy(row["split_policy"]),
            original_text=row["original_text"],
            original_hash=row["original_hash"],
            original_redacted_at_us=row["original_redacted_at_us"],
            created_at_us=int(row["created_at_us"]),
        )

    @staticmethod
    def _card_from_row(row: sqlite3.Row) -> Card:
        return Card(
            id=str(row["id"]),
            document_id=str(row["document_id"]),
            operation_id=str(row["operation_id"]),
            position_key=int(row["position_key"]),
            capture_seq=int(row["capture_seq"]),
            created_at_us=int(row["created_at_us"]),
            updated_at_us=int(row["updated_at_us"]),
            source=CardSource(row["source"]),
            body=str(row["body"]),
            body_hash=str(row["body_hash"]),
            current_revision_id=row["current_revision_id"],
            deleted_at_us=row["deleted_at_us"],
        )

    @staticmethod
    def _revision_from_row(row: sqlite3.Row) -> CardRevision:
        return CardRevision(
            id=str(row["id"]),
            card_id=str(row["card_id"]),
            event_seq=int(row["event_seq"]),
            parent_revision_id=row["parent_revision_id"],
            body=str(row["body"]),
            body_hash=str(row["body_hash"]),
            source=RevisionSource(row["source"]),
            created_at_us=int(row["created_at_us"]),
        )

    @staticmethod
    def _draft_from_row(row: sqlite3.Row) -> Draft:
        return Draft(
            id=str(row["id"]),
            document_id=str(row["document_id"]),
            card_id=row["card_id"],
            draft_kind=DraftKind(row["draft_kind"]),
            base_revision_id=row["base_revision_id"],
            draft_text=str(row["draft_text"]),
            draft_hash=str(row["draft_hash"]),
            cursor_position_qchar=int(row["cursor_position_qchar"]),
            updated_at_us=int(row["updated_at_us"]),
        )

    @staticmethod
    def _event_from_row(row: sqlite3.Row) -> EditEvent:
        return EditEvent(
            event_seq=int(row["event_seq"]),
            event_id=str(row["event_id"]),
            operation_id=row["operation_id"],
            document_id=str(row["document_id"]),
            card_id=row["card_id"],
            event_type=EventType(row["event_type"]),
            source=EventSource(row["source"]),
            occurred_at_us=int(row["occurred_at_us"]),
            details_json=str(row["details_json"]),
        )

    @staticmethod
    def _lineage_from_row(row: sqlite3.Row) -> CardLineage:
        return CardLineage(
            parent_card_id=str(row["parent_card_id"]),
            child_card_id=str(row["child_card_id"]),
            event_seq=int(row["event_seq"]),
            relation_type=LineageRelationType(row["relation_type"]),
        )
