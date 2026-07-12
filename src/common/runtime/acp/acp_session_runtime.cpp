#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_claude_message_handlers.h"
#include "common/runtime/acp/acp_codex_message_handlers.h"
#include "common/runtime/acp/acp_goal_loop.h"
#include "common/runtime/acp/acp_polling.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_session_update_handler.h"

#include "app/chat_domain_service.h"
#include "app/markdown_store_service.h"
#include "app/goal_service.h"
#include "app/memory_service.h"
#include "app/native_session_link_service.h"
#include "app/provider_resolution_service.h"
#include "common/chat/chat_repository.h"
#include <cstring>
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
		constexpr int kAcpReconnectMaxAttempts = 3;
		constexpr double kAcpReconnectBaseDelaySeconds = 0.25;

		double AcpReconnectDelaySeconds(int attempt)
		{
			return kAcpReconnectBaseDelaySeconds * (1 << std::clamp(attempt, 0, kAcpReconnectMaxAttempts - 1));
		}

		void ScheduleAcpReconnect(AcpSessionState& session, double now_seconds)
		{
			session.reconnect_pending = true;
			session.reconnect_attempts = 0;
			session.reconnect_not_before_time_s = now_seconds + AcpReconnectDelaySeconds(0);
			AppendAcpDiagnostic(session, "reconnect", "scheduled", "", "", false, 0, "Structured runtime disconnected; reconnect scheduled.");
		}

		bool TryReconnectAcpSession(AppState& app, AcpSessionState& session, ChatSession& chat, double now_seconds)
		{
			if (!session.reconnect_pending || now_seconds < session.reconnect_not_before_time_s)
			{
				return false;
			}

			std::string error;
			if (StartAcpProcessForChat(app, session, chat, &error))
			{
				session.reconnect_pending = false;
				session.reconnect_attempts = 0;
				session.reconnect_not_before_time_s = 0.0;
				AppendAcpDiagnostic(session, "reconnect", "started", "", "", false, 0, "Structured runtime reconnected.");
				return true;
			}

			++session.reconnect_attempts;
			if (session.reconnect_attempts >= kAcpReconnectMaxAttempts)
			{
				session.reconnect_pending = false;
				AppendAcpDiagnostic(session, "reconnect", "exhausted", "", "", false, 0, uam::strings::NonEmptyOrFallback(error, "Structured runtime reconnect failed."));
				return true;
			}

			session.reconnect_not_before_time_s = now_seconds + AcpReconnectDelaySeconds(session.reconnect_attempts);
			AppendAcpDiagnostic(session, "reconnect", "retry_scheduled", "", "", false, 0, uam::strings::NonEmptyOrFallback(error, "Structured runtime reconnect failed."));
			return true;
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
		session.goal_auto_resume_attempts = 0;
		session.goal_resume_suppressed = false;
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
			(void)acp_detail::SendPermissionResponse(*session, pending_permission_request_id, "", true, error_out);
		}
		const std::string pending_user_input_request_id = session->pending_user_input.request_id_json;
		if (!pending_user_input_request_id.empty())
		{
			(void)acp_detail::SendCodexUserInputResponse(*session, pending_user_input_request_id, {}, error_out);
		}

		session->queued_prompt.clear();
		session->processing = false;
		session->cancel_requested = true;
		session->goal_resume_suppressed = true;
		ResetAcpPendingInteractionState(*session);
		session->current_assistant_message_index = -1;
		session->pending_assistant_thoughts.clear();
		session->lifecycle_state = session->session_ready ? kAcpLifecycleReady : kAcpLifecycleStopped;

		if (!session->session_id.empty())
		{
			const IProviderRuntime& cancel_runtime = ProviderRuntimeRegistry::ResolveById(session->provider_id);
			int cancel_id = session->next_request_id++;
			std::string cancel_method;
			nlohmann::json cancel_msg = cancel_runtime.OnAcpBuildCancel(*session, cancel_id, cancel_method);
			if (cancel_msg.is_null() || cancel_msg.empty())
			{
				return StopAcpSession(app, chat_id);
			}
			if (!cancel_method.empty())
			{
				session->pending_request_methods[cancel_id] = cancel_method;
				session->cancel_request_id = cancel_id;
			}
			if (!acp_detail::WriteAcpMessage(*session, cancel_msg, error_out))
			{
				if (!cancel_method.empty())
				{
					session->pending_request_methods.erase(cancel_id);
					session->cancel_request_id = 0;
				}
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
		session->reconnect_pending = false;
		session->reconnect_attempts = 0;
		session->reconnect_not_before_time_s = 0.0;
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
		{
			const IProviderRuntime& sm_runtime = ProviderRuntimeRegistry::ResolveById(session->provider_id);
			session->current_mode_id = mode_id;
			if (sm_runtime.OnAcpSetModeLocally(*session, mode_id))
			{
				return true;
			}
			if (std::strcmp(sm_runtime.AcpProtocolKind(), "claude-code-stream-json") == 0)
			{
				return StopAcpSession(app, chat_id);
			}

			const int id = NextAcpRequestId(*session, uam::acp_methods::kSessionSetMode);
			if (!acp_detail::WriteAcpMessage(*session, BuildSetModeRequest(id, session->session_id, ProviderApprovalModeId(*session, mode_id)), error_out))
			{
				session->pending_request_methods.erase(id);
				return false;
			}
			session->current_mode_id = mode_id;
			return true;
		}
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
		{
			const IProviderRuntime& sm_runtime = ProviderRuntimeRegistry::ResolveById(session->provider_id);
			session->current_model_id = model_id;
			if (sm_runtime.OnAcpSetModelLocally(*session, model_id))
			{
				return true;
			}
			if (std::strcmp(sm_runtime.AcpProtocolKind(), "claude-code-stream-json") == 0)
			{
				return StopAcpSession(app, chat_id);
			}

			const int id = NextAcpRequestId(*session, uam::acp_methods::kSessionSetModel);
			if (!acp_detail::WriteAcpMessage(*session, BuildSetModelRequest(id, session->session_id, model_id), error_out))
			{
				session->pending_request_methods.erase(id);
				return false;
			}
			session->current_model_id = model_id;
			return true;
		}
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
		return acp_detail::TryAutoApprovePendingPermission(*session, *chat, error_out);
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

		if (!acp_detail::SendPermissionResponse(*session, request_id_json, option_id, cancelled, error_out))
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

		if (!acp_detail::SendCodexUserInputResponse(*session, request_id_json, answers, error_out))
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
				ChatSession* reconnect_chat = ChatDomainService().FindChatById(app, session.chat_id);
				if (reconnect_chat != nullptr && TryReconnectAcpSession(app, session, *reconnect_chat, GetAppTimeSeconds()))
				{
					changed = true;
				}
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

			if (ResumeStalledGoalLoopIfNeeded(app, session, chat, browser, GetAppTimeSeconds()))
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
					ScheduleAcpReconnect(session, GetAppTimeSeconds());
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
		return acp_detail::BuildCodexUserInputResponse(request_id_json, answers).dump();
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

	double AcpReconnectDelaySecondsForTests(int attempt)
	{
		return AcpReconnectDelaySeconds(attempt);
	}

	void ScheduleAcpReconnectForTests(AcpSessionState& session, double now_seconds)
	{
		ScheduleAcpReconnect(session, now_seconds);
	}

	std::string AutoApproveOptionIdForTests(const AcpPendingPermissionState& pending)
	{
		return acp_detail::AutoApproveOptionId(pending);
	}

	bool ResumeStalledGoalLoopForTests(AppState& app, AcpSessionState& session, ChatSession& chat, double now_seconds)
	{
		return acp_detail::ResumeStalledGoalLoopIfNeeded(app, session, chat, nullptr, now_seconds);
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
