#pragma once

#include "common/models/app_models.h"
#include <string_view>

namespace uam
{
	inline bool BoundedChatReferenceValue(std::string_view value, std::size_t limit)
	{
		if (value.size() > limit) return false;
		for (unsigned char character : value)
		{
			if (character < 32 || character == 127) return false;
		}
		return true;
	}

	/// <summary>Display snapshots may settle only for the captured live chat birth and workspace association.</summary>
	inline bool SameChatReferenceContext(const ChatSession& current, const ChatSession& captured)
	{
		return current.id == captured.id && current.created_at == captured.created_at &&
		    captured.metadata_incarnation && current.metadata_incarnation == captured.metadata_incarnation &&
		    current.execution_host_id == captured.execution_host_id && current.folder_id == captured.folder_id &&
		    current.imported_read_only == captured.imported_read_only &&
		    current.workspace_directory == captured.workspace_directory &&
		    current.workspace_isolation_kind == captured.workspace_isolation_kind &&
		    current.workspace_source_directory == captured.workspace_source_directory &&
		    current.workspace_worktree_directory == captured.workspace_worktree_directory &&
		    current.workspace_branch_name == captured.workspace_branch_name &&
		    current.workspace_base_ref == captured.workspace_base_ref;
	}
}
