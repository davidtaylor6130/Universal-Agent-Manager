#pragma once

#include "app/runtime_orchestration_services.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <chrono>
#include <ctime>
#include <map>
#include <set>
#include <mutex>
#include <future>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
		/// <summary>Shared by scopes admitted since the last pause; a pause stops them and installs a fresh source.</summary>
		std::stop_source cancel;
		/// <summary>An unreachable host's pending scopes wait until this time; the delay doubles per failed pass.</summary>
		std::chrono::steady_clock::time_point retry_at{};
		std::chrono::milliseconds retry_delay{0};
		bool unreachable_reported = false;
	};

	/// <summary>How an unreachable host is retried: one reconnect after reconnect_delay in the same pass, then later passes
	/// from first_retry, doubling up to max_retry.</summary>
	struct StartupNativeImportRemoteRetry
	{
		std::chrono::milliseconds reconnect_delay{3000};
		std::chrono::milliseconds first_retry{60000};
		std::chrono::milliseconds max_retry{900000};
	};

	struct StartupNativeImportRemoteScope
	{
		ChatFolder folder;
		ExecutionHost host;
		std::shared_ptr<StartupNativeImportRemoteScanGate> gate;
		std::shared_ptr<std::atomic<bool>> complete;
		std::uint64_t admission_generation = 0;
		// OpenCode history is listed by running its CLI, so a host where OpenCode is turned off is not asked.
		bool open_code_enabled = true;
	};

	struct StartupNativeImportScanRequest
	{
		std::optional<ProviderProfile> open_code_profile;
		bool open_code_local_enabled = true;
		std::size_t open_code_offset = 0;
		bool open_code_only = false;
		bool remote_only = false;
		std::optional<std::size_t> open_code_remaining;
		std::string open_code_list_fingerprint;
		std::vector<StartupNativeImportRemoteScope> remote_scopes;
	};

	/// <summary>Ownership inputs copied on the app thread when a scan or replan starts. The scan worker plans from these
	/// alone; it never reads live app state. Transcripts are not copied.</summary>
	struct StartupNativeImportPlanningSnapshot
	{
		/// <summary>Only data_root, folders, settings, chat ownership metadata, hidden and excluded chats and resolved native IDs are set.</summary>
		AppState app;
		std::unordered_set<std::string> live_chat_ids;
		/// <summary>Unsaved workspaces earlier candidates of this review were planned into.</summary>
		std::vector<ChatFolder> planned_folders;
		/// <summary>Native IDs (or journal chat IDs) that sub-chats staged before this pass name.</summary>
		std::unordered_set<std::string> child_named_ids;
		bool include_unregistered_local = false;
		std::time_t now = 0;
		/// <summary>StartupNativeImportService::PlanningFingerprint of the app and review when this was copied.</summary>
		std::uint64_t fingerprint = 0;
		/// <summary>The app-only part of that fingerprint, which saved-chat keys and owners depend on.</summary>
		std::uint64_t app_fingerprint = 0;
	};

	/// <summary>What the scan worker decided for one discovery. Entries refer to the discovery by position: chats first,
	/// then Codex rollouts. Valid only while the planning fingerprint still matches the app.</summary>
	struct StartupNativeImportPlan
	{
		struct History
		{
			std::size_t source = 0;
			std::string folder_id;
			std::string key;
			/// <summary>A workspace planned for this history that is not saved yet.</summary>
			std::optional<ChatFolder> new_workspace;
			bool deferred = false;
		};
		struct Child
		{
			std::size_t source = 0;
			std::string key;
			std::string parent_key;
		};
		std::uint64_t fingerprint = 0;
		std::uint64_t app_fingerprint = 0;
		bool filter_failed = false;
		std::string filter_error;
		std::vector<History> histories;
		std::vector<Child> children;
		/// <summary>Import keys of saved chats that a staged or found sub-chat names, by position in the snapshot's chats.</summary>
		std::unordered_map<std::size_t, std::string> saved_keys;
		/// <summary>Owning workspace ID ("" for none) of those saved chats that are controller-local, by the same position.</summary>
		std::unordered_map<std::size_t, std::string> saved_owners;
	};

	/// <summary>Stages provider history off-thread; only explicit selections may publish chats.</summary>
	class StartupNativeImportService
	{
	  public:
		using Scanner = std::function<ChatHistorySyncService::LocalHistoryDiscovery(const StartupNativeImportScanRequest&, std::stop_token)>;
		using RemoteScanner = std::function<ChatHistorySyncService::LocalHistoryDiscovery(const StartupNativeImportRemoteScope&, const ProviderProfile*, ChatHistorySyncService::RemoteHistoryScanContext&, std::stop_token)>;
		explicit StartupNativeImportService(Scanner scanner = {}, RemoteScanner remote_scanner = {}, StartupNativeImportRemoteRetry retry = {});
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
		/// <summary>Cancel running and prevent new remote scans on this host until every copy of the returned lease is released; cancelled scopes rescan later.</summary>
		std::shared_ptr<void> PauseBackgroundScanOnHost(const std::string& host_id);
		bool SelectedImportPendingOnHost(const std::string& host_id) const;
		/// <summary>Copies what planning reads from the app and this review. Reads memory only; runs on the app thread.</summary>
		StartupNativeImportPlanningSnapshot CapturePlanningSnapshot(const AppState& app) const;
		/// <summary>Hash of every app and review input a plan depends on. Reads memory only; a plan merges only while it matches.</summary>
		std::uint64_t PlanningFingerprint(const AppState& app) const;
		/// <summary>The app-only part of PlanningFingerprint: data root, folders, import settings, hosts and chat ownership.</summary>
		static std::uint64_t AppPlanningFingerprint(const AppState& app);
		/// <summary>Filters, resolves owners and identity keys, and plans workspaces for a discovery. Runs on the scan worker:
		/// each registered folder and distinct workspace is resolved once per call.</summary>
		static StartupNativeImportPlan PlanDiscovery(const StartupNativeImportPlanningSnapshot& snapshot, const ChatHistorySyncService::LocalHistoryDiscovery& discovery);

	  private:
		/// <summary>A scan worker's result: the discovery as found, and its plan against the snapshot taken at launch.</summary>
		struct ScanOutcome
		{
			ChatHistorySyncService::LocalHistoryDiscovery discovery;
			std::optional<StartupNativeImportPlan> plan;
			/// <summary>Errors and continuation counters of this discovery were already taken; only the plan was redone.</summary>
			bool replanned = false;
		};
		std::future<ScanOutcome> LaunchScan(const AppState& app, StartupNativeImportScanRequest request);
		/// <summary>Plans the same discovery again from a fresh snapshot after the app changed under the first plan.</summary>
		bool LaunchReplan(const AppState& app, ScanOutcome outcome);
		struct Candidate
		{
			StartupNativeImportCandidate summary;
			ChatSession chat;
			std::filesystem::path rollout;
			opencode_import::Spool open_code_export;
			ChatFolder owner;
			bool new_workspace = false;
			std::optional<ExecutionHost> host;
			/// <summary>NativeIdentityKeyForHistoryImport of chat, resolved on the scan worker.</summary>
			std::string key;
		};
		/// <summary>A sub-chat a scan found. It is never listed for review: it follows its parent's import decision.</summary>
		struct Child
		{
			ChatSession chat;
			std::filesystem::path rollout;
			std::string key;
			std::string parent_key;
		};
		/// <summary>Links a saved child that has no parent link to its saved parent, for a parent imported after its child.</summary>
		void LinkOwnedChildren(AppState& app);
		/// <summary>Children (and their children) of the given parent keys, each given its parent's workspace, at most 64 per parent including those `per_parent` already counts.</summary>
		std::vector<Child> CollectRiders(std::unordered_map<std::string, std::string>& folders, std::unordered_map<std::string, std::size_t> per_parent = {}) const;
		void QueueRiders(std::vector<Child> riders);
		/// <summary>After a scan the user started: imports the missing local sub-chats of chats already saved. Never runs for a startup scan.</summary>
		bool StartSavedParentBackfill(AppState& app);
		std::vector<Child> m_children;
		/// <summary>Set by a scan the user started and cleared when that scan settles, so a later background remote retry cannot repeat the backfill.</summary>
		bool m_backfill_due = false;
		/// <summary>Saved chats a found child could be or belong to, by import key. Uses the keys the last plan resolved while
		/// its fingerprint still matches; otherwise reads the file system only for chats whose native ID a child names.</summary>
		std::unordered_map<std::string, ChatSession*> SavedChatsNamedByChildren(AppState& app, bool planned_keys_current) const;
		bool PlannedKeysCurrent(const AppState& app) const;
		std::unordered_map<std::size_t, std::string> m_saved_keys;
		std::unordered_map<std::size_t, std::string> m_saved_owners;
		std::optional<std::uint64_t> m_saved_keys_fingerprint;
		bool Revalidate(AppState& app, std::string* error_out);
		void StartScan(const AppState& app, bool include_remote);
		void AddError(const std::string& error);
		void AddHostUnavailableError(const std::string& host_id, const std::string& fallback_name);
		void FinishImport(AppState& app);
		std::vector<StartupNativeImportRemoteScope> PendingRemoteScopes(const AppState& app);
		bool StartPendingRemoteScans(const AppState& app);
		/// <summary>True while a remote scope still needs a scan now; scopes of an unreachable host waiting for their retry time do not count.</summary>
		bool RemoteScansOutstanding() const;
		bool RemoteRetryDue() const;
		Scanner m_scanner;
		bool m_scoped_scanner = false;
		std::vector<StartupNativeImportRemoteScope> m_remote_scopes;
		StartupNativeImportScanRequest m_scan_request;
		std::map<std::string, ExecutionHost> m_remote_hosts;
		std::map<std::string, ChatFolder> m_remote_folders;
		std::set<std::string> m_unavailable_hosts_reported;
		bool m_initialized = false;
		bool m_include_unregistered_local = false;
		std::stop_source m_stop;
		std::future<ScanOutcome> m_scan;
		std::string m_phase = "idle";
		std::string m_generation;
		std::vector<Candidate> m_candidates;
		/// <summary>Remote sessions left out for age, an import filter or a missing workspace; explicit discovery reviews those eligible again.</summary>
		std::vector<Candidate> m_startup_deferred;
		void HoldBackIneligibleRemoteCandidates(const AppState& app);
		std::vector<Candidate> m_selected;
		ChatHistorySyncService::LocalHistoryDiscovery m_import;
		std::vector<std::string> m_errors;
		std::size_t m_omitted_error_count = 0;
		int m_imported_count = 0;
	};
}
