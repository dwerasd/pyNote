#include <catch_amalgamated.hpp>

#include "pynote/core/storage/migration_runner.h"
#include "pynote/core/storage/repositories.h"

#include <sqlite3/sqlite3.h>

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#pragma comment(lib, "NoteExCore")

namespace
{
	namespace domain = pynote::core::domain;
	namespace storage = pynote::core::storage;

	// 시험용 임시 데이터베이스 경로. 소멸 시 본체와 WAL/SHM 사이드카까지 지운다.
	class C_TEMP_DB
	{
	public:
		explicit C_TEMP_DB(const std::string& _sName)
		{
			m_Path = std::filesystem::temp_directory_path() / ("noteex_test_" + _sName + ".db");
			this->remove_all_();
		}

		~C_TEMP_DB()
		{
			this->remove_all_();
		}

		C_TEMP_DB(const C_TEMP_DB&) = delete;
		C_TEMP_DB& operator=(const C_TEMP_DB&) = delete;

		std::string Utf8() const { return(m_Path.string()); }

	private:
		void remove_all_()
		{
			std::error_code ec;
			std::filesystem::remove(m_Path, ec);
			std::filesystem::remove(m_Path.string() + "-wal", ec);
			std::filesystem::remove(m_Path.string() + "-shm", ec);
		}

		std::filesystem::path m_Path;
	};

	// 최신 스키마까지 올린 데이터베이스와 저장소 한 벌이다.
	class C_BINDING_FIXTURE
	{
	public:
		explicit C_BINDING_FIXTURE(const std::string& _sName)
			: m_Temp(_sName)
			, m_Repositories(m_Database)
		{
			REQUIRE(m_Database.Open(m_Temp.Utf8()));

			storage::C_MIGRATION_RUNNER Runner;
			Runner.SetExistingDatabase(false, m_Temp.Utf8());
			REQUIRE(Runner.Run(m_Database) == storage::E_MIGRATE_RESULT::Ok);

			domain::S_DOCUMENT Document;
			Document.sId          = "document-1";
			Document.sTitle       = "T";
			Document.nCreatedAtUs = 1;
			Document.nUpdatedAtUs = 1;
			REQUIRE(m_Repositories.CreateDocument(Document) == storage::E_REPO_RESULT::Ok);
		}

		storage::C_DATABASE&     Db() { return(m_Database); }
		storage::C_REPOSITORIES& Repo() { return(m_Repositories); }

		// 카드 한 장을 원본 create_cards 와 같은 경로로 만든다.
		domain::S_CARD MakeCard(int _nNumber)
		{
			domain::S_NEW_CAPTURE_OPERATION Operation;
			Operation.sId          = "operation-" + std::to_string(_nNumber);
			Operation.sDocumentId  = "document-1";
			Operation.eSource      = domain::E_CAPTURE_OPERATION_SOURCE::Import;
			Operation.eSplitPolicy = domain::E_SPLIT_POLICY::Keep;
			Operation.nCreatedAtUs = 10;

			domain::S_NEW_CARD New;
			New.sId             = "card-" + std::to_string(_nNumber);
			New.sRevisionId     = "revision-" + std::to_string(_nNumber);
			New.sEventId        = "event-" + std::to_string(_nNumber);
			New.nPositionKey    = _nNumber * 1024;
			New.sBody           = "b" + std::to_string(_nNumber);
			New.eCardSource     = domain::E_CARD_SOURCE::Import;
			New.eEventSource    = domain::E_EVENT_SOURCE::Import;
			New.eRevisionSource = domain::E_REVISION_SOURCE::Edit;
			New.nCreatedAtUs    = 10;

			std::vector<domain::S_CARD> Created;
			REQUIRE(m_Repositories.CreateCards(Operation, { New }, &Created) == storage::E_REPO_RESULT::Ok);
			REQUIRE(Created.size() == 1);
			return(Created.front());
		}

		void SoftDelete(const domain::S_CARD& _Card)
		{
			REQUIRE(_Card.sCurrentRevisionId.has_value());
			REQUIRE(m_Repositories.UpdateCardDeletedState(
				_Card.sId, _Card.nPositionKey, 12345, *_Card.sCurrentRevisionId)
				== storage::E_REPO_RESULT::Ok);
		}

		int BindingCount()
		{
			sqlite3_stmt* pStmt = nullptr;
			REQUIRE(::sqlite3_prepare_v2(
				m_Database.Handle(), "SELECT COUNT(*) FROM card_file_bindings", -1, &pStmt, nullptr) == SQLITE_OK);
			REQUIRE(::sqlite3_step(pStmt) == SQLITE_ROW);
			const int nValue = ::sqlite3_column_int(pStmt, 0);
			::sqlite3_finalize(pStmt);
			return(nValue);
		}

	private:
		C_TEMP_DB               m_Temp;
		storage::C_DATABASE     m_Database;
		storage::C_REPOSITORIES m_Repositories;
	};

	domain::S_FILE_BINDING make_binding(const std::string& _sCardId, const std::string& _sPath)
	{
		domain::S_FILE_BINDING Binding;
		Binding.sCardId          = _sCardId;
		Binding.sPath            = _sPath;
		Binding.sPathKey         = _sPath;
		Binding.sEncoding        = "utf-8";
		Binding.bBom             = false;
		Binding.eNewline         = domain::E_NEWLINE_KIND::Lf;
		Binding.bTrailingNewline = true;
		Binding.nBoundAtUs       = 1000;
		return(Binding);
	}
}

TEST_CASE("결속 행은 열두 열이 값 그대로 돌아오고 카드 충돌은 대체다", "[W1-file-binding][FS-port][WTL-CAP-FB-020]")
{
	C_BINDING_FIXTURE    Fixture("fb_repo_roundtrip");
	const domain::S_CARD Card = Fixture.MakeCard(1);

	domain::S_FILE_BINDING Binding;
	Binding.sCardId          = Card.sId;
	Binding.sPath            = "C:\\Notes\\A.txt";
	Binding.sPathKey         = "c:\\notes\\a.txt";
	Binding.sEncoding        = "utf-16-be";
	Binding.bBom             = true;
	Binding.eNewline         = domain::E_NEWLINE_KIND::Crlf;
	Binding.bTrailingNewline = false;
	Binding.nBoundAtUs       = 1000;
	Binding.nSyncedSize      = 12;
	Binding.nSyncedMtimeNs   = 34;
	Binding.sSyncedHash      = std::string("abc");
	Binding.nSyncedAtUs      = 2000;

	REQUIRE(Fixture.Repo().UpsertFileBinding(Binding) == storage::E_REPO_RESULT::Ok);

	domain::S_FILE_BINDING Loaded;
	REQUIRE(Fixture.Repo().GetFileBinding(Card.sId, &Loaded) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Loaded == Binding);

	domain::S_FILE_BINDING Moved = Binding;
	Moved.sPath    = "C:\\Notes\\B.txt";
	Moved.sPathKey = "c:\\notes\\b.txt";
	REQUIRE(Fixture.Repo().UpsertFileBinding(Moved) == storage::E_REPO_RESULT::Ok);

	domain::S_FILE_BINDING Reloaded;
	REQUIRE(Fixture.Repo().GetFileBinding(Card.sId, &Reloaded) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Reloaded == Moved);

	domain::S_FILE_BINDING Gone;
	REQUIRE(Fixture.Repo().FindBindingByPath("c:\\notes\\a.txt", &Gone) == storage::E_REPO_RESULT::NotFound);
	REQUIRE(Fixture.BindingCount() == 1);
}

TEST_CASE("결속의 NULL 열 넷은 값 없음으로 돌아온다", "[W1-file-binding][FS-port][WTL-CAP-FB-020]")
{
	C_BINDING_FIXTURE    Fixture("fb_repo_nulls");
	const domain::S_CARD Card = Fixture.MakeCard(1);

	const domain::S_FILE_BINDING Binding = make_binding(Card.sId, "C:\\Notes\\A.txt");
	REQUIRE(Fixture.Repo().UpsertFileBinding(Binding) == storage::E_REPO_RESULT::Ok);

	domain::S_FILE_BINDING Loaded;
	REQUIRE(Fixture.Repo().GetFileBinding(Card.sId, &Loaded) == storage::E_REPO_RESULT::Ok);
	REQUIRE_FALSE(Loaded.nSyncedSize.has_value());
	REQUIRE_FALSE(Loaded.nSyncedMtimeNs.has_value());
	REQUIRE_FALSE(Loaded.sSyncedHash.has_value());
	REQUIRE_FALSE(Loaded.nSyncedAtUs.has_value());
	REQUIRE(Loaded == Binding);
}

TEST_CASE("결속의 path_key 는 카드가 달라도 유일해 두 번째 결속이 실패한다", "[W1-file-binding][FS-port]")
{
	C_BINDING_FIXTURE    Fixture("fb_repo_unique");
	const domain::S_CARD First  = Fixture.MakeCard(1);
	const domain::S_CARD Second = Fixture.MakeCard(2);

	REQUIRE(Fixture.Repo().UpsertFileBinding(make_binding(First.sId, "C:\\Notes\\A.txt"))
		== storage::E_REPO_RESULT::Ok);
	REQUIRE(Fixture.Repo().UpsertFileBinding(make_binding(Second.sId, "C:\\Notes\\A.txt"))
		== storage::E_REPO_RESULT::Failed);
	REQUIRE(Fixture.BindingCount() == 1);
}

TEST_CASE("휴지통 카드의 결속은 남지만 활성 조회에서 빠진다", "[W1-file-binding][FS-port][WTL-CAP-FB-020]")
{
	C_BINDING_FIXTURE    Fixture("fb_repo_trashed");
	const domain::S_CARD Card = Fixture.MakeCard(1);
	REQUIRE(Fixture.Repo().UpsertFileBinding(make_binding(Card.sId, "C:\\Notes\\A.txt"))
		== storage::E_REPO_RESULT::Ok);

	domain::S_FILE_BINDING Active;
	REQUIRE(Fixture.Repo().FindActiveBindingByPath("C:\\Notes\\A.txt", &Active) == storage::E_REPO_RESULT::Ok);

	Fixture.SoftDelete(Card);

	REQUIRE(Fixture.Repo().FindActiveBindingByPath("C:\\Notes\\A.txt", &Active)
		== storage::E_REPO_RESULT::NotFound);
	domain::S_FILE_BINDING Any;
	REQUIRE(Fixture.Repo().FindBindingByPath("C:\\Notes\\A.txt", &Any) == storage::E_REPO_RESULT::Ok);
	domain::S_FILE_BINDING ByCard;
	REQUIRE(Fixture.Repo().GetFileBinding(Card.sId, &ByCard) == storage::E_REPO_RESULT::Ok);
}

TEST_CASE("DeleteFileBinding 은 지목한 카드의 행만 지운다", "[W1-file-binding][FS-port][WTL-CAP-FB-020]")
{
	C_BINDING_FIXTURE    Fixture("fb_repo_delete");
	const domain::S_CARD First  = Fixture.MakeCard(1);
	const domain::S_CARD Second = Fixture.MakeCard(2);
	REQUIRE(Fixture.Repo().UpsertFileBinding(make_binding(First.sId, "C:\\Notes\\A.txt"))
		== storage::E_REPO_RESULT::Ok);
	REQUIRE(Fixture.Repo().UpsertFileBinding(make_binding(Second.sId, "C:\\Notes\\B.txt"))
		== storage::E_REPO_RESULT::Ok);

	REQUIRE(Fixture.Repo().DeleteFileBinding(First.sId) == storage::E_REPO_RESULT::Ok);

	domain::S_FILE_BINDING Loaded;
	REQUIRE(Fixture.Repo().GetFileBinding(First.sId, &Loaded) == storage::E_REPO_RESULT::NotFound);
	REQUIRE(Fixture.Repo().GetFileBinding(Second.sId, &Loaded) == storage::E_REPO_RESULT::Ok);
	REQUIRE(Fixture.BindingCount() == 1);
}

TEST_CASE("결속이 없는 카드 조회는 NotFound 다", "[W1-file-binding][FS-port]")
{
	C_BINDING_FIXTURE Fixture("fb_repo_absent");

	domain::S_FILE_BINDING Loaded;
	REQUIRE(Fixture.Repo().GetFileBinding("card-none", &Loaded) == storage::E_REPO_RESULT::NotFound);
	REQUIRE(Fixture.Repo().FindBindingByPath("c:\\none", &Loaded) == storage::E_REPO_RESULT::NotFound);
	REQUIRE(Fixture.Repo().FindActiveBindingByPath("c:\\none", &Loaded) == storage::E_REPO_RESULT::NotFound);
	REQUIRE(Fixture.Db().IsOpen());
}
