#pragma once

#include "app/runtime_orchestration_services.h"

#include <functional>
#include <map>
#include <future>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace uam
{
	struct StartupNativeImportCandidate
	{
		std::string candidate_id;
		std::string provider_id;
		std::string native_session_id;
		std::string title;
		std::string folder_id;
		std::string workspace_directory;
		std::string execution_host_id;
		std::string updated_at;
	};

	struct StartupNativeImportSnapshot
	{
		std::string phase = "idle";
		std::string generation;
		std::vector<StartupNativeImportCandidate> candidates;
		std::vector<std::string> errors;
		std::size_t total_count = 0;
		std::size_t next_offset = 0;
		int imported_count = 0;
	};

	struct StartupNativeImportRemoteScope
	{
		ChatFolder folder;
		ExecutionHost host;
	};

	struct StartupNativeImportScanRequest
	{
		std::optional<ProviderProfile> open_code_profile;
		std::size_t open_code_offset = 0;
		std::vector<StartupNativeImportRemoteScope> remote_scopes;
	};

	/// <summary>Scans provider history once off-thread; only explicit selections may publish chats.</summary>
	class StartupNativeImportService
	{
	  public:
		using Scanner = std::function<ChatHistorySyncService::LocalHistoryDiscovery(const StartupNativeImportScanRequest&, std::stop_token)>;
		explicit StartupNativeImportService(Scanner scanner = {});
		~StartupNativeImportService();
		void Initialize(const AppState& app);
		bool Poll(AppState& app);
		/// <summary>Returns bounded metadata only, with a cursor for the remaining candidates.</summary>
		StartupNativeImportSnapshot Snapshot(std::size_t offset = 0, std::size_t limit = 100,
		    const std::string& folder_id = {}) const;
		/// <summary>Rejects stale ownership or decisions outside the requested workspace before saving declines or scheduling imports.</summary>
		bool Decide(AppState& app, const std::string& generation, const std::vector<std::string>& selected_ids,
		    const std::vector<std::string>& declined_ids, std::string* error_out = nullptr, const std::string& folder_id = {}, bool import_all = false);
		void Cancel();
		bool ImportPending() const { return m_phase == "importing"; }

	  private:
		struct Candidate
		{
			StartupNativeImportCandidate summary;
			ChatSession chat;
			std::filesystem::path rollout;
			ChatFolder owner;
			std::optional<ExecutionHost> host;
		};
		bool Revalidate(AppState& app, std::string* error_out);
		void AddError(const std::string& error);
		void FinishImport(AppState& app);
		Scanner m_scanner;
		std::map<std::string, ExecutionHost> m_remote_hosts;
		std::map<std::string, ChatFolder> m_remote_folders;
		bool m_initialized = false;
		std::stop_source m_stop;
		std::future<ChatHistorySyncService::LocalHistoryDiscovery> m_scan;
		std::string m_phase = "idle";
		std::string m_generation;
		std::vector<Candidate> m_candidates;
		std::vector<Candidate> m_selected;
		ChatHistorySyncService::LocalHistoryDiscovery m_import;
		std::vector<std::string> m_errors;
		int m_imported_count = 0;
	};
}
