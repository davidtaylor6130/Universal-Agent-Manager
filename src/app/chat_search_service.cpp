#include "app/chat_search_service.h"
#include "cef/state_serializer.h"
#include "common/chat/chat_repository.h"
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
			std::vector<bool> matched(tokens.size(), false);
			std::string carry;
			std::size_t max_token_size = 0;
			for (const std::string& token : tokens) max_token_size = std::max(max_token_size, token.size());
			const auto scan_field = [&](const std::string& field)
			{
				const std::string lowered = carry + " " + strings::ToLowerAscii(field);
				for (std::size_t index = 0; index < tokens.size(); ++index)
					matched[index] = matched[index] || strings::Contains(lowered, tokens[index]);
				carry = lowered.substr(lowered.size() > max_token_size ? lowered.size() - max_token_size : 0);
			};
			scan_field(chat.title + " " + chat.provider_id + " " + chat.workspace_directory);
			for (const Message& message : chat.messages)
			{
				scan_field(message.content);
				scan_field(message.thoughts);
				scan_field(message.plan_summary);
			}
			if (!std::ranges::all_of(matched, [](bool found) { return found; })) continue;
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

	std::optional<nlohmann::json> SearchSavedChatMessages(const std::filesystem::path& root, const std::vector<std::string>& tokens, const std::function<bool()>& canceled)
	{
		nlohmann::json result{{"chatIds", nlohmann::json::array()}, {"hits", nlohmann::json::array()}};
		if (canceled()) return std::nullopt;
		std::vector<ChatSession> summaries = ChatRepository::LoadLocalChatSummaries(root);
		std::ranges::sort(summaries, [](const ChatSession& left, const ChatSession& right) { return left.updated_at > right.updated_at; });
		for (const ChatSession& summary : summaries)
		{
			if (canceled()) return std::nullopt;
			std::optional<ChatSession> chat = ChatRepository::LoadLocalChat(root, summary.id, true, nullptr, false);
			if (canceled()) return std::nullopt;
			if (!chat) continue;
			std::vector<ChatSession> single;
			single.push_back(std::move(*chat));
			nlohmann::json found = SearchChatMessages(single, tokens);
			for (nlohmann::json& id : found["chatIds"]) result["chatIds"].push_back(std::move(id));
			for (nlohmann::json& hit : found["hits"])
				if (result["hits"].size() < kMaxHits) result["hits"].push_back(std::move(hit));
		}
		return result;
	}

	bool ChatSearchTargetMatches(const ChatSession& chat, const std::string& digest, std::size_t message_index, bool reuse_selected_history)
	{
		return !digest.empty() && chat.messages_loaded && message_index < chat.messages.size() && StateSerializer::MessageDigest(chat, reuse_selected_history) == digest;
	}
}
