#ifndef UAM_APP_GIT_WORKTREE_SERVICE_H
#define UAM_APP_GIT_WORKTREE_SERVICE_H

#include "common/state/app_state.h"

#include <filesystem>
#include <string>

namespace uam
{
	struct GitWorktreeStatus
	{
		bool is_git_repository = false;
		bool is_svn_workspace = false;
		bool isolated = false;
		bool source_dirty = false;
		bool worktree_dirty = false;
		bool worktree_missing = false;
		std::string source_directory;
		std::string worktree_directory;
		std::string branch_name;
		std::string base_ref;
		std::string warning;
		std::string error;
	};

	struct GitWorktreeOperationResult
	{
		bool ok = false;
		GitWorktreeStatus status;
		std::filesystem::path patch_path;
		std::string message;
	};

	class GitWorktreeService
	{
	  public:
		GitWorktreeStatus Status(const AppState& app, const ChatSession& chat) const;
		GitWorktreeOperationResult CreateForChat(AppState& app, ChatSession& chat) const;
		GitWorktreeOperationResult DiscardChatChanges(AppState& app, ChatSession& chat) const;
		GitWorktreeOperationResult PortChatChanges(AppState& app, ChatSession& chat) const;
	};
} // namespace uam

#endif // UAM_APP_GIT_WORKTREE_SERVICE_H
