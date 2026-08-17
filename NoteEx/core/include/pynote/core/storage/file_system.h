#pragma once

#include <cstdint>
#include <string>
#include <vector>

// core 계층. Win32/ATL/WTL/COM/DirectX 헤더를 포함하지 않는다(tools/gates 격리 게이트가 강제).

namespace pynote::core::storage
{
	// 백업 계층이 파일을 만지는 유일한 통로다. 파이썬 원본 infrastructure/backup.py 가 실제로
	// 부르는 연산만 담는다 - Path.exists(:113 :150), Path.is_file(:74 :111 :490),
	// Path.is_symlink(:489 :490), Path.mkdir(parents=True, exist_ok=True)(:115 :154),
	// os.replace(:128 :176 :178 :507), Path.unlink(missing_ok=True)(:137 :199 :515 :522),
	// tempfile.mkstemp + close(:531~540), Path.stat().st_mtime(:274), Path.glob(:271).
	// 호출부가 쓰지 않는 연산은 넣지 않는다 - 호출부보다 넓은 파일시스템 추상은 부채다.
	//
	// 원자적 교체(Replace)의 구현은 파이썬 os.replace 의 Windows 실측 동작을 따라야 한다.
	// 다른 원자 교체 API 로 바꾸면 내구성 동작이 달라지고, 그것은 이 파동이 허용한 개선 밖이다.
	//
	// 실패는 예외가 아니라 반환값으로 알린다. 사유는 LastError() 에 남는다.
	class I_FILE_SYSTEM
	{
	public:
		virtual ~I_FILE_SYSTEM() = default;

		// Path.exists() - 링크를 따라간 뒤의 존재 여부다.
		virtual bool Exists(const std::string& _sPath) const = 0;

		// Path.is_file() - 링크를 따라간 대상이 일반 파일인가.
		virtual bool IsRegularFile(const std::string& _sPath) const = 0;

		// Path.is_symlink() - 링크 자신을 본다(따라가지 않는다).
		virtual bool IsSymlink(const std::string& _sPath) const = 0;

		// Path.mkdir(parents=True, exist_ok=True). 이미 디렉터리면 성공이고, 같은 자리에
		// 일반 파일이 있으면 실패다(원본이 FileExistsError 를 올리는 자리).
		virtual bool CreateDirectories(const std::string& _sPath) = 0;

		// os.replace(_sFrom, _sTo). 대상이 없어도 성공해야 한다 - 원본은 백업 생성 경로에서
		// 존재하지 않는 대상으로 교체한다(:128).
		virtual bool Replace(const std::string& _sFrom, const std::string& _sTo) = 0;

		// Path.unlink(missing_ok=True). 대상이 없으면 성공이다.
		virtual bool Remove(const std::string& _sPath) = 0;

		// tempfile.mkstemp(prefix, suffix, dir) 뒤의 close 까지가 한 연산이다(:531~540).
		// 배타 생성한 0바이트 예약 파일을 남겨 이름 재사용 경쟁을 막는다. 쓰지 않은
		// 예약 파일의 정리는 호출부 책임이다.
		virtual bool CreateUniqueTemporaryPath(
			const std::string& _sDirectory,
			const std::string& _sPrefix,
			const std::string& _sSuffix,
			std::string*       _psPath) = 0;

		// Path.stat().st_mtime 를 epoch 마이크로초로 돌려준다. 원본이 그 값을 datetime 으로
		// 바꿔 쓰므로(:275) 계약상 정밀도는 마이크로초다.
		virtual bool ModifiedTimeUs(const std::string& _sPath, std::int64_t* _pnValueUs) const = 0;

		// Path.glob 의 자리다. 와일드카드 판정은 core 가 하고 여기서는 이름만 나열한다 -
		// Win32 의 와일드카드 의미(8.3 이름 등)가 파이썬 fnmatch 와 다르기 때문이다.
		// 디렉터리가 없으면 빈 목록으로 성공한다(Path.glob 이 그 자리에서 아무것도 내지 않는다).
		virtual bool ListDirectory(const std::string& _sDirectory, std::vector<std::string>* _pNames) const = 0;

		// 마지막 실패 사유. 원본이 OSError 메시지로 남기는 자리다.
		virtual const std::string& LastError() const = 0;
	};
}
