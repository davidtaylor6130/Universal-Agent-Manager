#include "test_harness.h"
#include "cef/state_serializer.h"
#include "common/chat/conversation_handoff.h"
#include "common/runtime/terminal/terminal_provider_cli.h"
#include "app/chat_search_service.h"

using namespace uam_test;

namespace
{
	struct HeldQueueFixture
	{
		TempDir temp{"uam-restored-queue-hold"};
		uam::AppState app;

		HeldQueueFixture()
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = {ProviderProfileStore::DefaultCodexProfile()};
			// A mistakenly admitted reconnect cannot execute a real provider in this fixture.
			app.provider_profiles.front().interactive_command.clear();
			app.runtime_cli_versions_by_provider_id["codex-cli"].checked = true;
			app.runtime_cli_versions_by_provider_id["codex-cli"].supported = true;
			app.runtime_cli_versions_by_provider_id["codex-cli"].installed_version = "0.160.0";
			ChatSession chat;
			chat.id = "held-queue";
			chat.provider_id = "codex-cli";
			chat.memory_enabled = false;
			for (const std::string& id : {"one", "two"})
			{
				uam::AcpQueuedUserPromptState prompt;
				prompt.id = id;
				prompt.text = id;
				prompt.markdown_store_prompt_blocks = {"IMMUTABLE_SKILL"};
				chat.acp_queued_prompts.push_back(prompt);
			}
			MessageAttachment attachment;
			attachment.id = "attachment";
			attachment.name = "fixture.txt";
			attachment.path = "fixture.txt";
			attachment.kind = "file";
			chat.acp_queued_prompts.front().attachments.push_back(attachment);
			app.chats.push_back(chat);
		}

		uam::AcpSessionState& Session() { return *app.acp_sessions.front(); }
	};
}

UAM_TEST(LegacyUnsentDiscardPreservesSubmittedTurnBranchesAndDurableQueue)
{
	HeldQueueFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	chat.messages = {
	    {.role = MessageRole::User, .content = "Old unsent input", .created_at = "2026-10-14T21:00:00Z", .interrupted = true, .acp_prompt_not_sent = true},
	    {.role = MessageRole::User, .content = "Submitted current input", .created_at = "2026-10-14T21:01:00Z"},
	    {.role = MessageRole::Assistant, .content = "Provider response", .created_at = "2026-10-14T21:02:00Z"}};
	chat.native_session_id = "original-native";
	chat.acp_queue_held = true;
	(void)uam::RestoreRemoteAcpSessionsAfterRestart(fixture.app);
	uam::AcpSessionState& session = fixture.Session();
	session.session_id = chat.native_session_id;
	session.session_ready = true;
	session.running = true;
	session.lifecycle_state = "error";
	session.process_execution_host_id = chat.execution_host_id;
	session.turn_user_message_index = 1;
	session.current_assistant_message_index = 2;
	session.turn_assistant_message_index = 2;
	chat.remote_active_turn_id = "later-rejected-provider-turn";
	chat.remote_turn_reconnect_pending = true;
	chat.remote_turn_user_message_index = 1;
	ChatSession child;
	child.id = "dependent-branch";
	child.parent_chat_id = chat.id;
	child.branch_from_message_index = 1;
	fixture.app.chats.push_back(child);
	ChatSession& current = fixture.app.chats.front();
	const ChatSession before = current;
	const std::string digest = uam::StateSerializer::MessageDigest(current);
	std::string error;
	UAM_ASSERT(uam::DiscardUnsentAcpMessage(fixture.app, current.id, 0, digest, &error));
	UAM_ASSERT_EQ(current.messages.size(), before.messages.size());
	UAM_ASSERT(current.messages[0].acp_prompt_discarded && current.messages[0].acp_prompt_not_sent);
	UAM_ASSERT_EQ(current.messages[0].content, before.messages[0].content);
	UAM_ASSERT(current.messages[1] == before.messages[1] && current.messages[2] == before.messages[2]);
	UAM_ASSERT(current.acp_queued_prompts == before.acp_queued_prompts);
	UAM_ASSERT(session.queue_held && current.acp_queue_held && session.running && !session.processing);
	UAM_ASSERT_EQ(session.turn_user_message_index, 1);
	UAM_ASSERT_EQ(fixture.app.chats.back().branch_from_message_index, 1);
	UAM_ASSERT_EQ(current.remote_active_turn_id, before.remote_active_turn_id);
	UAM_ASSERT(current.remote_turn_reconnect_pending && current.remote_turn_user_message_index == 1);
	UAM_ASSERT(uam::StateSerializer::MessageDigest(current) != digest);
	UAM_ASSERT(uam::StateSerializer::SerializeMessagePage(current, 10, std::nullopt, false)["messages"][0].value("acpPromptDiscarded", false));
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, current.id);
	UAM_ASSERT(saved.has_value() && saved->messages[0].acp_prompt_discarded);
	UAM_ASSERT(saved->messages[1] == before.messages[1] && saved->messages[2] == before.messages[2]);
	std::string context;
	UAM_ASSERT(uam::chat::BuildConversationHandoff(*saved, context));
	UAM_ASSERT(context.find("Old unsent input") == std::string::npos);
	UAM_ASSERT(uam::BuildProviderHandoffContext(*saved).find("Old unsent input") == std::string::npos);
	UAM_ASSERT(uam::BuildTemporarySideChatContext(saved->messages).find("Old unsent input") == std::string::npos);
	UAM_ASSERT(MemoryService::BuildWorkerPromptForTests(*saved, 0).find("Old unsent input") == std::string::npos);
	const ChatSession memory = MemoryService::CaptureWorkerInput(*saved, 0);
	UAM_ASSERT(memory.messages.size() == saved->messages.size() && memory.messages[0].content.empty() && memory.messages[0].acp_prompt_discarded);
	UAM_ASSERT_EQ(uam::StateSerializer::SerializeMessagePage(*saved, 10, std::nullopt, false)["userMessageIndices"], nlohmann::json::array({1}));
	UAM_ASSERT(!uam::DiscardUnsentAcpMessage(fixture.app, current.id, 1, uam::StateSerializer::MessageDigest(current), &error));
	UAM_ASSERT(!uam::DiscardUnsentAcpMessage(fixture.app, current.id, 0, digest, &error));
	// The fixture owns no real process despite its deliberately warm runtime flags.
	session.running = false;
}

UAM_TEST(LegacyUnsentDiscardLatestDraftSurvivesColdSaveAndCannotRetry)
{
	HeldQueueFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	chat.acp_queued_prompts.clear();
	chat.branch_root_chat_id = chat.id;
	chat.messages = {{.role = MessageRole::User, .content = "Never sent", .created_at = "2026-10-14T21:00:00Z", .interrupted = true, .acp_prompt_not_sent = true}};
	std::string error;
	UAM_ASSERT(uam::DiscardUnsentAcpMessage(fixture.app, chat.id, 0, uam::StateSerializer::MessageDigest(chat), &error));
	UAM_ASSERT(uam::RecoverableUnsentAcpMessageIndices(fixture.app, chat).empty());
	UAM_ASSERT(!uam::RetryFailedAcpMessage(fixture.app, chat.id, 0, &error));
	UAM_ASSERT(!uam::RetryLastAcpPrompt(fixture.app, chat.id, &error));
	UAM_ASSERT(fixture.app.acp_sessions.empty());
	std::optional<ChatSession> cold = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id, false);
	UAM_ASSERT(cold.has_value() && !cold->messages_loaded);
	cold->title = "Metadata save after removing input";
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, *cold));
	UAM_ASSERT(ChatRepository::HydrateChatMessages(fixture.app.data_root, *cold));
	UAM_ASSERT(cold->messages.size() == 1 && cold->messages[0].acp_prompt_discarded && cold->messages[0].acp_prompt_not_sent);
	cold->imported_read_only = true;
	UAM_ASSERT_EQ(uam::KnownUnsentAcpMessageIndices(fixture.app, *cold), std::vector<int>({0}));
	UAM_ASSERT(uam::RecoverableUnsentAcpMessageIndices(fixture.app, *cold).empty());
	cold->imported_read_only = false;
	cold->provider_id = "opencode-cli";
	cold->messages[0].provider = "codex-cli";
	UAM_ASSERT_EQ(uam::KnownUnsentAcpMessageIndices(fixture.app, *cold), std::vector<int>({0}));
	ChatSession native;
	native.messages = {{.role = MessageRole::Assistant, .content = "Native regrouping"}, cold->messages[0],
	    {.role = MessageRole::Assistant, .content = "A different response"},
	    {.role = MessageRole::User, .content = "Never sent", .created_at = "2026-10-14T21:05:00Z"}};
	native.messages[1].acp_prompt_discarded = native.messages[1].acp_prompt_not_sent = false;
	ChatHistorySyncService().OverlayPreparedLocalHistory(*cold, native);
	UAM_ASSERT(native.messages[1].acp_prompt_discarded && native.messages[1].acp_prompt_not_sent);
	UAM_ASSERT(!native.messages[3].acp_prompt_discarded && !native.messages[3].acp_prompt_not_sent);
}

UAM_TEST(LegacyUnsentDiscardExcludesLoadedAndSavedSearchWithoutMovingHits)
{
	HeldQueueFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	chat.acp_queued_prompts.clear();
	chat.messages = {{.role = MessageRole::User, .content = "discardedonly needle", .acp_prompt_not_sent = true, .acp_prompt_discarded = true},
	    {.role = MessageRole::User, .content = "Retained needle"}, {.role = MessageRole::Assistant, .content = "Delivered needle response"}};
	const std::string digest = uam::StateSerializer::MessageDigest(chat);
	UAM_ASSERT(uam::SearchChatMessages({chat}, {"discardedonly"})["chatIds"].empty());
	const nlohmann::json found = uam::SearchChatMessages({chat}, {"needle"});
	UAM_ASSERT_EQ(found["hits"].size(), std::size_t{2});
	UAM_ASSERT_EQ(found["hits"][0]["messageIndex"], 1);
	UAM_ASSERT_EQ(found["hits"][1]["messageIndex"], 2);
	UAM_ASSERT(!uam::ChatSearchTargetMatches(chat, digest, 0));
	UAM_ASSERT(uam::ChatSearchTargetMatches(chat, digest, 1));
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
	const std::optional<nlohmann::json> saved = uam::SearchSavedChatMessages(fixture.app.data_root, {"needle"}, []() { return false; });
	UAM_ASSERT(saved.has_value() && saved->at("hits").size() == 2 && saved->at("hits")[0]["messageIndex"] == 1);
}

UAM_TEST(LegacyUnsentDiscardRejectsUncertainOrChangedInputAndRollsBackFailedSave)
{
	for (const int guard : {0, 1, 2, 3, 4, 5, 6, 7, 8})
	{
		HeldQueueFixture fixture;
		ChatSession& chat = fixture.app.chats.front();
		chat.acp_queued_prompts.clear();
		chat.messages = {{.role = MessageRole::User, .content = "Retained draft", .created_at = "2026-10-14T21:00:00Z", .interrupted = true, .acp_prompt_not_sent = true}};
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
		const std::string digest = uam::StateSerializer::MessageDigest(chat);
		if (guard == 0) chat.imported_read_only = true;
		if (guard == 1) chat.remote_prompt_delivery_id = "uncertain-current-input";
		if (guard == 2) chat.messages[0].content = "Changed after display";
		if (guard == 3) chat.messages.push_back({.role = MessageRole::Assistant, .content = "Actual provider output"});
		if (guard == 4) chat.acp_queued_prompts.push_back({.text = chat.messages[0].content, .existing_user_message_index = 0});
		if (guard == 5) chat.acp_queued_prompts.push_back({.text = "Prepared wire input", .prepared_for_delivery = true});
		if (guard == 6) fixture.app.pending_chat_save_at_by_chat_id[chat.id] = 1.0;
		if (guard == 7)
		{
			fixture.app.data_root = fixture.temp.root / "blocked";
			UAM_ASSERT(uam::io::WriteTextFile(fixture.app.data_root, "blocked"));
		}
		if (guard == 8)
		{
			fixture.app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
			fixture.Session().chat_id = chat.id;
			fixture.Session().provider_id = "foreign-provider";
		}
		const ChatSession before = chat;
		std::string error;
		UAM_ASSERT(!uam::DiscardUnsentAcpMessage(fixture.app, chat.id, 0,
		    guard == 2 ? digest : uam::StateSerializer::MessageDigest(chat), &error));
		UAM_ASSERT(!error.empty());
		UAM_ASSERT(chat.messages == before.messages && chat.acp_queued_prompts == before.acp_queued_prompts);
		const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.temp.root / "data", chat.id);
		UAM_ASSERT(saved.has_value() && !saved->messages[0].acp_prompt_discarded);
	}
}

UAM_TEST(RestartHoldsUndispatchedQueueDurablyAndFailedHoldSaveCannotLaunch)
{
	for (const bool save_fails : {false, true})
	{
		HeldQueueFixture fixture;
		ChatSession& chat = fixture.app.chats.front();
		chat.acp_dispatched_queued_prompt_count = 1;
		if (save_fails)
		{
			fixture.app.data_root = fixture.temp.root / "blocked";
			UAM_ASSERT(uam::io::WriteTextFile(fixture.app.data_root, "blocked"));
		}
		UAM_ASSERT_EQ(uam::RestoreRemoteAcpSessionsAfterRestart(fixture.app), std::size_t{1});
		uam::AcpSessionState& session = fixture.Session();
		UAM_ASSERT(session.queue_held && chat.acp_queue_held);
		UAM_ASSERT(!session.reconnect_pending && !session.running && !session.processing);
		UAM_ASSERT_EQ(session.queued_user_prompts.size(), std::size_t{1});
		UAM_ASSERT_EQ(session.queued_user_prompts.front().id, std::string("two"));
		UAM_ASSERT(!uam::DrainNextQueuedAcpUserPrompt(fixture.app, session, chat));
		(void)uam::PollAllAcpSessions(fixture.app);
		UAM_ASSERT(!session.running && !session.processing);
		UAM_ASSERT_EQ(session.turn_serial, 0);
		UAM_ASSERT(uam::StateSerializer::Serialize(fixture.app)["chats"][0]["acpSession"].value("queueHeld", false));
		if (!save_fails)
		{
			const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
			UAM_ASSERT(saved.has_value() && saved->acp_queue_held);
			UAM_ASSERT_EQ(saved->acp_queued_prompts.size(), std::size_t{1});
			fixture.app.chats = ChatRepository::LoadLocalChatSummaries(fixture.app.data_root);
			UAM_ASSERT(ChatRepository::HydrateChatMessages(fixture.app.data_root, fixture.app.chats.front()));
			UAM_ASSERT(fixture.app.chats.front().acp_queue_held);
			// Quitting without Resume must keep the held intent available on the next restart.
			uam::FastStopAcpSessionsForExit(fixture.app);
			const std::optional<ChatSession> after_exit = ChatRepository::LoadLocalChat(fixture.app.data_root, "held-queue");
			UAM_ASSERT(after_exit.has_value() && after_exit->acp_queue_held);
			UAM_ASSERT_EQ(after_exit->acp_queued_prompts.size(), std::size_t{1});
			UAM_ASSERT_EQ(after_exit->acp_queued_prompts.front().id, std::string("two"));
			UAM_ASSERT(after_exit->messages.empty());
			fixture.app.chats = ChatRepository::LoadLocalChatSummaries(fixture.app.data_root);
			(void)uam::RestoreRemoteAcpSessionsAfterRestart(fixture.app);
			UAM_ASSERT(fixture.Session().queue_held && !fixture.Session().reconnect_pending);
			UAM_ASSERT_EQ(fixture.Session().queued_user_prompts.front().id, std::string("two"));
		}
	}
}

UAM_TEST(TransportRecoveryDoesNotNewlyHoldQueuesOrReplayAcceptedRemotePrefix)
{
	HeldQueueFixture retry;
	UAM_ASSERT_EQ(uam::RestoreRemoteAcpSessionsAfterRestart(retry.app, false), std::size_t{1});
	UAM_ASSERT(!retry.Session().queue_held && retry.Session().reconnect_pending);
	UAM_ASSERT(!retry.app.chats.front().acp_queue_held);
	// Retrying another chat's hydration must not rebuild an existing live outbox.
	retry.Session().queued_user_prompts.front().text = "new live queue snapshot";
	(void)uam::RestoreRemoteAcpSessionsAfterRestart(retry.app, false);
	UAM_ASSERT_EQ(retry.Session().queued_user_prompts.front().text, std::string("new live queue snapshot"));

	HeldQueueFixture accepted;
	ChatSession& chat = accepted.app.chats.front();
	chat.execution_host_id = "missing-remote-host";
	chat.remote_turn_reconnect_pending = true;
	chat.remote_process_exists = true;
	chat.remote_active_turn_id = "accepted-native-turn";
	chat.remote_turn_serial = 7;
	chat.acp_dispatched_queued_prompt_count = 1;
	chat.acp_queued_prompts.front().prepared_for_delivery = true;
	UAM_ASSERT(ChatRepository::SaveChat(accepted.app.data_root, chat));
	(void)uam::RestoreRemoteAcpSessionsAfterRestart(accepted.app);
	uam::AcpSessionState& session = accepted.Session();
	UAM_ASSERT(session.recovering_remote_turn && session.recovering_remote_process && session.processing);
	UAM_ASSERT(session.queue_held && chat.acp_queue_held);
	UAM_ASSERT_EQ(session.turn_serial, 7);
	UAM_ASSERT_EQ(session.queued_user_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(session.queued_user_prompts.front().id, std::string("two"));
	UAM_ASSERT(chat.remote_turn_reconnect_pending);
	UAM_ASSERT_EQ(chat.acp_queued_prompts.front().id, std::string("one"));
	uam::FastStopAcpSessionsForExit(accepted.app);
	const std::optional<ChatSession> after_exit = ChatRepository::LoadLocalChat(accepted.app.data_root, "held-queue");
	UAM_ASSERT(after_exit.has_value() && after_exit->acp_queue_held && after_exit->remote_turn_reconnect_pending);
	UAM_ASSERT_EQ(after_exit->acp_dispatched_queued_prompt_count, std::size_t{1});
	UAM_ASSERT_EQ(after_exit->acp_queued_prompts.size(), std::size_t{2});
	UAM_ASSERT_EQ(after_exit->acp_queued_prompts.front().id, std::string("one"));
	UAM_ASSERT(after_exit->acp_queued_prompts.front().prepared_for_delivery);
	UAM_ASSERT_EQ(after_exit->acp_queued_prompts.back().id, std::string("two"));
	accepted.app.chats = ChatRepository::LoadLocalChatSummaries(accepted.app.data_root);
	(void)uam::RestoreRemoteAcpSessionsAfterRestart(accepted.app);
	UAM_ASSERT(accepted.Session().queue_held && accepted.Session().recovering_remote_turn);
	UAM_ASSERT_EQ(accepted.Session().queued_user_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(accepted.Session().queued_user_prompts.front().id, std::string("two"));
	for (const bool restart : {false, true})
	for (const bool save_fails : {false, true})
	{
		HeldQueueFixture cleanup;
		ChatSession& cleanup_chat = cleanup.app.chats.front();
		cleanup_chat.execution_host_id = "missing-remote-host";
		cleanup_chat.remote_stop_cleanup_pending = true;
		cleanup_chat.remote_restart_pending = restart;
		cleanup_chat.remote_process_exists = true;
		UAM_ASSERT(ChatRepository::SaveChat(cleanup.app.data_root, cleanup_chat));
		(void)uam::RestoreRemoteAcpSessionsAfterRestart(cleanup.app);
		uam::AcpSessionState& cleanup_session = cleanup.Session();
		UAM_ASSERT(cleanup_session.queue_held);
		UAM_ASSERT_EQ(cleanup_session.queued_user_prompts.size(), std::size_t{2});
		UAM_ASSERT(cleanup_session.restart_after_remote_stop_cleanup);

#if defined(_WIN32)
		const std::vector<std::string> argv = {"cmd.exe", "/d", "/s", "/c", "set /p line= & exit /b 0"};
#else
		const std::vector<std::string> argv = {"/bin/sh", "-c", "IFS= read -r line; exit 0"};
#endif
		std::string cleanup_error;
		UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(cleanup_session, cleanup.temp.root, argv, &cleanup_error));
		cleanup_session.running = true;
		struct CleanupStub
		{
			uam::AppState& app;
			~CleanupStub() { uam::FastStopAcpSessionsForExit(app); }
		} cleanup_stub{cleanup.app};
		UAM_ASSERT(!uam::StopAcpSession(cleanup.app, cleanup_chat.id, uam::AcpStopPurpose::Interrupt, true));
		UAM_ASSERT(cleanup_session.remote_stop_pending && cleanup_session.queue_held);
		UAM_ASSERT_EQ(cleanup_session.queued_user_prompts.size(), std::size_t{2});
		UAM_ASSERT_EQ(cleanup.app.pending_acp_remote_stops.size(), std::size_t{1});
		const fs::path good_root = cleanup.app.data_root;
		if (save_fails)
		{
			cleanup.app.data_root = cleanup.temp.root / "blocked";
			UAM_ASSERT(uam::io::WriteTextFile(cleanup.app.data_root, "blocked"));
		}
		const double deadline = uam::GetAppTimeSeconds() + 2.0;
		while (!cleanup.app.pending_acp_remote_stops.empty() && uam::GetAppTimeSeconds() < deadline)
		{
			(void)uam::PollAllAcpSessions(cleanup.app);
			std::this_thread::yield();
		}
		UAM_ASSERT(cleanup.app.pending_acp_remote_stops.empty());
		UAM_ASSERT(cleanup_session.queue_held && cleanup_chat.acp_queue_held);
		UAM_ASSERT_EQ(cleanup_session.queued_user_prompts.size(), std::size_t{2});
		UAM_ASSERT(!cleanup_session.reconnect_pending && !cleanup_session.running);
		UAM_ASSERT(!cleanup_chat.remote_stop_cleanup_pending && !cleanup_chat.remote_restart_pending);
		if (save_fails)
		{
			UAM_ASSERT(cleanup.app.pending_chat_save_at_by_chat_id.contains(cleanup_chat.id));
			cleanup.app.data_root = good_root;
			uam::FlushPendingChatSaves(cleanup.app, true);
		}
		const std::optional<ChatSession> saved_cleanup = ChatRepository::LoadLocalChat(cleanup.app.data_root, "held-queue");
		UAM_ASSERT(saved_cleanup.has_value() && saved_cleanup->acp_queue_held);
		UAM_ASSERT_EQ(saved_cleanup->acp_queued_prompts.size(), std::size_t{2});
		UAM_ASSERT(saved_cleanup->messages.empty());
	}

}

UAM_TEST(HeldQueueAllowsEditsAndRemovalButSteeringAndFailedLastRemovalStayHeld)
{
	HeldQueueFixture fixture;
	(void)uam::RestoreRemoteAcpSessionsAfterRestart(fixture.app);
	std::string error;
	UAM_ASSERT(!uam::SteerQueuedAcpPrompt(fixture.app, "held-queue", 0, &error, std::make_pair(std::string("one"), 1)));
	UAM_ASSERT(!uam::SteerAcpPrompt(fixture.app, "held-queue", "bypass", {}, {}, false, &error));
	UAM_ASSERT(uam::EditQueuedAcpPrompt(fixture.app, "held-queue", "one", 1, "edited", &error));
	UAM_ASSERT(uam::ReorderQueuedAcpPrompts(fixture.app, "held-queue", {{"one", 2}, {"two", 1}}, {"two", "one"}, &error));
	UAM_ASSERT(fixture.Session().queue_held);
	UAM_ASSERT_EQ(fixture.Session().queued_user_prompts.back().attachments.size(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.Session().queued_user_prompts.back().markdown_store_prompt_blocks.front(), std::string("IMMUTABLE_SKILL"));
	UAM_ASSERT(uam::RemoveQueuedAcpPrompt(fixture.app, "held-queue", 0, &error, std::make_pair(std::string("two"), 1)));
	const fs::path good_root = fixture.app.data_root;
	fixture.app.data_root = fixture.temp.root / "blocked";
	UAM_ASSERT(uam::io::WriteTextFile(fixture.app.data_root, "blocked"));
	UAM_ASSERT(!uam::RemoveQueuedAcpPrompt(fixture.app, "held-queue", 0, &error, std::make_pair(std::string("one"), 2)));
	UAM_ASSERT(fixture.Session().queue_held && fixture.app.chats.front().acp_queue_held);
	UAM_ASSERT_EQ(fixture.Session().queued_user_prompts.size(), std::size_t{1});
	fixture.app.data_root = good_root;
	UAM_ASSERT(uam::RemoveQueuedAcpPrompt(fixture.app, "held-queue", 0, &error, std::make_pair(std::string("one"), 2)));
	UAM_ASSERT(!fixture.Session().queue_held && !fixture.app.chats.front().acp_queue_held);
	UAM_ASSERT(!fixture.Session().reconnect_pending);
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(good_root, "held-queue");
	UAM_ASSERT(saved.has_value() && !saved->acp_queue_held && saved->acp_queued_prompts.empty());
	for (const bool stop : {false, true})
	{
		HeldQueueFixture discard;
		(void)uam::RestoreRemoteAcpSessionsAfterRestart(discard.app);
		const fs::path good = discard.app.data_root;
		discard.app.data_root = discard.temp.root / "blocked";
		UAM_ASSERT(uam::io::WriteTextFile(discard.app.data_root, "blocked"));
		UAM_ASSERT(!(stop ? uam::StopAcpSession(discard.app, "held-queue") : uam::CancelAcpTurn(discard.app, "held-queue", &error, false, false)));
		UAM_ASSERT(discard.Session().queue_held && discard.app.chats.front().acp_queue_held);
		UAM_ASSERT_EQ(discard.Session().queued_user_prompts.size(), std::size_t{2});
		UAM_ASSERT_EQ(discard.app.chats.front().acp_queued_prompts.size(), std::size_t{2});
		UAM_ASSERT(discard.app.chats.front().messages.empty());
		discard.app.data_root = good;
		UAM_ASSERT(stop ? uam::StopAcpSession(discard.app, "held-queue") : uam::CancelAcpTurn(discard.app, "held-queue", &error, false, false));
		UAM_ASSERT(!discard.Session().queue_held && !discard.app.chats.front().acp_queue_held);
		UAM_ASSERT(discard.Session().queued_user_prompts.empty());
		const std::optional<ChatSession> saved_discard = ChatRepository::LoadLocalChat(good, "held-queue");
		UAM_ASSERT(saved_discard.has_value() && !saved_discard->acp_queue_held && saved_discard->acp_queued_prompts.empty());
		UAM_ASSERT_EQ(saved_discard->messages.size(), stop ? std::size_t{2} : std::size_t{0});
		if (stop) UAM_ASSERT(saved_discard->messages.front().interrupted && saved_discard->messages.front().acp_prompt_not_sent);
	}

	for (const bool remove : {true, false})
	{
		HeldQueueFixture pending;
		pending.app.settings.provider_chat_defaults["codex-cli"].feature_preference = "provider";
		(void)uam::RestoreRemoteAcpSessionsAfterRestart(pending.app);
		uam::AcpSessionState& session = pending.Session();
		ChatSession& chat = pending.app.chats.front();
		session.queued_user_prompts.pop_back();
		session.processing = true;
		session.queued_prompt = "Unconfirmed native delivery";
		chat.execution_host_id = "missing-remote-host";
		uam::AcpQueuedUserPromptState prefix;
		prefix.id = "prepared-prefix";
		prefix.text = session.queued_prompt;
		prefix.prepared_for_delivery = true;
		chat.acp_queued_prompts.assign(session.queued_user_prompts.begin(), session.queued_user_prompts.end());
		chat.acp_queued_prompts.insert(chat.acp_queued_prompts.begin(), prefix);
		chat.acp_dispatched_queued_prompt_count = 1;
		UAM_ASSERT(ChatRepository::SaveChat(pending.app.data_root, chat));
		const fs::path good = pending.app.data_root;
		pending.app.data_root = pending.temp.root / "blocked";
		UAM_ASSERT(uam::io::WriteTextFile(pending.app.data_root, "blocked"));
		UAM_ASSERT(!(remove ? uam::RemoveQueuedAcpPrompt(pending.app, chat.id, 0, &error, std::make_pair(std::string("one"), 1))
		                   : uam::SendAcpPrompt(pending.app, chat.id, "New held intent", &error)));
		UAM_ASSERT_EQ(error, std::string("Queued prompts could not be saved."));
		UAM_ASSERT(session.queue_held && chat.acp_queue_held);
		UAM_ASSERT_EQ(session.queued_user_prompts.size(), std::size_t{1});
		UAM_ASSERT_EQ(session.queued_user_prompts.front().id, std::string("one"));
		UAM_ASSERT_EQ(session.queued_user_prompts.front().revision, 1);
		UAM_ASSERT_EQ(chat.acp_queued_prompts.size(), std::size_t{2});
		UAM_ASSERT_EQ(chat.acp_queued_prompts.front().id, std::string("prepared-prefix"));
		UAM_ASSERT(chat.acp_queued_prompts.front().prepared_for_delivery);
		UAM_ASSERT_EQ(chat.acp_dispatched_queued_prompt_count, std::size_t{1});
		pending.app.data_root = good;
		uam::FlushPendingChatSaves(pending.app, true);
		const std::optional<ChatSession> repaired = ChatRepository::LoadLocalChat(good, chat.id);
		UAM_ASSERT(repaired.has_value() && repaired->acp_queue_held);
		UAM_ASSERT_EQ(repaired->acp_queued_prompts.size(), std::size_t{2});
		UAM_ASSERT_EQ(repaired->acp_queued_prompts.front().id, std::string("prepared-prefix"));
		UAM_ASSERT(repaired->acp_queued_prompts.front().prepared_for_delivery);
		UAM_ASSERT_EQ(repaired->acp_dispatched_queued_prompt_count, std::size_t{1});
		UAM_ASSERT_EQ(repaired->acp_queued_prompts.back().id, std::string("one"));
		UAM_ASSERT_EQ(repaired->acp_queued_prompts.back().attachments.size(), std::size_t{1});
	}

}

UAM_TEST(HeldQueueResumeRequiresExactSnapshotAndDurableAdmissionBeforeReconnect)
{
	HeldQueueFixture fixture;
	(void)uam::RestoreRemoteAcpSessionsAfterRestart(fixture.app);
	std::string error;
	const std::vector<std::pair<std::string, int>> expected = {{"one", 1}, {"two", 1}};
	UAM_ASSERT(!uam::ResumeQueuedAcpPrompts(fixture.app, "held-queue", {{"two", 1}, {"one", 1}}, &error));
	UAM_ASSERT(!uam::ResumeQueuedAcpPrompts(fixture.app, "held-queue", {{"one", 2}, {"two", 1}}, &error));
	UAM_ASSERT(!uam::ResumeQueuedAcpPrompts(fixture.app, "held-queue", {{"one", 1}}, &error));
	fixture.Session().queued_user_prompts.front().prepared_for_delivery = true;
	UAM_ASSERT(!uam::ResumeQueuedAcpPrompts(fixture.app, "held-queue", expected, &error));
	fixture.Session().queued_user_prompts.front().prepared_for_delivery = false;
	const fs::path good_root = fixture.app.data_root;
	fixture.app.data_root = fixture.temp.root / "blocked";
	UAM_ASSERT(uam::io::WriteTextFile(fixture.app.data_root, "blocked"));
	UAM_ASSERT(!uam::ResumeQueuedAcpPrompts(fixture.app, "held-queue", expected, &error));
	UAM_ASSERT(fixture.Session().queue_held && fixture.app.chats.front().acp_queue_held);
	UAM_ASSERT(!fixture.Session().reconnect_pending && !fixture.Session().processing);
	fixture.app.data_root = good_root;
	UAM_ASSERT(uam::ResumeQueuedAcpPrompts(fixture.app, "held-queue", expected, &error));
	UAM_ASSERT(!fixture.Session().queue_held && !fixture.app.chats.front().acp_queue_held);
	UAM_ASSERT(fixture.Session().reconnect_pending && !fixture.Session().running && !fixture.Session().processing);
	UAM_ASSERT_EQ(fixture.Session().queued_user_prompts.size(), std::size_t{2});
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(good_root, "held-queue");
	UAM_ASSERT(saved.has_value() && !saved->acp_queue_held);
	UAM_ASSERT_EQ(saved->acp_queued_prompts.front().attachments.size(), std::size_t{1});
	UAM_ASSERT(!uam::ResumeQueuedAcpPrompts(fixture.app, "held-queue", expected, &error));
}

UAM_TEST(LegacyUnsentProjectionSerializesNativeIndicesAndRevokesRetryAuthority)
{
	HeldQueueFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	chat.acp_queued_prompts.clear();
	for (const std::string& text : {"earlier failed input", "latest failed input"})
	{
		Message row;
		row.role = MessageRole::User;
		row.content = text;
		row.acp_prompt_not_sent = true;
		row.interrupted = true;
		chat.messages.push_back(row);
	}
	const auto snapshot = [&fixture]()
	{
		return uam::StateSerializer::Serialize(fixture.app)["chats"][0]["acpSession"];
	};
	UAM_ASSERT(snapshot()["unsentMessageIndices"] == nlohmann::json::array({0, 1}));
	UAM_ASSERT(snapshot()["recoverableUnsentMessageIndices"] == nlohmann::json::array({1}));
	chat.remote_process_exists = true;
	UAM_ASSERT(snapshot()["recoverableUnsentMessageIndices"].empty());
	chat.remote_process_exists = false;
	chat.messages.back().acp_prompt_not_sent = false;
	Message answer;
	answer.role = MessageRole::Assistant;
	answer.content = "Delivered response";
	chat.messages.push_back(answer);
	UAM_ASSERT(snapshot()["unsentMessageIndices"] == nlohmann::json::array({0}));
	UAM_ASSERT(snapshot()["recoverableUnsentMessageIndices"].empty());
	chat.imported_read_only = true;
	UAM_ASSERT(snapshot()["unsentMessageIndices"].empty());
	UAM_ASSERT(snapshot()["recoverableUnsentMessageIndices"].empty());
}

UAM_TEST(HeldQueueSerializesOnlyNonnegativeReferencedNativeIndices)
{
	HeldQueueFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	chat.acp_queued_prompts.front().existing_user_message_index = 23;
	chat.acp_queued_prompts.front().append_user_message = false;
	const nlohmann::json summary = uam::StateSerializer::Serialize(fixture.app)["chats"][0]["acpSession"];
	UAM_ASSERT(summary["queuedPrompts"][0]["existingUserMessageIndex"] == 23);
	UAM_ASSERT(!summary["queuedPrompts"][1].contains("existingUserMessageIndex"));
}
