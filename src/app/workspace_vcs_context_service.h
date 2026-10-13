#pragma once

#include <atomic>
#include <memory>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

namespace uam
{
	struct WorkspaceGitBranch
	{
		std::string name;
		std::string commit;
	};

	struct WorkspaceVcsContext
	{
		std::string vcs;
		std::string current_branch;
		std::string current_commit;
		bool detached = false;
		std::vector<WorkspaceGitBranch> local_branches;
		std::string checkout_url;
		std::string revision;
		std::string error;
	};

	/// <summary>Bounds read-only probes; skipped workers release their slot with the capture.</summary>
	std::shared_ptr<void> AcquireWorkspaceVcsContextSlot(const std::shared_ptr<std::atomic<unsigned>>& pending);

	/// <summary>Reads local repository context without switching, fetching, or changing files.</summary>
	WorkspaceVcsContext ReadWorkspaceVcsContext(const std::filesystem::path& directory,
	    bool git_enabled, bool svn_enabled, std::stop_token stop = {});
}
