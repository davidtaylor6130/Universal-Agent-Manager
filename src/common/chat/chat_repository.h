#pragma once

#include "common/models/app_models.h"

#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

struct ChatStorageDeleteResult
{
	bool unsafe_chat_id = false;
	std::error_code legacy_directory_error;
	std::error_code metadata_file_error;
	std::error_code metadata_backup_file_error;
	std::error_code summary_file_error;
	std::error_code summary_backup_file_error;

	bool Failed() const
	{
		return unsafe_chat_id || static_cast<bool>(legacy_directory_error) || static_cast<bool>(metadata_file_error) ||
		       static_cast<bool>(metadata_backup_file_error) || static_cast<bool>(summary_file_error) ||
		       static_cast<bool>(summary_backup_file_error);
	}
};

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
	/// <summary>Loads one locally persisted chat without scanning sibling files.</summary>
	static std::optional<ChatSession> LoadLocalChat(const std::filesystem::path& data_root, std::string_view chat_id, bool include_messages = true, std::string* warning_out = nullptr);
	/// <summary>Loads one locally persisted chat with its message bodies.</summary>
	static bool HydrateChatMessages(const std::filesystem::path& data_root, ChatSession& chat, std::string* warning_out = nullptr);
	/// <summary>Deletes both legacy chat directories and current UAM chat metadata for a chat id.</summary>
	static ChatStorageDeleteResult DeleteChatStorageFiles(const std::filesystem::path& data_root, std::string_view chat_id);
};
