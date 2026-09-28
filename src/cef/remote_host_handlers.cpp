#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_async.h"

#include "app/persistence_coordinator.h"
#include "cef/cef_push.h"
#include "common/config/execution_host_config.h"
#include "common/constants/app_constants.h"
#include "common/paths/path_utils.h"
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
		const auto lease = app.remote_vcs_operation_leases_by_host_id.find(host_id);
		if (lease != app.remote_vcs_operation_leases_by_host_id.end() && !lease->second.expired())
			return true;
		const auto chat_uses_host = [&](const std::string& chat_id)
		{
			const auto chat = std::ranges::find(app.chats, chat_id, &ChatSession::id);
			return chat != app.chats.end() && chat->execution_host_id == host_id;
		};
		if (std::ranges::any_of(app.chats, [&](const ChatSession& chat)
		    { return chat.execution_host_id == host_id &&
		             (chat.remote_turn_reconnect_pending || chat.remote_stop_cleanup_pending ||
		              chat.remote_restart_pending); }))
			return true;
		if (std::ranges::any_of(app.acp_sessions, [&](const auto& session)
		    { return session != nullptr && chat_uses_host(session->chat_id) &&
		             (session->running || session->remote_stop_pending ||
		              session->remote_stop_unconfirmed || session->reconnect_pending ||
		              session->recovering_remote_turn || session->recovering_remote_process); }))
			return true;
		if (std::ranges::any_of(app.pending_acp_remote_stops, [&](const auto& stop)
		    { return stop != nullptr && chat_uses_host(stop->chat_id); }))
			return true;
		return std::ranges::any_of(app.cli_terminals, [&](const auto& terminal)
		{
			return terminal != nullptr && (terminal->running || terminal->native_session_setup_cancel != nullptr) &&
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
		    : 2;
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
	const bool queued = uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime,
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
			                                   plan.version, plan.runner_directory,
			                                   uam::remote::kRunnerProtocolVersion),
			        plan.version, uam::remote::kRunnerProtocolVersion);
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
					            plan.previous_version, plan.previous_runner_directory,
					            plan.previous_protocol_version),
					        plan.previous_version, plan.previous_protocol_version);
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
	    [this, browser, cb, host_id = host.id, install_nonce,
	     ssh_alias = host.ssh_alias, runner_directory = host.runner_directory,
	     previous_host, install_plan, result](
	        uam::query_handler_async::AsyncCefResult& response)
	    {
		    const auto release_install = [host_id, install_nonce]()
		    {
			    const auto operation = g_remote_install_tokens.find(host_id);
			    if (operation != g_remote_install_tokens.end() && operation->second == install_nonce)
				    g_remote_install_tokens.erase(operation);
		    };
		    const auto restore_host = [this, host_id, previous_host]()
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
		    const auto finalize_remote = [&](bool keep_new_runner, bool restore_previous)
		    {
			    if (!result->ok)
			    {
				    release_install();
				    uam::PushStateUpdateIfChanged(browser, m_app);
				    return;
			    }
			    uam::PushStateUpdateIfChanged(browser, m_app);
			    uam::query_handler_async::AsyncCefResult outcome = response;
			    const auto finalized = std::make_shared<bool>(false);
			    const bool finalization_queued = uam::query_handler_async::RunAsyncCefQuery(
			        m_asyncLifetime, cb,
				[install_plan, result, keep_new_runner, outcome, finalized]() mutable
			        {
				        std::string error;
				        *finalized = uam::remote::FinalizeBootstrapPlan(
				            *install_plan, *result, keep_new_runner, &error);
				        if (!*finalized)
				        {
					        if (keep_new_runner)
						        return uam::query_handler_async::AsyncSuccess({
						            {"ok", true}, {"warning", error.empty()
						                ? "The old remote helper backup could not be removed." : error}});
					        outcome.error += " Remote helper rollback failed." +
					            (error.empty() ? std::string{} : " " + error);
				        }
				        return outcome;
			        },
				 [this, browser, host_id, install_nonce, restore_previous,
			         restore_host, release_install, finalized](
			            uam::query_handler_async::AsyncCefResult& final_response)
			        {
				        const auto operation = g_remote_install_tokens.find(host_id);
				        if (restore_previous && operation != g_remote_install_tokens.end() &&
				            operation->second == install_nonce)
				        {
					        restore_host();
					        if (!*finalized)
					        {
						        const auto current = FindMutableHost(m_app.settings.execution_hosts, host_id);
						        if (current != m_app.settings.execution_hosts.end()) current->runner_status = "error";
					        }
					        if (!PersistenceCoordinator().SaveSettings(m_app))
						        final_response.error += " The previous host status also could not be saved.";
				        }
				        release_install();
				        uam::PushStateUpdateIfChanged(browser, m_app);
			        });
			    response.callback_deferred = true;
			    if (!finalization_queued)
			    {
				    // No rollback ran. Keep the host unavailable until setup is retried.
				    if (restore_previous)
				    {
					    const auto current = FindMutableHost(m_app.settings.execution_hosts, host_id);
					    if (current != m_app.settings.execution_hosts.end()) current->runner_status = "error";
					    PersistenceCoordinator().SaveSettings(m_app);
				    }
				    release_install();
				    uam::PushStateUpdateIfChanged(browser, m_app);
			    }
		    };
		    const auto operation = g_remote_install_tokens.find(host_id);
		    if (operation == g_remote_install_tokens.end() || operation->second != install_nonce)
		    {
			    response = uam::query_handler_async::AsyncFailure(
			        409, "A stale remote helper install result was ignored.");
			    finalize_remote(false, false);
			    return;
		    }
		    auto found = FindMutableHost(m_app.settings.execution_hosts, host_id);
		    if (found == m_app.settings.execution_hosts.end())
		    {
			    response = uam::query_handler_async::AsyncFailure(
			        409, "The remote host was removed before setup finished.");
			    finalize_remote(false, false);
			    return;
		    }
		    if (found->runner_status != "installing" || found->ssh_alias != ssh_alias ||
		        found->runner_directory != runner_directory)
		    {
			    response = uam::query_handler_async::AsyncFailure(
			        409, "A stale remote helper install result was ignored.");
			    finalize_remote(false, false);
			    return;
		    }
		    if (!result->ok)
			    restore_host();
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
			    response = uam::query_handler_async::AsyncFailure(
			        500, "Remote setup could not save its status.");
			    if (result->ok)
			    {
				    found->runner_status = "installing";
				    finalize_remote(false, true);
			    }
			    else
			    {
				    if (!PersistenceCoordinator().SaveSettings(m_app))
					    response.error += " The previous host status also could not be saved.";
				    finalize_remote(false, false);
			    }
			    return;
		    }
		    finalize_remote(true, false);
	    });
	if (!queued)
	{
		g_remote_install_tokens.erase(host.id);
		m_app.settings = previous;
		PersistenceCoordinator().SaveSettings(m_app);
		uam::PushStateUpdateIfChanged(browser, m_app);
	}
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
	if (!uam::execution_hosts::CanBrowseRemoteDirectories(*configured))
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
	uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime,
	    cb, [host, directory, result]()
	    {
		uam::remote::RunnerClient client(
		    PlatformServicesFactory::Instance().process_service,
		    uam::remote::SshBridgeArgv(host.ssh_alias, host.platform,
		                               host.runner_version, host.runner_directory,
		                               host.runner_protocol_version),
		    host.runner_version, host.runner_protocol_version);
		if (!client.Connect(&result->error))
			return uam::query_handler_async::AsyncFailure(
			    502, result->error.empty() ? "The remote helper could not be reached." : result->error);
		result->connected = true;
		if (!client.ListDirectories(uam::paths::PathFromUtf8(directory), result->listing, &result->error))
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
	    [this, browser, host, result](
	        uam::query_handler_async::AsyncCefResult& response)
	    {
		    auto found = FindMutableHost(m_app.settings.execution_hosts, host.id);
		    if (found == m_app.settings.execution_hosts.end() ||
		        !uam::execution_hosts::ApplyHealthObservation(*found, host, result->connected,
		                                                      uam::time::IsoUtcTimestampNow()))
		    {
			    response = uam::query_handler_async::AsyncFailure(
			        409, "The remote helper changed while browsing. Try again.");
			    return;
		    }
		    if (!PersistenceCoordinator().SaveSettings(m_app) && response.ok)
			    response = uam::query_handler_async::AsyncFailure(
			        500, "Remote helper health changed, but its status could not be saved.");
		    uam::PushStateUpdateIfChanged(browser, m_app);
	    });
}
