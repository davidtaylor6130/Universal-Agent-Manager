#ifndef UAM_APP_MEMORY_SERVICE_H
#define UAM_APP_MEMORY_SERVICE_H

#include "common/state/app_state.h"

#include <filesystem>
#include <string>

class MemoryService
{
  public:
	static std::filesystem::path GlobalMemoryRoot(const std::filesystem::path& data_root);
	static std::filesystem::path LocalMemoryRoot(const std::filesystem::path& workspace_root);
	static std::filesystem::path CategoryPath(const std::filesystem::path& root, const std::string& category);
	static bool EnsureMemoryLayout(const std::filesystem::path& root);
	static std::string BuildRecallPreface(const uam::AppState& app, const ChatSession& chat, const std::string& prompt);
	static bool ApplyWorkerOutput(uam::AppState& app, ChatSession& chat, const std::filesystem::path& workspace_root, const std::string& output, int processed_message_count = -1, std::string* error_out = nullptr);
	static bool ProcessDueMemoryWork(uam::AppState& app);
	static void StopMemoryTasks(uam::AppState& app);
};

#endif // UAM_APP_MEMORY_SERVICE_H
