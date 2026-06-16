#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_session_internal.h"

#include "app/chat_domain_service.h"
#include "app/markdown_store_service.h"
#include "app/goal_service.h"
#include "app/memory_service.h"
#include "app/native_session_link_service.h"
#include "app/provider_resolution_service.h"
#include "common/chat/chat_repository.h"
#include "common/config/approval_modes.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_root.h"
#include "common/platform/platform_services.h"
#include "common/provider/codex/codex_options.h"
#include "common/provider/codex/cli/codex_thread_id.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_profile_constants.h"
#include "common/provider/provider_runtime.h"
#include "common/provider/runtime/provider_build_config.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/runtime/acp/acp_attention_kind.h"
#include "common/runtime/acp/acp_claude_stream.h"
#include "common/runtime/acp/acp_content.h"
#include "common/runtime/acp/acp_json_rpc.h"
#include "common/runtime/acp/acp_model_json.h"
#include "common/runtime/acp/acp_permissions.h"
#include "common/runtime/acp/acp_protocol_methods.h"
#include "common/runtime/acp/acp_request_defaults.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/acp/acp_statuses.h"
#include "common/runtime/acp/acp_stream_types.h"
#include "common/runtime/acp/acp_tool_items.h"
#include "common/runtime/terminal/terminal_identity.h"

#include "cef/cef_push.h"
#include "common/runtime/acp/acp_tool_kinds.h"
#include "common/runtime/app_time.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/parse_utils.h"
#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uam
{
	namespace
	{
		using namespace acp_detail;

		void CompletePromptTurn(AcpSessionState& session, std::string_view lifecycle_state);
		void CompletePromptTurnAndHandleGoalLoop(AppState& app, AcpSessionState& session, ChatSession& chat, std::string_view lifecycle_state, CefRefPtr<CefBrowser> browser);
		void FailAcpTurnOrSession(AcpSessionState& session, const std::string& message);
		void MarkAcpChatUnseenIfBackground(AppState& app, const ChatSession& chat);
		void SaveChatQuietly(AppState& app, const ChatSession& chat);
		void ScheduleChatSave(AppState& app, const ChatSession& chat, double delay_seconds = 0.5);
		void FlushPendingChatSaves(AppState& app);
		bool SetChatNativeSessionIdIfChanged(ChatSession& chat, std::string_view session_id);
		bool SyncAcpToolCallsToAssistantMessage(ChatSession& chat, AcpSessionState& session, bool create_if_missing);
		bool UpdateAcpStaleWait(AcpSessionState& session, double now_seconds);
		bool TryAutoApprovePendingPermission(AcpSessionState& session, const ChatSession& chat, std::string* error_out = nullptr);

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

		bool WriteAcpMessage(AcpSessionState& session, const nlohmann::json& message, std::string* error_out = nullptr)
		{
			std::string line = message.dump();
			line.push_back('\n');

			const std::string method = AcpMessageMethodForDiagnostics(message);
			const std::string request_id = AcpMessageRequestIdForDiagnostics(message);
			const std::string detail = AcpMessageDetailForDiagnostics(message);
			std::string write_error;
			if (!PlatformServicesFactory::Instance().process_service.WriteToStdioProcess(session, line.data(), line.size(), &write_error))
			{
				const std::string runtime_name = RuntimeDisplayName(session);
				session.last_error = write_error.empty() ? ("Failed to write message to " + runtime_name + ".") : ("Failed to write message to " + runtime_name + ": " + write_error);
				session.lifecycle_state = kAcpLifecycleError;
				AppendAcpDiagnostic(session, "write", "write_failed", method, request_id, false, 0, session.last_error, detail);
				if (error_out != nullptr)
				{
					*error_out = session.last_error;
				}
				return false;
			}

			AppendAcpDiagnostic(session, "write", "sent", method, request_id, false, 0, "", detail);
			return true;
		}

		bool SendInitialize(AcpSessionState& session, std::string* error_out = nullptr)
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

		void SyncResolvedNativeSessionIdForChat(AppState& app, const ChatSession& chat, std::string_view session_id, std::string_view previous_session_id = {})
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

		bool HandleGoalReviewCompletion(AppState& app, AcpSessionState& session, ChatSession& chat, CefRefPtr<CefBrowser> browser)
		{
			if (!session.goal_review_turn)
			{
				return false;
			}

			const std::string goal_id = session.goal_review_goal_id;
			const std::string review_user_prompt = session.goal_review_user_prompt;
			const std::string review_assistant_text = session.goal_review_assistant_text;
			const std::string review_text = MessageTextForGoalReview(chat, session.turn_assistant_message_index);
			ClearGoalReviewState(session);

			const std::optional<GoalService::ReviewDecision> parsed = GoalService::ParseReviewDecision(review_text);
			if (!parsed.has_value())
			{
				GoalService::RecordBlocker(app, goal_id, "Goal reviewer returned invalid JSON.");
				SaveChatQuietly(app, chat);
				if (Goal* active_goal = GoalService::FindActiveGoal(app, chat.id); active_goal != nullptr && active_goal->id == goal_id && active_goal->blocked_turn_count < 2)
				{
					(void)QueueGoalInternalPrompt(session, chat, GoalService::BuildReviewPrompt(*active_goal, review_user_prompt, review_assistant_text), true);
				}
						if (browser)
				{
					uam::PushStateUpdateIfChanged(browser, app);
				}
				return true;
			}

			const GoalService::ReviewDecision& decision = *parsed;
			if (decision.decision == "complete")
			{
				if (Goal* goal = GoalService::FindGoalById(app, chat.id, goal_id); goal != nullptr)
				{
					ApplyGoalProgressUpdate(*goal, decision);
				}
				(void)GoalService::UpdateGoalStatus(app, goal_id, GoalStatus::Complete);
				SaveChatQuietly(app, chat);
					if (browser)
				{
					uam::PushStateUpdateIfChanged(browser, app);
				}
				return true;
			}
			if (decision.decision == "blocked")
			{
				if (Goal* goal = GoalService::FindGoalById(app, chat.id, goal_id); goal != nullptr)
				{
					ApplyGoalProgressUpdate(*goal, decision);
				}
				GoalService::RecordBlocker(app, goal_id, decision.reason);
				if (GoalBlockerStopsImmediately(decision.blocker_kind))
				{
					(void)GoalService::UpdateGoalStatus(app, goal_id, GoalStatus::Blocked);
				}
				SaveChatQuietly(app, chat);
				if (Goal* active_goal = GoalService::FindActiveGoal(app, chat.id); active_goal != nullptr && active_goal->id == goal_id && !GoalBlockerStopsImmediately(decision.blocker_kind))
				{
					(void)QueueGoalInternalPrompt(session, chat, GoalService::BuildContinuationPrompt(*active_goal, active_goal->tokens_used, active_goal->token_budget), false);
				}
					if (browser)
				{
					uam::PushStateUpdateIfChanged(browser, app);
				}
				return true;
			}

			Goal* active_goal = GoalService::FindActiveGoal(app, chat.id);
			if (active_goal == nullptr || active_goal->id != goal_id)
			{
				SaveChatQuietly(app, chat);
				return true;
			}

			ApplyGoalProgressUpdate(*active_goal, decision);
			const std::string follow_up = NormalizeGoalNextPrompt(decision.next_prompt);
			if (active_goal->last_next_prompt == follow_up)
			{
				active_goal->same_next_prompt_count += 1;
			}
			else
			{
				active_goal->last_next_prompt = follow_up;
				active_goal->same_next_prompt_count = 1;
			}
			if (active_goal->same_next_prompt_count >= 3)
			{
				GoalService::RecordBlocker(app, goal_id, "Goal reviewer repeated the same next prompt.");
				SaveChatQuietly(app, chat);
				if (browser)
				{
					uam::PushStateUpdateIfChanged(browser, app);
				}
				return true;
			}
			SaveChatQuietly(app, chat);
			(void)QueueGoalInternalPrompt(session, chat, follow_up, false);
			if (browser)
			{
				uam::PushStateUpdateIfChanged(browser, app);
			}
			return true;
		}

		void ScheduleGoalReviewAfterSuccessfulTurn(AppState& app, AcpSessionState& session, ChatSession& chat, CefRefPtr<CefBrowser> browser)
		{
			if (session.goal_review_turn || session.goal_review_scheduled)
			{
				return;
			}
			if (session.goal_turn_kind == kGoalTurnKindReview)
			{
				AppendGoalLoopDiagnostic(session, "skip_schedule_after_review_turn", session.goal_review_goal_id);
				return;
			}
			if (session.turn_user_message_index < 0 || session.turn_assistant_message_index < 0)
			{
				AppendGoalLoopDiagnostic(session, "skip_schedule_missing_turn_indexes", session.goal_review_goal_id);
				return;
			}

			const std::string recent_user_prompt = MessageTextForGoalReview(chat, session.turn_user_message_index);
			const std::string recent_assistant_text = MessageTextForGoalReview(chat, session.turn_assistant_message_index);
			if (uam::strings::Trim(recent_user_prompt).empty() || uam::strings::Trim(recent_assistant_text).empty())
			{
				AppendGoalLoopDiagnostic(session, "skip_schedule_empty_turn_text", session.goal_review_goal_id, recent_assistant_text);
				return;
			}
			if (GoalService::ParseReviewDecision(recent_assistant_text).has_value())
			{
				AppendGoalLoopDiagnostic(session, "skip_schedule_review_decision_output", session.goal_review_goal_id, recent_assistant_text);
				return;
			}

			Goal* active_goal = GoalService::FindActiveGoal(app, chat.id);
			if (active_goal == nullptr || active_goal->objective.empty())
			{
				return;
			}

			GoalService::RecordTurnCompletion(app, active_goal->id, EstimateGoalTurnTokens(chat, session));
			active_goal = GoalService::FindActiveGoal(app, chat.id);
			SaveChatQuietly(app, chat);
			if (active_goal == nullptr)
			{
				if (browser)
				{
					uam::PushStateUpdateIfChanged(browser, app);
				}
				return;
			}

			session.goal_review_scheduled = true;
			session.goal_review_goal_id = active_goal->id;
			session.goal_review_user_prompt = recent_user_prompt;
			session.goal_review_assistant_text = recent_assistant_text;
			const std::string review_prompt = GoalService::BuildReviewPrompt(*active_goal, session.goal_review_user_prompt, session.goal_review_assistant_text);
			AppendGoalLoopDiagnostic(session, "schedule_review", active_goal->id, recent_assistant_text);
			(void)QueueGoalInternalPrompt(session, chat, review_prompt, true);
		}

		void CompletePromptTurnAndHandleGoalLoop(AppState& app, AcpSessionState& session, ChatSession& chat, std::string_view lifecycle_state, CefRefPtr<CefBrowser> browser)
		{
			const std::string completed_goal_turn_kind = session.goal_turn_kind;
			const bool completed_review_turn = completed_goal_turn_kind == kGoalTurnKindReview || session.goal_review_turn;
			const std::string goal_id = session.goal_review_goal_id;
			CompletePromptTurn(session, lifecycle_state);
			session.crash_restart_attempts = 0;

			if (completed_review_turn)
			{
				AppendGoalLoopDiagnostic(session, "complete_review_turn", goal_id, MessageTextForGoalReview(chat, session.turn_assistant_message_index));
				if (!HandleGoalReviewCompletion(app, session, chat, browser))
				{
					if (!goal_id.empty())
					{
						GoalService::RecordBlocker(app, goal_id, "Goal reviewer turn completed but could not be consumed.");
						SaveChatQuietly(app, chat);
						if (browser)
						{
							uam::PushStateUpdateIfChanged(browser, app);
						}
					}
					ClearGoalReviewState(session);
				}
				if (session.goal_turn_kind == completed_goal_turn_kind)
				{
					session.goal_turn_kind.clear();
				}
				return;
			}

			ScheduleGoalReviewAfterSuccessfulTurn(app, session, chat, browser);
			if (session.goal_turn_kind == completed_goal_turn_kind)
			{
				session.goal_turn_kind.clear();
			}
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

		Message& EnsureAssistantMessage(ChatSession& chat, AcpSessionState& session)
		{
			if (Message* message = CurrentAssistantMessage(chat, session))
			{
				return *message;
			}

			Message message;
			message.role = MessageRole::Assistant;
			message.provider = MessageProviderId(session);
			message.created_at = AcpTimestampNow();
			chat.messages.push_back(std::move(message));
			session.current_assistant_message_index = static_cast<int>(chat.messages.size()) - 1;
			session.turn_assistant_message_index = session.current_assistant_message_index;
			return chat.messages.back();
		}

		bool AppendThoughtChunk(ChatSession& chat, AcpSessionState& session, const std::string& chunk)
		{
			if (chunk.empty())
			{
				return false;
			}

			const bool starts_new_block = AppendThoughtTurnEvent(session, chunk);
			if (Message* message = CurrentAssistantMessage(chat, session))
			{
				AppendThoughtText(message->thoughts, chunk, starts_new_block);
				(void)SyncMessageBlocksFromTurnEvents(*message, session);
				chat.updated_at = AcpTimestampNow();
				return true;
			}

			AppendThoughtText(session.pending_assistant_thoughts, chunk, starts_new_block);
			return false;
		}

		std::string AppendAssistantChunk(ChatSession& chat, AcpSessionState& session, const std::string& chunk)
		{
			if (chunk.empty())
			{
				return "";
			}

			std::string current_assistant_text;
			if (const Message* current_message = CurrentAssistantMessage(chat, session))
			{
				current_assistant_text = current_message->content;
			}

			const std::string delta = AssistantDeltaForIncomingText(session, current_assistant_text, chunk);
			if (delta.empty())
			{
				return "";
			}

			Message* current_message = CurrentAssistantMessage(chat, session);
			Message& message = current_message == nullptr ? EnsureAssistantMessage(chat, session) : *current_message;
			if (!session.pending_assistant_thoughts.empty())
			{
				AppendThoughtText(message.thoughts, session.pending_assistant_thoughts, !message.thoughts.empty());
				session.pending_assistant_thoughts.clear();
			}
			message.content += delta;
			(void)SyncAcpToolCallsToAssistantMessage(chat, session, false);
			chat.updated_at = AcpTimestampNow();
			if (session.turn_assistant_message_index < 0)
			{
				session.turn_assistant_message_index = session.current_assistant_message_index;
			}
			AppendAssistantTextTurnEvent(session, delta);
			(void)SyncCurrentAssistantMessageBlocksFromTurnEvents(chat, session);
			return delta;
		}

		ToolCall PersistedToolCallFromAcpToolCall(const AcpToolCallState& tool_call)
		{
			ToolCall persisted;
			persisted.id = tool_call.id;
			persisted.name = uam::strings::NonEmptyOrFallback(tool_call.title, uam::strings::NonEmptyOrFallback(tool_call.kind, tool_call.id));
			persisted.status = tool_call.status;
			persisted.result_text = tool_call.content;
			persisted.is_sub_agent = tool_call.is_sub_agent;
			persisted.sub_agent_id = tool_call.sub_agent_id;
			persisted.sub_agent_title = tool_call.sub_agent_title;
			return persisted;
		}

		bool UpsertPersistedToolCall(std::vector<ToolCall>& tool_calls, const AcpToolCallState& tool_call)
		{
			if (tool_call.id.empty())
			{
				return false;
			}

			const ToolCall persisted = PersistedToolCallFromAcpToolCall(tool_call);
			for (ToolCall& existing : tool_calls)
			{
				if (existing.id != persisted.id)
				{
					continue;
				}

				if (existing.name == persisted.name && existing.args_json == persisted.args_json && existing.result_text == persisted.result_text && existing.status == persisted.status && existing.is_sub_agent == persisted.is_sub_agent && existing.sub_agent_id == persisted.sub_agent_id && existing.sub_agent_title == persisted.sub_agent_title)
				{
					return false;
				}

				existing = persisted;
				return true;
			}

			tool_calls.push_back(persisted);
			return true;
		}

		bool SyncAcpToolCallsToAssistantMessage(ChatSession& chat, AcpSessionState& session, bool create_if_missing)
		{
			if (session.tool_calls.empty())
			{
				return false;
			}

			Message* message = CurrentAssistantMessage(chat, session);
			if (message == nullptr)
			{
				if (!create_if_missing)
				{
					return false;
				}

				message = &EnsureAssistantMessage(chat, session);
			}

			bool changed = false;
			for (const AcpToolCallState& tool_call : session.tool_calls)
			{
				changed |= UpsertPersistedToolCall(message->tool_calls, tool_call);
			}
			changed |= SyncMessageBlocksFromTurnEvents(*message, session);
			if (changed)
			{
				chat.updated_at = AcpTimestampNow();
			}
			return changed;
		}

		MessagePlanEntry PersistedPlanEntryFromAcpPlanEntry(const AcpPlanEntryState& entry)
		{
			MessagePlanEntry persisted;
			persisted.content = entry.content;
			persisted.priority = entry.priority;
			persisted.status = entry.status;
			return persisted;
		}

		std::vector<MessagePlanEntry> PersistedPlanEntriesFromAcpPlanEntries(const std::vector<AcpPlanEntryState>& entries)
		{
			std::vector<MessagePlanEntry> persisted;
			persisted.reserve(entries.size());
			for (const AcpPlanEntryState& entry : entries)
			{
				persisted.push_back(PersistedPlanEntryFromAcpPlanEntry(entry));
			}
			return persisted;
		}

		bool MessagePlanEntriesEqual(const std::vector<MessagePlanEntry>& lhs, const std::vector<MessagePlanEntry>& rhs)
		{
			if (lhs.size() != rhs.size())
			{
				return false;
			}
			for (std::size_t i = 0; i < lhs.size(); ++i)
			{
				if (lhs[i].content != rhs[i].content || lhs[i].priority != rhs[i].priority || lhs[i].status != rhs[i].status)
				{
					return false;
				}
			}
			return true;
		}

		bool SyncAcpPlanToAssistantMessage(ChatSession& chat, AcpSessionState& session, bool create_if_missing)
		{
			if (uam::strings::IsBlank(session.plan_summary) && session.plan_entries.empty())
			{
				return false;
			}

			Message* message = CurrentAssistantMessage(chat, session);
			if (message == nullptr)
			{
				if (!create_if_missing)
				{
					return false;
				}
				message = &EnsureAssistantMessage(chat, session);
			}

			const std::vector<MessagePlanEntry> persisted_entries = PersistedPlanEntriesFromAcpPlanEntries(session.plan_entries);
			bool changed = false;
			if (message->plan_summary != session.plan_summary || !MessagePlanEntriesEqual(message->plan_entries, persisted_entries))
			{
				message->plan_summary = session.plan_summary;
				message->plan_entries = persisted_entries;
				changed = true;
			}
			changed |= SyncMessageBlocksFromTurnEvents(*message, session);
			if (changed)
			{
				chat.updated_at = AcpTimestampNow();
			}
			return changed;
		}

		AcpToolCallState& UpsertToolCall(AcpSessionState& session, const std::string& id)
		{
			for (AcpToolCallState& tool_call : session.tool_calls)
			{
				if (tool_call.id == id)
				{
					return tool_call;
				}
			}

			AcpToolCallState tool_call;
			tool_call.id = id;
			session.tool_calls.push_back(std::move(tool_call));
			return session.tool_calls.back();
		}

		bool LooksLikeSubAgentTool(const nlohmann::json& update, const AcpToolCallState& tool_call, const IProviderRuntime& runtime)
		{
			if (JsonBooleanValueOr(update, "isSubAgent", false) || JsonBooleanValueOr(update, "subAgent", false))
			{
				return true;
			}

			if (WordMatchesAnyCaseInsensitive(tool_call.kind, {"subagent", "sub-agent"}) ||
			    WordMatchesAnyCaseInsensitive(tool_call.title, {"subagent", "sub-agent"}))
			{
				return true;
			}

			return runtime.ProviderRecognizesSubagentTool(tool_call.title);
		}

		void ApplySubAgentMetadata(AcpToolCallState& tool_call, const nlohmann::json& update, const IProviderRuntime& runtime)
		{
			const std::string sub_agent_id = uam::strings::NonEmptyOrFallback(
			    JsonDiagnosticStringValue(update, "subAgentId"),
			    uam::strings::NonEmptyOrFallback(JsonDiagnosticStringValue(update, "agentId"), JsonDiagnosticStringValue(update, "sessionId")));
			const std::string sub_agent_title = uam::strings::NonEmptyOrFallback(
			    JsonDiagnosticStringValue(update, "subAgentTitle"),
			    uam::strings::NonEmptyOrFallback(JsonDiagnosticStringValue(update, "agentName"), JsonDiagnosticStringValue(update, "agent")));

			if (!sub_agent_id.empty())
			{
				tool_call.sub_agent_id = sub_agent_id;
			}
			if (!sub_agent_title.empty())
			{
				tool_call.sub_agent_title = sub_agent_title;
			}
			if (LooksLikeSubAgentTool(update, tool_call, runtime) || !tool_call.sub_agent_id.empty() || !tool_call.sub_agent_title.empty())
			{
				tool_call.is_sub_agent = true;
			}
		}

		void HandleSessionUpdate(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& params, CefRefPtr<CefBrowser> browser)
		{
			const nlohmann::json update = JsonObjectValue(params, "update");
			if (!update.is_object())
			{
				return;
			}

			std::string update_type = JsonDiagnosticStringValue(update, "sessionUpdate");
			const bool is_thought_update = JsonBooleanValueOr(update, "thought", false);
			const bool has_tool_call_id = uam::nlohmann_json::FindField(update, "toolCallId") != nullptr;
			if (update_type.empty() && is_thought_update)
			{
				update_type = uam::acp_stream_types::kSessionUpdateAgentThoughtChunk;
			}
			if (update_type.empty() && has_tool_call_id)
			{
				update_type = uam::acp_stream_types::kSessionUpdateToolCallUpdate;
			}
			const nlohmann::json* content = uam::nlohmann_json::FindField(update, "content");
			const std::string content_text = content == nullptr ? "" : ContentTextFromJson(*content);
			std::string live_text;
			if (update_type == uam::acp_stream_types::kSessionUpdateCurrentMode)
			{
				const std::string current_mode_id = uam::nlohmann_json::TrimmedStringValue(update, {"currentModeId"});
				if (!current_mode_id.empty())
				{
					session.current_mode_id = AppApprovalModeId(current_mode_id);
				}
				return;
			}
			if (session.ignore_session_updates_until_ready)
			{
				(void)TryConsumeLoadHistoryReplayUpdate(session, update, update_type, content_text, live_text);
				return;
			}

			if (!uam::AcpSessionHasActiveTurn(session))
			{
				return;
			}

			if (update_type == uam::acp_stream_types::kSessionUpdateUserMessageChunk)
			{
				(void)TryConsumeLoadHistoryReplayUpdate(session, update, update_type, content_text, live_text);
				return;
			}

			if (update_type == uam::acp_stream_types::kSessionUpdateAgentThoughtChunk || is_thought_update)
			{
				live_text = content_text;
				if (TryConsumeLoadHistoryReplayUpdate(session, update, update_type, content_text, live_text) && live_text.empty())
				{
					return;
				}

				if (AppendThoughtChunk(chat, session, live_text))
				{
					if (browser)
					{
						uam::PushStreamToken(browser, chat.id, live_text);
					}
					ScheduleChatSave(app, chat, 0.5);
				}
				return;
			}

			if (update_type == uam::acp_stream_types::kSessionUpdateAgentMessageChunk)
			{
				live_text = content_text;
				if (TryConsumeLoadHistoryReplayUpdate(session, update, update_type, content_text, live_text) && live_text.empty())
				{
					return;
				}

				const std::string appended = AppendAssistantChunk(chat, session, live_text);
				if (browser && !appended.empty())
				{
					uam::PushStreamToken(browser, chat.id, appended);
				}
				ScheduleChatSave(app, chat, 0.5);
				return;
			}

			if (update_type == uam::acp_stream_types::kSessionUpdateToolCall || has_tool_call_id)
			{
				if (TryConsumeLoadHistoryReplayUpdate(session, update, update_type, content_text, live_text))
				{
					return;
				}

				const std::string id = JsonDiagnosticStringValue(update, "toolCallId");
				if (!id.empty())
				{
					AcpToolCallState& tool_call = UpsertToolCall(session, id);
					tool_call.title = JsonDiagnosticStringValueOr(update, "title", tool_call.title);
					tool_call.kind = JsonDiagnosticStringValueOr(update, "kind", uam::acp_tool_kinds::ExistingOrOther(tool_call.kind));
					tool_call.status = JsonDiagnosticStringValueOr(update, "status", uam::acp_statuses::ExistingOrPending(tool_call.status));
					if (content != nullptr)
					{
						tool_call.content = ContentTextFromJson(*content);
					}
					if (const ProviderProfile* provider_profile = ProviderResolutionService().ProviderForChat(app, chat); provider_profile != nullptr)
					{
						const IProviderRuntime& runtime = ProviderRuntimeRegistry::Resolve(*provider_profile);
						ApplySubAgentMetadata(tool_call, update, runtime);
					}
					else if (!chat.provider_id.empty())
					{
						const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(chat.provider_id);
						ApplySubAgentMetadata(tool_call, update, runtime);
					}
					else
					{
						ApplySubAgentMetadata(tool_call, update, ProviderRuntimeRegistry::ResolveById(std::string_view{}));
					}
					AppendToolTurnEventIfNeeded(session, id);
					if (SyncAcpToolCallsToAssistantMessage(chat, session, false))
					{
						SaveChatQuietly(app, chat);
					}
				}
				return;
			}

			if (const nlohmann::json* entries = uam::nlohmann_json::FindArrayField(update, "entries");
			    update_type == uam::acp_stream_types::kSessionUpdatePlan && entries != nullptr)
			{
				session.plan_summary = JsonDiagnosticStringValueOr(update, "summary", JsonDiagnosticStringValue(update, "explanation"));
				session.plan_entries.clear();
				for (const nlohmann::json& entry : *entries)
				{
					if (!entry.is_object())
					{
						continue;
					}
					AcpPlanEntryState plan_entry;
					plan_entry.content = JsonDiagnosticStringValue(entry, "content");
					plan_entry.priority = JsonDiagnosticStringValue(entry, "priority");
					plan_entry.status = JsonDiagnosticStringValue(entry, "status");
					session.plan_entries.push_back(std::move(plan_entry));
				}
				AppendPlanTurnEventIfNeeded(session);
				if (SyncAcpPlanToAssistantMessage(chat, session, true))
				{
					SaveChatQuietly(app, chat);
				}
			}
		}

		void SendJsonRpcError(AcpSessionState& session, const nlohmann::json& id, int code, const std::string& message)
		{
			(void)WriteAcpMessage(session, uam::acp_json_rpc::ErrorResponse(id, code, message));
		}

		nlohmann::json BuildGenericPermissionOutcomeResult(const std::string& option_id, bool cancelled)
		{
			nlohmann::json outcome = {
			    {uam::acp_permissions::kOutcomeField, cancelled ? uam::acp_permissions::kCancelledOutcome : uam::acp_permissions::kSelectedOutcome},
			};

			if (!cancelled)
			{
				outcome[uam::acp_permissions::kOptionIdField] = option_id;
			}

			return {
			    {uam::acp_permissions::kOutcomeField, std::move(outcome)},
			};
		}

		bool SendPermissionResponse(AcpSessionState& session, const std::string& request_id_json, const std::string& option_id, bool cancelled, std::string* error_out = nullptr)
		{
			nlohmann::json response = uam::acp_json_rpc::SuccessResponse(StableStringToJsonRpcId(request_id_json), nlohmann::json::object());
			if (IsCodexSession(session))
			{
				const std::string kind = session.pending_permission.provider_request_kind;
				const bool deny = uam::acp_permissions::IsDenyDecision(option_id, cancelled);
				if (uam::acp_permissions::IsCodexDecisionPermissionKind(kind))
				{
					response["result"] = {{"decision", uam::acp_permissions::CodexDecisionForOption(option_id, cancelled)}};
				}
				else if (kind == uam::acp_permissions::kCodexPermissionsRequestKind)
				{
					nlohmann::json permissions = nlohmann::json::object();
					if (!deny && !session.pending_permission.codex_approval_payload_json.empty())
					{
						try
						{
							const nlohmann::json payload = nlohmann::json::parse(session.pending_permission.codex_approval_payload_json);
							if (const nlohmann::json* parsed_permissions = uam::nlohmann_json::FindField(payload, "permissions"); parsed_permissions != nullptr)
							{
								permissions = *parsed_permissions;
							}
						}
						catch (const nlohmann::json::exception&)
						{
							permissions = nlohmann::json::object();
						}
					}
					response["result"] = {
					    {uam::acp_permissions::kPermissionsField, permissions},
					    {uam::acp_permissions::kScopeField, uam::acp_permissions::kSessionScope},
					};
				}
				else
				{
					response["result"] = nlohmann::json::object();
				}
				return WriteAcpMessage(session, response, error_out);
			}

			response["result"] = BuildGenericPermissionOutcomeResult(option_id, cancelled);
			return WriteAcpMessage(session, response, error_out);
		}

		bool LooksLikeAutoApprovablePermission(const AcpPendingPermissionState& pending)
		{
			return TextContainsAnyCaseInsensitive(pending.kind,
			                                      {
			                                          "command",
			                                          "file",
			                                          "permission",
			                                          "tool",
			                                      }) ||
			       TextContainsAnyCaseInsensitive(pending.title, {
			                                             "command",
			                                             "file change",
			                                             "permission",
			                                         });
		}

		bool IsRejectPermissionOption(const std::string& id, const std::string& name, const std::string& kind)
		{
			return TextContainsAnyCaseInsensitive(id,
			                                      {
			                                          "decline",
			                                          "deny",
			                                          uam::acp_permissions::kCancelDecision,
			                                      }) ||
			       TextContainsAnyCaseInsensitive(name,
			                                      {
			                                          "decline",
			                                          "deny",
			                                          uam::acp_permissions::kCancelDecision,
			                                      }) ||
			       TextContainsAnyCaseInsensitive(kind, {
			                                            uam::acp_permissions::kCancelOptionKind,
			                                        });
		}

		bool IsAcceptPermissionOption(const std::string& id, const std::string& name)
		{
			return TextContainsAnyCaseInsensitive(id,
			                                      {
			                                          "accept",
			                                          "allow",
			                                      }) ||
			       TextContainsAnyCaseInsensitive(name, {
			                                            "accept",
			                                            "allow",
			                                        });
		}

		std::string AutoApproveOptionId(const AcpPendingPermissionState& pending)
		{
			for (const AcpPermissionOptionState& option : pending.options)
			{
				const std::string id = uam::strings::ToLowerAscii(option.id);
				const std::string name = uam::strings::ToLowerAscii(option.name);
				const std::string kind = uam::strings::ToLowerAscii(option.kind);
				if (IsRejectPermissionOption(id, name, kind))
				{
					continue;
				}
				if (IsAcceptPermissionOption(id, name))
				{
					return option.id;
				}
			}
			return "";
		}

		bool TryAutoApprovePendingPermission(AcpSessionState& session, const ChatSession& chat, std::string* error_out)
		{
			if (!chat.auto_approve_commands || session.pending_permission.request_id_json.empty())
			{
				return false;
			}
			if (!LooksLikeAutoApprovablePermission(session.pending_permission))
			{
				return false;
			}

			std::string option_id = AutoApproveOptionId(session.pending_permission);
			if (!IsCodexSession(session) && option_id.empty())
			{
				return false;
			}

			if (!SendPermissionResponse(session, session.pending_permission.request_id_json, option_id, false, error_out))
			{
				return false;
			}

			if (!session.pending_permission.tool_call_id.empty())
			{
				AcpToolCallState& tracked_tool_call = UpsertToolCall(session, session.pending_permission.tool_call_id);
				tracked_tool_call.status = uam::acp_statuses::kAutoApproved;
			}
			AppendAcpDiagnostic(session, "permission", uam::acp_statuses::kAutoApproved, session.pending_permission.provider_request_method, session.pending_permission.request_id_json, false, 0, "UAM yolo auto-approved a command permission request.");
			session.pending_permission = AcpPendingPermissionState{};
			session.waiting_for_permission = false;
			ClearAcpPendingWait(session);
			session.lifecycle_state = session.processing ? kAcpLifecycleProcessing : kAcpLifecycleReady;
			return true;
		}

		void AppendIgnoredRequestDuringCancelDiagnostic(AcpSessionState& session, const nlohmann::json& message, const char* reason, const char* diagnostic_message)
		{
			AppendAcpDiagnostic(session, "request", reason, JsonDiagnosticStringValue(message, "method"), JsonRpcIdToStableString(JsonRpcIdOrNull(message)), false, 0, diagnostic_message);
		}

		nlohmann::json BuildCodexUserInputResponse(const std::string& request_id_json, const std::map<std::string, std::vector<std::string>>& answers)
		{
			nlohmann::json answer_map = nlohmann::json::object();
			for (const auto& [question_id, values] : answers)
			{
				if (question_id.empty())
				{
					continue;
				}

				nlohmann::json answer_values = nlohmann::json::array();
				for (const std::string& value : values)
				{
					answer_values.push_back(value);
				}
				answer_map[question_id] = {{"answers", std::move(answer_values)}};
			}

			return uam::acp_json_rpc::SuccessResponse(StableStringToJsonRpcId(request_id_json), {
			                                                                                        {"answers", std::move(answer_map)},
			                                                                                    });
		}

		bool SendCodexUserInputResponse(AcpSessionState& session, const std::string& request_id_json, const std::map<std::string, std::vector<std::string>>& answers, std::string* error_out = nullptr)
		{
			return WriteAcpMessage(session, BuildCodexUserInputResponse(request_id_json, answers), error_out);
		}

		void HandlePermissionRequest(AcpSessionState& session, ChatSession& chat, const nlohmann::json& message)
		{
			if (uam::AcpSessionHasPendingCancel(session))
			{
				AppendIgnoredRequestDuringCancelDiagnostic(session, message, "ignored_permission_during_cancel", "Ignoring permission request while a turn cancel is pending.");
				return;
			}

			const nlohmann::json params = JsonObjectValue(message, "params");
			const nlohmann::json tool_call = JsonObjectValue(params, "toolCall");

			AcpPendingPermissionState pending;
			pending.request_id_json = JsonRpcIdToStableString(JsonRpcIdOrNull(message));
			pending.tool_call_id = JsonDiagnosticStringValue(tool_call, "toolCallId");
			pending.title = JsonDiagnosticStringValueOr(tool_call, "title", "Permission required");
			pending.kind = JsonDiagnosticStringValueOr(tool_call, "kind", uam::acp_tool_kinds::kOther);
			pending.status = JsonDiagnosticStringValueOr(tool_call, "status", std::string(uam::acp_statuses::kPending));
			if (const nlohmann::json* content = uam::nlohmann_json::FindField(tool_call, "content"); content != nullptr)
			{
				pending.content = ContentTextFromJson(*content);
			}

			const nlohmann::json options = JsonArrayValue(params, "options");
			if (options.is_array())
			{
				for (const nlohmann::json& option : options)
				{
					if (!option.is_object())
					{
						continue;
					}
					AcpPermissionOptionState parsed;
					parsed.id = JsonDiagnosticStringValue(option, "optionId");
					parsed.name = JsonDiagnosticStringValueOr(option, "name", parsed.id);
					parsed.kind = JsonDiagnosticStringValue(option, "kind");
					if (!parsed.id.empty())
					{
						pending.options.push_back(std::move(parsed));
					}
				}
			}

			if (!pending.tool_call_id.empty())
			{
				AcpToolCallState& tracked_tool_call = UpsertToolCall(session, pending.tool_call_id);
				tracked_tool_call.title = pending.title;
				tracked_tool_call.kind = pending.kind;
				tracked_tool_call.status = pending.status;
				tracked_tool_call.content = pending.content;
			}
			AppendPermissionTurnEventIfNeeded(session, pending.request_id_json, pending.tool_call_id);

			session.pending_permission = std::move(pending);
			if (TryAutoApprovePendingPermission(session, chat))
			{
				return;
			}
			session.waiting_for_permission = true;
			BeginAcpPendingWait(session, kAcpLifecycleWaitingPermission);
		}

		std::string CodexItemTitle(const nlohmann::json& item)
		{
			const std::string type = JsonDiagnosticStringValueOr(item, "type", "tool");
			if (type == uam::acp_tool_items::kCommandExecution)
			{
				const std::string command = JsonDiagnosticStringValue(item, "command");
				return uam::strings::NonEmptyOrFallback(command, "Command");
			}
			if (type == uam::acp_tool_items::kFileChange)
			{
				return "File changes";
			}
			if (type == uam::acp_tool_items::kMcpToolCall)
			{
				return JsonDiagnosticStringValueOr(item, "tool", "MCP tool");
			}
			if (type == uam::acp_tool_items::kDynamicToolCall)
			{
				return JsonDiagnosticStringValueOr(item, "tool", "Tool");
			}
			return type;
		}

		std::string CodexItemContent(const nlohmann::json& item)
		{
			const std::string type = JsonDiagnosticStringValue(item, "type");
			if (type == uam::acp_tool_items::kCommandExecution)
			{
				return JsonDiagnosticStringValue(item, "aggregatedOutput");
			}
			if (type == uam::acp_tool_items::kAgentMessage)
			{
				return JsonDiagnosticStringValue(item, uam::acp_content::kTextField);
			}
			if (type == uam::acp_tool_items::kPlan)
			{
				return JsonDiagnosticStringValue(item, uam::acp_content::kTextField);
			}
			if (uam::acp_tool_items::UsesWholeItemAsContent(type))
			{
				return item.dump();
			}
			return "";
		}

		std::string CodexReasoningPartText(const nlohmann::json& value)
		{
			if (value.is_string())
			{
				return value.get_ref<const std::string&>();
			}
			if (value.is_object())
			{
				return JsonDiagnosticStringValue(value, uam::acp_content::kTextField);
			}
			return value.is_null() ? "" : value.dump();
		}

		std::string CodexReasoningKey(const std::string& item_id, const std::string& section, int index)
		{
			if (item_id.empty())
			{
				return "";
			}
			if (index < 0)
			{
				return item_id + "\n" + section;
			}
			return item_id + "\n" + section + "\n" + std::to_string(index);
		}

		int JsonIntValueOr(const nlohmann::json& object, const char* key, int fallback)
		{
			if (!object.is_object())
			{
				return fallback;
			}
			return uam::nlohmann_json::IntFieldStrict(object, key).value_or(fallback);
		}

		bool CodexReasoningWasStreamed(const AcpSessionState& session, const std::string& item_id, const std::string& section, int index)
		{
			if (item_id.empty())
			{
				return false;
			}
			const std::string wildcard_key = CodexReasoningKey(item_id, section, -1);
			const std::string indexed_key = CodexReasoningKey(item_id, section, index);
			return session.codex_streamed_reasoning_keys.contains(wildcard_key) || session.codex_streamed_reasoning_keys.contains(indexed_key);
		}

		std::string CodexCompletedReasoningSectionText(const AcpSessionState& session, const nlohmann::json& item, const char* key, const std::string& item_id, const std::string& section)
		{
			if (!item.is_object())
			{
				return "";
			}
			const auto it = item.find(key);
			if (it == item.end() || it->is_null())
			{
				return "";
			}
			if (!it->is_array())
			{
				if (CodexReasoningWasStreamed(session, item_id, section, 0))
				{
					return "";
				}
				return JsonDiagnosticStringValue(item, key);
			}

			std::vector<std::string> reasoning_parts;
			for (std::size_t i = 0; i < it->size(); ++i)
			{
				if (CodexReasoningWasStreamed(session, item_id, section, static_cast<int>(i)))
				{
					continue;
				}
				const std::string text = CodexReasoningPartText((*it)[i]);
				if (text.empty())
				{
					continue;
				}
				reasoning_parts.push_back(text);
			}
			return uam::strings::JoinNonEmpty(reasoning_parts, "\n");
		}

		bool AppendCodexReasoningThought(ChatSession& chat, AcpSessionState& session, const std::string& item_id, const std::string& section, const std::string& text, int index, bool streamed)
		{
			if (text.empty())
			{
				return false;
			}

			if (streamed && !item_id.empty())
			{
				session.codex_streamed_reasoning_keys.insert(CodexReasoningKey(item_id, section, index));
			}

			std::string chunk = text;
			if (session.codex_last_reasoning_section != section)
			{
				chunk = (session.codex_last_reasoning_section.empty() ? "### " : "\n\n### ") + section + "\n" + text;
				session.codex_last_reasoning_section = section;
			}

			(void)EnsureAssistantMessage(chat, session);
			return AppendThoughtChunk(chat, session, chunk);
		}

		bool HandleCodexCompletedReasoningItem(ChatSession& chat, AcpSessionState& session, const nlohmann::json& item)
		{
			const std::string item_id = JsonDiagnosticStringValue(item, "id");
			bool changed = false;
			const std::string raw_content = CodexCompletedReasoningSectionText(session, item, "content", item_id, "Reasoning");
			if (!raw_content.empty())
			{
				changed |= AppendCodexReasoningThought(chat, session, item_id, "Reasoning", raw_content, -1, false);
			}

			const std::string summary = CodexCompletedReasoningSectionText(session, item, "summary", item_id, "Summary");
			if (!summary.empty())
			{
				changed |= AppendCodexReasoningThought(chat, session, item_id, "Summary", summary, -1, false);
			}
			return changed;
		}

		std::string CodexStreamedAgentMessageDelta(AcpSessionState& session, const std::string& item_id, const std::string& delta)
		{
			if (delta.empty())
			{
				return "";
			}
			if (!item_id.empty())
			{
				session.codex_agent_message_text_by_item_id[item_id] += delta;
			}
			return delta;
		}

		std::string CodexCompletedAgentMessageDelta(AcpSessionState& session, const std::string& item_id, const std::string& text)
		{
			if (text.empty())
			{
				return "";
			}
			if (item_id.empty())
			{
				return text;
			}

			std::string& streamed_text = session.codex_agent_message_text_by_item_id[item_id];
			if (streamed_text.empty())
			{
				streamed_text = text;
				return text;
			}
			if (text == streamed_text || uam::strings::StartsWith(streamed_text, text))
			{
				return "";
			}
			if (uam::strings::StartsWith(text, streamed_text))
			{
				const std::string suffix = text.substr(streamed_text.size());
				streamed_text = text;
				return suffix;
			}

			streamed_text = text;
			return text;
		}

		bool CurrentAssistantMessageHasContent(const ChatSession& chat, const AcpSessionState& session)
		{
			const Message* message = CurrentAssistantMessage(chat, session);
			return message != nullptr && !message->content.empty();
		}

		std::string AppendCodexAgentMessageText(ChatSession& chat, AcpSessionState& session, const std::string& item_id, const std::string& delta)
		{
			if (delta.empty())
			{
				return "";
			}

			std::string chunk = delta;
			if (!item_id.empty() && !session.codex_last_agent_message_item_id.empty() && session.codex_last_agent_message_item_id != item_id && CurrentAssistantMessageHasContent(chat, session) && !StartsWithLineBreak(delta))
			{
				chunk = "\n\n" + delta;
			}
			if (!item_id.empty())
			{
				session.codex_last_agent_message_item_id = item_id;
			}
			return AppendAssistantChunk(chat, session, chunk);
		}

		void RemoveCodexPlanDeltaEntryForItem(AcpSessionState& session, const std::string& item_id)
		{
			if (item_id.empty())
			{
				return;
			}
			std::erase_if(session.plan_entries, [&](const AcpPlanEntryState& entry) { return entry.priority == item_id; });
		}

		void HandleCodexToolItem(AcpSessionState& session, ChatSession& chat, const nlohmann::json& item)
		{
			const std::string item_id = JsonDiagnosticStringValue(item, "id");
			const std::string type = JsonDiagnosticStringValue(item, "type");
			if (item_id.empty())
			{
				return;
			}
			if (type == uam::acp_tool_items::kAgentMessage)
			{
				const std::string content = CodexItemContent(item);
				if (!content.empty())
				{
					AppendCodexAgentMessageText(chat, session, item_id, CodexCompletedAgentMessageDelta(session, item_id, content));
				}
				return;
			}
			if (type == uam::acp_tool_items::kReasoning)
			{
				(void)HandleCodexCompletedReasoningItem(chat, session, item);
				return;
			}
			if (type == uam::acp_tool_items::kPlan)
			{
				session.plan_summary = CodexItemContent(item);
				RemoveCodexPlanDeltaEntryForItem(session, item_id);
				AppendPlanTurnEventIfNeeded(session);
				(void)SyncAcpPlanToAssistantMessage(chat, session, true);
				return;
			}
			if (!uam::acp_tool_items::IsCodexToolItemType(type))
			{
				return;
			}

			AcpToolCallState& tool_call = UpsertToolCall(session, item_id);
			tool_call.title = CodexItemTitle(item);
			tool_call.kind = type;
			tool_call.status = JsonDiagnosticStringValueOr(item, "status", uam::acp_statuses::ExistingOrPending(tool_call.status));
			const std::string content = CodexItemContent(item);
			if (!content.empty())
			{
				tool_call.content = content;
			}
			AppendToolTurnEventIfNeeded(session, item_id);
			(void)SyncAcpToolCallsToAssistantMessage(chat, session, false);
		}

		void HandleCodexPendingPermission(AcpSessionState& session, ChatSession& chat, const nlohmann::json& message, const std::string& kind)
		{
			if (uam::AcpSessionHasPendingCancel(session))
			{
				AppendIgnoredRequestDuringCancelDiagnostic(session, message, "ignored_permission_during_cancel", "Ignoring permission request while a turn cancel is pending.");
				return;
			}

			const nlohmann::json params = JsonObjectValue(message, "params");
			AcpPendingPermissionState pending;
			pending.request_id_json = JsonRpcIdToStableString(JsonRpcIdOrNull(message));
			pending.provider_request_method = JsonDiagnosticStringValue(message, "method");
			pending.provider_request_kind = kind;
			pending.codex_approval_payload_json = params.dump();
			pending.tool_call_id = JsonDiagnosticStringValueOr(params, "itemId", pending.request_id_json);
			pending.status = uam::acp_statuses::kPending;

			if (kind == uam::acp_permissions::kCodexCommandRequestKind)
			{
				pending.title = "Command approval";
				pending.kind = uam::acp_tool_items::kCommandExecution;
				pending.content = JsonDiagnosticStringValueOr(params, "command", JsonDiagnosticStringValue(params, "reason"));
				const nlohmann::json decisions = JsonArrayValue(params, "availableDecisions");
				if (decisions.is_array())
				{
					for (const nlohmann::json& decision : decisions)
					{
						if (!decision.is_string())
						{
							continue;
						}
						const std::string_view id = decision.get_ref<const std::string&>();
						pending.options.push_back(AcpPermissionOptionState{std::string(id), uam::acp_permissions::CodexDecisionLabel(id), uam::acp_permissions::kDecisionOptionKind});
					}
				}
			}
			else if (kind == uam::acp_permissions::kCodexFileRequestKind)
			{
				pending.title = "File change approval";
				pending.kind = uam::acp_tool_items::kFileChange;
				pending.content = JsonDiagnosticStringValueOr(params, "reason", JsonDiagnosticStringValue(params, "grantRoot"));
			}
			else
			{
				pending.title = "Permission approval";
				pending.kind = uam::acp_permissions::kPermissionsToolKind;
				pending.content = JsonDiagnosticStringValue(params, "reason");
			}

			if (pending.options.empty())
			{
				pending.options.push_back(AcpPermissionOptionState{uam::acp_permissions::kAcceptDecision, "Allow", uam::acp_permissions::kDecisionOptionKind});
				pending.options.push_back(AcpPermissionOptionState{uam::acp_permissions::kDeclineDecision, "Deny", uam::acp_permissions::kDecisionOptionKind});
			}
			pending.options.push_back(AcpPermissionOptionState{uam::acp_permissions::kCancelledOptionId, "Cancel", uam::acp_permissions::kCancelOptionKind});

			if (!pending.tool_call_id.empty())
			{
				AcpToolCallState& tracked_tool_call = UpsertToolCall(session, pending.tool_call_id);
				tracked_tool_call.title = pending.title;
				tracked_tool_call.kind = pending.kind;
				tracked_tool_call.status = pending.status;
				tracked_tool_call.content = pending.content;
			}
			AppendPermissionTurnEventIfNeeded(session, pending.request_id_json, pending.tool_call_id);
			session.pending_permission = std::move(pending);
			if (TryAutoApprovePendingPermission(session, chat))
			{
				return;
			}
			session.waiting_for_permission = true;
			BeginAcpPendingWait(session, kAcpLifecycleWaitingPermission);
		}

			std::string CodexUserInputContent(const AcpPendingUserInputState& pending)
			{
				std::vector<std::string> question_blocks;
				question_blocks.reserve(pending.questions.size());
				for (const AcpUserInputQuestionState& question : pending.questions)
				{
					std::string question_block;
					if (!question.header.empty())
					{
						question_block = question.header + "\n";
					}
					question_block += question.question;
					question_blocks.push_back(question_block);
				}
				return uam::strings::Join(question_blocks, "\n\n");
			}

		void HandleCodexUserInputRequest(AcpSessionState& session, const nlohmann::json& message)
		{
			if (uam::AcpSessionHasPendingCancel(session))
			{
				AppendIgnoredRequestDuringCancelDiagnostic(session, message, "ignored_user_input_during_cancel", "Ignoring user input request while a turn cancel is pending.");
				return;
			}

			const nlohmann::json params = JsonObjectValue(message, "params");
			AcpPendingUserInputState pending;
			pending.request_id_json = JsonRpcIdToStableString(JsonRpcIdOrNull(message));
			pending.item_id = JsonDiagnosticStringValue(params, "itemId");
			pending.status = uam::acp_statuses::kPending;
			pending.attention_kind = NormalizeAcpAttentionKind(JsonDiagnosticStringValueOr(params, "attentionKind", JsonDiagnosticStringValueOr(params, "inputKind", JsonDiagnosticStringValue(params, "kind"))), "question");

			const nlohmann::json questions = JsonArrayValue(params, "questions");
			if (questions.is_array())
			{
				for (const nlohmann::json& question_json : questions)
				{
					if (!question_json.is_object())
					{
						continue;
					}

					AcpUserInputQuestionState question;
					question.id = JsonDiagnosticStringValue(question_json, "id");
					question.header = JsonDiagnosticStringValue(question_json, "header");
					question.question = JsonDiagnosticStringValue(question_json, "question");
					question.is_other = JsonBooleanValueOr(question_json, "isOther", false);
					question.is_secret = JsonBooleanValueOr(question_json, "isSecret", false);

					const nlohmann::json options = JsonArrayValue(question_json, "options");
					if (options.is_array())
					{
						for (const nlohmann::json& option_json : options)
						{
							if (!option_json.is_object())
							{
								continue;
							}

							AcpUserInputOptionState option;
							option.label = JsonDiagnosticStringValue(option_json, "label");
							option.description = JsonDiagnosticStringValue(option_json, "description");
							if (!option.label.empty() || !option.description.empty())
							{
								question.options.push_back(std::move(option));
							}
						}
					}

					if (!question.id.empty())
					{
						pending.questions.push_back(std::move(question));
					}
				}
			}

			if (!pending.item_id.empty())
			{
				AcpToolCallState& tracked_tool_call = UpsertToolCall(session, pending.item_id);
				tracked_tool_call.title = "User input";
				tracked_tool_call.kind = uam::acp_tool_items::kUserInput;
				tracked_tool_call.status = pending.status;
				tracked_tool_call.content = CodexUserInputContent(pending);
			}

			AppendUserInputTurnEventIfNeeded(session, pending.request_id_json, pending.item_id);
			session.pending_user_input = std::move(pending);
			session.waiting_for_user_input = true;
			BeginAcpPendingWait(session, kAcpLifecycleWaitingUserInput);
		}

		void HandleCodexMessage(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message, CefRefPtr<CefBrowser> browser)
		{
			const std::string method = JsonDiagnosticStringValue(message, "method");
			const nlohmann::json params = JsonObjectValue(message, "params");

			if (method == uam::acp_methods::kTurnStarted)
			{
				const nlohmann::json turn = JsonObjectValue(params, "turn");
				if (turn.is_object())
				{
					session.codex_turn_id = JsonDiagnosticStringValueOr(turn, "id", session.codex_turn_id);
				}
				session.lifecycle_state = kAcpLifecycleProcessing;
				return;
			}
			if (method == uam::acp_methods::kTurnCompleted)
			{
				const nlohmann::json turn = JsonObjectValue(params, "turn");
				const nlohmann::json error = JsonObjectValue(turn, "error");
				const std::string turn_status = JsonDiagnosticStringValue(turn, "status");
				if ((error.is_object() && !error.empty()) || uam::acp_statuses::IsFailedStatus(turn_status))
				{
					nlohmann::json error_params = {
					    {"willRetry", false},
					    {"threadId", JsonDiagnosticStringValue(params, "threadId")},
					    {"turnId", JsonDiagnosticStringValue(params, "turnId")},
					};
					if (JsonDiagnosticStringValue(error_params, "turnId").empty() && turn.is_object())
					{
						error_params["turnId"] = JsonDiagnosticStringValue(turn, "id");
					}
					const nlohmann::json normalized_error = error.is_object() ? error : nlohmann::json::object();
					const std::string error_message = CodexTurnErrorMessage(normalized_error);
					const std::string detail = CodexTurnErrorDetails(session, error_params, normalized_error);
					AppendAcpDiagnostic(session, "notification", "codex_turn_completed_error", method, "", false, 0, error_message, detail);
					(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
					AcpFailureDetails failure;
					failure.method = method;
					failure.message = error_message;
					failure.has_detail = !detail.empty();
					FailAcpTurnOrSession(session, FormatAcpFailureMessage(session, failure));
					SaveChatQuietly(app, chat);
					MarkAcpChatUnseenIfBackground(app, chat);
					return;
				}
				(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
				CompletePromptTurnAndHandleGoalLoop(app, session, chat, kAcpLifecycleReady, browser);
				if (browser)
				{
					uam::PushStreamDone(browser, chat.id);
				}
				SaveChatQuietly(app, chat);
				MarkAcpChatUnseenIfBackground(app, chat);
				return;
			}
			if (method == uam::acp_methods::kItemAgentMessageDelta)
			{
				const std::string item_id = JsonDiagnosticStringValue(params, "itemId");
				const std::string delta = CodexStreamedAgentMessageDelta(session, item_id, JsonDiagnosticStringValue(params, "delta"));
				const std::string appended = AppendCodexAgentMessageText(chat, session, item_id, delta);
				if (browser && !appended.empty())
				{
					uam::PushStreamToken(browser, chat.id, appended);
				}
				ScheduleChatSave(app, chat, 0.5);
				return;
			}
			if (method == uam::acp_methods::kItemReasoningTextDelta)
			{
				const std::string delta = JsonDiagnosticStringValue(params, "delta");
				if (AppendCodexReasoningThought(chat, session, JsonDiagnosticStringValue(params, "itemId"), "Reasoning", delta, JsonIntValueOr(params, "contentIndex", -1), true))
				{
					if (browser && !delta.empty())
					{
						uam::PushStreamToken(browser, chat.id, delta);
					}
						ScheduleChatSave(app, chat, 0.5);
				}
				return;
			}
			if (method == uam::acp_methods::kItemReasoningSummaryTextDelta)
			{
				const std::string delta = JsonDiagnosticStringValue(params, "delta");
				if (AppendCodexReasoningThought(chat, session, JsonDiagnosticStringValue(params, "itemId"), "Summary", delta, JsonIntValueOr(params, "summaryIndex", -1), true))
				{
					if (browser && !delta.empty())
					{
						uam::PushStreamToken(browser, chat.id, delta);
					}
						ScheduleChatSave(app, chat, 0.5);
				}
				return;
			}
			if (method == uam::acp_methods::kItemReasoningSummaryPartAdded)
			{
				return;
			}
			if (method == uam::acp_methods::kItemPlanDelta)
			{
				const std::string item_id = JsonDiagnosticStringValue(params, "itemId");
				if (item_id.empty())
				{
					return;
				}
				AcpPlanEntryState* entry = nullptr;
				for (AcpPlanEntryState& existing : session.plan_entries)
				{
					if (existing.priority == item_id)
					{
						entry = &existing;
						break;
					}
				}
				if (entry == nullptr)
				{
					AcpPlanEntryState created;
					created.priority = item_id;
					created.status = uam::acp_statuses::kPending;
					session.plan_entries.push_back(std::move(created));
					entry = &session.plan_entries.back();
				}
				entry->content += JsonDiagnosticStringValue(params, "delta");
				AppendPlanTurnEventIfNeeded(session);
				(void)SyncAcpPlanToAssistantMessage(chat, session, true);
				SaveChatQuietly(app, chat);
				return;
			}
			if (method == uam::acp_methods::kTurnPlanUpdated)
			{
				session.plan_summary = JsonDiagnosticStringValue(params, "explanation");
				const nlohmann::json plan = JsonArrayValue(params, "plan");
				if (plan.is_array())
				{
					session.plan_entries.clear();
					for (const nlohmann::json& step : plan)
					{
						if (!step.is_object())
						{
							continue;
						}
						AcpPlanEntryState entry;
						entry.content = JsonDiagnosticStringValue(step, "step");
						entry.status = JsonDiagnosticStringValue(step, "status");
						session.plan_entries.push_back(std::move(entry));
					}
				}
				AppendPlanTurnEventIfNeeded(session);
				if (SyncAcpPlanToAssistantMessage(chat, session, true))
				{
					SaveChatQuietly(app, chat);
				}
				return;
			}
			if (uam::acp_methods::IsCodexItemLifecycleMethod(method))
			{
				HandleCodexToolItem(session, chat, JsonObjectValue(params, "item"));
				SaveChatQuietly(app, chat);
				return;
			}
			if (uam::acp_methods::IsCodexToolOutputDeltaMethod(method))
			{
				const std::string item_id = JsonDiagnosticStringValue(params, "itemId");
				if (!item_id.empty())
				{
					AcpToolCallState& tool_call = UpsertToolCall(session, item_id);
					const bool is_file_change = uam::acp_methods::IsCodexFileChangeOutputDeltaMethod(method);
					if (tool_call.title.empty())
					{
						tool_call.title = is_file_change ? "File changes" : "Command output";
					}
					if (tool_call.kind.empty())
					{
						tool_call.kind = is_file_change ? uam::acp_tool_items::kFileChange : uam::acp_tool_items::kCommandExecution;
					}
					if (tool_call.status.empty())
					{
						tool_call.status = uam::acp_statuses::kRunning;
					}
					tool_call.content += JsonDiagnosticStringValue(params, "delta");
					AppendToolTurnEventIfNeeded(session, item_id);
					(void)SyncAcpToolCallsToAssistantMessage(chat, session, false);
					SaveChatQuietly(app, chat);
				}
				return;
			}
			if (method == uam::acp_methods::kItemCommandExecutionRequestApproval)
			{
				HandleCodexPendingPermission(session, chat, message, uam::acp_permissions::kCodexCommandRequestKind);
				return;
			}
			if (method == uam::acp_methods::kItemFileChangeRequestApproval)
			{
				HandleCodexPendingPermission(session, chat, message, uam::acp_permissions::kCodexFileRequestKind);
				return;
			}
			if (method == uam::acp_methods::kItemPermissionsRequestApproval)
			{
				HandleCodexPendingPermission(session, chat, message, uam::acp_permissions::kCodexPermissionsRequestKind);
				return;
			}
			if (method == uam::acp_methods::kItemToolRequestUserInput)
			{
				HandleCodexUserInputRequest(session, message);
				(void)SyncAcpToolCallsToAssistantMessage(chat, session, false);
				(void)SyncCurrentAssistantMessageBlocksFromTurnEvents(chat, session);
				SaveChatQuietly(app, chat);
				return;
			}
			if (method == uam::acp_methods::kError)
			{
				const nlohmann::json error = JsonObjectValue(params, "error");
				const std::string error_message = CodexTurnErrorMessage(error);
				const std::string detail = CodexTurnErrorDetails(session, params, error);
				const bool will_retry = JsonBooleanValueOr(params, "willRetry", false);
				AppendAcpDiagnostic(session, "notification", will_retry ? "codex_turn_error_retrying" : "codex_turn_error", method, "", false, 0, error_message, detail);
				if (will_retry)
				{
					session.lifecycle_state = kAcpLifecycleProcessing;
					return;
				}
				(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
				AcpFailureDetails failure;
				failure.method = "turn";
				failure.message = error_message;
				failure.has_detail = !detail.empty();
				FailAcpTurnOrSession(session, FormatAcpFailureMessage(session, failure));
				SaveChatQuietly(app, chat);
				MarkAcpChatUnseenIfBackground(app, chat);
				return;
			}
			if (uam::acp_methods::IsIgnoredCodexAppServerMethod(method))
			{
				return;
			}

			if (uam::nlohmann_json::FindField(message, "id") != nullptr)
			{
				const nlohmann::json request_id = JsonRpcIdOrNull(message);
				AppendAcpDiagnostic(session, "request", "unsupported_method", method, JsonRpcIdToStableString(request_id), true, -32601, "UAM Codex app-server client does not implement method: " + method);
				SendJsonRpcError(session, request_id, -32601, "UAM Codex app-server client does not implement method: " + method);
			}
		}

		void HandleAcpRequest(AppState&, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message)
		{
			const std::string method = JsonDiagnosticStringValue(message, "method");
			if (method == uam::acp_methods::kSessionUpdate)
			{
				return;
			}

			if (method == uam::acp_methods::kSessionRequestPermission)
			{
				HandlePermissionRequest(session, chat, message);
				return;
			}

			if (uam::nlohmann_json::FindField(message, "id") != nullptr)
			{
				const nlohmann::json request_id = JsonRpcIdOrNull(message);
				AppendAcpDiagnostic(session, "request", "unsupported_method", method, JsonRpcIdToStableString(request_id), true, -32601, "UAM ACP client does not implement method: " + method);
				SendJsonRpcError(session, request_id, -32601, "UAM ACP client does not implement method: " + method);
			}
		}

			std::string PendingRequestSummary(const AcpSessionState& session)
			{
				if (session.pending_request_methods.empty())
				{
					return "";
				}

				std::vector<std::string> pending_requests;
				pending_requests.reserve(session.pending_request_methods.size());
				for (const auto& entry : session.pending_request_methods)
				{
					pending_requests.push_back(std::to_string(entry.first) + ":" + entry.second);
				}
				return uam::strings::JoinNonEmpty(pending_requests, ", ");
			}

		std::string ErrorDataForDiagnostics(const nlohmann::json& error)
		{
			const nlohmann::json* data = uam::nlohmann_json::FindField(error, "data");
			if (data == nullptr)
			{
				return "";
			}
			return CapDiagnosticString(data->dump(), kMaxAcpDiagnosticDetailBytes);
		}

		void UpdateAcpModesFromJson(AcpSessionState& session, const nlohmann::json& modes)
		{
			if (!modes.is_object())
			{
				return;
			}

			if (const nlohmann::json* available_modes = uam::nlohmann_json::FindArrayField(modes, "availableModes"); available_modes != nullptr)
			{
				session.available_modes.clear();
				for (const nlohmann::json& mode : *available_modes)
				{
					const std::string provider_mode_id = uam::nlohmann_json::TrimmedStringValue(mode, {"id"});
					if (uam::approval_modes::IsSuppressedProviderApprovalMode(provider_mode_id))
					{
						continue;
					}
					AcpModeState parsed;
					parsed.id = AppApprovalModeId(provider_mode_id);
					parsed.name = uam::nlohmann_json::TrimmedStringValue(mode, {"name"});
					if (parsed.name.empty())
					{
						parsed.name = parsed.id;
					}
					parsed.description = uam::nlohmann_json::TrimmedStringValue(mode, {"description"});
					if (!parsed.id.empty())
					{
						session.available_modes.push_back(std::move(parsed));
					}
				}
			}

			const std::string current_mode_id = uam::nlohmann_json::TrimmedStringValue(modes, {"currentModeId"});
			if (!current_mode_id.empty())
			{
				session.current_mode_id = AppApprovalModeId(current_mode_id);
			}
		}

		void UpdateAcpModelsFromJson(AcpSessionState& session, const nlohmann::json& models)
		{
			if (!models.is_object())
			{
				return;
			}

			if (const nlohmann::json* available_models = uam::nlohmann_json::FindArrayField(models, "availableModels"); available_models != nullptr)
			{
				session.available_models.clear();
				for (const nlohmann::json& model : *available_models)
				{
					if (std::optional<AcpModelState> parsed = uam::acp_models::ParseAcpModelState(model))
					{
						session.available_models.push_back(std::move(*parsed));
					}
				}
			}

			const std::string current_model_id = uam::nlohmann_json::TrimmedStringValue(models, {"currentModelId"});
			if (!current_model_id.empty())
			{
				session.current_model_id = current_model_id;
			}
		}

		void HandleAcpResponse(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message)
		{
			const nlohmann::json response_id = JsonRpcIdOrNull(message);
			const std::string request_id = JsonRpcIdToStableString(response_id);
			const int id = JsonRpcNumericId(response_id);
			if (id == 0)
			{
				AppendAcpDiagnostic(session, "response", "ignored_invalid_id", "", request_id, false, 0, "", CapDiagnosticString(message.dump(), kMaxAcpDiagnosticDetailBytes));
				return;
			}

			std::string method;
			if (const auto it = session.pending_request_methods.find(id); it != session.pending_request_methods.end())
			{
				method = it->second;
				session.pending_request_methods.erase(it);
			}
			if (session.prompt_request_id != 0 && id == session.prompt_request_id)
			{
				method = IsCodexSession(session) ? uam::acp_methods::kTurnStart : uam::acp_methods::kSessionPrompt;
			}

			if (const nlohmann::json* error_ptr = uam::nlohmann_json::FindField(message, "error"))
			{
				const nlohmann::json& error = *error_ptr;
				const nlohmann::json* code_json = uam::nlohmann_json::FindField(error, "code");
				const std::optional<int> parsed_code = code_json == nullptr ? std::nullopt : uam::nlohmann_json::IntValueStrict(*code_json);
				const bool has_code = parsed_code.has_value();
				const int code = parsed_code.value_or(0);
				const std::string default_error = std::string(RuntimeDisplayName(session)) + " request failed.";
				const std::string error_message = error.is_object() ? JsonDiagnosticStringValueOr(error, "message", default_error) : default_error;
				const std::string error_data = ErrorDataForDiagnostics(error);
				std::ostringstream detail;
				bool has_detail = false;
				if (!error_data.empty())
				{
					detail << "error.data=" << error_data;
					has_detail = true;
				}
				if (!session.recent_stderr.empty())
				{
					if (has_detail)
					{
						detail << "\n";
					}
					detail << "stderr_tail=" << RecentStderrTail(session);
					has_detail = true;
				}
				const std::string pending_summary = PendingRequestSummary(session);
				if (!pending_summary.empty())
				{
					if (has_detail)
					{
						detail << "\n";
					}
					detail << "pending_requests=" << pending_summary;
					has_detail = true;
				}
				const std::string detail_text = detail.str();
				AcpFailureDetails failure;
				failure.method = method;
				failure.request_id = request_id;
				failure.has_code = has_code;
				failure.code = code;
				failure.message = error_message;
				failure.has_detail = !detail_text.empty();
				const std::string formatted_error = FormatAcpFailureMessage(session, failure);
				AppendAcpDiagnostic(session, "response", "jsonrpc_error", method, request_id, has_code, code, error_message, detail_text);
				if (IsCodexSession(session) && method == uam::acp_methods::kThreadResume && has_code && code == -32600 && uam::codex::ErrorLooksLikeInvalidThreadId(error_message) && !session.codex_resume_fallback_attempted)
				{
					session.codex_resume_fallback_attempted = true;
					session.session_setup_request_id = 0;
					session.session_id.clear();
					session.codex_thread_id.clear();
					chat.native_session_id.clear();
					SaveChatQuietly(app, chat);
					AppendAcpDiagnostic(session, "response", "codex_invalid_resume_id_retry_start", method, request_id, has_code, code, "Codex rejected the stored thread id. Starting a new thread instead.", detail_text);

					const std::filesystem::path workspace_root = uam::paths::ResolveWorkspaceRootPath(app, chat);
					const std::string cwd = AcpWorkingDirectoryString(workspace_root);
					const int retry_id = NextAcpRequestId(session, uam::acp_methods::kThreadStart);
					session.session_setup_request_id = retry_id;
					session.lifecycle_state = kAcpLifecycleStarting;
					if (!WriteAcpMessage(session, BuildCodexThreadStartRequest(retry_id, chat, cwd)))
					{
						session.pending_request_methods.erase(retry_id);
						session.session_setup_request_id = 0;
						(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
						FailAcpTurnOrSession(session, uam::strings::NonEmptyOrFallback(session.last_error, formatted_error));
						SaveChatQuietly(app, chat);
						MarkAcpChatUnseenIfBackground(app, chat);
					}
					return;
				}
				AcpInvalidLoadRetryDetails invalid_load_retry;
				invalid_load_retry.failure = failure;
				invalid_load_retry.error_data = error_data;
				invalid_load_retry.detail_text = detail_text;
				invalid_load_retry.formatted_error = formatted_error;
				if (RetryGeminiSessionNewAfterInvalidLoad(app, session, chat, invalid_load_retry))
				{
					return;
				}
				if (method == uam::acp_methods::kSessionSetModel && id == session.startup_model_request_id)
				{
					ClearAcpStartupModelRequest(session);
				}
				if (method == uam::acp_methods::kSessionPrompt || session.processing || session.waiting_for_permission || session.waiting_for_user_input || !session.queued_prompt.empty())
				{
					(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
					FailAcpTurnOrSession(session, formatted_error);
					SaveChatQuietly(app, chat);
					MarkAcpChatUnseenIfBackground(app, chat);
				}
				else
				{
					session.last_error = formatted_error;
					session.lifecycle_state = kAcpLifecycleError;
				}
				return;
			}

			const nlohmann::json result = uam::nlohmann_json::ValueOrNull(uam::nlohmann_json::FindField(message, "result"));
			if (uam::acp_methods::IsLifecycleResultMethod(method))
			{
				AppendAcpDiagnostic(session, "response", "jsonrpc_result", method, request_id, false, 0, "", "result=" + CapDiagnosticString(result.dump(), kMaxAcpDiagnosticDetailBytes));
			}
			if (method == uam::acp_methods::kInitialize)
			{
				session.initialize_request_id = 0;
				session.initialized = true;
				session.lifecycle_state = kAcpLifecycleStarting;
				if (IsCodexSession(session))
				{
					session.agent_name = "codex";
					session.agent_title = "Codex";
					if (result.is_object())
					{
						session.agent_version = JsonDiagnosticStringValue(result, "userAgent");
					}
					session.load_session_supported = true;
					(void)WriteAcpMessage(session, BuildCodexInitializedNotification());
					const int model_list_id = NextAcpRequestId(session, uam::acp_methods::kModelList);
					(void)WriteAcpMessage(session, BuildCodexModelListRequest(model_list_id));
					return;
				}
				if (result.is_object())
				{
					const nlohmann::json agent_info = JsonObjectValue(result, "agentInfo");
					if (agent_info.is_object())
					{
						session.agent_name = JsonDiagnosticStringValue(agent_info, "name");
						session.agent_title = JsonDiagnosticStringValue(agent_info, "title");
						session.agent_version = JsonDiagnosticStringValue(agent_info, "version");
					}
					const nlohmann::json agent_capabilities = JsonObjectValue(result, "agentCapabilities");
					if (agent_capabilities.is_object())
					{
						session.load_session_supported = JsonBooleanValueOr(agent_capabilities, "loadSession", false);
					}
				}
				return;
			}

			if (method == uam::acp_methods::kModelList)
			{
				if (result.is_object())
				{
					const nlohmann::json data = JsonArrayValue(result, "data");
					if (data.is_array())
					{
						session.available_models.clear();
						std::vector<std::string> seen_model_ids;
						uam::acp_models::CodexModelParseOptions parse_options;
						parse_options.skip_hidden_field = true;
						parse_options.allow_default_non_list_visibility = true;
						for (const nlohmann::json& model : data)
						{
							const auto parsed = uam::acp_models::ParseCodexModelEntry(model, parse_options);
							if (!parsed)
							{
								continue;
							}
							if (uam::ranges::Contains(seen_model_ids, parsed->model.id))
							{
								continue;
							}

							if (parsed->is_default)
							{
								session.current_model_id = parsed->model.id;
							}
							seen_model_ids.push_back(parsed->model.id);
							session.available_models.push_back(std::move(parsed->model));
						}

						const std::string explicit_current_model = uam::nlohmann_json::TrimmedStringValue(result, {"currentModelId", "model"});
						if (!explicit_current_model.empty())
						{
							session.current_model_id = explicit_current_model;
						}
					}
				}
				return;
			}

			if (uam::acp_methods::IsCodexThreadSetupMethod(method))
			{
				session.session_setup_request_id = 0;
				std::string returned_thread_id;
				if (result.is_object())
				{
					const nlohmann::json thread = JsonObjectValue(result, "thread");
					if (thread.is_object())
					{
						returned_thread_id = JsonDiagnosticStringValue(thread, "id");
					}
					session.current_model_id = uam::nlohmann_json::TrimmedStringValueOr(result, "model", session.current_model_id);
				}
				if (uam::codex::IsValidThreadId(returned_thread_id))
				{
					session.codex_thread_id = returned_thread_id;
					session.session_id = session.codex_thread_id;
				}
				else
				{
					session.codex_thread_id.clear();
					session.session_id.clear();
				}
				const std::string previous_native_session_id = chat.native_session_id;
				SetChatNativeSessionIdIfChanged(chat, session.session_id);
				SyncResolvedNativeSessionIdForChat(app, chat, session.session_id, previous_native_session_id);
				session.available_modes = {
				    AcpModeState{uam::approval_modes::kDefaultApprovalMode, "Default", "Use Codex default collaboration mode."},
				    AcpModeState{uam::approval_modes::kPlanApprovalMode, "Plan", "Ask Codex to plan before implementing."},
				};
				session.current_mode_id = chat.approval_mode.empty() ? uam::approval_modes::kDefaultApprovalMode : chat.approval_mode;
				session.session_ready = !session.session_id.empty();
				session.lifecycle_state = session.session_ready ? kAcpLifecycleReady : kAcpLifecycleError;
				if (!session.session_ready)
				{
					const std::string detail = "result=" + CapDiagnosticString(result.dump(), kMaxAcpDiagnosticDetailBytes) + (session.recent_stderr.empty() ? "" : "\nstderr_tail=" + RecentStderrTail(session));
					AcpFailureDetails failure;
					failure.method = method;
					failure.request_id = request_id;
					failure.message = "Codex app-server did not return a valid thread id.";
					failure.has_detail = true;
					session.last_error = FormatAcpFailureMessage(session, failure);
					AppendAcpDiagnostic(session, "response", "missing_thread_id", method, request_id, false, 0, session.last_error, detail);
				}
				SaveChatQuietly(app, chat);
				return;
			}

			if (method == uam::acp_methods::kTurnStart)
			{
				session.prompt_request_id = 0;
				if (result.is_object())
				{
					const nlohmann::json turn = JsonObjectValue(result, "turn");
					if (turn.is_object())
					{
						session.codex_turn_id = JsonDiagnosticStringValueOr(turn, "id", session.codex_turn_id);
					}
				}
				session.lifecycle_state = kAcpLifecycleProcessing;
				return;
			}

			if (method == uam::acp_methods::kSessionNew)
			{
				session.session_setup_request_id = 0;
				if (result.is_object())
				{
					session.session_id = uam::nlohmann_json::TrimmedStringValueOr(result, "sessionId", session.session_id);
					UpdateAcpModesFromJson(session, JsonObjectValue(result, "modes"));
					UpdateAcpModelsFromJson(session, JsonObjectValue(result, "models"));
				}
				const std::string previous_native_session_id = chat.native_session_id;
				SetChatNativeSessionIdIfChanged(chat, session.session_id);
				SyncResolvedNativeSessionIdForChat(app, chat, session.session_id, previous_native_session_id);
				session.session_ready = !session.session_id.empty();
				session.lifecycle_state = session.session_ready ? kAcpLifecycleReady : kAcpLifecycleError;
				if (!session.session_ready)
				{
					const std::string detail = "result=" + CapDiagnosticString(result.dump(), kMaxAcpDiagnosticDetailBytes) + (session.recent_stderr.empty() ? "" : "\nstderr_tail=" + RecentStderrTail(session));
					AcpFailureDetails failure;
					failure.method = method;
					failure.request_id = request_id;
					failure.message = std::string(RuntimeDisplayName(session)) + " did not return a session id.";
					failure.has_detail = true;
					session.last_error = FormatAcpFailureMessage(session, failure);
					AppendAcpDiagnostic(session, "response", "missing_session_id", method, request_id, false, 0, session.last_error, detail);
				}
				SaveChatQuietly(app, chat);
				return;
			}

			if (method == uam::acp_methods::kSessionLoad)
			{
				session.session_setup_request_id = 0;
				if (result.is_object())
				{
					UpdateAcpModesFromJson(session, JsonObjectValue(result, "modes"));
					UpdateAcpModelsFromJson(session, JsonObjectValue(result, "models"));
				}
				session.session_ready = true;
				session.ignore_session_updates_until_ready = false;
				session.lifecycle_state = kAcpLifecycleReady;
				return;
			}

			if (method == uam::acp_methods::kSessionPrompt)
			{
				(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
				CompletePromptTurnAndHandleGoalLoop(app, session, chat, kAcpLifecycleReady, nullptr);
				SaveChatQuietly(app, chat);
				MarkAcpChatUnseenIfBackground(app, chat);
				return;
			}

			if (method == uam::acp_methods::kSessionCancel)
			{
				session.cancel_requested = false;
				session.cancel_request_id = 0;
				return;
			}

			if (method == uam::acp_methods::kTurnInterrupt)
			{
				session.cancel_requested = false;
				session.cancel_request_id = 0;
				session.codex_turn_id.clear();
				return;
			}

			if (uam::acp_methods::IsSessionModeOrModelUpdateMethod(method))
			{
				if (method == uam::acp_methods::kSessionSetModel && JsonRpcNumericId(JsonRpcIdOrNull(message)) == session.startup_model_request_id)
				{
					ClearAcpStartupModelRequest(session);
				}
				return;
			}

			AppendAcpDiagnostic(session, "response", "unknown_request_id", method, request_id, false, 0, "", CapDiagnosticString(message.dump(), kMaxAcpDiagnosticDetailBytes));
		}

		void HandleClaudeAssistantMessage(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message, CefRefPtr<CefBrowser> browser)
		{
			const nlohmann::json assistant_message = JsonObjectValue(message, "message");
			const nlohmann::json content = JsonArrayValue(assistant_message, "content");
			if (!content.is_array())
			{
				const std::string fallback_text = ClaudeContentTextFromMessage(assistant_message);
				if (!fallback_text.empty())
				{
					const std::string appended = AppendAssistantChunk(chat, session, fallback_text);
					if (browser && !appended.empty())
					{
						uam::PushStreamToken(browser, chat.id, appended);
					}
						ScheduleChatSave(app, chat, 0.5);
				}
				return;
			}

			bool changed = false;
			for (const nlohmann::json& item : content)
			{
				if (!item.is_object())
				{
					continue;
				}

				const std::string type = JsonDiagnosticStringValue(item, "type");
				if (type == uam::acp_content::kTextType)
				{
					const std::string text = ContentTextFromJson(item);
					if (!text.empty())
					{
						const std::string appended = AppendAssistantChunk(chat, session, text);
						if (browser && !appended.empty())
						{
							uam::PushStreamToken(browser, chat.id, appended);
						}
						changed = true;
					}
					continue;
				}

				if (type == uam::acp_claude_stream::kContentThinking)
				{
					const std::string thought = ContentTextFromJson(item);
					if (!thought.empty())
					{
						changed = AppendThoughtChunk(chat, session, thought) || changed;
						if (browser)
						{
							uam::PushStreamToken(browser, chat.id, thought);
						}
					}
					continue;
				}

				if (type == uam::acp_claude_stream::kContentToolUse)
				{
					const std::string tool_id = JsonDiagnosticStringValue(item, "id");
					if (tool_id.empty())
					{
						continue;
					}

					AcpToolCallState& tool_call = UpsertToolCall(session, tool_id);
					tool_call.kind = JsonDiagnosticStringValueOr(item, "name", tool_call.kind);
					tool_call.title = tool_call.kind;
					tool_call.status = uam::acp_statuses::kRunning;
					if (const nlohmann::json* input = uam::nlohmann_json::FindField(item, "input"); input != nullptr)
					{
						tool_call.content = "Arguments:\n" + CapDiagnosticString(input->dump(), kMaxAcpDiagnosticDetailBytes);
					}
					AppendToolTurnEventIfNeeded(session, tool_id);
					changed = SyncAcpToolCallsToAssistantMessage(chat, session, true) || changed;
				}
			}

			if (changed)
			{
				SaveChatQuietly(app, chat);
				MarkAcpChatUnseenIfBackground(app, chat);
			}
		}

		void HandleClaudeUserMessage(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message)
		{
			const nlohmann::json user_message = JsonObjectValue(message, "message");
			const nlohmann::json content = JsonArrayValue(user_message, "content");
			if (!content.is_array())
			{
				return;
			}

			bool changed = false;
			for (const nlohmann::json& item : content)
			{
				if (!item.is_object())
				{
					continue;
				}

				if (JsonDiagnosticStringValue(item, "type") != uam::acp_claude_stream::kContentToolResult)
				{
					continue;
				}

				const std::string tool_id = JsonDiagnosticStringValue(item, "tool_use_id");
				if (tool_id.empty())
				{
					continue;
				}

				AcpToolCallState& tool_call = UpsertToolCall(session, tool_id);
				tool_call.status = JsonBooleanValueOr(item, "is_error", false) ? uam::acp_statuses::kFailed : uam::acp_statuses::kCompleted;
				const nlohmann::json* content_value = uam::nlohmann_json::FindField(item, "content");
				const std::string result_text = ContentTextFromJson(content_value == nullptr ? nlohmann::json::array() : *content_value);
				if (tool_call.content.empty())
				{
					tool_call.content = result_text;
				}
				else if (!result_text.empty())
				{
					tool_call.content += "\n\nResult:\n" + result_text;
				}
				AppendToolTurnEventIfNeeded(session, tool_id);
				changed = SyncAcpToolCallsToAssistantMessage(chat, session, true) || changed;
			}

			if (changed)
			{
				SaveChatQuietly(app, chat);
			}
		}

		void HandleClaudeResult(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message, CefRefPtr<CefBrowser> browser)
		{
			const std::string session_id = uam::nlohmann_json::TrimmedStringValueOr(message, "session_id", "");
			if (!session_id.empty())
			{
				session.session_id = session_id;
				const std::string previous_native_session_id = chat.native_session_id;
				SetChatNativeSessionIdIfChanged(chat, session_id);
				SyncResolvedNativeSessionIdForChat(app, chat, session_id, previous_native_session_id);
			}

			const std::string model_id = uam::nlohmann_json::TrimmedStringValueOr(message, "model", "");
			if (!model_id.empty())
			{
				session.current_model_id = model_id;
			}

			if (session.available_models.empty() && !session.current_model_id.empty())
			{
				session.available_models.push_back(AcpModelState{session.current_model_id, session.current_model_id, ""});
			}

			if (session.turn_assistant_message_index < 0)
			{
				const std::string result_text = uam::nlohmann_json::TrimmedStringValueOr(message, "result", "");
				if (!result_text.empty())
				{
					AppendAssistantChunk(chat, session, result_text);
				}
			}

			(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
			const bool is_error = JsonBooleanValueOr(message, "is_error", false);
			const std::string subtype = JsonDiagnosticStringValue(message, "subtype");
			if (is_error || uam::acp_claude_stream::IsResultErrorSubtype(subtype))
			{
				const std::string result_text = uam::nlohmann_json::TrimmedStringValueOr(message, "result", "");
				FailAcpTurnOrSession(session, uam::strings::NonEmptyOrFallback(result_text, "Claude stream-json turn failed."));
			}
			else
			{
				CompletePromptTurnAndHandleGoalLoop(app, session, chat, kAcpLifecycleReady, browser);
			}

			if (browser)
			{
				uam::PushStreamDone(browser, chat.id);
			}
			SaveChatQuietly(app, chat);
			MarkAcpChatUnseenIfBackground(app, chat);
		}

		void HandleClaudeMessage(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message, CefRefPtr<CefBrowser> browser)
		{
			const std::string type = JsonDiagnosticStringValue(message, "type");
			if (type == uam::acp_claude_stream::kMessageTypeSystem && JsonDiagnosticStringValue(message, "subtype") == uam::acp_claude_stream::kSubtypeInit)
			{
				session.initialized = true;
				const std::string session_id = uam::nlohmann_json::TrimmedStringValueOr(message, "session_id", "");
				if (!session_id.empty())
				{
					session.session_id = session_id;
					if (SetChatNativeSessionIdIfChanged(chat, session_id))
					{
						const std::string previous_native_session_id = chat.native_session_id;
						SyncResolvedNativeSessionIdForChat(app, chat, session_id, previous_native_session_id);
						SaveChatQuietly(app, chat);
					}
				}

				session.current_model_id = uam::nlohmann_json::TrimmedStringValueOr(message, "model", session.current_model_id);
				session.current_mode_id = uam::nlohmann_json::TrimmedStringValueOr(message, "permissionMode", uam::strings::NonEmptyOrFallback(session.current_mode_id, uam::approval_modes::kDefaultApprovalMode));
				if (!session.current_model_id.empty() && session.available_models.empty())
				{
					session.available_models.push_back(AcpModelState{session.current_model_id, session.current_model_id, ""});
				}
				return;
			}

			if (type == uam::acp_claude_stream::kMessageTypeAssistant)
			{
				HandleClaudeAssistantMessage(app, session, chat, message, browser);
				return;
			}

			if (type == uam::acp_claude_stream::kMessageTypeUser)
			{
				HandleClaudeUserMessage(app, session, chat, message);
				return;
			}

			if (type == uam::acp_claude_stream::kMessageTypeResult)
			{
				HandleClaudeResult(app, session, chat, message, browser);
				return;
			}

			AppendAcpDiagnostic(session, "message", "ignored_claude_message", "", "", false, 0, "", CapDiagnosticString(message.dump(), kMaxAcpDiagnosticDetailBytes));
		}

		bool ProcessAcpLine(AppState& app, AcpSessionState& session, ChatSession& chat, const std::string& line, CefRefPtr<CefBrowser> browser)
		{
			const std::string trimmed = uam::strings::Trim(line);
			if (trimmed.empty())
			{
				return false;
			}

			nlohmann::json message;
			try
			{
				message = nlohmann::json::parse(trimmed);
			}
			catch (const std::exception& ex)
			{
				const std::string error_message = std::string("Invalid JSON from ") + RuntimeDisplayName(session) + ": " + ex.what();
				AppendAcpDiagnostic(session, "parse", "invalid_json", "", "", false, 0, error_message, CapDiagnosticString(trimmed, kMaxAcpDiagnosticDetailBytes));
				FailAcpTurnOrSession(session, error_message);
				MarkAcpChatUnseenIfBackground(app, chat);
				return true;
			}

			if (IsClaudeSession(session))
			{
				try
				{
					HandleClaudeMessage(app, session, chat, message, browser);
				}
				catch (const std::exception& ex)
				{
					const std::string error_message = std::string("Claude stream-json message handling failed: ") + ex.what();
					AppendAcpDiagnostic(session, "parse", "claude_message_parse_error", "", "", false, 0, error_message, CapDiagnosticString(message.dump(), kMaxAcpDiagnosticDetailBytes));
					FailAcpTurnOrSession(session, error_message);
					MarkAcpChatUnseenIfBackground(app, chat);
				}
				return true;
			}

			if (uam::nlohmann_json::FindField(message, "method") != nullptr)
			{
				const std::string method = JsonDiagnosticStringValue(message, "method");
				if (IsCodexSession(session))
				{
					try
					{
						HandleCodexMessage(app, session, chat, message, browser);
					}
					catch (const std::exception& ex)
					{
						const std::string error_message = std::string("Codex app-server message handling failed: ") + ex.what();
						AppendAcpDiagnostic(session, "parse", "codex_message_parse_error", method, "", false, 0, error_message, CapDiagnosticString(message.dump(), kMaxAcpDiagnosticDetailBytes));
						FailAcpTurnOrSession(session, error_message);
						MarkAcpChatUnseenIfBackground(app, chat);
					}
				}
				else if (method == uam::acp_methods::kSessionUpdate)
				{
					HandleSessionUpdate(app, session, chat, JsonObjectValue(message, "params"), browser);
				}
				else
				{
					HandleAcpRequest(app, session, chat, message);
				}
				return true;
			}

			if (uam::nlohmann_json::FindField(message, "id") != nullptr)
			{
				HandleAcpResponse(app, session, chat, message);
				return true;
			}

			AppendAcpDiagnostic(session, "message", "ignored_without_method_or_id", "", "", false, 0, "", CapDiagnosticString(message.dump(), kMaxAcpDiagnosticDetailBytes));
			return false;
		}

		bool DrainStdout(AppState& app, AcpSessionState& session, ChatSession& chat, CefRefPtr<CefBrowser> browser)
		{
			bool changed = false;
			std::array<char, 8192> buffer{};
			while (true)
			{
				std::string read_error;
				const std::ptrdiff_t read_bytes = PlatformServicesFactory::Instance().process_service.ReadStdioProcessStdout(session, buffer.data(), buffer.size(), &read_error);
				if (read_bytes > 0)
				{
					changed = MarkAcpRuntimeActivity(session) || changed;
					session.stdout_buffer.append(buffer.data(), static_cast<std::size_t>(read_bytes));
					std::size_t newline_pos = std::string::npos;
					while ((newline_pos = session.stdout_buffer.find('\n')) != std::string::npos)
					{
						std::string line = session.stdout_buffer.substr(0, newline_pos);
						session.stdout_buffer.erase(0, newline_pos + 1);
						changed = ProcessAcpLine(app, session, chat, line, browser) || changed;
					}
					continue;
				}

				if (read_bytes == -2)
				{
					break;
				}

				if (read_bytes == 0)
				{
					break;
				}

				const std::string message = read_error.empty() ? ("Failed to read " + std::string(RuntimeDisplayName(session)) + " stdout.") : ("Failed to read " + std::string(RuntimeDisplayName(session)) + " stdout: " + read_error);
				AppendAcpDiagnostic(session, "read", "stdout_read_failed", "", "", false, 0, message);
				FailAcpTurnOrSession(session, message);
				MarkAcpChatUnseenIfBackground(app, chat);
				changed = true;
				break;
			}
			return changed;
		}

		bool DrainStderr(AcpSessionState& session)
		{
			bool changed = false;
			std::array<char, 4096> buffer{};
			while (true)
			{
				std::string read_error;
				const std::ptrdiff_t read_bytes = PlatformServicesFactory::Instance().process_service.ReadStdioProcessStderr(session, buffer.data(), buffer.size(), &read_error);
				if (read_bytes > 0)
				{
					changed = MarkAcpRuntimeActivity(session) || changed;
					AppendRecentStderr(session, std::string(buffer.data(), static_cast<std::size_t>(read_bytes)));
					changed = true;
					continue;
				}

				if (read_bytes == -2 || read_bytes == 0)
				{
					break;
				}

				const std::string message = read_error.empty() ? ("Failed to read " + std::string(RuntimeDisplayName(session)) + " stderr.") : ("Failed to read " + std::string(RuntimeDisplayName(session)) + " stderr: " + read_error);
				AppendAcpDiagnostic(session, "read", "stderr_read_failed", "", "", false, 0, message);
				changed = true;
				break;
			}
			return changed;
		}

		void MarkAcpProcessExited(AcpSessionState& session, bool has_exit_code = false, int exit_code = 0)
		{
			if (has_exit_code)
			{
				session.has_last_exit_code = true;
				session.last_exit_code = exit_code;
			}
			const bool active_turn = uam::AcpSessionHasActiveTurn(session);
			std::ostringstream detail;
			bool has_detail = false;
			if (has_exit_code)
			{
				detail << "exit_code=" << exit_code;
				has_detail = true;
			}
			if (!session.recent_stderr.empty())
			{
				if (has_detail)
				{
					detail << "\n";
				}
				detail << "stderr_tail=" << RecentStderrTail(session);
				has_detail = true;
			}
			const std::string pending_summary = PendingRequestSummary(session);
			if (!pending_summary.empty())
			{
				if (has_detail)
				{
					detail << "\n";
				}
				detail << "pending_requests=" << pending_summary;
			}
			AppendAcpDiagnostic(session, "process_exit", active_turn ? "active_turn" : "idle", "", "", has_exit_code, exit_code, "", detail.str());
			session.running = false;
			session.initialized = false;
			session.session_ready = false;
			if (active_turn)
			{
				const std::string message = uam::strings::NonEmptyOrFallback(session.last_error, std::string(RuntimeDisplayName(session)) + " process exited during an active turn.");
				FailAcpTurnOrSession(session, message);
			}
			else
			{
				session.lifecycle_state = kAcpLifecycleStopped;
			}
			session.processing = false;
			session.prompt_request_id = 0;
			session.cancel_request_id = 0;
			session.current_assistant_message_index = -1;
			session.pending_assistant_thoughts.clear();
			ResetAcpPendingInteractionState(session);
			PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(session);
		}
	} // namespace

	AcpSessionState* FindAcpSessionForChat(AppState& app, const std::string& chat_id)
	{
		for (auto& session : app.acp_sessions)
		{
			if (session != nullptr && session->chat_id == chat_id)
			{
				return session.get();
			}
		}
		return nullptr;
	}

	const AcpSessionState* FindAcpSessionForChat(const AppState& app, const std::string& chat_id)
	{
		for (const auto& session : app.acp_sessions)
		{
			if (session != nullptr && session->chat_id == chat_id)
			{
				return session.get();
			}
		}
		return nullptr;
	}

	bool SendAcpPrompt(AppState& app, const std::string& chat_id, const std::string& text, const std::vector<std::string>& markdown_store_files, const std::vector<MessageAttachment>& attachments, bool, std::string* error_out)
	{
		const std::string prompt = uam::strings::Trim(text);
		if (prompt.empty())
		{
			if (error_out != nullptr)
			{
				*error_out = "Prompt is empty.";
			}
			return false;
		}

		ChatSession* chat_ptr = ChatDomainService().FindChatById(app, chat_id);
		if (chat_ptr == nullptr)
		{
			if (error_out != nullptr)
			{
				*error_out = "Chat not found: " + chat_id;
			}
			return false;
		}

		ChatSession& chat = *chat_ptr;
		std::vector<std::string> validated_markdown_store_files;
		const std::filesystem::path markdown_store_root = MarkdownStoreService::NormalizeRoot(app.settings.markdown_store_directory);
		for (const std::string& file : markdown_store_files)
		{
			std::filesystem::path normalized_file;
			if (!MarkdownStoreService::ValidateStoreFilePath(markdown_store_root, file, &normalized_file, error_out))
			{
				return false;
			}
			validated_markdown_store_files.push_back(normalized_file.string());
		}

		AcpSessionState& session = EnsureAcpSessionForChat(app, chat);
		if (uam::AcpSessionHasPendingCancel(session))
		{
			const std::string provider_id = session.provider_id;
			const std::string protocol_kind = session.protocol_kind;
			if (!StopAcpSession(app, chat_id))
			{
				if (error_out != nullptr)
				{
					*error_out = "Failed to restart ACP session after cancelling the previous turn.";
				}
				return false;
			}

			session.provider_id = provider_id;
			session.protocol_kind = protocol_kind;
		}
		if (session.processing)
		{
			if (error_out != nullptr)
			{
				*error_out = std::string(RuntimeDisplayName(session)) + " is already processing this chat.";
			}
			return false;
		}

		if (!StartAcpProcessForChat(app, session, chat, error_out))
		{
			return false;
		}

		const std::string recall_preface = MemoryService::BuildRecallPreface(app, chat, prompt);
		std::string effective_prompt = recall_preface.empty() ? prompt : recall_preface + prompt;

		const Goal* active_goal = GoalService::FindActiveGoal(app, chat.id);
		if (active_goal && active_goal->status == GoalStatus::Active && !active_goal->objective.empty())
		{
			std::string goal_prompt = GoalService::BuildContinuationPrompt(*active_goal, active_goal->tokens_used, active_goal->token_budget);
			if (!goal_prompt.empty())
			{
				effective_prompt = goal_prompt + "\n\n" + effective_prompt;
			}
		}
		
		if (!validated_markdown_store_files.empty())
		{
			effective_prompt += "\n\nReferenced Markdown Store files:\n";
			for (const std::string& file : validated_markdown_store_files)
			{
				effective_prompt += "- " + file + "\n";
			}
		}
		if (!attachments.empty())
		{
			bool wrote_files_header = false;
			bool wrote_directories_header = false;
			for (const MessageAttachment& attachment : attachments)
			{
				if (attachment.path.empty())
				{
					continue;
				}
				if (attachment.kind == "directory")
				{
					if (!wrote_directories_header)
					{
						effective_prompt += "\n\nReferenced directories:\n";
						wrote_directories_header = true;
					}
					effective_prompt += "- " + attachment.path + "\n";
				}
				else
				{
					if (!wrote_files_header)
					{
						effective_prompt += "\n\nReferenced files:\n";
						wrote_files_header = true;
					}
					effective_prompt += "- " + attachment.path + "\n";
				}
			}
		}

		ChatDomainService::MessageAnalytics analytics;
		analytics.provider = MessageProviderId(session);
		ChatDomainService().AddMessageWithAnalytics(chat, MessageRole::User, prompt, analytics);
		if (!validated_markdown_store_files.empty() && !chat.messages.empty())
		{
			chat.messages.back().markdown_store_files = validated_markdown_store_files;
		}
		if (!attachments.empty() && !chat.messages.empty())
		{
			chat.messages.back().attachments = attachments;
			for (const MessageAttachment& attachment : attachments)
			{
				if (!attachment.path.empty() && !uam::ranges::Contains(chat.linked_files, attachment.path))
				{
					chat.linked_files.push_back(attachment.path);
				}
			}
		}
		SaveChatQuietly(app, chat);

		session.queued_prompt = effective_prompt;
		session.crash_restart_attempts = 0;
		ClearGoalReviewState(session);
		session.goal_turn_kind.clear();
		session.processing = true;
		session.cancel_requested = false;
		session.current_assistant_message_index = -1;
		session.turn_user_message_index = static_cast<int>(chat.messages.size()) - 1;
		session.turn_assistant_message_index = -1;
		session.turn_serial += 1;
		RememberAssistantReplayPrefixes(session, chat, session.turn_user_message_index);
		RememberLoadHistoryReplayUpdates(session, chat, session.turn_user_message_index);
		ResetAcpTurnStreamState(session);
		ResetAcpPendingInteractionState(session);
		session.last_runtime_activity_time_s = GetAppTimeSeconds();
		session.last_error.clear();
		session.lifecycle_state = session.session_ready ? kAcpLifecycleProcessing : kAcpLifecycleStarting;

		if (session.session_ready)
		{
			(void)SendQueuedPromptIfReady(session, chat);
		}

		return true;
	}

	bool SendAcpPrompt(AppState& app, const std::string& chat_id, const std::string& text, std::string* error_out)
	{
		return SendAcpPrompt(app, chat_id, text, std::vector<std::string>{}, std::vector<MessageAttachment>{}, false, error_out);
	}

	bool CancelAcpTurn(AppState& app, const std::string& chat_id, std::string* error_out)
	{
		AcpSessionState* session = FindAcpSessionForChat(app, chat_id);
		if (session == nullptr || !session->running)
		{
			return true;
		}

		const std::string pending_permission_request_id = session->pending_permission.request_id_json;
		if (!pending_permission_request_id.empty())
		{
			(void)SendPermissionResponse(*session, pending_permission_request_id, "", true, error_out);
		}
		const std::string pending_user_input_request_id = session->pending_user_input.request_id_json;
		if (!pending_user_input_request_id.empty())
		{
			(void)SendCodexUserInputResponse(*session, pending_user_input_request_id, {}, error_out);
		}

		session->queued_prompt.clear();
		session->processing = false;
		session->cancel_requested = true;
		ResetAcpPendingInteractionState(*session);
		session->current_assistant_message_index = -1;
		session->pending_assistant_thoughts.clear();
		session->lifecycle_state = session->session_ready ? kAcpLifecycleReady : kAcpLifecycleStopped;

		if (IsCodexSession(*session) && !session->session_id.empty() && !session->codex_turn_id.empty())
		{
			const int id = NextAcpRequestId(*session, uam::acp_methods::kTurnInterrupt);
			session->cancel_request_id = id;
			if (!WriteAcpMessage(*session, BuildCodexTurnInterruptRequest(id, session->session_id, session->codex_turn_id), error_out))
			{
				session->pending_request_methods.erase(id);
				session->cancel_request_id = 0;
				return false;
			}
		}
		else if (!session->session_id.empty())
		{
			if (!WriteAcpMessage(*session, BuildCancelNotification(session->session_id), error_out))
			{
				return false;
			}
		}

		return true;
	}

	bool StopAcpSession(AppState& app, const std::string& chat_id)
	{
		AcpSessionState* session = FindAcpSessionForChat(app, chat_id);
		if (session == nullptr)
		{
			return true;
		}

		if (session->running)
		{
			PlatformServicesFactory::Instance().process_service.StopStdioProcess(*session, true);
		}

		session->running = false;
		session->initialized = false;
		session->session_ready = false;
		session->processing = false;
		session->cancel_requested = false;
		session->lifecycle_state = kAcpLifecycleStopped;
		session->queued_prompt.clear();
		ClearAcpStartupModelRequest(*session);
		session->prompt_request_id = 0;
		session->cancel_request_id = 0;
		session->current_assistant_message_index = -1;
		session->turn_user_message_index = -1;
		session->turn_assistant_message_index = -1;
		session->turn_events.clear();
		session->assistant_replay_prefixes.clear();
		session->load_history_replay_updates.clear();
		session->pending_assistant_thoughts.clear();
		ResetAcpPendingInteractionState(*session);
		PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(*session);
		return true;
	}

	bool SetAcpSessionMode(AppState& app, const std::string& chat_id, const std::string& mode_id, std::string* error_out)
	{
		AcpSessionState* session = FindAcpSessionForChat(app, chat_id);
		if (session == nullptr || !session->running)
		{
			return true;
		}
		if (uam::AcpSessionHasCancelableWork(*session))
		{
			if (error_out != nullptr)
			{
				*error_out = "Cannot change structured runtime mode while " + std::string(RuntimeDisplayName(*session)) + " is busy.";
			}
			return false;
		}
		if (!session->session_ready || session->session_id.empty())
		{
			if (error_out != nullptr)
			{
				*error_out = "ACP session is not ready.";
			}
			return false;
		}
		if (IsCodexSession(*session))
		{
			session->current_mode_id = mode_id;
			return true;
		}
		if (IsClaudeSession(*session))
		{
			session->current_mode_id = mode_id;
			return StopAcpSession(app, chat_id);
		}

		const int id = NextAcpRequestId(*session, uam::acp_methods::kSessionSetMode);
		if (!WriteAcpMessage(*session, BuildSetModeRequest(id, session->session_id, ProviderApprovalModeId(*session, mode_id)), error_out))
		{
			session->pending_request_methods.erase(id);
			return false;
		}
		session->current_mode_id = mode_id;
		return true;
	}

	bool SetAcpSessionModel(AppState& app, const std::string& chat_id, const std::string& model_id, std::string* error_out)
	{
		AcpSessionState* session = FindAcpSessionForChat(app, chat_id);
		if (session == nullptr || !session->running)
		{
			return true;
		}
		if (uam::AcpSessionHasCancelableWork(*session))
		{
			if (error_out != nullptr)
			{
				*error_out = "Cannot change structured runtime model while " + std::string(RuntimeDisplayName(*session)) + " is busy.";
			}
			return false;
		}
		if (!session->session_ready || session->session_id.empty())
		{
			if (error_out != nullptr)
			{
				*error_out = "ACP session is not ready.";
			}
			return false;
		}
		if (IsCodexSession(*session))
		{
			session->current_model_id = model_id;
			return true;
		}
		if (IsClaudeSession(*session))
		{
			session->current_model_id = model_id;
			return StopAcpSession(app, chat_id);
		}

		const int id = NextAcpRequestId(*session, uam::acp_methods::kSessionSetModel);
		if (!WriteAcpMessage(*session, BuildSetModelRequest(id, session->session_id, model_id), error_out))
		{
			session->pending_request_methods.erase(id);
			return false;
		}
		session->current_model_id = model_id;
		return true;
	}

	bool TryAutoApprovePendingAcpPermission(AppState& app, const std::string& chat_id, std::string* error_out)
	{
		AcpSessionState* session = FindAcpSessionForChat(app, chat_id);
		if (session == nullptr || !session->running || session->pending_permission.request_id_json.empty())
		{
			return false;
		}
		ChatSession* chat = nullptr;
		for (ChatSession& candidate : app.chats)
		{
			if (candidate.id == chat_id)
			{
				chat = &candidate;
				break;
			}
		}
		if (chat == nullptr)
		{
			if (error_out != nullptr)
			{
				*error_out = "Chat not found: " + chat_id;
			}
			return false;
		}
		return TryAutoApprovePendingPermission(*session, *chat, error_out);
	}

	bool ResolveAcpPermission(AppState& app, const std::string& chat_id, const std::string& request_id_json, const std::string& option_id, bool cancelled, std::string* error_out)
	{
		AcpSessionState* session = FindAcpSessionForChat(app, chat_id);
		if (session == nullptr || !session->running)
		{
			if (error_out != nullptr)
			{
				*error_out = "ACP session is not running.";
			}
			return false;
		}

		if (session->pending_permission.request_id_json != request_id_json)
		{
			if (error_out != nullptr)
			{
				*error_out = "ACP permission request is no longer active.";
			}
			return false;
		}

		if (!SendPermissionResponse(*session, request_id_json, option_id, cancelled, error_out))
		{
			return false;
		}

		session->pending_permission = AcpPendingPermissionState{};
		session->waiting_for_permission = false;
		ClearAcpPendingWait(*session);
		session->cancel_requested = false;
		session->lifecycle_state = session->processing ? kAcpLifecycleProcessing : kAcpLifecycleReady;
		return true;
	}

	bool ResolveAcpUserInput(AppState& app, const std::string& chat_id, const std::string& request_id_json, const std::map<std::string, std::vector<std::string>>& answers, std::string* error_out)
	{
		AcpSessionState* session = FindAcpSessionForChat(app, chat_id);
		if (session == nullptr || !session->running)
		{
			if (error_out != nullptr)
			{
				*error_out = "ACP session is not running.";
			}
			return false;
		}

		if (session->pending_user_input.request_id_json != request_id_json)
		{
			if (error_out != nullptr)
			{
				*error_out = "ACP user input request is no longer active.";
			}
			return false;
		}

		if (!SendCodexUserInputResponse(*session, request_id_json, answers, error_out))
		{
			return false;
		}

		session->pending_user_input = AcpPendingUserInputState{};
		session->waiting_for_user_input = false;
		ClearAcpPendingWait(*session);
		session->cancel_requested = false;
		session->lifecycle_state = session->processing ? kAcpLifecycleProcessing : kAcpLifecycleReady;
		return true;
	}

	bool PollAllAcpSessions(AppState& app, CefRefPtr<CefBrowser> browser)
	{
		bool changed = false;
		for (auto& session_ptr : app.acp_sessions)
		{
			if (session_ptr == nullptr)
			{
				continue;
			}

			AcpSessionState& session = *session_ptr;
			if (!session.running)
			{
				continue;
			}

			ChatSession* chat_ptr = ChatDomainService().FindChatById(app, session.chat_id);
			if (chat_ptr == nullptr)
			{
				PlatformServicesFactory::Instance().process_service.StopStdioProcess(session, true);
				MarkAcpProcessExited(session, false, 0);
				changed = true;
				continue;
			}

			ChatSession& chat = *chat_ptr;
			changed = DrainStderr(session) || changed;
			changed = DrainStdout(app, session, chat, browser) || changed;

			if (SendSessionSetupIfReady(app, session, chat))
			{
				changed = true;
			}

			if (SendQueuedPromptIfReady(session, chat))
			{
				if (!session.last_error.empty() && session.lifecycle_state == kAcpLifecycleError)
				{
					MarkAcpChatUnseenIfBackground(app, chat);
				}
				changed = true;
			}

			if (UpdateAcpStaleWait(session, GetAppTimeSeconds()))
			{
				changed = true;
			}

			int exit_code = 0;
			if (PlatformServicesFactory::Instance().process_service.PollStdioProcessExited(session, &exit_code))
			{
				// Snapshot the turn before MarkAcpProcessExited clears it: if the
				// process died before the queued prompt was ever delivered (e.g. a
				// startup crash), the turn can be retried safely without risking a
				// duplicate prompt reaching the provider.
				const bool turn_was_active = uam::AcpSessionHasActiveTurn(session);
				const bool undelivered_prompt = session.processing && session.prompt_request_id == 0 && !session.queued_prompt.empty();
				const std::string pending_prompt = session.queued_prompt;
				const int turn_user_message_index = session.turn_user_message_index;
				const int turn_serial = session.turn_serial;
				const std::string goal_turn_kind = session.goal_turn_kind;
				const bool goal_review_turn = session.goal_review_turn;
				const bool goal_review_scheduled = session.goal_review_scheduled;
				const std::string goal_review_goal_id = session.goal_review_goal_id;
				const std::string goal_review_user_prompt = session.goal_review_user_prompt;
				const std::string goal_review_assistant_text = session.goal_review_assistant_text;

				MarkAcpProcessExited(session, true, exit_code);

				std::string restart_error;
				if (undelivered_prompt && session.crash_restart_attempts < 1 && StartAcpProcessForChat(app, session, chat, &restart_error))
				{
					session.crash_restart_attempts = 1;
					session.queued_prompt = pending_prompt;
					session.processing = true;
					session.turn_user_message_index = turn_user_message_index;
					session.turn_assistant_message_index = -1;
					session.turn_serial = turn_serial + 1;
					session.goal_turn_kind = goal_turn_kind;
					session.goal_review_turn = goal_review_turn;
					session.goal_review_scheduled = goal_review_scheduled;
					session.goal_review_goal_id = goal_review_goal_id;
					session.goal_review_user_prompt = goal_review_user_prompt;
					session.goal_review_assistant_text = goal_review_assistant_text;
					session.last_error.clear();
					AppendGoalLoopDiagnostic(session, "auto_restart_after_startup_crash", goal_review_goal_id, pending_prompt);
				}
				else
				{
					if (!session.last_error.empty())
					{
						MarkAcpChatUnseenIfBackground(app, chat);
					}
					// A goal must not stay Active with no running session; surface
					// the crash as a blocker so the goal loop ends visibly instead
					// of stalling silently.
					if (turn_was_active)
					{
						if (Goal* active_goal = GoalService::FindActiveGoal(app, chat.id); active_goal != nullptr)
						{
							const std::string blocker = uam::strings::NonEmptyOrFallback(session.last_error, std::string(RuntimeDisplayName(session)) + " process exited during a goal turn.");
							GoalService::RecordBlocker(app, active_goal->id, blocker);
							(void)GoalService::UpdateGoalStatus(app, active_goal->id, GoalStatus::Blocked);
							AppendGoalLoopDiagnostic(session, "goal_blocked_process_exit", active_goal->id, blocker);
						}
					}
				}
				changed = true;
			}
		}

		return changed;
	}

	void FastStopAcpSessionsForExit(AppState& app)
	{
		for (auto& session : app.acp_sessions)
		{
			if (session != nullptr)
			{
				PlatformServicesFactory::Instance().process_service.StopStdioProcess(*session, true);
				session->running = false;
				session->lifecycle_state = kAcpLifecycleStopped;
			}
		}
		app.acp_sessions.clear();
	}

	std::vector<std::string> BuildAcpLaunchArgvForTests(const ChatSession& chat)
	{
		ProviderProfile provider;
		provider.id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(chat.provider_id);
		return acp_detail::BuildAcpLaunchArgv(provider, chat);
	}

	std::string BuildAcpLaunchDetailForTests(const std::filesystem::path& workspace_root, const ChatSession& chat)
	{
		ProviderProfile provider;
		provider.id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(chat.provider_id);
		AppState app;
		return acp_detail::BuildAcpLaunchDetail(provider, app, workspace_root, chat);
	}

	std::string BuildAcpLaunchDetailForTests(const AppState& app, const std::filesystem::path& workspace_root, const ChatSession& chat)
	{
		return acp_detail::BuildAcpLaunchDetail(app, workspace_root, chat);
	}

	std::string BuildAcpInitializeRequestForTests(int request_id)
	{
		return acp_detail::BuildInitializeRequest(request_id).dump();
	}

	std::string BuildAcpNewSessionRequestForTests(int request_id, const std::string& cwd)
	{
		return acp_detail::BuildNewSessionRequest(request_id, cwd).dump();
	}

	std::string BuildGeminiSessionSetupRequestForTests(int request_id, const ChatSession& chat, const std::string& cwd, bool load_session_supported)
	{
		return acp_detail::BuildGeminiSessionSetupRequest(request_id, chat, cwd, load_session_supported).dump();
	}

	std::string BuildAcpPromptRequestForTests(int request_id, const std::string& session_id, const std::string& text)
	{
		return acp_detail::BuildPromptRequest(request_id, session_id, text).dump();
	}

	std::string BuildAcpSetModeRequestForTests(int request_id, const std::string& session_id, const std::string& mode_id)
	{
		return acp_detail::BuildSetModeRequest(request_id, session_id, mode_id).dump();
	}

	std::string BuildAcpSetModelRequestForTests(int request_id, const std::string& session_id, const std::string& model_id)
	{
		return acp_detail::BuildSetModelRequest(request_id, session_id, model_id).dump();
	}

	std::string BuildCodexInitializeRequestForTests(int request_id)
	{
		return acp_detail::BuildCodexInitializeRequest(request_id).dump();
	}

	std::string BuildCodexInitializedNotificationForTests()
	{
		return acp_detail::BuildCodexInitializedNotification().dump();
	}

	std::string BuildCodexModelListRequestForTests(int request_id)
	{
		return acp_detail::BuildCodexModelListRequest(request_id).dump();
	}

	std::string BuildCodexSessionSetupRequestForTests(int request_id, const ChatSession& chat, const std::string& cwd)
	{
		return acp_detail::BuildCodexSessionSetupRequest(request_id, chat, cwd).dump();
	}

	std::string BuildCodexThreadStartRequestForTests(int request_id, const ChatSession& chat, const std::string& cwd)
	{
		return acp_detail::BuildCodexThreadStartRequest(request_id, chat, cwd).dump();
	}

	std::string BuildCodexThreadResumeRequestForTests(int request_id, const ChatSession& chat, const std::string& cwd)
	{
		return acp_detail::BuildCodexThreadResumeRequest(request_id, chat, cwd).dump();
	}

	std::string BuildCodexTurnStartRequestForTests(int request_id, const std::string& thread_id, const std::string& text, const ChatSession& chat, const std::string& active_model_id)
	{
		return acp_detail::BuildCodexTurnStartRequest(request_id, thread_id, text, chat, active_model_id).dump();
	}

	std::string BuildCodexTurnInterruptRequestForTests(int request_id, const std::string& thread_id, const std::string& turn_id)
	{
		return acp_detail::BuildCodexTurnInterruptRequest(request_id, thread_id, turn_id).dump();
	}

	std::string BuildCodexUserInputResponseForTests(const std::string& request_id_json, const std::map<std::string, std::vector<std::string>>& answers)
	{
		return BuildCodexUserInputResponse(request_id_json, answers).dump();
	}

	std::string ResolveAcpSessionResumeIdForTests(const AppState& app, const ChatSession& chat)
	{
		return ResolvedAcpResumeIdForChat(app, chat);
	}

	bool ProcessAcpLineForTests(AppState& app, AcpSessionState& session, ChatSession& chat, const std::string& line)
	{
		return ProcessAcpLine(app, session, chat, line, nullptr);
	}

	bool IsValidCodexThreadIdForTests(const std::string& thread_id)
	{
		return uam::codex::IsValidThreadId(thread_id);
	}

	bool UpdateAcpStaleWaitForTests(AcpSessionState& session, double now_seconds)
	{
		return UpdateAcpStaleWait(session, now_seconds);
	}

	void FlushPendingChatSaves(AppState& app)
	{
		const double now = GetAppTimeSeconds();
		std::vector<std::string> due_chat_ids;
		for (const auto& entry : app.pending_chat_save_at_by_chat_id)
		{
			if (entry.second <= now)
			{
				due_chat_ids.push_back(entry.first);
			}
		}
		for (const std::string& chat_id : due_chat_ids)
		{
			const ChatSession* chat = ChatDomainService().FindChatById(app, chat_id);
			if (chat != nullptr)
			{
				(void)ChatRepository::SaveChat(app.data_root, *chat);
			}
			app.pending_chat_save_at_by_chat_id.erase(chat_id);
		}
	}

} // namespace uam
