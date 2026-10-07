#include "cef/uam_cef_app.h"
#include "cef/uam_cef_client.h"
#include "cef/uam_cef_command_line_config.h"
#include "cef/uam_cef_security.h"
#include "cef/ui_asset_snapshot.h"
#include "include/views/cef_label_button.h"
#include "include/views/cef_panel.h"
#include "include/views/cef_box_layout.h"
#include "include/views/cef_button_delegate.h"
#include "include/wrapper/cef_byte_read_handler.h"
#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"
#include "common/utils/io_utils.h"
#include "common/utils/diagnostic_log.h"

#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#include "include/cef_image.h"
#include "include/cef_parser.h"
#include "include/cef_path_util.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_fill_layout.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_stream_resource_handler.h"

#include <filesystem>
#include <vector>

namespace
{
	class UamUiSchemeHandlerFactory final : public CefSchemeHandlerFactory
	{
	  public:
		explicit UamUiSchemeHandlerFactory(const std::filesystem::path& root)
		{
			try { m_assets.emplace(root); }
			catch (const std::exception& error)
			{
				uam::diagnostics::Write(std::string("[UI recovery] Asset snapshot failed: ") + error.what());
			}
		}

		CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
		                                      const CefString& scheme_name,
		                                      CefRefPtr<CefRequest> request) override
		{
			if (request == nullptr || !uam::strings::EqualsIgnoreCase(scheme_name.ToString(), "uam") ||
			    request->GetMethod().ToString() != "GET")
			{
				return nullptr;
			}
			const std::optional<std::filesystem::path> relative = uam::cef::TrustedUiResourceRelativePath(request->GetURL().ToString());
			if (!relative) return nullptr;
			const std::string key = relative->generic_string();
			const std::string* bytes = m_assets ? m_assets->Find(key) : nullptr;
			if (!m_assets && key == "index.html") bytes = &m_startupFailure;
			if (!bytes) return nullptr;
			// The reader retains this factory, so its immutable bytes outlive pending requests.
			CefRefPtr<CefStreamReader> stream = CefStreamReader::CreateForHandler(new CefByteReadHandler(
			    reinterpret_cast<const unsigned char*>(bytes->data()), bytes->size(), this));
			std::string extension = relative->extension().string();
			if (!extension.empty() && extension.front() == '.') extension.erase(0, 1);
			CefString mime = CefGetMimeType(extension);
			if (mime.empty()) mime = "application/octet-stream";
			CefResponse::HeaderMap headers;
			headers.emplace("Content-Security-Policy", std::string(uam::cef::kContentSecurityPolicy));
			headers.emplace("X-Content-Type-Options", "nosniff");
			headers.emplace("Cache-Control", "no-cache");
			return new CefStreamResourceHandler(200, "OK", mime, headers, stream);
		}

	  private:
		std::optional<uam::cef::UiAssetSnapshot> m_assets;
		const std::string m_startupFailure = "<!doctype html><html><body style='background:#000;color:#fff;padding:24px;font:14px system-ui'><h1 style='font-size:16px'>The installed interface could not load.</h1><p>Close UAM and install it again.</p><p>Your saved chats are kept in the data folder.</p></body></html>";
		IMPLEMENT_REFCOUNTING(UamUiSchemeHandlerFactory);
	};

	CefRefPtr<CefImage> LoadUamWindowIcon()
	{
		CefString exe_dir_string;
		if (!CefGetPath(PK_DIR_EXE, exe_dir_string))
		{
			return nullptr;
		}

		const std::filesystem::path exe_dir = uam::paths::PathFromUtf8(exe_dir_string.ToString());
		const std::vector<std::filesystem::path> icon_paths = {
		    exe_dir / "app_icon.png",
		    uam::paths::LexicallyNormalPath(exe_dir / ".." / "Resources" / "app_icon.png"),
		};

		for (const std::filesystem::path& icon_path : icon_paths)
		{
			if (!uam::paths::PathExistsNoThrow(icon_path))
			{
				continue;
			}

			std::string bytes;
			if (!uam::io::TryReadBinaryFile(icon_path, bytes))
			{
				continue;
			}
			if (bytes.empty())
			{
				continue;
			}

			CefRefPtr<CefImage> image = CefImage::CreateImage();
			if (image != nullptr && image->AddPNG(1.0f, bytes.data(), bytes.size()))
			{
				return image;
			}
		}

		return nullptr;
	}

	class UamRootWindowDelegate : public CefWindowDelegate, public CefBrowserViewDelegate, public CefButtonDelegate
	{
	  public:
		explicit UamRootWindowDelegate(const CefRect& initial_bounds) : m_initialBounds(initial_bounds)
		{
		}

		void SetBrowserView(CefRefPtr<CefBrowserView> browser_view)
		{
			m_browserView = browser_view;
		}

		void SetClient(CefRefPtr<UamCefClient> client) { m_client = client; }

		/// Native fallback is painted by the browser process, even when the renderer cannot run.
		void ShowRecovery(bool visible)
		{
			if (!m_recoveryPanel || !m_browserView) return;
			m_recoveryPanel->SetBackgroundColor(0xFF000000);
			m_browserView->SetVisible(!visible);
			m_recoveryPanel->SetVisible(visible);
			if (CefRefPtr<CefWindow> window = m_browserView->GetWindow()) window->Layout();
			if (visible) m_recoveryButton->RequestFocus();
		}

		void OnThemeChanged(CefRefPtr<CefView> view) override
		{
			view->SetBackgroundColor(0xFF000000);
		}

		void OnButtonPressed(CefRefPtr<CefButton> button) override
		{
			(void)button;
			if (m_client) m_client->ReloadInterface();
		}

		bool OnAccelerator(CefRefPtr<CefWindow> window, int command_id) override
		{
			(void)window;
			if (command_id != MENU_ID_USER_FIRST || !m_client) return false;
			m_client->ReloadInterface();
			return true;
		}

		void OnWindowCreated(CefRefPtr<CefWindow> window) override
		{
			CEF_REQUIRE_UI_THREAD();

			window->SetTitle("Universal Agent Manager");
			if (CefRefPtr<CefImage> icon = LoadUamWindowIcon())
			{
				window->SetWindowIcon(icon);
				window->SetWindowAppIcon(icon);
			}

			window->SetToFillLayout();

			if (m_browserView)
			{
				window->AddChildView(m_browserView);
			}

			m_recoveryPanel = CefPanel::CreatePanel(nullptr);
			m_recoveryPanel->SetBackgroundColor(0xFF000000);
			CefBoxLayoutSettings layout;
			layout.horizontal = false;
			layout.inside_border_insets = CefInsets(24, 24, 24, 24);
			layout.between_child_spacing = 12;
			layout.main_axis_alignment = CEF_AXIS_ALIGNMENT_START;
			layout.cross_axis_alignment = CEF_AXIS_ALIGNMENT_START;
			m_recoveryPanel->SetToBoxLayout(layout);
			CefRefPtr<CefLabelButton> title = CefLabelButton::CreateLabelButton(this, "The interface could not load.");
			title->SetEnabled(false);
			title->SetTextColor(CEF_BUTTON_STATE_DISABLED, 0xFFFFFFFF);
			m_recoveryPanel->AddChildView(title);
			CefRefPtr<CefLabelButton> detail = CefLabelButton::CreateLabelButton(this, "Active sessions keep running. Unsaved drafts may be lost when reloading.");
			detail->SetEnabled(false);
			detail->SetTextColor(CEF_BUTTON_STATE_DISABLED, 0xFFFFFFFF);
			m_recoveryPanel->AddChildView(detail);
			m_recoveryButton = CefLabelButton::CreateLabelButton(this, "Reload interface (Ctrl+Shift+R)");
			m_recoveryButton->SetTextColor(CEF_BUTTON_STATE_NORMAL, 0xFFFFFFFF);
			m_recoveryButton->SetTextColor(CEF_BUTTON_STATE_HOVERED, 0xFFFFFFFF);
			m_recoveryButton->SetTextColor(CEF_BUTTON_STATE_PRESSED, 0xFFFFFFFF);
			m_recoveryPanel->AddChildView(m_recoveryButton);
			m_recoveryPanel->SetVisible(false);
			window->AddChildView(m_recoveryPanel);
			window->SetAccelerator(MENU_ID_USER_FIRST, 'R', true, true, false, true);
			window->Show();
		}

		void OnWindowDestroyed(CefRefPtr<CefWindow> /*window*/) override
		{
			CEF_REQUIRE_UI_THREAD();
			m_browserView = nullptr;
			m_recoveryButton = nullptr;
			m_recoveryPanel = nullptr;
			m_client = nullptr;
		}

		CefRect GetInitialBounds(CefRefPtr<CefWindow> /*window*/) override
		{
			return m_initialBounds;
		}

		bool CanClose(CefRefPtr<CefWindow> /*window*/) override
		{
			CEF_REQUIRE_UI_THREAD();

			if (!m_browserView)
			{
				return true;
			}

			CefRefPtr<CefBrowser> browser = m_browserView->GetBrowser();
			if (!browser)
			{
				return true;
			}

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
		CefRect m_initialBounds;
		CefRefPtr<CefBrowserView> m_browserView;
		CefRefPtr<CefLabelButton> m_recoveryButton;
		CefRefPtr<CefPanel> m_recoveryPanel;
		CefRefPtr<UamCefClient> m_client;

		IMPLEMENT_REFCOUNTING(UamRootWindowDelegate);
	};

	bool IsTrustedRendererFrame(CefRefPtr<CefFrame> frame, const std::string& trusted_ui_index_url)
	{
		return frame != nullptr && uam::cef::IsTrustedUiUrl(frame->GetURL().ToString(), trusted_ui_index_url);
	}

} // namespace

// Forward declaration — Application owns AppState and provides access to the browser ref.
// We use a global pointer set by Application::InitializeCef().
// This keeps the CEF layer decoupled from Application's header.
namespace uam_cef_globals
{
	extern uam::AppState* g_app_state;
	extern CefRefPtr<UamCefClient> g_client;
} // namespace uam_cef_globals

void UamCefApp::FailStartup(const std::string& error)
{
	if (m_onFatalStartup)
	{
		m_onFatalStartup(error);
		return;
	}
	uam::diagnostics::Write("[CEF] " + error);
}

void UamCefApp::OnBeforeCommandLineProcessing(const CefString& process_type, CefRefPtr<CefCommandLine> command_line)
{
	if (command_line == nullptr)
	{
		return;
	}

#if defined(__APPLE__)
	// Disable Chromium features that trigger an EXC_BREAKPOINT / SIGTRAP crash
	// on macOS 26.x (beta).  The crash manifests as NSApplication receiving a
	// message with the selector "%s" (an unsubstituted format-string placeholder)
	// from ChromeWebAppShortcutCopierMain — a Chromium internal worker that
	// handles OS-level web-app shortcut creation.  UAM does not use web-app
	// shortcuts, so these features can be disabled without functional loss.
	const std::string disabled_features = uam::cef::MacOsWebAppShortcutCrashDisabledFeatures();
	command_line->AppendSwitchWithValue("disable-features", disabled_features);

	// Skip first-run tasks and background-mode processes that may exercise
	// other macOS APIs removed or renamed in the beta OS.
	command_line->AppendSwitch("no-first-run");
	command_line->AppendSwitch("disable-background-mode");
	command_line->AppendSwitch("disable-background-networking");
	command_line->AppendSwitch("disable-component-update");
	command_line->AppendSwitch("disable-default-apps");
	command_line->AppendSwitch("disable-sync");

	if (process_type.empty())
	{
		uam::diagnostics::Write("[CEF] macOS web-app shortcut crash workaround: disable-features=" + disabled_features);
	}
#endif

	if (PlatformServicesFactory::Instance().process_service.EmbeddedBrowserUsesMockKeychain())
	{
		command_line->AppendSwitch("use-mock-keychain");
	}
}

void UamCefApp::OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar)
{
	if (registrar == nullptr) return;
	registrar->AddCustomScheme(
	    "uam", CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE |
	               CEF_SCHEME_OPTION_CORS_ENABLED | CEF_SCHEME_OPTION_FETCH_ENABLED |
	               CEF_SCHEME_OPTION_DISPLAY_ISOLATED);
}

void UamCefApp::OnContextInitialized()
{
	CEF_REQUIRE_UI_THREAD();
	m_trustedUiIndexUrl = uam::cef::ResolveTrustedUiIndexUrl();
	CefString exe_dir_string;
	const std::filesystem::path exe_dir = CefGetPath(PK_DIR_EXE, exe_dir_string)
	                                          ? uam::paths::PathFromUtf8(exe_dir_string.ToString())
	                                          : std::filesystem::path(".");
	if (!CefRegisterSchemeHandlerFactory(
	        "uam", "app", new UamUiSchemeHandlerFactory(uam::cef::ResolveTrustedUiRoot(exe_dir))))
	{
		FailStartup("Failed to register the bundled UAM UI scheme.");
		return;
	}

	// Reuse the pre-constructed client if Application already set it up
	// (e.g. with a BrowserReadyCallback).  Fall back to a fresh client if not.
	uam::AppState* app_state = uam_cef_globals::g_app_state;
	if (app_state == nullptr)
	{
		FailStartup("AppState is unavailable during context initialization.");
		return;
	}

	CefRefPtr<UamCefClient> client = uam_cef_globals::g_client;
	if (!client)
	{
		client = new UamCefClient(*app_state, m_trustedUiIndexUrl);
		uam_cef_globals::g_client = client;
	}

	CefBrowserSettings browser_settings;
	browser_settings.javascript = STATE_ENABLED;
	browser_settings.local_storage = STATE_ENABLED;
	browser_settings.javascript_access_clipboard = STATE_ENABLED;

	CefRect initial_bounds;
	initial_bounds.x = 100;
	initial_bounds.y = 100;
	initial_bounds.width = 1400;
	initial_bounds.height = 900;

	CefRefPtr<UamRootWindowDelegate> window_delegate = new UamRootWindowDelegate(initial_bounds);

	CefRefPtr<CefBrowserView> browser_view = CefBrowserView::CreateBrowserView(client, m_trustedUiIndexUrl, browser_settings, nullptr, nullptr, window_delegate);
	if (browser_view == nullptr)
	{
		FailStartup("Failed to create the application browser view.");
		return;
	}

	window_delegate->SetClient(client);
	client->SetNativeRecoveryCallback([window_delegate](bool visible) { window_delegate->ShowRecovery(visible); });
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
	m_trustedUiIndexUrl = uam::cef::ResolveTrustedUiIndexUrl();

	// Create the renderer-side message router. This injects window.cefQuery
	// into every page loaded by the renderer. Must use the same config as the
	// browser-side router in UamCefClient.
	CefMessageRouterConfig config;
	m_renderer_router = CefMessageRouterRendererSide::Create(config);
}

void UamCefApp::OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context)
{
	if (m_renderer_router != nullptr && IsTrustedRendererFrame(frame, m_trustedUiIndexUrl))
	{
		m_renderer_router->OnContextCreated(browser, frame, context);
	}
}

void UamCefApp::OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context)
{
	if (m_renderer_router != nullptr && IsTrustedRendererFrame(frame, m_trustedUiIndexUrl))
	{
		m_renderer_router->OnContextReleased(browser, frame, context);
	}
}

bool UamCefApp::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source_process, CefRefPtr<CefProcessMessage> message)
{
	if (m_renderer_router == nullptr || !IsTrustedRendererFrame(frame, m_trustedUiIndexUrl))
	{
		return false;
	}

	return m_renderer_router->OnProcessMessageReceived(browser, frame, source_process, message);
}
