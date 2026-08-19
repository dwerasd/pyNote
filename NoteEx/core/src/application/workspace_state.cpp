#include "pynote/core/application/workspace_state.h"

#include "pynote/core/storage/database.h"

#include <sqlite3/sqlite3.h>

#include <algorithm>
#include <string_view>
#include <utility>

namespace pynote::core::application
{
	namespace
	{
		using storage::E_REPO_RESULT;

		std::string_view sort_text(domain::E_CARD_LIST_SORT_MODE _eMode) noexcept
		{
			switch (_eMode) {
			case domain::E_CARD_LIST_SORT_MODE::Recency: return "recency";
			case domain::E_CARD_LIST_SORT_MODE::Position: return "position";
			case domain::E_CARD_LIST_SORT_MODE::Capture: return "capture";
			}
			return {};
		}

		bool parse_sort(const unsigned char* _pText, domain::E_CARD_LIST_SORT_MODE* _pOut) noexcept
		{
			if (_pText == nullptr) { return false; }
			const std::string_view text(reinterpret_cast<const char*>(_pText));
			if (text == "recency") { *_pOut = domain::E_CARD_LIST_SORT_MODE::Recency; return true; }
			if (text == "position") { *_pOut = domain::E_CARD_LIST_SORT_MODE::Position; return true; }
			if (text == "capture") { *_pOut = domain::E_CARD_LIST_SORT_MODE::Capture; return true; }
			return false;
		}

		std::optional<std::string> nullable_text(sqlite3_stmt* _pStatement, int _nColumn)
		{
			if (::sqlite3_column_type(_pStatement, _nColumn) == SQLITE_NULL) { return std::nullopt; }
			const auto* text = ::sqlite3_column_text(_pStatement, _nColumn);
			return text == nullptr ? std::optional<std::string>{} :
				std::optional<std::string>(reinterpret_cast<const char*>(text));
		}

		bool bind_text(sqlite3_stmt* _pStatement, int _nIndex, std::string_view _sValue)
		{
			return ::sqlite3_bind_text(_pStatement, _nIndex, _sValue.data(),
				static_cast<int>(_sValue.size()), SQLITE_TRANSIENT) == SQLITE_OK;
		}

		bool bind_nullable_text(sqlite3_stmt* _pStatement, int _nIndex,
			const std::optional<std::string>& _sValue)
		{
			return _sValue ? bind_text(_pStatement, _nIndex, *_sValue) :
				::sqlite3_bind_null(_pStatement, _nIndex) == SQLITE_OK;
		}

		bool bind_nullable_int64(sqlite3_stmt* _pStatement, int _nIndex,
			const std::optional<std::int64_t>& _nValue)
		{
			return _nValue ? ::sqlite3_bind_int64(_pStatement, _nIndex, *_nValue) == SQLITE_OK :
				::sqlite3_bind_null(_pStatement, _nIndex) == SQLITE_OK;
		}

		E_REPO_RESULT fail(storage::C_DATABASE& _Database)
		{
			_Database.SetLastError(::sqlite3_errmsg(_Database.Handle()));
			return E_REPO_RESULT::Failed;
		}
	}

	C_WORKSPACE_STATE_STORE::C_WORKSPACE_STATE_STORE(storage::C_DATABASE& _Database,
		storage::C_REPOSITORIES& _Repositories, std::string _sWindowId)
		: m_Database(_Database), m_Repositories(_Repositories), m_sWindowId(std::move(_sWindowId))
	{
	}

	E_REPO_RESULT C_WORKSPACE_STATE_STORE::LoadWorkspace(domain::S_WORKSPACE_WINDOW* _pOut)
	{
		const auto result = m_Repositories.GetWorkspaceWindow(m_sWindowId, _pOut);
		if (result != E_REPO_RESULT::NotFound) { return result; }
		*_pOut = {};
		_pOut->sWindowId = m_sWindowId;
		return E_REPO_RESULT::Ok;
	}

	E_REPO_RESULT C_WORKSPACE_STATE_STORE::SaveWorkspace(const std::vector<std::string>& _OpenDocumentIds,
		const std::optional<std::string>& _sActiveDocumentId, domain::S_WORKSPACE_WINDOW* _pOut)
	{
		return m_Repositories.SaveWorkspaceWindow(m_sWindowId, _OpenDocumentIds, _sActiveDocumentId, _pOut);
	}

	E_REPO_RESULT C_WORKSPACE_STATE_STORE::LoadDocumentUiState(const std::string& _sDocumentId,
		S_DOCUMENT_UI_STATE* _pOut)
	{
		*_pOut = {};
		_pOut->sDocumentId = _sDocumentId;
		constexpr const char* sql = R"SQL(
            SELECT document_id, selected_card_id, list_scroll_position, sort_mode,
                   editor_card_id, editor_base_revision_id, editor_cursor_qchar,
                   editor_split_left, editor_split_right, updated_at_us
            FROM document_ui_states
            WHERE document_id = ?
            )SQL";
		sqlite3_stmt* statement = nullptr;
		if (::sqlite3_prepare_v2(m_Database.Handle(), sql, -1, &statement, nullptr) != SQLITE_OK) {
			return fail(m_Database);
		}
		if (!bind_text(statement, 1, _sDocumentId)) {
			::sqlite3_finalize(statement); return fail(m_Database);
		}
		const int step = ::sqlite3_step(statement);
		if (step == SQLITE_DONE) { ::sqlite3_finalize(statement); return E_REPO_RESULT::Ok; }
		if (step != SQLITE_ROW) { ::sqlite3_finalize(statement); return fail(m_Database); }

		S_DOCUMENT_UI_STATE state;
		const auto* documentId = ::sqlite3_column_text(statement, 0);
		if (documentId == nullptr || !parse_sort(::sqlite3_column_text(statement, 3), &state.eSortMode)) {
			m_Database.SetLastError("document_ui_states row is invalid");
			::sqlite3_finalize(statement); return E_REPO_RESULT::Invalid;
		}
		state.sDocumentId = reinterpret_cast<const char*>(documentId);
		state.sSelectedCardId = nullable_text(statement, 1);
		state.nListScrollPosition = ::sqlite3_column_int64(statement, 2);
		state.sEditorCardId = nullable_text(statement, 4);
		state.sEditorBaseRevisionId = nullable_text(statement, 5);
		if (::sqlite3_column_type(statement, 6) != SQLITE_NULL) {
			state.nEditorCursorQchar = ::sqlite3_column_int64(statement, 6);
		}
		const bool leftNull = ::sqlite3_column_type(statement, 7) == SQLITE_NULL;
		const bool rightNull = ::sqlite3_column_type(statement, 8) == SQLITE_NULL;
		if (!leftNull && !rightNull) {
			state.EditorSplitSizes = std::pair(::sqlite3_column_int64(statement, 7),
				::sqlite3_column_int64(statement, 8));
		}
		state.nUpdatedAtUs = ::sqlite3_column_int64(statement, 9);
		::sqlite3_finalize(statement);
		*_pOut = std::move(state);
		return E_REPO_RESULT::Ok;
	}

	E_REPO_RESULT C_WORKSPACE_STATE_STORE::SaveDocumentUiState(const S_DOCUMENT_UI_STATE& _State)
	{
		const std::string_view sort = sort_text(_State.eSortMode);
		if (sort.empty()) { return E_REPO_RESULT::Invalid; }
		constexpr const char* sql = R"SQL(
            INSERT INTO document_ui_states(
                document_id, selected_card_id, list_scroll_position,
                sort_mode, editor_card_id, editor_base_revision_id,
                editor_cursor_qchar, editor_split_left,
                editor_split_right, updated_at_us
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(document_id) DO UPDATE SET
                selected_card_id = excluded.selected_card_id,
                list_scroll_position = excluded.list_scroll_position,
                sort_mode = excluded.sort_mode,
                editor_card_id = excluded.editor_card_id,
                editor_base_revision_id = excluded.editor_base_revision_id,
                editor_cursor_qchar = excluded.editor_cursor_qchar,
                editor_split_left = excluded.editor_split_left,
                editor_split_right = excluded.editor_split_right,
                updated_at_us = excluded.updated_at_us
            )SQL";
		storage::C_TRANSACTION transaction(m_Database);
		if (!transaction.IsActive()) { return E_REPO_RESULT::Failed; }
		sqlite3_stmt* statement = nullptr;
		if (::sqlite3_prepare_v2(m_Database.Handle(), sql, -1, &statement, nullptr) != SQLITE_OK) {
			return fail(m_Database);
		}
		const std::optional<std::int64_t> splitLeft = _State.EditorSplitSizes ?
			std::optional<std::int64_t>(_State.EditorSplitSizes->first) : std::nullopt;
		const std::optional<std::int64_t> splitRight = _State.EditorSplitSizes ?
			std::optional<std::int64_t>(_State.EditorSplitSizes->second) : std::nullopt;
		const bool bound = bind_text(statement, 1, _State.sDocumentId)
			&& bind_nullable_text(statement, 2, _State.sSelectedCardId)
			&& ::sqlite3_bind_int64(statement, 3, _State.nListScrollPosition) == SQLITE_OK
			&& bind_text(statement, 4, sort)
			&& bind_nullable_text(statement, 5, _State.sEditorCardId)
			&& bind_nullable_text(statement, 6, _State.sEditorBaseRevisionId)
			&& bind_nullable_int64(statement, 7, _State.nEditorCursorQchar)
			&& bind_nullable_int64(statement, 8, splitLeft)
			&& bind_nullable_int64(statement, 9, splitRight)
			&& ::sqlite3_bind_int64(statement, 10, _State.nUpdatedAtUs) == SQLITE_OK;
		if (!bound || ::sqlite3_step(statement) != SQLITE_DONE) {
			::sqlite3_finalize(statement); return fail(m_Database);
		}
		::sqlite3_finalize(statement);
		return transaction.Commit() ? E_REPO_RESULT::Ok : E_REPO_RESULT::Failed;
	}

	std::vector<S_WORKSPACE_RESTORE_PLAN_ENTRY> BuildWorkspaceRestorePlan(
		const std::vector<domain::S_WORKSPACE_WINDOW>& _Records,
		const std::set<std::string>& _EligibleDocumentIds)
	{
		std::set<std::string> claimed;
		std::vector<S_WORKSPACE_RESTORE_PLAN_ENTRY> plan;
		plan.reserve(_Records.size());
		for (const auto& input : _Records) {
			std::vector<std::string> candidates;
			for (const auto& documentId : input.OpenDocumentIds) {
				if (_EligibleDocumentIds.contains(documentId) && !claimed.contains(documentId)) {
					candidates.push_back(documentId);
				}
			}
			std::optional<std::string> retained;
			if (input.sActiveDocumentId &&
				std::find(candidates.begin(), candidates.end(), *input.sActiveDocumentId) != candidates.end()) {
				retained = input.sActiveDocumentId;
			} else if (!candidates.empty()) {
				retained = candidates.front();
			}
			domain::S_WORKSPACE_WINDOW output = input;
			output.OpenDocumentIds = retained ? std::vector<std::string>{*retained} : std::vector<std::string>{};
			output.sActiveDocumentId = retained;
			if (retained) { claimed.insert(*retained); }
			plan.push_back({ output, output.OpenDocumentIds != input.OpenDocumentIds ||
				output.sActiveDocumentId != input.sActiveDocumentId });
		}
		return plan;
	}
}
