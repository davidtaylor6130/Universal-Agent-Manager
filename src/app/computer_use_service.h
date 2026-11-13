#pragma once

#include "common/state/app_state.h"

#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace uam
{
	class ComputerUseService
	{
	  public:
		static bool ResetControlsForStartup(AppState& app);
		static bool Poll(AppState& app, std::string_view chat_id = {});
		/// <summary>Scans control files off-thread and adopts only unchanged chat/control identities.</summary>
		bool PollInBackground(AppState& app);
		/// <summary>Whether a background scan is running or waiting to be adopted.</summary>
		bool ScanPendingForTests() const { return m_scan.valid(); }
		/// <summary>Chats copied into the most recent background scan.</summary>
		std::size_t LastScanChatCountForTests() const { return m_lastScanChatCount; }
		static bool SetControlState(AppState& app, const std::string& chat_id, std::string_view state,
		    std::string* error = nullptr);
		static bool PersistTrustedTask(AppState& app, const std::string& chat_id,
		    std::string_view prompt, std::string* error = nullptr);
	  private:
		struct Scan;
		std::future<std::shared_ptr<Scan>> m_scan;
		std::shared_ptr<Scan> m_retiredScan;
		double m_nextScanTime = 0.0;
		// Session folders the last finished scan found under its data root. Until one has, every chat is scanned.
		std::optional<std::unordered_set<std::string>> m_sessionIds;
		std::filesystem::path m_sessionIdsRoot;
		std::size_t m_lastScanChatCount = 0;
	};
} // namespace uam
