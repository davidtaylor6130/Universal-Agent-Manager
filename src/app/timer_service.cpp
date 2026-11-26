#include "app/timer_service.h"

#include "app/chat_domain_service.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/state/app_state.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/io_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <algorithm>
#include <iomanip>
#include <random>
#include <set>
#include <sstream>

namespace uam
{
	namespace
	{
		constexpr std::size_t kMaxWakes = 200;
		constexpr std::size_t kMaxNotices = 20;
		constexpr std::size_t kMaxScheduleNameChars = 80;
		constexpr std::size_t kMaxFileBytes = 4 * 1024 * 1024;
		constexpr std::int64_t kWakeRetryMs = 60 * 1000;

		bool SendWake(AppState& app, const std::string& chat_id, const std::string& text, std::string* error)
		{
			return uam::SendAcpPrompt(app, chat_id, text, error);
		}

		std::string RandomId(std::string_view prefix)
		{
			std::random_device random;
			std::ostringstream out;
			out << prefix << std::hex << std::setw(8) << std::setfill('0') << static_cast<std::uint32_t>(random()) << std::setw(8) << static_cast<std::uint32_t>(random());
			return out.str();
		}

		template <typename Value> Value Field(const nlohmann::json& object, const char* key, Value fallback)
		{
			const auto found = object.find(key);
			if (found == object.end()) return fallback;
			try { return found->get<Value>(); }
			catch (const nlohmann::json::exception&) { return fallback; }
		}

		std::optional<nlohmann::json> ReadJsonFile(const std::filesystem::path& path, bool& unreadable)
		{
			unreadable = false;
			std::error_code error;
			if (!std::filesystem::exists(path, error))
			{
				unreadable = static_cast<bool>(error);
				return std::nullopt;
			}
			std::string text;
			const auto size = std::filesystem::file_size(path, error);
			if (error || size > kMaxFileBytes || !uam::io::TryReadTextFile(path, text))
			{
				unreadable = true;
				return std::nullopt;
			}
			nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
			if (!root.is_object())
			{
				unreadable = true;
				return std::nullopt;
			}
			return root;
		}

		bool WriteJsonFile(const std::filesystem::path& path, const nlohmann::json& root)
		{
			if (!uam::io::AtomicWriteFile(path, root.dump(1, '\t', false, nlohmann::json::error_handler_t::replace))) return false;
			std::error_code error;
			std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, error);
			return true;
		}

		std::string DurationText(std::int64_t minutes)
		{
			if (minutes % 60 == 0 && minutes >= 60) return std::to_string(minutes / 60) + (minutes == 60 ? " hour" : " hours");
			return std::to_string(minutes) + (minutes == 1 ? " minute" : " minutes");
		}

		bool ChatCannotBeWoken(const ChatSession& chat)
		{
			return !chat.agent_run_id.empty() || !chat.subagent_parent_chat_id.empty() || !chat.temporary_parent_chat_id.empty();
		}

		/// Only the fields of POST /automation/chats are kept, so a stored request never carries anything else.
		nlohmann::json RequestFields(const nlohmann::json& request)
		{
			nlohmann::json kept = nlohmann::json::object();
			if (!request.is_object()) return kept;
			for (const char* key : {"prompt", "workspaceId", "providerId", "maxIterations", "gitPermission"})
				if (const auto found = request.find(key); found != request.end()) kept[key] = *found;
			// The optional fields are left out when blank, as an API caller would.
			for (const char* key : {"modelId", "title"})
				if (const auto found = request.find(key); found != request.end() && !(found->is_string() && uam::strings::IsBlank(found->get_ref<const std::string&>())))
					kept[key] = *found;
			return kept;
		}
	} // namespace

	TimerService::TimerService(std::filesystem::path data_root, WakeSender sender, ChatCreator creator, scheduling::LocalClock clock)
	    : m_wakes_file(data_root / "chat_wakes.json"), m_schedules_file(data_root / "automation_schedules.json"),
	      m_sender(sender ? std::move(sender) : WakeSender(SendWake)),
	      m_creator(creator ? std::move(creator) : ChatCreator(AutomationApiService::CreateScheduledChat)),
	      m_clock(clock.to_local && clock.from_local ? std::move(clock) : scheduling::SystemLocalClock())
	{
		LoadWakes();
		LoadSchedules();
	}

	void TimerService::LoadWakes()
	{
		bool unreadable = false;
		const std::optional<nlohmann::json> root = ReadJsonFile(m_wakes_file, unreadable);
		if (unreadable) uam::diagnostics::Write("Timers: the saved follow-up list could not be read; starting without follow-ups.");
		if (!root || !root->contains("wakes") || !(*root)["wakes"].is_array()) return;
		std::size_t dropped = 0;
		for (const nlohmann::json& item : (*root)["wakes"])
		{
			if (!item.is_object() || m_wakes.size() >= kMaxWakes) continue;
			ChatWake wake{.id = Field<std::string>(item, "id", {}), .chat_id = Field<std::string>(item, "chatId", {}), .note = Field<std::string>(item, "note", {}),
			    .created_ms = Field<std::int64_t>(item, "createdMs", 0), .due_ms = Field<std::int64_t>(item, "dueMs", 0),
			    .settled_seen = Field<std::string>(item, "settledSeen", {}), .delivering = Field(item, "delivering", false)};
			if (wake.id.empty() || wake.chat_id.empty() || wake.due_ms <= 0) continue;
			// The app stopped during this wake's delivery: it may have been sent, so it is not sent again.
			if (wake.delivering)
			{
				++dropped;
				continue;
			}
			m_wakes.push_back(std::move(wake));
		}
		if (dropped > 0) uam::diagnostics::Write("Timers: " + std::to_string(dropped) + " follow-up(s) were being delivered when UAM stopped and were not sent again.");
	}

	void TimerService::LoadSchedules()
	{
		bool unreadable = false;
		const std::optional<nlohmann::json> root = ReadJsonFile(m_schedules_file, unreadable);
		if (!unreadable && root && (!root->contains("schedules") || !(*root)["schedules"].is_array())) unreadable = true;
		if (unreadable)
		{
			// The file is kept as it is: nothing runs and nothing overwrites it until it can be read.
			m_schedules_unreadable = true;
			uam::diagnostics::Write("Timers: the automation schedule file could not be read; schedules are paused until it can be.");
			return;
		}
		if (!root) return;
		for (const nlohmann::json& item : (*root)["schedules"])
		{
			if (!item.is_object()) continue;
			const std::optional<scheduling::Trigger> trigger = scheduling::ParseTrigger(item.value("trigger", nlohmann::json()));
			const std::optional<scheduling::MissedPolicy> policy = scheduling::ParseMissedPolicy(Field<std::string>(item, "missedPolicy", {}));
			Schedule schedule{.id = Field<std::string>(item, "id", {}), .name = Field<std::string>(item, "name", {}), .paused = Field(item, "paused", false),
			    .trusted = Field(item, "trusted", false), .trigger = trigger.value_or(scheduling::Trigger{}),
			    .missed_policy = policy.value_or(scheduling::MissedPolicy::Skip), .request = RequestFields(item.value("request", nlohmann::json::object())),
			    .created_at = Field<std::string>(item, "createdAt", {}), .anchor_ms = Field<std::int64_t>(item, "anchorMs", 0),
			    .next_run_ms = Field<std::int64_t>(item, "nextRunMs", 0), .last_run_ms = Field<std::int64_t>(item, "lastRunMs", 0),
			    .last_result = Field<std::string>(item, "lastResult", {}), .last_error = Field<std::string>(item, "lastError", {}),
			    .last_chat_id = Field<std::string>(item, "lastChatId", {}), .last_skipped = Field(item, "lastSkipped", 0)};
			if (schedule.id.empty() || !trigger || !policy || m_schedules.size() >= kMaxSchedules)
			{
				m_schedules_unreadable = true;
				uam::diagnostics::Write("Timers: a saved automation schedule is not valid; schedules are paused until the file is fixed.");
				m_schedules.clear();
				return;
			}
			// A run the app did not see finish is resolved against the chats on the first poll.
			if (schedule.last_result == "starting") m_unstarted_after_restart.push_back(schedule.id);
			m_schedules.push_back(std::move(schedule));
		}
	}

	bool TimerService::SaveWakes()
	{
		nlohmann::json wakes = nlohmann::json::array();
		for (const ChatWake& wake : m_wakes)
			wakes.push_back({{"id", wake.id}, {"chatId", wake.chat_id}, {"note", wake.note}, {"createdMs", wake.created_ms}, {"dueMs", wake.due_ms},
			    {"settledSeen", wake.settled_seen}, {"delivering", wake.delivering}});
		if (!WriteJsonFile(m_wakes_file, {{"version", 1}, {"wakes", std::move(wakes)}}))
		{
			if (!m_wakes_save_failed) uam::diagnostics::Write("Timers: the follow-up list could not be saved; it is retried on the next change.");
			m_wakes_save_failed = true;
			return false;
		}
		m_wakes_save_failed = false;
		return true;
	}

	nlohmann::json TimerService::ScheduleJson(const Schedule& schedule)
	{
		return {{"id", schedule.id}, {"name", schedule.name}, {"paused", schedule.paused}, {"trusted", schedule.trusted},
		    {"trigger", scheduling::TriggerJson(schedule.trigger)}, {"missedPolicy", scheduling::MissedPolicyName(schedule.missed_policy)},
		    {"request", schedule.request}, {"createdAt", schedule.created_at}, {"anchorMs", schedule.anchor_ms}, {"nextRunMs", schedule.next_run_ms},
		    {"lastRunMs", schedule.last_run_ms}, {"lastResult", schedule.last_result}, {"lastError", schedule.last_error},
		    {"lastChatId", schedule.last_chat_id}, {"lastSkipped", schedule.last_skipped}};
	}

	bool TimerService::SaveSchedules()
	{
		if (m_schedules_unreadable) return false;
		nlohmann::json schedules = nlohmann::json::array();
		for (const Schedule& schedule : m_schedules) schedules.push_back(ScheduleJson(schedule));
		if (!WriteJsonFile(m_schedules_file, {{"version", 1}, {"schedules", std::move(schedules)}}))
		{
			if (!m_schedules_save_failed) uam::diagnostics::Write("Timers: the automation schedule file could not be saved; no run starts until it can be.");
			m_schedules_save_failed = true;
			return false;
		}
		m_schedules_save_failed = false;
		return true;
	}

	void TimerService::Notify(std::string title, std::string message, std::string tone, std::string chat_id)
	{
		m_notices.push_back({.serial = ++m_notice_serial, .title = std::move(title), .message = std::move(message), .tone = std::move(tone), .chat_id = std::move(chat_id)});
		if (m_notices.size() > kMaxNotices) m_notices.erase(m_notices.begin(), m_notices.end() - kMaxNotices);
	}

	nlohmann::json TimerService::SerializeForFrontend() const
	{
		nlohmann::json notices = nlohmann::json::array();
		for (const Notice& notice : m_notices)
		{
			nlohmann::json row = {{"serial", notice.serial}, {"title", notice.title}, {"message", notice.message}, {"tone", notice.tone}};
			if (!notice.chat_id.empty()) row["chatId"] = notice.chat_id;
			notices.push_back(std::move(row));
		}
		return {{"notices", std::move(notices)}};
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Chat wakes

	std::optional<TimerService::ChatWake> TimerService::ScheduleWake(const AppState& app, std::string_view chat_id, int minutes, std::string_view note, std::int64_t now_ms, std::string* error_out)
	{
		const auto fail = [&](std::string message) -> std::optional<ChatWake>
		{
			if (error_out != nullptr) *error_out = std::move(message);
			return std::nullopt;
		};
		const ChatSession* chat = ChatDomainService().FindChatById(app, std::string(chat_id));
		if (chat == nullptr) return fail("The chat is unavailable.");
		// Their turn ends with their task, so nothing would be there to wake.
		if (ChatCannotBeWoken(*chat)) return fail("Subagents and temporary side chats cannot schedule follow-ups. Ask the chat that delegated to you to schedule it.");
		if (minutes < kMinWakeMinutes || minutes > kMaxWakeMinutes) return fail("Choose a delay from 1 minute to 24 hours (1440 minutes).");
		const std::string text = uam::strings::Trim(note);
		if (text.empty() || text.size() > kMaxWakeNoteBytes || text.find('\0') != std::string::npos) return fail("Give a note of 1 to 500 bytes saying what to do when the follow-up arrives.");
		if (WakesForChat(chat_id).size() >= kMaxWakesPerChat)
			return fail("This chat already has " + std::to_string(kMaxWakesPerChat) + " follow-ups scheduled. Wait for one to arrive, or ask the user to press Stop to clear them.");
		if (m_wakes.size() >= kMaxWakes) return fail("UAM already has " + std::to_string(kMaxWakes) + " follow-ups scheduled. Try again later.");
		m_wakes.push_back({.id = RandomId("wk_"), .chat_id = std::string(chat_id), .note = text, .created_ms = now_ms,
		    .due_ms = now_ms + static_cast<std::int64_t>(minutes) * scheduling::kMinuteMs, .settled_seen = chat->settled_at});
		if (!SaveWakes())
		{
			m_wakes.pop_back();
			return fail("The follow-up could not be saved. Try again.");
		}
		return m_wakes.back();
	}

	bool TimerService::CancelWakesForChat(std::string_view chat_id)
	{
		if (std::erase_if(m_wakes, [&](const ChatWake& wake) { return wake.chat_id == chat_id; }) == 0) return false;
		SaveWakes();
		return true;
	}

	bool TimerService::CancelWake(std::string_view wake_id)
	{
		if (std::erase_if(m_wakes, [&](const ChatWake& wake) { return wake.id == wake_id; }) == 0) return false;
		SaveWakes();
		return true;
	}

	std::vector<TimerService::ChatWake> TimerService::WakesForChat(std::string_view chat_id) const
	{
		std::vector<ChatWake> wakes;
		for (const ChatWake& wake : m_wakes)
			if (wake.chat_id == chat_id) wakes.push_back(wake);
		return wakes;
	}

	bool TimerService::PollWakes(AppState& app, std::int64_t now_ms)
	{
		bool changed = false;
		// ponytail: as in the pull request watch, an empty chat list counts as "not loaded yet" until chats were seen
		// once, so startup can never wipe the list; after that a deleted chat takes its wakes with it.
		m_chats_seen = m_chats_seen || !app.chats.empty();
		if (m_chats_seen)
		{
			const std::size_t removed = std::erase_if(m_wakes, [&](const ChatWake& wake)
			{
				const ChatSession* chat = ChatDomainService().FindChatById(app, wake.chat_id);
				// Marked done since the wake was set: the user has finished with this work.
				return chat == nullptr || (!chat->settled_at.empty() && chat->settled_at != wake.settled_seen);
			});
			if (removed > 0)
			{
				SaveWakes();
				changed = true;
			}
		}
		for (std::size_t index = 0; index < m_wakes.size();)
		{
			ChatWake& wake = m_wakes[index];
			// The clock was set back since the wake was set: it is due no later than its longest delay from now.
			const std::int64_t latest_due = now_ms + static_cast<std::int64_t>(kMaxWakeMinutes) * scheduling::kMinuteMs;
			if (wake.due_ms > latest_due)
			{
				// Keeps the delay the agent asked for, counted from the new time, and saves it so a restart keeps it too.
				wake.due_ms = now_ms + std::clamp<std::int64_t>(wake.due_ms - wake.created_ms, scheduling::kMinuteMs, latest_due - now_ms);
				wake.created_ms = now_ms;
				SaveWakes();
			}
			if (now_ms < wake.due_ms || now_ms < wake.retry_at_ms)
			{
				++index;
				continue;
			}
			// Saved first, so a crash during the send drops the wake instead of sending it a second time.
			wake.delivering = true;
			if (!SaveWakes())
			{
				wake.delivering = false;
				break;
			}
			// The time since it was set, so a follow-up delivered late (UAM closed, chat busy) says how long it really was.
			const std::string text = "UAM follow-up: the follow-up you scheduled " + DurationText(std::max<std::int64_t>(1, (now_ms - wake.created_ms) / scheduling::kMinuteMs)) +
			    " ago is due. Your note:\n\n" + wake.note;
			std::string error;
			const std::string chat_id = wake.chat_id;
			const std::string wake_id = wake.id;
			const bool sent = m_sender(app, chat_id, text, &error);
			// Found again by its ID: the list is not assumed unchanged across the send.
			const auto found = std::ranges::find(m_wakes, wake_id, &ChatWake::id);
			if (found == m_wakes.end())
			{
				SaveWakes();
				changed = true;
				continue;
			}
			index = static_cast<std::size_t>(found - m_wakes.begin());
			if (sent)
			{
				m_wakes.erase(found);
				SaveWakes();
				changed = true;
				continue;
			}
			ChatWake& refused = m_wakes[index];
			refused.delivering = false;
			refused.retry_at_ms = now_ms + kWakeRetryMs;
			// The runtime's reason can carry private text; only the chat is named, and only for the first refusal.
			if (++refused.refusals == 1) uam::diagnostics::Write("Timers: chat " + chat_id + " did not accept a follow-up; it is retried every minute.");
			if (refused.refusals >= kWakeRefusalLimit)
			{
				uam::diagnostics::Write("Timers: chat " + chat_id + " refused a follow-up " + std::to_string(kWakeRefusalLimit) + " times; it was dropped.");
				m_wakes.erase(m_wakes.begin() + static_cast<std::ptrdiff_t>(index));
				Notify("Follow-up dropped", "A scheduled follow-up was dropped because the chat did not accept it for " + std::to_string(kWakeRefusalLimit) + " minutes; open the chat in Chat view to continue.", "warning", chat_id);
				changed = true;
			}
			else ++index;
			SaveWakes();
		}
		return changed;
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Automation schedules

	TimerService::Schedule* TimerService::FindSchedule(std::string_view id)
	{
		const auto found = std::ranges::find(m_schedules, id, &Schedule::id);
		return found == m_schedules.end() ? nullptr : &*found;
	}

	std::optional<nlohmann::json> TimerService::ListSchedules(std::string* error_out) const
	{
		if (m_schedules_unreadable)
		{
			if (error_out != nullptr) *error_out = "The automation schedule file could not be read.";
			return std::nullopt;
		}
		nlohmann::json list = nlohmann::json::array();
		for (const Schedule& schedule : m_schedules) list.push_back(ScheduleJson(schedule));
		return list;
	}

	std::optional<TimerService::Schedule> TimerService::ScheduleFromSpec(const AppState& app, const nlohmann::json& spec, std::string* error_out) const
	{
		const auto fail = [&](std::string message) -> std::optional<Schedule>
		{
			if (error_out != nullptr) *error_out = std::move(message);
			return std::nullopt;
		};
		if (!spec.is_object()) return fail("The schedule is not valid.");
		Schedule schedule;
		schedule.name = uam::strings::SafeLine(Field<std::string>(spec, "name", {}), kMaxScheduleNameChars, true);
		if (schedule.name.empty()) return fail("Enter a schedule name.");
		schedule.trusted = Field(spec, "trusted", false);
		std::string error;
		const std::optional<scheduling::Trigger> trigger = scheduling::ParseTrigger(spec.value("trigger", nlohmann::json()), &error);
		if (!trigger) return fail(std::move(error));
		schedule.trigger = *trigger;
		const std::optional<scheduling::MissedPolicy> policy = scheduling::ParseMissedPolicy(Field<std::string>(spec, "missedPolicy", "skip"));
		if (!policy) return fail("Choose what happens to runs missed while UAM was closed.");
		schedule.missed_policy = *policy;
		schedule.request = RequestFields(spec.value("request", nlohmann::json::object()));
		if (std::string refusal = AutomationApiService::ScheduledRequestRefusal(app, schedule.request); !refusal.empty()) return fail(std::move(refusal));
		return schedule;
	}

	std::optional<nlohmann::json> TimerService::CreateSchedule(const AppState& app, const nlohmann::json& spec, std::int64_t now_ms, std::string* error_out)
	{
		const auto fail = [&](std::string message) -> std::optional<nlohmann::json>
		{
			if (error_out != nullptr) *error_out = std::move(message);
			return std::nullopt;
		};
		if (m_schedules_unreadable) return fail("The automation schedule file could not be read, so no schedule was created.");
		if (m_schedules.size() >= kMaxSchedules) return fail("At most " + std::to_string(kMaxSchedules) + " schedules can exist. Delete one first.");
		std::optional<Schedule> schedule = ScheduleFromSpec(app, spec, error_out);
		if (!schedule) return std::nullopt;
		schedule->id = RandomId("as_");
		schedule->created_at = uam::time::TimestampNow();
		schedule->paused = Field(spec, "paused", false);
		schedule->anchor_ms = now_ms;
		schedule->next_run_ms = scheduling::NextRunAfter(schedule->trigger, now_ms, now_ms, m_clock);
		m_schedules.push_back(*schedule);
		if (!SaveSchedules())
		{
			m_schedules.pop_back();
			return fail("The schedule could not be saved. Try again.");
		}
		uam::diagnostics::Write("Timers: created automation schedule " + schedule->id + ".");
		return ScheduleJson(*schedule);
	}

	std::optional<nlohmann::json> TimerService::UpdateSchedule(const AppState& app, std::string_view id, const nlohmann::json& spec, std::int64_t now_ms, std::string* error_out)
	{
		const auto fail = [&](std::string message) -> std::optional<nlohmann::json>
		{
			if (error_out != nullptr) *error_out = std::move(message);
			return std::nullopt;
		};
		if (m_schedules_unreadable) return fail("The automation schedule file could not be read, so the schedule was not changed.");
		Schedule* existing = FindSchedule(id);
		if (existing == nullptr) return fail("That schedule no longer exists.");
		std::optional<Schedule> edited = ScheduleFromSpec(app, spec, error_out);
		if (!edited) return std::nullopt;
		const Schedule before = *existing;
		existing->name = edited->name;
		existing->trusted = edited->trusted;
		existing->trigger = edited->trigger;
		existing->missed_policy = edited->missed_policy;
		existing->request = edited->request;
		// A changed trigger starts its timing again from now, so an edit never runs it at once for times it missed. An edit
		// that keeps the trigger keeps the schedule's timing, so saving a renamed or reworded schedule does not move it.
		if (scheduling::TriggerJson(before.trigger) != scheduling::TriggerJson(existing->trigger))
		{
			existing->anchor_ms = now_ms;
			existing->next_run_ms = scheduling::NextRunAfter(existing->trigger, now_ms, now_ms, m_clock);
		}
		if (!SaveSchedules())
		{
			*existing = before;
			return fail("The schedule could not be saved. Try again.");
		}
		return ScheduleJson(*existing);
	}

	std::optional<nlohmann::json> TimerService::SetSchedulePaused(std::string_view id, bool paused, std::int64_t now_ms, std::string* error_out)
	{
		const auto fail = [&](std::string message) -> std::optional<nlohmann::json>
		{
			if (error_out != nullptr) *error_out = std::move(message);
			return std::nullopt;
		};
		if (m_schedules_unreadable) return fail("The automation schedule file could not be read, so the schedule was not changed.");
		Schedule* schedule = FindSchedule(id);
		if (schedule == nullptr) return fail("That schedule no longer exists.");
		const Schedule before = *schedule;
		schedule->paused = paused;
		// Resuming does not run the slots that passed while paused.
		if (!paused && before.paused)
		{
			schedule->anchor_ms = now_ms;
			schedule->next_run_ms = scheduling::NextRunAfter(schedule->trigger, now_ms, now_ms, m_clock);
		}
		if (!SaveSchedules())
		{
			*schedule = before;
			return fail(paused ? "The schedule could not be paused. Try again." : "The schedule could not be resumed. Try again.");
		}
		return ScheduleJson(*schedule);
	}

	bool TimerService::DeleteSchedule(std::string_view id, std::string* error_out)
	{
		const auto fail = [&](std::string message)
		{
			if (error_out != nullptr) *error_out = std::move(message);
			return false;
		};
		if (m_schedules_unreadable) return fail("The automation schedule file could not be read, so the schedule was not deleted.");
		const auto found = std::ranges::find(m_schedules, id, &Schedule::id);
		if (found == m_schedules.end()) return fail("That schedule no longer exists.");
		const Schedule removed = *found;
		const std::ptrdiff_t position = found - m_schedules.begin();
		m_schedules.erase(found);
		if (!SaveSchedules())
		{
			m_schedules.insert(m_schedules.begin() + position, removed);
			return fail("The schedule could not be deleted and may still run. Try again.");
		}
		uam::diagnostics::Write("Timers: deleted automation schedule " + std::string(id) + ".");
		return true;
	}

	void TimerService::RecordLaunchResult(std::string_view schedule_id, std::string_view chat_id, bool started, std::string_view error)
	{
		Schedule* schedule = FindSchedule(schedule_id);
		if (schedule == nullptr || schedule->last_chat_id != chat_id) return;
		schedule->last_result = started ? "started" : "failed";
		schedule->last_error = started ? std::string{} : uam::strings::SafeLine(error, 300, true);
		if (!started) Notify("Scheduled chat did not start", "Schedule “" + schedule->name + "” had a problem starting its chat: " + schedule->last_error, "warning", std::string(chat_id));
		SaveSchedules();
	}

	void TimerService::RunSchedule(AppState& app, Schedule& schedule)
	{
		const std::string previous_error = schedule.last_error;
		const AutomationApiResponse response = m_creator(app, {.id = schedule.id, .name = schedule.name, .trusted = schedule.trusted}, schedule.request);
		if (response.status != 200)
		{
			schedule.last_result = "failed";
			schedule.last_error = uam::strings::SafeLine(response.body.value("error", std::string("The chat could not be created.")), 300, true);
			// One notice per new reason, so a schedule that keeps failing the same way does not repeat it every run.
			if (schedule.last_error != previous_error) Notify("Scheduled run did not start", "Schedule “" + schedule.name + "” did not start a chat: " + schedule.last_error, "warning");
			return;
		}
		schedule.last_error.clear();
		schedule.last_chat_id = response.body.value("chatId", std::string{});
		if (response.launch_chat_id.empty())
		{
			schedule.last_result = "awaitingApproval";
			return;
		}
		if (!m_launcher)
		{
			// No desktop bridge to start it: the chat waits for the user, as an untrusted run's does.
			ChatDomainService().MarkChatNeedsAttention(app, response.launch_chat_id);
			schedule.last_result = "awaitingApproval";
			return;
		}
		schedule.last_result = "starting";
		m_launcher(app, schedule.id, response.launch_chat_id);
	}

	bool TimerService::PollSchedules(AppState& app, std::int64_t now_ms)
	{
		if (m_schedules_unreadable) return false;
		bool changed = false;
		// Runs UAM stopped during, resolved once the chats are loaded: a chat that still waits is marked for the user (a
		// trusted run's chat was never started or marked), a running one counts as started, and anything else is a failure.
		if (!m_unstarted_after_restart.empty() && (m_chats_seen || !app.chats.empty()))
		{
			for (const std::string& id : m_unstarted_after_restart)
			{
				Schedule* schedule = FindSchedule(id);
				if (schedule == nullptr || schedule->last_result != "starting") continue;
				// The run's own chat, or (when UAM stopped before recording it) any chat of this schedule still waiting.
				bool waiting = false, exists = false;
				for (const ChatSession& chat : app.chats)
				{
					if (chat.automation_token_id != schedule->id) continue;
					const bool this_run = chat.id == schedule->last_chat_id;
					if (chat.AwaitsAutomationApproval() && (this_run || schedule->last_chat_id.empty()))
					{
						ChatDomainService().MarkChatNeedsAttention(app, chat.id);
						waiting = true;
					}
					exists = exists || this_run;
				}
				if (waiting) schedule->last_result = "awaitingApproval";
				else if (exists) schedule->last_result = "started";
				else
				{
					schedule->last_result = "failed";
					schedule->last_error = "UAM stopped during this run, so it may not have started a chat.";
					Notify("Scheduled run interrupted", "Schedule “" + schedule->name + "” stopped with UAM during a run; check Active Chats for its chat.", "warning");
				}
			}
			m_unstarted_after_restart.clear();
			SaveSchedules();
			changed = true;
		}
		for (std::size_t index = 0; index < m_schedules.size(); ++index)
		{
			Schedule& schedule = m_schedules[index];
			if (schedule.paused) continue;
			// The clock was set back (or a run happened while it was wrong): a next run more than one period away starts
			// its timing again from now rather than leaving the schedule silent until the clock catches up.
			const std::int64_t period_ms = schedule.trigger.kind == scheduling::TriggerKind::Interval
			    ? static_cast<std::int64_t>(schedule.trigger.interval_minutes) * scheduling::kMinuteMs : 8 * 24 * 60 * scheduling::kMinuteMs;
			if (schedule.next_run_ms > now_ms + period_ms)
			{
				schedule.anchor_ms = now_ms;
				schedule.next_run_ms = 0;
			}
			// A weekday time whose next run is later than the trigger now gives (the clock went back, or the computer moved
			// to another zone) takes the earlier one. NextRunAfter only moves later as time passes, so this never repeats a run.
			if (schedule.trigger.kind == scheduling::TriggerKind::Weekly && schedule.next_run_ms > 0)
				schedule.next_run_ms = std::min(schedule.next_run_ms, scheduling::NextRunAfter(schedule.trigger, schedule.anchor_ms, now_ms, m_clock));
			if (schedule.next_run_ms <= 0) schedule.next_run_ms = scheduling::NextRunAfter(schedule.trigger, schedule.anchor_ms, now_ms, m_clock);
			if (now_ms < schedule.next_run_ms) continue;
			const scheduling::DueDecision decision = scheduling::Decide(schedule.trigger, schedule.missed_policy, schedule.anchor_ms, schedule.next_run_ms, now_ms, m_clock);
			const Schedule before = schedule;
			schedule.next_run_ms = decision.next_run_ms;
			schedule.last_skipped = decision.skipped;
			if (decision.run)
			{
				schedule.last_run_ms = now_ms;
				schedule.last_result = "starting";
				schedule.last_chat_id.clear();
			}
			// The run is recorded before it starts, so a crash or restart can never start the same slot again.
			if (!SaveSchedules())
			{
				m_schedules[index] = before;
				continue;
			}
			changed = true;
			if (decision.skipped > 0 && !decision.run)
				uam::diagnostics::Write("Timers: schedule " + schedule.id + " skipped " + std::to_string(decision.skipped) + " missed run(s).");
			if (!decision.run) continue;
			RunSchedule(app, m_schedules[index]);
			SaveSchedules();
		}
		return changed;
	}

	bool TimerService::Poll(AppState& app)
	{
		return Poll(app, uam::time::SystemEpochMillisecondsNow());
	}

	bool TimerService::Poll(AppState& app, std::int64_t now_ms)
	{
		if (m_wakes.empty() && m_schedules.empty()) return false;
		// The app polls many times a second; timers have minute granularity, so once a second is enough. A clock set
		// back resets the limiter instead of pausing every timer until the clock catches up.
		if (now_ms < m_next_housekeeping_ms && now_ms >= m_next_housekeeping_ms - 1000) return false;
		m_next_housekeeping_ms = now_ms + 1000;
		const bool wakes_changed = PollWakes(app, now_ms);
		const bool schedules_changed = PollSchedules(app, now_ms);
		return wakes_changed || schedules_changed;
	}
} // namespace uam
