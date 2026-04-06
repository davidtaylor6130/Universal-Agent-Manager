#pragma once

#include <filesystem>
#include <optional>
#include <string>

/// <summary>
/// Resolves canonical file-system paths used by the application.
/// </summary>
class AppPaths
{
  public:
	/// <summary>Returns the settings file path for a data root.</summary>
	static std::filesystem::path SettingsFilePath(const std::filesystem::path& data_root);
	/// <summary>Returns the chat root directory for a data root.</summary>
	static std::filesystem::path ChatsRootPath(const std::filesystem::path& data_root);
	/// <summary>Returns the on-disk folder path for a specific chat id.</summary>
	static std::filesystem::path ChatPath(const std::filesystem::path& data_root, const std::string& chat_id);
	/// <summary>Returns the preferred default data root path for this platform.</summary>
	static std::filesystem::path DefaultDataRootPath();
	/// <summary>Returns the user's Gemini home directory.</summary>
	static std::filesystem::path GeminiHomePath();
	/// <summary>Returns the global Gemini root directory for shared templates and config.</summary>
	static std::filesystem::path DefaultGeminiUniversalRootPath();
	/// <summary>Resolves Gemini's per-project temp directory for a workspace, when available.</summary>
	static std::optional<std::filesystem::path> ResolveGeminiProjectTmpDir(const std::filesystem::path& project_root);
	/// <summary>Returns the UAM chat files root directory (separate from native history).</summary>
	static std::filesystem::path UamChatsRootPath(const std::filesystem::path& data_root);
	/// <summary>Returns the UAM chat JSON file path for a specific chat id.</summary>
	static std::filesystem::path UamChatFilePath(const std::filesystem::path& data_root, const std::string& chat_id);
};

bool FolderDirectoryMatches(const std::filesystem::path& lhs, const std::filesystem::path& rhs);
