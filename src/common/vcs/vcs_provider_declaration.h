#pragma once

#include <array>
#include <span>
#include <string_view>

namespace uam::vcs
{
	/// <summary>Argument grammar for one admitted command; option lists are `|`-separated names.</summary>
	struct ReadOnlyCommandOptions
	{
		std::string_view short_switches;
		std::string_view short_values;
		std::string_view switches;
		std::string_view values;
		/// Switches that must all appear; `,` separates accepted spellings of one requirement.
		std::string_view required = {};
		/// Value options (long names, or `-x` for a short letter) whose values are filesystem paths.
		std::string_view path_values = {};
	};

	/// <summary>Subcommands (`|`-separated names) sharing one argument grammar.</summary>
	struct ReadOnlyCommand
	{
		std::string_view actions;
		ReadOnlyCommandOptions options;
	};

	// Only literal local-or-repository reads. Admitted diffs, logs and shows must disable Git's external helpers,
	// status must not recurse into submodule configuration, and logs/shows must not run gpg.program;
	// the global `-c` and `--paginate` options and every write subcommand stay unrecognized.
	inline constexpr std::array<ReadOnlyCommand, 3> kGitReadOnlyCommands = {{
	    {"status", {"bsz", "u", "--short|--branch|--show-stash|--porcelain|--porcelain=1|--porcelain=2|--long|--verbose|--ignored|--no-renames|--ignore-submodules|--ignore-submodules=all", "--untracked-files|--column", "--ignore-submodules,--ignore-submodules=all"}},
	    {"diff", {"abcpRsuwz", "UGOS", "--stat|--numstat|--shortstat|--name-only|--name-status|--summary|--check|--quiet|--exit-code|--cached|--staged|--no-index|--no-ext-diff|--no-textconv|--no-color|--color=never|--color=auto|--color=always|--ignore-space-at-eol|--ignore-all-space|--ignore-space-change|--word-diff|--word-diff=plain|--word-diff=color|--word-diff=porcelain", "--unified|--diff-filter|--word-diff-regex|--relative", "--no-ext-diff|--no-textconv", "-O"}},
	    {"log|show", {"abcgmprsuwz", "nUGOS", "--oneline|--stat|--numstat|--shortstat|--name-only|--name-status|--summary|--no-patch|--patch|--no-ext-diff|--no-textconv|--no-show-signature|--no-color|--color=never|--color=auto|--color=always|--all|--decorate|--no-decorate|--reverse|--first-parent|--merges|--no-merges|--date-order|--topo-order", "--max-count|--skip|--since|--until|--author|--committer|--grep|--format|--pretty|--date|--unified|--diff-filter", "--no-ext-diff|--no-textconv|--no-show-signature", "-O"}},
	}};

// Adjacent-literal concatenation keeps every SVN row a single constant string.
#define UAM_SVN_COMMON_READ_SWITCHES "--non-interactive|--quiet|--verbose|--xml|--incremental"
	// Only literal working-copy reads: URL operands are rejected by the filter. Diffs must use the internal engine so no
	// configured diff-cmd runs; --config-dir/--config-option and every write subcommand stay unrecognized.
	inline constexpr std::array<ReadOnlyCommand, 8> kSvnReadOnlyCommands = {{
	    {"status|stat|st", {"qvRu", "rcl", UAM_SVN_COMMON_READ_SWITCHES "|--show-updates|--no-ignore|--ignore-externals", "--depth|--changelist|--cl"}},
	    {"info", {"qvRu", "rcl", UAM_SVN_COMMON_READ_SWITCHES "|--no-newline|--recursive|--include-externals", "--show-item|--depth|--revision|--changelist|--cl"}},
	    {"log", {"qvRu", "rcl", UAM_SVN_COMMON_READ_SWITCHES "|--stop-on-copy|--use-merge-history|--with-all-revprops|--with-no-revprops", "--limit|--revision|--search|--search-and|--depth|--with-revprop"}},
	    {"list|ls", {"qvRu", "rcl", UAM_SVN_COMMON_READ_SWITCHES "|--recursive|--include-externals|--human-readable", "--depth|--revision"}},
	    {"cat", {"qvRu", "rcl", UAM_SVN_COMMON_READ_SWITCHES "|--ignore-keywords", "--revision"}},
	    {"blame|praise|annotate|ann", {"qvRu", "rcl", UAM_SVN_COMMON_READ_SWITCHES "|--use-merge-history|--force", "--revision"}},
	    {"propget|pget|pg|proplist|plist|pl", {"qvRu", "rcl", UAM_SVN_COMMON_READ_SWITCHES "|--recursive|--revprop|--strict|--no-newline|--show-inherited-props", "--depth|--revision|--changelist|--cl"}},
	    {"diff|di", {"qvRu", "rcl", UAM_SVN_COMMON_READ_SWITCHES "|--internal-diff|--summarize|--git|--ignore-properties|--properties-only|--show-copies-as-adds|--notice-ancestry|--patch-compatible|--force", "--revision|--change|--depth|--changelist|--cl|--old|--new", "--internal-diff", "--old|--new"}},
	}};
#undef UAM_SVN_COMMON_READ_SWITCHES

	/// <summary>What a system can do. VcsCommitService declines History, Update and OriginComparison for a system that does not declare them,
	/// and the commit panel offers them from the list sent with each status.</summary>
	enum class VcsCapability
	{
		Status,
		Diff,
		Commit,
		ChatComparison,
		RevisionComparison,
		BranchList,
		BranchCreate,
		BranchSwitch,
		WorkspaceIsolation,
		/// Paged revision history with changed paths and one revision's patch for a path.
		History,
		/// Bring the working copy up to the repository's latest revision.
		Update,
		/// Relate a branch or tag to the path it was copied from, for the history graph: the copy point, its own revisions and the origin's.
		OriginComparison,
	};

	/// <summary>Stable bridge name for a capability.</summary>
	constexpr std::string_view VcsCapabilityId(const VcsCapability capability)
	{
		switch (capability)
		{
		case VcsCapability::Status: return "status";
		case VcsCapability::Diff: return "diff";
		case VcsCapability::Commit: return "commit";
		case VcsCapability::ChatComparison: return "chatComparison";
		case VcsCapability::RevisionComparison: return "revisionComparison";
		case VcsCapability::BranchList: return "branchList";
		case VcsCapability::BranchCreate: return "branchCreate";
		case VcsCapability::BranchSwitch: return "branchSwitch";
		case VcsCapability::WorkspaceIsolation: return "workspaceIsolation";
		case VcsCapability::History: return "history";
		case VcsCapability::Update: return "update";
		case VcsCapability::OriginComparison: return "originComparison";
		}
		return "";
	}

	inline constexpr std::array<VcsCapability, 9> kGitCapabilities = {
	    VcsCapability::Status, VcsCapability::Diff, VcsCapability::Commit, VcsCapability::ChatComparison, VcsCapability::RevisionComparison,
	    VcsCapability::BranchList, VcsCapability::BranchCreate, VcsCapability::BranchSwitch, VcsCapability::WorkspaceIsolation};
	// SVN has no chat or revision comparison in the panel and cannot create a branch. Its branch listing and switching are local-only,
	// and its isolation is a managed local Git snapshot. Git has no History, Update or origin comparison in the panel yet.
	inline constexpr std::array<VcsCapability, 9> kSvnCapabilities = {
	    VcsCapability::Status, VcsCapability::Diff, VcsCapability::Commit,
	    VcsCapability::BranchList, VcsCapability::BranchSwitch, VcsCapability::WorkspaceIsolation,
	    VcsCapability::History, VcsCapability::Update, VcsCapability::OriginComparison};

	/// <summary>Static facts about one version-control system. Lives below the application layer so the
	/// command filter can read each system's allow-list.</summary>
	struct VcsProviderDeclaration
	{
		std::string_view id;
		std::string_view display_name;
		std::string_view executable;
		/// A global flag accepted before the subcommand; empty when the system has none.
		std::string_view optional_leading_flag;
		std::span<const ReadOnlyCommand> read_only_commands;
		std::span<const VcsCapability> capabilities;
	};

	inline constexpr VcsProviderDeclaration kGitProviderDeclaration{"git", "Git", "git", "--no-pager", kGitReadOnlyCommands, kGitCapabilities};
	inline constexpr VcsProviderDeclaration kSvnProviderDeclaration{"svn", "SVN", "svn", "", kSvnReadOnlyCommands, kSvnCapabilities};

	/// <summary>Every declared system, read by the literal command filter.</summary>
	inline constexpr std::array<const VcsProviderDeclaration*, 2> kVcsProviderDeclarations = {&kGitProviderDeclaration, &kSvnProviderDeclaration};
} // namespace uam::vcs
