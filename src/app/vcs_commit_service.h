#pragma once

#include "common/state/app_state.h"

#include <filesystem>
#include <functional>
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
		VcsCommitMessageSuggestion GenerateMessage(const AppState& app, const ChatSession& chat, VcsType type, const std::vector<std::string>& files) const;
		static std::string BuildCommitMessagePromptForTests(const VcsCommitStatus& status, const std::vector<std::string>& selected_files);
		static VcsCommitMessageSuggestion ParseWorkerOutputForTests(const std::string& output, std::string_view worker_provider_id = {});
		/// <summary>Runs the remote (no local filesystem) SVN selection scheduler against an injected `svn args...` runner.</summary>
		static bool CommitSvnSelectionForTests(const std::function<ProcessExecutionResult(const std::vector<std::string>&)>& svn, const std::string& message, const std::vector<std::string>& files, std::string& error);
		/// <summary>Applies SVN line counts to status rows through an injected `svn args...` runner.</summary>
		static void ApplySvnLineStatsForTests(const std::function<ProcessExecutionResult(const std::vector<std::string>&)>& svn, std::vector<VcsChangedFile>& files);
	};
} // namespace uam
