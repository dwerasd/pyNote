#pragma once

#include "pynote/core/domain/card_list_projection.h"
#include "pynote/core/domain/models.h"
#include "pynote/core/storage/repositories.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace pynote::core::application
{
	struct S_DOCUMENT_UI_STATE
	{
		std::string sDocumentId;
		std::optional<std::string> sSelectedCardId{};
		std::int64_t nListScrollPosition{ 0 };
		domain::E_CARD_LIST_SORT_MODE eSortMode{ domain::E_CARD_LIST_SORT_MODE::Recency };
		std::optional<std::string> sEditorCardId{};
		std::optional<std::string> sEditorBaseRevisionId{};
		std::optional<std::int64_t> nEditorCursorQchar{};
		std::optional<std::pair<std::int64_t, std::int64_t>> EditorSplitSizes{};
		std::int64_t nUpdatedAtUs{ 0 };
		bool operator==(const S_DOCUMENT_UI_STATE&) const = default;
	};

	class C_WORKSPACE_STATE_STORE
	{
	public:
		C_WORKSPACE_STATE_STORE(storage::C_DATABASE& _Database,
			storage::C_REPOSITORIES& _Repositories, std::string _sWindowId);

		const std::string& WindowId() const noexcept { return m_sWindowId; }
		storage::E_REPO_RESULT LoadWorkspace(domain::S_WORKSPACE_WINDOW* _pOut);
		storage::E_REPO_RESULT SaveWorkspace(const std::vector<std::string>& _OpenDocumentIds,
			const std::optional<std::string>& _sActiveDocumentId, domain::S_WORKSPACE_WINDOW* _pOut);
		storage::E_REPO_RESULT LoadDocumentUiState(const std::string& _sDocumentId,
			S_DOCUMENT_UI_STATE* _pOut);
		storage::E_REPO_RESULT SaveDocumentUiState(const S_DOCUMENT_UI_STATE& _State);

	private:
		storage::C_DATABASE& m_Database;
		storage::C_REPOSITORIES& m_Repositories;
		std::string m_sWindowId;
	};

	struct S_WORKSPACE_RESTORE_PLAN_ENTRY
	{
		domain::S_WORKSPACE_WINDOW Workspace;
		bool bNeedsRewrite{ false };
		bool operator==(const S_WORKSPACE_RESTORE_PLAN_ENTRY&) const = default;
	};

	std::vector<S_WORKSPACE_RESTORE_PLAN_ENTRY> BuildWorkspaceRestorePlan(
		const std::vector<domain::S_WORKSPACE_WINDOW>& _Records,
		const std::set<std::string>& _EligibleDocumentIds);
}
