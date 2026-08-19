#include <catch_amalgamated.hpp>

#include "pynote/core/domain/card_drag_session_registry.h"
#include "pynote/core/domain/card_list_projection.h"

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
	namespace domain=pynote::core::domain;
	domain::S_CARD card(int number)
	{
		domain::S_CARD value;value.sId="multi-"+std::to_string(number);value.sDocumentId="document-1";value.sOperationId="operation-1";
		value.nPositionKey=number*1024;value.nCaptureSeq=number;value.nCreatedAtUs=number;value.nUpdatedAtUs=number;
		value.eSource=domain::E_CARD_SOURCE::Typing;value.sBody="body";value.sBodyHash="hash";value.sCurrentRevisionId="revision-"+std::to_string(number);return value;
	}
	std::vector<domain::S_CARD> cards(){std::vector<domain::S_CARD> result;for(int i=1;i<=6;++i)result.push_back(card(i));return result;}
	domain::C_CARD_LIST_PROJECTION projection(){domain::C_CARD_LIST_PROJECTION value;value.SetCards(cards());value.SetSortMode(domain::E_CARD_LIST_SORT_MODE::Position);value.TakeDeltas();return value;}
	std::string selection(const domain::C_CARD_LIST_PROJECTION& value){std::string result;for(const auto& id:value.SelectedCardIds()){if(!result.empty())result+=',';result+=id;}return result.empty()?"-":result;}
	std::string mode(const domain::C_CARD_LIST_PROJECTION& value){return value.SelectionMode()==domain::E_CARD_SELECTION_MODE::Single?"single":"extended";}
	void emit(std::string_view id,std::string_view payload)
	{
		wchar_t path[32768]={};const DWORD length=::GetEnvironmentVariableW(L"PYNOTE_CARD_SELECTION_DRAG_GOLDEN_OUT",path,32768);if(length==0||length>=32768)return;
		std::ofstream output(std::filesystem::path(path),std::ios::binary|(id=="WTL-W2-0019"?std::ios::trunc:std::ios::app));REQUIRE(output.is_open());output<<id<<'|'<<payload<<'\n';REQUIRE(output.good());
	}
}

TEST_CASE("WTL-W2-0019","[W2-R5][core][domain][card-drag][WTL-W2-0019]")
{
	domain::C_CARD_DRAG_SESSION_REGISTRY registry([]{return 77;});auto authentic=registry.RegisterSource(0x111);auto spoof=registry.RegisterSource(0x222);
	static_cast<void>(spoof);
	const auto token=registry.BeginSession(0x111,"card-1",std::optional<std::string>("revision-1"));REQUIRE(token);REQUIRE(*token!=0);REQUIRE(registry.Validate(*token,0x111,"card-1",std::optional<std::string>("revision-1")));
	std::vector<std::string> deleted,moved;std::vector<int> trash,reorder;
	for(int phase=0;phase<3;++phase){const bool trashAllowed=registry.Validate(*token,0x222,"card-1",std::optional<std::string>("revision-1"));trash.push_back(trashAllowed);if(trashAllowed)deleted.push_back("card-1");const bool reorderAllowed=registry.Validate(*token,0x222,"card-1",std::optional<std::string>("revision-1"));reorder.push_back(reorderAllowed);if(reorderAllowed)moved.push_back("card-1");}
	REQUIRE((trash==std::vector<int>{0,0,0}));REQUIRE((reorder==std::vector<int>{0,0,0}));REQUIRE(deleted.empty());REQUIRE(moved.empty());
	REQUIRE_FALSE(registry.Validate(*token,0x111,"other",std::optional<std::string>("revision-1")));REQUIRE_FALSE(registry.Validate(*token,0x111,"card-1",std::optional<std::string>("other")));
	authentic.Reset();REQUIRE_FALSE(registry.Validate(*token,0x111,"card-1",std::optional<std::string>("revision-1")));
	emit("WTL-W2-0019","trash=0,0,0|reorder=0,0,0|deleted=-|moved=-");
}

#define SELECTION_TAGS(ID) "[W2-R5][core][domain][card-selection][" ID "]"

TEST_CASE("WTL-W2-0020",SELECTION_TAGS("WTL-W2-0020"))
{
	auto p=projection();REQUIRE_FALSE(p.MultiSelectionEnabled());for(int i=1;i<=4;++i)REQUIRE(p.SelectVisibleCard("multi-"+std::to_string(i),domain::E_CARD_SELECTION_INTENT::Replace));REQUIRE(p.SelectedCardIds()==std::vector<std::string>{"multi-4"});emit("WTL-W2-0020","default_key=1|enabled=0|mode="+mode(p)+"|selected="+selection(p));
}

TEST_CASE("WTL-W2-0021",SELECTION_TAGS("WTL-W2-0021")){domain::C_CARD_LIST_PROJECTION p;REQUIRE(p.SelectionMode()==domain::E_CARD_SELECTION_MODE::Single);emit("WTL-W2-0021","mode="+mode(p));}

TEST_CASE("WTL-W2-0022",SELECTION_TAGS("WTL-W2-0022"))
{
	auto p=projection();for(int i=1;i<=3;++i)REQUIRE(p.SelectVisibleCard("multi-"+std::to_string(i),domain::E_CARD_SELECTION_INTENT::Additive));REQUIRE(p.SelectedCardIds()==std::vector<std::string>{"multi-3"});emit("WTL-W2-0022","mode="+mode(p)+"|selected="+selection(p));
}

TEST_CASE("WTL-W2-0023",SELECTION_TAGS("WTL-W2-0023"))
{
	auto p=projection();p.SetMultiSelectionEnabled(true);REQUIRE(p.SelectVisibleCard("multi-2",domain::E_CARD_SELECTION_INTENT::Replace));REQUIRE(p.SelectVisibleCard("multi-4",domain::E_CARD_SELECTION_INTENT::RangeLike));REQUIRE((p.SelectedCardIds()==std::vector<std::string>{"multi-2","multi-3","multi-4"}));emit("WTL-W2-0023","mode="+mode(p)+"|selected="+selection(p));
}

TEST_CASE("WTL-W2-0024",SELECTION_TAGS("WTL-W2-0024"))
{
	auto p=projection();p.SetMultiSelectionEnabled(true);p.SelectVisibleCard("multi-2",domain::E_CARD_SELECTION_INTENT::Replace);p.SelectVisibleCard("multi-4",domain::E_CARD_SELECTION_INTENT::RangeLike);p.SetCurrentCardId("multi-3");p.SetMultiSelectionEnabled(false);REQUIRE(p.CurrentCardId()==std::optional<std::string>("multi-3"));REQUIRE(p.SelectedCardIds()==std::vector<std::string>{"multi-3"});p.TakeSelectionDeltas();auto moved=card(3);moved.nPositionKey=1;REQUIRE(p.UpdateCard(moved));REQUIRE(p.TakeSelectionDeltas().empty());emit("WTL-W2-0024","mode="+mode(p)+"|current="+*p.CurrentCardId()+"|selected="+selection(p));
}

TEST_CASE("WTL-W2-0025",SELECTION_TAGS("WTL-W2-0025"))
{
	auto p=projection();p.SetMultiSelectionEnabled(true);p.SelectVisibleCard("multi-3",domain::E_CARD_SELECTION_INTENT::Replace);p.SelectVisibleCard("multi-5",domain::E_CARD_SELECTION_INTENT::Additive);p.SetCurrentCardId("multi-6");p.SetMultiSelectionEnabled(false);REQUIRE(p.CurrentCardId()==std::optional<std::string>("multi-3"));REQUIRE(p.SelectedCardIds()==std::vector<std::string>{"multi-3"});emit("WTL-W2-0025","mode="+mode(p)+"|current="+*p.CurrentCardId()+"|selected="+selection(p));
}

TEST_CASE("WTL-W2-0026",SELECTION_TAGS("WTL-W2-0026"))
{
	auto p=projection();p.SelectVisibleCard("multi-1",domain::E_CARD_SELECTION_INTENT::Replace);p.SelectVisibleCard("multi-2",domain::E_CARD_SELECTION_INTENT::Additive);const auto afterCtrl=p.SelectedCardIds().size();p.SelectVisibleCard("multi-4",domain::E_CARD_SELECTION_INTENT::RangeLike);const auto afterShift=p.SelectedCardIds().size();REQUIRE(afterCtrl==1);REQUIRE(afterShift==1);emit("WTL-W2-0026","after_ctrl_count=1|after_shift_count=1");
}

TEST_CASE("WTL-W2-0027",SELECTION_TAGS("WTL-W2-0027"))
{
	auto p=projection();p.SetMultiSelectionEnabled(true);p.SelectVisibleCard("multi-2",domain::E_CARD_SELECTION_INTENT::Replace);p.SelectVisibleCard("multi-4",domain::E_CARD_SELECTION_INTENT::RangeLike);p.SetMultiSelectionEnabled(false);const auto command=p.CopySelectionForCommand();REQUIRE(command==std::vector<std::string>{"multi-4"});REQUIRE(selection(p)=="multi-4");emit("WTL-W2-0027","delete=multi-4|count=1");
}

TEST_CASE("WTL-W2-0028",SELECTION_TAGS("WTL-W2-0028"))
{
	auto p=projection();p.SelectVisibleCard("multi-1",domain::E_CARD_SELECTION_INTENT::Replace);REQUIRE(p.MoveCurrentBy(1));REQUIRE(p.RowForCard(*p.CurrentCardId())==1);REQUIRE(p.SelectedCardIds()==std::vector<std::string>{"multi-2"});emit("WTL-W2-0028","row=1|current="+*p.CurrentCardId()+"|selected="+selection(p));
}

#undef SELECTION_TAGS
