#pragma once

#include "common/state/app_state.h"
#include "app/opencode_import_stream.h"

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <cstdint>
#include <optional>
#include <map>
#include <memory>
#include <future>
#include "common/chat/chat_repository.h"
#include "common/chat/chat_folder_store.h"
#include "common/provider/antigravity_native_history.h"
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace uam
{
	class PreparedNativeMediaChatSave;
	class ImportedWorkspaceFolderIndex;
}

namespace uam::remote { class RunnerClient; }

class ChatHistorySyncService
{
  public:
	struct RemoteOpenCodeSession
	{
		std::string id;
		std::string title;
		std::string directory;
		std::int64_t created_epoch_ms = 0;
		std::int64_t updated_epoch_ms = 0;
		bool open_code_v2 = false;
	};
	struct RemoteOpenCodeTranscript
	{
		bool success = false;
		bool verified_empty = false;
		std::string session_id;
		std::string directory;
		std::vector<Message> messages;
		std::string error;
		/// The export's own `parentID`; empty for a root session. Last, so positional initialisers keep their meaning.
		std::string parent_session_id;
	};
	struct RemoteCodexSession
	{
		std::string id;
		std::string title;
		std::string directory;
		std::int64_t created_epoch_seconds = 0;
		std::int64_t updated_epoch_seconds = 0;
		/// The spawning thread Codex reports for a subagent thread; empty for a root thread. Last, so positional initialisers keep their meaning.
		std::string parent_thread_id;
	};
	/// <summary>One scan pass on one host: a shared runner client and its first connect failure.</summary>
	struct RemoteHistoryScanContext
	{
		explicit RemoteHistoryScanContext(const ExecutionHost& host);
		std::shared_ptr<uam::remote::RunnerClient> client;
		bool connect_failed = false;
		std::string connect_error;
	};

	struct RemoteOpenCodeDiscovery
	{
		std::vector<RemoteOpenCodeSession> sessions;
		std::string error;
	};
	RemoteOpenCodeDiscovery DiscoverRemoteOpenCodeSessions(const ChatFolder& folder, const ExecutionHost& host,
	    const ProviderProfile& profile, std::stop_token stop_token = {}, RemoteHistoryScanContext* scan = nullptr) const;
	static RemoteOpenCodeDiscovery ParseRemoteOpenCodeSessionList(std::string_view output, bool open_code_v2);
	struct RemoteCodexDiscovery
	{
		std::vector<RemoteCodexSession> sessions;
		std::string error;
	};
	struct RemoteCodexTranscript
	{
		bool success = false;
		std::string session_id;
		std::string directory;
		std::vector<Message> messages;
		std::string error;
	};

	struct ImportResult
	{
		int imported_count = 0;
		/// Sub-chats published behind a selected parent; included in imported_count.
		int imported_subchat_count = 0;
		int total_count = 0;
		bool success = true;
		std::vector<std::string> errors;
		std::vector<std::string> confirmed_empty_native_keys;
		// Exact rows saved by the partial local importer, eligible for sidebar adoption.
		std::vector<std::string> published_chat_ids;
		std::size_t omitted_error_count = 0;
		/// A history refused because the reader does not know one of its fields yet.
		/// Its message is also in errors; callers may report each one once.
		struct FormatRefusal
		{
			/// Provider and native session, stable across reader versions and file changes.
			std::string identity;
			/// Changes when the reader version or the native file changes.
			std::string report_key;
			std::string message;
			/// Detailed log line naming the message kind, field number and wire type.
			std::string diagnostic;
		};
		std::vector<FormatRefusal> format_refusals;

		bool partial() const { return imported_count > 0 && !success; }
		void Fail(std::string error)
		{
			success = false;
			if (!error.empty()) errors.push_back(std::move(error));
		}
		void Merge(const ImportResult& other)
		{
			imported_count += other.imported_count;
			imported_subchat_count += other.imported_subchat_count;
			total_count += other.total_count;
			omitted_error_count += other.omitted_error_count;
			if (!other.success) success = false;
			errors.insert(errors.end(), other.errors.begin(), other.errors.end());
			confirmed_empty_native_keys.insert(confirmed_empty_native_keys.end(), other.confirmed_empty_native_keys.begin(), other.confirmed_empty_native_keys.end());
			published_chat_ids.insert(published_chat_ids.end(), other.published_chat_ids.begin(), other.published_chat_ids.end());
			format_refusals.insert(format_refusals.end(), other.format_refusals.begin(), other.format_refusals.end());
		}
	};
	/// Final reconciliation may supply its still-attached terminal; ownership is revalidated before publication.
	ImportResult ImportAllNativeChatsToLocal(uam::AppState& app, bool delete_native_after_import, const std::string& target_chat_id = "", const uam::CliTerminalState* final_terminal = nullptr) const;
	ImportResult ImportAllNativeChatsByDiscovery(uam::AppState& app, bool delete_native_after_import, const std::string& target_chat_id = "") const;
	/// A loaded snapshot has no final-terminal permission; an error-free failure requests retry without a new diagnostic.
	ImportResult ImportLoadedNativeChatsToLocal(uam::AppState& app, std::vector<ChatSession> native_chats, const std::string& target_chat_id) const;
	// nullopt means the existing synchronous/non-media route owns this request.
	// Borrowed input remains caller-owned on capacity deferral; production supplies
	// its pre-admitted parser owner, transferred through preparation and retirement.
	std::optional<ImportResult> QueueLoadedTerminalNativeMedia(uam::AppState& app,
	    std::vector<ChatSession>& input, bool consume_input, std::string_view target,
	    bool preserve_selection, const std::shared_ptr<uam::platform::AsyncNativeChatLoadTask::State>& owner = {},
	    std::string_view digest = {}, const std::shared_ptr<uam::CliTerminalHistoryGeneration>& producer = {}) const;
	std::optional<ImportResult> QueueFinalTerminalNativeMedia(uam::AppState& app,
	    const uam::CliTerminalState& terminal, bool wait_for_stop) const;
	bool PollTerminalNativeMedia(uam::AppState& app) const;
	/// Includes final-stop waits and real prepare/publication/ACK completion.
	bool TerminalNativeMediaPending(const uam::AppState& app) const;
	/// Final captures take priority over admitting new periodic parser payloads.
	bool TerminalNativeMediaFinalPending(const uam::AppState& app) const;
	void CancelTerminalNativeMediaForChat(uam::AppState& app, std::string_view chat_id) const;
	void SettleTerminalNativeMediaForExit(uam::AppState& app) const;
	enum class ImportPolicy { ExplicitImport, RegisteredWorkspacesOnly, SelectedNewChatsOnly };
	struct PreparedHistoryChat
	{
		ChatSession chat;
		std::optional<ChatSession> expected_live;
		std::optional<ExecutionHost> expected_host;
		std::optional<ChatFolder> expected_folder;
		std::string expected_fingerprint;
		std::shared_ptr<PreparedChatSave> files;
		std::shared_ptr<uam::PreparedNativeMediaChatSave> native_media;
		bool native_media_acknowledged = false;
		std::filesystem::path codex_rollout;
		uam::opencode_import::Spool open_code_export;
		std::string error;
		bool retain_messages = false;
		bool skipped = false;
		bool confirmed_empty = false;
	};
	struct PreparedHistoryBatch
	{
		std::filesystem::path expected_data_root;
		std::vector<PreparedHistoryChat> chats;
		// Consumed media inputs rejected before preparation still retire on the armed worker.
		std::vector<ChatSession> retired_inputs;
		bool native_media_worker_owned = false;
		std::string expected_folders;
		std::vector<ChatFolder> folders;
		std::shared_ptr<PreparedFolderSave> folder_files;
	};
	struct LocalHistoryDiscovery
	{
		std::vector<ChatSession> chats;
		bool remote_host_unreachable = false;
		std::map<std::string, uam::opencode_import::Spool> open_code_exports;
		std::vector<std::pair<ChatSession, std::filesystem::path>> codex_rollouts;
		ImportResult result;
		/// Antigravity's per-conversation refusals, so an exact read can report its plain reason.
		std::vector<uam::antigravity::history::Refusal> antigravity_refusals;
		std::shared_future<std::shared_ptr<PreparedHistoryBatch>> preparation;
		std::shared_future<bool> publication_sync;
		std::optional<std::size_t> open_code_next_offset;
		std::size_t open_code_session_count = 0;
		std::size_t open_code_scanned_count = 0;
		std::string open_code_list_fingerprint;
		std::map<std::string, ExecutionHost> expected_hosts;
		std::map<std::string, ChatFolder> selected_folder_snapshots;
		// Explicitly reviewed local workspace definitions; published only with selected new chats.
		std::vector<ChatFolder> selected_new_folders;
		std::stop_source preparation_stop;
		bool Pending() const { return !chats.empty() || !codex_rollouts.empty() || preparation.valid() || publication_sync.valid(); }
		void CancelPending();
	};
	LocalHistoryDiscovery DiscoverRemoteProviderChatsForFolder(const ChatFolder& folder, const ExecutionHost& host, const ProviderProfile* open_code_profile, std::stop_token stop_token = {}, RemoteHistoryScanContext* scan = nullptr) const;
	/// Read provider transcripts without accessing live app state or writing local history.
	LocalHistoryDiscovery DiscoverProviderChatsForFolder(const ChatFolder& folder, const ProviderProfile* open_code_profile = nullptr, std::stop_token stop_token = {}, std::size_t open_code_scan_offset = 0, bool open_code_only = false, std::optional<std::size_t> open_code_remaining = {}, const std::string& open_code_list_fingerprint = {}, const std::string& provider_filter = {}, const std::string& native_session_filter = {}, const std::filesystem::path& native_history_root = {}) const;
	/// <summary>Resolves existing workspace ownership without creating folders. A local_folders index of app.folders resolves
	/// each registered folder once for a whole pass; an index of another folder list is ignored.</summary>
	const ChatFolder* StartupImportWorkspaceOwner(const uam::AppState& app, const ChatSession& chat, const uam::ImportedWorkspaceFolderIndex* local_folders = nullptr) const;
	/// <summary>Finds or plans (unsaved, in plan.folders) a workspace for an unmatched local history whose absolute directory exists; remote and other paths return null.
	/// An index of plan.folders is kept current as planned folders are appended.</summary>
	const ChatFolder* PlanImportWorkspace(uam::AppState& plan, const ChatSession& chat, uam::ImportedWorkspaceFolderIndex* local_folders = nullptr) const;
	/// <summary>What a startup filter pass may share or take from a snapshot instead of the live app.</summary>
	struct StartupImportFilterContext
	{
		/// <summary>Chats with a live terminal or running ACP session when the snapshot was taken; null reads the live app.</summary>
		const std::unordered_set<std::string>* live_chat_ids = nullptr;
		/// <summary>Resolved local workspaces shared across the pass (chat_identity::NativeWorkspaceCache).</summary>
		std::unordered_map<std::string, std::string>* workspaces = nullptr;
		/// <summary>An index of the filtered app's folders, shared across the pass.</summary>
		const uam::ImportedWorkspaceFolderIndex* local_folders = nullptr;
	};
	/// <summary>Chat IDs with a live terminal or running ACP session, for a StartupImportFilterContext snapshot. Reads memory only.</summary>
	std::unordered_set<std::string> StartupImportLiveChatIds(const uam::AppState& app) const;
	/// <summary>FilterStartupImportCandidates without changing anything: for each chat, in order, the owner folder ID it would
	/// keep ("" for a retained unregistered local workspace), or nullopt when the filter would remove it.</summary>
	bool ClassifyStartupImportCandidates(const uam::AppState& app, const std::vector<const ChatSession*>& chats, std::vector<std::optional<std::string>>& owners,
	    std::string* error_out = nullptr, bool include_unregistered_local = false, const StartupImportFilterContext* context = nullptr) const;
	/// <summary>Removes owned, declined and duplicate candidates; explicit local review may retain unmatched absolute workspaces.
	/// Each registered folder and distinct workspace is resolved once per call (or once per shared context).</summary>
	bool FilterStartupImportCandidates(const uam::AppState& app, LocalHistoryDiscovery& discovery, std::string* error_out = nullptr, bool include_unregistered_local = false, const StartupImportFilterContext* context = nullptr) const;
	/// Revalidate and merge a completed scan on the app thread.
	ImportResult ImportDiscoveredProviderChatsForFolder(uam::AppState& app, const ChatFolder& folder, LocalHistoryDiscovery discovery, bool sidebar_is_complete = false) const;
	ImportResult ImportDiscoveredProviderChatsBatch(uam::AppState& app, const ChatFolder& folder, LocalHistoryDiscovery& discovery, std::size_t max_chats = 8, ImportPolicy policy = ImportPolicy::ExplicitImport) const;
	ImportResult ImportProviderChatsForFolder(uam::AppState& app, const std::string& folder_id) const;
	ImportResult ImportRemoteOpenCodeChatsForFolder(
	    uam::AppState& app, const std::string& folder_id,
	    const std::vector<RemoteOpenCodeSession>& sessions) const;
	RemoteOpenCodeTranscript LoadRemoteOpenCodeTranscript(
	    const ExecutionHost& host, const ChatSession& chat, const ProviderProfile& profile, std::stop_token stop_token = {}, RemoteHistoryScanContext* scan = nullptr) const;
	/// Runs a bounded native export; callers must keep this off the GUI thread.
	RemoteOpenCodeTranscript LoadLocalOpenCodeTranscript(const ChatSession& chat, const ProviderProfile& profile, std::stop_token stop_token = {}) const;
	static RemoteOpenCodeTranscript ParseRemoteOpenCodeTranscript(std::string_view output);
	RemoteCodexDiscovery DiscoverRemoteCodexSessions(
	    const ExecutionHost& host, const ChatFolder& folder, std::stop_token stop_token = {}, RemoteHistoryScanContext* scan = nullptr) const;
	ImportResult ImportRemoteCodexChatsForFolder(
	    uam::AppState& app, const std::string& folder_id,
	    const std::vector<RemoteCodexSession>& sessions) const;
	/// Reads a local rollout without touching application state; run off the GUI thread.
	/// unsettled_out is set when the provider is still writing the exact conversation, so a
	/// passive refresh can keep saved history instead of reporting a failure.
	std::optional<ChatSession> LoadLocalNativeTranscript(const ChatSession& chat, std::string* error = nullptr, std::stop_token stop_token = {}, bool* unsettled_out = nullptr) const;
	/// <summary>Reads exact Gemini, Claude or Copilot native history through an authenticated immutable runner snapshot.</summary>
	std::optional<ChatSession> LoadRemoteNativeTranscript(const ExecutionHost& host, const ChatSession& chat, std::string* error = nullptr, std::stop_token stop_token = {}) const;
	/// <summary>Validates snapshot ownership and reuses provider-native parsers without changing environment variables.</summary>
	std::optional<ChatSession> ParseRemoteNativeHistorySnapshot(const ExecutionHost& host, const ChatSession& chat, const nlohmann::json& snapshot, std::string* error = nullptr, std::stop_token stop_token = {}) const;
	std::optional<ChatSession> LoadLocalCodexChildChat(const ChatSession& chat, std::string* error = nullptr) const;
	RemoteCodexTranscript LoadRemoteCodexTranscript(
	    const ExecutionHost& host, const ChatSession& chat, std::stop_token stop_token = {}, RemoteHistoryScanContext* scan = nullptr) const;
	static bool AppendRemoteCodexSessions(
	    const nlohmann::json& result, std::vector<RemoteCodexSession>& sessions,
	    std::string* error = nullptr);
	static RemoteCodexTranscript ParseRemoteCodexTranscript(const nlohmann::json& result, const ChatSession* input_receipt_owner = nullptr);
	ImportResult ImportCodexRolloutChatsForFolder(uam::AppState& app, const std::string& folder_id) const;
	bool AddNativeImportTombstones(const std::filesystem::path& data_root, const std::vector<ChatSession>& chats, std::vector<std::string>& added_keys, const std::unordered_set<std::string>& retained_keys = {}) const;
	bool FilterDeletedNativeHistory(const std::filesystem::path& data_root, std::vector<ChatSession>& chats, std::string* error_out = nullptr) const;
	// Exact deletion keys (NativeIdentityKeyForHistoryImport); nullopt when the records cannot be read.
	std::optional<std::unordered_set<std::string>> NativeImportTombstoneKeys(const std::filesystem::path& data_root) const;
	bool RemoveNativeImportTombstones(const std::filesystem::path& data_root, const std::vector<std::string>& keys) const;
	/// Notes unknown-format refusals in the import tombstone record, apart from deletion keys.
	/// Returns the messages of refusals already reported for the same reader version and file revision.
	std::unordered_set<std::string> RecordNativeFormatRefusals(const std::filesystem::path& data_root, const std::vector<ImportResult::FormatRefusal>& refusals) const;
	/// <summary>Save a validated native transcript before replacing the live chat; keep active or unsaved work.</summary>
	/// Build a loaded detached candidate without storage or live-runtime access; false preserves a longer baseline.
	bool BuildNativeTranscriptCandidate(ChatSession& detached, std::vector<Message> messages) const;
	bool SaveNativeTranscript(uam::AppState& app, ChatSession& current, std::vector<Message> messages) const;
	/// <summary>`refusal_out` receives a user-facing reason without paths; `status_line` keeps its own wording.</summary>
	bool SaveChatWithStatus(uam::AppState& app, ChatSession& chat, const std::string& success, const std::string& failure, std::string* refusal_out = nullptr) const;
	bool RenameChat(uam::AppState& app, ChatSession& chat, const std::string& requested_title, std::string* refusal_out = nullptr) const;
	std::vector<ChatSession> LoadNativeSessionChats(const std::filesystem::path& chats_dir, const ProviderProfile& provider, std::stop_token stop_token = {}) const;
	std::optional<std::filesystem::path> ResolveNativeHistoryChatsDirForWorkspace(const std::filesystem::path& workspace_root) const;
	std::filesystem::path ResolveNativeHistoryChatsDirForChat(const uam::AppState& app, const ChatSession& chat) const;
	void LoadSidebarChats(uam::AppState& app) const;
	/// Same, for summaries already read by ChatRepository::LoadLocalChatSummaries (for example off the app thread).
	void LoadSidebarChats(uam::AppState& app, std::vector<ChatSession> chats, std::string warning) const;
	/// <summary>Retains hidden agent identity without transcript bodies or additional storage reads.</summary>
	void RetainHiddenAgentChatOwnership(uam::AppState& app, const ChatSession& chat) const;
	/// Keep current rows except explicitly published IDs whose saved history should be adopted.
	void MergeSidebarChatsPreservingCurrent(uam::AppState& app, const std::vector<std::string>& published_chat_ids = {}) const;
	void RefreshNativeSessionDirectory(uam::AppState& app) const;
	bool StartAsyncNativeChatLoad(uam::AppState& app, const ProviderProfile& provider, const std::filesystem::path& chats_dir) const;
	bool TryConsumeAsyncNativeChatLoad(uam::AppState& app, std::vector<ChatSession>& chats_out, std::string& error_out) const;
	std::vector<std::string> SessionIdsFromChats(const std::vector<ChatSession>& chats) const;
	std::optional<std::filesystem::path> FindNativeSessionFilePath(const std::filesystem::path& chats_dir, const std::string& session_id, std::stop_token stop_token = {}) const;
	bool DeleteNativeSessionFileForChat(const uam::AppState& app, const ChatSession& chat, std::error_code* error_out = nullptr) const;
	bool DeleteNativeWorkspaceHistoryForFolder(const uam::AppState& app, const ChatFolder& folder, std::error_code* error_out = nullptr) const;
	std::string ResolveResumeSessionIdForChat(const uam::AppState& app, const ChatSession& chat) const;
	void ForgetResolvedNativeSessionForChat(uam::AppState& app, const std::string& chat_id) const;
	/// <summary>Roll back an uncommitted in-memory import without deleting previously saved history.</summary>
	void RollbackOpenNativeSessionChatImport(uam::AppState& app, const std::string& chat_id, const std::string& previous_selected_chat_id) const;
	void RestoreOpenNativeSessionResolvedMapping(uam::AppState& app, const std::string& chat_id, bool had_previous_resolved_native_session, const std::string& previous_resolved_native_session_id) const;
	ChatSession* FindInMemoryNativeSessionChatForOpen(
	    uam::AppState& app,
	    const ChatSession& source_chat,
	    const ProviderProfile& provider,
	    const std::string& native_session_id,
	    bool persist_resolved_mapping = true) const;
	ChatSession* FindOrImportNativeSessionChatForOpen(
	    uam::AppState& app,
	    const ChatSession& source_chat,
	    const ProviderProfile& provider,
	    const std::string& native_session_id,
	    bool persist_provider_normalization = true,
	    const ChatSession* native_snapshot = nullptr,
	    bool native_snapshot_prepared = false, std::stop_token stop_token = {}, const std::vector<ChatSession>* prepared_local_summaries = nullptr) const;
	bool PersistLocalDraftNativeSessionLink(const uam::AppState& app, ChatSession& local_chat, const std::string& native_session_id) const;
	void ApplyLocalOverrides(uam::AppState& app, std::vector<ChatSession>& native_chats, bool persist_local_draft_links = true) const;
	bool TruncateNativeSessionFromDisplayedMessage(const uam::AppState& app, const ChatSession& chat, int displayed_message_index, std::string* error_out) const;
	bool MoveChatToFolder(uam::AppState& app, ChatSession& chat, const std::string& new_folder_id) const;
	bool ExportChatToNative(const uam::AppState& app, const ChatSession& chat) const;

	/// <summary>Apply a hydrated baseline to a detached native snapshot without reading storage.</summary>
	void OverlayPreparedLocalHistory(const ChatSession& local_chat, ChatSession& native_chat) const;

	/// Applies persisted metadata to candidates without replacing live chats or session mappings.
	/// Required hydration failure clears candidates/returned rows and sets the optional retry error.
	/// Earlier completed inferred-link saves retain their existing ownership; this is not a batch rollback.
	std::vector<ChatSession> OverlayLocalHistory(const uam::AppState& app, std::vector<ChatSession>& native_chats, bool persist_local_draft_links, const std::vector<ChatSession>* prepared_local_summaries = nullptr, std::string* error_out = nullptr) const;
};
