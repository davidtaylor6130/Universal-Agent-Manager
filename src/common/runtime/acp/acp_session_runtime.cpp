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
#include <iterator>
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
#include "common/runtime/provider_cli_compatibility_service.h"
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

#include "common/runtime/terminal/terminal_lifecycle.h"

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
		constexpr double kAcpCancelTimeoutSeconds = 5.0;
		constexpr double kAcpSetupInactivityTimeoutSeconds = 60.0;
		constexpr std::size_t kMarkdownStorePromptMaxBytes = 2U * 1024U * 1024U;

		bool IsCopilotCompatibilityCheckPending(const AppState& app)
		{
			const auto state = app.runtime_cli_versions_by_provider_id.find(uam::provider_ids::kCopilotCli);
			return state != app.runtime_cli_versions_by_provider_id.end() && !state->second.checked;
		}

		double AcpReconnectDelaySeconds(int attempt)
		{
			return kAcpReconnectBaseDelaySeconds * (1 << std::clamp(attempt, 0, kAcpReconnectMaxAttempts - 1));
		}

		void ScheduleAcpReconnect(AcpSessionState& session, double now_seconds)
		{
			if (session.reconnect_attempts >= kAcpReconnectMaxAttempts)
			{
				session.reconnect_pending = false;
				session.reconnect_not_before_time_s = 0.0;
				AppendAcpDiagnostic(session, "reconnect", "exhausted", "", "", false, 0, "Structured runtime reconnect attempts exhausted.");
				return;
			}
			session.reconnect_pending = true;
			session.reconnect_not_before_time_s = now_seconds + AcpReconnectDelaySeconds(session.reconnect_attempts);
			AppendAcpDiagnostic(session, "reconnect", "scheduled", "", "", false, 0, "Structured runtime disconnected; reconnect scheduled.");
		}

		bool TryReconnectAcpSession(AppState& app, AcpSessionState& session, ChatSession& chat, double now_seconds)
		{
			if (!session.reconnect_pending || now_seconds < session.reconnect_not_before_time_s)
			{
				return false;
			}

			std::string error;
			if (!PrepareCliTerminalForAcpLaunch(app, chat.id, &error))
			{
				session.reconnect_not_before_time_s = now_seconds + kAcpReconnectBaseDelaySeconds;
				return true;
			}

			const int next_attempt = session.reconnect_attempts + 1;
			if (StartAcpProcessForChat(app, session, chat, &error))
			{
				session.reconnect_pending = false;
				session.reconnect_attempts = next_attempt;
				session.reconnect_not_before_time_s = 0.0;
				AppendAcpDiagnostic(session, "reconnect", "started", "", "", false, 0, "Structured runtime reconnected.");
				return true;
			}

			session.reconnect_attempts = next_attempt;
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

		bool HandleAcpSetupInactivityTimeout(AppState& app, AcpSessionState& session, ChatSession& chat, double now_seconds)
		{
			const bool initial_setup_pending = !session.session_ready && session.lifecycle_state == kAcpLifecycleStarting;
			const bool control_request_pending = session.session_ready &&
			    (session.startup_model_request_id != 0 || session.mode_change_request_id != 0 || session.model_change_request_id != 0);
			if (!session.running ||
			    (!initial_setup_pending && !control_request_pending) ||
			    uam::AcpSessionIsWaitingForInput(session) ||
			    now_seconds - session.last_runtime_activity_time_s < kAcpSetupInactivityTimeoutSeconds)
			{
				return false;
			}

			const bool active_turn = uam::AcpSessionHasActiveTurn(session);
			const bool undelivered_prompt = session.processing && session.prompt_request_id == 0 && !session.queued_prompt.empty();
			const std::string pending_prompt = session.queued_prompt;
			const bool model_discovery_only = session.model_discovery_only;
			const std::string message = std::string(RuntimeDisplayName(session)) + " setup timed out after 60 seconds without runtime activity.";
			const std::string detail = "pending_requests=" + PendingRequestSummary(session) +
			    (session.recent_stderr.empty() ? "" : "\nstderr_tail=" + RecentStderrTail(session));
			AppendAcpDiagnostic(session, "session_setup", "setup_timeout", "", "", false, 0, message, detail);
			PlatformServicesFactory::Instance().process_service.StopStdioProcess(session, true);
			session.last_error = message;
			MarkAcpProcessExited(session, false, 0);

			if (undelivered_prompt)
			{
				session.queued_prompt = pending_prompt;
				session.processing = true;
			}

			if (model_discovery_only)
			{
				session.model_discovery_only = false;
				session.session_id.clear();
				session.codex_thread_id.clear();
				if (app.provider_model_catalog != nullptr)
				{
					app.provider_model_catalog->RememberRefreshFailure(session.provider_id, message);
				}
			}
			else
			{
				ScheduleAcpReconnect(session, now_seconds);
				if (active_turn)
				{
					if (Goal* active_goal = GoalService::FindActiveGoal(app, chat.id); active_goal != nullptr)
					{
						GoalService::RecordBlocker(app, active_goal->id, message);
						(void)GoalService::UpdateGoalStatus(app, active_goal->id, GoalStatus::Blocked);
						AppendGoalLoopDiagnostic(session, "goal_blocked_setup_timeout", active_goal->id, message);
					}
				}
			}

			MarkAcpChatUnseenIfBackground(app, chat);
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

	namespace
	{
		std::string BuildMarkdownStorePromptBlock(const MarkdownStoreService::Entry& entry)
		{
			return "--- BEGIN ATTACHED SKILL: " + entry.title + " ---\n" + entry.body + "\n--- END ATTACHED SKILL ---";
		}

		bool BuildAcpPromptBody(AppState& app, ChatSession& chat, const AcpQueuedUserPromptState& queued, std::size_t& markdown_store_bytes, std::string& effective_prompt, std::string* error_out)
		{
			const std::string recall_preface = MemoryService::BuildRecallPreface(app, chat, queued.text);
			effective_prompt = recall_preface.empty() ? queued.text : recall_preface + queued.text;
			if (!queued.markdown_store_files.empty())
			{
				const std::filesystem::path markdown_store_root = MarkdownStoreService::NormalizeRoot(app.settings.markdown_store_directory);
				const bool has_snapshots = queued.markdown_store_prompt_blocks.size() == queued.markdown_store_files.size();
				for (std::size_t index = 0; index < queued.markdown_store_files.size(); ++index)
				{
					std::string prompt_block;
					if (has_snapshots)
					{
						prompt_block = queued.markdown_store_prompt_blocks[index];
					}
					else
					{
						MarkdownStoreService::Entry entry;
						std::string load_error;
						if (!MarkdownStoreService::LoadEntry(markdown_store_root, queued.markdown_store_files[index], &entry, &load_error))
						{
							if (error_out != nullptr)
								*error_out = "Could not load an attached skill: " + load_error;
							return false;
						}
						prompt_block = BuildMarkdownStorePromptBlock(entry);
					}
					if (prompt_block.size() > kMarkdownStorePromptMaxBytes - markdown_store_bytes)
					{
						if (error_out != nullptr)
							*error_out = "Attached skill content exceeds the 2 MiB prompt limit.";
						return false;
					}
					markdown_store_bytes += prompt_block.size();
					effective_prompt += "\n\n" + prompt_block;
				}
			}
			if (!queued.attachments.empty())
			{
				bool wrote_files_header = false;
				bool wrote_directories_header = false;
				for (const MessageAttachment& attachment : queued.attachments)
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
			return true;
		}

		bool BuildAcpBatchPrompt(AppState& app, ChatSession& chat, const std::deque<AcpQueuedUserPromptState>& batch, std::string& effective_prompt, std::string* error_out)
		{
			std::size_t markdown_store_bytes = 0;
			for (std::size_t index = 0; index < batch.size(); ++index)
			{
				if (!effective_prompt.empty())
				{
					effective_prompt += "\n\n";
				}
				if (batch.size() > 1)
				{
					effective_prompt += "Queued user message " + std::to_string(index + 1) + ":\n";
				}
				std::string prompt_body;
				if (!BuildAcpPromptBody(app, chat, batch[index], markdown_store_bytes, prompt_body, error_out))
				{
					return false;
				}
				effective_prompt += prompt_body;
			}

			const AcpQueuedUserPromptState& first = batch.front();
			const Goal* goal = first.goal_id.empty()
			                       ? GoalService::FindActiveGoal(app, chat.id)
			                       : GoalService::FindGoalById(app, chat.id, first.goal_id);
			if (first.goal_mode && goal != nullptr && goal->status == GoalStatus::Active && !goal->objective.empty())
			{
				const std::string goal_prompt = GoalService::BuildContinuationPrompt(*goal, goal->tokens_used, goal->token_budget, chat.small_model_mode);
				if (!goal_prompt.empty())
				{
					effective_prompt = goal_prompt + "\n\n" + effective_prompt;
				}
			}
			return true;
		}

		void AppendQueuedUserMessages(ChatSession& chat, AcpSessionState& session, const std::deque<AcpQueuedUserPromptState>& batch)
		{
			for (const AcpQueuedUserPromptState& queued : batch)
			{
				if (!queued.append_user_message)
				{
					continue;
				}

				ChatDomainService::MessageAnalytics analytics;
				analytics.provider = MessageProviderId(session);
				ChatDomainService().AddMessageWithAnalytics(chat, MessageRole::User, queued.text, analytics);
				chat.messages.back().markdown_store_files = queued.markdown_store_files;
				chat.messages.back().markdown_store_prompt_blocks = queued.markdown_store_prompt_blocks;
				chat.messages.back().attachments = queued.attachments;
				for (const MessageAttachment& attachment : queued.attachments)
				{
					if (!attachment.path.empty() && !uam::ranges::Contains(chat.linked_files, attachment.path))
					{
						chat.linked_files.push_back(attachment.path);
					}
				}
			}
		}

		bool StartAcpUserPromptBatch(AppState& app, AcpSessionState& session, ChatSession& chat, const std::deque<AcpQueuedUserPromptState>& batch, std::string* error_out)
		{
			if (batch.empty())
			{
				return false;
			}
			if (!PrepareCliTerminalForAcpLaunch(app, chat.id, error_out))
			{
				return false;
			}
			std::string effective_prompt;
			if (!BuildAcpBatchPrompt(app, chat, batch, effective_prompt, error_out) || !StartAcpProcessForChat(app, session, chat, error_out))
			{
				return false;
			}
			const std::string selected_model_id = uam::strings::NonEmptyOrFallback(chat.model_id, session.current_model_id);
			const auto selected_model = std::ranges::find_if(session.available_models, [&selected_model_id](const AcpModelState& model) { return model.id == selected_model_id; });
			if (selected_model != session.available_models.end() && !chat.reasoning_effort.empty() && !selected_model->supported_reasoning_efforts.empty() && !uam::ranges::Contains(selected_model->supported_reasoning_efforts, chat.reasoning_effort))
			{
				chat.reasoning_effort = uam::ranges::Contains(selected_model->supported_reasoning_efforts, selected_model->default_reasoning_effort)
				                            ? selected_model->default_reasoning_effort
				                            : selected_model->supported_reasoning_efforts.front();
			}
			if (selected_model != session.available_models.end() && !chat.service_tier.empty() && !uam::ranges::Contains(selected_model->additional_speed_tiers, chat.service_tier))
			{
				chat.service_tier.clear();
			}
			const std::string desired_mode = uam::approval_modes::EffectiveProviderMode(chat.approval_mode, chat.command_safety_tier);
			const bool must_leave_hidden_autopilot = session.current_mode_id == uam::approval_modes::kAcpAutopilotMode;
			if (session.session_ready && (!uam::strings::IsBlank(chat.approval_mode) || must_leave_hidden_autopilot) && session.current_mode_id != desired_mode && !SetAcpSessionMode(app, chat.id, desired_mode, error_out))
			{
				return false;
			}
			if (session.session_ready && !chat.model_id.empty() && session.current_model_id != chat.model_id && !SetAcpSessionModel(app, chat.id, chat.model_id, error_out))
			{
				return false;
			}

			session.queued_prompt = std::move(effective_prompt);
			session.crash_restart_attempts = 0;
			session.goal_auto_resume_attempts = 0;
			session.goal_resume_suppressed = false;
			ClearGoalReviewState(session);
			session.goal_turn_kind.clear();
			session.processing = true;
			session.cancel_requested = false;
			session.cancel_requested_time_s = 0.0;
			session.current_assistant_message_index = -1;
			const std::size_t appended_message_count = static_cast<std::size_t>(std::count_if(batch.begin(), batch.end(), [](const AcpQueuedUserPromptState& queued) { return queued.append_user_message; }));
			session.turn_user_message_index = static_cast<int>(chat.messages.size() + appended_message_count) - 1;
			session.turn_assistant_message_index = -1;
			session.turn_serial += 1;
			RememberAssistantReplayPrefixes(session, chat, session.turn_user_message_index);
			RememberLoadHistoryReplayUpdates(session, chat, session.turn_user_message_index);
			ResetAcpTurnStreamState(session);
			ResetAcpPendingInteractionState(session);
			session.turn_started_time_s = GetAppTimeSeconds();
			session.last_runtime_activity_time_s = session.turn_started_time_s;
			session.last_error.clear();
			session.lifecycle_state = session.session_ready ? kAcpLifecycleProcessing : kAcpLifecycleStarting;

			if (session.session_ready)
			{
				(void)SendQueuedPromptIfReady(session, chat);
				if (session.lifecycle_state == kAcpLifecycleError)
				{
					if (error_out != nullptr)
					{
						*error_out = uam::strings::NonEmptyOrFallback(session.last_error, "Failed to deliver queued prompt batch.");
					}
					return false;
				}
			}
			AppendQueuedUserMessages(chat, session, batch);
			SaveChatQuietly(app, chat);
			return true;
		}

		bool StartAcpUserPrompt(AppState& app, AcpSessionState& session, ChatSession& chat, const AcpQueuedUserPromptState& queued, std::string* error_out)
		{
			return StartAcpUserPromptBatch(app, session, chat, std::deque<AcpQueuedUserPromptState>{queued}, error_out);
		}

		bool BuildQueuedAcpUserPrompt(AppState& app, ChatSession& chat, const std::string& text, const std::vector<std::string>& markdown_store_files, const std::vector<MessageAttachment>& attachments, bool goal_mode, const std::string& goal_id, AcpQueuedUserPromptState& queued, std::string* error_out)
		{
			queued.text = uam::strings::Trim(text);
			if (queued.text.empty())
			{
				if (error_out != nullptr)
				{
					*error_out = "Prompt is empty.";
				}
				return false;
			}

			const std::filesystem::path markdown_store_root = MarkdownStoreService::NormalizeRoot(app.settings.markdown_store_directory);
			std::size_t markdown_store_bytes = 0;
			for (const std::string& file : markdown_store_files)
			{
				MarkdownStoreService::Entry entry;
				if (!MarkdownStoreService::LoadEntry(markdown_store_root, file, &entry, error_out))
				{
					return false;
				}
				std::string prompt_block = BuildMarkdownStorePromptBlock(entry);
				if (prompt_block.size() > kMarkdownStorePromptMaxBytes - markdown_store_bytes)
				{
					if (error_out != nullptr)
						*error_out = "Attached skill content exceeds the 2 MiB prompt limit.";
					return false;
				}
				markdown_store_bytes += prompt_block.size();
				queued.markdown_store_files.push_back(uam::paths::Utf8PathString(entry.file_path));
				queued.markdown_store_prompt_blocks.push_back(std::move(prompt_block));
			}

			const Goal* active_goal = GoalService::FindActiveGoal(app, chat.id);
			if (chat.small_model_mode && active_goal == nullptr && !goal_mode && goal_id.empty())
			{
				std::string created_goal_id;
				if (!GoalService::CreateGoal(app, chat.id, queued.text, 0, &created_goal_id) ||
				    !GoalService::SetActiveGoal(app, chat.id, created_goal_id))
				{
					if (error_out != nullptr)
					{
						*error_out = "Failed to start the small-model workflow.";
					}
					return false;
				}
				SaveChatQuietly(app, chat);
				active_goal = GoalService::FindActiveGoal(app, chat.id);
			}
			queued.attachments = attachments;
			queued.goal_mode = goal_mode || active_goal != nullptr;
			queued.goal_id = goal_id.empty() && active_goal != nullptr ? active_goal->id : goal_id;
			return true;
		}

		bool CanMergeQueuedUserPrompts(const AcpQueuedUserPromptState& target, const AcpQueuedUserPromptState& next)
		{
			return target.append_user_message && next.append_user_message &&
			       !target.priority_steer && !next.priority_steer &&
			       target.goal_mode == next.goal_mode && target.goal_id == next.goal_id;
		}

		bool QueuedMarkdownStoreContentFits(const std::deque<AcpQueuedUserPromptState>& existing, const AcpQueuedUserPromptState& next, bool merge_with_back, std::string* error_out)
		{
			std::size_t markdown_store_bytes = 0;
			const auto add_prompt_block = [&](const std::string& prompt_block)
			{
				if (prompt_block.size() > kMarkdownStorePromptMaxBytes - markdown_store_bytes)
				{
					if (error_out != nullptr)
						*error_out = "Queued attached skill content exceeds the 2 MiB prompt limit.";
					return false;
				}
				markdown_store_bytes += prompt_block.size();
				return true;
			};

			for (const AcpQueuedUserPromptState& queued : existing)
			{
				for (const std::string& prompt_block : queued.markdown_store_prompt_blocks)
				{
					if (!add_prompt_block(prompt_block))
						return false;
				}
			}
			for (std::size_t index = 0; index < next.markdown_store_prompt_blocks.size(); ++index)
			{
				if (merge_with_back && uam::ranges::Contains(existing.back().markdown_store_files, next.markdown_store_files[index]))
				{
					continue;
				}
				if (!add_prompt_block(next.markdown_store_prompt_blocks[index]))
					return false;
			}
			return true;
		}

		void MergeQueuedUserPrompt(AcpQueuedUserPromptState& target, AcpQueuedUserPromptState&& next)
		{
			target.text += "\n\n" + next.text;
			const bool can_merge_snapshots = target.markdown_store_prompt_blocks.size() == target.markdown_store_files.size() && next.markdown_store_prompt_blocks.size() == next.markdown_store_files.size();
			for (std::size_t index = 0; index < next.markdown_store_files.size(); ++index)
			{
				std::string& file = next.markdown_store_files[index];
				if (!uam::ranges::Contains(target.markdown_store_files, file))
				{
					target.markdown_store_files.push_back(std::move(file));
					if (can_merge_snapshots)
					{
						target.markdown_store_prompt_blocks.push_back(std::move(next.markdown_store_prompt_blocks[index]));
					}
			}
			}
			if (!can_merge_snapshots)
				target.markdown_store_prompt_blocks.clear();
			target.attachments.insert(target.attachments.end(),
			                          std::make_move_iterator(next.attachments.begin()),
			                          std::make_move_iterator(next.attachments.end()));
		}
	} // namespace

	bool SendAcpPrompt(AppState& app, const std::string& chat_id, const std::string& text, const std::vector<std::string>& markdown_store_files, const std::vector<MessageAttachment>& attachments, bool goal_mode, std::string* error_out, const std::string& goal_id)
	{
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
		AcpSessionState& session = EnsureAcpSessionForChat(app, chat);
		AcpQueuedUserPromptState queued;
		if (!BuildQueuedAcpUserPrompt(app, chat, text, markdown_store_files, attachments, goal_mode, goal_id, queued, error_out))
		{
			return false;
		}
		if (uam::AcpSessionHasPendingCancel(session))
		{
			const std::string provider_id = session.provider_id;
			const std::string protocol_kind = session.protocol_kind;
			std::deque<AcpQueuedUserPromptState> queued_prompts = std::move(session.queued_user_prompts);
			if (!StopAcpSession(app, chat_id))
			{
				session.queued_user_prompts = std::move(queued_prompts);
				if (error_out != nullptr)
				{
					*error_out = "Failed to restart ACP session after cancelling the previous turn.";
				}
				return false;
			}

			session.provider_id = provider_id;
			session.protocol_kind = protocol_kind;
			session.queued_user_prompts = std::move(queued_prompts);
			if (!session.queued_user_prompts.empty() && !DrainNextQueuedAcpUserPrompt(app, session, chat))
			{
				if (error_out != nullptr)
				{
					*error_out = uam::strings::NonEmptyOrFallback(session.last_error, "Failed to restart ACP session after cancelling the previous turn.");
				}
				return false;
			}
		}
		const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
		const bool copilot = uam::provider_ids::IsCliProviderAliasOf(provider.id, uam::provider_ids::kCopilotCli);
		if (!session.running && !session.processing && copilot)
		{
			const std::string compatibility_error = CopilotLaunchBlockReason(app);
			if (IsCopilotCompatibilityCheckPending(app))
			{
				if (session.queued_user_prompts.empty())
				{
					if (!PrepareCliTerminalForAcpLaunch(app, chat.id, error_out))
					{
						return false;
					}
					session.queued_user_prompts.push_back(std::move(queued));
					session.reconnect_pending = true;
					session.reconnect_attempts = 0;
					session.reconnect_not_before_time_s = 0.0;
					session.lifecycle_state = kAcpLifecycleStarting;
					session.last_error.clear();
					return true;
				}
			}
			else if (!compatibility_error.empty())
			{
				session.lifecycle_state = kAcpLifecycleError;
				session.last_error = compatibility_error;
				if (error_out != nullptr)
					*error_out = compatibility_error;
				return false;
			}
		}
		if (session.processing || !session.queued_user_prompts.empty())
		{
			const bool merge_with_back = !chat.small_model_mode && !session.queued_user_prompts.empty() && CanMergeQueuedUserPrompts(session.queued_user_prompts.back(), queued);
			if (!chat.small_model_mode && !QueuedMarkdownStoreContentFits(session.queued_user_prompts, queued, merge_with_back, error_out))
			{
				return false;
			}
			if (merge_with_back)
			{
				MergeQueuedUserPrompt(session.queued_user_prompts.back(), std::move(queued));
			}
			else
			{
				session.queued_user_prompts.push_back(std::move(queued));
			}
			if (!session.running && copilot && CopilotLaunchBlockReason(app).empty())
			{
				session.reconnect_pending = true;
				session.reconnect_attempts = 0;
				session.reconnect_not_before_time_s = 0.0;
			}
			return true;
		}
		return StartAcpUserPrompt(app, session, chat, queued, error_out);
	}

	bool SteerAcpPrompt(AppState& app, const std::string& chat_id, const std::string& text, const std::vector<std::string>& markdown_store_files, const std::vector<MessageAttachment>& attachments, bool goal_mode, std::string* error_out, const std::string& goal_id)
	{
		ChatSession* chat = ChatDomainService().FindChatById(app, chat_id);
		if (chat == nullptr)
		{
			if (error_out != nullptr)
			{
				*error_out = "Chat not found: " + chat_id;
			}
			return false;
		}

		AcpQueuedUserPromptState steering_prompt;
		if (!BuildQueuedAcpUserPrompt(app, *chat, text, markdown_store_files, attachments, goal_mode, goal_id, steering_prompt, error_out))
		{
			return false;
		}
		steering_prompt.priority_steer = true;

		AcpSessionState& session = EnsureAcpSessionForChat(app, *chat);
		if (uam::AcpSessionHasDeferredUserQueueOnly(session))
		{
			session.queued_user_prompts.push_front(std::move(steering_prompt));
			return true;
		}
		if (!uam::AcpSessionHasActiveTurn(session))
		{
			return StartAcpUserPrompt(app, session, *chat, steering_prompt, error_out);
		}

		std::deque<AcpQueuedUserPromptState> existing_queue = std::move(session.queued_user_prompts);
		if (!CancelAcpTurn(app, chat_id, error_out))
		{
			session.queued_user_prompts = std::move(existing_queue);
			return false;
		}
		session.queued_user_prompts = std::move(existing_queue);
		session.queued_user_prompts.push_front(std::move(steering_prompt));
		return uam::AcpSessionHasPendingCancel(session) || DrainNextQueuedAcpUserPrompt(app, session, *chat);
	}

	bool RemoveQueuedAcpPrompt(AppState& app, const std::string& chat_id, std::size_t index, std::string* error_out)
	{
		AcpSessionState* session = FindAcpSessionForChat(app, chat_id);
		if (session == nullptr || index >= session->queued_user_prompts.size())
		{
			if (error_out != nullptr) *error_out = "Queued prompt not found.";
			return false;
		}
		session->queued_user_prompts.erase(session->queued_user_prompts.begin() + static_cast<std::ptrdiff_t>(index));
		if (!session->running && session->queued_user_prompts.empty() && !uam::AcpSessionHasActiveTurn(*session))
		{
			session->reconnect_pending = false;
			session->reconnect_attempts = 0;
			session->reconnect_not_before_time_s = 0.0;
			session->lifecycle_state = kAcpLifecycleStopped;
			session->last_error.clear();
		}
		return true;
	}

	bool SteerQueuedAcpPrompt(AppState& app, const std::string& chat_id, std::size_t index, std::string* error_out)
	{
		ChatSession* chat = ChatDomainService().FindChatById(app, chat_id);
		AcpSessionState* session = FindAcpSessionForChat(app, chat_id);
		if (chat == nullptr || session == nullptr || index >= session->queued_user_prompts.size())
		{
			if (error_out != nullptr) *error_out = "Queued prompt not found.";
			return false;
		}
		const bool deferred_queue = uam::AcpSessionHasDeferredUserQueueOnly(*session);
		AcpQueuedUserPromptState prompt = std::move(session->queued_user_prompts[index]);
		session->queued_user_prompts.erase(session->queued_user_prompts.begin() + static_cast<std::ptrdiff_t>(index));
		prompt.priority_steer = true;
		if (deferred_queue)
		{
			session->queued_user_prompts.push_front(std::move(prompt));
			return true;
		}
		if (!uam::AcpSessionHasActiveTurn(*session)) return StartAcpUserPrompt(app, *session, *chat, prompt, error_out);
		std::deque<AcpQueuedUserPromptState> remaining_queue = std::move(session->queued_user_prompts);
		if (!CancelAcpTurn(app, chat_id, error_out))
		{
			session->queued_user_prompts = std::move(remaining_queue);
			const std::size_t restored_index = std::min(index, session->queued_user_prompts.size());
			session->queued_user_prompts.insert(session->queued_user_prompts.begin() + static_cast<std::ptrdiff_t>(restored_index), std::move(prompt));
			return false;
		}
		session->queued_user_prompts = std::move(remaining_queue);
		session->queued_user_prompts.push_front(std::move(prompt));
		return uam::AcpSessionHasPendingCancel(*session) || DrainNextQueuedAcpUserPrompt(app, *session, *chat);
	}

	bool StartAcpModelDiscovery(AppState& app, const std::string& chat_id, std::string* error_out, bool stop_when_complete)
	{
		ChatSession* chat = ChatDomainService().FindChatById(app, chat_id);
		if (chat == nullptr)
		{
			if (error_out != nullptr) *error_out = "Chat not found: " + chat_id;
			return false;
		}
		AcpSessionState& session = EnsureAcpSessionForChat(app, *chat);
		if (session.running)
		{
			if (!session.available_models.empty())
			{
				if (app.provider_model_catalog != nullptr)
				{
					nlohmann::json models = nlohmann::json::array();
					for (const AcpModelState& model : session.available_models)
					{
						models.push_back({{"id", model.id}, {"name", model.name}, {"description", model.description}, {"defaultReasoningEffort", model.default_reasoning_effort}, {"supportedReasoningEfforts", model.supported_reasoning_efforts}, {"additionalSpeedTiers", model.additional_speed_tiers}});
					}
					(void)app.provider_model_catalog->RememberSuccessfulModels(session.provider_id, models);
				}
				return true;
			}
			if (uam::AcpSessionHasPendingRuntimeRequest(session) || !session.session_ready) return true;
			if (uam::AcpSessionHasActiveTurn(session))
			{
				if (error_out != nullptr) *error_out = "Model discovery is unavailable during an active provider turn; retry when it finishes.";
				return false;
			}
			(void)StopAcpSession(app, chat_id);
		}
		if (!StartAcpProcessForChat(app, session, *chat, error_out))
		{
			return false;
		}
		session.model_discovery_only = stop_when_complete;
		return true;
	}

	bool DrainNextQueuedAcpUserPrompt(AppState& app, AcpSessionState& session, ChatSession& chat)
	{
		if (session.queued_user_prompts.empty())
		{
			return false;
		}
		std::deque<AcpQueuedUserPromptState> batch;
		if (chat.small_model_mode || session.queued_user_prompts.front().priority_steer) batch.push_back(session.queued_user_prompts.front());
		else batch = session.queued_user_prompts;
		std::string error;
		if (!StartAcpUserPromptBatch(app, session, chat, batch, &error))
		{
			session.last_error = uam::strings::NonEmptyOrFallback(error, "Failed to deliver queued prompt.");
			session.lifecycle_state = kAcpLifecycleError;
			return false;
		}
		for (std::size_t index = 0; index < batch.size(); ++index)
		{
			session.queued_user_prompts.pop_front();
		}
		return true;
	}

	namespace acp_detail
	{

	bool ResumeQueuedUserPromptsAfterSessionSetup(AppState& app, AcpSessionState& session, ChatSession& chat)
	{
		if (!session.session_ready || session.model_discovery_only)
		{
			return false;
		}
		if (session.processing)
		{
			return SendQueuedPromptIfReady(session, chat);
		}
		return DrainNextQueuedAcpUserPrompt(app, session, chat);
	}

	} // namespace acp_detail

	bool SendAcpPrompt(AppState& app, const std::string& chat_id, const std::string& text, std::string* error_out)
	{
		return SendAcpPrompt(app, chat_id, text, std::vector<std::string>{}, std::vector<MessageAttachment>{}, false, error_out);
	}

	bool RetryLastAcpPrompt(AppState& app, const std::string& chat_id, std::string* error_out)
	{
		ChatSession* chat = ChatDomainService().FindChatById(app, chat_id);
		if (chat == nullptr || chat->messages.empty() || chat->messages.back().role != MessageRole::User)
		{
			if (error_out != nullptr)
			{
				*error_out = "The message to retry is no longer available.";
			}
			return false;
		}

		const Message& message = chat->messages.back();
		AcpQueuedUserPromptState queued;
		if (chat->messages.size() > 1)
		{
			queued.text = "Continue this branched conversation using the prior transcript as context.\n\n"
			              "Prior conversation:\n";
			for (auto prior = chat->messages.begin(); prior != chat->messages.end() - 1; ++prior)
			{
				if (!uam::strings::IsBlank(prior->content))
				{
					queued.text += RoleToString(prior->role) + ": " + prior->content + "\n\n";
				}
			}
			queued.text += "Current user message:\n" + message.content;
		}
		else
		{
			queued.text = message.content;
		}
		queued.markdown_store_files = message.markdown_store_files;
		queued.markdown_store_prompt_blocks = message.markdown_store_prompt_blocks;
		queued.attachments = message.attachments;
		queued.append_user_message = false;

		AcpSessionState& session = EnsureAcpSessionForChat(app, *chat);
		return StartAcpUserPrompt(app, session, *chat, queued, error_out);
	}

	bool CancelAcpTurn(AppState& app, const std::string& chat_id, std::string* error_out)
	{
		AcpSessionState* session = FindAcpSessionForChat(app, chat_id);
		if (session == nullptr)
		{
			return true;
		}
		session->queued_user_prompts.clear();
		session->reconnect_pending = false;
		session->reconnect_attempts = 0;
		session->reconnect_not_before_time_s = 0.0;
		ChatSession* chat = ChatDomainService().FindChatById(app, chat_id);
		if (chat != nullptr && acp_detail::FinalizeActiveAcpToolCallsAsCancelled(*chat, *session))
		{
			acp_detail::SaveChatQuietly(app, *chat);
		}
		if (chat != nullptr)
		{
			if (Goal* active_goal = GoalService::FindActiveGoal(app, chat_id); active_goal != nullptr && GoalService::IsProviderManaged(*active_goal))
			{
				(void)GoalService::UpdateGoalStatus(app, active_goal->id, GoalStatus::Paused);
				acp_detail::SaveChatQuietly(app, *chat);
			}
		}
		if (session->running)
		{
			acp_detail::CancelPendingAcpPermissions(*session, error_out);
		}
		const std::string pending_user_input_request_id = session->pending_user_input.request_id_json;
		if (session->running && !pending_user_input_request_id.empty())
		{
			(void)acp_detail::SendCodexUserInputResponse(*session, pending_user_input_request_id, {}, error_out);
		}

		session->queued_prompt.clear();
		session->processing = false;
		session->cancel_requested = true;
		session->cancel_requested_time_s = GetAppTimeSeconds();
		session->goal_resume_suppressed = true;
		ResetAcpPendingInteractionState(*session);
		session->current_assistant_message_index = -1;
		session->pending_assistant_thoughts.clear();
		session->lifecycle_state = session->session_ready ? kAcpLifecycleReady : kAcpLifecycleStopped;
		if (!session->running)
		{
			session->cancel_requested = false;
			session->cancel_requested_time_s = 0.0;
			session->lifecycle_state = kAcpLifecycleStopped;
			return true;
		}

		if (session->session_id.empty())
		{
			return StopAcpSession(app, chat_id);
		}

		const IProviderRuntime& cancel_runtime = ProviderRuntimeRegistry::ResolveById(session->provider_id);
		int cancel_id = session->next_request_id++;
		std::string cancel_method;
		nlohmann::json cancel_msg = cancel_runtime.OnAcpBuildCancel(*session, cancel_id, cancel_method);
		if (cancel_msg.is_null() || cancel_msg.empty())
		{
			if (std::strcmp(cancel_runtime.AcpProtocolKind(), "codex-app-server") == 0)
			{
				return true;
			}
			return StopAcpSession(app, chat_id);
		}
		if (!cancel_method.empty())
		{
			if (session->prompt_request_id != 0)
			{
				session->pending_request_methods.erase(session->prompt_request_id);
				session->prompt_request_id = 0;
			}
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

		return true;
	}

	bool StopAcpSession(AppState& app, const std::string& chat_id)
	{
		AcpSessionState* session = FindAcpSessionForChat(app, chat_id);
		if (session == nullptr)
		{
			return true;
		}
		if (ChatSession* chat = ChatDomainService().FindChatById(app, chat_id); chat != nullptr && acp_detail::FinalizeActiveAcpToolCallsAsCancelled(*chat, *session))
		{
			acp_detail::SaveChatQuietly(app, *chat);
		}

		if (session->running)
		{
			PlatformServicesFactory::Instance().process_service.StopStdioProcess(*session, true);
		}

		session->running = false;
		session->initialized = false;
		session->session_ready = false;
		session->model_discovery_only = false;
		session->processing = false;
		session->cancel_requested = false;
		session->cancel_requested_time_s = 0.0;
		session->lifecycle_state = kAcpLifecycleStopped;
		session->queued_prompt.clear();
		session->queued_user_prompts.clear();
		session->reconnect_pending = false;
		session->reconnect_attempts = 0;
		session->reconnect_not_before_time_s = 0.0;
		ClearAcpStartupModelRequest(*session);
		ClearAcpReasoningChangeRequest(*session);
		ClearAcpModeChangeRequest(*session);
		ClearAcpModelChangeRequest(*session);
		session->awaiting_model_config_options = false;
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

	bool SetAcpSessionMode(AppState& app,
	                       const std::string& chat_id,
	                       const std::string& mode_id,
	                       std::string* error_out,
	                       std::optional<std::string> previous_chat_mode_id,
	                       std::optional<std::string> previous_command_safety_tier)
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
		if (session->mode_change_request_id != 0 || session->model_change_request_id != 0 || session->reasoning_change_request_id != 0 || session->awaiting_model_config_options)
		{
			if (error_out != nullptr)
			{
				*error_out = "Cannot change structured runtime mode while another session setting change is pending.";
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
			const std::string previous_mode_id = session->current_mode_id;
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
			session->mode_change_request_id = id;
			session->mode_change_previous_id = previous_mode_id;
			session->mode_change_previous_chat_id = std::move(previous_chat_mode_id);
			session->mode_change_previous_command_safety_tier = std::move(previous_command_safety_tier);
			session->mode_change_requested_id = mode_id;
			if (!acp_detail::WriteAcpMessage(*session, BuildSetModeRequest(id, session->session_id, ProviderApprovalModeId(*session, mode_id)), error_out))
			{
				session->pending_request_methods.erase(id);
				if (ChatSession* chat = ChatDomainService().FindChatById(app, chat_id); chat != nullptr)
				{
					if (RollbackAcpModeChange(*session, *chat))
					{
						SaveChatQuietly(app, *chat);
					}
				}
				else
				{
					session->current_mode_id = session->mode_change_previous_id;
					ClearAcpModeChangeRequest(*session);
				}
				return false;
			}
			session->current_mode_id = mode_id;
			session->last_runtime_activity_time_s = GetAppTimeSeconds();
			return true;
		}
	}

	bool SetAcpSessionReasoningEffort(AppState& app, const std::string& chat_id, const std::string& reasoning_effort, std::string* error_out, std::optional<std::string> previous_chat_reasoning_effort)
	{
		AcpSessionState* session = FindAcpSessionForChat(app, chat_id);
		if (session == nullptr || !session->running)
		{
			return true;
		}
		const bool prompt_is_queued_but_not_sent = session->processing && session->prompt_request_id == 0 && !session->queued_prompt.empty() && !session->waiting_for_permission && !session->waiting_for_user_input;
		if (uam::AcpSessionHasCancelableWork(*session) && !prompt_is_queued_but_not_sent)
		{
			if (error_out != nullptr)
			{
				*error_out = "Cannot change Copilot reasoning effort while Copilot is busy.";
			}
			return false;
		}
		if (session->reasoning_change_request_id != 0 || session->startup_model_request_id != 0 || session->mode_change_request_id != 0 || session->model_change_request_id != 0 || session->awaiting_model_config_options)
		{
			if (error_out != nullptr)
			{
				*error_out = "Cannot change Copilot reasoning effort while another session setting change is pending.";
			}
			return false;
		}
		if (!session->session_ready || session->session_id.empty())
		{
			if (error_out != nullptr)
			{
				*error_out = "Copilot ACP session is not ready.";
			}
			return false;
		}

		AcpModelState* selected_model = nullptr;
		for (AcpModelState& model : session->available_models)
		{
			if (model.id == session->current_model_id)
			{
				selected_model = &model;
				break;
			}
		}
		if (selected_model == nullptr || !uam::ranges::Contains(selected_model->supported_reasoning_efforts, reasoning_effort))
		{
			if (error_out != nullptr)
			{
				*error_out = "The selected Copilot model does not support that reasoning effort.";
			}
			return false;
		}
		if (selected_model->default_reasoning_effort == reasoning_effort)
		{
			return true;
		}

		const int id = NextAcpRequestId(*session, uam::acp_methods::kSessionSetConfigOption);
		session->reasoning_change_request_id = id;
		session->reasoning_change_previous_id = selected_model->default_reasoning_effort;
		session->reasoning_change_previous_chat_id = previous_chat_reasoning_effort;
		session->reasoning_change_requested_id = reasoning_effort;
		if (!acp_detail::WriteAcpMessage(*session, BuildSetConfigOptionRequest(id, session->session_id, "reasoning_effort", reasoning_effort), error_out))
		{
			session->pending_request_methods.erase(id);
			ClearAcpReasoningChangeRequest(*session);
			return false;
		}
		selected_model->default_reasoning_effort = reasoning_effort;
		return true;
	}

	bool SetAcpSessionModel(AppState& app, const std::string& chat_id, const std::string& model_id, std::string* error_out, std::optional<std::string> previous_chat_model_id)
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
		if (session->model_change_request_id != 0 || session->mode_change_request_id != 0 || session->reasoning_change_request_id != 0 || session->awaiting_model_config_options)
		{
			if (error_out != nullptr)
			{
				*error_out = "Cannot change structured runtime model while another session setting change is pending.";
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
			const std::string previous_model_id = session->current_model_id;
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
			session->model_change_request_id = id;
			session->model_change_previous_id = previous_model_id;
			session->model_change_previous_chat_id = previous_chat_model_id.value_or(previous_model_id);
			session->model_change_requested_id = model_id;
			session->awaiting_model_config_options = uam::provider_ids::IsCliProviderAliasOf(session->provider_id, uam::provider_ids::kCopilotCli);
			if (!acp_detail::WriteAcpMessage(*session, BuildSetModelRequest(id, session->session_id, model_id), error_out))
			{
				session->pending_request_methods.erase(id);
				session->current_model_id = session->model_change_previous_id;
				ClearAcpModelChangeRequest(*session);
				session->awaiting_model_config_options = false;
				return false;
			}
			session->current_model_id = model_id;
			session->last_runtime_activity_time_s = GetAppTimeSeconds();
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

		if (ChatSession* chat = ChatDomainService().FindChatById(app, chat_id); chat != nullptr)
		{
			AdvanceAcpPermissionQueue(*session, *chat, error_out);
		}
		else
		{
			session->pending_permission = AcpPendingPermissionState{};
			session->queued_permissions.clear();
			session->waiting_for_permission = false;
			ClearAcpPendingWait(*session);
		}
		session->cancel_requested = false;
		session->cancel_requested_time_s = 0.0;
		if (!session->waiting_for_permission)
		{
			session->lifecycle_state = session->processing ? kAcpLifecycleProcessing : kAcpLifecycleReady;
		}
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
				if (reconnect_chat != nullptr && session.reconnect_pending && !session.queued_user_prompts.empty())
				{
					const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, *reconnect_chat);
					if (uam::provider_ids::IsCliProviderAliasOf(provider.id, uam::provider_ids::kCopilotCli))
					{
						if (IsCopilotCompatibilityCheckPending(app))
						{
							continue;
						}
						const std::string compatibility_error = CopilotLaunchBlockReason(app);
						if (!compatibility_error.empty())
						{
							if (session.last_error != compatibility_error || session.lifecycle_state != kAcpLifecycleError)
							{
								session.lifecycle_state = kAcpLifecycleError;
								session.last_error = compatibility_error;
								changed = true;
							}
							continue;
						}
					}
				}
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
			const double now_seconds = GetAppTimeSeconds();
			changed = DrainStderr(session) || changed;
			changed = DrainStdout(app, session, chat, browser) || changed;
			if (!session.running)
			{
				continue;
			}
			if (uam::AcpSessionHasPendingCancel(session) && session.cancel_requested_time_s > 0.0 && now_seconds - session.cancel_requested_time_s >= kAcpCancelTimeoutSeconds)
			{
				std::deque<AcpQueuedUserPromptState> queued = std::move(session.queued_user_prompts);
				AppendAcpDiagnostic(session, "cancel", "timeout_restart", "", "", false, 0, "Cancellation timed out; restarting the structured runtime.");
				(void)StopAcpSession(app, session.chat_id);
				session.queued_user_prompts = std::move(queued);
				if (!session.queued_user_prompts.empty())
				{
					(void)DrainNextQueuedAcpUserPrompt(app, session, chat);
				}
				changed = true;
				continue;
			}
			if (HandleAcpSetupInactivityTimeout(app, session, chat, now_seconds))
			{
				changed = true;
				continue;
			}
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
				const bool model_discovery_only = session.model_discovery_only;
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
				if (model_discovery_only)
				{
					session.model_discovery_only = false;
					session.session_id.clear();
					session.codex_thread_id.clear();
					if (app.provider_model_catalog != nullptr)
					{
						app.provider_model_catalog->RememberRefreshFailure(session.provider_id, std::string(RuntimeDisplayName(session)) + " exited before model discovery completed.");
					}
					changed = true;
					continue;
				}

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
			if (chat == nullptr)
			{
				app.pending_chat_save_at_by_chat_id.erase(chat_id);
				continue;
			}
			if (ChatRepository::SaveChat(app.data_root, *chat))
			{
				app.pending_chat_save_at_by_chat_id.erase(chat_id);
			}
			else
			{
				app.pending_chat_save_at_by_chat_id[chat_id] = now + 1.0;
			}
		}
	}

} // namespace uam
