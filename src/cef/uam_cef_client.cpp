#include "cef/uam_cef_client.h"
#include "cef/cef_push.h"
#include "cef/state_serializer.h"

#include "include/wrapper/cef_helpers.h"
#include "include/wrapper/cef_message_router.h"
#include "include/cef_parser.h"

UamCefClient::UamCefClient(uam::AppState& app, BrowserReadyCallback on_ready)
	: m_app(app)
	, m_onReady(std::move(on_ready))
{
	CefMessageRouterConfig router_config;
	m_router        = CefMessageRouterBrowserSide::Create(router_config);
	m_queryHandler  = std::make_unique<UamQueryHandler>(app);
	m_router->AddHandler(m_queryHandler.get(), true);
}

// ---------------------------------------------------------------------------
// CefLifeSpanHandler
// ---------------------------------------------------------------------------

void UamCefClient::OnAfterCreated(CefRefPtr<CefBrowser> browser)
{
	CEF_REQUIRE_UI_THREAD();
	m_browser = browser;

	if (m_onReady)
		m_onReady(browser);
}

bool UamCefClient::DoClose(CefRefPtr<CefBrowser> /*browser*/)
{
	// Allow the OS close to proceed.
	return false;
}

void UamCefClient::OnBeforeClose(CefRefPtr<CefBrowser> browser)
{
	CEF_REQUIRE_UI_THREAD();
	m_router->OnBeforeClose(browser);
	m_browser = nullptr;
	CefQuitMessageLoop();
}

// ---------------------------------------------------------------------------
// CefLoadHandler
// ---------------------------------------------------------------------------

void UamCefClient::OnLoadEnd(CefRefPtr<CefBrowser> browser,
                              CefRefPtr<CefFrame>   frame,
                              int                   http_status_code)
{
	if (!frame->IsMain())
		return;

	// Push the full serialised application state as soon as the page is ready.
	uam::PushStateUpdate(browser, m_app);
}

void UamCefClient::OnLoadError(CefRefPtr<CefBrowser> /*browser*/,
                                CefRefPtr<CefFrame>   frame,
                                ErrorCode             error_code,
                                const CefString&      error_text,
                                const CefString&      failed_url)
{
	if (!frame->IsMain())
		return;

	// Inject a minimal error page so the window is not blank.
	std::string html =
		"<html><body style='background:#0b0b0e;color:#f97316;font-family:monospace;padding:40px'>"
		"<h2>UAM — Load Error</h2>"
		"<p>Failed to load: " + failed_url.ToString() + "</p>"
		"<p>Error " + std::to_string(static_cast<int>(error_code)) + ": " + error_text.ToString() + "</p>"
		"</body></html>";

	frame->LoadURL("data:text/html;charset=utf-8," + CefURIEncode(html, false).ToString());
}

// ---------------------------------------------------------------------------
// CefDisplayHandler
// ---------------------------------------------------------------------------

void UamCefClient::OnTitleChange(CefRefPtr<CefBrowser> browser,
                                  const CefString&       title)
{
	// No-op for now — native window title is set during creation.
	(void)browser;
	(void)title;
}

// ---------------------------------------------------------------------------
// CefContextMenuHandler — suppress browser context menu entirely
// ---------------------------------------------------------------------------

void UamCefClient::OnBeforeContextMenu(CefRefPtr<CefBrowser>           /*browser*/,
                                        CefRefPtr<CefFrame>             /*frame*/,
                                        CefRefPtr<CefContextMenuParams> /*params*/,
                                        CefRefPtr<CefMenuModel>         model)
{
	model->Clear();
}

// ---------------------------------------------------------------------------
// CefKeyboardHandler — block DevTools and view-source shortcuts
// ---------------------------------------------------------------------------

bool UamCefClient::OnKeyEvent(CefRefPtr<CefBrowser> /*browser*/,
                               const CefKeyEvent&    event,
                               CefEventHandle        /*os_event*/)
{
	if (event.type == KEYEVENT_RAWKEYDOWN)
	{
		// F12
		if (event.windows_key_code == 123)
			return true;

		// Ctrl+Shift+I (DevTools)
		if ((event.modifiers & EVENTFLAG_CONTROL_DOWN) &&
		    (event.modifiers & EVENTFLAG_SHIFT_DOWN) &&
		    event.windows_key_code == 'I')
			return true;

		// Ctrl+U (view-source)
		if ((event.modifiers & EVENTFLAG_CONTROL_DOWN) &&
		    event.windows_key_code == 'U')
			return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// CefClient
// ---------------------------------------------------------------------------

bool UamCefClient::OnProcessMessageReceived(CefRefPtr<CefBrowser>        browser,
                                             CefRefPtr<CefFrame>          frame,
                                             CefProcessId                 source_process,
                                             CefRefPtr<CefProcessMessage> message)
{
	return m_router->OnProcessMessageReceived(browser, frame, source_process, message);
}
