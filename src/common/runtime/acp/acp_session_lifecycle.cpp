#include "common/runtime/acp/acp_session_internal.h"

#include "app/chat_domain_service.h"
#include "app/provider_resolution_service.h"
#include "common/chat/chat_repository.h"
#include "common/config/approval_modes.h"
#include "common/paths/workspace_root.h"
#include "common/platform/platform_services.h"
#include "common/runtime/acp/acp_protocol_methods.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/utils/string_utils.h"

#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace uam::acp_detail
{

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
	if (IsClaudeSession(session))
	{
		session.initialized = true;
		session.load_session_supported = true;
		session.available_modes = {
		    AcpModeState{uam::approval_modes::kDefaultApprovalMode, "Default", "Use Claude default permissions."},
		    AcpModeState{uam::approval_modes::kAcceptEditsApprovalMode, "Accept Edits", "Auto-approve Claude file edits in the workspace."},
		    AcpModeState{uam::approval_modes::kPlanApprovalMode, "Plan", "Let Claude research and propose changes without editing files."},
		};
		if (session.current_mode_id.empty())
		{
			session.current_mode_id = uam::approval_modes::kDefaultApprovalMode;
		}
		return true;
	}

	const int id = NextAcpRequestId(session, uam::acp_methods::kInitialize);
	session.initialize_request_id = id;
	return WriteAcpMessage(session, IsCodexSession(session) ? BuildCodexInitializeRequest(id) : BuildInitializeRequest(id), error_out);
}

void ResetAcpRuntimeState(AcpSessionState& session)
{
	session.initialized = false;
	session.session_ready = false;
	session.load_session_supported = false;
	session.processing = false;
	session.cancel_requested = false;
	session.next_request_id = 1;
	session.initialize_request_id = 0;
	session.session_setup_request_id = 0;
	ClearAcpStartupModelRequest(session);
	session.prompt_request_id = 0;
	session.cancel_request_id = 0;
	session.current_assistant_message_index = -1;
	session.turn_user_message_index = -1;
	session.turn_assistant_message_index = -1;
	session.turn_serial = 0;
	session.queued_prompt.clear();
	session.goal_turn_kind.clear();
	session.goal_review_turn = false;
	session.goal_review_scheduled = false;
	session.goal_review_goal_id.clear();
	session.goal_review_user_prompt.clear();
	session.goal_review_assistant_text.clear();
	session.ignore_session_updates_until_ready = false;
	session.codex_resume_fallback_attempted = false;
	session.gemini_resume_fallback_attempted = false;
	session.stdout_buffer.clear();
	session.stderr_buffer.clear();
	session.recent_stderr.clear();
	session.last_error.clear();
	session.has_last_exit_code = false;
	session.last_exit_code = 0;
	session.last_process_id.clear();
	session.assistant_replay_prefixes.clear();
	session.load_history_replay_updates.clear();
	session.diagnostics.clear();
	session.agent_name.clear();
	session.agent_title.clear();
	session.agent_version.clear();
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
		return *existing;
	}

	auto session = std::make_unique<AcpSessionState>();
	session->chat_id = chat.id;
	session->provider_id = provider.id;
	session->protocol_kind = ProviderStructuredProtocolOrDefault(provider);
	app.acp_sessions.push_back(std::move(session));
	return *app.acp_sessions.back();
}

bool FailAcpSessionSetupWrite(AppState& app, AcpSessionState& session, ChatSession& chat, const std::string& fallback_message)
{
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

	PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(session);
	ResetAcpRuntimeState(session);
	session.chat_id = chat.id;
	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
	session.provider_id = provider.id;
	session.protocol_kind = ProviderStructuredProtocolOrDefault(provider);
	const std::string codex_resume_id = IsCodexSession(session) ? ValidCodexResumeId(chat) : std::string{};
	const std::string acp_resume_id = IsCodexSession(session) ? std::string{} : ResolvedAcpResumeIdForChat(app, chat);
	session.session_id = IsCodexSession(session) ? codex_resume_id : acp_resume_id;
	session.codex_thread_id = codex_resume_id;
	session.lifecycle_state = kAcpLifecycleStarting;

	std::string startup_error;
	const std::filesystem::path workspace_root = uam::paths::ResolveWorkspaceRootPath(app, chat);
	const std::vector<std::string> launch_argv = BuildAcpLaunchArgv(provider, chat);
	const std::string launch_detail = BuildAcpLaunchDetail(app, workspace_root, chat);
	AppendAcpDiagnostic(session, "process_launch", "starting", "", "", false, 0, "", launch_detail);
	if (!PlatformServicesFactory::Instance().process_service.StartStdioProcess(session, workspace_root, launch_argv, &startup_error))
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
	session.last_process_id = AcpProcessHandleLabel(session);
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

	if (IsClaudeSession(session))
	{
		session.session_ready = true;
		session.session_id = ResolvedAcpResumeIdForChat(app, chat);
		session.current_mode_id = chat.approval_mode.empty() ? uam::approval_modes::kDefaultApprovalMode : chat.approval_mode;
		session.lifecycle_state = session.processing ? kAcpLifecycleProcessing : kAcpLifecycleReady;
		return true;
	}

	const std::filesystem::path workspace_root = uam::paths::ResolveWorkspaceRootPath(app, chat);
	const std::string cwd = AcpWorkingDirectoryString(workspace_root);
	const std::string resolved_resume_id = ResolvedAcpResumeIdForChat(app, chat);
	ChatSession resume_chat = chat;
	resume_chat.native_session_id = resolved_resume_id;
	if (IsCodexSession(session))
	{
		const std::string raw_resume_id = uam::strings::Trim(chat.native_session_id);
		const std::string resume_id = ValidCodexResumeId(resume_chat);
		if (!raw_resume_id.empty() && resume_id.empty())
		{
			AppendAcpDiagnostic(session, "session_setup", "codex_invalid_resume_id_ignored", "", "", false, 0, "Ignoring invalid Codex thread id and starting a new thread.", "nativeSessionId=" + raw_resume_id);
			chat.native_session_id.clear();
			SaveChatQuietly(app, chat);
		}
		const bool can_resume = !resume_id.empty();
		const int id = NextAcpRequestId(session, can_resume ? uam::acp_methods::kThreadResume : uam::acp_methods::kThreadStart);
		session.session_setup_request_id = id;
		session.ignore_session_updates_until_ready = false;
		session.lifecycle_state = kAcpLifecycleStarting;
		session.session_id = can_resume ? resume_id : "";
		session.codex_thread_id = session.session_id;
		ChatSession setup_chat = resume_chat;
		setup_chat.native_session_id = resume_id;
		const bool written = WriteAcpMessage(session, can_resume ? BuildCodexThreadResumeRequest(id, setup_chat, cwd) : BuildCodexThreadStartRequest(id, setup_chat, cwd));
		if (!written)
		{
			return FailAcpSessionSetupWrite(app, session, chat, "Failed to create Codex app-server thread.");
		}
		return written;
	}

	const std::string raw_resume_id = uam::strings::Trim(chat.native_session_id);
	const std::string resume_id = IsGenericAcpSession(session) ? ValidGenericAcpResumeId(resume_chat) : ValidGeminiResumeId(resume_chat);
	if (!raw_resume_id.empty() && resume_id.empty())
	{
		AppendInvalidResumeDiagnostic(session, raw_resume_id);
		chat.native_session_id.clear();
		SaveChatQuietly(app, chat);
	}

	const bool can_load = !resume_id.empty() && session.load_session_supported;
	const int id = NextAcpRequestId(session, can_load ? uam::acp_methods::kSessionLoad : uam::acp_methods::kSessionNew);
	session.session_setup_request_id = id;
	session.ignore_session_updates_until_ready = can_load;
	session.lifecycle_state = kAcpLifecycleStarting;
	session.session_id = can_load ? resume_id : "";

	if (can_load)
	{
		const bool written = WriteAcpMessage(session, BuildLoadSessionRequest(id, resume_id, cwd));
		if (!written)
		{
			return FailAcpSessionSetupWrite(app, session, chat, "Failed to load " + std::string(RuntimeDisplayName(session)) + " session.");
		}
		return written;
	}

	const bool written = WriteAcpMessage(session, BuildNewSessionRequest(id, cwd));
	if (!written)
	{
		return FailAcpSessionSetupWrite(app, session, chat, "Failed to create " + std::string(RuntimeDisplayName(session)) + " session.");
	}
	return written;
}

bool RetryGeminiSessionNewAfterInvalidLoad(AppState& app, AcpSessionState& session, ChatSession& chat, const AcpInvalidLoadRetryDetails& details)
{
	const bool unsupported_session = IsCodexSession(session) || IsGenericAcpSession(session);
	const bool is_session_load = details.failure.method == uam::acp_methods::kSessionLoad;
	const bool invalid_resume_error = GeminiErrorLooksLikeInvalidSessionId(details.failure.message, details.error_data);
	if (unsupported_session || !is_session_load || session.gemini_resume_fallback_attempted || !invalid_resume_error)
	{
		return false;
	}

	session.gemini_resume_fallback_attempted = true;
	session.session_setup_request_id = 0;
	session.session_id.clear();
	chat.native_session_id.clear();
	SaveChatQuietly(app, chat);
	const AcpFailureDetails& failure = details.failure;
	const std::string retry_message = "Gemini rejected the stored session id. Starting a new session instead.";
	AppendAcpDiagnostic(session, "response", "gemini_invalid_resume_id_retry_new", failure.method, failure.request_id, failure.has_code, failure.code, retry_message, details.detail_text);

	const std::filesystem::path workspace_root = uam::paths::ResolveWorkspaceRootPath(app, chat);
	const std::string cwd = AcpWorkingDirectoryString(workspace_root);
	const int retry_id = NextAcpRequestId(session, uam::acp_methods::kSessionNew);
	session.session_setup_request_id = retry_id;
	session.ignore_session_updates_until_ready = false;
	session.lifecycle_state = kAcpLifecycleStarting;

	if (!WriteAcpMessage(session, BuildNewSessionRequest(retry_id, cwd)))
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

bool SendStartupModelIfNeeded(AcpSessionState& session, const ChatSession& chat)
{
	if (!IsGenericAcpSession(session) || !session.running || !session.session_ready || session.startup_model_request_id != 0 || session.session_id.empty())
	{
		return false;
	}

	const std::string model_id = uam::strings::Trim(chat.model_id);
	if (model_id.empty() || session.current_model_id == model_id)
	{
		ClearAcpStartupModelRequest(session);
		return false;
	}

	const int id = NextAcpRequestId(session, uam::acp_methods::kSessionSetModel);
	session.startup_model_request_id = id;
	session.pending_startup_model_id = model_id;
	if (!WriteAcpMessage(session, BuildSetModelRequest(id, session.session_id, model_id)))
	{
		session.pending_request_methods.erase(id);
		ClearAcpStartupModelRequest(session);
		FailAcpTurnOrSession(session, uam::strings::NonEmptyOrFallback(session.last_error, "Failed to set OpenCode ACP model."));
		return false;
	}

	session.current_model_id = model_id;
	return true;
}

bool SendQueuedPromptIfReady(AcpSessionState& session, const ChatSession& chat)
{
	if (session.startup_model_request_id != 0)
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
	if (IsClaudeSession(session))
	{
		if (!WriteAcpMessage(session, BuildClaudeInputMessage(prompt)))
		{
			CompletePromptTurn(session, kAcpLifecycleError);
			return true;
		}
		session.queued_prompt.clear();
		return true;
	}

	const int id = NextAcpRequestId(session, IsCodexSession(session) ? uam::acp_methods::kTurnStart : uam::acp_methods::kSessionPrompt);
	session.prompt_request_id = id;
	const nlohmann::json request = IsCodexSession(session) ? BuildCodexTurnStartRequest(id, session.session_id, prompt, chat, session.current_model_id) : BuildPromptRequest(id, session.session_id, prompt);
	if (!WriteAcpMessage(session, request))
	{
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
	const auto it = app.pending_chat_save_at_by_chat_id.find(chat.id);
	if (it == app.pending_chat_save_at_by_chat_id.end() || it->second > due_at)
	{
		app.pending_chat_save_at_by_chat_id[chat.id] = due_at;
	}
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
	session.prompt_request_id = 0;
	session.processing = false;
	session.cancel_requested = false;
	session.queued_prompt.clear();
	session.current_assistant_message_index = -1;
	session.codex_turn_id.clear();
	session.load_history_replay_updates.clear();
	session.pending_assistant_thoughts.clear();
	ResetAcpPendingInteractionState(session);
	session.lifecycle_state.assign(lifecycle_state);
}

bool QueueGoalInternalPrompt(AcpSessionState& session, ChatSession& chat, const std::string& prompt, bool review_turn)
{
	if (prompt.empty() || !CanQueueGoalInternalPrompt(session))
	{
		return false;
	}
	session.queued_prompt = prompt;
	session.goal_turn_kind = review_turn ? std::string(kGoalTurnKindReview) : std::string(kGoalTurnKindWorkerContinuation);
	session.goal_review_turn = review_turn;
	AppendGoalLoopDiagnostic(session, review_turn ? "queue_review" : "queue_worker_continuation", session.goal_review_goal_id, prompt);
	session.processing = true;
	session.cancel_requested = false;
	session.current_assistant_message_index = -1;
	session.turn_user_message_index = -1;
	session.turn_assistant_message_index = -1;
	session.turn_serial += 1;
	ResetAcpTurnStreamState(session);
	ResetAcpPendingInteractionState(session);
	session.last_runtime_activity_time_s = GetAppTimeSeconds();
	session.last_error.clear();
	session.lifecycle_state = kAcpLifecycleProcessing;
	return SendQueuedPromptIfReady(session, chat);
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
