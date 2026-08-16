"""자기시험용 파이썬 원본 표본. 실제 시험 트리와 무관하게 게이트만 검증한다.

여기를 저장소의 진짜 `tests/` 를 가리키게 하면, 파이썬 시험 이름 하나가 바뀔 때마다
게이트의 자기시험이 게이트와 상관없는 이유로 깨진다.
"""


def test_alpha() -> None:
    pass


def test_beta() -> None:
    pass


class TestGroup:
    def test_gamma(self) -> None:
        pass


# 정의가 아니라 호출·언급이다. `ast` 판정이 아니라 문자열 검색으로 해소하는 게이트는
# 이 줄에 속아 `test_ghost` 를 실재한다고 말한다.
def _caller() -> None:
    test_ghost = "test_ghost"
    assert test_ghost
