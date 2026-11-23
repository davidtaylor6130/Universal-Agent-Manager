#pragma once

#include "app/svn_branch_service.h"
#include "app/vcs_commit_service.h"
#include "common/vcs/vcs_provider_declaration.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uam
{
	class RemoteVcsContext;

	struct VcsStatusRequest
	{
		const AppState& app;
		const ChatSession& chat;
		bool include_line_stats;
		std::string_view comparison_ref;
		bool context_only;
	};

	/// <summary>Identity of the place a located branch listing was read from. A later selection echoes it back, and the
	/// system refuses it when the working copy has moved since. Only SVN locates its listing today.</summary>
	using VcsBranchContext = SvnBranchContext;

	struct VcsBranch
	{
		std::string name;
		/// Repository path of the choice in a located listing; empty for a named branch.
		std::string path;
		bool current = false;
	};

	struct VcsBranchListing
	{
		bool ok = false;
		std::string error;
		/// The system that answered. Git's listing answers SVN, with no branches, for an SVN working copy so the picker can switch selector.
		VcsType type = VcsType::Git;
		std::vector<VcsBranch> branches;
		VcsBranchContext context;
	};

	/// <summary>One branch choice: a branch name, or a repository path from a located listing with the context it was listed in.</summary>
	struct VcsBranchSelection
	{
		std::string branch;
		bool create = false;
		VcsBranchContext context;
	};

	struct VcsBranchOutcome
	{
		bool ok = false;
		std::string error;
		/// The failure is repository state that moved after the listing, not an unusable choice.
		bool conflict = false;
		/// After a switch: what the picker shows as the current choice.
		std::string branch;
		/// After a switch in a located listing: the working copy's new identity.
		VcsBranchContext context;
		/// After a switch: the chat's saved branch fields were rewritten, so the live chat takes them over.
		bool chat_updated = false;
	};

	/// <summary>One version-control system behind VcsCommitService and the chat branch picker. To add a system, declare it in
	/// vcs_provider_declaration.h, implement this interface and register it in VcsProviders(). Implementations currently
	/// live in vcs_commit_service.cpp, which alone defines RemoteVcsContext.</summary>
	class VcsProvider
	{
	  public:
		virtual ~VcsProvider() = default;

		virtual VcsType Type() const = 0;
		virtual const vcs::VcsProviderDeclaration& Declaration() const = 0;
		virtual bool Enabled(const AppSettings& settings) const = 0;

		/// <summary>`root` arrives as the workspace and is replaced only when a repository root is known.</summary>
		virtual bool DetectLocal(const std::filesystem::path& workspace, bool validate, std::filesystem::path& root) const = 0;
		virtual bool DetectRemote(RemoteVcsContext& context, const std::string& workspace, std::string& root) const = 0;

		/// <summary>Fills branch or revision and changed files for an already detected and selected system.</summary>
		virtual void PopulateLocalStatus(const VcsStatusRequest& request, const std::filesystem::path& workspace, const std::filesystem::path& root, VcsCommitStatus& status) const = 0;
		virtual void PopulateRemoteStatus(RemoteVcsContext& context, const VcsStatusRequest& request, const std::string& root, VcsCommitStatus& status) const = 0;

		/// <summary>Returns the patch for one selected path. The remote form has the caller's already validated changed-file row.</summary>
		virtual std::string LocalDiff(const AppState& app, const ChatSession& chat, const std::filesystem::path& workspace, const std::string& trimmed_path, std::string_view comparison_ref, std::string* error_out) const = 0;
		virtual std::string RemoteDiff(RemoteVcsContext& context, const AppState& app, const ChatSession& chat, const std::string& path, const VcsChangedFile& selected, std::string cwd, const std::string& comparison_ref, std::string* error_out) const = 0;

		/// <summary>Publishes the selected paths. On false, `error` is set and the system's own staging state has been restored or the failure reported in it.</summary>
		virtual bool LocalCommit(const std::filesystem::path& workspace, const std::string& message, const std::vector<std::string>& files, const VcsCommitStatus& status, ProcessExecutionResult& committed, std::string& error) const = 0;
		virtual bool RemoteCommit(RemoteVcsContext& context, std::string cwd, const std::string& message, std::vector<std::string> selected_files, const VcsCommitStatus& status, ProcessExecutionResult& committed, std::string& error) const = 0;

		/// <summary>History and Update run through `run`, which already targets the local or remote workspace, so one
		/// implementation serves both. VcsCommitService calls these only for a system that declares the matching capability.</summary>
		virtual VcsHistoryPage History(const VcsCommandRunner& run, const VcsHistoryRequest& request) const;
		virtual std::string RevisionDiff(const VcsCommandRunner& run, const std::string& revision, const std::string& path, std::string& error) const;
		virtual VcsUpdateResult Update(const VcsCommandRunner& run) const;
		virtual VcsOriginReply Origin(const VcsCommandRunner& run, const VcsOriginRequest& request) const;

		// Chat branch picker operations. `snapshot` is a worker-owned copy of the settings, folders and chat references;
		// `chat` is the captured chat. Each runs on a worker thread and never touches live app state.

		/// <summary>True when choices are repository paths under an explicit parent and a selection must carry the
		/// listing's context (SVN). False when choices are the repository's named branches (Git).</summary>
		virtual bool BranchesAreLocated() const = 0;
		/// <summary>Lists the choices for the chat's workspace; `parent` is the explicit location for a located listing.</summary>
		virtual VcsBranchListing ListBranches(const AppState& snapshot, const ChatSession& chat, const std::string& parent) const = 0;
		/// <summary>Checks a choice when the picker submits it, before any runtime is stopped.</summary>
		virtual VcsBranchOutcome CheckBranchSelection(const AppState& snapshot, const ChatSession& chat, const VcsBranchSelection& selection) const = 0;
		/// <summary>Rechecks the workspace under the exclusive workspace lease, after the chat's runtime has settled.</summary>
		virtual VcsBranchOutcome CheckBranchSwitch(const AppState& snapshot, const ChatSession& chat, const VcsBranchSelection& selection) const = 0;
		/// <summary>Switches, or creates and switches. `chat` is updated in place when the system records the branch on the chat.</summary>
		virtual VcsBranchOutcome SwitchBranch(AppState& snapshot, ChatSession& chat, const VcsBranchSelection& selection) const = 0;
	};

	/// <summary>Registered providers in detection order; the first is the default selection.</summary>
	std::span<const VcsProvider* const> VcsProviders();
	const VcsProvider& VcsProviderFor(VcsType type);
	/// <summary>The provider whose declared id is exactly `id`; the default selection otherwise.</summary>
	const VcsProvider& VcsProviderForId(std::string_view id);
	bool DeclaresCapability(const VcsProvider& provider, vcs::VcsCapability capability);
} // namespace uam
