#pragma once

#include "cef/cef_includes.h"

#include <string>

namespace uam::cef
{
	/// <summary>Registers the packaged UI scheme; every CEF process must register the same schemes.</summary>
	void RegisterUiScheme(CefRawPtr<CefSchemeRegistrar> registrar);
}

/// <summary>
/// CefApp for CEF subprocesses (renderer, GPU, utility). It creates the renderer-side message router
/// that injects window.cefQuery into the trusted UI document. It deliberately avoids application and
/// platform services so the sandboxed macOS helper executable can link it on its own.
/// </summary>
class UamCefRendererApp : public CefApp, public CefRenderProcessHandler
{
  public:
	CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override
	{
		return this;
	}

	void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override;

	void OnWebKitInitialized() override;
	void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) override;
	void OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) override;
	bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source_process, CefRefPtr<CefProcessMessage> message) override;

  private:
	std::string m_trustedUiIndexUrl;
	CefRefPtr<CefMessageRouterRendererSide> m_renderer_router;

	IMPLEMENT_REFCOUNTING(UamCefRendererApp);
};
