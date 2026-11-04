#include "app/workspace_vcs_context_service.h"
#include "app/vcs_commit_service.h"
#include "common/platform/platform_services.h"
#include "common/paths/path_utils.h"
#include "common/utils/shell_escape.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <sstream>

namespace uam
{
	std::shared_ptr<void> AcquireWorkspaceVcsContextSlot(const std::shared_ptr<std::atomic<unsigned>>& pending)
	{
		unsigned count = pending->load();
		while (count < 4)
		{
			if (pending->compare_exchange_weak(count, count + 1))
				return std::shared_ptr<void>(pending.get(), [pending](void*) { pending->fetch_sub(1); });
		}
		return {};
	}

	WorkspaceVcsContext ReadWorkspaceVcsContext(const std::filesystem::path& directory,
	    bool git_enabled, bool svn_enabled, std::stop_token stop)
	{
		WorkspaceVcsContext context;
		if (stop.stop_requested()) { context.error = "Repository context was canceled."; return context; }
		std::error_code ec;
		if (!directory.is_absolute() || !std::filesystem::is_directory(directory, ec) || ec)
		{ context.error = "The workspace directory is unavailable."; return context; }
		if (!git_enabled && !svn_enabled) return context;

		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		const std::string path = uam::shell::EscapeArg(uam::paths::Utf8PathString(directory));
		const std::string git = "git --no-optional-locks -C " + path + " ";
		const std::function<bool(const std::string&, std::string&, const char*)> run = [&](const std::string& command, std::string& output, const char* svn_item)
		{
			const long long remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
			if (remaining <= 0 || stop.stop_requested()) return false;
			const int timeout = static_cast<int>(std::min(remaining, 2000LL));
			const ProcessExecutionResult result = svn_item != nullptr
			    ? RunSvn(directory, {"info", "--non-interactive", "--show-item", svn_item, "--", "."}, timeout, stop)
			    : PlatformServicesFactory::Instance().process_service.ExecuteCommand(command, timeout, stop);
			if (!result.ok || result.exit_code != 0 || result.output_truncated || result.canceled || result.timed_out) return false;
			output = uam::strings::Trim(result.output);
			return true;
		};
		const auto read = [&](const std::string& command, std::string& output) { return run(command, output, nullptr); };
		const auto svn = [&](const char* item, std::string& output) { return run({}, output, item); };
		const std::function<bool(const std::string&)> valid_commit = [](const std::string& commit)
		{
			return (commit.size() == 40 || commit.size() == 64) && std::all_of(commit.begin(), commit.end(), [](char value)
			{ return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); });
		};
		std::string inside;
		if (git_enabled && read(git + "rev-parse --is-inside-work-tree", inside) && inside == "true")
		{
			context.vcs = "git";
			std::string refs;
			if (!read(git + "rev-parse --verify HEAD", context.current_commit) || !valid_commit(context.current_commit) ||
			    !read(git + "branch --show-current", context.current_branch) ||
			    !read(git + "for-each-ref --sort=-committerdate --format=" + uam::shell::EscapeArg("%(refname)%09%(objectname)%09%(objecttype)") + " refs/heads/", refs))
			{ context.error = "Could not read local Git branches."; return context; }
			context.detached = context.current_branch.empty();
			std::istringstream lines(refs);
			std::string line;
			while (std::getline(lines, line))
			{
				const std::size_t tab = line.find('\t');
				if (!line.starts_with("refs/heads/") || tab == std::string::npos || tab <= 11 || context.local_branches.size() >= 4096)
				{ context.error = "Local Git branch context exceeded its safe format or size."; return context; }
				const std::size_t type_tab = line.find('\t', tab + 1);
				if (type_tab == std::string::npos || line.substr(type_tab + 1) != "commit")
				{ context.error = "A local Git branch does not reference a commit."; return context; }
				WorkspaceGitBranch branch{line.substr(11, tab - 11), line.substr(tab + 1, type_tab - tab - 1)};
				if (!valid_commit(branch.commit)) { context.error = "A local Git branch has an invalid commit."; return context; }
				context.local_branches.push_back(std::move(branch));
			}
			std::string commit;
			std::string branch;
			if (!read(git + "rev-parse --verify HEAD", commit) || !read(git + "branch --show-current", branch) ||
			    commit != context.current_commit || branch != context.current_branch)
				context.error = "The repository changed while its context was loading.";
			return context;
		}
		std::string url;
		if (svn_enabled && svn("url", url))
		{
			context.vcs = "svn";
			context.checkout_url = url;
			std::string revision;
			std::string checked_url;
			std::string checked_revision;
			if (!svn("revision", revision) || revision.empty() ||
			    !std::all_of(revision.begin(), revision.end(), [](char value) { return value >= '0' && value <= '9'; }) ||
			    !svn("url", checked_url) || !svn("revision", checked_revision) ||
			    checked_url != url || checked_revision != revision)
				context.error = "The SVN checkout changed while its context was loading.";
			else context.revision = revision;
		}
		if (stop.stop_requested()) context.error = "Repository context was canceled.";
		else if (std::chrono::steady_clock::now() >= deadline) context.error = "Repository context loading timed out.";
		return context;
	}
}
