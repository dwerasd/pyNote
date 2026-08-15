from __future__ import annotations

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (
    QDialog,
    QLabel,
    QLineEdit,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

from pynote.infrastructure.repositories import Repositories


class SearchDialog(QDialog):
    """문서 제목과 활성 카드 본문을 SQLite LIKE로 함께 검색한다."""

    result_activated = Signal(str, object)

    def __init__(
        self,
        repositories: Repositories,
        *,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._repositories = repositories
        self.setObjectName("globalSearchDialog")
        self.setWindowTitle("문서와 카드 검색")
        self.resize(680, 480)

        self.query_edit = QLineEdit(self)
        self.query_edit.setObjectName("globalSearchEdit")
        self.query_edit.setPlaceholderText("문서 제목 또는 카드 본문 검색")
        self.query_edit.setClearButtonEnabled(True)
        self.query_edit.textChanged.connect(self.search)

        self.result_label = QLabel("검색어를 입력하세요.", self)
        self.result_tree = QTreeWidget(self)
        self.result_tree.setObjectName("globalSearchResults")
        self.result_tree.setHeaderLabels(("종류", "문서", "일치 내용"))
        self.result_tree.itemActivated.connect(self._activate)

        layout = QVBoxLayout(self)
        layout.addWidget(self.query_edit)
        layout.addWidget(self.result_label)
        layout.addWidget(self.result_tree, 1)

    def focus_search(self) -> None:
        """대화상자를 표시하고 검색 입력에 키보드 포커스를 둔다."""
        self.show()
        self.raise_()
        self.activateWindow()
        self.query_edit.setFocus()
        self.query_edit.selectAll()

    def search(self, query: str) -> None:
        """LIKE 검색 결과를 제목 행과 카드 본문 행으로 표시한다."""
        self.result_tree.clear()
        normalized = query.strip()
        if not normalized:
            self.result_label.setText("검색어를 입력하세요.")
            return
        documents = self._repositories.search_documents(normalized)
        cards = self._repositories.search_cards(normalized)
        titles = {document.id: document.title for document in documents}
        for document in documents:
            if normalized.casefold() not in document.title.casefold():
                continue
            item = QTreeWidgetItem(
                self.result_tree,
                ("문서 제목", document.title, document.title),
            )
            item.setData(0, Qt.ItemDataRole.UserRole, document.id)
            item.setData(1, Qt.ItemDataRole.UserRole, None)
        for card in cards:
            title = titles.get(card.document_id)
            if title is None:
                document = self._repositories.get_document(card.document_id)
                title = "" if document is None else document.title
            preview = card.body.replace("\n", " ")
            if len(preview) > 160:
                preview = f"{preview[:157]}…"
            item = QTreeWidgetItem(
                self.result_tree,
                ("카드 본문", title, preview),
            )
            item.setData(0, Qt.ItemDataRole.UserRole, card.document_id)
            item.setData(1, Qt.ItemDataRole.UserRole, card.id)
        count = self.result_tree.topLevelItemCount()
        self.result_label.setText(f"{count}개 결과")
        for column in range(3):
            self.result_tree.resizeColumnToContents(column)

    def _activate(self, item: QTreeWidgetItem) -> None:
        document_id = item.data(0, Qt.ItemDataRole.UserRole)
        card_id = item.data(1, Qt.ItemDataRole.UserRole)
        if isinstance(document_id, str):
            self.result_activated.emit(
                document_id,
                card_id if isinstance(card_id, str) else None,
            )
