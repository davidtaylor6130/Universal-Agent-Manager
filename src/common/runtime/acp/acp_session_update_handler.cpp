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
#include "common/utils/diagnostic_log.h"

namespace uam::acp_detail
{

namespace
{
	/// <summary>Applies materialized load replay to saved history without adding live turn events.</summary>
	void ApplyReplayedCompaction(AppState& app, AcpSessionState& session, ChatSession& chat,
	                             const std::string& id, const nlohmann::json* summary)
	{
		const std::string identity = "acp-compaction:" + id;
		for (Message& message : chat.messages)
		{
			for (MessageBlock& block : message.blocks)
			{
				// Native OpenCode import stores the raw message ID; live ACP uses a namespaced ID.
				if (block.type != "context_compaction" || (block.request_id_json != identity && block.request_id_json != id)) continue;
				if (summary != nullptr)
				{
					block.text = ContentTextFromJson(*summary);
					ScheduleChatSave(app, chat, 0.0);
				}
				return;
			}
		}

		const int history_end = session.load_history_replay_end_message_index >= 0
		    ? session.load_history_replay_end_message_index
		    : (session.turn_first_user_message_index >= 0 ? session.turn_first_user_message_index : static_cast<int>(chat.messages.size()));
		const AcpReplayUpdateState* next = session.load_history_replay_updates.empty() ? nullptr : &session.load_history_replay_updates.front();
		int message_index = std::clamp(next != nullptr && next->message_index >= 0 ? next->message_index : history_end,
		                               0, static_cast<int>(chat.messages.size()));
		int block_index = next != nullptr ? next->block_index : -1;
		if (message_index >= history_end || message_index >= static_cast<int>(chat.messages.size()) ||
		    chat.messages[static_cast<std::size_t>(message_index)].role != MessageRole::Assistant)
		{
			if (message_index > 0 && chat.messages[static_cast<std::size_t>(message_index - 1)].role == MessageRole::Assistant)
			{
				--message_index;
				block_index = -1;
				next = nullptr;
			}
			else
			{
				Message marker;
				marker.role = MessageRole::Assistant;
				marker.provider = session.provider_id;
				marker.created_at = AcpTimestampNow();
				chat.messages.insert(chat.messages.begin() + message_index, std::move(marker));
				const auto shift = [message_index](int& index) { if (index >= message_index) ++index; };
				shift(session.current_assistant_message_index);
				shift(session.turn_assistant_message_index);
				shift(session.turn_first_user_message_index);
				shift(session.turn_user_message_index);
				shift(session.load_history_replay_end_message_index);
				shift(chat.remote_turn_user_message_index);
				for (auto& [request_id, steer] : session.pending_steer_requests) shift(steer.user_message_index);
				for (AcpRemotePendingRequestState& request : chat.remote_pending_requests) shift(request.user_message_index);
				for (auto& [tool_id, owner] : session.tool_call_message_indices) shift(owner);
				for (AcpReplayUpdateState& replay : session.load_history_replay_updates) shift(replay.message_index);
				block_index = 0;
				next = nullptr;
			}
		}
		Message& message = chat.messages[static_cast<std::size_t>(message_index)];
		if (message.blocks.empty())
		{
			AcpSessionState snapshot;
			RestoreTurnEventsFromMessageBlocks(snapshot, message);
			message.blocks = MessageBlocksFromTurnEvents(snapshot);
		}
		if (block_index < 0)
		{
			block_index = static_cast<int>(message.blocks.size());
			if (next != nullptr)
			{
				for (std::size_t index = 0; index < message.blocks.size(); ++index)
				{
					const MessageBlock& block = message.blocks[index];
					if ((next->session_update == "agent_message_chunk" && block.type == "assistant_text") ||
					    (next->session_update == "agent_thought_chunk" && block.type == "thought") ||
					    (block.type == "tool_call" && block.tool_call_id == next->tool_call_id))
					{
						block_index = static_cast<int>(index);
						break;
					}
				}
			}
		}
		block_index = std::clamp(block_index, 0, static_cast<int>(message.blocks.size()));
		message.blocks.insert(message.blocks.begin() + block_index,
		    {"context_compaction", summary == nullptr ? "" : ContentTextFromJson(*summary), "", identity});
		for (AcpReplayUpdateState& replay : session.load_history_replay_updates)
			if (replay.message_index == message_index && replay.block_index >= block_index) ++replay.block_index;
		ScheduleChatSave(app, chat, 0.0);
	}

	/// <summary>Normalizes ACP compaction into the same saved timeline blocks as Codex.</summary>
	bool HandleCompactionUpdate(AppState& app, AcpSessionState& session, ChatSession& chat,
	                            const nlohmann::json& update, const std::string& update_type)
	{
		const bool chunk = update_type == "compaction_summary_chunk";
		const bool standard = chunk || update_type == "compaction_update";
		const nlohmann::json metadata = JsonObjectValue(update, "_meta");
		const nlohmann::json* marker = uam::nlohmann_json::FindField(metadata, "opencode/compaction");
		if (!standard && (update_type != "session_info_update" || marker == nullptr || !marker->is_object())) return false;
		// Child compaction does not compact the parent conversation.
		if (metadata.contains("opencode/child-session")) return true;
		const nlohmann::json& details = standard ? update : *marker;
		const std::string id = JsonDiagnosticStringValue(details, standard ? "compactionId" : "messageId");
		if (id.empty()) return true;
		const std::string identity = "acp-compaction:" + id;
		const std::string status = JsonDiagnosticStringValue(details, "status");
		if (!chunk && status.empty()) return true;
		if (session.ignore_session_updates_until_ready)
		{
			if (!chunk && status == "completed")
				ApplyReplayedCompaction(app, session, chat, id, standard ? uam::nlohmann_json::FindField(update, "summary") : nullptr);
			return true;
		}
		if (!uam::AcpSessionHasActiveTurn(session)) return true;
		auto event = std::ranges::find_if(session.turn_events, [&](const AcpTurnEventState& candidate)
		{
			return (candidate.type == "context_compaction" || candidate.type == "context_compaction_pending") &&
			       candidate.request_id_json == identity;
		});
		if (chunk)
		{
			if (event != session.turn_events.end() && event->type == "context_compaction_pending")
				event->text += ContentTextFromJson(JsonObjectValue(update, "content"));
			return true;
		}
		if (status == "failed" || status == "cancelled")
		{
			if (status == "failed")
				uam::diagnostics::Write("[ACP] Compaction failed for " + session.provider_id + ": " +
				    JsonDiagnosticStringValue(details, "error"));
			if (event != session.turn_events.end() && event->type == "context_compaction_pending") session.turn_events.erase(event);
			return true;
		}
		if (event == session.turn_events.end())
		{
			// Reserve the start position without displaying a successful boundary prematurely.
			session.turn_events.push_back({.type = "context_compaction_pending", .request_id_json = identity});
			event = std::prev(session.turn_events.end());
		}
		if (standard && update.contains("summary")) event->text = ContentTextFromJson(update["summary"]);
		if (status == "completed")
		{
			event->type = "context_compaction";
			EnsureAssistantMessage(chat, session);
			SyncCurrentAssistantMessageBlocksFromTurnEvents(chat, session);
			ScheduleChatSave(app, chat, 0.0);
		}
		return true;
	}
}

void HandleSessionUpdate(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& params, CefRefPtr<CefBrowser> browser)
{
	if (uam::AcpSessionHasPendingCancel(session))
	{
		return;
	}
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
	if (update_type == uam::acp_stream_types::kSessionUpdateConfigOptions)
	{
		if (const nlohmann::json* config_options = uam::nlohmann_json::FindArrayField(update, "configOptions"))
		{
			(void)UpdateAcpConfigOptions(session, *config_options);
			(void)ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpConfigOptionsUpdated(session, *config_options);
			if (session.config_option_change_request_id != 0)
			{
				const auto confirmed = std::ranges::find_if(session.available_config_options, [&](const AcpConfigOptionState& option) {
					return option.id == session.config_option_change_id && option.current_value == session.config_option_change_requested_value;
				});
				if (confirmed != session.available_config_options.end()) ClearAcpConfigOptionChangeRequest(session);
			}
			session.awaiting_model_config_options = false;
			(void)ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpReconcileModelOptions(app, session, chat);
			(void)SendQueuedPromptIfReady(app, session, chat);
		}
		return;
	}
	if (update_type == uam::acp_stream_types::kSessionUpdateAvailableCommands)
	{
		session.available_commands.clear();
		for (const nlohmann::json& command : JsonArrayValue(update, "availableCommands"))
		{
			AcpCommandState parsed;
			parsed.name = uam::nlohmann_json::TrimmedStringValue(command, {"name"});
			if (parsed.name.empty())
			{
				continue;
			}
			parsed.description = uam::nlohmann_json::TrimmedStringValue(command, {"description"});
			parsed.input_hint = uam::nlohmann_json::TrimmedStringValue(JsonObjectValue(command, "input"), {"hint"});
			session.available_commands.push_back(std::move(parsed));
		}
		return;
	}
	if (HandleCompactionUpdate(app, session, chat, update, update_type)) return;
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
			uam::PushStreamToken(browser, chat.id, session.current_assistant_message_index, appended);
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
			const std::string tool_content = ToolCallContentTextFromJson(update);
			if (!tool_content.empty())
			{
				tool_call.content = tool_content;
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
