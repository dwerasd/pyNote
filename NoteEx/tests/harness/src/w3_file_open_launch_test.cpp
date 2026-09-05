#include <catch_amalgamated.hpp>

// CApplication.h 가 windows.h 와 core 헤더를 정해진 순서로 끌고 온다 - 이 TU 는 ATL/WTL 을
// 읽지 않으므로 CreateEvent 매크로 계약(CDocumentPage.cpp:3~8)의 영향을 받지 않는다.
#include "CApplication.h"

#include "pynote/core/storage/migration_runner.h"

#include <sqlite3/sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#pragma comment(lib, "NoteExCore")

// P2 [FS-launch-port] 의 셸 라우팅 시험이다. 판정(CApplication.h 의 ResolveOpenPath)에
// 저장소·카드 서비스·문단 파서·결속 파일 시스템은 실물을 물리고 창·대화상자만 대역으로 받는다.
// 시험 프로젝트가 CApplication.cpp 를 컴파일할 수 없는 제약은 CChangeBus.h 머리 주석이 소유한다.
namespace
{
	namespace app = pynote::core::application;
	namespace domain = pynote::core::domain;
	namespace storage = pynote::core::storage;

	constexpr std::int64_t LAUNCH_CLOCK_US = 1'700'000'000'000'000;

	// path::string() 은 ANSI 코드 페이지로 접는다 - core·platform 이 받는 것은 UTF-8 이라
	// 한글 파일명이 그대로 실패한다(실측). 경로의 좁은 표현은 전부 이 함수를 거친다.
	std::string utf8_path(const std::filesystem::path& _Path)
	{
		const std::u8string sValue = _Path.u8string();
		return(std::string(sValue.begin(), sValue.end()));
	}

	struct S_SHELL_LOG
	{
		std::vector<std::pair<std::wstring, std::wstring>> Dialogs;
		std::vector<std::string> Routed;
		std::vector<std::string> Revealed;
		int nWindows{ 0 };
		int nActivated{ 0 };
	};

	class C_LAUNCH_FIXTURE
	{
	public:
		explicit C_LAUNCH_FIXTURE(const std::string& _sName)
			: m_Root(std::filesystem::temp_directory_path() /
				("NoteEx-W3-FB-launch-" + _sName + "-" + std::to_string(::GetCurrentProcessId())))
			, m_Repositories(m_Database)
		{
			std::error_code Error;
			std::filesystem::remove_all(m_Root, Error);
			REQUIRE(std::filesystem::create_directories(m_Root, Error));
			REQUIRE(m_Database.Open(utf8_path(m_Root / "pynote.sqlite3")));
			storage::C_MIGRATION_RUNNER Runner;
			Runner.SetExistingDatabase(false, utf8_path(m_Root / "pynote.sqlite3"));
			REQUIRE(Runner.Run(m_Database) == storage::E_MIGRATE_RESULT::Ok);
			m_CardService = std::make_unique<app::C_CARD_SERVICE>(
				m_Database, m_Repositories, m_Parser,
				[this]() { return(++m_nClock); },
				[this]() { return("id-" + std::to_string(++m_nSequence)); });

			m_Shell.ShowDialog = [this](const std::wstring& _sTitle, const std::wstring& _sText)
			{
				m_Log.Dialogs.emplace_back(_sTitle, _sText);
			};
			m_Shell.RouteBoundCard = [this](const std::string& _sCardId)
			{
				m_Log.Routed.push_back(_sCardId);
				++m_Log.nActivated;
				return(true);
			};
			m_Shell.CreateWindowForFile = [this](std::string* _psDocumentId)
			{
				*_psDocumentId = this->CreateDocument();
				++m_Log.nWindows;
				return(true);
			};
			m_Shell.RevealAndActivate = [this](const std::string& _sCardId)
			{
				m_Log.Revealed.push_back(_sCardId);
				++m_Log.nActivated;
				return(true);
			};
			m_Shell.Now = []() { return(LAUNCH_CLOCK_US); };
		}

		~C_LAUNCH_FIXTURE()
		{
			m_CardService.reset();
			m_Database.Close();
			std::error_code Error;
			std::filesystem::remove_all(m_Root, Error);
		}

		C_LAUNCH_FIXTURE(const C_LAUNCH_FIXTURE&) = delete;
		C_LAUNCH_FIXTURE& operator=(const C_LAUNCH_FIXTURE&) = delete;

		storage::C_REPOSITORIES& Repo() { return(m_Repositories); }
		app::C_CARD_SERVICE& Cards() { return(*m_CardService); }
		const S_SHELL_LOG& Log() const { return(m_Log); }

		std::string CreateDocument()
		{
			domain::S_DOCUMENT Document;
			Document.sId = "document-" + std::to_string(++m_nSequence);
			Document.sTitle = "launch";
			Document.nCreatedAtUs = 1;
			Document.nUpdatedAtUs = 1;
			REQUIRE(m_Repositories.CreateDocument(Document) == storage::E_REPO_RESULT::Ok);
			return(Document.sId);
		}

		std::filesystem::path Write(const std::wstring& _sName, const std::string& _sBytes)
		{
			const std::filesystem::path Target = m_Root / _sName;
			std::ofstream Stream(Target, std::ios::binary | std::ios::trunc);
			REQUIRE(Stream.is_open());
			Stream.write(_sBytes.data(), static_cast<std::streamsize>(_sBytes.size()));
			Stream.close();
			return(Target);
		}

		std::filesystem::path Root() const { return(m_Root); }

		bool Open(const std::filesystem::path& _Path)
		{
			return(ResolveOpenPath(
				_Path.native(), m_Repositories, *m_CardService, m_Parser, m_FileSystem, m_Shell));
		}

		int BindingCount()
		{
			sqlite3_stmt* pStatement = nullptr;
			REQUIRE(::sqlite3_prepare_v2(m_Database.Handle(),
				"SELECT COUNT(*) FROM card_file_bindings", -1, &pStatement, nullptr) == SQLITE_OK);
			REQUIRE(::sqlite3_step(pStatement) == SQLITE_ROW);
			const int nValue = ::sqlite3_column_int(pStatement, 0);
			::sqlite3_finalize(pStatement);
			return(nValue);
		}

	private:
		std::filesystem::path m_Root;
		storage::C_DATABASE m_Database;
		storage::C_REPOSITORIES m_Repositories;
		domain::C_PARAGRAPH_PARSER m_Parser;
		pynote::platform::C_WIN32_BINDING_FILE_SYSTEM m_FileSystem;
		std::unique_ptr<app::C_CARD_SERVICE> m_CardService;
		S_OPEN_PATH_SHELL m_Shell;
		S_SHELL_LOG m_Log;
		std::int64_t m_nClock{ 1000 };
		int m_nSequence{ 0 };
	};
}

TEST_CASE("W3-FB-201 a new bindable file makes one window one card and one binding row",
	"[W3-file-binding][FS-port]")
{
	C_LAUNCH_FIXTURE Fixture("bind");
	const std::string sBytes = "\xEB\xB3\xB8\xEB\xAC\xB8 \xED\x95\x98\xEB\x82\x98\r\n";
	const auto Path = Fixture.Write(L"결속.txt", sBytes);

	REQUIRE(Fixture.Open(Path));

	REQUIRE(Fixture.Log().nWindows == 1);
	REQUIRE(Fixture.Log().Dialogs.empty());
	REQUIRE(Fixture.Log().Revealed.size() == 1);
	REQUIRE(Fixture.Log().nActivated == 1);
	REQUIRE(Fixture.BindingCount() == 1);

	const std::string sCardId = Fixture.Log().Revealed.front();
	domain::S_FILE_BINDING Binding;
	REQUIRE(Fixture.Repo().GetFileBinding(sCardId, &Binding) == storage::E_REPO_RESULT::Ok);
	std::string sExpectedPath;
	std::string sExpectedKey;
	REQUIRE(pynote::platform::ResolveBindingPath(utf8_path(Path), &sExpectedPath, &sExpectedKey));
	REQUIRE(Binding.sPath == sExpectedPath);
	REQUIRE(Binding.sPathKey == sExpectedKey);
	REQUIRE(Binding.sEncoding == "utf-8");
	REQUIRE_FALSE(Binding.bBom);
	REQUIRE(Binding.eNewline == domain::E_NEWLINE_KIND::Crlf);
	REQUIRE(Binding.bTrailingNewline);
	// synced_size 는 stat 의 크기가 아니라 방금 읽은 바이트 길이다(원본 document_page.py:886).
	REQUIRE(Binding.nSyncedSize == static_cast<std::int64_t>(sBytes.size()));
	REQUIRE(Binding.sSyncedHash == app::HashBytes(std::span(
		reinterpret_cast<const std::uint8_t*>(sBytes.data()), sBytes.size())));
	REQUIRE(Binding.nBoundAtUs == LAUNCH_CLOCK_US);
	REQUIRE(Binding.nSyncedAtUs == LAUNCH_CLOCK_US);
	REQUIRE(Binding.nSyncedMtimeNs.has_value());

	// 카드는 정확히 한 장이고 본문은 줄끝을 정규화한 텍스트다 - CRLF 가 LF 로 접힌다
	// (split=false, Import).
	domain::S_CARD Card;
	REQUIRE(Fixture.Repo().GetCard(sCardId, &Card) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Card.sBody == "\xEB\xB3\xB8\xEB\xAC\xB8 \xED\x95\x98\xEB\x82\x98\n");
	REQUIRE(Card.eSource == domain::E_CARD_SOURCE::Import);
}

TEST_CASE("W3-FB-202 an already bound path routes to the owning card without a new window",
	"[W3-file-binding][FS-port][WTL-CAP-FB-010]")
{
	C_LAUNCH_FIXTURE Fixture("route");
	const auto Path = Fixture.Write(L"재사용.txt", "\xEB\xB3\xB8\xEB\xAC\xB8\n");
	REQUIRE(Fixture.Open(Path));
	const std::string sCardId = Fixture.Log().Revealed.front();
	REQUIRE(Fixture.Log().nWindows == 1);

	// 두 번째 배달은 창을 만들지 않고 소유 카드로 라우팅한다(원본 app.py:663~672).
	REQUIRE(Fixture.Open(Path));

	REQUIRE(Fixture.Log().nWindows == 1);
	REQUIRE(Fixture.Log().Routed == std::vector<std::string>{ sCardId });
	REQUIRE(Fixture.Log().Revealed.size() == 1);
	REQUIRE(Fixture.Log().Dialogs.empty());
	REQUIRE(Fixture.BindingCount() == 1);
}

TEST_CASE("W3-FB-203 a missing path and a directory are reported without creating a window",
	"[W3-file-binding][FS-port]")
{
	C_LAUNCH_FIXTURE Fixture("missing");

	REQUIRE_FALSE(Fixture.Open(Fixture.Root() / L"없는파일.txt"));
	REQUIRE(Fixture.Log().nWindows == 0);
	REQUIRE(Fixture.Log().Dialogs.size() == 1);
	REQUIRE(Fixture.Log().Dialogs.front().first == L"파일 열기");
	REQUIRE(Fixture.Log().Dialogs.front().second == L"없는파일.txt: 파일을 찾을 수 없습니다.");

	const auto Directory = Fixture.Root() / L"폴더";
	std::error_code Error;
	REQUIRE(std::filesystem::create_directory(Directory, Error));
	REQUIRE_FALSE(Fixture.Open(Directory));
	REQUIRE(Fixture.Log().nWindows == 0);
	REQUIRE(Fixture.Log().Dialogs.size() == 2);
	REQUIRE(Fixture.Log().Dialogs.back().second == L"폴더: 디렉터리는 열 수 없습니다.");
	REQUIRE(Fixture.BindingCount() == 0);
}

TEST_CASE("W3-FB-204 an unbindable file only shows the notice and creates no window",
	"[W3-file-binding][FS-port][WTL-CAP-FB-016]")
{
	C_LAUNCH_FIXTURE Fixture("unbindable");
	// 제어 문자가 있으면 결속 불가다(원본 has_control_chars).
	const auto Path = Fixture.Write(L"제어.txt", std::string("\xEB\xB3\xB8\xEB\xAC\xB8\x01\xEB\x81\x9D", 10));

	REQUIRE_FALSE(Fixture.Open(Path));

	REQUIRE(Fixture.Log().nWindows == 0);
	REQUIRE(Fixture.BindingCount() == 0);
	REQUIRE(Fixture.Log().Dialogs.size() == 1);
	REQUIRE(Fixture.Log().Dialogs.front().first == L"결속할 수 없는 파일");
	REQUIRE(Fixture.Log().Dialogs.front().second ==
		L"제어.txt 파일은 텍스트로 해석되지 않아 편집 결과를 되쓸 수 없습니다.");
}

TEST_CASE("W3-FB-205 a file over the import ceiling is refused instead of silently truncated",
	"[W3-file-binding][FS-port]")
{
	C_LAUNCH_FIXTURE Fixture("toolarge");
	// 상한 + 1 로 읽지 않으면 이 파일이 앞 4 MiB 만 결속된 채 통과한다(감사 A-2).
	const auto Path = Fixture.Write(
		L"과대.txt", std::string(app::MAX_IMPORT_FILE_BYTES + 1, 'a'));

	REQUIRE_FALSE(Fixture.Open(Path));

	REQUIRE(Fixture.Log().nWindows == 0);
	REQUIRE(Fixture.BindingCount() == 0);
	REQUIRE(Fixture.Log().Dialogs.size() == 1);
	REQUIRE(Fixture.Log().Dialogs.front().first == L"파일 열기 실패");
	REQUIRE(Fixture.Log().Dialogs.front().second == L"과대.txt: 파일당 4 MiB 상한을 초과했습니다.");
}

TEST_CASE("W3-FB-206 the held-path check runs before the zero-paragraph refusal",
	"[W3-file-binding][FS-port]")
{
	C_LAUNCH_FIXTURE Fixture("zeroparagraph");
	const auto Path = Fixture.Write(L"빈문단.txt", "   \n\n \n");
	std::string sPath;
	std::string sPathKey;
	REQUIRE(pynote::platform::ResolveBindingPath(utf8_path(Path), &sPath, &sPathKey));

	// 휴지통 카드가 그 경로를 쥐고 있게 만든다 - PrepareBindingPath 만 이 행을 지운다.
	const std::string sDocumentId = Fixture.CreateDocument();
	std::vector<domain::S_CARD> Created;
	REQUIRE(Fixture.Cards().CreateCards(sDocumentId, "본문",
		domain::E_CAPTURE_OPERATION_SOURCE::Import, false, std::nullopt, &Created) ==
		app::E_CARD_SERVICE_RESULT::Ok);
	REQUIRE(Created.size() == 1);
	domain::S_FILE_BINDING Stale;
	Stale.sCardId = Created.front().sId;
	Stale.sPath = sPath;
	Stale.sPathKey = sPathKey;
	Stale.sEncoding = "utf-8";
	Stale.eNewline = domain::E_NEWLINE_KIND::Lf;
	Stale.nBoundAtUs = 1;
	REQUIRE(Fixture.Repo().UpsertFileBinding(Stale) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Created.front().sCurrentRevisionId.has_value());
	REQUIRE(Fixture.Repo().UpdateCardDeletedState(Created.front().sId,
		Created.front().nPositionKey, 12345, *Created.front().sCurrentRevisionId) ==
		storage::E_REPO_RESULT::Ok);
	REQUIRE(Fixture.BindingCount() == 1);

	REQUIRE_FALSE(Fixture.Open(Path));

	// 문단 0 으로 거절됐어도 점유 해소는 이미 일어났다 - 순서가 관측되는 자리다.
	REQUIRE(Fixture.BindingCount() == 0);
	REQUIRE(Fixture.Log().nWindows == 0);
	REQUIRE(Fixture.Log().Dialogs.size() == 1);
	REQUIRE(Fixture.Log().Dialogs.front().first == L"파일 열기");
	REQUIRE(Fixture.Log().Dialogs.front().second == L"빈 파일 결속은 편집기 이식 뒤 지원됩니다");
}
