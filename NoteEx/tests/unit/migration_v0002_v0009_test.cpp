#include <catch_amalgamated.hpp>

#include "pynote/core/storage/migration_runner.h"
#include "pynote/core/storage/migrations/registry.h"
#include "pynote/core/storage/migrations/v0002_data_policy_settings.h"
#include "pynote/core/storage/migrations/v0003_storage_invariants.h"
#include "pynote/core/storage/migrations/v0004_workspace_windows.h"
#include "pynote/core/storage/migrations/v0005_recency_sort_mode.h"
#include "pynote/core/storage/migrations/v0006_editor_split_sizes.h"
#include "pynote/core/storage/migrations/v0007_vertical_split_reset.h"
#include "pynote/core/storage/migrations/v0008_horizontal_split_reset.h"
#include "pynote/core/storage/migrations/v0009_preview_lines_default.h"

#include <sqlite3/sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#pragma comment(lib, "NoteExCore")

namespace
{
	// 시험용 임시 데이터베이스 경로. 소멸 시 본체와 WAL/SHM 사이드카까지 지운다.
	// 사이드카를 남기면 다음 시험이 이전 상태를 물려받아 통과 여부가 순서에 의존하게 된다.
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

	// 원본이 applied_at_us 로 넘기는 값을 시험에서는 고정한다. 시계를 읽으면 저장된 값을
	// 그대로 대조할 수 없다.
	constexpr std::int64_t APPLIED_AT_US = 1700000000000000LL;

	int scalar_int(pynote::core::storage::C_DATABASE& _database, const std::string& _sSql)
	{
		sqlite3_stmt* pStmt = nullptr;
		REQUIRE(::sqlite3_prepare_v2(_database.Handle(), _sSql.c_str(), -1, &pStmt, nullptr) == SQLITE_OK);
		REQUIRE(::sqlite3_step(pStmt) == SQLITE_ROW);
		const int nValue = ::sqlite3_column_int(pStmt, 0);
		::sqlite3_finalize(pStmt);
		return(nValue);
	}

	std::int64_t scalar_int64(pynote::core::storage::C_DATABASE& _database, const std::string& _sSql)
	{
		sqlite3_stmt* pStmt = nullptr;
		REQUIRE(::sqlite3_prepare_v2(_database.Handle(), _sSql.c_str(), -1, &pStmt, nullptr) == SQLITE_OK);
		REQUIRE(::sqlite3_step(pStmt) == SQLITE_ROW);
		const std::int64_t nValue = ::sqlite3_column_int64(pStmt, 0);
		::sqlite3_finalize(pStmt);
		return(nValue);
	}

	double scalar_double(pynote::core::storage::C_DATABASE& _database, const std::string& _sSql)
	{
		sqlite3_stmt* pStmt = nullptr;
		REQUIRE(::sqlite3_prepare_v2(_database.Handle(), _sSql.c_str(), -1, &pStmt, nullptr) == SQLITE_OK);
		REQUIRE(::sqlite3_step(pStmt) == SQLITE_ROW);
		const double dValue = ::sqlite3_column_double(pStmt, 0);
		::sqlite3_finalize(pStmt);
		return(dValue);
	}

	// NULL 열은 빈 문자열로 돌아온다. NULL 여부를 봐야 하면 COUNT 와 IS NULL 로 따로 묻는다.
	std::string scalar_text(pynote::core::storage::C_DATABASE& _database, const std::string& _sSql)
	{
		sqlite3_stmt* pStmt = nullptr;
		REQUIRE(::sqlite3_prepare_v2(_database.Handle(), _sSql.c_str(), -1, &pStmt, nullptr) == SQLITE_OK);
		REQUIRE(::sqlite3_step(pStmt) == SQLITE_ROW);
		const unsigned char* pText = ::sqlite3_column_text(pStmt, 0);
		const std::string sValue(pText ? reinterpret_cast<const char*>(pText) : "");
		::sqlite3_finalize(pStmt);
		return(sValue);
	}

	void exec(pynote::core::storage::C_DATABASE& _database, const std::string& _sSql)
	{
		INFO(_sSql);
		REQUIRE(_database.Execute(_sSql));
	}

	bool has_table(pynote::core::storage::C_DATABASE& _database, const std::string& _sName)
	{
		return(scalar_int(_database,
			"SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = '" + _sName + "'") == 1);
	}

	// PRAGMA table_info 의 notnull 열(인덱스 3)이 0 이면 nullable 이다.
	bool column_is_nullable(
		pynote::core::storage::C_DATABASE& _database, const std::string& _sTable, const std::string& _sColumn)
	{
		const std::string sSql = "PRAGMA table_info(" + _sTable + ")";
		sqlite3_stmt* pStmt = nullptr;
		REQUIRE(::sqlite3_prepare_v2(_database.Handle(), sSql.c_str(), -1, &pStmt, nullptr) == SQLITE_OK);

		bool bFound    = false;
		bool bNullable = false;
		while (::sqlite3_step(pStmt) == SQLITE_ROW)
		{
			const unsigned char* pName = ::sqlite3_column_text(pStmt, 1);
			if (pName != nullptr && _sColumn == reinterpret_cast<const char*>(pName))
			{
				bFound    = true;
				bNullable = (::sqlite3_column_int(pStmt, 3) == 0);
			}
		}
		::sqlite3_finalize(pStmt);

		INFO(_sTable << "." << _sColumn);
		REQUIRE(bFound);
		return(bNullable);
	}

	// 등록 목록의 앞 _nVersion 본만 적용해 정확히 그 버전의 데이터베이스를 만든다.
	// 인자 없는 Run 은 LATEST 까지 올리므로 중간 버전을 만드는 데 쓸 수 없다.
	void migrate_to(pynote::core::storage::C_DATABASE& _database, int _nVersion)
	{
		pynote::core::storage::C_MIGRATION_RUNNER runner;
		runner.SetExistingDatabase(false, "");

		INFO("목표 버전 " << _nVersion);
		REQUIRE(runner.Run(
			_database,
			pynote::core::storage::migrations::Registry().first(static_cast<std::size_t>(_nVersion)),
			_nVersion) == pynote::core::storage::E_MIGRATE_RESULT::Ok);
		REQUIRE(scalar_int(_database, "SELECT version FROM schema_version WHERE id = 1") == _nVersion);
	}

	// v0003 트리거가 감시하는 카드/리비전 그래프의 정상 상태다. 두 카드가 각자 자기 리비전을
	// 현재 리비전으로 가리키고 본문과 해시가 일치하므로 어느 트리거도 걸리지 않는다.
	// 문자열은 전부 ASCII 다 - narrow 리터럴이 이 기계에서 CP949 로 변환되므로 시험 데이터에
	// 한국어를 넣으면 무엇을 재는 시험인지가 흐려진다.
	void seed_card_graph(pynote::core::storage::C_DATABASE& _database)
	{
		exec(_database,
			"INSERT INTO documents(id, title, created_at_us, updated_at_us)"
			" VALUES ('doc-1', 'document', 1, 1)");
		exec(_database,
			"INSERT INTO capture_operations(id, document_id, source, split_policy, created_at_us)"
			" VALUES ('op-1', 'doc-1', 'typing', 'keep', 1)");
		exec(_database,
			"INSERT INTO edit_events(event_id, operation_id, document_id, event_type, source,"
			" occurred_at_us, details_json)"
			" VALUES ('event-1', 'op-1', 'doc-1', 'create', 'typing', 1, '{}')");
		exec(_database,
			"INSERT INTO edit_events(event_id, operation_id, document_id, event_type, source,"
			" occurred_at_us, details_json)"
			" VALUES ('event-2', 'op-1', 'doc-1', 'create', 'typing', 2, '{}')");
		exec(_database,
			"INSERT INTO cards(id, document_id, operation_id, position_key, capture_seq,"
			" created_at_us, updated_at_us, source, body, body_hash)"
			" VALUES ('card-1', 'doc-1', 'op-1', 1, 1, 1, 1, 'typing', 'body-1', 'hash-1')");
		exec(_database,
			"INSERT INTO cards(id, document_id, operation_id, position_key, capture_seq,"
			" created_at_us, updated_at_us, source, body, body_hash)"
			" VALUES ('card-2', 'doc-1', 'op-1', 2, 2, 1, 1, 'typing', 'body-2', 'hash-2')");
		exec(_database,
			"INSERT INTO card_revisions(id, card_id, event_seq, body, body_hash, source, created_at_us)"
			" VALUES ('rev-1', 'card-1', 1, 'body-1', 'hash-1', 'edit', 1)");
		exec(_database,
			"INSERT INTO card_revisions(id, card_id, event_seq, body, body_hash, source, created_at_us)"
			" VALUES ('rev-2', 'card-2', 2, 'body-2', 'hash-2', 'edit', 1)");
		exec(_database, "UPDATE cards SET current_revision_id = 'rev-1' WHERE id = 'card-1'");
		exec(_database, "UPDATE cards SET current_revision_id = 'rev-2' WHERE id = 'card-2'");
	}

	// 문서 한 건과 그 문서의 UI 상태 한 행을 만든다. v0005~v0008 이 다루는 표본이다.
	void seed_ui_state(
		pynote::core::storage::C_DATABASE& _database, const std::string& _sDocumentId,
		int _nScroll, const std::string& _sSortMode)
	{
		exec(_database,
			"INSERT INTO documents(id, title, created_at_us, updated_at_us)"
			" VALUES ('" + _sDocumentId + "', 'document', 1, 1)");
		exec(_database,
			"INSERT INTO document_ui_states(document_id, list_scroll_position, sort_mode, updated_at_us)"
			" VALUES ('" + _sDocumentId + "', " + std::to_string(_nScroll) + ", '" + _sSortMode + "', 2)");
	}

	// v0003 첫 트리거의 RAISE 문구 '카드와 현재 리비전이 일치하지 않습니다' 를 두 인코딩의
	// 바이트로 박아 둔다. 리터럴 인코딩에 의존하지 않는 형태여야 인코딩 회귀를 실제로 가른다.
	const std::string UTF8_MISMATCH_MESSAGE =
		"\xEC\xB9\xB4\xEB\x93\x9C\xEC\x99\x80 \xED\x98\x84\xEC\x9E\xAC \xEB\xA6\xAC\xEB\xB9\x84"
		"\xEC\xA0\x84\xEC\x9D\xB4 \xEC\x9D\xBC\xEC\xB9\x98\xED\x95\x98\xEC\xA7\x80 \xEC\x95\x8A"
		"\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4";
	const std::string CP949_MISMATCH_MESSAGE =
		"\xC4\xAB\xB5\xE5\xBF\xCD \xC7\xF6\xC0\xE7 \xB8\xAE\xBA\xF1\xC0\xFC\xC0\xCC \xC0\xCF\xC4"
		"\xA1\xC7\xCF\xC1\xF6 \xBE\xCA\xBD\xC0\xB4\xCF\xB4\xD9";

	const char* const TRIGGER_NAMES[] = {
		"cards_current_revision_insert",
		"cards_current_revision_update",
		"card_revisions_parent_insert",
		"card_revisions_parent_update",
		"card_revisions_current_update",
		"capture_counter_no_decrease",
		"capture_counter_no_rename",
		"capture_counter_no_delete"
	};
}

// 대응 원본: tests/integration/test_database.py::test_v1_database_fixture_migrates_data_policy_after_backup_hook
// (원본 시험은 v9 까지 올린 뒤를 보므로 preview_lines 가 3 이다. 여기서는 v0002 직후를 본다.)
TEST_CASE("v0002 는 data_policy_settings 에 기본값 한 행을 시드한다", "[core][storage][migration]")
{
	const C_TEMP_DB temp("v0002_seed");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));
	migrate_to(db, 1);

	REQUIRE(pynote::core::storage::migrations::v0002::Migrate(db, APPLIED_AT_US));

	REQUIRE(has_table(db, "data_policy_settings"));
	REQUIRE(scalar_int(db, "SELECT COUNT(*) FROM data_policy_settings") == 1);
	REQUIRE(scalar_int(db, "SELECT id FROM data_policy_settings") == 1);
	REQUIRE(scalar_int(db, "SELECT draft_idle_ms FROM data_policy_settings") == 2000);
	REQUIRE(scalar_text(db, "SELECT split_policy FROM data_policy_settings") == "keep");
	// v0009 가 3 으로 정규화하기 전의 값이다. 여기서 6 이 아니면 v0009 의 조건절이 무의미해진다.
	REQUIRE(scalar_int(db, "SELECT preview_lines FROM data_policy_settings") == 6);
	REQUIRE(scalar_double(db, "SELECT backup_interval_hours FROM data_policy_settings") == 24.0);
	REQUIRE(scalar_int(db, "SELECT trash_retention_days FROM data_policy_settings") == 30);
	REQUIRE(scalar_int64(db, "SELECT updated_at_us FROM data_policy_settings") == APPLIED_AT_US);

	REQUIRE(scalar_int(db, "SELECT version FROM schema_version WHERE id = 1") == 2);
	REQUIRE(scalar_int64(db, "SELECT applied_at_us FROM schema_version WHERE id = 1") == APPLIED_AT_US);
}

// 대응 원본: v0003_storage_invariants.py:6~41 (_validate_existing_rows) 의 첫 조회.
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("v0003 은 카드와 현재 리비전이 어긋난 데이터에서 진입을 거부한다", "[core][storage][migration]")
{
	const C_TEMP_DB temp("v0003_reject_current");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));
	migrate_to(db, 2);
	seed_card_graph(db);

	// 트리거가 아직 없으므로 이 손상은 그대로 들어간다. v0003 이 막아야 하는 상태다.
	exec(db, "UPDATE cards SET body = 'body-1-changed' WHERE id = 'card-1'");

	REQUIRE_FALSE(pynote::core::storage::migrations::v0003::Migrate(db, APPLIED_AT_US));

	// DDL 앞에서 멈췄으므로 트리거도 버전도 그대로여야 한다.
	REQUIRE(scalar_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'trigger'") == 0);
	REQUIRE(scalar_int(db, "SELECT version FROM schema_version WHERE id = 1") == 2);

	// 원본은 위반 행 id 를 담은 RuntimeError 를 올린다(v0003_storage_invariants.py:24~26).
	// 실패 사실만 남기고 사유를 잃으면 손상된 데이터베이스를 만난 사용자가 이유를 못 받는다.
	REQUIRE(db.LastError().find("카드와 현재 리비전 불변조건이 손상되어 있습니다") != std::string::npos);
	REQUIRE(db.LastError().find("card-1") != std::string::npos);
}

// 대응 원본: v0003_storage_invariants.py:6~41 (_validate_existing_rows) 의 둘째 조회.
// 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("v0003 은 부모 리비전이 다른 카드에 속한 데이터에서 진입을 거부한다", "[core][storage][migration]")
{
	const C_TEMP_DB temp("v0003_reject_parent");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));
	migrate_to(db, 2);
	seed_card_graph(db);

	exec(db, "UPDATE card_revisions SET parent_revision_id = 'rev-1' WHERE id = 'rev-2'");

	REQUIRE_FALSE(pynote::core::storage::migrations::v0003::Migrate(db, APPLIED_AT_US));

	REQUIRE(scalar_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'trigger'") == 0);
	REQUIRE(scalar_int(db, "SELECT version FROM schema_version WHERE id = 1") == 2);

	// 원본의 두 번째 검사도 위반 행 id 를 담아 올린다(v0003_storage_invariants.py:38~41).
	// 두 검사의 문구가 서로 달라야 어느 불변조건이 깨졌는지 구별된다.
	REQUIRE(db.LastError().find("리비전 부모 카드 불변조건이 손상되어 있습니다") != std::string::npos);
	REQUIRE(db.LastError().find("rev-2") != std::string::npos);
}

// 대응 원본: tests/integration/test_database.py::test_v2_database_fixture_adds_storage_invariant_triggers
TEST_CASE("v0003 은 정상 데이터에서 여덟 트리거를 만든다", "[core][storage][migration]")
{
	const C_TEMP_DB temp("v0003_triggers");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));
	migrate_to(db, 2);
	seed_card_graph(db);

	REQUIRE(pynote::core::storage::migrations::v0003::Migrate(db, APPLIED_AT_US));

	for (const char* pszName : TRIGGER_NAMES)
	{
		INFO(pszName);
		REQUIRE(scalar_int(db,
			std::string("SELECT COUNT(*) FROM sqlite_master WHERE type = 'trigger' AND name = '")
			+ pszName + "'") == 1);
	}
	REQUIRE(scalar_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'trigger'") == 8);
	REQUIRE(scalar_int(db, "SELECT version FROM schema_version WHERE id = 1") == 3);
}

// 대응 원본: v0003_storage_invariants.py:47~149 의 여덟 트리거 본문.
// 원본 시험은 트리거의 존재만 확인한다 - 실제 중단 여부의 pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("v0003 의 여덟 트리거는 각자 지키는 연산을 실제로 중단시킨다", "[core][storage][migration]")
{
	const C_TEMP_DB temp("v0003_trigger_abort");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));
	migrate_to(db, 3);
	seed_card_graph(db);

	// 1. cards_current_revision_insert - 다른 카드의 리비전을 현재 리비전으로 달고 들어온다.
	REQUIRE_FALSE(db.Execute(
		"INSERT INTO cards(id, document_id, operation_id, position_key, capture_seq,"
		" created_at_us, updated_at_us, source, body, body_hash, current_revision_id)"
		" VALUES ('card-3', 'doc-1', 'op-1', 3, 3, 1, 1, 'typing', 'body-3', 'hash-3', 'rev-1')"));
	REQUIRE(scalar_int(db, "SELECT COUNT(*) FROM cards") == 2);
	// 실패 사유가 트리거의 RAISE 인지까지 본다. 외래키나 유니크 제약으로 막혀도 위 술어는
	// 통과하므로, 이 검사가 없으면 트리거를 실제로 시험했다고 말할 수 없다.
	REQUIRE(db.LastError().find(UTF8_MISMATCH_MESSAGE) != std::string::npos);

	// 2. cards_current_revision_update - 현재 리비전을 그대로 둔 채 본문만 바꾼다.
	REQUIRE_FALSE(db.Execute("UPDATE cards SET body = 'body-1-changed' WHERE id = 'card-1'"));
	REQUIRE(scalar_text(db, "SELECT body FROM cards WHERE id = 'card-1'") == "body-1");

	// 3. card_revisions_parent_insert - 부모가 다른 카드의 리비전이다.
	REQUIRE_FALSE(db.Execute(
		"INSERT INTO card_revisions(id, card_id, event_seq, parent_revision_id, body, body_hash,"
		" source, created_at_us)"
		" VALUES ('rev-3', 'card-2', 2, 'rev-1', 'body-2', 'hash-2', 'edit', 1)"));
	REQUIRE(scalar_int(db, "SELECT COUNT(*) FROM card_revisions") == 2);

	// 4. card_revisions_parent_update - 나중에 부모를 다른 카드의 리비전으로 바꾼다.
	REQUIRE_FALSE(db.Execute("UPDATE card_revisions SET parent_revision_id = 'rev-1' WHERE id = 'rev-2'"));
	REQUIRE(scalar_int(db,
		"SELECT COUNT(*) FROM card_revisions WHERE id = 'rev-2' AND parent_revision_id IS NULL") == 1);

	// 5. card_revisions_current_update - 카드가 가리키는 현재 리비전의 본문을 바꾼다.
	REQUIRE_FALSE(db.Execute("UPDATE card_revisions SET body = 'body-1-changed' WHERE id = 'rev-1'"));
	REQUIRE(scalar_text(db, "SELECT body FROM card_revisions WHERE id = 'rev-1'") == "body-1");

	// 6. capture_counter_no_decrease - 증가는 통과해야 하고 감소만 막혀야 한다.
	exec(db, "UPDATE counters SET next_value = 2 WHERE name = 'capture'");
	REQUIRE(scalar_int(db, "SELECT next_value FROM counters WHERE name = 'capture'") == 2);
	REQUIRE_FALSE(db.Execute("UPDATE counters SET next_value = 1 WHERE name = 'capture'"));
	REQUIRE(scalar_int(db, "SELECT next_value FROM counters WHERE name = 'capture'") == 2);

	// 7. capture_counter_no_rename
	REQUIRE_FALSE(db.Execute("UPDATE counters SET name = 'other' WHERE name = 'capture'"));
	REQUIRE(scalar_int(db, "SELECT COUNT(*) FROM counters WHERE name = 'capture'") == 1);

	// 8. capture_counter_no_delete
	REQUIRE_FALSE(db.Execute("DELETE FROM counters WHERE name = 'capture'"));
	REQUIRE(scalar_int(db, "SELECT COUNT(*) FROM counters WHERE name = 'capture'") == 1);
}

// 대응 원본: v0003_storage_invariants.py:61 의 RAISE(ABORT, ...) 문구.
// SPEC T-R2 §1(a) 가 요구하는 인코딩 계약이다 - 파이썬 시험 트리에 대응 케이스가 없어
// pytest node ID 는 W0 T4 역보강 대기다.
TEST_CASE("v0003 트리거의 한국어 문구는 UTF-8 로 저장된다", "[core][storage][migration]")
{
	const C_TEMP_DB temp("v0003_encoding");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));
	migrate_to(db, 3);

	const std::string sTriggerSql = scalar_text(db,
		"SELECT sql FROM sqlite_master WHERE type = 'trigger' AND name = 'cards_current_revision_insert'");
	REQUIRE_FALSE(sTriggerSql.empty());

	// 계측기 자체 검증. 이 기계의 narrow 실행 문자셋이 CP949 라는 SPEC T-R2 §1(a) 의 측정이
	// 아직 성립하는지 시험 안에서 확인한다. 이 술어가 깨지면 아래 두 검사는 더 이상
	// 인코딩 회귀를 가르지 못하므로, 통과 여부보다 이 자리에서 먼저 드러나야 한다.
	REQUIRE(std::string("카드와 현재 리비전이 일치하지 않습니다") == CP949_MISMATCH_MESSAGE);

	REQUIRE(sTriggerSql.find(UTF8_MISMATCH_MESSAGE) != std::string::npos);
	REQUIRE(sTriggerSql.find(CP949_MISMATCH_MESSAGE) == std::string::npos);
}

// 대응 원본: tests/integration/test_database.py::test_v3_workspace_fixture_migrates_rows_without_data_loss
TEST_CASE("v0004 는 workspace_state 행을 창 행으로 옮기고 원본 테이블을 지운다", "[core][storage][migration]")
{
	const C_TEMP_DB temp("v0004_carry");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));
	migrate_to(db, 3);

	exec(db,
		"INSERT INTO documents(id, title, created_at_us, updated_at_us)"
		" VALUES ('doc-v3', 'v3 document', 1, 2)");
	exec(db,
		"INSERT INTO workspace_state(id, open_document_ids_json, active_document_id, updated_at_us)"
		" VALUES (1, '[\"doc-v3\"]', 'doc-v3', 3)");

	REQUIRE(pynote::core::storage::migrations::v0004::Migrate(db, APPLIED_AT_US));

	REQUIRE(scalar_int(db, "SELECT COUNT(*) FROM workspace_windows") == 1);
	REQUIRE(scalar_text(db, "SELECT open_document_ids_json FROM workspace_windows") == "[\"doc-v3\"]");
	REQUIRE(scalar_text(db, "SELECT active_document_id FROM workspace_windows") == "doc-v3");
	REQUIRE(scalar_int64(db, "SELECT updated_at_us FROM workspace_windows") == 3);

	// window_id 는 randomblob/random() 산물이라 값이 아니라 모양만 계약이다(SPEC T-R2 §2).
	const std::string sWindowId = scalar_text(db, "SELECT window_id FROM workspace_windows");
	REQUIRE(sWindowId.size() == 36);
	REQUIRE(sWindowId[8] == '-');
	REQUIRE(sWindowId[13] == '-');
	REQUIRE(sWindowId[14] == '4');
	REQUIRE(sWindowId[18] == '-');
	REQUIRE(std::string("89ab").find(sWindowId[19]) != std::string::npos);
	REQUIRE(sWindowId[23] == '-');
	REQUIRE(sWindowId == scalar_text(db, "SELECT lower(window_id) FROM workspace_windows"));

	REQUIRE_FALSE(has_table(db, "workspace_state"));
	REQUIRE(scalar_int(db, "SELECT version FROM schema_version WHERE id = 1") == 4);
}

// 대응 원본: v0004_workspace_windows.py:19~40 의 INSERT ... SELECT 와 DROP.
// 빈 workspace_state 경로는 파이썬 시험 트리에 대응 케이스가 없어 pytest node ID 는
// W0 T4 역보강 대기다.
TEST_CASE("v0004 는 workspace_state 가 비어 있으면 창 행을 만들지 않는다", "[core][storage][migration]")
{
	const C_TEMP_DB temp("v0004_empty");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));
	migrate_to(db, 3);

	REQUIRE(pynote::core::storage::migrations::v0004::Migrate(db, APPLIED_AT_US));

	REQUIRE(has_table(db, "workspace_windows"));
	REQUIRE(scalar_int(db, "SELECT COUNT(*) FROM workspace_windows") == 0);
	REQUIRE_FALSE(has_table(db, "workspace_state"));
	REQUIRE(scalar_int(db, "SELECT version FROM schema_version WHERE id = 1") == 4);
}

// 대응 원본: tests/integration/test_database.py::test_v4_sort_mode_fixture_preserves_values_and_accepts_recency
TEST_CASE("v0005 는 개명-복사-삭제로 행 값을 보존한다", "[core][storage][migration]")
{
	const C_TEMP_DB temp("v0005_rows");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));
	migrate_to(db, 4);

	exec(db,
		"INSERT INTO documents(id, title, created_at_us, updated_at_us)"
		" VALUES ('doc-ui', 'document', 1, 1)");
	exec(db,
		"INSERT INTO document_ui_states(document_id, selected_card_id, list_scroll_position, sort_mode,"
		" editor_card_id, editor_base_revision_id, editor_cursor_qchar, updated_at_us)"
		" VALUES ('doc-ui', NULL, 7, 'capture', NULL, NULL, 11, 2)");

	REQUIRE(pynote::core::storage::migrations::v0005::Migrate(db, APPLIED_AT_US));

	REQUIRE(scalar_int(db, "SELECT COUNT(*) FROM document_ui_states") == 1);
	REQUIRE(scalar_text(db, "SELECT document_id FROM document_ui_states") == "doc-ui");
	REQUIRE(scalar_int(db, "SELECT list_scroll_position FROM document_ui_states") == 7);
	REQUIRE(scalar_text(db, "SELECT sort_mode FROM document_ui_states") == "capture");
	REQUIRE(scalar_int(db, "SELECT editor_cursor_qchar FROM document_ui_states") == 11);
	REQUIRE(scalar_int64(db, "SELECT updated_at_us FROM document_ui_states") == 2);
	REQUIRE(scalar_int(db,
		"SELECT COUNT(*) FROM document_ui_states"
		" WHERE selected_card_id IS NULL AND editor_card_id IS NULL"
		" AND editor_base_revision_id IS NULL") == 1);

	// 임시 테이블은 남지 않는다.
	REQUIRE_FALSE(has_table(db, "document_ui_states_v4"));

	// 새 테이블도 recency 를 받는다(원본 시험이 확인하는 계약).
	exec(db,
		"INSERT INTO documents(id, title, created_at_us, updated_at_us)"
		" VALUES ('doc-recency', 'document', 1, 1)");
	exec(db,
		"INSERT INTO document_ui_states(document_id, list_scroll_position, sort_mode, updated_at_us)"
		" VALUES ('doc-recency', 0, 'recency', 3)");

	REQUIRE(scalar_int(db, "SELECT version FROM schema_version WHERE id = 1") == 5);
}

// 대응 원본: tests/integration/test_database.py::test_v5_fixture_gains_editor_split_size_columns
TEST_CASE("v0006 은 편집 분할 크기 열 둘을 nullable 로 추가한다", "[core][storage][migration]")
{
	const C_TEMP_DB temp("v0006_columns");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));
	migrate_to(db, 5);
	seed_ui_state(db, "doc-split", 0, "recency");

	REQUIRE(pynote::core::storage::migrations::v0006::Migrate(db, APPLIED_AT_US));

	REQUIRE(column_is_nullable(db, "document_ui_states", "editor_split_left"));
	REQUIRE(column_is_nullable(db, "document_ui_states", "editor_split_right"));

	// 기존 행은 두 열이 NULL 인 채로 남는다.
	REQUIRE(scalar_int(db,
		"SELECT COUNT(*) FROM document_ui_states"
		" WHERE editor_split_left IS NULL AND editor_split_right IS NULL") == 1);

	// nullable 판정의 known-good - 실제로 NULL 을 다시 써 넣을 수 있어야 한다.
	exec(db, "UPDATE document_ui_states SET editor_split_left = 100, editor_split_right = 200");
	exec(db, "UPDATE document_ui_states SET editor_split_left = NULL, editor_split_right = NULL");

	REQUIRE(scalar_int(db, "SELECT version FROM schema_version WHERE id = 1") == 6);
}

// 대응 원본: tests/integration/test_database.py::test_v6_fixture_clears_horizontal_split_sizes
TEST_CASE("v0007 은 저장된 분할 크기를 NULL 로 되돌리고 나머지 UI 상태는 그대로 둔다", "[core][storage][migration]")
{
	const C_TEMP_DB temp("v0007_reset");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));
	migrate_to(db, 6);
	seed_ui_state(db, "doc-vertical", 3, "position");
	exec(db, "UPDATE document_ui_states SET editor_split_left = 400, editor_split_right = 800");

	REQUIRE(pynote::core::storage::migrations::v0007::Migrate(db, APPLIED_AT_US));

	REQUIRE(scalar_int(db,
		"SELECT COUNT(*) FROM document_ui_states"
		" WHERE editor_split_left IS NULL AND editor_split_right IS NULL") == 1);
	REQUIRE(scalar_text(db, "SELECT sort_mode FROM document_ui_states") == "position");
	REQUIRE(scalar_int(db, "SELECT list_scroll_position FROM document_ui_states") == 3);
	REQUIRE(scalar_int(db, "SELECT version FROM schema_version WHERE id = 1") == 7);
}

// 대응 원본: tests/integration/test_database.py::test_v7_fixture_clears_vertical_split_sizes
// v0007 과 문장이 같은 것은 원본의 의도다 - 사유가 다를 뿐 같은 열을 되돌린다(SPEC T-R2 §2).
TEST_CASE("v0008 도 저장된 분할 크기를 NULL 로 되돌린다", "[core][storage][migration]")
{
	const C_TEMP_DB temp("v0008_reset");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));
	migrate_to(db, 7);
	seed_ui_state(db, "doc-horizontal", 4, "capture");
	exec(db, "UPDATE document_ui_states SET editor_split_left = 500, editor_split_right = 300");

	REQUIRE(pynote::core::storage::migrations::v0008::Migrate(db, APPLIED_AT_US));

	REQUIRE(scalar_int(db,
		"SELECT COUNT(*) FROM document_ui_states"
		" WHERE editor_split_left IS NULL AND editor_split_right IS NULL") == 1);
	REQUIRE(scalar_text(db, "SELECT sort_mode FROM document_ui_states") == "capture");
	REQUIRE(scalar_int(db, "SELECT list_scroll_position FROM document_ui_states") == 4);
	REQUIRE(scalar_int(db, "SELECT version FROM schema_version WHERE id = 1") == 8);
}

// 대응 원본: tests/integration/test_database.py::test_v8_fixture_normalizes_old_preview_line_default
TEST_CASE("v0009 는 미리보기 줄 수 6 을 3 으로 정규화한다", "[core][storage][migration]")
{
	const C_TEMP_DB temp("v0009_normalize");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));
	migrate_to(db, 8);

	REQUIRE(scalar_int(db, "SELECT preview_lines FROM data_policy_settings WHERE id = 1") == 6);
	REQUIRE(pynote::core::storage::migrations::v0009::Migrate(db, APPLIED_AT_US));

	REQUIRE(scalar_int(db, "SELECT preview_lines FROM data_policy_settings WHERE id = 1") == 3);
	REQUIRE(scalar_int(db, "SELECT version FROM schema_version WHERE id = 1") == 9);
}

// 대응 원본: tests/integration/test_database.py::test_v8_fixture_preserves_explicit_preview_line_value
TEST_CASE("v0009 는 6 이 아닌 미리보기 줄 수를 건드리지 않는다", "[core][storage][migration]")
{
	const C_TEMP_DB temp("v0009_preserve");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));
	migrate_to(db, 8);

	exec(db, "UPDATE data_policy_settings SET preview_lines = 9 WHERE id = 1");
	REQUIRE(pynote::core::storage::migrations::v0009::Migrate(db, APPLIED_AT_US));

	REQUIRE(scalar_int(db, "SELECT preview_lines FROM data_policy_settings WHERE id = 1") == 9);
	REQUIRE(scalar_int(db, "SELECT version FROM schema_version WHERE id = 1") == 9);
}

// 대응 원본: tests/integration/test_database.py::test_new_v0_database_migrates_to_latest
TEST_CASE("빈 데이터베이스에 v1~v9 사다리를 한 번의 Run 으로 적용한다", "[core][storage][migration]")
{
	const C_TEMP_DB temp("ladder_v1_v9");
	pynote::core::storage::C_DATABASE db;
	REQUIRE(db.Open(temp.Utf8()));

	pynote::core::storage::C_MIGRATION_RUNNER runner;
	runner.SetExistingDatabase(false, temp.Utf8());
	REQUIRE(runner.Run(db) == pynote::core::storage::E_MIGRATE_RESULT::Ok);

	REQUIRE(pynote::core::storage::migrations::LatestSchemaVersion() == 9);
	REQUIRE(scalar_int(db, "SELECT version FROM schema_version WHERE id = 1") == 9);
	REQUIRE(db.SchemaVersion() == 9);

	// v0001 의 11 개에서 v0004 가 workspace_state 를 빼고 data_policy_settings 와
	// workspace_windows 가 더해져 12 개다. v0005 의 임시 테이블은 남지 않는다.
	REQUIRE(scalar_int(db,
		"SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'") == 12);
	REQUIRE(has_table(db, "data_policy_settings"));
	REQUIRE(has_table(db, "workspace_windows"));
	REQUIRE_FALSE(has_table(db, "workspace_state"));
	REQUIRE_FALSE(has_table(db, "document_ui_states_v4"));

	REQUIRE(scalar_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'trigger'") == 8);
	REQUIRE(column_is_nullable(db, "document_ui_states", "editor_split_left"));
	REQUIRE(column_is_nullable(db, "document_ui_states", "editor_split_right"));

	// v0002 가 6 으로 시드한 값을 v0009 가 3 으로 정규화한다. 두 본이 등록 순서대로
	// 돌았다는 증거이고, 순서가 뒤집히면 6 이 남는다.
	REQUIRE(scalar_int(db, "SELECT preview_lines FROM data_policy_settings WHERE id = 1") == 3);
	REQUIRE(scalar_int(db, "SELECT COUNT(*) FROM data_policy_settings") == 1);
	REQUIRE(scalar_int(db, "SELECT next_value FROM counters WHERE name = 'capture'") == 1);

	// 본마다 시계를 다시 읽으므로 마지막 본의 시각이 남는다. 2020~2100 사이면 마이크로초다.
	const std::int64_t nAppliedAtUs = scalar_int64(db, "SELECT applied_at_us FROM schema_version WHERE id = 1");
	REQUIRE(nAppliedAtUs > 1577836800000000LL);
	REQUIRE(nAppliedAtUs < 4102444800000000LL);
}
