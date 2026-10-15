#include <nlohmann/json.hpp>
#include "app/git_worktree_service.h"
#include "app/chat_domain_service.h"
#include "app/worktree_launch_preparation_service.h"

#include "app/provider_resolution_service.h"
#include "common/chat/chat_repository.h"
#include "common/chat/native_chat_identity.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_root.h"
#include "common/platform/platform_services.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/utils/hash_utils.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/io_utils.h"
#include "common/utils/shell_escape.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"
#include "remote/runner_client.h"

#include <fstream>
#include <functional>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace uam
{
	GitTurnRollbackSnapshot CaptureGitTurnRollbackSnapshot(const ChatSession& chat, int message_index)
	{
		GitTurnRollbackSnapshot snapshot;
		snapshot.chat_id = chat.id;
		snapshot.native_identity_key = uam::chat_identity::NativeIdentityKeyForHistoryImport(chat);
		snapshot.provider_id = chat.provider_id;
		snapshot.execution_host_id = chat.execution_host_id;
		snapshot.workspace_directory = chat.workspace_directory;
		snapshot.workspace_isolation_kind = chat.workspace_isolation_kind;
		snapshot.workspace_source_directory = chat.workspace_source_directory;
		snapshot.workspace_base_ref = chat.workspace_base_ref;
		snapshot.workspace_branch_name = chat.workspace_branch_name;
		snapshot.workspace_worktree_directory = chat.workspace_worktree_directory;
		snapshot.updated_at = chat.updated_at;
		snapshot.message_count = chat.messages_loaded ? chat.messages.size() : chat.persisted_message_count;
		snapshot.persisted_messages_digest = snapshot.updated_at + ":" + std::to_string(snapshot.message_count);
		snapshot.message_index = message_index;
		if (chat.messages_loaded && message_index >= 0 && message_index < static_cast<int>(chat.messages.size()))
		{
			snapshot.checkpoint_sha = chat.messages[message_index].checkpoint_sha;
			snapshot.checkpoint_parent_sha = chat.messages[message_index].checkpoint_parent_sha;
		}
		return snapshot;
	}

	bool MatchesGitTurnRollbackSnapshot(const ChatSession& chat, const GitTurnRollbackSnapshot& snapshot)
	{
		const std::size_t message_count = chat.messages_loaded ? chat.messages.size() : chat.persisted_message_count;
		return chat.id == snapshot.chat_id &&
		    uam::chat_identity::NativeIdentityKeyForHistoryImport(chat) == snapshot.native_identity_key &&
		    chat.provider_id == snapshot.provider_id &&
		    chat.execution_host_id == snapshot.execution_host_id &&
		    chat.workspace_directory == snapshot.workspace_directory &&
		    chat.workspace_isolation_kind == snapshot.workspace_isolation_kind &&
		    chat.workspace_source_directory == snapshot.workspace_source_directory &&
		    chat.workspace_base_ref == snapshot.workspace_base_ref &&
		    chat.workspace_branch_name == snapshot.workspace_branch_name &&
		    chat.workspace_worktree_directory == snapshot.workspace_worktree_directory &&
		    chat.updated_at == snapshot.updated_at && message_count == snapshot.message_count &&
		    chat.updated_at + ":" + std::to_string(message_count) == snapshot.persisted_messages_digest &&
		    (snapshot.checkpoint_sha.empty() ||
		     (!chat.messages_loaded || (snapshot.message_index >= 0 &&
		      snapshot.message_index < static_cast<int>(chat.messages.size()) &&
		      chat.messages[snapshot.message_index].checkpoint_sha == snapshot.checkpoint_sha &&
		      chat.messages[snapshot.message_index].checkpoint_parent_sha == snapshot.checkpoint_parent_sha)));
	}

	bool ApplyGitTurnRollbackSnapshot(ChatSession& live_chat, const GitTurnRollbackSnapshot& before,
	                                  const ChatSession& persisted_after, const std::string& checkpoint_sha)
	{
		if (checkpoint_sha.empty() || !MatchesGitTurnRollbackSnapshot(live_chat, before) ||
		    persisted_after.messages_loaded || persisted_after.id != before.chat_id ||
		    uam::chat_identity::NativeIdentityKeyForHistoryImport(persisted_after) != before.native_identity_key ||
		    persisted_after.provider_id != before.provider_id ||
		    persisted_after.execution_host_id != before.execution_host_id ||
		    persisted_after.workspace_directory != before.workspace_directory ||
		    persisted_after.workspace_isolation_kind != before.workspace_isolation_kind ||
		    persisted_after.workspace_source_directory != before.workspace_source_directory ||
		    persisted_after.workspace_base_ref != before.workspace_base_ref ||
		    persisted_after.workspace_branch_name != before.workspace_branch_name ||
		    persisted_after.workspace_worktree_directory != before.workspace_worktree_directory ||
		    persisted_after.persisted_message_count != before.message_count ||
		    persisted_after.persisted_messages_digest != persisted_after.updated_at + ":" +
		        std::to_string(persisted_after.persisted_message_count))
			return false;

		if (live_chat.messages_loaded)
		{
			if (before.message_index < 0 || before.message_index >= static_cast<int>(live_chat.messages.size()) ||
			    live_chat.messages[before.message_index].checkpoint_sha != checkpoint_sha)
				return false;
			live_chat.messages[before.message_index].checkpoint_sha.clear();
			live_chat.messages[before.message_index].checkpoint_parent_sha.clear();
		}
		else if (!live_chat.messages.empty())
		{
			return false;
		}

		live_chat.updated_at = persisted_after.updated_at;
		live_chat.persisted_message_count = persisted_after.persisted_message_count;
		live_chat.persisted_messages_digest = persisted_after.persisted_messages_digest;
		return true;
	}

	namespace
	{
		constexpr int kDefaultGitCommandTimeoutMs = 120000;
		constexpr std::uintmax_t kMaxManagedSnapshotBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
		constexpr std::size_t kMaxManagedSnapshotFiles = 100000;
		constexpr std::string_view kRemoteWorkspaceUnsupported =
		    "Local Git worktrees are unavailable for remote workspaces.";

		struct SnapshotFile
		{
			std::filesystem::path source;
			std::filesystem::path relative;
			std::uintmax_t size = 0;
			std::filesystem::file_time_type modified_at;
		};

		ProcessExecutionResult RunCommand(const std::string& command, int timeout_ms = kDefaultGitCommandTimeoutMs, std::stop_token stop_token = {})
		{
			return PlatformServicesFactory::Instance().process_service.ExecuteCommand(command, timeout_ms, stop_token);
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
			return "git -C " + uam::shell::EscapeArg(uam::paths::Utf8PathString(cwd)) + " " + args;
		}

		bool CommandSucceeded(const ProcessExecutionResult& result)
		{
			return result.ok && !result.timed_out && !result.canceled && result.exit_code == 0;
		}

		bool GitCommand(const std::filesystem::path& cwd, const std::string& args, std::string* error_out = nullptr, std::stop_token stop_token = {})
		{
			if (error_out != nullptr)
			{
				error_out->clear();
			}

			const ProcessExecutionResult result = RunCommand(BuildGitCommandInDirectory(cwd, args), kDefaultGitCommandTimeoutMs, stop_token);
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

		bool GitOutput(const std::filesystem::path& cwd, const std::string& args, std::string* output_out, std::string* error_out = nullptr, std::stop_token stop_token = {})
		{
			if (output_out != nullptr)
			{
				output_out->clear();
			}
			if (error_out != nullptr)
			{
				error_out->clear();
			}

			const ProcessExecutionResult result = RunCommand(BuildGitCommandInDirectory(cwd, args), kDefaultGitCommandTimeoutMs, stop_token);
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

		bool PullCheckedOutBase(const std::filesystem::path& repository, const std::string& configured_base, std::string& error, std::stop_token stop)
		{
			std::string branch, upstream, dirty;
			if (!GitOutput(repository, "symbolic-ref --quiet --short HEAD", &branch, &error, stop))
			{ error = "Pull before chat requires a checked-out local branch."; return false; }
			if (!configured_base.empty() && configured_base != branch)
			{ error = "Pull before chat requires the default base to be the checked-out branch."; return false; }
			if (!GitOutput(repository, "rev-parse --abbrev-ref --symbolic-full-name @{upstream}", &upstream, &error, stop) || upstream.empty())
			{ error = "Pull before chat requires a configured upstream."; return false; }
			if (!GitOutput(repository, "status --porcelain --untracked-files=no", &dirty, &error, stop) || !dirty.empty())
			{ error = "Pull before chat requires a clean tracked workspace and index."; return false; }
			for (const std::string_view marker : {"MERGE_HEAD", "CHERRY_PICK_HEAD", "REVERT_HEAD", "rebase-merge", "rebase-apply", "sequencer"})
			{
				std::string path;
				if (!GitOutput(repository, "rev-parse --path-format=absolute --git-path " + uam::shell::EscapeArg(std::string(marker)), &path, &error, stop)) return false;
				if (uam::paths::PathExistsNoThrow(uam::paths::PathFromUtf8(path)))
				{ error = "Finish the current Git operation before pulling for a chat."; return false; }
			}
			// Automatic preparation never prompts or runs user hooks; environment overrides affect only this child command.
#if defined(_WIN32)
			const std::string environment = "set \"GIT_TERMINAL_PROMPT=0\" && set \"GIT_ASKPASS=\" && set \"SSH_ASKPASS=\" && set \"SSH_ASKPASS_REQUIRE=never\" && set \"GIT_SSH_COMMAND=ssh -oBatchMode=yes -oStrictHostKeyChecking=yes\" && ";
			const std::string hooks = "NUL";
#else
			const std::string environment = "GIT_TERMINAL_PROMPT=0 GIT_ASKPASS='' SSH_ASKPASS='' SSH_ASKPASS_REQUIRE=never GIT_SSH_COMMAND='ssh -oBatchMode=yes -oStrictHostKeyChecking=yes' ";
			const std::string hooks = "/dev/null";
#endif
			const std::string command = environment + BuildGitCommandInDirectory(repository,
			    "-c credential.interactive=false -c core.askPass= -c core.hooksPath=" + uam::shell::EscapeArg(hooks) +
			    " pull --ff-only --no-rebase --no-autostash --recurse-submodules=no");
			const ProcessExecutionResult result = RunCommand(command, kDefaultGitCommandTimeoutMs, stop);
			if (!CommandSucceeded(result))
			{ error = CommandOutputOrFallback(result, "Pull before chat failed; no chat workspace was created."); return false; }
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

		enum class LocalSnapshotSource { Unknown, Git, Svn, Plain };

		LocalSnapshotSource ClassifyLocalSnapshotSource(const std::filesystem::path& source)
		{
			std::error_code source_error;
			const std::filesystem::path physical = std::filesystem::canonical(source, source_error);
			if (source_error || !paths::IsDirectoryNoThrow(physical)) return LocalSnapshotSource::Unknown;
			bool svn = false;
			for (std::filesystem::path directory = physical; !directory.empty(); directory = directory.parent_path())
			{
				bool markers[5]{};
				const char* names[] = {".git", "HEAD", "objects", "refs", "config"};
				for (std::size_t index = 0; index < 5; ++index)
				{
					std::error_code error;
					const std::filesystem::file_status status = std::filesystem::symlink_status(directory / names[index], error);
					if (error && error != std::errc::no_such_file_or_directory) return LocalSnapshotSource::Unknown;
					markers[index] = std::filesystem::exists(status);
				}
				// Broken Git markers and bare repositories must retain normal Git validation.
				if (markers[0] || (markers[1] && markers[2]) || ((markers[1] || markers[2]) && markers[3] && markers[4])) return LocalSnapshotSource::Git;
				std::error_code error;
				const std::filesystem::file_status status = std::filesystem::symlink_status(directory / ".svn", error);
				if (error && error != std::errc::no_such_file_or_directory) return LocalSnapshotSource::Unknown;
				svn = svn || std::filesystem::exists(status);
				if (directory == directory.parent_path()) break;
			}
			return svn ? LocalSnapshotSource::Svn : LocalSnapshotSource::Plain;
		}

		std::filesystem::path WorktreeRootForChat(const AppState& app, const std::filesystem::path& source_root, const std::string& relative_name)
		{
			const std::string normalized_source_root = uam::paths::NormalizedPortablePathString(source_root);
			const std::string repo_key = uam::hashing::Hex64(uam::hashing::Fnv1a64(normalized_source_root));
			const std::filesystem::path root = app.data_root / "worktrees" / repo_key;
			return relative_name.empty() ? root : root / uam::paths::PathFromUtf8(relative_name);
		}

		/// Keeps Git's Windows administrative path within its startup limit without changing the branch ref.
		bool PhysicalWorktreeRootForChat(const AppState& app, const std::filesystem::path& source_root,
		    const std::string& relative_name, std::filesystem::path& path, std::string& error)
		{
			path = WorktreeRootForChat(app, source_root, relative_name);
#if defined(_WIN32)
			constexpr std::size_t kMaxGitAdministrativePathBytes = 220;
			const std::filesystem::path absolute = uam::paths::AbsolutePathNoThrow(path);
			if (uam::paths::Utf8PathString(absolute / ".git").size() > kMaxGitAdministrativePathBytes)
			{
				const std::filesystem::path parent = absolute.parent_path();
				const std::size_t fixed_bytes = uam::paths::Utf8PathString(parent / "x" / ".git").size() - 1;
				const std::string digest = uam::hashing::Hex64Padded(uam::hashing::Fnv1a64(relative_name));
				// At least one readable byte, a separator and the complete stable digest must fit.
				if (fixed_bytes + 1 + 1 + digest.size() > kMaxGitAdministrativePathBytes)
				{
					error = "The data directory is too long for a Windows Git worktree. Choose a shorter data directory.";
					return false;
				}
				std::string leaf = uam::paths::Utf8PathString(path.filename());
				std::size_t length = std::min(leaf.size(), kMaxGitAdministrativePathBytes - fixed_bytes - 1 - digest.size());
				while (length > 0 && length < leaf.size() && (static_cast<unsigned char>(leaf[length]) & 0xc0) == 0x80) --length;
				if (length == 0)
				{
					error = "The data directory leaves no room for the worktree's UTF-8 name. Choose a shorter data directory.";
					return false;
				}
				leaf.resize(length);
				path = path.parent_path() / uam::paths::PathFromUtf8(leaf + "-" + digest);
			}
#endif
			return true;
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
				const std::string name = uam::strings::ToLowerAscii(uam::paths::Utf8PathString(part));
				if (name == ".git" || name == ".svn" || name == ".hg" || name == ".uam")
				{
					return true;
				}
			}
			return false;
		}

		bool HasUnsafeSnapshotName(const std::filesystem::path& relative)
		{
			const std::string value = uam::paths::PortablePathString(relative);
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
					error = "Failed to inspect source path " + uam::paths::PortablePathString(relative) + ": " + ec.message();
					return false;
				}
				if (std::filesystem::is_symlink(status))
				{
					error = "Source workspace contains a symbolic link that cannot be snapshotted safely: " + uam::paths::PortablePathString(relative);
					return false;
				}
				if (std::filesystem::is_directory(status))
				{
					continue;
				}
				if (!std::filesystem::is_regular_file(status))
				{
					error = "Source workspace contains an unsupported file type: " + uam::paths::PortablePathString(relative);
					return false;
				}

				const std::uintmax_t size = std::filesystem::file_size(path, ec);
				const std::filesystem::file_time_type modified_at = std::filesystem::last_write_time(path, ec);
				std::ifstream readable(path, std::ios::binary);
				if (ec || !readable)
				{
					error = "Source file is unreadable: " + uam::paths::PortablePathString(relative);
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
					error = "Failed to snapshot source file " + uam::paths::PortablePathString(file.relative) + ": " + ec.message();
					return false;
				}
				const std::uintmax_t current_size = std::filesystem::file_size(file.source, ec);
				const std::filesystem::file_time_type current_modified_at = std::filesystem::last_write_time(file.source, ec);
				if (ec || current_size != file.size || current_modified_at != file.modified_at)
				{
					error = "Source changed while its baseline was being created: " + uam::paths::PortablePathString(file.relative);
					return false;
				}
			}
			return true;
		}

		/// Produces a portable path component that is also safe in a Git branch ref.
		std::string WorktreeNameComponent(const std::string& value)
		{
			std::string safe;
			for (const unsigned char ch : uam::strings::Trim(value))
			{
				if (uam::strings::IsAsciiAlnum(ch) || ch >= 128 || ch == '_')
				{
					safe.push_back(ch < 128 ? static_cast<char>(std::tolower(ch)) : static_cast<char>(ch));
				}
				else if (!safe.empty() && safe.back() != '-')
				{
					safe.push_back('-');
				}
			}
			if (safe.size() > 80)
			{
				std::size_t length = 80;
				while ((static_cast<unsigned char>(safe[length]) & 0xc0) == 0x80) --length;
				safe.resize(length);
			}
			while (!safe.empty() && safe.back() == '-') safe.pop_back();
			if (safe == "con" || safe == "prn" || safe == "aux" || safe == "nul" ||
			    (safe.size() == 4 && (safe.starts_with("com") || safe.starts_with("lpt")) &&
			     safe.back() >= '1' && safe.back() <= '9')) safe.insert(0, "chat-");
			return safe;
		}

		/// Uses the chat label, falling back to its ID only when no useful label exists.
		std::string BranchNameForChat(const ChatSession& chat)
		{
			const std::string title = uam::strings::Trim(chat.title);
			const bool placeholder = title == "New Session" || title == "Untitled Chat" ||
			    title == "Untitled chat" || (!chat.created_at.empty() && title == "Chat " + chat.created_at);
			const std::string label = placeholder ? std::string() : WorktreeNameComponent(title);
			return uam::strings::NonEmptyOrFallback(WorktreeNameComponent(chat.uam_agent_id), "agents") + "/" +
			    uam::strings::NonEmptyOrFallback(label, uam::strings::NonEmptyOrFallback(WorktreeNameComponent(chat.id), "chat"));
		}

		/// Avoids both filesystem and branch collisions without adding IDs to readable names.
		bool ChooseWorktreeName(const AppState& app, const std::filesystem::path& source,
		    const std::filesystem::path& repository, const ChatSession& chat, std::string& name,
		    std::filesystem::path& worktree, std::string& error, std::stop_token stop = {})
		{
			std::string refs;
			if (!GitOutput(repository, "for-each-ref refs/heads", &refs, &error, stop)) return false;
			std::vector<std::string> branches;
			std::istringstream lines(refs);
			std::string ref;
			while (std::getline(lines, ref))
			{
				// Git's default output separates the object fields from the full ref with a tab.
				const std::size_t separator = ref.find('\t');
				const std::string full_ref = separator == std::string::npos ? std::string() : uam::strings::Trim(ref.substr(separator + 1));
				if (!full_ref.starts_with("refs/heads/"))
				{
					error = "Could not read Git branch names for worktree creation.";
					return false;
				}
				branches.push_back(full_ref.substr(std::string_view("refs/heads/").size()));
			}
			const std::string preferred = BranchNameForChat(chat);
			const std::size_t separator = preferred.find('/');
			const std::string agent = preferred.substr(0, separator);
			std::string prefix = agent;
			for (int suffix = 2; suffix < 10000; ++suffix)
			{
				const std::filesystem::path parent = WorktreeRootForChat(app, source, prefix);
				if (std::find(branches.begin(), branches.end(), prefix) == branches.end() &&
				    (!uam::paths::PathExistsNoThrow(parent) || uam::paths::IsDirectoryNoThrow(parent))) break;
				prefix = agent + "-" + std::to_string(suffix);
			}
			const std::string base = prefix + preferred.substr(separator);
			for (int suffix = 1; suffix < 10000; ++suffix)
			{
				name = base + (suffix == 1 ? "" : "-" + std::to_string(suffix));
				if (!PhysicalWorktreeRootForChat(app, source, name, worktree, error)) return false;
				bool collision = uam::paths::PathExistsNoThrow(worktree) ||
				    uam::paths::PathExistsNoThrow(WorktreeRootForChat(app, source, name));
				for (const std::string& branch : branches)
				{
					if (branch == name || branch.starts_with(name + "/") || name.starts_with(branch + "/")) collision = true;
				}
				if (!collision) return true;
			}
			error = "Could not find an available worktree name for this chat.";
			return false;
		}

		bool IsDirty(const std::filesystem::path& repo, bool* dirty_out, std::string* error_out = nullptr, std::stop_token stop_token = {})
		{
			if (dirty_out != nullptr)
			{
				*dirty_out = false;
			}

			std::string status;
			if (!GitOutput(repo, "status --porcelain", &status, error_out, stop_token))
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
				const std::filesystem::path relative = uam::paths::PathFromUtf8(relative_text);
				if (HasUnsafeSnapshotName(relative) || IsExcludedSnapshotPath(relative))
				{
					error = "Chat changes contain an unsafe path: " + relative_text;
					return true;
				}
				if (!FilesHaveSameContents(repository / relative, source / relative))
				{
					error = "Source changed after the isolation baseline at " + uam::paths::PortablePathString(relative) + ". Resolve that conflict before porting chat changes.";
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
			return true;
		}

		std::filesystem::path PatchPathForChat(const AppState& app, const std::string& chat_id)
		{
			return app.data_root / "worktrees" / "patches" / (chat_id + "-" + uam::time::SystemEpochMicrosecondsTokenNow() + ".patch");
		}

		std::filesystem::path TrimmedPathOrEmpty(std::string_view value)
		{
			const std::string_view trimmed = uam::strings::TrimAsciiView(value);
			return trimmed.empty() ? std::filesystem::path() : uam::paths::PathFromUtf8(trimmed);
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

		/** Refuse destructive mutations while another local chat references this worktree. */
		bool RefuseSharedWorktreeMutation(const AppState& app, const ChatSession& chat,
		    const std::filesystem::path& worktree, std::string* error_out)
		{
			if (worktree.empty()) return false;
			std::error_code physical_error;
			const std::filesystem::path physical = std::filesystem::weakly_canonical(worktree, physical_error);
			if (physical_error)
			{
				if (error_out != nullptr) *error_out = "Could not verify worktree references; its files were left untouched.";
				return true;
			}
			const auto references_worktree = [&](const ChatSession& other)
			{
				if (other.id == chat.id || !uam::paths::IsControllerLocalWorkspace(other)) return false;
				for (const std::string* directory : {&other.workspace_directory, &other.workspace_worktree_directory, &other.workspace_source_directory})
				{
					if (directory->empty()) continue;
					std::error_code reference_error;
					const std::filesystem::path reference = std::filesystem::weakly_canonical(uam::paths::PathFromUtf8(*directory), reference_error);
					if (reference_error) continue;
					if (PathIsWithin(reference, physical)) return true;
				}
				return false;
			};
			// Async operation snapshots contain only the target chat; consult persisted metadata too.
			const std::vector<ChatSession> persisted = ChatRepository::LoadLocalChatSummariesReadOnly(app.data_root);
			if (std::any_of(app.chats.begin(), app.chats.end(), references_worktree) ||
			    std::any_of(persisted.begin(), persisted.end(), references_worktree))
			{
				if (error_out != nullptr) *error_out = "Another chat uses this worktree. Its files and branch were left untouched.";
				return true;
			}
			return false;
		}

		/// Git's administrative backlink proves ownership even after checkout changes the path's original branch label.
		bool VerifiedBranchSelectionWorkspace(const AppState& app, const ChatSession& chat, std::filesystem::path& worktree, std::string& error)
		{
			if (!app.settings.git_enabled || !paths::IsControllerLocalWorkspace(chat) || !paths::IsGitWorktreeIsolated(chat) ||
			    chat.imported_read_only || !chat.agent_run_id.empty() || !chat.goal_owner_chat_id.empty() || !chat.temporary_parent_chat_id.empty())
			{ error = "Choose an editable local Git worktree."; return false; }
			error = "Could not verify the linked worktree ownership.";
			const std::filesystem::path source = EffectiveSourceWorkspace(app, chat);
			if (IsSvnWorkspace(source)) { error = "Branch switching is unavailable for SVN."; return false; }
			const ChatFolder* folder = ChatDomainService().FindFolderById(app, chat.folder_id);
			std::error_code ec;
			const std::filesystem::path original = std::filesystem::canonical(source, ec);
			if (ec || !folder || std::filesystem::canonical(paths::PathFromUtf8(folder->directory), ec) != original || ec)
			{ error = "The Git source no longer matches the chat workspace."; return false; }
			worktree = paths::AbsolutePathNoThrow(paths::PathFromUtf8(chat.workspace_worktree_directory)).lexically_normal();
			const std::filesystem::path bucket = paths::AbsolutePathNoThrow(WorktreeRootForChat(app, source, "")).lexically_normal();
			if (worktree == bucket || !PathIsWithin(worktree, bucket))
			{ error = "The worktree is outside its owned repository directory."; return false; }
			for (std::filesystem::path current = worktree; ; current = current.parent_path())
			{
				if (paths::IsLinkOrReparsePointNoThrow(current) || !paths::IsDirectoryNoThrow(current))
				{ error = "The worktree ownership path changed."; return false; }
				if (current == paths::AbsolutePathNoThrow(app.data_root).lexically_normal()) break;
				if (current == current.parent_path()) return false;
			}
			const std::filesystem::path marker = worktree / ".git";
			if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(marker, ec)) || ec || paths::IsLinkOrReparsePointNoThrow(marker))
			{ error = "The workspace is not an owned linked Git worktree."; return false; }
			std::string common, expected_common, admin, top, actual_branch;
			if (!GitOutput(worktree, "rev-parse --path-format=absolute --git-common-dir", &common, &error) ||
			    !GitOutput(source, "rev-parse --path-format=absolute --git-common-dir", &expected_common, &error) ||
			    !GitOutput(worktree, "rev-parse --absolute-git-dir", &admin, &error) ||
			    !GitOutput(worktree, "rev-parse --show-toplevel", &top, &error) ||
			    !GitOutput(worktree, "symbolic-ref --quiet --short HEAD", &actual_branch, &error)) return false;
			const std::filesystem::path common_path = std::filesystem::canonical(paths::PathFromUtf8(common), ec);
			if (ec) return false;
			const std::filesystem::path expected_path = std::filesystem::canonical(paths::PathFromUtf8(expected_common), ec);
			if (ec) return false;
			const std::filesystem::path top_path = std::filesystem::canonical(paths::PathFromUtf8(top), ec);
			if (ec) return false;
			const std::filesystem::path physical_worktree = std::filesystem::canonical(worktree, ec);
			if (ec || common_path != expected_path || top_path != physical_worktree || actual_branch != chat.workspace_branch_name)
			{ error = "The worktree repository or recorded branch changed."; return false; }
			const std::filesystem::path admin_path = std::filesystem::canonical(paths::PathFromUtf8(admin), ec);
			if (ec || admin_path.parent_path() != common_path / "worktrees" || paths::IsLinkOrReparsePointNoThrow(paths::PathFromUtf8(admin))) return false;
			const std::filesystem::path backlink = admin_path / "gitdir";
			if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(backlink, ec)) || ec ||
			    paths::IsLinkOrReparsePointNoThrow(backlink) || std::filesystem::file_size(backlink, ec) > 8192 || ec) return false;
			const std::filesystem::path back_path = std::filesystem::canonical(paths::PathFromUtf8(strings::Trim(io::ReadTextFile(backlink))), ec);
			if (ec) return false;
			const std::filesystem::path marker_path = std::filesystem::canonical(marker, ec);
			if (ec || back_path != marker_path) { error = "The Git administrative backlink changed."; return false; }
			error.clear();
			return true;
		}

		bool OriginalWorktreeBranchPath(const AppState& app, const std::filesystem::path& source,
		    const std::filesystem::path& worktree, const std::string& branch, const std::string& chat_id)
		{
			std::filesystem::path physical;
			std::string error;
			const std::filesystem::path actual = paths::AbsolutePathNoThrow(worktree).lexically_normal();
			if (actual == paths::AbsolutePathNoThrow(WorktreeRootForChat(app, source, chat_id)).lexically_normal()) return false;
			return actual == paths::AbsolutePathNoThrow(WorktreeRootForChat(app, source, branch)).lexically_normal() ||
			    (PhysicalWorktreeRootForChat(app, source, branch, physical, error) && actual == paths::AbsolutePathNoThrow(physical).lexically_normal());
		}

		bool ClearWorktreeIsolation(AppState& app, ChatSession& chat, const GitWorktreeStatus& status, std::string* error_out)
		{
			const std::filesystem::path source = SourcePathFromStatusOrChat(app, chat, status);
			const std::filesystem::path repository = status.managed_repository ? ManagedRepositoryRoot(app, source, chat.id) : source;
			const std::filesystem::path worktree = WorktreePathFromStatus(status);
			const std::filesystem::path managed_root = app.data_root / "worktrees";
			if (RefuseSharedWorktreeMutation(app, chat, worktree, error_out)) return false;

			if (status.managed_repository && !PathIsWithin(repository, managed_root))
			{
				if (error_out != nullptr)
				{
					*error_out = "Refusing to remove a managed repository outside UAM's data directory.";
				}
				return false;
			}

			ChatSession cleared_chat = chat;
			cleared_chat.workspace_isolation_kind.clear();
			cleared_chat.workspace_source_directory.clear();
			cleared_chat.workspace_base_ref.clear();
			cleared_chat.workspace_branch_name.clear();
			cleared_chat.workspace_worktree_directory.clear();
			cleared_chat.updated_at = uam::time::TimestampNow();
			if (!SaveChat(app, cleared_chat, error_out))
			{
				return false;
			}

			if (!worktree.empty())
			{
				if (uam::paths::PathExistsNoThrow(worktree))
				{
					if (!GitCommand(repository, "worktree remove --force " + uam::shell::EscapeArg(uam::paths::Utf8PathString(worktree)), error_out))
					{
						SaveChat(app, chat, nullptr);
						return false;
					}
				}
			}

			const std::string branch_name = BranchNameFromStatusOrChat(status, chat);
			if (!branch_name.empty() && OriginalWorktreeBranchPath(app, source, worktree, branch_name, chat.id))
			{
				GitCommand(repository, "branch -D " + uam::shell::EscapeArg(branch_name));
			}

			if (status.managed_repository && uam::paths::PathExistsNoThrow(repository))
			{
				std::error_code ec;
				if (!uam::paths::RemoveAllNoThrow(repository, &ec))
				{
					chat = std::move(cleared_chat);
					if (error_out != nullptr)
					{
						*error_out = "Failed to remove managed repository: " + ec.message();
					}
					return false;
				}
			}

			chat = std::move(cleared_chat);
			return true;
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
				if (IsDirty(uam::paths::PathFromUtf8(repo_root), &source_dirty))
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

			const std::filesystem::path worktree_path = TrimmedPathOrEmpty(status.worktree_directory);
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

		bool IsCommitId(std::string_view value)
		{
			return (value.size() == 40 || value.size() == 64) && std::ranges::all_of(value, [](unsigned char c) { return std::isxdigit(c) != 0; });
		}
	} // namespace

	std::string RemoteWorktreeHostIdentity(const ExecutionHost& host)
	{
		return nlohmann::json::array({host.id, host.transport, host.ssh_alias, host.platform, host.architecture,
		    host.runner_version, host.runner_protocol_version, host.runner_directory, host.instruction_file,
		    host.instruction_text ? nlohmann::json(*host.instruction_text) : nlohmann::json(nullptr)}).dump();
	}

	std::string RemoteWorktreeStableHostIdentity(const ExecutionHost& host)
	{
		return nlohmann::json::array({host.id, host.transport, host.ssh_alias, host.platform}).dump();
	}

	namespace
	{
		std::string RemoteOperationToken()
		{
			return PlatformServicesFactory::Instance().process_service.GenerateUuid();
		}

		ExecutionHost CapturedRemoteWorktreeHost(const nlohmann::json& receipt)
		{
			const nlohmann::json identity = nlohmann::json::parse(receipt.at("remoteHost").get<std::string>());
			if (!identity.is_array() || identity.size() != 10) throw std::runtime_error("Invalid captured SSH worktree host");
			ExecutionHost host;
			host.id = identity.at(0).get<std::string>();
			host.transport = identity.at(1).get<std::string>();
			host.ssh_alias = identity.at(2).get<std::string>();
			host.platform = identity.at(3).get<std::string>();
			host.architecture = identity.at(4).get<std::string>();
			host.runner_version = identity.at(5).get<std::string>();
			host.runner_protocol_version = identity.at(6).get<int>();
			host.runner_directory = identity.at(7).get<std::string>();
			host.instruction_file = identity.at(8).get<std::string>();
			if (!identity.at(9).is_null()) host.instruction_text = identity.at(9).get<std::string>();
			return host;
		}

		void RemoveSettledRemoteWorktreeProcess(remote::RunnerClient& client, const std::string& operation_id, const std::string& token)
		{
			for (int attempt = 0; attempt < 2; ++attempt)
			{
				client.SetProcessControlToken(operation_id, token);
				std::string error;
				if (client.RemoveProcess(operation_id, &error)) return;
				client.SetProcessControlToken(operation_id, token);
				remote::ProcessPollResult process;
				bool missing = false;
				if (client.Connect(&error) && !client.PollProcess(operation_id, process, &error, {}, nullptr, nullptr, &missing) && missing) return;
			}
			diagnostics::Write("[SSH worktree] Workspace receipt is settled, but its exited helper record could not be removed. The recorded ownership token was preserved.");
		}

		bool RemoteGit(remote::RunnerClient& client, const ExecutionHost& host, const std::string& directory,
		    std::vector<std::string> arguments, std::string& output, std::string& error, std::stop_token stop,
		    int expected_exit = 0, const std::string& operation_id = {}, int* observed_exit_out = nullptr, const std::string& retained_control_token = {}, bool* start_request_attempted_out = nullptr)
		{
			if (observed_exit_out) *observed_exit_out = -1;
			std::vector<std::string> argv{"git", "--no-pager", "-c", "core.fsmonitor=false", "-c",
			    host.platform == "windows" || host.platform == "Windows" ? "core.hooksPath=NUL" : "core.hooksPath=/dev/null"};
			argv.insert(argv.end(), arguments.begin(), arguments.end());
			const ProcessExecutionResult result = client.ExecuteCommand(
			    operation_id.empty() ? "worktree-read-" + RemoteOperationToken() : operation_id,
			    paths::PathFromUtf8(directory), argv, 120000, stop, {}, retained_control_token, start_request_attempted_out);
			if (observed_exit_out && !result.canceled && !result.timed_out) *observed_exit_out = result.exit_code;
			if (result.canceled || result.timed_out || result.output_truncated || !result.error.empty() || result.exit_code != expected_exit || result.output.size() > 256 * 1024)
			{
				error = strings::NonEmptyOrFallback(result.error, "Remote Git could not verify this workspace.");
				return false;
			}
			output = strings::Trim(result.output);
			return true;
		}

		bool InspectRemoteDirectory(remote::RunnerClient& client, const std::string& path, std::string& canonical,
		    std::string& error, std::stop_token stop)
		{
			remote::PathInspection inspected;
			if (!client.InspectPath(path, false, inspected, &error, stop)) return false;
			if (!inspected.exists || !inspected.directory)
			{ error = "The remote workspace is not a safe existing directory."; return false; }
			canonical = inspected.path;
			return true;
		}

		std::string RemoteParent(const ExecutionHost& host, std::string path)
		{
			if (host.platform == "windows" || host.platform == "Windows") std::ranges::replace(path, '\\', '/');
			const std::size_t separator = path.find_last_of('/');
			if (separator == std::string::npos) return {};
			if (separator == 0 || (separator == 2 && path[1] == ':')) return path.substr(0, separator + 1);
			return path.substr(0, separator);
		}

		bool RemoteWorkspaceHasGitMetadata(remote::RunnerClient& client, const ExecutionHost& host,
		    const std::string& source, bool& found, std::string& error, std::stop_token stop)
		{
			found = false;
			std::string directory = source;
			for (int depth = 0; depth < 256 && !stop.stop_requested(); ++depth)
			{
				remote::PathInspection marker, head, objects;
				if (!client.InspectPath(execution_hosts::JoinRemotePath(host.platform, directory, ".git"), true, marker, &error, stop)) return false;
				if (marker.exists) { found = true; return true; }
				// Bare repositories also count as Git; broken metadata must never become a plain-folder fallback.
				if (!client.InspectPath(execution_hosts::JoinRemotePath(host.platform, directory, "HEAD"), true, head, &error, stop) ||
				    !client.InspectPath(execution_hosts::JoinRemotePath(host.platform, directory, "objects"), true, objects, &error, stop)) return false;
				if (head.exists && objects.exists) { found = true; return true; }
				if (head.exists || objects.exists)
				{
					remote::PathInspection refs, config;
					if (!client.InspectPath(execution_hosts::JoinRemotePath(host.platform, directory, "refs"), true, refs, &error, stop) ||
					    !client.InspectPath(execution_hosts::JoinRemotePath(host.platform, directory, "config"), true, config, &error, stop)) return false;
					if (refs.exists && config.exists) { found = true; return true; }
				}
				const std::string normalized = execution_hosts::NormalizeRemotePath(host.platform, directory);
				const std::string parent = RemoteParent(host, directory);
				const std::size_t server_end = normalized.find('/', 2);
				const bool share_root = normalized.starts_with("//") && server_end != std::string::npos && normalized.find('/', server_end + 1) == std::string::npos;
				if (parent.empty() || execution_hosts::RemotePathsMatch(host.platform, parent, directory) || share_root) return true;
				directory = parent;
			}
			error = "The remote workspace ancestry could not be safely inspected.";
			return false;
		}

		bool RemoteGitIdentity(remote::RunnerClient& client, const ExecutionHost& host, const std::string& source,
		    const std::string& common, const ChatSession& chat, std::string& error, std::stop_token stop)
		{
			std::string directory, output;
			if (!IsCommitId(chat.workspace_base_ref) || chat.workspace_branch_name.empty() ||
			    !InspectRemoteDirectory(client, source, directory, error, stop) || !execution_hosts::RemotePathsMatch(host.platform, directory, source) ||
			    !InspectRemoteDirectory(client, common, directory, error, stop) || !execution_hosts::RemotePathsMatch(host.platform, directory, common) ||
			    !InspectRemoteDirectory(client, chat.workspace_worktree_directory, directory, error, stop) ||
			    !execution_hosts::RemotePathsMatch(host.platform, directory, chat.workspace_worktree_directory)) return false;
			remote::PathInspection marker;
			if (!client.InspectPath(execution_hosts::JoinRemotePath(host.platform, directory, ".git"), false, marker, &error, stop) || !marker.regular_file)
			{ if (error.empty()) error = "The remote worktree Git marker is unavailable."; return false; }
			if (!RemoteGit(client, host, directory, {"rev-parse", "--show-toplevel"}, output, error, stop) ||
			    !execution_hosts::RemotePathsMatch(host.platform, output, directory) ||
			    !RemoteGit(client, host, directory, {"rev-parse", "--path-format=absolute", "--git-common-dir"}, output, error, stop) ||
			    !execution_hosts::RemotePathsMatch(host.platform, output, common) ||
			    !RemoteGit(client, host, source, {"rev-parse", "--path-format=absolute", "--git-common-dir"}, output, error, stop) ||
			    !execution_hosts::RemotePathsMatch(host.platform, output, common) ||
			    !RemoteGit(client, host, directory, {"symbolic-ref", "--short", "HEAD"}, output, error, stop) || output != chat.workspace_branch_name) return false;
			std::string head, reference, admin;
			if (!RemoteGit(client, host, directory, {"rev-parse", "HEAD"}, head, error, stop) || !IsCommitId(head) ||
			    !RemoteGit(client, host, source, {"rev-parse", "--verify", "--end-of-options", "refs/heads/" + chat.workspace_branch_name}, reference, error, stop) || reference != head ||
			    !RemoteGit(client, host, directory, {"merge-base", "--is-ancestor", chat.workspace_base_ref, "HEAD"}, output, error, stop) ||
			    !RemoteGit(client, host, directory, {"rev-parse", "--absolute-git-dir"}, admin, error, stop) ||
			    !InspectRemoteDirectory(client, admin, output, error, stop) ||
			    !execution_hosts::RemotePathsMatch(host.platform, RemoteParent(host, output), execution_hosts::JoinRemotePath(host.platform, common, "worktrees"))) return false;
			remote::PathInspection backlink;
			if (!client.InspectPath(execution_hosts::JoinRemotePath(host.platform, admin, "gitdir"), false, backlink, &error, stop) || !backlink.regular_file ||
			    !client.ReadTextFile(paths::PathFromUtf8(backlink.path), output, &error, stop) ||
			    !execution_hosts::RemotePathsMatch(host.platform, strings::Trim(output), marker.path)) return false;
			if (!RemoteGit(client, host, source, {"worktree", "list", "--porcelain"}, output, error, stop)) return false;
			std::istringstream lines(output);
			std::string line;
			int matches = 0;
			bool target = false, branch_matches = false, head_matches = false, unavailable = false, unsafe_target = false;
			const auto finish = [&]()
			{
				if (target && unavailable) unsafe_target = true;
				if (target && branch_matches && head_matches && !unavailable) ++matches;
				target = branch_matches = head_matches = unavailable = false;
			};
			while (std::getline(lines, line))
			{
				if (line.empty()) { finish(); continue; }
				if (line.starts_with("worktree ")) target = execution_hosts::RemotePathsMatch(host.platform, line.substr(9), directory);
				if (line.starts_with("HEAD ")) head_matches = line.substr(5) == head;
				if (line.starts_with("branch ")) branch_matches = line.substr(7) == "refs/heads/" + chat.workspace_branch_name;
				if (line == "locked" || line.starts_with("locked ") || line == "prunable" || line.starts_with("prunable ")) unavailable = true;
			}
			finish();
			if (unsafe_target) { error = "The remote worktree is initializing, locked or prunable. Its files were preserved."; return false; }
			if (matches != 1) { error = "The remote worktree registration changed."; return false; }
			return true;
		}

		GitWorktreeOperationResult PrepareRemoteWorktree(AppState& snapshot, ChatSession& chat,
		    const GitWorktreeIntentRecorder& record, std::stop_token stop, bool explicit_create)
		{
			GitWorktreeOperationResult result;
			const ExecutionHost* host = execution_hosts::Find(snapshot.settings.execution_hosts, chat.execution_host_id);
			if (!host || !remote::CanAttemptRunnerConnection(*host) || host->runner_protocol_version < 3)
			{ result.message = "Remote worktrees require an available protocol-3 SSH helper."; return result; }
			if (!record) { result.message = "Remote worktrees require owned asynchronous preparation."; return result; }
			if (chat.svn_update_pending)
			{ result.message = "SSH worktree creation supports existing Git repositories only; SVN preparation is unavailable."; return result; }
			remote::RunnerClient client(PlatformServicesFactory::Instance().process_service,
			    remote::SshBridgeArgv(host->ssh_alias, host->platform, host->runner_version, host->runner_directory, host->runner_protocol_version), host->runner_version, host->runner_protocol_version);
			std::string requested, source, common, output;
			bool git_metadata = false;
			if (!InspectRemoteDirectory(client, chat.workspace_directory, requested, result.message, stop) ||
			    !RemoteWorkspaceHasGitMetadata(client, *host, requested, git_metadata, result.message, stop)) return result;
			if (!git_metadata)
			{
				if (explicit_create || chat.git_base_branch_override || chat.workspace_isolation_kind != paths::kPendingGitWorktreeIsolationKind ||
				    !chat.workspace_source_directory.empty() || !chat.workspace_worktree_directory.empty() || !chat.workspace_branch_name.empty() || !chat.workspace_base_ref.empty())
				{ result.message = "Creating an SSH Git worktree requires a Git repository."; return result; }
				chat.workspace_isolation_kind.clear();
				chat.updated_at = time::TimestampNow();
				result.ok = true;
				result.status.source_directory = requested;
				result.message = "Git preparation skipped for an ordinary SSH workspace.";
				return result;
			}
			if (!RemoteGit(client, *host, requested, {"rev-parse", "--show-toplevel"}, output, result.message, stop) ||
			    !InspectRemoteDirectory(client, output, source, result.message, stop) ||
			    !RemoteGit(client, *host, source, {"rev-parse", "--path-format=absolute", "--git-common-dir"}, output, result.message, stop) ||
			    !InspectRemoteDirectory(client, output, common, result.message, stop)) return result;
			remote::PathInspection marker;
			if (!client.InspectPath(execution_hosts::JoinRemotePath(host->platform, source, ".git"), false, marker, &result.message, stop) ||
			    (!marker.regular_file && !marker.directory)) return result;
			const std::string base = strings::Trim(chat.git_base_branch_override.value_or(snapshot.settings.git_default_base_branch));
			if ((!base.empty() && !RemoteGit(client, *host, source, {"check-ref-format", "--branch", base}, output, result.message, stop)) ||
			    !RemoteGit(client, *host, source, {"rev-parse", "--verify", "--end-of-options", (base.empty() ? "HEAD" : "refs/heads/" + base) + "^{commit}"}, output, result.message, stop) || !IsCommitId(output)) return result;
			GitWorktreeCreationIntent intent;
			intent.source = paths::PathFromUtf8(source);
			intent.repository = paths::PathFromUtf8(common);
			intent.base_sha = output;
			intent.operation_id = "worktree-create-" + RemoteOperationToken();
			intent.control_token = RemoteOperationToken();
			if (intent.operation_id == "worktree-create-" || intent.control_token.empty())
			{ result.message = "Could not allocate remote worktree ownership."; return result; }
			const std::string suffix = hashing::Hex64(hashing::Fnv1a64(chat.id + ":" + intent.operation_id));
			intent.branch = "uam/" + suffix;
			intent.worktree = paths::PathFromUtf8(execution_hosts::JoinRemotePath(host->platform, RemoteParent(*host, source), "uam-worktree-" + suffix));
			remote::PathInspection target;
			if (!client.InspectPath(paths::Utf8PathString(intent.worktree), true, target, &result.message, stop) || target.exists ||
			    !RemoteGit(client, *host, source, {"show-ref", "--verify", "--quiet", "refs/heads/" + intent.branch}, output, result.message, stop, 1))
			{ if (result.message.empty()) result.message = "The remote worktree destination or branch is occupied."; return result; }
			if (stop.stop_requested() || !record(intent, result.message)) return result;
			client.SetProcessControlToken(intent.operation_id, intent.control_token);
			if (!client.InspectPath(paths::Utf8PathString(intent.worktree), true, target, &result.message, stop) || target.exists)
			{
				if (result.message.empty()) result.message = "The remote destination changed before Git started. Its files were preserved.";
				intent.operation_state = "failed";
				std::string receipt_error;
				if (!record(intent, receipt_error)) result.message += " " + receipt_error;
				return result;
			}
			int observed_exit = -1;
			bool start_request_attempted = false;
			if (!RemoteGit(client, *host, source, {"worktree", "add", "-b", intent.branch, "--", paths::Utf8PathString(intent.worktree), intent.base_sha}, output, result.message, stop, 0, intent.operation_id, &observed_exit, intent.control_token, &start_request_attempted))
			{
				std::string inspection_error;
				if (!start_request_attempted || (observed_exit > 0 && client.InspectPath(paths::Utf8PathString(intent.worktree), true, target, &inspection_error, stop) && !target.exists))
				{
					intent.operation_state = "failed";
					std::string receipt_error;
					if (record(intent, receipt_error) && start_request_attempted)
						RemoveSettledRemoteWorktreeProcess(client, intent.operation_id, intent.control_token);
					if (!receipt_error.empty()) result.message += " " + receipt_error;
				}
				return result;
			}
			chat.workspace_isolation_kind = paths::kGitWorktreeIsolationKind;
			chat.workspace_source_directory = source;
			chat.workspace_worktree_directory = paths::Utf8PathString(intent.worktree);
			chat.workspace_branch_name = intent.branch;
			chat.workspace_base_ref = intent.base_sha;
			chat.updated_at = time::TimestampNow();
			if (!RemoteGitIdentity(client, *host, source, common, chat, result.message, stop)) return result;
			intent.operation_state = "completed";
			if (!record(intent, result.message)) return result;
			RemoveSettledRemoteWorktreeProcess(client, intent.operation_id, intent.control_token);
			result.ok = true;
			result.message = "Created an isolated SSH Git worktree.";
			result.status = {.is_git_repository = true, .isolated = true, .source_directory = source,
			    .worktree_directory = chat.workspace_worktree_directory, .branch_name = chat.workspace_branch_name, .base_ref = chat.workspace_base_ref};
			if (snapshot.settings.git_pull_before_chat)
			{
				result.status.warning = "Pull before chat was skipped because SSH pulling is unavailable; the current Git base was used.";
				diagnostics::Write("[SSH worktree] " + result.status.warning);
			}
			return result;
		}
	}

	bool GitWorktreeService::ValidateRemotePreparedWorkspace(const ExecutionHost& host, const std::filesystem::path& root,
	    const std::filesystem::path& receipt, const ChatSession& metadata, std::string& error, std::stop_token stop, bool* definite_failure_out) const
	{
		if (definite_failure_out) *definite_failure_out = false;
		try
		{
			if (host.runner_protocol_version < 3 || !remote::CanAttemptRunnerConnection(host) ||
			    !paths::IsSameOrInsideRoot(root, receipt) || paths::IsLinkOrReparsePointNoThrow(receipt) || paths::IsLinkOrReparsePointNoThrow(receipt.parent_path())) return false;
			if (metadata.workspace_isolation_kind.empty() && metadata.workspace_source_directory.empty() && metadata.workspace_worktree_directory.empty() &&
			    metadata.workspace_branch_name.empty() && metadata.workspace_base_ref.empty() && !metadata.git_base_branch_override && !paths::PathExistsNoThrow(receipt))
			{
				remote::RunnerClient client(PlatformServicesFactory::Instance().process_service,
				    remote::SshBridgeArgv(host.ssh_alias, host.platform, host.runner_version, host.runner_directory, host.runner_protocol_version), host.runner_version, host.runner_protocol_version);
				std::string source;
				bool git_metadata = false;
				return InspectRemoteDirectory(client, metadata.workspace_directory, source, error, stop) &&
				    RemoteWorkspaceHasGitMetadata(client, host, source, git_metadata, error, stop) && !git_metadata;
			}
			std::string bytes;
			if (!io::TryReadTextFile(receipt, bytes, 64 * 1024)) return false;
			nlohmann::json saved = nlohmann::json::parse(bytes);
			if (saved.at("stableHost") != RemoteWorktreeStableHostIdentity(host) || saved.at("chatId") != metadata.id ||
			    saved.at("createdAt") != metadata.created_at || saved.at("source") != metadata.workspace_source_directory ||
			    saved.at("worktree") != metadata.workspace_worktree_directory || saved.at("branch") != metadata.workspace_branch_name ||
			    saved.at("baseSha") != metadata.workspace_base_ref || !paths::IsGitWorktreeIsolated(metadata)) return false;
			const std::string operation_id = saved.at("operationId").get<std::string>();
			const std::string token = saved.at("controlToken").get<std::string>();
			const std::string state = saved.value("operationState", "pending");
			if (state != "pending" && state != "completed")
			{ error = "The SSH worktree operation has no verified completion."; return false; }
			if (operation_id.empty() || operation_id.size() > 128 || token.empty() || token.size() > 128 ||
			    std::ranges::any_of(operation_id + token, [](unsigned char c) { return !std::isalnum(c) && c != '-'; })) return false;
			const std::string suffix = hashing::Hex64(hashing::Fnv1a64(metadata.id + ":" + operation_id));
			const std::string expected = execution_hosts::JoinRemotePath(host.platform, RemoteParent(host, metadata.workspace_source_directory), "uam-worktree-" + suffix);
			if (metadata.workspace_branch_name != "uam/" + suffix || !execution_hosts::RemotePathsMatch(host.platform, expected, metadata.workspace_worktree_directory)) return false;
			const ExecutionHost captured = CapturedRemoteWorktreeHost(saved);
			if (RemoteWorktreeStableHostIdentity(captured) != RemoteWorktreeStableHostIdentity(host)) return false;
			const ExecutionHost& inspection_host = state == "completed" ? host : captured;
			if (inspection_host.runner_protocol_version < 3 || !remote::CanAttemptRunnerConnection(inspection_host)) return false;
			remote::RunnerClient client(PlatformServicesFactory::Instance().process_service,
			    remote::SshBridgeArgv(inspection_host.ssh_alias, inspection_host.platform, inspection_host.runner_version, inspection_host.runner_directory, inspection_host.runner_protocol_version), inspection_host.runner_version, inspection_host.runner_protocol_version);
			remote::ProcessPollResult process;
			if (!client.Connect(&error, stop)) return false;
			if (state == "pending")
			{
				client.SetProcessControlToken(operation_id, token);
				const bool observed = client.PollProcess(operation_id, process, &error, [stop]() { return stop.stop_requested(); });
				if (!observed || process.running || process.exit_code < 0)
				{ error = "The original remote worktree operation is still running or its exit is unconfirmed. Its files were preserved."; return false; }
				if (process.exit_code != 0)
				{
					remote::PathInspection target;
					std::string inspection_error;
					error = "The original remote Git operation exited unsuccessfully. Its files and branch were preserved.";
					if (client.InspectPath(metadata.workspace_worktree_directory, true, target, &inspection_error, stop) && !target.exists)
					{
						saved["operationState"] = "failed";
						if (!io::WriteTextFile(receipt, saved.dump()))
						{ error += " Could not save its confirmed failure."; return false; }
						if (definite_failure_out) *definite_failure_out = true;
						RemoveSettledRemoteWorktreeProcess(client, operation_id, token);
					}
					return false;
				}
			}
			error.clear();
			if (!RemoteGitIdentity(client, inspection_host, metadata.workspace_source_directory, saved.at("repository").get<std::string>(), metadata, error, stop))
			{ if (error.empty()) error = "The saved SSH worktree no longer matches its owned receipt."; return false; }
			if (state == "pending")
			{
				saved["operationState"] = "completed";
				if (!io::WriteTextFile(receipt, saved.dump()))
				{ error = "Could not save verified SSH worktree completion. Its files were preserved."; return false; }
				RemoveSettledRemoteWorktreeProcess(client, operation_id, token);
			}
			return true;
		}
		catch (const std::exception&) { error = "The SSH worktree ownership receipt could not be verified."; return false; }
	}

	GitWorktreeStatus GitWorktreeService::Status(const AppState& app, const ChatSession& chat) const
	{
		GitWorktreeStatus status;
		if (!uam::paths::IsControllerLocalWorkspace(chat))
		{
			const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
			if (!host || host->runner_protocol_version < 3 || !remote::CanAttemptRunnerConnection(*host))
			{ status.error = "SSH Git worktrees require an available protocol-3 helper."; return status; }
			remote::RunnerClient client(PlatformServicesFactory::Instance().process_service,
			    remote::SshBridgeArgv(host->ssh_alias, host->platform, host->runner_version, host->runner_directory, host->runner_protocol_version), host->runner_version, host->runner_protocol_version);
			const std::string requested = paths::HasGitWorktreeSource(chat) ? chat.workspace_source_directory : paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, chat));
			std::string source, output;
			if (!InspectRemoteDirectory(client, requested, source, status.error, {}) ||
			    !RemoteGit(client, *host, source, {"rev-parse", "--show-toplevel"}, output, status.error, {}) ||
			    !InspectRemoteDirectory(client, output, status.source_directory, status.error, {})) return status;
			status.is_git_repository = true;
			if (!RemoteGit(client, *host, status.source_directory, {"status", "--porcelain"}, output, status.error, {})) return status;
			status.source_dirty = !output.empty();
			status.isolated = paths::IsGitWorktreeIsolated(chat);
			status.worktree_directory = chat.workspace_worktree_directory;
			status.branch_name = chat.workspace_branch_name;
			status.base_ref = chat.workspace_base_ref;
			if (status.isolated)
			{
				remote::PathInspection worktree;
				if (!client.InspectPath(status.worktree_directory, true, worktree, &status.error)) return status;
				if (!worktree.exists) { status.worktree_missing = true; return status; }
				if (!worktree.directory) { status.error = "The remote worktree is not a directory."; return status; }
				if (!RemoteGit(client, *host, worktree.path, {"status", "--porcelain"}, output, status.error, {})) return status;
				status.worktree_dirty = !output.empty();
			}
			return status;
		}
		status.isolated = uam::paths::IsGitWorktreeIsolated(chat);
		status.source_directory = uam::strings::Trim(chat.workspace_source_directory);
		status.worktree_directory = uam::strings::Trim(chat.workspace_worktree_directory);
		status.branch_name = uam::strings::Trim(chat.workspace_branch_name);
		status.base_ref = uam::strings::Trim(chat.workspace_base_ref);

		const std::filesystem::path source_candidate = EffectiveSourceWorkspace(app, chat);
		status.is_svn_workspace = app.settings.svn_enabled && IsSvnWorkspace(source_candidate);
		if (!app.settings.git_enabled)
		{
			status.warning = "Git is disabled in Settings.";
			status.worktree_missing = status.isolated && !uam::paths::IsDirectoryNoThrow(TrimmedPathOrEmpty(chat.workspace_worktree_directory));
			return status;
		}
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

	bool GitWorktreeService::ListChatBranches(const AppState& app, const ChatSession& chat, std::vector<GitChatBranch>& branches, bool& svn, std::string& error) const
	{
		branches.clear();
		svn = paths::IsControllerLocalWorkspace(chat) && IsSvnWorkspace(EffectiveSourceWorkspace(app, chat));
		if (svn) return true;
		std::filesystem::path worktree;
		if (!VerifiedBranchSelectionWorkspace(app, chat, worktree, error)) return false;
		std::string refs;
		if (!GitOutput(worktree, "for-each-ref " + shell::EscapeArg("--format=%(refname)") + " refs/heads", &refs, &error)) return false;
		std::istringstream lines(refs);
		std::string ref;
		while (std::getline(lines, ref))
		{
			ref = strings::Trim(ref);
			if (!ref.starts_with("refs/heads/")) { error = "Could not read local Git branches."; return false; }
			const std::string name = ref.substr(11);
			branches.push_back({name, name == chat.workspace_branch_name});
		}
		return true;
	}

		bool GitWorktreeService::ValidateChatBranchSelection(const AppState& app, const ChatSession& chat,
		    const std::string& branch, bool create, std::string& error) const
		{
		if (branch.empty() || branch.size() > 255 || branch.front() == '-' || branch == "HEAD" ||
		    std::ranges::any_of(branch, [](unsigned char c) { return c < 32 || c == 127; }))
		{ error = "Choose a valid local Git branch name."; return false; }
		std::filesystem::path worktree;
		if (!VerifiedBranchSelectionWorkspace(app, chat, worktree, error) ||
		    RefuseSharedWorktreeMutation(app, chat, worktree, &error)) return false;
		std::string checked;
		if (!GitOutput(worktree, "check-ref-format --branch " + shell::EscapeArg(branch), &checked, &error) || checked != branch)
		{ error = "Choose a literal local Git branch name."; return false; }
		bool dirty = false;
		if (!IsDirty(worktree, &dirty, &error)) return false;
		if (dirty) { error = "Commit or discard worktree changes before switching branches."; return false; }

			const ProcessExecutionResult ref = RunCommand(BuildGitCommandInDirectory(worktree,
			    "show-ref --verify --quiet " + shell::EscapeArg("refs/heads/" + branch)), 10000);
			if ((!ref.ok && ref.exit_code != 1) || ref.timed_out || ref.canceled || ref.output_truncated || !ref.error.empty() || (ref.exit_code != 0 && ref.exit_code != 1))
			{ error = "Could not verify the selected local branch."; return false; }
			if ((create && ref.exit_code == 0) || (!create && ref.exit_code == 1))
			{ error = create ? "That local branch already exists." : "The selected local branch no longer exists."; return false; }
			std::string worktrees;
			if (!GitOutput(worktree, "worktree list --porcelain", &worktrees, &error)) return false;
			if (!create && branch != chat.workspace_branch_name &&
			    ("\n" + worktrees + "\n").find("\nbranch refs/heads/" + branch + "\n") != std::string::npos)
			{ error = "The selected branch is already checked out in another worktree."; return false; }
			return true;
		}

	bool GitWorktreeService::SwitchChatBranch(AppState& app, ChatSession& chat, const std::string& branch, bool create, std::string& error) const
	{
		if (chat.acp_queue_held || !chat.acp_queued_prompts.empty())
		{ error = "Finish or resolve queued input before switching branches."; return false; }
		if (AcpStopInProgress(app, chat.id) || IsWorktreeLaunchPreparationPending(app, chat.id) || app.git_port_requests.contains(chat.id) ||
		    app.pending_chat_save_at_by_chat_id.contains(chat.id) || app.pending_chat_save_tasks.contains(chat.id))
		{ error = "Wait for the chat runtime and history save to finish."; return false; }
		for (const std::unique_ptr<AcpSessionState>& session : app.acp_sessions)
			if (session && session->chat_id == chat.id && (session->running || session->local_stop_pending || session->remote_stop_pending || session->queue_held || !session->queued_user_prompts.empty()))
			{ error = "Stop the chat runtime before switching branches."; return false; }
		for (const std::unique_ptr<CliTerminalState>& terminal : app.cli_terminals)
			if (terminal && terminal->running && CliTerminalMatchesChatId(*terminal, chat.id))
			{ error = "Stop the chat terminal before switching branches."; return false; }
		if (!ValidateChatBranchSelection(app, chat, branch, create, error)) return false;
		const std::filesystem::path worktree = paths::PathFromUtf8(chat.workspace_worktree_directory);
		if (!create && branch == chat.workspace_branch_name) return true;
		const std::optional<std::string> revision = ChatRepository::ChatStorageRevision(app.data_root, chat.id);
		const std::optional<ChatSession> persisted = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
		if (!revision || !persisted || ChatRepository::ChatStorageRevision(app.data_root, chat.id) != revision ||
		    persisted->workspace_worktree_directory != chat.workspace_worktree_directory || persisted->workspace_branch_name != chat.workspace_branch_name ||
		    persisted->workspace_source_directory != chat.workspace_source_directory || persisted->workspace_base_ref != chat.workspace_base_ref ||
		    persisted->provider_id != chat.provider_id || persisted->execution_host_id != chat.execution_host_id ||
		    persisted->native_session_id != chat.native_session_id || persisted->updated_at != chat.updated_at)
		{ error = "Chat metadata changed before branch switching."; return false; }
		const std::string previous_branch = chat.workspace_branch_name;
		if (!GitCommand(worktree, std::string("switch --no-guess --no-overwrite-ignore ") + (create ? "-c " : "") + shell::EscapeArg(branch), &error)) return false;
		ChatSession switched = *persisted;
		switched.workspace_branch_name = branch;
		switched.updated_at = time::TimestampNow();
		// Preserve the original integration base and history; checkout only changes the active branch.
		const std::shared_ptr<PreparedChatSave> prepared = ChatRepository::PrepareChatSave(app.data_root, switched, *revision);
		if (!prepared || ChatRepository::ChatStorageRevision(app.data_root, chat.id) != revision ||
		    !ChatRepository::PublishPreparedChatSave(prepared, true, nullptr,
		        [&app, &chat, &revision]() { return ChatRepository::ChatStorageRevision(app.data_root, chat.id) == revision; }))
		{
			error = "Failed to persist branch selection.";
			std::string rollback_error;
			if (!GitCommand(worktree, "switch --no-guess --no-overwrite-ignore " + shell::EscapeArg(previous_branch), &rollback_error))
				error = "Chat metadata could not be saved and Git could not restore the previous branch. Inspect this worktree before retrying.";
			return false;
		}
		chat.workspace_branch_name = switched.workspace_branch_name;
		chat.updated_at = switched.updated_at;
		return true;
	}

	bool ValidatePendingGitBaseOverrideScope(const AppState& app, const ChatSession& chat, std::string& error)
	{
		if (chat.workspace_isolation_kind != paths::kPendingGitWorktreeIsolationKind || !chat.git_base_branch_override) return true;
		if (!app.settings.git_enabled || !app.settings.git_auto_worktree ||
		    chat.imported_read_only || !chat.agent_run_id.empty() || !chat.goal_owner_chat_id.empty() ||
		    !chat.temporary_parent_chat_id.empty() || !chat.native_session_id.empty() || chat.routing_mode != "explicit")
		{
			error = "The selected Git base requires a new chat with automatic worktree creation.";
			return false;
		}
		return true;
	}

	std::string ManagedSnapshotDisabledReason(const AppState& app, const ChatSession& chat)
	{
		if ((app.settings.git_snapshot_svn && app.settings.git_snapshot_plain_folders) ||
		    !paths::IsControllerLocalWorkspace(chat) || paths::IsGitWorktreeIsolated(chat)) return {};
		const LocalSnapshotSource source = ClassifyLocalSnapshotSource(EffectiveSourceWorkspace(app, chat));
		if (source == LocalSnapshotSource::Svn && !app.settings.git_snapshot_svn)
			return "Managed Git snapshots for SVN workspaces are disabled in VCS settings.";
		if (source == LocalSnapshotSource::Plain && !app.settings.git_snapshot_plain_folders)
			return "Managed Git snapshots for plain folders are disabled in VCS settings.";
		return {};
	}

	std::string ManagedSnapshotBypassError(const AppState& app, const ChatSession& chat)
	{
		if (!chat.workspace_source_directory.empty() || !chat.workspace_worktree_directory.empty() ||
		    !chat.workspace_branch_name.empty() || !chat.workspace_base_ref.empty())
			return "Existing snapshot ownership must be resolved before using the original workspace. Its files were preserved.";
		const std::filesystem::path directory = app.data_root / ".worktree-launch";
		std::error_code error;
		const std::filesystem::file_status status = std::filesystem::symlink_status(directory, error);
		if (error == std::errc::no_such_file_or_directory || status.type() == std::filesystem::file_type::not_found) return {};
		if (error || !std::filesystem::is_directory(status)) return "Snapshot ownership storage could not be verified. Its files were preserved.";
		const std::string prefix = hashing::Hex64(hashing::Fnv1a64(chat.id)) + "-";
		std::filesystem::directory_iterator iterator(directory, error), end;
		while (!error && iterator != end)
		{
			const std::filesystem::path receipt = iterator->path();
			if (receipt.filename().string().starts_with(prefix) && receipt.extension() == ".json")
				return "Interrupted snapshot creation must be resolved before using the original workspace. Its files were preserved.";
			iterator.increment(error);
		}
		return error ? "Snapshot ownership storage could not be verified. Its files were preserved." : std::string{};
	}

	bool GitWorktreeService::PrepareForFirstLaunch(AppState& app, ChatSession& chat, std::string* error_out) const
	{
		std::string scope_error;
		if (!ValidatePendingGitBaseOverrideScope(app, chat, scope_error))
		{ if (error_out) *error_out = scope_error; return false; }
		if (chat.workspace_isolation_kind != uam::paths::kPendingGitWorktreeIsolationKind ||
		    !uam::paths::IsControllerLocalWorkspace(chat) || chat.imported_read_only ||
		    !chat.agent_run_id.empty() || !chat.goal_owner_chat_id.empty() ||
		    !chat.temporary_parent_chat_id.empty()) return true;
		if (!app.settings.git_enabled || !app.settings.git_auto_worktree ||
		    (!chat.git_base_branch_override && !ManagedSnapshotDisabledReason(app, chat).empty()))
		{
			if (!ManagedSnapshotDisabledReason(app, chat).empty())
			{
				const std::string ownership_error = ManagedSnapshotBypassError(app, chat);
				if (!ownership_error.empty()) { if (error_out) *error_out = ownership_error; return false; }
			}
			// Only an uncreated default is cleared; existing isolated paths remain owned.
			chat.workspace_isolation_kind.clear();
			return true;
		}
		const GitWorktreeOperationResult result = CreateForChat(app, chat);
		if (!result.ok && error_out != nullptr)
		{
			*error_out = "Could not create this chat's worktree. Retry after resolving: " + result.message;
		}
		return result.ok;
	}

	GitWorktreeOperationResult GitWorktreeService::CreateForChat(AppState& app, ChatSession& chat) const
	{
		return CreateForChatCore(app, chat, true, {}, {});
	}

	GitWorktreeOperationResult GitWorktreeService::PrepareForChat(AppState& snapshot, ChatSession& metadata, const GitWorktreeIntentRecorder& record, std::stop_token stop, bool explicit_create) const
	{
		if (!record) return {.message = "Workspace ownership recording is required."};
		if (!paths::IsControllerLocalWorkspace(metadata)) return PrepareRemoteWorktree(snapshot, metadata, record, stop, explicit_create);
		if (!explicit_create && !metadata.git_base_branch_override && !ManagedSnapshotDisabledReason(snapshot, metadata).empty())
		{
			const std::string ownership_error = ManagedSnapshotBypassError(snapshot, metadata);
			if (!ownership_error.empty()) return {.message = ownership_error};
			if (metadata.workspace_isolation_kind == paths::kPendingGitWorktreeIsolationKind) metadata.workspace_isolation_kind.clear();
			return {.ok = true};
		}
		return CreateForChatCore(snapshot, metadata, false, record, stop);
	}

	bool GitWorktreeService::ValidatePreparedWorkspace(const std::filesystem::path& root, const std::filesystem::path& receipt, const ChatSession& metadata, std::string& error, std::stop_token stop) const
	{
		try
		{
			if (uam::paths::IsLinkOrReparsePointNoThrow(receipt) || !uam::paths::IsSameOrInsideRoot(root, receipt)) return false;
			std::string receipt_text;
			if (!uam::io::TryReadTextFile(receipt, receipt_text, 64 * 1024))
			{
				error = "Prepared workspace ownership receipt could not be read within its size limit. The workspace was preserved.";
				return false;
			}
			const nlohmann::json record = nlohmann::json::parse(receipt_text);
			const std::filesystem::path repository = uam::paths::PathFromUtf8(record.at("repository").get<std::string>());
			const std::filesystem::path worktree = uam::paths::PathFromUtf8(metadata.workspace_worktree_directory);
			AppState snapshot;
			snapshot.data_root = root;
			const std::filesystem::path source = uam::paths::PathFromUtf8(metadata.workspace_source_directory);
			const std::filesystem::path expected_repository = record.at("managedRepository").get<bool>() ? ManagedRepositoryRoot(snapshot, source, metadata.id) : source;
			if (repository != expected_repository || metadata.workspace_isolation_kind != uam::paths::kGitWorktreeIsolationKind) return false;
			if (record.at("chatId") != metadata.id || record.at("createdAt") != metadata.created_at ||
			    record.at("source") != metadata.workspace_source_directory || record.at("worktree") != metadata.workspace_worktree_directory ||
			    record.at("branch") != metadata.workspace_branch_name || record.at("baseSha") != metadata.workspace_base_ref ||
			    !uam::paths::IsSameOrInsideRoot(root, worktree)) return false;
			if (uam::paths::IsLinkOrReparsePointNoThrow(root) || uam::paths::IsLinkOrReparsePointNoThrow(repository) ||
			    uam::paths::IsLinkOrReparsePointNoThrow(repository / ".git") || uam::paths::IsLinkOrReparsePointNoThrow(worktree / ".git")) return false;
			const std::filesystem::path owned_root = uam::paths::NormalizeExistingOrAbsolutePath(root);
			std::vector<std::filesystem::path> owned_artifacts{worktree, receipt};
			if (record.at("managedRepository").get<bool>()) owned_artifacts.push_back(repository);
			for (const std::filesystem::path& artifact : owned_artifacts)
			{
				for (std::filesystem::path path = uam::paths::AbsolutePathNoThrow(artifact); !path.empty(); path = path.parent_path())
				{
					if (uam::paths::IsLinkOrReparsePointNoThrow(path)) return false;
					if (uam::paths::NormalizeExistingOrAbsolutePath(path) == owned_root) break;
					if (path == path.parent_path() || !uam::paths::IsSameOrInsideRoot(root, path)) return false;
				}
			}

			std::string common, expected_common, top_level, branch, head, reference;
			if (!GitOutput(worktree, "rev-parse --path-format=absolute --git-common-dir", &common, &error, stop) ||
			    !GitOutput(repository, "rev-parse --path-format=absolute --git-common-dir", &expected_common, &error, stop) ||
			    !GitOutput(worktree, "rev-parse --show-toplevel", &top_level, &error, stop) ||
			    !GitOutput(worktree, "symbolic-ref --short HEAD", &branch, &error, stop) ||
			    !GitOutput(worktree, "rev-parse HEAD", &head, &error, stop) ||
			    !GitOutput(repository, "rev-parse --verify --end-of-options " + uam::shell::EscapeArg("refs/heads/" + metadata.workspace_branch_name), &reference, &error, stop)) return false;
			return uam::paths::NormalizeExistingOrAbsolutePath(uam::paths::PathFromUtf8(common)) == uam::paths::NormalizeExistingOrAbsolutePath(uam::paths::PathFromUtf8(expected_common)) &&
			    uam::paths::NormalizeExistingOrAbsolutePath(uam::paths::PathFromUtf8(top_level)) == uam::paths::NormalizeExistingOrAbsolutePath(worktree) &&
			    branch == metadata.workspace_branch_name && head == metadata.workspace_base_ref && reference == head;
		}
		catch (const std::exception&) { error = "Prepared workspace ownership could not be verified."; return false; }
	}

	GitWorktreeOperationResult GitWorktreeService::CreateForChatCore(AppState& app, ChatSession& chat, bool persist, const GitWorktreeIntentRecorder& record, std::stop_token stop) const
	{
		GitWorktreeOperationResult result;
		if (!ValidatePendingGitBaseOverrideScope(app, chat, result.message)) return result;
		if (stop.stop_requested()) { result.message = "Workspace preparation was canceled."; return result; }
		if (!app.settings.git_enabled) { result.message = "Git is disabled in Settings."; return result; }
		if (!uam::paths::IsControllerLocalWorkspace(chat))
		{
			result.message = std::string(kRemoteWorkspaceUnsupported);
			return result;
		}
		result.status = Status(app, chat);
		if (result.status.isolated)
		{
			CompleteSuccessfulResult(result, result.status, "Chat already has an isolated Git worktree.");
			return result;
		}
		const std::filesystem::path source_root = result.status.is_git_repository ? uam::paths::PathFromUtf8(result.status.source_directory) : EffectiveSourceWorkspace(app, chat);
		const bool managed_repository = !result.status.is_git_repository;
		if (managed_repository)
		{
			const LocalSnapshotSource source_kind = ClassifyLocalSnapshotSource(source_root);
			if (source_kind == LocalSnapshotSource::Git || source_kind == LocalSnapshotSource::Unknown)
			{
				result.message = source_kind == LocalSnapshotSource::Git ? "Source Git metadata could not be verified. No managed snapshot was created." : "Source workspace is not an available directory.";
				return result;
			}
			result.message = ManagedSnapshotDisabledReason(app, chat);
			if (!result.message.empty()) return result;
		}
		const std::filesystem::path repository = managed_repository ? ManagedRepositoryRoot(app, source_root, chat.id) : source_root;
		const std::string selected_base = uam::strings::Trim(chat.git_base_branch_override.value_or(app.settings.git_default_base_branch));
		std::string override_head;
		if (chat.git_base_branch_override)
		{
			std::string attached_branch;
			if (!app.settings.git_auto_worktree || managed_repository ||
			    !GitOutput(repository, "symbolic-ref --quiet --short HEAD", &attached_branch, &result.message, stop) || attached_branch.empty() ||
			    chat.git_base_branch_override->size() > 255 || std::ranges::any_of(*chat.git_base_branch_override, [](unsigned char c) { return c < 32 || c == 127; }) ||
			    (!selected_base.empty() && !GitCommand(repository, "check-ref-format --branch " + uam::shell::EscapeArg(selected_base), &result.message, stop)))
			{ result.message = "A Git base override requires an attached local Git workspace and automatic worktrees."; return result; }
			const std::string reference = selected_base.empty() ? "HEAD" : "refs/heads/" + selected_base;
			if (!GitOutput(repository, "rev-parse --verify --end-of-options " + uam::shell::EscapeArg(reference + "^{commit}"), &override_head, &result.message, stop)) return result;
		}
		if (record && !managed_repository && app.settings.git_pull_before_chat)
		{
			std::string common_directory;
			if (!GitOutput(repository, "rev-parse --path-format=absolute --git-common-dir", &common_directory, &result.message, stop)) return result;
			// A directly selected UAM snapshot is still owned storage, even in a new chat.
			const bool owned_baseline = !app.data_root.empty() && PathIsWithin(paths::NormalizeExistingOrAbsolutePath(paths::PathFromUtf8(common_directory)), paths::NormalizeExistingOrAbsolutePath(app.data_root / "worktrees"));
			if (!owned_baseline && !PullCheckedOutBase(repository, selected_base, result.message, stop)) return result;
		}
		if (chat.git_base_branch_override && record && app.settings.git_pull_before_chat)
		{
			const std::string reference = selected_base.empty() ? "HEAD" : "refs/heads/" + selected_base;
			if (!GitOutput(repository, "rev-parse --verify --end-of-options " + uam::shell::EscapeArg(reference + "^{commit}"), &override_head, &result.message, stop)) return result;
		}
		std::filesystem::path preferred_worktree;
		if (!PhysicalWorktreeRootForChat(app, source_root, BranchNameForChat(chat), preferred_worktree, result.message)) return result;
		GitWorktreeCreationIntent intent{source_root, repository, preferred_worktree, {}, {}, managed_repository};
		if (record && !record(intent, result.message)) return result;
		// Once owned mutation begins, finish bookkeeping; cancellation only revokes publication.
		if (record) stop = {};
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
				result.message = "Managed repository path already exists: " + uam::paths::Utf8PathString(repository);
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
			if (!GitCommand(repository, "init", &result.message, stop) ||
			    !GitCommand(repository, "config user.email uam@local.invalid", &result.message, stop) ||
			    !GitCommand(repository, "config user.name UAM", &result.message, stop) ||
			    !GitCommand(repository, "config core.autocrlf false", &result.message, stop) ||
			    !CopySnapshotFiles(files, repository, result.message) ||
			    !GitCommand(repository, "add -A", &result.message, stop) ||
			    !GitCommand(repository, "commit --allow-empty -m " + uam::shell::EscapeArg("UAM isolation baseline"), &result.message, stop))
			{
				cleanup_managed_repository();
				return result;
			}
		}

		const std::string base_branch = managed_repository ? std::string() : selected_base;
		if (!base_branch.empty() && (base_branch.size() > 255 ||
		    std::ranges::any_of(base_branch, [](unsigned char c) { return c < 32 || c == 127; }) ||
		    !GitCommand(repository, "check-ref-format --branch " + uam::shell::EscapeArg(base_branch), &result.message, stop)))
		{
			result.message = "The default Git base branch is invalid.";
			return result;
		}
		const std::string base_ref = base_branch.empty() ? "HEAD" : "refs/heads/" + base_branch;
		head = override_head;
		if (head.empty() && !GitOutput(repository, "rev-parse --verify --end-of-options " + uam::shell::EscapeArg(base_ref + "^{commit}"), &head, &result.message, stop))
		{
			if (!base_branch.empty())
			{
				result.message = "The default Git base branch is unavailable: " + base_branch;
				return result;
			}
			std::string head_ref;
			if (!managed_repository && GitOutput(repository, "symbolic-ref -q HEAD", &head_ref, nullptr, stop) && !head_ref.empty())
			{
				const ProcessExecutionResult reference = RunCommand(BuildGitCommandInDirectory(repository, "show-ref --verify --quiet " + uam::shell::EscapeArg(head_ref)), kDefaultGitCommandTimeoutMs, stop);
				if (!reference.timed_out && !reference.canceled && reference.exit_code == 1)
				{
					result.message = "This repository has no commits. Create an initial commit before creating a worktree.";
				}
			}
			if (managed_repository)
			{
				std::error_code cleanup_error;
				uam::paths::RemoveAllNoThrow(repository, &cleanup_error);
			}
			return result;
		}

		std::string branch_name;
		std::filesystem::path worktree_root;
		if (!ChooseWorktreeName(app, source_root, repository, chat, branch_name, worktree_root, result.message, stop))
		{
			if (managed_repository) (void)uam::paths::RemoveAllNoThrow(repository);
			return result;
		}
		if (uam::paths::PathExistsNoThrow(worktree_root))
		{
			result.message = "Worktree path already exists: " + uam::paths::Utf8PathString(worktree_root);
			if (managed_repository)
			{
				std::error_code cleanup_error;
				uam::paths::RemoveAllNoThrow(repository, &cleanup_error);
			}
			return result;
		}
		intent.worktree = worktree_root;
		intent.branch = branch_name;
		intent.base_sha = head;
		if (stop.stop_requested() || (record && !record(intent, result.message))) return result;
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

		const ProcessExecutionResult add_result = RunCommand(BuildGitCommandInDirectory(repository, "worktree add -b " + uam::shell::EscapeArg(branch_name) + " " + uam::shell::EscapeArg(uam::paths::Utf8PathString(worktree_root)) + " " + uam::shell::EscapeArg(head)), kDefaultGitCommandTimeoutMs, stop);
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

		const std::string previous_isolation_kind = chat.workspace_isolation_kind;
		chat.workspace_isolation_kind = uam::paths::kGitWorktreeIsolationKind;
		chat.workspace_source_directory = uam::paths::Utf8PathString(source_root);
		chat.workspace_base_ref = head;
		chat.workspace_branch_name = branch_name;
		chat.workspace_worktree_directory = uam::paths::Utf8PathString(worktree_root);
		chat.updated_at = uam::time::TimestampNow();

		std::string save_error;
		if (persist && !SaveChat(app, chat, &save_error))
		{
			result.message = save_error;
			GitCommand(repository, "worktree remove --force " + uam::shell::EscapeArg(uam::paths::Utf8PathString(worktree_root)));
			GitCommand(repository, "branch -D " + uam::shell::EscapeArg(branch_name));
			if (managed_repository)
			{
				std::error_code cleanup_error;
				uam::paths::RemoveAllNoThrow(repository, &cleanup_error);
			}
			chat.workspace_isolation_kind = previous_isolation_kind;
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

	GitWorktreeOperationResult GitWorktreeService::CreateBranchForChat(const AppState& app, const ChatSession& source, ChatSession& branch, const std::string& checkpoint_sha) const
	{
		GitWorktreeOperationResult result;
		if (!app.settings.git_enabled) { result.message = "Git is disabled in Settings."; return result; }
		if (!uam::paths::IsControllerLocalWorkspace(source) || !uam::paths::IsControllerLocalWorkspace(branch))
		{
			result.message = std::string(kRemoteWorkspaceUnsupported);
			return result;
		}
		if (!uam::paths::HasGitWorktreeSource(source) || !uam::paths::HasGitWorktreeDirectory(source) ||
		    !uam::paths::IsDirectoryNoThrow(uam::paths::PathFromUtf8(source.workspace_worktree_directory)))
		{
			result.message = "The source chat's isolated worktree is unavailable.";
			return result;
		}
		if (!IsCommitId(checkpoint_sha))
		{
			result.message = "The branch checkpoint is missing or invalid.";
			return result;
		}

		const std::filesystem::path source_root = uam::paths::PathFromUtf8(source.workspace_source_directory);
		const std::filesystem::path parent_managed_repository = ManagedRepositoryRoot(app, source_root, source.id);
		const bool managed_repository = uam::paths::IsDirectoryNoThrow(parent_managed_repository);
		const std::filesystem::path parent_repository = managed_repository ? parent_managed_repository : source_root;
		if (!GitCommand(parent_repository, "cat-file -e " + uam::shell::EscapeArg(checkpoint_sha + "^{commit}"), &result.message))
		{
			result.message = "The branch checkpoint is no longer available in the source repository.";
			return result;
		}

		const std::filesystem::path repository = managed_repository ? ManagedRepositoryRoot(app, source_root, branch.id) : parent_repository;
		std::string branch_name;
		std::filesystem::path worktree;
		if (!ChooseWorktreeName(app, source_root, parent_repository, branch, branch_name, worktree, result.message)) return result;
		if (uam::paths::PathExistsNoThrow(repository) && managed_repository)
		{
			result.message = "The branch's managed repository path already exists.";
			return result;
		}
		if (uam::paths::PathExistsNoThrow(worktree))
		{
			result.message = "The branch worktree path already exists.";
			return result;
		}
		std::error_code ec;
		if (!uam::paths::CreateDirectoriesNoThrow(worktree.parent_path(), &ec))
		{
			result.message = "Failed to create the branch worktree directory: " + ec.message();
			return result;
		}
		if (managed_repository)
		{
			const ProcessExecutionResult clone = RunCommand("git clone --no-hardlinks --quiet " +
			    uam::shell::EscapeArg(uam::paths::Utf8PathString(parent_repository)) + " " +
			    uam::shell::EscapeArg(uam::paths::Utf8PathString(repository)));
			if (!CommandSucceeded(clone) ||
			    !GitCommand(repository, "remote remove origin", &result.message) ||
			    !GitCommand(repository, "config core.autocrlf false", &result.message))
			{
				if (!CommandSucceeded(clone)) result.message = CommandOutputOrFallback(clone, "Failed to clone the source chat's managed repository.");
				(void)uam::paths::RemoveAllNoThrow(repository, &ec);
				return result;
			}
		}

		const ProcessExecutionResult add = RunCommand(BuildGitCommandInDirectory(repository,
		    "worktree add -b " + uam::shell::EscapeArg(branch_name) + " " +
		    uam::shell::EscapeArg(uam::paths::Utf8PathString(worktree)) + " " +
		    uam::shell::EscapeArg(checkpoint_sha)));
		if (!CommandSucceeded(add))
		{
			result.message = CommandOutputOrFallback(add, "Failed to create the branch worktree.");
			(void)GitCommand(repository, "worktree remove --force " + uam::shell::EscapeArg(uam::paths::Utf8PathString(worktree)));
			(void)GitCommand(repository, "branch -D " + uam::shell::EscapeArg(branch_name));
			if (managed_repository) (void)uam::paths::RemoveAllNoThrow(repository, &ec);
			return result;
		}

		branch.workspace_isolation_kind = uam::paths::kGitWorktreeIsolationKind;
		branch.workspace_source_directory = uam::paths::Utf8PathString(source_root);
		branch.workspace_worktree_directory = uam::paths::Utf8PathString(worktree);
		branch.workspace_branch_name = branch_name;
		branch.workspace_base_ref = checkpoint_sha;
		CompleteSuccessfulResult(result, Status(app, branch), "Created an isolated branch at the selected message's checkpoint.");
		return result;
	}

	bool GitWorktreeService::InspectRetentionCandidate(const std::filesystem::path& data_root,
	    const std::filesystem::path& source_root, const std::filesystem::path& worktree,
	    const std::vector<std::filesystem::path>& protected_paths, GitWorktreeRetentionCandidate& candidate,
	    std::stop_token stop_token, int retention_days) const
	{
		candidate = {};
		if (stop_token.stop_requested() || retention_days < 1 || retention_days > 3650 || data_root.empty() || source_root.empty() || worktree.empty()) return false;
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		const std::function<bool(const std::filesystem::path&, const std::string&, std::string&)> query = [&stop_token, deadline](const std::filesystem::path& directory, const std::string& arguments, std::string& output)
		{
			const int remaining_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
			if (remaining_ms <= 0 || stop_token.stop_requested()) return false;
			const ProcessExecutionResult result = RunCommand("git --no-optional-locks -C " +
			    uam::shell::EscapeArg(uam::paths::Utf8PathString(directory)) + " " + arguments, std::min(2000, remaining_ms), stop_token);
			if (!CommandSucceeded(result) || result.output_truncated) return false;
			output = uam::strings::Trim(result.output);
			return true;
		};
		std::string repository_root;
		if (!query(source_root, "rev-parse --show-toplevel", repository_root)) return false;
		const std::filesystem::path bucket = data_root / "worktrees" /
		    uam::hashing::Hex64(uam::hashing::Fnv1a64(uam::paths::NormalizedPortablePathString(uam::paths::PathFromUtf8(repository_root))));
		const std::filesystem::path absolute = uam::paths::AbsolutePathNoThrow(worktree).lexically_normal();
		const std::filesystem::path owned_root = uam::paths::AbsolutePathNoThrow(bucket).lexically_normal();
		if (absolute == owned_root || !PathIsWithin(absolute, owned_root)) return false;
		// Walk every owned ancestor: canonical containment alone would accept an internal directory link.
		for (std::filesystem::path path = absolute; ; path = path.parent_path())
		{
			if (!uam::paths::IsDirectoryNoThrow(path) || uam::paths::IsLinkOrReparsePointNoThrow(path)) return false;
			if (path == uam::paths::AbsolutePathNoThrow(data_root).lexically_normal()) break;
			if (path == path.parent_path()) return false;
		}
		for (const std::filesystem::path& protected_path : protected_paths)
		{
			if (protected_path.empty()) continue;
			std::error_code reference_error;
			const std::filesystem::path reference = std::filesystem::weakly_canonical(protected_path, reference_error);
			if (reference_error) return false;
			const std::filesystem::path resolved = std::filesystem::canonical(absolute, reference_error);
			if (reference_error || PathIsWithin(reference, resolved) || PathIsWithin(resolved, reference)) return false;
			// File identity also protects alternate casing on case-insensitive volumes.
			for (const std::pair<std::filesystem::path, std::filesystem::path>& pair :
			    {std::make_pair(reference, resolved), std::make_pair(resolved, reference)})
			{
				for (std::filesystem::path parent = pair.first; ; parent = parent.parent_path())
				{
					if (std::filesystem::equivalent(parent, pair.second, reference_error)) return false;
					if (reference_error && reference_error != std::errc::no_such_file_or_directory) return false;
					reference_error.clear();
					if (parent == parent.parent_path()) break;
				}
			}
		}
		std::error_code error;
		const std::filesystem::file_status marker_status = std::filesystem::symlink_status(absolute / ".git", error);
		if (error || !std::filesystem::is_regular_file(marker_status) || uam::paths::IsLinkOrReparsePointNoThrow(absolute / ".git")) return false;

		std::string common_output;
		std::string expected_common_output;
		std::string admin_output;
		if (!query(absolute, "rev-parse --path-format=absolute --git-common-dir", common_output) ||
		    !query(source_root, "rev-parse --path-format=absolute --git-common-dir", expected_common_output) ||
		    !query(absolute, "rev-parse --absolute-git-dir", admin_output)) return false;
		const std::filesystem::path common = std::filesystem::canonical(uam::paths::PathFromUtf8(common_output), error);
		if (error) return false;
		const std::filesystem::path expected_common = std::filesystem::canonical(uam::paths::PathFromUtf8(expected_common_output), error);
		if (error || common != expected_common) return false;
		const std::filesystem::path admin = std::filesystem::canonical(uam::paths::PathFromUtf8(admin_output), error);
		if (error || admin.parent_path() != common / "worktrees" || uam::paths::IsLinkOrReparsePointNoThrow(uam::paths::PathFromUtf8(admin_output))) return false;
		if (std::filesystem::exists(admin / "locked", error) || error) return false;
		const std::filesystem::path gitdir_file = admin / "gitdir";
		if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(gitdir_file, error)) || error ||
		    std::filesystem::file_size(gitdir_file, error) > 8192 || error) return false;
		const std::filesystem::path back_pointer = std::filesystem::canonical(
		    uam::paths::PathFromUtf8(uam::strings::Trim(uam::io::ReadTextFile(gitdir_file))), error);
		if (error || back_pointer != std::filesystem::canonical(absolute / ".git", error) || error) return false;
		GitWorktreeRetentionCandidate inspected;
		inspected.directory = absolute;
		inspected.administrative_directory = admin;
		if (!query(absolute, "symbolic-ref --short HEAD", inspected.branch_name) ||
		    !query(absolute, "rev-parse HEAD", inspected.head_sha)) return false;
		const std::filesystem::file_time_type cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * retention_days);
		inspected.activity_at = std::filesystem::file_time_type::min();
		const std::vector<std::filesystem::path> activity_paths = {
		    absolute, absolute / ".git", admin / "HEAD", admin / "index", admin / "logs" / "HEAD",
		    common / "refs" / "heads" / uam::paths::PathFromUtf8(inspected.branch_name),
		    common / "logs" / "refs" / "heads" / uam::paths::PathFromUtf8(inspected.branch_name)};
		for (const std::filesystem::path& path : activity_paths)
		{
			const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
			if (error == std::errc::no_such_file_or_directory) { error.clear(); continue; }
			if (error || uam::paths::IsLinkOrReparsePointNoThrow(path)) return false;
			if (!std::filesystem::exists(status)) continue;
			const std::filesystem::file_time_type modified = std::filesystem::last_write_time(path, error);
			if (error || modified >= cutoff) return false;
			inspected.activity_at = std::max(inspected.activity_at, modified);
		}
		// Git removes ignored files with a clean worktree, so include them in the preservation check.
		std::string status;
		if (!query(absolute, "status --porcelain --untracked-files=normal --ignored=matching", status) || !status.empty() || stop_token.stop_requested()) return false;
		candidate = std::move(inspected);
		return true;
	}

	bool GitWorktreeService::RemoveRetentionCandidate(const std::filesystem::path& data_root,
	    const std::filesystem::path& source_root, const GitWorktreeRetentionCandidate& candidate,
	    const std::vector<std::filesystem::path>& protected_paths, const WorkspaceLeaseGate::Lease& lease,
	    std::stop_token stop_token, int retention_days) const
	{
		if (!lease.IsExclusive() || stop_token.stop_requested()) return false;
		GitWorktreeRetentionCandidate current;
		if (!InspectRetentionCandidate(data_root, source_root, candidate.directory, protected_paths, current, stop_token, retention_days) ||
		    current.administrative_directory != candidate.administrative_directory || current.branch_name != candidate.branch_name ||
		    current.head_sha != candidate.head_sha || current.activity_at != candidate.activity_at) return false;
		const ProcessExecutionResult result = RunCommand(BuildGitCommandInDirectory(source_root,
		    "worktree remove " + uam::shell::EscapeArg(uam::paths::Utf8PathString(candidate.directory))), 2000, stop_token);
		return CommandSucceeded(result);
	}

	bool GitWorktreeService::RemoveUnusedBranchWorktree(const AppState& app, const ChatSession& branch, std::string* error_out) const
	{
		if (error_out != nullptr) error_out->clear();
		if (!uam::paths::IsGitWorktreeIsolated(branch)) return true;
		if (!uam::paths::IsControllerLocalWorkspace(branch) ||
		    uam::strings::IsBlank(branch.workspace_source_directory) ||
		    uam::strings::IsBlank(branch.workspace_worktree_directory) ||
		    uam::strings::IsBlank(branch.workspace_branch_name) || !IsCommitId(branch.workspace_base_ref))
		{
			if (error_out != nullptr) *error_out = "Branch worktree metadata is incomplete; it was left untouched.";
			return false;
		}
		const std::filesystem::path source_root = uam::paths::PathFromUtf8(branch.workspace_source_directory);
		const std::filesystem::path worktree = uam::paths::PathFromUtf8(branch.workspace_worktree_directory);
		if (RefuseSharedWorktreeMutation(app, branch, worktree, error_out)) return false;
		const bool original_branch_path = OriginalWorktreeBranchPath(app, source_root, worktree, branch.workspace_branch_name, branch.id);
		std::filesystem::path verified;
		std::string path_error;
		if ((!original_branch_path && !VerifiedBranchSelectionWorkspace(app, branch, verified, path_error)) ||
		    !PathIsWithin(worktree, WorktreeRootForChat(app, source_root, "")) ||
		    !paths::IsDirectoryNoThrow(worktree) || paths::IsLinkOrReparsePointNoThrow(worktree))
		{
			if (error_out != nullptr) *error_out = "Branch worktree path is unavailable or unexpected; it was left untouched.";
			return false;
		}

		bool dirty = false;
		std::string error;
		std::string head;
		if (!IsDirty(worktree, &dirty, &error) || !GitOutput(worktree, "rev-parse HEAD", &head, &error))
		{
			if (error_out != nullptr) *error_out = uam::strings::NonEmptyOrFallback(error, "Could not inspect branch worktree.");
			return false;
		}
		std::string actual_branch;
		if (!GitOutput(worktree, "symbolic-ref --short HEAD", &actual_branch, &error) ||
		    actual_branch != branch.workspace_branch_name)
		{
			if (error_out != nullptr) *error_out = "Branch worktree no longer has the expected branch; it was left untouched.";
			return false;
		}
		if (dirty || head != branch.workspace_base_ref)
		{
			if (error_out != nullptr) *error_out = "Branch worktree contains changes; it was left untouched.";
			return false;
		}
		const std::filesystem::path managed_repository = ManagedRepositoryRoot(app, source_root, branch.id);
		const bool managed = uam::paths::IsDirectoryNoThrow(managed_repository);
		const std::filesystem::path repository = managed ? managed_repository : source_root;
		if (!GitCommand(repository, "worktree remove " + uam::shell::EscapeArg(uam::paths::Utf8PathString(worktree)), &error))
		{
			if (error_out != nullptr) *error_out = uam::strings::NonEmptyOrFallback(error, "Failed to remove unused branch worktree.");
			return false;
		}
		// A selected branch can predate this chat; only its original branch/path pair owns deletion.
		if (original_branch_path) (void)GitCommand(repository, "branch -D " + uam::shell::EscapeArg(branch.workspace_branch_name));
		std::error_code ec;
		if (managed && !uam::paths::RemoveAllNoThrow(repository, &ec))
		{
			if (error_out != nullptr) *error_out = "Failed to remove the unused branch's managed repository: " + ec.message();
			return false;
		}
		return true;
	}

	GitWorktreeOperationResult GitWorktreeService::DiscardChatChanges(AppState& app, ChatSession& chat) const
	{
		GitWorktreeOperationResult result;
		if (!app.settings.git_enabled) { result.message = "Git is disabled in Settings."; return result; }
		if (!uam::paths::IsControllerLocalWorkspace(chat))
		{
			result.message = std::string(kRemoteWorkspaceUnsupported);
			return result;
		}
		result.status = Status(app, chat);
		if (!result.status.isolated)
		{
			result.message = "Chat does not have an isolated Git worktree.";
			return result;
		}

		if (RefuseSharedWorktreeMutation(app, chat, WorktreePathFromStatus(result.status), &result.message)) return result;

		if (!result.status.worktree_missing)
		{
			const std::filesystem::path worktree = WorktreePathFromStatus(result.status);
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

	namespace
	{
		bool SamePortPath(const std::filesystem::path& left, const std::filesystem::path& right)
		{
			std::error_code left_error, right_error;
			const std::filesystem::path a = std::filesystem::canonical(left, left_error);
			const std::filesystem::path b = std::filesystem::canonical(right, right_error);
			return !left_error && !right_error && a == b;
		}

		GitWorktreeOperationResult StageCommittedPort(AppState& app, const ChatSession& chat, GitWorktreeOperationResult result, std::stop_token stop)
		{
			if (stop.stop_requested()) { result.message = "Port operation was canceled before staging."; return result; }
			const std::shared_ptr<WorkspaceLeaseGate::Lease> lease = app.worktree_retention_gate.TryAcquireMutation();
			if (!lease) { result.message = "Workspace cleanup is finishing. Retry porting changes."; return result; }
			if (chat.imported_read_only || result.status.managed_repository || result.status.is_svn_workspace || !result.status.is_git_repository)
			{ result.message = "Merge and squash require an owned local Git worktree."; return result; }
			const std::filesystem::path source = SourcePathFromStatusOrChat(app, chat, result.status);
			const std::filesystem::path worktree = WorktreePathFromStatus(result.status);
			const std::filesystem::path bucket = WorktreeRootForChat(app, source, {});
			if (!PathIsWithin(uam::paths::AbsolutePathNoThrow(worktree).lexically_normal(), uam::paths::AbsolutePathNoThrow(bucket).lexically_normal()) ||
			    worktree == bucket || uam::paths::IsLinkOrReparsePointNoThrow(worktree / ".git"))
			{ result.message = "Workspace ownership could not be validated."; return result; }
			for (std::filesystem::path path = uam::paths::AbsolutePathNoThrow(worktree).lexically_normal(); ; path = path.parent_path())
			{
				if (uam::paths::IsLinkOrReparsePointNoThrow(path)) { result.message = "Workspace storage contains a link."; return result; }
				if (path == uam::paths::AbsolutePathNoThrow(app.data_root).lexically_normal()) break;
				if (path == path.parent_path()) { result.message = "Workspace ownership root is invalid."; return result; }
			}
			std::string source_branch, worktree_branch, source_sha, worktree_sha, source_common, worktree_common, top, dirty;
			if (!GitOutput(source, "symbolic-ref --quiet --short HEAD", &source_branch, &result.message) ||
			    !GitOutput(worktree, "symbolic-ref --quiet --short HEAD", &worktree_branch, &result.message) || worktree_branch != chat.workspace_branch_name ||
			    !GitOutput(source, "rev-parse HEAD", &source_sha, &result.message) ||
			    !GitOutput(worktree, "rev-parse HEAD", &worktree_sha, &result.message) ||
			    !GitOutput(source, "rev-parse --path-format=absolute --git-common-dir", &source_common, &result.message) ||
			    !GitOutput(worktree, "rev-parse --path-format=absolute --git-common-dir", &worktree_common, &result.message) ||
			    !SamePortPath(uam::paths::PathFromUtf8(source_common), uam::paths::PathFromUtf8(worktree_common)) ||
			    !GitOutput(worktree, "rev-parse --show-toplevel", &top, &result.message) ||
			    !SamePortPath(uam::paths::PathFromUtf8(top), worktree) ||
			    chat.workspace_base_ref.empty() || !GitCommand(worktree, "merge-base --is-ancestor " + uam::shell::EscapeArg(chat.workspace_base_ref) + " " + uam::shell::EscapeArg(worktree_sha), &result.message))
			{ result.message = "Workspace branch, baseline or repository ownership changed."; return result; }
			for (const std::filesystem::path& directory : {source, worktree})
			{
				if (!GitOutput(directory, "status --porcelain --untracked-files=no", &dirty, &result.message) || !dirty.empty())
				{ result.message = "Commit the tracked workspace changes before merge or squash."; return result; }
				for (const std::string_view marker : {"MERGE_HEAD", "CHERRY_PICK_HEAD", "REVERT_HEAD", "rebase-merge", "rebase-apply", "sequencer", "index.lock"})
				{
					std::string path;
					if (!GitOutput(directory, "rev-parse --path-format=absolute --git-path " + uam::shell::EscapeArg(std::string(marker)), &path, &result.message)) return result;
					if (uam::paths::PathExistsNoThrow(uam::paths::PathFromUtf8(path)))
					{ result.message = "Finish the current Git operation before porting changes."; return result; }
				}
			}
			std::string common_base;
			if (!GitOutput(source, "merge-base " + uam::shell::EscapeArg(source_sha) + " " + uam::shell::EscapeArg(worktree_sha), &common_base, &result.message)) return result;
			const std::filesystem::path receipt_root = app.data_root / ".worktree-ports";
			if (uam::paths::IsLinkOrReparsePointNoThrow(receipt_root) || !uam::paths::CreateDirectoriesNoThrow(receipt_root))
			{ result.message = "Could not create safe port operation storage."; return result; }
			const std::string method = app.settings.git_port_method == GitPortMethod::Merge ? "merge" : "squash";
			result.operation_receipt = receipt_root / (uam::hashing::Hex64(uam::hashing::Fnv1a64(chat.id)) + "-" +
			    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
			nlohmann::json receipt = {{"chatId", chat.id}, {"createdAt", chat.created_at}, {"method", method}, {"source", uam::paths::Utf8PathString(source)},
			    {"worktree", uam::paths::Utf8PathString(worktree)}, {"sourceBranch", source_branch}, {"sourceSha", source_sha}, {"worktreeBranch", worktree_branch}, {"worktreeSha", worktree_sha}, {"state", "intent"}};
			if (!uam::io::WriteTextFile(result.operation_receipt, receipt.dump()))
			{ result.message = "Could not record the port operation before changing Git."; return result; }
			if (stop.stop_requested()) { receipt["state"] = "canceled"; (void)uam::io::WriteTextFile(result.operation_receipt, receipt.dump()); result.message = "Port operation was canceled before staging."; return result; }
			// Only this bounded child disables prompts and hooks; merge stages changes without creating a user commit.
#if defined(_WIN32)
			const std::string environment = "set \"GIT_TERMINAL_PROMPT=0\" && set \"GIT_ASKPASS=\" && set \"SSH_ASKPASS=\" && ";
			const std::string hooks = "NUL";
#else
			const std::string environment = "GIT_TERMINAL_PROMPT=0 GIT_ASKPASS='' SSH_ASKPASS='' ";
			const std::string hooks = "/dev/null";
#endif
			const ProcessExecutionResult operation = RunCommand(environment + BuildGitCommandInDirectory(source,
			    "-c credential.interactive=false -c core.askPass= -c core.hooksPath=" + uam::shell::EscapeArg(hooks) +
			    " merge " + (method == "merge" ? "--no-ff --no-commit " : "--squash --no-commit ") + uam::shell::EscapeArg(worktree_sha)), kDefaultGitCommandTimeoutMs, stop);
			std::string conflicts;
			const bool inspected = GitOutput(source, "diff --name-only --diff-filter=U", &conflicts, &result.message);
			result.port_state = CommandSucceeded(operation) ? "staged" : inspected && !conflicts.empty() ? "conflicts" : operation.canceled || operation.timed_out ? "interrupted" : "failed";
			receipt["state"] = result.port_state;
			receipt["conflicts"] = conflicts;
			if (!uam::io::WriteTextFile(result.operation_receipt, receipt.dump()))
			{ result.message = "Git finished but its result receipt could not be saved. Inspect the source workspace."; return result; }
			result.ok = CommandSucceeded(operation);
			result.message = result.ok ? "Changes are staged in the source workspace. Review and commit them to finish." :
			    result.port_state == "conflicts" ? "Porting left conflicts in the source workspace. Resolve them there; the chat worktree is retained." : CommandOutputOrFallback(operation, "Git refused to stage the port operation.");
			return result;
		}
	}

	void CancelGitPortRequest(AppState& app, const std::string& chat_id)
	{
		const std::unordered_map<std::string, std::shared_ptr<AppState::GitPortRequest>>::const_iterator request = app.git_port_requests.find(chat_id);
		if (request != app.git_port_requests.end()) request->second->cancellation->request_stop();
	}

	void PollGitPortRequests(AppState& app)
	{
		for (std::unordered_map<std::string, std::shared_ptr<AppState::GitPortRequest>>::iterator iterator = app.git_port_requests.begin(); iterator != app.git_port_requests.end();)
		{
			const ChatSession* chat = ChatDomainService().FindChatById(app, iterator->first);
			const std::shared_ptr<AppState::GitPortRequest> request = iterator->second;
			if ((request->lifetime_bound && request->handler_lifetime.expired()) || !chat || app.data_root != request->root || chat->metadata_incarnation != request->incarnation ||
			    chat->workspace_directory != request->workspace || chat->workspace_source_directory != request->source ||
			    chat->workspace_worktree_directory != request->worktree || chat->workspace_branch_name != request->branch || chat->workspace_base_ref != request->base)
				request->cancellation->request_stop();
			if (request->cancellation->stop_requested())
			{
				// A canceled queued request cannot subsequently win the worker's start transition.
				AppState::GitPortRequest::WorkerState queued = AppState::GitPortRequest::WorkerState::Queued;
				(void)request->worker_state.compare_exchange_strong(queued, AppState::GitPortRequest::WorkerState::Finished);
			}
			if (request->worker_state.load() == AppState::GitPortRequest::WorkerState::Finished)
			{
				request->gui_settled = true;
				request->lease.reset();
				app.worktree_operation_chat_ids.erase(iterator->first);
				iterator = app.git_port_requests.erase(iterator);
				continue;
			}
			++iterator;
		}
	}

	GitWorktreeOperationResult GitWorktreeService::PortCommittedChanges(AppState& app, const ChatSession& chat, std::stop_token stop) const
	{
		GitWorktreeOperationResult result;
		if (!app.settings.git_enabled || !uam::paths::IsControllerLocalWorkspace(chat) || app.settings.git_port_method == GitPortMethod::Patch)
		{ result.message = "Staged porting requires an enabled local Git merge or squash method."; return result; }
		result.status = Status(app, chat);
		if (!result.status.isolated || result.status.worktree_missing) { result.message = "An owned isolated worktree is required."; return result; }
		return StageCommittedPort(app, chat, std::move(result), stop);
	}

	GitWorktreeOperationResult GitWorktreeService::PortChatChanges(AppState& app, ChatSession& chat) const
	{
		GitWorktreeOperationResult result;
		if (!app.settings.git_enabled) { result.message = "Git is disabled in Settings."; return result; }
		if (!uam::paths::IsControllerLocalWorkspace(chat))
		{
			result.message = std::string(kRemoteWorkspaceUnsupported);
			return result;
		}
		result.status = Status(app, chat);
		if (!result.status.isolated || result.status.worktree_missing)
		{
			result.message = "Chat does not have an available isolated Git worktree.";
			return result;
		}

		if (app.settings.git_port_method != GitPortMethod::Patch) return StageCommittedPort(app, chat, std::move(result), {});

		if (RefuseSharedWorktreeMutation(app, chat, WorktreePathFromStatus(result.status), &result.message)) return result;

		const std::filesystem::path source = SourcePathFromStatusOrChat(app, chat, result.status);
		const std::filesystem::path worktree = WorktreePathFromStatus(result.status);
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
			const ProcessExecutionResult check_result = RunCommand(BuildGitCommandInDirectory(source, "-c core.autocrlf=false apply --check " + uam::shell::EscapeArg(uam::paths::Utf8PathString(patch_path))));
			if (!CommandSucceeded(check_result))
			{
				result.patch_path = patch_path;
				result.message = CommandOutputOrFallback(check_result, "Chat changes cannot be applied cleanly to the source workspace.");
				return result;
			}
		}
		ProcessExecutionResult apply_result = RunCommand(BuildGitCommandInDirectory(source, std::string(result.status.managed_repository ? "-c core.autocrlf=false apply " : "apply --3way ") + uam::shell::EscapeArg(uam::paths::Utf8PathString(patch_path))));
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

	bool GitWorktreeService::CanCheckpointTurn(const AppState& app, const ChatSession& chat, std::string* reason_out, std::stop_token stop_token) const
	{
		if (reason_out != nullptr) reason_out->clear();
		if (!app.settings.git_enabled) { if (reason_out != nullptr) *reason_out = "Git is disabled in Settings."; return false; }
		if (!uam::paths::IsControllerLocalWorkspace(chat))
		{
			if (reason_out != nullptr) *reason_out = std::string(kRemoteWorkspaceUnsupported);
			return false;
		}
		if (!uam::paths::IsGitWorktreeIsolated(chat))
		{
			if (reason_out != nullptr) *reason_out = "Automatic checkpoints require an available UAM isolated Git worktree.";
			return false;
		}
		const std::filesystem::path worktree = TrimmedPathOrEmpty(chat.workspace_worktree_directory);
		if (worktree.empty() || !uam::paths::IsDirectoryNoThrow(worktree))
		{
			if (reason_out != nullptr) *reason_out = "Automatic checkpoints require an available UAM isolated Git worktree.";
			return false;
		}
		bool dirty = false;
		std::string error;
		if (!IsDirty(worktree, &dirty, &error, stop_token))
		{
			if (reason_out != nullptr) *reason_out = uam::strings::NonEmptyOrFallback(error, "Failed to inspect the isolated worktree.");
			return false;
		}
		if (dirty)
		{
			if (reason_out != nullptr) *reason_out = "The isolated worktree already had changes before this turn, so UAM will not absorb them into an automatic checkpoint.";
			return false;
		}
		return true;
	}

	GitTurnCheckpointResult GitWorktreeService::CreateTurnCheckpoint(AppState& app, ChatSession& chat, int assistant_message_index, std::stop_token stop_token, bool persist_chat) const
	{
		GitTurnCheckpointResult result;
		if (!app.settings.git_enabled) { result.message = "Git is disabled in Settings."; return result; }
		if (!uam::paths::IsControllerLocalWorkspace(chat))
		{
			result.message = std::string(kRemoteWorkspaceUnsupported);
			return result;
		}
		if (assistant_message_index < 0 || assistant_message_index >= static_cast<int>(chat.messages.size()) || chat.messages[assistant_message_index].role != MessageRole::Assistant)
		{
			result.message = "The completed assistant turn is unavailable for checkpointing.";
			return result;
		}
		Message& message = chat.messages[assistant_message_index];
		if (!message.checkpoint_sha.empty())
		{
			result.ok = true;
			result.checkpoint_sha = message.checkpoint_sha;
			result.parent_sha = message.checkpoint_parent_sha;
			result.message = "This turn already has a checkpoint.";
			return result;
		}
		if (!uam::paths::IsGitWorktreeIsolated(chat))
		{
			result.message = "Automatic checkpoints require an available UAM isolated Git worktree.";
			return result;
		}
		const std::filesystem::path worktree = TrimmedPathOrEmpty(chat.workspace_worktree_directory);
		if (worktree.empty() || !uam::paths::IsDirectoryNoThrow(worktree))
		{
			result.message = "Automatic checkpoints require an available UAM isolated Git worktree.";
			return result;
		}
		// Preflight was clean, but a user can stage work while the provider runs.
		// Skip the automatic commit rather than consume their current index.
		std::string staged_paths;
		if (!GitOutput(worktree, "diff --cached --name-only -z --", &staged_paths, &result.message, stop_token)) return result;
		if (!staged_paths.empty())
		{
			result.ok = true;
			result.message = "Automatic checkpoint skipped because staged changes exist. Your staging was preserved.";
			return result;
		}

		bool dirty = false;
		if (!IsDirty(worktree, &dirty, &result.message, stop_token)) return result;
		if (!dirty)
		{
			result.ok = true;
			result.message = "The completed turn did not change the isolated worktree.";
			return result;
		}
		if (!GitOutput(worktree, "rev-parse HEAD", &result.parent_sha, &result.message, stop_token) || !IsCommitId(result.parent_sha))
		{
			return result;
		}
		if (!GitCommand(worktree, "add -A", &result.message, stop_token))
		{
			(void)GitCommand(worktree, "reset --mixed " + uam::shell::EscapeArg(result.parent_sha));
			result.message = uam::strings::NonEmptyOrFallback(result.message, "Automatic checkpoint staging was cancelled or failed.");
			return result;
		}
		const std::string commit_message = "UAM checkpoint: " + chat.id + " turn " + std::to_string(assistant_message_index);
		if (!GitCommand(worktree, "-c user.name=UAM -c user.email=uam@local.invalid commit -m " + uam::shell::EscapeArg(commit_message), &result.message, stop_token) ||
		    !GitOutput(worktree, "rev-parse HEAD", &result.checkpoint_sha, &result.message, stop_token) || !IsCommitId(result.checkpoint_sha))
		{
			(void)GitCommand(worktree, "reset --mixed " + uam::shell::EscapeArg(result.parent_sha));
			result.message = uam::strings::NonEmptyOrFallback(result.message, "Automatic checkpoint creation was cancelled or failed.");
			return result;
		}

		message.checkpoint_sha = result.checkpoint_sha;
		message.checkpoint_parent_sha = result.parent_sha;
		chat.updated_at = uam::time::TimestampNow();
		if (persist_chat && !SaveChat(app, chat, &result.message))
		{
			message.checkpoint_sha.clear();
			message.checkpoint_parent_sha.clear();
			(void)GitCommand(worktree, "reset --mixed " + uam::shell::EscapeArg(result.parent_sha));
			return result;
		}
		result.ok = true;
		result.changed = true;
		result.message = "Created an automatic checkpoint for this completed turn.";
		return result;
	}

	GitTurnCheckpointResult GitWorktreeService::PreviewTurnRollback(const AppState& app, const ChatSession& chat, int assistant_message_index) const
	{
		GitTurnCheckpointResult result;
		if (!app.settings.git_enabled) { result.message = "Git is disabled in Settings."; return result; }
		if (!uam::paths::IsControllerLocalWorkspace(chat))
		{
			result.message = std::string(kRemoteWorkspaceUnsupported);
			return result;
		}
		if (assistant_message_index < 0 || assistant_message_index >= static_cast<int>(chat.messages.size()))
		{
			result.message = "The requested turn is unavailable.";
			return result;
		}
		const Message& message = chat.messages[assistant_message_index];
		result.checkpoint_sha = message.checkpoint_sha;
		result.parent_sha = message.checkpoint_parent_sha;
		if (!IsCommitId(result.checkpoint_sha) || !IsCommitId(result.parent_sha))
		{
			result.message = "This turn has no valid rollback checkpoint.";
			return result;
		}
		const GitWorktreeStatus status = Status(app, chat);
		if (!status.isolated || status.worktree_missing)
		{
			result.message = "The isolated worktree is unavailable.";
			return result;
		}
		std::string head;
		const std::filesystem::path worktree = WorktreePathFromStatus(status);
		if (!GitOutput(worktree, "rev-parse HEAD", &head, &result.message)) return result;
		if (head != result.checkpoint_sha)
		{
			result.message = "Later commits or source drift exist. Roll back the newest checkpoint first or resolve the worktree manually.";
			return result;
		}
		bool dirty = false;
		if (!IsDirty(worktree, &dirty, &result.message)) return result;
		if (dirty)
		{
			result.message = "The isolated worktree has uncommitted changes. Preserve or discard them before rollback.";
			return result;
		}
		const ProcessExecutionResult diff = RunCommand(BuildGitCommandInDirectory(worktree, "diff --stat " + uam::shell::EscapeArg(result.parent_sha) + " " + uam::shell::EscapeArg(result.checkpoint_sha)));
		if (!CommandSucceeded(diff))
		{
			result.message = CommandOutputOrFallback(diff, "Failed to preview the checkpoint rollback.");
			return result;
		}
		result.diff = diff.output.substr(0, 64 * 1024);
		result.ok = true;
		result.changed = true;
		result.message = "Rollback preview is ready.";
		return result;
	}

	GitTurnCheckpointResult GitWorktreeService::RollbackTurn(AppState& app, ChatSession& chat, int assistant_message_index) const
	{
		GitTurnCheckpointResult result = PreviewTurnRollback(app, chat, assistant_message_index);
		if (!result.ok) return result;
		const GitWorktreeStatus status = Status(app, chat);
		const std::filesystem::path worktree = WorktreePathFromStatus(status);
		if (RefuseSharedWorktreeMutation(app, chat, worktree, &result.message))
		{
			result.ok = false;
			result.changed = false;
			return result;
		}

		if (!GitCommand(worktree, "reset --hard " + uam::shell::EscapeArg(result.parent_sha), &result.message))
		{
			result.ok = false;
			return result;
		}
		Message& message = chat.messages[assistant_message_index];
		const std::string checkpoint_sha = message.checkpoint_sha;
		const std::string parent_sha = message.checkpoint_parent_sha;
		message.checkpoint_sha.clear();
		message.checkpoint_parent_sha.clear();
		chat.updated_at = uam::time::TimestampNow();
		if (!SaveChat(app, chat, &result.message))
		{
			message.checkpoint_sha = checkpoint_sha;
			message.checkpoint_parent_sha = parent_sha;
			(void)GitCommand(worktree, "reset --hard " + uam::shell::EscapeArg(checkpoint_sha));
			result.ok = false;
			return result;
		}
		result.ok = true;
		result.message = "Rolled the isolated worktree back to before this turn.";
		return result;
	}
} // namespace uam
