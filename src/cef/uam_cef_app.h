#pragma once

#include "cef/cef_includes.h"
#include <string>

/// <summary>
/// CefApp implementation for both the browser (main) process and renderer subprocesses.
/// Implements CefRenderProcessHandler so CefMessageRouterRendererSide is created in the
/// renderer process — this is what injects window.cefQuery into the page.
/// </summary>
class UamCefApp : public CefApp,
                  public CefBrowserProcessHandler,
                  public CefRenderProcessHandler
{
  public:
	UamCefApp() = default;

	// CefApp
	CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
	CefRefPtr<CefRenderProcessHandler>  GetRenderProcessHandler()  override { return this; }

	void OnBeforeCommandLineProcessing(const CefString& process_type,
	                                   CefRefPtr<CefCommandLine> command_line) override;

	// CefBrowserProcessHandler
	void OnContextInitialized() override;
	void OnBeforeChildProcessLaunch(CefRefPtr<CefCommandLine> command_line) override;

	// CefRenderProcessHandler — creates the renderer-side message router that injects window.cefQuery
	void OnWebKitInitialized() override;
	void OnContextCreated(CefRefPtr<CefBrowser>   browser,
	                      CefRefPtr<CefFrame>     frame,
	                      CefRefPtr<CefV8Context> context) override;
	void OnContextReleased(CefRefPtr<CefBrowser>   browser,
	                       CefRefPtr<CefFrame>     frame,
	                       CefRefPtr<CefV8Context> context) override;
	bool OnProcessMessageReceived(CefRefPtr<CefBrowser>        browser,
	                              CefRefPtr<CefFrame>          frame,
	                              CefProcessId                 source_process,
	                              CefRefPtr<CefProcessMessage> message) override;

  private:
	std::string m_trustedUiIndexUrl;
	CefRefPtr<CefMessageRouterRendererSide> m_renderer_router;

	IMPLEMENT_REFCOUNTING(UamCefApp);
};
