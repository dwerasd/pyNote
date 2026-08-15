from __future__ import annotations

from collections.abc import Sequence
from enum import StrEnum
from pathlib import Path

from pynote.domain.models import Card
from pynote.infrastructure.repositories import Repositories


class NewlineFormat(StrEnum):
    """내보내기 파일의 줄바꿈 형식이다."""

    LF = "lf"
    CRLF = "crlf"

    @property
    def characters(self) -> str:
        """파일에 기록할 줄바꿈 문자를 반환한다."""
        return "\n" if self is NewlineFormat.LF else "\r\n"


def render_cards(
    cards: Sequence[Card],
    *,
    newline: NewlineFormat = NewlineFormat.LF,
) -> str:
    """활성 카드의 현재 본문을 위치순으로 직렬화한다."""
    ordered = sorted(
        (card for card in cards if card.deleted_at_us is None),
        key=lambda card: (card.position_key, card.id),
    )
    separator = newline.characters * 2
    return separator.join(_convert_newlines(card.body, newline.characters) for card in ordered)


def export_cards(
    path: Path,
    cards: Sequence[Card],
    *,
    newline: NewlineFormat = NewlineFormat.LF,
) -> None:
    """현재 카드 본문을 UTF-8 TXT 또는 Markdown 파일로 기록한다."""
    _require_text_suffix(path)
    content = render_cards(cards, newline=newline)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as output:
        output.write(content)


def export_document(
    path: Path,
    repositories: Repositories,
    document_id: str,
    *,
    newline: NewlineFormat = NewlineFormat.LF,
) -> None:
    """문서의 현재 활성 카드를 위치순으로 내보낸다."""
    if repositories.get_document(document_id) is None:
        raise KeyError(f"존재하지 않는 문서입니다: {document_id}")
    export_cards(
        path,
        repositories.list_cards(document_id),
        newline=newline,
    )


def _convert_newlines(text: str, newline: str) -> str:
    normalized = text.replace("\r\n", "\n").replace("\r", "\n")
    return normalized.replace("\n", newline)


def _require_text_suffix(path: Path) -> None:
    if path.suffix.lower() not in {".txt", ".md"}:
        raise ValueError("TXT 또는 Markdown(.txt, .md) 파일만 내보낼 수 있습니다.")
