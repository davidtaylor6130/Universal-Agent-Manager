#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_async.h"
#include "common/usage/usage_history_service.h"

using namespace uam::query_handler_async;

void UamQueryHandler::HandleGetUsageHistory(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	uam::usage::HistoryRequest request;
	std::string error;
	if (!uam::usage::ParseHistoryRequest(payload, request, error)) { cb->Failure(400, error); return; }
	if (request.execution_host_id != "local") { cb->Failure(400, "Usage history is not available on remote hosts yet."); return; }
	if (!m_usageHistory) m_usageHistory = std::make_shared<uam::usage::HistoryService>();
	const std::shared_ptr<uam::usage::HistoryService> history = m_usageHistory;
	const std::weak_ptr<void> lifetime = m_asyncLifetime;
	RunAsyncCefQuery(lifetime, cb, [history, request, lifetime]()
	{
		nlohmann::json result = history->Refresh(request.refresh_id, request.start_ms, request.end_ms, request.prices, [lifetime]() { return lifetime.expired(); });
		if (result.value("cancelled", false)) return AsyncFailure(409, "Usage history loading was cancelled.");
		if (result.contains("error")) return AsyncFailure(400, result["error"].get<std::string>());
		std::string body = result.dump();
		if (body.size() > 2 * 1024 * 1024) return AsyncFailure(413, "Usage results exceed the display limit. Choose a shorter time range.");
		return AsyncCefResult{true, 200, std::move(body), ""};
	});
}
