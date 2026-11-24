#pragma once

#include "app/git_worktree_service.h"

#include <chrono>
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
		/// Remote only: saved chat summaries and the read-only run ledger (including restart-inert v2 runs) observed by
		/// the worker. The owner admits references against them with live runtime state; they grant no authority.
		std::vector<ChatSession> references;
		std::vector<AgentRun> reference_runs;
		/// The read-only ledger read reported no unreadable record, so a run record absent from it has expired.
		bool reference_runs_complete = false;
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
		/// Remote only: past this point the owner cancels the worker, so an unanswered helper cannot hold the chat's workspace operation forever.
		std::chrono::steady_clock::time_point worker_deadline = std::chrono::steady_clock::time_point::max();
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
	/// A worktree chat launches only after its pending save, which may publish its workspace fields, reaches disk.
	bool WorktreeLaunchWaitsForChatSave(const AppState& app, const ChatSession& chat);
	/// <summary>True while this chat's preparation worker or its admission save is still running; a finished result is not running.</summary>
	bool IsWorktreeLaunchPreparationRunning(const AppState& app, const std::string& chat_id);
	bool PollWorktreeLaunchPreparations(AppState& app);
	void CancelWorktreeLaunchPreparation(AppState& app, const std::string& chat_id, const std::shared_ptr<std::stop_source>& expected = {});
}
