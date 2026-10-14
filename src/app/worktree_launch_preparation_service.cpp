#include "app/worktree_launch_preparation_service.h"

#include "app/chat_domain_service.h"
#include "app/vcs_commit_service.h"
#include "common/chat/chat_repository.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_root.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/utils/hash_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/time_utils.h"
#include "common/utils/shell_escape.h"
#include "common/utils/string_utils.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <thread>
#include <sstream>

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
			return PhysicalWorkspaceFieldsMatch(a, b) && a.git_base_branch_override == b.git_base_branch_override && a.svn_update_pending == b.svn_update_pending;
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
			    app.settings.git_default_base_branch != task.base_branch || app.settings.git_pull_before_chat != task.pull_before_chat ||
			    app.settings.svn_enabled != task.svn_enabled || app.settings.svn_update_before_chat != task.svn_update_before_chat) return false;
			if (task.merged) return task.prepared && WorkspaceFieldsMatch(chat, task.prepared->metadata);
			return WorkspaceFieldsMatch(chat, task.original) && (task.update_only || (app.settings.git_enabled && app.settings.git_auto_worktree)) &&
			    paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, chat)) == task.resolved_source;
		}

		std::filesystem::path SvnCheckoutRoot(const std::filesystem::path& source)
		{
			for (std::filesystem::path directory = paths::NormalizeExistingOrAbsolutePath(source); !directory.empty(); directory = directory.parent_path())
			{
				if (paths::PathExistsNoThrow(directory / ".svn")) return directory;
				if (paths::PathExistsNoThrow(directory / ".git") || directory == directory.parent_path()) break;
			}
			return {};
		}

		bool PrepareSvnUpdate(const std::filesystem::path& root, const ChatSession& metadata, bool validate_only,
		    std::string& error, std::stop_token stop)
		{
			const std::filesystem::path source = paths::NormalizeExistingOrAbsolutePath(paths::PathFromUtf8(metadata.workspace_directory));
			const std::filesystem::path directory = root / ".svn-launch";
			const std::filesystem::path receipt = directory / (hashing::Hex64(hashing::Fnv1a64(metadata.id)) + ".json");
			const std::filesystem::path checkout = SvnCheckoutRoot(source);
			if (checkout.empty())
			{
				if (paths::PathExistsNoThrow(receipt)) { error = "The saved SVN update no longer matches this workspace."; return false; }
				return true;
			}
			if (!paths::IsDirectoryNoThrow(source) || paths::IsLinkOrReparsePointNoThrow(checkout / ".svn"))
			{ error = "The SVN workspace is unavailable or unsafe."; return false; }
			const std::string target = shell::EscapeArg(paths::Utf8PathString(checkout));
			bool command_uncertain = false;
			const auto command = [&](const std::string& arguments, std::string& output, int timeout = 10000)
			{
				const ProcessExecutionResult result = PlatformServicesFactory::Instance().process_service.ExecuteCommand(
				    "svn " + arguments + " --non-interactive -- " + target, timeout, stop);
				command_uncertain = stop.stop_requested() || result.canceled || result.timed_out || result.output_truncated;
				if (stop.stop_requested() || result.canceled) { error = "SVN update was canceled. Retry opening the chat."; return false; }
				if (result.timed_out) { error = "SVN update timed out. Retry after checking the working copy."; return false; }
				if (!result.ok || result.exit_code != 0 || result.output_truncated)
				{ error = "SVN preparation failed. Check the working copy and authentication, then retry."; return false; }
				output = strings::Trim(result.output);
				return true;
			};
			std::string checkout_root, url, uuid;
			if (!command("info --show-item wc-root", checkout_root) || !command("info --show-item url", url) ||
			    !command("info --show-item repos-uuid", uuid) || url.empty() || uuid.empty() ||
			    paths::NormalizeExistingOrAbsolutePath(paths::PathFromUtf8(checkout_root)) != checkout)
			{ if (error.empty()) error = "SVN checkout ownership could not be verified."; return false; }
			if (paths::IsLinkOrReparsePointNoThrow(root) || paths::IsLinkOrReparsePointNoThrow(directory) || paths::IsLinkOrReparsePointNoThrow(receipt))
			{ error = "SVN update ownership storage is unsafe."; return false; }
			std::string status;
			const auto conflicts = [](const std::string& value)
			{
				std::istringstream lines(value);
				std::string line;
				while (std::getline(lines, line))
					if ((!line.empty() && line[0] == 'C') || (line.size() > 1 && line[1] == 'C') || (line.size() > 6 && line[6] == 'C')) return true;
				return false;
			};
			if (!command("status", status)) return false;
			if (conflicts(status)) { error = "Resolve SVN conflicts before starting the chat."; return false; }
			const nlohmann::json identity = {{"version", 1}, {"chatId", metadata.id}, {"createdAt", metadata.created_at},
			    {"source", paths::Utf8PathString(source)}, {"checkout", paths::Utf8PathString(checkout)}, {"url", url}, {"uuid", uuid}};
			std::error_code ec;
			if (std::filesystem::exists(receipt, ec))
			{
				std::string text;
				try
				{
					if (!io::TryReadTextFile(receipt, text, 64 * 1024))
					{ error = "The saved SVN update receipt exceeded its size limit."; return false; }
					nlohmann::json saved = nlohmann::json::parse(text);
					const std::string state = saved.value("state", "");
					saved.erase("state");
					if (saved != identity)
					{ error = "The saved SVN update belongs to a different workspace. The working copy was preserved."; return false; }
					if (state == "complete") return true;
					if (state != "failed" || validate_only)
					{ error = "An interrupted SVN update needs working-copy verification before another update."; return false; }
				}
				catch (const std::exception&) { error = "The saved SVN update receipt could not be verified."; return false; }
			}
			if (ec || validate_only) { error = "The completed SVN update receipt is unavailable."; return false; }
			const auto save_receipt = [&](const char* state)
			{
				nlohmann::json record = identity;
				record["state"] = state;
				return io::WriteTextFile(receipt, record.dump());
			};
			if (stop.stop_requested()) { error = "SVN update was canceled."; return false; }
			if (!save_receipt("started")) { error = "Could not record SVN update ownership."; return false; }
			std::string output;
			if (!command("update --accept postpone", output, 120000))
			{
				if (!command_uncertain) (void)save_receipt("failed");
				return false;
			}
			if (!command("status", status)) return false;
			if (conflicts(status))
			{ (void)save_receipt("failed"); error = "SVN update left conflicts. Resolve them before starting the chat."; return false; }
			if (!save_receipt("complete"))
			{ error = "SVN updated, but its completion could not be saved. Check the working copy before retrying."; return false; }
			return true;
		}

		bool RecordIntent(const std::filesystem::path& root, const std::filesystem::path& receipt,
		    const std::string& chat_id, const std::string& created_at, const nlohmann::json& policy, const GitWorktreeCreationIntent& intent, std::string& error)
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
			    {"managedRepository", intent.managed_repository}, {"launchPolicy", policy}};
			if (!io::WriteTextFile(receipt, record.dump())) { error = "Could not record workspace ownership."; return false; }
			return true;
		}

		bool ReferencesPreparedWorkspace(const ChatSession& other, const ChatSession& prepared)
		{
			if (other.id == prepared.id || !paths::IsControllerLocalWorkspace(other)) return false;
			const std::filesystem::path worktree = paths::PathFromUtf8(prepared.workspace_worktree_directory);
			for (const std::string* path : {&other.workspace_directory, &other.workspace_worktree_directory, &other.workspace_source_directory})
				if (!path->empty() && paths::IsSameOrInsideRoot(worktree, paths::PathFromUtf8(*path))) return true;
			return false;
		}

		bool RecoverOwnedWorkspace(const std::filesystem::path& root, const std::vector<ChatSession>& live_chats,
		    ChatSession& metadata, const nlohmann::json& policy, std::filesystem::path& receipt, bool& found, std::string& error, std::stop_token stop)
		{
			const std::filesystem::path directory = root / ".worktree-launch";
			std::error_code scan_error;
			if (!std::filesystem::exists(directory, scan_error)) return !scan_error;
			if (paths::IsLinkOrReparsePointNoThrow(directory)) return false;
			const std::string prefix = hashing::Hex64(hashing::Fnv1a64(metadata.id)) + "-";
			try
			{
				for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory))
				{
					if (!entry.path().filename().string().starts_with(prefix) || entry.path().extension() != ".json") continue;
					if (found || paths::IsLinkOrReparsePointNoThrow(entry.path())) return false;
					found = true;
					std::string receipt_text;
					if (!io::TryReadTextFile(entry.path(), receipt_text, 64 * 1024))
					{
						error = "Interrupted workspace ownership receipt could not be read within its size limit. The workspace was preserved.";
						return false;
					}
					const nlohmann::json record = nlohmann::json::parse(receipt_text);
					if (record.at("chatId") != metadata.id || record.at("createdAt") != metadata.created_at ||
					    record.at("launchPolicy") != policy ||
					    !paths::IsSameOrInsideRoot(paths::PathFromUtf8(record.at("source").get<std::string>()), paths::PathFromUtf8(metadata.workspace_directory))) return false;
					metadata.workspace_isolation_kind = paths::kGitWorktreeIsolationKind;
					metadata.workspace_source_directory = record.at("source").get<std::string>();
					metadata.workspace_worktree_directory = record.at("worktree").get<std::string>();
					metadata.workspace_branch_name = record.at("branch").get<std::string>();
					metadata.workspace_base_ref = record.at("baseSha").get<std::string>();
					if (metadata.workspace_branch_name.empty() || metadata.workspace_base_ref.empty() ||
					    !GitWorktreeService().ValidatePreparedWorkspace(root, entry.path(), metadata, error, stop)) return false;
					const auto references = [&](const ChatSession& other) { return ReferencesPreparedWorkspace(other, metadata); };
					const std::vector<ChatSession> persisted = ChatRepository::LoadLocalChatSummariesReadOnly(root);
					if (std::any_of(live_chats.begin(), live_chats.end(), references) ||
					    std::any_of(persisted.begin(), persisted.end(), references)) return false;
					receipt = entry.path();
					metadata.updated_at = time::TimestampNow();
				}
				return true;
			}
			catch (const std::exception&) { return false; }
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
			chat.svn_update_pending = task->original.svn_update_pending;
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
			std::packaged_task<PreparedLaunchWorktree()> validation([root = task->data_root, receipt = task->receipt_file, prepared = std::move(prepared), cancellation = task->cancellation, update_only = task->update_only, svn_update = task->svn_enabled && task->svn_update_before_chat && task->original.svn_update_pending, lease]() mutable
			{
				prepared.operation.ok = update_only
				    ? (!svn_update || PrepareSvnUpdate(root, prepared.metadata, true, prepared.operation.message, cancellation->get_token()))
				    : GitWorktreeService().ValidatePreparedWorkspace(root, receipt, prepared.metadata, prepared.operation.message, cancellation->get_token());
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
				if (task->prepared->operation.ok && !task->update_only && std::any_of(app.chats.begin(), app.chats.end(),
				    [&](const ChatSession& other) { return ReferencesPreparedWorkspace(other, task->prepared->metadata); }))
				{
					task->prepared->operation.ok = false;
					task->prepared->operation.message = "Another chat uses the prepared workspace. The workspace was preserved.";
				}
				if (!task->prepared->operation.ok) task->error = task->prepared->operation.message.empty() ? "Workspace preparation failed. Retry opening the chat." : task->prepared->operation.message;
				else
				{
					const ChatSession& metadata = task->prepared->metadata;
					chat->workspace_isolation_kind = metadata.workspace_isolation_kind;
					chat->workspace_source_directory = metadata.workspace_source_directory;
					chat->workspace_worktree_directory = metadata.workspace_worktree_directory;
					chat->workspace_branch_name = metadata.workspace_branch_name;
					chat->workspace_base_ref = metadata.workspace_base_ref;
					chat->svn_update_pending = metadata.svn_update_pending;
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
					task->svn_enabled = app.settings.svn_enabled;
					task->svn_update_before_chat = app.settings.svn_update_before_chat;
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
		if ((chat.workspace_isolation_kind != paths::kPendingGitWorktreeIsolationKind && !chat.svn_update_pending) || !paths::IsControllerLocalWorkspace(chat) ||
		    chat.imported_read_only || !chat.agent_run_id.empty() || !chat.goal_owner_chat_id.empty() || !chat.temporary_parent_chat_id.empty()) return {};
		if ((!app.settings.git_enabled || !app.settings.git_auto_worktree) && !chat.svn_update_pending)
		{
			chat.workspace_isolation_kind.clear();
			return {};
		}
		if (app.worktree_operation_chat_ids.contains(chat.id)) return {WorktreeLaunchPreparationState::Failed, "Wait for the chat's workspace operation to finish.", {}};
		if (app.data_root.empty()) return {WorktreeLaunchPreparationState::Failed, "Chat storage is unavailable.", {}};
		if (chat.svn_update_pending && app.settings.svn_enabled && app.settings.svn_update_before_chat)
		{
			const std::filesystem::path checkout = SvnCheckoutRoot(paths::NormalizeExistingOrAbsolutePath(paths::ResolveWorkspaceRootPath(app, chat)));
			if (!checkout.empty())
				for (const auto& [id, operation] : app.worktree_launch_preparations)
					if (id != chat.id && operation->lease && operation->svn_enabled && operation->svn_update_before_chat && operation->original.svn_update_pending &&
					    SvnCheckoutRoot(paths::PathFromUtf8(operation->resolved_source)) == checkout)
						return {WorktreeLaunchPreparationState::Pending, {}, {}};
		}
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
		task->svn_enabled = app.settings.svn_enabled;
		task->svn_update_before_chat = app.settings.svn_update_before_chat;
		task->update_only = chat.workspace_isolation_kind != paths::kPendingGitWorktreeIsolationKind || !task->git_enabled || !task->automatic_worktree;
		task->lease = lease;
		task->receipt_file = task->data_root / ".worktree-launch" / (hashing::Hex64(hashing::Fnv1a64(chat.id)) + "-" + time::SteadyEpochNanosecondsTokenNow() + ".json");
		AppSettings settings;
		settings.git_enabled = app.settings.git_enabled;
		settings.svn_enabled = app.settings.svn_enabled;
		settings.svn_update_before_chat = app.settings.svn_update_before_chat;
		settings.git_auto_worktree = app.settings.git_auto_worktree;
		settings.git_default_base_branch = app.settings.git_default_base_branch;
		settings.git_pull_before_chat = app.settings.git_pull_before_chat;
		ChatSession metadata = task->original;
		metadata.workspace_directory = task->resolved_source;
		std::vector<ChatSession> live_chats;
		live_chats.reserve(app.chats.size());
		for (const ChatSession& other : app.chats) live_chats.push_back(CaptureVcsChatContext(other));
		std::packaged_task<PreparedLaunchWorktree()> worker([task, live_chats = std::move(live_chats), root = task->data_root, receipt = task->receipt_file, settings = std::move(settings), metadata = std::move(metadata), cancellation = task->cancellation, lease]() mutable
		{
			AppState snapshot;
			snapshot.data_root = root;
			snapshot.settings = std::move(settings);
			std::string update_error;
			if (metadata.svn_update_pending && snapshot.settings.svn_enabled && snapshot.settings.svn_update_before_chat &&
			    !PrepareSvnUpdate(root, metadata, false, update_error, cancellation->get_token()))
				return PreparedLaunchWorktree{GitWorktreeOperationResult{.message = update_error}, std::move(metadata)};
			metadata.svn_update_pending = false;
			if (task->update_only)
			{
				if (metadata.workspace_isolation_kind == paths::kPendingGitWorktreeIsolationKind) metadata.workspace_isolation_kind.clear();
				return PreparedLaunchWorktree{GitWorktreeOperationResult{.ok = true}, std::move(metadata)};
			}
			const nlohmann::json policy = {{"version", 1}, {"requestedSource", metadata.workspace_directory}, {"baseBranch", snapshot.settings.git_default_base_branch},
			    {"pullBeforeChat", snapshot.settings.git_pull_before_chat}, {"gitEnabled", snapshot.settings.git_enabled},
			    {"automaticWorktree", snapshot.settings.git_auto_worktree},
			    {"baseOverride", metadata.git_base_branch_override ? nlohmann::json(*metadata.git_base_branch_override) : nlohmann::json(nullptr)}};
			bool recovered = false;
			GitWorktreeOperationResult recovery;
			if (!RecoverOwnedWorkspace(root, live_chats, metadata, policy, task->receipt_file, recovered, recovery.message, cancellation->get_token()))
			{
				if (recovery.message.empty()) recovery.message = "Interrupted workspace ownership could not be verified. The workspace was preserved.";
				return PreparedLaunchWorktree{std::move(recovery), std::move(metadata)};
			}
			if (recovered)
			{
				recovery.ok = true;
				return PreparedLaunchWorktree{std::move(recovery), std::move(metadata)};
			}
			const GitWorktreeIntentRecorder record = [root, receipt, policy, id = metadata.id, created = metadata.created_at](const GitWorktreeCreationIntent& intent, std::string& error)
			{ return RecordIntent(root, receipt, id, created, policy, intent, error); };
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
