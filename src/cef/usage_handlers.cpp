#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_async.h"
#include "common/usage/multi_host_history.h"
#include "common/usage/history_transport.h"
#include "common/config/execution_host_config.h"
#include "common/utils/nlohmann_json_utils.h"

using namespace uam::query_handler_async;

namespace
{
	std::string UsageOwner(nlohmann::json payload, const uam::AppState& app)
	{
		payload.erase("snapshotId"); payload.erase("offset");
		payload["dataRoot"] = app.data_root.string();
		nlohmann::json connections = nlohmann::json::array();
		for (const ExecutionHost& host : app.settings.execution_hosts)
		{
			if (payload.value("executionHostId", std::string("local")) != "*" && payload.value("executionHostId", std::string("local")) != host.id) continue;
			connections.push_back({host.id, host.transport, host.ssh_alias, host.platform, host.architecture, host.runner_version,
				host.runner_protocol_version, host.runner_directory, host.instruction_file, host.instruction_text ? nlohmann::json(*host.instruction_text) : nlohmann::json(nullptr)});
		}
		payload["connections"] = std::move(connections);
		return payload.dump();
	}
}


void UamQueryHandler::RefreshUsageTargets()
{
	if (m_usageHistory) m_usageHistory->SetHosts(m_app.settings.execution_hosts);
}

void UamQueryHandler::HandleGetUsageHistory(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	if (!browser) { cb->Failure(400, "Usage request has no browser owner."); return; }
	uam::usage::HistoryRequest request;
	std::string error;
	if (!uam::usage::ParseHistoryRequest(payload, request, error)) { cb->Failure(400, error); return; }
	if (request.execution_host_id != uam::usage::kAllMachinesHostId && !uam::execution_hosts::Find(m_app.settings.execution_hosts, request.execution_host_id)) { cb->Failure(404, "Usage machine is no longer configured."); return; }
	if (!m_usageHistory || m_usageHistory->DataRoot() != m_app.data_root) { cb->Failure(409, "Usage storage changed. Restart UAM to collect the selected data root."); return; }
	const nlohmann::json* days_value = uam::nlohmann_json::FindField(payload, "windowDays");
	if (!days_value || !days_value->is_number_integer() || (*days_value != 0 && *days_value != 1 && *days_value != 7 && *days_value != 30 && *days_value != 90)) { cb->Failure(400, "Choose a valid usage period."); return; }
	const int days = days_value->get<int>();
	if (days == 0 && request.start_ms != 0) { cb->Failure(400, "All Time accounting must start at the beginning."); return; }
	const bool continuation = payload.contains("snapshotId") || payload.contains("offset");
	if (continuation && (!payload.contains("snapshotId") || !payload["snapshotId"].is_string() || payload["snapshotId"].get_ref<const std::string&>().empty() || payload["snapshotId"].get_ref<const std::string&>().size() > 32 || !payload.contains("offset") || !payload["offset"].is_number_unsigned()))
	{ cb->Failure(400, "Usage continuation is invalid."); return; }
	const std::string owner = UsageOwner(payload, m_app);
	const int browser_id = browser->GetIdentifier();
	const std::string snapshot_id = continuation ? payload["snapshotId"].get<std::string>() : "";
	const std::size_t offset = continuation ? payload["offset"].get<std::size_t>() : 0;
	RefreshUsageTargets();
	if (!m_usageTransport) m_usageTransport = std::make_shared<uam::usage::HistoryTransport>();
	const std::shared_ptr<uam::usage::HistoryTransport> transport = m_usageTransport;
	const std::shared_ptr<uam::usage::MultiHostHistory> history = m_usageHistory;
	const std::weak_ptr<void> lifetime = m_asyncLifetime;
	if (!m_usageQueries) m_usageQueries = CefThread::CreateThread("uam-usage-queries");
	if (!m_usageQueries) { cb->Failure(503, "The usage query worker is unavailable."); return; }
	if (!m_usageQueries->GetTaskRunner()->PostTask(new CefQueryWorkerTask(lifetime, cb, [history, transport, request, days, continuation, browser_id, owner, snapshot_id, offset]()
	{
		if (continuation)
		{
			try { return AsyncSuccess(transport->Page(browser_id, owner, snapshot_id, offset)); }
			catch (const std::runtime_error& error) { return AsyncFailure(409, error.what()); }
		}
		nlohmann::json result = history->Query(request.execution_host_id, days, request.prices);
		if (result.value("cancelled", false)) return AsyncFailure(409, "Usage history loading was cancelled.");
		if (result.contains("error")) return AsyncFailure(400, result["error"].get<std::string>());
		return AsyncSuccess(transport->Start(browser_id, owner, request.refresh_id, result.dump()));
	}, [this, owner, payload](AsyncCefResult& result)
	{
		if (UsageOwner(payload, m_app) != owner || !m_usageHistory || m_usageHistory->DataRoot() != m_app.data_root)
			result = AsyncFailure(409, "Usage machine or storage changed. Refresh to try again.");
	}))) cb->Failure(503, "The usage query worker is unavailable.");
}
