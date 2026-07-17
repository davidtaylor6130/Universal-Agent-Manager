#include "common/runtime/acp/acp_codex_message_handlers.h"
#include "common/runtime/acp/acp_goal_loop.h"
#include "common/runtime/acp/acp_session_internal.h"

#include "cef/cef_push.h"
#include "common/runtime/acp/acp_attention_kind.h"
#include "common/runtime/acp/acp_content.h"
#include "common/runtime/acp/acp_permissions.h"
#include "common/runtime/acp/acp_protocol_methods.h"
#include "common/runtime/acp/acp_statuses.h"
#include "common/runtime/acp/acp_tool_items.h"
#include "common/utils/string_utils.h"

#include <string>
#include <string_view>
#include <vector>

namespace uam::acp_detail
{

namespace
{

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
	ApplySubAgentMetadata(tool_call, item, ProviderRuntimeRegistry::ResolveById(session.provider_id));
	AppendToolTurnEventIfNeeded(session, item_id);
	(void)SyncAcpToolCallsToAssistantMessage(chat, session, false);
}

void HandleCodexPendingPermission(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message, const std::string& kind)
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
	ApplyCommandSafetyDecision(app, chat, pending);
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

} // anonymous namespace

void HandleCodexMessage(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message, CefRefPtr<CefBrowser> browser)
{
	const std::string method = JsonDiagnosticStringValue(message, "method");
	const nlohmann::json params = JsonObjectValue(message, "params");
	const bool permission_request = method == uam::acp_methods::kItemCommandExecutionRequestApproval ||
	                                method == uam::acp_methods::kItemFileChangeRequestApproval ||
	                                method == uam::acp_methods::kItemPermissionsRequestApproval;
	if (!permission_request && !uam::AcpSessionHasActiveTurn(session) && (method.rfind("item/", 0) == 0 || method.rfind("turn/", 0) == 0 || method == uam::acp_methods::kError))
	{
		return;
	}

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
		HandleCodexPendingPermission(app, session, chat, message, uam::acp_permissions::kCodexCommandRequestKind);
		return;
	}
	if (method == uam::acp_methods::kItemFileChangeRequestApproval)
	{
		HandleCodexPendingPermission(app, session, chat, message, uam::acp_permissions::kCodexFileRequestKind);
		return;
	}
	if (method == uam::acp_methods::kItemPermissionsRequestApproval)
	{
		HandleCodexPendingPermission(app, session, chat, message, uam::acp_permissions::kCodexPermissionsRequestKind);
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

} // namespace uam::acp_detail
