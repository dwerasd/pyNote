#!/usr/bin/env python3
"""바인드 파라미터 순서·인덱스 이식 게이트(저장소 계층 + 마이그레이션 아홉 쌍).

`check_repository_sql_parity.py` 와 `check_migration_sql_parity.py` 는 SQL **본문**이
바이트까지 같은지만 본다. 본문이 같아도 `?` 자리에 넣는 값의 순서가 어긋나면 컴파일도
되고 왕복 시험도 통과할 수 있다 - 같은 형의 파라미터가 연달아 있는 문장
(`UpdateCaptureOperation` 8개, `CreateCard` 12개, `UpdateDraft` 9개)에서 두 자리를
맞바꾸면 시험 값이 서로 비슷할 때 아무도 알아채지 못한다. 이 게이트는 그 구멍 하나만
맡는다.

## 무엇을 증명하나 (아래로 갈수록 어렵고, 아래로 갈수록 덜 확실하다)

1. **개수·인덱스 정합**: 문장의 `?` 개수와 바인드 호출 수가 같고, 인덱스가 빠짐도
   중복도 없이 정확히 1..N 이다. 인덱스 오타를 잡는다.
2. **오름차순 배치**: 바인드 호출이 소스에 인덱스 오름차순으로 놓인다. 읽는 사람이
   위에서 아래로 따라갈 수 있어야 한다는 계약이며, 1 과 달리 동작이 아니라 가독성을
   보호한다.
3. **값 대응**: 파이썬 원본이 넘기는 **튜플 순서**와 C++ 이식본이 **인덱스로 지정한
   순서**가 같은 값을 가리킨다. 파이썬은 위치로, C++ 은 이름(인덱스)으로 말하므로
   둘을 잇는 데 `card.updated_at_us` <-> `_Card.nUpdatedAtUs` 같은 이름 사상이
   필요하다. 이것만 **추정(heuristic)** 이고, 규칙은 아래 "이름 사상" 절에 있다.
4. **센서스(누락 방지)**: 양쪽 모두, `?` 를 담은 SQL 문자열이 인식된 바인드 자리에
   붙어 있지 않으면 실패다. 게이트가 모르는 방식으로 바인딩하면 조용히 넘어가는 대신
   그 문장을 실패로 보고한다.

## 무엇을 증명하지 않나 (여기 적힌 것은 이 게이트가 못 보는 것이다)

* **SQL 본문 동등성.** 짝짓기 키로만 쓰고 대조하지 않는다. 본문은 위 두 게이트 소관이다.
* **바인드 형이 열 정의와 맞는지.** `BindText` 냐 `BindInt64` 냐, `Nullable` 이냐는
  보지 않는다. 형이 어긋나면 C++ 컴파일러가 먼저 잡는 자리다.
* **이름이 같으면 값도 같다는 보장.** 사상은 many-to-one 이다. `_Card.sId` 와
  `_Draft.sId` 는 둘 다 `id` 로 접힌다. 소유자(`card` / `draft`)까지 양쪽에 다 있으면
  그것도 대조하지만, 한쪽이 지역 변수면 마지막 이름만 본다. 즉 **이름이 다르면 확실히
  잡고, 이름이 같으면 아마 맞다** 까지다.
* **파라미터 없는 문장.** `?` 가 없는 문장은 짝짓기 대상이 아니다 - 지킬 순서가 없고,
  마이그레이션 이식본은 그런 문장을 배열·상수로 두고 `Execute` 로 흘려보내 바인드 자리를
  아예 만들지 않는다. 그 문장들의 존재·본문은 SQL 축자 게이트 둘이 지킨다.
* **파이썬 쪽 비리터럴 SQL.** 문자열 리터럴이 아닌 방식으로 조립한 SQL 은
  `execute()` 인자에서 뽑히지 않는다. 다만 파라미터를 함께 넘기면 그 자리가 실패이고,
  `?` 를 담은 SQL 문자열 리터럴이 모듈 안에 떠 있으면 센서스가 잡는다.
* **C++ 변수 유효범위.** 바인드는 **바로 앞에 선언된 같은 이름의 문장**에 붙인다.
  중괄호 범위를 해석하지 않으므로, 앞 블록의 변수 이름을 재사용하면서 새 선언 없이
  바인드하는 코드는 잘못 붙는다. 이식본은 선언 직후에만 바인드하므로 지금은 성립한다.
* **실행 시점 동작.** 그 값이 정말 그 열에 들어가는지는 왕복 시험 소관이다.
* **백업 계층.** 범위 밖이다(T-R5 지시서). 파일이 자리를 잡으면 `--python/--cpp` 로
  같은 규칙을 걸 수 있다.

## 이름 사상 (3번 항목의 추정 규칙)

C++ 식: `domain::ToText(...)` 만 벗기고, 앞의 `*`/`&` 를 떼고, 점으로 나눈 마지막
식별자를 취한다. 그 식별자에서 앞의 `_`/`m_` 와 헝가리안 접두(`s` `n` `b` `e` `p`
`psz` 등)를 떼고 CamelCase 를 snake_case 로 접는다. `_Card.nUpdatedAtUs` -> 소유자
`card`, 이름 `updated_at_us`.

파이썬 식: 이름 또는 속성 사슬만 받는다. 열거형의 `.value` 는 C++ 의 `ToText(...)` 와
같은 자리이므로 떼고 본다. `card.updated_at_us` -> 소유자 `card`, 이름 `updated_at_us`.

**접을 수 없는 식은 건너뛰지 않고 실패다.** 함수 호출, 첨자, 연산식, 헝가리안도
snake 도 아닌 식별자가 나오면 그 문장을 위반으로 보고한다. 모르는 것을 조용히 넘기는
검사기는 없느니만 못하다 - 통과라고 보고하기 때문이다.

## 종료 코드

  0  검사 대상 전건 통과
  1  위반 검출(자기시험이면 기대 위반)
  2  사용법·환경 오류(경로 없음, 읽기 실패, **C++ 이식본 없음**, **검사 대상 0건**)

C++ 이식본이 없는 파이썬 원본이 하나라도 있으면 2 다. 있는 것만 보고 통과라고 말하지
않는다 - 판정할 수 없는 상태는 통과가 아니다.
"""

from __future__ import annotations

import argparse
import ast
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Sequence

REPO_ROOT = Path(__file__).resolve().parents[3]
FIXTURE_DIR = Path(__file__).resolve().parent / "fixtures" / "bind_order"

DEFAULT_PY_REPOSITORIES = REPO_ROOT / "src" / "pynote" / "infrastructure" / "repositories.py"
DEFAULT_CPP_REPOSITORIES = REPO_ROOT / "NoteEx" / "core" / "src" / "storage" / "repositories.cpp"
DEFAULT_PY_MIGRATIONS = REPO_ROOT / "src" / "pynote" / "infrastructure" / "migrations"
DEFAULT_CPP_MIGRATIONS = REPO_ROOT / "NoteEx" / "core" / "src" / "storage" / "migrations"

# C++ 원시 문자열 구분자. 저장소·마이그레이션 양쪽이 `SQL` 로 고정이다.
RAW_LITERAL = re.compile(r'(?:u8|u|U|L)?R"SQL\((.*?)\)SQL"', re.DOTALL)
CONSTANT_DECL = re.compile(r'constexpr\s+const\s+char8_t\s*\*\s*(\w+)\s*=\s*(?:u8|u|U|L)?R"SQL\(')
STATEMENT_DECL = re.compile(r"\bC_STATEMENT\s+(\w+)\s*\(")
BIND_CALL = re.compile(r"\b(\w+)\s*\.\s*Bind([A-Za-z]\w*)\s*\(")
EXEC_BOUND = re.compile(r"\bExecuteBoundInt64\s*\(")

# 인식하는 바인드 메서드. 여기 없는 `Bind...` 는 위반으로 보고한다(`BindOk` 만 예외 -
# 바인드가 아니라 성공 여부 질의다).
KNOWN_BIND_METHODS = {"BindText", "BindNullableText", "BindInt64", "BindNullableInt64"}
NON_BIND_METHODS = {"Ok"}

# 값을 바꾸지 않고 표현만 옮기는 호출. 파이썬 열거형의 `.value` 와 같은 자리다.
TRANSPARENT_CALLS = {"ToText"}

# 헝가리안 접두. 긴 것부터 시도한다.
HUNGARIAN_PREFIX = re.compile(
    r"^(?:psz|pfn|wsz|lp|sz|dw|by|ch|p|s|n|b|e|f|d|w|u|i|h)([A-Z][A-Za-z0-9]*)$"
)
SNAKE_IDENTIFIER = re.compile(r"^[a-z][a-z0-9_]*$")
PASCAL_IDENTIFIER = re.compile(r"^[A-Z][A-Za-z0-9]*$")
DOTTED_PATH = re.compile(r"^[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*$")

# SQL 로 볼 문자열. 센서스가 주석·설명문을 SQL 로 오인하지 않게 좁힌다.
SQL_LOOKING = re.compile(r"^\s*(SELECT|INSERT|UPDATE|DELETE|REPLACE|WITH)\b", re.IGNORECASE)


class MappingError(RuntimeError):
    """이름 사상이 접을 수 없는 식을 만났다."""


# ---------------------------------------------------------------------------------------
# 문자열·SQL 훑기
# ---------------------------------------------------------------------------------------
def normalise(text: str) -> str:
    return text.replace("\r\n", "\n").replace("\r", "\n")


def statement_key(sql: str) -> str:
    """짝짓기 키. 공백을 접는다 - 본문 대조는 이 게이트의 일이 아니다."""
    return " ".join(sql.split())


def scan_placeholders(sql: str) -> tuple[int, list[str]]:
    """(익명 `?` 개수, 지원하지 않는 파라미터 표기 목록).

    문자열 리터럴·식별자 인용·주석 안의 `?` 는 세지 않는다. `?NNN`/`:name`/`@name`/
    `$name` 은 위치 계약이 무너지는 표기라 세는 대신 사유로 돌려준다.
    """
    count = 0
    unsupported: list[str] = []
    index = 0
    size = len(sql)
    while index < size:
        char = sql[index]
        if char == "'":
            index += 1
            while index < size:
                if sql[index] == "'":
                    if index + 1 < size and sql[index + 1] == "'":
                        index += 2
                        continue
                    index += 1
                    break
                index += 1
            continue
        if char in '"`':
            closing = char
            index += 1
            while index < size and sql[index] != closing:
                index += 1
            index += 1
            continue
        if char == "[":
            index += 1
            while index < size and sql[index] != "]":
                index += 1
            index += 1
            continue
        if char == "-" and index + 1 < size and sql[index + 1] == "-":
            while index < size and sql[index] != "\n":
                index += 1
            continue
        if char == "/" and index + 1 < size and sql[index + 1] == "*":
            index += 2
            while index + 1 < size and not (sql[index] == "*" and sql[index + 1] == "/"):
                index += 1
            index += 2
            continue
        if char == "?":
            following = sql[index + 1 : index + 2]
            if following.isdigit():
                unsupported.append("?" + following + "...")
                index += 2
                continue
            count += 1
            index += 1
            continue
        if char in ":@$":
            following = sql[index + 1 : index + 2]
            if following and (following.isalpha() or following == "_"):
                unsupported.append(sql[index : index + 8])
                index += 2
                continue
        index += 1
    return count, unsupported


# ---------------------------------------------------------------------------------------
# 이름 사상
# ---------------------------------------------------------------------------------------
def camel_to_snake(name: str) -> str:
    stepped = re.sub(r"(?<=[a-z0-9])([A-Z])", r"_\1", name)
    stepped = re.sub(r"(?<=[A-Z])([A-Z][a-z])", r"_\1", stepped)
    return stepped.lower()


def canonical_identifier(name: str) -> str:
    """식별자 하나를 snake_case 로 접는다. 접을 수 없으면 MappingError."""
    stripped = name
    while stripped.startswith("_"):
        stripped = stripped[1:]
    if stripped.startswith("m_"):
        stripped = stripped[2:]
    if not stripped:
        raise MappingError(f"빈 식별자: {name!r}")
    if SNAKE_IDENTIFIER.match(stripped):
        return stripped
    matched = HUNGARIAN_PREFIX.match(stripped)
    if matched is not None:
        return camel_to_snake(matched.group(1))
    if PASCAL_IDENTIFIER.match(stripped):
        return camel_to_snake(stripped)
    raise MappingError(f"헝가리안도 snake 도 아닌 식별자: {name!r}")


def unwrap_transparent(expr: str) -> str:
    """`domain::ToText(x)` 처럼 값을 바꾸지 않는 호출만 벗긴다."""
    text = expr.strip()
    while True:
        matched = re.match(r"^([A-Za-z_][\w:]*)\s*\((.*)\)$", text, re.DOTALL)
        if matched is None:
            return text
        callee = matched.group(1).split("::")[-1]
        if callee not in TRANSPARENT_CALLS:
            raise MappingError(f"값을 옮기는지 알 수 없는 호출: {expr.strip()!r}")
        inner = matched.group(2)
        if not _balanced(inner):
            # 괄호가 두 개 이상 이어 붙은 식(`f(a), g(b)`)은 인자 하나가 아니다.
            raise MappingError(f"인자를 하나로 볼 수 없는 식: {expr.strip()!r}")
        text = inner.strip()


def _balanced(text: str) -> bool:
    depth = 0
    for char in text:
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth < 0:
                return False
    return depth == 0


def canonical_cpp(expr: str) -> tuple[str | None, str]:
    """C++ 바인드 식을 (소유자, 이름) 으로 접는다."""
    text = unwrap_transparent(expr).strip()
    while text.startswith(("*", "&")):
        text = text[1:].strip()
    text = text.replace("->", ".")
    if not DOTTED_PATH.match(text):
        raise MappingError(f"이름 경로가 아닌 식: {expr.strip()!r}")
    parts = text.split(".")
    name = canonical_identifier(parts[-1])
    owner = canonical_identifier(parts[-2]) if len(parts) >= 2 else None
    return owner, name


def canonical_py(node: ast.expr) -> tuple[str | None, str]:
    """파이썬 파라미터 식을 (소유자, 이름) 으로 접는다."""
    parts: list[str] = []
    cursor: ast.expr = node
    while isinstance(cursor, ast.Attribute):
        parts.append(cursor.attr)
        cursor = cursor.value
    if not isinstance(cursor, ast.Name):
        raise MappingError(f"이름 경로가 아닌 식: {ast.unparse(node)!r}")
    parts.append(cursor.id)
    parts.reverse()
    # 열거형의 `.value` 는 C++ 의 ToText(...) 와 같은 자리다.
    if len(parts) >= 2 and parts[-1] == "value":
        parts = parts[:-1]
    name = canonical_identifier(parts[-1])
    owner = canonical_identifier(parts[-2]) if len(parts) >= 2 else None
    return owner, name


# ---------------------------------------------------------------------------------------
# 파이썬 쪽 추출
# ---------------------------------------------------------------------------------------
@dataclass
class PySite:
    sql: str
    line: int
    params: list[ast.expr]


def python_sites(source: str, label: str) -> tuple[list[PySite], list[str]]:
    tree = ast.parse(source)
    sites: list[PySite] = []
    problems: list[str] = []
    consumed: set[int] = set()

    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        func = node.func
        if not isinstance(func, ast.Attribute) or func.attr != "execute":
            continue
        if not node.args:
            continue
        first = node.args[0]
        literal = isinstance(first, ast.Constant) and isinstance(first.value, str)
        if not literal:
            if len(node.args) > 1:
                problems.append(
                    f"{label}:{node.lineno} 파라미터를 넘기는데 SQL 이 문자열 리터럴이 아니다"
                )
            continue
        consumed.add(id(first))
        params: list[ast.expr] = []
        if len(node.args) > 1:
            second = node.args[1]
            if isinstance(second, (ast.Tuple, ast.List)):
                params = list(second.elts)
            else:
                problems.append(
                    f"{label}:{node.lineno} 파라미터가 튜플·리스트 리터럴이 아니다: "
                    f"{ast.unparse(second)!r}"
                )
                continue
        sites.append(PySite(sql=normalise(str(first.value)), line=node.lineno, params=params))

    # 센서스. `?` 를 담은 SQL 문자열이 execute 인자로 쓰이지 않았으면 어딘가 다른 길로
    # 실행되고 있다는 뜻이라 조용히 넘기지 않는다.
    for node in ast.walk(tree):
        if not isinstance(node, ast.Constant) or not isinstance(node.value, str):
            continue
        if id(node) in consumed:
            continue
        text = normalise(node.value)
        if SQL_LOOKING.match(text) is None:
            continue
        count, unsupported = scan_placeholders(text)
        if count or unsupported:
            problems.append(
                f"{label}:{node.lineno} 바인드 자리를 찾지 못한 SQL 문자열: "
                f"{statement_key(text)[:60]!r}"
            )
    return sites, problems


# ---------------------------------------------------------------------------------------
# C++ 쪽 추출
# ---------------------------------------------------------------------------------------
@dataclass
class CppBind:
    index: int | None
    method: str
    expr: str
    line: int


@dataclass
class CppSite:
    sql: str
    line: int
    form: str
    literal_id: int
    binds: list[CppBind] = field(default_factory=list)


def _line_of(text: str, position: int) -> int:
    return text.count("\n", 0, position) + 1


def _literal_spans(source: str) -> list[tuple[int, int, str]]:
    return [(m.start(), m.end(), m.group(1)) for m in RAW_LITERAL.finditer(source)]


def _inside_literal(spans: Sequence[tuple[int, int, str]], position: int) -> bool:
    return any(start <= position < end for start, end, _ in spans)


def _literal_in_range(spans: Sequence[tuple[int, int, str]], low: int, high: int) -> int | None:
    for index, (start, end, _) in enumerate(spans):
        if low <= start and end <= high:
            return index
    return None


def _split_arguments(
    source: str, open_paren: int, spans: Sequence[tuple[int, int, str]]
) -> tuple[list[tuple[int, int]], int] | None:
    """여는 괄호 다음부터 짝이 맞는 닫는 괄호까지를 최상위 콤마로 나눈다.

    원시 SQL 리터럴 안의 괄호·콤마는 건너뛴다. 짝을 못 찾으면 None.
    """
    arguments: list[tuple[int, int]] = []
    depth = 0
    index = open_paren
    start = open_paren + 1
    size = len(source)
    while index < size:
        if _inside_literal(spans, index):
            index += 1
            continue
        char = source[index]
        if char in "\"'":
            closing = char
            index += 1
            while index < size:
                if source[index] == "\\":
                    index += 2
                    continue
                if source[index] == closing:
                    break
                index += 1
            index += 1
            continue
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
            if depth == 0:
                arguments.append((start, index))
                return arguments, index
        elif char == "," and depth == 1:
            arguments.append((start, index))
            start = index + 1
        index += 1
    return None


def cpp_sites(source: str, label: str) -> tuple[list[CppSite], list[str]]:
    spans = _literal_spans(source)
    problems: list[str] = []

    constants: dict[str, int] = {}
    for matched in CONSTANT_DECL.finditer(source):
        if _inside_literal(spans, matched.start()):
            continue
        # 선언 뒤 처음 나오는 리터럴이 그 상수의 본문이다(정규식이 여는 구분자까지 먹었다).
        for index, (start, _end, _body) in enumerate(spans):
            if start >= matched.start():
                constants[matched.group(1)] = index
                break

    events: list[tuple[int, str, re.Match[str]]] = []
    for matched in STATEMENT_DECL.finditer(source):
        if not _inside_literal(spans, matched.start()):
            events.append((matched.start(), "declare", matched))
    for matched in BIND_CALL.finditer(source):
        if not _inside_literal(spans, matched.start()):
            events.append((matched.start(), "bind", matched))
    for matched in EXEC_BOUND.finditer(source):
        if not _inside_literal(spans, matched.start()):
            events.append((matched.start(), "exec", matched))
    events.sort(key=lambda item: item[0])

    sites: list[CppSite] = []
    active: dict[str, CppSite] = {}

    for position, kind, matched in events:
        line = _line_of(source, position)
        parsed = _split_arguments(source, matched.end() - 1, spans)
        if parsed is None:
            problems.append(f"{label}:{line} 괄호 짝을 찾지 못했다")
            continue
        arguments, _close = parsed
        texts = [source[start:end].strip() for start, end in arguments]

        if kind == "declare":
            variable = matched.group(1)
            literal_id = None
            if arguments:
                literal_id = _literal_in_range(spans, arguments[-1][0], arguments[-1][1])
            if literal_id is None:
                problems.append(f"{label}:{line} C_STATEMENT 의 SQL 이 원시 리터럴이 아니다")
                continue
            site = CppSite(
                sql=normalise(spans[literal_id][2]),
                line=line,
                form="C_STATEMENT",
                literal_id=literal_id,
            )
            sites.append(site)
            active[variable] = site
            continue

        if kind == "bind":
            variable = matched.group(1)
            method = matched.group(2)
            if method in NON_BIND_METHODS:
                continue
            site = active.get(variable)
            if site is None:
                problems.append(f"{label}:{line} 선언을 찾지 못한 문장 변수의 바인드: {variable}")
                continue
            full_method = "Bind" + method
            if full_method not in KNOWN_BIND_METHODS:
                problems.append(f"{label}:{line} 알 수 없는 바인드 형식: {full_method}")
                site.binds.append(CppBind(index=None, method=full_method, expr="", line=line))
                continue
            if len(texts) < 2:
                problems.append(f"{label}:{line} 바인드 인자가 모자라다: {full_method}")
                continue
            index = int(texts[0]) if texts[0].isdigit() else None
            if index is None:
                problems.append(
                    f"{label}:{line} 바인드 인덱스가 정수 리터럴이 아니다: {texts[0]!r}"
                )
            site.binds.append(CppBind(index=index, method=full_method, expr=texts[1], line=line))
            continue

        # ExecuteBoundInt64(_database, reinterpret_cast<const char*>(SQL_X), _nValue)
        if len(texts) != 3:
            problems.append(f"{label}:{line} ExecuteBoundInt64 인자 수가 3 이 아니다")
            continue
        constant = re.sub(r"^reinterpret_cast\s*<[^>]*>\s*\(", "", texts[1]).rstrip(")").strip()
        literal_id = constants.get(constant)
        if literal_id is None:
            problems.append(f"{label}:{line} SQL 상수를 찾지 못했다: {constant!r}")
            continue
        site = CppSite(
            sql=normalise(spans[literal_id][2]),
            line=line,
            form="ExecuteBoundInt64",
            literal_id=literal_id,
        )
        # 헬퍼가 인덱스 1 에 값 하나만 넣는다(migration_runner.cpp:68).
        site.binds.append(CppBind(index=1, method="BindInt64", expr=texts[2], line=line))
        sites.append(site)

    # 센서스. `?` 를 담은 리터럴이 어느 바인드 자리에도 붙지 않았으면 실패다.
    attached = {site.literal_id for site in sites}
    for index, (start, _end, body) in enumerate(spans):
        if index in attached:
            continue
        count, unsupported = scan_placeholders(normalise(body))
        if count or unsupported:
            problems.append(
                f"{label}:{_line_of(source, start)} 바인드 자리에 붙지 않은 SQL 리터럴: "
                f"{statement_key(body)[:60]!r}"
            )
    return sites, problems


# ---------------------------------------------------------------------------------------
# 대조
# ---------------------------------------------------------------------------------------
def _check_indices(binds: Sequence[CppBind], label: str, line: int, expected: int) -> list[str]:
    problems: list[str] = []
    indices = [bind.index for bind in binds]
    if any(index is None for index in indices):
        return problems  # 이미 위반으로 보고된 자리다.
    resolved = [index for index in indices if index is not None]
    if len(resolved) != expected:
        problems.append(
            f"{label}:{line} `?` {expected} 개에 바인드 {len(resolved)} 회 - 개수가 어긋난다"
        )
    if sorted(resolved) != list(range(1, len(resolved) + 1)):
        problems.append(f"{label}:{line} 바인드 인덱스가 1..N 이 아니다: {resolved}")
    elif resolved != sorted(resolved):
        problems.append(f"{label}:{line} 바인드가 인덱스 오름차순으로 놓이지 않았다: {resolved}")
    return problems


def compare_pair(
    py_source: str,
    cpp_source: str,
    py_label: str,
    cpp_label: str,
) -> tuple[list[str], int]:
    """(위반 목록, 대조한 문장 수)."""
    py_list, problems = python_sites(py_source, py_label)
    cpp_list, cpp_problems = cpp_sites(cpp_source, cpp_label)
    problems = list(problems) + cpp_problems

    for site in py_list:
        count, unsupported = scan_placeholders(site.sql)
        for token in unsupported:
            problems.append(f"{py_label}:{site.line} 지원하지 않는 파라미터 표기: {token!r}")
        if count != len(site.params):
            problems.append(
                f"{py_label}:{site.line} `?` {count} 개에 파라미터 {len(site.params)} 개"
            )

    for site in cpp_list:
        count, unsupported = scan_placeholders(site.sql)
        for token in unsupported:
            problems.append(f"{cpp_label}:{site.line} 지원하지 않는 파라미터 표기: {token!r}")
        problems.extend(_check_indices(site.binds, cpp_label, site.line, count))

    # 짝짓기는 **파라미터가 있는 문장만** 한다. `?` 가 없는 문장에는 지킬 바인드 순서가
    # 없고, 양쪽이 그런 문장을 담는 방식도 다르다 - 마이그레이션 이식본은 파라미터 없는
    # 문장을 배열·상수로 두고 `Execute` 로 흘려보내므로 여기서 짝을 요구하면 순서와
    # 무관한 잡음만 나온다. 그 문장들의 존재·본문은 SQL 축자 게이트 둘이 이미 지킨다.
    # `?` 를 담고도 바인드 자리에 붙지 않은 문장은 짝짓기가 아니라 센서스가 잡는다.
    py_groups: dict[str, list[PySite]] = {}
    for site in py_list:
        if scan_placeholders(site.sql)[0] == 0:
            continue
        py_groups.setdefault(statement_key(site.sql), []).append(site)
    cpp_groups: dict[str, list[CppSite]] = {}
    for site in cpp_list:
        if scan_placeholders(site.sql)[0] == 0:
            continue
        cpp_groups.setdefault(statement_key(site.sql), []).append(site)

    compared = 0
    for key, py_sites_for_key in sorted(py_groups.items()):
        cpp_sites_for_key = cpp_groups.get(key, [])
        if len(cpp_sites_for_key) != len(py_sites_for_key):
            problems.append(
                f"{py_label}:{py_sites_for_key[0].line} 짝 개수가 다르다"
                f"(파이썬 {len(py_sites_for_key)} vs C++ {len(cpp_sites_for_key)}): {key[:60]!r}"
            )
            continue
        for py_site, cpp_site in zip(py_sites_for_key, cpp_sites_for_key, strict=True):
            compared += 1
            problems.extend(_compare_values(py_site, cpp_site, py_label, cpp_label))

    for key, cpp_sites_for_key in sorted(cpp_groups.items()):
        if key not in py_groups:
            problems.append(
                f"{cpp_label}:{cpp_sites_for_key[0].line} 파이썬 쪽에 짝이 없는 문장: {key[:60]!r}"
            )
    return problems, compared


def _compare_values(
    py_site: PySite, cpp_site: CppSite, py_label: str, cpp_label: str
) -> list[str]:
    problems: list[str] = []
    ordered = sorted(
        (bind for bind in cpp_site.binds if bind.index is not None),
        key=lambda bind: bind.index or 0,
    )
    if len(ordered) != len(py_site.params):
        # 개수는 이미 위 단계가 보고했다. 값 대조는 성립하지 않는다.
        return problems

    for position, (py_expr, cpp_bind) in enumerate(
        zip(py_site.params, ordered, strict=True), start=1
    ):
        try:
            py_owner, py_name = canonical_py(py_expr)
        except MappingError as error:
            problems.append(f"{py_label}:{py_site.line} 인덱스 {position} 사상 실패 - {error}")
            continue
        try:
            cpp_owner, cpp_name = canonical_cpp(cpp_bind.expr)
        except MappingError as error:
            problems.append(f"{cpp_label}:{cpp_bind.line} 인덱스 {position} 사상 실패 - {error}")
            continue
        if py_name != cpp_name:
            problems.append(
                f"{cpp_label}:{cpp_bind.line} 인덱스 {position} 값 대응 어긋남 - "
                f"파이썬 {ast.unparse(py_expr)!r}({py_name}) vs C++ {cpp_bind.expr!r}({cpp_name})"
            )
        elif py_owner is not None and cpp_owner is not None and py_owner != cpp_owner:
            problems.append(
                f"{cpp_label}:{cpp_bind.line} 인덱스 {position} 값 소유자 어긋남 - "
                f"파이썬 {py_owner!r} vs C++ {cpp_owner!r}"
            )
    return problems


# ---------------------------------------------------------------------------------------
# 실행
# ---------------------------------------------------------------------------------------
def display_path(path: Path) -> str:
    try:
        return path.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return str(path)


def default_pairs() -> tuple[list[tuple[Path, Path]], list[str]]:
    pairs: list[tuple[Path, Path]] = [(DEFAULT_PY_REPOSITORIES, DEFAULT_CPP_REPOSITORIES)]
    missing: list[str] = []
    for python_path in sorted(DEFAULT_PY_MIGRATIONS.glob("v[0-9][0-9][0-9][0-9]_*.py")):
        cpp_path = DEFAULT_CPP_MIGRATIONS / (python_path.stem + ".cpp")
        if not cpp_path.is_file():
            missing.append(f"C++ 이식본이 없다: {display_path(cpp_path)}")
        pairs.append((python_path, cpp_path))
    return pairs, missing


def run_pairs(pairs: Sequence[tuple[Path, Path]]) -> tuple[list[str], int]:
    problems: list[str] = []
    compared = 0
    for python_path, cpp_path in pairs:
        py_source = python_path.read_text(encoding="utf-8")
        cpp_source = cpp_path.read_text(encoding="utf-8-sig")
        found, count = compare_pair(
            normalise(py_source),
            normalise(cpp_source),
            display_path(python_path),
            display_path(cpp_path),
        )
        problems.extend(found)
        compared += count
    return problems, compared


def _fixture(name: str) -> str:
    path = FIXTURE_DIR / name
    return normalise(path.read_text(encoding="utf-8-sig"))


def run_self_test() -> int:
    """통과해야 할 것이 통과하고 잡아야 할 것이 잡히는지 본다."""
    required = [
        "good.py",
        "good.cpp",
        "bad_duplicate_index.cpp",
        "bad_skipped_index.cpp",
        "bad_swapped_adjacent.cpp",
        "bad_bind_count.cpp",
        "bad_descending_order.cpp",
        "bad_orphan_literal.cpp",
        "bad_unmappable_expression.cpp",
        "bad_unmappable_param.py",
    ]
    absent = [name for name in required if not (FIXTURE_DIR / name).is_file()]
    if absent:
        print(f"  FAIL  fixture 가 없다: {absent}", file=sys.stderr)
        return 2

    good_py = _fixture("good.py")
    good_cpp = _fixture("good.cpp")

    failures = 0
    accepted, compared = compare_pair(good_py, good_cpp, "good.py", "good.cpp")
    if accepted:
        print(f"  FAIL  정상 표본이 거절됐다: {accepted}")
        failures += 1
    elif compared == 0:
        print("  FAIL  정상 표본에서 대조한 문장이 0 건이다")
        failures += 1
    else:
        print(f"  PASS  정상 표본 수용(문장 {compared} 건 대조)")

    # 기대 사유까지 맞춰 본다. "무엇이든 걸렸다" 로는 엉뚱한 이유로 통과한 자기시험을
    # 가려낼 수 없다 - 결함 표본은 심은 결함 때문에 거부돼야 한다.
    cases = [
        ("인덱스 중복", good_py, "bad_duplicate_index.cpp", "인덱스가 1..N 이 아니다"),
        ("인덱스 건너뜀", good_py, "bad_skipped_index.cpp", "인덱스가 1..N 이 아니다"),
        ("이웃한 동형 바인드 뒤바뀜", good_py, "bad_swapped_adjacent.cpp", "값 대응 어긋남"),
        ("바인드 수 불일치", good_py, "bad_bind_count.cpp", "개수가 어긋난다"),
        ("인덱스 역순 배치", good_py, "bad_descending_order.cpp", "오름차순으로 놓이지 않았다"),
        ("바인드에 안 붙은 리터럴", good_py, "bad_orphan_literal.cpp", "바인드 자리에 붙지 않은"),
        ("사상 불가 C++ 식", good_py, "bad_unmappable_expression.cpp", "사상 실패"),
        (
            "사상 불가 파이썬 파라미터",
            _fixture("bad_unmappable_param.py"),
            "good.cpp",
            "사상 실패",
        ),
    ]
    for name, py_source, cpp_name, expected in cases:
        found, _ = compare_pair(py_source, _fixture(cpp_name), "case.py", "case.cpp")
        hit = [line for line in found if expected in line]
        if hit:
            print(f"  PASS  결함 표본 거부: {name} ({hit[0]})")
        else:
            print(f"  FAIL  결함 표본({name})을 기대 사유로 못 잡았다: {found}")
            failures += 1

    # 실제 트리에서 바인드 자리가 실제로 뽑히는지도 본다. 0 건 추출이 "전건 통과" 로
    # 보고되는 경로를 닫는다 - 아무것도 비교하지 않은 게이트는 통과가 아니다.
    if DEFAULT_PY_REPOSITORIES.is_file() and DEFAULT_CPP_REPOSITORIES.is_file():
        sites, _ = cpp_sites(
            normalise(DEFAULT_CPP_REPOSITORIES.read_text(encoding="utf-8-sig")),
            display_path(DEFAULT_CPP_REPOSITORIES),
        )
        bound = [site for site in sites if site.binds]
        if bound:
            print(f"  PASS  실제 이식본에서 바인드 문장 {len(bound)} 건 추출")
        else:
            print("  FAIL  실제 이식본에서 바인드 문장을 하나도 못 뽑았다")
            failures += 1

    print(
        "자기시험 PASS: 정상 1건 수용, 결함 8종 전건 거부, 실제 이식본 추출 확인"
        if failures == 0
        else f"자기시험 실패 {failures} 건"
    )
    return 1 if failures else 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--self-test", action="store_true", help="게이트 자신을 양방향 검증한다")
    parser.add_argument("--python", type=Path, help="파이썬 원본 경로(한 쌍만 검사)")
    parser.add_argument("--cpp", type=Path, help="C++ 이식본 경로(한 쌍만 검사)")
    args = parser.parse_args(argv)

    if args.self_test:
        return run_self_test()

    if (args.python is None) != (args.cpp is None):
        print("오류: --python 과 --cpp 는 함께 준다.", file=sys.stderr)
        return 2

    if args.python is not None and args.cpp is not None:
        pairs = [(args.python, args.cpp)]
        missing: list[str] = []
    else:
        pairs, missing = default_pairs()

    for message in missing:
        print(f"오류: {message}", file=sys.stderr)
    if missing:
        return 2

    for python_path, cpp_path in pairs:
        for path in (python_path, cpp_path):
            if not path.is_file():
                print(f"오류: 경로가 없다 - {path}", file=sys.stderr)
                return 2

    try:
        problems, compared = run_pairs(pairs)
    except OSError as error:
        print(f"오류: 읽지 못했다 - {error}", file=sys.stderr)
        return 2
    except SyntaxError as error:
        print(f"오류: 파이썬 원본을 파싱하지 못했다 - {error}", file=sys.stderr)
        return 2

    # 위반을 먼저 게워 낸다. 대조 0 건이어도 위반이 있으면 그것이 보고할 내용이지,
    # 환경 오류로 접어 숨길 것이 아니다.
    if problems:
        for line in problems:
            print("위반:", line, file=sys.stderr)
        return 1

    if compared == 0:
        print("오류: 대조 대상 0건 - 바인드 자리를 하나도 찾지 못했다.", file=sys.stderr)
        return 2

    print(f"통과: 바인드 문장 {compared} 건, 인덱스·순서·값 대응 전건 일치")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
