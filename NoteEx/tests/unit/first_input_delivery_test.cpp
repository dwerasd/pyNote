#include <catch_amalgamated.hpp>

#include "pynote/core/application/first_input_capture.h"
#include "pynote/core/domain/paragraph_parser.h"
#include "pynote/core/storage/migration_runner.h"
#include "pynote/core/storage/repositories.h"

#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
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

	void emit(std::string_view id, std::string_view payload)
	{
		wchar_t path[32768] = {};
		const DWORD length = ::GetEnvironmentVariableW(L"PYNOTE_FIRST_INPUT_DELIVERY_GOLDEN_OUT", path, 32768);
		if (length == 0 || length >= 32768) { return; }
		std::ofstream output(std::filesystem::path(path), std::ios::binary |
			(id == "WTL-W2-0098" ? std::ios::trunc : std::ios::app));
		REQUIRE(output.is_open()); output << id << '|' << payload << '\n'; REQUIRE(output.good());
	}

	app::S_FIRST_INPUT_EVENT insertion(std::string text, domain::E_CAPTURE_OPERATION_SOURCE source)
	{
		return { std::move(text), 0, source, true };
	}

	std::vector<app::E_FIRST_INPUT_DELIVERY_EFFECT> effect_kinds(
		const app::S_FIRST_INPUT_RECOVERY_RESULT& result)
	{
		std::vector<app::E_FIRST_INPUT_DELIVERY_EFFECT> values;
		for (const auto& effect : result.DeliveryEffects) { values.push_back(effect.eEffect); }
		return values;
	}

	const std::vector<app::E_FIRST_INPUT_DELIVERY_EFFECT> AllEffects{
		app::E_FIRST_INPUT_DELIVERY_EFFECT::CardCreated,
		app::E_FIRST_INPUT_DELIVERY_EFFECT::HistoryRefresh,
		app::E_FIRST_INPUT_DELIVERY_EFFECT::ContentChanged,
		app::E_FIRST_INPUT_DELIVERY_EFFECT::DocumentChanged,
		app::E_FIRST_INPUT_DELIVERY_EFFECT::CardConnected,
		app::E_FIRST_INPUT_DELIVERY_EFFECT::HistoryCard,
		app::E_FIRST_INPUT_DELIVERY_EFFECT::CardOpened,
	};

	class Fixture
	{
	public:
		Fixture()
			: path_(std::filesystem::temp_directory_path() /
				("noteex_w2r12_delivery_" + std::to_string(::GetCurrentProcessId()) + "_" +
				std::to_string(++sequence_) + ".db")), repositories_(database_)
		{
			remove_(); REQUIRE(database_.Open(path_.string()));
			storage::C_MIGRATION_RUNNER runner; runner.SetExistingDatabase(false, path_.string());
			REQUIRE(runner.Run(database_) == storage::E_MIGRATE_RESULT::Ok);
			domain::S_DOCUMENT document; document.sId = DocumentId; document.sTitle = "delivery";
			document.nCreatedAtUs = 1000; document.nUpdatedAtUs = 1000;
			REQUIRE(repositories_.CreateDocument(document) == storage::E_REPO_RESULT::Ok);
		}
		~Fixture() { database_.Close(); remove_(); }

		app::C_CARD_SERVICE Service()
		{
			return app::C_CARD_SERVICE(database_, repositories_, parser_, [] { return 2000; },
				[this] { return next_id_(); });
		}

		app::C_FIRST_INPUT_CAPTURE Capture(app::C_CARD_SERVICE& service,
			domain::C_CARD_LIST_PROJECTION& projection, app::CreateOneCardPort create = {})
		{
			if (!create) {
				create = [&service](const std::string& text, domain::E_CAPTURE_OPERATION_SOURCE source) {
					domain::S_CARD card;
					if (service.CreateCard(DocumentId, text, source, std::nullopt, &card) !=
						app::E_CARD_SERVICE_RESULT::Ok) { return std::optional<domain::S_CARD>{}; }
					return std::optional<domain::S_CARD>(card);
				};
			}
			return app::C_FIRST_INPUT_CAPTURE(projection,
				[this](const std::string& text) { return parser_.Split(text).empty(); }, std::move(create),
				[this](const std::string& id) { domain::S_CARD card;
					if (repositories_.GetCard(id, &card) != storage::E_REPO_RESULT::Ok || card.nDeletedAtUs) {
						return std::optional<domain::S_CARD>{}; }
					return std::optional<domain::S_CARD>(card); }, [](const domain::S_CARD&) { return true; });
		}

		std::vector<domain::S_CARD> Cards()
		{
			std::vector<domain::S_CARD> cards;
			REQUIRE(repositories_.ListCards(DocumentId, &cards) == storage::E_REPO_RESULT::Ok); return cards;
		}

		void RequireProvenance(const domain::S_CARD& card, domain::E_CARD_SOURCE cardSource,
			domain::E_CAPTURE_OPERATION_SOURCE operationSource, domain::E_EVENT_SOURCE eventSource)
		{
			REQUIRE(card.eSource == cardSource);
			domain::S_CAPTURE_OPERATION operation;
			REQUIRE(repositories_.GetCaptureOperation(card.sOperationId, &operation) == storage::E_REPO_RESULT::Ok);
			REQUIRE(operation.eSource == operationSource);
			std::vector<domain::S_EDIT_EVENT> events;
			REQUIRE(repositories_.ListEvents(DocumentId, &events) == storage::E_REPO_RESULT::Ok);
			std::vector<domain::S_EDIT_EVENT> creates;
			for (const auto& event : events) {
				if (event.sCardId == card.sId && event.eEventType == domain::E_EVENT_TYPE::Create) { creates.push_back(event); }
			}
			REQUIRE(creates.size() == 1); REQUIRE(creates[0].eSource == eventSource);
		}

		inline static const std::string DocumentId = "document-delivery";

	private:
		std::string next_id_()
		{
			const int number = idCalls_ / 4 + 1; const int kind = idCalls_++ % 4;
			if (kind == 0) return "operation-" + std::to_string(number);
			if (kind == 1) return "card-" + std::to_string(number);
			if (kind == 2) return "revision-" + std::to_string(number);
			return "event-" + std::to_string(number);
		}
		void remove_() const
		{
			std::error_code error; std::filesystem::remove(path_, error);
			std::filesystem::remove(path_.string() + "-wal", error);
			std::filesystem::remove(path_.string() + "-shm", error);
		}
		std::filesystem::path path_; storage::C_DATABASE database_; storage::C_REPOSITORIES repositories_;
		domain::C_PARAGRAPH_PARSER parser_; int idCalls_{ 0 }; inline static int sequence_{ 0 };
	};

	#define DELIVERY_TAGS(ID) "[W2-R12][core][application][first-input-delivery][" ID "]"
}

TEST_CASE("WTL-W2-0098", DELIVERY_TAGS("WTL-W2-0098"))
{
	Fixture f; auto service = f.Service(); domain::C_CARD_LIST_PROJECTION projection;
	auto capture = f.Capture(service, projection);
	const auto first = capture.OnInputEvent(insertion("a", domain::E_CAPTURE_OPERATION_SOURCE::Typing));
	const auto later = capture.OnInputEvent(insertion("ab", domain::E_CAPTURE_OPERATION_SOURCE::Paste));
	const auto cards = f.Cards(); REQUIRE(cards.size() == 1); REQUIRE(cards[0].sBody == "a");
	f.RequireProvenance(cards[0], domain::E_CARD_SOURCE::Typing,
		domain::E_CAPTURE_OPERATION_SOURCE::Typing, domain::E_EVENT_SOURCE::Typing);
	REQUIRE(effect_kinds(first) == AllEffects); REQUIRE(later.DeliveryEffects.empty());
	emit("WTL-W2-0098", "first=typing|later=paste|card_source=typing|operation_source=typing|event_source=typing|surface=6162|cards=1|later_effects=0");
}

TEST_CASE("WTL-W2-0099", DELIVERY_TAGS("WTL-W2-0099"))
{
	Fixture f; auto service = f.Service(); domain::C_CARD_LIST_PROJECTION projection;
	std::vector<domain::E_CAPTURE_OPERATION_SOURCE> attempts;
	app::CreateOneCardPort create = [&](const std::string& text, auto source) {
		attempts.push_back(source); if (attempts.size() == 1) return std::optional<domain::S_CARD>{};
		domain::S_CARD card; REQUIRE(service.CreateCard(Fixture::DocumentId, text, source, std::nullopt, &card) == app::E_CARD_SERVICE_RESULT::Ok); return std::optional(card); };
	auto capture = f.Capture(service, projection, create);
	const auto failed = capture.OnInputEvent(insertion("p", domain::E_CAPTURE_OPERATION_SOURCE::Paste));
	const auto retry = capture.OnInputEvent(insertion("pt", domain::E_CAPTURE_OPERATION_SOURCE::Typing));
	const auto cards = f.Cards(); REQUIRE(cards.size() == 1); REQUIRE(cards[0].sBody == "pt");
	f.RequireProvenance(cards[0], domain::E_CARD_SOURCE::Typing,
		domain::E_CAPTURE_OPERATION_SOURCE::Typing, domain::E_EVENT_SOURCE::Typing);
	REQUIRE(failed.DeliveryEffects.empty()); REQUIRE(effect_kinds(retry) == AllEffects);
	REQUIRE((attempts == std::vector<domain::E_CAPTURE_OPERATION_SOURCE>{
		domain::E_CAPTURE_OPERATION_SOURCE::Paste, domain::E_CAPTURE_OPERATION_SOURCE::Typing}));
	emit("WTL-W2-0099", "failed=paste|retry=typing|attempts=paste,typing|body=7074|card_source=typing|operation_source=typing|event_source=typing|scoped=1");
}

TEST_CASE("WTL-W2-0100", DELIVERY_TAGS("WTL-W2-0100"))
{
	Fixture f; auto service = f.Service(); domain::C_CARD_LIST_PROJECTION projection;
	std::vector<domain::E_CAPTURE_OPERATION_SOURCE> attempts;
	app::CreateOneCardPort create = [&](const std::string& text, auto source) {
		attempts.push_back(source); if (attempts.size() == 1) return std::optional<domain::S_CARD>{};
		domain::S_CARD card; REQUIRE(service.CreateCard(Fixture::DocumentId, text, source, std::nullopt, &card) == app::E_CARD_SERVICE_RESULT::Ok); return std::optional(card); };
	auto capture = f.Capture(service, projection, create);
	const auto failed = capture.OnInputEvent(insertion("t", domain::E_CAPTURE_OPERATION_SOURCE::Typing));
	const auto retry = capture.OnInputEvent(insertion("tp", domain::E_CAPTURE_OPERATION_SOURCE::Paste));
	const auto cards = f.Cards(); REQUIRE(cards.size() == 1); REQUIRE(cards[0].sBody == "tp");
	f.RequireProvenance(cards[0], domain::E_CARD_SOURCE::Paste,
		domain::E_CAPTURE_OPERATION_SOURCE::Paste, domain::E_EVENT_SOURCE::Paste);
	REQUIRE(failed.DeliveryEffects.empty()); REQUIRE(effect_kinds(retry) == AllEffects);
	REQUIRE((attempts == std::vector<domain::E_CAPTURE_OPERATION_SOURCE>{
		domain::E_CAPTURE_OPERATION_SOURCE::Typing, domain::E_CAPTURE_OPERATION_SOURCE::Paste}));
	emit("WTL-W2-0100", "failed=typing|retry=paste|attempts=typing,paste|body=7470|card_source=paste|operation_source=paste|event_source=paste|scoped=1");
}

TEST_CASE("WTL-W2-0101", DELIVERY_TAGS("WTL-W2-0101"))
{
	Fixture f; auto service = f.Service(); domain::C_CARD_LIST_PROJECTION projection;
	auto capture = f.Capture(service, projection); const std::string body = "\xec\x86\x8c\xeb\xb9\x84\xec\x9e\x90 \xea\xb0\xb1\xec\x8b\xa0";
	const auto result = capture.OnInputEvent(insertion(body, domain::E_CAPTURE_OPERATION_SOURCE::Paste));
	const auto repeat = capture.OnInputEvent(insertion(body + "!", domain::E_CAPTURE_OPERATION_SOURCE::Typing));
	REQUIRE(effect_kinds(result) == AllEffects); REQUIRE(result.DeliveryEffects.size() == 7);
	for (const auto& effect : result.DeliveryEffects) {
		REQUIRE(effect.sCardId == "card-1"); REQUIRE(effect.sDocumentId == Fixture::DocumentId);
	}
	REQUIRE(projection.RowCount() == 1); REQUIRE(f.Cards().size() == 1); REQUIRE(repeat.DeliveryEffects.empty());
	emit("WTL-W2-0101", "card=card-1|body=ec868cebb984ec9e9020eab0b1ec8ba0|created=1|connected=1|content_changed=1|opened=1|history_refresh=1|history_card=1|published=1|projection=1|repeat_effects=0");
}

#undef DELIVERY_TAGS
