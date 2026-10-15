#pragma once

#include "app/git_worktree_service.h"

#include <future>
#include <memory>
#include <optional>
#include <stop_token>

namespace uam
{
	struct WorkspaceChatSaveTicket;
	enum class WorktreeLaunchPreparationState { Ready, Pending, Failed };
	struct WorktreeLaunchPreparationResult
	{
		WorktreeLaunchPreparationState state = WorktreeLaunchPreparationState::Ready;
		std::string error;
		std::shared_ptr<std::stop_source> cancellation;
	};
	struct PreparedLaunchWorktree
	{
		GitWorktreeOperationResult operation;
		ChatSession metadata;
	};
	struct WorktreeLaunchPreparationTask
	{
		std::filesystem::path data_root;
		std::filesystem::path receipt_file;
		ChatSession original;
		std::string resolved_source;
		std::string base_branch;
		bool pull_before_chat = false;
		bool git_enabled = true;
		bool automatic_worktree = true;
		bool snapshot_svn = true;
		bool snapshot_plain_folders = true;
		bool svn_enabled = true;
		bool svn_update_before_chat = false;
		bool update_only = false;
		bool explicit_create = false;
		std::optional<ExecutionHost> remote_host;
		std::shared_ptr<void> remote_host_lease;
		std::shared_ptr<std::stop_source> cancellation = std::make_shared<std::stop_source>();
		std::shared_ptr<WorkspaceLeaseGate::Lease> lease;
		std::future<PreparedLaunchWorktree> worker;
		std::optional<PreparedLaunchWorktree> prepared;
		std::optional<bool> saved;
		bool merged = false;
		bool saving = false;
		bool error_reported = false;
		bool retry_save_validation = false;
		std::shared_ptr<WorkspaceChatSaveTicket> save_ticket;
		std::string error;
		~WorktreeLaunchPreparationTask() { cancellation->request_stop(); }
	};

	/// <summary>Starts owned SVN update and optional Git preparation, then admits launch only after live workspace metadata is saved.</summary>
	WorktreeLaunchPreparationResult PrepareWorktreeForLaunch(AppState& app, ChatSession& chat, bool explicit_create = false);
	bool IsWorktreeLaunchPreparationPending(const AppState& app, const std::string& chat_id);
	bool PollWorktreeLaunchPreparations(AppState& app);
	void CancelWorktreeLaunchPreparation(AppState& app, const std::string& chat_id, const std::shared_ptr<std::stop_source>& expected = {});
}
