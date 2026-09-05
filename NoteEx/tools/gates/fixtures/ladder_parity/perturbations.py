"""사다리 게이트 자기시험용 교란 목록(fixture).

C++ 러너 없이 게이트의 탐지력을 증명하기 위해, **파이썬 러너를 일부러 망가뜨린
것**을 C++ 자리에 세운다. 각 교란은 등록 목록을 받아 새 목록을 돌려주며, 원본
`migrate` 를 감싸 결함을 흉내 낸다.

`apply` 는 마이그레이션 SQL 을 다시 쓰지 않는다 - 원본을 부른 뒤 결과를 비트는
방식이라 이 파일에도 스키마 SQL 사본이 없다. 흉내 내려는 것은 특정 문장이 아니라
**결함의 종류**다.

각 항목의 뜻:
  name       보고에 쓰는 이름
  describe   무엇을 흉내 내는가
  detected   True 면 최소 한 경로에서 잡혀야 하고, False 면 어느 경로에서도
             잡히면 안 된다(설계상 비탐지 = 음성 대조군)
  apply      (entries) -> entries
"""

from __future__ import annotations

from typing import Callable, NamedTuple, Sequence


class Perturbation(NamedTuple):
    name: str
    describe: str
    detected: bool
    apply: Callable[[Sequence], list]


def _index_of(entries: Sequence, version: int) -> int:
    for position, entry in enumerate(entries):
        if entry.version == version:
            return position
    raise LookupError(f"등록 목록에 v{version:04d} 가 없다")


def _wrap(entry, after):
    """원본 migrate 를 부른 뒤 `after(connection)` 를 덧붙인 항목을 만든다."""
    original = entry.migrate

    def migrate(connection, applied_at_us):
        original(connection, applied_at_us)
        after(connection)

    return entry._replace(migrate=migrate)


def _skip(entries: Sequence, version: int) -> list:
    return [entry for entry in entries if entry.version != version]


def _skip_v0009(entries: Sequence) -> list:
    return _skip(entries, 9)


def _skip_v0007(entries: Sequence) -> list:
    return _skip(entries, 7)


def _wrong_final_version(entries: Sequence) -> list:
    result = list(entries)
    # 교란 대상은 **마지막 등록 마이그레이션**이다. 특정 버전에 고정하면 그 뒤에 새
    # 마이그레이션이 붙는 순간 그쪽의 schema_version 갱신이 심어 둔 잘못된 값을
    # 덮어써 known-bad 가 미탐이 된다(v0010 도입 실측).
    position = len(result) - 1
    result[position] = _wrap(
        result[position],
        lambda connection: connection.execute(
            "UPDATE schema_version SET version = 8 WHERE id = 1"
        ),
    )
    return result


def _unconditional_preview_lines(entries: Sequence) -> list:
    result = list(entries)
    position = _index_of(result, 9)
    result[position] = _wrap(
        result[position],
        lambda connection: connection.execute(
            "UPDATE data_policy_settings SET preview_lines = 3 WHERE id = 1"
        ),
    )
    return result


def _swap_ui_state_columns(entries: Sequence) -> list:
    result = list(entries)
    position = _index_of(result, 5)
    result[position] = _wrap(
        result[position],
        lambda connection: connection.execute(
            "UPDATE document_ui_states SET selected_card_id = editor_card_id, "
            "editor_card_id = selected_card_id"
        ),
    )
    return result


def _drop_migrated_windows(entries: Sequence) -> list:
    result = list(entries)
    position = _index_of(result, 4)
    result[position] = _wrap(
        result[position],
        lambda connection: connection.execute("DELETE FROM workspace_windows"),
    )
    return result


def _extra_index(entries: Sequence) -> list:
    result = list(entries)
    position = _index_of(result, 9)
    result[position] = _wrap(
        result[position],
        lambda connection: connection.execute(
            "CREATE INDEX ladder_probe_extra ON documents(title)"
        ),
    )
    return result


PERTURBATIONS: tuple[Perturbation, ...] = (
    Perturbation(
        name="skip_v0009",
        describe="마지막 마이그레이션을 통째로 건너뛴다(버전이 8 에 머문다)",
        detected=True,
        apply=_skip_v0009,
    ),
    Perturbation(
        name="wrong_final_version",
        describe="문장은 다 돌지만 schema_version 이 틀린 번호로 남는다",
        detected=True,
        apply=_wrong_final_version,
    ),
    Perturbation(
        name="unconditional_preview_lines",
        describe="v0009 의 WHERE 조건을 잃어 이미 다른 값인 행까지 덮어쓴다",
        detected=True,
        apply=_unconditional_preview_lines,
    ),
    Perturbation(
        name="swap_ui_state_columns",
        describe=(
            "v0005 의 복사에서 두 열이 뒤바뀐다 - 스키마는 완전히 같고 행 값만 틀린다"
        ),
        detected=True,
        apply=_swap_ui_state_columns,
    ),
    Perturbation(
        name="drop_migrated_windows",
        describe="v0004 의 INSERT ... SELECT 가 옮긴 행이 남지 않는다",
        detected=True,
        apply=_drop_migrated_windows,
    ),
    Perturbation(
        name="extra_index",
        describe="원본에 없는 인덱스가 하나 늘어난다(스키마 발산)",
        detected=True,
        apply=_extra_index,
    ),
    Perturbation(
        name="skip_v0007",
        describe=(
            "음성 대조군: v0007 을 건너뛰어도 v0008 이 **문면이 같은** UPDATE 를 "
            "발행하므로 최종 상태가 같다. v0007 만 구간에 드는 경로는 없다 - "
            "이 게이트는 최종 상태만 보므로 잡히지 않는 것이 정상이고, 잡힌다면 "
            "사거리가 바뀐 것이라 문서를 함께 고쳐야 한다"
        ),
        detected=False,
        apply=_skip_v0007,
    ),
)
