#include "test_harness.h"
#include "app/pr_watch_service.h"
#include "app/uam_control_service.h"
#include "common/pr_watch/pr_watch_rules.h"
#include "common/utils/time_utils.h"

#include <map>
#include <mutex>

using namespace uam_test;
namespace prw = uam::pr_watch;

namespace
{
	const std::int64_t kNow = prw::ParseIsoMs("2026-11-11T12:00:00Z");
	const std::string kSha(40, 'a');
	const std::string kNextSha(40, 'b');
	constexpr std::int64_t kMinute = 60 * 1000;

	prw::Detail OpenDetail(std::vector<prw::Check> checks = {}, std::string sha = kSha)
	{
		prw::Detail detail;
		detail.head_sha = std::move(sha);
		detail.base_branch = "main";
		detail.author = "author";
		detail.viewer = "me";
		detail.mergeability = "mergeable";
		detail.checks = std::move(checks);
		return detail;
	}

	prw::Remark RemarkAt(std::string id, std::string author, std::int64_t at_ms, std::string body = "Please rename this.")
	{
		return {.id = std::move(id), .author = std::move(author), .body = std::move(body), .url = "https://github.com/acme/app/pull/12#c", .at_ms = at_ms};
	}

	bool Has(const prw::Report& report, prw::ChangeKind kind)
	{
		return std::ranges::any_of(report.changes, [kind](const prw::Change& change) { return change.kind == kind; });
	}

	/// Stands in for `gh api -i`: answers from a table, honours If-None-Match and records every call. Never touches the network.
	struct FakeGh
	{
		struct Answer
		{
			int status = 200;
			std::string etag;
			std::string body;
			std::string headers;
		};
		std::mutex mutex;
		std::map<std::string, Answer> answers;
		std::vector<std::vector<std::string>> calls;
		bool signed_out = false;

		void Set(const std::string& endpoint, nlohmann::json body, std::string etag = {})
		{
			std::lock_guard lock(mutex);
			answers[endpoint] = {.etag = etag.empty() ? "\"" + std::to_string(std::hash<std::string>{}(body.dump())) + "\"" : std::move(etag), .body = body.dump()};
		}
		void Fail(const std::string& endpoint, int status, std::string headers = {}, std::string body = "{}")
		{
			std::lock_guard lock(mutex);
			answers[endpoint] = {.status = status, .body = std::move(body), .headers = std::move(headers)};
		}
		std::string Run(const std::vector<std::string>& arguments)
		{
			std::lock_guard lock(mutex);
			calls.push_back(arguments);
			if (signed_out) return "To get started with GitHub CLI, please run:  gh auth login\n";
			std::string endpoint = arguments.size() > 1 && arguments[1] == "graphql" ? "graphql" : arguments.at(2);
			endpoint = endpoint.substr(0, endpoint.find('?'));
			const auto found = answers.find(endpoint);
			if (found == answers.end()) return "HTTP/2.0 404 Not Found\r\n\r\n{\"message\":\"Not Found\"}";
			const Answer& answer = found->second;
			const bool unchanged = answer.status == 200 && std::ranges::find(arguments, "If-None-Match: " + answer.etag) != arguments.end();
			if (unchanged) return "HTTP/2.0 304 Not Modified\r\nEtag: " + answer.etag + "\r\n\r\n";
			return "HTTP/2.0 " + std::to_string(answer.status) + " Answer\r\n" + (answer.etag.empty() ? "" : "Etag: " + answer.etag + "\r\n") + answer.headers + "\r\n" + answer.body;
		}
		std::size_t Count(std::string_view endpoint_prefix)
		{
			std::lock_guard lock(mutex);
			return static_cast<std::size_t>(std::ranges::count_if(calls, [&](const std::vector<std::string>& call) { return call.size() > 2 && call[2].starts_with(endpoint_prefix); }));
		}
		std::size_t Conditional()
		{
			std::lock_guard lock(mutex);
			return static_cast<std::size_t>(std::ranges::count_if(calls, [](const std::vector<std::string>& call) {
				return std::ranges::any_of(call, [](const std::string& argument) { return argument.starts_with("If-None-Match: "); });
			}));
		}
		std::size_t Calls()
		{
			std::lock_guard lock(mutex);
			return calls.size();
		}
	};

	nlohmann::json PullBody(const std::string& sha = kSha, bool merged = false)
	{
		nlohmann::json pull = nlohmann::json::parse(R"({"state":"open","merged":false,"title":"Add the parser","mergeable":true,"mergeable_state":"clean",
		    "updated_at":"2026-11-11T11:59:00Z","closed_at":null,"user":{"login":"author"},"head":{"sha":"","repo":{"id":1}},"base":{"ref":"main","repo":{"id":1}}})");
		pull["head"]["sha"] = sha;
		if (merged)
		{
			pull["state"] = "closed";
			pull["merged"] = true;
			pull["closed_at"] = "2026-11-11T11:59:00Z";
		}
		return pull;
	}

	nlohmann::json CheckRuns(std::vector<std::pair<std::string, std::string>> runs)
	{
		nlohmann::json list = nlohmann::json::array();
		for (const auto& [name, conclusion] : runs)
			list.push_back({{"name", name}, {"status", conclusion.empty() ? "in_progress" : "completed"},
			    {"conclusion", conclusion.empty() ? nlohmann::json(nullptr) : nlohmann::json(conclusion)}, {"html_url", "https://github.com/acme/app/actions/runs/7/job/" + name}});
		return {{"check_runs", std::move(list)}};
	}

	const prw::Reference kPr{.repository = "acme/app", .number = 12};
	const std::string kRoot = "repos/acme/app";

	struct WatchFixture
	{
		TempDir temp{"uam-pr-watch"};
		uam::AppState app;
		std::shared_ptr<FakeGh> gh = std::make_shared<FakeGh>();
		std::shared_ptr<std::vector<std::pair<std::string, std::string>>> wakes = std::make_shared<std::vector<std::pair<std::string, std::string>>>();
		std::shared_ptr<bool> accept = std::make_shared<bool>(true);
		std::unique_ptr<uam::PrWatchService> service;
		std::int64_t now = kNow;

		WatchFixture()
		{
			app.data_root = temp.root;
			AddChat("chat-1");
			gh->Set(kRoot + "/pulls/12", PullBody());
			SetChecks(kSha, {});
			gh->Set(kRoot + "/issues/12/comments", nlohmann::json::array());
			gh->Set(kRoot + "/pulls/12/comments", nlohmann::json::array());
			gh->Set(kRoot + "/pulls/12/reviews", nlohmann::json::array());
			gh->Set("user", {{"login", "me"}});
			gh->Set("graphql", nlohmann::json::parse(R"({"data":{"repository":{"pullRequest":{"commits":{"nodes":[]}}}}})"));
			Restart();
		}
		void AddChat(const std::string& id)
		{
			ChatSession chat;
			chat.id = id;
			chat.title = "Parser work";
			app.chats.push_back(chat);
		}
		void SetChecks(const std::string& sha, std::vector<std::pair<std::string, std::string>> runs)
		{
			gh->Set(kRoot + "/commits/" + sha + "/check-runs", CheckRuns(std::move(runs)));
			gh->Set(kRoot + "/commits/" + sha + "/status", {{"statuses", nlohmann::json::array()}});
		}
		/// A new service on the same data folder, as after an app restart.
		void Restart()
		{
			service = std::make_unique<uam::PrWatchService>(app.data_root,
			    [gh = gh](const std::vector<std::string>& arguments, std::stop_token) { return gh->Run(arguments); },
			    [wakes = wakes, accept = accept](uam::AppState&, const std::string& chat_id, const std::string& text, std::string* error) {
				    if (!*accept) { *error = "The provider is unavailable."; return false; }
				    wakes->emplace_back(chat_id, text);
				    return true;
			    });
		}
		void Watch(const std::string& chat_id = "chat-1")
		{
			std::string error;
			UAM_ASSERT(service->Watch(app, chat_id, kPr, now, &error));
		}
		/// One due sweep: start the read, let it finish, apply it and deliver wakes.
		void Sweep()
		{
			service->Poll(app, now);
			service->WaitForWorkerForTests();
			// The answer arrives a few seconds after the sweep started, as real reads do.
			service->Poll(app, now + 5000);
		}
		void SweepAfter(std::int64_t milliseconds)
		{
			now += milliseconds;
			Sweep();
		}
	};
} // namespace

UAM_TEST(PrWatchParsesUrlsAndRepositoryReferences)
{
	UAM_ASSERT(prw::ParseReference("https://github.com/acme/app/pull/12") == std::optional<prw::Reference>(kPr));
	UAM_ASSERT(prw::ParseReference(" https://github.com/acme/app/pull/12/files?diff=split#top ") == std::optional<prw::Reference>(kPr));
	UAM_ASSERT(prw::ParseReference("acme/app#12") == std::optional<prw::Reference>(kPr));
	UAM_ASSERT(prw::ParseReference("acme/app", 12) == std::optional<prw::Reference>(kPr));
	UAM_ASSERT_EQ(prw::Key({.repository = "Acme/App", .number = 12}), std::string("acme/app#12"));
	UAM_ASSERT_EQ(prw::Url(kPr), std::string("https://github.com/acme/app/pull/12"));
	// Anything that is not one github.com pull request is refused, including text that could reach the shell or another host.
	for (const char* bad : {"", "acme/app", "https://github.com/acme/app/issues/12", "https://gitlab.com/acme/app/pull/12", "acme/app#0", "acme/app#x",
	         "acme/app; rm -rf ~#1", "../..#1", "acme/app/extra#1", "https://github.com/acme/app/pull/", "https://evil.example/a#5", "http://example.com/acme/app#3"})
		UAM_ASSERT(!prw::ParseReference(bad).has_value());
}

UAM_TEST(PrWatchReportsAFailedCheckOnceAndAgainAfterAPush)
{
	prw::Reported watch;
	const prw::Detail failing = OpenDetail({{.name = "build", .status = "failure", .url = "https://ci/build"}, {.name = "lint", .status = "pending"}});
	prw::Report report = prw::Evaluate(watch, failing, nullptr, kNow);
	UAM_ASSERT_EQ(report.changes.size(), std::size_t{1});
	UAM_ASSERT(report.changes[0].kind == prw::ChangeKind::ChecksFailed);
	UAM_ASSERT_EQ(report.changes[0].checks[0].name, std::string("build"));
	// The same failure is not news again.
	report = prw::Evaluate(report.next, failing, nullptr, kNow + 2 * kMinute);
	UAM_ASSERT(report.changes.empty());
	// A second check failing is news; the first stays told.
	const prw::Detail both = OpenDetail({{.name = "build", .status = "failure"}, {.name = "lint", .status = "cancelled"}});
	report = prw::Evaluate(report.next, both, nullptr, kNow + 4 * kMinute);
	UAM_ASSERT_EQ(report.changes.size(), std::size_t{1});
	UAM_ASSERT_EQ(report.changes[0].checks.size(), std::size_t{1});
	UAM_ASSERT_EQ(report.changes[0].checks[0].name, std::string("lint"));
	// A read that could not list every check forgets nothing: the next full read does not repeat the failures.
	prw::Detail partial = OpenDetail({{.name = "docs", .status = "success"}});
	partial.checks_unread = true;
	report = prw::Evaluate(report.next, partial, nullptr, kNow + 5 * kMinute);
	UAM_ASSERT(report.changes.empty());
	report = prw::Evaluate(report.next, both, nullptr, kNow + 5 * kMinute);
	UAM_ASSERT(report.changes.empty());
	// A push starts over: the same check failing on the new head is reported.
	report = prw::Evaluate(report.next, OpenDetail({{.name = "build", .status = "failure"}}, kNextSha), nullptr, kNow + 6 * kMinute);
	UAM_ASSERT(Has(report, prw::ChangeKind::ChecksFailed));
	UAM_ASSERT_EQ(report.next.head_sha, kNextSha);
}

UAM_TEST(PrWatchReportsRequiredChecksPassingThenTheRemainingOnes)
{
	const prw::Check build{.name = "build", .status = "success", .required = true};
	prw::Report report = prw::Evaluate({}, OpenDetail({{.name = "build", .status = "pending", .required = true}, {.name = "docs", .status = "pending"}}), nullptr, kNow);
	UAM_ASSERT(report.changes.empty());
	report = prw::Evaluate(report.next, OpenDetail({build, {.name = "docs", .status = "pending"}}), nullptr, kNow);
	UAM_ASSERT_EQ(report.changes.size(), std::size_t{1});
	UAM_ASSERT(report.changes[0].kind == prw::ChangeKind::ChecksPassed && report.changes[0].required && report.changes[0].count == 1);
	// Still passed: nothing to say while the advisory check runs.
	report = prw::Evaluate(report.next, OpenDetail({build, {.name = "docs", .status = "pending"}}), nullptr, kNow);
	UAM_ASSERT(report.changes.empty());
	// The check that was pending when the required ones passed is owed one message when everything settles.
	report = prw::Evaluate(report.next, OpenDetail({build, {.name = "docs", .status = "success"}}), nullptr, kNow);
	UAM_ASSERT_EQ(report.changes.size(), std::size_t{1});
	UAM_ASSERT(report.changes[0].kind == prw::ChangeKind::ChecksPassed && !report.changes[0].required && report.changes[0].count == 2);
	report = prw::Evaluate(report.next, OpenDetail({build, {.name = "docs", .status = "success"}, {.name = "late-advisory", .status = "success"}}), nullptr, kNow);
	UAM_ASSERT(report.changes.empty());
	// An unread check source cannot establish success.
	prw::Detail unread = OpenDetail({build});
	unread.checks_unread = true;
	UAM_ASSERT(prw::Evaluate({.head_sha = kSha}, unread, nullptr, kNow).changes.empty());
}

UAM_TEST(PrWatchReportsChecksWaitingForApprovalOncePerHead)
{
	const prw::Detail held = OpenDetail({{.name = "CI", .status = "action-required", .url = "https://github.com/acme/app/actions/runs/7"}});
	prw::Report report = prw::Evaluate({}, held, nullptr, kNow);
	UAM_ASSERT_EQ(report.changes.size(), std::size_t{1});
	UAM_ASSERT(report.changes[0].kind == prw::ChangeKind::ChecksAwaitingApproval);
	report = prw::Evaluate(report.next, held, nullptr, kNow);
	UAM_ASSERT(report.changes.empty());
	// The hold stays until that run's jobs finish, so a partial read cannot announce success.
	report = prw::Evaluate(report.next, OpenDetail({{.name = "other", .status = "success", .url = "https://elsewhere/1"}}), nullptr, kNow);
	UAM_ASSERT(!Has(report, prw::ChangeKind::ChecksPassed));
	report = prw::Evaluate(report.next, OpenDetail({{.name = "build", .status = "success", .url = "https://github.com/acme/app/actions/runs/7/job/1"}}), nullptr, kNow);
	UAM_ASSERT(Has(report, prw::ChangeKind::ChecksPassed));
}

UAM_TEST(PrWatchReportsNoChecksAfterFifteenMinutesOnce)
{
	prw::Report report = prw::Evaluate({}, OpenDetail(), nullptr, kNow);
	UAM_ASSERT(report.changes.empty());
	report = prw::Evaluate(report.next, OpenDetail(), nullptr, kNow + 14 * kMinute);
	UAM_ASSERT(report.changes.empty());
	report = prw::Evaluate(report.next, OpenDetail(), nullptr, kNow + 16 * kMinute);
	UAM_ASSERT_EQ(report.changes.size(), std::size_t{1});
	UAM_ASSERT(report.changes[0].kind == prw::ChangeKind::NoChecks && report.changes[0].minutes == 16);
	report = prw::Evaluate(report.next, OpenDetail(), nullptr, kNow + 40 * kMinute);
	UAM_ASSERT(report.changes.empty());
	// A check appearing stops the clock.
	report = prw::Evaluate(prw::Evaluate({}, OpenDetail(), nullptr, kNow).next, OpenDetail({{.name = "build", .status = "pending"}}), nullptr, kNow + 5 * kMinute);
	UAM_ASSERT_EQ(report.next.checks_awaited_since_ms, std::int64_t{0});
}

UAM_TEST(PrWatchReportsOtherPeoplesRemarksOnceAndNeverYourOwn)
{
	prw::Reported watch{.head_sha = kSha, .remarks_through_ms = kNow};
	const prw::Detail detail = OpenDetail({{.name = "build", .status = "pending"}});
	std::vector<prw::Remark> remarks = {RemarkAt("issue-1", "Me", kNow + 1000), RemarkAt("issue-2", "reviewer", kNow - 1000), RemarkAt("issue-3", "reviewer", kNow + 2000)};
	prw::Report report = prw::Evaluate(watch, detail, &remarks, kNow + 3000);
	UAM_ASSERT_EQ(report.changes.size(), std::size_t{1});
	UAM_ASSERT(report.changes[0].kind == prw::ChangeKind::Remarks);
	UAM_ASSERT_EQ(report.changes[0].remarks.size(), std::size_t{1});
	UAM_ASSERT_EQ(report.changes[0].remarks[0].id, std::string("issue-3"));
	UAM_ASSERT_EQ(report.next.wakes, 1);
	// Already told; a second remark stamped in the same second is told apart by its ID.
	remarks.push_back(RemarkAt("issue-4", "reviewer", kNow + 2000));
	report = prw::Evaluate(report.next, detail, &remarks, kNow + 4000);
	UAM_ASSERT_EQ(report.changes.size(), std::size_t{1});
	UAM_ASSERT_EQ(report.changes[0].remarks.size(), std::size_t{1});
	UAM_ASSERT_EQ(report.changes[0].remarks[0].id, std::string("issue-4"));
	report = prw::Evaluate(report.next, detail, &remarks, kNow + 5000);
	UAM_ASSERT(report.changes.empty());
	// Comments that were not read are neither reported nor forgotten.
	const prw::Report unread = prw::Evaluate(watch, detail, nullptr, kNow + 3000);
	UAM_ASSERT(unread.changes.empty());
	UAM_ASSERT_EQ(unread.next.remarks_through_ms, kNow);
}

UAM_TEST(PrWatchReportsAConflictOnceAndKeepsItWhileGitHubRecomputes)
{
	prw::Detail detail = OpenDetail({{.name = "build", .status = "pending"}});
	detail.mergeability = "conflicting";
	prw::Report report = prw::Evaluate({.head_sha = kSha}, detail, nullptr, kNow);
	UAM_ASSERT(Has(report, prw::ChangeKind::Conflicting));
	report = prw::Evaluate(report.next, detail, nullptr, kNow);
	UAM_ASSERT(report.changes.empty());
	detail.mergeability = "unknown";
	report = prw::Evaluate(report.next, detail, nullptr, kNow);
	UAM_ASSERT(report.next.conflicting);
	detail.mergeability = "mergeable";
	report = prw::Evaluate(report.next, detail, nullptr, kNow);
	UAM_ASSERT(!report.next.conflicting);
	detail.mergeability = "conflicting";
	UAM_ASSERT(Has(prw::Evaluate(report.next, detail, nullptr, kNow), prw::ChangeKind::Conflicting));
}

UAM_TEST(PrWatchEndsAfterTenCommentOnlyWakesAndCheckNewsResetsTheCount)
{
	prw::Reported watch{.head_sha = kSha, .remarks_through_ms = kNow};
	prw::Detail detail = OpenDetail({{.name = "build", .status = "pending"}});
	prw::Report report;
	for (int wake = 1; wake <= prw::kWakeLimit; ++wake)
	{
		const std::vector<prw::Remark> remarks = {RemarkAt("issue-" + std::to_string(wake), "bot", kNow + wake * 1000)};
		report = prw::Evaluate(watch, detail, &remarks, kNow + wake * 1000);
		UAM_ASSERT_EQ(report.next.wakes, wake);
		UAM_ASSERT_EQ(report.exhausted, wake == prw::kWakeLimit);
		watch = report.next;
	}
	// A failing check beside a comment is progress and starts the count again.
	detail.checks[0].status = "failure";
	const std::vector<prw::Remark> remarks = {RemarkAt("issue-99", "bot", kNow + 99000)};
	report = prw::Evaluate(watch, detail, &remarks, kNow + 99000);
	UAM_ASSERT_EQ(report.next.wakes, 0);
	UAM_ASSERT(!report.exhausted);
}

UAM_TEST(PrWatchMessageNamesEachChangeWithLinks)
{
	const prw::Detail detail = OpenDetail({{.name = "build", .status = "failure", .url = "https://ci/build"}});
	const std::vector<prw::Remark> remarks = {RemarkAt("review-1", "reviewer", kNow + 1000, "<!-- bot marker -->  Looks\n good   to me")};
	prw::Report report = prw::Evaluate({.head_sha = kSha, .remarks_through_ms = kNow}, detail, &remarks, kNow + 2000);
	prw::Message message = prw::BuildMessage(kPr, detail, report);
	UAM_ASSERT(message.text.starts_with("Update on pull request #12 (https://github.com/acme/app/pull/12), which UAM is watching for this chat:"));
	UAM_ASSERT(message.text.find("- Checks failed on aaaaaaa:\n  - build https://ci/build") != std::string::npos);
	UAM_ASSERT(message.text.find("- 1 new comment:\n  - reviewer: \"Looks good to me\" https://github.com/acme/app/pull/12#c") != std::string::npos);
	UAM_ASSERT(message.text.find("pr_unwatch") != std::string::npos);
	UAM_ASSERT_EQ(message.summary, std::string("#12: checks failed, new comments"));
	UAM_ASSERT_EQ(message.outcome, std::string("failed"));
	report = prw::Evaluate({.head_sha = kSha}, OpenDetail({{.name = "build", .status = "success"}}), nullptr, kNow);
	message = prw::BuildMessage(kPr, detail, report);
	UAM_ASSERT(message.text.find("- All 1 check passed on aaaaaaa.") != std::string::npos);
	UAM_ASSERT_EQ(message.outcome, std::string("completed"));
	// A long comment is cut on a character boundary.
	std::string body(190, 'x');
	for (int index = 0; index < 10; ++index) body += "\xE2\x9C\x93";
	const std::vector<prw::Remark> long_remarks = {RemarkAt("issue-1", "reviewer", kNow + 1000, body)};
	message = prw::BuildMessage(kPr, detail, prw::Evaluate({.head_sha = kSha, .failed_checks = {"build"}, .remarks_through_ms = kNow}, detail, &long_remarks, kNow + 2000));
	UAM_ASSERT(message.text.find(std::string(190, 'x') + "\xE2\x9C\x93\xE2\x9C\x93...\"") != std::string::npos);
}

UAM_TEST(PrWatchCadenceFollowsWatchQuietClosedAndMissingRules)
{
	const auto due = [](prw::CadenceInput input, std::int64_t since_last_read) {
		input.last_read_ms = kNow - since_last_read;
		return prw::IsDue(input, kNow);
	};
	const prw::CadenceInput watched{.watched = true, .linked = true, .has_snapshot = true, .state = "open", .updated_at_ms = kNow - 60 * kMinute};
	UAM_ASSERT(!due(watched, 2 * kMinute - 1));
	UAM_ASSERT(due(watched, 2 * kMinute));
	// A linked pull request nobody watches: every 2 minutes while it is being updated, every 5 once quiet for 30.
	prw::CadenceInput linked{.linked = true, .has_snapshot = true, .state = "open", .updated_at_ms = kNow - 10 * kMinute};
	UAM_ASSERT(due(linked, 2 * kMinute));
	linked.updated_at_ms = kNow - 30 * kMinute;
	UAM_ASSERT(!due(linked, 5 * kMinute - 1));
	UAM_ASSERT(due(linked, 5 * kMinute));
	// A closed one is read every 15 minutes in case it is reopened, and not at all once closed for a day.
	prw::CadenceInput closed{.linked = true, .has_snapshot = true, .state = "closed", .closed_at_ms = kNow - 60 * kMinute};
	UAM_ASSERT(!due(closed, 15 * kMinute - 1));
	UAM_ASSERT(due(closed, 15 * kMinute));
	closed.closed_at_ms = kNow - prw::kClosedFinalAfterMs;
	UAM_ASSERT(!due(closed, 24 * 60 * kMinute));
	UAM_ASSERT(!due({.linked = true, .has_snapshot = true, .state = "merged"}, 24 * 60 * kMinute));
	// Never read: at once. Not found: slowly. Only settled chats link it: never.
	UAM_ASSERT(prw::IsDue({.linked = true}, kNow));
	UAM_ASSERT(!due({.linked = true, .missing = true}, 15 * kMinute - 1));
	UAM_ASSERT(due({.linked = true, .missing = true}, 15 * kMinute));
	UAM_ASSERT(!prw::IsDue({.watched = true}, kNow));
}

UAM_TEST(PrWatchDecodesGhApiAnswers)
{
	prw::GhResponse response = prw::ParseGhApiOutput("HTTP/2.0 200 OK\r\nContent-Type: application/json\r\nEtag: W/\"abc\"\r\n\r\n{\"a\":1}", kNow);
	UAM_ASSERT_EQ(response.status, 200);
	UAM_ASSERT_EQ(response.etag, std::string("W/\"abc\""));
	UAM_ASSERT_EQ(response.body, std::string("{\"a\":1}"));
	response = prw::ParseGhApiOutput("HTTP/2.0 304 Not Modified\nEtag: \"abc\"\n\n", kNow);
	UAM_ASSERT_EQ(response.status, 304);
	UAM_ASSERT(response.body.empty() && !response.rate_limited);
	// A primary limit names its reset time; a secondary one says how long to wait.
	response = prw::ParseGhApiOutput("HTTP/2.0 403 Forbidden\r\nX-Ratelimit-Remaining: 0\r\nX-Ratelimit-Reset: " + std::to_string(kNow / 1000 + 600) + "\r\n\r\n{}", kNow);
	UAM_ASSERT(response.rate_limited);
	UAM_ASSERT_EQ(response.retry_at_ms, kNow + 600000);
	response = prw::ParseGhApiOutput("HTTP/2.0 429 Too Many Requests\r\nRetry-After: 90\r\n\r\n{}", kNow);
	UAM_ASSERT(response.rate_limited);
	UAM_ASSERT_EQ(response.retry_at_ms, kNow + 90000);
	response = prw::ParseGhApiOutput("HTTP/2.0 403 Forbidden\r\n\r\n{\"message\":\"You have exceeded a secondary rate limit.\"}", kNow);
	UAM_ASSERT(response.rate_limited);
	UAM_ASSERT_EQ(response.retry_at_ms, kNow + 60000);
	UAM_ASSERT(!prw::ParseGhApiOutput("HTTP/2.0 403 Forbidden\r\n\r\n{\"message\":\"Resource not accessible\"}", kNow).rate_limited);
	// A signed-out or missing gh gives no HTTP answer.
	UAM_ASSERT_EQ(prw::ParseGhApiOutput("To get started with GitHub CLI, please run:  gh auth login\n", kNow).status, 0);
	UAM_ASSERT_EQ(prw::ParseIsoMs("1970-01-02T00:00:01Z"), std::int64_t{86401000});
	UAM_ASSERT_EQ(prw::FormatIsoMs(kNow), std::string("2026-11-11T12:00:00Z"));
	UAM_ASSERT_EQ(prw::ParseIsoMs("yesterday"), std::int64_t{0});
}

UAM_TEST(PrWatchDecodesGitHubJson)
{
	prw::Detail detail;
	nlohmann::json pull = PullBody();
	pull["mergeable"] = nullptr;
	pull["head"]["repo"]["id"] = 2;
	UAM_ASSERT(prw::DecodePull(pull.dump(), detail));
	UAM_ASSERT(detail.state == "open" && detail.head_sha == kSha && detail.base_branch == "main" && detail.author == "author" && detail.fork);
	UAM_ASSERT_EQ(detail.mergeability, std::string("unknown"));
	pull["mergeable_state"] = "dirty";
	UAM_ASSERT(prw::DecodePull(pull.dump(), detail) && detail.mergeability == "conflicting");
	UAM_ASSERT(prw::DecodePull(PullBody(kSha, true).dump(), detail) && detail.state == "merged" && !detail.fork);
	UAM_ASSERT(!prw::DecodePull("<html>", detail) && !prw::DecodePull("{}", detail));

	std::vector<prw::Check> checks;
	UAM_ASSERT(prw::DecodeCheckRuns(CheckRuns({{"build", "failure"}, {"lint", ""}, {"deploy", "action_required"}, {"docs", "skipped"}, {"slow", "timed_out"}}).dump(), checks));
	UAM_ASSERT_EQ(checks.size(), std::size_t{5});
	UAM_ASSERT(checks[0].status == "failure" && checks[1].status == "pending" && checks[2].status == "action-required" && checks[3].status == "neutral" && checks[4].status == "failure");
	UAM_ASSERT(prw::DecodeCombinedStatus(R"({"statuses":[{"context":"legacy","state":"error","target_url":"https://ci/1"},{"context":"cla","state":"success"}]})", checks));
	UAM_ASSERT(checks[5].name == "legacy" && checks[5].status == "failure" && checks[6].status == "success");
	UAM_ASSERT(prw::DecodeHeldWorkflowRuns(R"({"workflow_runs":[{"name":"CI","html_url":"https://github.com/acme/app/actions/runs/7"}]})", checks));
	UAM_ASSERT(checks[7].status == "action-required");
	UAM_ASSERT(!prw::DecodeCheckRuns("[]", checks));

	std::vector<prw::Remark> remarks;
	UAM_ASSERT(prw::DecodeIssueComments(R"([{"id":5,"user":{"login":"reviewer"},"body":"Hi","html_url":"https://x/5","created_at":"2026-11-11T12:00:01Z","updated_at":"2026-11-11T12:00:05Z"}])", remarks));
	UAM_ASSERT(prw::DecodeReviewComments(R"([{"id":5,"user":{"login":"reviewer"},"body":"Rename","path":"src/a.cpp","created_at":"2026-11-11T12:00:02Z"}])", remarks));
	UAM_ASSERT(prw::DecodeReviews(R"([{"id":5,"user":{"login":"reviewer"},"body":"","state":"APPROVED","submitted_at":"2026-11-11T12:00:03Z"},{"id":6,"state":"COMMENTED","body":""},{"id":7,"state":"PENDING","body":"draft"}])", remarks));
	UAM_ASSERT_EQ(remarks.size(), std::size_t{3});
	// An edited comment counts from its edit; the three sources never share an ID.
	UAM_ASSERT(remarks[0].id == "issue-5" && remarks[0].at_ms == kNow + 5000);
	UAM_ASSERT(remarks[1].id == "review-comment-5" && remarks[1].path == "src/a.cpp");
	UAM_ASSERT(remarks[2].id == "review-5" && remarks[2].review_state == "approved");

	std::vector<std::string> required;
	UAM_ASSERT(prw::DecodeRequiredChecks(R"({"data":{"repository":{"pullRequest":{"commits":{"nodes":[{"commit":{"statusCheckRollup":{"contexts":{"nodes":[{"name":"build","isRequired":true},{"context":"cla","isRequired":true},{"name":"docs","isRequired":false}]}}}}]}}}}})", required));
	UAM_ASSERT(required == (std::vector<std::string>{"build", "cla"}));
	UAM_ASSERT(!prw::DecodeRequiredChecks(R"({"errors":[{"message":"nope"}]})", required));
}

UAM_TEST(PrWatchRevalidatesWithEtagsSoAnUnchangedPullRequestIsNotReadAgain)
{
	FakeGh gh;
	gh.Set(kRoot + "/pulls/12", PullBody(), "\"pull-v1\"");
	gh.Set(kRoot + "/commits/" + kSha + "/check-runs", CheckRuns({{"build", "failure"}}), "W/\"runs-v1\"");
	gh.Set(kRoot + "/commits/" + kSha + "/status", {{"statuses", nlohmann::json::array()}});
	// Parsed text, not nested initializer lists: MSVC runs out of heap on deep brace nesting.
	gh.Set("graphql", nlohmann::json::parse(R"({"data":{"repository":{"pullRequest":{"commits":{"nodes":[{"commit":{"statusCheckRollup":{"contexts":{"nodes":[{"name":"build","isRequired":true}]}}}}]}}}}})"));
	const uam::PrWatchService::GhRunner runner = [&](const std::vector<std::string>& arguments, std::stop_token) { return gh.Run(arguments); };
	uam::PrWatchService::ReadJob job{.key = prw::Key(kPr), .pr = kPr};

	const uam::PrWatchService::ReadResult first = uam::PrWatchService::Read(job, runner, kNow);
	UAM_ASSERT(first.ok);
	UAM_ASSERT_EQ(first.detail.checks.size(), std::size_t{1});
	UAM_ASSERT(first.detail.checks[0].required && first.detail.checks[0].status == "failure");
	UAM_ASSERT_EQ(gh.Conditional(), std::size_t{0});
	const std::size_t first_calls = gh.Calls();
	UAM_ASSERT_EQ(first_calls, std::size_t{4}); // pull, check runs, statuses, required checks
	// The token never appears: gh holds it, and every call is a plain `gh api`.
	for (const std::vector<std::string>& call : gh.calls) UAM_ASSERT_EQ(call[0], std::string("api"));

	// Second read: every REST request carries its ETag, GitHub answers 304, and the kept bodies give the same detail.
	job.validators = first.validators;
	const uam::PrWatchService::ReadResult second = uam::PrWatchService::Read(job, runner, kNow + 2 * kMinute);
	UAM_ASSERT(second.ok);
	UAM_ASSERT_EQ(gh.Calls() - first_calls, std::size_t{3}); // Unchanged checks do not ask which are required again.
	UAM_ASSERT_EQ(gh.Conditional(), std::size_t{3});
	UAM_ASSERT(std::ranges::find(gh.calls[first_calls], std::string("If-None-Match: \"pull-v1\"")) != gh.calls[first_calls].end());
	UAM_ASSERT(std::ranges::find(gh.calls[first_calls + 1], std::string("If-None-Match: W/\"runs-v1\"")) != gh.calls[first_calls + 1].end());
	UAM_ASSERT(second.detail.checks == first.detail.checks);
	UAM_ASSERT_EQ(second.detail.head_sha, kSha);

	// A changed check list is read in full again, once.
	gh.Set(kRoot + "/commits/" + kSha + "/check-runs", CheckRuns({{"build", "success"}}), "W/\"runs-v2\"");
	job.validators = second.validators;
	const uam::PrWatchService::ReadResult third = uam::PrWatchService::Read(job, runner, kNow + 4 * kMinute);
	UAM_ASSERT(third.ok && third.detail.checks[0].status == "success" && third.detail.checks[0].required);
	UAM_ASSERT_EQ(third.validators.at(kRoot + "/commits/" + kSha + "/check-runs?per_page=100").etag, std::string("W/\"runs-v2\""));
}

UAM_TEST(PrWatchReadDistinguishesNotFoundRateLimitsAndUnreadChecks)
{
	FakeGh gh;
	const uam::PrWatchService::GhRunner runner = [&](const std::vector<std::string>& arguments, std::stop_token) { return gh.Run(arguments); };
	const uam::PrWatchService::ReadJob job{.key = prw::Key(kPr), .pr = kPr, .want_remarks = true, .remarks_since_ms = kNow};
	uam::PrWatchService::ReadResult result = uam::PrWatchService::Read(job, runner, kNow);
	UAM_ASSERT(!result.ok && result.not_found && !result.rate_limited);

	gh.Fail(kRoot + "/pulls/12", 403, "X-Ratelimit-Remaining: 0\r\nX-Ratelimit-Reset: " + std::to_string(kNow / 1000 + 300) + "\r\n");
	result = uam::PrWatchService::Read(job, runner, kNow);
	UAM_ASSERT(!result.ok && result.rate_limited && !result.not_found);
	UAM_ASSERT_EQ(result.retry_at_ms, kNow + 300000);

	// Checks that cannot be read leave the pull request readable but unable to report success; comments ask only since the last one told.
	gh.Set(kRoot + "/pulls/12", PullBody());
	gh.Fail(kRoot + "/commits/" + kSha + "/check-runs", 500);
	gh.Set(kRoot + "/commits/" + kSha + "/status", nlohmann::json::parse(R"({"statuses":[{"context":"cla","state":"success"}]})"));
	gh.Set(kRoot + "/issues/12/comments", nlohmann::json::array());
	gh.Set(kRoot + "/pulls/12/comments", nlohmann::json::array());
	gh.Set(kRoot + "/pulls/12/reviews", nlohmann::json::array());
	gh.Set("user", {{"login", "me"}});
	result = uam::PrWatchService::Read(job, runner, kNow);
	UAM_ASSERT(result.ok && result.detail.checks_unread && result.remarks_read);
	UAM_ASSERT_EQ(result.viewer, std::string("me"));
	UAM_ASSERT_EQ(gh.Count(kRoot + "/issues/12/comments?per_page=100&since=2026-11-11T12:00:00Z"), std::size_t{1});
	UAM_ASSERT(prw::Evaluate({.head_sha = kSha}, result.detail, &result.remarks, kNow).changes.empty());

	// Without the signed-in account's name its own comments cannot be excluded, so no comments are reported from this read.
	gh.Fail("user", 500);
	result = uam::PrWatchService::Read(job, runner, kNow);
	UAM_ASSERT(result.ok && !result.remarks_read && result.viewer.empty());

	gh.signed_out = true;
	result = uam::PrWatchService::Read(job, runner, kNow);
	UAM_ASSERT(!result.ok && !result.not_found && !result.rate_limited && result.status == 0);
}

UAM_TEST(PrWatchWakesTheChatOnceAndNeverRepeatsNewsAcrossSweepsOrRestart)
{
	WatchFixture fixture;
	fixture.SetChecks(kSha, {{"build", "failure"}});
	fixture.Watch();
	fixture.Sweep();
	UAM_ASSERT_EQ(fixture.wakes->size(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.wakes->front().first, std::string("chat-1"));
	UAM_ASSERT(fixture.wakes->front().second.find("- Checks failed on aaaaaaa:\n  - build https://github.com/acme/app/actions/runs/7/job/build") != std::string::npos);
	const nlohmann::json rows = fixture.service->SerializeForFrontend();
	UAM_ASSERT_EQ(rows.size(), std::size_t{1});
	UAM_ASSERT(rows[0]["chatId"] == "chat-1" && rows[0]["repository"] == "acme/app" && rows[0]["number"] == 12 && rows[0]["watching"] == true);
	UAM_ASSERT(rows[0]["state"] == "open" && rows[0]["title"] == "Add the parser" && rows[0]["checksFailed"] == 1 && rows[0]["checksPassed"] == 0);
	UAM_ASSERT(rows[0]["notice"]["serial"] == 1 && rows[0]["notice"]["message"] == "acme/app #12: checks failed" && rows[0]["notice"]["tone"] == "warning");

	// One minute later nothing is due; two minutes after the read it is, and the unchanged pull request says nothing new.
	const std::size_t calls = fixture.gh->Calls();
	fixture.SweepAfter(kMinute);
	UAM_ASSERT_EQ(fixture.gh->Calls(), calls);
	fixture.SweepAfter(kMinute);
	UAM_ASSERT(fixture.gh->Calls() > calls);
	UAM_ASSERT(fixture.gh->Conditional() >= std::size_t{5});
	UAM_ASSERT_EQ(fixture.wakes->size(), std::size_t{1});

	// After a restart the watch is still there and the same failure is still told.
	fixture.Restart();
	UAM_ASSERT(fixture.service->IsWatching("chat-1", kPr));
	UAM_ASSERT(fixture.service->SerializeForFrontend()[0]["checksFailed"] == 1);
	UAM_ASSERT(!fixture.service->SerializeForFrontend()[0].contains("notice"));
	fixture.Sweep();
	fixture.SweepAfter(2 * kMinute);
	UAM_ASSERT_EQ(fixture.wakes->size(), std::size_t{1});

	// A fix arrives: one more message, saying the checks passed.
	fixture.gh->Set(kRoot + "/pulls/12", PullBody(kNextSha));
	fixture.SetChecks(kNextSha, {{"build", "success"}});
	fixture.SweepAfter(2 * kMinute);
	UAM_ASSERT_EQ(fixture.wakes->size(), std::size_t{2});
	UAM_ASSERT(fixture.wakes->back().second.find("- All 1 check passed on bbbbbbb.") != std::string::npos);
}

UAM_TEST(PrWatchKeepsAnUndeliveredWakeAcrossRestartAndDeliversItOnce)
{
	WatchFixture fixture;
	fixture.SetChecks(kSha, {{"build", "failure"}});
	fixture.Watch();
	*fixture.accept = false;
	fixture.Sweep();
	UAM_ASSERT(fixture.wakes->empty());
	// Refused wakes are retried a minute later, not on every tick.
	*fixture.accept = true;
	fixture.service->Poll(fixture.app, fixture.now + 1000);
	UAM_ASSERT(fixture.wakes->empty());

	fixture.Restart();
	fixture.service->Poll(fixture.app, fixture.now + 2000);
	fixture.service->WaitForWorkerForTests();
	fixture.service->Poll(fixture.app, fixture.now + 3000);
	fixture.SweepAfter(2 * kMinute);
	fixture.Restart();
	fixture.Sweep();
	UAM_ASSERT_EQ(fixture.wakes->size(), std::size_t{1});
	UAM_ASSERT(fixture.wakes->front().second.find("Checks failed") != std::string::npos);
}

UAM_TEST(PrWatchStopDoneAndUnwatchCancelAnUndeliveredFinalMessage)
{
	// The pull request merges while the chat is mid-turn and refuses the final message; the user then stops it.
	const auto merged_while_busy = [](WatchFixture& fixture) {
		fixture.Watch();
		fixture.Sweep();
		fixture.gh->Set(kRoot + "/pulls/12", PullBody(kSha, true));
		*fixture.accept = false;
		fixture.SweepAfter(2 * kMinute);
		UAM_ASSERT(!fixture.service->IsWatching("chat-1", kPr));
		UAM_ASSERT(fixture.wakes->empty());
		*fixture.accept = true;
	};
	const auto nothing_arrives = [](WatchFixture& fixture) {
		for (int minute = 0; minute < 5; ++minute) fixture.SweepAfter(kMinute);
		fixture.Restart();
		fixture.SweepAfter(kMinute);
		UAM_ASSERT(fixture.wakes->empty());
	};
	{
		WatchFixture fixture;
		merged_while_busy(fixture);
		UAM_ASSERT(fixture.service->EndWatchesForChat("chat-1"));
		nothing_arrives(fixture);
	}
	{
		WatchFixture fixture;
		merged_while_busy(fixture);
		fixture.app.chats[0].settled_at = "2026-11-12 09:00:00";
		nothing_arrives(fixture);
	}
	{
		WatchFixture fixture;
		merged_while_busy(fixture);
		UAM_ASSERT(!fixture.service->Unwatch("chat-1", kPr)); // It had already stopped watching.
		nothing_arrives(fixture);
	}
	// Done in the same tick that applies the merged read: the final message is never sent.
	{
		WatchFixture fixture;
		fixture.Watch();
		fixture.Sweep();
		fixture.gh->Set(kRoot + "/pulls/12", PullBody(kSha, true));
		fixture.now += 2 * kMinute;
		fixture.service->Poll(fixture.app, fixture.now);
		fixture.service->WaitForWorkerForTests();
		fixture.app.chats[0].settled_at = "2026-11-12 09:00:00";
		fixture.service->Poll(fixture.app, fixture.now + 5000);
		nothing_arrives(fixture);
	}
}

UAM_TEST(PrWatchRemovesLinksWhenTheLastChatIsDeleted)
{
	WatchFixture fixture;
	fixture.Watch();
	fixture.Sweep();
	UAM_ASSERT_EQ(fixture.service->SerializeForFrontend().size(), std::size_t{1});
	fixture.app.chats.clear();
	fixture.SweepAfter(kMinute);
	UAM_ASSERT(fixture.service->SerializeForFrontend().empty());
	fixture.Restart();
	UAM_ASSERT(fixture.service->SerializeForFrontend().empty());
}

UAM_TEST(PrWatchEndsWhenTheChatKeepsRefusingItsWake)
{
	WatchFixture fixture;
	fixture.SetChecks(kSha, {{"build", "failure"}});
	fixture.Watch();
	*fixture.accept = false;
	fixture.Sweep();
	for (int refusal = 2; refusal < 30; ++refusal)
	{
		fixture.now += 66 * 1000;
		fixture.service->Poll(fixture.app, fixture.now);
		fixture.service->WaitForWorkerForTests();
	}
	UAM_ASSERT(fixture.service->IsWatching("chat-1", kPr));
	fixture.now += 66 * 1000;
	fixture.service->Poll(fixture.app, fixture.now);
	fixture.service->WaitForWorkerForTests();
	UAM_ASSERT(!fixture.service->IsWatching("chat-1", kPr));
	fixture.now += 66 * 1000;
	fixture.service->Poll(fixture.app, fixture.now);
	nlohmann::json row = fixture.service->SerializeForFrontend()[0];
	UAM_ASSERT(row["notice"]["tone"] == "warning");
	UAM_ASSERT(row["notice"]["message"].get<std::string>().find("did not accept") != std::string::npos);
	// The undelivered text is dropped with the watch, so a later watch does not send stale news.
	*fixture.accept = true;
	fixture.now += 66 * 1000;
	fixture.service->Poll(fixture.app, fixture.now);
	UAM_ASSERT(fixture.wakes->empty());
}

UAM_TEST(PrWatchSharesOneReadBetweenChatsWatchingTheSamePullRequest)
{
	WatchFixture fixture;
	fixture.AddChat("chat-2");
	fixture.SetChecks(kSha, {{"build", "failure"}});
	fixture.Watch("chat-1");
	fixture.Watch("chat-2");
	fixture.Sweep();
	UAM_ASSERT_EQ(fixture.gh->Count(kRoot + "/pulls/12"), std::size_t{3}); // The pull request, its review comments and its reviews: once each.
	UAM_ASSERT_EQ(fixture.gh->Count(kRoot + "/commits/"), std::size_t{2});
	UAM_ASSERT_EQ(fixture.wakes->size(), std::size_t{2});
	UAM_ASSERT(fixture.wakes->at(0).first != fixture.wakes->at(1).first);
	UAM_ASSERT_EQ(fixture.wakes->at(0).second, fixture.wakes->at(1).second);
	// Notice serials are never shared, so the interface reports each one once.
	const nlohmann::json rows = fixture.service->SerializeForFrontend();
	UAM_ASSERT(rows[0]["notice"]["serial"] == 1 && rows[1]["notice"]["serial"] == 2);
}

UAM_TEST(PrWatchEndsOnMergeStopSettleAndDeletedChat)
{
	WatchFixture fixture;
	fixture.Watch();
	fixture.Sweep();
	fixture.gh->Set(kRoot + "/pulls/12", PullBody(kSha, true));
	fixture.SweepAfter(2 * kMinute);
	UAM_ASSERT(!fixture.service->IsWatching("chat-1", kPr));
	// The agent is told once that it was merged, then the watch is over.
	UAM_ASSERT_EQ(fixture.wakes->size(), std::size_t{1});
	UAM_ASSERT(fixture.wakes->front().second.find("- The pull request was merged.") != std::string::npos);
	UAM_ASSERT(fixture.wakes->front().second.find("https://github.com/acme/app/pull/12") != std::string::npos);
	nlohmann::json row = fixture.service->SerializeForFrontend()[0];
	UAM_ASSERT(row["state"] == "merged" && row["watching"] == false && row["notice"]["tone"] == "success");
	UAM_ASSERT(row["notice"]["message"] == "acme/app #12: merged, stopped watching");
	// A merged pull request is not read again.
	const std::size_t calls = fixture.gh->Calls();
	fixture.SweepAfter(60 * kMinute);
	UAM_ASSERT_EQ(fixture.gh->Calls(), calls);

	// Watching it again reads it afresh, so a reopened pull request is seen.
	fixture.gh->Set(kRoot + "/pulls/12", PullBody());
	fixture.Watch();
	fixture.Sweep();
	UAM_ASSERT(fixture.service->IsWatching("chat-1", kPr));
	UAM_ASSERT(fixture.service->SerializeForFrontend()[0]["state"] == "open");
	// Stop on the chat ends its watches and drops news it had not been given yet.
	UAM_ASSERT(fixture.service->EndWatchesForChat("chat-1"));
	UAM_ASSERT(!fixture.service->IsWatching("chat-1", kPr));
	UAM_ASSERT(!fixture.service->EndWatchesForChat("chat-1"));

	// Marking the chat done ends them too. A chat marked done earlier can still start a watch, and keeps it across a restart.
	fixture.Watch();
	fixture.app.chats[0].settled_at = "2026-11-11 12:00:00";
	fixture.Sweep();
	UAM_ASSERT(!fixture.service->IsWatching("chat-1", kPr));
	fixture.Watch();
	fixture.Restart();
	fixture.Sweep();
	UAM_ASSERT(fixture.service->IsWatching("chat-1", kPr));
	fixture.app.chats[0].settled_at = "2026-11-11 12:30:00";
	fixture.SweepAfter(kMinute);
	UAM_ASSERT(!fixture.service->IsWatching("chat-1", kPr));
	std::string error;
	UAM_ASSERT_EQ(fixture.service->SerializeForFrontend().size(), std::size_t{1});
	// A closed pull request says so in its one final message.
	fixture.wakes->clear();
	nlohmann::json closed = PullBody();
	closed["state"] = "closed";
	closed["closed_at"] = "2026-11-11T11:59:00Z";
	fixture.gh->Set(kRoot + "/pulls/12", closed);
	fixture.Watch();
	fixture.Sweep();
	UAM_ASSERT_EQ(fixture.wakes->size(), std::size_t{1});
	UAM_ASSERT(fixture.wakes->front().second.find("closed without being merged") != std::string::npos);
	UAM_ASSERT(!fixture.service->IsWatching("chat-1", kPr));
	fixture.SweepAfter(20 * kMinute);
	UAM_ASSERT_EQ(fixture.wakes->size(), std::size_t{1});
	fixture.gh->Set(kRoot + "/pulls/12", PullBody());

	// A subagent's chat and a temporary side chat cannot watch, whichever control asks.
	fixture.app.chats[0].settled_at.clear();
	fixture.app.chats[0].temporary_parent_chat_id = "parent";
	UAM_ASSERT(!fixture.service->Watch(fixture.app, "chat-1", kPr, fixture.now, &error));
	UAM_ASSERT(error.find("Subagents") != std::string::npos);
	fixture.app.chats[0].temporary_parent_chat_id.clear();
	fixture.app.chats[0].agent_run_id = "run-1";
	UAM_ASSERT(!fixture.service->Watch(fixture.app, "chat-1", kPr, fixture.now, &error));
	fixture.app.chats[0].agent_run_id.clear();
	// Unwatch keeps the link; unlink and deleting the chat remove it.
	fixture.Watch();
	UAM_ASSERT(fixture.service->Unwatch("chat-1", kPr));
	UAM_ASSERT_EQ(fixture.service->SerializeForFrontend().size(), std::size_t{1});
	UAM_ASSERT(fixture.service->Unlink("chat-1", kPr));
	UAM_ASSERT(fixture.service->SerializeForFrontend().empty());
	fixture.Watch();
	fixture.AddChat("chat-2");
	std::erase_if(fixture.app.chats, [](const ChatSession& chat) { return chat.id == "chat-1"; });
	fixture.service->Poll(fixture.app, fixture.now);
	UAM_ASSERT(fixture.service->SerializeForFrontend().empty());
	fixture.Restart();
	UAM_ASSERT(fixture.service->SerializeForFrontend().empty());
}

UAM_TEST(PrWatchEndsAfterEightFailedReadsButARateLimitOnlyPauses)
{
	WatchFixture fixture;
	fixture.Watch();
	fixture.Sweep();

	// Rate limited: no failure is counted and GitHub is left alone until the retry time.
	fixture.gh->Fail(kRoot + "/pulls/12", 403, "X-Ratelimit-Remaining: 0\r\nX-Ratelimit-Reset: " + std::to_string((fixture.now + 12 * kMinute) / 1000) + "\r\n");
	fixture.SweepAfter(2 * kMinute);
	UAM_ASSERT(fixture.service->IsWatching("chat-1", kPr));
	UAM_ASSERT(fixture.service->SerializeForFrontend()[0]["paused"] == true);
	const std::size_t paused_calls = fixture.gh->Calls();
	for (int minute = 0; minute < 9; ++minute) fixture.SweepAfter(kMinute);
	UAM_ASSERT_EQ(fixture.gh->Calls(), paused_calls);
	fixture.gh->Set(kRoot + "/pulls/12", PullBody());
	fixture.SweepAfter(kMinute);
	UAM_ASSERT(fixture.gh->Calls() > paused_calls);
	UAM_ASSERT(fixture.service->SerializeForFrontend()[0]["paused"] == false);
	UAM_ASSERT(fixture.service->SerializeForFrontend()[0]["readFailing"] == false);

	// gh signed out: seven failed reads keep watching, the eighth ends it with a notice, and reading stops.
	fixture.gh->signed_out = true;
	for (int read = 1; read < prw::kFailedReadLimit; ++read)
	{
		fixture.SweepAfter(2 * kMinute);
		UAM_ASSERT(fixture.service->IsWatching("chat-1", kPr));
	}
	UAM_ASSERT(fixture.service->SerializeForFrontend()[0]["readFailing"] == true);
	fixture.SweepAfter(2 * kMinute);
	UAM_ASSERT(!fixture.service->IsWatching("chat-1", kPr));
	const nlohmann::json row = fixture.service->SerializeForFrontend()[0];
	UAM_ASSERT(row["notice"]["tone"] == "warning");
	UAM_ASSERT(row["notice"]["message"].get<std::string>().find("8 times in a row") != std::string::npos);
	UAM_ASSERT(fixture.wakes->empty());

	// One good read in between starts the count again.
	fixture.gh->signed_out = false;
	fixture.Watch();
	fixture.Sweep();
	fixture.gh->signed_out = true;
	for (int read = 1; read < prw::kFailedReadLimit; ++read) fixture.SweepAfter(2 * kMinute);
	fixture.gh->signed_out = false;
	fixture.SweepAfter(2 * kMinute);
	fixture.gh->signed_out = true;
	fixture.SweepAfter(2 * kMinute);
	UAM_ASSERT(fixture.service->IsWatching("chat-1", kPr));
}

UAM_TEST(PrWatchReportsCommentsFromOthersThroughTheService)
{
	WatchFixture fixture;
	fixture.SetChecks(kSha, {{"build", ""}});
	fixture.Watch();
	fixture.Sweep();
	UAM_ASSERT(fixture.wakes->empty());
	const std::string later = prw::FormatIsoMs(fixture.now + kMinute);
	nlohmann::json comments = nlohmann::json::parse(R"([{"id":1,"user":{"login":"me"},"body":"My own note"},
	    {"id":2,"user":{"login":"reviewer"},"body":"Please add a test","html_url":"https://github.com/acme/app/pull/12#issuecomment-2"}])");
	for (nlohmann::json& comment : comments) comment["created_at"] = later;
	fixture.gh->Set(kRoot + "/issues/12/comments", comments);
	fixture.SweepAfter(2 * kMinute);
	UAM_ASSERT_EQ(fixture.wakes->size(), std::size_t{1});
	UAM_ASSERT(fixture.wakes->front().second.find("- 1 new comment:\n  - reviewer: \"Please add a test\" https://github.com/acme/app/pull/12#issuecomment-2") != std::string::npos);
	UAM_ASSERT(fixture.wakes->front().second.find("My own note") == std::string::npos);
	fixture.SweepAfter(2 * kMinute);
	UAM_ASSERT_EQ(fixture.wakes->size(), std::size_t{1});
	// Watching again after a stop does not bring back comments made in between.
	fixture.service->Unwatch("chat-1", kPr);
	fixture.now += 10 * kMinute;
	fixture.Watch();
	fixture.Sweep();
	UAM_ASSERT_EQ(fixture.wakes->size(), std::size_t{1});
}

namespace
{
	/// A chat with a live structured session and one UAM control capability, as the chat_create tests set up.
	struct ToolFixture : WatchFixture
	{
		ToolFixture()
		{
			app.data_root = temp.root / "data";
			fs::create_directories(app.data_root);
			app.chats[0].uam_control_enabled = true;
			app.chats[0].provider_id = "codex-cli";
			app.chats[0].created_at = "2026-11-01 10:00:00";
			app.pr_watch_service = std::make_shared<uam::PrWatchService>(app.data_root,
			    [](const std::vector<std::string>&, std::stop_token) { return std::string{}; },
			    [](uam::AppState&, const std::string&, const std::string&, std::string*) { return true; });
			Issue("capability", {});
			Issue("child-capability", "run-1");
		}
		void Issue(const std::string& id, const std::string& run_id)
		{
			uam::UamControlCapability capability;
			capability.id = id;
			capability.directory = temp.root / id;
			fs::create_directories(capability.directory / "responses");
			fs::create_directories(capability.directory / "requests");
			capability.chat_id = "chat-1";
			capability.session_chat_id = "chat-1";
			capability.provider_id = "codex-cli";
			capability.agent_id = "build";
			capability.agent_run_id = run_id;
			capability.expires_at_epoch_ms = uam::time::SystemEpochMillisecondsNow() + 3600000;
			app.uam_control_capabilities.push_back(capability);
			auto session = std::make_unique<uam::AcpSessionState>();
			session->chat_id = "chat-1";
			session->provider_id = "codex-cli";
			session->running = true;
			session->uam_control_capability_id = id;
			app.acp_sessions.push_back(std::move(session));
		}
		nlohmann::json Call(const std::string& request_id, const std::string& method, nlohmann::json arguments, const std::string& capability = "capability")
		{
			return uam::UamControlService::HandleRequestForTests(app, capability, {{"requestId", request_id}, {"method", method}, {"arguments", std::move(arguments)}},
			    uam::time::SystemEpochMillisecondsNow());
		}
	};
} // namespace

UAM_TEST(UamControlPrWatchToolsActOnTheCallingChatOnly)
{
	ToolFixture fixture;
	nlohmann::json response = fixture.Call("watch-1", "pr_watch", {{"url", "https://github.com/acme/app/pull/12"}});
	UAM_ASSERT(response.value("ok", false));
	UAM_ASSERT(response["result"]["watching"] == true && response["result"]["pullRequest"] == "https://github.com/acme/app/pull/12");
	UAM_ASSERT(fixture.app.pr_watch_service->IsWatching("chat-1", kPr));
	response = fixture.Call("watch-2", "pr_watch", {{"repository", "acme/app"}, {"number", 13}});
	UAM_ASSERT(response.value("ok", false));
	UAM_ASSERT(fixture.app.pr_watch_service->IsWatching("chat-1", {.repository = "acme/app", .number = 13}));

	response = fixture.Call("unwatch-1", "pr_unwatch", {{"repository", "acme/app"}, {"number", 12}});
	UAM_ASSERT(response.value("ok", false) && response["result"]["wasWatching"] == true);
	UAM_ASSERT(!fixture.app.pr_watch_service->IsWatching("chat-1", kPr));
	UAM_ASSERT_EQ(fixture.app.pr_watch_service->SerializeForFrontend().size(), std::size_t{2});

	// A subagent cannot watch; the chat that delegated to it can.
	response = fixture.Call("watch-3", "pr_watch", {{"url", "https://github.com/acme/app/pull/14"}}, "child-capability");
	UAM_ASSERT(!response.value("ok", false));
	UAM_ASSERT(response.value("error", "").find("Subagents") != std::string::npos);
	// Malformed and non-GitHub references are refused without a watch.
	int bad_index = 0;
	for (const char* bad : {R"({"url":"https://example.com/acme/app/pull/12"})", R"({"repository":"acme/app","number":0})", R"({"repository":"acme/app"})",
	         R"({"url":"acme/app#1","number":1,"repository":"acme/app"})", "{}"})
		UAM_ASSERT(!fixture.Call("bad-" + std::to_string(++bad_index), "pr_watch", nlohmann::json::parse(bad)).value("ok", false));
	UAM_ASSERT_EQ(fixture.app.pr_watch_service->SerializeForFrontend().size(), std::size_t{2});

	const nlohmann::json tools = uam::UamControlService::ToolDefinitionsForTests();
	for (const char* name : {"pr_watch", "pr_unwatch"})
	{
		const auto tool = std::ranges::find_if(tools, [&](const nlohmann::json& candidate) { return candidate.value("name", "") == name; });
		UAM_ASSERT(tool != tools.end());
		UAM_ASSERT(!tool->contains("annotations"));
		UAM_ASSERT((*tool)["inputSchema"]["properties"].contains("url") && (*tool)["inputSchema"]["properties"].contains("number"));
	}
}
