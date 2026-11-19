#include "common/pr_watch/pr_watch_rules.h"

#include "common/utils/string_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <set>

namespace uam::pr_watch
{
	namespace
	{
		constexpr std::size_t kListedItems = 10;
		constexpr std::size_t kSnippetBytes = 200;

		bool Contains(const std::vector<std::string>& values, std::string_view value)
		{
			return std::ranges::find(values, value) != values.end();
		}

		bool IsFailed(const Check& check)
		{
			return check.status == "failure" || check.status == "cancelled";
		}

		bool IsUnfinished(const Check& check)
		{
			return check.status == "pending" || check.status == "action-required";
		}

		/// A GitHub Actions job URL extends its workflow run URL; their names need not match.
		std::string WorkflowRunOf(std::string_view url)
		{
			constexpr std::string_view marker = "/actions/runs/";
			const std::size_t at = url.rfind(marker);
			if (at == std::string_view::npos) return {};
			std::size_t end = at + marker.size();
			while (end < url.size() && url[end] >= '0' && url[end] <= '9') ++end;
			return end == at + marker.size() ? std::string{} : std::string(url.substr(0, end));
		}

		std::string ApprovalKey(const Check& check)
		{
			const std::string run = WorkflowRunOf(check.url);
			return run.empty() ? check.name : run;
		}

		std::vector<std::string> Names(const std::vector<Check>& checks)
		{
			std::vector<std::string> names;
			for (const Check& check : checks) names.push_back(check.name);
			return names;
		}

		bool SafeNamePart(std::string_view value)
		{
			return !value.empty() && value.size() <= 100 && value != "." && value != ".." &&
			       std::ranges::all_of(value, [](unsigned char ch) {
				       return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.';
			       });
		}

		std::optional<int> PositiveNumber(std::string_view text)
		{
			int value = 0;
			const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
			if (error != std::errc{} || end != text.data() + text.size() || value <= 0) return std::nullopt;
			return value;
		}

		/// Comment text on one line, without HTML comments, cut on a UTF-8 character boundary.
		std::string Snippet(std::string_view body)
		{
			std::string text;
			bool space = false;
			for (std::size_t index = 0; index < body.size();)
			{
				if (body.substr(index).starts_with("<!--"))
				{
					const std::size_t close = body.find("-->", index + 4);
					index = close == std::string_view::npos ? body.size() : close + 3;
					space = true;
					continue;
				}
				const char ch = body[index++];
				if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') { space = true; continue; }
				if (space && !text.empty()) text.push_back(' ');
				space = false;
				text.push_back(ch);
			}
			if (text.size() <= kSnippetBytes) return text;
			std::size_t end = kSnippetBytes - 3;
			while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
			return text.substr(0, end) + "...";
		}

		template <typename Item, typename Line> void Listed(std::vector<std::string>& lines, const std::vector<Item>& items, Line line)
		{
			for (std::size_t index = 0; index < items.size() && index < kListedItems; ++index) lines.push_back(line(items[index]));
			if (items.size() > kListedItems) lines.push_back("  - and " + std::to_string(items.size() - kListedItems) + " more");
		}

		std::string SummaryOf(ChangeKind kind)
		{
			switch (kind)
			{
				case ChangeKind::ChecksFailed: return "checks failed";
				case ChangeKind::ChecksPassed: return "checks passed";
				case ChangeKind::ChecksAwaitingApproval: return "checks await approval";
				case ChangeKind::Remarks: return "new comments";
				case ChangeKind::NoChecks: return "no checks reported";
				case ChangeKind::Conflicting: return "merge conflict";
			}
			return {};
		}

		std::string JsonString(const nlohmann::json& object, const char* key)
		{
			const auto value = object.find(key);
			return value != object.end() && value->is_string() ? value->get<std::string>() : std::string{};
		}

		std::string Login(const nlohmann::json& object, const char* key = "user")
		{
			const auto user = object.find(key);
			return user != object.end() && user->is_object() ? JsonString(*user, "login") : std::string{};
		}

		std::string IdOf(const nlohmann::json& object)
		{
			const auto id = object.find("id");
			return id != object.end() && id->is_number_integer() ? std::to_string(id->get<std::int64_t>()) : std::string{};
		}

		nlohmann::json Parse(std::string_view body)
		{
			return nlohmann::json::parse(body, nullptr, false);
		}
	} // namespace

	std::optional<Reference> ParseReference(std::string_view url_or_repository, int number)
	{
		std::string text = uam::strings::Trim(url_or_repository);
		bool link = false;
		for (std::string_view prefix : {"https://", "http://"})
		{
			if (!uam::strings::StartsWithIgnoreCase(text, prefix)) continue;
			text.erase(0, prefix.size());
			link = true;
		}
		for (std::string_view host : {"www.github.com/", "github.com/"})
		{
			if (!uam::strings::StartsWithIgnoreCase(text, host)) continue;
			link = false;
			text.erase(0, host.size());
			if (const std::size_t cut = text.find_first_of("?#"); cut != std::string::npos) text.erase(cut);
			// owner/name/pull/12[/files...]
			const std::size_t owner_end = text.find('/');
			const std::size_t name_end = owner_end == std::string::npos ? owner_end : text.find('/', owner_end + 1);
			if (name_end == std::string::npos || !text.substr(name_end).starts_with("/pull/")) return std::nullopt;
			const std::string rest = text.substr(name_end + 6);
			const std::optional<int> parsed = PositiveNumber(rest.substr(0, rest.find('/')));
			if (!parsed.has_value()) return std::nullopt;
			number = *parsed;
			text.erase(name_end);
			break;
		}
		// A link to any other host is refused instead of being read as an owner/name on GitHub.
		if (link) return std::nullopt;
		if (const std::size_t hash = text.find('#'); hash != std::string::npos)
		{
			const std::optional<int> parsed = PositiveNumber(std::string_view(text).substr(hash + 1));
			if (!parsed.has_value()) return std::nullopt;
			number = *parsed;
			text.erase(hash);
		}
		const std::size_t slash = text.find('/');
		if (number <= 0 || slash == std::string::npos || !SafeNamePart(std::string_view(text).substr(0, slash)) ||
		    !SafeNamePart(std::string_view(text).substr(slash + 1))) return std::nullopt;
		return Reference{.repository = text, .number = number};
	}

	std::string Key(const Reference& reference)
	{
		return uam::strings::ToLowerAscii(reference.repository) + "#" + std::to_string(reference.number);
	}

	std::string Url(const Reference& reference)
	{
		return "https://github.com/" + reference.repository + "/pull/" + std::to_string(reference.number);
	}

	Report Evaluate(const Reported& watch, const Detail& detail, const std::vector<Remark>* remarks, std::int64_t now_ms)
	{
		Report report;
		std::vector<Change>& changes = report.changes;
		const std::string head_sha = detail.head_sha.empty() ? watch.head_sha : detail.head_sha;
		const bool head_moved = head_sha != watch.head_sha;

		// A source can return no checks on a failed read; prior state is preserved.
		std::vector<std::string> failed_checks = head_moved ? std::vector<std::string>{} : watch.failed_checks;
		bool passed = !head_moved && watch.passed;
		std::vector<std::string> passed_checks = head_moved ? std::vector<std::string>{} : watch.passed_checks;
		std::vector<std::string> owed_checks = head_moved ? std::vector<std::string>{} : watch.owed_checks;
		std::vector<std::string> awaiting_approval = head_moved ? std::vector<std::string>{} : watch.awaiting_approval;
		bool approval_announced = !head_moved && watch.approval_announced;

		const std::vector<Check>& checks = detail.checks;
		std::vector<Check> failed, awaiting, required;
		for (const Check& check : checks)
		{
			if (IsFailed(check)) failed.push_back(check);
			if (check.status == "action-required") awaiting.push_back(check);
			if (check.required) required.push_back(check);
		}
		const std::vector<std::string> told_failed = failed_checks;
		const std::vector<std::string> held_before = awaiting_approval;
		if (!checks.empty())
		{
			std::vector<Check> newly_failed;
			for (const Check& check : failed)
				if (!Contains(failed_checks, check.name)) newly_failed.push_back(check);
			if (!newly_failed.empty()) changes.push_back({.kind = ChangeKind::ChecksFailed, .checks = std::move(newly_failed)});
			if (!approval_announced && !awaiting.empty())
			{
				changes.push_back({.kind = ChangeKind::ChecksAwaitingApproval, .checks = awaiting});
				approval_announced = true;
			}
			failed_checks = Names(failed);

			// Approval reads can omit gated runs, so each hold is kept until its jobs finish.
			std::vector<std::string> awaiting_now;
			for (const Check& check : awaiting)
				if (const std::string key = ApprovalKey(check); !Contains(awaiting_now, key)) awaiting_now.push_back(key);
			const auto name_unfinished = [&](std::string_view name) {
				bool found = false;
				for (const Check& check : checks)
				{
					if (check.name != name) continue;
					if (IsUnfinished(check)) return true;
					found = true;
				}
				return !found;
			};
			// GitHub deduplicates jobs across runs, so a visible pending job may hide one in an awaited run.
			const bool jobs_pending = std::ranges::any_of(checks, [](const Check& check) {
				return check.status == "pending" && (check.url.empty() || !WorkflowRunOf(check.url).empty());
			});
			std::set<std::string> finished;
			if (!jobs_pending)
				for (const Check& check : checks)
					if (!IsUnfinished(check)) finished.insert(WorkflowRunOf(check.url));
			std::vector<std::string> held = awaiting_now;
			for (const std::string& key : awaiting_approval)
			{
				if (Contains(awaiting_now, key)) continue;
				if (WorkflowRunOf(key) == key ? !finished.contains(key) : name_unfinished(key)) held.push_back(key);
			}
			awaiting_approval = std::move(held);
			// A partial read lists only some checks; what was already told stays told, or the next full read would repeat it.
			if (detail.checks_unread)
			{
				for (const std::string& name : told_failed)
					if (!Contains(failed_checks, name)) failed_checks.push_back(name);
				for (const std::string& key : held_before)
					if (!Contains(awaiting_approval, key)) awaiting_approval.push_back(key);
			}
		}

		const std::vector<Check>& gate = required.empty() ? checks : required;
		const bool passed_now = awaiting_approval.empty() &&
		    std::ranges::all_of(gate, [](const Check& check) { return check.status != "pending" && !IsFailed(check); });
		// An unread check source cannot establish success.
		if (!checks.empty() && !(detail.checks_unread && passed_now))
		{
			const std::vector<std::string> gate_names = Names(gate);
			const std::vector<std::string>& told = passed && passed_checks.empty() ? gate_names : passed_checks;
			// Required checks can finish between reads; with none required, later advisory checks are not news.
			const bool gate_grew = !required.empty() &&
			    std::ranges::any_of(gate_names, [&](const std::string& name) { return !Contains(told, name); });
			// Other checks still pending when the required ones passed are owed one wake when all settle.
			std::vector<std::string> owed;
			if (passed_now && passed)
				for (const Check& check : checks)
					if (!check.required && Contains(owed_checks, check.name)) owed.push_back(check.name);
			const bool settled = failed.empty() && awaiting_approval.empty() &&
			    std::ranges::none_of(checks, [](const Check& check) { return check.status == "pending"; });
			owed_checks = owed;
			if (passed_now && (!passed || gate_grew))
			{
				changes.push_back({.kind = ChangeKind::ChecksPassed, .count = static_cast<int>(gate.size()), .required = !required.empty()});
				owed_checks.clear();
				if (!required.empty())
					for (const Check& check : checks)
						if (!check.required && check.status == "pending") owed_checks.push_back(check.name);
			}
			else if (settled && !owed.empty())
			{
				changes.push_back({.kind = ChangeKind::ChecksPassed, .count = static_cast<int>(checks.size())});
				owed_checks.clear();
			}
			passed = passed_now;
			passed_checks = passed_now ? gate_names : std::vector<std::string>{};
		}

		// Only a head never read with checks waits for them; an empty read after checks were seen is a failed read.
		const std::int64_t awaited_before = head_moved ? now_ms : watch.checks_awaited_since_ms;
		std::int64_t checks_awaited_since = checks.empty() ? awaited_before : 0;
		const std::int64_t awaited_ms = checks_awaited_since == 0 ? 0 : now_ms - checks_awaited_since;
		if (!detail.checks_unread && awaited_ms >= kNoChecksMs)
		{
			changes.push_back({.kind = ChangeKind::NoChecks, .minutes = static_cast<int>((awaited_ms + 30000) / 60000)});
			checks_awaited_since = 0;
		}

		const std::string own = uam::strings::ToLowerAscii(detail.viewer.empty() ? detail.author : detail.viewer);
		const std::int64_t through = watch.remarks_through_ms;
		std::vector<Remark> fresh;
		if (remarks != nullptr)
			for (const Remark& remark : *remarks)
				// Hosts stamp remarks to the second, so remarks at the boundary time are told apart by ID.
				if ((remark.at_ms > through || (remark.at_ms == through && !Contains(watch.remark_ids, remark.id))) &&
				    uam::strings::ToLowerAscii(remark.author) != own) fresh.push_back(remark);
		std::int64_t latest = through;
		for (const Remark& remark : fresh) latest = std::max(latest, remark.at_ms);
		std::vector<std::string> remark_ids = latest == through ? watch.remark_ids : std::vector<std::string>{};
		for (const Remark& remark : fresh)
			if (remark.at_ms == latest) remark_ids.push_back(remark.id);
		if (!fresh.empty()) changes.push_back({.kind = ChangeKind::Remarks, .remarks = std::move(fresh)});

		// Like a failed check, a conflict that a push did not resolve is news for the new head.
		const bool told_conflict = !head_moved && watch.conflicting;
		if (detail.mergeability == "conflicting" && !told_conflict) changes.push_back({.kind = ChangeKind::Conflicting});
		// "unknown" is GitHub still computing after a push; only a clean answer clears a conflict.
		const bool conflicting = detail.mergeability == "unknown" ? told_conflict : detail.mergeability == "conflicting";

		const bool comments_only = !changes.empty() &&
		    std::ranges::all_of(changes, [](const Change& change) { return change.kind == ChangeKind::Remarks; });
		const bool progress = head_moved || (!changes.empty() && !comments_only);
		const int wakes = (progress ? 0 : watch.wakes) + (comments_only ? 1 : 0);
		report.next = {.head_sha = head_sha, .failed_checks = std::move(failed_checks), .passed = passed,
		    .passed_checks = std::move(passed_checks), .owed_checks = std::move(owed_checks),
		    .awaiting_approval = std::move(awaiting_approval), .approval_announced = approval_announced,
		    .remarks_through_ms = latest, .remark_ids = std::move(remark_ids), .conflicting = conflicting, .wakes = wakes,
		    .checks_awaited_since_ms = checks_awaited_since};
		report.exhausted = comments_only && wakes >= kWakeLimit;
		return report;
	}

	Message BuildMessage(const Reference& reference, const Detail& detail, const Report& report)
	{
		const std::string commit = detail.head_sha.empty() ? std::string{} : " on " + detail.head_sha.substr(0, 7);
		std::vector<std::string> lines = {"Update on pull request #" + std::to_string(reference.number) + " (" + Url(reference) +
		                                  "), which UAM is watching for this chat:"};
		std::vector<std::string> summary;
		bool failed = false;
		bool only_passed = true;
		for (const Change& change : report.changes)
		{
			summary.push_back(SummaryOf(change.kind));
			failed = failed || change.kind == ChangeKind::ChecksFailed || change.kind == ChangeKind::Conflicting;
			only_passed = only_passed && change.kind == ChangeKind::ChecksPassed;
			switch (change.kind)
			{
				case ChangeKind::ChecksFailed:
					lines.push_back("- Checks failed" + commit + ":");
					Listed(lines, change.checks, [](const Check& check) {
						return "  - " + check.name + (check.status == "failure" ? "" : " (" + check.status + ")") + (check.url.empty() ? "" : " " + check.url);
					});
					break;
				case ChangeKind::ChecksAwaitingApproval:
					lines.push_back("- Checks are waiting for approval or action from a maintainer" + commit + ":");
					Listed(lines, change.checks, [](const Check& check) { return "  - " + check.name + (check.url.empty() ? "" : " " + check.url); });
					break;
				case ChangeKind::ChecksPassed:
					lines.push_back("- All " + std::to_string(change.count) + (change.required ? " required" : "") +
					                (change.count == 1 ? " check" : " checks") + " passed" + commit + ".");
					break;
				case ChangeKind::Remarks:
					lines.push_back("- " + std::to_string(change.remarks.size()) + (change.remarks.size() == 1 ? " new comment:" : " new comments:"));
					Listed(lines, change.remarks, [](const Remark& remark) {
						const std::string body = Snippet(remark.body);
						const std::string said = body.empty() ? (remark.review_state.empty() ? "reviewed" : remark.review_state) : "\"" + body + "\"";
						return "  - " + (remark.author.empty() ? "someone" : remark.author) + (remark.path.empty() ? "" : " on " + remark.path) +
						       ": " + said + (remark.url.empty() ? "" : " " + remark.url);
					});
					break;
				case ChangeKind::NoChecks:
					lines.push_back("- No checks have reported" + commit + " after " + std::to_string(change.minutes) +
					                " minutes. None may be configured to run on this change.");
					break;
				case ChangeKind::Conflicting:
					lines.push_back("- The branch now conflicts with " + (detail.base_branch.empty() ? "its base branch" : detail.base_branch) + ".");
					break;
			}
		}
		lines.push_back("");
		lines.push_back(report.exhausted
		        ? "UAM stopped watching after " + std::to_string(kWakeLimit) + " comment-only updates in a row. Call pr_watch to watch it again."
		        : "Look into each item and act on it as your task requires. This pull request stays watched, so do not call pr_watch for it again. End your turn when you are done and UAM wakes this chat on the next change. Call pr_unwatch when you hand the work back to the user.");
		if (report.exhausted) summary.push_back("stopped watching");
		const auto join = [](const std::vector<std::string>& parts, std::string_view separator) {
			std::string text;
			for (std::size_t index = 0; index < parts.size(); ++index) text.append(index == 0 ? "" : separator).append(parts[index]);
			return text;
		};
		return {.text = join(lines, "\n"), .summary = "#" + std::to_string(reference.number) + ": " + join(summary, ", "),
		        .outcome = failed ? "failed" : only_passed ? "completed" : "updated"};
	}

	Message BuildEndedMessage(const Reference& reference, std::string_view state)
	{
		const bool merged = state == "merged";
		const std::string number = "#" + std::to_string(reference.number);
		return {.text = "Update on pull request " + number + " (" + Url(reference) + "), which UAM was watching for this chat:\n- The pull request was " +
		                (merged ? "merged." : "closed without being merged.") +
		                "\n\nUAM stopped watching it, so no further updates will arrive. Finish anything that depended on it. Call pr_watch again only if it is reopened.",
		        .summary = number + (merged ? ": merged, stopped watching" : ": closed, stopped watching"), .outcome = merged ? "completed" : "updated"};
	}

	bool IsDue(const CadenceInput& input, std::int64_t now_ms)
	{
		if (!input.linked) return false;
		const bool never = input.last_read_ms == 0;
		const std::int64_t since = now_ms - input.last_read_ms;
		// "Not found" is an answer; it is asked again only on the slow cadence.
		if (input.missing) return never || since >= kSlowIntervalMs;
		if (!input.has_snapshot) return true;
		if (input.state == "merged") return false;
		if (input.state == "closed" && now_ms - input.closed_at_ms >= kClosedFinalAfterMs) return false;
		if (never) return true;
		if (input.watched) return since >= kWatchIntervalMs;
		if (input.state == "open")
		{
			const bool quiet = input.updated_at_ms != 0 && now_ms - input.updated_at_ms >= kQuietAfterMs;
			return since >= (quiet ? kQuietIntervalMs : kWatchIntervalMs);
		}
		return since >= kSlowIntervalMs;
	}

	GhResponse ParseGhApiOutput(std::string_view output, std::int64_t now_ms)
	{
		GhResponse response;
		std::size_t start = output.starts_with("HTTP/") ? 0 : output.find("\nHTTP/");
		if (start == std::string_view::npos) return response;
		if (start != 0) ++start;
		std::string remaining, reset, retry_after;
		bool status_line = true;
		std::size_t position = start;
		while (position <= output.size())
		{
			const std::size_t end = std::min(output.find('\n', position), output.size());
			std::string_view line = output.substr(position, end - position);
			position = end + 1;
			if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
			if (status_line)
			{
				status_line = false;
				const std::size_t space = line.find(' ');
				if (space == std::string_view::npos) return response;
				std::from_chars(line.data() + space + 1, line.data() + line.size(), response.status);
				continue;
			}
			if (line.empty()) break;
			const std::size_t colon = line.find(':');
			if (colon == std::string_view::npos) continue;
			const std::string name = uam::strings::ToLowerAscii(line.substr(0, colon));
			const std::string value = uam::strings::Trim(line.substr(colon + 1));
			if (name == "etag") response.etag = value;
			else if (name == "x-ratelimit-remaining") remaining = value;
			else if (name == "x-ratelimit-reset") reset = value;
			else if (name == "retry-after") retry_after = value;
		}
		if (position < output.size()) response.body = std::string(output.substr(position));
		response.rate_limited = response.status == 429 ||
		    (response.status == 403 && (remaining == "0" || !retry_after.empty() || uam::strings::ToLowerAscii(response.body).find("rate limit") != std::string::npos));
		if (response.rate_limited)
		{
			std::int64_t seconds = 0;
			if (!retry_after.empty() && std::from_chars(retry_after.data(), retry_after.data() + retry_after.size(), seconds).ec == std::errc{} && seconds > 0)
				response.retry_at_ms = now_ms + seconds * 1000;
			else if (!reset.empty() && std::from_chars(reset.data(), reset.data() + reset.size(), seconds).ec == std::errc{} && seconds * 1000 > now_ms)
				response.retry_at_ms = seconds * 1000;
			else response.retry_at_ms = now_ms + 60000;
		}
		return response;
	}

	std::int64_t ParseIsoMs(std::string_view iso)
	{
		int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
		// GitHub REST timestamps are always UTC with a trailing Z.
		if (iso.size() < 20 || std::sscanf(std::string(iso).c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute, &second) != 6) return 0;
		const std::chrono::year_month_day date{std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)}, std::chrono::day{static_cast<unsigned>(day)}};
		if (!date.ok()) return 0;
		const std::chrono::sys_seconds time = std::chrono::sys_days{date} + std::chrono::hours{hour} + std::chrono::minutes{minute} + std::chrono::seconds{second};
		return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
	}

	std::string FormatIsoMs(std::int64_t epoch_ms)
	{
		const std::chrono::sys_seconds time{std::chrono::seconds{epoch_ms / 1000}};
		const std::chrono::sys_days days = std::chrono::floor<std::chrono::days>(time);
		const std::chrono::year_month_day date{days};
		const std::chrono::hh_mm_ss clock{time - days};
		char buffer[32];
		std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02uT%02d:%02d:%02dZ", static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
		    static_cast<unsigned>(date.day()), static_cast<int>(clock.hours().count()), static_cast<int>(clock.minutes().count()), static_cast<int>(clock.seconds().count()));
		return buffer;
	}

	bool DecodePull(std::string_view body, Detail& detail)
	{
		const nlohmann::json pull = Parse(body);
		if (!pull.is_object() || !pull.contains("head") || !pull["head"].is_object() || !pull.contains("base") || !pull["base"].is_object()) return false;
		const nlohmann::json& head = pull["head"];
		const nlohmann::json& base = pull["base"];
		detail.head_sha = JsonString(head, "sha");
		if (detail.head_sha.empty()) return false;
		detail.state = pull.value("merged", false) ? "merged" : JsonString(pull, "state") == "closed" ? "closed" : "open";
		detail.title = JsonString(pull, "title");
		detail.base_branch = JsonString(base, "ref");
		detail.author = Login(pull);
		detail.updated_at_ms = ParseIsoMs(JsonString(pull, "updated_at"));
		detail.closed_at_ms = ParseIsoMs(JsonString(pull, "closed_at"));
		const auto mergeable = pull.find("mergeable");
		detail.mergeability = JsonString(pull, "mergeable_state") == "dirty" || (mergeable != pull.end() && mergeable->is_boolean() && !mergeable->get<bool>())
		        ? "conflicting" : mergeable != pull.end() && mergeable->is_boolean() ? "mergeable" : "unknown";
		// The head repository is null once the fork a pull request came from was deleted.
		const auto repo_id = [](const nlohmann::json& side) {
			const auto repo = side.find("repo");
			return repo != side.end() && repo->is_object() ? IdOf(*repo) : std::string{};
		};
		detail.fork = repo_id(head) != repo_id(base);
		return true;
	}

	bool DecodeCheckRuns(std::string_view body, std::vector<Check>& checks)
	{
		const nlohmann::json root = Parse(body);
		if (!root.is_object() || !root.contains("check_runs") || !root["check_runs"].is_array()) return false;
		for (const nlohmann::json& run : root["check_runs"])
		{
			if (!run.is_object()) continue;
			const std::string conclusion = JsonString(run, "conclusion");
			const std::string status = JsonString(run, "status") != "completed" ? "pending"
			    : conclusion == "success" ? "success"
			    : conclusion == "cancelled" ? "cancelled"
			    : conclusion == "action_required" ? "action-required"
			    : conclusion == "failure" || conclusion == "timed_out" || conclusion == "startup_failure" ? "failure" : "neutral";
			checks.push_back({.name = JsonString(run, "name"), .status = status, .url = JsonString(run, "html_url")});
		}
		return true;
	}

	bool DecodeCombinedStatus(std::string_view body, std::vector<Check>& checks)
	{
		const nlohmann::json root = Parse(body);
		if (!root.is_object() || !root.contains("statuses") || !root["statuses"].is_array()) return false;
		for (const nlohmann::json& status : root["statuses"])
		{
			if (!status.is_object()) continue;
			const std::string state = JsonString(status, "state");
			checks.push_back({.name = JsonString(status, "context"),
			    .status = state == "success" ? "success" : state == "pending" ? "pending" : "failure", .url = JsonString(status, "target_url")});
		}
		return true;
	}

	bool DecodeHeldWorkflowRuns(std::string_view body, std::vector<Check>& checks)
	{
		const nlohmann::json root = Parse(body);
		if (!root.is_object() || !root.contains("workflow_runs") || !root["workflow_runs"].is_array()) return false;
		for (const nlohmann::json& run : root["workflow_runs"])
			if (run.is_object()) checks.push_back({.name = JsonString(run, "name"), .status = "action-required", .url = JsonString(run, "html_url")});
		return true;
	}

	namespace
	{
		template <typename Each> bool DecodeRemarkArray(std::string_view body, Each each)
		{
			const nlohmann::json root = Parse(body);
			if (!root.is_array()) return false;
			for (const nlohmann::json& item : root)
				if (item.is_object()) each(item);
			return true;
		}

		std::int64_t ActiveAt(const nlohmann::json& item, const char* created)
		{
			const std::int64_t updated = ParseIsoMs(JsonString(item, "updated_at"));
			return updated != 0 ? updated : ParseIsoMs(JsonString(item, created));
		}
	} // namespace

	bool DecodeIssueComments(std::string_view body, std::vector<Remark>& remarks)
	{
		return DecodeRemarkArray(body, [&](const nlohmann::json& item) {
			remarks.push_back({.id = "issue-" + IdOf(item), .author = Login(item), .body = JsonString(item, "body"), .url = JsonString(item, "html_url"), .at_ms = ActiveAt(item, "created_at")});
		});
	}

	bool DecodeReviewComments(std::string_view body, std::vector<Remark>& remarks)
	{
		return DecodeRemarkArray(body, [&](const nlohmann::json& item) {
			remarks.push_back({.id = "review-comment-" + IdOf(item), .author = Login(item), .body = JsonString(item, "body"), .path = JsonString(item, "path"),
			    .url = JsonString(item, "html_url"), .at_ms = ActiveAt(item, "created_at")});
		});
	}

	bool DecodeReviews(std::string_view body, std::vector<Remark>& remarks)
	{
		return DecodeRemarkArray(body, [&](const nlohmann::json& item) {
			const std::string state = uam::strings::ToLowerAscii(JsonString(item, "state"));
			const std::string text = JsonString(item, "body");
			// A pending review is unsubmitted; a bare "commented" review only wraps its inline comments, which are read separately.
			if (state == "pending" || (state == "commented" && uam::strings::IsBlank(text))) return;
			remarks.push_back({.id = "review-" + IdOf(item), .author = Login(item), .body = text, .url = JsonString(item, "html_url"),
			    .review_state = state == "changes_requested" ? "requested changes" : state, .at_ms = ParseIsoMs(JsonString(item, "submitted_at"))});
		});
	}

	bool DecodeRequiredChecks(std::string_view body, std::vector<std::string>& names)
	{
		const nlohmann::json root = Parse(body);
		const nlohmann::json::json_pointer path("/data/repository/pullRequest/commits/nodes/0/commit/statusCheckRollup");
		if (!root.is_object() || !root.contains(nlohmann::json::json_pointer("/data/repository/pullRequest/commits/nodes"))) return false;
		if (!root.contains(path) || !root[path].is_object()) return true; // No rollup: the head has no checks yet.
		const nlohmann::json& rollup = root[path];
		if (!rollup.contains("contexts") || !rollup["contexts"].is_object() || !rollup["contexts"].contains("nodes") || !rollup["contexts"]["nodes"].is_array()) return false;
		for (const nlohmann::json& node : rollup["contexts"]["nodes"])
		{
			if (!node.is_object() || !node.value("isRequired", false)) continue;
			const std::string name = JsonString(node, "name");
			names.push_back(name.empty() ? JsonString(node, "context") : name);
		}
		return true;
	}

	bool DecodeViewer(std::string_view body, std::string& login)
	{
		const nlohmann::json root = Parse(body);
		if (!root.is_object()) return false;
		login = JsonString(root, "login");
		return !login.empty();
	}
} // namespace uam::pr_watch
