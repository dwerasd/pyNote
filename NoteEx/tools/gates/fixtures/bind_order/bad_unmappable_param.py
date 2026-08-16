"""결함 표본: 사상할 수 없는 파이썬 파라미터(호출식).

실제 저장소 계층을 줄인 것이다. 세 문장이 각각 다른 모양을 맡는다 - 파라미터가
없는 문장, 같은 형이 연달아 붙어 뒤바뀌어도 티가 안 나는 문장, 마이그레이션과 같은
단일 파라미터 문장. 실행 대상이 아니라 게이트가 읽는 표본이다.
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
                card.created_at_us,
                card.updated_at_us,
                card.source.value,
                card.body,
                text_hash(card.body),
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
