#include "common/runtime/acp/acp_session_internal.h"

#include "common/runtime/acp/acp_stream_types.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/parse_utils.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <optional>
#include <string>

namespace uam::acp_detail
{

void RememberAssistantReplayPrefixes(AcpSessionState& session, const ChatSession& chat, int turn_user_message_index)
{
	session.assistant_replay_prefixes.clear();
	const int exclusive_end = std::min(turn_user_message_index, static_cast<int>(chat.messages.size()));
	for (int i = 0; i < exclusive_end; ++i)
	{
		const Message& message = chat.messages[static_cast<std::size_t>(i)];
		if (message.role != MessageRole::Assistant)
		{
			continue;
		}

		const std::string trimmed = uam::strings::Trim(message.content);
		if (trimmed.empty())
		{
			continue;
		}

		session.assistant_replay_prefixes.push_back(message.content);
		if (trimmed != message.content)
		{
			session.assistant_replay_prefixes.push_back(trimmed);
		}
	}

	std::ranges::sort(session.assistant_replay_prefixes, [](const std::string& lhs, const std::string& rhs) { return lhs.size() > rhs.size(); });
	const auto duplicate_prefixes = std::ranges::unique(session.assistant_replay_prefixes);
	session.assistant_replay_prefixes.erase(duplicate_prefixes.begin(), duplicate_prefixes.end());
}

void RememberLoadHistoryReplayUpdates(AcpSessionState& session, const ChatSession& chat, int turn_user_message_index)
{
	session.load_history_replay_updates.clear();
	const int exclusive_end = std::min(turn_user_message_index, static_cast<int>(chat.messages.size()));
	for (int i = 0; i < exclusive_end; ++i)
	{
		const Message& message = chat.messages[static_cast<std::size_t>(i)];
		if (message.role == MessageRole::User)
		{
			if (!uam::strings::IsBlank(message.content))
			{
				AcpReplayUpdateState replay;
				replay.session_update = uam::acp_stream_types::kSessionUpdateUserMessageChunk;
				replay.text = message.content;
				session.load_history_replay_updates.push_back(std::move(replay));
			}
			continue;
		}

		if (message.role != MessageRole::Assistant)
		{
			continue;
		}

		if (!message.blocks.empty())
		{
			for (const MessageBlock& block : message.blocks)
			{
				AcpReplayUpdateState replay;
				if (block.type == uam::acp_stream_types::kTurnEventThought)
				{
					replay.session_update = uam::acp_stream_types::kSessionUpdateAgentThoughtChunk;
					replay.text = block.text;
				}
				else if (block.type == uam::acp_stream_types::kTurnEventAssistantText)
				{
					replay.session_update = uam::acp_stream_types::kSessionUpdateAgentMessageChunk;
					replay.text = block.text;
				}
				else if (block.type == uam::acp_stream_types::kTurnEventToolCall)
				{
					const auto tool = std::ranges::find_if(message.tool_calls, [&](const ToolCall& candidate) { return candidate.id == block.tool_call_id; });
					if (tool == message.tool_calls.end()) continue;
					replay.session_update = uam::acp_stream_types::kSessionUpdateToolCall;
					replay.tool_call_id = tool->id;
					replay.title = tool->name;
				}
				else continue;
				if (!replay.text.empty() || !replay.tool_call_id.empty())
					session.load_history_replay_updates.push_back(std::move(replay));
			}
			continue;
		}

		if (!uam::strings::IsBlank(message.thoughts))
		{
			AcpReplayUpdateState replay;
			replay.session_update = uam::acp_stream_types::kSessionUpdateAgentThoughtChunk;
			replay.text = message.thoughts;
			session.load_history_replay_updates.push_back(std::move(replay));
		}

		if (!uam::strings::IsBlank(message.content))
		{
			AcpReplayUpdateState replay;
			replay.session_update = uam::acp_stream_types::kSessionUpdateAgentMessageChunk;
			replay.text = message.content;
			session.load_history_replay_updates.push_back(std::move(replay));
		}

		for (const ToolCall& tool_call : message.tool_calls)
		{
			AcpReplayUpdateState replay;
			replay.session_update = uam::acp_stream_types::kSessionUpdateToolCall;
			replay.tool_call_id = tool_call.id;
			replay.title = tool_call.name;
			session.load_history_replay_updates.push_back(std::move(replay));
		}
	}
}

std::string StripKnownAssistantReplayPrefix(const AcpSessionState& session, const std::string& text)
{
	for (const std::string& prefix : session.assistant_replay_prefixes)
	{
		if (text == prefix)
		{
			return "";
		}

		if (uam::strings::StartsWith(text, prefix))
		{
			const std::string suffix = text.substr(prefix.size());
			if (prefix.size() >= kMinAssistantReplayPrefixBytes || StartsWithLineBreak(suffix))
			{
				return StripLeadingLineBreaks(suffix);
			}
		}
	}

	return text;
}

std::string AssistantDeltaForIncomingText(const AcpSessionState& session, const std::string& current_assistant_text, const std::string& incoming_text)
{
	std::string candidate = StripKnownAssistantReplayPrefix(session, incoming_text);
	if (candidate.empty())
	{
		return "";
	}

	if (!current_assistant_text.empty())
	{
		if (candidate == current_assistant_text)
		{
			return candidate;
		}

		if (uam::strings::StartsWith(candidate, current_assistant_text))
		{
			return candidate.substr(current_assistant_text.size());
		}

	}

	return candidate;
}

bool ReplayUpdateTypesCompatible(const std::string& expected, const std::string& incoming)
{
	return uam::acp_stream_types::SessionUpdateTypesCompatible(expected, incoming);
}

bool ReplayToolUpdateMatches(const AcpReplayUpdateState& expected, const nlohmann::json& update, const std::string& update_type)
{
	if (!ReplayUpdateTypesCompatible(expected.session_update, update_type))
	{
		return false;
	}

	const std::string incoming_id = JsonDiagnosticStringValue(update, "toolCallId");
	if (!expected.tool_call_id.empty() && !incoming_id.empty())
	{
		return expected.tool_call_id == incoming_id;
	}

	const std::string incoming_title = JsonDiagnosticStringValue(update, "title");
	return !expected.title.empty() && !incoming_title.empty() && expected.title == incoming_title;
}

bool ReplayTextUpdateMatches(const AcpReplayUpdateState& expected, const std::string& update_type, const std::string& incoming_text, std::string& live_suffix)
{
	live_suffix.clear();
	if (!ReplayUpdateTypesCompatible(expected.session_update, update_type) || incoming_text.empty())
	{
		return false;
	}

	if (expected.text == incoming_text)
	{
		return true;
	}

	if (uam::strings::StartsWith(expected.text, incoming_text))
	{
		return true;
	}

	if (uam::strings::StartsWith(incoming_text, expected.text))
	{
		const std::string suffix = incoming_text.substr(expected.text.size());
		if (StartsWithLineBreak(suffix))
		{
			live_suffix = StripLeadingLineBreaks(suffix);
			return true;
		}
	}

	return false;
}

namespace
{

bool TryConsumeLongAssistantReplayPrefix(const AcpSessionState& session, const std::string& update_type, const std::string& incoming_text, std::string& live_text)
{
	if (update_type != uam::acp_stream_types::kSessionUpdateAgentMessageChunk || incoming_text.empty())
	{
		return false;
	}

	// Some providers begin replay with a cumulative assistant snapshot. Only a
	// distinctive prior prefix can switch from ordered replay to that phase;
	// short identical replies must remain eligible as new live answers.
	for (const std::string& prefix : session.assistant_replay_prefixes)
	{
		if (prefix.size() < kMinAssistantReplayPrefixBytes || !uam::strings::StartsWith(incoming_text, prefix))
		{
			continue;
		}

		live_text = StripLeadingLineBreaks(incoming_text.substr(prefix.size()));
		return true;
	}

	return false;
}

} // anonymous namespace

bool TryConsumeLoadHistoryReplayUpdate(AcpSessionState& session, const nlohmann::json& update, const std::string& update_type, const std::string& incoming_text, std::string& live_text)
{
	live_text = incoming_text;
	if (session.load_history_replay_updates.empty() || !update.is_object())
	{
		return false;
	}

	AcpReplayUpdateState& expected = session.load_history_replay_updates.front();
	if (!ReplayUpdateTypesCompatible(expected.session_update, update_type))
	{
		if (TryConsumeLongAssistantReplayPrefix(session, update_type, incoming_text, live_text))
		{
			session.load_history_replay_updates.clear();
			return true;
		}
		session.load_history_replay_updates.clear();
		session.assistant_replay_prefixes.clear();
		return false;
	}

	if (uam::acp_stream_types::IsToolSessionUpdateType(update_type))
	{
		if (!ReplayToolUpdateMatches(expected, update, update_type))
		{
			session.load_history_replay_updates.clear();
			session.assistant_replay_prefixes.clear();
			return false;
		}
		session.load_history_replay_updates.erase(session.load_history_replay_updates.begin());
		live_text.clear();
		return true;
	}

	std::string suffix;
	if (!ReplayTextUpdateMatches(expected, update_type, incoming_text, suffix))
	{
		if (TryConsumeLongAssistantReplayPrefix(session, update_type, incoming_text, live_text))
		{
			session.load_history_replay_updates.clear();
			return true;
		}
		session.load_history_replay_updates.clear();
		session.assistant_replay_prefixes.clear();
		return false;
	}

	if (uam::strings::StartsWith(expected.text, incoming_text) && expected.text != incoming_text)
	{
		expected.text = StripLeadingLineBreaks(expected.text.substr(incoming_text.size()));
		if (expected.text.empty())
		{
			session.load_history_replay_updates.erase(session.load_history_replay_updates.begin());
		}
		live_text.clear();
		return true;
	}

	session.load_history_replay_updates.erase(session.load_history_replay_updates.begin());
	live_text = suffix;
	return true;
}

std::string JsonRpcIdToStableString(const nlohmann::json& id)
{
	if (id.is_null())
	{
		return "";
	}
	return id.dump();
}

nlohmann::json StableStringToJsonRpcId(const std::string& request_id_json)
{
	try
	{
		return nlohmann::json::parse(request_id_json);
	}
	catch (const nlohmann::json::exception&)
	{
		return request_id_json;
	}
}

int JsonRpcNumericId(const nlohmann::json& id)
{
	if (const std::optional<int> parsed = uam::nlohmann_json::IntValueStrict(id))
	{
		return *parsed;
	}
	if (id.is_string())
	{
		return uam::parse::IntOr(id.get_ref<const std::string&>(), 0);
	}
	return 0;
}

void AppendRecentStderr(AcpSessionState& session, const std::string& chunk)
{
	session.recent_stderr += chunk;
	if (session.recent_stderr.size() > kMaxRecentStderrBytes)
	{
		session.recent_stderr.erase(0, session.recent_stderr.size() - kMaxRecentStderrBytes);
	}
}

} // namespace uam::acp_detail
