#pragma once

#include "app/automation_api_service.h"
#include "common/scheduling/schedule_rules.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uam
{
	struct AppState;

	/// The app's one wall-clock timer service. It owns two saved timer lists and fires them from the app's poll:
	/// - chat wakes: one-shot follow-ups a chat asks for through the UAM `schedule` tool (#965), delivered into that
	///   chat like a pull request watch message;
	/// - automation schedules: recurring automation-API chat requests the user configures in Settings (#518).
	/// Timers are local, so they run whether or not Phone access is on. Every member runs on the UI thread, and the
	/// clock is passed in, so tests drive time directly.
	class TimerService
	{
	  public:
		/// Delivers a wake's text into its chat; a busy chat queues it for after the current turn.
		using WakeSender = std::function<bool(AppState& app, const std::string& chat_id, const std::string& text, std::string* error)>;
		/// Creates one scheduled run's chat (tests replace it). Defaults to AutomationApiService::CreateScheduledChat.
		using ChatCreator = std::function<AutomationApiResponse(AppState& app, const AutomationScheduleCredential& credential, const nlohmann::json& request)>;
		/// Starts a trusted run's chat, which was created waiting; the outcome comes back through RecordLaunchResult.
		/// Registered by the desktop bridge; without one a trusted run's chat waits for the user like an untrusted one.
		using Launcher = std::function<void(AppState& app, const std::string& schedule_id, const std::string& chat_id)>;

		static constexpr int kMinWakeMinutes = 1;
		static constexpr int kMaxWakeMinutes = 24 * 60;
		static constexpr std::size_t kMaxWakesPerChat = 3;
		static constexpr std::size_t kMaxWakeNoteBytes = 500;
		static constexpr std::size_t kMaxSchedules = 32;
		/// Refused deliveries, a minute apart, after which a wake is dropped instead of retried for ever.
		static constexpr int kWakeRefusalLimit = 30;

		struct ChatWake
		{
			std::string id;
			std::string chat_id;
			std::string note;
			std::int64_t created_ms = 0;
			std::int64_t due_ms = 0;
			/// The chat's settled time when the wake was set; a different one later means the user has marked it done since.
			std::string settled_seen;
			/// Saved just before a delivery attempt, so a crash during one drops the wake rather than sending it twice.
			bool delivering = false;
			// This app run only.
			int refusals = 0;
			std::int64_t retry_at_ms = 0;
		};

		struct Schedule
		{
			std::string id;
			std::string name;
			bool paused = false;
			bool trusted = false;
			scheduling::Trigger trigger;
			scheduling::MissedPolicy missed_policy = scheduling::MissedPolicy::Skip;
			/// The same body as POST /automation/chats.
			nlohmann::json request = nlohmann::json::object();
			std::string created_at;
			/// Intervals count from here: creation, an edit or a resume.
			std::int64_t anchor_ms = 0;
			std::int64_t next_run_ms = 0;
			std::int64_t last_run_ms = 0;
			/// "started", "awaitingApproval", "starting" or "failed"; empty before the first run.
			std::string last_result;
			std::string last_error;
			std::string last_chat_id;
			/// Missed slots the last decision did not run.
			int last_skipped = 0;
		};

		explicit TimerService(std::filesystem::path data_root, WakeSender sender = {}, ChatCreator creator = {}, scheduling::LocalClock clock = {});

		/// Fires what is due: delivers wakes and starts scheduled runs. True when UI state changed.
		bool Poll(AppState& app, std::int64_t now_ms);
		bool Poll(AppState& app);

		// Chat wakes.
		/// Sets one wake for the chat. Returns the wake, or a one-line reason in error_out.
		std::optional<ChatWake> ScheduleWake(const AppState& app, std::string_view chat_id, int minutes, std::string_view note, std::int64_t now_ms, std::string* error_out = nullptr);
		/// The user pressed Stop on the chat: every pending wake of that chat is dropped.
		bool CancelWakesForChat(std::string_view chat_id);
		/// Removes one wake, e.g. when the tool call that set it could not be recorded.
		bool CancelWake(std::string_view wake_id);
		std::vector<ChatWake> WakesForChat(std::string_view chat_id) const;

		// Automation schedules; desktop Settings only.
		void SetLauncher(Launcher launcher) { m_launcher = std::move(launcher); }
		std::optional<nlohmann::json> ListSchedules(std::string* error_out = nullptr) const;
		std::optional<nlohmann::json> CreateSchedule(const AppState& app, const nlohmann::json& spec, std::int64_t now_ms, std::string* error_out = nullptr);
		std::optional<nlohmann::json> UpdateSchedule(const AppState& app, std::string_view id, const nlohmann::json& spec, std::int64_t now_ms, std::string* error_out = nullptr);
		std::optional<nlohmann::json> SetSchedulePaused(std::string_view id, bool paused, std::int64_t now_ms, std::string* error_out = nullptr);
		bool DeleteSchedule(std::string_view id, std::string* error_out = nullptr);
		/// The launcher's outcome for a trusted run's chat. A stale chat (an earlier run) changes nothing.
		void RecordLaunchResult(std::string_view schedule_id, std::string_view chat_id, bool started, std::string_view error);
		const std::vector<Schedule>& SchedulesForTests() const { return m_schedules; }

		/// {"notices": [{serial, message, tone, chatId?}]}: one-line messages for the notification system.
		nlohmann::json SerializeForFrontend() const;

	  private:
		void LoadWakes();
		void LoadSchedules();
		bool SaveWakes();
		bool SaveSchedules();
		bool PollWakes(AppState& app, std::int64_t now_ms);
		bool PollSchedules(AppState& app, std::int64_t now_ms);
		void RunSchedule(AppState& app, Schedule& schedule);
		void Notify(std::string message, std::string tone, std::string chat_id = {});
		Schedule* FindSchedule(std::string_view id);
		std::optional<Schedule> ScheduleFromSpec(const AppState& app, const nlohmann::json& spec, std::string* error_out) const;
		static nlohmann::json ScheduleJson(const Schedule& schedule);

		struct Notice
		{
			int serial = 0;
			std::string message;
			std::string tone;
			std::string chat_id;
		};

		std::filesystem::path m_wakes_file;
		std::filesystem::path m_schedules_file;
		WakeSender m_sender;
		ChatCreator m_creator;
		Launcher m_launcher;
		scheduling::LocalClock m_clock;
		std::vector<ChatWake> m_wakes;
		std::vector<Schedule> m_schedules;
		/// The saved schedule list exists but could not be read; nothing runs and nothing overwrites it.
		bool m_schedules_unreadable = false;
		/// Schedules whose last run's chat was created but not started when UAM stopped; checked on the first poll.
		std::vector<std::string> m_unstarted_after_restart;
		bool m_chats_seen = false;
		bool m_wakes_save_failed = false;
		bool m_schedules_save_failed = false;
		std::vector<Notice> m_notices;
		int m_notice_serial = 0;
		std::int64_t m_next_housekeeping_ms = 0;
	};
} // namespace uam
