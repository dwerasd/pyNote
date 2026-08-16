#include <catch_amalgamated.hpp>

#include "pynote/core/storage/database.h"

#include <sqlite3/sqlite3.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#pragma comment(lib, "NoteExCore")

// 연결 상태 방출기. `NOTEEX_PRAGMA_OUT` 이 가리키는 파일에 C_DATABASE 로 연 연결의
// 실효 PRAGMA 값을 적는다.
//
// 왜 파일에 적는가: 이 값들은 **연결 속성이라 데이터베이스 파일에 영속되지 않는다.**
// 방출된 DB 를 나중에 파이썬으로 열어 읽으면 파이썬 연결의 값이지 C++ 연결의 값이
// 아니다. 대조하려면 C++ 연결 자신이 살아 있는 동안 보고해야 한다.
//
// 지시서 T-R5 는 특정 값이 아니라 **파이썬 연결과 C++ 연결 실효값의 대조 일치**를
// gate 로 삼으라고 한다. 그래서 이 방출기는 판정하지 않고 관측만 적는다 - 판정은
// tools/gates/check_connection_parity.py 가 파이썬 쪽을 같은 방식으로 재고 비교한다.
namespace
{
	std::string environment_value(const char* _pszName)
	{
		char*  pszValue = nullptr;
		size_t nSize    = 0;
		if (::_dupenv_s(&pszValue, &nSize, _pszName) != 0 || pszValue == nullptr)
		{
			return(std::string{});
		}
		const std::unique_ptr<char, decltype(&std::free)> Owned(pszValue, &std::free);
		return(std::string(pszValue));
	}

	// PRAGMA 한 건을 문자열로 읽는다. 행이 없으면 빈 문자열이다.
	std::string read_pragma(sqlite3* _pHandle, const std::string& _sName)
	{
		sqlite3_stmt* pStmt = nullptr;
		const std::string sSql = "PRAGMA " + _sName;
		if (::sqlite3_prepare_v2(_pHandle, sSql.c_str(), -1, &pStmt, nullptr) != SQLITE_OK)
		{
			return(std::string{});
		}

		std::string sValue;
		if (::sqlite3_step(pStmt) == SQLITE_ROW)
		{
			const unsigned char* pText = ::sqlite3_column_text(pStmt, 0);
			sValue.assign(pText ? reinterpret_cast<const char*>(pText) : "");
		}
		::sqlite3_finalize(pStmt);
		return(sValue);
	}
}

// 대응 원본: 없음. 이것은 이식된 시험이 아니라 게이트가 쓰는 숨은 방출기다 -
// check_connection_parity.py 가 C++ 연결의 실효값을 얻는 유일한 경로이며 파이썬 쪽에
// 대응 케이스가 존재할 수 없다. node ID 도 존재하지 않는다.
TEST_CASE("연결 상태 대조용 PRAGMA 실효값을 방출한다", "[.][pragma-emit]")
{
	const std::string sOut = environment_value("NOTEEX_PRAGMA_OUT");
	REQUIRE_FALSE(sOut.empty());

	// 방출용 임시 데이터베이스. 사이드카까지 지우고 시작한다.
	const std::filesystem::path DbPath =
		std::filesystem::temp_directory_path() / "noteex_pragma_emit.db";
	std::error_code ec;
	std::filesystem::remove(DbPath, ec);
	std::filesystem::remove(DbPath.string() + "-wal", ec);
	std::filesystem::remove(DbPath.string() + "-shm", ec);

	pynote::core::storage::C_DATABASE Db;
	REQUIRE(Db.Open(DbPath.string()));

	// 파이썬 쪽 게이트가 재는 것과 같은 목록이고 순서도 같다.
	const char* const PRAGMAS[] = {
		"synchronous", "journal_mode", "foreign_keys",
		"page_size", "encoding", "auto_vacuum", "temp_store",
	};

	std::ofstream Out(sOut, std::ios::binary | std::ios::trunc);
	REQUIRE(Out.is_open());
	for (const char* pszName : PRAGMAS)
	{
		Out << pszName << '=' << read_pragma(Db.Handle(), pszName) << '\n';
	}
	Out << "sqlite_version=" << ::sqlite3_libversion() << '\n';
	Out.close();

	Db.Close();
	std::filesystem::remove(DbPath, ec);
	std::filesystem::remove(DbPath.string() + "-wal", ec);
	std::filesystem::remove(DbPath.string() + "-shm", ec);

	REQUIRE(std::filesystem::exists(sOut));
}
