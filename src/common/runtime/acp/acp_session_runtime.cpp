#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_goal_loop.h"
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
		ResetAcpPendingInteractionState(*session);
		session->current_assistant_message_index = -1;
		session->pending_assistant_thoughts.clear();
		session->lifecycle_state = session->session_ready ? kAcpLifecycleReady : kAcpLifecycleStopped;

		if (IsCodexSession(*session) && !session->session_id.empty() && !session->codex_turn_id.empty())
		{
			const int id = NextAcpRequestId(*session, uam::acp_methods::kTurnInterrupt);
			session->cancel_request_id = id;
			if (!acp_detail::WriteAcpMessage(*session, BuildCodexTurnInterruptRequest(id, session->session_id, session->codex_turn_id), error_out))
			{
				session->pending_request_methods.erase(id);
				session->cancel_request_id = 0;
				return false;
			}
		}
		else if (!session->session_id.empty())
		{
			if (!acp_detail::WriteAcpMessage(*session, BuildCancelNotification(session->session_id), error_out))
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
		if (!acp_detail::WriteAcpMessage(*session, BuildSetModeRequest(id, session->session_id, ProviderApprovalModeId(*session, mode_id)), error_out))
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
		if (!acp_detail::WriteAcpMessage(*session, BuildSetModelRequest(id, session->session_id, model_id), error_out))
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
