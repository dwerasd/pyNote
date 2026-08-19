#include <catch_amalgamated.hpp>

#include "pynote/core/application/card_service.h"
#include "pynote/core/application/draft_coordinator.h"
#include "pynote/core/application/export.h"
#include "pynote/core/application/import_pipeline.h"
#include "pynote/core/application/workspace_state.h"
#include "pynote/core/domain/card_list_projection.h"
#include "pynote/core/domain/paragraph_parser.h"
#include "pynote/core/storage/migration_runner.h"
#include "pynote/core/storage/repositories.h"
#include "pynote/core/text/utf16_offset.h"
#include "pynote/platform/win32_import_support.h"

#include <sqlite3/sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef CreateEvent

#pragma comment(lib, "NoteExCore")

namespace
{
	namespace app = pynote::core::application;
	namespace domain = pynote::core::domain;
	namespace platform = pynote::platform;
	namespace storage = pynote::core::storage;
	namespace text = pynote::core::text;

	std::string byte_string(std::initializer_list<std::uint8_t> bytes)
	{
		return std::string(reinterpret_cast<const char*>(bytes.begin()), bytes.size());
	}

	std::vector<std::uint8_t> byte_vector(std::string_view value)
	{
		return { value.begin(), value.end() };
	}

	class DbFixture
	{
	public:
		explicit DbFixture(std::string documentId = "document-w2-cap")
			: path_(std::filesystem::temp_directory_path() /
				("noteex_w2_cap_" + std::to_string(::GetCurrentProcessId()) + "_" +
					std::to_string(++sequence_) + ".db")),
			  repositories_(database_), documentId_(std::move(documentId))
		{
			remove_();
			REQUIRE(database_.Open(path_.string()));
			storage::C_MIGRATION_RUNNER runner;
			runner.SetExistingDatabase(false, path_.string());
			REQUIRE(runner.Run(database_) == storage::E_MIGRATE_RESULT::Ok);

			domain::S_DOCUMENT document;
			document.sId = documentId_;
			document.sTitle = "W2 capability";
			document.nCreatedAtUs = 1000;
			document.nUpdatedAtUs = 1000;
			REQUIRE(repositories_.CreateDocument(document) == storage::E_REPO_RESULT::Ok);
		}

		~DbFixture()
		{
			database_.Close();
			remove_();
		}

		app::C_CARD_SERVICE Service(std::vector<std::int64_t> times = { 2000 })
		{
			auto clockState = std::make_shared<std::pair<std::vector<std::int64_t>, std::size_t>>(
				std::move(times), 0);
			auto idState = std::make_shared<int>(0);
			return app::C_CARD_SERVICE(database_, repositories_, parser_,
				[clockState] {
					const std::size_t index = (std::min)(clockState->second++, clockState->first.size() - 1);
					return clockState->first[index];
				},
				[idState] { return "cap-id-" + std::to_string((*idState)++); });
		}

		std::int64_t Count(const char* table)
		{
			const std::string sql = std::string("SELECT COUNT(*) FROM ") + table;
			sqlite3_stmt* statement = nullptr;
			REQUIRE(::sqlite3_prepare_v2(database_.Handle(), sql.c_str(), -1, &statement, nullptr) == SQLITE_OK);
			REQUIRE(::sqlite3_step(statement) == SQLITE_ROW);
			const std::int64_t count = ::sqlite3_column_int64(statement, 0);
			::sqlite3_finalize(statement);
			return count;
		}

		const std::string& DocumentId() const noexcept { return documentId_; }

		storage::C_DATABASE database_;
		storage::C_REPOSITORIES repositories_;
		domain::C_PARAGRAPH_PARSER parser_;

	private:
		void remove_() const
		{
			std::error_code error;
			std::filesystem::remove(path_, error);
			std::filesystem::remove(path_.string() + "-wal", error);
			std::filesystem::remove(path_.string() + "-shm", error);
		}

		std::filesystem::path path_;
		std::string documentId_;
		inline static int sequence_{ 0 };
	};

	domain::S_CARD create_card(app::C_CARD_SERVICE& service, const std::string& documentId,
		std::string body, domain::E_CAPTURE_OPERATION_SOURCE source = domain::E_CAPTURE_OPERATION_SOURCE::Typing,
		std::optional<std::string> before = std::nullopt)
	{
		domain::S_CARD card;
		REQUIRE(service.CreateCard(documentId, body, source, before, &card) == app::E_CARD_SERVICE_RESULT::Ok);
		return card;
	}

	std::vector<std::string> card_ids(const std::vector<domain::S_CARD>& cards)
	{
		std::vector<std::string> result;
		for (const auto& card : cards) { result.push_back(card.sId); }
		return result;
	}

	std::vector<std::int64_t> position_keys(const std::vector<domain::S_CARD>& cards)
	{
		std::vector<std::int64_t> result;
		for (const auto& card : cards) { result.push_back(card.nPositionKey); }
		return result;
	}

	domain::S_CARD require_source_mapping(DbFixture& fixture, app::C_CARD_SERVICE& service,
		domain::E_CAPTURE_OPERATION_SOURCE input, domain::E_CARD_SOURCE expectedCard,
		domain::E_EVENT_SOURCE expectedEvent, std::string body)
	{
		const auto card = create_card(service, fixture.DocumentId(), std::move(body), input);
		domain::S_CAPTURE_OPERATION operation;
		REQUIRE(fixture.repositories_.GetCaptureOperation(card.sOperationId, &operation) == storage::E_REPO_RESULT::Ok);
		REQUIRE(operation.eSource == input);
		REQUIRE(card.eSource == expectedCard);

		std::vector<domain::S_EDIT_EVENT> events;
		REQUIRE(fixture.repositories_.ListEvents(fixture.DocumentId(), &events) == storage::E_REPO_RESULT::Ok);
		const auto event = std::find_if(events.begin(), events.end(), [&](const auto& value) {
			return value.sCardId == std::optional<std::string>(card.sId);
		});
		REQUIRE(event != events.end());
		REQUIRE(event->sOperationId == std::optional<std::string>(card.sOperationId));
		REQUIRE(event->eSource == expectedEvent);
		return card;
	}

	domain::S_CARD projection_card(std::string id, std::int64_t updated,
		std::int64_t capture, std::int64_t position)
	{
		domain::S_CARD card;
		card.sId = std::move(id);
		card.sDocumentId = "projection-document";
		card.sOperationId = "operation-" + card.sId;
		card.nPositionKey = position;
		card.nCaptureSeq = capture;
		card.nCreatedAtUs = updated;
		card.nUpdatedAtUs = updated;
		card.eSource = domain::E_CARD_SOURCE::Typing;
		card.sBody = card.sId;
		card.sBodyHash = storage::TextHash(card.sBody);
		card.sCurrentRevisionId = "revision-" + card.sId;
		return card;
	}

	std::vector<std::string> projection_ids(const domain::C_CARD_LIST_PROJECTION& projection)
	{
		std::vector<std::string> result;
		for (std::size_t row = 0; row < projection.RowCount(); ++row) {
			REQUIRE(projection.CardAt(row) != nullptr);
			result.push_back(projection.CardAt(row)->sId);
		}
		return result;
	}

	struct TriggerProbe
	{
		int calls{ 0 };
		std::vector<std::string> ids;
		std::vector<std::int64_t> positions;
	};

	void fail_second_position(sqlite3_context* context, int argumentCount, sqlite3_value** arguments)
	{
		auto* probe = static_cast<TriggerProbe*>(::sqlite3_user_data(context));
		if (probe == nullptr || argumentCount != 2) {
			::sqlite3_result_error(context, "invalid trigger probe", -1);
			return;
		}
		++probe->calls;
		const auto* textValue = ::sqlite3_value_text(arguments[0]);
		probe->ids.emplace_back(textValue == nullptr ? "" : reinterpret_cast<const char*>(textValue));
		probe->positions.push_back(::sqlite3_value_int64(arguments[1]));
		if (probe->calls == 2) {
			::sqlite3_result_error(context, "injected second temporary update failure", -1);
			return;
		}
		::sqlite3_result_int(context, 1);
	}
}

TEST_CASE("WTL-CAP-FI-118", "[WTL-CAP-FI-118][W2-CAP][core]")
{
	const std::string expected = byte_string({ 0x41, 0xEA, 0xB0, 0x80, 0xF0, 0x9F, 0x98, 0x80 });
	const std::vector<std::uint8_t> utf8Bom = { 0xEF, 0xBB, 0xBF, 0x41, 0xEA, 0xB0, 0x80, 0xF0, 0x9F, 0x98, 0x80 };
	const std::vector<std::uint8_t> utf16Le = { 0xFF, 0xFE, 0x41, 0x00, 0x00, 0xAC, 0x3D, 0xD8, 0x00, 0xDE };
	const std::vector<std::uint8_t> utf16Be = { 0xFE, 0xFF, 0x00, 0x41, 0xAC, 0x00, 0xD8, 0x3D, 0xDE, 0x00 };
	const std::vector<std::uint8_t> utf8 = { 0x41, 0xEA, 0xB0, 0x80, 0xF0, 0x9F, 0x98, 0x80 };
	auto unusedLegacy = [](std::span<const std::uint8_t>) { return std::string("unexpected"); };
	REQUIRE(app::DecodeImportBytes(utf8Bom, unusedLegacy) == expected);
	REQUIRE(app::DecodeImportBytes(utf16Le, unusedLegacy) == expected);
	REQUIRE(app::DecodeImportBytes(utf16Be, unusedLegacy) == expected);
	REQUIRE(app::DecodeImportBytes(utf8, unusedLegacy) == expected);

	int legacyCalls = 0;
	const std::vector<std::uint8_t> invalid = { 0x80 };
	REQUIRE(app::DecodeImportBytes(invalid, [&](std::span<const std::uint8_t> value) {
		++legacyCalls;
		REQUIRE(value.size() == 1);
		REQUIRE(value[0] == 0x80);
		return std::string("legacy-result");
	}) == "legacy-result");
	REQUIRE(legacyCalls == 1);

	const UINT activeCodePage = ::GetACP();
	REQUIRE(activeCodePage != 0);
	const std::vector<std::uint8_t> systemInput = { 0x41, 0x80, 0x42 };
	REQUIRE(platform::DecodeSystemAnsi(systemInput) ==
		platform::DecodeWindowsCodePage(systemInput, activeCodePage));
}

TEST_CASE("WTL-CAP-FI-119", "[WTL-CAP-FI-119][W2-CAP][core]")
{
	DbFixture fixture("document-import-cap");
	app::C_IMPORT_PIPELINE pipeline(fixture.parser_, [](std::span<const std::uint8_t> input) {
		return platform::DecodeSystemAnsi(input);
	});
	const std::string original = " first\r\nline\r\n\r\n \t\r\nsecond\r\n\r\nthird";
	app::S_IMPORT_PREPARATION prepared;
	REQUIRE(pipeline.PrepareFromBytes("memory", byte_vector(original), &prepared) == app::E_IMPORT_RESULT::Ok);
	REQUIRE(prepared.sText == original);

	auto service = fixture.Service({ 2000 });
	std::vector<domain::S_CARD> cards;
	REQUIRE(service.CreateCards(fixture.DocumentId(), prepared.sText,
		domain::E_CAPTURE_OPERATION_SOURCE::Import, true, std::nullopt, &cards) == app::E_CARD_SERVICE_RESULT::Ok);
	REQUIRE(cards.size() == 3);
	REQUIRE((std::vector<std::string>{ cards[0].sBody, cards[1].sBody, cards[2].sBody } ==
		std::vector<std::string>{ " first\nline", "second", "third" }));
	REQUIRE(cards[0].sOperationId == cards[1].sOperationId);
	REQUIRE(cards[1].sOperationId == cards[2].sOperationId);
	REQUIRE((std::vector<std::int64_t>{ cards[0].nCaptureSeq, cards[1].nCaptureSeq, cards[2].nCaptureSeq } ==
		std::vector<std::int64_t>{ 1, 2, 3 }));

	domain::S_CAPTURE_OPERATION operation;
	REQUIRE(fixture.repositories_.GetCaptureOperation(cards[0].sOperationId, &operation) == storage::E_REPO_RESULT::Ok);
	REQUIRE(operation.eSource == domain::E_CAPTURE_OPERATION_SOURCE::Import);
	REQUIRE(operation.eSplitPolicy == domain::E_SPLIT_POLICY::SplitByBlankLine);
	REQUIRE(operation.sOriginalText == std::optional<std::string>(original));
	REQUIRE(operation.sOriginalHash == std::optional<std::string>(storage::TextHash(original)));
	for (const auto& card : cards) {
		REQUIRE(card.eSource == domain::E_CARD_SOURCE::Import);
		REQUIRE(card.sOperationId == operation.sId);
	}
	std::vector<domain::S_EDIT_EVENT> events;
	REQUIRE(fixture.repositories_.ListEvents(fixture.DocumentId(), &events) == storage::E_REPO_RESULT::Ok);
	REQUIRE(events.size() == 3);
	for (std::size_t index = 0; index < events.size(); ++index) {
		REQUIRE(events[index].eSource == domain::E_EVENT_SOURCE::Import);
		REQUIRE(events[index].sOperationId == std::optional<std::string>(operation.sId));
		REQUIRE(events[index].sCardId == std::optional<std::string>(cards[index].sId));
	}

	DbFixture failing("document-import-rollback");
	app::C_IMPORT_PIPELINE failingPipeline(failing.parser_, [](std::span<const std::uint8_t> input) {
		return platform::DecodeSystemAnsi(input);
	});
	app::S_IMPORT_PREPARATION failingPrepared;
	const std::string failingOriginal = "first\r\n\r\nsecond";
	REQUIRE(failingPipeline.PrepareFromBytes("memory", byte_vector(failingOriginal), &failingPrepared) ==
		app::E_IMPORT_RESULT::Ok);
	REQUIRE(failing.database_.Execute(
		"CREATE TRIGGER fail_later_import BEFORE INSERT ON card_revisions "
		"WHEN NEW.body = 'second' BEGIN SELECT RAISE(ABORT, 'injected import failure'); END"));
	auto failingService = failing.Service({ 3000 });
	std::vector<domain::S_CARD> ignored;
	REQUIRE(failingService.CreateCards(failing.DocumentId(), failingPrepared.sText,
		domain::E_CAPTURE_OPERATION_SOURCE::Import, true, std::nullopt, &ignored) ==
		app::E_CARD_SERVICE_RESULT::Failed);
	REQUIRE(failing.Count("cards") == 0);
	REQUIRE(failing.Count("capture_operations") == 0);
	REQUIRE(failing.Count("card_revisions") == 0);
	REQUIRE(failing.Count("edit_events") == 0);
}

TEST_CASE("WTL-CAP-TI-001", "[WTL-CAP-TI-001][W2-CAP][core]")
{
	DbFixture fixture;
	auto service = fixture.Service({ 2000 });
	const std::string composed = byte_string({ 0xC3, 0xA9 });
	const std::string decomposed = byte_string({ 0x65, 0xCC, 0x81 });
	const std::string nonBmp = byte_string({ 0xF0, 0x9F, 0x98, 0x80 });
	const std::string body = composed + "|" + decomposed + "|\r|\r\n|\n|trail  |" + nonBmp;
	const auto card = create_card(service, fixture.DocumentId(), body);

	domain::S_CARD stored;
	domain::S_CARD_REVISION revision;
	REQUIRE(fixture.repositories_.GetCard(card.sId, &stored) == storage::E_REPO_RESULT::Ok);
	REQUIRE(stored.sBody == body);
	REQUIRE(stored.sBodyHash == storage::TextHash(body));
	REQUIRE(stored.sCurrentRevisionId.has_value());
	REQUIRE(fixture.repositories_.GetRevision(*stored.sCurrentRevisionId, &revision) == storage::E_REPO_RESULT::Ok);
	REQUIRE(revision.sBody == body);
	REQUIRE(revision.sBodyHash == storage::TextHash(body));

	const std::string expectedLf = composed + "|" + decomposed + "|\n|\n|\n|trail  |" + nonBmp;
	const std::string expectedCrlf = composed + "|" + decomposed + "|\r\n|\r\n|\r\n|trail  |" + nonBmp;
	REQUIRE(app::RenderCards({ stored }, app::E_NEWLINE_FORMAT::Lf) == expectedLf);
	REQUIRE(app::RenderCards({ stored }, app::E_NEWLINE_FORMAT::Crlf) == expectedCrlf);
	REQUIRE(stored.sBody == body);
	REQUIRE(composed != decomposed);
}

TEST_CASE("WTL-CAP-TI-002", "[WTL-CAP-TI-002][W2-CAP][core]")
{
	const std::string value = "A" + byte_string({ 0xEA, 0xB0, 0x80 }) +
		byte_string({ 0xF0, 0x9F, 0x98, 0x80 }) + "B";
	REQUIRE(text::utf8_index_to_utf16_offset(value, 0) == 0);
	REQUIRE(text::utf8_index_to_utf16_offset(value, 1) == 1);
	REQUIRE(text::utf8_index_to_utf16_offset(value, 2) == 1);
	REQUIRE(text::utf8_index_to_utf16_offset(value, 4) == 2);
	REQUIRE(text::utf8_index_to_utf16_offset(value, 8) == 4);
	REQUIRE(text::utf8_index_to_utf16_offset(value, 9) == 5);
	REQUIRE(text::utf16_offset_to_utf8_index(value, 0) == 0);
	REQUIRE(text::utf16_offset_to_utf8_index(value, 1) == 1);
	REQUIRE(text::utf16_offset_to_utf8_index(value, 2) == 4);
	REQUIRE(text::utf16_offset_to_utf8_index(value, 3) == 4);
	REQUIRE(text::utf16_offset_to_utf8_index(value, 4) == 8);
	REQUIRE(text::utf16_offset_to_utf8_index(value, 999) == value.size());

	DbFixture fixture("document-offset-cap");
	domain::S_DRAFT draft;
	draft.sId = "draft-offset";
	draft.sDocumentId = fixture.DocumentId();
	draft.eDraftKind = domain::E_DRAFT_KIND::New;
	draft.sDraftText = value;
	draft.sDraftHash = storage::TextHash(value);
	draft.nCursorPositionQchar = 4;
	draft.nUpdatedAtUs = 2000;
	REQUIRE(fixture.repositories_.CreateDraft(draft) == storage::E_REPO_RESULT::Ok);
	domain::S_DRAFT restoredDraft;
	REQUIRE(fixture.repositories_.GetDraft(draft.sId, &restoredDraft) == storage::E_REPO_RESULT::Ok);
	REQUIRE(restoredDraft.nCursorPositionQchar == 4);

	app::C_WORKSPACE_STATE_STORE workspace(fixture.database_, fixture.repositories_, "window-offset");
	app::S_DOCUMENT_UI_STATE state;
	state.sDocumentId = fixture.DocumentId();
	state.nEditorCursorQchar = 4;
	state.nUpdatedAtUs = 2000;
	REQUIRE(workspace.SaveDocumentUiState(state) == storage::E_REPO_RESULT::Ok);
	app::S_DOCUMENT_UI_STATE restoredState;
	REQUIRE(workspace.LoadDocumentUiState(fixture.DocumentId(), &restoredState) == storage::E_REPO_RESULT::Ok);
	REQUIRE(restoredState.nEditorCursorQchar == 4);

	state.nEditorCursorQchar = 999;
	state.nUpdatedAtUs = 3000;
	REQUIRE(workspace.SaveDocumentUiState(state) == storage::E_REPO_RESULT::Ok);
	REQUIRE(workspace.LoadDocumentUiState(fixture.DocumentId(), &restoredState) == storage::E_REPO_RESULT::Ok);
	REQUIRE(restoredState.nEditorCursorQchar == 999);
	REQUIRE(text::utf16_offset_to_utf8_index(value,
		static_cast<std::size_t>(*restoredState.nEditorCursorQchar)) == value.size());
}

TEST_CASE("WTL-CAP-TI-011", "[WTL-CAP-TI-011][W2-CAP][core]")
{
	DbFixture fixture("document-source-cap");
	auto service = fixture.Service({ 2000, 3000, 4000, 5000, 6000 });
	const auto typedCard = require_source_mapping(fixture, service, domain::E_CAPTURE_OPERATION_SOURCE::Typing,
		domain::E_CARD_SOURCE::Typing, domain::E_EVENT_SOURCE::Typing, "typing");
	require_source_mapping(fixture, service, domain::E_CAPTURE_OPERATION_SOURCE::Paste,
		domain::E_CARD_SOURCE::Paste, domain::E_EVENT_SOURCE::Paste, "paste");
	require_source_mapping(fixture, service, domain::E_CAPTURE_OPERATION_SOURCE::Mixed,
		domain::E_CARD_SOURCE::Mixed, domain::E_EVENT_SOURCE::Mixed, "typing-and-paste");
	require_source_mapping(fixture, service, domain::E_CAPTURE_OPERATION_SOURCE::Import,
		domain::E_CARD_SOURCE::Import, domain::E_EVENT_SOURCE::Import, "import");

	REQUIRE(typedCard.sCurrentRevisionId.has_value());
	const std::int64_t operationsBeforeRestore = fixture.Count("capture_operations");
	storage::C_TRANSACTION transaction(fixture.database_);
	REQUIRE(transaction.IsActive());
	domain::S_EDIT_EVENT restoreEvent;
	restoreEvent.sEventId = "event-restore";
	restoreEvent.sDocumentId = fixture.DocumentId();
	restoreEvent.sCardId = typedCard.sId;
	restoreEvent.eEventType = domain::E_EVENT_TYPE::Restore;
	restoreEvent.eSource = domain::E_EVENT_SOURCE::Restore;
	restoreEvent.nOccurredAtUs = 6000;
	restoreEvent.sDetailsJson = "{}";
	domain::S_EDIT_EVENT storedRestoreEvent;
	REQUIRE(fixture.repositories_.CreateEvent(restoreEvent, &storedRestoreEvent) == storage::E_REPO_RESULT::Ok);
	REQUIRE(storedRestoreEvent.nEventSeq.has_value());
	REQUIRE_FALSE(storedRestoreEvent.sOperationId.has_value());

	const std::string restoredBody = "restored-body";
	domain::S_CARD_REVISION restoreRevision;
	restoreRevision.sId = "revision-restore";
	restoreRevision.sCardId = typedCard.sId;
	restoreRevision.nEventSeq = *storedRestoreEvent.nEventSeq;
	restoreRevision.sParentRevisionId = typedCard.sCurrentRevisionId;
	restoreRevision.sBody = restoredBody;
	restoreRevision.sBodyHash = storage::TextHash(restoredBody);
	restoreRevision.eSource = domain::E_REVISION_SOURCE::Restore;
	restoreRevision.nCreatedAtUs = 6000;
	REQUIRE(fixture.repositories_.CreateRevision(restoreRevision) == storage::E_REPO_RESULT::Ok);

	domain::S_CARD restoredCard = typedCard;
	restoredCard.sBody = restoredBody;
	restoredCard.sBodyHash = restoreRevision.sBodyHash;
	restoredCard.sCurrentRevisionId = restoreRevision.sId;
	restoredCard.nUpdatedAtUs = 6000;
	restoredCard.eSource = domain::E_CARD_SOURCE::Restore;
	REQUIRE(fixture.repositories_.AdvanceCardRevision(restoredCard, *typedCard.sCurrentRevisionId) ==
		storage::E_REPO_RESULT::Ok);
	REQUIRE(fixture.repositories_.TouchDocument(fixture.DocumentId(), 6000) == storage::E_REPO_RESULT::Ok);
	REQUIRE(transaction.Commit());

	domain::S_CARD reloadedCard;
	domain::S_CARD_REVISION reloadedRevision;
	domain::S_EDIT_EVENT reloadedEvent;
	REQUIRE(fixture.repositories_.GetCard(typedCard.sId, &reloadedCard) == storage::E_REPO_RESULT::Ok);
	REQUIRE(fixture.repositories_.GetRevision(restoreRevision.sId, &reloadedRevision) == storage::E_REPO_RESULT::Ok);
	REQUIRE(fixture.repositories_.GetEvent(*storedRestoreEvent.nEventSeq, &reloadedEvent) == storage::E_REPO_RESULT::Ok);
	REQUIRE(reloadedCard == restoredCard);
	REQUIRE(reloadedCard.eSource == domain::E_CARD_SOURCE::Restore);
	REQUIRE(reloadedRevision == restoreRevision);
	REQUIRE(reloadedRevision.eSource == domain::E_REVISION_SOURCE::Restore);
	REQUIRE(reloadedEvent == storedRestoreEvent);
	REQUIRE(reloadedEvent.eEventType == domain::E_EVENT_TYPE::Restore);
	REQUIRE(reloadedEvent.eSource == domain::E_EVENT_SOURCE::Restore);
	REQUIRE_FALSE(reloadedEvent.sOperationId.has_value());
	REQUIRE(fixture.Count("capture_operations") == operationsBeforeRestore);

	app::C_REPOSITORY_DRAFT_STORE draftStore(fixture.database_, fixture.repositories_);
	app::C_DRAFT_COORDINATOR coordinator(draftStore, 2000,
		[] { return 7000; }, [] { return 8000; }, [] { return 0; }, [] { return "draft-sticky"; });
	const auto opened = coordinator.OpenNew(fixture.DocumentId());
	REQUIRE(opened.eOutcome == app::E_DRAFT_OUTCOME::Ok);
	REQUIRE(opened.Session.has_value());
	REQUIRE_FALSE(coordinator.IncludesPaste("draft-sticky"));
	REQUIRE(coordinator.UpdateSession("draft-sticky", "typed", 5, false).eOutcome == app::E_DRAFT_OUTCOME::Ok);
	REQUIRE_FALSE(coordinator.IncludesPaste("draft-sticky"));
	REQUIRE(coordinator.UpdateSession("draft-sticky", "typed-pasted", 12, true).eOutcome == app::E_DRAFT_OUTCOME::Ok);
	REQUIRE(coordinator.IncludesPaste("draft-sticky"));
	REQUIRE(coordinator.UpdateSession("draft-sticky", "typed-pasted-more", 17, false).eOutcome == app::E_DRAFT_OUTCOME::Ok);
	REQUIRE(coordinator.IncludesPaste("draft-sticky"));
	REQUIRE(coordinator.CompleteSave("draft-sticky", "typed-pasted-more", std::nullopt).eOutcome ==
		app::E_DRAFT_OUTCOME::Ok);
	REQUIRE_FALSE(coordinator.IncludesPaste("draft-sticky"));
}

TEST_CASE("WTL-CAP-TI-027", "[WTL-CAP-TI-027][W2-CAP][core]")
{
	REQUIRE(::GetACP() == 949);
	const std::vector<std::uint8_t> invalidCp949 = { 0x41, 0x80, 0x42, 0xFF, 0x43 };
	const std::string replacement = byte_string({ 0xEF, 0xBF, 0xBD });
	const std::string expected = "A" + replacement + "B" + replacement + "C";
	REQUIRE(platform::DecodeSystemAnsi(invalidCp949) == expected);
	REQUIRE(platform::DecodeSystemAnsi(invalidCp949) ==
		platform::DecodeWindowsCodePage(invalidCp949, 949));

	int legacyCalls = 0;
	const std::vector<std::uint8_t> malformedBom = { 0xEF, 0xBB, 0xBF, 0x41, 0x80, 0x42 };
	const std::string decoded = app::DecodeImportBytes(malformedBom,
		[&](std::span<const std::uint8_t>) { ++legacyCalls; return std::string("legacy"); });
	REQUIRE(decoded == "A" + replacement + "B");
	REQUIRE(legacyCalls == 0);
}

TEST_CASE("WTL-CAP-NC-006", "[WTL-CAP-NC-006][W2-CAP][core]")
{
	DbFixture fixture("document-original-cap");
	auto service = fixture.Service({ 2000, 3000 });
	const std::string keepBody = "keep\r\nbody  ";
	const auto keepCard = create_card(service, fixture.DocumentId(), keepBody,
		domain::E_CAPTURE_OPERATION_SOURCE::Paste);
	domain::S_CAPTURE_OPERATION keepOperation;
	domain::S_CARD_REVISION keepRevision;
	REQUIRE(fixture.repositories_.GetCaptureOperation(keepCard.sOperationId, &keepOperation) ==
		storage::E_REPO_RESULT::Ok);
	REQUIRE_FALSE(keepOperation.sOriginalText.has_value());
	REQUIRE_FALSE(keepOperation.sOriginalHash.has_value());
	REQUIRE(keepCard.sBody == keepBody);
	REQUIRE(fixture.repositories_.GetRevision(*keepCard.sCurrentRevisionId, &keepRevision) ==
		storage::E_REPO_RESULT::Ok);
	REQUIRE(keepRevision.sBody == keepBody);

	const std::string original = " first\r\nline\r\n\r\n  \r\nsecond\n\nthird\n";
	std::vector<domain::S_CARD> splitCards;
	REQUIRE(service.CreateCards(fixture.DocumentId(), original, domain::E_CAPTURE_OPERATION_SOURCE::Paste,
		true, std::nullopt, &splitCards) == app::E_CARD_SERVICE_RESULT::Ok);
	REQUIRE(splitCards.size() == 3);
	REQUIRE((std::vector<std::string>{ splitCards[0].sBody, splitCards[1].sBody, splitCards[2].sBody } ==
		std::vector<std::string>{ " first\nline", "second", "third" }));
	domain::S_CAPTURE_OPERATION splitOperation;
	REQUIRE(fixture.repositories_.GetCaptureOperation(splitCards[0].sOperationId, &splitOperation) ==
		storage::E_REPO_RESULT::Ok);
	REQUIRE(splitOperation.eSplitPolicy == domain::E_SPLIT_POLICY::SplitByBlankLine);
	REQUIRE(splitOperation.sOriginalText == std::optional<std::string>(original));
	REQUIRE(splitOperation.sOriginalHash == std::optional<std::string>(storage::TextHash(original)));
	for (const auto& card : splitCards) { REQUIRE(card.sOperationId == splitOperation.sId); }
}

TEST_CASE("WTL-CAP-NC-007", "[WTL-CAP-NC-007][W2-CAP][core]")
{
	DbFixture fixture("document-identity-cap");
	auto service = fixture.Service({ 2000, 3000, 4000, 5000, 6000 });
	const auto first = create_card(service, fixture.DocumentId(), "first");
	const auto second = create_card(service, fixture.DocumentId(), "second");
	const auto third = create_card(service, fixture.DocumentId(), "third");
	const std::map<std::string, std::int64_t> captureBefore = {
		{ first.sId, first.nCaptureSeq }, { second.sId, second.nCaptureSeq }, { third.sId, third.nCaptureSeq }
	};

	domain::S_CARD moved;
	domain::S_CARD deleted;
	REQUIRE(service.MoveCard(third.sId, second.sId, &moved) == app::E_CARD_SERVICE_RESULT::Ok);
	REQUIRE(service.SoftDelete(second.sId, std::nullopt, false, std::nullopt, &deleted) ==
		app::E_CARD_SERVICE_RESULT::Ok);

	std::vector<domain::S_CARD> stored;
	REQUIRE(fixture.repositories_.ListCards(fixture.DocumentId(), &stored) == storage::E_REPO_RESULT::Ok);
	REQUIRE(stored.size() == 3);
	const auto requireIdentity = [](const domain::S_CARD& before, const domain::S_CARD& after) {
		REQUIRE(after.sId == before.sId);
		REQUIRE(after.sDocumentId == before.sDocumentId);
		REQUIRE(after.sOperationId == before.sOperationId);
		REQUIRE(after.nCaptureSeq == before.nCaptureSeq);
		REQUIRE(after.nCreatedAtUs == before.nCreatedAtUs);
		REQUIRE(after.eSource == before.eSource);
		REQUIRE(after.sBody == before.sBody);
		REQUIRE(after.sBodyHash == before.sBodyHash);
		REQUIRE(after.sCurrentRevisionId == before.sCurrentRevisionId);
	};
	for (const auto& card : stored) {
		REQUIRE(captureBefore.at(card.sId) == card.nCaptureSeq);
		if (card.sId == first.sId) {
			REQUIRE(card == first);
		} else if (card.sId == second.sId) {
			requireIdentity(second, card);
			REQUIRE(card.nPositionKey == second.nPositionKey);
			REQUIRE(card.nDeletedAtUs.has_value());
		} else {
			requireIdentity(third, card);
			REQUIRE(card.nPositionKey != third.nPositionKey);
			REQUIRE_FALSE(card.nDeletedAtUs.has_value());
		}
	}

	std::vector<domain::S_CARD> captureOrder;
	std::vector<domain::S_CARD> positionOrder;
	REQUIRE(service.ListActiveCards(fixture.DocumentId(), app::E_CARD_SORT_MODE::Capture, &captureOrder) ==
		app::E_CARD_SERVICE_RESULT::Ok);
	REQUIRE(service.ListActiveCards(fixture.DocumentId(), app::E_CARD_SORT_MODE::Position, &positionOrder) ==
		app::E_CARD_SERVICE_RESULT::Ok);
	REQUIRE(card_ids(captureOrder) == std::vector<std::string>{ first.sId, third.sId });
	REQUIRE(card_ids(positionOrder) == std::vector<std::string>{ first.sId, third.sId });
}

TEST_CASE("WTL-CAP-NC-008", "[WTL-CAP-NC-008][W2-CAP][core]")
{
	const auto a = projection_card("a", 30, 6, 30);
	const auto b = projection_card("b", 30, 5, 20);
	const auto c = projection_card("c", 20, 1, 10);
	const auto d = projection_card("d", 10, 1, 10);
	domain::C_CARD_LIST_PROJECTION projection;
	projection.SetCards({ d, b, a, c });
	REQUIRE(projection_ids(projection) == std::vector<std::string>{ "a", "b", "c", "d" });
	projection.SetSortMode(domain::E_CARD_LIST_SORT_MODE::Capture);
	REQUIRE(projection_ids(projection) == std::vector<std::string>{ "c", "d", "b", "a" });
	projection.SetSortMode(domain::E_CARD_LIST_SORT_MODE::Position);
	REQUIRE(projection_ids(projection) == std::vector<std::string>{ "c", "d", "b", "a" });
}

TEST_CASE("WTL-CAP-NC-009", "[WTL-CAP-NC-009][W2-CAP][core]")
{
	{
		DbFixture fixture("document-rebalance-success");
		auto service = fixture.Service({ 2000, 3000, 4000 });
		const auto first = create_card(service, fixture.DocumentId(), "first");
		const auto second = create_card(service, fixture.DocumentId(), "second");
		REQUIRE(fixture.repositories_.UpdateCardPosition(first.sId, 1, *first.sCurrentRevisionId) ==
			storage::E_REPO_RESULT::Ok);
		REQUIRE(fixture.repositories_.UpdateCardPosition(second.sId, 2, *second.sCurrentRevisionId) ==
			storage::E_REPO_RESULT::Ok);
		const auto inserted = create_card(service, fixture.DocumentId(), "inserted",
			domain::E_CAPTURE_OPERATION_SOURCE::Typing, second.sId);
		std::vector<domain::S_CARD> ordered;
		REQUIRE(service.ListActiveCards(fixture.DocumentId(), app::E_CARD_SORT_MODE::Position, &ordered) ==
			app::E_CARD_SERVICE_RESULT::Ok);
		REQUIRE(card_ids(ordered) == std::vector<std::string>{ first.sId, inserted.sId, second.sId });
		REQUIRE(position_keys(ordered) == std::vector<std::int64_t>{ 1024, 1536, 2048 });
		REQUIRE(ordered[0].nCaptureSeq == first.nCaptureSeq);
		REQUIRE(ordered[1].nCaptureSeq == 3);
		REQUIRE(ordered[2].nCaptureSeq == second.nCaptureSeq);
	}

	TriggerProbe probe;
	DbFixture fixture("document-rebalance-rollback");
	auto service = fixture.Service({ 2000, 3000, 4000 });
	const auto first = create_card(service, fixture.DocumentId(), "first");
	const auto second = create_card(service, fixture.DocumentId(), "second");
	REQUIRE(fixture.repositories_.UpdateCardPosition(first.sId, 1, *first.sCurrentRevisionId) ==
		storage::E_REPO_RESULT::Ok);
	REQUIRE(fixture.repositories_.UpdateCardPosition(second.sId, 2, *second.sCurrentRevisionId) ==
		storage::E_REPO_RESULT::Ok);

	std::vector<domain::S_CARD> cardsBefore;
	REQUIRE(fixture.repositories_.ListCards(fixture.DocumentId(), &cardsBefore) == storage::E_REPO_RESULT::Ok);
	domain::S_DOCUMENT documentBefore;
	REQUIRE(fixture.repositories_.GetDocument(fixture.DocumentId(), &documentBefore) == storage::E_REPO_RESULT::Ok);
	std::int64_t counterBefore = 0;
	REQUIRE(fixture.repositories_.GetCounter("capture", &counterBefore) == storage::E_REPO_RESULT::Ok);
	const std::map<std::string, std::int64_t> countsBefore = {
		{ "cards", fixture.Count("cards") },
		{ "capture_operations", fixture.Count("capture_operations") },
		{ "card_revisions", fixture.Count("card_revisions") },
		{ "edit_events", fixture.Count("edit_events") }
	};

	REQUIRE(::sqlite3_create_function_v2(fixture.database_.Handle(), "fail_second_position", 2,
		SQLITE_UTF8, &probe, &fail_second_position, nullptr, nullptr, nullptr) == SQLITE_OK);
	const std::string triggerSql =
		"CREATE TRIGGER fail_second_existing_position BEFORE UPDATE OF position_key ON cards "
		"WHEN OLD.id IN ('" + first.sId + "','" + second.sId + "') "
		"BEGIN SELECT fail_second_position(OLD.id, NEW.position_key); END";
	REQUIRE(fixture.database_.Execute(triggerSql));

	domain::S_CARD ignored;
	REQUIRE(service.CreateCard(fixture.DocumentId(), "must-rollback",
		domain::E_CAPTURE_OPERATION_SOURCE::Typing, second.sId, &ignored) == app::E_CARD_SERVICE_RESULT::Failed);
	REQUIRE(probe.calls == 2);
	REQUIRE(probe.ids == std::vector<std::string>{ first.sId, second.sId });
	REQUIRE(probe.positions.size() == 2);
	REQUIRE(probe.positions[0] < 0);
	REQUIRE(probe.positions[1] < 0);

	std::vector<domain::S_CARD> cardsAfter;
	REQUIRE(fixture.repositories_.ListCards(fixture.DocumentId(), &cardsAfter) == storage::E_REPO_RESULT::Ok);
	REQUIRE(cardsAfter == cardsBefore);
	REQUIRE(fixture.Count("cards") == countsBefore.at("cards"));
	REQUIRE(fixture.Count("capture_operations") == countsBefore.at("capture_operations"));
	REQUIRE(fixture.Count("card_revisions") == countsBefore.at("card_revisions"));
	REQUIRE(fixture.Count("edit_events") == countsBefore.at("edit_events"));
	std::int64_t counterAfter = 0;
	REQUIRE(fixture.repositories_.GetCounter("capture", &counterAfter) == storage::E_REPO_RESULT::Ok);
	REQUIRE(counterAfter == counterBefore);
	domain::S_DOCUMENT documentAfter;
	REQUIRE(fixture.repositories_.GetDocument(fixture.DocumentId(), &documentAfter) == storage::E_REPO_RESULT::Ok);
	REQUIRE(documentAfter == documentBefore);
}
