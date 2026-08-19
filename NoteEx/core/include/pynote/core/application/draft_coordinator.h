#pragma once

#include "pynote/core/domain/models.h"
#include "pynote/core/storage/repositories.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pynote::core::application
{
	enum class E_DRAFT_DISPOSITION { Recover, Discard, Later };
	enum class E_DRAFT_OUTCOME { Ok, NoOp, NotFound, Invalid, Corrupt, StorageFailure };
	enum class E_DRAFT_SCHEDULER_ACTION { Arm, Cancel };

	struct S_DRAFT_SESSION
	{
		std::string sDraftId;
		std::string sDocumentId;
		std::optional<std::string> sCardId{};
		std::optional<std::string> sBaseRevisionId{};
		std::string sText;
		bool bDirty{ false };
		std::int64_t nCursorPositionQchar{ 0 };
		bool operator==(const S_DRAFT_SESSION&) const = default;
	};

	struct S_DRAFT_RECOVERY_CANDIDATE
	{
		domain::S_DRAFT Draft;
		std::string sCommittedText;
		std::optional<std::string> sCommittedRevisionId{};
		bool bCommittedIsNewer{ false };
	};

	struct S_DOCUMENT_RECOVERY_PLAN
	{
		std::string sDocumentId;
		std::string sDisplayCardId;
		std::vector<std::string> DeferredCardIds;
		bool operator==(const S_DOCUMENT_RECOVERY_PLAN&) const = default;
	};

	struct S_DRAFT_WRITE_MEASUREMENT
	{
		std::string sDraftId;
		std::size_t nTextBytes{ 0 };
		double dElapsedMs{ 0.0 };
	};

	struct S_DRAFT_SCHEDULER_COMMAND
	{
		E_DRAFT_SCHEDULER_ACTION eAction{ E_DRAFT_SCHEDULER_ACTION::Arm };
		std::string sDraftId;
		std::uint64_t nGeneration{ 0 };
		std::int64_t nDelayMs{ 0 };
	};

	struct S_DRAFT_PROTECTED_EVENT
	{
		std::string sDraftId;
		std::int64_t nUpdatedAtUs{ 0 };
		S_DRAFT_WRITE_MEASUREMENT Measurement;
	};

	struct S_DRAFT_FAILED_EVENT
	{
		std::string sDraftId;
		storage::E_REPO_RESULT eStorageResult{ storage::E_REPO_RESULT::Failed };
	};

	struct S_DRAFT_EMERGENCY_PAYLOAD
	{
		std::string sDraftId;
		std::string sDocumentId;
		std::optional<std::string> sCardId{};
		std::optional<std::string> sBaseRevisionId{};
		std::string sDraftText;
		std::int64_t nCursorPositionQchar{ 0 };
		std::int64_t nWrittenAtUs{ 0 };
	};

	struct S_DRAFT_OPERATION_RESULT
	{
		E_DRAFT_OUTCOME eOutcome{ E_DRAFT_OUTCOME::NoOp };
		storage::E_REPO_RESULT eStorageResult{ storage::E_REPO_RESULT::Ok };
		std::optional<S_DRAFT_SESSION> Session{};
		std::optional<S_DRAFT_WRITE_MEASUREMENT> Measurement{};
	};

	struct S_DRAFT_CANDIDATES_RESULT
	{
		E_DRAFT_OUTCOME eOutcome{ E_DRAFT_OUTCOME::Ok };
		storage::E_REPO_RESULT eStorageResult{ storage::E_REPO_RESULT::Ok };
		std::vector<S_DRAFT_RECOVERY_CANDIDATE> Candidates;
	};

	class I_DRAFT_STORE
	{
	public:
		virtual ~I_DRAFT_STORE() = default;
		virtual storage::E_REPO_RESULT ListDocuments(std::vector<domain::S_DOCUMENT>* _pOut) = 0;
		virtual storage::E_REPO_RESULT GetDocument(const std::string& _sId, domain::S_DOCUMENT* _pOut) = 0;
		virtual storage::E_REPO_RESULT ListDrafts(const std::string& _sDocumentId,
			std::vector<domain::S_DRAFT>* _pOut) = 0;
		virtual storage::E_REPO_RESULT GetDraft(const std::string& _sId, domain::S_DRAFT* _pOut) = 0;
		virtual storage::E_REPO_RESULT GetCard(const std::string& _sId, domain::S_CARD* _pOut) = 0;
		virtual storage::E_REPO_RESULT GetRevision(const std::string& _sId,
			domain::S_CARD_REVISION* _pOut) = 0;
		virtual storage::E_REPO_RESULT UpsertDraft(const domain::S_DRAFT& _Draft) = 0;
		virtual storage::E_REPO_RESULT DeleteDraft(const std::string& _sId) = 0;
	};

	class C_REPOSITORY_DRAFT_STORE final : public I_DRAFT_STORE
	{
	public:
		C_REPOSITORY_DRAFT_STORE(storage::C_DATABASE& _Database, storage::C_REPOSITORIES& _Repositories);

		storage::E_REPO_RESULT ListDocuments(std::vector<domain::S_DOCUMENT>* _pOut) override;
		storage::E_REPO_RESULT GetDocument(const std::string& _sId, domain::S_DOCUMENT* _pOut) override;
		storage::E_REPO_RESULT ListDrafts(const std::string& _sDocumentId,
			std::vector<domain::S_DRAFT>* _pOut) override;
		storage::E_REPO_RESULT GetDraft(const std::string& _sId, domain::S_DRAFT* _pOut) override;
		storage::E_REPO_RESULT GetCard(const std::string& _sId, domain::S_CARD* _pOut) override;
		storage::E_REPO_RESULT GetRevision(const std::string& _sId,
			domain::S_CARD_REVISION* _pOut) override;
		storage::E_REPO_RESULT UpsertDraft(const domain::S_DRAFT& _Draft) override;
		storage::E_REPO_RESULT DeleteDraft(const std::string& _sId) override;

	private:
		storage::C_DATABASE& m_Database;
		storage::C_REPOSITORIES& m_Repositories;
	};

	using DraftClock = std::function<std::int64_t()>;
	using DraftIdFactory = std::function<std::string()>;
	using DraftTextHash = std::function<std::string(const std::string&)>;
	using DraftSchedulerSink = std::function<void(const S_DRAFT_SCHEDULER_COMMAND&)>;
	using DraftProtectedSink = std::function<void(const S_DRAFT_PROTECTED_EVENT&)>;
	using DraftFailedSink = std::function<void(const S_DRAFT_FAILED_EVENT&)>;
	using DraftEmergencyWriter = std::function<bool(const S_DRAFT_EMERGENCY_PAYLOAD&)>;

	class C_DRAFT_COORDINATOR
	{
	public:
		C_DRAFT_COORDINATOR(I_DRAFT_STORE& _Store, std::int64_t _nIdleMs,
			DraftClock _WallClockUs, DraftClock _AgeClockUs, DraftClock _PerformanceClockNs,
			DraftIdFactory _IdFactory, DraftTextHash _TextHash = storage::TextHash,
			DraftSchedulerSink _Scheduler = {}, DraftProtectedSink _Protected = {},
			DraftFailedSink _Failed = {}, DraftEmergencyWriter _EmergencyWriter = {});

		void SetIdleMs(std::int64_t _nIdleMs);
		S_DRAFT_OPERATION_RESULT OpenCard(const domain::S_CARD& _Card,
			std::optional<E_DRAFT_DISPOSITION> _Disposition = std::nullopt);
		S_DRAFT_OPERATION_RESULT OpenNew(const std::string& _sDocumentId);
		S_DRAFT_OPERATION_RESULT UpdateSession(const std::string& _sDraftId,
			std::string _sText, std::int64_t _nCursorPositionQchar, bool _bIncludesPaste = false);
		S_DRAFT_OPERATION_RESULT SetImeComposing(const std::string& _sDraftId, bool _bComposing);
		bool IsImeComposing(const std::string& _sDraftId) const;
		bool IncludesPaste(const std::string& _sDraftId) const;
		S_DRAFT_OPERATION_RESULT ProtectNow(const std::string& _sDraftId);
		S_DRAFT_OPERATION_RESULT OnTimer(const std::string& _sDraftId, std::uint64_t _nGeneration);
		S_DRAFT_CANDIDATES_RESULT RecoveryCandidates(
			const std::optional<std::string>& _sDocumentId = std::nullopt);
		S_DRAFT_OPERATION_RESULT ResolveCandidate(const std::string& _sDraftId,
			E_DRAFT_DISPOSITION _Disposition);
		S_DRAFT_OPERATION_RESULT DiscardDraft(const std::string& _sDraftId);
		S_DRAFT_OPERATION_RESULT ReleaseSession(const std::string& _sDraftId);
		S_DRAFT_OPERATION_RESULT DiscardSession(const std::string& _sDraftId);
		S_DRAFT_OPERATION_RESULT CompleteSave(const std::string& _sDraftId,
			std::string _sCommittedText, std::optional<std::string> _sRevisionId);
		std::optional<S_DRAFT_SESSION> Session(const std::string& _sDraftId) const;

		static std::vector<S_DOCUMENT_RECOVERY_PLAN> BuildRecoveryPlans(
			const std::vector<S_DRAFT_RECOVERY_CANDIDATE>& _Candidates,
			const std::vector<std::pair<std::string, std::optional<std::string>>>& _OpenedEditorCards);

	private:
		struct S_SNAPSHOT
		{
			std::string sDraftId;
			std::string sDocumentId;
			std::optional<std::string> sCardId{};
			std::optional<std::string> sBaseRevisionId{};
			std::string sText;
			std::int64_t nCursorPositionQchar{ 0 };
		};

		struct S_STATE
		{
			S_DRAFT_SESSION Session;
			std::string sCommittedText;
			bool bIncludesPaste{ false };
			bool bComposing{ false };
			bool bWriteInProgress{ false };
			std::optional<S_SNAPSHOT> QueuedSnapshot{};
			std::optional<std::int64_t> nAgeCheckpointUs{};
			std::optional<std::pair<std::string, std::int64_t>> LastWrittenKey{};
			std::optional<std::uint64_t> nActiveGeneration{};
		};

		S_DRAFT_OPERATION_RESULT result_(E_DRAFT_OUTCOME _eOutcome,
			storage::E_REPO_RESULT _eStorage = storage::E_REPO_RESULT::Ok) const;
		S_DRAFT_OPERATION_RESULT session_result_(E_DRAFT_OUTCOME _eOutcome, const S_STATE& _State) const;
		S_DRAFT_OPERATION_RESULT safe_protect_(const std::string& _sDraftId);
		S_DRAFT_OPERATION_RESULT write_snapshot_(S_STATE& _State, const S_SNAPSHOT& _Snapshot,
			const std::string& _sHash);
		S_SNAPSHOT snapshot_(const S_STATE& _State) const;
		void arm_(S_STATE& _State);
		void cancel_(S_STATE& _State);
		bool hash_matches_(const domain::S_DRAFT& _Draft);
		S_STATE state_from_draft_(const domain::S_DRAFT& _Draft, std::string _sCommittedText) const;
		void report_failure_(const S_STATE& _State, storage::E_REPO_RESULT _eStorage);

		I_DRAFT_STORE& m_Store;
		std::int64_t m_nIdleMs{ 0 };
		DraftClock m_WallClockUs;
		DraftClock m_AgeClockUs;
		DraftClock m_PerformanceClockNs;
		DraftIdFactory m_IdFactory;
		DraftTextHash m_TextHash;
		DraftSchedulerSink m_Scheduler;
		DraftProtectedSink m_Protected;
		DraftFailedSink m_Failed;
		DraftEmergencyWriter m_EmergencyWriter;
		std::uint64_t m_nNextGeneration{ 0 };
		std::unordered_map<std::string, S_STATE> m_States;
	};
}
