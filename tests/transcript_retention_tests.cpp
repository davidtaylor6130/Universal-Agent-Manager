#include "test_harness.h"
#include "app/transcript_retention_service.h"
#include "cef/state_serializer.h"

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
