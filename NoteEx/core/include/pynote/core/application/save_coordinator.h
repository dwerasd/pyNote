#pragma once

#include "pynote/core/application/draft_coordinator.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace pynote::core::application
{
	enum class E_SAVE_OUTCOME { Saved, Unchanged, Conflict, Rejected, Failed, CommittedSessionFailure };
	enum class E_SAVE_ERROR
	{
		None,
		ImeComposing,
		MissingCardIdentity,
		InactiveCard,
		MissingDraftSession,
		InvalidDraftSession,
		DraftProtectionFailure,
		MissingCurrentRevision,
		MissingEventSequence,
		RepositoryFailure,
		CasConflict,
		TransactionFailure,
		SessionFinalizationFailure
	};

	struct S_SAVE_CONFLICT
	{
		std::string sCardId;
		std::optional<std::string> sBaseRevisionId{};
		std::optional<std::string> sCurrentRevisionId{};
		std::string sBaseText;
		std::string sCommittedText;
		std::string sDraftText;
	};

	struct S_SAVE_RESULT
	{
		E_SAVE_OUTCOME eOutcome{ E_SAVE_OUTCOME::Failed };
		E_SAVE_ERROR eError{ E_SAVE_ERROR::None };
		std::optional<storage::E_REPO_RESULT> eRepositoryResult{};
		std::optional<domain::S_CARD> Card{};
		std::optional<S_SAVE_CONFLICT> Conflict{};
		std::optional<S_DRAFT_OPERATION_RESULT> DraftResult{};
	};

	using SaveClock = std::function<std::int64_t()>;
	using SaveIdFactory = std::function<std::string()>;
	using BeforeSaveTransaction = std::function<void()>;

	class C_SAVE_COORDINATOR
	{
	public:
		C_SAVE_COORDINATOR(storage::C_DATABASE& _Database, storage::C_REPOSITORIES& _Repositories,
			C_DRAFT_COORDINATOR& _DraftCoordinator, SaveClock _Clock,
			SaveIdFactory _IdFactory, BeforeSaveTransaction _BeforeTransaction = {});

		S_SAVE_RESULT Save(const std::string& _sDraftId);

	private:
		S_SAVE_RESULT rejected_(E_SAVE_ERROR _eError,
			std::optional<storage::E_REPO_RESULT> _eRepository = std::nullopt) const;
		S_SAVE_RESULT failed_(E_SAVE_ERROR _eError,
			std::optional<storage::E_REPO_RESULT> _eRepository = std::nullopt) const;
		S_SAVE_RESULT repository_failed_(storage::E_REPO_RESULT _eResult) const;
		S_SAVE_RESULT finalize_(E_SAVE_OUTCOME _eOutcome, const domain::S_CARD& _Card,
			const std::string& _sDraftId);
		static std::string details_json_(const std::optional<std::string>& _sBaseRevisionId,
			bool _bIncludesPaste);
		static std::string json_string_(const std::string& _sValue);

		storage::C_DATABASE& m_Database;
		storage::C_REPOSITORIES& m_Repositories;
		C_DRAFT_COORDINATOR& m_DraftCoordinator;
		SaveClock m_Clock;
		SaveIdFactory m_IdFactory;
		BeforeSaveTransaction m_BeforeTransaction;
	};
}
