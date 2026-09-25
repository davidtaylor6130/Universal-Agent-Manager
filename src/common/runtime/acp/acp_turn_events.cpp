#include "common/runtime/acp/acp_session_internal.h"

#include "common/runtime/acp/acp_stream_types.h"

#include <algorithm>
#include <vector>

namespace uam::acp_detail
{

bool MessageBlocksEqual(const std::vector<MessageBlock>& lhs, const std::vector<MessageBlock>& rhs)
{
	if (lhs.size() != rhs.size())
	{
		return false;
	}
	for (std::size_t i = 0; i < lhs.size(); ++i)
	{
		if (lhs[i].type != rhs[i].type || lhs[i].text != rhs[i].text || lhs[i].tool_call_id != rhs[i].tool_call_id || lhs[i].request_id_json != rhs[i].request_id_json)
		{
			return false;
		}
	}
	return true;
}

bool PersistableTurnEvent(const AcpTurnEventState& event)
{
	if (uam::acp_stream_types::IsTextTurnEventType(event.type))
	{
		return !event.text.empty();
	}
	if (event.type == uam::acp_stream_types::kTurnEventToolCall)
	{
		return !event.tool_call_id.empty();
	}
	if (event.type == uam::acp_stream_types::kTurnEventPermissionRequest)
	{
		return !event.request_id_json.empty();
	}
	if (event.type == uam::acp_stream_types::kTurnEventUserInputRequest)
	{
		return !event.request_id_json.empty();
	}
	return event.type == uam::acp_stream_types::kTurnEventPlan;
}

bool CanMergeTurnEventWithLastBlock(const AcpTurnEventState& event, const std::vector<MessageBlock>& blocks)
{
	return uam::acp_stream_types::IsTextTurnEventType(event.type) && !blocks.empty() && blocks.back().type == event.type;
}

bool HasMatchingTurnBlock(const std::vector<MessageBlock>& blocks, const AcpTurnEventState& event)
{
	for (const MessageBlock& block : blocks)
	{
		if (event.type == uam::acp_stream_types::kTurnEventToolCall && block.type == uam::acp_stream_types::kTurnEventToolCall && block.tool_call_id == event.tool_call_id)
		{
			return true;
		}
		if (event.type == uam::acp_stream_types::kTurnEventPermissionRequest && block.type == uam::acp_stream_types::kTurnEventPermissionRequest && block.request_id_json == event.request_id_json)
		{
			return true;
		}
		if (event.type == uam::acp_stream_types::kTurnEventUserInputRequest && block.type == uam::acp_stream_types::kTurnEventUserInputRequest && block.request_id_json == event.request_id_json)
		{
			return true;
		}
	}
	return false;
}

std::vector<MessageBlock> MessageBlocksFromTurnEvents(const AcpSessionState& session)
{
	std::vector<MessageBlock> blocks;
	blocks.reserve(session.turn_events.size());
	for (const AcpTurnEventState& event : session.turn_events)
	{
		if (!PersistableTurnEvent(event))
		{
			continue;
		}

		if (CanMergeTurnEventWithLastBlock(event, blocks))
		{
			blocks.back().text += event.text;
			continue;
		}

		if (HasMatchingTurnBlock(blocks, event))
		{
			continue;
		}

		MessageBlock block;
		block.type = event.type;
		block.text = event.text;
		block.tool_call_id = event.tool_call_id;
		block.request_id_json = event.request_id_json;
		blocks.push_back(std::move(block));
	}
	return blocks;
}

namespace
{
	void RestoreToolCallsFromMessage(AcpSessionState& session, const Message& message, int owner)
	{
		for (const ToolCall& tool_call : message.tool_calls)
		{
			if (!tool_call.id.empty() && owner >= 0)
			{
				const std::pair<std::unordered_map<std::string, int>::iterator, bool> inserted = session.tool_call_message_indices.emplace(tool_call.id, owner);
				if (!inserted.second)
				{
					inserted.first->second = -1; // Duplicate IDs have no unambiguous transcript owner.
					continue;
				}
			}
			session.tool_calls.push_back({
			    .id = tool_call.id,
			    .title = tool_call.name,
			    .kind = tool_call.name,
			    .status = tool_call.status,
			    .content = tool_call.result_text,
			    .args_json = tool_call.args_json,
			    .is_sub_agent = tool_call.is_sub_agent,
			    .sub_agent_id = tool_call.sub_agent_id,
			    .sub_agent_title = tool_call.sub_agent_title,
			});
		}
	}
}

void RestoreTurnEventsFromMessageBlocks(AcpSessionState& session, const Message& message)
{
	session.tool_calls.clear();
	session.tool_call_message_indices.clear();
	session.tool_calls.reserve(message.tool_calls.size());
	RestoreToolCallsFromMessage(session, message, session.current_assistant_message_index);
	session.turn_events.clear();
	session.turn_events.reserve(message.blocks.empty()
	                                ? message.tool_calls.size() + 2
	                                : message.blocks.size());
	if (message.blocks.empty())
	{
		if (!message.thoughts.empty())
		{
			session.turn_events.push_back(
			    {.type = uam::acp_stream_types::kTurnEventThought,
			     .text = message.thoughts});
		}
		for (const ToolCall& tool_call : message.tool_calls)
		{
			session.turn_events.push_back(
			    {.type = uam::acp_stream_types::kTurnEventToolCall,
			     .tool_call_id = tool_call.id});
		}
		if (!message.content.empty())
		{
			session.turn_events.push_back(
			    {.type = uam::acp_stream_types::kTurnEventAssistantText,
			     .text = message.content});
		}
		return;
	}
	for (const MessageBlock& block : message.blocks)
	{
		AcpTurnEventState event;
		event.type = block.type;
		event.text = block.text;
		event.tool_call_id = block.tool_call_id;
		event.request_id_json = block.request_id_json;
		session.turn_events.push_back(std::move(event));
	}
}

void RestoreRemoteAcpTranscript(AcpSessionState& session, const ChatSession& chat)
{
	int last_user = -1;
	int last_assistant = -1;
	int start = chat.remote_turn_user_message_index;
	const bool valid_anchor = start >= 0 && start < static_cast<int>(chat.messages.size()) &&
	    chat.messages[static_cast<std::size_t>(start)].role == MessageRole::User;
	if (!valid_anchor) start = -1;
	for (int index = std::max(0, start); index < static_cast<int>(chat.messages.size()); ++index)
	{
		const Message& message = chat.messages[static_cast<std::size_t>(index)];
		if (message.role == MessageRole::User)
		{
			last_user = index;
			if (index > start && !message.priority_steer) start = index;
		}
	}
	// Legacy saves have no proof that earlier steers belong to this active turn.
	if (!valid_anchor) start = last_user;
	if (start < 0) start = last_user;
	session.turn_user_message_index = start;
	session.current_assistant_message_index = -1;
	session.turn_assistant_message_index = -1;
	session.tool_calls.clear();
	session.tool_call_message_indices.clear();
	session.turn_events.clear();
	if (start < 0) return;
	for (int index = start + 1; index < static_cast<int>(chat.messages.size()); ++index)
		if (chat.messages[static_cast<std::size_t>(index)].role == MessageRole::Assistant) last_assistant = index;
	session.turn_assistant_message_index = last_assistant;
	if (last_assistant > last_user)
	{
		session.current_assistant_message_index = last_assistant;
		RestoreTurnEventsFromMessageBlocks(session, chat.messages[static_cast<std::size_t>(last_assistant)]);
	}
	for (int index = start + 1; index < static_cast<int>(chat.messages.size()); ++index)
	{
		const Message& message = chat.messages[static_cast<std::size_t>(index)];
		if (message.role == MessageRole::Assistant && index != session.current_assistant_message_index)
			RestoreToolCallsFromMessage(session, message, index);
	}
}

bool SyncMessageBlocksFromTurnEvents(Message& message, const AcpSessionState& session)
{
	if (session.turn_events.empty())
	{
		return false;
	}

	std::vector<MessageBlock> blocks = MessageBlocksFromTurnEvents(session);
	if (blocks.empty() || MessageBlocksEqual(message.blocks, blocks))
	{
		return false;
	}

	message.blocks = std::move(blocks);
	return true;
}

Message* CurrentAssistantMessage(ChatSession& chat, const AcpSessionState& session)
{
	const int index = session.current_assistant_message_index;
	if (index < 0 || index >= static_cast<int>(chat.messages.size()))
	{
		return nullptr;
	}

	Message& message = chat.messages[static_cast<std::size_t>(index)];
	return message.role == MessageRole::Assistant ? &message : nullptr;
}

const Message* CurrentAssistantMessage(const ChatSession& chat, const AcpSessionState& session)
{
	const int index = session.current_assistant_message_index;
	if (index < 0 || index >= static_cast<int>(chat.messages.size()))
	{
		return nullptr;
	}

	const Message& message = chat.messages[static_cast<std::size_t>(index)];
	return message.role == MessageRole::Assistant ? &message : nullptr;
}

bool SyncCurrentAssistantMessageBlocksFromTurnEvents(ChatSession& chat, AcpSessionState& session)
{
	Message* message = CurrentAssistantMessage(chat, session);
	if (message == nullptr)
	{
		return false;
	}

	if (SyncMessageBlocksFromTurnEvents(*message, session))
	{
		chat.updated_at = AcpTimestampNow();
		return true;
	}
	return false;
}

void AppendAssistantTextTurnEvent(AcpSessionState& session, const std::string& chunk)
{
	if (!session.turn_events.empty() && session.turn_events.back().type == uam::acp_stream_types::kTurnEventAssistantText)
	{
		session.turn_events.back().text += chunk;
		return;
	}

	AcpTurnEventState event;
	event.type = uam::acp_stream_types::kTurnEventAssistantText;
	event.text = chunk;
	session.turn_events.push_back(std::move(event));
}

bool AppendThoughtTurnEvent(AcpSessionState& session, const std::string& chunk)
{
	if (chunk.empty())
	{
		return false;
	}

	if (!session.turn_events.empty() && session.turn_events.back().type == uam::acp_stream_types::kTurnEventThought)
	{
		session.turn_events.back().text += chunk;
		return false;
	}

	AcpTurnEventState event;
	event.type = uam::acp_stream_types::kTurnEventThought;
	event.text = chunk;
	session.turn_events.push_back(std::move(event));
	return true;
}

bool HasTurnToolEvent(const AcpSessionState& session, const std::string& tool_call_id)
{
	return std::ranges::any_of(session.turn_events, [&](const AcpTurnEventState& event) { return event.type == uam::acp_stream_types::kTurnEventToolCall && event.tool_call_id == tool_call_id; });
}

void AppendToolTurnEventIfNeeded(AcpSessionState& session, const std::string& tool_call_id)
{
	if (tool_call_id.empty() || HasTurnToolEvent(session, tool_call_id))
	{
		return;
	}
	const std::unordered_map<std::string, int>::const_iterator owner = session.tool_call_message_indices.find(tool_call_id);
	if (owner != session.tool_call_message_indices.end() &&
	    owner->second != session.current_assistant_message_index) return;

	AcpTurnEventState event;
	event.type = uam::acp_stream_types::kTurnEventToolCall;
	event.tool_call_id = tool_call_id;
	session.turn_events.push_back(std::move(event));
}

void AppendPermissionTurnEventIfNeeded(AcpSessionState& session, const std::string& request_id_json, const std::string& tool_call_id)
{
	if (request_id_json.empty())
	{
		return;
	}

	const bool exists = std::ranges::any_of(session.turn_events, [&](const AcpTurnEventState& event) { return event.type == uam::acp_stream_types::kTurnEventPermissionRequest && event.request_id_json == request_id_json; });
	if (exists)
	{
		return;
	}

	if (!tool_call_id.empty())
	{
		AppendToolTurnEventIfNeeded(session, tool_call_id);
	}

	AcpTurnEventState event;
	event.type = uam::acp_stream_types::kTurnEventPermissionRequest;
	event.request_id_json = request_id_json;
	event.tool_call_id = tool_call_id;
	session.turn_events.push_back(std::move(event));
}

void AppendUserInputTurnEventIfNeeded(AcpSessionState& session, const std::string& request_id_json, const std::string& item_id)
{
	if (request_id_json.empty())
	{
		return;
	}

	const bool exists = std::ranges::any_of(session.turn_events, [&](const AcpTurnEventState& event) { return event.type == uam::acp_stream_types::kTurnEventUserInputRequest && event.request_id_json == request_id_json; });
	if (exists)
	{
		return;
	}

	AcpTurnEventState event;
	event.type = uam::acp_stream_types::kTurnEventUserInputRequest;
	event.request_id_json = request_id_json;
	event.tool_call_id = item_id;
	session.turn_events.push_back(std::move(event));
}

void AppendPlanTurnEventIfNeeded(AcpSessionState& session)
{
	const bool exists = std::ranges::any_of(session.turn_events, [](const AcpTurnEventState& event) { return event.type == uam::acp_stream_types::kTurnEventPlan; });
	if (exists)
	{
		return;
	}

	AcpTurnEventState event;
	event.type = uam::acp_stream_types::kTurnEventPlan;
	session.turn_events.push_back(std::move(event));
}

} // namespace uam::acp_detail
