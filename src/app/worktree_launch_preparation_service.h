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

	/// <summary>Starts Git-only preparation, then admits launch only after live workspace metadata is saved.</summary>
	WorktreeLaunchPreparationResult PrepareWorktreeForLaunch(AppState& app, ChatSession& chat);
	bool PollWorktreeLaunchPreparations(AppState& app);
	void CancelWorktreeLaunchPreparation(AppState& app, const std::string& chat_id, const std::shared_ptr<std::stop_source>& expected = {});
}
