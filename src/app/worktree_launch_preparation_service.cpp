#include "app/worktree_launch_preparation_service.h"

#include "app/chat_domain_service.h"
#include "app/vcs_commit_service.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_root.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/utils/hash_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/time_utils.h"

#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>

namespace uam
{
	namespace
	{
		bool PhysicalWorkspaceFieldsMatch(const ChatSession& a, const ChatSession& b)
		{
			return a.workspace_isolation_kind == b.workspace_isolation_kind && a.workspace_source_directory == b.workspace_source_directory &&
			    a.workspace_worktree_directory == b.workspace_worktree_directory && a.workspace_branch_name == b.workspace_branch_name &&
			    a.workspace_base_ref == b.workspace_base_ref;
		}

		bool WorkspaceFieldsMatch(const ChatSession& a, const ChatSession& b)
		{
			return PhysicalWorkspaceFieldsMatch(a, b) && a.git_base_branch_override == b.git_base_branch_override;
		}

		bool Owns(const AppState& app, const ChatSession& chat, const WorktreeLaunchPreparationTask& task)
		{
			if (app.data_root != task.data_root || chat.id != task.original.id || chat.created_at != task.original.created_at ||
			    chat.metadata_incarnation != task.original.metadata_incarnation ||
			    chat.provider_id != task.original.provider_id || chat.execution_host_id != task.original.execution_host_id ||
			    chat.native_session_id != task.original.native_session_id || chat.routing_mode != task.original.routing_mode || chat.imported_read_only != task.original.imported_read_only ||
			    chat.agent_run_id != task.original.agent_run_id || chat.goal_owner_chat_id != task.original.goal_owner_chat_id ||
			    chat.temporary_parent_chat_id != task.original.temporary_parent_chat_id ||
			    chat.workspace_directory != task.original.workspace_directory || chat.folder_id != task.original.folder_id) return false;
			if (app.settings.git_enabled != task.git_enabled || app.settings.git_auto_worktree != task.automatic_worktree ||
			    app.settings.git_default_base_branch != task.base_branch || app.settings.git_pull_before_chat != task.pull_before_chat) return false;
			if (task.merged) return task.prepared && WorkspaceFieldsMatch(chat, task.prepared->metadata);
			return WorkspaceFieldsMatch(chat, task.original) && app.settings.git_enabled && app.settings.git_auto_worktree &&
			    paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, chat)) == task.resolved_source;
		}

		bool RecordIntent(const std::filesystem::path& root, const std::filesystem::path& receipt,
		    const std::string& chat_id, const std::string& created_at, const GitWorktreeCreationIntent& intent, std::string& error)
		{
			if (paths::IsLinkOrReparsePointNoThrow(root) || paths::IsLinkOrReparsePointNoThrow(receipt.parent_path()) ||
			    paths::IsLinkOrReparsePointNoThrow(receipt)) { error = "Workspace ownership storage is unsafe."; return false; }
			for (const std::filesystem::path& artifact : {intent.worktree, intent.managed_repository ? intent.repository : intent.worktree})
			{
				if (!paths::IsSameOrInsideRoot(root, artifact)) { error = "Workspace artifacts are outside owned storage."; return false; }
				const std::filesystem::path owned_root = paths::AbsolutePathNoThrow(root).lexically_normal();
				for (std::filesystem::path path = paths::AbsolutePathNoThrow(artifact).lexically_normal(); path != owned_root && !path.empty(); path = path.parent_path())
				{
					if (paths::IsLinkOrReparsePointNoThrow(path)) { error = "Workspace artifact storage is unsafe."; return false; }
					if (path == path.parent_path()) { error = "Workspace ownership root could not be validated."; return false; }
				}
			}
			const nlohmann::json record = {{"chatId", chat_id}, {"createdAt", created_at},
			    {"source", paths::Utf8PathString(intent.source)}, {"repository", paths::Utf8PathString(intent.repository)},
			    {"worktree", paths::Utf8PathString(intent.worktree)}, {"branch", intent.branch}, {"baseSha", intent.base_sha},
			    {"managedRepository", intent.managed_repository}};
			if (!io::WriteTextFile(receipt, record.dump())) { error = "Could not record workspace ownership."; return false; }
			return true;
		}

		void BeginSave(AppState& app, ChatSession& chat, const std::shared_ptr<WorktreeLaunchPreparationTask>& task)
		{
			task->saving = true;
			task->saved.reset();
			task->error.clear();
			task->error_reported = false;
			const std::weak_ptr<WorktreeLaunchPreparationTask> owner = task;
			task->save_ticket = ScheduleWorkspaceChatSave(app, chat, [&app, owner](bool saved)
			{
				if (const std::shared_ptr<WorktreeLaunchPreparationTask> task = owner.lock())
				{
					task->saving = false;
					task->saved = saved;
					if (!saved)
					{
						(void)RevokeWorkspaceChatSave(app, task->save_ticket);
						task->error = "Could not save the prepared workspace. Retry opening the chat.";
					}
				}
			});
			if (!task->save_ticket)
			{
				task->saving = false;
				task->saved = false;
				task->error = "Could not schedule the workspace save. Retry opening the chat.";
			}
		}

		void RestoreUnpublishedWorkspace(AppState& app, ChatSession& chat, const std::shared_ptr<WorktreeLaunchPreparationTask>& task)
		{
			if (!task->merged || !task->prepared || app.data_root != task->data_root ||
			    chat.id != task->original.id || chat.created_at != task->original.created_at ||
			    chat.metadata_incarnation != task->original.metadata_incarnation ||
			    chat.workspace_directory != task->original.workspace_directory || chat.folder_id != task->original.folder_id ||
			    !PhysicalWorkspaceFieldsMatch(chat, task->prepared->metadata)) return;
			const WorkspaceChatSaveRevocation revoked = RevokeWorkspaceChatSave(app, task->save_ticket);
			const bool preserve_save = revoked == WorkspaceChatSaveRevocation::ForeignNewer ||
			    (revoked != WorkspaceChatSaveRevocation::OwnedRevoked && app.pending_chat_save_at_by_chat_id.contains(chat.id));
			chat.workspace_isolation_kind = task->original.workspace_isolation_kind;
			chat.workspace_source_directory = task->original.workspace_source_directory;
			chat.workspace_worktree_directory = task->original.workspace_worktree_directory;
			chat.workspace_branch_name = task->original.workspace_branch_name;
			chat.workspace_base_ref = task->original.workspace_base_ref;
			task->merged = false;
			if (preserve_save) acp_detail::ScheduleChatSave(app, chat, 0.0);
		}

		WorktreeLaunchPreparationResult RevalidateWorkspace(AppState& app, const ChatSession& chat, const std::shared_ptr<WorktreeLaunchPreparationTask>& task, bool save_after)
		{
			if (task->worker.valid()) return {WorktreeLaunchPreparationState::Pending, {}, task->cancellation};
			const std::shared_ptr<WorkspaceLeaseGate::Lease> lease = app.worktree_retention_gate.TryAcquireMutation();
			if (!lease) return {WorktreeLaunchPreparationState::Pending, {}, {}};
			task->lease = lease;
			task->cancellation = std::make_shared<std::stop_source>();
			task->retry_save_validation = save_after;
			task->error.clear();
			task->error_reported = false;
			task->saved.reset();
			PreparedLaunchWorktree prepared = *task->prepared;
			if (!task->merged) task->prepared.reset();
			std::packaged_task<PreparedLaunchWorktree()> validation([root = task->data_root, receipt = task->receipt_file, prepared = std::move(prepared), cancellation = task->cancellation, lease]() mutable
			{
				prepared.operation.ok = GitWorktreeService().ValidatePreparedWorkspace(root, receipt, prepared.metadata, prepared.operation.message, cancellation->get_token());
				if (!prepared.operation.ok)
				{
					prepared.operation.message = "Prepared workspace ownership changed. The workspace was preserved.";
				}
				return prepared;
			});
			task->worker = validation.get_future();
			app.worktree_operation_chat_ids.insert(chat.id);
			try { std::thread(std::move(validation)).detach(); }
			catch (const std::exception&)
			{
				task->lease.reset();
				app.worktree_operation_chat_ids.erase(chat.id);
				task->retry_save_validation = false;
				task->error = "Could not validate the prepared workspace.";
			}
			return {WorktreeLaunchPreparationState::Pending, {}, task->cancellation};
		}

	}

	void CancelWorktreeLaunchPreparation(AppState& app, const std::string& chat_id, const std::shared_ptr<std::stop_source>& expected)
	{
		const std::unordered_map<std::string, std::shared_ptr<WorktreeLaunchPreparationTask>>::iterator found = app.worktree_launch_preparations.find(chat_id);
		if (found != app.worktree_launch_preparations.end() && (!expected || found->second->cancellation == expected)) found->second->cancellation->request_stop();
	}

	bool PollWorktreeLaunchPreparations(AppState& app)
	{
		bool changed = false;
		for (std::unordered_map<std::string, std::shared_ptr<WorktreeLaunchPreparationTask>>::iterator iterator = app.worktree_launch_preparations.begin(); iterator != app.worktree_launch_preparations.end();)
		{
			const std::shared_ptr<WorktreeLaunchPreparationTask> task = iterator->second;
			ChatSession* chat = ChatDomainService().FindChatById(app, iterator->first);
			const bool owned = chat != nullptr && Owns(app, *chat, *task);
			if (!owned) task->cancellation->request_stop();
			if (task->worker.valid())
			{
				if (task->worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++iterator; continue; }
				try { task->prepared = task->worker.get(); }
				catch (const std::exception&) { task->error = "Workspace preparation failed. Retry opening the chat."; }
				// The save worker excludes active Git operations; the lease still guards the unsaved workspace.
				app.worktree_operation_chat_ids.erase(iterator->first);
				changed = true;
			}
			if (!owned && task->merged && chat != nullptr && task->prepared &&
			    chat->git_base_branch_override != task->original.git_base_branch_override && !(task->saved && *task->saved))
			{
				// Restore only the old task's unchanged physical fields; the save flow preserves newer receipts.
				RestoreUnpublishedWorkspace(app, *chat, task);
			}
			if (!owned && task->merged && chat != nullptr && app.data_root == task->data_root &&
			    chat->metadata_incarnation == task->original.metadata_incarnation && task->prepared && WorkspaceFieldsMatch(*chat, task->prepared->metadata) &&
			    !(task->saved && *task->saved))
			{
				if (!task->saving)
				{
					task->error = "The prepared workspace changed before it was saved. Retry opening the chat.";
					task->lease.reset();
				}
				++iterator;
				continue;
			}
			if (owned && task->cancellation->stop_requested() && !task->merged && task->prepared && task->prepared->operation.ok)
			{
				// Keep the receipt and result for an explicit retry; cancellation cannot publish metadata.
				task->lease.reset();
				app.worktree_operation_chat_ids.erase(iterator->first);
				++iterator;
				continue;
			}
			if (!owned || (task->cancellation->stop_requested() && !task->merged))
			{
				// The worker's durable intent retains artifact locations; cancellation never removes branches or directories.
				task->lease.reset();
				app.worktree_operation_chat_ids.erase(iterator->first);
				iterator = app.worktree_launch_preparations.erase(iterator);
				changed = true;
				continue;
			}
			if (task->retry_save_validation && task->prepared)
			{
				task->retry_save_validation = false;
				if (!task->prepared->operation.ok)
				{
					RestoreUnpublishedWorkspace(app, *chat, task);
					task->error = task->prepared->operation.message;
				}
				else BeginSave(app, *chat, task);
			}
			if (task->prepared && !task->merged && task->error.empty())
			{
				if (!task->prepared->operation.ok) task->error = task->prepared->operation.message.empty() ? "Workspace preparation failed. Retry opening the chat." : task->prepared->operation.message;
				else
				{
					const ChatSession& metadata = task->prepared->metadata;
					chat->workspace_isolation_kind = metadata.workspace_isolation_kind;
					chat->workspace_source_directory = metadata.workspace_source_directory;
					chat->workspace_worktree_directory = metadata.workspace_worktree_directory;
					chat->workspace_branch_name = metadata.workspace_branch_name;
					chat->workspace_base_ref = metadata.workspace_base_ref;
					chat->updated_at = metadata.updated_at;
					task->merged = true;
					BeginSave(app, *chat, task);
				}
			}
			if (!task->error.empty() || (task->saved && *task->saved))
			{
				task->lease.reset();
				app.worktree_operation_chat_ids.erase(iterator->first);
			}
			++iterator;
		}
		return changed;
	}

	WorktreeLaunchPreparationResult PrepareWorktreeForLaunch(AppState& app, ChatSession& chat)
	{
		(void)PollWorktreeLaunchPreparations(app);
		std::string scope_error;
		if (!ValidatePendingGitBaseOverrideScope(app, chat, scope_error))
			return {WorktreeLaunchPreparationState::Failed, scope_error, {}};
		if (app.worktree_operation_chat_ids.contains(chat.id) && !app.worktree_launch_preparations.contains(chat.id))
			return {WorktreeLaunchPreparationState::Pending, {}, {}};
		const std::unordered_map<std::string, std::shared_ptr<WorktreeLaunchPreparationTask>>::iterator found = app.worktree_launch_preparations.find(chat.id);
		if (found != app.worktree_launch_preparations.end())
		{
			const std::shared_ptr<WorktreeLaunchPreparationTask> task = found->second;
			if (!task->merged && task->cancellation->stop_requested() && task->prepared && task->prepared->operation.ok)
			{
				return RevalidateWorkspace(app, chat, task, false);
			}
			if (task->merged && task->cancellation->stop_requested()) task->cancellation = std::make_shared<std::stop_source>();
			if (task->saved && *task->saved)
			{
				const std::shared_ptr<std::stop_source> cancellation = task->cancellation;
				app.worktree_launch_preparations.erase(found);
				return {WorktreeLaunchPreparationState::Ready, {}, cancellation};
			}
			if (!task->error.empty())
			{
				if (task->merged && task->error_reported)
				{
					if (app.data_root != task->data_root || chat.created_at != task->original.created_at ||
					    chat.metadata_incarnation != task->original.metadata_incarnation ||
					    chat.workspace_directory != task->original.workspace_directory || chat.folder_id != task->original.folder_id)
						return {WorktreeLaunchPreparationState::Failed, "The prepared workspace no longer belongs to this chat.", task->cancellation};
					task->git_enabled = app.settings.git_enabled;
					task->automatic_worktree = app.settings.git_auto_worktree;
					task->base_branch = app.settings.git_default_base_branch;
					task->pull_before_chat = app.settings.git_pull_before_chat;
					task->original.provider_id = chat.provider_id;
					task->original.execution_host_id = chat.execution_host_id;
					task->original.native_session_id = chat.native_session_id;
					task->original.imported_read_only = chat.imported_read_only;
					task->original.agent_run_id = chat.agent_run_id;
					task->original.goal_owner_chat_id = chat.goal_owner_chat_id;
					task->original.temporary_parent_chat_id = chat.temporary_parent_chat_id;
					return RevalidateWorkspace(app, chat, task, true);
				}
				task->error_reported = true;
				if (!task->merged) app.worktree_launch_preparations.erase(found);
				return {WorktreeLaunchPreparationState::Failed, task->error, task->cancellation};
			}
			return {WorktreeLaunchPreparationState::Pending, {}, task->cancellation};
		}
		if (paths::IsControllerLocalWorkspace(chat) && app.pending_chat_save_at_by_chat_id.contains(chat.id) &&
		    (chat.workspace_isolation_kind == paths::kPendingGitWorktreeIsolationKind || paths::IsGitWorktreeIsolated(chat)))
			return {WorktreeLaunchPreparationState::Pending, {}, {}};
		if (chat.workspace_isolation_kind != paths::kPendingGitWorktreeIsolationKind || !paths::IsControllerLocalWorkspace(chat) ||
		    chat.imported_read_only || !chat.agent_run_id.empty() || !chat.goal_owner_chat_id.empty() || !chat.temporary_parent_chat_id.empty()) return {};
		if (!app.settings.git_enabled || !app.settings.git_auto_worktree)
		{
			chat.workspace_isolation_kind.clear();
			return {};
		}
		if (app.worktree_operation_chat_ids.contains(chat.id)) return {WorktreeLaunchPreparationState::Failed, "Wait for the chat's workspace operation to finish.", {}};
		if (app.data_root.empty()) return {WorktreeLaunchPreparationState::Failed, "Chat storage is unavailable.", {}};
		const std::shared_ptr<WorkspaceLeaseGate::Lease> lease = app.worktree_retention_gate.TryAcquireMutation();
		if (!lease) return {WorktreeLaunchPreparationState::Pending, {}, {}};
		const std::shared_ptr<WorktreeLaunchPreparationTask> task = std::make_shared<WorktreeLaunchPreparationTask>();
		task->data_root = app.data_root;
		task->original = CaptureVcsChatContext(chat);
		task->original.created_at = chat.created_at;
		task->original.metadata_incarnation = chat.metadata_incarnation;
		task->original.native_session_id = chat.native_session_id;
		task->original.agent_run_id = chat.agent_run_id;
		task->original.goal_owner_chat_id = chat.goal_owner_chat_id;
		task->original.temporary_parent_chat_id = chat.temporary_parent_chat_id;
		task->original.title = chat.title;
		task->original.uam_agent_id = chat.uam_agent_id;
		task->original.workspace_base_ref = chat.workspace_base_ref;
		task->resolved_source = paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, chat));
		task->base_branch = app.settings.git_default_base_branch;
		task->pull_before_chat = app.settings.git_pull_before_chat;
		task->git_enabled = app.settings.git_enabled;
		task->automatic_worktree = app.settings.git_auto_worktree;
		task->lease = lease;
		task->receipt_file = task->data_root / ".worktree-launch" / (hashing::Hex64(hashing::Fnv1a64(chat.id)) + "-" + time::SteadyEpochNanosecondsTokenNow() + ".json");
		AppSettings settings;
		settings.git_enabled = app.settings.git_enabled;
		settings.svn_enabled = app.settings.svn_enabled;
		settings.git_auto_worktree = app.settings.git_auto_worktree;
		settings.git_default_base_branch = app.settings.git_default_base_branch;
		settings.git_pull_before_chat = app.settings.git_pull_before_chat;
		ChatSession metadata = task->original;
		metadata.workspace_directory = task->resolved_source;
		std::packaged_task<PreparedLaunchWorktree()> worker([root = task->data_root, receipt = task->receipt_file, settings = std::move(settings), metadata = std::move(metadata), cancellation = task->cancellation, lease]() mutable
		{
			AppState snapshot;
			snapshot.data_root = root;
			snapshot.settings = std::move(settings);
			const GitWorktreeIntentRecorder record = [root, receipt, id = metadata.id, created = metadata.created_at](const GitWorktreeCreationIntent& intent, std::string& error)
			{ return RecordIntent(root, receipt, id, created, intent, error); };
			GitWorktreeOperationResult operation = GitWorktreeService().PrepareForChat(snapshot, metadata, record, cancellation->get_token());
			return PreparedLaunchWorktree{std::move(operation), std::move(metadata)};
		});
		task->worker = worker.get_future();
		app.worktree_launch_preparations.emplace(chat.id, task);
		app.worktree_operation_chat_ids.insert(chat.id);
		try { std::thread(std::move(worker)).detach(); }
		catch (const std::exception&)
		{
			app.worktree_launch_preparations.erase(chat.id);
			app.worktree_operation_chat_ids.erase(chat.id);
			return {WorktreeLaunchPreparationState::Failed, "Could not start workspace preparation.", {}};
		}
		return {WorktreeLaunchPreparationState::Pending, {}, task->cancellation};
	}
}
