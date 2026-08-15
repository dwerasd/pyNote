from __future__ import annotations

import logging
import time
import uuid
from collections.abc import Callable

from pynote.domain.models import Document
from pynote.infrastructure.repositories import Repositories

LOGGER = logging.getLogger(__name__)


def next_default_title(repositories: Repositories) -> str:
    """저장된 문서 제목과 겹치지 않는 기본 제목을 반환한다."""
    titles = {document.title for document in repositories.list_documents()}
    if "새 문서" not in titles:
        return "새 문서"
    suffix = 2
    while f"새 문서 {suffix}" in titles:
        suffix += 1
    return f"새 문서 {suffix}"


def create_document(
    repositories: Repositories,
    title: str | None = None,
    *,
    clock_us: Callable[[], int] | None = None,
) -> Document:
    """새 문서를 저장하고 반환한다."""
    normalized_title = (
        next_default_title(repositories) if title is None else title.strip()
    )
    if not normalized_title:
        raise ValueError("문서 제목은 비어 있을 수 없습니다.")
    now_us = (clock_us or (lambda: time.time_ns() // 1_000))()
    document = Document(
        id=str(uuid.uuid4()),
        title=normalized_title,
        created_at_us=now_us,
        updated_at_us=now_us,
    )
    try:
        repositories.create_document(document)
    except BaseException:
        LOGGER.exception("문서 생성 저장에 실패했습니다.")
        raise
    return document
