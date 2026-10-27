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
		// Obstruct only the primary: the stop owner captured this data root and must still match it.
		const fs::path primary = AppPaths::UamChatFilePath(cleanup.app.data_root, cleanup_chat.id);
		const fs::path retained = primary.string() + ".test-retained";
		if (save_fails)
		{
			fs::rename(primary, retained);
			fs::create_directory(primary);
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
			fs::remove(primary);
			fs::rename(retained, primary);
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

#if defined(__APPLE__) && UAM_ENABLE_RUNTIME_CODEX_CLI
#include "common/platform/observed_process_stop.h"
#include "common/runtime/acp/acp_session_internal.h"
namespace uam
{
	bool PendingChatSavePreparationForTests(const AppState&, const std::string&, bool&);
	bool PendingChatSavePublicationForTests(const AppState&, const std::string&, bool&);
}

#include <cstdlib>
#include <cstdio>
namespace
{
	struct EmptyHeldFreshSendFixture
	{
		TempDir temp{"uam-empty-held-fresh-send"};
		ScopedEnvVar path{"PATH", (temp.root / "bin").string()};
		ScopedEnvVar codex_home{"CODEX_HOME", (temp.root / "codex-home").string()};
		uam::AppState app;
		const std::string native = "161c6159-2e57-48d0-9295-82ad65081c0c";
		const std::string fresh = "Retry with uam agents again for me";
		const std::string id = "empty-held-fresh-send";
		bool restore_private_seed_on_teardown = false;
		ChatSession teardown_seed_chat;
		std::string teardown_seed_primary, teardown_seed_summary;
		std::optional<std::string> teardown_seed_journal;
		fs::path Primary() const { return AppPaths::UamChatFilePath(temp.root / "data", id); }
		fs::path Journal() const { return temp.root / "data" / "queue-outboxes" / (id + ".json"); }
		ChatSession& Chat() { return app.chats.front(); }
		uam::AcpSessionState& Session() { return *app.acp_sessions.front(); }
		EmptyHeldFreshSendFixture(bool consumed_journal)
		{
			fs::create_directories(temp.root / "bin");
			fs::create_directories(temp.root / "workspace");
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "bin" / "codex",
			    "#!/usr/bin/python3 -I -S\nimport pathlib,sys\nroot=pathlib.Path(__file__).resolve().parent.parent\n(root/'private-cli-invoked').write_text('private-only')\nfor line in sys.stdin.buffer:\n with (root/'private-cli-input').open('ab') as out: out.write(line); out.flush()\n"));
			fs::permissions(temp.root / "bin" / "codex", fs::perms::owner_all);
			app.data_root = temp.root / "data";
			app.provider_profiles = {ProviderProfileStore::DefaultCodexProfile()};
			app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
			app.settings.central_provider_configuration.enabled = false;
			app.settings.git_enabled = app.settings.git_auto_worktree = app.settings.svn_enabled = false;
			app.settings.memory_enabled_default = app.settings.computer_use_globally_enabled = false;
			app.settings.memory_level_default = "off";
			app.settings.memory_recall_budget_bytes = 0;
			auto chat = ChatDomainService().CreateNewChat({}, "codex-cli", false);
			chat.id = id;
			chat.workspace_directory = (temp.root / "workspace").string();
			chat.routing_mode = "explicit";
			chat.model_id = "private-existing-model";
			chat.native_session_id = native;
			chat.memory_enabled = false;
			chat.memory_level = "off";
			chat.uam_control_enabled = false;
			chat.uam_control_explicitly_disabled = true;
			ChatDomainService().AddMessage(chat, MessageRole::User, "Previously delivered native work.");
			ChatDomainService().AddMessage(chat, MessageRole::Assistant, "Recorded prior answer.");
			chat.last_stop_reason = "timeout";
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
			if (consumed_journal)
			{
				UAM_ASSERT(ChatRepository::SaveQueuedPromptOutbox(app.data_root, chat));
				UAM_ASSERT(fs::exists(Journal()));
			}
			chat.acp_queue_held = true;
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
			const auto loaded = ChatRepository::LoadLocalChatBounded(app.data_root, id, 4 * 1024 * 1024);
			UAM_ASSERT(loaded && loaded->acp_queue_held && loaded->acp_queued_prompts.empty());
			app.chats.push_back(*loaded);
			auto session = std::make_unique<uam::AcpSessionState>();
			session->chat_id = id;
			session->provider_id = "codex-cli";
			session->protocol_kind = "codex-app-server";
			session->queue_held = true;
			session->session_id = native; // Existing DATA identity; no native initialization or process is fabricated.
			app.acp_sessions.push_back(std::move(session));
			auto& version = app.runtime_cli_versions_by_provider_id["codex-cli"];
			version.checked = version.supported = true;
			version.installed_version = "0.160.0";
			version.execution_host = app.settings.execution_hosts.front();
			// Constructor-owned cleanup seed, captured before any behavioral control mutates DATA.
			teardown_seed_chat = Chat();
			teardown_seed_primary = ReadFile(Primary());
			teardown_seed_summary = ReadFile(AppPaths::UamChatSummaryFilePath(temp.root / "data", id));
			if (fs::exists(Journal())) teardown_seed_journal = ReadFile(Journal());
		}
		struct Receipt { int calls = 0, live_fresh_at_ack = -1, durable_fresh_at_ack = -1; bool native_at_ack = false; std::optional<bool> saved; };
		std::shared_ptr<Receipt> Begin(const std::string& text)
		{
			std::string error;
			UAM_ASSERT(uam::SendAcpPrompt(app, id, text, &error));
			UAM_ASSERT(error.empty());
			auto receipt = std::make_shared<Receipt>();
			UAM_ASSERT(uam::DeferChatSaveResult(app, id, [receipt, this](bool saved)
			{
				++receipt->calls; receipt->saved = saved;
				if (!saved) return;
				const auto* current = ChatDomainService().FindChatById(app, id);
				if (current) receipt->live_fresh_at_ack = static_cast<int>(std::ranges::count(current->messages, fresh, &Message::content));
				try
				{
					const auto raw = nlohmann::json::parse(ReadFile(Primary()));
					receipt->durable_fresh_at_ack = 0;
					for (const auto& row : raw.at("messages"))
						if (row.value("role", "") == "user" && row.value("content", "") == fresh) ++receipt->durable_fresh_at_ack;
					receipt->native_at_ack = raw.value("native_session_id", "") == native;
				}
				catch (...) { receipt->durable_fresh_at_ack = -1; }
			}));
			UAM_ASSERT(receipt->calls == 0 && !receipt->saved);
			return receipt;
		}
		void AwaitFuture(bool publication, bool advance)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (std::chrono::steady_clock::now() < deadline)
			{
				bool ready = false;
				const bool present = publication ? uam::PendingChatSavePublicationForTests(app, id, ready)
				                                 : uam::PendingChatSavePreparationForTests(app, id, ready);
				if (present && ready) return; // Observe the real future; do not harvest or fabricate it.
				if (!present && advance) (void)uam::FlushPendingChatSaves(app);
				std::this_thread::yield();
			}
			throw std::runtime_error("Actual empty-held Send worker did not reach its observable phase.");
		}
		void Drain(bool force = false)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while ((!app.pending_chat_save_tasks.empty() || !app.pending_chat_save_at_by_chat_id.empty()) &&
			       std::chrono::steady_clock::now() < deadline)
			{
				(void)uam::FlushPendingChatSaves(app, force);
				if (!force) (void)uam::PollAllAcpSessions(app);
				std::this_thread::yield();
			}
			UAM_ASSERT(app.pending_chat_save_tasks.empty() && app.pending_chat_save_at_by_chat_id.empty());
		}
		void AssertNoFreshRows(const std::string& text) const
		{
			for (const auto& chat : app.chats)
				for (const auto& message : chat.messages) UAM_ASSERT(message.content != text);
		}
		void AssertOneFreshAdmission(const std::vector<Message>& original)
		{
			UAM_ASSERT_EQ(Chat().messages.size(), original.size() + 1);
			UAM_ASSERT(std::equal(original.begin(), original.end(), Chat().messages.begin()));
			const auto& row = Chat().messages.back();
			UAM_ASSERT(row.role == MessageRole::User && row.content == fresh);
			UAM_ASSERT_EQ(std::ranges::count(Chat().messages, fresh, &Message::content), 1);
			UAM_ASSERT_EQ(Chat().native_session_id, native);
			const auto saved = ChatRepository::LoadLocalChatBounded(temp.root / "data", id, 4 * 1024 * 1024);
			UAM_ASSERT(saved && saved->messages == Chat().messages && saved->native_session_id == native);
			UAM_ASSERT_EQ(std::ranges::count(saved->messages, fresh, &Message::content), 1);
			for (const auto& prompt : Session().queued_user_prompts)
				UAM_ASSERT(prompt.direct_admission && prompt.text == fresh); // Never replay old source hold content.
			// The private peer records stdin but never answers initialization: no provider delivery is claimed.
			const auto input = temp.root / "private-cli-input";
			if (fs::exists(input)) UAM_ASSERT(ReadFile(input).find(fresh) == std::string::npos);
		}
		~EmptyHeldFreshSendFixture() noexcept
		{
			try
			{
				if (restore_private_seed_on_teardown)
				{
					// Teardown only: never overwrite DATA while a real disk task can still own it.
					const bool safe = app.data_root == temp.root / "data" && app.pending_chat_save_tasks.empty() &&
					    app.chats.size() == 1 && app.chats.front().id == id;
					if (safe)
					{
						if (!uam::io::WriteTextFile(Primary(), teardown_seed_primary) ||
						    !uam::io::WriteTextFile(AppPaths::UamChatSummaryFilePath(temp.root / "data", id), teardown_seed_summary))
							std::abort();
						if (teardown_seed_journal)
						{
							if (!uam::io::WriteTextFile(Journal(), *teardown_seed_journal)) std::abort();
						}
						else
						{
							std::error_code error;
							(void)fs::remove(Journal(), error);
							if (error) std::abort();
						}
						app.chats.front() = teardown_seed_chat; // Same seed identity, revisions AND queue payload.
					}
					else
					{
						std::fprintf(stderr, "EMPTY_HELD_TEARDOWN seed_restore_skipped_unsafe\n");
						std::fflush(stderr);
					}
				}
				uam::FastStopAcpSessionsForExit(app);
				Drain(true); // Retire actual proof/publication futures before private storage/environment destruction.
				auto& process = PlatformServicesFactory::Instance().process_service;
				for (auto& session : app.acp_sessions)
					if (session)
					{
						const auto stopped = uam::platform::StopStdioProcessObserved(process, *session);
						if (!stopped.exit_confirmed) std::abort();
						process.CloseStdioProcessHandles(*session);
					}
			}
			catch (...) { std::abort(); }
		}
	};
}

UAM_TEST(ExplicitFreshSendRetiresOnlyOwnedEmptyHeldQueueWithExistingNativeIdentity)
{
	for (const bool consumed_journal : {false, true})
	{
		EmptyHeldFreshSendFixture fixture(consumed_journal);
		const auto original = fixture.Chat().messages;
		const auto revision = fixture.Chat().acp_queue_storage_revision;
		const auto model = fixture.Chat().model_id;
		const auto host = fixture.Chat().execution_host_id;
		const auto primary_before = ReadFile(fixture.Primary());
		if (consumed_journal)
		{
			const auto primary = nlohmann::json::parse(primary_before);
			const auto journal = nlohmann::json::parse(ReadFile(fixture.Journal()));
			UAM_ASSERT(journal.at("acp_queued_prompts").is_array() && journal.at("acp_queued_prompts").empty());
			UAM_ASSERT_EQ(journal.at("acp_dispatched_queued_prompt_count"), nlohmann::json(0));
			UAM_ASSERT_EQ(journal.at("revision"), primary.at("acp_queue_storage_revision"));
			UAM_ASSERT_EQ(journal.at("revision"), nlohmann::json(revision));
			UAM_ASSERT(journal.at("owner").is_string() && !journal.at("owner").get<std::string>().empty());
			UAM_ASSERT_EQ(journal.at("owner"), nlohmann::json(ChatRepository::QueueStorageOwner(fixture.Chat())));
		}
		else UAM_ASSERT(!fs::exists(fixture.Journal()));
		const auto receipt = fixture.Begin(fixture.fresh);
		fixture.AwaitFuture(false, false);
		UAM_ASSERT(receipt->calls == 0 && fixture.Chat().acp_queue_held && fixture.Session().queue_held);
		UAM_ASSERT_EQ(fixture.Chat().messages, original);
		UAM_ASSERT_EQ(ReadFile(fixture.Primary()), primary_before);
		UAM_ASSERT(!fs::exists(fixture.temp.root / "private-cli-invoked"));
		fixture.AwaitFuture(true, true);
		UAM_ASSERT(receipt->calls == 0 && fixture.Chat().acp_queue_held && fixture.Session().queue_held);
		UAM_ASSERT_EQ(fixture.Chat().messages, original); // Journal worker completion is not fresh Send admission.
		fixture.Drain();
		UAM_ASSERT(receipt->calls == 1 && receipt->saved && *receipt->saved);
		UAM_ASSERT_EQ(receipt->live_fresh_at_ack, 1);
		UAM_ASSERT_EQ(receipt->durable_fresh_at_ack, 1);
		UAM_ASSERT(receipt->native_at_ack);
		fixture.AssertOneFreshAdmission(original);
		UAM_ASSERT(!fixture.Session().queue_held && !fixture.Chat().acp_queue_held);
		UAM_ASSERT(fixture.Chat().acp_queue_storage_revision != revision);
		UAM_ASSERT_EQ(fixture.Chat().model_id, model);
		UAM_ASSERT_EQ(fixture.Chat().execution_host_id, host);
		(void)uam::FlushPendingChatSaves(fixture.app);
		UAM_ASSERT_EQ(receipt->calls, 1);
		fixture.AssertOneFreshAdmission(original);
	}
}

UAM_TEST(ExplicitFreshSendPreservesNonemptyUncertainMalformedAndUnsavedQueueHolds)
{
	for (int arm = 0; arm < 20; ++arm)
	{
		try
		{
			EmptyHeldFreshSendFixture fixture(true);
			fixture.restore_private_seed_on_teardown = true; // Opt in only this raw-DATA refusal case.
			auto& chat = fixture.Chat();
			auto& session = fixture.Session();
			if (arm <= 2)
			{
				uam::AcpQueuedUserPromptState queued;
				queued.id = "retained-input"; queued.text = "Retained authored intent.";
				queued.direct_admission = arm == 0;
				queued.prepared_for_delivery = arm == 1;
				queued.existing_user_message_index = arm == 2 ? 0 : -1;
				// The recovery entry is EMPTY live state. Nonempty saved DATA must refuse it;
				// real live queues retain the original Send classification instead.
				ChatSession raw = chat;
				raw.acp_queued_prompts.push_back(queued);
				UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, raw));
				UAM_ASSERT(chat.acp_queued_prompts.empty() && session.queued_user_prompts.empty());
				const auto persisted = nlohmann::json::parse(ReadFile(fixture.Primary()));
				UAM_ASSERT_EQ(persisted.at("acp_queued_prompts").size(), std::size_t{1});
				UAM_ASSERT_EQ(persisted.at("acp_queued_prompts")[0].at("id"), nlohmann::json(queued.id));
			}
			if (arm == 3) chat.messages.front().acp_prompt_not_sent = true;
			if (arm == 4) fixture.app.pending_chat_save_at_by_chat_id[chat.id] = 1.0;
			if (arm == 5) session.remote_stop_unconfirmed = true;
			if (arm == 6) chat.remote_prompt_delivery_id = "unresolved-delivery";
			if (arm == 7) chat.messages_loaded = false;
			if (arm >= 8 && arm <= 11)
			{
				auto journal = nlohmann::json::parse(ReadFile(fixture.Journal()));
				if (arm == 8) journal["owner"] = "foreign-owner";
				if (arm == 9) journal["revision"] = "different-unconsumed-revision";
				if (arm == 10) journal["acp_queued_prompts"] = nlohmann::json::array({false});
				if (arm == 11) journal["acp_queued_prompts"] = nlohmann::json::object();
				UAM_ASSERT(uam::io::WriteTextFile(fixture.Journal(), journal.dump()));
			}
			if (arm == 12 || arm == 13)
			{
				auto primary = nlohmann::json::parse(ReadFile(fixture.Primary()));
				primary["acp_queued_prompts"] = arm == 12 ? nlohmann::json::array({false}) : nlohmann::json::object();
				UAM_ASSERT(uam::io::WriteTextFile(fixture.Primary(), primary.dump()));
			}
			if (arm == 14) session.pending_request_methods.emplace(81, "thread/resume");
			if (arm == 15)
			{
				// A genuine repository-produced nonempty journal stays forbidden even
				// after its revision is consumed by an empty full-primary snapshot.
				ChatSession producer = chat;
				uam::AcpQueuedUserPromptState queued;
				queued.id = "consumed-but-nonempty"; queued.text = "Preserve foreign historical intent.";
				producer.acp_queued_prompts.push_back(queued);
				UAM_ASSERT(ChatRepository::SaveQueuedPromptOutbox(fixture.app.data_root, producer));
				chat.acp_queue_storage_revision = producer.acp_queue_storage_revision;
				UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
			}
			if (arm == 16 || arm == 17)
			{
				auto primary = nlohmann::json::parse(ReadFile(fixture.Primary()));
				if (arm == 16) primary["messages"][0]["acp_prompt_not_sent"] = "unknown-type";
				if (arm == 17) primary["acp_unsent_prompt_recoveries"] = nlohmann::json::array({false});
				UAM_ASSERT(uam::io::WriteTextFile(fixture.Primary(), primary.dump()));
			}
			if (arm == 18) chat.native_session_reset_pending = true;
			if (arm == 19)
			{
				ChatSession raw = chat;
				raw.native_session_reset_pending = true;
				UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, raw));
				UAM_ASSERT(nlohmann::json::parse(ReadFile(fixture.Primary())).at("native_session_reset_pending") == true);
				UAM_ASSERT(!chat.native_session_reset_pending);
			}
			const auto primary_before = ReadFile(fixture.Primary());
			const auto journal_before = ReadFile(fixture.Journal());
			const auto rows_before = chat.messages;
			const auto queued_before = chat.acp_queued_prompts;
			const auto revision = chat.acp_queue_storage_revision;
			std::string error;
			const bool pending = uam::SendAcpPrompt(fixture.app, fixture.id, fixture.fresh, &error);
			if (pending)
			{
				auto receipt = std::make_shared<EmptyHeldFreshSendFixture::Receipt>();
				UAM_ASSERT(uam::DeferChatSaveResult(fixture.app, fixture.id, [receipt](bool saved) { ++receipt->calls; receipt->saved = saved; }));
				fixture.Drain();
				UAM_ASSERT(receipt->calls == 1 && receipt->saved && !*receipt->saved);
			}
			else UAM_ASSERT(!error.empty());
			UAM_ASSERT(session.queue_held && chat.acp_queue_held);
			UAM_ASSERT(chat.messages == rows_before && chat.acp_queued_prompts == queued_before);
			UAM_ASSERT_EQ(chat.acp_queue_storage_revision, revision);
			UAM_ASSERT_EQ(ReadFile(fixture.Primary()), primary_before);
			UAM_ASSERT_EQ(ReadFile(fixture.Journal()), journal_before);
			UAM_ASSERT(!fs::exists(fixture.temp.root / "private-cli-invoked"));
		}
		catch (const std::exception& error)
		{
			throw std::runtime_error("Empty-held refusal arm " + std::to_string(arm) + ": " + error.what());
		}
	}
}

UAM_TEST(ExplicitFreshSendDoesNotReplaceThePendingEmptyHoldInvocation)
{
	EmptyHeldFreshSendFixture fixture(true);
	const auto original = fixture.Chat().messages;
	const auto receipt = fixture.Begin(fixture.fresh);
	fixture.AwaitFuture(false, false);
	const std::string other = "A second input must remain a separate unaccepted draft.";
	std::string error;
	UAM_ASSERT(!uam::SendAcpPrompt(fixture.app, fixture.id, other, &error));
	UAM_ASSERT(!error.empty() && receipt->calls == 0);
	UAM_ASSERT(fixture.Chat().acp_queue_held && fixture.Session().queue_held);
	fixture.AssertNoFreshRows(other);
	fixture.Drain();
	UAM_ASSERT(receipt->calls == 1 && receipt->saved && *receipt->saved);
	UAM_ASSERT_EQ(receipt->live_fresh_at_ack, 1);
	UAM_ASSERT_EQ(receipt->durable_fresh_at_ack, 1);
	UAM_ASSERT(receipt->native_at_ack);
	fixture.AssertOneFreshAdmission(original);
	fixture.AssertNoFreshRows(other);
}

UAM_TEST(ExplicitFreshSendRevokesEmptyHoldContinuationOnRealOwnerOrSaveChanges)
{
	enum Change { StopProof, StopPublished, CancelReceipt, Root, Native, Provider, Host, Incarnation, SuccessorSave, DeletedPrimary, ForcedFlush };
	for (const auto change : {StopProof, StopPublished, CancelReceipt, Root, Native, Provider, Host, Incarnation, SuccessorSave, DeletedPrimary, ForcedFlush})
	{
		EmptyHeldFreshSendFixture fixture(true);
		const auto original = fixture.Chat().messages;
		const auto original_queue_revision = fixture.Chat().acp_queue_storage_revision;
		const auto primary_before = ReadFile(fixture.Primary());
		const auto receipt = fixture.Begin(fixture.fresh);
		fixture.AwaitFuture(false, false);
		if (change == StopPublished) fixture.AwaitFuture(true, true);
		UAM_ASSERT(receipt->calls == 0 && fixture.Chat().messages == original);
		std::shared_ptr<EmptyHeldFreshSendFixture::Receipt> successor;
		if (change == StopProof) UAM_ASSERT(uam::StopAcpSession(fixture.app, fixture.id));
		if (change == StopPublished)
		{
			// The real journal committed but its revision has deliberately not been harvested.
			// First Stop must refuse its stale discard save while retaining the explicit hold.
			UAM_ASSERT(!uam::StopAcpSession(fixture.app, fixture.id));
			UAM_ASSERT(fixture.Chat().acp_queue_held && fixture.Session().queue_held);
			fixture.Drain(); // Harvest the revoked real receipt and its genuine ordinary successor save.
			UAM_ASSERT(receipt->calls == 1 && receipt->saved && !*receipt->saved);
			UAM_ASSERT_EQ(fixture.Chat().messages, original);
			UAM_ASSERT_EQ(fixture.Chat().native_session_id, fixture.native);
			UAM_ASSERT_EQ(fixture.Session().session_id, fixture.native);
			UAM_ASSERT(fixture.Chat().acp_queue_held && fixture.Session().queue_held);
			UAM_ASSERT(fixture.Chat().acp_queued_prompts.empty() && fixture.Session().queued_user_prompts.empty());
			UAM_ASSERT_EQ(fixture.Chat().acp_dispatched_queued_prompt_count, std::size_t{0});
			fixture.AssertNoFreshRows(fixture.fresh);
			UAM_ASSERT(!fs::exists(fixture.temp.root / "private-cli-invoked"));
			const auto settled = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.id);
			UAM_ASSERT(settled && settled->acp_queue_held && settled->messages == original);
			UAM_ASSERT_EQ(settled->native_session_id, fixture.native);
			UAM_ASSERT(settled->acp_queued_prompts.empty() && settled->acp_dispatched_queued_prompt_count == 0);
			UAM_ASSERT_EQ(ChatRepository::QueueStorageOwner(*settled), ChatRepository::QueueStorageOwner(fixture.Chat()));
			UAM_ASSERT_EQ(settled->acp_queue_storage_revision, fixture.Chat().acp_queue_storage_revision);
			UAM_ASSERT(!settled->acp_queue_storage_revision.empty() && settled->acp_queue_storage_revision != original_queue_revision);
			const auto journal = nlohmann::json::parse(ReadFile(fixture.Journal()));
			UAM_ASSERT_EQ(journal.at("revision"), nlohmann::json(settled->acp_queue_storage_revision));
			UAM_ASSERT_EQ(journal.at("owner"), nlohmann::json(ChatRepository::QueueStorageOwner(*settled)));
			UAM_ASSERT(journal.at("acp_queued_prompts").is_array() && journal.at("acp_queued_prompts").empty());
			UAM_ASSERT_EQ(journal.at("acp_dispatched_queued_prompt_count"), nlohmann::json(0));
			// Only this second explicit public Stop owns the now-current durable discard.
			UAM_ASSERT(uam::StopAcpSession(fixture.app, fixture.id));
		}
		if (change == CancelReceipt) uam::ResolvePendingChatSaveResults(fixture.app, fixture.id, false);
		if (change == Root) fixture.app.data_root = fixture.temp.root / "replacement-data-root";
		if (change == Native) fixture.Chat().native_session_id = "7d913497-8f8a-4c03-920c-cb6d6fb92836";
		if (change == Provider) fixture.Chat().provider_id = "gemini-cli";
		if (change == Host) fixture.Chat().execution_host_id = "replacement-execution-host";
		if (change == Incarnation) fixture.Chat().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		if (change == SuccessorSave)
		{
			fixture.Chat().title = "The genuine successor metadata save wins.";
			uam::acp_detail::ScheduleChatSave(fixture.app, fixture.Chat(), 0.0);
			successor = std::make_shared<EmptyHeldFreshSendFixture::Receipt>();
			UAM_ASSERT(uam::DeferChatSaveResult(fixture.app, fixture.id, [successor](bool saved) { ++successor->calls; successor->saved = saved; }));
		}
		if (change == DeletedPrimary)
		{
			UAM_ASSERT(!ChatRepository::DeleteChatStorageFiles(fixture.app.data_root, fixture.id).Failed());
			UAM_ASSERT(!fs::exists(fixture.Primary()));
		}
		fixture.Drain(change == ForcedFlush);
		UAM_ASSERT(receipt->calls == 1 && receipt->saved && !*receipt->saved);
		UAM_ASSERT_EQ(fixture.Chat().messages, original);
		if (change == StopProof || change == StopPublished)
			UAM_ASSERT(!fixture.Chat().acp_queue_held && !fixture.Session().queue_held); // Explicit default Stop owns discard.
		else UAM_ASSERT(fixture.Chat().acp_queue_held && fixture.Session().queue_held);
		fixture.AssertNoFreshRows(fixture.fresh);
		UAM_ASSERT(!fs::exists(fixture.temp.root / "private-cli-invoked"));
		if (change == StopPublished)
		{
			const auto stopped = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.id);
			UAM_ASSERT(stopped && !stopped->acp_queue_held && stopped->messages == original);
			UAM_ASSERT_EQ(stopped->native_session_id, fixture.native);
			UAM_ASSERT(stopped->acp_queued_prompts.empty() && stopped->acp_dispatched_queued_prompt_count == 0);
			UAM_ASSERT_EQ(ChatRepository::QueueStorageOwner(*stopped), ChatRepository::QueueStorageOwner(fixture.Chat()));
		}
		if (change == DeletedPrimary) UAM_ASSERT(!fs::exists(fixture.Primary())); // No resurrection by force/fallback.
		else if (change == SuccessorSave)
		{
			UAM_ASSERT(successor && successor->calls == 1 && successor->saved && *successor->saved);
			const auto saved = ChatRepository::LoadLocalChat(fixture.temp.root / "data", fixture.id);
			UAM_ASSERT(saved && saved->title == fixture.Chat().title && saved->messages == original && saved->acp_queue_held);
		}
		else if (change != StopProof && change != StopPublished) UAM_ASSERT_EQ(ReadFile(fixture.Primary()), primary_before);
		if (change == Root) UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.id)));
		if (change == Native) UAM_ASSERT_EQ(fixture.Chat().native_session_id, std::string("7d913497-8f8a-4c03-920c-cb6d6fb92836"));
		if (change == Provider) UAM_ASSERT_EQ(fixture.Chat().provider_id, std::string("gemini-cli"));
		if (change == Host) UAM_ASSERT_EQ(fixture.Chat().execution_host_id, std::string("replacement-execution-host"));
		(void)uam::FlushPendingChatSaves(fixture.app);
		UAM_ASSERT_EQ(receipt->calls, 1);
		fixture.AssertNoFreshRows(fixture.fresh);
	}
}

UAM_TEST(ExplicitFreshSendDoesNotAcknowledgeFailedEmptyHoldJournalPublication)
{
	EmptyHeldFreshSendFixture fixture(false);
	const auto directory = fixture.Journal().parent_path();
	fs::create_directories(directory);
	const auto permissions = fs::status(directory).permissions();
	struct RestorePermissions
	{
		fs::path path; fs::perms permissions;
		~RestorePermissions() { std::error_code error; fs::permissions(path, permissions, error); }
	} restore{directory, permissions};
	const auto primary_before = ReadFile(fixture.Primary());
	const auto original = fixture.Chat().messages;
	const auto receipt = fixture.Begin(fixture.fresh);
	fixture.AwaitFuture(false, false);
	fs::permissions(directory, fs::perms::owner_read | fs::perms::owner_exec);
	// Prove the ordinary non-root private filesystem denies writes before claiming this failure oracle.
	const auto denied = directory / "fixture-write-must-fail";
	UAM_ASSERT(!uam::io::WriteTextFile(denied, "No permission to publish."));
	UAM_ASSERT(!fs::exists(denied));
	fixture.AwaitFuture(true, true); // The actual publisher ran; its future is observed, never forged/harvested here.
	UAM_ASSERT(receipt->calls == 0 && fixture.Chat().acp_queue_held && fixture.Session().queue_held);
	fixture.Drain();
	UAM_ASSERT(receipt->calls == 1 && receipt->saved && !*receipt->saved);
	UAM_ASSERT(fixture.Chat().acp_queue_held && fixture.Session().queue_held);
	UAM_ASSERT_EQ(fixture.Chat().messages, original);
	UAM_ASSERT_EQ(ReadFile(fixture.Primary()), primary_before);
	fixture.AssertNoFreshRows(fixture.fresh);
	UAM_ASSERT(!fs::exists(fixture.temp.root / "private-cli-invoked"));
	// A failed ACK is never a rollback receipt: any actual journal fact is left untouched.
}
#endif
#if defined(__APPLE__) && UAM_ENABLE_RUNTIME_CODEX_CLI
namespace
{
	void DrainEmptyHeldSavesWithoutProviderPolling(uam::AppState& app)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while ((!app.pending_chat_save_tasks.empty() || !app.pending_chat_save_at_by_chat_id.empty()) &&
		       std::chrono::steady_clock::now() < deadline)
		{
			(void)uam::FlushPendingChatSaves(app);
			std::this_thread::yield();
		}
		UAM_ASSERT(app.pending_chat_save_tasks.empty() && app.pending_chat_save_at_by_chat_id.empty());
	}
}

UAM_TEST(ExplicitFreshSendPreservesAcceptedRemoteTurnHeldFollowupAdmission)
{
	HeldQueueFixture fixture;
	ChatSession& chat = fixture.app.chats.front();
	// Use the public constructor's real birth; an empty legacy birth is normalized on load.
	chat.created_at = ChatDomainService().CreateNewChat({}, "codex-cli", false).created_at;
	UAM_ASSERT(!chat.created_at.empty());
	// Disconnected legacy-helper follow-up capture, not healthy helper3 or SSH execution.
	const std::string native = "4e12128e-3194-44b7-bc53-83f9f365976c";
	ExecutionHost host;
	host.id = "private-legacy-remote-host";
	host.label = "Private legacy disconnected helper";
	host.transport = "ssh";
	host.ssh_alias = "private-never-connect.invalid";
	host.runner_status = "missing";
	host.runner_protocol_version = 2;
	host.platform = "linux";
	host.architecture = "x86_64";
	host.instruction_text = std::string{};
	fixture.app.settings.execution_hosts.push_back(host);
	chat.execution_host_id = host.id;
	chat.native_session_id = native;
	chat.remote_turn_reconnect_pending = true;
	chat.remote_process_exists = true;
	chat.remote_active_turn_id = "accepted-native-turn";
	chat.remote_turn_serial = 7;
	chat.acp_dispatched_queued_prompt_count = 1;
	chat.acp_queued_prompts.front().prepared_for_delivery = true;
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
	const auto seeded = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
	UAM_ASSERT(seeded && seeded->created_at == chat.created_at && seeded->native_session_id == native);
	UAM_ASSERT_EQ(ChatRepository::QueueStorageOwner(*seeded), ChatRepository::QueueStorageOwner(chat));
	(void)uam::RestoreRemoteAcpSessionsAfterRestart(fixture.app);
	auto& session = fixture.Session();
	UAM_ASSERT(session.processing && session.recovering_remote_turn && session.recovering_remote_process);
	UAM_ASSERT(!session.running && !session.session_ready && session.queue_held && chat.acp_queue_held);
	// Real Restore attempted recovery; its existing protocol2 safety guard refused BEFORE transport.
	UAM_ASSERT_EQ(session.last_error, std::string("This SSH helper cannot safely resume the active turn. Update the helper in Settings."));
	UAM_ASSERT(session.last_process_id.empty() && !session.initialized && session.pending_request_methods.empty());
	UAM_ASSERT_EQ(session.queued_user_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(session.queued_user_prompts.front().id, std::string("two"));
	const auto original_rows = chat.messages;
	const auto original_outbox = chat.acp_queued_prompts;
	const auto raw_primary_before_send = ReadFile(AppPaths::UamChatFilePath(fixture.app.data_root, chat.id));
	UAM_ASSERT_EQ(original_outbox.size(), std::size_t{2});
	UAM_ASSERT(original_outbox.front().prepared_for_delivery && original_outbox.front().id == "one");
	UAM_ASSERT_EQ(nlohmann::json::parse(raw_primary_before_send).at("acp_queued_prompts").size(), std::size_t{2});
	const auto original_future = session.queued_user_prompts;
	const auto owner = ChatRepository::QueueStorageOwner(chat);
	const std::string followup = "A new follow-up waits behind the accepted native turn.";
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(fixture.app, chat.id, followup, &error));
	UAM_ASSERT(error.empty());
	// Existing stopped SSH queue persistence acknowledges synchronously, with no provider polling.
	int callbacks = 0;
	UAM_ASSERT(!uam::DeferChatSaveResult(fixture.app, chat.id, [&callbacks](bool) { ++callbacks; }));
	UAM_ASSERT_EQ(callbacks, 0);
	UAM_ASSERT_EQ(chat.messages, original_rows);
	UAM_ASSERT_EQ(session.queued_user_prompts.size(), original_future.size() + 1);
	UAM_ASSERT(std::equal(original_future.begin(), original_future.end(), session.queued_user_prompts.begin()));
	// Existing reconnect-pending persistence settles the already-dispatched prepared prefix:
	// the saved outbox is the runtime FUTURE queue [two,new], not [one,two,new].
	// This compatibility case does not grant dispatch eligibility to that legacy marker.
	UAM_ASSERT_EQ(chat.acp_queued_prompts.size(), session.queued_user_prompts.size());
	UAM_ASSERT(std::equal(session.queued_user_prompts.begin(), session.queued_user_prompts.end(), chat.acp_queued_prompts.begin()));
	UAM_ASSERT_EQ(chat.acp_queued_prompts.front().id, original_future.front().id);
	UAM_ASSERT_EQ(std::ranges::count(chat.acp_queued_prompts, std::string("one"), &uam::AcpQueuedUserPromptState::id), 0);
	const auto fresh_id = session.queued_user_prompts.back().id;
	UAM_ASSERT(!fresh_id.empty() && session.queued_user_prompts.back().text == followup);
	UAM_ASSERT(!session.queued_user_prompts.back().direct_admission);
	UAM_ASSERT_EQ(chat.acp_queued_prompts.back().id, fresh_id);
	UAM_ASSERT_EQ(std::ranges::count(chat.acp_queued_prompts, followup, &uam::AcpQueuedUserPromptState::text), 1);
	UAM_ASSERT(session.processing && session.queue_held && chat.acp_queue_held);
	UAM_ASSERT(session.recovering_remote_turn && session.recovering_remote_process);
	UAM_ASSERT_EQ(session.turn_serial, 7);
	UAM_ASSERT_EQ(chat.native_session_id, native);
	UAM_ASSERT_EQ(chat.remote_active_turn_id, std::string("accepted-native-turn"));
	UAM_ASSERT_EQ(chat.acp_dispatched_queued_prompt_count, std::size_t{1});
	UAM_ASSERT_EQ(ChatRepository::QueueStorageOwner(chat), owner);
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
	UAM_ASSERT(saved && saved->messages == original_rows && saved->acp_queued_prompts == chat.acp_queued_prompts);
	UAM_ASSERT(saved->acp_queue_held && saved->remote_turn_reconnect_pending && saved->remote_process_exists);
	UAM_ASSERT_EQ(saved->native_session_id, chat.native_session_id);
	UAM_ASSERT_EQ(saved->remote_active_turn_id, chat.remote_active_turn_id);
	UAM_ASSERT_EQ(saved->acp_dispatched_queued_prompt_count, std::size_t{1});
	UAM_ASSERT_EQ(ChatRepository::QueueStorageOwner(*saved), owner);
	UAM_ASSERT(!session.running && session.pending_request_methods.empty());
	uam::FastStopAcpSessionsForExit(fixture.app);
}

UAM_TEST(ExplicitFreshSendAcknowledgesStoppedSshAutomaticDurableAdmissionOnce)
{
	EmptyHeldFreshSendFixture fixture(false);
	ExecutionHost host;
	host.id = "private-no-connect-ssh";
	host.label = "Private queue-only SSH fixture";
	host.transport = "ssh";
	host.ssh_alias = "private-never-connect.invalid";
	host.platform = "linux";
	host.architecture = "x86_64";
	host.runner_protocol_version = 3;
	host.instruction_text = std::string{};
	fixture.app.settings.execution_hosts.push_back(host);
	auto& chat = fixture.Chat();
	auto& session = fixture.Session();
	chat.execution_host_id = host.id;
	chat.routing_mode = "automatic";
	chat.workspace_directory = "/private/queue-only-workspace";
	ChatFolder folder;
	folder.id = "private-ssh-folder";
	folder.title = "Private SSH queue fixture";
	folder.directory = chat.workspace_directory;
	folder.execution_host_id = host.id;
	fixture.app.folders.push_back(folder);
	chat.folder_id = chat.routing_source_folder_id = folder.id;
	chat.routing_source_directory = folder.directory;
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, chat));
	UAM_ASSERT(!fs::exists(fixture.Journal()));
	const auto original = chat.messages;
	const auto owner = ChatRepository::QueueStorageOwner(chat);
	UAM_ASSERT(!session.running && !session.initialized && !session.session_ready && session.queue_held);
	UAM_ASSERT(chat.acp_queued_prompts.empty() && session.queued_user_prompts.empty());
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(fixture.app, fixture.id, fixture.fresh, &error));
	UAM_ASSERT(error.empty());
	struct QueueReceipt { int calls = 0, live_count = -1, saved_count = -1; bool owned = false; std::optional<bool> accepted; std::string id; };
	auto receipt = std::make_shared<QueueReceipt>();
	UAM_ASSERT(uam::DeferChatSaveResult(fixture.app, fixture.id, [receipt, &fixture, owner](bool accepted)
	{
		++receipt->calls;
		receipt->accepted = accepted;
		if (!accepted) return;
		receipt->live_count = static_cast<int>(std::ranges::count(fixture.Chat().acp_queued_prompts, fixture.fresh, &uam::AcpQueuedUserPromptState::text));
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.id);
		if (!saved) return;
		receipt->saved_count = static_cast<int>(std::ranges::count(saved->acp_queued_prompts, fixture.fresh, &uam::AcpQueuedUserPromptState::text));
		if (saved->acp_queued_prompts.size() == 1) receipt->id = saved->acp_queued_prompts.front().id;
		receipt->owned = saved->native_session_id == fixture.native && saved->execution_host_id == fixture.Chat().execution_host_id &&
		    ChatRepository::QueueStorageOwner(*saved) == owner;
	}));
	fixture.AwaitFuture(false, false);
	UAM_ASSERT(receipt->calls == 0 && session.queue_held && chat.acp_queue_held);
	fixture.AwaitFuture(true, true);
	UAM_ASSERT(receipt->calls == 0 && session.queue_held && chat.acp_queue_held);
	UAM_ASSERT_EQ(chat.messages, original); // Empty-hold journal ACK is not the fresh input ACK.
	DrainEmptyHeldSavesWithoutProviderPolling(fixture.app);
	UAM_ASSERT(receipt->calls == 1 && receipt->accepted && *receipt->accepted);
	UAM_ASSERT(receipt->live_count == 1 && receipt->saved_count == 1 && receipt->owned);
	UAM_ASSERT(!receipt->id.empty());
	UAM_ASSERT_EQ(chat.messages, original);
	UAM_ASSERT_EQ(chat.acp_queued_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(session.queued_user_prompts.size(), std::size_t{1});
	const auto& prompt = chat.acp_queued_prompts.front();
	UAM_ASSERT(prompt.id == receipt->id && prompt.text == fixture.fresh && prompt.direct_admission && !prompt.prepared_for_delivery);
	UAM_ASSERT_EQ(session.queued_user_prompts.front(), prompt);
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.id);
	UAM_ASSERT(saved && saved->messages == original && saved->acp_queued_prompts == chat.acp_queued_prompts);
	UAM_ASSERT_EQ(saved->native_session_id, fixture.native);
	UAM_ASSERT_EQ(ChatRepository::QueueStorageOwner(*saved), owner);
	const auto journal = nlohmann::json::parse(ReadFile(fixture.Journal()));
	UAM_ASSERT_EQ(journal.at("acp_queued_prompts").size(), std::size_t{1});
	UAM_ASSERT_EQ(journal.at("acp_queued_prompts")[0].at("id"), nlohmann::json(receipt->id));
	UAM_ASSERT_EQ(journal.at("owner"), nlohmann::json(owner));
	UAM_ASSERT(!session.queue_held && !chat.acp_queue_held && session.reconnect_pending);
	UAM_ASSERT(!session.running && !session.initialized && !session.session_ready && session.pending_request_methods.empty());
	UAM_ASSERT(!fs::exists(fixture.temp.root / "private-cli-invoked"));
	// The original CEF contract is immediate success when no admission save remains to defer.
	int late_callbacks = 0;
	UAM_ASSERT(!uam::DeferChatSaveResult(fixture.app, fixture.id, [&late_callbacks](bool) { ++late_callbacks; }));
	(void)uam::FlushPendingChatSaves(fixture.app);
	UAM_ASSERT(receipt->calls == 1 && late_callbacks == 0);
	UAM_ASSERT_EQ(chat.acp_queued_prompts.size(), std::size_t{1});
}
#endif

#include "common/chat/acp_prompt_recovery.h"
#include "common/runtime/acp/acp_session_runtime.h"
#if defined(__APPLE__) || defined(__linux__)
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace
{
	struct CapturedStopCarrierFixture : HeldQueueFixture
	{
		uam::AcpQueuedUserPromptState plan;

		CapturedStopCarrierFixture()
		{
			app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
			app.settings.git_enabled = app.settings.git_auto_worktree = false;
			app.settings.central_provider_configuration.enabled = false;
			auto& chat = app.chats.front();
			chat.created_at = "2026-10-19T15:30:00Z";
			chat.execution_host_id = "local";
			chat.workspace_directory = temp.root.string();
			chat.uam_control_enabled = false;
			chat.acp_queued_prompts.clear();
			plan = CapturePolicy("plan", "next-plan");
			UAM_ASSERT(plan.uam_agent_id == "plan" && plan.uam_agent_workspace_access == "read");
		}

		uam::AcpQueuedUserPromptState CapturePolicy(const std::string& agent, const std::string& id)
		{
			uam::SelectedPrimaryAgentSnapshot captured;
			std::string error;
			UAM_ASSERT(uam::AgentDefinitionService::CaptureSelectedPrimary(app.data_root, temp.root, agent, captured, error));
			uam::AcpQueuedUserPromptState prompt;
			prompt.id = id;
			prompt.text = "Authored input " + id;
			prompt.uam_agent_id = captured.definition.id;
			prompt.uam_agent_definition_hash = captured.definition.definition_hash;
			prompt.uam_agent_definition_snapshot = captured.definition.markdown_snapshot;
			prompt.uam_agent_instructions = captured.definition.instructions;
			prompt.uam_agent_skills = captured.definition.skills;
			prompt.uam_agent_delegates = captured.definition.delegates;
			prompt.uam_agent_workspace_access = captured.definition.workspace_access;
			prompt.uam_agent_execution_capability = uam::AgentDefinitionService::ExecutionCapabilityForProvider("codex-cli");
			prompt.direct_admission = false; // Raw queued intent; restore must not pre-settle a stopped direct receipt.
			prompt.markdown_store_files = {"authored-skill.md"};
			prompt.markdown_store_prompt_blocks = {"Immutable authored skill."};
			MessageAttachment attachment;
			attachment.id = "authored-attachment";
			attachment.name = "authored.txt";
			attachment.path = (temp.root / "authored.txt").string();
			attachment.kind = "file";
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "authored.txt", "Authored attachment bytes."));
			prompt.attachments = {attachment};
			UAM_ASSERT(uam::AgentDefinitionService::ValidateCapturedRuntimePolicy(prompt.uam_agent_id,
			    prompt.uam_agent_definition_snapshot, "codex-cli", prompt.uam_agent_execution_capability, &error));
			return prompt;
		}

		uam::AcpQueuedUserPromptState LargePolicy(std::size_t body_size, const std::string& id)
		{
			UAM_ASSERT(body_size > 0 && body_size <= 192U * 1024U);
			// A real private definition is parsed/captured/validated; no policy field is inflated after capture.
			UAM_ASSERT(uam::io::WriteTextFile(app.data_root / "agents" / "capacity-reader.md",
			    "---\nversion: 1\nname: capacity-reader\ndescription: Bounded private capacity reader\nmode: primary\nworkspaceAccess: read\nskills: []\ndelegates: []\n---\n" +
			    std::string(body_size, 'r') + "\n"));
			return CapturePolicy("capacity-reader", id);
		}

		void AddRecord(const uam::AcpQueuedUserPromptState& prompt)
		{
			auto& chat = app.chats.front();
			Message row;
			row.role = MessageRole::User;
			row.provider = chat.provider_id;
			row.content = prompt.text;
			row.created_at = "2026-10-19T15:30:" + std::to_string(10 + chat.messages.size()) + "Z";
			row.interrupted = row.acp_prompt_not_sent = true;
			row.attachments = prompt.attachments;
			row.markdown_store_files = prompt.markdown_store_files;
			row.markdown_store_prompt_blocks = prompt.markdown_store_prompt_blocks;
			chat.messages.push_back(row);
			auto recovery = uam::prompt_recovery::Capture(chat, static_cast<int>(chat.messages.size() - 1), prompt);
			UAM_ASSERT(uam::prompt_recovery::BindingMatches(chat, recovery));
			chat.acp_unsent_prompt_recoveries.push_back(std::move(recovery));
			UAM_ASSERT(uam::prompt_recovery::BindingsMatch(chat));
		}

		void SeedRecordLimit()
		{
			for (std::size_t i = 0; i < uam::prompt_recovery::kMaxRecords; ++i)
			{
				auto old = plan;
				old.id = "previous-plan-" + std::to_string(i);
				old.text = "Previous authored Plan " + std::to_string(i);
				AddRecord(old);
			}
			UAM_ASSERT_EQ(app.chats.front().acp_unsent_prompt_recoveries.size(), uam::prompt_recovery::kMaxRecords);
		}

		void SeedExactByteLimit()
		{
			for (int i = 0; i < 6; ++i) AddRecord(LargePolicy(160U * 1024U, "large-" + std::to_string(i)));
			auto& chat = app.chats.front();
			std::size_t used = 0;
			for (const auto& record : chat.acp_unsent_prompt_recoveries) used += uam::prompt_recovery::PolicyBytes(record);
			// Measure the real codec overhead, then author one legal definition to fill the aggregate exactly.
			auto last = LargePolicy(1, "last-large");
			ChatSession probe;
			probe.id = chat.id; probe.provider_id = chat.provider_id;
			probe.messages.push_back({.role = MessageRole::User, .content = last.text, .created_at = "2026-10-19T15:30:16Z", .provider = chat.provider_id});
			auto record = uam::prompt_recovery::Capture(probe, 0, last);
			const std::size_t overhead = uam::prompt_recovery::PolicyBytes(record);
			UAM_ASSERT(used + overhead < uam::prompt_recovery::kMaxBytes);
			const std::size_t remainder = uam::prompt_recovery::kMaxBytes - used - overhead;
			last = LargePolicy(1 + remainder / 2, (remainder % 2) ? "last-largex" : "last-large");
			AddRecord(last);
			used = 0;
			for (const auto& value : chat.acp_unsent_prompt_recoveries) used += uam::prompt_recovery::PolicyBytes(value);
			UAM_ASSERT_EQ(used, uam::prompt_recovery::kMaxBytes);
			UAM_ASSERT(chat.acp_unsent_prompt_recoveries.size() < uam::prompt_recovery::kMaxRecords);
		}

		void SaveAndRestore(const std::vector<uam::AcpQueuedUserPromptState>& queue, bool held)
		{
			auto& chat = app.chats.front();
			chat.acp_queued_prompts = queue;
			chat.acp_queue_held = held;
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id);
			UAM_ASSERT(saved && saved->messages == chat.messages && saved->acp_unsent_prompt_recoveries == chat.acp_unsent_prompt_recoveries);
			chat = *saved;
			UAM_ASSERT_EQ(uam::RestoreRemoteAcpSessionsAfterRestart(app, held), std::size_t{1});
			UAM_ASSERT_EQ(Session().queued_user_prompts, (std::deque<uam::AcpQueuedUserPromptState>(queue.begin(), queue.end())));
			UAM_ASSERT(!Session().session_ready && !Session().running && !Session().processing);
		}

		void AssertRetained(const std::vector<Message>& rows, const std::vector<uam::AcpUnsentPromptRecovery>& records,
		    const std::vector<uam::AcpQueuedUserPromptState>& queue)
		{
			const auto& chat = app.chats.front();
			UAM_ASSERT_EQ(chat.messages, rows);
			UAM_ASSERT_EQ(chat.acp_unsent_prompt_recoveries, records);
			UAM_ASSERT_EQ(chat.acp_queued_prompts, queue);
			UAM_ASSERT(chat.acp_queue_held && uam::prompt_recovery::BindingsMatch(chat));
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id);
			UAM_ASSERT(saved && saved->acp_queue_held);
			UAM_ASSERT_EQ(saved->messages, rows);
			UAM_ASSERT_EQ(saved->acp_unsent_prompt_recoveries, records);
			UAM_ASSERT_EQ(saved->acp_queued_prompts, queue);
		}
	};

#if defined(__APPLE__) || defined(__linux__)
	struct CapturedStopPrivateProcess
	{
		CapturedStopCarrierFixture& fixture;
		pid_t pid = -1;
		explicit CapturedStopPrivateProcess(CapturedStopCarrierFixture& value) : fixture(value) {}
		void Start()
		{
			auto& session = fixture.Session();
			std::string error;
			UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(session, fixture.temp.root,
			    {"/bin/sh", "-c", "while IFS= read -r line; do printf '%s\\n' \"$line\" >> \"$1\"; done", "private-peer", (fixture.temp.root / "received-input").string()}, &error));
			pid = session.child_pid;
			UAM_ASSERT(pid > 0);
			session.running = true; // Actual owned process, never initialized or native-ready.
		}
		bool Gone() const { errno = 0; return pid > 0 && ::kill(pid, 0) == -1 && errno == ESRCH; }
		void AwaitRetired()
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while ((!Gone() || !fixture.app.acp_process_stop_tasks.empty()) && std::chrono::steady_clock::now() < deadline)
			{
				(void)uam::PollCompletedAcpProcessStops(fixture.app, nullptr);
				std::this_thread::yield();
			}
			UAM_ASSERT(Gone() && fixture.app.acp_process_stop_tasks.empty());
			UAM_ASSERT(!fs::exists(fixture.temp.root / "received-input") || ReadFile(fixture.temp.root / "received-input").empty());
		}
		~CapturedStopPrivateProcess() noexcept
		{
			try
			{
				uam::FastStopAcpSessionsForExit(fixture.app);
				if (pid > 0 && !Gone()) std::abort();
			}
			catch (...) { std::abort(); }
		}
	};
#endif
}

#if defined(__APPLE__) || defined(__linux__)
UAM_TEST(CapturedStopAtRecordCapacityRetainsExactPlanWhilePhysicalProcessRetires)
{
	// Distinct callers: Finalize, initially held Discard+Finalize, and FastStop's post-conversion save.
	for (int route = 0; route < 3; ++route)
	{
		CapturedStopCarrierFixture fixture;
		fixture.SeedRecordLimit();
		const std::vector<uam::AcpQueuedUserPromptState> queue{fixture.plan};
		fixture.SaveAndRestore(queue, route == 1);
		const auto rows = fixture.app.chats.front().messages;
		const auto records = fixture.app.chats.front().acp_unsent_prompt_recoveries;
		CapturedStopPrivateProcess process(fixture);
		process.Start();
		if (route == 2) uam::FastStopAcpSessionsForExit(fixture.app);
		else UAM_ASSERT(uam::StopAcpSession(fixture.app, fixture.app.chats.front().id));
		process.AwaitRetired();
		fixture.AssertRetained(rows, records, queue);
		if (route == 2) UAM_ASSERT(fixture.app.acp_sessions.empty());
		else
		{
			UAM_ASSERT(fixture.Session().queue_held && !fixture.Session().running);
			UAM_ASSERT_EQ(fixture.Session().queued_user_prompts, (std::deque<uam::AcpQueuedUserPromptState>{fixture.plan}));
			UAM_ASSERT(uam::StopAcpSession(fixture.app, fixture.app.chats.front().id));
			fixture.AssertRetained(rows, records, queue); // Repeated explicit Stop must not duplicate or drop it.
		}
	}
}

UAM_TEST(CapturedStopAtExactByteCapacityRetainsValidPolicyWithoutAppending)
{
	for (const bool fast : {false, true})
	{
		CapturedStopCarrierFixture fixture;
		fixture.SeedExactByteLimit();
		const std::vector<uam::AcpQueuedUserPromptState> queue{fixture.plan};
		fixture.SaveAndRestore(queue, false);
		const auto rows = fixture.app.chats.front().messages;
		const auto records = fixture.app.chats.front().acp_unsent_prompt_recoveries;
		CapturedStopPrivateProcess process(fixture);
		process.Start();
		if (fast) uam::FastStopAcpSessionsForExit(fixture.app);
		else UAM_ASSERT(uam::StopAcpSession(fixture.app, fixture.app.chats.front().id));
		process.AwaitRetired();
		fixture.AssertRetained(rows, records, queue);
	}
}

UAM_TEST(CapturedStopMixedReferenceAndNewRowsKeepExactPolicyIndex)
{
	CapturedStopCarrierFixture fixture;
	fixture.AddRecord(fixture.plan);
	auto reference = fixture.plan;
	reference.append_user_message = false;
	reference.existing_user_message_index = 0;
	reference.existing_user_message_created_at = fixture.app.chats.front().messages.front().created_at;
	uam::AcpQueuedUserPromptState ordinary;
	ordinary.id = "ordinary-raw"; ordinary.text = "Ordinary authored input";
	auto next = fixture.plan;
	next.id = "new-captured-plan"; next.text = "Next captured Plan input";
	fixture.SaveAndRestore({reference, ordinary, next}, false);
	const auto old_row = fixture.app.chats.front().messages.front();
	const auto old_record = fixture.app.chats.front().acp_unsent_prompt_recoveries.front();
	CapturedStopPrivateProcess process(fixture);
	process.Start();
	UAM_ASSERT(uam::StopAcpSession(fixture.app, fixture.app.chats.front().id));
	process.AwaitRetired();
	const auto& chat = fixture.app.chats.front();
	UAM_ASSERT_EQ(chat.messages.size(), std::size_t{3});
	UAM_ASSERT_EQ(chat.messages.front(), old_row);
	UAM_ASSERT(chat.messages[1].content == ordinary.text && chat.messages[2].content == next.text);
	UAM_ASSERT(chat.messages[1].acp_prompt_not_sent && chat.messages[2].acp_prompt_not_sent);
	UAM_ASSERT_EQ(chat.messages[2].attachments, next.attachments);
	UAM_ASSERT_EQ(chat.messages[2].markdown_store_files, next.markdown_store_files);
	UAM_ASSERT_EQ(chat.messages[2].markdown_store_prompt_blocks, next.markdown_store_prompt_blocks);
	UAM_ASSERT_EQ(chat.acp_unsent_prompt_recoveries.size(), std::size_t{2});
	UAM_ASSERT_EQ(chat.acp_unsent_prompt_recoveries.front(), old_record);
	UAM_ASSERT_EQ(chat.acp_unsent_prompt_recoveries.back(), uam::prompt_recovery::Capture(chat, 2, next));
	UAM_ASSERT(uam::prompt_recovery::BindingsMatch(chat));
	UAM_ASSERT(chat.acp_queued_prompts.empty() && fixture.Session().queued_user_prompts.empty());
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, chat.id);
	UAM_ASSERT(saved && saved->messages == chat.messages && saved->acp_unsent_prompt_recoveries == chat.acp_unsent_prompt_recoveries);
	const auto after = chat.messages;
	UAM_ASSERT(uam::StopAcpSession(fixture.app, chat.id));
	UAM_ASSERT_EQ(chat.messages, after);
	UAM_ASSERT_EQ(chat.acp_unsent_prompt_recoveries.size(), std::size_t{2});
}
#endif

UAM_TEST(CapturedStopSharedConversionRejectsInvalidBindingsBeforeAnyMutation)
{
	for (const bool invalid_old : {false, true})
	{
		CapturedStopCarrierFixture fixture;
		fixture.AddRecord(fixture.plan);
		auto next = fixture.plan;
		next.id = "next-authored-plan"; next.text = "Retain invalid binding input";
		auto& chat = fixture.app.chats.front();
		if (invalid_old) chat.acp_unsent_prompt_recoveries.front().row_witness = "wrong-binding";
		else next.id.clear(); // Invalid new admission binding, not a valid-capacity test.
		uam::AcpSessionState session;
		session.chat_id = chat.id; session.provider_id = chat.provider_id;
		session.queued_user_prompts.push_back(next);
		chat.acp_queued_prompts = {next};
		const auto rows = chat.messages;
		const auto records = chat.acp_unsent_prompt_recoveries;
		const auto queue = chat.acp_queued_prompts;
		UAM_ASSERT(uam::PersistQueuedAcpUserPromptsAsInterrupted(session, chat));
		UAM_ASSERT(session.queue_held && chat.acp_queue_held);
		UAM_ASSERT_EQ(chat.messages, rows);
		UAM_ASSERT_EQ(chat.acp_unsent_prompt_recoveries, records);
		UAM_ASSERT_EQ(chat.acp_queued_prompts, queue);
		UAM_ASSERT_EQ(session.queued_user_prompts, (std::deque<uam::AcpQueuedUserPromptState>{next}));
		UAM_ASSERT(!uam::PersistQueuedAcpUserPromptsAsInterrupted(session, chat)); // No second mutation.
		UAM_ASSERT_EQ(chat.messages, rows);
	}
}
