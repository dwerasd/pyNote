#include "framework.h"
#include "CApplication.h"

#include "CChangeBus.h"
#include "CDocumentListShell.h"
#include "CMain.h"
#include "CSearchDialog.h"
#include "CWindowLayout.h"
#include "pynote/core/application/card_service.h"
#include "pynote/core/application/draft_coordinator.h"
#include "pynote/core/application/save_coordinator.h"
#include "pynote/core/domain/paragraph_parser.h"
#include "pynote/core/application/workspace_state.h"
#include "pynote/core/domain/models.h"
#include "pynote/core/storage/backup.h"
#include "pynote/core/storage/database.h"
#include "pynote/core/storage/migration_runner.h"
#include "pynote/core/storage/repositories.h"
#include "pynote/platform/win32_device_settings.h"
#include "pynote/platform/win32_file_system.h"

#include <D2DWrapp/D2DBrushCache.h>
#include <D2DWrapp/D2DDevice.h>
#include <D2DWrapp/D2DText.h>

#include <sqlite3/sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace
{
	constexpr wchar_t DISPATCHER_CLASS[] = L"NoteExApplicationDispatcher";
	constexpr UINT WM_NOTEEX_NEW_WINDOW = WM_APP + 20;
	constexpr UINT WM_NOTEEX_RETIRE_WINDOWS = WM_APP + 21;
	// 같은 디스패처 HWND·같은 창 프로시저를 공유하므로 위 둘을 재사용하지 않는다 - 창 은퇴
	// 포스트의 lParam 0 이 std::wstring* 로 해석되면 프로세스가 죽는다.
	constexpr UINT WM_NOTEEX_OPEN_FILE = WM_APP + 22;
	constexpr UINT_PTR MAINTENANCE_TIMER_ID = 1;

	std::string utf8(const std::wstring& _sValue)
	{
		if (_sValue.empty()) { return(std::string{}); }
		const int nSize = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
			_sValue.data(), static_cast<int>(_sValue.size()), nullptr, 0, nullptr, nullptr);
		if (nSize <= 0) { return(std::string{}); }
		std::string Result(static_cast<std::size_t>(nSize), '\0');
		if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, _sValue.data(),
			static_cast<int>(_sValue.size()), Result.data(), nSize, nullptr, nullptr) != nSize)
		{
			return(std::string{});
		}
		return(Result);
	}

	std::wstring wide(const std::string& _sValue)
	{
		if (_sValue.empty()) { return(std::wstring{}); }
		const int nSize = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			_sValue.data(), static_cast<int>(_sValue.size()), nullptr, 0);
		if (nSize <= 0) { return(std::wstring{}); }
		std::wstring Result(static_cast<std::size_t>(nSize), L'\0');
		if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, _sValue.data(),
			static_cast<int>(_sValue.size()), Result.data(), nSize) != nSize)
		{
			return(std::wstring{});
		}
		return(Result);
	}

	std::int64_t now_us()
	{
		return(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count());
	}

	// quick_check 요율 제한은 시스템 시각 변경에 흔들리면 안 되므로 단조시계다
	// (core backup.h 의 MonotonicSecFn 계약 - 벽시계와 재는 것이 다르다).
	double now_monotonic_seconds()
	{
		return(std::chrono::duration<double>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	}
}

struct CApplication::S_STATE : CMessageFilter
{
	CApplication* pOwner{ nullptr };
	HINSTANCE hInstance{ nullptr };
	CAppModule Module;
	CMessageLoop MessageLoop;
	pynote::platform::C_WIN32_FILE_SYSTEM FileSystem;
	pynote::platform::C_WIN32_DEVICE_SETTINGS Settings{ FileSystem };
	pynote::platform::C_WIN32_SINGLE_INSTANCE Instance;
	// 결속 행의 synced_mtime_ns 를 읽는 자리다. 되쓰기는 W6 이므로 여기서는 Stat 만 쓴다.
	pynote::platform::C_WIN32_BINDING_FILE_SYSTEM BindingFileSystem;
	// 기동 경로 안내의 seam. 결선 기본값은 MessageBoxW 이고 비대화형 구동은 대역을 끼운다.
	std::function<void(const std::wstring&, const std::wstring&)> DialogSink;
	pynote::core::storage::C_DATABASE Database;
	pynote::core::storage::C_BACKUP_SERVICE BackupService{ FileSystem };
	std::unique_ptr<pynote::core::storage::C_MIGRATION_BACKUP_HOOK> MigrationBackup;
	std::unique_ptr<pynote::core::storage::C_REPOSITORIES> Repositories;
	pynote::core::domain::C_PARAGRAPH_PARSER ParagraphParser;
	std::unique_ptr<pynote::core::application::C_CARD_SERVICE> CardService;
	std::unique_ptr<pynote::core::application::C_REPOSITORY_DRAFT_STORE> DraftStore;
	std::unique_ptr<pynote::core::application::C_DRAFT_COORDINATOR> DraftCoordinator;
	std::unique_ptr<pynote::core::application::C_SAVE_COORDINATOR> SaveCoordinator;
	C_SEARCH_DIALOG SearchDialog;
	HACCEL hAccelerator{ nullptr };
	d2d::C_D2D_DEVICE D2DDevice;
	d2d::C_D2D_BRUSH_CACHE BrushCache;
	d2d::C_D2D_TEXT TextEngine;
	pynote::core::application::C_WINDOW_REGISTRY Registry;
	pynote::core::application::C_WINDOW_LIFECYCLE Lifecycle;
	std::map<pynote::core::application::WINDOW_TOKEN, std::shared_ptr<C_MAIN>> Windows;
	std::vector<pynote::core::application::WINDOW_TOKEN> PendingRetirements;
	std::optional<pynote::shell::S_DATA_POLICY> DataPolicy;
	std::unique_ptr<pynote::core::storage::C_AUTOMATIC_BACKUP_MANAGER> BackupManager;
	std::unique_ptr<pynote::core::storage::C_PERIODIC_QUICK_CHECK> QuickCheck;
	pynote::shell::C_DOCUMENT_CHANGE_BUS ChangeBus;
	std::map<std::string, pynote::core::application::WINDOW_TOKEN> DocumentWindows;
	std::atomic<bool> bAcceptNewWindows{ false };
	HWND hDispatcher{ nullptr };
	bool bModuleInitialized{ false };
	bool bD2DInitialized{ false };
	bool bRuntimeFinalized{ false };
	bool bMessageFilterRegistered{ false };
	bool bGeometryMigrationAttempted{ false };
	bool bMaintenanceTimerActive{ false };
	std::uint64_t nIdentitySequence{ 1 };

	BOOL PreTranslateMessage(MSG* _pMessage) override
	{
		const auto Active = Registry.ActiveWindow();
		if (!Active) { return(FALSE); }
		const auto it = Windows.find(*Active);
		if (it == Windows.end() || !it->second) { return(FALSE); }
		return(pynote::shell::RouteFrameMessage(_pMessage, it->second->m_hWnd,
			[&](MSG* _pRouted) { return(it->second->PreTranslateMessage(_pRouted)); },
			hAccelerator) ? TRUE : FALSE);
	}

	std::string make_identity(const char* _pszPrefix)
	{
		return(std::string(_pszPrefix) + "-" + std::to_string(::GetCurrentProcessId()) + "-" +
			std::to_string(::GetTickCount64()) + "-" + std::to_string(nIdentitySequence++));
	}

	bool validate_policy()
	{
		constexpr char SQL[] = R"SQL(
			SELECT draft_idle_ms, split_policy, preview_lines,
			       backup_interval_hours, trash_retention_days
			FROM data_policy_settings WHERE id = 1
			)SQL";
		sqlite3_stmt* pStatement = nullptr;
		if (::sqlite3_prepare_v2(Database.Handle(), SQL, -1, &pStatement, nullptr) != SQLITE_OK) { return(false); }
		const int nStep = ::sqlite3_step(pStatement);
		const bool bValid = nStep == SQLITE_ROW && ::sqlite3_column_int64(pStatement, 0) >= 0 &&
			::sqlite3_column_text(pStatement, 1) != nullptr && ::sqlite3_column_int64(pStatement, 2) >= 0 &&
			::sqlite3_column_int64(pStatement, 3) >= 0 && ::sqlite3_column_int64(pStatement, 4) >= 0;
		::sqlite3_finalize(pStatement);
		return(bValid);
	}

	bool initialize_storage(const std::wstring& _sDatabasePath)
	{
		std::error_code Error;
		const std::filesystem::path Absolute = std::filesystem::absolute(_sDatabasePath, Error).lexically_normal();
		if (Error || Absolute.empty() || Absolute.parent_path().empty()) { return(false); }
		const std::string sDatabasePath = utf8(Absolute.native());
		const std::string sParentPath = utf8(Absolute.parent_path().native());
		if (sDatabasePath.empty() || sParentPath.empty() || !FileSystem.CreateDirectories(sParentPath)) { return(false); }

		const bool bHadDatabase = FileSystem.IsRegularFile(sDatabasePath) &&
			std::filesystem::file_size(Absolute, Error) > 0 && !Error;
		if (!Database.Open(sDatabasePath)) { return(false); }
		pynote::core::storage::C_MIGRATION_RUNNER Runner;
		Runner.SetExistingDatabase(bHadDatabase, sDatabasePath);
		std::wstring sBackupLocation;
		if (!Settings.GetString("backup/location", &sBackupLocation)) { return(false); }
		const auto nFirst = sBackupLocation.find_first_not_of(L" \t\r\n");
		const auto nLast = sBackupLocation.find_last_not_of(L" \t\r\n");
		std::optional<std::string> sBackupDirectory;
		if (nFirst != std::wstring::npos)
		{
			sBackupDirectory = utf8(sBackupLocation.substr(nFirst, nLast - nFirst + 1));
			if (sBackupDirectory->empty()) { return(false); }
		}
		MigrationBackup = std::make_unique<pynote::core::storage::C_MIGRATION_BACKUP_HOOK>(
			BackupService, sBackupDirectory, []() { return(now_us()); });
		Runner.SetBackupHook(std::ref(*MigrationBackup));
		if (Runner.Run(Database) != pynote::core::storage::E_MIGRATE_RESULT::Ok) { return(false); }
		if (!this->validate_policy()) { return(false); }
		// 정책 적재는 open/migration 과 validate_policy 성공 뒤다(원본 AppContext.open).
		// nullopt 는 원본이 기동을 닫는 자리이므로 여기서도 기동 실패다.
		DataPolicy = pynote::shell::LoadDataPolicy(Database);
		if (!DataPolicy) { return(false); }
		// 자동 유지보수의 백업 디렉터리 규칙은 마이그레이션 훅의 optional 규칙과 다르다
		// (원본 app.py:446~449 는 빈 설정을 DB 부모/backups 로 접는다). 위 optional 을
		// 재사용하면 "현재 디렉터리"를 뜻하는 인자와 구별이 사라진다.
		const std::string sMaintenanceBackupDirectory = sBackupDirectory
			? *sBackupDirectory
			: utf8((Absolute.parent_path() / L"backups").native());
		if (sMaintenanceBackupDirectory.empty()) { return(false); }
		BackupManager = std::make_unique<pynote::core::storage::C_AUTOMATIC_BACKUP_MANAGER>(
			BackupService, FileSystem, sDatabasePath, sMaintenanceBackupDirectory,
			DataPolicy->dBackupIntervalHours, []() { return(now_us()); });
		QuickCheck = std::make_unique<pynote::core::storage::C_PERIODIC_QUICK_CHECK>(
			Database.Handle(), DataPolicy->dBackupIntervalHours,
			[]() { return(now_monotonic_seconds()); });
		if (!BackupManager->IsValid() || !QuickCheck->IsValid()) { return(false); }
		Repositories = std::make_unique<pynote::core::storage::C_REPOSITORIES>(Database);
		CardService = std::make_unique<pynote::core::application::C_CARD_SERVICE>(
			Database, *Repositories, ParagraphParser, []() { return(now_us()); },
			[this]() { return(this->make_identity("card-data")); });
		DraftStore = std::make_unique<pynote::core::application::C_REPOSITORY_DRAFT_STORE>(
			Database, *Repositories);
		DraftCoordinator = std::make_unique<pynote::core::application::C_DRAFT_COORDINATOR>(
			*DraftStore, DataPolicy->nDraftIdleMs,
			[]() { return(now_us()); }, []() { return(now_us()); },
			[]() { return(now_us() * 1000); },
			[this]() { return(this->make_identity("draft")); });
		SaveCoordinator = std::make_unique<pynote::core::application::C_SAVE_COORDINATOR>(
			Database, *Repositories, *DraftCoordinator, []() { return(now_us()); },
			[this]() { return(this->make_identity("revision-event")); });
		return(true);
	}

	bool create_dispatcher()
	{
		WNDCLASSEXW Class{};
		Class.cbSize = sizeof(Class);
		Class.hInstance = hInstance;
		Class.lpfnWndProc = &S_STATE::DispatcherProcedure;
		Class.lpszClassName = DISPATCHER_CLASS;
		if (!::RegisterClassExW(&Class) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { return(false); }
		hDispatcher = ::CreateWindowExW(0, DISPATCHER_CLASS, L"", 0, 0, 0, 0, 0,
			HWND_MESSAGE, nullptr, hInstance, this);
		return(hDispatcher != nullptr);
	}

	static LRESULT CALLBACK DispatcherProcedure(HWND _hWnd, UINT _uMessage, WPARAM _wParam, LPARAM _lParam)
	{
		auto* pState = reinterpret_cast<S_STATE*>(::GetWindowLongPtrW(_hWnd, GWLP_USERDATA));
		if (_uMessage == WM_NCCREATE)
		{
			const auto* pCreate = reinterpret_cast<const CREATESTRUCTW*>(_lParam);
			pState = static_cast<S_STATE*>(pCreate->lpCreateParams);
			::SetWindowLongPtrW(_hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pState));
		}
		if (!pState) { return(::DefWindowProcW(_hWnd, _uMessage, _wParam, _lParam)); }
		if (_uMessage == WM_NOTEEX_NEW_WINDOW)
		{
			if (pState->bAcceptNewWindows.load()) { pState->pOwner->CreateMainWindow(); }
			return(0);
		}
		if (_uMessage == WM_NOTEEX_RETIRE_WINDOWS)
		{
			pState->retire_pending();
			return(0);
		}
		if (_uMessage == WM_NOTEEX_OPEN_FILE)
		{
			// 파이프 스레드가 heap 으로 넘긴 경로다 - 소유권을 여기서 회수한다.
			const std::unique_ptr<std::wstring> pPath(reinterpret_cast<std::wstring*>(_lParam));
			if (pPath && pState->bAcceptNewWindows.load()) { pState->open_path(*pPath); }
			return(0);
		}
		if (_uMessage == WM_TIMER && _wParam == MAINTENANCE_TIMER_ID)
		{
			pState->pOwner->RunAutomaticMaintenance();
			return(0);
		}
		return(::DefWindowProcW(_hWnd, _uMessage, _wParam, _lParam));
	}

	bool list_eligible_documents(
		std::vector<pynote::core::domain::S_DOCUMENT>* _pDocuments,
		std::set<std::string>* _pEligible)
	{
		if (Repositories->ListDocuments(_pDocuments) != pynote::core::storage::E_REPO_RESULT::Ok) { return(false); }
		_pEligible->clear();
		for (const auto& Document : *_pDocuments)
		{
			if (!Document.nArchivedAtUs && !Document.nTrashedAtUs) { _pEligible->insert(Document.sId); }
		}
		return(true);
	}

	// 소유 문서 집합이다. registry 항목과 살아 있는 창의 실제 문서를 합집합으로 본다 -
	// 외부 소멸 뒤 재채움한 창은 registry 항목이 죽은 문서를 계속 가리키므로 registry
	// 만 보면 이미 열린 문서를 후보로 다시 내놓아 중복 소유 불변식을 깬다.
	std::set<std::string> collect_owned_documents(
		std::optional<pynote::core::application::WINDOW_TOKEN> _Exclude) const
	{
		std::set<std::string> Owned;
		for (const auto& Window : Registry.Snapshot())
		{
			if (_Exclude && Window.Token == *_Exclude) { continue; }
			if (Window.sDocumentId) { Owned.insert(*Window.sDocumentId); }
		}
		for (const auto& [Token, Window] : Windows)
		{
			if (!Window || (_Exclude && Token == *_Exclude)) { continue; }
			const auto& sDocumentId = Window->DocumentId();
			if (sDocumentId) { Owned.insert(*sDocumentId); }
		}
		return(Owned);
	}

	std::optional<std::string> choose_document(
		const std::vector<pynote::core::domain::S_DOCUMENT>& _Documents,
		std::optional<pynote::core::application::WINDOW_TOKEN> _Exclude = std::nullopt)
	{
		const std::set<std::string> Owned = this->collect_owned_documents(_Exclude);
		std::vector<pynote::core::application::S_RESTORABLE_DOCUMENT> Candidates;
		for (const auto& Document : _Documents)
		{
			if (!Document.nArchivedAtUs && !Document.nTrashedAtUs)
			{
				Candidates.push_back({ Document.sId, Document.nUpdatedAtUs, Document.nCreatedAtUs });
			}
		}
		return(pynote::core::application::ChooseRecentUnownedDocument(Candidates, Owned));
	}

	bool create_document(std::string* _psDocumentId, std::string* _psTitle)
	{
		std::vector<pynote::core::domain::S_DOCUMENT> Documents;
		if (Repositories->ListDocuments(&Documents) != pynote::core::storage::E_REPO_RESULT::Ok) { return(false); }
		pynote::core::domain::S_DOCUMENT Document;
		Document.sId = this->make_identity("document");
		Document.sTitle = "Note " + std::to_string(Documents.size() + 1);
		Document.nCreatedAtUs = now_us();
		Document.nUpdatedAtUs = Document.nCreatedAtUs;
		if (Repositories->CreateDocument(Document) != pynote::core::storage::E_REPO_RESULT::Ok) { return(false); }
		*_psDocumentId = Document.sId;
		*_psTitle = Document.sTitle;
		// 발행 트리거 - 빈 DB 문서 생성 경로(PLAN-W3-0043).
		pOwner->PublishDocumentChange(Document.sId);
		return(true);
	}

	std::string document_title(const std::string& _sDocumentId)
	{
		pynote::core::domain::S_DOCUMENT Document;
		if (Repositories->GetDocument(_sDocumentId, &Document) != pynote::core::storage::E_REPO_RESULT::Ok)
		{
			return(_sDocumentId);
		}
		return(Document.sTitle);
	}

	bool create_native_window(
		const std::string& _sWorkspaceId, const std::string& _sDocumentId, bool _bDeleteRecordOnFailure,
		pynote::core::application::WINDOW_TOKEN* _pToken = nullptr)
	{
		const std::string sGeometryKey = pynote::shell::WindowGeometryKey(_sWorkspaceId);
		if (sGeometryKey.empty()) { return(false); }
		const bool bFirstGeometry = !bGeometryMigrationAttempted;
		if (bFirstGeometry)
		{
			bGeometryMigrationAttempted = true;
			std::vector<std::uint8_t> TargetBytes;
			std::vector<std::uint8_t> LegacyBytes;
			pynote::shell::S_WINDOW_GEOMETRY TargetGeometry;
			const bool bTargetExists = Settings.GetBytes(sGeometryKey, &TargetBytes);
			const bool bTargetValid = bTargetExists &&
				pynote::shell::DecodeWindowGeometry(TargetBytes, &TargetGeometry);
			if (bTargetExists && !bTargetValid && Settings.GetBytes("window/geometry", &LegacyBytes))
			{
				if (!Settings.Remove(sGeometryKey)) { return(false); }
			}
			if (!Settings.MigrateBytes("window/geometry", sGeometryKey)) { return(false); }
		}
		const auto Token = Registry.Register(_sWorkspaceId, _sDocumentId);
		if (Token == 0) { return(false); }
		auto Window = std::make_shared<C_MAIN>();
		Windows.emplace(Token, Window);
		// 창 제목은 seam 조립기 출력 축자다(CAP-FI-015). 빈 제목은 nullopt 로 넘겨야
		// 조립기의 "문서 없음 = pyNote" 계약이 성립한다(engaged 빈 문자열이면 " — pyNote").
		std::optional<std::wstring> sDocumentTitle;
		{
			std::wstring sRaw = wide(this->document_title(_sDocumentId));
			if (!sRaw.empty()) { sDocumentTitle = std::move(sRaw); }
		}
		const std::wstring sTitle = pynote::shell::ComposeWindowTitle(sDocumentTitle);
		if (Window->Init(
			hInstance, pOwner, Token, _sWorkspaceId, _sDocumentId, sTitle, bFirstGeometry))
		{
			if (_pToken) { *_pToken = Token; }
			return(true);
		}
		Registry.ReleaseOwnership(Token);
		Registry.Retire(Token);
		Windows.erase(Token);
		if (_bDeleteRecordOnFailure) { Repositories->DeleteWorkspaceWindow(_sWorkspaceId); }
		return(false);
	}

	// 원본 _protect_windows_quietly(app.py:652) - 앱 주도 창 전환(새 창 create_window:560, 소유 창 활성화
	// :578/594/621) 전에 생존 창 전부의 최신 초안 보호를 비차단으로 시도한다. 실패는 기록만 하고
	// 전환을 막지 않는다(CAP-FI-016 "살아 있는 초안을 보호한 뒤 창을 하나 더 만든다").
	void protect_windows_quietly()
	{
		for (const auto& Entry : Windows)
		{
			if (Entry.second && !Entry.second->Protect())
			{
				DBGPRINT(L"앱 주도 전환 전 창의 recovery draft 보호에 실패했습니다");
			}
		}
	}

	bool create_new_window(
		std::string* _psDocumentId = nullptr,
		pynote::core::application::WINDOW_TOKEN* _pToken = nullptr)
	{
		if (!bAcceptNewWindows.load() || !Lifecycle.AcceptsNewWindows()) { return(false); }
		this->protect_windows_quietly();
		std::vector<pynote::core::domain::S_DOCUMENT> Documents;
		std::set<std::string> Eligible;
		if (!this->list_eligible_documents(&Documents, &Eligible)) { return(false); }
		std::optional<std::string> DocumentId = this->choose_document(Documents);
		std::string sCreatedTitle;
		if (!DocumentId)
		{
			std::string sCreatedId;
			if (!this->create_document(&sCreatedId, &sCreatedTitle)) { return(false); }
			DocumentId = std::move(sCreatedId);
		}
		const std::string sWorkspaceId = this->make_identity("window");
		pynote::core::domain::S_WORKSPACE_WINDOW Saved;
		if (Repositories->SaveWorkspaceWindow(sWorkspaceId, { *DocumentId }, DocumentId, &Saved) !=
			pynote::core::storage::E_REPO_RESULT::Ok)
		{
			return(false);
		}
		if (!this->create_native_window(sWorkspaceId, *DocumentId, true, _pToken)) { return(false); }
		if (_psDocumentId) { *_psDocumentId = *DocumentId; }
		return(true);
	}

	bool restore_windows()
	{
		std::vector<pynote::core::domain::S_DOCUMENT> Documents;
		std::set<std::string> Eligible;
		if (!this->list_eligible_documents(&Documents, &Eligible)) { return(false); }
		std::vector<pynote::core::domain::S_WORKSPACE_WINDOW> Records;
		if (Repositories->ListWorkspaceWindows(&Records) != pynote::core::storage::E_REPO_RESULT::Ok) { return(false); }
		if (Records.empty()) { return(this->create_new_window()); }

		const auto Plan = pynote::core::application::BuildWorkspaceRestorePlan(Records, Eligible);
		for (const auto& Entry : Plan)
		{
			std::optional<std::string> DocumentId = Entry.Workspace.sActiveDocumentId;
			if (!DocumentId) { DocumentId = this->choose_document(Documents); }
			if (!DocumentId)
			{
				std::string sTitle;
				std::string sCreatedId;
				if (!this->create_document(&sCreatedId, &sTitle)) { return(false); }
				DocumentId = std::move(sCreatedId);
				pynote::core::domain::S_DOCUMENT Created;
				if (Repositories->GetDocument(*DocumentId, &Created) == pynote::core::storage::E_REPO_RESULT::Ok)
				{
					Documents.push_back(std::move(Created));
				}
			}
			if (Entry.bNeedsRewrite || Entry.Workspace.sActiveDocumentId != DocumentId)
			{
				pynote::core::domain::S_WORKSPACE_WINDOW Saved;
				if (Repositories->SaveWorkspaceWindow(Entry.Workspace.sWindowId, { *DocumentId }, DocumentId, &Saved) !=
					pynote::core::storage::E_REPO_RESULT::Ok)
				{
					return(false);
				}
			}
			if (!this->create_native_window(Entry.Workspace.sWindowId, *DocumentId, false)) { return(false); }
		}
		return(!Windows.empty());
	}

	pynote::core::application::S_WINDOW_LIFECYCLE_PARTICIPANT participant(
		pynote::core::application::WINDOW_TOKEN _Token)
	{
		return {
			_Token,
			[this, _Token]() {
				const auto it = Windows.find(_Token);
				return(it != Windows.end() && it->second->Protect());
			},
			[this, _Token]() {
				const auto it = Windows.find(_Token);
				return(it == Windows.end() ? pynote::core::application::E_LEAVE_RESULT::Denied : it->second->RequestLeave());
			},
			[this, _Token]() {
				const auto it = Windows.find(_Token);
				return(it != Windows.end() && it->second->PersistState());
			},
			[this, _Token]() {
				const auto it = Windows.find(_Token);
				return(it == Windows.end() || it->second->Cleanup());
			},
			[this, _Token]() {
				for (const auto& Window : Registry.Snapshot())
				{
					if (Window.Token == _Token)
					{
						const std::string sGeometryKey = pynote::shell::WindowGeometryKey(Window.sWorkspaceId);
						if (Repositories->DeleteWorkspaceWindow(Window.sWorkspaceId) !=
							pynote::core::storage::E_REPO_RESULT::Ok)
						{
							DBGPRINT(L"복원 행 삭제 실패");
						}
						if (sGeometryKey.empty() || !Settings.Remove(sGeometryKey) || !Settings.Sync())
						{
							DBGPRINT(L"창 geometry 삭제 실패");
						}
						break;
					}
				}
			},
			[this, _Token]() { Registry.ReleaseOwnership(_Token); },
			[this, _Token]() {
				const auto it = Windows.find(_Token);
				if (it != Windows.end()) { it->second->DestroyNative(); }
			},
		};
	}

	// 원본 _refresh_document_mapping(app.py:850~857) 의 하드 불변식이다. 한 문서가
	// 두 창에 열려 있으면 조용히 무시하지 않고 실패로 돌려준다.
	bool refresh_document_mapping()
	{
		DocumentWindows.clear();
		bool bOk = true;
		for (const auto& Window : Registry.Snapshot())
		{
			const auto it = Windows.find(Window.Token);
			if (it == Windows.end() || !it->second) { continue; }
			const auto& sDocumentId = it->second->DocumentId();
			if (!sDocumentId) { continue; }
			if (!DocumentWindows.emplace(*sDocumentId, Window.Token).second)
			{
				DBGPRINT(L"문서가 둘 이상의 창에 열렸습니다");
				bOk = false;
			}
		}
		return(bOk);
	}

	// 원본 _activate_window(app.py:1082~1089) 의 4 단이다.
	bool activate_window(pynote::core::application::WINDOW_TOKEN _Token)
	{
		const auto it = Windows.find(_Token);
		if (it == Windows.end() || !it->second || !::IsWindow(it->second->m_hWnd)) { return(false); }
		this->protect_windows_quietly();
		const HWND hWindow = it->second->m_hWnd;
		if (::IsIconic(hWindow)) { ::ShowWindow(hWindow, SW_RESTORE); }
		else { ::ShowWindow(hWindow, SW_SHOW); }
		::BringWindowToTop(hWindow);
		// 전경 전환은 OS 규칙상 거절될 수 있다 - 시도하되 활성 토큰 등록이 관측 지점이다.
		::SetForegroundWindow(hWindow);
		return(Registry.Activate(_Token));
	}

	// 원본 _report_path_open_failure 의 부모 규칙(app.py:709) - 첫 번째 살아 있는 창이고
	// 없으면 무부모다.
	HWND dialog_parent()
	{
		for (const auto& Window : Registry.Snapshot())
		{
			const auto it = Windows.find(Window.Token);
			if (it != Windows.end() && it->second && ::IsWindow(it->second->m_hWnd))
			{
				return(it->second->m_hWnd);
			}
		}
		return(nullptr);
	}

	// 원본 _binding_owner_window + _window_for_request + _open_bound_card(app.py:686~700,
	// ui/main_window.py:1243~1253) 의 포팅 가능한 부분이다 - 목록 선택까지이고 편집기 열기는 W6 다.
	bool route_bound_card(const std::string& _sCardId)
	{
		pynote::core::domain::S_CARD Card;
		if (Repositories->GetCard(_sCardId, &Card) != pynote::core::storage::E_REPO_RESULT::Ok)
		{
			return(false);
		}
		if (Card.nDeletedAtUs) { return(false); }
		if (!this->refresh_document_mapping()) { return(false); }
		pynote::core::application::WINDOW_TOKEN Requesting = 0;
		const auto itOwner = DocumentWindows.find(Card.sDocumentId);
		if (itOwner != DocumentWindows.end()) { Requesting = itOwner->second; }
		else
		{
			const auto Snapshot = Registry.Snapshot();
			if (!Snapshot.empty()) { Requesting = Snapshot.back().Token; }
		}
		// CEILING: 창이 0개면 원본은 create_window 로 요청 창을 만든다(app.py:700) - 포팅본은
		// restore_windows 가 최소 1창을 보장한다는 전제에 기대 실패로 접는다(P2 감사 1-5).
		if (Requesting == 0) { return(false); }
		// CEILING: 결속 카드의 문서를 연 창이 없으면 제자리 문서 전환이 필요하고 그것은 W7
		// 소유다(OpenDocument 가 false 로 돌린다) - 그때는 창을 만들지 않고 실패로 접는다.
		if (!pOwner->OpenDocument(Requesting, Card.sDocumentId)) { return(false); }
		if (!this->refresh_document_mapping()) { return(false); }
		const auto itSettled = DocumentWindows.find(Card.sDocumentId);
		if (itSettled == DocumentWindows.end()) { return(false); }
		const auto it = Windows.find(itSettled->second);
		if (it == Windows.end() || !it->second) { return(false); }
		if (!it->second->DocumentPage().RevealCard(_sCardId)) { return(false); }
		return(this->activate_window(itSettled->second));
	}

	// 기동 인자·파이프 수신 경로의 결선이다. 판정은 CApplication.h 의 ResolveOpenPath 가 갖는다.
	bool open_path(const std::wstring& _sPath)
	{
		if (!Repositories || !CardService) { return(false); }
		pynote::core::application::WINDOW_TOKEN CreatedToken = 0;
		S_OPEN_PATH_SHELL Shell;
		Shell.ShowDialog = [this](const std::wstring& _sTitle, const std::wstring& _sText)
		{
			if (DialogSink) { DialogSink(_sTitle, _sText); }
		};
		Shell.RouteBoundCard = [this](const std::string& _sCardId)
		{
			return(this->route_bound_card(_sCardId));
		};
		Shell.CreateWindowForFile = [this, &CreatedToken](std::string* _psDocumentId)
		{
			return(this->create_new_window(_psDocumentId, &CreatedToken));
		};
		Shell.RevealAndActivate = [this, &CreatedToken](const std::string& _sCardId)
		{
			const auto it = Windows.find(CreatedToken);
			if (it == Windows.end() || !it->second) { return(false); }
			// 창이 선 뒤에 만든 카드라 목록을 다시 읽어야 행이 생긴다(원본 add_cards 자리).
			if (!it->second->DocumentPage().Refresh()) { return(false); }
			if (!it->second->DocumentPage().RevealCard(_sCardId)) { return(false); }
			return(this->activate_window(CreatedToken));
		};
		Shell.Now = []() { return(now_us()); };
		return(ResolveOpenPath(
			_sPath, *Repositories, *CardService, ParagraphParser, BindingFileSystem, Shell));
	}

	// 원본 _prepare_window_for_input(app.py:860~) - 열 수 있는 문서가 없으면 만든다.
	std::optional<std::string> choose_refill_document(
		pynote::core::application::WINDOW_TOKEN _Token)
	{
		std::vector<pynote::core::domain::S_DOCUMENT> Documents;
		if (Repositories->ListDocuments(&Documents) != pynote::core::storage::E_REPO_RESULT::Ok)
		{
			return(std::nullopt);
		}
		if (auto Chosen = this->choose_document(Documents, _Token)) { return(Chosen); }
		std::string sCreatedId;
		std::string sCreatedTitle;
		if (!this->create_document(&sCreatedId, &sCreatedTitle)) { return(std::nullopt); }
		return(sCreatedId);
	}

	// 원본 _report_maintenance_failure(app.py:979~982). 부모는 첫 번째 살아 있는 창이고
	// 없으면 무부모다. 실패해도 프로세스는 계속 산다.
	void report_maintenance_failure(const std::string& _sError)
	{
		DBGPRINT(L"자동 유지보수 실패를 사용자에게 알립니다");
		HWND hParent = nullptr;
		for (const auto& Window : Registry.Snapshot())
		{
			const auto it = Windows.find(Window.Token);
			if (it != Windows.end() && it->second && ::IsWindow(it->second->m_hWnd))
			{
				hParent = it->second->m_hWnd;
				break;
			}
		}
		const std::wstring sMessage =
			L"자동 백업 전 DB 무결성 검사 또는 백업에 실패했습니다: " + wide(_sError);
		::MessageBoxW(hParent, sMessage.c_str(), L"자동 백업 실패",
			MB_OK | MB_ICONERROR | MB_APPLMODAL);
	}

	void stop_maintenance_timer()
	{
		if (!bMaintenanceTimerActive) { return; }
		if (::IsWindow(hDispatcher)) { ::KillTimer(hDispatcher, MAINTENANCE_TIMER_ID); }
		bMaintenanceTimerActive = false;
	}

	void finalize_for_loop_exit()
	{
		if (bRuntimeFinalized) { return; }
		this->stop_maintenance_timer();
		bAcceptNewWindows.store(false);
		Instance.SetNewWindowHandler({});
		Instance.SetOpenFileHandler({});
		BrushCache.Shutdown();
		TextEngine.Shutdown();
		D2DDevice.Shutdown();
		bD2DInitialized = false;
		SearchDialog.Destroy();
		SaveCoordinator.reset();
		DraftCoordinator.reset();
		DraftStore.reset();
		CardService.reset();
		QuickCheck.reset();
		BackupManager.reset();
		Repositories.reset();
		Database.Close();
		Instance.Close();
		bRuntimeFinalized = true;
		::PostQuitMessage(0);
	}

	void retire_pending()
	{
		const auto Pending = std::exchange(PendingRetirements, {});
		for (const auto Token : Pending)
		{
			if (!Registry.Retire(Token)) { continue; }
			Windows.erase(Token);
		}
		if (Registry.Size() == 0) { this->finalize_for_loop_exit(); }
	}
};

CApplication::CApplication()
	: m_pState(std::make_unique<S_STATE>())
{
	m_pState->pOwner = this;
	m_pState->DialogSink = [State = m_pState.get()](
		const std::wstring& _sTitle, const std::wstring& _sText)
	{
		::MessageBoxW(State->dialog_parent(), _sText.c_str(), _sTitle.c_str(),
			MB_OK | MB_ICONWARNING | MB_APPLMODAL);
	};
}

CApplication::~CApplication()
{
	this->Shutdown();
}

CApplication::E_INITIALIZE_RESULT CApplication::Initialize(
	HINSTANCE _hInstance, const pynote::platform::S_WIN32_STARTUP_OPTIONS& _Options)
{
	m_pState->hInstance = _hInstance;
	const auto eAcquire = m_pState->Instance.Acquire(_Options.sDatabasePath, _Options.Paths);
	if (eAcquire == pynote::platform::C_WIN32_SINGLE_INSTANCE::E_ACQUIRE_RESULT::SecondaryNotified)
	{
		return(E_INITIALIZE_RESULT::SecondaryNotified);
	}
	if (eAcquire != pynote::platform::C_WIN32_SINGLE_INSTANCE::E_ACQUIRE_RESULT::Primary)
	{
		return(E_INITIALIZE_RESULT::Failure);
	}
	if (!m_pState->Settings.Initialize() || !m_pState->initialize_storage(_Options.sDatabasePath))
	{
		this->Shutdown();
		return(E_INITIALIZE_RESULT::Failure);
	}
	if (!g_pLog) { g_pLog = new dk::C_LOG(L"log-NoteEx"); }
	m_pState->Module.Init(nullptr, _hInstance);
	m_pState->Module.AddMessageLoop(&m_pState->MessageLoop);
	m_pState->bModuleInitialized = true;
	const auto Accelerators = C_MAIN::RuntimeAccelerators();
	m_pState->hAccelerator = ::CreateAcceleratorTableW(
		const_cast<LPACCEL>(Accelerators.data()), static_cast<int>(Accelerators.size()));
	if (!m_pState->hAccelerator)
	{
		this->Shutdown();
		return(E_INITIALIZE_RESULT::Failure);
	}
	m_pState->MessageLoop.AddMessageFilter(m_pState.get());
	m_pState->bMessageFilterRegistered = true;
	if (!m_pState->D2DDevice.Initialize())
	{
		this->Shutdown();
		return(E_INITIALIZE_RESULT::Failure);
	}
	m_pState->BrushCache.Initialize(&m_pState->D2DDevice);
	m_pState->TextEngine.Initialize(&m_pState->D2DDevice);
	m_pState->bD2DInitialized = true;
	if (!m_pState->create_dispatcher())
	{
		this->Shutdown();
		return(E_INITIALIZE_RESULT::Failure);
	}
	m_pState->bAcceptNewWindows.store(true);
	// 순수 seam 은 전역 로거에 닿지 못한다 - 구독자 실패를 남길 자리를 여기서 준다.
	m_pState->ChangeBus.SetErrorSink(
		[](const std::string&) { DBGPRINT(L"문서 변경 구독자가 실패했습니다"); });
	if (!m_pState->restore_windows())
	{
		this->Shutdown();
		return(E_INITIALIZE_RESULT::Failure);
	}
	if (!m_pState->refresh_document_mapping())
	{
		this->Shutdown();
		return(E_INITIALIZE_RESULT::Failure);
	}
	// 복원 창을 먼저 세운 뒤 기동 경로를 연다 - 작업 공간을 대체하지 않는다(원본 app.py:1229~1231).
	for (const std::wstring& sPath : _Options.Paths) { m_pState->open_path(sPath); }
	this->StartAutomaticMaintenance();
	m_pState->Instance.SetNewWindowHandler([State = m_pState.get()]() {
		if (State->bAcceptNewWindows.load() && ::IsWindow(State->hDispatcher))
		{
			::PostMessageW(State->hDispatcher, WM_NOTEEX_NEW_WINDOW, 0, 0);
		}
	});
	m_pState->Instance.SetOpenFileHandler([State = m_pState.get()](std::wstring _sPath) {
		if (!State->bAcceptNewWindows.load() || !::IsWindow(State->hDispatcher)) { return; }
		auto pPath = std::make_unique<std::wstring>(std::move(_sPath));
		if (::PostMessageW(State->hDispatcher, WM_NOTEEX_OPEN_FILE, 0,
			reinterpret_cast<LPARAM>(pPath.get())) != FALSE)
		{
			// 디스패처가 소유권을 회수한다.
			static_cast<void>(pPath.release());
		}
	});
	return(E_INITIALIZE_RESULT::Primary);
}

int CApplication::Run()
{
	return(m_pState->bModuleInitialized ? m_pState->MessageLoop.Run() : 1);
}

void CApplication::Shutdown()
{
	if (!m_pState) { return; }
	m_pState->stop_maintenance_timer();
	m_pState->bAcceptNewWindows.store(false);
	m_pState->Instance.SetNewWindowHandler({});
	m_pState->Instance.SetOpenFileHandler({});
	for (auto& [Token, Window] : m_pState->Windows)
	{
		Window->Cleanup();
		Window->DestroyNative();
		m_pState->Registry.ReleaseOwnership(Token);
	}
	m_pState->Windows.clear();
	if (m_pState->hDispatcher)
	{
		::DestroyWindow(m_pState->hDispatcher);
		m_pState->hDispatcher = nullptr;
	}
	m_pState->SearchDialog.Destroy();
	if (m_pState->bD2DInitialized)
	{
		m_pState->BrushCache.Shutdown();
		m_pState->TextEngine.Shutdown();
		m_pState->D2DDevice.Shutdown();
		m_pState->bD2DInitialized = false;
	}
	m_pState->SaveCoordinator.reset();
	m_pState->DraftCoordinator.reset();
	m_pState->DraftStore.reset();
	m_pState->CardService.reset();
	m_pState->QuickCheck.reset();
	m_pState->BackupManager.reset();
	m_pState->Repositories.reset();
	m_pState->Database.Close();
	m_pState->Instance.Close();
	if (m_pState->bModuleInitialized)
	{
		if (m_pState->bMessageFilterRegistered)
		{
			m_pState->MessageLoop.RemoveMessageFilter(m_pState.get());
			m_pState->bMessageFilterRegistered = false;
		}
		m_pState->Module.RemoveMessageLoop();
		m_pState->Module.Term();
		m_pState->bModuleInitialized = false;
	}
	if (m_pState->hAccelerator)
	{
		::DestroyAcceleratorTable(m_pState->hAccelerator);
		m_pState->hAccelerator = nullptr;
	}
	if (g_pConfig) { delete g_pConfig; g_pConfig = nullptr; }
}

bool CApplication::CreateMainWindow()
{
	return(m_pState->create_new_window());
}

void CApplication::RequestCloseWindow(pynote::core::application::WINDOW_TOKEN _Token)
{
	if (!m_pState->Registry.Contains(_Token)) { return; }
	const bool bLast = m_pState->Registry.Snapshot().size() == 1;
	if (bLast) { m_pState->bAcceptNewWindows.store(false); }
	if (!m_pState->Lifecycle.CloseWindow(m_pState->participant(_Token), bLast) &&
		m_pState->Lifecycle.AcceptsNewWindows())
	{
		m_pState->bAcceptNewWindows.store(true);
	}
}

void CApplication::RequestApplicationQuit()
{
	std::vector<pynote::core::application::S_WINDOW_LIFECYCLE_PARTICIPANT> Participants;
	for (const auto& Window : m_pState->Registry.Snapshot())
	{
		Participants.push_back(m_pState->participant(Window.Token));
	}
	if (Participants.empty()) { return; }
	m_pState->bAcceptNewWindows.store(false);
	if (!m_pState->Lifecycle.QuitApplication(Participants))
	{
		m_pState->bAcceptNewWindows.store(true);
	}
}

void CApplication::NotifyWindowActivated(pynote::core::application::WINDOW_TOKEN _Token)
{
	m_pState->Registry.Activate(_Token);
}

void CApplication::NotifyWindowNcDestroy(pynote::core::application::WINDOW_TOKEN _Token)
{
	if (!m_pState->Registry.Contains(_Token)) { return; }
	m_pState->Registry.ReleaseOwnership(_Token);
	if (std::find(m_pState->PendingRetirements.begin(), m_pState->PendingRetirements.end(), _Token) ==
		m_pState->PendingRetirements.end())
	{
		m_pState->PendingRetirements.push_back(_Token);
	}
	if (::IsWindow(m_pState->hDispatcher))
	{
		::PostMessageW(m_pState->hDispatcher, WM_NOTEEX_RETIRE_WINDOWS, 0, 0);
	}
}

pynote::shell::C_DOCUMENT_CHANGE_BUS& CApplication::ChangeBus()
{
	return(m_pState->ChangeBus);
}

bool CApplication::PublishDocumentChange(const std::string& _sDocumentId)
{
	if (_sDocumentId.empty()) { return(false); }
	// 원본 publish 2 단(app.py:672~675) - 동기 전파 뒤 소유 매핑 재계산이다.
	m_pState->ChangeBus.Publish(_sDocumentId);
	return(m_pState->refresh_document_mapping());
}

bool CApplication::OpenDocument(
	pynote::core::application::WINDOW_TOKEN _Requesting, const std::string& _sDocumentId)
{
	if (_sDocumentId.empty() || !m_pState->Registry.Contains(_Requesting)) { return(false); }
	if (!m_pState->refresh_document_mapping()) { return(false); }
	std::optional<pynote::core::application::WINDOW_TOKEN> Owner;
	const auto it = m_pState->DocumentWindows.find(_sDocumentId);
	if (it != m_pState->DocumentWindows.end()) { Owner = it->second; }
	if (pynote::shell::ResolveOpenDocumentTarget(Owner, _Requesting) ==
		pynote::shell::E_OPEN_DOCUMENT_TARGET::ActivateOwner)
	{
		return(m_pState->activate_window(*Owner));
	}
	// 요청 창이 이미 그 문서를 들고 있으면 그 창을 앞으로 가져오는 것이 전부다.
	// 다른 문서로의 제자리 전환은 문서 관리 UI(W7) 소유라 여기서 발명하지 않는다.
	if (Owner && *Owner == _Requesting) { return(m_pState->activate_window(_Requesting)); }
	DBGPRINT(L"요청 창의 제자리 문서 전환은 W7 소유입니다");
	return(false);
}

std::optional<std::string> CApplication::DocumentTitle(const std::string& _sDocumentId)
{
	if (_sDocumentId.empty() || !m_pState->Repositories) { return(std::nullopt); }
	pynote::core::domain::S_DOCUMENT Document;
	if (m_pState->Repositories->GetDocument(_sDocumentId, &Document) !=
		pynote::core::storage::E_REPO_RESULT::Ok)
	{
		return(std::nullopt);
	}
	return(Document.sTitle);
}

std::optional<std::string> CApplication::ChooseRefillDocument(
	pynote::core::application::WINDOW_TOKEN _Token)
{
	if (!m_pState->Repositories) { return(std::nullopt); }
	return(m_pState->choose_refill_document(_Token));
}

void CApplication::StartAutomaticMaintenance()
{
	// 원본 start_automatic_maintenance(app.py:470~475) - 이미 활성인 타이머면
	// 즉시 실행 없이 반환한다(멱등).
	if (m_pState->bMaintenanceTimerActive) { return; }
	if (!m_pState->DataPolicy || !::IsWindow(m_pState->hDispatcher)) { return; }
	this->RunAutomaticMaintenance();
	const std::int64_t nIntervalMs = pynote::shell::ClampMaintenanceIntervalMs(
		m_pState->DataPolicy->dBackupIntervalHours);
	if (nIntervalMs <= 0) { return; }
	if (::SetTimer(m_pState->hDispatcher, MAINTENANCE_TIMER_ID,
		static_cast<UINT>(nIntervalMs), nullptr) == 0)
	{
		DBGPRINT(L"자동 유지보수 타이머 등록에 실패했습니다");
		return;
	}
	m_pState->bMaintenanceTimerActive = true;
}

bool CApplication::RunAutomaticMaintenance()
{
	if (!m_pState->QuickCheck || !m_pState->BackupManager) { return(false); }
	// 원본 run_automatic_maintenance(app.py:477~487) - quick_check(force) 가 성공해야
	// 백업이 돈다. 실패는 모달로 알리되 프로세스는 계속 산다.
	std::string sError;
	if (m_pState->QuickCheck->RunIfDue(true) == pynote::core::storage::E_QUICK_CHECK_RESULT::Failed)
	{
		sError = m_pState->QuickCheck->LastError();
		if (sError.empty()) { sError = "quick_check failed"; }
	}
	else
	{
		bool bCreated = false;
		std::string sDestination;
		if (m_pState->BackupManager->RunIfDue(false, &bCreated, &sDestination) !=
			pynote::core::storage::E_BACKUP_RESULT::Ok)
		{
			sError = m_pState->BackupManager->LastError();
			if (sError.empty()) { sError = "automatic backup failed"; }
		}
	}
	if (sError.empty()) { return(true); }
	m_pState->report_maintenance_failure(sError);
	return(false);
}

bool CApplication::PersistWindowState(
	pynote::core::application::WINDOW_TOKEN _Token,
	const std::string& _sWorkspaceId, const std::optional<std::string>& _sDocumentId)
{
	if (!m_pState->Registry.Contains(_Token) || !_sDocumentId || !m_pState->Repositories) { return(false); }
	pynote::core::domain::S_WORKSPACE_WINDOW Saved;
	if (m_pState->Repositories->SaveWorkspaceWindow(
		_sWorkspaceId, { *_sDocumentId }, _sDocumentId, &Saved) !=
		pynote::core::storage::E_REPO_RESULT::Ok)
	{
		return(false);
	}
	return(true);
}

bool CApplication::LoadDocumentSplit(
	const std::string& _sWorkspaceId, const std::string& _sDocumentId,
	std::optional<std::pair<int, int>>* _pSplitSizesDip)
{
	if (!_pSplitSizesDip || !m_pState->Repositories) { return(false); }
	pynote::core::application::C_WORKSPACE_STATE_STORE Store(
		m_pState->Database, *m_pState->Repositories, _sWorkspaceId);
	pynote::core::application::S_DOCUMENT_UI_STATE State;
	if (Store.LoadDocumentUiState(_sDocumentId, &State) != pynote::core::storage::E_REPO_RESULT::Ok)
	{
		return(false);
	}
	_pSplitSizesDip->reset();
	if (State.EditorSplitSizes)
	{
		const std::int64_t nLeft = State.EditorSplitSizes->first;
		const std::int64_t nRight = State.EditorSplitSizes->second;
		if (nLeft <= 0 || nRight <= 0 ||
			nLeft > static_cast<std::int64_t>((std::numeric_limits<int>::max)()) ||
			nRight > static_cast<std::int64_t>((std::numeric_limits<int>::max)()))
		{
			return(false);
		}
		*_pSplitSizesDip = std::pair<int, int>{
			static_cast<int>(nLeft), static_cast<int>(nRight) };
	}
	return(true);
}

pynote::platform::C_WIN32_DEVICE_SETTINGS& CApplication::Settings() { return(m_pState->Settings); }
pynote::core::storage::C_DATABASE& CApplication::Database() { return(m_pState->Database); }
pynote::core::storage::C_REPOSITORIES& CApplication::Repositories() { return(*m_pState->Repositories); }
pynote::core::application::C_CARD_SERVICE& CApplication::CardService() { return(*m_pState->CardService); }
pynote::core::application::C_DRAFT_COORDINATOR& CApplication::DraftCoordinator()
{
	return(*m_pState->DraftCoordinator);
}
pynote::core::application::C_SAVE_COORDINATOR& CApplication::SaveCoordinator()
{
	return(*m_pState->SaveCoordinator);
}
HACCEL CApplication::Accelerator() const noexcept { return(m_pState->hAccelerator); }
void CApplication::ShowSearchDialog(HWND _hOwner)
{
	if (!m_pState->SearchDialog.Hwnd()) { m_pState->SearchDialog.Initialize(m_pState->hInstance); }
	m_pState->SearchDialog.Show(_hOwner);
}
C_SEARCH_DIALOG& CApplication::SearchDialog() { return(m_pState->SearchDialog); }
d2d::C_D2D_DEVICE& CApplication::D2DDevice() { return(m_pState->D2DDevice); }
d2d::C_D2D_BRUSH_CACHE& CApplication::BrushCache() { return(m_pState->BrushCache); }
d2d::C_D2D_TEXT& CApplication::TextEngine() { return(m_pState->TextEngine); }
