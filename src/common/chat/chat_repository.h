#pragma once

#include "common/models/app_models.h"

#include <filesystem>
#include <vector>

/// <summary>
/// Persists local chat metadata and message files.
/// </summary>
class ChatRepository
{
  public:
	/// <summary>Saves one chat session to disk.</summary>
	static bool SaveChat(const std::filesystem::path& data_root, const ChatSession& chat);
	/// <summary>Loads locally persisted chat sessions from disk.</summary>
	static std::vector<ChatSession> LoadLocalChats(const std::filesystem::path& data_root, std::string* warning_out = nullptr);
	/// <summary>Loads locally persisted chat sessions without hydrating message bodies.</summary>
	static std::vector<ChatSession> LoadLocalChatSummaries(const std::filesystem::path& data_root, std::string* warning_out = nullptr);
	/// <summary>Loads one locally persisted chat with its message bodies.</summary>
	static bool HydrateChatMessages(const std::filesystem::path& data_root, ChatSession& chat, std::string* warning_out = nullptr);
};
