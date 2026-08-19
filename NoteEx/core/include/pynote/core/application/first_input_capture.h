#pragma once

#include "pynote/core/application/card_service.h"
#include "pynote/core/domain/card_list_projection.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace pynote::core::application
{
	enum class E_FIRST_INPUT_PHASE { Awaiting, PendingLink, Connected };
	enum class E_FIRST_INPUT_EFFECT { CreationFailed, ConnectCreatedCard, AlreadyConnected };
	enum class E_FIRST_INPUT_ATTEMPT_OUTCOME { NotAttempted, Succeeded, Rejected, Exception };
	enum class E_FIRST_INPUT_RECOVERY_EFFECT { SnapshotUpdated, CreateFailed, LinkPending, Connected, AlreadyConnected };

	struct S_NEW_BACKING_SNAPSHOT
	{
		std::string sText;
		std::int64_t nCursorQchar{ 0 };
		bool operator==(const S_NEW_BACKING_SNAPSHOT&) const = default;
	};

	struct S_FIRST_INPUT_EVENT
	{
		std::string sWholeText;
		std::int64_t nCursorQchar{ 0 };
		domain::E_CAPTURE_OPERATION_SOURCE eSource{ domain::E_CAPTURE_OPERATION_SOURCE::Typing };
		bool bInsertion{ false };
	};

	using ZeroParagraphPredicate = std::function<bool(const std::string&)>;
	using CreateOneCardPort = std::function<std::optional<domain::S_CARD>(
		const std::string&, domain::E_CAPTURE_OPERATION_SOURCE)>;
	using ActiveCardLookupPort = std::function<std::optional<domain::S_CARD>(const std::string&)>;
	using LinkCardPort = std::function<bool(const domain::S_CARD&)>;

	struct S_FIRST_INPUT_RECOVERY_RESULT
	{
		E_FIRST_INPUT_PHASE ePhase{ E_FIRST_INPUT_PHASE::Awaiting };
		E_FIRST_INPUT_RECOVERY_EFFECT eEffect{ E_FIRST_INPUT_RECOVERY_EFFECT::SnapshotUpdated };
		E_FIRST_INPUT_ATTEMPT_OUTCOME eCreateOutcome{ E_FIRST_INPUT_ATTEMPT_OUTCOME::NotAttempted };
		E_FIRST_INPUT_ATTEMPT_OUTCOME eLookupOutcome{ E_FIRST_INPUT_ATTEMPT_OUTCOME::NotAttempted };
		E_FIRST_INPUT_ATTEMPT_OUTCOME eLinkOutcome{ E_FIRST_INPUT_ATTEMPT_OUTCOME::NotAttempted };
		bool bCreateAttempted{ false };
		bool bLinkAttempted{ false };
		std::optional<std::string> sCreatedCardId{};
		std::optional<std::string> sPendingCardId{};
		std::optional<std::string> sConnectedCardId{};
	};

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
		C_FIRST_INPUT_CAPTURE(domain::C_CARD_LIST_PROJECTION& _Projection,
			ZeroParagraphPredicate _ZeroParagraphs, CreateOneCardPort _CreateOneCard,
			ActiveCardLookupPort _LookupActiveCard, LinkCardPort _LinkCard);

		S_FIRST_INPUT_RESULT OnMeaningfulInsertion(const std::string& _sWholeText,
			domain::E_CAPTURE_OPERATION_SOURCE _eSource);
		S_FIRST_INPUT_RECOVERY_RESULT OnInputEvent(const S_FIRST_INPUT_EVENT& _Event);
		void RestoreNewSnapshot(S_NEW_BACKING_SNAPSHOT _Snapshot);
		std::optional<S_NEW_BACKING_SNAPSHOT> ProtectionSnapshot() const;
		void ResetAfterAcceptedClose() noexcept;

		E_FIRST_INPUT_PHASE Phase() const noexcept { return m_ePhase; }
		const std::optional<std::string>& ConnectedCardId() const noexcept { return m_sConnectedCardId; }

	private:
		S_FIRST_INPUT_RECOVERY_RESULT result_(E_FIRST_INPUT_RECOVERY_EFFECT _eEffect) const;
		S_FIRST_INPUT_RECOVERY_RESULT create_and_link_(const S_FIRST_INPUT_EVENT& _Event,
			E_FIRST_INPUT_ATTEMPT_OUTCOME _eLookupOutcome);
		S_FIRST_INPUT_RECOVERY_RESULT link_(const domain::S_CARD& _Card,
			S_FIRST_INPUT_RECOVERY_RESULT _Result);

		C_CARD_SERVICE* m_pService{};
		domain::C_CARD_LIST_PROJECTION* m_pProjection{};
		std::string m_sDocumentId;
		ZeroParagraphPredicate m_ZeroParagraphs;
		CreateOneCardPort m_CreateOneCard;
		ActiveCardLookupPort m_LookupActiveCard;
		LinkCardPort m_LinkCard;
		E_FIRST_INPUT_PHASE m_ePhase{ E_FIRST_INPUT_PHASE::Awaiting };
		std::optional<S_NEW_BACKING_SNAPSHOT> m_NewSnapshot{};
		std::optional<std::string> m_sPendingCardId{};
		std::optional<std::string> m_sConnectedCardId{};
	};
}
