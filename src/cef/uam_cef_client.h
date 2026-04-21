#pragma once

#include "cef/cef_includes.h"
#include "cef/uam_query_handler.h"
#include "common/state/app_state.h"

#include <functional>
#include <memory>

/// <summary>
/// Callback type used by Application to receive the browser reference
/// once CEF creates it (for pushing state updates back to JS).
/// </summary>
using BrowserReadyCallback = std::function<void(CefRefPtr<CefBrowser>)>;

/// <summary>
/// CefClient implementation for UAM.
/// Handles browser lifecycle, message routing, and the initial state push.
/// </summary>
class UamCefClient : public CefClient,
                     public CefLifeSpanHandler,
                     public CefLoadHandler,
                     public CefDisplayHandler,
                     public CefContextMenuHandler,
                     public CefKeyboardHandler,
                     public CefRequestHandler
{
  public:
	explicit UamCefClient(uam::AppState& app, std::string trusted_ui_index_url, BrowserReadyCallback on_ready = nullptr);
	~UamCefClient() override = default;

	// CefClient
	CefRefPtr<CefLifeSpanHandler>    GetLifeSpanHandler()    override { return this; }
	CefRefPtr<CefLoadHandler>        GetLoadHandler()        override { return this; }
	CefRefPtr<CefDisplayHandler>     GetDisplayHandler()     override { return this; }
	CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
	CefRefPtr<CefKeyboardHandler>    GetKeyboardHandler()    override { return this; }
	CefRefPtr<CefRequestHandler>     GetRequestHandler()     override { return this; }

	bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
	                              CefRefPtr<CefFrame>   frame,
	                              CefProcessId          source_process,
	                              CefRefPtr<CefProcessMessage> message) override;

	// CefRequestHandler — block bridge access and route external links safely.
	bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
	                    CefRefPtr<CefFrame>   frame,
	                    CefRefPtr<CefRequest> request,
	                    bool                  user_gesture,
	                    bool                  is_redirect) override;
	bool OnOpenURLFromTab(CefRefPtr<CefBrowser> browser,
	                      CefRefPtr<CefFrame>   frame,
	                      const CefString&      target_url,
	                      cef_window_open_disposition_t target_disposition,
	                      bool                  user_gesture) override;

	// CefLifeSpanHandler
	void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
	bool DoClose(CefRefPtr<CefBrowser> browser) override;
	void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

	// CefLoadHandler
	void OnLoadEnd(CefRefPtr<CefBrowser> browser,
	               CefRefPtr<CefFrame>   frame,
	               int                   http_status_code) override;
	void OnLoadError(CefRefPtr<CefBrowser> browser,
	                 CefRefPtr<CefFrame>   frame,
	                 ErrorCode             error_code,
	                 const CefString&      error_text,
	                 const CefString&      failed_url) override;

	// CefDisplayHandler
	void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) override;

	// CefContextMenuHandler — clear all context menus so no browser chrome appears
	void OnBeforeContextMenu(CefRefPtr<CefBrowser>            browser,
	                         CefRefPtr<CefFrame>              frame,
	                         CefRefPtr<CefContextMenuParams>  params,
	                         CefRefPtr<CefMenuModel>          model) override;
	bool OnContextMenuCommand(CefRefPtr<CefBrowser> browser,
	                          CefRefPtr<CefFrame> frame,
	                          CefRefPtr<CefContextMenuParams> params,
	                          int command_id,
	                          EventFlags event_flags) override;

	// CefKeyboardHandler — block DevTools shortcuts (F12, Ctrl+Shift+I, Ctrl+U)
	bool OnKeyEvent(CefRefPtr<CefBrowser>  browser,
	                const CefKeyEvent&     event,
	                CefEventHandle         os_event) override;

	/// Returns the live browser (null until OnAfterCreated).
	CefRefPtr<CefBrowser> GetBrowser() const { return m_browser; }

  private:
	uam::AppState&                           m_app;
	std::string                              m_trustedUiIndexUrl;
	CefRefPtr<CefBrowser>                    m_browser;
	CefRefPtr<CefMessageRouterBrowserSide>   m_router;
	std::unique_ptr<UamQueryHandler>         m_queryHandler;
	BrowserReadyCallback                     m_onReady;

	bool IsTrustedMainFrame(CefRefPtr<CefFrame> frame) const;

	IMPLEMENT_REFCOUNTING(UamCefClient);
};
