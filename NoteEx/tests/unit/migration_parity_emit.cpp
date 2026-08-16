#include <catch_amalgamated.hpp>

#include "pynote/core/storage/migration_runner.h"

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#pragma comment(lib, "NoteExCore")

// parity 대조용 데이터베이스 생성기다. 파이썬 원본이 만든 데이터베이스와 스키마를 바이트
// 대조하려면 이식본이 만든 실물이 필요하고, 그 실물을 뽑는 것이 이 파일의 전부다.
//
// 태그 [.] 로 숨겨 두어 인자 없는 NoteExTests.exe 실행에서는 돌지 않는다.
// 호출 규약(다른 게이트가 이 형태에 의존한다):
//   NOTEEX_PARITY_DB=<경로> NoteExTests.exe "[parity-emit]"      -> 새 데이터베이스를 v9 로 만든다
//   NOTEEX_PARITY_DB=<경로> NoteExTests.exe "[parity-upgrade]"   -> 이미 있는 데이터베이스를 v9 로 올린다
// 둘 다 성공 시 종료코드 0 이다.

namespace
{
	// 환경변수에서 대상 경로를 읽어 UTF-8 로 되돌린다. 없으면 그 자리에서 실패한다.
	// std::getenv 는 MSVC 에서 C4996 이라 _dupenv_s 를 쓴다. 변수가 없어도 반환값은 0 이고
	// 대신 포인터가 null 로 남는다 - 존재 판정은 반환값이 아니라 포인터로 한다.
	std::filesystem::path parity_target_path()
	{
		char*             pszEnv     = nullptr;
		std::size_t       nEnvSize   = 0;
		const auto        nEnvResult = ::_dupenv_s(&pszEnv, &nEnvSize, "NOTEEX_PARITY_DB");
		const std::unique_ptr<char, void(*)(void*)> EnvGuard(pszEnv, &std::free);

		INFO("NOTEEX_PARITY_DB 환경변수에 대상 데이터베이스 경로를 지정해야 한다.");
		REQUIRE(nEnvResult == 0);
		REQUIRE(pszEnv != nullptr);
		REQUIRE(pszEnv[0] != '\0');

		return(std::filesystem::path(pszEnv));
	}

	// C_DATABASE::Open 은 UTF-8 경로를 받고 sqlite3 도 UTF-8 을 기대한다.
	// 환경변수는 실행 문자셋 바이트로 들어오므로 path 를 거쳐 UTF-8 로 되돌린다.
	std::string to_utf8(const std::filesystem::path& _Path)
	{
		const std::u8string u8Path = _Path.u8string();
		return(std::string(reinterpret_cast<const char*>(u8Path.c_str()), u8Path.size()));
	}
}

// 대응 원본: src/pynote/infrastructure/migrations/ 의 v0001~v0009 가 빈 데이터베이스에 만드는 스키마.
// 이 TEST_CASE 자체는 parity 대조용 생성기라 대응하는 파이썬 시험이 없다 - 뽑은 실물의
// 대조는 tools/gates/check_migration_ladder_parity.py 가 한다.
// pytest node ID 는 존재하지 않는다.
TEST_CASE("parity 데이터베이스를 새로 생성한다", "[.][parity-emit]")
{
	const std::filesystem::path OutPath  = parity_target_path();
	const std::string           sUtf8Path = to_utf8(OutPath);

	// 이전 실행이 남긴 본체와 WAL/SHM 사이드카를 지우고 시작한다. 남기면 이전 스키마를
	// 물려받은 데이터베이스가 parity 대조에 실려 나간다.
	std::error_code ec;
	std::filesystem::remove(OutPath, ec);
	std::filesystem::remove(OutPath.string() + "-wal", ec);
	std::filesystem::remove(OutPath.string() + "-shm", ec);
	REQUIRE_FALSE(std::filesystem::exists(OutPath));

	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(sUtf8Path));

	pynote::core::storage::C_MIGRATION_RUNNER runner;
	runner.SetExistingDatabase(false, sUtf8Path);
	REQUIRE(runner.Run(db) == pynote::core::storage::E_MIGRATE_RESULT::Ok);

	// 닫으면서 WAL 이 본체로 체크포인트된다. 사이드카가 남으면 대조 대상이 불완전해진다.
	db.Close();
	REQUIRE_FALSE(db.IsOpen());
	REQUIRE(std::filesystem::exists(OutPath));
}

// 대응 원본: database.py 의 기존 데이터베이스 경로(_had_database = True) 로 러너를 도는 흐름.
// 사다리 게이트가 중간 버전 fixture 를 여기로 밀어 넣어 v9 까지 올린다 - 대상이 없으면
// 실패하는 것이 맞다. 없는 파일을 새로 만들면 fixture 를 올린 것이 아니라 새로 만든 것이 된다.
// 이 TEST_CASE 도 parity 대조용 생성기라 대응하는 파이썬 시험이 없다 - 올린 실물의
// 대조는 tools/gates/check_migration_ladder_parity.py 가 한다.
// pytest node ID 는 존재하지 않는다.
TEST_CASE("이미 있는 데이터베이스를 최신 스키마로 올린다", "[.][parity-upgrade]")
{
	const std::filesystem::path OutPath  = parity_target_path();
	const std::string           sUtf8Path = to_utf8(OutPath);

	INFO("[parity-upgrade] 는 이미 있는 데이터베이스만 올린다: " << OutPath.string());
	REQUIRE(std::filesystem::exists(OutPath));

	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(sUtf8Path));

	pynote::core::storage::C_MIGRATION_RUNNER runner;
	runner.SetExistingDatabase(true, sUtf8Path);
	REQUIRE(runner.Run(db) == pynote::core::storage::E_MIGRATE_RESULT::Ok);

	db.Close();
	REQUIRE_FALSE(db.IsOpen());
	REQUIRE(std::filesystem::exists(OutPath));
}
