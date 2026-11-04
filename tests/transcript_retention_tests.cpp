#include "test_harness.h"
#include "app/transcript_retention_service.h"
#include "cef/state_serializer.h"
#include "common/runtime/acp/acp_session_state_helpers.h"

using namespace uam_test;

namespace
{
	ChatSession RetentionChat()
	{
		ChatSession chat;
		chat.id = "retention-chat";
		chat.provider_id = "codex-cli";
		chat.messages.push_back(Message{MessageRole::Assistant, "durable answer"});
		return chat;
	}
}

UAM_TEST(TranscriptRetentionReleasesDurableColdHistoryAndRehydrates)
{
	TempDir temp("uam-transcript-retention");
	uam::AppState app;
	app.data_root = temp.root;
	app.chats.push_back(RetentionChat());
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
	const auto saved = ChatRepository::LoadLocalChat(app.data_root, app.chats.front().id, true);
	UAM_ASSERT(saved.has_value());
	uam::TranscriptRetentionService retention;
	UAM_ASSERT(!retention.BeginRelease(app, saved->id));
	retention.SetVisibleChatIds({});
	const auto generation = retention.BeginRelease(app, saved->id);
	UAM_ASSERT(generation.has_value());
	UAM_ASSERT(retention.CompleteRelease(app, saved->id, *generation, *saved));
	UAM_ASSERT(app.chats.front().messages.empty() && !app.chats.front().messages_loaded);
	UAM_ASSERT_EQ(app.chats.front().persisted_message_count, saved->messages.size());
	UAM_ASSERT_EQ(app.chats.front().persisted_messages_digest, saved->persisted_messages_digest);
	UAM_ASSERT(ChatRepository::HydrateChatMessages(app.data_root, app.chats.front()));
	UAM_ASSERT_EQ(app.chats.front().messages, saved->messages);
}

UAM_TEST(ManagedAgentTranscriptDetachedReadValidatesColdIdentityAndKeepsLiveStateCold)
{
	TempDir temp("uam-managed-transcript-detached-read");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession root;
	root.id = "managed-read-root";
	root.provider_id = "codex-cli";
	root.workspace_directory = temp.root.string();
	ChatSession transcript;
	transcript.id = "managed-read-transcript";
	transcript.agent_run_id = "managed-read-run";
	transcript.provider_id = "codex-cli";
	transcript.workspace_directory = temp.root.string();
	for (int index = 0; index < 1000; ++index)
		transcript.messages.push_back(Message{MessageRole::Assistant, "transcript message " + std::to_string(index)});
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, root));
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, transcript));
	const std::optional<ChatSession> cold_root = ChatRepository::LoadLocalChat(temp.root, root.id, false);
	const std::optional<ChatSession> cold_transcript = ChatRepository::LoadLocalChat(temp.root, transcript.id, false);
	UAM_ASSERT(cold_root.has_value() && cold_transcript.has_value());
	app.chats = {*cold_root, *cold_transcript};
	AgentRun run;
	run.id = transcript.agent_run_id;
	run.root_chat_id = root.id;
	run.transcript_chat_id = transcript.id;
	run.agent_id = "worker";
	run.provider_id = transcript.provider_id;
	run.execution_capability = "acp";
	run.status = "completed";
	run.updated_at = "stable-terminal-run";
	app.agent_runs.push_back(run);

	const std::optional<uam::ManagedAgentTranscriptReadIdentity> identity =
	    uam::CaptureManagedAgentTranscriptRead(app, root.id, transcript.id);
	UAM_ASSERT(identity.has_value());
	const uam::ManagedAgentTranscriptReadResult read =
	    uam::LoadManagedAgentTranscriptDetached(temp.root, *identity);
	UAM_ASSERT_EQ(read.status, uam::ManagedAgentTranscriptReadStatus::Ready);
	UAM_ASSERT_EQ(read.transcript.messages.size(), std::size_t{1000});
	UAM_ASSERT(uam::MatchesManagedAgentTranscriptRead(app, *identity, read.transcript));
	UAM_ASSERT(!app.chats[1].messages_loaded && app.chats[1].messages.empty());
	UAM_ASSERT_EQ(app.chats[1].persisted_message_count, std::size_t{1000});
	auto active_session = std::make_unique<uam::AcpSessionState>();
	active_session->chat_id = transcript.id;
	active_session->processing = true;
	app.acp_sessions.push_back(std::move(active_session));
	UAM_ASSERT(!uam::MatchesManagedAgentTranscriptRead(app, *identity, read.transcript));
	app.acp_sessions.clear();
	UAM_ASSERT(uam::MatchesManagedAgentTranscriptRead(app, *identity, read.transcript));

	app.agent_runs.front().updated_at = "run-changed-during-read";
	UAM_ASSERT(!uam::MatchesManagedAgentTranscriptRead(app, *identity, read.transcript));
	app.agent_runs.front() = run;
	app.chats.front().workspace_directory = "changed-workspace";
	UAM_ASSERT(!uam::MatchesManagedAgentTranscriptRead(app, *identity, read.transcript));
}

UAM_TEST(ManagedAgentTranscriptDetachedReadRejectsChangedDurableContentAndWarmOrHotState)
{
	TempDir temp("uam-managed-transcript-detached-stale");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession root;
	root.id = "managed-stale-root";
	ChatSession transcript;
	transcript.id = "managed-stale-transcript";
	transcript.agent_run_id = "managed-stale-run";
	transcript.messages.push_back(Message{MessageRole::Assistant, "before"});
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, root));
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, transcript));
	app.chats = {*ChatRepository::LoadLocalChat(temp.root, root.id, false),
	             *ChatRepository::LoadLocalChat(temp.root, transcript.id, false)};
	AgentRun run;
	run.id = transcript.agent_run_id;
	run.root_chat_id = root.id;
	run.transcript_chat_id = transcript.id;
	run.provider_id = transcript.provider_id;
	run.status = "failed";
	run.updated_at = "terminal";
	app.agent_runs.push_back(run);
	const auto identity = uam::CaptureManagedAgentTranscriptRead(app, root.id, transcript.id);
	UAM_ASSERT(identity.has_value());

	ChatSession changed = transcript;
	changed.messages.front().content = "after";
	changed.updated_at = "updated-during-read";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, changed));
	const uam::ManagedAgentTranscriptReadResult stale =
	    uam::LoadManagedAgentTranscriptDetached(temp.root, *identity);
	UAM_ASSERT_EQ(stale.status, uam::ManagedAgentTranscriptReadStatus::Changed);

	app.chats[1].messages_loaded = true;
	app.chats[1].messages = {Message{MessageRole::Assistant, "unsaved warm answer"}};
	UAM_ASSERT(!uam::CaptureManagedAgentTranscriptRead(app, root.id, transcript.id).has_value());
	app.chats[1].messages.clear();
	app.chats[1].messages_loaded = false;
	app.agent_runs.front().status = "running";
	UAM_ASSERT(!uam::CaptureManagedAgentTranscriptRead(app, root.id, transcript.id).has_value());
}

UAM_TEST(TranscriptPageLoadingLeavesNativeHistoryColdAndPreservesDurableContext)
{
	uam_test::TempDir temp("uam-cold-transcript-page");
	ChatSession original = RetentionChat();
	for (int index = 0; index < 999; ++index) original.messages.push_back(Message{MessageRole::Assistant, "durable history " + std::to_string(index)});
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, original));
	std::optional<ChatSession> summary = ChatRepository::LoadLocalChat(temp.root, original.id, false);
	std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(temp.root, original.id, true);
	UAM_ASSERT(summary.has_value() && loaded.has_value());
	const std::size_t count = summary->persisted_message_count;
	const std::string digest = summary->persisted_messages_digest;
	const Message* const loaded_messages = loaded->messages.data();
	UAM_ASSERT(uam::TranscriptRetentionService::CompleteHistoryLoad(*summary, std::move(*loaded), count, digest, false));
	UAM_ASSERT(loaded->messages.data() == loaded_messages);
	UAM_ASSERT_EQ(loaded->messages.size(), original.messages.size());
	UAM_ASSERT(!summary->messages_loaded && summary->messages.empty());
	UAM_ASSERT_EQ(summary->persisted_message_count, std::size_t{1000});
	UAM_ASSERT_EQ(summary->persisted_messages_digest, digest);
	summary->title = "Renamed cold chat";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, *summary));
	UAM_ASSERT(ChatRepository::HydrateChatMessages(temp.root, *summary));
	UAM_ASSERT_EQ(summary->messages, original.messages);
	UAM_ASSERT_EQ(summary->title, std::string("Renamed cold chat"));

	summary = ChatRepository::LoadLocalChat(temp.root, original.id, false);
	loaded = ChatRepository::LoadLocalChat(temp.root, original.id, true);
	UAM_ASSERT(summary.has_value() && loaded.has_value());
	UAM_ASSERT(uam::TranscriptRetentionService::CompleteHistoryLoad(*summary, std::move(*loaded), summary->persisted_message_count, summary->persisted_messages_digest, true));
	UAM_ASSERT(summary->messages_loaded && summary->messages == original.messages);
	for (int guard = 0; guard < 7; ++guard)
	{
		summary = ChatRepository::LoadLocalChat(temp.root, original.id, false);
		loaded = ChatRepository::LoadLocalChat(temp.root, original.id, true);
		UAM_ASSERT(summary.has_value() && loaded.has_value());
		const std::size_t expected_count = summary->persisted_message_count;
		const std::string expected_digest = summary->persisted_messages_digest;
		if (guard == 0) ++summary->persisted_message_count;
		if (guard == 1) summary->persisted_messages_digest = "changed";
		if (guard == 2) loaded->id = "different";
		if (guard == 3) ++loaded->persisted_message_count;
		if (guard == 4) loaded->persisted_messages_digest = "changed";
		if (guard == 5) loaded->messages_loaded = false;
		if (guard == 6) summary->messages_loaded = true;
		UAM_ASSERT(!uam::TranscriptRetentionService::CompleteHistoryLoad(*summary, std::move(*loaded), expected_count, expected_digest, false));
		UAM_ASSERT(summary->messages.empty());
		UAM_ASSERT_EQ(summary->messages_loaded, guard == 6);
	}
}

UAM_TEST(TranscriptRetentionProtectsEveryVisiblePaneAndReopenedHydration)
{
	uam::AppState app;
	app.chats.push_back(RetentionChat());
	uam::TranscriptRetentionService retention;
	retention.SetVisibleChatIds({app.chats.front().id});
	UAM_ASSERT(!retention.BeginRelease(app, app.chats.front().id));
	retention.SetVisibleChatIds({});
	const auto generation = retention.BeginRelease(app, app.chats.front().id);
	UAM_ASSERT(generation.has_value());
	const ChatSession saved = app.chats.front();
	retention.SetVisibleChatIds({saved.id});
	UAM_ASSERT(!retention.CompleteRelease(app, saved.id, *generation, saved));
	retention.SetVisibleChatIds({});
	const auto later = retention.BeginRelease(app, saved.id);
	UAM_ASSERT(later.has_value());
	retention.Invalidate(saved.id);
	UAM_ASSERT(!retention.CompleteRelease(app, saved.id, *later, saved));
	UAM_ASSERT(app.chats.front().messages_loaded);
}

UAM_TEST(TranscriptRetentionNeverHidesSameLengthUnsavedContentOrToolChanges)
{
	for (const bool change_tool : {false, true})
	{
		uam::AppState app;
		app.chats.push_back(RetentionChat());
		ToolCall call;
		call.id = "call";
		call.result_text = "before";
		app.chats.front().messages.front().tool_calls.push_back(call);
		const ChatSession saved = app.chats.front();
		const std::string before = uam::StateSerializer::MessageDigest(saved);
		uam::TranscriptRetentionService retention;
		retention.SetVisibleChatIds({});
		const auto generation = retention.BeginRelease(app, saved.id);
		UAM_ASSERT(generation.has_value());
		if (change_tool) app.chats.front().messages.front().tool_calls.front().result_text = "after!";
		else app.chats.front().messages.front().content = "unsaved answer";
		UAM_ASSERT_EQ(app.chats.front().messages.front().content.size(), saved.messages.front().content.size());
		UAM_ASSERT(!retention.CompleteRelease(app, saved.id, *generation, saved));
		UAM_ASSERT(app.chats.front().messages_loaded);
		UAM_ASSERT(uam::StateSerializer::MessageDigest(app.chats.front()) != before);
	}
}

UAM_TEST(TranscriptRetentionProtectsPendingSaveSelectionAndRuntimeOwnership)
{
	for (int guard = 0; guard < 12; ++guard)
	{
		uam::AppState app;
		app.chats.push_back(RetentionChat());
		const std::string id = app.chats.front().id;
		const ChatSession saved = app.chats.front();
		uam::TranscriptRetentionService retention;
		retention.SetVisibleChatIds({});
		const auto generation = retention.BeginRelease(app, id);
		UAM_ASSERT(generation.has_value());
		if (guard == 0) app.pending_chat_save_at_by_chat_id[id] = 999;
		if (guard == 1) app.selected_chat_index = 0;
		if (guard == 2) app.worktree_operation_chat_ids.insert(id);
		if (guard == 3) app.native_chat_load_task.running = true;
		if (guard == 4) app.chats.front().remote_turn_reconnect_pending = true;
		if (guard == 5) app.chats.front().remote_pending_requests.emplace_back();
		if (guard == 6) { Goal goal; goal.status = GoalStatus::Active; app.chats.front().goals.push_back(goal); }
		if (guard >= 7)
		{
			auto session = std::make_unique<uam::AcpSessionState>();
			session->chat_id = id;
			if (guard == 7) session->processing = true;
			if (guard == 8) session->reconnect_pending = true;
			if (guard == 9) session->turn_checkpoint_preflight_pending = true;
			if (guard == 10) session->turn_checkpoint_commit_pending = true;
			if (guard == 11) session->pending_user_input.request_id_json = "1";
			app.acp_sessions.push_back(std::move(session));
		}
		UAM_ASSERT(!retention.BeginRelease(app, id));
		UAM_ASSERT(!retention.CompleteRelease(app, id, *generation, saved));
		UAM_ASSERT(app.chats.front().messages_loaded && app.chats.front().messages == saved.messages);
	}
}

UAM_TEST(TranscriptRetentionReleasesPausedOrBlockedGoalHistoryButProtectsActiveGoals)
{
	for (const GoalStatus status : {GoalStatus::Paused, GoalStatus::Blocked})
	{
		TempDir temp("uam-transcript-retention-settled-goal");
		uam::AppState app;
		app.data_root = temp.root;
		ChatSession chat = RetentionChat();
		chat.goals.push_back(Goal{});
		chat.goals.back().status = status;
		app.chats.push_back(std::move(chat));
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, app.chats.front().id, true);
		UAM_ASSERT(saved.has_value());

		uam::TranscriptRetentionService retention;
		retention.SetVisibleChatIds({});
		const auto generation = retention.BeginRelease(app, saved->id);
		UAM_ASSERT(generation.has_value());
		UAM_ASSERT(retention.CompleteRelease(app, saved->id, *generation, *saved));
		UAM_ASSERT(app.chats.front().messages.empty() && !app.chats.front().messages_loaded);
		UAM_ASSERT(ChatRepository::HydrateChatMessages(app.data_root, app.chats.front()));
		UAM_ASSERT_EQ(app.chats.front().messages, saved->messages);
	}

	uam::AppState app;
	app.chats.push_back(RetentionChat());
	app.chats.front().goals.push_back(Goal{});
	app.chats.front().goals.back().status = GoalStatus::Active;
	uam::TranscriptRetentionService retention;
	retention.SetVisibleChatIds({});
	UAM_ASSERT(!retention.BeginRelease(app, app.chats.front().id));
}

UAM_TEST(TranscriptRetentionKeepsMissingOrMismatchedDurableHistory)
{
	uam::AppState app;
	app.chats.push_back(RetentionChat());
	uam::TranscriptRetentionService retention;
	retention.SetVisibleChatIds({});
	const auto generation = retention.BeginRelease(app, app.chats.front().id);
	UAM_ASSERT(generation.has_value());
	ChatSession wrong = app.chats.front();
	wrong.messages_loaded = false;
	UAM_ASSERT(!retention.CompleteRelease(app, wrong.id, *generation, wrong));
	wrong.messages_loaded = true;
	wrong.id = "different";
	UAM_ASSERT(!retention.CompleteRelease(app, app.chats.front().id, *generation, wrong));
	UAM_ASSERT(app.chats.front().messages_loaded);
}

UAM_TEST(CopiedLegacyChatUpgradeReopensWithoutChangingOriginalOrRollbackBackup)
{
	TempDir temp("uam-copied-legacy-upgrade");
	const std::string legacy = R"({"id":"legacy-upgrade","provider_id":"codex-cli","native_session_id":"6a6f0f3b-1a0b-4a9c-8a01-111111111111","approval_mode":"yolo","workspace_directory":"/tmp/legacy-workspace","created_at":"2026-01-01 00:00:00","updated_at":"2026-01-01 00:00:01","messages":[{"role":"user","content":"Retain my question"},{"role":"assistant","content":"Retain my answer"}]})";
	const fs::path original = temp.root / "original.json";
	const fs::path copied_root = temp.root / "upgrade-copy";
	const fs::path copied = copied_root / "chats" / "legacy-upgrade.json";
	fs::create_directories(copied.parent_path());
	UAM_ASSERT(uam::io::WriteTextFile(original, legacy));
	fs::copy_file(original, copied);
	const auto migrated = ChatRepository::LoadLocalChat(copied_root, "legacy-upgrade", true);
	UAM_ASSERT(migrated.has_value());
	UAM_ASSERT_EQ(migrated->approval_mode, std::string("default"));
	UAM_ASSERT_EQ(migrated->command_safety_tier, std::string("yolo"));
	UAM_ASSERT(ChatRepository::SaveChat(copied_root, *migrated));
	const auto reopened = ChatRepository::LoadLocalChat(copied_root, migrated->id, true);
	UAM_ASSERT(reopened.has_value());
	UAM_ASSERT_EQ(reopened->messages, migrated->messages);
	UAM_ASSERT_EQ(reopened->provider_id, migrated->provider_id);
	UAM_ASSERT_EQ(reopened->native_session_id, migrated->native_session_id);
	UAM_ASSERT_EQ(reopened->workspace_directory, migrated->workspace_directory);
	UAM_ASSERT_EQ(reopened->command_safety_tier, migrated->command_safety_tier);
	UAM_ASSERT_EQ(ReadFile(original), legacy);
	UAM_ASSERT_EQ(ReadFile(uam::io::MakeBackupPath(copied)), legacy);
}

UAM_TEST(TranscriptReleaseTransfersLargeBuffersWithoutDestroyingThemOnCompletion)
{
	uam::AppState app;
	app.chats.push_back(RetentionChat());
	app.chats.front().messages.front().content.assign(8 * 1024 * 1024, 'x');
	const ChatSession persisted = app.chats.front();
	const char* original = app.chats.front().messages.front().content.data();
	uam::TranscriptRetentionService retention;
	retention.SetVisibleChatIds({});
	const auto generation = retention.BeginRelease(app, persisted.id);
	UAM_ASSERT(generation.has_value());
	std::vector<Message> released;
	UAM_ASSERT(retention.CompleteRelease(app, persisted.id, *generation, persisted, &released));
	UAM_ASSERT(!app.chats.front().messages_loaded);
	UAM_ASSERT(app.chats.front().messages.empty());
	UAM_ASSERT_EQ(released.front().content.data(), original);
}
