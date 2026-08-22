#include "pynote/core/application/window_lifecycle.h"
#include "pynote/core/application/workspace_state.h"

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

using namespace pynote::core;

namespace
{
	application::S_WINDOW_LIFECYCLE_PARTICIPANT participant(
		application::WINDOW_TOKEN _Token, std::vector<std::string>& _Log,
		application::E_LEAVE_RESULT _eLeave = application::E_LEAVE_RESULT::ApprovedClean)
	{
		return {
			_Token,
			[&_Log, _Token]() { _Log.push_back("protect:" + std::to_string(_Token)); return(true); },
			[&_Log, _Token, _eLeave]() { _Log.push_back("leave:" + std::to_string(_Token)); return(_eLeave); },
			[&_Log, _Token]() { _Log.push_back("persist:" + std::to_string(_Token)); return(true); },
			[&_Log, _Token]() { _Log.push_back("cleanup:" + std::to_string(_Token)); return(true); },
			[&_Log, _Token]() { _Log.push_back("delete:" + std::to_string(_Token)); },
			[&_Log, _Token]() { _Log.push_back("release:" + std::to_string(_Token)); },
			[&_Log, _Token]() { _Log.push_back("destroy:" + std::to_string(_Token)); },
		};
	}
}

TEST_CASE("registry enforces one document owner and active transitions", "[W3-multi-window-lifecycle]")
{
	application::C_WINDOW_REGISTRY Registry;
	const auto First = Registry.Register("workspace-1", "document-1");
	const auto Second = Registry.Register("workspace-2", "document-2");
	REQUIRE(First != 0);
	REQUIRE(Second != 0);
	REQUIRE(Registry.Register("workspace-duplicate", "document-1") == 0);
	REQUIRE(Registry.ActiveWindow() == First);
	REQUIRE(Registry.Activate(Second));
	REQUIRE(Registry.ActiveWindow() == Second);
	REQUIRE(Registry.ReleaseOwnership(Second));
	REQUIRE(Registry.ActiveWindow() == First);
	REQUIRE(Registry.DocumentOwner("document-2") == std::nullopt);
	REQUIRE(Registry.Retire(Second));
}

TEST_CASE("restore filters ineligible duplicate owners and chooses recent unowned document",
	"[W3-multi-window-lifecycle][WTL-CAP-FI-011][WTL-CAP-FI-012][WTL-CAP-PL-008]")
{
	std::vector<domain::S_WORKSPACE_WINDOW> Records{
		{ "window-1", { "eligible-1", "trashed" }, "eligible-1", 1 },
		{ "window-2", { "eligible-1", "eligible-2" }, "eligible-1", 2 },
	};
	const auto Plan = application::BuildWorkspaceRestorePlan(Records, { "eligible-1", "eligible-2" });
	REQUIRE(Plan[0].Workspace.OpenDocumentIds == std::vector<std::string>{ "eligible-1" });
	REQUIRE(Plan[1].Workspace.OpenDocumentIds == std::vector<std::string>{ "eligible-2" });
	REQUIRE(Plan[1].Workspace.sActiveDocumentId == "eligible-2");
	const std::vector<application::S_RESTORABLE_DOCUMENT> Documents{
		{ "older", 10, 9 }, { "owned", 30, 1 }, { "recent", 20, 2 },
	};
	REQUIRE(application::ChooseRecentUnownedDocument(Documents, { "owned" }) == "recent");
	REQUIRE(application::ChooseRecentUnownedDocument(Documents, { "older", "owned", "recent" }) == std::nullopt);
}

TEST_CASE("single close follows exact order and cancellation is non-mutating",
	"[W3-multi-window-lifecycle][WTL-CAP-FI-120][WTL-CAP-PL-022]")
{
	application::C_WINDOW_LIFECYCLE Lifecycle;
	std::vector<std::string> Log;
	auto ProtectFailed = participant(1, Log);
	ProtectFailed.Protect = [&]() { Log.push_back("protect-failed:1"); return(false); };
	REQUIRE_FALSE(Lifecycle.CloseWindow(ProtectFailed, false));
	REQUIRE(Log == std::vector<std::string>{ "protect-failed:1" });
	Log.clear();
	auto Denied = participant(1, Log, application::E_LEAVE_RESULT::Denied);
	REQUIRE_FALSE(Lifecycle.CloseWindow(Denied, false));
	REQUIRE(Log == std::vector<std::string>{ "protect:1", "leave:1" });
	Log.clear();
	auto Approved = participant(1, Log);
	REQUIRE(Lifecycle.CloseWindow(Approved, false));
	REQUIRE(Log == std::vector<std::string>{
		"protect:1", "leave:1", "persist:1", "cleanup:1", "delete:1", "release:1", "destroy:1" });
}

TEST_CASE("dirty leave denial and approved save paths are distinct", "[W3-multi-window-lifecycle]")
{
	application::C_WINDOW_LIFECYCLE Lifecycle;
	std::vector<std::string> Log;
	auto DirtyDenied = participant(1, Log, application::E_LEAVE_RESULT::Denied);
	REQUIRE_FALSE(Lifecycle.CloseWindow(DirtyDenied, false));
	Log.clear();
	auto DirtySaved = participant(2, Log, application::E_LEAVE_RESULT::ApprovedAfterSave);
	REQUIRE(Lifecycle.CloseWindow(DirtySaved, false));
	REQUIRE(Log[0] == "protect:2");
	REQUIRE(Log[1] == "leave:2");
	REQUIRE(Log[2] == "persist:2");
}

TEST_CASE("application quit persists all windows before any cleanup",
	"[W3-multi-window-lifecycle][WTL-CAP-FI-121][WTL-CAP-PL-021][WTL-CAP-PL-023][WTL-CAP-NC-032]")
{
	application::C_WINDOW_LIFECYCLE Blocked;
	std::vector<std::string> BlockedLog;
	auto First = participant(1, BlockedLog);
	auto Second = participant(2, BlockedLog);
	Second.Protect = [&]() -> bool { BlockedLog.push_back("protect-throw:2"); throw std::runtime_error("injected"); };
	REQUIRE_FALSE(Blocked.QuitApplication({ First, Second }));
	REQUIRE(BlockedLog == std::vector<std::string>{ "protect:1", "protect-throw:2" });

	application::C_WINDOW_LIFECYCLE Lifecycle;
	std::vector<std::string> Log;
	REQUIRE(Lifecycle.QuitApplication({ participant(1, Log), participant(2, Log) }));
	REQUIRE(Log == std::vector<std::string>{
		"protect:1", "protect:2", "leave:1", "leave:2", "persist:1", "persist:2", "cleanup:1", "cleanup:2",
		"release:1", "destroy:1", "release:2", "destroy:2" });
}

TEST_CASE("cleanup false and exception do not skip later participants", "[W3-multi-window-lifecycle][WTL-CAP-PL-025]")
{
	application::C_WINDOW_LIFECYCLE Lifecycle;
	std::vector<std::string> Log;
	auto First = participant(1, Log);
	First.Cleanup = [&]() { Log.push_back("cleanup-false:1"); return(false); };
	auto Second = participant(2, Log);
	Second.Cleanup = [&]() -> bool { Log.push_back("cleanup-throw:2"); throw std::runtime_error("injected"); };
	auto Third = participant(3, Log);
	REQUIRE(Lifecycle.QuitApplication({ First, Second, Third }));
	REQUIRE(std::find(Log.begin(), Log.end(), "cleanup:3") != Log.end());
	REQUIRE(std::find(Log.begin(), Log.end(), "destroy:3") != Log.end());
	const std::vector<application::S_WINDOW_CLEANUP_FAILURE> Expected{ { 1, false }, { 2, true } };
	REQUIRE(Lifecycle.CleanupFailures() == Expected);
}

TEST_CASE("non-last close deletes restoration while last and full quit preserve it",
	"[W3-multi-window-lifecycle][WTL-CAP-PL-024][WTL-CAP-NC-035]")
{
	std::vector<std::string> NonLastLog;
	application::C_WINDOW_LIFECYCLE NonLast;
	REQUIRE(NonLast.CloseWindow(participant(1, NonLastLog), false));
	REQUIRE(std::find(NonLastLog.begin(), NonLastLog.end(), "delete:1") != NonLastLog.end());
	std::vector<std::string> LastLog;
	application::C_WINDOW_LIFECYCLE Last;
	REQUIRE(Last.CloseWindow(participant(2, LastLog), true));
	REQUIRE(std::find(LastLog.begin(), LastLog.end(), "delete:2") == LastLog.end());
	std::vector<std::string> QuitLog;
	application::C_WINDOW_LIFECYCLE Quit;
	REQUIRE(Quit.QuitApplication({ participant(3, QuitLog), participant(4, QuitLog) }));
	REQUIRE(std::none_of(QuitLog.begin(), QuitLog.end(), [](const auto& _Value) { return(_Value.starts_with("delete:")); }));
}

TEST_CASE("duplicate close and reentrant quit are idempotent", "[W3-multi-window-lifecycle]")
{
	application::C_WINDOW_LIFECYCLE CloseLifecycle;
	std::vector<std::string> CloseLog;
	auto Window = participant(1, CloseLog);
	Window.RequestLeave = [&]() {
		CloseLog.push_back("leave:1");
		REQUIRE_FALSE(CloseLifecycle.CloseWindow(Window, false));
		return(application::E_LEAVE_RESULT::ApprovedClean);
	};
	REQUIRE(CloseLifecycle.CloseWindow(Window, false));
	const auto Size = CloseLog.size();
	REQUIRE_FALSE(CloseLifecycle.CloseWindow(Window, false));
	REQUIRE(CloseLog.size() == Size);
	application::C_WINDOW_LIFECYCLE QuitLifecycle;
	std::vector<std::string> QuitLog;
	auto QuitWindow = participant(2, QuitLog);
	QuitWindow.RequestLeave = [&]() {
		QuitLog.push_back("leave:2");
		REQUIRE_FALSE(QuitLifecycle.QuitApplication({ QuitWindow }));
		return(application::E_LEAVE_RESULT::ApprovedClean);
	};
	REQUIRE(QuitLifecycle.QuitApplication({ QuitWindow }));
}

TEST_CASE("stale destroyed callbacks and shutdown new-window callbacks are ignored", "[W3-multi-window-lifecycle]")
{
	application::C_WINDOW_REGISTRY Registry;
	const auto Token = Registry.Register("workspace", "document");
	REQUIRE(Registry.ReleaseOwnership(Token));
	REQUIRE(Registry.Retire(Token));
	REQUIRE_FALSE(Registry.Retire(Token));
	REQUIRE_FALSE(Registry.Activate(Token));
	application::C_WINDOW_LIFECYCLE Lifecycle;
	std::vector<std::string> Log;
	REQUIRE(Lifecycle.QuitApplication({ participant(1, Log) }));
	REQUIRE_FALSE(Lifecycle.AcceptsNewWindows());
	REQUIRE_FALSE(Lifecycle.QuitApplication({ participant(1, Log) }));
}
