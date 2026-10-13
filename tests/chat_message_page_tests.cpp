#include "test_harness.h"
#include "common/chat/chat_message_page_index.h"
#include <array>
#include <chrono>
#include <limits>

using namespace uam_test;

namespace
{
	ChatSession PageMetadata()
	{
		ChatSession chat;
		chat.id = "indexed-history";
		chat.provider_id = "codex-cli";
		chat.created_at = "2026-10-13T00:00:00.000Z";
		chat.updated_at = "2026-10-13T00:00:01.000Z";
		chat.workspace_directory = "/tmp/uam-indexed-history";
		return chat;
	}

	Message PageMessage(std::size_t index, std::size_t content_size = 0)
	{
		Message message;
		message.role = index % 2 == 0 ? MessageRole::User : MessageRole::Assistant;
		message.content = "Message " + std::to_string(index) + ": braces {} [] quotes \" slash \\ Unicode ☃ 🚀 " + std::string(content_size, 'x');
		message.created_at = "2026-10-13T00:00:01.000Z";
		message.provider = "codex-cli";
		message.model_id = "fixture-model";
		message.thoughts = "Complete reasoning " + std::to_string(index);
		message.plan_summary = "Complete plan";
		message.markdown_store_files = {"fixture.md"};
		message.markdown_store_prompt_blocks = {"Complete injected context"};
		ToolCall tool;
		tool.id = "tool-" + std::to_string(index);
		tool.name = "read";
		tool.kind = "read";
		tool.args_json = R"({"path":"complete.txt"})";
		tool.result_text = "Complete tool output ☃";
		tool.child_activity = "Complete child metadata";
		tool.status = "completed";
		message.tool_calls.push_back(tool);
		MessageBlock block;
		block.type = "text";
		block.text = "Ordered full block";
		message.blocks.push_back(block);
		message.plan_entries.push_back({"Complete entry", "high", "completed"});
		return message;
	}

	void SaveImported(const fs::path& root, ChatSession& metadata, std::size_t count, std::size_t content_size = 0)
	{
		const std::shared_ptr<PreparedChatSave> prepared = ChatRepository::PrepareImportedChatSave(root, metadata,
		    [count, content_size](const std::function<bool(const Message&)>& emit, ChatSession&)
		    {
			    for (std::size_t index = 0; index < count; ++index) if (!emit(PageMessage(index, content_size))) return false;
			    return true;
		    });
		UAM_ASSERT(prepared);
		const std::optional<uam::chat_pages::Index> staged_index = uam::chat_pages::ReadIndex(prepared->index_temp, prepared->primary_revision);
		UAM_ASSERT(staged_index);
		std::ifstream staged_primary(prepared->primary_temp, std::ios::binary);
		std::string opening, closing, staged_metadata;
		UAM_ASSERT(uam::chat_pages::ReadSpan(staged_primary, {staged_index->array_begin, 1}, opening));
		UAM_ASSERT(uam::chat_pages::ReadSpan(staged_primary, {staged_index->array_end, 1}, closing));
		UAM_ASSERT_EQ(opening, std::string("["));
		UAM_ASSERT_EQ(closing, std::string("]"));
		UAM_ASSERT(uam::chat_pages::ReadMetadata(staged_primary, *staged_index, staged_metadata));
		UAM_ASSERT_EQ(nlohmann::json::parse(staged_metadata)["id"].get<std::string>(), metadata.id);
		UAM_ASSERT(ChatRepository::PublishPreparedChatSave(prepared, true, &metadata));
		UAM_ASSERT(!metadata.messages_loaded);
		UAM_ASSERT(metadata.messages.empty());
	}

	void CheckIndexedTiming(std::size_t count, bool legacy)
	{
		TempDir temp("uam-indexed-page-timing");
		ChatSession metadata = PageMetadata();
		SaveImported(temp.root, metadata, count, 2048);
		if (legacy) UAM_ASSERT(uam::paths::RemoveFileNoThrow(uam::chat_pages::IndexPath(AppPaths::UamChatFilePath(temp.root, metadata.id))));
		const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
		const std::optional<ChatMessagePage> page = ChatRepository::ReadMessagePage(temp.root, metadata, 50);
		UAM_ASSERT(page);
		UAM_ASSERT_EQ(page->total_count, count);
		UAM_ASSERT_EQ(page->chat.messages.size(), std::min(count, std::size_t{50}));
		UAM_ASSERT_EQ(page->chat.messages.back(), PageMessage(count - 1, 2048));
		const nlohmann::json view = uam::StateSerializer::SerializeDetachedMessagePage(*page, true);
		UAM_ASSERT_EQ(view["messages"].size(), page->chat.messages.size());
		const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		std::cout << "[TIMING] " << (legacy ? "legacy first index" : "indexed page") << " " << count << " messages: " << elapsed << " ms (limit 1000 ms)\n";
		UAM_ASSERT(elapsed <= 1000.0);
	}
}

UAM_TEST(ImportedMessagePagePreservesCompletePayloadAndDigest)
{
	TempDir temp("uam-indexed-message-payload");
	ChatSession metadata = PageMetadata();
	SaveImported(temp.root, metadata, 7);
	const std::optional<ChatSession> complete = ChatRepository::LoadLocalChat(temp.root, metadata.id);
	UAM_ASSERT(complete);
	const std::optional<ChatMessagePage> page = ChatRepository::ReadMessagePage(temp.root, metadata, 3);
	UAM_ASSERT(page);
	UAM_ASSERT_EQ(page->start_index, std::size_t{4});
	UAM_ASSERT_EQ(page->total_count, std::size_t{7});
	UAM_ASSERT_EQ(page->messages_digest, uam::StateSerializer::MessageDigest(*complete));
	UAM_ASSERT_EQ(page->user_message_indices, (std::optional<std::vector<std::size_t>>{{0, 2, 4, 6}}));
	for (std::size_t index = 0; index < page->chat.messages.size(); ++index)
		UAM_ASSERT_EQ(page->chat.messages[index], PageMessage(index + 4));
	nlohmann::json actual = uam::StateSerializer::SerializeDetachedMessagePage(*page, true);
	actual.erase("chatId"); actual.erase("unchanged");
	UAM_ASSERT_EQ(actual, uam::StateSerializer::SerializeMessagePage(*complete, 3, {}, true));
	UAM_ASSERT(uam::StateSerializer::SerializeDetachedMessagePage(*page, true, page->messages_digest)["unchanged"].get<bool>());
	UAM_ASSERT(!uam::StateSerializer::SerializeDetachedMessagePage(*page, true, page->messages_digest, false)["unchanged"].get<bool>());
	UAM_ASSERT(metadata.messages.empty());
}

UAM_TEST(ImportedHistoryPagesCoverTheEntireTranscript)
{
	TempDir temp("uam-indexed-history-coverage");
	ChatSession metadata = PageMetadata();
	SaveImported(temp.root, metadata, 207);
	std::size_t before = 207;
	while (before != 0)
	{
		const std::optional<ChatMessagePage> page = ChatRepository::ReadMessagePage(temp.root, metadata, 50, before);
		UAM_ASSERT(page);
		UAM_ASSERT_EQ(page->start_index + page->chat.messages.size(), before);
		for (std::size_t index = 0; index < page->chat.messages.size(); ++index)
			UAM_ASSERT_EQ(page->chat.messages[index], PageMessage(page->start_index + index));
		before = page->start_index;
	}
}

UAM_TEST(IndexedPageRejectsProviderWorkspaceAndCountChanges)
{
	TempDir temp("uam-indexed-page-ownership");
	ChatSession metadata = PageMetadata();
	SaveImported(temp.root, metadata, 4);
	const std::optional<ChatMessagePage> page = ChatRepository::ReadMessagePage(temp.root, metadata, 2);
	UAM_ASSERT(page);
	for (int field = 0; field < 8; ++field)
	{
		ChatSession changed = metadata;
		if (field == 0) changed.provider_id = "gemini-cli";
		if (field == 1) changed.native_session_id = "different-native";
		if (field == 2) changed.workspace_directory += "-changed";
		if (field == 3) changed.execution_host_id = "other-host";
		if (field == 4) changed.folder_id = "other-folder";
		if (field == 5) changed.persisted_message_count += 1;
		if (field == 6) changed.persisted_messages_digest += "-changed";
		if (field == 7) changed.updated_at = "2026-10-13T00:00:03.000Z";
		UAM_ASSERT(!ChatRepository::MatchesMessagePage(changed, *page));
		UAM_ASSERT(!ChatRepository::ReadMessagePage(temp.root, changed, 2));
	}
}

UAM_TEST(IndexedPageRepairsMissingTruncatedAndCorruptOffsets)
{
	TempDir temp("uam-indexed-page-repair");
	ChatSession metadata = PageMetadata();
	SaveImported(temp.root, metadata, 8);
	const fs::path index_path = uam::chat_pages::IndexPath(AppPaths::UamChatFilePath(temp.root, metadata.id));
	for (int damage = 0; damage < 4; ++damage)
	{
		if (damage == 0) UAM_ASSERT(uam::paths::RemoveFileNoThrow(index_path));
		if (damage == 1) UAM_ASSERT(uam::io::WriteTextFile(index_path, "UAMPAGE1"));
		if (damage == 2)
		{
			std::string text = ReadFile(index_path);
			UAM_ASSERT(text.size() > 32);
			text[text.size() - 16] ^= 1; // Change the final message fingerprint while retaining a structurally valid index.
			UAM_ASSERT(uam::io::WriteTextFile(index_path, text));
		}
		if (damage == 3)
		{
			const std::optional<uam::chat_pages::Index> header = uam::chat_pages::ReadIndex(index_path, *ChatRepository::ChatStorageRevision(temp.root, metadata.id));
			UAM_ASSERT(header);
			std::string text = ReadFile(index_path);
			const std::size_t array_offset_field = 8 + 8 + header->source_revision.size() + 8 + header->messages_digest.size() + 8;
			text[array_offset_field] = static_cast<char>(header->array_begin - 1); // Structurally valid header pointing at the colon instead of '['.
			UAM_ASSERT(uam::io::WriteTextFile(index_path, text));
		}
		const std::optional<ChatMessagePage> page = ChatRepository::ReadMessagePage(temp.root, metadata, 3);
		UAM_ASSERT(page);
		UAM_ASSERT_EQ(page->chat.messages.back(), PageMessage(7));
	}
}

UAM_TEST(LegacyMetadataFirstHistoryGetsExactBoundedPages)
{
	TempDir temp("uam-indexed-legacy-layout");
	ChatSession chat = PageMetadata();
	for (std::size_t index = 0; index < 5; ++index) chat.messages.push_back(PageMessage(index));
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const std::string original = ReadFile(AppPaths::UamChatFilePath(temp.root, chat.id));
	const std::optional<ChatSession> metadata = ChatRepository::LoadLocalChat(temp.root, chat.id, false);
	UAM_ASSERT(metadata);
	const std::optional<ChatMessagePage> page = ChatRepository::ReadMessagePage(temp.root, *metadata, 2, 3);
	UAM_ASSERT(page);
	UAM_ASSERT_EQ(page->start_index, std::size_t{1});
	UAM_ASSERT_EQ(page->chat.messages[0], PageMessage(1));
	UAM_ASSERT_EQ(page->chat.messages[1], PageMessage(2));
	UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(temp.root, chat.id)), original);
}

UAM_TEST(IndexedHistoryCancellationDoesNotPublishPartialCache)
{
	TempDir temp("uam-indexed-page-cancel");
	ChatSession metadata = PageMetadata();
	SaveImported(temp.root, metadata, 20);
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, metadata.id);
	const std::string original = ReadFile(primary);
	const fs::path index = uam::chat_pages::IndexPath(primary);
	UAM_ASSERT(uam::paths::RemoveFileNoThrow(index));
	std::size_t checks = 0;
	UAM_ASSERT(!ChatRepository::ReadMessagePage(temp.root, metadata, 3, {}, nullptr, [&]() { return ++checks > 40; }));
	UAM_ASSERT(!uam::paths::PathExistsNoThrow(index));
	UAM_ASSERT_EQ(ReadFile(primary), original);
	UAM_ASSERT(ChatRepository::ReadMessagePage(temp.root, metadata, 3));
}

UAM_TEST(StreamedUnloadedMetadataSavePreservesCompleteTranscript)
{
	TempDir temp("uam-indexed-unloaded-save");
	ChatSession metadata = PageMetadata();
	ChatSession previous = metadata;
	previous.messages = {PageMessage(0)};
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, previous));
	SaveImported(temp.root, metadata, 103);
	// Legacy primaries may contain whitespace before the root message array.
	const fs::path primary_path = AppPaths::UamChatFilePath(temp.root, metadata.id);
	std::string primary_bytes = ReadFile(primary_path);
	const std::size_t opening = primary_bytes.find("\"messages\":[") + std::string("\"messages\":").size();
	UAM_ASSERT(opening < primary_bytes.size());
	primary_bytes.insert(opening, " ");
	UAM_ASSERT(uam::io::WriteTextFile(primary_path, primary_bytes));
	UAM_ASSERT(ChatRepository::ReadMessagePage(temp.root, metadata, 2));
	const fs::path index_path = uam::chat_pages::IndexPath(AppPaths::UamChatFilePath(temp.root, metadata.id));
	const std::optional<uam::chat_pages::Index> header = uam::chat_pages::ReadIndex(index_path, *ChatRepository::ChatStorageRevision(temp.root, metadata.id));
	UAM_ASSERT(header);
	std::string damaged_index = ReadFile(index_path);
	const std::size_t array_offset_field = 8 + 8 + header->source_revision.size() + 8 + header->messages_digest.size() + 8;
	damaged_index[array_offset_field] = static_cast<char>(header->array_begin - 1);
	UAM_ASSERT(uam::io::WriteTextFile(index_path, damaged_index));
	UAM_ASSERT_EQ(primary_bytes.at(header->array_begin - 1), ' ');
	UAM_ASSERT(!uam::chat_pages::ReadIndex(index_path, header->source_revision));
	metadata.title = "Renamed without hydrating";
	metadata.pinned = true;
	metadata.updated_at = "2026-10-13T00:00:02.000Z";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, metadata));
	UAM_ASSERT(!metadata.messages_loaded);
	UAM_ASSERT(metadata.messages.empty());
	const std::optional<ChatSession> complete = ChatRepository::LoadLocalChat(temp.root, metadata.id);
	UAM_ASSERT(complete);
	UAM_ASSERT_EQ(complete->messages.size(), std::size_t{103});
	UAM_ASSERT_EQ(complete->title, metadata.title);
	UAM_ASSERT(complete->pinned);
	for (std::size_t index = 0; index < complete->messages.size(); ++index) UAM_ASSERT_EQ(complete->messages[index], PageMessage(index));
	const std::optional<ChatSession> summary = ChatRepository::LoadLocalChat(temp.root, metadata.id, false);
	UAM_ASSERT(summary);
	const std::optional<ChatMessagePage> page = ChatRepository::ReadMessagePage(temp.root, *summary, 2);
	UAM_ASSERT(page);
	UAM_ASSERT_EQ(page->messages_digest, uam::StateSerializer::MessageDigest(*complete));
}

UAM_TEST(StreamedMetadataPublishRejectsChangedPrimary)
{
	TempDir temp("uam-indexed-save-stale");
	ChatSession metadata = PageMetadata();
	SaveImported(temp.root, metadata, 4);
	metadata.title = "Must not overwrite newer files";
	const std::shared_ptr<PreparedChatSave> prepared = ChatRepository::PrepareChatSave(temp.root, metadata);
	UAM_ASSERT(prepared);
	ChatSession newer = PageMetadata();
	newer.messages = {PageMessage(99)};
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, newer));
	UAM_ASSERT(!ChatRepository::PublishPreparedChatSave(prepared));
	const std::optional<ChatSession> complete = ChatRepository::LoadLocalChat(temp.root, metadata.id);
	UAM_ASSERT(complete);
	UAM_ASSERT_EQ(complete->messages, newer.messages);
}

UAM_TEST(IndexedPageClampsCursorsAndPreservesEmptyOlderPages)
{
	TempDir temp("uam-indexed-page-cursor");
	ChatSession metadata = PageMetadata();
	SaveImported(temp.root, metadata, 4);
	const std::optional<ChatMessagePage> empty = ChatRepository::ReadMessagePage(temp.root, metadata, 2, 0);
	UAM_ASSERT(empty);
	UAM_ASSERT(empty->chat.messages.empty());
	UAM_ASSERT_EQ(empty->start_index, std::size_t{0});
	UAM_ASSERT_EQ(empty->total_count, std::size_t{4});
	const std::optional<ChatMessagePage> clamped = ChatRepository::ReadMessagePage(temp.root, metadata, 2, std::numeric_limits<std::size_t>::max());
	UAM_ASSERT(clamped);
	UAM_ASSERT_EQ(clamped->start_index, std::size_t{2});
	UAM_ASSERT(!ChatRepository::ReadMessagePage(temp.root, metadata, 201));
}

UAM_TEST(IndexedHistoryInvalidJsonCannotReturnPartialHistory)
{
	TempDir temp("uam-indexed-page-invalid");
	ChatSession metadata = PageMetadata();
	SaveImported(temp.root, metadata, 4);
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, metadata.id);
	UAM_ASSERT(uam::io::WriteTextFile(primary, ReadFile(primary) + "invalid trailing bytes"));
	UAM_ASSERT(!ChatRepository::ReadMessagePage(temp.root, metadata, 1));
}

UAM_TEST(IndexedHistoryCacheFailureKeepsPrimaryAndRepairsLater)
{
	TempDir temp("uam-indexed-page-cache-failure");
	ChatSession metadata = PageMetadata();
	const std::shared_ptr<PreparedChatSave> prepared = ChatRepository::PrepareImportedChatSave(temp.root, metadata,
	    [](const std::function<bool(const Message&)>& emit, ChatSession&) { return emit(PageMessage(0)); });
	UAM_ASSERT(prepared);
	UAM_ASSERT(uam::paths::CreateDirectoriesNoThrow(prepared->index_path));
	UAM_ASSERT(ChatRepository::PublishPreparedChatSave(prepared));
	std::error_code removal_error;
	UAM_ASSERT(fs::remove(prepared->index_path, removal_error) && !removal_error);
	UAM_ASSERT(ChatRepository::ReadMessagePage(temp.root, metadata, 1));
}

UAM_TEST(DeletingIndexedHistoryRemovesDerivedOffsets)
{
	TempDir temp("uam-indexed-page-delete");
	ChatSession metadata = PageMetadata();
	SaveImported(temp.root, metadata, 4);
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, metadata.id);
	const fs::path index = uam::chat_pages::IndexPath(primary);
	UAM_ASSERT(uam::paths::PathExistsNoThrow(index));
	UAM_ASSERT(!ChatRepository::DeleteChatStorageFiles(temp.root, metadata.id).Failed());
	UAM_ASSERT(!uam::paths::PathExistsNoThrow(index));
	UAM_ASSERT(!ChatRepository::ReadMessagePage(temp.root, metadata, 1));
}

UAM_TEST(IndexedHistoryPage20MessagesWithinOneSecond) { CheckIndexedTiming(20, false); }
UAM_TEST(IndexedHistoryPage50MessagesWithinOneSecond) { CheckIndexedTiming(50, false); }
UAM_TEST(IndexedHistoryPage200MessagesWithinOneSecond) { CheckIndexedTiming(200, false); }
UAM_TEST(IndexedHistoryPage10000MessagesWithinOneSecond) { CheckIndexedTiming(10000, false); }
UAM_TEST(LegacyFirstIndex10000MessagesWithinOneSecond) { CheckIndexedTiming(10000, true); }

UAM_TEST(LargeImportedMetadataSavePreservesPast256MiB)
{
	TempDir temp("uam-indexed-large-metadata-save");
	std::error_code error;
	const fs::space_info space = fs::space(temp.root, error);
	UAM_ASSERT(!error && space.available >= 900ULL * 1024 * 1024);
	ChatSession metadata = PageMetadata();
	SaveImported(temp.root, metadata, 260, 1024 * 1024);
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, metadata.id);
	UAM_ASSERT(fs::file_size(primary) > 256ULL * 1024 * 1024);
	metadata.title = "Large complete history";
	metadata.pinned = true;
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, metadata));
	UAM_ASSERT(metadata.messages.empty());
	const std::optional<ChatSession> summary = ChatRepository::LoadLocalChat(temp.root, metadata.id, false);
	UAM_ASSERT(summary && summary->pinned && summary->title == metadata.title);
	UAM_ASSERT_EQ(summary->persisted_message_count, std::size_t{260});
	for (const std::size_t before : {std::size_t{1}, std::size_t{130}, std::size_t{260}})
	{
		const std::optional<ChatMessagePage> page = ChatRepository::ReadMessagePage(temp.root, *summary, 1, before);
		UAM_ASSERT(page);
		UAM_ASSERT_EQ(page->chat.messages.front(), PageMessage(before - 1, 1024 * 1024));
	}
	const nlohmann::json persisted_summary = nlohmann::json::parse(ReadFile(AppPaths::UamChatSummaryFilePath(temp.root, metadata.id)));
	UAM_ASSERT_EQ(persisted_summary["summary_source_size"].get<std::uintmax_t>(), fs::file_size(primary));
	UAM_ASSERT_EQ(persisted_summary["persisted_messages_digest"].get<std::string>(), summary->persisted_messages_digest);
}
