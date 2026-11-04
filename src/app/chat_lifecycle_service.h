#pragma once

#include "common/state/app_state.h"
#include "common/chat/chat_repository.h"

#include <optional>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace uam { class PreparedBranchOriginals; }

namespace uam
{
	/// <summary>Only the workspace and session needed to protect shared native history.</summary>
	struct NativeDeletionOwner
	{
		std::string workspace_directory;
		std::string native_session_id;
		bool unresolved_session = false;
	};

	struct DeletionRejection
	{
		std::string id;
		std::string error;
	};

	/// <summary>Owned snapshot for background staging and cleanup. Live state is committed on the UI thread.</summary>
	struct WorkspaceDeletionTask
	{
		AppState snapshot;
		std::vector<std::string> folder_ids;
		std::unordered_set<std::string> deleted_ids;
		std::unordered_set<std::string> retained_native_keys;
		std::unordered_set<std::string> retained_workspace_keys;
		std::vector<NativeDeletionOwner> retained_native_owners;
		std::unordered_map<std::string, std::string> native_stores_by_directory;
		/// <summary>Requested chat families or workspaces refused by their safeguards while the rest proceed.</summary>
		std::vector<DeletionRejection> rejected;
		/// <summary>Worker-only logical item completions; observers must marshal live state to the UI thread.</summary>
		std::function<void(std::size_t, std::size_t, const std::string&)> progress;
		bool commit_rejected = false;
		bool deleting_chats = false;
		bool metadata_saved = false;
		bool settings_saved = false;
	};

	/// <summary>Checks live runtime safeguards and captures deletion snapshots on the UI thread.</summary>
	bool PrepareWorkspaceDeletion(AppState& app, const std::vector<std::string>& folder_ids, WorkspaceDeletionTask& task);
	/// <summary>Preflights selected chats and captures only their rollback snapshots.</summary>
	bool PrepareChatDeletion(AppState& app, const std::vector<std::string>& chat_ids, WorkspaceDeletionTask& task);
	/// <summary>Multi-selection preflight: admits each branch family separately and records refused families in task.rejected.</summary>
	bool PrepareChatDeletionBatch(AppState& app, const std::vector<std::string>& chat_ids, WorkspaceDeletionTask& task);
	/// <summary>Multi-selection preflight: admits each workspace separately and records refused workspaces in task.rejected.</summary>
	bool PrepareWorkspaceDeletionBatch(AppState& app, const std::vector<std::string>& folder_ids, WorkspaceDeletionTask& task);
	/// <summary>Writes the durable recovery transaction using only owned snapshots on a worker.</summary>
	bool StageWorkspaceDeletion(WorkspaceDeletionTask& task);
	/// <summary>Persists branch and folder metadata, then merges deletion into current live state on the UI thread.</summary>
	void CommitWorkspaceDeletion(AppState& app, WorkspaceDeletionTask& task);
	/// <summary>Removes committed history on a worker; failures retain the recovery transaction.</summary>
	bool CleanupWorkspaceDeletion(WorkspaceDeletionTask& task);

	struct WorkspaceFolderRecoveryChat
	{
		std::string id;
		std::string title;
		std::string directory;
		std::string execution_host_id = "local";
		std::string reason;
	};

	struct WorkspaceFolderRecoveryGroup
	{
		std::string title;
		std::string directory;
		std::string execution_host_id = "local";
		std::string existing_folder_id;
		std::vector<std::string> chat_ids;
	};

	struct WorkspaceFolderRecoveryPreview
	{
		std::vector<WorkspaceFolderRecoveryGroup> groups;
		std::vector<WorkspaceFolderRecoveryChat> missing;
		std::vector<WorkspaceFolderRecoveryChat> unavailable;
		std::vector<WorkspaceFolderRecoveryChat> no_location;
	};

	struct WorkspaceFolderRecoveryResult
	{
		std::size_t organized_chat_count = 0;
		std::size_t created_folder_count = 0;
		std::size_t reused_folder_count = 0;
	};

	enum class ChatProviderSwitchResult
	{
		Changed,
		Unchanged,
		UnsupportedProvider,
		ChatNotFound,
		ActiveRuntime,
		RuntimeStopping,
		SaveFailed,
	};

	ChatProviderSwitchResult SwitchChatProvider(AppState& app, std::string_view chat_id, std::string_view provider_id);
	struct TemporarySideChatContext
	{
		ChatMessagePage page;
		std::string reference;
	};
	/// <summary>Builds a bounded reference from recent messages without copying full message bodies.</summary>
	std::string BuildTemporarySideChatContext(const std::vector<Message>& messages, std::string* error = nullptr);
	/// <summary>Reads a detached tail page on a worker; the parent remains unhydrated.</summary>
	std::optional<TemporarySideChatContext> PrepareTemporarySideChatContext(const std::filesystem::path& data_root,
	    const ChatSession& parent, const std::function<bool()>& cancelled = {});
	/// <summary>Builds a small candidate on the GUI thread without waiting for storage publication.</summary>
	std::optional<ChatSession> BuildTemporarySideChatCandidate(AppState& app, const std::string& parent_id,
	    const TemporarySideChatContext* prepared = nullptr, bool defer_context = false);
	/// <summary>Adopts a worker-verified durable candidate after current-parent checks, without reading primary JSON.</summary>
	bool AdoptTemporarySideChat(AppState& app, ChatSession candidate, const std::filesystem::path& saved_root,
	    std::string_view saved_revision, const std::shared_ptr<const std::uint8_t>& parent_incarnation,
	    const TemporarySideChatContext* prepared = nullptr);
	/// <summary>Synchronous durability helper; the GUI bridge uses candidate/background-save/adoption instead.</summary>
	bool CreateTemporarySideChat(AppState& app, const std::string& parent_id, std::string* created_id,
	    const TemporarySideChatContext* prepared = nullptr);
	bool RequestTemporarySideChatCleanup(AppState& app, const std::string& chat_id);
	bool PollTemporarySideChatCleanup(AppState& app);
	bool MigrateWorkspaceFolderOwnership(AppState& app);
	bool RecoverPendingDeletionTransaction(AppState& app);
	bool BranchFromMessageAndRetry(AppState& app, const std::string& source_chat_id, int message_index, const std::optional<std::string>& replacement_content, std::string* branch_id_out = nullptr, std::string* error_out = nullptr, const std::optional<std::string>& operation_id = std::nullopt, std::string* warning_out = nullptr, const std::optional<std::vector<std::string>>& retained_attachment_ids = std::nullopt, const ChatSession* prepared_source = nullptr, const ChatSession* prepared_existing_branch = nullptr, uam::PreparedBranchOriginals* prepared_originals = nullptr, const std::optional<std::vector<std::string>>& retained_markdown_store_files = std::nullopt, bool select_chat = true, bool regenerate = true);
	/// Companion callers retain their explicit selection/regeneration controls.
	bool BranchFromMessageAndRetry(AppState& app, const std::string& source_chat_id, int message_index, const std::optional<std::string>& replacement_content, std::string* branch_id_out, std::string* error_out, const std::optional<std::string>& operation_id, std::string* warning_out, const std::optional<std::vector<std::string>>& retained_attachment_ids, const ChatSession* prepared_source, const ChatSession* prepared_existing_branch, bool select_chat, bool regenerate = true);
	WorkspaceFolderRecoveryPreview PreviewUnsortedWorkspaceFolders(const AppState& app);
	bool RebuildUnsortedWorkspaceFolders(AppState& app, WorkspaceFolderRecoveryResult* result_out = nullptr);
}

bool RemoveChatById(uam::AppState& app, const std::string& chat_id);
bool RemoveChatsByIds(uam::AppState& app, const std::vector<std::string>& chat_ids);
bool DeleteFolderById(uam::AppState& app, const std::string& folder_id);
/// <summary>Preflights all selected workspaces and deletes them in one recoverable transaction.</summary>
bool DeleteFoldersByIds(uam::AppState& app, const std::vector<std::string>& folder_ids);
bool CreateFolder(uam::AppState& app, const std::string& title, const std::string& directory,
                  std::string* created_folder_id = nullptr,
                  const std::string& execution_host_id = "local");
bool RenameFolderById(uam::AppState& app, const std::string& folder_id, const std::string& title, const std::string& directory);
std::string ResolveRequestedNewChatFolderId(uam::AppState& app, const std::string& requested_folder_id = std::string());
