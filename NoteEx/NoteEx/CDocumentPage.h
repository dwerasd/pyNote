#pragma once

#include <windows.h>

#include "pynote/core/application/window_lifecycle.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

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
	bool PreTranslateMessage(MSG* _pMessage);
	bool Protect();
	pynote::core::application::E_LEAVE_RESULT RequestLeave();
	bool PersistState(const std::optional<std::pair<int, int>>& _SplitSizesDip);
	bool Cleanup();
	void Layout();

	bool OpenSelectedCard();
	bool Save();
	// 원본 DocumentPage.refresh() 자리 - 저장소에서 카드 목록을 다시 읽는다.
	bool Refresh();
	void FocusCardList();
	void ShowHistory();
	void ShowFind(bool _bReplace);

	HWND CardListHwnd() const noexcept;
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
