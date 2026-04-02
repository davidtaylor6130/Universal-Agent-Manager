#pragma once

#include "app_models.h"

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
	static std::vector<ChatSession> LoadLocalChats(const std::filesystem::path& data_root);
};
