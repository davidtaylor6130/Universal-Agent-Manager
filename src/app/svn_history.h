#pragma once

#include "app/vcs_commit_service.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

/// <summary>SVN revision history, one revision's patch and working-copy update, behind the SVN VcsProvider.
/// Every function runs `svn` only through the injected runner, so local and SSH workspaces share one implementation.</summary>
namespace uam::svn_history
{
	inline constexpr int kMaxPageSize = 200;
	inline constexpr std::size_t kMaxPathsPerEntry = 200;

	/// <summary>Parses `svn log --xml` output. Only complete `<logentry>` elements are returned, so output cut at the
	/// capture limit yields the entries that arrived whole. Each entry keeps at most `max_paths` changed paths.</summary>
	std::vector<VcsHistoryEntry> ParseLogXml(std::string_view xml, std::size_t max_paths = kMaxPathsPerEntry);

	/// <summary>One page of the workspace path's history, newest first, including revisions newer than the working copy.</summary>
	VcsHistoryPage Log(const VcsCommandRunner& svn, const VcsHistoryRequest& request);

	/// <summary>The patch `revision` applied to the repository path `path` (as listed in that revision's entry).</summary>
	std::string RevisionDiff(const VcsCommandRunner& svn, std::string_view revision, std::string_view path, std::string& error);

	/// <summary>`svn update` with conflicts postponed for the user.</summary>
	VcsUpdateResult Update(const VcsCommandRunner& svn);

	inline constexpr std::size_t kMaxBehindRevisions = 5000;

	/// <summary>Relates the working copy's branch or tag to the path it was copied from, for the history graph. Reads committed state at HEAD only.</summary>
	VcsOriginReply Origin(const VcsCommandRunner& svn, const VcsOriginRequest& request);
} // namespace uam::svn_history
