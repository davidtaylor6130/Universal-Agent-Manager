#pragma once

#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

struct ChatSession;
struct ChatFolder;

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

	/// The adapter runs `svn args...` (argv only) within `timeout_ms`, fills `output` and returns true on success. On failure
	/// it may set `failure` to one line for the person, which the service adds to its own refusal.
	using SvnBranchRunner = std::function<bool(const std::vector<std::string>& args, int timeout_ms, std::string& output, std::string& failure)>;
	/// RunSvn-backed adapter: the shared SVN environment, `stop` ends a read in flight (never `svn switch` itself), failures
	/// read as SvnFailureMessage and are logged.
	SvnBranchRunner CreateSvnBranchRunner(std::stop_token stop = {});

	/// Captures local workspace ownership using the same projection as the composer bridge.
	SvnBranchScope BuildSvnBranchScope(bool enabled, const ChatSession&, const std::vector<ChatFolder>&,
	    const std::vector<ChatSession>& references);

	/// Explicit repository-relative parent; no trunk/branches convention or checkout is inferred.
	SvnBranchResult DiscoverSvnBranches(const SvnBranchScope&, const std::string& parent, const SvnBranchRunner&);
	/// Rechecks working-copy identity, clean status, foreign users and pinned discovery before switch.
	SvnBranchResult SwitchSvnBranch(const SvnBranchScope&, const SvnBranchContext& expected,
	    const std::string& path, const SvnBranchRunner&);
}
