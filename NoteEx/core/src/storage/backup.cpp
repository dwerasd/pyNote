#include "pynote/core/storage/backup.h"

#include "pynote/core/storage/migrations/registry.h"
#include "pynote/core/storage/repositories.h"

#include <sqlite3/sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "sqlite3")

namespace
{
	namespace storage = pynote::core::storage;

	// SQL 원문은 u8 리터럴로 두고 여기 한 곳에서만 형을 바꾼다. 이 기계에서 좁은 리터럴은
	// CP949 로 컴파일되므로(spec_TR2 §1(a)) SQLite 에 넘길 바이트는 u8 이어야 한다.
	const char* as_sql(const char8_t* _pszSql)
	{
		return(reinterpret_cast<const char*>(_pszSql));
	}

	// SQL 실행 자체가 실패했을 때의 문구. 원본 inspect_backup 이 sqlite3.DatabaseError 를
	// 잡아 올리는 메시지다(:91~93). 논리 검사 실패와 구별되는 자리다.
	const char* const INSPECT_QUERY_FAILED = "백업 무결성 정보를 검사할 수 없습니다.";

	// 준비된 문장 하나. 소멸 시 finalize 한다.
	class C_STATEMENT
	{
	public:
		C_STATEMENT(sqlite3* _pHandle, const char8_t* _pszSql)
		{
			if (_pHandle == nullptr) { return; }
			if (::sqlite3_prepare_v2(_pHandle, as_sql(_pszSql), -1, &m_pStmt, nullptr) != SQLITE_OK)
			{
				m_pStmt = nullptr;
			}
		}

		~C_STATEMENT()
		{
			if (m_pStmt != nullptr) { ::sqlite3_finalize(m_pStmt); }
		}

		C_STATEMENT(const C_STATEMENT&) = delete;
		C_STATEMENT& operator=(const C_STATEMENT&) = delete;

		bool          IsPrepared() const noexcept { return(m_pStmt != nullptr); }
		sqlite3_stmt* Handle() const noexcept { return(m_pStmt); }
		int           Step() { return(::sqlite3_step(m_pStmt)); }

	private:
		sqlite3_stmt* m_pStmt{ nullptr };
	};

	std::string column_text(sqlite3_stmt* _pStmt, int _nIndex)
	{
		const unsigned char* pText = ::sqlite3_column_text(_pStmt, _nIndex);
		const int            nSize = ::sqlite3_column_bytes(_pStmt, _nIndex);
		if (pText == nullptr) { return(std::string{}); }
		return(std::string(reinterpret_cast<const char*>(pText), static_cast<std::size_t>(nSize)));
	}

	// 파이썬 원본의 `type(value) is not str` 은 sqlite3 가 TEXT 열에만 str 을 주는 것에
	// 기대고 있다(:462~465). 정수/실수/BLOB/NULL 은 전부 실패다.
	bool is_text(sqlite3_stmt* _pStmt, int _nIndex)
	{
		return(::sqlite3_column_type(_pStmt, _nIndex) == SQLITE_TEXT);
	}

	// 같은 이유로 `type(value) is not int` 는 INTEGER 열만 참이다(:421, :427).
	bool is_integer(sqlite3_stmt* _pStmt, int _nIndex)
	{
		return(::sqlite3_column_type(_pStmt, _nIndex) == SQLITE_INTEGER);
	}

	// ---------------------------------------------------------------------------------------
	// 경로 조작. 원본이 pathlib 으로 하는 일 중 이 계층이 쓰는 것만 옮긴다.
	// 구분자는 역슬래시와 슬래시 둘 다 받는다. 다만 pathlib 과 달리 입력에 있던 구분자를
	// 그대로 두고 이어붙일 때만 역슬래시를 쓴다 - 파일 연산은 두 표기를 모두 받는다.
	// ---------------------------------------------------------------------------------------
	bool is_separator(char _ch)
	{
		return(_ch == '\\' || _ch == '/');
	}

	// Path(...).name
	std::string file_name(const std::string& _sPath)
	{
		const std::size_t nPos = _sPath.find_last_of("\\/");
		if (nPos == std::string::npos) { return(_sPath); }
		return(_sPath.substr(nPos + 1));
	}

	// Path(...).parent - 구분자가 없으면 "." 다(pathlib 규약).
	std::string parent_directory(const std::string& _sPath)
	{
		const std::size_t nPos = _sPath.find_last_of("\\/");
		if (nPos == std::string::npos) { return("."); }
		if (nPos == 0) { return(_sPath.substr(0, 1)); }
		return(_sPath.substr(0, nPos));
	}

	// Path(...).stem - 마지막 확장자만 떼고, 이름 전체가 확장자인 형태(".bashrc")는 그대로 둔다.
	std::string file_stem(const std::string& _sPath)
	{
		const std::string sName = file_name(_sPath);
		const std::size_t nDot  = sName.find_last_of('.');
		if (nDot == std::string::npos || nDot == 0) { return(sName); }
		return(sName.substr(0, nDot));
	}

	// Path(dir) / name. 원본에서 dir 이 "." 이면 결과는 name 뿐이다(pathlib 규약).
	std::string join_path(const std::string& _sDirectory, const std::string& _sName)
	{
		if (_sDirectory.empty() || _sDirectory == ".") { return(_sName); }
		if (is_separator(_sDirectory.back())) { return(_sDirectory + _sName); }
		return(_sDirectory + "\\" + _sName);
	}

	// 원본 _temporary_database_path(:531~540) 가 만드는 이름의 접두다.
	std::string temporary_prefix(const std::string& _sPath)
	{
		return("." + file_name(_sPath) + ".");
	}

	// 원본 datetime.strftime("%Y%m%dT%H%M%S%fZ")(:223, :261). UTC 이고 %f 는 여섯 자리다.
	// epoch 이전 시각은 이 앱의 시계에서 나오지 않으므로 다루지 않는다.
	std::string format_backup_timestamp(std::int64_t _nEpochUs)
	{
		const std::chrono::microseconds Total(_nEpochUs);
		const auto                      Days      = std::chrono::floor<std::chrono::days>(Total);
		const std::chrono::year_month_day Date(std::chrono::sys_days{ Days });
		const auto                      TimeOfDay = Total - Days;
		const auto                      Hours     = std::chrono::duration_cast<std::chrono::hours>(TimeOfDay);
		const auto                      Minutes   = std::chrono::duration_cast<std::chrono::minutes>(TimeOfDay - Hours);
		const auto                      Seconds   = std::chrono::duration_cast<std::chrono::seconds>(TimeOfDay - Hours - Minutes);
		const auto                      Micros    = TimeOfDay - Hours - Minutes - Seconds;

		char szBuffer[40] = {};
		::snprintf(szBuffer, sizeof(szBuffer), "%04d%02u%02uT%02d%02d%02d%06dZ",
			static_cast<int>(Date.year()),
			static_cast<unsigned>(Date.month()),
			static_cast<unsigned>(Date.day()),
			static_cast<int>(Hours.count()),
			static_cast<int>(Minutes.count()),
			static_cast<int>(Seconds.count()),
			static_cast<int>(Micros.count()));
		return(std::string(szBuffer));
	}

	// 원본 _V1/_V2/_V4_REQUIRED_TABLES(:19~35). v4 부터 workspace_state 가 workspace_windows 로 바뀐다.
	std::vector<std::string> required_tables(int _nSchemaVersion)
	{
		std::vector<std::string> Tables = {
			"schema_version", "documents", "capture_operations", "cards", "card_revisions",
			"drafts", "edit_events", "card_lineage", "counters", "workspace_state",
			"document_ui_states",
		};
		if (_nSchemaVersion >= 2) { Tables.push_back("data_policy_settings"); }
		if (_nSchemaVersion >= 4)
		{
			Tables.erase(
				std::remove(Tables.begin(), Tables.end(), std::string("workspace_state")),
				Tables.end());
			Tables.push_back("workspace_windows");
		}
		return(Tables);
	}

	// ---------------------------------------------------------------------------------------
	// 검사 사슬. 실패는 false 이고 사유는 _psError 에 남는다.
	// SQL 실행 자체가 실패한 자리에는 INSPECT_QUERY_FAILED 를 넣는다 - 원본이 논리 실패와
	// 조회 실패를 서로 다른 메시지로 올리기 때문이다.
	// ---------------------------------------------------------------------------------------
	bool validate_text_hash(
		const std::string& _sText,
		const std::string& _sStoredHash,
		const std::string& _sLabel,
		std::string*       _psError)
	{
		// 원본 _validate_text_hash(:468~471) - UTF-8 바이트의 SHA-256 소문자 16진수다.
		if (storage::TextHash(_sText) != _sStoredHash)
		{
			*_psError = _sLabel + "의 SHA-256 해시가 일치하지 않습니다.";
			return(false);
		}
		return(true);
	}

	bool validated_text(
		sqlite3_stmt*      _pStmt,
		int                _nIndex,
		const std::string& _sLabel,
		std::string*       _psValue,
		std::string*       _psError)
	{
		// 원본 _validated_text(:462~465).
		if (!is_text(_pStmt, _nIndex))
		{
			*_psError = _sLabel + "가 문자열이 아닙니다.";
			return(false);
		}
		*_psValue = column_text(_pStmt, _nIndex);
		return(true);
	}

	bool read_schema_version(sqlite3* _pConnection, int* _pnVersion, std::string* _psError)
	{
		// 원본 _read_schema_version(:309~327).
		{
			C_STATEMENT Table(_pConnection, u8R"SQL(
        SELECT 1
        FROM sqlite_master
        WHERE type = 'table' AND name = 'schema_version'
        )SQL");
			if (!Table.IsPrepared()) { *_psError = INSPECT_QUERY_FAILED; return(false); }

			const int nStep = Table.Step();
			if (nStep == SQLITE_DONE)
			{
				*_psError = "pyNote schema_version이 없는 백업입니다.";
				return(false);
			}
			if (nStep != SQLITE_ROW) { *_psError = INSPECT_QUERY_FAILED; return(false); }
		}

		C_STATEMENT Row(_pConnection, u8"SELECT version FROM schema_version WHERE id = 1");
		if (!Row.IsPrepared()) { *_psError = INSPECT_QUERY_FAILED; return(false); }

		const int nStep = Row.Step();
		if (nStep == SQLITE_DONE)
		{
			*_psError = "백업의 schema version 행이 없습니다.";
			return(false);
		}
		if (nStep != SQLITE_ROW) { *_psError = INSPECT_QUERY_FAILED; return(false); }
		if (!is_integer(Row.Handle(), 0))
		{
			*_psError = "백업의 schema version이 정수가 아닙니다.";
			return(false);
		}

		*_pnVersion = ::sqlite3_column_int(Row.Handle(), 0);
		return(true);
	}

	bool validate_schema_tables(sqlite3* _pConnection, int _nSchemaVersion, std::string* _psError)
	{
		// 원본 _validate_schema_tables(:330~353).
		if (_nSchemaVersion == 0) { return(true); }

		C_STATEMENT Names(_pConnection, u8R"SQL(
        SELECT name
        FROM sqlite_master
        WHERE type = 'table'
        )SQL");
		if (!Names.IsPrepared()) { *_psError = INSPECT_QUERY_FAILED; return(false); }

		std::vector<std::string> Present;
		int                      nStep = 0;
		while ((nStep = Names.Step()) == SQLITE_ROW)
		{
			Present.push_back(column_text(Names.Handle(), 0));
		}
		if (nStep != SQLITE_DONE) { *_psError = INSPECT_QUERY_FAILED; return(false); }

		std::vector<std::string> Missing;
		for (const std::string& sRequired : required_tables(_nSchemaVersion))
		{
			if (std::find(Present.begin(), Present.end(), sRequired) == Present.end())
			{
				Missing.push_back(sRequired);
			}
		}
		if (Missing.empty()) { return(true); }

		// 원본은 sorted(required - table_names) 라 이름 오름차순이다(:350).
		std::sort(Missing.begin(), Missing.end());
		std::string sJoined;
		for (std::size_t i = 0; i < Missing.size(); ++i)
		{
			if (i != 0) { sJoined += ", "; }
			sJoined += Missing[i];
		}
		*_psError = "백업에 필수 테이블이 없습니다: " + sJoined;
		return(false);
	}

	bool validate_foreign_keys(sqlite3* _pConnection, std::string* _psError)
	{
		// 원본 _validate_foreign_keys(:356~364).
		C_STATEMENT Check(_pConnection, u8"PRAGMA foreign_key_check");
		if (!Check.IsPrepared()) { *_psError = INSPECT_QUERY_FAILED; return(false); }

		std::string sSample;
		int         nRows = 0;
		int         nStep = 0;
		while ((nStep = Check.Step()) == SQLITE_ROW)
		{
			if (nRows < 5)
			{
				if (nRows != 0) { sSample += "; "; }
				// rowid 는 WITHOUT ROWID 테이블에서 NULL 이고 원본 f-string 은 그것을 "None"
				// 으로 찍는다. 표본 문구까지 계약이라 같은 자리에 같은 글자를 넣는다.
				const std::string sRowId = (::sqlite3_column_type(Check.Handle(), 1) == SQLITE_NULL)
					? std::string("None")
					: column_text(Check.Handle(), 1);
				sSample += column_text(Check.Handle(), 0) + " rowid=" + sRowId
					+ " -> " + column_text(Check.Handle(), 2);
			}
			++nRows;
		}
		if (nStep != SQLITE_DONE) { *_psError = INSPECT_QUERY_FAILED; return(false); }
		if (nRows == 0) { return(true); }

		*_psError = "백업의 FK 무결성 검사에 실패했습니다: " + sSample;
		return(false);
	}

	bool validate_card_revision_integrity(sqlite3* _pConnection, std::string* _psError)
	{
		// 원본 _validate_card_revision_integrity(:373~414).
		{
			C_STATEMENT Cards(_pConnection, u8R"SQL(
        SELECT
            cards.id,
            cards.body,
            cards.body_hash,
            cards.current_revision_id,
            card_revisions.card_id,
            card_revisions.body,
            card_revisions.body_hash
        FROM cards
        LEFT JOIN card_revisions
          ON card_revisions.id = cards.current_revision_id
        )SQL");
			if (!Cards.IsPrepared()) { *_psError = INSPECT_QUERY_FAILED; return(false); }

			int nStep = 0;
			while ((nStep = Cards.Step()) == SQLITE_ROW)
			{
				sqlite3_stmt* pStmt = Cards.Handle();

				// 열 타입은 텍스트 변환 전에 읽는다 - sqlite3_column_text 가 타입을 바꿔 놓는다.
				const int nCardIdType   = ::sqlite3_column_type(pStmt, 0);
				const int nRevisionType = ::sqlite3_column_type(pStmt, 3);
				const int nOwnerType    = ::sqlite3_column_type(pStmt, 4);

				const std::string sCardId = column_text(pStmt, 0);
				const std::string sOwner  = column_text(pStmt, 4);

				// 원본은 row[3] is None 또는 row[4] != row[0] 이면 소유권 오류다(:391).
				// 파이썬은 타입이 다르면 값이 같아 보여도 같지 않으므로 타입까지 본다.
				const bool bOwned = (nRevisionType != SQLITE_NULL)
					&& (nOwnerType == nCardIdType)
					&& (sOwner == sCardId);
				if (!bOwned)
				{
					*_psError = "카드 " + sCardId + "의 현재 리비전 소유권이 올바르지 않습니다.";
					return(false);
				}

				std::string sCardBody;
				std::string sCardHash;
				std::string sRevisionBody;
				std::string sRevisionHash;
				if (!validated_text(pStmt, 1, "카드 " + sCardId + " 본문", &sCardBody, _psError)) { return(false); }
				if (!validated_text(pStmt, 2, "카드 " + sCardId + " 본문 해시", &sCardHash, _psError)) { return(false); }
				if (!validated_text(pStmt, 5, "카드 " + sCardId + " 현재 리비전 본문", &sRevisionBody, _psError)) { return(false); }
				if (!validated_text(pStmt, 6, "카드 " + sCardId + " 현재 리비전 본문 해시", &sRevisionHash, _psError)) { return(false); }

				if (sCardBody != sRevisionBody || sCardHash != sRevisionHash)
				{
					*_psError = "카드 " + sCardId + "의 본문과 현재 리비전이 일치하지 않습니다.";
					return(false);
				}
				if (!validate_text_hash(sCardBody, sCardHash, "카드 " + sCardId, _psError)) { return(false); }
			}
			if (nStep != SQLITE_DONE) { *_psError = INSPECT_QUERY_FAILED; return(false); }
		}

		C_STATEMENT Revisions(_pConnection, u8"SELECT id, body, body_hash FROM card_revisions");
		if (!Revisions.IsPrepared()) { *_psError = INSPECT_QUERY_FAILED; return(false); }

		int nStep = 0;
		while ((nStep = Revisions.Step()) == SQLITE_ROW)
		{
			sqlite3_stmt*     pStmt       = Revisions.Handle();
			const std::string sRevisionId = column_text(pStmt, 0);

			std::string sBody;
			std::string sBodyHash;
			if (!validated_text(pStmt, 1, "리비전 " + sRevisionId + " 본문", &sBody, _psError)) { return(false); }
			if (!validated_text(pStmt, 2, "리비전 " + sRevisionId + " 본문 해시", &sBodyHash, _psError)) { return(false); }
			if (!validate_text_hash(sBody, sBodyHash, "리비전 " + sRevisionId, _psError)) { return(false); }
		}
		if (nStep != SQLITE_DONE) { *_psError = INSPECT_QUERY_FAILED; return(false); }
		return(true);
	}

	bool validate_capture_counter(sqlite3* _pConnection, std::string* _psError)
	{
		// 원본 _validate_capture_counter(:417~430).
		std::int64_t nNextValue = 0;
		{
			C_STATEMENT Counter(_pConnection, u8"SELECT next_value FROM counters WHERE name = 'capture'");
			if (!Counter.IsPrepared()) { *_psError = INSPECT_QUERY_FAILED; return(false); }

			const int nStep = Counter.Step();
			if (nStep != SQLITE_ROW && nStep != SQLITE_DONE) { *_psError = INSPECT_QUERY_FAILED; return(false); }
			if (nStep == SQLITE_DONE || !is_integer(Counter.Handle(), 0))
			{
				*_psError = "capture 카운터가 없거나 정수가 아닙니다.";
				return(false);
			}
			nNextValue = ::sqlite3_column_int64(Counter.Handle(), 0);
		}

		C_STATEMENT Maximum(_pConnection, u8"SELECT COALESCE(MAX(capture_seq), 0) FROM cards");
		if (!Maximum.IsPrepared()) { *_psError = INSPECT_QUERY_FAILED; return(false); }

		const int nStep = Maximum.Step();
		if (nStep != SQLITE_ROW) { *_psError = INSPECT_QUERY_FAILED; return(false); }

		// 집계는 항상 한 행이라 원본의 maximum_row is None 갈래는 성립하지 않는다(:426).
		// 값의 타입 검사는 그대로 남는다 - capture_seq 에 정수가 아닌 값이 들어 있으면 걸린다.
		if (!is_integer(Maximum.Handle(), 0)
			|| nNextValue <= ::sqlite3_column_int64(Maximum.Handle(), 0))
		{
			*_psError = "capture 카운터가 이미 발급된 capture_seq보다 크지 않습니다.";
			return(false);
		}
		return(true);
	}

	bool validate_capture_operations(sqlite3* _pConnection, std::string* _psError)
	{
		// 원본 _validate_capture_operations(:433~459).
		C_STATEMENT Operations(_pConnection, u8R"SQL(
        SELECT id, original_text, original_hash, original_redacted_at_us
        FROM capture_operations
        )SQL");
		if (!Operations.IsPrepared()) { *_psError = INSPECT_QUERY_FAILED; return(false); }

		int nStep = 0;
		while ((nStep = Operations.Step()) == SQLITE_ROW)
		{
			sqlite3_stmt*     pStmt        = Operations.Handle();
			const std::string sOperationId = column_text(pStmt, 0);
			const bool        bHasText     = (::sqlite3_column_type(pStmt, 1) != SQLITE_NULL);
			const bool        bHasHash     = (::sqlite3_column_type(pStmt, 2) != SQLITE_NULL);

			if (bHasText != bHasHash)
			{
				*_psError = "입력 작업 " + sOperationId + "의 원문과 해시 쌍이 일치하지 않습니다.";
				return(false);
			}

			if (::sqlite3_column_type(pStmt, 3) != SQLITE_NULL)
			{
				if (!is_integer(pStmt, 3) || bHasText)
				{
					*_psError = "입력 작업 " + sOperationId + "의 redact 마커가 올바르지 않습니다.";
					return(false);
				}
				continue;
			}

			if (bHasText)
			{
				std::string sText;
				std::string sHash;
				if (!validated_text(pStmt, 1, "입력 작업 " + sOperationId + " 원문", &sText, _psError)) { return(false); }
				if (!validated_text(pStmt, 2, "입력 작업 " + sOperationId + " 원문 해시", &sHash, _psError)) { return(false); }
				if (!validate_text_hash(sText, sHash, "입력 작업 " + sOperationId, _psError)) { return(false); }
			}
		}
		if (nStep != SQLITE_DONE) { *_psError = INSPECT_QUERY_FAILED; return(false); }
		return(true);
	}

	bool validate_logical_integrity(sqlite3* _pConnection, std::string* _psError)
	{
		// 원본 _validate_logical_integrity(:367~370) - 순서까지 계약이다.
		if (!validate_card_revision_integrity(_pConnection, _psError)) { return(false); }
		if (!validate_capture_counter(_pConnection, _psError)) { return(false); }
		return(validate_capture_operations(_pConnection, _psError));
	}

	bool contains_path(const std::vector<std::string>& _Paths, const std::string& _sPath)
	{
		return(std::find(_Paths.begin(), _Paths.end(), _sPath) != _Paths.end());
	}

	// ASCII 대소문자를 무시한 접두/접미 비교. 원본의 Path.glob 은 Windows 에서 대소문자를
	// 가리지 않으므로(pathlib 이 그 플랫폼 규칙을 따른다) 자동 백업 후보 판정도 같아야 한다.
	char lower_ascii(char _ch)
	{
		return((_ch >= 'A' && _ch <= 'Z') ? static_cast<char>(_ch - 'A' + 'a') : _ch);
	}

	bool equals_ignore_case(const char* _pLeft, const char* _pRight, std::size_t _nSize)
	{
		for (std::size_t i = 0; i < _nSize; ++i)
		{
			if (lower_ascii(_pLeft[i]) != lower_ascii(_pRight[i])) { return(false); }
		}
		return(true);
	}
}

namespace pynote::core::storage
{
	bool RunQuickCheck(sqlite3* _pConnection, std::string* _psError)
	{
		// 원본 run_quick_check(:58~69).
		C_STATEMENT Check(_pConnection, u8"PRAGMA quick_check");
		if (!Check.IsPrepared())
		{
			*_psError = "SQLite quick_check를 실행할 수 없습니다.";
			return(false);
		}

		std::vector<std::string> Messages;
		int                      nStep = 0;
		while ((nStep = Check.Step()) == SQLITE_ROW)
		{
			Messages.push_back(column_text(Check.Handle(), 0));
		}
		if (nStep != SQLITE_DONE)
		{
			*_psError = "SQLite quick_check를 실행할 수 없습니다.";
			return(false);
		}

		// 계약은 "ok" 한 행이다. 행이 여럿이면 전부 이어 붙이고, 하나도 없으면 그 사실을 적는다.
		if (Messages.size() == 1 && Messages[0] == "ok") { return(true); }

		std::string sMessage;
		if (Messages.empty())
		{
			sMessage = "검사 결과 없음";
		}
		else
		{
			for (std::size_t i = 0; i < Messages.size(); ++i)
			{
				if (i != 0) { sMessage += "; "; }
				sMessage += Messages[i];
			}
		}
		*_psError = "SQLite 무결성 검사에 실패했습니다: " + sMessage;
		return(false);
	}

	C_BACKUP_SERVICE::C_BACKUP_SERVICE(I_FILE_SYSTEM& _fileSystem)
		: m_FileSystem(_fileSystem)
	{
	}

	void C_BACKUP_SERVICE::SetStepOptions(const S_BACKUP_STEP_OPTIONS& _Options)
	{
		m_StepOptions = _Options;
	}

	void C_BACKUP_SERVICE::set_error_(const std::string& _sMessage)
	{
		m_sLastError = _sMessage;
	}

	E_BACKUP_RESULT C_BACKUP_SERVICE::integrity_(const std::string& _sMessage)
	{
		this->set_error_(_sMessage);
		return(E_BACKUP_RESULT::Integrity);
	}

	E_BACKUP_RESULT C_BACKUP_SERVICE::file_system_failed_()
	{
		this->set_error_(m_FileSystem.LastError());
		return(E_BACKUP_RESULT::Failed);
	}

	bool C_BACKUP_SERVICE::open_read_only_(const std::string& _sPath, sqlite3** _ppConnection)
	{
		// 원본 _open_read_only(:527~528) 는 file: URI 에 mode=ro 를 붙인다. 여기서는 같은 뜻의
		// SQLITE_OPEN_READONLY 플래그를 쓴다 - URI 파싱을 거치지 않아 경로 안의 ? 나 # 이
		// 질의로 해석될 여지도 없다.
		const int nResult = ::sqlite3_open_v2(_sPath.c_str(), _ppConnection, SQLITE_OPEN_READONLY, nullptr);
		if (nResult != SQLITE_OK) { return(false); }

		// 파이썬 sqlite3.connect 의 기본 timeout=5.0 과 같은 대기다. 이 값이 없으면 잠금 경합에서
		// 즉시 SQLITE_BUSY 가 돌아와 재시도 리듬이 원본과 달라진다.
		::sqlite3_busy_timeout(*_ppConnection, 5000);
		return(true);
	}

	E_BACKUP_RESULT C_BACKUP_SERVICE::copy_database_(
		const std::string& _sSource, const std::string& _sDestination)
	{
		sqlite3* pSource = nullptr;
		if (!this->open_read_only_(_sSource, &pSource))
		{
			this->set_error_(std::string("백업 원본을 열지 못했습니다: ")
				+ (pSource != nullptr ? ::sqlite3_errmsg(pSource) : _sSource.c_str()));
			::sqlite3_close(pSource);
			return(E_BACKUP_RESULT::Failed);
		}

		sqlite3*  pDestination = nullptr;
		const int nOpen = ::sqlite3_open_v2(
			_sDestination.c_str(), &pDestination,
			SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
		if (nOpen != SQLITE_OK)
		{
			this->set_error_(std::string("백업 대상을 열지 못했습니다: ")
				+ (pDestination != nullptr ? ::sqlite3_errmsg(pDestination) : ::sqlite3_errstr(nOpen)));
			::sqlite3_close(pDestination);
			::sqlite3_close(pSource);
			return(E_BACKUP_RESULT::Failed);
		}
		::sqlite3_busy_timeout(pDestination, 5000);

		E_BACKUP_RESULT eResult = E_BACKUP_RESULT::Ok;
		sqlite3_backup* pBackup = ::sqlite3_backup_init(pDestination, "main", pSource, "main");
		if (pBackup == nullptr)
		{
			this->set_error_(std::string("온라인 백업을 시작하지 못했습니다: ")
				+ ::sqlite3_errmsg(pDestination));
			eResult = E_BACKUP_RESULT::Failed;
		}
		else
		{
			int  nStep      = SQLITE_OK;
			bool bCancelled = false;
			for (;;)
			{
				if (m_StepOptions.fnShouldContinue)
				{
					const int nRemaining = ::sqlite3_backup_remaining(pBackup);
					const int nPageCount = ::sqlite3_backup_pagecount(pBackup);
					if (!m_StepOptions.fnShouldContinue(nRemaining, nPageCount))
					{
						bCancelled = true;
						break;
					}
				}

				nStep = ::sqlite3_backup_step(pBackup, m_StepOptions.nPagesPerStep);
				if (nStep == SQLITE_DONE) { break; }
				if (nStep == SQLITE_OK) { continue; }
				if (nStep == SQLITE_BUSY || nStep == SQLITE_LOCKED)
				{
					// 원본 Connection.backup 도 BUSY/LOCKED 를 쉬었다가 다시 시도한다.
					const auto Sleep = std::chrono::duration<double>(m_StepOptions.dBusyRetrySeconds);
					std::this_thread::sleep_for(std::chrono::duration_cast<std::chrono::milliseconds>(Sleep));
					continue;
				}
				break;
			}

			const int nFinish = ::sqlite3_backup_finish(pBackup);
			if (bCancelled)
			{
				this->set_error_("온라인 백업이 취소되었습니다.");
				eResult = E_BACKUP_RESULT::Cancelled;
			}
			else if (nStep != SQLITE_DONE)
			{
				this->set_error_(std::string("온라인 백업에 실패했습니다: ") + ::sqlite3_errstr(nStep));
				eResult = E_BACKUP_RESULT::Failed;
			}
			else if (nFinish != SQLITE_OK)
			{
				this->set_error_(std::string("온라인 백업을 마무리하지 못했습니다: ")
					+ ::sqlite3_errstr(nFinish));
				eResult = E_BACKUP_RESULT::Failed;
			}
		}

		::sqlite3_close(pDestination);
		::sqlite3_close(pSource);
		return(eResult);
	}

	E_BACKUP_RESULT C_BACKUP_SERVICE::inspect_connection_(sqlite3* _pConnection, int* _pnSchemaVersion)
	{
		std::string sError;
		if (!RunQuickCheck(_pConnection, &sError)) { return(this->integrity_(sError)); }
		if (!read_schema_version(_pConnection, _pnSchemaVersion, &sError)) { return(this->integrity_(sError)); }

		// 지원 범위 밖 버전은 아래 검사를 건너뛴다. 범위 판정 자체는 연결을 닫은 뒤다(:97~105).
		const int nLatest = migrations::LatestSchemaVersion();
		if (*_pnSchemaVersion < 0 || *_pnSchemaVersion > nLatest) { return(E_BACKUP_RESULT::Ok); }

		if (!validate_schema_tables(_pConnection, *_pnSchemaVersion, &sError)) { return(this->integrity_(sError)); }
		if (!validate_foreign_keys(_pConnection, &sError)) { return(this->integrity_(sError)); }
		if (*_pnSchemaVersion >= 1)
		{
			if (!validate_logical_integrity(_pConnection, &sError)) { return(this->integrity_(sError)); }
		}
		return(E_BACKUP_RESULT::Ok);
	}

	E_BACKUP_RESULT C_BACKUP_SERVICE::Inspect(const std::string& _sPath, S_BACKUP_INSPECTION* _pOut)
	{
		m_sLastError.clear();

		if (!m_FileSystem.IsRegularFile(_sPath))
		{
			return(this->integrity_("백업 파일이 없습니다: " + _sPath));
		}

		sqlite3* pConnection = nullptr;
		if (!this->open_read_only_(_sPath, &pConnection))
		{
			::sqlite3_close(pConnection);
			return(this->integrity_("올바른 SQLite 백업 파일이 아닙니다."));
		}

		int                   nSchemaVersion = 0;
		const E_BACKUP_RESULT eResult = this->inspect_connection_(pConnection, &nSchemaVersion);
		::sqlite3_close(pConnection);
		if (eResult != E_BACKUP_RESULT::Ok) { return(eResult); }

		const int nLatest = migrations::LatestSchemaVersion();
		if (nSchemaVersion < 0 || nSchemaVersion > nLatest)
		{
			this->set_error_("지원하지 않는 백업 schema version입니다: " + std::to_string(nSchemaVersion));
			return(E_BACKUP_RESULT::Unsupported);
		}

		_pOut->sPath          = _sPath;
		_pOut->nSchemaVersion = nSchemaVersion;
		return(E_BACKUP_RESULT::Ok);
	}

	E_BACKUP_RESULT C_BACKUP_SERVICE::Create(
		const std::string& _sSource, const std::string& _sDestination, S_BACKUP_INSPECTION* _pOut)
	{
		m_sLastError.clear();

		if (!m_FileSystem.IsRegularFile(_sSource))
		{
			this->set_error_("백업할 데이터베이스 파일이 없습니다: " + _sSource);
			return(E_BACKUP_RESULT::SourceMissing);
		}
		if (m_FileSystem.Exists(_sDestination))
		{
			this->set_error_("기존 백업을 덮어쓰지 않습니다: " + _sDestination);
			return(E_BACKUP_RESULT::DestinationExists);
		}

		const std::string sParent = parent_directory(_sDestination);
		if (!m_FileSystem.CreateDirectories(sParent)) { return(this->file_system_failed_()); }

		std::string sTemporary;
		if (!m_FileSystem.CreateUniqueTemporaryPath(
			sParent, temporary_prefix(_sDestination), ".tmp", &sTemporary))
		{
			return(this->file_system_failed_());
		}

		E_BACKUP_RESULT eResult = this->copy_database_(_sSource, sTemporary);
		if (eResult == E_BACKUP_RESULT::Ok)
		{
			S_BACKUP_INSPECTION Inspection;
			eResult = this->Inspect(sTemporary, &Inspection);
			if (eResult == E_BACKUP_RESULT::Ok)
			{
				if (m_FileSystem.Replace(sTemporary, _sDestination))
				{
					_pOut->sPath          = _sDestination;
					_pOut->nSchemaVersion = Inspection.nSchemaVersion;
				}
				else
				{
					eResult = this->file_system_failed_();
				}
			}
		}

		// 원본의 finally 다 - 성공 경로에서도 부른다(:136~137). 교체를 마쳤으면 이미 없는 이름이다.
		// 파이썬에서 finally 안의 예외는 진행 중이던 결과를 덮으므로 삭제 실패는 성공까지 실패로
		// 바꾼다. 그 동작까지 옮긴다.
		if (!m_FileSystem.Remove(sTemporary)) { return(this->file_system_failed_()); }
		return(eResult);
	}

	std::vector<std::string> C_BACKUP_SERVICE::rollback_preserved_(
		const std::vector<std::string>&                         _MovedPaths,
		const std::vector<std::pair<std::string, std::string>>& _PreservedPaths)
	{
		// 원본 _restore_preserved_database_set(:496~516). 설치 교체는 복원 절차의 마지막
		// 연산이라 성공한 뒤 이 롤백이 불리는 상태는 없다 - destination 을 직접 옮기던
		// 구 1단계 분기는 도달 가능한 유일 상태(첫 비켜두기 실패)에서 원본 DB 를 임시
		// 이름으로 옮겨 말미 정리가 지우게 했으므로 파이썬 원본과 함께 제거했다.
		std::vector<std::string> Failures;

		// 옮긴 순서의 역순으로 되돌린다(:505).
		for (std::size_t i = _MovedPaths.size(); i > 0; --i)
		{
			const std::string& sPath = _MovedPaths[i - 1];
			for (const std::pair<std::string, std::string>& Pair : _PreservedPaths)
			{
				if (Pair.first != sPath) { continue; }
				if (!m_FileSystem.Replace(Pair.second, sPath)) { Failures.push_back(sPath); }
				break;
			}
		}

		// 옮기지 못한 자리의 예약 파일은 빈 자리표시자다(:513~515) - 지워서 잔존물을 남기지
		// 않는다. 옮긴 자리의 예약 파일은 원본 데이터를 들고 있으므로 건드리지 않는다.
		for (const std::pair<std::string, std::string>& Pair : _PreservedPaths)
		{
			if (!contains_path(_MovedPaths, Pair.first)) { m_FileSystem.Remove(Pair.second); }
		}
		return(Failures);
	}

	E_BACKUP_RESULT C_BACKUP_SERVICE::restore_body_(
		const std::string&              _sDestination,
		const std::vector<std::string>& _ExistingPaths,
		const std::string&              _sTemporaryPath,
		int*                            _pnSchemaVersion)
	{
		S_BACKUP_INSPECTION Restored;
		const E_BACKUP_RESULT eInspected = this->Inspect(_sTemporaryPath, &Restored);
		if (eInspected != E_BACKUP_RESULT::Ok) { return(eInspected); }
		*_pnSchemaVersion = Restored.nSchemaVersion;

		// 비켜 둘 이름은 옮기기 전에 전부 만든다(:173~174). 여기서 실패하면 아직 아무것도
		// 옮기지 않았으므로 이미 만든 예약 파일만 지우고 물러난다.
		std::vector<std::pair<std::string, std::string>> Preserved;
		for (const std::string& sPath : _ExistingPaths)
		{
			std::string sPreserved;
			if (!m_FileSystem.CreateUniqueTemporaryPath(
				parent_directory(sPath), temporary_prefix(sPath), ".tmp", &sPreserved))
			{
				// Remove 가 LastError 를 덮으므로 실패 사유를 먼저 잡아 둔다.
				const std::string sError = m_FileSystem.LastError();
				for (const std::pair<std::string, std::string>& Pair : Preserved)
				{
					m_FileSystem.Remove(Pair.second);
				}
				this->set_error_(sError);
				return(E_BACKUP_RESULT::Failed);
			}
			Preserved.emplace_back(sPath, sPreserved);
		}

		std::vector<std::string> Moved;
		bool                     bFailed = false;
		for (const std::pair<std::string, std::string>& Pair : Preserved)
		{
			if (!m_FileSystem.Replace(Pair.first, Pair.second)) { bFailed = true; break; }
			Moved.push_back(Pair.first);
		}
		if (!bFailed && !m_FileSystem.Replace(_sTemporaryPath, _sDestination)) { bFailed = true; }

		if (bFailed)
		{
			// 원본은 원래 예외를 그대로 다시 올리므로(:192) 사유도 파일시스템이 준 것을 쓴다.
			const std::string sOriginalError = m_FileSystem.LastError();
			const std::vector<std::string> Failures = this->rollback_preserved_(Moved, Preserved);
			if (!Failures.empty())
			{
				m_RollbackFailedPaths = Failures;
				this->set_error_("데이터베이스 복원과 원본 세트 롤백에 실패했습니다.");
				return(E_BACKUP_RESULT::RollbackFailed);
			}
			this->set_error_(sOriginalError);
			return(E_BACKUP_RESULT::Failed);
		}

		// 원본 _discard_preserved_database_set(:519~524) - 지우지 못해도 복원 자체는 성공이다.
		for (const std::pair<std::string, std::string>& Pair : Preserved)
		{
			m_FileSystem.Remove(Pair.second);
		}
		return(E_BACKUP_RESULT::Ok);
	}

	E_BACKUP_RESULT C_BACKUP_SERVICE::Restore(
		const std::string&   _sBackupPath,
		const std::string&   _sDestination,
		bool                 _bOverwrite,
		S_BACKUP_INSPECTION* _pOut)
	{
		m_sLastError.clear();
		m_RollbackFailedPaths.clear();

		S_BACKUP_INSPECTION Source;
		const E_BACKUP_RESULT eInspected = this->Inspect(_sBackupPath, &Source);
		if (eInspected != E_BACKUP_RESULT::Ok) { return(eInspected); }

		// 원본 _database_file_set(:479~484) - 문자열을 그대로 이어 붙인 세 경로다.
		const std::string DatabaseSet[3] = {
			_sDestination,
			_sDestination + "-wal",
			_sDestination + "-shm",
		};

		// 원본 _validate_restore_targets(:487~493) - 아무것도 옮기기 전에 본다.
		for (const std::string& sPath : DatabaseSet)
		{
			const bool bSymlink = m_FileSystem.IsSymlink(sPath);
			if ((m_FileSystem.Exists(sPath) || bSymlink) && (!m_FileSystem.IsRegularFile(sPath) || bSymlink))
			{
				this->set_error_("복원 대상 DB 세트 경로가 올바르지 않습니다: " + sPath);
				return(E_BACKUP_RESULT::TargetInvalid);
			}
		}

		std::vector<std::string> Existing;
		for (const std::string& sPath : DatabaseSet)
		{
			if (m_FileSystem.Exists(sPath)) { Existing.push_back(sPath); }
		}
		if (!Existing.empty() && !_bOverwrite)
		{
			this->set_error_("기존 데이터베이스를 덮어쓰지 않습니다: " + _sDestination);
			return(E_BACKUP_RESULT::DestinationExists);
		}

		const std::string sParent = parent_directory(_sDestination);
		if (!m_FileSystem.CreateDirectories(sParent)) { return(this->file_system_failed_()); }

		std::string sTemporary;
		if (!m_FileSystem.CreateUniqueTemporaryPath(
			sParent, temporary_prefix(_sDestination), ".tmp", &sTemporary))
		{
			return(this->file_system_failed_());
		}

		E_BACKUP_RESULT eResult = this->copy_database_(_sBackupPath, sTemporary);
		if (eResult == E_BACKUP_RESULT::Ok)
		{
			int nSchemaVersion = 0;
			eResult = this->restore_body_(_sDestination, Existing, sTemporary, &nSchemaVersion);
			if (eResult == E_BACKUP_RESULT::Ok)
			{
				_pOut->sPath          = _sDestination;
				_pOut->nSchemaVersion = nSchemaVersion;
			}
		}

		// 원본의 finally 다(:198~199). 이 시점의 임시 이름은 새 본체가 남긴 것뿐이다 -
		// 게시를 마쳤으면 이미 없는 이름이고, Create 와 같은 이유로 삭제 실패는 결과를 덮는다.
		if (!m_FileSystem.Remove(sTemporary)) { return(this->file_system_failed_()); }
		return(eResult);
	}

	// ------------------------------------------------------------------------------------------
	// C_MIGRATION_BACKUP_HOOK
	// ------------------------------------------------------------------------------------------
	C_MIGRATION_BACKUP_HOOK::C_MIGRATION_BACKUP_HOOK(
		C_BACKUP_SERVICE&                 _service,
		const std::optional<std::string>& _sBackupDirectory,
		WallClockUsFn                     _fnClock)
		: m_Service(_service)
		, m_sBackupDirectory(_sBackupDirectory)
		, m_fnClock(std::move(_fnClock))
	{
	}

	bool C_MIGRATION_BACKUP_HOOK::operator()(
		const std::string& _sDatabasePath, int _nCurrentVersion, int _nLatestVersion)
	{
		// 원본 __call__(:215~229). 주입된 디렉터리가 없을 때만 DB 옆의 "backups" 다(:222).
		const std::string sDirectory = m_sBackupDirectory.has_value()
			? *m_sBackupDirectory
			: join_path(parent_directory(_sDatabasePath), "backups");
		const std::string sTimestamp = format_backup_timestamp(m_fnClock());
		const std::string sDestination = join_path(sDirectory,
			file_stem(_sDatabasePath)
			+ ".pre-migration-v" + std::to_string(_nCurrentVersion)
			+ "-to-v" + std::to_string(_nLatestVersion)
			+ "-" + sTimestamp + ".sqlite3");

		S_BACKUP_INSPECTION Inspection;
		if (m_Service.Create(_sDatabasePath, sDestination, &Inspection) != E_BACKUP_RESULT::Ok)
		{
			return(false);
		}
		m_sLastBackupPath = sDestination;
		return(true);
	}

	// ------------------------------------------------------------------------------------------
	// C_AUTOMATIC_BACKUP_MANAGER
	// ------------------------------------------------------------------------------------------
	C_AUTOMATIC_BACKUP_MANAGER::C_AUTOMATIC_BACKUP_MANAGER(
		C_BACKUP_SERVICE&  _service,
		I_FILE_SYSTEM&     _fileSystem,
		const std::string& _sDatabasePath,
		const std::string& _sBackupDirectory,
		double             _dIntervalHours,
		WallClockUsFn      _fnClock)
		: m_Service(_service)
		, m_FileSystem(_fileSystem)
		, m_sDatabasePath(_sDatabasePath)
		, m_sBackupDirectory(_sBackupDirectory)
		, m_fnClock(std::move(_fnClock))
	{
		this->SetIntervalHours(_dIntervalHours);
	}

	bool C_AUTOMATIC_BACKUP_MANAGER::SetIntervalHours(double _dIntervalHours)
	{
		// 원본 set_interval_hours(:249~253). 거절해도 이미 들고 있던 주기는 그대로 둔다 -
		// 원본은 ValueError 를 올리고 _interval 에 손대지 않으므로 관리자는 계속 쓸 수 있다.
		// 생성자가 받은 값이 거절되면 유효한 주기가 한 번도 없으므로 IsValid 가 false 로 남는다.
		if (!(_dIntervalHours > 0.0))
		{
			m_sLastError = "자동 백업 주기는 0시간보다 커야 합니다.";
			return(false);
		}
		m_nIntervalUs    = static_cast<std::int64_t>(std::llround(_dIntervalHours * 3600.0 * 1000000.0));
		m_bValidInterval = true;
		return(true);
	}

	bool C_AUTOMATIC_BACKUP_MANAGER::latest_backup_time_(std::int64_t* _pnValueUs) const
	{
		// 원본 _latest_backup_time(:269~275) - 패턴은 "{stem}.auto-*.sqlite3" 이고 판정은
		// 파일 이름의 시각이 아니라 가장 최근 **수정 시각**이다. 같은 디렉터리의 pre-migration
		// 백업은 접두가 달라 주기 판정에 들어오지 않는다.
		// 와일드카드는 접두/접미 대조로 옮긴다. 원본 Path.glob 은 fnmatch 라 stem 에 대괄호가
		// 있으면 문자 클래스로 해석하지만 이쪽은 리터럴이다 - 실사용 경로에서 나오지 않는 차이라
		// 그대로 두고 사실만 남긴다(계약 대장 §5-17).
		std::vector<std::string> Names;
		if (!m_FileSystem.ListDirectory(m_sBackupDirectory, &Names)) { return(false); }

		const std::string sPrefix = file_stem(m_sDatabasePath) + ".auto-";
		const std::string sSuffix = ".sqlite3";

		bool         bFound  = false;
		std::int64_t nLatest = 0;
		for (const std::string& sName : Names)
		{
			if (sName.size() < sPrefix.size() + sSuffix.size()) { continue; }
			if (!equals_ignore_case(sName.data(), sPrefix.data(), sPrefix.size())) { continue; }
			if (!equals_ignore_case(
				sName.data() + (sName.size() - sSuffix.size()), sSuffix.data(), sSuffix.size()))
			{
				continue;
			}

			std::int64_t nModified = 0;
			if (!m_FileSystem.ModifiedTimeUs(join_path(m_sBackupDirectory, sName), &nModified)) { continue; }
			if (!bFound || nModified > nLatest)
			{
				nLatest = nModified;
				bFound  = true;
			}
		}

		if (bFound) { *_pnValueUs = nLatest; }
		return(bFound);
	}

	E_BACKUP_RESULT C_AUTOMATIC_BACKUP_MANAGER::RunIfDue(
		bool _bForce, bool* _pbCreated, std::string* _psDestination)
	{
		*_pbCreated = false;
		_psDestination->clear();

		if (!m_bValidInterval)
		{
			m_sLastError = "자동 백업 주기는 0시간보다 커야 합니다.";
			return(E_BACKUP_RESULT::Failed);
		}
		m_sLastError.clear();

		const std::int64_t nNow = m_fnClock();

		// 원본은 기억한 시각이 없을 때만 디스크를 본다(:258).
		std::int64_t nLast    = m_nLastBackupAtUs;
		bool         bHasLast = m_bHasLastBackup;
		if (!bHasLast) { bHasLast = this->latest_backup_time_(&nLast); }

		if (!_bForce && bHasLast && (nNow - nLast) < m_nIntervalUs) { return(E_BACKUP_RESULT::Ok); }

		const std::string sDestination = join_path(m_sBackupDirectory,
			file_stem(m_sDatabasePath) + ".auto-" + format_backup_timestamp(nNow) + ".sqlite3");

		S_BACKUP_INSPECTION   Inspection;
		const E_BACKUP_RESULT eResult = m_Service.Create(m_sDatabasePath, sDestination, &Inspection);
		if (eResult != E_BACKUP_RESULT::Ok)
		{
			// 원본은 예외를 그대로 올리므로 마지막 백업 시각을 갱신하지 않는다(:265~266).
			m_sLastError = m_Service.LastError();
			return(eResult);
		}

		m_nLastBackupAtUs = nNow;
		m_bHasLastBackup  = true;
		*_pbCreated       = true;
		*_psDestination   = sDestination;
		return(E_BACKUP_RESULT::Ok);
	}

	// ------------------------------------------------------------------------------------------
	// C_PERIODIC_QUICK_CHECK
	// ------------------------------------------------------------------------------------------
	C_PERIODIC_QUICK_CHECK::C_PERIODIC_QUICK_CHECK(
		sqlite3* _pConnection, double _dIntervalHours, MonotonicSecFn _fnClock)
		: m_pConnection(_pConnection)
		, m_fnClock(std::move(_fnClock))
	{
		// 원본 __init__(:288~293).
		if (_dIntervalHours > 0.0)
		{
			m_dIntervalSeconds = _dIntervalHours * 60.0 * 60.0;
			m_bValidInterval   = true;
		}
		else
		{
			m_sLastError = "quick_check 주기는 0시간보다 커야 합니다.";
		}
	}

	E_QUICK_CHECK_RESULT C_PERIODIC_QUICK_CHECK::RunIfDue(bool _bForce)
	{
		if (!m_bValidInterval)
		{
			m_sLastError = "quick_check 주기는 0시간보다 커야 합니다.";
			return(E_QUICK_CHECK_RESULT::Failed);
		}

		// 원본 run_if_due(:295~306).
		const double dNow = m_fnClock();
		if (!_bForce && m_bHasLastCheck && (dNow - m_dLastCheckAt) < m_dIntervalSeconds)
		{
			return(E_QUICK_CHECK_RESULT::Skipped);
		}

		std::string sError;
		if (!RunQuickCheck(m_pConnection, &sError))
		{
			// 원본은 예외를 올리므로 마지막 검사 시각을 갱신하지 않는다(:304~305).
			m_sLastError = sError;
			return(E_QUICK_CHECK_RESULT::Failed);
		}

		m_sLastError.clear();
		m_dLastCheckAt  = dNow;
		m_bHasLastCheck = true;
		return(E_QUICK_CHECK_RESULT::Passed);
	}
}
