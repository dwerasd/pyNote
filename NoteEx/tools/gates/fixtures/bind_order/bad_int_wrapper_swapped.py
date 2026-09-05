"""결함 표본: 단일 인자 int(...) 래퍼 안의 이름이 이웃 자리와 뒤바뀐 파이썬 튜플.

good.py 와 같되 created_at_us / updated_at_us 두 자리를 int(...) 로 감싸고 순서를 맞바꿨다.
게이트가 int(...) 래퍼를 벗겨 안쪽 이름 경로로 사상해야만 "값 대응 어긋남" 으로 잡힌다 -
래퍼를 사상 실패로 접거나 통째로 투명 처리하면 이 표본을 기대 사유로 못 잡는다.
실행 대상이 아니라 게이트가 읽는 표본이다.
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
                int(card.updated_at_us),
                int(card.created_at_us),
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
