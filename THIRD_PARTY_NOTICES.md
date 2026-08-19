# 오픈소스 라이선스 고지

이 저장소는 소스를 배포하며 Python 런타임과 빌드된 실행 파일은 포함하지 않는다.
Python 실행·개발 의존성의 선언과 잠금 정보는 `pyproject.toml`, `uv.lock`,
`requirements.txt`, `requirements-dev.txt` 에 있고, 빌드 도구는 `build.bat` 이
직접 설치한다. 저장소에 포함된 NoteEx 네이티브 제3자 소스·자료는 아래에 별도로
고지한다.

Python 실행에 필요한 제3자 구성요소는 하나다.

- **PySide6 (Qt for Python)** — `LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only`,
  또는 유효한 Qt 상용 라이선스. 함께 설치되는 PySide6-Essentials,
  PySide6-Addons, shiboken6 도 같은 조건이다.

`build.bat` 은 빌드 시 **PyInstaller** 를 설치한다. GPLv2-or-later 이며, 이
도구로 패키징한 프로그램의 배포를 허용하는 예외 조항이 있다.

`build.bat` 이 만드는 실행 파일에는 Qt 바이너리가 함께 묶인다. 그 실행 파일을
남에게 배포하면 PySide6 의 라이선스 조건이 적용되므로 배포 전에 조건을 확인하라.
각 구성요소의 라이선스 식별자와 패키지에 동봉된 라이선스 파일은 설치된 배포
패키지의 `.dist-info` 디렉터리에서 확인할 수 있다.

## NoteEx 네이티브 구성요소

- **Google CCTZ v2.5** — Apache License 2.0. NoteEx는 commit
  `d2f2abda066d74c6e110b6be959d50bfb365917a`의 필요한 소스 파일을 포함한다.
  라이선스 전문과 upstream 정보는 `NoteEx/third_party/cctz/`에 있다.
- **IANA Time Zone Database 2026c** — public-domain 및 기여자별 고지 조건.
  NoteEx 실행 파일에 포함되는 TZif 자료의 설치 원본 고지는
  `NoteEx/third_party/tzdata/2026c/COPYRIGHT`에 있다.
- **Unicode Character Database 15.1.0 case-fold data** — Unicode License
  Agreement. 라이선스 전문은 `NoteEx/third_party/unicode/LICENSE.txt`에 있다.
