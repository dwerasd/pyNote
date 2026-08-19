#include <catch_amalgamated.hpp>

#include "pynote/core/application/workspace_state.h"
#include "pynote/core/storage/migration_runner.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#pragma comment(lib, "NoteExCore")

namespace
{
	namespace app = pynote::core::application;
	namespace domain = pynote::core::domain;
	namespace storage = pynote::core::storage;

	void emit(std::string_view id, std::string_view payload)
	{
		wchar_t path[32768] = {};
		const DWORD length = ::GetEnvironmentVariableW(L"PYNOTE_WORKSPACE_STATE_GOLDEN_OUT", path, 32768);
		if (length == 0 || length >= 32768) { return; }
		std::ofstream output(std::filesystem::path(path), std::ios::binary |
			(id == "WTL-W2-0052" ? std::ios::trunc : std::ios::app));
		REQUIRE(output.is_open()); output << id << '|' << payload << '\n'; REQUIRE(output.good());
	}

	std::string sort_name(domain::E_CARD_LIST_SORT_MODE mode)
	{
		if (mode == domain::E_CARD_LIST_SORT_MODE::Position) { return "position"; }
		if (mode == domain::E_CARD_LIST_SORT_MODE::Capture) { return "capture"; }
		return "recency";
	}

	class Fixture
	{
	public:
		Fixture()
			: path_(std::filesystem::temp_directory_path() /
				("noteex_w2r10_workspace_" + std::to_string(::GetCurrentProcessId()) + "_" +
				std::to_string(++sequence_) + ".db")), repositories_(database_)
		{
			remove_(); REQUIRE(database_.Open(path_.string()));
			storage::C_MIGRATION_RUNNER runner; runner.SetExistingDatabase(false, path_.string());
			REQUIRE(runner.Run(database_) == storage::E_MIGRATE_RESULT::Ok);
		}
		~Fixture() { database_.Close(); remove_(); }

		void AddDocument(const std::string& id, std::int64_t time = 1000)
		{
			domain::S_DOCUMENT document; document.sId = id; document.sTitle = id;
			document.nCreatedAtUs = time; document.nUpdatedAtUs = time;
			REQUIRE(repositories_.CreateDocument(document) == storage::E_REPO_RESULT::Ok);
		}

		app::C_WORKSPACE_STATE_STORE Store(const std::string& windowId)
		{
			return app::C_WORKSPACE_STATE_STORE(database_, repositories_, windowId);
		}

		std::vector<domain::S_WORKSPACE_WINDOW> Workspaces()
		{
			std::vector<domain::S_WORKSPACE_WINDOW> values;
			REQUIRE(repositories_.ListWorkspaceWindows(&values) == storage::E_REPO_RESULT::Ok);
			return values;
		}

	private:
		void remove_() const
		{
			std::error_code error; std::filesystem::remove(path_, error);
			std::filesystem::remove(path_.string() + "-wal", error);
			std::filesystem::remove(path_.string() + "-shm", error);
		}

		std::filesystem::path path_;
		storage::C_DATABASE database_;
		storage::C_REPOSITORIES repositories_;
		inline static int sequence_{ 0 };
	};

	std::string plan_text(const std::vector<app::S_WORKSPACE_RESTORE_PLAN_ENTRY>& plan)
	{
		std::string value;
		for (const auto& entry : plan) {
			if (!value.empty()) { value += ';'; }
			value += entry.Workspace.sWindowId + ':';
			for (std::size_t index = 0; index < entry.Workspace.OpenDocumentIds.size(); ++index) {
				if (index != 0) { value += ','; }
				value += entry.Workspace.OpenDocumentIds[index];
			}
			value += '@' + entry.Workspace.sActiveDocumentId.value_or("-");
		}
		return value;
	}

	std::string rewrites(const std::vector<app::S_WORKSPACE_RESTORE_PLAN_ENTRY>& plan)
	{
		std::string value;
		for (const auto& entry : plan) {
			if (!entry.bNeedsRewrite) { continue; }
			if (!value.empty()) { value += ','; }
			value += entry.Workspace.sWindowId;
		}
		return value.empty() ? "-" : value;
	}

	#define WORKSPACE_TAGS(ID) "[W2-R10][core][application][workspace-state][" ID "]"
}

TEST_CASE("WTL-W2-0052", WORKSPACE_TAGS("WTL-W2-0052"))
{
	Fixture fixture; fixture.AddDocument("first"); auto store = fixture.Store("window-first");
	domain::S_WORKSPACE_WINDOW saved; REQUIRE(store.SaveWorkspace({"first"}, "first", &saved) == storage::E_REPO_RESULT::Ok);
	app::S_DOCUMENT_UI_STATE state; state.sDocumentId = "first"; state.nListScrollPosition = 137;
	state.eSortMode = domain::E_CARD_LIST_SORT_MODE::Capture; state.nEditorCursorQchar = 9; state.nUpdatedAtUs = 2000;
	REQUIRE(store.SaveDocumentUiState(state) == storage::E_REPO_RESULT::Ok);
	domain::S_WORKSPACE_WINDOW workspace; app::S_DOCUMENT_UI_STATE restored;
	REQUIRE(store.LoadWorkspace(&workspace) == storage::E_REPO_RESULT::Ok);
	REQUIRE(store.LoadDocumentUiState("first", &restored) == storage::E_REPO_RESULT::Ok);
	REQUIRE(workspace.OpenDocumentIds == std::vector<std::string>{"first"}); REQUIRE(workspace.sActiveDocumentId == "first");
	REQUIRE(restored.nListScrollPosition == 137); REQUIRE(restored.eSortMode == domain::E_CARD_LIST_SORT_MODE::Capture);
	REQUIRE(restored.nEditorCursorQchar == 9); REQUIRE_FALSE(restored.sSelectedCardId); REQUIRE_FALSE(restored.sEditorCardId);
	REQUIRE_FALSE(restored.sEditorBaseRevisionId); REQUIRE_FALSE(restored.EditorSplitSizes);
	emit("WTL-W2-0052", "workspace=first|active=first|selected=-|scroll=137|sort=capture|editor=-|base=-|cursor=9|split=-");
}

TEST_CASE("WTL-W2-0053", WORKSPACE_TAGS("WTL-W2-0053"))
{
	Fixture fixture; fixture.AddDocument("sort-document"); auto store = fixture.Store("sort-window");
	app::S_DOCUMENT_UI_STATE state; REQUIRE(store.LoadDocumentUiState("sort-document", &state) == storage::E_REPO_RESULT::Ok);
	REQUIRE(state.eSortMode == domain::E_CARD_LIST_SORT_MODE::Recency);
	state.eSortMode = domain::E_CARD_LIST_SORT_MODE::Position; state.nUpdatedAtUs = 2000;
	REQUIRE(store.SaveDocumentUiState(state) == storage::E_REPO_RESULT::Ok);
	app::S_DOCUMENT_UI_STATE restart; REQUIRE(store.LoadDocumentUiState("sort-document", &restart) == storage::E_REPO_RESULT::Ok);
	REQUIRE(restart.eSortMode == domain::E_CARD_LIST_SORT_MODE::Position);
	restart.eSortMode = domain::E_CARD_LIST_SORT_MODE::Recency; restart.nUpdatedAtUs = 3000;
	REQUIRE(store.SaveDocumentUiState(restart) == storage::E_REPO_RESULT::Ok);
	app::S_DOCUMENT_UI_STATE second; REQUIRE(store.LoadDocumentUiState("sort-document", &second) == storage::E_REPO_RESULT::Ok);
	REQUIRE(second.eSortMode == domain::E_CARD_LIST_SORT_MODE::Recency);
	emit("WTL-W2-0053", "initial=recency|stored=position|restart=position|resaved=recency|second_restart=recency");
}

TEST_CASE("WTL-W2-0081", WORKSPACE_TAGS("WTL-W2-0081"))
{
	Fixture fixture; fixture.AddDocument("legacy-a", 1); fixture.AddDocument("legacy-b", 2);
	domain::S_WORKSPACE_WINDOW saved; auto first = fixture.Store("legacy-window-1"); auto second = fixture.Store("legacy-window-2");
	REQUIRE(first.SaveWorkspace({"legacy-a","legacy-b"}, "legacy-a", &saved) == storage::E_REPO_RESULT::Ok);
	REQUIRE(second.SaveWorkspace({"legacy-b"}, "legacy-b", &saved) == storage::E_REPO_RESULT::Ok);
	const auto plan = app::BuildWorkspaceRestorePlan(fixture.Workspaces(), {"legacy-a","legacy-b"});
	REQUIRE(plan.size() == 2); REQUIRE(plan[0].bNeedsRewrite); REQUIRE_FALSE(plan[1].bNeedsRewrite);
	REQUIRE(plan_text(plan) == "legacy-window-1:legacy-a@legacy-a;legacy-window-2:legacy-b@legacy-b");
	emit("WTL-W2-0081", "input=legacy-window-1:legacy-a,legacy-b@legacy-a;legacy-window-2:legacy-b@legacy-b|output=" + plan_text(plan) + "|rewrites=" + rewrites(plan));
}

TEST_CASE("WTL-W2-0082", WORKSPACE_TAGS("WTL-W2-0082"))
{
	Fixture fixture; fixture.AddDocument("claimed-a", 1); fixture.AddDocument("unclaimed-b", 2);
	domain::S_WORKSPACE_WINDOW saved; auto first = fixture.Store("claimed-window-1"); auto second = fixture.Store("claimed-window-2");
	REQUIRE(first.SaveWorkspace({"claimed-a"}, "claimed-a", &saved) == storage::E_REPO_RESULT::Ok);
	REQUIRE(second.SaveWorkspace({"claimed-a","unclaimed-b"}, "claimed-a", &saved) == storage::E_REPO_RESULT::Ok);
	const auto plan = app::BuildWorkspaceRestorePlan(fixture.Workspaces(), {"claimed-a","unclaimed-b"});
	REQUIRE(plan.size() == 2); REQUIRE_FALSE(plan[0].bNeedsRewrite); REQUIRE(plan[1].bNeedsRewrite);
	REQUIRE(plan_text(plan) == "claimed-window-1:claimed-a@claimed-a;claimed-window-2:unclaimed-b@unclaimed-b");
	emit("WTL-W2-0082", "input=claimed-window-1:claimed-a@claimed-a;claimed-window-2:claimed-a,unclaimed-b@claimed-a|output=" + plan_text(plan) + "|rewrites=" + rewrites(plan));
}

#undef WORKSPACE_TAGS
