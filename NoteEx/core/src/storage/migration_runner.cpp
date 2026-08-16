#include "pynote/core/storage/migration_runner.h"

#include "pynote/core/storage/migrations/registry.h"

#include <sqlite3/sqlite3.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#pragma comment(lib, "sqlite3")

namespace
{
	// 첫 행 첫 열을 정수로 읽는다. 행이 없으면 _pFound 가 false 이고 조회 자체가 실패하면 false 다.
	// database.cpp 의 조회 도우미는 행 부재와 조회 실패를 모두 빈 값으로 접어 러너의 계약을
	// 만족하지 못한다 - 원본은 조회 실패를 예외로 올리므로 둘을 갈라야 한다.
	bool query_first_int(sqlite3* _pHandle, const char* _pszSql, int* _pValue, bool* _pFound)
	{
		sqlite3_stmt* pStmt = nullptr;
		if (::sqlite3_prepare_v2(_pHandle, _pszSql, -1, &pStmt, nullptr) != SQLITE_OK)
		{
			return(false);
		}

		const int nStep = ::sqlite3_step(pStmt);
		if (nStep == SQLITE_ROW)
		{
			*_pValue = ::sqlite3_column_int(pStmt, 0);
			*_pFound = true;
		}
		else
		{
			*_pFound = false;
		}

		::sqlite3_finalize(pStmt);
		return(nStep == SQLITE_ROW || nStep == SQLITE_DONE);
	}

	// 원본 time.time_ns() // 1_000 이식이다. system_clock 의 epoch 는 C++20 부터 Unix epoch 다.
	std::int64_t now_epoch_us()
	{
		const auto Since = std::chrono::system_clock::now().time_since_epoch();
		return(std::chrono::duration_cast<std::chrono::microseconds>(Since).count());
	}
}

namespace pynote::core::storage
{
	void C_MIGRATION_RUNNER::set_error_(const std::string& _sMessage)
	{
		m_sLastError = _sMessage;
	}

	void C_MIGRATION_RUNNER::SetExistingDatabase(bool _bHadDatabase, const std::string& _sPath)
	{
		m_bHadDatabase = _bHadDatabase;
		m_sPath = _sPath;
	}

	void C_MIGRATION_RUNNER::SetBackupHook(BackupHook _hook)
	{
		m_BackupHook = std::move(_hook);
	}

	bool C_MIGRATION_RUNNER::read_schema_version_(C_DATABASE& _database, int* _pVersion)
	{
		if (!_database.IsOpen())
		{
			this->set_error_("연결이 열려 있지 않습니다.");
			return(false);
		}

		sqlite3* pHandle = _database.Handle();

		// 테이블이 없으면 버전은 0 이다(database.py:84~93).
		int  nExists = 0;
		bool bFound  = false;
		const char* pszExistsSql =
			"SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'schema_version'";
		if (!query_first_int(pHandle, pszExistsSql, &nExists, &bFound))
		{
			this->set_error_(std::string("schema_version 조회에 실패했습니다: ") + ::sqlite3_errmsg(pHandle));
			return(false);
		}
		if (!bFound)
		{
			*_pVersion = 0;
			return(true);
		}

		// 테이블은 있는데 id = 1 행이 없어도 0 이다(:94~95).
		int nVersion = 0;
		if (!query_first_int(pHandle, "SELECT version FROM schema_version WHERE id = 1", &nVersion, &bFound))
		{
			this->set_error_(std::string("schema_version 값 조회에 실패했습니다: ") + ::sqlite3_errmsg(pHandle));
			return(false);
		}

		*_pVersion = bFound ? nVersion : 0;
		return(true);
	}

	E_MIGRATE_RESULT C_MIGRATION_RUNNER::Run(C_DATABASE& _database)
	{
		return(this->Run(_database, migrations::Registry(), migrations::LatestSchemaVersion()));
	}

	E_MIGRATE_RESULT C_MIGRATION_RUNNER::Run(
		C_DATABASE& _database, std::span<const S_MIGRATION> _migrations, int _nLatestVersion)
	{
		m_sLastError.clear();

		int nCurrentVersion = 0;
		if (!this->read_schema_version_(_database, &nCurrentVersion))
		{
			return(E_MIGRATE_RESULT::VersionReadFailed);
		}

		if (nCurrentVersion > _nLatestVersion)
		{
			this->set_error_("지원하지 않는 schema version입니다: " + std::to_string(nCurrentVersion));
			return(E_MIGRATE_RESULT::UnsupportedVersion);
		}

		// pending 은 version > current 인 항목이다(:101~103). 적용 순서는 목록 순서 그대로이므로
		// 별도로 모아 두지 않고 아래 루프에서 같은 술어로 건너뛴다.
		bool bHasPending = false;
		for (const S_MIGRATION& Migration : _migrations)
		{
			if (Migration.nVersion > nCurrentVersion)
			{
				bHasPending = true;
				break;
			}
		}
		if (!bHasPending) { return(E_MIGRATE_RESULT::Ok); }

		// 백업 훅은 pending 이 있을 때만, 그리고 트랜잭션을 열기 전에 부른다(:104~112).
		// 목표 버전 인자는 적용될 pending 의 최대 버전이 아니라 항상 LATEST 다(:109).
		if (m_bHadDatabase && m_BackupHook)
		{
			if (!m_BackupHook(m_sPath, nCurrentVersion, _nLatestVersion))
			{
				this->set_error_("schema migration 전 백업 훅이 실패했습니다.");
				return(E_MIGRATE_RESULT::BackupHookFailed);
			}
		}

		// pending 전체가 한 트랜잭션이다. 중도 실패는 C_TRANSACTION 소멸자가 전부 되돌린다(:114~117).
		C_TRANSACTION Transaction(_database);
		if (!Transaction.IsActive())
		{
			this->set_error_("쓰기 트랜잭션을 시작하지 못했습니다. 이미 트랜잭션 중이거나 연결이 닫혀 있습니다.");
			return(E_MIGRATE_RESULT::TransactionFailed);
		}

		for (const S_MIGRATION& Migration : _migrations)
		{
			if (Migration.nVersion <= nCurrentVersion) { continue; }

			// 원본이 pending 마다 시계를 다시 읽으므로 이식도 본마다 다시 읽는다(:117).
			if (!Migration.pfnMigrate(_database, now_epoch_us()))
			{
				this->set_error_("schema migration에 실패했습니다: "
					+ std::to_string(nCurrentVersion) + " -> " + std::to_string(_nLatestVersion)
					+ " (" + _database.LastError() + ")");
				return(E_MIGRATE_RESULT::MigrationFailed);
			}
		}

		if (!Transaction.Commit())
		{
			this->set_error_("schema migration 커밋에 실패했습니다: " + _database.LastError());
			return(E_MIGRATE_RESULT::MigrationFailed);
		}

		return(E_MIGRATE_RESULT::Ok);
	}
}
