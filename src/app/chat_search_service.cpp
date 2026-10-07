#include "app/chat_search_service.h"
#include "cef/state_serializer.h"
#include "common/utils/string_utils.h"
#include <algorithm>
#include <array>
#include <string_view>

namespace uam
{
	namespace
	{
		constexpr std::size_t kMaxHits = 32;
		constexpr std::size_t kMaxExcerptCharacters = 240;

		std::string Excerpt(const std::string& text, std::size_t match)
		{
			// UTF-8 continuation bytes never become the start or end of a snippet.
			std::size_t begin = match > 80 ? match - 80 : 0;
			while (begin > 0 && (static_cast<unsigned char>(text[begin]) & 0xc0) == 0x80) --begin;
			std::size_t end = begin;
			std::size_t characters = 0;
			while (end < text.size())
			{
				if ((static_cast<unsigned char>(text[end]) & 0xc0) != 0x80)
				{
					if (characters == kMaxExcerptCharacters) break;
					++characters;
				}
				++end;
			}
			return text.substr(begin, end - begin);
		}
	}

	nlohmann::json SearchChatMessages(const std::vector<ChatSession>& chats, const std::vector<std::string>& tokens)
	{
		nlohmann::json result{{"chatIds", nlohmann::json::array()}, {"hits", nlohmann::json::array()}};
		if (tokens.empty()) return result;
		for (const ChatSession& chat : chats)
		{
			std::string haystack = strings::ToLowerAscii(chat.title + " " + chat.provider_id + " " + chat.workspace_directory);
			for (const Message& message : chat.messages)
			{
				haystack += " " + strings::ToLowerAscii(message.content);
				haystack += " " + strings::ToLowerAscii(message.thoughts);
				haystack += " " + strings::ToLowerAscii(message.plan_summary);
			}
			if (!std::ranges::all_of(tokens, [&](const std::string& token) { return strings::Contains(haystack, token); })) continue;
			result["chatIds"].push_back(chat.id);
			if (result["hits"].size() >= kMaxHits) continue;
			const std::string digest = StateSerializer::MessageDigest(chat);
			for (std::size_t index = 0; index < chat.messages.size() && result["hits"].size() < kMaxHits; ++index)
			{
				const Message& message = chat.messages[index];
				const std::array<std::pair<std::string_view, const std::string*>, 3> fields = {{{"content", &message.content}, {"thoughts", &message.thoughts}, {"planSummary", &message.plan_summary}}};
				for (const std::pair<std::string_view, const std::string*>& field : fields)
				{
					const std::string lowered = strings::ToLowerAscii(*field.second);
					std::size_t first_match = std::string::npos;
					for (const std::string& token : tokens) first_match = std::min(first_match, lowered.find(token));
					if (first_match == std::string::npos) continue;
					result["hits"].push_back({{"chatId", chat.id}, {"messageIndex", index}, {"messagesDigest", digest}, {"field", field.first}, {"excerpt", Excerpt(*field.second, first_match)}});
					// One contributing field per message keeps results useful and bounded.
					break;
				}
			}
		}
		return result;
	}

	bool ChatSearchTargetMatches(const ChatSession& chat, const std::string& digest, std::size_t message_index)
	{
		return !digest.empty() && chat.messages_loaded && message_index < chat.messages.size() && StateSerializer::MessageDigest(chat) == digest;
	}
}
