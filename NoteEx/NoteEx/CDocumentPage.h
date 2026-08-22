#pragma once

#include <windows.h>

#include "pynote/core/application/window_lifecycle.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pynote::core::application
{
	class C_CARD_SERVICE;
	class C_DRAFT_COORDINATOR;
	class C_FIRST_INPUT_CAPTURE;
	class C_SAVE_COORDINATOR;
	struct S_DOCUMENT_UI_STATE;
}
namespace pynote::core::domain { class C_CARD_LIST_PROJECTION; }
namespace pynote::core::storage { class C_DATABASE; class C_REPOSITORIES; }
// 카드 목록 컨트롤(CCardList.h). 헤더에서 전역 선언만 하면 ATL/WTL·D2DWrapp 헤더가
// 이 파일을 읽는 모든 TU 로 번지지 않는다(W3 시험 TU 의 CreateEvent 순서 계약 보호).
class C_CARD_LIST;
struct S_CARD_LIST_DISPLAY;
namespace d2d { class C_D2D_DEVICE; class C_D2D_BRUSH_CACHE; class C_D2D_TEXT; }

class C_DOCUMENT_PAGE final
{
public:
	enum class E_LEAVE_CHOICE { Save, Discard, Cancel };
	using LeavePrompt = std::function<E_LEAVE_CHOICE(HWND)>;
	// 카드 생성·저장 완료를 창에 알리는 자리다. 비소유 TU 가 Init 을 부르므로
	// 시그니처를 늘리지 않고 별도 setter 로만 받는다(계약 plan R1 F17).
	using ChangeNotifier = std::function<void()>;

	C_DOCUMENT_PAGE();
	~C_DOCUMENT_PAGE();
	C_DOCUMENT_PAGE(const C_DOCUMENT_PAGE&) = delete;
	C_DOCUMENT_PAGE& operator=(const C_DOCUMENT_PAGE&) = delete;

	bool Init(
		HINSTANCE _hInstance, HWND _hListHost, HWND _hEditorHost,
		pynote::core::storage::C_DATABASE& _Database,
		pynote::core::storage::C_REPOSITORIES& _Repositories,
		pynote::core::application::C_CARD_SERVICE& _CardService,
		pynote::core::application::C_DRAFT_COORDINATOR& _DraftCoordinator,
		pynote::core::application::C_SAVE_COORDINATOR& _SaveCoordinator,
		std::string _sWorkspaceId, std::string _sDocumentId,
		LeavePrompt _LeavePrompt = {});
	void SetChangeNotifier(ChangeNotifier _Notifier);
	// 아래 둘은 Init 앞에 불러야 첫 프레임부터 그려진다. 서비스가 없으면 목록은
	// 그리지만 않고 행·메시지·Enter·스크롤은 그대로 동작한다.
	void SetRenderServices(d2d::C_D2D_DEVICE* _pDevice,
		d2d::C_D2D_BRUSH_CACHE* _pBrushCache, d2d::C_D2D_TEXT* _pText);
	void SetDisplaySettings(const S_CARD_LIST_DISPLAY& _Display);
	bool PreTranslateMessage(MSG* _pMessage);
	bool Protect();
	// 이탈 승인만(원본 card_editor.can_leave_editor) - 깨끗한 세션은 그대로 둔다. 창 닫기·앱 종료의
	// 영속이 편집기 카드를 기록하려면 승인 뒤에도 세션이 살아 있어야 한다(main_window.py:421~431).
	pynote::core::application::E_LEAVE_RESULT CanLeave();
	// 승인 + 세션 해제 + 편집면 비움 + 목록 포커스(원본 request_close - Back/Esc 경로).
	pynote::core::application::E_LEAVE_RESULT RequestLeave();
	bool PersistState(const std::optional<std::pair<int, int>>& _SplitSizesDip);
	bool Cleanup();
	void Layout();

	// 보기 메뉴 토글의 소비자다. 축소 규칙(살아남는 한 장)은 core 프로젝션이 소유한다.
	void SetMultiSelectionEnabled(bool _bEnabled);
	bool MultiSelectionEnabled() const;
	// 원본 _close_editor_on_empty_click(document_page.py:263~275). 마우스 없이도 몰 수 있게 공개다.
	bool OnEmptyAreaClicked();
	// 원본 _delete_cards(document_page.py:795~813) - 파괴적 명령 사전 점검을 거쳐 순서대로 지운다.
	bool DeleteCards(const std::vector<std::string>& _CardIds);

	bool OpenSelectedCard();
	bool Save();
	// 원본 DocumentPage.refresh() 자리 - 저장소에서 카드 목록을 다시 읽는다.
	bool Refresh();
	void FocusCardList();
	void ShowHistory();
	void ShowFind(bool _bReplace);

	HWND CardListHwnd() const noexcept;
	// 컨트롤 자체 API(스크롤 오프셋·프레임 관측)에 닿는 자리다.
	C_CARD_LIST& CardList() const noexcept;
	HWND EditorHwnd() const noexcept;
	HWND FindHwnd() const noexcept;
	HWND ReplaceHwnd() const noexcept;
	HWND HistoryHwnd() const noexcept;
	bool IsHistoryVisible() const noexcept;
	bool HasSession() const noexcept;
	bool HasDirtySession() const;
	// 원본 CardEditor._card_save_failed(:139) 의 latch 다 - 저장 성공까지 남는다.
	bool HasSaveFailed() const noexcept;
	const std::string& DocumentId() const noexcept { return m_sDocumentId; }

private:
	struct S_STATE;
	std::unique_ptr<S_STATE> m_pState;
	std::string m_sDocumentId;
};
