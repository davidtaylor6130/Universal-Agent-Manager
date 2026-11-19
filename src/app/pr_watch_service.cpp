#include "app/pr_watch_service.h"

#include "app/chat_domain_service.h"
#include "common/platform/background_thread_priority.h"
#include "common/platform/platform_services.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/state/app_state.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/io_utils.h"
#include "common/utils/shell_escape.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <algorithm>
#include <chrono>
#include <set>

namespace uam
{
	namespace
	{
		constexpr int kGhTimeoutMs = 30000;
		constexpr std::size_t kMaxLinksPerChat = 20;
		constexpr std::size_t kMaxLinks = 200;
		constexpr std::int64_t kWakeRetryMs = 60 * 1000;
		/// Refused deliveries, a minute apart, after which a watch ends instead of retrying for ever.
		constexpr int kWakeRefusalLimit = 30;
		constexpr std::size_t kMaxPendingWakeBytes = 16 * 1024;
		constexpr std::string_view kRequiredQuery =
		    "query($owner:String!,$name:String!,$number:Int!){repository(owner:$owner,name:$name){pullRequest(number:$number){commits(last:1){nodes{commit{statusCheckRollup{contexts(first:100){nodes{__typename ... on CheckRun{name isRequired(pullRequestNumber:$number)} ... on StatusContext{context isRequired(pullRequestNumber:$number)}}}}}}}}}}";

		std::string RunGh(const std::vector<std::string>& arguments, std::stop_token stop)
		{
			std::vector<std::string> argv = {"gh"};
			argv.insert(argv.end(), arguments.begin(), arguments.end());
			// `gh` holds the token; UAM never reads, stores or logs it.
			return PlatformServicesFactory::Instance().process_service.ExecuteCommand(uam::shell::JoinEscapedArgs(argv), kGhTimeoutMs, stop).output;
		}

		bool SendWake(AppState& app, const std::string& chat_id, const std::string& text, std::string* error)
		{
			return uam::SendAcpPrompt(app, chat_id, text, error);
		}

		nlohmann::json ToJson(const pr_watch::Reported& reported)
		{
			return {{"headSha", reported.head_sha}, {"failedChecks", reported.failed_checks}, {"passed", reported.passed},
			        {"passedChecks", reported.passed_checks}, {"owedChecks", reported.owed_checks}, {"awaitingApproval", reported.awaiting_approval},
			        {"approvalAnnounced", reported.approval_announced}, {"remarksThroughMs", reported.remarks_through_ms},
			        {"remarkIds", reported.remark_ids}, {"conflicting", reported.conflicting}, {"wakes", reported.wakes},
			        {"checksAwaitedSinceMs", reported.checks_awaited_since_ms}};
		}

		std::vector<std::string> Strings(const nlohmann::json& object, const char* key)
		{
			std::vector<std::string> values;
			if (const auto found = object.find(key); found != object.end() && found->is_array())
				for (const nlohmann::json& value : *found)
					if (value.is_string()) values.push_back(value.get<std::string>());
			return values;
		}

		template <typename Value> Value Field(const nlohmann::json& object, const char* key, Value fallback)
		{
			const auto found = object.find(key);
			if (found == object.end()) return fallback;
			try { return found->get<Value>(); }
			catch (const nlohmann::json::exception&) { return fallback; }
		}

		pr_watch::Reported ReportedFromJson(const nlohmann::json& object)
		{
			if (!object.is_object()) return {};
			return {.head_sha = Field<std::string>(object, "headSha", {}), .failed_checks = Strings(object, "failedChecks"),
			    .passed = Field(object, "passed", false), .passed_checks = Strings(object, "passedChecks"),
			    .owed_checks = Strings(object, "owedChecks"), .awaiting_approval = Strings(object, "awaitingApproval"),
			    .approval_announced = Field(object, "approvalAnnounced", false), .remarks_through_ms = Field<std::int64_t>(object, "remarksThroughMs", 0),
			    .remark_ids = Strings(object, "remarkIds"), .conflicting = Field(object, "conflicting", false), .wakes = Field(object, "wakes", 0),
			    .checks_awaited_since_ms = Field<std::int64_t>(object, "checksAwaitedSinceMs", 0)};
		}

	} // namespace

	PrWatchService::PrWatchService(std::filesystem::path data_root, GhRunner runner, WakeSender sender)
	    : m_file(std::move(data_root) / "pr-watches.json"), m_runner(runner ? std::move(runner) : GhRunner(RunGh)),
	      m_sender(sender ? std::move(sender) : WakeSender(SendWake))
	{
		Load();
	}

	PrWatchService::~PrWatchService()
	{
		Stop();
	}

	void PrWatchService::Stop()
	{
		m_stop.request_stop();
		// The detached reader owns copies of its inputs; an in-flight `gh` call ends on the stop request.
		if (m_worker.valid()) (void)m_worker.wait_for(std::chrono::seconds(2));
		m_worker = {};
	}

	void PrWatchService::Load()
	{
		std::string text;
		if (!uam::io::TryReadTextFile(m_file, text)) return;
		const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
		if (!root.is_object())
		{
			uam::diagnostics::Write("PR watch: the saved watch list could not be read; starting without watches.");
			return;
		}
		m_saved = text;
		if (const auto pulls = root.find("pullRequests"); pulls != root.end() && pulls->is_array())
			for (const nlohmann::json& item : *pulls)
			{
				if (!item.is_object()) continue;
				const std::optional<pr_watch::Reference> pr = pr_watch::ParseReference(Field<std::string>(item, "repository", {}), Field(item, "number", 0));
				if (!pr.has_value()) continue;
				Snapshot& snapshot = m_snapshots[pr_watch::Key(*pr)];
				snapshot.pr = *pr;
				snapshot.has_detail = Field(item, "hasDetail", false);
				snapshot.missing = Field(item, "missing", false);
				snapshot.detail = {.state = Field<std::string>(item, "state", "open"), .title = Field<std::string>(item, "title", {}),
				    .head_sha = Field<std::string>(item, "headSha", {}), .base_branch = Field<std::string>(item, "baseBranch", {}),
				    .mergeability = Field<std::string>(item, "mergeability", "unknown"), .updated_at_ms = Field<std::int64_t>(item, "updatedAtMs", 0),
				    .closed_at_ms = Field<std::int64_t>(item, "closedAtMs", 0)};
				if (const auto checks = item.find("checks"); checks != item.end() && checks->is_array())
					for (const nlohmann::json& check : *checks)
						snapshot.detail.checks.push_back({.name = Field<std::string>(check, "name", {}), .status = Field<std::string>(check, "status", {}),
						    .url = Field<std::string>(check, "url", {}), .required = Field(check, "required", false)});
			}
		if (const auto links = root.find("links"); links != root.end() && links->is_array())
			for (const nlohmann::json& item : *links)
			{
				if (!item.is_object()) continue;
				const std::optional<pr_watch::Reference> pr = pr_watch::ParseReference(Field<std::string>(item, "repository", {}), Field(item, "number", 0));
				const std::string chat_id = Field<std::string>(item, "chatId", {});
				if (!pr.has_value() || chat_id.empty() || m_links.size() >= kMaxLinks || FindLink(chat_id, *pr) != nullptr) continue;
				m_links.push_back({.chat_id = chat_id, .pr = *pr, .watching = Field(item, "watching", false),
				    .reported = ReportedFromJson(item.value("reported", nlohmann::json::object())), .pending_wake = Field<std::string>(item, "pendingWake", {}),
				    .settled_seen = Field<std::string>(item, "settledSeen", {})});
				m_snapshots[pr_watch::Key(*pr)].pr = *pr;
			}
	}

	bool PrWatchService::Save()
	{
		nlohmann::json links = nlohmann::json::array();
		for (const Link& link : m_links)
			links.push_back({{"chatId", link.chat_id}, {"repository", link.pr.repository}, {"number", link.pr.number}, {"watching", link.watching},
			    {"reported", ToJson(link.reported)}, {"pendingWake", link.pending_wake}, {"settledSeen", link.settled_seen}});
		nlohmann::json pulls = nlohmann::json::array();
		for (const auto& [key, snapshot] : m_snapshots)
		{
			nlohmann::json checks = nlohmann::json::array();
			for (const pr_watch::Check& check : snapshot.detail.checks)
				checks.push_back({{"name", check.name}, {"status", check.status}, {"url", check.url}, {"required", check.required}});
			pulls.push_back({{"repository", snapshot.pr.repository}, {"number", snapshot.pr.number}, {"hasDetail", snapshot.has_detail},
			    {"missing", snapshot.missing}, {"state", snapshot.detail.state}, {"title", snapshot.detail.title}, {"headSha", snapshot.detail.head_sha},
			    {"baseBranch", snapshot.detail.base_branch}, {"mergeability", snapshot.detail.mergeability},
			    {"updatedAtMs", snapshot.detail.updated_at_ms}, {"closedAtMs", snapshot.detail.closed_at_ms}, {"checks", std::move(checks)}});
		}
		m_dirty = false;
		const std::string text = nlohmann::json{{"version", 1}, {"links", std::move(links)}, {"pullRequests", std::move(pulls)}}.dump(1, '\t', false, nlohmann::json::error_handler_t::replace);
		if (text == m_saved) return true;
		if (!uam::io::AtomicWriteFile(m_file, text))
		{
			if (!m_save_failed) uam::diagnostics::Write("PR watch: the watch list could not be saved; it is retried on the next change.");
			m_save_failed = true;
			return false;
		}
		m_save_failed = false;
		m_saved = text;
		return true;
	}

	PrWatchService::Link* PrWatchService::FindLink(std::string_view chat_id, const pr_watch::Reference& pr)
	{
		const std::string key = pr_watch::Key(pr);
		const auto found = std::ranges::find_if(m_links, [&](const Link& link) { return link.chat_id == chat_id && pr_watch::Key(link.pr) == key; });
		return found == m_links.end() ? nullptr : &*found;
	}

	void PrWatchService::Notify(Link& link, std::string message, std::string tone)
	{
		link.notice_serial = ++m_notice_serial;
		link.notice_message = std::move(message);
		link.notice_tone = std::move(tone);
	}

	bool PrWatchService::Watch(const AppState& app, std::string_view chat_id, const pr_watch::Reference& pr, std::int64_t now_ms, std::string* error_out)
	{
		const auto fail = [&](std::string message) {
			if (error_out != nullptr) *error_out = std::move(message);
			return false;
		};
		const ChatSession* chat = ChatDomainService().FindChatById(app, std::string(chat_id));
		if (chat == nullptr) return fail("The chat is unavailable.");
		// Their turn ends with their task, so nothing would be there to wake.
		if (!chat->agent_run_id.empty() || !chat->subagent_parent_chat_id.empty() || !chat->temporary_parent_chat_id.empty())
			return fail("Subagents and temporary side chats cannot watch pull requests. Ask the chat that delegated to you to watch it.");
		Link* link = FindLink(chat_id, pr);
		if (link == nullptr)
		{
			if (m_links.size() >= kMaxLinks || static_cast<std::size_t>(std::ranges::count_if(m_links, [&](const Link& other) { return other.chat_id == chat_id; })) >= kMaxLinksPerChat)
				return fail("UAM links at most " + std::to_string(kMaxLinksPerChat) + " pull requests to a chat and " + std::to_string(kMaxLinks) + " in total. Unlink one first.");
			m_links.push_back({.chat_id = std::string(chat_id), .pr = pr});
			link = &m_links.back();
		}
		if (!link->watching)
		{
			// Comments made while nobody watched are not news; check and conflict news already told stays told.
			link->reported.remarks_through_ms = now_ms;
			link->reported.remark_ids.clear();
			link->reported.wakes = 0;
		}
		link->watching = true;
		link->settled_seen = chat->settled_at;
		Snapshot& snapshot = m_snapshots[pr_watch::Key(pr)];
		snapshot.pr = link->pr;
		// A fresh read decides, even when the saved state says merged or closed: it may have been reopened.
		snapshot.has_detail = false;
		snapshot.last_read_ms = 0;
		snapshot.failed_reads = 0;
		link->wake_refusals = 0;
		m_next_sweep_ms = 0;
		m_next_housekeeping_ms = 0;
		m_dirty = true;
		Save();
		return true;
	}

	bool PrWatchService::Unwatch(std::string_view chat_id, const pr_watch::Reference& pr)
	{
		Link* link = FindLink(chat_id, pr);
		// A finished watch can still hold its final message; stopping drops that too.
		if (link == nullptr || (!link->watching && link->pending_wake.empty())) return false;
		const bool was_watching = link->watching;
		link->watching = false;
		link->pending_wake.clear();
		m_dirty = true;
		Save();
		return was_watching;
	}

	bool PrWatchService::Unlink(std::string_view chat_id, const pr_watch::Reference& pr)
	{
		const std::string key = pr_watch::Key(pr);
		if (std::erase_if(m_links, [&](const Link& link) { return link.chat_id == chat_id && pr_watch::Key(link.pr) == key; }) == 0) return false;
		if (std::ranges::none_of(m_links, [&](const Link& link) { return pr_watch::Key(link.pr) == key; })) m_snapshots.erase(key);
		m_dirty = true;
		Save();
		return true;
	}

	bool PrWatchService::EndWatchesForChat(std::string_view chat_id)
	{
		bool changed = false;
		for (Link& link : m_links)
		{
			// Includes a finished watch whose final message is not delivered yet, so Stop and Done cancel it.
			if (link.chat_id != chat_id || (!link.watching && link.pending_wake.empty())) continue;
			link.watching = false;
			link.pending_wake.clear();
			changed = true;
		}
		if (changed)
		{
			m_dirty = true;
			Save();
		}
		return changed;
	}

	bool PrWatchService::IsWatching(std::string_view chat_id, const pr_watch::Reference& pr) const
	{
		const std::string key = pr_watch::Key(pr);
		return std::ranges::any_of(m_links, [&](const Link& link) { return link.watching && link.chat_id == chat_id && pr_watch::Key(link.pr) == key; });
	}

	nlohmann::json PrWatchService::SerializeForFrontend() const
	{
		nlohmann::json rows = nlohmann::json::array();
		for (const Link& link : m_links)
		{
			const auto found = m_snapshots.find(pr_watch::Key(link.pr));
			const Snapshot* snapshot = found == m_snapshots.end() ? nullptr : &found->second;
			int passed = 0, failed = 0, pending = 0;
			if (snapshot != nullptr)
				for (const pr_watch::Check& check : snapshot->detail.checks)
				{
					if (check.status == "failure" || check.status == "cancelled") ++failed;
					else if (check.status == "pending" || check.status == "action-required") ++pending;
					else ++passed;
				}
			const bool known = snapshot != nullptr && snapshot->has_detail;
			nlohmann::json row = {{"chatId", link.chat_id}, {"repository", link.pr.repository}, {"number", link.pr.number},
			    {"url", pr_watch::Url(link.pr)}, {"watching", link.watching},
			    {"state", snapshot != nullptr && snapshot->missing ? "missing" : known ? snapshot->detail.state : "unknown"},
			    {"title", known ? snapshot->detail.title : std::string{}}, {"checksPassed", passed}, {"checksFailed", failed},
			    {"checksPending", pending}, {"conflicting", known && snapshot->detail.mergeability == "conflicting"},
			    {"readFailing", snapshot != nullptr && snapshot->failed_reads > 0}, {"paused", m_paused_until_ms != 0}};
			if (link.notice_serial > 0)
				row["notice"] = {{"serial", link.notice_serial}, {"message", link.notice_message}, {"tone", link.notice_tone}};
			rows.push_back(std::move(row));
		}
		return rows;
	}

	PrWatchService::ReadResult PrWatchService::Read(const ReadJob& job, const GhRunner& runner, std::int64_t now_ms, std::stop_token stop)
	{
		ReadResult result{.key = job.key, .read_at_ms = now_ms, .viewer = job.viewer};
		bool fresh = false;
		// Revalidates one endpoint: an unchanged answer (304) costs nothing against the rate limit and reuses the kept body.
		const auto get = [&](const std::string& endpoint) -> std::optional<std::string> {
			if (result.rate_limited || stop.stop_requested()) return std::nullopt;
			const auto previous = job.validators.find(endpoint);
			std::vector<std::string> arguments = {"api", "-i", endpoint, "-H", "Accept: application/vnd.github+json"};
			if (previous != job.validators.end() && !previous->second.etag.empty())
			{
				arguments.push_back("-H");
				arguments.push_back("If-None-Match: " + previous->second.etag);
			}
			const pr_watch::GhResponse response = pr_watch::ParseGhApiOutput(runner(arguments, stop), now_ms);
			if (response.rate_limited)
			{
				result.rate_limited = true;
				result.retry_at_ms = response.retry_at_ms;
				return std::nullopt;
			}
			if (response.status == 304 && previous != job.validators.end())
			{
				result.validators[endpoint] = previous->second;
				return previous->second.body;
			}
			if (response.status != 200)
			{
				result.status = response.status;
				return std::nullopt;
			}
			fresh = true;
			result.validators[endpoint] = {.etag = response.etag, .body = response.body};
			return response.body;
		};

		const std::string root = "repos/" + job.pr.repository;
		const std::string number = std::to_string(job.pr.number);
		const std::optional<std::string> pull = get(root + "/pulls/" + number);
		if (!pull.has_value() || !pr_watch::DecodePull(*pull, result.detail))
		{
			result.not_found = result.status == 404;
			return result;
		}
		pr_watch::Detail& detail = result.detail;

		// ponytail: one page of 100 per endpoint; page through `Link: rel="next"` if a pull request ever outgrows it.
		fresh = false;
		const std::string commit = root + "/commits/" + detail.head_sha;
		const std::optional<std::string> runs = get(commit + "/check-runs?per_page=100");
		const std::optional<std::string> statuses = get(commit + "/status?per_page=100");
		// A fork's workflows wait for a maintainer before any check run exists.
		const std::optional<std::string> held = detail.fork ? get(root + "/actions/runs?head_sha=" + detail.head_sha + "&event=pull_request&status=action_required&per_page=100") : std::optional<std::string>{"{\"workflow_runs\":[]}"};
		if (result.rate_limited) return result;
		detail.checks_unread = !runs.has_value() || !pr_watch::DecodeCheckRuns(*runs, detail.checks);
		detail.checks_unread = !statuses.has_value() || !pr_watch::DecodeCombinedStatus(*statuses, detail.checks) || detail.checks_unread;
		detail.checks_unread = !held.has_value() || !pr_watch::DecodeHeldWorkflowRuns(*held, detail.checks) || detail.checks_unread;

		if (!detail.checks.empty())
		{
			// Which checks are required is asked again only when the checks changed.
			const std::string required_key = "required:" + detail.head_sha;
			const auto kept = job.validators.find(required_key);
			std::string body = kept == job.validators.end() ? std::string{} : kept->second.body;
			if (fresh || kept == job.validators.end())
			{
				const std::size_t slash = job.pr.repository.find('/');
				const pr_watch::GhResponse response = pr_watch::ParseGhApiOutput(runner({"api", "graphql", "-i", "-f", "query=" + std::string(kRequiredQuery),
				    "-F", "owner=" + job.pr.repository.substr(0, slash), "-F", "name=" + job.pr.repository.substr(slash + 1), "-F", "number=" + number}, stop), now_ms);
				if (response.status == 200) body = response.body;
			}
			std::vector<std::string> required;
			if (!body.empty() && pr_watch::DecodeRequiredChecks(body, required))
			{
				result.validators[required_key] = {.body = body};
				for (pr_watch::Check& check : detail.checks) check.required = std::ranges::find(required, check.name) != required.end();
			}
		}

		if (job.want_remarks)
		{
			const std::string since = "&since=" + pr_watch::FormatIsoMs(job.remarks_since_ms);
			const std::optional<std::string> comments = get(root + "/issues/" + number + "/comments?per_page=100" + since);
			const std::optional<std::string> review_comments = get(root + "/pulls/" + number + "/comments?per_page=100" + since);
			const std::optional<std::string> reviews = get(root + "/pulls/" + number + "/reviews?per_page=100");
			if (result.rate_limited) return result;
			result.remarks_read = comments.has_value() && review_comments.has_value() && reviews.has_value() &&
			    pr_watch::DecodeIssueComments(*comments, result.remarks) && pr_watch::DecodeReviewComments(*review_comments, result.remarks) &&
			    pr_watch::DecodeReviews(*reviews, result.remarks);
			if (result.viewer.empty())
			{
				const pr_watch::GhResponse response = pr_watch::ParseGhApiOutput(runner({"api", "-i", "user"}, stop), now_ms);
				if (response.status == 200) pr_watch::DecodeViewer(response.body, result.viewer);
			}
			// Without knowing the signed-in account, its own comments cannot be told from other people's; none are reported this read.
			if (result.viewer.empty()) result.remarks_read = false;
		}
		detail.viewer = result.viewer;
		result.status = 0;
		result.ok = !stop.stop_requested();
		return result;
	}

	void PrWatchService::StartSweep(const AppState& app, std::int64_t now_ms)
	{
		std::vector<ReadJob> jobs;
		for (auto& [key, snapshot] : m_snapshots)
		{
			pr_watch::CadenceInput cadence{.has_snapshot = snapshot.has_detail, .missing = snapshot.missing, .state = snapshot.detail.state,
			    .updated_at_ms = snapshot.detail.updated_at_ms, .closed_at_ms = snapshot.detail.closed_at_ms, .last_read_ms = snapshot.last_read_ms};
			ReadJob job{.key = key, .pr = snapshot.pr, .viewer = m_viewer};
			// Chats watching the same pull request share one read.
			for (const Link& link : m_links)
			{
				if (pr_watch::Key(link.pr) != key || ChatDomainService().FindChatById(app, link.chat_id) == nullptr) continue;
				cadence.linked = true;
				if (!link.watching) continue;
				job.remarks_since_ms = cadence.watched ? std::min(job.remarks_since_ms, link.reported.remarks_through_ms) : link.reported.remarks_through_ms;
				cadence.watched = true;
			}
			if (!pr_watch::IsDue(cadence, now_ms)) continue;
			job.want_remarks = cadence.watched;
			job.validators = snapshot.validators;
			jobs.push_back(std::move(job));
		}
		if (jobs.empty()) return;
		std::packaged_task<std::vector<ReadResult>()> task([jobs = std::move(jobs), runner = m_runner, stop = m_stop.get_token(), now_ms]() {
			std::vector<ReadResult> results;
			for (const ReadJob& job : jobs)
			{
				if (stop.stop_requested()) break;
				try { results.push_back(Read(job, runner, now_ms, stop)); }
				catch (const std::exception&) { results.push_back({.key = job.key, .read_at_ms = now_ms}); }
				// One account's limit covers every read, so the rest of the sweep waits too.
				if (results.back().rate_limited) break;
			}
			return results;
		});
		m_worker = task.get_future();
		uam::platform::StartBackgroundThread(std::move(task)).detach();
	}

	void PrWatchService::Apply(const std::vector<ReadResult>& results, std::int64_t now_ms)
	{
		for (const ReadResult& result : results)
		{
			const auto found = m_snapshots.find(result.key);
			if (found == m_snapshots.end()) continue; // Unlinked while the read ran.
			Snapshot& snapshot = found->second;
			const std::string label = snapshot.pr.repository + " #" + std::to_string(snapshot.pr.number);
			if (result.rate_limited)
			{
				// A rate limit pauses watching; it is not a failed read.
				if (m_paused_until_ms == 0) uam::diagnostics::Write("PR watch: GitHub rate limit reached; reads are paused until it resets.");
				m_paused_until_ms = std::max(m_paused_until_ms, result.retry_at_ms);
				m_dirty = true;
				continue;
			}
			snapshot.last_read_ms = result.read_at_ms;
			if (!result.viewer.empty()) m_viewer = result.viewer;
			if (!result.ok)
			{
				snapshot.missing = result.not_found;
				++snapshot.failed_reads;
				if (snapshot.failed_reads == 1)
					uam::diagnostics::Write("PR watch: a pull request read failed (HTTP " + std::to_string(result.status) + "); it is retried on the usual cadence.");
				if (snapshot.failed_reads == pr_watch::kFailedReadLimit)
				{
					uam::diagnostics::Write("PR watch: " + std::to_string(pr_watch::kFailedReadLimit) + " reads of a pull request failed in a row; its watches ended.");
					for (Link& link : m_links)
					{
						if (pr_watch::Key(link.pr) != result.key || !link.watching) continue;
						link.watching = false;
						Notify(link, "Stopped watching " + label + ": GitHub could not be read " + std::to_string(pr_watch::kFailedReadLimit) +
						             " times in a row. Check that gh is installed and signed in, then watch it again.", "warning");
					}
				}
				m_dirty = true;
				continue;
			}
			snapshot.failed_reads = 0;
			snapshot.missing = false;
			snapshot.has_detail = true;
			snapshot.detail = result.detail;
			snapshot.validators = result.validators;
			for (Link& link : m_links)
			{
				if (pr_watch::Key(link.pr) != result.key || !link.watching) continue;
				if (result.detail.state != "open")
				{
					// One final message tells the agent, then the watch ends; the message is still delivered below.
					const pr_watch::Message ended = pr_watch::BuildEndedMessage(link.pr, result.detail.state);
					link.pending_wake += (link.pending_wake.empty() ? "" : "\n\n") + ended.text;
					link.watching = false;
					Notify(link, snapshot.pr.repository + " " + ended.summary, result.detail.state == "merged" ? "success" : "info");
					continue;
				}
				pr_watch::Report report = pr_watch::Evaluate(link.reported, result.detail, result.remarks_read ? &result.remarks : nullptr, now_ms);
				link.reported = std::move(report.next);
				if (report.changes.empty()) continue;
				const pr_watch::Message message = pr_watch::BuildMessage(link.pr, result.detail, report);
				link.pending_wake += (link.pending_wake.empty() ? "" : "\n\n") + message.text;
				// Undelivered news is bounded; the newest is kept.
				if (link.pending_wake.size() > kMaxPendingWakeBytes) link.pending_wake = message.text.substr(0, kMaxPendingWakeBytes);
				Notify(link, snapshot.pr.repository + " " + message.summary, message.outcome == "completed" ? "success" : message.outcome == "failed" ? "warning" : "info");
				if (report.exhausted) link.watching = false;
			}
			m_dirty = true;
		}
	}

	bool PrWatchService::DeliverWakes(AppState& app, std::int64_t now_ms)
	{
		if (now_ms < m_next_wake_retry_ms || std::ranges::none_of(m_links, [](const Link& link) { return !link.pending_wake.empty(); })) return false;
		// The news is saved before it is sent, so a crash between the two delivers it after restart instead of losing it.
		if (!Save()) return false;
		bool delivered = false;
		for (Link& link : m_links)
		{
			if (link.pending_wake.empty()) continue;
			std::string error;
			if (m_sender(app, link.chat_id, link.pending_wake, &error))
			{
				link.pending_wake.clear();
				link.wake_refusals = 0;
				delivered = true;
				continue;
			}
			// The runtime's reason can carry private text; only the chat is named, and only for the first refusal.
			if (++link.wake_refusals == 1) uam::diagnostics::Write("PR watch: chat " + link.chat_id + " did not accept a wake; it is retried every minute.");
			m_next_wake_retry_ms = now_ms + kWakeRetryMs;
			if (link.wake_refusals < kWakeRefusalLimit) continue;
			uam::diagnostics::Write("PR watch: chat " + link.chat_id + " refused a wake " + std::to_string(kWakeRefusalLimit) + " times; its watch ended.");
			link.watching = false;
			link.pending_wake.clear();
			link.wake_refusals = 0;
			Notify(link, "Stopped watching " + link.pr.repository + " #" + std::to_string(link.pr.number) + ": the chat did not accept its updates. Open the chat in Chat view and watch it again.", "warning");
			delivered = true;
		}
		if (delivered)
		{
			m_dirty = true;
			Save();
		}
		return delivered;
	}

	bool PrWatchService::Poll(AppState& app)
	{
		return Poll(app, uam::time::SystemEpochMillisecondsNow());
	}

	bool PrWatchService::Poll(AppState& app, std::int64_t now_ms)
	{
		if (m_links.empty() && !m_worker.valid()) return false;
		bool applied = false;
		if (m_worker.valid() && m_worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			std::vector<ReadResult> results;
			try { results = m_worker.get(); }
			catch (const std::exception&) { uam::diagnostics::Write("PR watch: a sweep ended unexpectedly; the next one starts on schedule."); }
			Apply(results, now_ms);
			applied = true;
		}
		// The app polls many times a second; the chat scan and wake delivery below run once a second or after news.
		if (!applied && !m_dirty && now_ms < m_next_housekeeping_ms) return false;
		m_next_housekeeping_ms = now_ms + 1000;
		if (m_paused_until_ms != 0 && now_ms >= m_paused_until_ms)
		{
			m_paused_until_ms = 0;
			m_dirty = true;
		}

		// A deleted chat takes its links with it; a chat the user marks done keeps them but stops watching.
		// ponytail: an empty chat list counts as "not loaded yet" until chats were seen once, so startup can never wipe the
		// watch list; after that, deleting the last chat removes its links too.
		m_chats_seen = m_chats_seen || !app.chats.empty();
		if (m_chats_seen)
		{
			std::set<std::string> orphaned;
			for (Link& link : m_links)
			{
				const ChatSession* chat = ChatDomainService().FindChatById(app, link.chat_id);
				if (chat == nullptr) orphaned.insert(link.chat_id);
				else if ((link.watching || !link.pending_wake.empty()) && chat->settled_at != link.settled_seen && !chat->settled_at.empty())
				{
					// Marked done since the watch started: it ends, and news not yet delivered (such as a final merged message) is dropped.
					link.watching = false;
					link.pending_wake.clear();
					m_dirty = true;
				}
			}
			if (!orphaned.empty())
			{
				std::erase_if(m_links, [&](const Link& link) { return orphaned.contains(link.chat_id); });
				std::erase_if(m_snapshots, [&](const auto& entry) {
					return std::ranges::none_of(m_links, [&](const Link& link) { return pr_watch::Key(link.pr) == entry.first; });
				});
				m_dirty = true;
			}
		}

		const bool delivered = DeliverWakes(app, now_ms);
		if (!m_worker.valid() && now_ms >= m_next_sweep_ms && now_ms >= m_paused_until_ms)
		{
			m_next_sweep_ms = now_ms + pr_watch::kSweepIntervalMs;
			StartSweep(app, now_ms);
		}
		const bool changed = m_dirty;
		if (m_dirty) Save();
		return changed || delivered;
	}
} // namespace uam
