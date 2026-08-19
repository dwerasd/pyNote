#include "pynote/core/application/first_input_capture.h"

#include <utility>

namespace pynote::core::application
{
	C_FIRST_INPUT_CAPTURE::C_FIRST_INPUT_CAPTURE(C_CARD_SERVICE& _Service,
		domain::C_CARD_LIST_PROJECTION& _Projection, std::string _sDocumentId)
		: m_Service(_Service), m_Projection(_Projection), m_sDocumentId(std::move(_sDocumentId))
	{
	}

	S_FIRST_INPUT_RESULT C_FIRST_INPUT_CAPTURE::OnMeaningfulInsertion(const std::string& _sWholeText,
		domain::E_CAPTURE_OPERATION_SOURCE _eSource)
	{
		if (m_ePhase == E_FIRST_INPUT_PHASE::Connected) {
			return { std::nullopt, E_FIRST_INPUT_EFFECT::AlreadyConnected, m_sConnectedCardId, false };
		}

		domain::S_CARD card;
		const auto outcome = m_Service.CreateCard(m_sDocumentId, _sWholeText, _eSource, std::nullopt, &card);
		if (outcome != E_CARD_SERVICE_RESULT::Ok) {
			return { outcome, E_FIRST_INPUT_EFFECT::CreationFailed, std::nullopt, true };
		}

		m_Projection.AddCard(card);
		m_sConnectedCardId = card.sId;
		m_ePhase = E_FIRST_INPUT_PHASE::Connected;
		return { outcome, E_FIRST_INPUT_EFFECT::ConnectCreatedCard, m_sConnectedCardId, true };
	}

	void C_FIRST_INPUT_CAPTURE::ResetAfterAcceptedClose() noexcept
	{
		m_sConnectedCardId.reset();
		m_ePhase = E_FIRST_INPUT_PHASE::Awaiting;
	}
}
