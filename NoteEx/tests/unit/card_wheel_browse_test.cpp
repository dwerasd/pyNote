#include <catch_amalgamated.hpp>

#include "pynote/core/domain/card_wheel_browse.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#pragma comment(lib, "NoteExCore")

namespace
{
	namespace domain = pynote::core::domain;

	domain::S_CARD card(int number)
	{
		domain::S_CARD value;
		value.sId = "wheel-" + std::to_string(number);
		value.sDocumentId = "document-1";
		value.sOperationId = "operation-1";
		value.nPositionKey = number * 1024;
		value.nCaptureSeq = number;
		value.nCreatedAtUs = number;
		value.nUpdatedAtUs = number;
		value.eSource = domain::E_CARD_SOURCE::Typing;
		value.sBody = "body";
		value.sBodyHash = "hash";
		value.sCurrentRevisionId = "revision-" + std::to_string(number);
		return value;
	}

	domain::C_CARD_LIST_PROJECTION projection(int count)
	{
		std::vector<domain::S_CARD> cards;
		for (int number = 1; number <= count; ++number) { cards.push_back(card(number)); }
		domain::C_CARD_LIST_PROJECTION value;
		value.SetCards(cards);
		value.SetSortMode(domain::E_CARD_LIST_SORT_MODE::Position);
		value.TakeDeltas();
		value.TakeSelectionDeltas();
		return value;
	}

	void select(domain::C_CARD_LIST_PROJECTION& projection, int number)
	{
		REQUIRE(projection.SelectVisibleCard(
			"wheel-" + std::to_string(number), domain::E_CARD_SELECTION_INTENT::Replace));
	}

	std::string selected(const domain::C_CARD_LIST_PROJECTION& projection)
	{
		std::string result;
		for (const auto& id : projection.SelectedCardIds()) {
			if (!result.empty()) { result += ','; }
			result += id;
		}
		return result.empty() ? "-" : result;
	}

	void emit(std::string_view id, std::string_view payload)
	{
		wchar_t path[32768] = {};
		const DWORD length = ::GetEnvironmentVariableW(L"PYNOTE_CARD_WHEEL_GOLDEN_OUT", path, 32768);
		if (length == 0 || length >= 32768) { return; }
		std::ofstream output(std::filesystem::path(path), std::ios::binary |
			(id == "WTL-W2-0044" ? std::ios::trunc : std::ios::app));
		REQUIRE(output.is_open());
		output << id << '|' << payload << '\n';
		REQUIRE(output.good());
	}

	#define WHEEL_TAGS(ID) "[W2-R6][core][domain][card-wheel-browse][" ID "]"
}

TEST_CASE("WTL-W2-0044", WHEEL_TAGS("WTL-W2-0044"))
{
	auto p = projection(10); select(p, 4);
	std::int64_t now = 0; std::vector<domain::S_CARD_WHEEL_TIMER_COMMAND> scheduled;
	domain::C_CARD_WHEEL_BROWSE browse(p, [&]{ return now; }, [&](const auto& command){ scheduled.push_back(command); });
	std::vector<std::size_t> down, up;
	for (int count = 0; count < 3; ++count) { down.push_back(*browse.OnVerticalAngle(-120).nCurrentRow); }
	const auto downSelected = selected(p);
	for (int count = 0; count < 3; ++count) { up.push_back(*browse.OnVerticalAngle(120).nCurrentRow); }
	REQUIRE((down == std::vector<std::size_t>{4,5,6}));
	REQUIRE((up == std::vector<std::size_t>{5,4,3}));
	REQUIRE(downSelected == "wheel-7"); REQUIRE(selected(p) == "wheel-4");
	REQUIRE(scheduled.size() == 6);
	emit("WTL-W2-0044", "down=4,5,6|down_selected=wheel-7|up=5,4,3|up_selected=wheel-4");
}

TEST_CASE("WTL-W2-0045", WHEEL_TAGS("WTL-W2-0045"))
{
	auto p = projection(5); select(p, 5);
	std::vector<domain::S_CARD_WHEEL_TIMER_COMMAND> scheduled;
	domain::C_CARD_WHEEL_BROWSE browse(p, []{ return std::int64_t{0}; }, [&](const auto& command){ scheduled.push_back(command); });
	const auto bottom = browse.OnVerticalAngle(-120); select(p, 1); const auto top = browse.OnVerticalAngle(120);
	REQUIRE(bottom.bHandled); REQUIRE(bottom.nCurrentRow == 4); REQUIRE(bottom.sPendingCardId == "wheel-5");
	REQUIRE(top.bHandled); REQUIRE(top.nCurrentRow == 0); REQUIRE(top.sPendingCardId == "wheel-1");
	REQUIRE(scheduled.size() == 2); REQUIRE(scheduled[0].eOperation == domain::E_CARD_WHEEL_TIMER_OPERATION::Arm); REQUIRE(scheduled[1].eOperation == domain::E_CARD_WHEEL_TIMER_OPERATION::Arm);
	emit("WTL-W2-0045", "bottom=4,wheel-5|top=0,wheel-1|wrapped=0");
}

TEST_CASE("WTL-W2-0046", WHEEL_TAGS("WTL-W2-0046"))
{
	auto p = projection(10); select(p, 1);
	domain::C_CARD_WHEEL_BROWSE browse(p, []{ return std::int64_t{0}; }, [](const auto&){});
	std::vector<std::size_t> rows; std::vector<int> remainders;
	for (int count = 0; count < 6; ++count) { const auto result = browse.OnVerticalAngle(-40); rows.push_back(*result.nCurrentRow); remainders.push_back(result.nAngleRemainder); }
	REQUIRE((rows == std::vector<std::size_t>{0,0,1,1,1,2}));
	REQUIRE((remainders == std::vector<int>{-40,-80,0,-40,-80,0}));
	emit("WTL-W2-0046", "rows=0,0,1,1,1,2|remainders=-40,-80,0,-40,-80,0");
}

TEST_CASE("WTL-W2-0047", WHEEL_TAGS("WTL-W2-0047"))
{
	auto p = projection(10); select(p, 5);
	domain::C_CARD_WHEEL_BROWSE browse(p, []{ return std::int64_t{0}; }, [](const auto&){});
	browse.OnVerticalAngle(-40); const auto before = browse.OnVerticalAngle(-40); const auto after = browse.OnVerticalAngle(120);
	REQUIRE(before.nCurrentRow == 4); REQUIRE(before.nAngleRemainder == -80);
	REQUIRE(after.nCurrentRow == 3); REQUIRE(after.nAngleRemainder == 0);
	emit("WTL-W2-0047", "before=4,-80|after=3,0|discarded=1");
}

TEST_CASE("WTL-W2-0048", WHEEL_TAGS("WTL-W2-0048"))
{
	auto p = projection(12); select(p, 1);
	std::int64_t now = 0; std::vector<domain::S_CARD_WHEEL_TIMER_COMMAND> scheduled;
	domain::C_CARD_WHEEL_BROWSE browse(p, [&]{ return now; }, [&](const auto& command){ scheduled.push_back(command); });
	std::vector<std::uint64_t> generations;
	for (int input = 0; input < 4; ++input) { now = input * 40; const auto result = browse.OnVerticalAngle(-120); generations.push_back(*result.nGeneration); }
	for (std::size_t index = 0; index + 1 < generations.size(); ++index) { REQUIRE_FALSE(browse.OnTimer(generations[index])); }
	const auto request = browse.OnTimer(generations.back()); REQUIRE(request); REQUIRE(request->sCardId == "wheel-5");
	REQUIRE_FALSE(browse.OnTimer(generations.back())); REQUIRE(scheduled.back().nDeadlineMs == now + 120);
	emit("WTL-W2-0048", "during=-|opened=wheel-5|count=1|quiet_ms=120");
}

TEST_CASE("WTL-W2-0049", WHEEL_TAGS("WTL-W2-0049"))
{
	auto p = projection(10); select(p, 1);
	std::vector<domain::S_CARD_WHEEL_TIMER_COMMAND> scheduled;
	domain::C_CARD_WHEEL_BROWSE browse(p, []{ return std::int64_t{0}; }, [&](const auto& command){ scheduled.push_back(command); });
	const auto result = browse.OnVerticalAngle(-360); const auto generation = *result.nGeneration;
	REQUIRE(result.sPendingCardId == "wheel-4"); browse.Cancel();
	REQUIRE_FALSE(browse.PendingCardId()); REQUIRE(browse.AngleRemainder() == 0); REQUIRE_FALSE(browse.OnTimer(generation));
	REQUIRE(scheduled.back().eOperation == domain::E_CARD_WHEEL_TIMER_OPERATION::Cancel);
	REQUIRE(scheduled.back().nGeneration == generation);
	emit("WTL-W2-0049", "pending=wheel-4|cancel=reset|opened=-|editor=-");
}

TEST_CASE("WTL-W2-0050", WHEEL_TAGS("WTL-W2-0050"))
{
	auto p = projection(12); select(p, 1);
	domain::C_CARD_WHEEL_BROWSE browse(p, []{ return std::int64_t{0}; }, [](const auto&){}); browse.SetEditorCardId("wheel-1");
	const auto result = browse.OnVerticalAngle(-480); REQUIRE(result.nCurrentRow == 4); const auto request = browse.OnTimer(*result.nGeneration); REQUIRE(request); REQUIRE(request->sCardId == "wheel-5");
	const auto effect = browse.CompleteOpen(*request, false);
	REQUIRE(effect == domain::E_CARD_WHEEL_FOCUS_EFFECT::FocusEditor); REQUIRE(p.RowForCard(*p.CurrentCardId()) == 0); REQUIRE(selected(p) == "wheel-1"); REQUIRE(browse.EditorCardId() == "wheel-1");
	emit("WTL-W2-0050", "browse_row=4|current_row=0|selected=wheel-1|editor=wheel-1|focus=editor");
}

TEST_CASE("WTL-W2-0051", WHEEL_TAGS("WTL-W2-0051"))
{
	auto p = projection(0); std::vector<domain::S_CARD_WHEEL_TIMER_COMMAND> scheduled;
	domain::C_CARD_WHEEL_BROWSE browse(p, []{ return std::int64_t{0}; }, [&](const auto& command){ scheduled.push_back(command); });
	const auto result = browse.OnVerticalAngle(-360); REQUIRE_FALSE(result.bHandled); REQUIRE_FALSE(result.sPendingCardId); REQUIRE_FALSE(p.CurrentCardId()); REQUIRE(scheduled.empty());
	emit("WTL-W2-0051", "handled=0|requested=-|current=-");
}

#undef WHEEL_TAGS
