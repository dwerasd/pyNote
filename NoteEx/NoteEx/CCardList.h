#pragma once

// 카드 목록 자작 가상 컨트롤(W4 S1 [W4-render]). 원본 대조 정본은
// src/pynote/ui/cards/card_delegate.py(그리기·측정·말줄임)와 card_stream.py(픽셀 스크롤)다.
// 미리보기 예산·정렬·필터·선택 축소는 core C_CARD_LIST_PROJECTION 이 소유하므로 여기서
// 재구현하지 않는다.

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>

// 드래그 앤 드롭 seam 의 인자 타입이다(S4). atlbase.h 가 ole2.h 를 이미 읽지만 이 헤더가
// 스스로 요구하는 선언이므로 명시한다.
#include <ole2.h>

#include <D2DWrapp/D2DDef.h>
#include <D2DWrapp/D2DSwapTarget.h>
#include <D2DWrapp/D2DText.h>

#include "pynote/core/domain/time_zone_resolver.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace d2d
{
	class C_D2D_DEVICE;
	class C_D2D_BRUSH_CACHE;
}
// core 헤더를 여기서 읽지 않는다 - 이 헤더는 ATL/WTL 과 함께 #undef CreateEvent 앞에서
// 읽히므로 core 타입이 TU 마다 다른 매크로 상태로 파싱되면 안 된다(S1 include 순서 계약).
// 범위 있는 enum 의 불투명 선언은 기반 타입이 int 로 확정돼 멤버로 쓸 수 있다.
namespace pynote::core::domain
{
	class C_CARD_LIST_PROJECTION;
	enum class E_CARD_SELECTION_MODE;
	// 휠 탐색 상태 기계도 같은 이유로 CCardList.cpp 에서만 읽는다(W2-R6 완성분을 소비만 한다).
	class C_CARD_WHEEL_BROWSE;
	struct S_CARD_WHEEL_TIMER_COMMAND;
	// 드래그 세션 등록소(W2-R5)도 같은 이유로 .cpp 에서만 읽는다. 아래 두 별칭은 core 헤더의
	// 정의와 글자 그대로 같은 재선언이라(별칭 중복 선언은 적법하다) 타입이 갈리지 않는다.
	class C_CARD_DRAG_SESSION_REGISTRY;
	using CardDragSourceIdentity = std::uintptr_t;
	using CardDragSessionToken = std::uint64_t;
}

// 원본 CardDelegate 의 클래스 상수를 그대로 옮긴 것이다(card_delegate.py:23~27, :70).
inline constexpr int CARD_HORIZONTAL_INSET_DIP = 4;
inline constexpr int CARD_VERTICAL_INSET_DIP = 3;
inline constexpr int CARD_CONTENT_HORIZONTAL_MARGIN_DIP = 14;
inline constexpr int CARD_CONTENT_VERTICAL_MARGIN_DIP = 10;
inline constexpr int CARD_AUXILIARY_ROW_PADDING_DIP = 4;
inline constexpr float CARD_CORNER_RADIUS_DIP = 6.0f;

// 하단 삭제 오버레이의 원본 상수다(_position_delete_drop_zone document_page.py:953~967,
// CardDeleteDropZone card_stream.py:598~614). 좌우 예약 96 은 원본의 panel_width - 96 이다.
inline constexpr int CARD_DELETE_ZONE_MAX_WIDTH_DIP = 280;
inline constexpr int CARD_DELETE_ZONE_HEIGHT_DIP = 56;
inline constexpr int CARD_DELETE_ZONE_BOTTOM_GAP_DIP = 16;
inline constexpr int CARD_DELETE_ZONE_SIDE_RESERVE_DIP = 96;
inline constexpr int CARD_DELETE_ZONE_LABEL_MARGIN_DIP = 8;
inline constexpr float CARD_DELETE_ZONE_CORNER_RADIUS_DIP = 8.0f;

// QRect 와 같은 닫힌 정수 구간이다(Right = Left + Width - 1). Win32 RECT 의 열린 구간과
// 섞으면 1 DIP 오차가 조용히 생기므로 별도 타입으로 못박는다 - 파이썬 단언이 QRect
// 의미의 right/bottom/contains 를 직접 본다(test_card_stream.py:157·:191).
struct S_DIP_RECT
{
	int nLeft{ 0 };
	int nTop{ 0 };
	int nWidth{ 0 };
	int nHeight{ 0 };

	int Right() const noexcept { return(nLeft + nWidth - 1); }
	int Bottom() const noexcept { return(nTop + nHeight - 1); }

	S_DIP_RECT Adjusted(int _nLeft, int _nTop, int _nRight, int _nBottom) const noexcept
	{
		return(S_DIP_RECT{ nLeft + _nLeft, nTop + _nTop,
			nWidth - _nLeft + _nRight, nHeight - _nTop + _nBottom });
	}

	bool Contains(const S_DIP_RECT& _Other) const noexcept
	{
		return(_Other.nLeft >= nLeft && _Other.nTop >= nTop &&
			_Other.Right() <= this->Right() && _Other.Bottom() <= this->Bottom());
	}

	bool operator==(const S_DIP_RECT&) const = default;
};

// ResolveCardPalette 의 입력. Win32 호출은 컨트롤이 하고 해석기는 순수 함수로 남는다
// (UNC-006 - 3모드 fixture 가 이 구조체만 채워 넣는다).
struct S_SYSTEM_COLORS
{
	COLORREF nWindow{ 0 };
	COLORREF nWindowText{ 0 };
	COLORREF nHighlight{ 0 };
	COLORREF nHighlightText{ 0 };
	COLORREF nBtnShadow{ 0 };
	COLORREF nGrayText{ 0 };

	bool operator==(const S_SYSTEM_COLORS&) const = default;
};

// D2DWrapp 브러시는 ARGB(0xAARRGGBB)를 받으므로 COLORREF(0x00BBGGRR)에서 여기서 뒤집는다.
struct S_CARD_PALETTE
{
	d2d::Color nBase{ 0 };
	d2d::Color nHoverBase{ 0 };
	d2d::Color nText{ 0 };
	d2d::Color nHighlight{ 0 };
	d2d::Color nHighlightText{ 0 };
	d2d::Color nBorder{ 0 };
	d2d::Color nPlaceholder{ 0 };

	bool operator==(const S_CARD_PALETTE&) const = default;
};

// Qt 역할 -> Win32 시스템 색 매핑. 고대비 여부는 매핑을 바꾸지 않는다(Qt 동등).
S_CARD_PALETTE ResolveCardPalette(const S_SYSTEM_COLORS& _Colors, bool _bHighContrast) noexcept;

// 원본 CardDelegate._preview_lines(card_delegate.py:169~222)의 네이티브 쌍둥이다.
// _bTruncated 는 core 의 판정값이며 파이썬처럼 인자로 받는다(줄 수가 남아도 모델이
// 뒷부분을 잘랐으면 말줄임표가 선다).
std::vector<std::wstring> ComputeDisplayLines(
	std::wstring_view _sText, bool _bTruncated, d2d::C_D2D_TEXT& _Text,
	IDWriteTextFormat* _pFormat, int _nContentWidthDip, std::size_t _nMaxLines);

// 빈 sFamily / 0 이하 fSizeDip 은 "시스템 메시지 폰트로 해석하라"는 뜻이다.
struct S_CARD_LIST_FONT
{
	std::wstring sFamily;
	float fSizeDip{ 0.0f };
	DWRITE_FONT_WEIGHT eWeight{ DWRITE_FONT_WEIGHT_NORMAL };
};

struct S_CARD_LIST_DISPLAY
{
	std::wstring sTimeFormat{ L"yyyy-MM-dd HH:mm" };
	std::wstring sTimeZone{ L"system" };
	S_CARD_LIST_FONT Font{};
};

// 프레임 관측 seam(P2 last_frame() 선례). 그리기에 쓴 변수를 그대로 기록하며 그리기
// 자체를 바꾸지 않는다 - 파이썬 시험의 _RecordingPainter.drawn_text 에 대응한다.
struct S_CARD_TEXT_RUN
{
	S_DIP_RECT Rect{};
	std::wstring sText;
	bool bDrawn{ false };
};

struct S_CARD_LIST_ROW_FRAME
{
	std::size_t nRow{ 0 };
	std::string sCardId;
	S_DIP_RECT RowRect{};
	S_DIP_RECT CardRect{};
	S_DIP_RECT ContentRect{};
	d2d::Color nFillColor{ 0 };
	d2d::Color nTextColor{ 0 };
	d2d::Color nBorderColor{ 0 };
	bool bHovered{ false };
	bool bSelected{ false };
	std::vector<S_CARD_TEXT_RUN> BodyLines;
	S_CARD_TEXT_RUN TimeRun{};
	S_CARD_TEXT_RUN SuffixRun{};
};

struct S_CARD_LIST_FRAME
{
	std::size_t nFirstVisibleRow{ 0 };
	std::size_t nLastVisibleRow{ 0 };
	// 이 프레임에서 실제로 그린 텍스트 런 수다(카드당 본문 N + 보조행 1~2).
	std::size_t nLayoutCount{ 0 };
	bool bPresented{ false };
	std::vector<S_CARD_LIST_ROW_FRAME> Rows;
	// 삭제 오버레이는 목록 프레임 안에서 그린다(새 HWND·스왑 타깃 없음, spec §3.3.1).
	// 기하 단언이 논리로 닫히도록 그리기에 쓴 값을 그대로 싣는다.
	bool bDeleteZoneVisible{ false };
	S_DIP_RECT DeleteZoneRect{};
	d2d::Color nDeleteZoneColor{ 0 };
};

// Qt QAbstractItemView 의 상태 기계 중 S2 가 쓰는 세 가지다. Dragging 은 S2 에서
// 드래그 시작 전 단계일 뿐이고 S4 가 DoDragDrop 으로 본체를 채운다.
enum class E_CARD_LIST_VIEW_STATE { NoState, DragSelecting, Dragging };

enum class E_CARD_INPUT_PHASE { Press, Move, Release, Key };

enum class E_CARD_SELECTION_COMMAND
{
	NoUpdate, ClearAndSelect, Select, Deselect, Toggle,
	// Shift 범위(앵커에서 대상까지) / 고무줄 띠 치환 / Ctrl 고무줄 띠 토글.
	SelectCurrent, BandReplace, BandToggle
};

// ResolveSelectionCommand 의 입력. Win32 호출 없는 순수 함수라 시험이 표 구동으로
// 직접 부른다(spec §3.2.1).
struct S_CARD_SELECTION_INPUT
{
	// 값 초기화는 Single(0) 이다 - 열거자 이름은 정의를 읽는 쪽에서만 쓴다.
	pynote::core::domain::E_CARD_SELECTION_MODE eMode{};
	E_CARD_INPUT_PHASE ePhase{ E_CARD_INPUT_PHASE::Press };
	bool bRowValid{ false };
	bool bRowSelected{ false };
	bool bPressedAlreadySelected{ false };
	bool bSamePressedRow{ false };
	bool bCtrl{ false };
	bool bShift{ false };
	bool bDragSelecting{ false };
	int nKey{ 0 };
};

// Qt QAbstractItemViewPrivate::selectionCommand 의 쌍둥이. 오라클 표(spec 부록 A)가
// 단언이고 이 함수는 그 표를 재생산하는 규칙 집합이다.
E_CARD_SELECTION_COMMAND ResolveSelectionCommand(const S_CARD_SELECTION_INPUT& _Input) noexcept;

// 원본 _DragSnapshot(card_stream.py:283~296 press 분기) 자리다. S2 는 기록만 하고
// S4(드래그 앤 드롭)가 소비한다.
struct S_CARD_DRAG_SNAPSHOT
{
	std::string sCardId;
	std::optional<std::string> sRevisionId{};
	POINT PressPoint{};
};

// 드래그가 실제로 도는 동안의 상태다. press 상태와 별개 멤버인 것이 계약이다 -
// DoDragDrop 이 자기 캡처를 잡으면 WM_CAPTURECHANGED 가 press 상태를 지우는데, 원본은
// snapshot 을 finally 에서만 지우고 드롭 시점 판정이 전부 이 값을 읽는다(spec §3.1.7).
struct S_CARD_DRAG_SESSION
{
	pynote::core::domain::CardDragSessionToken nToken{ 0 };
	std::string sCardId;
	std::optional<std::string> sRevisionId{};
	// 원본 drag_executed - 실행되면 press 소비를 되돌리지 않는다(반환된 HRESULT 는 전부 실행).
	bool bExecuted{ false };
};

// OLE 드롭에는 "누가 끌었나" 가 실리지 않는다 - 원본 event.source() is self 에 대응물이
// 없으므로 데이터 개체가 자기 원본을 밝히고 드롭 대상이 자기 것과 대조한다(제품 경로).
struct __declspec(uuid("6C1D2E44-9E4A-4A3B-9E2C-2B0E1F5A7C31")) I_CARD_DRAG_SOURCE : public IUnknown
{
	virtual pynote::core::domain::CardDragSourceIdentity STDMETHODCALLTYPE SourceIdentity() = 0;
};

// QApplication.startDragDistance() 실측값(오라클 [ENV]). SM_CXDRAG 가 아니다.
inline constexpr int CARD_DRAG_DISTANCE_DIP = 10;
// QApplication.keyboardInputInterval() 기본값(타입어헤드 검색 재시작 간격).
inline constexpr std::uint64_t CARD_KEYBOARD_SEARCH_INTERVAL_MS = 400;
// PostDeferred 가 자기 자신에게 보내는 사설 메시지(QTimer.singleShot(0, ...) 쌍둥이).
// CApplication 은 다른 HWND 에서 WM_APP+20/21 을 쓰므로 충돌하지 않는다.
inline constexpr UINT CARD_LIST_DEFERRED_MESSAGE = WM_APP + 0x41;
// 휠 정숙 열기 타이머의 id 다. 이 컨트롤에는 다른 타이머가 없으므로 같은 id 를 다시 걸면
// 이전 타이머가 대체되고, 그것이 원본 QTimer.start() 재시작과 같은 관측을 만든다.
inline constexpr UINT_PTR CARD_LIST_WHEEL_TIMER_ID = 1;

// 드래그 등록소·원본 등록·COM 개체 수명은 core 헤더와 COM 스마트 포인터를 요구하므로
// .cpp 의 정의로 감춘다(이 헤더는 ATL/WTL 과 함께 #undef CreateEvent 앞에서 읽힌다).
struct S_CARD_DRAG_INTERNAL;

class C_CARD_LIST final : public CWindowImpl<C_CARD_LIST>
{
public:
	DECLARE_WND_CLASS_EX(L"NoteExCardList", CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW, -1)

	using ActivateHandler = std::function<void()>;
	using OpenCardHandler = std::function<void(const std::string&)>;
	using EmptyAreaClickHandler = std::function<void()>;
	using DeleteHandler = std::function<void(std::vector<std::string>)>;
	// 원본 card_browse_requested 의 자리다. 반환값은 페이지 _open_card 의 결과이고
	// core CompleteOpen 이 그 값으로 선택 복원 여부를 가른다.
	using BrowseCardHandler = std::function<bool(const std::string&)>;
	// 원본 self.editor.card_id 는 열기 실패 뒤에 읽힌다 - 미리 받아 두지 않고 그 시점에 부른다.
	using EditorCardProvider = std::function<std::optional<std::string>()>;

	// ---- 드래그 앤 드롭·컨텍스트 메뉴(S4) ----
	// 원본 QDrag.exec 자리. **기본값 없음(실행됨·취소 의미); 셸이 ::DoDragDrop 을 설치한다.**
	// pEffect 는 호출 전에 "제안 동작"(원본 drag.exec 의 두 번째 인자 = Copy)으로 채워 넣는다.
	using DragRunner = std::function<HRESULT(IDataObject*, IDropSource*, DWORD, DWORD*)>;
	// 원본 _execute_context_menu 자리. 고른 명령 id 를 돌리고 취소는 0 이다.
	// 컨트롤 기본값은 비어 있다 - 진짜 TrackPopupMenu 는 셸(CMain bind_card_list)이 건다.
	using MenuExecutor = std::function<UINT(HMENU, POINT)>;
	// EndDraw 직전의 장치 컨텍스트를 넘겨 주는 관측 seam(기본 미설치).
	using FrameCaptureHook = std::function<void(ID2D1DeviceContext*)>;
	// 원본 set_drag_body_provider. 미설치면 프로젝션의 확정 본문을 싣는다.
	using DragBodyProvider = std::function<std::string(const std::string&)>;
	// 원본 card_move_requested / card_delete_dropped / drag_started / drag_finished.
	using MoveCardHandler = std::function<void(const std::string&, const std::optional<std::string>&)>;
	using DeleteDroppedHandler = std::function<void(const std::string&)>;
	using DragStartedHandler =
		std::function<void(const std::string&, pynote::core::domain::CardDragSessionToken)>;
	using DragFinishedHandler = std::function<void(pynote::core::domain::CardDragSessionToken)>;

	C_CARD_LIST();
	~C_CARD_LIST();
	C_CARD_LIST(const C_CARD_LIST&) = delete;
	C_CARD_LIST& operator=(const C_CARD_LIST&) = delete;

	void Bind(pynote::core::domain::C_CARD_LIST_PROJECTION& _Projection) noexcept;
	void SetActivateHandler(ActivateHandler _Handler);
	// 원본 card_open_requested / empty_area_clicked / cards_delete_requested 의 자리다.
	void SetOpenCardHandler(OpenCardHandler _Handler);
	void SetEmptyAreaClickHandler(EmptyAreaClickHandler _Handler);
	void SetDeleteHandler(DeleteHandler _Handler);
	void SetBrowseCardHandler(BrowseCardHandler _Handler);
	void SetEditorCardProvider(EditorCardProvider _Provider);
	void SetDragRunner(DragRunner _Runner);
	void SetContextMenuExecutor(MenuExecutor _Executor);
	void SetFrameCaptureHook(FrameCaptureHook _Hook);
	void SetDragBodyProvider(DragBodyProvider _Provider);
	void SetMoveCardHandler(MoveCardHandler _Handler);
	void SetDeleteDroppedHandler(DeleteDroppedHandler _Handler);
	void SetDragStartedHandler(DragStartedHandler _Handler);
	void SetDragFinishedHandler(DragFinishedHandler _Handler);
	// 디바이스·브러시 캐시·텍스트 엔진은 CApplication 소유다. 붙지 않으면 그리지 않고
	// 나머지(행·LB 메시지·Enter·스크롤 산술)는 그대로 동작한다.
	void AttachRenderServices(d2d::C_D2D_DEVICE* _pDevice,
		d2d::C_D2D_BRUSH_CACHE* _pBrushCache, d2d::C_D2D_TEXT* _pText) noexcept;
	void SetDisplaySettings(const S_CARD_LIST_DISPLAY& _Display);

	// 프로젝션 행 집합이 바뀐 뒤 호출한다 - 내용 높이·스크롤 클램프·스크롤바를 다시 잡는다.
	void OnProjectionChanged();

	// 묶인 프로젝션(Bind 전에는 nullptr). 페이지 수준 시험이 선택을 읽는 관측 seam 이다.
	const pynote::core::domain::C_CARD_LIST_PROJECTION* Projection() const noexcept
	{
		return(m_pProjection);
	}
	// 클라이언트 픽셀 좌표의 행 판정(뷰포트 밖이면 nullopt). S4 도 이 자리를 다시 쓴다.
	std::optional<std::size_t> HitTestRow(POINT _ClientPx) const;
	E_CARD_LIST_VIEW_STATE ViewState() const noexcept { return(m_eViewState); }
	// Shift 범위의 기준 행(Qt currentSelectionStartIndex). 카드 id 로 들고 있다가 행으로 푼다.
	std::optional<std::size_t> AnchorRow() const;
	bool IsRowSelected(std::size_t _nRow) const;
	// QTimer.singleShot(0, ...) 의 네이티브 쌍둥이. 사설 메시지 도착 시 선입선출로 실행한다.
	void PostDeferred(std::function<void()> _Callable);

	// ---- 휠 탐색(S3). 각 누적·클램프·세대·데드라인·복원 정책은 core 가 소유한다. ----
	// 원본 _pending_browse_card_id / _wheel_angle / cancel_pending_browse 의 관측·조작 자리다.
	std::optional<std::string> PendingBrowseCardId() const;
	int WheelAngleRemainder() const;
	void CancelPendingBrowse();
	// Qt setCurrentIndex(index) 의 쌍둥이 - 선택 치환·앵커·Shift 기준·현재 관측·자동 스크롤까지다.
	// 대기 중 휠 탐색은 취소하지 않는다(원본 setCurrentIndex 도 취소하지 않고, 만료 시점의
	// 동일성 검사가 대신 막는다).
	void SetCurrentRow(std::size_t _nRow);
	// 원본 reveal_card(document_page.py:286~296)의 목록 쪽 절반 - 취소 뒤 SetCurrentRow 다.
	void RevealRow(std::size_t _nRow);

	// ---- 드래그 앤 드롭(S4). 정책(토큰 발급·4중 Validate·정렬 모드 술어)은 core 소유다. ----
	// 원본 active_drag_revision(card_stream.py:205~210) - 요청 카드가 현재 세션의 카드일 때만
	// press 시점 리비전을 돌린다. 네이티브 출처는 press 스냅샷이 아니라 세션 기록이다(spec §3.3.5).
	std::optional<std::string> ActiveDragRevision(const std::string& _sCardId) const;
	std::optional<pynote::core::domain::CardDragSessionToken> ActiveDragToken() const;
	// 원본 zone.arm(token) / disarm() + show()/hide(). 무장 토큰이 곧 표시 여부다.
	void ArmDeleteZone(pynote::core::domain::CardDragSessionToken _nToken);
	void DisarmDeleteZone();
	std::optional<pynote::core::domain::CardDragSessionToken> ArmedDeleteToken() const;
	// 그린 프레임과 무관하게 읽는 오버레이 기하(원본 _position_delete_drop_zone 축자 이식).
	S_DIP_RECT DeleteZoneRectDip() const;
	// 원본 model.mimeData((index,)) 자리 - 지금의 payload 상태(토큰·본문)로 데이터 개체를
	// 만든다. 호출자가 Release 하며, 정리 뒤 다시 만들면 토큰이 0 이다(spec §3.1.8).
	IDataObject* CreateDragDataObject(const std::string& _sCardId) const;
	// 시험이 모달 루프 없이 DragEnter/DragOver/Drop 을 직접 몰기 위한 자리다(제품 경로).
	IDropTarget* DropTargetForTest() const noexcept;
	// RegisterDragDrop 의 마지막 결과와 현재 등록 보유 여부. 실패는 치명적이지 않다.
	HRESULT DropRegistrationResult() const noexcept;
	bool HasDropRegistration() const noexcept;
	// 원본 Qt 줄 스크롤 양(spec §3.1.12): wheelScrollLines x (-delta/120) x 행 높이를
	// 절사하고 한 페이지(뷰포트 높이)로 묶는다. 반환값은 오프셋 증분이다.
	int ScrollLinesForWheel(int _nDelta) const;

	std::size_t RowCount() const noexcept;
	int LineSpacingDip() const;
	int RowHeightDip() const;
	int ViewportWidthDip() const;
	int ViewportHeightDip() const;
	int ContentHeightDip() const;
	S_DIP_RECT RowRectDip(std::size_t _nRow) const;

	int ScrollOffsetDip() const noexcept { return(m_nScrollOffsetDip); }
	void ScrollToPixel(int _nOffsetDip);
	void EnsureVisible(std::size_t _nRow);

	// 동기 페인트 1회. 반환값은 present 성공 여부다.
	bool Render();
	const S_CARD_LIST_FRAME& LastFrame() const noexcept { return(m_Frame); }
	const S_CARD_PALETTE& Palette() const noexcept { return(m_Palette); }
	std::wstring TimeLabel(std::int64_t _nEpochUs) const;

	BEGIN_MSG_MAP(C_CARD_LIST)
		MESSAGE_HANDLER(WM_CREATE, OnCreate)
		MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBkgnd)
		MESSAGE_HANDLER(WM_PAINT, OnPaint)
		MESSAGE_HANDLER(WM_SIZE, OnSize)
		MESSAGE_HANDLER(WM_DPICHANGED_AFTERPARENT, OnDpiChangedAfterParent)
		MESSAGE_HANDLER(WM_VSCROLL, OnVScroll)
		MESSAGE_HANDLER(WM_MOUSEMOVE, OnMouseMove)
		MESSAGE_HANDLER(WM_MOUSELEAVE, OnMouseLeave)
		MESSAGE_HANDLER(WM_LBUTTONDOWN, OnLButtonDown)
		// Qt 는 mouseDoubleClickEvent 를 재정의하지 않는다 - 두 번째 press 와 같다(오라클 N5).
		MESSAGE_HANDLER(WM_LBUTTONDBLCLK, OnLButtonDown)
		MESSAGE_HANDLER(WM_LBUTTONUP, OnLButtonUp)
		MESSAGE_HANDLER(WM_RBUTTONDOWN, OnRButtonDown)
		MESSAGE_HANDLER(WM_RBUTTONUP, OnRButtonUp)
		// 우버튼 릴리스를 더는 삼키지 않는다 - DefWindowProc 이 WM_CONTEXTMENU 를 합성하고
		// 키보드 컨텍스트 키(VK_APPS·Shift+F10)도 같은 자리로 들어온다(spec §3.4.1).
		MESSAGE_HANDLER(WM_CONTEXTMENU, OnContextMenu)
		MESSAGE_HANDLER(WM_CAPTURECHANGED, OnCaptureChanged)
		// WM_MOUSEHWHEEL 은 처리하지 않는다 - DefWindowProc 이 부모로 올려 보내는 것이
		// 원본의 "수평 각은 accept 하지 않는다"(오라클 P9)와 같은 관측이다.
		MESSAGE_HANDLER(WM_MOUSEWHEEL, OnMouseWheel)
		MESSAGE_HANDLER(WM_TIMER, OnTimer)
		// 가운데·X 버튼은 소비하지 않고 취소만 한다(Qt 도 mousePressEvent 를 주고 원본이 취소한다).
		MESSAGE_HANDLER(WM_MBUTTONDOWN, OnOtherButtonDown)
		MESSAGE_HANDLER(WM_XBUTTONDOWN, OnOtherButtonDown)
		// Alt 계열 키를 Win32 는 WM_SYSKEYDOWN/WM_SYSCHAR 로 돌리지만 Qt 는 같은 keyPressEvent 다.
		MESSAGE_HANDLER(WM_SYSKEYDOWN, OnSysKey)
		MESSAGE_HANDLER(WM_SYSCHAR, OnSysKey)
		MESSAGE_HANDLER(WM_KEYDOWN, OnKeyDown)
		MESSAGE_HANDLER(WM_CHAR, OnChar)
		MESSAGE_HANDLER(CARD_LIST_DEFERRED_MESSAGE, OnDeferred)
		MESSAGE_HANDLER(WM_SETFOCUS, OnFocusChanged)
		MESSAGE_HANDLER(WM_KILLFOCUS, OnFocusChanged)
		MESSAGE_HANDLER(WM_SYSCOLORCHANGE, OnPaletteChanged)
		MESSAGE_HANDLER(WM_THEMECHANGED, OnPaletteChanged)
		MESSAGE_HANDLER(WM_SETTINGCHANGE, OnPaletteChanged)
		MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
		MESSAGE_HANDLER(LB_GETCOUNT, OnListGetCount)
		MESSAGE_HANDLER(LB_GETCURSEL, OnListGetCurSel)
		MESSAGE_HANDLER(LB_SETCURSEL, OnListSetCurSel)
		MESSAGE_HANDLER(LB_GETTOPINDEX, OnListGetTopIndex)
		MESSAGE_HANDLER(LB_SETTOPINDEX, OnListSetTopIndex)
	END_MSG_MAP()

	LRESULT OnCreate(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnEraseBkgnd(UINT, WPARAM, LPARAM, BOOL&) { return(1); }
	LRESULT OnPaint(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnSize(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnDpiChangedAfterParent(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnVScroll(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnMouseMove(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnMouseLeave(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnLButtonDown(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnLButtonUp(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnRButtonDown(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnRButtonUp(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnContextMenu(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnCaptureChanged(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnMouseWheel(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnTimer(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnOtherButtonDown(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnSysKey(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnKeyDown(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnChar(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnDeferred(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnFocusChanged(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnPaletteChanged(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnListGetCount(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnListGetCurSel(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnListSetCurSel(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnListGetTopIndex(UINT, WPARAM, LPARAM, BOOL&);
	LRESULT OnListSetTopIndex(UINT, WPARAM, LPARAM, BOOL&);

private:
	int client_dip_(int _nPixels) const noexcept;
	bool ensure_target_();
	void handle_device_lost_();
	void capture_palette_();
	void resolve_font_();
	IDWriteTextFormat* text_format_() const;
	int max_scroll_offset_dip_() const;
	void update_scroll_bar_();
	void invalidate_row_(std::optional<std::size_t> _nRow);
	std::optional<std::size_t> row_at_dip_(int _nYdip) const;
	std::optional<std::size_t> current_row_() const;
	POINT point_from_lparam_(LPARAM _lParam) const noexcept;
	bool inside_viewport_(POINT _Point) const;
	std::optional<std::size_t> row_at_point_(POINT _Point) const;
	// "현재 카드를 한 번이라도 본 적이 있는가" - Qt 의 currentIndexSet 근사(spec §3.1.2).
	void observe_current_();
	void set_anchor_(std::optional<std::size_t> _nRow);
	void select_row_additive_(std::size_t _nRow);
	void deselect_row_(std::size_t _nRow);
	std::vector<std::string> band_rows_(int _nCursorYdip) const;
	std::size_t page_row_(std::size_t _nCurrentRow, bool _bDown) const;
	void apply_selection_command_(E_CARD_SELECTION_COMMAND _eCommand, E_CARD_INPUT_PHASE _ePhase,
		std::optional<std::size_t> _nTargetRow, std::optional<std::size_t> _nOldCurrentRow,
		const std::vector<std::string>& _Band);
	void handle_navigation_key_(int _nKey);
	void handle_space_key_();
	void keyboard_search_(wchar_t _Char);
	bool prefix_match_(std::size_t _nRow, const std::wstring& _sPrefix) const;
	void reset_press_state_();
	// ---- 드래그 앤 드롭 내부(S4) ----
	pynote::core::domain::CardDragSourceIdentity source_identity_() const noexcept;
	void ensure_drop_registration_();
	// 원본 startDrag(card_stream.py:439~488)의 순서를 그대로 도는 자리다.
	void begin_drag_();
	// 원본 finally 의 세 블록. 호출 자리는 정확히 둘(러너 반환 직후·OnDestroy)이고 멱등이다.
	void finish_drag_session_();
	std::string drag_body_(const std::string& _sCardId) const;
	// 원본 _drop_before_card_id(card_stream.py:548~560) - 행 반쪽 판정이다.
	std::optional<std::string> drop_before_card_(POINT _PointDip) const;
	bool zone_hit_(POINT _PointDip) const;
	POINT client_point_from_screen_(POINTL _Screen) const;
	// DragEnter/DragOver 의 여섯 술어(정렬·원본·payload·세션·토큰·카드/리비전).
	bool accepts_session_payload_(IDataObject* _pData) const;
	bool accepts_row_drag_(IDataObject* _pData) const;
	bool accepts_zone_drag_(IDataObject* _pData) const;
	DWORD handle_drag_over_(IDataObject* _pData, POINTL _Screen) const;
	DWORD handle_drop_(IDataObject* _pData, POINTL _Screen);
	void draw_delete_zone_(ID2D1DeviceContext* _pDc);
	// 원본 _build_context_menu / _execute_context_menu(card_stream.py:499~527, :495~497).
	void show_context_menu_(POINT _ClientDip, POINT _Screen);
	bool copy_body_to_clipboard_(const std::string& _sBody) const;
	// core 스케줄러 포트의 구현. Arm 은 SetTimer, Cancel 은 KillTimer 다.
	void schedule_wheel_timer_(const pynote::core::domain::S_CARD_WHEEL_TIMER_COMMAND& _Command);
	bool render_();
	bool draw_text_(ID2D1DeviceContext* _pDc, IDWriteTextFormat* _pFormat,
		ID2D1Brush* _pBrush, const S_DIP_RECT& _Rect, const std::wstring& _sText);
	void draw_row_(ID2D1DeviceContext* _pDc, std::size_t _nRow, IDWriteTextFormat* _pFormat);

	pynote::core::domain::C_CARD_LIST_PROJECTION* m_pProjection{ nullptr };
	ActivateHandler m_Activate;
	OpenCardHandler m_OpenCard;
	EmptyAreaClickHandler m_EmptyAreaClick;
	DeleteHandler m_Delete;
	d2d::C_D2D_DEVICE* m_pDevice{ nullptr };
	d2d::C_D2D_BRUSH_CACHE* m_pBrushCache{ nullptr };
	d2d::C_D2D_TEXT* m_pText{ nullptr };
	d2d::C_D2D_SWAP_TARGET m_Target;
	bool m_bTargetReady{ false };
	bool m_bRecoveringDevice{ false };
	S_CARD_LIST_DISPLAY m_Display;
	std::string m_sTimeFormatUtf8;
	std::string m_sTimeZoneUtf8;
	S_CARD_LIST_FONT m_Font;
	pynote::core::domain::C_TIME_ZONE_RESOLVER m_TimeZone;
	S_CARD_PALETTE m_Palette;
	S_CARD_LIST_FRAME m_Frame;
	int m_nScrollOffsetDip{ 0 };
	UINT m_nDpi{ USER_DEFAULT_SCREEN_DPI };
	// 줄 높이는 폰트·DPI 가 바뀔 때만 달라지는데 스크롤 산술이 매번 부르므로 캐시한다.
	mutable int m_nLineSpacingDip{ 0 };
	std::optional<std::size_t> m_nHoverRow{};
	bool m_bTrackingMouse{ false };

	// ---- 선택 입력 계층(S2). Qt QAbstractItemViewPrivate 의 press 기록에 대응한다. ----
	E_CARD_LIST_VIEW_STATE m_eViewState{ E_CARD_LIST_VIEW_STATE::NoState };
	POINT m_PressPoint{};
	std::optional<std::size_t> m_PressedRow{};
	bool m_bPressActive{ false };
	bool m_bPressOnEmpty{ false };
	bool m_bPressedAlreadySelected{ false };
	bool m_bDragConsumedPress{ false };
	// 원본 _empty_press_position 은 "수식키 없는 뷰포트 안 빈 영역 press" 만 기록한다.
	bool m_bEmptyPress{ false };
	bool m_bEmptyPressMoved{ false };
	// Qt noSelectionOnMousePress - release 명령을 적용할지 가르는 문이다.
	bool m_bNoSelectionOnMousePress{ false };
	// Qt ctrlDragSelectionFlag: press 한 행이 선택돼 있지 않았으면 띠는 선택, 선택돼 있었으면 해제다.
	bool m_bCtrlDragSelect{ true };
	std::optional<std::string> m_sAnchorCardId{};
	std::vector<std::string> m_ShiftBase;
	std::optional<S_CARD_DRAG_SNAPSHOT> m_DragSnapshot{};
	bool m_bFocusByMouse{ false };
	bool m_bCurrentObserved{ false };
	std::vector<std::function<void()>> m_Deferred;
	std::wstring m_sSearchInput;
	std::uint64_t m_nSearchTickMs{ 0 };

	// ---- 휠 탐색(S3). 상태 기계가 프로젝션을 참조로 잡으므로 Bind 마다 새로 만든다. ----
	std::unique_ptr<pynote::core::domain::C_CARD_WHEEL_BROWSE> m_pWheel;
	// 마지막 Arm 의 세대다. WM_TIMER 가 도착하면 core 가 이 값으로 신선도를 판정한다.
	std::uint64_t m_nWheelTimerGeneration{ 0 };
	BrowseCardHandler m_BrowseCard;
	EditorCardProvider m_EditorCard;

	// ---- 드래그 앤 드롭·컨텍스트 메뉴(S4) ----
	// press 상태와 분리된 세션이다. reset_press_state_ 는 이것을 건드리지 않는다(spec §3.1.7).
	std::optional<S_CARD_DRAG_SESSION> m_DragSession{};
	std::optional<pynote::core::domain::CardDragSessionToken> m_nArmedDeleteToken{};
	// 원본 CardListModel 의 _drag_token / _drag_body 다(card_model.py:188~197). 정리는
	// 토큰 0 · 본문 없음이며, 그 뒤 다시 만든 payload 는 확정 본문과 토큰 0 을 싣는다.
	pynote::core::domain::CardDragSessionToken m_nDragPayloadToken{ 0 };
	std::optional<std::string> m_sDragPayloadBody{};
	// 등록소·원본 등록·COM 개체는 core 와 COM 헤더를 요구하므로 .cpp 정의로 감춘다.
	std::unique_ptr<S_CARD_DRAG_INTERNAL> m_pDragInternal;
	DragRunner m_DragRunner;
	MenuExecutor m_MenuExecutor;
	FrameCaptureHook m_FrameCapture;
	DragBodyProvider m_DragBody;
	MoveCardHandler m_MoveCard;
	DeleteDroppedHandler m_DeleteDropped;
	DragStartedHandler m_DragStarted;
	DragFinishedHandler m_DragFinished;
};
