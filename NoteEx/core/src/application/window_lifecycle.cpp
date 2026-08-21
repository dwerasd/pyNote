#include "pynote/core/application/window_lifecycle.h"

#include <algorithm>
#include <utility>

namespace pynote::core::application
{
	S_WINDOW_REGISTRATION* C_WINDOW_REGISTRY::find_(WINDOW_TOKEN _Token)
	{
		const auto it = std::find_if(m_Windows.begin(), m_Windows.end(),
			[_Token](const auto& _Window) { return(_Window.Token == _Token); });
		return(it == m_Windows.end() ? nullptr : &*it);
	}

	const S_WINDOW_REGISTRATION* C_WINDOW_REGISTRY::find_(WINDOW_TOKEN _Token) const
	{
		const auto it = std::find_if(m_Windows.begin(), m_Windows.end(),
			[_Token](const auto& _Window) { return(_Window.Token == _Token); });
		return(it == m_Windows.end() ? nullptr : &*it);
	}

	WINDOW_TOKEN C_WINDOW_REGISTRY::Register(
		std::string _sWorkspaceId, std::optional<std::string> _sDocumentId)
	{
		if (_sWorkspaceId.empty()) { return(0); }
		if (_sDocumentId && !this->CanRegisterDocument(*_sDocumentId)) { return(0); }
		const WINDOW_TOKEN Token = m_nNextToken++;
		m_Windows.push_back({ Token, std::move(_sWorkspaceId), std::move(_sDocumentId), m_nNextOrdinal++, false });
		if (!m_ActiveWindow) { m_ActiveWindow = Token; }
		return(Token);
	}

	bool C_WINDOW_REGISTRY::Activate(WINDOW_TOKEN _Token)
	{
		const auto* pWindow = this->find_(_Token);
		if (!pWindow || pWindow->bOwnershipReleased) { return(false); }
		m_ActiveWindow = _Token;
		return(true);
	}

	bool C_WINDOW_REGISTRY::ReleaseOwnership(WINDOW_TOKEN _Token)
	{
		auto* pWindow = this->find_(_Token);
		if (!pWindow || pWindow->bOwnershipReleased) { return(false); }
		pWindow->bOwnershipReleased = true;
		pWindow->sDocumentId.reset();
		if (m_ActiveWindow == _Token) { this->choose_active_(); }
		return(true);
	}

	bool C_WINDOW_REGISTRY::Retire(WINDOW_TOKEN _Token)
	{
		const auto it = std::find_if(m_Windows.begin(), m_Windows.end(),
			[_Token](const auto& _Window) { return(_Window.Token == _Token); });
		if (it == m_Windows.end() || !it->bOwnershipReleased) { return(false); }
		m_Windows.erase(it);
		if (m_ActiveWindow == _Token) { this->choose_active_(); }
		return(true);
	}

	bool C_WINDOW_REGISTRY::Contains(WINDOW_TOKEN _Token) const
	{
		return(this->find_(_Token) != nullptr);
	}

	bool C_WINDOW_REGISTRY::CanRegisterDocument(const std::string& _sDocumentId) const
	{
		return(!this->DocumentOwner(_sDocumentId).has_value());
	}

	std::optional<WINDOW_TOKEN> C_WINDOW_REGISTRY::DocumentOwner(const std::string& _sDocumentId) const
	{
		for (const auto& Window : m_Windows)
		{
			if (!Window.bOwnershipReleased && Window.sDocumentId == _sDocumentId) { return(Window.Token); }
		}
		return(std::nullopt);
	}

	std::vector<S_WINDOW_REGISTRATION> C_WINDOW_REGISTRY::Snapshot() const
	{
		std::vector<S_WINDOW_REGISTRATION> Result;
		for (const auto& Window : m_Windows)
		{
			if (!Window.bOwnershipReleased) { Result.push_back(Window); }
		}
		return(Result);
	}

	void C_WINDOW_REGISTRY::choose_active_()
	{
		m_ActiveWindow.reset();
		const S_WINDOW_REGISTRATION* pBest = nullptr;
		for (const auto& Window : m_Windows)
		{
			if (Window.bOwnershipReleased) { continue; }
			if (!pBest || Window.nOrdinal < pBest->nOrdinal) { pBest = &Window; }
		}
		if (pBest) { m_ActiveWindow = pBest->Token; }
	}

	std::optional<std::string> ChooseRecentUnownedDocument(
		const std::vector<S_RESTORABLE_DOCUMENT>& _Documents,
		const std::set<std::string>& _OwnedDocumentIds)
	{
		const S_RESTORABLE_DOCUMENT* pBest = nullptr;
		for (const auto& Document : _Documents)
		{
			if (_OwnedDocumentIds.contains(Document.sDocumentId)) { continue; }
			if (!pBest || Document.nUpdatedAtUs > pBest->nUpdatedAtUs ||
				(Document.nUpdatedAtUs == pBest->nUpdatedAtUs && Document.nCreatedAtUs > pBest->nCreatedAtUs) ||
				(Document.nUpdatedAtUs == pBest->nUpdatedAtUs && Document.nCreatedAtUs == pBest->nCreatedAtUs &&
					Document.sDocumentId < pBest->sDocumentId))
			{
				pBest = &Document;
			}
		}
		return(pBest ? std::optional<std::string>(pBest->sDocumentId) : std::nullopt);
	}

	bool C_WINDOW_LIFECYCLE::protect_(const S_WINDOW_LIFECYCLE_PARTICIPANT& _Participant) const
	{
		if (!_Participant.Protect) { return(false); }
		try { return(_Participant.Protect()); }
		catch (...) { return(false); }
	}

	bool C_WINDOW_LIFECYCLE::approve_(const S_WINDOW_LIFECYCLE_PARTICIPANT& _Participant) const
	{
		if (!_Participant.RequestLeave) { return(false); }
		try { return(_Participant.RequestLeave() != E_LEAVE_RESULT::Denied); }
		catch (...) { return(false); }
	}

	void C_WINDOW_LIFECYCLE::cleanup_(const S_WINDOW_LIFECYCLE_PARTICIPANT& _Participant)
	{
		try
		{
			if (!_Participant.Cleanup || !_Participant.Cleanup())
			{
				m_CleanupFailures.push_back({ _Participant.Token, false });
			}
		}
		catch (...)
		{
			m_CleanupFailures.push_back({ _Participant.Token, true });
		}
	}

	bool C_WINDOW_LIFECYCLE::CloseWindow(
		const S_WINDOW_LIFECYCLE_PARTICIPANT& _Participant, bool _bLastWindow)
	{
		if (_Participant.Token == 0 || m_bQuitInProgress || m_Closing.contains(_Participant.Token) ||
			m_Closed.contains(_Participant.Token) || (m_bShuttingDown && !_bLastWindow))
		{
			return(false);
		}
		m_Closing.insert(_Participant.Token);
		if (!this->protect_(_Participant) || !this->approve_(_Participant))
		{
			m_Closing.erase(_Participant.Token);
			return(false);
		}
		if (_bLastWindow) { m_bShuttingDown = true; }
		bool bPersisted = false;
		try { bPersisted = _Participant.Persist && _Participant.Persist(); }
		catch (...) { bPersisted = false; }
		if (!bPersisted)
		{
			if (_bLastWindow) { m_bShuttingDown = false; }
			m_Closing.erase(_Participant.Token);
			return(false);
		}

		this->cleanup_(_Participant);
		if (!_bLastWindow && _Participant.DeleteRestoration) { _Participant.DeleteRestoration(); }
		if (_Participant.ReleaseOwnership) { _Participant.ReleaseOwnership(); }
		m_Closed.insert(_Participant.Token);
		if (_Participant.Destroy) { _Participant.Destroy(); }
		return(true);
	}

	bool C_WINDOW_LIFECYCLE::QuitApplication(
		const std::vector<S_WINDOW_LIFECYCLE_PARTICIPANT>& _Participants)
	{
		if (m_bShuttingDown || m_bQuitInProgress) { return(false); }
		m_bShuttingDown = true;
		m_bQuitInProgress = true;

		for (const auto& Participant : _Participants)
		{
			if (Participant.Token == 0 || m_Closing.contains(Participant.Token) ||
				m_Closed.contains(Participant.Token) || !this->protect_(Participant))
			{
				m_bQuitInProgress = false;
				m_bShuttingDown = false;
				return(false);
			}
		}
		for (const auto& Participant : _Participants)
		{
			if (!this->approve_(Participant))
			{
				m_bQuitInProgress = false;
				m_bShuttingDown = false;
				return(false);
			}
		}

		for (const auto& Participant : _Participants)
		{
			bool bPersisted = false;
			try { bPersisted = Participant.Persist && Participant.Persist(); }
			catch (...) { bPersisted = false; }
			if (!bPersisted)
			{
				m_bQuitInProgress = false;
				m_bShuttingDown = false;
				return(false);
			}
		}

		for (const auto& Participant : _Participants) { this->cleanup_(Participant); }
		for (const auto& Participant : _Participants)
		{
			if (Participant.ReleaseOwnership) { Participant.ReleaseOwnership(); }
			m_Closed.insert(Participant.Token);
			if (Participant.Destroy) { Participant.Destroy(); }
		}
		m_bQuitInProgress = false;
		return(true);
	}
}
