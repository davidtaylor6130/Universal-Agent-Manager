#include "test_harness.h"
#include "app/local_chat_bundle_service.h"
#include "cef/state_serializer.h"
#include "common/chat/chat_message_page_index.h"
#include "common/chat/message_attachment_sources.h"
#include <nlohmann/json.hpp>

using namespace uam_test;

namespace
{
	constexpr const char* kInputOne = "11111111-1111-4111-8111-111111111111";
	constexpr const char* kInputTwo = "22222222-2222-4222-8222-222222222222";
	constexpr const char* kOrigin = "ordinary-cef-request-correlation";
	void FingerprintHashString(std::uint64_t& hash, const std::string& value)
	{
		const std::uint64_t value_hash = std::hash<std::string>{}(value);
		uam::hashing::UpdateFnv1a64(hash, reinterpret_cast<const unsigned char*>(&value_hash), sizeof(value_hash));
	}
	void FingerprintHashBool(std::uint64_t& hash, bool value)
	{
		const unsigned char byte = value ? 1 : 0;
		uam::hashing::UpdateFnv1a64(hash, &byte, 1);
	}
	// Frozen pre-schema compatibility oracle, copied verbatim except its function/namespace names.
	void AppendLegacyMessageDigest(std::uint64_t& hash, const Message& message)
	{
		FingerprintHashString(hash, (message.role == MessageRole::Assistant ? "assistant" : message.role == MessageRole::System ? "system" : "user"));
		FingerprintHashString(hash, message.created_at);
		if (message.managed_origin_run_id)
		{
			FingerprintHashString(hash, "managed_origin_run_id");
			FingerprintHashString(hash, *message.managed_origin_run_id);
		}
		FingerprintHashString(hash, message.provider);
		FingerprintHashString(hash, message.model_id);
		FingerprintHashString(hash, message.content);
		FingerprintHashString(hash, std::to_string(message.provider_event_ids.size()));
		for (const std::string& id : message.provider_event_ids) FingerprintHashString(hash, id);
		FingerprintHashString(hash, std::to_string(message.processing_time_ms));
		FingerprintHashString(hash, message.checkpoint_sha);
		FingerprintHashString(hash, message.checkpoint_parent_sha);
		FingerprintHashString(hash, message.thoughts);
		FingerprintHashString(hash, message.plan_summary);
		FingerprintHashString(hash, std::to_string(message.tool_calls.size()));
		for (const ToolCall& tool_call : message.tool_calls)
		{
			FingerprintHashString(hash, tool_call.id);
			FingerprintHashString(hash, tool_call.name);
			FingerprintHashString(hash, tool_call.kind);
			FingerprintHashString(hash, tool_call.approval_status);
			FingerprintHashString(hash, tool_call.task_id);
			FingerprintHashString(hash, tool_call.child_activity);
			FingerprintHashString(hash, tool_call.status);
			// The native refresh guard needs content changes even when output is deferred.
			FingerprintHashString(hash, tool_call.args_json);
			FingerprintHashString(hash, tool_call.result_text);
			FingerprintHashBool(hash, tool_call.is_sub_agent);
			FingerprintHashString(hash, tool_call.sub_agent_id);
			FingerprintHashString(hash, tool_call.sub_agent_title);
			// Preserve legacy hashes when the optional metadata is absent.
			if (tool_call.sub_agent_children_authoritative) FingerprintHashString(hash, "sub_agent_children_authoritative");
			if (!tool_call.sub_agent_children.empty())
			{
				FingerprintHashString(hash, "sub_agent_children");
				FingerprintHashString(hash, std::to_string(tool_call.sub_agent_children.size()));
				for (const SubAgentChild& child : tool_call.sub_agent_children)
				{
					FingerprintHashString(hash, child.id);
					FingerprintHashString(hash, child.title);
					FingerprintHashString(hash, child.status);
					if (child.started_at_epoch_ms) { FingerprintHashString(hash, "started_at_epoch_ms"); FingerprintHashString(hash, std::to_string(*child.started_at_epoch_ms)); }
					if (child.finished_at_epoch_ms) { FingerprintHashString(hash, "finished_at_epoch_ms"); FingerprintHashString(hash, std::to_string(*child.finished_at_epoch_ms)); }
					if (child.duration_ms) { FingerprintHashString(hash, "duration_ms"); FingerprintHashString(hash, std::to_string(*child.duration_ms)); }
				}
			}
		}
		FingerprintHashString(hash, std::to_string(message.plan_entries.size()));
		for (const MessagePlanEntry& entry : message.plan_entries)
		{
			FingerprintHashString(hash, entry.content);
			FingerprintHashString(hash, entry.priority);
			FingerprintHashString(hash, entry.status);
		}
		FingerprintHashString(hash, std::to_string(message.blocks.size()));
		for (const MessageBlock& block : message.blocks)
		{
			FingerprintHashString(hash, block.type);
			FingerprintHashString(hash, block.text);
			FingerprintHashString(hash, block.tool_call_id);
			FingerprintHashString(hash, block.request_id_json);
			FingerprintHashString(hash, std::to_string(block.compaction_tokens_before));
			FingerprintHashString(hash, std::to_string(block.compaction_tokens_after));
		}
		FingerprintHashString(hash, std::to_string(message.markdown_store_files.size()));
		for (const std::string& file : message.markdown_store_files)
		{
			FingerprintHashString(hash, file);
		}
		FingerprintHashString(hash, std::to_string(message.markdown_store_prompt_blocks.size()));
		for (const std::string& prompt_block : message.markdown_store_prompt_blocks)
		{
			FingerprintHashString(hash, prompt_block);
		}
		FingerprintHashString(hash, std::to_string(message.attachments.size()));
		for (const MessageAttachment& attachment : message.attachments)
		{
			FingerprintHashString(hash, attachment.id);
			FingerprintHashString(hash, attachment.name);
			FingerprintHashString(hash, attachment.kind);
			FingerprintHashString(hash, attachment.mime_type);
			FingerprintHashString(hash, attachment.path);
			FingerprintHashString(hash, std::to_string(attachment.size_bytes));
			FingerprintHashBool(hash, attachment.copied);
			FingerprintHashString(hash, attachment.preview_key);
			FingerprintHashString(hash, attachment.preview_origin_path);
			// Preserve legacy hashes; native source/provenance participates in page identity.
			if (attachment.source != AttachmentSource::LegacyPath)
			{
				FingerprintHashString(hash, std::string(uam::attachment_sources::Name(attachment.source)));
				FingerprintHashString(hash, attachment.source_uri);
				FingerprintHashString(hash, attachment.origin_execution_host_id);
				FingerprintHashString(hash, attachment.native_part_key);
				FingerprintHashString(hash, attachment.native_tool_call_id);
				FingerprintHashString(hash, attachment.native_source_field);
				FingerprintHashString(hash, attachment.native_asset_key);
				FingerprintHashString(hash, attachment.content_digest);
			}
		}
		FingerprintHashBool(hash, message.interrupted);
		FingerprintHashString(hash, message.stop_reason);
		FingerprintHashBool(hash, message.acp_prompt_not_sent);
		// Keep existing page hashes stable until an explicit removal changes a row.
		if (message.acp_prompt_discarded) FingerprintHashBool(hash, true);
		FingerprintHashBool(hash, message.priority_steer);
		FingerprintHashBool(hash, message.continues_turn);
	}

	ChatSession ProvenanceChat(const std::string& id)
	{
		ChatSession chat;
		chat.id = id; chat.provider_id = "codex-cli"; chat.title = "Provenance fixture";
		chat.created_at = "2026-10-21T12:00:00Z";
		Message row;
		row.role = MessageRole::User; row.content = "Exact authored bytes\nkeep trailing space "; row.created_at = chat.created_at;
		row.native_input_id = kInputOne; row.origin_request_id = kOrigin; row.acp_prompt_not_sent = true;
		chat.messages.push_back(row);
		uam::AcpQueuedUserPromptState prompt;
		prompt.id = "existing-native-queue-id"; prompt.revision = 7; prompt.text = "Exact queued bytes\nretain ";
		prompt.native_input_id = kInputTwo; prompt.origin_request_id = kOrigin;
		chat.acp_queued_prompts.push_back(prompt); chat.acp_queue_held = true;
		return chat;
	}
}

UAM_TEST(InputProvenanceLegacyAbsentDigestMatchesFrozenPreSchemaAlgorithm)
{
	ChatSession chat = ProvenanceChat("legacy-provenance");
	Message row = chat.messages.front(); row.native_input_id.reset(); row.origin_request_id.reset();
	ToolCall tool; tool.id = "legacy-tool"; tool.name = "read"; tool.result_text = "Legacy tool output"; row.tool_calls.push_back(tool);
	row.markdown_store_files.push_back("AGENTS.md"); row.managed_origin_run_id = kInputTwo;
	std::uint64_t expected = uam::hashing::kFnv1a64OffsetBasis, actual = expected;
	AppendLegacyMessageDigest(expected, row);
	uam::chat_pages::AppendMessageDigest(actual, row);
	UAM_ASSERT_EQ(actual, expected);
	chat.messages = {row};
	const auto wire = uam::StateSerializer::SerializeSession(chat);
	UAM_ASSERT(!wire["messages"][0].contains("nativeInputId"));
	UAM_ASSERT(!wire["messages"][0].contains("originRequestId"));
}

UAM_TEST(InputProvenanceRealPrimaryAndQueueJournalRoundTrip)
{
	TempDir temp("uam-input-provenance-journal");
	ChatSession chat = ProvenanceChat("provenance-roundtrip");
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	chat.acp_queued_prompts.front().text += "new journal revision";
	bool full_save_required = false;
	UAM_ASSERT(ChatRepository::SaveQueuedPromptOutbox(temp.root, chat, &full_save_required));
	UAM_ASSERT(!full_save_required && !chat.acp_queue_storage_revision.empty());
	const auto loaded = ChatRepository::LoadLocalChat(temp.root, chat.id, true, nullptr, false);
	UAM_ASSERT(loaded && loaded->messages.size() == 1 && loaded->acp_queued_prompts.size() == 1);
	UAM_ASSERT(loaded->messages.front() == chat.messages.front());
	UAM_ASSERT(loaded->acp_queued_prompts.front() == chat.acp_queued_prompts.front());
	UAM_ASSERT_EQ(loaded->acp_queued_prompts.front().id, std::string("existing-native-queue-id"));
	UAM_ASSERT_EQ(loaded->acp_queued_prompts.front().revision, 7);
	const auto row = uam::StateSerializer::SerializeSession(*loaded)["messages"][0];
	UAM_ASSERT_EQ(row["nativeInputId"].get<std::string>(), std::string(kInputOne));
	UAM_ASSERT_EQ(row["originRequestId"].get<std::string>(), std::string(kOrigin));
}

UAM_TEST(InputProvenanceMalformedOptionalFieldsRetainAuthoredRowsAndCarriers)
{
	const std::vector<nlohmann::json> invalid_native = {nullptr, 12, true, nlohmann::json::object(), "", "not-a-uuid", std::string(129, 'a')};
	const std::vector<nlohmann::json> invalid_origin = {nullptr, 12, true, nlohmann::json::array(), "", std::string(129, 'a'), "line\nbreak", "non-ascii-é"};
	for (const bool native_field : {false, true})
	{
		for (const auto& invalid : native_field ? invalid_native : invalid_origin)
		{
			TempDir temp("uam-input-provenance-malformed");
			ChatSession chat = ProvenanceChat("provenance-malformed");
			UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
			const auto path = AppPaths::UamChatFilePath(temp.root, chat.id);
			auto document = nlohmann::json::parse(ReadFile(path));
			const char* field = native_field ? "native_input_id" : "origin_request_id";
			document["messages"][0][field] = invalid; document["acp_queued_prompts"][0][field] = invalid;
			UAM_ASSERT(uam::io::WriteTextFile(path, document.dump(2)));
			const auto loaded = ChatRepository::LoadLocalChat(temp.root, chat.id, true, nullptr, false);
			UAM_ASSERT(loaded && loaded->messages.size() == 1 && loaded->acp_queued_prompts.size() == 1);
			UAM_ASSERT_EQ(loaded->messages.front().content, chat.messages.front().content);
			UAM_ASSERT_EQ(loaded->acp_queued_prompts.front().text, chat.acp_queued_prompts.front().text);
			UAM_ASSERT_EQ(loaded->acp_queued_prompts.front().id, chat.acp_queued_prompts.front().id);
			UAM_ASSERT_EQ(loaded->acp_queued_prompts.front().revision, 7);
			UAM_ASSERT(loaded->acp_queue_held);
			if (native_field)
			{
				UAM_ASSERT(!loaded->messages.front().native_input_id && !loaded->acp_queued_prompts.front().native_input_id);
				UAM_ASSERT(loaded->messages.front().origin_request_id == chat.messages.front().origin_request_id);
			}
			else
			{
				UAM_ASSERT(!loaded->messages.front().origin_request_id && !loaded->acp_queued_prompts.front().origin_request_id);
				UAM_ASSERT(loaded->messages.front().native_input_id == chat.messages.front().native_input_id);
			}
		}
	}
}

UAM_TEST(InputProvenanceOnlyChangeInvalidatesMessagePageAndQueueFingerprint)
{
	ChatSession chat = ProvenanceChat("provenance-projection");
	const std::string before = uam::StateSerializer::MessageDigest(chat);
	const auto old_message = chat.messages.front();
	chat.messages.front().native_input_id = kInputTwo;
	UAM_ASSERT(!(chat.messages.front() == old_message));
	UAM_ASSERT(uam::StateSerializer::MessageDigest(chat) != before);
	const auto page = uam::StateSerializer::SerializeMessagePage(chat, 10, std::nullopt, false, before);
	UAM_ASSERT(!page.value("unchanged", false));
	UAM_ASSERT_EQ(page["messages"][0]["nativeInputId"].get<std::string>(), std::string(kInputTwo));
	const auto queue_before = ChatRepository::QueueAdmissionFingerprint(chat);
	const auto old_prompt = chat.acp_queued_prompts.front();
	chat.acp_queued_prompts.front().origin_request_id = "different-origin";
	UAM_ASSERT(!(chat.acp_queued_prompts.front() == old_prompt));
	UAM_ASSERT(ChatRepository::QueueAdmissionFingerprint(chat) != queue_before);
	UAM_ASSERT_EQ(chat.acp_queued_prompts.front().id, old_prompt.id);
	UAM_ASSERT_EQ(chat.acp_queued_prompts.front().revision, old_prompt.revision);
}

UAM_TEST(InputProvenanceAbsentBatchScalarNeverBorrowsAuthoredRowId)
{
	TempDir temp("uam-input-provenance-batch");
	ChatSession chat = ProvenanceChat("provenance-batch");
	chat.acp_queued_prompts.front().native_input_id.reset(); chat.acp_queued_prompts.front().origin_request_id.reset();
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const auto loaded = ChatRepository::LoadLocalChat(temp.root, chat.id, true, nullptr, false);
	UAM_ASSERT(loaded && !loaded->acp_queued_prompts.front().native_input_id && !loaded->acp_queued_prompts.front().origin_request_id);
	UAM_ASSERT(loaded->messages.front().native_input_id == chat.messages.front().native_input_id);
}

UAM_TEST(InputProvenanceDuplicateOriginsRemainDistinctThroughPassiveImport)
{
	TempDir temp("uam-input-provenance-copy");
	ChatSession chat = ProvenanceChat("provenance-original");
	Message second = chat.messages.front(); second.native_input_id = kInputTwo; chat.messages.push_back(second);
	UAM_ASSERT(chat.messages[0].origin_request_id == chat.messages[1].origin_request_id);
	UAM_ASSERT(chat.messages[0].native_input_id != chat.messages[1].native_input_id);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root / "source", chat));
	UAM_ASSERT(uam::LocalChatBundleService::Export(temp.root / "source", temp.root / "bundle").ok);
	const auto imported = uam::LocalChatBundleService::Import(temp.root / "target", temp.root / "bundle");
	UAM_ASSERT(imported.ok && imported.imported_count == 1);
	const auto loaded = ChatRepository::LoadLocalChat(temp.root / "target", imported.items.front().imported_id, true, nullptr, false);
	UAM_ASSERT(loaded && loaded->imported_read_only && loaded->messages.size() == 2);
	UAM_ASSERT(loaded->messages[0].native_input_id == chat.messages[0].native_input_id);
	UAM_ASSERT(loaded->messages[1].native_input_id == chat.messages[1].native_input_id);
	UAM_ASSERT(loaded->messages[0].origin_request_id == loaded->messages[1].origin_request_id);
	UAM_ASSERT(loaded->native_session_id.empty() && loaded->acp_queued_prompts.empty() && !loaded->acp_queue_held);
	// Preservation is historical provenance. This test never treats IDs as admission or current-action proof.
}

UAM_TEST(InputProvenanceValidatedBackupRecoveryRetainsOriginalProvenance)
{
	TempDir temp("uam-input-provenance-backup");
	ChatSession chat = ProvenanceChat("provenance-backup");
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const auto original_message = chat.messages.front();
	const auto original_prompt = chat.acp_queued_prompts.front();
	chat.messages.front().native_input_id = kInputTwo;
	chat.acp_queued_prompts.front().origin_request_id = "later-origin";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const auto path = AppPaths::UamChatFilePath(temp.root, chat.id);
	const auto backup = nlohmann::json::parse(ReadFile(path.string() + ".bak"));
	UAM_ASSERT_EQ(backup["messages"][0]["native_input_id"].get<std::string>(), *original_message.native_input_id);
	UAM_ASSERT_EQ(backup["messages"][0]["origin_request_id"].get<std::string>(), *original_message.origin_request_id);
	UAM_ASSERT_EQ(backup["acp_queued_prompts"][0]["native_input_id"].get<std::string>(), *original_prompt.native_input_id);
	UAM_ASSERT_EQ(backup["acp_queued_prompts"][0]["origin_request_id"].get<std::string>(), *original_prompt.origin_request_id);
	UAM_ASSERT(uam::io::WriteTextFile(path, "{broken-primary"));
	std::string warning;
	// The bulk loader owns validated backup recovery; the single-chat reader only parses its primary.
	const auto recovered_chats = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(recovered_chats.size(), std::size_t{1});
	UAM_ASSERT_EQ(recovered_chats.front().id, chat.id);
	const auto recovered = ChatRepository::LoadLocalChat(temp.root, chat.id, true, &warning, false);
	UAM_ASSERT(recovered && recovered->messages.size() == 1 && recovered->acp_queued_prompts.size() == 1);
	UAM_ASSERT(recovered->messages.front().native_input_id == original_message.native_input_id);
	UAM_ASSERT(recovered->messages.front().origin_request_id == original_message.origin_request_id);
	UAM_ASSERT(recovered->acp_queued_prompts.front().native_input_id == original_prompt.native_input_id);
	UAM_ASSERT(recovered->acp_queued_prompts.front().origin_request_id == original_prompt.origin_request_id);
	UAM_ASSERT_EQ(recovered->messages.front().content, original_message.content);
	UAM_ASSERT_EQ(recovered->acp_queued_prompts.front().text, original_prompt.text);
}
