#include "pynote/core/application/save_coordinator.h"

#include "pynote/core/storage/database.h"

#include <utility>

namespace pynote::core::application
{
	C_SAVE_COORDINATOR::C_SAVE_COORDINATOR(storage::C_DATABASE& _Database,
		storage::C_REPOSITORIES& _Repositories, C_DRAFT_COORDINATOR& _DraftCoordinator,
		SaveClock _Clock, SaveIdFactory _IdFactory, BeforeSaveTransaction _BeforeTransaction)
		: m_Database(_Database), m_Repositories(_Repositories), m_DraftCoordinator(_DraftCoordinator),
		  m_Clock(std::move(_Clock)), m_IdFactory(std::move(_IdFactory)),
		  m_BeforeTransaction(std::move(_BeforeTransaction)) {}

	S_SAVE_RESULT C_SAVE_COORDINATOR::rejected_(E_SAVE_ERROR _eError,
		std::optional<storage::E_REPO_RESULT> _eRepository) const
	{
		S_SAVE_RESULT result;
		result.eOutcome = E_SAVE_OUTCOME::Rejected;
		result.eError = _eError;
		result.eRepositoryResult = _eRepository;
		return result;
	}

	S_SAVE_RESULT C_SAVE_COORDINATOR::failed_(E_SAVE_ERROR _eError,
		std::optional<storage::E_REPO_RESULT> _eRepository) const
	{
		S_SAVE_RESULT result;
		result.eOutcome = E_SAVE_OUTCOME::Failed;
		result.eError = _eError;
		result.eRepositoryResult = _eRepository;
		return result;
	}

	S_SAVE_RESULT C_SAVE_COORDINATOR::repository_failed_(storage::E_REPO_RESULT _eResult) const
	{
		return this->failed_(_eResult == storage::E_REPO_RESULT::CasConflict ?
			E_SAVE_ERROR::CasConflict : E_SAVE_ERROR::RepositoryFailure, _eResult);
	}

	std::string C_SAVE_COORDINATOR::json_string_(const std::string& _sValue)
	{
		constexpr char hex[] = "0123456789abcdef";
		std::string output = "\"";
		for (const unsigned char byte : _sValue) {
			switch (byte) {
			case '"': output += "\\\""; break;
			case '\\': output += "\\\\"; break;
			case '\b': output += "\\b"; break;
			case '\f': output += "\\f"; break;
			case '\n': output += "\\n"; break;
			case '\r': output += "\\r"; break;
			case '\t': output += "\\t"; break;
			default:
				if (byte < 0x20) {
					output += "\\u00";
					output.push_back(hex[byte >> 4]);
					output.push_back(hex[byte & 15]);
				} else { output.push_back(static_cast<char>(byte)); }
				break;
			}
		}
		output.push_back('"');
		return output;
	}

	std::string C_SAVE_COORDINATOR::details_json_(
		const std::optional<std::string>& _sBaseRevisionId, bool _bIncludesPaste)
	{
		return "{\"base_revision_id\":" +
			(_sBaseRevisionId ? json_string_(*_sBaseRevisionId) : std::string("null")) +
			",\"includes_paste\":" + (_bIncludesPaste ? "true" : "false") + '}';
	}

	S_SAVE_RESULT C_SAVE_COORDINATOR::finalize_(E_SAVE_OUTCOME _eOutcome,
		const domain::S_CARD& _Card, const std::string& _sDraftId)
	{
		const auto finalized = m_DraftCoordinator.CompleteSave(
			_sDraftId, _Card.sBody, _Card.sCurrentRevisionId);
		if (finalized.eOutcome != E_DRAFT_OUTCOME::Ok) {
			S_SAVE_RESULT result;
			result.eOutcome = E_SAVE_OUTCOME::CommittedSessionFailure;
			result.eError = E_SAVE_ERROR::SessionFinalizationFailure;
			result.Card = _Card;
			result.DraftResult = finalized;
			return result;
		}
		S_SAVE_RESULT result;
		result.eOutcome = _eOutcome;
		result.Card = _Card;
		result.DraftResult = finalized;
		return result;
	}

	S_SAVE_RESULT C_SAVE_COORDINATOR::Save(const std::string& _sDraftId)
	{
		if (m_DraftCoordinator.IsImeComposing(_sDraftId)) {
			return this->rejected_(E_SAVE_ERROR::ImeComposing);
		}
		auto session = m_DraftCoordinator.Session(_sDraftId);
		if (!session) { return this->rejected_(E_SAVE_ERROR::MissingDraftSession); }
		if (!session->sCardId) { return this->rejected_(E_SAVE_ERROR::MissingCardIdentity); }

		domain::S_CARD initialCard;
		const auto initialRead = m_Repositories.GetCard(*session->sCardId, &initialCard);
		if (initialRead == storage::E_REPO_RESULT::NotFound ||
			(initialRead == storage::E_REPO_RESULT::Ok && initialCard.nDeletedAtUs)) {
			return this->rejected_(E_SAVE_ERROR::InactiveCard, initialRead);
		}
		if (initialRead != storage::E_REPO_RESULT::Ok) { return this->repository_failed_(initialRead); }

		if (m_BeforeTransaction) {
			try { m_BeforeTransaction(); }
			catch (...) { return this->failed_(E_SAVE_ERROR::TransactionFailure); }
		}
		const auto protectedDraft = m_DraftCoordinator.ProtectNow(_sDraftId);
		if (protectedDraft.eOutcome != E_DRAFT_OUTCOME::Ok &&
			protectedDraft.eOutcome != E_DRAFT_OUTCOME::NoOp) {
			auto result = this->failed_(E_SAVE_ERROR::DraftProtectionFailure,
				protectedDraft.eStorageResult == storage::E_REPO_RESULT::Ok ?
				std::nullopt : std::optional<storage::E_REPO_RESULT>(protectedDraft.eStorageResult));
			result.DraftResult = protectedDraft;
			return result;
		}
		session = m_DraftCoordinator.Session(_sDraftId);
		if (!session || !session->sCardId) {
			auto result = this->failed_(E_SAVE_ERROR::InvalidDraftSession);
			result.DraftResult = protectedDraft;
			return result;
		}
		const std::string draftHash = storage::TextHash(session->sText);

		storage::C_TRANSACTION transaction(m_Database);
		if (!transaction.IsActive()) { return this->failed_(E_SAVE_ERROR::TransactionFailure); }
		domain::S_CARD card;
		const auto lockedRead = m_Repositories.GetCard(*session->sCardId, &card);
		if (lockedRead == storage::E_REPO_RESULT::NotFound ||
			(lockedRead == storage::E_REPO_RESULT::Ok && card.nDeletedAtUs)) {
			return this->rejected_(E_SAVE_ERROR::InactiveCard, lockedRead);
		}
		if (lockedRead != storage::E_REPO_RESULT::Ok) { return this->repository_failed_(lockedRead); }

		if (session->sBaseRevisionId != card.sCurrentRevisionId) {
			std::string baseText;
			if (session->sBaseRevisionId) {
				domain::S_CARD_REVISION baseRevision;
				const auto baseRead = m_Repositories.GetRevision(*session->sBaseRevisionId, &baseRevision);
				if (baseRead == storage::E_REPO_RESULT::Ok) { baseText = baseRevision.sBody; }
				else if (baseRead != storage::E_REPO_RESULT::NotFound) { return this->repository_failed_(baseRead); }
			}
			if (!transaction.Commit()) { return this->failed_(E_SAVE_ERROR::TransactionFailure); }
			S_SAVE_RESULT result;
			result.eOutcome = E_SAVE_OUTCOME::Conflict;
			result.Card = card;
			result.Conflict = S_SAVE_CONFLICT{ card.sId, session->sBaseRevisionId,
				card.sCurrentRevisionId, std::move(baseText), card.sBody, session->sText };
			result.DraftResult = protectedDraft;
			return result;
		}

		if (draftHash == card.sBodyHash) {
			domain::S_DRAFT durableDraft;
			const auto draftRead = m_Repositories.GetDraft(_sDraftId, &durableDraft);
			if (draftRead == storage::E_REPO_RESULT::Ok) {
				const auto deleted = m_Repositories.DeleteDraft(_sDraftId);
				if (deleted != storage::E_REPO_RESULT::Ok) { return this->repository_failed_(deleted); }
			} else if (draftRead != storage::E_REPO_RESULT::NotFound) {
				return this->repository_failed_(draftRead);
			}
			if (!transaction.Commit()) { return this->failed_(E_SAVE_ERROR::TransactionFailure); }
			return this->finalize_(E_SAVE_OUTCOME::Unchanged, card, _sDraftId);
		}

		if (!card.sCurrentRevisionId) { return this->failed_(E_SAVE_ERROR::MissingCurrentRevision); }
		std::int64_t savedAtUs = 0;
		std::string revisionId;
		std::string eventId;
		try {
			savedAtUs = m_Clock();
			revisionId = m_IdFactory();
			eventId = m_IdFactory();
		} catch (...) { return this->failed_(E_SAVE_ERROR::RepositoryFailure); }

		domain::S_EDIT_EVENT event;
		event.sEventId = std::move(eventId);
		event.sDocumentId = card.sDocumentId;
		event.sCardId = card.sId;
		event.eEventType = domain::E_EVENT_TYPE::Update;
		event.eSource = domain::E_EVENT_SOURCE::Edit;
		event.nOccurredAtUs = savedAtUs;
		event.sDetailsJson = details_json_(session->sBaseRevisionId,
			m_DraftCoordinator.IncludesPaste(_sDraftId));
		domain::S_EDIT_EVENT storedEvent;
		auto repositoryResult = m_Repositories.CreateEvent(event, &storedEvent);
		if (repositoryResult != storage::E_REPO_RESULT::Ok) { return this->repository_failed_(repositoryResult); }
		if (!storedEvent.nEventSeq) { return this->failed_(E_SAVE_ERROR::MissingEventSequence); }

		domain::S_CARD_REVISION revision;
		revision.sId = revisionId;
		revision.sCardId = card.sId;
		revision.nEventSeq = *storedEvent.nEventSeq;
		revision.sParentRevisionId = card.sCurrentRevisionId;
		revision.sBody = session->sText;
		revision.sBodyHash = draftHash;
		revision.eSource = domain::E_REVISION_SOURCE::Edit;
		revision.nCreatedAtUs = savedAtUs;
		repositoryResult = m_Repositories.CreateRevision(revision);
		if (repositoryResult != storage::E_REPO_RESULT::Ok) { return this->repository_failed_(repositoryResult); }

		domain::S_CARD savedCard = card;
		savedCard.sBody = session->sText;
		savedCard.sBodyHash = draftHash;
		savedCard.sCurrentRevisionId = revisionId;
		savedCard.nUpdatedAtUs = savedAtUs;
		repositoryResult = m_Repositories.AdvanceCardRevision(savedCard, *card.sCurrentRevisionId);
		if (repositoryResult != storage::E_REPO_RESULT::Ok) { return this->repository_failed_(repositoryResult); }

		domain::S_DRAFT durableDraft;
		const auto draftRead = m_Repositories.GetDraft(_sDraftId, &durableDraft);
		if (draftRead == storage::E_REPO_RESULT::Ok) {
			repositoryResult = m_Repositories.DeleteDraft(_sDraftId);
			if (repositoryResult != storage::E_REPO_RESULT::Ok) { return this->repository_failed_(repositoryResult); }
		} else if (draftRead != storage::E_REPO_RESULT::NotFound) {
			return this->repository_failed_(draftRead);
		}
		repositoryResult = m_Repositories.TouchDocument(card.sDocumentId, savedAtUs);
		if (repositoryResult != storage::E_REPO_RESULT::Ok) { return this->repository_failed_(repositoryResult); }
		if (!transaction.Commit()) { return this->failed_(E_SAVE_ERROR::TransactionFailure); }
		return this->finalize_(E_SAVE_OUTCOME::Saved, savedCard, _sDraftId);
	}
}
