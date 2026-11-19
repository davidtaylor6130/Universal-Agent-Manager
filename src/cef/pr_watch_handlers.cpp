#include "cef/uam_query_handler.h"

#include "app/chat_domain_service.h"
#include "app/pr_watch_service.h"
#include "cef/cef_push.h"
#include "common/utils/time_utils.h"

/// The chat's own watch, unwatch and unlink controls; GitHub is read later, off this thread.
void UamQueryHandler::HandleManagePullRequestWatch(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string operation = payload.value("operation", "");
	const auto number = payload.find("number");
	const std::optional<uam::pr_watch::Reference> pr = uam::pr_watch::ParseReference(payload.value("pullRequest", ""),
	    number != payload.end() && number->is_number_integer() ? number->get<int>() : 0);
	if (m_app.pr_watch_service == nullptr || ChatDomainService().FindChatById(m_app, chat_id) == nullptr)
	{
		cb->Failure(400, "The chat is unavailable.");
		return;
	}
	if (!pr.has_value())
	{
		cb->Failure(400, "Enter a github.com pull request link, or owner/name#number.");
		return;
	}
	std::string error;
	if (operation == "watch")
	{
		if (!m_app.pr_watch_service->Watch(m_app, chat_id, *pr, uam::time::SystemEpochMillisecondsNow(), &error))
		{
			cb->Failure(400, error);
			return;
		}
	}
	else if (operation == "unwatch") (void)m_app.pr_watch_service->Unwatch(chat_id, *pr);
	else if (operation == "unlink") (void)m_app.pr_watch_service->Unlink(chat_id, *pr);
	else
	{
		cb->Failure(400, "Unknown pull request watch operation.");
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}
