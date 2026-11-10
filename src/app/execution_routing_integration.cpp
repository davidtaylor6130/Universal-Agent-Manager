#include "app/agent_run_scheduler.h"
#include "execution_routing_integration.h"
#include "app/repository_route_match_service.h"
#include "common/config/provider_availability.h"
#include "common/config/host_scoped_settings.h"
#include "common/config/settings_frontend_json.h"

#include "app/chat_domain_service.h"
#include "app/provider_resolution_service.h"
#include "common/chat/chat_repository.h"
#include "common/config/execution_host_config.h"
#include "common/config/central_provider_configuration.h"
#include "common/config/mcp_server_config.h"
#include "common/paths/workspace_root.h"
#include "common/paths/path_utils.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/app_time.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/utils/hash_utils.h"
#include "common/platform/background_thread_priority.h"
#include "common/platform/platform_services.h"
#include "common/utils/time_utils.h"
#include "remote/runner_client.h"
#include "remote/runner_protocol.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <sstream>

namespace uam
{
	namespace
	{
		std::string ConfigurationIdentity(const std::string& serialized)
		{
			std::uint64_t digest = hashing::kFnv1a64OffsetBasis;
			hashing::UpdateFnv1a64(digest, reinterpret_cast<const unsigned char*>(serialized.data()), serialized.size());
			std::ostringstream output;
			output << std::hex << digest;
			return output.str();
		}

		std::string WorkspaceIdentity(const AppState& app, const std::string& folder_id, const std::string& directory)
		{
			const ChatFolder* folder = ChatDomainService().FindFolderById(app, folder_id);
			if (folder == nullptr) return {};
			const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, folder->execution_host_id);
			if (host == nullptr) return {};
			const std::string normalized = host->id == "local"
			    ? paths::Utf8PathString(paths::PathFromUtf8(directory).lexically_normal())
			    : execution_hosts::NormalizeRemotePath(host->platform, directory);
			return nlohmann::json::array({host->id, folder_id, normalized}).dump();
		}

		ExecutionRoutingAssignment AssignmentForChat(const ChatSession& chat)
		{
			return {chat.id, chat.execution_host_id, chat.routing_assigned_connection_identity,
			    chat.routing_assigned_provider_configuration_identity, chat.folder_id,
			    chat.workspace_worktree_directory.empty() ? chat.workspace_directory : chat.workspace_worktree_directory,
			    chat.routing_mode == "automatic"};
		}

		ExecutionRoutingService& RoutingService(AppState& app)
		{
			if (!app.execution_routing_service) app.execution_routing_service = std::make_shared<ExecutionRoutingService>();
			return *app.execution_routing_service;
		}

		std::string WaitReason(ExecutionRoutingReason reason)
		{
			switch (reason)
			{
			case ExecutionRoutingReason::HostUnavailable: return "The assigned host is unavailable.";
			case ExecutionRoutingReason::OwnershipChanged: return "The assigned host or provider configuration changed. Confirm the provider for this computer in Routing settings; the chat then continues by itself.";
			case ExecutionRoutingReason::InconsistentSnapshot: return "Routing ownership is inconsistent. Review the chat before retrying.";
			default: return "Waiting for an eligible host with a matching workspace and available slot.";
			}
		}

		/// Version state is runtime-only, so routing starts the check it waits on. A check that
		/// did not produce a usable version is repeated with a doubling delay, up to five minutes.
		void RetryRoutingVersionCheck(AppState& app, const ProviderProfile& provider, const ExecutionHost& host, double now)
		{
			const std::string key = CliProviderVersionStateKey(provider.id, host.id);
			std::pair<double, double>& retry = app.execution_routing_version_check_retry[key];
			if (now < retry.first || !ProviderCliCompatibilityService().StartProviderVersionCheck(app, provider.id, true, host.id)) return;
			retry.second = retry.second <= 0.0 ? 15.0 : std::min(retry.second * 2.0, 300.0);
			retry.first = now + retry.second;
		}

		RemoteHostProbeOutcome ContactRemoteHost(const ExecutionHost& host, const std::string& session_id, std::stop_token stop)
		{
			remote::RunnerClient client(PlatformServicesFactory::Instance().process_service, host);
			if (!client.Connect(nullptr, stop)) return RemoteHostProbeOutcome::Unreachable;
			if (session_id.empty()) return RemoteHostProbeOutcome::Reachable;
			// The helper looks a session up before it checks the token, so a placeholder token
			// reads existence without renewing, reading or controlling the process.
			client.SetProcessControlToken(session_id, "ownership-probe");
			remote::ProcessPollResult ignored;
			bool missing = false;
			(void)client.PollProcess(session_id, ignored, nullptr, {}, nullptr, nullptr, &missing);
			return missing ? RemoteHostProbeOutcome::SessionMissing : RemoteHostProbeOutcome::Reachable;
		}

		/// Returns a finished contact once, otherwise Pending. A contact that is due starts on its own thread;
		/// each finished one doubles the wait before the next, from 15 seconds up to five minutes.
		RemoteHostProbeOutcome PollRemoteHostProbe(AppState& app, const std::string& key, const ExecutionHost& host, const std::string& session_id, double now)
		{
			std::shared_ptr<RemoteHostProbeTask>& task = app.remote_host_probes[key];
			if (!task) task = std::make_shared<RemoteHostProbeTask>();
			if (task->worker)
			{
				const RemoteHostProbeOutcome outcome = task->outcome.load(std::memory_order_acquire);
				if (outcome == RemoteHostProbeOutcome::Pending) return outcome;
				task->worker.reset();
				// An answer about an earlier host configuration is discarded and asked again.
				if (task->host != host || task->session_id != session_id) { task->retry_at_s = now; return RemoteHostProbeOutcome::Pending; }
				task->delay_s = task->delay_s <= 0.0 ? 15.0 : std::min(task->delay_s * 2.0, 300.0);
				task->retry_at_s = now + task->delay_s;
				return outcome;
			}
			if (now < task->retry_at_s) return RemoteHostProbeOutcome::Pending;
			task->host = host;
			task->session_id = session_id;
			task->outcome.store(RemoteHostProbeOutcome::Pending, std::memory_order_release);
			task->worker = std::make_unique<std::jthread>([state = task.get(), probe = app.remote_host_probe](std::stop_token stop)
			{
				uam::platform::LowerCurrentThreadToBackgroundPriority();
				RemoteHostProbeOutcome outcome = RemoteHostProbeOutcome::Unreachable;
				try { outcome = probe ? probe(state->host, state->session_id, stop) : ContactRemoteHost(state->host, state->session_id, stop); }
				catch (...) {}
				state->outcome.store(outcome == RemoteHostProbeOutcome::Pending ? RemoteHostProbeOutcome::Unreachable : outcome, std::memory_order_release);
			});
			return RemoteHostProbeOutcome::Pending;
		}

		/// The one place a helper contact outside a health check restores Ready. Install fences and installs stay as they are.
		bool MarkRemoteHostReachable(AppState& app, const ExecutionHost& observed)
		{
			const std::vector<ExecutionHost>::iterator current = std::ranges::find_if(app.settings.execution_hosts,
			    [&observed](const ExecutionHost& host) { return host.id == observed.id; });
			if (current == app.settings.execution_hosts.end() || current->runner_status != "error" ||
			    !execution_hosts::ApplyHealthObservation(*current, observed, true, uam::time::IsoUtcTimestampNow())) return false;
			app.remote_host_health_changed = true;
			return true;
		}
	}

	std::string ExecutionRoutingConnectionConfiguration(const ExecutionHost& host)
	{
		return nlohmann::json::array({host.id, host.transport, host.ssh_alias, host.platform, host.architecture, host.runner_directory, host.instruction_file, host.instruction_text ? nlohmann::json(*host.instruction_text) : nlohmann::json(nullptr)}).dump();
	}

	std::string ExecutionRoutingProviderConfiguration(const AppState& app, const ProviderProfile& provider, std::string_view host_id)
	{
		return nlohmann::json::array({provider.id, provider.execution_mode, provider.output_mode, provider.interactive_command, provider.structured_protocol, provider.runtime_flags, provider.resume_argument, provider.supports_cli, provider.supports_structured, provider.supports_interactive, provider.supports_resume, provider.history_adapter, provider.prompt_bootstrap, provider.prompt_bootstrap_path, app.settings.provider_extra_flags, central_configuration::Serialize(app.settings.central_provider_configuration), mcp_server_config::Serialize(host_scoped_settings::McpServers(app.settings, host_id)), settings_frontend_json::SerializeProviderChatDefaults({{provider.id, provider_chat_defaults::ForProvider(app.settings, provider.id, host_id)}})}).dump();
	}

	std::string ExecutionRoutingConnectionIdentity(const ExecutionHost& host)
	{
		return ConfigurationIdentity(ExecutionRoutingConnectionConfiguration(host));
	}

	std::string ExecutionRoutingProviderIdentity(const AppState& app, const ProviderProfile& provider, std::string_view host_id)
	{
		return ConfigurationIdentity(ExecutionRoutingProviderConfiguration(app, provider, host_id));
	}

	bool ExecutionRoutingOwnsWork(const AppState& app, const ChatSession& chat)
	{
		if (AgentRunScheduler::OwnsInitialRoutingPreparation(app, chat))
			return true;
		if (chat.remote_turn_reconnect_pending || !chat.remote_prompt_delivery_id.empty() ||
		    !chat.remote_pending_requests.empty() || !chat.remote_interaction_responses.empty() ||
		    !chat.remote_active_turn_id.empty() || chat.remote_stop_cleanup_pending || chat.remote_restart_pending)
			return true;
		const AcpSessionState* session = FindAcpSessionForChat(app, chat.id);
		if (session != nullptr && (session->processing || session->turn_checkpoint_commit_pending || session->turn_checkpoint_preflight_pending || AcpSessionIsWaitingForInput(*session) || session->prompt_request_id != 0 || !session->queued_prompt.empty() || AcpSessionHasPendingCancel(*session))) return true;
		for (const std::unique_ptr<CliTerminalState>& terminal : app.cli_terminals)
			if (terminal && (terminal->attached_chat_id == chat.id || terminal->frontend_chat_id == chat.id) &&
			    (terminal->running || terminal->context_preparation || terminal->lifecycle_state == CliTerminalLifecycleState::ShuttingDown)) return true;
		for (const AgentRun& run : app.agent_runs)
			if (run.transcript_chat_id == chat.id && run.status == "running") return true;
		return false;
	}

	ExecutionRoutingDecision AdmitExecutionRouting(AppState& app, ChatSession& chat, bool terminal)
	{
		const bool automatic = chat.routing_mode == "automatic";
		if (!automatic && !app.settings.execution_routing.enabled && !app.execution_routing_service)
			return {ExecutionRoutingStatus::Disabled, ExecutionRoutingReason::None, {}};
		if (automatic && app.execution_routing_restored_unresolved_chat_ids.contains(chat.id) && !ExecutionRoutingOwnsWork(app, chat))
		{
			app.execution_routing_waits[chat.id] = "Previous admission has unresolved ownership. Review and explicitly release its reservation before starting again. The previous runner process may still exist.";
			return {ExecutionRoutingStatus::Waiting, ExecutionRoutingReason::OwnershipChanged, AssignmentForChat(chat)};
		}
		const double now = GetAppTimeSeconds();
		const std::unordered_map<std::string, double>::const_iterator retry = app.execution_routing_retry_at_by_chat_id.find(chat.id);
		if (automatic && retry != app.execution_routing_retry_at_by_chat_id.end() && now < retry->second)
			return {ExecutionRoutingStatus::Waiting, ExecutionRoutingReason::NoEligibleHost, {}};
		const ProviderProfile* provider = ProviderResolutionService().ProviderForChat(app, chat);
		if (provider == nullptr) return {ExecutionRoutingStatus::Invalid, ExecutionRoutingReason::InconsistentSnapshot, {}};
		const bool repository_matches_current = automatic ? RefreshRepositoryRoutes(app) : false;
		std::vector<ExecutionRoutingCandidate> candidates;
		for (const ExecutionHost& host : app.settings.execution_hosts)
		{
			ExecutionRoutingCandidate candidate;
			candidate.host_id = host.id;
			candidate.connection_identity = ExecutionRoutingConnectionIdentity(host);
			candidate.provider_configuration_identity = ExecutionRoutingProviderIdentity(app, *provider, host.id);
			// New work goes only to a host last seen Ready. A chat already assigned to this host may try it
			// after a failed health check, exactly as an explicit SSH chat does.
			const bool assigned_here = automatic && !chat.routing_assigned_connection_identity.empty() && chat.execution_host_id == host.id;
			candidate.runner_ready = host.id == "local" || host.runner_status == "ready" || (assigned_here && execution_hosts::CanConnectRunner(host));
			candidate.runner_compatible = host.id == "local" || (!host.runner_version.empty() && host.runner_protocol_version == remote::kRunnerProtocolVersion);
			const std::unordered_map<std::string, CliProviderVersionState>::const_iterator version = app.runtime_cli_versions_by_provider_id.find(CliProviderVersionStateKey(provider->id, host.id));
			const bool version_usable = version != app.runtime_cli_versions_by_provider_id.end() && version->second.checked && version->second.supported &&
			    !version->second.installed_version.empty() && version->second.check_error.empty() && execution_hosts::SameConnection(host, version->second.execution_host);
			const bool provider_enabled = ProviderRuntime::IsRuntimeEnabled(*provider) && provider_availability::IsEnabled(app.settings, provider->id, host.id);
			candidate.provider_supported = provider_enabled && (terminal ? provider->supports_interactive : provider->supports_structured) &&
			    version_usable && ProviderCliLaunchBlockReason(app, provider->id, host.id).empty();
			for (const ExecutionRoutingHostPolicy& policy : app.settings.execution_routing.hosts)
			{
				if (policy.host_id != host.id) continue;
				if (version_usable) app.execution_routing_version_check_retry.erase(CliProviderVersionStateKey(provider->id, host.id));
				else if (automatic && host.id != "local" && policy.enabled && provider_enabled) RetryRoutingVersionCheck(app, *provider, host, now);
				candidate.pool_enabled = policy.enabled;
				candidate.max_active_turns = policy.max_active_turns;
				for (const ExecutionRoutingProviderConfirmation& confirmation : policy.provider_confirmations)
					if (confirmation.provider_id == provider->id)
					{
						candidate.provider_confirmed = true;
						candidate.confirmed_connection_identity = confirmation.connection_identity;
						candidate.confirmed_provider_configuration_identity = confirmation.provider_configuration_identity;
					}
				for (const ExecutionRoutingWorkspaceMapping& mapping : policy.workspace_mappings)
				{
					const ChatFolder* destination = ChatDomainService().FindFolderById(app, mapping.destination_folder_id);
					const std::string source = WorkspaceIdentity(app, mapping.source_folder_id, mapping.source_directory);
					if (destination == nullptr || destination->execution_host_id != host.id || source.empty() ||
					    !execution_hosts::IsAbsoluteRemotePath(host.platform, destination->directory)) continue;
					candidate.mappings.push_back({source, destination->id, destination->directory});
				}
			}
			if (repository_matches_current)
				for (const RepositoryRouteWorkspace& source : app.repository_route_workspaces)
				{
					if (source.host_id != "local") continue;
					for (const RepositoryRouteMatch& match : MatchRepositoryRoutes(source, app.repository_route_workspaces, app.settings.execution_routing))
					{
						if (match.manual || match.host_id != host.id || !execution_hosts::IsAbsoluteRemotePath(host.platform, match.directory)) continue;
						const std::string source_identity = WorkspaceIdentity(app, source.folder_id, source.directory);
						if (!source_identity.empty()) candidate.mappings.push_back({source_identity, match.destination_folder_id, match.directory});
					}
				}
			candidates.push_back(std::move(candidate));
		}
		if (std::ranges::none_of(candidates, [](const ExecutionRoutingCandidate& candidate) { return candidate.host_id == "local"; }))
		{
			ExecutionRoutingCandidate local;
			local.host_id = "local";
			local.runner_ready = local.runner_compatible = true;
			local.provider_supported = provider_availability::IsEnabled(app.settings, provider->id, "local");
			local.connection_identity = ExecutionRoutingConnectionIdentity(execution_hosts::LocalHost());
			local.provider_configuration_identity = ExecutionRoutingProviderIdentity(app, *provider, "local");
			candidates.push_back(std::move(local));
		}
		std::vector<ExecutionRoutingOccupancy> occupancy;
		for (const ChatSession& other : app.chats)
			if (ExecutionRoutingOwnsWork(app, other) || other.routing_reservation_pending)
				occupancy.push_back({other.id, other.execution_host_id});
		ExecutionRoutingRequest request;
		request.chat_id = chat.id;
		request.automatic_enabled = app.settings.execution_routing.enabled;
		request.recovery_of_owned_work = ExecutionRoutingOwnsWork(app, chat);
		if (!chat.routing_assigned_connection_identity.empty()) request.assigned = AssignmentForChat(chat);
		if (!request.assigned.host_id.empty())
		{
			const ChatFolder* destination = ChatDomainService().FindFolderById(app, request.assigned.destination_folder_id);
			if (destination == nullptr || destination->execution_host_id != request.assigned.host_id ||
			    destination->directory != chat.workspace_directory)
			{
				app.execution_routing_waits[chat.id] = "The assigned workspace changed. Review the assignment before retrying.";
				app.execution_routing_retry_at_by_chat_id[chat.id] = now + 1.0;
				return {ExecutionRoutingStatus::Waiting, ExecutionRoutingReason::OwnershipChanged, request.assigned};
			}
		}

		if (automatic && !request.assigned.host_id.empty())
		{
			const std::vector<ExecutionRoutingCandidate>::const_iterator owner = std::ranges::find(candidates, request.assigned.host_id, &ExecutionRoutingCandidate::host_id);
			if (owner != candidates.end() && (request.assigned.connection_identity != owner->connection_identity ||
			    request.assigned.provider_configuration_identity != owner->provider_configuration_identity))
			{
				// The assignment pins the chat to its host, so a newer configuration never moves it. With no turn,
				// delivery or pending answer on the old one, the chat adopts the configuration the user confirmed.
				// Work already on the host keeps its connection and adopts only provider settings, which apply at the next launch.
				const bool confirmed = owner->provider_confirmed && owner->connection_identity == owner->confirmed_connection_identity &&
				    owner->provider_configuration_identity == owner->confirmed_provider_configuration_identity;
				if (request.recovery_of_owned_work ? request.assigned.connection_identity == owner->connection_identity : confirmed)
				{
					(void)RoutingService(app).Release(chat.id, true);
					request.assigned.connection_identity = owner->connection_identity;
					request.assigned.provider_configuration_identity = owner->provider_configuration_identity;
				}
			}
		}

		if (automatic)
		{
			if (request.assigned.host_id.empty())
				chat.routing_source_directory = paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, chat));
			request.source_workspace_identity = WorkspaceIdentity(app, chat.routing_source_folder_id, chat.routing_source_directory);
			if (request.assigned.host_id.empty() && (!chat.native_session_id.empty() || chat.remote_process_exists))
			{
				app.execution_routing_waits[chat.id] = "This chat already has a provider session from before automatic routing assigned it a computer. Review the chat before retrying.";
				return {ExecutionRoutingStatus::Waiting, ExecutionRoutingReason::OwnershipChanged, {}};
			}
		}
		else
		{
			if (chat.folder_id.empty()) return {ExecutionRoutingStatus::Disabled, ExecutionRoutingReason::None, {}};
			request.explicit_host_id = chat.execution_host_id;
			request.explicit_folder_id = chat.folder_id;
			request.explicit_directory = paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, chat));
			// Explicit already-owned turns require no automatic admission or remapping.
			if (request.recovery_of_owned_work) return {ExecutionRoutingStatus::Disabled, ExecutionRoutingReason::None, {}};
		}
		ExecutionRoutingDecision decision = RoutingService(app).Admit(request, candidates, occupancy);
		if (decision.status == ExecutionRoutingStatus::Waiting || decision.status == ExecutionRoutingStatus::Invalid)
		{
			app.execution_routing_waits[chat.id] = decision.reason == ExecutionRoutingReason::OwnershipChanged && request.recovery_of_owned_work
			    ? "This chat still has work on its host's previous connection. Stop the turn, or restore the host's previous SSH settings, then retry."
			    : WaitReason(decision.reason);
			app.execution_routing_retry_at_by_chat_id[chat.id] = now + 1.0;
			return decision;
		}
		app.execution_routing_waits.erase(chat.id);
		app.execution_routing_retry_at_by_chat_id.erase(chat.id);
		if (decision.status == ExecutionRoutingStatus::Assigned) app.execution_routing_reserved_chat_ids.insert(chat.id);
		if (!automatic || decision.status != ExecutionRoutingStatus::Assigned) return decision;
		const std::string previous_model_id = chat.model_id;
		const std::string previous_reviewer_model_id = chat.reviewer_model_id;
		const std::string previous_reasoning_effort = chat.reasoning_effort;
		const std::string previous_service_tier = chat.service_tier;
		const bool previous_service_tier_explicit = chat.service_tier_explicit;
		const std::vector<std::string> previous_inherited_fields = chat.routing_inherited_default_fields;
		const ExecutionRoutingAssignment previous = AssignmentForChat(chat);
		const std::string previous_directory = chat.workspace_directory;
		const std::string previous_worktree = chat.workspace_worktree_directory;
		const bool previous_pending = chat.routing_reservation_pending;
		chat.execution_host_id = decision.assignment.host_id;
		chat.folder_id = decision.assignment.destination_folder_id;
		if (request.assigned.host_id.empty() && !chat.routing_inherited_default_fields.empty())
		{
			host_scoped_settings::ApplyInheritedRoutingDefaults(app.settings, chat);
			chat.routing_inherited_default_fields.clear();
		}
		if (request.assigned.host_id.empty())
		{
			chat.workspace_directory = decision.assignment.destination_directory;
			if (!chat.workspace_worktree_directory.empty()) chat.workspace_worktree_directory = decision.assignment.destination_directory;
		}
		chat.routing_assigned_connection_identity = decision.assignment.connection_identity;
		chat.routing_assigned_provider_configuration_identity = decision.assignment.provider_configuration_identity;
		chat.routing_reservation_pending = true;
		if (!ChatRepository::SaveChat(app.data_root, chat))
		{
			chat.model_id = previous_model_id;
			chat.reviewer_model_id = previous_reviewer_model_id;
			chat.reasoning_effort = previous_reasoning_effort;
			chat.service_tier = previous_service_tier;
			chat.service_tier_explicit = previous_service_tier_explicit;
			chat.routing_inherited_default_fields = previous_inherited_fields;
			chat.execution_host_id = previous.host_id;
			chat.folder_id = previous.destination_folder_id;
			chat.workspace_directory = previous_directory;
			chat.workspace_worktree_directory = previous_worktree;
			chat.routing_assigned_connection_identity = previous.connection_identity;
			chat.routing_assigned_provider_configuration_identity = previous.provider_configuration_identity;
			chat.routing_reservation_pending = previous_pending;
			if (!request.recovery_of_owned_work)
			{
				(void)RoutingService(app).Release(chat.id, true);
				app.execution_routing_reserved_chat_ids.erase(chat.id);
			}
			app.execution_routing_waits[chat.id] = "Routing assignment could not be saved. The provider was not launched.";
			app.execution_routing_retry_at_by_chat_id[chat.id] = now + 1.0;
			return {ExecutionRoutingStatus::Waiting, ExecutionRoutingReason::InconsistentSnapshot, {}};
		}
		if (request.assigned.host_id.empty()) RememberRepositoryRouteAssignment(app, chat);
		return decision;
	}

	bool RestoreExecutionRoutingReservations(AppState& app)
	{
		std::vector<ExecutionRoutingAssignment> reservations;
		for (const ChatSession& chat : app.chats)
			if (!chat.routing_assigned_connection_identity.empty() && (chat.routing_reservation_pending || ExecutionRoutingOwnsWork(app, chat)))
			{
				reservations.push_back(AssignmentForChat(chat));
				app.execution_routing_reserved_chat_ids.insert(chat.id);
				if (chat.routing_reservation_pending && !ExecutionRoutingOwnsWork(app, chat)) app.execution_routing_restored_unresolved_chat_ids.insert(chat.id);
			}
		return RoutingService(app).RestoreReservations(reservations);
	}

	bool PollExecutionRouting(AppState& app)
	{
		if (!app.execution_routing_service) return false;
		const double now = GetAppTimeSeconds();
		if (now < app.execution_routing_next_poll_s) return false;
		app.execution_routing_next_poll_s = now + 1.0;
		bool changed = false;
		std::vector<std::string> retry_ids;
		for (ChatSession& chat : app.chats)
		{
			const AcpSessionState* attached = FindAcpSessionForChat(app, chat.id);
			const ExecutionHost* owning_host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
			// Readiness follows the authenticated existing-process reconnect, not a durable intent alone.
			if (attached != nullptr && attached->running && attached->session_ready && !attached->remote_stop_unconfirmed &&
			    attached->process_execution_host_id == chat.execution_host_id && attached->provider_id == chat.provider_id &&
			    owning_host != nullptr && ExecutionRoutingConnectionIdentity(*owning_host) == chat.routing_assigned_connection_identity && !attached->recovering_remote_turn && !attached->recovering_remote_process)
			{
				changed = app.execution_routing_restored_unresolved_chat_ids.erase(chat.id) != 0 || changed;
			}
			if (ExecutionRoutingOwnsWork(app, chat)) continue;
			if (app.execution_routing_restored_unresolved_chat_ids.contains(chat.id))
			{
				app.execution_routing_waits[chat.id] = "Previous admission has unresolved ownership. Its runner process may still exist; review before releasing the reservation.";
				continue;
			}
			const AcpSessionState* session = FindAcpSessionForChat(app, chat.id);
			if (!chat.acp_queued_prompts.empty() || (session != nullptr && !session->queued_user_prompts.empty()))
			{
				if (chat.routing_mode == "automatic" && session != nullptr && session->running && session->session_ready) retry_ids.push_back(chat.id);
				continue;
			}
			bool terminal_pending = false;
			for (const std::unique_ptr<CliTerminalState>& terminal : app.cli_terminals)
				if (terminal && (terminal->attached_chat_id == chat.id || terminal->frontend_chat_id == chat.id) && terminal->should_launch) terminal_pending = true;
			if (terminal_pending) continue;
			if (chat.routing_reservation_pending)
			{
				chat.routing_reservation_pending = false;
				if (!ChatRepository::SaveChat(app.data_root, chat))
				{
					chat.routing_reservation_pending = true;
					continue;
				}
				changed = true;
			}
			(void)app.execution_routing_service->Release(chat.id, true);
			app.execution_routing_reserved_chat_ids.erase(chat.id);
		}
		for (const std::string& id : retry_ids)
			if (ChatSession* chat = ChatDomainService().FindChatById(app, id); chat != nullptr)
				if (AcpSessionState* session = FindAcpSessionForChat(app, id); session != nullptr)
					changed = DrainNextQueuedAcpUserPrompt(app, *session, *chat) || changed;
		const std::unordered_set<std::string> reserved_ids = app.execution_routing_reserved_chat_ids;
		for (const std::string& id : reserved_ids)
		{
			if (ChatDomainService().FindChatById(app, id) != nullptr || app.execution_routing_restored_unresolved_chat_ids.contains(id)) continue;
			bool owned = false;
			for (const std::unique_ptr<AcpSessionState>& session : app.acp_sessions)
				if (session && session->chat_id == id && (session->running || session->remote_stop_unconfirmed)) owned = true;
			for (const std::unique_ptr<CliTerminalState>& terminal : app.cli_terminals)
				if (terminal && (terminal->attached_chat_id == id || terminal->frontend_chat_id == id) &&
				    (terminal->running || terminal->context_preparation)) owned = true;
			if (!owned)
			{
				(void)app.execution_routing_service->Release(id, true);
				app.execution_routing_reserved_chat_ids.erase(id);
				app.execution_routing_waits.erase(id);
				app.execution_routing_retry_at_by_chat_id.erase(id);
			}
		}
		return changed;
	}

	bool PollRemoteHostRecovery(AppState& app)
	{
		const double now = GetAppTimeSeconds();
		bool changed = false;
		std::unordered_set<std::string> wanted;
		for (const ExecutionHost& host : std::vector<ExecutionHost>(app.settings.execution_hosts))
		{
			if (host.id == execution_hosts::kLocalHostId || host.runner_status != "error" || !remote::CanAttemptRunnerConnection(host)) continue;
			// A provider session attached through this helper is a contact that already succeeded.
			if (std::ranges::any_of(app.acp_sessions, [&host](const std::unique_ptr<AcpSessionState>& session)
			    { return session && session->running && session->session_ready && session->process_execution_host_id == host.id &&
			          !session->remote_stop_unconfirmed && !session->recovering_remote_turn && !session->recovering_remote_process; }))
			{
				changed = MarkRemoteHostReachable(app, host) || changed;
				continue;
			}
			const std::string key = "health:" + host.id;
			wanted.insert(key);
			if (PollRemoteHostProbe(app, key, host, {}, now) == RemoteHostProbeOutcome::Reachable) changed = MarkRemoteHostReachable(app, host) || changed;
		}
		for (const std::string& chat_id : std::vector<std::string>(app.execution_routing_restored_unresolved_chat_ids.begin(), app.execution_routing_restored_unresolved_chat_ids.end()))
		{
			const ChatSession* chat = ChatDomainService().FindChatById(app, chat_id);
			if (chat == nullptr || ExecutionRoutingOwnsWork(app, *chat)) continue;
			const ExecutionHost* found = execution_hosts::Find(app.settings.execution_hosts, chat->execution_host_id);
			if (found == nullptr || found->id == execution_hosts::kLocalHostId || !remote::CanAttemptRunnerConnection(*found)) continue;
			const ExecutionHost host = *found;
			const std::string key = "owner:" + chat_id;
			wanted.insert(key);
			const RemoteHostProbeOutcome outcome = PollRemoteHostProbe(app, key, host, "acp-" + chat_id, now);
			if (outcome == RemoteHostProbeOutcome::Reachable || outcome == RemoteHostProbeOutcome::SessionMissing) changed = MarkRemoteHostReachable(app, host) || changed;
			// Only the helper's own "no such process" releases. An unreachable host or a process that still exists keeps the manual release.
			std::string error;
			if (outcome == RemoteHostProbeOutcome::SessionMissing) changed = ReleaseUnresolvedExecutionRouting(app, AssignmentForChat(*chat), true, error) || changed;
		}
		// A contact still in flight is kept so this thread never waits for it.
		std::erase_if(app.remote_host_probes, [&wanted](const std::pair<const std::string, std::shared_ptr<RemoteHostProbeTask>>& probe)
		{ return !wanted.contains(probe.first) && !(probe.second->worker && probe.second->outcome.load(std::memory_order_acquire) == RemoteHostProbeOutcome::Pending); });
		return changed;
	}

	bool ReleaseUnresolvedExecutionRouting(AppState& app, const ExecutionRoutingAssignment& expected, bool acknowledged, std::string& error)
	{
		ChatSession* chat = ChatDomainService().FindChatById(app, expected.chat_id);
		if (!acknowledged || chat == nullptr || !app.execution_routing_restored_unresolved_chat_ids.contains(expected.chat_id) ||
		    !chat->routing_reservation_pending || AssignmentForChat(*chat) != expected)
		{
			error = "The reservation changed or unresolved ownership was not acknowledged. Refresh the chat before releasing it.";
			return false;
		}
		if (ExecutionRoutingOwnsWork(app, *chat))
		{
			error = "The chat still has active owning work. Stop or settle it before releasing its reservation.";
			return false;
		}
		for (const std::unique_ptr<AcpSessionState>& session : app.acp_sessions)
			if (session && session->chat_id == chat->id && (session->running || session->remote_stop_unconfirmed))
			{
				error = "The provider session is still running or its stop is unconfirmed.";
				return false;
			}
		chat->routing_reservation_pending = false;
		if (!ChatRepository::SaveChat(app.data_root, *chat))
		{
			chat->routing_reservation_pending = true;
			error = "The reservation release could not be saved.";
			return false;
		}
		(void)RoutingService(app).Release(chat->id, true);
		app.execution_routing_restored_unresolved_chat_ids.erase(chat->id);
		app.execution_routing_reserved_chat_ids.erase(chat->id);
		app.execution_routing_waits.erase(chat->id);
		app.execution_routing_retry_at_by_chat_id.erase(chat->id);
		error.clear();
		return true;
	}
}
