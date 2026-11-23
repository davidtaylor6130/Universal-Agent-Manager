#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

struct ChatSession;
struct ChatFolder;
struct SvnRepositoryLayout;

namespace uam
{
	struct SvnBranchScope
	{
		std::filesystem::path directory;
		std::filesystem::path folder;
		std::vector<std::filesystem::path> other_workspaces;
		bool editable = false;
	};

	struct SvnBranchContext
	{
		std::string directory, url, repository, uuid, revision, parent, repository_revision;
		bool operator==(const SvnBranchContext&) const = default;
	};

	struct SvnBranchChoice
	{
		std::string name, path;
		bool current = false;
	};

	struct SvnBranchResult
	{
		bool ok = false;
		SvnBranchContext context;
		std::vector<SvnBranchChoice> branches;
		std::string error;
	};

	/// The adapter executes a bounded argv command through the platform process service.
	using SvnBranchRunner = std::function<bool(const std::vector<std::string>&, int, std::string&)>;
	SvnBranchRunner CreateSvnBranchRunner();

	/// Captures local workspace ownership using the same projection as the composer bridge.
	SvnBranchScope BuildSvnBranchScope(bool enabled, const ChatSession&, const std::vector<ChatFolder>&,
	    const std::vector<ChatSession>& references);

	/// Explicit repository-relative parent; no trunk/branches convention or checkout is inferred.
	SvnBranchResult DiscoverSvnBranches(const SvnBranchScope&, const std::string& parent, const SvnBranchRunner&);
	/// Rechecks working-copy identity, clean status, foreign users and pinned discovery before switch.
	SvnBranchResult SwitchSvnBranch(const SvnBranchScope&, const SvnBranchContext& expected,
	    const std::string& path, const SvnBranchRunner&);

	/// Where a new branch starts: the working copy's own URL at its revision, or the main line (trunk) at the repository's newest revision.
	enum class SvnBranchSource
	{
		WorkingCopy,
		MainLine,
	};

	struct SvnBranchCreateRequest
	{
		std::string name;
		SvnBranchSource source = SvnBranchSource::WorkingCopy;
		/// One line; the commit message of the server-side copy.
		std::string message;
	};

	/// Repository-relative (`^/...`) folders that branch creation uses for a working copy, from the repository's layout.
	struct SvnBranchLocations
	{
		bool ok = false;
		std::string branches, main_line;
	};

	/// Reads the layout saved for the working copy's repository (root URL or UUID; `trunk`/`branches` otherwise). When the
	/// working copy sits under a project folder (`^/shop/trunk/...`), both folders are under that project folder too.
	SvnBranchLocations SvnBranchLocationsFor(const SvnBranchContext&, const std::vector<SvnRepositoryLayout>&);
	/// Same working copy: directory, URL, repository root, UUID and revision; the listed parent and its revision are ignored.
	bool SameSvnWorkingCopy(const SvnBranchContext&, const SvnBranchContext&);
	/// Name and message checks that need no repository access; `error` is one line for the person.
	bool ValidSvnBranchCreate(const SvnBranchCreateRequest&, std::string& error);
	/// What a creation did to the repository, for the diagnostic log and the reply.
	enum class SvnBranchCreateOutcome
	{
		/// Refused before anything was committed.
		Refused,
		/// The copy failed and the branch is confirmed absent.
		NotCreated,
		/// The repository may have changed in an unconfirmed way (no confirmation, or a copy nested into another folder).
		NeedsInspection,
		/// Committed; the working copy stayed where it was.
		CreatedNotSwitched,
		CreatedAndSwitched,
	};

	struct SvnBranchCreateResult : SvnBranchResult
	{
		SvnBranchCreateOutcome outcome = SvnBranchCreateOutcome::Refused;
	};

	/// Copies the chosen source to `^/<branches>/<name>` on the server (one commit, pinned source revision, never `--parents`),
	/// confirms that commit from the repository log, then switches the working copy straight to that commit with the folder
	/// switch's checks. The working copy must still match `expected` (directory, URL, repository, UUID and revision) and be
	/// clean before anything is committed. On success `branches` holds the new branch, marked current; after a commit whose
	/// switch did not finish it holds the branch marked not current, and `error` says the branch was created.
	SvnBranchCreateResult CreateSvnBranch(const SvnBranchScope&, const SvnBranchContext& expected, const SvnBranchCreateRequest&,
	    const std::vector<SvnRepositoryLayout>&, const SvnBranchRunner&);

	/// <summary>True for the svn commands branch selection and creation run that change the repository or the working copy
	/// (`copy` commits, `switch` rewrites the working copy). A runner that can be stopped must let these finish once started:
	/// stopping one midway leaves an unconfirmed commit or a locked, half-switched working copy.</summary>
	bool SvnCommandChangesRepository(const std::vector<std::string>& args);
}
