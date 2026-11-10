#include "test_harness.h"
#include "common/chat/chat_repository.h"
#include "common/runtime/acp/acp_session_internal.h"

using namespace uam_test;

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
			uam::acp_detail::ScheduleChatSave(fixture.app, fixture.Chat(), 0.5);
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
