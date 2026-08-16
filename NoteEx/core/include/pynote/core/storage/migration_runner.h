#pragma once

#include "pynote/core/storage/database.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>

// core 계층. Win32/WTL/DirectWrite 헤더를 포함하지 않는다(tools/gates 격리 게이트가 강제).

namespace pynote::core::storage
{
	// 마이그레이션 한 본. 파이썬 원본 migrations/*.py 의 migrate(connection, applied_at_us) 이식이다.
	// 원본은 실패를 예외로 던지지만 이식은 C_DATABASE 와 같은 반환값 오류 규약을 쓴다 -
	// false 를 돌려주고 사유는 연결의 LastError() 에 남긴다.
	using MigrateFn = bool (*)(C_DATABASE& _database, std::int64_t _nAppliedAtUs);

	struct S_MIGRATION
	{
		int       nVersion;
		MigrateFn pfnMigrate;
	};

	// 문장 하나를 실행하며 정수 파라미터 하나를 바인딩한다. 파이썬 원본이 applied_at_us 를
	// 바인드 파라미터로 넘기므로(v0002~v0009 전건, v0001 말미 upsert), SQL 원문을 그대로
	// 유지하려면 C_DATABASE::Execute 의 단문 경로가 아니라 이 경로가 필요하다.
	// 실패 시 false 를 돌려주고 사유는 _database 의 LastError 규약과 같은 자리에 남긴다.
	bool ExecuteBoundInt64(C_DATABASE& _database, const char* _pszSql, std::int64_t _nValue);

	// 사전 마이그레이션 백업 훅. 파이썬 원본 BackupHook = Callable[[Path, int, int], None] 이식이다.
	// 인자는 (데이터베이스 경로, 현재 버전, 목표 버전) 이고 목표 버전은 항상 LATEST 다 -
	// 적용될 pending 의 최대 버전이 아니다(database.py:109 실측).
	// core 는 파일시스템을 알지 않으므로 경로는 호출부가 준 UTF-8 문자열을 그대로 흘려보낸다.
	using BackupHook = std::function<bool(const std::string& _sPath, int _nCurrentVersion, int _nLatestVersion)>;

	// Run 의 결과. 원본에서 예외 종류로 구별되던 실패를 반환값으로 옮긴 것이다.
	// 시험이 다섯 계약을 서로 구별해야 하므로 bool 로 접지 않는다.
	enum class E_MIGRATE_RESULT
	{
		Ok,                 // pending 전체 적용 성공, 또는 pending 이 없어 아무것도 하지 않음
		VersionReadFailed,  // schema_version 조회 자체가 실패했다(테이블 부재와 구별한다)
		UnsupportedVersion, // 현재 버전 > LATEST 라 거부했다
		BackupHookFailed,   // 백업 훅이 실패해 마이그레이션을 진행하지 않았다
		TransactionFailed,  // BEGIN IMMEDIATE 를 잡지 못했다
		MigrationFailed     // 중도 실패라 전부 롤백했다
	};

	// 스키마 마이그레이션 러너. 파이썬 원본 infrastructure/database.py 의 Database._migrate 이식이다.
	// 연결 수명주기(C_DATABASE)에서 떼어 두어 러너만 단독으로 시험할 수 있다.
	//
	// 보존하는 계약(MODE A - 원본 database.py 실측):
	//  1. schema_version 테이블이 없거나 id = 1 행이 없으면 현재 버전은 0 이다(:84~95).
	//  2. 현재 버전이 LATEST 보다 크면 거부한다(:99).
	//  3. pending 은 version > current 인 (version, migrate) 를 등록 순서대로 적용한다
	//     (:101~103 - 순서는 migrations/__init__.py:33~42 의 등록 순서에 의존한다).
	//  4. 백업 훅은 기존 비어 있지 않은 데이터베이스(_had_database - :24) 이고 훅이 주입된
	//     경우에만 마이그레이션 전에 호출한다(:107~112). 훅 실패는 전파되어 마이그레이션이
	//     진행되지 않으며, 신규 데이터베이스 생성 경로에서는 훅을 호출하지 않는다.
	//  5. pending 전체는 하나의 BEGIN IMMEDIATE 트랜잭션에서 적용하고 중도 실패하면 전부
	//     롤백한다(트랜잭션 계약 :54~66, 적용 루프 :114~117).
	//  6. migrate 시그니처는 (연결, epoch 마이크로초) 이고 시간값은 time_ns() // 1_000 이다
	//     (:117 은 호출 실측). 원본이 pending 마다 시계를 다시 읽으므로 이식도 본마다 다시 읽는다.
	class C_MIGRATION_RUNNER
	{
	public:
		C_MIGRATION_RUNNER() = default;

		C_MIGRATION_RUNNER(const C_MIGRATION_RUNNER&) = delete;
		C_MIGRATION_RUNNER& operator=(const C_MIGRATION_RUNNER&) = delete;

		// 원본이 연결을 열기 전에 계산하는 _had_database(:24 - 파일이 있고 크기가 0 이 아님) 를
		// 호출부가 주입한다. core 는 파일시스템을 알지 않으므로 그 판정은 호출부 몫이다.
		// _sPath 는 훅에 그대로 전달되는 값일 뿐이라 러너는 해석하지 않는다.
		void SetExistingDatabase(bool _bHadDatabase, const std::string& _sPath);

		// 백업 훅 주입. 주입하지 않으면 훅 호출 경로 자체가 없다(원본의 backup_hook=None).
		void SetBackupHook(BackupHook _hook);

		// 등록된 마이그레이션 목록(migrations::Registry) 으로 실행한다.
		E_MIGRATE_RESULT Run(C_DATABASE& _database);

		// 시험용 주입 경로. _migrations 는 버전 오름차순 등록 순서여야 하고
		// _nLatestVersion 은 그 목록의 마지막 버전이다.
		E_MIGRATE_RESULT Run(C_DATABASE& _database, std::span<const S_MIGRATION> _migrations, int _nLatestVersion);

		const std::string& LastError() const noexcept { return(m_sLastError); }

	private:
		// C_DATABASE::SchemaVersion() 은 행 부재와 조회 실패를 모두 0 으로 접는다(database.h:36).
		// 원본 database.py 는 조회 실패를 예외로 전파하므로 러너는 둘을 구별해야 한다 - 직접 읽는다.
		bool read_schema_version_(C_DATABASE& _database, int* _pVersion);
		void set_error_(const std::string& _sMessage);

		BackupHook  m_BackupHook{};
		bool        m_bHadDatabase{ false };
		std::string m_sPath;
		std::string m_sLastError;
	};
}
