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

#include <fstream>
#include <algorithm>
#include <sstream>
#include <vector>

namespace uam
{
	namespace
	{
		constexpr int kDefaultGitCommandTimeoutMs = 120000;
		constexpr std::uintmax_t kMaxManagedSnapshotBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
		constexpr std::size_t kMaxManagedSnapshotFiles = 100000;

		struct SnapshotFile
		{
			std::filesystem::path source;
			std::filesystem::path relative;
			std::uintmax_t size = 0;
			std::filesystem::file_time_type modified_at;
		};

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

		std::filesystem::path ManagedRepositoryRoot(const AppState& app, const std::filesystem::path& source_root, const std::string& chat_id)
		{
			const std::string normalized_source_root = uam::paths::NormalizedPortablePathString(source_root);
			const std::string repo_key = uam::hashing::Hex64(uam::hashing::Fnv1a64(normalized_source_root));
			return app.data_root / "worktrees" / repo_key / ("managed-repository-" + uam::hashing::Hex64(uam::hashing::Fnv1a64(chat_id)));
		}

		bool PathIsWithin(const std::filesystem::path& candidate, const std::filesystem::path& parent)
		{
			const std::filesystem::path normalized_candidate = uam::paths::AbsolutePathNoThrow(candidate).lexically_normal();
			const std::filesystem::path normalized_parent = uam::paths::AbsolutePathNoThrow(parent).lexically_normal();
			auto candidate_part = normalized_candidate.begin();
			for (auto parent_part = normalized_parent.begin(); parent_part != normalized_parent.end(); ++parent_part, ++candidate_part)
			{
				if (candidate_part == normalized_candidate.end() || *candidate_part != *parent_part)
				{
					return false;
				}
			}
			return true;
		}

		bool IsExcludedSnapshotPath(const std::filesystem::path& relative)
		{
			for (const std::filesystem::path& part : relative)
			{
				const std::string name = uam::strings::ToLowerAscii(part.string());
				if (name == ".git" || name == ".svn" || name == ".hg" || name == ".uam")
				{
					return true;
				}
			}
			return false;
		}

		bool HasUnsafeSnapshotName(const std::filesystem::path& relative)
		{
			const std::string value = relative.generic_string();
			return value.empty() || relative.is_absolute() || value == ".." || value.starts_with("../") || value.find('\n') != std::string::npos || value.find('\r') != std::string::npos || value.find('\0') != std::string::npos;
		}

		bool CollectSnapshotFiles(const AppState& app, const std::filesystem::path& source_root, std::vector<SnapshotFile>& files, std::string& error)
		{
			files.clear();
			error.clear();
			if (!uam::paths::IsDirectoryNoThrow(source_root))
			{
				error = "Source workspace is not an available directory.";
				return false;
			}
			if (!app.data_root.empty() && PathIsWithin(source_root, app.data_root))
			{
				error = "Source workspace cannot be inside UAM's managed data directory.";
				return false;
			}

			std::uintmax_t total_bytes = 0;
			std::error_code ec;
			std::filesystem::recursive_directory_iterator it(source_root, ec);
			const std::filesystem::recursive_directory_iterator end;
			if (ec)
			{
				error = "Failed to enumerate source workspace: " + ec.message();
				return false;
			}
			for (; it != end; it.increment(ec))
			{
				if (ec)
				{
					error = "Failed to enumerate source workspace: " + ec.message();
					return false;
				}
				const std::filesystem::path path = it->path();
				const std::filesystem::path relative = std::filesystem::relative(path, source_root, ec);
				if (ec || HasUnsafeSnapshotName(relative))
				{
					error = "Source workspace contains an unsafe path.";
					return false;
				}
				if (IsExcludedSnapshotPath(relative) || (!app.data_root.empty() && PathIsWithin(path, app.data_root)))
				{
					if (it->is_directory(ec))
					{
						it.disable_recursion_pending();
					}
					ec.clear();
					continue;
				}

				const std::filesystem::file_status status = it->symlink_status(ec);
				if (ec)
				{
					error = "Failed to inspect source path " + relative.generic_string() + ": " + ec.message();
					return false;
				}
				if (std::filesystem::is_symlink(status))
				{
					error = "Source workspace contains a symbolic link that cannot be snapshotted safely: " + relative.generic_string();
					return false;
				}
				if (std::filesystem::is_directory(status))
				{
					continue;
				}
				if (!std::filesystem::is_regular_file(status))
				{
					error = "Source workspace contains an unsupported file type: " + relative.generic_string();
					return false;
				}

				const std::uintmax_t size = std::filesystem::file_size(path, ec);
				const std::filesystem::file_time_type modified_at = std::filesystem::last_write_time(path, ec);
				std::ifstream readable(path, std::ios::binary);
				if (ec || !readable)
				{
					error = "Source file is unreadable: " + relative.generic_string();
					return false;
				}
				total_bytes += size;
				if (files.size() >= kMaxManagedSnapshotFiles || total_bytes > kMaxManagedSnapshotBytes)
				{
					error = "Source workspace exceeds the managed snapshot limit (100000 files or 2 GiB).";
					return false;
				}
				files.push_back({path, relative, size, modified_at});
			}
			return true;
		}

		bool CopySnapshotFiles(const std::vector<SnapshotFile>& files, const std::filesystem::path& repository, std::string& error)
		{
			std::error_code ec;
			for (const SnapshotFile& file : files)
			{
				const std::filesystem::path target = repository / file.relative;
				if (!uam::paths::CreateDirectoriesNoThrow(target.parent_path(), &ec) || !std::filesystem::copy_file(file.source, target, std::filesystem::copy_options::none, ec))
				{
					error = "Failed to snapshot source file " + file.relative.generic_string() + ": " + ec.message();
					return false;
				}
				const std::uintmax_t current_size = std::filesystem::file_size(file.source, ec);
				const std::filesystem::file_time_type current_modified_at = std::filesystem::last_write_time(file.source, ec);
				if (ec || current_size != file.size || current_modified_at != file.modified_at)
				{
					error = "Source changed while its baseline was being created: " + file.relative.generic_string();
					return false;
				}
			}
			return true;
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

		bool FilesHaveSameContents(const std::filesystem::path& lhs, const std::filesystem::path& rhs)
		{
			std::error_code ec;
			const bool lhs_exists = uam::paths::PathExistsNoThrow(lhs);
			const bool rhs_exists = uam::paths::PathExistsNoThrow(rhs);
			if (lhs_exists != rhs_exists)
			{
				return false;
			}
			if (!lhs_exists)
			{
				return true;
			}
			if (!std::filesystem::is_regular_file(lhs, ec) || ec || !std::filesystem::is_regular_file(rhs, ec) || ec)
			{
				return false;
			}
			if (std::filesystem::file_size(lhs, ec) != std::filesystem::file_size(rhs, ec) || ec)
			{
				return false;
			}
			std::ifstream left(lhs, std::ios::binary);
			std::ifstream right(rhs, std::ios::binary);
			char left_buffer[8192];
			char right_buffer[8192];
			while (left && right)
			{
				left.read(left_buffer, sizeof(left_buffer));
				right.read(right_buffer, sizeof(right_buffer));
				if (left.gcount() != right.gcount() || !std::equal(left_buffer, left_buffer + left.gcount(), right_buffer))
				{
					return false;
				}
			}
			return left.eof() && right.eof();
		}

		bool ManagedSourceHasConflicts(const std::filesystem::path& repository, const std::filesystem::path& source, const std::filesystem::path& worktree, const std::string& base_ref, std::string& error)
		{
			std::string changed;
			if (!GitOutput(worktree, "diff --cached --name-only --diff-filter=ACDMRTUXB " + uam::shell::EscapeArg(base_ref), &changed, &error))
			{
				return true;
			}
			std::istringstream paths(changed);
			std::string relative_text;
			while (std::getline(paths, relative_text))
			{
				const std::filesystem::path relative(relative_text);
				if (HasUnsafeSnapshotName(relative) || IsExcludedSnapshotPath(relative))
				{
					error = "Chat changes contain an unsafe path: " + relative_text;
					return true;
				}
				if (!FilesHaveSameContents(repository / relative, source / relative))
				{
					error = "Source changed after the isolation baseline at " + relative.generic_string() + ". Resolve that conflict before porting chat changes.";
					return true;
				}
			}
			return false;
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
			const std::filesystem::path repository = status.managed_repository ? ManagedRepositoryRoot(app, source, chat.id) : source;
			const std::filesystem::path worktree = WorktreePathFromStatus(status);

			if (!worktree.empty())
			{
				if (uam::paths::PathExistsNoThrow(worktree))
				{
					if (!GitCommand(repository, "worktree remove --force " + uam::shell::EscapeArg(worktree.string()), error_out))
					{
						return false;
					}
				}
			}

			const std::string branch_name = BranchNameFromStatusOrChat(status, chat);
			if (!branch_name.empty())
			{
				GitCommand(repository, "branch -D " + uam::shell::EscapeArg(branch_name));
			}

			if (status.managed_repository && uam::paths::PathExistsNoThrow(repository))
			{
				const std::filesystem::path managed_root = app.data_root / "worktrees";
				if (!PathIsWithin(repository, managed_root))
				{
					if (error_out != nullptr)
					{
						*error_out = "Refusing to remove a managed repository outside UAM's data directory.";
					}
					return false;
				}
				std::error_code ec;
				if (!uam::paths::RemoveAllNoThrow(repository, &ec))
				{
					if (error_out != nullptr)
					{
						*error_out = "Failed to remove managed repository: " + ec.message();
					}
					return false;
				}
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

			(void)git_error;
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
		if (status.isolated && !status.is_git_repository)
		{
			status.managed_repository = uam::paths::IsDirectoryNoThrow(ManagedRepositoryRoot(app, source_candidate, chat.id));
			if (!status.managed_repository)
			{
				status.error = "The UAM-managed repository for this isolated workspace is missing.";
			}
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
		const std::filesystem::path source_root = result.status.is_git_repository ? std::filesystem::path(result.status.source_directory) : EffectiveSourceWorkspace(app, chat);
		const bool managed_repository = !result.status.is_git_repository;
		const std::filesystem::path repository = managed_repository ? ManagedRepositoryRoot(app, source_root, chat.id) : source_root;
		std::string head;
		if (managed_repository)
		{
			std::vector<SnapshotFile> files;
			if (!CollectSnapshotFiles(app, source_root, files, result.message))
			{
				return result;
			}
			if (uam::paths::PathExistsNoThrow(repository))
			{
				result.message = "Managed repository path already exists: " + repository.string();
				return result;
			}
			std::error_code ec;
			if (!uam::paths::CreateDirectoriesNoThrow(repository, &ec))
			{
				result.message = "Failed to create managed repository directory: " + ec.message();
				return result;
			}
			auto cleanup_managed_repository = [&repository]()
			{
				std::error_code cleanup_error;
				uam::paths::RemoveAllNoThrow(repository, &cleanup_error);
			};
			if (!GitCommand(repository, "init", &result.message) ||
			    !GitCommand(repository, "config user.email uam@local.invalid", &result.message) ||
			    !GitCommand(repository, "config user.name UAM", &result.message) ||
			    !GitCommand(repository, "config core.autocrlf false", &result.message) ||
			    !CopySnapshotFiles(files, repository, result.message) ||
			    !GitCommand(repository, "add -A", &result.message) ||
			    !GitCommand(repository, "commit --allow-empty -m " + uam::shell::EscapeArg("UAM isolation baseline"), &result.message))
			{
				cleanup_managed_repository();
				return result;
			}
		}

		if (!GitOutput(repository, "rev-parse HEAD", &head, &result.message))
		{
			if (managed_repository)
			{
				std::error_code cleanup_error;
				uam::paths::RemoveAllNoThrow(repository, &cleanup_error);
			}
			return result;
		}

		const std::filesystem::path worktree_root = WorktreeRootForChat(app, source_root, chat.id);
		if (uam::paths::PathExistsNoThrow(worktree_root))
		{
			result.message = "Worktree path already exists: " + worktree_root.string();
			if (managed_repository)
			{
				std::error_code cleanup_error;
				uam::paths::RemoveAllNoThrow(repository, &cleanup_error);
			}
			return result;
		}
		std::error_code ec;
		if (!uam::paths::CreateDirectoriesNoThrow(worktree_root.parent_path(), &ec))
		{
			result.message = "Failed to create worktree parent directory.";
			if (managed_repository)
			{
				std::error_code cleanup_error;
				uam::paths::RemoveAllNoThrow(repository, &cleanup_error);
			}
			return result;
		}

		const std::string branch_name = BranchNameForChat(chat.id);
		const ProcessExecutionResult add_result = RunCommand(BuildGitCommandInDirectory(repository, "worktree add -b " + uam::shell::EscapeArg(branch_name) + " " + uam::shell::EscapeArg(worktree_root.string()) + " HEAD"));
		if (!CommandSucceeded(add_result))
		{
			result.message = CommandOutputOrFallback(add_result, "Failed to create Git worktree.");
			if (managed_repository)
			{
				std::error_code cleanup_error;
				uam::paths::RemoveAllNoThrow(repository, &cleanup_error);
			}
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
			GitCommand(repository, "worktree remove --force " + uam::shell::EscapeArg(worktree_root.string()));
			GitCommand(repository, "branch -D " + uam::shell::EscapeArg(branch_name));
			if (managed_repository)
			{
				std::error_code cleanup_error;
				uam::paths::RemoveAllNoThrow(repository, &cleanup_error);
			}
			chat.workspace_isolation_kind.clear();
			chat.workspace_source_directory.clear();
			chat.workspace_base_ref.clear();
			chat.workspace_branch_name.clear();
			chat.workspace_worktree_directory.clear();
			return result;
		}

		const std::string message = managed_repository ? "Created an isolated Git worktree from a local UAM-managed baseline." : result.status.source_dirty ? "Created isolated Git worktree from HEAD. Source workspace has uncommitted changes that were not copied." : "Created isolated Git worktree.";
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
		const std::string base_ref = uam::strings::Trim(result.status.base_ref);
		if (base_ref.empty())
		{
			result.message = "Isolation baseline is missing.";
			return result;
		}
		const std::filesystem::path repository = result.status.managed_repository ? ManagedRepositoryRoot(app, source, chat.id) : source;
		if (!result.status.managed_repository)
		{
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
		}

		if (!GitCommand(worktree, "add -A", &result.message))
		{
			return result;
		}
		if (result.status.managed_repository && ManagedSourceHasConflicts(repository, source, worktree, base_ref, result.message))
		{
			GitCommand(worktree, "reset --mixed HEAD");
			return result;
		}

		ProcessExecutionResult diff_result = RunCommand(BuildGitCommandInDirectory(worktree, "diff --binary --cached " + uam::shell::EscapeArg(base_ref)));
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

		if (result.status.managed_repository)
		{
			const ProcessExecutionResult check_result = RunCommand(BuildGitCommandInDirectory(source, "-c core.autocrlf=false apply --check " + uam::shell::EscapeArg(patch_path.string())));
			if (!CommandSucceeded(check_result))
			{
				result.patch_path = patch_path;
				result.message = CommandOutputOrFallback(check_result, "Chat changes cannot be applied cleanly to the source workspace.");
				return result;
			}
		}
		ProcessExecutionResult apply_result = RunCommand(BuildGitCommandInDirectory(source, std::string(result.status.managed_repository ? "-c core.autocrlf=false apply " : "apply --3way ") + uam::shell::EscapeArg(patch_path.string())));
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
