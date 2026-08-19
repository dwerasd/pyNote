#include <catch_amalgamated.hpp>

#include "pynote/core/application/first_input_capture.h"
#include "pynote/core/domain/paragraph_parser.h"
#include "pynote/core/storage/migration_runner.h"
#include "pynote/core/storage/repositories.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
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
		const DWORD length = ::GetEnvironmentVariableW(L"PYNOTE_FIRST_INPUT_GOLDEN_OUT", path, 32768);
		if (length == 0 || length >= 32768) { return; }
		std::ofstream output(std::filesystem::path(path), std::ios::binary |
			(id == "WTL-W2-0077" ? std::ios::trunc : std::ios::app));
		REQUIRE(output.is_open());
		output << id << '|' << payload << '\n';
		REQUIRE(output.good());
	}

	std::string ids(const std::vector<domain::S_CARD>& cards)
	{
		std::string value;
		for (const auto& card : cards) { if (!value.empty()) { value += ','; } value += card.sId; }
		return value;
	}

	std::string bodies(const std::vector<domain::S_CARD>& cards)
	{
		std::string value;
		for (const auto& card : cards) { if (!value.empty()) { value += ','; } value += card.sBody; }
		return value;
	}

	std::string projection_ids(const domain::C_CARD_LIST_PROJECTION& projection)
	{
		std::string value;
		for (std::size_t row = 0; row < projection.RowCount(); ++row) {
			if (!value.empty()) { value += ','; }
			value += projection.CardAt(row)->sId;
		}
		return value;
	}

	class Fixture
	{
	public:
		Fixture()
			: path_(std::filesystem::temp_directory_path() /
				("noteex_w2r9_first_input_" + std::to_string(::GetCurrentProcessId()) + "_" +
				std::to_string(++sequence_) + ".db")), repositories_(database_)
		{
			remove_();
			REQUIRE(database_.Open(path_.string()));
			storage::C_MIGRATION_RUNNER runner;
			runner.SetExistingDatabase(false, path_.string());
			REQUIRE(runner.Run(database_) == storage::E_MIGRATE_RESULT::Ok);
			domain::S_DOCUMENT document;
			document.sId = DocumentId;
			document.sTitle = "first input";
			document.nCreatedAtUs = 1000;
			document.nUpdatedAtUs = 1000;
			REQUIRE(repositories_.CreateDocument(document) == storage::E_REPO_RESULT::Ok);
		}

		~Fixture() { database_.Close(); remove_(); }

		app::C_CARD_SERVICE Service()
		{
			return app::C_CARD_SERVICE(database_, repositories_, parser_,
				[this] { return ++clock_; }, [this] { return next_id_(); });
		}

		std::vector<domain::S_CARD> Cards()
		{
			std::vector<domain::S_CARD> cards;
			REQUIRE(repositories_.ListCards(DocumentId, &cards) == storage::E_REPO_RESULT::Ok);
			return cards;
		}

		domain::S_CAPTURE_OPERATION Operation(const domain::S_CARD& card)
		{
			domain::S_CAPTURE_OPERATION operation;
			REQUIRE(repositories_.GetCaptureOperation(card.sOperationId, &operation) == storage::E_REPO_RESULT::Ok);
			return operation;
		}

		inline static const std::string DocumentId = "document-first-input";

	private:
		std::string next_id_()
		{
			const int number = idCalls_ / 4 + 1;
			const int kind = idCalls_++ % 4;
			if (kind == 0) { return "operation-" + std::to_string(number); }
			if (kind == 1) { return "card-" + std::to_string(number); }
			if (kind == 2) { return "revision-" + std::to_string(number); }
			return "event-" + std::to_string(number);
		}

		void remove_() const
		{
			std::error_code error;
			std::filesystem::remove(path_, error);
			std::filesystem::remove(path_.string() + "-wal", error);
			std::filesystem::remove(path_.string() + "-shm", error);
		}

		std::filesystem::path path_;
		storage::C_DATABASE database_;
		storage::C_REPOSITORIES repositories_;
		domain::C_PARAGRAPH_PARSER parser_;
		std::int64_t clock_{ 1999 };
		int idCalls_{ 0 };
		inline static int sequence_{ 0 };
	};

	#define FIRST_INPUT_TAGS(ID) "[W2-R9][core][application][first-input][" ID "]"
}

TEST_CASE("WTL-W2-0077", FIRST_INPUT_TAGS("WTL-W2-0077"))
{
	Fixture fixture; auto service = fixture.Service(); domain::C_CARD_LIST_PROJECTION projection;
	app::C_FIRST_INPUT_CAPTURE capture(service, projection, Fixture::DocumentId);
	const std::vector<std::string> texts{
		"\xec\xb2\xab \xec\x88\x98\xec\xa7\x91",
		"\xeb\x91\x98\xec\xa7\xb8 \xec\x88\x98\xec\xa7\x91",
		"\xec\x85\x8b\xec\xa7\xb8 \xec\x88\x98\xec\xa7\x91" };
	for (std::size_t index = 0; index < texts.size(); ++index) {
		const auto result = capture.OnMeaningfulInsertion(texts[index], domain::E_CAPTURE_OPERATION_SOURCE::Paste);
		REQUIRE(result.bCreateAttempted); REQUIRE(result.eServiceOutcome == app::E_CARD_SERVICE_RESULT::Ok);
		REQUIRE(result.eEffect == app::E_FIRST_INPUT_EFFECT::ConnectCreatedCard);
		if (index + 1 < texts.size()) { capture.ResetAfterAcceptedClose(); }
	}
	const auto cards = fixture.Cards();
	REQUIRE(ids(cards) == "card-1,card-2,card-3"); REQUIRE(bodies(cards) == texts[0] + "," + texts[1] + "," + texts[2]);
	for (std::size_t index = 0; index < cards.size(); ++index) {
		REQUIRE(cards[index].eSource == domain::E_CARD_SOURCE::Paste);
		REQUIRE(fixture.Operation(cards[index]).sId == "operation-" + std::to_string(index + 1));
		REQUIRE(fixture.Operation(cards[index]).eSource == domain::E_CAPTURE_OPERATION_SOURCE::Paste);
	}
	REQUIRE(capture.ConnectedCardId() == "card-3");
	emit("WTL-W2-0077", "cards=card-1,card-2,card-3|bodies=ecb2ab20ec8898eca791,eb9198eca7b820ec8898eca791,ec858beca7b820ec8898eca791|sources=paste,paste,paste|operations=operation-1,operation-2,operation-3|connected=card-3");
}

TEST_CASE("WTL-W2-0078", FIRST_INPUT_TAGS("WTL-W2-0078"))
{
	Fixture fixture; auto service = fixture.Service(); domain::C_CARD_LIST_PROJECTION projection;
	app::C_FIRST_INPUT_CAPTURE capture(service, projection, Fixture::DocumentId);
	REQUIRE(capture.OnMeaningfulInsertion("first", domain::E_CAPTURE_OPERATION_SOURCE::Typing).eEffect == app::E_FIRST_INPUT_EFFECT::ConnectCreatedCard);
	capture.ResetAfterAcceptedClose();
	REQUIRE(capture.OnMeaningfulInsertion("second", domain::E_CAPTURE_OPERATION_SOURCE::Typing).eEffect == app::E_FIRST_INPUT_EFFECT::ConnectCreatedCard);
	const auto cards = fixture.Cards(); REQUIRE(bodies(cards) == "first,second");
	REQUIRE(cards[0].nPositionKey == 1024); REQUIRE(cards[1].nPositionKey == 2048);
	projection.SetSortMode(domain::E_CARD_LIST_SORT_MODE::Recency);
	REQUIRE(projection_ids(projection) == "card-2,card-1");
	REQUIRE(fixture.Operation(cards[0]).sId == "operation-1"); REQUIRE(fixture.Operation(cards[1]).sId == "operation-2");
	emit("WTL-W2-0078", "stored=first,second|recency=second,first|positions=1024,2048|operations=operation-1,operation-2");
}

TEST_CASE("WTL-W2-0079", FIRST_INPUT_TAGS("WTL-W2-0079"))
{
	Fixture fixture; auto service = fixture.Service(); domain::C_CARD_LIST_PROJECTION projection;
	projection.SetSourceFilter(std::set<domain::E_CARD_SOURCE>{domain::E_CARD_SOURCE::Typing});
	app::C_FIRST_INPUT_CAPTURE capture(service, projection, Fixture::DocumentId);
	const auto result = capture.OnMeaningfulInsertion("filtered paste", domain::E_CAPTURE_OPERATION_SOURCE::Paste);
	const auto cards = fixture.Cards(); REQUIRE(cards.size() == 1); REQUIRE(cards[0].sId == "card-1");
	REQUIRE(cards[0].eSource == domain::E_CARD_SOURCE::Paste); REQUIRE(result.sConnectedCardId == "card-1");
	REQUIRE_FALSE(projection.RowForCard("card-1"));
	emit("WTL-W2-0079", "filter=typing|card=card-1|source=paste|connected=card-1|row=-");
}

TEST_CASE("WTL-W2-0080", FIRST_INPUT_TAGS("WTL-W2-0080"))
{
	Fixture fixture; auto service = fixture.Service(); domain::C_CARD_LIST_PROJECTION projection;
	app::C_FIRST_INPUT_CAPTURE capture(service, projection, Fixture::DocumentId);
	std::string surface = "a";
	const auto first = capture.OnMeaningfulInsertion(surface, domain::E_CAPTURE_OPERATION_SOURCE::Typing);
	surface += "b";
	const auto later = capture.OnMeaningfulInsertion(surface, domain::E_CAPTURE_OPERATION_SOURCE::Paste);
	REQUIRE(first.bCreateAttempted); REQUIRE(first.sConnectedCardId == "card-1");
	REQUIRE_FALSE(later.bCreateAttempted); REQUIRE_FALSE(later.eServiceOutcome); REQUIRE(later.eEffect == app::E_FIRST_INPUT_EFFECT::AlreadyConnected);
	const auto cards = fixture.Cards(); REQUIRE(cards.size() == 1); REQUIRE(cards[0].sBody == "a");
	REQUIRE(cards[0].eSource == domain::E_CARD_SOURCE::Typing);
	REQUIRE(fixture.Operation(cards[0]).eSource == domain::E_CAPTURE_OPERATION_SOURCE::Typing);
	emit("WTL-W2-0080", "card=card-1|surface=6162|card_source=typing|operation_source=typing|cards=1|create_attempts=1");
}

#undef FIRST_INPUT_TAGS
