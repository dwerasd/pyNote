#pragma once

#include <windows.h>

// windows.h 의 CreateEvent 매크로가 살아 있는 채로 아래 core 헤더를 읽는다 - 여기서 걷으면
// ATL/WTL 템플릿(atlbase.h:5360·atlapp.h:1068)의 ::CreateEvent 호출이 인스턴스화 시점에
// 식별자를 잃는다(실측 C3861/C2039). 매크로가 repositories.h 의 CreateEvent 멤버 이름을
// 바꾸지만 이 헤더를 읽는 TU 중 그 멤버를 부르는 것은 없다(종전 CApplication.cpp 와 같다).
#include "CChangeBus.h"
#include "pynote/core/application/card_service.h"
#include "pynote/core/application/file_binding_service.h"
#include "pynote/core/application/import_pipeline.h"
#include "pynote/core/application/window_lifecycle.h"
#include "pynote/core/domain/models.h"
#include "pynote/core/domain/paragraph_parser.h"
#include "pynote/core/storage/repositories.h"
#include "pynote/platform/win32_file_binding_support.h"
#include "pynote/platform/win32_import_support.h"
#include "pynote/platform/win32_single_instance.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace d2d
{
	class C_D2D_BRUSH_CACHE;
	class C_D2D_DEVICE;
	class C_D2D_TEXT;
}

namespace pynote::platform
{
	class C_WIN32_DEVICE_SETTINGS;
}

namespace pynote::core::application
{
	class C_CARD_SERVICE;
	class C_DRAFT_COORDINATOR;
	class C_SAVE_COORDINATOR;
}
namespace pynote::core::storage { class C_DATABASE; class C_REPOSITORIES; }

class C_SEARCH_DIALOG;

// 기동 인자·두 번째 실행이 넘긴 경로를 여는 판정이다 - 원본 WindowManager.open_path
// (app.py:660~684) + MainWindow.open_file_path(ui/main_window.py:1230~1253) +
// DocumentPage.open_file(ui/document_page.py:315~360) 중 포팅 가능한 부분.
//
// 판정을 헤더에 두는 이유는 CChangeBus.h 머리 주석과 같다 - 시험 프로젝트는 CApplication.cpp
// 를 컴파일할 수 없다(g_pConfig/g_pLog 정의가 WinMain.cpp 뿐이고 C_MAIN 은 완전 초기화된
// CApplication 없이 서지 않는다). 저장소·카드 서비스·문단 파서·결속 파일 시스템은 시험도
// 실물을 쓰고 대역은 창과 대화상자뿐이다.
struct S_OPEN_PATH_SHELL
{
	// 원본 _report_path_open_failure(app.py:702~711)·_error_reporter 자리. 결선 기본값은 MessageBoxW 다.
	std::function<void(const std::wstring& _sTitle, const std::wstring& _sText)> ShowDialog;
	// 결속 적중 갈래 - 소유 창(없으면 요청 창)에 문서를 열고 카드를 표시·활성화한다.
	std::function<bool(const std::string& _sCardId)> RouteBoundCard;
	// 새 결속 갈래 - 창을 만들고 그 창이 연 문서 id 를 돌려준다(원본 create_window).
	// 이름에 CreateWindow 를 쓰지 않는다 - windows.h 의 매크로가 CreateWindowW 로 바꾼다.
	std::function<bool(std::string* _psDocumentId)> CreateWindowForFile;
	// 방금 만든 창에서 카드를 표시하고 그 창을 활성화한다.
	std::function<bool(const std::string& _sCardId)> RevealAndActivate;
	// 원본 _now_us. bound_at_us 와 synced_at_us 는 같은 시각이다(document_page.py:874~891).
	std::function<std::int64_t()> Now;
};

inline bool ResolveOpenPath(
	const std::wstring& _sPath,
	pynote::core::storage::C_REPOSITORIES& _Repositories,
	pynote::core::application::C_CARD_SERVICE& _CardService,
	const pynote::core::domain::C_PARAGRAPH_PARSER& _Parser,
	pynote::core::application::I_BINDING_FILE_SYSTEM& _FileSystem,
	const S_OPEN_PATH_SHELL& _Shell)
{
	namespace app = pynote::core::application;
	namespace domain = pynote::core::domain;
	namespace storage = pynote::core::storage;

	const std::wstring sFileName = std::filesystem::path(_sPath).filename().native();
	std::string sUtf8Path;
	{
		if (_sPath.empty()) { return(false); }
		const int nRequired = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
			_sPath.data(), static_cast<int>(_sPath.size()), nullptr, 0, nullptr, nullptr);
		if (nRequired <= 0) { return(false); }
		sUtf8Path.assign(static_cast<std::size_t>(nRequired), '\0');
		if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, _sPath.data(),
			static_cast<int>(_sPath.size()), sUtf8Path.data(), nRequired, nullptr, nullptr) != nRequired)
		{
			return(false);
		}
	}
	std::string sResolved;
	std::string sPathKey;
	if (!pynote::platform::ResolveBindingPath(sUtf8Path, &sResolved, &sPathKey)) { return(false); }

	// 1) 결속 적중 - 그 카드를 이미 연 창으로 라우팅한다(원본 app.py:663~672).
	domain::S_FILE_BINDING Existing;
	const auto eExisting = _Repositories.FindActiveBindingByPath(sPathKey, &Existing);
	if (eExisting == storage::E_REPO_RESULT::Ok) { return(_Shell.RouteBoundCard(Existing.sCardId)); }
	if (eExisting != storage::E_REPO_RESULT::NotFound) { return(false); }

	// 2) 부재 경로·디렉터리 - 알리고 창을 만들지 않는다(원본 app.py:667~669·702~711).
	std::error_code FileError;
	if (!std::filesystem::is_regular_file(_sPath, FileError))
	{
		std::error_code DirectoryError;
		const bool bDirectory = std::filesystem::is_directory(_sPath, DirectoryError);
		_Shell.ShowDialog(L"파일 열기", sFileName +
			(bDirectory ? L": 디렉터리는 열 수 없습니다." : L": 파일을 찾을 수 없습니다."));
		return(false);
	}

	// 3) 새 파일. 상한 + 1 로 읽어야 4 MiB 초과를 절단이 아니라 초과로 판정한다
	// (원본 document_page.py:322~333, import_pipeline.cpp:136~137 과 같은 관례).
	std::vector<std::uint8_t> Bytes;
	std::string sReadError;
	if (!pynote::platform::ReadFileBounded(
		sResolved, app::MAX_IMPORT_FILE_BYTES + 1, &Bytes, &sReadError))
	{
		std::wstring sWideError;
		{
			const int nRequired = sReadError.empty() ? 0 : ::MultiByteToWideChar(
				CP_UTF8, 0, sReadError.data(), static_cast<int>(sReadError.size()), nullptr, 0);
			if (nRequired > 0)
			{
				sWideError.assign(static_cast<std::size_t>(nRequired), L'\0');
				::MultiByteToWideChar(CP_UTF8, 0, sReadError.data(),
					static_cast<int>(sReadError.size()), sWideError.data(), nRequired);
			}
		}
		_Shell.ShowDialog(L"파일 열기 실패", sFileName + L": " + sWideError);
		return(false);
	}
	if (Bytes.size() > app::MAX_IMPORT_FILE_BYTES)
	{
		_Shell.ShowDialog(L"파일 열기 실패", sFileName + L": 파일당 4 MiB 상한을 초과했습니다.");
		return(false);
	}

	app::S_DETECTED_TEXT Detected;
	if (!app::DetectText(Bytes, app::StrictLegacyDecoder(
		pynote::platform::DecodeSystemAnsiStrict), &Detected))
	{
		// 원본의 "사본 가져오기" 선택지(document_page.py:802~814)는 가져오기 UI 가 W7 이라
		// 안내만 남긴다 - 부분 이식이다.
		// 문구는 원본 document_page.py:807 그대로(파일명 접두) - 설계지시서 인용의 축약형이 아니다.
		_Shell.ShowDialog(L"결속할 수 없는 파일",
			sFileName + L" 파일은 텍스트로 해석되지 않아 편집 결과를 되쓸 수 없습니다.");
		return(false);
	}

	// 점유 판정을 카드 생성보다 먼저 한다 - 휴지통 카드의 결속 행 삭제라는 부작용이 있어
	// 순서가 관측된다(원본 document_page.py:341~342).
	app::S_BINDING_PATH_RESOLUTION Resolution;
	if (!app::PrepareBindingPath(_Repositories, sPathKey, &Resolution)) { return(false); }
	if (Resolution.eStatus == app::E_BINDING_PATH_STATUS::HeldByActiveCard)
	{
		_Shell.ShowDialog(L"파일 열기 실패", sFileName + L": 이미 다른 카드에 결속된 파일입니다.");
		return(false);
	}

	if (_Parser.IsZeroParagraphInput(Detected.sText))
	{
		// CEILING: 문단 0 파일은 W6 까지 열리지 않는다 - 결속 대기 상태를 들고 있을 편집
		// 세션이 포팅본에 없다.
		_Shell.ShowDialog(L"파일 열기", L"빈 파일 결속은 편집기 이식 뒤 지원됩니다");
		return(false);
	}

	std::string sDocumentId;
	if (!_Shell.CreateWindowForFile(&sDocumentId) || sDocumentId.empty()) { return(false); }
	std::vector<domain::S_CARD> Created;
	if (_CardService.CreateCards(sDocumentId, Detected.sText,
		domain::E_CAPTURE_OPERATION_SOURCE::Import, false, std::nullopt, &Created) !=
		app::E_CARD_SERVICE_RESULT::Ok)
	{
		_Shell.ShowDialog(L"파일 열기 실패", sFileName + L": 카드를 만들지 못했습니다.");
		return(false);
	}
	if (Created.size() != 1)
	{
		_Shell.ShowDialog(L"파일 열기 실패", sFileName + L": 카드가 정확히 한 장 생성되지 않았습니다.");
		return(false);
	}

	// 원본 _binding_for_opened_file(document_page.py:865~892) 그대로다 - synced_size 는 stat 의
	// 크기가 아니라 방금 읽은 바이트 길이이고, mtime 은 stat 실패 시 값 없음이다.
	const std::int64_t nNowUs = _Shell.Now();
	domain::S_FILE_BINDING Binding;
	Binding.sCardId = Created.front().sId;
	Binding.sPath = sResolved;
	Binding.sPathKey = sPathKey;
	Binding.sEncoding = Detected.sEncoding;
	Binding.bBom = Detected.bBom;
	Binding.eNewline = Detected.eNewline;
	Binding.bTrailingNewline = Detected.bTrailingNewline;
	Binding.nBoundAtUs = nNowUs;
	Binding.nSyncedSize = static_cast<std::int64_t>(Bytes.size());
	std::int64_t nStatSize = 0;
	std::int64_t nStatMtimeNs = 0;
	if (_FileSystem.Stat(sResolved, &nStatSize, &nStatMtimeNs)) { Binding.nSyncedMtimeNs = nStatMtimeNs; }
	Binding.sSyncedHash = app::HashBytes(Bytes);
	Binding.nSyncedAtUs = nNowUs;
	if (_Repositories.UpsertFileBinding(Binding) != storage::E_REPO_RESULT::Ok) { return(false); }
	return(_Shell.RevealAndActivate(Binding.sCardId));
}

class CApplication
{
public:
	enum class E_INITIALIZE_RESULT
	{
		Primary,
		SecondaryNotified,
		Failure,
	};

	CApplication();
	~CApplication();

	CApplication(const CApplication&) = delete;
	CApplication& operator=(const CApplication&) = delete;

	E_INITIALIZE_RESULT Initialize(
		HINSTANCE _hInstance, const pynote::platform::S_WIN32_STARTUP_OPTIONS& _Options);
	int Run();
	void Shutdown();

	bool CreateMainWindow();
	void RequestCloseWindow(pynote::core::application::WINDOW_TOKEN _Token);
	void RequestApplicationQuit();
	void NotifyWindowActivated(pynote::core::application::WINDOW_TOKEN _Token);
	void NotifyWindowNcDestroy(pynote::core::application::WINDOW_TOKEN _Token);

	// 앱이 버스를 하나 소유한다(CAP-PL-012). 창은 파괴 전에 구독을 해제한다.
	pynote::shell::C_DOCUMENT_CHANGE_BUS& ChangeBus();
	// 원본 publish_document_change(app.py:672~675) 의 2 단이다 - 발행 뒤 문서 -> 창
	// 소유 매핑을 다시 계산하고, 한 문서가 두 창에 열렸으면 false 다.
	bool PublishDocumentChange(const std::string& _sDocumentId);
	bool OpenDocument(
		pynote::core::application::WINDOW_TOKEN _Requesting, const std::string& _sDocumentId);
	std::optional<std::string> DocumentTitle(const std::string& _sDocumentId);
	// 외부 소멸로 비워진 창을 다시 채울 문서를 고른다(없으면 새로 만든다).
	std::optional<std::string> ChooseRefillDocument(
		pynote::core::application::WINDOW_TOKEN _Token);
	void StartAutomaticMaintenance();
	bool RunAutomaticMaintenance();

	bool PersistWindowState(
		pynote::core::application::WINDOW_TOKEN _Token,
		const std::string& _sWorkspaceId, const std::optional<std::string>& _sDocumentId);
	bool LoadDocumentSplit(
		const std::string& _sWorkspaceId, const std::string& _sDocumentId,
		std::optional<std::pair<int, int>>* _pSplitSizesDip);

	pynote::platform::C_WIN32_DEVICE_SETTINGS& Settings();
	pynote::core::storage::C_DATABASE& Database();
	pynote::core::storage::C_REPOSITORIES& Repositories();
	pynote::core::application::C_CARD_SERVICE& CardService();
	pynote::core::application::C_DRAFT_COORDINATOR& DraftCoordinator();
	pynote::core::application::C_SAVE_COORDINATOR& SaveCoordinator();
	HACCEL Accelerator() const noexcept;
	void ShowSearchDialog(HWND _hOwner);
	C_SEARCH_DIALOG& SearchDialog();
	d2d::C_D2D_DEVICE& D2DDevice();
	d2d::C_D2D_BRUSH_CACHE& BrushCache();
	d2d::C_D2D_TEXT& TextEngine();

private:
	struct S_STATE;
	std::unique_ptr<S_STATE> m_pState;
};
