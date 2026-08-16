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
// 대조하려면 이식본이 만든 실물이 필요하고, 그 실물을 뽑는 것이 이 시험 케이스의 전부다.
//
// 태그 [.] 로 숨겨 두어 인자 없는 NoteExTests.exe 실행에서는 돌지 않는다.
// 호출 규약(다른 게이트가 이 형태에 의존한다):
//   NOTEEX_PARITY_DB=<경로> NoteExTests.exe "[parity-emit]"   -> 성공 시 종료코드 0
//
// 대응 원본: src/pynote/infrastructure/migrations/v0001_initial.py 의 migrate 가 만드는 스키마.
TEST_CASE("v0001 parity 데이터베이스를 생성한다", "[.][parity-emit]")
{
	// std::getenv 는 MSVC 에서 C4996 이라 _dupenv_s 를 쓴다. 변수가 없어도 반환값은 0 이고
	// 대신 포인터가 null 로 남는다 - 존재 판정은 반환값이 아니라 포인터로 한다.
	char*             pszEnv   = nullptr;
	std::size_t       nEnvSize = 0;
	const auto        nEnvResult = ::_dupenv_s(&pszEnv, &nEnvSize, "NOTEEX_PARITY_DB");
	const std::unique_ptr<char, void(*)(void*)> EnvGuard(pszEnv, &std::free);

	INFO("NOTEEX_PARITY_DB 환경변수에 생성할 데이터베이스 경로를 지정해야 한다.");
	REQUIRE(nEnvResult == 0);
	REQUIRE(pszEnv != nullptr);
	REQUIRE(pszEnv[0] != '\0');

	// 환경변수는 실행 문자셋 바이트로 들어오므로 path 를 거쳐 UTF-8 로 되돌린다.
	// C_DATABASE::Open 은 UTF-8 경로를 받고 sqlite3 도 UTF-8 을 기대한다.
	const std::filesystem::path OutPath(pszEnv);
	const std::u8string u8Path = OutPath.u8string();
	const std::string sUtf8Path(reinterpret_cast<const char*>(u8Path.c_str()), u8Path.size());

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
