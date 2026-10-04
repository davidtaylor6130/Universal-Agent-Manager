#include "cef/uam_cef_client.h"
#include "cef/cef_push.h"
#include "cef/state_serializer.h"
#include "cef/uam_cef_security.h"
#include "common/utils/diagnostic_log.h"
#include "include/cef_task.h"

#include "include/wrapper/cef_helpers.h"
#include "include/wrapper/cef_message_router.h"
#include "include/cef_parser.h"

#include <utility>

namespace
{
	class UiRecoveryTask final : public CefTask
	{
	  public:
		explicit UiRecoveryTask(std::function<void()> callback) : m_callback(std::move(callback)) {}
		void Execute() override { m_callback(); }
	  private:
		std::function<void()> m_callback;
		IMPLEMENT_REFCOUNTING(UiRecoveryTask);
	};
}

UamCefClient::UamCefClient(uam::AppState& app, std::string trusted_ui_index_url, BrowserReadyCallback on_ready)
	: m_app(app)
	, m_trustedUiIndexUrl(std::move(trusted_ui_index_url))
	, m_onReady(std::move(on_ready))
{
	// The message router cannot be created here: this client is constructed
	// before CefInitialize(), and CefMessageRouterBrowserSide::AddHandler()
	// requires the CEF UI thread to exist (fatal CHECK otherwise).  It is
	// created lazily in EnsureMessageRouter() from OnAfterCreated().
}

void UamCefClient::EnsureMessageRouter()
{
	CEF_REQUIRE_UI_THREAD();
	if (m_router != nullptr)
	{
		return;
	}

	CefMessageRouterConfig router_config;
	m_router        = CefMessageRouterBrowserSide::Create(router_config);
	m_queryHandler  = std::make_unique<UamQueryHandler>(m_app, m_trustedUiIndexUrl);
	m_router->AddHandler(m_queryHandler.get(), true);
}

bool UamCefClient::IsTrustedMainFrame(CefRefPtr<CefFrame> frame) const
{
	return uam::cef::IsTrustedMainFrame(frame, m_trustedUiIndexUrl);
}

bool UamCefClient::ShouldCancelNavigationToUrl(const std::string& target_url) const
{
	if (uam::cef::IsTrustedUiUrl(target_url, m_trustedUiIndexUrl))
	{
		return false;
	}

	if (uam::cef::ShouldOpenExternally(target_url))
	{
		(void)uam::cef::OpenUrlExternally(target_url);
	}

	return true;
}

// ---------------------------------------------------------------------------
// CefLifeSpanHandler
// ---------------------------------------------------------------------------

bool UamCefClient::OnBeforePopup(CefRefPtr<CefBrowser> /*browser*/,
                                  CefRefPtr<CefFrame> /*frame*/,
                                  int /*popup_id*/,
                                  const CefString& target_url,
                                  const CefString& /*target_frame_name*/,
                                  CefLifeSpanHandler::WindowOpenDisposition /*target_disposition*/,
                                  bool /*user_gesture*/,
                                  const CefPopupFeatures& /*popup_features*/,
                                  CefWindowInfo& /*window_info*/,
                                  CefRefPtr<CefClient>& /*client*/,
                                  CefBrowserSettings& /*settings*/,
                                  CefRefPtr<CefDictionaryValue>& /*extra_info*/,
                                  bool* /*no_javascript_access*/)
{
	CEF_REQUIRE_UI_THREAD();
	return uam::cef::CancelPopupAndOpenExternally(target_url.ToString(), [](const std::string& url) {
		return uam::cef::OpenUrlExternally(url);
	});
}

void UamCefClient::OnAfterCreated(CefRefPtr<CefBrowser> browser)
{
	CEF_REQUIRE_UI_THREAD();
	EnsureMessageRouter();
	m_browser = browser;
	// Expose React controls to native accessibility readers and computer-use targeting.
	browser->GetHost()->SetAccessibilityState(STATE_ENABLED);
	m_queryHandler->StartCompanion(browser);

	if (m_onReady)
	{
		m_onReady(browser);
	}
}

bool UamCefClient::DoClose(CefRefPtr<CefBrowser> /*browser*/)
{
	// Allow the OS close to proceed.
	return false;
}

void UamCefClient::OnBeforeClose(CefRefPtr<CefBrowser> browser)
{
	CEF_REQUIRE_UI_THREAD();
	if (m_router != nullptr)
	{
		m_router->OnBeforeClose(browser);
		m_router->RemoveHandler(m_queryHandler.get());
		m_queryHandler.reset();
		// CEF's router can begin destroying itself while its close callbacks unwind.
		// The process is exiting, so drop our wrapper without re-entering Release().
		(void)m_router.release();
	}
	++m_recoveryGeneration;
	m_nativeRecovery = {};
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
	(void)browser;
	(void)http_status_code;

	if (!IsTrustedMainFrame(frame))
	{
		return;
	}

	if (http_status_code >= 200 && http_status_code < 400)
	{
		// A replacement renderer needs a fresh accessibility tree, even for the same browser.
		browser->GetHost()->SetAccessibilityState(STATE_DISABLED);
		browser->GetHost()->SetAccessibilityState(STATE_ENABLED);
		if (m_nativeRecovery) m_nativeRecovery(false);
	}

	// React pulls the initial state with getInitialState during store bootstrap.
	// Avoid a duplicate full-state serialization/push on the CEF UI thread here.
}

void UamCefClient::OnLoadError(CefRefPtr<CefBrowser> /*browser*/,
                                CefRefPtr<CefFrame>   frame,
                                ErrorCode             error_code,
                                const CefString&      error_text,
                                const CefString&      failed_url)
{
	if (frame == nullptr || !frame->IsMain())
	{
		return;
	}

	(void)error_text;
	(void)failed_url;
	if (error_code == ERR_ABORTED) return;
	uam::diagnostics::Write("[UI recovery] Main document load failed: " + std::to_string(static_cast<int>(error_code)));
	if (m_nativeRecovery) m_nativeRecovery(true);

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
// CefContextMenuHandler — suppress browser chrome while preserving text editing
// ---------------------------------------------------------------------------

void UamCefClient::OnBeforeContextMenu(CefRefPtr<CefBrowser>           /*browser*/,
                                        CefRefPtr<CefFrame>             /*frame*/,
                                        CefRefPtr<CefContextMenuParams> params,
                                        CefRefPtr<CefMenuModel>         model)
{
	if (model == nullptr)
	{
		return;
	}

	model->Clear();
	model->AddItem(MENU_ID_USER_FIRST, "Reload interface (Ctrl+Shift+R)");
	model->AddSeparator();

	if (params == nullptr)
	{
		return;
	}

	if (params->IsEditable())
	{
		model->AddItem(MENU_ID_CUT, "Cut");
		model->AddItem(MENU_ID_COPY, "Copy");
		model->AddItem(MENU_ID_PASTE, "Paste");
		model->AddSeparator();
		model->AddItem(MENU_ID_SELECT_ALL, "Select All");
		return;
	}

	if (!params->GetSelectionText().empty())
	{
		model->AddItem(MENU_ID_COPY, "Copy");
	}
}

bool UamCefClient::OnContextMenuCommand(CefRefPtr<CefBrowser>           /*browser*/,
                                         CefRefPtr<CefFrame>             frame,
                                         CefRefPtr<CefContextMenuParams> /*params*/,
                                         int                             command_id,
                                         EventFlags                      /*event_flags*/)
{
	if (command_id == MENU_ID_USER_FIRST) { ReloadInterface(); return true; }
	if (frame == nullptr)
	{
		return false;
	}

	switch (command_id)
	{
	case MENU_ID_CUT:
		frame->Cut();
		return true;
	case MENU_ID_COPY:
		frame->Copy();
		return true;
	case MENU_ID_PASTE:
		frame->Paste();
		return true;
	case MENU_ID_SELECT_ALL:
		frame->SelectAll();
		return true;
	default:
		return false;
	}
}

// ---------------------------------------------------------------------------
// CefKeyboardHandler — block DevTools/view-source shortcuts and bridge standard edits
// ---------------------------------------------------------------------------

bool UamCefClient::OnKeyEvent(CefRefPtr<CefBrowser> browser,
                               const CefKeyEvent&    event,
                               CefEventHandle        /*os_event*/)
{
	if (event.type == KEYEVENT_RAWKEYDOWN)
	{
		// F12
		if (event.windows_key_code == 123)
		{
			return true;
		}

		// Ctrl+Shift+I (DevTools)
		if ((event.modifiers & EVENTFLAG_CONTROL_DOWN) &&
		    (event.modifiers & EVENTFLAG_SHIFT_DOWN) &&
		    event.windows_key_code == 'I')
		{
			return true;
		}

		// Ctrl+U (view-source)
		if ((event.modifiers & EVENTFLAG_CONTROL_DOWN) &&
		    event.windows_key_code == 'U')
		{
			return true;
		}

		CefRefPtr<CefFrame> frame = browser ? browser->GetFocusedFrame() : nullptr;
		if (frame && uam::cef::DispatchEditCommandForKeyEvent(event, *frame))
		{
			return true;
		}
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
	if (m_router == nullptr || !IsTrustedMainFrame(frame))
	{
		return false;
	}

	return m_router->OnProcessMessageReceived(browser, frame, source_process, message);
}

// ---------------------------------------------------------------------------
// CefRequestHandler
// ---------------------------------------------------------------------------

bool UamCefClient::OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame>   frame,
                                   CefRefPtr<CefRequest> request,
                                   bool                  /*user_gesture*/,
                                   bool                  /*is_redirect*/)
{
	CEF_REQUIRE_UI_THREAD();
	(void)browser;
	(void)frame;

	if (request == nullptr)
	{
		return true;
	}

	const bool cancel = ShouldCancelNavigationToUrl(request->GetURL());
	if (!cancel && frame && frame->IsMain()) ++m_recoveryGeneration;
	if (!cancel && m_router) m_router->OnBeforeBrowse(browser, frame);
	return cancel;
}

bool UamCefClient::OnOpenURLFromTab(CefRefPtr<CefBrowser> browser,
                                     CefRefPtr<CefFrame>   frame,
                                     const CefString&      target_url,
                                     cef_window_open_disposition_t /*target_disposition*/,
                                     bool                  /*user_gesture*/)
{
	CEF_REQUIRE_UI_THREAD();
	(void)browser;
	(void)frame;

	return ShouldCancelNavigationToUrl(target_url.ToString());
}

void UamCefClient::ReloadInterface()
{
	CEF_REQUIRE_UI_THREAD();
	if (!m_browser || !m_browser->IsValid()) return;
	m_recoveryBudget.Reset();
	m_rendererUnresponsive = false;
	++m_recoveryGeneration;
	uam::diagnostics::Write("[UI recovery] User requested interface reload.");
	m_browser->GetMainFrame()->LoadURL(m_trustedUiIndexUrl);
}

void UamCefClient::RecoverInterface(unsigned int generation)
{
	CEF_REQUIRE_UI_THREAD();
	if (generation != m_recoveryGeneration || !m_browser || !m_browser->IsValid()) return;
	m_browser->GetMainFrame()->LoadURL(m_trustedUiIndexUrl);
}

void UamCefClient::OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus status, int error_code, const CefString& error_string)
{
	CEF_REQUIRE_UI_THREAD();
	(void)error_string;
	if (m_router) m_router->OnRenderProcessTerminated(browser);
	m_rendererUnresponsive = false;
	uam::diagnostics::Write("[UI recovery] Renderer terminated: status=" + std::to_string(static_cast<int>(status)) + " code=" + std::to_string(error_code));
	if (m_nativeRecovery) m_nativeRecovery(true);
	const unsigned int generation = ++m_recoveryGeneration;
	if (!m_recoveryBudget.AllowAutomaticRecovery())
	{
		uam::diagnostics::Write("[UI recovery] Automatic retry limit reached; native recovery remains available.");
		return;
	}
	const CefRefPtr<UamCefClient> client(this);
	CefPostDelayedTask(TID_UI, new UiRecoveryTask([client, generation]() { client->RecoverInterface(generation); }), 1000);
}

bool UamCefClient::OnRenderProcessUnresponsive(CefRefPtr<CefBrowser> browser, CefRefPtr<CefUnresponsiveProcessCallback> callback)
{
	CEF_REQUIRE_UI_THREAD();
	(void)browser;
	if (!m_rendererUnresponsive)
	{
		m_rendererUnresponsive = true;
		uam::diagnostics::Write("[UI recovery] Renderer unresponsive; allowing one additional hang-monitor interval.");
		callback->Wait();
	}
	else
	{
		uam::diagnostics::Write("[UI recovery] Renderer still unresponsive; terminating it for recovery.");
		callback->Terminate();
	}
	return true;
}

void UamCefClient::OnRenderProcessResponsive(CefRefPtr<CefBrowser> browser)
{
	CEF_REQUIRE_UI_THREAD();
	(void)browser;
	if (!m_rendererUnresponsive) return;
	m_rendererUnresponsive = false;
	if (m_nativeRecovery) m_nativeRecovery(false);
}

bool UamCefClient::OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level, const CefString& message, const CefString& source, int line)
{
	(void)browser;
	(void)source;
	(void)line;
	const std::string text = message.ToString();
	if (level >= LOGSEVERITY_ERROR || text.starts_with("[UI recovery]") || text.starts_with("[UI startup]"))
		uam::diagnostics::Write("[frontend] " + text);
	return false;
}
