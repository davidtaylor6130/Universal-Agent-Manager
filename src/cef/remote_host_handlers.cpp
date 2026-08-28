#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_async.h"

#include "app/persistence_coordinator.h"
#include "cef/cef_push.h"
#include "common/config/execution_host_config.h"
#include "common/constants/app_constants.h"
#include "common/platform/platform_services.h"
#include "common/utils/io_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"
#include "remote/runner_bootstrap.h"
#include "remote/runner_proxy.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>

namespace
{
	bool RemoteHostFromPayload(const nlohmann::json& payload, ExecutionHost& host,
	                           std::string& error)
	{
		host.id = payload.value("id", "");
		host.label = payload.value("label", "");
		host.ssh_alias = payload.value("sshAlias", "");
		host.runner_status = "uninstalled";
		return uam::execution_hosts::NormalizeRemote(host, &error);
	}

	bool BuildPlan(const ExecutionHost& host, uam::remote::BootstrapPlan& plan,
	               std::string& error)
	{
		const std::filesystem::path runner = uam::remote::PackagedRunnerPath();
		const auto checksum = uam::io::ReadFirstTextFileLine(runner.parent_path() /
		                                                      "uam-runner.sha256");
		if (!checksum)
		{
			error = "The packaged UAM remote runner checksum is missing.";
			return false;
		}
		std::string version = uam::constants::kAppVersion;
		if (!version.empty() && (version.front() == 'V' || version.front() == 'v'))
			version.erase(version.begin());
		std::string nonce =
		    PlatformServicesFactory::Instance().process_service.GenerateUuid();
		if (nonce.empty()) nonce = uam::time::SteadyEpochNanosecondsTokenNow();
		return uam::remote::BuildBootstrapPlan(
		    host.ssh_alias, runner, version, uam::strings::Trim(*checksum), nonce, plan, &error);
	}

	auto FindMutableHost(std::vector<ExecutionHost>& hosts, const std::string& id)
	{
		return std::ranges::find_if(hosts, [&id](const ExecutionHost& value)
		                           { return value.id == id; });
	}
}

void UamQueryHandler::HandlePreviewRemoteHost(CefRefPtr<CefBrowser>,
	                                           const nlohmann::json& payload,
	                                           CefRefPtr<Callback> cb)
{
	ExecutionHost host;
	std::string error;
	uam::remote::BootstrapPlan plan;
	if (!RemoteHostFromPayload(payload, host, error) || !BuildPlan(host, plan, error))
	{
		cb->Failure(400, error);
		return;
	}
	cb->Success(nlohmann::json{{"host", uam::execution_hosts::Serialize({host})[1]},
	                           {"preview", uam::remote::BootstrapPlanPreview(plan)}}.dump());
}

void UamQueryHandler::HandleInstallRemoteHost(CefRefPtr<CefBrowser> browser,
	                                           const nlohmann::json& payload,
	                                           CefRefPtr<Callback> cb)
{
	ExecutionHost host;
	std::string error;
	uam::remote::BootstrapPlan plan;
	if (!RemoteHostFromPayload(payload, host, error) || !BuildPlan(host, plan, error))
	{
		cb->Failure(400, error);
		return;
	}
	if (const auto collision = FindMutableHost(m_app.settings.execution_hosts, host.id);
	    collision != m_app.settings.execution_hosts.end() &&
	    collision->ssh_alias != host.ssh_alias)
	{
		cb->Failure(409, "Another remote host already uses this id.");
		return;
	}
	const AppSettings previous = m_app.settings;
	host.runner_status = "installing";
	if (auto existing = FindMutableHost(m_app.settings.execution_hosts, host.id);
	    existing != m_app.settings.execution_hosts.end())
		*existing = host;
	else
		m_app.settings.execution_hosts.push_back(host);
	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		m_app.settings = previous;
		cb->Failure(500, "Failed to persist the remote host before setup.");
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);

	auto result = std::make_shared<uam::remote::BootstrapResult>();
	uam::query_handler_async::RunAsyncCefQuery(
	    cb,
	    [plan = std::move(plan), result]() mutable
	    {
		    *result = uam::remote::ExecuteBootstrapPlan(plan);
		    return result->ok
		        ? uam::query_handler_async::AsyncSuccess(nlohmann::json{{"ok", true}})
		        : uam::query_handler_async::AsyncFailure(500, result->error);
	    },
	    [this, browser, host_id = host.id, result](
	        uam::query_handler_async::AsyncCefResult& response)
	    {
		    auto found = FindMutableHost(m_app.settings.execution_hosts, host_id);
		    if (found == m_app.settings.execution_hosts.end()) return;
		    found->runner_status = result->ok ? "ready" : "error";
		    if (result->ok)
		    {
			    std::string version = uam::constants::kAppVersion;
			    if (!version.empty() && (version.front() == 'V' || version.front() == 'v'))
				    version.erase(version.begin());
			    found->runner_version = std::move(version);
			    found->platform = result->platform;
			    found->architecture = result->architecture;
			    found->last_seen_at = uam::time::IsoUtcTimestampNow();
		    }
		    if (!PersistenceCoordinator().SaveSettings(m_app))
			    response = uam::query_handler_async::AsyncFailure(
			        500, "Remote setup finished, but its status could not be saved.");
		    uam::PushStateUpdateIfChanged(browser, m_app);
	    });
}

void UamQueryHandler::HandleRemoveRemoteHost(CefRefPtr<CefBrowser> browser,
	                                          const nlohmann::json& payload,
	                                          CefRefPtr<Callback> cb)
{
	const std::string id = uam::strings::Trim(payload.value("id", ""));
	if (id.empty() || id == uam::execution_hosts::kLocalHostId)
	{
		cb->Failure(400, "Select a remote host to remove.");
		return;
	}
	if (std::ranges::any_of(m_app.chats, [&id](const ChatSession& chat)
	                       { return chat.execution_host_id == id; }))
	{
		cb->Failure(409, "Move or delete chats assigned to this host before removing it.");
		return;
	}
	const AppSettings previous = m_app.settings;
	std::erase_if(m_app.settings.execution_hosts,
	              [&id](const ExecutionHost& host) { return host.id == id; });
	if (m_app.settings.execution_hosts.size() == previous.execution_hosts.size())
	{
		cb->Failure(404, "Remote host not found.");
		return;
	}
	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		m_app.settings = previous;
		cb->Failure(500, "Failed to remove the remote host.");
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}
