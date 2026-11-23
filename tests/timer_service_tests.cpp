#include "test_harness.h"
#include "app/automation_api_service.h"
#include "app/chat_domain_service.h"
#include "app/timer_service.h"
#include "app/uam_control_service.h"
#include "common/config/execution_host_config.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/scheduling/schedule_rules.h"
#include "common/utils/io_utils.h"
#include "common/utils/time_utils.h"

#include <stdexcept>

using namespace uam_test;
namespace sched = uam::scheduling;

namespace
{
	constexpr std::int64_t kMinute = sched::kMinuteMs;
	constexpr std::int64_t kHour = 60 * kMinute;
	constexpr std::int64_t kDay = 24 * kHour;
	// Monday 2026-11-23 08:00 local in a UTC+1 zone.
	const sched::LocalClock kClock = sched::FixedOffsetClock(60);
	const std::int64_t kMonday8 = kClock.from_local(2026, 11, 23, 8, 0);

	sched::Trigger Weekly(std::initializer_list<int> days, int hour, int minute)
	{
		sched::Trigger trigger{.kind = sched::TriggerKind::Weekly, .hour = hour, .minute = minute};
		for (int day : days) trigger.days[static_cast<std::size_t>(day)] = true;
		return trigger;
	}

	sched::Trigger Every(int minutes)
	{
		return {.kind = sched::TriggerKind::Interval, .interval_minutes = minutes};
	}

	/// A zone that leaves summer time at 2026-10-25T01:00:00Z: local 02:00 becomes 01:00, so 01:00-01:59 happens twice.
	/// An ambiguous local time resolves to its later instance, the case that could run a daily schedule twice.
	sched::LocalClock FallBackClock()
	{
		const std::int64_t change = sched::FixedOffsetClock(0).from_local(2026, 10, 25, 1, 0);
		const sched::LocalClock summer = sched::FixedOffsetClock(60), winter = sched::FixedOffsetClock(0);
		return {.to_local = [=](std::int64_t ms) { return ms < change ? summer.to_local(ms) : winter.to_local(ms); },
		    .from_local = [=](int year, int month, int day, int hour, int minute)
		    {
			    const std::int64_t as_winter = winter.from_local(year, month, day, hour, minute);
			    return as_winter >= change ? as_winter : summer.from_local(year, month, day, hour, minute);
		    }};
	}
} // namespace

UAM_TEST(ScheduleRulesParseTriggersAndRefuseInvalidOnes)
{
	std::string error;
	const auto interval = sched::ParseTrigger({{"kind", "interval"}, {"minutes", 90}}, &error);
	UAM_ASSERT(interval.has_value() && interval->kind == sched::TriggerKind::Interval && interval->interval_minutes == 90);
	const auto weekly = sched::ParseTrigger(nlohmann::json::parse(R"({"kind":"weekly","days":[1,3],"time":"09:05"})"), &error);
	UAM_ASSERT(weekly.has_value() && weekly->days[1] && weekly->days[3] && !weekly->days[0] && weekly->hour == 9 && weekly->minute == 5);
	UAM_ASSERT(sched::TriggerJson(*weekly) == nlohmann::json::parse(R"({"kind":"weekly","days":[1,3],"time":"09:05"})"));
	for (const char* bad : {R"({"kind":"interval","minutes":14})", R"({"kind":"interval","minutes":10081})", R"({"kind":"interval","minutes":"60"})",
	         R"({"kind":"weekly","days":[],"time":"09:00"})", R"({"kind":"weekly","days":[7],"time":"09:00"})", R"({"kind":"weekly","days":[1],"time":"24:00"})",
	         R"({"kind":"weekly","days":[1],"time":"9:00"})", R"({"kind":"cron"})", "null"})
	{
		error.clear();
		UAM_ASSERT(!sched::ParseTrigger(nlohmann::json::parse(bad), &error).has_value());
		UAM_ASSERT(!error.empty() && error.find('\n') == std::string::npos);
	}
	UAM_ASSERT(sched::ParseMissedPolicy("skip") == sched::MissedPolicy::Skip && sched::ParseMissedPolicy("runOnce") == sched::MissedPolicy::RunOnce);
	UAM_ASSERT(!sched::ParseMissedPolicy("twice").has_value());
}

UAM_TEST(ScheduleRulesComputeIntervalAndWeekdayRunsInLocalTime)
{
	// An interval counts from its anchor.
	UAM_ASSERT_EQ(sched::NextRunAfter(Every(60), kMonday8, kMonday8, kClock), kMonday8 + kHour);
	UAM_ASSERT_EQ(sched::NextRunAfter(Every(60), kMonday8, kMonday8 + 90 * kMinute, kClock), kMonday8 + 2 * kHour);
	// Monday and Wednesday at 09:00 local: Monday's run is still ahead at 08:00, then Wednesday, then next Monday.
	const sched::Trigger trigger = Weekly({1, 3}, 9, 0);
	const std::int64_t monday9 = kClock.from_local(2026, 11, 23, 9, 0);
	UAM_ASSERT_EQ(sched::NextRunAfter(trigger, 0, kMonday8, kClock), monday9);
	UAM_ASSERT_EQ(sched::NextRunAfter(trigger, 0, monday9, kClock), monday9 + 2 * kDay);
	UAM_ASSERT_EQ(sched::NextRunAfter(trigger, 0, monday9 + 2 * kDay, kClock), monday9 + 7 * kDay);
	// The local time is what counts: 09:00 in UTC+1 is 08:00 UTC.
	const sched::LocalTime local = kClock.to_local(monday9);
	UAM_ASSERT(local.hour == 9 && local.minute == 0 && local.weekday == 1 && local.day == 23 && local.month == 11 && local.year == 2026);
	UAM_ASSERT_EQ(sched::FixedOffsetClock(0).to_local(monday9).hour, 8);
}

UAM_TEST(ScheduleRulesRunADailyTimeOnceWhenClocksGoBack)
{
	const sched::LocalClock clock = FallBackClock();
	const sched::Trigger nightly = Weekly({0, 1, 2, 3, 4, 5, 6}, 1, 30);
	// Sunday 2026-10-25 local 01:00 (summer time) is 00:00Z.
	const std::int64_t before = sched::FixedOffsetClock(0).from_local(2026, 10, 25, 0, 0);
	const std::int64_t first = sched::NextRunAfter(nightly, 0, before, clock);
	UAM_ASSERT_EQ(clock.to_local(first).hour, 1);
	UAM_ASSERT_EQ(clock.to_local(first).minute, 30);
	// After that run the next one is the following night, not the repeated 01:30 an hour later.
	const std::int64_t second = sched::NextRunAfter(nightly, 0, first, clock);
	UAM_ASSERT(second - first > 20 * kHour);
	UAM_ASSERT_EQ(clock.to_local(second).day, 26);
	// A run at the earlier 01:30 does not run again at the later one either.
	const std::int64_t earlier = sched::FixedOffsetClock(60).from_local(2026, 10, 25, 1, 30);
	UAM_ASSERT_EQ(clock.to_local(sched::NextRunAfter(nightly, 0, earlier, clock)).day, 26);
}

UAM_TEST(ScheduleRulesRunATimeSkippedWhenClocksGoForwardOnce)
{
	// Like the C library on macOS: a local time in the skipped hour (02:00-02:59 on 2026-03-29) cannot be expressed.
	const sched::LocalClock base = sched::FixedOffsetClock(0);
	const sched::LocalClock clock{.to_local = base.to_local, .from_local = [base](int year, int month, int day, int hour, int minute)
	{ return year == 2026 && month == 3 && day == 29 && hour == 2 ? std::int64_t{-1} : base.from_local(year, month, day, hour, minute); }};
	const sched::Trigger nightly = Weekly({0, 1, 2, 3, 4, 5, 6}, 2, 30);
	const std::int64_t run = sched::NextRunAfter(nightly, 0, base.from_local(2026, 3, 29, 0, 0), clock);
	UAM_ASSERT_EQ(run, base.from_local(2026, 3, 29, 3, 30));
	UAM_ASSERT_EQ(clock.to_local(sched::NextRunAfter(nightly, 0, run, clock)).day, 30);
}

UAM_TEST(ScheduleRulesApplyTheMissedRunPolicy)
{
	const sched::Trigger hourly = Every(60);
	const std::int64_t next = kMonday8 + kHour;
	// Noticed on time: it runs, under either policy.
	for (sched::MissedPolicy policy : {sched::MissedPolicy::Skip, sched::MissedPolicy::RunOnce})
	{
		const sched::DueDecision decision = sched::Decide(hourly, policy, kMonday8, next, next + 2 * kMinute, kClock);
		UAM_ASSERT(decision.run && decision.skipped == 0 && decision.slot_ms == next && decision.next_run_ms == next + kHour);
	}
	// Three slots passed while UAM was closed: skip runs none, run-once runs one for all of them.
	const std::int64_t later = next + 2 * kHour + 30 * kMinute;
	const sched::DueDecision skipped = sched::Decide(hourly, sched::MissedPolicy::Skip, kMonday8, next, later, kClock);
	UAM_ASSERT(!skipped.run && skipped.skipped == 3 && skipped.next_run_ms == next + 3 * kHour);
	const sched::DueDecision once = sched::Decide(hourly, sched::MissedPolicy::RunOnce, kMonday8, next, later, kClock);
	UAM_ASSERT(once.run && once.skipped == 2 && once.slot_ms == next + 2 * kHour && once.next_run_ms == next + 3 * kHour);
	// Nothing is due before the slot.
	UAM_ASSERT(!sched::Decide(hourly, sched::MissedPolicy::RunOnce, kMonday8, next, next - 1, kClock).run);
}

namespace
{
	/// A data folder with one local workspace and an available Codex, as the automation API tests set up.
	struct TimerFixture
	{
		TempDir temp{"uam-timers"};
		uam::AppState app;
		std::shared_ptr<std::vector<std::pair<std::string, std::string>>> wakes = std::make_shared<std::vector<std::pair<std::string, std::string>>>();
		std::shared_ptr<bool> accept = std::make_shared<bool>(true);
		/// Credentials and requests the fake creator received; empty when the real one is used.
		std::shared_ptr<std::vector<uam::AutomationScheduleCredential>> runs = std::make_shared<std::vector<uam::AutomationScheduleCredential>>();
		std::shared_ptr<int> throw_on_run = std::make_shared<int>(0);
		std::shared_ptr<std::vector<std::string>> launched = std::make_shared<std::vector<std::string>>();
		bool fake_creator = true;
		std::unique_ptr<uam::TimerService> service;
		std::int64_t now = kMonday8;

		explicit TimerFixture(bool fake = true) : fake_creator(fake)
		{
			const fs::path workspace = temp.root / "workspace";
			fs::create_directories(workspace);
			app.data_root = temp.root / "data";
			fs::create_directories(app.data_root);
			app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
			app.settings.git_enabled = true;
			app.settings.git_auto_worktree = true;
			app.settings.git_snapshot_plain_folders = true;
			app.settings.git_snapshot_svn = true;
			app.provider_profiles = {ProviderProfileStore::DefaultCodexProfile()};
			ChatFolder folder;
			folder.id = "folder-a";
			folder.title = "folder-a";
			folder.directory = workspace.string();
			app.folders.push_back(folder);
			uam::CliProviderVersionState& version = app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey("codex-cli", "local")];
			version.checked = true;
			version.supported = true;
			version.installed_version = "1.2.3";
			version.execution_host = app.settings.execution_hosts.front();
			AddChat("chat-1");
			Restart();
		}
		void AddChat(const std::string& id)
		{
			ChatSession chat;
			chat.id = id;
			chat.title = "Parser work";
			app.chats.push_back(chat);
		}
		/// A new service on the same data folder, as after an app restart.
		void Restart()
		{
			uam::TimerService::ChatCreator creator;
			if (fake_creator)
				creator = [runs = runs, throw_on_run = throw_on_run](uam::AppState&, const uam::AutomationScheduleCredential& credential, const nlohmann::json&)
				{
					// Stands in for the app stopping after the run was recorded and before its chat existed.
					if (*throw_on_run > 0 && --*throw_on_run == 0) throw std::runtime_error("stopped");
					runs->push_back(credential);
					uam::AutomationApiResponse response{.body = {{"chatId", "run-" + std::to_string(runs->size())}, {"status", "awaitingApproval"}}};
					if (credential.trusted) response.launch_chat_id = response.body["chatId"];
					return response;
				};
			service = std::make_unique<uam::TimerService>(app.data_root,
			    [wakes = wakes, accept = accept](uam::AppState&, const std::string& chat_id, const std::string& text, std::string* error)
			    {
				    if (!*accept)
				    {
					    *error = "The provider is unavailable.";
					    return false;
				    }
				    wakes->emplace_back(chat_id, text);
				    return true;
			    },
			    creator, kClock);
			service->SetLauncher([launched = launched](uam::AppState&, const std::string&, const std::string& chat_id) { launched->push_back(chat_id); });
		}
		void PollAt(std::int64_t at)
		{
			now = at;
			service->Poll(app, now);
		}
		static nlohmann::json Request()
		{
			return {{"prompt", "Summarise yesterday's failures."}, {"workspaceId", "folder-a"}, {"providerId", "codex-cli"}, {"maxIterations", 3}, {"gitPermission", "commit"}};
		}
		std::string Create(nlohmann::json trigger, const std::string& policy = "skip", bool trusted = false)
		{
			std::string error;
			const auto created = service->CreateSchedule(app, {{"name", "Nightly triage"}, {"trusted", trusted}, {"trigger", std::move(trigger)}, {"missedPolicy", policy}, {"request", Request()}}, now, &error);
			UAM_ASSERT(created.has_value());
			return created->at("id").get<std::string>();
		}
		const uam::TimerService::Schedule& Only() const
		{
			UAM_ASSERT_EQ(service->SchedulesForTests().size(), std::size_t{1});
			return service->SchedulesForTests().front();
		}
	};
} // namespace

UAM_TEST(TimerScheduleRunsAnIntervalOncePerSlot)
{
	TimerFixture fixture;
	fixture.Create({{"kind", "interval"}, {"minutes", 60}});
	UAM_ASSERT_EQ(fixture.Only().next_run_ms, kMonday8 + kHour);
	fixture.PollAt(kMonday8 + 59 * kMinute);
	UAM_ASSERT(fixture.runs->empty());
	fixture.PollAt(kMonday8 + kHour);
	UAM_ASSERT_EQ(fixture.runs->size(), std::size_t{1});
	UAM_ASSERT(fixture.runs->front().id == fixture.Only().id && fixture.runs->front().name == "Nightly triage" && !fixture.runs->front().trusted);
	UAM_ASSERT(fixture.Only().last_result == "awaitingApproval" && fixture.Only().last_chat_id == "run-1");
	// Polling again in the same slot, or after a restart, never runs it twice.
	fixture.PollAt(kMonday8 + kHour + 2000);
	fixture.Restart();
	fixture.PollAt(kMonday8 + kHour + 4000);
	UAM_ASSERT_EQ(fixture.runs->size(), std::size_t{1});
	fixture.PollAt(kMonday8 + 2 * kHour);
	UAM_ASSERT_EQ(fixture.runs->size(), std::size_t{2});
	UAM_ASSERT(fixture.launched->empty());
}

UAM_TEST(TimerScheduleRunsAWeekdayTimeAndStartsTrustedRuns)
{
	TimerFixture fixture;
	fixture.Create({{"kind", "weekly"}, {"days", {1, 3}}, {"time", "09:00"}}, "skip", true);
	const std::int64_t monday9 = kClock.from_local(2026, 11, 23, 9, 0);
	UAM_ASSERT_EQ(fixture.Only().next_run_ms, monday9);
	fixture.PollAt(monday9 + kMinute);
	UAM_ASSERT_EQ(fixture.runs->size(), std::size_t{1});
	// A trusted run's chat is handed to the launcher, which reports back.
	UAM_ASSERT(*fixture.launched == std::vector<std::string>{"run-1"});
	UAM_ASSERT_EQ(fixture.Only().last_result, std::string("starting"));
	fixture.service->RecordLaunchResult(fixture.Only().id, "run-1", true, "");
	UAM_ASSERT_EQ(fixture.Only().last_result, std::string("started"));
	// Tuesday passes without a run; Wednesday runs.
	fixture.PollAt(monday9 + kDay);
	UAM_ASSERT_EQ(fixture.runs->size(), std::size_t{1});
	fixture.PollAt(monday9 + 2 * kDay);
	UAM_ASSERT_EQ(fixture.runs->size(), std::size_t{2});
	// A failed start is recorded and raises one warning notice naming the schedule.
	fixture.service->RecordLaunchResult(fixture.Only().id, "run-2", false, "The provider is unavailable.");
	UAM_ASSERT(fixture.Only().last_result == "failed" && fixture.Only().last_error == "The provider is unavailable.");
	const nlohmann::json notices = fixture.service->SerializeForFrontend()["notices"];
	UAM_ASSERT_EQ(notices.size(), std::size_t{1});
	UAM_ASSERT(notices[0]["tone"] == "warning" && notices[0]["message"].get<std::string>().find("Nightly triage") != std::string::npos);
	UAM_ASSERT(notices[0]["message"].get<std::string>().find('\n') == std::string::npos);
	// An outcome for an earlier run's chat changes nothing.
	fixture.service->RecordLaunchResult(fixture.Only().id, "run-1", true, "");
	UAM_ASSERT_EQ(fixture.Only().last_result, std::string("failed"));
}

UAM_TEST(TimerSchedulePausedEditedAndDeletedSchedulesDoNotRun)
{
	TimerFixture fixture;
	const std::string id = fixture.Create({{"kind", "interval"}, {"minutes", 60}});
	std::string error;
	UAM_ASSERT(fixture.service->SetSchedulePaused(id, true, fixture.now, &error).has_value());
	fixture.PollAt(kMonday8 + 5 * kHour);
	UAM_ASSERT(fixture.runs->empty());
	// Resuming starts the timing again: the hours that passed while paused are not run.
	UAM_ASSERT(fixture.service->SetSchedulePaused(id, false, fixture.now, &error).has_value());
	fixture.PollAt(fixture.now + kMinute);
	UAM_ASSERT(fixture.runs->empty());
	UAM_ASSERT_EQ(fixture.Only().next_run_ms, kMonday8 + 6 * kHour);
	// An edit that keeps the trigger keeps the timing.
	nlohmann::json renamed = {{"name", "Renamed"}, {"trigger", {{"kind", "interval"}, {"minutes", 60}}}, {"missedPolicy", "skip"}, {"request", TimerFixture::Request()}};
	const std::int64_t kept = fixture.Only().next_run_ms;
	UAM_ASSERT(fixture.service->UpdateSchedule(fixture.app, id, renamed, fixture.now + 10 * kMinute, &error).has_value());
	UAM_ASSERT_EQ(fixture.Only().next_run_ms, kept);
	// An edit that changes the trigger restarts the timing from the edit.
	nlohmann::json edited = {{"name", "Morning triage"}, {"trigger", {{"kind", "interval"}, {"minutes", 30}}}, {"missedPolicy", "runOnce"}, {"request", TimerFixture::Request()}};
	edited["request"]["prompt"] = "Summarise this morning's failures.";
	UAM_ASSERT(fixture.service->UpdateSchedule(fixture.app, id, edited, fixture.now, &error).has_value());
	UAM_ASSERT(fixture.Only().name == "Morning triage" && fixture.Only().request["prompt"] == "Summarise this morning's failures.");
	UAM_ASSERT_EQ(fixture.Only().next_run_ms, fixture.now + 30 * kMinute);
	// Invalid edits are refused with one line and change nothing.
	edited["request"]["workspaceId"] = "missing";
	UAM_ASSERT(!fixture.service->UpdateSchedule(fixture.app, id, edited, fixture.now, &error).has_value());
	UAM_ASSERT_EQ(error, std::string("That workspace no longer exists."));
	edited["request"]["workspaceId"] = "folder-a";
	edited["request"]["providerId"] = "antigravity-cli";
	UAM_ASSERT(!fixture.service->UpdateSchedule(fixture.app, id, edited, fixture.now, &error).has_value());
	UAM_ASSERT(error.find("permission prompts") != std::string::npos);
	UAM_ASSERT_EQ(fixture.Only().name, std::string("Morning triage"));
	UAM_ASSERT(fixture.service->DeleteSchedule(id, &error));
	fixture.PollAt(fixture.now + 2 * kHour);
	fixture.Restart();
	fixture.PollAt(fixture.now + 2 * kHour);
	UAM_ASSERT(fixture.runs->empty());
	UAM_ASSERT(fixture.service->SchedulesForTests().empty());
}

UAM_TEST(TimerScheduleMissedRunsAfterRestartFollowThePolicy)
{
	for (const std::string policy : {"skip", "runOnce"})
	{
		TimerFixture fixture;
		fixture.Create({{"kind", "interval"}, {"minutes", 60}}, policy);
		// UAM is closed from 08:00 until 11:30: the 09:00, 10:00 and 11:00 slots pass.
		fixture.Restart();
		fixture.PollAt(kMonday8 + 3 * kHour + 30 * kMinute);
		fixture.PollAt(fixture.now + 2000);
		UAM_ASSERT_EQ(fixture.runs->size(), std::size_t{policy == "skip" ? 0u : 1u});
		UAM_ASSERT_EQ(fixture.Only().next_run_ms, kMonday8 + 4 * kHour);
		UAM_ASSERT_EQ(fixture.Only().last_skipped, policy == "skip" ? 3 : 2);
		fixture.Restart();
		fixture.PollAt(fixture.now + 4000);
		UAM_ASSERT_EQ(fixture.runs->size(), std::size_t{policy == "skip" ? 0u : 1u});
	}
}

UAM_TEST(TimerScheduleAStopBetweenRecordingAndStartingNeverRunsTheSlotAgain)
{
	TimerFixture fixture;
	fixture.Create({{"kind", "interval"}, {"minutes", 60}}, "runOnce");
	*fixture.throw_on_run = 1;
	bool stopped = false;
	try { fixture.PollAt(kMonday8 + kHour); }
	catch (const std::runtime_error&) { stopped = true; }
	UAM_ASSERT(stopped);
	// The restarted app sees the slot as already run, even under run-once, and says no chat was created.
	fixture.Restart();
	UAM_ASSERT(fixture.Only().last_result == "failed" && fixture.Only().last_error == "UAM stopped before the chat was created.");
	fixture.PollAt(kMonday8 + kHour + 5000);
	UAM_ASSERT(fixture.runs->empty());
	fixture.PollAt(kMonday8 + 2 * kHour);
	UAM_ASSERT_EQ(fixture.runs->size(), std::size_t{1});
}

UAM_TEST(TimerScheduleRefusedRunRecordsOneFailureAndCreatesNoChat)
{
	// The real creator: the same checks as POST /automation/chats, under the schedule's own credential.
	TimerFixture fixture(false);
	fixture.app.chats.clear();
	const std::string id = fixture.Create({{"kind", "interval"}, {"minutes", 60}});
	// The workspace is removed after the schedule was saved.
	fixture.app.folders.clear();
	fixture.PollAt(kMonday8 + kHour);
	UAM_ASSERT(fixture.app.chats.empty());
	UAM_ASSERT(fixture.Only().last_result == "failed" && fixture.Only().last_error == "That workspace no longer exists.");
	fixture.PollAt(kMonday8 + 2 * kHour);
	// The same reason is not announced again on every run.
	UAM_ASSERT_EQ(fixture.service->SerializeForFrontend()["notices"].size(), std::size_t{1});
	UAM_ASSERT(fixture.app.chats.empty());
	(void)id;
}

UAM_TEST(TimerScheduleRunCreatesAWaitingChatUnderItsOwnCredential)
{
	TimerFixture fixture(false);
	fixture.app.chats.clear();
	const std::string id = fixture.Create({{"kind", "interval"}, {"minutes", 60}});
	fixture.PollAt(kMonday8 + kHour);
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{1});
	const ChatSession& chat = fixture.app.chats.front();
	// The schedule is the credential: it never expires and is not an external token.
	UAM_ASSERT(chat.automation_token_id == id && chat.automation_token_name == "Nightly triage");
	UAM_ASSERT(chat.AwaitsAutomationApproval() && chat.automation_pending_prompt == "Summarise yesterday's failures.");
	UAM_ASSERT(chat.automation_max_iterations == 3 && chat.automation_git_permission == "commit");
	UAM_ASSERT(fixture.Only().last_result == "awaitingApproval" && fixture.Only().last_chat_id == chat.id);
	// No external token can read it.
	UAM_ASSERT(!fs::exists(fixture.app.data_root / "automation_tokens.json"));
}

UAM_TEST(TimerScheduleUnreadableFileIsKeptAndNothingRuns)
{
	TimerFixture fixture;
	fixture.Create({{"kind", "interval"}, {"minutes", 60}});
	const fs::path file = fixture.app.data_root / "automation_schedules.json";
	UAM_ASSERT(uam::io::WriteTextFile(file, "{not json"));
	fixture.Restart();
	std::string error;
	UAM_ASSERT(!fixture.service->ListSchedules(&error).has_value() && !error.empty());
	UAM_ASSERT(!fixture.service->CreateSchedule(fixture.app, {{"name", "x"}}, fixture.now, &error).has_value());
	fixture.PollAt(kMonday8 + 3 * kHour);
	UAM_ASSERT(fixture.runs->empty());
	std::string text;
	UAM_ASSERT(uam::io::TryReadTextFile(file, text) && text == "{not json");
}

namespace
{
	/// Saving fails while a directory stands where the file is written; removing it lets saves succeed again.
	void BlockSaves(const fs::path& file)
	{
		fs::remove(file);
		fs::create_directories(file / "blocked");
	}
	void UnblockSaves(const fs::path& file)
	{
		fs::remove_all(file);
	}
} // namespace

UAM_TEST(TimerScheduleSaveFailuresStartNothingAndKeepTheSchedule)
{
	TimerFixture fixture;
	const std::string id = fixture.Create({{"kind", "interval"}, {"minutes", 60}});
	const fs::path file = fixture.app.data_root / "automation_schedules.json";
	BlockSaves(file);
	// The run cannot be recorded first, so it does not start; the slot stays due.
	fixture.PollAt(kMonday8 + kHour);
	UAM_ASSERT(fixture.runs->empty());
	UAM_ASSERT_EQ(fixture.Only().next_run_ms, kMonday8 + kHour);
	std::string error;
	nlohmann::json edited = {{"name", "Edited"}, {"trigger", {{"kind", "interval"}, {"minutes", 30}}}, {"missedPolicy", "skip"}, {"request", TimerFixture::Request()}};
	UAM_ASSERT(!fixture.service->UpdateSchedule(fixture.app, id, edited, fixture.now, &error).has_value());
	UAM_ASSERT(fixture.Only().name == "Nightly triage" && fixture.Only().trigger.interval_minutes == 60);
	UAM_ASSERT(!fixture.service->SetSchedulePaused(id, true, fixture.now, &error).has_value() && !fixture.Only().paused);
	UAM_ASSERT(!fixture.service->DeleteSchedule(id, &error));
	UAM_ASSERT_EQ(fixture.service->SchedulesForTests().size(), std::size_t{1});
	UnblockSaves(file);
	fixture.PollAt(kMonday8 + kHour + 2000);
	UAM_ASSERT_EQ(fixture.runs->size(), std::size_t{1});
	fixture.Restart();
	fixture.PollAt(kMonday8 + kHour + 4000);
	UAM_ASSERT_EQ(fixture.runs->size(), std::size_t{1});
}

UAM_TEST(TimerScheduleWeekdayRunOnceAfterRestart)
{
	TimerFixture fixture;
	fixture.Create({{"kind", "weekly"}, {"days", {1, 3}}, {"time", "09:00"}}, "runOnce");
	// UAM is closed from Monday 08:00 until Wednesday 10:00: Monday's and Wednesday's runs pass.
	fixture.Restart();
	fixture.PollAt(kClock.from_local(2026, 11, 25, 10, 0));
	fixture.PollAt(fixture.now + 2000);
	UAM_ASSERT_EQ(fixture.runs->size(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.Only().last_skipped, 1);
	UAM_ASSERT_EQ(fixture.Only().next_run_ms, kClock.from_local(2026, 11, 30, 9, 0));
}

UAM_TEST(TimerClockSetBackDoesNotSilenceTimers)
{
	TimerFixture fixture;
	fixture.Create({{"kind", "interval"}, {"minutes", 60}});
	std::string error;
	UAM_ASSERT(fixture.service->ScheduleWake(fixture.app, "chat-1", 30, "Check CI.", fixture.now, &error).has_value());
	fixture.PollAt(kMonday8 + 10 * kMinute);
	// The clock jumps back two days: the schedule and the follow-up are each due within one period of the new time.
	fixture.PollAt(kMonday8 - 2 * kDay);
	UAM_ASSERT_EQ(fixture.Only().next_run_ms, kMonday8 - 2 * kDay + kHour);
	UAM_ASSERT_EQ(fixture.service->WakesForChat("chat-1").front().due_ms, kMonday8 - 2 * kDay + kDay);
	fixture.PollAt(kMonday8 - 2 * kDay + kHour);
	UAM_ASSERT_EQ(fixture.runs->size(), std::size_t{1});
	fixture.PollAt(kMonday8 - kDay);
	UAM_ASSERT_EQ(fixture.wakes->size(), std::size_t{1});
}

UAM_TEST(TimerScheduleTrustedChatLeftUnstartedByAStopWaitsForTheUser)
{
	TimerFixture fixture(false);
	fixture.app.chats.clear();
	fixture.Create({{"kind", "interval"}, {"minutes", 60}}, "skip", true);
	fixture.PollAt(kMonday8 + kHour);
	// The launcher was handed the chat, but UAM stops before it starts.
	UAM_ASSERT_EQ(fixture.launched->size(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.Only().last_result, std::string("starting"));
	const std::string chat_id = fixture.app.chats.front().id;
	fixture.app.chats_with_unseen_updates.clear();
	fixture.Restart();
	UAM_ASSERT_EQ(fixture.Only().last_result, std::string("awaitingApproval"));
	fixture.PollAt(kMonday8 + kHour + 2000);
	UAM_ASSERT(fixture.app.chats_with_unseen_updates.contains(chat_id));
	UAM_ASSERT(fixture.app.chats.front().AwaitsAutomationApproval());
}

UAM_TEST(TimerWakeSaveFailureSendsNothing)
{
	TimerFixture fixture;
	std::string error;
	UAM_ASSERT(fixture.service->ScheduleWake(fixture.app, "chat-1", 1, "Check CI.", fixture.now, &error).has_value());
	const fs::path file = fixture.app.data_root / "chat_wakes.json";
	BlockSaves(file);
	fixture.PollAt(kMonday8 + 2 * kMinute);
	UAM_ASSERT(fixture.wakes->empty());
	UAM_ASSERT(!fixture.service->ScheduleWake(fixture.app, "chat-1", 5, "Another.", fixture.now, &error).has_value());
	UAM_ASSERT_EQ(error, std::string("The follow-up could not be saved. Try again."));
	UnblockSaves(file);
	fixture.PollAt(kMonday8 + 2 * kMinute + 2000);
	UAM_ASSERT_EQ(fixture.wakes->size(), std::size_t{1});
}

UAM_TEST(TimerWakeFiresOnceAndSurvivesRestart)
{
	TimerFixture fixture;
	std::string error;
	const auto wake = fixture.service->ScheduleWake(fixture.app, "chat-1", 30, "Check whether CI passed on the parser branch.", fixture.now, &error);
	UAM_ASSERT(wake.has_value() && wake->due_ms == kMonday8 + 30 * kMinute);
	fixture.PollAt(kMonday8 + 29 * kMinute);
	UAM_ASSERT(fixture.wakes->empty());
	fixture.Restart();
	UAM_ASSERT_EQ(fixture.service->WakesForChat("chat-1").size(), std::size_t{1});
	fixture.PollAt(kMonday8 + 31 * kMinute);
	UAM_ASSERT_EQ(fixture.wakes->size(), std::size_t{1});
	UAM_ASSERT(fixture.wakes->front().first == "chat-1");
	// The message says how long ago it was really set, here one minute past its due time.
	UAM_ASSERT(fixture.wakes->front().second.find("31 minutes ago") != std::string::npos);
	UAM_ASSERT(fixture.wakes->front().second.find("Check whether CI passed on the parser branch.") != std::string::npos);
	fixture.PollAt(kMonday8 + 40 * kMinute);
	fixture.Restart();
	fixture.PollAt(kMonday8 + 50 * kMinute);
	UAM_ASSERT_EQ(fixture.wakes->size(), std::size_t{1});
}

UAM_TEST(TimerWakeDueWhileClosedFiresOnceAfterRestart)
{
	TimerFixture fixture;
	std::string error;
	UAM_ASSERT(fixture.service->ScheduleWake(fixture.app, "chat-1", 10, "Look again.", fixture.now, &error).has_value());
	// UAM is closed for two hours.
	fixture.Restart();
	fixture.PollAt(kMonday8 + 2 * kHour);
	fixture.PollAt(fixture.now + 2000);
	UAM_ASSERT_EQ(fixture.wakes->size(), std::size_t{1});
}

UAM_TEST(TimerWakeStopDoneAndDeleteCancelIt)
{
	std::string error;
	{
		TimerFixture fixture;
		UAM_ASSERT(fixture.service->ScheduleWake(fixture.app, "chat-1", 5, "Stop me.", fixture.now, &error).has_value());
		UAM_ASSERT(fixture.service->CancelWakesForChat("chat-1"));
		fixture.Restart();
		fixture.PollAt(kMonday8 + kHour);
		UAM_ASSERT(fixture.wakes->empty());
	}
	{
		TimerFixture fixture;
		UAM_ASSERT(fixture.service->ScheduleWake(fixture.app, "chat-1", 5, "Done.", fixture.now, &error).has_value());
		fixture.app.chats[0].settled_at = "2026-11-23 08:01:00";
		fixture.PollAt(kMonday8 + kHour);
		UAM_ASSERT(fixture.wakes->empty() && fixture.service->WakesForChat("chat-1").empty());
	}
	{
		TimerFixture fixture;
		fixture.AddChat("chat-2");
		UAM_ASSERT(fixture.service->ScheduleWake(fixture.app, "chat-1", 5, "Deleted.", fixture.now, &error).has_value());
		fixture.app.chats.erase(fixture.app.chats.begin());
		fixture.PollAt(kMonday8 + kHour);
		UAM_ASSERT(fixture.wakes->empty() && fixture.service->WakesForChat("chat-1").empty());
	}
	// A chat that was already done when the wake was set keeps it.
	{
		TimerFixture fixture;
		fixture.app.chats[0].settled_at = "2026-11-23 07:00:00";
		UAM_ASSERT(fixture.service->ScheduleWake(fixture.app, "chat-1", 5, "Still wanted.", fixture.now, &error).has_value());
		fixture.PollAt(kMonday8 + kHour);
		UAM_ASSERT_EQ(fixture.wakes->size(), std::size_t{1});
	}
}

UAM_TEST(TimerWakeLimitsBoundsAndRefusals)
{
	TimerFixture fixture;
	std::string error;
	for (int index = 0; index < 3; ++index)
		UAM_ASSERT(fixture.service->ScheduleWake(fixture.app, "chat-1", 60 + index, "Again.", fixture.now, &error).has_value());
	UAM_ASSERT(!fixture.service->ScheduleWake(fixture.app, "chat-1", 60, "One too many.", fixture.now, &error).has_value());
	UAM_ASSERT(error.find("3 follow-ups") != std::string::npos);
	UAM_ASSERT(!fixture.service->ScheduleWake(fixture.app, "chat-1", 0, "Too soon.", fixture.now, &error).has_value());
	UAM_ASSERT(!fixture.service->ScheduleWake(fixture.app, "chat-1", 1441, "Too late.", fixture.now, &error).has_value());
	UAM_ASSERT(!fixture.service->ScheduleWake(fixture.app, "chat-1", 10, "   ", fixture.now, &error).has_value());
	// Subagents and side chats have no later turn to wake.
	fixture.AddChat("side");
	fixture.app.chats.back().temporary_parent_chat_id = "chat-1";
	UAM_ASSERT(!fixture.service->ScheduleWake(fixture.app, "side", 10, "Side.", fixture.now, &error).has_value());
	UAM_ASSERT(error.find("Subagents") != std::string::npos);

	// A chat that refuses the wake is retried a minute later, not on every poll.
	UAM_ASSERT(fixture.service->CancelWakesForChat("chat-1"));
	UAM_ASSERT(fixture.service->ScheduleWake(fixture.app, "chat-1", 1, "Retry me.", fixture.now, &error).has_value());
	*fixture.accept = false;
	fixture.PollAt(kMonday8 + kMinute);
	fixture.PollAt(kMonday8 + kMinute + 30000);
	*fixture.accept = true;
	fixture.PollAt(kMonday8 + kMinute + 40000);
	UAM_ASSERT(fixture.wakes->empty());
	fixture.PollAt(kMonday8 + 2 * kMinute);
	UAM_ASSERT_EQ(fixture.wakes->size(), std::size_t{1});

	// One that keeps refusing is dropped with one warning notice for the chat.
	UAM_ASSERT(fixture.service->ScheduleWake(fixture.app, "chat-1", 1, "Never accepted.", fixture.now, &error).has_value());
	*fixture.accept = false;
	for (int attempt = 0; attempt <= uam::TimerService::kWakeRefusalLimit; ++attempt) fixture.PollAt(fixture.now + kMinute);
	UAM_ASSERT(fixture.service->WakesForChat("chat-1").empty());
	const nlohmann::json notices = fixture.service->SerializeForFrontend()["notices"];
	UAM_ASSERT(notices.size() == 1 && notices[0]["chatId"] == "chat-1" && notices[0]["tone"] == "warning");
}

UAM_TEST(TimerWakeInterruptedDeliveryIsNotSentAgain)
{
	TimerFixture fixture;
	const fs::path file = fixture.app.data_root / "chat_wakes.json";
	UAM_ASSERT(uam::io::WriteTextFile(file, nlohmann::json{{"version", 1}, {"wakes", {{{"id", "wk_1"}, {"chatId", "chat-1"}, {"note", "Maybe sent."},
	    {"createdMs", kMonday8}, {"dueMs", kMonday8 + kMinute}, {"settledSeen", ""}, {"delivering", true}}}}}.dump()));
	fixture.Restart();
	fixture.PollAt(kMonday8 + kHour);
	UAM_ASSERT(fixture.wakes->empty());
	UAM_ASSERT(fixture.service->WakesForChat("chat-1").empty());
}

namespace
{
	/// A chat with a live structured session and one root and one subagent UAM control capability.
	struct ToolFixture : TimerFixture
	{
		ToolFixture()
		{
			app.chats[0].uam_control_enabled = true;
			app.chats[0].provider_id = "codex-cli";
			app.chats[0].created_at = "2026-11-01 10:00:00";
			app.timer_service = std::make_shared<uam::TimerService>(app.data_root, [](uam::AppState&, const std::string&, const std::string&, std::string*) { return true; });
			Issue("capability", {});
			Issue("child-capability", "run-1");
			Issue("reviewer-capability", {});
			app.uam_control_capabilities.back().read_only_at_launch = true;
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
			session->active_uam_agent_workspace_access = "write";
			app.acp_sessions.push_back(std::move(session));
		}
		nlohmann::json Call(const std::string& request_id, const std::string& method, nlohmann::json arguments, const std::string& capability = "capability")
		{
			return uam::UamControlService::HandleRequestForTests(app, capability, {{"requestId", request_id}, {"method", method}, {"arguments", std::move(arguments)}},
			    uam::time::SystemEpochMillisecondsNow());
		}
	};
} // namespace

UAM_TEST(UamControlScheduleToolWakesOnlyTheCallingChat)
{
	ToolFixture fixture;
	nlohmann::json response = fixture.Call("schedule-1", "schedule", {{"minutes", 30}, {"note", "Check CI."}});
	UAM_ASSERT(response.value("ok", false));
	UAM_ASSERT(response["result"]["scheduled"] == true && response["result"]["minutes"] == 30 && response["result"]["pendingForThisChat"] == 1);
	UAM_ASSERT(response["result"]["followUpId"].get<std::string>().starts_with("wk_"));
	UAM_ASSERT_EQ(fixture.app.timer_service->WakesForChat("chat-1").size(), std::size_t{1});
	// A subagent cannot schedule; the chat that delegated to it can.
	response = fixture.Call("schedule-2", "schedule", {{"minutes", 30}, {"note", "Child."}}, "child-capability");
	UAM_ASSERT(!response.value("ok", false) && response.value("error", "").find("Subagents") != std::string::npos);
	// Out-of-range delays and malformed arguments are refused without a wake.
	int bad_index = 0;
	for (const char* bad : {R"({"minutes":0,"note":"x"})", R"({"minutes":1441,"note":"x"})", R"({"minutes":"30","note":"x"})", R"({"minutes":30})",
	         R"({"minutes":30,"note":"x","extra":1})", R"({"minutes":30,"note":""})"})
		UAM_ASSERT(!fixture.Call("bad-" + std::to_string(++bad_index), "schedule", nlohmann::json::parse(bad)).value("ok", false));
	UAM_ASSERT_EQ(fixture.app.timer_service->WakesForChat("chat-1").size(), std::size_t{1});

	// A goal reviewer is refused by the server even if a provider calls the hidden tool.
	UAM_ASSERT(!fixture.Call("schedule-reviewer", "schedule", {{"minutes", 30}, {"note", "Reviewer."}}, "reviewer-capability").value("ok", false));
	UAM_ASSERT(!fixture.Call("notify-reviewer", "notify", {{"message", "Reviewer."}}, "reviewer-capability").value("ok", false));
	UAM_ASSERT_EQ(fixture.app.timer_service->WakesForChat("chat-1").size(), std::size_t{1});

	const nlohmann::json tools = uam::UamControlService::ToolDefinitionsForTests();
	const auto tool = std::ranges::find_if(tools, [](const nlohmann::json& candidate) { return candidate.value("name", "") == "schedule"; });
	UAM_ASSERT(tool != tools.end() && !tool->contains("annotations"));
	UAM_ASSERT((*tool)["inputSchema"]["required"] == nlohmann::json::array({"minutes", "note"}));
}

UAM_TEST(UamControlNotifyToolRaisesTheChatAttentionFlagAtMostOnceAMinute)
{
	ToolFixture fixture;
	const std::string before = fixture.app.chats[0].attention_revision;
	fixture.app.chats[0].snoozed_until_ms = uam::time::SystemEpochMillisecondsNow() + kHour;
	nlohmann::json response = fixture.Call("notify-1", "notify", {{"message", "The release build is ready for you."}});
	UAM_ASSERT(response.value("ok", false));
	UAM_ASSERT(response["result"]["notified"] == true && response["result"]["delivery"] == "attention");
	// It follows the ordinary attention rules: a new attention revision, unseen and unsnoozed.
	UAM_ASSERT(fixture.app.chats[0].attention_revision != before);
	UAM_ASSERT(fixture.app.chats_with_unseen_updates.contains("chat-1"));
	UAM_ASSERT_EQ(fixture.app.chats[0].snoozed_until_ms, std::int64_t{0});
	response = fixture.Call("notify-2", "notify", {{"message", "Again."}});
	UAM_ASSERT(!response.value("ok", false) && response.value("error", "").find("last minute") != std::string::npos);
	response = fixture.Call("notify-3", "notify", {{"message", "Child."}}, "child-capability");
	UAM_ASSERT(!response.value("ok", false) && response.value("error", "").find("Subagents") != std::string::npos);
	UAM_ASSERT(!fixture.Call("notify-4", "notify", {{"message", std::string(301, 'a')}}).value("ok", false));
}
