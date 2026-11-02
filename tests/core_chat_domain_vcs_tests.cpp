#include "common/companion/protocol.h"
#include "common/provider/copilot_native_history.h"
#include "core/chat_import_utils.h"
#include "cef/uam_bridge_request.h"
#include "common/platform/platform_speech_output.h"
#include "test_harness.h"
#include "common/platform/file_explorer_application.h"
#include "common/platform/powershell_selection.h"
#include "remote/remote_workspace_terminal.h"
#include "common/config/custom_icon.h"
#include "common/config/companion_network.h"
#include "app/chat_lifecycle_service.h"
#include "app/chat_search_service.h"
#include "app/local_chat_bundle_service.h"
#include "app/persistence_coordinator.h"
#include "app/transcript_retention_service.h"
#include "app/startup_native_import_service.h"
#include "app/runtime_orchestration_internal.h"
#include "app/uam_control_service.h"
#include "cef/state_serializer.h"
#include "cef/uam_cef_client.h"
#include "cef/uam_query_handler_internal.h"
#include "cef/uam_query_handler_async.h"
#include "common/config/mcp_server_config.h"
#include "common/chat/chat_repository.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_goal_loop.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/time_utils.h"
#include "common/utils/uuid.h"

#include <cstdio>
#include <fstream>
#include <future>
#include <type_traits>

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#include <versionhelpers.h>
#endif

#if defined(__APPLE__)
#include "common/platform/platform_application_macos.h"
#endif

using namespace uam_test;

UAM_TEST(ChatDomainServiceSortsByUpdatedThenCreatedWithoutSelectionReordering)
{
	ChatSession oldest;
	oldest.id = "oldest";
	oldest.created_at = "2026-01-01T00:00:00.000Z";
	oldest.updated_at = "2026-01-01T00:00:01.000Z";

	ChatSession opened;
	opened.id = "opened";
	opened.created_at = "2026-01-01T00:00:00.000Z";
	opened.updated_at = "2026-01-01T00:00:02.000Z";
	opened.last_opened_at = "2026-01-01T00:00:04.000Z";

	ChatSession updated;
	updated.id = "updated";
	updated.created_at = "2026-01-01T00:00:00.000Z";
	updated.updated_at = "2026-01-01T00:00:03.000Z";

	ChatSession created_tiebreaker;
	created_tiebreaker.id = "created-tiebreaker";
	created_tiebreaker.created_at = "2026-01-01T00:00:05.000Z";
	created_tiebreaker.updated_at = oldest.updated_at;

	std::vector<ChatSession> chats = {oldest, opened, updated, created_tiebreaker};
	ChatDomainService().SortChatsByRecent(chats);

	UAM_ASSERT_EQ(chats[0].id, std::string("updated"));
	UAM_ASSERT_EQ(chats[1].id, std::string("opened"));
	UAM_ASSERT_EQ(chats[2].id, std::string("created-tiebreaker"));
	UAM_ASSERT_EQ(chats[3].id, std::string("oldest"));
}

UAM_TEST(ChatDomainServiceDeduplicatesNativeIdentityAcrossProviderAliases)
{
	ChatSession canonical;
	canonical.id = "chat-canonical";
	canonical.provider_id = "codex-cli";
	canonical.workspace_directory = "/workspace/project";
	canonical.native_session_id = "6a6f0f3b-1a0b-4a9c-8a01-111111111111";
	canonical.created_at = "2026-01-01T00:00:00.000Z";
	canonical.updated_at = "2026-01-01T00:00:01.000Z";

	ChatSession aliased = canonical;
	aliased.id = "chat-alias";
	aliased.provider_id = " CoDeX ";
	aliased.updated_at = "2026-01-01T00:00:02.000Z";

	std::vector<ChatSession> deduped = ChatDomainService().DeduplicateChatsById({canonical, aliased});

	UAM_ASSERT_EQ(deduped.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(deduped[0].id, std::string("chat-alias"));
	UAM_ASSERT_EQ(deduped[0].provider_id, std::string(" CoDeX "));
}

UAM_TEST(ChatDomainServiceDeduplicationRemovesStaleNativeIndexesAfterReplacement)
{
	ChatSession native_original;
	native_original.id = "chat-shared";
	native_original.provider_id = "gemini-cli";
	native_original.native_session_id = "native-1";
	native_original.updated_at = "2026-01-01T00:00:01.000Z";

	ChatSession local_replacement = native_original;
	local_replacement.native_session_id.clear();
	local_replacement.updated_at = "2026-01-01T00:00:03.000Z";
	local_replacement.messages.push_back(Message{MessageRole::User, "newer local copy"});

	ChatSession native_later = native_original;
	native_later.id = "chat-native-later";
	native_later.updated_at = "2026-01-01T00:00:02.000Z";

	const std::vector<ChatSession> deduped = ChatDomainService().DeduplicateChatsById({native_original, local_replacement, native_later});

	UAM_ASSERT_EQ(deduped.size(), static_cast<std::size_t>(2));
	UAM_ASSERT(std::ranges::any_of(deduped, [](const ChatSession& chat) { return chat.id == "chat-shared" && chat.native_session_id.empty(); }));
	UAM_ASSERT(std::ranges::any_of(deduped, [](const ChatSession& chat) { return chat.id == "chat-native-later" && chat.native_session_id == "native-1"; }));
}

UAM_TEST(ChatDomainServiceDuplicateTieBreaksIgnoreBlankOptionalIds)
{
	ChatSession existing;
	existing.id = "chat-duplicate";
	existing.provider_id = "gemini-cli";
	existing.parent_chat_id = "parent-chat";
	existing.branch_root_chat_id = "branch-root";
	existing.created_at = "2026-01-01T00:00:00.000Z";
	existing.updated_at = "2026-01-01T00:00:00.000Z";

	ChatSession blank_candidate = existing;
	blank_candidate.provider_id = "  ";
	blank_candidate.parent_chat_id = "  ";
	blank_candidate.branch_root_chat_id = "  ";

	const std::vector<ChatSession> deduped = ChatDomainService().DeduplicateChatsById({existing, blank_candidate});

	UAM_ASSERT_EQ(deduped.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(deduped.front().provider_id, std::string("gemini-cli"));
	UAM_ASSERT_EQ(deduped.front().parent_chat_id, std::string("parent-chat"));
	UAM_ASSERT_EQ(deduped.front().branch_root_chat_id, std::string("branch-root"));
}

UAM_TEST(ChatDomainServiceDuplicateSelectionUsesPersistedSummaryMessageCount)
{
	ChatSession loaded;
	loaded.id = "chat-duplicate";
	loaded.messages_loaded = true;
	loaded.messages.push_back(Message{MessageRole::User, "complete transcript"});
	loaded.updated_at = "2026-01-01T00:00:01.000Z";

	ChatSession newer_summary = loaded;
	newer_summary.messages_loaded = false;
	newer_summary.messages.clear();
	newer_summary.persisted_message_count = 2;
	newer_summary.updated_at = "2026-01-01T00:00:02.000Z";

	auto deduped = ChatDomainService().DeduplicateChatsById({loaded, newer_summary});
	UAM_ASSERT_EQ(deduped.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(!deduped.front().messages_loaded);
	UAM_ASSERT_EQ(deduped.front().persisted_message_count, static_cast<std::size_t>(2));

	newer_summary.persisted_message_count = 1;
	deduped = ChatDomainService().DeduplicateChatsById({newer_summary, loaded});
	UAM_ASSERT(deduped.front().messages_loaded);
	UAM_ASSERT_EQ(deduped.front().messages.size(), static_cast<std::size_t>(1));
}

UAM_TEST(NativeChatIdentityNamesWorkspacePoliciesExplicitly)
{
	ChatSession chat;
	chat.provider_id = " CoDeX ";
	chat.folder_id = " folder-1 ";
	chat.native_session_id = " native-1 ";

	UAM_ASSERT_EQ(uam::chat_identity::NativeIdentityKeyForLocalDeduplication(chat), std::string("codex-cli|folder-1|native-1"));
	UAM_ASSERT_EQ(uam::chat_identity::NativeIdentityKeyForHistoryImport(chat), std::string("codex-cli||native-1"));

	chat.workspace_directory = " workspace/../workspace ";
	UAM_ASSERT_EQ(uam::chat_identity::NativeIdentityKeyForLocalDeduplication(chat), std::string("codex-cli|workspace|native-1"));
	UAM_ASSERT(!uam::chat_identity::NativeIdentityKeyHash(uam::chat_identity::NativeIdentityKeyForLocalDeduplication(chat)).empty());
	UAM_ASSERT_EQ(uam::chat_identity::NativeIdentityKeyHash(std::string_view("xxcodex-cli|workspace|native-1yy").substr(2, 28)), uam::chat_identity::NativeIdentityKeyHash("codex-cli|workspace|native-1"));

	chat.provider_id = " CUSTOM-PROVIDER ";
	UAM_ASSERT_EQ(uam::chat_identity::NativeIdentityKeyForLocalDeduplication(chat), std::string("custom-provider|workspace|native-1"));

	chat.provider_id.clear();
	UAM_ASSERT_EQ(uam::chat_identity::NativeIdentityKeyForLocalDeduplication(chat), std::string("|workspace|native-1"));

	chat.provider_id = uam::provider_ids::kOpenCodeCli;
	chat.execution_host_id = "gaming-ai";
	chat.workspace_directory = R"(C:\Users\example\project)";
	UAM_ASSERT_EQ(uam::chat_identity::NativeWorkspaceForHistoryImport(chat),
	              std::string("remote:gaming-ai:C:/Users/example/project"));
	UAM_ASSERT(uam::chat_identity::NativeIdentityKeyForHistoryImport(chat).find(
	               "remote:gaming-ai:") != std::string::npos);
	for (const std::string& provider : {std::string("codex-cli"), std::string("opencode-cli")})
	{
		chat.id = "remote-a";
		chat.provider_id = provider;
		chat.native_session_id = "6a6f0f3b-1a0b-4a9c-8a01-222222222222";
		ChatSession other_host = chat;
		other_host.id = "remote-b";
		other_host.execution_host_id = "other-host";
		ChatSession local = chat;
		local.id = "local-chat";
		local.execution_host_id = "local";
		ChatSession duplicate = chat;
		duplicate.id = "remote-a-duplicate";
		const std::vector<ChatSession> deduped = ChatDomainService().DeduplicateChatsById(
		    {chat, other_host, local, duplicate});
		UAM_ASSERT_EQ(deduped.size(), static_cast<std::size_t>(3));
	}

}

UAM_TEST(ChatBranchingReparentsDeletedBranchChildren)
{
	ChatSession root;
	root.id = "chat-root";

	ChatSession deleted;
	deleted.id = "chat-deleted";
	deleted.parent_chat_id = root.id;
	deleted.branch_from_message_index = 2;

	ChatSession child;
	child.id = "chat-child";
	child.parent_chat_id = deleted.id;
	child.branch_from_message_index = 4;

	std::vector<ChatSession> chats{root, deleted, child};
	ChatBranching::ReparentChildrenAfterDelete(chats, std::string_view("xx chat-deleted yy").substr(2, 14));

	UAM_ASSERT_EQ(chats[2].parent_chat_id, root.id);
	UAM_ASSERT_EQ(chats[2].branch_root_chat_id, root.id);
	UAM_ASSERT_EQ(chats[2].branch_from_message_index, 4);
}

UAM_TEST(ChatBranchingTrimsStoredRelationshipIdsBeforeValidation)
{
	ChatSession root;
	root.id = "chat-root";

	ChatSession child;
	child.id = "chat-child";
	child.parent_chat_id = " chat-root ";
	child.branch_root_chat_id = " stale-root ";
	child.branch_from_message_index = -1;

	std::vector<ChatSession> chats{root, child};
	ChatBranching::Normalize(chats);

	UAM_ASSERT_EQ(chats[1].parent_chat_id, root.id);
	UAM_ASSERT_EQ(chats[1].branch_root_chat_id, root.id);
	UAM_ASSERT_EQ(chats[1].branch_from_message_index, 0);
}

UAM_TEST(ChatBranchingNormalizeBreaksParentCycles)
{
	ChatSession first;
	first.id = "chat-first";
	first.parent_chat_id = "chat-second";

	ChatSession second;
	second.id = "chat-second";
	second.parent_chat_id = "chat-first";

	std::vector<ChatSession> chats{first, second};
	ChatBranching::Normalize(chats);

	UAM_ASSERT(chats[0].parent_chat_id.empty() || chats[1].parent_chat_id.empty());
}

UAM_TEST(ChatDomainServiceFindChatByIdReturnsMutableAndConstPointers)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = " chat-1 ";
	chat.title = "Original";
	app.chats.push_back(chat);

	ChatSession* mutable_chat = ChatDomainService().FindChatById(app, " chat-1 ");
	UAM_ASSERT(mutable_chat != nullptr);
	mutable_chat->title = "Updated";
	UAM_ASSERT_EQ(ChatDomainService().FindChatIndexById(app, "\tchat-1\n"), 0);

	const uam::AppState& const_app = app;
	const ChatSession* const_chat = ChatDomainService().FindChatById(const_app, " chat-1 ");
	UAM_ASSERT(const_chat != nullptr);
	UAM_ASSERT_EQ(const_chat->title, std::string("Updated"));
	UAM_ASSERT(ChatDomainService().FindChatById(app, "missing") == nullptr);
	UAM_ASSERT(ChatDomainService().FindChatById(const_app, "missing") == nullptr);
}

UAM_TEST(ChatDomainServiceFindChatByNativeSessionIdReturnsMutableAndConstPointers)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-1";
	chat.native_session_id = " native-session-1 ";
	chat.title = "Original";
	app.chats.push_back(chat);

	ChatSession* mutable_chat = ChatDomainService().FindChatByNativeSessionId(app, " native-session-1 ");
	UAM_ASSERT(mutable_chat != nullptr);
	mutable_chat->title = "Updated";

	const uam::AppState& const_app = app;
	const ChatSession* const_chat = ChatDomainService().FindChatByNativeSessionId(const_app, " native-session-1 ");
	UAM_ASSERT(const_chat != nullptr);
	UAM_ASSERT_EQ(const_chat->title, std::string("Updated"));
	UAM_ASSERT(ChatDomainService().FindChatByNativeSessionId(app, "missing") == nullptr);
	UAM_ASSERT(ChatDomainService().FindChatByNativeSessionId(const_app, "missing") == nullptr);
}

UAM_TEST(ChatDomainServiceFindChatByNativeSessionIdPrefersResolvedRuntimeMapping)
{
	uam::AppState app;

	ChatSession stale_chat;
	stale_chat.id = "chat-stale";
	stale_chat.native_session_id = "other-session";
	stale_chat.title = "Stale";
	app.chats.push_back(stale_chat);

	ChatSession resolved_chat;
	resolved_chat.id = "chat-resolved";
	resolved_chat.native_session_id = "other-session";
	resolved_chat.title = "Resolved";
	app.chats.push_back(resolved_chat);

	app.resolved_native_sessions_by_chat_id["chat-resolved"] = "resolved-session";

	ChatSession* mutable_chat = ChatDomainService().FindChatByNativeSessionId(app, " resolved-session ");
	UAM_ASSERT(mutable_chat != nullptr);
	UAM_ASSERT_EQ(mutable_chat->id, std::string("chat-resolved"));

	const uam::AppState& const_app = app;
	const ChatSession* const_chat = ChatDomainService().FindChatByNativeSessionId(const_app, "resolved-session");
	UAM_ASSERT(const_chat != nullptr);
	UAM_ASSERT_EQ(const_chat->id, std::string("chat-resolved"));
}

UAM_TEST(ChatDomainServiceFindChatByNativeSessionIdSkipsStaleResolvedEntries)
{
	uam::AppState app;

	ChatSession stale_chat;
	stale_chat.id = "chat-stale";
	stale_chat.native_session_id = "resolved-session";
	stale_chat.title = "Stale";
	app.resolved_native_sessions_by_chat_id[stale_chat.id] = "resolved-session";

	ChatSession live_chat;
	live_chat.id = "chat-live";
	live_chat.native_session_id = "resolved-session";
	live_chat.title = "Live";
	app.chats.push_back(live_chat);

	ChatSession* mutable_chat = ChatDomainService().FindChatByNativeSessionId(app, "resolved-session");
	UAM_ASSERT(mutable_chat != nullptr);
	UAM_ASSERT_EQ(mutable_chat->id, std::string("chat-live"));

	const uam::AppState& const_app = app;
	const ChatSession* const_chat = ChatDomainService().FindChatByNativeSessionId(const_app, "resolved-session");
	UAM_ASSERT(const_chat != nullptr);
	UAM_ASSERT_EQ(const_chat->id, std::string("chat-live"));
}

UAM_TEST(ChatDomainServiceFindChatByNativeSessionIdPrefersNewerEqualPriorityMatch)
{
	uam::AppState app;

	ChatSession older_chat;
	older_chat.id = "chat-older";
	older_chat.provider_id = uam::provider_ids::kOpenCodeCli;
	older_chat.native_session_id = "resolved-session";
	older_chat.updated_at = "2026-01-01T00:00:01.000Z";
	app.chats.push_back(older_chat);
	app.resolved_native_sessions_by_chat_id[older_chat.id] = "resolved-session";

	ChatSession newer_chat = older_chat;
	newer_chat.id = "chat-newer";
	newer_chat.updated_at = "2026-01-01T00:00:02.000Z";
	app.chats.push_back(newer_chat);
	app.resolved_native_sessions_by_chat_id[newer_chat.id] = "resolved-session";

	ChatSession* mutable_chat = ChatDomainService().FindChatByNativeSessionId(app, "resolved-session");
	UAM_ASSERT(mutable_chat != nullptr);
	UAM_ASSERT_EQ(mutable_chat->id, std::string("chat-newer"));

	const uam::AppState& const_app = app;
	const ChatSession* const_chat = ChatDomainService().FindChatByNativeSessionId(const_app, "resolved-session");
	UAM_ASSERT(const_chat != nullptr);
	UAM_ASSERT_EQ(const_chat->id, std::string("chat-newer"));
}

UAM_TEST(ChatDomainServiceFindChatByNativeSessionIdPrefersNewerRawChatOverStaleResolvedMapping)
{
	uam::AppState app;

	ChatSession stale_resolved_chat;
	stale_resolved_chat.id = "chat-stale";
	stale_resolved_chat.native_session_id = "stale-session";
	stale_resolved_chat.updated_at = "2026-01-01T00:00:01.000Z";
	app.chats.push_back(stale_resolved_chat);
	app.resolved_native_sessions_by_chat_id[stale_resolved_chat.id] = "resolved-session";

	ChatSession live_chat;
	live_chat.id = "chat-live";
	live_chat.native_session_id = "resolved-session";
	live_chat.updated_at = "2026-01-01T00:00:02.000Z";
	app.chats.push_back(live_chat);

	ChatSession* mutable_chat = ChatDomainService().FindChatByNativeSessionId(app, "resolved-session");
	UAM_ASSERT(mutable_chat != nullptr);
	UAM_ASSERT_EQ(mutable_chat->id, std::string("chat-live"));

	const uam::AppState& const_app = app;
	const ChatSession* const_chat = ChatDomainService().FindChatByNativeSessionId(const_app, "resolved-session");
	UAM_ASSERT(const_chat != nullptr);
	UAM_ASSERT_EQ(const_chat->id, std::string("chat-live"));
}

UAM_TEST(ChatDomainServiceFindFolderByIdTrimsRequestedFolderId)
{
	uam::AppState app;
	ChatFolder folder;
	folder.id = " folder-1 ";
	folder.title = "Original";
	app.folders.push_back(folder);

	ChatFolder* mutable_folder = ChatDomainService().FindFolderById(app, " folder-1 ");
	UAM_ASSERT(mutable_folder != nullptr);
	mutable_folder->title = "Updated";
	UAM_ASSERT_EQ(ChatDomainService().FindFolderIndexById(app, "\tfolder-1\n"), 0);

	const uam::AppState& const_app = app;
	const ChatFolder* const_folder = ChatDomainService().FindFolderById(const_app, " folder-1 ");
	UAM_ASSERT(const_folder != nullptr);
	UAM_ASSERT_EQ(const_folder->title, std::string("Updated"));
	UAM_ASSERT(ChatDomainService().FindFolderById(app, "missing") == nullptr);
	UAM_ASSERT(ChatDomainService().FindFolderById(const_app, "missing") == nullptr);
}

UAM_TEST(ChatDomainServiceNormalizesNewChatFolderSelection)
{
	uam::AppState app;

	ChatFolder folder;
	folder.id = "folder-1";
	app.folders.push_back(folder);

	ChatSession chat;
	chat.id = "chat-1";
	chat.folder_id = " " + folder.id + " ";
	app.chats.push_back(chat);

	app.new_chat_folder_id = " folder-1 ";
	ChatDomainService().EnsureNewChatFolderSelection(app);
	UAM_ASSERT_EQ(app.new_chat_folder_id, std::string("folder-1"));
	UAM_ASSERT_EQ(ChatDomainService().FolderForNewChat(app), std::string("folder-1"));
	UAM_ASSERT_EQ(ChatDomainService().CountChatsInFolder(app, "\tfolder-1\n"), 1);

	app.new_chat_folder_id = " missing-folder ";
	ChatDomainService().EnsureNewChatFolderSelection(app);
	UAM_ASSERT(app.new_chat_folder_id.empty());
}

UAM_TEST(ChatDomainServiceSelectedChatIdHandlesInvalidSelection)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = " chat-1 ";
	chat.provider_id = " codex-cli ";
	app.chats.push_back(chat);

	app.selected_chat_index = 0;
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), std::string("chat-1"));
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatProviderId(app), std::string("codex-cli"));

	app.selected_chat_index = -1;
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), std::string(""));
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatProviderId(app), std::string(""));

	app.selected_chat_index = 4;
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), std::string(""));
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatProviderId(app), std::string(""));
}

UAM_TEST(ChatDomainServiceSetSelectedChatIndexOrNearestClampsAndRefreshes)
{
	uam::AppState app;
	app.settings.remember_last_chat = true;

	ChatSession first;
	first.id = "chat-first";
	ChatSession second;
	second.id = " chat-second ";
	app.chats.push_back(first);
	app.chats.push_back(second);

	ChatDomainService().SetSelectedChatIndexOrNearest(app, 4);
	UAM_ASSERT_EQ(app.selected_chat_index, 1);
	UAM_ASSERT_EQ(app.settings.last_selected_chat_id, std::string("chat-second"));

	ChatDomainService().SetSelectedChatIndexOrNearest(app, -2);
	UAM_ASSERT_EQ(app.selected_chat_index, 0);
	UAM_ASSERT_EQ(app.settings.last_selected_chat_id, std::string("chat-first"));

	app.chats.clear();
	ChatDomainService().SetSelectedChatIndexOrNearest(app, 0);
	UAM_ASSERT_EQ(app.selected_chat_index, -1);
	UAM_ASSERT_EQ(app.settings.last_selected_chat_id, std::string(""));
}

UAM_TEST(ChatAttentionRequiresMatchingInteractionAndSurvivesRestart)
{
	TempDir temp("uam-attention");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kClaudeCli);
	chat.id = "attention-chat";
	app.chats.push_back(chat);
	app.selected_chat_index = 0;
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, app.chats.front()));
	ChatDomainService domain;
	domain.MarkChatNeedsAttention(app, chat.id);
	const std::string first = app.chats.front().attention_revision;
	UAM_ASSERT(!first.empty());
	domain.SelectChatById(app, chat.id);
	UAM_ASSERT(app.chats_with_unseen_updates.contains(chat.id));
	UAM_ASSERT_EQ(app.chats.front().attention_revision, first);
	std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT(uam::io::ReadTextFile(AppPaths::UamChatSummaryFilePath(app.data_root, chat.id)).find(first) != std::string::npos);
	UAM_ASSERT_EQ(loaded->attention_revision, first);
	UAM_ASSERT(domain.SetChatSettled(app, chat.id, true));
	const std::string settled_at = app.chats.front().settled_at;
	UAM_ASSERT(!settled_at.empty());
	UAM_ASSERT(settled_at >= app.chats.front().interaction_at);
	loaded = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT_EQ(loaded->settled_at, settled_at);
	UAM_ASSERT_EQ(loaded->attention_revision, first);
	UAM_ASSERT(domain.SetChatSettled(app, chat.id, false));
	loaded = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT(loaded->settled_at.empty());
	domain.MarkChatNeedsAttention(app, chat.id);
	const std::string second = app.chats.front().attention_revision;
	UAM_ASSERT(first != second);
	UAM_ASSERT(domain.AcknowledgeChatAttention(app, chat.id, first));
	UAM_ASSERT_EQ(app.chats.front().attention_revision, second);
	UAM_ASSERT(domain.AcknowledgeChatAttention(app, chat.id, second));
	UAM_ASSERT(app.chats.front().attention_revision.empty());
	UAM_ASSERT(!app.chats_with_unseen_updates.contains(chat.id));
	loaded = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT(loaded->attention_revision.empty());
}

UAM_TEST(LegacyChatAttentionCanBeAcknowledgedWithoutClearingANewerCompletion)
{
	TempDir temp("uam-legacy-attention");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "legacy-attention";
	chat.updated_at = "2026-10-01T11:00:00Z";
	chat.last_opened_at = "2026-10-01T10:00:00Z";
	app.chats.push_back(chat);
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	ChatDomainService domain;
	const std::string legacy = domain.AttentionRevision(app, app.chats.front());
	UAM_ASSERT(!legacy.empty());
	UAM_ASSERT(domain.AcknowledgeChatAttention(app, chat.id, legacy));
	UAM_ASSERT(domain.AttentionRevision(app, app.chats.front()).empty());
	domain.MarkChatNeedsAttention(app, chat.id);
	const std::string fresh = app.chats.front().attention_revision;
	UAM_ASSERT(domain.AcknowledgeChatAttention(app, chat.id, legacy));
	UAM_ASSERT_EQ(app.chats.front().attention_revision, fresh);
}

#if defined(__APPLE__)
UAM_TEST(MacOwnedProviderGroupIsCleanedBeforeExitedLeaderIsReaped)
{
	TempDir temp("uam-owned-group");
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	uam::platform::StdioProcessPlatformFields owned;
	uam::platform::StdioProcessPlatformFields unrelated;
	std::string error;
	const fs::path marker = temp.root / "descendant-survived";
	const fs::path unrelated_marker = temp.root / "unrelated-survived";
	UAM_ASSERT(service.StartStdioProcess(unrelated, temp.root, {"/bin/sh", "-c", "sleep 0.4; touch '" + unrelated_marker.string() + "'; sleep 10"}, &error));
	UAM_ASSERT(service.StartStdioProcess(owned, temp.root, {"/bin/sh", "-c", "(/bin/sh -c \"trap '' TERM; sleep 0.3; touch '" + marker.string() + "'; sleep 10\" &) ; exit 0"}, &error));
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!service.PollStdioProcessExited(owned) && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	UAM_ASSERT(owned.child_pid <= 0);
	const auto unrelated_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!fs::exists(unrelated_marker) && std::chrono::steady_clock::now() < unrelated_deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	service.StopStdioProcess(owned, true);
	const bool unrelated_running = !service.PollStdioProcessExited(unrelated);
	service.StopStdioProcess(unrelated, true);
	UAM_ASSERT(!fs::exists(marker));
	UAM_ASSERT(fs::exists(unrelated_marker));
	UAM_ASSERT(unrelated_running);
}
#endif

UAM_TEST(ChatDomainServiceSelectRememberedOrFirstChatTrimsPersistedId)
{
	uam::AppState app;
	app.settings.remember_last_chat = true;
	app.settings.last_selected_chat_id = " chat-second ";

	ChatSession first;
	first.id = "chat-first";
	ChatSession second;
	second.id = " chat-second ";
	app.chats.push_back(first);
	app.chats.push_back(second);

	ChatDomainService().SelectRememberedOrFirstChat(app);
	UAM_ASSERT_EQ(app.selected_chat_index, 1);
	UAM_ASSERT_EQ(app.settings.last_selected_chat_id, std::string("chat-second"));
}

UAM_TEST(ChatDomainServiceSelectChatByIdTrimsRequestedChatId)
{
	uam::AppState app;
	app.settings.remember_last_chat = true;

	ChatSession first;
	first.id = "chat-first";
	ChatSession second;
	second.id = " chat-second ";
	app.chats.push_back(first);
	app.chats.push_back(second);
	app.selected_chat_index = 0;
	app.composer_text = "draft";
	app.chats_with_unseen_updates.insert("chat-second");

	ChatDomainService().SelectChatById(app, " chat-second ");
	UAM_ASSERT_EQ(app.selected_chat_index, 1);
	UAM_ASSERT_EQ(app.settings.last_selected_chat_id, std::string("chat-second"));
	UAM_ASSERT(app.composer_text.empty());
	UAM_ASSERT(app.chats_with_unseen_updates.contains("chat-second"));

	app.composer_text = "keep draft";
	ChatDomainService().SelectChatById(app, " chat-second ");
	UAM_ASSERT_EQ(app.selected_chat_index, 1);
	UAM_ASSERT_EQ(app.composer_text, std::string("keep draft"));

	ChatDomainService().SelectChatById(app, "");
	UAM_ASSERT_EQ(app.selected_chat_index, -1);
	UAM_ASSERT_EQ(app.settings.last_selected_chat_id, std::string(""));
}

UAM_TEST(FinalizeChatSyncSelectionKeepsClosedSelectionEmpty)
{
	TempDir temp("uam-sync-selection-closed");
	uam::AppState app;
	app.data_root = temp.root;
	app.settings.remember_last_chat = true;
	for (const char* id : {"chat-first", "chat-terminal"})
	{
		ChatSession chat;
		chat.id = id;
		chat.messages_loaded = true;
		app.chats.push_back(chat);
	}

	// Closing or deleting the open chat leaves no selection. A later terminal
	// history sync (its own chat, another native ID, or a removed chat) must not
	// reopen anything.
	for (const char* preferred : {"chat-terminal", "native-session-id", "chat-deleted", ""})
	{
		ChatDomainService().SelectChatById(app, "");
		UAM_ASSERT(uam::FinalizeChatSyncSelection(app, "", preferred, false));
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), std::string());
		UAM_ASSERT(uam::FinalizeChatSyncSelection(app, "", preferred, true));
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), std::string());
		UAM_ASSERT_EQ(app.settings.last_selected_chat_id, std::string());
	}

	// An open chat still follows its terminal's import target as before.
	ChatDomainService().SelectChatById(app, "chat-first");
	UAM_ASSERT(uam::FinalizeChatSyncSelection(app, "chat-first", "chat-terminal", false));
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), std::string("chat-terminal"));
}

UAM_TEST(FinalizeChatSyncSelectionPrunesReferencesAndReportsHydrationFailure)
{
	TempDir temp("uam-sync-selection-hydration");
	uam::AppState app;
	app.data_root = temp.root;
	app.settings.remember_last_chat = true;

	ChatSession chat;
	chat.id = "chat-present";
	app.chats.push_back(chat);
	app.selected_chat_index = 0;

	app.chats_with_unseen_updates.insert("chat-missing");
	app.collapsed_branch_chat_ids.insert("chat-missing");
	app.filtered_chat_ids.insert("chat-missing");
	app.filtered_chat_ids.insert(" chat-present ");

	uam::FinalizeChatSyncSelection(app, "chat-missing", "", true);

	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), std::string("chat-present"));
	UAM_ASSERT(app.chats_with_unseen_updates.empty());
	UAM_ASSERT(app.collapsed_branch_chat_ids.empty());
	UAM_ASSERT_EQ(app.filtered_chat_ids.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(app.filtered_chat_ids.contains("chat-present"));
	UAM_ASSERT_EQ(app.settings.last_selected_chat_id, std::string("chat-present"));
	app.chats.front().messages_loaded = false;
	UAM_ASSERT(!uam::FinalizeChatSyncSelection(app, chat.id, "missing-chat", true));
	UAM_ASSERT(uam::strings::Contains(app.status_line, "could not load chat history"));
	chat.messages.push_back(Message{MessageRole::Assistant, "Recovered history"});
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	UAM_ASSERT(uam::SyncChatsFromLoadedNative(app, {}, " missing-chat ", true));
	UAM_ASSERT(app.status_line.empty());
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), chat.id);
	UAM_ASSERT_EQ(app.chats.front().messages.front().content, std::string("Recovered history"));
}

UAM_TEST(SyncChatsFromLoadedNativeTrimsPreferredNativeSessionId)
{
	TempDir temp("uam-sync-loaded-native-trim");
	uam::AppState app;
	app.data_root = temp.root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();

	ChatSession target;
	target.id = "target-chat";
	target.provider_id = "gemini-cli";
	target.native_session_id = "target-native";
	target.title = "Target";
	target.messages.push_back(Message{MessageRole::Assistant, "Target history"});

	ChatSession other;
	other.id = "other-chat";
	other.provider_id = "gemini-cli";
	other.native_session_id = "other-native";
	other.title = "Other";

	UAM_ASSERT(uam::SyncChatsFromLoadedNative(app, {target, other}, " target-native "));

	const std::vector<ChatSession> saved = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(saved.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(saved.front().id, std::string("target-chat"));
	UAM_ASSERT_EQ(saved.front().native_session_id, std::string("target-native"));
	// No chat was open, so the background import must not open one.
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), std::string());
	app.chats.clear();
	app.selected_chat_index = -1;
	UAM_ASSERT(uam::SyncChatsFromLoadedNative(app, {}, "missing-chat"));
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), std::string());
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.front().id, std::string("target-chat"));
	UAM_ASSERT(ChatRepository::HydrateChatMessages(app.data_root, app.chats.front()));
	UAM_ASSERT_EQ(app.chats.front().messages.front().content, std::string("Target history"));
}

UAM_TEST(NativeTerminalRefreshPreservesConcurrentAcpAndDeferredChatState)
{
	for (const bool transport_running : {false, true})
	{
		TempDir temp("uam-terminal-sync-live-state");
		uam::AppState app;
		app.data_root = temp.root;
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		ChatSession active;
		active.id = "active-acp";
		active.provider_id = uam::provider_ids::kOpenCodeCli;
		active.messages.push_back(Message{MessageRole::Assistant, "Saved stream"});
		ChatSession deferred = active;
		deferred.id = "deferred-save";
		ChatSession checkpoint = active;
		checkpoint.id = "checkpoint-commit";
		ChatSession selected = active;
		selected.id = "selected-idle-chat";
		ChatSession cold = active;
		cold.id = "cold-history";
		cold.messages.assign(1000, Message{MessageRole::Assistant, std::string(1000, 'x')});
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, active));
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, deferred));
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, selected));
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, checkpoint));
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, cold));
		active.messages.back().content = "Unsaved stream";
		deferred.messages.back().content = "Unsaved completed text";
		checkpoint.messages.back().content = "Unsaved checkpoint text";
		app.chats = {active, deferred, selected, cold, checkpoint};
		app.selected_chat_index = 2;
		app.pending_chat_save_at_by_chat_id[deferred.id] = uam::GetAppTimeSeconds() + 60.0;
		std::unique_ptr<uam::AcpSessionState> session = std::make_unique<uam::AcpSessionState>();
		session->chat_id = active.id;
		session->provider_id = active.provider_id;
		session->running = transport_running;
		session->processing = true;
		session->reconnect_pending = !transport_running;
		session->turn_assistant_message_index = 0;
		session->current_assistant_message_index = 0;
		app.acp_sessions.push_back(std::move(session));
		std::unique_ptr<uam::AcpSessionState> checkpoint_session = std::make_unique<uam::AcpSessionState>();
		checkpoint_session->chat_id = checkpoint.id;
		checkpoint_session->provider_id = checkpoint.provider_id;
		checkpoint_session->running = true;
		checkpoint_session->turn_checkpoint_commit_pending = true;
		app.acp_sessions.push_back(std::move(checkpoint_session));
		UAM_ASSERT(!app.acp_sessions.back()->processing);
		UAM_ASSERT(!app.pending_chat_save_at_by_chat_id.contains(checkpoint.id));

		ChatSession native;
		native.id = "terminal-chat";
		native.provider_id = uam::provider_ids::kGeminiCli;
		native.native_session_id = "terminal-native";
		native.messages.push_back(Message{MessageRole::Assistant, "CLI result"});
		UAM_ASSERT(uam::SyncChatsFromLoadedNative(app, {native}, native.id, true));
		const ChatSession* cold_summary = ChatDomainService().FindChatById(app, cold.id);
		// Receipt-scoped adoption preserves unrelated live rows, including their
		// loaded cache; only the published native owner may be replaced from disk.
		UAM_ASSERT(cold_summary != nullptr && cold_summary->messages_loaded);
		UAM_ASSERT_EQ(cold_summary->messages, cold.messages);
		UAM_ASSERT_EQ(cold_summary->persisted_message_count, cold.persisted_message_count);
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), selected.id);
		UAM_ASSERT_EQ(ChatDomainService().SelectedChat(app)->messages.front().content, std::string("Saved stream"));
		ChatSession* published_native = ChatDomainService().FindChatById(app, native.id);
		UAM_ASSERT(published_native != nullptr);
		// A newly published, unselected row can be lazy; its saved transcript must hydrate.
		UAM_ASSERT(ChatRepository::HydrateChatMessages(app.data_root, *published_native));
		UAM_ASSERT_EQ(published_native->messages.size(), std::size_t(1));
		UAM_ASSERT_EQ(published_native->messages.front().content, std::string("CLI result"));
		for (const bool reload_sidebar : {false, true})
		{
			if (reload_sidebar) ChatHistorySyncService().LoadSidebarChats(app);
			const ChatSession* preserved = ChatDomainService().FindChatById(app, active.id);
			UAM_ASSERT(preserved != nullptr && preserved->messages.size() == 1);
			UAM_ASSERT_EQ(preserved->messages.back().content, std::string("Unsaved stream"));
			preserved = ChatDomainService().FindChatById(app, deferred.id);
			UAM_ASSERT(preserved != nullptr && preserved->messages.size() == 1);
			UAM_ASSERT_EQ(preserved->messages.back().content, std::string("Unsaved completed text"));
			preserved = ChatDomainService().FindChatById(app, checkpoint.id);
			UAM_ASSERT(preserved != nullptr && preserved->messages_loaded && preserved->messages.size() == 1);
			UAM_ASSERT_EQ(preserved->messages.back().content, std::string("Unsaved checkpoint text"));
		}
		ChatSession* continued = ChatDomainService().FindChatById(app, active.id);
		UAM_ASSERT(uam::ProcessAcpLineForTests(app, *app.acp_sessions.front(), *continued,
		    R"({"jsonrpc":"2.0","method":"session/update","params":{"update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":" plus next chunk"}}}})"));
		uam::FlushPendingChatSaves(app, true);
		const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(app.data_root, active.id);
		UAM_ASSERT(saved && saved->messages.size() == 1);
		UAM_ASSERT_EQ(saved->messages.back().content, std::string("Unsaved stream plus next chunk"));
		UAM_ASSERT(ChatRepository::LoadLocalChat(app.data_root, native.id).has_value());
	}
}

UAM_TEST(ChatDomainServiceAnalyticsAutoTitlesOnlyPlaceholderNewSession)
{
	ChatSession placeholder_chat;
	placeholder_chat.title = "New Session";

	ChatDomainService::MessageAnalytics analytics;
	analytics.provider = "codex-cli";
	analytics.input_tokens = 100;
	analytics.output_chars = 40;
	analytics.time_to_first_token_ms = 5;
	analytics.processing_time_ms = 20;
	ChatDomainService().AddMessageWithAnalytics(placeholder_chat, MessageRole::User, "Summarize the incident review action items", analytics);
	UAM_ASSERT_EQ(placeholder_chat.title, std::string("Summarize the incident review action items"));
	UAM_ASSERT_EQ(placeholder_chat.messages.back().tokens_input, 100);
	UAM_ASSERT_EQ(placeholder_chat.messages.back().tokens_output, 10);

	ChatSession custom_title_chat;
	custom_title_chat.title = "Incident review";

	ChatDomainService().AddMessageWithAnalytics(custom_title_chat, MessageRole::User, "Do not overwrite", analytics);
	UAM_ASSERT_EQ(custom_title_chat.title, std::string("Incident review"));

	ChatDomainService::MessageAnalytics negative_analytics;
	negative_analytics.input_tokens = -1;
	negative_analytics.output_chars = -4;
	negative_analytics.time_to_first_token_ms = -5;
	negative_analytics.processing_time_ms = -6;
	ChatSession bounded_chat;
	ChatDomainService().AddMessageWithAnalytics(bounded_chat, MessageRole::Assistant, "bounded", negative_analytics);
	UAM_ASSERT_EQ(bounded_chat.messages.back().tokens_input, 0);
	UAM_ASSERT_EQ(bounded_chat.messages.back().tokens_output, 0);
	UAM_ASSERT_EQ(bounded_chat.messages.back().time_to_first_token_ms, 0);
	UAM_ASSERT_EQ(bounded_chat.messages.back().processing_time_ms, 0);
	UAM_ASSERT_EQ(bounded_chat.messages.back().estimated_cost_usd, 0.0);
}

UAM_TEST(ChatRepositoryPersistsPinnedFlag)
{
	TempDir temp("uam-chat-pinned");
	ChatSession chat;
	chat.id = "chat-pinned";
	chat.provider_id = "gemini-cli";
	chat.title = "Pinned";
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = "2026-01-01T00:00:01.000Z";
	chat.pinned = true;

	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));

	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(loaded.front().pinned);

	const nlohmann::json persisted = nlohmann::json::parse(ReadFile(AppPaths::UamChatFilePath(temp.root, chat.id)));
	UAM_ASSERT(persisted.value("pinned", false));
}

UAM_TEST(AssistantModelSnapshotSurvivesModelChangesAndPersistence)
{
	TempDir temp("uam-model-snapshot");
	ChatSession chat;
	chat.id = "model-history";
	chat.provider_id = "codex-cli";
	chat.messages_loaded = true;
	uam::AcpSessionState session;
	session.provider_id = "codex-cli";
	session.current_model_id = "model-first";
	Message& response = uam::acp_detail::EnsureAssistantMessage(chat, session);
	response.content = "Saved response";
	UAM_ASSERT_EQ(response.model_id, std::string("model-first"));
	session.current_model_id = "model-second";
	UAM_ASSERT_EQ(uam::acp_detail::EnsureAssistantMessage(chat, session).model_id, std::string("model-first"));
	chat.model_id = "model-second";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT_EQ(loaded->messages.front().model_id, std::string("model-first"));
	const nlohmann::json state = uam::StateSerializer::SerializeSession(*loaded);
	UAM_ASSERT_EQ(state["messages"][0].value("modelId", ""), std::string("model-first"));
}

UAM_TEST(ChatRepositoryRoundTripsGoalAccountingAboveIntMax)
{
	TempDir temp("uam-chat-goal-int64");
	ChatSession chat;
	chat.id = "chat-goal-int64";
	chat.provider_id = "opencode-cli";
	chat.title = "Large goal accounting";
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = "2026-01-01T00:00:01.000Z";
	Goal goal;
	goal.id = "goal-large";
	goal.objective = "Keep the full durable accounting value.";
	goal.token_budget = 5'000'000'000LL;
	goal.tokens_used = 4'000'000'000LL;
	chat.goals.push_back(goal);

	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().goals.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().goals.front().token_budget, 5'000'000'000LL);
	UAM_ASSERT_EQ(loaded.front().goals.front().tokens_used, 4'000'000'000LL);
}

UAM_TEST(ChatRepositoryPersistsCommandSafetyTierAndDefaultsToOff)
{
	TempDir temp("uam-chat-command-safety");
	ChatSession chat;
	chat.id = "chat-safety";
	chat.provider_id = "codex-cli";
	chat.title = "Safety";
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = "2026-01-01T00:00:01.000Z";
	chat.command_safety_tier = "aiReview";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));

	std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.front().command_safety_tier, std::string("aiReview"));
	chat.command_safety_tier = "off";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.front().command_safety_tier, std::string("off"));
	chat.command_safety_tier = "yolo";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.front().command_safety_tier, std::string("yolo"));
	chat.command_safety_tier = "acceptEdits";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.front().command_safety_tier, std::string("acceptEdits"));

	nlohmann::json persisted = nlohmann::json::parse(ReadFile(AppPaths::UamChatFilePath(temp.root, chat.id)));
	persisted.erase("commandSafetyTier");
	UAM_ASSERT(uam::io::WriteTextFile(AppPaths::UamChatFilePath(temp.root, chat.id), persisted.dump(2)));
	loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.front().command_safety_tier, std::string("off"));
}

UAM_TEST(ChatRepositoryLoadsChatsNewestUpdatedFirst)
{
	TempDir temp("uam-chat-load-order");

	ChatSession older;
	older.id = "chat-older";
	older.provider_id = "gemini-cli";
	older.title = "Older";
	older.created_at = "2026-01-01T00:00:00.000Z";
	older.updated_at = "2026-01-01T00:00:01.000Z";

	ChatSession newer = older;
	newer.id = "chat-newer";
	newer.title = "Newer";
	newer.updated_at = "2026-01-01T00:00:03.000Z";

	UAM_ASSERT(ChatRepository::SaveChat(temp.root, older));
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, newer));

	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(loaded[0].id, std::string("chat-newer"));
	UAM_ASSERT_EQ(loaded[1].id, std::string("chat-older"));
}

UAM_TEST(ChatRepositoryWritesLightweightSidebarSummaries)
{
	TempDir temp("uam-chat-summary-cache");
	ChatSession chat;
	chat.id = "chat-summary-cache";
	chat.provider_id = "opencode-cli";
	chat.title = "Cached";
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = "2026-01-01T00:00:01.000Z";
	chat.messages.push_back(Message{MessageRole::Assistant, std::string(1024 * 1024, 'x')});
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));

	const fs::path summary_path = AppPaths::UamChatSummaryFilePath(temp.root, chat.id);
	UAM_ASSERT(uam::paths::PathExistsNoThrow(summary_path));
	UAM_ASSERT(fs::file_size(summary_path) < 16 * 1024);

	const std::vector<ChatSession> summaries = ChatRepository::LoadLocalChatSummaries(temp.root);
	UAM_ASSERT_EQ(summaries.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(summaries.front().id, chat.id);
	UAM_ASSERT(!summaries.front().messages_loaded);
	UAM_ASSERT_EQ(summaries.front().persisted_message_count, static_cast<std::size_t>(1));
	UAM_ASSERT(summaries.front().messages.empty());
}

UAM_TEST(ChatRepositoryLoadsOneChatWithoutScanningSiblingFiles)
{
	TempDir temp("uam-chat-single-load");
	ChatSession chat;
	chat.id = "chat-single-load";
	chat.title = "Target";
	chat.messages.push_back(Message{MessageRole::Assistant, "Loaded"});
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	UAM_ASSERT(uam::io::WriteTextFile(AppPaths::UamChatFilePath(temp.root, "broken-sibling"), "{not json"));

	std::string warning;
	const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(temp.root, chat.id, true, &warning);

	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT(warning.empty());
	UAM_ASSERT_EQ(loaded->messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded->messages.front().content, std::string("Loaded"));
}

UAM_TEST(ChatRepositoryPersistsGitWorktreeMetadata)
{
	TempDir temp("uam-chat-worktree-metadata");
	ChatSession chat;
	chat.id = "chat-worktree";
	chat.provider_id = "codex-cli";
	chat.title = "Worktree";
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = "2026-01-01T00:00:01.000Z";
	chat.workspace_directory = (temp.root / "repo").string();
	chat.workspace_isolation_kind = "gitWorktree";
	chat.workspace_source_directory = (temp.root / "repo").string();
	chat.workspace_base_ref = "abc123";
	chat.workspace_branch_name = "uam/chat-worktree";
	chat.workspace_worktree_directory = (temp.root / "worktree").string();

	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));

	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().workspace_isolation_kind, std::string("gitWorktree"));
	UAM_ASSERT_EQ(loaded.front().workspace_source_directory, chat.workspace_source_directory);
	UAM_ASSERT_EQ(loaded.front().workspace_base_ref, std::string("abc123"));
	UAM_ASSERT_EQ(loaded.front().workspace_branch_name, std::string("uam/chat-worktree"));
	UAM_ASSERT_EQ(loaded.front().workspace_worktree_directory, chat.workspace_worktree_directory);

	const nlohmann::json persisted = nlohmann::json::parse(ReadFile(AppPaths::UamChatFilePath(temp.root, chat.id)));
	UAM_ASSERT_EQ(persisted.value("workspace_isolation_kind", ""), std::string("gitWorktree"));
	UAM_ASSERT_EQ(persisted.value("workspace_worktree_directory", ""), chat.workspace_worktree_directory);
}

UAM_TEST(ResolveWorkspaceRootPathPrefersGitWorktreeDirectory)
{
	TempDir temp("uam-worktree-resolve");
	uam::AppState app;
	app.data_root = temp.root / "data";

	ChatFolder folder;
	folder.id = " folder ";
	folder.directory = " " + (temp.root / "repo").string() + " ";
	app.folders.push_back(folder);
	UAM_ASSERT_EQ(uam::paths::FindWorkspaceFolderById(app, std::string_view("xx folder yy").substr(2, 8))->id, folder.id);

	ChatSession chat = ChatDomainService().CreateNewChat(" folder ", "codex-cli");
	chat.workspace_directory = folder.directory;
	chat.workspace_isolation_kind = " gitWorktree ";
	chat.workspace_source_directory = folder.directory;
	chat.workspace_worktree_directory = " " + (temp.root / "worktree").string() + " ";

	UAM_ASSERT(uam::paths::IsGitWorktreeIsolated(chat));
	UAM_ASSERT(uam::paths::HasGitWorktreeSource(chat));
	UAM_ASSERT(uam::paths::HasGitWorktreeDirectory(chat));
	UAM_ASSERT_EQ(uam::paths::ResolveWorkspaceRootPath(app, chat), uam::paths::AbsolutePathNoThrow(temp.root / "worktree"));

	chat.workspace_worktree_directory.clear();
	chat.workspace_directory.clear();

	UAM_ASSERT(!uam::paths::HasGitWorktreeDirectory(chat));
	UAM_ASSERT_EQ(uam::paths::ResolveWorkspaceRootPath(app, chat), uam::paths::AbsolutePathNoThrow(temp.root / "repo"));
}

UAM_TEST(RemoteHostHealthObservationsPreserveNewerState)
{
	const ExecutionHost observed{"lab", "Lab", "ssh", "lab", "ready", "1", "linux", "x64", "before", "runner", 2};
	for (const bool connected : {false, true})
	{
		for (int change = 0; change < 5; ++change)
		{
			ExecutionHost current = observed;
			switch (change)
			{
			case 0: current.runner_status = "installing"; break;
			case 1: current.runner_directory = "replacement"; break;
			case 2: current.ssh_alias = "replacement"; break;
			case 3: current.runner_version = "2"; break;
			case 4: current.last_seen_at = "newer"; break;
			}
			const ExecutionHost newer = current;
			UAM_ASSERT(!uam::execution_hosts::ApplyHealthObservation(current, observed, connected, "after"));
			UAM_ASSERT(current == newer);
		}
		ExecutionHost current = observed;
		UAM_ASSERT(uam::execution_hosts::ApplyHealthObservation(current, observed, connected, "after"));
		UAM_ASSERT_EQ(current.runner_status, connected ? std::string("ready") : std::string("error"));
		UAM_ASSERT_EQ(current.last_seen_at, connected ? std::string("after") : std::string("before"));
	}
}

UAM_TEST(RemoteHostDirectoryBrowseAllowsInstalledHealthRecheck)
{
	for (const std::string status : {"ready", "error", "offline"})
	{
		ExecutionHost host{"lab", "Lab", "ssh", "lab", status, "1", "linux", "x64", "", "runner", 2};
		UAM_ASSERT(uam::execution_hosts::CanBrowseRemoteDirectories(host));
	}
	ExecutionHost uninstalled{"lab", "Lab", "ssh", "lab", "uninstalled", "", "linux", "x64", "", "runner", 2};
	ExecutionHost installing = uninstalled;
	installing.runner_status = "installing";
	UAM_ASSERT(!uam::execution_hosts::CanBrowseRemoteDirectories(uninstalled));
	UAM_ASSERT(!uam::execution_hosts::CanBrowseRemoteDirectories(installing));
}

UAM_TEST(RemoteWorkspacePathsRemainNativeToTheTargetHost)
{
	TempDir temp("uam-remote-native-path");
	uam::AppState app;
	ChatSession chat;
	chat.execution_host_id = "remote";
	chat.workspace_directory = (temp.root / "local-collision").string();
	fs::create_directories(chat.workspace_directory);
	UAM_ASSERT_EQ(uam::paths::ResolveWorkspaceRootPath(app, chat).string(),
	              chat.workspace_directory);
	UAM_ASSERT(uam::paths::ResolveControllerWorkspaceRootPath(app, chat).empty());
	ChatFolder folder;
	folder.id = "remote-folder";
	folder.execution_host_id = "remote";
	UAM_ASSERT(!uam::paths::IsControllerLocalWorkspace(folder));
	folder.directory = R"(C:\Users\builder\project)";
	app.folders.push_back(folder);
	chat.folder_id = folder.id;
	chat.workspace_directory.clear();
	UAM_ASSERT_EQ(uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(app, chat)),
	              folder.directory);
	UAM_ASSERT(!uam::remote::BuildRemoteTerminalSshArgv(
	    "test-host", "windows", "4.9.0", uam::paths::ResolveWorkspaceRootPath(app, chat),
	    {"codex"}).empty());
	chat.execution_host_id = "different-remote";
	UAM_ASSERT(uam::paths::ResolveWorkspaceRootPath(app, chat).empty());
	chat.execution_host_id = "remote";
	chat.folder_id.clear();
	UAM_ASSERT(uam::paths::ResolveWorkspaceRootPath(app, chat).empty());
	chat.execution_host_id = "local";
	chat.workspace_directory = (temp.root / "local-collision").string();
	UAM_ASSERT_EQ(uam::paths::ResolveControllerWorkspaceRootPath(app, chat),
	              uam::paths::AbsolutePathNoThrow(chat.workspace_directory));
	chat.execution_host_id = "remote";
	chat.workspace_directory = "C:\\Users\\builder\\project";
	UAM_ASSERT(uam::execution_hosts::IsAbsoluteRemotePath(
	    "windows", chat.workspace_directory));
	UAM_ASSERT_EQ(uam::execution_hosts::JoinRemotePath(
	                  "windows", chat.workspace_directory, ".UAM/attachments/file.txt"),
	              std::string("C:\\Users\\builder\\project\\.UAM\\attachments\\file.txt"));
	UAM_ASSERT(uam::execution_hosts::IsAbsoluteRemotePath("linux", "/srv/project"));
	UAM_ASSERT_EQ(uam::execution_hosts::JoinRemotePath(
	                  "linux", "/srv/project/", ".UAM\\attachments\\file.txt"),
	              std::string("/srv/project/.UAM/attachments/file.txt"));
	// Review indexes use host separators even when the controller has another OS.
	UAM_ASSERT_EQ(uam::execution_hosts::JoinRemotePath("linux", "/srv/project/.git", "uam-review-fixture"), std::string("/srv/project/.git/uam-review-fixture"));
	UAM_ASSERT_EQ(uam::execution_hosts::JoinRemotePath("windows", R"(C:\project\.git)", "uam-review-fixture"), std::string(R"(C:\project\.git\uam-review-fixture)"));
	UAM_ASSERT(uam::execution_hosts::IsAbsoluteRemotePath("windows", R"(\\server\share\index)"));
	UAM_ASSERT(!uam::execution_hosts::IsAbsoluteRemotePath("windows", "relative\\project"));
	UAM_ASSERT(!uam::execution_hosts::IsAbsoluteRemotePath("linux", "relative/project"));
	for (const std::string& directory : {std::string("C:\\Users\\Jos\xC3\xA9\\\xE9\xA1\xB9\xE7\x9B\xAE"),
	                                     std::string("/srv/r\xC3\xA9sum\xC3\xA9/\xE9\xA1\xB9\xE7\x9B\xAE")})
	{
		chat.workspace_directory = directory;
		UAM_ASSERT_EQ(uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(app, chat)), directory);
	}
	UAM_ASSERT(!uam::execution_hosts::RemotePathsMatch("linux", R"(/srv/project\one)", "/srv/project/one"));
	chat.provider_id = "codex-cli";
	chat.native_session_id = "same-native-id";
	chat.workspace_directory = R"(/srv/project\one)";
	ChatSession other_directory = chat;
	other_directory.workspace_directory = "/srv/project/one";
	UAM_ASSERT(uam::chat_identity::NativeIdentityKeyForLocalDeduplication(chat) !=
	           uam::chat_identity::NativeIdentityKeyForLocalDeduplication(other_directory));
	chat.workspace_directory = R"(C:\Work\Project)";
	other_directory.workspace_directory = "C:/Work/Project";
	UAM_ASSERT_EQ(uam::chat_identity::NativeIdentityKeyForHistoryImport(chat),
	              uam::chat_identity::NativeIdentityKeyForHistoryImport(other_directory));
	UAM_ASSERT(uam::paths::WorkspaceOwnershipKey(app, "remote", R"(/srv/project\one)") !=
	           uam::paths::WorkspaceOwnershipKey(app, "remote", "/srv/project/one"));
	UAM_ASSERT(uam::execution_hosts::RemotePathsMatch("windows", R"(C:\Work\Project\)", "c:/work/project"));
	UAM_ASSERT(uam::execution_hosts::RemotePathsMatch("linux", "/srv/project/", "/srv/project"));
	UAM_ASSERT_EQ(uam::execution_hosts::JoinRemotePath("linux", R"(/srv/project\)", ".UAM/attachments/file.txt"),
	              std::string(R"(/srv/project\/.UAM/attachments/file.txt)"));
}

UAM_TEST(RemoteWorkspaceLocalWorktreeActionsFailClosedAndMissingHostBlocksVcs)
{
	TempDir temp("uam-remote-workspace-local-actions");
	uam::AppState app;
	app.data_root = temp.root / "uam-data";
	const fs::path local_collision = temp.root / "remote-looking-workspace";
	fs::create_directories(local_collision);
	UAM_ASSERT(uam::io::WriteTextFile(local_collision / "sentinel.txt", "unchanged"));

	ChatSession chat;
	chat.id = "remote-chat";
	chat.execution_host_id = "ssh-remote";
	chat.workspace_directory = local_collision.string();
	chat.workspace_isolation_kind = "gitWorktree";
	chat.workspace_source_directory = local_collision.string();
	chat.workspace_worktree_directory = local_collision.string();
	chat.messages.push_back(Message{MessageRole::Assistant, "done"});

	const uam::GitWorktreeService worktrees;
	const uam::GitWorktreeStatus worktree_status = worktrees.Status(app, chat);
	UAM_ASSERT(!worktree_status.is_git_repository);
	UAM_ASSERT(!worktree_status.error.empty());
	UAM_ASSERT(!worktree_status.worktree_missing);
	UAM_ASSERT(worktrees.CreateForChat(app, chat).message.find("remote") != std::string::npos);
	UAM_ASSERT(worktrees.DiscardChatChanges(app, chat).message.find("remote") != std::string::npos);
	UAM_ASSERT(worktrees.PortChatChanges(app, chat).message.find("remote") != std::string::npos);
	std::string checkpoint_reason;
	UAM_ASSERT(!worktrees.CanCheckpointTurn(app, chat, &checkpoint_reason));
	UAM_ASSERT(checkpoint_reason.find("remote") != std::string::npos);
	UAM_ASSERT(worktrees.CreateTurnCheckpoint(app, chat, 0).message.find("remote") != std::string::npos);
	UAM_ASSERT(worktrees.PreviewTurnRollback(app, chat, 0).message.find("remote") != std::string::npos);

	const uam::VcsCommitService vcs;
	const uam::VcsCommitStatus vcs_status = vcs.Status(app, chat);
	UAM_ASSERT(!vcs_status.available);
	UAM_ASSERT(vcs_status.error.find("host") != std::string::npos);
	std::string diff_error;
	UAM_ASSERT(vcs.Diff(app, chat, "sentinel.txt", uam::VcsType::Git, &diff_error).empty());
	UAM_ASSERT(diff_error.find("host") != std::string::npos);
	UAM_ASSERT(vcs.Commit(app, chat, uam::VcsType::Git, "Must not run", {"sentinel.txt"})
	               .error.find("host") != std::string::npos);
	UAM_ASSERT(vcs.GenerateMessage(app, chat, uam::VcsType::Git, {"sentinel.txt"})
	               .error.find("host") != std::string::npos);
	ChatFolder remote_folder;
	remote_folder.directory = local_collision.string();
	remote_folder.execution_host_id = chat.execution_host_id;
	std::error_code native_history_error;
	UAM_ASSERT(!ChatHistorySyncService().DeleteNativeWorkspaceHistoryForFolder(
	    app, remote_folder, &native_history_error));
	UAM_ASSERT(!native_history_error);

	UAM_ASSERT_EQ(ReadFile(local_collision / "sentinel.txt"), std::string("unchanged"));
	UAM_ASSERT(!uam::paths::PathExistsNoThrow(app.data_root / "worktrees"));
}

UAM_TEST(RelativePathIfInsideRootUsesPathSegments)
{
	TempDir temp("uam-path-relative");
	const fs::path root = temp.root / "workspace";
	const fs::path inside = root / "nested" / "file.txt";
	const fs::path inside_dot_prefixed = root / "..not-parent" / "file.txt";
	const fs::path sibling = temp.root / "workspace-sibling" / "file.txt";
	UAM_ASSERT(uam::io::WriteTextFile(inside, "inside"));
	UAM_ASSERT(uam::io::WriteTextFile(inside_dot_prefixed, "dot-prefixed"));
	UAM_ASSERT(uam::io::WriteTextFile(sibling, "sibling"));

	const std::optional<fs::path> inside_relative = uam::paths::RelativePathIfInsideRoot(root, inside);
	UAM_ASSERT(inside_relative.has_value());
	UAM_ASSERT_EQ(uam::paths::PortablePathString(*inside_relative), std::string("nested/file.txt"));

	const std::optional<fs::path> dot_prefixed_relative = uam::paths::RelativePathIfInsideRoot(root, inside_dot_prefixed);
	UAM_ASSERT(dot_prefixed_relative.has_value());
	UAM_ASSERT_EQ(uam::paths::PortablePathString(*dot_prefixed_relative), std::string("..not-parent/file.txt"));

	UAM_ASSERT(!uam::paths::RelativePathIfInsideRoot(root, sibling).has_value());
}

UAM_TEST(FolderDirectoryMatchesNormalizesEquivalentPathShapes)
{
	TempDir temp("uam-folder-path-match");
#if defined(_WIN32)
	ScopedEnvVar home("USERPROFILE", temp.root.string());
#else
	ScopedEnvVar home("HOME", temp.root.string());
#endif
	const fs::path workspace = temp.root / "workspace";
	const fs::path child = workspace / "child";
	fs::create_directories(child);

	UAM_ASSERT(FolderDirectoryMatches(workspace / ".", child / ".."));
	UAM_ASSERT(FolderDirectoryMatches(workspace, child / ".." / "."));
	UAM_ASSERT(FolderDirectoryMatches(fs::path("~/workspace"), workspace));
	UAM_ASSERT(!FolderDirectoryMatches(workspace, temp.root / "workspace-sibling"));
}

UAM_TEST(GitWorktreeCreationExplainsUnbornHeadWithoutChangingRepository)
{
	UAM_ASSERT(GitAvailableForTests());
	for (const bool with_files : {false, true})
	{
		TempDir temp("uam-unborn-worktree");
		const fs::path repo = temp.root / "repo";
		fs::create_directories(repo);
		UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(uam::paths::Utf8PathString(repo))));
		UAM_ASSERT(RunGitForTest(repo, "symbolic-ref HEAD refs/heads/new-work"));
		if (with_files)
		{
			UAM_ASSERT(uam::io::WriteTextFile(repo / "staged.txt", "staged\n"));
			UAM_ASSERT(uam::io::WriteTextFile(repo / "untracked.txt", "untracked\n"));
			UAM_ASSERT(RunGitForTest(repo, "add staged.txt"));
		}
		const std::string original_head = uam::io::ReadTextFile(repo / ".git" / "HEAD");
		const std::string original_index = uam::io::ReadTextFile(repo / ".git" / "index");
		uam::AppState app;
		app.data_root = temp.root / "data";
		ChatSession chat;
		chat.id = "unborn-worktree";
		chat.workspace_directory = uam::paths::Utf8PathString(repo);
		const uam::GitWorktreeOperationResult result = uam::GitWorktreeService().CreateForChat(app, chat);
		UAM_ASSERT(!result.ok);
		UAM_ASSERT_EQ(result.message, std::string("This repository has no commits. Create an initial commit before creating a worktree."));
		UAM_ASSERT(chat.workspace_isolation_kind.empty());
		UAM_ASSERT(chat.workspace_worktree_directory.empty());
		UAM_ASSERT_EQ(uam::io::ReadTextFile(repo / ".git" / "HEAD"), original_head);
		UAM_ASSERT_EQ(uam::io::ReadTextFile(repo / ".git" / "index"), original_index);
		UAM_ASSERT(!fs::exists(repo / ".git" / "refs" / "heads" / "new-work"));
		UAM_ASSERT(!fs::exists(repo / ".git" / "worktrees"));
		UAM_ASSERT(!fs::exists(app.data_root));
		if (with_files)
		{
			UAM_ASSERT_EQ(uam::io::ReadTextFile(repo / "staged.txt"), std::string("staged\n"));
			UAM_ASSERT_EQ(uam::io::ReadTextFile(repo / "untracked.txt"), std::string("untracked\n"));
		}
	}
}

UAM_TEST(GitWorktreeNamesUseAgentAndTitleWithSafeFallbacksAndCollisions)
{
	UAM_ASSERT(GitAvailableForTests());
	TempDir temp("uam-worktree-names");
	const fs::path repo = temp.root / "source";
	fs::create_directories(repo);
	UAM_ASSERT(RunGitForTest(repo, "init"));
	UAM_ASSERT(RunGitForTest(repo, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(repo, "config user.name UAM"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "app.txt", "initial\n"));
	UAM_ASSERT(RunGitForTest(repo, "add app.txt"));
	UAM_ASSERT(RunGitForTest(repo, "commit -m initial"));
	uam::AppState app;
	app.data_root = temp.root / "data";
	const uam::GitWorktreeService service;
	const std::vector<std::pair<std::string, std::string>> cases = {
	    {" Fix Login! ", "review/fix-login"},
	    {"Fix Login!", "review/fix-login-3"},
	    {"Fix Login!", "review/fix-login-3"},
	    {"../CON.lock / path@{name}", "review/con-lock-path-name"},
	    {"CON", "review/chat-con"},
	    {"New Session", "review/chat-name-5"},
	    {"...///", "review/chat-name-6"},
	    {"", "review/chat-name-7"},
	    {"r\xC3\xA9parer", "review/r\xC3\xA9parer"},
	    {std::string(200, 'A'), "review/" + std::string(80, 'a')},
	    {"Custom chat", "agents-2/custom-chat"}};
	UAM_ASSERT(RunGitForTest(repo, "branch review/fix-login-2"));
	UAM_ASSERT(RunGitForTest(repo, "branch agents"));
	for (std::size_t index = 0; index < cases.size(); ++index)
	{
		ChatSession chat;
		chat.id = "chat-name-" + std::to_string(index);
		chat.title = cases[index].first;
		chat.uam_agent_id = index == cases.size() - 1 ? "" : "Review";
		chat.workspace_directory = repo.string();
		const uam::GitWorktreeOperationResult result = service.CreateForChat(app, chat);
		if (!result.ok)
		{
			uam_test::Fail(("worktree case " + std::to_string(index) + " (" + chat.title + "): " + result.message).c_str(), __FILE__, __LINE__);
		}
		UAM_ASSERT_EQ(chat.workspace_branch_name, cases[index].second);
		const fs::path worktree = uam::paths::PathFromUtf8(chat.workspace_worktree_directory);
#if defined(_WIN32)
		const std::string repo_key = uam::hashing::Hex64(uam::hashing::Fnv1a64(uam::paths::NormalizedPortablePathString(repo)));
		const fs::path original_path = app.data_root / "worktrees" / repo_key / uam::paths::PathFromUtf8(cases[index].second);
		if (uam::paths::Utf8PathString(uam::paths::AbsolutePathNoThrow(original_path) / ".git").size() > 220)
		{
			UAM_ASSERT(uam::paths::Utf8PathString(uam::paths::AbsolutePathNoThrow(worktree) / ".git").size() <= 220);
			UAM_ASSERT_EQ(uam::paths::Utf8PathString(worktree.parent_path().filename()), cases[index].second.substr(0, cases[index].second.find('/')));
			UAM_ASSERT(uam::paths::Utf8PathString(worktree.filename()).ends_with("-" + uam::hashing::Hex64Padded(uam::hashing::Fnv1a64(cases[index].second))));
		}
		else
#endif
		{
			UAM_ASSERT_EQ(uam::paths::Utf8PathString(worktree.parent_path().filename()) + "/" + uam::paths::Utf8PathString(worktree.filename()), cases[index].second);
		}
		UAM_ASSERT(RunGitForTest(repo, "check-ref-format --branch " + ShellQuoteForTest(chat.workspace_branch_name)));
		chat.title = "Renamed after creation";
		chat.uam_agent_id = "build";
		std::string error;
		// Retain the first worktree to exercise the filesystem collision on the next chat.
		if (index != 0)
		{
			UAM_ASSERT(service.RemoveUnusedBranchWorktree(app, chat, &error));
			// Match rollback ownership: a removed chat must not retain a claim on a reused path.
			UAM_ASSERT(!ChatRepository::DeleteChatStorageFiles(app.data_root, chat.id).Failed());
		}
		UAM_ASSERT_EQ(fs::exists(worktree), index == 0);
	}

	ChatSession legacy;
	legacy.id = "chat-legacy";
	legacy.title = "Legacy compatibility";
	legacy.folder_id = "legacy-workspace";
	legacy.workspace_directory = repo.string();
	app.folders.push_back(ChatFolder{legacy.folder_id, "Legacy workspace", repo.string(), false});
	UAM_ASSERT(service.CreateForChat(app, legacy).ok);
	const fs::path named_path = legacy.workspace_worktree_directory;
	const fs::path legacy_path = named_path.parent_path().parent_path() / legacy.id;
	UAM_ASSERT(RunGitForTest(repo, "worktree move " + ShellQuoteForTest(named_path.string()) + " " + ShellQuoteForTest(legacy_path.string())));
	UAM_ASSERT(RunGitForTest(legacy_path, "branch -m uam/chat-legacy"));
	legacy.workspace_worktree_directory = legacy_path.string();
	legacy.workspace_branch_name = "uam/chat-legacy";
	std::string error;
	UAM_ASSERT(service.RemoveUnusedBranchWorktree(app, legacy, &error));
	UAM_ASSERT(!fs::exists(legacy_path));
}

#if defined(_WIN32)
UAM_TEST(GitWorktreeWindowsPhysicalBudgetPreservesBranchesAndCleanup)
{
	UAM_ASSERT(GitAvailableForTests());
	TempDir temp("uam-git-budget");
	const fs::path repo = temp.root / "source";
	fs::create_directories(repo);
	UAM_ASSERT(RunGitForTest(repo, "init --template="));
	UAM_ASSERT(RunGitForTest(repo, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(repo, "config user.name UAM"));
	UAM_ASSERT(RunGitForTest(repo, "commit --allow-empty -m initial"));
	const std::function<std::string(const std::string&)> git_output = [&repo](const std::string& args)
	{
		const ProcessExecutionResult result = PlatformServicesFactory::Instance().process_service.ExecuteCommand(
		    "git -C " + ShellQuoteForTest(uam::paths::Utf8PathString(repo)) + " " + args, 120000);
		UAM_ASSERT(result.ok && !result.timed_out && !result.canceled && result.exit_code == 0);
		return uam::strings::Trim(result.output);
	};
	// The service keys worktrees by Git's verified top level, which Git for Windows reports in
	// long-name form even when the runner's TEMP uses an 8.3 alias such as RUNNER~1.
	const std::string repo_key = uam::hashing::Hex64(uam::hashing::Fnv1a64(uam::paths::NormalizedPortablePathString(uam::paths::PathFromUtf8(git_output("rev-parse --show-toplevel")))));
	uam::AppState app;
	app.data_root = temp.root / "data";
	const std::size_t initial_parent_bytes = uam::paths::Utf8PathString(fs::weakly_canonical(app.data_root / "worktrees" / repo_key / "review")).size();
	UAM_ASSERT(initial_parent_bytes <= 182);
	app.data_root = temp.root / ("data" + std::string(182 - initial_parent_bytes, 'p'));
	const uam::GitWorktreeService service;
	const std::string logical_name = "review/" + std::string(80, 'a');
	const fs::path original = app.data_root / "worktrees" / repo_key / uam::paths::PathFromUtf8(logical_name);
	UAM_ASSERT_EQ(uam::paths::Utf8PathString(fs::weakly_canonical(original) / ".git").size(), static_cast<std::size_t>(268));
	std::vector<ChatSession> chats(4);
	for (std::size_t index = 0; index < chats.size(); ++index)
	{
		ChatSession& chat = chats[index];
		chat.id = "budget-chat-" + std::to_string(index);
		if (index == 3) for (int letter = 0; letter < 100; ++letter) chat.title += "r\xC3\xA9";
		else chat.title = std::string(200, 'A');
		chat.uam_agent_id = "Review";
		chat.workspace_directory = uam::paths::Utf8PathString(repo);
		const uam::GitWorktreeOperationResult result = index == 1
		    ? service.CreateBranchForChat(app, chats.front(), chat, chats.front().workspace_base_ref)
		    : service.CreateForChat(app, chat);
		if (!result.ok) uam_test::Fail(result.message.c_str(), __FILE__, __LINE__);
		std::string unicode_name = "review/";
		if (index == 3) for (int letter = 0; letter < 26; ++letter) unicode_name += "r\xC3\xA9";
		const std::string expected_branch = index == 3 ? unicode_name + "r" : logical_name + (index == 0 ? "" : "-" + std::to_string(index + 1));
		UAM_ASSERT_EQ(chat.workspace_branch_name, expected_branch);
		const fs::path actual = uam::paths::PathFromUtf8(chat.workspace_worktree_directory);
		UAM_ASSERT_EQ(actual.parent_path(), original.parent_path());
		UAM_ASSERT(uam::paths::Utf8PathString(fs::weakly_canonical(actual) / ".git").size() <= 220);
		UAM_ASSERT_EQ(uam::paths::Utf8PathString(actual.filename()), (index == 3 ? std::string("r\xC3\xA9r\xC3\xA9r\xC3\xA9r\xC3\xA9r\xC3\xA9") : std::string(15, 'a')) + "-" + uam::hashing::Hex64Padded(uam::hashing::Fnv1a64(expected_branch)));
		UAM_ASSERT(("\n" + git_output("worktree list --porcelain") + "\n").find("\nbranch refs/heads/" + expected_branch + "\n") != std::string::npos);
		UAM_ASSERT(service.Status(app, chat).isolated);
		if (index > 0) UAM_ASSERT(chat.workspace_worktree_directory != chats[index - 1].workspace_worktree_directory);
	}
	for (const ChatSession& chat : chats)
	{
		std::string error;
		if (!service.RemoveUnusedBranchWorktree(app, chat, &error))
			uam_test::Fail(("RemoveUnusedBranchWorktree(" + chat.id + ") refused: " + error + " worktree=" + chat.workspace_worktree_directory + " source=" + chat.workspace_source_directory).c_str(), __FILE__, __LINE__);
		UAM_ASSERT(!fs::exists(uam::paths::PathFromUtf8(chat.workspace_worktree_directory)));
	}
	UAM_ASSERT(git_output("worktree list --porcelain").find("refs/heads/" + logical_name) == std::string::npos);

	// A parent that cannot hold a readable hash-backed leaf is refused before creating data.
	const std::string refs_before = git_output("for-each-ref refs/heads");
	app.data_root = temp.root / ("data" + std::string(212 - initial_parent_bytes, 'p'));
	UAM_ASSERT(!fs::exists(app.data_root));
	ChatSession refused;
	refused.id = "budget-refused";
	refused.title = std::string(200, 'A');
	refused.uam_agent_id = "Review";
	refused.workspace_directory = uam::paths::Utf8PathString(repo);
	const uam::GitWorktreeOperationResult rejected = service.CreateForChat(app, refused);
	UAM_ASSERT(!rejected.ok);
	UAM_ASSERT(rejected.message.find("data directory") != std::string::npos);
	UAM_ASSERT(refused.workspace_worktree_directory.empty());
	UAM_ASSERT(!fs::exists(app.data_root));
	UAM_ASSERT_EQ(git_output("for-each-ref refs/heads"), refs_before);
	const fs::path plain = temp.root / "plain";
	fs::create_directory(plain);
	UAM_ASSERT(uam::io::WriteTextFile(plain / "app.txt", "unversioned\n"));
	refused.workspace_directory = uam::paths::Utf8PathString(plain);
	UAM_ASSERT(!service.CreateForChat(app, refused).ok);
	UAM_ASSERT(!fs::exists(app.data_root));

	// Actual managed baseline refs still reserve an agent-prefix branch namespace.
	app.data_root = temp.root / "managed-data";
	refused.id = "managed-default-branch";
	refused.title = "Short";
	ScopedEnvVar config_count("GIT_CONFIG_COUNT", "1");
	ScopedEnvVar config_key("GIT_CONFIG_KEY_0", "init.defaultBranch");
	ScopedEnvVar config_value("GIT_CONFIG_VALUE_0", "review");
	const uam::GitWorktreeOperationResult managed = service.CreateForChat(app, refused);
	if (!managed.ok) uam_test::Fail(managed.message.c_str(), __FILE__, __LINE__);
	UAM_ASSERT_EQ(refused.workspace_branch_name, std::string("review-2/short"));
	std::string managed_cleanup_error;
	UAM_ASSERT(service.RemoveUnusedBranchWorktree(app, refused, &managed_cleanup_error));
}
#endif

UAM_TEST(GitWorktreeServiceCreatesDiscardsAndPortsChanges)
{
	if (!GitAvailableForTests())
	{
		return;
	}

	TempDir temp("uam-git-worktree");
	const fs::path repo = temp.root / uam::paths::PathFromUtf8("r\xC3\xA9po-\xE2\x98\x83");
	fs::create_directories(repo);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(uam::paths::Utf8PathString(repo))));
	UAM_ASSERT(RunGitForTest(repo, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(repo, "config user.name UAM"));
	UAM_ASSERT(RunGitForTest(repo, "config core.autocrlf false"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "app.txt", "one\n"));
	UAM_ASSERT(RunGitForTest(repo, "add app.txt"));
	UAM_ASSERT(RunGitForTest(repo, "commit -m initial"));

	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatFolder folder;
	folder.id = "folder";
	folder.directory = uam::paths::Utf8PathString(repo);
	app.folders.push_back(folder);
	ChatSession chat = ChatDomainService().CreateNewChat(folder.id, "codex-cli");
	chat.id = "chat-worktree-service";
	chat.title = "Worktree Service";
	chat.workspace_directory = uam::paths::Utf8PathString(repo);
	app.chats.push_back(chat);

	uam::GitWorktreeService service;
	uam::GitWorktreeOperationResult created = service.CreateForChat(app, app.chats.front());
	UAM_ASSERT(created.ok);
	UAM_ASSERT_EQ(app.chats.front().workspace_isolation_kind, std::string("gitWorktree"));
	UAM_ASSERT_EQ(app.chats.front().workspace_branch_name, std::string("build/worktree-service"));
	const fs::path worktree = uam::paths::PathFromUtf8(app.chats.front().workspace_worktree_directory);
	const std::string branch_name = app.chats.front().workspace_branch_name;
	UAM_ASSERT(fs::exists(worktree / "app.txt"));
	UAM_ASSERT_EQ(uam::paths::ResolveWorkspaceRootPath(app, app.chats.front()), uam::paths::AbsolutePathNoThrow(worktree));

	UAM_ASSERT(uam::io::WriteTextFile(worktree / "app.txt", "discard me\n"));
	UAM_ASSERT(uam::io::WriteTextFile(worktree / "scratch.txt", "remove me\n"));
	app.chats.front().workspace_source_directory = " " + app.chats.front().workspace_source_directory + " ";
	app.chats.front().workspace_branch_name = " " + branch_name + " ";
	const fs::path persisted_data_root = app.data_root;
	const fs::path blocked_data_root = temp.root / "blocked-data-root";
	UAM_ASSERT(uam::io::WriteTextFile(blocked_data_root, "not a directory"));
	app.data_root = blocked_data_root;
	const uam::GitWorktreeOperationResult failed_discard = service.DiscardChatChanges(app, app.chats.front());
	UAM_ASSERT(!failed_discard.ok);
	UAM_ASSERT_EQ(app.chats.front().workspace_isolation_kind, std::string("gitWorktree"));
	UAM_ASSERT(fs::exists(worktree / "app.txt"));
	app.data_root = persisted_data_root;
	uam::GitWorktreeOperationResult discarded = service.DiscardChatChanges(app, app.chats.front());
	UAM_ASSERT(discarded.ok);
	UAM_ASSERT_EQ(app.chats.front().workspace_isolation_kind, std::string(""));
	UAM_ASSERT_EQ(app.chats.front().workspace_worktree_directory, std::string(""));
	UAM_ASSERT_EQ(uam::paths::ResolveWorkspaceRootPath(app, app.chats.front()), uam::paths::AbsolutePathNoThrow(repo));
	UAM_ASSERT(!fs::exists(worktree));
	UAM_ASSERT(!RunGitForTest(repo, "rev-parse --verify " + ShellQuoteForTest(branch_name)));

	created = service.CreateForChat(app, app.chats.front());
	UAM_ASSERT(created.ok);
	const fs::path second_worktree = app.chats.front().workspace_worktree_directory;
	UAM_ASSERT(uam::io::WriteTextFile(second_worktree / "app.txt", "two\n"));
	UAM_ASSERT(uam::io::WriteTextFile(second_worktree / "new.txt", "new\n"));
	uam::GitWorktreeOperationResult ported = service.PortChatChanges(app, app.chats.front());
	UAM_ASSERT(ported.ok);
	UAM_ASSERT(fs::exists(ported.patch_path));
	UAM_ASSERT_EQ(ReadFile(repo / "app.txt"), std::string("two\n"));
	UAM_ASSERT_EQ(ReadFile(repo / "new.txt"), std::string("new\n"));
	UAM_ASSERT_EQ(app.chats.front().workspace_isolation_kind, std::string(""));
	UAM_ASSERT_EQ(app.chats.front().workspace_worktree_directory, std::string(""));
	UAM_ASSERT_EQ(uam::paths::ResolveWorkspaceRootPath(app, app.chats.front()), uam::paths::AbsolutePathNoThrow(repo));
	UAM_ASSERT(!fs::exists(second_worktree));
}

UAM_TEST(GitWorktreeTurnCheckpointsAreAtomicIdempotentAndRollbackSafe)
{
	if (!GitAvailableForTests()) return;

	TempDir temp("uam-git-turn-checkpoint");
	const fs::path repo = temp.root / "repo";
	fs::create_directories(repo);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repo.string())));
	UAM_ASSERT(RunGitForTest(repo, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(repo, "config user.name UAM"));
	UAM_ASSERT(RunGitForTest(repo, "config core.autocrlf false"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "app.txt", "before\n"));
	UAM_ASSERT(RunGitForTest(repo, "add app.txt"));
	UAM_ASSERT(RunGitForTest(repo, "commit -m initial"));

	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatFolder folder;
	folder.id = "folder";
	folder.directory = repo.string();
	app.folders.push_back(folder);
	ChatSession chat = ChatDomainService().CreateNewChat(folder.id, "codex-cli");
	chat.id = "chat-turn-checkpoint";
	chat.workspace_directory = repo.string();
	chat.messages.push_back(Message{MessageRole::User, "Change the app."});
	chat.messages.push_back(Message{MessageRole::Assistant, "Done."});
	app.chats.push_back(std::move(chat));

	uam::GitWorktreeService service;
	UAM_ASSERT(service.CreateForChat(app, app.chats.front()).ok);
	ChatSession& isolated = app.chats.front();
	const fs::path worktree = isolated.workspace_worktree_directory;
	UAM_ASSERT(service.CanCheckpointTurn(app, isolated));
	UAM_ASSERT(uam::io::WriteTextFile(worktree / "app.txt", "after\n"));
	UAM_ASSERT(uam::io::WriteTextFile(worktree / "new.txt", "new\n"));
	uam::AcpSessionState canceled_turn;
	canceled_turn.processing = true;
	canceled_turn.current_assistant_message_index = 1;
	canceled_turn.turn_checkpoint_eligible = true;
	uam::acp_detail::CompletePromptTurnAndHandleGoalLoop(app, canceled_turn, isolated, "ready", nullptr, false);
	UAM_ASSERT(isolated.messages[1].checkpoint_sha.empty());

	const uam::GitTurnCheckpointResult created = service.CreateTurnCheckpoint(app, isolated, 1);
	UAM_ASSERT(created.ok && created.changed);
	UAM_ASSERT_EQ(created.checkpoint_sha.size(), static_cast<std::size_t>(40));
	UAM_ASSERT_EQ(isolated.messages[1].checkpoint_sha, created.checkpoint_sha);
	UAM_ASSERT_EQ(ReadFile(repo / "app.txt"), std::string("before\n"));
	UAM_ASSERT_EQ(ReadFile(worktree / "app.txt"), std::string("after\n"));
	const uam::VcsCommitStatus review = uam::VcsCommitService().Status(app, isolated, uam::VcsType::Git, true, isolated.workspace_base_ref);
	UAM_ASSERT(review.available && review.error.empty());
	UAM_ASSERT(std::ranges::any_of(review.changed_files, [](const uam::VcsChangedFile& file) { return file.path == "app.txt" && file.additions == 1 && file.deletions == 1; }));
	UAM_ASSERT(std::ranges::any_of(review.changed_files, [](const uam::VcsChangedFile& file) { return file.path == "new.txt" && file.additions == 1; }));
	std::string diff_error;
	const std::string committed_diff = uam::VcsCommitService().Diff(app, isolated, "app.txt", uam::VcsType::Git, &diff_error, isolated.workspace_base_ref);
	UAM_ASSERT(diff_error.empty());
	UAM_ASSERT(committed_diff.find("-before") != std::string::npos && committed_diff.find("+after") != std::string::npos);

	const ProcessExecutionResult checkpoint_count = PlatformServicesFactory::Instance().process_service.ExecuteCommand(
	    "git -C " + ShellQuoteForTest(worktree.string()) + " rev-list --count HEAD", 120000);
	UAM_ASSERT(checkpoint_count.ok);
	const uam::GitTurnCheckpointResult duplicate = service.CreateTurnCheckpoint(app, isolated, 1);
	UAM_ASSERT(duplicate.ok && !duplicate.changed);
	const ProcessExecutionResult duplicate_count = PlatformServicesFactory::Instance().process_service.ExecuteCommand(
	    "git -C " + ShellQuoteForTest(worktree.string()) + " rev-list --count HEAD", 120000);
	UAM_ASSERT(duplicate_count.ok);
	UAM_ASSERT_EQ(uam::strings::Trim(duplicate_count.output), uam::strings::Trim(checkpoint_count.output));

	const std::vector<ChatSession> persisted = ChatRepository::LoadLocalChats(app.data_root);
	UAM_ASSERT_EQ(persisted.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(persisted.front().messages[1].checkpoint_sha, created.checkpoint_sha);
	UAM_ASSERT(uam::io::WriteTextFile(worktree / "partial.txt", "keep me\n"));
	UAM_ASSERT(!service.PreviewTurnRollback(app, isolated, 1).ok);
	UAM_ASSERT(!service.RollbackTurn(app, isolated, 1).ok);
	UAM_ASSERT(fs::exists(worktree / "partial.txt"));
	UAM_ASSERT(fs::remove(worktree / "partial.txt"));

	const uam::GitTurnCheckpointResult preview = service.PreviewTurnRollback(app, isolated, 1);
	UAM_ASSERT(preview.ok);
	UAM_ASSERT(preview.diff.find("app.txt") != std::string::npos);
	const std::vector<Message> messages_before_rollback = isolated.messages;
	std::optional<ChatSession> cold_live = ChatRepository::LoadLocalChat(app.data_root, isolated.id, false);
	std::optional<ChatSession> rollback_worker = ChatRepository::LoadLocalChat(app.data_root, isolated.id, true);
	UAM_ASSERT(cold_live.has_value() && rollback_worker.has_value());
	const uam::GitTurnRollbackSnapshot cold_snapshot = uam::CaptureGitTurnRollbackSnapshot(*cold_live, 1);
	const uam::GitTurnRollbackSnapshot loaded_snapshot = uam::CaptureGitTurnRollbackSnapshot(isolated, 1);
	UAM_ASSERT(uam::MatchesGitTurnRollbackSnapshot(*rollback_worker, cold_snapshot));
	UAM_ASSERT(uam::MatchesGitTurnRollbackSnapshot(isolated, loaded_snapshot));
	ChatSession stale = *rollback_worker;
	stale.workspace_worktree_directory += "/changed";
	UAM_ASSERT(!uam::MatchesGitTurnRollbackSnapshot(stale, cold_snapshot));
	stale = *rollback_worker;
	stale.updated_at += "-changed";
	UAM_ASSERT(!uam::MatchesGitTurnRollbackSnapshot(stale, cold_snapshot));
	stale = *rollback_worker;
	stale.native_session_id += "-changed";
	UAM_ASSERT(!uam::MatchesGitTurnRollbackSnapshot(stale, cold_snapshot));
	stale = isolated;
	const uam::GitTurnRollbackSnapshot loaded_checkpoint_snapshot = uam::CaptureGitTurnRollbackSnapshot(stale, 1);
	stale.messages[1].checkpoint_parent_sha += "-changed";
	UAM_ASSERT(!uam::MatchesGitTurnRollbackSnapshot(stale, loaded_checkpoint_snapshot));
	stale = *rollback_worker;
	stale.messages.clear();
	stale.messages_loaded = false;
	++stale.persisted_message_count;
	UAM_ASSERT(!uam::MatchesGitTurnRollbackSnapshot(stale, cold_snapshot));
	UAM_ASSERT(!service.RollbackTurn(app, *rollback_worker, 0).ok);
	const uam::GitTurnCheckpointResult rolled_back = service.RollbackTurn(app, *rollback_worker, 1);
	UAM_ASSERT(rolled_back.ok);
	UAM_ASSERT_EQ(rolled_back.checkpoint_sha, preview.checkpoint_sha);
	UAM_ASSERT_EQ(ReadFile(worktree / "app.txt"), std::string("before\n"));
	UAM_ASSERT(!fs::exists(worktree / "new.txt"));
	const std::optional<ChatSession> persisted_after = ChatRepository::LoadLocalChat(app.data_root, isolated.id, false);
	UAM_ASSERT(persisted_after.has_value());
	ChatSession raced_live = *cold_live;
	raced_live.workspace_worktree_directory += "/changed";
	UAM_ASSERT(!uam::ApplyGitTurnRollbackSnapshot(raced_live, cold_snapshot, *persisted_after, rolled_back.checkpoint_sha));
	ChatSession stale_parent_live = isolated;
	stale_parent_live.messages[1].checkpoint_parent_sha += "-changed";
	UAM_ASSERT(!uam::ApplyGitTurnRollbackSnapshot(stale_parent_live, loaded_checkpoint_snapshot,
	                                              *persisted_after, rolled_back.checkpoint_sha));
	UAM_ASSERT(uam::ApplyGitTurnRollbackSnapshot(*cold_live, cold_snapshot, *persisted_after, rolled_back.checkpoint_sha));
	UAM_ASSERT(cold_live->messages.empty() && !cold_live->messages_loaded);
	ChatSession loaded_after = isolated;
	UAM_ASSERT(uam::ApplyGitTurnRollbackSnapshot(loaded_after, loaded_snapshot, *persisted_after, rolled_back.checkpoint_sha));
	UAM_ASSERT(loaded_after.messages[1].checkpoint_sha.empty());
	UAM_ASSERT_EQ(loaded_after.persisted_messages_digest, persisted_after->persisted_messages_digest);
	std::optional<ChatSession> rehydrated_after = ChatRepository::LoadLocalChat(app.data_root, isolated.id, true);
	UAM_ASSERT(rehydrated_after.has_value());
	UAM_ASSERT_EQ(rehydrated_after->messages.size(), messages_before_rollback.size());
	UAM_ASSERT_EQ(rehydrated_after->messages[0].content, messages_before_rollback[0].content);
	UAM_ASSERT(rehydrated_after->messages[1].checkpoint_sha.empty());
	isolated = *rehydrated_after;
	UAM_ASSERT_EQ(ReadFile(repo / "app.txt"), std::string("before\n"));

	isolated.messages.push_back(Message{MessageRole::Assistant, "No files changed."});
	const uam::GitTurnCheckpointResult unchanged = service.CreateTurnCheckpoint(app, isolated, 2);
	UAM_ASSERT(unchanged.ok && !unchanged.changed);

	auto checkpoint_session = std::make_unique<uam::AcpSessionState>();
	checkpoint_session->chat_id = isolated.id;
	checkpoint_session->provider_id = "codex-cli";
	checkpoint_session->running = true;
	checkpoint_session->processing = true;
	checkpoint_session->queued_prompt = "Held until checkpoint preflight completes.";
	checkpoint_session->turn_serial = 7;
	uam::AcpSessionState& live_checkpoint_session = *checkpoint_session;
	app.acp_sessions.push_back(std::move(checkpoint_session));
	UAM_ASSERT(uam::acp_detail::ScheduleTurnCheckpointPreflight(app, live_checkpoint_session, isolated));
	UAM_ASSERT(live_checkpoint_session.turn_checkpoint_preflight_pending);
	UAM_ASSERT_EQ(app.turn_checkpoint_tasks.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(live_checkpoint_session.prompt_request_id, 0);
	for (int attempt = 0; attempt < 400 && !app.turn_checkpoint_tasks.empty(); ++attempt)
	{
		(void)uam::acp_detail::PollTurnCheckpointTasks(app, nullptr);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT(app.turn_checkpoint_tasks.empty());
	UAM_ASSERT(!live_checkpoint_session.turn_checkpoint_preflight_pending);
	UAM_ASSERT(live_checkpoint_session.turn_checkpoint_eligible);

	isolated.messages.push_back(Message{MessageRole::User, "Make one more change."});
	isolated.messages.push_back(Message{MessageRole::Assistant, "Made one more change."});
	UAM_ASSERT(uam::io::WriteTextFile(worktree / "async.txt", "checkpointed off the UI thread\n"));
	live_checkpoint_session.queued_prompt.clear();
	live_checkpoint_session.current_assistant_message_index = 4;
	live_checkpoint_session.turn_assistant_message_index = 4;
	live_checkpoint_session.turn_checkpoint_eligible = true;
	uam::acp_detail::CompletePromptTurnAndHandleGoalLoop(app, live_checkpoint_session, isolated, "ready", nullptr, true);
	UAM_ASSERT(live_checkpoint_session.turn_checkpoint_commit_pending);
	UAM_ASSERT(isolated.messages[4].checkpoint_sha.empty());
	UAM_ASSERT(app.worktree_operation_chat_ids.contains(isolated.id));
	for (int attempt = 0; attempt < 400 && !app.turn_checkpoint_tasks.empty(); ++attempt)
	{
		(void)uam::acp_detail::PollTurnCheckpointTasks(app, nullptr);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT(app.turn_checkpoint_tasks.empty());
	UAM_ASSERT(!live_checkpoint_session.turn_checkpoint_commit_pending);
	UAM_ASSERT(!app.worktree_operation_chat_ids.contains(isolated.id));
	UAM_ASSERT_EQ(isolated.messages[4].checkpoint_sha.size(), static_cast<std::size_t>(40));

	uam::AsyncTurnCheckpointTask waiting_task;
	waiting_task.chat_id = isolated.id;
	waiting_task.state = std::make_shared<uam::AsyncTurnCheckpointState>();
	waiting_task.worker = std::make_unique<std::jthread>([](std::stop_token stop_token) {
		while (!stop_token.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	});
	app.worktree_operation_chat_ids.insert(isolated.id);
	app.turn_checkpoint_tasks.push_back(std::move(waiting_task));
	uam::acp_detail::StopTurnCheckpointTasks(app);
	UAM_ASSERT(app.turn_checkpoint_tasks.empty());
	UAM_ASSERT(!app.worktree_operation_chat_ids.contains(isolated.id));

	UAM_ASSERT(uam::io::WriteTextFile(worktree / "preexisting.txt", "existing\n"));
	UAM_ASSERT(!service.CanCheckpointTurn(app, isolated));
}

UAM_TEST(GitWorktreeDefaultLaunchPersistsRetriesAndLeavesExistingWorkspacesAlone)
{
	UAM_ASSERT(GitAvailableForTests());
	TempDir temp("uam-default-worktree");
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	const fs::path source = temp.root / "source";
	fs::create_directories(source);
	UAM_ASSERT(uam::io::WriteTextFile(source / "app.txt", "source\n"));
	ChatSession chat = ChatDomainService().CreateNewChat("", "codex-cli");
	chat.title = "Fix login";
	chat.workspace_directory = source.string();
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	const std::vector<ChatSession> restored = ChatRepository::LoadLocalChats(app.data_root);
	UAM_ASSERT_EQ(restored.size(), static_cast<std::size_t>(1));
	chat = restored.front();
	UAM_ASSERT_EQ(chat.workspace_isolation_kind, std::string(uam::paths::kPendingGitWorktreeIsolationKind));
	const uam::GitWorktreeService service;
	std::string error;
	ChatSession remote = chat;
	remote.execution_host_id = "remote";
	UAM_ASSERT(service.PrepareForFirstLaunch(app, remote, &error));
	UAM_ASSERT(remote.workspace_worktree_directory.empty());
	ChatSession legacy = chat;
	legacy.workspace_isolation_kind.clear();
	UAM_ASSERT(service.PrepareForFirstLaunch(app, legacy, &error));
	UAM_ASSERT(legacy.workspace_worktree_directory.empty());
	chat.workspace_directory = (temp.root / "missing").string();
	app.chats.push_back(chat);
	UAM_ASSERT(uam::SendAcpPrompt(app, chat.id, "Fix login", &error));
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	const uam::AcpSessionState* failed_session = nullptr;
	do
	{
		(void)uam::FlushPendingChatSaves(app);
		(void)uam::PollAllAcpSessions(app);
		failed_session = uam::FindAcpSessionForChat(app, chat.id);
		if (failed_session != nullptr && failed_session->queue_held && !failed_session->last_error.empty()) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	} while (std::chrono::steady_clock::now() < deadline);
	UAM_ASSERT(failed_session != nullptr && failed_session->queue_held);
	UAM_ASSERT_EQ(failed_session->last_error, std::string("Source workspace is not an available directory."));
	UAM_ASSERT(!failed_session->running && !failed_session->processing && !failed_session->reconnect_pending);
	UAM_ASSERT(app.chats.front().messages.empty());
	UAM_ASSERT_EQ(failed_session->queued_user_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(failed_session->queued_user_prompts.front().text, std::string("Fix login"));
	while (app.pending_chat_save_at_by_chat_id.contains(chat.id) && std::chrono::steady_clock::now() < deadline)
	{
		(void)uam::FlushPendingChatSaves(app);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	UAM_ASSERT(!app.pending_chat_save_at_by_chat_id.contains(chat.id));
	const std::optional<ChatSession> held = ChatRepository::LoadLocalChat(app.data_root, chat.id);
	UAM_ASSERT(held && held->acp_queue_held && held->messages.empty());
	UAM_ASSERT_EQ(held->acp_queued_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(held->acp_queued_prompts.front().text, std::string("Fix login"));
	(void)uam::PollAllAcpSessions(app);
	UAM_ASSERT(failed_session->queue_held && !failed_session->running && !failed_session->reconnect_pending);
	UAM_ASSERT_EQ(failed_session->queued_user_prompts.size(), std::size_t{1});
	app.chats.clear();
	UAM_ASSERT(!service.PrepareForFirstLaunch(app, chat, &error));
	UAM_ASSERT(error.find("Could not create this chat's worktree") != std::string::npos);
	UAM_ASSERT_EQ(chat.workspace_isolation_kind, std::string(uam::paths::kPendingGitWorktreeIsolationKind));
	chat.workspace_directory = source.string();
	UAM_ASSERT(service.PrepareForFirstLaunch(app, chat, &error));
	UAM_ASSERT_EQ(chat.workspace_branch_name, std::string("build/fix-login"));
	const std::string worktree = chat.workspace_worktree_directory;
	UAM_ASSERT(service.PrepareForFirstLaunch(app, chat, &error));
	UAM_ASSERT_EQ(chat.workspace_worktree_directory, worktree);
	UAM_ASSERT_EQ(ReadFile(fs::path(worktree) / "app.txt"), std::string("source\n"));
	UAM_ASSERT(!fs::exists(source / ".git"));
	UAM_ASSERT(service.DiscardChatChanges(app, chat).ok);
	UAM_ASSERT(service.PrepareForFirstLaunch(app, chat, &error));
	UAM_ASSERT(chat.workspace_worktree_directory.empty());
}

UAM_TEST(GitWorktreeServiceIsolatesSvnAndPlainFoldersWithManagedLocalGit)
{
	const ScopedCleanSvnStatus clean_svn;
	if (!GitAvailableForTests())
	{
		return;
	}

	TempDir temp("uam-managed-git-worktree");
	const fs::path source = temp.root / "source";
	fs::create_directories(source / ".svn");
	fs::create_directories(source / ".UAM");
	UAM_ASSERT(uam::io::WriteTextFile(source / "app.txt", "local baseline edit\n"));
	UAM_ASSERT(uam::io::WriteTextFile(source / "unrelated.txt", "original\n"));
	UAM_ASSERT(uam::io::WriteTextFile(source / ".svn" / "entries", "svn metadata\n"));
	UAM_ASSERT(uam::io::WriteTextFile(source / ".UAM" / "memory.md", "private UAM data\n"));

	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatFolder folder;
	folder.id = "folder";
	folder.directory = source.string();
	app.folders.push_back(folder);
	ChatSession chat = ChatDomainService().CreateNewChat(folder.id, "codex-cli");
	chat.id = "chat-managed-worktree";
	chat.workspace_directory = source.string();
	app.chats.push_back(chat);

	uam::GitWorktreeService service;
	uam::GitWorktreeOperationResult created = service.CreateForChat(app, app.chats.front());
	UAM_ASSERT(created.ok);
	UAM_ASSERT(created.status.managed_repository);
	UAM_ASSERT(created.status.is_svn_workspace);
	const fs::path svn_worktree = app.chats.front().workspace_worktree_directory;
	const fs::path svn_repository = svn_worktree.parent_path().parent_path() / ("managed-repository-" + uam::hashing::Hex64(uam::hashing::Fnv1a64(app.chats.front().id)));
	UAM_ASSERT_EQ(ReadFile(svn_worktree / "app.txt"), std::string("local baseline edit\n"));
	UAM_ASSERT(!fs::exists(svn_worktree / ".svn"));
	UAM_ASSERT(!fs::exists(svn_worktree / ".UAM"));
	const ProcessExecutionResult remotes = PlatformServicesFactory::Instance().process_service.ExecuteCommand("git -C " + ShellQuoteForTest(svn_worktree.string()) + " remote", 120000);
	UAM_ASSERT(remotes.ok && remotes.exit_code == 0);
	UAM_ASSERT(uam::strings::IsBlank(remotes.output));
	// The managed Git layer provides diffs and rollback while SVN metadata stays in the source.
	ChatSession& isolated_svn = app.chats.front();
	isolated_svn.messages.push_back(Message{MessageRole::User, "Update app"});
	isolated_svn.messages.push_back(Message{MessageRole::Assistant, "Updated"});
	UAM_ASSERT(uam::io::WriteTextFile(svn_worktree / "app.txt", "checkpoint change\n"));
	const uam::GitTurnCheckpointResult checkpoint = service.CreateTurnCheckpoint(app, isolated_svn, 1);
	UAM_ASSERT(checkpoint.ok && checkpoint.changed);
	const uam::GitTurnCheckpointResult preview = service.PreviewTurnRollback(app, isolated_svn, 1);
	UAM_ASSERT(preview.ok && preview.diff.find("app.txt") != std::string::npos);
	UAM_ASSERT(service.RollbackTurn(app, isolated_svn, 1).ok);
	UAM_ASSERT_EQ(ReadFile(svn_worktree / "app.txt"), std::string("local baseline edit\n"));
	UAM_ASSERT_EQ(ReadFile(source / ".svn" / "entries"), std::string("svn metadata\n"));
	ChatSession concurrent = ChatDomainService().CreateNewChat(folder.id, "codex-cli");
	concurrent.id = "chat-managed-concurrent";
	concurrent.workspace_directory = source.string();
	app.chats.push_back(std::move(concurrent));
	UAM_ASSERT(service.CreateForChat(app, app.chats.back()).ok);
	const fs::path concurrent_worktree = app.chats.back().workspace_worktree_directory;
	const fs::path concurrent_repository = concurrent_worktree.parent_path().parent_path() / ("managed-repository-" + uam::hashing::Hex64(uam::hashing::Fnv1a64(app.chats.back().id)));
	UAM_ASSERT(concurrent_repository != svn_repository);

	UAM_ASSERT(uam::io::WriteTextFile(svn_worktree / "app.txt", "discarded change\n"));
	UAM_ASSERT(service.DiscardChatChanges(app, app.chats.front()).ok);
	UAM_ASSERT_EQ(ReadFile(source / "app.txt"), std::string("local baseline edit\n"));
	UAM_ASSERT_EQ(ReadFile(source / "unrelated.txt"), std::string("original\n"));
	UAM_ASSERT_EQ(ReadFile(source / ".svn" / "entries"), std::string("svn metadata\n"));
	UAM_ASSERT(!fs::exists(svn_worktree));
	UAM_ASSERT(!fs::exists(svn_repository));
	UAM_ASSERT(fs::exists(concurrent_worktree));
	UAM_ASSERT(fs::exists(concurrent_repository));
	UAM_ASSERT(service.DiscardChatChanges(app, app.chats.back()).ok);
	UAM_ASSERT(!fs::exists(concurrent_worktree));
	UAM_ASSERT(!fs::exists(concurrent_repository));

	std::error_code ec;
	fs::remove_all(source / ".svn", ec);
	UAM_ASSERT(!ec);
	created = service.CreateForChat(app, app.chats.front());
	UAM_ASSERT(created.ok);
	UAM_ASSERT(created.status.managed_repository);
	UAM_ASSERT(!created.status.is_svn_workspace);
	const fs::path plain_worktree = app.chats.front().workspace_worktree_directory;
	const fs::path plain_repository = plain_worktree.parent_path().parent_path() / ("managed-repository-" + uam::hashing::Hex64(uam::hashing::Fnv1a64(app.chats.front().id)));
	UAM_ASSERT(uam::io::WriteTextFile(plain_worktree / "app.txt", "ported committed change\n"));
	UAM_ASSERT(uam::io::WriteTextFile(plain_worktree / "new.txt", "new file\n"));
	UAM_ASSERT(RunGitForTest(plain_worktree, "add -A"));
	UAM_ASSERT(RunGitForTest(plain_worktree, "commit -m " + ShellQuoteForTest("agent commit")));
	UAM_ASSERT(uam::io::WriteTextFile(source / "unrelated.txt", "unrelated source change\n"));
	uam::GitWorktreeOperationResult ported = service.PortChatChanges(app, app.chats.front());
	UAM_ASSERT(ported.ok);
	UAM_ASSERT_EQ(ReadFile(source / "app.txt"), std::string("ported committed change\n"));
	UAM_ASSERT_EQ(ReadFile(source / "new.txt"), std::string("new file\n"));
	UAM_ASSERT_EQ(ReadFile(source / "unrelated.txt"), std::string("unrelated source change\n"));
	UAM_ASSERT(!fs::exists(plain_worktree));
	UAM_ASSERT(!fs::exists(plain_repository));

	created = service.CreateForChat(app, app.chats.front());
	UAM_ASSERT(created.ok);
	const fs::path conflict_worktree = app.chats.front().workspace_worktree_directory;
	UAM_ASSERT(uam::io::WriteTextFile(conflict_worktree / "app.txt", "agent version\n"));
	UAM_ASSERT(uam::io::WriteTextFile(source / "app.txt", "source version\n"));
	const uam::GitWorktreeOperationResult conflicted = service.PortChatChanges(app, app.chats.front());
	UAM_ASSERT(!conflicted.ok);
	UAM_ASSERT(conflicted.message.find("Source changed after the isolation baseline") != std::string::npos);
	UAM_ASSERT_EQ(ReadFile(source / "app.txt"), std::string("source version\n"));
	UAM_ASSERT(fs::exists(conflict_worktree));
	UAM_ASSERT(service.DiscardChatChanges(app, app.chats.front()).ok);
}

UAM_TEST(VcsCommitServiceDetectsGitSvnBothAndNone)
{
	TempDir temp("uam-vcs-detect");
	uam::AppState app;
	app.data_root = temp.root / "data";
	ChatSession chat;
	chat.id = "chat-vcs";

	UAM_ASSERT_EQ(uam::VcsTypeFromString(" SVN "), uam::VcsType::Svn);
	UAM_ASSERT_EQ(uam::VcsTypeFromString("Git"), uam::VcsType::Git);
	UAM_ASSERT_EQ(uam::VcsTypeFromString("unknown"), uam::VcsType::Git);

	chat.workspace_directory = (temp.root / "none").string();
	fs::create_directories(chat.workspace_directory);
	uam::VcsCommitStatus none_status = uam::VcsCommitService().Status(app, chat);
	UAM_ASSERT(!none_status.available);
	UAM_ASSERT_EQ(none_status.warning, std::string("No Git or SVN repository detected for this workspace."));

	chat.workspace_directory = (temp.root / "svn").string();
	fs::create_directories(fs::path(chat.workspace_directory) / ".svn");
	uam::VcsCommitStatus svn_status = uam::VcsCommitService().Status(app, chat, uam::VcsType::Svn);
	UAM_ASSERT(svn_status.available);
	UAM_ASSERT(uam::ranges::Contains(svn_status.vcs_types, uam::VcsType::Svn));

	if (!GitAvailableForTests())
	{
		return;
	}

	const fs::path repo = temp.root / "repo";
	fs::create_directories(repo);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repo.string())));
	chat.workspace_directory = repo.string();
	uam::VcsCommitStatus git_status = uam::VcsCommitService().Status(app, chat);
	UAM_ASSERT(git_status.available);
	UAM_ASSERT(uam::ranges::Contains(git_status.vcs_types, uam::VcsType::Git));

	fs::create_directories(repo / ".svn");
	uam::VcsCommitStatus both_status = uam::VcsCommitService().Status(app, chat);
	UAM_ASSERT(both_status.available);
	UAM_ASSERT_EQ(both_status.active_vcs_type, uam::VcsType::Git);
	UAM_ASSERT(uam::ranges::Contains(both_status.vcs_types, uam::VcsType::Git));
	UAM_ASSERT(uam::ranges::Contains(both_status.vcs_types, uam::VcsType::Svn));
}

UAM_TEST(VcsCommitServiceUsesResolvedWorkspaceForGitStatusDiffAndCommit)
{
	if (!GitAvailableForTests())
	{
		return;
	}

	TempDir temp("uam-vcs-git");
	const fs::path source = temp.root / uam::paths::PathFromUtf8("source-\xE2\x98\x83");
	const fs::path worktree = temp.root / uam::paths::PathFromUtf8("worktree-\xC3\xA9");
	fs::create_directories(source);
	fs::create_directories(worktree);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(uam::paths::Utf8PathString(source))));
	UAM_ASSERT(RunGitForTest(source, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(source, "config user.name UAM"));
	UAM_ASSERT(uam::io::WriteTextFile(source / "app.txt", "source\n"));
	UAM_ASSERT(RunGitForTest(source, "add app.txt"));
	UAM_ASSERT(RunGitForTest(source, "commit -m initial"));
	UAM_ASSERT(RunTestCommand("git clone " + ShellQuoteForTest(uam::paths::Utf8PathString(source)) + " " + ShellQuoteForTest(uam::paths::Utf8PathString(worktree))));
	UAM_ASSERT(RunGitForTest(worktree, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(worktree, "config user.name UAM"));

	uam::AppState app;
	app.data_root = temp.root / "data";
	ChatSession chat;
	chat.id = "chat-vcs-worktree";
	chat.workspace_directory = uam::paths::Utf8PathString(source);
	chat.workspace_isolation_kind = "gitWorktree";
	chat.workspace_worktree_directory = uam::paths::Utf8PathString(worktree);
	UAM_ASSERT(uam::io::WriteTextFile(worktree / "app.txt", "worktree\n"));

	uam::VcsCommitService service;
	uam::VcsCommitStatus status = service.Status(app, chat);
	UAM_ASSERT(status.available);
	UAM_ASSERT_EQ(uam::paths::PathFromUtf8(status.workspace_directory), uam::paths::AbsolutePathNoThrow(worktree));
	UAM_ASSERT_EQ(status.changed_files.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(status.changed_files.front().additions, 1);
	UAM_ASSERT_EQ(status.changed_files.front().deletions, 1);
	UAM_ASSERT(!status.changed_files.front().binary);

	std::string error = "stale error";
	const std::string diff = service.Diff(app, chat, " app.txt ", uam::VcsType::Git, &error);
	UAM_ASSERT(error.empty());
	UAM_ASSERT(diff.find("worktree") != std::string::npos);

	error = "stale error";
	UAM_ASSERT(service.Diff(app, chat, "   ", uam::VcsType::Git, &error).empty());
	UAM_ASSERT_EQ(error, std::string("No file selected."));

	uam::VcsCommitResult committed = service.Commit(app, chat, uam::VcsType::Git, "worktree commit", {" app.txt "});
	UAM_ASSERT(committed.ok);
	UAM_ASSERT(RunGitForTest(worktree, "log --oneline --grep " + ShellQuoteForTest("worktree commit")));
	UAM_ASSERT_EQ(ReadFile(source / "app.txt"), std::string("source\n"));
}

UAM_TEST(VcsCommitServiceRestoresTheGitIndexWhenCommitFails)
{
	if (!GitAvailableForTests()) return;

	TempDir temp("uam-vcs-index-rollback");
	const fs::path workspace = temp.root / "repo";
	fs::create_directories(workspace);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(uam::paths::Utf8PathString(workspace))));
	UAM_ASSERT(RunGitForTest(workspace, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(workspace, "config user.name UAM"));
	UAM_ASSERT(uam::io::WriteTextFile(workspace / "selected.txt", "initial\n"));
	UAM_ASSERT(uam::io::WriteTextFile(workspace / "staged.txt", "initial\n"));
	UAM_ASSERT(RunGitForTest(workspace, "add -- selected.txt staged.txt"));
	UAM_ASSERT(RunGitForTest(workspace, "commit -m initial"));
	UAM_ASSERT(uam::io::WriteTextFile(workspace / "selected.txt", "selected change\n"));
	UAM_ASSERT(uam::io::WriteTextFile(workspace / "staged.txt", "staged change\n"));
	UAM_ASSERT(RunGitForTest(workspace, "add -- staged.txt"));
	const ProcessExecutionResult staged_before_result = PlatformServicesFactory::Instance().process_service.ExecuteCommand("git -C " + ShellQuoteForTest(uam::paths::Utf8PathString(workspace)) + " diff --cached --binary", 120000);
	UAM_ASSERT(staged_before_result.ok);
	const std::string staged_before = staged_before_result.output;
	UAM_ASSERT(RunGitForTest(workspace, "config user.name ''"));

	uam::AppState app;
	ChatSession chat;
	chat.workspace_directory = uam::paths::Utf8PathString(workspace);
	const uam::VcsCommitResult result = uam::VcsCommitService().Commit(app, chat, uam::VcsType::Git, "must fail", {"selected.txt"});
	UAM_ASSERT(!result.ok);
	const ProcessExecutionResult staged_after_result = PlatformServicesFactory::Instance().process_service.ExecuteCommand("git -C " + ShellQuoteForTest(uam::paths::Utf8PathString(workspace)) + " diff --cached --binary", 120000);
	UAM_ASSERT(staged_after_result.ok);
	const std::string staged_after = staged_after_result.output;
	UAM_ASSERT_EQ(staged_after, staged_before);
}

UAM_TEST(VcsCommitServiceTreatsSelectedGitFilesAsLiteralPathspecs)
{
	if (!GitAvailableForTests()) return;

	TempDir temp("uam-vcs-literal-pathspec");
	const fs::path workspace = temp.root / "repo";
	fs::create_directories(workspace);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(uam::paths::Utf8PathString(workspace))));
	UAM_ASSERT(RunGitForTest(workspace, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(workspace, "config user.name UAM"));
	UAM_ASSERT(RunGitForTest(workspace, "config core.autocrlf false"));
	UAM_ASSERT(uam::io::WriteTextFile(workspace / "file[1].txt", "initial\n"));
	UAM_ASSERT(uam::io::WriteTextFile(workspace / "file1.txt", "initial\n"));
	UAM_ASSERT(RunGitForTest(workspace, "add -- ."));
	UAM_ASSERT(RunGitForTest(workspace, "commit -m initial"));
	UAM_ASSERT(uam::io::WriteTextFile(workspace / "file[1].txt", "selected\n"));
	UAM_ASSERT(uam::io::WriteTextFile(workspace / "file1.txt", "unselected\n"));

	uam::AppState app;
	ChatSession chat;
	chat.workspace_directory = uam::paths::Utf8PathString(workspace);
	const uam::VcsCommitResult result = uam::VcsCommitService().Commit(app, chat, uam::VcsType::Git, "literal path", {"file[1].txt"});
	UAM_ASSERT(result.ok);
	const ProcessExecutionResult remaining_result = PlatformServicesFactory::Instance().process_service.ExecuteCommand("git -C " + ShellQuoteForTest(uam::paths::Utf8PathString(workspace)) + " diff --name-only HEAD", 120000);
	UAM_ASSERT(remaining_result.ok);
	const std::string remaining = remaining_result.output;
	UAM_ASSERT_EQ(uam::strings::Trim(remaining), std::string("file1.txt"));
}

UAM_TEST(NativeSpeechOutputBoundsAndQueuedCancellationNeverActivateAudio)
{
	uam::SpeechOutputOptions options;
	options.speech_id = "speech-fixture";
	options.text = "Synthetic text that must never be spoken.";
	std::string error;
	UAM_ASSERT(uam::ValidateSpeechOutputOptions(options, &error));
	options.rate = std::numeric_limits<double>::quiet_NaN();
	UAM_ASSERT(!uam::ValidateSpeechOutputOptions(options, &error));
	options.rate = 1.0;
	options.volume = 1.1;
	UAM_ASSERT(!uam::ValidateSpeechOutputOptions(options, &error));
	options.volume = 1.0;
	options.text.assign(65537, 'x');
	UAM_ASSERT(!uam::ValidateSpeechOutputOptions(options, &error));
	options.text = "Synthetic text that must never be spoken.";
	const std::uint64_t request = uam::PrepareSpeechOutputRequest(options.speech_id);
	UAM_ASSERT(uam::GetSpeechOutputState().speaking);
	UAM_ASSERT(!uam::StopSpeechOutput("unrelated-playback"));
	UAM_ASSERT(uam::GetSpeechOutputState().speaking);
	UAM_ASSERT(uam::StopSpeechOutput(options.speech_id));
	UAM_ASSERT(!uam::GetSpeechOutputState().speaking);
	UAM_ASSERT(!uam::StartSpeechOutput(options, request, &error));
	UAM_ASSERT(error.find("canceled") != std::string::npos);
	UAM_ASSERT(!uam::GetSpeechOutputState().speaking);
	const std::uint64_t superseded = uam::PrepareSpeechOutputRequest(options.speech_id);
	const std::uint64_t replacement = uam::PrepareSpeechOutputRequest(options.speech_id);
	UAM_ASSERT(replacement != superseded);
	UAM_ASSERT(!uam::StartSpeechOutput(options, superseded, &error));
	UAM_ASSERT(uam::GetSpeechOutputState().speaking);
	UAM_ASSERT(uam::StopSpeechOutput(options.speech_id));
	UAM_ASSERT(!uam::StartSpeechOutput(options, replacement, &error));
	UAM_ASSERT(!uam::GetSpeechOutputState().speaking);
	const std::uint64_t invalid = uam::PrepareSpeechOutputRequest(options.speech_id);
	options.volume = 2.0;
	UAM_ASSERT(!uam::StartSpeechOutput(options, invalid, &error));
	UAM_ASSERT(!uam::GetSpeechOutputState().speaking);
	const std::uint64_t newer = uam::PrepareSpeechOutputRequest(options.speech_id);
	UAM_ASSERT(!uam::StartSpeechOutput(options, invalid, &error));
	UAM_ASSERT(uam::GetSpeechOutputState().speaking);
	UAM_ASSERT(newer != invalid);
	UAM_ASSERT(uam::StopSpeechOutput(options.speech_id));
}

UAM_TEST(VcsChatBaselinePersistsAllContentWithoutChangingSourceIndexOrBranch)
{
	if (!GitAvailableForTests()) return;
	TempDir temp("uam-durable-chat-baseline");
	const fs::path repo = temp.root / "repo";
	fs::create_directories(repo);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repo.string())));
	UAM_ASSERT(RunGitForTest(repo, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(repo, "config user.name UAM"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "staged.txt", "initial\n"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "unstaged.txt", "initial\n"));
	UAM_ASSERT(RunGitForTest(repo, "add -A"));
	UAM_ASSERT(RunGitForTest(repo, "commit -m initial"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "staged.txt", "prior staged\n"));
	UAM_ASSERT(RunGitForTest(repo, "add staged.txt"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "unstaged.txt", "prior unstaged\n"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "untracked.txt", "prior untracked\n"));
	const auto git_output = [&](const std::string& args)
	{
		const ProcessExecutionResult result = PlatformServicesFactory::Instance().process_service.ExecuteCommand("git -C " + ShellQuoteForTest(repo.string()) + " " + args, 120000);
		UAM_ASSERT(result.ok && result.exit_code == 0);
		return result.output;
	};
	UAM_ASSERT(uam::io::WriteTextFile(repo / ".gitignore", "ignored.txt\n"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "ignored.txt", "tracked ignored content\n"));
	UAM_ASSERT(RunGitForTest(repo, "add -f ignored.txt"));
	const std::string initial_head = git_output("rev-parse HEAD");
	const std::string initial_index = git_output("diff --cached --binary");
	uam::AppState app;
	app.data_root = temp.root / "data";
	ChatSession chat;
	chat.id = "baseline-chat";
	chat.workspace_directory = repo.string();
	const uam::VcsCommitService service;
	std::string error;
	ChatSession existing = chat;
	existing.id = "existing-chat";
	existing.persisted_message_count = 1;
	UAM_ASSERT(!service.CaptureChatBaseline(app, existing, &error));
	UAM_ASSERT(error.find("existing conversation") != std::string::npos);
	const uam::VcsCommitStatus unavailable = service.Status(app, existing, uam::VcsType::Git, true, "chat");
	UAM_ASSERT(!unavailable.error.empty());
	UAM_ASSERT(unavailable.changed_files.empty());
	UAM_ASSERT_EQ(git_output("rev-parse HEAD"), initial_head);
	UAM_ASSERT_EQ(git_output("diff --cached --binary"), initial_index);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	uam::ChatBaselineReadiness readiness = uam::PrepareChatRepositoryBaseline(app, chat, &error);
	while (readiness == uam::ChatBaselineReadiness::Pending && std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		readiness = uam::PrepareChatRepositoryBaseline(app, chat, &error);
	}
	UAM_ASSERT(readiness == uam::ChatBaselineReadiness::Ready);
	UAM_ASSERT(service.CaptureChatBaseline(app, chat, &error));
	UAM_ASSERT(error.empty());
	ChatSession canceled_chat = chat;
	canceled_chat.id = "canceled-before-capture";
	std::stop_source canceled;
	canceled.request_stop();
	UAM_ASSERT(!service.CaptureChatBaseline(app, canceled_chat, &error, canceled.get_token()));
	UAM_ASSERT(!service.Status(app, canceled_chat, uam::VcsType::Git, true, "chat").error.empty());
	const uam::VcsCommitStatus before = service.Status(app, chat, uam::VcsType::Git, true, "chat");
	UAM_ASSERT(before.error.empty());
	UAM_ASSERT(before.changed_files.empty());
	UAM_ASSERT_EQ(git_output("rev-parse HEAD"), initial_head);
	UAM_ASSERT_EQ(git_output("diff --cached --binary"), initial_index);
	UAM_ASSERT(uam::io::WriteTextFile(repo / "untracked.txt", "chat replacement\n"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "unstaged.txt", "chat replacement\n"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "new.txt", "chat addition\n"));
	UAM_ASSERT(service.CaptureChatBaseline(app, chat, &error));
	uam::AppState restarted;
	restarted.data_root = app.data_root;
	const uam::VcsCommitStatus changed = service.Status(restarted, chat, uam::VcsType::Git, true, "chat");
	UAM_ASSERT(changed.error.empty());
	UAM_ASSERT_EQ(changed.changed_files.size(), static_cast<std::size_t>(3));
	UAM_ASSERT(!std::ranges::any_of(changed.changed_files, [](const uam::VcsChangedFile& file) { return file.path == "staged.txt"; }));
	const std::string patch = service.Diff(restarted, chat, "untracked.txt", uam::VcsType::Git, &error, "chat");
	UAM_ASSERT(error.empty());
	UAM_ASSERT(patch.find("-prior untracked") != std::string::npos);
	UAM_ASSERT(patch.find("+chat replacement") != std::string::npos);
	UAM_ASSERT_EQ(git_output("diff --cached --binary"), initial_index);
	UAM_ASSERT_EQ(git_output("rev-parse HEAD"), initial_head);
	UAM_ASSERT(RunGitForTest(repo, "mv staged.txt renamed.txt"));
	const uam::VcsCommitStatus renamed = service.Status(app, chat, uam::VcsType::Git, true, "chat");
	UAM_ASSERT(renamed.error.empty());
	const auto rename = std::ranges::find(renamed.changed_files, std::string("renamed.txt"), &uam::VcsChangedFile::path);
	UAM_ASSERT(rename != renamed.changed_files.end());
	UAM_ASSERT_EQ(rename->previous_path, std::string("staged.txt"));
	const std::string rename_patch = service.Diff(app, chat, "renamed.txt", uam::VcsType::Git, &error, "chat");
	UAM_ASSERT(rename_patch.find("rename from staged.txt") != std::string::npos);
	UAM_ASSERT(rename_patch.find("rename to renamed.txt") != std::string::npos);
	UAM_ASSERT(service.Diff(app, chat, "../outside.txt", uam::VcsType::Git, &error, "chat").empty());
	UAM_ASSERT(!error.empty());
	chat.workspace_directory = temp.root.string();
	UAM_ASSERT(!service.CaptureChatBaseline(app, chat, &error));
}

UAM_TEST(AcpInitialRepositoryBaselineQueuesDurablyBeforeProviderLaunch)
{
	if (!GitAvailableForTests()) return;
	TempDir temp("uam-acp-initial-baseline");
	const fs::path repo = temp.root / "repo";
	fs::create_directories(repo);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repo.string())));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "prior.txt", "preexisting\n"));
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession chat;
	chat.id = "queued-initial-baseline";
	chat.provider_id = uam::provider_ids::kGeminiCli;
	chat.workspace_directory = repo.string();
	app.chats.push_back(chat);
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(app, chat.id, "First original prompt", {}, {}, false, &error));
	uam::FlushPendingChatSaves(app, true); // Idle admission saves are deferred to the app tick.
	UAM_ASSERT(!app.acp_sessions.empty());
	UAM_ASSERT(!app.acp_sessions.front()->running);
	UAM_ASSERT_EQ(app.acp_sessions.front()->queued_user_prompts.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(app.chats.front().messages.empty());
	const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true, &error);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->acp_queued_prompts.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(saved->acp_queued_prompts.front().text, std::string("First original prompt"));
	const auto task = app.repository_baseline_preparations.at(chat.id);
	UAM_ASSERT(uam::WaitForChatRepositoryBaseline(task, {}, &error) == uam::ChatBaselineReadiness::Ready);
	uam::AppState restarted;
	restarted.data_root = app.data_root;
	const uam::VcsCommitService vcs;
	UAM_ASSERT(vcs.Status(restarted, *saved, uam::VcsType::Git, true, "chat").changed_files.empty());
	UAM_ASSERT(uam::io::WriteTextFile(repo / "prior.txt", "provider modification\n"));
	UAM_ASSERT_EQ(vcs.Status(restarted, *saved, uam::VcsType::Git, true, "chat").changed_files.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(!fs::exists(repo / ".git" / "index"));
	const fs::path moved = temp.root / "moved";
	fs::create_directories(moved);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(moved.string())));
	ChatSession changed_scope = *saved;
	changed_scope.workspace_directory = moved.string();
	UAM_ASSERT(uam::PrepareChatRepositoryBaseline(restarted, changed_scope, &error) == uam::ChatBaselineReadiness::Pending);
	UAM_ASSERT(uam::WaitForChatRepositoryBaseline(restarted.repository_baseline_preparations.at(chat.id), {}, &error) == uam::ChatBaselineReadiness::Unavailable);
	UAM_ASSERT(!vcs.Status(restarted, changed_scope, uam::VcsType::Git, true, "chat").error.empty());
	UAM_ASSERT_EQ(vcs.Status(restarted, *saved, uam::VcsType::Git, true, "chat").changed_files.size(), static_cast<std::size_t>(1));
}

UAM_TEST(AcpRepositoryBaselineFailureRetriesTheExistingDurableQueueOnce)
{
	if (!GitAvailableForTests()) return;
	TempDir temp("uam-acp-baseline-retry");
	const fs::path repo = temp.root / "repo";
	fs::create_directories(repo);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repo.string())));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "prior.txt", "preexisting\n"));
	uam::AppState app;
	app.data_root = temp.root / "data";
	fs::create_directories(app.data_root);
	UAM_ASSERT(uam::io::WriteTextFile(app.data_root / "repository-review", "storage unavailable"));
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ChatSession chat;
	chat.id = "chat-failed-baseline-retry"; // A new-chat id, so restore does not adopt it as a native session id.
	chat.provider_id = uam::provider_ids::kGeminiCli;
	chat.workspace_directory = repo.string();
	app.chats.push_back(chat);
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(app, chat.id, "Keep this prompt once", {}, {}, false, &error));
	uam::FlushPendingChatSaves(app, true); // Idle admission saves are deferred to the app tick.
	UAM_ASSERT(uam::WaitForChatRepositoryBaseline(app.repository_baseline_preparations.at(chat.id), {}, &error) == uam::ChatBaselineReadiness::Failed);
	UAM_ASSERT(uam::PrepareChatRepositoryBaseline(app, app.chats.front(), &error) == uam::ChatBaselineReadiness::Failed);
	UAM_ASSERT_EQ(app.acp_sessions.front()->queued_user_prompts.size(), static_cast<std::size_t>(1));
	// The app tick holds the input and stops reconnecting instead of silently retrying the failed baseline.
	const auto settle_hold = [&]()
	{
		(void)uam::PollAllAcpSessions(app);
		uam::FlushPendingChatSaves(app, true);
	};
	const auto retry_allowed = [](const uam::AppState& state)
	{
		const nlohmann::json projected = uam::StateSerializer::Serialize(state).at("chats").at(0).at("acpSession");
		UAM_ASSERT(projected.at("pendingDirectPrompts").empty() && projected.at("unsentDirectPrompts").size() == 1);
		return projected.at("unsentDirectPrompts").at(0).value("retryAllowed", false);
	};
	settle_hold();
	const uam::AcpSessionState& held = *app.acp_sessions.front();
	UAM_ASSERT(held.queue_held && app.chats.front().acp_queue_held && !held.reconnect_pending && !held.running);
	UAM_ASSERT(!held.last_error.empty());
	UAM_ASSERT_EQ(app.chats.front().acp_queued_prompts.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(app.chats.front().acp_queued_prompts.front().repository_baseline_hold);
	UAM_ASSERT(retry_allowed(app));
	// The hold reason is saved, so a restart keeps the exact receipt retryable.
	const fs::path restart_root = temp.root / "restart-data";
	fs::copy(app.data_root, restart_root, fs::copy_options::recursive);
	UAM_ASSERT(fs::remove(restart_root / "repository-review"));
	const auto restored = ChatRepository::LoadLocalChat(restart_root, chat.id, true, &error);
	UAM_ASSERT(restored && restored->acp_queue_held && restored->acp_queued_prompts.size() == 1 &&
	           restored->acp_queued_prompts.front().repository_baseline_hold);
	const uam::AcpQueuedUserPromptState receipt = app.chats.front().acp_queued_prompts.front();
	// A stale identity is refused without touching the hold.
	const std::string held_error = held.last_error;
	UAM_ASSERT(!uam::RetryQueuedAcpPrompt(app, chat.id, {receipt.id, receipt.revision + 1}, &error));
	UAM_ASSERT_EQ(error, std::string("The retained message changed or its delivery is unresolved. Your input is kept."));
	UAM_ASSERT(held.queue_held && app.chats.front().acp_queue_held && !held.reconnect_pending && held.last_error == held_error);
	UAM_ASSERT(app.chats.front().acp_queued_prompts.size() == 1 && app.chats.front().acp_queued_prompts.front() == receipt);
	// Retrying while storage still fails re-holds the same input with a fresh marker once the baseline fails again.
	error.clear();
	UAM_ASSERT(uam::RetryQueuedAcpPrompt(app, chat.id, {receipt.id, receipt.revision}, &error)); // Fresh capture is pending.
	UAM_ASSERT(!held.queue_held && held.reconnect_pending && !app.chats.front().acp_queued_prompts.front().repository_baseline_hold);
	UAM_ASSERT(uam::WaitForChatRepositoryBaseline(app.repository_baseline_preparations.at(chat.id), {}, &error) == uam::ChatBaselineReadiness::Failed);
	settle_hold();
	UAM_ASSERT(held.queue_held && app.chats.front().acp_queue_held && !held.reconnect_pending && !held.last_error.empty());
	UAM_ASSERT(app.chats.front().acp_queued_prompts.size() == 1 && app.chats.front().acp_queued_prompts.front().repository_baseline_hold);
	UAM_ASSERT(retry_allowed(app));
	UAM_ASSERT(fs::remove(app.data_root / "repository-review"));
	(void)uam::PollAllAcpSessions(app);
	UAM_ASSERT(held.queue_held && !held.reconnect_pending);
	UAM_ASSERT(uam::PrepareChatRepositoryBaseline(app, app.chats.front(), &error) == uam::ChatBaselineReadiness::Failed);
	error.clear();
	UAM_ASSERT(uam::RetryQueuedAcpPrompt(app, chat.id, {receipt.id, receipt.revision}, &error));
	UAM_ASSERT(error.empty() && !held.queue_held && held.reconnect_pending);
	UAM_ASSERT(!app.chats.front().acp_queued_prompts.front().repository_baseline_hold);
	UAM_ASSERT(!uam::RetryQueuedAcpPrompt(app, chat.id, {receipt.id, receipt.revision}, &error));
	UAM_ASSERT(!held.queue_held && held.reconnect_pending);
	UAM_ASSERT(uam::WaitForChatRepositoryBaseline(app.repository_baseline_preparations.at(chat.id), {}, &error) == uam::ChatBaselineReadiness::Ready);
	UAM_ASSERT(!app.acp_sessions.front()->running);
	UAM_ASSERT_EQ(app.acp_sessions.front()->queued_user_prompts.size(), static_cast<std::size_t>(1));
	const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true, &error);
	UAM_ASSERT(saved.has_value());
	UAM_ASSERT_EQ(saved->acp_queued_prompts.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(saved->acp_queued_prompts.front().text, std::string("Keep this prompt once"));
	// Restored state: no session yet; the saved hold alone authorizes the same exact Retry.
	uam::AppState restarted;
	restarted.data_root = restart_root;
	restarted.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	restarted.chats.push_back(*restored);
	UAM_ASSERT(restarted.acp_sessions.empty());
	UAM_ASSERT(retry_allowed(restarted));
	error.clear();
	UAM_ASSERT(uam::RetryQueuedAcpPrompt(restarted, chat.id, {receipt.id, receipt.revision}, &error));
	UAM_ASSERT(error.empty() && !restarted.acp_sessions.front()->queue_held && restarted.acp_sessions.front()->reconnect_pending);
	UAM_ASSERT_EQ(restarted.acp_sessions.front()->queued_user_prompts.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(uam::WaitForChatRepositoryBaseline(restarted.repository_baseline_preparations.at(chat.id), {}, &error) == uam::ChatBaselineReadiness::Ready);
	UAM_ASSERT(!restarted.acp_sessions.front()->running);
}

UAM_TEST(AcpRepositoryBaselineHoldRetryIsOfferedOnlyForTheLoneMarkedReceipt)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "baseline-hold-projection";
	chat.provider_id = uam::provider_ids::kGeminiCli;
	chat.acp_queue_held = true;
	uam::AcpQueuedUserPromptState marked;
	marked.id = "marked";
	marked.revision = 1;
	marked.text = "Held by a failed baseline";
	marked.direct_admission = true;
	marked.repository_baseline_hold = true;
	uam::AcpQueuedUserPromptState unrelated = marked;
	unrelated.id = "unrelated";
	unrelated.repository_baseline_hold = false;
	const auto retry_flags = [&](std::vector<uam::AcpQueuedUserPromptState> outbox)
	{
		app.chats = {chat};
		app.chats.front().acp_queued_prompts = std::move(outbox);
		std::vector<bool> flags;
		const nlohmann::json state = uam::StateSerializer::Serialize(app);
		for (const auto& receipt : state.at("chats").at(0).at("acpSession").at("unsentDirectPrompts"))
			flags.push_back(receipt.value("retryAllowed", false));
		return flags;
	};
	UAM_ASSERT(retry_flags({marked}) == std::vector<bool>{true});
	UAM_ASSERT(uam::AcpBaselineHoldRetryAllowed(app, app.chats.front(), nullptr));
	UAM_ASSERT(retry_flags({unrelated}) == std::vector<bool>{false});
	UAM_ASSERT(retry_flags({marked, unrelated}) == (std::vector<bool>{false, false}));
	UAM_ASSERT(!uam::AcpBaselineHoldRetryAllowed(app, app.chats.front(), nullptr));
	std::string error;
	UAM_ASSERT(!uam::RetryQueuedAcpPrompt(app, chat.id, {marked.id, marked.revision}, &error));
	UAM_ASSERT(app.acp_sessions.empty() || app.acp_sessions.front()->queue_held);
	chat.acp_queue_held = false;
	UAM_ASSERT(retry_flags({marked}) == std::vector<bool>{false});
}

UAM_TEST(VcsChatComparisonExcludesUnchangedPreexistingIndexAndWorkingChanges)
{
	if (!GitAvailableForTests()) return;
	TempDir temp("uam-vcs-chat-comparison");
	const fs::path repo = temp.root / "repo";
	fs::create_directories(repo);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repo.string())));
	UAM_ASSERT(RunGitForTest(repo, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(repo, "config user.name UAM"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "staged.txt", "original\n"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "unstaged.txt", "original\n"));
	UAM_ASSERT(RunGitForTest(repo, "add -A"));
	UAM_ASSERT(RunGitForTest(repo, "commit -m initial"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "staged.txt", "existing staged\n"));
	UAM_ASSERT(RunGitForTest(repo, "add staged.txt"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "unstaged.txt", "existing unstaged\n"));
	const ProcessExecutionResult baseline = PlatformServicesFactory::Instance().process_service.ExecuteCommand("git -C " + ShellQuoteForTest(repo.string()) + " stash create", 120000);
	UAM_ASSERT(baseline.ok && baseline.exit_code == 0);
	const std::string ref = uam::strings::Trim(baseline.output);
	UAM_ASSERT(!ref.empty());
	uam::AppState app;
	ChatSession chat;
	chat.workspace_directory = repo.string();
	const uam::VcsCommitService service;
	const uam::VcsCommitStatus unchanged = service.Status(app, chat, uam::VcsType::Git, true, ref);
	UAM_ASSERT(unchanged.error.empty());
	UAM_ASSERT(unchanged.changed_files.empty());
	UAM_ASSERT(uam::io::WriteTextFile(repo / "staged.txt", "chat changed staged\n"));
	UAM_ASSERT(RunGitForTest(repo, "add staged.txt"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "new.txt", "chat added\n"));
	const uam::VcsCommitStatus changed = service.Status(app, chat, uam::VcsType::Git, true, ref);
	UAM_ASSERT_EQ(changed.changed_files.size(), static_cast<std::size_t>(2));
	UAM_ASSERT(std::ranges::any_of(changed.changed_files, [](const uam::VcsChangedFile& file) { return file.path == "staged.txt" && file.staged; }));
	UAM_ASSERT(std::ranges::any_of(changed.changed_files, [](const uam::VcsChangedFile& file) { return file.path == "new.txt"; }));
	std::string error;
	const std::string patch = service.Diff(app, chat, "staged.txt", uam::VcsType::Git, &error, ref);
	UAM_ASSERT(error.empty());
	UAM_ASSERT(patch.find("-existing staged") != std::string::npos);
	UAM_ASSERT(patch.find("+chat changed staged") != std::string::npos);
}

UAM_TEST(VcsCommitServiceFingerprintsTrackedAndUntrackedContent)
{
	if (!GitAvailableForTests())
	{
		return;
	}

	TempDir temp("uam-vcs-content-fingerprint");
	const fs::path repo = temp.root / "repo";
	fs::create_directories(repo);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repo.string())));
	UAM_ASSERT(RunGitForTest(repo, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(repo, "config user.name UAM"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "tracked.txt", "original\n"));
	UAM_ASSERT(RunGitForTest(repo, "add tracked.txt"));
	UAM_ASSERT(RunGitForTest(repo, "commit -m initial"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "tracked.txt", "before!!\n"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "untracked.txt", "before!!\n"));

	uam::AppState app;
	ChatSession chat;
	chat.workspace_directory = repo.string();
	const uam::VcsCommitStatus before = uam::VcsCommitService().Status(app, chat);
	const auto find_file = [](const uam::VcsCommitStatus& status, const std::string& path) -> const uam::VcsChangedFile&
	{
		const auto found = std::find_if(status.changed_files.begin(), status.changed_files.end(), [&](const uam::VcsChangedFile& file) { return file.path == path; });
		UAM_ASSERT(found != status.changed_files.end());
		return *found;
	};
	const uam::VcsChangedFile& tracked_before = find_file(before, "tracked.txt");
	const uam::VcsChangedFile& untracked_before = find_file(before, "untracked.txt");
	UAM_ASSERT(!tracked_before.content_fingerprint.empty());
	UAM_ASSERT(!untracked_before.content_fingerprint.empty());
	const uam::VcsCommitStatus cheap = uam::VcsCommitService().Status(app, chat, uam::VcsType::Git, false);
	UAM_ASSERT(find_file(cheap, "tracked.txt").content_fingerprint.empty());
	UAM_ASSERT(find_file(cheap, "untracked.txt").content_fingerprint.empty());

	UAM_ASSERT(uam::io::WriteTextFile(repo / "tracked.txt", "after!!!\n"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "untracked.txt", "after!!!\n"));
	const uam::VcsCommitStatus after = uam::VcsCommitService().Status(app, chat);
	const uam::VcsChangedFile& tracked_after = find_file(after, "tracked.txt");
	const uam::VcsChangedFile& untracked_after = find_file(after, "untracked.txt");
	UAM_ASSERT_EQ(tracked_before.additions, tracked_after.additions);
	UAM_ASSERT_EQ(tracked_before.deletions, tracked_after.deletions);
	UAM_ASSERT_EQ(untracked_before.additions, untracked_after.additions);
	UAM_ASSERT_EQ(untracked_before.deletions, untracked_after.deletions);
	UAM_ASSERT(tracked_before.content_fingerprint != tracked_after.content_fingerprint);
	UAM_ASSERT(untracked_before.content_fingerprint != untracked_after.content_fingerprint);
}

UAM_TEST(VcsCommitServiceFingerprintsRepoRelativePathsFromSubdirectories)
{
	if (!GitAvailableForTests())
	{
		return;
	}

	TempDir temp("uam-vcs-subdirectory-fingerprint");
	const fs::path repo = temp.root / "repo";
	const fs::path subdir = repo / "subdir";
	fs::create_directories(subdir);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repo.string())));
	UAM_ASSERT(RunGitForTest(repo, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(repo, "config user.name UAM"));
	UAM_ASSERT(uam::io::WriteTextFile(subdir / "tracked.txt", "original\n"));
	UAM_ASSERT(RunGitForTest(repo, "add subdir/tracked.txt"));
	UAM_ASSERT(RunGitForTest(repo, "commit -m initial"));
	UAM_ASSERT(uam::io::WriteTextFile(subdir / "tracked.txt", "before!!\n"));

	uam::AppState app;
	ChatSession chat;
	chat.workspace_directory = subdir.string();
	const uam::VcsChangedFile before = uam::VcsCommitService().Status(app, chat).changed_files.front();
	UAM_ASSERT_EQ(before.path, std::string("subdir/tracked.txt"));
	UAM_ASSERT(!before.content_fingerprint.empty());

	UAM_ASSERT(uam::io::WriteTextFile(subdir / "tracked.txt", "after!!!\n"));
	const uam::VcsChangedFile after = uam::VcsCommitService().Status(app, chat).changed_files.front();
	UAM_ASSERT_EQ(before.additions, after.additions);
	UAM_ASSERT_EQ(before.deletions, after.deletions);
	UAM_ASSERT(before.content_fingerprint != after.content_fingerprint);
}

#if !defined(_WIN32)
UAM_TEST(VcsCommitServicePreservesUntrackedFileNamesContainingRenameSeparator)
{
	if (!GitAvailableForTests())
	{
		return;
	}

	TempDir temp("uam-vcs-arrow-name");
	const fs::path repo = temp.root / "repo";
	fs::create_directories(repo);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repo.string())));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "a -> b.txt", "content\n"));

	uam::AppState app;
	ChatSession chat;
	chat.workspace_directory = repo.string();

	const uam::VcsCommitStatus status = uam::VcsCommitService().Status(app, chat);

	UAM_ASSERT_EQ(status.changed_files.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(status.changed_files.front().path, std::string("a -> b.txt"));
}
#endif

UAM_TEST(VcsCommitServiceDecodesQuotedGitFileNames)
{
	if (!GitAvailableForTests())
	{
		return;
	}

	TempDir temp("uam-vcs-quoted-name");
	const fs::path repo = temp.root / "repo";
	fs::create_directories(repo);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repo.string())));
	UAM_ASSERT(RunGitForTest(repo, "config core.quotePath true"));
	const std::string file_name = "caf\xc3\xa9.txt";
	UAM_ASSERT(uam::io::WriteTextFile(repo / uam::paths::PathFromUtf8(file_name), "content\n"));

	uam::AppState app;
	ChatSession chat;
	chat.workspace_directory = repo.string();

	const uam::VcsCommitStatus status = uam::VcsCommitService().Status(app, chat);

	UAM_ASSERT_EQ(status.changed_files.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(status.changed_files.front().path, file_name);
}

UAM_TEST(VcsCommitServiceShowsDiffForStagedChanges)
{
	if (!GitAvailableForTests())
	{
		return;
	}

	TempDir temp("uam-vcs-staged-diff");
	const fs::path repo = temp.root / "repo";
	fs::create_directories(repo);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repo.string())));
	UAM_ASSERT(RunGitForTest(repo, "config user.email uam@example.test"));
	UAM_ASSERT(RunGitForTest(repo, "config user.name UAM"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "app.txt", "before\n"));
	UAM_ASSERT(RunGitForTest(repo, "add app.txt"));
	UAM_ASSERT(RunGitForTest(repo, "commit -m initial"));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "app.txt", "after\n"));
	UAM_ASSERT(RunGitForTest(repo, "add app.txt"));

	uam::AppState app;
	ChatSession chat;
	chat.workspace_directory = repo.string();
	std::string error;

	const std::string diff = uam::VcsCommitService().Diff(app, chat, "app.txt", uam::VcsType::Git, &error);

	UAM_ASSERT(error.empty());
	UAM_ASSERT(diff.find("after") != std::string::npos);
}

UAM_TEST(VcsCommitServiceShowsDiffForUntrackedFilesFromWorkspaceSubdirectories)
{
	if (!GitAvailableForTests())
	{
		return;
	}

	TempDir temp("uam-vcs-untracked-diff");
	const fs::path repo = temp.root / "repo";
	const fs::path workspace = repo / "nested";
	fs::create_directories(workspace);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repo.string())));
	UAM_ASSERT(uam::io::WriteTextFile(workspace / "new file.txt", "first line\nsecond line\n"));

	uam::AppState app;
	ChatSession chat;
	chat.workspace_directory = workspace.string();
	std::string error = "stale error";

	const std::string diff = uam::VcsCommitService().Diff(app, chat, "nested/new file.txt", uam::VcsType::Git, &error);

	UAM_ASSERT(error.empty());
	UAM_ASSERT(diff.find("new file.txt") != std::string::npos);
	UAM_ASSERT(diff.find("+first line") != std::string::npos);
	UAM_ASSERT(diff.find("+second line") != std::string::npos);
}

UAM_TEST(VcsCommitServiceRejectsBlankCommitMessageSelectionsBeforeWorkerLookup)
{
	uam::AppState app;
	ChatSession chat;
	chat.id = "chat-vcs-message-selection";

	const uam::VcsCommitMessageSuggestion suggestion = uam::VcsCommitService().GenerateMessage(app, chat, uam::VcsType::Git, {"   ", "\t"});

	UAM_ASSERT(!suggestion.ok);
	UAM_ASSERT_EQ(suggestion.error, std::string("Select at least one changed file before generating a commit message."));
}

UAM_TEST(VcsCommitServiceBlocksCopilotWorkerWhileCompatibilityCheckIsPending)
{
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
	if (!GitAvailableForTests())
	{
		return;
	}

	TempDir temp("uam-vcs-copilot-version-pending");
	const fs::path repo = temp.root / "repo";
	fs::create_directories(repo);
	UAM_ASSERT(RunTestCommand("git init " + ShellQuoteForTest(repo.string())));
	UAM_ASSERT(uam::io::WriteTextFile(repo / "app.txt", "change\n"));

	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.runtime_cli_versions_by_provider_id[uam::provider_ids::kCopilotCli] = {};
	ChatSession chat;
	chat.provider_id = uam::provider_ids::kCopilotCli;
	chat.workspace_directory = uam::paths::Utf8PathString(repo);

	const uam::VcsCommitMessageSuggestion suggestion = uam::VcsCommitService().GenerateMessage(app, chat, uam::VcsType::Git, {"app.txt"});

	UAM_ASSERT(!suggestion.ok);
	UAM_ASSERT(suggestion.error.find("Checking") != std::string::npos);
#endif
}

UAM_TEST(VcsCommitServiceTrimsSelectedFilesForCommitMessagePrompt)
{
	uam::VcsCommitStatus status;
	status.active_vcs_type = uam::VcsType::Git;
	status.branch_or_revision = "main";
	status.changed_files.push_back({"app.txt", " M", false, 2, 1, false});
	status.changed_files.push_back({"other.txt", " M", false, 5, 0, false});

	const std::string prompt = uam::VcsCommitService::BuildCommitMessagePromptForTests(status, {" app.txt ", ""});

	UAM_ASSERT(prompt.find("-  M app.txt +2 -1") != std::string::npos);
	UAM_ASSERT(prompt.find("other.txt") == std::string::npos);
}

UAM_TEST(CodexCommitSuggestionUsesCompletedAssistantRatherThanReasoningDraft)
{
	const std::string draft = R"({"title":"Draft","description":"Reasoning candidate"})";
	const std::string final = R"({"title":"Final","description":"Actual assistant result"})";
	const std::string stream = R"({"type":"thread.started","thread_id":"owned-review"})" + std::string("\n") +
	    R"({"type":"turn.started"})" + "\n" +
	    nlohmann::json({{"type", "item.completed"}, {"item", {{"type", "reasoning"}, {"text", draft}}}}).dump() + "\n" +
	    nlohmann::json({{"type", "item.completed"}, {"item", {{"type", "agent_message"}, {"text", final}}}}).dump() + "\n" +
	    R"({"type":"turn.completed"})" + "\n";
	const uam::VcsCommitMessageSuggestion suggestion = uam::VcsCommitService::ParseWorkerOutputForTests(stream, "codex-cli");
	UAM_ASSERT(suggestion.ok);
	UAM_ASSERT_EQ(suggestion.title, std::string("Final"));
	UAM_ASSERT_EQ(uam::VcsCommitService::ParseWorkerOutputForTests(final, "codex-cli").title, std::string("Final"));
	UAM_ASSERT(!uam::VcsCommitService::ParseWorkerOutputForTests(stream.substr(0, stream.find(R"({"type":"turn.completed")")), "codex-cli").ok);
	UAM_ASSERT(!uam::VcsCommitService::ParseWorkerOutputForTests(stream + R"({"type":"error","message":"failed"})", "codex-cli").ok);
	for (const std::string text : {std::string{}, std::string("No commit message can be suggested.")})
	{
		const std::string failed = std::string("{\"type\":\"thread.started\",\"thread_id\":\"owned\"}\n{\"type\":\"turn.started\"}\n") +
		    nlohmann::json({{"type", "item.completed"}, {"item", {{"type", "reasoning"}, {"text", draft}}}}).dump() + "\n" +
		    nlohmann::json({{"type", "item.completed"}, {"item", {{"type", "agent_message"}, {"text", text}}}}).dump() + "\n{\"type\":\"turn.completed\"}\n";
		UAM_ASSERT(!uam::VcsCommitService::ParseWorkerOutputForTests(failed, "codex-cli").ok);
	}
	for (const std::string type : {"reasoning", "command_execution"})
	{
		const std::string single = nlohmann::json({{"type", "item.completed"}, {"item", {{"type", type}, {"text", final}}}}).dump();
		UAM_ASSERT(!uam::VcsCommitService::ParseWorkerOutputForTests(single, "codex-cli").ok);
	}
	const std::string tool = R"({"type":"item.completed","item":{"type":"command_execution","command":"unsafe"}})" + std::string("\n");
	UAM_ASSERT(!uam::VcsCommitService::ParseWorkerOutputForTests(stream.substr(0, stream.find(R"({"type":"turn.completed")")) + tool + R"({"type":"turn.completed"})", "codex-cli").ok);
}

UAM_TEST(VcsCommitServiceParsesWorkerSuggestionOutputShapes)
{
	uam::VcsCommitMessageSuggestion direct = uam::VcsCommitService::ParseWorkerOutputForTests(R"({"title":"  Update worker parsing  ","description":"  - Keep JSON robust  "})");
	UAM_ASSERT(direct.ok);
	UAM_ASSERT_EQ(direct.title, std::string("Update worker parsing"));
	UAM_ASSERT_EQ(direct.description, std::string("- Keep JSON robust"));

	uam::VcsCommitMessageSuggestion fenced = uam::VcsCommitService::ParseWorkerOutputForTests(R"(Here is the JSON:
```json
{"title":"Handle wrapped JSON","description":""}
```
)");
	UAM_ASSERT(fenced.ok);
	UAM_ASSERT_EQ(fenced.title, std::string("Handle wrapped JSON"));

	uam::VcsCommitMessageSuggestion nested = uam::VcsCommitService::ParseWorkerOutputForTests(R"({"type":"message","content":[{"text":"{\"title\":\"Parse nested worker JSON\",\"description\":\"- From event payload\"}"}]})");
	UAM_ASSERT(nested.ok);
	UAM_ASSERT_EQ(nested.title, std::string("Parse nested worker JSON"));
	UAM_ASSERT_EQ(nested.description, std::string("- From event payload"));

	uam::VcsCommitMessageSuggestion empty_title = uam::VcsCommitService::ParseWorkerOutputForTests(R"({"title":"   ","description":"ignored"})");
	UAM_ASSERT(!empty_title.ok);
	UAM_ASSERT_EQ(empty_title.error, std::string("Commit message worker returned an empty title."));

	uam::VcsCommitMessageSuggestion invalid = uam::VcsCommitService::ParseWorkerOutputForTests("not json");
	UAM_ASSERT(!invalid.ok);
	UAM_ASSERT_EQ(invalid.error, std::string("Commit message worker did not return the required JSON."));
}

UAM_TEST(ChatRepositoryPersistsAssistantPlanFields)
{
	TempDir temp("uam-chat-plan-fields");
	ChatSession chat;
	chat.id = "chat-plan";
	chat.provider_id = "codex-cli";
	chat.title = "Plan Fields";
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = "2026-01-01T00:00:01.000Z";

	Message assistant;
	assistant.role = MessageRole::Assistant;
	assistant.content = "";
	assistant.created_at = "2026-01-01T00:00:01.000Z";
	assistant.plan_summary = "Review the implementation path.";
	MessagePlanEntry entry;
	entry.content = "Patch Codex reasoning handling";
	entry.priority = "1";
	entry.status = "inProgress";
	assistant.plan_entries.push_back(std::move(entry));
	MessageBlock text_block;
	text_block.type = "assistant_text";
	text_block.text = "Review the implementation path.";
	assistant.blocks.push_back(std::move(text_block));
	MessageBlock plan_block;
	plan_block.type = "plan";
	assistant.blocks.push_back(std::move(plan_block));
	chat.messages.push_back(std::move(assistant));

	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));

	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().messages[0].plan_summary, std::string("Review the implementation path."));
	UAM_ASSERT_EQ(loaded.front().messages[0].plan_entries.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().messages[0].plan_entries[0].content, std::string("Patch Codex reasoning handling"));
	UAM_ASSERT_EQ(loaded.front().messages[0].plan_entries[0].status, std::string("inProgress"));
	UAM_ASSERT_EQ(loaded.front().messages[0].blocks.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(loaded.front().messages[0].blocks[0].type, std::string("assistant_text"));
	UAM_ASSERT_EQ(loaded.front().messages[0].blocks[0].text, std::string("Review the implementation path."));
	UAM_ASSERT_EQ(loaded.front().messages[0].blocks[1].type, std::string("plan"));

	const nlohmann::json persisted = nlohmann::json::parse(ReadFile(AppPaths::UamChatFilePath(temp.root, chat.id)));
	UAM_ASSERT_EQ(persisted["messages"][0].value("plan_summary", ""), std::string("Review the implementation path."));
	UAM_ASSERT_EQ(persisted["messages"][0]["plan_entries"][0].value("content", ""), std::string("Patch Codex reasoning handling"));
	UAM_ASSERT_EQ(persisted["messages"][0]["blocks"][0].value("type", ""), std::string("assistant_text"));
	UAM_ASSERT_EQ(persisted["messages"][0]["blocks"][0].value("text", ""), std::string("Review the implementation path."));
	UAM_ASSERT_EQ(persisted["messages"][0]["blocks"][1].value("type", ""), std::string("plan"));
}

UAM_TEST(ChatRepositoryPersistsMessageAttachments)
{
	TempDir temp("uam-chat-attachments");
	ChatSession chat;
	chat.id = "chat-attachments";
	chat.provider_id = "codex-cli";
	chat.title = "Attachments";
	chat.created_at = "2026-01-01T00:00:00.000Z";
	chat.updated_at = "2026-01-01T00:00:01.000Z";

	Message user;
	user.role = MessageRole::User;
	user.content = "Please inspect these files.";
	user.created_at = "2026-01-01T00:00:01.000Z";
	MessageAttachment file;
	file.id = "att-1";
	file.name = "screenshot.png";
	file.kind = "image";
	file.mime_type = "image/png";
	file.path = ".UAM/attachments/chat-attachments/screenshot.png";
	file.size_bytes = 1234;
	file.copied = true;
	user.attachments.push_back(std::move(file));
	MessageAttachment directory;
	directory.id = "att-2";
	directory.name = "src";
	directory.kind = "directory";
	directory.path = "src";
	directory.copied = false;
	user.attachments.push_back(std::move(directory));
	chat.messages.push_back(std::move(user));

	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	nlohmann::json persisted = nlohmann::json::parse(ReadFile(AppPaths::UamChatFilePath(temp.root, chat.id)));
	persisted["messages"][0]["attachments"][1]["size_bytes"] = -42;
	persisted["messages"][0]["attachments"].push_back({{"id", "att-3"}, {"kind", "file"}, {"name", "huge.bin"}, {"path", ".UAM/attachments/chat-attachments/huge.bin"}, {"size_bytes", 1.0e100}});
	UAM_ASSERT(uam::io::WriteTextFile(AppPaths::UamChatFilePath(temp.root, chat.id), persisted.dump(2)));

	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().messages.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(loaded.front().messages[0].attachments.size(), static_cast<std::size_t>(3));
	UAM_ASSERT_EQ(loaded.front().messages[0].attachments[0].path, std::string(".UAM/attachments/chat-attachments/screenshot.png"));
	UAM_ASSERT_EQ(loaded.front().messages[0].attachments[0].kind, std::string("image"));
	UAM_ASSERT_EQ(loaded.front().messages[0].attachments[0].size_bytes, static_cast<std::uintmax_t>(1234));
	UAM_ASSERT(loaded.front().messages[0].attachments[0].copied);
	UAM_ASSERT_EQ(loaded.front().messages[0].attachments[1].path, std::string("src"));
	UAM_ASSERT_EQ(loaded.front().messages[0].attachments[1].kind, std::string("directory"));
	UAM_ASSERT_EQ(loaded.front().messages[0].attachments[1].size_bytes, static_cast<std::uintmax_t>(0));
	UAM_ASSERT_EQ(loaded.front().messages[0].attachments[2].size_bytes, std::numeric_limits<std::uintmax_t>::max());
}

UAM_TEST(ChatRepositoryDoesNotSynthesizeInvalidCodexNativeIds)
{
	TempDir temp("uam-provider-native-normalize");
	const fs::path chats_dir = temp.root / "chats";
	fs::create_directories(chats_dir);
	struct IdentityCase
	{
		const char* id;
		const char* provider;
		const char* stored;
		const char* expected;
	};
	const IdentityCase cases[] = {
	    {"chat-codex-missing", "codex-cli", "", ""},
	    {"6a6f0f3b-1a0b-4a9c-8a01-111111111111", "codex-cli", "", ""},
	    {"chat-codex-invalid", " CoDeX ", "chat-codex-invalid", ""},
	    {"chat-codex-padded", "codex-cli", " 6a6f0f3b-1a0b-4a9c-8a01-111111111111 ", "6a6f0f3b-1a0b-4a9c-8a01-111111111111"},
	    {"chat-codex-urn", "codex-cli", "urn:uuid:6a6f0f3b-1a0b-4a9c-8a01-111111111111", "urn:uuid:6a6f0f3b-1a0b-4a9c-8a01-111111111111"},
	    {"gemini-missing", "gemini-cli", "", "gemini-missing"},
	    {"chat-gemini-draft", "gemini-cli", "", ""},
	    {"opencode-missing", "opencode-cli", "", "opencode-missing"},
	    {"claude-missing", "claude-cli", "", "claude-missing"},
	    {"copilot-missing", "copilot-cli", "", "copilot-missing"},
	    {"chat-existing", "opencode-cli", "ses_existing", "ses_existing"},
	};
	for (const IdentityCase& value : cases)
	{
		const nlohmann::json document = {{"id", value.id}, {"provider_id", value.provider},
		    {"native_session_id", value.stored}, {"title", value.id}, {"messages", nlohmann::json::array()}};
		UAM_ASSERT(uam::io::WriteTextFile(chats_dir / (std::string(value.id) + ".json"), document.dump()));
	}
	const std::vector<ChatSession> loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.size(), std::size(cases));
	for (const IdentityCase& value : cases)
	{
		const std::vector<ChatSession>::const_iterator found = std::ranges::find(loaded, value.id, &ChatSession::id);
		UAM_ASSERT(found != loaded.end());
		UAM_ASSERT_EQ(found->native_session_id, std::string(value.expected));
	}
}
