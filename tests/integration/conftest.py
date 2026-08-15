from __future__ import annotations

import json
from collections.abc import Callable
from dataclasses import dataclass, replace

import pytest

from pynote.domain.events import EditEvent, EventSource, EventType
from pynote.domain.models import (
    CaptureOperation,
    CaptureOperationSource,
    Card,
    CardLineage,
    CardRevision,
    CardSource,
    LineageRelationType,
    NewCaptureOperation,
    NewCard,
    RevisionSource,
    SplitPolicy,
)
from pynote.infrastructure.database import Database
from pynote.infrastructure.repositories import Repositories, text_hash


@dataclass(frozen=True, slots=True)
class LegacyTransformationGraph:
    parents: tuple[Card, ...]
    children: tuple[Card, ...]
    transformation_event: EditEvent
    operation_id: str


LegacyGraphFactory = Callable[
    [str, str, LineageRelationType],
    LegacyTransformationGraph,
]


@pytest.fixture
def legacy_graph_factory(
    database: Database,
    repositories: Repositories,
) -> LegacyGraphFactory:
    def seed(
        document_id: str,
        prefix: str,
        relation_type: LineageRelationType,
    ) -> LegacyTransformationGraph:
        if relation_type is LineageRelationType.SPLIT:
            parent_bodies = ("민감 A\n\n민감 B",)
            child_bodies = ("민감 A", "민감 B")
            operation_source = CaptureOperationSource.SPLIT
            card_source = CardSource.SPLIT
            revision_source = RevisionSource.SPLIT
            event_type = EventType.SPLIT
            split_policy = SplitPolicy.SPLIT_BY_BLANK_LINE
            mode = "blank_line"
        else:
            parent_bodies = ("민감 A", "민감 B")
            child_bodies = ("민감 A\n민감 B",)
            operation_source = CaptureOperationSource.MERGE
            card_source = CardSource.MERGE
            revision_source = RevisionSource.MERGE
            event_type = EventType.MERGE
            split_policy = SplitPolicy.KEEP
            mode = "merge"

        parents = repositories.create_cards(
            NewCaptureOperation(
                id=f"{prefix}-source-operation",
                document_id=document_id,
                source=CaptureOperationSource.PASTE,
                split_policy=SplitPolicy.KEEP,
                original_text=None,
                created_at_us=10,
            ),
            [
                NewCard(
                    id=f"{prefix}-parent-{index}",
                    revision_id=f"{prefix}-parent-revision-{index}",
                    event_id=f"{prefix}-parent-event-{index}",
                    position_key=index * 1_024,
                    body=body,
                    card_source=CardSource.PASTE,
                    event_source=EventSource.PASTE,
                    revision_source=RevisionSource.EDIT,
                    created_at_us=10,
                )
                for index, body in enumerate(parent_bodies, start=1)
            ],
        )
        parent_revision_ids = tuple(parent.current_revision_id for parent in parents)
        if any(revision_id is None for revision_id in parent_revision_ids):
            raise RuntimeError("legacy graph 부모 카드의 현재 리비전이 없습니다.")

        operation_id = f"{prefix}-transformation-operation"
        child_ids = tuple(f"{prefix}-child-{index}" for index in range(1, len(child_bodies) + 1))
        details: dict[str, object] = {
            "mode": mode,
            "parent_card_ids": [parent.id for parent in parents],
            "parent_revision_ids": list(parent_revision_ids),
            "child_card_ids": list(child_ids),
            "original_position_keys": [parent.position_key for parent in parents],
            "left_neighbor_id": None,
            "right_neighbor_id": None,
        }
        if relation_type is LineageRelationType.MERGE:
            details["separator"] = "\n"

        with database.transaction():
            repositories.create_capture_operation(
                CaptureOperation(
                    id=operation_id,
                    document_id=document_id,
                    source=operation_source,
                    split_policy=split_policy,
                    original_text=None,
                    original_hash=None,
                    original_redacted_at_us=None,
                    created_at_us=20,
                )
            )
            transformation_event = repositories.create_event(
                EditEvent(
                    event_seq=None,
                    event_id=f"{prefix}-transformation-event",
                    operation_id=operation_id,
                    document_id=document_id,
                    card_id=None,
                    event_type=event_type,
                    source=EventSource.SYSTEM,
                    occurred_at_us=20,
                    details_json=json.dumps(
                        details,
                        ensure_ascii=False,
                        separators=(",", ":"),
                    ),
                )
            )
            if transformation_event.event_seq is None:
                raise RuntimeError("legacy graph 변환 이벤트 순번이 없습니다.")

            for parent in parents:
                if parent.current_revision_id is None:
                    raise RuntimeError("legacy graph 부모 카드의 현재 리비전이 없습니다.")
                repositories.update_card_deleted_state(
                    parent.id,
                    position_key=parent.position_key,
                    deleted_at_us=20,
                    expected_revision_id=parent.current_revision_id,
                )

            children: list[Card] = []
            for index, (card_id, body) in enumerate(
                zip(child_ids, child_bodies, strict=True),
                start=1,
            ):
                event = repositories.create_event(
                    EditEvent(
                        event_seq=None,
                        event_id=f"{prefix}-child-event-{index}",
                        operation_id=operation_id,
                        document_id=document_id,
                        card_id=card_id,
                        event_type=EventType.CREATE,
                        source=EventSource.SYSTEM,
                        occurred_at_us=20,
                        details_json=json.dumps(
                            {
                                "position_key": index * 1_024,
                                "transformation_event_seq": (transformation_event.event_seq),
                                "parent_card_ids": [parent.id for parent in parents],
                            },
                            ensure_ascii=False,
                            separators=(",", ":"),
                        ),
                    )
                )
                if event.event_seq is None:
                    raise RuntimeError("legacy graph 생성 이벤트 순번이 없습니다.")
                sequence_row = database.connection.execute(
                    """
                    UPDATE counters
                    SET next_value = next_value + 1
                    WHERE name = 'capture'
                    RETURNING next_value - 1
                    """
                ).fetchone()
                if sequence_row is None:
                    raise RuntimeError("legacy graph capture 순번 발급에 실패했습니다.")
                revision_id = f"{prefix}-child-revision-{index}"
                body_hash = text_hash(body)
                card = Card(
                    id=card_id,
                    document_id=document_id,
                    operation_id=operation_id,
                    position_key=index * 1_024,
                    capture_seq=int(sequence_row[0]),
                    created_at_us=20,
                    updated_at_us=20,
                    source=card_source,
                    body=body,
                    body_hash=body_hash,
                    current_revision_id=None,
                )
                repositories.create_card(card)
                repositories.create_revision(
                    CardRevision(
                        id=revision_id,
                        card_id=card.id,
                        event_seq=event.event_seq,
                        parent_revision_id=None,
                        body=body,
                        body_hash=body_hash,
                        source=revision_source,
                        created_at_us=20,
                    )
                )
                repositories.link_initial_revision(card.id, revision_id)
                stored = replace(card, current_revision_id=revision_id)
                children.append(stored)
                for parent in parents:
                    repositories.create_lineage(
                        CardLineage(
                            parent_card_id=parent.id,
                            child_card_id=stored.id,
                            event_seq=transformation_event.event_seq,
                            relation_type=relation_type,
                        )
                    )
            repositories.touch_document(document_id, 20)

        return LegacyTransformationGraph(
            parents=tuple(replace(parent, deleted_at_us=20) for parent in parents),
            children=tuple(children),
            transformation_event=transformation_event,
            operation_id=operation_id,
        )

    return seed
