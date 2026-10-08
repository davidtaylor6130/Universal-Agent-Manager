#include "test_harness.h"
#include "app/local_chat_bundle_service.h"

using namespace uam_test;

namespace
{
	ChatSession ExportTemporaryTranscript(const fs::path& source, const fs::path& bundle)
	{
		ChatSession chat;
		chat.id = "portable-side-chat";
		chat.provider_id = "codex-cli";
		chat.title = "Keep this transcript";
		chat.temporary_parent_chat_id = "source-owner";
		chat.side_cleanup_requested = true;
		chat.provider_handoff_context = "Source-only instructions";
		chat.provider_handoff_session_id = "source-session";
		chat.provider_handoff_cli_contexts.push_back({"local", "/source/runtime-context/owned", "source-connection"});
		chat.messages.push_back(Message{MessageRole::User, "Portable history"});
		UAM_ASSERT(ChatRepository::SaveChat(source, chat));
		UAM_ASSERT(uam::LocalChatBundleService::Export(source, bundle).ok);
		return chat;
	}
}

UAM_TEST(LocalChatBundleImportedSideTranscriptSurvivesStartupCleanup)
{
	TempDir temp("uam-bundle-side-startup");
	const fs::path source = temp.root / "source";
	const fs::path bundle = temp.root / "bundle";
	const fs::path target = temp.root / "target";
	const ChatSession original = ExportTemporaryTranscript(source, bundle);
	const std::string source_bytes = ReadFile(AppPaths::UamChatFilePath(source, original.id));
	UAM_ASSERT(uam::LocalChatBundleService::Import(target, bundle).ok);
	uam::AppState app;
	app.data_root = target;
	app.chats = ChatRepository::LoadLocalChatSummaries(target);
	// Exercise the actual startup cleanup path, without launching a provider.
	for (const ChatSession& chat : std::vector<ChatSession>(app.chats))
		if (!chat.temporary_parent_chat_id.empty())
			(void)uam::RequestTemporarySideChatCleanup(app, chat.id);
	UAM_ASSERT(!uam::PollTemporarySideChatCleanup(app));
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
	const std::optional<ChatSession> persisted = ChatRepository::LoadLocalChat(target, original.id);
	UAM_ASSERT(persisted.has_value());
	UAM_ASSERT_EQ(persisted->messages, original.messages);
	UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(source, original.id)), source_bytes);
}

UAM_TEST(LocalChatBundleImportDropsSourceCleanupAndHandoffAuthority)
{
	TempDir temp("uam-bundle-source-authority");
	const fs::path target = temp.root / "target";
	const ChatSession original = ExportTemporaryTranscript(temp.root / "source", temp.root / "bundle");
	UAM_ASSERT(uam::LocalChatBundleService::Import(target, temp.root / "bundle").ok);
	const nlohmann::json persisted = nlohmann::json::parse(ReadFile(AppPaths::UamChatFilePath(target, original.id)));
	UAM_ASSERT_EQ(persisted.value("temporary_parent_chat_id", ""), std::string{});
	UAM_ASSERT(!persisted.value("side_cleanup_requested", false));
	UAM_ASSERT_EQ(persisted.value("provider_handoff_context", ""), std::string{});
	UAM_ASSERT_EQ(persisted.value("provider_handoff_session_id", ""), std::string{});
	UAM_ASSERT(persisted["provider_handoff_cli_contexts"].empty());
}

UAM_TEST(ChatRepositoryKeepsPreviouslyImportedSideTranscriptsPassive)
{
	TempDir temp("uam-bundle-legacy-passive");
	ChatSession legacy;
	legacy.id = "legacy-imported-side";
	legacy.imported_read_only = true;
	legacy.temporary_parent_chat_id = "old-owner";
	legacy.side_cleanup_requested = true;
	legacy.provider_handoff_context = "Old handoff";
	legacy.provider_handoff_session_id = "old-session";
	legacy.provider_handoff_cli_contexts.push_back({"local", "/old/context", "old-connection"});
	legacy.messages.push_back(Message{MessageRole::User, "Retain me"});
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, legacy));
	const std::string original_bytes = ReadFile(AppPaths::UamChatFilePath(temp.root, legacy.id));
	for (const bool include_messages : {false, true})
	{
		const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(temp.root, legacy.id, include_messages);
		UAM_ASSERT(loaded.has_value());
		UAM_ASSERT(loaded->temporary_parent_chat_id.empty());
		UAM_ASSERT(!loaded->side_cleanup_requested);
		UAM_ASSERT(loaded->provider_handoff_context.empty());
		UAM_ASSERT(loaded->provider_handoff_session_id.empty());
		UAM_ASSERT(loaded->provider_handoff_cli_contexts.empty());
		if (include_messages) UAM_ASSERT_EQ(loaded->messages, legacy.messages);
	}
	UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(temp.root, legacy.id)), original_bytes);
}
