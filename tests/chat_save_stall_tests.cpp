#include "test_harness.h"
#include <set>
#include "common/chat/chat_repository.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/app_time.h"

using namespace uam_test;

namespace uam
{
	bool PendingChatSavePreparationForTests(const AppState&, const std::string&, bool&);
	bool TranscriptWorkerBoundBusyForTests(const AppState&);
}

namespace
{
	using Clock = std::chrono::steady_clock;

	double MsSince(Clock::time_point start)
	{
		return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
	}

	// The reported install: 1,842 chats, each with a primary, backup, page index and more beside it.
	void FillDirectory(const fs::path& directory, int entries)
	{
		fs::create_directories(directory);
		for (int index = 0; index < entries; ++index) std::ofstream(directory / ("filler-" + std::to_string(index) + ".json")) << "{}";
	}

	struct SaveStallFixture
	{
		TempDir temporary{"uam-chat-save-stall"};
		uam::AppState app;
		uam::AcpSessionState* session = nullptr;

		explicit SaveStallFixture(int messages = 5000, std::size_t bytes_each = 6 * 1024)
		{
			app.data_root = temporary.root;
			ChatSession chat; chat.id = "stall-chat"; chat.provider_id = "codex-cli";
			chat.created_at = chat.updated_at = "2026-11-10T00:00:00Z";
			for (int index = 0; index < messages; ++index)
				chat.messages.push_back({.role = index % 2 == 0 ? MessageRole::User : MessageRole::Assistant,
				    .content = std::string(bytes_each, static_cast<char>('a' + index % 26))});
			app.chats.push_back(std::move(chat));
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
			app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
			session = app.acp_sessions.back().get();
			session->chat_id = "stall-chat"; session->provider_id = "codex-cli"; session->processing = true;
		}
		ChatSession& Chat() { return app.chats.front(); }
		std::string SavedTail() const
		{
			const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, "stall-chat", true, nullptr, false);
			return saved && !saved->messages.empty() ? saved->messages.back().content : std::string();
		}
		std::string SavedRow(std::size_t index) const
		{
			const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, "stall-chat", true, nullptr, false);
			return saved && index < saved->messages.size() ? saved->messages[index].content : std::string();
		}
		// Polls until a snapshot has been captured and its preparation is running or parked.
		void AwaitCapture()
		{
			bool ready = false;
			const Clock::time_point deadline = Clock::now() + std::chrono::seconds(60);
			while (!uam::PendingChatSavePreparationForTests(app, "stall-chat", ready) && Clock::now() < deadline)
			{
				uam::FlushPendingChatSaves(app); std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			UAM_ASSERT(uam::PendingChatSavePreparationForTests(app, "stall-chat", ready));
		}
		void Stream(const std::string& text)
		{
			Chat().messages.back().content += text;
			uam::acp_detail::ScheduleChatSave(app, Chat(), 0.0, true);
		}
		void Drain()
		{
			const Clock::time_point deadline = Clock::now() + std::chrono::seconds(120);
			while (!app.pending_chat_save_at_by_chat_id.empty() && Clock::now() < deadline)
			{
				uam::FlushPendingChatSaves(app); std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			UAM_ASSERT(app.pending_chat_save_at_by_chat_id.empty());
		}
	};
}

// Prints the workload figures quoted in the change log; the budget cases below hold the limits.
UAM_TEST(ChatSaveStallMeasurements)
{
	{
		TempDir temporary{"uam-atomic-write-scale"};
		for (const int entries : {10, 4000})
		{
			const fs::path directory = temporary.root / std::to_string(entries);
			FillDirectory(directory, entries);
			const fs::path target = directory / "target.json";
			UAM_ASSERT(uam::io::WriteTextFileWithBackup(target, "first"));
			UAM_ASSERT(uam::io::WriteTextFileWithBackup(target, "second"));
			const Clock::time_point start = Clock::now();
			for (int write = 0; write < 100; ++write) UAM_ASSERT(uam::io::WriteTextFileWithBackup(target, "content " + std::to_string(write)));
			std::fprintf(stderr, "[measure] 100 backup-preserving atomic rewrites beside %d entries: %.1f ms\n", entries, MsSince(start));
		}
	}
	{
		TempDir temporary{"uam-import-scale"};
		FillDirectory(temporary.root / "chats", 4000);
		std::vector<ChatSession> chats(100);
		for (std::size_t index = 0; index < chats.size(); ++index)
		{
			chats[index].id = "imported-" + std::to_string(index); chats[index].provider_id = "codex-cli";
			chats[index].created_at = chats[index].updated_at = "2026-11-10T00:00:00Z";
			chats[index].messages.push_back({.role = MessageRole::User, .content = "hello"});
		}
		Clock::time_point start = Clock::now();
		for (const ChatSession& chat : chats) UAM_ASSERT(ChatRepository::SaveChat(temporary.root, chat));
		std::fprintf(stderr, "[measure] 100 new small chats saved beside 4000 entries: %.1f ms\n", MsSince(start));
		start = Clock::now();
		for (const ChatSession& chat : chats) UAM_ASSERT(ChatRepository::SaveChat(temporary.root, chat));
		std::fprintf(stderr, "[measure] 100 existing small chats re-saved beside 4000 entries: %.1f ms\n", MsSince(start));
	}
	SaveStallFixture fixture;
	fixture.Chat().messages.back().content += " tool update";
	Clock::time_point start = Clock::now();
	UAM_ASSERT(uam::acp_detail::SaveChatQuietly(fixture.app, fixture.Chat()));
	std::fprintf(stderr, "[measure] synchronous save of 5000 messages (30 MB) on the calling thread: %.1f ms\n", MsSince(start));

	// Three seconds of 20 ms chunks, polled like the UI loop; then look at the disk before the stream pauses.
	double longest_poll = 0.0, total_poll = 0.0;
	std::size_t streamed = 0;
	const Clock::time_point stream_start = Clock::now();
	Clock::time_point next_chunk = stream_start;
	while (Clock::now() - stream_start < std::chrono::seconds(3))
	{
		if (Clock::now() >= next_chunk)
		{
			fixture.Chat().messages.back().content += std::string(200, 'S'); streamed += 200;
			uam::acp_detail::ScheduleChatSave(fixture.app, fixture.Chat(), 0.5, true);
			next_chunk += std::chrono::milliseconds(20);
		}
		start = Clock::now();
		uam::FlushPendingChatSaves(fixture.app);
		const double poll = MsSince(start); longest_poll = std::max(longest_poll, poll); total_poll += poll;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const std::string tail = fixture.SavedTail();
	const std::size_t on_disk = static_cast<std::size_t>(std::ranges::count(tail, 'S'));
	std::fprintf(stderr, "[measure] after 3 s of unbroken streaming: %zu of %zu streamed bytes on disk; poll thread spent %.1f ms in total, longest poll %.1f ms\n",
	    on_disk, streamed, total_poll, longest_poll);
	fixture.Drain();
	UAM_ASSERT_EQ(fixture.SavedTail(), fixture.Chat().messages.back().content);
}

UAM_TEST(AtomicWriteDoesNotScaleWithDirectoryEntries)
{
	TempDir temporary{"uam-atomic-write-budget"};
	FillDirectory(temporary.root, 4000);
	const fs::path target = temporary.root / "target.json";
	UAM_ASSERT(uam::io::WriteTextFileWithBackup(target, "first"));
	const std::uint64_t before = uam::io::AtomicTempSweepEntriesExamined().load();
	for (int write = 0; write < 100; ++write) UAM_ASSERT(uam::io::WriteTextFileWithBackup(target, "content " + std::to_string(write)));
	// Before the fix each write looked at every neighbour twice: 800,000 entries here. Now the directory
	// was listed once by the first write, and at most once more if ten minutes passed.
	UAM_ASSERT(uam::io::AtomicTempSweepEntriesExamined().load() - before <= 4100);
}

UAM_TEST(AtomicWriteRemovesOnlyAbandonedTempsOfItsDestination)
{
	TempDir temporary{"uam-atomic-temp-sweep"};
	const fs::path target = temporary.root / "target.json", other = temporary.root / "other.json";
	const auto old = fs::file_time_type::clock::now() - std::chrono::hours(25);
	// Seeded before this process first writes here, as a crashed earlier launch leaves them.
	std::ofstream(target) << "seed"; std::ofstream(uam::io::MakeBackupPath(target)) << "seed"; std::ofstream(other) << "seed";
	const fs::path abandoned = temporary.root / "target.json.tmp.1", abandoned_backup = temporary.root / "target.json.bak.tmp.2";
	const fs::path live = temporary.root / "target.json.tmp.3", foreign = temporary.root / "other.json.tmp.4";
	for (const fs::path& path : {abandoned, abandoned_backup, live, foreign}) std::ofstream(path) << "partial";
	for (const fs::path& path : {abandoned, abandoned_backup, foreign}) fs::last_write_time(path, old);
	UAM_ASSERT(uam::io::WriteTextFileWithBackup(target, "first"));
	UAM_ASSERT(!fs::exists(abandoned) && !fs::exists(abandoned_backup));
	// A recent temporary may belong to a live writer; another destination's is that destination's to remove.
	UAM_ASSERT(fs::exists(live) && fs::exists(foreign));
	UAM_ASSERT(uam::io::WriteTextFileWithBackup(other, "first"));
	UAM_ASSERT(!fs::exists(foreign) && fs::exists(live));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(target), std::string("first"));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(uam::io::MakeBackupPath(target)), std::string("seed"));
}

UAM_TEST(LaterScheduleCannotPostponeDueChatSave)
{
	SaveStallFixture fixture(4, 64);
	uam::acp_detail::ScheduleChatSave(fixture.app, fixture.Chat(), 0.0);
	const double due = fixture.app.pending_chat_save_at_by_chat_id.at("stall-chat");
	uam::acp_detail::ScheduleChatSave(fixture.app, fixture.Chat(), 60.0);
	uam::acp_detail::ScheduleChatSave(fixture.app, fixture.Chat(), 0.5, true);
	UAM_ASSERT_EQ(fixture.app.pending_chat_save_at_by_chat_id.at("stall-chat"), due);
	fixture.Drain();
}

UAM_TEST(StreamingTextReachesDiskWithinMaximumWait)
{
	SaveStallFixture fixture(200, 6 * 1024);
	std::size_t streamed = 0, streamed_by_1500ms = 0;
	const Clock::time_point stream_start = Clock::now();
	Clock::time_point next_chunk = stream_start;
	while (Clock::now() - stream_start < std::chrono::seconds(5))
	{
		if (Clock::now() >= next_chunk)
		{
			fixture.Chat().messages.back().content += std::string(100, 'S'); streamed += 100;
			if (Clock::now() - stream_start <= std::chrono::milliseconds(1500)) streamed_by_1500ms = streamed;
			uam::acp_detail::ScheduleChatSave(fixture.app, fixture.Chat(), 0.5, true);
			next_chunk += std::chrono::milliseconds(10);
		}
		uam::FlushPendingChatSaves(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	// The stream never paused for the 0.5 s debounce, yet text older than the 2 s maximum wait is durable.
	const std::string tail = fixture.SavedTail();
	const std::size_t on_disk = static_cast<std::size_t>(std::ranges::count(tail, 'S'));
	UAM_ASSERT(streamed_by_1500ms > 0 && on_disk >= streamed_by_1500ms);
	// It is an older snapshot, so the chat is still owed a save: never clean with unsaved text.
	UAM_ASSERT(on_disk < streamed && fixture.app.pending_chat_save_at_by_chat_id.contains("stall-chat"));
	fixture.Drain();
	UAM_ASSERT_EQ(fixture.SavedTail(), fixture.Chat().messages.back().content);
}

UAM_TEST(StreamingSnapshotIsRevokedByOrdinaryChange)
{
	SaveStallFixture fixture(2000, 6 * 1024);
	fixture.Chat().messages.back().content += "S";
	uam::acp_detail::ScheduleChatSave(fixture.app, fixture.Chat(), 0.0, true);
	bool ready = false;
	const Clock::time_point deadline = Clock::now() + std::chrono::seconds(60);
	while (!uam::PendingChatSavePreparationForTests(fixture.app, "stall-chat", ready) && Clock::now() < deadline)
	{
		uam::FlushPendingChatSaves(fixture.app); std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT(uam::PendingChatSavePreparationForTests(fixture.app, "stall-chat", ready));
	// An edit to an earlier row is not append-only: the captured snapshot must not be published as current.
	fixture.Chat().messages.front().content = "edited";
	uam::acp_detail::ScheduleChatSave(fixture.app, fixture.Chat(), 0.0);
	fixture.Drain();
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, "stall-chat", true, nullptr, false);
	UAM_ASSERT(saved && saved->messages.size() == fixture.Chat().messages.size());
	UAM_ASSERT_EQ(saved->messages.front().content, std::string("edited"));
	UAM_ASSERT_EQ(saved->messages.back().content, fixture.Chat().messages.back().content);
}

UAM_TEST(ToolCallUpdateDoesNotWriteTranscriptOnCallingThread)
{
	SaveStallFixture fixture;
	Clock::time_point start = Clock::now();
	for (int update = 0; update < 20; ++update)
	{
		fixture.Chat().messages.back().content += " tool status";
		uam::acp_detail::SaveChatSoon(fixture.app, fixture.Chat());
	}
	const double calling_thread = MsSince(start);
	std::fprintf(stderr, "[measure] 20 tool-call updates on a 5000-message (30 MB) chat held the calling thread %.2f ms\n", calling_thread);
	// Twenty whole-transcript synchronous saves took seconds; scheduling them is independent of transcript size.
	UAM_ASSERT(calling_thread <= 100.0);
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.contains("stall-chat"));
	double longest_poll = 0.0;
	const Clock::time_point deadline = Clock::now() + std::chrono::seconds(120);
	while (!fixture.app.pending_chat_save_at_by_chat_id.empty() && Clock::now() < deadline)
	{
		start = Clock::now(); uam::FlushPendingChatSaves(fixture.app); longest_poll = std::max(longest_poll, MsSince(start));
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	std::fprintf(stderr, "[measure] background save of that chat: longest single poll %.1f ms\n", longest_poll);
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.empty());
	UAM_ASSERT_EQ(fixture.SavedTail(), fixture.Chat().messages.back().content);

	// Outside an active turn the durable receipt still precedes the return.
	fixture.session->processing = false;
	fixture.Chat().messages.back().content += " settled";
	uam::acp_detail::SaveChatSoon(fixture.app, fixture.Chat());
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.empty());
	UAM_ASSERT_EQ(fixture.SavedTail(), fixture.Chat().messages.back().content);
}

UAM_TEST(ChatSaveFailureSurfacesOnceAndBacksOff)
{
	SaveStallFixture fixture(4, 64);
	const fs::path primary = AppPaths::UamChatFilePath(fixture.app.data_root, "stall-chat");
	fs::rename(primary, fixture.temporary.root / "preserved.json"); fs::create_directory(primary);
	fixture.Stream(" unsaved output");
	const auto attempts = [&] { const auto found = fixture.app.chat_save_failures_by_chat_id.find("stall-chat"); return found == fixture.app.chat_save_failures_by_chat_id.end() ? 0 : found->second.attempts; };
	const auto poll_until_attempts = [&](int wanted)
	{
		const Clock::time_point deadline = Clock::now() + std::chrono::seconds(30);
		while (attempts() < wanted && Clock::now() < deadline)
		{
			// The stream keeps producing output while the disk refuses it.
			uam::acp_detail::ScheduleChatSave(fixture.app, fixture.Chat(), 0.5, true);
			uam::FlushPendingChatSaves(fixture.app); std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		UAM_ASSERT_EQ(attempts(), wanted);
	};
	poll_until_attempts(1);
	UAM_ASSERT_EQ(fixture.app.status_line, std::string(uam::acp_detail::kChatSaveFailedStatus));
	const double first_wait = fixture.app.chat_save_failures_by_chat_id.at("stall-chat").retry_at - uam::GetAppTimeSeconds();
	UAM_ASSERT(first_wait > 0.0 && first_wait <= 1.0);
	// A burst of synchronous saves that fail before the retry is due is the same failure, not five more.
	for (int save = 0; save < 5; ++save) UAM_ASSERT(!uam::acp_detail::SaveChatQuietly(fixture.app, fixture.Chat()));
	UAM_ASSERT_EQ(attempts(), 1);
	// Told once: the scheduled retry fails quietly and doubles the wait.
	fixture.app.status_line.clear();
	poll_until_attempts(2);
	UAM_ASSERT(fixture.app.status_line.empty());
	UAM_ASSERT(fixture.app.chat_save_failures_by_chat_id.at("stall-chat").retry_at - uam::GetAppTimeSeconds() > 1.5);
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.contains("stall-chat")); // Still owed, never marked clean.
	// The quit barrier tries again and leaves the failure on record rather than dropping it.
	uam::FlushPendingChatSaves(fixture.app, true);
	UAM_ASSERT_EQ(attempts(), 2);
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.contains("stall-chat"));

	fs::remove(primary); fs::rename(fixture.temporary.root / "preserved.json", primary);
	uam::acp_detail::ScheduleChatSave(fixture.app, fixture.Chat(), 0.0);
	fixture.Drain();
	UAM_ASSERT(fixture.app.chat_save_failures_by_chat_id.empty());
	UAM_ASSERT_EQ(fixture.SavedTail(), fixture.Chat().messages.back().content);
}

UAM_TEST(ChatSaveRecordsDoNotOutliveDeletedChat)
{
	SaveStallFixture fixture(4, 64);
	fixture.Stream(" saved once");
	fixture.Drain();
	UAM_ASSERT(fixture.app.chat_save_pace_by_chat_id.contains("stall-chat") && !fixture.app.saved_chat_rows.empty());
	(void)uam::acp_detail::NoteChatSaveFailed(fixture.app, "stall-chat");
	UAM_ASSERT_EQ(fixture.app.status_line, std::string(uam::acp_detail::kChatSaveFailedStatus));
	fixture.app.acp_sessions.clear(); fixture.app.chats.clear();
	fixture.app.chat_save_records_pruned_at = uam::GetAppTimeSeconds() - 10.0;
	uam::FlushPendingChatSaves(fixture.app);
	UAM_ASSERT(fixture.app.chat_save_failures_by_chat_id.empty() && fixture.app.chat_save_pace_by_chat_id.empty() && fixture.app.saved_chat_rows.empty());
	UAM_ASSERT(fixture.app.status_line.empty());
}

UAM_TEST(ChatSaveRefusalIsNotReportedAsStorageFailure)
{
	SaveStallFixture fixture(4, 64);
	fixture.Chat().managed_followup_incarnation = "not-a-uuid";
	UAM_ASSERT(!uam::acp_detail::SaveChatQuietly(fixture.app, fixture.Chat()));
	UAM_ASSERT_EQ(fixture.app.status_line, std::string(uam::acp_detail::kChatSaveRefusedStatus));
	fixture.Chat().managed_followup_incarnation.clear();
	UAM_ASSERT(uam::acp_detail::SaveChatQuietly(fixture.app, fixture.Chat()));
	UAM_ASSERT(fixture.app.status_line.empty() && fixture.app.chat_save_failures_by_chat_id.empty());
}

UAM_TEST(ZeroDelayUpdatesRewriteTranscriptAtMostTwiceASecond)
{
	SaveStallFixture fixture(50, 1024);
	std::set<double> finished;
	const Clock::time_point start = Clock::now();
	while (Clock::now() - start < std::chrono::seconds(3))
	{
		// Tool status and plan rows arrive far faster than this small transcript takes to write.
		fixture.Chat().messages.back().content += "t";
		uam::acp_detail::SaveChatSoon(fixture.app, fixture.Chat());
		uam::FlushPendingChatSaves(fixture.app);
		if (const auto pace = fixture.app.chat_save_pace_by_chat_id.find("stall-chat"); pace != fixture.app.chat_save_pace_by_chat_id.end()) finished.insert(pace->second.finished_at);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	// One rewrite at once, then no sooner than half a second after each finishes: at most 7 in 3 s.
	UAM_ASSERT(finished.size() >= 2 && finished.size() <= 7);
	fixture.Drain();
	UAM_ASSERT_EQ(fixture.SavedTail(), fixture.Chat().messages.back().content);
}

UAM_TEST(UpdatesToEarlierRowReachDiskWhileTheyContinue)
{
	SaveStallFixture fixture(2000, 6 * 1024);
	// A tool row with later rows after it (a steer, a sub-agent row), updated faster than one preparation.
	const std::size_t row = fixture.Chat().messages.size() - 3;
	std::size_t updates = 0;
	const Clock::time_point start = Clock::now();
	Clock::time_point next = start;
	while (Clock::now() - start < std::chrono::seconds(5))
	{
		if (Clock::now() >= next)
		{
			fixture.Chat().messages[row].content += "U"; ++updates;
			uam::acp_detail::SaveChatSoon(fixture.app, fixture.Chat());
			next += std::chrono::milliseconds(5);
		}
		uam::FlushPendingChatSaves(fixture.app);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const std::string saved = fixture.SavedRow(row);
	const std::size_t on_disk = static_cast<std::size_t>(std::ranges::count(saved, 'U'));
	UAM_ASSERT(on_disk > 0 && on_disk < updates);
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.contains("stall-chat"));
	fixture.Drain();
	UAM_ASSERT_EQ(fixture.SavedRow(row), fixture.Chat().messages[row].content);
}

UAM_TEST(OlderSnapshotDoesNotReplaceNewerSummaryOverlay)
{
	SaveStallFixture fixture(2000, 6 * 1024);
	fixture.Stream("S");
	fixture.AwaitCapture();
	// The user opens the chat: the summary overlay is written directly, with no save revocation.
	fixture.Chat().last_opened_at = "2026-11-10T09:09:09Z";
	UAM_ASSERT(ChatRepository::SaveLastOpenedAt(fixture.app.data_root, fixture.Chat(), false));
	fixture.Stream("S");
	const auto saved_last_opened = [&]
	{
		for (const ChatSession& summary : ChatRepository::LoadLocalChatSummariesReadOnly(fixture.app.data_root))
			if (summary.id == "stall-chat") return summary.last_opened_at;
		return std::string();
	};
	const Clock::time_point deadline = Clock::now() + std::chrono::seconds(120);
	while (!fixture.app.pending_chat_save_at_by_chat_id.empty() && Clock::now() < deadline)
	{
		uam::FlushPendingChatSaves(fixture.app);
		UAM_ASSERT_EQ(saved_last_opened(), std::string("2026-11-10T09:09:09Z"));
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.empty());
	UAM_ASSERT_EQ(fixture.SavedTail(), fixture.Chat().messages.back().content);
}

UAM_TEST(SynchronousSaveWinsOverOlderSnapshotInFlight)
{
	SaveStallFixture fixture(2000, 6 * 1024);
	fixture.Stream("A");
	fixture.AwaitCapture();
	fixture.Stream("B");
	fixture.Chat().messages.back().content += "C";
	UAM_ASSERT(uam::acp_detail::SaveChatQuietly(fixture.app, fixture.Chat()));
	UAM_ASSERT_EQ(fixture.SavedTail(), fixture.Chat().messages.back().content);
	// The snapshot captured at "A" finishes preparing afterwards and must not be published over "ABC".
	const Clock::time_point until = Clock::now() + std::chrono::seconds(3);
	while (Clock::now() < until || !fixture.app.pending_chat_save_tasks.empty())
	{
		uam::FlushPendingChatSaves(fixture.app); std::this_thread::sleep_for(std::chrono::milliseconds(2));
		UAM_ASSERT(Clock::now() < until + std::chrono::seconds(120));
	}
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.empty());
	UAM_ASSERT_EQ(fixture.SavedTail(), fixture.Chat().messages.back().content);
}

UAM_TEST(ForceBarrierWinsOverOlderSnapshotInFlight)
{
	SaveStallFixture fixture(2000, 6 * 1024);
	fixture.Stream("A");
	fixture.AwaitCapture();
	fixture.Stream("B");
	uam::FlushPendingChatSaves(fixture.app, true);
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.empty());
	UAM_ASSERT_EQ(fixture.SavedTail(), fixture.Chat().messages.back().content);
	for (int poll = 0; poll < 500; ++poll) { uam::FlushPendingChatSaves(fixture.app); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
	UAM_ASSERT_EQ(fixture.SavedTail(), fixture.Chat().messages.back().content);
}

UAM_TEST(FinishedSaveResultDoesNotOccupyTranscriptWorkerBound)
{
	SaveStallFixture fixture(2000, 6 * 1024);
	fixture.Stream("A");
	fixture.AwaitCapture();
	bool ready = false;
	// While the preparation runs it is the one transcript worker; once its result only waits to be
	// collected (as it does for a chat the poll skips), it must not stop another chat's save from starting.
	if (uam::PendingChatSavePreparationForTests(fixture.app, "stall-chat", ready) && !ready) UAM_ASSERT(uam::TranscriptWorkerBoundBusyForTests(fixture.app));
	const Clock::time_point deadline = Clock::now() + std::chrono::seconds(120);
	while (uam::PendingChatSavePreparationForTests(fixture.app, "stall-chat", ready) && !ready && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(2));
	UAM_ASSERT(ready);
	UAM_ASSERT(!uam::TranscriptWorkerBoundBusyForTests(fixture.app));
	fixture.Drain();
}
