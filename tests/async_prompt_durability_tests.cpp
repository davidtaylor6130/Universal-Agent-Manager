#include "test_harness.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/paths/app_paths.h"
#include <future>

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
	void AwaitSave(uam::AppState& app)
	{
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!app.pending_chat_save_at_by_chat_id.empty() && std::chrono::steady_clock::now() < deadline)
		{
			uam::FlushPendingChatSaves(app);
			std::this_thread::yield();
		}
		UAM_ASSERT(app.pending_chat_save_at_by_chat_id.empty());
	}
	void AwaitDelivery(uam::AppState& app, uam::AcpSessionState& session)
	{
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (session.processing && session.prompt_request_id == 0 && std::chrono::steady_clock::now() < deadline)
		{
			uam::FlushPendingChatSaves(app);
			(void)uam::acp_detail::SendQueuedPromptIfReady(app, session, app.chats.front());
			std::this_thread::yield();
		}
	}
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
	AwaitDelivery(app, *session);
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
	AwaitDelivery(app, *fixture.session);
	UAM_ASSERT(fixture.session->prompt_request_id != 0);
}

UAM_TEST(AcpDeliveryMarkerHoldsOneRequestUntilItsOwnedSaveReceipt)
{
	PromptFixture fixture;
	uam::AppState& app = fixture.app;
	uam::AcpSessionState& session = *fixture.session;
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "One immutable request", &error));
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (app.pending_chat_save_at_by_chat_id.contains(app.chats.front().id) && std::chrono::steady_clock::now() < deadline)
	{
		uam::FlushPendingChatSaves(app);
		std::this_thread::yield();
	}
	UAM_ASSERT(!app.pending_chat_save_at_by_chat_id.contains(app.chats.front().id));
	const std::string admitted = ReadFile(fixture.saved_path);
	const int request_id = session.next_request_id;
	(void)uam::acp_detail::SendQueuedPromptIfReady(app, session, app.chats.front());
	UAM_ASSERT_EQ(session.prompt_request_id, 0);
	UAM_ASSERT_EQ(ReadFile(fixture.saved_path), admitted);
	UAM_ASSERT(app.pending_chat_save_at_by_chat_id.contains(app.chats.front().id));
	const int allocated_next_id = session.next_request_id;
	UAM_ASSERT_EQ(allocated_next_id, request_id + 1);
	for (int attempt = 0; attempt < 10; ++attempt)
		(void)uam::acp_detail::SendQueuedPromptIfReady(app, session, app.chats.front());
	UAM_ASSERT_EQ(session.next_request_id, allocated_next_id);
	UAM_ASSERT_EQ(session.prompt_request_id, 0);
	while (app.pending_chat_save_at_by_chat_id.contains(app.chats.front().id) && std::chrono::steady_clock::now() < deadline)
	{
		uam::FlushPendingChatSaves(app);
		std::this_thread::yield();
	}
	UAM_ASSERT(!app.pending_chat_save_at_by_chat_id.contains(app.chats.front().id));
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, app.chats.front().id);
	UAM_ASSERT(saved.has_value() && !saved->messages.back().acp_prompt_not_sent);
	UAM_ASSERT_EQ(session.prompt_request_id, 0);
	(void)uam::acp_detail::SendQueuedPromptIfReady(app, session, app.chats.front());
	UAM_ASSERT_EQ(session.prompt_request_id, request_id);
	UAM_ASSERT_EQ(session.next_request_id, allocated_next_id);
	const int delivered = session.prompt_request_id;
	(void)uam::acp_detail::SendQueuedPromptIfReady(app, session, app.chats.front());
	UAM_ASSERT_EQ(session.prompt_request_id, delivered);
	UAM_ASSERT_EQ(session.next_request_id, allocated_next_id);
}

UAM_TEST(AcpCanceledStagedDeliveryRetainsUnsentIntentWithoutProviderPrompt)
{
	PromptFixture fixture;
	uam::AppState& app = fixture.app;
	uam::AcpSessionState& session = *fixture.session;
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Cancel before delivery", &error));
	AwaitSave(app);
	(void)uam::acp_detail::SendQueuedPromptIfReady(app, session, app.chats.front());
	UAM_ASSERT(session.pending_prompt_delivery != nullptr);
	UAM_ASSERT_EQ(session.prompt_request_id, 0);
	UAM_ASSERT(uam::CancelAcpTurn(app, app.chats.front().id, &error));
	AwaitSave(app);
	(void)uam::acp_detail::SendQueuedPromptIfReady(app, session, app.chats.front());
	UAM_ASSERT(session.pending_prompt_delivery == nullptr);
	UAM_ASSERT_EQ(session.prompt_request_id, 0);
	UAM_ASSERT(app.chats.front().messages.back().acp_prompt_not_sent);
	UAM_ASSERT(app.chats.front().messages.back().interrupted);
	UAM_ASSERT(std::ranges::none_of(session.diagnostics, [](const uam::AcpDiagnosticEntryState& diagnostic)
	    { return diagnostic.method == "session/prompt" && diagnostic.reason == "sent"; }));
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, app.chats.front().id);
	UAM_ASSERT(saved.has_value() && saved->messages.back().acp_prompt_not_sent);
}

UAM_TEST(AcpStagedDeliverySaveFailureRestoresUnsentRetryAffordance)
{
	PromptFixture fixture;
	uam::AppState& app = fixture.app;
	uam::AcpSessionState& session = *fixture.session;
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Retain failed delivery", &error));
	AwaitSave(app);
	const std::string admitted = ReadFile(fixture.saved_path);
	app.data_root = fixture.temp.root / "blocked-marker";
	UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "Blocked storage"));
	(void)uam::acp_detail::SendQueuedPromptIfReady(app, session, app.chats.front());
	UAM_ASSERT(session.pending_prompt_delivery != nullptr);
	AwaitDelivery(app, session);
	UAM_ASSERT_EQ(session.prompt_request_id, 0);
	UAM_ASSERT(!session.processing);
	UAM_ASSERT(session.pending_prompt_delivery == nullptr);
	UAM_ASSERT(app.chats.front().messages.back().acp_prompt_not_sent);
	UAM_ASSERT(app.chats.front().messages.back().interrupted);
	UAM_ASSERT(!session.last_error.empty());
	UAM_ASSERT_EQ(ReadFile(fixture.saved_path), admitted);
}

UAM_TEST(AcpStagedReceiptCannotDeliverIntoANewerTurn)
{
	PromptFixture fixture;
	uam::AppState& app = fixture.app;
	uam::AcpSessionState& session = *fixture.session;
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Old turn intent", &error));
	AwaitSave(app);
	(void)uam::acp_detail::SendQueuedPromptIfReady(app, session, app.chats.front());
	const int next_id = session.next_request_id;
	++session.turn_serial;
	session.queued_prompt = "New turn intent";
	app.chats.front().messages.push_back({.role = MessageRole::User, .content = "New turn intent", .acp_prompt_not_sent = true});
	session.turn_first_user_message_index = session.turn_user_message_index = 2;
	uam::acp_detail::ScheduleChatSave(app, app.chats.front(), 0.0);
	AwaitSave(app);
	(void)uam::acp_detail::SendQueuedPromptIfReady(app, session, app.chats.front());
	UAM_ASSERT_EQ(session.prompt_request_id, 0);
	UAM_ASSERT_EQ(session.next_request_id, next_id);
	UAM_ASSERT(session.processing);
	UAM_ASSERT_EQ(session.queued_prompt, std::string("New turn intent"));
	UAM_ASSERT(app.chats.front().messages[1].acp_prompt_not_sent);
	UAM_ASSERT(app.chats.front().messages[2].acp_prompt_not_sent);
	UAM_ASSERT(session.pending_prompt_delivery == nullptr);
}

UAM_TEST(AcpStagedDeliveryRejectsChangedMemoryPolicy)
{
	PromptFixture fixture;
	uam::AppState& app = fixture.app;
	uam::AcpSessionState& session = *fixture.session;
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Respect changed context", &error));
	AwaitSave(app);
	(void)uam::acp_detail::SendQueuedPromptIfReady(app, session, app.chats.front());
	app.chats.front().memory_level = "open";
	app.chats.front().memory_enabled = true;
	uam::acp_detail::ScheduleChatSave(app, app.chats.front(), 0.0);
	AwaitSave(app);
	(void)uam::acp_detail::SendQueuedPromptIfReady(app, session, app.chats.front());
	UAM_ASSERT_EQ(session.prompt_request_id, 0);
	UAM_ASSERT(!session.processing);
	UAM_ASSERT(session.pending_prompt_delivery == nullptr);
	UAM_ASSERT(session.last_error.find("ownership changed") != std::string::npos);
	UAM_ASSERT(app.chats.front().messages.back().acp_prompt_not_sent);
	UAM_ASSERT(app.chats.front().messages.back().interrupted);
	AwaitSave(app);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, app.chats.front().id);
	UAM_ASSERT(saved.has_value() && saved->messages.back().acp_prompt_not_sent);
}

UAM_TEST(FailedSynchronousSaveRevokesPendingAdmissionBeforeRejectingItsReceipt)
{
	PromptFixture fixture;
	uam::AppState& app = fixture.app;
	uam::AcpSessionState& session = *fixture.session;
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Do not revive rejected admission", &error));
	bool rejected = false;
	UAM_ASSERT(uam::DeferChatSaveResult(app, app.chats.front().id, [&session, &rejected](bool saved)
	{
		UAM_ASSERT(!saved);
		UAM_ASSERT(!session.processing);
		UAM_ASSERT(session.queued_prompt.empty());
		rejected = true;
	}));
	const fs::path root = app.data_root;
	app.data_root = fixture.temp.root / "blocked-synchronous-save";
	UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "Blocked storage"));
	UAM_ASSERT(!uam::acp_detail::SaveChatQuietly(app, app.chats.front()));
	UAM_ASSERT(rejected);
	UAM_ASSERT_EQ(session.prompt_request_id, 0);
	UAM_ASSERT(app.chats.front().messages.back().interrupted);
	UAM_ASSERT(app.chats.front().messages.back().acp_prompt_not_sent);
	UAM_ASSERT(!session.last_turn_error.empty());
	app.data_root = root;
	uam::acp_detail::ScheduleChatSave(app, app.chats.front(), 0.0);
	AwaitSave(app);
	(void)uam::acp_detail::SendQueuedPromptIfReady(app, session, app.chats.front());
	UAM_ASSERT_EQ(session.prompt_request_id, 0);
	UAM_ASSERT(!session.processing);
}

UAM_TEST(AcpGoalRevisionRetiresStagedPacketAndPreservesUnsentIntent)
{
	PromptFixture fixture;
	uam::AppState& app = fixture.app;
	uam::AcpSessionState& session = *fixture.session;
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Stop obsolete goal delivery", &error));
	AwaitSave(app);
	session.goal_turn_kind = "worker";
	session.goal_command_revision = app.chats.front().goal_command_revision = "original-command";
	(void)uam::acp_detail::SendQueuedPromptIfReady(app, session, app.chats.front());
	UAM_ASSERT(session.pending_prompt_delivery != nullptr);
	app.chats.front().goal_command_revision = "new-command";
	uam::acp_detail::ScheduleChatSave(app, app.chats.front(), 0.0);
	AwaitSave(app);
	const int next_id = session.next_request_id;
	(void)uam::acp_detail::SendQueuedPromptIfReady(app, session, app.chats.front());
	UAM_ASSERT_EQ(session.prompt_request_id, 0);
	UAM_ASSERT_EQ(session.next_request_id, next_id);
	UAM_ASSERT(!session.processing);
	UAM_ASSERT(session.pending_prompt_delivery == nullptr);
	UAM_ASSERT(app.chats.front().messages.back().acp_prompt_not_sent);
	AwaitSave(app);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, app.chats.front().id);
	UAM_ASSERT(saved.has_value() && saved->messages.back().acp_prompt_not_sent);
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

UAM_TEST(AcpBusyQueueAdmissionReturnsWithinInputBudgetUnderStorageContention)
{
	PromptFixture fixture;
	uam::AppState& app = fixture.app;
	uam::AcpSessionState& session = *fixture.session;
	session.processing = true;
	session.prompt_request_id = 10;
	session.next_request_id = 11;
	session.queued_prompt = "Existing delivered turn";
	const std::shared_ptr<PreparedChatSave> prepared = ChatRepository::PrepareChatSave(app.data_root, app.chats.front());
	UAM_ASSERT(prepared != nullptr);
	std::promise<void> entered;
	std::future<void> locked = entered.get_future();
	std::jthread publication([&entered, prepared]()
	{
		(void)ChatRepository::PublishPreparedChatSave(prepared, true, nullptr, [&entered]()
		{
			entered.set_value();
			std::this_thread::sleep_for(std::chrono::milliseconds(350));
			return false;
		});
	});
	UAM_ASSERT(locked.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
	std::string error;
	const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
	const bool admitted = uam::SendAcpPrompt(app, app.chats.front().id, "Queued while history publication is busy", &error);
	const std::chrono::steady_clock::duration elapsed = std::chrono::steady_clock::now() - started;
	publication.join();
	UAM_ASSERT(admitted);
	UAM_ASSERT(elapsed <= std::chrono::milliseconds(100));
	UAM_ASSERT_EQ(session.prompt_request_id, 10);
	UAM_ASSERT_EQ(session.queued_user_prompts.size(), std::size_t{1});
	UAM_ASSERT(app.pending_chat_save_at_by_chat_id.contains(app.chats.front().id));
	std::optional<bool> acknowledgement;
	UAM_ASSERT(uam::DeferChatSaveResult(app, app.chats.front().id, [&acknowledgement](bool saved) { acknowledgement = saved; }));
	session.processing = false;
	session.prompt_request_id = 0;
	session.queued_prompt.clear();
	UAM_ASSERT(!uam::DrainNextQueuedAcpUserPrompt(app, session, app.chats.front()));
	UAM_ASSERT(!acknowledgement.has_value());
	AwaitSave(app);
	UAM_ASSERT(acknowledgement.has_value() && *acknowledgement);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, app.chats.front().id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->messages.size(), std::size_t{1});
	UAM_ASSERT_EQ(saved->acp_queued_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(saved->acp_queued_prompts.front().text, std::string("Queued while history publication is busy"));
}

UAM_TEST(AcpFailedBusyQueueSaveHoldsIntentWithoutInterruptingDeliveredTurn)
{
	PromptFixture fixture;
	uam::AppState& app = fixture.app;
	uam::AcpSessionState& session = *fixture.session;
	session.processing = true;
	session.prompt_request_id = 10;
	session.queued_prompt = "Existing delivered turn";
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Queue intent survives storage failure", &error));
	std::optional<bool> acknowledgement;
	UAM_ASSERT(uam::DeferChatSaveResult(app, app.chats.front().id, [&acknowledgement](bool saved) { acknowledgement = saved; }));
	fs::remove_all(app.data_root);
	UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "private blocked data root"));
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!acknowledgement.has_value() && std::chrono::steady_clock::now() < deadline)
	{
		uam::FlushPendingChatSaves(app);
		std::this_thread::yield();
	}
	UAM_ASSERT(acknowledgement.has_value() && !*acknowledgement);
	UAM_ASSERT(session.processing);
	UAM_ASSERT_EQ(session.prompt_request_id, 10);
	UAM_ASSERT_EQ(session.queued_prompt, std::string("Existing delivered turn"));
	UAM_ASSERT(session.last_error.empty());
	UAM_ASSERT(session.queue_held && app.chats.front().acp_queue_held);
	UAM_ASSERT_EQ(session.queued_user_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(session.queued_user_prompts.front().text, std::string("Queue intent survives storage failure"));
	UAM_ASSERT_EQ(app.chats.front().messages.size(), std::size_t{1});
}

UAM_TEST(AcpBusyQueueSaveFailureDoesNotHoldRemovedOrChangedOwnerIntent)
{
	for (const bool changed_owner : {false, true})
	{
		PromptFixture fixture;
		uam::AppState& app = fixture.app;
		uam::AcpSessionState& session = *fixture.session;
		session.processing = true;
		session.prompt_request_id = 10;
		session.queued_prompt = "Existing delivered turn";
		std::string error;
		UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Old queue ownership", &error));
		std::optional<bool> acknowledgement;
		UAM_ASSERT(uam::DeferChatSaveResult(app, app.chats.front().id, [&acknowledgement](bool saved) { acknowledgement = saved; }));
		if (changed_owner)
		{
			app.chats.front().provider_id = "codex-cli";
			session.provider_id = "codex-cli";
		}
		else
		{
			session.queued_user_prompts.clear();
			app.chats.front().acp_queued_prompts.clear();
		}
		fs::remove_all(app.data_root);
		UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "private blocked data root"));
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!acknowledgement.has_value() && std::chrono::steady_clock::now() < deadline)
		{
			uam::FlushPendingChatSaves(app);
			std::this_thread::yield();
		}
		UAM_ASSERT(acknowledgement.has_value() && !*acknowledgement);
		UAM_ASSERT(session.processing);
		UAM_ASSERT_EQ(session.prompt_request_id, 10);
		UAM_ASSERT(!session.queue_held && !app.chats.front().acp_queue_held);
		UAM_ASSERT(session.last_error.empty());
	}
}

UAM_TEST(AcpBusyQueueAdmissionIsDurableWhileAssistantOutputContinues)
{
	PromptFixture fixture;
	uam::AppState& app = fixture.app;
	uam::AcpSessionState& session = *fixture.session;
	session.processing = true;
	session.prompt_request_id = 10;
	session.queued_prompt = "Existing delivered turn";
	app.chats.front().messages.push_back({.role = MessageRole::Assistant, .content = "Ongoing output"});
	session.current_assistant_message_index = 1;
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Durable queue during streaming", &error));
	std::optional<bool> acknowledgement;
	UAM_ASSERT(uam::DeferChatSaveResult(app, app.chats.front().id, [&acknowledgement](bool saved) { acknowledgement = saved; }));
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	while (!acknowledgement.has_value() && std::chrono::steady_clock::now() < deadline)
	{
		uam::FlushPendingChatSaves(app);
		app.chats.front().messages.back().content += ".";
		uam::acp_detail::ScheduleChatSave(app, app.chats.front(), 0.5);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT(acknowledgement.has_value() && *acknowledgement);
	UAM_ASSERT(session.processing);
	UAM_ASSERT_EQ(session.prompt_request_id, 10);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, app.chats.front().id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->acp_queued_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(saved->acp_queued_prompts.front().text, std::string("Durable queue during streaming"));
}

UAM_TEST(AcpLocalQueueDispatchReturnsBeforeStorageAndRetiresHeadWithItsUserMessage)
{
	PromptFixture fixture;
	uam::AppState& app = fixture.app;
	uam::AcpSessionState& session = *fixture.session;
	session.processing = true;
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "First durable queue entry", &error));
	UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Second durable queue entry", &error));
	session.processing = false;
	const std::shared_ptr<PreparedChatSave> prepared = ChatRepository::PrepareChatSave(app.data_root, app.chats.front());
	UAM_ASSERT(prepared != nullptr);
	std::promise<void> entered;
	std::future<void> locked = entered.get_future();
	std::jthread publication([&entered, prepared]()
	{
		(void)ChatRepository::PublishPreparedChatSave(prepared, true, nullptr, [&entered]()
		{
			entered.set_value();
			std::this_thread::sleep_for(std::chrono::milliseconds(350));
			return false;
		});
	});
	UAM_ASSERT(locked.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
	const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
	const bool dispatched = uam::DrainNextQueuedAcpUserPrompt(app, session, app.chats.front());
	const std::chrono::steady_clock::duration elapsed = std::chrono::steady_clock::now() - started;
	publication.join();
	UAM_ASSERT(dispatched);
	UAM_ASSERT(elapsed <= std::chrono::milliseconds(100));
	UAM_ASSERT(session.processing);
	UAM_ASSERT_EQ(session.prompt_request_id, 0);
	UAM_ASSERT_EQ(session.queued_user_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(session.queued_user_prompts.front().text, std::string("Second durable queue entry"));
	std::optional<bool> acknowledgement;
	UAM_ASSERT(uam::DeferChatSaveResult(app, app.chats.front().id, [&acknowledgement](bool saved) { acknowledgement = saved; }));
	AwaitSave(app);
	UAM_ASSERT(acknowledgement.has_value() && *acknowledgement);
	const std::optional<ChatSession> admitted = ChatRepository::LoadLocalChat(app.data_root, app.chats.front().id);
	UAM_ASSERT(admitted.has_value());
	UAM_ASSERT_EQ(admitted->acp_dispatched_queued_prompt_count, std::size_t{0});
	UAM_ASSERT_EQ(admitted->acp_queued_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(admitted->messages.back().content, std::string("First durable queue entry"));
	UAM_ASSERT(admitted->messages.back().acp_prompt_not_sent);
	const fs::path restart_root = fixture.temp.root / "restart-copy";
	fs::copy(app.data_root, restart_root, fs::copy_options::recursive);
	uam::AppState reopened;
	reopened.data_root = restart_root;
	reopened.provider_profiles = app.provider_profiles;
	reopened.chats.push_back(*admitted);
	UAM_ASSERT_EQ(uam::RestoreRemoteAcpSessionsAfterRestart(reopened, true), std::size_t{1});
	UAM_ASSERT(reopened.chats.front().acp_queue_held);
	UAM_ASSERT_EQ(reopened.chats.front().acp_queued_prompts.front().text, std::string("Second durable queue entry"));
	UAM_ASSERT(reopened.chats.front().messages.back().acp_prompt_not_sent);
	UAM_ASSERT(uam::acp_detail::SendQueuedPromptIfReady(app, session, app.chats.front()));
	AwaitDelivery(app, session);
	UAM_ASSERT(session.prompt_request_id != 0);
}

UAM_TEST(AcpFailedLocalQueueAdmissionRetainsOneUnsentMessageAndHeldFollowingIntent)
{
	PromptFixture fixture;
	uam::AppState& app = fixture.app;
	uam::AcpSessionState& session = *fixture.session;
	session.processing = true;
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "First queued failure intent", &error));
	UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Second queued held intent", &error));
	session.processing = false;
	UAM_ASSERT(uam::DrainNextQueuedAcpUserPrompt(app, session, app.chats.front()));
	std::optional<bool> acknowledgement;
	UAM_ASSERT(uam::DeferChatSaveResult(app, app.chats.front().id, [&acknowledgement](bool saved) { acknowledgement = saved; }));
	fs::remove_all(app.data_root);
	UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "private blocked data root"));
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!acknowledgement.has_value() && std::chrono::steady_clock::now() < deadline)
	{
		uam::FlushPendingChatSaves(app);
		std::this_thread::yield();
	}
	UAM_ASSERT(acknowledgement.has_value() && !*acknowledgement);
	UAM_ASSERT(!session.processing);
	UAM_ASSERT_EQ(session.prompt_request_id, 0);
	UAM_ASSERT_EQ(app.chats.front().messages.size(), std::size_t{2});
	UAM_ASSERT_EQ(app.chats.front().messages.back().content, std::string("First queued failure intent"));
	UAM_ASSERT(app.chats.front().messages.back().acp_prompt_not_sent && app.chats.front().messages.back().interrupted);
	UAM_ASSERT_EQ(session.queued_user_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(session.queued_user_prompts.front().text, std::string("Second queued held intent"));
	UAM_ASSERT(session.queue_held && app.chats.front().acp_queue_held);
	UAM_ASSERT_EQ(app.chats.front().acp_dispatched_queued_prompt_count, std::size_t{0});
}

UAM_TEST(AcpSameSessionRetryReturnsWithinInputBudgetBeforeOwnedStorageReceipt)
{
	PromptFixture fixture;
	uam::AppState& app = fixture.app;
	uam::AcpSessionState& session = *fixture.session;
	ChatSession& chat = app.chats.front();
	chat.native_session_id = session.session_id;
	chat.messages.front().interrupted = true;
	chat.messages.front().acp_prompt_not_sent = true;
	chat.messages.push_back({.role = MessageRole::Assistant});
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	const std::shared_ptr<PreparedChatSave> prepared = ChatRepository::PrepareChatSave(app.data_root, chat);
	UAM_ASSERT(prepared != nullptr);
	std::promise<void> entered;
	std::future<void> locked = entered.get_future();
	std::jthread publication([&entered, prepared]()
	{
		(void)ChatRepository::PublishPreparedChatSave(prepared, true, nullptr, [&entered]()
		{
			entered.set_value();
			std::this_thread::sleep_for(std::chrono::milliseconds(350));
			return false;
		});
	});
	UAM_ASSERT(locked.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
	std::string error;
	const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
	const bool retried = uam::RetryFailedAcpMessage(app, chat.id, 0, &error);
	const std::chrono::steady_clock::duration elapsed = std::chrono::steady_clock::now() - started;
	publication.join();
	UAM_ASSERT(retried);
	UAM_ASSERT(elapsed <= std::chrono::milliseconds(100));
	UAM_ASSERT(session.processing && session.prompt_request_id == 0);
	UAM_ASSERT_EQ(chat.messages.size(), std::size_t{1});
	UAM_ASSERT(chat.messages.front().acp_prompt_not_sent);
	std::optional<bool> acknowledgement;
	UAM_ASSERT(uam::DeferChatSaveResult(app, chat.id, [&acknowledgement](bool saved) { acknowledgement = saved; }));
	AwaitSave(app);
	UAM_ASSERT(acknowledgement.has_value() && *acknowledgement);
	const std::optional<ChatSession> admitted = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(admitted.has_value() && admitted->messages.size() == 1 && admitted->messages.front().acp_prompt_not_sent);
	AwaitDelivery(app, session);
	UAM_ASSERT(session.prompt_request_id != 0);
	UAM_ASSERT_EQ(chat.messages.size(), std::size_t{1});
}

UAM_TEST(AcpFailedRetryAdmissionRestoresOriginalMessageWithoutProviderInput)
{
	for (const bool originally_unsent : {false, true})
	{
		PromptFixture fixture;
		uam::AppState& app = fixture.app;
		uam::AcpSessionState& session = *fixture.session;
		ChatSession& chat = app.chats.front();
		chat.native_session_id = session.session_id;
		chat.messages.front().interrupted = true;
		chat.messages.front().acp_prompt_not_sent = originally_unsent;
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
		std::string error;
		UAM_ASSERT(uam::RetryFailedAcpMessage(app, chat.id, 0, &error));
		std::optional<bool> acknowledgement;
		UAM_ASSERT(uam::DeferChatSaveResult(app, chat.id, [&acknowledgement](bool saved) { acknowledgement = saved; }));
		fs::remove_all(app.data_root);
		UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "private failed retry storage"));
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!acknowledgement.has_value() && std::chrono::steady_clock::now() < deadline)
		{
			uam::FlushPendingChatSaves(app);
			std::this_thread::yield();
		}
		UAM_ASSERT(acknowledgement.has_value() && !*acknowledgement);
		UAM_ASSERT(!session.processing && session.prompt_request_id == 0);
		UAM_ASSERT_EQ(chat.messages.size(), std::size_t{1});
		UAM_ASSERT(chat.messages.front().interrupted && chat.messages.front().acp_prompt_not_sent);
		UAM_ASSERT_EQ(chat.messages.front().content, std::string("Previously saved history"));
		fs::remove(app.data_root);
		AwaitSave(app);
		(void)uam::acp_detail::SendQueuedPromptIfReady(app, session, chat);
		UAM_ASSERT_EQ(session.prompt_request_id, 0);
	}
}

UAM_TEST(AcpCanceledStagedRetryPreservesOneUnsentOriginalMessage)
{
	PromptFixture fixture;
	uam::AppState& app = fixture.app;
	uam::AcpSessionState& session = *fixture.session;
	ChatSession& chat = app.chats.front();
	chat.native_session_id = session.session_id;
	chat.messages.front().interrupted = true;
	chat.messages.front().acp_prompt_not_sent = false;
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	std::string error;
	UAM_ASSERT(uam::RetryFailedAcpMessage(app, chat.id, 0, &error));
	AwaitSave(app);
	UAM_ASSERT(uam::acp_detail::SendQueuedPromptIfReady(app, session, chat));
	UAM_ASSERT(session.pending_prompt_delivery != nullptr);
	UAM_ASSERT_EQ(session.prompt_request_id, 0);
	UAM_ASSERT(uam::CancelAcpTurn(app, chat.id, &error));
	AwaitSave(app);
	(void)uam::acp_detail::SendQueuedPromptIfReady(app, session, chat);
	UAM_ASSERT_EQ(session.prompt_request_id, 0);
	UAM_ASSERT_EQ(chat.messages.size(), std::size_t{1});
	UAM_ASSERT(chat.messages.front().interrupted && chat.messages.front().acp_prompt_not_sent);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(saved.has_value() && saved->messages.size() == 1 && saved->messages.front().acp_prompt_not_sent);
}

UAM_TEST(AcpPendingFirstPromptQueueReturnsWithinInputBudgetWithCompleteOwnedAdmission)
{
	for (const bool staged_delivery : {false, true})
	{
		PromptFixture fixture;
		uam::AppState& app = fixture.app;
		uam::AcpSessionState& session = *fixture.session;
		ChatSession& chat = app.chats.front();
		std::string error;
		UAM_ASSERT(uam::SendAcpPrompt(app, chat.id, "First still-undelivered input", &error));
		if (staged_delivery)
		{
			AwaitSave(app);
			UAM_ASSERT(uam::acp_detail::SendQueuedPromptIfReady(app, session, chat));
			UAM_ASSERT(session.pending_prompt_delivery != nullptr);
		}
		UAM_ASSERT_EQ(session.prompt_request_id, 0);
		const std::shared_ptr<PreparedChatSave> prepared = ChatRepository::PrepareChatSave(app.data_root, chat);
		UAM_ASSERT(prepared != nullptr);
		std::promise<void> entered;
		std::future<void> locked = entered.get_future();
		std::jthread publication([&entered, prepared]()
		{
			(void)ChatRepository::PublishPreparedChatSave(prepared, true, nullptr, [&entered]()
			{
				entered.set_value();
				std::this_thread::sleep_for(std::chrono::milliseconds(350));
				return false;
			});
		});
		UAM_ASSERT(locked.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
		const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
		const bool queued = uam::SendAcpPrompt(app, chat.id, "Follow-up before first delivery", &error);
		const std::chrono::steady_clock::duration elapsed = std::chrono::steady_clock::now() - started;
		publication.join();
		UAM_ASSERT(queued);
		UAM_ASSERT(elapsed <= std::chrono::milliseconds(100));
		UAM_ASSERT_EQ(session.prompt_request_id, 0);
		UAM_ASSERT_EQ(session.queued_user_prompts.size(), std::size_t{1});
		std::optional<bool> acknowledgement;
		UAM_ASSERT(uam::DeferChatSaveResult(app, chat.id, [&acknowledgement](bool saved) { acknowledgement = saved; }));
		AwaitSave(app);
		UAM_ASSERT(acknowledgement.has_value() && *acknowledgement);
		const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, chat.id);
		UAM_ASSERT(saved.has_value());
		UAM_ASSERT_EQ(saved->messages.size(), std::size_t{2});
		UAM_ASSERT_EQ(saved->messages.back().content, std::string("First still-undelivered input"));
		UAM_ASSERT_EQ(saved->acp_queued_prompts.size(), std::size_t{1});
		UAM_ASSERT_EQ(saved->acp_queued_prompts.front().text, std::string("Follow-up before first delivery"));
		AwaitDelivery(app, session);
		UAM_ASSERT(session.prompt_request_id != 0);
		UAM_ASSERT_EQ(chat.messages.size(), std::size_t{2});
		UAM_ASSERT_EQ(session.queued_user_prompts.front().text, std::string("Follow-up before first delivery"));
	}
}

UAM_TEST(AcpFailedPendingFirstPromptQueueAdmissionPreservesBothUndeliveredIntents)
{
	for (const bool staged_delivery : {false, true})
	{
		PromptFixture fixture;
		uam::AppState& app = fixture.app;
		uam::AcpSessionState& session = *fixture.session;
		ChatSession& chat = app.chats.front();
		std::string error;
		UAM_ASSERT(uam::SendAcpPrompt(app, chat.id, "First pending failure intent", &error));
		if (staged_delivery)
		{
			AwaitSave(app);
			UAM_ASSERT(uam::acp_detail::SendQueuedPromptIfReady(app, session, chat));
			UAM_ASSERT(session.pending_prompt_delivery != nullptr);
		}
		UAM_ASSERT(uam::SendAcpPrompt(app, chat.id, "Retained follow-up failure intent", &error));
		std::optional<bool> acknowledgement;
		UAM_ASSERT(uam::DeferChatSaveResult(app, chat.id, [&acknowledgement](bool saved) { acknowledgement = saved; }));
		fs::remove_all(app.data_root);
		UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "private failed pending admission storage"));
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!acknowledgement.has_value() && std::chrono::steady_clock::now() < deadline)
		{
			uam::FlushPendingChatSaves(app);
			std::this_thread::yield();
		}
		UAM_ASSERT(acknowledgement.has_value() && !*acknowledgement);
		UAM_ASSERT(!session.processing && session.prompt_request_id == 0);
		UAM_ASSERT_EQ(chat.messages.size(), std::size_t{2});
		UAM_ASSERT(chat.messages.back().interrupted && chat.messages.back().acp_prompt_not_sent);
		UAM_ASSERT_EQ(chat.messages.back().content, std::string("First pending failure intent"));
		UAM_ASSERT_EQ(session.queued_user_prompts.size(), std::size_t{1});
		UAM_ASSERT_EQ(session.queued_user_prompts.front().text, std::string("Retained follow-up failure intent"));
		UAM_ASSERT(session.queue_held && chat.acp_queue_held);
		fs::remove(app.data_root);
		AwaitSave(app);
		UAM_ASSERT(!uam::DrainNextQueuedAcpUserPrompt(app, session, chat));
		UAM_ASSERT_EQ(session.prompt_request_id, 0);
	}
}

UAM_TEST(AcpQueuedControlReceiptsWaitForOwnedStorageWithoutInterruptingDeliveredTurn)
{
	for (const std::string operation : {"edit", "reorder", "remove", "resume"})
	{
		for (const bool save_fails : {false, true})
		{
			PromptFixture fixture;
			uam::AppState& app = fixture.app;
			uam::AcpSessionState& session = *fixture.session;
			ChatSession& chat = app.chats.front();
			session.processing = true;
			session.prompt_request_id = 17;
			std::string error;
			UAM_ASSERT(uam::SendAcpPrompt(app, chat.id, "First control intent", &error));
			UAM_ASSERT(uam::SendAcpPrompt(app, chat.id, "Second control intent", &error));
			AwaitSave(app);
			std::vector<std::pair<std::string, int>> expected;
			for (const uam::AcpQueuedUserPromptState& prompt : session.queued_user_prompts)
				expected.emplace_back(prompt.id, prompt.revision);
			if (operation == "edit")
				UAM_ASSERT(uam::EditQueuedAcpPrompt(app, chat.id, expected[0].first, expected[0].second, "Edited control intent", &error));
			else if (operation == "reorder")
				UAM_ASSERT(uam::ReorderQueuedAcpPrompts(app, chat.id, expected, {expected[1].first, expected[0].first}, &error));
			else if (operation == "remove")
				UAM_ASSERT(uam::RemoveQueuedAcpPrompt(app, chat.id, 0, &error, expected[0]));
			else
			{
				session.queue_held = chat.acp_queue_held = true;
				UAM_ASSERT(uam::acp_detail::SaveChatQuietly(app, chat));
				UAM_ASSERT(uam::ResumeQueuedAcpPrompts(app, chat.id, expected, &error));
			}
			std::optional<bool> acknowledgement;
			UAM_ASSERT(uam::DeferChatSaveResult(app, chat.id, [&acknowledgement](bool saved) { acknowledgement = saved; }));
			UAM_ASSERT(!acknowledgement.has_value());
			if (save_fails)
			{
				fs::remove_all(app.data_root);
				UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "private failed queue control storage"));
			}
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (!acknowledgement.has_value() && std::chrono::steady_clock::now() < deadline)
			{
				uam::FlushPendingChatSaves(app);
				std::this_thread::yield();
			}
			UAM_ASSERT(acknowledgement.has_value() && *acknowledgement == !save_fails);
			UAM_ASSERT(session.processing && session.prompt_request_id == 17 && session.last_error.empty());
			if (save_fails)
			{
				UAM_ASSERT(session.queue_held && chat.acp_queue_held);
				fs::remove(app.data_root);
			}
			AwaitSave(app);
			const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, chat.id);
			UAM_ASSERT(saved.has_value());
			UAM_ASSERT_EQ(saved->messages, chat.messages);
			UAM_ASSERT_EQ(saved->acp_queued_prompts.size(), session.queued_user_prompts.size());
			for (std::size_t index = 0; index < session.queued_user_prompts.size(); ++index)
			{
				UAM_ASSERT_EQ(saved->acp_queued_prompts[index].id, session.queued_user_prompts[index].id);
				UAM_ASSERT_EQ(saved->acp_queued_prompts[index].text, session.queued_user_prompts[index].text);
				UAM_ASSERT_EQ(saved->acp_queued_prompts[index].revision, session.queued_user_prompts[index].revision);
			}
			UAM_ASSERT_EQ(saved->acp_queue_held, session.queue_held);
		}
	}
}

UAM_TEST(AcpIdleDeferredAdmissionReturnsWithinInputBudgetAndSavesCompleteHistory)
{
	for (const bool compatibility_pending : {false, true})
	{
		PromptFixture fixture;
		uam::AppState& app = fixture.app;
		uam::AcpSessionState& session = *fixture.session;
		ChatSession& chat = app.chats.front();
		fixture.process.StopStdioProcess(session, true);
		fixture.process.CloseStdioProcessHandles(session);
		session.running = session.processing = session.session_ready = false;
		if (compatibility_pending)
		{
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			chat.provider_id = session.provider_id = "opencode-cli";
			session.protocol_kind = "opencode-acp";
			app.runtime_cli_versions_by_provider_id[chat.provider_id] = {};
		}
		else chat.routing_mode = "automatic";
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
		const std::shared_ptr<PreparedChatSave> prepared = ChatRepository::PrepareChatSave(app.data_root, chat);
		UAM_ASSERT(prepared != nullptr);
		chat.messages.push_back({.role = MessageRole::Assistant, .content = "Latest unsaved idle transcript"});
		std::promise<void> entered;
		std::future<void> locked = entered.get_future();
		std::jthread publication([&entered, prepared]()
		{
			(void)ChatRepository::PublishPreparedChatSave(prepared, true, nullptr, [&entered]()
			{
				entered.set_value();
				std::this_thread::sleep_for(std::chrono::milliseconds(350));
				return false;
			});
		});
		UAM_ASSERT(locked.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
		std::string error;
		const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
		const bool accepted = uam::SendAcpPrompt(app, chat.id, "Idle deferred input", &error);
		const std::chrono::steady_clock::duration elapsed = std::chrono::steady_clock::now() - started;
		publication.join();
		UAM_ASSERT(accepted);
		UAM_ASSERT(elapsed <= std::chrono::milliseconds(100));
		UAM_ASSERT(!session.processing && session.prompt_request_id == 0);
		UAM_ASSERT_EQ(session.queued_user_prompts.size(), std::size_t{1});
		UAM_ASSERT(!uam::DrainNextQueuedAcpUserPrompt(app, session, chat));
		std::optional<bool> acknowledgement;
		UAM_ASSERT(uam::DeferChatSaveResult(app, chat.id, [&acknowledgement](bool saved) { acknowledgement = saved; }));
		UAM_ASSERT(!acknowledgement.has_value());
		AwaitSave(app);
		UAM_ASSERT(acknowledgement.has_value() && *acknowledgement);
		const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, chat.id);
		UAM_ASSERT(saved.has_value());
		UAM_ASSERT_EQ(saved->messages, chat.messages);
		UAM_ASSERT_EQ(saved->messages.back().content, std::string("Latest unsaved idle transcript"));
		UAM_ASSERT_EQ(saved->acp_queued_prompts.size(), std::size_t{1});
		UAM_ASSERT_EQ(saved->acp_queued_prompts.front().text, std::string("Idle deferred input"));
		UAM_ASSERT_EQ(session.prompt_request_id, 0);
	}
}

UAM_TEST(AcpFailedIdleDeferredAdmissionHoldsCapturedIntentWithoutProviderLaunch)
{
	for (const bool compatibility_pending : {false, true})
	{
		PromptFixture fixture;
		uam::AppState& app = fixture.app;
		uam::AcpSessionState& session = *fixture.session;
		ChatSession& chat = app.chats.front();
		fixture.process.StopStdioProcess(session, true);
		fixture.process.CloseStdioProcessHandles(session);
		session.running = session.processing = session.session_ready = false;
		if (compatibility_pending)
		{
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			chat.provider_id = session.provider_id = "opencode-cli";
			session.protocol_kind = "opencode-acp";
			app.runtime_cli_versions_by_provider_id[chat.provider_id] = {};
		}
		else chat.routing_mode = "automatic";
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
		std::string error;
		UAM_ASSERT(uam::SendAcpPrompt(app, chat.id, "Retained idle failure input", &error));
		std::optional<bool> acknowledgement;
		UAM_ASSERT(uam::DeferChatSaveResult(app, chat.id, [&acknowledgement](bool saved) { acknowledgement = saved; }));
		fs::remove_all(app.data_root);
		UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "private failed idle admission storage"));
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!acknowledgement.has_value() && std::chrono::steady_clock::now() < deadline)
		{
			uam::FlushPendingChatSaves(app);
			std::this_thread::yield();
		}
		UAM_ASSERT(acknowledgement.has_value() && !*acknowledgement);
		UAM_ASSERT(session.queue_held && chat.acp_queue_held);
		UAM_ASSERT(!session.running && !session.processing && session.prompt_request_id == 0);
		UAM_ASSERT_EQ(chat.messages.size(), std::size_t{1});
		UAM_ASSERT_EQ(session.queued_user_prompts.size(), std::size_t{1});
		UAM_ASSERT_EQ(session.queued_user_prompts.front().text, std::string("Retained idle failure input"));
		UAM_ASSERT(!uam::DrainNextQueuedAcpUserPrompt(app, session, chat));
	}
}
