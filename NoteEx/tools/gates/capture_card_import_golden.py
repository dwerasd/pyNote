from __future__ import annotations

import argparse
import hashlib
import json
import sqlite3
import sys
import tempfile
from pathlib import Path


def _hex(text: str) -> str:
    return text.encode("utf-8").hex()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    source_root = args.source_root.resolve()
    sys.path.insert(0, str(source_root / "src"))

    import pynote
    from pynote.domain.paragraph_parser import ParagraphParser
    from pynote.ui import import_dialog
    from pynote.application.card_service import CardService
    from pynote.domain.models import CaptureOperationSource, Document, Draft, DraftKind
    from pynote.infrastructure.database import Database
    from pynote.infrastructure.repositories import CardCompareAndSwapError, Repositories, text_hash

    resolved_package = Path(pynote.__file__).resolve()
    if not resolved_package.is_relative_to(source_root):
        raise RuntimeError(f"pynote import escaped source root: {resolved_package}")

    import_dialog._ANSI_ENCODING = "cp949"
    decode = import_dialog.decode_import_bytes
    prepare_bytes = import_dialog.prepare_import_from_bytes
    paragraph_parser = ParagraphParser()
    lines: list[str] = []

    original = "첫 카드\r\n둘째 줄"
    prepared = prepare_bytes(Path("stable.txt"), b"\xef\xbb\xbf" + original.encode())
    lines.append(f"WTL-W2-0001|body={_hex(prepared.text)}")

    with tempfile.TemporaryDirectory() as directory:
        exact = Path(directory) / "exact.txt"
        exact.write_bytes(b"x" * import_dialog.MAX_IMPORT_FILE_BYTES)
        exact_prepared = import_dialog.prepare_import(exact)
        lines.append(
            f"WTL-W2-0002|result=ok|bytes={len(exact_prepared.text.encode())}|read_limit=4194305"
        )
        oversized = Path(directory) / "oversized.txt"
        oversized.write_bytes(b"x" * (import_dialog.MAX_IMPORT_FILE_BYTES + 1))
        try:
            import_dialog.prepare_import(oversized)
        except ValueError:
            lines.append("WTL-W2-0003|result=too-large|bytes=4194305|read_limit=4194305")
        else:
            raise RuntimeError("oversized import unexpectedly succeeded")

    in_memory = prepare_bytes(Path("stable.txt"), b"x" * (import_dialog.MAX_IMPORT_FILE_BYTES + 1))
    lines.append(f"WTL-W2-0004|result=ok|bytes={len(in_memory.text.encode())}")

    expected = "A가"
    utf8_bom = decode(b"\xef\xbb\xbf" + expected.encode())
    utf16_le = decode(b"\xff\xfe" + expected.encode("utf-16-le"))
    utf16_be = decode(b"\xfe\xff" + expected.encode("utf-16-be"))
    utf8 = decode(expected.encode())
    ansi = decode("가".encode("cp949"))
    lines.append(
        "WTL-W2-0005"
        f"|utf8bom={_hex(utf8_bom)}|utf16le={_hex(utf16_le)}"
        f"|utf16be={_hex(utf16_be)}|utf8={_hex(utf8)}|ansi={_hex(ansi)}"
    )

    corpus = decode(bytes(range(256)))
    lines.append(
        "WTL-W2-0006|sha256="
        + hashlib.sha256(corpus.encode("utf-8")).hexdigest()
    )
    lines.append(f"WTL-W2-0007|decoded={_hex(decode(bytes.fromhex('81')))}")
    lines.append(f"WTL-W2-0008|decoded={_hex(decode(bytes.fromhex('f09f')))}")

    split_original = "첫 문단\r\n\r\n둘째 문단"
    split_prepared = prepare_bytes(Path("stable.txt"), split_original.encode())
    paragraphs = paragraph_parser.split(split_prepared.text)
    lines.append(
        "WTL-W2-0009|bodies="
        + ",".join(_hex(value) for value in paragraphs)
        + "|source=import|operations=1"
    )
    invalid = prepare_bytes(Path("stable.txt"), b"\x81notepad")
    lines.append(f"WTL-W2-0010|body={_hex(invalid.text)}")

    whitespace = " \t\r\n\u0085\u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200a\u2028\u2029\u202f\u205f\u3000"
    if paragraph_parser.split(whitespace):
        raise RuntimeError("Python-whitespace-only input unexpectedly produced a paragraph")
    lines.append("WTL-W2-0011|result=invalid|cards=0|events=0")

    blank_cases = [
        ("0012", b""),
        ("0013", b" \t\r\n"),
        ("0014", b"\xef\xbb\xbf"),
        ("0015", "\u0085".encode()),
        ("0016", "\u2007".encode()),
        ("0017", "\u202f".encode()),
        ("0018", "\u3000".encode()),
    ]
    for stable_id, data in blank_cases:
        try:
            prepare_bytes(Path("stable.txt"), data)
        except ValueError:
            lines.append(f"WTL-W2-{stable_id}|input={data.hex()}|result=blank")
        else:
            raise RuntimeError(f"blank import WTL-W2-{stable_id} unexpectedly succeeded")

    with tempfile.TemporaryDirectory() as directory:
        databases: list[Database] = []

        def fixture(name: str, times: tuple[int, ...] = (2_000,)) -> tuple[Database, Repositories, CardService, str]:
            database = Database(Path(directory) / f"{name}.sqlite3")
            databases.append(database)
            repositories = Repositories(database)
            document_id = "document-card-service"
            repositories.create_document(Document(
                id=document_id, title="카드 서비스 테스트",
                created_at_us=1_000, updated_at_us=1_000,
            ))
            clock_index = 0
            id_index = 0

            def clock() -> int:
                nonlocal clock_index
                value = times[min(clock_index, len(times) - 1)]
                clock_index += 1
                return value

            def new_id() -> str:
                nonlocal id_index
                value = f"id-{id_index}"
                id_index += 1
                return value

            return database, repositories, CardService(
                database, repositories, clock=clock, id_factory=new_id,
            ), document_id

        database, repositories, service, document_id = fixture("0102")
        body = "\n".join(f"{line}번째 줄" for line in range(1, 61))
        (card,) = service.create_cards(document_id, body, source=CaptureOperationSource.PASTE)
        operation = repositories.get_capture_operation(card.operation_id)
        lines.append(
            f"WTL-W2-0102|cards=1|lines={len(card.body.splitlines())}"
            f"|original={'none' if operation is not None and operation.original_text is None else 'present'}"
        )

        database, repositories, service, document_id = fixture("0103")
        original = " 첫 문단\r\n한 줄 더\r\n\r\n  \r\n둘째 문단\n\n셋째 문단\n"
        cards = service.create_cards(document_id, original, source=CaptureOperationSource.PASTE, split=True)
        operation = repositories.get_capture_operation(cards[0].operation_id)
        lines.append(
            f"WTL-W2-0103|cards={len(cards)}|capture="
            + ",".join(str(card.capture_seq) for card in cards)
            + f"|original={'exact' if operation is not None and operation.original_text == original else 'different'}"
        )

        database, repositories, service, document_id = fixture("0104")
        card = service.create_card(document_id, "직접 입력 뒤 붙여넣기", source=CaptureOperationSource.MIXED)
        operation = repositories.get_capture_operation(card.operation_id)
        event = repositories.list_events(document_id)[0]
        lines.append(
            f"WTL-W2-0104|operation={operation.source.value if operation else 'missing'}"
            f"|card={card.source.value}|event={event.source.value}"
        )

        database, repositories, service, document_id = fixture("0105")
        database.connection.execute(
            "CREATE TRIGGER fail_second BEFORE INSERT ON card_revisions "
            "WHEN NEW.body = '둘째 문단' BEGIN SELECT RAISE(ABORT, 'fail'); END"
        )
        try:
            service.create_cards(document_id, "첫 문단\n\n둘째 문단", source=CaptureOperationSource.PASTE, split=True)
        except sqlite3.IntegrityError:
            pass
        else:
            raise RuntimeError("split failure oracle unexpectedly succeeded")
        counts = [database.connection.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
                  for table in ("capture_operations", "cards", "edit_events", "card_revisions")]
        lines.append(
            "WTL-W2-0105|rows=" + ",".join(str(value) for value in counts)
            + f"|counter={repositories.get_counter('capture')}"
        )

        database, repositories, service, document_id = fixture("0106", (2_000, 2_000))
        card = service.create_card(document_id, "삭제 성공")
        repositories.create_draft(Draft(
            id="draft-delete", document_id=document_id, card_id=card.id,
            draft_kind=DraftKind.EDIT, base_revision_id=card.current_revision_id,
            draft_text="edit", draft_hash=text_hash("edit"), cursor_position_qchar=4,
            updated_at_us=3_000,
        ))
        deleted = service.soft_delete(
            card.id, expected_revision_id=card.current_revision_id,
            discard_draft_id="draft-delete",
        )
        lines.append(
            f"WTL-W2-0106|draft={'deleted' if repositories.get_draft('draft-delete') is None else 'present'}"
            f"|card={'soft-deleted' if deleted.deleted_at_us is not None else 'active'}"
        )

        database, repositories, service, document_id = fixture("0107")
        first = service.create_card(document_id, "A")
        second = service.create_card(document_id, "B")
        service.create_card(document_id, "C", before_card_id=second.id)
        ordered = service.list_active_cards(document_id, sort_mode="position")
        lines.append(
            "WTL-W2-0107|bodies=" + ",".join(_hex(card.body) for card in ordered)
            + "|capture=" + ",".join(str(card.capture_seq) for card in ordered)
        )

        database, repositories, service, document_id = fixture("0108")
        created = tuple(service.create_card(document_id, value) for value in ("A", "B", "C"))
        recent = service.list_active_cards(document_id)
        positioned = service.list_active_cards(document_id, sort_mode="position")
        capture_by_id = {card.id: card.capture_seq for card in created}
        lines.append(
            "WTL-W2-0108|recency=" + ",".join(str(capture_by_id[card.id]) for card in recent)
            + "|position=" + ",".join(str(capture_by_id[card.id]) for card in positioned)
        )

        database, repositories, service, document_id = fixture("0109")
        first = service.create_card(document_id, "A")
        second = service.create_card(document_id, "B")
        repositories.update_card_position(first.id, 1, expected_revision_id=first.current_revision_id or "")
        repositories.update_card_position(second.id, 2, expected_revision_id=second.current_revision_id or "")
        middle = service.create_card(document_id, "C", before_card_id=second.id)
        ordered = service.list_active_cards(document_id, sort_mode="position")
        lines.append(
            "WTL-W2-0109|bodies=" + ",".join(_hex(card.body) for card in ordered)
            + f"|unique_positions={len({card.position_key for card in ordered})}|capture={middle.capture_seq}"
        )

        database, repositories, service, document_id = fixture("0110", (2_000, 3_000, 4_000, 5_000, 6_000))
        first = service.create_card(document_id, "A")
        second = service.create_card(document_id, "B")
        third = service.create_card(document_id, "C")
        moved = service.move_card(third.id, before_card_id=second.id)
        deleted = service.soft_delete(moved.id)
        active = service.list_active_cards(document_id, sort_mode="position")
        details = json.loads(repositories.list_events(document_id)[-1].details_json)
        neighbors = "first,second" if (
            details["left_neighbor_id"] == first.id and details["right_neighbor_id"] == second.id
        ) else "different"
        lines.append(
            f"WTL-W2-0110|capture={deleted.capture_seq}|active="
            + ",".join(_hex(card.body) for card in active) + f"|neighbors={neighbors}"
        )

        database, repositories, service, document_id = fixture("0111")
        card = service.create_card(document_id, "삭제 실패")
        events_before = repositories.list_events(document_id)
        database.connection.execute(
            "CREATE TRIGGER fail_delete BEFORE UPDATE OF deleted_at_us ON cards "
            "WHEN NEW.deleted_at_us IS NOT NULL BEGIN SELECT RAISE(ABORT, 'fail'); END"
        )
        try:
            service.soft_delete(card.id)
        except sqlite3.IntegrityError:
            pass
        else:
            raise RuntimeError("soft-delete failure oracle unexpectedly succeeded")
        stored = repositories.get_card(card.id)
        lines.append(
            f"WTL-W2-0111|deleted={int(stored is not None and stored.deleted_at_us is not None)}"
            f"|events_unchanged={int(repositories.list_events(document_id) == events_before)}"
        )

        database, repositories, service, document_id = fixture("0112")
        card = service.create_card(document_id, "비어 있지 않은 본문")
        events_before = repositories.list_events(document_id)
        result = "ok"
        try:
            service.soft_delete(
                card.id, expected_revision_id=card.current_revision_id,
                require_empty_body=True,
            )
        except CardCompareAndSwapError:
            result = "conflict"
        stored = repositories.get_card(card.id)
        lines.append(
            f"WTL-W2-0112|result={result}"
            f"|deleted={int(stored is not None and stored.deleted_at_us is not None)}"
            f"|events_unchanged={int(repositories.list_events(document_id) == events_before)}"
        )

        for database in databases:
            database.close()

    if len(lines) != 29:
        raise RuntimeError(f"expected 29 golden lines, got {len(lines)}")
    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
