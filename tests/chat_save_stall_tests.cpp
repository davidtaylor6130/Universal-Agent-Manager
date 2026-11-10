#include "test_harness.h"
#include "common/chat/chat_repository.h"
#include "common/runtime/acp/acp_session_internal.h"

using namespace uam_test;

namespace uam
{
	bool PendingChatSavePreparationForTests(const AppState&, const std::string&, bool&);
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
	double elapsed[2] = {};
	int slot = 0;
	for (const int entries : {10, 4000})
	{
		const fs::path directory = temporary.root / std::to_string(entries);
		FillDirectory(directory, entries);
		const fs::path target = directory / "target.json";
		UAM_ASSERT(uam::io::WriteTextFileWithBackup(target, "first"));
		UAM_ASSERT(uam::io::WriteTextFileWithBackup(target, "second"));
		const Clock::time_point start = Clock::now();
		for (int write = 0; write < 100; ++write) UAM_ASSERT(uam::io::WriteTextFileWithBackup(target, "content " + std::to_string(write)));
		elapsed[slot++] = MsSince(start);
	}
	// Before the fix each write listed and inspected every neighbour twice: 4,000 entries cost many times 10.
	UAM_ASSERT(elapsed[1] <= elapsed[0] * 2.0 + 1000.0);
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
