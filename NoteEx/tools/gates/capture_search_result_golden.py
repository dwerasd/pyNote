from __future__ import annotations

import argparse
import os
import sys
import tempfile
from collections.abc import Iterator
from dataclasses import replace
from pathlib import Path
from typing import Any


def main() -> int:
    parser=argparse.ArgumentParser();parser.add_argument("--source-root",required=True,type=Path);parser.add_argument("--output",required=True,type=Path);args=parser.parse_args()
    source_root=args.source_root.resolve();os.environ["QT_QPA_PLATFORM"]="offscreen";sys.path.insert(0,str(source_root/"src"))
    import pynote
    from PySide6.QtCore import Qt
    from PySide6.QtWidgets import QApplication
    from pynote.application.card_service import CardService
    from pynote.domain.models import CaptureOperationSource,Card,CardSource,Document
    from pynote.infrastructure.database import Database
    from pynote.infrastructure.repositories import Repositories
    from pynote.ui.panels.document_navigator import DocumentNavigator
    from pynote.ui.search_dialog import SearchDialog

    def verify_imports()->None:
        for name,module in tuple(sys.modules.items()):
            if name!="pynote" and not name.startswith("pynote."):continue
            value=getattr(module,"__file__",None)
            if value is not None and not Path(value).resolve().is_relative_to(source_root):raise RuntimeError(f"pynote import escaped source root: {name}={value}")
    verify_imports()
    if not Path(pynote.__file__).resolve().is_relative_to(source_root):raise RuntimeError("root pynote import escaped")
    application=QApplication.instance() or QApplication([]);records:list[str]=[];widgets:list[Any]=[]
    def hx(value:str)->str:return value.encode().hex()
    def doc(identifier:str,title:str)->Document:return Document(identifier,title,1000,1000)
    def card(identifier:str,document_id:str,body:str)->Card:return Card(identifier,document_id,"operation",0,1,1000,1000,CardSource.TYPING,body,"",None)
    def serialize(name:str,query:str,state:str,rows:list[tuple[str,str,str|None,str,str]])->None:
        encoded=",".join(f"{kind}:{hx(document_id)}:{hx(card_id) if card_id is not None else '-'}:{hx(title)}:{hx(match)}" for kind,document_id,card_id,title,match in rows) or "-"
        records.append(f"{name}|query={hx(query)}|state={state}|count={len(rows)}|rows={encoded}")
    def dialog_rows(dialog:SearchDialog)->list[tuple[str,str,str|None,str,str]]:
        result=[]
        for index in range(dialog.result_tree.topLevelItemCount()):
            item=dialog.result_tree.topLevelItem(index);label=item.text(0)
            if label not in {"문서 제목","카드 본문"}:raise RuntimeError(f"unexpected row label: {label}")
            result.append(("title" if label=="문서 제목" else "card",str(item.data(0,Qt.ItemDataRole.UserRole)),item.data(1,Qt.ItemDataRole.UserRole),item.text(1),item.text(2)))
        return result
    class Scripted:
        def __init__(self,documents:list[Document],cards:list[Card],fallback:Document|None=None)->None:self.documents=documents;self.cards=cards;self.fallback=fallback;self.document_calls=0;self.card_calls=0;self.get_calls=0
        def search_documents(self,_query:str)->tuple[Document,...]:self.document_calls+=1;return tuple(self.documents)
        def search_cards(self,_query:str)->tuple[Card,...]:self.card_calls+=1;return tuple(self.cards)
        def get_document(self,_id:str)->Document|None:self.get_calls+=1;return self.fallback
        def list_documents(self)->tuple[Document,...]:return tuple(self.documents)
        def list_cards(self,_id:str)->tuple[Card,...]:return ()
    def run_dialog(name:str,raw:str,source:Any)->list[tuple[str,str,str|None,str,str]]:
        dialog=SearchDialog(source);widgets.append(dialog);dialog.search(raw);application.processEvents();normalized=raw.strip();rows=dialog_rows(dialog)
        state="needs-query" if not normalized else "results"
        expected_label="검색어를 입력하세요." if state=="needs-query" else f"{len(rows)}개 결과"
        if dialog.result_label.text()!=expected_label:raise RuntimeError("localized result label mismatch")
        serialize(name,normalized,state,rows);return rows
    def identifiers()->Iterator[str]:
        number=0
        while True:
            number+=1
            yield f"operation-{number}";yield f"card-{number}";yield f"revision-{number}";yield f"event-{number}"
    class Fixture:
        def __init__(self,root:Path,name:str,title:str,body:str,archived:bool)->None:
            self.database=Database(root/f"{name}.sqlite3");self.repositories=Repositories(self.database);self.document=doc("doc-1",title)
            if archived:self.document=replace(self.document,archived_at_us=1500)
            self.repositories.create_document(self.document);ids=identifiers();service=CardService(self.database,self.repositories,clock=lambda:2000,id_factory=lambda:next(ids));service.create_cards("doc-1",body,source=CaptureOperationSource.TYPING,split=False)

    source=Scripted([],[]);run_dialog("empty-unicode-strip","\u3000\u00a0\t",source)
    if (source.document_calls,source.card_calls,source.get_calls)!=(0,0,0):raise RuntimeError("empty query called source")

    source=Scripted([doc("doc-b","Straße"),doc("doc-a","neutral")],[]);navigator=DocumentNavigator(source);widgets.append(navigator);navigator.search_edit.setText(" \u00a0STRASSE\u3000");application.processEvents()
    visible=navigator.visible_document_ids()
    if visible!=("doc-b",):raise RuntimeError(f"navigator identity mismatch: {visible}")
    item=navigator.document_list.item(0)
    if item.toolTip()!="Straße":raise RuntimeError("navigator title label mismatch")
    serialize("navigator-title-casefold","STRASSE","results",[("title","doc-b",None,"Straße","Straße")])

    with tempfile.TemporaryDirectory() as temporary:
        root=Path(temporary);fixture=Fixture(root,"global","Straße","STRASSE anchor",True);run_dialog("global-title-casefold","STRASSE",fixture.repositories)
        fixture.database.close();fixture=Fixture(root,"body","neutral","Straße",False);run_dialog("body-like-not-casefold","STRASSE",fixture.repositories);fixture.database.close()

    run_dialog("result-order","a",Scripted([doc("doc-b","aB"),doc("doc-a","aA")],[card("card-a2","doc-a","a2"),card("card-a1","doc-a","a1"),card("card-b","doc-b","b")]))
    fallback=Scripted([], [card("card-x","doc-x","match")],doc("doc-x","Fallback"));run_dialog("fallback-document-title","match",fallback)
    if fallback.get_calls!=1:raise RuntimeError("fallback document call count mismatch")
    body160="a"*156+"😀\n\rz";run_dialog("preview-160","x",Scripted([], [card("card-160","doc-p",body160)]))
    body161="a"*156+"😀bcde";rows=run_dialog("preview-161","x",Scripted([], [card("card-161","doc-p",body161)]))
    if rows[0][4]!="a"*156+"😀…":raise RuntimeError("161-scalar preview mismatch")
    if len(records)!=8:raise RuntimeError(f"expected 8 records, got {len(records)}")
    verify_imports();args.output.write_text("\n".join(records)+"\n",encoding="ascii",newline="\n")
    for widget in widgets:widget.close();application.processEvents()
    return 0

if __name__=="__main__":raise SystemExit(main())
