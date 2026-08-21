#include "CChangeBus.h"

// core 헤더보다 windows.h 가 먼저 들어온 TU 에서 이 파일이 포함될 일은 없지만, 다른
// 셸 TU(CDocumentPage.cpp·w3_shell_consumer_test.cpp)와 같은 순서 계약을 지켜 둔다 -
// 같은 바이너리 안에서 repositories.h 의 멤버 이름이 갈리면 링크가 조용히 깨진다.
#ifdef CreateEvent
#undef CreateEvent
#endif

#include "pynote/core/domain/models.h"
#include "pynote/core/storage/database.h"
#include "pynote/core/storage/repositories.h"

#include <sqlite3/sqlite3.h>

// 시험 실행 파일에는 프로젝트 수준 sqlite3.lib 의존이 없다(셸 프로젝트만 갖는다).
// 이 TU 가 sqlite3 를 직접 부르므로 링크 지시를 여기서 건다 - core 의 관용구와 같다.
#pragma comment(lib, "sqlite3")

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace pynote::shell
{
	namespace
	{
		namespace domain = pynote::core::domain;
		namespace storage = pynote::core::storage;

		// UTF-8 선행 바이트 수 = 코드포인트 수다. 이어지는 바이트(10xxxxxx)만 건너뛴다.
		std::int64_t count_codepoints(const std::string& _sUtf8)
		{
			std::int64_t nCount = 0;
			for (const char cByte : _sUtf8)
			{
				if ((static_cast<unsigned char>(cByte) & 0xC0u) != 0x80u) { ++nCount; }
			}
			return(nCount);
		}

		// v0002 스키마의 CHECK 제약 등가다. 스키마가 걸러 주는 값이지만 오래된·손상된 DB 가
		// 같은 표를 다른 제약으로 들고 있을 수 있으므로 적재기가 다시 본다.
		bool policy_is_valid(const S_DATA_POLICY& _Policy)
		{
			return(_Policy.nDraftIdleMs >= 0 &&
				(_Policy.sSplitPolicy == "keep" || _Policy.sSplitPolicy == "split_by_blank_line") &&
				_Policy.nPreviewLines >= 1 &&
				_Policy.dBackupIntervalHours > 0.0 &&
				_Policy.nTrashRetentionDays >= 0);
		}
	}

	SUBSCRIPTION_TOKEN C_DOCUMENT_CHANGE_BUS::Subscribe(Subscriber _Handler)
	{
		if (!_Handler) { return(0); }
		const SUBSCRIPTION_TOKEN Token = m_nNextToken++;
		m_Subscribers.push_back(S_ENTRY{ Token, std::move(_Handler) });
		return(Token);
	}

	bool C_DOCUMENT_CHANGE_BUS::Unsubscribe(SUBSCRIPTION_TOKEN _Token)
	{
		const auto it = std::find_if(m_Subscribers.begin(), m_Subscribers.end(),
			[_Token](const S_ENTRY& _Entry) { return(_Entry.Token == _Token); });
		if (it == m_Subscribers.end()) { return(false); }
		m_Subscribers.erase(it);
		return(true);
	}

	void C_DOCUMENT_CHANGE_BUS::Publish(const std::string& _sDocumentId)
	{
		// 발행 중 구독·해제가 일어나도 벡터 재할당에 걸리지 않도록 사본 위를 돈다. 다만
		// 사본에 남은 구독자라도 그 사이 해제됐으면 부르지 않는다 - 원본 Qt disconnect 가
		// 진행 중 emit 에도 즉시 효력을 갖고, PLAN-W3-0042 의 "파괴 전 해제"가 그 성질에 기댄다.
		const std::vector<S_ENTRY> Snapshot = m_Subscribers;
		for (const auto& Entry : Snapshot)
		{
			if (!Entry.Handler || !this->contains_(Entry.Token)) { continue; }
			try
			{
				Entry.Handler(_sDocumentId);
			}
			catch (const std::exception& Error)
			{
				// CAP-PL-012: 한 구독자의 실패는 로그만 남기고 뒤 구독자를 멈추지 않는다.
				this->report_(_sDocumentId, Error.what());
			}
			catch (...)
			{
				this->report_(_sDocumentId, "unknown exception");
			}
		}
	}

	void C_DOCUMENT_CHANGE_BUS::SetErrorSink(ErrorSink _Sink)
	{
		m_ErrorSink = std::move(_Sink);
	}

	std::size_t C_DOCUMENT_CHANGE_BUS::SubscriberCount() const noexcept
	{
		return(m_Subscribers.size());
	}

	bool C_DOCUMENT_CHANGE_BUS::contains_(SUBSCRIPTION_TOKEN _Token) const
	{
		return(std::find_if(m_Subscribers.begin(), m_Subscribers.end(),
			[_Token](const S_ENTRY& _Entry) { return(_Entry.Token == _Token); }) != m_Subscribers.end());
	}

	void C_DOCUMENT_CHANGE_BUS::report_(const std::string& _sDocumentId, const char* _pszReason) const
	{
		if (!m_ErrorSink) { return; }
		m_ErrorSink("document change subscriber failed: document=" + _sDocumentId +
			" reason=" + (_pszReason ? _pszReason : "(null)"));
	}

	std::optional<S_DATA_POLICY> LoadDataPolicy(storage::C_DATABASE& _Database)
	{
		if (!_Database.IsOpen()) { return(std::nullopt); }
		constexpr char SQL[] = R"SQL(
			SELECT draft_idle_ms, split_policy, preview_lines,
			       backup_interval_hours, trash_retention_days
			FROM data_policy_settings WHERE id = 1
			)SQL";
		sqlite3_stmt* pStatement = nullptr;
		if (::sqlite3_prepare_v2(_Database.Handle(), SQL, -1, &pStatement, nullptr) != SQLITE_OK)
		{
			return(std::nullopt);
		}

		std::optional<S_DATA_POLICY> Result;
		if (::sqlite3_step(pStatement) == SQLITE_ROW)
		{
			const unsigned char* pszSplitPolicy = ::sqlite3_column_text(pStatement, 1);
			S_DATA_POLICY Policy;
			Policy.nDraftIdleMs = ::sqlite3_column_int64(pStatement, 0);
			Policy.sSplitPolicy = pszSplitPolicy ? reinterpret_cast<const char*>(pszSplitPolicy) : "";
			Policy.nPreviewLines = ::sqlite3_column_int64(pStatement, 2);
			// REAL 열이다. column_int64 로 읽으면 0.5 시간이 0 이 되어 주기가 사라진다.
			Policy.dBackupIntervalHours = ::sqlite3_column_double(pStatement, 3);
			Policy.nTrashRetentionDays = ::sqlite3_column_int64(pStatement, 4);
			if (policy_is_valid(Policy)) { Result = std::move(Policy); }
		}
		::sqlite3_finalize(pStatement);
		return(Result);
	}

	std::optional<S_DOCUMENT_CARD_STATS> CountActiveCards(
		storage::C_REPOSITORIES& _Repositories, const std::string& _sDocumentId)
	{
		std::vector<domain::S_CARD> Cards;
		if (_Repositories.ListCards(_sDocumentId, &Cards) != storage::E_REPO_RESULT::Ok)
		{
			return(std::nullopt);
		}
		S_DOCUMENT_CARD_STATS Stats;
		for (const auto& Card : Cards)
		{
			// 원본 :716~721 - 소프트 삭제된 카드는 계수에서 빠진다.
			if (Card.nDeletedAtUs) { continue; }
			++Stats.nCards;
			Stats.nCodepoints += count_codepoints(Card.sBody);
		}
		return(Stats);
	}

	std::wstring ComposeStatusText(
		std::int64_t _nCards, std::int64_t _nCodepoints, const std::wstring& _sSaveState)
	{
		return(std::to_wstring(_nCards) + L"개 카드 · " + std::to_wstring(_nCodepoints) + L"자 · " +
			_sSaveState + L" · 로컬 DB");
	}

	std::wstring ComposeEmptyStatusText()
	{
		return(std::wstring(L"문서를 선택하거나 새 문서를 만드세요."));
	}

	std::wstring ComposeSaveStateText(bool _bHasSession, bool _bDirty, bool _bSaveFailed)
	{
		if (!_bHasSession) { return(std::wstring(L"모든 변경 저장됨")); }
		// 원본은 마지막으로 설정된 편집기 상태 문안을 쓴다 - 저장 실패가 dirty 보다 나중에
		// 찍히므로 둘 다 참이면 실패 문안이 이긴다(card_editor.py:280~284).
		if (_bSaveFailed) { return(std::wstring(L"저장 실패 — 다시 시도")); }
		if (_bDirty) { return(std::wstring(L"편집 중")); }
		return(std::wstring(L"모든 변경 저장됨"));
	}

	std::wstring ComposeWindowTitle(const std::optional<std::wstring>& _sDocumentTitle)
	{
		if (!_sDocumentTitle) { return(std::wstring(L"pyNote")); }
		return(*_sDocumentTitle + L" — pyNote");
	}

	std::optional<E_DOCUMENT_CHANGE> ClassifyDocumentChange(
		storage::C_REPOSITORIES& _Repositories, const std::string& _sDocumentId)
	{
		domain::S_DOCUMENT Document;
		const storage::E_REPO_RESULT eResult = _Repositories.GetDocument(_sDocumentId, &Document);
		if (eResult == storage::E_REPO_RESULT::NotFound) { return(E_DOCUMENT_CHANGE::RemovedNoSave); }
		if (eResult != storage::E_REPO_RESULT::Ok) { return(std::nullopt); }
		if (Document.nTrashedAtUs || Document.nArchivedAtUs) { return(E_DOCUMENT_CHANGE::RemovedSaveUi); }
		return(E_DOCUMENT_CHANGE::Alive);
	}

	E_OPEN_DOCUMENT_TARGET ResolveOpenDocumentTarget(
		const std::optional<pynote::core::application::WINDOW_TOKEN>& _Owner,
		pynote::core::application::WINDOW_TOKEN _Requesting)
	{
		// 원본 :575~581 - 소유 창이 있고 그 창이 요청 창이 아닐 때만 활성화다.
		if (_Owner && *_Owner != _Requesting) { return(E_OPEN_DOCUMENT_TARGET::ActivateOwner); }
		return(E_OPEN_DOCUMENT_TARGET::OpenInRequesting);
	}

	std::int64_t ClampMaintenanceIntervalMs(double _dIntervalHours)
	{
		constexpr std::int64_t MAXIMUM_INTERVAL_MS = 2147483647;
		const double dMilliseconds = _dIntervalHours * 60.0 * 60.0 * 1000.0;
		// 원본은 min(round(h*3600*1000), 2147483647) 이고 주기는 스키마 CHECK 로 양수다.
		// NaN·비양수·표현 범위 초과는 원본에 없는 입력이지만 llround 의 미정의 동작을 막기
		// 위해 먼저 접는다 - 적재기가 CHECK 위반을 nullopt 로 거르므로 정상 경로는 그대로다.
		if (!(dMilliseconds > 0.0)) { return(0); }
		if (dMilliseconds >= static_cast<double>(MAXIMUM_INTERVAL_MS)) { return(MAXIMUM_INTERVAL_MS); }
		return(static_cast<std::int64_t>(std::llround(dMilliseconds)));
	}
}
