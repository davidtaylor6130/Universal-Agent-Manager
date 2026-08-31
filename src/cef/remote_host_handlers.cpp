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
#include "remote/runner_client.h"
#include "remote/runner_protocol.h"
#include "remote/runner_proxy.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace
{
	std::unordered_map<std::string, std::string> g_remote_install_tokens;

	bool RemoteHostFromPayload(const nlohmann::json& payload, ExecutionHost& host,
	                           std::string& error)
	{
		host.id = payload.value("id", "");
		host.label = payload.value("label", "");
		host.ssh_alias = payload.value("sshAlias", "");
		host.runner_directory = payload.value("runnerDirectory", "");
		host.runner_status = "uninstalled";
		return uam::execution_hosts::NormalizeRemote(host, &error);
	}

	bool BuildPlan(const ExecutionHost& host, uam::remote::BootstrapPlan& plan,
	               std::string& error)
	{
		const std::filesystem::path artifact_root =
		    uam::remote::PackagedRunnerPath().parent_path();
		std::vector<uam::remote::RunnerArtifact> artifacts;
		for (const auto& [platform, architecture, executable] :
		     std::vector<std::tuple<std::string, std::string, std::string>>{
		         {"linux", "arm64", "uam-runner"},
		         {"linux", "x86_64", "uam-runner"},
		         {"windows", "x86_64", "uam-runner.exe"}})
		{
			const std::filesystem::path directory = artifact_root /
			    (platform + "-" + architecture);
			const std::filesystem::path runner = directory / executable;
			const auto checksum = uam::io::ReadFirstTextFileLine(directory /
			                                                      "uam-runner.sha256");
			std::error_code status_error;
			const bool has_runner = std::filesystem::is_regular_file(runner, status_error) &&
			                        !status_error;
			if (!has_runner && !checksum) continue;
			if (!has_runner || !checksum)
			{
				error = "A packaged UAM remote runner or checksum is missing.";
				return false;
			}
			artifacts.push_back({platform, architecture, runner,
			                     uam::strings::Trim(*checksum)});
		}
		std::string version = uam::constants::kAppVersion;
		if (!version.empty() && (version.front() == 'V' || version.front() == 'v'))
			version.erase(version.begin());
		std::string nonce =
		    PlatformServicesFactory::Instance().process_service.GenerateUuid();
		if (nonce.empty()) nonce = uam::time::SteadyEpochNanosecondsTokenNow();
		return uam::remote::BuildBootstrapPlan(host.ssh_alias, version, nonce,
		                                         std::move(artifacts), plan, &error,
		                                         host.runner_directory);
	}

	auto FindMutableHost(std::vector<ExecutionHost>& hosts, const std::string& id)
	{
		return std::ranges::find_if(hosts, [&id](const ExecutionHost& value)
		                           { return value.id == id; });
	}

	bool HostHasActiveRuntimeWork(const uam::AppState& app, const std::string& host_id)
	{
		const auto chat_uses_host = [&](const std::string& chat_id)
		{
			const auto chat = std::ranges::find(app.chats, chat_id, &ChatSession::id);
			return chat != app.chats.end() && chat->execution_host_id == host_id;
		};
		if (std::ranges::any_of(app.chats, [&](const ChatSession& chat)
		    { return chat.execution_host_id == host_id && chat.remote_turn_reconnect_pending; }))
			return true;
		if (std::ranges::any_of(app.acp_sessions, [&](const auto& session)
		    { return session != nullptr && session->running && chat_uses_host(session->chat_id); }))
			return true;
		return std::ranges::any_of(app.cli_terminals, [&](const auto& terminal)
		{
			return terminal != nullptr && terminal->running &&
			       (chat_uses_host(terminal->attached_chat_id) ||
			        chat_uses_host(terminal->frontend_chat_id));
		});
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
	if (!RemoteHostFromPayload(payload, host, error))
	{
		cb->Failure(400, error);
		return;
	}
	const auto collision = FindMutableHost(m_app.settings.execution_hosts, host.id);
	if (collision != m_app.settings.execution_hosts.end() &&
	    collision->ssh_alias != host.ssh_alias)
	{
		cb->Failure(409, "Another remote host already uses this id.");
		return;
	}
	if (g_remote_install_tokens.contains(host.id))
	{
		cb->Failure(409, "This remote helper is already being installed.");
		return;
	}
	if (HostHasActiveRuntimeWork(m_app, host.id))
	{
		cb->Failure(409, "Stop or finish work running on this host before updating its helper.");
		return;
	}
	const AppSettings previous = m_app.settings;
	std::optional<ExecutionHost> previous_host;
	if (collision != m_app.settings.execution_hosts.end()) previous_host = *collision;
	uam::remote::BootstrapPlan plan;
	if (!BuildPlan(host, plan, error))
	{
		cb->Failure(400, error);
		return;
	}
	if (previous_host.has_value())
	{
		plan.previous_platform = previous_host->platform;
		plan.previous_version = previous_host->runner_version;
		plan.previous_runner_directory = previous_host->runner_directory;
		plan.previous_protocol_version = previous_host->runner_protocol_version > 0
		    ? previous_host->runner_protocol_version
		    : uam::remote::kRunnerProtocolVersion;
	}
	host.runner_status = "installing";
	if (auto existing = FindMutableHost(m_app.settings.execution_hosts, host.id);
	    existing != m_app.settings.execution_hosts.end())
	{
		host.platform = existing->platform;
		host.architecture = existing->architecture;
		host.runner_version = existing->runner_version;
		host.runner_protocol_version = existing->runner_protocol_version;
		host.last_seen_at = existing->last_seen_at;
		*existing = host;
	}
	else
		m_app.settings.execution_hosts.push_back(host);
	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		m_app.settings = previous;
		cb->Failure(500, "Failed to persist the remote host before setup.");
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	const std::string install_nonce = plan.nonce;
	g_remote_install_tokens[host.id] = install_nonce;

	auto result = std::make_shared<uam::remote::BootstrapResult>();
	auto install_plan = std::make_shared<uam::remote::BootstrapPlan>(std::move(plan));
	uam::query_handler_async::RunAsyncCefQuery(
	    cb,
	    [install_plan, result]()
	    {
		    const uam::remote::BootstrapPlan& plan = *install_plan;
		    *result = uam::remote::ExecuteBootstrapPlan(plan);
		    if (result->ok)
		    {
			    uam::remote::RunnerClient client(
			        PlatformServicesFactory::Instance().process_service,
			        uam::remote::SshBridgeArgv(plan.ssh_alias, result->platform,
			                                   plan.version, plan.runner_directory),
			        plan.version);
			    if (!client.Connect(&result->error))
			    {
				    std::string rollback_error;
				    result->ok = false;
				    const bool rolled_back = uam::remote::FinalizeBootstrapPlan(
				        plan, *result, false, &rollback_error);
				    if (!rolled_back && !rollback_error.empty())
					    result->error += " Rollback failed: " + rollback_error;
				    else if (!plan.previous_version.empty())
				    {
					    uam::remote::RunnerClient previous_client(
					        PlatformServicesFactory::Instance().process_service,
					        uam::remote::SshBridgeArgv(
					            plan.ssh_alias, plan.previous_platform,
					            plan.previous_version, plan.previous_runner_directory),
					        plan.previous_version);
					    if (!previous_client.Connect(&rollback_error))
						    result->error += " Previous helper verification failed: " +
						                     rollback_error;
				    }
			    }
		    }
		    return result->ok
		        ? uam::query_handler_async::AsyncSuccess(nlohmann::json{{"ok", true}})
		        : uam::query_handler_async::AsyncFailure(500, result->error);
	    },
	    [this, browser, host_id = host.id, install_nonce,
	     ssh_alias = host.ssh_alias,
	     runner_directory = host.runner_directory, previous_host = std::move(previous_host),
	     install_plan, result](
	        uam::query_handler_async::AsyncCefResult& response)
	    {
		    const auto rollback_remote = [&]()
		    {
			    std::string rollback_error;
			    if (result->ok && !uam::remote::FinalizeBootstrapPlan(
			                          *install_plan, *result, false, &rollback_error))
				    return rollback_error.empty() ? std::string("Remote helper rollback failed.")
				                                  : "Remote helper rollback failed: " + rollback_error;
			    return std::string{};
		    };
		    const auto restore_host = [&]()
		    {
			    auto current = FindMutableHost(m_app.settings.execution_hosts, host_id);
			    if (previous_host.has_value())
			    {
				    if (current != m_app.settings.execution_hosts.end()) *current = *previous_host;
				    else m_app.settings.execution_hosts.push_back(*previous_host);
			    }
			    else if (current != m_app.settings.execution_hosts.end())
				    m_app.settings.execution_hosts.erase(current);
		    };
		    const auto operation = g_remote_install_tokens.find(host_id);
		    if (operation == g_remote_install_tokens.end() || operation->second != install_nonce)
		    {
			    const std::string rollback_error = rollback_remote();
			    response = uam::query_handler_async::AsyncFailure(
			        409, "A stale remote helper install result was ignored." +
			                 (rollback_error.empty() ? std::string{} : " " + rollback_error));
			    return;
		    }
		    g_remote_install_tokens.erase(operation);
		    auto found = FindMutableHost(m_app.settings.execution_hosts, host_id);
		    if (found == m_app.settings.execution_hosts.end())
		    {
			    const std::string rollback_error = rollback_remote();
			    response = uam::query_handler_async::AsyncFailure(
			        409, "The remote host was removed before setup finished." +
			                 (rollback_error.empty() ? std::string{} : " " + rollback_error));
			    return;
		    }
		    if (found->runner_status != "installing" || found->ssh_alias != ssh_alias ||
		        found->runner_directory != runner_directory)
		    {
			    const std::string rollback_error = rollback_remote();
			    response = uam::query_handler_async::AsyncFailure(
			        409, "A stale remote helper install result was ignored." +
			                 (rollback_error.empty() ? std::string{} : " " + rollback_error));
			    return;
		    }
		    if (!result->ok)
		    {
			    restore_host();
		    }
		    else
		    {
			    found->runner_status = "ready";
			    std::string version = uam::constants::kAppVersion;
			    if (!version.empty() && (version.front() == 'V' || version.front() == 'v'))
				    version.erase(version.begin());
			    found->runner_version = std::move(version);
			    found->platform = result->platform;
			    found->architecture = result->architecture;
			    found->last_seen_at = uam::time::IsoUtcTimestampNow();
			    found->runner_protocol_version = uam::remote::kRunnerProtocolVersion;
		    }
		    if (!PersistenceCoordinator().SaveSettings(m_app))
		    {
			    const std::string rollback_error = rollback_remote();
			    restore_host();
			    const bool rollback_saved = PersistenceCoordinator().SaveSettings(m_app);
			    response = uam::query_handler_async::AsyncFailure(
			        500, "Remote setup was rolled back because its status could not be saved." +
			                 (rollback_error.empty() ? std::string{} : " " + rollback_error) +
			                 (rollback_saved ? std::string{} : " The previous host status also could not be saved."));
		    }
		    else if (result->ok)
		    {
			    std::string cleanup_error;
			    if (!uam::remote::FinalizeBootstrapPlan(
			            *install_plan, *result, true, &cleanup_error))
			    {
				    response = uam::query_handler_async::AsyncSuccess(
				        nlohmann::json{{"ok", true},
				                       {"warning", cleanup_error.empty()
				                            ? "The old remote helper backup could not be removed."
				                            : cleanup_error}});
			    }
		    }
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
	if (g_remote_install_tokens.contains(id))
	{
		cb->Failure(409, "Wait for the remote helper install to finish before removing it.");
		return;
	}
	if (std::ranges::any_of(m_app.chats, [&id](const ChatSession& chat)
	                       { return chat.execution_host_id == id; }))
	{
		cb->Failure(409, "Move or delete chats assigned to this host before removing it.");
		return;
	}
	if (std::ranges::any_of(m_app.folders, [&id](const ChatFolder& folder)
	                       { return folder.execution_host_id == id; }))
	{
		cb->Failure(409, "Delete workspaces assigned to this host before removing it.");
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

void UamQueryHandler::HandleListRemoteDirectories(CefRefPtr<CefBrowser> browser,
	                                               const nlohmann::json& payload,
	                                               CefRefPtr<Callback> cb)
{
	const std::string host_id = uam::strings::Trim(payload.value("executionHostId", ""));
	const std::string directory = uam::strings::Trim(payload.value("directory", ""));
	const ExecutionHost* configured = uam::execution_hosts::Find(
	    m_app.settings.execution_hosts, host_id);
	if (configured == nullptr || configured->id == uam::execution_hosts::kLocalHostId)
	{
		cb->Failure(404, "Select a configured remote host.");
		return;
	}
	if (configured->runner_status != "ready")
	{
		cb->Failure(409, "The selected remote helper is not ready.");
		return;
	}
	if (!uam::execution_hosts::IsAbsoluteRemotePath(configured->platform, directory))
	{
		cb->Failure(400, "Enter an absolute path for the selected remote system.");
		return;
	}

	const ExecutionHost host = *configured;
	struct DirectoryResult
	{
		uam::remote::DirectoryListing listing;
		std::string error;
		bool connected = false;
	};
	auto result = std::make_shared<DirectoryResult>();
	uam::query_handler_async::RunAsyncCefQuery(
	    cb, [host, directory, result]()
	    {
		uam::remote::RunnerClient client(
		    PlatformServicesFactory::Instance().process_service,
		    uam::remote::SshBridgeArgv(host.ssh_alias, host.platform,
		                               host.runner_version, host.runner_directory),
		    host.runner_version);
		if (!client.Connect(&result->error))
			return uam::query_handler_async::AsyncFailure(
			    502, result->error.empty() ? "The remote helper could not be reached." : result->error);
		result->connected = true;
		if (!client.ListDirectories(directory, result->listing, &result->error))
			return uam::query_handler_async::AsyncFailure(
			    502, result->error.empty() ? "The remote directory could not be listed." : result->error);
		nlohmann::json directories = nlohmann::json::array();
		for (const auto& [name, path] : result->listing.directories)
			directories.push_back({{"name", name}, {"path", path}});
		return uam::query_handler_async::AsyncSuccess({
		    {"directory", result->listing.directory},
		    {"parentDirectory", result->listing.parent_directory},
		    {"directories", std::move(directories)},
		    {"truncated", result->listing.truncated},
		});
	    },
	    [this, browser, host_id = host.id, result](
	        uam::query_handler_async::AsyncCefResult& response)
	    {
		    auto found = FindMutableHost(m_app.settings.execution_hosts, host_id);
		    if (found == m_app.settings.execution_hosts.end()) return;
		    found->runner_status = result->connected ? "ready" : "error";
		    if (result->connected) found->last_seen_at = uam::time::IsoUtcTimestampNow();
		    if (!PersistenceCoordinator().SaveSettings(m_app) && response.ok)
			    response = uam::query_handler_async::AsyncFailure(
			        500, "Remote helper health changed, but its status could not be saved.");
		    uam::PushStateUpdateIfChanged(browser, m_app);
	    });
}
