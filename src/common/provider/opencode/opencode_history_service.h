#pragma once

#include "common/models/app_models.h"

#include <filesystem>
#include <string>
#include <vector>

class OpenCodeHistoryService
{
  public:
	static std::vector<ChatSession> LoadOpenCodeHistory(const std::filesystem::path& data_root, const std::filesystem::path& workspace_directory);
	static std::string RunOpenCodeCommand(const std::vector<std::string>& args, const std::filesystem::path& cwd);

  private:
	static std::vector<ChatSession> ParseSessionListJson(const std::string& json_output, const std::filesystem::path& workspace_directory);
	static ChatSession ExportAndParseSession(const std::string& session_id, const std::filesystem::path& workspace_directory);
};
