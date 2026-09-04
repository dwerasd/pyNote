from __future__ import annotations

from dataclasses import dataclass
from enum import StrEnum

from pynote.domain.events import EventSource


class CaptureOperationSource(StrEnum):
    TYPING = "typing"
    PASTE = "paste"
    IMPORT = "import"
    MIXED = "mixed"
    SPLIT = "split"
    MERGE = "merge"
    SYSTEM = "system"


class SplitPolicy(StrEnum):
    KEEP = "keep"
    SPLIT_BY_BLANK_LINE = "split_by_blank_line"


class CardSource(StrEnum):
    TYPING = "typing"
    PASTE = "paste"
    IMPORT = "import"
    MIXED = "mixed"
    RESTORE = "restore"
    SPLIT = "split"
    MERGE = "merge"
    SYSTEM = "system"


class RevisionSource(StrEnum):
    EDIT = "edit"
    RESTORE = "restore"
    SPLIT = "split"
    MERGE = "merge"


class DraftKind(StrEnum):
    NEW = "new"
    EDIT = "edit"


class LineageRelationType(StrEnum):
    SPLIT = "split"
    MERGE = "merge"


class NewlineKind(StrEnum):
    """결속 파일에 되쓸 줄바꿈 형식이다."""

    LF = "lf"
    CRLF = "crlf"
    CR = "cr"

    @property
    def characters(self) -> str:
        """파일에 기록할 줄바꿈 문자를 반환한다."""
        if self is NewlineKind.CRLF:
            return "\r\n"
        return "\r" if self is NewlineKind.CR else "\n"


@dataclass(frozen=True, slots=True)
class Document:
    id: str
    title: str
    created_at_us: int
    updated_at_us: int
    archived_at_us: int | None = None
    trashed_at_us: int | None = None


@dataclass(frozen=True, slots=True)
class CaptureOperation:
    id: str
    document_id: str
    source: CaptureOperationSource
    split_policy: SplitPolicy
    original_text: str | None
    original_hash: str | None
    original_redacted_at_us: int | None
    created_at_us: int


@dataclass(frozen=True, slots=True)
class Card:
    id: str
    document_id: str
    operation_id: str
    position_key: int
    capture_seq: int
    created_at_us: int
    updated_at_us: int
    source: CardSource
    body: str
    body_hash: str
    current_revision_id: str | None
    deleted_at_us: int | None = None


@dataclass(frozen=True, slots=True)
class CardRevision:
    id: str
    card_id: str
    event_seq: int
    parent_revision_id: str | None
    body: str
    body_hash: str
    source: RevisionSource
    created_at_us: int


@dataclass(frozen=True, slots=True)
class Draft:
    id: str
    document_id: str
    card_id: str | None
    draft_kind: DraftKind
    base_revision_id: str | None
    draft_text: str
    draft_hash: str
    cursor_position_qchar: int
    updated_at_us: int


@dataclass(frozen=True, slots=True)
class FileBinding:
    """카드 한 장과 디스크 파일 한 개의 결속 상태다."""

    card_id: str
    path: str
    path_key: str
    encoding: str
    bom: bool
    newline: NewlineKind
    trailing_newline: bool
    bound_at_us: int
    synced_size: int | None = None
    synced_mtime_ns: int | None = None
    synced_hash: str | None = None
    synced_at_us: int | None = None


@dataclass(frozen=True, slots=True)
class CardLineage:
    parent_card_id: str
    child_card_id: str
    event_seq: int
    relation_type: LineageRelationType


@dataclass(frozen=True, slots=True)
class NewCaptureOperation:
    id: str
    document_id: str
    source: CaptureOperationSource
    split_policy: SplitPolicy
    original_text: str | None
    created_at_us: int


@dataclass(frozen=True, slots=True)
class NewCard:
    id: str
    revision_id: str
    event_id: str
    position_key: int
    body: str
    card_source: CardSource
    event_source: EventSource
    revision_source: RevisionSource
    created_at_us: int
    event_details_json: str = "{}"
