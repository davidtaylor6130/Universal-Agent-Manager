#pragma once

#include "common/state/app_state.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uam
{
	/// <summary>Owned snapshot for background staging and cleanup. Live state is committed on the UI thread.</summary>
	struct WorkspaceDeletionTask
	{
		AppState snapshot;
		std::vector<std::string> folder_ids;
		std::unordered_set<std::string> deleted_ids;
		bool metadata_saved = false;
		bool settings_saved = false;
	};

	/// <summary>Checks live runtime safeguards and captures deletion snapshots on the UI thread.</summary>
	bool PrepareWorkspaceDeletion(AppState& app, const std::vector<std::string>& folder_ids, WorkspaceDeletionTask& task);
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
	bool CreateTemporarySideChat(AppState& app, const std::string& parent_id, std::string* created_id);
	bool RequestTemporarySideChatCleanup(AppState& app, const std::string& chat_id);
	bool PollTemporarySideChatCleanup(AppState& app);
	bool MigrateWorkspaceFolderOwnership(AppState& app);
	bool RecoverPendingDeletionTransaction(AppState& app);
	bool BranchFromMessageAndRetry(AppState& app, const std::string& source_chat_id, int message_index, const std::optional<std::string>& replacement_content, std::string* branch_id_out = nullptr, std::string* error_out = nullptr, const std::optional<std::string>& operation_id = std::nullopt, std::string* warning_out = nullptr);
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
