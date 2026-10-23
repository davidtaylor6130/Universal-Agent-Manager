#include "test_harness.h"
#include "common/chat/native_attachment_assets.h"
#include "common/provider/gemini_history_loader.h"
#include "core/native_import_content.h"

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))

namespace
{
	using Json = nlohmann::json;
	enum class BatchMediaBody { InlineOnly, Mixed, Text, OpaqueUri };
	const std::string kBatchMediaSession = "dddddddd-dddd-4ddd-8ddd-dddddddddddd";
	const std::string kBatchMediaPng = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==";
	const std::string kBatchMediaUri = "https://assets.invalid/opaque-native-original.pdf";

	struct ExpectedBatchPart
	{
		std::size_t message = 0;
		MessageAttachment metadata;
	};

	struct GeminiMediaBatchFixture
	{
		uam_test::TempDir temp{"uam-gemini-discovered-media"};
		uam::AppState app;
		const ChatHistorySyncService service;
		std::shared_ptr<GeminiHistoryParseContext> context = std::make_shared<GeminiHistoryParseContext>();
		fs::path source = temp.root / "native" / "session.json";
		std::string source_bytes;
		bool deletion_file_existed = false;
		std::string deletion_file_bytes;
		ChatSession identity;
		std::vector<std::string> expected_content;
		std::vector<ExpectedBatchPart> expected_parts;

		GeminiMediaBatchFixture()
		{
			app.data_root = temp.root / "data";
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			const auto workspace = temp.root / "workspace";
			fs::create_directories(app.data_root);
			fs::create_directories(workspace);
			fs::create_directories(source.parent_path());
			app.folders.push_back({"media-workspace", "Media Workspace", workspace.string(), false, "local"});
		}

		ChatSession Parse(BatchMediaBody body)
		{
			const auto inline_part = []
			{
				return Json{{"inlineData", {{"mimeType", "image/png"}, {"data", kBatchMediaPng}}}};
			};
			Json parts = Json::array();
			if (body == BatchMediaBody::Text || body == BatchMediaBody::Mixed) parts.push_back({{"text", "Preserve the authored native request"}});
			if (body == BatchMediaBody::InlineOnly || body == BatchMediaBody::Mixed) parts.push_back(inline_part());
			if (body == BatchMediaBody::OpaqueUri) parts.push_back({{"fileData", {{"mimeType", "application/pdf"}, {"fileUri", kBatchMediaUri}}}});
			Json messages = Json::array({{{"id", "batch-user"}, {"type", "user"}, {"timestamp", "2026-10-17T12:00:01Z"}, {"content", std::move(parts)}}});
			if (body == BatchMediaBody::Mixed)
			{
				// Exercise an actual native tool-result association, not only an empty tool ID.
				Json result = Json::array({{{"functionResponse", {{"id", "batch-read-tool"}, {"name", "read_file"}, {"response", {{"output", "Read the image"}}}}}}, inline_part()});
				messages.push_back({{"id", "batch-answer"}, {"type", "gemini"}, {"timestamp", "2026-10-17T12:00:02Z"}, {"content", "Native answer"},
				    {"toolCalls", Json::array({{{"id", "batch-read-tool"}, {"name", "read_file"}, {"args", {{"path", "not-authorized-as-original.png"}}},
				        {"status", "success"}, {"timestamp", "2026-10-17T12:00:02Z"}, {"result", std::move(result)}}})}});
			}
			const Json document{{"sessionId", kBatchMediaSession}, {"projectHash", "private-media-project"},
			    {"workspace", app.folders.front().directory}, {"startTime", "2026-10-17T12:00:00Z"},
			    {"lastUpdated", "2026-10-17T12:00:02Z"}, {"messages", std::move(messages)}};
			source_bytes = document.dump();
			UAM_ASSERT(uam::io::WriteTextFile(source, source_bytes));
			GeminiJsonHistoryStoreOptions options;
			options.execution_host_id = "local";
			options.context = context;
			auto parsed = GeminiJsonHistoryStore::ParseFileResult(source, ProviderProfileStore::DefaultGeminiProfile(), options);
			UAM_ASSERT_EQ(parsed.status, GeminiHistoryParseStatus::Content);
			UAM_ASSERT(parsed.chat && parsed.error.empty());
			ChatSession candidate = std::move(*parsed.chat);
			// The direct parser maps content; discovery normally supplies its attested
			// project's workspace (gemini_history_loader.cpp, Load selected_project).
			candidate.workspace_directory = app.folders.front().directory;
			candidate.execution_host_id = options.execution_host_id;
			UAM_ASSERT(candidate.messages_loaded);
			UAM_ASSERT_EQ(candidate.id, kBatchMediaSession);
			UAM_ASSERT_EQ(candidate.native_session_id, kBatchMediaSession);
			UAM_ASSERT_EQ(candidate.provider_id, std::string("gemini-cli"));
			UAM_ASSERT_EQ(service.StartupImportWorkspaceOwner(app, candidate), &app.folders.front());
			UAM_ASSERT(uam::HasImportedConversationContent(candidate.messages));
			expected_content.clear();
			expected_parts.clear();
			for (std::size_t row = 0; row < candidate.messages.size(); ++row)
			{
				expected_content.push_back(candidate.messages[row].content);
				for (const auto& attachment : candidate.messages[row].attachments)
				{
					ExpectedBatchPart expected{row, attachment};
					// Expected metadata must not retain an extra original-payload alias.
					expected.metadata.native_inline_payload.reset();
					expected_parts.push_back(std::move(expected));
				}
			}
			const std::size_t originals = body == BatchMediaBody::Mixed ? 2 : body == BatchMediaBody::InlineOnly ? 1 : 0;
			UAM_ASSERT_EQ(context->payload_budget->UsedCount(), originals);
			for (const auto& expected : expected_parts)
			{
				UAM_ASSERT(expected.metadata.path.empty());
				UAM_ASSERT(!expected.metadata.native_part_key.empty());
				if (body == BatchMediaBody::OpaqueUri) UAM_ASSERT_EQ(expected.metadata.source, AttachmentSource::ProviderUri);
				else UAM_ASSERT_EQ(expected.metadata.source, AttachmentSource::NativeInlinePending);
			}
			if (body == BatchMediaBody::InlineOnly) UAM_ASSERT(candidate.messages.front().content.empty());
			if (body == BatchMediaBody::Mixed)
			{
				UAM_ASSERT_EQ(expected_parts.size(), std::size_t(2));
				UAM_ASSERT_EQ(expected_parts.back().metadata.native_tool_call_id, std::string("batch-read-tool"));
			}
			// Metadata-only identity for tombstone API calls; no pending payload alias.
			identity.id = candidate.id;
			identity.native_session_id = candidate.native_session_id;
			identity.provider_id = candidate.provider_id;
			identity.execution_host_id = candidate.execution_host_id;
			identity.workspace_directory = candidate.workspace_directory;
			return candidate;
		}

		ChatHistorySyncService::LocalHistoryDiscovery Stage(BatchMediaBody body)
		{
			const auto deletion_file = app.data_root / "native-import-tombstones.json";
			deletion_file_existed = fs::exists(deletion_file);
			deletion_file_bytes = uam_test::ReadFile(deletion_file);
			ChatHistorySyncService::LocalHistoryDiscovery discovery;
			discovery.chats.push_back(Parse(body));
			const auto admitted = service.ImportDiscoveredProviderChatsBatch(app, {}, discovery, 8, ChatHistorySyncService::ImportPolicy::SelectedNewChatsOnly);
			UAM_ASSERT(admitted.success && admitted.imported_count == 0);
			UAM_ASSERT(admitted.confirmed_empty_native_keys.empty());
			UAM_ASSERT(discovery.chats.empty() && discovery.preparation.valid());
			UAM_ASSERT_EQ(discovery.preparation.wait_for(std::chrono::seconds(10)), std::future_status::ready);
			{
				const auto prepared = discovery.preparation.get();
				UAM_ASSERT(prepared);
				UAM_ASSERT_EQ(prepared->expected_data_root, app.data_root);
				UAM_ASSERT_EQ(prepared->chats.size(), std::size_t(1));
				const auto& candidate = prepared->chats.front();
				UAM_ASSERT(!candidate.skipped && !candidate.confirmed_empty);
				UAM_ASSERT_EQ(candidate.chat.id, identity.id);
				UAM_ASSERT_EQ(candidate.chat.native_session_id, identity.native_session_id);
				UAM_ASSERT_EQ(candidate.chat.workspace_directory, app.folders.front().directory);
				UAM_ASSERT(candidate.expected_folder && candidate.expected_folder->id == app.folders.front().id);
			}
			// Preparation and its originals are not a primary-publication receipt.
			UAM_ASSERT(app.chats.empty());
			UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(app.data_root, identity.id)));
			UAM_ASSERT_EQ(uam_test::ReadFile(source), source_bytes);
			AssertNoNewDeletionReceipt();
			return discovery;
		}

		ChatHistorySyncService::ImportResult Drain(ChatHistorySyncService::LocalHistoryDiscovery& discovery)
		{
			ChatHistorySyncService::ImportResult result;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (discovery.Pending())
			{
				UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
				result.Merge(service.ImportDiscoveredProviderChatsBatch(app, {}, discovery, 8, ChatHistorySyncService::ImportPolicy::SelectedNewChatsOnly));
				std::this_thread::yield();
			}
			return result;
		}

		void AssertNoNewDeletionReceipt() const
		{
			const auto file = app.data_root / "native-import-tombstones.json";
			UAM_ASSERT_EQ(fs::exists(file), deletion_file_existed);
			UAM_ASSERT_EQ(uam_test::ReadFile(file), deletion_file_bytes);
			std::vector<ChatSession> probe{identity};
			UAM_ASSERT(service.FilterDeletedNativeHistory(app.data_root, probe));
			UAM_ASSERT_EQ(probe.size(), std::size_t(1));
		}

		void AssertPayloadReleased() const
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (context->payload_budget->UsedCount() != 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
			UAM_ASSERT_EQ(context->payload_budget->UsedCount(), std::size_t(0));
			UAM_ASSERT_EQ(context->payload_budget->UsedBytes(), std::size_t(0));
		}

		void AssertPublished(ChatHistorySyncService::LocalHistoryDiscovery& discovery, bool original)
		{
			const auto result = Drain(discovery);
			// On current production this is the intended missing-media-preparation RED,
			// after Stage has proved content, workspace, candidate and async admission.
			UAM_ASSERT(result.success);
			UAM_ASSERT_EQ(result.imported_count, 1);
			UAM_ASSERT(result.confirmed_empty_native_keys.empty());
			UAM_ASSERT_EQ(app.chats.size(), std::size_t(1));
			UAM_ASSERT_EQ(app.chats.front().id, identity.id);
			const auto loaded = ChatRepository::LoadLocalChat(app.data_root, identity.id, true);
			UAM_ASSERT(loaded && loaded->messages.size() == expected_content.size());
			UAM_ASSERT_EQ(loaded->provider_id, identity.provider_id);
			UAM_ASSERT_EQ(loaded->native_session_id, identity.native_session_id);
			UAM_ASSERT_EQ(loaded->folder_id, app.folders.front().id);
			UAM_ASSERT_EQ(loaded->workspace_directory, identity.workspace_directory);
			std::size_t total_attachments = 0;
			for (std::size_t row = 0; row < loaded->messages.size(); ++row)
			{
				UAM_ASSERT_EQ(loaded->messages[row].content, expected_content[row]);
				total_attachments += loaded->messages[row].attachments.size();
			}
			UAM_ASSERT_EQ(total_attachments, expected_parts.size());
			std::string original_bytes;
			UAM_ASSERT(uam::base64::Decode(kBatchMediaPng, original_bytes));
			for (const auto& expected : expected_parts)
			{
				const auto& attachments = loaded->messages[expected.message].attachments;
				const auto found = std::ranges::find(attachments, expected.metadata.id, &MessageAttachment::id);
				UAM_ASSERT(found != attachments.end());
				UAM_ASSERT_EQ(found->native_part_key, expected.metadata.native_part_key);
				UAM_ASSERT_EQ(found->native_tool_call_id, expected.metadata.native_tool_call_id);
				UAM_ASSERT_EQ(found->native_source_field, expected.metadata.native_source_field);
				UAM_ASSERT_EQ(found->origin_execution_host_id, expected.metadata.origin_execution_host_id);
				UAM_ASSERT_EQ(found->mime_type, expected.metadata.mime_type);
				UAM_ASSERT(found->path.empty() && !found->native_inline_payload);
				if (original)
				{
					UAM_ASSERT_EQ(found->source, AttachmentSource::OwnedNativeAsset);
					UAM_ASSERT_EQ(found->content_digest, expected.metadata.content_digest);
					UAM_ASSERT_EQ(found->size_bytes, expected.metadata.size_bytes);
					UAM_ASSERT(uam::native_assets::IsKeyForOwner(identity.id, found->native_asset_key));
					const auto read = uam::native_assets::ReadOwned(app.data_root, identity.id, found->native_asset_key);
					UAM_ASSERT(read.ok && read.error.empty());
					UAM_ASSERT_EQ(read.bytes, original_bytes);
				}
				else
				{
					UAM_ASSERT_EQ(found->source, AttachmentSource::ProviderUri);
					UAM_ASSERT_EQ(found->source_uri, kBatchMediaUri);
					UAM_ASSERT(found->native_asset_key.empty());
				}
			}
			if (!original) UAM_ASSERT(!fs::exists(app.data_root / std::string(uam::native_assets::kStoreDirectory)));
			const auto saved_bytes = uam_test::ReadFile(AppPaths::UamChatFilePath(app.data_root, identity.id));
			UAM_ASSERT(saved_bytes.find(kBatchMediaPng) == std::string::npos);
			UAM_ASSERT_EQ(uam_test::ReadFile(source), source_bytes);
			AssertNoNewDeletionReceipt();
			AssertPayloadReleased();
		}
	};
}

UAM_TEST(GeminiDiscoveredBatchPublishesInlineOnlyOriginalAfterPrimary)
{
	GeminiMediaBatchFixture fixture;
	auto discovery = fixture.Stage(BatchMediaBody::InlineOnly);
	fixture.AssertPublished(discovery, true);
}

UAM_TEST(GeminiDiscoveredBatchPublishesMixedTextAndToolOriginalsAfterPrimary)
{
	GeminiMediaBatchFixture fixture;
	auto discovery = fixture.Stage(BatchMediaBody::Mixed);
	fixture.AssertPublished(discovery, true);
}

UAM_TEST(GeminiDiscoveredBatchPreservesTextAndOpaqueUriControls)
{
	for (const auto body : {BatchMediaBody::Text, BatchMediaBody::OpaqueUri})
	{
		GeminiMediaBatchFixture fixture;
		auto discovery = fixture.Stage(body);
		fixture.AssertPublished(discovery, false);
	}
}

UAM_TEST(GeminiDiscoveredBatchRejectsTombstoneAfterPreparationWithoutEmptyReceipt)
{
	// Text first independently exercises the existing publication guard on the
	// unmodified source; inline repeats it once real media preparation exists.
	for (const auto body : {BatchMediaBody::Text, BatchMediaBody::InlineOnly})
	{
		GeminiMediaBatchFixture fixture;
		auto discovery = fixture.Stage(body);
		{
			const auto prepared = discovery.preparation.get();
			UAM_ASSERT(prepared && prepared->chats.size() == 1);
			// An inline failure here is a preparation prerequisite failure, not proof
			// that the later stale-publication guard was reached or is defective.
			UAM_ASSERT(prepared->chats.front().error.empty());
		}
		std::vector<std::string> keys;
		UAM_ASSERT(fixture.service.AddNativeImportTombstones(fixture.app.data_root, {fixture.identity}, keys));
		UAM_ASSERT_EQ(keys.size(), std::size_t(1));
		const auto tombstone_file = fixture.app.data_root / "native-import-tombstones.json";
		const auto tombstone_bytes = uam_test::ReadFile(tombstone_file);
		const auto rejected = fixture.Drain(discovery);
		UAM_ASSERT_EQ(rejected.imported_count, 0);
		UAM_ASSERT(rejected.confirmed_empty_native_keys.empty());
		UAM_ASSERT(fixture.app.chats.empty());
		UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(fixture.app.data_root, fixture.identity.id)));
		UAM_ASSERT_EQ(uam_test::ReadFile(tombstone_file), tombstone_bytes);
		UAM_ASSERT_EQ(uam_test::ReadFile(fixture.source), fixture.source_bytes);
		fixture.AssertPayloadReleased();
		UAM_ASSERT(fixture.service.RemoveNativeImportTombstones(fixture.app.data_root, keys));
		// Read the same intact raw source through the real parser on a fresh attempt.
		fixture.context = std::make_shared<GeminiHistoryParseContext>();
		auto retry = fixture.Stage(body);
		fixture.AssertPublished(retry, body == BatchMediaBody::InlineOnly);
	}
}

#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))

namespace
{
	ChatSession ParseExistingDiscoveredGeminiSource(GeminiMediaBatchFixture& fixture)
	{
		fixture.context = std::make_shared<GeminiHistoryParseContext>();
		GeminiJsonHistoryStoreOptions options;
		options.execution_host_id = "local";
		options.context = fixture.context;
		auto parsed = GeminiJsonHistoryStore::ParseFileResult(fixture.source, ProviderProfileStore::DefaultGeminiProfile(), options);
		UAM_ASSERT_EQ(parsed.status, GeminiHistoryParseStatus::Content);
		UAM_ASSERT(parsed.chat && parsed.error.empty());
		ChatSession candidate = std::move(*parsed.chat);
		candidate.workspace_directory = fixture.app.folders.front().directory;
		candidate.execution_host_id = options.execution_host_id;
		UAM_ASSERT_EQ(fixture.service.StartupImportWorkspaceOwner(fixture.app, candidate), &fixture.app.folders.front());
		UAM_ASSERT_EQ(candidate.provider_id, std::string("gemini-cli"));
		UAM_ASSERT_EQ(candidate.native_session_id, fixture.identity.native_session_id);
		UAM_ASSERT(candidate.messages_loaded && candidate.messages.size() == 1);
		UAM_ASSERT_EQ(fixture.context->payload_budget->UsedCount(), std::size_t(1));
		UAM_ASSERT_EQ(candidate.messages.front().attachments.size(), std::size_t(1));
		UAM_ASSERT_EQ(candidate.messages.front().attachments.front().source, AttachmentSource::NativeInlinePending);
		return candidate;
	}

	ChatHistorySyncService::LocalHistoryDiscovery StageRegisteredGeminiUpdate(GeminiMediaBatchFixture& fixture, ChatSession candidate)
	{
		UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t(1));
		const auto existing = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.app.chats.front().id, true);
		UAM_ASSERT(existing && existing->messages.size() == 1);
		UAM_ASSERT_EQ(candidate.native_session_id, existing->native_session_id);
		UAM_ASSERT_EQ(candidate.provider_id, existing->provider_id);
		UAM_ASSERT_EQ(candidate.workspace_directory, existing->workspace_directory);
		UAM_ASSERT_EQ(candidate.messages.size(), existing->messages.size());
		UAM_ASSERT_EQ(candidate.updated_at, existing->updated_at);
		UAM_ASSERT_EQ(candidate.messages.front().role, existing->messages.front().role);
		UAM_ASSERT_EQ(candidate.messages.front().content, existing->messages.front().content);
		UAM_ASSERT_EQ(candidate.messages.front().created_at, existing->messages.front().created_at);
		const auto primary = AppPaths::UamChatFilePath(fixture.app.data_root, existing->id);
		const auto before = uam_test::ReadFile(primary);
		ChatHistorySyncService::LocalHistoryDiscovery discovery;
		discovery.chats.push_back(std::move(candidate));
		const auto admitted = fixture.service.ImportDiscoveredProviderChatsBatch(fixture.app, {}, discovery, 8, ChatHistorySyncService::ImportPolicy::RegisteredWorkspacesOnly);
		UAM_ASSERT(admitted.success && admitted.imported_count == 0);
		UAM_ASSERT(admitted.confirmed_empty_native_keys.empty());
		UAM_ASSERT(discovery.chats.empty() && discovery.preparation.valid());
		UAM_ASSERT_EQ(discovery.preparation.wait_for(std::chrono::seconds(10)), std::future_status::ready);
		{
			const auto prepared = discovery.preparation.get();
			UAM_ASSERT(prepared && prepared->chats.size() == 1);
			const auto& staged = prepared->chats.front();
			UAM_ASSERT(staged.expected_live && staged.expected_live->id == existing->id);
			UAM_ASSERT(!staged.confirmed_empty && staged.error.empty());
			// Do not require !skipped: this is the source finding to prove through
			// the public publication result, rather than changing prepared flags.
		}
		UAM_ASSERT_EQ(uam_test::ReadFile(primary), before);
		UAM_ASSERT_EQ(uam_test::ReadFile(fixture.source), fixture.source_bytes);
		fixture.AssertNoNewDeletionReceipt();
		return discovery;
	}

	ChatHistorySyncService::ImportResult DrainRegisteredGeminiUpdate(GeminiMediaBatchFixture& fixture, ChatHistorySyncService::LocalHistoryDiscovery& discovery)
	{
		ChatHistorySyncService::ImportResult result;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (discovery.Pending())
		{
			UAM_ASSERT(std::chrono::steady_clock::now() < deadline);
			result.Merge(fixture.service.ImportDiscoveredProviderChatsBatch(fixture.app, {}, discovery, 8, ChatHistorySyncService::ImportPolicy::RegisteredWorkspacesOnly));
			std::this_thread::yield();
		}
		UAM_ASSERT(result.success && result.confirmed_empty_native_keys.empty());
		UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t(1));
		UAM_ASSERT_EQ(uam_test::ReadFile(fixture.source), fixture.source_bytes);
		fixture.AssertNoNewDeletionReceipt();
		fixture.AssertPayloadReleased();
		return result;
	}
}

UAM_TEST(GeminiDiscoveredBatchPublishesEqualCountMediaOnlyUpdate)
{
	GeminiMediaBatchFixture fixture;
	auto initial = fixture.Stage(BatchMediaBody::Text);
	fixture.AssertPublished(initial, false);
	const auto previous = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.identity.id, true);
	UAM_ASSERT(previous && previous->messages.size() == 1 && previous->messages.front().attachments.empty());
	Json document = Json::parse(fixture.source_bytes);
	// The same session, message identity, authored text and timestamps gain only
	// one native media part. Do not add a row or advance recency to force import.
	document["messages"][0]["content"].push_back({{"inlineData", {{"mimeType", "image/png"}, {"data", kBatchMediaPng}}}});
	fixture.source_bytes = document.dump();
	UAM_ASSERT(uam::io::WriteTextFile(fixture.source, fixture.source_bytes));
	auto candidate = ParseExistingDiscoveredGeminiSource(fixture);
	MessageAttachment expected = candidate.messages.front().attachments.front();
	expected.native_inline_payload.reset();
	auto update = StageRegisteredGeminiUpdate(fixture, std::move(candidate));
	const auto result = DrainRegisteredGeminiUpdate(fixture, update);
	// Intended new RED: equal-count reconciliation currently omits attachments.
	UAM_ASSERT_EQ(result.imported_count, 1);
	const auto loaded = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.identity.id, true);
	UAM_ASSERT(loaded && loaded->messages.size() == 1 && loaded->messages.front().attachments.size() == 1);
	UAM_ASSERT_EQ(loaded->messages.front().content, previous->messages.front().content);
	UAM_ASSERT_EQ(loaded->messages.front().created_at, previous->messages.front().created_at);
	const auto& original = loaded->messages.front().attachments.front();
	UAM_ASSERT_EQ(original.source, AttachmentSource::OwnedNativeAsset);
	UAM_ASSERT_EQ(original.id, expected.id);
	UAM_ASSERT_EQ(original.native_part_key, expected.native_part_key);
	UAM_ASSERT_EQ(original.native_source_field, expected.native_source_field);
	UAM_ASSERT_EQ(original.native_tool_call_id, expected.native_tool_call_id);
	UAM_ASSERT_EQ(original.origin_execution_host_id, expected.origin_execution_host_id);
	UAM_ASSERT_EQ(original.content_digest, expected.content_digest);
	UAM_ASSERT_EQ(original.size_bytes, expected.size_bytes);
	UAM_ASSERT(original.path.empty() && !original.native_inline_payload);
	const auto read = uam::native_assets::ReadOwned(fixture.app.data_root, loaded->id, original.native_asset_key);
	std::string bytes;
	UAM_ASSERT(uam::base64::Decode(kBatchMediaPng, bytes));
	UAM_ASSERT(read.ok && read.error.empty());
	UAM_ASSERT_EQ(read.bytes, bytes);
}

UAM_TEST(GeminiDiscoveredBatchSkipsUnchangedPendingVersusOwnedOriginal)
{
	GeminiMediaBatchFixture fixture;
	auto initial = fixture.Stage(BatchMediaBody::InlineOnly);
	fixture.AssertPublished(initial, true);
	const auto primary = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.identity.id);
	const auto before = uam_test::ReadFile(primary);
	const auto previous = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.identity.id, true);
	UAM_ASSERT(previous && previous->messages.size() == 1 && previous->messages.front().attachments.size() == 1);
	const auto key = previous->messages.front().attachments.front().native_asset_key;
	UAM_ASSERT(!key.empty());
	// Read the same unmodified raw bytes with a fresh parser/budget. Pending and
	// Owned are different storage representations of this same native original.
	auto candidate = ParseExistingDiscoveredGeminiSource(fixture);
	auto unchanged = StageRegisteredGeminiUpdate(fixture, std::move(candidate));
	const auto result = DrainRegisteredGeminiUpdate(fixture, unchanged);
	UAM_ASSERT_EQ(result.imported_count, 0);
	UAM_ASSERT_EQ(uam_test::ReadFile(primary), before);
	const auto loaded = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.identity.id, true);
	UAM_ASSERT(loaded && loaded->messages.size() == 1 && loaded->messages.front().attachments.size() == 1);
	UAM_ASSERT_EQ(loaded->messages.front().attachments.front().native_asset_key, key);
	const auto read = uam::native_assets::ReadOwned(fixture.app.data_root, loaded->id, key);
	std::string bytes;
	UAM_ASSERT(uam::base64::Decode(kBatchMediaPng, bytes));
	UAM_ASSERT(read.ok && read.error.empty());
	UAM_ASSERT_EQ(read.bytes, bytes);
}

#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))

namespace
{
	ChatSession ParseCurrentDiscoveredGeminiUri(GeminiMediaBatchFixture& fixture)
	{
		fixture.context = std::make_shared<GeminiHistoryParseContext>();
		GeminiJsonHistoryStoreOptions options;
		options.execution_host_id = "local";
		options.context = fixture.context;
		auto parsed = GeminiJsonHistoryStore::ParseFileResult(fixture.source, ProviderProfileStore::DefaultGeminiProfile(), options);
		UAM_ASSERT_EQ(parsed.status, GeminiHistoryParseStatus::Content);
		UAM_ASSERT(parsed.chat && parsed.error.empty());
		ChatSession candidate = std::move(*parsed.chat);
		candidate.execution_host_id = options.execution_host_id;
		candidate.workspace_directory = fixture.app.folders.front().directory;
		UAM_ASSERT_EQ(fixture.service.StartupImportWorkspaceOwner(fixture.app, candidate), &fixture.app.folders.front());
		UAM_ASSERT_EQ(candidate.native_session_id, fixture.identity.native_session_id);
		UAM_ASSERT_EQ(candidate.provider_id, std::string("gemini-cli"));
		UAM_ASSERT(candidate.messages_loaded && candidate.messages.size() == 1);
		UAM_ASSERT_EQ(candidate.messages.front().attachments.size(), std::size_t(1));
		const auto& uri = candidate.messages.front().attachments.front();
		UAM_ASSERT_EQ(uri.source, AttachmentSource::ProviderUri);
		UAM_ASSERT(uri.path.empty() && uri.native_asset_key.empty() && !uri.native_inline_payload);
		UAM_ASSERT_EQ(fixture.context->payload_budget->UsedCount(), std::size_t(0));
		return candidate;
	}
}

UAM_TEST(GeminiDiscoveredBatchPublishesEqualCountOpaqueUriChangeWithoutFetch)
{
	GeminiMediaBatchFixture fixture;
	auto initial = fixture.Stage(BatchMediaBody::OpaqueUri);
	fixture.AssertPublished(initial, false);
	const auto primary = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.identity.id);
	const auto first_primary = uam_test::ReadFile(primary);
	// First establish that an unchanged opaque reference remains a real no-op.
	auto unchanged_candidate = ParseCurrentDiscoveredGeminiUri(fixture);
	auto unchanged = StageRegisteredGeminiUpdate(fixture, std::move(unchanged_candidate));
	const auto no_change = DrainRegisteredGeminiUpdate(fixture, unchanged);
	UAM_ASSERT_EQ(no_change.imported_count, 0);
	UAM_ASSERT_EQ(uam_test::ReadFile(primary), first_primary);
	const auto previous = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.identity.id, true);
	UAM_ASSERT(previous && previous->messages.size() == 1 && previous->messages.front().attachments.size() == 1);
	UAM_ASSERT_EQ(previous->messages.front().attachments.front().source_uri, kBatchMediaUri);
	const std::string next_uri = "https://different-assets.invalid/opaque-native-revision.pdf";
	Json document = Json::parse(fixture.source_bytes);
	// Keep the same session, native row, empty text, timestamps and row count.
	document["messages"][0]["content"][0]["fileData"]["fileUri"] = next_uri;
	fixture.source_bytes = document.dump();
	UAM_ASSERT(uam::io::WriteTextFile(fixture.source, fixture.source_bytes));
	auto candidate = ParseCurrentDiscoveredGeminiUri(fixture);
	UAM_ASSERT_EQ(candidate.messages.front().attachments.front().source_uri, next_uri);
	auto update = StageRegisteredGeminiUpdate(fixture, std::move(candidate));
	const auto result = DrainRegisteredGeminiUpdate(fixture, update);
	// Intended separate RED: exact opaque-reference changes are source changes.
	UAM_ASSERT_EQ(result.imported_count, 1);
	const auto loaded = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.identity.id, true);
	UAM_ASSERT(loaded && loaded->messages.size() == 1 && loaded->messages.front().attachments.size() == 1);
	const auto& uri = loaded->messages.front().attachments.front();
	UAM_ASSERT_EQ(uri.source, AttachmentSource::ProviderUri);
	UAM_ASSERT_EQ(uri.source_uri, next_uri);
	UAM_ASSERT_EQ(loaded->messages.front().content, previous->messages.front().content);
	UAM_ASSERT_EQ(loaded->messages.front().created_at, previous->messages.front().created_at);
	UAM_ASSERT(uri.path.empty() && uri.native_asset_key.empty() && uri.content_digest.empty() && !uri.native_inline_payload);
	UAM_ASSERT(!fs::exists(fixture.app.data_root / std::string(uam::native_assets::kStoreDirectory)));
}

#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))

namespace
{
	void AssertDiscoveredGeminiOriginalHealthRepair(bool corrupt)
	{
		GeminiMediaBatchFixture fixture;
		auto initial = fixture.Stage(BatchMediaBody::InlineOnly);
		fixture.AssertPublished(initial, true);
		const auto previous = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.identity.id, true);
		UAM_ASSERT(previous && previous->messages.size() == 1 && previous->messages.front().attachments.size() == 1);
		const auto& saved = previous->messages.front().attachments.front();
		UAM_ASSERT_EQ(saved.source, AttachmentSource::OwnedNativeAsset);
		const auto healthy = uam::native_assets::ReadOwned(fixture.app.data_root, previous->id, saved.native_asset_key);
		std::string expected;
		UAM_ASSERT(uam::base64::Decode(kBatchMediaPng, expected));
		UAM_ASSERT(healthy.ok && healthy.error.empty());
		UAM_ASSERT_EQ(healthy.bytes, expected);
		// Deliberate filesystem fault only inside this private fixture's store.
		// No production URI/key is interpreted as a path or given new authority.
		std::vector<fs::path> originals;
		for (const auto& entry : fs::recursive_directory_iterator(fixture.app.data_root / std::string(uam::native_assets::kStoreDirectory)))
			if (entry.is_regular_file() && entry.path().extension() == ".bin") originals.push_back(entry.path());
		UAM_ASSERT_EQ(originals.size(), std::size_t(1));
		UAM_ASSERT_EQ(uam_test::ReadFile(originals.front()), expected);
		const auto primary = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.identity.id);
		const auto primary_before = uam_test::ReadFile(primary);
		if (corrupt)
		{
			std::string damaged = expected;
			UAM_ASSERT(!damaged.empty());
			damaged.back() = static_cast<char>(damaged.back() ^ 1);
			UAM_ASSERT_EQ(damaged.size(), expected.size());
			UAM_ASSERT(uam::io::WriteTextFile(originals.front(), damaged));
			UAM_ASSERT_EQ(uam_test::ReadFile(originals.front()), damaged);
		}
		else UAM_ASSERT(fs::remove(originals.front()));
		const auto unavailable = uam::native_assets::ReadOwned(fixture.app.data_root, previous->id, saved.native_asset_key);
		UAM_ASSERT(!unavailable.ok && !unavailable.error.empty());
		UAM_ASSERT_EQ(uam_test::ReadFile(primary), primary_before);
		UAM_ASSERT_EQ(uam_test::ReadFile(fixture.source), fixture.source_bytes);
		// Fresh validated pending bytes come from the same native source, not the
		// missing/corrupt store; row count, identity, text and times stay unchanged.
		auto candidate = ParseExistingDiscoveredGeminiSource(fixture);
		auto refresh = StageRegisteredGeminiUpdate(fixture, std::move(candidate));
		const auto result = DrainRegisteredGeminiUpdate(fixture, refresh);
		const auto loaded = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.identity.id, true);
		UAM_ASSERT(loaded && loaded->messages.size() == 1 && loaded->messages.front().attachments.size() == 1);
		const auto& original = loaded->messages.front().attachments.front();
		const auto repaired = uam::native_assets::ReadOwned(fixture.app.data_root, loaded->id, original.native_asset_key);
		// Intended independent RED: metadata-only unchanged reconciliation leaves
		// the actual saved original unavailable despite admitted fresh bytes.
		UAM_ASSERT(repaired.ok && repaired.error.empty());
		UAM_ASSERT_EQ(repaired.bytes, expected);
		UAM_ASSERT_EQ(result.imported_count, 1);
		UAM_ASSERT_EQ(original.source, AttachmentSource::OwnedNativeAsset);
		UAM_ASSERT_EQ(original.id, saved.id);
		UAM_ASSERT_EQ(original.native_part_key, saved.native_part_key);
		UAM_ASSERT_EQ(original.native_source_field, saved.native_source_field);
		UAM_ASSERT_EQ(original.content_digest, saved.content_digest);
		UAM_ASSERT_EQ(original.mime_type, saved.mime_type);
		UAM_ASSERT_EQ(original.size_bytes, saved.size_bytes);
	}
}

UAM_TEST(GeminiDiscoveredBatchRepairsMissingOwnedOriginalFromUnchangedNativeSource)
{
	AssertDiscoveredGeminiOriginalHealthRepair(false);
}

UAM_TEST(GeminiDiscoveredBatchRepairsCorruptOwnedOriginalFromUnchangedNativeSource)
{
	AssertDiscoveredGeminiOriginalHealthRepair(true);
}

#endif
