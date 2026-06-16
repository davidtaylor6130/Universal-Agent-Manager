#include "common/runtime/acp/acp_session_update_handler.h"
#include "common/runtime/acp/acp_session_internal.h"

#include "cef/cef_push.h"
#include "app/provider_resolution_service.h"
#include "common/config/approval_modes.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/acp/acp_statuses.h"
#include "common/runtime/acp/acp_stream_types.h"
#include "common/runtime/acp/acp_tool_kinds.h"
#include "common/utils/nlohmann_json_utils.h"

namespace uam::acp_detail
{

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

} // namespace uam::acp_detail
