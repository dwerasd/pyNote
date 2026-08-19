#include <catch_amalgamated.hpp>

#include "pynote/core/domain/card_list_projection.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
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
	namespace domain=pynote::core::domain;
	std::string u8s(const char8_t* value){return std::string(reinterpret_cast<const char*>(value));}
	std::string hex(std::string_view text){std::ostringstream s;s<<std::hex<<std::setfill('0');for(const unsigned char c:text)s<<std::setw(2)<<static_cast<unsigned int>(c);return s.str();}
	std::string repeat(std::string_view value,std::size_t count){std::string result;result.reserve(value.size()*count);for(std::size_t i=0;i<count;++i)result+=value;return result;}
	domain::S_CARD card(int number,std::string body="",domain::E_CARD_SOURCE source=domain::E_CARD_SOURCE::Typing)
	{
		domain::S_CARD c;c.sId="card-"+std::to_string(number);c.sDocumentId="document-1";c.sOperationId="operation-"+std::to_string(number);
		c.nPositionKey=number*1024;c.nCaptureSeq=number;c.nCreatedAtUs=1000000+number;c.nUpdatedAtUs=1000000+number;c.eSource=source;
		c.sBody=body.empty()?u8s(u8"카드 ")+std::to_string(number):std::move(body);c.sBodyHash="hash";c.sCurrentRevisionId="revision-"+std::to_string(number);return c;
	}
	std::string ids(const domain::C_CARD_LIST_PROJECTION& projection){std::string result;for(std::size_t row=0;row<projection.RowCount();++row){if(!result.empty())result+=',';result+=projection.CardAt(row)->sId;}return result.empty()?"-":result;}
	std::string selected(const std::vector<std::string>& values){std::string result;for(const auto& value:values){if(!result.empty())result+=',';result+=value;}return result.empty()?"-":result;}
	std::string delta_fields(const domain::S_CARD_LIST_DELTA& delta){std::string result;for(const auto field:delta.Fields){if(!result.empty())result+=',';result+=domain::TraceName(field);}return result.empty()?"-":result;}
	std::string delta_kind(domain::E_CARD_LIST_DELTA_KIND kind){switch(kind){case domain::E_CARD_LIST_DELTA_KIND::Reset:return"reset";case domain::E_CARD_LIST_DELTA_KIND::Insert:return"insert";case domain::E_CARD_LIST_DELTA_KIND::Move:return"move";default:return"update";}}
	std::string deltas(const std::vector<domain::S_CARD_LIST_DELTA>& values){std::string result;for(const auto& d:values){if(!result.empty())result+=';';result+=delta_kind(d.eKind)+":"+(d.nOldRow?std::to_string(*d.nOldRow):"-")+":"+(d.nNewRow?std::to_string(*d.nNewRow):"-")+":"+delta_fields(d);}return result.empty()?"-":result;}
	void emit(std::string_view id,std::string_view payload)
	{
		wchar_t path[32768]={};const DWORD length=::GetEnvironmentVariableW(L"PYNOTE_CARD_LIST_GOLDEN_OUT",path,32768);if(length==0||length>=32768)return;
		std::ofstream output(std::filesystem::path(path),std::ios::binary|(id=="WTL-W2-0029"?std::ios::trunc:std::ios::app));
		REQUIRE(output.is_open());output<<id<<'|'<<payload<<'\n';REQUIRE(output.good());
	}
}

#define TAGS(ID) "[W2-R4][core][domain][card-list-projection][" ID "]"

TEST_CASE("WTL-W2-0029",TAGS("WTL-W2-0029"))
{
	const std::string body=u8s(u8"첫 줄\n둘째 줄");domain::C_CARD_LIST_PROJECTION p;p.SetCards({card(1,body)});p.TakeDeltas();const auto preview=p.PreviewForCard("card-1");
	REQUIRE(p.FullBodyForCard("card-1")==body);REQUIRE(preview->sText==body);REQUIRE_FALSE(preview->bTruncated);
	emit("WTL-W2-0029","full="+hex(body)+"|preview="+hex(preview->sText)+"|truncated=0|expanded=absent");
}

TEST_CASE("WTL-W2-0030",TAGS("WTL-W2-0030"))
{
	auto first=card(1,"",domain::E_CARD_SOURCE::Paste);auto middle=card(3,"",domain::E_CARD_SOURCE::Import);middle.nPositionKey=2048;middle.nCaptureSeq=3;middle.nUpdatedAtUs=2000000;auto last=card(2);last.nPositionKey=3072;last.nCaptureSeq=2;
	auto deleted=card(4,"",domain::E_CARD_SOURCE::Import);deleted.nDeletedAtUs=3;deleted.nUpdatedAtUs=3000000;domain::C_CARD_LIST_PROJECTION p;p.SetCards({first,middle,last,deleted});p.TakeDeltas();const auto recency=ids(p);p.SetSortMode(domain::E_CARD_LIST_SORT_MODE::Position);const auto position=ids(p);p.SetSortMode(domain::E_CARD_LIST_SORT_MODE::Capture);const auto capture=ids(p);const auto number=p.PositionNumber("card-3");p.SetSourceFilter(std::set<domain::E_CARD_SOURCE>{domain::E_CARD_SOURCE::Import});
	REQUIRE(recency=="card-3,card-2,card-1");REQUIRE(position=="card-1,card-3,card-2");REQUIRE(capture=="card-1,card-2,card-3");REQUIRE(number==2);REQUIRE(ids(p)=="card-3");
	emit("WTL-W2-0030","recency="+recency+"|position="+position+"|capture="+capture+"|middle_position=2|filter="+ids(p));
}

TEST_CASE("WTL-W2-0031",TAGS("WTL-W2-0031"))
{
	auto a=card(1),b=card(2),c=card(3);a.nUpdatedAtUs=b.nUpdatedAtUs=c.nUpdatedAtUs=2000000;domain::C_CARD_LIST_PROJECTION p;p.SetCards({a,b,c});p.TakeDeltas();REQUIRE(ids(p)=="card-3,card-2,card-1");emit("WTL-W2-0031","capture=3,2,1");
}

TEST_CASE("WTL-W2-0032",TAGS("WTL-W2-0032"))
{
	auto first=card(1),second=card(2);domain::C_CARD_LIST_PROJECTION p;p.SetCards({first,second});p.TakeDeltas();p.SetCurrentCardId(first.sId);p.SetSelectedCardIds({first.sId});first.nUpdatedAtUs=2000000;first.sBody=u8s(u8"수정된 첫 카드");REQUIRE(p.UpdateCard(first));const auto changes=p.TakeDeltas();
	REQUIRE(ids(p)=="card-1,card-2");REQUIRE(changes[0].eKind==domain::E_CARD_LIST_DELTA_KIND::Move);REQUIRE(changes[0].nOldRow==1);REQUIRE(changes[0].nNewRow==0);REQUIRE(p.CurrentCardId()==first.sId);REQUIRE((p.SelectedCardIds()==std::vector<std::string>{first.sId}));
	emit("WTL-W2-0032","order="+ids(p)+"|deltas="+deltas(changes)+"|current="+*p.CurrentCardId()+"|selected="+selected(p.SelectedCardIds())+"|reset=0");
}

TEST_CASE("WTL-W2-0033",TAGS("WTL-W2-0033"))
{
	domain::C_CARD_LIST_PROJECTION p;p.SetCards({card(1)});p.TakeDeltas();REQUIRE(p.AddCard(card(2)));const auto changes=p.TakeDeltas();REQUIRE(ids(p)=="card-2,card-1");REQUIRE(changes.size()==1);REQUIRE(changes[0].eKind==domain::E_CARD_LIST_DELTA_KIND::Insert);REQUIRE(changes[0].nNewRow==0);emit("WTL-W2-0033","order="+ids(p)+"|deltas="+deltas(changes)+"|reset=0");
}

TEST_CASE("WTL-W2-0034",TAGS("WTL-W2-0034"))
{
	domain::C_CARD_LIST_PROJECTION p;std::string policy;for(const auto mode:{domain::E_CARD_LIST_SORT_MODE::Recency,domain::E_CARD_LIST_SORT_MODE::Capture,domain::E_CARD_LIST_SORT_MODE::Position}){p.SetSortMode(mode);if(!policy.empty())policy+=';';policy+=(p.CanDragOut()?"1":"0")+std::string(",")+(p.CanInternalReorder()?"1":"0")+","+(p.CanAcceptInternalDrop()?"1":"0");}REQUIRE(policy=="1,0,0;1,0,0;1,1,1");emit("WTL-W2-0034","policy="+policy);
}

TEST_CASE("WTL-W2-0035",TAGS("WTL-W2-0035"))
{
	domain::C_CARD_LIST_PROJECTION p;p.SetCards({card(1),card(2),card(3)});p.TakeDeltas();p.SetCardDirty("card-2",true);p.SetCardDirty("card-2",true);p.SetCardDirty("card-2",false);const auto changes=p.TakeDeltas();REQUIRE(changes.size()==2);REQUIRE(changes[0].nNewRow==1);REQUIRE(changes[0].Fields==std::vector<domain::E_CARD_LIST_DELTA_FIELD>{domain::E_CARD_LIST_DELTA_FIELD::DirtyDraft});emit("WTL-W2-0035","deltas="+deltas(changes));
}

TEST_CASE("WTL-W2-0036",TAGS("WTL-W2-0036")){REQUIRE(domain::PREVIEW_CODEPOINT_BUDGET_PER_LINE==4096);emit("WTL-W2-0036","per_line=4096");}
TEST_CASE("WTL-W2-0037",TAGS("WTL-W2-0037")){REQUIRE(domain::TraceName(domain::E_CARD_LIST_DELTA_FIELD::PreviewTruncated)=="previewTruncated");emit("WTL-W2-0037","trace=previewTruncated");}

TEST_CASE("WTL-W2-0038",TAGS("WTL-W2-0038"))
{
	domain::C_CARD_LIST_PROJECTION p;const auto budget=p.PreviewBudget();const std::string body=u8s(u8"머리")+repeat(u8s(u8"가"),budget)+u8s(u8"꼬리");p.SetCards({card(1,body)});const auto preview=p.PreviewForCard("card-1");REQUIRE(budget==16384);REQUIRE(preview->nCodepointsExamined==budget);REQUIRE(preview->bTruncated);REQUIRE(p.FullBodyForCard("card-1")==body);emit("WTL-W2-0038","budget="+std::to_string(budget)+"|preview="+hex(preview->sText)+"|truncated=1|examined="+std::to_string(preview->nCodepointsExamined)+"|bytes="+std::to_string(preview->nBytesConsumed));
}

TEST_CASE("WTL-W2-0039",TAGS("WTL-W2-0039"))
{
	const std::string body=u8s(u8"한 줄\n두 줄");domain::C_CARD_LIST_PROJECTION p;p.SetCards({card(1,body)});const auto preview=p.PreviewForCard("card-1");REQUIRE(preview->sText==body);REQUIRE_FALSE(preview->bTruncated);emit("WTL-W2-0039","preview="+hex(preview->sText)+"|truncated=0|examined="+std::to_string(preview->nCodepointsExamined));
}

TEST_CASE("WTL-W2-0040",TAGS("WTL-W2-0040"))
{
	domain::C_CARD_LIST_PROJECTION p;auto value=card(1,u8s(u8"짧은 본문"));p.SetCards({value});p.TakeDeltas();REQUIRE_FALSE(p.PreviewForCard(value.sId)->bTruncated);value.sBody=repeat(u8s(u8"가"),p.PreviewBudget()+1);REQUIRE(p.UpdateCard(value));const auto changes=p.TakeDeltas();REQUIRE(changes.size()==1);REQUIRE(changes[0].eKind==domain::E_CARD_LIST_DELTA_KIND::Update);REQUIRE(std::find(changes[0].Fields.begin(),changes[0].Fields.end(),domain::E_CARD_LIST_DELTA_FIELD::Preview)!=changes[0].Fields.end());REQUIRE(std::find(changes[0].Fields.begin(),changes[0].Fields.end(),domain::E_CARD_LIST_DELTA_FIELD::PreviewTruncated)!=changes[0].Fields.end());REQUIRE(p.PreviewForCard(value.sId)->bTruncated);emit("WTL-W2-0040","before=0|after=1|deltas="+deltas(changes));
}

TEST_CASE("WTL-W2-0041",TAGS("WTL-W2-0041"))
{
	const std::string body=repeat(u8s(u8"가"),10*domain::PREVIEW_CODEPOINT_BUDGET_PER_LINE);domain::C_CARD_LIST_PROJECTION p;p.SetCards({card(1,body)});p.TakeDeltas();const auto three=p.PreviewForCard("card-1");p.SetPreviewLineCount(9);const auto nine=p.PreviewForCard("card-1");const auto changes=p.TakeDeltas();REQUIRE(three->nCodepointsExamined==16384);REQUIRE(three->bTruncated);REQUIRE(nine->nCodepointsExamined==40960);REQUIRE_FALSE(nine->bTruncated);REQUIRE(changes.size()==1);REQUIRE(changes[0].eKind==domain::E_CARD_LIST_DELTA_KIND::Reset);emit("WTL-W2-0041","three=16384,1|nine=40960,0|deltas="+deltas(changes)+"|preview="+hex(nine->sText));
}

TEST_CASE("WTL-W2-0042",TAGS("WTL-W2-0042"))
{
	domain::C_CARD_LIST_PROJECTION p;const auto budget=p.PreviewBudget();const std::string tail=u8s(u8"감시 대상 꼬리");const std::string body=u8s(u8"머리\n")+repeat(u8s(u8"가"),budget)+"\n"+tail;p.SetCards({card(1,body)});const auto preview=p.PreviewForCard("card-1");REQUIRE(preview->nCodepointsExamined<=budget);REQUIRE(preview->nBytesConsumed==preview->sText.size());REQUIRE(preview->sText.find(tail)==std::string::npos);emit("WTL-W2-0042","budget="+std::to_string(budget)+"|preview="+hex(preview->sText)+"|truncated=1|examined="+std::to_string(preview->nCodepointsExamined)+"|bytes="+std::to_string(preview->nBytesConsumed)+"|tail=absent");
}

TEST_CASE("WTL-W2-0043",TAGS("WTL-W2-0043"))
{
	domain::C_CARD_LIST_PROJECTION p;const auto budget=p.PreviewBudget();const std::string emoji=u8s(u8"😀");const std::string replacement=u8s(u8"�");const std::string body=repeat(u8s(u8"가"),budget-1)+emoji+u8s(u8"꼬리");p.SetCards({card(1,body)});const auto preview=p.PreviewForCard("card-1");REQUIRE(preview->nCodepointsExamined==budget);REQUIRE(preview->sText.ends_with(emoji));REQUIRE(preview->sText.find(replacement)==std::string::npos);emit("WTL-W2-0043","budget="+std::to_string(budget)+"|preview="+hex(preview->sText)+"|truncated=1|examined="+std::to_string(preview->nCodepointsExamined)+"|bytes="+std::to_string(preview->nBytesConsumed)+"|emoji=final");
}

#undef TAGS
