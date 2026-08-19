#include "pynote/core/domain/card_list_projection.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace pynote::core::domain
{
	namespace
	{
		std::size_t scalar_length(unsigned char lead) noexcept
		{
			if (lead < 0x80) { return 1; }
			if ((lead & 0xE0) == 0xC0) { return 2; }
			if ((lead & 0xF0) == 0xE0) { return 3; }
			return 4;
		}

		S_CARD_PREVIEW bounded_preview(std::string_view body, std::size_t budget)
		{
			std::size_t offset = 0, examined = 0;
			while (offset < body.size() && examined < budget) {
				offset += scalar_length(static_cast<unsigned char>(body[offset]));
				++examined;
			}
			return {std::string(body.substr(0,offset)),offset<body.size(),examined,offset};
		}
	}

	std::string_view TraceName(E_CARD_LIST_DELTA_FIELD field) noexcept
	{
		switch (field) {
		case E_CARD_LIST_DELTA_FIELD::Card: return "card";
		case E_CARD_LIST_DELTA_FIELD::Preview: return "preview";
		case E_CARD_LIST_DELTA_FIELD::PreviewTruncated: return "previewTruncated";
		default: return "dirtyDraft";
		}
	}

	C_CARD_LIST_PROJECTION::C_CARD_LIST_PROJECTION(std::size_t lineCount)
		: m_nPreviewLineCount(lineCount)
	{
		if (lineCount < 1) { throw std::invalid_argument("preview line count must be positive"); }
	}

	void C_CARD_LIST_PROJECTION::SetCards(const std::vector<S_CARD>& cards)
	{
		m_Cards.clear();
		for (const auto& card : cards) { if (!card.nDeletedAtUs.has_value()) { m_Cards.push_back(card); } }
		this->rebuild_visible_();
		m_Deltas.push_back({E_CARD_LIST_DELTA_KIND::Reset,std::nullopt,std::nullopt,{}});
	}

	bool C_CARD_LIST_PROJECTION::AddCard(const S_CARD& card)
	{
		return this->AddCards({card}) == 1;
	}

	std::size_t C_CARD_LIST_PROJECTION::AddCards(const std::vector<S_CARD>& cards)
	{
		std::set<std::string> existing;
		for (const auto& card : m_Cards) { existing.insert(card.sId); }
		std::vector<std::string> added;
		for (const auto& card : cards) {
			if (card.nDeletedAtUs.has_value() || existing.contains(card.sId)) { continue; }
			existing.insert(card.sId); added.push_back(card.sId); m_Cards.push_back(card);
		}
		if (added.empty()) { return 0; }
		this->rebuild_visible_();
		for (std::size_t row=0; row<m_Visible.size(); ++row) {
			if (std::find(added.begin(),added.end(),m_Visible[row]->sId)!=added.end()) {
				m_Deltas.push_back({E_CARD_LIST_DELTA_KIND::Insert,std::nullopt,row,{E_CARD_LIST_DELTA_FIELD::Card}});
			}
		}
		return added.size();
	}

	bool C_CARD_LIST_PROJECTION::UpdateCard(const S_CARD& card)
	{
		const auto oldRow=this->RowForCard(card.sId);
		auto found=std::find_if(m_Cards.begin(),m_Cards.end(),[&](const auto& value){return value.sId==card.sId;});
		if(found==m_Cards.end())return false;
		if(card.nDeletedAtUs.has_value()) { m_Cards.erase(found); this->rebuild_visible_(); m_Deltas.push_back({E_CARD_LIST_DELTA_KIND::Reset,{},{},{}}); return true; }
		*found=card; this->rebuild_visible_(); const auto newRow=this->RowForCard(card.sId);
		if(oldRow&&newRow&&*oldRow!=*newRow)m_Deltas.push_back({E_CARD_LIST_DELTA_KIND::Move,*oldRow,*newRow,{E_CARD_LIST_DELTA_FIELD::Card}});
		if(newRow)m_Deltas.push_back({E_CARD_LIST_DELTA_KIND::Update,*newRow,*newRow,
			{E_CARD_LIST_DELTA_FIELD::Card,E_CARD_LIST_DELTA_FIELD::Preview,E_CARD_LIST_DELTA_FIELD::PreviewTruncated}});
		return true;
	}

	void C_CARD_LIST_PROJECTION::SetSortMode(E_CARD_LIST_SORT_MODE mode)
	{
		if(mode==m_eSortMode)return; m_eSortMode=mode; this->rebuild_visible_();
		m_Deltas.push_back({E_CARD_LIST_DELTA_KIND::Reset,{},{},{}});
	}

	void C_CARD_LIST_PROJECTION::SetSourceFilter(const std::optional<std::set<E_CARD_SOURCE>>& sources)
	{
		if(sources==m_SourceFilter)return; m_SourceFilter=sources; this->rebuild_visible_();
		m_Deltas.push_back({E_CARD_LIST_DELTA_KIND::Reset,{},{},{}});
	}

	void C_CARD_LIST_PROJECTION::SetPreviewLineCount(std::size_t count)
	{
		if(count<1)throw std::invalid_argument("preview line count must be positive");
		if(count==m_nPreviewLineCount)return; m_nPreviewLineCount=count;
		m_Deltas.push_back({E_CARD_LIST_DELTA_KIND::Reset,{},{},{}});
	}

	std::size_t C_CARD_LIST_PROJECTION::PreviewBudget() const noexcept
	{
		return (m_nPreviewLineCount+1)*PREVIEW_CODEPOINT_BUDGET_PER_LINE;
	}

	const S_CARD* C_CARD_LIST_PROJECTION::CardAt(std::size_t row) const noexcept
	{
		return row<m_Visible.size()?m_Visible[row]:nullptr;
	}

	std::optional<std::size_t> C_CARD_LIST_PROJECTION::RowForCard(std::string_view id) const noexcept
	{
		for(std::size_t row=0;row<m_Visible.size();++row)if(m_Visible[row]->sId==id)return row; return std::nullopt;
	}

	std::optional<std::size_t> C_CARD_LIST_PROJECTION::PositionNumber(std::string_view id) const noexcept
	{
		std::vector<const S_CARD*> ordered; for(const auto& card:m_Cards)ordered.push_back(&card);
		std::sort(ordered.begin(),ordered.end(),[](const auto* a,const auto* b){return a->nPositionKey!=b->nPositionKey?a->nPositionKey<b->nPositionKey:a->sId<b->sId;});
		for(std::size_t index=0;index<ordered.size();++index)if(ordered[index]->sId==id)return index+1; return std::nullopt;
	}

	std::optional<std::int64_t> C_CARD_LIST_PROJECTION::PositionKeyForCard(std::string_view id) const noexcept
	{
		const auto* card=this->find_card_(id); return card?std::optional<std::int64_t>(card->nPositionKey):std::nullopt;
	}

	std::optional<S_CARD_PREVIEW> C_CARD_LIST_PROJECTION::PreviewForCard(std::string_view id) const
	{
		const auto* card=this->find_card_(id); if(!card)return std::nullopt; return bounded_preview(card->sBody,this->PreviewBudget());
	}

	std::optional<std::string_view> C_CARD_LIST_PROJECTION::FullBodyForCard(std::string_view id) const noexcept
	{
		const auto* card=this->find_card_(id); return card?std::optional<std::string_view>(card->sBody):std::nullopt;
	}

	void C_CARD_LIST_PROJECTION::SetCurrentCardId(std::optional<std::string> id) { m_sCurrentCardId=std::move(id); }
	void C_CARD_LIST_PROJECTION::SetSelectedCardIds(std::vector<std::string> ids) { m_SelectedCardIds=std::move(ids); }

	void C_CARD_LIST_PROJECTION::SetCardDirty(std::string_view id,bool dirty)
	{
		const bool before=m_DirtyCardIds.contains(std::string(id));
		if(before==dirty)return; if(dirty)m_DirtyCardIds.insert(std::string(id)); else m_DirtyCardIds.erase(std::string(id));
		const auto row=this->RowForCard(id); if(row)m_Deltas.push_back({E_CARD_LIST_DELTA_KIND::Update,*row,*row,{E_CARD_LIST_DELTA_FIELD::DirtyDraft}});
	}

	bool C_CARD_LIST_PROJECTION::IsCardDirty(std::string_view id) const { return m_DirtyCardIds.contains(std::string(id)); }
	std::vector<S_CARD_LIST_DELTA> C_CARD_LIST_PROJECTION::TakeDeltas() { auto result=std::move(m_Deltas);m_Deltas.clear();return result; }

	const S_CARD* C_CARD_LIST_PROJECTION::find_card_(std::string_view id) const noexcept
	{
		const auto found=std::find_if(m_Cards.begin(),m_Cards.end(),[&](const auto& card){return card.sId==id;}); return found==m_Cards.end()?nullptr:&*found;
	}

	void C_CARD_LIST_PROJECTION::rebuild_visible_()
	{
		m_Visible.clear(); for(const auto& card:m_Cards)if(!m_SourceFilter||m_SourceFilter->contains(card.eSource))m_Visible.push_back(&card);
		if(m_eSortMode==E_CARD_LIST_SORT_MODE::Position)std::sort(m_Visible.begin(),m_Visible.end(),[](const auto* a,const auto* b){return a->nPositionKey!=b->nPositionKey?a->nPositionKey<b->nPositionKey:a->sId<b->sId;});
		else if(m_eSortMode==E_CARD_LIST_SORT_MODE::Capture)std::sort(m_Visible.begin(),m_Visible.end(),[](const auto* a,const auto* b){return a->nCaptureSeq!=b->nCaptureSeq?a->nCaptureSeq<b->nCaptureSeq:a->sId<b->sId;});
		else std::sort(m_Visible.begin(),m_Visible.end(),[](const auto* a,const auto* b){return a->nUpdatedAtUs!=b->nUpdatedAtUs?a->nUpdatedAtUs>b->nUpdatedAtUs:a->nCaptureSeq>b->nCaptureSeq;});
	}
}
