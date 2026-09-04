#include "common/runtime/acp/acp_session_internal.h"

#include "common/runtime/acp/acp_statuses.h"
#include "common/utils/string_utils.h"

#include <cstddef>
#include <string>
#include <vector>

namespace uam::acp_detail
{

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
	Message& message = EnsureAssistantMessage(chat, session);
	AppendThoughtText(message.thoughts, chunk, starts_new_block);
	(void)SyncMessageBlocksFromTurnEvents(message, session);
	chat.updated_at = AcpTimestampNow();
	return true;
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
	persisted.args_json = tool_call.args_json;
	persisted.result_text = tool_call.content;
	if (!tool_call.permission_review_reason.empty())
	{
		if (!persisted.result_text.empty()) persisted.result_text += "\n\n";
		persisted.result_text += "AI Review (" + tool_call.permission_review_decision + "): " + tool_call.permission_review_reason;
	}
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

namespace
{
	bool FinalizeActiveAcpToolCalls(ChatSession& chat, AcpSessionState& session, const char* status)
	{
		bool changed = false;
		for (AcpToolCallState& tool_call : session.tool_calls)
		{
			if (uam::acp_statuses::IsActiveStatus(tool_call.status))
			{
				tool_call.status = status;
				changed = true;
			}
		}
		return SyncAcpToolCallsToAssistantMessage(chat, session, true) || changed;
	}
}

bool FinalizeActiveAcpToolCallsAsCancelled(ChatSession& chat, AcpSessionState& session)
{
	return FinalizeActiveAcpToolCalls(chat, session, uam::acp_statuses::kCancelled);
}

bool FinalizeActiveAcpToolCallsAsFailed(ChatSession& chat, AcpSessionState& session)
{
	return FinalizeActiveAcpToolCalls(chat, session, uam::acp_statuses::kFailed);
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

} // namespace uam::acp_detail
