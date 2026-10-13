#include "test_harness.h"
#include "common/config/custom_icon.h"

using namespace uam_test;

namespace
{
	ChatSession IconMetadataChat()
	{
		ChatSession chat;
		chat.id = "chat-icon-metadata";
		chat.provider_id = "codex-cli";
		chat.title = "Private icon fixture";
		chat.created_at = "2026-10-13T00:00:00.000Z";
		chat.updated_at = "2026-10-13T00:00:01.000Z";
		Message message;
		message.role = MessageRole::User;
		message.content = "Retained transcript";
		message.created_at = chat.created_at;
		chat.messages.push_back(message);
		return chat;
	}
}

UAM_TEST(ChatIconMetadataRoundTripsTextLibraryAndPortablePng)
{
	TempDir temp("uam-chat-icon-roundtrip");
	ChatSession chat = IconMetadataChat();
	for (const CustomIcon& icon : {CustomIcon{"text", "📌"}, CustomIcon{"text", "folder-code"}, CustomIcon{"png", "12345678-1234-1234-1234-123456789abc.png"}, CustomIcon{}})
	{
		UAM_ASSERT(uam::icons::IsValid(icon));
		chat.custom_icon = icon;
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
		const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(temp.root, chat.id);
		UAM_ASSERT(loaded); UAM_ASSERT(loaded->custom_icon == icon);
		UAM_ASSERT_EQ(loaded->messages, chat.messages);
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, *loaded));
		UAM_ASSERT(ChatRepository::LoadLocalChat(temp.root, chat.id)->custom_icon == icon);
	}
}

UAM_TEST(ChatIconMetadataSurvivesIndexedPagesAndUnloadedSave)
{
	TempDir temp("uam-chat-icon-indexed");
	ChatSession chat = IconMetadataChat();
	chat.custom_icon = {"text", "📌"};
	for (int index = 1; index < 64; ++index)
	{
		Message message = chat.messages.front();
		message.content = "Retained message " + std::to_string(index);
		chat.messages.push_back(std::move(message));
	}
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	std::optional<ChatSession> metadata = ChatRepository::LoadLocalChat(temp.root, chat.id, false);
	UAM_ASSERT(metadata && !metadata->messages_loaded && metadata->messages.empty());
	const std::optional<ChatMessagePage> page = ChatRepository::ReadMessagePage(temp.root, *metadata, 2);
	UAM_ASSERT(page); UAM_ASSERT(page->chat.custom_icon == chat.custom_icon);
	metadata->custom_icon = {"text", "folder-code"};
	ChatSession hydration = *metadata;
	UAM_ASSERT(ChatRepository::HydrateChatMessages(temp.root, hydration));
	UAM_ASSERT(hydration.custom_icon == metadata->custom_icon);
	UAM_ASSERT_EQ(hydration.messages, chat.messages);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, *metadata));
	const std::optional<ChatSession> complete = ChatRepository::LoadLocalChat(temp.root, chat.id);
	UAM_ASSERT(complete); UAM_ASSERT(complete->custom_icon == metadata->custom_icon);
	UAM_ASSERT_EQ(complete->messages, chat.messages);
	const std::optional<ChatSession> summary = ChatRepository::LoadLocalChatSummaryReadOnly(temp.root, chat.id);
	UAM_ASSERT(summary); UAM_ASSERT(summary->custom_icon == metadata->custom_icon);
}

UAM_TEST(ChatIconMetadataRejectsInvalidFieldsAndPreservesLegacyChats)
{
	TempDir temp("uam-chat-icon-invalid");
	ChatSession chat = IconMetadataChat();
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id);
	const nlohmann::json original = nlohmann::json::parse(ReadFile(primary));
	for (const nlohmann::json& icon : {nlohmann::json(nullptr), nlohmann::json(42), nlohmann::json{{"type", "text"}},
	    nlohmann::json{{"type", "text"}, {"value", 42}}, nlohmann::json{{"type", "text"}, {"value", "bad\ntext"}},
	    nlohmann::json{{"type", "svg"}, {"value", "<svg/>"}}, nlohmann::json{{"type", "png"}, {"value", "../outside.png"}}})
	{
		nlohmann::json wire = original; wire["custom_icon"] = icon;
		UAM_ASSERT(uam::io::WriteTextFile(primary, wire.dump()));
		const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(temp.root, chat.id);
		UAM_ASSERT(loaded); UAM_ASSERT(loaded->custom_icon == CustomIcon{});
		UAM_ASSERT_EQ(loaded->messages, chat.messages);
	}
	nlohmann::json legacy = original; legacy.erase("custom_icon");
	UAM_ASSERT(uam::io::WriteTextFile(primary, legacy.dump()));
	UAM_ASSERT(ChatRepository::LoadLocalChat(temp.root, chat.id)->custom_icon == CustomIcon{});
	chat.custom_icon = {"png", "../outside.png"};
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	UAM_ASSERT(nlohmann::json::parse(ReadFile(primary))["custom_icon"].is_null());
}

UAM_TEST(ChatIconMetadataBackupRecoveryKeepsTheRecoveredIcon)
{
	TempDir temp("uam-chat-icon-backup");
	ChatSession chat = IconMetadataChat();
	chat.custom_icon = {"text", "📌"};
	const CustomIcon previous = chat.custom_icon;
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	chat.custom_icon = {"text", "folder-code"};
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id);
	UAM_ASSERT(uam::io::WriteTextFile(primary, "{broken"));
	const std::vector<ChatSession> recovered = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(recovered.size(), std::size_t{1}); UAM_ASSERT(recovered.front().custom_icon == previous);
	UAM_ASSERT_EQ(recovered.front().messages, chat.messages);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, recovered.front()));
	UAM_ASSERT(ChatRepository::LoadLocalChat(temp.root, chat.id)->custom_icon == previous);
}
