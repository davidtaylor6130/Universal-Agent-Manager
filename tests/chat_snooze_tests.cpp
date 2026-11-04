#include "test_harness.h"
#include "app/chat_domain_service.h"
#include "common/utils/time_utils.h"
#include "common/runtime/acp/acp_session_runtime.h"

using namespace uam_test;

UAM_TEST(ChatSnoozePersistsThroughSummaryAndHydrationWithoutRewritingTranscript)
{
	TempDir temp("uam-chat-snooze");
	uam::AppState app; app.data_root = temp.root;
	ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli"); chat.id = "snooze-persist";
	ChatDomainService().AddMessage(chat, MessageRole::User, "Keep the transcript");
	app.chats.push_back(chat);
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	const std::string original = uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, chat.id));
	const std::int64_t until = uam::time::SystemEpochMillisecondsNow() + 60'000;
	UAM_ASSERT(ChatDomainService().SetChatSnoozedUntil(app, chat.id, until));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(app.data_root, chat.id)), original);
	std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(app.data_root, chat.id, false);
	UAM_ASSERT(loaded.has_value()); UAM_ASSERT_EQ(loaded->snoozed_until_ms, until);
	UAM_ASSERT(ChatRepository::HydrateChatMessages(app.data_root, *loaded));
	UAM_ASSERT_EQ(loaded->snoozed_until_ms, until); UAM_ASSERT_EQ(loaded->messages.front().content, std::string("Keep the transcript"));
	UAM_ASSERT(ChatDomainService().SetChatSnoozedUntil(app, chat.id, 0));
	loaded = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(loaded.has_value()); UAM_ASSERT_EQ(loaded->snoozed_until_ms, std::int64_t{0});
}

UAM_TEST(ChatSnoozeRejectsPendingRequestsAndQueuesWithoutStoppingProcessing)
{
	uam::AppState app; ChatSession chat; chat.id = "snooze-running"; app.chats.push_back(chat);
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back(); session.chat_id = chat.id; session.processing = true; session.running = true;
	const std::int64_t until = uam::time::SystemEpochMillisecondsNow() + 60'000;
	ChatDomainService domain; std::string error;
	UAM_ASSERT(domain.SetChatSnoozedUntil(app, chat.id, until, &error));
	UAM_ASSERT(session.processing && session.running);
	session.waiting_for_permission = true;
	UAM_ASSERT(!domain.SetChatSnoozedUntil(app, chat.id, until + 1, &error)); UAM_ASSERT(error.find("pending") != std::string::npos);
	UAM_ASSERT(domain.SetChatSnoozedUntil(app, chat.id, 0));
	session.waiting_for_permission = false; session.waiting_for_user_input = true;
	UAM_ASSERT(!domain.SetChatSnoozedUntil(app, chat.id, until));
	session.waiting_for_user_input = false; session.queued_prompt = "queued";
	UAM_ASSERT(!domain.SetChatSnoozedUntil(app, chat.id, until)); UAM_ASSERT_EQ(session.queued_prompt, std::string("queued"));
	session.queued_prompt.clear(); app.chats.front().acp_queued_prompts.emplace_back();
	UAM_ASSERT(!domain.SetChatSnoozedUntil(app, chat.id, until)); UAM_ASSERT_EQ(app.chats.front().acp_queued_prompts.size(), std::size_t{1});
	UAM_ASSERT(session.processing && session.running);
}

UAM_TEST(ChatSnoozeAttentionAndExplicitUserMessagesWakeWithoutAcknowledgingAttention)
{
	TempDir temp("uam-snooze-wake"); uam::AppState app; app.data_root = temp.root;
	ChatSession chat; chat.id = "snooze-wake"; app.chats.push_back(chat);
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	ChatDomainService domain; const std::int64_t until = uam::time::SystemEpochMillisecondsNow() + 60'000;
	UAM_ASSERT(domain.SetChatSnoozedUntil(app, chat.id, until));
	domain.MarkChatNeedsAttention(app, chat.id);
	UAM_ASSERT_EQ(app.chats.front().snoozed_until_ms, std::int64_t{0});
	UAM_ASSERT(!app.chats.front().attention_revision.empty()); UAM_ASSERT(app.chats_with_unseen_updates.contains(chat.id));
	const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(loaded.has_value()); UAM_ASSERT_EQ(loaded->snoozed_until_ms, std::int64_t{0});
	UAM_ASSERT(domain.SetChatSnoozedUntil(app, chat.id, until));
	domain.AddMessage(app.chats.front(), MessageRole::User, "Resume now");
	UAM_ASSERT_EQ(app.chats.front().snoozed_until_ms, std::int64_t{0}); UAM_ASSERT(!app.chats.front().attention_revision.empty());
}

UAM_TEST(ChatSnoozeValidatesDatesAndRestoresValueAfterStorageFailure)
{
	TempDir temp("uam-snooze-failure"); uam::AppState app; app.data_root = temp.root;
	ChatSession chat; chat.id = "snooze-failure"; chat.snoozed_until_ms = 42; app.chats.push_back(chat);
	ChatDomainService domain; const std::int64_t until = uam::time::SystemEpochMillisecondsNow() + 60'000;
	UAM_ASSERT(!domain.SetChatSnoozedUntil(app, "missing", until));
	UAM_ASSERT(!domain.SetChatSnoozedUntil(app, chat.id, -1));
	UAM_ASSERT(!domain.SetChatSnoozedUntil(app, chat.id, uam::time::SystemEpochMillisecondsNow() - 1));
	UAM_ASSERT(!domain.SetChatSnoozedUntil(app, chat.id, 8'640'000'000'000'001LL));
	// A file in place of the chat directory makes both summary and full-save fallback fail.
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "chats", "blocked"));
	std::string error; UAM_ASSERT(!domain.SetChatSnoozedUntil(app, chat.id, until, &error));
	UAM_ASSERT_EQ(app.chats.front().snoozed_until_ms, std::int64_t{42}); UAM_ASSERT(error.find("save") != std::string::npos);
}

UAM_TEST(ChatSnoozeBranchAttentionWakesRootAndQueuedBranchesBlockRootSnooze)
{
	TempDir temp("uam-snooze-family"); uam::AppState app; app.data_root = temp.root;
	ChatSession root; root.id = "snooze-root"; ChatSession branch; branch.id = "snooze-branch"; branch.branch_root_chat_id = root.id;
	app.chats.push_back(root); app.chats.push_back(branch);
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, root)); UAM_ASSERT(ChatRepository::SaveChat(app.data_root, branch));
	ChatDomainService domain; const std::int64_t until = uam::time::SystemEpochMillisecondsNow() + 60'000;
	app.chats.back().acp_queued_prompts.emplace_back(); UAM_ASSERT(!domain.SetChatSnoozedUntil(app, root.id, until));
	app.chats.back().acp_queued_prompts.clear(); UAM_ASSERT(domain.SetChatSnoozedUntil(app, root.id, until));
	UAM_ASSERT_EQ(app.chats.back().snoozed_until_ms, std::int64_t{0});
	domain.MarkChatNeedsAttention(app, branch.id);
	UAM_ASSERT_EQ(app.chats.front().snoozed_until_ms, std::int64_t{0}); UAM_ASSERT(!app.chats.back().attention_revision.empty());
	UAM_ASSERT(app.pending_chat_save_at_by_chat_id.contains(root.id));
	uam::FlushPendingChatSaves(app, true);
	const std::optional<ChatSession> restored = ChatRepository::LoadLocalChat(app.data_root, root.id);
	UAM_ASSERT(restored.has_value()); UAM_ASSERT_EQ(restored->snoozed_until_ms, std::int64_t{0});
}

UAM_TEST(ChatSnoozePreparedMetadataRejectsAChangedSummaryAndPreservesColdHistory)
{
	TempDir temp("uam-snooze-prepared"); ChatSession chat; chat.id = "snooze-prepared";
	ChatDomainService().AddMessage(chat, MessageRole::User, "Keep history");
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const std::string before = uam::io::ReadTextFile(AppPaths::UamChatFilePath(temp.root, chat.id));
	ChatSession metadata; metadata.id = chat.id; metadata.snoozed_until_ms = uam::time::SystemEpochMillisecondsNow() + 60'000;
	const std::shared_ptr<PreparedChatSave> prepared = ChatRepository::PrepareLastOpenedAt(temp.root, metadata);
	UAM_ASSERT(prepared != nullptr);
	chat.attention_revision = "newer-attention"; UAM_ASSERT(ChatRepository::SaveLastOpenedAt(temp.root, chat));
	UAM_ASSERT(!ChatRepository::PublishPreparedChatSave(prepared, true, &chat));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(AppPaths::UamChatFilePath(temp.root, chat.id)), before);
	const std::optional<ChatSession> restored = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(restored.has_value()); UAM_ASSERT_EQ(restored->attention_revision, std::string("newer-attention"));
	UAM_ASSERT_EQ(restored->snoozed_until_ms, std::int64_t{0});
}
