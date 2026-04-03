#include "native_session_link_service.h"

#include "app/application_core_helpers.h"

#include <unordered_set>

namespace
{
	bool MessagesEquivalentForNativeLinking(const Message& local_message, const Message& native_message)
	{
		return local_message.role == native_message.role && Trim(local_message.content) == Trim(native_message.content);
	}

	bool IsMessagePrefixForNativeLinking(const std::vector<Message>& local_messages, const std::vector<Message>& native_messages)
	{
		if (local_messages.empty() || local_messages.size() > native_messages.size())
		{
			return false;
		}

		for (std::size_t i = 0; i < local_messages.size(); ++i)
		{
			if (!MessagesEquivalentForNativeLinking(local_messages[i], native_messages[i]))
			{
				return false;
			}
		}

		return true;
	}

	std::optional<std::string> SingleSessionIdFromSet(const std::unordered_set<std::string>& session_ids)
	{
		if (session_ids.size() != 1)
		{
			return std::nullopt;
		}

		return *session_ids.begin();
	}

} // namespace

bool NativeSessionLinkService::IsLocalDraftChatId(const std::string& chat_id) const
{
	return chat_id.rfind("chat-", 0) == 0;
}

std::optional<std::string> NativeSessionLinkService::InferNativeSessionIdForLocalDraft(const ChatSession& local_chat,
	                                                                                      const std::vector<ChatSession>& native_chats) const
{
	if (!IsLocalDraftChatId(local_chat.id) || local_chat.messages.size() < 2)
	{
		return std::nullopt;
	}

	std::unordered_set<std::string> exact_match_ids;
	std::unordered_set<std::string> prefix_match_ids;

	for (const ChatSession& native_chat : native_chats)
	{
		if (!native_chat.uses_native_session || native_chat.native_session_id.empty())
		{
			continue;
		}

		if (!IsMessagePrefixForNativeLinking(local_chat.messages, native_chat.messages))
		{
			continue;
		}

		prefix_match_ids.insert(native_chat.native_session_id);

		if (local_chat.messages.size() == native_chat.messages.size())
		{
			exact_match_ids.insert(native_chat.native_session_id);
		}
	}

	if (const auto exact_match = SingleSessionIdFromSet(exact_match_ids); exact_match.has_value())
	{
		return exact_match;
	}

	if (local_chat.messages.size() >= 3)
	{
		return SingleSessionIdFromSet(prefix_match_ids);
	}

	return std::nullopt;
}

std::vector<std::string> NativeSessionLinkService::CollectNewSessionIds(const std::vector<ChatSession>& loaded_chats,
	                                                                      const std::vector<std::string>& existing_ids) const
{
	std::unordered_set<std::string> seen(existing_ids.begin(), existing_ids.end());
	std::vector<std::string> discovered;

	for (const ChatSession& chat : loaded_chats)
	{
		if (!chat.native_session_id.empty() && seen.find(chat.native_session_id) == seen.end())
		{
			discovered.push_back(chat.native_session_id);
		}
	}

	return discovered;
}

std::string NativeSessionLinkService::PickFirstUnblockedSessionId(const std::vector<std::string>& candidate_ids,
	                                                                const std::unordered_set<std::string>& blocked_ids) const
{
	for (const std::string& candidate : candidate_ids)
	{
		if (!candidate.empty() && blocked_ids.find(candidate) == blocked_ids.end())
		{
			return candidate;
		}
	}

	return "";
}

bool NativeSessionLinkService::SessionIdExistsInLoadedChats(const std::vector<ChatSession>& loaded_chats,
	                                                        const std::string& session_id) const
{
	if (session_id.empty())
	{
		return false;
	}

	for (const ChatSession& chat : loaded_chats)
	{
		if (chat.uses_native_session && chat.native_session_id == session_id)
		{
			return true;
		}
	}

	return false;
}

