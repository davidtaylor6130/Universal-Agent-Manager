#pragma once

#include "app/runtime_orchestration_services.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
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
		std::string workspace_title;
		bool new_workspace = false;
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

	struct StartupNativeImportRemoteScanGate
	{
		std::mutex mutex;
		unsigned pauses = 0;
		unsigned active = 0;
		std::uint64_t generation = 0;
	};

	struct StartupNativeImportRemoteScope
	{
		ChatFolder folder;
		ExecutionHost host;
		std::shared_ptr<StartupNativeImportRemoteScanGate> gate;
		std::shared_ptr<std::atomic<bool>> complete;
		std::uint64_t admission_generation = 0;
	};

	struct StartupNativeImportScanRequest
	{
		std::optional<ProviderProfile> open_code_profile;
		std::size_t open_code_offset = 0;
		bool open_code_only = false;
		bool remote_only = false;
		std::optional<std::size_t> open_code_remaining;
		std::string open_code_list_fingerprint;
		std::vector<StartupNativeImportRemoteScope> remote_scopes;
	};

	/// <summary>Stages provider history off-thread; only explicit selections may publish chats.</summary>
	class StartupNativeImportService
	{
	  public:
		using Scanner = std::function<ChatHistorySyncService::LocalHistoryDiscovery(const StartupNativeImportScanRequest&, std::stop_token)>;
		using RemoteScanner = std::function<ChatHistorySyncService::LocalHistoryDiscovery(const StartupNativeImportRemoteScope&, const ProviderProfile*, std::stop_token)>;
		explicit StartupNativeImportService(Scanner scanner = {}, RemoteScanner remote_scanner = {});
		~StartupNativeImportService();
		void Initialize(const AppState& app);
		/// <summary>Starts a fresh controller-local scan without changing saved chats or overlapping active discovery/import.</summary>
		bool DiscoverLocalHistory(const AppState& app, std::string* error_out = nullptr);
		/// <summary>Stage an exact native workspace assignment without saving folders or chats.</summary>
		bool AssignWorkspace(AppState& app, const std::string& generation, const std::string& candidate_id,
		    const std::string& title, std::string* error_out = nullptr);
		bool Poll(AppState& app);
		/// <summary>Returns bounded metadata only, with a cursor for the remaining candidates.</summary>
		StartupNativeImportSnapshot Snapshot(std::size_t offset = 0, std::size_t limit = 100,
		    const std::string& folder_id = {}) const;
		/// <summary>Rejects stale ownership or decisions outside the requested workspace before saving declines or scheduling imports.</summary>
		bool Decide(AppState& app, const std::string& generation, const std::vector<std::string>& selected_ids,
		    const std::vector<std::string>& declined_ids, std::string* error_out = nullptr, const std::string& folder_id = {}, bool import_all = false);
		void Cancel();
		bool ImportPending() const { return m_phase == "importing"; }
		bool BackgroundScanPendingOnHost(const std::string& host_id) const;
		/// <summary>Prevent new remote scans on this host until every copy of the returned lease is released.</summary>
		std::shared_ptr<void> PauseBackgroundScanOnHost(const std::string& host_id);
		bool SelectedImportPendingOnHost(const std::string& host_id) const;

	  private:
		struct Candidate
		{
			StartupNativeImportCandidate summary;
			ChatSession chat;
			std::filesystem::path rollout;
			opencode_import::Spool open_code_export;
			ChatFolder owner;
			bool new_workspace = false;
			std::optional<ExecutionHost> host;
		};
		bool Revalidate(AppState& app, std::string* error_out);
		void StartScan(const AppState& app, bool include_remote);
		void AddError(const std::string& error);
		void FinishImport(AppState& app);
		std::vector<StartupNativeImportRemoteScope> PendingRemoteScopes(const AppState& app);
		bool StartPendingRemoteScans(const AppState& app);
		Scanner m_scanner;
		bool m_scoped_scanner = false;
		std::vector<StartupNativeImportRemoteScope> m_remote_scopes;
		StartupNativeImportScanRequest m_scan_request;
		std::map<std::string, ExecutionHost> m_remote_hosts;
		std::map<std::string, ChatFolder> m_remote_folders;
		bool m_initialized = false;
		bool m_include_unregistered_local = false;
		std::stop_source m_stop;
		std::future<ChatHistorySyncService::LocalHistoryDiscovery> m_scan;
		std::string m_phase = "idle";
		std::string m_generation;
		std::vector<Candidate> m_candidates;
		std::vector<Candidate> m_selected;
		ChatHistorySyncService::LocalHistoryDiscovery m_import;
		std::vector<std::string> m_errors;
		std::size_t m_omitted_error_count = 0;
		int m_imported_count = 0;
	};
}
