#include <catch_amalgamated.hpp>

#include "pynote/core/application/draft_coordinator.h"
#include "pynote/core/storage/migration_runner.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

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
		output.reserve(value.size() * 2);
		for (const unsigned char byte : value) {
			output.push_back(digits[byte >> 4]); output.push_back(digits[byte & 15]);
		}
		return output;
	}

	std::string repeat(std::string_view value, std::size_t count)
	{
		std::string output;
		output.reserve(value.size() * count);
		for (std::size_t index = 0; index < count; ++index) { output += value; }
		return output;
	}

	std::string truth(bool value) { return value ? "true" : "false"; }

	std::string join(const std::vector<std::string>& values, std::string_view separator = ",")
	{
		std::string output;
		for (const auto& value : values) { if (!output.empty()) { output += separator; } output += value; }
		return output;
	}

	void write_golden(const std::vector<std::string>& lines)
	{
		wchar_t path[32768] = {};
		const DWORD length = ::GetEnvironmentVariableW(L"PYNOTE_DRAFT_COORDINATOR_GOLDEN_OUT", path, 32768);
		if (length == 0 || length >= 32768) { return; }
		std::ofstream output(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
		REQUIRE(output.is_open());
		for (const auto& line : lines) { output << line << '\n'; }
		REQUIRE(output.good());
	}

	class Fixture
	{
	public:
		Fixture()
			: path_(std::filesystem::temp_directory_path() /
				("noteex_w2z4_draft_" + std::to_string(::GetCurrentProcessId()) + "_" +
				std::to_string(++sequence_) + ".db")), repositories_(database_), store_(database_, repositories_)
		{
			remove_();
			REQUIRE(database_.Open(path_.string()));
			storage::C_MIGRATION_RUNNER runner;
			runner.SetExistingDatabase(false, path_.string());
			REQUIRE(runner.Run(database_) == storage::E_MIGRATE_RESULT::Ok);
		}

		~Fixture() { database_.Close(); remove_(); }

		domain::S_DOCUMENT Document(std::string id, std::int64_t order = 1000)
		{
			domain::S_DOCUMENT document{ std::move(id), "draft test", order, order };
			REQUIRE(repositories_.CreateDocument(document) == storage::E_REPO_RESULT::Ok);
			return document;
		}

		domain::S_CARD Card(const std::string& documentId, int number)
		{
			domain::S_NEW_CAPTURE_OPERATION operation;
			operation.sId = "operation-" + std::to_string(number);
			operation.sDocumentId = documentId;
			operation.nCreatedAtUs = 2000 + number;
			domain::S_NEW_CARD card;
			card.sId = "card-" + std::to_string(number);
			card.sRevisionId = "revision-" + std::to_string(number);
			card.sEventId = "event-" + std::to_string(number);
			card.nPositionKey = static_cast<std::int64_t>(number) * 1024;
			card.sBody = "committed-" + std::to_string(number);
			card.nCreatedAtUs = 2000 + number;
			std::vector<domain::S_CARD> output;
			REQUIRE(repositories_.CreateCards(operation, { card }, &output) == storage::E_REPO_RESULT::Ok);
			REQUIRE(output.size() == 1);
			return output.front();
		}

		domain::S_DRAFT Draft(const std::string& id, const std::string& documentId,
			const std::optional<std::string>& cardId, std::string text, std::int64_t updated,
			std::int64_t cursor = 2)
		{
			domain::S_DRAFT draft;
			draft.sId = id; draft.sDocumentId = documentId; draft.sCardId = cardId;
			draft.eDraftKind = cardId ? domain::E_DRAFT_KIND::Edit : domain::E_DRAFT_KIND::New;
			draft.sDraftText = std::move(text); draft.sDraftHash = storage::TextHash(draft.sDraftText);
			draft.nCursorPositionQchar = cursor; draft.nUpdatedAtUs = updated;
			REQUIRE(repositories_.CreateDraft(draft) == storage::E_REPO_RESULT::Ok);
			return draft;
		}

		std::optional<domain::S_DRAFT> Stored(const std::string& id)
		{
			domain::S_DRAFT draft;
			return repositories_.GetDraft(id, &draft) == storage::E_REPO_RESULT::Ok ?
				std::optional<domain::S_DRAFT>(draft) : std::nullopt;
		}

		std::size_t RevisionCount(const std::string& cardId)
		{
			std::vector<domain::S_CARD_REVISION> revisions;
			REQUIRE(repositories_.ListRevisions(cardId, &revisions) == storage::E_REPO_RESULT::Ok);
			return revisions.size();
		}

		storage::C_DATABASE database_;
		storage::C_REPOSITORIES repositories_;
		app::C_REPOSITORY_DRAFT_STORE store_;

	private:
		void remove_() const
		{
			std::error_code error;
			std::filesystem::remove(path_, error);
			std::filesystem::remove(path_.string() + "-wal", error);
			std::filesystem::remove(path_.string() + "-shm", error);
		}
		std::filesystem::path path_;
		inline static int sequence_{ 0 };
	};

	class ScriptedStore final : public app::I_DRAFT_STORE
	{
	public:
		storage::E_REPO_RESULT ListDocuments(std::vector<domain::S_DOCUMENT>* out) override
			{ *out = documents; return storage::E_REPO_RESULT::Ok; }
		storage::E_REPO_RESULT GetDocument(const std::string& id, domain::S_DOCUMENT* out) override
		{
			for (const auto& value : documents) { if (value.sId == id) { *out = value; return storage::E_REPO_RESULT::Ok; } }
			return storage::E_REPO_RESULT::NotFound;
		}
		storage::E_REPO_RESULT ListDrafts(const std::string& documentId,
			std::vector<domain::S_DRAFT>* out) override
		{
			out->clear(); for (const auto& value : drafts) { if (value.sDocumentId == documentId) { out->push_back(value); } }
			return storage::E_REPO_RESULT::Ok;
		}
		storage::E_REPO_RESULT GetDraft(const std::string& id, domain::S_DRAFT* out) override
		{
			for (const auto& value : drafts) { if (value.sId == id) { *out = value; return storage::E_REPO_RESULT::Ok; } }
			return storage::E_REPO_RESULT::NotFound;
		}
		storage::E_REPO_RESULT GetCard(const std::string& id, domain::S_CARD* out) override
		{
			for (const auto& value : cards) { if (value.sId == id) { *out = value; return storage::E_REPO_RESULT::Ok; } }
			return storage::E_REPO_RESULT::NotFound;
		}
		storage::E_REPO_RESULT GetRevision(const std::string& id, domain::S_CARD_REVISION* out) override
		{
			for (const auto& value : revisions) { if (value.sId == id) { *out = value; return storage::E_REPO_RESULT::Ok; } }
			return storage::E_REPO_RESULT::NotFound;
		}
		storage::E_REPO_RESULT UpsertDraft(const domain::S_DRAFT& draft) override
		{
			++writes;
			if (failNext) { failNext = false; return storage::E_REPO_RESULT::Failed; }
			auto found = std::find_if(drafts.begin(), drafts.end(), [&draft](const auto& value) { return value.sId == draft.sId; });
			if (found == drafts.end()) { drafts.push_back(draft); } else { *found = draft; }
			if (onWrite) { onWrite(); }
			return storage::E_REPO_RESULT::Ok;
		}
		storage::E_REPO_RESULT DeleteDraft(const std::string& id) override
		{
			const auto before = drafts.size();
			drafts.erase(std::remove_if(drafts.begin(), drafts.end(), [&id](const auto& value) { return value.sId == id; }), drafts.end());
			return drafts.size() == before ? storage::E_REPO_RESULT::NotFound : storage::E_REPO_RESULT::Ok;
		}

		std::vector<domain::S_DOCUMENT> documents;
		std::vector<domain::S_DRAFT> drafts;
		std::vector<domain::S_CARD> cards;
		std::vector<domain::S_CARD_REVISION> revisions;
		std::function<void()> onWrite;
		bool failNext{ false };
		int writes{ 0 };
	};

	app::S_DRAFT_RECOVERY_CANDIDATE candidate(std::string document, std::optional<std::string> card,
		std::string draft)
	{
		app::S_DRAFT_RECOVERY_CANDIDATE value;
		value.Draft.sId = std::move(draft); value.Draft.sDocumentId = std::move(document);
		value.Draft.sCardId = std::move(card);
		value.Draft.eDraftKind = value.Draft.sCardId ? domain::E_DRAFT_KIND::Edit : domain::E_DRAFT_KIND::New;
		return value;
	}

	std::uint64_t last_arm(const std::vector<app::S_DRAFT_SCHEDULER_COMMAND>& commands)
	{
		for (auto it = commands.rbegin(); it != commands.rend(); ++it) {
			if (it->eAction == app::E_DRAFT_SCHEDULER_ACTION::Arm) { return it->nGeneration; }
		}
		return 0;
	}
}

TEST_CASE("W2-Z4 draft coordinator core", "[W2-Z4][core][application][draft]")
{
	std::vector<std::string> output;

	{
		const std::vector<app::S_DRAFT_RECOVERY_CANDIDATE> candidates{
			candidate("document-b", "card-b1", "draft-b1"), candidate("document-a", "card-a1", "draft-a1"),
			candidate("document-b", "card-b2", "draft-b2"), candidate("document-b", "card-b1", "copy"),
			candidate("document-a", std::nullopt, "new"), candidate("document-a", "card-a2", "draft-a2") };
		const auto plans = app::C_DRAFT_COORDINATOR::BuildRecoveryPlans(candidates,
			{ { "document-b", "card-b2" }, { "document-a", "not-a-candidate" } });
		REQUIRE(plans.size() == 2); REQUIRE(plans[0].sDisplayCardId == "card-b2");
		REQUIRE(plans[0].DeferredCardIds == std::vector<std::string>{ "card-b1" });
		REQUIRE(plans[1].sDisplayCardId == "card-a1");
		std::string planText;
		for (const auto& plan : plans) {
			if (!planText.empty()) { planText += ';'; }
			planText += plan.sDocumentId + ':' + plan.sDisplayCardId + '>' + join(plan.DeferredCardIds);
		}
		output.push_back("W2-Z4-0144|plans=" + planText);
	}

	for (const auto [label, updated, expected] : std::vector<std::tuple<std::string, std::int64_t, bool>>{
		{ "W2-Z4-0145", 2002, false }, { "W2-Z4-0146", 2001, true }, { "W2-Z4-0147", 2000, true } }) {
		Fixture fixture; fixture.Document("document-1"); const auto card = fixture.Card("document-1", 1);
		fixture.Draft("draft-time", "document-1", card.sId, "draft-time", updated);
		app::C_DRAFT_COORDINATOR coordinator(fixture.store_, 2000, [] { return 1; }, [] { return 1; },
			[] { return 1; }, [] { return "unused"; });
		const auto result = coordinator.RecoveryCandidates();
		REQUIRE(result.eOutcome == app::E_DRAFT_OUTCOME::Ok); REQUIRE(result.Candidates.size() == 1);
		REQUIRE(result.Candidates[0].bCommittedIsNewer == expected);
		output.push_back(label + "|newer=" + truth(result.Candidates[0].bCommittedIsNewer));
	}

	{
		Fixture fixture; fixture.Document("document-1"); const auto card = fixture.Card("document-1", 1);
		REQUIRE(fixture.database_.Execute("UPDATE cards SET current_revision_id = NULL WHERE id = 'card-1'"));
		fixture.Draft("draft-revisionless", "document-1", card.sId, "revisionless", 1);
		const auto before = fixture.Draft("draft-new", "document-1", std::nullopt, "new", 2);
		app::C_DRAFT_COORDINATOR coordinator(fixture.store_, 2000, [] { return 1; }, [] { return 1; },
			[] { return 1; }, [] { return "unused"; });
		const auto result = coordinator.RecoveryCandidates(); const auto after = fixture.Stored("draft-new");
		REQUIRE(result.Candidates.size() == 1); REQUIRE_FALSE(result.Candidates[0].bCommittedIsNewer);
		REQUIRE((after && *after == before));
		output.push_back("W2-Z4-0148|candidates=" + result.Candidates[0].Draft.sId +
			"|new_unchanged=" + truth(after && *after == before));
	}

	{
		Fixture fixture; fixture.Document("document-1"); const auto card = fixture.Card("document-1", 1);
		std::vector<app::S_DRAFT_SCHEDULER_COMMAND> commands;
		app::C_DRAFT_COORDINATOR coordinator(fixture.store_, 10, [] { return 5000; }, [] { return 0; },
			[] { return 0; }, [] { return "draft-ime"; }, storage::TextHash,
			[&](const auto& command) { commands.push_back(command); });
		const auto opened = coordinator.OpenCard(card); REQUIRE(opened.Session);
		coordinator.UpdateSession("draft-ime", "\xe3\x85\x8e", 1); coordinator.SetImeComposing("draft-ime", true);
		const auto stale = last_arm(commands); REQUIRE(coordinator.OnTimer("draft-ime", stale).eOutcome == app::E_DRAFT_OUTCOME::NoOp);
		const auto during = fixture.Stored("draft-ime"); REQUIRE_FALSE(during); coordinator.SetImeComposing("draft-ime", false);
		REQUIRE(coordinator.OnTimer("draft-ime", last_arm(commands)).eOutcome == app::E_DRAFT_OUTCOME::Ok);
		const auto stored = fixture.Stored("draft-ime"); REQUIRE(stored);
		output.push_back("W2-Z4-0149|during=" + std::string(during ? "present" : "missing") +
			"|after=" + hex(stored->sDraftText));
	}

	{
		Fixture fixture; fixture.Document("document-1"); const auto card = fixture.Card("document-1", 1);
		std::int64_t wall = 500000, age = 0;
		app::C_DRAFT_COORDINATOR coordinator(fixture.store_, 100, [&] { return wall; }, [&] { return age; },
			[] { return 0; }, [] { return "draft-max-age"; });
		coordinator.OpenCard(card); const auto revisions = fixture.RevisionCount(card.sId);
		for (const auto& [at, text] : std::vector<std::pair<std::int64_t, std::string>>{
			{ 0, "continuous-1" }, { 40000, "continuous-12" }, { 80000, "continuous-123" } }) {
			age = at; wall = 500000 - at; coordinator.UpdateSession("draft-max-age", text, 1);
			REQUIRE_FALSE(fixture.Stored("draft-max-age"));
		}
		age = 120000; wall = 380000; coordinator.UpdateSession("draft-max-age", "continuous-1234", 10);
		const auto stored = fixture.Stored("draft-max-age"); REQUIRE(stored); REQUIRE(stored->nUpdatedAtUs == 380000);
		REQUIRE(fixture.RevisionCount(card.sId) == revisions);
		output.push_back("W2-Z4-0150|text=" + hex(stored->sDraftText) + "|updated=" +
			std::to_string(stored->nUpdatedAtUs) + "|revisions=" + std::to_string(fixture.RevisionCount(card.sId)));
	}

	{
		Fixture fixture; fixture.Document("document-1"); const auto card = fixture.Card("document-1", 1);
		std::int64_t age = 0;
		app::C_DRAFT_COORDINATOR coordinator(fixture.store_, 100, [] { return 7000; }, [&] { return age; },
			[] { return 0; }, [] { return "draft-first-anchor"; });
		coordinator.OpenCard(card); age = 10000000; coordinator.UpdateSession("draft-first-anchor", "first-dirty", 11);
		const auto stored = fixture.Stored("draft-first-anchor"); REQUIRE_FALSE(stored);
		output.push_back("W2-Z4-0151|stored=" + truth(stored.has_value()));
	}

	{
		Fixture fixture; fixture.Document("document-1"); const auto card = fixture.Card("document-1", 1);
		std::int64_t age = 0;
		app::C_DRAFT_COORDINATOR coordinator(fixture.store_, 100, [] { return 8000; }, [&] { return age; },
			[] { return 0; }, [] { return "draft-reset"; });
		coordinator.OpenCard(card); coordinator.UpdateSession("draft-reset", "dirty-one", 8);
		age = 90000; coordinator.UpdateSession("draft-reset", card.sBody, 4);
		age = 1000000; coordinator.UpdateSession("draft-reset", "dirty-two", 9);
		age = 1099999; coordinator.UpdateSession("draft-reset", "dirty-two-2", 11);
		const auto before = fixture.Stored("draft-reset"); REQUIRE_FALSE(before); age = 1100000;
		coordinator.UpdateSession("draft-reset", "dirty-two-3", 11); const auto stored = fixture.Stored("draft-reset"); REQUIRE(stored);
		output.push_back("W2-Z4-0152|at99999=" + truth(before.has_value()) + "|at100000=" + hex(stored->sDraftText));
	}

	{
		Fixture fixture; fixture.Document("document-1"); const auto card = fixture.Card("document-1", 1);
		std::vector<app::S_DRAFT_SCHEDULER_COMMAND> commands; int protectedCount = 0;
		app::C_DRAFT_COORDINATOR coordinator(fixture.store_, 10, [] { return 5000; }, [] { return 0; },
			[] { return 0; }, [] { return "draft-dedup"; }, storage::TextHash,
			[&](const auto& command) { commands.push_back(command); }, [&](const auto&) { ++protectedCount; });
		coordinator.OpenCard(card); coordinator.UpdateSession("draft-dedup", "once", 5); const auto generation = last_arm(commands);
		coordinator.ProtectNow("draft-dedup"); coordinator.ProtectNow("draft-dedup"); coordinator.ProtectNow("draft-dedup");
		coordinator.OnTimer("draft-dedup", generation); REQUIRE(protectedCount == 1); const int timerCount = protectedCount;
		coordinator.DiscardDraft("draft-dedup"); coordinator.ProtectNow("draft-dedup"); REQUIRE(protectedCount == 2);
		output.push_back("W2-Z4-0153|writes=" + std::to_string(timerCount) + ',' + std::to_string(protectedCount) +
			"|timer=" + std::to_string(timerCount) + "|revisions=" + std::to_string(fixture.RevisionCount(card.sId)));
	}

	{
		Fixture fixture; fixture.Document("document-1"); const auto card = fixture.Card("document-1", 1); int hashes = 0;
		app::C_DRAFT_COORDINATOR coordinator(fixture.store_, 2000, [] { return 1; }, [] { return 1; },
			[] { return 0; }, [] { return "draft-one-hash"; }, [&](const std::string& text) { ++hashes; return storage::TextHash(text); });
		coordinator.OpenCard(card); coordinator.UpdateSession("draft-one-hash", "hash-once", 3);
		coordinator.ProtectNow("draft-one-hash"); REQUIRE(hashes == 1);
		output.push_back("W2-Z4-0154|hash_calls=" + std::to_string(hashes));
	}

	{
		Fixture fixture; fixture.Document("document-1"); const auto card = fixture.Card("document-1", 1);
		app::C_DRAFT_COORDINATOR coordinator(fixture.store_, 2000, [] { return 5000; }, [] { return 6000; },
			[] { return 0; }, [] { return "draft-save"; });
		coordinator.OpenCard(card); coordinator.UpdateSession("draft-save", "before-save", 10); coordinator.ProtectNow("draft-save");
		REQUIRE(fixture.store_.DeleteDraft("draft-save") == storage::E_REPO_RESULT::Ok);
		coordinator.CompleteSave("draft-save", card.sBody, card.sCurrentRevisionId);
		coordinator.UpdateSession("draft-save", "before-save", 10); coordinator.ProtectNow("draft-save");
		const auto stored = fixture.Stored("draft-save"); REQUIRE(stored);
		output.push_back("W2-Z4-0155|restored=" + hex(stored->sDraftText));
	}

	{
		Fixture fixture; fixture.Document("document-1"); const auto card = fixture.Card("document-1", 1);
		app::C_DRAFT_COORDINATOR coordinator(fixture.store_, 2000, [] { return 5000; }, [] { return 6000; },
			[] { return 0; }, [] { return "draft-reused"; });
		coordinator.OpenCard(card); coordinator.UpdateSession("draft-reused", "before-release", 10); coordinator.ProtectNow("draft-reused");
		coordinator.ReleaseSession("draft-reused"); REQUIRE(fixture.store_.DeleteDraft("draft-reused") == storage::E_REPO_RESULT::Ok);
		coordinator.OpenCard(card); coordinator.UpdateSession("draft-reused", "before-release", 10); coordinator.ProtectNow("draft-reused");
		const auto stored = fixture.Stored("draft-reused"); REQUIRE(stored);
		output.push_back("W2-Z4-0156|restored=" + hex(stored->sDraftText));
	}

	{
		ScriptedStore store; domain::S_CARD card; card.sId = "card-1"; card.sDocumentId = "document-1"; card.sBody = "committed";
		store.cards.push_back(card); store.failNext = true; std::int64_t age = 0; int failures = 0;
		app::C_DRAFT_COORDINATOR coordinator(store, 100, [] { return 5000; }, [&] { return age; }, [] { return 0; },
			[] { return "draft-retry"; }, storage::TextHash, {}, {}, [&](const auto&) { ++failures; });
		coordinator.OpenCard(card); coordinator.UpdateSession("draft-retry", "before-failure", 6); age = 100000;
		coordinator.UpdateSession("draft-retry", "first-failure", 8); REQUIRE(failures == 1); REQUIRE(store.drafts.empty());
		const bool before = !store.drafts.empty();
		age = 110000; coordinator.UpdateSession("draft-retry", "next-retry", 9); REQUIRE(store.drafts.size() == 1);
		output.push_back("W2-Z4-0157|failures=" + std::to_string(failures) + "|before=" + truth(before) +
			"|after=" + hex(store.drafts[0].sDraftText));
	}

	{
		Fixture fixture; fixture.Document("document-1"); std::vector<domain::S_CARD> cards;
		for (int number = 1; number <= 3; ++number) { cards.push_back(fixture.Card("document-1", number)); }
		for (int number = 1; number <= 3; ++number) {
			fixture.Draft(number == 1 ? "draft-recover" : number == 2 ? "draft-discard" : "draft-later",
				"document-1", cards[static_cast<std::size_t>(number - 1)].sId,
				cards[static_cast<std::size_t>(number - 1)].sBody + "+unsaved", 9000 + number, 4);
		}
		app::C_DRAFT_COORDINATOR restarted(fixture.store_, 2000, [] { return 1; }, [] { return 1; },
			[] { return 0; }, [] { return "fresh"; });
		const auto candidates = restarted.RecoveryCandidates(); REQUIRE(candidates.Candidates.size() == 3);
		const auto recovered = restarted.ResolveCandidate("draft-recover", app::E_DRAFT_DISPOSITION::Recover); REQUIRE(recovered.Session);
		restarted.ResolveCandidate("draft-discard", app::E_DRAFT_DISPOSITION::Discard);
		restarted.ResolveCandidate("draft-later", app::E_DRAFT_DISPOSITION::Later);
		const auto discarded = fixture.Stored("draft-discard"); const auto later = fixture.Stored("draft-later");
		REQUIRE_FALSE(discarded); REQUIRE(later);
		const auto openedLater = restarted.OpenCard(cards[2]); REQUIRE(openedLater.eOutcome == app::E_DRAFT_OUTCOME::NoOp);
		std::vector<std::string> candidateIds;
		for (const auto& value : candidates.Candidates) { candidateIds.push_back(value.Draft.sId); }
		output.push_back("W2-Z4-0158|candidates=" + join(candidateIds) + "|recover=" + hex(recovered.Session->sText) +
			"|discarded=" + truth(!discarded) + "|later=" + truth(later.has_value()) +
			"|open=" + (openedLater.eOutcome == app::E_DRAFT_OUTCOME::NoOp ? "deferred" : "opened"));
	}

	{
		Fixture fixture; fixture.Document("document-1"); const auto card = fixture.Card("document-1", 1);
		fixture.Draft("draft-corrupt", "document-1", card.sId, "valid", 9000);
		REQUIRE(fixture.database_.Execute("UPDATE drafts SET draft_text = 'corrupt' WHERE id = 'draft-corrupt'"));
		app::C_DRAFT_COORDINATOR coordinator(fixture.store_, 2000, [] { return 1; }, [] { return 1; },
			[] { return 0; }, [] { return "unused"; });
		const auto candidates = coordinator.RecoveryCandidates();
		REQUIRE(candidates.Candidates.empty());
		const auto resolved = coordinator.ResolveCandidate("draft-corrupt", app::E_DRAFT_DISPOSITION::Recover);
		const auto opened = coordinator.OpenCard(card, app::E_DRAFT_DISPOSITION::Recover);
		REQUIRE(resolved.eOutcome == app::E_DRAFT_OUTCOME::Corrupt); REQUIRE(opened.eOutcome == app::E_DRAFT_OUTCOME::Corrupt);
		output.push_back("W2-Z4-0159|candidates=" + std::to_string(candidates.Candidates.size()) +
			"|resolve=" + (resolved.eOutcome == app::E_DRAFT_OUTCOME::Corrupt ? "corrupt" : "other") +
			"|open=" + (opened.eOutcome == app::E_DRAFT_OUTCOME::Corrupt ? "corrupt" : "other"));
	}

	{
		ScriptedStore store; domain::S_CARD card; card.sId = "card-1"; card.sDocumentId = "document-1"; card.sBody = "committed";
		store.cards.push_back(card); app::C_DRAFT_COORDINATOR* coordinatorAddress = nullptr;
		app::C_DRAFT_COORDINATOR coordinator(store, 2000, [] { return 5000; }, [] { return 0; }, [] { return 0; },
			[] { return "draft-coalesced"; }); coordinatorAddress = &coordinator;
		bool reentered = false; store.onWrite = [&] {
			if (reentered) { return; } reentered = true;
			coordinatorAddress->UpdateSession("draft-coalesced", "latest", 9);
			REQUIRE(coordinatorAddress->ProtectNow("draft-coalesced").eOutcome == app::E_DRAFT_OUTCOME::NoOp);
		};
		coordinator.OpenCard(card); coordinator.UpdateSession("draft-coalesced", "old", 5); coordinator.ProtectNow("draft-coalesced");
		REQUIRE(store.drafts.size() == 1); REQUIRE(store.drafts[0].nCursorPositionQchar == 9);
		output.push_back("W2-Z4-0160|text=" + hex(store.drafts[0].sDraftText) + "|cursor=" +
			std::to_string(store.drafts[0].nCursorPositionQchar));
	}

	{
		ScriptedStore store; domain::S_CARD card; card.sId = "card-1"; card.sDocumentId = "document-1";
		card.sBody = "committed"; card.sCurrentRevisionId = "revision-1"; store.cards.push_back(card); store.failNext = true;
		int failures = 0; std::optional<app::S_DRAFT_EMERGENCY_PAYLOAD> emergency;
		app::C_DRAFT_COORDINATOR coordinator(store, 2000, [] { return 7000; }, [] { return 0; }, [] { return 0; },
			[] { return "draft-emergency"; }, storage::TextHash, {}, {}, [&](const auto&) { ++failures; },
			[&](const auto& payload) { emergency = payload; return true; });
		coordinator.OpenCard(card); coordinator.UpdateSession("draft-emergency", "preserve-me", 5, true);
		const auto result = coordinator.ProtectNow("draft-emergency");
		REQUIRE(result.eOutcome == app::E_DRAFT_OUTCOME::StorageFailure); REQUIRE(failures == 1); REQUIRE(emergency);
		output.push_back("W2-Z4-0161|result=" + std::string(result.eOutcome == app::E_DRAFT_OUTCOME::StorageFailure ?
			"storage_failure" : "other") + "|failed=" + std::to_string(failures) + "|emergency=" + emergency->sDraftId + "," +
			emergency->sDocumentId + "," + *emergency->sCardId + "," + *emergency->sBaseRevisionId + "," +
			hex(emergency->sDraftText) + "," + std::to_string(emergency->nCursorPositionQchar) + "," +
			std::to_string(emergency->nWrittenAtUs));
	}

	{
		Fixture fixture; fixture.Document("document-1"); const auto card = fixture.Card("document-1", 1);
		int ids = 0, protectedCount = 0; std::int64_t performance = 0;
		app::C_DRAFT_COORDINATOR coordinator(fixture.store_, 2000, [] { return 1; }, [] { return 1; },
			[&] { performance += 1000; return performance; }, [&] { return "draft-perf-" + std::to_string(++ids); },
			storage::TextHash, {}, [&](const auto&) { ++protectedCount; });
		std::vector<std::size_t> bytes; bool nonnegative = true;
		for (const auto& text : std::vector<std::string>{ repeat("\xea\xb0\x80", 1024),
			std::string(1024 * 1024, 'a'), std::string(10 * 1024 * 1024, 'a') }) {
			const auto opened = coordinator.OpenCard(card); REQUIRE(opened.Session);
			coordinator.UpdateSession(opened.Session->sDraftId, text, 0);
			const auto result = coordinator.ProtectNow(opened.Session->sDraftId); REQUIRE(result.Measurement);
			bytes.push_back(result.Measurement->nTextBytes); nonnegative = nonnegative && result.Measurement->dElapsedMs >= 0;
			coordinator.DiscardSession(opened.Session->sDraftId);
		}
		REQUIRE(bytes == std::vector<std::size_t>{ 3072, 1048576, 10485760 }); REQUIRE(nonnegative); REQUIRE(protectedCount == 3);
		std::vector<std::string> byteText;
		for (const auto value : bytes) { byteText.push_back(std::to_string(value)); }
		output.push_back("W2-Z4-0162|bytes=" + join(byteText) + "|nonnegative=" + truth(nonnegative) +
			"|protected=" + std::to_string(protectedCount));
	}

	REQUIRE(output.size() == 19);
	for (std::size_t index = 0; index < output.size(); ++index) {
		REQUIRE(output[index].starts_with("W2-Z4-0" + std::to_string(144 + index)));
	}
	write_golden(output);
}
