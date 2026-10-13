#include "computer_use/computer_use_mcp_config.h"
#include "common/config/provider_availability.h"
#include "common/config/settings_frontend_json.h"
#include <atomic>
#include <thread>
#include "common/provider/provider_setup_input.h"
#include "common/runtime/acp/claude_quota_continuation.h"
#include "app/execution_routing_integration.h"
#include "app/provider_configuration_service.h"
#include "app/worktree_launch_preparation_service.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_session_runtime.h"

#include "app/agent_definition_service.h"
#include "app/memory_service.h"
#include "common/provider/provider_runtime.h"
#include "app/chat_domain_service.h"
#include "app/provider_resolution_service.h"
#include "app/git_worktree_service.h"
#include "app/uam_control_service.h"
#include "common/chat/chat_repository.h"
#include "common/config/approval_modes.h"
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
#include "remote/runner_proxy.h"
#include "remote/host_context.h"
#include "common/memory/memory_levels.h"
#include "common/memory/memory_policy.h"
#include "remote/runner_protocol.h"

#include <cstring>
#include <filesystem>
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
		bool success = false;
		std::atomic<bool> finished{false};
		std::unique_ptr<std::jthread> worker;
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
		std::string native_session_id;
		std::optional<ExecutionHost> remote_host;
		std::optional<AcpQueuedUserPromptState> remote_prepared_head;
		std::string delivery_session_id;
		std::string delivery_id;
		std::string delivery_payload;
		std::string proxy_token;
		int proxy_protocol = 0;
		acp_detail::DurableAdmissionState admission = acp_detail::DurableAdmissionState::Pending;
	};
}

namespace uam::acp_detail
{

namespace
{
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
	std::string LocalLaunchOwnership(const AppState& app, const AcpSessionState& session, const ChatSession& chat)
	{
		const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
		return nlohmann::json::array({provider.id, provider.interactive_command, provider.runtime_flags, provider.resume_argument,
		    provider.supports_structured, provider.structured_protocol, provider.execution_mode, provider.output_mode,
		    provider.prompt_bootstrap, provider.prompt_bootstrap_path, app.settings.git_auto_worktree,
		    chat.workspace_base_ref,
		    chat.git_base_branch_override ? nlohmann::json(*chat.git_base_branch_override) : nlohmann::json(nullptr), chat.workspace_branch_name, chat.id, chat.created_at, chat.provider_id, chat.native_session_id,
		    chat.execution_host_id, chat.workspace_directory, chat.workspace_source_directory,
		    chat.workspace_worktree_directory, chat.workspace_isolation_kind, chat.routing_mode,
		    chat.routing_assigned_connection_identity, chat.model_id, chat.approval_mode, chat.extra_flags,
		    chat.active_goal_id, chat.goal_command_revision, chat.uam_agent_id, chat.small_model_mode,
		    chat.reasoning_effort, chat.service_tier, chat.command_safety_tier, session.goal_turn_model_id,
		    session.active_uam_agent_id, session.active_uam_agent_definition_hash,
		    session.active_uam_agent_instructions, session.active_uam_agent_execution_capability,
		    session.active_uam_agent_workspace_access, session.last_process_id, session.session_id, AcpProcessHandleLabel(session),
		    session.model_discovery_only, session.goal_internal_session, session.recovering_remote_turn, session.recovering_remote_process,
		    settings_frontend_json::SerializeFingerprintSettingsFields(app.settings),
		    PromptDeliveryConfiguration(app, session, chat)}).dump();
	}
}

std::string LocalSessionReceiptOwnership(const AppState& app, const AcpSessionState& session, const ChatSession& chat)
{
	return LocalLaunchOwnership(app, session, chat);
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

std::string PromptDeliveryConfiguration(const AppState& app, const AcpSessionState& session, const ChatSession& chat)
{
	return nlohmann::json::array({chat.model_id, chat.reasoning_effort, chat.service_tier, chat.service_tier_explicit,
	    chat.approval_mode, chat.command_safety_tier, session.current_model_id,
	    session.current_mode_id, session.goal_turn_model_id, session.active_uam_agent_id,
	    session.active_uam_agent_definition_hash, chat.memory_level, memory_policy::Resolve(app.settings, chat),
	    app.settings.memory_recall_budget_bytes, app.settings.computer_use_globally_enabled, chat.computer_use_enabled,
	    chat.computer_use_backend, chat.computer_use_target_id,
	    chat.computer_use_target_process_id, chat.computer_use_target_input_mode,
	    chat.claude_quota_continuation.id, chat.claude_quota_continuation.state,
	    chat.claude_quota_continuation.chat_id, chat.claude_quota_continuation.native_session_id,
	    chat.claude_quota_continuation.context, chat.claude_quota_continuation.accepted_message_digest,
	    chat.claude_quota_continuation.agent_id, chat.claude_quota_continuation.agent_definition_hash,
	    chat.claude_quota_continuation.agent_workspace_access, chat.claude_quota_continuation.agent_execution_capability,
	    chat.claude_quota_continuation.observed_at_epoch_s, chat.claude_quota_continuation.resets_at_epoch_s,
	    chat.claude_quota_continuation.user_message_index, chat.claude_quota_continuation.message_count}).dump();
}

namespace
{
	bool OwnsPromptAdmission(const AppState& app, const AcpSessionState& session, const ChatSession& chat,
	    const PendingAcpPromptDelivery& packet)
	{
		const bool remote = packet.remote_host.has_value();
		const ExecutionHost* host = remote ? execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id) : nullptr;
		const bool remote_owned = !remote || (host != nullptr && execution_hosts::SameConnection(*host, *packet.remote_host) &&
		    host->runner_protocol_version == packet.proxy_protocol && session.remote_runner_protocol_version == packet.proxy_protocol &&
		    session.remote_output_delivery_token == packet.proxy_token && session.process_execution_host_id == host->id &&
		    session.prompt_request_id == 0 && chat.remote_turn_reconnect_pending && chat.remote_prompt_delivery_session_id == packet.delivery_session_id &&
		    chat.remote_prompt_delivery_id == packet.delivery_id && chat.remote_prompt_delivery_payload == packet.delivery_payload &&
		    std::ranges::any_of(chat.remote_pending_requests, [&packet](const AcpRemotePendingRequestState& request)
		    { return request.request_id == packet.request_id && request.turn_serial == packet.turn_serial && request.method == packet.method && !request.response_consumed; }));
		return packet.turn_serial == session.turn_serial && packet.prompt == session.queued_prompt &&
		    session.processing && !session.cancel_requested && packet.data_root == app.data_root &&
		    packet.created_at == chat.created_at && packet.provider_id == session.provider_id &&
		    packet.provider_id == chat.provider_id && packet.session_id == session.session_id &&
		    packet.process_id == AcpProcessHandleLabel(session) && packet.workspace == chat.workspace_directory &&
		    (remote ? remote_owned : chat.execution_host_id == execution_hosts::kLocalHostId) &&
		    packet.native_session_id == chat.native_session_id && packet.active_goal_id == chat.active_goal_id &&
		    packet.goal_revision == chat.goal_command_revision && packet.small_model_mode == chat.small_model_mode &&
		    packet.configuration == PromptDeliveryConfiguration(app, session, chat) &&
		    provider_availability::IsEnabled(app.settings, chat.provider_id, chat.execution_host_id) &&
		    std::ranges::all_of(packet.user_messages, [&chat](const std::pair<std::size_t, std::string>& user)
		    {
			    return user.first < chat.messages.size() && chat.messages[user.first].role == MessageRole::User &&
			        chat.messages[user.first].content == user.second && !chat.messages[user.first].acp_prompt_not_sent;
		    });
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
				if (host == nullptr || host->runner_status != "ready")
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
		const bool accepts = method == uam::acp_methods::kSessionNew ||
		                     method == uam::acp_methods::kSessionLoad ||
		                     method == uam::acp_methods::kSessionResume;
		if (!accepts)
		{
			return true;
		}
		ChatSession* chat = ChatDomainService().FindChatById(app, session.chat_id);
		if (chat != nullptr && chat->execution_host_id != uam::execution_hosts::kLocalHostId)
		{
			const ExecutionHost* execution_host = uam::execution_hosts::Find(
			    app.settings.execution_hosts, chat->execution_host_id);
			if (execution_host == nullptr || execution_host->runner_status != "ready")
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
		    app.settings.mcp_servers, cwd, session.mcp_http_supported,
		    session.mcp_sse_supported, &error);
		if (servers.is_null())
		{
			session.last_error = std::move(error);
			return false;
		}
		if (chat != nullptr && uam::paths::HasGitWorktreeSource(*chat) &&
		    uam::mcp_server_config::WorkspaceKey(chat->workspace_source_directory) != uam::mcp_server_config::WorkspaceKey(cwd))
		{
			nlohmann::json source_servers = uam::mcp_server_config::ResolveForWorkspace(
			    app.settings.mcp_servers, chat->workspace_source_directory,
			    session.mcp_http_supported, session.mcp_sse_supported, &error);
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
	session.pending_local_launch.reset();
	AbandonPendingLocalSteers(session);
	RestorePendingPromptDelivery(session, chat);
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
	session.turn_computer_use_mode = false;
	session.memory_recall_position.reset();
	session.memory_recall_query.clear();
	session.memory_recall_preparation.reset();
	if (session.remote_context_preparation != nullptr) session.remote_context_preparation->cancel.request_stop();
	session.remote_context_preparation.reset();
	session.claude_input_preparation.reset();
	session.goal_turn_kind.clear();
	session.goal_turn_model_id.clear();
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

bool StopAcpProcessForRestart(AppState& app, AcpSessionState& session, const ChatSession& chat)
{
	if (!session.running) return true;
	auto& process_service = PlatformServicesFactory::Instance().process_service;
	const bool remote = chat.execution_host_id != uam::execution_hosts::kLocalHostId;
	if (remote)
	{
		if (ChatSession* persisted = ChatDomainService().FindChatById(app, chat.id);
		    persisted != nullptr)
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
		return false;
	}
	QueueAcpProcessStop(app, session, session.chat_id);
	session.running = false;
	return true;
}

void RestoreRemoteAcpRequests(AcpSessionState& session, const ChatSession& chat)
{
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

bool StartAcpProcessForChat(AppState& app, AcpSessionState& session, ChatSession& chat, std::string* error_out)
{
	const std::shared_ptr<PendingLocalAcpLaunch> prepared = session.pending_local_launch;
	if (prepared)
	{
		const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
		const std::unordered_map<std::string, std::shared_ptr<WorktreeLaunchPreparationTask>>::const_iterator workspace_preparation = app.worktree_launch_preparations.find(chat.id);
		if (prepared->incarnation != chat.metadata_incarnation || prepared->root != app.data_root ||
		    host == nullptr || !execution_hosts::SameConnection(prepared->host, *host) ||
		    prepared->ownership != LocalLaunchOwnership(app, session, chat) ||
		    app.worktree_operation_chat_ids.contains(chat.id) || app.branch_preparations.contains(chat.id) ||
		    (workspace_preparation != app.worktree_launch_preparations.end() &&
		     !(workspace_preparation->second->saved && *workspace_preparation->second->saved)))
		{
			session.pending_local_launch.reset();
			session.last_error = "Local launch ownership changed during configuration preparation.";
			session.lifecycle_state = kAcpLifecycleError;
			if (error_out) *error_out = session.last_error;
			return false;
		}
		if (!prepared->finished.load(std::memory_order_acquire)) return true;
		if (!prepared->success)
		{
			session.pending_local_launch.reset();
			session.last_error = prepared->error;
			session.lifecycle_state = kAcpLifecycleError;
			if (error_out) *error_out = session.last_error;
			return false;
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
	if (!session.managed_agent_run_id.empty() && session.managed_launch_attempted &&
	    !(remote && (session.recovering_remote_turn ||
	                 session.recovering_remote_process)))
	{
		if (error_out != nullptr) *error_out = "Managed agent runs are never relaunched automatically.";
		return false;
	}
	if (execution_host == nullptr ||
	    (remote && execution_host->runner_status != "ready" &&
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
		// The selected launch model must survive transport reset before async ownership is captured.
		session.goal_turn_model_id = goal_turn_model_id;
	}
	if (retrying_undelivered_prompt || recovering_remote_turn || recovering_remote_process)
	{
		if (retrying_undelivered_prompt) session.queued_prompt = pending_prompt;
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
	session.session_id = codex_resume_id.empty() ? acp_resume_id : codex_resume_id;
	session.codex_thread_id = codex_resume_id;
	session.lifecycle_state = kAcpLifecycleStarting;

	std::string startup_error;
	const std::filesystem::path workspace_root = uam::paths::ResolveWorkspaceRootPath(app, chat);
	ChatSession launch_chat = remote ? chat : LocalLaunchMetadata(chat);
	launch_chat.computer_use_globally_enabled = app.settings.computer_use_globally_enabled;
	if (session.active_uam_agent_execution_capability == "uam-central-native-setup") launch_chat.uam_agent_id = session.active_uam_agent_id;
	if (session.active_uam_agent_workspace_access == "read") launch_chat.approval_mode = uam::approval_modes::kPlanApprovalMode;
	if (!session.goal_turn_model_id.empty()) launch_chat.model_id = session.goal_turn_model_id;
	const bool defer_configuration = !remote && !session.model_discovery_only &&
	    !session.recovering_remote_turn && !session.recovering_remote_process &&
	    app.settings.central_provider_configuration.enabled;
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
			        session.active_uam_agent_instructions, &adapter, &startup_error))
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
		}
	}
	nlohmann::json launch_control_servers = nlohmann::json::array();
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
				if (chat.remote_uam_control_channel_id.empty() || !SaveChatQuietly(app, chat))
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
			session.uam_control_relay = std::make_unique<std::jthread>([host, spec](std::stop_token stop) {
				uam::remote::RunLocalMcpRelayFromSpec(stop, host.ssh_alias, host.platform, host.runner_version, host.runner_directory, host.runner_protocol_version, spec);
			});
			server = uam::remote::BuildRemoteMcpServer(channel, host.platform, host.runner_version, host.runner_directory, host.runner_protocol_version);
		}
		launch_control_servers.push_back(std::move(server));
		if (!configuration_settings.central_provider_configuration.enabled)
		{
			configuration_settings.central_provider_configuration = {};
			configuration_settings.central_provider_configuration.enabled = true;
		}
	}
	ProviderConfigurationBundle configuration_bundle;
	if (defer_configuration && !prepared)
	{
		const std::shared_ptr<PendingLocalAcpLaunch> task = std::make_shared<PendingLocalAcpLaunch>();
		task->incarnation = chat.metadata_incarnation;
		task->root = app.data_root;
		task->host = *execution_host;
		task->ownership = LocalLaunchOwnership(app, session, chat);
		const bool central_native = session.active_uam_agent_execution_capability == "uam-central-native-setup";
		const std::string central_instructions = session.active_uam_agent_instructions;
		session.pending_local_launch = task;
		app.local_acp_launch_tasks.push_back(task);
		PendingLocalAcpLaunch* result = task.get();
		try
		{
			task->worker = std::make_unique<std::jthread>([result, provider, launch_chat = std::move(launch_chat), configuration_settings,
			    workspace_root, launch_argv = std::move(launch_argv), launch_environment = std::move(launch_environment),
			    launch_control_servers, central_native, central_instructions](std::stop_token) mutable
			{
				try
				{
					result->argv = BuildAcpLaunchArgv(provider, launch_chat);
					result->argv.insert(result->argv.end(), launch_argv.begin(), launch_argv.end());
					result->environment = uam::provider_runtime_internal::ProviderChildEnvironmentOverrides(provider);
					const uam::provider_setup::Environment runtime_environment = ProviderRuntimeRegistry::ResolveById(provider.id).BuildStructuredLaunchEnvironment(provider, launch_chat);
					result->environment.insert(result->environment.end(), runtime_environment.begin(), runtime_environment.end());
					result->environment.insert(result->environment.end(), launch_environment.begin(), launch_environment.end());
					result->success = PrepareProviderConfiguration(result->root, configuration_settings, launch_chat, result->host,
					    workspace_root, result->bundle, result->error, central_native ? &central_instructions : nullptr) &&
					    (result->bundle.local_directory.empty() || uam::provider_setup::Apply(provider.id, result->bundle.local_directory,
					        workspace_root, result->argv, result->environment, result->error, launch_control_servers, &result->instructions));
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
			if (error_out) *error_out = session.last_error;
			return false;
		}
		launch_argv = prepared->argv;
		launch_environment = prepared->environment;
		configuration_bundle = std::move(prepared->bundle);
		session.provider_protocol_instructions = prepared->instructions;
		session.pending_local_launch.reset();
	}
	if (!prepared && !session.model_discovery_only && !session.recovering_remote_turn && !session.recovering_remote_process &&
	    (!PrepareProviderConfiguration(app.data_root, configuration_settings, launch_chat, *execution_host, workspace_root, configuration_bundle, startup_error,
	                                  session.active_uam_agent_execution_capability == "uam-central-native-setup" ? &session.active_uam_agent_instructions : nullptr) ||
	     (!remote && !configuration_bundle.local_directory.empty() && !uam::provider_setup::Apply(provider.id, configuration_bundle.local_directory, workspace_root, launch_argv, launch_environment, startup_error, launch_control_servers, &session.provider_protocol_instructions))))
	{
		session.lifecycle_state = kAcpLifecycleError;
		session.last_error = startup_error;
		if (error_out) *error_out = startup_error;
		return false;
	}
	if (!session.recovering_remote_turn && !session.recovering_remote_process) session.provider_configuration_directory = configuration_bundle.local_directory;
	for (UamControlCapability& capability : app.uam_control_capabilities)
		if (capability.id == session.uam_control_capability_id) capability.provider_configuration_directory = session.provider_configuration_directory;
	const std::string launch_detail = "host=" + execution_host->id +
	                                  ", cwd=" + AcpWorkingDirectoryString(workspace_root) +
	                                  ", argv=" + JoinAcpArgvForDiagnostics(launch_argv) +
	                                  ", nativeSessionId=" + session.session_id;
	AppendAcpDiagnostic(session, "process_launch", "starting", "", "", false, 0, "", launch_detail);
	if (!session.managed_agent_run_id.empty()) session.managed_launch_attempted = true;
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
			if (!SaveChatQuietly(app, chat))
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
			process_environment = {{uam::remote::kRemoteProcessSpecEnvironment,
				                        uam::remote::BuildProcessProxySpec(
				                            "acp-" + chat.id, workspace_root, launch_argv,
					                            launch_environment, session.recovering_remote_turn || session.recovering_remote_process,
					                            session.remote_output_delivery_token,
					                            (session.recovering_remote_turn || session.recovering_remote_process)
					                                ? chat.remote_delivered_stdout_cursor : 0,
					                            (session.recovering_remote_turn || session.recovering_remote_process)
					                                ? chat.remote_delivered_stderr_cursor : 0, {}, {},
					                            configuration_bundle.local_directory.empty() ? "" : provider.id,
					                            uam::paths::Utf8PathString(configuration_bundle.local_directory), launch_control_servers)}};
		}
	}
	if (remote && startup_error.empty())
	{
		chat.remote_process_exists = true;
		if (!SaveChatQuietly(app, chat))
			startup_error = "Remote process ownership could not be saved before launch.";
	}
	if (!startup_error.empty() ||
	    !PlatformServicesFactory::Instance().process_service.StartStdioProcess(
	        session, process_working_directory, process_argv, &startup_error,
	        process_environment))
	{
		if (remote && !recovering_remote_turn && !recovering_remote_process)
		{
			chat.remote_process_exists = false;
			(void)SaveChatQuietly(app, chat);
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

	session.running = true;
	session.reconnect_pending = false;
	session.reconnect_not_before_time_s = 0.0;
	session.last_process_id = AcpProcessHandleLabel(session);
	session.last_runtime_activity_time_s = GetAppTimeSeconds();
	AppendAcpDiagnostic(session, "process_launch", "started", "", "", false, 0, "", launch_detail);
	if (session.recovering_remote_turn || session.recovering_remote_process)
	{
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
}

bool SendSessionSetupIfReady(AppState& app, AcpSessionState& session, ChatSession& chat)
{
	if (session.remote_source_exit_reported) return false;
	if (!session.running || !session.initialized || session.session_ready || session.session_setup_request_id != 0)
	{
		return false;
	}

	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(session.provider_id);
	if (session.model_discovery_only && std::strcmp(runtime.AcpProtocolKind(), "codex-app-server") == 0)
	{
		return false;
	}
	const std::filesystem::path workspace_root = uam::paths::ResolveWorkspaceRootPath(app, chat);
	const std::string cwd = AcpWorkingDirectoryString(workspace_root);

	const std::string raw_resume_id = session.goal_internal_session ? std::string{} : uam::strings::Trim(chat.native_session_id);
	const std::string resume_id = session.goal_internal_session ? std::string{} : runtime.OnAcpValidateResumeId(chat);
	if (!raw_resume_id.empty() && resume_id.empty())
	{
		AppendInvalidResumeDiagnostic(session, raw_resume_id);
		if (!ClearSavedAcpResumeId(app, session, chat)) return false;
	}

	const std::string resolved_resume_id = session.goal_internal_session ? std::string{} : ResolvedAcpResumeIdForChat(app, chat);

	ChatSession resume_chat = chat;
	resume_chat.computer_use_globally_enabled = app.settings.computer_use_globally_enabled;
	resume_chat.native_session_id = resume_id.empty() ? resolved_resume_id : resume_id;
	if (session.provider_id == uam::provider_ids::kCodexCli)
	{
		session.codex_approvals_reviewer.clear();
		if (session.goal_review_turn || session.active_uam_agent_workspace_access == "read") resume_chat.approval_mode = uam::approval_modes::kPlanApprovalMode;
	}
	if (!session.goal_turn_model_id.empty()) resume_chat.model_id = session.goal_turn_model_id;

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
		msg = runtime.OnAcpBuildSetupRequest(id, resume_chat, cwd, session.load_session_supported, method);
	}
	if (session.provider_id == uam::provider_ids::kCodexCli)
		(void)uam::codex::ApplySetupInstructions(msg, session.provider_protocol_instructions);
	if (!AddWorkspaceMcpServers(app, session, method, cwd, msg))
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

	const std::string desired_mode = session.goal_review_turn
	                                   ? std::string(uam::approval_modes::kPlanApprovalMode)
	                                   : uam::approval_modes::EffectiveProviderMode(chat.approval_mode, chat.command_safety_tier);
	const bool must_leave_hidden_autopilot = session.current_mode_id == uam::approval_modes::kAcpAutopilotMode;
	if ((uam::strings::IsBlank(chat.approval_mode) && !must_leave_hidden_autopilot) || session.current_mode_id == desired_mode)
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

bool SendStartupModelIfNeeded(AcpSessionState& session, const ChatSession& chat)
{
	if (!ProviderRuntimeRegistry::ResolveById(session.provider_id).IsGenericAcpSession() || !session.running || !session.session_ready || session.startup_model_request_id != 0 || session.reasoning_change_request_id != 0 || session.config_option_change_request_id != 0 || session.model_change_request_id != 0 || session.awaiting_model_config_options || session.session_id.empty())
	{
		return false;
	}

	const std::string model_id = uam::strings::Trim(
	    uam::strings::NonEmptyOrFallback(session.goal_turn_model_id, chat.model_id));
	if (model_id.empty() || session.current_model_id == model_id)
	{
		ClearAcpStartupModelRequest(session);
		return false;
	}
	const auto model_option = std::ranges::find_if(session.available_config_options, [](const AcpConfigOptionState& option) {
		return option.category == "model" || option.id == "model";
	});
	if (model_option != session.available_config_options.end() &&
	    std::ranges::none_of(model_option->choices, [&](const AcpConfigOptionChoiceState& choice) { return choice.value == model_id; }))
	{
		FailAcpTurnOrSession(session, nullptr, "The provider no longer offers the saved model '" + model_id + "'. Refresh models in Settings or restart UAM, then select an available model before retrying.");
		return true;
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

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
static ClaudeInputIdentity ClaudeInputSnapshot(const AppState& app, const AcpSessionState& session, const ChatSession& chat)
{
	const int first = session.turn_first_user_message_index >= 0 ? session.turn_first_user_message_index : session.turn_user_message_index;
	return {chat.id, session.provider_id, chat.provider_id, session.session_id,
	    session.process_execution_host_id, chat.execution_host_id, paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, chat)), PreparedPromptText(session),
	    chat.model_id, session.goal_turn_model_id, chat.approval_mode, chat.computer_use_backend, chat.computer_use_enabled,
	    session.turn_serial, first, session.turn_user_message_index, uam::claude_input::TurnImages(chat, first, session.turn_user_message_index)};
}
#endif

/// <summary>Restores only the staged user bodies still owned by this transcript.</summary>
void RestorePendingPromptDelivery(AcpSessionState& session, ChatSession& chat)
{
	const std::shared_ptr<PendingAcpPromptDelivery> packet = std::move(session.pending_prompt_delivery);
	if (!packet) return;
	if (packet->remote_host && packet->created_at == chat.created_at &&
	    packet->delivery_id == chat.remote_prompt_delivery_id && packet->delivery_payload == chat.remote_prompt_delivery_payload)
	{
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
	for (const std::size_t index : unsent_messages)
		packet->user_messages.emplace_back(index, chat.messages[index].content);
	session.pending_prompt_delivery = packet;
	packet->created_at = chat.created_at;
	packet->process_id = AcpProcessHandleLabel(session);
	packet->active_goal_id = chat.active_goal_id;
	packet->goal_revision = chat.goal_command_revision;
	packet->small_model_mode = chat.small_model_mode;
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
static bool DeliverPreparedPrompt(AppState& app, AcpSessionState& session, ChatSession& chat,
                                  const nlohmann::json& msg, const std::string& method, int id,
                                  const std::vector<std::size_t>& unsent_messages, bool marker_committed = false)
{
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
		AcpRemotePendingRequestState pending;
		pending.request_id = id;
		pending.method = method;
		pending.turn_serial = session.turn_serial;
		chat.remote_pending_requests.push_back(std::move(pending));
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
			chat.remote_prompt_delivery_payload = msg.dump();
			chat.remote_prompt_delivery_payload.push_back('\n');
		}
		if (CanDeferRemotePromptAdmission(app, session, chat))
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
			chat.remote_pending_requests.pop_back();
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
	if (remote && session.remote_runner_protocol_version >= 3)
	{
		prompt_written = WriteCommittedRemotePrompt(app, session, chat);
	}
	else
	{
		prompt_written = WriteAcpMessage(session, msg);
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
		CompletePromptTurn(session, kAcpLifecycleError);
		InvalidateAcpTransport(app, session, chat, session.last_error);
		return true;
	}

	// The delivery-intent save already persists local state unless delivery changes metadata.
	bool local_delivery_save_needed = unsent_messages.empty();
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

bool CanDeferRemotePromptAdmission(const AppState& app, const AcpSessionState& session, const ChatSession& chat)
{
	const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
	return chat.execution_host_id != execution_hosts::kLocalHostId && !chat.small_model_mode &&
	    app.claude_quota_dispatch_id.empty() && session.remote_runner_protocol_version >= 3 &&
	    session.running && session.session_ready && host != nullptr && host->runner_protocol_version == session.remote_runner_protocol_version &&
	    session.process_execution_host_id == host->id && !session.remote_output_delivery_token.empty();
}

bool SendQueuedPromptIfReady(AppState& app, AcpSessionState& session, ChatSession& chat)
{
	if (session.turn_computer_use_mode && (!app.settings.computer_use_globally_enabled ||
	    !chat.computer_use_enabled || !uam::computer_use::AvailableForChat(chat)))
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
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	if (session.claude_input_preparation != nullptr)
	{
		const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
		if (session.remote_source_exit_reported || session.queued_prompt.empty() || session.protocol_kind != "claude-code-stream-json" ||
		    chat.execution_host_id != execution_hosts::kLocalHostId || host == nullptr ||
		    session.queued_prompt.size() > platform::kAsyncInputMaxQueuedBytes ||
		    session.claude_input_preparation->state->identity != ClaudeInputSnapshot(app, session, chat) ||
		    !execution_hosts::SameConnection(session.claude_input_preparation->state->host, *host))
			session.claude_input_preparation.reset();
	}
#endif
	if (session.remote_source_exit_reported) return false;
	// A command made while startup was pending may invalidate an unsent goal turn.
	if (!session.goal_turn_kind.empty() && session.prompt_request_id == 0 && !session.queued_prompt.empty())
	{
		const ChatSession* owner = ChatDomainService().FindChatById(app, uam::strings::NonEmptyOrFallback(chat.goal_owner_chat_id, chat.id));
		if (owner == nullptr || owner->goal_command_revision != session.goal_command_revision)
		{
			RestorePendingPromptDelivery(session, chat);
			ScheduleChatSave(app, chat, 0.0);
			session.queued_prompt.clear();
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
		return DeliverPreparedPrompt(app, session, chat, packet->message, packet->method,
		                             packet->request_id, packet->unsent_messages, packet->remote_host.has_value());
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
	const std::string prompt = PreparedPromptText(session);
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
	session.lifecycle_state = kAcpLifecycleProcessing;

	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(session.provider_id);
	if (chat.remote_pending_requests.size() >= 64)
	{
		session.last_error = "Wait for pending remote requests to be confirmed before sending another prompt.";
		return true;
	}
	std::string method;
	nlohmann::json msg;
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
	const int first = session.turn_first_user_message_index >= 0 ? session.turn_first_user_message_index : session.turn_user_message_index;
	const std::vector<MessageAttachment> images = session.protocol_kind == "claude-code-stream-json" && chat.execution_host_id == execution_hosts::kLocalHostId
	    ? uam::claude_input::TurnImages(chat, first, session.turn_user_message_index) : std::vector<MessageAttachment>{};
	if (session.protocol_kind == "claude-code-stream-json" && chat.execution_host_id == execution_hosts::kLocalHostId && !images.empty())
	{
		const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
		if (host == nullptr) { FailAcpTurnOrSession(session, &chat, "The execution host no longer exists."); return true; }
		if (prompt.size() > platform::kAsyncInputMaxQueuedBytes)
		{ FailAcpTurnOrSession(session, &chat, "The prompt exceeds UAM's 4 MiB input limit."); return true; }
		const ClaudeInputIdentity identity = ClaudeInputSnapshot(app, session, chat);
		if (session.claude_input_preparation != nullptr &&
		    (session.claude_input_preparation->state->identity != identity ||
		     !execution_hosts::SameConnection(session.claude_input_preparation->state->host, *host)))
			session.claude_input_preparation.reset();
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
					state->message = uam::claude_input::Build(state->identity.prompt,
					    paths::PathFromUtf8(state->identity.workspace), state->identity.images,
					    state->error, true, state->cancel.get_token());
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
		msg = state->message;
	}
	else
#endif
	{
		session.claude_input_preparation.reset();
		// Prompt hooks need settings only; current-turn Claude images are prepared above.
		ChatSession prompt_chat;
		prompt_chat.workspace_directory = paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, chat));
		prompt_chat.execution_host_id = chat.execution_host_id;
		prompt_chat.model_id = session.goal_turn_model_id.empty() ? chat.model_id : session.goal_turn_model_id;
		prompt_chat.reasoning_effort = chat.reasoning_effort;
		prompt_chat.service_tier = chat.service_tier;
		prompt_chat.service_tier_explicit = chat.service_tier_explicit;
		prompt_chat.approval_mode = chat.approval_mode;
		prompt_chat.command_safety_tier = chat.command_safety_tier;
		msg = runtime.OnAcpBuildPrompt(session, session.next_request_id, context + prompt, prompt_chat, method);
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
	for (int index = first_user_message_index; index >= 0 && index <= last_user_message_index &&
	     index < static_cast<int>(chat.messages.size()); ++index)
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
	if (!CanDeferRemotePromptAdmission(app, session, chat) && !unsent_messages.empty() && !SaveChatQuietly(app, chat))
	{
		for (const std::size_t index : unsent_messages) chat.messages[index].acp_prompt_not_sent = true;
		session.last_error = "Prompt is waiting for its delivery intent to be saved.";
		return true;
	}
	return DeliverPreparedPrompt(app, session, chat, msg, method, id, unsent_messages);
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

bool SaveChatQuietly(AppState& app, const ChatSession& chat)
{
	// Discovery sessions are temporary and must not reappear as chats on the next launch.
	if (std::any_of(app.model_discovery_chats.begin(), app.model_discovery_chats.end(),
	        [&chat](const ChatSession& discovery) { return discovery.id == chat.id; })) return true;
	ChatSession* persisted = ChatDomainService().FindChatById(app, chat.id);
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
			persisted->remote_turn_user_message_index = uam::AcpSessionHasActiveTurn(*active)
			    ? active->turn_user_message_index : -1;
			persisted->remote_active_turn_id = ProviderRuntimeRegistry::ResolveById(active->provider_id).AcpTurnIdentity(*active);
		}
	}
	if (persisted != nullptr && persisted->remote_turn_reconnect_pending)
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
	const bool saved = ChatRepository::SaveChat(app.data_root, persisted == nullptr ? chat : *persisted);
	if (saved)
	{
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
	session.pending_local_launch.reset();
	AbandonPendingLocalSteers(session);
	if (session.processing) session.interaction_at = uam::time::InteractionTimestampNow();
	// A completion notification can arrive before the prompt request's reply.
	session.pending_request_methods.erase(session.prompt_request_id);
	if (session.processing && !session.manual_compaction_pending && session.turn_serial > 0)
	{
		session.last_settled_turn_serial = session.turn_serial;
		session.last_turn_outcome = lifecycle_state == kAcpLifecycleReady ? "completed" : "failed";
		session.last_turn_error = lifecycle_state == kAcpLifecycleReady ? std::string{} : session.last_error;
	}
	session.prompt_request_id = 0;
	session.pending_prompt_delivery.reset();
	session.defer_prompt_save = false;
	session.processing = false;
	session.manual_compaction_pending = false;
	session.context_compaction_in_progress = false;
	session.recovering_remote_turn = false;
	session.cancel_requested = false;
	session.cancel_requested_time_s = 0.0;
	session.inactivity_timeout_pending = false;
	session.queued_prompt.clear();
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
	if (async_receipt && session.goal_queue_admission == AcpSessionState::GoalQueueAdmission::Held &&
	    session.goal_queue_admission_turn_serial == session.turn_serial && session.queued_prompt == prompt)
	{
		// Only a newly admitted explicit control can retry its retained exact queued intent.
		session.queued_prompt.clear();
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
	session.last_error = message;
	if (chat != nullptr)
	{
		RestorePendingPromptDelivery(session, *chat);
		// A failure before delivery retires the staged copy; the visible user message
		// remains available for retry. Unconfirmed deliveries and later queued prompts stay intact.
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
