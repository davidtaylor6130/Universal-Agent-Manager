#pragma once

// Pull request watch rules: what counts as news, when a pull request is read, and how `gh api`
// answers are decoded. Pure functions with no I/O. The behaviour follows T3 Code's
// pullRequestWatch.ts and PullRequestSyncReactor.ts (MIT, github.com/pingdotgg/t3code).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uam::pr_watch
{
	/// Bounds consecutive comment-only wakes to stop bot reply loops; check, conflict or push news resets it.
	inline constexpr int kWakeLimit = 10;
	/// Consecutive failed reads after which watching ends.
	inline constexpr int kFailedReadLimit = 8;
	/// How long a head may go without any check before the agent is told none reported.
	inline constexpr std::int64_t kNoChecksMs = 15 * 60 * 1000;
	inline constexpr std::int64_t kSweepIntervalMs = 60 * 1000;
	inline constexpr std::int64_t kWatchIntervalMs = 2 * 60 * 1000;
	inline constexpr std::int64_t kSlowIntervalMs = 15 * 60 * 1000;
	inline constexpr std::int64_t kQuietAfterMs = 30 * 60 * 1000;
	inline constexpr std::int64_t kQuietIntervalMs = 5 * 60 * 1000;
	inline constexpr std::int64_t kClosedFinalAfterMs = 24 * 60 * 60 * 1000;

	struct Reference
	{
		/// "owner/name" as GitHub spells it.
		std::string repository;
		int number = 0;
		bool operator==(const Reference&) const = default;
	};

	/// Accepts a github.com pull request URL, "owner/name#12", or "owner/name" with a number.
	std::optional<Reference> ParseReference(std::string_view url_or_repository, int number = 0);
	/// Case-insensitive identity, "owner/name#12".
	std::string Key(const Reference& reference);
	std::string Url(const Reference& reference);

	struct Check
	{
		std::string name;
		/// success, failure, cancelled, pending, action-required or neutral.
		std::string status;
		std::string url;
		bool required = false;
		bool operator==(const Check&) const = default;
	};

	struct Remark
	{
		std::string id;
		std::string author;
		std::string body;
		/// File path for a review comment; empty otherwise.
		std::string path;
		std::string url;
		/// Review verdict such as "approved" for a review without text.
		std::string review_state;
		std::int64_t at_ms = 0;
	};

	struct Detail
	{
		/// open, closed or merged.
		std::string state = "open";
		std::string title;
		std::string head_sha;
		std::string base_branch;
		std::string author;
		/// Signed-in GitHub account; its own remarks never wake.
		std::string viewer;
		/// mergeable, conflicting or unknown.
		std::string mergeability = "unknown";
		std::int64_t updated_at_ms = 0;
		std::int64_t closed_at_ms = 0;
		bool fork = false;
		std::vector<Check> checks;
		/// A check source could not be read, so success and "no checks" cannot be established.
		bool checks_unread = false;
	};

	/// What one chat has already been told about one pull request.
	struct Reported
	{
		std::string head_sha;
		std::vector<std::string> failed_checks;
		bool passed = false;
		std::vector<std::string> passed_checks;
		std::vector<std::string> owed_checks;
		std::vector<std::string> awaiting_approval;
		bool approval_announced = false;
		std::int64_t remarks_through_ms = 0;
		std::vector<std::string> remark_ids;
		bool conflicting = false;
		int wakes = 0;
		/// Zero when no "no checks" clock is running.
		std::int64_t checks_awaited_since_ms = 0;
		bool operator==(const Reported&) const = default;
	};

	enum class ChangeKind { ChecksFailed, ChecksPassed, ChecksAwaitingApproval, Remarks, NoChecks, Conflicting };

	struct Change
	{
		ChangeKind kind = ChangeKind::ChecksFailed;
		std::vector<Check> checks;
		std::vector<Remark> remarks;
		int count = 0;
		bool required = false;
		int minutes = 0;
	};

	struct Report
	{
		std::vector<Change> changes;
		Reported next;
		/// The comment-only wake limit was reached; watching ends with this report.
		bool exhausted = false;
	};

	/// `remarks` is null when comments were not read; nothing is then reported or forgotten.
	Report Evaluate(const Reported& watch, const Detail& detail, const std::vector<Remark>* remarks, std::int64_t now_ms);

	struct Message
	{
		/// Text delivered into the chat.
		std::string text;
		/// One line for the notifications panel, such as "#12: checks failed".
		std::string summary;
		/// failed, completed or updated.
		std::string outcome;
	};

	Message BuildMessage(const Reference& reference, const Detail& detail, const Report& report);
	/// The one final message for a pull request that was merged or closed; watching ends with it.
	Message BuildEndedMessage(const Reference& reference, std::string_view state);

	struct CadenceInput
	{
		/// A chat is watching this pull request.
		bool watched = false;
		/// A chat that still exists links it.
		bool linked = false;
		bool has_snapshot = false;
		/// The last read answered "not found".
		bool missing = false;
		std::string state;
		std::int64_t updated_at_ms = 0;
		std::int64_t closed_at_ms = 0;
		/// Zero when never read in this app run.
		std::int64_t last_read_ms = 0;
	};

	bool IsDue(const CadenceInput& input, std::int64_t now_ms);

	struct GhResponse
	{
		/// Zero when `gh` produced no HTTP answer (not installed, signed out or offline).
		int status = 0;
		std::string etag;
		std::string body;
		bool rate_limited = false;
		/// When a rate-limited request may be retried.
		std::int64_t retry_at_ms = 0;
	};

	/// Decodes the output of `gh api -i`.
	GhResponse ParseGhApiOutput(std::string_view output, std::int64_t now_ms);

	/// UTC "2026-01-02T03:04:05Z" to epoch milliseconds; zero when malformed.
	std::int64_t ParseIsoMs(std::string_view iso);
	std::string FormatIsoMs(std::int64_t epoch_ms);

	/// Each decoder returns false when the body is not the expected GitHub JSON.
	bool DecodePull(std::string_view body, Detail& detail);
	bool DecodeCheckRuns(std::string_view body, std::vector<Check>& checks);
	bool DecodeCombinedStatus(std::string_view body, std::vector<Check>& checks);
	/// Workflow runs held for a maintainer's approval on a fork's pull request.
	bool DecodeHeldWorkflowRuns(std::string_view body, std::vector<Check>& checks);
	bool DecodeIssueComments(std::string_view body, std::vector<Remark>& remarks);
	bool DecodeReviewComments(std::string_view body, std::vector<Remark>& remarks);
	bool DecodeReviews(std::string_view body, std::vector<Remark>& remarks);
	/// Names of the required checks from the GraphQL rollup answer.
	bool DecodeRequiredChecks(std::string_view body, std::vector<std::string>& names);
	bool DecodeViewer(std::string_view body, std::string& login);
} // namespace uam::pr_watch
