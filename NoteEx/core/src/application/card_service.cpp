#include "pynote/core/application/card_service.h"

#include "pynote/core/domain/paragraph_parser.h"
#include "pynote/core/storage/database.h"
#include "pynote/core/storage/repositories.h"

#include <algorithm>
#include <sstream>
#include <tuple>
#include <utility>

namespace pynote::core::application
{
	namespace
	{
		bool allowed_source(domain::E_CAPTURE_OPERATION_SOURCE _eSource) noexcept
		{
			using E = domain::E_CAPTURE_OPERATION_SOURCE;
			return _eSource == E::Typing || _eSource == E::Paste || _eSource == E::Import
				|| _eSource == E::Mixed || _eSource == E::System;
		}

		domain::E_CARD_SOURCE card_source(domain::E_CAPTURE_OPERATION_SOURCE _eSource)
		{
			using I = domain::E_CAPTURE_OPERATION_SOURCE;
			using O = domain::E_CARD_SOURCE;
			switch (_eSource) {
			case I::Typing: return O::Typing;
			case I::Paste: return O::Paste;
			case I::Import: return O::Import;
			case I::Mixed: return O::Mixed;
			default: return O::System;
			}
		}

		domain::E_EVENT_SOURCE event_source(domain::E_CAPTURE_OPERATION_SOURCE _eSource)
		{
			using I = domain::E_CAPTURE_OPERATION_SOURCE;
			using O = domain::E_EVENT_SOURCE;
			switch (_eSource) {
			case I::Typing: return O::Typing;
			case I::Paste: return O::Paste;
			case I::Import: return O::Import;
			case I::Mixed: return O::Mixed;
			default: return O::System;
			}
		}

		std::string position_json(std::int64_t _nPosition)
		{
			return std::string("{\"position_key\": ") + std::to_string(_nPosition) + "}";
		}

		std::string neighbor_json(std::int64_t _nPosition, const std::optional<std::string>& _sLeft,
			const std::optional<std::string>& _sRight)
		{
			auto json_value = [](const std::optional<std::string>& value) {
				return value.has_value() ? std::string("\"") + *value + "\"" : std::string("null");
			};
			return std::string("{\"position_key\": ") + std::to_string(_nPosition)
				+ ", \"left_neighbor_id\": " + json_value(_sLeft)
				+ ", \"right_neighbor_id\": " + json_value(_sRight) + "}";
		}

		std::string move_json(std::int64_t _nOldPosition, std::int64_t _nNewPosition,
			const std::optional<std::string>& _sLeft, const std::optional<std::string>& _sRight)
		{
			auto json_value = [](const std::optional<std::string>& value) {
				return value.has_value() ? std::string("\"") + *value + "\"" : std::string("null");
			};
			return std::string("{\"old_position_key\": ") + std::to_string(_nOldPosition)
				+ ", \"new_position_key\": " + std::to_string(_nNewPosition)
				+ ", \"left_neighbor_id\": " + json_value(_sLeft)
				+ ", \"right_neighbor_id\": " + json_value(_sRight) + "}";
		}
	}

	C_CARD_SERVICE::C_CARD_SERVICE(storage::C_DATABASE& _Database,
		storage::C_REPOSITORIES& _Repositories, const domain::C_PARAGRAPH_PARSER& _Parser,
		Clock _Clock, IdFactory _IdFactory)
		: m_Database(_Database), m_Repositories(_Repositories), m_Parser(_Parser),
		  m_Clock(std::move(_Clock)), m_IdFactory(std::move(_IdFactory))
	{
	}

	E_CARD_SERVICE_RESULT C_CARD_SERVICE::from_repo_(storage::E_REPO_RESULT _eResult)
	{
		switch (_eResult) {
		case storage::E_REPO_RESULT::Ok: return E_CARD_SERVICE_RESULT::Ok;
		case storage::E_REPO_RESULT::NotFound: return E_CARD_SERVICE_RESULT::NotFound;
		case storage::E_REPO_RESULT::Invalid: return E_CARD_SERVICE_RESULT::Invalid;
		case storage::E_REPO_RESULT::CasConflict: return E_CARD_SERVICE_RESULT::Conflict;
		default: return E_CARD_SERVICE_RESULT::Failed;
		}
	}

	E_CARD_SERVICE_RESULT C_CARD_SERVICE::active_cards_(const std::string& _sDocumentId,
		std::vector<domain::S_CARD>* _pOut)
	{
		std::vector<domain::S_CARD> Cards;
		const auto result = m_Repositories.ListCards(_sDocumentId, &Cards);
		if (result != storage::E_REPO_RESULT::Ok) { return from_repo_(result); }
		_pOut->clear();
		for (domain::S_CARD& card : Cards) {
			if (!card.nDeletedAtUs.has_value()) { _pOut->push_back(std::move(card)); }
		}
		return E_CARD_SERVICE_RESULT::Ok;
	}

	E_CARD_SERVICE_RESULT C_CARD_SERVICE::require_active_card_(const std::string& _sCardId,
		domain::S_CARD* _pOut)
	{
		const auto result = m_Repositories.GetCard(_sCardId, _pOut);
		if (result != storage::E_REPO_RESULT::Ok) { return from_repo_(result); }
		return _pOut->nDeletedAtUs.has_value() ? E_CARD_SERVICE_RESULT::NotFound : E_CARD_SERVICE_RESULT::Ok;
	}

	E_CARD_SERVICE_RESULT C_CARD_SERVICE::insertion_index_(const std::vector<domain::S_CARD>& _Cards,
		const std::optional<std::string>& _sBeforeCardId, std::size_t* _pIndex) const
	{
		if (!_sBeforeCardId.has_value()) { *_pIndex = _Cards.size(); return E_CARD_SERVICE_RESULT::Ok; }
		for (std::size_t index = 0; index < _Cards.size(); ++index) {
			if (_Cards[index].sId == *_sBeforeCardId) { *_pIndex = index; return E_CARD_SERVICE_RESULT::Ok; }
		}
		return E_CARD_SERVICE_RESULT::NotFound;
	}

	E_CARD_SERVICE_RESULT C_CARD_SERVICE::rebalance_(std::vector<domain::S_CARD>* _pCards)
	{
		if (_pCards->empty()) { return E_CARD_SERVICE_RESULT::Ok; }
		const auto minimum = std::min_element(_pCards->begin(), _pCards->end(),
			[](const auto& left, const auto& right) { return left.nPositionKey < right.nPositionKey; })->nPositionKey;
		const std::int64_t temporary = minimum - static_cast<std::int64_t>(_pCards->size()) - 1;
		for (std::size_t index = 0; index < _pCards->size(); ++index) {
			if (!(*_pCards)[index].sCurrentRevisionId.has_value()) { return E_CARD_SERVICE_RESULT::Failed; }
			const auto result = m_Repositories.UpdateCardPosition((*_pCards)[index].sId,
				temporary + static_cast<std::int64_t>(index), *(*_pCards)[index].sCurrentRevisionId);
			if (result != storage::E_REPO_RESULT::Ok) { return from_repo_(result); }
		}
		for (std::size_t index = 0; index < _pCards->size(); ++index) {
			const auto position = POSITION_STEP * static_cast<std::int64_t>(index + 1);
			const auto result = m_Repositories.UpdateCardPosition((*_pCards)[index].sId,
				position, *(*_pCards)[index].sCurrentRevisionId);
			if (result != storage::E_REPO_RESULT::Ok) { return from_repo_(result); }
			(*_pCards)[index].nPositionKey = position;
		}
		return E_CARD_SERVICE_RESULT::Ok;
	}

	E_CARD_SERVICE_RESULT C_CARD_SERVICE::allocate_positions_(std::vector<domain::S_CARD>* _pCards,
		std::size_t _nInsertIndex, std::size_t _nCount, std::vector<std::int64_t>* _pOut)
	{
		if (_nCount == 0) { return E_CARD_SERVICE_RESULT::Invalid; }
		auto calculate = [&]() {
			const std::int64_t left = _nInsertIndex == 0 ? 0 : (*_pCards)[_nInsertIndex - 1].nPositionKey;
			const std::int64_t right = _nInsertIndex < _pCards->size()
				? (*_pCards)[_nInsertIndex].nPositionKey
				: left + POSITION_STEP * static_cast<std::int64_t>(_nCount + 1);
			return std::pair(left, (right - left) / static_cast<std::int64_t>(_nCount + 1));
		};
		if (_pCards->empty()) {
			_pOut->clear();
			for (std::size_t index = 0; index < _nCount; ++index) {
				_pOut->push_back(POSITION_STEP * static_cast<std::int64_t>(index + 1));
			}
			return E_CARD_SERVICE_RESULT::Ok;
		}
		auto [left, step] = calculate();
		if (step < 1) {
			const auto result = this->rebalance_(_pCards);
			if (result != E_CARD_SERVICE_RESULT::Ok) { return result; }
			std::tie(left, step) = calculate();
		}
		_pOut->clear();
		for (std::size_t index = 0; index < _nCount; ++index) {
			_pOut->push_back(left + step * static_cast<std::int64_t>(index + 1));
		}
		return E_CARD_SERVICE_RESULT::Ok;
	}

	E_CARD_SERVICE_RESULT C_CARD_SERVICE::CreateCards(const std::string& _sDocumentId,
		const std::string& _sText, domain::E_CAPTURE_OPERATION_SOURCE _eSource, bool _bSplit,
		const std::optional<std::string>& _sBeforeCardId, std::vector<domain::S_CARD>* _pOut)
	{
		_pOut->clear();
		const std::vector<std::string> Paragraphs = m_Parser.Split(_sText);
		if (Paragraphs.empty() || !allowed_source(_eSource)) { return E_CARD_SERVICE_RESULT::Invalid; }
		domain::S_DOCUMENT Document;
		const auto documentResult = m_Repositories.GetDocument(_sDocumentId, &Document);
		if (documentResult != storage::E_REPO_RESULT::Ok) { return from_repo_(documentResult); }

		const bool shouldSplit = _bSplit && Paragraphs.size() >= 2;
		const std::vector<std::string> Bodies = shouldSplit ? Paragraphs : std::vector<std::string>{ m_Parser.Keep(_sText) };
		const std::int64_t createdAt = m_Clock();
		domain::S_NEW_CAPTURE_OPERATION Operation;
		Operation.sId = m_IdFactory();
		Operation.sDocumentId = _sDocumentId;
		Operation.eSource = _eSource;
		Operation.eSplitPolicy = shouldSplit ? domain::E_SPLIT_POLICY::SplitByBlankLine : domain::E_SPLIT_POLICY::Keep;
		Operation.sOriginalText = shouldSplit ? std::optional<std::string>(_sText) : std::nullopt;
		Operation.nCreatedAtUs = createdAt;

		storage::C_TRANSACTION Transaction(m_Database);
		if (!Transaction.IsActive()) { return E_CARD_SERVICE_RESULT::Failed; }
		std::vector<domain::S_CARD> ActiveCards;
		auto result = this->active_cards_(_sDocumentId, &ActiveCards);
		if (result != E_CARD_SERVICE_RESULT::Ok) { return result; }
		std::size_t insertIndex = 0;
		result = this->insertion_index_(ActiveCards, _sBeforeCardId, &insertIndex);
		if (result != E_CARD_SERVICE_RESULT::Ok) { return result; }
		std::vector<std::int64_t> Positions;
		result = this->allocate_positions_(&ActiveCards, insertIndex, Bodies.size(), &Positions);
		if (result != E_CARD_SERVICE_RESULT::Ok) { return result; }

		std::vector<domain::S_NEW_CARD> NewCards;
		for (std::size_t index = 0; index < Bodies.size(); ++index) {
			domain::S_NEW_CARD card;
			card.sId = m_IdFactory();
			card.sRevisionId = m_IdFactory();
			card.sEventId = m_IdFactory();
			card.nPositionKey = Positions[index];
			card.sBody = Bodies[index];
			card.eCardSource = card_source(_eSource);
			card.eEventSource = event_source(_eSource);
			card.eRevisionSource = domain::E_REVISION_SOURCE::Edit;
			card.nCreatedAtUs = createdAt;
			card.sEventDetailsJson = position_json(Positions[index]);
			NewCards.push_back(std::move(card));
		}
		std::vector<domain::S_CARD> Created;
		const auto createResult = m_Repositories.CreateCardsInActiveTransaction(Operation, NewCards, &Created);
		if (createResult != storage::E_REPO_RESULT::Ok) { return from_repo_(createResult); }
		const auto touchResult = m_Repositories.TouchDocument(_sDocumentId, createdAt);
		if (touchResult != storage::E_REPO_RESULT::Ok) { return from_repo_(touchResult); }
		if (!Transaction.Commit()) { return E_CARD_SERVICE_RESULT::Failed; }
		*_pOut = std::move(Created);
		return E_CARD_SERVICE_RESULT::Ok;
	}

	E_CARD_SERVICE_RESULT C_CARD_SERVICE::CreateCard(const std::string& _sDocumentId,
		const std::string& _sText, domain::E_CAPTURE_OPERATION_SOURCE _eSource,
		const std::optional<std::string>& _sBeforeCardId, domain::S_CARD* _pOut)
	{
		std::vector<domain::S_CARD> Created;
		const auto result = this->CreateCards(_sDocumentId, _sText, _eSource, false, _sBeforeCardId, &Created);
		if (result == E_CARD_SERVICE_RESULT::Ok) { *_pOut = Created.front(); }
		return result;
	}

	E_CARD_SERVICE_RESULT C_CARD_SERVICE::MoveCard(const std::string& _sCardId,
		const std::optional<std::string>& _sBeforeCardId, domain::S_CARD* _pOut)
	{
		domain::S_CARD Initial;
		auto result = this->require_active_card_(_sCardId, &Initial);
		if (result != E_CARD_SERVICE_RESULT::Ok || !Initial.sCurrentRevisionId.has_value()) { return result; }
		if (_sBeforeCardId == std::optional<std::string>(_sCardId)) { *_pOut = Initial; return E_CARD_SERVICE_RESULT::Ok; }
		const std::string expectedRevision = *Initial.sCurrentRevisionId;
		const std::int64_t occurredAt = m_Clock();
		storage::C_TRANSACTION Transaction(m_Database);
		if (!Transaction.IsActive()) { return E_CARD_SERVICE_RESULT::Failed; }
		domain::S_CARD Card;
		result = this->require_active_card_(_sCardId, &Card);
		if (result != E_CARD_SERVICE_RESULT::Ok) { return result; }
		if (Card.sCurrentRevisionId != std::optional<std::string>(expectedRevision)) { return E_CARD_SERVICE_RESULT::Conflict; }
		std::vector<domain::S_CARD> Active;
		result = this->active_cards_(Card.sDocumentId, &Active);
		if (result != E_CARD_SERVICE_RESULT::Ok) { return result; }
		const auto minimumActive = std::min_element(Active.begin(), Active.end(),
			[](const auto& a, const auto& b) { return a.nPositionKey < b.nPositionKey; });
		if (minimumActive == Active.end()) { return E_CARD_SERVICE_RESULT::NotFound; }
		std::vector<std::string> original;
		for (const auto& item : Active) { original.push_back(item.sId); }
		Active.erase(std::remove_if(Active.begin(), Active.end(), [&](const auto& item) { return item.sId == Card.sId; }), Active.end());
		std::size_t insertIndex = 0;
		result = this->insertion_index_(Active, _sBeforeCardId, &insertIndex);
		if (result != E_CARD_SERVICE_RESULT::Ok) { return result; }
		std::vector<std::string> finalOrder;
		for (const auto& item : Active) { finalOrder.push_back(item.sId); }
		finalOrder.insert(finalOrder.begin() + static_cast<std::ptrdiff_t>(insertIndex), Card.sId);
		if (finalOrder == original) { *_pOut = Card; return E_CARD_SERVICE_RESULT::Ok; }
		const std::int64_t temporary = minimumActive->nPositionKey - POSITION_STEP;
		auto repoResult = m_Repositories.UpdateCardPosition(Card.sId, temporary, expectedRevision);
		if (repoResult != storage::E_REPO_RESULT::Ok) { return from_repo_(repoResult); }
		std::vector<std::int64_t> Positions;
		result = this->allocate_positions_(&Active, insertIndex, 1, &Positions);
		if (result != E_CARD_SERVICE_RESULT::Ok) { return result; }
		const std::optional<std::string> left = insertIndex ? std::optional<std::string>(Active[insertIndex - 1].sId) : std::nullopt;
		const std::optional<std::string> right = insertIndex < Active.size() ? std::optional<std::string>(Active[insertIndex].sId) : std::nullopt;
		domain::S_EDIT_EVENT Event;
		Event.sEventId = m_IdFactory(); Event.sDocumentId = Card.sDocumentId; Event.sCardId = Card.sId;
		Event.eEventType = domain::E_EVENT_TYPE::Move; Event.eSource = domain::E_EVENT_SOURCE::System;
		Event.nOccurredAtUs = occurredAt; Event.sDetailsJson = move_json(Card.nPositionKey, Positions[0], left, right);
		domain::S_EDIT_EVENT StoredEvent;
		repoResult = m_Repositories.CreateEvent(Event, &StoredEvent);
		if (repoResult != storage::E_REPO_RESULT::Ok) { return from_repo_(repoResult); }
		repoResult = m_Repositories.UpdateCardPosition(Card.sId, Positions[0], expectedRevision);
		if (repoResult != storage::E_REPO_RESULT::Ok) { return from_repo_(repoResult); }
		repoResult = m_Repositories.TouchDocument(Card.sDocumentId, occurredAt);
		if (repoResult != storage::E_REPO_RESULT::Ok || !Transaction.Commit()) { return E_CARD_SERVICE_RESULT::Failed; }
		Card.nPositionKey = Positions[0]; *_pOut = Card;
		return E_CARD_SERVICE_RESULT::Ok;
	}

	E_CARD_SERVICE_RESULT C_CARD_SERVICE::SoftDelete(const std::string& _sCardId,
		const std::optional<std::string>& _sExpectedRevisionId, bool _bRequireEmptyBody,
		const std::optional<std::string>& _sDiscardDraftId, domain::S_CARD* _pOut)
	{
		domain::S_CARD Initial;
		auto result = this->require_active_card_(_sCardId, &Initial);
		if (result != E_CARD_SERVICE_RESULT::Ok || !Initial.sCurrentRevisionId.has_value()) { return result; }
		const std::string expected = _sExpectedRevisionId.value_or(*Initial.sCurrentRevisionId);
		const std::int64_t deletedAt = m_Clock();
		storage::C_TRANSACTION Transaction(m_Database);
		if (!Transaction.IsActive()) { return E_CARD_SERVICE_RESULT::Failed; }
		domain::S_CARD Card;
		result = this->require_active_card_(_sCardId, &Card);
		if (result != E_CARD_SERVICE_RESULT::Ok) { return result; }
		if (Card.sCurrentRevisionId != std::optional<std::string>(expected)) { return E_CARD_SERVICE_RESULT::Conflict; }
		if (_bRequireEmptyBody && !m_Parser.IsZeroParagraphInput(Card.sBody)) { return E_CARD_SERVICE_RESULT::Conflict; }
		std::vector<domain::S_CARD> Active;
		result = this->active_cards_(Card.sDocumentId, &Active);
		if (result != E_CARD_SERVICE_RESULT::Ok) { return result; }
		const auto found = std::find_if(Active.begin(), Active.end(), [&](const auto& item) { return item.sId == Card.sId; });
		if (found == Active.end()) { return E_CARD_SERVICE_RESULT::NotFound; }
		const std::size_t index = static_cast<std::size_t>(found - Active.begin());
		const std::optional<std::string> left = index ? std::optional<std::string>(Active[index - 1].sId) : std::nullopt;
		const std::optional<std::string> right = index + 1 < Active.size() ? std::optional<std::string>(Active[index + 1].sId) : std::nullopt;
		domain::S_EDIT_EVENT Event;
		Event.sEventId = m_IdFactory(); Event.sDocumentId = Card.sDocumentId; Event.sCardId = Card.sId;
		Event.eEventType = domain::E_EVENT_TYPE::Delete; Event.eSource = domain::E_EVENT_SOURCE::System;
		Event.nOccurredAtUs = deletedAt; Event.sDetailsJson = neighbor_json(Card.nPositionKey, left, right);
		domain::S_EDIT_EVENT StoredEvent;
		auto repoResult = m_Repositories.CreateEvent(Event, &StoredEvent);
		if (repoResult != storage::E_REPO_RESULT::Ok) { return from_repo_(repoResult); }
		repoResult = m_Repositories.UpdateCardDeletedState(Card.sId, Card.nPositionKey, deletedAt, expected);
		if (repoResult != storage::E_REPO_RESULT::Ok) { return from_repo_(repoResult); }
		if (_sDiscardDraftId.has_value()) {
			repoResult = m_Repositories.DeleteDraft(*_sDiscardDraftId);
			if (repoResult != storage::E_REPO_RESULT::Ok) { return from_repo_(repoResult); }
		}
		repoResult = m_Repositories.TouchDocument(Card.sDocumentId, deletedAt);
		if (repoResult != storage::E_REPO_RESULT::Ok || !Transaction.Commit()) { return E_CARD_SERVICE_RESULT::Failed; }
		Card.nDeletedAtUs = deletedAt; *_pOut = Card;
		return E_CARD_SERVICE_RESULT::Ok;
	}

	E_CARD_SERVICE_RESULT C_CARD_SERVICE::ListActiveCards(const std::string& _sDocumentId,
		E_CARD_SORT_MODE _eSortMode, std::vector<domain::S_CARD>* _pOut)
	{
		auto result = this->active_cards_(_sDocumentId, _pOut);
		if (result != E_CARD_SERVICE_RESULT::Ok) { return result; }
		if (_eSortMode == E_CARD_SORT_MODE::Capture) {
			std::sort(_pOut->begin(), _pOut->end(), [](const auto& a, const auto& b) {
				return a.nCaptureSeq != b.nCaptureSeq ? a.nCaptureSeq < b.nCaptureSeq : a.sId < b.sId;
			});
		}
		else if (_eSortMode == E_CARD_SORT_MODE::Recency) {
			std::sort(_pOut->begin(), _pOut->end(), [](const auto& a, const auto& b) {
				return a.nUpdatedAtUs != b.nUpdatedAtUs ? a.nUpdatedAtUs > b.nUpdatedAtUs : a.nCaptureSeq > b.nCaptureSeq;
			});
		}
		return E_CARD_SERVICE_RESULT::Ok;
	}
}
