#include "cef/html_view_render_check.h"

#include "cef/cef_includes.h"
#include "cef/uam_cef_app.h"
#include "cef/uam_cef_security.h"
#include "common/utils/diagnostic_log.h"

#include "include/cef_render_handler.h"
#include "include/cef_task.h"

#include <algorithm>
#include <functional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using uam::html_view::RenderCheckDone;
	using uam::html_view::RenderCheckResult;
	using uam::html_view::RenderCheckStatus;

	class ClosureTask final : public CefTask
	{
	  public:
		explicit ClosureTask(std::function<void()> callback) : m_callback(std::move(callback)) {}
		void Execute() override { m_callback(); }

	  private:
		std::function<void()> m_callback;
		IMPLEMENT_REFCOUNTING(ClosureTask);
	};

	std::string NewNonce()
	{
		std::random_device random;
		return "uam-render-check-" + std::to_string(random()) + "-" + std::to_string(random());
	}

	class HiddenBrowserRenderChecker;

	/// One hidden browser for one page. It has no message router, so nothing in it can reach the UAM bridge.
	class RenderCheckClient final : public CefClient, public CefLifeSpanHandler, public CefLoadHandler, public CefDisplayHandler, public CefRenderHandler, public CefRequestHandler
	{
	  public:
		RenderCheckClient(std::weak_ptr<HiddenBrowserRenderChecker> checker, std::string owner, std::string html, RenderCheckDone done)
			: m_checker(std::move(checker)), m_owner(std::move(owner)), m_html(std::move(html)), m_done(std::move(done)), m_nonce(NewNonce())
		{
		}

		const std::string& Owner() const { return m_owner; }

		bool Begin(CefRefPtr<CefRequestContext> context)
		{
			CefWindowInfo window;
			window.SetAsWindowless(kNullWindowHandle);
			CefBrowserSettings settings; // Default frame rate, so requestAnimationFrame runs as it does in the card.
			if (!CefBrowserHost::CreateBrowser(window, this, std::string(uam::cef::kHtmlViewProbeUrl), settings, nullptr, context))
			{
				Finish(RenderCheckStatus::NotRun, "the hidden browser could not be started.");
				return false;
			}
			CefRefPtr<RenderCheckClient> self(this);
			CefPostDelayedTask(TID_UI, new ClosureTask([self] { self->Finish(RenderCheckStatus::TimedOut); }), uam::html_view::kRenderCheckTimeoutMs);
			return true;
		}

		void Finish(RenderCheckStatus status, std::string not_run_reason = {})
		{
			if (!m_done) return;
			const RenderCheckDone done = std::exchange(m_done, nullptr);
			m_html.clear();
			m_result.status = status;
			m_result.not_run_reason = std::move(not_run_reason);
			if (status != RenderCheckStatus::Completed) // Fixed text only: page content never reaches the log.
				uam::diagnostics::Write("[HTML view check] Did not complete: " + (status == RenderCheckStatus::TimedOut ? std::string("timed out") : m_result.not_run_reason));
			if (m_browser) m_browser->GetHost()->CloseBrowser(true);
			done(std::move(m_result));
		}

		// CefClient
		CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
		CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
		CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
		CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
		CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

		// CefRenderHandler: required for a windowless browser; the page is never shown.
		void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override { rect = CefRect(0, 0, 1024, 720); }
		void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList&, const void*, int, int) override {}

		// CefLifeSpanHandler
		bool OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int, const CefString&, const CefString&, CefLifeSpanHandler::WindowOpenDisposition, bool, const CefPopupFeatures&, CefWindowInfo&, CefRefPtr<CefClient>&, CefBrowserSettings&, CefRefPtr<CefDictionaryValue>&, bool*) override
		{
			return true;
		}

		void OnAfterCreated(CefRefPtr<CefBrowser> browser) override
		{
			m_browser = browser;
			if (!m_done) browser->GetHost()->CloseBrowser(true); // Cancelled while the browser was being created.
		}

		void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

		// CefRequestHandler: the same frame rules as the visible card, and blocked URLs never open the system browser.
		bool OnBeforeBrowse(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request, bool, bool) override
		{
			return request == nullptr || !AllowedNavigation(frame, request->GetURL());
		}

		bool OnOpenURLFromTab(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, const CefString& target_url, cef_window_open_disposition_t, bool) override
		{
			return !AllowedNavigation(frame, target_url);
		}

		void OnRenderProcessTerminated(CefRefPtr<CefBrowser>, TerminationStatus, int, const CefString&) override
		{
			Finish(RenderCheckStatus::NotRun, "the renderer process for the page ended early.");
		}

		// CefLoadHandler
		void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int http_status_code) override
		{
			if (!m_done || frame == nullptr) return;
			if (!frame->IsMain())
			{
				if (m_pageFrame == nullptr && frame->GetURL().ToString() == uam::cef::kHtmlViewFrameUrl) m_pageFrame = frame;
				return;
			}
			if (m_delivered) return;
			if (http_status_code != 200 || m_pageFrame == nullptr)
			{
				Finish(RenderCheckStatus::NotRun, "the sandboxed frame could not be loaded.");
				return;
			}
			m_delivered = true;
			// The same delivery as the visible card: the host page replaces itself with this document.
			const nlohmann::json message = {{"type", "uam-html-view"}, {"html", m_html}};
			frame->ExecuteJavaScript("document.querySelector('iframe').contentWindow.postMessage(" + message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + ",'*');", frame->GetURL(), 0);
			CefRefPtr<RenderCheckClient> self(this);
			CefPostDelayedTask(TID_UI, new ClosureTask([self] { self->RequestReceipt(); }), uam::html_view::kRenderCheckSettleMs);
		}

		void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, ErrorCode error_code, const CefString&, const CefString& failed_url) override
		{
			if (error_code == ERR_ABORTED || frame == nullptr) return;
			if (frame->IsMain() || failed_url.ToString() == uam::cef::kHtmlViewFrameUrl)
				Finish(RenderCheckStatus::NotRun, "the sandboxed frame could not be loaded.");
		}

		// CefDisplayHandler
		bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t level, const CefString& message, const CefString& source, int line) override
		{
			if (!m_done) return true;
			const std::string text = message.ToString();
			if (level == LOGSEVERITY_INFO && text == m_nonce) Finish(RenderCheckStatus::Completed);
			else if (++m_consoleMessages >= uam::html_view::kMaxRenderCheckConsoleMessages) Finish(RenderCheckStatus::NotRun, "the page logged too many console messages and was stopped.");
			else if (level >= LOGSEVERITY_ERROR && source.ToString() != uam::cef::kHtmlViewProbeUrl) uam::html_view::AddRenderCheckError(m_result, text, line);
			return true; // Page text never goes to UAM's own logs.
		}

	  private:
		bool AllowedNavigation(CefRefPtr<CefFrame> frame, const CefString& url) const
		{
			return uam::cef::DecideNavigation(frame && frame->IsMain(), url.ToString(), std::string(uam::cef::kHtmlViewProbeUrl)) == uam::cef::NavigationDecision::Allow;
		}

		/// <summary>This script runs only after the page's own work yields, so a page that never yields reaches the timeout instead.</summary>
		/// ponytail: a page replacing console.info also times out, which only costs that page its own result; a renderer round trip would avoid it.
		void RequestReceipt()
		{
			if (!m_done || m_pageFrame == nullptr) return;
			m_pageFrame->ExecuteJavaScript("console.info('" + m_nonce + "')", m_pageFrame->GetURL(), 0);
		}

		std::weak_ptr<HiddenBrowserRenderChecker> m_checker;
		std::string m_owner;
		std::string m_html;
		RenderCheckDone m_done;
		std::string m_nonce;
		RenderCheckResult m_result;
		CefRefPtr<CefBrowser> m_browser;
		CefRefPtr<CefFrame> m_pageFrame;
		bool m_delivered = false;
		std::size_t m_consoleMessages = 0;

		IMPLEMENT_REFCOUNTING(RenderCheckClient);
	};

	class HiddenBrowserRenderChecker final : public uam::html_view::RenderChecker, public std::enable_shared_from_this<HiddenBrowserRenderChecker>
	{
	  public:
		void Start(std::string owner, std::string html, RenderCheckDone done) override
		{
			CEF_REQUIRE_UI_THREAD();
			const char* refused = m_stopped ? "the app is closing." : m_active.size() >= uam::html_view::kMaxRenderChecks ? "too many pages are being checked at once." : nullptr;
			const CefRefPtr<CefRequestContext> context = refused ? nullptr : IsolatedContext();
			if (refused || !context)
			{
				RenderCheckResult result;
				result.not_run_reason = refused ? refused : "the hidden browser could not be isolated from the app.";
				done(std::move(result));
				return;
			}
			CefRefPtr<RenderCheckClient> client = new RenderCheckClient(weak_from_this(), std::move(owner), std::move(html), std::move(done));
			m_active.push_back(client);
			if (!client->Begin(context)) Forget(client.get());
		}

		void Cancel(std::string_view owner) override
		{
			CEF_REQUIRE_UI_THREAD();
			for (const CefRefPtr<RenderCheckClient>& client : std::vector(m_active)) // Finish can re-enter Forget.
				if (owner.empty() || client->Owner() == owner) client->Finish(RenderCheckStatus::NotRun, "it was cancelled.");
			if (owner.empty()) m_stopped = true;
			ReleaseContextWhenStopped();
		}

		bool Idle() const override { return m_active.empty(); }

		void Forget(const RenderCheckClient* client)
		{
			std::erase_if(m_active, [client](const CefRefPtr<RenderCheckClient>& active) { return active.get() == client; });
			ReleaseContextWhenStopped();
		}

	  private:
		/// <summary>A context of their own keeps stuck pages out of the app's renderer pool, where an open card could keep one alive. Created once, on first use.</summary>
		CefRefPtr<CefRequestContext> IsolatedContext()
		{
			if (m_context) return m_context;
			const CefRefPtr<CefSchemeHandlerFactory> factory = uam::cef::UiSchemeHandlerFactory();
			const CefRefPtr<CefRequestContext> context = factory ? CefRequestContext::CreateContext(CefRequestContextSettings{}, nullptr) : nullptr;
			if (!context || !context->RegisterSchemeHandlerFactory("uam", "app", factory)) return nullptr;
			CefRefPtr<CefValue> network_prediction = CefValue::Create();
			network_prediction->SetInt(uam::cef::kNetworkPredictionNever);
			CefString error;
			if (!context->SetPreference(uam::cef::kNetworkPredictionPreference, network_prediction, error)) return nullptr; // Never run a page with prediction on.
			m_context = context;
			return m_context;
		}

		void ReleaseContextWhenStopped()
		{
			if (m_stopped && m_active.empty()) m_context = nullptr; // Before CefShutdown.
		}

		CefRefPtr<CefRequestContext> m_context;
		bool m_stopped = false;
		std::vector<CefRefPtr<RenderCheckClient>> m_active; // Until OnBeforeClose, so closing browsers still count towards the limit.
	};

	void RenderCheckClient::OnBeforeClose(CefRefPtr<CefBrowser>)
	{
		Finish(RenderCheckStatus::NotRun, "the hidden browser closed early.");
		m_browser = nullptr;
		m_pageFrame = nullptr;
		if (const std::shared_ptr<HiddenBrowserRenderChecker> checker = m_checker.lock()) checker->Forget(this);
	}
} // namespace

std::shared_ptr<uam::html_view::RenderChecker> uam::cef::CreateHtmlViewRenderChecker()
{
	return std::make_shared<HiddenBrowserRenderChecker>();
}
