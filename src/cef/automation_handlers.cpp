#include "app/automation_api_service.h"
#include "app/chat_domain_service.h"
#include "cef/cef_push.h"
#include "cef/uam_query_handler.h"

namespace
{
	/// <summary>Replaces a send's reply, undoing the launch if the send is refused.</summary>
	class AutomationLaunchReply final : public CefMessageRouterBrowserSide::Callback
	{
	  public:
		using Failed = std::function<void(int, const std::string&)>;
		AutomationLaunchReply(CefRefPtr<Callback> target, std::string response, Failed failed) : m_target(target), m_response(std::move(response)), m_failed(std::move(failed)) {}
		void Success(const CefString&) override { m_target->Success(m_response); }
		void Success(const void*, size_t) override { m_target->Success(m_response); }
		void Failure(int status, const CefString& error) override { m_failed(status, error.ToString()); }

	  private:
		CefRefPtr<Callback> m_target;
		std::string m_response;
		Failed m_failed;
		IMPLEMENT_REFCOUNTING(AutomationLaunchReply);
	};
}

void UamQueryHandler::LaunchAutomationChat(CefRefPtr<CefBrowser> browser, const std::string& chat_id, std::string response, CefRefPtr<Callback> cb, const std::string& origin_request_id)
{
	// Only the HTTP caller needs telling which chat failed and that it is in UAM.
	const bool from_desktop = !origin_request_id.empty();
	std::string error;
	const std::optional<uam::AutomationChatLaunch> launch = uam::AutomationApiService::BeginLaunch(m_app, chat_id, &error);
	if (!launch)
	{
		ChatSession* chat = ChatDomainService().FindChatById(m_app, chat_id);
		if (chat == nullptr || !chat->AwaitsAutomationApproval())
		{
			cb->Failure(409, error);
			return;
		}
		// The chat stays held, so it must be visible and the caller must be told which one.
		ChatDomainService().MarkChatNeedsAttention(m_app, chat_id);
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Failure(409, from_desktop ? error : "The chat " + chat_id + " could not start and is waiting in UAM: " + error);
		return;
	}
	const std::weak_ptr<void> lifetime = m_asyncLifetime;
	nlohmann::json send = {{"chatId", chat_id}, {"text", launch->text}, {"goalMode", true}, {"goalId", launch->goal_id}};
	// Carries the desktop request ID, so the sent message clears its "Sending" state as any composer send does.
	if (!origin_request_id.empty()) send["_uamOriginRequestId"] = origin_request_id;
	HandleSendAcpPrompt(browser, send,
	    new AutomationLaunchReply(cb, std::move(response), [this, lifetime, browser, cb, chat_id, from_desktop, launch = *launch](int status, const std::string& reason)
	    {
		    const bool waiting = !lifetime.expired() && uam::AutomationApiService::AbandonLaunch(m_app, chat_id, launch);
		    if (!lifetime.expired()) uam::PushStateUpdateIfChanged(browser, m_app);
		    if (from_desktop) cb->Failure(status, waiting ? reason : "This chat may not have started: " + reason);
		    else cb->Failure(status, "The chat " + chat_id + (waiting ? " could not start and is waiting in UAM: " : " may not have started: ") + reason);
	    }));
}

void UamQueryHandler::HandleAutomationHttpRequest(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	uam::AutomationApiResponse response = uam::AutomationApiService::Handle(m_app, {.method = payload.value("method", ""), .path = payload.value("path", ""), .token_hash = payload.value("tokenHash", ""), .body = payload.value("body", "")});
	if (response.status != 200)
	{
		cb->Failure(response.status, response.body.value("error", "Automation request refused."));
		return;
	}
	if (response.launch_chat_id.empty())
	{
		// A held chat is saved directly, which the state tracker does not see.
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success(response.body.dump());
		return;
	}
	response.body["status"] = "running";
	LaunchAutomationChat(browser, response.launch_chat_id, response.body.dump(), cb);
}

void UamQueryHandler::HandleApproveAutomationChat(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	LaunchAutomationChat(browser, chat_id, nlohmann::json{{"chatId", chat_id}}.dump(), cb, payload.value("_uamOriginRequestId", ""));
}

void UamQueryHandler::HandleGetAutomationTokens(CefRefPtr<CefBrowser>, const nlohmann::json&, CefRefPtr<Callback> cb)
{
	std::string error;
	const std::optional<nlohmann::json> tokens = uam::AutomationApiService::ListTokens(m_app, &error);
	if (tokens) cb->Success(nlohmann::json{{"tokens", *tokens}}.dump());
	else cb->Failure(500, error);
}

void UamQueryHandler::HandleCreateAutomationToken(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	std::string error;
	const std::optional<nlohmann::json> created = uam::AutomationApiService::CreateToken(m_app, payload, &error);
	if (created) cb->Success(created->dump());
	else cb->Failure(400, error);
}

void UamQueryHandler::HandleRevokeAutomationToken(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	std::string error;
	if (uam::AutomationApiService::RevokeToken(m_app, payload.value("id", ""), &error)) cb->Success("{}");
	else cb->Failure(409, error);
}
