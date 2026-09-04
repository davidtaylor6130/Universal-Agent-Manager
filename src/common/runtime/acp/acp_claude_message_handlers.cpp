#include "common/runtime/acp/acp_claude_message_handlers.h"
#include "common/runtime/acp/acp_goal_loop.h"
#include "common/runtime/acp/acp_session_internal.h"

#include "cef/cef_push.h"
#include "common/config/approval_modes.h"
#include "common/runtime/acp/acp_claude_stream.h"
#include "common/runtime/acp/acp_content.h"
#include "common/runtime/acp/acp_statuses.h"
#include "common/utils/string_utils.h"

#include <string>

namespace uam::acp_detail
{

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
	(void)app;
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

	if (session.turn_assistant_message_index < 0)
	{
		const std::string result_text = uam::nlohmann_json::TrimmedStringValueOr(message, "result", "");
		if (!result_text.empty())
		{
			AppendAssistantChunk(chat, session, result_text);
		}
	}

	const bool is_error = JsonBooleanValueOr(message, "is_error", false);
	const std::string subtype = JsonDiagnosticStringValue(message, "subtype");
	if (is_error || uam::acp_claude_stream::IsResultErrorSubtype(subtype))
	{
		const std::string result_text = uam::nlohmann_json::TrimmedStringValueOr(message, "result", "");
		(void)FinalizeActiveAcpToolCallsAsFailed(chat, session);
		FailAcpTurnOrSession(session, &chat,
		                     uam::strings::NonEmptyOrFallback(result_text, "Claude stream-json turn failed."));
	}
	else
	{
		(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
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

} // namespace uam::acp_detail
