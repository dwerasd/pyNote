#include "pynote/core/storage/database.h"

#include <sqlite3/sqlite3.h>

#include <cctype>
#include <cstdlib>

#pragma comment(lib, "sqlite3")

namespace
{
	// PRAGMA 결과 첫 열을 문자열로 읽는다. 행이 없으면 빈 문자열이다.
	bool query_first_text(sqlite3* _pHandle, const std::string& _sSql, std::string* _pOut)
	{
		sqlite3_stmt* pStmt = nullptr;
		if (::sqlite3_prepare_v2(_pHandle, _sSql.c_str(), -1, &pStmt, nullptr) != SQLITE_OK)
		{
			return(false);
		}

		const int nStep = ::sqlite3_step(pStmt);
		if (nStep == SQLITE_ROW)
		{
			const unsigned char* pText = ::sqlite3_column_text(pStmt, 0);
			_pOut->assign(pText ? reinterpret_cast<const char*>(pText) : "");
		}
		else
		{
			_pOut->clear();
		}

		::sqlite3_finalize(pStmt);
		return(nStep == SQLITE_ROW || nStep == SQLITE_DONE);
	}

	// 대소문자를 무시한 비교. PRAGMA 반환값이 구현에 따라 대소문자가 다를 수 있다.
	bool equals_ignore_case(const std::string& _sLeft, const std::string& _sRight)
	{
		if (_sLeft.size() != _sRight.size()) { return(false); }
		for (std::size_t i = 0; i < _sLeft.size(); ++i)
		{
			const char chLeft = static_cast<char>(::tolower(static_cast<unsigned char>(_sLeft[i])));
			const char chRight = static_cast<char>(::tolower(static_cast<unsigned char>(_sRight[i])));
			if (chLeft != chRight) { return(false); }
		}
		return(true);
	}
}

namespace pynote::core::storage
{
	C_DATABASE::~C_DATABASE()
	{
		this->Close();
	}

	void C_DATABASE::set_error_(const std::string& _sMessage)
	{
		m_sLastError = _sMessage;
	}

	bool C_DATABASE::verify_pragma_(const std::string& _sPragma, const std::string& _sExpected)
	{
		std::string sValue;
		if (!query_first_text(m_pHandle, _sPragma, &sValue))
		{
			this->set_error_(_sPragma + " 실행에 실패했습니다: " + ::sqlite3_errmsg(m_pHandle));
			return(false);
		}
		if (!equals_ignore_case(sValue, _sExpected))
		{
			this->set_error_(_sPragma + " 검증에 실패했습니다. 기대값 " + _sExpected + ", 실제값 " + sValue);
			return(false);
		}
		return(true);
	}

	bool C_DATABASE::Open(const std::string& _sPath)
	{
		this->Close();
		m_sLastError.clear();

		const int nResult = ::sqlite3_open_v2(
			_sPath.c_str(), &m_pHandle,
			SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
		if (nResult != SQLITE_OK)
		{
			this->set_error_(std::string("데이터베이스를 열지 못했습니다: ")
				+ (m_pHandle ? ::sqlite3_errmsg(m_pHandle) : ::sqlite3_errstr(nResult)));
			this->Close();
			return(false);
		}

		// 외래키는 연결마다 꺼진 상태로 시작하므로 켜고 되읽어 확인한다.
		// prepare 는 첫 문장만 준비하므로 설정과 확인을 한 문자열에 붙이지 않는다.
		if (!this->Execute("PRAGMA foreign_keys = ON"))
		{
			this->Close();
			return(false);
		}
		if (!this->verify_pragma_("PRAGMA foreign_keys", "1"))
		{
			this->Close();
			return(false);
		}

		// WAL 전환은 반환값이 곧 확인이다. 파일 데이터베이스가 아니면 여기서 걸린다.
		if (!this->verify_pragma_("PRAGMA journal_mode = WAL;", "wal"))
		{
			this->Close();
			return(false);
		}

		return(true);
	}

	void C_DATABASE::Close()
	{
		if (m_pHandle != nullptr)
		{
			::sqlite3_close(m_pHandle);
			m_pHandle = nullptr;
		}
	}

	bool C_DATABASE::Execute(const std::string& _sSql)
	{
		if (m_pHandle == nullptr)
		{
			this->set_error_("연결이 열려 있지 않습니다.");
			return(false);
		}

		char* pErrorMessage = nullptr;
		if (::sqlite3_exec(m_pHandle, _sSql.c_str(), nullptr, nullptr, &pErrorMessage) != SQLITE_OK)
		{
			this->set_error_(pErrorMessage ? pErrorMessage : "알 수 없는 SQL 오류입니다.");
			::sqlite3_free(pErrorMessage);
			return(false);
		}
		return(true);
	}

	int C_DATABASE::SchemaVersion() const
	{
		if (m_pHandle == nullptr)
		{
			m_sLastError = "연결이 열려 있지 않습니다.";
			return(0);
		}

		// 테이블이 없으면 0 이다. 이 분기가 있어야 새 데이터베이스와 손상된 데이터베이스를 구분한다.
		std::string sExists;
		const std::string sExistsSql =
			"SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'schema_version'";
		if (!query_first_text(m_pHandle, sExistsSql, &sExists))
		{
			m_sLastError = std::string("schema_version 조회에 실패했습니다: ") + ::sqlite3_errmsg(m_pHandle);
			return(0);
		}
		if (sExists.empty()) { return(0); }

		std::string sVersion;
		if (!query_first_text(m_pHandle, "SELECT version FROM schema_version WHERE id = 1", &sVersion))
		{
			m_sLastError = std::string("schema_version 값 조회에 실패했습니다: ") + ::sqlite3_errmsg(m_pHandle);
			return(0);
		}
		if (sVersion.empty()) { return(0); }

		return(::atoi(sVersion.c_str()));
	}

	C_TRANSACTION::C_TRANSACTION(C_DATABASE& _database)
		: m_Database(_database)
	{
		// 중첩 금지. 이미 트랜잭션 안이면 시작하지 않는다.
		if (!m_Database.IsOpen()) { return; }
		if (::sqlite3_get_autocommit(m_Database.Handle()) == 0) { return; }

		m_bActive = m_Database.Execute("BEGIN IMMEDIATE");
	}

	C_TRANSACTION::~C_TRANSACTION()
	{
		// 커밋하지 않고 범위를 벗어나면 전부 되돌린다.
		if (m_bActive)
		{
			m_Database.Execute("ROLLBACK");
			m_bActive = false;
		}
	}

	bool C_TRANSACTION::Commit()
	{
		if (!m_bActive) { return(false); }

		const bool bResult = m_Database.Execute("COMMIT");
		if (!bResult && ::sqlite3_get_autocommit(m_Database.Handle()) == 0)
		{
			// 원본 transaction() 은 commit() 이 던지면 rollback() 을 부르고 예외를 올린다
			// (database.py:62~66). 커밋이 실패했는데 트랜잭션이 아직 열려 있으면 되돌려
			// 쓰기 잠금을 놓아야 한다 - 안 그러면 다음 BEGIN IMMEDIATE 가 막혀 연결이 고착된다.
			// SQLite 가 이미 자동 롤백한 경우에는 부르지 않는다. 그래야 COMMIT 실패 사유가
			// LastError 에 남고 "no transaction is active" 로 덮이지 않는다.
			m_Database.Execute("ROLLBACK");
		}

		m_bActive = false;
		return(bResult);
	}
}
