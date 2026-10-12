#include "test_harness.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/paths/app_paths.h"

using namespace uam_test;

namespace
{
	struct PromptFixture
	{
		TempDir temp{"uam-async-send-durability"};
		uam::AppState app;
		IPlatformProcessService& process = PlatformServicesFactory::Instance().process_service;
		uam::AcpSessionState* session = nullptr;
		fs::path saved_path;
		std::string original;

		PromptFixture()
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = {ProviderProfileStore::DefaultGeminiProfile()};
			ChatSession chat;
			chat.id = "async-send";
			chat.provider_id = "gemini-cli";
			chat.workspace_directory = temp.root.string();
			chat.approval_mode.clear();
			chat.memory_enabled = false;
			chat.messages.push_back({.role = MessageRole::User, .content = "Previously saved history"});
			app.chats.push_back(std::move(chat));
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
			saved_path = AppPaths::UamChatFilePath(app.data_root, app.chats.front().id);
			original = ReadFile(saved_path);
		
			std::unique_ptr<uam::AcpSessionState> owned = std::make_unique<uam::AcpSessionState>();
			owned->chat_id = app.chats.front().id;
			owned->provider_id = app.chats.front().provider_id;
			owned->session_id = "private-fake-session";
			owned->session_ready = true;
			owned->current_mode_id = "default";
			session = owned.get();
			app.acp_sessions.push_back(std::move(owned));
		#if defined(_WIN32)
			const std::vector<std::string> sink = {"cmd", "/C", "more > NUL"};
		#else
			const std::vector<std::string> sink = {"/bin/sh", "-c", "cat >/dev/null"};
		#endif
			std::string error;
			UAM_ASSERT(process.StartStdioProcess(*session, temp.root, sink, &error));
			session->running = true;
			session->session_ready = true;
		}
		~PromptFixture()
		{
			if (session != nullptr) { process.StopStdioProcess(*session, true); process.CloseStdioProcessHandles(*session); }
		}
	};
}

UAM_TEST(AcpSendReturnsPendingBeforeDurablePublicationAndProviderInput)
{
	PromptFixture fixture;
	uam::AppState& app = fixture.app;
	uam::AcpSessionState* session = fixture.session;
	const fs::path& saved_path = fixture.saved_path;
	const std::string& original = fixture.original;
	std::string error;
	const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
	UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Accepted local prompt", &error));
	const std::chrono::milliseconds elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
	UAM_ASSERT(elapsed <= std::chrono::milliseconds(100));
	// Owner polling must authorize publication before any provider input is released.
	UAM_ASSERT_EQ(ReadFile(saved_path), original);
	UAM_ASSERT_EQ(session->prompt_request_id, 0);
	UAM_ASSERT(session->queued_prompt.find("Accepted local prompt") != std::string::npos);
	UAM_ASSERT(session->processing);
	UAM_ASSERT(app.pending_chat_save_at_by_chat_id.contains(app.chats.front().id));
	std::optional<bool> acknowledgement;
	UAM_ASSERT(uam::DeferChatSaveResult(app, app.chats.front().id, [&acknowledgement](bool saved) { acknowledgement = saved; }));
	UAM_ASSERT(!acknowledgement.has_value());
	UAM_ASSERT(!RemoveChatById(app, app.chats.front().id));
	UAM_ASSERT(app.status_line.find("finish saving") != std::string::npos);
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (app.pending_chat_save_at_by_chat_id.contains(app.chats.front().id) && std::chrono::steady_clock::now() < deadline)
	{
		uam::FlushPendingChatSaves(app);
		std::this_thread::yield();
	}
	UAM_ASSERT(!app.pending_chat_save_at_by_chat_id.contains(app.chats.front().id));
	UAM_ASSERT(acknowledgement.has_value() && *acknowledgement);
	UAM_ASSERT(uam::acp_detail::SendQueuedPromptIfReady(app, *session, app.chats.front()));
	UAM_ASSERT(session->prompt_request_id != 0);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, app.chats.front().id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->messages.size(), std::size_t{2});
	UAM_ASSERT_EQ(saved->messages.front().content, std::string("Previously saved history"));
	UAM_ASSERT_EQ(saved->messages.back().content, std::string("Accepted local prompt"));
	UAM_ASSERT(!saved->messages.back().acp_prompt_not_sent);
}

UAM_TEST(AcpLargeUnsavedHistoryIsDurableBeforePromptDispatch)
{
	PromptFixture fixture;
	uam::AppState& app = fixture.app;
	for (int index = 1; index < 200; ++index)
		app.chats.front().messages.push_back({.role = MessageRole::Assistant,
		    .content = std::to_string(index) + std::string(8192, 'x')});
	const std::string last_unsaved = app.chats.front().messages.back().content;
	std::string error;
	const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
	UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Preserve unsaved history", &error));
	UAM_ASSERT(std::chrono::steady_clock::now() - started <= std::chrono::milliseconds(100));
	UAM_ASSERT_EQ(ReadFile(fixture.saved_path), fixture.original);
	UAM_ASSERT_EQ(fixture.session->prompt_request_id, 0);
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (app.pending_chat_save_at_by_chat_id.contains(app.chats.front().id) && std::chrono::steady_clock::now() < deadline)
	{
		uam::FlushPendingChatSaves(app);
		std::this_thread::yield();
	}
	UAM_ASSERT(!app.pending_chat_save_at_by_chat_id.contains(app.chats.front().id));
	// Inspect storage before invoking delivery: unsaved bodies must already be durable.
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, app.chats.front().id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->messages.size(), std::size_t{201});
	UAM_ASSERT_EQ(saved->messages[199].content, last_unsaved);
	UAM_ASSERT_EQ(saved->messages.back().content, std::string("Preserve unsaved history"));
	UAM_ASSERT(saved->messages.back().acp_prompt_not_sent);
	UAM_ASSERT(uam::acp_detail::SendQueuedPromptIfReady(app, *fixture.session, app.chats.front()));
	UAM_ASSERT(fixture.session->prompt_request_id != 0);
}

UAM_TEST(PreparedChatSaveOwnershipPreventsMissingFileResurrection)
{
	TempDir temp("uam-async-save-ownership");
	ChatSession old;
	old.id = "unpublished-chat";
	old.provider_id = "gemini-cli";
	old.messages.push_back({.role = MessageRole::User, .content = "Old unpublished intent"});
	const std::shared_ptr<PreparedChatSave> prepared = ChatRepository::PrepareChatSave(temp.root, old, "missing");
	UAM_ASSERT(prepared != nullptr);
	ChatSession newer = old;
	newer.messages.front().content = "New owner";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, newer));
	UAM_ASSERT(!ChatRepository::DeleteChatStorageFiles(temp.root, newer.id).Failed());
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, old.id);
	UAM_ASSERT(!fs::exists(primary));
	bool ownership_checked = false;
	UAM_ASSERT(!ChatRepository::PublishPreparedChatSave(prepared, true, nullptr, [&ownership_checked]()
	{
		ownership_checked = true;
		return false;
	}));
	UAM_ASSERT(ownership_checked);
	UAM_ASSERT(!fs::exists(primary));
}

UAM_TEST(AsyncChatSaveAcknowledgementReportsStorageFailure)
{
	TempDir temp("uam-async-save-failure");
	uam::AppState app;
	app.data_root = temp.root / "blocked";
	UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "Not a directory"));
	ChatSession chat;
	chat.id = "failed-save";
	chat.messages.push_back({.role = MessageRole::User, .content = "Retained unsent intent"});
	app.chats.push_back(chat);
	uam::acp_detail::ScheduleChatSave(app, app.chats.front(), 0.0);
	std::optional<bool> acknowledgement;
	UAM_ASSERT(uam::DeferChatSaveResult(app, chat.id, [&acknowledgement](bool saved) { acknowledgement = saved; }));
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!acknowledgement.has_value() && std::chrono::steady_clock::now() < deadline)
	{
		uam::FlushPendingChatSaves(app);
		std::this_thread::yield();
	}
	UAM_ASSERT(acknowledgement.has_value() && !*acknowledgement);
	UAM_ASSERT_EQ(app.chats.front().messages.front().content, std::string("Retained unsent intent"));
}

UAM_TEST(AsyncChatSaveAcknowledgementCanScheduleNewOwnerWork)
{
	TempDir temp("uam-async-save-callback");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "callback-save";
	chat.title = "Original title";
	app.chats.push_back(chat);
	uam::acp_detail::ScheduleChatSave(app, app.chats.front(), 0.0);
	int callbacks = 0;
	UAM_ASSERT(uam::DeferChatSaveResult(app, chat.id, [&app, &callbacks](bool saved)
	{
		UAM_ASSERT(saved);
		++callbacks;
		app.chats.front().title = "New title from callback";
		uam::acp_detail::ScheduleChatSave(app, app.chats.front(), 0.0);
	}));
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (app.pending_chat_save_at_by_chat_id.contains(chat.id) && std::chrono::steady_clock::now() < deadline)
	{
		uam::FlushPendingChatSaves(app);
		std::this_thread::yield();
	}
	UAM_ASSERT_EQ(callbacks, 1);
	UAM_ASSERT(!app.pending_chat_save_at_by_chat_id.contains(chat.id));
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->title, std::string("New title from callback"));
}

UAM_TEST(AcpAsyncSendFailureRejectsAcknowledgementAndKeepsIntentUnsent)
{
	PromptFixture fixture;
	uam::AppState& app = fixture.app;
	app.data_root = fixture.temp.root / "blocked-storage";
	UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "Not a directory"));
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Cannot persist this prompt", &error));
	std::optional<bool> acknowledgement;
	UAM_ASSERT(uam::DeferChatSaveResult(app, app.chats.front().id, [&acknowledgement](bool saved) { acknowledgement = saved; }));
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	bool state_changed = false;
	while (!acknowledgement.has_value() && std::chrono::steady_clock::now() < deadline)
	{
		state_changed = uam::FlushPendingChatSaves(app) || state_changed;
		std::this_thread::yield();
	}
	UAM_ASSERT(acknowledgement.has_value() && !*acknowledgement);
	UAM_ASSERT(state_changed);
	UAM_ASSERT_EQ(fixture.session->prompt_request_id, 0);
	UAM_ASSERT(!fixture.session->processing);
	UAM_ASSERT(fixture.session->queued_prompt.empty());
	UAM_ASSERT(!fixture.session->last_error.empty());
	UAM_ASSERT(app.chats.front().messages.back().acp_prompt_not_sent);
	UAM_ASSERT(app.chats.front().messages.back().interrupted);
	UAM_ASSERT_EQ(ReadFile(fixture.saved_path), fixture.original);
}
