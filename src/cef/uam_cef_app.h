#pragma once

#include "cef/cef_includes.h"
#include <functional>
#include <string>
#include <utility>

/// <summary>
/// CefApp implementation for both the browser (main) process and renderer subprocesses.
/// Implements CefRenderProcessHandler so CefMessageRouterRendererSide is created in the
/// renderer process — this is what injects window.cefQuery into the page.
/// </summary>
class UamCefApp : public CefApp, public CefBrowserProcessHandler, public CefRenderProcessHandler
{
  public:
	using FatalStartupCallback = std::function<void(const std::string&)>;

	explicit UamCefApp(FatalStartupCallback on_fatal_startup = {})
		: m_onFatalStartup(std::move(on_fatal_startup))
	{
	}

	// CefApp
	CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override
	{
		return this;
	}
	CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override
	{
		return this;
	}

	void OnBeforeCommandLineProcessing(const CefString& process_type, CefRefPtr<CefCommandLine> command_line) override;
	void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override;

	// CefBrowserProcessHandler
	void OnContextInitialized() override;
	void OnBeforeChildProcessLaunch(CefRefPtr<CefCommandLine> command_line) override;

	// CefRenderProcessHandler — creates the renderer-side message router that injects window.cefQuery
	void OnWebKitInitialized() override;
	void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) override;
	void OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) override;
	bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source_process, CefRefPtr<CefProcessMessage> message) override;

  private:
	void FailStartup(const std::string& error);

	std::string m_trustedUiIndexUrl;
	CefRefPtr<CefMessageRouterRendererSide> m_renderer_router;
	FatalStartupCallback m_onFatalStartup;

	IMPLEMENT_REFCOUNTING(UamCefApp);
};
