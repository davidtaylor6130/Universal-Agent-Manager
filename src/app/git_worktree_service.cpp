#include "app/git_worktree_service.h"

#include "app/application_core_helpers.h"
#include "app/persistence_coordinator.h"
#include "app/provider_resolution_service.h"
#include "common/chat/chat_repository.h"
#include "common/platform/platform_services.h"
#include "common/provider/provider_runtime.h"

#include <chrono>
#include <cctype>
#include <fstream>
#include <sstream>

namespace uam
{
	namespace
	{
		constexpr const char* kGitWorktreeIsolationKind = "gitWorktree";

		std::string ShellQuote(const std::string& value)
		{
	#if defined(_WIN32)
			std::string escaped = "\"";
			for (const char ch : value)
			{
				if (ch == '"')
				{
					escaped += "\"\"";
				}
				else if (ch == '%')
				{
					escaped += "%%";
				}
				else if (ch == '\r' || ch == '\n')
				{
					escaped.push_back(' ');
				}
				else
				{
					escaped.push_back(ch);
				}
			}
			escaped.push_back('"');
			return escaped;
	#else
			std::string escaped = "'";
			for (const char ch : value)
			{
				if (ch == '\'')
				{
					escaped += "'\\''";
				}
				else
				{
					escaped.push_back(ch);
				}
			}
			escaped.push_back('\'');
			return escaped;
	#endif
		}

		ProcessExecutionResult RunCommand(const std::string& command, const int timeout_ms = 120000)
		{
			return PlatformServicesFactory::Instance().process_service.ExecuteCommand(command, timeout_ms);
		}

		std::string CommandOutputOrError(const ProcessExecutionResult& result)
		{
			std::string detail = Trim(result.output);
			const std::string error = Trim(result.error);
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

		std::string GitC(const std::filesystem::path& cwd, const std::string& args)
		{
			return "git -C " + ShellQuote(cwd.string()) + " " + args;
		}

		bool CommandSucceeded(const ProcessExecutionResult& result)
		{
			return result.ok && !result.timed_out && !result.canceled && result.exit_code == 0;
		}

		bool GitOutput(const std::filesystem::path& cwd, const std::string& args, std::string* output_out, std::string* error_out = nullptr)
		{
			const ProcessExecutionResult result = RunCommand(GitC(cwd, args));
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
				*output_out = Trim(result.output);
			}
			return true;
		}

		std::filesystem::path EffectiveSourceWorkspace(const AppState& app, const ChatSession& chat)
		{
			if (Trim(chat.workspace_isolation_kind) == kGitWorktreeIsolationKind && !Trim(chat.workspace_source_directory).empty())
			{
				return PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(chat.workspace_source_directory);
			}

			ChatSession source_chat = chat;
			source_chat.workspace_isolation_kind.clear();
			source_chat.workspace_worktree_directory.clear();
			return ResolveWorkspaceRootPath(app, source_chat);
		}

		bool IsSvnWorkspace(const std::filesystem::path& workspace)
		{
			std::error_code ec;
			if (std::filesystem::exists(workspace / ".svn", ec) && !ec)
			{
				return true;
			}

			std::filesystem::path current = workspace;
			while (!current.empty() && current.has_parent_path() && current != current.parent_path())
			{
				if (std::filesystem::exists(current / ".svn", ec) && !ec)
				{
					return true;
				}
				current = current.parent_path();
			}
			return false;
		}

		std::filesystem::path WorktreeRootForChat(const AppState& app, const std::filesystem::path& source_root, const std::string& chat_id)
		{
			const std::string repo_key = Hex64(Fnv1a64(source_root.lexically_normal().generic_string()));
			return app.data_root / "worktrees" / repo_key / chat_id;
		}

		std::string BranchNameForChat(const std::string& chat_id)
		{
			std::string safe;
			safe.reserve(chat_id.size());
			for (const unsigned char ch : chat_id)
			{
				if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.')
				{
					safe.push_back(static_cast<char>(ch));
				}
				else
				{
					safe.push_back('-');
				}
			}
			return "uam/" + (safe.empty() ? std::string("chat") : safe);
		}

		bool IsDirty(const std::filesystem::path& repo, bool* dirty_out, std::string* error_out = nullptr)
		{
			std::string status;
			if (!GitOutput(repo, "status --porcelain", &status, error_out))
			{
				return false;
			}
			*dirty_out = !Trim(status).empty();
			return true;
		}

		bool SaveChat(AppState& app, const ChatSession& chat, std::string* error_out)
		{
			const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
			if (!ProviderRuntime::SaveHistory(provider, app.data_root, chat))
			{
				if (error_out != nullptr)
				{
					*error_out = app.status_line.empty() ? "Failed to persist chat worktree metadata." : app.status_line;
				}
				return false;
			}
			if (!PersistenceCoordinator().SaveSettings(app))
			{
				if (error_out != nullptr)
				{
					*error_out = app.status_line.empty() ? "Failed to persist application settings." : app.status_line;
				}
				return false;
			}
			return true;
		}

		std::filesystem::path PatchPathForChat(const AppState& app, const std::string& chat_id)
		{
			const auto ticks = std::chrono::system_clock::now().time_since_epoch().count();
			return app.data_root / "worktrees" / "patches" / (chat_id + "-" + std::to_string(ticks) + ".patch");
		}

		bool WriteTextFileEnsuringParent(const std::filesystem::path& path, const std::string& content)
		{
			std::error_code ec;
			std::filesystem::create_directories(path.parent_path(), ec);
			if (ec)
			{
				return false;
			}
			std::ofstream out(path, std::ios::binary);
			if (!out.good())
			{
				return false;
			}
			out << content;
			return out.good();
		}
	} // namespace

	GitWorktreeStatus GitWorktreeService::Status(const AppState& app, const ChatSession& chat) const
	{
		GitWorktreeStatus status;
		status.isolated = Trim(chat.workspace_isolation_kind) == kGitWorktreeIsolationKind;
		status.source_directory = Trim(chat.workspace_source_directory);
		status.worktree_directory = Trim(chat.workspace_worktree_directory);
		status.branch_name = Trim(chat.workspace_branch_name);
		status.base_ref = Trim(chat.workspace_base_ref);

		const std::filesystem::path source_candidate = EffectiveSourceWorkspace(app, chat);
		status.is_svn_workspace = IsSvnWorkspace(source_candidate);

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
		}
		else if (!status.is_svn_workspace)
		{
			status.error = git_error.empty() ? "Workspace is not inside a Git repository." : git_error;
		}

		if (status.is_svn_workspace && !status.is_git_repository)
		{
			status.error = "SVN workspaces are not supported by Git worktree isolation in this release.";
		}

		if (status.isolated)
		{
			std::error_code ec;
			const std::filesystem::path worktree_path(status.worktree_directory);
			status.worktree_missing = status.worktree_directory.empty() || !std::filesystem::exists(worktree_path, ec) || ec;
			if (!status.worktree_missing)
			{
				bool worktree_dirty = false;
				if (IsDirty(worktree_path, &worktree_dirty))
				{
					status.worktree_dirty = worktree_dirty;
				}
			}
		}

		return status;
	}

	GitWorktreeOperationResult GitWorktreeService::CreateForChat(AppState& app, ChatSession& chat) const
	{
		GitWorktreeOperationResult result;
		result.status = Status(app, chat);
		if (result.status.isolated)
		{
			result.ok = true;
			result.message = "Chat already has an isolated Git worktree.";
			return result;
		}
		if (!result.status.is_git_repository)
		{
			result.message = result.status.error.empty() ? "Workspace is not inside a Git repository." : result.status.error;
			return result;
		}

		const std::filesystem::path source_root(result.status.source_directory);
		std::string head;
		if (!GitOutput(source_root, "rev-parse HEAD", &head, &result.message))
		{
			return result;
		}

		const std::filesystem::path worktree_root = WorktreeRootForChat(app, source_root, chat.id);
		std::error_code ec;
		if (std::filesystem::exists(worktree_root, ec))
		{
			result.message = "Worktree path already exists: " + worktree_root.string();
			return result;
		}
		std::filesystem::create_directories(worktree_root.parent_path(), ec);
		if (ec)
		{
			result.message = "Failed to create worktree parent directory.";
			return result;
		}

		const std::string branch_name = BranchNameForChat(chat.id);
		const ProcessExecutionResult add_result = RunCommand(GitC(source_root, "worktree add -b " + ShellQuote(branch_name) + " " + ShellQuote(worktree_root.string()) + " HEAD"));
		if (!CommandSucceeded(add_result))
		{
			result.message = CommandOutputOrError(add_result);
			if (result.message.empty())
			{
				result.message = "Failed to create Git worktree.";
			}
			return result;
		}

		chat.workspace_isolation_kind = kGitWorktreeIsolationKind;
		chat.workspace_source_directory = source_root.string();
		chat.workspace_base_ref = head;
		chat.workspace_branch_name = branch_name;
		chat.workspace_worktree_directory = worktree_root.string();
		chat.updated_at = TimestampNow();

		std::string save_error;
		if (!SaveChat(app, chat, &save_error))
		{
			result.message = save_error;
			return result;
		}

		result.ok = true;
		result.message = result.status.source_dirty
			? "Created isolated Git worktree from HEAD. Source workspace has uncommitted changes that were not copied."
			: "Created isolated Git worktree.";
		result.status = Status(app, chat);
		return result;
	}

	GitWorktreeOperationResult GitWorktreeService::DiscardChatChanges(AppState& app, ChatSession& chat) const
	{
		GitWorktreeOperationResult result;
		result.status = Status(app, chat);
		if (!result.status.isolated || result.status.worktree_missing)
		{
			result.message = "Chat does not have an available isolated Git worktree.";
			return result;
		}

		const std::filesystem::path worktree(result.status.worktree_directory);
		ProcessExecutionResult reset_result = RunCommand(GitC(worktree, "reset --hard HEAD"));
		if (!CommandSucceeded(reset_result))
		{
			result.message = CommandOutputOrError(reset_result);
			return result;
		}

		ProcessExecutionResult clean_result = RunCommand(GitC(worktree, "clean -fd"));
		if (!CommandSucceeded(clean_result))
		{
			result.message = CommandOutputOrError(clean_result);
			return result;
		}

		chat.updated_at = TimestampNow();
		std::string save_error;
		if (!SaveChat(app, chat, &save_error))
		{
			result.message = save_error;
			return result;
		}

		result.ok = true;
		result.message = "Discarded changes in the chat worktree.";
		result.status = Status(app, chat);
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

		ProcessExecutionResult add_result = RunCommand(GitC(worktree, "add -A"));
		if (!CommandSucceeded(add_result))
		{
			result.message = CommandOutputOrError(add_result);
			return result;
		}

		ProcessExecutionResult diff_result = RunCommand(GitC(worktree, "diff --binary --cached HEAD"));
		const ProcessExecutionResult reset_index_result = RunCommand(GitC(worktree, "reset --mixed HEAD"));
		if (!CommandSucceeded(diff_result))
		{
			result.message = CommandOutputOrError(diff_result);
			return result;
		}
		if (!CommandSucceeded(reset_index_result))
		{
			result.message = CommandOutputOrError(reset_index_result);
			return result;
		}

		const std::string patch = diff_result.output;
		if (Trim(patch).empty())
		{
			result.ok = true;
			result.message = "No chat worktree changes to port.";
			result.status = Status(app, chat);
			return result;
		}

		const std::filesystem::path patch_path = PatchPathForChat(app, chat.id);
		if (!WriteTextFileEnsuringParent(patch_path, patch))
		{
			result.message = "Failed to write patch file.";
			return result;
		}

		ProcessExecutionResult apply_result = RunCommand(GitC(source, "apply --3way " + ShellQuote(patch_path.string())));
		if (!CommandSucceeded(apply_result))
		{
			result.patch_path = patch_path;
			result.message = CommandOutputOrError(apply_result);
			if (result.message.empty())
			{
				result.message = "Failed to apply patch to source workspace.";
			}
			return result;
		}

		chat.updated_at = TimestampNow();
		std::string save_error;
		if (!SaveChat(app, chat, &save_error))
		{
			result.message = save_error;
			return result;
		}

		result.ok = true;
		result.patch_path = patch_path;
		result.message = "Applied chat worktree changes to the source workspace.";
		result.status = Status(app, chat);
		return result;
	}
} // namespace uam
