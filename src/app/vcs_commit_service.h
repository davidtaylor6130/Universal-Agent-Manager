#pragma once

#include "common/state/app_state.h"

#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace uam
{
	/// <summary>Captures only repository/provider context, never transcript or tool payloads.</summary>
	ChatSession CaptureVcsChatContext(const ChatSession& chat);

	enum class ChatBaselineReadiness { Pending, Ready, Unavailable, Failed };

	/// <summary>Start one snapshot worker per chat; provider launches wait for its durable result.</summary>
	ChatBaselineReadiness PrepareChatRepositoryBaseline(AppState& app, const ChatSession& chat, std::string* error_out = nullptr, std::shared_ptr<std::stop_source>* cancellation_out = nullptr, bool retry_failed = false);
	void CancelChatRepositoryBaseline(AppState& app, const std::string& chat_id);
	ChatBaselineReadiness WaitForChatRepositoryBaseline(const std::shared_ptr<ChatBaselinePreparation>& task, std::stop_token cancellation, std::string* error_out);

	enum class VcsType
	{
		Git,
		Svn
	};

	struct VcsChangedFile
	{
		std::string path;
		std::string status;
		bool staged = false;
		int additions = 0;
		int deletions = 0;
		bool binary = false;
		std::string content_fingerprint;
		std::string previous_path;
	};

	struct VcsCommitStatus
	{
		bool available = false;
		std::vector<VcsType> vcs_types;
		VcsType active_vcs_type = VcsType::Git;
		std::string workspace_directory;
		std::string branch_or_revision;
		std::vector<VcsChangedFile> changed_files;
		bool line_stats_ready = true;
		std::string warning;
		std::string error;
		/// The chat comparison has no usable baseline: Retry cannot help, unlike any other read failure in `error`.
		bool baseline_unavailable = false;
	};

	struct VcsCommitResult
	{
		bool ok = false;
		VcsCommitStatus status;
		std::string message;
		std::string error;
	};

	struct VcsCommitMessageSuggestion
	{
		bool ok = false;
		std::string title;
		std::string description;
		std::string error;
	};

	/// <summary>One path a revision changed. Revisions are opaque strings so a later system can use hashes.</summary>
	struct VcsHistoryPath
	{
		std::string action;
		/// Repository path as the system reports it (SVN: absolute within the repository, such as `/trunk/a.txt`).
		std::string path;
		std::string kind;
		std::string copy_from_path;
		std::string copy_from_revision;
	};

	struct VcsHistoryEntry
	{
		std::string revision;
		std::string author;
		/// ISO 8601 UTC as reported by the system.
		std::string date;
		std::string message;
		std::vector<VcsHistoryPath> paths;
		/// Changed paths left out of `paths` to keep the reply bounded.
		std::size_t paths_omitted = 0;
	};

	struct VcsHistoryRequest
	{
		/// Empty for the newest page; otherwise the `next` cursor of the previous page.
		std::string before;
		int limit = 50;
		/// Optional text the system matches against author, date, message and changed paths.
		std::string search;
	};

	struct VcsHistoryPage
	{
		std::vector<VcsHistoryEntry> entries;
		/// Cursor for the following page; empty at the end of history.
		std::string next;
		/// Filled on the newest page: the working copy's revision and its location in the repository.
		std::string working_revision;
		std::string location;
		/// Filled on the newest page: identifies the repository for per-repository settings.
		std::string repository_root;
		std::string repository_uuid;
		std::string error;
	};

	/// <summary>How a branch or tag relates to the path it was copied from, for the history graph. `view` selects one
	/// part: `summary` (the copy point and the origin revisions it lacks), `ahead` (a page of its own revisions since
	/// the copy) or `behind` (details for the listed origin revisions).</summary>
	struct VcsOriginRequest
	{
		std::string view = "summary";
		std::string before;
		int limit = 50;
		/// `behind`: revisions taken from the summary's list.
		std::vector<std::string> revisions;
	};

	struct VcsOriginReply
	{
		std::string error;
		/// Empty `origin_path` means this line of history was not copied from anywhere (a main line).
		std::string branch_path;
		std::string origin_path;
		std::string copy_revision;
		std::string origin_revision;
		/// Origin revisions SVN's merge tracking would still merge, newest first.
		std::vector<std::string> behind_revisions;
		/// The branch records a merge from the origin, so `behind_revisions` already leaves out what was merged at its root.
		bool merges_recorded = false;
		/// Merge tracking could not be read; `behind_revisions` is every origin revision since the copy.
		bool behind_from_log = false;
		VcsHistoryPage page;
	};

	struct VcsUpdateResult
	{
		bool ok = false;
		/// The update finished but left conflicts for the user to resolve.
		bool conflicts = false;
		std::string revision;
		std::string output;
		std::string error;
	};

	/// <summary>Runs one system's client with `args` in the captured workspace, locally or on its SSH host, honouring the caller's cancellation.</summary>
	using VcsCommandRunner = std::function<ProcessExecutionResult(const std::vector<std::string>& args, int timeout_ms)>;

	std::string VcsTypeToString(VcsType type);
	VcsType VcsTypeFromString(std::string_view value);

	/// <summary>Runs `svn args...` as argv (no shell) in `cwd` with a UTF-8 locale; a missing client gets a clear error.</summary>
	ProcessExecutionResult RunSvn(const std::filesystem::path& cwd, const std::vector<std::string>& args, int timeout_ms = 120000, std::stop_token stop = {});
	/// <summary>Bounded failure detail: the missing-client notice or svn's first "E<code>: ..." or "W<code>: ..." line; empty when neither exists.</summary>
	std::string SvnFailureSummary(const ProcessExecutionResult& result);
	/// <summary>Strict `svn status` text parser. Rows need seven valid status columns and a separator; headers and
	/// tree-conflict details are skipped. Codes keep their column positions (right-trimmed). Absolute paths under
	/// `workspace` become workspace-relative.</summary>
	std::vector<VcsChangedFile> ParseSvnStatusOutput(std::string_view output, std::string_view workspace = {});

	class VcsCommitService
	{
	  public:
		bool CaptureChatBaseline(const AppState& app, const ChatSession& chat, std::string* error_out = nullptr, std::stop_token cancellation = {}) const;
		VcsCommitStatus Status(const AppState& app, const ChatSession& chat, VcsType requested_type = VcsType::Git, bool include_line_stats = true, std::string_view comparison_ref = {}, bool context_only = false) const;
		std::string Diff(const AppState& app, const ChatSession& chat, const std::string& path, VcsType type, std::string* error_out = nullptr, std::string_view comparison_ref = {}) const;
		VcsCommitResult Commit(const AppState& app, const ChatSession& chat, VcsType type, const std::string& message, const std::vector<std::string>& files) const;
		/// <summary>One page of the workspace path's history, newest first. Never blocks beyond the system's client; `stop` ends it early.</summary>
		VcsHistoryPage History(const AppState& app, const ChatSession& chat, VcsType type, const VcsHistoryRequest& request, std::stop_token stop = {}) const;
		/// <summary>The patch one revision applied to one repository path taken from that revision's history entry.</summary>
		std::string RevisionDiff(const AppState& app, const ChatSession& chat, VcsType type, const std::string& revision, const std::string& path, std::string* error_out = nullptr, std::stop_token stop = {}) const;
		/// <summary>Updates the working copy to the newest revision, postponing conflicts; never commits or discards local changes.</summary>
		VcsUpdateResult Update(const AppState& app, const ChatSession& chat, VcsType type, std::stop_token stop = {}) const;
		/// <summary>Compares the workspace's branch or tag with the path it was copied from; see VcsOriginRequest.</summary>
		VcsOriginReply Origin(const AppState& app, const ChatSession& chat, VcsType type, const VcsOriginRequest& request, std::stop_token stop = {}) const;
		VcsCommitMessageSuggestion GenerateMessage(const AppState& app, const ChatSession& chat, VcsType type, const std::vector<std::string>& files) const;
		static std::string BuildCommitMessagePromptForTests(const VcsCommitStatus& status, const std::vector<std::string>& selected_files);
		static VcsCommitMessageSuggestion ParseWorkerOutputForTests(const std::string& output, std::string_view worker_provider_id = {});
		/// <summary>Runs the remote (no local filesystem) SVN selection scheduler against an injected `svn args...` runner.</summary>
		static bool CommitSvnSelectionForTests(const std::function<ProcessExecutionResult(const std::vector<std::string>&)>& svn, const std::string& message, const std::vector<std::string>& files, std::string& error);
		/// <summary>Applies SVN line counts to status rows through an injected `svn args...` runner.</summary>
		static void ApplySvnLineStatsForTests(const std::function<ProcessExecutionResult(const std::vector<std::string>&)>& svn, std::vector<VcsChangedFile>& files);
	};
} // namespace uam
