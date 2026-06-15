#include "app/git_worktree_service.h"

#include "app/persistence_coordinator.h"
#include "app/provider_resolution_service.h"
#include "common/chat/chat_repository.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_root.h"
#include "common/platform/platform_services.h"
#include "common/provider/provider_runtime.h"
#include "common/utils/hash_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/shell_escape.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

namespace uam
{
	namespace
	{
		constexpr int kDefaultGitCommandTimeoutMs = 120000;

		ProcessExecutionResult RunCommand(const std::string& command, int timeout_ms = kDefaultGitCommandTimeoutMs)
		{
			return PlatformServicesFactory::Instance().process_service.ExecuteCommand(command, timeout_ms);
		}

		std::string CommandOutputOrError(const ProcessExecutionResult& result)
		{
			std::string detail = uam::strings::Trim(result.output);
			const std::string error = uam::strings::Trim(result.error);
			if (!error.empty())
			{
				if (!detail.empty())
				{
					detail += "\n";
				}
				detail += error;
			}
			return detail;
		}

		std::string CommandOutputOrFallback(const ProcessExecutionResult& result, const std::string& fallback)
		{
			return uam::strings::NonEmptyOrFallback(CommandOutputOrError(result), fallback);
		}

		std::string BuildGitCommandInDirectory(const std::filesystem::path& cwd, const std::string& args)
		{
			return "git -C " + uam::shell::EscapeArg(cwd.string()) + " " + args;
		}

		bool CommandSucceeded(const ProcessExecutionResult& result)
		{
			return result.ok && !result.timed_out && !result.canceled && result.exit_code == 0;
		}

		bool GitCommand(const std::filesystem::path& cwd, const std::string& args, std::string* error_out = nullptr)
		{
			if (error_out != nullptr)
			{
				error_out->clear();
			}

			const ProcessExecutionResult result = RunCommand(BuildGitCommandInDirectory(cwd, args));
			if (CommandSucceeded(result))
			{
				return true;
			}

			if (error_out != nullptr)
			{
				*error_out = CommandOutputOrError(result);
			}
			return false;
		}

		bool GitOutput(const std::filesystem::path& cwd, const std::string& args, std::string* output_out, std::string* error_out = nullptr)
		{
			if (output_out != nullptr)
			{
				output_out->clear();
			}
			if (error_out != nullptr)
			{
				error_out->clear();
			}

			const ProcessExecutionResult result = RunCommand(BuildGitCommandInDirectory(cwd, args));
			if (!CommandSucceeded(result))
			{
				if (error_out != nullptr)
				{
					*error_out = CommandOutputOrError(result);
				}
				return false;
			}
			if (output_out != nullptr)
			{
				*output_out = uam::strings::Trim(result.output);
			}
			return true;
		}

		std::filesystem::path EffectiveSourceWorkspace(const AppState& app, const ChatSession& chat)
		{
			if (uam::paths::HasGitWorktreeSource(chat))
			{
				return PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(uam::strings::Trim(chat.workspace_source_directory));
			}

			ChatSession source_chat = chat;
			source_chat.workspace_isolation_kind.clear();
			source_chat.workspace_worktree_directory.clear();
			return uam::paths::ResolveWorkspaceRootPath(app, source_chat);
		}

		bool IsSvnWorkspace(const std::filesystem::path& workspace)
		{
			if (uam::paths::PathExistsNoThrow(workspace / ".svn"))
			{
				return true;
			}

			std::filesystem::path current = workspace;
			while (!current.empty() && current.has_parent_path() && current != current.parent_path())
			{
				if (uam::paths::PathExistsNoThrow(current / ".svn"))
				{
					return true;
				}
				current = current.parent_path();
			}
			return false;
		}

		std::filesystem::path WorktreeRootForChat(const AppState& app, const std::filesystem::path& source_root, const std::string& chat_id)
		{
			const std::string normalized_source_root = uam::paths::NormalizedPortablePathString(source_root);
			const std::string repo_key = uam::hashing::Hex64(uam::hashing::Fnv1a64(normalized_source_root));
			return app.data_root / "worktrees" / repo_key / chat_id;
		}

		std::string BranchNameForChat(const std::string& chat_id)
		{
			std::string safe;
			safe.reserve(chat_id.size());
			for (const unsigned char ch : chat_id)
			{
				if (uam::strings::IsAsciiAlnum(ch) || ch == '-' || ch == '_' || ch == '.')
				{
					safe.push_back(static_cast<char>(ch));
				}
				else
				{
					safe.push_back('-');
				}
			}
			return "uam/" + uam::strings::NonEmptyOrFallback(safe, "chat");
		}

		bool IsDirty(const std::filesystem::path& repo, bool* dirty_out, std::string* error_out = nullptr)
		{
			if (dirty_out != nullptr)
			{
				*dirty_out = false;
			}

			std::string status;
			if (!GitOutput(repo, "status --porcelain", &status, error_out))
			{
				return false;
			}
			if (dirty_out != nullptr)
			{
				*dirty_out = !uam::strings::IsBlank(status);
			}
			return true;
		}

		bool SaveChat(AppState& app, const ChatSession& chat, std::string* error_out)
		{
			const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
			if (!ProviderRuntime::SaveHistory(provider, app.data_root, chat))
			{
				if (error_out != nullptr)
				{
					*error_out = uam::strings::NonEmptyOrFallback(app.status_line, "Failed to persist chat worktree metadata.");
				}
				return false;
			}
			if (!PersistenceCoordinator().SaveSettings(app))
			{
				if (error_out != nullptr)
				{
					*error_out = uam::strings::NonEmptyOrFallback(app.status_line, "Failed to persist application settings.");
				}
				return false;
			}
			return true;
		}

		std::filesystem::path PatchPathForChat(const AppState& app, const std::string& chat_id)
		{
			return app.data_root / "worktrees" / "patches" / (chat_id + "-" + uam::time::SystemEpochMicrosecondsTokenNow() + ".patch");
		}

		std::filesystem::path TrimmedPathOrEmpty(std::string_view value)
		{
			const std::string_view trimmed = uam::strings::TrimAsciiView(value);
			return trimmed.empty() ? std::filesystem::path() : std::filesystem::path(std::string(trimmed));
		}

		std::filesystem::path SourcePathFromStatusOrChat(const AppState& app, const ChatSession& chat, const GitWorktreeStatus& status)
		{
			const std::filesystem::path source_directory = TrimmedPathOrEmpty(status.source_directory);
			return source_directory.empty() ? EffectiveSourceWorkspace(app, chat) : source_directory;
		}

		std::filesystem::path WorktreePathFromStatus(const GitWorktreeStatus& status)
		{
			return TrimmedPathOrEmpty(status.worktree_directory);
		}

		std::string BranchNameFromStatusOrChat(const GitWorktreeStatus& status, const ChatSession& chat)
		{
			return uam::strings::NonEmptyOrFallback(uam::strings::Trim(status.branch_name), uam::strings::Trim(chat.workspace_branch_name));
		}

		bool ClearWorktreeIsolation(AppState& app, ChatSession& chat, const GitWorktreeStatus& status, std::string* error_out)
		{
			const std::filesystem::path source = SourcePathFromStatusOrChat(app, chat, status);
			const std::filesystem::path worktree = WorktreePathFromStatus(status);

			if (!worktree.empty())
			{
				if (uam::paths::PathExistsNoThrow(worktree))
				{
					if (!GitCommand(source, "worktree remove --force " + uam::shell::EscapeArg(worktree.string()), error_out))
					{
						return false;
					}
				}
			}

			const std::string branch_name = BranchNameFromStatusOrChat(status, chat);
			if (!branch_name.empty())
			{
				GitCommand(source, "branch -D " + uam::shell::EscapeArg(branch_name));
			}

			chat.workspace_isolation_kind.clear();
			chat.workspace_source_directory.clear();
			chat.workspace_base_ref.clear();
			chat.workspace_branch_name.clear();
			chat.workspace_worktree_directory.clear();
			chat.updated_at = uam::time::TimestampNow();
			return SaveChat(app, chat, error_out);
		}

		void PopulateSourceRepositoryStatus(GitWorktreeStatus& status, const std::filesystem::path& source_candidate)
		{
			std::string repo_root;
			std::string git_error;
			if (GitOutput(source_candidate, "rev-parse --show-toplevel", &repo_root, &git_error))
			{
				status.is_git_repository = true;
				status.source_directory = repo_root;
				bool source_dirty = false;
				if (IsDirty(repo_root, &source_dirty))
				{
					status.source_dirty = source_dirty;
				}
				return;
			}

			if (!status.is_svn_workspace)
			{
				status.error = uam::strings::NonEmptyOrFallback(git_error, "Workspace is not inside a Git repository.");
			}
		}

		void PopulateIsolatedWorktreeStatus(GitWorktreeStatus& status)
		{
			if (!status.isolated)
			{
				return;
			}

			const std::filesystem::path worktree_path(status.worktree_directory);
			status.worktree_missing = status.worktree_directory.empty() || !uam::paths::PathExistsNoThrow(worktree_path);
			if (status.worktree_missing)
			{
				return;
			}

			bool worktree_dirty = false;
			if (IsDirty(worktree_path, &worktree_dirty))
			{
				status.worktree_dirty = worktree_dirty;
			}
		}

		void CompleteSuccessfulResult(GitWorktreeOperationResult& result, const GitWorktreeStatus& status, const std::string& message)
		{
			result.ok = true;
			result.message = message;
			result.status = status;
		}
	} // namespace

	GitWorktreeStatus GitWorktreeService::Status(const AppState& app, const ChatSession& chat) const
	{
		GitWorktreeStatus status;
		status.isolated = uam::paths::IsGitWorktreeIsolated(chat);
		status.source_directory = uam::strings::Trim(chat.workspace_source_directory);
		status.worktree_directory = uam::strings::Trim(chat.workspace_worktree_directory);
		status.branch_name = uam::strings::Trim(chat.workspace_branch_name);
		status.base_ref = uam::strings::Trim(chat.workspace_base_ref);

		const std::filesystem::path source_candidate = EffectiveSourceWorkspace(app, chat);
		status.is_svn_workspace = IsSvnWorkspace(source_candidate);
		PopulateSourceRepositoryStatus(status, source_candidate);

		if (status.is_svn_workspace && !status.is_git_repository)
		{
			status.error = "SVN workspaces are not supported by Git worktree isolation in this release.";
		}

		PopulateIsolatedWorktreeStatus(status);
		return status;
	}

	GitWorktreeOperationResult GitWorktreeService::CreateForChat(AppState& app, ChatSession& chat) const
	{
		GitWorktreeOperationResult result;
		result.status = Status(app, chat);
		if (result.status.isolated)
		{
			CompleteSuccessfulResult(result, result.status, "Chat already has an isolated Git worktree.");
			return result;
		}
		if (!result.status.is_git_repository)
		{
			result.message = uam::strings::NonEmptyOrFallback(result.status.error, "Workspace is not inside a Git repository.");
			return result;
		}

		const std::filesystem::path source_root(result.status.source_directory);
		std::string head;
		if (!GitOutput(source_root, "rev-parse HEAD", &head, &result.message))
		{
			return result;
		}

		const std::filesystem::path worktree_root = WorktreeRootForChat(app, source_root, chat.id);
		if (uam::paths::PathExistsNoThrow(worktree_root))
		{
			result.message = "Worktree path already exists: " + worktree_root.string();
			return result;
		}
		std::error_code ec;
		if (!uam::paths::CreateDirectoriesNoThrow(worktree_root.parent_path(), &ec))
		{
			result.message = "Failed to create worktree parent directory.";
			return result;
		}

		const std::string branch_name = BranchNameForChat(chat.id);
		const ProcessExecutionResult add_result = RunCommand(BuildGitCommandInDirectory(source_root, "worktree add -b " + uam::shell::EscapeArg(branch_name) + " " + uam::shell::EscapeArg(worktree_root.string()) + " HEAD"));
		if (!CommandSucceeded(add_result))
		{
			result.message = CommandOutputOrFallback(add_result, "Failed to create Git worktree.");
			return result;
		}

		chat.workspace_isolation_kind = uam::paths::kGitWorktreeIsolationKind;
		chat.workspace_source_directory = source_root.string();
		chat.workspace_base_ref = head;
		chat.workspace_branch_name = branch_name;
		chat.workspace_worktree_directory = worktree_root.string();
		chat.updated_at = uam::time::TimestampNow();

		std::string save_error;
		if (!SaveChat(app, chat, &save_error))
		{
			result.message = save_error;
			return result;
		}

		const std::string message = result.status.source_dirty ? "Created isolated Git worktree from HEAD. Source workspace has uncommitted changes that were not copied." : "Created isolated Git worktree.";
		CompleteSuccessfulResult(result, Status(app, chat), message);
		return result;
	}

	GitWorktreeOperationResult GitWorktreeService::DiscardChatChanges(AppState& app, ChatSession& chat) const
	{
		GitWorktreeOperationResult result;
		result.status = Status(app, chat);
		if (!result.status.isolated)
		{
			result.message = "Chat does not have an isolated Git worktree.";
			return result;
		}

		if (!result.status.worktree_missing)
		{
			const std::filesystem::path worktree(result.status.worktree_directory);
			if (!GitCommand(worktree, "reset --hard HEAD", &result.message))
			{
				return result;
			}

			if (!GitCommand(worktree, "clean -fd", &result.message))
			{
				return result;
			}
		}

		std::string save_error;
		if (!ClearWorktreeIsolation(app, chat, result.status, &save_error))
		{
			result.message = save_error;
			return result;
		}

		CompleteSuccessfulResult(result, Status(app, chat), "Discarded chat worktree changes and returned to the source workspace.");
		return result;
	}

	GitWorktreeOperationResult GitWorktreeService::PortChatChanges(AppState& app, ChatSession& chat) const
	{
		GitWorktreeOperationResult result;
		result.status = Status(app, chat);
		if (!result.status.isolated || result.status.worktree_missing)
		{
			result.message = "Chat does not have an available isolated Git worktree.";
			return result;
		}

		const std::filesystem::path source(result.status.source_directory);
		const std::filesystem::path worktree(result.status.worktree_directory);
		bool source_dirty = false;
		if (!IsDirty(source, &source_dirty, &result.message))
		{
			return result;
		}
		if (source_dirty)
		{
			result.message = "Source workspace has uncommitted changes. Commit, stash, or discard them before porting chat changes back.";
			return result;
		}

		if (!GitCommand(worktree, "add -A", &result.message))
		{
			return result;
		}

		ProcessExecutionResult diff_result = RunCommand(BuildGitCommandInDirectory(worktree, "diff --binary --cached HEAD"));
		if (!CommandSucceeded(diff_result))
		{
			result.message = CommandOutputOrError(diff_result);
			return result;
		}
		if (!GitCommand(worktree, "reset --mixed HEAD", &result.message))
		{
			return result;
		}

		const std::string patch = diff_result.output;
		if (uam::strings::IsBlank(patch))
		{
			std::string save_error;
			if (!ClearWorktreeIsolation(app, chat, result.status, &save_error))
			{
				result.message = save_error;
				return result;
			}
			CompleteSuccessfulResult(result, Status(app, chat), "No chat worktree changes to port. Returned to the source workspace.");
			return result;
		}

		const std::filesystem::path patch_path = PatchPathForChat(app, chat.id);
		if (!uam::io::WriteTextFile(patch_path, patch))
		{
			result.message = "Failed to write patch file.";
			return result;
		}

		ProcessExecutionResult apply_result = RunCommand(BuildGitCommandInDirectory(source, "apply --3way " + uam::shell::EscapeArg(patch_path.string())));
		if (!CommandSucceeded(apply_result))
		{
			result.patch_path = patch_path;
			result.message = CommandOutputOrFallback(apply_result, "Failed to apply patch to source workspace.");
			return result;
		}

		std::string save_error;
		if (!ClearWorktreeIsolation(app, chat, result.status, &save_error))
		{
			result.message = save_error;
			return result;
		}

		result.patch_path = patch_path;
		CompleteSuccessfulResult(result, Status(app, chat), "Applied chat worktree changes and returned to the source workspace.");
		return result;
	}
} // namespace uam
