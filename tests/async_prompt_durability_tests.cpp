#include "test_harness.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/paths/app_paths.h"
#include "common/utils/hash_utils.h"
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
	void PrepareNativeSteer(PromptFixture& fixture)
	{
		uam::AppState& app = fixture.app;
		uam::AcpSessionState& session = *fixture.session;
		ChatSession& chat = app.chats.front();
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		chat.provider_id = session.provider_id = "codex-cli";
		chat.native_session_id = session.session_id = "6a6f0f3b-1a0b-4a9c-8a01-444444444444";
		session.protocol_kind = "codex-app-server";
		session.codex_thread_id = session.session_id;
		session.codex_turn_id = "private-active-turn";
		session.processing = true;
		session.prompt_request_id = 17;
		session.turn_serial = 7;
		session.turn_user_message_index = session.turn_first_user_message_index = 0;
		chat.messages.push_back({.role = MessageRole::Assistant, .content = "Parent output before steering"});
		session.current_assistant_message_index = session.turn_assistant_message_index = 1;
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	}
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

UAM_TEST(AcpNativeSteerReturnsBeforeStorageAndReleasesOneOwnedControl)
{
	PromptFixture fixture;
	uam::AppState& app = fixture.app;
	uam::AcpSessionState& session = *fixture.session;
	ChatSession& chat = app.chats.front();
	PrepareNativeSteer(fixture);
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
	const bool accepted = uam::SteerAcpPrompt(app, chat.id, "Owned additional requirement", {}, {}, false, &error);
	const std::chrono::steady_clock::duration elapsed = std::chrono::steady_clock::now() - started;
	publication.join();
	UAM_ASSERT(accepted);
	UAM_ASSERT(elapsed <= std::chrono::milliseconds(100));
	UAM_ASSERT(session.processing && session.prompt_request_id == 17 && session.turn_serial == 7);
	UAM_ASSERT(session.pending_request_methods.empty());
	std::optional<bool> acknowledgement;
	UAM_ASSERT(uam::DeferChatSaveResult(app, chat.id, [&acknowledgement](bool saved) { acknowledgement = saved; }));
	UAM_ASSERT(!acknowledgement.has_value());
	AwaitSave(app);
	UAM_ASSERT(acknowledgement.has_value() && *acknowledgement);
	UAM_ASSERT_EQ(session.pending_request_methods.size(), std::size_t{1});
	UAM_ASSERT_EQ(chat.messages.size(), std::size_t{3});
	UAM_ASSERT_EQ(chat.messages.back().content, std::string("Owned additional requirement"));
	UAM_ASSERT(chat.messages.back().continues_turn && !chat.messages.back().acp_prompt_not_sent);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->messages.back().content, chat.messages.back().content);
	UAM_ASSERT(!saved->messages.back().acp_prompt_not_sent);
	UAM_ASSERT_EQ(std::ranges::count_if(session.diagnostics, [](const uam::AcpDiagnosticEntryState& item)
	    { return item.method == "turn/steer" && item.reason == "sent"; }), std::ptrdiff_t{1});
}

UAM_TEST(AcpNativeSteerAdmissionSurvivesContinuousParentOutput)
{
	PromptFixture fixture;
	PrepareNativeSteer(fixture);
	uam::AppState& app = fixture.app;
	uam::AcpSessionState& session = *fixture.session;
	ChatSession& chat = app.chats.front();
	std::string error;
	UAM_ASSERT(uam::SteerAcpPrompt(app, chat.id, "Steer while parent streams", {}, {}, false, &error));
	std::optional<bool> acknowledgement;
	UAM_ASSERT(uam::DeferChatSaveResult(app, chat.id, [&acknowledgement](bool saved) { acknowledgement = saved; }));
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	while (!acknowledgement.has_value() && std::chrono::steady_clock::now() < deadline)
	{
		chat.messages[1].content += " parent-chunk";
		uam::acp_detail::ScheduleChatSave(app, chat, 0.0);
		uam::FlushPendingChatSaves(app);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	UAM_ASSERT(acknowledgement.has_value() && *acknowledgement);
	UAM_ASSERT_EQ(session.pending_request_methods.size(), std::size_t{1});
	UAM_ASSERT(session.processing && session.prompt_request_id == 17);
	AwaitSave(app);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->messages[1].content, chat.messages[1].content);
	UAM_ASSERT_EQ(saved->messages.back().content, std::string("Steer while parent streams"));
}

UAM_TEST(AcpNativeSteerSaveFailureKeepsUnsentIntentWithoutStoppingParent)
{
	PromptFixture fixture;
	PrepareNativeSteer(fixture);
	uam::AppState& app = fixture.app;
	uam::AcpSessionState& session = *fixture.session;
	ChatSession& chat = app.chats.front();
	std::string error;
	UAM_ASSERT(uam::SteerAcpPrompt(app, chat.id, "Retain failed steering", {}, {}, false, &error));
	std::optional<bool> acknowledgement;
	UAM_ASSERT(uam::DeferChatSaveResult(app, chat.id, [&acknowledgement](bool saved) { acknowledgement = saved; }));
	fs::remove_all(app.data_root);
	UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "private failed steering storage"));
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!acknowledgement.has_value() && std::chrono::steady_clock::now() < deadline)
	{
		uam::FlushPendingChatSaves(app);
		std::this_thread::yield();
	}
	UAM_ASSERT(acknowledgement.has_value() && !*acknowledgement);
	UAM_ASSERT(session.processing && session.prompt_request_id == 17);
	UAM_ASSERT(session.pending_request_methods.empty());
	UAM_ASSERT(session.pending_local_steer_abandon.empty());
	UAM_ASSERT(chat.messages.back().acp_prompt_not_sent && chat.messages.back().interrupted);
	UAM_ASSERT(session.goal_resume_suppressed);
	fs::remove(app.data_root);
	AwaitSave(app);
	UAM_ASSERT(session.pending_request_methods.empty());
}

UAM_TEST(AcpNativeSteerCancellationAndCompletionNeverReleaseExpiredControl)
{
	for (const bool cancel : {false, true})
	{
		PromptFixture fixture;
		PrepareNativeSteer(fixture);
		uam::AppState& app = fixture.app;
		uam::AcpSessionState& session = *fixture.session;
		ChatSession& chat = app.chats.front();
		std::string error;
		UAM_ASSERT(uam::SteerAcpPrompt(app, chat.id, "Abandon pending steering", {}, {}, false, &error));
		if (cancel) UAM_ASSERT(uam::CancelAcpTurn(app, chat.id, &error));
		else uam::acp_detail::CompletePromptTurn(session, "ready");
		AwaitSave(app);
		UAM_ASSERT(session.pending_local_steer_abandon.empty());
		UAM_ASSERT(chat.messages.back().acp_prompt_not_sent && chat.messages.back().interrupted);
		UAM_ASSERT(session.goal_resume_suppressed);
		UAM_ASSERT(std::ranges::none_of(session.diagnostics, [](const uam::AcpDiagnosticEntryState& item)
		    { return item.method == "turn/steer" && item.reason == "sent"; }));
		const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, chat.id);
		UAM_ASSERT(saved.has_value() && saved->messages.back().acp_prompt_not_sent);
	}
}

UAM_TEST(AcpNativeSteerChangedConfigurationRetainsUnsentOriginalIntent)
{
	PromptFixture fixture;
	PrepareNativeSteer(fixture);
	uam::AppState& app = fixture.app;
	uam::AcpSessionState& session = *fixture.session;
	ChatSession& chat = app.chats.front();
	std::string error;
	UAM_ASSERT(uam::SteerAcpPrompt(app, chat.id, "Original steering context", {}, {}, false, &error));
	chat.reasoning_effort = "changed-policy";
	uam::acp_detail::ScheduleChatSave(app, chat, 0.0);
	AwaitSave(app);
	UAM_ASSERT(session.pending_request_methods.empty());
	UAM_ASSERT(chat.messages.back().acp_prompt_not_sent && chat.messages.back().interrupted);
	UAM_ASSERT(session.processing && session.prompt_request_id == 17);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(saved.has_value() && saved->messages.back().acp_prompt_not_sent);
}

UAM_TEST(AcpNativeSteerForceReceiptIncludesItsMessageAndDoesNotReplay)
{
	PromptFixture fixture;
	PrepareNativeSteer(fixture);
	uam::AppState& app = fixture.app;
	uam::AcpSessionState& session = *fixture.session;
	ChatSession& chat = app.chats.front();
	std::string error;
	UAM_ASSERT(uam::SteerAcpPrompt(app, chat.id, "Forced durable steering", {}, {}, false, &error));
	UAM_ASSERT(uam::acp_detail::SaveChatQuietly(app, chat));
	AwaitSave(app);
	UAM_ASSERT_EQ(session.pending_request_methods.size(), std::size_t{1});
	UAM_ASSERT_EQ(std::ranges::count_if(session.diagnostics, [](const uam::AcpDiagnosticEntryState& item)
	    { return item.method == "turn/steer" && item.reason == "sent"; }), std::ptrdiff_t{1});
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->messages.back().content, std::string("Forced durable steering"));
	UAM_ASSERT(!saved->messages.back().acp_prompt_not_sent);
}

UAM_TEST(AcpNativeSteerCoalescesFollowingQueueIntoCompleteOwnedReceipt)
{
	PromptFixture fixture;
	PrepareNativeSteer(fixture);
	uam::AppState& app = fixture.app;
	uam::AcpSessionState& session = *fixture.session;
	ChatSession& chat = app.chats.front();
	std::string error;
	UAM_ASSERT(uam::SteerAcpPrompt(app, chat.id, "First coalesced steering", {}, {}, false, &error));
	UAM_ASSERT(uam::SteerAcpPrompt(app, chat.id, "Second coalesced steering", {}, {}, false, &error));
	UAM_ASSERT(uam::SendAcpPrompt(app, chat.id, "Following ordinary queued input", &error));
	std::optional<bool> acknowledgement;
	UAM_ASSERT(uam::DeferChatSaveResult(app, chat.id, [&acknowledgement](bool saved) { acknowledgement = saved; }));
	AwaitSave(app);
	UAM_ASSERT(acknowledgement.has_value() && *acknowledgement);
	UAM_ASSERT_EQ(session.pending_request_methods.size(), std::size_t{2});
	UAM_ASSERT_EQ(chat.messages.size(), std::size_t{4});
	UAM_ASSERT_EQ(session.queued_user_prompts.size(), std::size_t{1});
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->messages[2].content, std::string("First coalesced steering"));
	UAM_ASSERT_EQ(saved->messages[3].content, std::string("Second coalesced steering"));
	UAM_ASSERT_EQ(saved->acp_queued_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(saved->acp_queued_prompts.front().text, std::string("Following ordinary queued input"));
	UAM_ASSERT_EQ(std::ranges::count_if(session.diagnostics, [](const uam::AcpDiagnosticEntryState& item)
	    { return item.method == "turn/steer" && item.reason == "sent"; }), std::ptrdiff_t{2});
}

UAM_TEST(AcpNativeSteerRejectsReplacedNativeTurnBeforeItsReceipt)
{
	PromptFixture fixture;
	PrepareNativeSteer(fixture);
	uam::AppState& app = fixture.app;
	uam::AcpSessionState& session = *fixture.session;
	ChatSession& chat = app.chats.front();
	std::string error;
	UAM_ASSERT(uam::SteerAcpPrompt(app, chat.id, "Original native turn only", {}, {}, false, &error));
	session.codex_turn_id = "replacement-native-turn";
	uam::acp_detail::ScheduleChatSave(app, chat, 0.0);
	AwaitSave(app);
	UAM_ASSERT(session.pending_request_methods.empty());
	UAM_ASSERT(session.processing && session.prompt_request_id == 17);
	UAM_ASSERT(chat.messages.back().acp_prompt_not_sent && chat.messages.back().interrupted);
	UAM_ASSERT(std::ranges::none_of(session.diagnostics, [](const uam::AcpDiagnosticEntryState& item)
	    { return item.method == "turn/steer" && item.reason == "sent"; }));
}

UAM_TEST(AcpSmallModelSendWaitsForDurableGoalBeforeProviderInput)
{
	PromptFixture fixture;
	uam::AppState& app = fixture.app;
	app.chats.front().small_model_mode = true;
	const std::shared_ptr<PreparedChatSave> prepared = ChatRepository::PrepareChatSave(app.data_root, app.chats.front());
	UAM_ASSERT(prepared);
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
	const bool accepted = uam::SendAcpPrompt(app, app.chats.front().id, "Small model durable objective", &error);
	const std::chrono::steady_clock::duration elapsed = std::chrono::steady_clock::now() - started;
	publication.join();
	UAM_ASSERT(accepted);
	UAM_ASSERT(elapsed <= std::chrono::milliseconds(100));
	UAM_ASSERT_EQ(fixture.session->prompt_request_id, 0);
	UAM_ASSERT_EQ(ReadFile(fixture.saved_path), fixture.original);
	UAM_ASSERT_EQ(app.chats.front().goals.size(), std::size_t{1});
	const std::string goal_id = app.chats.front().active_goal_id;
	UAM_ASSERT(!goal_id.empty());
	AwaitSave(app);
	const std::optional<ChatSession> durable = ChatRepository::LoadLocalChat(app.data_root, app.chats.front().id);
	UAM_ASSERT(durable);
	UAM_ASSERT_EQ(durable->active_goal_id, goal_id);
	UAM_ASSERT_EQ(durable->goals.size(), std::size_t{1});
	UAM_ASSERT_EQ(durable->messages.size(), std::size_t{2});
	UAM_ASSERT_EQ(durable->messages.back().content, std::string("Small model durable objective"));
	AwaitDelivery(app, *fixture.session);
	UAM_ASSERT(fixture.session->prompt_request_id != 0);
}

UAM_TEST(AcpSmallModelCanceledAdmissionRetainsGoalWithoutProviderPrompt)
{
	PromptFixture fixture;
	uam::AppState& app = fixture.app;
	app.chats.front().small_model_mode = true;
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


UAM_TEST(AcpSmallModelFailedAdmissionKeepsGoalAndPromptUnsent)
{
	PromptFixture fixture;
	uam::AppState& app = fixture.app;
	app.chats.front().small_model_mode = true;
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
	UAM_ASSERT_EQ(app.chats.front().goals.size(), std::size_t{1});
	UAM_ASSERT(!fixture.session->processing);
	UAM_ASSERT(fixture.session->queued_prompt.empty());
	UAM_ASSERT(!fixture.session->last_error.empty());
	UAM_ASSERT(app.chats.front().messages.back().acp_prompt_not_sent);
	UAM_ASSERT(app.chats.front().messages.back().interrupted);
	UAM_ASSERT_EQ(ReadFile(fixture.saved_path), fixture.original);
}

UAM_TEST(AcpSmallModelLargeUnsavedHistoryKeepsAdmissionPollWithinInputBudget)
{
	PromptFixture fixture;
	uam::AppState& app = fixture.app;
	app.chats.front().small_model_mode = true;
	for (int index = 1; index < 200; ++index)
		app.chats.front().messages.push_back({.role = MessageRole::Assistant,
		    .content = std::to_string(index) + std::string(128 * 1024, 'x')});
	const std::string last_unsaved = app.chats.front().messages.back().content;
	std::string error;
	const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
	UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Keep large small-model history", &error));
	UAM_ASSERT(std::chrono::steady_clock::now() - started <= std::chrono::milliseconds(100));
	UAM_ASSERT_EQ(fixture.session->prompt_request_id, 0);
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (app.pending_chat_save_at_by_chat_id.contains(app.chats.front().id) && std::chrono::steady_clock::now() < deadline)
	{
		const std::chrono::steady_clock::time_point poll_started = std::chrono::steady_clock::now();
		uam::FlushPendingChatSaves(app);
		UAM_ASSERT(std::chrono::steady_clock::now() - poll_started <= std::chrono::milliseconds(100));
		std::this_thread::yield();
	}
	UAM_ASSERT(!app.pending_chat_save_at_by_chat_id.contains(app.chats.front().id));
	const std::optional<ChatSession> durable = ChatRepository::LoadLocalChat(app.data_root, app.chats.front().id);
	UAM_ASSERT(durable);
	UAM_ASSERT_EQ(durable->messages.size(), std::size_t{201});
	UAM_ASSERT_EQ(durable->messages[199].content, last_unsaved);
	UAM_ASSERT_EQ(durable->messages.back().content, std::string("Keep large small-model history"));
	UAM_ASSERT_EQ(durable->goals.size(), std::size_t{1});
	UAM_ASSERT(!durable->active_goal_id.empty());
	UAM_ASSERT_EQ(fixture.session->prompt_request_id, 0);
	AwaitDelivery(app, *fixture.session);
	UAM_ASSERT(fixture.session->prompt_request_id != 0);
}

static void CheckColdLocalConfigurationAdmission(std::string_view variation)
{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
	TempDir temp("uam-cold-local-admission");
	const fs::path assets = temp.root / "private-skills" / "fixture";
	fs::create_directories(assets);
	UAM_ASSERT(uam::io::WriteTextFile(assets / "SKILL.md", "Read the fixture references when requested."));
	// This supported cold bundle exercises real resource reads, hashes and installation.
	for (int index = 0; index < 2048; ++index)
	{
		std::ofstream file(assets / ("reference-" + std::to_string(index) + ".txt"), std::ios::binary);
		file << "Private fixture reference " << index << std::string(512, 'x');
		UAM_ASSERT(file.good());
	}
	const fs::path provider_input = temp.root / "private-provider-input.jsonl";
	const fs::path prompt_log = temp.root / "private-provider-prompts.jsonl";
#if defined(_WIN32)
	const fs::path shim = temp.root / "opencode.cmd";
	const fs::path script = temp.root / "provider-fixture.ps1";
	UAM_ASSERT(uam::io::WriteTextFile(shim, "@echo off\r\npowershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%~dp0provider-fixture.ps1\"\r\n"));
	UAM_ASSERT(uam::io::WriteTextFile(script, R"(
$heldId = $null
$heldOnce = $false
function Reply($id, $result) { [Console]::WriteLine((@{jsonrpc='2.0';id=$id;result=$result} | ConvertTo-Json -Compress -Depth 12)) }
while ($null -ne ($line = [Console]::ReadLine())) {
  [IO.File]::AppendAllText($env:UAM_TEST_COLD_INPUT, $line + "`n")
  $request = $line | ConvertFrom-Json
  switch ($request.method) {
    'initialize' { Reply $request.id @{protocolVersion=1;agentCapabilities=@{loadSession=$true}} }
    'session/new' { Reply $request.id @{sessionId='cold-private-session'}; if ($env:UAM_TEST_COLD_EXIT_AFTER_SETUP -eq '1') { exit 0 } }
    'session/load' { Reply $request.id @{sessionId='cold-private-session'}; if ($env:UAM_TEST_COLD_EXIT_AFTER_SETUP -eq '1') { exit 0 } }
    'session/set_mode' { Reply $request.id @{} }
    'session/set_model' { Reply $request.id @{} }
    'session/set_config_option' { Reply $request.id @{} }
    'fixture/chunk' { [Console]::WriteLine('{"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"stream-during-agent-preparation"}}}}') }
    'fixture/release' { if ($null -ne $heldId) { Reply $heldId @{stopReason='end_turn'}; $heldId = $null } }
    'session/prompt' {
      $durable = [IO.File]::ReadAllText($env:UAM_TEST_COLD_PRIMARY)
      if (-not $durable.Contains('Launch with the private cold configuration') -or -not $durable.Contains('199xxxxxxxx') -or -not $durable.Contains($env:UAM_TEST_COLD_AGENT_HASH) -or -not $durable.Contains('cold-private-session')) { exit 94 }
      [IO.File]::AppendAllText($env:UAM_TEST_COLD_PROMPTS, $line + "`n")
      [Console]::WriteLine('{"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"private-cold-launch-ok"}}}}')
      if ($env:UAM_TEST_COLD_HOLD_COMPLETION -ne '1' -or $heldOnce) { Reply $request.id @{stopReason='end_turn'} } else { $heldId = $request.id; $heldOnce = $true }
    }
  }
}
)"));
	const char separator = ';';
#else
	const fs::path shim = temp.root / "opencode";
	UAM_ASSERT(uam::io::WriteTextFile(shim, R"(#!/bin/sh
held_id=
held_once=0
while IFS= read -r line; do
  printf '%s\n' "$line" >> "$UAM_TEST_COLD_INPUT"
  id=$(printf '%s\n' "$line" | sed -n 's/.*"id":\([0-9][0-9]*\).*/\1/p')
  case "$line" in
    *'"method":"initialize"'*) printf '{"jsonrpc":"2.0","id":%s,"result":{"protocolVersion":1,"agentCapabilities":{"loadSession":true}}}\n' "$id" ;;
    *'"method":"session/new"'*|*'"method":"session/load"'*) printf '{"jsonrpc":"2.0","id":%s,"result":{"sessionId":"cold-private-session"}}\n' "$id"; if [ "$UAM_TEST_COLD_EXIT_AFTER_SETUP" = 1 ]; then exit 0; fi ;;
    *'"method":"session/set_mode"'*|*'"method":"session/set_model"'*|*'"method":"session/set_config_option"'*) printf '{"jsonrpc":"2.0","id":%s,"result":{}}\n' "$id" ;;
    *'"method":"fixture/chunk"'*) printf '%s\n' '{"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"stream-during-agent-preparation"}}}}' ;;
    *'"method":"fixture/release"'*) if [ -n "$held_id" ]; then printf '{"jsonrpc":"2.0","id":%s,"result":{"stopReason":"end_turn"}}\n' "$held_id"; held_id=; fi ;;
    *'"method":"session/prompt"'*)
      grep -Fq 'Launch with the private cold configuration' "$UAM_TEST_COLD_PRIMARY" || exit 94
      grep -Fq '199xxxxxxxx' "$UAM_TEST_COLD_PRIMARY" || exit 94
      grep -Fq "$UAM_TEST_COLD_AGENT_HASH" "$UAM_TEST_COLD_PRIMARY" || exit 94
      grep -Fq 'cold-private-session' "$UAM_TEST_COLD_PRIMARY" || exit 94
      printf '%s\n' "$line" >> "$UAM_TEST_COLD_PROMPTS"
      printf '%s\n' '{"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"private-cold-launch-ok"}}}}'
      if [ "$UAM_TEST_COLD_HOLD_COMPLETION" != 1 ] || [ "$held_once" = 1 ]; then
        printf '{"jsonrpc":"2.0","id":%s,"result":{"stopReason":"end_turn"}}\n' "$id"
      else
        held_id=$id
        held_once=1
      fi
      ;;
  esac
done
)"));
	fs::permissions(shim, fs::perms::owner_all, fs::perm_options::replace);
	const char separator = ':';
#endif
	ScopedEnvVar path("PATH", temp.root.string() + separator + uam::env::GetNonEmptyString("PATH").value_or(""));
	ScopedEnvVar input_path("UAM_TEST_COLD_INPUT", provider_input.string());
	ScopedEnvVar prompts_path("UAM_TEST_COLD_PROMPTS", prompt_log.string());
	ScopedEnvVar hold_completion("UAM_TEST_COLD_HOLD_COMPLETION", variation.starts_with("busy") ? "1" : "0");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
	app.settings.provider_chat_defaults["opencode-cli"].feature_preference = "uam";
	app.settings.central_provider_configuration.enabled = true;
	app.settings.central_provider_configuration.skill_directories = {(temp.root / "private-skills").string()};
	if (variation == "failure") app.settings.central_provider_configuration.instruction_files = {(temp.root / "missing-instructions.md").string()};
	uam::CliProviderVersionState& version = app.runtime_cli_versions_by_provider_id["opencode-cli"];
	version.checked = version.supported = true;
	const ProviderCliPolicy* policy = ProviderRuntimeRegistry::ResolveById("opencode-cli").CliVersionPolicy();
	UAM_ASSERT(policy != nullptr);
	version.installed_version = policy->minimum_version;
	ChatSession chat;
	chat.id = "cold-local-admission";
	chat.provider_id = "opencode-cli";
	if (variation.empty()) chat.model_id = "selected-cold-model";
	chat.uam_agent_id = "build";
	chat.approval_mode.clear();
	chat.memory_enabled = false;
	chat.workspace_directory = temp.root.string();
	chat.messages.push_back({.role = MessageRole::User, .content = "Previously durable history"});
	app.chats.push_back(std::move(chat));
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
	if (variation == "catalog" || variation == "default" || variation == "prepare" || variation == "source" || variation == "agentfailure" || variation == "warm" || variation.starts_with("busy"))
	{
		// Both supported scopes stay below their existing 8 MiB and 256-file limits.
		for (const bool workspace_scope : {false, true})
		{
			const fs::path directory = workspace_scope ? temp.root / ".uam" / "agents" : app.data_root / "agents";
			fs::create_directories(directory);
			for (int index = 0; index < 48; ++index)
			{
				const std::string id = std::string(workspace_scope ? "workspace-" : "global-") + std::to_string(index);
				std::ofstream file(directory / (id + ".md"), std::ios::binary);
				file << "---\nversion: 1\nname: " << id << "\ndescription: Private selected agent timing fixture\nmode: primary\nworkspaceAccess: write\nskills: []\ndelegates: []\n---\n";
				file << "Retain selected agent instructions " << id << "\n" << std::string(170 * 1024, 'x') << '\n';
				UAM_ASSERT(file.good());
			}
		}
		app.chats.front().uam_agent_id = variation == "default" ? std::string{} : variation == "agentfailure" ? "missing-agent" : "workspace-47";
	}
	ScopedEnvVar durable_path("UAM_TEST_COLD_PRIMARY", AppPaths::UamChatFilePath(app.data_root, app.chats.front().id).string());
	const int history_count = variation == "receiptexit" ? 2 : 200;
	for (int index = 1; index < history_count; ++index)
		app.chats.front().messages.push_back({.role = MessageRole::Assistant,
		    .content = std::to_string(index) + std::string(128 * 1024, 'x')});
	std::string expected_hash = "opencode-cli:builtin-build-v1";
	if (app.chats.front().uam_agent_id == "workspace-47")
		expected_hash = "opencode-cli:fnv1a64:" + uam::hashing::Hex64Padded(uam::hashing::Fnv1a64(ReadFile(temp.root / ".uam/agents/workspace-47.md")));
	ScopedEnvVar binding_hash("UAM_TEST_COLD_AGENT_HASH", expected_hash);
	ScopedEnvVar exit_after_setup("UAM_TEST_COLD_EXIT_AFTER_SETUP", variation == "receiptexit" ? "1" : "0");
	struct Cleanup
	{
		uam::AppState& app;
		~Cleanup() { uam::FastStopAcpSessionsForExit(app); }
	} cleanup{app};
	std::string error;
	const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
	const bool accepted = uam::SendAcpPrompt(app, app.chats.front().id, "Launch with the private cold configuration", &error);
	const std::chrono::steady_clock::duration feedback = std::chrono::steady_clock::now() - started;
	UAM_ASSERT(accepted);
	UAM_ASSERT(feedback <= std::chrono::milliseconds(100));
	UAM_ASSERT(!app.acp_sessions.empty() && (app.acp_sessions.front()->pending_agent_input || app.acp_sessions.front()->pending_local_launch));
	if (app.acp_sessions.front()->pending_agent_input)
	{
		UAM_ASSERT_EQ(app.acp_sessions.front()->lifecycle_state, std::string("preparing_agent"));
		UAM_ASSERT_EQ(app.chats.front().messages.size(), static_cast<std::size_t>(history_count + 1));
		UAM_ASSERT_EQ(app.chats.front().messages.back().content, std::string("Launch with the private cold configuration"));
		UAM_ASSERT(app.chats.front().messages.back().acp_prompt_not_sent);
		UAM_ASSERT(app.chats.front().last_prompt_agent_definition_hash.empty());
		UAM_ASSERT(!fs::exists(provider_input));
	}
	if (variation == "prepare")
	{
		const std::chrono::steady_clock::time_point second_started = std::chrono::steady_clock::now();
		UAM_ASSERT(!uam::SendAcpPrompt(app, app.chats.front().id, "Hold additional input during definition preparation", &error));
		UAM_ASSERT(std::chrono::steady_clock::now() - second_started <= std::chrono::milliseconds(100));
		UAM_ASSERT_EQ(app.chats.front().messages.size(), std::size_t{201});
		const std::shared_ptr<std::optional<bool>> receipt = std::make_shared<std::optional<bool>>();
		UAM_ASSERT(uam::DeferChatSaveResult(app, app.chats.front().id, [receipt](bool saved) { *receipt = saved; }));
		const std::chrono::steady_clock::time_point deadline = started + std::chrono::seconds(10);
		while (!receipt->has_value() && std::chrono::steady_clock::now() < deadline)
		{
			const std::chrono::steady_clock::time_point tick = std::chrono::steady_clock::now();
			(void)uam::FlushPendingChatSaves(app);
			UAM_ASSERT(std::chrono::steady_clock::now() - tick <= std::chrono::milliseconds(100));
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		UAM_ASSERT(receipt->has_value() && **receipt);
		const std::optional<ChatSession> unbound = ChatRepository::LoadLocalChat(app.data_root, app.chats.front().id);
		UAM_ASSERT(unbound && unbound->messages.size() == 201 && unbound->messages.back().acp_prompt_not_sent);
		UAM_ASSERT(unbound->last_prompt_agent_definition_hash.empty() && unbound->acp_queued_prompts.empty());
		UAM_ASSERT_EQ(unbound->messages.back().content, std::string("Launch with the private cold configuration"));
		UAM_ASSERT(!fs::exists(provider_input));
	}
	if (variation == "source")
	{
		const std::chrono::steady_clock::time_point deadline = started + std::chrono::seconds(10);
		while (!uam::acp_detail::AgentInputPreparationReady(*app.acp_sessions.front()) && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		UAM_ASSERT(uam::acp_detail::AgentInputPreparationReady(*app.acp_sessions.front()));
		const fs::path source = temp.root / ".uam/agents/workspace-47.md";
		const fs::file_time_type stamp = fs::last_write_time(source);
		std::string replacement = ReadFile(source);
		replacement[replacement.size() - 2] = 'y';
		UAM_ASSERT(uam::io::WriteTextFile(source, replacement));
		fs::last_write_time(source, stamp);
	}
	if (!variation.empty() && variation != "catalog" && variation != "default" && variation != "prepare" && variation != "receiptstop" && variation != "receiptfailure" && variation != "completionnext" && variation != "receiptroot" && variation != "receiptrecreation" && variation != "receiptqueue" && variation != "receiptexit" && variation != "warm" && !variation.starts_with("busy"))
	{
		if (variation == "cancel") UAM_ASSERT(uam::CancelAcpTurn(app, app.chats.front().id));
		else if (variation == "stop") UAM_ASSERT(uam::StopAcpSession(app, app.chats.front().id));
		else if (variation == "recreation")
		{
			ChatSession replacement;
			replacement.id = app.chats.front().id;
			replacement.provider_id = app.chats.front().provider_id;
			replacement.workspace_directory = app.chats.front().workspace_directory;
			replacement.messages = std::move(app.chats.front().messages);
			app.chats.front() = std::move(replacement);
		}
		else if (variation == "settings") app.settings.central_provider_configuration.instructions = "Changed after admission";
		else if (variation == "workspace") app.chats.front().workspace_directory = (temp.root / "replacement-workspace").string();
		else if (variation == "root") app.data_root = temp.root / "replacement-data";
		else if (variation == "provider") app.chats.front().provider_id = "gemini-cli";
		else if (variation == "goal") app.chats.front().goal_command_revision = "replacement-goal-revision";
		else if (variation == "host") app.settings.execution_hosts.front().instruction_file = (temp.root / "changed-host.md").string();
		const std::chrono::steady_clock::time_point deadline = started + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline &&
		    (app.acp_sessions.front()->pending_agent_input || app.acp_sessions.front()->pending_local_launch ||
		     !app.agent_input_preparation_tasks.empty() || !app.local_acp_launch_tasks.empty()))
		{
			const std::chrono::steady_clock::time_point poll_started = std::chrono::steady_clock::now();
			(void)uam::PollAllAcpSessions(app);
			UAM_ASSERT(std::chrono::steady_clock::now() - poll_started <= std::chrono::milliseconds(100));
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		UAM_ASSERT(app.local_acp_launch_tasks.empty() && app.agent_input_preparation_tasks.empty());
		UAM_ASSERT(!app.acp_sessions.front()->pending_agent_input);
		UAM_ASSERT(!app.acp_sessions.front()->pending_local_launch);
		UAM_ASSERT(!app.acp_sessions.front()->running);
		UAM_ASSERT(!fs::exists(provider_input));
		UAM_ASSERT(!fs::exists(prompt_log));
		if (variation == "failure" || variation == "agentfailure") UAM_ASSERT(!app.acp_sessions.front()->last_error.empty());
		else if (variation == "source") UAM_ASSERT(app.acp_sessions.front()->last_error.find("sources changed") != std::string::npos);
		else if (variation != "cancel" && variation != "stop") UAM_ASSERT(app.acp_sessions.front()->last_error.find("ownership changed") != std::string::npos);
		UAM_ASSERT_EQ(app.chats.front().messages[199].content, std::to_string(199) + std::string(128 * 1024, 'x'));
		if (variation == "agentfailure" || variation == "source")
		{
			const std::chrono::steady_clock::time_point deadline = started + std::chrono::seconds(10);
			while (!app.pending_chat_save_at_by_chat_id.empty() && std::chrono::steady_clock::now() < deadline)
			{
				const std::chrono::steady_clock::time_point tick = std::chrono::steady_clock::now();
				(void)uam::FlushPendingChatSaves(app);
				UAM_ASSERT(std::chrono::steady_clock::now() - tick <= std::chrono::milliseconds(100));
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			const std::optional<ChatSession> unsent = ChatRepository::LoadLocalChat(app.data_root, app.chats.front().id);
			UAM_ASSERT(unsent && unsent->messages.size() == 201 && unsent->messages.back().acp_prompt_not_sent);
			UAM_ASSERT(unsent->last_prompt_agent_definition_hash.empty());
		}
		return;
	}
	bool completed = false;
	bool receipt_guard_exercised = false;
	bool completion_guard_exercised = false;
	const fs::path blocked_chats = app.data_root / "chats";
	const fs::path saved_chats = app.data_root / "fixture-saved-chats";
	const std::chrono::steady_clock::time_point deadline = started + std::chrono::seconds(10);
	while (std::chrono::steady_clock::now() < deadline)
	{
		const std::chrono::steady_clock::time_point poll_started = std::chrono::steady_clock::now();
		(void)uam::PollAllAcpSessions(app);
		UAM_ASSERT(std::chrono::steady_clock::now() - poll_started <= std::chrono::milliseconds(100));
		if (variation == "receiptexit" && app.acp_sessions.front()->pending_local_session_receipt &&
		    !app.acp_sessions.front()->running && !app.acp_sessions.front()->pending_local_launch &&
		    !app.acp_sessions.front()->pending_agent_input)
		{
			UAM_ASSERT(!fs::exists(prompt_log));
			UAM_ASSERT_EQ(app.chats.front().messages[2].content, std::string("Launch with the private cold configuration"));
			UAM_ASSERT(app.chats.front().messages[2].acp_prompt_not_sent);
			UAM_ASSERT(!app.acp_sessions.front()->processing);
			return;
		}
		if (variation == "receiptexit" && app.acp_sessions.front()->pending_local_session_receipt)
		{
			// Hold the full receipt pending to prove provider monitoring does not depend on publication.
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			continue;
		}
		if (!receipt_guard_exercised && app.acp_sessions.front()->pending_local_session_receipt &&
		    (variation == "receiptstop" || variation == "receiptfailure" || variation == "receiptroot" || variation == "receiptrecreation" || variation == "receiptqueue"))
		{
			receipt_guard_exercised = true;
			UAM_ASSERT(!fs::exists(prompt_log));
			if (variation == "receiptqueue")
			{
				const std::chrono::steady_clock::time_point queued_started = std::chrono::steady_clock::now();
				UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Launch with the private cold configuration", &error));
				UAM_ASSERT(std::chrono::steady_clock::now() - queued_started <= std::chrono::milliseconds(100));
				UAM_ASSERT(!fs::exists(prompt_log));
			}
			else if (variation == "receiptstop")
			{
				UAM_ASSERT(uam::StopAcpSession(app, app.chats.front().id));
				while (!app.pending_chat_save_at_by_chat_id.empty() && std::chrono::steady_clock::now() < deadline)
				{
					(void)uam::PollAllAcpSessions(app);
					(void)uam::FlushPendingChatSaves(app);
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				}
				UAM_ASSERT(!fs::exists(prompt_log));
				UAM_ASSERT(!app.acp_sessions.front()->running);
				UAM_ASSERT_EQ(app.chats.front().messages[200].content, std::string("Launch with the private cold configuration"));
				UAM_ASSERT(app.chats.front().messages[200].acp_prompt_not_sent);
				return;
			}
			if (variation == "receiptroot" || variation == "receiptrecreation")
			{
				if (variation == "receiptroot") app.data_root = temp.root / "replacement-data";
				else app.chats.front().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
				while (app.acp_sessions.front()->last_error.find("ownership changed") == std::string::npos &&
				    std::chrono::steady_clock::now() < deadline)
				{
					const std::chrono::steady_clock::time_point tick = std::chrono::steady_clock::now();
					(void)uam::PollAllAcpSessions(app);
					(void)uam::FlushPendingChatSaves(app);
					UAM_ASSERT(std::chrono::steady_clock::now() - tick <= std::chrono::milliseconds(100));
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				}
				UAM_ASSERT(app.acp_sessions.front()->last_error.find("ownership changed") != std::string::npos);
				UAM_ASSERT(!fs::exists(prompt_log));
				UAM_ASSERT_EQ(app.chats.front().messages[200].content, std::string("Launch with the private cold configuration"));
				return;
			}
			if (variation == "receiptfailure")
			{
				fs::rename(blocked_chats, saved_chats);
				UAM_ASSERT(uam::io::WriteTextFile(blocked_chats, "private storage failure"));
			}
		}
		if (variation == "completionnext" && !completion_guard_exercised &&
		    app.acp_sessions.front()->session_ready && !app.acp_sessions.front()->processing &&
		    app.chats.front().messages.size() >= 202)
		{
			completion_guard_exercised = true;
			UAM_ASSERT(app.pending_chat_save_at_by_chat_id.contains(app.chats.front().id));
			const std::chrono::steady_clock::time_point input_started = std::chrono::steady_clock::now();
			UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Launch with the private cold configuration", &error));
			UAM_ASSERT(std::chrono::steady_clock::now() - input_started <= std::chrono::milliseconds(100));
			const std::string before_receipt = ReadFile(prompt_log);
			UAM_ASSERT_EQ(std::count(before_receipt.begin(), before_receipt.end(), '\n'), 1);
		}
		const std::chrono::steady_clock::time_point flush_started = std::chrono::steady_clock::now();
		(void)uam::FlushPendingChatSaves(app);
		UAM_ASSERT(std::chrono::steady_clock::now() - flush_started <= std::chrono::milliseconds(100));
		if (variation != "receiptstop" && variation != "receiptfailure" && variation != "completionnext")
			UAM_ASSERT(std::chrono::steady_clock::now() - poll_started <= std::chrono::milliseconds(100));
		if (variation == "receiptfailure" && receipt_guard_exercised && fs::is_regular_file(blocked_chats) &&
		    !app.acp_sessions.front()->processing)
		{
			UAM_ASSERT(!fs::exists(prompt_log));
			UAM_ASSERT(app.chats.front().messages[200].acp_prompt_not_sent);
			UAM_ASSERT(app.chats.front().messages[200].interrupted);
			UAM_ASSERT_EQ(app.chats.front().messages[199].content, std::to_string(199) + std::string(128 * 1024, 'x'));
			fs::remove(blocked_chats);
			fs::rename(saved_chats, blocked_chats);
			const std::chrono::steady_clock::time_point retry_started = std::chrono::steady_clock::now();
			UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Launch with the private cold configuration", &error));
			UAM_ASSERT(std::chrono::steady_clock::now() - retry_started <= std::chrono::milliseconds(100));
		}
		if (variation.starts_with("busy") && app.acp_sessions.front()->session_ready &&
		    app.acp_sessions.front()->processing && app.chats.front().messages.size() >= 202 &&
		    fs::exists(prompt_log))
		{
			const std::string first_prompt = ReadFile(prompt_log);
			UAM_ASSERT_EQ(std::count(first_prompt.begin(), first_prompt.end(), '\n'), 1);
			const std::uint64_t first_turn = app.acp_sessions.front()->turn_serial;
			const int first_user = app.acp_sessions.front()->turn_user_message_index;
			const int first_assistant = app.acp_sessions.front()->current_assistant_message_index;
			const std::string first_lifecycle = app.acp_sessions.front()->lifecycle_state;
			const int first_request = app.acp_sessions.front()->prompt_request_id;
			if (variation == "busyfailure") app.chats.front().uam_agent_id = "missing-agent";
			const std::chrono::steady_clock::time_point queued_started = std::chrono::steady_clock::now();
			UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Queue input against the current selected catalogue", &error));
			UAM_ASSERT(std::chrono::steady_clock::now() - queued_started <= std::chrono::milliseconds(100));
			UAM_ASSERT(app.acp_sessions.front()->pending_agent_input);
			UAM_ASSERT_EQ(app.acp_sessions.front()->turn_serial, first_turn);
			UAM_ASSERT_EQ(app.acp_sessions.front()->turn_user_message_index, first_user);
			UAM_ASSERT_EQ(app.acp_sessions.front()->current_assistant_message_index, first_assistant);
			UAM_ASSERT_EQ(app.acp_sessions.front()->lifecycle_state, first_lifecycle);
			UAM_ASSERT_EQ(app.acp_sessions.front()->prompt_request_id, first_request);
			UAM_ASSERT_EQ(app.chats.front().messages.size(), std::size_t{203});
			UAM_ASSERT(app.chats.front().messages[202].acp_prompt_not_sent);
			if (variation == "busycancel")
			{
				// An unrelated retained queue item must survive the explicit preserve flag.
				uam::AcpQueuedUserPromptState unrelated;
				unrelated.id = "unrelated-held-input";
				unrelated.text = "Retain this independently owned queued input";
				app.acp_sessions.front()->queued_user_prompts.push_back(unrelated);
				app.chats.front().acp_queued_prompts = {unrelated};
				app.acp_sessions.front()->queue_held = app.chats.front().acp_queue_held = true;
				UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
				UAM_ASSERT(uam::CancelAcpTurn(app, app.chats.front().id, &error, true, true));
				UAM_ASSERT(!app.acp_sessions.front()->pending_agent_input);
				while ((ReadFile(provider_input).find("session/cancel") == std::string::npos ||
				    !app.pending_chat_save_at_by_chat_id.empty()) && std::chrono::steady_clock::now() < deadline)
				{
					const std::chrono::steady_clock::time_point tick = std::chrono::steady_clock::now();
					(void)uam::PollAllAcpSessions(app);
					(void)uam::FlushPendingChatSaves(app);
					UAM_ASSERT(std::chrono::steady_clock::now() - tick <= std::chrono::milliseconds(100));
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
				}
				UAM_ASSERT(ReadFile(provider_input).find("session/cancel") != std::string::npos);
				UAM_ASSERT(app.acp_sessions.front()->running);
				UAM_ASSERT(app.acp_sessions.front()->cancel_requested);
				UAM_ASSERT_EQ(app.acp_sessions.front()->turn_serial, first_turn);
				UAM_ASSERT_EQ(app.acp_sessions.front()->queued_user_prompts.size(), std::size_t{1});
				UAM_ASSERT_EQ(app.acp_sessions.front()->queued_user_prompts.front().id, unrelated.id);
				UAM_ASSERT(app.chats.front().messages[202].acp_prompt_not_sent && app.chats.front().messages[202].interrupted);
				UAM_ASSERT(app.chats.front().messages[201].interrupted);
				UAM_ASSERT(app.pending_chat_save_at_by_chat_id.empty());
				const std::optional<ChatSession> canceled_primary = ChatRepository::LoadLocalChat(app.data_root, app.chats.front().id);
				UAM_ASSERT(canceled_primary && canceled_primary->messages.size() == 203);
				UAM_ASSERT(canceled_primary->messages[202].acp_prompt_not_sent && canceled_primary->messages[202].interrupted);
				UAM_ASSERT_EQ(canceled_primary->acp_queued_prompts.size(), std::size_t{1});
				UAM_ASSERT_EQ(canceled_primary->acp_queued_prompts.front().id, unrelated.id);
				UAM_ASSERT_EQ(ReadFile(prompt_log), first_prompt);
				return;
			}
			if (variation == "busysource")
			{
				while (!uam::acp_detail::AgentInputPreparationReady(*app.acp_sessions.front()) && std::chrono::steady_clock::now() < deadline)
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				UAM_ASSERT(uam::acp_detail::AgentInputPreparationReady(*app.acp_sessions.front()));
				const fs::path source = temp.root / ".uam/agents/workspace-47.md";
				const fs::file_time_type stamp = fs::last_write_time(source);
				std::string replacement = ReadFile(source);
				replacement[replacement.size() - 2] = 'y';
				UAM_ASSERT(uam::io::WriteTextFile(source, replacement));
				fs::last_write_time(source, stamp);
			}
			IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
			const std::string chunk = "{\"method\":\"fixture/chunk\"}\n";
			UAM_ASSERT(service.WriteToStdioProcess(*app.acp_sessions.front(), chunk.data(), chunk.size(), &error));
			const std::chrono::steady_clock::time_point chunk_started = std::chrono::steady_clock::now();
			while (app.chats.front().messages[201].content.find("stream-during-agent-preparation") == std::string::npos &&
			    std::chrono::steady_clock::now() - chunk_started <= std::chrono::milliseconds(100))
			{
				const std::chrono::steady_clock::time_point tick = std::chrono::steady_clock::now();
				(void)uam::PollAllAcpSessions(app);
				(void)uam::FlushPendingChatSaves(app);
				UAM_ASSERT(std::chrono::steady_clock::now() - tick <= std::chrono::milliseconds(100));
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			UAM_ASSERT(app.chats.front().messages[201].content.find("stream-during-agent-preparation") != std::string::npos);
			while ((app.acp_sessions.front()->pending_agent_input || !app.pending_chat_save_at_by_chat_id.empty()) &&
			    std::chrono::steady_clock::now() < deadline)
			{
				const std::chrono::steady_clock::time_point tick = std::chrono::steady_clock::now();
				(void)uam::PollAllAcpSessions(app);
				(void)uam::FlushPendingChatSaves(app);
				UAM_ASSERT(std::chrono::steady_clock::now() - tick <= std::chrono::milliseconds(100));
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			UAM_ASSERT(!app.acp_sessions.front()->pending_agent_input && app.pending_chat_save_at_by_chat_id.empty());
			UAM_ASSERT(app.acp_sessions.front()->processing);
			UAM_ASSERT_EQ(app.acp_sessions.front()->turn_serial, first_turn);
			UAM_ASSERT_EQ(app.acp_sessions.front()->turn_user_message_index, first_user);
			UAM_ASSERT_EQ(app.acp_sessions.front()->current_assistant_message_index, first_assistant);
			UAM_ASSERT_EQ(app.acp_sessions.front()->prompt_request_id, first_request);
			UAM_ASSERT_EQ(ReadFile(prompt_log), first_prompt);
			const std::optional<ChatSession> queued_primary = ChatRepository::LoadLocalChat(app.data_root, app.chats.front().id);
			UAM_ASSERT(queued_primary && queued_primary->messages.size() == 203);
			UAM_ASSERT_EQ(queued_primary->messages[202].content, std::string("Queue input against the current selected catalogue"));
			if (variation == "busysource" || variation == "busyfailure")
			{
				UAM_ASSERT(queued_primary->messages[202].acp_prompt_not_sent && queued_primary->messages[202].interrupted);
				UAM_ASSERT(app.acp_sessions.front()->queued_user_prompts.empty());
				if (variation == "busysource") UAM_ASSERT(app.acp_sessions.front()->last_error.find("sources changed") != std::string::npos);
				if (variation == "busyfailure") UAM_ASSERT(!app.acp_sessions.front()->last_error.empty());
				return;
			}
			UAM_ASSERT_EQ(app.acp_sessions.front()->queued_user_prompts.size(), std::size_t{1});
			UAM_ASSERT_EQ(queued_primary->acp_queued_prompts.size(), std::size_t{1});
			const uam::AcpQueuedUserPromptState& restored = queued_primary->acp_queued_prompts.front();
			UAM_ASSERT_EQ(restored.existing_user_message_index, 202);
			UAM_ASSERT_EQ(restored.existing_user_message_created_at, queued_primary->messages[202].created_at);
			UAM_ASSERT(!restored.append_user_message && !restored.uam_agent_definition_hash.empty());
			if (variation == "busyreload" || variation.starts_with("busysummary") || variation.starts_with("busyrelaunch"))
			{
				app.acp_sessions.front()->queue_held = app.chats.front().acp_queue_held = true;
				UAM_ASSERT(uam::StopAcpSession(app, app.chats.front().id, uam::AcpStopPurpose::Interrupt, true));
				const std::optional<ChatSession> restart_primary = ChatRepository::LoadLocalChat(app.data_root, app.chats.front().id);
				UAM_ASSERT(restart_primary && restart_primary->messages.size() == 203 && restart_primary->acp_queued_prompts.size() == 1);
				ScopedEnvVar restarted_hold("UAM_TEST_COLD_HOLD_COMPLETION", "0");
				uam::AppState restarted;
				restarted.data_root = app.data_root;
				restarted.settings = app.settings;
				restarted.provider_profiles = app.provider_profiles;
				restarted.runtime_cli_versions_by_provider_id = app.runtime_cli_versions_by_provider_id;
				if (variation.starts_with("busysummary"))
				{
					const std::optional<ChatSession> summary = ChatRepository::LoadLocalChatSummaryReadOnly(app.data_root, app.chats.front().id);
					UAM_ASSERT(summary && !summary->messages_loaded && summary->messages.empty());
					UAM_ASSERT_EQ(summary->acp_queued_prompts.size(), std::size_t{1});
					UAM_ASSERT_EQ(summary->acp_queued_prompts.front().existing_user_message_index, 202);
					UAM_ASSERT_EQ(summary->acp_queued_prompts.front().existing_user_message_created_at, restored.existing_user_message_created_at);
					restarted.chats.push_back(*summary);
				}
				else restarted.chats.push_back(*restart_primary);
				Cleanup restarted_cleanup{restarted};
				uam::AcpSessionState& resumed = uam::acp_detail::EnsureAcpSessionForChat(restarted, restarted.chats.front());
				UAM_ASSERT(resumed.queue_held && resumed.queued_user_prompts.size() == 1);
				if (variation == "busyrelaunchfailure")
					restarted.settings.central_provider_configuration.instruction_files = {(temp.root / "missing-restart-instructions.md").string()};
				const std::chrono::steady_clock::time_point resume_started = std::chrono::steady_clock::now();
				UAM_ASSERT(uam::ResumeQueuedAcpPrompts(restarted, restarted.chats.front().id, {{restored.id, restored.revision}}, &error));
				UAM_ASSERT(std::chrono::steady_clock::now() - resume_started <= std::chrono::milliseconds(100));
				if (variation.starts_with("busysummary"))
				{
					UAM_ASSERT(!restarted.chats.front().messages_loaded && restarted.chats.front().messages.empty());
					UAM_ASSERT_EQ(ReadFile(prompt_log), first_prompt);
				}
				if (variation == "busysummaryroot" || variation == "busysummarystop" || variation == "busysummarystorage" || variation == "busysummaryrevision")
				{
					UAM_ASSERT(resumed.pending_queued_history);
					if (variation == "busysummaryroot") restarted.data_root = temp.root / "replacement-data-root";
					if (variation == "busysummarystop") UAM_ASSERT(uam::StopAcpSession(restarted, restarted.chats.front().id, uam::AcpStopPurpose::Interrupt, true));
					if (variation == "busysummaryrevision")
						UAM_ASSERT(uam::EditQueuedAcpPrompt(restarted, restarted.chats.front().id, restored.id, restored.revision,
						    "Revised pending input must not inherit the old raw-row authority", &error));
					if (variation == "busysummarystorage")
					{
						ChatSession replacement = *restart_primary;
						replacement.native_session_id = "replacement-native-session";
						UAM_ASSERT(ChatRepository::SaveChat(app.data_root, replacement));
					}
					const std::chrono::steady_clock::time_point guard_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
					while (resumed.pending_queued_history && std::chrono::steady_clock::now() < guard_deadline)
					{
						const std::chrono::steady_clock::time_point tick = std::chrono::steady_clock::now();
						(void)uam::PollAllAcpSessions(restarted);
						(void)uam::FlushPendingChatSaves(restarted);
						UAM_ASSERT(std::chrono::steady_clock::now() - tick <= std::chrono::milliseconds(100));
						std::this_thread::sleep_for(std::chrono::milliseconds(2));
					}
					UAM_ASSERT(!resumed.pending_queued_history);
					UAM_ASSERT(!restarted.chats.front().messages_loaded && restarted.chats.front().messages.empty());
					UAM_ASSERT(resumed.queue_held && resumed.queued_user_prompts.size() == 1);
					UAM_ASSERT_EQ(resumed.queued_user_prompts.front().existing_user_message_index, 202);
					UAM_ASSERT_EQ(resumed.queued_user_prompts.front().existing_user_message_created_at, restored.existing_user_message_created_at);
					UAM_ASSERT_EQ(ReadFile(prompt_log), first_prompt);
					const std::optional<ChatSession> retained = ChatRepository::LoadLocalChat(app.data_root, app.chats.front().id);
					UAM_ASSERT(retained && retained->messages.size() == 203);
					UAM_ASSERT_EQ(retained->messages[202].content, restored.text);
					UAM_ASSERT_EQ(retained->messages[202].created_at, restored.existing_user_message_created_at);
					UAM_ASSERT(retained->messages[202].acp_prompt_not_sent);
					return;
				}
				if (variation.starts_with("busyrelaunch"))
				{
					const std::chrono::steady_clock::time_point stage_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
					while (resumed.pending_referenced_queue_launch_fingerprint.empty() && std::chrono::steady_clock::now() < stage_deadline)
					{
						const std::chrono::steady_clock::time_point tick = std::chrono::steady_clock::now();
						(void)uam::PollAllAcpSessions(restarted);
						(void)uam::FlushPendingChatSaves(restarted);
						UAM_ASSERT(std::chrono::steady_clock::now() - tick <= std::chrono::milliseconds(100));
						std::this_thread::sleep_for(std::chrono::milliseconds(2));
					}
					UAM_ASSERT(!resumed.pending_referenced_queue_launch_fingerprint.empty());
					UAM_ASSERT(!resumed.running && resumed.queued_user_prompts.size() == 1);
					if (variation == "busyrelaunchsettings") restarted.chats.front().uam_agent_id = "replacement-agent";
					if (variation == "busyrelaunchqueue")
						UAM_ASSERT(uam::EditQueuedAcpPrompt(restarted, restarted.chats.front().id, restored.id, restored.revision,
						    "Revised queued input must not launch an obsolete agent snapshot", &error));
					while (!resumed.pending_referenced_queue_launch_fingerprint.empty() && std::chrono::steady_clock::now() < stage_deadline)
					{
						const std::chrono::steady_clock::time_point tick = std::chrono::steady_clock::now();
						(void)uam::PollAllAcpSessions(restarted);
						(void)uam::FlushPendingChatSaves(restarted);
						UAM_ASSERT(std::chrono::steady_clock::now() - tick <= std::chrono::milliseconds(100));
						std::this_thread::sleep_for(std::chrono::milliseconds(2));
					}
					UAM_ASSERT(resumed.pending_referenced_queue_launch_fingerprint.empty());
					UAM_ASSERT(!resumed.running && resumed.queue_held && resumed.queued_user_prompts.size() == 1);
					UAM_ASSERT_EQ(resumed.queued_user_prompts.front().id, restored.id);
					UAM_ASSERT_EQ(resumed.queued_user_prompts.front().existing_user_message_index, 202);
					UAM_ASSERT_EQ(resumed.queued_user_prompts.front().existing_user_message_created_at, restored.existing_user_message_created_at);
					UAM_ASSERT(restarted.chats.front().messages[202].acp_prompt_not_sent);
					UAM_ASSERT(!resumed.last_error.empty());
					UAM_ASSERT_EQ(ReadFile(prompt_log), first_prompt);
					return;
				}
				const std::chrono::steady_clock::time_point restart_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
				while (std::chrono::steady_clock::now() < restart_deadline)
				{
					const std::chrono::steady_clock::time_point tick = std::chrono::steady_clock::now();
					(void)uam::PollAllAcpSessions(restarted);
					(void)uam::FlushPendingChatSaves(restarted);

					UAM_ASSERT(std::chrono::steady_clock::now() - tick <= std::chrono::milliseconds(100));
					if (!resumed.processing && resumed.queued_user_prompts.empty() && restarted.pending_chat_save_at_by_chat_id.empty() &&
					    restarted.chats.front().messages.size() == 204) break;
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				}
				UAM_ASSERT(!resumed.processing && resumed.queued_user_prompts.empty());
				const std::optional<ChatSession> after_reload = ChatRepository::LoadLocalChat(restarted.data_root, restarted.chats.front().id);
				UAM_ASSERT(after_reload && after_reload->messages.size() == 204);
				UAM_ASSERT_EQ(after_reload->messages[202].content, restored.text);
				UAM_ASSERT_EQ(after_reload->messages[202].created_at, restored.existing_user_message_created_at);
				UAM_ASSERT_EQ(after_reload->native_session_id, restart_primary->native_session_id);
				UAM_ASSERT_EQ(after_reload->messages[199].content, restart_primary->messages[199].content);
				UAM_ASSERT_EQ(std::count_if(after_reload->messages.begin(), after_reload->messages.end(), [&restored](const Message& row)
				{
					return row.role == MessageRole::User && row.content == restored.text;
				}), 1);
				UAM_ASSERT(!after_reload->messages[202].acp_prompt_not_sent);
				const std::string after_restart_prompts = ReadFile(prompt_log);
				UAM_ASSERT_EQ(std::count(after_restart_prompts.begin(), after_restart_prompts.end(), '\n'), 2);
				UAM_ASSERT_EQ(resumed.active_uam_agent_definition_hash, restored.uam_agent_definition_hash);
				UAM_ASSERT(resumed.pending_referenced_queue_launch_fingerprint.empty());
				const std::string all_input = ReadFile(provider_input);
				const auto method_count = [&all_input](std::string_view method)
				{
					std::size_t count = 0, position = 0;
					while ((position = all_input.find(method, position)) != std::string::npos) { ++count; position += method.size(); }
					return count;
				};
				UAM_ASSERT_EQ(method_count("\"method\":\"session/new\""), std::size_t{1});
				UAM_ASSERT_EQ(method_count("\"method\":\"session/load\""), std::size_t{1});
				return;
			}
			if (variation == "busyrow") app.chats.front().messages[202].created_at = "replacement-row";
			const std::string release = "{\"method\":\"fixture/release\"}\n";
			UAM_ASSERT(service.WriteToStdioProcess(*app.acp_sessions.front(), release.data(), release.size(), &error));
			while (std::chrono::steady_clock::now() < deadline)
			{
				const std::chrono::steady_clock::time_point tick = std::chrono::steady_clock::now();
				(void)uam::PollAllAcpSessions(app);
				(void)uam::FlushPendingChatSaves(app);
				UAM_ASSERT(std::chrono::steady_clock::now() - tick <= std::chrono::milliseconds(100));
				if (variation == "busyrow" && !app.acp_sessions.front()->last_error.empty()) break;
				if (variation != "busyrow" && !app.acp_sessions.front()->processing && app.acp_sessions.front()->queued_user_prompts.empty() &&
				    app.pending_chat_save_at_by_chat_id.empty() && app.chats.front().messages.size() >= 204) break;
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			if (variation == "busyrow")
			{
				UAM_ASSERT(app.acp_sessions.front()->last_error.find("original unsent message changed") != std::string::npos);
				UAM_ASSERT_EQ(ReadFile(prompt_log), first_prompt);
				UAM_ASSERT_EQ(app.acp_sessions.front()->queued_user_prompts.size(), std::size_t{1});
				return;
			}
			UAM_ASSERT(!app.acp_sessions.front()->processing && app.acp_sessions.front()->queued_user_prompts.empty());
			const std::optional<ChatSession> after_queue = ChatRepository::LoadLocalChat(app.data_root, app.chats.front().id);
			UAM_ASSERT(after_queue && after_queue->messages.size() == 204);
			UAM_ASSERT_EQ(after_queue->messages[202].content, restored.text);
			UAM_ASSERT(!after_queue->messages[202].acp_prompt_not_sent);
			const std::string delivered = ReadFile(prompt_log);
			UAM_ASSERT_EQ(std::count(delivered.begin(), delivered.end(), '\n'), 2);
			return;
		}
		if (!app.acp_sessions.empty() && app.acp_sessions.front()->session_ready &&
		    !app.acp_sessions.front()->processing && app.chats.front().messages.size() >= 202 &&
		    app.pending_chat_save_at_by_chat_id.empty())
		{
			completed = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	if (variation.empty()) UAM_ASSERT_EQ(app.acp_sessions.front()->last_error, std::string{});
	UAM_ASSERT(completed);
	if (variation.empty()) UAM_ASSERT_EQ(app.acp_sessions.front()->current_model_id, app.chats.front().model_id);
	if (variation == "receiptfailure") UAM_ASSERT(receipt_guard_exercised && !fs::exists(saved_chats));
	if (variation == "completionnext") UAM_ASSERT(completion_guard_exercised);
	if (variation == "receiptqueue") UAM_ASSERT(receipt_guard_exercised);
	const std::optional<ChatSession> durable = ChatRepository::LoadLocalChat(app.data_root, app.chats.front().id);
	UAM_ASSERT(durable && durable->messages.size() >= 202);
	UAM_ASSERT_EQ(durable->messages.front().content, std::string("Previously durable history"));
	for (int index = 1; index < 200; ++index)
		UAM_ASSERT_EQ(durable->messages[static_cast<std::size_t>(index)].content, std::to_string(index) + std::string(128 * 1024, 'x'));
	UAM_ASSERT_EQ(durable->messages[200].content, std::string("Launch with the private cold configuration"));
	UAM_ASSERT_EQ(durable->messages.back().content, std::string("private-cold-launch-ok"));
	UAM_ASSERT_EQ(durable->last_prompt_agent_definition_hash, expected_hash);
	const std::string prompts = ReadFile(prompt_log);
	UAM_ASSERT_EQ(std::count(prompts.begin(), prompts.end(), '\n'), (variation == "completionnext" || variation == "receiptqueue") ? 2 : 1);
	if (variation == "receiptfailure")
	{
		UAM_ASSERT(durable->messages[200].acp_prompt_not_sent && durable->messages[200].interrupted);
		UAM_ASSERT_EQ(std::count_if(durable->messages.begin(), durable->messages.end(), [](const Message& row)
		    { return row.role == MessageRole::User && row.content == "Launch with the private cold configuration"; }), 2);
	}
	UAM_ASSERT(prompts.find("Launch with the private cold configuration") != std::string::npos);
	UAM_ASSERT(!app.acp_sessions.front()->provider_configuration_directory.empty());
	UAM_ASSERT(fs::exists(app.acp_sessions.front()->provider_configuration_directory / "skills/fixture/reference-2047.txt"));
	if (variation == "catalog" || variation == "prepare" || variation == "warm")
	{
		UAM_ASSERT_EQ(app.acp_sessions.front()->active_uam_agent_id, std::string("workspace-47"));
		UAM_ASSERT(!app.acp_sessions.front()->active_uam_agent_definition_hash.empty());
		const std::string selected = ReadFile(app.acp_sessions.front()->provider_configuration_directory / "agents/workspace-47.md");
		UAM_ASSERT(selected.find("Retain selected agent instructions workspace-47") != std::string::npos);
		UAM_ASSERT(selected.find(std::string(170 * 1024, 'x')) != std::string::npos);
	}
	if (variation == "warm")
	{
		UAM_ASSERT(app.acp_sessions.front()->running && app.acp_sessions.front()->session_ready);
		UAM_ASSERT(!app.acp_sessions.front()->pending_agent_input && !app.acp_sessions.front()->pending_local_launch);
		const std::chrono::steady_clock::time_point warm_started = std::chrono::steady_clock::now();
		UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Warm input against the current selected catalogue", &error));
		UAM_ASSERT(std::chrono::steady_clock::now() - warm_started <= std::chrono::milliseconds(100));
		UAM_ASSERT_EQ(ReadFile(prompt_log), prompts);
		const std::chrono::steady_clock::time_point warm_deadline = warm_started + std::chrono::seconds(10);
		while ((app.acp_sessions.front()->pending_agent_input || app.acp_sessions.front()->processing || !app.pending_chat_save_at_by_chat_id.empty()) &&
		    std::chrono::steady_clock::now() < warm_deadline)
		{
			const std::chrono::steady_clock::time_point tick = std::chrono::steady_clock::now();
			(void)uam::PollAllAcpSessions(app);
			(void)uam::FlushPendingChatSaves(app);
			UAM_ASSERT(std::chrono::steady_clock::now() - tick <= std::chrono::milliseconds(100));
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		UAM_ASSERT(!app.acp_sessions.front()->pending_agent_input && !app.acp_sessions.front()->processing && app.pending_chat_save_at_by_chat_id.empty());
		const std::optional<ChatSession> warm_durable = ChatRepository::LoadLocalChat(app.data_root, app.chats.front().id);
		UAM_ASSERT(warm_durable && warm_durable->messages.size() == 204);
		UAM_ASSERT_EQ(warm_durable->messages[202].content, std::string("Warm input against the current selected catalogue"));
		UAM_ASSERT(!warm_durable->messages[202].acp_prompt_not_sent);
		UAM_ASSERT_EQ(warm_durable->native_session_id, durable->native_session_id);
		const std::string warm_prompts = ReadFile(prompt_log);
		UAM_ASSERT_EQ(std::count(warm_prompts.begin(), warm_prompts.end(), '\n'), 2);
	}
#endif
}

UAM_TEST(AcpColdLocalConfigurationAdmissionKeepsInputAndPollBudgets)
{
	CheckColdLocalConfigurationAdmission({});
}

UAM_TEST(AcpColdLocalConfigurationCancellationRevokesLaunch)
{
	CheckColdLocalConfigurationAdmission("cancel");
	CheckColdLocalConfigurationAdmission("stop");
}

UAM_TEST(AcpColdLocalConfigurationRejectsRecreatedChat)
{
	CheckColdLocalConfigurationAdmission("recreation");
}

UAM_TEST(AcpColdLocalConfigurationFailureNeverStartsProvider)
{
	CheckColdLocalConfigurationAdmission("failure");
}

UAM_TEST(AcpColdLocalConfigurationRejectsChangedLaunchScope)
{
	for (const std::string_view variation : {"settings", "workspace", "root", "provider", "goal", "host"})
		CheckColdLocalConfigurationAdmission(variation);
}

UAM_TEST(AcpColdSelectedAgentAdmissionKeepsInputAndPollBudgets)
{
	CheckColdLocalConfigurationAdmission("catalog");
}

UAM_TEST(AcpColdDefaultAgentAdmissionKeepsInputAndPollBudgets)
{
	CheckColdLocalConfigurationAdmission("default");
}

UAM_TEST(AcpPreparingAgentRetainsUnboundDurableIntentAndRejectsAdditionalInput)
{
	CheckColdLocalConfigurationAdmission("prepare");
}

UAM_TEST(AcpPreparingAgentRejectsSameStampSourceChange)
{
	CheckColdLocalConfigurationAdmission("source");
}

UAM_TEST(AcpPreparingUnavailableAgentRetainsCompleteUnsentIntent)
{
	CheckColdLocalConfigurationAdmission("agentfailure");
}

UAM_TEST(AcpLocalSetupReceiptFailureRetainsIntentAndAllowsExplicitRetry)
{
	CheckColdLocalConfigurationAdmission("receiptfailure");
}

UAM_TEST(AcpLocalSetupReceiptStopPreventsPromptTransport)
{
	CheckColdLocalConfigurationAdmission("receiptstop");
}

UAM_TEST(AcpLocalCompletionSaveAllowsOwnedNextInput)
{
	CheckColdLocalConfigurationAdmission("completionnext");
}

UAM_TEST(AcpLocalSetupReceiptRejectsChangedDataRoot)
{
	CheckColdLocalConfigurationAdmission("receiptroot");
}

UAM_TEST(AcpLocalSetupReceiptRejectsRecreatedIdentity)
{
	CheckColdLocalConfigurationAdmission("receiptrecreation");
}

UAM_TEST(AcpLocalSetupReceiptRestagesAcceptedQueuedInput)
{
	CheckColdLocalConfigurationAdmission("receiptqueue");
}

UAM_TEST(AcpLocalSetupReceiptStillObservesProviderExit)
{
	CheckColdLocalConfigurationAdmission("receiptexit");
}

UAM_TEST(AcpWarmSelectedAgentSendKeepsInputBudget)
{
	CheckColdLocalConfigurationAdmission("warm");
}

UAM_TEST(AcpBusySelectedAgentEnqueueKeepsInputBudget)
{
	CheckColdLocalConfigurationAdmission("busy");
}

UAM_TEST(InitialPromptSaveFailureCannotRejectReplacementTurnOrIncarnation)
{
	for (int replacement = 0; replacement < 3; ++replacement)
	{
		PromptFixture fixture;
		uam::AppState& app = fixture.app;
		uam::AcpSessionState& session = *fixture.session;
		std::string error;
		UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Original deferred input", &error));
		if (replacement == 0) ++session.turn_serial;
		else if (replacement == 1) session.queued_prompt = "Replacement input";
		else app.chats.front().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		const std::string pending = session.queued_prompt;
		std::optional<bool> acknowledgement;
		UAM_ASSERT(uam::DeferChatSaveResult(app, app.chats.front().id,
		    [&acknowledgement](bool saved) { acknowledgement = saved; }));
		app.data_root = fixture.temp.root / "blocked-replacement-save";
		UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "Private blocked root"));
		UAM_ASSERT(!uam::acp_detail::SaveChatQuietly(app, app.chats.front()));
		UAM_ASSERT(acknowledgement.has_value() && !*acknowledgement);
		UAM_ASSERT(session.processing);
		UAM_ASSERT_EQ(session.queued_prompt, pending);
		UAM_ASSERT_EQ(session.prompt_request_id, 0);
	}
}

UAM_TEST(AcpBusyAgentPreparationCancelCancelsActiveTurn)
{
	CheckColdLocalConfigurationAdmission("busycancel");
}

UAM_TEST(AcpBusyAgentPreparationRejectsChangedDefinition)
{
	CheckColdLocalConfigurationAdmission("busysource");
}

UAM_TEST(AcpBusyPreparedAgentQueueRejectsChangedRawRow)
{
	CheckColdLocalConfigurationAdmission("busyrow");
}

UAM_TEST(AcpBusyPreparedAgentQueueResumesExactRawRowAfterReload)
{
	CheckColdLocalConfigurationAdmission("busyreload");
}

UAM_TEST(AcpBusyAgentPreparationFailurePreservesActiveTurn)
{
	CheckColdLocalConfigurationAdmission("busyfailure");
}

UAM_TEST(AcpDetachedPreparedAgentQueueResumesExactRawRowAfterRestart)
{
	CheckColdLocalConfigurationAdmission("busysummary");
}

UAM_TEST(BindingOnlyFullSaveFailurePreservesUndeliveredPromptIntent)
{
	PromptFixture fixture;
	uam::AppState& app = fixture.app;
	uam::AcpSessionState& session = *fixture.session;
	app.data_root = fixture.temp.root / "blocked-binding-save";
	UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "Private blocked root"));
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Held native objective", &error));
	const std::string prompt = session.queued_prompt;
	const std::filesystem::path root = app.data_root;
	const std::shared_ptr<const std::uint8_t> incarnation = app.chats.front().metadata_incarnation;
	std::optional<uam::acp_detail::DurableAdmissionState> receipt;
	int callbacks = 0;
	UAM_ASSERT(uam::acp_detail::BeginDurableAdmission(app, app.chats.front(), [&]()
	{
		return app.data_root == root && app.chats.front().metadata_incarnation == incarnation &&
		    session.processing && session.queued_prompt == prompt && session.prompt_request_id == 0;
	}, [&](uam::acp_detail::DurableAdmissionState result)
	{
		receipt = result;
		++callbacks;
	}, uam::acp_detail::DurableAdmissionFailurePurpose::BindingOnly) == uam::acp_detail::DurableAdmissionState::Pending);
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!receipt.has_value() && std::chrono::steady_clock::now() < deadline)
	{
		(void)uam::FlushPendingChatSaves(app);
		std::this_thread::yield();
	}
	UAM_ASSERT(receipt.has_value() && *receipt == uam::acp_detail::DurableAdmissionState::Failed);
	UAM_ASSERT_EQ(callbacks, 1);
	UAM_ASSERT(session.processing);
	UAM_ASSERT_EQ(session.queued_prompt, prompt);
	UAM_ASSERT_EQ(session.prompt_request_id, 0);
	UAM_ASSERT_EQ(ReadFile(fixture.saved_path), fixture.original);
}

UAM_TEST(MixedBindingAndPromptFullSaveFailureRetainsPromptRejection)
{
	PromptFixture fixture;
	uam::AppState& app = fixture.app;
	uam::AcpSessionState& session = *fixture.session;
	app.data_root = fixture.temp.root / "blocked-mixed-save";
	UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "Private blocked root"));
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(app, app.chats.front().id, "Owned prompt failure", &error));
	const std::string prompt = session.queued_prompt;
	const std::function<bool()> owned = [&]() { return session.processing && session.queued_prompt == prompt; };
	int callbacks = 0;
	const std::function<void(uam::acp_detail::DurableAdmissionState)> completion = [&](uam::acp_detail::DurableAdmissionState result)
	{
		UAM_ASSERT(result == uam::acp_detail::DurableAdmissionState::Failed);
		UAM_ASSERT(!session.processing && session.queued_prompt.empty());
		++callbacks;
	};
	UAM_ASSERT(uam::acp_detail::BeginDurableAdmission(app, app.chats.front(), owned, completion,
		uam::acp_detail::DurableAdmissionFailurePurpose::BindingOnly) == uam::acp_detail::DurableAdmissionState::Pending);
	UAM_ASSERT(uam::acp_detail::BeginDurableAdmission(app, app.chats.front(), owned, completion) == uam::acp_detail::DurableAdmissionState::Pending);
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (callbacks < 2 && std::chrono::steady_clock::now() < deadline)
	{
		(void)uam::FlushPendingChatSaves(app);
		std::this_thread::yield();
	}
	UAM_ASSERT_EQ(callbacks, 2);
	UAM_ASSERT_EQ(session.prompt_request_id, 0);
	UAM_ASSERT(app.chats.front().messages.back().acp_prompt_not_sent);
	UAM_ASSERT(app.chats.front().messages.back().interrupted);
	UAM_ASSERT_EQ(ReadFile(fixture.saved_path), fixture.original);
}

UAM_TEST(AcpDetachedQueueHistoryRejectsChangedRoot)
{
	CheckColdLocalConfigurationAdmission("busysummaryroot");
}

UAM_TEST(AcpDetachedQueueHistoryStopRetainsHeldRawRow)
{
	CheckColdLocalConfigurationAdmission("busysummarystop");
}

UAM_TEST(AcpDetachedQueueHistoryRejectsChangedStorageRevision)
{
	CheckColdLocalConfigurationAdmission("busysummarystorage");
}

UAM_TEST(AcpDetachedQueueHistoryRejectsChangedQueueRevision)
{
	CheckColdLocalConfigurationAdmission("busysummaryrevision");
}

UAM_TEST(AcpReferencedQueueLaunchFailureRetainsOwnedRawRow)
{
	CheckColdLocalConfigurationAdmission("busyrelaunchfailure");
}

UAM_TEST(AcpReferencedQueueLaunchRejectsChangedAgentScope)
{
	CheckColdLocalConfigurationAdmission("busyrelaunchsettings");
}

UAM_TEST(AcpReferencedQueueLaunchRejectsChangedQueueRevision)
{
	CheckColdLocalConfigurationAdmission("busyrelaunchqueue");
}

UAM_TEST(AcpSmallModelNativeSteerKeepsInputBudgetAndDurability)
{
	PromptFixture fixture;
	PrepareNativeSteer(fixture);
	uam::AppState& app = fixture.app;
	uam::AcpSessionState& session = *fixture.session;
	ChatSession& chat = app.chats.front();
	chat.small_model_mode = true;
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	const std::shared_ptr<PreparedChatSave> prepared = ChatRepository::PrepareChatSave(app.data_root, chat);
	UAM_ASSERT(prepared);
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
	const bool accepted = uam::SteerAcpPrompt(app, chat.id, "Small-model steer retains its parent turn", {}, {}, false, &error);
	const std::chrono::steady_clock::duration elapsed = std::chrono::steady_clock::now() - started;
	publication.join();
	UAM_ASSERT(accepted);
	UAM_ASSERT(elapsed <= std::chrono::milliseconds(100));
	UAM_ASSERT(session.processing && session.prompt_request_id == 17 && session.turn_serial == 7);
	UAM_ASSERT(session.pending_request_methods.empty());
	UAM_ASSERT_EQ(session.codex_turn_id, std::string("private-active-turn"));
	std::optional<bool> acknowledgement;
	UAM_ASSERT(uam::DeferChatSaveResult(app, chat.id, [&acknowledgement](bool saved) { acknowledgement = saved; }));
	UAM_ASSERT(!acknowledgement.has_value());
	AwaitSave(app);
	UAM_ASSERT(acknowledgement.has_value() && *acknowledgement);
	UAM_ASSERT_EQ(session.pending_request_methods.size(), std::size_t{1});
	UAM_ASSERT(session.processing && session.prompt_request_id == 17 && session.turn_serial == 7);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(saved && saved->small_model_mode && saved->messages.size() == 3);
	UAM_ASSERT_EQ(saved->messages[0].content, std::string("Previously saved history"));
	UAM_ASSERT_EQ(saved->messages[1].content, std::string("Parent output before steering"));
	UAM_ASSERT_EQ(saved->messages[2].content, std::string("Small-model steer retains its parent turn"));
	UAM_ASSERT(saved->messages[2].continues_turn && !saved->messages[2].acp_prompt_not_sent);
	UAM_ASSERT_EQ(std::ranges::count_if(session.diagnostics, [](const uam::AcpDiagnosticEntryState& item)
	    { return item.method == "turn/steer" && item.reason == "sent"; }), std::ptrdiff_t{1});
}

namespace
{
	void CheckSmallModelSteerRevocation(int mutation)
	{
		PromptFixture fixture;
		PrepareNativeSteer(fixture);
		uam::AppState& app = fixture.app;
		uam::AcpSessionState& session = *fixture.session;
		ChatSession& chat = app.chats.front();
		chat.small_model_mode = true;
		std::string error;
		const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
		UAM_ASSERT(uam::SteerAcpPrompt(app, chat.id, "Owned small-model steer pending receipt", {}, {}, false, &error));
		UAM_ASSERT(std::chrono::steady_clock::now() - started <= std::chrono::milliseconds(100));
		std::optional<bool> acknowledgement;
		int callback_count = 0;
		UAM_ASSERT(uam::DeferChatSaveResult(app, chat.id, [&acknowledgement, &callback_count](bool saved) { ++callback_count; acknowledgement = saved; }));
		const fs::path retained_root = fixture.temp.root / "retained-data";
		if (mutation == 0)
		{
			fs::rename(app.data_root, retained_root);
			UAM_ASSERT(uam::io::WriteTextFile(app.data_root, "Private unavailable storage"));
		}
		if (mutation == 1) chat.small_model_mode = false;
		if (mutation == 2) session.codex_turn_id = "replacement-native-turn";
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!acknowledgement.has_value() && std::chrono::steady_clock::now() < deadline)
		{
			const std::chrono::steady_clock::time_point tick = std::chrono::steady_clock::now();
			(void)uam::FlushPendingChatSaves(app);
			UAM_ASSERT(std::chrono::steady_clock::now() - tick <= std::chrono::milliseconds(100));
			std::this_thread::yield();
		}
		UAM_ASSERT(acknowledgement.has_value());
		UAM_ASSERT_EQ(*acknowledgement, mutation != 0);
		UAM_ASSERT_EQ(callback_count, 1);
		UAM_ASSERT(session.processing && session.prompt_request_id == 17 && session.turn_serial == 7);
		UAM_ASSERT(session.pending_request_methods.empty() && session.pending_local_steer_abandon.empty());
		UAM_ASSERT_EQ(chat.messages.size(), std::size_t{3});
		UAM_ASSERT(chat.messages.back().acp_prompt_not_sent && chat.messages.back().interrupted);
		if (mutation == 0)
		{
			fs::remove(app.data_root);
			fs::rename(retained_root, app.data_root);
		}
		AwaitSave(app);
		UAM_ASSERT(session.pending_request_methods.empty());
		const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, chat.id);
		UAM_ASSERT(saved && saved->messages.size() == 3 && saved->messages.back().acp_prompt_not_sent && saved->messages.back().interrupted);
		UAM_ASSERT_EQ(saved->messages.back().content, std::string("Owned small-model steer pending receipt"));
		UAM_ASSERT_EQ(callback_count, 1);
		UAM_ASSERT_EQ(saved->messages[0].content, std::string("Previously saved history"));
		UAM_ASSERT_EQ(std::ranges::count_if(session.diagnostics, [](const uam::AcpDiagnosticEntryState& item)
		    { return item.method == "turn/steer" && item.reason == "sent"; }), std::ptrdiff_t{0});
	}
}

UAM_TEST(AcpSmallModelSteerFailureRetainsIntentAndParent)
{
	CheckSmallModelSteerRevocation(0);
}
UAM_TEST(AcpSmallModelSteerRejectsChangedSmallModelScope)
{
	CheckSmallModelSteerRevocation(1);
}
UAM_TEST(AcpSmallModelSteerRejectsChangedNativeTurn)
{
	CheckSmallModelSteerRevocation(2);
}
