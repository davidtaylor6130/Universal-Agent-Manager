#pragma once

#include "cef/cef_includes.h"
#include <functional>
#include <string>
#include <utility>

namespace uam::cef
{
	/// <summary>Serves the packaged UI. Null until the CEF context is initialised; the hidden HTML view browsers register it in their own context.</summary>
	CefRefPtr<CefSchemeHandlerFactory> UiSchemeHandlerFactory();
}

/// <summary>
/// CefApp implementation for the browser (main) process. Subprocesses use UamCefRendererApp,
/// which injects window.cefQuery into the trusted UI document.
/// </summary>
class UamCefApp : public CefApp, public CefBrowserProcessHandler
{
  public:
	using FatalStartupCallback = std::function<void(const std::string&)>;

	explicit UamCefApp(FatalStartupCallback on_fatal_startup = {}, std::function<void()> on_management_ready = {})
		: m_onFatalStartup(std::move(on_fatal_startup))
		, m_onManagementReady(std::move(on_management_ready))
	{
	}

	// CefApp
	CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override
	{
		return this;
	}

	void OnBeforeCommandLineProcessing(const CefString& process_type, CefRefPtr<CefCommandLine> command_line) override;
	void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override;

	// CefBrowserProcessHandler
	void OnContextInitialized() override;
	void OnBeforeChildProcessLaunch(CefRefPtr<CefCommandLine> command_line) override;

  private:
	void FailStartup(const std::string& error);

	std::string m_trustedUiIndexUrl;
	FatalStartupCallback m_onFatalStartup;
	std::function<void()> m_onManagementReady;

	IMPLEMENT_REFCOUNTING(UamCefApp);
};
