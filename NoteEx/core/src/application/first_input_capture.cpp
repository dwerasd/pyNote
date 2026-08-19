#include "pynote/core/application/first_input_capture.h"

#include <utility>

namespace pynote::core::application
{
	C_FIRST_INPUT_CAPTURE::C_FIRST_INPUT_CAPTURE(C_CARD_SERVICE& _Service,
		domain::C_CARD_LIST_PROJECTION& _Projection, std::string _sDocumentId)
		: m_pService(&_Service), m_pProjection(&_Projection), m_sDocumentId(std::move(_sDocumentId))
	{
	}

	C_FIRST_INPUT_CAPTURE::C_FIRST_INPUT_CAPTURE(domain::C_CARD_LIST_PROJECTION& _Projection,
		ZeroParagraphPredicate _ZeroParagraphs, CreateOneCardPort _CreateOneCard,
		ActiveCardLookupPort _LookupActiveCard, LinkCardPort _LinkCard)
		: m_pProjection(&_Projection), m_ZeroParagraphs(std::move(_ZeroParagraphs)),
		  m_CreateOneCard(std::move(_CreateOneCard)), m_LookupActiveCard(std::move(_LookupActiveCard)),
		  m_LinkCard(std::move(_LinkCard))
	{
	}

	S_FIRST_INPUT_RESULT C_FIRST_INPUT_CAPTURE::OnMeaningfulInsertion(const std::string& _sWholeText,
		domain::E_CAPTURE_OPERATION_SOURCE _eSource)
	{
		if (m_ePhase == E_FIRST_INPUT_PHASE::Connected) {
			return { std::nullopt, E_FIRST_INPUT_EFFECT::AlreadyConnected, m_sConnectedCardId, false };
		}

		domain::S_CARD card;
		const auto outcome = m_pService->CreateCard(m_sDocumentId, _sWholeText, _eSource, std::nullopt, &card);
		if (outcome != E_CARD_SERVICE_RESULT::Ok) {
			return { outcome, E_FIRST_INPUT_EFFECT::CreationFailed, std::nullopt, true };
		}

		m_pProjection->AddCard(card);
		m_sConnectedCardId = card.sId;
		m_ePhase = E_FIRST_INPUT_PHASE::Connected;
		return { outcome, E_FIRST_INPUT_EFFECT::ConnectCreatedCard, m_sConnectedCardId, true };
	}

	S_FIRST_INPUT_RECOVERY_RESULT C_FIRST_INPUT_CAPTURE::result_(E_FIRST_INPUT_RECOVERY_EFFECT _eEffect) const
	{
		S_FIRST_INPUT_RECOVERY_RESULT result;
		result.ePhase = m_ePhase;
		result.eEffect = _eEffect;
		result.sPendingCardId = m_sPendingCardId;
		result.sConnectedCardId = m_sConnectedCardId;
		return result;
	}

	S_FIRST_INPUT_RECOVERY_RESULT C_FIRST_INPUT_CAPTURE::link_(const domain::S_CARD& _Card,
		S_FIRST_INPUT_RECOVERY_RESULT _Result)
	{
		_Result.bLinkAttempted = true;
		try {
			if (!m_LinkCard(_Card)) {
				_Result.eLinkOutcome = E_FIRST_INPUT_ATTEMPT_OUTCOME::Rejected;
				_Result.eEffect = E_FIRST_INPUT_RECOVERY_EFFECT::LinkPending;
				_Result.ePhase = m_ePhase;
				_Result.sPendingCardId = m_sPendingCardId;
				return _Result;
			}
		} catch (...) {
			_Result.eLinkOutcome = E_FIRST_INPUT_ATTEMPT_OUTCOME::Exception;
			_Result.eEffect = E_FIRST_INPUT_RECOVERY_EFFECT::LinkPending;
			_Result.ePhase = m_ePhase;
			_Result.sPendingCardId = m_sPendingCardId;
			return _Result;
		}

		_Result.eLinkOutcome = E_FIRST_INPUT_ATTEMPT_OUTCOME::Succeeded;
		m_sConnectedCardId = _Card.sId;
		m_sPendingCardId.reset();
		m_NewSnapshot.reset();
		m_ePhase = E_FIRST_INPUT_PHASE::Connected;
		_Result.ePhase = m_ePhase;
		_Result.eEffect = E_FIRST_INPUT_RECOVERY_EFFECT::Connected;
		_Result.sPendingCardId.reset();
		_Result.sConnectedCardId = m_sConnectedCardId;
		return _Result;
	}

	S_FIRST_INPUT_RECOVERY_RESULT C_FIRST_INPUT_CAPTURE::create_and_link_(const S_FIRST_INPUT_EVENT& _Event,
		E_FIRST_INPUT_ATTEMPT_OUTCOME _eLookupOutcome)
	{
		auto result = this->result_(E_FIRST_INPUT_RECOVERY_EFFECT::CreateFailed);
		result.eLookupOutcome = _eLookupOutcome;
		result.bCreateAttempted = true;
		std::optional<domain::S_CARD> card;
		try {
			card = m_CreateOneCard(_Event.sWholeText, _Event.eSource);
		} catch (...) {
			result.eCreateOutcome = E_FIRST_INPUT_ATTEMPT_OUTCOME::Exception;
			return result;
		}
		if (!card) {
			result.eCreateOutcome = E_FIRST_INPUT_ATTEMPT_OUTCOME::Rejected;
			return result;
		}
		result.eCreateOutcome = E_FIRST_INPUT_ATTEMPT_OUTCOME::Succeeded;
		result.sCreatedCardId = card->sId;
		m_pProjection->AddCard(*card);
		m_sPendingCardId = card->sId;
		m_ePhase = E_FIRST_INPUT_PHASE::PendingLink;
		result.ePhase = m_ePhase;
		result.sPendingCardId = m_sPendingCardId;
		return this->link_(*card, std::move(result));
	}

	S_FIRST_INPUT_RECOVERY_RESULT C_FIRST_INPUT_CAPTURE::OnInputEvent(const S_FIRST_INPUT_EVENT& _Event)
	{
		if (m_ePhase == E_FIRST_INPUT_PHASE::Connected) {
			return this->result_(E_FIRST_INPUT_RECOVERY_EFFECT::AlreadyConnected);
		}
		m_NewSnapshot = S_NEW_BACKING_SNAPSHOT{ _Event.sWholeText, _Event.nCursorQchar };
		if (!_Event.bInsertion) { return this->result_(E_FIRST_INPUT_RECOVERY_EFFECT::SnapshotUpdated); }

		if (m_ePhase == E_FIRST_INPUT_PHASE::PendingLink) {
			auto result = this->result_(E_FIRST_INPUT_RECOVERY_EFFECT::LinkPending);
			std::optional<domain::S_CARD> pending;
			try {
				pending = m_LookupActiveCard(*m_sPendingCardId);
			} catch (...) {
				result.eLookupOutcome = E_FIRST_INPUT_ATTEMPT_OUTCOME::Exception;
				return result;
			}
			if (pending && !pending->nDeletedAtUs) {
				result.eLookupOutcome = E_FIRST_INPUT_ATTEMPT_OUTCOME::Succeeded;
				return this->link_(*pending, std::move(result));
			}
			m_sPendingCardId.reset();
			m_ePhase = E_FIRST_INPUT_PHASE::Awaiting;
			if (m_ZeroParagraphs(_Event.sWholeText)) {
				result = this->result_(E_FIRST_INPUT_RECOVERY_EFFECT::SnapshotUpdated);
				result.eLookupOutcome = E_FIRST_INPUT_ATTEMPT_OUTCOME::Rejected;
				return result;
			}
			return this->create_and_link_(_Event, E_FIRST_INPUT_ATTEMPT_OUTCOME::Rejected);
		}

		if (m_ZeroParagraphs(_Event.sWholeText)) {
			return this->result_(E_FIRST_INPUT_RECOVERY_EFFECT::SnapshotUpdated);
		}
		return this->create_and_link_(_Event, E_FIRST_INPUT_ATTEMPT_OUTCOME::NotAttempted);
	}

	void C_FIRST_INPUT_CAPTURE::RestoreNewSnapshot(S_NEW_BACKING_SNAPSHOT _Snapshot)
	{
		m_NewSnapshot = std::move(_Snapshot);
		m_sPendingCardId.reset();
		m_sConnectedCardId.reset();
		m_ePhase = E_FIRST_INPUT_PHASE::Awaiting;
	}

	std::optional<S_NEW_BACKING_SNAPSHOT> C_FIRST_INPUT_CAPTURE::ProtectionSnapshot() const
	{
		return m_ePhase == E_FIRST_INPUT_PHASE::Connected ? std::nullopt : m_NewSnapshot;
	}

	void C_FIRST_INPUT_CAPTURE::ResetAfterAcceptedClose() noexcept
	{
		m_NewSnapshot.reset();
		m_sPendingCardId.reset();
		m_sConnectedCardId.reset();
		m_ePhase = E_FIRST_INPUT_PHASE::Awaiting;
	}
}
