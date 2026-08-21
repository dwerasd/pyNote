#pragma once

// W3 settings/bus/status/maintenance 조각의 검증 seam.
//
// 시험 프로젝트는 CApplication.cpp·CMain.cpp 를 컴파일하지 않으며 넣을 수도 없다
// (g_pConfig/g_pLog 정의가 WinMain.cpp 뿐이고 wWinMain 이 Catch2 의 main 과 충돌한다).
// C_MAIN 도 완전 초기화된 CApplication 없이는 만들 수 없다. 따라서 이 조각의 검증 가능한
// 로직 - 변경 버스·정책 적재·카드 계수·상태/제목 조립·소멸 분류·열기 대상 판정·주기 클램프 -
// 를 전부 여기 두고 CApplication/CMain 은 배선만 갖는다. ApplyFocusMode 선례와 같은 구조로
// NoteEx.vcxproj 와 NoteExTests.vcxproj 가 이 TU 를 함께 컴파일한다.
//
// windows.h 를 포함하지 않는다. 조립기·분류기는 순수 로직이라 Win32 에 의존할 이유가 없고,
// core 헤더보다 windows.h 가 먼저 들어오면 CreateEvent 매크로가 repositories.h 의 멤버
// 이름을 바꾼다(w3_shell_consumer_test.cpp·CDocumentPage.cpp 의 #undef 계약과 같은 이유).

#include "pynote/core/application/window_lifecycle.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pynote::core::storage
{
	class C_DATABASE;
	class C_REPOSITORIES;
}

namespace pynote::shell
{
	using SUBSCRIPTION_TOKEN = std::uint64_t;

	// 원본 DocumentChangeBus(app.py:400~403)와 그 Qt Signal 전파 계약 이식이다.
	// CAP-PL-012 원문이 못박는 성질 두 가지를 계약으로 갖는다 - 발행은 동기이고, 구독자
	// 하나의 실패·예외가 뒤 구독자를 멈추지 않는다.
	class C_DOCUMENT_CHANGE_BUS final
	{
	public:
		using Subscriber = std::function<void(const std::string&)>;

		// 구독자 실패를 남길 자리. 이 TU 는 순수 seam 이라 DBGPRINT·g_pLog 에 닿지 못한다
		// (시험 실행 파일에는 DarkCore 도 전역 로거도 없다). 실제 로거는 CApplication 이 준다.
		using ErrorSink = std::function<void(const std::string&)>;

		SUBSCRIPTION_TOKEN Subscribe(Subscriber _Handler);
		bool Unsubscribe(SUBSCRIPTION_TOKEN _Token);
		void Publish(const std::string& _sDocumentId);
		void SetErrorSink(ErrorSink _Sink);
		std::size_t SubscriberCount() const noexcept;

	private:
		struct S_ENTRY
		{
			SUBSCRIPTION_TOKEN Token{ 0 };
			Subscriber Handler;
		};

		bool contains_(SUBSCRIPTION_TOKEN _Token) const;
		void report_(const std::string& _sDocumentId, const char* _pszReason) const;

		std::vector<S_ENTRY> m_Subscribers;
		ErrorSink m_ErrorSink;
		SUBSCRIPTION_TOKEN m_nNextToken{ 1 };
	};

	// 원본 DataPolicySettings(infrastructure/settings.py) 의 적재 대상 필드다. 타입은
	// v0002 스키마 그대로 고정한다 - backup_interval_hours 는 REAL 이고 0.5 가 합법이라
	// int 로 접으면 주기가 통째로 사라진다(인접 관용구 column_int64 를 복사하지 마라).
	struct S_DATA_POLICY
	{
		std::int64_t nDraftIdleMs{ 0 };
		std::string  sSplitPolicy;
		std::int64_t nPreviewLines{ 0 };
		double       dBackupIntervalHours{ 0.0 };
		std::int64_t nTrashRetentionDays{ 0 };
	};

	// data_policy_settings 단일 행을 읽는다. 행이 없거나 v0002 CHECK 제약을 어긴 값이면
	// nullopt 다 - 원본 AppContext.open 이 ValueError/RuntimeError 로 기동을 닫는 자리라
	// 소비측은 이것을 기동 실패로 다뤄야 한다.
	std::optional<S_DATA_POLICY> LoadDataPolicy(pynote::core::storage::C_DATABASE& _Database);

	struct S_DOCUMENT_CARD_STATS
	{
		std::int64_t nCards{ 0 };
		std::int64_t nCodepoints{ 0 };
	};

	// 원본 _update_status(main_window.py:711~732) 의 계수 두 개다. 문자 수는 UTF-8 바이트도
	// UTF-16 코드유닛도 아닌 코드포인트 수여야 한다(원본 len(card.body) 등가).
	// 저장소 조회가 실패하면 nullopt 다 - 0 으로 접으면 실패가 정상 상태로 위장된다.
	std::optional<S_DOCUMENT_CARD_STATS> CountActiveCards(
		pynote::core::storage::C_REPOSITORIES& _Repositories, const std::string& _sDocumentId);

	// 상태 바 문안 조립기. 원본 f-string(main_window.py:730~732) 축자 이식이다.
	std::wstring ComposeStatusText(
		std::int64_t _nCards, std::int64_t _nCodepoints, const std::wstring& _sSaveState);

	// 원본 :714 - 활성 문서가 없는 창의 상태 바 문안.
	std::wstring ComposeEmptyStatusText();

	// 원본 :723~729 - 세션이 있고 dirty 이거나 저장이 실패했을 때만 편집기 상태 문자열이고
	// 그 밖에는 "모든 변경 저장됨"이다. 세션이 없으면 조건 자체가 성립하지 않는다.
	// CEILING: 저장 실패 문안에 원본의 오류 상세({error})가 없다 - 이식된 저장 경로는 메시지가
	// 아니라 결과 열거를 돌려준다. 편집기 상태 5단계 이식(W5)에서 상세를 실어 올린다.
	std::wstring ComposeSaveStateText(bool _bHasSession, bool _bDirty, bool _bSaveFailed);

	// 원본 _install_page(main_window.py:515~520)·apply_document_change(:560) 의 제목 규칙.
	std::wstring ComposeWindowTitle(const std::optional<std::wstring>& _sDocumentTitle);

	// 원본 apply_document_change(main_window.py:547~557) 의 문서 소멸 2분기를
	// save_ui_state=document is not None 까지 펴서 3분기로 만든 것이다.
	enum class E_DOCUMENT_CHANGE
	{
		Alive,          // 문서가 살아 있다 - 제목·페이지·상태 바 갱신 경로.
		RemovedSaveUi,  // 행은 있으나 trashed/archived - UI 상태를 저장한 뒤 분리한다.
		RemovedNoSave,  // 행 자체가 없다 - 저장할 대상이 없으므로 그냥 분리한다.
	};

	// 저장소 조회가 실패하면 nullopt 다. 일시적 SQLite 오류를 RemovedNoSave 로 접으면
	// 살아 있는 문서의 UI 상태를 저장 없이 버리게 된다 - 분류가 아니라 관측 실패다.
	std::optional<E_DOCUMENT_CHANGE> ClassifyDocumentChange(
		pynote::core::storage::C_REPOSITORIES& _Repositories, const std::string& _sDocumentId);

	// 원본 WindowManager.open_document(app.py:573~584) 의 소유 창 판정이다.
	enum class E_OPEN_DOCUMENT_TARGET
	{
		ActivateOwner,
		OpenInRequesting,
	};

	E_OPEN_DOCUMENT_TARGET ResolveOpenDocumentTarget(
		const std::optional<pynote::core::application::WINDOW_TOKEN>& _Owner,
		pynote::core::application::WINDOW_TOKEN _Requesting);

	// 원본 AppContext.__init__(app.py:463~466) 의 interval_ms 계산이다.
	std::int64_t ClampMaintenanceIntervalMs(double _dIntervalHours);
}
