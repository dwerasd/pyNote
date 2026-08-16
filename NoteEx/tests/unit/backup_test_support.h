#pragma once

#include <catch_amalgamated.hpp>

#include "pynote/core/domain/models.h"
#include "pynote/core/storage/backup.h"
#include "pynote/core/storage/database.h"
#include "pynote/core/storage/migration_runner.h"
#include "pynote/core/storage/migrations/registry.h"
#include "pynote/core/storage/repositories.h"
#include "pynote/platform/win32_file_system.h"

#include <sqlite3/sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

// 백업 시험 세 벌이 함께 쓰는 도구다. 시험용 임시 트리, 최신 스키마 시드, 그리고 파일
// 연산에 고장을 주입하는 파일시스템 래퍼가 들어 있다.

namespace backup_support
{
	namespace domain   = pynote::core::domain;
	namespace storage  = pynote::core::storage;
	namespace platform = pynote::platform;

	// 좁은 리터럴은 이 기계에서 CP949 로 컴파일되므로(spec_TR2 §1(a)) 저장할 한국어는 u8 로 쓴다.
	inline std::string u8s(const char8_t* _pszText)
	{
		return(std::string(reinterpret_cast<const char*>(_pszText)));
	}

	// 시험 하나가 쓰는 임시 디렉터리. 소멸 시 통째로 지운다 - 백업/복원은 사이드카까지
	// 남기므로 파일 단위로 지우면 다음 시험이 이전 상태를 물려받는다.
	class C_TEMP_TREE
	{
	public:
		explicit C_TEMP_TREE(const std::string& _sName)
		{
			m_Path = std::filesystem::temp_directory_path() / ("noteex_backup_" + _sName);
			std::error_code ec;
			std::filesystem::remove_all(m_Path, ec);
			std::filesystem::create_directories(m_Path, ec);
		}

		~C_TEMP_TREE()
		{
			std::error_code ec;
			std::filesystem::remove_all(m_Path, ec);
		}

		C_TEMP_TREE(const C_TEMP_TREE&) = delete;
		C_TEMP_TREE& operator=(const C_TEMP_TREE&) = delete;

		std::string Utf8() const { return(m_Path.string()); }
		std::string Child(const std::string& _sName) const { return((m_Path / _sName).string()); }

	private:
		std::filesystem::path m_Path;
	};

	inline void write_bytes(const std::string& _sPath, const std::string& _sBytes)
	{
		std::ofstream Stream(_sPath, std::ios::binary | std::ios::trunc);
		REQUIRE(Stream.is_open());
		Stream.write(_sBytes.data(), static_cast<std::streamsize>(_sBytes.size()));
	}

	inline std::string read_bytes(const std::string& _sPath)
	{
		std::ifstream Stream(_sPath, std::ios::binary);
		if (!Stream.is_open()) { return(std::string{}); }
		return(std::string((std::istreambuf_iterator<char>(Stream)), std::istreambuf_iterator<char>()));
	}

	inline void execute_sql(const std::string& _sPath, const char* _pszSql)
	{
		sqlite3* pConnection = nullptr;
		REQUIRE(::sqlite3_open_v2(
			_sPath.c_str(), &pConnection, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK);
		char*     pErrorMessage = nullptr;
		const int nResult = ::sqlite3_exec(pConnection, _pszSql, nullptr, nullptr, &pErrorMessage);
		if (pErrorMessage != nullptr) { ::sqlite3_free(pErrorMessage); }
		::sqlite3_close(pConnection);
		REQUIRE(nResult == SQLITE_OK);
	}

	// quick_check 가 오류를 행으로 **보고**하는 상태를 만든다. 페이지를 망가뜨리는 방법은
	// PRAGMA 실행 자체를 실패시켜(실측 2026-08-16: "database disk image is malformed" 예외)
	// 다른 갈래로 가므로, 스키마만 바꿔 NOT NULL 위반을 남긴다.
	inline void make_quick_check_violation(const std::string& _sPath)
	{
		execute_sql(_sPath, "CREATE TABLE t (v TEXT)");
		execute_sql(_sPath, "INSERT INTO t (v) VALUES (NULL)");
		execute_sql(_sPath,
			"PRAGMA writable_schema=ON;"
			"UPDATE sqlite_master SET sql = 'CREATE TABLE t (v TEXT NOT NULL)'"
			" WHERE type = 'table' AND name = 't';"
			"PRAGMA writable_schema=OFF;");
	}

	inline std::int64_t query_int(const std::string& _sPath, const char* _pszSql)
	{
		sqlite3* pConnection = nullptr;
		REQUIRE(::sqlite3_open_v2(_sPath.c_str(), &pConnection, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
		sqlite3_stmt* pStmt = nullptr;
		REQUIRE(::sqlite3_prepare_v2(pConnection, _pszSql, -1, &pStmt, nullptr) == SQLITE_OK);
		REQUIRE(::sqlite3_step(pStmt) == SQLITE_ROW);
		const std::int64_t nValue = ::sqlite3_column_int64(pStmt, 0);
		::sqlite3_finalize(pStmt);
		::sqlite3_close(pConnection);
		return(nValue);
	}

	inline std::string query_text(const std::string& _sPath, const char* _pszSql)
	{
		sqlite3* pConnection = nullptr;
		REQUIRE(::sqlite3_open_v2(_sPath.c_str(), &pConnection, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
		sqlite3_stmt* pStmt = nullptr;
		REQUIRE(::sqlite3_prepare_v2(pConnection, _pszSql, -1, &pStmt, nullptr) == SQLITE_OK);
		std::string sValue;
		if (::sqlite3_step(pStmt) == SQLITE_ROW)
		{
			const unsigned char* pText = ::sqlite3_column_text(pStmt, 0);
			const int            nSize = ::sqlite3_column_bytes(pStmt, 0);
			if (pText != nullptr) { sValue.assign(reinterpret_cast<const char*>(pText), static_cast<std::size_t>(nSize)); }
		}
		::sqlite3_finalize(pStmt);
		::sqlite3_close(pConnection);
		return(sValue);
	}

	// 최신 스키마까지 올리고 문서 하나와 카드 _nCards 장을 넣는다. 파이썬 시험의
	// _seed_database(tests/integration/test_backup.py:37~60) 자리다 - 그쪽은 CardService 를
	// 거치지만 그 계층은 아직 이식 전이라 저장소 API 로 같은 모양을 만든다. 입력 작업이
	// 원문을 들고 있어야 검사 사슬의 원문/해시 쌍 갈래까지 실제로 지나간다.
	inline void seed_database(const std::string& _sPath, int _nCards = 3)
	{
		{
			storage::C_DATABASE Database;
			REQUIRE(Database.Open(_sPath));

			storage::C_MIGRATION_RUNNER Runner;
			Runner.SetExistingDatabase(false, _sPath);
			REQUIRE(Runner.Run(Database) == storage::E_MIGRATE_RESULT::Ok);

			storage::C_REPOSITORIES Repositories(Database);

			domain::S_DOCUMENT Document;
			Document.sId          = "document-1";
			Document.sTitle       = u8s(u8"백업 문서");
			Document.nCreatedAtUs = 1000;
			Document.nUpdatedAtUs = 1000;
			REQUIRE(Repositories.CreateDocument(Document) == storage::E_REPO_RESULT::Ok);

			domain::S_NEW_CAPTURE_OPERATION Operation;
			Operation.sId           = "operation-1";
			Operation.sDocumentId   = Document.sId;
			Operation.eSource       = domain::E_CAPTURE_OPERATION_SOURCE::Import;
			Operation.eSplitPolicy  = domain::E_SPLIT_POLICY::SplitByBlankLine;
			Operation.sOriginalText = u8s(u8"첫 카드\n\n둘째 카드");
			Operation.nCreatedAtUs  = 2000;

			std::vector<domain::S_NEW_CARD> NewCards;
			for (int i = 0; i < _nCards; ++i)
			{
				domain::S_NEW_CARD Card;
				Card.sId             = "card-" + std::to_string(i);
				Card.sRevisionId     = "revision-" + std::to_string(i);
				Card.sEventId        = "event-" + std::to_string(i);
				Card.nPositionKey    = (i + 1) * 1024;
				Card.sBody           = u8s(u8"카드 본문 ") + std::to_string(i);
				Card.eCardSource     = domain::E_CARD_SOURCE::Import;
				Card.eEventSource    = domain::E_EVENT_SOURCE::Import;
				Card.eRevisionSource = domain::E_REVISION_SOURCE::Edit;
				Card.nCreatedAtUs    = 2000 + i;
				NewCards.push_back(Card);
			}

			std::vector<domain::S_CARD> Created;
			REQUIRE(Repositories.CreateCards(Operation, NewCards, &Created) == storage::E_REPO_RESULT::Ok);
			REQUIRE(Created.size() == static_cast<std::size_t>(_nCards));
		}

		// 연결이 닫히면 SQLite 가 체크포인트하고 사이드카를 지운다. 백업 대상이 실제 앱의
		// 종료 후 상태와 같아야 하므로 그 자리까지가 시드다.
		REQUIRE_FALSE(std::filesystem::exists(_sPath + "-wal"));
	}

	// 실제 Win32 구현을 감싸 관측과 고장 주입을 더한다. 백업 계층이 파일을 만지는 통로가
	// 인터페이스 하나뿐이라 여기서 게시 실패와 롤백 실패를 결정적으로 만들 수 있다.
	class C_PROBE_FILE_SYSTEM : public storage::I_FILE_SYSTEM
	{
	public:
		// true 를 돌려주면 그 교체는 실패한다.
		std::function<bool(const std::string& _sFrom, const std::string& _sTo)> fnFailReplace;

		// true 를 돌려주면 그 삭제는 실패한다.
		std::function<bool(const std::string& _sPath)> fnFailRemove;

		// 임시 이름을 돌려주기 전에 그 자리에 SQLite 가 아닌 바이트를 채운다.
		bool bCorruptTemporary{ false };

		// 만들어 준 임시 경로 전부. 시험이 임시 파일 잔존 여부를 확인한다.
		std::vector<std::string> TemporaryPaths;

		bool Exists(const std::string& _sPath) const override { return(m_Inner.Exists(_sPath)); }
		bool IsRegularFile(const std::string& _sPath) const override { return(m_Inner.IsRegularFile(_sPath)); }
		bool IsSymlink(const std::string& _sPath) const override { return(m_Inner.IsSymlink(_sPath)); }
		bool CreateDirectories(const std::string& _sPath) override { return(m_Inner.CreateDirectories(_sPath)); }
		bool Remove(const std::string& _sPath) override
		{
			if (fnFailRemove && fnFailRemove(_sPath))
			{
				m_bInjected      = true;
				m_sInjectedError = "주입한 삭제 실패: " + _sPath;
				return(false);
			}
			m_bInjected = false;
			return(m_Inner.Remove(_sPath));
		}

		bool Replace(const std::string& _sFrom, const std::string& _sTo) override
		{
			if (fnFailReplace && fnFailReplace(_sFrom, _sTo))
			{
				m_bInjected     = true;
				m_sInjectedError = "주입한 교체 실패: " + _sFrom + " -> " + _sTo;
				return(false);
			}
			m_bInjected = false;
			return(m_Inner.Replace(_sFrom, _sTo));
		}

		bool CreateUniqueTemporaryPath(
			const std::string& _sDirectory,
			const std::string& _sPrefix,
			const std::string& _sSuffix,
			std::string*       _psPath) override
		{
			if (!m_Inner.CreateUniqueTemporaryPath(_sDirectory, _sPrefix, _sSuffix, _psPath)) { return(false); }
			TemporaryPaths.push_back(*_psPath);
			if (bCorruptTemporary) { write_bytes(*_psPath, "not a sqlite database"); }
			return(true);
		}

		bool ModifiedTimeUs(const std::string& _sPath, std::int64_t* _pnValueUs) const override
		{
			return(m_Inner.ModifiedTimeUs(_sPath, _pnValueUs));
		}

		bool ListDirectory(const std::string& _sDirectory, std::vector<std::string>* _pNames) const override
		{
			return(m_Inner.ListDirectory(_sDirectory, _pNames));
		}

		const std::string& LastError() const override
		{
			return(m_bInjected ? m_sInjectedError : m_Inner.LastError());
		}

		// 기록한 임시 경로가 하나도 남지 않았는가. 원본은 성공 경로에서도 임시 파일을 지운다.
		bool NoTemporaryLeftBehind() const
		{
			for (const std::string& sPath : TemporaryPaths)
			{
				if (m_Inner.Exists(sPath)) { return(false); }
			}
			return(true);
		}

	private:
		platform::C_WIN32_FILE_SYSTEM m_Inner;
		std::string                   m_sInjectedError;
		bool                          m_bInjected{ false };
	};
}
