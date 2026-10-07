#include "test_harness.h"
#include "cef/state_serializer.h"

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
