#pragma once

#include "common/models/app_models.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <filesystem>
#include <functional>
#include <optional>

namespace uam
{
	/// <summary>Preserves chat-wide AND matching while returning bounded message provenance.</summary>
	nlohmann::json SearchChatMessages(const std::vector<ChatSession>& chats, const std::vector<std::string>& tokens);
	/// <summary>Reads one durable chat at a time and abandons superseded searches between records.</summary>
	std::optional<nlohmann::json> SearchSavedChatMessages(const std::filesystem::path& root, const std::vector<std::string>& tokens, const std::function<bool()>& canceled);
	/// <summary>Rejects a search cursor after its transcript changes, including same-size edits.</summary>
	bool ChatSearchTargetMatches(const ChatSession& chat, const std::string& digest, std::size_t message_index, bool reuse_selected_history = false);
}
