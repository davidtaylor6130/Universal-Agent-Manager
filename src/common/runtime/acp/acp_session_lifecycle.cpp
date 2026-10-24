#include "app/managed_parent_continuation_service.h"
#include "app/agent_run_scheduler.h"
#include "common/chat/acp_prompt_recovery.h"
#include "common/chat/message_attachment_sources.h"
#include "app/repository_route_match_service.h"
#include "common/config/host_scoped_settings.h"
#include "computer_use/computer_use_mcp_config.h"
#include "common/config/provider_availability.h"
#include "common/config/settings_frontend_json.h"
#include "common/config/settings_normalization.h"
#include <atomic>
#include <thread>
#include "common/provider/provider_setup_input.h"
#include "common/runtime/acp/claude_quota_continuation.h"
#include "app/execution_routing_integration.h"
#include "app/provider_configuration_service.h"
#include "app/worktree_launch_preparation_service.h"
#include "app/opencode_compaction_sync_service.h"
#include "app/opencode_native_activity_service.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_control_capability_retention.h"
#include "common/runtime/acp/acp_model_json.h"
#include "common/runtime/acp/acp_session_runtime.h"

#include "app/agent_definition_service.h"
#include "app/memory_service.h"
#include "common/provider/provider_runtime.h"
#include "common/provider/copilot_protocol.h"
#include "common/provider/structured_attachments.h"
#include "app/chat_domain_service.h"
#include "app/provider_resolution_service.h"
#include "app/git_worktree_service.h"
#include "app/vcs_commit_service.h"
#include "app/uam_control_service.h"
#include "common/chat/chat_repository.h"
#include "common/config/approval_modes.h"
#include "common/config/provider_chat_defaults.h"
#include "common/config/execution_host_config.h"
#include "common/config/mcp_server_config.h"
#include "common/paths/workspace_root.h"
#include "common/platform/platform_services.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/runtime/acp/acp_protocol_methods.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"
#include "common/utils/hash_utils.h"
#include "remote/runner_proxy.h"
#include "remote/host_context.h"
#include "common/memory/memory_levels.h"
#include "common/memory/memory_policy.h"
#include "remote/runner_protocol.h"

#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace uam
{
	/** Immutable configuration work retained by the app after its launch authority is revoked. */
	struct PendingLocalAcpLaunch
	{
		std::shared_ptr<const std::uint8_t> incarnation;
		std::filesystem::path root;
		ExecutionHost host;
		std::string ownership;
		std::vector<std::string> argv;
		uam::provider_setup::Environment environment;
		ProviderConfigurationBundle bundle;
		std::string instructions;
		std::string error;
		bool version_only = false;
		bool success = false;
		std::atomic<bool> finished{false};
		std::unique_ptr<std::jthread> worker;
		// Non-callable UAM capability reservation while this exact local launch is prepared.
		const AcpSessionState* control_session = nullptr;
		std::string control_capability_id, control_manager_id, control_root_chat_id;
		std::shared_ptr<const std::uint8_t> control_root_incarnation;
		std::int64_t control_capability_expiry_epoch_ms = 0;
		double control_retention_deadline_s = 0.0;
		// Process authority is saved after the caller has bound its admitted input.
		std::function<bool(AppState&, AcpSessionState&, ChatSession&)> cold_start;
		ClaudeQuotaContinuation cold_quota;
		int cold_serial = 0;
		bool cold_bound = false, cold_armed = false, cold_attempted = false;
		acp_detail::DurableAdmissionState cold_admission = acp_detail::DurableAdmissionState::Pending;
		std::string cold_prompt, cold_queue, cold_authority, cold_runtime_requests;
		std::optional<std::string> cold_recovery;
		std::unordered_map<int, std::string> cold_methods;
		int cold_input_first = -1, cold_input_last = -1, cold_request = 0;
		std::shared_ptr<platform::AsyncByteWriter> cold_writer;
		std::optional<acp_detail::GoalPromptReceiptScope> cold_goal;
		bool cold_processing = false;
		std::deque<AcpQueuedUserPromptState> cold_raw;
		std::vector<std::pair<std::size_t, Message>> cold_rows;
	};

	// Proposal: separate from PendingAcpPromptDelivery, which retires after queue handoff.
	struct LocalInputWriteObservation
	{
		struct Row { std::size_t index; std::string native_id, carrier; };
		AppState* app = nullptr;
		std::filesystem::path data_root;
		std::string chat_id, created_at, native_id, provider_id, runtime_session_id, process_id, birth, agent_run_id;
		std::string configuration, method, wire_bytes;
		std::shared_ptr<const std::uint8_t> chat_incarnation;
		const AcpSessionState* runtime = nullptr;
		std::shared_ptr<const char> runtime_incarnation;
		int turn_serial = 0, request_id = 0;
		std::weak_ptr<platform::AsyncByteWriter> writer;
		std::shared_ptr<const platform::AsyncByteWriter::EnqueueReceipt> receipt;
		bool writer_provenance_verified = false; // Set once at original observed handoff, never rebound.
		std::vector<Row> rows;
	};

	struct PendingAcpPromptDelivery
	{
		nlohmann::json message;
		std::string method;
		int request_id = 0;
		int turn_serial = 0;
		std::filesystem::path data_root;
		std::string provider_id;
		std::string session_id;
		std::string workspace;
		std::string prompt;
		std::string configuration;
		std::vector<std::size_t> unsent_messages;
		std::vector<std::pair<std::size_t, std::string>> user_messages;
		std::string created_at;
		std::string process_id;
		std::string active_goal_id;
		std::string goal_revision;
		bool small_model_mode = false;
		std::optional<acp_detail::GoalPromptReceiptScope> goal_scope;
		bool quota_process_authority = false;
		std::shared_ptr<const std::uint8_t> scoped_chat_incarnation;
		std::shared_ptr<platform::AsyncByteWriter> scoped_writer;
		std::vector<std::pair<std::size_t, Message>> scoped_user_messages;
		std::string native_session_id;
		std::optional<ExecutionHost> remote_host;
		std::optional<AcpQueuedUserPromptState> remote_prepared_head;
		std::string delivery_session_id;
		std::string delivery_id;
		std::string delivery_payload;
		std::string proxy_token;
		int proxy_protocol = 0;
		acp_detail::DurableAdmissionState admission = acp_detail::DurableAdmissionState::Pending;
		std::shared_ptr<ClaudeInputPreparation> saved_image_preparation;
		std::shared_ptr<const ManagedResultPromptInput> managed_result_input;
	};
}

namespace uam::acp_detail
{

namespace
{
	std::string CapturePromptDeliveryConfiguration(const AppState& app, const AcpSessionState& session,
	    const ChatSession& chat, bool include_quota_settlement);

	/** Copies launch metadata while keeping the complete transcript exclusively on the GUI thread. */
	ChatSession LocalLaunchMetadata(ChatSession& chat)
	{
		struct RestoreTranscript
		{
			ChatSession& owner;
			std::vector<Message> messages;
			std::vector<AcpQueuedUserPromptState> queued;
			std::string handoff;
			std::vector<Goal> goals;
			std::vector<AcpRemotePendingRequestState> remote_requests;
			std::vector<UamControlAuditRecord> audit;
			std::vector<std::string> linked_files;
			~RestoreTranscript()
			{
				owner.messages = std::move(messages);
				owner.acp_queued_prompts = std::move(queued);
				owner.provider_handoff_context = std::move(handoff);
				owner.goals = std::move(goals);
				owner.remote_pending_requests = std::move(remote_requests);
				owner.uam_control_audit = std::move(audit);
				owner.linked_files = std::move(linked_files);
			}
		} restore{chat, std::move(chat.messages), std::move(chat.acp_queued_prompts), std::move(chat.provider_handoff_context),
		    std::move(chat.goals), std::move(chat.remote_pending_requests), std::move(chat.uam_control_audit), std::move(chat.linked_files)};
		return chat;
	}
	std::string LocalLaunchOwnership(const AppState& app, const AcpSessionState& session, const ChatSession& chat,
	    bool include_quota_settlement = true)
	{
		const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
		nlohmann::json settings = settings_frontend_json::SerializeFingerprintSettingsFields(app.settings);
		// Health observations can be published by compatibility polling between the two
		// ownership checks; they do not change launch configuration or host identity.
		for (nlohmann::json& host : settings["executionHosts"])
		{
			host.erase("runnerStatus");
			host.erase("lastSeenAt");
		}
		return nlohmann::json::array({provider.id, provider.interactive_command, provider.runtime_flags, provider.resume_argument,
		    provider.supports_structured, provider.structured_protocol, provider.execution_mode, provider.output_mode,
		    provider.prompt_bootstrap, provider.prompt_bootstrap_path, app.settings.git_auto_worktree,
		    chat.workspace_base_ref,
		    chat.git_base_branch_override ? nlohmann::json(*chat.git_base_branch_override) : nlohmann::json(nullptr), chat.svn_update_pending, chat.workspace_branch_name, chat.id, chat.created_at, chat.provider_id, chat.native_session_id,
		    chat.execution_host_id, chat.workspace_directory, chat.workspace_source_directory,
		    chat.workspace_worktree_directory, chat.workspace_isolation_kind, chat.routing_mode,
		    chat.routing_assigned_connection_identity, chat.model_id, chat.approval_mode, chat.extra_flags,
		    chat.active_goal_id, chat.goal_command_revision, chat.uam_agent_id, chat.small_model_mode,
		    chat.reasoning_effort, chat.service_tier, chat.command_safety_tier, session.goal_turn_model_id,
		    session.active_uam_agent_id, session.active_uam_agent_definition_hash,
		    session.active_uam_agent_instructions, session.active_uam_agent_execution_capability,
		    session.active_uam_agent_workspace_access, session.last_process_id, session.session_id, AcpProcessHandleLabel(session),
		    session.model_discovery_only, session.goal_internal_session, session.recovering_remote_turn, session.recovering_remote_process,
		    std::move(settings),
		    CapturePromptDeliveryConfiguration(app, session, chat, include_quota_settlement)}).dump();
	}
	bool OwnsColdLaunch(const AppState&, const AcpSessionState&, const ChatSession&, const PendingLocalAcpLaunch&);
}

bool ValidateCapturedAcpAgentPolicy(const AppState& app, const ChatSession& chat,
    const std::string& agent_id, const std::string& snapshot, const std::string& capability,
    std::string* error_out)
{
	std::string effective_capability = capability;
	if (chat.execution_host_id != execution_hosts::kLocalHostId)
		effective_capability = "uam-prompt-injected";
	if (app.settings.central_provider_configuration.enabled ||
	    central_configuration::McpEnabled(app.settings.central_provider_configuration, chat.provider_id,
	        chat.execution_host_id, "shared-setup", app.settings.central_provider_configuration.enabled))
		effective_capability = "uam-central-native-setup";
	if (provider_chat_defaults::ForProvider(app.settings, chat.provider_id, chat.execution_host_id).feature_preference == "provider")
		effective_capability = "provider-native";
	return AgentDefinitionService::ValidateCapturedRuntimePolicy(agent_id, snapshot, chat.provider_id,
	    effective_capability, error_out);
}

bool ShouldDeferLocalAcpConfiguration(const AppState& app, const AcpSessionState& session, const ChatSession& chat)
{
	const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
	if (host == nullptr || host->id != execution_hosts::kLocalHostId || session.model_discovery_only ||
	    session.recovering_remote_turn || session.recovering_remote_process) return false;
	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
	return central_configuration::McpEnabled(app.settings.central_provider_configuration, provider.id, host->id, "shared-setup", app.settings.central_provider_configuration.enabled) || ((provider.id == provider_ids::kClaudeCli || provider.id == provider_ids::kAntigravityCli) && UamControlService::EnabledForChat(app.settings, chat)) ||
		   (provider.id == provider_ids::kAntigravityCli &&
		    [&]
		    {
			    ChatSession launch_chat;
			    launch_chat.id = chat.id;
			    launch_chat.provider_id = chat.provider_id;
			    launch_chat.execution_host_id = chat.execution_host_id;
			    launch_chat.computer_use_backend = chat.computer_use_backend;
			    launch_chat.computer_use_globally_enabled = app.settings.computer_use_globally_enabled;
			    return !computer_use::AcpMcpServers(launch_chat).empty();
		    }());
}

bool ShouldRetainPendingLocalAcpControlCapability(const AppState& app,
    const UamControlCapability& capability, std::int64_t now_epoch_ms, double now_seconds)
{
	const AcpSessionState* session = FindAcpSessionForChat(app, capability.session_chat_id);
	const ChatSession* chat = ChatDomainService().FindChatById(app, capability.session_chat_id);
	const ChatSession* root = ChatDomainService().FindChatById(app, capability.chat_id);
	if (!session || !chat || !root || session->running || session->lifecycle_state != kAcpLifecycleStarting ||
	    session->model_discovery_only || ((session->recovering_remote_turn || session->recovering_remote_process) && !HasPendingAcpColdLaunch(*session)) ||
	    session->queue_held || session->managed_cancellation_pending || AcpSessionHasPendingCancel(*session) ||
	    session->local_stop_pending || session->remote_stop_pending || session->remote_stop_unconfirmed ||
	    chat->side_cleanup_requested || root->side_cleanup_requested || chat->imported_read_only || root->imported_read_only ||
	    session->provider_id != capability.provider_id || session->uam_control_capability_id != capability.id ||
	    !UamControlService::EnabledForChat(app.settings, *chat) || !UamControlService::EnabledForChat(app.settings, *root) ||
	    now_seconds < 0.0 || now_epoch_ms < 0 || now_epoch_ms >= capability.expires_at_epoch_ms || app.uam_control_manager_id.empty() ||
	    capability.directory != app.data_root / "uam-control" / app.uam_control_manager_id / capability.id ||
	    !paths::IsDirectoryNoThrow(capability.directory) || paths::IsLinkOrReparsePointNoThrow(capability.directory))
		return false;
	const auto& pending = session->pending_local_launch;
	const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat->execution_host_id);
	const auto workspace = app.worktree_launch_preparations.find(chat->id);
	const auto branch = app.branch_preparations.find(chat->id);
	if (pending && pending->cold_start)
	{
		if (!OwnsColdLaunch(app, *session, *chat, *pending) ||
		    pending->control_capability_id != capability.id ||
		    pending->control_retention_deadline_s <= now_seconds) return false;
	}
	else if (!pending || !pending->worker || pending->version_only || pending->control_session != session ||
	    pending->control_capability_id != capability.id || pending->control_manager_id != app.uam_control_manager_id ||
	    pending->control_root_chat_id != root->id || !pending->control_root_incarnation ||
	    pending->control_root_incarnation != root->metadata_incarnation ||
	    pending->control_capability_expiry_epoch_ms != capability.expires_at_epoch_ms ||
	    pending->control_retention_deadline_s <= 0.0 || now_seconds < 0.0 || now_seconds >= pending->control_retention_deadline_s ||
	    pending->root != app.data_root || pending->incarnation != chat->metadata_incarnation ||
	    !host || host->id != execution_hosts::kLocalHostId || !execution_hosts::SameConnection(pending->host, *host) ||
	    pending->ownership != LocalLaunchOwnership(app, *session, *chat) ||
	    std::ranges::find(app.local_acp_launch_tasks, pending) == app.local_acp_launch_tasks.end() ||
	    (pending->finished.load(std::memory_order_acquire) && !pending->success) ||
	    app.worktree_operation_chat_ids.contains(chat->id) ||
	    (branch != app.branch_preparations.end() && !branch->second.expired()) ||
	    (workspace != app.worktree_launch_preparations.end() && !(workspace->second->saved && *workspace->second->saved)) ||
	    !AgentRunScheduler::InitialLaunchAllowed(app, *session, *chat, nullptr))
		return false;
	if (!chat->agent_run_id.empty())
	{
		const auto run = std::ranges::find(app.agent_runs, chat->agent_run_id, &AgentRun::id);
		if (run == app.agent_runs.end() || run->id != capability.agent_run_id || run->root_chat_id != root->id ||
		    run->transcript_chat_id != chat->id || run->provider_id != capability.provider_id || run->status != "running" ||
		    app.pending_agent_run_cancellation_ids.contains(run->id) || app.managed_subagent_cancellation_receipts.contains(run->id))
			return false;
	}
	else if (!capability.agent_run_id.empty() || root != chat)
		return false;
	return true;
}

std::string LocalSessionReceiptOwnership(const AppState& app, const AcpSessionState& session, const ChatSession& chat)
{
	return LocalLaunchOwnership(app, session, chat);
}

namespace
{
	std::string ColdLaunchAuthority(const ChatSession& chat)
	{
		return nlohmann::json::array({chat.native_session_id, chat.remote_process_exists,
		    chat.remote_process_control_token, chat.remote_uam_control_channel_id,
		    chat.remote_structured_protocol, chat.remote_opencode_native_port,
		    chat.remote_opencode_native_token}).dump();
	}

	std::string ColdRecoveryMetadata(const ChatSession& chat)
	{
		nlohmann::json requests = nlohmann::json::array(), responses = nlohmann::json::array();
		for (const auto& request : chat.remote_pending_requests)
			requests.push_back(nlohmann::json::array({request.request_id, request.method, request.delivery_id,
			    request.payload, request.user_message_index, request.turn_serial, request.provider_turn_id, request.response_consumed}));
		for (const auto& response : chat.remote_interaction_responses)
			responses.push_back(nlohmann::json::array({response.request_id_json, response.response_json}));
		return nlohmann::json::array({chat.remote_turn_reconnect_pending, chat.remote_active_turn_id,
		    chat.remote_turn_serial, chat.remote_turn_user_message_index, chat.remote_prompt_delivery_session_id,
		    chat.remote_prompt_delivery_id, chat.remote_prompt_delivery_payload, chat.remote_next_request_id,
		    chat.remote_delivered_stdout_cursor, chat.remote_delivered_stderr_cursor, chat.remote_source_exit_pending,
		    chat.remote_source_exit_code, chat.remote_manual_compaction_pending, chat.remote_manual_compaction_boundary_json,
		    std::move(requests), std::move(responses)}).dump();
	}

	std::string ColdRuntimeRequests(const AcpSessionState& session)
	{
		return nlohmann::json::array({session.initialize_request_id, session.session_setup_request_id,
		    session.startup_model_request_id, session.reasoning_change_request_id, session.config_option_change_request_id,
		    session.mode_change_request_id, session.model_change_request_id, session.awaiting_model_config_options,
		    session.prompt_request_id}).dump();
	}

	std::string ColdLaunchConfiguration(const AppState& app, const AcpSessionState& session, const ChatSession& chat,
	    const PendingLocalAcpLaunch& task)
	{
		// Only the owned claim -> continued settlement may change these two fields before arming.
		return LocalLaunchOwnership(app, session, chat, task.cold_quota.id.empty());
	}

	bool OwnsColdLaunch(const AppState& app, const AcpSessionState& session, const ChatSession& chat,
	    const PendingLocalAcpLaunch& task)
	{
		const auto* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
		const auto workspace = app.worktree_launch_preparations.find(chat.id);
		const auto branch = app.branch_preparations.find(chat.id);
		if ((workspace != app.worktree_launch_preparations.end() && !(workspace->second->saved && *workspace->second->saved)) ||
		    (branch != app.branch_preparations.end() && !branch->second.expired())) return false;
		if (task.incarnation != chat.metadata_incarnation || task.root != app.data_root ||
		    session.pending_local_launch.get() != &task || task.control_session != &session ||
		    !host || !execution_hosts::SameConnection(task.host, *host) ||
		    host->runner_protocol_version != task.host.runner_protocol_version ||
		    session.running || session.stdin_writer != task.cold_writer || session.cancel_requested || AcpSessionHasPendingCancel(session) ||
		    session.local_stop_pending || session.remote_stop_pending || session.remote_stop_unconfirmed ||
		    session.queue_held || session.managed_cancellation_pending || app.managed_initial_shutdown ||
		    AcpStopInProgress(app, chat.id) || chat.side_cleanup_requested || chat.imported_read_only ||
		    chat.acp_queue_held || chat.acp_queue_storage_error || chat.remote_stop_cleanup_pending ||
		    chat.remote_restart_pending || chat.remote_source_exit_pending ||
		    !session.transport_ownership_error.empty() || session.pending_agent_input || session.pending_queued_history ||
		    session.pending_prompt_delivery || session.pending_local_session_receipt ||
		    (!task.cold_recovery && AcpSessionHasPendingRuntimeRequest(session)) ||
		    session.manual_compaction_pending || session.context_compaction_in_progress ||
		    app.worktree_operation_chat_ids.contains(chat.id) ||
		    !provider_availability::IsEnabled(app.settings, chat.provider_id, chat.execution_host_id) ||
		    !AgentRunScheduler::InitialLaunchAllowed(app, session, chat, nullptr) ||
		    task.ownership != ColdLaunchConfiguration(app, session, chat, task) ||
		    task.cold_authority != ColdLaunchAuthority(chat)) return false;
		if (task.cold_recovery && (ColdRecoveryMetadata(chat) != *task.cold_recovery ||
		    !chat.remote_process_exists || chat.remote_process_control_token.empty() ||
		    session.remote_output_delivery_token != chat.remote_process_control_token ||
		    session.process_execution_host_id != chat.execution_host_id || session.provider_id != chat.provider_id ||
		    (!session.goal_internal_session && !chat.native_session_id.empty() && session.session_id != chat.native_session_id) ||
		    session.remote_runner_protocol_version <= 0 || (host->runner_protocol_version != 0 &&
		    session.remote_runner_protocol_version != host->runner_protocol_version))) return false;
		if (!task.cold_quota.id.empty())
		{
			auto expected = task.cold_quota;
			if (task.cold_armed) expected.state = "continued";
			if (chat.claude_quota_continuation != expected ||
			    (task.cold_bound && session.claude_quota_delivery_id != expected.id)) return false;
		}
		if (!task.control_capability_id.empty())
		{
			const auto capability = std::ranges::find(app.uam_control_capabilities, task.control_capability_id, &UamControlCapability::id);
			const auto* root = ChatDomainService().FindChatById(app, task.control_root_chat_id);
			if (task.control_retention_deadline_s <= GetAppTimeSeconds() ||
			    capability == app.uam_control_capabilities.end() || !root ||
			    root->metadata_incarnation != task.control_root_incarnation || root->side_cleanup_requested || root->imported_read_only ||
			    task.control_manager_id != app.uam_control_manager_id || session.uam_control_capability_id != capability->id ||
			    capability->expires_at_epoch_ms != task.control_capability_expiry_epoch_ms ||
			    time::SystemEpochMillisecondsNow() >= capability->expires_at_epoch_ms ||
			    capability->directory != task.root / "uam-control" / task.control_manager_id / capability->id ||
			    !paths::IsDirectoryNoThrow(capability->directory) || paths::IsLinkOrReparsePointNoThrow(capability->directory) ||
			    !UamControlService::EnabledForChat(app.settings, chat) || !UamControlService::EnabledForChat(app.settings, *root)) return false;
		}
		return !task.cold_bound || ((!chat.small_model_mode || (task.cold_goal &&
		    CaptureGoalPromptReceiptScope(app, session, chat) == task.cold_goal)) && session.turn_serial == task.cold_serial && session.processing == task.cold_processing &&
		    session.queued_prompt == task.cold_prompt && session.prompt_request_id == task.cold_request &&
		    session.pending_request_methods == task.cold_methods && ColdRuntimeRequests(session) == task.cold_runtime_requests &&
		    session.queued_user_prompts == task.cold_raw && ChatRepository::QueueAdmissionFingerprint(chat) == task.cold_queue &&
		    std::ranges::all_of(task.cold_rows, [&chat](const auto& row)
		    { return row.first < chat.messages.size() && chat.messages[row.first] == row.second; }));
	}

	bool PollColdLaunch(AppState& app, AcpSessionState& session, ChatSession& chat)
	{
		const auto task = session.pending_local_launch;
		if (!task || !task->cold_start) return false;
		if (!task->cold_bound && !session.processing && session.turn_serial == task->cold_serial)
			BindPendingAcpColdLaunch(app, session, chat); // Explicit opening without authored input.
		if (!OwnsColdLaunch(app, session, chat, *task) || task->cold_admission == DurableAdmissionState::Failed)
		{
			session.pending_local_launch.reset();
			session.reconnect_pending = false;
			session.queue_held = true;
			session.last_error = "Provider launch lost its owned full-save receipt. Retry the held input.";
			if (task->root == app.data_root && task->incarnation == chat.metadata_incarnation)
			{
				chat.acp_queue_held = true;
				for (const auto& row : task->cold_rows)
					if (!task->cold_recovery && task->cold_processing && task->cold_input_first >= 0 &&
					    row.first >= static_cast<std::size_t>(task->cold_input_first) &&
					    row.first <= static_cast<std::size_t>(task->cold_input_last) &&
					    row.first < chat.messages.size() && chat.messages[row.first] == row.second)
						chat.messages[row.first].acp_prompt_not_sent = chat.messages[row.first].interrupted = true;
				if (!task->cold_recovery && task->cold_authority == ColdLaunchAuthority(chat) && chat.execution_host_id != execution_hosts::kLocalHostId)
				{
					chat.remote_process_exists = false;
					chat.remote_structured_protocol.clear();
				}
				ScheduleChatSave(app, chat, 0.0);
			}
			UamControlService::RevokeForSession(app, session);
			return false;
		}
		if (!task->cold_armed || task->cold_admission == DurableAdmissionState::Pending) return true;
		if (task->cold_attempted) return false;
		task->cold_attempted = true; // The receipt permits one launch attempt, never automatic replay.
		session.pending_local_launch.reset();
		return task->cold_start(app, session, chat);
	}
}

bool StageAcpColdLaunch(AppState& app, AcpSessionState& session, ChatSession& chat,
    std::function<bool(AppState&, AcpSessionState&, ChatSession&)> start)
{
	const auto* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
	if (!host || !start || session.pending_local_launch) return false;
	const auto task = std::make_shared<PendingLocalAcpLaunch>();
	task->incarnation = chat.metadata_incarnation;
	task->root = app.data_root;
	task->host = *host;
	task->control_session = &session;
	task->cold_start = std::move(start);
	task->cold_serial = session.turn_serial;
	task->cold_writer = session.stdin_writer;
	if (session.recovering_remote_turn || session.recovering_remote_process) task->cold_recovery = ColdRecoveryMetadata(chat);
	if (!app.claude_quota_dispatch_id.empty() || (!session.claude_quota_delivery_id.empty() &&
	    session.claude_quota_delivery_id == chat.claude_quota_continuation.id)) task->cold_quota = chat.claude_quota_continuation;
	task->ownership = ColdLaunchConfiguration(app, session, chat, *task);
	task->cold_authority = ColdLaunchAuthority(chat);
	const auto control = std::ranges::find(app.uam_control_capabilities, session.uam_control_capability_id, &UamControlCapability::id);
	if (control != app.uam_control_capabilities.end())
		if (const auto* root = ChatDomainService().FindChatById(app, control->chat_id))
		{
			task->control_capability_id = control->id;
			task->control_manager_id = app.uam_control_manager_id;
			task->control_root_chat_id = root->id;
			task->control_root_incarnation = root->metadata_incarnation;
			task->control_capability_expiry_epoch_ms = control->expires_at_epoch_ms;
			const int timeout = app.settings.acp_setup_inactivity_timeout_seconds <= 0
			    ? settings::kDefaultAcpSetupInactivityTimeoutSeconds
			    : std::clamp(app.settings.acp_setup_inactivity_timeout_seconds,
			        settings::kMinAcpSetupInactivityTimeoutSeconds, settings::kMaxAcpSetupInactivityTimeoutSeconds);
			task->control_retention_deadline_s = GetAppTimeSeconds() + timeout;
		}
	session.pending_local_launch = task;
	session.lifecycle_state = kAcpLifecycleStarting;
	if (task->cold_recovery || (session.processing && !session.queued_prompt.empty())) BindPendingAcpColdLaunch(app, session, chat);
	return true;
}

void BindPendingAcpColdLaunch(AppState& app, AcpSessionState& session, ChatSession& chat, bool quota_settlement)
{
	const auto task = session.pending_local_launch;
	if (!task || !task->cold_start || task->cold_armed) return;
	if (quota_settlement && !task->cold_quota.id.empty())
	{
		auto expected = task->cold_quota;
		expected.state = "continued";
		expected.context = ClaudeQuotaContext(app, chat);
		if (!task->cold_bound || chat.claude_quota_continuation != expected)
		{ task->cold_admission = DurableAdmissionState::Failed; return; }
		task->cold_quota = expected;
	}
	if (!OwnsColdLaunch(app, session, chat, *task)) { task->cold_admission = DurableAdmissionState::Failed; return; }
	if (!task->cold_bound)
	{
		task->cold_bound = true;
		if (chat.small_model_mode) task->cold_goal = CaptureGoalPromptReceiptScope(app, session, chat);
		task->cold_serial = session.turn_serial;
		task->cold_processing = session.processing;
		task->cold_request = session.prompt_request_id;
		task->cold_methods = session.pending_request_methods;
		task->cold_runtime_requests = ColdRuntimeRequests(session);
		task->cold_input_first = session.turn_first_user_message_index >= 0 ? session.turn_first_user_message_index : session.turn_user_message_index;
		task->cold_input_last = session.turn_user_message_index;
		task->cold_prompt = session.queued_prompt;
		task->cold_queue = ChatRepository::QueueAdmissionFingerprint(chat);
		task->cold_raw = session.queued_user_prompts;
		for (std::size_t i = 0; i < chat.messages.size(); ++i)
			if (chat.messages[i].role == MessageRole::User) task->cold_rows.emplace_back(i, chat.messages[i]);
	}
	if (!task->cold_quota.id.empty() && chat.claude_quota_continuation.state == "claimed") return;
	task->cold_armed = true;
	const std::string id = chat.id;
	const auto owned = [&app, task, id]()
	{
		const auto* current = ChatDomainService().FindChatById(app, id);
		auto* native = FindAcpSessionForChat(app, id);
		return current && native && native == task->control_session && !task->cold_attempted && OwnsColdLaunch(app, *native, *current, *task);
	};
	const auto complete = [task](DurableAdmissionState state) { task->cold_admission = state; };
	task->cold_admission = BeginDurableAdmission(app, chat, owned, complete, DurableAdmissionFailurePurpose::BindingOnly);
	PinDurableAdmissionRoot(app, id);
}

bool RebindPendingAcpQuotaConfiguration(AppState& app, AcpSessionState& session, ChatSession& chat)
{
	const auto task = session.pending_local_launch;
	if (!task || task->cold_start) return true;
	if (task->cold_quota.id.empty()) return false;
	auto expected = task->cold_quota;
	expected.state = "continued";
	expected.context = ClaudeQuotaContext(app, chat);
	if (chat.claude_quota_continuation != expected || task->root != app.data_root ||
	    task->incarnation != chat.metadata_incarnation) return false;
	// The saved worker scope must equal this state with only its explicit quota settlement reversed.
	ChatSession previous = LocalLaunchMetadata(chat);
	previous.claude_quota_continuation = task->cold_quota;
	if (task->ownership != LocalLaunchOwnership(app, session, previous)) return false;
	task->ownership = LocalLaunchOwnership(app, session, chat);
	return true;
}

bool AcpLaunchConfigurationFinished(const AcpSessionState& session)
{
	return session.pending_local_launch && !session.pending_local_launch->cold_start &&
	    session.pending_local_launch->finished.load(std::memory_order_acquire);
}

bool HasPendingAcpColdLaunch(const AcpSessionState& session)
{
	return session.pending_local_launch && static_cast<bool>(session.pending_local_launch->cold_start);
}

std::string SavedImageRetryExecutionOwnership(const AppState& app, const AcpSessionState& session, const ChatSession& chat, bool active_turn)
{
	// Scope only the selected execution context. Appearance, voice/editor state,
	// host health timestamps and other hosts do not change this invocation.
	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
	auto central = app.settings.central_provider_configuration;
	const std::string provider_id = provider_ids::NormalizeCliProviderAliasOrSelf(chat.provider_id);
	std::erase_if(central.mcp_overrides, [&](const ProviderMcpOverride& item)
	{
		return provider_ids::NormalizeCliProviderAliasOrSelf(item.provider_id) != provider_id || item.execution_host_id != chat.execution_host_id;
	});
	std::map<std::string, MemoryWorkerBinding> memory_bindings;
	for (const auto& [id, binding] : app.settings.memory_worker_bindings)
		if (provider_ids::NormalizeCliProviderAliasOrSelf(id) == provider_id) memory_bindings.emplace(id, binding);
	nlohmann::json computer_rules = nlohmann::json::array();
	for (const auto& rule : app.settings.computer_use_allowed_applications)
		computer_rules.push_back({rule.identity_kind, rule.identity});
	return nlohmann::json::array({
	    nlohmann::json::array({provider.id, provider.interactive_command, provider.runtime_flags, provider.resume_argument,
	        provider.supports_cli, provider.supports_structured, provider.supports_resume, provider.structured_protocol,
	        provider.execution_mode, provider.output_mode, provider.prompt_bootstrap, provider.prompt_bootstrap_path}),
	    nlohmann::json::array({chat.id, chat.created_at, chat.provider_id, chat.native_session_id, chat.execution_host_id,
	        chat.workspace_directory, chat.workspace_source_directory, chat.workspace_worktree_directory, chat.workspace_isolation_kind,
	        chat.workspace_base_ref, chat.git_base_branch_override ? nlohmann::json(*chat.git_base_branch_override) : nlohmann::json(nullptr),
	        chat.workspace_branch_name, chat.svn_update_pending, chat.routing_mode, chat.routing_assigned_connection_identity,
	        chat.model_id, chat.reasoning_effort, chat.service_tier, chat.service_tier_explicit, chat.approval_mode,
	        chat.command_safety_tier, chat.extra_flags, chat.active_goal_id, chat.goal_command_revision, chat.uam_agent_id,
	        chat.small_model_mode, chat.memory_level, memory_policy::Resolve(app.settings, chat), chat.uam_control_enabled,
	        chat.uam_control_explicitly_disabled, chat.computer_use_enabled, chat.computer_use_backend,
	        chat.computer_use_target_kind, chat.computer_use_target_id, chat.computer_use_target_process_id, chat.computer_use_target_input_mode}),
	    nlohmann::json::array({session.session_id, session.process_execution_host_id, AcpProcessHandleLabel(session),
	        session.current_model_id, session.current_mode_id, session.model_discovery_only, session.goal_internal_session}),
	    nlohmann::json::array({app.settings.git_auto_worktree, provider_chat_defaults::ForProvider(app.settings, chat.provider_id, chat.execution_host_id).feature_preference}),
	    central_configuration::Serialize(central),
	    mcp_server_config::Serialize(host_scoped_settings::VisibleMcpServers(app.settings, chat.execution_host_id)),
	    settings_frontend_json::SerializeMemoryWorkerBindings(memory_bindings),
	    nlohmann::json::array({app.settings.memory_recall_budget_bytes, app.settings.markdown_store_directory,
	        app.settings.permission_reviewer_provider_id, app.settings.permission_reviewer_model_id,
	        app.settings.computer_use_globally_enabled, app.settings.computer_use_allowlist_enabled, std::move(computer_rules)}),
	    provider_availability::IsEnabled(app.settings, chat.provider_id, chat.execution_host_id),
	    active_turn ? nlohmann::json::array({PromptDeliveryConfiguration(app, session, chat), session.active_uam_agent_instructions,
	        session.active_uam_agent_execution_capability, session.active_uam_agent_workspace_access}) : nlohmann::json(nullptr)}).dump();
}

void ReapLocalAcpLaunchTasks(AppState& app)
{
	std::erase_if(app.local_acp_launch_tasks, [](const std::shared_ptr<PendingLocalAcpLaunch>& task)
	{
		if (!task->finished.load(std::memory_order_acquire) || task.use_count() != 1) return false;
		// All work is finished; releasing the thread handle must not wait for scheduling.
		if (task->worker && task->worker->joinable()) task->worker->detach();
		return true;
	});
}

namespace
{
	std::string CapturePromptDeliveryConfiguration(const AppState& app, const AcpSessionState& session,
	    const ChatSession& chat, bool include_quota_settlement)
	{
		return nlohmann::json::array({chat.model_id, chat.reasoning_effort, chat.service_tier, chat.service_tier_explicit,
		    chat.approval_mode, chat.command_safety_tier, session.current_model_id,
		    session.current_mode_id, session.goal_turn_model_id, session.active_uam_agent_id,
		    session.active_uam_agent_definition_hash, chat.memory_level, memory_policy::Resolve(app.settings, chat),
		    app.settings.memory_recall_budget_bytes, app.settings.computer_use_globally_enabled, chat.computer_use_enabled,
		    chat.computer_use_backend, chat.computer_use_target_id,
		    chat.computer_use_target_process_id, chat.computer_use_target_input_mode,
		    chat.claude_quota_continuation.id,
		    include_quota_settlement ? nlohmann::json(chat.claude_quota_continuation.state) : nlohmann::json(nullptr),
		    chat.claude_quota_continuation.chat_id, chat.claude_quota_continuation.native_session_id,
		    include_quota_settlement ? nlohmann::json(chat.claude_quota_continuation.context) : nlohmann::json(nullptr),
		    chat.claude_quota_continuation.accepted_message_digest,
		    chat.claude_quota_continuation.agent_id, chat.claude_quota_continuation.agent_definition_hash,
		    chat.claude_quota_continuation.agent_workspace_access, chat.claude_quota_continuation.agent_execution_capability,
		    chat.claude_quota_continuation.observed_at_epoch_s, chat.claude_quota_continuation.resets_at_epoch_s,
		    chat.claude_quota_continuation.user_message_index, chat.claude_quota_continuation.message_count}).dump();
	}
}

std::string PromptDeliveryConfiguration(const AppState& app, const AcpSessionState& session, const ChatSession& chat)
{
	return CapturePromptDeliveryConfiguration(app, session, chat, true);
}


namespace
{
	// No message-neighbor/content guessing. native_input_id must identify exactly one raw row.
	// Runtime-derived interruption/usage/event bookkeeping is deliberately not an authored carrier.
	std::optional<std::string> LocalWriteCarrier(const Message& row)
	{
		if (row.role != MessageRole::User || !row.native_input_id ||
		    !input_provenance::ValidNativeInputId(*row.native_input_id) ||
		    (row.origin_request_id && !input_provenance::ValidOriginRequestId(*row.origin_request_id)) ||
		    !row.tool_calls.empty() || !row.thoughts.empty() || !row.plan_summary.empty() || !row.plan_entries.empty() ||
		    !row.blocks.empty())
			return {}; // Unmodelled authored carriers retain existing send behavior, but no new fact.

		nlohmann::json attachments = nlohmann::json::array();
		for (const auto& a : row.attachments)
		{
			// Inline source pointers cannot stand in for exact bytes; require the original digest.
			if (a.native_inline_payload && a.content_digest.empty()) return {};
			attachments.push_back(nlohmann::json::array({a.id, a.name, a.kind, a.mime_type, a.path,
			    a.size_bytes, a.copied, a.preview_key, a.preview_origin_path, static_cast<int>(a.source),
			    a.source_uri, a.origin_execution_host_id, a.native_part_key, a.native_tool_call_id,
			    a.native_source_field, a.native_asset_key, a.content_digest}));
		}
		const auto value = nlohmann::json::array({*row.native_input_id,
		    row.origin_request_id ? nlohmann::json(*row.origin_request_id) : nlohmann::json(nullptr),
		    row.managed_origin_run_id ? nlohmann::json(*row.managed_origin_run_id) : nlohmann::json(nullptr),
		    row.content, row.created_at, row.provider, row.model_id, attachments,
		    row.markdown_store_files, row.markdown_store_prompt_blocks, row.priority_steer,
		    row.checkpoint_sha, row.checkpoint_parent_sha}).dump();
		return value.size() <= 1024 * 1024 ? std::optional<std::string>(value) : std::nullopt;
	}

	std::shared_ptr<LocalInputWriteObservation> CaptureLocalInputWrite(AppState& app,
	    AcpSessionState& session, const ChatSession& chat, const nlohmann::json& message,
	    std::string_view method, int request_id, const std::vector<std::size_t>& authored_rows)
	try
	{
		// Best-effort evidence addition only: limits/legacy IDs leave the old send path unchanged.
		// At most eight simultaneously pending observations, each <=4MiB wire +1MiB carriers.
		if (app.local_input_write_observations.size() >= 8 || authored_rows.empty() || authored_rows.size() > 64 ||
		    chat.execution_host_id != execution_hosts::kLocalHostId || session.process_execution_host_id != execution_hosts::kLocalHostId ||
		    !chat.metadata_incarnation || !session.permission_review_incarnation ||
		    !session.running || !session.transport_ownership_error.empty() || session.turn_serial <= 0 ||
		    session.local_stop_pending || session.remote_stop_pending || session.remote_stop_unconfirmed || session.cancel_requested ||
		    FindAcpSessionForChat(app, chat.id) != &session || ChatDomainService().FindChatById(app, chat.id) != &chat)
			return {};
		if (!method.empty() && (!message.contains("id") || !message["id"].is_number_integer() || message["id"] != request_id)) return {};
		auto observed = std::make_shared<LocalInputWriteObservation>();
		observed->app = &app;
		observed->data_root = app.data_root;
		observed->chat_id = chat.id;
		observed->created_at = chat.created_at;
		observed->birth = chat.managed_followup_incarnation;
		observed->agent_run_id = chat.agent_run_id;
		observed->chat_incarnation = chat.metadata_incarnation;
		observed->runtime = &session;
		observed->runtime_incarnation = session.permission_review_incarnation;
		observed->native_id = chat.native_session_id;
		observed->runtime_session_id = session.session_id;
		observed->provider_id = session.provider_id;
		observed->process_id = AcpProcessHandleLabel(session);
		observed->configuration = SavedImageRetryExecutionOwnership(app, session, chat, true); // Pure selected execution snapshot, no Retry authority.
		observed->method = method;
		observed->request_id = request_id;
		observed->turn_serial = session.turn_serial;
		if ((observed->process_id.empty() || observed->process_id == "0") || observed->configuration.size() > 256 * 1024) return {};
		std::size_t carrier_bytes = 0;
		std::vector<std::size_t> unique_indices;
		for (const std::size_t index : authored_rows)
		{
			if (index >= chat.messages.size() || std::ranges::find(unique_indices, index) != unique_indices.end()) return {};
			unique_indices.push_back(index);
			const Message& row = chat.messages[index];
			if (row.acp_prompt_discarded || row.acp_prompt_not_sent) return {}; // Active handoff still requires its old strict input eligibility.
			const auto carrier = LocalWriteCarrier(row);
			if (!carrier || !row.native_input_id) return {};
			if (std::ranges::count_if(chat.messages, [&](const Message& r) { return r.native_input_id == row.native_input_id; }) != 1) return {};
			carrier_bytes += carrier->size();
			if (carrier_bytes > 1024 * 1024) return {};
			observed->rows.push_back({index, *row.native_input_id, *carrier});
		}
		return observed;
	}
	catch (...) { return {}; } // Evidence preparation cannot change whether legacy input is attempted.

	bool LocalInputWriteHistoricalRowsCurrent(AppState& app, const LocalInputWriteObservation& observed,
	    std::vector<std::size_t>& exact_rows)
	try
	{
		exact_rows.clear();
		if (observed.app != &app || observed.data_root != app.data_root || !observed.writer_provenance_verified ||
		    !observed.receipt || observed.wire_bytes.empty()) return false;
		const ChatSession* chat = ChatDomainService().FindChatById(app, observed.chat_id);
		if (!chat || chat->metadata_incarnation != observed.chat_incarnation || chat->created_at != observed.created_at ||
		    chat->managed_followup_incarnation != observed.birth || chat->agent_run_id != observed.agent_run_id ||
		    !chat->messages_loaded || chat->imported_read_only || chat->side_cleanup_requested) return false;
		// Historical write DATA is independent of current runtime, turn, model/mode, native ID and permission.
		// The original private receipt was tied to the original writer and exact bytes at handoff.
		// A later runtime/Stop cannot mint, revoke, transfer or revive that physical sink fact.
		for (const auto& row : observed.rows)
		{
			std::optional<std::size_t> exact;
			for (std::size_t index = 0; index < chat->messages.size(); ++index)
			{
				const auto& candidate = chat->messages[index];
				if (!candidate.native_input_id || *candidate.native_input_id != row.native_id) continue;
				if (exact) return false; // No first-match or content-based alias.
				exact = index;
			}
			if (!exact) return false;
			const auto carrier = LocalWriteCarrier(chat->messages[*exact]);
			if (!carrier || *carrier != row.carrier) return false;
			exact_rows.push_back(*exact);
		}
		return !exact_rows.empty();
	}
	catch (...) { return false; }
}

// This is the actual authored-prompt handoff; tests use a real controlled local pipe sink.
bool WriteLocalAcpPromptWithObservation(AppState& app, AcpSessionState& session, ChatSession& chat,
    const nlohmann::json& message, const std::string& method, int request_id,
    const std::vector<std::size_t>& authored_rows)
{
	const auto observed = CaptureLocalInputWrite(app, session, chat, message, method, request_id, authored_rows);
	if (!observed) return WriteAcpMessage(session, message);
	// Capture before the one write attempt. No retry after observed false: a prefix may have been written.
	app.local_input_write_observations.push_back(observed);
	const bool enqueued = WriteAcpMessage(session, message, nullptr, &observed->receipt, &observed->wire_bytes);
	const auto writer = session.stdin_writer; // Same exclusive owner-thread call, no intervening reassignment.
	if (observed->receipt && writer && writer->OwnsReceipt(*observed->receipt) &&
	    !observed->wire_bytes.empty() && observed->wire_bytes.size() <= platform::kAsyncInputMaxQueuedBytes)
	{
		observed->writer = writer;
		observed->writer_provenance_verified = true;
	}
	else
		// true+null is compatible unobserved legacy/out-of-band success, never a completed sink fact.
		// Invalid passive observation is retired without touching input/row/queue/error/save state.
		std::erase(app.local_input_write_observations, observed);
	return enqueued;
}

bool PollLocalInputWriteObservations(AppState& app)
{
	bool changed = false;
	std::erase_if(app.local_input_write_observations, [&](const auto& observed)
	{
		if (!observed) return true;
		// Passive observation is not cleanup or input authority. On irreversible ownership loss,
		// retire ONLY this record. No row/queue/error/save mutation and no later flags-only revival.
		// The original writer's immutable Complete/Incomplete state is never demoted or rewritten.
		if (!observed->receipt || !observed->writer_provenance_verified) return true;
		const auto status = observed->receipt->Completion();
		std::vector<std::size_t> exact_rows;
		if (!LocalInputWriteHistoricalRowsCurrent(app, *observed, exact_rows)) return true;
		if (status == platform::AsyncByteWriter::WriteCompletion::Pending) return false;
		if (status != platform::AsyncByteWriter::WriteCompletion::Complete)
		{
			// Incomplete is NOT definitely-unsent: a prefix may have been accepted.
			// Retire passive observation only; authored intent/recovery remains in its existing stores.
			return true;
		}
		ChatSession* chat = ChatDomainService().FindChatById(app, observed->chat_id);
		for (const std::size_t index : exact_rows) chat->messages[index].input_write_completed = true;
		// Existing owner-thread save scheduler; this does not ACK persistence or provider processing.
		ScheduleChatSave(app, *chat, 0.0);
		changed = true;
		return true;
	});
	return changed;
}

bool SavedImageRetrySourceMatches(const AppState& app, const AcpSessionState& session, const ChatSession& chat, const SavedImageRetryPermission& permission)
{
	if (permission.root.empty() || !permission.incarnation || session.chat_id != chat.id || permission.root != app.data_root || permission.incarnation != chat.metadata_incarnation || permission.chat_id != chat.id || permission.chat_created_at != chat.created_at || permission.provider_id != chat.provider_id || session.provider_id != chat.provider_id || permission.execution_host_id != chat.execution_host_id || (!permission.remote_scope && !paths::IsControllerLocalWorkspace(chat)) || !ProviderRuntimeRegistry::ResolveById(chat.provider_id).SupportsSavedImageRetry(session) || !chat.messages_loaded || chat.imported_read_only || chat.side_cleanup_requested || permission.row_index < 0 || static_cast<std::size_t>(permission.row_index) >= chat.messages.size())
		return false;
	if (permission.remote_scope)
	{
		const auto& scope = *permission.remote_scope;
		const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
		if (paths::IsControllerLocalWorkspace(chat) || !ProviderRuntimeRegistry::ResolveById(chat.provider_id).SupportsRemoteOwnedImageRetry(session) ||
		    !host || scope.host.id != chat.execution_host_id || !execution_hosts::SameConnection(scope.host, *host) ||
		    !session.running || !session.session_ready || session.local_stop_pending || session.remote_stop_pending || session.remote_stop_unconfirmed ||
		    session.reconnect_pending || session.recovering_remote_turn || session.recovering_remote_process || session.remote_source_exit_reported ||
		    chat.remote_stop_cleanup_pending || chat.remote_source_exit_pending || chat.remote_restart_pending || chat.native_session_reset_pending ||
		    !chat.remote_process_exists || scope.native_session_id.empty() || scope.runtime_session_id.empty() || scope.native_session_id != scope.runtime_session_id ||
		    chat.native_session_id != scope.native_session_id || session.session_id != scope.runtime_session_id ||
		    scope.process_id.empty() || scope.process_id != AcpProcessHandleLabel(session) || session.process_execution_host_id != host->id ||
		    scope.proxy_token.empty() || scope.proxy_token != session.remote_output_delivery_token || scope.proxy_token != chat.remote_process_control_token ||
		    scope.proxy_protocol < 3 || scope.proxy_protocol != session.remote_runner_protocol_version || scope.proxy_protocol != host->runner_protocol_version ||
		    scope.configuration != SavedImageRetryExecutionOwnership(app, session, chat) || permission.attachments.size() > 32)
			return false;
	}
	const Message& row = chat.messages[static_cast<std::size_t>(permission.row_index)];
	if (row.role != MessageRole::User || row.acp_prompt_discarded || row.created_at != permission.row_created_at || row.content != permission.content || row.attachments != permission.attachments || row.attachments.size() > 256)
		return false;
	bool owned = false;
	for (const auto& attachment : row.attachments)
	{
		if (!attachment_sources::IsPersistable(attachment, chat.id))
			return false;
		if (attachment.source == AttachmentSource::LegacyPath)
		{
			if (permission.remote_scope) return false;
			continue;
		}
		if (attachment.source != AttachmentSource::OwnedNativeAsset || attachment.kind != "image")
			return false;
		if (permission.remote_scope && (attachment.origin_execution_host_id != permission.execution_host_id ||
		    !uam::ranges::Contains(std::array<std::string_view, 4>{"image/png", "image/jpeg", "image/gif", "image/webp"}, std::string_view(strings::ToLowerAscii(attachment.mime_type)))))
			return false;
		owned = true;
	}
	return owned;
}

void RevokeSavedImageRetry(AcpSessionState& session)
{
	if (session.claude_input_preparation && session.claude_input_preparation->state->identity.saved_image_retry)
		session.claude_input_preparation.reset();
	session.saved_image_retry.reset();
	session.saved_image_retry_turn_serial = -1;
}

static ClaudeInputIdentity ClaudeInputSnapshot(const AppState&, const AcpSessionState&, const ChatSession&);
static bool OwnsPreparedSavedImageRetry(const AppState&, const AcpSessionState&, const ChatSession&, const std::shared_ptr<ClaudeInputPreparation>&);
static bool AbortSavedImageRetry(AppState&, AcpSessionState&, ChatSession&, const std::string&);
static bool OwnsSavedImageRetryPacket(const AppState&, const AcpSessionState&, const ChatSession&, const std::shared_ptr<ClaudeInputPreparation>&, const nlohmann::json&, const std::string&, int);

namespace
{
	/// <summary>Names revoked ownership without logging authored input or configuration values.</summary>
	std::string_view PromptAdmissionOwnershipMismatch(const AppState& app, const AcpSessionState& session,
	    const ChatSession& chat, const PendingAcpPromptDelivery& packet)
	{
		if (packet.turn_serial != session.turn_serial || packet.prompt != session.queued_prompt ||
		    !session.processing || session.cancel_requested) return "turn";
		if (packet.data_root != app.data_root || packet.created_at != chat.created_at) return "chat";
		if (packet.provider_id != session.provider_id || packet.provider_id != chat.provider_id) return "provider";
		if (packet.session_id != session.session_id || packet.native_session_id != chat.native_session_id) return "native_session";
		if (packet.process_id != AcpProcessHandleLabel(session)) return "process";
		if (packet.workspace != chat.workspace_directory) return "workspace";
		if (packet.active_goal_id != chat.active_goal_id || packet.goal_revision != chat.goal_command_revision || packet.small_model_mode != chat.small_model_mode)
			return "goal";
		if (packet.goal_scope && (!CanStageRemoteGoalPromptAdmission(app, session, chat) ||
		    !ValidateAcpRestartOwnership(app, session, chat, nullptr) ||
		    CaptureGoalPromptReceiptScope(app, session, chat) != packet.goal_scope ||
		    chat.metadata_incarnation != packet.scoped_chat_incarnation || session.stdin_writer != packet.scoped_writer ||
		    !std::ranges::all_of(packet.scoped_user_messages, [&chat](const auto& input)
		    { return input.first < chat.messages.size() && chat.messages[input.first] == input.second; })))
			return "goal_owner";
		if (packet.quota_process_authority && (app.managed_initial_shutdown || AcpStopInProgress(app, chat.id) ||
		    chat.side_cleanup_requested || !session.transport_ownership_error.empty() ||
		    !ValidateAcpRestartOwnership(app, session, chat, nullptr) || chat.metadata_incarnation != packet.scoped_chat_incarnation ||
		    session.stdin_writer != packet.scoped_writer ||
		    !std::ranges::all_of(packet.scoped_user_messages, [&chat](const auto& input)
		    { return input.first < chat.messages.size() && chat.messages[input.first] == input.second; }))) return "quota_process";
		if (packet.configuration != PromptDeliveryConfiguration(app, session, chat))
			return "configuration";
		if (packet.saved_image_preparation && !OwnsSavedImageRetryPacket(app, session, chat, packet.saved_image_preparation, packet.message, packet.method, packet.request_id))
			return "saved_image_retry";
		if (!provider_availability::IsEnabled(app.settings, chat.provider_id, chat.execution_host_id))
			return "provider_disabled";
		if (!std::ranges::all_of(packet.user_messages, [&chat](const std::pair<std::size_t, std::string>& user) { return user.first < chat.messages.size() && chat.messages[user.first].role == MessageRole::User && chat.messages[user.first].content == user.second && !chat.messages[user.first].acp_prompt_not_sent; }))
			return "authored_input";
		if (!packet.remote_host) return chat.execution_host_id == execution_hosts::kLocalHostId ? "" : "execution_host";
		const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
		if (host == nullptr || !execution_hosts::SameConnection(*host, *packet.remote_host) ||
		    host->runner_protocol_version != packet.proxy_protocol || session.remote_runner_protocol_version != packet.proxy_protocol ||
		    session.remote_output_delivery_token != packet.proxy_token || session.process_execution_host_id != host->id) return "remote_connection";
		if (session.prompt_request_id != 0 || !chat.remote_turn_reconnect_pending ||
		    chat.remote_prompt_delivery_session_id != packet.delivery_session_id || chat.remote_prompt_delivery_id != packet.delivery_id ||
		    chat.remote_prompt_delivery_payload != packet.delivery_payload) return "remote_delivery";
		if (!packet.method.empty() && !std::ranges::any_of(chat.remote_pending_requests, [&packet](const AcpRemotePendingRequestState& request)
		    { return request.request_id == packet.request_id && request.turn_serial == packet.turn_serial && request.method == packet.method && !request.response_consumed; })) return "remote_request";
		return "";
	}

	bool OwnsPromptAdmission(const AppState& app, const AcpSessionState& session, const ChatSession& chat,
	    const PendingAcpPromptDelivery& packet)
	{
		return PromptAdmissionOwnershipMismatch(app, session, chat, packet).empty();
	}

	bool ConfigureRemoteUamControlRelay(AppState& app, AcpSessionState& session,
	                                    ChatSession& chat, const ExecutionHost& execution_host,
	                                    std::string_view method,
	                                    nlohmann::json* provider_request)
	{
		if (!UamControlService::EnabledForChat(app.settings, chat)) return true;
		const int runner_protocol_version = execution_host.runner_protocol_version == 0 &&
		                                    (session.recovering_remote_turn || session.recovering_remote_process)
		                                        ? 2
		                                        : execution_host.runner_protocol_version;
		if (runner_protocol_version <= 0)
		{
			session.last_error = "The selected remote runner has no negotiated protocol version.";
			return false;
		}
		nlohmann::json local_request = {
		    {"params", {{"mcpServers", nlohmann::json::array()}}}};
		std::string error;
		if (!UamControlService::AppendSessionMcpServer(
		        app, session, chat, method, local_request, &error))
		{
			session.last_error = std::move(error);
			return false;
		}
		if (chat.remote_uam_control_channel_id.empty())
		{
			chat.remote_uam_control_channel_id =
			    PlatformServicesFactory::Instance().process_service.GenerateUuid();
			if (chat.remote_uam_control_channel_id.empty())
				chat.remote_uam_control_channel_id =
				    uam::time::SteadyEpochNanosecondsTokenNow();
			if (!SaveChatQuietly(app, chat))
			{
				UamControlService::RevokeForSession(app, session);
				session.last_error = "Remote UAM control channel could not be saved.";
				return false;
			}
		}

		const nlohmann::json& local_server =
		    local_request["params"]["mcpServers"].back();
		std::vector<std::string> local_argv = {local_server.value("command", "")};
		for (const nlohmann::json& argument :
		     local_server.value("args", nlohmann::json::array()))
			if (argument.is_string()) local_argv.push_back(argument.get<std::string>());
		std::vector<std::pair<std::string, std::string>> local_environment;
		for (const nlohmann::json& entry :
		     local_server.value("env", nlohmann::json::array()))
			if (entry.is_object() && entry.contains("name") && entry["name"].is_string() &&
			    entry.contains("value") && entry["value"].is_string())
				local_environment.emplace_back(entry["name"].get<std::string>(),
				                               entry["value"].get<std::string>());
		const std::string channel_id = "control-" + chat.remote_uam_control_channel_id;
		const std::string control_line = uam::remote::BuildRemoteMcpControlLine(
		    channel_id, std::filesystem::path(local_argv.front()).parent_path(),
		    local_argv, local_environment);
		std::string write_error;
		if (control_line.empty() ||
		    !PlatformServicesFactory::Instance().process_service.WriteToStdioProcess(
		        session, control_line.data(), control_line.size(), &write_error))
		{
			UamControlService::RevokeForSession(app, session);
			session.last_error = write_error.empty()
			    ? "Remote UAM control relay could not be configured."
			    : std::move(write_error);
			return false;
		}
		if (provider_request == nullptr) return true;

		nlohmann::json& request_servers = (*provider_request)["params"]["mcpServers"];
		if (!request_servers.is_array()) request_servers = nlohmann::json::array();
		if (execution_host.platform == "windows" || execution_host.platform == "Windows")
		{
			const std::string runner_root = uam::execution_hosts::RunnerDirectory(
			    execution_host.platform, execution_host.runner_directory);
			request_servers.push_back({
			    {"name", "uam-control"}, {"command", "powershell.exe"},
			    {"args", nlohmann::json::array({
			        "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
			        "-Command", "& (Join-Path $HOME '" + runner_root + "/" +
			            execution_host.runner_version +
			            "/uam-runner.exe') mcp --channel '" + channel_id + "'"})},
			    {"env", nlohmann::json::array()},
			});
		}
		else
		{
			const std::string runner_root = uam::execution_hosts::RunnerDirectory(
			    execution_host.platform, execution_host.runner_directory);
			request_servers.push_back({
			    {"name", "uam-control"}, {"command", "/bin/sh"},
			    {"args", nlohmann::json::array({
			        "-c", "exec \"$HOME/" + runner_root + "/" +
			            execution_host.runner_version +
			            "/uam-runner\" mcp --channel \"$1\" --socket \"$HOME/" + runner_root +
			            "/" + uam::remote::RunnerEndpointName(
			                execution_host.runner_version, runner_protocol_version) +
			            ".sock\"",
			        "uam-control", channel_id})},
			    {"env", nlohmann::json::array()},
			});
		}
		return true;
	}

	bool AddWorkspaceMcpServers(AppState& app, AcpSessionState& session,
	                            std::string_view method, const std::string& cwd,
	                            nlohmann::json& request)
	{
		if (session.model_discovery_only) return true;
		if (method == uam::acp_methods::kThreadStart || method == uam::acp_methods::kThreadResume)
		{
			ChatSession* chat = ChatDomainService().FindChatById(app, session.chat_id);
			if (chat == nullptr || !UamControlService::EnabledForChat(app.settings, *chat)) return true;
			// The installed 0.159.2 schema verifies per-thread config on start and resume.
			if (!CliProviderVersionAtLeast(session.agent_version, "0.159.2"))
			{
				session.last_error = "UAM Control needs Codex 0.159.2 or newer. Update Codex, or turn off UAM Control for this chat.";
				return false;
			}
			nlohmann::json control_request = {{"params", {{"mcpServers", nlohmann::json::array()}}}};
			std::string error;
			if (chat->execution_host_id != uam::execution_hosts::kLocalHostId)
			{
				const ExecutionHost* host = uam::execution_hosts::Find(app.settings.execution_hosts, chat->execution_host_id);
				if (host == nullptr || !uam::remote::CanAttemptRunnerConnection(*host))
				{
					session.last_error = "The selected remote runner is not ready.";
					return false;
				}
				if (!ConfigureRemoteUamControlRelay(app, session, *chat, *host, uam::acp_methods::kSessionNew, &control_request)) return false;
			}
			else if (!UamControlService::AppendSessionMcpServer(app, session, *chat, uam::acp_methods::kSessionNew, control_request, &error))
			{
				session.last_error = std::move(error);
				return false;
			}
			const nlohmann::json& server = control_request["params"]["mcpServers"].back();
			nlohmann::json environment = nlohmann::json::object();
			for (const nlohmann::json& entry : server["env"]) environment[entry["name"].get<std::string>()] = entry["value"];
			request["params"]["config"]["mcp_servers.uam-control"] = {
			    {"command", server["command"]}, {"args", server["args"]}, {"env", std::move(environment)},
			    {"enabled", true}, {"required", true}, {"tool_timeout_sec", 300}, {"default_tools_approval_mode", "approve"}};
			return true;
		}
		const bool accepts = method == "session.create" || method == "session.resume" || method == uam::acp_methods::kSessionNew || method == uam::acp_methods::kSessionLoad || method == uam::acp_methods::kSessionResume;
		if (!accepts)
		{
			return true;
		}
		ChatSession* chat = ChatDomainService().FindChatById(app, session.chat_id);
		if (chat != nullptr && chat->execution_host_id != uam::execution_hosts::kLocalHostId)
		{
			const ExecutionHost* execution_host = uam::execution_hosts::Find(
			    app.settings.execution_hosts, chat->execution_host_id);
			if (execution_host == nullptr || !uam::remote::CanAttemptRunnerConnection(*execution_host))
			{
				session.last_error = "The selected remote runner is not ready.";
				return false;
			}
			return ConfigureRemoteUamControlRelay(
			    app, session, *chat, *execution_host, method, &request);
		}

		// Native launch configuration already owns shared MCP connections for this session.
		if (!session.provider_configuration_directory.empty())
		{
			std::string error;
			if (chat != nullptr && !UamControlService::AppendSessionMcpServer(app, session, *chat, method, request, &error))
			{
				session.last_error = std::move(error);
				return false;
			}
			return true;
		}
		std::string error;
		nlohmann::json servers = uam::mcp_server_config::ResolveForWorkspace(
		    uam::host_scoped_settings::McpServers(app.settings, session.process_execution_host_id), cwd, session.mcp_http_supported,
		    session.mcp_sse_supported, &error, session.process_execution_host_id.empty() ? "local" : session.process_execution_host_id);
		if (servers.is_null())
		{
			session.last_error = std::move(error);
			return false;
		}
		if (chat != nullptr && uam::paths::HasGitWorktreeSource(*chat) &&
		    uam::mcp_server_config::WorkspaceKey(chat->workspace_source_directory) != uam::mcp_server_config::WorkspaceKey(cwd))
		{
			nlohmann::json source_servers = uam::mcp_server_config::ResolveForWorkspace(
			    uam::host_scoped_settings::McpServers(app.settings, session.process_execution_host_id), chat->workspace_source_directory,
			    session.mcp_http_supported, session.mcp_sse_supported, &error, session.process_execution_host_id.empty() ? "local" : session.process_execution_host_id);
			if (source_servers.is_null())
			{
				session.last_error = std::move(error);
				return false;
			}
			for (nlohmann::json& server : source_servers) servers.push_back(std::move(server));
		}
		nlohmann::json& request_servers = request["params"]["mcpServers"];
		if (!request_servers.is_array()) request_servers = nlohmann::json::array();
		for (nlohmann::json& server : servers) request_servers.push_back(std::move(server));
		if (chat != nullptr && !UamControlService::AppendSessionMcpServer(
		                         app, session, *chat, method, request, &error))
		{
			session.last_error = std::move(error);
			return false;
		}
		return true;
	}
} // namespace

bool UpdateAcpStaleWait(AcpSessionState& session, double now_seconds)
{
	if (!session.running || !uam::AcpSessionIsWaitingForInput(session))
	{
		if (session.wait_is_stale || session.wait_started_time_s > 0.0 || !session.wait_stale_reason.empty())
		{
			ResetAcpWaitState(session);
			return true;
		}
		return false;
	}

	if (session.wait_started_time_s <= 0.0)
	{
		session.wait_started_time_s = now_seconds;
	}
	if (session.last_runtime_activity_time_s <= 0.0)
	{
		session.last_runtime_activity_time_s = session.wait_started_time_s;
	}

	const double wait_age = now_seconds - session.wait_started_time_s;
	const double idle_age = now_seconds - session.last_runtime_activity_time_s;
	const bool stale = wait_age >= kAcpStaleWaitSeconds && idle_age >= kAcpStaleWaitSeconds;
	if (!stale)
	{
		return false;
	}

	if (session.wait_is_stale)
	{
		return false;
	}

	session.wait_is_stale = true;
	session.wait_stale_reason = session.waiting_for_permission ? "No runtime activity while waiting for command or tool approval." : "No runtime activity while waiting for user input.";

	std::ostringstream detail;
	detail << "wait_seconds=" << static_cast<int>(wait_age) << "\nidle_seconds=" << static_cast<int>(idle_age) << "\nrequest_id=" << ActiveAcpWaitRequestId(session) << "\ntool_id=" << ActiveAcpWaitToolId(session);
	if (!session.recent_stderr.empty())
	{
		detail << "\nstderr_tail=" << RecentStderrTail(session);
	}
	AppendAcpDiagnostic(session, "wait", session.waiting_for_permission ? "stale_permission_wait" : "stale_user_input_wait", "", ActiveAcpWaitRequestId(session), false, 0, session.wait_stale_reason, detail.str());
	return true;
}

bool SendInitialize(AcpSessionState& session, std::string* error_out)
{
	const int id = session.next_request_id++;
	std::string method;
	nlohmann::json msg = ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpBuildInitialize(session, id);
	if (msg.is_null() || msg.empty())
	{
		return true;
	}
	method = uam::acp_methods::kInitialize;
	session.pending_request_methods[id] = method;
	session.initialize_request_id = id;
	return WriteAcpMessage(session, msg, error_out);
}

void ResetAcpRuntimeState(AppState& app, AcpSessionState& session, ChatSession& chat)
{
	ManagedParentContinuationService::Revoke(app, chat.id, "runtime_reset");
	session.codex_origin_rpc_turn_id.clear();
	session.codex_origin_rpc_turn_serial = 0;
	AgentRunScheduler::InitialTurnSettled(session, "failed", session.last_error);
	RevokeSavedImageRetry(session);
	const bool preserve_native_recovery = (session.recovering_remote_process || session.recovering_remote_turn) && !session.remote_source_exit_reported;
	session.pending_local_launch.reset();
	AbandonPendingLocalSteers(session);
	RestorePendingPromptDelivery(session, chat);
	session.delivered_prompt_recoveries.clear();
	session.defer_prompt_save = false;
	const std::string quota_delivery_id = session.claude_quota_delivery_id;
	if (!session.local_stop_pending && !session.remote_stop_pending && !session.remote_stop_unconfirmed) session.stop_purpose = AcpStopPurpose::Interrupt;
	InterruptUnconfirmedAcpSteers(app, session, chat);
	if (!session.remote_stop_pending && !session.remote_stop_unconfirmed) session.stop_purpose = AcpStopPurpose::Interrupt;
	session.claude_live_background_task_ids.clear();
	if ((!session.recovering_remote_process && !session.recovering_remote_turn) || session.remote_source_exit_reported)
	{
		session.codex_known_child_thread_ids.clear();
		session.codex_live_background_thread_ids.clear();
	}
	session.codex_approvals_reviewer.clear();
	session.provider_protocol_instructions.clear();
	session.initialized = false;
	session.session_ready = false;
	session.load_session_supported = false;
	session.resume_session_supported = false;
	session.mcp_http_supported = false;
	session.mcp_sse_supported = false;
	if (!preserve_native_recovery)
		session.prompt_image_supported = false;
	session.processing = false;
	session.manual_compaction_pending = false;
	session.context_compaction_in_progress = false;
	session.recovering_remote_turn = false;
	session.restart_marker_save_pending = false;
	session.restart_after_remote_stop_cleanup = false;
	session.cancel_requested = false;
	session.cancel_requested_time_s = 0.0;
	session.inactivity_timeout_pending = false;
	session.turn_checkpoint_eligible = false;
	session.turn_checkpoint_preflight_pending = false;
	session.turn_checkpoint_commit_pending = false;
	session.initialize_request_id = 0;
	session.session_setup_request_id = 0;
	ClearAcpStartupModelRequest(session);
	ClearAcpReasoningChangeRequest(session);
	ClearAcpConfigOptionChangeRequest(session);
	ClearAcpModeChangeRequest(session);
	ClearAcpModelChangeRequest(session);
	session.awaiting_model_config_options = false;
	session.prompt_request_id = 0;
	session.cancel_request_id = 0;
	session.current_assistant_message_index = -1;
	session.turn_first_user_message_index = -1;
	session.turn_user_message_index = -1;
	session.turn_assistant_message_index = -1;
	session.turn_serial = chat.execution_host_id == uam::execution_hosts::kLocalHostId
	    ? 0 : std::max(session.turn_serial, chat.remote_turn_serial);
	session.queued_prompt.clear();
	session.queued_skill_spans.clear();
	if (session.attached_skill_preparation != nullptr) session.attached_skill_preparation->cancel.request_stop();
	session.attached_skill_preparation.reset();
	session.turn_computer_use_mode = false;
	session.memory_recall_position.reset();
	session.memory_recall_query.clear();
	session.memory_recall_preparation.reset();
	if (session.remote_context_preparation != nullptr) session.remote_context_preparation->cancel.request_stop();
	session.remote_context_preparation.reset();
	session.claude_input_preparation.reset();
	session.goal_turn_kind.clear();
	session.goal_turn_model_id.clear();
	session.launch_model_options_id.clear();
	session.goal_internal_session = false;
	session.goal_review_turn = false;
	session.goal_review_scheduled = false;
	session.goal_review_goal_id.clear();
	session.goal_review_user_prompt.clear();
	session.goal_review_assistant_text.clear();
	session.goal_review_repair_attempts = 0;
	session.ignore_session_updates_until_ready = false;
	session.codex_resume_fallback_attempted = false;
	session.acp_resume_fallback_attempted = false;
	session.stdout_buffer.clear();
	if (!preserve_native_recovery)
		session.copilot_native_state = nlohmann::json::object();
	if (!preserve_native_recovery)
		session.opencode_native_companion.reset();
	session.stderr_buffer.clear();
	session.stdout_poll_pending = false;
	session.stderr_poll_pending = false;
	session.recent_stderr.clear();
	session.last_runtime_activity_time_s = 0.0;
	session.idle_interaction_started_time_s = 0.0;
	session.last_error.clear();
	session.has_last_exit_code = false;
	session.last_exit_code = 0;
	session.last_process_id.clear();
	session.process_exit_observed = false;
	session.observed_process_exit_code = -1;
	session.remote_runner_protocol_version = 0;
	session.pending_remote_input_receipt_id.clear();
	session.remote_consumed_stderr_cursor = 0;
	session.assistant_replay_prefixes.clear();
	session.load_history_replay_updates.clear();
	session.agent_name.clear();
	session.agent_title.clear();
	session.agent_version.clear();
	session.codex_model_discovery = {};
	session.available_commands.clear();
	session.available_config_options.clear();
	session.pending_request_methods.clear();
	session.pending_steer_requests.clear();
	ResetAcpTurnStreamState(session);
	// A transport reset retains the current turn ownership; a new turn clears it.
	session.claude_quota_delivery_id = quota_delivery_id;
	session.codex_turn_id.clear();
	ResetAcpPendingInteractionState(session);
}

bool ValidateAcpRestartOwnership(const AppState& app, const AcpSessionState& session, const ChatSession& chat, std::string* error_out)
{
	const auto reject = [error_out](const char* message)
	{
		if (error_out != nullptr)
			*error_out = message;
		return false;
	};
	const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
	if (host == nullptr)
		return reject("The selected execution host no longer exists.");
	if (session.chat_id != chat.id || session.provider_id.empty() || session.provider_id != chat.provider_id || session.process_execution_host_id != host->id)
		return reject("The running provider belongs to a different chat, provider or execution host.");
	const bool remote = host->id != execution_hosts::kLocalHostId;
	if (host->transport != (remote ? "ssh" : "local"))
		return reject("The selected execution host transport does not match the running provider.");
	// Private discovery and goal contexts deliberately have their own native session identity.
	if (!session.model_discovery_only && !session.goal_internal_session && !chat.native_session_id.empty() && session.session_id != chat.native_session_id)
		return reject("The running provider belongs to a different native session.");
	if (!session.last_process_id.empty() && session.last_process_id != AcpProcessHandleLabel(session))
		return reject("The running provider process ownership changed before restart.");
	// A matching saved host ID alone cannot grant control over a remote process.
	if (remote && (!chat.remote_process_exists || chat.remote_process_control_token.empty() || session.remote_output_delivery_token != chat.remote_process_control_token || session.remote_runner_protocol_version <= 0 || (host->runner_protocol_version != 0 && host->runner_protocol_version != session.remote_runner_protocol_version)))
		return reject("The running remote provider no longer has matching process authority.");
	if (!chat.routing_assigned_connection_identity.empty() && ExecutionRoutingConnectionIdentity(*host) != chat.routing_assigned_connection_identity)
		return reject("The assigned execution host connection changed before restart.");
	return true;
}

bool StopAcpProcessForRestart(AppState& app, AcpSessionState& session, const ChatSession& chat)
{
	if (!session.running) return true;
	std::string ownership_error;
	if (!ValidateAcpRestartOwnership(app, session, chat, &ownership_error))
	{
		if (session.transport_ownership_error.empty())
			session.last_error = std::move(ownership_error);
		return false;
	}
	auto& process_service = PlatformServicesFactory::Instance().process_service;
	const bool remote = chat.execution_host_id != uam::execution_hosts::kLocalHostId;
	if (remote)
	{
		ChatSession* persisted = ChatDomainService().FindChatById(app, chat.id);
		if (persisted == nullptr && session.ephemeral_model_discovery)
		{
			const std::vector<ChatSession>::iterator found = std::ranges::find_if(app.model_discovery_chats, [&chat](const ChatSession& item) { return item.id == chat.id; });
			if (found != app.model_discovery_chats.end()) persisted = &*found;
		}
		if (persisted != nullptr)
		{
			persisted->remote_restart_pending = true;
			persisted->remote_stop_cleanup_pending = true;
			persisted->remote_process_exists = true;
			if (!SaveChatQuietly(app, *persisted))
			{
				ScheduleChatSave(app, *persisted, 0.0);
				session.restart_marker_save_pending = true;
				session.restart_after_remote_stop_cleanup = true;
				app.status_line = "The remote restart is waiting for recovery data to be saved.";
				return false;
			}
		}
		session.restart_marker_save_pending = false;
		session.restart_after_remote_stop_cleanup = true;
		std::string ignored_error;
		const bool stop_requested = process_service.WriteToStdioProcess(
		    session, uam::remote::kRemoteStopControlLine.data(),
		    uam::remote::kRemoteStopControlLine.size(), &ignored_error);
		auto pending = std::make_unique<PendingAcpRemoteStop>();
		pending->chat_id = chat.id;
		pending->deadline_time_s = GetAppTimeSeconds() + (stop_requested ? kRemoteStopConfirmationTimeoutSeconds : 0.0);
		pending->recoverable_turn = AcpSessionHasActiveTurn(session);
		pending->restart_after_stop = true;
		TransferStdioProcessFields(session, *pending);
		app.pending_acp_remote_stops.push_back(std::move(pending));
		session.running = false;
		session.remote_stop_pending = true;
		session.transport_ownership_error.clear();
		return false;
	}
	QueueAcpProcessStop(app, session, session.chat_id);
	session.running = false;
	session.transport_ownership_error.clear();
	return true;
}

void RestoreRemoteAcpRequests(AcpSessionState& session, const ChatSession& chat)
{
	if (chat.remote_manual_compaction_pending && chat.remote_process_exists && !chat.remote_process_control_token.empty() && session.remote_output_delivery_token == chat.remote_process_control_token && session.process_execution_host_id == chat.execution_host_id && session.provider_id == chat.provider_id)
	{
		session.manual_compaction_pending = true;
		session.processing = true;
		session.turn_user_message_index = -1;
		session.turn_first_user_message_index = -1;
		session.current_assistant_message_index = -1;
		session.turn_assistant_message_index = -1;
		session.turn_checkpoint_eligible = false;
		if (session.protocol_kind == "claude-code-stream-json" && !chat.remote_manual_compaction_boundary_json.empty())
		{
			const auto boundary = nlohmann::json::parse(chat.remote_manual_compaction_boundary_json, nullptr, false);
			if (boundary.is_object() && boundary.contains("sessionId") && boundary["sessionId"].is_string() && boundary["sessionId"] == session.session_id && boundary.contains("summary") && boundary["summary"].is_string() && boundary.contains("identity") && boundary["identity"].is_string() && (!boundary.contains("tokensBefore") || boundary["tokensBefore"].is_number_integer()) && (!boundary.contains("tokensAfter") || boundary["tokensAfter"].is_number_integer()))
			{
				AcpTurnEventState event;
				event.type = "claude_manual_compaction_boundary";
				event.text = boundary["summary"].get<std::string>();
				event.request_id_json = boundary["identity"].get<std::string>();
				event.compaction_tokens_before = boundary.value("tokensBefore", std::int64_t{-1});
				event.compaction_tokens_after = boundary.value("tokensAfter", std::int64_t{-1});
				session.turn_events.push_back(std::move(event));
				session.claude_manual_compaction_boundary_seen = true;
			}
		}
	}
	session.next_request_id = std::max(session.next_request_id, chat.remote_next_request_id);
	session.turn_serial = std::max(session.turn_serial, chat.remote_turn_serial);
	if (chat.remote_turn_reconnect_pending)
		ProviderRuntimeRegistry::ResolveById(session.provider_id).RestoreAcpTurnIdentity(session, chat.remote_active_turn_id);
	for (const AcpRemotePendingRequestState& request : chat.remote_pending_requests)
	{
		session.next_request_id = std::max(session.next_request_id, request.request_id + 1);
		if (request.response_consumed) continue;
		session.pending_request_methods[request.request_id] = request.method;
		if (request.user_message_index >= 0)
		{
			AcpPendingSteerState& steer = session.pending_steer_requests[std::to_string(request.request_id)];
			steer.user_message_index = request.user_message_index;
			steer.turn_serial = request.turn_serial;
			steer.provider_turn_id = request.provider_turn_id;
		}
	}
}

void RetireRemoteAcpRequests(AppState& app, AcpSessionState& session, ChatSession& chat)
{
	// Retain tombstones until the output cursor is saved, even if another event saves first.
	bool changed = false;
	for (AcpRemotePendingRequestState& request : chat.remote_pending_requests)
	{
		if (request.response_consumed || session.pending_request_methods.contains(request.request_id)) continue;
		// Setup responses may stage the next prompt before its receipt registers the wire request.
		const std::shared_ptr<PendingAcpPromptDelivery>& staged = session.pending_prompt_delivery;
		if (staged && staged->remote_host && staged->admission != DurableAdmissionState::Failed &&
		    staged->request_id == request.request_id && staged->turn_serial == request.turn_serial &&
		    staged->method == request.method && OwnsPromptAdmission(app, session, chat, *staged)) continue;
		request.response_consumed = true;
		request.payload.clear();
		changed = true;
	}
	if (changed) (void)SaveChatQuietly(app, chat);
}

bool WriteRemoteAcpRequest(AcpSessionState& session, const AcpRemotePendingRequestState& request)
{
	const std::string line = uam::remote::BuildRemoteInputDeliveryLine(
	    session.remote_output_delivery_token, request.delivery_id, request.payload);
	std::string error;
	if (!line.empty() && PlatformServicesFactory::Instance().process_service.WriteToStdioProcess(
	        session, line.data(), line.size(), &error)) return true;
	session.last_error = "Failed to deliver remote input: " + uam::strings::NonEmptyOrFallback(error, "transport unavailable");
	return false;
}

AcpSessionState& EnsureAcpSessionForChat(AppState& app, const ChatSession& chat)
{
	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
	if (AcpSessionState* existing = FindAcpSessionForChat(app, chat.id); existing != nullptr)
	{
		existing->provider_id = provider.id;
		existing->protocol_kind = ProviderStructuredProtocolOrDefault(provider);
		existing->managed_agent_run_id = chat.agent_run_id;
		return *existing;
	}

	auto session = std::make_unique<AcpSessionState>();
	session->chat_id = chat.id;
	session->provider_id = provider.id;
	session->protocol_kind = ProviderStructuredProtocolOrDefault(provider);
	session->managed_agent_run_id = chat.agent_run_id;
	session->queue_held = chat.acp_queue_held;
	if (session->queue_held)
	{
		const std::size_t dispatched = std::min(chat.acp_dispatched_queued_prompt_count, chat.acp_queued_prompts.size());
		for (std::size_t index = dispatched; index < chat.acp_queued_prompts.size(); ++index)
		{
			const AcpQueuedUserPromptState& prompt = chat.acp_queued_prompts[index];
			if (!prompt.prepared_for_delivery && prompt.claude_quota_continuation_id.empty()) session->queued_user_prompts.push_back(prompt);
		}
	}
	if (chat.execution_host_id != uam::execution_hosts::kLocalHostId)
	{
		session->next_request_id = chat.remote_next_request_id;
		session->turn_serial = chat.remote_turn_serial;
		if (chat.remote_turn_reconnect_pending)
			ProviderRuntimeRegistry::ResolveById(session->provider_id).RestoreAcpTurnIdentity(*session, chat.remote_active_turn_id);
	}
	app.acp_sessions.push_back(std::move(session));
	return *app.acp_sessions.back();
}

bool FailAcpSessionSetupWrite(AppState& app, AcpSessionState& session, ChatSession& chat, const std::string& fallback_message)
{
	UamControlService::RevokeForSession(app, session);
	session.session_setup_request_id = 0;
	FailAcpTurnOrSession(session, &chat,
	                     uam::strings::NonEmptyOrFallback(session.last_error,
	                                                      fallback_message));
	MarkAcpChatUnseenIfBackground(app, chat);
	return false;
}

bool WriteCommittedRemotePrompt(AppState& app, AcpSessionState& session, const ChatSession& chat)
{
	if (session.pending_prompt_delivery || app.pending_chat_save_at_by_chat_id.contains(chat.id) ||
	    !chat.remote_turn_reconnect_pending || chat.remote_prompt_delivery_id.empty() || chat.remote_prompt_delivery_payload.empty())
	{
		session.last_error = "Remote prompt is waiting for its committed recovery marker.";
		return false;
	}
	const std::string line = uam::remote::BuildRemoteInputDeliveryLine(
	    session.remote_output_delivery_token, chat.remote_prompt_delivery_id, chat.remote_prompt_delivery_payload);
	std::string error;
	if (!line.empty() && PlatformServicesFactory::Instance().process_service.WriteToStdioProcess(session, line.data(), line.size(), &error)) return true;
	session.last_error = "Remote prompt could not be queued for confirmed delivery: " +
	    uam::strings::NonEmptyOrFallback(error, "transport unavailable");
	return false;
}

WorktreeLaunchPreparationState PrepareAcpWorkspaceForLaunch(AppState& app, AcpSessionState& session, ChatSession& chat, std::string* error_out)
{
	if (session.running) return WorktreeLaunchPreparationState::Ready;
	// Poll the captured operation before the service can replace a revoked task.
	if (session.worktree_launch_pending && session.worktree_launch_cancellation)
	{
		(void)PollWorktreeLaunchPreparations(app);
		if (session.worktree_launch_cancellation->stop_requested())
		{
			session.worktree_launch_pending = false;
			session.worktree_launch_cancellation.reset();
			session.reconnect_pending = false;
			session.queue_held = chat.acp_queue_held = true;
			session.last_error = "Workspace preparation was canceled or changed. Retry the held input.";
			session.lifecycle_state = kAcpLifecycleError;
			ScheduleChatSave(app, chat, 0.0);
			if (error_out) *error_out = session.last_error;
			return WorktreeLaunchPreparationState::Failed;
		}
	}
	const WorktreeLaunchPreparationResult workspace = PrepareWorktreeForLaunch(app, chat);
	session.worktree_launch_pending = workspace.state == WorktreeLaunchPreparationState::Pending;
	if (session.worktree_launch_pending)
	{
		session.worktree_launch_cancellation = workspace.cancellation;
		session.lifecycle_state = "waiting_workspace";
		return workspace.state;
	}
	session.worktree_launch_cancellation.reset();
	if (workspace.state == WorktreeLaunchPreparationState::Failed)
	{
		session.reconnect_pending = false;
		session.queue_held = chat.acp_queue_held = true;
		session.last_error = workspace.error;
		session.lifecycle_state = kAcpLifecycleError;
		ScheduleChatSave(app, chat, 0.0);
		if (error_out) *error_out = workspace.error;
	}
	return workspace.state;
}

static bool SavedImageRetryLaunchAllowed(const AppState& app, const AcpSessionState& session, const ChatSession& chat, const SavedImageRetryPermission* starting)
{
	const auto& permission = session.saved_image_retry;
	if (!permission)
		return true;
	if (!SavedImageRetrySourceMatches(app, session, chat, *permission))
		return false;
	if (session.saved_image_retry_turn_serial >= 0)
		return session.processing && session.saved_image_retry_turn_serial == session.turn_serial && session.turn_first_user_message_index == permission->row_index && session.turn_user_message_index == permission->row_index;
	if (starting == permission.get())
		return true;
	return !permission->queued_id.empty() && std::ranges::any_of(session.queued_user_prompts, [&permission](const AcpQueuedUserPromptState& queued) { return queued.id == permission->queued_id && queued.revision == permission->queued_revision && !queued.append_user_message && queued.existing_user_message_index == permission->row_index && queued.text == permission->content && queued.attachments == permission->attachments; });
}

bool StartAcpProcessForChat(AppState& app, AcpSessionState& session, ChatSession& chat, std::string* error_out, const SavedImageRetryPermission* starting_saved_retry)
{
	if (!AgentRunScheduler::InitialLaunchAllowed(app, session, chat, error_out))
		return false;
	const auto saved_retry = session.saved_image_retry;
	const int saved_retry_serial = session.saved_image_retry_turn_serial;
	if (!SavedImageRetryLaunchAllowed(app, session, chat, starting_saved_retry))
	{
		RevokeSavedImageRetry(session);
		session.reconnect_pending = false;
		session.last_error = "Saved image Retry ownership changed. Retry the saved message again.";
		if (session.processing)
			FailAcpTurnOrSession(session, &chat, session.last_error);
		if (error_out)
			*error_out = session.last_error;
		return false;
	}
	const WorktreeLaunchPreparationState workspace = PrepareAcpWorkspaceForLaunch(app, session, chat, error_out);
	if (workspace != WorktreeLaunchPreparationState::Ready)
		return workspace == WorktreeLaunchPreparationState::Pending;
	std::shared_ptr<PendingLocalAcpLaunch> prepared = session.pending_local_launch;
	if (prepared && prepared->cold_start) return PollColdLaunch(app, session, chat);
	if (prepared)
	{
		const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
		const std::unordered_map<std::string, std::shared_ptr<WorktreeLaunchPreparationTask>>::const_iterator workspace_preparation = app.worktree_launch_preparations.find(chat.id);
		const auto branch_preparation = app.branch_preparations.find(chat.id);
		if (prepared->incarnation != chat.metadata_incarnation || prepared->root != app.data_root || host == nullptr || !execution_hosts::SameConnection(prepared->host, *host) || prepared->ownership != LocalLaunchOwnership(app, session, chat) || app.worktree_operation_chat_ids.contains(chat.id) || (branch_preparation != app.branch_preparations.end() && !branch_preparation->second.expired()) || (workspace_preparation != app.worktree_launch_preparations.end() && !(workspace_preparation->second->saved && *workspace_preparation->second->saved)))
		{
			session.pending_local_launch.reset();
			session.last_error = "Local launch ownership changed during configuration preparation.";
			session.lifecycle_state = kAcpLifecycleError;
			if (error_out)
				*error_out = session.last_error;
			return false;
		}
		// Configuration may finish before quota settlement. It cannot consume that receipt's launch owner.
		if (!prepared->cold_quota.id.empty() && chat.claude_quota_continuation.state == "continued" &&
		    app.pending_chat_save_at_by_chat_id.contains(chat.id)) return true;
		if (!prepared->version_only && !AcpLaunchConfigurationFinished(session)) return true;
		if (!prepared->version_only && !prepared->success)
		{
			session.pending_local_launch.reset();
			session.last_error = prepared->error;
			session.lifecycle_state = kAcpLifecycleError;
			if (error_out) *error_out = session.last_error;
			return false;
		}
		if (!prepared->control_capability_id.empty())
		{
			const auto control = std::ranges::find(app.uam_control_capabilities, prepared->control_capability_id, &UamControlCapability::id);
			if (control == app.uam_control_capabilities.end() ||
			    !ShouldRetainPendingLocalAcpControlCapability(app, *control, time::SystemEpochMillisecondsNow(), GetAppTimeSeconds()))
			{
				session.pending_local_launch.reset();
				session.last_error = "UAM control setup expired or changed before launch. Retry the message.";
				session.lifecycle_state = kAcpLifecycleError;
				if (error_out) *error_out = session.last_error;
				return false;
			}
		}
	}

	if (session.pending_prompt_delivery && session.pending_prompt_delivery->remote_host)
	{
		const std::shared_ptr<PendingAcpPromptDelivery> packet = session.pending_prompt_delivery;
		if (!OwnsPromptAdmission(app, session, chat, *packet) || packet->admission == DurableAdmissionState::Failed)
		{
			RestorePendingPromptDelivery(session, chat);
			ScheduleChatSave(app, chat, 0.0);
		}
		if (error_out != nullptr) *error_out = "Remote prompt is waiting for its owned durable receipt.";
		return false;
	}

	if (session.local_stop_pending)
	{
		if (error_out != nullptr) *error_out = "The provider is stopping.";
		return false;
	}
	if (!session.model_discovery_only && chat.routing_mode == "automatic")
	{
		if (!ExecutionRoutingOwnsWork(app, chat) && chat.routing_assigned_connection_identity.empty())
		{
			app.execution_routing_waits[chat.id] = "Waiting for routing admission before the first turn.";
			session.lifecycle_state = "waiting_routing";
			return false;
		}
		const ExecutionRoutingDecision routing = AdmitExecutionRouting(app, chat);
		if (routing.status == ExecutionRoutingStatus::Waiting || routing.status == ExecutionRoutingStatus::Invalid)
		{
			session.lifecycle_state = "waiting_routing";
			return false;
		}
	}

	if (!session.model_discovery_only && !session.running && !session.recovering_remote_turn && !session.recovering_remote_process)
	{
		std::string repository_error;
		const RepositoryRouteLaunchState repository = PrepareRepositoryRouteLaunch(app, chat, repository_error);
		if (repository != RepositoryRouteLaunchState::Ready)
		{
			if (repository == RepositoryRouteLaunchState::Pending)
			{
				session.lifecycle_state = "waiting_routing";
				session.reconnect_pending = true;
				session.reconnect_not_before_time_s = GetAppTimeSeconds() + 0.1;
				return true;
			}
			session.last_error = repository_error;
			session.lifecycle_state = kAcpLifecycleError;
			if (error_out) *error_out = repository_error;
			return false;
		}
	}

	if (chat.side_cleanup_requested)
	{
		if (error_out != nullptr) *error_out = "This side chat is closing.";
		return false;
	}
	if (chat.side_cleanup_stop_finished)
	{
		bool stopping = !chat.side_cleanup_stop_finished->load();
		for (const AsyncAcpProcessStopTask& task : app.acp_process_stop_tasks)
			if (task.finished == chat.side_cleanup_stop_finished && task.observe_exit && !task.result->exit_confirmed) stopping = true;
		if (stopping)
		{
			if (error_out != nullptr) *error_out = "The previous side-chat process is still stopping.";
			return false;
		}
	}
	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
	const ProviderProfile* selected_provider = ProviderResolutionService().ProviderForChat(app, chat);
	if (selected_provider != nullptr && !selected_provider->supports_structured)
	{
		if (error_out != nullptr) *error_out = "This provider supports terminal chat only.";
		return false;
	}
	if (const std::string update_error = ProviderCliLaunchBlockReason(app, provider.id, chat.execution_host_id); !update_error.empty())
	{
		if (error_out != nullptr) *error_out = update_error;
		return false;
	}
	if (!session.model_discovery_only && !ValidateCapturedAcpAgentPolicy(app, chat,
	        session.active_uam_agent_id, session.active_uam_agent_definition_snapshot,
	        session.active_uam_agent_execution_capability, error_out)) return false;
	if (session.running)
	{
		return true;
	}
	if (!provider_availability::IsEnabled(app.settings, provider.id, chat.execution_host_id))
	{
		if (error_out != nullptr) *error_out = provider_availability::DisabledReason();
		return false;
	}
	if (!session.goal_internal_session && !session.model_discovery_only && !PrepareCodexThreadForRuntimeLaunch(app, chat, error_out))
	{
		return false;
	}
	const bool remote_stop_pending = session.remote_stop_pending || std::ranges::any_of(
	    app.pending_acp_remote_stops,
	    [&](const auto& pending) { return pending != nullptr && pending->chat_id == chat.id; });
	if (remote_stop_pending)
	{
		if (error_out != nullptr) *error_out = "The remote turn is still stopping.";
		return false;
	}
	if (session.remote_stop_unconfirmed && !session.recovering_remote_turn &&
	    !session.recovering_remote_process)
	{
		if (error_out != nullptr) *error_out = "The previous remote stop was not confirmed.";
		return false;
	}
	const ExecutionHost* execution_host =
	    uam::execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
	const bool remote = execution_host != nullptr && execution_host->id != uam::execution_hosts::kLocalHostId;
	const bool migrate_copilot_wire = remote && uam::copilot::NeedsNativeWireMigration(chat, provider.id);
	if (migrate_copilot_wire)
	{
		if (execution_host->runner_protocol_version < 3 || chat.remote_process_control_token.empty())
		{
			session.lifecycle_state = kAcpLifecycleError;
			session.last_error = "The previous remote Copilot connection needs a confirmed owned stop before switching to the SDK server. Reconnect or update its SSH helper first.";
			if (error_out)
				*error_out = session.last_error;
			return false;
		}
		// Attach only to stop the old process: never initialize SDK RPC, replay
		// old ACP prompts, or create another legacy CLI through this bridge.
		session.recovering_remote_process = true;
	}
	if (!session.managed_agent_run_id.empty() && session.managed_launch_attempted &&
	    !(remote && (session.recovering_remote_turn ||
	                 session.recovering_remote_process)))
	{
		if (error_out != nullptr) *error_out = "Managed agent runs are never relaunched automatically.";
		return false;
	}
	if (execution_host == nullptr ||
	    (remote && !uam::remote::CanAttemptRunnerConnection(*execution_host) &&
	     !session.recovering_remote_turn && !session.recovering_remote_process))
	{
		const std::string startup_error = remote ? "The selected remote runner is not ready. Recheck it in Settings."
		                                         : "The selected execution host no longer exists.";
		session.lifecycle_state = kAcpLifecycleError;
		session.last_error = startup_error;
		if (error_out != nullptr) *error_out = startup_error;
		return false;
	}
	if (remote && session.recovering_remote_turn && execution_host->runner_protocol_version < 3)
	{
		const std::string startup_error =
		    "This SSH helper cannot safely resume the active turn. Update the helper in Settings.";
		session.lifecycle_state = kAcpLifecycleError;
		session.last_error = startup_error;
		if (error_out != nullptr) *error_out = startup_error;
		return false;
	}
	if (remote && session.model_discovery_only && session.ephemeral_model_discovery && execution_host->runner_protocol_version < 3)
	{
		const std::string startup_error = "This SSH helper cannot safely clean up background model discovery. Update the helper in Settings.";
		session.lifecycle_state = kAcpLifecycleError;
		session.last_error = startup_error;
		if (error_out != nullptr) *error_out = startup_error;
		return false;
	}
	// Launch-configured thinking and remote OpenCode discovery need the executing host's version before capture.
	// Reuse the existing asynchronous version worker and launch ownership packet; reconnect never reconfigures.
	const std::string configuration_provider_id = provider_ids::CanonicalCliProviderLookupId(provider.id);
	const bool needs_option_version = ProviderRuntimeRegistry::Resolve(provider).ModelOptionsRequireRestart() && !chat.reasoning_effort.empty() && (session.goal_turn_model_id.empty() || session.goal_turn_model_id == chat.model_id);
	if (remote && execution_host->runner_protocol_version < 3 && needs_option_version && !session.recovering_remote_turn && !session.recovering_remote_process)
	{
		session.last_error = "Update the SSH helper before launching Gemini with a named thinking level.";
		session.lifecycle_state = kAcpLifecycleError;
		if (error_out)
			*error_out = session.last_error;
		return false;
	}
	if ((!remote || execution_host->runner_protocol_version >= 3) && !session.model_discovery_only && !session.recovering_remote_turn && !session.recovering_remote_process && ((remote && configuration_provider_id == provider_ids::kOpenCodeCli) || needs_option_version))
	{
		ProviderCliCompatibilityService versions;
		versions.Poll(app);
		const std::string key = CliProviderVersionStateKey(provider.id, execution_host->id);
		CliProviderVersionState& version = app.runtime_cli_versions_by_provider_id[key];
		const bool checked = version.checked && execution_hosts::SameConnection(version.execution_host, *execution_host);
		const auto fail_version = [&](const std::string& error)
		{
			session.pending_local_launch.reset();
			session.reconnect_pending = false;
			session.queue_held = chat.acp_queue_held = true;
			session.last_error = "Could not prepare " + configuration_provider_id + " on this host: " + error;
			session.lifecycle_state = kAcpLifecycleError;
			ScheduleChatSave(app, chat, 0.0);
			if (error_out) *error_out = session.last_error;
			return false;
		};
		if (checked && prepared && !version.check_error.empty()) return fail_version(version.check_error);
		if (checked && needs_option_version && version.check_error.empty() && !version.supported)
			return fail_version("The installed CLI does not satisfy the native version manager's compatibility policy.");
		if (checked && version.check_error.empty())
		{
			std::erase(app.runtime_cli_version_check_queue, std::pair<std::string, std::string>{configuration_provider_id, execution_host->id});
			if (prepared && prepared->version_only)
			{
				session.pending_local_launch.reset();
				prepared.reset();
			}
		}
		else
		{
			if (!prepared)
			{
				prepared = std::make_shared<PendingLocalAcpLaunch>();
				prepared->incarnation = chat.metadata_incarnation;
				prepared->root = app.data_root;
				prepared->host = *execution_host;
				prepared->ownership = LocalLaunchOwnership(app, session, chat);
				prepared->version_only = true;
				session.pending_local_launch = prepared;
				if (!execution_hosts::SameConnection(version.execution_host, *execution_host)) version = {};
				version.provider_id = configuration_provider_id;
				version.execution_host = *execution_host;
				version.checked = false; // A previous failed check is retried only by a new owned launch attempt.
			}
			const std::pair<std::string, std::string> target{configuration_provider_id, execution_host->id};
			const bool checking_target = app.runtime_cli_version_check_task.running &&
			    app.runtime_cli_version_provider_id == target.first &&
			    execution_hosts::SameConnection(app.runtime_cli_version_check_task.execution_host, *execution_host);
			if (!checking_target)
			{
				std::erase(app.runtime_cli_version_check_queue, target);
				if (app.runtime_cli_version_check_task.running) app.runtime_cli_version_check_queue.push_front(target);
				else
				{
					std::string error;
					if (!versions.StartProviderVersionCheck(app, target.first, true, target.second, &error)) return fail_version(error);
				}
			}
			session.lifecycle_state = kAcpLifecycleStarting;
			return true;
		}
	}
	if (!session.model_discovery_only && !session.recovering_remote_turn && !session.recovering_remote_process)
	{
		const ChatBaselineReadiness baseline = PrepareChatRepositoryBaseline(app, chat, error_out);
		if (baseline == ChatBaselineReadiness::Pending || baseline == ChatBaselineReadiness::Failed) return false;
	}
	if (!remote)
	{
		ProviderCliCompatibilityService().Poll(app);
		const std::string compatibility_error = ProviderRuntimeRegistry::Resolve(provider).LocalCliCompatibilityError(app);
		if (!compatibility_error.empty())
		{
			session.provider_usage.context_usage = {};
			session.provider_id = provider.id;
			session.protocol_kind = ProviderStructuredProtocolOrDefault(provider);
			session.lifecycle_state = kAcpLifecycleError;
			session.last_error = compatibility_error;
			AppendAcpDiagnostic(session, "process_launch", "version_blocked", "", "", false, 0, compatibility_error);
			if (error_out != nullptr)
			{
				*error_out = compatibility_error;
			}
			return false;
		}
	}

	const bool retrying_undelivered_prompt = session.processing && session.prompt_request_id == 0 && !session.queued_prompt.empty();
	const bool recovering_remote_turn = session.recovering_remote_turn;
	const bool recovering_remote_process = session.recovering_remote_process;
	const bool was_processing = session.processing;
	const std::string pending_prompt = session.queued_prompt;
	const std::vector<attached_skills::Span> pending_skill_spans = session.queued_skill_spans;
	const int turn_first_user_message_index = session.turn_first_user_message_index;
	const int turn_user_message_index = session.turn_user_message_index;
	const int current_assistant_message_index = session.current_assistant_message_index;
	const int turn_assistant_message_index = session.turn_assistant_message_index;
	const int turn_serial = session.turn_serial;
	const std::string goal_turn_kind = session.goal_turn_kind;
	const std::string goal_turn_model_id = session.goal_turn_model_id;
	const bool goal_internal_session = session.goal_internal_session;
	const bool goal_review_turn = session.goal_review_turn;
	const bool goal_review_scheduled = session.goal_review_scheduled;
	const std::string goal_review_goal_id = session.goal_review_goal_id;
	const std::string goal_review_user_prompt = session.goal_review_user_prompt;
	const std::string goal_review_assistant_text = session.goal_review_assistant_text;

	PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(session);
	if (recovering_remote_turn || recovering_remote_process)
	{
		session.running = false;
		session.initialized = false;
		session.session_ready = false;
		session.stdout_buffer.clear();
		session.stderr_buffer.clear();
		// Diagnostics belong to the proxy instance that just disconnected. Do not
		// let a prior "remote process does not exist" line classify the replacement
		// bridge's exit as permanent remote-process loss.
		session.recent_stderr.clear();
		session.stdout_poll_pending = false;
		session.stderr_poll_pending = false;
	}
	else if (!prepared)
	{
		ResetAcpRuntimeState(app, session, chat);
		// Preserve this invocation across its own launch reset, never create a second permission.
		if (saved_retry && SavedImageRetrySourceMatches(app, session, chat, *saved_retry))
		{
			session.saved_image_retry = saved_retry;
			session.saved_image_retry_turn_serial = saved_retry_serial;
		}
		// The selected launch model must survive transport reset before async ownership is captured.
		session.goal_turn_model_id = goal_turn_model_id;
	}
	if (retrying_undelivered_prompt || recovering_remote_turn || recovering_remote_process)
	{
		if (retrying_undelivered_prompt)
		{
			session.queued_prompt = pending_prompt;
			session.queued_skill_spans = pending_skill_spans;
		}
		session.processing = was_processing;
		session.recovering_remote_turn = recovering_remote_turn;
		session.recovering_remote_process = recovering_remote_process;
		session.turn_first_user_message_index = turn_first_user_message_index;
		session.turn_user_message_index = turn_user_message_index;
		session.current_assistant_message_index = current_assistant_message_index;
		session.turn_assistant_message_index = turn_assistant_message_index;
		session.turn_serial = turn_serial;
		session.goal_turn_kind = goal_turn_kind;
		session.goal_turn_model_id = goal_turn_model_id;
		session.goal_internal_session = goal_internal_session;
		session.goal_review_turn = goal_review_turn;
		session.goal_review_scheduled = goal_review_scheduled;
		session.goal_review_goal_id = goal_review_goal_id;
		session.goal_review_user_prompt = goal_review_user_prompt;
		session.goal_review_assistant_text = goal_review_assistant_text;
	}
	session.chat_id = chat.id;
	session.provider_usage.context_usage = {};
	session.provider_id = provider.id;
	session.process_execution_host_id = chat.execution_host_id;
	if (!recovering_remote_turn && !recovering_remote_process)
	{
		session.model_catalog_scope_key = app.provider_model_catalog == nullptr ? std::string{} :
		    app.provider_model_catalog->CaptureScopeKey(session.provider_id,
		        uam::paths::ResolveWorkspaceRootPath(app, chat).generic_string(), chat.execution_host_id);
	}
	session.protocol_kind = ProviderStructuredProtocolOrDefault(provider);
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(session.provider_id);
	const std::string codex_resume_id = !session.goal_internal_session && std::strcmp(runtime.AcpProtocolKind(), "codex-app-server") == 0 ? runtime.OnAcpValidateResumeId(chat) : std::string{};
	const std::string acp_resume_id = !session.goal_internal_session && codex_resume_id.empty() ? ResolvedAcpResumeIdForChat(app, chat) : std::string{};
	const std::string& validated_resume_id = codex_resume_id.empty() ? acp_resume_id : codex_resume_id;
	if (!AgentRunScheduler::ManagedResumeIdentityAllowed(app, session, chat, validated_resume_id))
	{
		if (error_out) *error_out = session.last_error;
		return false;
	}
	session.session_id = validated_resume_id;
	session.codex_thread_id = codex_resume_id;
	const bool defer_cold_launch = !session.model_discovery_only && !saved_retry && !migrate_copilot_wire;
	// Persist validated resume identity before launching a process bound to that identity.
	if (!session.goal_internal_session && !session.model_discovery_only && !recovering_remote_turn && !recovering_remote_process && !chat.native_session_id.empty() && chat.native_session_id != session.session_id)
	{
		const std::string previous_id = chat.native_session_id;
		chat.native_session_id = session.session_id;
		if (!defer_cold_launch && !SaveChatQuietly(app, chat, AcpChatSavePurpose::PreserveQueuedIntent))
		{
			chat.native_session_id = previous_id;
			session.lifecycle_state = kAcpLifecycleError;
			session.last_error = "Could not save validated session recovery data before provider launch.";
			if (error_out)
				*error_out = session.last_error;
			return false;
		}
		if (session.session_id.empty())
			AppendInvalidResumeDiagnostic(session, previous_id);
	}
	session.lifecycle_state = kAcpLifecycleStarting;

	std::string startup_error;
	const std::filesystem::path workspace_root = uam::paths::ResolveWorkspaceRootPath(app, chat);
	ChatSession launch_chat = remote ? chat : LocalLaunchMetadata(chat);
	launch_chat.computer_use_globally_enabled = app.settings.computer_use_globally_enabled;
	if (session.active_uam_agent_execution_capability == "uam-central-native-setup") launch_chat.uam_agent_id = session.active_uam_agent_id;
	if (session.goal_review_turn || session.active_uam_agent_workspace_access == "read") launch_chat.approval_mode = uam::approval_modes::kPlanApprovalMode;
	if (!session.goal_turn_model_id.empty()) launch_chat.model_id = session.goal_turn_model_id;
	if (runtime.ModelOptionsRequireRestart() && launch_chat.model_id != chat.model_id)
		launch_chat.reasoning_effort.clear();
	const bool defer_configuration = ShouldDeferLocalAcpConfiguration(app, session, chat);
	std::vector<std::string> launch_argv = defer_configuration ? std::vector<std::string>{} : BuildAcpLaunchArgv(provider, launch_chat);
	std::vector<std::pair<std::string, std::string>> launch_environment =
	    defer_configuration ? uam::provider_setup::Environment{} : uam::provider_runtime_internal::ProviderChildEnvironmentOverrides(provider);
	const std::vector<std::pair<std::string, std::string>> runtime_environment =
	    defer_configuration ? uam::provider_setup::Environment{} : runtime.BuildStructuredLaunchEnvironment(provider, launch_chat);
	launch_environment.insert(launch_environment.end(), runtime_environment.begin(), runtime_environment.end());
	if (!prepared) session.active_uam_agent_adapter_directory.clear();
	if (!prepared && !session.model_discovery_only && !session.active_uam_agent_instructions.empty() &&
	    session.active_uam_agent_execution_capability != "uam-central-native-setup")
	{
		if (remote)
		{
			session.active_uam_agent_execution_capability = "uam-prompt-injected";
		}
		else
		{
			ProviderAgentRuntimeAdapter adapter;
			if (!AgentDefinitionService::PrepareRuntimeAdapter(
			        app.data_root, chat.id, provider.id, session.active_uam_agent_id,
			        session.active_uam_agent_instructions, &adapter, &startup_error,
			        &session.active_uam_agent_definition_snapshot))
			{
				session.lifecycle_state = kAcpLifecycleError;
				session.last_error = startup_error;
				if (error_out != nullptr) *error_out = startup_error;
				return false;
			}
			session.active_uam_agent_execution_capability = adapter.execution_capability;
			session.active_uam_agent_adapter_directory = adapter.directory;
			launch_argv.insert(launch_argv.end(), adapter.launch_arguments.begin(), adapter.launch_arguments.end());
			launch_environment.insert(launch_environment.end(), adapter.launch_environment.begin(),
			                          adapter.launch_environment.end());
			std::string permission_json;
			if (!AgentDefinitionService::CapturedOpenCodePermission(session.active_uam_agent_id,
			        session.active_uam_agent_definition_snapshot, permission_json, &startup_error))
			{
				if (error_out) *error_out = startup_error;
				return false;
			}
			if (!permission_json.empty())
			{
				const nlohmann::ordered_json source_permission = nlohmann::ordered_json::parse(permission_json);
				const auto permission_entry = std::find_if(launch_environment.rbegin(), launch_environment.rend(),
				    [](const std::pair<std::string, std::string>& entry) { return entry.first == "OPENCODE_PERMISSION"; });
				if (permission_entry == launch_environment.rend())
				{
					if (error_out) *error_out = "The native OpenCode permission boundary is unavailable.";
					return false;
				}
				nlohmann::ordered_json permission = nlohmann::ordered_json::parse(permission_entry->second, nullptr, false);
				if (!permission.is_object())
				{
					if (error_out) *error_out = "The native OpenCode permission boundary is invalid.";
					return false;
				}
				for (const auto& rule : source_permission.items())
					if (!permission.contains(rule.key()) || permission[rule.key()] != "deny") permission[rule.key()] = rule.value();
				permission["task"] = "deny";
				permission["uam-computer_computer_observe"] = "deny";
				permission["uam-computer_computer_action"] = "deny";
				permission_entry->second = permission.dump();
			}
		}
	}
	nlohmann::json launch_control_servers = nlohmann::json::array();
	std::function<void(AcpSessionState&)> start_control_relay;
	AppSettings configuration_settings = app.settings;
	if (!prepared && !session.model_discovery_only && (provider.id == uam::provider_ids::kClaudeCli || provider.id == uam::provider_ids::kAntigravityCli) && UamControlService::EnabledForChat(app.settings, chat))
	{
		nlohmann::json control_request = {{"params", {{"mcpServers", nlohmann::json::array()}}}};
		if (!UamControlService::AppendSessionMcpServer(app, session, chat, uam::acp_methods::kSessionNew, control_request, &startup_error))
		{
			session.lifecycle_state = kAcpLifecycleError;
			session.last_error = startup_error;
			if (error_out) *error_out = startup_error;
			return false;
		}
		nlohmann::json server = control_request["params"]["mcpServers"].back();
		if (remote)
		{
			const ExecutionHost host = *execution_host;
			if (chat.remote_uam_control_channel_id.empty())
			{
				chat.remote_uam_control_channel_id = PlatformServicesFactory::Instance().process_service.GenerateUuid();
				if (chat.remote_uam_control_channel_id.empty() || (!defer_cold_launch && !SaveChatQuietly(app, chat)))
				{
					UamControlService::RevokeForSession(app, session);
					session.last_error = "Could not save the remote UAM tool connection.";
					if (error_out) *error_out = session.last_error;
					return false;
				}
			}
			const std::string channel = (provider.id == uam::provider_ids::kClaudeCli ? "claude-control-" : "antigravity-control-") + chat.remote_uam_control_channel_id;
			std::vector<std::string> control_argv{server["command"].get<std::string>()};
			for (const nlohmann::json& argument : server["args"]) control_argv.push_back(argument.get<std::string>());
			uam::provider_setup::Environment control_environment;
			for (const nlohmann::json& entry : server["env"]) control_environment.emplace_back(entry["name"].get<std::string>(), entry["value"].get<std::string>());
			const std::string spec = uam::remote::BuildProcessProxySpec(channel, uam::paths::PathFromUtf8(control_argv.front()).parent_path(), control_argv, control_environment);
			start_control_relay = [host, spec](AcpSessionState& target)
			{
				target.uam_control_relay = std::make_unique<std::jthread>([host, spec](std::stop_token stop)
				{
					uam::remote::RunLocalMcpRelayFromSpec(stop, host.ssh_alias, host.platform, host.runner_version, host.runner_directory, host.runner_protocol_version, spec);
				});
			};
			if (!defer_cold_launch) { start_control_relay(session); start_control_relay = {}; }
			server = uam::remote::BuildRemoteMcpServer(channel, host.platform, host.runner_version, host.runner_directory, host.runner_protocol_version);
		}
		launch_control_servers.push_back(std::move(server));
	}
	// Antigravity has no session/new MCP payload; deliver the existing local-only
	// computer server through its isolated native launch configuration instead.
	if (!prepared && !session.model_discovery_only && !remote && provider.id == uam::provider_ids::kAntigravityCli)
		for (const nlohmann::json& server : uam::computer_use::AcpMcpServers(launch_chat))
			if (std::ranges::none_of(launch_control_servers, [&](const nlohmann::json& existing) { return existing.value("name", "") == server.value("name", ""); }))
				launch_control_servers.push_back(server);
	if (!launch_control_servers.empty() && provider.id != uam::provider_ids::kAntigravityCli && !configuration_settings.central_provider_configuration.enabled)
	{
		configuration_settings.central_provider_configuration = {};
		configuration_settings.central_provider_configuration.enabled = true;
	}
	ProviderConfigurationBundle configuration_bundle;
	const std::string configuration_provider_version = CheckedProviderConfigurationVersion(app, provider.id, *execution_host);
	if (defer_configuration && !prepared)
	{
		const std::shared_ptr<PendingLocalAcpLaunch> task = std::make_shared<PendingLocalAcpLaunch>();
		task->incarnation = chat.metadata_incarnation;
		task->root = app.data_root;
		task->host = *execution_host;
		task->ownership = LocalLaunchOwnership(app, session, chat);
		if (!app.claude_quota_dispatch_id.empty()) task->cold_quota = chat.claude_quota_continuation;
		const auto control = std::ranges::find(app.uam_control_capabilities, session.uam_control_capability_id, &UamControlCapability::id);
		if (control != app.uam_control_capabilities.end())
			if (const ChatSession* root = ChatDomainService().FindChatById(app, control->chat_id))
			{
				task->control_session = &session;
				task->control_capability_id = control->id;
				task->control_manager_id = app.uam_control_manager_id;
				task->control_root_chat_id = root->id;
				task->control_root_incarnation = root->metadata_incarnation;
				task->control_capability_expiry_epoch_ms = control->expires_at_epoch_ms;
				const int timeout = app.settings.acp_setup_inactivity_timeout_seconds <= 0
				    ? settings::kDefaultAcpSetupInactivityTimeoutSeconds
				    : std::clamp(app.settings.acp_setup_inactivity_timeout_seconds,
				        settings::kMinAcpSetupInactivityTimeoutSeconds, settings::kMaxAcpSetupInactivityTimeoutSeconds);
				task->control_retention_deadline_s = GetAppTimeSeconds() + timeout;
			}
		const bool central_native = session.active_uam_agent_execution_capability == "uam-central-native-setup";
		const std::string central_instructions = session.active_uam_agent_instructions;
		session.pending_local_launch = task;
		app.local_acp_launch_tasks.push_back(task);
		PendingLocalAcpLaunch* result = task.get();
		try
		{
			task->worker = std::make_unique<std::jthread>([result, provider, launch_chat = std::move(launch_chat), configuration_settings,
			    workspace_root, launch_argv = std::move(launch_argv), launch_environment = std::move(launch_environment),
			    launch_control_servers, central_native, central_instructions, configuration_provider_version](std::stop_token) mutable
			{
				try
				{
					result->argv = BuildAcpLaunchArgv(provider, launch_chat);
					result->argv.insert(result->argv.end(), launch_argv.begin(), launch_argv.end());
					result->environment = uam::provider_runtime_internal::ProviderChildEnvironmentOverrides(provider);
					const uam::provider_setup::Environment runtime_environment = ProviderRuntimeRegistry::ResolveById(provider.id).BuildStructuredLaunchEnvironment(provider, launch_chat);
					result->environment.insert(result->environment.end(), runtime_environment.begin(), runtime_environment.end());
					result->environment.insert(result->environment.end(), launch_environment.begin(), launch_environment.end());
					result->success = PrepareProviderConfiguration(result->root, configuration_settings, launch_chat, result->host, workspace_root, result->bundle, result->error, central_native ? &central_instructions : nullptr, configuration_provider_version, !launch_control_servers.empty()) && (result->bundle.local_directory.empty() || uam::provider_setup::Apply(provider.id, result->bundle.local_directory, workspace_root, result->argv, result->environment, result->error, launch_control_servers, &result->instructions));
				}
				catch (const std::exception& exception) { result->error = std::string("Local configuration preparation failed: ") + exception.what(); }
				catch (...) { result->error = "Local configuration preparation failed."; }
				result->bundle.files.clear();
				result->finished.store(true, std::memory_order_release);
			});
		}
		catch (const std::exception& exception)
		{
			task->error = std::string("Could not start local configuration preparation: ") + exception.what();
			task->finished.store(true, std::memory_order_release);
			session.pending_local_launch.reset();
			session.last_error = task->error;
			if (error_out) *error_out = session.last_error;
			return false;
		}
		return true;
	}
	if (prepared)
	{
		if (prepared->ownership != LocalLaunchOwnership(app, session, chat))
		{
			session.pending_local_launch.reset();
			session.last_error = "Local launch ownership changed before process creation.";
			const nlohmann::json captured = nlohmann::json::parse(prepared->ownership, nullptr, false);
			const nlohmann::json current = nlohmann::json::parse(LocalLaunchOwnership(app, session, chat), nullptr, false);
			static constexpr std::string_view fields[] = {
			    "provider", "command", "runtime_flags", "resume_argument", "structured_support",
			    "protocol", "execution_mode", "output_mode", "bootstrap", "bootstrap_path",
			    "auto_worktree", "base_ref", "base_override", "svn_update", "branch", "chat",
			    "birth", "chat_provider", "native_session", "host", "workspace", "source_workspace",
			    "worktree", "isolation", "routing", "routing_connection", "model", "approval",
			    "extra_flags", "goal", "goal_revision", "agent", "small_model", "reasoning", "tier",
			    "safety", "turn_model", "active_agent", "agent_definition", "agent_instructions",
			    "agent_capability", "agent_access", "last_process", "session", "process",
			    "model_discovery", "internal_goal", "recovering_turn", "recovering_process",
			    "settings", "delivery_configuration"};
			std::string changed;
			if (captured.is_array() && current.is_array() && captured.size() == std::size(fields) && current.size() == std::size(fields))
				for (std::size_t index = 0; index < std::size(fields); ++index)
					if (captured[index] != current[index])
					{
						if (!changed.empty()) changed += ',';
						changed += fields[index];
					}
			AppendAcpDiagnostic(session, "process_launch", "ownership_changed", "", "", false, 0,
			    session.last_error, "changed_fields=" + (changed.empty() ? std::string("unknown") : changed));
			if (error_out) *error_out = session.last_error;
			return false;
		}
		launch_argv = prepared->argv;
		launch_environment = prepared->environment;
		configuration_bundle = std::move(prepared->bundle);
		session.provider_protocol_instructions = prepared->instructions;
		session.pending_local_launch.reset();
	}
	if (!SavedImageRetryLaunchAllowed(app, session, chat, starting_saved_retry))
	{
		RevokeSavedImageRetry(session);
		session.last_error = "Saved image Retry ownership changed before process creation.";
		if (error_out)
			*error_out = session.last_error;
		return false;
	}
	if (!prepared && !session.model_discovery_only && !session.recovering_remote_turn && !session.recovering_remote_process && (!PrepareProviderConfiguration(app.data_root, configuration_settings, launch_chat, *execution_host, workspace_root, configuration_bundle, startup_error, session.active_uam_agent_execution_capability == "uam-central-native-setup" ? &session.active_uam_agent_instructions : nullptr, configuration_provider_version, !launch_control_servers.empty()) || (!remote && !configuration_bundle.local_directory.empty() && !uam::provider_setup::Apply(provider.id, configuration_bundle.local_directory, workspace_root, launch_argv, launch_environment, startup_error, launch_control_servers, &session.provider_protocol_instructions))))
	{
		session.lifecycle_state = kAcpLifecycleError;
		session.last_error = startup_error;
		if (error_out) *error_out = startup_error;
		return false;
	}
	if (!session.recovering_remote_turn && !session.recovering_remote_process) session.provider_configuration_directory = configuration_bundle.local_directory;
	for (UamControlCapability& capability : app.uam_control_capabilities)
		if (capability.id == session.uam_control_capability_id) capability.provider_configuration_directory = session.provider_configuration_directory;
	if (session.protocol_kind == provider_profile_constants::kProtocolOpenCodeAcp && !session.model_discovery_only && session.active_uam_agent_execution_capability != "opencode-native-agent-config")
	{
		const bool recovering = session.recovering_remote_turn || session.recovering_remote_process;
		if (!recovering && !opencode_native::SupportsCompanionLaunch(launch_argv, execution_host->platform))
		{
			session.opencode_native_companion.reset();
			chat.remote_opencode_native_port = 0;
			chat.remote_opencode_native_token.clear();
			AppendAcpDiagnostic(session, "native_permission", "task_unavailable_custom_launch", "", "", false, 0, "This custom OpenCode launch retains basic ACP and its configured network policy; native Task stays disabled without a verified native companion route.");
		}
		else
		{
			opencode_native::Endpoint endpoint;
			if (remote && recovering)
				endpoint = {chat.remote_opencode_native_port, chat.remote_opencode_native_token};
			else
			{
				endpoint.token = PlatformServicesFactory::Instance().process_service.GenerateUuid();
				endpoint.port = 49152 + static_cast<int>(hashing::Fnv1a64(endpoint.token) % 16384);
			}
			if (!opencode_native::ValidEndpoint(endpoint))
			{
				if (!recovering)
				{
					session.last_error = "The private OpenCode native companion could not create process authority.";
					if (error_out != nullptr)
						*error_out = session.last_error;
					return false;
				}
				session.opencode_native_companion.reset();
				AppendAcpDiagnostic(session, "native_permission", "task_unavailable_until_restart", "", "", false, 0, "This existing OpenCode process has no authenticated native companion; native Task stays disabled until a deliberate chat restart.");
			}
			else
			{
				session.opencode_native_companion = OpenCodeNativeActivityService::CreateCompanion(endpoint);
				if (!recovering)
				{
					launch_argv.insert(launch_argv.end(), {"--hostname", "127.0.0.1", "--port", std::to_string(endpoint.port), "--no-mdns"});
					launch_environment.emplace_back("OPENCODE_SERVER_USERNAME", "opencode");
					launch_environment.emplace_back("OPENCODE_SERVER_PASSWORD", endpoint.token);
					if (remote)
					{
						chat.remote_opencode_native_port = endpoint.port;
						chat.remote_opencode_native_token = endpoint.token;
					}
				}
			}
		}
	}
	// Configuration preparation may yield; retain resume-only authority at the actual launch seam.
	if (!AgentRunScheduler::ManagedResumeIdentityAllowed(app, session, chat, session.session_id))
	{
		if (error_out) *error_out = session.last_error;
		return false;
	}
	const std::string launch_detail = "host=" + execution_host->id +
	                                  ", cwd=" + AcpWorkingDirectoryString(workspace_root) +
	                                  ", argv=" + JoinAcpArgvForDiagnostics(launch_argv) +
	                                  ", nativeSessionId=" + session.session_id;
	AppendAcpDiagnostic(session, "process_launch", "starting", "", "", false, 0, "", launch_detail);
	if (!defer_cold_launch && !session.managed_agent_run_id.empty()) session.managed_launch_attempted = true;
	std::filesystem::path process_working_directory = workspace_root;
	std::vector<std::string> process_argv = launch_argv;
	std::vector<std::pair<std::string, std::string>> process_environment = launch_environment;
	session.remote_output_delivery_token.clear();
	session.pending_remote_output_ack_line.clear();
	session.pending_remote_stdout_cursor = 0;
	session.pending_remote_stderr_cursor = 0;
	session.remote_consumed_stderr_cursor = 0;
	session.pending_remote_source_exit_ack_line.clear();
	session.remote_source_exit_reported = false;
	session.remote_source_exit_code = -1;
	if (remote)
	{
		const std::vector<std::string> proxy_provider_argv = migrate_copilot_wire ? std::vector<std::string>{"copilot", "--acp", "--stdio"} : launch_argv;
		const int runner_protocol_version = execution_host->runner_protocol_version == 0 &&
		                                    (session.recovering_remote_turn || session.recovering_remote_process)
		                                        ? 2
		                                        : execution_host->runner_protocol_version;
		if (runner_protocol_version <= 0)
		{
			startup_error =
			    "The selected remote runner has no negotiated protocol version.";
		}
		if (chat.remote_process_control_token.empty())
		{
			chat.remote_process_control_token =
			    PlatformServicesFactory::Instance().process_service.GenerateUuid();
			if (chat.remote_process_control_token.empty())
				chat.remote_process_control_token = uam::time::SteadyEpochNanosecondsTokenNow();
			if (!defer_cold_launch && !SaveChatQuietly(app, chat))
				startup_error = "Remote process authority could not be saved before launch.";
		}
		session.remote_output_delivery_token = chat.remote_process_control_token;
		session.remote_runner_protocol_version = runner_protocol_version;
		if (session.recovering_remote_turn || session.recovering_remote_process)
			session.remote_consumed_stderr_cursor = chat.remote_delivered_stderr_cursor;
		const std::filesystem::path runner = uam::remote::PackagedRunnerPath();
		std::error_code runner_error;
		if (!std::filesystem::is_regular_file(runner, runner_error) || runner_error)
		{
			startup_error = "The packaged UAM remote runner is missing.";
		}
		else
		{
			process_working_directory = runner.parent_path();
			process_argv = {uam::paths::Utf8PathString(runner), "proxy", "--alias", execution_host->ssh_alias,
			                "--platform", execution_host->platform,
			                "--version", execution_host->runner_version,
			                "--directory", uam::execution_hosts::RunnerDirectory(
			                    execution_host->platform, execution_host->runner_directory),
			                "--protocol", std::to_string(runner_protocol_version)};
			process_environment = {{uam::remote::kRemoteProcessSpecEnvironment, uam::remote::BuildProcessProxySpec("acp-" + chat.id, workspace_root, proxy_provider_argv, launch_environment, session.recovering_remote_turn || session.recovering_remote_process, session.remote_output_delivery_token, (session.recovering_remote_turn || session.recovering_remote_process) ? chat.remote_delivered_stdout_cursor : 0, (session.recovering_remote_turn || session.recovering_remote_process) ? chat.remote_delivered_stderr_cursor : 0, {}, {}, configuration_bundle.local_directory.empty() ? "" : provider.id, uam::paths::Utf8PathString(configuration_bundle.local_directory), launch_control_servers, session.model_discovery_only && session.ephemeral_model_discovery)}};
		}
	}
	if (remote && defer_cold_launch && startup_error.empty() &&
	    UamControlService::EnabledForChat(app.settings, chat) && chat.remote_uam_control_channel_id.empty())
	{
		chat.remote_uam_control_channel_id = PlatformServicesFactory::Instance().process_service.GenerateUuid();
		if (chat.remote_uam_control_channel_id.empty()) startup_error = "Could not create remote control authority.";
	}
	if (remote && startup_error.empty())
	{
		if (!session.recovering_remote_turn && !session.recovering_remote_process)
			chat.remote_prompt_image_supported = false;
		if (!session.recovering_remote_turn && !session.recovering_remote_process)
			chat.remote_structured_protocol = session.protocol_kind;
		chat.remote_process_exists = true;
		if (!defer_cold_launch && !SaveChatQuietly(app, chat))
			startup_error = "Remote process ownership could not be saved before launch.";
	}
	auto start_process = [remote, recovering_remote_turn, recovering_remote_process, defer_cold_launch,
	    migrate_copilot_wire, host = *execution_host, process_working_directory, process_argv, process_environment,
	    launch_detail, launch_model_id = launch_chat.model_id, start_control_relay, startup_error]
	    (AppState& app, AcpSessionState& session, ChatSession& chat, std::string* error_out) mutable -> bool
	{
		const ExecutionHost* execution_host = &host;
		if (startup_error.empty() && !session.managed_agent_run_id.empty()) session.managed_launch_attempted = true;
		if (startup_error.empty()) session.permission_review_incarnation = std::make_shared<const char>(0);
		if (!startup_error.empty() ||
		    !PlatformServicesFactory::Instance().process_service.StartStdioProcess(
		        session, process_working_directory, process_argv, &startup_error,
		        process_environment))
		{
			if (remote && !recovering_remote_turn && !recovering_remote_process)
			{
				chat.remote_process_exists = false;
				chat.remote_structured_protocol.clear();
				chat.remote_prompt_image_supported = false;
				chat.remote_opencode_native_port = 0;
				chat.remote_opencode_native_token.clear();
				if (defer_cold_launch) ScheduleChatSave(app, chat, 0.0);
				else (void)SaveChatQuietly(app, chat);
			}
			session.lifecycle_state = kAcpLifecycleError;
			session.last_error = startup_error.empty() ? ("Failed to start " + std::string(RuntimeDisplayName(session)) + " process.") : startup_error;
			AppendAcpDiagnostic(session, "process_launch", "start_failed", "", "", false, 0, session.last_error, launch_detail);
			if (error_out != nullptr)
			{
				*error_out = session.last_error;
			}
			return false;
		}

		if (start_control_relay) start_control_relay(session);
		session.running = true;
		if (!session.recovering_remote_turn && !session.recovering_remote_process)
			session.launch_model_options_id = launch_model_id;
		session.reconnect_pending = false;
		session.reconnect_not_before_time_s = 0.0;
		session.last_process_id = AcpProcessHandleLabel(session);
		AgentRunScheduler::InitialProcessStarted(app, session, chat);
		// A successfully captured replacement transport retires the old volatile failure latch.
		session.transport_ownership_error.clear();
		session.last_runtime_activity_time_s = GetAppTimeSeconds();
		AppendAcpDiagnostic(session, "process_launch", "started", "", "", false, 0, "", launch_detail);
		if (session.recovering_remote_turn || session.recovering_remote_process)
		{
			if (migrate_copilot_wire)
			{
				if (!ValidateAcpRestartOwnership(app, session, chat, &session.last_error))
				{
					session.transport_ownership_error = session.last_error;
					session.lifecycle_state = kAcpLifecycleError;
					app.status_line = session.last_error;
					if (error_out)
						*error_out = session.last_error;
					return false;
				}
				session.protocol_kind = "copilot-acp";
				session.initialized = false;
				session.session_ready = false;
				session.copilot_native_state["legacyWireMigration"] = true;
				// Confirmed-stop cleanup preserves queued follow-ups and the native
				// UUID, while interrupting uncertain already-dispatched old-wire input.
				session.queue_held = true;
				chat.acp_queue_held = true;
				(void)StopAcpProcessForRestart(app, session, chat);
				app.status_line = "Stopping the previous Copilot connection before switching to the SDK server; queued follow-ups are retained.";
				if (error_out)
					*error_out = app.status_line;
				return false;
			}
			session.prompt_image_supported = chat.remote_process_exists && !chat.remote_process_control_token.empty() && session.process_execution_host_id == chat.execution_host_id && session.provider_id == chat.provider_id && chat.remote_prompt_image_supported;
			// The remote provider never stopped. Resume its existing byte stream without
			// sending a second initialize or replaying the in-flight prompt. The local
			// UAM-control capability did stop with the previous GUI, so rebuild only that
			// side of the already configured remote channel.
			const std::string_view setup_method = session.session_id.empty()
			                                          ? uam::acp_methods::kSessionNew
			                                          : uam::acp_methods::kSessionResume;
			if (!ConfigureRemoteUamControlRelay(
			        app, session, chat, *execution_host, setup_method, nullptr))
			{
				if (error_out != nullptr) *error_out = session.last_error;
				RecoverDisconnectedRemoteAcpTransport(
				    app, session, chat,
				    uam::strings::NonEmptyOrFallback(
				        session.last_error, "Remote UAM control relay disconnected."));
				return false;
			}
			RestoreRemoteAcpRequests(session, chat);
			session.initialized = true;
			session.session_ready = true;
			session.lifecycle_state = session.processing ? kAcpLifecycleProcessing : kAcpLifecycleReady;
			if (session.remote_runner_protocol_version >= 3 &&
			    !chat.remote_prompt_delivery_id.empty() &&
			    !chat.remote_prompt_delivery_payload.empty())
			{
				if (!WriteCommittedRemotePrompt(app, session, chat))
				{
					session.last_error = "Remote prompt replay could not be queued for delivery.";
					if (error_out != nullptr) *error_out = session.last_error;
					RecoverDisconnectedRemoteAcpTransport(app, session, chat, session.last_error);
					return false;
				}
			}
			for (const AcpRemotePendingRequestState& request : chat.remote_pending_requests)
			{
				if (request.payload.empty()) continue;
				if (!WriteRemoteAcpRequest(session, request))
				{
					if (error_out != nullptr) *error_out = session.last_error;
					RecoverDisconnectedRemoteAcpTransport(app, session, chat, session.last_error);
					return false;
				}
			}
			return true;
		}
		if (!SendInitialize(session, error_out))
		{
			(void)StopAcpProcessForRestart(app, session, chat);
			return false;
		}

		return true;
	};
	if (defer_cold_launch && startup_error.empty())
	{
		const bool staged = StageAcpColdLaunch(app, session, chat,
		    [start_process](AppState& owner, AcpSessionState& native, ChatSession& stored) mutable
		    { return start_process(owner, native, stored, nullptr); });
		if (staged && prepared && prepared->control_capability_id == session.uam_control_capability_id &&
		    prepared->control_retention_deadline_s > 0.0)
			session.pending_local_launch->control_retention_deadline_s = std::min(
			    session.pending_local_launch->control_retention_deadline_s, prepared->control_retention_deadline_s);
		return staged;
	}
	return start_process(app, session, chat, error_out);
}

bool DecorateAcpSessionSetupRequest(AppState& app, AcpSessionState& session, std::string_view method,
    const std::string& cwd, nlohmann::json& request)
{
	if (session.provider_id == uam::provider_ids::kCodexCli)
		(void)uam::codex::ApplySetupInstructions(request, session.provider_protocol_instructions);
	return AddWorkspaceMcpServers(app, session, method, cwd, request);
}

bool SendSessionSetupIfReady(AppState& app, AcpSessionState& session, ChatSession& chat)
{
	if (!AgentRunScheduler::InitialSessionOwned(app, session, chat))
		return false;
	if (session.remote_source_exit_reported) return false;
	if (!session.running || !session.initialized || session.session_ready || session.session_setup_request_id != 0)
	{
		return false;
	}
	if (!AgentRunScheduler::PrepareInitialSetupRow(app, session, chat))
		return false;

	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(session.provider_id);
	if (session.model_discovery_only && (std::strcmp(runtime.AcpProtocolKind(), "codex-app-server") == 0 || std::strcmp(runtime.AcpProtocolKind(), "copilot-sdk") == 0))
	{
		return false;
	}
	const std::filesystem::path workspace_root = uam::paths::ResolveWorkspaceRootPath(app, chat);
	const std::string cwd = AcpWorkingDirectoryString(workspace_root);

	const std::string raw_resume_id = session.goal_internal_session ? std::string{} : uam::strings::Trim(chat.native_session_id);
	const std::string resume_id = session.goal_internal_session ? std::string{} : runtime.OnAcpValidateResumeId(chat);
	if (!AgentRunScheduler::ManagedResumeIdentityAllowed(app, session, chat, resume_id))
		return false;
	if (!raw_resume_id.empty() && resume_id.empty())
	{
		AppendInvalidResumeDiagnostic(session, raw_resume_id);
		if (!ClearSavedAcpResumeId(app, session, chat)) return false;
	}

	const std::string resolved_resume_id = session.goal_internal_session ? std::string{} : ResolvedAcpResumeIdForChat(app, chat);
	if (!AgentRunScheduler::ManagedResumeIdentityAllowed(app, session, chat, resolved_resume_id))
		return false;

	ChatSession resume_chat = chat;
	resume_chat.computer_use_globally_enabled = app.settings.computer_use_globally_enabled;
	resume_chat.native_session_id = resume_id.empty() ? resolved_resume_id : resume_id;
	if (session.provider_id == uam::provider_ids::kCodexCli)
	{
		session.codex_approvals_reviewer.clear();
		if (session.goal_review_turn || session.active_uam_agent_workspace_access == "read") resume_chat.approval_mode = uam::approval_modes::kPlanApprovalMode;
	}
	if (!session.goal_turn_model_id.empty()) resume_chat.model_id = session.goal_turn_model_id;

	std::string setup_error;
	const auto setup_preflight = runtime.OnAcpPrepareSetup(app, session, chat, cwd, setup_error);
	if (setup_preflight == AcpRequestPreflight::Pending)
		return false;
	if (setup_preflight == AcpRequestPreflight::Rejected)
	{
		session.last_error = uam::strings::NonEmptyOrFallback(
		    setup_error, "The provider could not verify session setup. No task input was sent.");
		return FailAcpSessionSetupWrite(app, session, chat, session.last_error);
	}

	const int id = session.next_request_id++;
	std::string method;
	nlohmann::json msg;
	if (session.resume_session_supported && !uam::strings::IsBlank(resume_chat.native_session_id))
	{
		method = uam::acp_methods::kSessionResume;
		msg = BuildResumeSessionRequest(id, resume_chat.native_session_id, cwd, &resume_chat);
	}
	else
	{
		msg = runtime.OnAcpBuildSessionSetupRequest(session, id, resume_chat, cwd, session.load_session_supported, method);
	}
	if ((msg.is_null() || msg.empty()) && !method.empty())
		return FailAcpSessionSetupWrite(app, session, chat, "The provider could not build the verified session setup. No task input was sent.");
	if (!DecorateAcpSessionSetupRequest(app, session, method, cwd, msg))
	{
		return FailAcpSessionSetupWrite(app, session, chat, "Failed to configure MCP servers.");
	}

	if (msg.is_null() || msg.empty())
	{
		session.session_ready = true;
		session.session_id = resolved_resume_id;
		session.current_mode_id = uam::approval_modes::EffectiveProviderMode(chat.approval_mode, chat.command_safety_tier);
		session.lifecycle_state = session.processing ? kAcpLifecycleProcessing : kAcpLifecycleReady;
		(void)ResumeQueuedUserPromptsAfterSessionSetup(app, session, chat);
		return true;
	}

	session.pending_request_methods[id] = method;
	session.session_setup_request_id = id;
	session.ignore_session_updates_until_ready = method == uam::acp_methods::kSessionLoad;
	if (session.ignore_session_updates_until_ready)
	{
		const int history_end = uam::AcpSessionHasActiveTurn(session) && session.turn_first_user_message_index >= 0
		    ? session.turn_first_user_message_index : static_cast<int>(chat.messages.size());
		RememberLoadHistoryReplayUpdates(session, chat, history_end);
	}
	session.lifecycle_state = kAcpLifecycleStarting;
	session.session_id = resume_chat.native_session_id;
	session.codex_thread_id = resume_chat.native_session_id;

	const bool written = WriteAcpMessage(session, msg);
	if (!written)
	{
		UamControlService::RevokeForSession(app, session);
		session.session_setup_request_id = 0;
		RecoverDisconnectedRemoteAcpTransport(
		    app, session, chat,
		    uam::strings::NonEmptyOrFallback(
		        session.last_error,
		        "Failed to " + std::string(method.find("load") != std::string::npos ||
		                                      method.find("resume") != std::string::npos
		                                  ? "load" : "create") +
		            " session."));
		return false;
	}
	return written;
}

bool RetrySessionNewAfterInvalidLoad(AppState& app, AcpSessionState& session, ChatSession& chat, const AcpResponseFailureDetails& details)
{
	if ((details.failure.method != uam::acp_methods::kSessionLoad &&
	     details.failure.method != uam::acp_methods::kSessionResume) ||
	    session.acp_resume_fallback_attempted)
	{
		return false;
	}
	const std::string failure_text = details.failure.message + "\n" + details.error_data;
	if (!TextContainsAnyCaseInsensitive(failure_text,
	        {"invalid session", "session not found", "session was not found", "unknown session", "session does not exist"}))
	{
		return false;
	}

	// A managed follow-up consumes the refusal without creating a fresh native conversation.
	if (!AgentRunScheduler::ManagedResumeIdentityAllowed(app, session, chat, {}))
		return true;
	if (!ClearSavedAcpResumeId(app, session, chat)) return true;
	session.acp_resume_fallback_attempted = true;
	session.session_setup_request_id = 0;
	session.session_id.clear();
	SyncResolvedNativeSessionIdForChat(app, chat, {});
	const AcpFailureDetails& failure = details.failure;
	const std::string retry_message = std::string(RuntimeDisplayName(session)) + " rejected the stored session id. Starting a new session instead.";
	AppendAcpDiagnostic(session, "response", "invalid_resume_id_retry_new", failure.method, failure.request_id, failure.has_code, failure.code, retry_message, details.detail_text);

	const std::filesystem::path workspace_root = uam::paths::ResolveWorkspaceRootPath(app, chat);
	const std::string cwd = AcpWorkingDirectoryString(workspace_root);
	const int retry_id = NextAcpRequestId(session, uam::acp_methods::kSessionNew);
	session.session_setup_request_id = retry_id;
	session.ignore_session_updates_until_ready = false;
	session.lifecycle_state = kAcpLifecycleStarting;

	ChatSession retry_chat = chat;
	retry_chat.computer_use_globally_enabled = app.settings.computer_use_globally_enabled;
	nlohmann::json retry_request = BuildNewSessionRequest(retry_id, cwd, &retry_chat);
	if (!AddWorkspaceMcpServers(app, session, uam::acp_methods::kSessionNew, cwd,
	                            retry_request))
	{
		session.pending_request_methods.erase(retry_id);
		session.session_setup_request_id = 0;
		FailAcpTurnOrSession(session, &chat, session.last_error);
		SaveChatQuietly(app, chat);
		MarkAcpChatUnseenIfBackground(app, chat);
		return true;
	}
	if (!WriteAcpMessage(session, retry_request))
	{
		session.pending_request_methods.erase(retry_id);
		session.session_setup_request_id = 0;
		RecoverDisconnectedRemoteAcpTransport(
		    app, session, chat,
		    uam::strings::NonEmptyOrFallback(session.last_error,
		                                     details.formatted_error));
	}
	return true;
}

bool SendStartupModeIfNeeded(AcpSessionState& session, const ChatSession& chat)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(session.provider_id);
	if (!session.running || !session.session_ready || session.mode_change_request_id != 0 || session.session_id.empty() || runtime.AcpModeChangeAction(session) != ProviderAcpSettingChangeAction::SendRequest)
	{
		return false;
	}

	const bool requires_plan = session.goal_review_turn || session.active_uam_agent_workspace_access == "read";
	const std::string desired_mode = requires_plan
	                                   ? std::string(uam::approval_modes::kPlanApprovalMode)
	                                   : uam::approval_modes::EffectiveProviderMode(chat.approval_mode, chat.command_safety_tier);
	const bool must_leave_hidden_autopilot = session.current_mode_id == uam::approval_modes::kAcpAutopilotMode;
	if ((!requires_plan && uam::strings::IsBlank(chat.approval_mode) && !must_leave_hidden_autopilot) || session.current_mode_id == desired_mode)
	{
		return false;
	}

	const int id = NextAcpRequestId(session, uam::acp_methods::kSessionSetMode);
	session.mode_change_request_id = id;
	session.mode_change_previous_id = session.current_mode_id;
	session.mode_change_requested_id = desired_mode;
	if (!WriteAcpMessage(session, BuildSetModeRequest(id, session.session_id, ProviderApprovalModeId(session, desired_mode))))
	{
		session.pending_request_methods.erase(id);
		session.current_mode_id = session.mode_change_previous_id;
		ClearAcpModeChangeRequest(session);
		return true;
	}

	session.current_mode_id = desired_mode;
	return true;
}

bool SendStartupModelIfNeeded(AcpSessionState& session, ChatSession& chat)
{
	if (!ProviderRuntimeRegistry::ResolveById(session.provider_id).IsGenericAcpSession() || !session.running || !session.session_ready || session.startup_model_request_id != 0 || session.reasoning_change_request_id != 0 || session.config_option_change_request_id != 0 || session.model_change_request_id != 0 || session.awaiting_model_config_options || session.session_id.empty())
	{
		return false;
	}

	const std::string requested_model = uam::strings::Trim(
	    uam::strings::NonEmptyOrFallback(session.goal_turn_model_id, chat.model_id));
	std::string model_error;
	const std::optional<std::string> resolved_model = uam::acp_models::ResolveAdvertisedModelId(session, requested_model, model_error);
	if (!resolved_model)
	{
		FailAcpTurnOrSession(session, &chat, model_error);
		return true;
	}
	const std::string& model_id = *resolved_model;
	if (model_id != requested_model)
	{
		if (!session.goal_turn_model_id.empty()) session.goal_turn_model_id = model_id;
		if (session.goal_turn_kind.empty() && !session.goal_review_turn && uam::strings::Trim(chat.model_id) == requested_model) chat.model_id = model_id;
	}
	if (model_id.empty() || session.current_model_id == model_id)
	{
		ClearAcpStartupModelRequest(session);
		return false;
	}

	const int id = NextAcpRequestId(session, uam::acp_methods::kSessionSetModel);
	session.startup_model_request_id = id;
	session.pending_startup_model_id = model_id;
	session.model_change_request_id = id;
	session.model_change_previous_id = session.current_model_id;
	session.model_change_previous_chat_id = session.current_model_id;
	session.model_change_requested_id = model_id;
	session.awaiting_model_config_options =
	    ProviderRuntimeRegistry::ResolveById(session.provider_id).AcpModelChangeAction() == ProviderAcpSettingChangeAction::SendRequestAndAwaitConfigOptions;
	if (!WriteAcpMessage(session, BuildSetModelRequest(id, session.session_id, model_id)))
	{
		session.pending_request_methods.erase(id);
		ClearAcpStartupModelRequest(session);
		ClearAcpModelChangeRequest(session);
		session.awaiting_model_config_options = false;
		return true;
	}

	session.current_model_id = model_id;
	return true;
}

static MemoryRecallIdentity MemoryRecallSnapshot(const AppState& app, const AcpSessionState& session, const ChatSession& chat)
{
	return {app.data_root, paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, chat)),
	    chat.id, session.provider_id, session.session_id, session.queued_prompt, session.memory_recall_query,
	    session.memory_recall_position.value_or(0), session.turn_serial, app.settings.memory_recall_budget_bytes,
	    memory_policy::Resolve(app.settings, chat), chat.small_model_mode};
}

static std::string PreparedPromptText(const AcpSessionState& session)
{
	std::string prompt = session.queued_prompt;
	if (session.memory_recall_preparation != nullptr && session.memory_recall_position.has_value())
	{
		const std::shared_ptr<MemoryRecallPreparation>& state = session.memory_recall_preparation->state;
		if (state->finished.load(std::memory_order_acquire) && state->error.empty() && *session.memory_recall_position <= prompt.size())
			prompt.insert(*session.memory_recall_position, state->context);
	}
	return prompt;
}

static std::vector<attached_skills::Span> PreparedSkillSpans(const AcpSessionState& session)
{
	std::vector<attached_skills::Span> spans = session.queued_skill_spans;
	if (session.memory_recall_preparation != nullptr && session.memory_recall_position.has_value())
	{
		const std::shared_ptr<MemoryRecallPreparation>& state = session.memory_recall_preparation->state;
		if (state->finished.load(std::memory_order_acquire) && state->error.empty())
			for (attached_skills::Span& span : spans)
				if (span.offset >= *session.memory_recall_position) span.offset += state->context.size();
	}
	return spans;
}

static std::string AttachedSkillOwnership(const AppState& app, const AcpSessionState& session, const ChatSession& chat)
{
	return nlohmann::json::array({session.saved_image_retry && session.saved_image_retry->remote_scope
	        ? SavedImageRetryExecutionOwnership(app, session, chat, true) : LocalLaunchOwnership(app, session, chat), session.turn_serial,
	    session.turn_first_user_message_index, session.turn_user_message_index, session.goal_command_revision,
	    session.protocol_kind, session.process_execution_host_id, session.remote_output_delivery_token,
	    session.queued_prompt}).dump();
}

static bool OwnsAttachedSkillPreparation(const AppState& app, const AcpSessionState& session, const ChatSession& chat)
{
	const RemoteContextPreparation& state = *session.attached_skill_preparation;
	const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
	return state.data_root == app.data_root && state.incarnation == chat.metadata_incarnation &&
	    host != nullptr && execution_hosts::SameConnection(state.host, *host) &&
	    state.workspace == paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, chat)) &&
	    state.ownership == AttachedSkillOwnership(app, session, chat) &&
	    state.prompt == PreparedPromptText(session) && state.skill_spans == PreparedSkillSpans(session) &&
	    !state.cancel.stop_requested() && !session.cancel_requested && session.processing && !session.remote_source_exit_reported;
}

static std::string WirePromptText(const AcpSessionState& session)
{
	const std::shared_ptr<RemoteContextPreparation>& state = session.attached_skill_preparation;
	return state != nullptr && state->finished.load(std::memory_order_acquire) && state->skills_prepared
	    ? state->wire_prompt : PreparedPromptText(session);
}

static bool UsesBoundedImagePreparation(const AcpSessionState& session)
{
	const bool acp = session.protocol_kind == provider_profile_constants::kProtocolGeminiAcp || session.protocol_kind == provider_profile_constants::kProtocolOpenCodeAcp || session.protocol_kind == provider_profile_constants::kProtocolCopilotAcp;
	return session.protocol_kind == provider_profile_constants::kProtocolClaudeCodeStreamJson || (acp && session.prompt_image_supported) || (session.saved_image_retry && ProviderRuntimeRegistry::ResolveById(session.provider_id).SupportsSavedImageRetry(session));
}

static ChatSession PromptSettings(const AppState& app, const AcpSessionState& session, const ChatSession& chat)
{
	// Prompt hooks receive the same effective settings for comparison and actual dispatch.
	ChatSession result;
	result.workspace_directory = paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, chat));
	result.execution_host_id = chat.execution_host_id;
	result.native_session_id = chat.native_session_id;
	result.model_id = session.goal_turn_model_id.empty() ? chat.model_id : session.goal_turn_model_id;
	result.reasoning_effort = chat.reasoning_effort;
	result.service_tier = chat.service_tier;
	result.service_tier_explicit = chat.service_tier_explicit;
	result.approval_mode = chat.approval_mode;
	result.command_safety_tier = chat.command_safety_tier;
	return result;
}

static ClaudeInputIdentity ClaudeInputSnapshot(const AppState& app, const AcpSessionState& session, const ChatSession& chat)
{
	const int first = session.turn_first_user_message_index >= 0 ? session.turn_first_user_message_index : session.turn_user_message_index;
	ClaudeInputIdentity identity{chat.id, session.provider_id, chat.provider_id, session.session_id, session.process_execution_host_id, chat.execution_host_id, paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, chat)), WirePromptText(session), chat.model_id, session.goal_turn_model_id, chat.approval_mode, chat.computer_use_backend, chat.computer_use_enabled, session.turn_serial, first, session.turn_user_message_index, uam::claude_input::TurnImages(chat, first, session.turn_user_message_index)};
	if (session.saved_image_retry)
	{
		identity.original_root = app.data_root;
		identity.original_incarnation = chat.metadata_incarnation;
		identity.saved_image_retry = session.saved_image_retry;
		identity.native_metadata = ProviderRuntimeRegistry::ResolveById(session.provider_id).SavedImageRetryMetadata(session, PromptSettings(app, session, chat));
		if (session.saved_image_retry->remote_scope)
			identity.remote_retry_ownership = SavedImageRetryExecutionOwnership(app, session, chat, true);
	}
	return identity;
}

static bool OwnsPreparedSavedImageRetry(const AppState& app, const AcpSessionState& session, const ChatSession& chat, const std::shared_ptr<ClaudeInputPreparation>& state)
{
	const auto& permission = session.saved_image_retry;
	const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
	if (!permission || !state || !session.claude_input_preparation || session.claude_input_preparation->state != state || state->identity.saved_image_retry != permission || !SavedImageRetrySourceMatches(app, session, chat, *permission) || session.saved_image_retry_turn_serial != session.turn_serial || session.turn_first_user_message_index != permission->row_index || session.turn_user_message_index != permission->row_index || !session.processing || session.cancel_requested || session.remote_source_exit_reported || !host || !execution_hosts::SameConnection(state->host, *host) || (!permission->remote_scope && state->host.id != execution_hosts::kLocalHostId) || !state->finished.load(std::memory_order_acquire) || state->cancel.stop_requested() || !state->error.empty() || state->identity != ClaudeInputSnapshot(app, session, chat))
		return false;
	if (!state->message.is_object() || !state->message.contains("message") || !state->message["message"].is_object() || !state->message["message"].contains("content") || !state->message["message"]["content"].is_array())
		return false;
	return std::ranges::count_if(state->message["message"]["content"], [](const nlohmann::json& part) { return part.is_object() && part.value("type", "") == "image"; }) == static_cast<std::ptrdiff_t>(state->identity.images.size());
}

static bool OwnsSavedImageRetryPacket(const AppState& app, const AcpSessionState& session, const ChatSession& chat, const std::shared_ptr<ClaudeInputPreparation>& state, const nlohmann::json& message, const std::string& method, int request_id)
{
	return OwnsPreparedSavedImageRetry(app, session, chat, state) && !state->sealed_message.is_null() && (!state->identity.native_metadata.is_null() || state->message == message) && state->sealed_message == message && state->sealed_method == method && state->sealed_request_id == request_id;
}

static bool AbortSavedImageRetry(AppState& app, AcpSessionState& session, ChatSession& chat, const std::string& error)
{
	const auto permission = session.saved_image_retry ? session.saved_image_retry : session.claude_input_preparation ? session.claude_input_preparation->state->identity.saved_image_retry : nullptr;
	const bool same_owner = permission && permission->root == app.data_root && permission->incarnation == chat.metadata_incarnation && permission->chat_id == chat.id && permission->chat_created_at == chat.created_at;
	if (same_owner)
		RestorePendingPromptDelivery(session, chat);
	else
		session.pending_prompt_delivery.reset();
	RevokeSavedImageRetry(session);
	FailAcpTurnOrSession(session, same_owner ? &chat : nullptr, error);
	if (same_owner)
		ScheduleChatSave(app, chat, 0.0);
	return true;
}

/// <summary>Restores only the staged user bodies still owned by this transcript.</summary>
void RestorePendingPromptDelivery(AcpSessionState& session, ChatSession& chat)
{
	const std::shared_ptr<PendingAcpPromptDelivery> packet = std::move(session.pending_prompt_delivery);
	if (!packet) return;
	if (packet->remote_host && packet->created_at == chat.created_at &&
	    packet->delivery_id == chat.remote_prompt_delivery_id && packet->delivery_payload == chat.remote_prompt_delivery_payload)
	{
		if (!packet->method.empty())
			std::erase_if(chat.remote_pending_requests, [&packet](const AcpRemotePendingRequestState& request)
			{ return request.request_id == packet->request_id && request.turn_serial == packet->turn_serial && request.method == packet->method; });
		chat.remote_turn_reconnect_pending = false;
		chat.remote_prompt_delivery_session_id.clear();
		chat.remote_prompt_delivery_id.clear();
		chat.remote_prompt_delivery_payload.clear();
		if (packet->remote_prepared_head && std::ranges::none_of(chat.acp_queued_prompts, [&packet](const AcpQueuedUserPromptState& prompt)
		    { return prompt.id == packet->remote_prepared_head->id && prompt.text == packet->remote_prepared_head->text; }))
		{
			chat.acp_queued_prompts.insert(chat.acp_queued_prompts.begin(), std::move(*packet->remote_prepared_head));
			chat.acp_dispatched_queued_prompt_count = 1;
		}
	}
	for (const std::pair<std::size_t, std::string>& user : packet->user_messages)
		if (user.first < chat.messages.size() && chat.messages[user.first].role == MessageRole::User &&
		    chat.messages[user.first].content == user.second)
			chat.messages[user.first].acp_prompt_not_sent = true;
}

/// <summary>Retains one immutable packet until its owned full-save receipt authorizes transport.</summary>
static std::shared_ptr<PendingAcpPromptDelivery> StagePromptAdmission(AppState& app, AcpSessionState& session,
    ChatSession& chat, const nlohmann::json& msg, const std::string& method, int id,
    const std::vector<std::size_t>& unsent_messages, std::optional<AcpQueuedUserPromptState>* remote_head = nullptr)
{
	std::shared_ptr<PendingAcpPromptDelivery> packet = std::make_shared<PendingAcpPromptDelivery>();
	packet->message = msg;
	packet->method = method;
	packet->request_id = id;
	packet->turn_serial = session.turn_serial;
	packet->data_root = app.data_root;
	packet->provider_id = session.provider_id;
	packet->session_id = session.session_id;
	packet->workspace = chat.workspace_directory;
	packet->prompt = session.queued_prompt;
	packet->configuration = PromptDeliveryConfiguration(app, session, chat);
	packet->unsent_messages = unsent_messages;
	if (session.saved_image_retry && session.claude_input_preparation)
		packet->saved_image_preparation = session.claude_input_preparation->state;
	for (const std::size_t index : unsent_messages)
		packet->user_messages.emplace_back(index, chat.messages[index].content);
	session.pending_prompt_delivery = packet;
	packet->created_at = chat.created_at;
	packet->process_id = AcpProcessHandleLabel(session);
	packet->active_goal_id = chat.active_goal_id;
	packet->goal_revision = chat.goal_command_revision;
	packet->small_model_mode = chat.small_model_mode;
	packet->quota_process_authority = chat.execution_host_id != execution_hosts::kLocalHostId && chat.provider_id == "claude-cli" &&
	    !chat.claude_quota_continuation.id.empty() && (chat.claude_quota_continuation.state == "cancelled" ||
	    session.claude_quota_delivery_id == chat.claude_quota_continuation.id);
	if (chat.execution_host_id != execution_hosts::kLocalHostId && (chat.small_model_mode || packet->quota_process_authority))
	{
		if (chat.small_model_mode) packet->goal_scope = CaptureGoalPromptReceiptScope(app, session, chat);
		packet->scoped_chat_incarnation = chat.metadata_incarnation;
		packet->scoped_writer = session.stdin_writer;
		for (const std::size_t index : unsent_messages) packet->scoped_user_messages.emplace_back(index, chat.messages[index]);
	}
	packet->native_session_id = chat.native_session_id;
	if (remote_head != nullptr)
	{
		const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
		if (host == nullptr) { packet->admission = DurableAdmissionState::Failed; return packet; }
		packet->remote_host = *host;
		packet->remote_prepared_head = std::move(*remote_head);
		packet->delivery_session_id = chat.remote_prompt_delivery_session_id;
		packet->delivery_id = chat.remote_prompt_delivery_id;
		packet->delivery_payload = chat.remote_prompt_delivery_payload;
		packet->proxy_token = session.remote_output_delivery_token;
		packet->proxy_protocol = session.remote_runner_protocol_version;
	}
	const std::weak_ptr<PendingAcpPromptDelivery> weak = packet;
	const std::string chat_id = chat.id;
	packet->admission = BeginDurableAdmission(app, chat, [&app, chat_id, weak]()
	{
		const std::shared_ptr<PendingAcpPromptDelivery> pending = weak.lock();
		const ChatSession* current = ChatDomainService().FindChatById(app, chat_id);
		const AcpSessionState* owner = FindAcpSessionForChat(app, chat_id);
		return pending && current != nullptr && owner != nullptr && owner->pending_prompt_delivery == pending &&
		    OwnsPromptAdmission(app, *owner, *current, *pending);
	}, [weak](DurableAdmissionState state)
	{
		if (const std::shared_ptr<PendingAcpPromptDelivery> pending = weak.lock()) pending->admission = state;
	});
	return packet;
}

/// <summary>Releases an already prepared packet after its durable barrier; never allocates another ID.</summary>
static bool DeliverPreparedPrompt(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& msg, const std::string& method, int id, const std::vector<std::size_t>& unsent_messages, bool marker_committed = false, AcpPromptFailurePurpose* failure_purpose = nullptr, std::shared_ptr<ClaudeInputPreparation> saved_image_preparation = {})
{
	if (!AgentRunScheduler::InitialInputReady(app, session, chat, true))
		return false;
	const std::string prompt = session.queued_prompt;
	const bool remote = chat.execution_host_id != uam::execution_hosts::kLocalHostId;
	if (remote && !marker_committed)
	{
		const bool staged_prompt = chat.acp_dispatched_queued_prompt_count == 1 &&
		    !chat.acp_queued_prompts.empty() &&
		    chat.acp_queued_prompts.front().prepared_for_delivery &&
		    chat.acp_queued_prompts.front().text == prompt;
		AcpQueuedUserPromptState staged_entry;
		if (staged_prompt)
		{
			staged_entry = std::move(chat.acp_queued_prompts.front());
			chat.acp_queued_prompts.erase(chat.acp_queued_prompts.begin());
			chat.acp_dispatched_queued_prompt_count = 0;
		}
		// Native input without an RPC method still owns a durable delivery marker,
		// but has no outgoing RPC response to correlate in the request journal.
		const std::size_t pending_request_index = chat.remote_pending_requests.size();
		std::optional<AcpRemotePendingRequestState> pending_request;
		if (!method.empty())
		{
			pending_request.emplace();
			pending_request->request_id = id;
			pending_request->method = method;
			pending_request->turn_serial = session.turn_serial;
			chat.remote_pending_requests.push_back(*pending_request);
		}
		// Persist intent before the transport write. A process write can succeed even
		// when the manager dies before observing its result; the durable marker makes
		// restart choose reattach rather than issuing the prompt a second time.
		chat.remote_next_request_id = std::max(chat.remote_next_request_id, session.next_request_id);
		chat.remote_turn_serial = std::max(chat.remote_turn_serial, session.turn_serial);
		chat.remote_turn_user_message_index = session.turn_user_message_index;
		chat.remote_active_turn_id = ProviderRuntimeRegistry::ResolveById(session.provider_id).AcpTurnIdentity(session);
		chat.remote_turn_reconnect_pending = true;
		chat.remote_restart_pending = false;
		session.restart_after_remote_stop_cleanup = false;
		if (session.remote_runner_protocol_version >= 3)
		{
			chat.remote_prompt_delivery_session_id = "acp-" + chat.id;
			chat.remote_prompt_delivery_id =
			    PlatformServicesFactory::Instance().process_service.GenerateUuid();
			if (chat.remote_prompt_delivery_id.empty())
				chat.remote_prompt_delivery_id =
				    uam::time::SteadyEpochNanosecondsTokenNow();
			chat.remote_prompt_delivery_payload = SerializeStructuredMessage(session, msg);
		}
		if (CanDeferRemotePromptAdmission(app, session, chat) || CanStageRemoteGoalPromptAdmission(app, session, chat))
		{
			std::optional<AcpQueuedUserPromptState> head;
			if (staged_prompt) head = std::move(staged_entry);
			(void)StagePromptAdmission(app, session, chat, msg, method, id, unsent_messages, &head);
			return true;
		}
		if (!SaveChatQuietly(app, chat))
		{
			if (staged_prompt)
			{
				chat.acp_queued_prompts.insert(chat.acp_queued_prompts.begin(),
				                               std::move(staged_entry));
				chat.acp_dispatched_queued_prompt_count = 1;
			}
			if (pending_request && pending_request_index < chat.remote_pending_requests.size() &&
			    chat.remote_pending_requests[pending_request_index] == *pending_request)
				chat.remote_pending_requests.erase(chat.remote_pending_requests.begin() + static_cast<std::ptrdiff_t>(pending_request_index));
			chat.remote_turn_reconnect_pending = false;
			chat.remote_prompt_delivery_session_id.clear();
			chat.remote_prompt_delivery_id.clear();
			chat.remote_prompt_delivery_payload.clear();
			session.pending_request_methods.erase(id);
			session.prompt_request_id = 0;
			session.last_error =
			    "Prompt is waiting for its remote recovery marker to be saved.";
			app.status_line = session.last_error;
			AppendAcpDiagnostic(session, "persistence", "remote_turn_marker_save_failed",
			                    "", "", false, 0, session.last_error);
			ScheduleChatSave(app, chat, 0.0);
			return true;
		}
	}

	if (!method.empty())
	{
		session.pending_request_methods[id] = method;
		session.prompt_request_id = id;
	}

	bool prompt_written = false;
	if (saved_image_preparation && !OwnsSavedImageRetryPacket(app, session, chat, saved_image_preparation, msg, method, id))
	{
		return AbortSavedImageRetry(app, session, chat, "Saved image Retry ownership changed before delivery.");
	}
	AgentRunScheduler::InitialInputAttempted(session);
	if (remote && session.remote_runner_protocol_version >= 3)
	{
		prompt_written = WriteCommittedRemotePrompt(app, session, chat);
	}
	else
	{
		prompt_written = WriteLocalAcpPromptWithObservation(app, session, chat, msg, method, id, unsent_messages);
	}
	if (!prompt_written)
	{
		if (remote && session.remote_runner_protocol_version >= 3)
		{
			RecoverDisconnectedRemoteAcpTransport(
			    app, session, chat,
			    uam::strings::NonEmptyOrFallback(
			        session.last_error, "Remote prompt transport disconnected."));
			return true;
		}
		for (const std::size_t index : unsent_messages)
		{
			chat.messages[index].acp_prompt_not_sent = true;
			chat.messages[index].interrupted = true;
		}
		session.pending_request_methods.erase(id);
		session.prompt_request_id = 0;
		if (failure_purpose)
			*failure_purpose = AcpPromptFailurePurpose::Transport;
		CompletePromptTurn(session, kAcpLifecycleError);
		InvalidateAcpTransport(app, session, chat, session.last_error);
		return true;
	}

	AgentRunScheduler::InitialInputWritten(session);
	// The delivery-intent save already persists local state unless delivery changes metadata.
	bool local_delivery_save_needed = unsent_messages.empty() || HasCompletedAcpPromptRecoveries(app, chat);
	if (remote && HasCompletedAcpPromptRecoveries(app, chat))
		ScheduleChatSave(app, chat, 0.0);
	for (const std::size_t index : unsent_messages)
	{
		if (index >= chat.messages.size() || !chat.messages[index].interrupted) continue;
		chat.messages[index].interrupted = false;
		local_delivery_save_needed = true;
		if (remote) ScheduleChatSave(app, chat, 0.0);
	}
	session.delivered_prompt_recoveries.clear();
	if (prompt_recovery::BindingsMatch(chat))
	{
		for (const auto& record : chat.acp_unsent_prompt_recoveries)
		{
			if (record.user_message_index < session.turn_first_user_message_index || record.user_message_index > session.turn_user_message_index)
				continue;
			const Message& row = chat.messages[static_cast<std::size_t>(record.user_message_index)];
			if (!row.acp_prompt_not_sent && !row.interrupted && !row.acp_prompt_discarded)
				session.delivered_prompt_recoveries.push_back(record);
		}
	}
	if (session.active_uam_agent_execution_capability == "uam-prompt-injected" &&
	    !session.active_uam_agent_instructions.empty())
	{
		const std::string definition_hash = chat.provider_id + ":" + session.active_uam_agent_definition_hash;
		local_delivery_save_needed = local_delivery_save_needed || chat.last_prompt_agent_definition_hash != definition_hash;
		chat.last_prompt_agent_definition_hash = definition_hash;
		if (remote) ScheduleChatSave(app, chat, 0.0);
	}
	if (!chat.provider_handoff_context.empty())
	{
		local_delivery_save_needed = local_delivery_save_needed || chat.provider_handoff_session_id != session.session_id;
		chat.provider_handoff_session_id = session.session_id;
		if (remote) ScheduleChatSave(app, chat, 0.0);
	}
	session.queued_prompt.clear();
	session.queued_skill_spans.clear();
	RevokeSavedImageRetry(session);
	if (session.attached_skill_preparation != nullptr) session.attached_skill_preparation->cancel.request_stop();
	session.attached_skill_preparation.reset();
	session.turn_computer_use_mode = false;
	session.memory_recall_position.reset();
	session.memory_recall_query.clear();
	session.memory_recall_preparation.reset();
	if (session.remote_context_preparation != nullptr) session.remote_context_preparation->cancel.request_stop();
	session.remote_context_preparation.reset();
	session.claude_input_preparation.reset();
	if (!remote && local_delivery_save_needed && session.defer_prompt_save)
		ScheduleChatSave(app, chat, 0.0);
	else if (!remote && local_delivery_save_needed && !SaveChatQuietly(app, chat))
	{
		session.last_error = "Prompt was delivered, but chat state could not be saved.";
		app.status_line = session.last_error;
		AppendAcpDiagnostic(session, "persistence", "remote_turn_marker_save_failed", "", "",
		                    false, 0, session.last_error);
		ScheduleChatSave(app, chat, 0.0);
	}
	return true;
}

bool BeginManagedResultPrompt(AppState& app, AcpSessionState& session, ChatSession& chat,
    std::shared_ptr<const ManagedResultPromptInput> input, const SavedChatPrimaryWitness& predecessor)
{
	if (!input || !input->owned || !input->prepared || !input->written || !input->failed || !input->owned() ||
	    session.pending_prompt_delivery || !chat.managed_parent_result || chat.managed_parent_result->stage != "admitted") return false;
	auto packet = std::make_shared<PendingAcpPromptDelivery>();
	packet->managed_result_input = input;
	packet->request_id = session.next_request_id++;
	packet->message = ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpBuildPrompt(session,
	    packet->request_id, input->text, chat, packet->method);
	if (packet->message.empty() || packet->method.empty()) return false;
	chat.managed_parent_result->stage = "write-unconfirmed";
	chat.managed_parent_result->native_request_id = std::to_string(packet->request_id);
	if (!input->prepared(packet->request_id)) return false;
	session.pending_prompt_delivery = packet;
	const std::string id = chat.id;
	const std::weak_ptr<PendingAcpPromptDelivery> weak = packet;
	auto owned = [&app, id, weak]()
	{
		const auto current = weak.lock();
		const auto* runtime = FindAcpSessionForChat(app, id);
		return current && runtime && runtime->pending_prompt_delivery == current &&
		    current->managed_result_input->owned();
	};
	const DurableAdmissionState state = BeginDurableBindingAdmission(app, chat, predecessor, owned,
	    [&app, id, weak, owned](DurableBindingAdmissionReceipt receipt)
	    {
		const auto current = weak.lock();
		auto* runtime = FindAcpSessionForChat(app, id);
		auto* owner = ChatDomainService().FindChatById(app, id);
		if (!current || !runtime || !owner) return;
		const auto source = current->managed_result_input;
		if (receipt.state != DurableAdmissionState::Committed || !owned())
		{
			if (runtime->pending_prompt_delivery == current) runtime->pending_prompt_delivery.reset();
			source->failed(); return;
		}
		runtime->pending_prompt_delivery.reset();
		++runtime->turn_serial;
		runtime->processing = true;
		runtime->lifecycle_state = kAcpLifecycleProcessing;
		runtime->turn_started_time_s = GetAppTimeSeconds();
		runtime->current_assistant_message_index = runtime->turn_assistant_message_index = -1;
		runtime->turn_first_user_message_index = runtime->turn_user_message_index = -1;
		runtime->last_error.clear();
		runtime->cancel_requested = false;
		runtime->last_runtime_activity_time_s = runtime->turn_started_time_s;
		ResetAcpTurnStreamState(*runtime);
		runtime->prompt_request_id = current->request_id;
		runtime->pending_request_methods[current->request_id] = current->method;
		source->written(current->request_id); // Captures ownership before any native response can be observed.
		if (!WriteAcpMessage(*runtime, current->message))
		{
			source->failed(); // Partial write is UNKNOWN. No queue row, transport restart or replay.
			runtime->pending_request_methods.erase(current->request_id);
			CompletePromptTurn(*runtime, kAcpLifecycleError);
		}
	    });
	if (state != DurableAdmissionState::Pending)
	{
		if (session.pending_prompt_delivery == packet) session.pending_prompt_delivery.reset();
		input->failed(); return false;
	}
	return true;
}

bool CanDeferRemotePromptAdmission(const AppState& app, const AcpSessionState& session, const ChatSession& chat)
{
	const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
	return chat.execution_host_id != execution_hosts::kLocalHostId && !chat.small_model_mode &&
	    app.claude_quota_dispatch_id.empty() && session.remote_runner_protocol_version >= 3 &&
	    session.running && session.session_ready && host != nullptr && host->runner_protocol_version == session.remote_runner_protocol_version &&
	    session.process_execution_host_id == host->id && !session.remote_output_delivery_token.empty();
}

std::optional<GoalPromptReceiptScope> CaptureGoalPromptReceiptScope(const AppState& app, const AcpSessionState& session, const ChatSession& chat)
{
	const std::string owner_id = strings::NonEmptyOrFallback(chat.goal_owner_chat_id, chat.id);
	const ChatSession* owner = ChatDomainService().FindChatById(app, owner_id);
	if (!owner || owner->imported_read_only || owner->side_cleanup_requested) return std::nullopt;
	const std::string goal_id = strings::NonEmptyOrFallback(chat.goal_iteration_goal_id, owner->active_goal_id);
	const auto goal = std::ranges::find(owner->goals, goal_id, &Goal::id);
	if (goal_id.empty() || goal == owner->goals.end() || goal->status != GoalStatus::Active ||
	    (!session.goal_turn_kind.empty() && session.goal_command_revision != owner->goal_command_revision)) return std::nullopt;
	return GoalPromptReceiptScope{owner_id, owner->metadata_incarnation, nlohmann::json::array({
	    owner->active_goal_id, owner->goal_command_revision, owner->native_session_id, owner->provider_id,
	    owner->execution_host_id, owner->workspace_directory, owner->model_id, owner->small_model_mode,
	    owner->uam_agent_id, owner->approval_mode, owner->command_safety_tier, owner->reasoning_effort, owner->service_tier,
	    chat.goal_owner_chat_id,
	    chat.goal_iteration_goal_id, chat.goal_iteration_turn_kind, chat.goal_iteration_repair_attempts,
	    session.goal_turn_kind, session.goal_command_revision, session.goal_review_turn, session.goal_review_goal_id,
	    session.goal_internal_session, session.goal_turn_model_id,
	    goal->id, goal->created_at, goal->objective, static_cast<int>(goal->status), goal->execution_owner,
	    goal->token_budget, goal->tokens_used, goal->loop_count, goal->updated_at,
	    goal->worker_model_id, goal->reviewer_model_id}).dump(), goal->native_binding};
}

bool CanStageRemoteGoalPromptAdmission(const AppState& app, const AcpSessionState& session, const ChatSession& chat)
{
	const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
	// This distinct route adds goal ownership; the ordinary v3 admission predicate remains unchanged.
	return !app.managed_initial_shutdown && chat.small_model_mode && chat.execution_host_id != execution_hosts::kLocalHostId &&
	    session.transport_ownership_error.empty() &&
	    app.claude_quota_dispatch_id.empty() && session.remote_runner_protocol_version >= 3 &&
	    session.running && session.session_ready && host && host->runner_protocol_version == session.remote_runner_protocol_version &&
	    session.process_execution_host_id == host->id && !session.remote_output_delivery_token.empty() &&
	    !AcpStopInProgress(app, chat.id) && !AcpSessionHasPendingCancel(session) && !AcpSessionIsWaitingForInput(session) &&
	    !session.remote_source_exit_reported && !session.recovering_remote_turn && !session.recovering_remote_process &&
	    !session.local_stop_pending && !session.remote_stop_pending && !session.remote_stop_unconfirmed &&
	    !session.manual_compaction_pending && !session.context_compaction_in_progress && !session.queue_held &&
	    !chat.acp_queue_held && !chat.acp_queue_storage_error && !chat.side_cleanup_requested &&
	    CaptureGoalPromptReceiptScope(app, session, chat).has_value();
}

bool SendQueuedPromptIfReady(AppState& app, AcpSessionState& session, ChatSession& chat, AcpPromptFailurePurpose* failure_purpose)
{
	if (failure_purpose)
		*failure_purpose = AcpPromptFailurePurpose::Admission;
	if (session.pending_prompt_delivery && session.pending_prompt_delivery->managed_result_input) return false;
	if (session.saved_image_retry && session.saved_image_retry_turn_serial >= 0 && (!SavedImageRetrySourceMatches(app, session, chat, *session.saved_image_retry) || session.saved_image_retry_turn_serial != session.turn_serial || session.turn_first_user_message_index != session.saved_image_retry->row_index || session.turn_user_message_index != session.saved_image_retry->row_index))
		return AbortSavedImageRetry(app, session, chat, "Saved image Retry ownership changed. Retry the saved message again.");
	if (session.turn_computer_use_mode && (!app.settings.computer_use_globally_enabled || !chat.computer_use_enabled || !uam::computer_use::AvailableForChat(chat)))
	{
		session.last_error = "Computer Use was disabled before prompt delivery.";
		session.queue_held = chat.acp_queue_held = true;
		return false;
	}
	if ((session.goal_queue_admission == AcpSessionState::GoalQueueAdmission::Pending ||
	     session.goal_queue_admission == AcpSessionState::GoalQueueAdmission::Held) &&
	    session.goal_queue_admission_turn_serial == session.turn_serial) return false;
	if (session.pending_local_session_receipt) return false;
	// Reject stale intent even while a revoked worker save is still pending.
	const bool pending_owned = !session.pending_prompt_delivery || OwnsPromptAdmission(app, session, chat, *session.pending_prompt_delivery);
	if (session.pending_prompt_delivery && (!pending_owned ||
	    session.pending_prompt_delivery->admission == DurableAdmissionState::Failed))
	{
		const bool remote = session.pending_prompt_delivery->remote_host.has_value();
		const bool same_turn = session.pending_prompt_delivery->turn_serial == session.turn_serial &&
		    session.pending_prompt_delivery->prompt == session.queued_prompt;
		AppendAcpDiagnostic(session, "persistence", "prompt_admission_rejected", "", "", false, 0, "",
		    "ownership=" + uam::strings::NonEmptyOrFallback(std::string(PromptAdmissionOwnershipMismatch(app, session, chat, *session.pending_prompt_delivery)), "unchanged") +
		    "; receipt=" + (session.pending_prompt_delivery->admission == DurableAdmissionState::Failed ? "failed" :
		        session.pending_prompt_delivery->admission == DurableAdmissionState::Committed ? "committed" : "pending"));
		RestorePendingPromptDelivery(session, chat);
		if (same_turn && remote)
		{
			session.last_error = "Remote prompt was not sent because its owned durable receipt was not confirmed.";
			session.queue_held = chat.acp_queue_held = true;
			session.goal_resume_suppressed = true;
			CompletePromptTurn(session, kAcpLifecycleError);
		}
		else if (same_turn) FailAcpTurnOrSession(session, &chat, pending_owned
		    ? "Prompt is waiting for its delivery intent to be saved."
		    : "Prompt ownership changed before delivery.");
		ScheduleChatSave(app, chat, 0.0);
		return true;
	}

	if (session.attached_skill_preparation != nullptr && !OwnsAttachedSkillPreparation(app, session, chat))
	{
		session.attached_skill_preparation->cancel.request_stop();
		session.attached_skill_preparation.reset();
		if (!session.queued_prompt.empty())
		{
			FailAcpTurnOrSession(session, &chat, "Attached skill preparation ownership changed. Retry the prompt.");
			ScheduleChatSave(app, chat, 0.0);
		}
		return true;
	}

	if (!provider_availability::IsEnabled(app.settings, chat.provider_id, chat.execution_host_id)) return false;
	if (app.pending_chat_save_at_by_chat_id.contains(chat.id)) return false;
	if (session.memory_recall_preparation != nullptr &&
	    (!paths::IsControllerLocalWorkspace(chat) || !session.memory_recall_position.has_value() ||
	     session.queued_prompt.empty() || !memory_levels::IsEnabled(memory_policy::Resolve(app.settings, chat)) ||
	     app.settings.memory_recall_budget_bytes <= 0 ||
	     session.memory_recall_preparation->state->identity != MemoryRecallSnapshot(app, session, chat)))
		session.memory_recall_preparation.reset();
	// Cancel obsolete context even while startup responses prevent prompt delivery.
	if (session.remote_context_preparation != nullptr)
	{
		const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
		const int memory_budget = session.turn_first_user_message_index <= 0 && memory_levels::IsEnabled(memory_policy::Resolve(app.settings, chat))
		    ? app.settings.memory_recall_budget_bytes : 0;
		const std::shared_ptr<RemoteContextPreparation>& preparation = session.remote_context_preparation;
		if (chat.execution_host_id == execution_hosts::kLocalHostId || host == nullptr ||
		    !execution_hosts::SameConnection(preparation->host, *host) ||
		    preparation->workspace != paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, chat)) ||
		    preparation->prompt != session.queued_prompt || preparation->memory_budget != memory_budget ||
		    preparation->memory_level != memory_policy::Resolve(app.settings, chat))
		{
			preparation->cancel.request_stop();
			session.remote_context_preparation.reset();
		}
	}
	if (session.claude_input_preparation != nullptr)
	{
		const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
		if (session.remote_source_exit_reported || session.queued_prompt.empty() || !UsesBoundedImagePreparation(session) || host == nullptr || session.queued_prompt.size() > platform::kAsyncInputMaxQueuedBytes || session.claude_input_preparation->state->identity != ClaudeInputSnapshot(app, session, chat) || !execution_hosts::SameConnection(session.claude_input_preparation->state->host, *host))
		{
			if (session.claude_input_preparation->state->identity.saved_image_retry)
				return AbortSavedImageRetry(app, session, chat, "Saved image preparation ownership changed. Retry the saved message again.");
			session.claude_input_preparation.reset();
		}
	}
	if (session.remote_source_exit_reported)
		return false;
	// A command made while startup was pending may invalidate an unsent goal turn.
	if (!session.goal_turn_kind.empty() && session.prompt_request_id == 0 && !session.queued_prompt.empty())
	{
		const ChatSession* owner = ChatDomainService().FindChatById(app, uam::strings::NonEmptyOrFallback(chat.goal_owner_chat_id, chat.id));
		if (owner == nullptr || owner->goal_command_revision != session.goal_command_revision)
		{
			RestorePendingPromptDelivery(session, chat);
			ScheduleChatSave(app, chat, 0.0);
			session.queued_prompt.clear();
			session.queued_skill_spans.clear();
			if (session.attached_skill_preparation != nullptr) session.attached_skill_preparation->cancel.request_stop();
			session.attached_skill_preparation.reset();
			session.turn_computer_use_mode = false;
			session.memory_recall_position.reset();
			session.memory_recall_query.clear();
			session.memory_recall_preparation.reset();
			session.claude_input_preparation.reset();
			session.goal_turn_kind.clear();
			session.goal_review_turn = false;
			session.processing = false;
			session.manual_compaction_pending = false;
			session.context_compaction_in_progress = false;
			session.lifecycle_state = session.running ? kAcpLifecycleReady : kAcpLifecycleStopped;
			return true;
		}
	}

	if (session.turn_checkpoint_preflight_pending)
	{
		return false;
	}
	if (session.startup_model_request_id != 0 || session.reasoning_change_request_id != 0 || session.config_option_change_request_id != 0 || session.mode_change_request_id != 0 || session.model_change_request_id != 0 || session.awaiting_model_config_options)
	{
		return false;
	}
	if (!AcpSessionCanSendQueuedPrompt(session))
	{
		return false;
	}
	if (session.protocol_kind == provider_profile_constants::kProtocolOpenCodeAcp)
	{
		if (app.opencode_native_activity == nullptr)
			app.opencode_native_activity = std::make_shared<OpenCodeNativeActivityService>();
		std::string error;
		const OpenCodeNativeGateState gate = app.opencode_native_activity->BeforeDispatch(app, session, chat, error);
		if (gate == OpenCodeNativeGateState::Pending)
			return false;
		if (gate == OpenCodeNativeGateState::Failed)
		{
			FailAcpTurnOrSession(session, &chat, strings::NonEmptyOrFallback(error, "The native OpenCode permission companion is unavailable. The prompt was not sent."));
			return true;
		}
	}
	if (app.opencode_compaction_before_dispatch)
	{
		std::string error;
		const OpenCodeCompactionGateState gate = app.opencode_compaction_before_dispatch(session, chat, error);
		if (gate == OpenCodeCompactionGateState::Pending) return false;
		if (gate != OpenCodeCompactionGateState::Ready && gate != OpenCodeCompactionGateState::NotApplicable)
		{
			session.goal_resume_suppressed = true;
			FailAcpTurnOrSession(session, &chat, strings::NonEmptyOrFallback(std::move(error), "Native compaction metadata could not be verified. The prompt was not sent."));
			// Failed authored rows retain Retry/Delete; only a real future outbox needs Resume.
			session.queue_held = chat.acp_queue_held = !session.queued_user_prompts.empty();
			AppendAcpDiagnostic(session, "native_history", "compaction_metadata_failed", "", "", false, 0, session.last_error);
			ScheduleChatSave(app, chat, 0.0);
			return true;
		}
	}
	if (!ValidateClaudeQuotaDelivery(app, session, chat))
	{
		(void)CancelClaudeQuotaWait(app, chat);
		FailAcpTurnOrSession(session, &chat, "Native continuation ownership changed before delivery.");
		SaveChatQuietly(app, chat);
		return true;
	}
	if (SendStartupModeIfNeeded(session, chat))
	{
		return false;
	}
	if (SendStartupModelIfNeeded(session, chat))
	{
		return false;
	}
	if (!AgentRunScheduler::InitialInputReady(app, session, chat))
		return false;
	if (session.pending_prompt_delivery)
	{
		const std::shared_ptr<PendingAcpPromptDelivery> packet = session.pending_prompt_delivery;
		const bool same_turn = packet->turn_serial == session.turn_serial && packet->prompt == session.queued_prompt;
		const bool owned = OwnsPromptAdmission(app, session, chat, *packet);
		if (!owned || packet->admission == DurableAdmissionState::Failed)
		{
			RestorePendingPromptDelivery(session, chat);
			if (same_turn) FailAcpTurnOrSession(session, &chat, owned
			    ? "Prompt is waiting for its delivery intent to be saved."
			    : "Prompt ownership changed before delivery.");
			ScheduleChatSave(app, chat, 0.0);
			return true;
		}
		if (packet->admission == DurableAdmissionState::Pending) return false;
		session.pending_prompt_delivery.reset();
		return DeliverPreparedPrompt(app, session, chat, packet->message, packet->method, packet->request_id, packet->unsent_messages, packet->remote_host.has_value(), failure_purpose, packet->saved_image_preparation);
	}
	if (session.saved_image_retry)
	{
		const auto images = uam::claude_input::TurnImages(chat, session.turn_first_user_message_index, session.turn_user_message_index);
		std::string error;
		const auto preflight = ProviderRuntimeRegistry::ResolveById(session.provider_id).PrepareSavedImageRetry(session, PromptSettings(app, session, chat), images, error);
		if (preflight == SavedImageRetryPreflight::Pending)
			return false;
		if (preflight == SavedImageRetryPreflight::Rejected)
			return AbortSavedImageRetry(app, session, chat, strings::NonEmptyOrFallback(error, "The native provider could not verify saved-image support. The prompt was not sent."));
	}
	if (paths::IsControllerLocalWorkspace(chat) && session.memory_recall_position.has_value() &&
	    memory_levels::IsEnabled(memory_policy::Resolve(app.settings, chat)) && app.settings.memory_recall_budget_bytes > 0)
	{
		if (*session.memory_recall_position > session.queued_prompt.size())
		{
			FailAcpTurnOrSession(session, &chat, "Memory recall prompt ownership changed.");
			return true;
		}
		if (session.memory_recall_preparation == nullptr)
		{
			const std::size_t active = std::ranges::count_if(app.memory_recall_tasks, [](const MemoryRecallTask& task)
			{ return task.state != nullptr && !task.state->finished.load(std::memory_order_acquire); });
			if (active >= 2) return false;
			std::shared_ptr<MemoryRecallPreparation> state = std::make_shared<MemoryRecallPreparation>();
			state->identity = MemoryRecallSnapshot(app, session, chat);
			MemoryRecallTask task;
			task.state = state;
			task.worker = std::make_unique<std::jthread>([state](std::stop_token shutdown)
			{
				std::stop_callback canceled(shutdown, [state]() { state->cancel.request_stop(); });
				try
				{
					AppState detached;
					detached.data_root = state->identity.data_root;
					detached.settings.memory_recall_budget_bytes = state->identity.budget;
					ChatSession input;
					input.workspace_directory = state->identity.workspace;
					input.imported_read_only = state->identity.workspace.empty();
					input.memory_level = state->identity.memory_level;
					input.small_model_mode = state->identity.small_model_mode;
					state->context = MemoryService::BuildRecallPreface(detached, input, state->identity.query, state->cancel.get_token());
				}
				catch (...) { state->error = "Memory recall could not be prepared. Retry the prompt."; }
				state->finished.store(true, std::memory_order_release);
			});
			app.memory_recall_tasks.push_back(std::move(task));
			session.memory_recall_preparation = std::make_unique<MemoryRecallPreparationLease>(state);
			return false;
		}
		const std::shared_ptr<MemoryRecallPreparation>& state = session.memory_recall_preparation->state;
		if (!state->finished.load(std::memory_order_acquire)) return false;
		if (!state->error.empty())
		{
			const std::string error = state->error;
			FailAcpTurnOrSession(session, &chat, error);
			return true;
		}
	}
	std::string prompt = PreparedPromptText(session);
	std::string context;
	if (chat.execution_host_id != execution_hosts::kLocalHostId)
	{
		const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
		const std::string workspace = paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, chat));
		const bool first_prompt = session.turn_first_user_message_index <= 0;
		const int memory_budget = first_prompt && memory_levels::IsEnabled(memory_policy::Resolve(app.settings, chat))
		    ? app.settings.memory_recall_budget_bytes : 0;
		if (host == nullptr)
		{
			FailAcpTurnOrSession(session, &chat, "The SSH host no longer exists.");
			return true;
		}
		if ((host->instruction_text ? !host->instruction_text->empty() : !host->instruction_file.empty()) || memory_budget > 0)
		{
			std::shared_ptr<RemoteContextPreparation>& preparation = session.remote_context_preparation;
			if (preparation != nullptr && (!execution_hosts::SameConnection(preparation->host, *host) ||
			    preparation->workspace != workspace || preparation->prompt != prompt ||
		    preparation->memory_budget != memory_budget || preparation->memory_level != memory_policy::Resolve(app.settings, chat)))
			{
				preparation->cancel.request_stop();
				preparation.reset();
			}
			if (preparation == nullptr)
			{
				preparation = std::make_shared<RemoteContextPreparation>();
				preparation->host = *host; preparation->workspace = workspace; preparation->prompt = prompt;
				preparation->memory_budget = memory_budget;
				preparation->memory_level = memory_policy::Resolve(app.settings, chat);
				RemoteContextTask task;
				task.state = preparation;
				task.worker = std::make_unique<std::jthread>([state = preparation, memory_budget](std::stop_token shutdown)
				{
					std::stop_callback canceled(shutdown, [state]() { state->cancel.request_stop(); });
					(void)remote::ResolveHostContext(PlatformServicesFactory::Instance().process_service,
					    state->host, state->workspace, memory_budget, state->context, state->error, state->cancel.get_token());
					state->finished.store(true, std::memory_order_release);
				});
				app.remote_context_tasks.push_back(std::move(task));
				return false;
			}
			if (!preparation->finished.load(std::memory_order_acquire)) return false;
			if (!preparation->error.empty())
			{
				const std::string error = preparation->error;
				preparation.reset();
				FailAcpTurnOrSession(session, &chat, error);
				return true;
			}
			context = preparation->context;
		}
	}
	if (!session.queued_skill_spans.empty())
	{
		const std::vector<attached_skills::Span> spans = PreparedSkillSpans(session);
		std::string error;
		if (!attached_skills::Validate(prompt, spans, error))
		{
			FailAcpTurnOrSession(session, &chat, error);
			ScheduleChatSave(app, chat, 0.0);
			return true;
		}
		std::shared_ptr<RemoteContextPreparation>& preparation = session.attached_skill_preparation;
		if (preparation == nullptr)
		{
			const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
			if (host == nullptr) { FailAcpTurnOrSession(session, &chat, "The execution host no longer exists."); return true; }
			const std::size_t active = std::ranges::count_if(app.remote_context_tasks, [](const RemoteContextTask& task)
			{ return task.state != nullptr && !task.state->finished.load(std::memory_order_acquire); });
			if (active >= 2) return false;
			preparation = std::make_shared<RemoteContextPreparation>();
			preparation->data_root = app.data_root; preparation->incarnation = chat.metadata_incarnation;
			preparation->host = *host;
			preparation->workspace = paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, chat));
			preparation->ownership = AttachedSkillOwnership(app, session, chat);
			preparation->prompt = prompt; preparation->skill_spans = spans;
			RemoteContextTask task;
			task.state = preparation;
			task.worker = std::make_unique<std::jthread>([state = preparation](std::stop_token shutdown)
			{
				std::stop_callback canceled(shutdown, [state]() { state->cancel.request_stop(); });
				try
				{
					state->skills_prepared = attached_skills::Prepare(state->data_root, state->host, state->workspace,
					    state->prompt, state->skill_spans, state->wire_prompt, state->error, state->cancel.get_token());
				}
				catch (...) { state->error = "Attached skill files could not be prepared."; }
				state->finished.store(true, std::memory_order_release);
			});
			app.remote_context_tasks.push_back(std::move(task));
			return false;
		}
		if (!preparation->finished.load(std::memory_order_acquire)) return false;
		if (!preparation->error.empty())
		{
			// Only preparation failure can use the captured inline form, before any delivery receipt.
			AppendAcpDiagnostic(session, "skills", "resource_preparation_inline_fallback", "", "", false, 0, preparation->error);
			preparation->error.clear();
		}
		prompt = WirePromptText(session);
	}
	session.lifecycle_state = kAcpLifecycleProcessing;

	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(session.provider_id);
	if (chat.remote_pending_requests.size() >= 64)
	{
		session.last_error = "Wait for pending remote requests to be confirmed before sending another prompt.";
		return true;
	}
	std::string method;
	nlohmann::json msg;
	const int first = session.turn_first_user_message_index >= 0 ? session.turn_first_user_message_index : session.turn_user_message_index;
	const std::vector<MessageAttachment> images = UsesBoundedImagePreparation(session) ? uam::claude_input::TurnImages(chat, first, session.turn_user_message_index) : std::vector<MessageAttachment>{};
	if (!images.empty())
	{
		const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
		if (host == nullptr) { FailAcpTurnOrSession(session, &chat, "The execution host no longer exists."); return true; }
		if (prompt.size() > platform::kAsyncInputMaxQueuedBytes)
		{ FailAcpTurnOrSession(session, &chat, "The prompt exceeds UAM's 4 MiB input limit."); return true; }
		const ClaudeInputIdentity identity = ClaudeInputSnapshot(app, session, chat);
		if (session.claude_input_preparation != nullptr && (session.claude_input_preparation->state->identity != identity || !execution_hosts::SameConnection(session.claude_input_preparation->state->host, *host)))
		{
			if (session.claude_input_preparation->state->identity.saved_image_retry)
				return AbortSavedImageRetry(app, session, chat, "Saved image preparation ownership changed. Retry the saved message again.");
			session.claude_input_preparation.reset();
		}
		if (session.claude_input_preparation == nullptr)
		{
			// A small active-worker cap bounds concurrent image decoder memory.
			const std::size_t active = std::ranges::count_if(app.claude_input_tasks, [](const ClaudeInputPreparationTask& task)
			{ return task.state != nullptr && !task.state->finished.load(std::memory_order_acquire); });
			if (active >= 2) return false;
			std::shared_ptr<ClaudeInputPreparation> state = std::make_shared<ClaudeInputPreparation>();
			state->identity = identity; state->host = *host;
			ClaudeInputPreparationTask task;
			task.state = state;
			task.worker = std::make_unique<std::jthread>([state](std::stop_token shutdown)
			{
				std::stop_callback canceled(shutdown, [state]() { state->cancel.request_stop(); });
				try
				{
					if (state->identity.saved_image_retry)
					{
						const bool remote_owned = state->identity.saved_image_retry->remote_scope.has_value();
						state->message = uam::claude_input::BuildSavedImageRetry(state->identity.original_root, state->identity.saved_image_retry->chat_id, paths::PathFromUtf8(state->identity.workspace), state->identity.prompt, state->identity.saved_image_retry->attachments, state->error, state->cancel.get_token(),
						    remote_owned ? uam::claude_input::SavedImageSourcePolicy::OwnedImagesOnly : uam::claude_input::SavedImageSourcePolicy::LocalCompatible,
						    remote_owned ? 2 * 1024 * 1024 - 16 * 1024 : platform::kAsyncInputMaxQueuedBytes);
					}
					else if (state->host.id == execution_hosts::kLocalHostId)
						state->message = uam::claude_input::Build(state->identity.prompt, paths::PathFromUtf8(state->identity.workspace), state->identity.images, state->error, true, state->cancel.get_token());
					else
					{
						remote::RunnerClient client(PlatformServicesFactory::Instance().process_service, remote::SshBridgeArgv(state->host.ssh_alias, state->host.platform, state->host.runner_version, state->host.runner_directory, state->host.runner_protocol_version), state->host.runner_version, state->host.runner_protocol_version);
						state->message = uam::claude_input::BuildWithReader(state->identity.prompt, state->identity.images, state->error, true, state->cancel.get_token(), [&client, &state](const MessageAttachment& image, std::string& bytes, std::size_t max_bytes, std::string& error) { return client.ReadWorkspaceAttachment(state->identity.workspace, image.path, bytes, max_bytes, &error, state->cancel.get_token()); }, 2 * 1024 * 1024 - 16 * 1024);
					}
				}
				catch (...) { state->error = "Image preparation failed. Remove or replace the image and try again."; }
				state->finished.store(true, std::memory_order_release);
			});
			app.claude_input_tasks.push_back(std::move(task));
			session.claude_input_preparation = std::make_unique<ClaudeInputPreparationLease>(std::move(state));
			return false;
		}
		const std::shared_ptr<ClaudeInputPreparation>& state = session.claude_input_preparation->state;
		if (!state->finished.load(std::memory_order_acquire)) return false;
		if (!state->error.empty())
		{
			const std::string error = state->error;
			session.claude_input_preparation.reset();
			FailAcpTurnOrSession(session, &chat, error); return true;
		}
		if (session.protocol_kind == provider_profile_constants::kProtocolClaudeCodeStreamJson)
			msg = state->message;
	}
	else
		session.claude_input_preparation.reset();
	if (msg.is_null())
	{
		const ChatSession prompt_chat = PromptSettings(app, session, chat);
		msg = runtime.OnAcpBuildPrompt(session, session.next_request_id, context + prompt, prompt_chat, method);
		if (session.claude_input_preparation != nullptr && !msg.is_null())
		{
			const auto& state = session.claude_input_preparation->state;
			if (state->identity.saved_image_retry && !state->identity.native_metadata.is_null())
			{
				if (!runtime.EncodePreparedSavedImageRetry(msg, state->message, session.last_error, state->identity.images, state->identity.native_metadata))
					return AbortSavedImageRetry(app, session, chat, session.last_error);
			}
			else
				structured_attachments::AppendPreparedAcpImages(msg, state->message);
		}
	}
	if (!msg.is_null() && !msg.empty())
	{
		const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
		const bool remote = !chat.execution_host_id.empty() && chat.execution_host_id != execution_hosts::kLocalHostId;
		if (remote && host == nullptr)
		{
			FailAcpTurnOrSession(session, &chat, "The execution host no longer exists.");
			return true;
		}
		const std::string workspace = remote ? chat.workspace_directory : paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, chat));
		auto attachments = structured_attachments::CurrentTurn(chat, session);
		if (session.saved_image_retry)
		{
			if (!session.claude_input_preparation || !OwnsPreparedSavedImageRetry(app, session, chat, session.claude_input_preparation->state))
				return AbortSavedImageRetry(app, session, chat, "Saved image Retry has no complete prepared-image witness.");
			const bool native_images = !session.claude_input_preparation->state->identity.native_metadata.is_null();
			std::erase_if(attachments, [native_images](const MessageAttachment& attachment) { return attachment.kind == "image" && (native_images || attachment.source == AttachmentSource::OwnedNativeAsset); });
		}
		if (!structured_attachments::AppendNative(msg, session, attachments, workspace, host != nullptr ? host->platform : std::string{}, remote, session.last_error))
		{
			FailAcpTurnOrSession(session, &chat, session.last_error);
			return true;
		}
		if (remote && SerializeStructuredMessage(session, msg).size() > 2 * 1024 * 1024)
		{
			FailAcpTurnOrSession(session, &chat, "The prompt and attachments exceed the SSH 2 MiB input limit. Shorten the prompt or send fewer images.");
			return true;
		}
	}
	if (session.saved_image_retry && !msg.is_null() && !msg.empty())
	{
		if (SerializeStructuredMessage(session, msg).size() > platform::kAsyncInputMaxQueuedBytes)
			return AbortSavedImageRetry(app, session, chat, "The prompt and saved images exceed UAM's 4 MiB input limit.");
		const auto& state = session.claude_input_preparation->state;
		state->sealed_message = msg;
		state->sealed_method = method;
		state->sealed_request_id = session.next_request_id;
	}
	const int id = session.next_request_id++;

	if (msg.is_null() || msg.empty())
	{
		FailAcpTurnOrSession(session, &chat, session.last_error);
		return true;
	}

	std::vector<std::size_t> unsent_messages;
	const int first_user_message_index = session.turn_first_user_message_index;
	const int last_user_message_index = session.turn_user_message_index;
	for (int index = first_user_message_index; index >= 0 && index <= last_user_message_index && index < static_cast<int>(chat.messages.size()); ++index)
	{
		Message& user_message = chat.messages[static_cast<std::size_t>(index)];
		if (user_message.role == MessageRole::User && user_message.acp_prompt_not_sent)
		{
			unsent_messages.push_back(static_cast<std::size_t>(index));
			user_message.acp_prompt_not_sent = false;
		}
	}
	if (session.defer_prompt_save && chat.execution_host_id == execution_hosts::kLocalHostId)
	{
		(void)StagePromptAdmission(app, session, chat, msg, method, id, unsent_messages);
		return true;
	}
	if (!CanDeferRemotePromptAdmission(app, session, chat) && !CanStageRemoteGoalPromptAdmission(app, session, chat) && !unsent_messages.empty() && !SaveChatQuietly(app, chat))
	{
		for (const std::size_t index : unsent_messages) chat.messages[index].acp_prompt_not_sent = true;
		session.last_error = "Prompt is waiting for its delivery intent to be saved.";
		return true;
	}
	return DeliverPreparedPrompt(app, session, chat, msg, method, id, unsent_messages, false, failure_purpose, session.saved_image_retry && session.claude_input_preparation ? session.claude_input_preparation->state : nullptr);
}

void InterruptUnconfirmedAcpSteers(AppState& app, AcpSessionState& session, ChatSession& chat)
{
	bool changed = false;
	for (const std::pair<const std::string, AcpPendingSteerState>& entry : session.pending_steer_requests)
	{
		const int index = entry.second.user_message_index;
		if (index < 0 || index >= static_cast<int>(chat.messages.size())) continue;
		Message& message = chat.messages[static_cast<std::size_t>(index)];
		if (message.role != MessageRole::User) continue;
		message.interrupted = true;
		changed = true;
	}
	if (changed) (void)SaveChatQuietly(app, chat);
}

/// <summary>Retire only proven undelivered direct receipts while retaining their authored message.</summary>
static std::optional<ChatSession> FailedDirectPromptRecovery(
    const AppState& app, const ChatSession& chat, const AcpSessionState& session)
{
	if ((session.lifecycle_state != kAcpLifecycleError && !(session.queue_held && !session.running)) || session.processing || session.prompt_request_id != 0 || !session.queued_prompt.empty() || session.pending_prompt_delivery || session.pending_agent_input || session.pending_local_launch || session.pending_local_session_receipt || session.pending_queued_history || session.worktree_launch_pending || session.turn_checkpoint_preflight_pending || session.turn_checkpoint_commit_pending || session.manual_compaction_pending || session.context_compaction_in_progress || session.model_discovery_only || !session.managed_agent_run_id.empty() || session.reconnect_pending || session.recovering_remote_turn || session.recovering_remote_process || session.remote_stop_pending || session.remote_stop_unconfirmed || AcpSessionHasPendingCancel(session) || !session.pending_steer_requests.empty() || !session.pending_request_methods.empty() || session.goal_internal_session || ProviderRuntimeRegistry::ResolveById(session.provider_id).AcpBackgroundTaskCount(session) > 0 || session.provider_id != chat.provider_id || !chat.messages_loaded || chat.imported_read_only || chat.acp_queue_storage_error || chat.side_cleanup_requested || chat.remote_turn_reconnect_pending || chat.remote_restart_pending || chat.remote_stop_cleanup_pending || chat.remote_source_exit_pending || !chat.remote_active_turn_id.empty() || !chat.remote_prompt_delivery_session_id.empty() || !chat.remote_prompt_delivery_id.empty() || !chat.remote_prompt_delivery_payload.empty() || !chat.remote_pending_requests.empty() || !chat.remote_interaction_responses.empty() || chat.acp_dispatched_queued_prompt_count != 0 || session.queued_rollback_save_pending || !prompt_recovery::BindingsMatch(chat) || chat.acp_queued_prompts.empty() || chat.acp_queued_prompts.size() != session.queued_user_prompts.size())
		return {};
	for (std::size_t index = 0; index < chat.acp_queued_prompts.size(); ++index)
		if (chat.acp_queued_prompts[index] != session.queued_user_prompts[index]) return {};
	const std::vector<int> known = KnownUnsentAcpMessageIndices(app, chat);
	std::optional<ChatSession> recovered;
	std::vector<AcpQueuedUserPromptState> retained;
	for (const AcpQueuedUserPromptState& prompt : chat.acp_queued_prompts)
	{
		// A referenced future or workspace retry retains its captured policy until explicit dispatch.
		if (!prompt.direct_admission || prompt.include_prior_context || prompt.prepared_for_delivery || !prompt.claude_quota_continuation_id.empty())
		{
			retained.push_back(prompt);
			continue;
		}
		int recovered_index = prompt.existing_user_message_index;
		if (prompt.existing_user_message_index >= 0)
		{
			const std::size_t index = static_cast<std::size_t>(prompt.existing_user_message_index);
			if (index >= chat.messages.size()) return {};
			const Message& row = chat.messages[index];
			if (row.role != MessageRole::User || !row.acp_prompt_not_sent || row.acp_prompt_discarded ||
			    row.created_at != prompt.existing_user_message_created_at || row.content != prompt.text ||
			    row.attachments != prompt.attachments || row.markdown_store_files != prompt.markdown_store_files ||
			    row.markdown_store_prompt_blocks != prompt.markdown_store_prompt_blocks ||
			    std::ranges::find(known, static_cast<int>(index)) == known.end()) return {};
			if (!recovered) recovered = chat;
			recovered->messages[index].interrupted = true;
		}
		else
		{
			if (!prompt.append_user_message || (prompt.text.empty() && prompt.attachments.empty() && prompt.markdown_store_prompt_blocks.empty())) return {};
			if (!recovered) recovered = chat;
			ChatDomainService::MessageAnalytics analytics; analytics.provider = chat.provider_id;
			ChatDomainService().AddMessageWithAnalytics(*recovered, MessageRole::User, prompt.text, analytics);
			recovered_index = static_cast<int>(recovered->messages.size() - 1);
			Message& row = recovered->messages.back();
			row.interrupted = row.acp_prompt_not_sent = true;
			row.priority_steer = prompt.priority_steer;
			row.attachments = prompt.attachments;
			row.markdown_store_files = prompt.markdown_store_files;
			row.markdown_store_prompt_blocks = prompt.markdown_store_prompt_blocks;
			for (const MessageAttachment& attachment : row.attachments)
				if (!attachment.path.empty() && std::ranges::find(recovered->linked_files, attachment.path) == recovered->linked_files.end())
					recovered->linked_files.push_back(attachment.path);
		}
		if (prompt_recovery::HasCapturedPolicy(prompt))
		{
			auto value = prompt_recovery::Capture(*recovered, recovered_index, prompt);
			if (!prompt_recovery::Bounded(value))
				return {};
			auto& values = recovered->acp_unsent_prompt_recoveries;
			std::erase_if(values, [&](const auto& previous) { return previous.user_message_index == recovered_index; });
			values.push_back(std::move(value));
			if (!prompt_recovery::Bounded(values))
				return {};
		}
	}
	if (recovered)
	{
		recovered->acp_queued_prompts = std::move(retained);
		recovered->acp_queue_held = !recovered->acp_queued_prompts.empty();
	}
	return recovered;
}

bool SaveChatQuietly(AppState& app, const ChatSession& chat, AcpChatSavePurpose purpose, SavedChatPrimaryWitness* own_primary)
{
	AcpSessionState* continuation_owner = FindAcpSessionForChat(app, chat.id);
	if (own_primary == nullptr && continuation_owner && continuation_owner->managed_parent_continuation &&
	    !continuation_owner->managed_parent_continuation->revoked)
	{
		SavedChatPrimaryWitness actual;
		const bool saved = SaveChatQuietly(app, chat,
		    continuation_owner->managed_parent_continuation->internal_turn ? AcpChatSavePurpose::PreserveQueuedIntent : purpose, &actual);
		AcpSessionState* current = FindAcpSessionForChat(app, chat.id);
		if (saved && current == continuation_owner) ManagedParentContinuationService::ObservePrimarySaved(*current, actual);
		return saved;
	}
	if (own_primary) *own_primary = {};
	if (AgentRunScheduler::HasUnconfirmedInitialRootPublication(app, chat))
		return false;
	// Discovery sessions are temporary and must not reappear as chats on the next launch.
	if (std::any_of(app.model_discovery_chats.begin(), app.model_discovery_chats.end(),
	        [&chat](const ChatSession& discovery) { return discovery.id == chat.id; })) return true;
	ChatSession* persisted = ChatDomainService().FindChatById(app, chat.id);
	const bool had_remote_delivery_authority = persisted &&
	    (persisted->remote_turn_reconnect_pending || !persisted->remote_active_turn_id.empty() ||
	     !persisted->remote_prompt_delivery_session_id.empty() || !persisted->remote_prompt_delivery_id.empty() ||
	     !persisted->remote_prompt_delivery_payload.empty() || !persisted->remote_pending_requests.empty());
	const AcpSessionState* original_session = persisted ? FindAcpSessionForChat(app, chat.id) : nullptr;
	const bool preserve_remote_delivery_authority = had_remote_delivery_authority && original_session &&
	    original_session->lifecycle_state == kAcpLifecycleError &&
	    (!persisted->messages_loaded || std::ranges::any_of(persisted->messages, [](const Message& row)
	    { return row.role == MessageRole::User && row.acp_prompt_not_sent; }));
	if (persisted != nullptr)
	{
		if (const AcpSessionState* active = FindAcpSessionForChat(app, chat.id))
			persisted->interaction_at = uam::time::LatestInteractionTimestamp(persisted->interaction_at, active->interaction_at);
	}
	if (persisted != nullptr && persisted->execution_host_id != uam::execution_hosts::kLocalHostId)
	{
		if (const AcpSessionState* active = FindAcpSessionForChat(app, chat.id))
		{
			persisted->remote_next_request_id = std::max(persisted->remote_next_request_id, active->next_request_id);
			persisted->remote_turn_serial = std::max(persisted->remote_turn_serial, active->turn_serial);
			// Only an explicit runtime settlement may clear an uncertain failed prompt's authority.
			if (!preserve_remote_delivery_authority)
			{
				persisted->remote_turn_user_message_index = uam::AcpSessionHasActiveTurn(*active)
				    ? active->turn_user_message_index : -1;
				persisted->remote_active_turn_id = ProviderRuntimeRegistry::ResolveById(active->provider_id).AcpTurnIdentity(*active);
			}
		}
	}
	if (persisted != nullptr && persisted->remote_turn_reconnect_pending && !preserve_remote_delivery_authority && !persisted->remote_manual_compaction_pending)
	{
		const AcpSessionState* session = FindAcpSessionForChat(app, chat.id);
		if (session == nullptr ||
		    (!uam::AcpSessionHasActiveTurn(*session) && !session->reconnect_pending))
		{
			persisted->remote_turn_reconnect_pending = false;
			if (!persisted->remote_process_exists)
			{
				persisted->remote_delivered_stdout_cursor = 0;
				persisted->remote_delivered_stderr_cursor = 0;
			}
		}
	}
	if (persisted != nullptr && !persisted->remote_turn_reconnect_pending && !persisted->remote_process_exists)
	{
		persisted->remote_delivered_stdout_cursor = 0;
		persisted->remote_delivered_stderr_cursor = 0;
	}
	FinishPendingQueuedAdmissionSave(app, chat.id);
	InvalidatePendingChatSave(app, chat.id);
	AcpSessionState* active = persisted ? FindAcpSessionForChat(app, chat.id) : nullptr;
	const CompletedAcpPromptRecoverySave completed = persisted ? PrepareCompletedAcpPromptRecoverySave(app, *persisted) : CompletedAcpPromptRecoverySave{};
	const std::vector<AcpUnsentPromptRecovery> previous_recoveries = completed.records.empty() ? std::vector<AcpUnsentPromptRecovery>{} : persisted->acp_unsent_prompt_recoveries;
	if (persisted)
		ApplyCompletedAcpPromptRecoverySave(*persisted, completed);
	std::optional<ChatSession> recovered = purpose != AcpChatSavePurpose::PreserveQueuedIntent && active && persisted && !had_remote_delivery_authority ? FailedDirectPromptRecovery(app, *persisted, *active) : std::nullopt;
	std::optional<ChatSession> previous_chat;
	std::deque<AcpQueuedUserPromptState> previous_queue;
	const bool previous_held = active && active->queue_held;
	if (recovered)
	{
		previous_chat = *persisted;
		previous_queue = active->queued_user_prompts;
		*persisted = std::move(*recovered);
		active->queued_user_prompts.assign(persisted->acp_queued_prompts.begin(), persisted->acp_queued_prompts.end());
		active->queue_held = persisted->acp_queue_held;
	}
	const ChatSession& actual = persisted == nullptr ? chat : *persisted;
	const bool saved = own_primary ? ChatRepository::SaveLoadedChatWithPrimaryWitness(app.data_root, actual, *own_primary)
	    : ChatRepository::SaveChat(app.data_root, actual);
	if (recovered)
	{
		if (saved && !active->running) active->initialized = active->session_ready = false;
		if (!saved)
		{
			*persisted = std::move(*previous_chat);
			active->queued_user_prompts = std::move(previous_queue);
			active->queue_held = previous_held;
		}
	}
	if (persisted && !completed.records.empty())
	{
		if (saved)
			CommitCompletedAcpPromptRecoverySave(app, *persisted, completed);
		else
			persisted->acp_unsent_prompt_recoveries = previous_recoveries;
	}
	if (saved)
	{
		AgentRunScheduler::InitialChatSaved(app, persisted == nullptr ? chat : *persisted);
		if (active)
			active->queued_rollback_save_pending = false;
		app.pending_chat_save_at_by_chat_id.erase(chat.id);
	}
	else if (persisted != nullptr)
	{
		ScheduleChatSave(app, *persisted, 1.0);
	}
	ResolvePendingChatSaveResults(app, chat.id, saved);
	return saved;
}


bool ClearSavedAcpResumeId(AppState& app, AcpSessionState& session, ChatSession& chat)
{
	if (!AgentRunScheduler::ManagedResumeIdentityAllowed(app, session, chat, {}))
		return false;
	const std::string previous_id = chat.native_session_id;
	chat.native_session_id.clear();
	if (SaveChatQuietly(app, chat)) return true;
	chat.native_session_id = previous_id;
	const std::string message = "Could not save session recovery data. Check disk space and folder access, then retry.";
	AppendAcpDiagnostic(session, "persistence", "resume_identity_save_failed", "", "", false, 0, message);
	InvalidateAcpTransport(app, session, chat, message);
	return false;
}

void ScheduleChatSave(AppState& app, const ChatSession& chat, double delay_seconds)
{
	NoteChatSaveScheduled(app, chat);
	// Streaming may retain a scoped admission; queue, owner changes and force barriers revoke it.
	InvalidatePendingChatSave(app, chat.id, true);
	const double now = GetAppTimeSeconds();
	const double due_at = now + delay_seconds;
	app.pending_chat_save_at_by_chat_id[chat.id] = due_at;
}

bool SetChatNativeSessionIdIfChanged(ChatSession& chat, std::string_view session_id)
{
	const std::string normalized_session_id = uam::strings::Trim(std::string(session_id));
	if (normalized_session_id.empty() || chat.native_session_id == normalized_session_id)
	{
		return false;
	}

	if (!chat.claude_quota_continuation.id.empty() &&
	    (chat.native_session_id != chat.claude_quota_continuation.native_session_id || normalized_session_id != chat.claude_quota_continuation.native_session_id))
		chat.claude_quota_continuation.state = "cancelled";
	chat.native_session_id = normalized_session_id;
	chat.native_session_reset_pending = false;
	return true;
}

void SyncResolvedNativeSessionIdForChat(AppState& app, const ChatSession& chat, std::string_view session_id, std::string_view previous_session_id)
{
	const std::string normalized_session_id = uam::strings::Trim(std::string(session_id));
	if (normalized_session_id.empty())
	{
		app.resolved_native_sessions_by_chat_id.erase(chat.id);
		return;
	}

	const auto previous_resolved = app.resolved_native_sessions_by_chat_id.find(chat.id);
	const std::string previous_resolved_session_id = previous_resolved == app.resolved_native_sessions_by_chat_id.end() ? std::string{} : uam::strings::Trim(previous_resolved->second);
	const std::string normalized_previous_session_id = uam::strings::Trim(std::string(previous_session_id));
	app.resolved_native_sessions_by_chat_id[chat.id] = normalized_session_id;

	for (const auto& terminal_ptr : app.cli_terminals)
	{
		if (terminal_ptr == nullptr)
		{
			continue;
		}

		CliTerminalState& terminal = *terminal_ptr;
		const std::string attached_session_id = CliTerminalAttachedSessionId(terminal);
		const bool matches_chat_identity = CliTerminalPrimaryChatId(terminal) == chat.id || CliTerminalAttachedChatId(terminal) == chat.id;
		const bool matches_previous_session = attached_session_id == normalized_previous_session_id || attached_session_id == previous_resolved_session_id;
		if (!matches_chat_identity && !matches_previous_session)
		{
			continue;
		}

		if (!matches_chat_identity)
		{
			terminal.frontend_chat_id = chat.id;
			terminal.attached_chat_id = chat.id;
		}

		if (attached_session_id != normalized_session_id)
		{
			terminal.attached_session_id = normalized_session_id;
		}
	}
}

void CompletePromptTurn(AcpSessionState& session, std::string_view lifecycle_state)
{
	if (lifecycle_state != kAcpLifecycleReady && session.managed_parent_continuation)
		session.managed_parent_continuation->revoked = true;
	RevokeSavedImageRetry(session);
	session.pending_local_launch.reset();
	AbandonPendingLocalSteers(session);
	if (session.processing) session.interaction_at = uam::time::InteractionTimestampNow();
	// A completion notification can arrive before the prompt request's reply.
	if (session.prompt_request_id != ManagedParentContinuationService::PendingNativeAckRequest(session))
		session.pending_request_methods.erase(session.prompt_request_id);
	if (session.processing && !session.manual_compaction_pending && session.turn_serial > 0)
	{
		session.last_settled_turn_serial = session.turn_serial;
		session.last_turn_outcome = lifecycle_state == kAcpLifecycleReady ? "completed" : "failed";
		session.last_turn_error = lifecycle_state == kAcpLifecycleReady ? std::string{} : session.last_error;
		AgentRunScheduler::InitialTurnSettled(session, session.last_turn_outcome, session.last_turn_error);
	}
	session.prompt_request_id = 0;
	session.pending_prompt_delivery.reset();
	session.delivered_prompt_recoveries.clear();
	session.defer_prompt_save = false;
	session.processing = false;
	session.manual_compaction_pending = false;
	session.context_compaction_in_progress = false;
	session.recovering_remote_turn = false;
	session.cancel_requested = false;
	session.cancel_requested_time_s = 0.0;
	session.inactivity_timeout_pending = false;
	session.queued_prompt.clear();
	session.queued_skill_spans.clear();
	if (session.attached_skill_preparation != nullptr) session.attached_skill_preparation->cancel.request_stop();
	session.attached_skill_preparation.reset();
	session.turn_computer_use_mode = false;
	session.memory_recall_position.reset();
	session.memory_recall_query.clear();
	session.memory_recall_preparation.reset();
	if (session.remote_context_preparation != nullptr) session.remote_context_preparation->cancel.request_stop();
	session.remote_context_preparation.reset();
	session.claude_input_preparation.reset();
	session.current_assistant_message_index = -1;
	session.codex_turn_id.clear();
	session.load_history_replay_updates.clear();
	session.pending_assistant_thoughts.clear();
	ResetAcpPendingInteractionState(session);
	session.lifecycle_state.assign(lifecycle_state);
}

bool QueueGoalInternalPrompt(AppState& app, AcpSessionState& session, ChatSession& chat,
                             const std::string& prompt, bool review_turn,
                             const std::string& model_id, bool fresh_session, std::function<bool()> continuation_owned,
                             std::function<void(bool, std::string)> completion)
{
	const bool async_receipt = static_cast<bool>(continuation_owned) && static_cast<bool>(completion);
	if (static_cast<bool>(continuation_owned) != static_cast<bool>(completion) || (async_receipt && !continuation_owned())) return false;
	if (fresh_session && !prompt.empty() && !session.processing && !session.waiting_for_permission &&
	    !session.waiting_for_user_input && !session.cancel_requested && session.prompt_request_id == 0 && session.cancel_request_id == 0 &&
	    session.queued_user_prompts.empty() && (session.queued_prompt.empty() ||
	        (async_receipt && session.goal_queue_admission == AcpSessionState::GoalQueueAdmission::Held &&
	         session.goal_queue_admission_turn_serial == session.turn_serial && session.queued_prompt == prompt)))
	{
		const WorktreeLaunchPreparationResult workspace = PrepareWorktreeForLaunch(app, chat);
		if (workspace.state != WorktreeLaunchPreparationState::Ready)
		{
			session.last_error = workspace.state == WorktreeLaunchPreparationState::Pending
			    ? "Workspace preparation is still running. Retry Resume after it finishes." : workspace.error;
			return false;
		}
	}
	if (app.opencode_compaction_before_turn_reset) app.opencode_compaction_before_turn_reset(session, chat);
	if (async_receipt && session.goal_queue_admission == AcpSessionState::GoalQueueAdmission::Held &&
	    session.goal_queue_admission_turn_serial == session.turn_serial && session.queued_prompt == prompt)
	{
		// Only a newly admitted explicit control can retry its retained exact queued intent.
		session.queued_prompt.clear();
		session.queued_skill_spans.clear();
		if (session.attached_skill_preparation != nullptr) session.attached_skill_preparation->cancel.request_stop();
		session.attached_skill_preparation.reset();
		session.turn_computer_use_mode = false;
	}
	if (!async_receipt && session.goal_queue_admission != AcpSessionState::GoalQueueAdmission::None &&
	    session.goal_queue_admission_turn_serial == session.turn_serial) return false;
	const bool fresh_context_busy = session.processing || session.waiting_for_permission ||
	                                session.waiting_for_user_input || session.cancel_requested ||
	                                session.prompt_request_id != 0 || session.cancel_request_id != 0 ||
	                                !session.queued_prompt.empty() || !session.queued_user_prompts.empty();
	if (prompt.empty() || (fresh_session ? fresh_context_busy : !CanQueueGoalInternalPrompt(session)))
	{
		return false;
	}
	bool remote_stop_deferred = false;
	if (fresh_session)
	{
		session.restart_after_remote_stop_cleanup = true;
		if (!StopAcpSession(app, chat.id) && !session.remote_stop_pending)
		{
			session.restart_after_remote_stop_cleanup = false;
			return false;
		}
		remote_stop_deferred = session.remote_stop_pending;
	}
	session.queued_prompt = prompt;
	session.goal_turn_kind = review_turn ? std::string(kGoalTurnKindReview) : std::string(kGoalTurnKindWorkerContinuation);
	session.goal_turn_model_id = uam::strings::Trim(model_id);
	session.goal_internal_session = fresh_session;
	session.goal_review_turn = review_turn;
	if (chat.goal_owner_chat_id.empty() && !chat.active_goal_id.empty())
	{
		chat.goal_iteration_goal_id = chat.active_goal_id;
		chat.goal_iteration_turn_kind = session.goal_turn_kind;
		chat.goal_iteration_repair_attempts = session.goal_review_repair_attempts;
	}
	if (!review_turn)
	{
		session.goal_review_user_prompt = prompt;
	}
	AppendGoalLoopDiagnostic(session, review_turn ? "queue_review" : "queue_worker_continuation", session.goal_review_goal_id, prompt);
	session.processing = true;
	session.turn_checkpoint_eligible = false;
	session.cancel_requested = false;
	session.cancel_requested_time_s = 0.0;
	session.inactivity_timeout_pending = false;
	session.current_assistant_message_index = -1;
	session.turn_first_user_message_index = -1;
	session.turn_user_message_index = -1;
	session.turn_assistant_message_index = -1;
	session.turn_serial += 1;
	const ChatSession* goal_owner = ChatDomainService().FindChatById(app, uam::strings::NonEmptyOrFallback(chat.goal_owner_chat_id, chat.id));
	session.goal_command_revision = goal_owner != nullptr ? goal_owner->goal_command_revision : "";
	ResetAcpTurnStreamState(session);
	ResetAcpPendingInteractionState(session);
	session.turn_started_time_s = GetAppTimeSeconds();
	session.last_runtime_activity_time_s = session.turn_started_time_s;
	session.last_error.clear();
	session.lifecycle_state = kAcpLifecycleProcessing;
	if (async_receipt)
	{
		session.goal_queue_admission = AcpSessionState::GoalQueueAdmission::Pending;
		session.goal_queue_admission_turn_serial = session.turn_serial;
		session.goal_resume_suppressed = true;
		const std::filesystem::path root = app.data_root;
		const std::string chat_id = chat.id;
		const std::shared_ptr<const std::uint8_t> incarnation = chat.metadata_incarnation;
		const std::string process = session.last_process_id;
		const std::string actual_process = AcpProcessHandleLabel(session);
		const std::string thread = session.session_id;
		const int serial = session.turn_serial;
		const std::string revision = chat.goal_command_revision;
		const std::string configuration = PromptDeliveryConfiguration(app, session, chat);
		const std::shared_ptr<bool> finished = std::make_shared<bool>(false);
		const std::function<bool()> owned = [&app, continuation_owned, finished, root, chat_id, incarnation,
		    process, actual_process, thread, serial, revision, configuration, prompt, model_id]()
		{
			if (*finished || !continuation_owned()) return false;
			const AcpSessionState* current = FindAcpSessionForChat(app, chat_id);
			const ChatSession* stored = ChatDomainService().FindChatById(app, chat_id);
			return app.data_root == root && stored && current && stored->metadata_incarnation == incarnation &&
			    stored->goal_command_revision == revision && current->last_process_id == process && AcpProcessHandleLabel(*current) == actual_process &&
			    current->session_id == thread && current->turn_serial == serial && !current->cancel_requested &&
			    current->goal_queue_admission == AcpSessionState::GoalQueueAdmission::Pending && current->goal_queue_admission_turn_serial == serial &&
			    current->queued_prompt == prompt && current->goal_turn_model_id == uam::strings::Trim(model_id) &&
			    configuration == PromptDeliveryConfiguration(app, *current, *stored);
		};
		const std::function<void(DurableAdmissionState)> settle = [&app, owned, finished, chat_id,
		    fresh_session, remote_stop_deferred, completion](DurableAdmissionState state)
		{
			if (*finished) return;
			const bool current = owned();
			*finished = true;
			if (!current) { completion(false, "Goal context changed before queue publication."); return; }
			AcpSessionState* native = FindAcpSessionForChat(app, chat_id);
			ChatSession* stored = ChatDomainService().FindChatById(app, chat_id);
			if (state != DurableAdmissionState::Committed)
			{
				native->processing = false;
				native->goal_queue_admission = AcpSessionState::GoalQueueAdmission::Held;
				native->goal_resume_suppressed = true;
				native->last_error = "Failed to persist the goal continuation.";
				completion(false, native->last_error);
				return;
			}
			native->goal_queue_admission = AcpSessionState::GoalQueueAdmission::None;
			native->goal_resume_suppressed = false;
			if (fresh_session && !remote_stop_deferred && !StartAcpProcessForChat(app, *native, *stored))
			{
				FailAcpTurnOrSession(*native, stored, uam::strings::NonEmptyOrFallback(native->last_error, "Failed to start the private goal context."));
				ScheduleChatSave(app, *stored, 0.0);
				completion(false, native->last_error);
				return;
			}
			(void)ScheduleTurnCheckpointPreflight(app, *native, *stored);
			ScheduleChatSave(app, *stored, 0.0);
			(void)SendQueuedPromptIfReady(app, *native, *stored);
			ScheduleChatSave(app, *stored, 0.0);
			completion(true, "");
		};
		const DurableAdmissionState admission = BeginDurableAdmission(app, chat, owned, settle,
		    DurableAdmissionFailurePurpose::BindingOnly);
		if (admission == DurableAdmissionState::Failed) settle(admission);
		return true;
	}
	if (fresh_session && !remote_stop_deferred &&
	    !StartAcpProcessForChat(app, session, chat))
	{
		FailAcpTurnOrSession(session, &chat,
		                     uam::strings::NonEmptyOrFallback(session.last_error, "Failed to start the private goal context."));
		SaveChatQuietly(app, chat);
		return false;
	}
	(void)ScheduleTurnCheckpointPreflight(app, session, chat);
	SaveChatQuietly(app, chat);
	// Queued is success; if the session cannot send yet, the poll loop
	// delivers the prompt once the session is ready.
	(void)SendQueuedPromptIfReady(app, session, chat);
	SaveChatQuietly(app, chat);
	return true;
}

void ClearGoalReviewState(AcpSessionState& session)
{
	session.goal_review_turn = false;
	session.goal_review_scheduled = false;
	session.goal_review_goal_id.clear();
	session.goal_review_user_prompt.clear();
	session.goal_review_assistant_text.clear();
	session.goal_review_repair_attempts = 0;
}

void FailAcpTurnOrSession(AcpSessionState& session, ChatSession* chat,
	                      const std::string& message)
{
	if (session.managed_parent_continuation)
	{
		session.managed_parent_continuation->revoked = true;
		if (chat && chat->managed_parent_result && chat->managed_parent_result->id == session.managed_parent_continuation->id)
		{
			chat->managed_parent_result->stage = "held";
			chat->managed_parent_result->reason = "native_error";
		}
	}
	session.last_error = message;
	if (chat != nullptr)
	{
		RestorePendingPromptDelivery(session, *chat);
		// Retire rejected prepared wire text while retaining the authored unsent message.
		// Unconfirmed deliveries and later queued prompts stay intact.
		if (session.prompt_request_id == 0 && !session.queued_prompt.empty() &&
		    !chat->remote_turn_reconnect_pending && chat->remote_pending_requests.empty() &&
		    chat->remote_prompt_delivery_id.empty() && chat->remote_prompt_delivery_payload.empty() &&
		    chat->acp_dispatched_queued_prompt_count == 1 && !chat->acp_queued_prompts.empty() &&
		    chat->acp_queued_prompts.front().prepared_for_delivery &&
		    chat->acp_queued_prompts.front().text == session.queued_prompt)
		{
			chat->acp_queued_prompts.erase(chat->acp_queued_prompts.begin());
			chat->acp_dispatched_queued_prompt_count = 0;
		}
		if (session.turn_user_message_index >= 0 && session.turn_user_message_index < static_cast<int>(chat->messages.size()))
		{
			Message& user = chat->messages[static_cast<std::size_t>(session.turn_user_message_index)];
			if (user.role == MessageRole::User && user.acp_prompt_not_sent) user.interrupted = true;
		}
		const int index = session.current_assistant_message_index >= 0
		                      ? session.current_assistant_message_index
		                      : session.turn_assistant_message_index;
		if (index >= 0 && index < static_cast<int>(chat->messages.size()) &&
		    chat->messages[static_cast<std::size_t>(index)].role == MessageRole::Assistant)
		{
			chat->messages[static_cast<std::size_t>(index)].interrupted = true;
		}
	}
	if (session.processing || session.waiting_for_permission || session.prompt_request_id != 0 || !session.queued_prompt.empty())
	{
		CompletePromptTurn(session, kAcpLifecycleError);
		return;
	}

	session.lifecycle_state = kAcpLifecycleError;
}

void MarkAcpChatUnseenIfBackground(AppState& app, const ChatSession& chat)
{
	ChatDomainService().MarkChatNeedsAttention(app, chat.id);
}

} // namespace uam::acp_detail

namespace uam
{
/// <summary>Normal history hydration may retire stopped direct intent, never replay it.</summary>
bool SettleStoppedDirectAcpAdmissions(AppState& app, ChatSession& chat)
{
	if (!chat.messages_loaded || !chat.acp_queue_held || chat.acp_queue_storage_error ||
	    chat.imported_read_only || chat.acp_queued_prompts.empty()) return false;
	AcpSessionState* session = FindAcpSessionForChat(app, chat.id);
	if (!session)
	{
		if (chat.remote_turn_reconnect_pending || chat.remote_restart_pending || chat.remote_stop_cleanup_pending ||
		    !chat.remote_active_turn_id.empty() || !chat.remote_prompt_delivery_id.empty() ||
		    !chat.remote_pending_requests.empty()) return false;
		session = &acp_detail::EnsureAcpSessionForChat(app, chat);
	}
	if (!acp_detail::FailedDirectPromptRecovery(app, chat, *session)) return false;
	return acp_detail::SaveChatQuietly(app, chat);
}

}
