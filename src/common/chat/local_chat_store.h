#pragma once

#include "app_models.h"

#include <filesystem>
#include <vector>

/// <summary>
/// Shared local chat persistence store used by non-native provider runtimes.
/// </summary>
class LocalChatStore
{
  public:
	/// <summary>Loads all local chats from the app data root.</summary>
	static std::vector<ChatSession> Load(const std::filesystem::path& data_root);

	/// <summary>Saves one local chat to the app data root.</summary>
	static bool Save(const std::filesystem::path& data_root, const ChatSession& chat);
};
