#include "common/runtime/acp/acp_session_internal.h"

#include "common/runtime/acp/acp_statuses.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <string>
#include <vector>

namespace uam::acp_detail
{

bool ConfirmTurnInputReceived(const AcpSessionState& session, ChatSession& chat)
{
	// Called only where a provider has proved it completed this turn's own prompt. Even then nothing is confirmed
	// while a helper receipt is still owed (the receipt is the only proof for that delivery), for a cancelled turn,
	// or for a prompt that has not left the queue. A stale marker is acceptable; a falsely confirmed loss is not.
	if (!session.processing || session.cancel_requested || uam::AcpSessionHasPendingCancel(session) || !session.queued_prompt.empty() ||
	    !chat.remote_prompt_delivery_id.empty()) return false;
	// A goal turn carries no authored row of its own, and without a recorded first row the turn's rows are not known.
	// A restored internal turn saves no anchor, and the restore then falls back to the last user row: its completion
	// must not confirm that unrelated row.
	if (session.goal_internal_session || !session.goal_turn_kind.empty() || session.turn_first_user_message_index < 0) return false;
	bool changed = false;
	const int last = session.turn_user_message_index;
	// Read through const access: touching the list any other way restamps it even when no row changes.
	const auto& rows = std::as_const(chat).messages;
	for (int index = session.turn_first_user_message_index;
	     index <= last && index < static_cast<int>(rows.size()); ++index)
	{
		const Message& row = rows[static_cast<std::size_t>(index)];
		// A row held back as unsent or removed was not part of what the provider answered.
		if (row.role != MessageRole::User || row.input_write_completed || row.acp_prompt_not_sent || row.acp_prompt_discarded ||
		    !row.native_input_id || !input_provenance::ValidNativeInputId(*row.native_input_id)) continue;
		// Same rule as the helper receipt: an ID shared by two rows identifies neither.
		if (std::ranges::count_if(rows, [&row](const Message& other) { return other.role == MessageRole::User && other.native_input_id == row.native_input_id; }) != 1) continue;
		chat.messages[static_cast<std::size_t>(index)].input_write_completed = true;
		changed = true;
	}
	return changed;
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
	message.model_id = session.current_model_id;
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

std::string AppendAssistantChunk(ChatSession& chat, AcpSessionState& session, const std::string& chunk, const std::string& identity)
{
	if (chunk.empty())
	{
		return "";
	}

	std::string current_assistant_text;
	if (identity.empty())
	{
		if (const Message* current_message = CurrentAssistantMessage(chat, session))
			current_assistant_text = current_message->content;
	}

	// Identified native chunks are incremental; explicit load replay was consumed by the caller.
	const std::string delta = identity.empty()
	    ? AssistantDeltaForIncomingText(session, current_assistant_text, chunk) : chunk;
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
	AppendAssistantTextTurnEvent(session, delta, identity);
	(void)SyncCurrentAssistantMessageBlocksFromTurnEvents(chat, session);
	// Quarantined summary chunks are saved above, but must not enter the plain-text fast stream.
	return !session.turn_events.empty() && session.turn_events.back().type == "context_compaction_summary" ? std::string{} : delta;
}

ToolCall PersistedToolCallFromAcpToolCall(const AcpToolCallState& tool_call)
{
	ToolCall persisted;
	persisted.id = tool_call.id;
	persisted.name = uam::strings::NonEmptyOrFallback(tool_call.title, uam::strings::NonEmptyOrFallback(tool_call.kind, tool_call.id));
	persisted.kind = tool_call.kind;
	persisted.approval_status = tool_call.approval_status;
	persisted.task_id = tool_call.task_id;
	persisted.child_activity = tool_call.child_activity;
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
	persisted.sub_agent_children = tool_call.sub_agent_children;
	persisted.sub_agent_children_authoritative = tool_call.sub_agent_children_authoritative;
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

		if (existing == persisted)
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

	bool changed = false;
	const double now = GetAppTimeSeconds();
	for (std::size_t index = 0; index < session.tool_calls.size(); ++index)
	{
		AcpToolCallState& tool_call = session.tool_calls[index];
		if (tool_call.id.empty()) continue;
		// Every provider's tool calls reach this sink. Feed each finished call to the stream watch once, after a later
		// call has started, so a result that arrives in a later update is the one hashed.
		const bool finished = !uam::strings::IsBlank(tool_call.status) && !uam::acp_statuses::IsActiveStatus(tool_call.status);
		// The start also bounds an open call for the turn inactivity decision, so a call restored on reattach is stamped too.
		if (tool_call.stream_watch_started_s == 0.0) tool_call.stream_watch_started_s = finished ? -1.0 : now;
		if (!tool_call.stream_watch_observed)
		{
			if (finished && tool_call.stream_watch_finished_s == 0.0) tool_call.stream_watch_finished_s = now;
			if (finished && index + 1 < session.tool_calls.size())
			{
				tool_call.stream_watch_observed = true;
				const double duration = tool_call.stream_watch_started_s < 0.0 ? -1.0 : tool_call.stream_watch_finished_s - tool_call.stream_watch_started_s;
				session.stream_watch.ObserveTool(uam::strings::NonEmptyOrFallback(tool_call.title, tool_call.kind), tool_call.args_json, tool_call.content,
				                                 duration, tool_call.stream_watch_finished_s);
			}
		}
		Message* message = nullptr;
		const std::unordered_map<std::string, int>::const_iterator owner = session.tool_call_message_indices.find(tool_call.id);
		if (owner != session.tool_call_message_indices.end())
		{
			const int index = owner->second;
			if (index < 0 || index >= static_cast<int>(chat.messages.size()) ||
			    chat.messages[static_cast<std::size_t>(index)].role != MessageRole::Assistant) continue;
			message = &chat.messages[static_cast<std::size_t>(index)];
		}
		else
		{
			message = CurrentAssistantMessage(chat, session);
			if (message == nullptr && create_if_missing) message = &EnsureAssistantMessage(chat, session);
			if (message == nullptr) continue;
			session.tool_call_message_indices.emplace(tool_call.id, session.current_assistant_message_index);
		}
		changed |= UpsertPersistedToolCall(message->tool_calls, tool_call);
	}
	// Earlier segments retain their own blocks when a late tool result arrives after a steer.
	if (Message* message = CurrentAssistantMessage(chat, session))
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
		// Completed tools retained after a turn must not create another assistant on stop.
		return SyncAcpToolCallsToAssistantMessage(chat, session, changed) || changed;
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
