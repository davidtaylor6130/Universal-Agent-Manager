#pragma once

#include <algorithm>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_set>

namespace uam::cef
{
	/// <summary>
	/// The refusal for a bridge action blocked by a pending per-chat worktree operation or the app-wide workspace
	/// deletion; empty when the action may run. Reads, selection and runtime stops stay available throughout.
	/// </summary>
	inline std::string_view PendingOperationRefusal(std::string_view action, bool chat_has_worktree_operation, bool any_worktree_operation, bool workspace_deletion_pending)
	{
		const bool is_runtime_stop = action == "cancelAcpTurn" || action == "stopAcpSession" || action == "cancelChatTitle";
		const bool is_cached_history_read = action == "getChatMessages" || action == "getToolCallContent" || action == "getAttachmentImagePreview";
		// Selecting a chat changes no chat data, so neither gate refuses the click.
		const bool is_selection = action == "selectSession";
		if (workspace_deletion_pending && is_cached_history_read && chat_has_worktree_operation) return "Chat deletion is in progress.";
		if ((!is_runtime_stop && !is_cached_history_read && !is_selection && chat_has_worktree_operation) ||
		    (any_worktree_operation && (action == "deleteFolder" || action == "deleteFolders" || action == "rescanFolderChats")))
			return "Wait for the chat worktree operation to finish.";
		// Read-only queries and unrelated runtime cancellation stay available during disk work.
		if (workspace_deletion_pending && !action.starts_with("get") && !action.starts_with("list") && !is_selection && !is_runtime_stop)
			return "Wait for workspace deletion to finish.";
		return {};
	}

	/// <summary>
	/// The refusal for a change that names a folder whose rejected deletion commit completes at restart: anything
	/// placed in that folder now would be deleted with it. Everything outside those folders stays available.
	/// </summary>
	inline std::string_view RestartDeletionFolderRefusal(std::string_view action, const nlohmann::json& payload, const std::unordered_set<std::string>& folder_ids)
	{
		if (folder_ids.empty() || !payload.is_object() || action.starts_with("get") || action.starts_with("list")) return {};
		const auto names = [&](const char* key)
		{
			const auto value = payload.find(key);
			return value != payload.end() && value->is_string() && folder_ids.contains(value->get_ref<const std::string&>());
		};
		const auto listed = payload.find("folderIds");
		const bool any_listed = listed != payload.end() && listed->is_array() && std::ranges::any_of(*listed, [&](const nlohmann::json& id)
		    { return id.is_string() && folder_ids.contains(id.get_ref<const std::string&>()); });
		return names("folderId") || names("destinationFolderId") || names("targetFolderId") || any_listed ? "Wait for workspace deletion to finish." : std::string_view{};
	}
}
