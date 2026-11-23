#include "test_harness.h"
#include <set>
#include "common/chat/chat_repository.h"
#include "common/runtime/acp/acp_polling.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/app_time.h"
#include "common/utils/diagnostic_log.h"

#include <cstdio>
#include <mutex>
#if defined(_WIN32)
#include <io.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

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

namespace
{
	// Stands in for a remote helper's stdin: records each write and what the chat file held at that moment.
	class RemoteHelperInput
	{
	public:
		RemoteHelperInput(uam::AcpSessionState& session, fs::path data_root, std::string chat_id)
		    : m_session(session), m_file(std::tmpfile()), m_root(std::move(data_root)), m_chat_id(std::move(chat_id))
		{
			UAM_ASSERT(m_file != nullptr);
#if defined(_WIN32)
			m_session.stdin_write = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(m_file)));
			UAM_ASSERT(CreatePipe(&m_session.stdout_read, &m_stdout_write, nullptr, 0));
#else
			m_session.stdin_write_fd = fileno(m_file);
			int descriptors[2];
			UAM_ASSERT_EQ(pipe(descriptors), 0);
			m_session.stdout_read_fd = descriptors[0];
			m_stdout_write = descriptors[1];
			UAM_ASSERT(fcntl(descriptors[0], F_SETFL, O_NONBLOCK) >= 0);
#endif
			m_session.stdin_writer = std::make_shared<uam::platform::AsyncByteWriter>(
			    [this](const char* bytes, std::size_t count, std::string&) -> std::ptrdiff_t
			    {
				    const std::optional<ChatSession> saved = record_saved_cursor ? ChatRepository::LoadLocalChat(m_root, m_chat_id, true, nullptr, false) : std::nullopt;
				    const std::lock_guard lock(m_mutex);
				    m_writes.push_back({std::string(bytes, count), saved ? saved->remote_delivered_stdout_cursor : 0});
				    return static_cast<std::ptrdiff_t>(count);
			    }, [] {});
		}
		~RemoteHelperInput()
		{
			m_session.stdin_writer.reset();
#if defined(_WIN32)
			m_session.stdin_write = INVALID_HANDLE_VALUE;
			CloseHandle(m_session.stdout_read);
			CloseHandle(m_stdout_write);
			m_session.stdout_read = INVALID_HANDLE_VALUE;
#else
			m_session.stdin_write_fd = -1;
			close(m_session.stdout_read_fd);
			close(m_stdout_write);
			m_session.stdout_read_fd = -1;
#endif
			std::fclose(m_file);
		}
		struct Write { std::string bytes; std::uintmax_t saved_cursor = 0; };
		std::vector<Write> Writes() { const std::lock_guard lock(m_mutex); return m_writes; }
		// The session's input writer is asynchronous: an accepted acknowledgement reaches this sink shortly after.
		Write AwaitWrite(const std::string& bytes)
		{
			const Clock::time_point deadline = Clock::now() + std::chrono::seconds(10);
			for (;;)
			{
				for (const Write& write : Writes()) if (write.bytes == bytes) return write;
				UAM_ASSERT(Clock::now() < deadline);
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
		bool record_saved_cursor = false;
	private:
		uam::AcpSessionState& m_session;
		std::FILE* m_file;
		fs::path m_root;
		std::string m_chat_id;
		std::mutex m_mutex;
		std::vector<Write> m_writes;
#if defined(_WIN32)
		HANDLE m_stdout_write = INVALID_HANDLE_VALUE;
#else
		int m_stdout_write = -1;
#endif
	};

	// A chat running on an SSH host, mid-turn, whose helper delivers output in acknowledged batches.
	struct RemoteStallFixture : SaveStallFixture
	{
		explicit RemoteStallFixture(int messages = 2000, std::size_t bytes_each = 6 * 1024) : SaveStallFixture(messages, bytes_each)
		{
			Chat().execution_host_id = "remote-host";
			Chat().remote_process_exists = true;
			session->running = session->initialized = session->session_ready = true;
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, Chat()));
		}
		// One output batch: the provider's rows are applied, then the helper's marker asks for its acknowledgement.
		std::string DeliverBatch(int batch)
		{
			Chat().messages.back().content += " remote output " + std::to_string(batch);
			const std::string ack = "ack " + std::to_string(batch) + "\n";
			session->pending_remote_output_ack_line = ack;
			session->pending_remote_stdout_cursor = 100 * static_cast<std::uintmax_t>(batch + 1);
			return ack;
		}
		// Runs the poll-loop work (stdout drain, then the save flush) and returns the longest single poll.
		// With `durable_cursor`, checks the chat file the moment the drain decides to acknowledge.
		double PollUntilAcknowledged(double& waited_ms, std::optional<std::uintmax_t> durable_cursor = std::nullopt)
		{
			double longest = 0.0;
			const Clock::time_point start = Clock::now();
			while (!session->pending_remote_output_ack_line.empty() && Clock::now() - start < std::chrono::seconds(120))
			{
				const Clock::time_point poll = Clock::now();
				(void)uam::acp_detail::DrainStdout(app, *session, Chat(), nullptr);
				if (durable_cursor && session->pending_remote_output_ack_line.empty())
				{
					const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, "stall-chat", true, nullptr, false);
					UAM_ASSERT(saved && saved->remote_delivered_stdout_cursor == *durable_cursor);
				}
				uam::FlushPendingChatSaves(app);
				longest = std::max(longest, MsSince(poll));
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			UAM_ASSERT(session->pending_remote_output_ack_line.empty());
			waited_ms = MsSince(start);
			return longest;
		}
	};
}

// Remote (SSH) chats: tool/plan events and acknowledged output batches on a 2,000-message transcript.
UAM_TEST(RemoteChatSaveStallMeasurements)
{
	RemoteStallFixture fixture;
	Clock::time_point start = Clock::now();
	for (int update = 0; update < 20; ++update)
	{
		fixture.Chat().messages.back().content += " tool status";
		uam::acp_detail::SaveChatSoon(fixture.app, fixture.Chat());
	}
	std::fprintf(stderr, "[measure] remote chat, 2000 messages (12 MB): 20 tool/plan events held the calling thread %.1f ms (%.2f ms per event)\n", MsSince(start), MsSince(start) / 20.0);
	fixture.Drain();
	RemoteHelperInput helper(*fixture.session, fixture.app.data_root, "stall-chat");
	double longest = 0.0, waited = 0.0, waited_total = 0.0;
	for (int batch = 0; batch < 10; ++batch)
	{
		(void)fixture.DeliverBatch(batch);
		longest = std::max(longest, fixture.PollUntilAcknowledged(waited));
		waited_total += waited;
	}
	std::fprintf(stderr, "[measure] remote chat, 10 acknowledged output batches: longest single poll %.1f ms; mean marker-to-acknowledgement %.1f ms\n", longest, waited_total / 10.0);
	fixture.Drain();
	UAM_ASSERT_EQ(fixture.SavedTail(), fixture.Chat().messages.back().content);
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

UAM_TEST(ChatSaveReturnsItsRefusalOrWriteErrorCause)
{
	SaveStallFixture fixture(4, 64);
	ChatSession& chat = fixture.Chat();
	ChatSaveFailure failure{"stale", true};
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat, false, &failure));
	UAM_ASSERT(failure.reason.empty() && !failure.refused);

	chat.managed_followup_incarnation = "not-a-uuid";
	UAM_ASSERT(!ChatRepository::SaveChat(fixture.app.data_root, chat, false, &failure));
	UAM_ASSERT(failure.refused);
	UAM_ASSERT_EQ(failure.reason, std::string("content_not_storable"));
	chat.managed_followup_incarnation.clear();

	// A journal UAM cannot read is held for the user, never overwritten: a refusal, not a storage failure.
	const fs::path journal = fixture.app.data_root / "queue-outboxes" / "stall-chat.json";
	fs::create_directories(journal.parent_path());
	UAM_ASSERT(uam::io::WriteTextFile(journal, "{broken"));
	UAM_ASSERT(!ChatRepository::SaveChat(fixture.app.data_root, chat, false, &failure));
	UAM_ASSERT(failure.refused);
	UAM_ASSERT_EQ(failure.reason, std::string("queue_journal_unreadable"));
	fs::remove(journal);

	// A primary UAM cannot read (here a directory in its place) is a storage failure, not a refusal.
	const fs::path primary = AppPaths::UamChatFilePath(fixture.app.data_root, "stall-chat");
	fs::rename(primary, fixture.temporary.root / "preserved.json"); fs::create_directory(primary);
	UAM_ASSERT(!ChatRepository::SaveChat(fixture.app.data_root, chat, false, &failure));
	UAM_ASSERT(!failure.refused);
	UAM_ASSERT_EQ(failure.reason, std::string("primary_unreadable"));
	SavedChatPrimaryWitness witness;
	UAM_ASSERT(!ChatRepository::SaveLoadedChatWithPrimaryWitness(fixture.app.data_root, chat, witness, &failure));
	UAM_ASSERT(!failure.refused && failure.reason == "primary_unreadable");
	fs::remove(primary); fs::rename(fixture.temporary.root / "preserved.json", primary);
	UAM_ASSERT(ChatRepository::SaveLoadedChatWithPrimaryWitness(fixture.app.data_root, chat, witness, &failure));
	UAM_ASSERT(failure.reason.empty());
#if !defined(_WIN32)
	// A failed write carries the atomic writer's own text (the step and OS error), never a path.
	{
		struct FileSizeLimit
		{
			rlimit previous{};
			void (*previous_handler)(int) = SIG_DFL;
			FileSizeLimit()
			{
				UAM_ASSERT(getrlimit(RLIMIT_FSIZE, &previous) == 0);
				previous_handler = std::signal(SIGXFSZ, SIG_IGN);
				rlimit small = previous;
				small.rlim_cur = 16;
				UAM_ASSERT(setrlimit(RLIMIT_FSIZE, &small) == 0);
			}
			~FileSizeLimit() { (void)setrlimit(RLIMIT_FSIZE, &previous); std::signal(SIGXFSZ, previous_handler); }
		} limit;
		chat.messages.back().content += " grows past the limit";
		UAM_ASSERT(!ChatRepository::SaveChat(fixture.app.data_root, chat, false, &failure));
	}
	UAM_ASSERT(!failure.refused);
	UAM_ASSERT(failure.reason.rfind("write: Failed to write temporary file: ", 0) == 0);
	UAM_ASSERT(failure.reason.find(fixture.app.data_root.filename().string()) == std::string::npos);
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat, false, &failure));
#endif
}

UAM_TEST(BackgroundChatSavePreparationReturnsItsCause)
{
	// The streaming save prepares on a worker; its refusal must reach the log as a refusal, not a storage failure.
	SaveStallFixture fixture(4, 64);
	const fs::path journal = fixture.app.data_root / "queue-outboxes" / "stall-chat.json";
	fs::create_directories(journal.parent_path());
	UAM_ASSERT(uam::io::WriteTextFile(journal, "{broken"));
	ChatSaveFailure failure;
	UAM_ASSERT(ChatRepository::PrepareChatSave(fixture.app.data_root, fixture.Chat(), {}, &failure) == nullptr);
	UAM_ASSERT(failure.refused && failure.reason == "queue_journal_unreadable");
	fs::remove(journal);
	UAM_ASSERT(ChatRepository::PrepareChatSave(fixture.app.data_root, fixture.Chat(), "not-the-revision", &failure) == nullptr);
	UAM_ASSERT(failure.refused && failure.reason == "primary_changed_since_capture");
	UAM_ASSERT(ChatRepository::PrepareChatSave(fixture.app.data_root, fixture.Chat(), {}, &failure) != nullptr);
	UAM_ASSERT(failure.reason.empty());
}

UAM_TEST(ChatSaveFailureLogsOneLineWithItsCauseAtDurableBarrier)
{
	SaveStallFixture fixture(4, 64);
	std::string error;
	UAM_ASSERT(uam::diagnostics::StartFileLog(fixture.temporary.root / "uam.log", error, 0));
	struct StopLog { ~StopLog() { uam::diagnostics::StopFileLog(); } } stop_log;
	const fs::path primary = AppPaths::UamChatFilePath(fixture.app.data_root, "stall-chat");
	fs::rename(primary, fixture.temporary.root / "preserved.json"); fs::create_directory(primary);
	fixture.Chat().messages.back().content += " unsaved output";
	// Not yet due, so only the quit barrier attempts it: the case that logged two reasonless lines per chat.
	uam::acp_detail::ScheduleChatSave(fixture.app, fixture.Chat(), 100.0);
	uam::FlushPendingChatSaves(fixture.app, true);
	uam::FlushPendingChatSaves(fixture.app, true); // Quit runs two forced barriers; the second must stay quiet.
	uam::diagnostics::FlushFileLog();
	const std::string text = uam::io::ReadTextFile(fixture.temporary.root / "uam.log");
	std::size_t lines = 0;
	for (std::size_t at = text.find("chat_id=stall-chat"); at != std::string::npos; at = text.find("chat_id=stall-chat", at + 1)) ++lines;
	UAM_ASSERT_EQ(lines, std::size_t{1});
	UAM_ASSERT(uam::strings::Contains(text, "Chat save failed at the durable barrier: chat_id=stall-chat, reason=primary_unreadable; the chat stays unsaved until a later save succeeds."));
	UAM_ASSERT(!uam::strings::Contains(text, "Chat history was not saved"));
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.contains("stall-chat"));
	fs::remove(primary); fs::rename(fixture.temporary.root / "preserved.json", primary);

	const std::string refused = uam::acp_detail::ChatSaveFailureLogLine("held-chat", {"queue_journal_revision_changed", true}, false);
	UAM_ASSERT_EQ(refused, std::string("Chat save refused before writing: chat_id=held-chat, reason=queue_journal_revision_changed; retrying with backoff up to 60 s while UAM runs."));
	UAM_ASSERT(uam::strings::Contains(uam::acp_detail::ChatSaveFailureLogLine("chat", {}, false), "Chat save failed: chat_id=chat, reason=unclassified;"));
	UAM_ASSERT(uam::acp_detail::ChatSaveFailureLogLine("chat", {std::string(5000, 'x'), false}, false).size() < 400);
	UAM_ASSERT(uam::strings::Contains(uam::acp_detail::ChatSaveFailureLogLine("../outside", {"content_not_storable", true}, false), "chat_id=unsafe chat id,"));
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

UAM_TEST(RemoteToolUpdatesDoNotWriteTranscriptOnCallingThread)
{
	RemoteStallFixture fixture;
	fixture.session->next_request_id = 41;
	Clock::time_point start = Clock::now();
	for (int update = 0; update < 20; ++update)
	{
		fixture.Chat().messages.back().content += " tool status";
		uam::acp_detail::SaveChatSoon(fixture.app, fixture.Chat());
	}
	const double calling_thread = MsSince(start);
	std::fprintf(stderr, "[measure] 20 tool-call updates on a remote 2000-message (12 MB) chat held the calling thread %.2f ms\n", calling_thread);
	// The same fixed budget as the local case: scheduling is independent of transcript size.
	UAM_ASSERT(calling_thread <= 100.0);
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.contains("stall-chat"));
	// A crash now loses nothing the helper cannot replay: none of this output has been acknowledged.
	UAM_ASSERT(fixture.SavedTail() != fixture.Chat().messages.back().content);
	fixture.Drain();
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, "stall-chat", true, nullptr, false);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->messages.back().content, fixture.Chat().messages.back().content);
	// The background write carries the runtime state the synchronous save records for a remote chat.
	UAM_ASSERT_EQ(saved->remote_next_request_id, 41);
	UAM_ASSERT(saved->remote_process_exists);
}

// A remote helper's output batch is acknowledged only after a background save holding its delivered cursor is
// on disk; the poll that handles the batch never writes the transcript itself.
UAM_TEST(RemoteOutputAckFollowsDurableSaveOffPoll)
{
	RemoteStallFixture fixture;
	// Calibration: the blocking work this guards against, a synchronous save of the fixture chat (median of three).
	std::vector<double> saves;
	for (int run = 0; run < 3; ++run)
	{
		const Clock::time_point start = Clock::now();
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.Chat()));
		saves.push_back(MsSince(start));
	}
	std::ranges::sort(saves);
	RemoteHelperInput helper(*fixture.session, fixture.app.data_root, "stall-chat");
	helper.record_saved_cursor = true;
	std::uintmax_t acknowledged_cursor = 0;
	for (int batch = 0; batch < 3; ++batch)
	{
		const std::string ack = fixture.DeliverBatch(batch);
		const std::uintmax_t cursor = fixture.session->pending_remote_stdout_cursor;
		const Clock::time_point start = Clock::now();
		(void)uam::acp_detail::DrainStdout(fixture.app, *fixture.session, fixture.Chat(), nullptr);
		const double poll = MsSince(start);
		std::fprintf(stderr, "[measure] remote output batch poll %.2f ms; synchronous save median %.1f ms\n", poll, saves[1]);
		// Per-poll budget below one unit of the guarded work (half the cheapest calibration save), so a synchronous save fails it.
		UAM_ASSERT(poll < saves.front() / 2.0);
		// Simulated crash point: the batch is applied in memory but not durable and not acknowledged, so a
		// restart reads the previous cursor and the helper replays this batch.
		UAM_ASSERT_EQ(fixture.session->pending_remote_output_ack_line, ack);
		const std::optional<ChatSession> restarted = ChatRepository::LoadLocalChat(fixture.app.data_root, "stall-chat", true, nullptr, false);
		UAM_ASSERT(restarted && restarted->remote_delivered_stdout_cursor == acknowledged_cursor);
		if (batch == 1)
		{
			// A durable barrier takes over the pending save; the acknowledgement still waits for its own certified write.
			uam::FlushPendingChatSaves(fixture.app, true);
			(void)uam::acp_detail::DrainStdout(fixture.app, *fixture.session, fixture.Chat(), nullptr);
			UAM_ASSERT_EQ(fixture.session->pending_remote_output_ack_line, ack);
		}
		double waited = 0.0;
		(void)fixture.PollUntilAcknowledged(waited, cursor);
		// The chat file already held this batch's cursor when the acknowledgement reached the helper.
		UAM_ASSERT_EQ(helper.AwaitWrite(ack).saved_cursor, cursor);
		acknowledged_cursor = cursor;
	}
	UAM_ASSERT_EQ(helper.Writes().size(), std::size_t{3});
	fixture.Drain();
	UAM_ASSERT_EQ(fixture.SavedTail(), fixture.Chat().messages.back().content);
	// Outside an active turn the synchronous save and acknowledgement still happen in the same poll.
	fixture.session->processing = false;
	const std::string settled = fixture.DeliverBatch(3);
	(void)uam::acp_detail::DrainStdout(fixture.app, *fixture.session, fixture.Chat(), nullptr);
	UAM_ASSERT(fixture.session->pending_remote_output_ack_line.empty());
	UAM_ASSERT_EQ(helper.AwaitWrite(settled).saved_cursor, std::uintmax_t{400});
	UAM_ASSERT_EQ(helper.Writes().size(), std::size_t{4});
}

// A snapshot taken after the acknowledgement's save was scheduled but without its cursors (a reset moved them)
// can never certify it: the ticket is lost, the drain decides again, and the chat does not keep rewriting.
UAM_TEST(RemoteOutputAckSaveWhoseCursorsMovedIsDecidedAgain)
{
	RemoteStallFixture fixture(200, 512);
	RemoteHelperInput helper(*fixture.session, fixture.app.data_root, "stall-chat");
	const std::string ack = fixture.DeliverBatch(0);
	(void)uam::acp_detail::DrainStdout(fixture.app, *fixture.session, fixture.Chat(), nullptr);
	UAM_ASSERT(fixture.session->remote_output_ack_save != nullptr);
	fixture.Chat().remote_delivered_stdout_cursor = 0;
	const Clock::time_point deadline = Clock::now() + std::chrono::seconds(60);
	while (uam::PollRemoteOutputAckSave(fixture.session->remote_output_ack_save) == std::nullopt && Clock::now() < deadline)
	{
		uam::FlushPendingChatSaves(fixture.app); std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT(uam::PollRemoteOutputAckSave(fixture.session->remote_output_ack_save) == std::optional<bool>(false));
	UAM_ASSERT(fixture.session->pending_remote_output_ack_line == ack);
	double waited = 0.0;
	(void)fixture.PollUntilAcknowledged(waited, std::uintmax_t{100});
	UAM_ASSERT_EQ(helper.AwaitWrite(ack).bytes, ack);
	fixture.Drain();
}
