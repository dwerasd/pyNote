"""결함 표본: 인자가 둘인 호출식 파라미터(min(...)).

good.py 와 같되 created_at_us 자리를 min(card.created_at_us, card.updated_at_us) 로 바꿨다.
단일 인자 int(...) 래퍼만 투명하게 벗기는 규칙이 다른 호출로 넓어지지 않도록 고정한다 -
이 표본은 여전히 "사상 실패" 로 거부돼야 한다. 실행 대상이 아니라 게이트가 읽는 표본이다.
"""

from __future__ import annotations

import sqlite3
from dataclasses import dataclass
from enum import StrEnum


class CardSource(StrEnum):
    CAPTURE = "capture"
    EDIT = "edit"


@dataclass(frozen=True, slots=True)
class Card:
    id: str
    document_id: str
    created_at_us: int
    updated_at_us: int
    source: CardSource
    body: str
    body_hash: str
    deleted_at_us: int | None


class Sample:
    def __init__(self, connection: sqlite3.Connection) -> None:
        self._connection = connection

    def list_cards(self) -> None:
        self._connection.execute(
            """
            SELECT *
            FROM cards
            ORDER BY position_key, id
            """
        )

    def create_card(self, card: Card) -> None:
        self._connection.execute(
            """
            INSERT INTO cards(
                id, document_id, created_at_us, updated_at_us,
                source, body, body_hash, deleted_at_us
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                card.id,
                card.document_id,
                min(card.created_at_us, card.updated_at_us),
                card.updated_at_us,
                card.source.value,
                card.body,
                card.body_hash,
                card.deleted_at_us,
            ),
        )

    def stamp_version(self, applied_at_us: int) -> None:
        self._connection.execute(
            """
            UPDATE schema_version
            SET version = 1, applied_at_us = ?
            WHERE id = 1
            """,
            (applied_at_us,),
        )
