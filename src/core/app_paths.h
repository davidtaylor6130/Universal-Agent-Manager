#pragma once

#include <filesystem>
#include <optional>
#include <string>

class AppPaths {
 public:
  static std::filesystem::path SettingsFilePath(const std::filesystem::path& data_root);
  static std::filesystem::path ChatsRootPath(const std::filesystem::path& data_root);
  static std::filesystem::path ChatPath(const std::filesystem::path& data_root, const std::string& chat_id);
  static std::filesystem::path GeminiHomePath();
  static std::filesystem::path DefaultGeminiUniversalRootPath();
  static std::optional<std::filesystem::path> ResolveGeminiProjectTmpDir(const std::filesystem::path& project_root);
};
