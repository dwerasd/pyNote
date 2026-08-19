#include "pynote/core/application/draft_coordinator.h"

#include "pynote/core/storage/database.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pynote::core::application
{
	C_REPOSITORY_DRAFT_STORE::C_REPOSITORY_DRAFT_STORE(storage::C_DATABASE& _Database,
		storage::C_REPOSITORIES& _Repositories)
		: m_Database(_Database), m_Repositories(_Repositories) {}

	storage::E_REPO_RESULT C_REPOSITORY_DRAFT_STORE::ListDocuments(
		std::vector<domain::S_DOCUMENT>* _pOut) { return m_Repositories.ListDocuments(_pOut); }
	storage::E_REPO_RESULT C_REPOSITORY_DRAFT_STORE::GetDocument(
		const std::string& _sId, domain::S_DOCUMENT* _pOut) { return m_Repositories.GetDocument(_sId, _pOut); }
	storage::E_REPO_RESULT C_REPOSITORY_DRAFT_STORE::ListDrafts(
		const std::string& _sDocumentId, std::vector<domain::S_DRAFT>* _pOut)
		{ return m_Repositories.ListDrafts(_sDocumentId, _pOut); }
	storage::E_REPO_RESULT C_REPOSITORY_DRAFT_STORE::GetDraft(
		const std::string& _sId, domain::S_DRAFT* _pOut) { return m_Repositories.GetDraft(_sId, _pOut); }
	storage::E_REPO_RESULT C_REPOSITORY_DRAFT_STORE::GetCard(
		const std::string& _sId, domain::S_CARD* _pOut) { return m_Repositories.GetCard(_sId, _pOut); }
	storage::E_REPO_RESULT C_REPOSITORY_DRAFT_STORE::GetRevision(
		const std::string& _sId, domain::S_CARD_REVISION* _pOut)
		{ return m_Repositories.GetRevision(_sId, _pOut); }

	storage::E_REPO_RESULT C_REPOSITORY_DRAFT_STORE::UpsertDraft(const domain::S_DRAFT& _Draft)
	{
		storage::C_TRANSACTION transaction(m_Database);
		if (!transaction.IsActive()) { return storage::E_REPO_RESULT::Failed; }
		domain::S_DRAFT existing;
		const auto found = m_Repositories.GetDraft(_Draft.sId, &existing);
		storage::E_REPO_RESULT result = storage::E_REPO_RESULT::Failed;
		if (found == storage::E_REPO_RESULT::NotFound) { result = m_Repositories.CreateDraft(_Draft); }
		else if (found == storage::E_REPO_RESULT::Ok) { result = m_Repositories.UpdateDraft(_Draft); }
		else { return found; }
		if (result != storage::E_REPO_RESULT::Ok) { return result; }
		return transaction.Commit() ? storage::E_REPO_RESULT::Ok : storage::E_REPO_RESULT::Failed;
	}

	storage::E_REPO_RESULT C_REPOSITORY_DRAFT_STORE::DeleteDraft(const std::string& _sId)
	{
		storage::C_TRANSACTION transaction(m_Database);
		if (!transaction.IsActive()) { return storage::E_REPO_RESULT::Failed; }
		domain::S_DRAFT existing;
		const auto found = m_Repositories.GetDraft(_sId, &existing);
		if (found != storage::E_REPO_RESULT::Ok) { return found; }
		const auto result = m_Repositories.DeleteDraft(_sId);
		if (result != storage::E_REPO_RESULT::Ok) { return result; }
		return transaction.Commit() ? storage::E_REPO_RESULT::Ok : storage::E_REPO_RESULT::Failed;
	}

	C_DRAFT_COORDINATOR::C_DRAFT_COORDINATOR(I_DRAFT_STORE& _Store, std::int64_t _nIdleMs,
		DraftClock _WallClockUs, DraftClock _AgeClockUs, DraftClock _PerformanceClockNs,
		DraftIdFactory _IdFactory, DraftTextHash _TextHash, DraftSchedulerSink _Scheduler,
		DraftProtectedSink _Protected, DraftFailedSink _Failed, DraftEmergencyWriter _EmergencyWriter)
		: m_Store(_Store), m_nIdleMs(_nIdleMs), m_WallClockUs(std::move(_WallClockUs)),
		  m_AgeClockUs(std::move(_AgeClockUs)), m_PerformanceClockNs(std::move(_PerformanceClockNs)),
		  m_IdFactory(std::move(_IdFactory)), m_TextHash(std::move(_TextHash)),
		  m_Scheduler(std::move(_Scheduler)), m_Protected(std::move(_Protected)),
		  m_Failed(std::move(_Failed)), m_EmergencyWriter(std::move(_EmergencyWriter))
	{
		if (m_nIdleMs < 0) { throw std::invalid_argument("draft idle interval must be nonnegative"); }
	}

	S_DRAFT_OPERATION_RESULT C_DRAFT_COORDINATOR::result_(E_DRAFT_OUTCOME _eOutcome,
		storage::E_REPO_RESULT _eStorage) const
	{
		S_DRAFT_OPERATION_RESULT result;
		result.eOutcome = _eOutcome;
		result.eStorageResult = _eStorage;
		return result;
	}

	S_DRAFT_OPERATION_RESULT C_DRAFT_COORDINATOR::session_result_(E_DRAFT_OUTCOME _eOutcome,
		const S_STATE& _State) const
	{
		auto result = this->result_(_eOutcome);
		result.Session = _State.Session;
		return result;
	}

	void C_DRAFT_COORDINATOR::SetIdleMs(std::int64_t _nIdleMs)
	{
		if (_nIdleMs < 0) { throw std::invalid_argument("draft idle interval must be nonnegative"); }
		m_nIdleMs = _nIdleMs;
		for (auto& [id, state] : m_States) {
			(void)id;
			if (state.nActiveGeneration) { this->arm_(state); }
		}
	}

	void C_DRAFT_COORDINATOR::arm_(S_STATE& _State)
	{
		if (_State.nActiveGeneration) { this->cancel_(_State); }
		const auto generation = ++m_nNextGeneration;
		_State.nActiveGeneration = generation;
		if (m_Scheduler) {
			m_Scheduler({ E_DRAFT_SCHEDULER_ACTION::Arm, _State.Session.sDraftId,
				generation, m_nIdleMs });
		}
	}

	void C_DRAFT_COORDINATOR::cancel_(S_STATE& _State)
	{
		if (!_State.nActiveGeneration) { return; }
		if (m_Scheduler) {
			m_Scheduler({ E_DRAFT_SCHEDULER_ACTION::Cancel, _State.Session.sDraftId,
				*_State.nActiveGeneration, 0 });
		}
		_State.nActiveGeneration.reset();
	}

	bool C_DRAFT_COORDINATOR::hash_matches_(const domain::S_DRAFT& _Draft)
	{
		return m_TextHash(_Draft.sDraftText) == _Draft.sDraftHash;
	}

	C_DRAFT_COORDINATOR::S_STATE C_DRAFT_COORDINATOR::state_from_draft_(
		const domain::S_DRAFT& _Draft, std::string _sCommittedText) const
	{
		S_STATE state;
		state.Session.sDraftId = _Draft.sId;
		state.Session.sDocumentId = _Draft.sDocumentId;
		state.Session.sCardId = _Draft.sCardId;
		state.Session.sBaseRevisionId = _Draft.sBaseRevisionId;
		state.Session.sText = _Draft.sDraftText;
		state.Session.bDirty = _Draft.sDraftText != _sCommittedText;
		state.Session.nCursorPositionQchar = _Draft.nCursorPositionQchar;
		state.sCommittedText = std::move(_sCommittedText);
		return state;
	}

	S_DRAFT_OPERATION_RESULT C_DRAFT_COORDINATOR::OpenCard(const domain::S_CARD& _Card,
		std::optional<E_DRAFT_DISPOSITION> _Disposition)
	{
		std::vector<domain::S_DRAFT> drafts;
		const auto listed = m_Store.ListDrafts(_Card.sDocumentId, &drafts);
		if (listed != storage::E_REPO_RESULT::Ok) {
			return this->result_(E_DRAFT_OUTCOME::StorageFailure, listed);
		}
		const auto found = std::find_if(drafts.begin(), drafts.end(), [&_Card](const auto& draft) {
			return draft.sCardId && *draft.sCardId == _Card.sId;
		});
		if (found != drafts.end()) {
			if (_Disposition == E_DRAFT_DISPOSITION::Discard) {
				const auto discarded = this->DiscardDraft(found->sId);
				if (discarded.eOutcome != E_DRAFT_OUTCOME::Ok) { return discarded; }
			} else {
				if (!this->hash_matches_(*found)) { return this->result_(E_DRAFT_OUTCOME::Corrupt); }
				if (!_Disposition || *_Disposition == E_DRAFT_DISPOSITION::Later) {
					return this->result_(E_DRAFT_OUTCOME::NoOp);
				}
				auto state = this->state_from_draft_(*found, _Card.sBody);
				const auto id = state.Session.sDraftId;
				m_States[id] = std::move(state);
				return this->session_result_(E_DRAFT_OUTCOME::Ok, m_States.at(id));
			}
		}

		S_STATE state;
		state.Session.sDraftId = m_IdFactory();
		state.Session.sDocumentId = _Card.sDocumentId;
		state.Session.sCardId = _Card.sId;
		state.Session.sBaseRevisionId = _Card.sCurrentRevisionId;
		state.Session.sText = _Card.sBody;
		state.sCommittedText = _Card.sBody;
		const auto id = state.Session.sDraftId;
		m_States[id] = std::move(state);
		return this->session_result_(E_DRAFT_OUTCOME::Ok, m_States.at(id));
	}

	S_DRAFT_OPERATION_RESULT C_DRAFT_COORDINATOR::OpenNew(const std::string& _sDocumentId)
	{
		domain::S_DOCUMENT document;
		const auto documentResult = m_Store.GetDocument(_sDocumentId, &document);
		if (documentResult == storage::E_REPO_RESULT::NotFound) {
			return this->result_(E_DRAFT_OUTCOME::NotFound, documentResult);
		}
		if (documentResult != storage::E_REPO_RESULT::Ok) {
			return this->result_(E_DRAFT_OUTCOME::StorageFailure, documentResult);
		}
		std::vector<domain::S_DRAFT> drafts;
		const auto listed = m_Store.ListDrafts(_sDocumentId, &drafts);
		if (listed != storage::E_REPO_RESULT::Ok) {
			return this->result_(E_DRAFT_OUTCOME::StorageFailure, listed);
		}
		auto found = drafts.end();
		for (auto it = drafts.begin(); it != drafts.end(); ++it) {
			if (it->eDraftKind != domain::E_DRAFT_KIND::New) { continue; }
			if (found == drafts.end() || std::tie(found->nUpdatedAtUs, found->sId) <
				std::tie(it->nUpdatedAtUs, it->sId)) { found = it; }
		}
		S_STATE state;
		if (found != drafts.end()) {
			if (!this->hash_matches_(*found)) { return this->result_(E_DRAFT_OUTCOME::Corrupt); }
			state = this->state_from_draft_(*found, "");
		} else {
			state.Session.sDraftId = m_IdFactory();
			state.Session.sDocumentId = _sDocumentId;
		}
		const auto id = state.Session.sDraftId;
		m_States[id] = std::move(state);
		return this->session_result_(E_DRAFT_OUTCOME::Ok, m_States.at(id));
	}

	S_DRAFT_OPERATION_RESULT C_DRAFT_COORDINATOR::UpdateSession(const std::string& _sDraftId,
		std::string _sText, std::int64_t _nCursorPositionQchar, bool _bIncludesPaste)
	{
		auto found = m_States.find(_sDraftId);
		if (found == m_States.end()) { return this->result_(E_DRAFT_OUTCOME::NotFound); }
		if (_nCursorPositionQchar < 0) { return this->result_(E_DRAFT_OUTCOME::Invalid); }
		auto& state = found->second;
		state.Session.sText = std::move(_sText);
		state.Session.nCursorPositionQchar = _nCursorPositionQchar;
		state.Session.bDirty = state.Session.sText != state.sCommittedText;
		state.bIncludesPaste = state.bIncludesPaste || _bIncludesPaste;
		if (!state.Session.bDirty) {
			this->cancel_(state);
			state.nAgeCheckpointUs.reset();
			return this->session_result_(E_DRAFT_OUTCOME::Ok, state);
		}
		const auto now = m_AgeClockUs();
		if (!state.nAgeCheckpointUs) { state.nAgeCheckpointUs = now; }
		else if (now - *state.nAgeCheckpointUs >= m_nIdleMs * 1000) {
			(void)this->safe_protect_(_sDraftId);
		}
		if (!state.bComposing) { this->arm_(state); }
		return this->session_result_(E_DRAFT_OUTCOME::Ok, state);
	}

	S_DRAFT_OPERATION_RESULT C_DRAFT_COORDINATOR::SetImeComposing(
		const std::string& _sDraftId, bool _bComposing)
	{
		auto found = m_States.find(_sDraftId);
		if (found == m_States.end()) { return this->result_(E_DRAFT_OUTCOME::NotFound); }
		auto& state = found->second;
		const bool wasComposing = state.bComposing;
		state.bComposing = _bComposing;
		if (_bComposing) { this->cancel_(state); }
		else if (wasComposing && state.Session.bDirty) { this->arm_(state); }
		return this->session_result_(E_DRAFT_OUTCOME::Ok, state);
	}

	bool C_DRAFT_COORDINATOR::IsImeComposing(const std::string& _sDraftId) const
	{
		const auto found = m_States.find(_sDraftId);
		return found != m_States.end() && found->second.bComposing;
	}

	bool C_DRAFT_COORDINATOR::IncludesPaste(const std::string& _sDraftId) const
	{
		const auto found = m_States.find(_sDraftId);
		return found != m_States.end() && found->second.bIncludesPaste;
	}

	C_DRAFT_COORDINATOR::S_SNAPSHOT C_DRAFT_COORDINATOR::snapshot_(const S_STATE& _State) const
	{
		return { _State.Session.sDraftId, _State.Session.sDocumentId, _State.Session.sCardId,
			_State.Session.sBaseRevisionId, _State.Session.sText,
			_State.Session.nCursorPositionQchar };
	}

	void C_DRAFT_COORDINATOR::report_failure_(const S_STATE& _State,
		storage::E_REPO_RESULT _eStorage)
	{
		if (m_Failed) { m_Failed({ _State.Session.sDraftId, _eStorage }); }
		if (m_EmergencyWriter) {
			const S_DRAFT_EMERGENCY_PAYLOAD payload{ _State.Session.sDraftId,
				_State.Session.sDocumentId, _State.Session.sCardId, _State.Session.sBaseRevisionId,
				_State.Session.sText, _State.Session.nCursorPositionQchar, m_WallClockUs() };
			try { (void)m_EmergencyWriter(payload); } catch (...) { }
		}
	}

	S_DRAFT_OPERATION_RESULT C_DRAFT_COORDINATOR::write_snapshot_(S_STATE& _State,
		const S_SNAPSHOT& _Snapshot, const std::string& _sHash)
	{
		const auto started = m_PerformanceClockNs();
		domain::S_DRAFT draft;
		draft.sId = _Snapshot.sDraftId;
		draft.sDocumentId = _Snapshot.sDocumentId;
		draft.sCardId = _Snapshot.sCardId;
		draft.eDraftKind = _Snapshot.sCardId ? domain::E_DRAFT_KIND::Edit : domain::E_DRAFT_KIND::New;
		draft.sBaseRevisionId = _Snapshot.sBaseRevisionId;
		draft.sDraftText = _Snapshot.sText;
		draft.sDraftHash = _sHash;
		draft.nCursorPositionQchar = _Snapshot.nCursorPositionQchar;
		draft.nUpdatedAtUs = m_WallClockUs();
		const auto stored = m_Store.UpsertDraft(draft);
		if (stored != storage::E_REPO_RESULT::Ok) {
			this->report_failure_(_State, stored);
			return this->result_(E_DRAFT_OUTCOME::StorageFailure, stored);
		}

		_State.LastWrittenKey = std::make_pair(_sHash, _Snapshot.nCursorPositionQchar);
		_State.nAgeCheckpointUs = m_AgeClockUs();
		const auto elapsedNs = (std::max)(std::int64_t{ 0 }, m_PerformanceClockNs() - started);
		S_DRAFT_WRITE_MEASUREMENT measurement{ draft.sId, _Snapshot.sText.size(),
			static_cast<double>(elapsedNs) / 1000000.0 };
		if (m_Protected) { m_Protected({ draft.sId, draft.nUpdatedAtUs, measurement }); }
		auto result = this->result_(E_DRAFT_OUTCOME::Ok);
		result.Measurement = measurement;
		return result;
	}

	S_DRAFT_OPERATION_RESULT C_DRAFT_COORDINATOR::ProtectNow(const std::string& _sDraftId)
	{
		auto found = m_States.find(_sDraftId);
		if (found == m_States.end()) { return this->result_(E_DRAFT_OUTCOME::NotFound); }
		auto& state = found->second;
		if (!state.Session.bDirty || state.bComposing) { return this->result_(E_DRAFT_OUTCOME::NoOp); }
		const auto current = this->snapshot_(state);
		if (state.bWriteInProgress) {
			state.QueuedSnapshot = current;
			return this->result_(E_DRAFT_OUTCOME::NoOp);
		}

		state.bWriteInProgress = true;
		S_DRAFT_OPERATION_RESULT finalResult = this->result_(E_DRAFT_OUTCOME::NoOp);
		std::optional<S_SNAPSHOT> pending = current;
		while (pending) {
			const std::string hash = m_TextHash(pending->sText);
			const auto key = std::make_pair(hash, pending->nCursorPositionQchar);
			if (state.LastWrittenKey && *state.LastWrittenKey == key) {
				state.nAgeCheckpointUs = m_AgeClockUs();
			} else {
				finalResult = this->write_snapshot_(state, *pending, hash);
				if (finalResult.eOutcome == E_DRAFT_OUTCOME::StorageFailure) {
					state.QueuedSnapshot.reset();
					break;
				}
			}
			pending = std::move(state.QueuedSnapshot);
			state.QueuedSnapshot.reset();
		}
		state.bWriteInProgress = false;
		return finalResult;
	}

	S_DRAFT_OPERATION_RESULT C_DRAFT_COORDINATOR::safe_protect_(const std::string& _sDraftId)
	{
		return this->ProtectNow(_sDraftId);
	}

	S_DRAFT_OPERATION_RESULT C_DRAFT_COORDINATOR::OnTimer(
		const std::string& _sDraftId, std::uint64_t _nGeneration)
	{
		auto found = m_States.find(_sDraftId);
		if (found == m_States.end()) { return this->result_(E_DRAFT_OUTCOME::NoOp); }
		auto& state = found->second;
		if (!state.nActiveGeneration || *state.nActiveGeneration != _nGeneration) {
			return this->result_(E_DRAFT_OUTCOME::NoOp);
		}
		state.nActiveGeneration.reset();
		return this->safe_protect_(_sDraftId);
	}

	S_DRAFT_CANDIDATES_RESULT C_DRAFT_COORDINATOR::RecoveryCandidates(
		const std::optional<std::string>& _sDocumentId)
	{
		S_DRAFT_CANDIDATES_RESULT output;
		std::vector<std::string> documentIds;
		if (_sDocumentId) { documentIds.push_back(*_sDocumentId); }
		else {
			std::vector<domain::S_DOCUMENT> documents;
			const auto listed = m_Store.ListDocuments(&documents);
			if (listed != storage::E_REPO_RESULT::Ok) {
				output.eOutcome = E_DRAFT_OUTCOME::StorageFailure;
				output.eStorageResult = listed;
				return output;
			}
			for (const auto& document : documents) { documentIds.push_back(document.sId); }
		}

		for (const auto& documentId : documentIds) {
			std::vector<domain::S_DRAFT> drafts;
			const auto listed = m_Store.ListDrafts(documentId, &drafts);
			if (listed != storage::E_REPO_RESULT::Ok) {
				output.Candidates.clear();
				output.eOutcome = E_DRAFT_OUTCOME::StorageFailure;
				output.eStorageResult = listed;
				return output;
			}
			for (const auto& draft : drafts) {
				if (draft.eDraftKind == domain::E_DRAFT_KIND::New || !this->hash_matches_(draft)) { continue; }
				S_DRAFT_RECOVERY_CANDIDATE candidate;
				candidate.Draft = draft;
				if (draft.sCardId) {
					domain::S_CARD card;
					const auto cardResult = m_Store.GetCard(*draft.sCardId, &card);
					if (cardResult == storage::E_REPO_RESULT::Ok) {
						candidate.sCommittedText = card.sBody;
						candidate.sCommittedRevisionId = card.sCurrentRevisionId;
						if (card.sCurrentRevisionId) {
							domain::S_CARD_REVISION revision;
							const auto revisionResult = m_Store.GetRevision(*card.sCurrentRevisionId, &revision);
							if (revisionResult == storage::E_REPO_RESULT::Ok) {
								candidate.bCommittedIsNewer = draft.nUpdatedAtUs <= revision.nCreatedAtUs;
							} else if (revisionResult != storage::E_REPO_RESULT::NotFound) {
								output.Candidates.clear(); output.eOutcome = E_DRAFT_OUTCOME::StorageFailure;
								output.eStorageResult = revisionResult; return output;
							}
						}
					} else if (cardResult != storage::E_REPO_RESULT::NotFound) {
						output.Candidates.clear(); output.eOutcome = E_DRAFT_OUTCOME::StorageFailure;
						output.eStorageResult = cardResult; return output;
					}
				}
				output.Candidates.push_back(std::move(candidate));
			}
		}
		return output;
	}

	S_DRAFT_OPERATION_RESULT C_DRAFT_COORDINATOR::ResolveCandidate(
		const std::string& _sDraftId, E_DRAFT_DISPOSITION _Disposition)
	{
		domain::S_DRAFT draft;
		const auto found = m_Store.GetDraft(_sDraftId, &draft);
		if (found == storage::E_REPO_RESULT::NotFound) {
			return this->result_(E_DRAFT_OUTCOME::NotFound, found);
		}
		if (found != storage::E_REPO_RESULT::Ok) {
			return this->result_(E_DRAFT_OUTCOME::StorageFailure, found);
		}
		if (_Disposition == E_DRAFT_DISPOSITION::Later) { return this->result_(E_DRAFT_OUTCOME::NoOp); }
		if (_Disposition == E_DRAFT_DISPOSITION::Discard) { return this->DiscardDraft(_sDraftId); }
		if (!this->hash_matches_(draft)) { return this->result_(E_DRAFT_OUTCOME::Corrupt); }
		std::string committed;
		if (draft.sCardId) {
			domain::S_CARD card;
			const auto cardResult = m_Store.GetCard(*draft.sCardId, &card);
			if (cardResult == storage::E_REPO_RESULT::NotFound) {
				return this->result_(E_DRAFT_OUTCOME::NotFound, cardResult);
			}
			if (cardResult != storage::E_REPO_RESULT::Ok) {
				return this->result_(E_DRAFT_OUTCOME::StorageFailure, cardResult);
			}
			committed = card.sBody;
		}
		auto state = this->state_from_draft_(draft, std::move(committed));
		m_States[_sDraftId] = std::move(state);
		return this->session_result_(E_DRAFT_OUTCOME::Ok, m_States.at(_sDraftId));
	}

	S_DRAFT_OPERATION_RESULT C_DRAFT_COORDINATOR::DiscardDraft(const std::string& _sDraftId)
	{
		const auto deleted = m_Store.DeleteDraft(_sDraftId);
		if (deleted == storage::E_REPO_RESULT::NotFound) {
			return this->result_(E_DRAFT_OUTCOME::NotFound, deleted);
		}
		if (deleted != storage::E_REPO_RESULT::Ok) {
			return this->result_(E_DRAFT_OUTCOME::StorageFailure, deleted);
		}
		auto state = m_States.find(_sDraftId);
		if (state != m_States.end()) { state->second.LastWrittenKey.reset(); }
		return this->result_(E_DRAFT_OUTCOME::Ok);
	}

	S_DRAFT_OPERATION_RESULT C_DRAFT_COORDINATOR::ReleaseSession(const std::string& _sDraftId)
	{
		auto found = m_States.find(_sDraftId);
		if (found == m_States.end()) { return this->result_(E_DRAFT_OUTCOME::NotFound); }
		this->cancel_(found->second);
		m_States.erase(found);
		return this->result_(E_DRAFT_OUTCOME::Ok);
	}

	S_DRAFT_OPERATION_RESULT C_DRAFT_COORDINATOR::DiscardSession(const std::string& _sDraftId)
	{
		auto discarded = this->DiscardDraft(_sDraftId);
		if (discarded.eOutcome != E_DRAFT_OUTCOME::Ok && discarded.eOutcome != E_DRAFT_OUTCOME::NotFound) {
			return discarded;
		}
		return this->ReleaseSession(_sDraftId);
	}

	S_DRAFT_OPERATION_RESULT C_DRAFT_COORDINATOR::CompleteSave(const std::string& _sDraftId,
		std::string _sCommittedText, std::optional<std::string> _sRevisionId)
	{
		auto found = m_States.find(_sDraftId);
		if (found == m_States.end()) { return this->result_(E_DRAFT_OUTCOME::NotFound); }
		auto& state = found->second;
		this->cancel_(state);
		state.Session.sText = _sCommittedText;
		state.sCommittedText = std::move(_sCommittedText);
		state.Session.sBaseRevisionId = std::move(_sRevisionId);
		state.Session.bDirty = false;
		state.bIncludesPaste = false;
		state.nAgeCheckpointUs.reset();
		state.LastWrittenKey.reset();
		return this->session_result_(E_DRAFT_OUTCOME::Ok, state);
	}

	std::optional<S_DRAFT_SESSION> C_DRAFT_COORDINATOR::Session(const std::string& _sDraftId) const
	{
		const auto found = m_States.find(_sDraftId);
		return found == m_States.end() ? std::nullopt : std::optional<S_DRAFT_SESSION>(found->second.Session);
	}

	std::vector<S_DOCUMENT_RECOVERY_PLAN> C_DRAFT_COORDINATOR::BuildRecoveryPlans(
		const std::vector<S_DRAFT_RECOVERY_CANDIDATE>& _Candidates,
		const std::vector<std::pair<std::string, std::optional<std::string>>>& _OpenedEditorCards)
	{
		std::vector<std::string> documentOrder;
		std::unordered_map<std::string, std::vector<std::string>> cards;
		std::unordered_map<std::string, std::unordered_set<std::string>> seen;
		for (const auto& candidate : _Candidates) {
			if (!candidate.Draft.sCardId) { continue; }
			const auto& documentId = candidate.Draft.sDocumentId;
			if (cards.find(documentId) == cards.end()) { documentOrder.push_back(documentId); }
			if (seen[documentId].insert(*candidate.Draft.sCardId).second) {
				cards[documentId].push_back(*candidate.Draft.sCardId);
			}
		}
		std::unordered_map<std::string, std::optional<std::string>> opened;
		for (const auto& value : _OpenedEditorCards) { opened[value.first] = value.second; }
		std::vector<S_DOCUMENT_RECOVERY_PLAN> plans;
		for (const auto& documentId : documentOrder) {
			const auto& candidates = cards.at(documentId);
			std::string display = candidates.front();
			const auto current = opened.find(documentId);
			if (current != opened.end() && current->second &&
				std::find(candidates.begin(), candidates.end(), *current->second) != candidates.end()) {
				display = *current->second;
			}
			S_DOCUMENT_RECOVERY_PLAN plan{ documentId, display, {} };
			for (const auto& card : candidates) { if (card != display) { plan.DeferredCardIds.push_back(card); } }
			plans.push_back(std::move(plan));
		}
		return plans;
	}
}
