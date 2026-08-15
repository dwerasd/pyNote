from __future__ import annotations

from dataclasses import dataclass
from enum import StrEnum


class EventType(StrEnum):
    CREATE = "create"
    UPDATE = "update"
    MOVE = "move"
    SPLIT = "split"
    MERGE = "merge"
    DELETE = "delete"
    RESTORE = "restore"


class EventSource(StrEnum):
    TYPING = "typing"
    PASTE = "paste"
    IMPORT = "import"
    MIXED = "mixed"
    EDIT = "edit"
    RESTORE = "restore"
    SYSTEM = "system"


@dataclass(frozen=True, slots=True)
class EditEvent:
    event_seq: int | None
    event_id: str
    operation_id: str | None
    document_id: str
    card_id: str | None
    event_type: EventType
    source: EventSource
    occurred_at_us: int
    details_json: str
