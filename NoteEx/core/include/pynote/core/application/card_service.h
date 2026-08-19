#pragma once

#include "pynote/core/domain/models.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pynote::core::domain { class C_PARAGRAPH_PARSER; }
namespace pynote::core::storage { class C_DATABASE; class C_REPOSITORIES; enum class E_REPO_RESULT; }

namespace pynote::core::application
{
	inline constexpr std::int64_t POSITION_STEP = 1024;

	enum class E_CARD_SERVICE_RESULT { Ok, NotFound, Invalid, Conflict, Failed };
	enum class E_CARD_SORT_MODE { Recency, Position, Capture };

	using Clock = std::function<std::int64_t()>;
	using IdFactory = std::function<std::string()>;

	class C_CARD_SERVICE
	{
	public:
		C_CARD_SERVICE(
			storage::C_DATABASE& _Database,
			storage::C_REPOSITORIES& _Repositories,
			const domain::C_PARAGRAPH_PARSER& _Parser,
			Clock _Clock,
			IdFactory _IdFactory);

		E_CARD_SERVICE_RESULT CreateCards(
			const std::string& _sDocumentId,
			const std::string& _sText,
			domain::E_CAPTURE_OPERATION_SOURCE _eSource,
			bool _bSplit,
			const std::optional<std::string>& _sBeforeCardId,
			std::vector<domain::S_CARD>* _pOut);
		E_CARD_SERVICE_RESULT CreateCard(
			const std::string& _sDocumentId,
			const std::string& _sText,
			domain::E_CAPTURE_OPERATION_SOURCE _eSource,
			const std::optional<std::string>& _sBeforeCardId,
			domain::S_CARD* _pOut);
		E_CARD_SERVICE_RESULT MoveCard(
			const std::string& _sCardId,
			const std::optional<std::string>& _sBeforeCardId,
			domain::S_CARD* _pOut);
		E_CARD_SERVICE_RESULT SoftDelete(
			const std::string& _sCardId,
			const std::optional<std::string>& _sExpectedRevisionId,
			bool _bRequireEmptyBody,
			const std::optional<std::string>& _sDiscardDraftId,
			domain::S_CARD* _pOut);
		E_CARD_SERVICE_RESULT ListActiveCards(
			const std::string& _sDocumentId,
			E_CARD_SORT_MODE _eSortMode,
			std::vector<domain::S_CARD>* _pOut);

	private:
		E_CARD_SERVICE_RESULT active_cards_(const std::string& _sDocumentId, std::vector<domain::S_CARD>* _pOut);
		E_CARD_SERVICE_RESULT require_active_card_(const std::string& _sCardId, domain::S_CARD* _pOut);
		E_CARD_SERVICE_RESULT insertion_index_(
			const std::vector<domain::S_CARD>& _Cards,
			const std::optional<std::string>& _sBeforeCardId,
			std::size_t* _pIndex) const;
		E_CARD_SERVICE_RESULT allocate_positions_(
			std::vector<domain::S_CARD>* _pCards,
			std::size_t _nInsertIndex,
			std::size_t _nCount,
			std::vector<std::int64_t>* _pOut);
		E_CARD_SERVICE_RESULT rebalance_(std::vector<domain::S_CARD>* _pCards);
		static E_CARD_SERVICE_RESULT from_repo_(storage::E_REPO_RESULT _eResult);

		storage::C_DATABASE& m_Database;
		storage::C_REPOSITORIES& m_Repositories;
		const domain::C_PARAGRAPH_PARSER& m_Parser;
		Clock m_Clock;
		IdFactory m_IdFactory;
	};
}
