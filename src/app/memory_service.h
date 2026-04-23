#ifndef UAM_APP_MEMORY_SERVICE_H
#define UAM_APP_MEMORY_SERVICE_H

#include "common/state/app_state.h"

#include <filesystem>
#include <string>
#include <vector>

class MemoryService
{
  public:
	struct ManualScanCandidate
	{
		std::string chat_id;
		std::string title;
		std::string folder_id;
		std::string folder_title;
		std::string provider_id;
		int message_count = 0;
		bool memory_enabled = true;
		std::string memory_last_processed_at;
		bool already_fully_processed = false;
	};

	static std::filesystem::path GlobalMemoryRoot(const std::filesystem::path& data_root);
	static std::filesystem::path LocalMemoryRoot(const std::filesystem::path& workspace_root);
	static std::filesystem::path CategoryPath(const std::filesystem::path& root, const std::string& category);
	static bool EnsureMemoryLayout(const std::filesystem::path& root);
	static std::string BuildRecallPreface(const uam::AppState& app, const ChatSession& chat, const std::string& prompt);
	static bool ApplyWorkerOutput(uam::AppState& app, ChatSession& chat, const std::filesystem::path& workspace_root, const std::string& output, int processed_message_count = -1, std::string* error_out = nullptr);
	static uam::MemoryActivityState BuildMemoryActivity(const uam::AppState& app);
	static void RefreshMemoryActivity(uam::AppState& app);
	static std::string BuildWorkerCommandForTests(const ProviderProfile& profile, const AppSettings& settings, const std::string& prompt, const std::string& model_id);
	static std::string BuildWorkerPromptForTests(const ChatSession& chat, int start_message_index = -1);
	static std::vector<ManualScanCandidate> ListManualScanCandidates(const uam::AppState& app);
	static bool QueueManualScan(uam::AppState& app, const std::vector<std::string>& chat_ids, int* queued_count_out = nullptr, std::string* error_out = nullptr);
	static bool ProcessDueMemoryWork(uam::AppState& app);
	static void StopMemoryTasks(uam::AppState& app);
};

#endif // UAM_APP_MEMORY_SERVICE_H
