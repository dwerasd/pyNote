from __future__ import annotations

import argparse
import os
import stat
import sys
import tempfile
from pathlib import Path

# 파일 결속 골든 벡터 캡처. 네이티브 emit(tests/unit/file_binding_service_test.cpp) 과
# **바이트까지 같은** 줄을 낸다 - 비교는 정규화 없이 그대로 한다.
#
# ANSI(mbcs) 벡터는 이 기계의 코드 페이지(CP949)에 종속되므로 양쪽을 같은 기계에서 뽑는다.


def _hex(data: bytes) -> str:
    return data.hex()


def _hex_text(text: str) -> str:
    return text.encode("utf-8").hex()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    source_root = args.source_root.resolve()
    sys.path.insert(0, str(source_root / "src"))

    import pynote
    from pynote.application.file_binding_service import (
        FileSyncOutcome,
        detect_text,
        hash_bytes,
        render_bytes,
        resolve_path,
        sync_file,
    )
    from pynote.domain.events import EventSource
    from pynote.domain.models import (
        CaptureOperationSource,
        CardSource,
        Document,
        FileBinding,
        NewCaptureOperation,
        NewCard,
        NewlineKind,
        RevisionSource,
        SplitPolicy,
    )
    from pynote.infrastructure.database import Database
    from pynote.infrastructure.repositories import Repositories

    resolved_package = Path(pynote.__file__).resolve()
    if not resolved_package.is_relative_to(source_root):
        raise RuntimeError(f"pynote import escaped source root: {resolved_package}")

    ansi = "mbcs"
    bom_by_encoding = {
        "utf-8": b"\xef\xbb\xbf",
        "utf-16-le": b"\xff\xfe",
        "utf-16-be": b"\xfe\xff",
    }

    body_unicode = "첫 줄 한글 \U0001F600\n두 번째 줄 ASCII\n세 번째"
    body_ansi = "첫 줄 한글\n두 번째 줄 ASCII\n세 번째"
    body_json = '{\n  "이름": "값",\n  "목록": [1, 2, 3]\n}'

    encodings = (("utf-8", False), ("utf-8", True), ("utf-16-le", True), ("utf-16-be", True), (ansi, False))
    newlines = (NewlineKind.LF, NewlineKind.CRLF)
    trailings = (True, False)

    def binding_for(encoding: str, bom: bool, newline: NewlineKind, trailing: bool) -> FileBinding:
        return FileBinding(
            card_id="card-1",
            path="C:\\notes\\sample.txt",
            path_key="c:\\notes\\sample.txt",
            encoding=encoding,
            bom=bom,
            newline=newline,
            trailing_newline=trailing,
            bound_at_us=1_000,
        )

    def source_bytes(body: str, encoding: str, bom: bool, newline: NewlineKind, trailing: bool) -> bytes:
        text = body.replace("\n", newline.characters)
        if trailing:
            text += newline.characters
        prefix = bom_by_encoding[encoding] if bom else b""
        return prefix + text.encode(encoding)

    lines: list[str] = []

    # ---------------------------------------------------------------------------- D 벡터(감지)
    def detect_line(stable_id: str, data: bytes) -> str:
        detected = detect_text(data)
        fields = [stable_id, f"input={_hex(data)}", f"ok={1 if detected is not None else 0}"]
        if detected is None:
            fields += ["text=", "encoding=", "bom=", "newline=", "trailing="]
            return "|".join(fields)
        fields += [
            f"text={_hex_text(detected.text)}",
            f"encoding={detected.encoding}",
            f"bom={int(detected.bom)}",
            f"newline={detected.newline.value}",
            f"trailing={int(detected.trailing_newline)}",
        ]
        return "|".join(fields)

    detect_index = 1

    def next_detect_id() -> str:
        nonlocal detect_index
        stable_id = f"FB-D{detect_index:03d}"
        detect_index += 1
        return stable_id

    for encoding, bom in encodings:
        body = body_ansi if encoding == ansi else body_unicode
        for newline in newlines:
            for trailing in trailings:
                data = source_bytes(body, encoding, bom, newline, trailing)
                lines.append(detect_line(next_detect_id(), data))

    lines.append(detect_line(next_detect_id(), source_bytes(body_json, "utf-8", False, NewlineKind.CRLF, True)))
    lines.append(detect_line(next_detect_id(), b""))
    lines.append(detect_line(next_detect_id(), b"  \t  "))
    lines.append(detect_line(next_detect_id(), b"a\nb\r\nc"))
    lines.append(detect_line(next_detect_id(), b"a\r\nb\nc"))
    lines.append(detect_line(next_detect_id(), b"a\rb\nc"))
    lines.append(detect_line(next_detect_id(), b"a\rb"))
    lines.append(detect_line(next_detect_id(), b"\x80"))
    lines.append(detect_line(next_detect_id(), "앞\t\f\v뒤".encode()))

    lines.append(detect_line(next_detect_id(), b"\x89PNG\r\n\x1a\n\x00\x00\x00\r"))
    lines.append(detect_line(next_detect_id(), "앞\u00a0뒤".encode()))
    lines.append(detect_line(next_detect_id(), "앞\u2028뒤".encode()))
    lines.append(detect_line(next_detect_id(), "앞\u2029뒤".encode()))
    lines.append(detect_line(next_detect_id(), "앞\ufdd0뒤".encode()))
    lines.append(detect_line(next_detect_id(), "앞\ufdd1뒤".encode()))
    lines.append(detect_line(next_detect_id(), b"\xff"))
    lines.append(detect_line(next_detect_id(), "앞\u0001뒤".encode()))
    lines.append(detect_line(next_detect_id(), "앞\u007f뒤".encode()))
    lines.append(detect_line(next_detect_id(), b"\xef\xbb\xbf\xff"))
    lines.append(detect_line(next_detect_id(), b"\xff\xfeA"))
    lines.append(detect_line(next_detect_id(), b"\xff\xfe\x00\xd8"))

    # ---------------------------------------------------------------------------- R 벡터(렌더)
    def render_line(stable_id: str, text: str, target: FileBinding) -> str:
        try:
            rendered = render_bytes(text, target)
        except (UnicodeEncodeError, LookupError, ValueError):
            return f"{stable_id}|ok=0|bytes="
        return f"{stable_id}|ok=1|bytes={_hex(rendered)}"

    render_index = 1

    def next_render_id() -> str:
        nonlocal render_index
        stable_id = f"FB-R{render_index:03d}"
        render_index += 1
        return stable_id

    for encoding, bom in encodings:
        body = body_ansi if encoding == ansi else body_unicode
        for newline in newlines:
            for trailing in trailings:
                text = body + "\n" if trailing else body
                lines.append(render_line(next_render_id(), text, binding_for(encoding, bom, newline, trailing)))

    lines.append(render_line(next_render_id(), "이모지 \U0001F642", binding_for(ansi, False, NewlineKind.LF, False)))
    lines.append(render_line(next_render_id(), "\u00c0", binding_for(ansi, False, NewlineKind.LF, False)))
    lines.append(render_line(next_render_id(), "본문", binding_for(ansi, True, NewlineKind.LF, False)))
    lines.append(render_line(next_render_id(), "body", binding_for("no-such-encoding", False, NewlineKind.LF, False)))

    # ---------------------------------------------------------------------------- H 벡터(해시)
    for index, data in enumerate((b"", "본문\n".encode(), b"\x00\x01\xff\xfe\x80"), start=1):
        lines.append(f"FB-H{index:03d}|input={_hex(data)}|hash={hash_bytes(data)}")

    # ---------------------------------------------------------------------------- 공통 카드 준비
    def create_card(repositories: Repositories, body: str):
        repositories.create_document(
            Document(id="document-1", title="T", created_at_us=1, updated_at_us=1)
        )
        created = repositories.create_cards(
            NewCaptureOperation(
                id="operation-1",
                document_id="document-1",
                source=CaptureOperationSource.IMPORT,
                split_policy=SplitPolicy.KEEP,
                original_text=None,
                created_at_us=10,
            ),
            [
                NewCard(
                    id="card-1",
                    revision_id="revision-1",
                    event_id="event-1",
                    position_key=1_024,
                    body=body,
                    card_source=CardSource.IMPORT,
                    event_source=EventSource.IMPORT,
                    revision_source=RevisionSource.EDIT,
                    created_at_us=10,
                )
            ],
        )
        return created[0]

    def optional_text(value: object) -> str:
        return "" if value is None else str(value)

    # ---------------------------------------------------------------------------- B 벡터(저장소)
    def binding_line(stable_id: str, binding: FileBinding) -> str:
        return "|".join(
            [
                stable_id,
                f"card_id={binding.card_id}",
                f"path={_hex_text(binding.path)}",
                f"path_key={_hex_text(binding.path_key)}",
                f"encoding={binding.encoding}",
                f"bom={int(binding.bom)}",
                f"newline={binding.newline.value}",
                f"trailing={int(binding.trailing_newline)}",
                f"synced_size={optional_text(binding.synced_size)}",
                f"synced_mtime_ns={optional_text(binding.synced_mtime_ns)}",
                f"synced_hash={optional_text(binding.synced_hash)}",
                f"bound_at_us={binding.bound_at_us}",
                f"synced_at_us={optional_text(binding.synced_at_us)}",
            ]
        )

    with tempfile.TemporaryDirectory(prefix="fb_golden_db_") as db_root:
        with Database(Path(db_root) / "golden.sqlite3") as database:
            repositories = Repositories(database)
            card = create_card(repositories, "본문\n")
            distinct = FileBinding(
                card_id=card.id,
                path="C:/Notes/Mixed/A.TXT",
                path_key="c:\\notes\\mixed\\a.txt",
                encoding="utf-16-be",
                bom=True,
                newline=NewlineKind.CRLF,
                trailing_newline=False,
                bound_at_us=1_000_000,
                synced_size=4_242,
                synced_mtime_ns=1_700_000_000_123_456_789,
                synced_hash="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                synced_at_us=2_000_000,
            )
            repositories.upsert_file_binding(distinct)
            by_card = repositories.get_file_binding(card.id)
            assert by_card is not None
            lines.append(binding_line("FB-B001", by_card))
            by_path = repositories.find_binding_by_path(distinct.path_key)
            assert by_path is not None
            lines.append(binding_line("FB-B002", by_path))

    # ---------------------------------------------------------------------------- S 벡터(동기)
    def temp_left(directory: Path) -> int:
        return 1 if any(entry.name.endswith(".tmp") for entry in directory.iterdir()) else 0

    def sync_line(stable_id: str, outcome: FileSyncOutcome, path: Path, directory: Path,
                  repositories: Repositories, card_id: str) -> str:
        stored = repositories.get_file_binding(card_id)
        file_field = _hex(path.read_bytes()) if path.exists() else "absent"
        return "|".join(
            [
                stable_id,
                outcome.value,
                f"file={file_field}",
                f"synced_hash={optional_text(stored.synced_hash) if stored else ''}",
                f"synced_size={optional_text(stored.synced_size) if stored else ''}",
                f"synced_at_us={optional_text(stored.synced_at_us) if stored else ''}",
                f"temp_left={temp_left(directory)}",
            ]
        )

    class Scenario:
        """네이티브 C_SYNC_SCENARIO 와 같은 준비물이다 - 임시 디렉터리 + 임시 DB + 결속 카드."""

        def __init__(self, stack_root: Path, name: str) -> None:
            self.directory = stack_root / name
            self.directory.mkdir(parents=True)
            self.db_directory = stack_root / (name + "_db")
            self.db_directory.mkdir(parents=True)
            self.database = Database(self.db_directory / "golden.sqlite3")
            self.repositories = Repositories(self.database)

        def bind(self, data: bytes, name: str = "note.txt", synced: bool = True) -> None:
            path = self.directory / name
            path.write_bytes(data)
            detected = detect_text(data)
            assert detected is not None
            self.card = create_card(self.repositories, detected.text)
            path_string, path_key = resolve_path(path)
            stat_result = path.stat()
            self.binding = FileBinding(
                card_id=self.card.id,
                path=path_string,
                path_key=path_key,
                encoding=detected.encoding,
                bom=detected.bom,
                newline=detected.newline,
                trailing_newline=detected.trailing_newline,
                bound_at_us=1_000,
                synced_size=stat_result.st_size if synced else None,
                synced_mtime_ns=stat_result.st_mtime_ns if synced else None,
                synced_hash=hash_bytes(data) if synced else None,
                synced_at_us=1_000 if synced else None,
            )
            self.repositories.upsert_file_binding(self.binding)
            self.path = Path(path_string)

        def edited(self, body: str):
            from dataclasses import replace as dataclass_replace

            return dataclass_replace(self.card, body=body)

        def close(self) -> None:
            self.database.close()

    with tempfile.TemporaryDirectory(prefix="fb_golden_sync_") as sync_root:
        root = Path(sync_root)

        def run(name: str, prepare, card_factory, *, force: bool = False, interactive: bool = False,
                clock: int = 2_000, twice: bool = False) -> tuple:
            scenario = Scenario(root, name)
            prepare(scenario)
            target = card_factory(scenario)
            if twice:
                sync_file(scenario.repositories, target, force=force, interactive=interactive,
                          clock=lambda: 2_000)
                result = sync_file(scenario.repositories, target, force=force, interactive=interactive,
                                   clock=lambda: 3_000)
            else:
                result = sync_file(scenario.repositories, target, force=force, interactive=interactive,
                                   clock=lambda: clock)
            return scenario, result

        def emit(stable_id: str, scenario: Scenario, result) -> None:
            lines.append(sync_line(stable_id, result.outcome, scenario.path, scenario.directory,
                                   scenario.repositories, scenario.card.id))
            scenario.close()

        def bind_plain(scenario: Scenario) -> None:
            scenario.bind("본문\n".encode())

        scenario, result = run("s001", bind_plain, lambda s: s.card)
        emit("FB-S001", scenario, result)

        scenario, result = run("s002", bind_plain, lambda s: s.edited("편집한 본문\n"))
        emit("FB-S002", scenario, result)

        def bind_and_remove(scenario: Scenario) -> None:
            bind_plain(scenario)
            scenario.path.unlink()

        scenario, result = run("s003", bind_and_remove, lambda s: s.card)
        emit("FB-S003", scenario, result)

        def bind_unsynced(scenario: Scenario) -> None:
            scenario.bind("본문\n".encode(), synced=False)

        scenario, result = run("s004", bind_unsynced, lambda s: s.edited("다른 본문\n"))
        emit("FB-S004", scenario, result)

        def bind_external(scenario: Scenario) -> None:
            bind_plain(scenario)
            scenario.path.write_bytes("바깥에서 바꾼 본문\n".encode())

        scenario, result = run("s005", bind_external, lambda s: s.edited("편집한 본문\n"))
        emit("FB-S005", scenario, result)

        scenario, result = run("s006", bind_external, lambda s: s.edited("편집한 본문\n"), interactive=True)
        emit("FB-S006", scenario, result)

        scenario, result = run("s007", bind_external, lambda s: s.edited("편집한 본문\n"),
                               interactive=True, twice=True)
        emit("FB-S007", scenario, result)

        scenario, result = run("s008", bind_external, lambda s: s.edited("편집한 본문\n"), force=True)
        emit("FB-S008", scenario, result)

        scenario, result = run("s009", bind_external, lambda s: s.edited("바깥에서 바꾼 본문\n"))
        emit("FB-S009", scenario, result)

        def bind_and_unbind(scenario: Scenario) -> None:
            bind_plain(scenario)
            scenario.repositories.delete_file_binding(scenario.card.id)

        scenario, result = run("s010", bind_and_unbind, lambda s: s.edited("편집한 본문\n"))
        emit("FB-S010", scenario, result)

        # 교체 실패 주입. 원본 시험이 os.replace 를 가로채는 자리와 같다.
        scenario = Scenario(root, "s011")
        bind_plain(scenario)
        real_replace = os.replace

        def refuse(source: object, destination: object) -> None:
            raise OSError(13, "교체 거부 주입")

        os.replace = refuse  # type: ignore[assignment]
        try:
            result = sync_file(scenario.repositories, scenario.edited("편집한 본문\n"), clock=lambda: 2_000)
        finally:
            os.replace = real_replace  # type: ignore[assignment]
        emit("FB-S011", scenario, result)

        # 읽기 전용 대상.
        scenario = Scenario(root, "s012")
        bind_plain(scenario)
        os.chmod(scenario.path, stat.S_IREAD)
        try:
            result = sync_file(scenario.repositories, scenario.edited("편집한 본문\n"), clock=lambda: 2_000)
        finally:
            os.chmod(scenario.path, stat.S_IWRITE)
        emit("FB-S012", scenario, result)

        # 표현할 수 없는 문자.
        def bind_ansi(scenario: Scenario) -> None:
            scenario.bind(source_bytes("본문", ansi, False, NewlineKind.LF, True), name="ansi.txt")

        scenario, result = run("s013", bind_ansi, lambda s: s.edited("이모지 \U0001F642\n"))
        emit("FB-S013", scenario, result)

    # ---------------------------------------------------------------------------- P 벡터(경로)
    root_path, root_key = resolve_path(source_root)

    def path_line(stable_id: str, relative: str) -> str:
        resolved, key = resolve_path(root_path + "\\" + relative)
        if not resolved.startswith(root_path) or not key.startswith(root_key):
            raise RuntimeError(f"path vector escaped the source root: {resolved}")
        return "|".join(
            [
                stable_id,
                f"path={_hex_text('<ROOT>' + resolved[len(root_path):])}",
                f"path_key={_hex_text('<ROOT>' + key[len(root_key):])}",
            ]
        )

    lines.append(path_line("FB-P001", "NoteEx/TOOLS/GATES/README.MD"))
    lines.append(path_line("FB-P002", "NoteEx/tools/gates/../gates/README.md"))
    lines.append(path_line("FB-P003", "src/pynote/domain/models.py"))
    lines.append(path_line("FB-P004", "NoteEx/tools/GATES/NoSuchFile.TXT"))

    # D 41 + R 24 + H 3 + B 2 + S 13 + P 4.
    if len(lines) != 87:
        raise RuntimeError(f"expected 87 golden lines, got {len(lines)}")
    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
