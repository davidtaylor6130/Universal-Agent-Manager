#include "test_harness.h"
#include "app/chat_file_change_receipt_service.h"
#include "common/paths/app_paths.h"

using namespace uam_test;

namespace
{
	ChatSession ReceiptChat(const nlohmann::json& changes)
	{
		ChatSession chat;
		chat.id = "receipt-chat";
		chat.provider_id = "codex-cli";
		chat.messages.push_back(Message{MessageRole::Assistant, "Edited"});
		chat.messages.front().provider = "codex-cli";
		chat.messages.front().created_at = "2026-10-07T10:00:00Z";
		ToolCall tool;
		tool.id = "receipt-tool";
		tool.kind = "fileChange";
		tool.status = "completed";
		tool.result_text = nlohmann::json{{"type", "fileChange"}, {"status", "completed"}, {"changes", changes}}.dump();
		chat.messages.front().tool_calls.push_back(std::move(tool));
		return chat;
	}

	nlohmann::json ReceiptChange(std::string path, std::string patch = "-before\n+after\n")
	{
		return {{"path", std::move(path)}, {"diff", std::move(patch)}, {"kind", {{"type", "update"}}}};
	}
}

UAM_TEST(ChatFileReceiptsPreserveHistoricalPatchIdentityAndRenameDeleteEvidence)
{
	const nlohmann::json rename = {{"path", "src/old.cpp"}, {"diff", "recorded rename patch"}, {"kind", {{"type", "update"}, {"movePath", "src/new.cpp"}}}};
	const nlohmann::json deletion = {{"path", "removed.cpp"}, {"diff", "-deleted\n"}, {"kind", {{"type", "delete"}}}};
	ChatSession chat = ReceiptChat(nlohmann::json::array({ReceiptChange("/workspace/src/main.cpp"), rename, deletion}));
	const auto result = uam::ChatFileChangeReceiptService::Extract(chat, "/workspace");
	UAM_ASSERT_EQ(result.receipts.size(), std::size_t{3});
	const auto& first = result.receipts.front();
	UAM_ASSERT_EQ(first.chat_id, chat.id);
	UAM_ASSERT_EQ(first.message_index, std::size_t{0});
	UAM_ASSERT_EQ(first.tool_call_id, std::string("receipt-tool"));
	UAM_ASSERT_EQ(first.message_created_at, chat.messages.front().created_at);
	UAM_ASSERT_EQ(first.provider_id, std::string("codex-cli"));
	UAM_ASSERT_EQ(first.workspace_relative_path, std::string("src/main.cpp"));
	UAM_ASSERT_EQ(first.patch, std::string("-before\n+after\n"));
	UAM_ASSERT_EQ(nlohmann::json::parse(result.receipts[1].kind_json), rename["kind"]);
	UAM_ASSERT_EQ(result.receipts[1].workspace_relative_destination, std::string("src/new.cpp"));
	UAM_ASSERT_EQ(nlohmann::json::parse(result.receipts[2].kind_json), deletion["kind"]);
	// A separate chat editing the same file cannot change this immutable receipt.
	ChatSession other = ReceiptChat(nlohmann::json::array({ReceiptChange("src/main.cpp", "other chat patch")}));
	other.id = "other-chat";
	UAM_ASSERT_EQ(uam::ChatFileChangeReceiptService::Extract(other, "/workspace").receipts.front().chat_id, other.id);
	UAM_ASSERT_EQ(uam::ChatFileChangeReceiptService::Extract(chat, "/workspace").receipts.front().patch, first.patch);
}

UAM_TEST(ChatFileReceiptsNeverAttributeReadLinksShellOrUncompletedEdits)
{
	for (int guard = 0; guard < 5; ++guard)
	{
		ChatSession chat = ReceiptChat(nlohmann::json::array({ReceiptChange("main.cpp")}));
		chat.linked_files = {"main.cpp"};
		ToolCall& tool = chat.messages.front().tool_calls.front();
		if (guard == 0) tool.kind = "read";
		if (guard == 1) tool.kind = "commandExecution";
		if (guard == 2) tool.status = "failed";
		if (guard == 3) tool.result_text = "malformed";
		if (guard == 4) chat.messages.front().provider = "claude-code";
		UAM_ASSERT(uam::ChatFileChangeReceiptService::Extract(chat, "/workspace").receipts.empty());
	}
}

UAM_TEST(ChatFileReceiptsRejectOutsidePathsAndNormalizeWindowsAndUnicode)
{
	ChatSession chat = ReceiptChat(nlohmann::json::array({ReceiptChange("../outside.cpp"), ReceiptChange("/workspace-other/file.cpp"), ReceiptChange("/outside.cpp"), ReceiptChange("src/../safe.cpp")}));
	const auto unix_result = uam::ChatFileChangeReceiptService::Extract(chat, "/workspace");
	UAM_ASSERT_EQ(unix_result.receipts.size(), std::size_t{1});
	UAM_ASSERT_EQ(unix_result.omitted_count, std::size_t{3});
	UAM_ASSERT_EQ(unix_result.receipts.front().workspace_relative_path, std::string("safe.cpp"));
	chat = ReceiptChat(nlohmann::json::array({ReceiptChange("C:\\Work\\Repo\\資料.cpp"), ReceiptChange("c:\\work\\repo-other\\outside.cpp")}));
	const auto windows_result = uam::ChatFileChangeReceiptService::Extract(chat, "c:/work/repo");
	UAM_ASSERT_EQ(windows_result.receipts.size(), std::size_t{1});
	UAM_ASSERT_EQ(windows_result.receipts.front().workspace_relative_path, std::string("資料.cpp"));
}

UAM_TEST(ChatFileReceiptsEnforceIndependentPatchAndTotalBudgets)
{
	ChatSession chat = ReceiptChat(nlohmann::json::array({ReceiptChange("too-large.cpp", std::string(1024 * 1024 + 1, 'x')), ReceiptChange("small.cpp")}));
	const auto single = uam::ChatFileChangeReceiptService::Extract(chat, "/workspace");
	UAM_ASSERT_EQ(single.receipts.size(), std::size_t{1});
	UAM_ASSERT_EQ(single.omitted_count, std::size_t{1});
	nlohmann::json changes = nlohmann::json::array();
	for (int index = 0; index < 6; ++index) changes.push_back(ReceiptChange("file" + std::to_string(index) + ".cpp", std::string(1024 * 1024, 'x')));
	chat = ReceiptChat(changes);
	const auto total = uam::ChatFileChangeReceiptService::Extract(chat, "/workspace");
	UAM_ASSERT_EQ(total.receipts.size(), std::size_t{4});
	UAM_ASSERT_EQ(total.omitted_count, std::size_t{2});
}

UAM_TEST(ChatFileReceiptsRejectWrongDiskChatIdentityOrHistorySnapshot)
{
	TempDir temp("uam-receipts-snapshot");
	ChatSession loaded;
	loaded.id = "other-chat";
	loaded.messages_loaded = true;
	loaded.messages.push_back(Message{MessageRole::Assistant, "Recorded edit"});
	loaded.persisted_messages_digest = "digest";
	UAM_ASSERT(!uam::ChatFileChangeReceiptService::MatchesPersistedSnapshot(loaded, "requested-chat", 1, "digest"));
	loaded.id = "requested-chat";
	UAM_ASSERT(!uam::ChatFileChangeReceiptService::MatchesPersistedSnapshot(loaded, loaded.id, 2, "digest"));
	UAM_ASSERT(!uam::ChatFileChangeReceiptService::MatchesPersistedSnapshot(loaded, loaded.id, 1, "older-digest"));
	UAM_ASSERT(uam::ChatFileChangeReceiptService::MatchesPersistedSnapshot(loaded, loaded.id, 1, "digest"));
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, loaded));
	const std::optional<ChatSession> original = ChatRepository::LoadLocalChat(temp.root, loaded.id);
	UAM_ASSERT(original.has_value());
	ChatSession other = loaded;
	other.id = "other-chat";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, other));
	std::filesystem::copy_file(AppPaths::UamChatFilePath(temp.root, other.id), AppPaths::UamChatFilePath(temp.root, loaded.id), std::filesystem::copy_options::overwrite_existing);
	const std::optional<ChatSession> replaced = ChatRepository::LoadLocalChat(temp.root, loaded.id);
	UAM_ASSERT(!replaced || !uam::ChatFileChangeReceiptService::MatchesPersistedSnapshot(*replaced, loaded.id, 1, original->persisted_messages_digest));
	loaded.messages_loaded = false;
	UAM_ASSERT(!uam::ChatFileChangeReceiptService::MatchesPersistedSnapshot(loaded, loaded.id, 1, "digest"));
}
