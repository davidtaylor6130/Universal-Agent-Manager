#include "common/provider/claude/cli/claude_acp_message_handlers.h"
#include "common/provider/claude/cli/claude_tool_projection.h"
#include "common/runtime/acp/acp_goal_loop.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_session_runtime.h"

#include "app/provider_model_catalog_service.h"
#include "cef/cef_push.h"
#include "common/config/approval_modes.h"
#include "common/runtime/acp/acp_claude_stream.h"
#include "common/runtime/acp/acp_content.h"
#include "common/runtime/acp/acp_permissions.h"
#include "common/runtime/acp/acp_statuses.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <string>
#include <vector>

namespace uam::acp_detail
{

	namespace
	{
		/// <summary>Render Claude result blocks without exposing binary payloads or protocol envelopes.</summary>
		std::string ClaudeToolResultText(const nlohmann::json& content)
		{
			if (content.is_string())
			{
				return content.get<std::string>();
			}
			if (content.is_array())
			{
				std::vector<std::string> pieces;
				for (const nlohmann::json& block : content)
				{
					pieces.push_back(ClaudeToolResultText(block));
				}
				return uam::strings::JoinNonEmpty(pieces, "\n");
			}
			if (!content.is_object())
			{
				return content.is_null() ? std::string{} : content.dump();
			}
			const std::string type = JsonDiagnosticStringValue(content, "type");
			if (type == "text")
			{
				return JsonDiagnosticStringValue(content, "text");
			}
			if (type == "image")
			{
				return "[Image]";
			}
			if (type == "document")
			{
				return "[Document]";
			}
			return type.empty() ? content.dump(2) : "[" + type + "]";
		}

		/// <summary>Late task/child events update their original message even after the next turn starts.</summary>
		AcpToolCallState& RestoreClaudeTool(AcpSessionState& session, const ChatSession& chat, const std::string& id)
		{
			for (AcpToolCallState& tool : session.tool_calls) if (tool.id == id) return tool;
			AcpToolCallState& tool = UpsertToolCall(session, id);
			for (std::size_t index = 0; index < chat.messages.size(); ++index)
			{
				for (const ToolCall& saved : chat.messages[index].tool_calls)
				{
					if (saved.id != id) continue;
					tool.title = saved.name;
					tool.kind = saved.kind;
					tool.status = saved.status;
					tool.args_json = saved.args_json;
					tool.content = saved.result_text;
					tool.is_sub_agent = saved.is_sub_agent;
					tool.sub_agent_id = saved.sub_agent_id;
					tool.sub_agent_title = saved.sub_agent_title;
					tool.approval_status = saved.approval_status;
					tool.task_id = saved.task_id;
					tool.child_activity = saved.child_activity;
					session.tool_call_message_indices[id] = static_cast<int>(index);
					return tool;
				}
			}
			return tool;
		}

		bool HandleClaudeTaskEvent(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message)
		{
			const std::string subtype = JsonDiagnosticStringValue(message, "subtype");
			if (subtype != "task_started" && subtype != "task_progress" && subtype != "task_notification" && subtype != "task_updated") return false;
			if (subtype == "task_updated")
			{
				const std::string status = JsonDiagnosticStringValueOr(JsonObjectValue(message, "patch"), "status", JsonDiagnosticStringValue(message, "status"));
				if (status != "completed" && status != "failed" && status != "stopped" && status != "cancelled" && status != "killed" && status != "pending" && status != "running" && status != "paused") return true;
			}
			const std::string task_id = JsonDiagnosticStringValue(message, "task_id");
			std::string tool_id = JsonDiagnosticStringValue(message, "tool_use_id");
			if (tool_id.empty() && !task_id.empty())
			{
				for (const AcpToolCallState& tool : session.tool_calls) if (tool.task_id == task_id) tool_id = tool.id;
				if (tool_id.empty()) for (const Message& saved : chat.messages)
					for (const ToolCall& tool : saved.tool_calls) if (tool.task_id == task_id) tool_id = tool.id;
			}
			if (tool_id.empty())
			{
				if (task_id.empty()) return true;
				tool_id = "claude-task:" + task_id;
			}
			AcpToolCallState& tool = RestoreClaudeTool(session, chat, tool_id);
			const bool first_task_event = tool.task_id.empty() && !task_id.empty();
			if (!task_id.empty()) tool.task_id = task_id;
			if (tool.title.empty()) tool.title = JsonDiagnosticStringValueOr(message, "description", "Background task");
			if (JsonDiagnosticStringValue(message, "task_type") == "local_agent") tool.is_sub_agent = true;
			if (subtype == "task_notification" || subtype == "task_updated")
			{
				const nlohmann::json patch = JsonObjectValue(message, "patch");
				const std::string status = JsonDiagnosticStringValueOr(patch, "status", JsonDiagnosticStringValue(message, "status"));
				if (status == "completed") tool.status = uam::acp_statuses::kCompleted;
				else if (status == "failed") tool.status = uam::acp_statuses::kFailed;
				else if (status == "stopped" || status == "cancelled" || status == "killed") tool.status = uam::acp_statuses::kCancelled;
				else if (status == "pending" || status == "running" || status == "paused")
				{
					if (tool.status.empty() || tool.status == "paused" || uam::acp_statuses::IsActiveStatus(tool.status)) tool.status = status;
				}
				else return true;
				const std::string summary = JsonDiagnosticStringValue(message, "summary");
				if (!summary.empty() && !tool.content.ends_with(summary))
				{
					if (!tool.content.empty()) tool.content += "\n\n";
					tool.content += summary;
				}
			}
			else if (tool.status.empty() || tool.status == "paused" || uam::acp_statuses::IsActiveStatus(tool.status) ||
			         (first_task_event && !uam::AcpSessionHasPendingCancel(session))) tool.status = uam::acp_statuses::kRunning;
			AppendToolTurnEventIfNeeded(session, tool_id);
			(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
			SaveChatQuietly(app, chat);
			MarkAcpChatUnseenIfBackground(app, chat);
			return true;
		}

		/// <summary>Child narration and tools belong to the launching tool, never the parent timeline.</summary>
		bool HandleClaudeChildMessage(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message)
		{
			const std::string parent = JsonDiagnosticStringValue(message, "parent_tool_use_id");
			if (parent.empty()) return false;
			const std::string type = JsonDiagnosticStringValue(message, "type");
			if (type != "assistant" && type != "user") return false;
			bool known_parent = std::ranges::any_of(session.tool_calls, [&parent](const AcpToolCallState& tool) { return tool.id == parent; });
			if (!known_parent)
			{
				for (const Message& saved : chat.messages)
					if (std::ranges::any_of(saved.tool_calls, [&parent](const ToolCall& tool) { return tool.id == parent; })) { known_parent = true; break; }
			}
			if (!known_parent) return true;
			const std::string identity = JsonDiagnosticStringValue(message, "uuid");
			if (!identity.empty() && !session.claude_seen_child_messages.insert(identity).second) return true;
			AcpToolCallState& tool = RestoreClaudeTool(session, chat, parent);
			tool.is_sub_agent = true;
			if (tool.title.empty()) tool.title = "Sub-agent";
			const nlohmann::json content = uam::nlohmann_json::ValueOrNull(uam::nlohmann_json::FindField(JsonObjectValue(message, "message"), "content"));
			if (content.is_string() && !content.get_ref<const std::string&>().empty())
			{
				if (!tool.child_activity.empty()) tool.child_activity += "\n\n";
				tool.child_activity += content.get_ref<const std::string&>();
			}
			for (const nlohmann::json& block : JsonArrayValue(JsonObjectValue(message, "message"), "content"))
			{
				const std::string block_type = JsonDiagnosticStringValue(block, "type");
				std::string activity;
				if (block_type == "text") activity = ContentTextFromJson(block);
				else if (block_type == "thinking") activity = JsonDiagnosticStringValue(block, "thinking");
				else if (block_type == "tool_use") activity = uam::claude::ToolTitle(JsonDiagnosticStringValue(block, "name"), JsonObjectValue(block, "input"));
				else if (block_type == "tool_result")
				{
					const nlohmann::json output = uam::nlohmann_json::ValueOrNull(uam::nlohmann_json::FindField(block, "content"));
					activity = ClaudeToolResultText(output);
				}
				if (!activity.empty())
				{
					if (!tool.child_activity.empty()) tool.child_activity += "\n\n";
					tool.child_activity += activity;
				}
			}
			AppendToolTurnEventIfNeeded(session, parent);
			(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
			SaveChatQuietly(app, chat);
			return true;
		}

		void HandleClaudeControlRequest(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message)
		{
			const nlohmann::json request = JsonObjectValue(message, "request");
			const std::string id = uam::nlohmann_json::TrimmedStringValue(message, {"request_id"});
			if (id.empty())
				return;
			const std::string subtype = JsonDiagnosticStringValue(request, "subtype");
			if (subtype != "can_use_tool")
			{
				(void)WriteAcpMessage(session, {{"type", "control_response"}, {"response", {{"subtype", "error"}, {"request_id", id}, {"error", "Unsupported Claude control request: " + subtype}}}});
				return;
			}
			if (uam::nlohmann_json::FindObjectField(request, "input") == nullptr || JsonDiagnosticStringValue(request, "tool_name").empty())
			{
				(void)WriteAcpMessage(session, uam::acp_claude_stream::ControlResponse(id, {{"behavior", "deny"}, {"message", "Malformed Claude tool request."}}));
				return;
			}
			const nlohmann::json input = JsonObjectValue(request, "input");
			const std::string tool_name = JsonDiagnosticStringValue(request, "tool_name");
			const std::string tool_id = JsonDiagnosticStringValueOr(request, "tool_use_id", id);
			if (uam::AcpSessionHasPendingCancel(session))
			{
				(void)WriteAcpMessage(session, uam::acp_claude_stream::ControlResponse(id, {{"behavior", "deny"}, {"message", "Turn cancelled."}}));
				return;
			}
			AcpToolCallState& tool = UpsertToolCall(session, tool_id);
			tool.title = uam::claude::ToolTitle(tool_name, input);
			tool.kind = uam::claude::ToolKind(tool_name);
			tool.is_sub_agent = tool.kind == "sub-agent";
			tool.args_json = input.dump();
			tool.status = uam::acp_statuses::kPending;
			AppendToolTurnEventIfNeeded(session, tool_id);
			if (tool_name == "AskUserQuestion")
			{
				if (!session.pending_user_input.request_id_json.empty())
				{
					(void)WriteAcpMessage(session, uam::acp_claude_stream::ControlResponse(id, {{"behavior", "deny"}, {"message", "Another question is awaiting an answer."}}));
					return;
				}
				AcpPendingUserInputState pending;
				pending.request_id_json = JsonRpcIdToStableString(id);
				pending.provider_input_json = input.dump();
				pending.item_id = tool_id;
				pending.status = uam::acp_statuses::kPending;
				pending.attention_kind = "question";
				for (const nlohmann::json& item : JsonArrayValue(input, "questions"))
				{
					if (!item.is_object())
						continue;
					AcpUserInputQuestionState question;
					question.id = std::to_string(pending.questions.size());
					question.header = JsonDiagnosticStringValue(item, "header");
					question.question = JsonDiagnosticStringValue(item, "question");
					question.is_other = true;
					question.is_multiple = item.contains("multiSelect") && item["multiSelect"].is_boolean() && item["multiSelect"].get<bool>();
					for (const nlohmann::json& option : JsonArrayValue(item, "options"))
					{
						if (!option.is_object())
							continue;
						question.options.push_back({JsonDiagnosticStringValue(option, "label"), JsonDiagnosticStringValue(option, "description")});
					}
					if (!question.question.empty())
						pending.questions.push_back(std::move(question));
				}
				if (pending.questions.empty())
				{
					(void)WriteAcpMessage(session, uam::acp_claude_stream::ControlResponse(id, {{"behavior", "deny"}, {"message", "Claude sent no answerable questions."}}));
					return;
				}
				AppendUserInputTurnEventIfNeeded(session, pending.request_id_json, tool_id);
				session.pending_user_input = std::move(pending);
				session.waiting_for_user_input = true;
				BeginAcpPendingWait(session, kAcpLifecycleWaitingUserInput);
			}
			else
			{
				AcpPendingPermissionState pending;
				pending.request_id_json = JsonRpcIdToStableString(id);
				pending.provider_request_method = "can_use_tool";
				pending.provider_request_kind = "claude-tool";
				pending.provider_input_json = input.dump();
				pending.tool_call_id = tool_id;
				pending.title = tool.title;
				pending.kind = tool_name == "Bash" ? "execute" : tool_name == "Edit" || tool_name == "Write" || tool_name == "NotebookEdit" ? "edit" : tool_name;
				pending.status = uam::acp_statuses::kPending;
				pending.content = tool_name == "Bash" ? JsonDiagnosticStringValue(input, "command") : input.dump(2);
				pending.options = {{"allow", "Allow", "allow_once"}, {"deny", "Deny", "reject_once"}};
				ApplyCommandSafetyDecision(app, chat, pending);
				QueueAcpPermission(app, session, chat, std::move(pending));
			}
			(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
			SaveChatQuietly(app, chat);
			MarkAcpChatUnseenIfBackground(app, chat);
		}
	} // namespace

void HandleClaudeAssistantMessage(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message, CefRefPtr<CefBrowser> browser)
{
	const nlohmann::json assistant_message = JsonObjectValue(message, "message");
	const nlohmann::json content = uam::nlohmann_json::ValueOrNull(uam::nlohmann_json::FindField(assistant_message, "content"));
	const bool child_message = !JsonDiagnosticStringValue(message, "parent_tool_use_id").empty();
	if (!content.is_array())
	{
		if (child_message) return;
		const std::string fallback_text = ClaudeContentTextFromMessage(assistant_message);
		if (!fallback_text.empty())
		{
			const std::string appended = AppendAssistantChunk(chat, session, fallback_text);
			if (browser && !appended.empty())
			{
				uam::PushStreamToken(browser, chat.id, session.current_assistant_message_index, appended);
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
			if (child_message)
				continue;
			const std::string text = ContentTextFromJson(item);
			if (!text.empty())
			{
				const std::string appended = AppendAssistantChunk(chat, session, text);
				if (browser && !appended.empty())
				{
					uam::PushStreamToken(browser, chat.id, session.current_assistant_message_index, appended);
				}
				changed = true;
			}
			continue;
		}

		if (type == uam::acp_claude_stream::kContentThinking)
		{
			if (child_message)
				continue;
			const std::string thought = JsonDiagnosticStringValue(item, "thinking");
			if (!thought.empty())
			{
				changed = AppendThoughtChunk(chat, session, thought) || changed;
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

			AcpToolCallState& tool_call = RestoreClaudeTool(session, chat, tool_id);
			const std::string tool_name = JsonDiagnosticStringValueOr(item, "name", tool_call.kind);
			tool_call.kind = uam::claude::ToolKind(tool_name);
			tool_call.title = uam::claude::ToolTitle(tool_name, JsonObjectValue(item, "input"));
			tool_call.is_sub_agent = tool_call.kind == "sub-agent" || tool_call.is_sub_agent;
			if (tool_call.status.empty() || uam::acp_statuses::IsActiveStatus(tool_call.status)) tool_call.status = uam::acp_statuses::kRunning;
			if (const nlohmann::json* input = uam::nlohmann_json::FindField(item, "input"); input != nullptr)
			{
				tool_call.args_json = input->dump();
			}
			ApplySubAgentMetadata(tool_call, item, ProviderRuntimeRegistry::ResolveById(session.provider_id));
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

		AcpToolCallState& tool_call = RestoreClaudeTool(session, chat, tool_id);
		if (JsonBooleanValueOr(item, "is_error", false)) tool_call.status = uam::acp_statuses::kFailed;
		else if (tool_call.task_id.empty()) tool_call.status = uam::acp_statuses::kCompleted;
		const nlohmann::json* content_value = uam::nlohmann_json::FindField(item, "content");
		const std::string result_text = content_value == nullptr ? std::string{} : ClaudeToolResultText(*content_value);
		if (!tool_call.task_id.empty() && !uam::acp_statuses::IsActiveStatus(tool_call.status) && !tool_call.content.empty())
		{
			if (!result_text.empty() && !tool_call.content.ends_with(result_text)) tool_call.content += "\n\n" + result_text;
		}
		else tool_call.content = result_text;
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
		if (!session.goal_internal_session)
		{
			const std::string previous_native_session_id = chat.native_session_id;
			SetChatNativeSessionIdIfChanged(chat, session_id);
			SyncResolvedNativeSessionIdForChat(app, chat, session_id, previous_native_session_id);
		}
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

	const bool has_answer = std::ranges::any_of(session.turn_events, [](const AcpTurnEventState& event)
	{
		return event.type == "assistant_text" && !event.text.empty();
	});
	if (!has_answer && !uam::AcpSessionHasPendingCancel(session) && !JsonBooleanValueOr(message, "is_error", false) && !uam::acp_claude_stream::IsResultErrorSubtype(JsonDiagnosticStringValue(message, "subtype")))
	{
		const std::string result_text = uam::nlohmann_json::TrimmedStringValueOr(message, "result", "");
		if (!result_text.empty())
		{
			AppendAssistantChunk(chat, session, result_text);
		}
	}

	const bool is_error = JsonBooleanValueOr(message, "is_error", false);
	const std::string subtype = JsonDiagnosticStringValue(message, "subtype");
	const bool cancelled = uam::AcpSessionHasPendingCancel(session);
	if (cancelled && session.inactivity_timeout_pending)
	{
		FinalizeAcpTurnInactivityTimeout(app, session, chat);
		return;
	}
	session.pending_request_methods.erase(session.cancel_request_id);
	session.cancel_request_id = 0;
	if (!cancelled && (is_error || uam::acp_claude_stream::IsResultErrorSubtype(subtype)))
	{
		const std::string result_text = uam::strings::JoinNonEmpty(std::vector<std::string>{ContentTextFromJson(JsonArrayValue(message, "errors")), JsonDiagnosticStringValue(message, "error"), uam::nlohmann_json::TrimmedStringValueOr(message, "result", "")}, "\n");
		(void)FinalizeActiveAcpToolCallsAsFailed(chat, session);
		FailAcpTurnOrSession(session, &chat,
		                     uam::strings::NonEmptyOrFallback(result_text, "Claude stream-json turn failed."));
	}
	else
	{
		(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
		CompletePromptTurnAndHandleGoalLoop(app, session, chat, kAcpLifecycleReady, browser, !cancelled);
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
	if (type == "system" && HandleClaudeTaskEvent(app, session, chat, message)) return;
	if (HandleClaudeChildMessage(app, session, chat, message)) return;
	if (type == "user")
	{
		const nlohmann::json notification = uam::claude::TaskNotification(ClaudeContentTextFromMessage(JsonObjectValue(message, "message")));
		if (notification.is_object() && HandleClaudeTaskEvent(app, session, chat, notification)) return;
	}
	if (type == uam::acp_claude_stream::kMessageTypeSystem && JsonDiagnosticStringValue(message, "subtype") == "compact_boundary")
	{
		AppendContextCompactionEvent(app, session, chat, JsonDiagnosticStringValue(message, "summary"), JsonDiagnosticStringValue(message, "uuid"));
		return;
	}
	if (type == "control_response")
	{
		const nlohmann::json response = JsonObjectValue(message, "response");
		const int id = JsonRpcNumericId(uam::nlohmann_json::ValueOrNull(uam::nlohmann_json::FindField(response, "request_id")));
		if (id <= 0 || !session.pending_request_methods.contains(id))
			return;
		if (session.pending_request_methods.at(id) == "claude/interrupt" && JsonDiagnosticStringValue(response, "subtype") == "success")
		{
			// The acknowledgement precedes the terminal result; keep cancellation pending until it arrives.
			session.pending_request_methods.erase(id);
			return;
		}
		const bool initialization = session.pending_request_methods.at(id) == "initialize";
		nlohmann::json normalized = {{"id", id}};
		if (JsonDiagnosticStringValue(response, "subtype") == "error")
			normalized["error"] = {{"message", JsonDiagnosticStringValue(response, "error")}};
		else
			normalized["result"] = JsonObjectValue(response, "response");
		HandleAcpResponse(app, session, chat, normalized);
		if (initialization && session.initialized)
		{
			RememberDiscoveredModels(app, session, chat);
			if (app.provider_model_catalog != nullptr && session.available_models.empty())
			{
				app.provider_model_catalog->RememberRefreshFailure(session.provider_id, "Claude reported no available models.", chat.workspace_directory, chat.execution_host_id);
			}
			StopBackgroundModelDiscovery(app, session);
		}
		return;
	}
	if (type == "control_request")
	{
		if (ReplayPersistedInteractionResponseIfMatched(app, session, chat, {{"id", JsonDiagnosticStringValue(message, "request_id")}}))
			return;
		HandleClaudeControlRequest(app, session, chat, message);
		return;
	}
	if (type == "control_cancel_request")
	{
		const std::string id = JsonRpcIdToStableString(JsonDiagnosticStringValue(message, "request_id"));
		const auto cancel_tool = [&session](const std::string& tool_id)
		{
			for (AcpToolCallState& tool : session.tool_calls)
				if (tool.id == tool_id && uam::acp_statuses::IsActiveStatus(tool.status)) tool.status = uam::acp_statuses::kCancelled;
		};
		if (session.pending_permission.request_id_json == id) cancel_tool(session.pending_permission.tool_call_id);
		for (const AcpPendingPermissionState& queued : session.queued_permissions)
			if (queued.request_id_json == id) cancel_tool(queued.tool_call_id);
		if (session.pending_user_input.request_id_json == id) cancel_tool(session.pending_user_input.item_id);
		std::erase_if(session.queued_permissions, [&id](const AcpPendingPermissionState& pending) { return pending.request_id_json == id; });
		if (session.pending_permission.request_id_json == id)
		{
			StopPermissionReviewTasks(app, chat.id, id);
			AdvanceAcpPermissionQueue(app, session, chat);
		}
		if (session.pending_user_input.request_id_json == id)
		{
			session.pending_user_input = {};
			session.waiting_for_user_input = false;
			if (session.waiting_for_permission)
			{
				BeginAcpPendingWait(session, kAcpLifecycleWaitingPermission);
			}
			else
			{
				ClearAcpPendingWait(session);
				session.lifecycle_state = session.processing ? kAcpLifecycleProcessing : kAcpLifecycleReady;
			}
		}
		(void)SyncAcpToolCallsToAssistantMessage(chat, session, false);
		SaveChatQuietly(app, chat);
		return;
	}
	if (type == uam::acp_claude_stream::kMessageTypeSystem && JsonDiagnosticStringValue(message, "subtype") == uam::acp_claude_stream::kSubtypeInit)
	{
		if (session.initialize_request_id == 0)
			session.initialized = true;
		const std::string session_id = uam::nlohmann_json::TrimmedStringValueOr(message, "session_id", "");
		if (!session_id.empty())
		{
			session.session_id = session_id;
			const std::string previous_native_session_id = chat.native_session_id;
			if (!session.goal_internal_session && SetChatNativeSessionIdIfChanged(chat, session_id))
			{
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

	if (type == uam::acp_claude_stream::kMessageTypeAssistant ||
	    type == uam::acp_claude_stream::kMessageTypeResult)
	{
		// Claude emits live answers, rather than ACP session/load history replay.
		session.assistant_replay_prefixes.clear();
		session.load_history_replay_updates.clear();
	}
	if (type == uam::acp_claude_stream::kMessageTypeAssistant)
	{
		if (!uam::AcpSessionHasPendingCancel(session))
			HandleClaudeAssistantMessage(app, session, chat, message, browser);
		return;
	}

	if (type == uam::acp_claude_stream::kMessageTypeUser)
	{
		if (!uam::AcpSessionHasPendingCancel(session))
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

} // namespace uam::acp_detail
