#pragma once

#include "pynote/core/application/card_service.h"
#include "pynote/core/domain/card_list_projection.h"

#include <optional>
#include <string>

namespace pynote::core::application
{
	enum class E_FIRST_INPUT_PHASE { Awaiting, Connected };
	enum class E_FIRST_INPUT_EFFECT { CreationFailed, ConnectCreatedCard, AlreadyConnected };

	struct S_FIRST_INPUT_RESULT
	{
		std::optional<E_CARD_SERVICE_RESULT> eServiceOutcome{};
		E_FIRST_INPUT_EFFECT eEffect{ E_FIRST_INPUT_EFFECT::CreationFailed };
		std::optional<std::string> sConnectedCardId{};
		bool bCreateAttempted{ false };
	};

	class C_FIRST_INPUT_CAPTURE
	{
	public:
		C_FIRST_INPUT_CAPTURE(C_CARD_SERVICE& _Service, domain::C_CARD_LIST_PROJECTION& _Projection,
			std::string _sDocumentId);

		S_FIRST_INPUT_RESULT OnMeaningfulInsertion(const std::string& _sWholeText,
			domain::E_CAPTURE_OPERATION_SOURCE _eSource);
		void ResetAfterAcceptedClose() noexcept;

		E_FIRST_INPUT_PHASE Phase() const noexcept { return m_ePhase; }
		const std::optional<std::string>& ConnectedCardId() const noexcept { return m_sConnectedCardId; }

	private:
		C_CARD_SERVICE& m_Service;
		domain::C_CARD_LIST_PROJECTION& m_Projection;
		std::string m_sDocumentId;
		E_FIRST_INPUT_PHASE m_ePhase{ E_FIRST_INPUT_PHASE::Awaiting };
		std::optional<std::string> m_sConnectedCardId{};
	};
}
