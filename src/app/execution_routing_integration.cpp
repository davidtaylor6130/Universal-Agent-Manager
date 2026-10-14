#include "execution_routing_integration.h"
#include "common/config/provider_availability.h"

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
#include "remote/runner_protocol.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <sstream>

namespace uam
{
	namespace
	{
		std::string ConfigurationIdentity(const nlohmann::json& value)
		{
			const std::string serialized = value.dump();
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
			case ExecutionRoutingReason::OwnershipChanged: return "The assigned host or provider configuration changed. Review the assignment before retrying.";
			case ExecutionRoutingReason::InconsistentSnapshot: return "Routing ownership is inconsistent. Review the chat before retrying.";
			default: return "Waiting for an eligible host with a matching workspace and available slot.";
			}
		}
	}

	std::string ExecutionRoutingConnectionIdentity(const ExecutionHost& host)
	{
		return ConfigurationIdentity(nlohmann::json::array({host.id, host.transport, host.ssh_alias,
		    host.platform, host.architecture, host.runner_version, host.runner_protocol_version,
		    host.runner_directory, host.instruction_file, host.instruction_text ? nlohmann::json(*host.instruction_text) : nlohmann::json(nullptr)}));
	}

	std::string ExecutionRoutingProviderIdentity(const AppState& app, const ProviderProfile& provider)
	{
		return ConfigurationIdentity(nlohmann::json::array({provider.id, provider.execution_mode, provider.output_mode,
		    provider.interactive_command, provider.structured_protocol, provider.runtime_flags, provider.resume_argument,
		    provider.supports_cli, provider.supports_structured, provider.supports_interactive, provider.supports_resume, provider.history_adapter,
		    provider.prompt_bootstrap, provider.prompt_bootstrap_path, app.settings.provider_extra_flags,
		    central_configuration::Serialize(app.settings.central_provider_configuration), mcp_server_config::Serialize(app.settings.mcp_servers)}));
	}

	bool ExecutionRoutingOwnsWork(const AppState& app, const ChatSession& chat)
	{
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
		std::vector<ExecutionRoutingCandidate> candidates;
		for (const ExecutionHost& host : app.settings.execution_hosts)
		{
			ExecutionRoutingCandidate candidate;
			candidate.host_id = host.id;
			candidate.connection_identity = ExecutionRoutingConnectionIdentity(host);
			candidate.provider_configuration_identity = ExecutionRoutingProviderIdentity(app, *provider);
			candidate.runner_ready = host.id == "local" || host.runner_status == "ready";
			candidate.runner_compatible = host.id == "local" || (!host.runner_version.empty() && host.runner_protocol_version == remote::kRunnerProtocolVersion);
			const std::unordered_map<std::string, CliProviderVersionState>::const_iterator version = app.runtime_cli_versions_by_provider_id.find(CliProviderVersionStateKey(provider->id, host.id));
			candidate.provider_supported = ProviderRuntime::IsRuntimeEnabled(*provider) && provider_availability::IsEnabled(app.settings, provider->id, host.id) && (terminal ? provider->supports_interactive : provider->supports_structured) &&
			    version != app.runtime_cli_versions_by_provider_id.end() && version->second.checked && version->second.supported &&
			    !version->second.installed_version.empty() && version->second.check_error.empty() && execution_hosts::SameConnection(host, version->second.execution_host) &&
			    ProviderCliLaunchBlockReason(app, provider->id, host.id).empty();
			for (const ExecutionRoutingHostPolicy& policy : app.settings.execution_routing.hosts)
			{
				if (policy.host_id != host.id) continue;
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
			candidates.push_back(std::move(candidate));
		}
		if (std::ranges::none_of(candidates, [](const ExecutionRoutingCandidate& candidate) { return candidate.host_id == "local"; }))
		{
			ExecutionRoutingCandidate local;
			local.host_id = "local";
			local.runner_ready = local.runner_compatible = true;
			local.provider_supported = provider_availability::IsEnabled(app.settings, provider->id, "local");
			local.connection_identity = ExecutionRoutingConnectionIdentity(execution_hosts::LocalHost());
			local.provider_configuration_identity = ExecutionRoutingProviderIdentity(app, *provider);
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

		if (automatic)
		{
			if (request.assigned.host_id.empty())
				chat.routing_source_directory = paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, chat));
			request.source_workspace_identity = WorkspaceIdentity(app, chat.routing_source_folder_id, chat.routing_source_directory);
			if (request.assigned.host_id.empty() && (!chat.native_session_id.empty() || chat.remote_process_exists))
			{
				app.execution_routing_waits[chat.id] = WaitReason(ExecutionRoutingReason::OwnershipChanged);
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
			app.execution_routing_waits[chat.id] = WaitReason(decision.reason);
			app.execution_routing_retry_at_by_chat_id[chat.id] = now + 1.0;
			return decision;
		}
		app.execution_routing_waits.erase(chat.id);
		app.execution_routing_retry_at_by_chat_id.erase(chat.id);
		if (decision.status == ExecutionRoutingStatus::Assigned) app.execution_routing_reserved_chat_ids.insert(chat.id);
		if (!automatic || decision.status != ExecutionRoutingStatus::Assigned) return decision;
		const ExecutionRoutingAssignment previous = AssignmentForChat(chat);
		const std::string previous_directory = chat.workspace_directory;
		const std::string previous_worktree = chat.workspace_worktree_directory;
		const bool previous_pending = chat.routing_reservation_pending;
		chat.execution_host_id = decision.assignment.host_id;
		chat.folder_id = decision.assignment.destination_folder_id;
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
