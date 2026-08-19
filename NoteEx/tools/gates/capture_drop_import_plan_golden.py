from __future__ import annotations

import argparse
import codecs
import ctypes
import locale
import os
import sys
import tempfile
from pathlib import Path
from typing import IO, Any


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    source_root = args.source_root.resolve()
    os.environ["QT_QPA_PLATFORM"] = "offscreen"
    sys.path.insert(0, str(source_root / "src"))

    import pynote
    from PySide6.QtCore import QMimeData, QPointF, QSettings, Qt, QUrl
    from PySide6.QtGui import QDragEnterEvent, QDragMoveEvent, QDropEvent
    from PySide6.QtWidgets import QApplication
    from pynote.app import initialize_device_settings
    from pynote.application import document_service
    from pynote.domain.events import EventSource, EventType
    from pynote.domain.models import CaptureOperationSource, CardSource
    from pynote.infrastructure.database import Database
    from pynote.infrastructure.repositories import Repositories
    from pynote.ui.document_page import DocumentPage

    package = Path(pynote.__file__).resolve()
    if not package.is_relative_to(source_root):
        raise RuntimeError(f"pynote import escaped source root: {package}")
    application = QApplication.instance() or QApplication([])
    lines: list[str] = []
    resources: list[tuple[DocumentPage, Database]] = []
    fixture_number = 0

    def hx(text: str) -> str:
        return text.encode("utf-8").hex()

    def page_fixture(directory: Path) -> tuple[DocumentPage, Database, Repositories, list[tuple[str, str]]]:
        nonlocal fixture_number
        fixture_number += 1
        database = Database(directory / f"drop-{fixture_number}.sqlite3")
        repositories = Repositories(database)
        document = document_service.create_document(repositories, f"drop-{fixture_number}")
        settings = QSettings(str(directory / f"drop-{fixture_number}.ini"), QSettings.Format.IniFormat)
        initialize_device_settings(settings)
        settings.setValue("first_run/guide_shown", True)
        errors: list[tuple[str, str]] = []
        page = DocumentPage(database, repositories, document.id, settings=settings, error_reporter=lambda title, message: errors.append((title, message)))
        page.resize(900, 600)
        page.show()
        application.processEvents()
        resources.append((page, database))
        return page, database, repositories, errors

    def mime(paths: list[Path]) -> QMimeData:
        value = QMimeData()
        value.setUrls([QUrl.fromLocalFile(str(path)) for path in paths])
        return value

    def enter(page: DocumentPage, value: QMimeData) -> QDragEnterEvent:
        event = QDragEnterEvent(page.editor.cursorRect().center(), Qt.DropAction.CopyAction, value, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier)
        application.sendEvent(page.editor.viewport(), event)
        return event

    def drop(page: DocumentPage, paths: list[Path]) -> None:
        value = mime(paths)
        entered = enter(page, value)
        if not entered.isAccepted():
            raise RuntimeError("local file drop enter rejected")
        moved = QDragMoveEvent(page.editor.cursorRect().center(), Qt.DropAction.CopyAction, value, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier)
        application.sendEvent(page.editor.viewport(), moved)
        dropped = QDropEvent(QPointF(page.editor.cursorRect().center()), Qt.DropAction.CopyAction, value, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier)
        application.sendEvent(page.editor.viewport(), dropped)
        if not moved.isAccepted() or not dropped.isAccepted():
            raise RuntimeError("local file drop sequence rejected")
        application.processEvents()

    def cards(repositories: Repositories, document_id: str) -> tuple[Any, ...]:
        return repositories.list_cards(document_id)

    def post(created: int) -> str:
        return "none" if created == 0 else "connect-one" if created == 1 else "reveal-last"

    def counts(database: Database) -> tuple[int, int, int, int]:
        return tuple(int(database.connection.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]) for table in ("cards", "card_revisions", "capture_operations", "edit_events"))  # type: ignore[return-value]

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)

        page, _db, repo, errors = page_fixture(root)
        paths = [root / "LICENSE", root / ".gitignore", root / "manual.pdf"]
        bodies = ["확장자 없음", "닷파일", "임의 확장자"]
        for path, body in zip(paths, bodies, strict=True): path.write_text(body, encoding="utf-8")
        drop(page, paths); created = cards(repo, page.document_id)
        lines.append(f"WTL-W2-0054|paths={','.join(path.name for path in paths)}|bodies={','.join(hx(card.body) for card in created)}|post={post(len(created))}")

        page, _db, repo, errors = page_fixture(root)
        image, executable = root / "image.png", root / "program.exe"
        image_bytes = bytes.fromhex("89504e470d0a1a0a0000000d49484452"); exe_bytes = bytes.fromhex("4d5a9000030000000400ff")
        image.write_bytes(image_bytes); executable.write_bytes(exe_bytes); drop(page, [image, executable]); created = cards(repo, page.document_id)
        lines.append(f"WTL-W2-0055|image_input={image_bytes.hex()}|exe_input={exe_bytes.hex()}|cards={len(created)}|image_nonempty={int(bool(created[0].body))}|exe_prefix={created[1].body.encode()[:2].hex()}|errors={len(errors)}|post={post(len(created))}")

        page, _db, repo, _errors = page_fixture(root)
        path = root / "utf16.data"; original = "UTF-16 한글 원문\r\n둘째 줄"; raw = b"\xff\xfe" + original.encode("utf-16-le"); path.write_bytes(raw); drop(page, [path]); created = cards(repo, page.document_id)
        lines.append(f"WTL-W2-0056|input={raw.hex()}|body={hx(created[0].body)}|post={post(len(created))}")

        actual_acp = int(ctypes.windll.kernel32.GetACP())
        if actual_acp != 949 or codecs.lookup(locale.getencoding()).name != codecs.lookup("cp949").name:
            raise RuntimeError(f"WTL-W2-0057 blocked: system ACP is not 949 ({locale.getencoding()})")
        page, _db, repo, _errors = page_fixture(root)
        path = root / "ansi.data"; original = "CP949 한글 원문"; raw = original.encode("cp949"); path.write_bytes(raw); drop(page, [path]); created = cards(repo, page.document_id)
        lines.append(f"WTL-W2-0057|acp=949|applicable=1|input={raw.hex()}|body={hx(created[0].body)}|post={post(len(created))}")

        page, _db, repo, errors = page_fixture(root)
        bom, whitespace = root / "bom-only.txt", root / "space.txt"; bom.write_bytes(b"\xff\xfe"); whitespace.write_bytes(b" \t\r\n")
        gate_calls: list[bool] = []; original_gate = page.can_leave_editor; page.can_leave_editor = lambda **kwargs: gate_calls.append(bool(kwargs.get("protect_now"))) or original_gate(**kwargs)  # type: ignore[method-assign]
        before = (page.editor.card_id, page.editor.session, counts(_db)); drop(page, [bom, whitespace]); after = (page.editor.card_id, page.editor.session, counts(_db))
        lines.append(f"WTL-W2-0058|inputs=fffe,20090d0a|prepared=0|read_failures={len(errors[0][1].splitlines())-1}|gate={len(gate_calls)}|create={len(cards(repo,page.document_id))}|editor_state={'preserved' if before==after else 'changed'}")

        page, _db, repo, errors = page_fixture(root)
        first, oversized, unreadable, blank, last = (root / name for name in ("first.txt","oversized.txt","unreadable.md","empty-utf16-bom.txt","last.txt"))
        first.write_text("첫 유효 본문",encoding="utf-8");oversized.write_bytes(b"x"*(4*1024*1024+1));unreadable.write_text("x",encoding="utf-8");blank.write_bytes(b"\xff\xfe");last.write_text("둘째 유효 본문",encoding="utf-8")
        original_open=Path.open
        def failing_open(target: Path,*open_args: Any,**kwargs: Any)->IO[Any]:
            if target==unreadable and open_args and open_args[0]=="rb": raise OSError("injected")
            return original_open(target,*open_args,**kwargs)
        Path.open=failing_open  # type: ignore[method-assign]
        try: drop(page,[first,oversized,unreadable,blank,last])
        finally: Path.open=original_open  # type: ignore[method-assign]
        created=cards(repo,page.document_id)
        lines.append(f"WTL-W2-0059|created={','.join(hx(card.body) for card in created)}|failures=oversized.txt:file-too-large,unreadable.md:read-failed,empty-utf16-bom.txt:blank|post={post(len(created))}|editor={page.editor.card_id or '-'}")

        page, database, repo, _errors = page_fixture(root)
        first,last=root/"four-mib.md",root/"last.md";first.write_bytes(b"x"*(4*1024*1024));last.write_bytes(b"y");before=counts(database);gate_calls=[];page.can_leave_editor=lambda **kwargs: gate_calls.append(True) or True  # type: ignore[method-assign]
        drop(page,[first,last]);after=counts(database);delta=tuple(a-b for a,b in zip(after,before,strict=True))
        lines.append(f"WTL-W2-0060|first_bytes={first.stat().st_size}|next_bytes={last.stat().st_size}|batch_fatal=last.md:total-too-large|discarded_prepared=1|gate={len(gate_calls)}|created={len(cards(repo,page.document_id))}|db_delta={','.join(map(str,delta))}")

        page, _db, repo, _errors = page_fixture(root)
        path=root/"snapshot.txt";initial="최초 snapshot";later="두 번째 내용";path.write_text(initial,encoding="utf-8");original_open=Path.open;read_calls=[]
        class MutatingReader:
            def __init__(self,stream:IO[bytes])->None:self.stream=stream
            def __enter__(self)->MutatingReader:self.stream.__enter__();return self
            def __exit__(self,*values:object)->object:return self.stream.__exit__(*values)
            def read(self,size:int=-1)->bytes:
                data=self.stream.read(size)
                with original_open(path,"w",encoding="utf-8") as target: target.write(later)
                return data
        def mutating_open(target:Path,*open_args:Any,**kwargs:Any)->Any:
            stream=original_open(target,*open_args,**kwargs)
            if target==path and open_args and open_args[0]=="rb":read_calls.append(open_args);return MutatingReader(stream)
            return stream
        Path.open=mutating_open  # type: ignore[method-assign]
        try:drop(page,[path])
        finally:Path.open=original_open  # type: ignore[method-assign]
        created=cards(repo,page.document_id)
        lines.append(f"WTL-W2-0061|initial={hx(initial)}|created={hx(created[0].body)}|path_after={hx(path.read_text(encoding='utf-8'))}|read_calls={len(read_calls)}|read_limit=4194305")

        page, _db, repo, errors = page_fixture(root)
        remote=QMimeData();remote.setUrls([QUrl("https://example.com/note.md")]);nonlocal_result="enter-reject" if not enter(page,remote).isAccepted() else "accepted"
        directory,missing=root/"directory.md",root/"missing.txt";directory.mkdir(exist_ok=True);gate_calls=[];page.can_leave_editor=lambda **kwargs:gate_calls.append(True) or True  # type: ignore[method-assign]
        drop(page,[directory]);drop(page,[missing])
        lines.append(f"WTL-W2-0062|nonlocal={nonlocal_result}|directory=structural-reject|missing=structural-reject|reads=0|gate={len(gate_calls)}|created={len(cards(repo,page.document_id))}|reports={len(errors)}")

        page, _db, repo, _errors = page_fixture(root)
        duplicate=root/"duplicate.md";duplicate.write_text("한 번만",encoding="utf-8");drop(page,[duplicate,duplicate]);created=cards(repo,page.document_id)
        lines.append(f"WTL-W2-0063|input_paths=duplicate.md,duplicate.md|unique={len(created)}|body={hx(created[0].body)}|post={post(len(created))}")

        page, _db, repo, _errors = page_fixture(root)
        source=root/"source.txt";source.write_text("출처 확인",encoding="utf-8");drop(page,[source]);card=cards(repo,page.document_id)[0];operation=repo.get_capture_operation(card.operation_id);events=[event for event in repo.list_events(page.document_id) if event.card_id==card.id and event.event_type is EventType.CREATE]
        lines.append(f"WTL-W2-0064|card={card.source.value}|operation={operation.source.value}|event={events[0].source.value}|create_events={len(events)}|post=connect-one")

        page, _db, repo, errors = page_fixture(root)
        valid=root/"valid.md";valid.write_text("검증은 끝남",encoding="utf-8");gate_calls=[];page.can_leave_editor=lambda **kwargs:gate_calls.append(bool(kwargs.get('protect_now'))) or False  # type: ignore[method-assign]
        drop(page,[valid]);lines.append(f"WTL-W2-0065|reads=1|prepared=1|gate=protect-now:{'reject' if gate_calls==[True] else 'wrong'}|created={len(cards(repo,page.document_id))}|errors={len(errors)}|post=none")

        page, _db, repo, errors = page_fixture(root)
        blank,failure,success=root/"empty-utf16-bom.txt",root/"failure.md",root/"success.txt";blank.write_bytes(b"\xff\xfe");failure.write_text("생성 실패",encoding="utf-8");success.write_text("후속 성공",encoding="utf-8");original_create=page.card_service.create_cards;create_calls=[]
        def create_with_failure(document_id:str,body:str,**kwargs:Any)->Any:
            create_calls.append(body)
            if len(create_calls)==1:raise RuntimeError("injected")
            return original_create(document_id,body,**kwargs)
        page.card_service.create_cards=create_with_failure  # type: ignore[method-assign]
        drop(page,[blank,failure,success]);created=cards(repo,page.document_id)
        lines.append(f"WTL-W2-0066|blank_input=fffe|create_calls={','.join(hx(value) for value in create_calls)}|created={hx(created[0].body)}|read_failures=empty-utf16-bom.txt:blank|create_failures=failure.md:create-failed|post={post(len(created))}")

        page, _db, repo, _errors = page_fixture(root)
        many=[root/f"count-{index}.txt" for index in range(21)]
        for path in many:path.write_bytes(b"x")
        gate_calls=[];page.can_leave_editor=lambda **kwargs:gate_calls.append(True) or True  # type: ignore[method-assign]
        drop(page,many);lines.append(f"WTL-W2-0067|count={len(many)}|max=20|batch_fatal=too-many-files|gate={len(gate_calls)}|created={len(cards(repo,page.document_id))}")

        page, _db, repo, _errors = page_fixture(root)
        oversized=root/"oversized.md";oversized.write_bytes(b"x"*(4*1024*1024+1));gate_calls=[];page.can_leave_editor=lambda **kwargs:gate_calls.append(True) or True  # type: ignore[method-assign]
        drop(page,[oversized]);lines.append(f"WTL-W2-0068|bytes={oversized.stat().st_size}|read_limit=4194305|failures=oversized.md:file-too-large|prepared=0|gate={len(gate_calls)}|created={len(cards(repo,page.document_id))}")

        page, database, repo, _errors = page_fixture(root)
        extended=root/"extended-blank.txt";extended.write_bytes(b"\x0b\x0c");before=counts(database);drop(page,[extended]);after=counts(database);delta=tuple(a-b for a,b in zip(after,before,strict=True))
        lines.append(f"WTL-W2-0069|input=0b0c|result=blank|db_delta={','.join(map(str,delta))}|created={len(cards(repo,page.document_id))}|post=none")

        page, _db, repo, _errors = page_fixture(root)
        extended,valid=root/"extended-blank.txt",root/"valid.txt";extended.write_bytes(b"\x0b\x0c");valid.write_text("정상 카드",encoding="utf-8");drop(page,[extended,valid]);created=cards(repo,page.document_id)
        lines.append(f"WTL-W2-0070|blank_input=0b0c|valid_input={hx('정상 카드')}|created={hx(created[0].body)}|read_failures=extended-blank.txt:blank|post={post(len(created))}")

        page, _db, repo, _errors = page_fixture(root)
        blank=root/"vt-ff-nbsp.txt";raw=b"\x0b\x0c\xc2\xa0";blank.write_bytes(raw);create_calls=[];original_create=page.card_service.create_cards
        def observe_create(document_id:str,body:str,**kwargs:Any)->Any:create_calls.append(body);return original_create(document_id,body,**kwargs)
        page.card_service.create_cards=observe_create  # type: ignore[method-assign]
        drop(page,[blank]);lines.append(f"WTL-W2-0071|input={raw.hex()}|result=blank|create_calls={len(create_calls)}|cards={len(cards(repo,page.document_id))}|post=none")

        if len(lines) != 18: raise RuntimeError(f"expected 18 golden lines, got {len(lines)}")
        args.output.write_text("\n".join(lines)+"\n",encoding="utf-8",newline="\n")
        for page,database in resources:
            if page.editor.session is not None: page.editor.request_close()
            page.close();database.close()
        application.processEvents()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
