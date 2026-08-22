#include <catch_amalgamated.hpp>

// 픽스처 임시 파일 이름의 GetCurrentProcessId 하나만 쓴다. 하네스 헤더와 같은 가드로
// 들여 min/max 매크로가 뒤따르는 core 헤더를 건드리지 않게 한다.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// windows.h 의 CreateEvent 매크로는 repositories.h 의 멤버 이름을 바꾼다 - 다른
// TU(CDocumentPage.cpp·w3_shell_consumer_test.cpp)와 같은 순서 계약이어야 같은
// 바이너리 안에서 멤버 이름이 갈리지 않는다.
#ifdef CreateEvent
#undef CreateEvent
#endif

#include "CChangeBus.h"

#include "pynote/core/application/card_service.h"
#include "pynote/core/application/window_lifecycle.h"
#include "pynote/core/domain/models.h"
#include "pynote/core/domain/paragraph_parser.h"
#include "pynote/core/storage/database.h"
#include "pynote/core/storage/migration_runner.h"
#include "pynote/core/storage/repositories.h"

#include <sqlite3/sqlite3.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#pragma comment(lib, "NoteExCore")

// 이 조각의 시험은 전부 pynote::shell seam 을 직접 구동한다. CApplication/CMain 은 시험
// 프로젝트에 없고 넣을 수도 없으므로(계약 spec "시험 가능성 설계") 주기 타이머·실패 모달·
// 재진입 가드·매핑 재계산은 여기서 다루지 않는다 - G4' 스모크와 정적 감사가 담당한다.
namespace
{
	namespace app = pynote::core::application;
	namespace domain = pynote::core::domain;
	namespace shell = pynote::shell;
	namespace storage = pynote::core::storage;

	// 실 DB 픽스처. 마이그레이션을 끝까지 돌린 실제 파일 위에서만 정책·카드 계수를 본다
	// (시험 전용 fake DB 금지 - 계약 scope boundary).
	class C_POLICY_FIXTURE
	{
	public:
		C_POLICY_FIXTURE()
			: m_Path(std::filesystem::temp_directory_path() /
				("NoteEx-W3-settings-" + std::to_string(::GetCurrentProcessId()) + "-" +
				std::to_string(++s_nSequence) + ".db")), m_Repositories(m_Database)
		{
			this->remove_();
			REQUIRE(m_Database.Open(m_Path.string()));
			storage::C_MIGRATION_RUNNER Runner;
			Runner.SetExistingDatabase(false, m_Path.string());
			REQUIRE(Runner.Run(m_Database) == storage::E_MIGRATE_RESULT::Ok);
			m_CardService = std::make_unique<app::C_CARD_SERVICE>(m_Database, m_Repositories, m_Parser,
				[this]() { return(++m_nClock); }, [this]() { return(this->next_id_("card-data")); });
		}

		~C_POLICY_FIXTURE()
		{
			m_CardService.reset();
			m_Database.Close();
			this->remove_();
		}

		std::string CreateDocument(const std::string& _sTitle)
		{
			domain::S_DOCUMENT Document;
			Document.sId = this->next_id_("document");
			Document.sTitle = _sTitle;
			Document.nCreatedAtUs = ++m_nClock;
			Document.nUpdatedAtUs = Document.nCreatedAtUs;
			REQUIRE(m_Repositories.CreateDocument(Document) == storage::E_REPO_RESULT::Ok);
			return(Document.sId);
		}

		void CreateCard(const std::string& _sDocumentId, const std::string& _sBody)
		{
			domain::S_CARD Card;
			REQUIRE(m_CardService->CreateCard(_sDocumentId, _sBody,
				domain::E_CAPTURE_OPERATION_SOURCE::Typing, std::nullopt, &Card) ==
				app::E_CARD_SERVICE_RESULT::Ok);
		}

		void MarkDocument(
			const std::string& _sDocumentId,
			std::optional<std::int64_t> _nTrashedAtUs,
			std::optional<std::int64_t> _nArchivedAtUs)
		{
			domain::S_DOCUMENT Document;
			REQUIRE(m_Repositories.GetDocument(_sDocumentId, &Document) == storage::E_REPO_RESULT::Ok);
			Document.nTrashedAtUs = _nTrashedAtUs;
			Document.nArchivedAtUs = _nArchivedAtUs;
			Document.nUpdatedAtUs = ++m_nClock;
			REQUIRE(m_Repositories.UpdateDocument(Document) == storage::E_REPO_RESULT::Ok);
		}

		void Execute(const char* _pszSql)
		{
			char* pszError = nullptr;
			const int nResult = ::sqlite3_exec(m_Database.Handle(), _pszSql, nullptr, nullptr, &pszError);
			if (pszError) { ::sqlite3_free(pszError); }
			REQUIRE(nResult == SQLITE_OK);
		}

		storage::C_DATABASE& Database() { return(m_Database); }
		storage::C_REPOSITORIES& Repositories() { return(m_Repositories); }

	private:
		std::string next_id_(const char* _pszPrefix)
		{
			return(std::string(_pszPrefix) + "-" + std::to_string(++m_nId));
		}

		void remove_() const
		{
			std::error_code Error;
			std::filesystem::remove(m_Path, Error);
			std::filesystem::remove(m_Path.string() + "-wal", Error);
			std::filesystem::remove(m_Path.string() + "-shm", Error);
		}

		std::filesystem::path m_Path;
		storage::C_DATABASE m_Database;
		storage::C_REPOSITORIES m_Repositories;
		domain::C_PARAGRAPH_PARSER m_Parser;
		std::unique_ptr<app::C_CARD_SERVICE> m_CardService;
		std::int64_t m_nClock{ 5000 };
		std::uint64_t m_nId{};
		inline static std::atomic<unsigned long> s_nSequence{};
	};

	// 창의 버스 구독자가 하는 일과 같은 조립이다 - 계수와 조립기를 한 번에 통과시킨다.
	std::wstring compose_from_repository(
		storage::C_REPOSITORIES& _Repositories, const std::string& _sDocumentId)
	{
		const auto Stats = shell::CountActiveCards(_Repositories, _sDocumentId);
		REQUIRE(Stats.has_value());
		return(shell::ComposeStatusText(Stats->nCards, Stats->nCodepoints,
			shell::ComposeSaveStateText(false, false, false)));
	}
}

TEST_CASE("PLAN-W3-0043 document creation publish composes exact empty document status",
	"[W3-settings-bus-status][PLAN-W3-0043]")
{
	C_POLICY_FIXTURE Fixture;
	const std::string sDocumentId = Fixture.CreateDocument("empty document");

	shell::C_DOCUMENT_CHANGE_BUS Bus;
	std::wstring sObserved;
	std::string sObservedDocumentId;
	const auto Token = Bus.Subscribe([&](const std::string& _sChangedId) {
		sObservedDocumentId = _sChangedId;
		sObserved = compose_from_repository(Fixture.Repositories(), _sChangedId);
	});
	REQUIRE(Token != 0);

	Bus.Publish(sDocumentId);

	REQUIRE(sObservedDocumentId == sDocumentId);
	REQUIRE(sObserved == std::wstring(L"0개 카드 · 0자 · 모든 변경 저장됨 · 로컬 DB"));
}

TEST_CASE("PLAN-W3-0045 page content publish composes exact card and character status",
	"[W3-settings-bus-status][PLAN-W3-0045][WTL-CAP-FI-036]")
{
	C_POLICY_FIXTURE Fixture;
	const std::string sDocumentId = Fixture.CreateDocument("korean document");
	// 계수 단위가 코드포인트임을 3자 구분으로 강제한다 - "한국어 본문"+astral 1자는
	// 코드포인트 7 / UTF-8 바이트 20 / UTF-16 코드유닛 8 이라, 바이트로 세도
	// 코드유닛으로 세도 이 단언이 깨진다(BMP 만으로는 코드유닛과 구별 불가).
	Fixture.CreateCard(sDocumentId,
		"\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4 \xeb\xb3\xb8\xeb\xac\xb8\xf0\x9f\x93\x9d");

	const auto Stats = shell::CountActiveCards(Fixture.Repositories(), sDocumentId);
	REQUIRE(Stats.has_value());
	REQUIRE(Stats->nCards == 1);
	REQUIRE(Stats->nCodepoints == 7);

	shell::C_DOCUMENT_CHANGE_BUS Bus;
	std::wstring sObserved;
	REQUIRE(Bus.Subscribe([&](const std::string& _sChangedId) {
		sObserved = compose_from_repository(Fixture.Repositories(), _sChangedId);
	}) != 0);

	Bus.Publish(sDocumentId);

	REQUIRE(sObserved == std::wstring(L"1개 카드 · 7자 · 모든 변경 저장됨 · 로컬 DB"));

	// 저장 상태 규칙(원본 main_window.py:723~729): 세션이 존재하고 dirty 또는 저장
	// 실패일 때만 편집기 상태 문자열이고, 세션이 없으면 조건 자체가 성립하지 않는다.
	REQUIRE(shell::ComposeSaveStateText(true, true, false) == std::wstring(L"편집 중"));
	// REQUIRE 는 식 원문을 narrow 리터럴로 stringify 하므로 CP949 밖 문자(U+2014)가 식 안에
	// 있으면 C4566 — 기대 문자열은 식 밖 상수로 둔다.
	const std::wstring sSaveFailed = L"저장 실패 — 다시 시도";
	REQUIRE(shell::ComposeSaveStateText(true, false, true) == sSaveFailed);
	REQUIRE(shell::ComposeSaveStateText(true, true, true) == sSaveFailed);
	REQUIRE(shell::ComposeSaveStateText(false, true, true) ==
		std::wstring(L"모든 변경 저장됨"));
	REQUIRE(shell::ComposeSaveStateText(true, false, false) ==
		std::wstring(L"모든 변경 저장됨"));
}

TEST_CASE("PLAN-W3-0047 empty window refill publish recomposes status from refilled document",
	"[W3-settings-bus-status][PLAN-W3-0047]")
{
	C_POLICY_FIXTURE Fixture;
	const std::string sRemovedId = Fixture.CreateDocument("removed document");
	const std::string sRefillId = Fixture.CreateDocument("refill document");
	// "가나"(2) + "다라마"(3) = 5 코드포인트.
	Fixture.CreateCard(sRefillId, "\xea\xb0\x80\xeb\x82\x98");
	Fixture.CreateCard(sRefillId, "\xeb\x8b\xa4\xeb\x9d\xbc\xeb\xa7\x88");
	Fixture.MarkDocument(sRemovedId, 9000, std::nullopt);

	// 소멸한 문서는 분리 대상이고, 재채움 대상 문서의 데이터로 상태 바를 다시 조립한다.
	REQUIRE(shell::ClassifyDocumentChange(Fixture.Repositories(), sRemovedId) ==
		std::optional(shell::E_DOCUMENT_CHANGE::RemovedSaveUi));

	shell::C_DOCUMENT_CHANGE_BUS Bus;
	std::wstring sObserved;
	REQUIRE(Bus.Subscribe([&](const std::string&) {
		sObserved = compose_from_repository(Fixture.Repositories(), sRefillId);
	}) != 0);

	Bus.Publish(sRemovedId);

	REQUIRE(sObserved == std::wstring(L"2개 카드 · 5자 · 모든 변경 저장됨 · 로컬 DB"));
}

TEST_CASE("PLAN-W3-0042 closed window unsubscribes and failing subscriber does not stop publish",
	"[W3-settings-bus-status][PLAN-W3-0042][WTL-CAP-PL-012]")
{
	shell::C_DOCUMENT_CHANGE_BUS Bus;
	std::vector<std::string> Calls;
	std::vector<std::string> Reported;
	Bus.SetErrorSink([&](const std::string& _sMessage) { Reported.push_back(_sMessage); });

	REQUIRE(Bus.Subscribe([&](const std::string&) { Calls.push_back("first"); }) != 0);
	REQUIRE(Bus.Subscribe([&](const std::string&) {
		Calls.push_back("throwing");
		throw std::runtime_error("subscriber failed");
	}) != 0);
	REQUIRE(Bus.Subscribe([&](const std::string&) { Calls.push_back("third"); }) != 0);
	const auto ClosedToken = Bus.Subscribe([&](const std::string&) { Calls.push_back("closed"); });
	REQUIRE(ClosedToken != 0);
	REQUIRE(Bus.SubscriberCount() == 4);

	// 창은 파괴 전에 구독을 해제한다(PLAN-W3-0042).
	REQUIRE(Bus.Unsubscribe(ClosedToken));
	REQUIRE_FALSE(Bus.Unsubscribe(ClosedToken));
	REQUIRE(Bus.SubscriberCount() == 3);

	Bus.Publish("document-1");

	REQUIRE(Calls == std::vector<std::string>{ "first", "throwing", "third" });
	REQUIRE(Reported.size() == 1);
	REQUIRE(Reported[0].find("document-1") != std::string::npos);

	// 예외 구독자는 살아 있고 다음 발행에서도 뒤 구독자를 막지 않는다.
	Calls.clear();
	Bus.Publish("document-2");
	REQUIRE(Calls == std::vector<std::string>{ "first", "throwing", "third" });
}

TEST_CASE("CAP-FI-038 owned document open resolves to activation not a new window",
	"[W3-settings-bus-status][WTL-CAP-FI-038]")
{
	app::C_WINDOW_REGISTRY Registry;
	const auto OwnerToken = Registry.Register("workspace-owner", std::string("document-owned"));
	const auto RequestingToken = Registry.Register("workspace-requesting", std::string("document-other"));
	REQUIRE(OwnerToken != 0);
	REQUIRE(RequestingToken != 0);
	REQUIRE(Registry.DocumentOwner("document-owned") == std::optional(OwnerToken));

	// 등록은 첫 창만 활성으로 만든다 - 요청 창이 활성인 상태에서 시작한다.
	REQUIRE(Registry.ActiveWindow() == std::optional(OwnerToken));
	REQUIRE(Registry.Activate(RequestingToken));
	REQUIRE(Registry.ActiveWindow() == std::optional(RequestingToken));

	// 다른 창이 소유한 문서 - 새 창이 아니라 소유 창 활성화다.
	REQUIRE(shell::ResolveOpenDocumentTarget(
		Registry.DocumentOwner("document-owned"), RequestingToken) ==
		shell::E_OPEN_DOCUMENT_TARGET::ActivateOwner);
	// 요청 창이 이미 소유한 문서, 그리고 아무도 소유하지 않은 문서 - 요청 창에서 연다.
	REQUIRE(shell::ResolveOpenDocumentTarget(
		Registry.DocumentOwner("document-other"), RequestingToken) ==
		shell::E_OPEN_DOCUMENT_TARGET::OpenInRequesting);
	REQUIRE(shell::ResolveOpenDocumentTarget(std::nullopt, RequestingToken) ==
		shell::E_OPEN_DOCUMENT_TARGET::OpenInRequesting);

	// 관측 지점은 registry 의 활성 토큰이다 - 4 단 native 활성화 자체는 셸 배선이라
	// in-process 로 결정적으로 볼 수 없다(G4' 스모크·정적 감사 소관).
	REQUIRE(Registry.Activate(OwnerToken));
	REQUIRE(Registry.ActiveWindow() == std::optional(OwnerToken));
	REQUIRE(Registry.Size() == 2);
}

TEST_CASE("CAP-PL-005 storage policy loads typed fields after migration and rejects invalid rows",
	"[W3-settings-bus-status][WTL-CAP-PL-005]")
{
	C_POLICY_FIXTURE Fixture;
	{
		const auto Policy = shell::LoadDataPolicy(Fixture.Database());
		REQUIRE(Policy.has_value());
		REQUIRE(Policy->nDraftIdleMs == 2000);
		REQUIRE(Policy->sSplitPolicy == "keep");
		REQUIRE(Policy->nPreviewLines == 3);
		REQUIRE(Policy->dBackupIntervalHours == 24.0);
		REQUIRE(Policy->nTrashRetentionDays == 30);
	}

	// REAL 열의 0.5 가 double 로 살아남아야 한다. column_int64 로 접으면 0 이 되고
	// 유지보수 주기가 통째로 사라진다.
	Fixture.Execute("UPDATE data_policy_settings SET backup_interval_hours = 0.5 WHERE id = 1");
	{
		const auto Policy = shell::LoadDataPolicy(Fixture.Database());
		REQUIRE(Policy.has_value());
		REQUIRE(Policy->dBackupIntervalHours == 0.5);
		REQUIRE(shell::ClampMaintenanceIntervalMs(Policy->dBackupIntervalHours) == 1800000);
	}
	REQUIRE(shell::ClampMaintenanceIntervalMs(24.0) == 86400000);
	REQUIRE(shell::ClampMaintenanceIntervalMs(1000000.0) == 2147483647);

	// 행 부재 - 원본 AppContext.open 이 기동을 닫는 자리다.
	Fixture.Execute("DELETE FROM data_policy_settings");
	REQUIRE_FALSE(shell::LoadDataPolicy(Fixture.Database()).has_value());

	// CHECK 없는 표를 들고 있는 손상·구형 DB. 적재기가 다시 걸러야 한다.
	Fixture.Execute(
		"DROP TABLE data_policy_settings;"
		"CREATE TABLE data_policy_settings ("
		"  id INTEGER PRIMARY KEY, draft_idle_ms INTEGER NOT NULL,"
		"  split_policy TEXT NOT NULL, preview_lines INTEGER NOT NULL,"
		"  backup_interval_hours REAL NOT NULL, trash_retention_days INTEGER NOT NULL,"
		"  updated_at_us INTEGER NOT NULL);"
		"INSERT INTO data_policy_settings VALUES (1, 2000, 'keep', 3, 0, 30, 1);");
	REQUIRE_FALSE(shell::LoadDataPolicy(Fixture.Database()).has_value());

	Fixture.Execute("UPDATE data_policy_settings SET backup_interval_hours = 6,"
		" split_policy = 'unsupported' WHERE id = 1");
	REQUIRE_FALSE(shell::LoadDataPolicy(Fixture.Database()).has_value());

	Fixture.Execute("UPDATE data_policy_settings SET split_policy = 'keep',"
		" preview_lines = 0 WHERE id = 1");
	REQUIRE_FALSE(shell::LoadDataPolicy(Fixture.Database()).has_value());

	Fixture.Execute("UPDATE data_policy_settings SET preview_lines = 3 WHERE id = 1");
	REQUIRE(shell::LoadDataPolicy(Fixture.Database()).has_value());
}

TEST_CASE("CAP-PL-009 external document removal classifies save and no-save branches",
	"[W3-settings-bus-status][WTL-CAP-PL-009]")
{
	C_POLICY_FIXTURE Fixture;
	const std::string sAliveId = Fixture.CreateDocument("alive");
	const std::string sTrashedId = Fixture.CreateDocument("trashed");
	const std::string sArchivedId = Fixture.CreateDocument("archived");
	Fixture.MarkDocument(sTrashedId, 7000, std::nullopt);
	Fixture.MarkDocument(sArchivedId, std::nullopt, 7100);

	REQUIRE(shell::ClassifyDocumentChange(Fixture.Repositories(), sAliveId) ==
		std::optional(shell::E_DOCUMENT_CHANGE::Alive));
	// 행은 살아 있으므로 UI 상태를 저장한 뒤 분리한다(save_ui_state=document is not None).
	REQUIRE(shell::ClassifyDocumentChange(Fixture.Repositories(), sTrashedId) ==
		std::optional(shell::E_DOCUMENT_CHANGE::RemovedSaveUi));
	REQUIRE(shell::ClassifyDocumentChange(Fixture.Repositories(), sArchivedId) ==
		std::optional(shell::E_DOCUMENT_CHANGE::RemovedSaveUi));
	// 행 자체가 없으면 저장할 대상이 없다.
	REQUIRE(shell::ClassifyDocumentChange(Fixture.Repositories(), "document-absent") ==
		std::optional(shell::E_DOCUMENT_CHANGE::RemovedNoSave));
}

TEST_CASE("CAP-FI-015 window title composes document title with application name",
	"[W3-settings-bus-status][WTL-CAP-FI-015]")
{
	// 기대 문자열을 식 밖에 두는 이유는 저장 상태 블록과 같다(REQUIRE stringify + U+2014).
	const std::wstring sMeetingTitle = L"회의록 — pyNote";
	const std::wstring sNoteTitle = L"Note 1 — pyNote";
	REQUIRE(shell::ComposeWindowTitle(std::optional<std::wstring>(L"회의록")) == sMeetingTitle);
	REQUIRE(shell::ComposeWindowTitle(std::optional<std::wstring>(L"Note 1")) == sNoteTitle);
	REQUIRE(shell::ComposeWindowTitle(std::nullopt) == std::wstring(L"pyNote"));
	// 활성 문서가 없는 창의 상태 바 문안도 같은 자리에서 굳힌다(원본 :714).
	REQUIRE(shell::ComposeEmptyStatusText() ==
		std::wstring(L"문서를 선택하거나 새 문서를 만드세요."));
}
