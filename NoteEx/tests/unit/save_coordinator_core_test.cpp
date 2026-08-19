#include <catch_amalgamated.hpp>

#include "pynote/core/application/save_coordinator.h"
#include "pynote/core/storage/migration_runner.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
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
	namespace storage = pynote::core::storage;

	std::string hex(std::string_view value)
	{
		constexpr char digits[] = "0123456789abcdef";
		std::string output;
		for (const unsigned char byte : value) {
			output.push_back(digits[byte >> 4]); output.push_back(digits[byte & 15]);
		}
		return output;
	}

	std::string truth(bool value) { return value ? "true" : "false"; }

	std::string outcome(app::E_SAVE_OUTCOME value)
	{
		switch (value) {
		case app::E_SAVE_OUTCOME::Saved: return "saved";
		case app::E_SAVE_OUTCOME::Unchanged: return "unchanged";
		case app::E_SAVE_OUTCOME::Conflict: return "conflict";
		case app::E_SAVE_OUTCOME::Rejected: return "rejected";
		case app::E_SAVE_OUTCOME::Failed: return "failed";
		case app::E_SAVE_OUTCOME::CommittedSessionFailure: return "committed_session_failure";
		}
		return "unknown";
	}

	std::string error(app::E_SAVE_ERROR value)
	{
		switch (value) {
		case app::E_SAVE_ERROR::ImeComposing: return "ime_composing";
		case app::E_SAVE_ERROR::MissingCardIdentity: return "missing_card_identity";
		case app::E_SAVE_ERROR::InactiveCard: return "inactive_card";
		case app::E_SAVE_ERROR::RepositoryFailure: return "repository";
		default: return "other";
		}
	}

	void write_golden(const std::vector<std::string>& lines)
	{
		wchar_t path[32768] = {};
		const DWORD length = ::GetEnvironmentVariableW(L"PYNOTE_SAVE_COORDINATOR_GOLDEN_OUT", path, 32768);
		if (length == 0 || length >= 32768) { return; }
		std::ofstream outputFile(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
		REQUIRE(outputFile.is_open());
		for (const auto& line : lines) { outputFile << line << '\n'; }
		REQUIRE(outputFile.good());
	}

	class Fixture
	{
	public:
		Fixture()
			: path_(std::filesystem::temp_directory_path() /
				("noteex_w2z5_save_" + std::to_string(::GetCurrentProcessId()) + "_" +
				std::to_string(++sequence_) + ".db")), repositories_(database_), draftStore_(database_, repositories_)
		{
			remove_(); REQUIRE(database_.Open(path_.string()));
			storage::C_MIGRATION_RUNNER runner; runner.SetExistingDatabase(false, path_.string());
			REQUIRE(runner.Run(database_) == storage::E_MIGRATE_RESULT::Ok);
		}

		~Fixture() { database_.Close(); remove_(); }

		domain::S_CARD CreateCard()
		{
			domain::S_DOCUMENT document{ "document-1", "save test", 1000, 1000 };
			REQUIRE(repositories_.CreateDocument(document) == storage::E_REPO_RESULT::Ok);
			domain::S_NEW_CAPTURE_OPERATION operation;
			operation.sId = "operation-1"; operation.sDocumentId = document.sId; operation.nCreatedAtUs = 2000;
			domain::S_NEW_CARD card;
			card.sId = "card-1"; card.sRevisionId = "revision-1"; card.sEventId = "event-1";
			card.nPositionKey = 1024; card.sBody = "\xea\xb8\xb0\xec\xa1\xb4 \xed\x99\x95\xec\xa0\x95 \xeb\xb3\xb8\xeb\xac\xb8";
			card.nCreatedAtUs = 2000;
			std::vector<domain::S_CARD> cards;
			REQUIRE(repositories_.CreateCards(operation, { card }, &cards) == storage::E_REPO_RESULT::Ok);
			return cards.front();
		}

		std::vector<domain::S_CARD_REVISION> Revisions(const std::string& cardId)
		{
			std::vector<domain::S_CARD_REVISION> values;
			REQUIRE(repositories_.ListRevisions(cardId, &values) == storage::E_REPO_RESULT::Ok);
			return values;
		}

		std::vector<domain::S_EDIT_EVENT> Events()
		{
			std::vector<domain::S_EDIT_EVENT> values;
			REQUIRE(repositories_.ListEvents("document-1", &values) == storage::E_REPO_RESULT::Ok);
			return values;
		}

		domain::S_CARD StoredCard()
		{
			domain::S_CARD card; REQUIRE(repositories_.GetCard("card-1", &card) == storage::E_REPO_RESULT::Ok);
			return card;
		}

		domain::S_DOCUMENT StoredDocument()
		{
			domain::S_DOCUMENT document;
			REQUIRE(repositories_.GetDocument("document-1", &document) == storage::E_REPO_RESULT::Ok);
			return document;
		}

		std::optional<domain::S_DRAFT> StoredDraft(const std::string& id)
		{
			domain::S_DRAFT draft;
			return repositories_.GetDraft(id, &draft) == storage::E_REPO_RESULT::Ok ?
				std::optional<domain::S_DRAFT>(draft) : std::nullopt;
		}

		const std::filesystem::path& Path() const { return path_; }

		storage::C_DATABASE database_;
		storage::C_REPOSITORIES repositories_;
		app::C_REPOSITORY_DRAFT_STORE draftStore_;

	private:
		void remove_() const
		{
			std::error_code ignored;
			std::filesystem::remove(path_, ignored);
			std::filesystem::remove(path_.string() + "-wal", ignored);
			std::filesystem::remove(path_.string() + "-shm", ignored);
		}
		std::filesystem::path path_;
		inline static int sequence_{ 0 };
	};

	domain::S_CARD commit_body(storage::C_DATABASE& database, storage::C_REPOSITORIES& repositories,
		const std::string& body, const std::string& suffix, std::int64_t at)
	{
		domain::S_CARD card; REQUIRE(repositories.GetCard("card-1", &card) == storage::E_REPO_RESULT::Ok);
		REQUIRE(card.sCurrentRevisionId);
		storage::C_TRANSACTION transaction(database); REQUIRE(transaction.IsActive());
		domain::S_EDIT_EVENT event;
		event.sEventId = "event-" + suffix; event.sDocumentId = card.sDocumentId; event.sCardId = card.sId;
		event.eEventType = domain::E_EVENT_TYPE::Update; event.eSource = domain::E_EVENT_SOURCE::Edit;
		event.nOccurredAtUs = at; event.sDetailsJson = "{}";
		domain::S_EDIT_EVENT storedEvent;
		REQUIRE(repositories.CreateEvent(event, &storedEvent) == storage::E_REPO_RESULT::Ok);
		REQUIRE(storedEvent.nEventSeq);
		domain::S_CARD_REVISION revision;
		revision.sId = "revision-" + suffix; revision.sCardId = card.sId;
		revision.nEventSeq = *storedEvent.nEventSeq; revision.sParentRevisionId = card.sCurrentRevisionId;
		revision.sBody = body; revision.sBodyHash = storage::TextHash(body);
		revision.eSource = domain::E_REVISION_SOURCE::Edit; revision.nCreatedAtUs = at;
		REQUIRE(repositories.CreateRevision(revision) == storage::E_REPO_RESULT::Ok);
		domain::S_CARD saved = card; saved.sBody = body; saved.sBodyHash = revision.sBodyHash;
		saved.sCurrentRevisionId = revision.sId; saved.nUpdatedAtUs = at;
		REQUIRE(repositories.AdvanceCardRevision(saved, *card.sCurrentRevisionId) == storage::E_REPO_RESULT::Ok);
		REQUIRE(repositories.TouchDocument(card.sDocumentId, at) == storage::E_REPO_RESULT::Ok);
		REQUIRE(transaction.Commit());
		return saved;
	}
}

TEST_CASE("W2-Z5 save coordinator core", "[W2-Z5][core][application][save]")
{
	std::vector<std::string> output;

	{
		Fixture fixture; const auto card = fixture.CreateCard();
		app::C_DRAFT_COORDINATOR drafts(fixture.draftStore_, 2000, [] { return 3000; }, [] { return 0; },
			[] { return 0; }, [] { return "draft-1"; });
		std::vector<std::string> idOrder; int clockCalls = 0;
		app::C_SAVE_COORDINATOR save(fixture.database_, fixture.repositories_, drafts,
			[&] { ++clockCalls; return 4000; }, [&] { idOrder.push_back(idOrder.empty() ? "revision-save" : "event-save"); return idOrder.back(); });
		const auto opened = drafts.OpenCard(card); REQUIRE(opened.Session);
		const std::string body = "\xec\x83\x88 \xed\x99\x95\xec\xa0\x95 \xeb\xb3\xb8\xeb\xac\xb8";
		drafts.UpdateSession("draft-1", body, 7, true);
		const auto result = save.Save("draft-1"); REQUIRE(result.eOutcome == app::E_SAVE_OUTCOME::Saved); REQUIRE(result.Card);
		const auto events = fixture.Events(); const auto revisions = fixture.Revisions(card.sId);
		const auto document = fixture.StoredDocument(); const auto session = drafts.Session("draft-1");
		const bool immutable = result.Card->sOperationId == card.sOperationId &&
			result.Card->nPositionKey == card.nPositionKey && result.Card->nCaptureSeq == card.nCaptureSeq &&
			result.Card->nCreatedAtUs == card.nCreatedAtUs && result.Card->eSource == card.eSource &&
			result.Card->sDocumentId == card.sDocumentId && result.Card->nDeletedAtUs == card.nDeletedAtUs;
		REQUIRE(clockCalls == 1); REQUIRE(idOrder == std::vector<std::string>{ "revision-save", "event-save" });
		REQUIRE(events.size() == 2); REQUIRE(revisions.size() == 2); REQUIRE(events.back().sDetailsJson ==
			"{\"base_revision_id\":\"revision-1\",\"includes_paste\":true}");
		REQUIRE(revisions.back().sParentRevisionId == card.sCurrentRevisionId); REQUIRE(document.nUpdatedAtUs == 4000);
		REQUIRE_FALSE(fixture.StoredDraft("draft-1")); REQUIRE((session && !session->bDirty)); REQUIRE(immutable);
		output.push_back("W2-Z5-0001|outcome=" + outcome(result.eOutcome) + "|body=" + hex(result.Card->sBody) +
			"|time=" + std::to_string(result.Card->nUpdatedAtUs) + "|ids=" + idOrder[0] + ',' + idOrder[1] +
			"|details=" + hex(events.back().sDetailsJson) + "|parent=" + *revisions.back().sParentRevisionId +
			"|counts=" + std::to_string(revisions.size()) + ',' + std::to_string(events.size()) +
			"|draft=" + (fixture.StoredDraft("draft-1") ? "present" : "missing") +
			"|session=" + (session && !session->bDirty ? "clean" : "dirty") + "|immutable=" + truth(immutable) +
			"|document=" + std::to_string(document.nUpdatedAtUs));
	}

	{
		Fixture fixture; const auto card = fixture.CreateCard();
		app::C_DRAFT_COORDINATOR drafts(fixture.draftStore_, 2000, [] { return 3000; }, [] { return 0; },
			[] { return 0; }, [] { return "draft-failure"; });
		int idCalls = 0;
		app::C_SAVE_COORDINATOR save(fixture.database_, fixture.repositories_, drafts, [] { return 4000; },
			[&] { return idCalls++ == 0 ? "revision-failure" : "event-failure"; });
		REQUIRE(drafts.OpenCard(card).Session); drafts.UpdateSession("draft-failure", "rollback-body", 6);
		REQUIRE(fixture.database_.Execute("CREATE TRIGGER fail_card_update BEFORE UPDATE ON cards "
			"WHEN NEW.body = 'rollback-body' BEGIN SELECT RAISE(ABORT, 'save failure'); END"));
		const auto result = save.Save("draft-failure"); const auto stored = fixture.StoredDraft("draft-failure");
		const auto session = drafts.Session("draft-failure"); const auto document = fixture.StoredDocument();
		REQUIRE(result.eOutcome == app::E_SAVE_OUTCOME::Failed); REQUIRE(result.eError == app::E_SAVE_ERROR::RepositoryFailure);
		REQUIRE(fixture.StoredCard() == card); REQUIRE(fixture.Revisions(card.sId).size() == 1); REQUIRE(fixture.Events().size() == 1);
		REQUIRE((stored && stored->sDraftText == "rollback-body")); REQUIRE((session && session->bDirty));
		output.push_back("W2-Z5-0002|outcome=" + outcome(result.eOutcome) + "|reason=" + error(result.eError) +
			"|card=" + hex(fixture.StoredCard().sBody) + "|counts=" + std::to_string(fixture.Revisions(card.sId).size()) +
			',' + std::to_string(fixture.Events().size()) + "|document=" + std::to_string(document.nUpdatedAtUs) +
			"|draft=" + hex(stored->sDraftText) + "|dirty=" + truth(session->bDirty));
	}

	{
		Fixture fixture; const auto card = fixture.CreateCard();
		app::C_DRAFT_COORDINATOR drafts(fixture.draftStore_, 2000, [] { return 3000; }, [] { return 0; },
			[] { return 0; }, [] { return "draft-same"; });
		int clockCalls = 0, idCalls = 0;
		app::C_SAVE_COORDINATOR save(fixture.database_, fixture.repositories_, drafts,
			[&] { ++clockCalls; return 4000; }, [&] { ++idCalls; return "unused"; });
		REQUIRE(drafts.OpenCard(card).Session); drafts.UpdateSession("draft-same", "temporary", 3); drafts.ProtectNow("draft-same");
		drafts.UpdateSession("draft-same", card.sBody, 0); const auto result = save.Save("draft-same");
		const auto session = drafts.Session("draft-same"); REQUIRE(result.eOutcome == app::E_SAVE_OUTCOME::Unchanged);
		const bool missingAfterSave = !fixture.StoredDraft("draft-same").has_value();
		REQUIRE(clockCalls == 0); REQUIRE(idCalls == 0); REQUIRE(missingAfterSave);
		drafts.UpdateSession("draft-same", "temporary", 3); drafts.ProtectNow("draft-same");
		const auto rewritten = fixture.StoredDraft("draft-same"); REQUIRE(rewritten);
		output.push_back("W2-Z5-0003|outcome=" + outcome(result.eOutcome) + "|counts=" +
			std::to_string(fixture.Revisions(card.sId).size()) + ',' + std::to_string(fixture.Events().size()) +
			"|card_same=" + truth(fixture.StoredCard() == card) + "|document_same=" +
			truth(fixture.StoredDocument().nUpdatedAtUs == 1000) + "|draft=" +
			(missingAfterSave ? "missing" : "present") + "|session=" +
			(session && !session->bDirty ? "clean" : "dirty") + "|rewrite=" + hex(rewritten->sDraftText));
	}

	{
		Fixture fixture; const auto card = fixture.CreateCard();
		app::C_DRAFT_COORDINATOR first(fixture.draftStore_, 2000, [] { return 3000; }, [] { return 0; },
			[] { return 0; }, [] { return "draft-first"; });
		app::C_DRAFT_COORDINATOR second(fixture.draftStore_, 2000, [] { return 3000; }, [] { return 0; },
			[] { return 0; }, [] { return "draft-second"; });
		REQUIRE(first.OpenCard(card).Session); REQUIRE(second.OpenCard(card).Session);
		const std::string staleText = "\xec\x98\xa4\xeb\x9e\x98\xeb\x90\x9c \xed\x8e\xb8\xec\xa7\x91";
		const std::string committedText = "\xeb\xa8\xbc\xec\xa0\x80 \xec\xa0\x80\xec\x9e\xa5\xeb\x90\x9c \xed\x8e\xb8\xec\xa7\x91";
		first.UpdateSession("draft-first", staleText, 5); second.UpdateSession("draft-second", committedText, 5);
		int secondId = 0;
		app::C_SAVE_COORDINATOR secondSave(fixture.database_, fixture.repositories_, second, [] { return 4000; },
			[&] { return secondId++ == 0 ? "revision-second" : "event-second"; });
		REQUIRE(secondSave.Save("draft-second").eOutcome == app::E_SAVE_OUTCOME::Saved);
		int staleIds = 0;
		app::C_SAVE_COORDINATOR firstSave(fixture.database_, fixture.repositories_, first, [] { return 5000; },
			[&] { ++staleIds; return "unused"; });
		const auto result = firstSave.Save("draft-first"); const auto durable = fixture.StoredDraft("draft-first");
		const auto session = first.Session("draft-first"); REQUIRE(result.eOutcome == app::E_SAVE_OUTCOME::Conflict);
		REQUIRE(result.Conflict); REQUIRE(staleIds == 0); REQUIRE(durable); REQUIRE((session && session->bDirty));
		output.push_back("W2-Z5-0004|outcome=" + outcome(result.eOutcome) + "|base=" + *result.Conflict->sBaseRevisionId +
			"|current=" + *result.Conflict->sCurrentRevisionId + "|base_text=" + hex(result.Conflict->sBaseText) +
			"|committed=" + hex(result.Conflict->sCommittedText) + "|draft=" + hex(result.Conflict->sDraftText) +
			"|counts=" + std::to_string(fixture.Revisions(card.sId).size()) + ',' + std::to_string(fixture.Events().size()) +
			"|durable=" + hex(durable->sDraftText) + "|dirty=" + truth(session->bDirty));
	}

	{
		Fixture fixture; const auto card = fixture.CreateCard();
		app::C_DRAFT_COORDINATOR drafts(fixture.draftStore_, 2000, [] { return 3000; }, [] { return 0; },
			[] { return 0; }, [] { return "draft-race"; });
		REQUIRE(drafts.OpenCard(card).Session); drafts.UpdateSession("draft-race", "stale-save", 0);
		storage::C_DATABASE otherDatabase; REQUIRE(otherDatabase.Open(fixture.Path().string()));
		storage::C_REPOSITORIES otherRepositories(otherDatabase); bool hookCalled = false;
		app::C_SAVE_COORDINATOR save(fixture.database_, fixture.repositories_, drafts, [] { return 6000; },
			[] { return "unused"; }, [&] { hookCalled = true; (void)commit_body(otherDatabase, otherRepositories,
				"racing-save", "race", 10000); });
		const auto result = save.Save("draft-race"); otherDatabase.Close(); const auto stored = fixture.StoredCard();
		const auto durable = fixture.StoredDraft("draft-race"); REQUIRE(hookCalled); REQUIRE(result.eOutcome == app::E_SAVE_OUTCOME::Conflict);
		REQUIRE(stored.sBody == "racing-save"); REQUIRE(stored.sCurrentRevisionId == "revision-race"); REQUIRE(durable);
		output.push_back("W2-Z5-0005|outcome=" + outcome(result.eOutcome) + "|body=" + hex(stored.sBody) +
			"|revision=" + *stored.sCurrentRevisionId + "|counts=" + std::to_string(fixture.Revisions(card.sId).size()) +
			',' + std::to_string(fixture.Events().size()) + "|draft=" + hex(durable->sDraftText));
	}

	{
		Fixture fixture; const auto card = fixture.CreateCard(); int draftId = 0;
		app::C_DRAFT_COORDINATOR drafts(fixture.draftStore_, 2000, [] { return 3000; }, [] { return 0; },
			[] { return 0; }, [&] { return draftId++ == 0 ? "draft-ime" : draftId == 2 ? "draft-new" : "draft-inactive"; });
		int clockCalls = 0, idCalls = 0;
		app::C_SAVE_COORDINATOR save(fixture.database_, fixture.repositories_, drafts,
			[&] { ++clockCalls; return 4000; }, [&] { ++idCalls; return "unused"; });
		REQUIRE(drafts.OpenCard(card).Session); drafts.SetImeComposing("draft-ime", true);
		const auto ime = save.Save("draft-ime"); drafts.SetImeComposing("draft-ime", false); drafts.ReleaseSession("draft-ime");
		const auto openedNew = drafts.OpenNew("document-1"); REQUIRE(openedNew.Session); const auto missing = save.Save(openedNew.Session->sDraftId);
		const auto openedInactive = drafts.OpenCard(card); REQUIRE(openedInactive.Session);
		REQUIRE(card.sCurrentRevisionId); REQUIRE(fixture.repositories_.UpdateCardDeletedState(card.sId, card.nPositionKey, 9000,
			*card.sCurrentRevisionId) == storage::E_REPO_RESULT::Ok);
		const auto before = fixture.StoredCard(); const auto inactive = save.Save(openedInactive.Session->sDraftId);
		REQUIRE(ime.eError == app::E_SAVE_ERROR::ImeComposing); REQUIRE(missing.eError == app::E_SAVE_ERROR::MissingCardIdentity);
		REQUIRE(inactive.eError == app::E_SAVE_ERROR::InactiveCard); REQUIRE(fixture.StoredCard() == before);
		REQUIRE(clockCalls == 0); REQUIRE(idCalls == 0);
		output.push_back("W2-Z5-0006|ime=" + error(ime.eError) + "|new=" + error(missing.eError) +
			"|inactive=" + error(inactive.eError) + "|history=" + std::to_string(fixture.Revisions(card.sId).size()) +
			',' + std::to_string(fixture.Events().size()) + "|clock=" + std::to_string(clockCalls) +
			"|ids=" + std::to_string(idCalls));
	}

	REQUIRE(output.size() == 6);
	for (std::size_t index = 0; index < output.size(); ++index) {
		REQUIRE(output[index].starts_with("W2-Z5-000" + std::to_string(index + 1)));
	}
	write_golden(output);
}
