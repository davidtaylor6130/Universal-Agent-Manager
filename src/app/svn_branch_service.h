#pragma once

#include <filesystem>
#include <functional>
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
}
