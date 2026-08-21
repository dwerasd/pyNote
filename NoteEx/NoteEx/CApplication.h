#pragma once

#include <windows.h>

#include "pynote/core/application/window_lifecycle.h"
#include "pynote/platform/win32_single_instance.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

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
