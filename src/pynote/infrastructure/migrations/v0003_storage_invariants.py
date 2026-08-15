from __future__ import annotations

import sqlite3


def _validate_existing_rows(connection: sqlite3.Connection) -> None:
    invalid_current = connection.execute(
        """
        SELECT cards.id
        FROM cards
        LEFT JOIN card_revisions
          ON card_revisions.id = cards.current_revision_id
        WHERE cards.current_revision_id IS NOT NULL
          AND (
              card_revisions.id IS NULL
              OR card_revisions.card_id != cards.id
              OR card_revisions.body != cards.body
              OR card_revisions.body_hash != cards.body_hash
          )
        LIMIT 1
        """
    ).fetchone()
    if invalid_current is not None:
        raise RuntimeError(
            f"카드와 현재 리비전 불변조건이 손상되어 있습니다: {invalid_current[0]}"
        )

    invalid_parent = connection.execute(
        """
        SELECT child.id
        FROM card_revisions AS child
        JOIN card_revisions AS parent
          ON parent.id = child.parent_revision_id
        WHERE child.card_id != parent.card_id
        LIMIT 1
        """
    ).fetchone()
    if invalid_parent is not None:
        raise RuntimeError(
            f"리비전 부모 카드 불변조건이 손상되어 있습니다: {invalid_parent[0]}"
        )


def migrate(connection: sqlite3.Connection, applied_at_us: int) -> None:
    """카드·리비전 교차 불변조건과 capture counter 단조성을 강제한다."""
    _validate_existing_rows(connection)
    statements = (
        """
        CREATE TRIGGER IF NOT EXISTS cards_current_revision_insert
        BEFORE INSERT ON cards
        WHEN NEW.current_revision_id IS NOT NULL
          AND NOT EXISTS (
              SELECT 1
              FROM card_revisions
              WHERE id = NEW.current_revision_id
                AND card_id = NEW.id
                AND body = NEW.body
                AND body_hash = NEW.body_hash
          )
        BEGIN
            SELECT RAISE(ABORT, '카드와 현재 리비전이 일치하지 않습니다');
        END
        """,
        """
        CREATE TRIGGER IF NOT EXISTS cards_current_revision_update
        BEFORE UPDATE OF current_revision_id, body, body_hash ON cards
        WHEN NEW.current_revision_id IS NOT NULL
          AND NOT EXISTS (
              SELECT 1
              FROM card_revisions
              WHERE id = NEW.current_revision_id
                AND card_id = NEW.id
                AND body = NEW.body
                AND body_hash = NEW.body_hash
          )
        BEGIN
            SELECT RAISE(ABORT, '카드와 현재 리비전이 일치하지 않습니다');
        END
        """,
        """
        CREATE TRIGGER IF NOT EXISTS card_revisions_parent_insert
        BEFORE INSERT ON card_revisions
        WHEN NEW.parent_revision_id IS NOT NULL
          AND NOT EXISTS (
              SELECT 1
              FROM card_revisions
              WHERE id = NEW.parent_revision_id
                AND card_id = NEW.card_id
          )
        BEGIN
            SELECT RAISE(ABORT, '부모 리비전은 같은 카드에 속해야 합니다');
        END
        """,
        """
        CREATE TRIGGER IF NOT EXISTS card_revisions_parent_update
        BEFORE UPDATE OF card_id, parent_revision_id ON card_revisions
        WHEN NEW.parent_revision_id IS NOT NULL
          AND NOT EXISTS (
              SELECT 1
              FROM card_revisions
              WHERE id = NEW.parent_revision_id
                AND card_id = NEW.card_id
          )
        BEGIN
            SELECT RAISE(ABORT, '부모 리비전은 같은 카드에 속해야 합니다');
        END
        """,
        """
        CREATE TRIGGER IF NOT EXISTS card_revisions_current_update
        BEFORE UPDATE OF card_id, body, body_hash ON card_revisions
        WHEN EXISTS (
            SELECT 1
            FROM cards
            WHERE current_revision_id = OLD.id
              AND (
                  id != NEW.card_id
                  OR body != NEW.body
                  OR body_hash != NEW.body_hash
              )
        )
        BEGIN
            SELECT RAISE(ABORT, '현재 리비전과 카드가 일치해야 합니다');
        END
        """,
        """
        CREATE TRIGGER IF NOT EXISTS capture_counter_no_decrease
        BEFORE UPDATE OF next_value ON counters
        WHEN OLD.name = 'capture' AND NEW.next_value < OLD.next_value
        BEGIN
            SELECT RAISE(ABORT, 'capture counter는 감소시킬 수 없습니다');
        END
        """,
        """
        CREATE TRIGGER IF NOT EXISTS capture_counter_no_rename
        BEFORE UPDATE OF name ON counters
        WHEN OLD.name = 'capture' AND NEW.name != OLD.name
        BEGIN
            SELECT RAISE(ABORT, 'capture counter는 이름을 바꿀 수 없습니다');
        END
        """,
        """
        CREATE TRIGGER IF NOT EXISTS capture_counter_no_delete
        BEFORE DELETE ON counters
        WHEN OLD.name = 'capture'
        BEGIN
            SELECT RAISE(ABORT, 'capture counter는 삭제할 수 없습니다');
        END
        """,
    )
    for statement in statements:
        connection.execute(statement)
    connection.execute(
        """
        UPDATE schema_version
        SET version = 3, applied_at_us = ?
        WHERE id = 1
        """,
        (applied_at_us,),
    )
