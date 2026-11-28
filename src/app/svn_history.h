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
	/// <summary>Bound for one read (`info`, `status`, `log`) on a large or distant working copy.</summary>
	inline constexpr int kReadTimeoutMs = 120000;
	/// <summary>A large checkout can take minutes to update; the caller's stop token still ends it early.</summary>
	inline constexpr int kUpdateTimeoutMs = 15 * 60 * 1000;
	inline constexpr std::size_t kMaxPathsPerEntry = 200;

	/// <summary>Parses `svn log --xml` output. Only complete `<logentry>` elements are returned, so output cut at the
	/// capture limit yields the entries that arrived whole. Each entry keeps at most `max_paths` changed paths.</summary>
	std::vector<VcsHistoryEntry> ParseLogXml(std::string_view xml, std::size_t max_paths = kMaxPathsPerEntry);

	/// <summary>One page of the workspace path's history, newest first, including revisions newer than the working copy.</summary>
	VcsHistoryPage Log(const VcsCommandRunner& svn, const VcsHistoryRequest& request);

	/// <summary>The patch `revision` applied to the repository path `path` (as listed in that revision's entry).</summary>
	std::string RevisionDiff(const VcsCommandRunner& svn, std::string_view revision, std::string_view path, std::string& error);

	/// <summary>`svn update` with conflicts postponed for the user. A broken `svn:externals` definition is a warning, not a failure.</summary>
	VcsUpdateResult Update(const VcsCommandRunner& svn);

	/// <summary>Whether an `svn update` run finished updating the working copy. svn exits 1 with only E205011 when an
	/// external could not be updated, after the working copy itself was; any other error, a timeout or a stop is unfinished.</summary>
	bool UpdateFinished(const ProcessExecutionResult& result);
	/// <summary>True when svn refused the target because it is not under version control (W155010, E155007, E155010).</summary>
	bool NotVersioned(const ProcessExecutionResult& result);
	inline constexpr std::string_view kNotVersionedFolder = "This folder is not under version control, so SVN has nothing to update here. Add it to SVN, or update the folder that holds it.";
	/// <summary>One "External 'x' could not be updated." line per external svn reported (W205011), in order.</summary>
	std::vector<std::string> ExternalUpdateWarnings(std::string_view output);

	/// <summary>True when a status row is text- (column 1), property- (column 2) or tree- (column 7) conflicted.</summary>
	bool IsConflicted(const VcsChangedFile& file);
	/// <summary>Workspace-relative leftover files (`.mine`, `.rN`, `.prej` and the like) named by one `svn info --xml`
	/// reply for conflicted items. Paths use `/`; svn reports them absolute, so each is placed beside (or, for a folder,
	/// inside) the entry that reported it.</summary>
	std::vector<std::string> ParseConflictLeftovers(std::string_view info_xml);
	/// <summary>Reads the leftover files svn left for every conflicted row in `files` and returns the unversioned rows among
	/// them, in their status spelling. False with `error` when svn could not say, so the caller can refuse every new file.</summary>
	bool ConflictLeftovers(const VcsCommandRunner& svn, const std::vector<VcsChangedFile>& files, std::vector<std::string>& leftovers, std::string& error);

	/// <summary>`svn resolve --accept <accept>` for one conflicted workspace-relative path. `accept` is `working`,
	/// `theirs-full` or `mine-full`; a tree conflict accepts only `working`. Empty on success, otherwise the reason.</summary>
	std::string Resolve(const VcsCommandRunner& svn, std::string_view path, std::string_view accept, std::string& output);

	/// <summary>The working-copy revision or mixed range (`5`, `3:5`) from `svnversion` output; empty when it has none.</summary>
	std::string ParseSvnVersion(std::string_view output);

	inline constexpr std::size_t kMaxBehindRevisions = 5000;

	/// <summary>Relates the working copy's branch or tag to the path it was copied from, for the history graph. Reads committed state at HEAD only.</summary>
	VcsOriginReply Origin(const VcsCommandRunner& svn, const VcsOriginRequest& request);
} // namespace uam::svn_history

namespace uam
{
	/// <summary>Strict `svn status` text parser. Rows need seven valid status columns and a separator; headers and
	/// tree-conflict details are skipped, as are rows that report only a lock. Codes keep their column positions (right-trimmed). Absolute paths under
	/// `workspace` become workspace-relative.</summary>
	std::vector<VcsChangedFile> ParseSvnStatusOutput(std::string_view output, std::string_view workspace = {});
} // namespace uam
