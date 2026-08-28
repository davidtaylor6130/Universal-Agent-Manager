#pragma once

#include "common/paths/path_utils.h"
#include "common/config/execution_host_config.h"
#include "common/platform/platform_services.h"
#include "common/state/app_state.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>

namespace uam::paths
{
	inline constexpr const char* kGitWorktreeIsolationKind = "gitWorktree";

	inline const ChatFolder* FindWorkspaceFolderById(const uam::AppState& app, std::string_view folder_id)
	{
		const std::string_view normalized_folder_id = uam::strings::TrimAsciiView(folder_id);
		const auto found = std::ranges::find_if(app.folders, [&normalized_folder_id](const ChatFolder& folder) {
			return uam::strings::TrimmedEquals(folder.id, normalized_folder_id);
		});
		return found == app.folders.end() ? nullptr : &*found;
	}

	inline bool IsGitWorktreeIsolated(const ChatSession& chat)
	{
		return uam::strings::TrimmedEquals(chat.workspace_isolation_kind, kGitWorktreeIsolationKind);
	}

	inline bool HasGitWorktreeSource(const ChatSession& chat)
	{
		return IsGitWorktreeIsolated(chat) && !uam::strings::IsBlank(chat.workspace_source_directory);
	}

	inline bool HasGitWorktreeDirectory(const ChatSession& chat)
	{
		return IsGitWorktreeIsolated(chat) && !uam::strings::IsBlank(chat.workspace_worktree_directory);
	}

	inline std::filesystem::path ExpandTrimmedWorkspacePath(std::string_view raw_path)
	{
		const std::string_view trimmed = uam::strings::TrimAsciiView(raw_path);
		return trimmed.empty() ? std::filesystem::path{} : PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(std::string(trimmed));
	}

	inline std::filesystem::path ResolveWorkspaceRootPath(const uam::AppState& app, const ChatSession& chat)
	{
		if (chat.imported_read_only)
		{
			return {};
		}
		if (chat.execution_host_id != uam::execution_hosts::kLocalHostId &&
		    !uam::strings::IsBlank(chat.workspace_directory))
			return std::filesystem::path(uam::strings::Trim(chat.workspace_directory));

		std::filesystem::path workspace_root;

		if (IsGitWorktreeIsolated(chat))
		{
			workspace_root = ExpandTrimmedWorkspacePath(chat.workspace_worktree_directory);
		}

		if (workspace_root.empty())
		{
			workspace_root = ExpandTrimmedWorkspacePath(chat.workspace_directory);
		}

		if (const ChatFolder* folder = FindWorkspaceFolderById(app, chat.folder_id); folder != nullptr && workspace_root.empty())
		{
			workspace_root = ExpandTrimmedWorkspacePath(folder->directory);
		}

		if (workspace_root.empty())
		{
			workspace_root = uam::paths::CurrentPathOrEmpty();
		}

		return uam::paths::AbsolutePathNoThrow(workspace_root);
	}
} // namespace uam::paths
