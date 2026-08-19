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
		const auto beforeMode=m_eSelectionMode;const auto beforeCurrent=m_sCurrentCardId;const auto beforeSelected=m_SelectedCardIds;
		m_Cards.clear();
		for (const auto& card : cards) { if (!card.nDeletedAtUs.has_value()) { m_Cards.push_back(card); } }
		this->rebuild_visible_();
		m_Deltas.push_back({E_CARD_LIST_DELTA_KIND::Reset,std::nullopt,std::nullopt,{}});
		this->record_selection_change_(beforeMode,beforeCurrent,beforeSelected);
	}

	bool C_CARD_LIST_PROJECTION::AddCard(const S_CARD& card)
	{
		return this->AddCards({card}) == 1;
	}

	std::size_t C_CARD_LIST_PROJECTION::AddCards(const std::vector<S_CARD>& cards)
	{
		const auto beforeMode=m_eSelectionMode;const auto beforeCurrent=m_sCurrentCardId;const auto beforeSelected=m_SelectedCardIds;
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
		this->record_selection_change_(beforeMode,beforeCurrent,beforeSelected);
		return added.size();
	}

	bool C_CARD_LIST_PROJECTION::UpdateCard(const S_CARD& card)
	{
		const auto beforeMode=m_eSelectionMode;const auto beforeCurrent=m_sCurrentCardId;const auto beforeSelected=m_SelectedCardIds;
		const auto oldRow=this->RowForCard(card.sId);
		auto found=std::find_if(m_Cards.begin(),m_Cards.end(),[&](const auto& value){return value.sId==card.sId;});
		if(found==m_Cards.end())return false;
		if(card.nDeletedAtUs.has_value()) { m_Cards.erase(found); this->rebuild_visible_(); m_Deltas.push_back({E_CARD_LIST_DELTA_KIND::Reset,{},{},{}}); this->record_selection_change_(beforeMode,beforeCurrent,beforeSelected); return true; }
		*found=card; this->rebuild_visible_(); const auto newRow=this->RowForCard(card.sId);
		if(oldRow&&newRow&&*oldRow!=*newRow)m_Deltas.push_back({E_CARD_LIST_DELTA_KIND::Move,*oldRow,*newRow,{E_CARD_LIST_DELTA_FIELD::Card}});
		if(newRow)m_Deltas.push_back({E_CARD_LIST_DELTA_KIND::Update,*newRow,*newRow,
			{E_CARD_LIST_DELTA_FIELD::Card,E_CARD_LIST_DELTA_FIELD::Preview,E_CARD_LIST_DELTA_FIELD::PreviewTruncated}});
		this->record_selection_change_(beforeMode,beforeCurrent,beforeSelected);
		return true;
	}

	void C_CARD_LIST_PROJECTION::SetSortMode(E_CARD_LIST_SORT_MODE mode)
	{
		if(mode==m_eSortMode)return;const auto beforeMode=m_eSelectionMode;const auto beforeCurrent=m_sCurrentCardId;const auto beforeSelected=m_SelectedCardIds;m_eSortMode=mode; this->rebuild_visible_();
		m_Deltas.push_back({E_CARD_LIST_DELTA_KIND::Reset,{},{},{}});
		this->record_selection_change_(beforeMode,beforeCurrent,beforeSelected);
	}

	void C_CARD_LIST_PROJECTION::SetSourceFilter(const std::optional<std::set<E_CARD_SOURCE>>& sources)
	{
		if(sources==m_SourceFilter)return;const auto beforeMode=m_eSelectionMode;const auto beforeCurrent=m_sCurrentCardId;const auto beforeSelected=m_SelectedCardIds;m_SourceFilter=sources; this->rebuild_visible_();
		m_Deltas.push_back({E_CARD_LIST_DELTA_KIND::Reset,{},{},{}});
		this->record_selection_change_(beforeMode,beforeCurrent,beforeSelected);
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

	void C_CARD_LIST_PROJECTION::SetCurrentCardId(std::optional<std::string> id)
	{
		const auto beforeMode=m_eSelectionMode;const auto beforeCurrent=m_sCurrentCardId;const auto beforeSelected=m_SelectedCardIds;
		if(id&&this->find_card_(*id)==nullptr)id.reset();m_sCurrentCardId=std::move(id);this->normalize_selection_();
		this->record_selection_change_(beforeMode,beforeCurrent,beforeSelected);
	}

	void C_CARD_LIST_PROJECTION::SetSelectedCardIds(std::vector<std::string> ids)
	{
		const auto beforeMode=m_eSelectionMode;const auto beforeCurrent=m_sCurrentCardId;const auto beforeSelected=m_SelectedCardIds;
		m_SelectedCardIds=std::move(ids);this->normalize_selection_();
		if(m_eSelectionMode==E_CARD_SELECTION_MODE::Single&&!m_SelectedCardIds.empty())m_sCurrentCardId=m_SelectedCardIds.front();
		this->record_selection_change_(beforeMode,beforeCurrent,beforeSelected);
	}

	void C_CARD_LIST_PROJECTION::SetMultiSelectionEnabled(bool enabled)
	{
		const auto beforeMode=m_eSelectionMode;const auto beforeCurrent=m_sCurrentCardId;const auto beforeSelected=m_SelectedCardIds;
		m_eSelectionMode=enabled?E_CARD_SELECTION_MODE::Extended:E_CARD_SELECTION_MODE::Single;
		this->normalize_selection_();
		if(!enabled) {
			std::optional<std::string> survivor;
			if(m_sCurrentCardId&&std::find(m_SelectedCardIds.begin(),m_SelectedCardIds.end(),*m_sCurrentCardId)!=m_SelectedCardIds.end())survivor=m_sCurrentCardId;
			else if(!m_SelectedCardIds.empty())survivor=m_SelectedCardIds.front();
			else if(m_sCurrentCardId&&this->RowForCard(*m_sCurrentCardId))survivor=m_sCurrentCardId;
			m_SelectedCardIds=survivor?std::vector<std::string>{*survivor}:std::vector<std::string>{};m_sCurrentCardId=survivor;
		}
		this->record_selection_change_(beforeMode,beforeCurrent,beforeSelected);
	}

	bool C_CARD_LIST_PROJECTION::SelectVisibleCard(std::string_view id,E_CARD_SELECTION_INTENT intent)
	{
		const auto target=this->RowForCard(id);if(!target)return false;
		const auto beforeMode=m_eSelectionMode;const auto beforeCurrent=m_sCurrentCardId;const auto beforeSelected=m_SelectedCardIds;
		if(m_eSelectionMode==E_CARD_SELECTION_MODE::Single||intent==E_CARD_SELECTION_INTENT::Replace)m_SelectedCardIds={std::string(id)};
		else if(intent==E_CARD_SELECTION_INTENT::Additive) {
			if(std::find(m_SelectedCardIds.begin(),m_SelectedCardIds.end(),id)==m_SelectedCardIds.end())m_SelectedCardIds.push_back(std::string(id));
		}
		else {
			const auto anchor=m_sCurrentCardId?this->RowForCard(*m_sCurrentCardId):std::nullopt;
			if(!anchor)m_SelectedCardIds={std::string(id)};
			else {m_SelectedCardIds.clear();const auto first=(std::min)(*anchor,*target);const auto last=(std::max)(*anchor,*target);for(std::size_t row=first;row<=last;++row)m_SelectedCardIds.push_back(m_Visible[row]->sId);}
		}
		m_sCurrentCardId=std::string(id);this->normalize_selection_();this->record_selection_change_(beforeMode,beforeCurrent,beforeSelected);return true;
	}

	bool C_CARD_LIST_PROJECTION::MoveCurrentBy(std::ptrdiff_t delta)
	{
		if(!m_sCurrentCardId||(delta!=-1&&delta!=1))return false;const auto row=this->RowForCard(*m_sCurrentCardId);if(!row)return false;
		const std::ptrdiff_t target=static_cast<std::ptrdiff_t>(*row)+delta;if(target<0||target>=static_cast<std::ptrdiff_t>(m_Visible.size()))return false;
		return this->SelectVisibleCard(m_Visible[static_cast<std::size_t>(target)]->sId,E_CARD_SELECTION_INTENT::Replace);
	}

	std::vector<S_CARD_SELECTION_DELTA> C_CARD_LIST_PROJECTION::TakeSelectionDeltas(){auto result=std::move(m_SelectionDeltas);m_SelectionDeltas.clear();return result;}

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
		this->normalize_selection_();
	}

	void C_CARD_LIST_PROJECTION::normalize_selection_()
	{
		std::vector<std::string> normalized;
		for(const auto* card:m_Visible)if(std::find(m_SelectedCardIds.begin(),m_SelectedCardIds.end(),card->sId)!=m_SelectedCardIds.end())normalized.push_back(card->sId);
		if(m_eSelectionMode==E_CARD_SELECTION_MODE::Single&&normalized.size()>1) {
			const auto current=m_sCurrentCardId?std::find(normalized.begin(),normalized.end(),*m_sCurrentCardId):normalized.end();
			normalized={current!=normalized.end()?*current:normalized.front()};
		}
		m_SelectedCardIds=std::move(normalized);
		if(m_sCurrentCardId&&this->find_card_(*m_sCurrentCardId)==nullptr)m_sCurrentCardId.reset();
	}

	void C_CARD_LIST_PROJECTION::record_selection_change_(E_CARD_SELECTION_MODE beforeMode,
		const std::optional<std::string>& beforeCurrent,const std::vector<std::string>& beforeSelected)
	{
		if(beforeMode!=m_eSelectionMode||beforeCurrent!=m_sCurrentCardId||beforeSelected!=m_SelectedCardIds)
			m_SelectionDeltas.push_back({m_eSelectionMode,m_sCurrentCardId,m_SelectedCardIds});
	}
}
