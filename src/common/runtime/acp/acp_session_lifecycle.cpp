#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_session_runtime.h"

#include "app/agent_definition_service.h"
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
#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/runtime/acp/acp_protocol_methods.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/utils/string_utils.h"
#include "remote/runner_proxy.h"

#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace uam::acp_detail
{

namespace
{
	bool AddWorkspaceMcpServers(AppState& app, AcpSessionState& session,
	                            std::string_view method, const std::string& cwd,
	                            nlohmann::json& request)
	{
		if (session.model_discovery_only) return true;
		const bool accepts = method == uam::acp_methods::kSessionNew ||
		                     method == uam::acp_methods::kSessionLoad ||
		                     method == uam::acp_methods::kSessionResume;
		if (!accepts)
		{
			return true;
		}
		const ChatSession* chat = ChatDomainService().FindChatById(app, session.chat_id);
		if (chat != nullptr && chat->execution_host_id != uam::execution_hosts::kLocalHostId)
		{
			if (!chat->uam_control_enabled) return true;
			nlohmann::json local_request = {
			    {"params", {{"mcpServers", nlohmann::json::array()}}}};
			std::string error;
			if (!UamControlService::AppendSessionMcpServer(
			        app, session, *chat, method, local_request, &error))
			{
				session.last_error = std::move(error);
				return false;
			}
			const nlohmann::json& local_server = local_request["params"]["mcpServers"].back();
			std::vector<std::string> local_argv = {local_server.value("command", "")};
			for (const nlohmann::json& argument : local_server.value("args", nlohmann::json::array()))
				if (argument.is_string()) local_argv.push_back(argument.get<std::string>());
			std::vector<std::pair<std::string, std::string>> local_environment;
			for (const nlohmann::json& entry : local_server.value("env", nlohmann::json::array()))
				if (entry.is_object() && entry.contains("name") && entry["name"].is_string() &&
				    entry.contains("value") && entry["value"].is_string())
					local_environment.emplace_back(entry["name"].get<std::string>(),
					                               entry["value"].get<std::string>());
			const std::string channel_id = "control-" + session.uam_control_capability_id;
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
			nlohmann::json& request_servers = request["params"]["mcpServers"];
			if (!request_servers.is_array()) request_servers = nlohmann::json::array();
			request_servers.push_back({
			    {"name", "uam-control"}, {"command", "/bin/sh"},
			    {"args", nlohmann::json::array({
			                 "-c",
			                 "exec \"$HOME/.local/share/uam/runner/current/uam-runner\" mcp --channel \"$1\" --socket \"$HOME/.local/share/uam/runner/uam.sock\"",
			                 "uam-control", channel_id})},
			    {"env", nlohmann::json::array()},
			});
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

void ResetAcpRuntimeState(AcpSessionState& session)
{
	session.initialized = false;
	session.session_ready = false;
	session.load_session_supported = false;
	session.resume_session_supported = false;
	session.mcp_http_supported = false;
	session.mcp_sse_supported = false;
	session.processing = false;
	session.cancel_requested = false;
	session.cancel_requested_time_s = 0.0;
	session.inactivity_timeout_pending = false;
	session.turn_checkpoint_eligible = false;
	session.turn_checkpoint_preflight_pending = false;
	session.turn_checkpoint_commit_pending = false;
	session.next_request_id = 1;
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
	session.turn_user_message_index = -1;
	session.turn_assistant_message_index = -1;
	session.turn_serial = 0;
	session.queued_prompt.clear();
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
	session.last_error.clear();
	session.has_last_exit_code = false;
	session.last_exit_code = 0;
	session.last_process_id.clear();
	session.assistant_replay_prefixes.clear();
	session.load_history_replay_updates.clear();
	session.agent_name.clear();
	session.agent_title.clear();
	session.agent_version.clear();
	session.available_commands.clear();
	session.available_config_options.clear();
	session.pending_request_methods.clear();
	ResetAcpTurnStreamState(session);
	session.codex_turn_id.clear();
	ResetAcpPendingInteractionState(session);
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
	app.acp_sessions.push_back(std::move(session));
	return *app.acp_sessions.back();
}

bool FailAcpSessionSetupWrite(AppState& app, AcpSessionState& session, ChatSession& chat, const std::string& fallback_message)
{
	UamControlService::RevokeForSession(app, session);
	session.session_setup_request_id = 0;
	FailAcpTurnOrSession(session, uam::strings::NonEmptyOrFallback(session.last_error, fallback_message));
	MarkAcpChatUnseenIfBackground(app, chat);
	return false;
}

bool StartAcpProcessForChat(AppState& app, AcpSessionState& session, const ChatSession& chat, std::string* error_out)
{
	if (session.running)
	{
		return true;
	}
	if (!session.managed_agent_run_id.empty() && session.managed_launch_attempted)
	{
		if (error_out != nullptr) *error_out = "Managed agent runs are never relaunched automatically.";
		return false;
	}
	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
	const ExecutionHost* execution_host =
	    uam::execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
	const bool remote = execution_host != nullptr && execution_host->id != uam::execution_hosts::kLocalHostId;
	if (execution_host == nullptr || (remote && execution_host->runner_status != "ready"))
	{
		const std::string startup_error = remote ? "The selected remote runner is not ready. Recheck it in Settings."
		                                         : "The selected execution host no longer exists.";
		session.lifecycle_state = kAcpLifecycleError;
		session.last_error = startup_error;
		if (error_out != nullptr) *error_out = startup_error;
		return false;
	}
	const bool copilot = uam::provider_ids::IsCliProviderAliasOf(provider.id, uam::provider_ids::kCopilotCli);
	const bool opencode = uam::provider_ids::IsCliProviderAliasOf(provider.id, uam::provider_ids::kOpenCodeCli);
	if (!remote && (copilot || opencode))
	{
		ProviderCliCompatibilityService().Poll(app);
		const std::string compatibility_error = copilot ? CopilotLaunchBlockReason(app) : OpenCodeLaunchBlockReason(app);
		if (!compatibility_error.empty())
		{
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
	const std::string pending_prompt = session.queued_prompt;
	const int turn_user_message_index = session.turn_user_message_index;
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
	ResetAcpRuntimeState(session);
	if (retrying_undelivered_prompt)
	{
		session.queued_prompt = pending_prompt;
		session.processing = true;
		session.turn_user_message_index = turn_user_message_index;
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
	session.provider_id = provider.id;
	session.protocol_kind = ProviderStructuredProtocolOrDefault(provider);
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(session.provider_id);
	const std::string codex_resume_id = !session.goal_internal_session && std::strcmp(runtime.AcpProtocolKind(), "codex-app-server") == 0 ? ValidCodexResumeId(chat) : std::string{};
	const std::string acp_resume_id = !session.goal_internal_session && codex_resume_id.empty() ? ResolvedAcpResumeIdForChat(app, chat) : std::string{};
	session.session_id = codex_resume_id.empty() ? acp_resume_id : codex_resume_id;
	session.codex_thread_id = codex_resume_id;
	session.lifecycle_state = kAcpLifecycleStarting;

	std::string startup_error;
	const std::filesystem::path workspace_root = uam::paths::ResolveWorkspaceRootPath(app, chat);
	ChatSession launch_chat = chat;
	if (!session.goal_turn_model_id.empty()) launch_chat.model_id = session.goal_turn_model_id;
	std::vector<std::string> launch_argv = BuildAcpLaunchArgv(provider, launch_chat);
	std::vector<std::pair<std::string, std::string>> launch_environment =
	    uam::provider_runtime_internal::ProviderChildEnvironmentOverrides(provider);
	const std::vector<std::pair<std::string, std::string>> runtime_environment =
	    runtime.BuildStructuredLaunchEnvironment(provider, launch_chat);
	launch_environment.insert(launch_environment.end(), runtime_environment.begin(), runtime_environment.end());
	session.active_uam_agent_adapter_directory.clear();
	if (!session.model_discovery_only && !session.active_uam_agent_instructions.empty())
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
	const std::string launch_detail = "host=" + execution_host->id +
	                                  ", cwd=" + AcpWorkingDirectoryString(workspace_root) +
	                                  ", argv=" + JoinAcpArgvForDiagnostics(launch_argv) +
	                                  ", nativeSessionId=" + session.session_id;
	AppendAcpDiagnostic(session, "process_launch", "starting", "", "", false, 0, "", launch_detail);
	if (!session.managed_agent_run_id.empty()) session.managed_launch_attempted = true;
	std::filesystem::path process_working_directory = workspace_root;
	std::vector<std::string> process_argv = launch_argv;
	std::vector<std::pair<std::string, std::string>> process_environment = launch_environment;
	if (remote)
	{
		const std::filesystem::path runner = uam::remote::PackagedRunnerPath();
		std::error_code runner_error;
		if (!std::filesystem::is_regular_file(runner, runner_error) || runner_error)
		{
			startup_error = "The packaged UAM remote runner is missing.";
		}
		else
		{
			process_working_directory = runner.parent_path();
			process_argv = {runner.string(), "proxy", "--alias", execution_host->ssh_alias};
			process_environment = {{uam::remote::kRemoteProcessSpecEnvironment,
			                        uam::remote::BuildProcessProxySpec(
			                            "acp-" + chat.id, workspace_root, launch_argv,
			                            launch_environment)}};
		}
	}
	if (!startup_error.empty() ||
	    !PlatformServicesFactory::Instance().process_service.StartStdioProcess(
	        session, process_working_directory, process_argv, &startup_error,
	        process_environment))
	{
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
	session.reconnect_attempts = 0;
	session.reconnect_not_before_time_s = 0.0;
	session.last_process_id = AcpProcessHandleLabel(session);
	session.last_runtime_activity_time_s = GetAppTimeSeconds();
	AppendAcpDiagnostic(session, "process_launch", "started", "", "", false, 0, "", launch_detail);
	if (!SendInitialize(session, error_out))
	{
		PlatformServicesFactory::Instance().process_service.StopStdioProcess(session, true);
		session.running = false;
		return false;
	}

	return true;
}

bool SendSessionSetupIfReady(AppState& app, AcpSessionState& session, ChatSession& chat)
{
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
	const std::string resolved_resume_id = session.goal_internal_session ? std::string{} : ResolvedAcpResumeIdForChat(app, chat);

	const std::string raw_resume_id = session.goal_internal_session ? std::string{} : uam::strings::Trim(chat.native_session_id);
	const std::string resume_id = session.goal_internal_session ? std::string{} : runtime.OnAcpValidateResumeId(chat);
	if (!raw_resume_id.empty() && resume_id.empty())
	{
		AppendInvalidResumeDiagnostic(session, raw_resume_id);
		chat.native_session_id.clear();
		SaveChatQuietly(app, chat);
	}

	ChatSession resume_chat = chat;
	resume_chat.native_session_id = resume_id.empty() ? resolved_resume_id : resume_id;
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
	session.lifecycle_state = kAcpLifecycleStarting;
	session.session_id = resume_chat.native_session_id;
	session.codex_thread_id = resume_chat.native_session_id;

	const bool written = WriteAcpMessage(session, msg);
	if (!written)
	{
		return FailAcpSessionSetupWrite(app, session, chat, "Failed to " + std::string(method.find("load") != std::string::npos || method.find("resume") != std::string::npos ? "load" : "create") + " session.");
	}
	return written;
}

bool RetrySessionNewAfterInvalidLoad(AppState& app, AcpSessionState& session, ChatSession& chat, const AcpInvalidLoadRetryDetails& details)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(session.provider_id);
	const bool is_session_load = details.failure.method == uam::acp_methods::kSessionLoad;
	const bool copilot_session_not_found = std::strcmp(runtime.RuntimeId(), uam::provider_ids::kCopilotCli) == 0 && details.failure.has_code && details.failure.code == -32002;
	const bool gemini_invalid_session = !runtime.IsGenericAcpSession() && std::strcmp(runtime.AcpProtocolKind(), "codex-app-server") != 0 && GeminiErrorLooksLikeInvalidSessionId(details.failure.message, details.error_data);
	if (!is_session_load || session.acp_resume_fallback_attempted || (!copilot_session_not_found && !gemini_invalid_session))
	{
		return false;
	}

	session.acp_resume_fallback_attempted = true;
	session.session_setup_request_id = 0;
	session.session_id.clear();
	chat.native_session_id.clear();
	SaveChatQuietly(app, chat);
	const AcpFailureDetails& failure = details.failure;
	const std::string retry_message = std::string(RuntimeDisplayName(session)) + " rejected the stored session id. Starting a new session instead.";
	AppendAcpDiagnostic(session, "response", "invalid_resume_id_retry_new", failure.method, failure.request_id, failure.has_code, failure.code, retry_message, details.detail_text);

	const std::filesystem::path workspace_root = uam::paths::ResolveWorkspaceRootPath(app, chat);
	const std::string cwd = AcpWorkingDirectoryString(workspace_root);
	const int retry_id = NextAcpRequestId(session, uam::acp_methods::kSessionNew);
	session.session_setup_request_id = retry_id;
	session.ignore_session_updates_until_ready = false;
	session.lifecycle_state = kAcpLifecycleStarting;

	nlohmann::json retry_request = BuildNewSessionRequest(retry_id, cwd, &chat);
	if (!AddWorkspaceMcpServers(app, session, uam::acp_methods::kSessionNew, cwd,
	                            retry_request))
	{
		session.pending_request_methods.erase(retry_id);
		session.session_setup_request_id = 0;
		FailAcpTurnOrSession(session, session.last_error);
		SaveChatQuietly(app, chat);
		MarkAcpChatUnseenIfBackground(app, chat);
		return true;
	}
	if (!WriteAcpMessage(session, retry_request))
	{
		session.pending_request_methods.erase(retry_id);
		session.session_setup_request_id = 0;
		(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
		FailAcpTurnOrSession(session, uam::strings::NonEmptyOrFallback(session.last_error, details.formatted_error));
		SaveChatQuietly(app, chat);
		MarkAcpChatUnseenIfBackground(app, chat);
	}
	return true;
}

bool SendStartupModeIfNeeded(AcpSessionState& session, const ChatSession& chat)
{
	if (session.active_uam_agent_execution_capability == "opencode-native-agent-config")
	{
		return false;
	}
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(session.provider_id);
	const char* protocol = runtime.AcpProtocolKind();
	if (!session.running || !session.session_ready || session.mode_change_request_id != 0 || session.session_id.empty() || std::strcmp(protocol, "codex-app-server") == 0 || std::strcmp(protocol, "claude-code-stream-json") == 0)
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
		FailAcpTurnOrSession(session, uam::strings::NonEmptyOrFallback(session.last_error, "Failed to set " + std::string(RuntimeDisplayName(session)) + " mode."));
		return false;
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

	const int id = NextAcpRequestId(session, uam::acp_methods::kSessionSetModel);
	session.startup_model_request_id = id;
	session.pending_startup_model_id = model_id;
	session.model_change_request_id = id;
	session.model_change_previous_id = session.current_model_id;
	session.model_change_previous_chat_id = session.current_model_id;
	session.model_change_requested_id = model_id;
	session.awaiting_model_config_options =
	    uam::provider_ids::IsCliProviderAliasOf(session.provider_id, uam::provider_ids::kCopilotCli);
	if (!WriteAcpMessage(session, BuildSetModelRequest(id, session.session_id, model_id)))
	{
		session.pending_request_methods.erase(id);
		ClearAcpStartupModelRequest(session);
		ClearAcpModelChangeRequest(session);
		session.awaiting_model_config_options = false;
		FailAcpTurnOrSession(session, uam::strings::NonEmptyOrFallback(session.last_error, "Failed to set " + std::string(RuntimeDisplayName(session)) + " model."));
		return false;
	}

	session.current_model_id = model_id;
	return true;
}

bool SendQueuedPromptIfReady(AcpSessionState& session, const ChatSession& chat)
{
	if (session.turn_checkpoint_preflight_pending)
	{
		return false;
	}
	if (session.startup_model_request_id != 0 || session.reasoning_change_request_id != 0 || session.config_option_change_request_id != 0 || session.mode_change_request_id != 0 || session.model_change_request_id != 0 || session.awaiting_model_config_options)
	{
		return false;
	}
	if (SendStartupModeIfNeeded(session, chat))
	{
		return false;
	}
	if (SendStartupModelIfNeeded(session, chat))
	{
		return false;
	}
	if (!AcpSessionCanSendQueuedPrompt(session))
	{
		return false;
	}

	const std::string prompt = session.queued_prompt;
	session.lifecycle_state = kAcpLifecycleProcessing;

	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(session.provider_id);
	const int id = session.next_request_id++;
	std::string method;
	ChatSession prompt_chat = chat;
	if (!session.goal_turn_model_id.empty()) prompt_chat.model_id = session.goal_turn_model_id;
	nlohmann::json msg = runtime.OnAcpBuildPrompt(session, id, prompt, prompt_chat, method);

	if (msg.is_null() || msg.empty())
	{
		CompletePromptTurn(session, kAcpLifecycleError);
		return true;
	}

	if (!method.empty())
	{
		session.pending_request_methods[id] = method;
		session.prompt_request_id = id;
	}

	if (!WriteAcpMessage(session, msg))
	{
		session.pending_request_methods.erase(id);
		session.prompt_request_id = 0;
		CompletePromptTurn(session, kAcpLifecycleError);
		return true;
	}

	session.queued_prompt.clear();
	return true;
}

void SaveChatQuietly(AppState& app, const ChatSession& chat)
{
	(void)ChatRepository::SaveChat(app.data_root, chat);
}

void ScheduleChatSave(AppState& app, const ChatSession& chat, double delay_seconds)
{
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

	chat.native_session_id = normalized_session_id;
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
	if (session.processing && session.turn_serial > 0)
	{
		session.last_settled_turn_serial = session.turn_serial;
		session.last_turn_outcome = lifecycle_state == kAcpLifecycleReady ? "completed" : "failed";
		session.last_turn_error = lifecycle_state == kAcpLifecycleReady ? std::string{} : session.last_error;
	}
	session.prompt_request_id = 0;
	session.processing = false;
	session.cancel_requested = false;
	session.cancel_requested_time_s = 0.0;
	session.inactivity_timeout_pending = false;
	session.queued_prompt.clear();
	session.current_assistant_message_index = -1;
	session.codex_turn_id.clear();
	session.load_history_replay_updates.clear();
	session.pending_assistant_thoughts.clear();
	ResetAcpPendingInteractionState(session);
	session.lifecycle_state.assign(lifecycle_state);
}

bool QueueGoalInternalPrompt(AppState& app, AcpSessionState& session, ChatSession& chat,
                             const std::string& prompt, bool review_turn,
                             const std::string& model_id, bool fresh_session)
{
	const bool fresh_context_busy = session.processing || session.waiting_for_permission ||
	                                session.waiting_for_user_input || session.cancel_requested ||
	                                session.prompt_request_id != 0 || session.cancel_request_id != 0 ||
	                                !session.queued_prompt.empty() || !session.queued_user_prompts.empty();
	if (prompt.empty() || (fresh_session ? fresh_context_busy : !CanQueueGoalInternalPrompt(session)))
	{
		return false;
	}
	if (fresh_session && !StopAcpSession(app, chat.id)) return false;
	session.queued_prompt = prompt;
	session.goal_turn_kind = review_turn ? std::string(kGoalTurnKindReview) : std::string(kGoalTurnKindWorkerContinuation);
	session.goal_turn_model_id = uam::strings::Trim(model_id);
	session.goal_internal_session = fresh_session;
	session.goal_review_turn = review_turn;
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
	session.turn_user_message_index = -1;
	session.turn_assistant_message_index = -1;
	session.turn_serial += 1;
	ResetAcpTurnStreamState(session);
	ResetAcpPendingInteractionState(session);
	session.turn_started_time_s = GetAppTimeSeconds();
	session.last_runtime_activity_time_s = session.turn_started_time_s;
	session.last_error.clear();
	session.lifecycle_state = kAcpLifecycleProcessing;
	if (fresh_session && !StartAcpProcessForChat(app, session, chat))
	{
		FailAcpTurnOrSession(session, uam::strings::NonEmptyOrFallback(session.last_error, "Failed to start the private goal context."));
		return false;
	}
	(void)ScheduleTurnCheckpointPreflight(app, session, chat);
	// Queued is success; if the session cannot send yet, the poll loop
	// delivers the prompt once the session is ready.
	(void)SendQueuedPromptIfReady(session, chat);
	return true;
}

void ClearGoalReviewState(AcpSessionState& session)
{
	session.goal_review_turn = false;
	session.goal_review_scheduled = false;
	session.goal_review_goal_id.clear();
	session.goal_review_user_prompt.clear();
	session.goal_review_assistant_text.clear();
}

void FailAcpTurnOrSession(AcpSessionState& session, const std::string& message)
{
	session.last_error = message;
	if (session.processing || session.waiting_for_permission || session.prompt_request_id != 0 || !session.queued_prompt.empty())
	{
		CompletePromptTurn(session, kAcpLifecycleError);
		return;
	}

	session.lifecycle_state = kAcpLifecycleError;
}

void MarkAcpChatUnseenIfBackground(AppState& app, const ChatSession& chat)
{
	if (ChatDomainService().SelectedChatId(app) == chat.id)
	{
		return;
	}

	app.chats_with_unseen_updates.insert(chat.id);
}

} // namespace uam::acp_detail
