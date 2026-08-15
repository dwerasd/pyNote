# 오픈소스 라이선스 고지

이 저장소는 소스만 배포한다. 제3자 패키지도, Python 런타임도, 빌드된 실행
파일도 포함되어 있지 않다 — 실행·개발·빌드에 필요한 패키지는 사용자가 직접
설치한다. 실행·개발 의존성의 선언과 잠금 정보는 `pyproject.toml`, `uv.lock`,
`requirements.txt`, `requirements-dev.txt` 에 있고, 빌드 도구는 `build.bat` 이
직접 설치한다.

실행에 필요한 제3자 구성요소는 하나다.

- **PySide6 (Qt for Python)** — `LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only`,
  또는 유효한 Qt 상용 라이선스. 함께 설치되는 PySide6-Essentials,
  PySide6-Addons, shiboken6 도 같은 조건이다.

`build.bat` 은 빌드 시 **PyInstaller** 를 설치한다. GPLv2-or-later 이며, 이
도구로 패키징한 프로그램의 배포를 허용하는 예외 조항이 있다.

`build.bat` 이 만드는 실행 파일에는 Qt 바이너리가 함께 묶인다. 그 실행 파일을
남에게 배포하면 PySide6 의 라이선스 조건이 적용되므로 배포 전에 조건을 확인하라.
각 구성요소의 라이선스 식별자와 패키지에 동봉된 라이선스 파일은 설치된 배포
패키지의 `.dist-info` 디렉터리에서 확인할 수 있다.
