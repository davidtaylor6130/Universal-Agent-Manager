#pragma once

#include "common/state/app_state.h"

#include <filesystem>
#include <stop_token>
#include <string>

namespace uam
{
	struct GitWorktreeStatus
	{
		bool is_git_repository = false;
		bool is_svn_workspace = false;
		bool managed_repository = false;
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

	struct GitTurnCheckpointResult
	{
		bool ok = false;
		bool changed = false;
		std::string checkpoint_sha;
		std::string parent_sha;
		std::string diff;
		std::string message;
	};

	class GitWorktreeService
	{
	  public:
		GitWorktreeStatus Status(const AppState& app, const ChatSession& chat) const;
		GitWorktreeOperationResult CreateForChat(AppState& app, ChatSession& chat) const;
		GitWorktreeOperationResult DiscardChatChanges(AppState& app, ChatSession& chat) const;
		GitWorktreeOperationResult PortChatChanges(AppState& app, ChatSession& chat) const;
		bool CanCheckpointTurn(const AppState& app, const ChatSession& chat, std::string* reason_out = nullptr, std::stop_token stop_token = {}) const;
		GitTurnCheckpointResult CreateTurnCheckpoint(AppState& app, ChatSession& chat, int assistant_message_index, std::stop_token stop_token = {}) const;
		GitTurnCheckpointResult PreviewTurnRollback(const AppState& app, const ChatSession& chat, int assistant_message_index) const;
		GitTurnCheckpointResult RollbackTurn(AppState& app, ChatSession& chat, int assistant_message_index) const;
	};
} // namespace uam
