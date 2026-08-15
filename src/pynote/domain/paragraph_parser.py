from __future__ import annotations

from typing import Protocol


class ParagraphSplitPolicy(Protocol):
    """텍스트를 논리 문단으로 나누는 교체 가능한 정책이다."""

    def split(self, text: str) -> tuple[str, ...]:
        """텍스트에서 비어 있지 않은 논리 문단을 반환한다."""
        ...


class BlankLineParagraphPolicy:
    """하나 이상의 빈 줄을 문단 경계로 사용하는 기본 정책이다."""

    def split(self, text: str) -> tuple[str, ...]:
        """CRLF를 LF로 정규화하고 빈 줄 경계를 제외한 문단을 반환한다."""
        normalized_text = text.replace("\r\n", "\n")
        paragraphs: list[str] = []
        current_lines: list[str] = []

        for line in normalized_text.split("\n"):
            if line.strip():
                current_lines.append(line)
            elif current_lines:
                paragraphs.append("\n".join(current_lines))
                current_lines = []

        if current_lines:
            paragraphs.append("\n".join(current_lines))

        return tuple(paragraphs)


class ParagraphParser:
    """정책에 따라 문단을 감지하고 keep 원문을 보존하는 순수 파서다."""

    def __init__(self, policy: ParagraphSplitPolicy | None = None) -> None:
        self._policy = policy if policy is not None else BlankLineParagraphPolicy()

    def split(self, text: str) -> tuple[str, ...]:
        """현재 정책으로 텍스트를 논리 문단으로 나눈다."""
        return self._policy.split(text)

    def is_zero_paragraph_input(self, text: str) -> bool:
        """카드로 제출할 논리 문단이 없는 입력인지 판정한다."""
        return not self.split(text)

    def keep(self, text: str) -> str:
        """분리하지 않는 입력을 어떤 정규화도 하지 않고 반환한다."""
        return text
