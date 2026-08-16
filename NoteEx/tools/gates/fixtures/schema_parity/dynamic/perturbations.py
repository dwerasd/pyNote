"""동적 게이트 자기시험용 교란 목록(fixture).

원본 `STATEMENTS` 를 받아 **의도적으로 망가뜨린** 문장 목록을 돌려주는 함수들이다.
스키마 사본을 들지 않기 위해 상수 SQL 을 새로 쓰지 않고 원문을 문자열 수술로
바꾼다. 그래서 원본 문면이 바뀌면 교란이 무효(no-op)가 될 수 있는데, 그때는
자기시험이 "교란이 아무것도 바꾸지 않았다"로 실패한다 - 조용히 통과하지 않는다.

`detected=False` 는 **음성 대조군**이다. 이 게이트가 못 잡는 것이 무엇인지도
시험으로 고정해 둔다. 사거리가 넓어지거나 좁아지면 자기시험이 깨지고, 그때는
README 의 사거리 설명을 함께 고쳐야 한다.
"""

from __future__ import annotations

from typing import Callable, NamedTuple, Sequence


class Perturbation(NamedTuple):
    """교란 1종."""

    name: str
    describe: str
    detected: bool  # 이 게이트가 잡아야 하는가
    apply: Callable[[Sequence[str]], tuple[str, ...]]


def _replace_in_statement(
    statements: Sequence[str], marker: str, old: str, new: str
) -> tuple[str, ...]:
    """`marker` 를 담은 첫 문장에서 `old` 를 한 번만 `new` 로 바꾼다."""
    result = list(statements)
    for index, statement in enumerate(result):
        if marker in statement and old in statement:
            result[index] = statement.replace(old, new, 1)
            break
    return tuple(result)


def drop_not_null(statements: Sequence[str]) -> tuple[str, ...]:
    """documents.title 의 NOT NULL 을 없앤다(제약 소실)."""
    return _replace_in_statement(
        statements, "CREATE TABLE documents", "title TEXT NOT NULL", "title TEXT"
    )


def inner_whitespace(statements: Sequence[str]) -> tuple[str, ...]:
    """documents 의 첫 열 들여쓰기를 한 칸 늘린다(문장 내부 공백 표류)."""
    return _replace_in_statement(
        statements,
        "CREATE TABLE documents",
        "\n        id TEXT PRIMARY KEY",
        "\n         id TEXT PRIMARY KEY",
    )


def extra_index(statements: Sequence[str]) -> tuple[str, ...]:
    """원본에 없는 인덱스를 하나 더 만든다(잉여 스키마 객체)."""
    return tuple(statements) + (
        "\n    CREATE INDEX parity_fixture_extra\n    ON documents(title)\n    ",
    )


def drop_seed_insert(statements: Sequence[str]) -> tuple[str, ...]:
    """counters 시드 INSERT 를 지운다. 스키마 객체가 아니라서 잡히지 않는다."""
    return tuple(
        statement
        for statement in statements
        if "INSERT INTO counters" not in statement
    )


PERTURBATIONS: tuple[Perturbation, ...] = (
    Perturbation(
        name="drop_not_null",
        describe="documents.title 의 NOT NULL 제거(제약 소실)",
        detected=True,
        apply=drop_not_null,
    ),
    Perturbation(
        name="inner_whitespace",
        describe="문장 내부 들여쓰기 한 칸 변화(공백 정규화를 하지 않는다는 증명)",
        detected=True,
        apply=inner_whitespace,
    ),
    Perturbation(
        name="extra_index",
        describe="원본에 없는 인덱스 1개 추가(잉여 객체)",
        detected=True,
        apply=extra_index,
    ),
    Perturbation(
        name="drop_seed_insert",
        describe=(
            "counters 시드 INSERT 누락 - sqlite_master 에 흔적이 없어 이 게이트의 "
            "사거리 밖이다. 정적 게이트가 덮는 사각이다"
        ),
        detected=False,
        apply=drop_seed_insert,
    ),
)
