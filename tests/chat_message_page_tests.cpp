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
		{
			// Close before publishing: MSVC streams omit FILE_SHARE_DELETE, which blocks the Windows rename.
			std::ifstream staged_primary(prepared->primary_temp, std::ios::binary);
			std::string opening, closing, staged_metadata;
			UAM_ASSERT(uam::chat_pages::ReadSpan(staged_primary, {staged_index->array_begin, 1}, opening));
			UAM_ASSERT(uam::chat_pages::ReadSpan(staged_primary, {staged_index->array_end, 1}, closing));
			UAM_ASSERT_EQ(opening, std::string("["));
			UAM_ASSERT_EQ(closing, std::string("]"));
			UAM_ASSERT(uam::chat_pages::ReadMetadata(staged_primary, *staged_index, staged_metadata));
			UAM_ASSERT_EQ(nlohmann::json::parse(staged_metadata)["id"].get<std::string>(), metadata.id);
		}
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

UAM_TEST(SavedHistoryKeepsEmptySkillSnapshotsPairedWithFiles)
{
	TempDir temp("uam-empty-skill-snapshot");
	ChatSession chat = PageMetadata();
	chat.messages.push_back(PageMessage(0));
	chat.messages.back().markdown_store_files = {"missing-snapshot.md", "captured.md"};
	chat.messages.back().markdown_store_prompt_blocks = {"", "Captured block"};
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(temp.root, chat.id, true);
	UAM_ASSERT(loaded && loaded->messages.size() == 1);
	UAM_ASSERT_EQ(loaded->messages[0].markdown_store_files, chat.messages[0].markdown_store_files);
	UAM_ASSERT_EQ(loaded->messages[0].markdown_store_prompt_blocks, chat.messages[0].markdown_store_prompt_blocks);
	const std::optional<ChatSession> metadata = ChatRepository::LoadLocalChat(temp.root, chat.id, false);
	UAM_ASSERT(metadata);
	const std::optional<ChatMessagePage> page = ChatRepository::ReadMessagePage(temp.root, *metadata, 1);
	UAM_ASSERT(page && page->chat.messages.size() == 1);
	UAM_ASSERT_EQ(page->chat.messages[0].markdown_store_prompt_blocks, chat.messages[0].markdown_store_prompt_blocks);
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


UAM_TEST(ManagedOriginMessageJsonPageDigestAndMalformedInput)
{
	TempDir temp("uam-managed-origin-message");
	ChatSession chat = PageMetadata();
	Message original = PageMessage(0);
	const std::string id = "5ad39bc5-8084-4e5e-8bbf-d3974ecca5c7";
	Message annotated = original;
	annotated.managed_origin_run_id = id;
	UAM_ASSERT(original != annotated);
	std::uint64_t old_hash = 14695981039346656037ULL, new_hash = old_hash;
	uam::chat_pages::AppendMessageDigest(old_hash, original);
	uam::chat_pages::AppendMessageDigest(new_hash, annotated);
	UAM_ASSERT(old_hash != new_hash);
	chat.messages = {annotated};
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const auto saved = ChatRepository::LoadLocalChat(temp.root, chat.id, true);
	UAM_ASSERT(saved && saved->messages == chat.messages);
	const auto metadata = ChatRepository::LoadLocalChat(temp.root, chat.id, false);
	UAM_ASSERT(metadata && !metadata->messages_loaded);
	const auto page = ChatRepository::ReadMessagePage(temp.root, *metadata, 1);
	UAM_ASSERT(page && page->chat.messages == chat.messages);
	const auto primary = AppPaths::UamChatFilePath(temp.root, chat.id);
	const auto document = nlohmann::json::parse(ReadFile(primary));
	UAM_ASSERT_EQ(document.at("messages").at(0).at("managed_origin_run_id").get<std::string>(), id);
	{
		auto unknown = document;
		unknown["messages"][0].erase("managed_origin_run_id");
		unknown["messages"][0]["unknown_managed_origin"] = id;
		UAM_ASSERT(uam::io::WriteTextFile(primary, unknown.dump()));
		const auto loaded = ChatRepository::LoadLocalChat(temp.root, chat.id, true, nullptr, false);
		UAM_ASSERT(loaded && loaded->messages[0] == original);
	}
	for (const auto& malformed : std::vector<nlohmann::json>{nullptr, 1, nlohmann::json::object(), "", "not-a-run", " " + id, id + " ", std::string(10000, 'x')})
	{
		auto candidate = document;
		candidate["messages"][0]["managed_origin_run_id"] = malformed;
		candidate["messages"][0]["unknown_managed_origin"] = id;
		UAM_ASSERT(uam::io::WriteTextFile(primary, candidate.dump()));
		const auto loaded = ChatRepository::LoadLocalChat(temp.root, chat.id, true, nullptr, false);
		UAM_ASSERT(loaded && !loaded->messages[0].managed_origin_run_id);
		UAM_ASSERT_EQ(loaded->messages[0], original);
	}
}


// Shape derives from the sealed alpha135 saved delegate receipt. All task/result values
// below are synthetic; this is not a copy of user data or a live-provider acceptance test.
UAM_TEST(DeferredManagedLaunchReceiptSurvivesSavedPageWithoutToolContent)
{
	TempDir temp("uam-deferred-managed-launch");
	ChatSession chat = PageMetadata();
	const std::string run_id = "8509f0ba-ea06-4068-a6ed-3e1bab09bc53";
	const nlohmann::json arguments{{"agentId", "plan"}, {"providerId", "copilot-cli"}, {"task", "Synthetic saved launch fixture"}};
	const nlohmann::json accepted{{"runId", run_id}, {"status", "queued"}};
	const nlohmann::json wrapper{
	    {"type", "mcpToolCall"}, {"id", "exec-synthetic-saved-delegate"},
	    {"server", "uam-control"}, {"tool", "agent_delegate"}, {"status", "completed"},
	    {"arguments", arguments}, {"durationMs", 537}, {"error", nullptr},
	    {"appContext", nullptr}, {"mcpAppUi", nullptr}, {"pluginId", nullptr}, {"readOnlyHint", nullptr},
	    {"result", {{"_meta", nullptr}, {"structuredContent", nullptr},
	        {"content", nlohmann::json::array({{{"type", "text"}, {"text", accepted.dump()}}})}}}};
	ToolCall delegate;
	delegate.id = "exec-synthetic-saved-delegate";
	delegate.name = "agent_delegate";
	delegate.kind = "mcpToolCall";
	delegate.status = "completed";
	delegate.args_json = arguments.dump();
	delegate.result_text = wrapper.dump(2);
	Message message;
	message.role = MessageRole::Assistant;
	message.created_at = "2026-10-20T19:00:00.000Z";
	message.provider = "codex-cli";
	message.tool_calls = {delegate};
	MessageBlock block;
	block.type = "tool_call";
	block.tool_call_id = delegate.id;
	message.blocks = {block};
	chat.messages = {message};
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const auto saved = ChatRepository::LoadLocalChat(temp.root, chat.id, true);
	UAM_ASSERT(saved && saved->messages == chat.messages);
	const auto metadata = ChatRepository::LoadLocalChat(temp.root, chat.id, false);
	UAM_ASSERT(metadata && !metadata->messages_loaded && metadata->messages.empty());
	const auto page = ChatRepository::ReadMessagePage(temp.root, *metadata, 50);
	UAM_ASSERT(page && page->chat.messages == chat.messages);
	const auto deferred = uam::StateSerializer::SerializeDetachedMessagePage(*page, true);
	const auto inline_page = uam::StateSerializer::SerializeDetachedMessagePage(*page, false);
	const auto& tool = deferred.at("messages").at(0).at("toolCalls").at(0);
	UAM_ASSERT_EQ(tool.at("title").get<std::string>(), delegate.name);
	UAM_ASSERT_EQ(tool.at("kind").get<std::string>(), delegate.kind);
	UAM_ASSERT_EQ(tool.at("status").get<std::string>(), delegate.status);
	UAM_ASSERT(tool.at("contentDeferred").get<bool>());
	UAM_ASSERT(!tool.contains("content"));
	UAM_ASSERT(!tool.at("contentDigest").get<std::string>().empty());
	const std::string full_content = uam::StateSerializer::ToolCallContentForFrontend(delegate);
	UAM_ASSERT_EQ(inline_page.at("messages").at(0).at("toolCalls").at(0).at("content").get<std::string>(), full_content);
	const auto details = uam::StateSerializer::ToolCallContentPageForFrontend(page->chat.messages.at(0).tool_calls.at(0), 0);
	UAM_ASSERT_EQ(details.at("content").get<std::string>(), full_content);
	UAM_ASSERT(!details.at("hasMore").get<bool>());
	UAM_ASSERT(metadata->messages.empty());
	// This synthetic, actual serializer output is emitted before the expected baseline
	// failure so a later authorized run can pin it for the real TS sanitizer/classifier.
	std::cout << "[UAM_DEFERRED_LAUNCH_PAGE] " << deferred.dump() << '\n';
	// Presentation correlation only: the scoped native run feed still owns card/actions.
	UAM_ASSERT_EQ(tool.value("managedLaunchRunId", std::string{}), run_id);
}

UAM_TEST(DeferredManagedLaunchProjectionRejectsUnacceptedOrForeignReceipts)
{
	const std::string run_id = "8509f0ba-ea06-4068-a6ed-3e1bab09bc53";
	const nlohmann::json accepted{{"runId", run_id}, {"status", "queued"}};
	const nlohmann::json wrapper{{"type", "mcpToolCall"}, {"server", "uam-control"},
	    {"tool", "agent_delegate"}, {"status", "completed"}, {"error", nullptr},
	    {"result", {{"content", nlohmann::json::array({{{"type", "text"}, {"text", accepted.dump()}}})}}}};
	for (int variant = 0; variant < 13; ++variant)
	{
		ToolCall tool;
		tool.id = "rejected-launch-" + std::to_string(variant);
		tool.name = "agent_delegate";
		tool.kind = "mcpToolCall";
		tool.status = "completed";
		tool.args_json = R"({"agentId":"plan","task":"Synthetic negative fixture"})";
		auto result = wrapper;
		if (variant == 0) result["server"] = "foreign-server";
		if (variant == 1) result["tool"] = "agent_status";
		if (variant == 2) result["status"] = "failed";
		if (variant == 3) result["error"] = "refused";
		if (variant == 4) result["result"]["isError"] = true;
		if (variant == 5) result["result"]["content"][0]["text"] = nlohmann::json{{"runId", run_id}, {"status", "running"}}.dump();
		if (variant == 6) result["result"]["content"][0]["text"] = nlohmann::json{{"runId", "not-a-run"}, {"status", "queued"}}.dump();
		if (variant == 7) result["result"]["content"].push_back({{"type", "text"}, {"text", accepted.dump()}});
		if (variant == 8) result["result"]["content"][0]["text"] = std::string(8193, 'x');
		if (variant == 9) tool.name = "agent_wait";
		if (variant == 10) tool.kind = "read";
		if (variant == 11) tool.status = "failed";
		tool.result_text = variant == 12 ? "Arbitrary prose mentioning " + run_id : result.dump();
		ChatSession chat = PageMetadata();
		Message message;
		message.role = MessageRole::Assistant;
		message.tool_calls = {tool};
		chat.messages = {message};
		const auto page = uam::StateSerializer::SerializeMessagePage(chat, 50, {}, true);
		const auto& projected = page.at("messages").at(0).at("toolCalls").at(0);
		UAM_ASSERT(projected.at("contentDeferred").get<bool>());
		UAM_ASSERT(!projected.contains("content"));
		UAM_ASSERT(!projected.contains("managedLaunchRunId"));
	}
}


UAM_TEST(DeferredManagedLaunchCorrelationRequiresProvenServerAcrossKnownNames)
{
	const std::string id = "8509f0ba-ea06-4068-a6ed-3e1bab09bc53";
	const nlohmann::json accepted{{"runId", id}, {"status", "queued"}};
	const std::array<std::string, 3> qualified_names{"uam-control.agent_delegate", "mcp__uam_control__agent_delegate", "mcp__uam-control__agent_delegate"};
	for (const auto& name : qualified_names)
	{
		for (bool foreign : {false, true})
		{
			ToolCall tool;
			tool.id = "qualified-delegate";
			tool.name = name;
			tool.kind = name;
			tool.status = "completed";
			tool.result_text = foreign ? nlohmann::json{{"server", "foreign"}, {"tool", "agent_delegate"}, {"status", "completed"}, {"result", accepted}}.dump() : accepted.dump();
			ChatSession chat = PageMetadata();
			Message message;
			message.role = MessageRole::Assistant;
			message.tool_calls = {tool};
			chat.messages = {message};
			const auto view = uam::StateSerializer::SerializeMessagePage(chat, 50, {}, true);
			const auto& projected = view.at("messages").at(0).at("toolCalls").at(0);
			UAM_ASSERT(!projected.contains("content"));
			if (foreign) UAM_ASSERT(!projected.contains("managedLaunchRunId"));
			else UAM_ASSERT_EQ(projected.value("managedLaunchRunId", std::string{}), id);
		}
	}
	// Some adapters flatten a server-less MCP reply to bare name/kind. Existing
	// content classification is unchanged, but deferral cannot invent provenance.
	for (const std::string kind : {"mcpToolCall", "other"})
	{
		ToolCall tool;
		tool.id = "bare-delegate";
		tool.name = "agent_delegate";
		tool.kind = kind;
		tool.status = "completed";
		tool.result_text = accepted.dump();
		ChatSession chat = PageMetadata();
		Message message;
		message.role = MessageRole::Assistant;
		message.tool_calls = {tool};
		chat.messages = {message};
		const auto view = uam::StateSerializer::SerializeMessagePage(chat, 50, {}, true);
		UAM_ASSERT(!view.at("messages").at(0).at("toolCalls").at(0).contains("managedLaunchRunId"));
	}
}


UAM_TEST(ApprovalBearingManagedLaunchKeepsDeferredCorrelationAndDetailContent)
{
	const std::string id = "8509f0ba-ea06-4068-a6ed-3e1bab09bc53";
	ToolCall tool;
	tool.id = "synthetic-approved-qualified-launch";
	tool.name = "mcp__uam-control__agent_delegate";
	tool.kind = tool.name;
	tool.status = "completed";
	tool.approval_status = "auto_approved";
	tool.args_json = R"({"agentId":"plan","task":"Synthetic approval fixture"})";
	tool.result_text = nlohmann::json{{"runId", id}, {"status", "queued"}}.dump();
	ChatSession chat = PageMetadata();
	Message message;
	message.role = MessageRole::Assistant;
	message.tool_calls = {tool};
	chat.messages = {message};
	const auto inline_page = uam::StateSerializer::SerializeMessagePage(chat, 50, {}, false);
	const auto deferred_page = uam::StateSerializer::SerializeMessagePage(chat, 50, {}, true);
	const auto& inline_tool = inline_page.at("messages").at(0).at("toolCalls").at(0);
	const auto& deferred_tool = deferred_page.at("messages").at(0).at("toolCalls").at(0);
	const std::string expected_content = "Approval: auto_approved\n\nArguments:\n" + tool.args_json + "\n\nResult:\n" + tool.result_text;
	UAM_ASSERT_EQ(inline_tool.at("content").get<std::string>(), expected_content);
	UAM_ASSERT_EQ(inline_tool.at("approvalStatus").get<std::string>(), tool.approval_status);
	UAM_ASSERT_EQ(uam::StateSerializer::ToolCallContentPageForFrontend(tool, 0).at("content").get<std::string>(), expected_content);
	UAM_ASSERT(deferred_tool.at("contentDeferred").get<bool>());
	UAM_ASSERT(!deferred_tool.contains("content"));
	UAM_ASSERT_EQ(deferred_tool.at("approvalStatus").get<std::string>(), tool.approval_status);
	std::cout << "[UAM_APPROVED_LAUNCH_INLINE_PAGE] " << inline_page.dump() << '\n';
	std::cout << "[UAM_APPROVED_LAUNCH_DEFERRED_PAGE] " << deferred_page.dump() << '\n';
	UAM_ASSERT_EQ(deferred_tool.value("managedLaunchRunId", std::string{}), id);
}

UAM_TEST(DeferredManagedLaunchBoundariesRetainExactReceiptFences)
{
	const std::string id = "8509f0ba-ea06-4068-a6ed-3e1bab09bc53";
	const std::string name = "mcp__uam-control__agent_delegate";
	const nlohmann::json accepted{{"runId", id}, {"status", "queued"}};
	struct Boundary
	{
		std::string label;
		std::string result;
		std::string kind;
		bool accepted;
	};
	std::vector<Boundary> boundaries;
	for (std::size_t size : {std::size_t{8192}, std::size_t{8193}})
	{
		std::string text = accepted.dump();
		text.append(size - text.size(), ' ');
		const auto envelope = nlohmann::json{{"content", nlohmann::json::array({{{"type", "text"}, {"text", text}}})}};
		boundaries.push_back({"inner-" + std::to_string(size), envelope.dump(), name, size == 8192});
	}
	for (std::size_t size : {std::size_t{65536}, std::size_t{65537}})
	{
		std::string text = accepted.dump();
		text.append(size - text.size(), ' ');
		boundaries.push_back({"total-" + std::to_string(size), std::move(text), name, size == 65536});
	}
	for (int wrappers : {4, 5})
	{
		auto value = accepted;
		for (int depth = 0; depth < wrappers; ++depth) value = nlohmann::json{{"result", value}};
		boundaries.push_back({"result-wrappers-" + std::to_string(wrappers), value.dump(), name, wrappers == 4});
	}
	boundaries.push_back({"kind-equals-qualified-name", accepted.dump(), name, true});
	boundaries.push_back({"kind-mismatches-qualified-name", accepted.dump(), "agent_delegate", false});
	const std::vector<nlohmann::json> errors{nullptr, false, "", 0, 0.0, true, "refused", 1, nlohmann::json::object(), nlohmann::json::array()};
	for (std::size_t index = 0; index < errors.size(); ++index)
	{
		auto value = accepted;
		value["error"] = errors[index];
		boundaries.push_back({"error-truthiness-" + std::to_string(index), value.dump(), name, index < 5});
	}
	for (const auto& boundary : boundaries)
	{
		ToolCall tool;
		tool.id = boundary.label;
		tool.name = name;
		tool.kind = boundary.kind;
		tool.status = "completed";
		tool.result_text = boundary.result;
		ChatSession chat = PageMetadata();
		Message message;
		message.role = MessageRole::Assistant;
		message.tool_calls = {tool};
		chat.messages = {message};
		const auto page = uam::StateSerializer::SerializeMessagePage(chat, 50, {}, true);
		const auto& projected = page.at("messages").at(0).at("toolCalls").at(0);
		UAM_ASSERT(projected.at("contentDeferred").get<bool>());
		UAM_ASSERT(!projected.contains("content"));
		if (boundary.accepted) UAM_ASSERT_EQ(projected.value("managedLaunchRunId", std::string{}), id);
		else UAM_ASSERT(!projected.contains("managedLaunchRunId"));
	}
}
