#include <catch_amalgamated.hpp>

#include "pynote/core/application/drop_import_completion.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
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

	domain::S_CARD card(int number, domain::E_CARD_SOURCE source = domain::E_CARD_SOURCE::Import)
	{
		domain::S_CARD value;
		value.sId = "card-" + std::to_string(number);
		value.sDocumentId = "document-1";
		value.sOperationId = "operation-" + std::to_string(number);
		value.nPositionKey = number * 1024;
		value.nCaptureSeq = number;
		value.nCreatedAtUs = number;
		value.nUpdatedAtUs = number;
		value.eSource = source;
		value.sBody = "body";
		value.sBodyHash = "hash";
		value.sCurrentRevisionId = "revision-" + std::to_string(number);
		return value;
	}

	app::S_DROP_IMPORT_EXECUTION execution(std::vector<std::string> ids,
		app::E_DROP_IMPORT_POST_ACTION action = app::E_DROP_IMPORT_POST_ACTION::RevealLastCreated)
	{
		app::S_DROP_IMPORT_EXECUTION value;
		value.sWindowId = "window-1";
		value.sDocumentId = "document-1";
		value.sCorrelationId = "correlation-1";
		value.CreatedCardIds = std::move(ids);
		value.ePostAction = action;
		return value;
	}

	domain::C_CARD_LIST_PROJECTION projection(domain::E_CARD_LIST_SORT_MODE mode,
		bool withBase = true)
	{
		domain::C_CARD_LIST_PROJECTION value;
		if (withBase) { value.SetCards({card(1, domain::E_CARD_SOURCE::Typing)}); }
		value.SetSortMode(mode);
		if (withBase) { value.SelectVisibleCard("card-1", domain::E_CARD_SELECTION_INTENT::Replace); }
		value.TakeDeltas(); value.TakeSelectionDeltas();
		return value;
	}

	void emit(std::string_view id, std::string_view payload)
	{
		wchar_t path[32768] = {};
		const DWORD length = ::GetEnvironmentVariableW(L"PYNOTE_DROP_IMPORT_COMPLETION_GOLDEN_OUT", path, 32768);
		if (length == 0 || length >= 32768) { return; }
		std::ofstream output(std::filesystem::path(path), std::ios::binary |
			(id == "WTL-W2-0072" ? std::ios::trunc : std::ios::app));
		REQUIRE(output.is_open()); output << id << '|' << payload << '\n'; REQUIRE(output.good());
	}

	#define COMPLETION_TAGS(ID) "[W2-R8][core][application][drop-import-completion][" ID "]"
}

TEST_CASE("WTL-W2-0072", COMPLETION_TAGS("WTL-W2-0072"))
{
	auto p = projection(domain::E_CARD_LIST_SORT_MODE::Recency);
	p.SetSourceFilter(std::set<domain::E_CARD_SOURCE>{domain::E_CARD_SOURCE::Typing});
	p.TakeDeltas(); p.TakeSelectionDeltas();
	const auto beforeCurrent = p.CurrentCardId(); const auto beforeSelected = p.SelectedCardIds();
	const auto result = app::CompleteDropImport(execution({"card-2","card-3"}), {card(2),card(3)}, p);
	REQUIRE(result.nAddedCount == 2); REQUIRE(result.sTargetCardId == "card-3");
	REQUIRE(result.eEffect == app::E_DROP_IMPORT_LIST_EFFECT::PreserveHiddenTarget);
	REQUIRE_FALSE(p.RowForCard("card-3")); REQUIRE(p.CurrentCardId() == beforeCurrent); REQUIRE(p.SelectedCardIds() == beforeSelected); REQUIRE(p.RowCount() == 1);

	auto guarded = projection(domain::E_CARD_LIST_SORT_MODE::Recency); const auto rows = guarded.RowCount(); const auto deltas = guarded.Deltas().size();
	REQUIRE_THROWS_AS(app::CompleteDropImport(execution({"card-2","card-3"}), {card(2)}, guarded), std::invalid_argument);
	REQUIRE(guarded.RowCount() == rows); REQUIRE(guarded.Deltas().size() == deltas);
	REQUIRE_THROWS_AS(app::CompleteDropImport(execution({"card-2","wrong"}), {card(2),card(3)}, guarded), std::invalid_argument);
	REQUIRE(guarded.RowCount() == rows); REQUIRE(guarded.Deltas().size() == deltas);
	emit("WTL-W2-0072", "created=card-2,card-3|target=card-3|target_visible=0|current=card-1|visible_rows=1|filter=typing|reveal=0|editor=-");
}

TEST_CASE("WTL-W2-0073", COMPLETION_TAGS("WTL-W2-0073"))
{
	auto p = projection(domain::E_CARD_LIST_SORT_MODE::Recency, false);
	auto e = execution({"card-1","card-2"}); e.ReadFailures.push_back({"blank.txt",app::E_DROP_IMPORT_FAILURE_KIND::Blank,"blank"});
	const auto result = app::CompleteDropImport(e, {card(1),card(2)}, p);
	REQUIRE(result.sTargetCardId == "card-2"); REQUIRE(result.eEffect == app::E_DROP_IMPORT_LIST_EFFECT::RevealVisibleTarget); REQUIRE(p.RowForCard(*result.sTargetCardId) == 0); REQUIRE(p.CurrentCardId() == "card-2"); REQUIRE(p.SelectedCardIds() == std::vector<std::string>{"card-2"}); REQUIRE(e.ReadFailures.size() == 1);
	emit("WTL-W2-0073", "created=card-1,card-2|final_input=blank|target=card-2|target_row=0|current=card-2|read_failures=1|reveal=1|editor=-");
}

TEST_CASE("WTL-W2-0074", COMPLETION_TAGS("WTL-W2-0074"))
{
	auto p = projection(domain::E_CARD_LIST_SORT_MODE::Recency); const auto result=app::CompleteDropImport(execution({"card-2","card-3"}),{card(2),card(3)},p); REQUIRE(result.sTargetCardId=="card-3");REQUIRE(result.eEffect==app::E_DROP_IMPORT_LIST_EFFECT::RevealVisibleTarget);REQUIRE(p.RowForCard(*result.sTargetCardId)==0);REQUIRE(p.CurrentCardId()=="card-3");REQUIRE(p.SelectedCardIds()==std::vector<std::string>{"card-3"});emit("WTL-W2-0074","sort=recency|created=card-2,card-3|target=card-3|target_row=0|current=card-3|reveal=1|editor=-");
}

TEST_CASE("WTL-W2-0075", COMPLETION_TAGS("WTL-W2-0075"))
{
	auto p = projection(domain::E_CARD_LIST_SORT_MODE::Position); const auto result=app::CompleteDropImport(execution({"card-2","card-3"}),{card(2),card(3)},p); REQUIRE(result.sTargetCardId=="card-3");REQUIRE(result.eEffect==app::E_DROP_IMPORT_LIST_EFFECT::RevealVisibleTarget);REQUIRE(p.RowForCard(*result.sTargetCardId)==2);REQUIRE(p.CurrentCardId()=="card-3");REQUIRE(p.SelectedCardIds()==std::vector<std::string>{"card-3"});emit("WTL-W2-0075","sort=position|created=card-2,card-3|target=card-3|target_row=2|current=card-3|reveal=1|editor=-");
}

TEST_CASE("WTL-W2-0076", COMPLETION_TAGS("WTL-W2-0076"))
{
	auto p = projection(domain::E_CARD_LIST_SORT_MODE::Capture); const auto result=app::CompleteDropImport(execution({"card-2","card-3"}),{card(2),card(3)},p); REQUIRE(result.sTargetCardId=="card-3");REQUIRE(result.eEffect==app::E_DROP_IMPORT_LIST_EFFECT::RevealVisibleTarget);REQUIRE(p.RowForCard(*result.sTargetCardId)==2);REQUIRE(p.CurrentCardId()=="card-3");REQUIRE(p.SelectedCardIds()==std::vector<std::string>{"card-3"});emit("WTL-W2-0076","sort=capture|created=card-2,card-3|target=card-3|target_row=2|current=card-3|reveal=1|editor=-");
}

#undef COMPLETION_TAGS
