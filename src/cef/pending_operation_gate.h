#pragma once

#include <string_view>

namespace uam::cef
{
	/// <summary>
	/// The refusal for a bridge action blocked by a pending per-chat worktree operation or the app-wide workspace
	/// deletion; empty when the action may run. Reads, selection and runtime stops stay available throughout.
	/// </summary>
	inline std::string_view PendingOperationRefusal(std::string_view action, bool chat_has_worktree_operation, bool any_worktree_operation, bool workspace_deletion_pending, bool chat_deleted_at_restart = false)
	{
		const bool is_runtime_stop = action == "cancelAcpTurn" || action == "stopAcpSession" || action == "cancelChatTitle";
		const bool is_cached_history_read = action == "getChatMessages" || action == "getToolCallContent" || action == "getAttachmentImagePreview";
		// Selecting a chat changes no chat data, so neither gate refuses the click.
		const bool is_selection = action == "selectSession";
		if (workspace_deletion_pending && is_cached_history_read && chat_has_worktree_operation) return "Chat deletion is in progress.";
		// A rejected deletion's chats stay fenced until the restart that deletes them: nothing will "finish" before then.
		if (chat_deleted_at_restart && !is_runtime_stop && !is_cached_history_read && !is_selection) return "This chat will be deleted when UAM restarts.";
		if ((!is_runtime_stop && !is_cached_history_read && !is_selection && chat_has_worktree_operation) ||
		    (any_worktree_operation && (action == "deleteFolder" || action == "deleteFolders" || action == "rescanFolderChats")))
			return "Wait for the chat worktree operation to finish.";
		// Read-only queries and unrelated runtime cancellation stay available during disk work.
		if (workspace_deletion_pending && !action.starts_with("get") && !action.starts_with("list") && !is_selection && !is_runtime_stop)
			return "Wait for workspace deletion to finish.";
		return {};
	}

	/// <summary>
	/// A deletion whose commit was rejected keeps its recovery record, which completes it when UAM restarts. Until
	/// then another deletion (or a folder rescan) cannot start; it says so instead of asking the user to wait for
	/// something that never finishes. Everything else stays available.
	/// </summary>
	inline std::string_view UnfinishedDeletionRefusal(std::string_view action, bool deletion_awaits_restart)
	{
		return deletion_awaits_restart && (action == "deleteSession" || action == "deleteSessions" || action == "deleteFolder" ||
		        action == "deleteFolders" || action == "rescanFolderChats")
		    ? "An earlier deletion is unfinished and completes when UAM restarts. Restart UAM first, then try again." : std::string_view{};
	}
}
