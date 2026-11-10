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
	/// The refusal for a change to something a rejected deletion still owns: its recovery record completes that
	/// deletion at restart, so its chats stay unavailable and nothing new may be placed in its folders. Reads,
	/// selection and runtime stops stay available, and everything outside that deletion is unaffected.
	/// </summary>
	inline std::string_view RestartDeletionRefusal(std::string_view action, const nlohmann::json& payload,
	    const std::unordered_set<std::string>& chat_ids, const std::unordered_set<std::string>& folder_ids)
	{
		if ((chat_ids.empty() && folder_ids.empty()) || !payload.is_object() || action.starts_with("get") || action.starts_with("list") ||
		    action == "selectSession" || action == "cancelAcpTurn" || action == "stopAcpSession" || action == "cancelChatTitle") return {};
		const auto names = [&payload](const char* key, const std::unordered_set<std::string>& ids)
		{
			const auto value = payload.find(key);
			if (value == payload.end()) return false;
			if (value->is_string()) return ids.contains(value->get_ref<const std::string&>());
			return value->is_array() && std::ranges::any_of(*value, [&ids](const nlohmann::json& id)
			    { return id.is_string() && ids.contains(id.get_ref<const std::string&>()); });
		};
		if (names("chatId", chat_ids) || names("chatIds", chat_ids)) return "Chat deletion is in progress.";
		if (names("folderId", folder_ids) || names("folderIds", folder_ids) || names("destinationFolderId", folder_ids) || names("targetFolderId", folder_ids))
			return "Wait for workspace deletion to finish.";
		return {};
	}
}
