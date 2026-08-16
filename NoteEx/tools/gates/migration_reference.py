#!/usr/bin/env python3
"""마이그레이션 게이트 공용 지원 모듈(게이트가 아니다).

`check_migration_sql_parity.py`, `check_migration_ladder_parity.py`,
`check_schema_parity.py`, `make_ladder_fixtures.py` 가 함께 쓰는 것들만 모았다.
이 모듈은 판정하지 않는다 - 판정은 게이트가 하고, 이 모듈은 **파이썬 원본의
동작을 그대로 재현**하는 일만 한다.

이 모듈에도 스키마 SQL 이 한 줄도 없다. 마이그레이션은 원본 모듈을 적재해
**직접 호출**하며, 등록 순서는 원본 `migrations/__init__.py` 의 `MIGRATIONS`
튜플을 그대로 읽는다. 사본을 든 게이트는 아무것도 증명하지 못한다.

원본 적재 방식:
  `<저장소>/src` 를 `sys.path` 앞에 넣고 `pynote.infrastructure.migrations` 를
  가져온다. 패키지 **설치는 필요 없다** - `pynote/__init__.py` 와
  `pynote/infrastructure/__init__.py` 는 docstring 뿐이고 마이그레이션 모듈은
  표준 라이브러리만 가져오기 때문이다. 적재된 모듈이 이 저장소의 것인지
  `__file__` 로 확인하므로, 다른 곳에 설치된 동명 패키지가 가로채면 오류로 멈춘다.

정규화 규칙은 이 모듈이 아니라 **각 게이트의 모듈 docstring** 이 소유한다.
여기 있는 `dump_schema` / `dump_data` 는 규칙의 구현일 뿐이고, 읽는 사람이
규칙을 찾을 자리는 게이트 문서다.
"""

from __future__ import annotations

import importlib
import re
import sqlite3
import subprocess
import sys
from pathlib import Path
from typing import Any, Callable, Iterable, NamedTuple, Sequence

# 저장소 루트. 이 파일은 <루트>/NoteEx/tools/gates/ 에 있다.
REPO_ROOT = Path(__file__).resolve().parents[3]
SRC_ROOT = REPO_ROOT / "src"
PYTHON_MIGRATIONS_DIR = SRC_ROOT / "pynote" / "infrastructure" / "migrations"
CPP_MIGRATIONS_DIR = (
    REPO_ROOT / "NoteEx" / "core" / "src" / "storage" / "migrations"
)
DEFAULT_TEST_EXE = REPO_ROOT / "NoteEx" / "x64" / "ReleaseMD" / "NoteExTests.exe"

REGISTRY_MODULE = "pynote.infrastructure.migrations"

# sqlite_master 덤프 질의. rootpage 제외·결정적 정렬은 게이트 docstring 의 계약이다.
SCHEMA_QUERY = (
    "SELECT type, name, tbl_name, sql FROM sqlite_master "
    "ORDER BY type, name, tbl_name"
)

# v0004 가 randomblob/random 으로 만드는 UUIDv4 문자열의 모양.
# 값 자체는 비결정적이라 대조할 수 없지만 **모양은 계약**이라 이것으로 검사한다.
UUID4_PATTERN = re.compile(
    r"^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"
)


class MigrationEntry(NamedTuple):
    """원본 `MIGRATIONS` 튜플 1건 + 그 원본 파일 경로."""

    version: int
    migrate: Callable[..., None]
    source: Path

    @property
    def stem(self) -> str:
        """`v0001_initial` 처럼 확장자 없는 파일 이름. C++ 짝을 찾는 근거다."""
        return self.source.stem


class CapturedStatement(NamedTuple):
    """`migrate()` 가 실제로 발행한 문장 1건."""

    version: int
    index: int  # 그 마이그레이션 안에서의 발행 순서(0-base)
    sql: str
    bound: bool  # 바인드 파라미터를 함께 넘겼는가


class SchemaDump(NamedTuple):
    """sqlite_master 덤프. objects 는 행 수다."""

    text: str
    objects: int


class DataDump(NamedTuple):
    """행 데이터 덤프. violations 는 diff 와 무관하게 게이트를 실패시킨다."""

    text: str
    rows: int
    violations: list[str]


class NormalisationRule(NamedTuple):
    """비결정 열 1건의 처리 규칙.

    `writer_version` 은 그 열에 값을 써 넣는 마이그레이션 버전이다. 대조 구간
    안에서 실행되면 양쪽 러너가 각자 값을 만들어 내므로 정규화 대상이고,
    구간 밖이면 fixture 가 준 값을 양쪽이 그대로 나르므로 **축자 대조 대상**이다.
    `None` 은 "마이그레이션마다 다시 쓴다"는 뜻이라 언제나 정규화 대상이다.
    """

    table: str
    column: str
    writer_version: int | None
    validator: str  # 'positive_int' | 'uuid4'
    placeholder: str


# 비결정 열 목록. 근거는 각 게이트 docstring 의 정규화 규칙 절에 적어 둔다.
NORMALISATION_RULES: tuple[NormalisationRule, ...] = (
    NormalisationRule("schema_version", "applied_at_us", None, "positive_int", "<applied_at_us>"),
    NormalisationRule("data_policy_settings", "updated_at_us", 2, "positive_int", "<updated_at_us>"),
    NormalisationRule("workspace_windows", "window_id", 4, "uuid4", "<window_id:uuid4>"),
)


def display_path(path: Path) -> str:
    """경로를 슬래시로 정규화해 셸·로그 어디서나 같은 문자열이 되게 한다."""
    return str(path).replace("\\", "/")


def escape(text: str) -> str:
    """표시용 단사 이스케이프. 정규화가 아니다.

    양쪽 덤프에 똑같이 적용하므로 서로 다른 원문이 같은 덤프가 되는 일이 없다.
    눈에 보이지 않는 CR 차이를 diff 에서 읽으려는 표기일 뿐이다.
    """
    return (
        text.replace("\\", "\\\\")
        .replace("\r", "\\r")
        .replace("\n", "\\n")
        .replace("\t", "\\t")
    )


def force_utf8_output() -> None:
    """콘솔 로케일(Windows 기본 cp949)과 무관하게 UTF-8 로 출력한다."""
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="backslashreplace")


def decode_process_output(raw: bytes) -> str:
    """자식 프로세스 출력 해독. 콘솔 로케일이 cp949 인 환경을 함께 받아 준다."""
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode("cp949", errors="replace")


def tail(text: str, limit: int = 20) -> str:
    """로그 전문 덤프를 피하고 꼬리만 인용한다."""
    lines = text.strip().splitlines()
    if not lines:
        return "(출력 없음)"
    shown = lines[-limit:]
    prefix = "" if len(lines) <= limit else f"(마지막 {limit}줄)\n"
    return prefix + "\n".join(f"      {line}" for line in shown)


def load_registry() -> tuple[MigrationEntry, ...]:
    """원본 `MIGRATIONS` 튜플을 등록 순서 그대로 돌려준다.

    설치본이 가로채면 오류다 - 이 게이트가 대조하는 것은 작업 트리의 원본이지
    어딘가에 설치된 사본이 아니다.
    """
    source_root = str(SRC_ROOT)
    if source_root not in sys.path:
        sys.path.insert(0, source_root)

    module = importlib.import_module(REGISTRY_MODULE)
    loaded = Path(getattr(module, "__file__", "")).resolve()
    if SRC_ROOT not in loaded.parents:
        raise ImportError(
            f"{REGISTRY_MODULE} 을 이 저장소 밖에서 적재했다: {display_path(loaded)}. "
            f"기대 경로는 {display_path(SRC_ROOT)} 아래다."
        )

    migrations = getattr(module, "MIGRATIONS", None)
    if not isinstance(migrations, tuple) or not migrations:
        raise AttributeError(f"MIGRATIONS 가 없거나 비었다: {display_path(loaded)}")

    entries: list[MigrationEntry] = []
    for position, item in enumerate(migrations):
        if not isinstance(item, tuple) or len(item) != 2:
            raise TypeError(f"MIGRATIONS[{position}] 의 모양이 (version, migrate) 가 아니다.")
        version, migrate = item
        if not isinstance(version, int) or not callable(migrate):
            raise TypeError(f"MIGRATIONS[{position}] 의 형이 (int, callable) 이 아니다.")
        owner = sys.modules.get(migrate.__module__)
        source = getattr(owner, "__file__", None)
        if source is None:
            raise AttributeError(f"MIGRATIONS[{position}] 의 원본 파일을 찾을 수 없다.")
        entries.append(MigrationEntry(version, migrate, Path(source).resolve()))

    versions = [entry.version for entry in entries]
    if versions != sorted(versions) or len(set(versions)) != len(versions):
        raise ValueError(f"MIGRATIONS 의 버전이 오름차순 유일하지 않다: {versions}")
    return tuple(entries)


def latest_version(entries: Sequence[MigrationEntry]) -> int:
    """원본 `LATEST_SCHEMA_VERSION` 과 같은 값(= 마지막 등록 버전)."""
    return entries[-1].version


def find_cpp_source(entry: MigrationEntry, cpp_dir: Path | None = None) -> Path | None:
    """마이그레이션 1건의 C++ 짝을 찾는다.

    관례는 파일 이름 일치(`v0003_storage_invariants.py` -> `.cpp`)다. 관례를
    벗어난 이름이라도 `v0003_*.cpp` 가 정확히 하나면 그것으로 본다 - 파일 이름
    선택으로 게이트가 거짓 환경 오류를 내지 않게 하려는 것이다.
    """
    root = CPP_MIGRATIONS_DIR if cpp_dir is None else cpp_dir
    by_name = root / f"{entry.stem}.cpp"
    if by_name.is_file():
        return by_name
    candidates = sorted(root.glob(f"v{entry.version:04d}_*.cpp"))
    if len(candidates) == 1:
        return candidates[0]
    return None


def open_connection(db_path: Path) -> sqlite3.Connection:
    """`database.py` `_open`(68~82행)의 연결 수명주기를 그대로 재현한다.

    autocommit(`isolation_level=None`), `foreign_keys` 를 켠 뒤 되읽어 검증,
    `journal_mode = WAL` 은 반환값으로 검증한다. 검증 실패 시 연결을 닫고 예외를
    던지는 것까지 원본과 같다.
    """
    db_path.parent.mkdir(parents=True, exist_ok=True)
    connection = sqlite3.connect(db_path, isolation_level=None)
    connection.row_factory = sqlite3.Row
    connection.execute("PRAGMA foreign_keys = ON")
    foreign_keys = connection.execute("PRAGMA foreign_keys").fetchone()
    if foreign_keys is None or foreign_keys[0] != 1:
        connection.close()
        raise RuntimeError("SQLite foreign_keys 활성화에 실패했습니다.")

    journal_mode = connection.execute("PRAGMA journal_mode = WAL").fetchone()
    if journal_mode is None or str(journal_mode[0]).lower() != "wal":
        connection.close()
        raise RuntimeError("SQLite WAL 모드 활성화 검증에 실패했습니다.")
    return connection


def read_schema_version(connection: sqlite3.Connection) -> int:
    """`database.py` `_read_schema_version`(84~95행)을 그대로 재현한다."""
    table = connection.execute(
        """
        SELECT 1
        FROM sqlite_master
        WHERE type = 'table' AND name = 'schema_version'
        """
    ).fetchone()
    if table is None:
        return 0
    row = connection.execute(
        "SELECT version FROM schema_version WHERE id = 1"
    ).fetchone()
    return 0 if row is None else int(row[0])


def database_version(db_path: Path) -> int:
    """파일 하나의 schema version 을 읽는다(없으면 0)."""
    connection = sqlite3.connect(db_path)
    try:
        connection.row_factory = sqlite3.Row
        return read_schema_version(connection)
    finally:
        connection.close()


def apply_migrations(
    connection: sqlite3.Connection,
    entries: Iterable[MigrationEntry],
    applied_at_us: int,
    wrapper: Callable[[MigrationEntry], Any] | None = None,
) -> None:
    """`database.py` `_migrate`(97~124행)의 트랜잭션 구조를 그대로 재현한다.

    보류 목록 **전체가 한 트랜잭션**이고 실패 시 전량 롤백이다. `wrapper` 는
    마이그레이션마다 connection 대신 넘길 객체를 만들어 주는 훅으로, 문장 기록
    프록시를 끼우는 데 쓴다(없으면 원본 연결을 그대로 넘긴다).
    """
    pending = list(entries)
    if not pending:
        return
    connection.execute("BEGIN IMMEDIATE")
    try:
        for entry in pending:
            target = connection if wrapper is None else wrapper(entry)
            entry.migrate(target, applied_at_us)
        connection.commit()
    except BaseException:
        connection.rollback()
        raise


def build_database(
    db_path: Path,
    entries: Sequence[MigrationEntry],
    applied_at_us: int,
    upto: int | None = None,
    start_version: int = 0,
) -> None:
    """빈 파일에서 시작해 `upto` 버전까지 파이썬 러너로 올린다."""
    selected = [
        entry
        for entry in entries
        if entry.version > start_version and (upto is None or entry.version <= upto)
    ]
    connection = open_connection(db_path)
    try:
        apply_migrations(connection, selected, applied_at_us)
    finally:
        connection.close()


def upgrade_database(
    db_path: Path, entries: Sequence[MigrationEntry], applied_at_us: int
) -> int:
    """이미 있는 데이터베이스를 최신 버전까지 올린다. 시작 버전을 돌려준다."""
    connection = open_connection(db_path)
    try:
        current = read_schema_version(connection)
        pending = [entry for entry in entries if entry.version > current]
        apply_migrations(connection, pending, applied_at_us)
        return current
    finally:
        connection.close()


class RecordingConnection:
    """`migrate()` 가 발행하는 문장을 순서대로 기록하며 실제 연결로 중계한다.

    정적 게이트가 소스를 파싱하지 않고 **실제 발행 문장**을 잡는 근거다. 원본이
    문장을 모듈 상수로 노출하든(v0001), 인라인으로 부르든(v0002 이후), 지역
    튜플로 묶든(v0003) 결과가 같다.

    `execute` 만 중계한다. 다른 속성 접근은 오류다 - `executescript` 나
    `cursor().execute()` 로 우회하면 이 게이트가 문장을 **조용히** 놓치므로,
    조용한 누락 대신 시끄러운 실패를 택한다. 원본이 그런 경로를 쓰게 되면
    프록시를 넓히고 자기시험을 함께 추가해야 한다.
    """

    def __init__(self, connection: sqlite3.Connection, version: int) -> None:
        self._connection = connection
        self._version = version
        self.captured: list[CapturedStatement] = []

    def execute(self, sql: str, parameters: Any = None) -> sqlite3.Cursor:
        self.captured.append(
            CapturedStatement(
                version=self._version,
                index=len(self.captured),
                sql=sql,
                bound=parameters is not None,
            )
        )
        if parameters is None:
            return self._connection.execute(sql)
        return self._connection.execute(sql, parameters)

    def __getattr__(self, name: str) -> Any:
        raise AttributeError(
            f"기록 프록시는 execute 만 중계한다(요청: {name}). 원본 migrate 가 다른 "
            "경로로 문장을 보내면 이 게이트가 문장을 조용히 놓치므로 실패로 막는다. "
            "프록시를 넓히고 자기시험을 함께 추가해야 한다."
        )


def capture_statements(
    entries: Sequence[MigrationEntry], db_path: Path, applied_at_us: int
) -> dict[int, tuple[CapturedStatement, ...]]:
    """사다리를 실제로 올리면서 버전별 발행 문장을 기록한다.

    실행을 흉내 내지 않고 **진짜 실행**한다. v0005 의 `ALTER TABLE` 처럼 앞
    버전의 상태가 있어야 성립하는 문장이 있고, v0003 은 DDL 앞에서 기존 행을
    조회하기 때문이다.
    """
    recorders: dict[int, RecordingConnection] = {}

    def wrapper(entry: MigrationEntry) -> RecordingConnection:
        recorder = RecordingConnection(connection, entry.version)
        recorders[entry.version] = recorder
        return recorder

    connection = open_connection(db_path)
    try:
        apply_migrations(connection, entries, applied_at_us, wrapper=wrapper)
    finally:
        connection.close()

    return {
        version: tuple(recorder.captured) for version, recorder in recorders.items()
    }


def dump_schema(db_path: Path) -> SchemaDump:
    """sqlite_master 를 결정적 순서로 덤프한다(게이트 docstring 의 정규화 규칙)."""
    connection = sqlite3.connect(db_path)
    try:
        rows = connection.execute(SCHEMA_QUERY).fetchall()
    finally:
        connection.close()

    lines: list[str] = []
    for row_type, name, tbl_name, sql in rows:
        lines.append(
            f"### type={escape(str(row_type))} "
            f"name={escape(str(name))} "
            f"tbl_name={escape(str(tbl_name))}"
        )
        if sql is None:
            lines.append("sql: <NULL>")
            continue
        segments = str(sql).split("\n")
        lines.append(f"sql: {len(segments)}줄")
        lines.extend(f"  |{escape(segment)}" for segment in segments)

    return SchemaDump(text="\n".join(lines) + ("\n" if lines else ""), objects=len(rows))


def _encode_value(value: Any) -> str:
    """행 값 1개를 형까지 드러나게 표기한다. 형이 바뀌면 그것도 발산이다."""
    if value is None:
        return "NULL"
    if isinstance(value, int):
        return f"INT:{value}"
    if isinstance(value, float):
        return f"REAL:{value!r}"
    if isinstance(value, str):
        return f"TEXT:{escape(value)}"
    if isinstance(value, (bytes, bytearray)):
        return f"BLOB:{bytes(value).hex()}"
    return f"OTHER:{type(value).__name__}:{escape(str(value))}"


def _validate_normalised(rule: NormalisationRule, value: Any) -> str | None:
    """정규화 대상 값이 그 열의 계약을 지키는지 본다. 위반이면 사유 문자열."""
    if rule.validator == "positive_int":
        if not isinstance(value, int) or value <= 0:
            return f"{rule.table}.{rule.column} 이 양의 정수가 아니다: {value!r}"
        return None
    if rule.validator == "uuid4":
        if not isinstance(value, str) or UUID4_PATTERN.match(value) is None:
            return (
                f"{rule.table}.{rule.column} 이 v0004 가 만드는 UUIDv4 모양이 "
                f"아니다: {value!r}"
            )
        return None
    return f"알 수 없는 검증기: {rule.validator}"


def active_rules(start_version: int) -> tuple[NormalisationRule, ...]:
    """대조 구간 `(start_version, 최신]` 안에서 값이 생성되는 열만 고른다."""
    return tuple(
        rule
        for rule in NORMALISATION_RULES
        if rule.writer_version is None or rule.writer_version > start_version
    )


def data_tables(connection: sqlite3.Connection) -> list[str]:
    """대조 대상 테이블 이름. 내부 테이블 중 sqlite_sequence 만 포함한다.

    AUTOINCREMENT 최고값은 사용자 데이터의 일부라 뺄 이유가 없고, 나머지
    `sqlite_%` 는 SQLite 내부 구현이다.
    """
    rows = connection.execute(
        "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name"
    ).fetchall()
    names = [str(row[0]) for row in rows]
    return [
        name
        for name in names
        if not name.startswith("sqlite_") or name == "sqlite_sequence"
    ]


def dump_data(db_path: Path, start_version: int) -> DataDump:
    """행 데이터를 결정적 순서로 덤프한다.

    비결정 열은 `start_version` 기준으로 정규화 여부가 갈린다(NormalisationRule
    참조). 정규화한 값은 자리표시자로 바뀌지만 **검증은 하고 넘어간다** - 양쪽이
    똑같이 망가지면 diff 가 비어 통과하는 구멍을 막기 위해 위반을 따로 모은다.
    """
    rules = {
        (rule.table, rule.column): rule for rule in active_rules(start_version)
    }
    violations: list[str] = []
    lines: list[str] = []
    total_rows = 0

    connection = sqlite3.connect(db_path)
    try:
        for table in data_tables(connection):
            columns = [
                str(row[1])
                for row in connection.execute(f'PRAGMA table_info("{table}")').fetchall()
            ]
            if not columns:
                lines.append(f"### table={escape(table)} (열 없음)")
                continue
            selected = ", ".join(f'"{column}"' for column in columns)
            rows = connection.execute(f'SELECT {selected} FROM "{table}"').fetchall()

            encoded_rows: list[str] = []
            for row in rows:
                cells: list[str] = []
                for column, value in zip(columns, row):
                    rule = rules.get((table, column))
                    if rule is None:
                        cells.append(f"{column}={_encode_value(value)}")
                        continue
                    problem = _validate_normalised(rule, value)
                    if problem is not None:
                        violations.append(problem)
                    cells.append(f"{column}={rule.placeholder}")
                encoded_rows.append(" | ".join(cells))

            encoded_rows.sort()
            total_rows += len(encoded_rows)
            lines.append(
                f"### table={escape(table)} columns={escape(','.join(columns))} "
                f"rows={len(encoded_rows)}"
            )
            lines.extend(f"  |{line}" for line in encoded_rows)
    finally:
        connection.close()

    return DataDump(
        text="\n".join(lines) + ("\n" if lines else ""),
        rows=total_rows,
        violations=violations,
    )


class TestListing(NamedTuple):
    """Catch2 태그 조회 결과."""

    matched: int
    returncode: int
    output: str


def count_matching_tests(exe: Path, tag: str, timeout: float = 60.0) -> TestListing:
    """실행 파일이 그 태그를 아는지 본다.

    **이 조회가 필요한 이유**: 태그가 없는 낡은 실행 파일에
    `NoteExTests.exe "[parity-upgrade]"` 를 주면 "No test cases matched" 를 찍고
    **종료 코드 0** 을 낸다(2026-08-16 실측). 종료 코드만 보면 낡은 실행 파일이
    조용히 통과한다. XML 리포터의 `<TestCase>` 수로 태그 존재를 먼저 확인한다.

    XML 은 파싱하지 않고 `<TestCase>` 를 센다 - 시험 이름이 CP949 로 실려 나와
    UTF-8 선언과 어긋나므로 XML 파서는 거부한다(같은 날 실측).
    """
    completed = subprocess.run(
        [str(exe), "--list-tests", tag, "--reporter", "xml"],
        cwd=str(exe.parent),
        capture_output=True,
        timeout=timeout,
    )
    output = decode_process_output(completed.stdout) + decode_process_output(
        completed.stderr
    )
    return TestListing(
        matched=output.count("<TestCase>"),
        returncode=completed.returncode,
        output=output,
    )
