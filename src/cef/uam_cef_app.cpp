#include "cef/uam_cef_app.h"
#include "cef/uam_cef_client.h"

#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_fill_layout.h"
#include "include/views/cef_window.h"

#include <filesystem>

namespace
{

class UamRootWindowDelegate : public CefWindowDelegate,
                              public CefBrowserViewDelegate
{
  public:
	explicit UamRootWindowDelegate(const CefRect& initial_bounds)
		: m_initialBounds(initial_bounds)
	{
	}

	void SetBrowserView(CefRefPtr<CefBrowserView> browser_view)
	{
		m_browserView = browser_view;
	}

	void OnWindowCreated(CefRefPtr<CefWindow> window) override
	{
		CEF_REQUIRE_UI_THREAD();

		window->SetTitle("Universal Agent Manager");
		window->SetToFillLayout();

		if (m_browserView)
			window->AddChildView(m_browserView);

		window->Show();
	}

	void OnWindowDestroyed(CefRefPtr<CefWindow> /*window*/) override
	{
		CEF_REQUIRE_UI_THREAD();
		m_browserView = nullptr;
	}

	CefRect GetInitialBounds(CefRefPtr<CefWindow> /*window*/) override
	{
		return m_initialBounds;
	}

	bool CanClose(CefRefPtr<CefWindow> /*window*/) override
	{
		CEF_REQUIRE_UI_THREAD();

		if (!m_browserView)
			return true;

		CefRefPtr<CefBrowser> browser = m_browserView->GetBrowser();
		if (!browser)
			return true;

		return browser->GetHost()->TryCloseBrowser();
	}

	cef_runtime_style_t GetWindowRuntimeStyle() override
	{
		return CEF_RUNTIME_STYLE_ALLOY;
	}

	cef_runtime_style_t GetBrowserRuntimeStyle() override
	{
		return CEF_RUNTIME_STYLE_ALLOY;
	}

  private:
	CefRect                    m_initialBounds;
	CefRefPtr<CefBrowserView>  m_browserView;

	IMPLEMENT_REFCOUNTING(UamRootWindowDelegate);
};

} // namespace

// Forward declaration — Application owns AppState and provides access to the browser ref.
// We use a global pointer set by Application::InitializeCef().
// This keeps the CEF layer decoupled from Application's header.
namespace uam_cef_globals
{
	extern uam::AppState* g_app_state;
	extern CefRefPtr<UamCefClient> g_client;
} // namespace uam_cef_globals

void UamCefApp::OnBeforeCommandLineProcessing(const CefString& /*process_type*/,
                                              CefRefPtr<CefCommandLine> command_line)
{
	// Allow file:// pages to make XHR/fetch requests to other file:// URLs.
	// Required because the React UI is served from file:// in production.
	command_line->AppendSwitch("allow-file-access-from-files");
	command_line->AppendSwitch("disable-web-security");

	// Disable Chromium features that trigger an EXC_BREAKPOINT / SIGTRAP crash
	// on macOS 26.x (beta).  The crash manifests as NSApplication receiving a
	// message with the selector "%s" (an unsubstituted format-string placeholder)
	// from ChromeWebAppShortcutCopierMain — a Chromium internal worker that
	// handles OS-level web-app shortcut creation.  UAM does not use web-app
	// shortcuts, so these features can be disabled without functional loss.
	command_line->AppendSwitchWithValue("disable-features",
	    "WebAppEnableShortcuts,"
	    "DesktopPWADeterminedInstalledByOsIntegration,"
	    "WebAppSystemMediaControlsWin");

	// Skip first-run tasks and background-mode processes that may exercise
	// other macOS APIs removed or renamed in the beta OS.
	command_line->AppendSwitch("no-first-run");
	command_line->AppendSwitch("disable-background-mode");
}

void UamCefApp::OnContextInitialized()
{
	CEF_REQUIRE_UI_THREAD();

	// Reuse the pre-constructed client if Application already set it up
	// (e.g. with a BrowserReadyCallback).  Fall back to a fresh client if not.
	uam::AppState* app_state = uam_cef_globals::g_app_state;
	CefRefPtr<UamCefClient> client = uam_cef_globals::g_client;
	if (!client)
	{
		client = new UamCefClient(*app_state);
		uam_cef_globals::g_client = client;
	}

	CefBrowserSettings browser_settings;
	browser_settings.javascript = STATE_ENABLED;
	browser_settings.local_storage = STATE_ENABLED;

	// Resolve the path to the React bundle.
	// Search order:
	//  1. Contents/Resources/UI-V2/dist/  (macOS bundle, correct location)
	//  2. Contents/MacOS/UI-V2/dist/      (macOS bundle, legacy / flat fallback)
	//  3. <exe_dir>/UI-V2/dist/           (Windows flat / dev build)
	//  4. cwd-relative path               (last resort)
	std::string url;

	CefString exe_dir_str;
	if (CefGetPath(PK_DIR_EXE, exe_dir_str))
	{
		const std::filesystem::path exe_dir(exe_dir_str.ToString());

		// On macOS bundle the exe is in Contents/MacOS/; resources are in Contents/Resources/.
		const auto resources_dist = exe_dir / ".." / "Resources" / "UI-V2" / "dist" / "index.html";
		if (std::filesystem::exists(resources_dist))
		{
			url = "file://" + std::filesystem::canonical(resources_dist).string();
		}

		if (url.empty())
		{
			const auto macos_dist = exe_dir / "UI-V2" / "dist" / "index.html";
			if (std::filesystem::exists(macos_dist))
				url = "file://" + macos_dist.string();
		}
	}

	if (url.empty())
	{
		// Last resort: cwd-relative (useful when running directly from build dir)
		url = "file://" + std::filesystem::absolute("UI-V2/dist/index.html").string();
	}

	CefRect initial_bounds;
	initial_bounds.x      = 100;
	initial_bounds.y      = 100;
	initial_bounds.width  = 1400;
	initial_bounds.height = 900;

	CefRefPtr<UamRootWindowDelegate> window_delegate =
		new UamRootWindowDelegate(initial_bounds);

	CefRefPtr<CefBrowserView> browser_view = CefBrowserView::CreateBrowserView(
		client,
		url,
		browser_settings,
		nullptr,
		nullptr,
		window_delegate);

	window_delegate->SetBrowserView(browser_view);
	CefWindow::CreateTopLevelWindow(window_delegate);
}

void UamCefApp::OnBeforeChildProcessLaunch(CefRefPtr<CefCommandLine> /*command_line*/)
{
	// Called before each subprocess is launched (renderer, GPU, etc.)
	// No modifications needed for UAM.
}

// ---------------------------------------------------------------------------
// CefRenderProcessHandler — runs in the renderer subprocess
// ---------------------------------------------------------------------------

void UamCefApp::OnWebKitInitialized()
{
	// Create the renderer-side message router. This injects window.cefQuery
	// into every page loaded by the renderer. Must use the same config as the
	// browser-side router in UamCefClient.
	CefMessageRouterConfig config;
	m_renderer_router = CefMessageRouterRendererSide::Create(config);
}

void UamCefApp::OnContextCreated(CefRefPtr<CefBrowser>   browser,
                                  CefRefPtr<CefFrame>     frame,
                                  CefRefPtr<CefV8Context> context)
{
	m_renderer_router->OnContextCreated(browser, frame, context);
}

void UamCefApp::OnContextReleased(CefRefPtr<CefBrowser>   browser,
                                   CefRefPtr<CefFrame>     frame,
                                   CefRefPtr<CefV8Context> context)
{
	m_renderer_router->OnContextReleased(browser, frame, context);
}

bool UamCefApp::OnProcessMessageReceived(CefRefPtr<CefBrowser>        browser,
                                          CefRefPtr<CefFrame>          frame,
                                          CefProcessId                 source_process,
                                          CefRefPtr<CefProcessMessage> message)
{
	return m_renderer_router->OnProcessMessageReceived(browser, frame, source_process, message);
}
