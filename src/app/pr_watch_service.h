#pragma once

#include "common/pr_watch/pr_watch_rules.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace uam
{
	struct AppState;

	/// Watches GitHub pull requests for chats and wakes a chat when one changes.
	/// One instance serves the whole app. GitHub is read through the signed-in `gh` on a background
	/// thread; every other member runs on the UI thread. Cadence, conditional requests and stop
	/// conditions follow T3 Code (MIT, github.com/pingdotgg/t3code).
	class PrWatchService
	{
	  public:
		/// Runs `gh` with these arguments and returns its standard output. Tests supply a fake.
		using GhRunner = std::function<std::string(const std::vector<std::string>& arguments, std::stop_token stop)>;
		/// Delivers the wake text into a chat; a busy chat queues it for after the current turn.
		using WakeSender = std::function<bool(AppState& app, const std::string& chat_id, const std::string& text, std::string* error)>;

		struct Validator
		{
			std::string etag;
			std::string body;
		};

		struct ReadJob
		{
			std::string key;
			pr_watch::Reference pr;
			std::map<std::string, Validator> validators;
			/// Comments and reviews are read only for a watched pull request.
			bool want_remarks = false;
			std::int64_t remarks_since_ms = 0;
			/// Signed-in account when already known.
			std::string viewer;
		};

		struct ReadResult
		{
			std::string key;
			bool ok = false;
			bool not_found = false;
			bool rate_limited = false;
			std::int64_t retry_at_ms = 0;
			/// HTTP status of the failed request; zero when `gh` gave no answer.
			int status = 0;
			/// When the sweep that made this read started; cadence counts from here, not from when the answer was applied.
			std::int64_t read_at_ms = 0;
			pr_watch::Detail detail;
			bool remarks_read = false;
			std::vector<pr_watch::Remark> remarks;
			std::string viewer;
			std::map<std::string, Validator> validators;
		};

		explicit PrWatchService(std::filesystem::path data_root, GhRunner runner = {}, WakeSender sender = {});
		~PrWatchService();
		void Stop();

		/// Applies finished reads, delivers wakes and starts the next sweep. True when UI state changed.
		bool Poll(AppState& app, std::int64_t now_ms);
		bool Poll(AppState& app);

		/// Links the pull request to the chat and watches it. Watching again keeps what was already reported.
		bool Watch(const AppState& app, std::string_view chat_id, const pr_watch::Reference& pr, std::int64_t now_ms, std::string* error_out = nullptr);
		/// Stops watching and keeps the link and what was reported.
		bool Unwatch(std::string_view chat_id, const pr_watch::Reference& pr);
		/// Removes the link and forgets what was reported.
		bool Unlink(std::string_view chat_id, const pr_watch::Reference& pr);
		/// The user pressed Stop on the chat: every watch of that chat ends.
		bool EndWatchesForChat(std::string_view chat_id);
		bool IsWatching(std::string_view chat_id, const pr_watch::Reference& pr) const;

		nlohmann::json SerializeForFrontend() const;

		/// One conditional read of a pull request through `gh api`.
		static ReadResult Read(const ReadJob& job, const GhRunner& runner, std::int64_t now_ms, std::stop_token stop = {});
		bool WorkerPending() const { return m_worker.valid(); }
		/// Blocks until the running sweep finishes; tests only.
		void WaitForWorkerForTests() const { if (m_worker.valid()) m_worker.wait(); }

	  private:
		struct Link
		{
			std::string chat_id;
			pr_watch::Reference pr;
			bool watching = false;
			pr_watch::Reported reported;
			/// News not yet accepted by the chat; saved so a restart delivers it once.
			std::string pending_wake;
			/// The chat's settled time when watching started; a different one later means the user has marked it done since.
			std::string settled_seen;
			// This app run only: refused deliveries of the pending wake, and the latest notice for the notifications panel.
			int wake_refusals = 0;
			int notice_serial = 0;
			std::string notice_message;
			std::string notice_tone;
		};

		struct Snapshot
		{
			pr_watch::Reference pr;
			bool has_detail = false;
			pr_watch::Detail detail;
			bool missing = false;
			// This app run only.
			std::map<std::string, Validator> validators;
			std::int64_t last_read_ms = 0;
			int failed_reads = 0;
		};

		void Load();
		bool Save();
		void Apply(const std::vector<ReadResult>& results, std::int64_t now_ms);
		bool DeliverWakes(AppState& app, std::int64_t now_ms);
		void StartSweep(const AppState& app, std::int64_t now_ms);
		Link* FindLink(std::string_view chat_id, const pr_watch::Reference& pr);
		void Notify(Link& link, std::string message, std::string tone);

		std::filesystem::path m_file;
		GhRunner m_runner;
		WakeSender m_sender;
		std::vector<Link> m_links;
		std::map<std::string, Snapshot> m_snapshots;
		std::string m_viewer;
		/// One counter for every link, so a re-linked pull request never reuses a serial the interface has seen.
		int m_notice_serial = 0;
		std::string m_saved;
		bool m_dirty = false;
		bool m_save_failed = false;
		std::future<std::vector<ReadResult>> m_worker;
		std::stop_source m_stop;
		std::int64_t m_next_sweep_ms = 0;
		std::int64_t m_next_housekeeping_ms = 0;
		std::int64_t m_next_wake_retry_ms = 0;
		std::int64_t m_paused_until_ms = 0;
	};
} // namespace uam
