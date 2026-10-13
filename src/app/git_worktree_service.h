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

	struct GitWorktreeRetentionCandidate
	{
		std::filesystem::path directory;
		std::filesystem::path administrative_directory;
		std::string branch_name;
		std::string head_sha;
		std::filesystem::file_time_type activity_at;
	};

	struct GitTurnRollbackSnapshot
	{
		std::string chat_id;
		std::string native_identity_key;
		std::string provider_id;
		std::string execution_host_id;
		std::string workspace_directory;
		std::string workspace_isolation_kind;
		std::string workspace_source_directory;
		std::string workspace_base_ref;
		std::string workspace_branch_name;
		std::string workspace_worktree_directory;
		std::string updated_at;
		std::string persisted_messages_digest;
		std::size_t message_count = 0;
		int message_index = -1;
		std::string checkpoint_sha;
		std::string checkpoint_parent_sha;
	};

	GitTurnRollbackSnapshot CaptureGitTurnRollbackSnapshot(const ChatSession& chat, int message_index);
	bool MatchesGitTurnRollbackSnapshot(const ChatSession& chat, const GitTurnRollbackSnapshot& snapshot);
	bool ApplyGitTurnRollbackSnapshot(ChatSession& live_chat, const GitTurnRollbackSnapshot& before,
	                                  const ChatSession& persisted_after, const std::string& checkpoint_sha);

	class GitWorktreeService
	{
	  public:
		/// Background-only inspection. Removal requires a separate exclusive workspace lease and fresh reference validation.
		bool InspectRetentionCandidate(const std::filesystem::path& data_root, const std::filesystem::path& source_root,
		    const std::filesystem::path& worktree, const std::vector<std::filesystem::path>& protected_paths,
		    GitWorktreeRetentionCandidate& candidate, std::stop_token stop_token = {}, int retention_days = 14) const;
		/// Requires the live data-root gate's exclusive lease and freshly captured references. Keeps the branch and its commits.
		bool RemoveRetentionCandidate(const std::filesystem::path& data_root, const std::filesystem::path& source_root,
		    const GitWorktreeRetentionCandidate& candidate, const std::vector<std::filesystem::path>& protected_paths,
		    const WorkspaceLeaseGate::Lease& lease, std::stop_token stop_token = {}, int retention_days = 14) const;
		GitWorktreeStatus Status(const AppState& app, const ChatSession& chat) const;
		/// Creates the saved default worktree before a new local chat launches a provider.
		bool PrepareForFirstLaunch(AppState& app, ChatSession& chat, std::string* error_out = nullptr) const;
		GitWorktreeOperationResult CreateForChat(AppState& app, ChatSession& chat) const;
		GitWorktreeOperationResult CreateBranchForChat(const AppState& app, const ChatSession& source, ChatSession& branch, const std::string& checkpoint_sha) const;
		bool RemoveUnusedBranchWorktree(const AppState& app, const ChatSession& branch, std::string* error_out = nullptr) const;
		GitWorktreeOperationResult DiscardChatChanges(AppState& app, ChatSession& chat) const;
		GitWorktreeOperationResult PortChatChanges(AppState& app, ChatSession& chat) const;
		bool CanCheckpointTurn(const AppState& app, const ChatSession& chat, std::string* reason_out = nullptr, std::stop_token stop_token = {}) const;
		/// Async snapshots set persist_chat=false; the UI owner merges and saves checkpoint metadata.
		GitTurnCheckpointResult CreateTurnCheckpoint(AppState& app, ChatSession& chat, int assistant_message_index, std::stop_token stop_token = {}, bool persist_chat = true) const;
		GitTurnCheckpointResult PreviewTurnRollback(const AppState& app, const ChatSession& chat, int assistant_message_index) const;
		GitTurnCheckpointResult RollbackTurn(AppState& app, ChatSession& chat, int assistant_message_index) const;
	};
} // namespace uam
