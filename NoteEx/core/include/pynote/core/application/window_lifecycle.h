#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace pynote::core::application
{
	using WINDOW_TOKEN = std::uint64_t;

	struct S_WINDOW_REGISTRATION
	{
		WINDOW_TOKEN Token{ 0 };
		std::string sWorkspaceId;
		std::optional<std::string> sDocumentId{};
		std::uint64_t nOrdinal{ 0 };
		bool bOwnershipReleased{ false };
		bool operator==(const S_WINDOW_REGISTRATION&) const = default;
	};

	class C_WINDOW_REGISTRY
	{
	public:
		WINDOW_TOKEN Register(std::string _sWorkspaceId, std::optional<std::string> _sDocumentId);
		bool Activate(WINDOW_TOKEN _Token);
		bool ReleaseOwnership(WINDOW_TOKEN _Token);
		bool Retire(WINDOW_TOKEN _Token);

		bool Contains(WINDOW_TOKEN _Token) const;
		bool CanRegisterDocument(const std::string& _sDocumentId) const;
		std::optional<WINDOW_TOKEN> DocumentOwner(const std::string& _sDocumentId) const;
		std::optional<WINDOW_TOKEN> ActiveWindow() const noexcept { return(m_ActiveWindow); }
		std::vector<S_WINDOW_REGISTRATION> Snapshot() const;
		std::size_t Size() const noexcept { return(m_Windows.size()); }

	private:
		S_WINDOW_REGISTRATION* find_(WINDOW_TOKEN _Token);
		const S_WINDOW_REGISTRATION* find_(WINDOW_TOKEN _Token) const;
		void choose_active_();

		std::vector<S_WINDOW_REGISTRATION> m_Windows;
		std::optional<WINDOW_TOKEN> m_ActiveWindow{};
		WINDOW_TOKEN m_nNextToken{ 1 };
		std::uint64_t m_nNextOrdinal{ 1 };
	};

	struct S_RESTORABLE_DOCUMENT
	{
		std::string sDocumentId;
		std::int64_t nUpdatedAtUs{ 0 };
		std::int64_t nCreatedAtUs{ 0 };
		bool operator==(const S_RESTORABLE_DOCUMENT&) const = default;
	};

	std::optional<std::string> ChooseRecentUnownedDocument(
		const std::vector<S_RESTORABLE_DOCUMENT>& _Documents,
		const std::set<std::string>& _OwnedDocumentIds);

	enum class E_LEAVE_RESULT
	{
		Denied,
		ApprovedClean,
		ApprovedAfterSave,
	};

	struct S_WINDOW_LIFECYCLE_PARTICIPANT
	{
		WINDOW_TOKEN Token{ 0 };
		std::function<bool()> Protect;
		std::function<E_LEAVE_RESULT()> RequestLeave;
		std::function<bool()> Persist;
		std::function<bool()> Cleanup;
		std::function<void()> DeleteRestoration;
		std::function<void()> ReleaseOwnership;
		std::function<void()> Destroy;
	};

	struct S_WINDOW_CLEANUP_FAILURE
	{
		WINDOW_TOKEN Token{ 0 };
		bool bException{ false };
		bool operator==(const S_WINDOW_CLEANUP_FAILURE&) const = default;
	};

	class C_WINDOW_LIFECYCLE
	{
	public:
		bool CloseWindow(const S_WINDOW_LIFECYCLE_PARTICIPANT& _Participant, bool _bLastWindow);
		bool QuitApplication(const std::vector<S_WINDOW_LIFECYCLE_PARTICIPANT>& _Participants);

		bool AcceptsNewWindows() const noexcept { return(!m_bShuttingDown); }
		bool IsShuttingDown() const noexcept { return(m_bShuttingDown); }
		bool IsClosed(WINDOW_TOKEN _Token) const { return(m_Closed.contains(_Token)); }
		const std::vector<S_WINDOW_CLEANUP_FAILURE>& CleanupFailures() const noexcept { return(m_CleanupFailures); }

	private:
		bool protect_(const S_WINDOW_LIFECYCLE_PARTICIPANT& _Participant) const;
		bool approve_(const S_WINDOW_LIFECYCLE_PARTICIPANT& _Participant) const;
		void cleanup_(const S_WINDOW_LIFECYCLE_PARTICIPANT& _Participant);

		bool m_bShuttingDown{ false };
		bool m_bQuitInProgress{ false };
		std::set<WINDOW_TOKEN> m_Closing;
		std::set<WINDOW_TOKEN> m_Closed;
		std::vector<S_WINDOW_CLEANUP_FAILURE> m_CleanupFailures;
	};
}
