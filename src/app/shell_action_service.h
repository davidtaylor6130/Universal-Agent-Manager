#pragma once

#include "common/state/app_state.h"

#include <filesystem>
#include <string>
#include <vector>

class ShellActionService
{
  public:
	static std::vector<ShellAction> Load(const std::filesystem::path& data_root);
	static bool Save(const std::filesystem::path& data_root, const std::vector<ShellAction>& actions, std::string* error_out = nullptr);
	static bool Apply(const uam::AppState& app, const std::filesystem::path& executable, std::string* error_out = nullptr);
	static bool QueueLaunchRequest(const std::filesystem::path& data_root, const std::vector<std::string>& args, bool* recognized_out, std::string* error_out = nullptr);
	static bool ProcessPendingRequests(uam::AppState& app);
};
