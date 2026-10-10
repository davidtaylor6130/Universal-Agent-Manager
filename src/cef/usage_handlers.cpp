#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_async.h"
#include "common/usage/usage_collector.h"
#include "common/utils/nlohmann_json_utils.h"

using namespace uam::query_handler_async;

void UamQueryHandler::HandleGetUsageHistory(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	uam::usage::HistoryRequest request;
	std::string error;
	if (!uam::usage::ParseHistoryRequest(payload, request, error)) { cb->Failure(400, error); return; }
	if (request.execution_host_id != "local") { cb->Failure(400, "Usage history is not available on remote hosts yet."); return; }
	if (!m_usageHistory || m_usageHistory->DataRoot() != m_app.data_root) { cb->Failure(409, "Usage storage changed. Restart UAM to collect the selected data root."); return; }
	const nlohmann::json* days_value = uam::nlohmann_json::FindField(payload, "windowDays");
	if (!days_value || !days_value->is_number_integer() || (*days_value != 1 && *days_value != 7 && *days_value != 30 && *days_value != 90)) { cb->Failure(400, "Choose a valid usage period."); return; }
	const int days = days_value->get<int>();
	const std::shared_ptr<uam::usage::Collector> history = m_usageHistory;
	const std::weak_ptr<void> lifetime = m_asyncLifetime;
	if (!m_usageQueries) m_usageQueries = CefThread::CreateThread("uam-usage-queries");
	if (!m_usageQueries) { cb->Failure(503, "The usage query worker is unavailable."); return; }
	if (!m_usageQueries->GetTaskRunner()->PostTask(new CefQueryWorkerTask(lifetime, cb, [history, request, days]()
	{
		nlohmann::json result = history->Query(days, request.prices);
		if (result.value("cancelled", false)) return AsyncFailure(409, "Usage history loading was cancelled.");
		if (result.contains("error")) return AsyncFailure(400, result["error"].get<std::string>());
		std::string body = result.dump();
		if (body.size() > 2 * 1024 * 1024) return AsyncFailure(413, "Usage results exceed the display limit. Choose a shorter time range.");
		return AsyncCefResult{true, 200, std::move(body), ""};
	}))) cb->Failure(503, "The usage query worker is unavailable.");
}
