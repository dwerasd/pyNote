#pragma once

#include <windows.h>
// 드래그 실행기 전달자의 인자 타입(IDataObject·IDropSource)만 필요하다. ATL/WTL·D2DWrapp 을
// 끌어오지 않으므로 W3 시험 TU 의 CreateEvent 순서 계약에 영향이 없다.
#include <ole2.h>

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
namespace pynote::core::domain
{
	class C_CARD_LIST_PROJECTION;
	// S5 정렬/출처 필터 진입점의 인자 타입이다 - 같은 패턴(class C_CARD_LIST_PROJECTION;)의
	// 불투명 전방 선언이며 core 도메인 헤더는 여기서 읽지 않는다(W3 CreateEvent 순서 계약).
	enum class E_CARD_LIST_SORT_MODE;
	enum class E_CARD_SOURCE;
}
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
	// 원본 _ask_drag_delete_choice(document_page.py:1038~1063). LeavePrompt 와 같은 모양의
	// 주입 자리이며 기본값은 진짜 대화상자다(W4 S4 spec §3.3.6).
	enum class E_DRAG_DELETE_CHOICE { Save, Discard, Cancel };
	using DragDeletePrompt = std::function<E_DRAG_DELETE_CHOICE(HWND)>;
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
	void SetDragDeletePrompt(DragDeletePrompt _Prompt);
	// 진짜 TrackPopupMenu 실행기는 셸이 건다 - 이 자리는 컨트롤로 그대로 넘겨 준다.
	// 페이지 픽스처는 이 함수를 부르지 않으므로 컨트롤 기본값(빈 실행기)이 남는다.
	void SetContextMenuExecutor(std::function<UINT(HMENU, POINT)> _Executor);
	// 진짜 ::DoDragDrop 러너도 같은 이유로 셸이 건다(fix1) - 컨트롤 기본값이 비어 있어야
	// 시험 프로세스가 실제 모달 드래그 루프에 들어가지 않는다.
	void SetDragRunner(std::function<HRESULT(IDataObject*, IDropSource*, DWORD, DWORD*)> _Runner);
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
	// 원본 _move_card(document_page.py:919~932) - 낙관적 동시성은 서비스 안에 있고 드래그
	// 시점 리비전을 넘기지 않는다(삭제 경로와 다른 자리다).
	bool MoveCard(const std::string& _sCardId, const std::optional<std::string>& _sBeforeCardId);
	// 원본 _delete_dragged_card(document_page.py:978~1036) - press 시점 리비전으로 CAS 하고
	// 연결된 카드는 초안을 discard 로 넘긴다. DeleteCards 와는 다른 경로다.
	bool DeleteDraggedCard(const std::string& _sCardId);
	// 원본 reveal_card(document_page.py:286~296) - 행을 선택·표시만 하고 편집면에는 연결하지
	// 않는다. 필터에 가려 행이 없으면 false 를 돌리고 선택도 대기 중 휠 탐색도 건드리지 않는다.
	bool RevealCard(const std::string& _sCardId);

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
	// S5 정렬/출처 필터/휴지통 스트립과 카드 툴팁 창(spec §3.1.1/§3.2.8, S2/S3 의
	// CardListHwnd() 선례).
	HWND SortComboHwnd() const noexcept;
	HWND SourceFilterHwnd() const noexcept;
	HWND TrashButtonHwnd() const noexcept;
	HWND TooltipHwnd() const noexcept;
	// 원본 card_model.py:338~367 의 두 진입점(sort_combo/source_filter 의
	// currentIndexChanged 가 부르는 자리) - 페이지 자체가 동일값 조기 반환을 먼저 본다
	// (spec §3.1.6~7). core 는 필터의 "현재 선택값" 을 소유하지 않으므로 SourceFilter() 는
	// 페이지 상태의 순수 getter 다.
	void SetSortMode(pynote::core::domain::E_CARD_LIST_SORT_MODE _eMode);
	void SetSourceFilter(std::optional<pynote::core::domain::E_CARD_SOURCE> _eSource);
	std::optional<pynote::core::domain::E_CARD_SOURCE> SourceFilter() const noexcept;
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
