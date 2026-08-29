#pragma once

#include "common/state/app_state.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uam
{
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
		SaveFailed,
	};

	ChatProviderSwitchResult SwitchChatProvider(AppState& app, std::string_view chat_id, std::string_view provider_id);
	bool MigrateWorkspaceFolderOwnership(AppState& app);
	bool RecoverPendingDeletionTransaction(AppState& app);
	bool BranchFromMessageAndRetry(AppState& app, const std::string& source_chat_id, int message_index, const std::optional<std::string>& replacement_content, std::string* branch_id_out = nullptr, std::string* error_out = nullptr);
	WorkspaceFolderRecoveryPreview PreviewUnsortedWorkspaceFolders(const AppState& app);
	bool RebuildUnsortedWorkspaceFolders(AppState& app, WorkspaceFolderRecoveryResult* result_out = nullptr);
}

bool RemoveChatById(uam::AppState& app, const std::string& chat_id);
bool RemoveChatsByIds(uam::AppState& app, const std::vector<std::string>& chat_ids);
bool DeleteFolderById(uam::AppState& app, const std::string& folder_id);
bool CreateFolder(uam::AppState& app, const std::string& title, const std::string& directory,
                  std::string* created_folder_id = nullptr,
                  const std::string& execution_host_id = "local");
bool RenameFolderById(uam::AppState& app, const std::string& folder_id, const std::string& title, const std::string& directory);
std::string ResolveRequestedNewChatFolderId(uam::AppState& app, const std::string& requested_folder_id = std::string());
