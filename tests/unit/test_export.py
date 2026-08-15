from __future__ import annotations

from pathlib import Path

from pynote.domain.models import Card, CardSource
from pynote.infrastructure.export import NewlineFormat, export_cards, render_cards


def _card(
    identifier: str,
    body: str,
    position_key: int,
    *,
    deleted_at_us: int | None = None,
) -> Card:
    return Card(
        id=identifier,
        document_id="document-1",
        operation_id="operation-1",
        position_key=position_key,
        capture_seq=position_key,
        created_at_us=1,
        updated_at_us=1,
        source=CardSource.TYPING,
        body=body,
        body_hash="hash",
        current_revision_id=f"revision-{identifier}",
        deleted_at_us=deleted_at_us,
    )


def test_single_card_export_is_exact_confirmed_body() -> None:
    card = _card("card-1", "첫 줄\n\n마지막 줄", 1_024)

    assert render_cards((card,)) == card.body


def test_export_uses_current_position_and_selected_newline(tmp_path: Path) -> None:
    path = tmp_path / "cards.md"
    cards = (
        _card("second", "둘째\r\n줄", 2_048),
        _card("deleted", "제외", 3_072, deleted_at_us=3),
        _card("first", "첫째\n줄", 1_024),
    )

    export_cards(path, cards, newline=NewlineFormat.CRLF)

    assert path.read_bytes() == "첫째\r\n줄\r\n\r\n둘째\r\n줄".encode()


def test_export_rejects_non_text_extension(tmp_path: Path) -> None:
    path = tmp_path / "cards.json"

    try:
        export_cards(path, ())
    except ValueError as error:
        assert "TXT 또는 Markdown" in str(error)
    else:
        raise AssertionError("지원하지 않는 확장자가 거부되어야 합니다.")
