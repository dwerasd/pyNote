#pragma once

#include "pynote/core/domain/models.h"

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace pynote::core::domain
{
	inline constexpr std::size_t PREVIEW_CODEPOINT_BUDGET_PER_LINE = 4096;

	enum class E_CARD_LIST_SORT_MODE { Recency, Position, Capture };
	enum class E_CARD_LIST_DELTA_KIND { Reset, Insert, Move, Update };
	enum class E_CARD_LIST_DELTA_FIELD { Card, Preview, PreviewTruncated, DirtyDraft };

	struct S_CARD_LIST_DELTA
	{
		E_CARD_LIST_DELTA_KIND eKind{ E_CARD_LIST_DELTA_KIND::Reset };
		std::optional<std::size_t> nOldRow{};
		std::optional<std::size_t> nNewRow{};
		std::vector<E_CARD_LIST_DELTA_FIELD> Fields;
		bool operator==(const S_CARD_LIST_DELTA&) const = default;
	};

	struct S_CARD_PREVIEW
	{
		std::string sText;
		bool bTruncated{ false };
		std::size_t nCodepointsExamined{ 0 };
		std::size_t nBytesConsumed{ 0 };
		bool operator==(const S_CARD_PREVIEW&) const = default;
	};

	std::string_view TraceName(E_CARD_LIST_DELTA_FIELD _eField) noexcept;

	class C_CARD_LIST_PROJECTION
	{
	public:
		explicit C_CARD_LIST_PROJECTION(std::size_t _nPreviewLineCount = 3);

		void SetCards(const std::vector<S_CARD>& _Cards);
		bool AddCard(const S_CARD& _Card);
		std::size_t AddCards(const std::vector<S_CARD>& _Cards);
		bool UpdateCard(const S_CARD& _Card);
		void SetSortMode(E_CARD_LIST_SORT_MODE _eMode);
		E_CARD_LIST_SORT_MODE SortMode() const noexcept { return m_eSortMode; }
		void SetSourceFilter(const std::optional<std::set<E_CARD_SOURCE>>& _Sources);
		void SetPreviewLineCount(std::size_t _nLineCount);
		std::size_t PreviewLineCount() const noexcept { return m_nPreviewLineCount; }
		std::size_t PreviewBudget() const noexcept;

		std::size_t RowCount() const noexcept { return m_Visible.size(); }
		const S_CARD* CardAt(std::size_t _nRow) const noexcept;
		std::optional<std::size_t> RowForCard(std::string_view _sCardId) const noexcept;
		std::optional<std::size_t> PositionNumber(std::string_view _sCardId) const noexcept;
		std::optional<std::int64_t> PositionKeyForCard(std::string_view _sCardId) const noexcept;
		std::optional<S_CARD_PREVIEW> PreviewForCard(std::string_view _sCardId) const;
		std::optional<std::string_view> FullBodyForCard(std::string_view _sCardId) const noexcept;

		void SetCurrentCardId(std::optional<std::string> _sCardId);
		const std::optional<std::string>& CurrentCardId() const noexcept { return m_sCurrentCardId; }
		void SetSelectedCardIds(std::vector<std::string> _CardIds);
		const std::vector<std::string>& SelectedCardIds() const noexcept { return m_SelectedCardIds; }

		void SetCardDirty(std::string_view _sCardId, bool _bDirty);
		bool IsCardDirty(std::string_view _sCardId) const;
		bool CanDragOut() const noexcept { return true; }
		bool CanInternalReorder() const noexcept { return m_eSortMode == E_CARD_LIST_SORT_MODE::Position; }
		bool CanAcceptInternalDrop() const noexcept { return this->CanInternalReorder(); }

		const std::vector<S_CARD_LIST_DELTA>& Deltas() const noexcept { return m_Deltas; }
		std::vector<S_CARD_LIST_DELTA> TakeDeltas();

	private:
		void rebuild_visible_();
		const S_CARD* find_card_(std::string_view _sCardId) const noexcept;
		std::vector<S_CARD> m_Cards;
		std::vector<const S_CARD*> m_Visible;
		E_CARD_LIST_SORT_MODE m_eSortMode{ E_CARD_LIST_SORT_MODE::Recency };
		std::optional<std::set<E_CARD_SOURCE>> m_SourceFilter{};
		std::size_t m_nPreviewLineCount{ 3 };
		std::optional<std::string> m_sCurrentCardId{};
		std::vector<std::string> m_SelectedCardIds;
		std::set<std::string> m_DirtyCardIds;
		std::vector<S_CARD_LIST_DELTA> m_Deltas;
	};
}
