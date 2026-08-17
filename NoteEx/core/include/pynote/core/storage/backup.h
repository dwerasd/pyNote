#pragma once

#include "pynote/core/storage/file_system.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct sqlite3;

// core 계층. Win32/ATL/WTL/COM/DirectX 헤더를 포함하지 않는다(tools/gates 격리 게이트가 강제).
// 백업은 원자적 파일 교체가 필요하지만 그 구현은 platform/ 이 갖고, 여기는 주입된
// I_FILE_SYSTEM 만 안다. 온라인 백업과 검사 논리 자체는 전부 이 계층에 있다.

namespace pynote::core::storage
{
	// 백업/복원 결과. 파이썬 원본 infrastructure/backup.py 가 예외 종류로 구별하던 실패를
	// 이 계층의 반환값 규약으로 옮긴 것이다. 사유는 LastError() 에 남는다.
	enum class E_BACKUP_RESULT
	{
		Ok,
		SourceMissing,     // 백업할 원본 파일이 없다. 원본 BackupError 자리다(:111~112).
		DestinationExists, // 기존 대상을 덮어쓰지 않는다. 원본 FileExistsError 자리다(:114, :151~152).
		Integrity,         // 원본 BackupIntegrityError 자리다.
		Unsupported,       // 앱보다 새로운 schema version. 원본 UnsupportedBackupError 자리다(:103~105).
		TargetInvalid,     // 복원 대상 DB 세트 경로가 일반 파일이 아니다. 원본 BackupError 자리다(:496).
		Cancelled,         // 분할 백업 루프가 취소됐다. 아래 개선이 들여온 유일한 새 경로다.
		RollbackFailed,    // 복원 실패 뒤 원본 세트 롤백까지 실패했다(:192~194). 원래 실패와 구별한다.
		Failed             // 그 밖의 파일/SQLite 실패.
	};

	// 검증을 마친 백업의 메타데이터. 원본 BackupInspection(:50~55) 이식이다.
	struct S_BACKUP_INSPECTION
	{
		std::string sPath;
		int         nSchemaVersion{ 0 };
	};

	// 열린 연결에 quick_check 를 돌린다. 원본 run_quick_check(:58~69) 이식이고 실패 사유를
	// _psError 에 남긴다 - 원본이 BackupIntegrityError 메시지로 올리는 문구 그대로다.
	bool RunQuickCheck(sqlite3* _pConnection, std::string* _psError);

	// 벽시계는 epoch 마이크로초(원본 datetime.now(UTC) 자리), 단조시계는 초(원본 time.monotonic
	// 자리)다. 두 시계는 서로 다른 것을 재므로 하나로 합치지 않는다 - 자동 백업 주기는 파일
	// 수정 시각과 비교해야 해서 벽시계여야 하고, quick_check 요율 제한은 시스템 시각 변경에
	// 흔들리면 안 되므로 단조시계여야 한다.
	using WallClockUsFn  = std::function<std::int64_t()>;
	using MonotonicSecFn = std::function<double()>;

	// 분할 온라인 백업 루프의 설정.
	//
	// W1 지시서가 기록한 이 파동의 유일한 의도적 개선이다. 파이썬 원본은
	// Connection.backup(destination) 한 번 호출이라 응용 계층의 증분 루프가 없고, 이식본은
	// sqlite3_backup_step 을 나눠 부른다. 그래서 원본에 없던 두 가지가 생긴다 - 진행 중
	// 취소와 단계 사이 관측이다.
	struct S_BACKUP_STEP_OPTIONS
	{
		// step 한 번에 옮길 페이지 수. 계약이 아니라 조절 손잡이다.
		int nPagesPerStep{ 64 };

		// SQLITE_BUSY/SQLITE_LOCKED 를 만났을 때 다시 시도하기까지 쉬는 시간. 파이썬
		// Connection.backup 의 기본값 sleep=0.250 과 같은 값이라 재시도 리듬이 원본과 같다.
		double dBusyRetrySeconds{ 0.25 };

		// 각 step 직전에 부른다. false 를 돌려주면 취소하고 아무것도 게시하지 않는다.
		// 인자는 sqlite3_backup_remaining / sqlite3_backup_pagecount 이며, 첫 step 전에는
		// 둘 다 0 이다(SQLite 가 첫 step 에서야 채운다).
		std::function<bool(int _nRemaining, int _nPageCount)> fnShouldContinue{};
	};

	// 백업 생성/검사/복원. 파이썬 원본 backup.py 의 모듈 수준 함수 묶음 이식이다.
	//
	// 보존하는 계약(MODE A - 원본 backup.py 실측):
	//  1. 생성은 기존 대상을 덮어쓰지 않는다(:114). 대상 디렉터리 안의 유일한 임시 파일에 쓰고
	//     검사를 통과한 뒤에야 교체하며, 임시 파일은 성공을 포함한 모든 경로에서 지운다(:137).
	//  2. 복원은 본체와 -wal, -shm 을 한 세트로 다룬다(:482~487). 존재하지만 일반 파일이 아니거나
	//     심볼릭 링크인 대상은 아무것도 옮기기 전에 거절한다(:490~496).
	//  3. 복원은 기존 세트를 임시 이름으로 비켜 두고 새 본체를 제자리에 놓는다. 어느 단계든
	//     실패하면 옮긴 순서의 역순으로 전부 되돌리고, 되돌리지 못한 경로를 보고한다(:499~534).
	//     롤백 자체의 실패는 원래 실패와 다른 오류다(:187~194).
	//  4. 검사는 읽기 전용으로 열어 quick_check, schema version 지원 범위, 필수 테이블, 외래키,
	//     해시 재계산, 현재 리비전 소유권, 카운터 하한, 원문 소거 정합을 본다(:72~106).
	//  5. 앱보다 새로운 schema version 은 거절한다(:97~105).
	class C_BACKUP_SERVICE
	{
	public:
		explicit C_BACKUP_SERVICE(I_FILE_SYSTEM& _fileSystem);

		C_BACKUP_SERVICE(const C_BACKUP_SERVICE&) = delete;
		C_BACKUP_SERVICE& operator=(const C_BACKUP_SERVICE&) = delete;

		void SetStepOptions(const S_BACKUP_STEP_OPTIONS& _Options);

		// 원본 inspect_backup(:72) 이식.
		E_BACKUP_RESULT Inspect(const std::string& _sPath, S_BACKUP_INSPECTION* _pOut);

		// 원본 create_database_backup(:109) 이식.
		E_BACKUP_RESULT Create(
			const std::string&   _sSource,
			const std::string&   _sDestination,
			S_BACKUP_INSPECTION* _pOut);

		// 원본 restore_database(:140) 이식. _bOverwrite 는 원본의 키워드 인자와 같은 뜻이다.
		E_BACKUP_RESULT Restore(
			const std::string&   _sBackupPath,
			const std::string&   _sDestination,
			bool                 _bOverwrite,
			S_BACKUP_INSPECTION* _pOut);

		const std::string& LastError() const noexcept { return(m_sLastError); }

		// 롤백까지 실패했을 때 되돌리지 못한 경로들이다. 원본은 이 목록을 로그로 남기지만
		// (:188~191) 이식본에는 로거가 없어 호출부가 읽을 수 있는 자리에 남긴다.
		const std::vector<std::string>& RollbackFailedPaths() const noexcept { return(m_RollbackFailedPaths); }

	private:
		// 읽기 전용 연결. 원본 _open_read_only(:545~546) 자리다.
		bool open_read_only_(const std::string& _sPath, sqlite3** _ppConnection);

		// 분할 온라인 백업. 원본은 Connection.backup 한 번이다(:122, :163).
		E_BACKUP_RESULT copy_database_(const std::string& _sSource, const std::string& _sDestination);

		// 열린 연결 위의 검사 사슬. 반환이 Ok 일 때만 _pnSchemaVersion 이 뜻을 갖는다.
		E_BACKUP_RESULT inspect_connection_(sqlite3* _pConnection, int* _pnSchemaVersion);

		E_BACKUP_RESULT restore_body_(
			const std::string&              _sDestination,
			const std::vector<std::string>& _ExistingPaths,
			const std::string&              _sTemporaryPath,
			int*                            _pnSchemaVersion);

		// 원본 _restore_preserved_database_set(:499) 이식. 되돌리지 못한 경로를 돌려주고,
		// 확정된 게시만 되돌리며, 옮기지 못한 자리의 예약 파일을 지운다.
		std::vector<std::string> rollback_preserved_(
			const std::string&                                            _sDestination,
			const std::string&                                            _sTemporaryPath,
			const std::vector<std::string>&                               _ExistingPaths,
			const std::vector<std::string>&                               _MovedPaths,
			const std::vector<std::pair<std::string, std::string>>&       _PreservedPaths);

		E_BACKUP_RESULT integrity_(const std::string& _sMessage);
		E_BACKUP_RESULT file_system_failed_();
		void            set_error_(const std::string& _sMessage);

		I_FILE_SYSTEM&           m_FileSystem;
		S_BACKUP_STEP_OPTIONS    m_StepOptions{};
		std::string              m_sLastError;
		std::vector<std::string> m_RollbackFailedPaths;
	};

	// 사전 마이그레이션 백업 훅. 파이썬 원본 MigrationBackupHook(:205) 이식이다.
	// C_MIGRATION_RUNNER 의 BackupHook 계약(migration_runner.h)을 그대로 만족한다 - 러너에
	// 넘길 때는 std::ref 로 감싸야 LastBackupPath() 가 호출 뒤에도 보인다.
	class C_MIGRATION_BACKUP_HOOK
	{
	public:
		// 원본의 판정은 `self._backup_directory or database_path.parent / "backups"`(:225) 이고
		// 그 술어는 **None 인가**이지 빈 경로인가가 아니다. 빈 문자열을 None 으로 접으면 "현재
		// 디렉터리"를 뜻하는 인자가 조용히 다른 곳을 가리키므로 optional 로 그대로 옮긴다.
		C_MIGRATION_BACKUP_HOOK(
			C_BACKUP_SERVICE&                 _service,
			const std::optional<std::string>& _sBackupDirectory,
			WallClockUsFn                     _fnClock);

		// 원본 __call__(:218~232). 실패하면 false 이며 러너가 마이그레이션을 진행하지 않는다.
		bool operator()(const std::string& _sDatabasePath, int _nCurrentVersion, int _nLatestVersion);

		// 원본 last_backup_path(:216). 아직 만든 적이 없으면 빈 문자열이다.
		const std::string& LastBackupPath() const noexcept { return(m_sLastBackupPath); }

	private:
		C_BACKUP_SERVICE&          m_Service;
		std::optional<std::string> m_sBackupDirectory;
		WallClockUsFn              m_fnClock;
		std::string                m_sLastBackupPath;
	};

	// 설정된 시간 간격에 따라 자동 백업 실행 여부를 정한다. 원본 AutomaticBackupManager(:235) 이식이다.
	class C_AUTOMATIC_BACKUP_MANAGER
	{
	public:
		C_AUTOMATIC_BACKUP_MANAGER(
			C_BACKUP_SERVICE&  _service,
			I_FILE_SYSTEM&     _fileSystem,
			const std::string& _sDatabasePath,
			const std::string& _sBackupDirectory,
			double             _dIntervalHours,
			WallClockUsFn      _fnClock);

		// 원본 set_interval_hours(:252~256). 0 이하는 거절한다 - 원본이 ValueError 를 올리는 자리다.
		bool SetIntervalHours(double _dIntervalHours);

		// 생성자가 받은 주기가 유효한가. 원본은 __init__ 에서 예외를 올리지만 이식본의
		// 생성자는 실패를 알릴 수 없으므로 여기로 옮긴다.
		bool IsValid() const noexcept { return(m_bValidInterval); }

		// 원본 run_if_due(:258~270). 주기가 지나지 않았으면 Ok 이면서 _pbCreated 가 false 다
		// (원본이 None 을 돌려주는 자리). 만들었으면 _psDestination 에 경로가 들어간다.
		E_BACKUP_RESULT RunIfDue(bool _bForce, bool* _pbCreated, std::string* _psDestination);

		const std::string& LastError() const noexcept { return(m_sLastError); }

	private:
		// 원본 _latest_backup_time(:272~278). 후보가 없으면 false 다.
		bool latest_backup_time_(std::int64_t* _pnValueUs) const;

		C_BACKUP_SERVICE& m_Service;
		I_FILE_SYSTEM&    m_FileSystem;
		std::string       m_sDatabasePath;
		std::string       m_sBackupDirectory;
		WallClockUsFn     m_fnClock;
		std::int64_t      m_nIntervalUs{ 0 };
		std::int64_t      m_nLastBackupAtUs{ 0 };
		bool              m_bHasLastBackup{ false };
		bool              m_bValidInterval{ false };
		std::string       m_sLastError;
	};

	// 원본 PeriodicQuickCheck(:281) 의 세 갈래다. 원본은 "실행했는가"를 bool 로 돌려주고
	// 무결성 실패는 예외로 올리므로, 반환값 규약에서는 셋을 구별해야 한다.
	enum class E_QUICK_CHECK_RESULT
	{
		Skipped, // 주기가 아직 지나지 않았다(원본 False).
		Passed,  // 실행했고 통과했다(원본 True).
		Failed   // 실행했고 무결성 검사가 실패했다(원본 BackupIntegrityError).
	};

	// 호출 시점에 주기가 도래했으면 quick_check 를 수행한다. 원본 PeriodicQuickCheck(:281) 이식이다.
	class C_PERIODIC_QUICK_CHECK
	{
	public:
		C_PERIODIC_QUICK_CHECK(sqlite3* _pConnection, double _dIntervalHours, MonotonicSecFn _fnClock);

		// 원본 __init__ 은 0 이하 주기에 ValueError 를 올린다(:291~292). 생성자가 실패를 알릴
		// 수 없으므로 여기로 옮기고, 무효한 객체의 RunIfDue 는 Failed 다.
		bool IsValid() const noexcept { return(m_bValidInterval); }

		// 원본 run_if_due(:298~309).
		E_QUICK_CHECK_RESULT RunIfDue(bool _bForce);

		const std::string& LastError() const noexcept { return(m_sLastError); }

	private:
		sqlite3*       m_pConnection{ nullptr };
		MonotonicSecFn m_fnClock;
		double         m_dIntervalSeconds{ 0.0 };
		double         m_dLastCheckAt{ 0.0 };
		bool           m_bHasLastCheck{ false };
		bool           m_bValidInterval{ false };
		std::string    m_sLastError;
	};
}
