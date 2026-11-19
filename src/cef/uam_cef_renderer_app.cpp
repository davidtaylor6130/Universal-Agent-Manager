#include "cef/uam_cef_renderer_app.h"
#include "cef/uam_cef_security.h"

namespace
{
	bool IsTrustedRendererFrame(CefRefPtr<CefFrame> frame, const std::string& trusted_ui_index_url)
	{
		return frame != nullptr && uam::cef::ShouldBindUiBridge(frame->IsMain(), frame->GetURL().ToString(), trusted_ui_index_url);
	}
} // namespace

void uam::cef::RegisterUiScheme(CefRawPtr<CefSchemeRegistrar> registrar)
{
	if (registrar == nullptr) return;
	registrar->AddCustomScheme(
	    "uam", CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE |
	               CEF_SCHEME_OPTION_CORS_ENABLED | CEF_SCHEME_OPTION_FETCH_ENABLED |
	               CEF_SCHEME_OPTION_DISPLAY_ISOLATED);
}

void UamCefRendererApp::OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar)
{
	uam::cef::RegisterUiScheme(registrar);
}

void UamCefRendererApp::OnWebKitInitialized()
{
	m_trustedUiIndexUrl = uam::cef::ResolveTrustedUiIndexUrl();

	// Create the renderer-side message router. This injects window.cefQuery
	// into every page loaded by the renderer. Must use the same config as the
	// browser-side router in UamCefClient.
	CefMessageRouterConfig config;
	m_renderer_router = CefMessageRouterRendererSide::Create(config);
}

void UamCefRendererApp::OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context)
{
	// Runs natively for every frame, before any of its script, so a page or a child frame never sees these.
	const CefRefPtr<CefV8Value> global = context != nullptr ? context->GetGlobal() : nullptr;
	if (global != nullptr)
	{
		for (const char* name : uam::cef::kRendererGlobalsRemoved) (void)global->DeleteValue(name);
	}
	if (m_renderer_router != nullptr && IsTrustedRendererFrame(frame, m_trustedUiIndexUrl))
	{
		m_renderer_router->OnContextCreated(browser, frame, context);
	}
}

void UamCefRendererApp::OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context)
{
	if (m_renderer_router != nullptr && IsTrustedRendererFrame(frame, m_trustedUiIndexUrl))
	{
		m_renderer_router->OnContextReleased(browser, frame, context);
	}
}

bool UamCefRendererApp::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source_process, CefRefPtr<CefProcessMessage> message)
{
	if (m_renderer_router == nullptr || !IsTrustedRendererFrame(frame, m_trustedUiIndexUrl))
	{
		return false;
	}

	return m_renderer_router->OnProcessMessageReceived(browser, frame, source_process, message);
}
