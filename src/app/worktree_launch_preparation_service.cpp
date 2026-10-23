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
#include "remote/runner_client.h"

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
			if (task.remote_host)
			{
				const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
				if (!host || !execution_hosts::SameConnection(*host, *task.remote_host) || !remote::CanAttemptRunnerConnection(*host)) return false;
			}
			if (app.data_root != task.data_root || chat.id != task.original.id || chat.created_at != task.original.created_at ||
			    chat.metadata_incarnation != task.original.metadata_incarnation ||
			    chat.provider_id != task.original.provider_id || chat.execution_host_id != task.original.execution_host_id ||
			    chat.native_session_id != task.original.native_session_id || chat.routing_mode != task.original.routing_mode || chat.imported_read_only != task.original.imported_read_only ||
			    chat.agent_run_id != task.original.agent_run_id || chat.goal_owner_chat_id != task.original.goal_owner_chat_id ||
			    chat.temporary_parent_chat_id != task.original.temporary_parent_chat_id ||
			    chat.workspace_directory != task.original.workspace_directory || chat.folder_id != task.original.folder_id) return false;
			if ((!task.remote_host && (app.settings.git_snapshot_svn != task.snapshot_svn || app.settings.git_snapshot_plain_folders != task.snapshot_plain_folders)) ||
			    app.settings.git_enabled != task.git_enabled || app.settings.git_auto_worktree != task.automatic_worktree ||
			    app.settings.git_default_base_branch != task.base_branch || app.settings.git_pull_before_chat != task.pull_before_chat ||
			    app.settings.svn_enabled != task.svn_enabled || app.settings.svn_update_before_chat != task.svn_update_before_chat) return false;
			if (task.merged)
			{
				if (!task.prepared || !WorkspaceFieldsMatch(chat, task.prepared->metadata)) return false;
				if (task.remote_host)
				{
					ChatSession source = CaptureVcsChatContext(chat);
					source.workspace_isolation_kind.clear();
					source.workspace_worktree_directory.clear();
					if (!execution_hosts::RemotePathsMatch(task.remote_host->platform,
					        paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, source)), task.prepared->metadata.workspace_directory)) return false;
				}
				return true;
			}
			return WorkspaceFieldsMatch(chat, task.original) && (task.update_only || (app.settings.git_enabled && (app.settings.git_auto_worktree || task.explicit_create))) &&
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

		bool ReferencesPreparedWorkspace(const ChatSession& other, const ChatSession& prepared, std::string_view remote_platform = {})
		{
			if (!paths::IsGitWorktreeIsolated(prepared) || prepared.workspace_worktree_directory.empty()) return false;
			if (other.id == prepared.id) return false;
			if (!paths::IsControllerLocalWorkspace(prepared))
			{
				if (other.execution_host_id != prepared.execution_host_id) return false;
				const std::string worktree = execution_hosts::NormalizeRemotePath(remote_platform, prepared.workspace_worktree_directory);
				for (const std::string* path : {&other.workspace_directory, &other.workspace_worktree_directory, &other.workspace_source_directory})
				{
					const std::string reference = execution_hosts::NormalizeRemotePath(remote_platform, *path);
					if (!reference.empty() && (reference == worktree || reference.starts_with(worktree + "/"))) return true;
				}
				return false;
			}
			if (!paths::IsControllerLocalWorkspace(other)) return false;
			const std::filesystem::path worktree = paths::PathFromUtf8(prepared.workspace_worktree_directory);
			for (const std::string* path : {&other.workspace_directory, &other.workspace_worktree_directory, &other.workspace_source_directory})
				if (!path->empty() && paths::IsSameOrInsideRoot(worktree, paths::PathFromUtf8(*path))) return true;
			return false;
		}

		bool SnapshotLaunchPoliciesMatch(nlohmann::json saved, const nlohmann::json& expected)
		{
			// Receipts predating these controls used the enabled defaults.
			if (saved.is_object())
			{
				if (!saved.contains("snapshotSvn")) saved["snapshotSvn"] = true;
				if (!saved.contains("snapshotPlainFolders")) saved["snapshotPlainFolders"] = true;
			}
			return saved == expected;
		}

		bool RecoverOwnedWorkspace(const std::filesystem::path& root, const std::vector<ChatSession>& live_chats,
		    ChatSession& metadata, const nlohmann::json& policy, std::filesystem::path& receipt, bool& found, std::shared_ptr<const platform::PublishedFileChangeWitness>& resumed_intent, std::string& error, std::stop_token stop)
		{
			const std::filesystem::path directory = root / ".worktree-launch";
			std::error_code scan_error;
			if (!std::filesystem::exists(directory, scan_error)) return !scan_error;
			if (paths::IsLinkOrReparsePointNoThrow(directory)) return false;
			const std::string prefix = hashing::Hex64(hashing::Fnv1a64(metadata.id)) + "-";
			try
			{
				std::optional<std::filesystem::directory_entry> matching;
				for (const auto& entry : std::filesystem::directory_iterator(directory))
				{
					if (!entry.path().filename().string().starts_with(prefix) || entry.path().extension() != ".json") continue;
					// Reject the second match immediately, before reading or progressing either journal.
					if (matching) return false;
					matching = entry;
				}
				if (!matching) return true;
				const std::filesystem::directory_entry& entry = *matching;
				{
					if (found || paths::IsLinkOrReparsePointNoThrow(entry.path())) return false;
					found = true;
					std::string receipt_text;
					if (!io::TryReadTextFile(entry.path(), receipt_text, 64 * 1024))
					{
						error = "Interrupted workspace ownership receipt could not be read within its size limit. The workspace was preserved.";
						return false;
					}
					const auto witness = platform::PublishedFileChangeWitness::Capture(entry.path());
					const auto held_bytes = witness ? witness->ReadBounded(64 * 1024, stop) : std::nullopt;
					if (!held_bytes || *held_bytes != receipt_text) return false;
					const nlohmann::json record = nlohmann::json::parse(receipt_text);
					if (record.at("chatId") != metadata.id || record.at("createdAt") != metadata.created_at ||
					    !SnapshotLaunchPoliciesMatch(record.at("launchPolicy"), policy) ||
					    !paths::IsSameOrInsideRoot(paths::PathFromUtf8(record.at("source").get<std::string>()), paths::PathFromUtf8(metadata.workspace_directory))) return false;
					const std::string branch = record.at("branch").get<std::string>();
					const std::string base = record.at("baseSha").get<std::string>();
					if (branch.empty() && base.empty())
					{
						if (!GitWorktreeService().ValidateUncreatedWorkspaceIntent(root, entry.path(), metadata, error, stop)) return false;
						ChatSession planned = metadata;
						planned.workspace_isolation_kind = paths::kGitWorktreeIsolationKind;
						planned.workspace_worktree_directory = record.at("worktree").get<std::string>();
						const auto references = [&](const ChatSession& other) { return ReferencesPreparedWorkspace(other, planned); };
						const auto persisted = ChatRepository::LoadLocalChatSummariesReadOnly(root);
						if (std::any_of(live_chats.begin(), live_chats.end(), references) || std::any_of(persisted.begin(), persisted.end(), references) ||
						    !witness->Current() || stop.stop_requested()) return false;
						// Resume this exact owner's journal; no artifact or receipt is removed or accepted as ready.
						receipt = entry.path();
						resumed_intent = witness;
						found = false;
						return true;
					}
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

		bool ArchiveFailedRemoteReceipt(const std::filesystem::path& root, const std::filesystem::path& receipt, std::string& error)
		{
			const std::filesystem::path archived = paths::PathFromUtf8(paths::Utf8PathString(receipt) + ".failed");
			if (!paths::IsSameOrInsideRoot(root, receipt) || paths::IsLinkOrReparsePointNoThrow(receipt) ||
			    paths::IsLinkOrReparsePointNoThrow(receipt.parent_path()) || paths::PathExistsNoThrow(archived))
			{ error = "The failed SSH worktree receipt could not be safely archived."; return false; }
			std::error_code ec;
			std::filesystem::rename(receipt, archived, ec);
			if (ec) { error = "Could not archive the failed SSH worktree receipt. Its files were preserved."; return false; }
			return true;
		}

		bool RemoteRecordIntent(const std::filesystem::path& root, const std::filesystem::path& receipt,
		    const ChatSession& chat, const ExecutionHost& host, const nlohmann::json& policy,
		    const GitWorktreeCreationIntent& intent, std::string& error)
		{
			if (paths::IsLinkOrReparsePointNoThrow(root) || paths::IsLinkOrReparsePointNoThrow(receipt.parent_path()) ||
			    paths::IsLinkOrReparsePointNoThrow(receipt) || !paths::IsSameOrInsideRoot(root, receipt))
			{ error = "SSH worktree ownership storage is unsafe."; return false; }
			const nlohmann::json record = {{"chatId", chat.id}, {"createdAt", chat.created_at}, {"remoteHost", RemoteWorktreeHostIdentity(host)},
			    {"stableHost", RemoteWorktreeStableHostIdentity(host)}, {"operationState", intent.operation_state},
			    {"requestedSource", policy.at("requestedSource")}, {"source", paths::Utf8PathString(intent.source)},
			    {"repository", paths::Utf8PathString(intent.repository)}, {"worktree", paths::Utf8PathString(intent.worktree)},
			    {"branch", intent.branch}, {"baseSha", intent.base_sha}, {"operationId", intent.operation_id},
			    {"controlToken", intent.control_token}, {"launchPolicy", policy}};
			if (!io::WriteTextFile(receipt, record.dump()))
			{ error = "Could not save SSH worktree ownership before creation."; return false; }
			// The durable failed state permits process cleanup even if archival must be retried.
			if (intent.operation_state == "failed") (void)ArchiveFailedRemoteReceipt(root, receipt, error);
			return true;
		}

		GitWorktreeOperationResult PrepareRemoteLaunch(AppState& snapshot, ChatSession& metadata,
		    const std::shared_ptr<WorktreeLaunchPreparationTask>& task)
		{
			GitWorktreeOperationResult result;
			const ExecutionHost& host = *task->remote_host;
			const nlohmann::json policy = {{"version", 1}, {"requestedSource", metadata.workspace_directory},
			    {"baseBranch", task->base_branch}, {"pullBeforeChat", task->pull_before_chat}, {"gitEnabled", task->git_enabled},
			    {"automaticWorktree", task->automatic_worktree}, {"explicitCreate", task->explicit_create},
			    {"baseOverride", metadata.git_base_branch_override ? nlohmann::json(*metadata.git_base_branch_override) : nlohmann::json(nullptr)}};
			const std::filesystem::path directory = task->data_root / ".worktree-launch";
			const std::string prefix = hashing::Hex64(hashing::Fnv1a64(metadata.id)) + "-";
			bool found = false;
			std::error_code ec;
			if (paths::IsLinkOrReparsePointNoThrow(directory)) { result.message = "SSH worktree ownership storage is unsafe."; return result; }
			if (std::filesystem::exists(directory, ec))
			{
				for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory))
				{
					if (!entry.path().filename().string().starts_with(prefix) || entry.path().extension() != ".json") continue;
					std::string bytes;
					if (paths::IsLinkOrReparsePointNoThrow(entry.path()) || !io::TryReadTextFile(entry.path(), bytes, 64 * 1024))
					{ result.message = "The SSH worktree ownership receipt is ambiguous or unreadable."; return result; }
					nlohmann::json saved = nlohmann::json::parse(bytes);
					if (saved.at("chatId") != metadata.id || saved.at("createdAt") != metadata.created_at)
					{ result.message = "The saved SSH worktree belongs to a different chat, host or creation policy."; return result; }
					if (saved.value("operationState", "pending") == "failed")
					{
						if (!ArchiveFailedRemoteReceipt(task->data_root, entry.path(), result.message)) return result;
						continue;
					}
					if (found || saved.at("stableHost") != RemoteWorktreeStableHostIdentity(host) || saved.at("requestedSource") != metadata.workspace_directory)
					{ result.message = "The saved SSH worktree belongs to a different chat, host or creation policy, or is ambiguous."; return result; }
					if (!paths::IsGitWorktreeIsolated(metadata) && saved.at("launchPolicy") != policy)
					{ result.message = "The saved SSH worktree belongs to a different creation policy. Its files were preserved."; return result; }
					found = true;
					if (paths::IsGitWorktreeIsolated(metadata) && (saved.at("source") != metadata.workspace_source_directory ||
					    saved.at("worktree") != metadata.workspace_worktree_directory || saved.at("branch") != metadata.workspace_branch_name || saved.at("baseSha") != metadata.workspace_base_ref))
					{ result.message = "The saved SSH worktree metadata changed."; return result; }
					metadata.workspace_isolation_kind = paths::kGitWorktreeIsolationKind;
					metadata.workspace_source_directory = saved.at("source").get<std::string>();
					metadata.workspace_worktree_directory = saved.at("worktree").get<std::string>();
					metadata.workspace_branch_name = saved.at("branch").get<std::string>();
					metadata.workspace_base_ref = saved.at("baseSha").get<std::string>();
					task->receipt_file = entry.path();
					bool definite_failure = false;
					if (!GitWorktreeService().ValidateRemotePreparedWorkspace(host, task->data_root, entry.path(), metadata, result.message, task->cancellation->get_token(), &definite_failure))
					{
						if (definite_failure)
						{
							std::string receipt_error;
							(void)ArchiveFailedRemoteReceipt(task->data_root, entry.path(), receipt_error);
							if (!receipt_error.empty()) result.message += " " + receipt_error;
						}
						return result;
					}
				}
			}
			if (ec) { result.message = "The SSH worktree ownership storage could not be inspected."; return result; }
			if (found)
			{
				metadata.updated_at = time::TimestampNow();
				result.ok = true;
				result.message = "Verified the saved SSH Git worktree.";
				return result;
			}
			if (paths::IsGitWorktreeIsolated(metadata))
			{ result.message = "The isolated SSH worktree has no owned receipt. Its files were preserved."; return result; }
			const GitWorktreeIntentRecorder record = [task, policy, metadata](const GitWorktreeCreationIntent& intent, std::string& error)
			{ return RemoteRecordIntent(task->data_root, task->receipt_file, metadata, *task->remote_host, policy, intent, error); };
			return GitWorktreeService().PrepareForChat(snapshot, metadata, record, task->cancellation->get_token(), task->explicit_create);
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
			if (task->remote_host)
			{
				std::weak_ptr<void>& activity = app.remote_vcs_operation_leases_by_host_id[chat.execution_host_id];
				task->remote_host_lease = activity.lock();
				if (!task->remote_host_lease) { task->remote_host_lease = std::make_shared<int>(0); activity = task->remote_host_lease; }
			}
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
			std::packaged_task<PreparedLaunchWorktree()> validation([root = task->data_root, receipt = task->receipt_file, prepared = std::move(prepared), cancellation = task->cancellation, update_only = task->update_only, svn_update = task->svn_enabled && task->svn_update_before_chat && task->original.svn_update_pending, lease, host = task->remote_host]() mutable
			{
				prepared.operation.ok = host
				    ? GitWorktreeService().ValidateRemotePreparedWorkspace(*host, root, receipt, prepared.metadata, prepared.operation.message, cancellation->get_token())
				    : update_only
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
				task->remote_host_lease.reset();
				app.worktree_operation_chat_ids.erase(chat.id);
				task->retry_save_validation = false;
				task->error = "Could not validate the prepared workspace.";
			}
			return {WorktreeLaunchPreparationState::Pending, {}, task->cancellation};
		}

	}

	bool IsWorktreeLaunchPreparationPending(const AppState& app, const std::string& chat_id)
	{
		const std::unordered_map<std::string, std::shared_ptr<WorktreeLaunchPreparationTask>>::const_iterator found = app.worktree_launch_preparations.find(chat_id);
		return found != app.worktree_launch_preparations.end() &&
		    !(found->second->saved.value_or(false) && found->second->error.empty() && !found->second->cancellation->stop_requested());
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
				task->remote_host_lease.reset();
				}
				++iterator;
				continue;
			}
			if (owned && task->cancellation->stop_requested() && !task->merged && task->prepared && task->prepared->operation.ok)
			{
				// Keep the receipt and result for an explicit retry; cancellation cannot publish metadata.
				task->lease.reset();
				task->remote_host_lease.reset();
				app.worktree_operation_chat_ids.erase(iterator->first);
				++iterator;
				continue;
			}
			if (!owned || (task->cancellation->stop_requested() && !task->merged))
			{
				// The worker's durable intent retains artifact locations; cancellation never removes branches or directories.
				task->lease.reset();
				task->remote_host_lease.reset();
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
				if (task->prepared->operation.ok && (task->remote_host || !task->update_only) && std::any_of(app.chats.begin(), app.chats.end(),
				    [&](const ChatSession& other) { return ReferencesPreparedWorkspace(other, task->prepared->metadata, task->remote_host ? std::string_view(task->remote_host->platform) : std::string_view{}); }))
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
				task->remote_host_lease.reset();
				app.worktree_operation_chat_ids.erase(iterator->first);
			}
			++iterator;
		}
		return changed;
	}

	WorktreeLaunchPreparationResult PrepareWorktreeForLaunch(AppState& app, ChatSession& chat, bool explicit_create)
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
			if (task->saved && *task->saved && task->error.empty())
			{
				if (explicit_create && task->remote_host && !paths::IsGitWorktreeIsolated(chat))
				{
					// An automatic plain-folder result cannot satisfy an explicit isolation request.
					app.worktree_launch_preparations.erase(found);
					return PrepareWorktreeForLaunch(app, chat, true);
				}
				const std::shared_ptr<std::stop_source> cancellation = task->cancellation;
				// Local tasks retire here, so publish the warning once, on the owner after durable success.
				if (!task->remote_host && task->prepared && !task->prepared->operation.status.warning.empty())
					app.status_line = task->prepared->operation.status.warning;
				if (!task->remote_host) app.worktree_launch_preparations.erase(found);
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
					if (!task->remote_host && (task->snapshot_svn != app.settings.git_snapshot_svn || task->snapshot_plain_folders != app.settings.git_snapshot_plain_folders))
						return {WorktreeLaunchPreparationState::Failed, "Managed Git snapshot policy changed before publication. Its files were preserved.", task->cancellation};
					task->git_enabled = app.settings.git_enabled;
					task->automatic_worktree = app.settings.git_auto_worktree;
					task->snapshot_svn = app.settings.git_snapshot_svn;
					task->snapshot_plain_folders = app.settings.git_snapshot_plain_folders;
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
		if (app.pending_chat_save_at_by_chat_id.contains(chat.id) &&
		    (chat.workspace_isolation_kind == paths::kPendingGitWorktreeIsolationKind || paths::IsGitWorktreeIsolated(chat)))
			return {WorktreeLaunchPreparationState::Pending, {}, {}};
		const bool remote_workspace = !paths::IsControllerLocalWorkspace(chat);
		if (remote_workspace && explicit_create && !app.settings.git_enabled)
			return {WorktreeLaunchPreparationState::Failed, "Git is disabled in Settings.", {}};
		if ((chat.workspace_isolation_kind != paths::kPendingGitWorktreeIsolationKind && !chat.svn_update_pending &&
		     !(remote_workspace && (explicit_create || paths::IsGitWorktreeIsolated(chat)))) ||
		    chat.imported_read_only || !chat.agent_run_id.empty() || !chat.goal_owner_chat_id.empty() || !chat.temporary_parent_chat_id.empty()) return {};
		const bool snapshot_disabled = !chat.git_base_branch_override && !ManagedSnapshotDisabledReason(app, chat).empty();
		if (snapshot_disabled)
		{
			const std::string error = explicit_create ? ManagedSnapshotDisabledReason(app, chat) : ManagedSnapshotBypassError(app, chat);
			if (!error.empty()) return {WorktreeLaunchPreparationState::Failed, error, {}};
		}
		if ((!app.settings.git_enabled || (!app.settings.git_auto_worktree && !explicit_create)) && !chat.svn_update_pending && !(remote_workspace && paths::IsGitWorktreeIsolated(chat)))
		{
			chat.workspace_isolation_kind.clear();
			return {};
		}

		if (snapshot_disabled && !chat.svn_update_pending)
		{
			if (chat.workspace_isolation_kind == paths::kPendingGitWorktreeIsolationKind) chat.workspace_isolation_kind.clear();
			return {};
		}
		if (app.worktree_operation_chat_ids.contains(chat.id)) return {WorktreeLaunchPreparationState::Failed, "Wait for the chat's workspace operation to finish.", {}};
		if (app.data_root.empty()) return {WorktreeLaunchPreparationState::Failed, "Chat storage is unavailable.", {}};
		const ExecutionHost* remote_host = remote_workspace ? execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id) : nullptr;
		if (remote_workspace && (!remote_host || remote_host->runner_protocol_version < 3 || !remote::CanAttemptRunnerConnection(*remote_host)))
			return {WorktreeLaunchPreparationState::Failed, "SSH worktrees require an available protocol-3 helper.", {}};
		if (remote_workspace && chat.svn_update_pending)
			return {WorktreeLaunchPreparationState::Failed, "SSH worktree preparation does not support SVN.", {}};
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
		task->snapshot_svn = app.settings.git_snapshot_svn;
		task->snapshot_plain_folders = app.settings.git_snapshot_plain_folders;
		task->svn_enabled = app.settings.svn_enabled;
		task->svn_update_before_chat = app.settings.svn_update_before_chat;
		task->update_only = snapshot_disabled || chat.workspace_isolation_kind != paths::kPendingGitWorktreeIsolationKind || !task->git_enabled || !task->automatic_worktree;
		task->explicit_create = remote_workspace && explicit_create;
		if (remote_host)
		{
			task->remote_host = *remote_host;
			std::weak_ptr<void>& activity = app.remote_vcs_operation_leases_by_host_id[chat.execution_host_id];
			task->remote_host_lease = activity.lock();
			if (!task->remote_host_lease) { task->remote_host_lease = std::make_shared<int>(0); activity = task->remote_host_lease; }
		}
		task->lease = lease;
		task->receipt_file = task->data_root / ".worktree-launch" / (hashing::Hex64(hashing::Fnv1a64(chat.id)) + "-" + time::SteadyEpochNanosecondsTokenNow() + ".json");
		AppSettings settings;
		settings.git_enabled = app.settings.git_enabled;
		settings.svn_enabled = app.settings.svn_enabled;
		settings.svn_update_before_chat = app.settings.svn_update_before_chat;
		settings.git_auto_worktree = app.settings.git_auto_worktree;
		settings.git_snapshot_svn = app.settings.git_snapshot_svn;
		settings.git_snapshot_plain_folders = app.settings.git_snapshot_plain_folders;
		settings.git_default_base_branch = app.settings.git_default_base_branch;
		settings.git_pull_before_chat = app.settings.git_pull_before_chat;
		if (remote_host) settings.execution_hosts = {*remote_host};
		ChatSession metadata = task->original;
		metadata.workspace_directory = task->resolved_source;
		if (remote_workspace && paths::IsGitWorktreeIsolated(chat))
		{
			ChatSession source = task->original;
			source.workspace_isolation_kind.clear();
			source.workspace_worktree_directory.clear();
			metadata.workspace_directory = paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, source));
		}
		std::vector<ChatSession> live_chats;
		live_chats.reserve(app.chats.size());
		for (const ChatSession& other : app.chats) live_chats.push_back(CaptureVcsChatContext(other));
		std::packaged_task<PreparedLaunchWorktree()> worker([task, live_chats = std::move(live_chats), root = task->data_root, settings = std::move(settings), metadata = std::move(metadata), cancellation = task->cancellation, lease]() mutable
		{
			AppState snapshot;
			snapshot.data_root = root;
			snapshot.settings = std::move(settings);
			if (task->remote_host)
			{
				GitWorktreeOperationResult operation = PrepareRemoteLaunch(snapshot, metadata, task);
				const std::vector<ChatSession> persisted = operation.ok ? ChatRepository::LoadLocalChatSummariesReadOnly(root) : std::vector<ChatSession>{};
				const auto referenced = [&metadata, task](const ChatSession& other)
				{ return ReferencesPreparedWorkspace(other, metadata, task->remote_host->platform); };
				if (operation.ok && (std::ranges::any_of(live_chats, referenced) || std::ranges::any_of(persisted, referenced)))
				{ operation.ok = false; operation.message = "Another chat references the SSH worktree. Its files were preserved."; }
				return PreparedLaunchWorktree{std::move(operation), std::move(metadata)};
			}
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
			    {"snapshotSvn", snapshot.settings.git_snapshot_svn}, {"snapshotPlainFolders", snapshot.settings.git_snapshot_plain_folders},
			    {"baseOverride", metadata.git_base_branch_override ? nlohmann::json(*metadata.git_base_branch_override) : nlohmann::json(nullptr)}};
			bool recovered = false;
			std::shared_ptr<const platform::PublishedFileChangeWitness> resumed_intent;
			GitWorktreeOperationResult recovery;
			if (!RecoverOwnedWorkspace(root, live_chats, metadata, policy, task->receipt_file, recovered, resumed_intent, recovery.message, cancellation->get_token()))
			{
				if (recovery.message.empty()) recovery.message = "Interrupted workspace ownership could not be verified. The workspace was preserved.";
				return PreparedLaunchWorktree{std::move(recovery), std::move(metadata)};
			}
			if (recovered)
			{
				recovery.ok = true;
				return PreparedLaunchWorktree{std::move(recovery), std::move(metadata)};
			}
			const GitWorktreeIntentRecorder record = [root, receipt = task->receipt_file, policy, id = metadata.id, created = metadata.created_at, witness = std::move(resumed_intent)](const GitWorktreeCreationIntent& intent, std::string& error) mutable
			{
				if (witness && !witness->Current()) { error = "Workspace ownership journal changed before preparation. Its files were preserved."; return false; }
				if (!RecordIntent(root, receipt, id, created, policy, intent, error)) return false;
				witness = platform::PublishedFileChangeWitness::Capture(receipt);
				if (!witness) { error = "Workspace ownership journal could not be retained after preparation."; return false; }
				return true;
			};
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
