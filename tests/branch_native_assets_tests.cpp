#include "test_harness.h"
#include "common/chat/native_attachment_assets.h"
#include "common/chat/native_media_chat_save.h"

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
namespace
{
	const std::string kBranchOriginalPng = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==";
	const std::string kBranchOpaqueUri = "https://assets.invalid/opaque-branch-reference.pdf";

	MessageAttachment BranchPendingOriginal()
	{
		MessageAttachment attachment;
		attachment.id = "retained-native-part";
		attachment.name = "Retained native original";
		attachment.kind = "image";
		attachment.source = AttachmentSource::NativeInlinePending;
		attachment.native_part_key = "v1:branch-source/assistant/inline-part";
		attachment.native_tool_call_id = "recorded-image-tool";
		attachment.native_source_field = "toolCalls/recorded-image-tool/result/0";
		attachment.origin_execution_host_id = "local";
		std::string error;
		attachment.native_inline_payload = uam::native_assets::MakeInlinePayload(
		    std::string(kBranchOriginalPng), "image/png", std::make_shared<uam::native_assets::PayloadBudget>(), error);
		UAM_ASSERT(attachment.native_inline_payload && error.empty());
		attachment.mime_type = attachment.native_inline_payload->MimeType();
		attachment.content_digest = attachment.native_inline_payload->Digest();
		attachment.size_bytes = attachment.native_inline_payload->DecodedSize();
		return attachment;
	}

	MessageAttachment BranchOpaqueReference()
	{
		MessageAttachment attachment;
		attachment.id = "retained-opaque-part";
		attachment.name = "Opaque provider reference";
		attachment.kind = "file";
		attachment.mime_type = "application/pdf";
		attachment.source = AttachmentSource::ProviderUri;
		attachment.source_uri = kBranchOpaqueUri;
		attachment.native_part_key = "v1:branch-source/assistant/file-part";
		attachment.native_source_field = "content/1/fileData";
		attachment.origin_execution_host_id = "local";
		return attachment;
	}

	struct BranchNativeOriginalFixture
	{
		uam_test::TempDir temp{"uam-native-original-branch"};
		uam::AppState app;
		std::string source_id;
		std::string source_key;
		std::string expected_original;
		std::vector<Message> source_messages;

		explicit BranchNativeOriginalFixture(bool owned_original = true)
		{
			app.data_root = temp.root;
			app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
			app.settings.git_enabled = false;
			app.settings.git_auto_worktree = false;
			const auto workspace = temp.root / "workspace";
			fs::create_directories(workspace);
			ChatSession source = ChatDomainService().CreateNewChat("", uam::provider_ids::kGeminiCli, false);
			source_id = source.id;
			source.title = "Private original branch source";
			source.execution_host_id = "local";
			source.workspace_directory = workspace.string();
			source.native_session_id = "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee";
			source.messages = {{MessageRole::User, "Inspect the recorded original."},
			    {MessageRole::Assistant, "Preserve this answer and its source attachment."},
			    {MessageRole::User, "Continue from this chosen point."}};
			for (std::size_t index = 0; index < source.messages.size(); ++index)
			{
				source.messages[index].provider = uam::provider_ids::kGeminiCli;
				source.messages[index].created_at = "2026-10-17T12:00:0" + std::to_string(index + 1) + "Z";
			}
			UAM_ASSERT(source.messages_loaded && !source.imported_read_only);
			UAM_ASSERT(source.workspace_isolation_kind.empty());
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, source));
			const auto primary = AppPaths::UamChatFilePath(app.data_root, source_id);
			const std::string before = uam_test::ReadFile(primary);
			UAM_ASSERT(!before.empty());
			if (owned_original)
			{
				UAM_ASSERT(uam::base64::Decode(kBranchOriginalPng, expected_original));
				UAM_ASSERT(!expected_original.empty());
				source.messages[1].attachments.push_back(BranchPendingOriginal());
				const auto revision = ChatRepository::ChatStorageRevision(app.data_root, source_id);
				UAM_ASSERT(revision);
				std::string error;
				auto prepared = uam::PreparedNativeMediaChatSave::Prepare(app.data_root, source, *revision, error);
				UAM_ASSERT(prepared && error.empty());
				UAM_ASSERT_EQ(uam_test::ReadFile(primary), before);
				UAM_ASSERT(!prepared->PrimaryAcknowledged());
				UAM_ASSERT(prepared->Publish(error, [] { return true; }));
				UAM_ASSERT(error.empty() && prepared->PrimaryAcknowledged());
				UAM_ASSERT(prepared->AcknowledgeOriginals(error) && error.empty());
				auto committed = prepared->TakeCommittedChat();
				UAM_ASSERT(committed);
				source = std::move(*committed);
				const auto& attachment = source.messages[1].attachments.front();
				UAM_ASSERT_EQ(attachment.source, AttachmentSource::OwnedNativeAsset);
				UAM_ASSERT(!attachment.native_inline_payload && attachment.path.empty());
				source_key = attachment.native_asset_key;
				UAM_ASSERT(uam::native_assets::IsKeyForOwner(source_id, source_key));
				UAM_ASSERT(!uam::native_assets::IsKeyForOwner(source.native_session_id, source_key));
			}
			else
			{
				source.messages[1].attachments.push_back(BranchOpaqueReference());
				UAM_ASSERT(ChatRepository::SaveChat(app.data_root, source));
			}
			source_messages = source.messages;
			app.chats.push_back(std::move(source));
			ChatDomainService().SelectChatById(app, source_id);
			AssertSource(owned_original);
			AssertNoRuntime();
		}

		void AssertNoRuntime()
		{
			UAM_ASSERT(app.acp_sessions.empty());
			// Successful CLI-view branching records a launch intent; without Poll it
			// must remain an inert terminal, never a provider process or input write.
			for (const auto& terminal : app.cli_terminals)
			{
				UAM_ASSERT(terminal && !terminal->running && !terminal->input_writer);
				UAM_ASSERT(!terminal->uam_control_session && !terminal->uam_control_relay);
				UAM_ASSERT(!terminal->context_preparation && !terminal->native_session_setup_cancel);
				UAM_ASSERT(!uam::platform::CliTerminalHasWritableInput(*terminal));
#if defined(_WIN32)
				UAM_ASSERT(terminal->process_info.dwProcessId == 0 && terminal->pseudo_console == nullptr);
#else
				UAM_ASSERT(terminal->child_pid == -1 && terminal->watchdog_pid == -1);
#endif
			}
			UAM_ASSERT(app.worktree_operation_chat_ids.empty());
		}

		void AssertSource(bool owned_original = true)
		{
			const auto* current = ChatDomainService().FindChatById(app, source_id);
			UAM_ASSERT(current && current->id == source_id && current->messages_loaded);
			UAM_ASSERT_EQ(current->messages, source_messages);
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, source_id);
			UAM_ASSERT(saved && saved->id == source_id && !saved->imported_read_only);
			UAM_ASSERT_EQ(saved->messages, source_messages);
			UAM_ASSERT_EQ(saved->provider_id, current->provider_id);
			UAM_ASSERT_EQ(saved->messages.size(), std::size_t{3});
			UAM_ASSERT_EQ(saved->messages[1].attachments.size(), std::size_t{1});
			const auto& attachment = saved->messages[1].attachments.front();
			if (owned_original)
			{
				UAM_ASSERT_EQ(attachment.source, AttachmentSource::OwnedNativeAsset);
				UAM_ASSERT_EQ(attachment.native_asset_key, source_key);
				const auto original = uam::native_assets::ReadOwned(app.data_root, source_id, source_key);
				UAM_ASSERT(original.ok && original.error.empty());
				UAM_ASSERT_EQ(original.bytes, expected_original);
			}
			else
			{
				UAM_ASSERT_EQ(attachment.source, AttachmentSource::ProviderUri);
				UAM_ASSERT_EQ(attachment.source_uri, kBranchOpaqueUri);
				UAM_ASSERT(attachment.native_asset_key.empty() && attachment.path.empty());
			}
		}

		void SwitchTo(std::string_view provider)
		{
			// Gemini also crosses a genuine public provider switch instead of only
			// accepting the same-provider Unchanged shortcut.
			if (provider == uam::provider_ids::kGeminiCli)
			{
				UAM_ASSERT_EQ(uam::SwitchChatProvider(app, source_id, uam::provider_ids::kOpenCodeCli),
				    uam::ChatProviderSwitchResult::Changed);
				AssertSource();
			}
			UAM_ASSERT_EQ(uam::SwitchChatProvider(app, source_id, provider), uam::ChatProviderSwitchResult::Changed);
			const auto* current = ChatDomainService().FindChatById(app, source_id);
			UAM_ASSERT(current && current->provider_id == provider && current->id == source_id);
			UAM_ASSERT(current->native_session_id.empty() && !current->imported_read_only);
			UAM_ASSERT(current->workspace_isolation_kind.empty());
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, source_id);
			UAM_ASSERT(saved && saved->provider_id == provider && saved->native_session_id.empty());
			AssertSource();
			AssertNoRuntime();
		}

		void BranchRetainsOwnedOriginal(std::string_view provider)
		{
			SwitchTo(provider);
			const auto primary = AppPaths::UamChatFilePath(app.data_root, source_id);
			const auto source_primary = uam_test::ReadFile(primary);
			const auto source_revision = ChatRepository::ChatStorageRevision(app.data_root, source_id);
			UAM_ASSERT(!source_primary.empty() && source_revision);
			// Retention only: do not use BranchFromMessageAndRetry or any Send/Poll.
			const bool created = ChatDomainService().CreateBranchFromMessage(app, source_id, 2);
			UAM_ASSERT_EQ(uam_test::ReadFile(primary), source_primary);
			UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(app.data_root, source_id), source_revision);
			AssertSource();
			AssertNoRuntime();
			UAM_ASSERT(created);
			const auto* selected = ChatDomainService().SelectedChat(app);
			UAM_ASSERT(selected && selected->id != source_id);
			const std::string branch_id = selected->id;
			UAM_ASSERT_EQ(selected->provider_id, std::string(provider));
			UAM_ASSERT_EQ(selected->parent_chat_id, source_id);
			UAM_ASSERT_EQ(selected->branch_from_message_index, 2);
			UAM_ASSERT(selected->native_session_id.empty());
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, branch_id);
			UAM_ASSERT(saved && saved->id == branch_id && saved->provider_id == provider);
			UAM_ASSERT_EQ(saved->parent_chat_id, source_id);
			UAM_ASSERT_EQ(saved->branch_from_message_index, 2);
			UAM_ASSERT_EQ(saved->messages.size(), std::size_t{3});
			UAM_ASSERT_EQ(saved->messages[1].attachments.size(), std::size_t{1});
			for (std::size_t index = 0; index < saved->messages.size(); ++index)
			{
				UAM_ASSERT_EQ(saved->messages[index].content, source_messages[index].content);
				UAM_ASSERT_EQ(saved->messages[index].created_at, source_messages[index].created_at);
			}
			const auto& attachment = saved->messages[1].attachments.front();
			UAM_ASSERT_EQ(attachment.source, AttachmentSource::OwnedNativeAsset);
			UAM_ASSERT(uam::native_assets::IsKeyForOwner(branch_id, attachment.native_asset_key));
			UAM_ASSERT(!uam::native_assets::IsKeyForOwner(source_id, attachment.native_asset_key));
			auto expected_metadata = source_messages[1].attachments.front();
			expected_metadata.native_asset_key = attachment.native_asset_key;
			UAM_ASSERT_EQ(attachment, expected_metadata);
			const auto original = uam::native_assets::ReadOwned(app.data_root, branch_id, attachment.native_asset_key);
			UAM_ASSERT(original.ok && original.error.empty());
			UAM_ASSERT_EQ(original.bytes, expected_original);
			UAM_ASSERT(!uam::native_assets::ReadOwned(app.data_root, source_id, attachment.native_asset_key).ok);
			UAM_ASSERT_EQ(uam_test::ReadFile(primary), source_primary);
			AssertSource();
			AssertNoRuntime();
		}
	};
}

#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
UAM_TEST(GeminiBranchRetainsNativeOwnedOriginalUnderNewOwner)
{
	BranchNativeOriginalFixture fixture;
	fixture.BranchRetainsOwnedOriginal(uam::provider_ids::kGeminiCli);
}
#endif

#if UAM_ENABLE_RUNTIME_CODEX_CLI
UAM_TEST(CodexBranchRetainsNativeOwnedOriginalUnderNewOwner)
{
	BranchNativeOriginalFixture fixture;
	fixture.BranchRetainsOwnedOriginal(uam::provider_ids::kCodexCli);
}
#endif

#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
UAM_TEST(ClaudeBranchRetainsNativeOwnedOriginalUnderNewOwner)
{
	BranchNativeOriginalFixture fixture;
	fixture.BranchRetainsOwnedOriginal(uam::provider_ids::kClaudeCli);
}
#endif

#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
UAM_TEST(OpenCodeBranchRetainsNativeOwnedOriginalUnderNewOwner)
{
	BranchNativeOriginalFixture fixture;
	fixture.BranchRetainsOwnedOriginal(uam::provider_ids::kOpenCodeCli);
}
#endif

#if UAM_ENABLE_RUNTIME_COPILOT_CLI
UAM_TEST(CopilotBranchRetainsNativeOwnedOriginalUnderNewOwner)
{
	BranchNativeOriginalFixture fixture;
	fixture.BranchRetainsOwnedOriginal(uam::provider_ids::kCopilotCli);
}
#endif

#if UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI
UAM_TEST(AntigravityBranchRetainsNativeOwnedOriginalUnderNewOwner)
{
	BranchNativeOriginalFixture fixture;
	fixture.BranchRetainsOwnedOriginal(uam::provider_ids::kAntigravityCli);
}
#endif

UAM_TEST(BranchRetainsTextAndOpaqueProviderUriWithoutOriginalMaterialization)
{
	BranchNativeOriginalFixture fixture(false);
	const auto primary = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.source_id);
	const auto before = uam_test::ReadFile(primary);
	UAM_ASSERT(ChatDomainService().CreateBranchFromMessage(fixture.app, fixture.source_id, 2));
	fixture.AssertSource(false);
	fixture.AssertNoRuntime();
	UAM_ASSERT_EQ(uam_test::ReadFile(primary), before);
	const auto* selected = ChatDomainService().SelectedChat(fixture.app);
	UAM_ASSERT(selected && selected->id != fixture.source_id && selected->native_session_id.empty());
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, selected->id);
	UAM_ASSERT(saved && saved->parent_chat_id == fixture.source_id && saved->branch_from_message_index == 2);
	UAM_ASSERT_EQ(saved->messages, fixture.source_messages);
	UAM_ASSERT_EQ(saved->messages[1].attachments.front().source_uri, kBranchOpaqueUri);
	UAM_ASSERT(!fs::exists(fixture.app.data_root / std::string(uam::native_assets::kStoreDirectory)));
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
UAM_TEST(BranchRetainsSharedOriginalBytesWithDistinctMimeTypes)
{
	// Start without owned originals so both new MIME declarations are published
	// in one real generation, rather than reusing a previous image-only key.
	BranchNativeOriginalFixture fixture(false);
	ChatSession source = *ChatDomainService().FindChatById(fixture.app, fixture.source_id);
	MessageAttachment image = BranchPendingOriginal();
	MessageAttachment binary = image;
	binary.id = "retained-native-binary-part";
	binary.name = "Same original with recorded binary MIME";
	binary.kind = "file";
	binary.mime_type = "application/octet-stream";
	binary.native_part_key = "v1:branch-source/assistant/binary-part";
	binary.native_source_field = "toolCalls/recorded-image-tool/result/1";
	std::string error;
	binary.native_inline_payload = uam::native_assets::MakeInlinePayload(
	    std::string(kBranchOriginalPng), binary.mime_type,
	    std::make_shared<uam::native_assets::PayloadBudget>(), error);
	UAM_ASSERT(binary.native_inline_payload && error.empty());
	binary.content_digest = binary.native_inline_payload->Digest();
	binary.size_bytes = binary.native_inline_payload->DecodedSize();
	UAM_ASSERT_EQ(image.content_digest, binary.content_digest);
	UAM_ASSERT_EQ(image.size_bytes, binary.size_bytes);
	UAM_ASSERT(image.mime_type != binary.mime_type);
	source.messages[1].attachments = {std::move(image), std::move(binary)};
	const auto revision = ChatRepository::ChatStorageRevision(fixture.app.data_root, source.id);
	UAM_ASSERT(revision);
	auto prepared = uam::PreparedNativeMediaChatSave::Prepare(fixture.app.data_root, source, *revision, error);
	UAM_ASSERT(prepared && error.empty() && !prepared->PrimaryAcknowledged());
	UAM_ASSERT(prepared->Publish(error, [] { return true; }));
	UAM_ASSERT(error.empty() && prepared->PrimaryAcknowledged());
	UAM_ASSERT(prepared->AcknowledgeOriginals(error) && error.empty());
	auto committed = prepared->TakeCommittedChat();
	UAM_ASSERT(committed);
	const int source_index = ChatDomainService().FindChatIndexById(fixture.app, fixture.source_id);
	UAM_ASSERT(source_index >= 0);
	fixture.app.chats[static_cast<std::size_t>(source_index)] = std::move(*committed);
	const auto saved_source = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.source_id, true);
	UAM_ASSERT(saved_source && saved_source->messages.size() == 3);
	const auto& source_parts = saved_source->messages[1].attachments;
	UAM_ASSERT_EQ(source_parts.size(), std::size_t{2});
	UAM_ASSERT_EQ(source_parts[0].mime_type, std::string("image/png"));
	UAM_ASSERT_EQ(source_parts[1].mime_type, std::string("application/octet-stream"));
	// Actual storage permits MIME to vary per part: its original key identifies
	// owner/generation/digest/size, and both declarations must be valid/readable.
	UAM_ASSERT(!source_parts[0].native_asset_key.empty());
	UAM_ASSERT_EQ(source_parts[0].native_asset_key, source_parts[1].native_asset_key);
	std::string expected;
	UAM_ASSERT(uam::base64::Decode(kBranchOriginalPng, expected));
	for (const auto& part : source_parts)
	{
		UAM_ASSERT_EQ(part.source, AttachmentSource::OwnedNativeAsset);
		UAM_ASSERT(!part.native_inline_payload && part.path.empty());
		UAM_ASSERT(uam::native_assets::IsReferenceValid(
		    {part.native_asset_key, part.mime_type, part.content_digest, static_cast<std::size_t>(part.size_bytes)}, fixture.source_id));
		const auto read = uam::native_assets::ReadOwned(fixture.app.data_root, fixture.source_id, part.native_asset_key);
		UAM_ASSERT(read.ok && read.error.empty());
		UAM_ASSERT_EQ(read.bytes, expected);
	}
	const auto primary = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.source_id);
	const auto before = uam_test::ReadFile(primary);
	const auto before_revision = ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.source_id);
	UAM_ASSERT(before_revision && !before.empty());
	fixture.AssertNoRuntime();
	// Public retention only: no lifecycle retry, Poll, Submit or provider process.
	const bool created = ChatDomainService().CreateBranchFromMessage(fixture.app, fixture.source_id, 2);
	UAM_ASSERT_EQ(uam_test::ReadFile(primary), before);
	UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.source_id), before_revision);
	fixture.AssertNoRuntime();
	UAM_ASSERT(created);
	const auto* selected = ChatDomainService().SelectedChat(fixture.app);
	UAM_ASSERT(selected && selected->id != fixture.source_id && selected->native_session_id.empty());
	const auto branch = ChatRepository::LoadLocalChat(fixture.app.data_root, selected->id, true);
	UAM_ASSERT(branch && branch->parent_chat_id == fixture.source_id && branch->branch_from_message_index == 2);
	UAM_ASSERT_EQ(branch->messages.size(), saved_source->messages.size());
	const auto& branch_parts = branch->messages[1].attachments;
	UAM_ASSERT_EQ(branch_parts.size(), source_parts.size());
	UAM_ASSERT_EQ(branch_parts[0].native_asset_key, branch_parts[1].native_asset_key);
	for (std::size_t index = 0; index < source_parts.size(); ++index)
	{
		const auto& part = branch_parts[index];
		auto expected_metadata = source_parts[index];
		expected_metadata.native_asset_key = part.native_asset_key;
		UAM_ASSERT_EQ(part, expected_metadata);
		UAM_ASSERT(uam::native_assets::IsKeyForOwner(branch->id, part.native_asset_key));
		UAM_ASSERT(!uam::native_assets::IsKeyForOwner(fixture.source_id, part.native_asset_key));
		const auto read = uam::native_assets::ReadOwned(fixture.app.data_root, branch->id, part.native_asset_key);
		UAM_ASSERT(read.ok && read.error.empty());
		UAM_ASSERT_EQ(read.bytes, expected);
		const auto source_read = uam::native_assets::ReadOwned(fixture.app.data_root, fixture.source_id, source_parts[index].native_asset_key);
		UAM_ASSERT(source_read.ok && source_read.error.empty());
		UAM_ASSERT_EQ(source_read.bytes, expected);
	}
	const auto source_after = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.source_id, true);
	UAM_ASSERT(source_after);
	UAM_ASSERT_EQ(source_after->messages, saved_source->messages);
	UAM_ASSERT_EQ(ChatDomainService().FindChatById(fixture.app, fixture.source_id)->messages, saved_source->messages);
	UAM_ASSERT_EQ(uam_test::ReadFile(primary), before);
	fixture.AssertNoRuntime();
}
#endif

#include "common/chat/branch_native_assets.h"
#include <set>

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
UAM_TEST(BranchOriginalRejectedHistorySaveNeverAcknowledges)
{
	BranchNativeOriginalFixture fixture;
	const auto source_primary = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.source_id);
	const auto before = uam_test::ReadFile(source_primary);
	const auto revision = ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.source_id);
	const auto selected = ChatDomainService().SelectedChatId(fixture.app);
	const auto store = fixture.app.data_root / std::string(uam::native_assets::kStoreDirectory);
	const auto generations = [&store]()
	{
		std::set<fs::path> result;
		for (const auto& owner : fs::directory_iterator(store))
			if (owner.is_directory())
				for (const auto& generation : fs::directory_iterator(owner.path()))
					if (generation.is_directory())
						result.insert(generation.path());
		return result;
	};
	const auto old_generations = generations();
	const auto final_owner = ChatDomainService().CreateNewChat("", uam::provider_ids::kGeminiCli, false).id;
	UAM_ASSERT(final_owner != fixture.source_id);
	std::string error;
	auto originals = uam::PreparedBranchOriginals::Prepare(fixture.app.data_root, *ChatDomainService().FindChatById(fixture.app, fixture.source_id), final_owner, 2, std::nullopt, error);
	UAM_ASSERT(originals && error.empty() && !originals->PrimaryAcknowledged());
	UAM_ASSERT(originals->MatchesRequest(fixture.app.data_root, fixture.source_id, 2, std::nullopt));
	UAM_ASSERT(originals->MatchesSource(*ChatDomainService().FindChatById(fixture.app, fixture.source_id)));
	std::vector<fs::path> added;
	for (const auto& generation : generations())
		if (!old_generations.contains(generation))
			added.push_back(generation);
	UAM_ASSERT_EQ(added.size(), std::size_t{1});
	const auto generation = added.front();
	UAM_ASSERT(fs::is_regular_file(generation / "attempted.receipt"));
	UAM_ASSERT(!fs::exists(generation / "published.receipt"));
	const auto manifest = uam_test::ReadFile(generation / "prepared.receipt");
	const auto key_begin = manifest.find("\nna1:");
	UAM_ASSERT(key_begin != std::string::npos);
	const auto key_end = manifest.find('\n', key_begin + 1);
	UAM_ASSERT(key_end != std::string::npos);
	const auto key = manifest.substr(key_begin + 1, key_end - key_begin - 1);
	UAM_ASSERT(uam::native_assets::IsKeyForOwner(final_owner, key));
	const auto staged = uam::native_assets::ReadOwned(fixture.app.data_root, final_owner, key);
	UAM_ASSERT(staged.ok && staged.error.empty());
	UAM_ASSERT_EQ(staged.bytes, fixture.expected_original);
	// Leave final-owner admission valid, but make the real SaveHistory queue
	// witness unavailable. This is a storage rejection, not an injected boolean.
	const auto obstruction = fixture.app.data_root / "queue-outboxes" / (final_owner + ".json");
	fs::create_directories(obstruction);
	UAM_ASSERT(uam::io::WriteTextFile(obstruction / "keep", "private invalid queue journal"));
	UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, final_owner), std::optional<std::string>("missing"));
	UAM_ASSERT(!ChatDomainService().CreateBranchFromMessage(fixture.app, fixture.source_id, 2, std::nullopt, std::string("branch-original-save-rejected"), nullptr, std::nullopt, nullptr, nullptr, originals.get()));
	UAM_ASSERT_EQ(fixture.app.status_line, std::string("Failed to save branch chat."));
	// ApplyTo consumed the admitted token before ProviderRuntime::SaveHistory
	// rejected it; this must not be mistaken for a preflight-only owner refusal.
	UAM_ASSERT(!originals->MatchesRequest(fixture.app.data_root, fixture.source_id, 2, std::nullopt));
	UAM_ASSERT(!originals->PrimaryAcknowledged());
	UAM_ASSERT(!originals->AcknowledgeOriginals(error) && !error.empty());
	UAM_ASSERT(!originals->PrimaryAcknowledged());
	UAM_ASSERT_EQ(fixture.app.chats.size(), std::size_t{1});
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), selected);
	UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, final_owner), std::optional<std::string>("missing"));
	UAM_ASSERT(!fs::exists(AppPaths::UamChatSummaryFilePath(fixture.app.data_root, final_owner)));
	UAM_ASSERT_EQ(uam_test::ReadFile(source_primary), before);
	UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.source_id), revision);
	fixture.AssertSource();
	fixture.AssertNoRuntime();
	originals.reset();
	UAM_ASSERT(fs::is_regular_file(generation / "attempted.receipt"));
	UAM_ASSERT(!fs::exists(generation / "published.receipt"));
	UAM_ASSERT_EQ(uam_test::ReadFile(generation / "prepared.receipt"), manifest);
	const auto retained = uam::native_assets::ReadOwned(fixture.app.data_root, final_owner, key);
	UAM_ASSERT(retained.ok && retained.error.empty());
	UAM_ASSERT_EQ(retained.bytes, fixture.expected_original);
	UAM_ASSERT_EQ(uam_test::ReadFile(obstruction / "keep"), std::string("private invalid queue journal"));
	fixture.AssertSource();
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
namespace
{
	struct BranchSkillSnapshotFixture : BranchNativeOriginalFixture
	{
		std::vector<std::string> files, blocks;
		std::string prefix_file, prefix_block = "CAPTURED PREFIX\n";
		std::vector<std::string> disk_bytes = {"CHANGED DISK A", "CHANGED DISK B", "CHANGED DISK C"};

		BranchSkillSnapshotFixture()
		{
			prefix_file = (temp.root / "prefix-skill.md").string();
			UAM_ASSERT(uam::io::WriteTextFile(fs::path(prefix_file), "CHANGED PREFIX DISK"));
			blocks = {"  CAPTURED A\nsecond line\n", "CAPTURED B\n  spaces  ", "CAPTURED C\n"};
			for (std::size_t index = 0; index < blocks.size(); ++index)
			{
				files.push_back((temp.root / ("skill-" + std::to_string(index) + ".md")).string());
				UAM_ASSERT(uam::io::WriteTextFile(fs::path(files.back()), disk_bytes[index]));
				UAM_ASSERT(blocks[index] != disk_bytes[index]);
			}
			auto* source = ChatDomainService().FindChatById(app, source_id);
			UAM_ASSERT(source && source->messages_loaded && source->messages.size() == 3);
			source->messages[0].markdown_store_files = {prefix_file};
			source->messages[0].markdown_store_prompt_blocks = {prefix_block};
			source->messages[2].markdown_store_files = files;
			source->messages[2].markdown_store_prompt_blocks = blocks;
			// Both a prefix original and a selected-row original exercise native retention independently of skills.
			source->messages[2].attachments = source->messages[1].attachments;
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *source));
			source_messages = source->messages;
			AssertSource();
			AssertNoRuntime();
		}

		std::set<fs::path> Generations() const
		{
			std::set<fs::path> result;
			const auto store = app.data_root / std::string(uam::native_assets::kStoreDirectory);
			for (const auto& owner : fs::directory_iterator(store))
				if (owner.is_directory())
					for (const auto& generation : fs::directory_iterator(owner.path()))
						if (generation.is_directory())
							result.insert(generation.path());
			return result;
		}

		bool Create(const std::optional<std::vector<std::string>>& skills, const std::optional<std::vector<std::string>>& native = std::nullopt, const std::string& text = "Edited input with captured resources.", const std::string& operation = "skill-operation", bool* reused = nullptr, const ChatSession* prepared_source = nullptr, uam::PreparedBranchOriginals* originals = nullptr)
		{
			return ChatDomainService().CreateBranchFromMessage(app, source_id, 2, text, operation, reused, native, prepared_source, nullptr, originals, skills);
		}

		void Refuses(const std::optional<std::vector<std::string>>& skills, const std::optional<std::vector<std::string>>& native = std::nullopt, const std::string& text = "Edited input with captured resources.", const std::string& operation = "skill-operation", const ChatSession* prepared_source = nullptr, uam::PreparedBranchOriginals* originals = nullptr)
		{
			const auto source_before = ChatDomainService().FindChatById(app, source_id)->messages;
			const auto primary = AppPaths::UamChatFilePath(app.data_root, source_id);
			const auto bytes = uam_test::ReadFile(primary);
			const auto revision = ChatRepository::ChatStorageRevision(app.data_root, source_id);
			const auto selected = ChatDomainService().SelectedChatId(app);
			const auto generations = Generations();
			const auto count = app.chats.size();
			UAM_ASSERT(!Create(skills, native, text, operation, nullptr, prepared_source, originals));
			UAM_ASSERT(!app.status_line.empty());
			UAM_ASSERT_EQ(app.chats.size(), count);
			UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), selected);
			UAM_ASSERT_EQ(ChatDomainService().FindChatById(app, source_id)->messages, source_before);
			UAM_ASSERT_EQ(uam_test::ReadFile(primary), bytes);
			UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(app.data_root, source_id), revision);
			UAM_ASSERT_EQ(Generations(), generations);
			AssertNoRuntime();
		}

		std::string AssertBranch(const std::vector<std::string>& expected_files, const std::vector<std::string>& expected_blocks, bool selected_native = true, const std::string& text = "Edited input with captured resources.")
		{
			const auto* selected = ChatDomainService().SelectedChat(app);
			UAM_ASSERT(selected && selected->id != source_id);
			const auto branch = ChatRepository::LoadLocalChat(app.data_root, selected->id);
			UAM_ASSERT(branch && branch->messages.size() == 3 && branch->parent_chat_id == source_id && branch->branch_from_message_index == 2);
			UAM_ASSERT(branch->branch_message_edited && branch->native_session_id.empty());
			UAM_ASSERT_EQ(branch->messages[2].content, text);
			UAM_ASSERT_EQ(branch->messages[2].markdown_store_files, expected_files);
			UAM_ASSERT_EQ(branch->messages[2].markdown_store_prompt_blocks, expected_blocks);
			for (std::size_t row = 0; row < branch->messages.size(); ++row)
			{
				const auto& message = branch->messages[row];
				UAM_ASSERT_EQ(message.created_at, source_messages[row].created_at);
				if (row != 2)
				{
					UAM_ASSERT_EQ(message.content, source_messages[row].content);
					UAM_ASSERT_EQ(message.markdown_store_files, source_messages[row].markdown_store_files);
					UAM_ASSERT_EQ(message.markdown_store_prompt_blocks, source_messages[row].markdown_store_prompt_blocks);
				}
				const auto expected_count = row == 2 && !selected_native ? std::size_t{0} : source_messages[row].attachments.size();
				UAM_ASSERT_EQ(message.attachments.size(), expected_count);
				for (std::size_t part = 0; part < message.attachments.size(); ++part)
				{
					const auto& attachment = message.attachments[part];
					UAM_ASSERT(uam::native_assets::IsKeyForOwner(branch->id, attachment.native_asset_key));
					UAM_ASSERT(!uam::native_assets::IsKeyForOwner(source_id, attachment.native_asset_key));
					auto expected = source_messages[row].attachments[part];
					expected.native_asset_key = attachment.native_asset_key;
					UAM_ASSERT_EQ(attachment, expected);
					const auto original = uam::native_assets::ReadOwned(app.data_root, branch->id, attachment.native_asset_key);
					UAM_ASSERT(original.ok && original.error.empty());
					UAM_ASSERT_EQ(original.bytes, expected_original);
				}
			}
			UAM_ASSERT_EQ(uam_test::ReadFile(fs::path(prefix_file)), std::string("CHANGED PREFIX DISK"));
			for (std::size_t index = 0; index < files.size(); ++index)
				UAM_ASSERT_EQ(uam_test::ReadFile(fs::path(files[index])), disk_bytes[index]);
			AssertSource();
			AssertNoRuntime();
			return branch->id;
		}
	};
} // namespace

UAM_TEST(BranchDeliveredSkillSelectionUsesOriginalOrderAndCapturedSnapshots)
{
	BranchSkillSnapshotFixture fixture;
	const auto primary = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.source_id);
	const auto bytes = uam_test::ReadFile(primary);
	const auto revision = ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.source_id);
	const std::vector<std::string> selection = {fixture.files[2], fixture.files[0]};
	const std::vector<std::string> native = {fixture.source_messages[2].attachments.front().id};
	UAM_ASSERT(fixture.Create(selection, native));
	fixture.AssertBranch({fixture.files[0], fixture.files[2]}, {fixture.blocks[0], fixture.blocks[2]});
	UAM_ASSERT_EQ(uam_test::ReadFile(primary), bytes);
	UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.source_id), revision);
}

UAM_TEST(BranchDeliveredSkillEmptyAndOmittedSelectionsPreserveIndependentResources)
{
	BranchSkillSnapshotFixture fixture;
	UAM_ASSERT(fixture.Create(std::vector<std::string>{}));
	fixture.AssertBranch({}, {});
	UAM_ASSERT(fixture.Create(std::nullopt, std::nullopt, "Edited input with captured resources.", "legacy-omitted-operation"));
	fixture.AssertBranch(fixture.files, fixture.blocks);
}

UAM_TEST(BranchDeliveredSkillOnlyBlankInputRequiresExplicitRetainedContext)
{
	BranchSkillSnapshotFixture fixture;
	const std::vector<std::string> none;
	fixture.Refuses(none, none, " \n ");
	fixture.Refuses(std::nullopt, none, " \n ");
	UAM_ASSERT(fixture.Create(std::vector<std::string>{fixture.files[1]}, none, " \n "));
	fixture.AssertBranch({fixture.files[1]}, {fixture.blocks[1]}, false, " \n ");
}

UAM_TEST(BranchDeliveredSkillInvalidSelectionsRefuseBeforeOriginalPreparation)
{
	BranchSkillSnapshotFixture fixture;
	fixture.Refuses(std::vector<std::string>{fixture.files[0], fixture.files[0]});
	fixture.Refuses(std::vector<std::string>{"foreign-skill.md"});
	fixture.Refuses(std::vector<std::string>{""});
	fixture.Refuses(std::vector<std::string>(129, fixture.files[0]));
	fixture.Refuses(std::vector<std::string>{std::string(16385, 'x')});
	auto* source = ChatDomainService().FindChatById(fixture.app, fixture.source_id);
	source->messages[2].markdown_store_prompt_blocks.pop_back();
	fixture.Refuses(std::vector<std::string>{fixture.files[0]});
	fixture.Refuses(fixture.files);
	source->messages = fixture.source_messages;
	source->messages[2].markdown_store_files[1] = fixture.files[0];
	fixture.Refuses(std::vector<std::string>{fixture.files[0]});
	source->messages = fixture.source_messages;
	source->messages[2].markdown_store_prompt_blocks[0].clear();
	fixture.Refuses(std::vector<std::string>{fixture.files[0]});
	// An explicit empty selection may remove this unretained legacy representation.
	UAM_ASSERT(fixture.Create(std::vector<std::string>{}, std::nullopt, "Edited input with captured resources.", "remove-empty-snapshot"));
	ChatDomainService().FindChatById(fixture.app, fixture.source_id)->messages = fixture.source_messages;
	fixture.AssertBranch({}, {});
	UAM_ASSERT(fixture.Create(std::vector<std::string>{fixture.files[0]}));
	fixture.AssertBranch({fixture.files[0]}, {fixture.blocks[0]});
}

UAM_TEST(BranchDeliveredSkillOmissionPreservesLegacyUnpairedSnapshots)
{
	BranchSkillSnapshotFixture fixture;
	auto* source = ChatDomainService().FindChatById(fixture.app, fixture.source_id);
	source->messages[2].markdown_store_prompt_blocks.pop_back();
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, *source));
	fixture.source_messages = source->messages;
	UAM_ASSERT(fixture.Create(std::nullopt));
	fixture.AssertBranch(fixture.files, {fixture.blocks[0], fixture.blocks[1]});
}

UAM_TEST(BranchDeliveredSkillOperationReuseComparesEffectivePrefixAndTarget)
{
	BranchSkillSnapshotFixture fixture;
	const std::vector<std::string> all_reversed = {fixture.files[2], fixture.files[1], fixture.files[0]};
	bool reused = true;
	UAM_ASSERT(fixture.Create(all_reversed, std::nullopt, "Edited input with captured resources.", "reuse-skills", &reused));
	UAM_ASSERT(!reused);
	const auto id = fixture.AssertBranch(fixture.files, fixture.blocks);
	const auto generations = fixture.Generations();
	const auto count = fixture.app.chats.size();
	const auto branch_primary = AppPaths::UamChatFilePath(fixture.app.data_root, id);
	const auto branch_bytes = uam_test::ReadFile(branch_primary);
	for (const std::optional<std::vector<std::string>>& selection : {std::optional<std::vector<std::string>>{}, std::optional<std::vector<std::string>>{fixture.files}, std::optional<std::vector<std::string>>{all_reversed}})
	{
		reused = false;
		UAM_ASSERT(fixture.Create(selection, std::nullopt, "Edited input with captured resources.", "reuse-skills", &reused));
		UAM_ASSERT(reused);
		UAM_ASSERT_EQ(fixture.AssertBranch(fixture.files, fixture.blocks), id);
	}
	fixture.Refuses(std::vector<std::string>{fixture.files[0]}, std::nullopt, "Edited input with captured resources.", "reuse-skills");
	auto* source = ChatDomainService().FindChatById(fixture.app, fixture.source_id);
	source->messages[2].markdown_store_prompt_blocks[0] += "changed";
	fixture.Refuses(all_reversed, std::nullopt, "Edited input with captured resources.", "reuse-skills");
	source->messages = fixture.source_messages;
	source->messages[0].markdown_store_prompt_blocks[0] += "changed prefix";
	fixture.Refuses(std::nullopt, std::nullopt, "Edited input with captured resources.", "reuse-skills");
	source->messages = fixture.source_messages;
	UAM_ASSERT_EQ(fixture.app.chats.size(), count);
	UAM_ASSERT_EQ(fixture.Generations(), generations);
	UAM_ASSERT_EQ(uam_test::ReadFile(branch_primary), branch_bytes);
	fixture.AssertSource();
}

UAM_TEST(BranchPreparedOriginalSkillRequestAndCandidateWitnessAreExact)
{
	BranchSkillSnapshotFixture fixture;
	const auto source = *ChatDomainService().FindChatById(fixture.app, fixture.source_id);
	const auto final_owner = ChatDomainService().CreateNewChat("", uam::provider_ids::kGeminiCli, false).id;
	const std::vector<std::string> selection = {fixture.files[2], fixture.files[0]};
	std::string error;
	const auto generations = fixture.Generations();
	ChatSession invalid_snapshot = source;
	invalid_snapshot.messages[2].markdown_store_prompt_blocks[0].clear();
	auto rejected = uam::PreparedBranchOriginals::Prepare(fixture.app.data_root, invalid_snapshot, final_owner, 2, std::nullopt, error, {}, selection);
	UAM_ASSERT(!rejected && !error.empty());
	UAM_ASSERT_EQ(fixture.Generations(), generations);
	auto originals = uam::PreparedBranchOriginals::Prepare(fixture.app.data_root, source, final_owner, 2, std::nullopt, error, {}, selection);
	UAM_ASSERT(originals && error.empty() && originals->MatchesSource(source));
	UAM_ASSERT(originals->MatchesRequest(fixture.app.data_root, fixture.source_id, 2, std::nullopt, selection));
	UAM_ASSERT(!originals->MatchesRequest(fixture.app.data_root, fixture.source_id, 2, std::nullopt, std::vector<std::string>{fixture.files[0]}));
	fixture.Refuses(std::vector<std::string>{fixture.files[0]}, std::nullopt, "Edited input with captured resources.", "skill-operation", nullptr, originals.get());
	ChatSession candidate = source;
	candidate.id = final_owner;
	candidate.parent_chat_id = source.id;
	candidate.branch_from_message_index = 2;
	candidate.messages[2].markdown_store_files = {fixture.files[0], fixture.files[2]};
	candidate.messages[2].markdown_store_prompt_blocks = {fixture.blocks[0], fixture.blocks[2]};
	const auto expected_messages = candidate.messages;
	candidate.messages[0].markdown_store_prompt_blocks[0] += "changed prefix";
	const auto changed_prefix = candidate.messages;
	UAM_ASSERT(!originals->ApplyTo(candidate, error) && !error.empty());
	UAM_ASSERT_EQ(candidate.messages, changed_prefix);
	candidate.messages = expected_messages;
	candidate.messages[2].markdown_store_prompt_blocks[1] += "changed target";
	const auto changed_target = candidate.messages;
	UAM_ASSERT(!originals->ApplyTo(candidate, error) && !error.empty());
	UAM_ASSERT_EQ(candidate.messages, changed_target);
	candidate.messages = expected_messages;
	UAM_ASSERT(originals->MatchesRequest(fixture.app.data_root, fixture.source_id, 2, std::nullopt, selection));
	UAM_ASSERT(originals->ApplyTo(candidate, error));
	UAM_ASSERT(!originals->MatchesRequest(fixture.app.data_root, fixture.source_id, 2, std::nullopt, selection));
	UAM_ASSERT(!originals->PrimaryAcknowledged());
	for (const std::size_t row : {std::size_t{1}, std::size_t{2}})
	{
		const auto& attachment = candidate.messages[row].attachments.front();
		UAM_ASSERT(uam::native_assets::IsKeyForOwner(final_owner, attachment.native_asset_key));
		const auto read = uam::native_assets::ReadOwned(fixture.app.data_root, final_owner, attachment.native_asset_key);
		UAM_ASSERT(read.ok && read.error.empty());
		UAM_ASSERT_EQ(read.bytes, fixture.expected_original);
	}
	fixture.AssertSource();
	fixture.AssertNoRuntime();
}

UAM_TEST(BranchPreparedOriginalRejectsSameRevisionChangedSourceSkillSnapshots)
{
	BranchSkillSnapshotFixture fixture;
	auto* source = ChatDomainService().FindChatById(fixture.app, fixture.source_id);
	const auto revision = ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.source_id);
	const auto final_owner = ChatDomainService().CreateNewChat("", uam::provider_ids::kGeminiCli, false).id;
	const std::vector<std::string> selection = {fixture.files[0]};
	std::string error;
	auto originals = uam::PreparedBranchOriginals::Prepare(fixture.app.data_root, *source, final_owner, 2, std::nullopt, error, {}, selection);
	UAM_ASSERT(originals && error.empty() && originals->MatchesSource(*source));
	for (const std::size_t row : {std::size_t{0}, std::size_t{2}})
	{
		// Target row changes a DROPPED block: the original witness must retain it too.
		source->messages[row].markdown_store_prompt_blocks[row == 2 ? 1 : 0] += "same-revision changed snapshot";
		UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.source_id), revision);
		UAM_ASSERT(!originals->MatchesSource(*source));
		fixture.Refuses(selection, std::nullopt, "Edited input with captured resources.", "skill-operation", nullptr, originals.get());
		source->messages = fixture.source_messages;
	}
	UAM_ASSERT(originals->MatchesSource(*source));
	UAM_ASSERT(fixture.Create(selection, std::nullopt, "Edited input with captured resources.", "skill-operation", nullptr, nullptr, originals.get()));
	UAM_ASSERT_EQ(fixture.AssertBranch({fixture.files[0]}, {fixture.blocks[0]}), final_owner);
}

UAM_TEST(BranchDeliveredSkillPreparedSourceRejectsLoadedPrefixSnapshotChange)
{
	BranchSkillSnapshotFixture fixture;
	auto* source = ChatDomainService().FindChatById(fixture.app, fixture.source_id);
	const ChatSession prepared_source = *source;
	const auto revision = ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.source_id);
	source->messages[0].markdown_store_prompt_blocks[0] += "loaded source changed without save";
	UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.source_id), revision);
	fixture.Refuses(std::vector<std::string>{fixture.files[0]}, std::nullopt, "Edited input with captured resources.", "skill-operation", &prepared_source);
	source->messages = fixture.source_messages;
	UAM_ASSERT(fixture.Create(std::vector<std::string>{fixture.files[0]}, std::nullopt, "Edited input with captured resources.", "skill-operation", nullptr, &prepared_source));
	fixture.AssertBranch({fixture.files[0]}, {fixture.blocks[0]});
}
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && (defined(__APPLE__) || defined(_WIN32))
namespace
{
	void ReloadBranchSkillSourceWithMissingSnapshot(BranchSkillSnapshotFixture& fixture)
	{
		auto* source = ChatDomainService().FindChatById(fixture.app, fixture.source_id);
		UAM_ASSERT(source && source->messages_loaded && source->messages.size() == 3);
		UAM_ASSERT_EQ(source->messages[2].markdown_store_files, fixture.files);
		UAM_ASSERT_EQ(source->messages[2].markdown_store_prompt_blocks, fixture.blocks);
		source->messages[2].markdown_store_prompt_blocks.back().clear();
		UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, *source));
		const auto reloaded = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.source_id);
		UAM_ASSERT(reloaded && reloaded->messages_loaded && reloaded->messages.size() == 3);
		UAM_ASSERT_EQ(reloaded->messages[2].markdown_store_files, fixture.files);
		UAM_ASSERT_EQ(reloaded->messages[2].markdown_store_prompt_blocks, (std::vector<std::string>{fixture.blocks[0], fixture.blocks[1]}));
		UAM_ASSERT_EQ(reloaded->messages[2].attachments, fixture.source_messages[2].attachments);
		*source = *reloaded;
		fixture.source_messages = source->messages;
		// Adopt only the actual reload. Do not resave/repair the persisted missing snapshot.
		fixture.AssertSource();
		fixture.AssertNoRuntime();
	}
} // namespace

UAM_TEST(BranchDeliveredEmptySkillSelectionRemovesReloadedMissingSnapshots)
{
	BranchSkillSnapshotFixture fixture;
	ReloadBranchSkillSourceWithMissingSnapshot(fixture);
	const auto primary = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.source_id);
	const auto bytes = uam_test::ReadFile(primary);
	const auto revision = ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.source_id);
	// Nonempty retention remains strict even if the requested first block exists.
	fixture.Refuses(std::vector<std::string>{fixture.files[0]});
	UAM_ASSERT(fixture.Create(std::vector<std::string>{}));
	fixture.AssertBranch({}, {});
	UAM_ASSERT_EQ(uam_test::ReadFile(primary), bytes);
	UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.source_id), revision);
}

UAM_TEST(BranchPreparedOriginalEmptySkillSelectionRemovesReloadedMissingSnapshots)
{
	BranchSkillSnapshotFixture fixture;
	ReloadBranchSkillSourceWithMissingSnapshot(fixture);
	const auto source = *ChatDomainService().FindChatById(fixture.app, fixture.source_id);
	const auto primary = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.source_id);
	const auto bytes = uam_test::ReadFile(primary);
	const auto revision = ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.source_id);
	const auto final_owner = ChatDomainService().CreateNewChat("", uam::provider_ids::kGeminiCli, false).id;
	const auto generations = fixture.Generations();
	std::string error;
	auto rejected = uam::PreparedBranchOriginals::Prepare(fixture.app.data_root, source, final_owner, 2, std::nullopt, error, {}, std::vector<std::string>{fixture.files[0]});
	UAM_ASSERT(!rejected && !error.empty());
	UAM_ASSERT_EQ(fixture.Generations(), generations);
	const std::vector<std::string> none;
	auto originals = uam::PreparedBranchOriginals::Prepare(fixture.app.data_root, source, final_owner, 2, std::nullopt, error, {}, none);
	UAM_ASSERT(originals && error.empty());
	UAM_ASSERT(originals->MatchesSource(source));
	UAM_ASSERT(originals->MatchesRequest(fixture.app.data_root, fixture.source_id, 2, std::nullopt, none));
	UAM_ASSERT(!originals->MatchesRequest(fixture.app.data_root, fixture.source_id, 2, std::nullopt));
	UAM_ASSERT(!originals->PrimaryAcknowledged());
	ChatSession candidate = source;
	candidate.id = final_owner;
	candidate.parent_chat_id = source.id;
	candidate.branch_from_message_index = 2;
	candidate.native_session_id.clear();
	// Explicit removal must refuse a candidate that still retains a target skill.
	candidate.messages[2].markdown_store_files = {fixture.files[0]};
	candidate.messages[2].markdown_store_prompt_blocks = {fixture.blocks[0]};
	const auto rejected_messages = candidate.messages;
	UAM_ASSERT(!originals->ApplyTo(candidate, error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT_EQ(candidate.messages, rejected_messages);
	for (const std::size_t row : {std::size_t{1}, std::size_t{2}})
		UAM_ASSERT_EQ(candidate.messages[row].attachments.front().native_asset_key, source.messages[row].attachments.front().native_asset_key);
	UAM_ASSERT(originals->MatchesRequest(fixture.app.data_root, fixture.source_id, 2, std::nullopt, none));
	UAM_ASSERT(!originals->PrimaryAcknowledged());
	error.clear();
	candidate.messages[2].markdown_store_files.clear();
	candidate.messages[2].markdown_store_prompt_blocks.clear();
	auto expected_messages = candidate.messages;
	UAM_ASSERT(originals->ApplyTo(candidate, error));
	UAM_ASSERT(candidate.messages[2].markdown_store_files.empty() && candidate.messages[2].markdown_store_prompt_blocks.empty());
	for (const std::size_t row : {std::size_t{1}, std::size_t{2}})
	{
		const auto& attachment = candidate.messages[row].attachments.front();
		UAM_ASSERT(uam::native_assets::IsKeyForOwner(final_owner, attachment.native_asset_key));
		UAM_ASSERT(!uam::native_assets::IsKeyForOwner(fixture.source_id, attachment.native_asset_key));
		expected_messages[row].attachments.front().native_asset_key = attachment.native_asset_key;
		const auto original = uam::native_assets::ReadOwned(fixture.app.data_root, final_owner, attachment.native_asset_key);
		UAM_ASSERT(original.ok && original.error.empty());
		UAM_ASSERT_EQ(original.bytes, fixture.expected_original);
	}
	UAM_ASSERT_EQ(candidate.messages, expected_messages);
	UAM_ASSERT(!originals->MatchesRequest(fixture.app.data_root, fixture.source_id, 2, std::nullopt, none));
	UAM_ASSERT(!originals->PrimaryAcknowledged());
	UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, final_owner), std::optional<std::string>("missing"));
	UAM_ASSERT_EQ(uam_test::ReadFile(primary), bytes);
	UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.source_id), revision);
	fixture.AssertSource();
	fixture.AssertNoRuntime();
}

UAM_TEST(BranchPreparedOriginalOmittedSkillsRejectStalePrefixAndAcceptRestoredSnapshot)
{
	BranchSkillSnapshotFixture fixture;
	auto* source = ChatDomainService().FindChatById(fixture.app, fixture.source_id);
	const auto primary = AppPaths::UamChatFilePath(fixture.app.data_root, fixture.source_id);
	const auto bytes = uam_test::ReadFile(primary);
	const auto revision = ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.source_id);
	const auto final_owner = ChatDomainService().CreateNewChat("", uam::provider_ids::kGeminiCli, false).id;
	std::string error;
	// Old argument list: the new skill selection is omitted, not an explicit all/empty request.
	auto originals = uam::PreparedBranchOriginals::Prepare(fixture.app.data_root, *source, final_owner, 2, std::nullopt, error);
	UAM_ASSERT(originals && error.empty() && originals->MatchesSource(*source));
	UAM_ASSERT(originals->MatchesRequest(fixture.app.data_root, fixture.source_id, 2, std::nullopt));
	const auto generations = fixture.Generations();
	const auto selected = ChatDomainService().SelectedChatId(fixture.app);
	const auto count = fixture.app.chats.size();
	source->messages[0].markdown_store_prompt_blocks[0] += "omitted selection prefix drift";
	const auto changed_messages = source->messages;
	UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.source_id), revision);
	UAM_ASSERT(!originals->MatchesSource(*source));
	UAM_ASSERT(!ChatDomainService().CreateBranchFromMessage(fixture.app, fixture.source_id, 2, std::string("Edited input with captured resources."), std::string("omitted-skill-witness"), nullptr, std::nullopt, nullptr, nullptr, originals.get()));
	UAM_ASSERT(!fixture.app.status_line.empty());
	UAM_ASSERT_EQ(ChatDomainService().FindChatById(fixture.app, fixture.source_id)->messages, changed_messages);
	UAM_ASSERT_EQ(fixture.app.chats.size(), count);
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(fixture.app), selected);
	UAM_ASSERT_EQ(fixture.Generations(), generations);
	UAM_ASSERT_EQ(uam_test::ReadFile(primary), bytes);
	UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.source_id), revision);
	UAM_ASSERT(originals->MatchesRequest(fixture.app.data_root, fixture.source_id, 2, std::nullopt));
	UAM_ASSERT(!originals->PrimaryAcknowledged());
	source->messages = fixture.source_messages;
	UAM_ASSERT(originals->MatchesSource(*source));
	UAM_ASSERT(ChatDomainService().CreateBranchFromMessage(fixture.app, fixture.source_id, 2, std::string("Edited input with captured resources."), std::string("omitted-skill-witness"), nullptr, std::nullopt, nullptr, nullptr, originals.get()));
	UAM_ASSERT_EQ(fixture.AssertBranch(fixture.files, fixture.blocks), final_owner);
	UAM_ASSERT(originals->PrimaryAcknowledged());
	UAM_ASSERT(originals->AcknowledgeOriginals(error) && error.empty());
	UAM_ASSERT_EQ(fixture.Generations(), generations);
	UAM_ASSERT_EQ(uam_test::ReadFile(primary), bytes);
	UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.source_id), revision);
	fixture.AssertSource();
	fixture.AssertNoRuntime();
}
#endif


#include "common/runtime/acp/acp_session_internal.h"
#include "common/platform/async_byte_writer.h"
#include <sstream>

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && UAM_ENABLE_RUNTIME_CLAUDE_CLI && (defined(__APPLE__) || defined(_WIN32))
namespace
{
	struct ClaudeOwnedRetryFixtureR71 : BranchNativeOriginalFixture
	{
		std::string branch_id, branch_key, source_bytes;
		std::optional<std::string> source_revision;
		uam::AcpSessionState* session = nullptr;
		std::shared_ptr<std::string> wire = std::make_shared<std::string>();
		std::shared_ptr<uam::platform::AsyncByteWriter> observed, transport;

		ClaudeOwnedRetryFixtureR71()
		{
			app.settings.central_provider_configuration.enabled = false;
			app.settings.central_provider_configuration.uam_control_enabled = false;
			app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
			auto* source = ChatDomainService().FindChatById(app, source_id);
			UAM_ASSERT(source && source->messages_loaded);
			source->memory_enabled = source->uam_control_enabled = false;
			source->approval_mode = "default";
			// Reuse the actually published original in the selected user, alongside its older assistant reference.
			source->messages[2].attachments = source->messages[1].attachments;
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *source));
			source_messages = source->messages;
			SwitchTo(uam::provider_ids::kClaudeCli);
			UAM_ASSERT(ChatDomainService().CreateBranchFromMessage(app, source_id, 2, std::string("Describe the retained original PNG.")));
			const auto* selected = ChatDomainService().SelectedChat(app);
			UAM_ASSERT(selected && selected->id != source_id && selected->native_session_id.empty());
			branch_id = selected->id;
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, branch_id);
			UAM_ASSERT(saved && saved->messages_loaded && saved->messages.size() == 3);
			UAM_ASSERT(saved->parent_chat_id == source_id && saved->branch_from_message_index == 2);
			UAM_ASSERT_EQ(saved->messages[2].attachments.size(), std::size_t{1});
			branch_key = saved->messages[2].attachments.front().native_asset_key;
			UAM_ASSERT(uam::native_assets::IsKeyForOwner(branch_id, branch_key));
			UAM_ASSERT(!uam::native_assets::IsKeyForOwner(source_id, branch_key));
			UAM_ASSERT(saved->messages[2].attachments.front().path.empty());
			const auto original = uam::native_assets::ReadOwned(app.data_root, branch_id, branch_key);
			UAM_ASSERT(original.ok && original.error.empty());
			UAM_ASSERT_EQ(original.bytes, expected_original);
			*ChatDomainService().FindChatById(app, branch_id) = *saved;
			source_bytes = uam_test::ReadFile(AppPaths::UamChatFilePath(app.data_root, source_id));
			source_revision = ChatRepository::ChatStorageRevision(app.data_root, source_id);
			UAM_ASSERT(!source_bytes.empty() && source_revision);
			AssertSource();
			AssertNoRuntime();
		}

		~ClaudeOwnedRetryFixtureR71() { CloseSink(); }
		ChatSession& Branch() { return *ChatDomainService().FindChatById(app, branch_id); }

		void StartSink()
		{
			UAM_ASSERT(!session && Branch().native_session_id.empty());
			session = &uam::acp_detail::EnsureAcpSessionForChat(app, Branch());
			session->protocol_kind = "claude-code-stream-json";
#if defined(_WIN32)
			const std::vector<std::string> argv = {"cmd.exe", "/d", "/c", "more > NUL"};
#else
			const std::vector<std::string> argv = {"/bin/sh", "-c", "cat >/dev/null"};
#endif
			auto& process = PlatformServicesFactory::Instance().process_service;
			std::string error;
			UAM_ASSERT(process.StartStdioProcess(*session, temp.root, argv, &error));
			session->process_execution_host_id = Branch().execution_host_id;
			session->running = session->initialized = session->session_ready = true;
			session->lifecycle_state = "ready";
			session->current_mode_id = "default";
			session->current_model_id = Branch().model_id;
			session->last_process_id = uam::acp_detail::AcpProcessHandleLabel(*session);
			UAM_ASSERT(session->session_id.empty()); // Claude permits input without inventing a native session ID.
			UAM_ASSERT(process.WriteToStdioProcess(*session, "\n", 1, &error));
			transport = session->stdin_writer;
			UAM_ASSERT(transport && transport->Flush(std::chrono::seconds(1)));
			observed = std::make_shared<uam::platform::AsyncByteWriter>(
			    [capture = wire, writer = transport](const char* bytes, std::size_t count, std::string& failure) -> std::ptrdiff_t
			    {
				    capture->append(bytes, count);
				    return writer->Enqueue(bytes, count, &failure) ? static_cast<std::ptrdiff_t>(count) : -1;
			    }, [] {});
			session->stdin_writer = observed;
			UAM_ASSERT(uam::acp_detail::ValidateAcpRestartOwnership(app, *session, Branch(), &error));
		}

		void CloseSink()
		{
			if (observed) observed->Stop();
			if (transport) transport->Stop();
			if (session)
			{
				auto& process = PlatformServicesFactory::Instance().process_service;
				process.StopStdioProcess(*session, true);
				process.CloseStdioProcessHandles(*session);
				session->running = session->initialized = session->session_ready = false;
			}
		}

		bool BeginRetry(std::string& error) { return uam::RetryLastAcpPrompt(app, branch_id, &error); }

		void Drain()
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (session->processing && !session->queued_prompt.empty() && session->prompt_request_id == 0 && std::chrono::steady_clock::now() < deadline)
			{
				(void)uam::FlushPendingChatSaves(app);
				(void)uam::acp_detail::SendQueuedPromptIfReady(app, *session, Branch());
				std::this_thread::yield();
			}
			UAM_ASSERT(!session->processing || session->queued_prompt.empty() || session->prompt_request_id != 0);
			UAM_ASSERT(observed->Flush(std::chrono::seconds(1)) && transport->Flush(std::chrono::seconds(1)));
		}

		std::shared_ptr<uam::ClaudeInputPreparation> FinishedWorkerBeforeCompletion()
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (!session->claude_input_preparation && session->processing && std::chrono::steady_clock::now() < deadline)
			{
				(void)uam::FlushPendingChatSaves(app);
				(void)uam::acp_detail::SendQueuedPromptIfReady(app, *session, Branch());
				std::this_thread::yield();
			}
			UAM_ASSERT(session->claude_input_preparation);
			const auto state = session->claude_input_preparation->state;
			// No owner polling after the real worker starts: mutation precedes completion consumption.
			while (!state->finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
				std::this_thread::yield();
			UAM_ASSERT(state->finished.load(std::memory_order_acquire));
			UAM_ASSERT_EQ(session->prompt_request_id, 0);
			UAM_ASSERT(!session->pending_prompt_delivery && wire->empty());
			return state;
		}

		std::vector<nlohmann::json> UserMessages() const
		{
			std::vector<nlohmann::json> result;
			std::istringstream lines(*wire);
			std::string line;
			while (std::getline(lines, line))
			{
				if (line.empty()) continue;
				const auto request = nlohmann::json::parse(line, nullptr, false);
				UAM_ASSERT(!request.is_discarded());
				if (request.is_object() && (request.value("type", "") == "user" ||
				    (request.contains("message") && request["message"].is_object() && request["message"].value("role", "") == "user")))
					result.push_back(request);
			}
			return result;
		}

		std::vector<nlohmann::json> Images() const
		{
			std::vector<nlohmann::json> result;
			for (const auto& request : UserMessages())
			{
				UAM_ASSERT(request.contains("message") && request["message"].is_object() && request["message"].contains("content") && request["message"]["content"].is_array());
				for (const auto& part : request["message"]["content"])
					if (part.is_object() && part.value("type", "") == "image") result.push_back(part);
			}
			return result;
		}

		void AssertOriginalSource()
		{
			UAM_ASSERT_EQ(uam_test::ReadFile(AppPaths::UamChatFilePath(app.data_root, source_id)), source_bytes);
			UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(app.data_root, source_id), source_revision);
			AssertSource();
			UAM_ASSERT(app.pending_acp_remote_stops.empty());
			// Public preparation may retain a terminal record; it must remain completely inert.
			for (const auto& terminal : app.cli_terminals)
			{
				UAM_ASSERT(terminal && !terminal->running && !terminal->input_writer);
				UAM_ASSERT(!terminal->uam_control_session && !terminal->uam_control_relay);
				UAM_ASSERT(!terminal->context_preparation && !terminal->native_session_setup_cancel);
				UAM_ASSERT(!uam::platform::CliTerminalHasWritableInput(*terminal));
#if defined(_WIN32)
				UAM_ASSERT(terminal->process_info.dwProcessId == 0 && terminal->pseudo_console == nullptr);
#else
				UAM_ASSERT(terminal->child_pid == -1 && terminal->watchdog_pid == -1);
#endif
			}
			UAM_ASSERT(app.worktree_operation_chat_ids.empty());
		}

		fs::path BlobPath() const
		{
			std::vector<std::string> parts;
			std::istringstream text(branch_key);
			std::string part;
			while (std::getline(text, part, ':')) parts.push_back(part);
			UAM_ASSERT(parts.size() == 5 && parts[0] == "na1");
			const auto blob = app.data_root / std::string(uam::native_assets::kStoreDirectory) / parts[1] / parts[2] / (parts[3] + ".bin");
			UAM_ASSERT(fs::is_regular_file(blob));
			UAM_ASSERT_EQ(uam_test::ReadFile(blob), expected_original);
			return blob;
		}
	};
} // namespace

UAM_TEST(ClaudeSavedBranchRetryDeliversOwnedPngThroughRealBoundedWorker)
{
	ClaudeOwnedRetryFixtureR71 fixture;
	fixture.StartSink();
	const int initial_next_request = fixture.session->next_request_id;
	std::string error;
	UAM_ASSERT(fixture.BeginRetry(error) && error.empty());
	fixture.Drain();
	const auto users = fixture.UserMessages();
	UAM_ASSERT_EQ(users.size(), std::size_t{1}); // Intended RED: no user envelope reaches the sink today.
	const auto images = fixture.Images();
	UAM_ASSERT_EQ(images.size(), std::size_t{1});
	UAM_ASSERT_EQ(images.front()["source"]["type"], nlohmann::json("base64"));
	UAM_ASSERT_EQ(images.front()["source"]["media_type"], nlohmann::json("image/png"));
	std::string decoded;
	UAM_ASSERT(uam::base64::Decode(images.front()["source"]["data"].get<std::string>(), decoded));
	UAM_ASSERT_EQ(decoded, fixture.expected_original);
	const auto& captured_name = fixture.Branch().messages[2].attachments.front().name;
	UAM_ASSERT(!captured_name.empty() && fixture.wire->find(captured_name) != std::string::npos);
	std::string prompt_text;
	for (const auto& part : users.front()["message"]["content"])
		if (part.is_object() && part.value("type", "") == "text") prompt_text += part.value("text", "");
	UAM_ASSERT(prompt_text.find(captured_name) != std::string::npos);
	UAM_ASSERT(fixture.session->last_error.empty() && fixture.session->processing && fixture.session->queued_prompt.empty());
	UAM_ASSERT_EQ(fixture.session->prompt_request_id, 0); // Claude stream has no protocol request method/ID.
	UAM_ASSERT_EQ(fixture.session->next_request_id, initial_next_request + 1); // One internal dispatch allocation.
	UAM_ASSERT(fixture.session->pending_request_methods.empty());
	UAM_ASSERT(fixture.wire->find(fixture.branch_key) == std::string::npos && fixture.wire->find(fixture.source_key) == std::string::npos);
	UAM_ASSERT(fixture.wire->find("localImage") == std::string::npos && fixture.wire->find("file://") == std::string::npos);
	const auto original = uam::native_assets::ReadOwned(fixture.app.data_root, fixture.branch_id, fixture.branch_key);
	UAM_ASSERT(original.ok && original.error.empty());
	UAM_ASSERT_EQ(original.bytes, fixture.expected_original);
	fixture.AssertOriginalSource();
}

UAM_TEST(ClaudeDirectSendCannotBorrowSavedOwnedRetryAuthority)
{
	ClaudeOwnedRetryFixtureR71 fixture;
	fixture.StartSink();
	const auto attachments = fixture.Branch().messages[2].attachments;
	std::string error;
	const bool accepted = uam::SendAcpPrompt(fixture.app, fixture.branch_id, "New direct input must not borrow saved Retry authority.", {}, attachments, false, &error);
	if (accepted) fixture.Drain();
	UAM_ASSERT(!error.empty() || !fixture.session->last_error.empty());
	UAM_ASSERT(fixture.UserMessages().empty() && fixture.session->prompt_request_id == 0);
	fixture.AssertOriginalSource();
}

UAM_TEST(ClaudeSavedOwnedRetryRejectsCorruptMissingForeignMixedAndOpaqueSources)
{
	for (const std::string variation : {"corrupt", "missing", "foreign", "mixed", "opaque"})
	{
		ClaudeOwnedRetryFixtureR71 fixture;
		const auto blob = fixture.BlobPath();
		const auto sentinel = fixture.temp.root / "real-mixed-source.png";
		if (variation == "corrupt") UAM_ASSERT(uam::io::WriteBinaryFile(blob, std::string(fixture.expected_original.size(), 'x')));
		else if (variation == "missing") UAM_ASSERT(fs::remove(blob));
		else if (variation == "foreign") fixture.Branch().messages[2].attachments.front().native_asset_key = fixture.source_key;
		else if (variation == "mixed")
		{
			UAM_ASSERT(uam::io::WriteBinaryFile(sentinel, fixture.expected_original));
			fixture.Branch().messages[2].attachments.front().path = sentinel.string();
		}
		else
		{
			auto opaque = BranchOpaqueReference();
			opaque.kind = "image"; opaque.mime_type = "image/png";
			fixture.Branch().messages[2].attachments = {opaque};
			UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.Branch()));
			const auto reloaded = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.branch_id);
			UAM_ASSERT(reloaded && reloaded->messages[2].attachments.front() == opaque);
			fixture.Branch() = *reloaded;
		}
		fixture.StartSink();
		std::string error;
		const bool accepted = fixture.BeginRetry(error);
		if (accepted) fixture.Drain();
		UAM_ASSERT(!error.empty() || !fixture.session->last_error.empty());
		UAM_ASSERT(fixture.UserMessages().empty() && fixture.session->prompt_request_id == 0);
		if (variation == "mixed") UAM_ASSERT_EQ(uam_test::ReadFile(sentinel), fixture.expected_original);
		if (variation == "corrupt" || variation == "missing") UAM_ASSERT(uam::io::WriteBinaryFile(blob, fixture.expected_original));
		fixture.AssertOriginalSource();
	}
}

UAM_TEST(ClaudeSavedOwnedRetryRejectsStopRootIncarnationAndDeclarationDriftBeforeCompletion)
{
	for (const std::string mutation : {"stop", "root", "incarnation", "declaration"})
	{
		ClaudeOwnedRetryFixtureR71 fixture;
		fixture.StartSink();
		std::string error;
		UAM_ASSERT(fixture.BeginRetry(error) && error.empty());
		const auto state = fixture.FinishedWorkerBeforeCompletion();
		// Today's unavailable-source error must not masquerade as successful stale-result coverage.
		UAM_ASSERT(state->error.empty() && state->message.is_object());
		UAM_ASSERT(state->message.contains("message") && state->message["message"].is_object() && state->message["message"].contains("content") && state->message["message"]["content"].is_array());
		std::size_t prepared_images = 0;
		for (const auto& part : state->message["message"]["content"])
			if (part.is_object() && part.value("type", "") == "image") ++prepared_images;
		UAM_ASSERT_EQ(prepared_images, std::size_t{1});
		const auto original_root = fixture.app.data_root;
		const auto original_incarnation = fixture.Branch().metadata_incarnation;
		const auto original_attachment = fixture.Branch().messages[2].attachments.front();
		const auto tasks = fixture.app.claude_input_tasks.size();
		const int next_request = fixture.session->next_request_id;
		if (mutation == "stop") (void)uam::StopAcpSession(fixture.app, fixture.branch_id);
		else if (mutation == "root")
		{
			fixture.app.data_root = fixture.temp.root / "different-owned-root";
			fs::create_directories(fixture.app.data_root);
		}
		else if (mutation == "incarnation") fixture.Branch().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		else fixture.Branch().messages[2].attachments.front().native_part_key += ":different-declaration";
		const auto changed_attachment = fixture.Branch().messages[2].attachments.front();
		if (mutation != "stop")
		{
			(void)uam::acp_detail::SendQueuedPromptIfReady(fixture.app, *fixture.session, fixture.Branch());
			UAM_ASSERT(fixture.observed->Flush(std::chrono::seconds(1)));
			UAM_ASSERT(!fixture.session->pending_prompt_delivery && fixture.session->prompt_request_id == 0);
			UAM_ASSERT_EQ(fixture.session->next_request_id, next_request);
			UAM_ASSERT_EQ(fixture.app.claude_input_tasks.size(), tasks); // No re-authorisation under changed scope.
		}
		else fixture.CloseSink(); // Joins the private writer before inspecting any cancellation control lines.
		UAM_ASSERT(fixture.UserMessages().empty());
		UAM_ASSERT(state->cancel.stop_requested());
		UAM_ASSERT_EQ(fixture.Branch().messages[2].attachments.front(), changed_attachment);
		fixture.app.data_root = original_root;
		fixture.Branch().metadata_incarnation = original_incarnation;
		fixture.Branch().messages[2].attachments.front() = original_attachment;
		fixture.AssertOriginalSource();
	}
}
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && UAM_ENABLE_RUNTIME_CLAUDE_CLI && (defined(__APPLE__) || defined(_WIN32))
UAM_TEST(ClaudeSavedOwnedRetryRevokesCompletedWorkerForAppendedLegacyFile)
{
	ClaudeOwnedRetryFixtureR71 fixture;
	fixture.StartSink();
	std::string error;
	UAM_ASSERT(fixture.BeginRetry(error) && error.empty());
	const auto state = fixture.FinishedWorkerBeforeCompletion();
	UAM_ASSERT(state->error.empty() && state->message.is_object());
	UAM_ASSERT(state->message.contains("message") && state->message["message"].contains("content") && state->message["message"]["content"].is_array());
	UAM_ASSERT_EQ(std::ranges::count_if(state->message["message"]["content"], [](const nlohmann::json& part) { return part.is_object() && part.value("type", "") == "image"; }), std::ptrdiff_t{1});
	const auto original_declarations = fixture.Branch().messages[2].attachments;
	const auto tasks = fixture.app.claude_input_tasks.size();
	const int next_request = fixture.session->next_request_id;
	const auto file = fixture.temp.root / "new-real-legacy-document.txt";
	const std::string bytes = "Real non-image declaration added after image preparation.";
	UAM_ASSERT(uam::io::WriteTextFile(file, bytes));
	MessageAttachment legacy;
	legacy.id = "appended-legacy-document";
	legacy.name = "new-real-legacy-document.txt";
	legacy.kind = "file";
	legacy.mime_type = "text/plain";
	legacy.source = AttachmentSource::LegacyPath;
	legacy.path = file.string();
	fixture.Branch().messages[2].attachments.push_back(legacy);
	const auto changed_declarations = fixture.Branch().messages[2].attachments;
	(void)uam::acp_detail::SendQueuedPromptIfReady(fixture.app, *fixture.session, fixture.Branch());
	UAM_ASSERT_EQ(fixture.Branch().messages[2].attachments, changed_declarations);
	UAM_ASSERT(!fixture.session->saved_image_retry && !fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery);
	UAM_ASSERT(state->cancel.stop_requested() && !fixture.session->last_error.empty());
	fixture.Branch().messages[2].attachments = original_declarations;
	(void)uam::acp_detail::SendQueuedPromptIfReady(fixture.app, *fixture.session, fixture.Branch());
	UAM_ASSERT(fixture.observed->Flush(std::chrono::seconds(1)) && fixture.transport->Flush(std::chrono::seconds(1)));
	UAM_ASSERT(fixture.UserMessages().empty() && fixture.session->prompt_request_id == 0);
	UAM_ASSERT_EQ(fixture.session->next_request_id, next_request);
	UAM_ASSERT_EQ(fixture.app.claude_input_tasks.size(), tasks);
	UAM_ASSERT(!fixture.session->saved_image_retry && !fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery);
	UAM_ASSERT_EQ(uam_test::ReadFile(file), bytes);
	(void)fixture.BlobPath();
	fixture.AssertOriginalSource();
}

UAM_TEST(ClaudeSavedOwnedRetryRevokesActualPendingDurablePacketForDeclarationDrift)
{
	ClaudeOwnedRetryFixtureR71 fixture;
	fixture.StartSink();
	std::string error;
	UAM_ASSERT(fixture.BeginRetry(error) && error.empty());
	const auto state = fixture.FinishedWorkerBeforeCompletion();
	UAM_ASSERT(state->error.empty() && state->message.is_object());
	UAM_ASSERT(state->message.contains("message") && state->message["message"].contains("content") && state->message["message"]["content"].is_array());
	UAM_ASSERT_EQ(std::ranges::count_if(state->message["message"]["content"], [](const nlohmann::json& part) { return part.is_object() && part.value("type", "") == "image"; }), std::ptrdiff_t{1});
	UAM_ASSERT(fixture.session->defer_prompt_save && !fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.branch_id));
	// Exactly one owner call consumes the actual worker and stages its real save packet.
	UAM_ASSERT(uam::acp_detail::SendQueuedPromptIfReady(fixture.app, *fixture.session, fixture.Branch()));
	const auto packet = fixture.session->pending_prompt_delivery;
	UAM_ASSERT(packet && fixture.session->claude_input_preparation && fixture.session->claude_input_preparation->state == state);
	const auto save_completions = std::make_shared<std::atomic<int>>(0);
	UAM_ASSERT(uam::DeferChatSaveResult(fixture.app, fixture.branch_id, [save_completions](bool) { ++*save_completions; }));
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.branch_id) && fixture.wire->empty());
	const auto original_declarations = fixture.Branch().messages[2].attachments;
	const auto tasks = fixture.app.claude_input_tasks.size();
	const int next_request = fixture.session->next_request_id;
	fixture.Branch().messages[2].attachments.front().name += " changed while actual delivery receipt pending";
	const auto changed_declarations = fixture.Branch().messages[2].attachments;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.branch_id) && std::chrono::steady_clock::now() < deadline)
	{
		(void)uam::FlushPendingChatSaves(fixture.app);
		std::this_thread::yield();
	}
	UAM_ASSERT(!fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.branch_id));
	UAM_ASSERT_EQ(save_completions->load(), 1);
	(void)uam::acp_detail::SendQueuedPromptIfReady(fixture.app, *fixture.session, fixture.Branch());
	UAM_ASSERT_EQ(fixture.Branch().messages[2].attachments, changed_declarations);
	UAM_ASSERT(!fixture.session->saved_image_retry && !fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery);
	fixture.Branch().messages[2].attachments = original_declarations;
	(void)uam::acp_detail::SendQueuedPromptIfReady(fixture.app, *fixture.session, fixture.Branch());
	UAM_ASSERT(fixture.observed->Flush(std::chrono::seconds(1)) && fixture.transport->Flush(std::chrono::seconds(1)));
	UAM_ASSERT(fixture.UserMessages().empty() && fixture.session->prompt_request_id == 0);
	UAM_ASSERT_EQ(fixture.session->next_request_id, next_request);
	UAM_ASSERT_EQ(fixture.app.claude_input_tasks.size(), tasks);
	UAM_ASSERT(state->cancel.stop_requested() && !fixture.session->last_error.empty());
	UAM_ASSERT(!fixture.session->saved_image_retry && !fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery);
	(void)fixture.BlobPath();
	fixture.AssertOriginalSource();
}

UAM_TEST(ClaudeSavedOwnedRetryRetainsUnboundPermissionAndRejectsChangedRowBeforeWorkspaceLaunch)
{
	ClaudeOwnedRetryFixtureR71 fixture;
	fixture.StartSink();
	fixture.Branch().svn_update_pending = true;
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.Branch()));
	const auto recorded = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.branch_id);
	UAM_ASSERT(recorded && recorded->svn_update_pending);
	std::string error;
	UAM_ASSERT(fixture.BeginRetry(error) && error.empty());
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{1});
	const auto queued = fixture.session->queued_user_prompts.front();
	const auto permission = fixture.session->saved_image_retry;
	UAM_ASSERT(permission && !queued.id.empty() && fixture.session->saved_image_retry_turn_serial == -1);
	UAM_ASSERT_EQ(permission->queued_id, queued.id);
	UAM_ASSERT_EQ(permission->queued_revision, queued.revision);
	UAM_ASSERT_EQ(permission->root, fixture.app.data_root);
	UAM_ASSERT(permission->incarnation == fixture.Branch().metadata_incarnation);
	UAM_ASSERT_EQ(permission->chat_id, fixture.branch_id);
	UAM_ASSERT_EQ(permission->row_index, 2);
	UAM_ASSERT_EQ(permission->row_created_at, fixture.Branch().messages[2].created_at);
	UAM_ASSERT_EQ(permission->content, fixture.Branch().messages[2].content);
	UAM_ASSERT_EQ(permission->attachments, fixture.Branch().messages[2].attachments);
	UAM_ASSERT(!queued.append_user_message && queued.existing_user_message_index == 2);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.branch_id) && std::chrono::steady_clock::now() < deadline)
	{
		(void)uam::FlushPendingChatSaves(fixture.app);
		std::this_thread::yield();
	}
	UAM_ASSERT(!fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.branch_id));
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.branch_id);
	UAM_ASSERT(saved && saved->svn_update_pending && saved->acp_queued_prompts.size() == 1);
	UAM_ASSERT_EQ(saved->acp_queued_prompts.front().id, queued.id);
	UAM_ASSERT_EQ(saved->acp_queued_prompts.front().revision, queued.revision);
	UAM_ASSERT(fixture.app.worktree_launch_preparations.empty() && fixture.app.worktree_operation_chat_ids.empty());
	UAM_ASSERT(!fixture.session->worktree_launch_pending && !fixture.session->pending_local_launch && fixture.app.claude_input_tasks.empty());
	const auto process = uam::acp_detail::AcpProcessHandleLabel(*fixture.session);
	fixture.Branch().messages[2].attachments.front().native_part_key += ":changed-before-workspace";
	const auto changed_declarations = fixture.Branch().messages[2].attachments;
	UAM_ASSERT(!uam::acp_detail::StartAcpProcessForChat(fixture.app, *fixture.session, fixture.Branch(), &error));
	UAM_ASSERT_EQ(error, std::string("Saved image Retry ownership changed. Retry the saved message again."));
	UAM_ASSERT(!fixture.session->saved_image_retry && fixture.session->saved_image_retry_turn_serial == -1);
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.front().id, queued.id);
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.front().revision, queued.revision);
	UAM_ASSERT_EQ(fixture.Branch().messages[2].attachments, changed_declarations);
	UAM_ASSERT(fixture.Branch().svn_update_pending && fixture.session->running && !fixture.session->processing);
	UAM_ASSERT_EQ(uam::acp_detail::AcpProcessHandleLabel(*fixture.session), process);
	UAM_ASSERT(fixture.app.worktree_launch_preparations.empty() && fixture.app.worktree_operation_chat_ids.empty());
	UAM_ASSERT(!fixture.session->worktree_launch_pending && !fixture.session->pending_local_launch && fixture.app.claude_input_tasks.empty());
	UAM_ASSERT(fixture.observed->Flush(std::chrono::seconds(1)) && fixture.transport->Flush(std::chrono::seconds(1)));
	UAM_ASSERT(fixture.UserMessages().empty() && fixture.session->prompt_request_id == 0);
	(void)fixture.BlobPath();
	fixture.AssertOriginalSource();
}
#endif


#include "app/worktree_launch_preparation_service.h"

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && UAM_ENABLE_RUNTIME_CLAUDE_CLI && (defined(__APPLE__) || defined(_WIN32))
UAM_TEST(ClaudeSavedOwnedRetrySurvivesIdleProviderStopAndRealWorkspacePending)
{
	ClaudeOwnedRetryFixtureR71 fixture;
	fixture.StartSink();
	UAM_ASSERT(fixture.session->running && fixture.session->session_ready && !fixture.session->processing);
	fixture.app.settings.svn_enabled = false; // The real update-only worker must not execute SVN.
	UAM_ASSERT(!fixture.app.settings.git_enabled && !fixture.app.settings.git_auto_worktree);
	fixture.Branch().svn_update_pending = true;
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.Branch()));
	const auto recorded = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.branch_id);
	UAM_ASSERT(recorded && recorded->svn_update_pending);
	std::string error;
	UAM_ASSERT(fixture.BeginRetry(error) && error.empty());
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{1});
	const auto queued = fixture.session->queued_user_prompts.front();
	const auto permission = fixture.session->saved_image_retry;
	auto* const original_session = fixture.session;
	UAM_ASSERT(permission && !queued.id.empty() && permission->queued_id == queued.id && permission->queued_revision == queued.revision);
	UAM_ASSERT_EQ(fixture.session->saved_image_retry_turn_serial, -1);
	UAM_ASSERT(queued.direct_admission && !queued.append_user_message && queued.existing_user_message_index == 2);
	UAM_ASSERT_EQ(permission->attachments, fixture.Branch().messages[2].attachments);
	const auto save_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.branch_id) && std::chrono::steady_clock::now() < save_deadline)
	{
		(void)uam::FlushPendingChatSaves(fixture.app);
		std::this_thread::yield();
	}
	UAM_ASSERT(!fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.branch_id));
	const auto saved_queue = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.branch_id);
	UAM_ASSERT(saved_queue && saved_queue->svn_update_pending && saved_queue->acp_queued_prompts.size() == 1);
	UAM_ASSERT_EQ(saved_queue->acp_queued_prompts.front(), queued);
	UAM_ASSERT(fixture.session->running && !fixture.session->processing && fixture.UserMessages().empty());

	// A real exclusive retention lease prevents workspace mutation; no task or permission is fabricated.
	auto cleanup = fixture.app.worktree_retention_gate.TryAcquireCleanup();
	UAM_ASSERT(cleanup && cleanup->IsExclusive());
	UAM_ASSERT(!fixture.app.worktree_retention_gate.TryAcquireMutation());
	UAM_ASSERT(!uam::DrainNextQueuedAcpUserPrompt(fixture.app, *fixture.session, fixture.Branch()));
	UAM_ASSERT(!fixture.session->running && !fixture.session->processing);
	UAM_ASSERT(fixture.session->worktree_launch_pending && fixture.session->lifecycle_state == "waiting_workspace");
	UAM_ASSERT(fixture.Branch().svn_update_pending && !fixture.session->queue_held && !fixture.Branch().acp_queue_held);
	UAM_ASSERT(fixture.app.worktree_launch_preparations.empty() && fixture.app.worktree_operation_chat_ids.empty());
	UAM_ASSERT(!fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery && !fixture.session->pending_local_launch);
	UAM_ASSERT(fixture.app.claude_input_tasks.empty() && fixture.app.local_acp_launch_tasks.empty());
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.front(), queued);
	UAM_ASSERT_EQ(fixture.Branch().acp_queued_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.Branch().acp_queued_prompts.front(), queued);
	UAM_ASSERT(fixture.UserMessages().empty() && fixture.Images().empty() && fixture.session->prompt_request_id == 0);
	UAM_ASSERT_EQ(fixture.session->saved_image_retry, permission); // Intended RED: internal Stop loses this exact Retry permission.
	UAM_ASSERT_EQ(fixture.session->saved_image_retry_turn_serial, -1);
	UAM_ASSERT_EQ(permission->queued_id, queued.id);
	UAM_ASSERT_EQ(permission->queued_revision, queued.revision);

	// Reap only the genuine private sink stop while workspace mutation is still excluded.
	const auto stop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!fixture.app.acp_process_stop_tasks.empty() && std::chrono::steady_clock::now() < stop_deadline)
	{
		(void)uam::PollAllAcpSessions(fixture.app);
		(void)uam::FlushPendingChatSaves(fixture.app);
		UAM_ASSERT_EQ(fixture.session->saved_image_retry, permission);
		UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{1});
		UAM_ASSERT_EQ(fixture.session->queued_user_prompts.front(), queued);
		UAM_ASSERT(!fixture.session->running && fixture.session->worktree_launch_pending && fixture.UserMessages().empty());
		std::this_thread::yield();
	}
	UAM_ASSERT(fixture.app.acp_process_stop_tasks.empty() && !fixture.session->local_stop_pending);
	fixture.CloseSink();
	UAM_ASSERT(fixture.UserMessages().empty() && fixture.Images().empty());
	cleanup.reset();

	// Public workspace/save polling only: never poll runtime reconnect after Ready or launch installed Claude.
	const auto started = uam::PrepareWorktreeForLaunch(fixture.app, fixture.Branch());
	UAM_ASSERT_EQ(started.state, uam::WorktreeLaunchPreparationState::Pending);
	const auto task = fixture.app.worktree_launch_preparations.at(fixture.branch_id);
	UAM_ASSERT(task && task->worker.valid() && task->update_only && !task->svn_enabled && !task->git_enabled);
	uam::WorktreeLaunchPreparationState workspace = started.state;
	const auto workspace_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (workspace == uam::WorktreeLaunchPreparationState::Pending && std::chrono::steady_clock::now() < workspace_deadline)
	{
		(void)uam::FlushPendingChatSaves(fixture.app);
		workspace = uam::PrepareWorktreeForLaunch(fixture.app, fixture.Branch()).state;
		UAM_ASSERT_EQ(fixture.session->saved_image_retry, permission);
		UAM_ASSERT_EQ(fixture.session->saved_image_retry_turn_serial, -1);
		UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{1});
		UAM_ASSERT_EQ(fixture.session->queued_user_prompts.front(), queued);
		UAM_ASSERT(fixture.UserMessages().empty() && fixture.Images().empty());
		std::this_thread::yield();
	}
	UAM_ASSERT_EQ(workspace, uam::WorktreeLaunchPreparationState::Ready);
	UAM_ASSERT(task->prepared && task->prepared->operation.ok && task->merged && task->saved && *task->saved);
	UAM_ASSERT(!fixture.Branch().svn_update_pending && !fixture.session->running);
	UAM_ASSERT_EQ(uam::acp_detail::PrepareAcpWorkspaceForLaunch(fixture.app, *fixture.session, fixture.Branch(), &error), uam::WorktreeLaunchPreparationState::Ready);
	UAM_ASSERT(!fixture.session->worktree_launch_pending && error.empty());
	UAM_ASSERT_EQ(fixture.Branch().acp_queued_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.Branch().acp_queued_prompts.front(), queued);
	UAM_ASSERT_EQ(fixture.session->saved_image_retry, permission);
	UAM_ASSERT(fixture.app.worktree_operation_chat_ids.empty() && fixture.app.local_acp_launch_tasks.empty());

	// Reuse the same owned session and real private transport, without a second Retry/capture.
	fixture.session = nullptr;
	fixture.StartSink();
	UAM_ASSERT_EQ(fixture.session, original_session);
	UAM_ASSERT_EQ(fixture.session->saved_image_retry, permission);
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.size(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.session->queued_user_prompts.front(), queued);
	const int next_request = fixture.session->next_request_id;
	UAM_ASSERT(uam::DrainNextQueuedAcpUserPrompt(fixture.app, *fixture.session, fixture.Branch()));
	fixture.Drain();
	const auto users = fixture.UserMessages();
	const auto images = fixture.Images();
	UAM_ASSERT_EQ(users.size(), std::size_t{1});
	UAM_ASSERT_EQ(images.size(), std::size_t{1});
	UAM_ASSERT_EQ(images.front()["source"]["type"], nlohmann::json("base64"));
	UAM_ASSERT_EQ(images.front()["source"]["media_type"], nlohmann::json("image/png"));
	std::string decoded;
	UAM_ASSERT(uam::base64::Decode(images.front()["source"]["data"].get<std::string>(), decoded));
	UAM_ASSERT_EQ(decoded, fixture.expected_original);
	std::string prompt_text;
	for (const auto& part : users.front()["message"]["content"])
		if (part.is_object() && part.value("type", "") == "text") prompt_text += part.value("text", "");
	UAM_ASSERT(prompt_text.find(fixture.Branch().messages[2].attachments.front().name) != std::string::npos);
	UAM_ASSERT(fixture.wire->find(fixture.branch_key) == std::string::npos && fixture.wire->find(fixture.source_key) == std::string::npos);
	UAM_ASSERT(fixture.wire->find("localImage") == std::string::npos && fixture.wire->find("file://") == std::string::npos);
	UAM_ASSERT(fixture.session->processing && fixture.session->queued_prompt.empty() && fixture.session->last_error.empty());
	UAM_ASSERT(fixture.session->queued_user_prompts.empty() && fixture.Branch().acp_queued_prompts.empty());
	UAM_ASSERT(!fixture.session->saved_image_retry && !fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery);
	UAM_ASSERT_EQ(fixture.session->prompt_request_id, 0); // Claude stream has no wire request ID.
	UAM_ASSERT_EQ(fixture.session->next_request_id, next_request + 1);
	UAM_ASSERT(fixture.session->pending_request_methods.empty());
	(void)fixture.BlobPath();
	fixture.AssertOriginalSource();
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && UAM_ENABLE_RUNTIME_CLAUDE_CLI && UAM_ENABLE_RUNTIME_CODEX_CLI && defined(__APPLE__)
namespace
{
	struct CodexOwnedRetryFixtureR75
	{
		// Reverse destruction keeps PATH/private peer alive through AppState and all owned task teardown.
		uam_test::TempDir peer_root{"uam-private-codex-image-peer"};
		std::unique_ptr<uam_test::ScopedEnvVar> private_path;
		ClaudeOwnedRetryFixtureR71 fixture;
		std::string native_thread;
		const fs::path original_root = fixture.app.data_root;

		CodexOwnedRetryFixtureR75()
		{
			const auto executable = peer_root.root / "codex";
			UAM_ASSERT(uam::io::WriteTextFile(executable, R"PY(#!/usr/bin/python3 -I
import json, os, sys, uuid
if sys.argv[1:] != ["--disable", "computer_use", "app-server", "--listen", "stdio://"]:
    raise RuntimeError("Only the private app-server protocol fixture is allowed")
trace_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "trace.json")
thread = str(uuid.uuid4())
traffic = []
def record(direction, value):
    traffic.append({"direction": direction, "value": value})
    temporary = trace_path + ".tmp"
    with open(temporary, "w", encoding="utf-8") as stream:
        json.dump(traffic, stream, separators=(",", ":"))
    os.replace(temporary, trace_path)
for line in sys.stdin:
    request = json.loads(line)
    record("request", request)
    method = request.get("method", "")
    if method == "initialized":
        continue
    if method == "initialize":
        result = {"userAgent": "codex-cli/0.148.0"}
    elif method == "model/list":
        result = {"data": [{"id": "gpt-5.4", "displayName": "Private fixture model", "isDefault": True,
                   "defaultReasoningEffort": "low", "supportedReasoningEfforts": [{"reasoningEffort": "low"}, {"reasoningEffort": "high"}],
                   "additionalSpeedTiers": ["fast"]}], "currentModelId": "gpt-5.4", "nextCursor": None}
    elif method == "account/rateLimits/read":
        result = {"rateLimits": {}}
    elif method == "thread/start":
        result = {"thread": {"id": thread}, "model": "gpt-5.4", "approvalsReviewer": "user"}
    elif method == "turn/start":
        continue  # Deliberately retain the real pending request until test inspection/Stop; no model runs.
    elif method == "turn/interrupt":
        result = {}
    else:
        raise RuntimeError("Unexpected private protocol method: " + method)
    response = {"jsonrpc": "2.0", "id": request["id"], "result": result}
    record("response", response)
    print(json.dumps(response, separators=(",", ":")), flush=True)
)PY"));
			fs::permissions(executable, fs::perms::owner_all);
			private_path = std::make_unique<uam_test::ScopedEnvVar>("PATH", peer_root.root.string());
			auto& app = fixture.app;
			UAM_ASSERT(!app.provider_model_catalog);
			app.settings.central_provider_configuration = {};
			app.settings.central_provider_configuration.enabled = false;
			app.settings.central_provider_configuration.uam_control_enabled = false;
			app.settings.mcp_servers.clear();
			app.settings.host_scoped_settings.clear();
			app.settings.git_enabled = app.settings.git_auto_worktree = app.settings.svn_enabled = false;
			app.settings.computer_use_globally_enabled = false;
			UAM_ASSERT_EQ(uam::SwitchChatProvider(app, fixture.branch_id, uam::provider_ids::kCodexCli), uam::ChatProviderSwitchResult::Changed);
			auto& chat = fixture.Branch();
			chat.memory_enabled = chat.uam_control_enabled = chat.computer_use_enabled = false;
			chat.model_id.clear(); // Actual parsed model/list + thread/start provide the native fallback model.
			chat.reasoning_effort.clear();
			chat.service_tier.clear();
			chat.service_tier_explicit = false;
			chat.approval_mode = "default";
			chat.command_safety_tier = "off";
			UAM_ASSERT(chat.native_session_id.empty() && chat.messages[2].attachments.front().path.empty());
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
			const auto reloaded = ChatRepository::LoadLocalChat(app.data_root, fixture.branch_id);
			UAM_ASSERT(reloaded && reloaded->provider_id == uam::provider_ids::kCodexCli && reloaded->native_session_id.empty());
			UAM_ASSERT_EQ(reloaded->messages[2].attachments.front().native_asset_key, fixture.branch_key);
			chat = *reloaded;
			std::string error;
			const bool started = uam::StartAcpModelDiscovery(app, fixture.branch_id, &error, false);
			fixture.session = uam::FindAcpSessionForChat(app, fixture.branch_id);
			UAM_ASSERT(started && error.empty() && fixture.session);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while ((!fixture.session->session_ready || !fixture.session->pending_request_methods.empty()) && std::chrono::steady_clock::now() < deadline)
			{
				(void)uam::PollAllAcpSessions(app);
				std::this_thread::yield();
			}
			UAM_ASSERT(fixture.session->running && fixture.session->initialized && fixture.session->session_ready && !fixture.session->processing);
			UAM_ASSERT(!fixture.session->model_discovery_only && fixture.session->pending_request_methods.empty() && fixture.session->last_error.empty());
			const auto traffic = Trace();
			int setups = 0;
			for (const auto& item : traffic)
			{
				if (item["direction"] != "request" || item["value"].value("method", "") != "thread/start") continue;
				++setups;
				const auto id = item["value"]["id"];
				UAM_ASSERT(!item["value"]["params"].contains("threadId"));
				for (const auto& reply : traffic)
					if (reply["direction"] == "response" && reply["value"]["id"] == id)
						native_thread = reply["value"]["result"]["thread"]["id"].get<std::string>();
			}
			UAM_ASSERT_EQ(setups, 1);
			UAM_ASSERT(!native_thread.empty());
			UAM_ASSERT_EQ(fixture.session->session_id, native_thread);
			UAM_ASSERT_EQ(fixture.session->codex_thread_id, native_thread);
			UAM_ASSERT_EQ(fixture.Branch().native_session_id, native_thread);
			const auto durable = ChatRepository::LoadLocalChat(app.data_root, fixture.branch_id);
			UAM_ASSERT(durable && durable->native_session_id == native_thread);
			UAM_ASSERT_EQ(fixture.session->current_model_id, std::string("gpt-5.4"));
			fixture.transport = fixture.session->stdin_writer;
			UAM_ASSERT(fixture.transport && fixture.transport->Flush(std::chrono::seconds(1)));
			fixture.observed = std::make_shared<uam::platform::AsyncByteWriter>(
			    [capture = fixture.wire, writer = fixture.transport](const char* bytes, std::size_t count, std::string& failure) -> std::ptrdiff_t
			    {
				    capture->append(bytes, count);
				    return writer->Enqueue(bytes, count, &failure) ? static_cast<std::ptrdiff_t>(count) : -1;
			    }, [] {});
			fixture.session->stdin_writer = fixture.observed;
			UAM_ASSERT(uam::acp_detail::ValidateAcpRestartOwnership(app, *fixture.session, fixture.Branch(), &error));
			UAM_ASSERT(Turns().empty() && PeerTurns().empty());
			fixture.AssertOriginalSource();
		}

		~CodexOwnedRetryFixtureR75()
		{
			fixture.app.data_root = original_root;
			if (!fixture.session) fixture.session = uam::FindAcpSessionForChat(fixture.app, fixture.branch_id);
			if (fixture.session)
			{
				(void)uam::StopAcpSession(fixture.app, fixture.branch_id);
				fixture.CloseSink();
			}
			// AppState still owns workers and stop packets; join before its destructor and before PATH restoration.
			for (auto& task : fixture.app.acp_process_stop_tasks)
				if (task.worker && task.worker->joinable()) task.worker->join();
			fixture.app.acp_process_stop_tasks.clear();
			for (auto& task : fixture.app.claude_input_tasks)
			{
				if (task.state) task.state->cancel.request_stop();
				if (task.worker && task.worker->joinable()) task.worker->join();
			}
		}

		nlohmann::json Trace() const
		{
			const auto value = nlohmann::json::parse(uam_test::ReadFile(peer_root.root / "trace.json"), nullptr, false);
			UAM_ASSERT(value.is_array());
			return value;
		}

		std::vector<nlohmann::json> PeerTurns() const
		{
			std::vector<nlohmann::json> result;
			for (const auto& item : Trace())
				if (item["direction"] == "request" && item["value"].value("method", "") == "turn/start") result.push_back(item["value"]);
			return result;
		}

		std::vector<nlohmann::json> Turns()
		{
			if (fixture.session->running)
				UAM_ASSERT(fixture.observed->Flush(std::chrono::seconds(1)) && fixture.transport->Flush(std::chrono::seconds(1)));
			std::vector<nlohmann::json> result;
			std::istringstream lines(*fixture.wire);
			std::string line;
			while (std::getline(lines, line))
			{
				const auto value = nlohmann::json::parse(line, nullptr, false);
				UAM_ASSERT(value.is_object());
				if (value.value("method", "") == "turn/start") result.push_back(value);
			}
			return result;
		}

		void RequireRetry()
		{
			std::string error;
			if (!fixture.BeginRetry(error))
				throw uam_test::TestFailure("Codex accepted-Retry prerequisite after parsed private thread setup: " + error);
			UAM_ASSERT(error.empty());
		}

		std::shared_ptr<uam::ClaudeInputPreparation> FinishedImageWorker()
		{
			const auto state = fixture.FinishedWorkerBeforeCompletion();
			UAM_ASSERT(state->error.empty() && state->message.is_object());
			UAM_ASSERT(state->message.contains("message") && state->message["message"].contains("content") && state->message["message"]["content"].is_array());
			UAM_ASSERT_EQ(std::ranges::count_if(state->message["message"]["content"], [](const nlohmann::json& part) { return part.is_object() && part.value("type", "") == "image"; }), std::ptrdiff_t{1});
			UAM_ASSERT(Turns().empty() && PeerTurns().empty());
			return state;
		}

		nlohmann::json DeliveredTurn()
		{
			fixture.Drain();
			const auto turns = Turns();
			UAM_ASSERT_EQ(turns.size(), std::size_t{1});
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (PeerTurns().empty() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
			const auto peer_turns = PeerTurns();
			UAM_ASSERT_EQ(peer_turns.size(), std::size_t{1});
			UAM_ASSERT_EQ(peer_turns.front(), turns.front());
			UAM_ASSERT_EQ(turns.front()["params"]["threadId"], nlohmann::json(native_thread));
			UAM_ASSERT_EQ(turns.front()["id"], nlohmann::json(fixture.session->prompt_request_id));
			UAM_ASSERT_EQ(fixture.session->pending_request_methods.at(fixture.session->prompt_request_id), std::string("turn/start"));
			UAM_ASSERT(fixture.session->processing && fixture.session->queued_prompt.empty() && fixture.session->last_error.empty());
			return turns.front();
		}

		void AssertNoTurn()
		{
			UAM_ASSERT(Turns().empty() && PeerTurns().empty()); // Counts text-only turn/start as well as image delivery.
			UAM_ASSERT_EQ(fixture.session->prompt_request_id, 0);
		}

		static std::string DecodeImage(const nlohmann::json& part, const std::string& mime = "image/png")
		{
			UAM_ASSERT_EQ(part["type"], nlohmann::json("image"));
			UAM_ASSERT(!part.contains("path"));
			const auto url = part["url"].get<std::string>();
			const std::string prefix = "data:" + mime + ";base64,";
			UAM_ASSERT(url.starts_with(prefix));
			std::string decoded;
			UAM_ASSERT(uam::base64::Decode(url.substr(prefix.size()), decoded));
			return decoded;
		}
	};
} // namespace

UAM_TEST(CodexSavedBranchRetryDeliversOwnedPngThroughParsedThreadAndBoundedWorker)
{
	CodexOwnedRetryFixtureR75 peer;
	auto& fixture = peer.fixture;
	const int next_request = fixture.session->next_request_id;
	peer.RequireRetry(); // Intended old-source RED: exact supported-provider refusal, after real setup/reload.
	const auto turn = peer.DeliveredTurn();
	const auto& input = turn["params"]["input"];
	UAM_ASSERT(input.is_array());
	std::string text;
	std::vector<nlohmann::json> images;
	for (const auto& part : input)
	{
		if (part.value("type", "") == "text") text += part.value("text", "");
		else if (part.value("type", "") == "image") images.push_back(part);
		else UAM_ASSERT(false); // No localImage/path/key transport for the saved original.
	}
	UAM_ASSERT_EQ(images.size(), std::size_t{1});
	UAM_ASSERT_EQ(CodexOwnedRetryFixtureR75::DecodeImage(images.front()), fixture.expected_original);
	UAM_ASSERT(text.find(fixture.Branch().messages[2].attachments.front().name) != std::string::npos);
	UAM_ASSERT(text.find(fixture.Branch().messages[2].content) != std::string::npos);
	UAM_ASSERT_EQ(turn["params"]["collaborationMode"]["settings"]["model"], nlohmann::json("gpt-5.4"));
	UAM_ASSERT_EQ(turn["params"]["approvalPolicy"], nlohmann::json("on-request"));
	UAM_ASSERT_EQ(turn["params"]["approvalsReviewer"], nlohmann::json("user"));
	UAM_ASSERT_EQ(turn["params"]["sandboxPolicy"]["type"], nlohmann::json("workspaceWrite"));
	UAM_ASSERT_EQ(fixture.session->next_request_id, next_request + 1);
	UAM_ASSERT(turn.dump().find(fixture.branch_key) == std::string::npos && turn.dump().find(fixture.source_key) == std::string::npos);
	UAM_ASSERT(!fixture.session->saved_image_retry && !fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery);
	(void)fixture.BlobPath();
	fixture.AssertOriginalSource();
}

UAM_TEST(CodexOwnedDirectInputRefusesWhileOrdinaryLegacyImageKeepsNativePath)
{
	for (const bool owned : {true, false})
	{
		CodexOwnedRetryFixtureR75 peer;
		auto& fixture = peer.fixture;
		auto attachment = fixture.Branch().messages[2].attachments.front();
		const auto file = fixture.temp.root / "ordinary-real-image.png";
		if (!owned)
		{
			UAM_ASSERT(uam::io::WriteBinaryFile(file, fixture.expected_original));
			attachment = {};
			attachment.id = "ordinary-path-image"; attachment.name = "ordinary-real-image.png";
			attachment.kind = "image"; attachment.mime_type = "image/png"; attachment.path = file.string();
		}
		std::string error;
		const bool accepted = uam::SendAcpPrompt(fixture.app, fixture.branch_id, "This is fresh direct input.", {}, {attachment}, false, &error);
		if (owned)
		{
			if (accepted) fixture.Drain();
			UAM_ASSERT(!error.empty() || !fixture.session->last_error.empty());
			peer.AssertNoTurn();
		}
		else
		{
			UAM_ASSERT(accepted && error.empty() && !fixture.session->saved_image_retry && !fixture.session->claude_input_preparation);
			const auto turn = peer.DeliveredTurn();
			std::size_t paths = 0;
			for (const auto& part : turn["params"]["input"])
			{
				UAM_ASSERT(part.value("type", "") != "image");
				if (part.value("type", "") == "localImage") { ++paths; UAM_ASSERT_EQ(part["path"], nlohmann::json(file.string())); }
			}
			UAM_ASSERT_EQ(paths, std::size_t{1});
			UAM_ASSERT_EQ(uam_test::ReadFile(file), fixture.expected_original);
		}
		fixture.AssertOriginalSource();
	}
}

UAM_TEST(CodexSavedOwnedRetryRejectsInvalidCompleteSourcesWithoutTextFallback)
{
	for (const std::string variant : {"corrupt", "missing", "foreign", "mixed", "uri", "pending", "nonimage", "mime"})
	{
		CodexOwnedRetryFixtureR75 peer;
		auto& fixture = peer.fixture;
		const auto blob = fixture.BlobPath();
		const auto original = fixture.Branch().messages[2].attachments;
		auto& attachment = fixture.Branch().messages[2].attachments.front();
		if (variant == "corrupt") UAM_ASSERT(uam::io::WriteBinaryFile(blob, std::string(fixture.expected_original.size(), 'x')));
		else if (variant == "missing") UAM_ASSERT(fs::remove(blob));
		else if (variant == "foreign") attachment.native_asset_key = fixture.source_key;
		else if (variant == "mixed")
		{
			const auto sentinel = fixture.temp.root / "mixed-real-image.png";
			UAM_ASSERT(uam::io::WriteBinaryFile(sentinel, fixture.expected_original));
			attachment.path = sentinel.string();
		}
		else if (variant == "uri")
		{
			attachment = BranchOpaqueReference(); attachment.kind = "image"; attachment.mime_type = "image/png";
			UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.Branch()));
			const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.branch_id);
			UAM_ASSERT(saved && saved->messages[2].attachments.front() == attachment);
		}
		else if (variant == "pending") attachment = BranchPendingOriginal();
		else if (variant == "nonimage") attachment.kind = "file";
		else attachment.mime_type = "image/tiff";
		std::string error;
		const bool accepted = fixture.BeginRetry(error);
		if (accepted) fixture.Drain();
		UAM_ASSERT(!error.empty() || !fixture.session->last_error.empty());
		peer.AssertNoTurn();
		if (variant == "corrupt" || variant == "missing") UAM_ASSERT(uam::io::WriteBinaryFile(blob, fixture.expected_original));
		fixture.Branch().messages[2].attachments = original;
		(void)fixture.BlobPath();
		fixture.AssertOriginalSource();
	}
}

UAM_TEST(CodexSavedOwnedRetryRejectsFinishedWorkerSourceAndNativeMetadataDrift)
{
	for (const std::string variant : {"root", "incarnation", "vector", "name", "effort", "tier-explicit", "reviewer", "mode", "current-model", "goal-review", "agent-access", "stop", "cancel"})
	{
		CodexOwnedRetryFixtureR75 peer;
		auto& fixture = peer.fixture;
		peer.RequireRetry(); // Old-source accepted-operation prerequisite RED; never call it stale-result proof.
		const auto state = peer.FinishedImageWorker();
		const auto original = fixture.Branch();
		const auto current_model = fixture.session->current_model_id;
		const auto agent_access = fixture.session->active_uam_agent_workspace_access;
		const bool goal_review = fixture.session->goal_review_turn;
		const auto tasks = fixture.app.claude_input_tasks.size();
		const int next_request = fixture.session->next_request_id;
		if (variant == "root") { fixture.app.data_root = fixture.temp.root / "changed-image-root"; fs::create_directories(fixture.app.data_root); }
		else if (variant == "incarnation") fixture.Branch().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		else if (variant == "vector") fixture.Branch().messages[2].attachments.push_back(fixture.Branch().messages[2].attachments.front());
		else if (variant == "name") fixture.Branch().messages[2].attachments.front().name += " changed after worker";
		else if (variant == "effort") fixture.Branch().reasoning_effort = "high";
		else if (variant == "tier-explicit") fixture.Branch().service_tier_explicit = true;
		else if (variant == "reviewer") fixture.Branch().command_safety_tier = "aiReview";
		else if (variant == "mode") fixture.Branch().approval_mode = "plan";
		else if (variant == "current-model") fixture.session->current_model_id = "changed-native-fallback";
		else if (variant == "goal-review") fixture.session->goal_review_turn = true;
		else if (variant == "agent-access") fixture.session->active_uam_agent_workspace_access = "read";
		else if (variant == "stop") (void)uam::StopAcpSession(fixture.app, fixture.branch_id);
		else { std::string error; (void)uam::CancelAcpTurn(fixture.app, fixture.branch_id, &error); }
		if (variant == "stop") fixture.CloseSink();
		else (void)uam::acp_detail::SendQueuedPromptIfReady(fixture.app, *fixture.session, fixture.Branch());
		peer.AssertNoTurn();
		UAM_ASSERT(state->cancel.stop_requested());
		UAM_ASSERT(!fixture.session->saved_image_retry && !fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery);
		UAM_ASSERT_EQ(fixture.session->next_request_id, next_request + (variant == "cancel" ? 1 : 0)); // Public Cancel allocates even without a native turn id.
		UAM_ASSERT_EQ(fixture.app.claude_input_tasks.size(), tasks);
		fixture.app.data_root = peer.original_root;
		fixture.Branch().metadata_incarnation = original.metadata_incarnation;
		fixture.Branch().messages[2].attachments = original.messages[2].attachments;
		fixture.Branch().reasoning_effort = original.reasoning_effort;
		fixture.Branch().service_tier_explicit = original.service_tier_explicit;
		fixture.Branch().command_safety_tier = original.command_safety_tier;
		fixture.Branch().approval_mode = original.approval_mode;
		fixture.session->current_model_id = current_model;
		fixture.session->goal_review_turn = goal_review;
		fixture.session->active_uam_agent_workspace_access = agent_access;
		if (fixture.session->running) (void)uam::acp_detail::SendQueuedPromptIfReady(fixture.app, *fixture.session, fixture.Branch());
		peer.AssertNoTurn();
		UAM_ASSERT_EQ(fixture.app.claude_input_tasks.size(), tasks); // Restoring old values never recaptures authority.
		UAM_ASSERT_EQ(fixture.session->next_request_id, next_request + (variant == "cancel" ? 1 : 0));
		UAM_ASSERT(!fixture.session->saved_image_retry && !fixture.session->claude_input_preparation);
		fixture.AssertOriginalSource();
	}
}

UAM_TEST(CodexSavedOwnedRetryRejectsRealDurablePacketDriftBeforeNativeWrite)
{
	for (const std::string variant : {"declaration", "native-thread", "model", "tier-explicit", "policy"})
	{
		CodexOwnedRetryFixtureR75 peer;
		auto& fixture = peer.fixture;
		peer.RequireRetry();
		const auto state = peer.FinishedImageWorker();
		UAM_ASSERT(fixture.session->defer_prompt_save && !fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.branch_id));
		UAM_ASSERT(uam::acp_detail::SendQueuedPromptIfReady(fixture.app, *fixture.session, fixture.Branch()));
		const auto packet = fixture.session->pending_prompt_delivery;
		UAM_ASSERT(packet && fixture.session->claude_input_preparation && fixture.session->claude_input_preparation->state == state);
		const auto completed = std::make_shared<std::atomic<int>>(0);
		UAM_ASSERT(uam::DeferChatSaveResult(fixture.app, fixture.branch_id, [completed](bool) { ++*completed; }));
		UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.branch_id));
		peer.AssertNoTurn();
		const auto original = fixture.Branch();
		const auto session_id = fixture.session->session_id;
		const auto tasks = fixture.app.claude_input_tasks.size();
		const int next_request = fixture.session->next_request_id;
		if (variant == "declaration") fixture.Branch().messages[2].attachments.front().name += " changed during durable save";
		else if (variant == "native-thread") fixture.session->session_id.clear(); // Negative drift ONLY, never positive setup authority.
		else if (variant == "model") fixture.Branch().model_id = "changed-durable-model";
		else if (variant == "tier-explicit") fixture.Branch().service_tier_explicit = true;
		else fixture.Branch().command_safety_tier = "aiReview";
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.branch_id) && std::chrono::steady_clock::now() < deadline)
		{
			(void)uam::FlushPendingChatSaves(fixture.app);
			std::this_thread::yield();
		}
		UAM_ASSERT(!fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.branch_id));
		UAM_ASSERT_EQ(completed->load(), 1);
		(void)uam::acp_detail::SendQueuedPromptIfReady(fixture.app, *fixture.session, fixture.Branch());
		peer.AssertNoTurn();
		UAM_ASSERT(state->cancel.stop_requested() && !fixture.session->last_error.empty());
		UAM_ASSERT(!fixture.session->saved_image_retry && !fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery);
		fixture.Branch().messages[2].attachments = original.messages[2].attachments;
		fixture.Branch().model_id = original.model_id;
		fixture.Branch().service_tier_explicit = original.service_tier_explicit;
		fixture.Branch().command_safety_tier = original.command_safety_tier;
		fixture.session->session_id = session_id;
		(void)uam::acp_detail::SendQueuedPromptIfReady(fixture.app, *fixture.session, fixture.Branch());
		peer.AssertNoTurn();
		UAM_ASSERT_EQ(fixture.session->next_request_id, next_request);
		UAM_ASSERT_EQ(fixture.app.claude_input_tasks.size(), tasks);
		UAM_ASSERT(!fixture.session->saved_image_retry && !fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery);
		fixture.AssertOriginalSource();
	}
}

UAM_TEST(CodexSavedOwnedAndLegacyImagesDeliverOnceWithStrictTextFileRemainder)
{
	CodexOwnedRetryFixtureR75 peer;
	auto& fixture = peer.fixture;
	const auto image_file = fixture.temp.root / "genuine-second-image.gif";
	const auto text_file = fixture.temp.root / "genuine-context.txt";
	const std::string text_bytes = "Genuine existing local text file remainder.";
	std::string second_image;
	UAM_ASSERT(uam::base64::Decode("R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7", second_image));
	UAM_ASSERT(second_image != fixture.expected_original);
	UAM_ASSERT(uam::io::WriteBinaryFile(image_file, second_image));
	UAM_ASSERT(uam::io::WriteTextFile(text_file, text_bytes));
	MessageAttachment image;
	image.id = "real-second-image"; image.name = "Second image retained name"; image.kind = "image";
	image.mime_type = "image/gif"; image.path = image_file.string();
	MessageAttachment document;
	document.id = "real-text-remainder"; document.name = "Context text retained name"; document.kind = "file";
	document.mime_type = "text/plain"; document.path = text_file.string();
	fixture.Branch().messages[2].attachments.push_back(image);
	fixture.Branch().messages[2].attachments.push_back(document);
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.Branch()));
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.branch_id);
	UAM_ASSERT(saved && saved->messages[2].attachments == fixture.Branch().messages[2].attachments);
	peer.RequireRetry();
	const auto turn = peer.DeliveredTurn();
	std::string text;
	std::vector<nlohmann::json> images;
	for (const auto& part : turn["params"]["input"])
	{
		UAM_ASSERT(part.value("type", "") != "localImage");
		if (part.value("type", "") == "image") images.push_back(part);
		else if (part.value("type", "") == "text") text += part.value("text", "");
	}
	UAM_ASSERT_EQ(images.size(), std::size_t{2});
	UAM_ASSERT_EQ(CodexOwnedRetryFixtureR75::DecodeImage(images[0]), fixture.expected_original);
	UAM_ASSERT_EQ(CodexOwnedRetryFixtureR75::DecodeImage(images[1], "image/gif"), second_image);
	const auto first_name = text.find(fixture.Branch().messages[2].attachments[0].name);
	const auto second_name = text.find(image.name);
	UAM_ASSERT(first_name != std::string::npos && second_name != std::string::npos && first_name < second_name);
	UAM_ASSERT(text.find(document.path) != std::string::npos); // The strict nonimage remainder retains its existing prompt-path reference.
	UAM_ASSERT(turn.dump().find(fixture.branch_key) == std::string::npos && turn.dump().find(fixture.source_key) == std::string::npos);
	UAM_ASSERT_EQ(uam_test::ReadFile(image_file), second_image);
	UAM_ASSERT_EQ(uam_test::ReadFile(text_file), text_bytes);
	(void)fixture.BlobPath();
	fixture.AssertOriginalSource();
}
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && UAM_ENABLE_RUNTIME_CLAUDE_CLI && UAM_ENABLE_RUNTIME_CODEX_CLI && defined(__APPLE__)
UAM_TEST(CodexSavedOwnedRetryRevokesIfSavedNativeThreadClearsDuringWorker)
{
	CodexOwnedRetryFixtureR75 peer;
	auto& fixture = peer.fixture;
	peer.RequireRetry();
	const auto state = peer.FinishedImageWorker();
	UAM_ASSERT_EQ(fixture.Branch().native_session_id, peer.native_thread);
	fixture.Branch().native_session_id.clear(); // Negative drift only, following actual matched setup and successful preparation.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (fixture.session->processing && !fixture.session->queued_prompt.empty() && std::chrono::steady_clock::now() < deadline)
	{
		(void)uam::FlushPendingChatSaves(fixture.app);
		(void)uam::PollAllAcpSessions(fixture.app);
		std::this_thread::yield();
	}
	peer.AssertNoTurn();
	UAM_ASSERT(state->cancel.stop_requested());
	UAM_ASSERT(!fixture.session->saved_image_retry && !fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery);
	const int next_request = fixture.session->next_request_id;
	fixture.Branch().native_session_id = peer.native_thread; // Restore the exact parsed identity without granting a new Retry.
	(void)uam::FlushPendingChatSaves(fixture.app);
	(void)uam::PollAllAcpSessions(fixture.app);
	peer.AssertNoTurn();
	UAM_ASSERT_EQ(fixture.session->next_request_id, next_request);
	UAM_ASSERT(!fixture.session->saved_image_retry && !fixture.session->claude_input_preparation);
	fixture.AssertOriginalSource();
}
#endif


#include "common/runtime/structured_framing.h"

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && UAM_ENABLE_RUNTIME_CLAUDE_CLI && UAM_ENABLE_RUNTIME_COPILOT_CLI && defined(__APPLE__)
namespace
{
	struct CopilotOwnedRetryFixtureR77
	{
		// PATH and the peer files outlive the complete AppState, including assertion-failure unwinding.
		uam_test::TempDir peer_root{"uam-private-copilot-image-peer"};
		std::unique_ptr<uam_test::ScopedEnvVar> private_path;
		ClaudeOwnedRetryFixtureR71 fixture;
		struct ObservedGeneration
		{
			std::shared_ptr<std::string> wire = std::make_shared<std::string>();
			std::shared_ptr<uam::platform::AsyncByteWriter> observed, transport;
		};
		std::vector<ObservedGeneration> generations;
		std::string native_session;

		CopilotOwnedRetryFixtureR77()
		{
			UAM_ASSERT(uam::io::WriteTextFile(peer_root.root / "allowed-root", fixture.temp.root.string()));
			const auto executable = peer_root.root / "copilot";
			UAM_ASSERT(uam::io::WriteTextFile(executable, R"PY(#!/usr/bin/python3 -I
import json, os, select, sys, uuid
root = os.path.dirname(os.path.abspath(__file__))
args = sys.argv[1:]
if args[:3] != ["--headless", "--stdio", "--no-auto-update"]:
    raise RuntimeError("Unexpected private SDK base argv: " + repr(args))
tail = args[3:]
if tail:
    if len(tail) != 5 or tail[0] != "--plugin-dir" or tail[2] != "--agent" or tail[4] != "--excluded-tools=task,read_agent,list_agents,write_agent":
        raise RuntimeError("Unexpected private SDK agent argv: " + repr(tail))
    with open(os.path.join(root, "allowed-root"), encoding="utf-8") as stream:
        allowed = os.path.realpath(stream.read())
    plugin = os.path.realpath(tail[1])
    if os.path.commonpath([allowed, plugin]) != allowed or not tail[3].startswith("uam-") or "/" in tail[3]:
        raise RuntimeError("Agent plugin is not fixture-owned")
    for filename in [os.path.join(plugin, "plugin.json"), os.path.join(plugin, "agents", tail[3] + ".agent.md")]:
        if not os.path.isfile(filename):
            raise RuntimeError("Missing actual generated agent file: " + filename)
trace_path = os.path.join(root, "trace-" + str(os.getpid()) + ".json")
registry = os.path.join(root, "native-session")
traffic, held, buffer = [], [], b""
model_lists = 0
model = {"id": "fixture-vision-model", "name": "Private fixture vision model", "capabilities": {"supports": {"vision": True}}}
def record(direction, value, frame=None, waiting=False):
    entry = {"direction": direction, "value": value, "held": waiting}
    if frame is not None:
        entry["frame"] = frame.decode("utf-8")
    traffic.append(entry)
    temporary = trace_path + ".tmp"
    with open(temporary, "w", encoding="utf-8") as stream:
        json.dump(traffic, stream, separators=(",", ":"))
    os.replace(temporary, trace_path)
def respond(value):
    body = json.dumps(value, separators=(",", ":")).encode("utf-8")
    frame = ("Content-Length: " + str(len(body)) + "\r\n\r\n").encode("ascii") + body
    record("response", value, frame)
    sys.stdout.buffer.write(frame)
    sys.stdout.buffer.flush()
def request(value, frame):
    global model_lists
    method = value.get("method", "")
    if method == "models.list":
        model_lists += 1
    waiting = os.path.isfile(os.path.join(root, "image-phase")) and (method == "status.get" or method == "session.model.getCurrent" or (method == "models.list" and model_lists > 1))
    record("request", value, frame, waiting)
    if method == "ping":
        result = {"protocolVersion": 3}
    elif method == "models.list":
        result = {"models": [model]}
    elif method == "status.get":
        result = {"version": "1.0.98", "protocolVersion": 3}
    elif method == "session.model.list":
        result = {"list": [model]}
    elif method == "session.model.getCurrent":
        result = {"modelId": "fixture-vision-model"}
    elif method == "session.create":
        if os.path.exists(registry):
            raise RuntimeError("Unexpected replacement create instead of actual native resume")
        session = str(uuid.uuid4())
        with open(registry, "w", encoding="utf-8") as stream:
            stream.write(session)
        result = {"sessionId": session}
    elif method == "session.resume":
        with open(registry, encoding="utf-8") as stream:
            session = stream.read()
        if value["params"]["sessionId"] != session:
            raise RuntimeError("Resume does not name the actual privately created session")
        result = {"sessionId": session}
    elif method == "session.send":
        return  # Hold the actual native ACK until inspection; no provider/model executes.
    elif method == "session.abort":
        result = {}
    else:
        raise RuntimeError("Unexpected private SDK method: " + method)
    response = {"jsonrpc": "2.0", "id": value["id"], "result": result}
    if waiting:
        held.append(response)
    else:
        respond(response)
while True:
    if held and os.path.isfile(os.path.join(root, "release-images")):
        for response in held:
            respond(response)
        held.clear()
    readable, _, _ = select.select([sys.stdin.buffer], [], [], 0.005)
    if not readable:
        continue
    data = os.read(sys.stdin.fileno(), 65536)
    if not data:
        break
    buffer += data
    while b"\r\n\r\n" in buffer:
        end = buffer.index(b"\r\n\r\n")
        if end > 8192:
            raise RuntimeError("Oversized SDK header")
        headers = buffer[:end].decode("ascii").split("\r\n")
        lengths = [int(line.split(":", 1)[1].strip()) for line in headers if line.lower().startswith("content-length:")]
        if len(lengths) != 1 or not 0 < lengths[0] <= 16 * 1024 * 1024:
            raise RuntimeError("Invalid SDK frame length")
        consumed = end + 4 + lengths[0]
        if len(buffer) < consumed:
            break
        frame, buffer = buffer[:consumed], buffer[consumed:]
        request(json.loads(frame[end + 4:]), frame)
)PY"));
			fs::permissions(executable, fs::perms::owner_all);
			private_path = std::make_unique<uam_test::ScopedEnvVar>("PATH", peer_root.root.string());
			auto& app = fixture.app;
			UAM_ASSERT(!app.provider_model_catalog);
			app.settings.central_provider_configuration = {};
			app.settings.central_provider_configuration.enabled = false;
			app.settings.central_provider_configuration.uam_control_enabled = false;
			app.settings.mcp_servers.clear();
			app.settings.host_scoped_settings.clear();
			app.settings.git_enabled = app.settings.git_auto_worktree = app.settings.svn_enabled = false;
			app.settings.computer_use_globally_enabled = false;
			UAM_ASSERT_EQ(uam::SwitchChatProvider(app, fixture.branch_id, uam::provider_ids::kCopilotCli), uam::ChatProviderSwitchResult::Changed);
			auto& chat = fixture.Branch();
			chat.memory_enabled = chat.uam_control_enabled = chat.computer_use_enabled = false;
			chat.model_id = "fixture-vision-model";
			chat.reasoning_effort.clear();
			chat.service_tier.clear();
			chat.service_tier_explicit = false;
			chat.approval_mode = "default";
			chat.command_safety_tier = "off";
			UAM_ASSERT(chat.native_session_id.empty());
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
			const auto reloaded = ChatRepository::LoadLocalChat(app.data_root, fixture.branch_id);
			UAM_ASSERT(reloaded && reloaded->provider_id == uam::provider_ids::kCopilotCli && reloaded->native_session_id.empty());
			UAM_ASSERT_EQ(reloaded->messages[2].attachments.front().native_asset_key, fixture.branch_key);
			chat = *reloaded;
			std::string error;
			const bool started = uam::StartAcpModelDiscovery(app, fixture.branch_id, &error, false);
			fixture.session = uam::FindAcpSessionForChat(app, fixture.branch_id);
			if (!started || !fixture.session) throw uam_test::TestFailure("Private Copilot setup did not start: " + error);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while ((!fixture.session->session_ready || !fixture.session->pending_request_methods.empty()) && std::chrono::steady_clock::now() < deadline) Tick();
			UAM_ASSERT(fixture.session->running && fixture.session->initialized && fixture.session->session_ready && !fixture.session->processing);
			UAM_ASSERT(!fixture.session->model_discovery_only && fixture.session->pending_request_methods.empty() && fixture.session->last_error.empty());
			const auto traffic = Trace();
			int creates = 0;
			for (const auto& item : traffic)
			{
				if (item["direction"] != "request" || item["value"].value("method", "") != "session.create") continue;
				++creates;
				UAM_ASSERT(!item["value"]["params"].contains("sessionId"));
				for (const auto& reply : traffic)
					if (reply["pid"] == item["pid"] && reply["direction"] == "response" && reply["value"]["id"] == item["value"]["id"])
						native_session = reply["value"]["result"]["sessionId"].get<std::string>();
			}
			UAM_ASSERT_EQ(creates, 1);
			UAM_ASSERT(!native_session.empty());
			UAM_ASSERT_EQ(fixture.session->session_id, native_session);
			UAM_ASSERT_EQ(chat.native_session_id, native_session);
			const auto durable = ChatRepository::LoadLocalChat(app.data_root, fixture.branch_id);
			UAM_ASSERT(durable && durable->native_session_id == native_session);
			UAM_ASSERT_EQ(fixture.session->current_model_id, chat.model_id);
			UAM_ASSERT(Sends().empty());
			fixture.AssertOriginalSource();
		}

		~CopilotOwnedRetryFixtureR77()
		{
			if (!fixture.session) fixture.session = uam::FindAcpSessionForChat(fixture.app, fixture.branch_id);
			if (fixture.session) (void)uam::StopAcpSession(fixture.app, fixture.branch_id);
			for (auto& generation : generations)
			{
				generation.observed->Stop();
				generation.transport->Stop();
			}
			fixture.CloseSink();
			for (auto& task : fixture.app.acp_process_stop_tasks)
				if (task.worker && task.worker->joinable()) task.worker->join();
			fixture.app.acp_process_stop_tasks.clear();
			for (auto& task : fixture.app.claude_input_tasks)
			{
				if (task.state) task.state->cancel.request_stop();
				if (task.worker && task.worker->joinable()) task.worker->join();
			}
		}

		void ObserveWriter()
		{
			fixture.session = uam::FindAcpSessionForChat(fixture.app, fixture.branch_id);
			if (!fixture.session || !fixture.session->stdin_writer) return;
			for (const auto& generation : generations)
				if (generation.observed == fixture.session->stdin_writer) return;
			ObservedGeneration generation;
			generation.transport = fixture.session->stdin_writer;
			generation.observed = std::make_shared<uam::platform::AsyncByteWriter>(
			    [capture = generation.wire, writer = generation.transport](const char* bytes, std::size_t count, std::string& failure) -> std::ptrdiff_t
			    {
				    capture->append(bytes, count);
				    return writer->Enqueue(bytes, count, &failure) ? static_cast<std::ptrdiff_t>(count) : -1;
			    }, [] {});
			fixture.session->stdin_writer = generation.observed;
			generations.push_back(std::move(generation));
		}

		void Tick()
		{
			UAM_ASSERT(std::string(std::getenv("PATH")) == peer_root.root.string());
			UAM_ASSERT((fs::status(peer_root.root / "copilot").permissions() & fs::perms::owner_exec) != fs::perms::none);
			ObserveWriter();
			(void)uam::FlushPendingChatSaves(fixture.app);
			(void)uam::PollAllAcpSessions(fixture.app);
			ObserveWriter();
			std::this_thread::yield();
		}

		nlohmann::json Trace() const
		{
			nlohmann::json result = nlohmann::json::array();
			for (const auto& path : fs::directory_iterator(peer_root.root))
			{
				const std::string name = path.path().filename().string();
				if (!name.starts_with("trace-") || !name.ends_with(".json")) continue;
				const auto traffic = nlohmann::json::parse(uam_test::ReadFile(path.path()), nullptr, false);
				UAM_ASSERT(traffic.is_array());
				for (auto entry : traffic) { entry["pid"] = name; result.push_back(std::move(entry)); }
			}
			return result;
		}

		std::vector<nlohmann::json> Sends() const
		{
			std::vector<nlohmann::json> result;
			for (const auto& entry : Trace())
				if (entry["direction"] == "request" && entry["value"].value("method", "") == "session.send") result.push_back(entry["value"]);
			return result; // Includes any stripped text-only fallback.
		}

		std::vector<nlohmann::json> ObservedSends()
		{
			std::vector<nlohmann::json> result;
			for (auto& generation : generations)
			{
				if (generation.observed == fixture.session->stdin_writer)
					UAM_ASSERT(generation.observed->Flush(std::chrono::seconds(1)) && generation.transport->Flush(std::chrono::seconds(1)));
				else
				{
					generation.observed->Stop(); // Join a retired observer before reading its captured bytes.
					generation.transport->Stop();
				}
				std::string_view remaining(*generation.wire);
				while (!remaining.empty())
				{
					const auto frame = uam::structured_framing::Read(remaining);
					UAM_ASSERT_EQ(frame.status, uam::structured_framing::Status::Complete);
					const auto value = nlohmann::json::parse(remaining.substr(frame.body_offset, frame.body_bytes));
					if (value.value("method", "") == "session.send") result.push_back(value);
					remaining.remove_prefix(frame.consumed_bytes);
				}
			}
			return result;
		}
	};
} // namespace

UAM_TEST(CopilotSavedBranchRetryDeliversOwnedPngThroughParsedSdkAndBoundedWorker)
{
	CopilotOwnedRetryFixtureR77 peer;
	auto& fixture = peer.fixture;
	UAM_ASSERT(uam::io::WriteTextFile(peer.peer_root.root / "image-phase", "hold actual image evidence"));
	std::string error;
	const bool accepted = fixture.BeginRetry(error);
	if (!accepted)
	{
		std::cerr << "Copilot Retry startup refusal: " << error << std::endl;
		UAM_ASSERT_EQ(error, std::string("Recorded image Retry requires valid local originals and a supported provider. Remove unsupported sources or attach files again."));
		throw uam_test::TestFailure("Copilot accepted-Retry prerequisite after parsed SDK setup and durable reload: " + error); // Intended current-source RED.
	}
	peer.ObserveWriter();
	UAM_ASSERT(error.empty() && fixture.session->saved_image_retry);
	const auto permission = fixture.session->saved_image_retry;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	std::set<std::string> held;
	while (held.size() != 3 && std::chrono::steady_clock::now() < deadline)
	{
		peer.Tick();
		for (const auto& entry : peer.Trace())
			if (entry["direction"] == "request" && entry.value("held", false)) held.insert(entry["value"]["method"].get<std::string>());
		UAM_ASSERT(peer.Sends().empty());
		UAM_ASSERT(!fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery);
	}
	UAM_ASSERT_EQ(held, (std::set<std::string>{"status.get", "models.list", "session.model.getCurrent"}));
	UAM_ASSERT_EQ(fixture.session->saved_image_retry, permission);
	UAM_ASSERT(uam::io::WriteTextFile(peer.peer_root.root / "release-images", "release actual matched responses"));
	const auto delivery_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (peer.Sends().empty() && std::chrono::steady_clock::now() < delivery_deadline) peer.Tick();
	const auto sends = peer.Sends();
	UAM_ASSERT_EQ(sends.size(), std::size_t{1});
	UAM_ASSERT_EQ(peer.ObservedSends(), sends);
	const auto& send = sends.front();
	UAM_ASSERT_EQ(send["params"]["sessionId"], nlohmann::json(peer.native_session));
	UAM_ASSERT_EQ(send["id"], nlohmann::json(fixture.session->prompt_request_id));
	UAM_ASSERT_EQ(fixture.session->pending_request_methods.at(fixture.session->prompt_request_id), std::string("session.send"));
	UAM_ASSERT_EQ(send["params"]["mode"], nlohmann::json("enqueue"));
	UAM_ASSERT_EQ(send["params"]["agentMode"], nlohmann::json("interactive"));
	const auto& attachments = send["params"]["attachments"];
	UAM_ASSERT(attachments.is_array() && attachments.size() == 1);
	const auto& image = attachments.front();
	UAM_ASSERT_EQ(image["type"], nlohmann::json("blob"));
	UAM_ASSERT_EQ(image["mimeType"], nlohmann::json("image/png"));
	UAM_ASSERT_EQ(image["displayName"], nlohmann::json(fixture.Branch().messages[2].attachments.front().name));
	std::string decoded;
	UAM_ASSERT(uam::base64::Decode(image["data"].get<std::string>(), decoded));
	UAM_ASSERT_EQ(decoded, fixture.expected_original);
	const auto prompt = send["params"]["prompt"].get<std::string>();
	UAM_ASSERT(prompt.find(fixture.Branch().messages[2].attachments.front().name) != std::string::npos);
	UAM_ASSERT(send.dump().find(fixture.branch_key) == std::string::npos);
	UAM_ASSERT(!image.contains("path") && !image.contains("url"));
	UAM_ASSERT(fixture.session->processing && fixture.session->queued_prompt.empty() && fixture.session->last_error.empty());
	UAM_ASSERT_EQ(fixture.session->session_id, peer.native_session);
	const auto durable = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.branch_id);
	UAM_ASSERT(durable && durable->native_session_id == peer.native_session);
	UAM_ASSERT_EQ(durable->messages[2].attachments.front().native_asset_key, fixture.branch_key);
	(void)fixture.BlobPath();
	fixture.AssertOriginalSource();
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && UAM_ENABLE_RUNTIME_CLAUDE_CLI && UAM_ENABLE_RUNTIME_COPILOT_CLI && defined(__APPLE__)
namespace
{
	struct CopilotImageSupplementR77 : CopilotOwnedRetryFixtureR77
	{
		std::string replies;
		int parsed_events = 0;
		std::set<int> parsed_reply_ids;
		nlohmann::json control = nlohmann::json::object();

		explicit CopilotImageSupplementR77(const std::string& variant = "healthy")
		{
			// Amend only this private peer, then publicly Stop its already-proved warm process.
			// The next Retry must genuinely resume through the changed executable and parsed callbacks.
			const auto executable = peer_root.root / "copilot";
			std::string script = uam_test::ReadFile(executable);
			const auto replace = [&script](const std::string& before, const std::string& after)
			{
				const auto position = script.find(before);
				UAM_ASSERT(position != std::string::npos && script.find(before, position + 1) == std::string::npos);
				script.replace(position, before.size(), after);
			};
			replace("def request(value, frame):", R"PY(def settings():
    path = os.path.join(root, "control.json")
    if not os.path.isfile(path):
        return {}
    with open(path, encoding="utf-8") as stream:
        return json.load(stream)
def request(value, frame):)PY");
			replace(R"FIND(    response = {"jsonrpc": "2.0", "id": value["id"], "result": result})FIND", R"PY(    option = settings().get("variant", "healthy")
    result = json.loads(json.dumps(result))
    if method == "status.get":
        if option == "old-version": result["version"] = "1.0.59"
        if option == "malformed-version": result["version"] = "not-a-version"
        if option == "wrong-protocol": result["protocolVersion"] = 2
    if waiting and method == "session.model.getCurrent" and option == "different-model":
        result["modelId"] = "different-native-model"
    if waiting and method == "models.list":
        selected = result["models"][0]
        if option == "vision-false": selected["capabilities"]["supports"]["vision"] = False
        if option == "vision-missing": del selected["capabilities"]["supports"]["vision"]
        if option == "vision-string": selected["capabilities"]["supports"]["vision"] = "true"
        if option == "disabled-policy": selected["policy"] = {"state": "disabled", "terms": "Private test restriction"}
        if option in ["mime-restricted", "count-restricted", "size-restricted", "malformed-limits", "valid-limits"]:
            limits = {"supported_media_types": ["image/png"], "max_prompt_images": 1, "max_prompt_image_size": 1048576}
            if option == "mime-restricted": limits["supported_media_types"] = ["image/jpeg"]
            if option == "count-restricted": limits["max_prompt_images"] = 0
            if option == "size-restricted": limits["max_prompt_image_size"] = 1
            if option == "malformed-limits": limits["max_prompt_images"] = "1"
            selected["capabilities"]["limits"] = {"vision": limits}
    response = {"jsonrpc": "2.0", "id": value["id"], "result": result}
    if method == "status.get" and option == "rpc-error":
        response = {"jsonrpc": "2.0", "id": value["id"], "error": {"code": -32601, "message": "Private status unsupported"}})PY");
			replace("while True:\n    if held", R"PY(event_sent = False
while True:
    event_path = os.path.join(root, "emit-event.json")
    if not event_sent and os.path.isfile(event_path):
        with open(event_path, encoding="utf-8") as stream:
            event = json.load(stream)
        with open(registry, encoding="utf-8") as stream:
            actual_session = stream.read()
        native_event = {"type": "session.model_change", "id": "private-actual-model-event", "data": {"autoTier": None}}
        if event.get("child", False): native_event["agentId"] = "private-child"
        respond({"jsonrpc": "2.0", "method": "session.event", "params": {"sessionId": actual_session, "event": native_event}})
        event_sent = True
    if held)PY");
			replace("        for response in held:\n            respond(response)\n        held.clear()", R"PY(        remaining = []
        for response in held:
            original = next(item["value"] for item in traffic if item["direction"] == "request" and item["value"].get("id") == response.get("id"))
            if settings().get("hold_status", False) and original.get("method") == "status.get":
                remaining.append(response)
            else:
                respond(response)
        held = remaining)PY");
			UAM_ASSERT(uam::io::WriteTextFile(executable, script));
			fs::permissions(executable, fs::perms::owner_all);
			UAM_ASSERT((fs::status(executable).permissions() & fs::perms::owner_exec) != fs::perms::none);
			control["variant"] = variant;
			WriteControl();
			UAM_ASSERT(uam::StopAcpSession(fixture.app, fixture.branch_id));
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while ((!fixture.app.acp_process_stop_tasks.empty() || fixture.session->local_stop_pending) && std::chrono::steady_clock::now() < deadline) Tick();
			UAM_ASSERT(!fixture.session->running && !fixture.session->local_stop_pending && fixture.app.acp_process_stop_tasks.empty());
			UAM_ASSERT_EQ(fixture.Branch().native_session_id, native_session);
		}

		void WriteControl() { UAM_ASSERT(uam::io::WriteTextFile(peer_root.root / "control.json", control.dump())); }
		void RequireRetry()
		{
			UAM_ASSERT(uam::io::WriteTextFile(peer_root.root / "image-phase", "hold actual evidence"));
			std::string error;
			if (!fixture.BeginRetry(error)) throw uam_test::TestFailure("Copilot saved-image feature prerequisite after genuine cold-process resume setup: " + error);
			ObserveWriter();
			UAM_ASSERT(error.empty() && fixture.session->saved_image_retry);
		}
		std::size_t HeldRequestCount() const
		{
			std::size_t count = 0;
			for (const auto& entry : Trace()) if (entry["direction"] == "request" && entry.value("held", false)) ++count;
			return count;
		}
		void WaitHeld()
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			std::set<std::string> methods;
			while (methods.size() != 3 && std::chrono::steady_clock::now() < deadline)
			{
				Tick();
				for (const auto& entry : Trace())
					if (entry["direction"] == "request" && entry.value("held", false)) methods.insert(entry["value"]["method"].get<std::string>());
				UAM_ASSERT(Sends().empty() && !fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery);
			}
			UAM_ASSERT_EQ(methods, (std::set<std::string>{"status.get", "models.list", "session.model.getCurrent"}));
			UAM_ASSERT_EQ(HeldRequestCount(), std::size_t{3});
			UAM_ASSERT(fixture.app.claude_input_tasks.empty());
		}
		void Release() { UAM_ASSERT(uam::io::WriteTextFile(peer_root.root / "release-images", "release genuine matching replies")); }
		void ReadRepliesOnly()
		{
			char bytes[65536];
			for (;;)
			{
				const auto count = PlatformServicesFactory::Instance().process_service.ReadStdioProcessStdout(*fixture.session, bytes, sizeof(bytes));
				if (count <= 0) break;
				replies.append(bytes, static_cast<std::size_t>(count));
			}
			for (;;)
			{
				const auto frame = uam::structured_framing::Read(replies);
				UAM_ASSERT(frame.status != uam::structured_framing::Status::Invalid);
				if (frame.status != uam::structured_framing::Status::Complete) break;
				const std::string body = replies.substr(frame.body_offset, frame.body_bytes);
				const auto message = nlohmann::json::parse(body);
				(void)uam::ProcessAcpLineForTests(fixture.app, *fixture.session, fixture.Branch(), body); // Actual private process output, never invented positive state.
				if (message.value("method", "") == "session.event") ++parsed_events;
				if (message.contains("id") && message["id"].is_number_integer()) parsed_reply_ids.insert(message["id"].get<int>());
				replies.erase(0, frame.consumed_bytes);
			}
		}
		std::shared_ptr<uam::ClaudeInputPreparation> FinishedWorker()
		{
			WaitHeld();
			Release();
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (!fixture.session->claude_input_preparation && std::chrono::steady_clock::now() < deadline)
			{
				ReadRepliesOnly();
				(void)uam::FlushPendingChatSaves(fixture.app);
				(void)uam::acp_detail::SendQueuedPromptIfReady(fixture.app, *fixture.session, fixture.Branch());
				std::this_thread::yield();
			}
			UAM_ASSERT(fixture.session->claude_input_preparation);
			const auto state = fixture.session->claude_input_preparation->state;
			while (!state->finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
			UAM_ASSERT(state->finished.load(std::memory_order_acquire) && state->error.empty() && state->message.is_object());
			UAM_ASSERT(state->message.contains("message") && state->message["message"].contains("content") && state->message["message"]["content"].is_array());
			UAM_ASSERT_EQ(std::ranges::count_if(state->message["message"]["content"], [](const nlohmann::json& part) { return part.is_object() && part.value("type", "") == "image"; }), std::ptrdiff_t{1});
			UAM_ASSERT(Sends().empty() && !fixture.session->pending_prompt_delivery);
			return state;
		}
		nlohmann::json Delivered()
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (Sends().empty() && std::chrono::steady_clock::now() < deadline) Tick();
			const auto sends = Sends();
			UAM_ASSERT_EQ(sends.size(), std::size_t{1});
			UAM_ASSERT_EQ(ObservedSends(), sends);
			UAM_ASSERT_EQ(sends.front()["params"]["sessionId"], nlohmann::json(native_session));
			UAM_ASSERT_EQ(sends.front()["id"], nlohmann::json(fixture.session->prompt_request_id));
			UAM_ASSERT_EQ(fixture.session->pending_request_methods.at(fixture.session->prompt_request_id), std::string("session.send"));
			return sends.front();
		}
		void Reject(std::chrono::seconds bound = std::chrono::seconds(5))
		{
			const auto deadline = std::chrono::steady_clock::now() + bound;
			while (fixture.session->saved_image_retry && std::chrono::steady_clock::now() < deadline) { Tick(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
			UAM_ASSERT(!fixture.session->saved_image_retry && !fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery);
			UAM_ASSERT(!fixture.session->last_error.empty() && Sends().empty());
		}
		void EmitActualEvent(bool child)
		{
			UAM_ASSERT(uam::io::WriteTextFile(peer_root.root / "emit-event.json", nlohmann::json{{"child", child}}.dump()));
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (parsed_events == 0 && std::chrono::steady_clock::now() < deadline) { ReadRepliesOnly(); std::this_thread::yield(); }
			UAM_ASSERT_EQ(parsed_events, 1);
		}
		static std::string DecodeBlob(const nlohmann::json& image, const std::string& mime, const std::string& name)
		{
			UAM_ASSERT_EQ(image["type"], nlohmann::json("blob"));
			UAM_ASSERT_EQ(image["mimeType"], nlohmann::json(mime));
			UAM_ASSERT_EQ(image["displayName"], nlohmann::json(name));
			UAM_ASSERT(!image.contains("path") && !image.contains("url"));
			std::string bytes;
			UAM_ASSERT(uam::base64::Decode(image["data"].get<std::string>(), bytes));
			return bytes;
		}
	};
} // namespace

UAM_TEST(CopilotSavedRetryColdResumeAndOrdinaryTextKeepNativeSetupContracts)
{
	for (const bool image : {true, false})
	{
		CopilotImageSupplementR77 peer(image ? "healthy" : "rpc-error");
		auto& fixture = peer.fixture;
		UAM_ASSERT(!fixture.session->running && !fixture.Branch().native_session_id.empty()); // Cold PROCESS resume, not first-ever session creation.
		if (image)
		{
			peer.RequireRetry();
			const auto permission = fixture.session->saved_image_retry;
			peer.WaitHeld();
			UAM_ASSERT_EQ(fixture.session->saved_image_retry, permission);
			peer.Release();
			const auto send = peer.Delivered();
			UAM_ASSERT_EQ(send["params"]["attachments"].size(), std::size_t{1});
			UAM_ASSERT_EQ(CopilotImageSupplementR77::DecodeBlob(send["params"]["attachments"][0], "image/png", fixture.Branch().messages[2].attachments[0].name), fixture.expected_original);
		}
		else
		{
			fixture.Branch().messages[2].attachments.clear();
			UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.Branch()));
			std::string error;
			UAM_ASSERT(fixture.BeginRetry(error) && error.empty());
			const auto send = peer.Delivered();
			UAM_ASSERT(send["params"]["prompt"].get<std::string>().find(fixture.Branch().messages[2].content) != std::string::npos);
			UAM_ASSERT(!fixture.session->saved_image_retry && !fixture.session->claude_input_preparation && fixture.app.claude_input_tasks.empty());
			for (const auto& entry : peer.Trace()) UAM_ASSERT(entry["direction"] != "request" || entry["value"].value("method", "") != "status.get");
		}
		int resumes = 0;
		for (const auto& entry : peer.Trace())
			if (entry["direction"] == "request" && entry["value"].value("method", "") == "session.resume") { ++resumes; UAM_ASSERT_EQ(entry["value"]["params"]["sessionId"], nlohmann::json(peer.native_session)); }
		UAM_ASSERT(resumes >= 1);
		const auto durable = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.branch_id);
		UAM_ASSERT(durable && durable->native_session_id == peer.native_session);
		fixture.AssertOriginalSource();
	}
}

UAM_TEST(CopilotSavedOwnedRetryRequiresNativeEvidenceAndRetiresActualTimeout)
{
	for (const std::string variant : {"old-version", "malformed-version", "wrong-protocol", "rpc-error", "vision-false", "vision-missing", "vision-string", "different-model", "disabled-policy", "mime-restricted", "count-restricted", "size-restricted", "malformed-limits", "valid-limits", "timeout"})
	{
		CopilotImageSupplementR77 peer(variant);
		peer.control["hold_status"] = variant == "timeout";
		peer.WriteControl();
		const auto attempt_started = std::chrono::steady_clock::now();
		peer.RequireRetry();
		peer.WaitHeld();
		const auto queries = peer.HeldRequestCount();
		peer.Release();
		if (variant == "valid-limits")
		{
			const auto send = peer.Delivered();
			UAM_ASSERT_EQ(send["params"]["attachments"].size(), std::size_t{1});
			const auto bytes = CopilotImageSupplementR77::DecodeBlob(send["params"]["attachments"][0], "image/png", peer.fixture.Branch().messages[2].attachments[0].name);
			UAM_ASSERT_EQ(bytes, peer.fixture.expected_original);
			UAM_ASSERT(bytes.size() <= 1048576); // Actual outgoing decoded bytes/MIME, not base64 length or declared size alone.
		}
		else
		{
			peer.Reject(variant == "timeout" ? std::chrono::seconds(35) : std::chrono::seconds(5));
			if (variant == "timeout")
			{
				UAM_ASSERT(std::chrono::steady_clock::now() - attempt_started >= std::chrono::seconds(30)); // Unchanged real 30s query deadline; no private deadline/clock mutation.
				peer.control["hold_status"] = false;
				peer.WriteControl(); // Release the ACTUAL expired response; never invent an ID or pending request.
				int expired_status_id = 0;
				for (const auto& entry : peer.Trace())
					if (entry["direction"] == "request" && entry.value("held", false) && entry["value"].value("method", "") == "status.get") expired_status_id = entry["value"]["id"].get<int>();
				UAM_ASSERT(expired_status_id > 0 && !peer.fixture.session->pending_request_methods.contains(expired_status_id));
				const auto late_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
				while (!peer.parsed_reply_ids.contains(expired_status_id) && std::chrono::steady_clock::now() < late_deadline) { peer.ReadRepliesOnly(); std::this_thread::yield(); }
				UAM_ASSERT(peer.parsed_reply_ids.contains(expired_status_id));
				UAM_ASSERT(!peer.fixture.session->saved_image_retry && !peer.fixture.session->claude_input_preparation && peer.Sends().empty());
			}
			for (int poll = 0; poll < 3; ++poll) peer.Tick();
			UAM_ASSERT_EQ(peer.HeldRequestCount(), queries); // No automatic replacement queries or replay.
			UAM_ASSERT(peer.fixture.app.claude_input_tasks.empty());
		}
		peer.fixture.AssertOriginalSource();
	}
}

UAM_TEST(CopilotSavedOwnedAndLegacyImagesPreserveExactBytesNamesAndOrder)
{
	CopilotImageSupplementR77 peer;
	auto& fixture = peer.fixture;
	const std::string repeated_name = "Repeated \"image\" name";
	fixture.Branch().messages[2].attachments[0].name = repeated_name;
	auto pending = BranchPendingOriginal();
	pending.id = "second-real-owned-gif"; pending.name = repeated_name;
	pending.native_part_key += ":second-gif";
	std::string error, gif;
	const std::string encoded_gif = "R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7";
	UAM_ASSERT(uam::base64::Decode(encoded_gif, gif));
	pending.native_inline_payload = uam::native_assets::MakeInlinePayload(std::string(encoded_gif), "image/gif", std::make_shared<uam::native_assets::PayloadBudget>(), error);
	UAM_ASSERT(pending.native_inline_payload && error.empty());
	pending.mime_type = pending.native_inline_payload->MimeType(); pending.content_digest = pending.native_inline_payload->Digest(); pending.size_bytes = pending.native_inline_payload->DecodedSize();
	auto detached = fixture.Branch();
	detached.messages[2].attachments.push_back(pending);
	const auto revision = ChatRepository::ChatStorageRevision(fixture.app.data_root, fixture.branch_id);
	UAM_ASSERT(revision);
	auto prepared = uam::PreparedNativeMediaChatSave::Prepare(fixture.app.data_root, detached, *revision, error);
	UAM_ASSERT(prepared && error.empty() && prepared->Publish(error, [] { return true; }) && prepared->PrimaryAcknowledged());
	UAM_ASSERT(prepared->AcknowledgeOriginals(error) && error.empty());
	auto committed = prepared->TakeCommittedChat();
	UAM_ASSERT(committed);
	fixture.Branch() = *committed;
	const auto owned_gif = fixture.Branch().messages[2].attachments[1];
	UAM_ASSERT(uam::native_assets::IsKeyForOwner(fixture.branch_id, owned_gif.native_asset_key));
	const auto original_gif = uam::native_assets::ReadOwned(fixture.app.data_root, fixture.branch_id, owned_gif.native_asset_key);
	UAM_ASSERT(original_gif.ok && original_gif.bytes == gif);
	std::string jpeg;
	UAM_ASSERT(uam::base64::Decode("/9j/4AAQSkZJRgABAQAAAQABAAD/4QAiRXhpZgAATU0AKgAAAAgAAQESAAMAAAABAAYAAAAAAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/2wBDAQMEBAUEBQkFBQkUDQsNFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/wAARCAACAAMDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDxKiiivyo/0jP/2Q==", jpeg)); // Existing real EXIF JPEG fixture, small enough to preserve original bytes.
	const auto jpeg_path = fixture.temp.root / "genuine-third-image.jpg";
	const auto text_path = fixture.temp.root / "genuine-context.txt";
	const std::string text_bytes = "Real clean Legacy text remainder.";
	UAM_ASSERT(uam::io::WriteBinaryFile(jpeg_path, jpeg) && uam::io::WriteTextFile(text_path, text_bytes));
	MessageAttachment legacy_image; legacy_image.id = "legacy-jpeg"; legacy_image.kind = "image"; legacy_image.mime_type = "image/jpeg"; legacy_image.name = "Third native image"; legacy_image.path = jpeg_path.string();
	MessageAttachment legacy_text; legacy_text.id = "legacy-text"; legacy_text.kind = "file"; legacy_text.mime_type = "text/plain"; legacy_text.name = "Clean native file"; legacy_text.path = text_path.string();
	fixture.Branch().messages[2].attachments.push_back(legacy_image);
	fixture.Branch().messages[2].attachments.push_back(legacy_text);
	UAM_ASSERT(ChatRepository::SaveChat(fixture.app.data_root, fixture.Branch()));
	const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.branch_id);
	UAM_ASSERT(saved && saved->messages[2].attachments == fixture.Branch().messages[2].attachments);
	fixture.Branch() = *saved;
	const auto declarations = fixture.Branch().messages[2].attachments;
	peer.RequireRetry(); peer.WaitHeld(); peer.Release();
	const auto send = peer.Delivered();
	const auto& parts = send["params"]["attachments"];
	UAM_ASSERT(parts.is_array() && parts.size() == 4);
	UAM_ASSERT_EQ(CopilotImageSupplementR77::DecodeBlob(parts[0], "image/png", repeated_name), fixture.expected_original);
	UAM_ASSERT_EQ(CopilotImageSupplementR77::DecodeBlob(parts[1], "image/gif", repeated_name), gif);
	UAM_ASSERT_EQ(CopilotImageSupplementR77::DecodeBlob(parts[2], "image/jpeg", legacy_image.name), jpeg);
	UAM_ASSERT_EQ(parts[3]["type"], nlohmann::json("file"));
	UAM_ASSERT_EQ(parts[3]["path"], nlohmann::json(text_path.string()));
	UAM_ASSERT_EQ(parts[3]["displayName"], nlohmann::json(legacy_text.name));
	const auto prompt = send["params"]["prompt"].get<std::string>();
	const auto first = prompt.find("Image 1 (native image-block order): " + nlohmann::json(repeated_name).dump());
	const auto second = prompt.find("Image 2 (native image-block order): " + nlohmann::json(repeated_name).dump());
	const auto third = prompt.find("Image 3 (native image-block order): " + nlohmann::json(legacy_image.name).dump());
	UAM_ASSERT(first != std::string::npos && second != std::string::npos && third != std::string::npos && first < second && second < third);
	UAM_ASSERT(send.dump().find(fixture.branch_key) == std::string::npos && send.dump().find(owned_gif.native_asset_key) == std::string::npos);
	UAM_ASSERT_EQ(fixture.Branch().messages[2].attachments, declarations);
	UAM_ASSERT_EQ(uam_test::ReadFile(jpeg_path), jpeg); UAM_ASSERT_EQ(uam_test::ReadFile(text_path), text_bytes);
	UAM_ASSERT_EQ(uam::native_assets::ReadOwned(fixture.app.data_root, fixture.branch_id, owned_gif.native_asset_key).bytes, gif);
	fixture.AssertOriginalSource();
}

UAM_TEST(CopilotSavedOwnedRetryRejectsFinishedWorkerAndActualDurableEventDrift)
{
	for (const std::string variant : {"root", "incarnation", "vector", "name", "model", "stop", "cancel", "durable-root-event", "durable-child-control"})
	{
		CopilotImageSupplementR77 peer;
		auto& fixture = peer.fixture;
		peer.RequireRetry();
		const auto state = peer.FinishedWorker(); // Honest old-source feature prerequisite; never call an unavailable worker stale-result proof.
		const auto original = fixture.Branch();
		const auto root = fixture.app.data_root;
		const auto tasks = fixture.app.claude_input_tasks.size();
		const auto queries = peer.HeldRequestCount();
		if (variant.starts_with("durable-"))
		{
			UAM_ASSERT(fixture.session->defer_prompt_save && !fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.branch_id));
			UAM_ASSERT(uam::acp_detail::SendQueuedPromptIfReady(fixture.app, *fixture.session, fixture.Branch()));
			const auto packet = fixture.session->pending_prompt_delivery;
			UAM_ASSERT(packet && fixture.session->claude_input_preparation && fixture.session->claude_input_preparation->state == state && peer.Sends().empty());
			const auto completions = std::make_shared<std::atomic<int>>(0);
			UAM_ASSERT(uam::DeferChatSaveResult(fixture.app, fixture.branch_id, [completions](bool) { ++*completions; }));
			UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.branch_id));
			peer.EmitActualEvent(variant == "durable-child-control");
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.branch_id) && std::chrono::steady_clock::now() < deadline) { (void)uam::FlushPendingChatSaves(fixture.app); std::this_thread::yield(); }
			UAM_ASSERT_EQ(completions->load(), 1);
			if (variant == "durable-child-control")
			{
				const auto send = peer.Delivered();
				UAM_ASSERT_EQ(CopilotImageSupplementR77::DecodeBlob(send["params"]["attachments"][0], "image/png", original.messages[2].attachments[0].name), fixture.expected_original);
				fixture.AssertOriginalSource();
				continue;
			}
		}
		else if (variant == "root") { fixture.app.data_root = fixture.temp.root / "changed-scope"; fs::create_directories(fixture.app.data_root); }
		else if (variant == "incarnation") fixture.Branch().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
		else if (variant == "vector")
		{
			const auto path = fixture.temp.root / "late-real-file.txt";
			UAM_ASSERT(uam::io::WriteTextFile(path, "Real newly declared context after image worker completion."));
			MessageAttachment file; file.id = "late-file"; file.name = "Late file"; file.kind = "file"; file.mime_type = "text/plain"; file.path = path.string();
			fixture.Branch().messages[2].attachments.push_back(file);
		}
		else if (variant == "name") fixture.Branch().messages[2].attachments[0].name += " changed after worker";
		else if (variant == "model") fixture.Branch().model_id = "changed-explicit-model";
		else if (variant == "stop") (void)uam::StopAcpSession(fixture.app, fixture.branch_id);
		else { std::string error; (void)uam::CancelAcpTurn(fixture.app, fixture.branch_id, &error); }
		if (variant != "stop") (void)uam::acp_detail::SendQueuedPromptIfReady(fixture.app, *fixture.session, fixture.Branch());
		UAM_ASSERT(peer.Sends().empty() && state->cancel.stop_requested());
		UAM_ASSERT(!fixture.session->saved_image_retry && !fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery);
		fixture.app.data_root = root;
		fixture.Branch().metadata_incarnation = original.metadata_incarnation;
		fixture.Branch().messages[2].attachments = original.messages[2].attachments;
		fixture.Branch().model_id = original.model_id;
		for (int poll = 0; poll < 3; ++poll)
		{
			peer.Tick();
			// Normal Poll reaps finished tasks; any replacement must still be rejected by identity.
			UAM_ASSERT(fixture.app.claude_input_tasks.size() <= tasks);
			for (const auto& task : fixture.app.claude_input_tasks) UAM_ASSERT(task.state == state);
			UAM_ASSERT(peer.Sends().empty() && peer.HeldRequestCount() == queries);
			UAM_ASSERT(!fixture.session->saved_image_retry && !fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery);
		}

		UAM_ASSERT(!fixture.session->saved_image_retry && !fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery);
		fixture.AssertOriginalSource();
	}
}

UAM_TEST(CopilotOwnedInputKeepsStrictDirectAndInvalidCompleteSourceRefusal)
{
	for (const std::string variant : {"direct-owned", "direct-legacy", "corrupt", "missing", "foreign", "mixed", "uri", "pending", "nonimage", "mime"})
	{
		CopilotImageSupplementR77 peer;
		auto& fixture = peer.fixture;
		const auto blob = fixture.BlobPath();
		const auto original = fixture.Branch().messages[2].attachments;
		auto& image = fixture.Branch().messages[2].attachments[0];
		const auto sentinel = fixture.temp.root / "real-native-default.png";
		if (variant.starts_with("direct-"))
		{
			auto attachment = image;
			if (variant == "direct-legacy")
			{
				UAM_ASSERT(uam::io::WriteBinaryFile(sentinel, fixture.expected_original));
				attachment = {}; attachment.id = "genuine-default-image"; attachment.kind = "image"; attachment.name = "Genuine default image"; attachment.mime_type = "image/png"; attachment.path = sentinel.string();
			}
			std::string error;
			const bool accepted = uam::SendAcpPrompt(fixture.app, fixture.branch_id, "Fresh direct input, never a saved Retry.", {}, {attachment}, false, &error);
			if (variant == "direct-owned")
			{
				if (accepted) { const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5); while (fixture.session->processing && std::chrono::steady_clock::now() < deadline) peer.Tick(); }
				UAM_ASSERT(!error.empty() || !fixture.session->last_error.empty());
				UAM_ASSERT(peer.Sends().empty() && !fixture.session->saved_image_retry && !fixture.session->claude_input_preparation);
			}
			else
			{
				UAM_ASSERT(accepted && error.empty());
				const auto send = peer.Delivered();
				UAM_ASSERT_EQ(send["params"]["attachments"].size(), std::size_t{1});
				UAM_ASSERT_EQ(send["params"]["attachments"][0]["type"], nlohmann::json("file"));
				UAM_ASSERT_EQ(send["params"]["attachments"][0]["path"], nlohmann::json(sentinel.string()));
				UAM_ASSERT(!fixture.session->saved_image_retry && fixture.app.claude_input_tasks.empty());
				UAM_ASSERT_EQ(uam_test::ReadFile(sentinel), fixture.expected_original);
			}
		}
		else
		{
			if (variant == "corrupt") UAM_ASSERT(uam::io::WriteBinaryFile(blob, std::string(fixture.expected_original.size(), 'x')));
			else if (variant == "missing") UAM_ASSERT(fs::remove(blob));
			else if (variant == "foreign") image.native_asset_key = fixture.source_key;
			else if (variant == "mixed") { UAM_ASSERT(uam::io::WriteBinaryFile(sentinel, fixture.expected_original)); image.path = sentinel.string(); }
			else if (variant == "uri") { image = BranchOpaqueReference(); image.kind = "image"; image.mime_type = "image/png"; }
			else if (variant == "pending") image = BranchPendingOriginal();
			else if (variant == "nonimage") image.kind = "file";
			else image.mime_type = "image/tiff";
			std::string error;
			const bool accepted = fixture.BeginRetry(error);
			if (accepted)
			{
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
				do
				{
					peer.Tick(); // URI/pending default retries have no saved-image permission; drive their actual launch/validation too.
					UAM_ASSERT(peer.Sends().empty());
					if (!fixture.session->last_error.empty()) break;
					const bool pending = fixture.session->processing || !fixture.session->queued_prompt.empty() ||
					    fixture.session->pending_local_launch || fixture.session->pending_agent_input || fixture.session->local_stop_pending ||
					    fixture.session->worktree_launch_pending || fixture.session->reconnect_pending || fixture.session->prompt_request_id != 0 ||
					    !fixture.app.local_acp_launch_tasks.empty() || fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.branch_id);
					if (!pending) break;
				} while (std::chrono::steady_clock::now() < deadline);
			}
			UAM_ASSERT(!error.empty() || !fixture.session->last_error.empty());
			UAM_ASSERT(peer.Sends().empty() && !fixture.session->saved_image_retry && !fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery);
			if (variant == "corrupt" || variant == "missing") UAM_ASSERT(uam::io::WriteBinaryFile(blob, fixture.expected_original));
			if (variant == "mixed") UAM_ASSERT_EQ(uam_test::ReadFile(sentinel), fixture.expected_original);
			fixture.Branch().messages[2].attachments = original;
		}
		(void)fixture.BlobPath();
		fixture.AssertOriginalSource();
	}
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && UAM_ENABLE_RUNTIME_CLAUDE_CLI && UAM_ENABLE_RUNTIME_COPILOT_CLI && defined(__APPLE__)
namespace
{
	struct CopilotQueryOrderFixtureR78 : CopilotImageSupplementR77
	{
		explicit CopilotQueryOrderFixtureR78()
		{
			// Ordinary product preference, before actual setup/admission. Never fill captured agent fields.
			fixture.app.settings.provider_chat_defaults[uam::provider_ids::kCopilotCli].feature_preference = "provider";
			const auto executable = peer_root.root / "copilot";
			std::string script = uam_test::ReadFile(executable);
			const auto replace = [&script](const std::string& before, const std::string& after)
			{
				const auto position = script.find(before);
				UAM_ASSERT(position != std::string::npos && script.find(before, position + 1) == std::string::npos);
				script.replace(position, before.size(), after);
			};
			replace("traffic, held, buffer = [], [], b\"\"", "traffic, held, buffer = [], [], b\"\"\nauto_ids = set()");
			replace("    record(\"request\", value, frame, waiting)", R"PY(    auto_held = method == "session.model.getCurrent" and settings().get("hold_auto", False) and not os.path.isfile(os.path.join(root, "image-phase"))
    if auto_held:
        waiting = True
        auto_ids.add(value["id"])
    record("request", value, frame, waiting))PY");
			replace("    option = settings().get(\"variant\", \"healthy\")", R"PY(    if auto_held:
        result["modelId"] = settings().get("auto_model", "fixture-vision-model")
    option = settings().get("variant", "healthy"))PY");
			replace("            if settings().get(\"hold_status\", False) and original.get(\"method\") == \"status.get\":", R"PY(            if response.get("id") in auto_ids and not settings().get("release_auto", False):
                remaining.append(response)
            elif response.get("id") not in auto_ids and settings().get("hold_image_rest", False) and original.get("method") in ["status.get", "models.list"]:
                remaining.append(response)
            elif settings().get("hold_status", False) and original.get("method") == "status.get":)PY");
			UAM_ASSERT(uam::io::WriteTextFile(executable, script));
			fs::permissions(executable, fs::perms::owner_all);
			UAM_ASSERT((fs::status(executable).permissions() & fs::perms::owner_exec) != fs::perms::none);
			std::string error;
			UAM_ASSERT(uam::StartAcpModelDiscovery(fixture.app, fixture.branch_id, &error, false) && error.empty());
			fixture.session = uam::FindAcpSessionForChat(fixture.app, fixture.branch_id);
			UAM_ASSERT(fixture.session);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while ((!fixture.session->session_ready || !fixture.session->pending_request_methods.empty()) && std::chrono::steady_clock::now() < deadline) Tick();
			UAM_ASSERT(fixture.session->running && fixture.session->session_ready && fixture.session->initialized && !fixture.session->processing);
			UAM_ASSERT(fixture.session->pending_request_methods.empty() && fixture.session->last_error.empty());
			UAM_ASSERT_EQ(fixture.session->session_id, native_session);
			const auto durable = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.branch_id);
			UAM_ASSERT(durable && durable->native_session_id == native_session);
		}

		void WaitParsed(int id)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (!parsed_reply_ids.contains(id) && std::chrono::steady_clock::now() < deadline) { ReadRepliesOnly(); std::this_thread::yield(); }
			UAM_ASSERT(parsed_reply_ids.contains(id));
		}
	};
} // namespace

UAM_TEST(CopilotSavedImageEvidenceRejectsLaterOwnedAutoModelWhileQueriesPending)
{
	// Same-model control precedes the conflicting observation, defeating blanket refresh/Retry refusal.
	for (const bool conflicting : {false, true})
	{
		CopilotQueryOrderFixtureR78 peer;
		auto& fixture = peer.fixture;
		const auto& runtime = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli);
		const std::string observed_model = conflicting ? "different-native-model" : "fixture-vision-model";
		peer.control["hold_auto"] = true;
		peer.control["auto_model"] = observed_model;
		peer.control["release_auto"] = false;
		peer.control["hold_image_rest"] = true;
		peer.WriteControl();
		const auto process = uam::acp_detail::AcpProcessHandleLabel(*fixture.session);
		const auto writer = fixture.session->stdin_writer;
		std::string error;
		// The same public adapter hook is called by CEF refreshNativeModelOptions; it allocates the real owned IDs.
		UAM_ASSERT(runtime.OnAcpRefreshNativeModelOptions(fixture.app, *fixture.session, fixture.Branch(), &error) && error.empty());
		int auto_current = 0;
		std::string pid;
		const auto refresh_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (auto_current == 0 && std::chrono::steady_clock::now() < refresh_deadline)
		{
			peer.ReadRepliesOnly();
			for (const auto& entry : peer.Trace())
				if (entry["direction"] == "request" && entry.value("held", false) && entry["value"].value("method", "") == "session.model.getCurrent")
				{
					auto_current = entry["value"]["id"].get<int>();
					pid = entry["pid"].get<std::string>();
				}
			std::this_thread::yield();
		}
		UAM_ASSERT(auto_current > 0 && fixture.session->pending_request_methods.contains(auto_current));
		UAM_ASSERT_EQ(fixture.session->pending_request_methods.at(auto_current), std::string("session.model.getCurrent"));
		const auto list_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (fixture.session->pending_request_methods.size() != 1 && std::chrono::steady_clock::now() < list_deadline) { peer.ReadRepliesOnly(); std::this_thread::yield(); }
		UAM_ASSERT_EQ(fixture.session->pending_request_methods.size(), std::size_t{1});
		UAM_ASSERT(fixture.session->pending_request_methods.contains(auto_current));
		UAM_ASSERT(!fixture.session->processing && peer.Sends().empty());
		peer.RequireRetry(); // Existing PUBLIC direct Retry permits the pending Auto query; UI RetryFailed has a different busy check.
		const auto permission = fixture.session->saved_image_retry;
		UAM_ASSERT(permission);
		int own_current = 0, own_status = 0, own_models = 0;
		const auto query_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while ((own_current == 0 || own_status == 0 || own_models == 0) && std::chrono::steady_clock::now() < query_deadline)
		{
			peer.Tick();
			for (const auto& entry : peer.Trace())
			{
				if (entry["direction"] != "request" || !entry.value("held", false) || entry["value"]["id"] == auto_current) continue;
				UAM_ASSERT_EQ(entry["pid"], nlohmann::json(pid));
				const auto method = entry["value"].value("method", "");
				if (method == "session.model.getCurrent") own_current = entry["value"]["id"].get<int>();
				else if (method == "status.get") own_status = entry["value"]["id"].get<int>();
				else if (method == "models.list") own_models = entry["value"]["id"].get<int>();
			}
			UAM_ASSERT(!fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery && peer.Sends().empty());
		}
		UAM_ASSERT(own_current > 0 && own_status > 0 && own_models > 0 && own_current != auto_current);
		UAM_ASSERT_EQ(peer.HeldRequestCount(), std::size_t{4});
		UAM_ASSERT_EQ(uam::acp_detail::AcpProcessHandleLabel(*fixture.session), process);
		UAM_ASSERT_EQ(fixture.session->stdin_writer, writer); // No restart/adoption can retire the independent Auto owner.
		UAM_ASSERT_EQ(fixture.session->saved_image_retry, permission);
		peer.Release(); // Only own getCurrent=M can pass the private response gate first.
		peer.WaitParsed(own_current);
		UAM_ASSERT(!fixture.session->pending_request_methods.contains(own_current));
		UAM_ASSERT(fixture.session->pending_request_methods.contains(auto_current) && fixture.session->pending_request_methods.contains(own_status) && fixture.session->pending_request_methods.contains(own_models));
		UAM_ASSERT(!fixture.session->claude_input_preparation && peer.Sends().empty());
		peer.control["release_auto"] = true;
		peer.WriteControl();
		peer.WaitParsed(auto_current); // Actual newer independently owned N (or healthy M), still before status/models.
		UAM_ASSERT(!fixture.session->pending_request_methods.contains(auto_current));
		const auto projected = runtime.OnAcpNativeModelOptionsForFrontend(*fixture.session).at("autoRouting");
		UAM_ASSERT_EQ(projected.at("currentModel").at("value"), nlohmann::json(observed_model));
		UAM_ASSERT(!fixture.session->claude_input_preparation && peer.Sends().empty());
		peer.control["hold_image_rest"] = false;
		peer.WriteControl();
		peer.WaitParsed(own_status);
		peer.WaitParsed(own_models); // Late real controls cannot revive a contradictory attempt.
		if (!conflicting)
		{
			const auto send = peer.Delivered();
			UAM_ASSERT_EQ(send["params"]["attachments"].size(), std::size_t{1});
			UAM_ASSERT_EQ(CopilotImageSupplementR77::DecodeBlob(send["params"]["attachments"][0], "image/png", fixture.Branch().messages[2].attachments[0].name), fixture.expected_original);
		}
		else
		{
			(void)uam::acp_detail::SendQueuedPromptIfReady(fixture.app, *fixture.session, fixture.Branch());
			UAM_ASSERT(!fixture.session->claude_input_preparation && fixture.app.claude_input_tasks.empty()); // Intended unfixed-source RED: conflicting owned Auto N must block original reading while querying.
			UAM_ASSERT(!fixture.session->saved_image_retry && !fixture.session->pending_prompt_delivery && !fixture.session->last_error.empty());
			for (int poll = 0; poll < 3; ++poll) peer.Tick();
			UAM_ASSERT(peer.Sends().empty() && peer.HeldRequestCount() == 4 && !fixture.session->saved_image_retry && fixture.app.claude_input_tasks.empty());
		}
		fixture.AssertOriginalSource();
	}
}
#endif


#include "app/opencode_native_activity_service.h"
#if UAM_ENABLE_RUNTIME_GEMINI_CLI && UAM_ENABLE_RUNTIME_CLAUDE_CLI && UAM_ENABLE_RUNTIME_OPENCODE_CLI && defined(__APPLE__)
#include <unistd.h>
namespace
{
	struct OpenCodeOwnedImageFixtureR80
	{
		// Reverse member destruction retains the exclusive PATH and peer files through the whole AppState.
		uam_test::TempDir peer_root{"uam-private-opencode-image-peer"};
		std::unique_ptr<uam_test::ScopedEnvVar> private_path;
		ClaudeOwnedRetryFixtureR71 fixture;
		struct Generation
		{
			std::shared_ptr<std::string> wire = std::make_shared<std::string>();
			std::shared_ptr<uam::platform::AsyncByteWriter> observed, transport;
		};
		std::vector<Generation> generations;
		std::string native_session;
		const fs::path executable = peer_root.root / "opencode";
		static constexpr const char* model = "fixture-provider/fixture-model";

		OpenCodeOwnedImageFixtureR80()
		{
			UAM_ASSERT(uam::io::WriteTextFile(executable, R"PY(#!/usr/bin/python3 -I
import base64, http.server, json, os, select, sys, threading, time, urllib.parse, uuid
root = os.path.dirname(os.path.abspath(__file__))
args = sys.argv[1:]
if len(args) != 6 or args[:3] != ["acp", "--hostname", "127.0.0.1"] or args[3] != "--port" or args[5] != "--no-mdns":
    raise RuntimeError("Only the canonical private ACP/companion argv is allowed: " + repr(args))
port = int(args[4])
password = os.environ.get("OPENCODE_SERVER_PASSWORD", "")
if not 1024 <= port <= 65535 or not password or os.environ.get("OPENCODE_SERVER_USERNAME") != "opencode":
    raise RuntimeError("Missing generated private companion authority")
permission = json.loads(os.environ.get("OPENCODE_PERMISSION", "{}"))
if permission.get("task") != "deny":
    raise RuntimeError("Canonical launch must initially deny native Task")
expected_authorization = "Basic " + base64.b64encode(("opencode:" + password).encode()).decode("ascii")
trace_path = os.path.join(root, "opencode-trace-" + str(os.getpid()) + ".json")
registry_path = os.path.join(root, "native-registry.json")
lock, traffic = threading.RLock(), []
provider, native_model = "fixture-provider", "fixture-model"
selected_model, secondary_model = provider + "/" + native_model, provider + "/text-model"
def write_json(path, value):
    temporary = path + "." + str(threading.get_ident()) + ".tmp"
    with open(temporary, "w", encoding="utf-8") as stream:
        json.dump(value, stream, separators=(",", ":"))
    os.replace(temporary, path)
def record(kind, value):
    with lock:
        traffic.append({"kind": kind, "value": value})
        write_json(trace_path, traffic)
def registry():
    if not os.path.isfile(registry_path):
        return {}
    with open(registry_path, encoding="utf-8") as stream:
        return json.load(stream)
def image_control():
    path = os.path.join(root, "image-control.json")
    if not os.path.isfile(path):
        return {}
    with open(path, encoding="utf-8") as stream:
        return json.load(stream)
def configuration(state):
    return [{"id": "model", "name": "Model", "category": "model", "type": "select", "currentValue": state["model"],
             "options": [{"value": selected_model, "name": "Private vision model"}, {"value": secondary_model, "name": "Private text model"}]},
            {"id": "mode", "name": "Mode", "category": "mode", "type": "select", "currentValue": state["mode"],
             "options": [{"value": "build", "name": "Build"}, {"value": "plan", "name": "Plan"}]}]
def session_result(identifier, state):
    return {"sessionId": identifier, "models": {"currentModelId": state["model"], "availableModels": [
             {"modelId": selected_model, "name": "Private vision model"}, {"modelId": secondary_model, "name": "Private text model"}]},
            "modes": {"currentModeId": state["mode"], "availableModes": [{"id": "build", "name": "Build"}, {"id": "plan", "name": "Plan"}]},
            "configOptions": configuration(state)}
class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *args):
        pass
    def answer(self, status, value, raw=None):
        body = raw.encode("utf-8") if raw is not None else json.dumps(value, separators=(",", ":")).encode("utf-8")
        written = False
        try:
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            self.wfile.flush()
            written = True
        except (BrokenPipeError, ConnectionResetError):
            pass
        self.close_connection = True
        record("http.response", {"method": self.command, "path": self.path, "status": status, "body": value, "written": written, "raw": raw is not None})
    def handle_request(self):
        parsed = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(parsed.query)
        authorized = self.headers.get("Authorization", "") == expected_authorization
        length = int(self.headers.get("Content-Length", "0"))
        if length < 0 or length > 65536:
            self.answer(413, {"error": "oversized private request"})
            return
        body = json.loads(self.rfile.read(length)) if length else None
        record("http.request", {"method": self.command, "path": self.path, "directory": query.get("directory", [""])[0], "authorized": authorized, "body": body})
        if not authorized:
            self.answer(401, {"error": "unauthorized"})
            return
        if parsed.path == "/global/health" and self.command == "GET":
            variant = image_control().get("variant", "healthy")
            self.answer(200, {"healthy": variant != "unhealthy", "version": "1.18.36" if variant == "version-mismatch" else "1.18.35"})
            return
        directory = query.get("directory", [""])[0]
        with lock:
            sessions = registry()
            owned = [state for state in sessions.values() if os.path.realpath(directory) == state["directory"]]
        if not directory or not owned:
            self.answer(400, {"error": "actual native directory required"})
            return
        if parsed.path == "/config/providers" and self.command == "GET":
            # Only this native image-evidence operation is held. Permission/bootstrap remains live.
            deadline = time.monotonic() + 1.75
            while not os.path.isfile(os.path.join(root, "release-image-evidence")) and time.monotonic() < deadline:
                time.sleep(0.002)
            if not os.path.isfile(os.path.join(root, "release-image-evidence")):
                self.answer(503, {"error": "private evidence release deadline"})
                return
            def info(identifier, image):
                return {"id": identifier, "providerID": provider, "name": "Private configured model",
                        "capabilities": {"input": {"image": image}}}
            value = {"providers": [{"id": provider, "name": "Private configured provider", "models": {
                 native_model: info(native_model, True), "text-model": info("text-model", False)}}], "default": {provider: native_model}}
            variant = image_control().get("variant", "healthy")
            selected = value["providers"][0]["models"][native_model]
            if variant == "vision-false": selected["capabilities"]["input"]["image"] = False
            if variant == "vision-missing": del selected["capabilities"]["input"]["image"]
            if variant == "vision-string": selected["capabilities"]["input"]["image"] = "true"
            if variant == "selected-missing": del value["providers"][0]["models"][native_model]
            if variant == "provider-mismatch": value["providers"][0]["id"] = "other-provider"
            if variant == "record-mismatch": selected["providerID"] = "other-provider"
            if variant == "duplicate-provider":
                duplicate = json.loads(json.dumps(value["providers"][0]))
                duplicate["models"][native_model]["capabilities"]["input"]["image"] = False
                value["providers"].append(duplicate)
            if variant == "http-error":
                self.answer(503, {"error": "private image evidence unavailable"})
            elif variant == "malformed":
                self.answer(200, None, raw="not-json")
            else:
                self.answer(200, value)
            return
        if parsed.path == "/permission" and self.command == "GET":
            self.answer(200, [])
            return
        if parsed.path == "/session/status" and self.command == "GET":
            self.answer(200, {})
            return
        if parsed.path.startswith("/session/"):
            parts = parsed.path.split("/")
            identifier = parts[2]
            with lock:
                sessions = registry()
                state = sessions.get(identifier)
                if not state or state["directory"] != os.path.realpath(directory):
                    self.answer(404, {"error": "unowned native session"})
                    return
                if len(parts) == 3 and self.command == "PATCH":
                    if body != {"permission": [{"permission": "task", "pattern": "*", "action": "ask"}]}:
                        self.answer(400, {"error": "only exact real bootstrap task policy is supported"})
                        return
                    state["task"] = "ask"
                    write_json(registry_path, sessions)
                if len(parts) == 3 and self.command in ["GET", "PATCH"]:
                    self.answer(200, {"id": identifier, "directory": state["directory"], "permission": [
                        {"permission": "task", "pattern": "*", "action": state["task"]}]})
                    return
                if len(parts) == 4 and parts[3] == "message" and self.command == "GET":
                    self.answer(200, [])
                    return
        self.answer(404, {"error": "unexpected private endpoint"})
    do_GET = handle_request
    do_PATCH = handle_request
server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
server.daemon_threads = True
threading.Thread(target=server.serve_forever, daemon=True).start()
record("launch", {"argv": args, "host": "127.0.0.1", "port": port, "privateAuthPresent": True})
def respond(identifier, result):
    response = {"jsonrpc": "2.0", "id": identifier, "result": result}
    record("acp.response", response)
    sys.stdout.write(json.dumps(response, separators=(",", ":")) + "\n")
    sys.stdout.flush()
buffer, pending_prompt = b"", None
active_native, config_emitted = None, False
try:
    while True:
        notification_path = os.path.join(root, "emit-config.json")
        if active_native is not None and not config_emitted and os.path.isfile(notification_path):
            with open(notification_path, encoding="utf-8") as stream:
                change = json.load(stream)
            if change != {"model": secondary_model}:
                raise RuntimeError("Only the advertised private native model change is supported")
            with lock:
                sessions = registry()
                state = sessions[active_native]
                state["model"] = secondary_model
                write_json(registry_path, sessions)
                notification = {"jsonrpc": "2.0", "method": "session/update", "params": {"sessionId": active_native,
                    "update": {"sessionUpdate": "config_option_update", "configOptions": configuration(state)}}}
            sys.stdout.write(json.dumps(notification, separators=(",", ":")) + "\n")
            sys.stdout.flush()
            record("acp.notification.flushed", notification) # Record ONLY after actual native bytes are available, before C++ polls.
            config_emitted = True
        if pending_prompt is not None and os.path.isfile(os.path.join(root, "finish-prompt")):
            respond(pending_prompt, {"stopReason": "end_turn"})
            pending_prompt = None
        readable, _, _ = select.select([sys.stdin.buffer], [], [], 0.005)
        if not readable:
            continue
        chunk = os.read(sys.stdin.fileno(), 65536)
        if not chunk:
            break
        buffer += chunk
        while b"\n" in buffer:
            raw, buffer = buffer.split(b"\n", 1)
            if not raw.strip():
                continue
            if len(raw) > 4 * 1024 * 1024:
                raise RuntimeError("Private ACP input bound exceeded")
            request = json.loads(raw)
            record("acp.request", request)
            method, params = request.get("method", ""), request.get("params", {})
            if method == "initialize":
                respond(request["id"], {"protocolVersion": 1, "agentInfo": {"name": "opencode", "title": "OpenCode", "version": "1.18.35"},
                    "agentCapabilities": {"loadSession": True, "promptCapabilities": {"image": True}}})
                continue
            if method == "initialized" or method == "session/cancel":
                continue
            with lock:
                sessions = registry()
                if method == "session/new":
                    identifier = "ses_" + uuid.uuid4().hex
                    state = {"directory": os.path.realpath(params["cwd"]), "model": selected_model, "mode": "build", "task": "deny"}
                    sessions[identifier] = state
                    active_native = identifier
                    write_json(registry_path, sessions)
                    respond(request["id"], session_result(identifier, state))
                    continue
                identifier = params.get("sessionId", "")
                state = sessions.get(identifier)
                if not state or state["directory"] != os.path.realpath(params.get("cwd", state["directory"])):
                    raise RuntimeError("ACP request does not name actual registered native ownership")
                if method in ["session/load", "session/resume"]:
                    active_native = identifier
                    respond(request["id"], session_result(identifier, state))
                elif method == "session/set_model":
                    if params["modelId"] not in [selected_model, secondary_model]:
                        raise RuntimeError("Unadvertised model")
                    state["model"] = params["modelId"]
                    write_json(registry_path, sessions)
                    respond(request["id"], {"configOptions": configuration(state)})
                elif method == "session/set_mode":
                    if params["modeId"] not in ["build", "plan"]:
                        raise RuntimeError("Unadvertised mode")
                    state["mode"] = params["modeId"]
                    write_json(registry_path, sessions)
                    respond(request["id"], {"configOptions": configuration(state)})
                elif method == "session/set_config_option":
                    if params["configId"] != "model" or params["value"] not in [selected_model, secondary_model]:
                        raise RuntimeError("Unadvertised model configuration")
                    state["model"] = params["value"]
                    write_json(registry_path, sessions)
                    respond(request["id"], {"configOptions": configuration(state)})
                elif method == "session/prompt":
                    if pending_prompt is not None:
                        raise RuntimeError("Duplicate native prompt")
                    pending_prompt = request["id"] # Actual request retained for a real end_turn ACK after inspection.
                else:
                    raise RuntimeError("Unexpected private ACP method: " + method)
finally:
    server.shutdown()
    server.server_close()
)PY"));
			fs::permissions(executable, fs::perms::owner_all);
			AssertExecutable(); // Atomic writes replace file mode; check AFTER chmod and before any launch.
			private_path = std::make_unique<uam_test::ScopedEnvVar>("PATH", peer_root.root.string());
			auto& app = fixture.app;
			UAM_ASSERT(!app.provider_model_catalog);
			app.settings.central_provider_configuration = {};
			app.settings.central_provider_configuration.enabled = false;
			app.settings.central_provider_configuration.uam_control_enabled = false;
			app.settings.mcp_servers.clear(); app.settings.host_scoped_settings.clear();
			app.settings.git_enabled = app.settings.git_auto_worktree = app.settings.svn_enabled = false;
			app.settings.computer_use_globally_enabled = false;
			app.settings.provider_chat_defaults[uam::provider_ids::kOpenCodeCli].feature_preference = "provider";
			UAM_ASSERT_EQ(uam::SwitchChatProvider(app, fixture.branch_id, uam::provider_ids::kOpenCodeCli), uam::ChatProviderSwitchResult::Changed);
			auto& chat = fixture.Branch();
			chat.memory_enabled = chat.uam_control_enabled = chat.computer_use_enabled = false;
			chat.model_id = model; chat.reasoning_effort.clear(); chat.service_tier.clear(); chat.service_tier_explicit = false;
			chat.approval_mode = "default"; chat.command_safety_tier = "off";
			UAM_ASSERT(chat.native_session_id.empty() && chat.messages[2].attachments.front().path.empty());
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
			const auto reloaded = ChatRepository::LoadLocalChat(app.data_root, fixture.branch_id);
			UAM_ASSERT(reloaded && reloaded->provider_id == uam::provider_ids::kOpenCodeCli && reloaded->native_session_id.empty());
			UAM_ASSERT_EQ(reloaded->messages[2].attachments.front().native_asset_key, fixture.branch_key);
			chat = *reloaded;
			// This is the same DEFAULT app-owned service polled by Application::PollTick, with actual HTTP execution.
			app.opencode_native_activity = std::make_shared<uam::OpenCodeNativeActivityService>();
			std::string error;
			AssertExecutable();
			const bool started = uam::StartAcpModelDiscovery(app, fixture.branch_id, &error, false);
			fixture.session = uam::FindAcpSessionForChat(app, fixture.branch_id);
			if (!started || !fixture.session) throw uam_test::TestFailure("Private OpenCode setup did not start: " + error);
			WaitReady();
			const auto traffic = Trace();
			int creates = 0;
			for (const auto& item : traffic)
			{
				if (item["kind"] != "acp.request" || item["value"].value("method", "") != "session/new") continue;
				++creates;
				for (const auto& response : traffic)
					if (response["pid"] == item["pid"] && response["kind"] == "acp.response" && response["value"]["id"] == item["value"]["id"])
						native_session = response["value"]["result"]["sessionId"].get<std::string>();
			}
			UAM_ASSERT_EQ(creates, 1);
			UAM_ASSERT(native_session.starts_with("ses_") && native_session.size() > 4);
			UAM_ASSERT_EQ(fixture.session->session_id, native_session);
			UAM_ASSERT_EQ(chat.native_session_id, native_session);
			UAM_ASSERT_EQ(fixture.session->agent_version, std::string("1.18.35"));
			UAM_ASSERT(fixture.session->prompt_image_supported && fixture.session->opencode_native_companion);
			// Actual public setting requests and matched replies, not optimistic current_* assignments.
			UAM_ASSERT(uam::SetAcpSessionModel(app, fixture.branch_id, model, &error) && error.empty()); WaitReady();
			UAM_ASSERT(uam::SetAcpSessionConfigOption(app, fixture.branch_id, "model", "fixture-provider/text-model", &error) && error.empty()); WaitReady();
			UAM_ASSERT(uam::SetAcpSessionConfigOption(app, fixture.branch_id, "model", model, &error) && error.empty()); WaitReady();
			UAM_ASSERT(uam::SetAcpSessionMode(app, fixture.branch_id, "default", &error) && error.empty()); WaitReady();
			UAM_ASSERT_EQ(fixture.session->current_model_id, std::string(model));
			UAM_ASSERT_EQ(fixture.session->current_mode_id, std::string("default"));
			WaitBootstrap();
			const auto durable = ChatRepository::LoadLocalChat(app.data_root, fixture.branch_id);
			UAM_ASSERT(durable && durable->native_session_id == native_session && durable->messages[2].attachments == chat.messages[2].attachments);
			UAM_ASSERT(Prompts().empty() && HttpCount("/config/providers") == 0);
			fixture.AssertOriginalSource();
		}

		~OpenCodeOwnedImageFixtureR80()
		{
			(void)uam::io::WriteTextFile(peer_root.root / "release-image-evidence", "release private handler during teardown");
			(void)uam::io::WriteTextFile(peer_root.root / "finish-prompt", "ack only an actual captured request");
			try
			{
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
				while (fixture.session && fixture.session->running && fixture.session->processing && std::chrono::steady_clock::now() < deadline)
				{
					(void)uam::PollAllAcpSessions(fixture.app);
					(void)uam::FlushPendingChatSaves(fixture.app);
					std::this_thread::yield();
				}
			}
			catch (...) {}
			if (fixture.app.opencode_native_activity) fixture.app.opencode_native_activity->Stop();
			if (fixture.session) (void)uam::StopAcpSession(fixture.app, fixture.branch_id);
			for (auto& generation : generations) { generation.observed->Stop(); generation.transport->Stop(); }
			fixture.CloseSink();
			for (auto& task : fixture.app.acp_process_stop_tasks) if (task.worker && task.worker->joinable()) task.worker->join();
			fixture.app.acp_process_stop_tasks.clear();
			for (auto& task : fixture.app.claude_input_tasks)
			{
				if (task.state) task.state->cancel.request_stop();
				if (task.worker && task.worker->joinable()) task.worker->join();
			}
		}
		void AssertExecutable() const
		{
			UAM_ASSERT(fs::is_regular_file(executable));
			UAM_ASSERT((fs::status(executable).permissions() & fs::perms::owner_exec) != fs::perms::none);
			UAM_ASSERT(::access(executable.c_str(), X_OK) == 0);
		}
		void ObserveWriter()
		{
			fixture.session = uam::FindAcpSessionForChat(fixture.app, fixture.branch_id);
			if (!fixture.session || !fixture.session->stdin_writer) return;
			for (const auto& generation : generations) if (generation.observed == fixture.session->stdin_writer) return;
			Generation generation;
			generation.transport = fixture.session->stdin_writer;
			generation.observed = std::make_shared<uam::platform::AsyncByteWriter>(
			    [capture = generation.wire, writer = generation.transport](const char* bytes, std::size_t count, std::string& error) -> std::ptrdiff_t
			    { capture->append(bytes, count); return writer->Enqueue(bytes, count, &error) ? static_cast<std::ptrdiff_t>(count) : -1; }, [] {});
			fixture.session->stdin_writer = generation.observed;
			generations.push_back(std::move(generation));
		}
		void Tick()
		{
			AssertExecutable();
			ObserveWriter();
			(void)uam::PollAllAcpSessions(fixture.app);
			if (fixture.app.opencode_native_activity) (void)fixture.app.opencode_native_activity->Poll(fixture.app);
			(void)uam::FlushPendingChatSaves(fixture.app);
			ObserveWriter();
			std::this_thread::yield();
		}
		void WaitReady()
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
			do { Tick(); } while ((!fixture.session->session_ready || !fixture.session->pending_request_methods.empty() || fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.branch_id)) && std::chrono::steady_clock::now() < deadline);
			if (!fixture.session->running || !fixture.session->initialized || !fixture.session->session_ready || !fixture.session->last_error.empty()) throw uam_test::TestFailure("Private OpenCode readiness prerequisite: " + fixture.session->last_error);
			UAM_ASSERT(fixture.session->pending_request_methods.empty() && !fixture.session->processing);
			UAM_ASSERT(!fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.branch_id));
		}
		void WaitBootstrap()
		{
			std::string error;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
			uam::OpenCodeNativeGateState gate;
			bool patched = false, readback = false;
			const auto observe_readback = [&]()
			{
				patched = false; readback = false;
				for (const auto& entry : Trace())
				{
					if (entry["kind"] == "http.request" && entry["value"].value("method", "") == "PATCH" && entry["value"].value("path", "").starts_with("/session/" + native_session + "?")) patched = entry["value"].value("authorized", false);
					if (patched && entry["kind"] == "http.response" && entry["value"].value("method", "") == "GET" && entry["value"].value("path", "").starts_with("/session/" + native_session + "?"))
						readback = uam::OpenCodeNativeActivityService::TaskApprovalEnabled(entry["value"]["body"]);
				}
			};
			do
			{
				Tick();
				gate = fixture.app.opencode_native_activity->BeforeDispatch(fixture.app, *fixture.session, fixture.Branch(), error);
				// HTTP response bytes can arrive before the peer publishes its trace.
				observe_readback();
			} while ((gate == uam::OpenCodeNativeGateState::Pending ||
			          (gate == uam::OpenCodeNativeGateState::Ready && (!patched || !readback))) &&
			         std::chrono::steady_clock::now() < deadline);
			if (gate != uam::OpenCodeNativeGateState::Ready)
			{
				const auto& state = *fixture.session;
				std::cerr << "[private OpenCode bootstrap] running=" << state.running << " ready=" << state.session_ready << " processing=" << state.processing << " lastProcess=" << state.last_process_id << " session=" << state.session_id << " sessionChat=" << state.chat_id << " chat=" << fixture.Branch().id << " cleanup=" << fixture.Branch().side_cleanup_requested << " localStop=" << state.local_stop_pending << " remoteStop=" << state.remote_stop_pending << " lifecycle=" << state.lifecycle_state << " lastError=" << state.last_error << std::endl;
				const auto trace = Trace();
				std::size_t http = 0;
				for (const auto& entry : trace)
					if (entry["kind"].get<std::string>().starts_with("http.") && http++ < 16) std::cerr << "[private OpenCode HTTP] " << entry.dump().substr(0, 2048) << std::endl;
				std::cerr << "[private OpenCode HTTP count] " << http << " stderr=" << state.recent_stderr.substr(0, 4096) << std::endl;
				throw uam_test::TestFailure("Real OpenCode permission-bootstrap prerequisite: " + error);
			}
			UAM_ASSERT(patched && readback && Prompts().empty());
		}
		nlohmann::json Trace() const
		{
			nlohmann::json result = nlohmann::json::array();
			for (const auto& path : fs::directory_iterator(peer_root.root))
			{
				const auto name = path.path().filename().string();
				if (!name.starts_with("opencode-trace-") || !name.ends_with(".json")) continue;
				const auto traffic = nlohmann::json::parse(uam_test::ReadFile(path.path()), nullptr, false);
				UAM_ASSERT(traffic.is_array());
				for (auto entry : traffic) { entry["pid"] = name; result.push_back(std::move(entry)); }
			}
			return result;
		}
		std::size_t HttpCount(const std::string& route) const
		{
			std::size_t count = 0;
			for (const auto& entry : Trace()) if (entry["kind"] == "http.request" && entry["value"].value("path", "").starts_with(route + "?")) ++count;
			return count;
		}
		std::vector<nlohmann::json> Prompts() const
		{
			std::vector<nlohmann::json> result;
			for (const auto& entry : Trace()) if (entry["kind"] == "acp.request" && entry["value"].value("method", "") == "session/prompt") result.push_back(entry["value"]);
			return result; // Counts every prompt, including an unwanted text-only fallback.
		}
		std::vector<nlohmann::json> ObservedPrompts()
		{
			std::vector<nlohmann::json> result;
			for (auto& generation : generations)
			{
				if (generation.observed == fixture.session->stdin_writer) UAM_ASSERT(generation.observed->Flush(std::chrono::seconds(1)) && generation.transport->Flush(std::chrono::seconds(1)));
				else { generation.observed->Stop(); generation.transport->Stop(); }
				std::istringstream lines(*generation.wire);
				std::string line;
				while (std::getline(lines, line))
				{
					if (line.empty()) continue;
					const auto value = nlohmann::json::parse(line);
					if (value.value("method", "") == "session/prompt") result.push_back(value);
				}
			}
			return result;
		}
	};
} // namespace

UAM_TEST(OpenCodeSavedBranchRetryDeliversOwnedPngThroughActualAcpAndNativeEvidence)
{
	for (const bool cold : {false, true})
	{
		OpenCodeOwnedImageFixtureR80 peer;
		auto& fixture = peer.fixture;
		if (cold)
		{
			UAM_ASSERT(uam::StopAcpSession(fixture.app, fixture.branch_id));
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
			while ((!fixture.app.acp_process_stop_tasks.empty() || fixture.session->local_stop_pending) && std::chrono::steady_clock::now() < deadline) peer.Tick();
			UAM_ASSERT(!fixture.session->running && !fixture.session->local_stop_pending && fixture.app.acp_process_stop_tasks.empty());
			UAM_ASSERT_EQ(fixture.Branch().native_session_id, peer.native_session);
		}
		std::string error;
		peer.AssertExecutable();
		const bool accepted = fixture.BeginRetry(error);
		if (!accepted)
		{
			if (error != "Recorded image Retry requires valid local originals and a supported provider. Remove unsupported sources or attach files again.") throw uam_test::TestFailure("OpenCode saved-image prerequisite failed with another error: " + error);
			throw uam_test::TestFailure("OpenCode accepted-Retry prerequisite after actual ACP/native bootstrap and durable reload: " + error); // Intended current unsupported-route RED, warm arm first.
		}
		peer.ObserveWriter();
		const auto permission = fixture.session->saved_image_retry;
		UAM_ASSERT(permission && error.empty());
		const auto evidence_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		while (peer.HttpCount("/config/providers") == 0 && std::chrono::steady_clock::now() < evidence_deadline)
		{
			peer.Tick();
			UAM_ASSERT_EQ(fixture.session->saved_image_retry, permission);
			UAM_ASSERT(!fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery && fixture.app.claude_input_tasks.empty() && peer.Prompts().empty());
		}
		UAM_ASSERT_EQ(peer.HttpCount("/config/providers"), std::size_t{1});
		std::string evidence_pid;
		for (const auto& entry : peer.Trace())
			if (entry["kind"] == "http.request" && entry["value"].value("path", "").starts_with("/config/providers?"))
			{
				evidence_pid = entry["pid"].get<std::string>();
				UAM_ASSERT(entry["value"].value("authorized", false));
				UAM_ASSERT_EQ(fs::canonical(fs::path(entry["value"]["directory"].get<std::string>())), fs::canonical(fs::path(fixture.Branch().workspace_directory)));
			}
		bool patched = false, readback = false;
		for (const auto& entry : peer.Trace())
		{
			if (entry["pid"] != evidence_pid) continue;
			if (entry["kind"] == "http.request" && entry["value"].value("method", "") == "PATCH" && entry["value"].value("path", "").starts_with("/session/" + peer.native_session + "?")) patched = entry["value"].value("authorized", false);
			if (patched && entry["kind"] == "http.response" && entry["value"].value("method", "") == "GET" && entry["value"].value("path", "").starts_with("/session/" + peer.native_session + "?")) readback = uam::OpenCodeNativeActivityService::TaskApprovalEnabled(entry["value"]["body"]);
		}
		UAM_ASSERT(!evidence_pid.empty() && patched && readback); // The SAME peer generation honestly bootstrapped, including the cold arm.
		UAM_ASSERT_EQ(fixture.session->saved_image_retry, permission);
		UAM_ASSERT(!fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery && peer.Prompts().empty());
		UAM_ASSERT(uam::io::WriteTextFile(peer.peer_root.root / "release-image-evidence", "release only actual native catalog response"));
		const auto delivery_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		while (peer.Prompts().empty() && std::chrono::steady_clock::now() < delivery_deadline) peer.Tick();
		const auto prompts = peer.Prompts();
		UAM_ASSERT_EQ(prompts.size(), std::size_t{1});
		UAM_ASSERT_EQ(peer.ObservedPrompts(), prompts);
		const auto& request = prompts.front();
		UAM_ASSERT_EQ(request["params"]["sessionId"], nlohmann::json(peer.native_session));
		UAM_ASSERT_EQ(request["id"], nlohmann::json(fixture.session->prompt_request_id));
		UAM_ASSERT_EQ(fixture.session->pending_request_methods.at(fixture.session->prompt_request_id), std::string("session/prompt"));
		std::string text;
		std::vector<nlohmann::json> images;
		for (const auto& part : request["params"]["prompt"])
		{
			if (part.value("type", "") == "image") images.push_back(part);
			else { UAM_ASSERT_EQ(part.value("type", ""), std::string("text")); text += part.value("text", ""); }
		}
		UAM_ASSERT_EQ(images.size(), std::size_t{1});
		UAM_ASSERT_EQ(images[0]["mimeType"], nlohmann::json("image/png"));
		std::string decoded;
		UAM_ASSERT(uam::base64::Decode(images[0]["data"].get<std::string>(), decoded));
		UAM_ASSERT_EQ(decoded, fixture.expected_original);
		UAM_ASSERT(!images[0].contains("uri") && !images[0].contains("path"));
		UAM_ASSERT(text.find("Image 1 (native image-block order): " + nlohmann::json(fixture.Branch().messages[2].attachments[0].name).dump()) != std::string::npos);
		UAM_ASSERT(request.dump().find(fixture.branch_key) == std::string::npos && request.dump().find(fixture.source_key) == std::string::npos);
		UAM_ASSERT_EQ(fixture.session->current_model_id, std::string(OpenCodeOwnedImageFixtureR80::model));
		UAM_ASSERT_EQ(fixture.session->agent_version, std::string("1.18.35"));
		const auto saved = ChatRepository::LoadLocalChat(fixture.app.data_root, fixture.branch_id);
		UAM_ASSERT(saved && saved->native_session_id == peer.native_session && saved->messages[2].attachments == fixture.Branch().messages[2].attachments);
		if (cold)
		{
			bool resumed = false;
			for (const auto& entry : peer.Trace()) if (entry["kind"] == "acp.request" && (entry["value"].value("method", "") == "session/load" || entry["value"].value("method", "") == "session/resume"))
			{ resumed = true; UAM_ASSERT_EQ(entry["value"]["params"]["sessionId"], nlohmann::json(peer.native_session)); }
			UAM_ASSERT(resumed);
		}
		(void)fixture.BlobPath();
		fixture.AssertOriginalSource();
	}
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && UAM_ENABLE_RUNTIME_CLAUDE_CLI && UAM_ENABLE_RUNTIME_OPENCODE_CLI && defined(__APPLE__)
namespace
{
	struct OpenCodeImageControlsR81 : OpenCodeOwnedImageFixtureR80
	{
		void Control(const std::string& variant)
		{
			UAM_ASSERT(uam::io::WriteTextFile(peer_root.root / "image-control.json", nlohmann::json{{"variant", variant}}.dump()));
		}
		void RequireRetry()
		{
			std::string error;
			if (!fixture.BeginRetry(error)) throw uam_test::TestFailure("OpenCode image-route prerequisite after genuine ACP/HTTP bootstrap: " + error);
			ObserveWriter();
			UAM_ASSERT(error.empty() && fixture.session->saved_image_retry);
		}
		std::size_t Http(const std::string& kind, const std::string& route) const
		{
			std::size_t count = 0;
			for (const auto& entry : Trace())
			{
				if (entry["kind"] != kind) continue;
				const auto path = entry["value"].value("path", "");
				if (path.substr(0, path.find('?')) == route) ++count;
			}
			return count;
		}
		void WaitHeld()
		{
			const auto permission = fixture.session->saved_image_retry;
			UAM_ASSERT(permission);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
			while (Http("http.request", "/config/providers") == 0 && std::chrono::steady_clock::now() < deadline)
			{
				Tick();
				UAM_ASSERT_EQ(fixture.session->saved_image_retry, permission);
				UAM_ASSERT(!fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery && fixture.app.claude_input_tasks.empty() && Prompts().empty());
			}
			UAM_ASSERT_EQ(Http("http.request", "/config/providers"), std::size_t{1});
			UAM_ASSERT_EQ(Http("http.response", "/config/providers"), std::size_t{0});
			UAM_ASSERT_EQ(fixture.session->saved_image_retry, permission);
			UAM_ASSERT(!fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery && Prompts().empty());
		}
		void Release() { UAM_ASSERT(uam::io::WriteTextFile(peer_root.root / "release-image-evidence", "release actual outstanding response")); }
		void Refused()
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
			while ((fixture.session->saved_image_retry || fixture.session->processing) && fixture.session->last_error.empty() && std::chrono::steady_clock::now() < deadline) Tick();
			UAM_ASSERT(!fixture.session->saved_image_retry && !fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery);
			UAM_ASSERT(!fixture.session->last_error.empty() && Prompts().empty());
		}
		void PermissionStillReady()
		{
			std::string error;
			UAM_ASSERT(fixture.session->running && fixture.session->session_ready);
			UAM_ASSERT_EQ(fixture.app.opencode_native_activity->BeforeDispatch(fixture.app, *fixture.session, fixture.Branch(), error), uam::OpenCodeNativeGateState::Ready);
			UAM_ASSERT(error.empty());
		}
		std::shared_ptr<uam::ClaudeInputPreparation> FinishedWorker()
		{
			WaitHeld(); Release();
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
			while (!fixture.session->claude_input_preparation && std::chrono::steady_clock::now() < deadline)
			{
				(void)fixture.app.opencode_native_activity->Poll(fixture.app);
				(void)uam::FlushPendingChatSaves(fixture.app);
				(void)uam::acp_detail::SendQueuedPromptIfReady(fixture.app, *fixture.session, fixture.Branch());
				std::this_thread::yield();
			}
			UAM_ASSERT(fixture.session->claude_input_preparation);
			const auto state = fixture.session->claude_input_preparation->state;
			// Stop owner admission polling while this genuine worker finishes; never construct a prepared result.
			while (!state->finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
			UAM_ASSERT(state->finished.load(std::memory_order_acquire) && state->error.empty());
			UAM_ASSERT(state->message.is_object() && state->message["message"]["content"].is_array());
			UAM_ASSERT_EQ(std::ranges::count_if(state->message["message"]["content"], [](const nlohmann::json& part) { return part.is_object() && part.value("type", "") == "image"; }), std::ptrdiff_t{1});
			UAM_ASSERT(!fixture.session->pending_prompt_delivery && Prompts().empty());
			return state;
		}
		void EmitNativeConfig()
		{
			UAM_ASSERT(uam::io::WriteTextFile(peer_root.root / "emit-config.json", nlohmann::json{{"model", "fixture-provider/text-model"}}.dump()));
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			bool flushed = false;
			while (!flushed && std::chrono::steady_clock::now() < deadline)
			{
				for (const auto& entry : Trace())
					if (entry["kind"] == "acp.notification.flushed")
					{
						UAM_ASSERT_EQ(entry["value"]["params"]["sessionId"], nlohmann::json(native_session));
						UAM_ASSERT_EQ(entry["value"]["params"]["update"]["sessionUpdate"], nlohmann::json("config_option_update"));
						flushed = true;
					}
				std::this_thread::yield(); // No runtime/save polling can consume the finished worker or pending packet here.
			}
			UAM_ASSERT(flushed && Prompts().empty());
		}
		void DrainAcceptedRefusal()
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
			do
			{
				Tick();
				UAM_ASSERT(Prompts().empty());
				if (!fixture.session->last_error.empty()) break;
				if (!fixture.session->processing && fixture.session->queued_prompt.empty() && !fixture.session->pending_local_launch &&
				    !fixture.session->pending_agent_input && !fixture.session->local_stop_pending && !fixture.session->worktree_launch_pending &&
				    !fixture.session->reconnect_pending && fixture.session->prompt_request_id == 0 && fixture.app.local_acp_launch_tasks.empty() &&
				    !fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.branch_id)) break;
			} while (std::chrono::steady_clock::now() < deadline);
		}
	};
}

UAM_TEST(OpenCodeSavedImageEvidenceRefusesUnknownNativeModelWithoutPoisoningPermissions)
{
	for (const std::string variant : {"vision-false", "vision-missing", "vision-string", "selected-missing", "provider-mismatch", "record-mismatch", "duplicate-provider", "http-error", "malformed", "unhealthy", "version-mismatch"})
	{
		OpenCodeImageControlsR81 peer;
		peer.Control(variant);
		peer.RequireRetry();
		const bool health_failure = variant == "unhealthy" || variant == "version-mismatch";
		if (health_failure)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
			while (peer.Http("http.request", "/global/health") == 0 && std::chrono::steady_clock::now() < deadline) { peer.Tick(); UAM_ASSERT(peer.Prompts().empty() && peer.fixture.app.claude_input_tasks.empty()); }
			UAM_ASSERT_EQ(peer.Http("http.request", "/global/health"), std::size_t{1});
		}
		else { peer.WaitHeld(); peer.Release(); }
		peer.Refused();
		peer.PermissionStillReady(); // Image failure is not a failed permission snapshot/relay.
		UAM_ASSERT(peer.fixture.app.claude_input_tasks.empty());
		const auto queries = peer.Http("http.request", "/config/providers");
		if (health_failure) UAM_ASSERT_EQ(queries, std::size_t{0});
		for (int poll = 0; poll < 3; ++poll) peer.Tick();
		UAM_ASSERT_EQ(peer.Http("http.request", "/config/providers"), queries);
		UAM_ASSERT(peer.Prompts().empty() && !peer.fixture.session->saved_image_retry);
		peer.fixture.AssertOriginalSource();
	}
}

UAM_TEST(OpenCodeBlockedImageEvidenceRevokesCancelStopAndChangedSourceWithoutReplay)
{
	for (const std::string variant : {"cancel", "stop", "root", "incarnation", "declaration"})
	{
		OpenCodeImageControlsR81 peer;
		auto& fixture = peer.fixture;
		peer.RequireRetry(); peer.WaitHeld();
		const auto root = fixture.app.data_root;
		const auto incarnation = fixture.Branch().metadata_incarnation;
		const auto declarations = fixture.Branch().messages[2].attachments;
		if (variant == "cancel") { std::string error; UAM_ASSERT(uam::CancelAcpTurn(fixture.app, fixture.branch_id, &error)); }
		else if (variant == "stop") UAM_ASSERT(uam::StopAcpSession(fixture.app, fixture.branch_id));
		else
		{
			if (variant == "root") { fixture.app.data_root = fixture.temp.root / "different-owned-root"; fs::create_directories(fixture.app.data_root); }
			else if (variant == "incarnation") fixture.Branch().metadata_incarnation = std::make_shared<const std::uint8_t>(0);
			else fixture.Branch().messages[2].attachments.front().name += " changed before HTTP result";
			(void)uam::acp_detail::SendQueuedPromptIfReady(fixture.app, *fixture.session, fixture.Branch());
		}
		UAM_ASSERT(!fs::exists(peer.peer_root.root / "release-image-evidence"));
		UAM_ASSERT_EQ(peer.Http("http.response", "/config/providers"), std::size_t{0}); // Public revocation returned while the actual HTTP response was still held.
		UAM_ASSERT(!fixture.session->saved_image_retry && !fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery && peer.Prompts().empty());
		peer.Release();
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (variant != "stop" && peer.Http("http.response", "/config/providers") == 0 && std::chrono::steady_clock::now() < deadline) { peer.Tick(); UAM_ASSERT(peer.Prompts().empty()); }
		if (variant != "stop") UAM_ASSERT_EQ(peer.Http("http.response", "/config/providers"), std::size_t{1}); // A cancelled client may have closed; this is a genuine late server response attempt.
		for (int poll = 0; poll < 3; ++poll) peer.Tick();
		UAM_ASSERT(peer.Prompts().empty() && !fixture.session->saved_image_retry && fixture.app.claude_input_tasks.empty());
		UAM_ASSERT_EQ(peer.Http("http.request", "/config/providers"), std::size_t{1});
		fixture.app.data_root = root; // Restore only for source/asset checks and teardown, never another Retry/admission.
		fixture.Branch().metadata_incarnation = incarnation;
		fixture.Branch().messages[2].attachments = declarations;
		fixture.AssertOriginalSource();
	}
}

UAM_TEST(OpenCodeActualNativeConfigDriftRevokesFinishedImageAndDurablePacket)
{
	for (const bool durable : {false, true})
	{
		OpenCodeImageControlsR81 peer;
		auto& fixture = peer.fixture;
		peer.RequireRetry();
		const auto state = peer.FinishedWorker();
		const auto tasks = fixture.app.claude_input_tasks.size();
		const auto queries = peer.Http("http.request", "/config/providers");
		auto completions = std::make_shared<std::atomic<int>>(0);
		if (durable)
		{
			UAM_ASSERT(fixture.session->defer_prompt_save && !fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.branch_id));
			UAM_ASSERT(uam::acp_detail::SendQueuedPromptIfReady(fixture.app, *fixture.session, fixture.Branch()));
			const auto packet = fixture.session->pending_prompt_delivery;
			UAM_ASSERT(packet && fixture.session->claude_input_preparation && fixture.session->claude_input_preparation->state == state && peer.Prompts().empty());
			UAM_ASSERT(uam::DeferChatSaveResult(fixture.app, fixture.branch_id, [completions](bool) { ++*completions; }));
			UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.branch_id));
		}
		peer.EmitNativeConfig();
		(void)uam::PollAllAcpSessions(fixture.app); // Drain actual native bytes before final admission; config handler itself rechecks queued delivery.
		UAM_ASSERT_EQ(fixture.session->current_model_id, std::string("fixture-provider/text-model"));
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		while ((fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.branch_id) || fixture.session->saved_image_retry) && std::chrono::steady_clock::now() < deadline) peer.Tick();
		if (durable) UAM_ASSERT_EQ(completions->load(), 1);
		UAM_ASSERT(state->cancel.stop_requested() && peer.Prompts().empty());
		UAM_ASSERT(!fixture.session->saved_image_retry && !fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery && !fixture.session->last_error.empty());
		UAM_ASSERT(fixture.app.claude_input_tasks.size() <= tasks);
		UAM_ASSERT_EQ(peer.Http("http.request", "/config/providers"), queries);
		peer.PermissionStillReady();
		fixture.AssertOriginalSource();
	}
}

UAM_TEST(OpenCodeOrdinaryTextAndRecordedSourceRefusalKeepImageOptInScoped)
{
	for (const std::string variant : {"text", "direct-owned", "saved-opaque", "saved-mixed"})
	{
		OpenCodeImageControlsR81 peer;
		auto& fixture = peer.fixture;
		const auto declarations = fixture.Branch().messages[2].attachments;
		std::string error;
		bool accepted = false;
		if (variant == "text")
		{
			accepted = uam::SendAcpPrompt(fixture.app, fixture.branch_id, "Ordinary fresh text must not resend retained history images.", {}, {}, false, &error);
			UAM_ASSERT(accepted && error.empty());
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
			while (peer.Prompts().empty() && std::chrono::steady_clock::now() < deadline) peer.Tick();
			const auto prompts = peer.Prompts();
			UAM_ASSERT_EQ(prompts.size(), std::size_t{1});
			UAM_ASSERT_EQ(peer.ObservedPrompts(), prompts);
			std::string text;
			for (const auto& part : prompts.front()["params"]["prompt"]) { UAM_ASSERT_EQ(part.value("type", ""), std::string("text")); text += part.value("text", ""); }
			UAM_ASSERT(text.find("Ordinary fresh text must not resend retained history images.") != std::string::npos);
		}
		else
		{
			if (variant == "direct-owned") accepted = uam::SendAcpPrompt(fixture.app, fixture.branch_id, "Raw recorded metadata must not borrow saved Retry.", {}, declarations, false, &error);
			else
			{
				if (variant == "saved-opaque") { auto opaque = BranchOpaqueReference(); opaque.kind = "image"; opaque.mime_type = "image/png"; fixture.Branch().messages[2].attachments = {opaque}; }
				else
				{
					const auto file = fixture.temp.root / "mixed-real-image.png";
					UAM_ASSERT(uam::io::WriteBinaryFile(file, fixture.expected_original));
					fixture.Branch().messages[2].attachments.front().path = file.string();
				}
				accepted = fixture.BeginRetry(error);
			}
			if (accepted) peer.DrainAcceptedRefusal();
			UAM_ASSERT(!error.empty() || !fixture.session->last_error.empty());
			UAM_ASSERT(peer.Prompts().empty());
			if (variant == "saved-mixed") UAM_ASSERT_EQ(uam_test::ReadFile(fixture.temp.root / "mixed-real-image.png"), fixture.expected_original);
			fixture.Branch().messages[2].attachments = declarations;
		}
		UAM_ASSERT(!fixture.session->saved_image_retry && !fixture.session->claude_input_preparation && !fixture.session->pending_prompt_delivery && fixture.app.claude_input_tasks.empty());
		UAM_ASSERT_EQ(peer.Http("http.request", "/config/providers"), std::size_t{0});
		UAM_ASSERT_EQ(peer.Http("http.request", "/global/health"), std::size_t{0});
		UAM_ASSERT_EQ(fixture.Branch().messages[2].attachments, declarations);
		(void)fixture.BlobPath();
		fixture.AssertOriginalSource();
	}
}
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && UAM_ENABLE_RUNTIME_CLAUDE_CLI && UAM_ENABLE_RUNTIME_CODEX_CLI && defined(__APPLE__)
UAM_TEST(ManualCompactionSerializerRecognizesRealOwnedIdleNativeCodexSession)
{
	// Reuse the existing isolated public setup peer; do not call Retry, Send or any image worker.
	CodexOwnedRetryFixtureR75 peer;
	auto& fixture = peer.fixture;
	auto& app = fixture.app;
	auto& chat = fixture.Branch();
	const auto* session = fixture.session;
	const fs::path executable = peer.peer_root.root / "codex";
	UAM_ASSERT(fs::is_regular_file(executable) && !fs::is_symlink(executable));
	UAM_ASSERT((fs::status(executable).permissions() & fs::perms::owner_exec) != fs::perms::none);
	UAM_ASSERT(fs::is_regular_file("/usr/bin/python3"));
	const char* path = std::getenv("PATH");
	UAM_ASSERT(path && std::string(path) == peer.peer_root.root.string());
	UAM_ASSERT(!app.provider_model_catalog && session && session->running && session->initialized && session->session_ready && !session->processing);
	UAM_ASSERT(session->pending_request_methods.empty() && session->queued_user_prompts.empty() && session->queued_prompt.empty());
	UAM_ASSERT(session->child_pid > 0 && session->process_execution_host_id == "local");
	UAM_ASSERT(!chat.imported_read_only && !chat.side_cleanup_requested && chat.acp_queued_prompts.empty() && !chat.acp_queue_held);
	UAM_ASSERT_EQ(session->provider_id, std::string("codex-cli"));
	UAM_ASSERT_EQ(chat.native_session_id, peer.native_thread);
	UAM_ASSERT_EQ(session->session_id, peer.native_thread);
	UAM_ASSERT_EQ(session->codex_thread_id, peer.native_thread);
	UAM_ASSERT_EQ(session->current_model_id, std::string("gpt-5.4"));
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(chat.provider_id);
	UAM_ASSERT(runtime.SupportsAcpCompaction());
	std::string method;
	const auto compact = runtime.OnAcpBuildCompact(*session, 0, method); // Pure existing adapter construction, no request ID reservation or wire.
	UAM_ASSERT_EQ(method, std::string("thread/compact/start"));
	UAM_ASSERT_EQ(compact.at("params").at("threadId"), nlohmann::json(peer.native_thread));
	peer.AssertNoTurn();
	const int next_request = session->next_request_id;
	const auto pending = session->pending_request_methods;
	const auto messages = chat.messages;
	const auto trace = peer.Trace(); // Constructor checked real request/reply IDs and parsed native thread ownership.
	const auto wire = *fixture.wire;
	for (const auto& state : {uam::StateSerializer::Serialize(app), uam::StateSerializer::SerializeFingerprint(app)})
	{
		const auto row = std::ranges::find_if(state.at("chats"), [&](const nlohmann::json& value) { return value.value("id", "") == chat.id; });
		UAM_ASSERT(row != state.at("chats").end());
		const auto& native = row->at("acpSession");
		UAM_ASSERT_EQ(native.at("sessionId"), nlohmann::json(peer.native_thread));
		UAM_ASSERT_EQ(native.at("providerId"), nlohmann::json("codex-cli"));
		const auto projection = native.value("manualCompaction", nlohmann::json::object());
		// First feature oracle: false unavailable stub must fail here after all genuine setup prerequisites.
		UAM_ASSERT(projection.value("available", false));
		UAM_ASSERT_EQ(projection.at("providerId"), nlohmann::json(chat.provider_id));
		UAM_ASSERT_EQ(projection.at("modelId"), nlohmann::json(chat.model_id)); // Selected chat anchor, not parsed-model alias inference.
		UAM_ASSERT_EQ(projection.at("nativeSessionId"), nlohmann::json(peer.native_thread));
	}
	UAM_ASSERT_EQ(session->next_request_id, next_request);
	UAM_ASSERT_EQ(session->pending_request_methods, pending);
	UAM_ASSERT_EQ(chat.messages, messages);
	UAM_ASSERT_EQ(*fixture.wire, wire);
	UAM_ASSERT_EQ(peer.Trace(), trace);
	peer.AssertNoTurn();
	fixture.AssertOriginalSource();
}
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && UAM_ENABLE_RUNTIME_CLAUDE_CLI && UAM_ENABLE_RUNTIME_CODEX_CLI && defined(__APPLE__)
UAM_TEST(ManualCompactionProjectionRejectsReadOnlyIdentityDriftBusyAndStoppingWithoutSideEffects)
{
	CodexOwnedRetryFixtureR75 peer; // Genuine public private app-server setup and parsed thread; no installed provider.
	auto& fixture = peer.fixture;
	auto& app = fixture.app;
	auto& chat = fixture.Branch();
	auto& session = *fixture.session;
	UAM_ASSERT(session.running && session.session_ready && !session.processing && !app.provider_model_catalog);
	UAM_ASSERT_EQ(chat.native_session_id, peer.native_thread);
	const auto projection = [&]
	{
		const auto next = session.next_request_id;
		const auto pending = session.pending_request_methods;
		const auto messages = chat.messages;
		const auto wire = *fixture.wire;
		const auto state = uam::StateSerializer::Serialize(app);
		const auto row = std::ranges::find_if(state.at("chats"), [&](const nlohmann::json& value) { return value.value("id", "") == chat.id; });
		UAM_ASSERT(row != state.at("chats").end());
		const auto value = row->at("acpSession").value("manualCompaction", nlohmann::json::object());
		UAM_ASSERT_EQ(session.next_request_id, next);
		UAM_ASSERT_EQ(session.pending_request_methods, pending);
		UAM_ASSERT_EQ(chat.messages, messages);
		UAM_ASSERT_EQ(*fixture.wire, wire);
		return value.value("available", false);
	};
	UAM_ASSERT(projection()); // Same true producer prerequisite as the separately established false-stub RED.
	const auto initial_trace = peer.Trace();
	chat.imported_read_only = true;
	UAM_ASSERT(!projection());
	chat.imported_read_only = false;
	const auto native = chat.native_session_id;
	chat.native_session_id = PlatformServicesFactory::Instance().process_service.GenerateUuid();
	UAM_ASSERT(chat.native_session_id != native); // Negative source drift only; never grants input/native identity.
	UAM_ASSERT(!projection());
	chat.native_session_id = native;
	UAM_ASSERT(projection());
	UAM_ASSERT_EQ(peer.Trace(), initial_trace);
	peer.AssertNoTurn();

	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(app, fixture.branch_id, "Hold a real private ordinary turn to test compaction availability.", &error));
	UAM_ASSERT(error.empty());
	const auto input = peer.DeliveredTurn(); // Actual wire+peer equality and pending turn/start; peer performs no inference.
	UAM_ASSERT(session.processing && session.prompt_request_id == input.at("id").get<int>());
	UAM_ASSERT(!projection());
	const auto busy_trace = peer.Trace();
	UAM_ASSERT(!projection());
	UAM_ASSERT_EQ(peer.Trace(), busy_trace);
	(void)uam::StopAcpSession(app, fixture.branch_id); // Real Stop; pending Stop is also unavailable.
	UAM_ASSERT(!session.running || uam::AcpStopInProgress(app, fixture.branch_id));
	UAM_ASSERT(!projection());
	fixture.AssertOriginalSource();
	// Existing fixture teardown owns actual process/handle/worker cleanup; no readiness/finished/authority flag is assigned here.
}
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && defined(__APPLE__)
namespace
{
	// Reuses the real native-asset publisher and branch fixture; no provider-private API.
	struct GeminiOwnedImagePeer : BranchNativeOriginalFixture
	{
		std::string branch_id, branch_key, source_primary, skill_file;
		const std::string skill_snapshot = "CAPTURED GEMINI RETRY SKILL\n";
		uam::AcpSessionState* session = nullptr;
		std::unique_ptr<uam_test::ScopedEnvVar> private_path, private_native_home, private_system_settings, private_system_defaults;

		explicit GeminiOwnedImagePeer(bool advertise_images = true, bool warm = true)
		{
			try
			{
				app.settings.central_provider_configuration = {};
				app.settings.central_provider_configuration.enabled = false;
				app.settings.central_provider_configuration.uam_control_enabled = false;
				app.settings.execution_hosts = {uam::execution_hosts::LocalHost()};
				app.settings.mcp_servers.clear();
				app.settings.host_scoped_settings.clear();
				app.settings.git_enabled = app.settings.git_auto_worktree = app.settings.svn_enabled = false;
				app.settings.computer_use_globally_enabled = false;
				auto* source = ChatDomainService().FindChatById(app, source_id);
				UAM_ASSERT(source && source->messages.size() == 3);
				source->memory_enabled = source->uam_control_enabled = source->computer_use_enabled = false;
				source->model_id = "gemini-private-image-model";
				source->reasoning_effort.clear();
				source->service_tier.clear();
				source->service_tier_explicit = false;
				source->approval_mode = "default";
				source->command_safety_tier = "off";
				source->messages[2].attachments = source->messages[1].attachments;
				skill_file = (temp.root / "captured-only-skill.md").string();
				source->messages[2].markdown_store_files = {skill_file};
				source->messages[2].markdown_store_prompt_blocks = {skill_snapshot};
				UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *source));
				source_messages = source->messages;
				source_primary = uam_test::ReadFile(AppPaths::UamChatFilePath(app.data_root, source_id));
				const auto executable = temp.root / "gemini";
				UAM_ASSERT(uam::io::WriteTextFile(temp.root / "image-capability", advertise_images ? "yes" : "no"));
				UAM_ASSERT(uam::io::WriteTextFile(executable, R"PY(#!/usr/bin/python3 -I
import json, os, select, sys, uuid
if sys.argv[1:] == ["--version"]:
    print("0.55.1")
    sys.exit(0)
if "--acp" not in sys.argv[1:]:
    raise RuntimeError("Only private Gemini ACP is supported")
root = os.path.dirname(os.path.abspath(__file__))
native = str(uuid.uuid4())
traffic, buffer, prompt, fenced, mode_emitted = [], b"", None, False, False
def record(kind, value):
    traffic.append({"kind": kind, "value": value})
    with open(os.path.join(root, "trace.tmp"), "w", encoding="utf-8") as stream:
        json.dump(traffic, stream, separators=(",", ":"))
    os.replace(os.path.join(root, "trace.tmp"), os.path.join(root, "trace.json"))
def reply(identifier, result):
    response = {"jsonrpc": "2.0", "id": identifier, "result": result}
    record("response", response)
    print(json.dumps(response, separators=(",", ":")), flush=True)
record("started", {"native": native})
while True:
    readable, _, _ = select.select([sys.stdin.buffer], [], [], 0.005)
    if readable:
        data = os.read(sys.stdin.fileno(), 65536)
        if not data:
            record("eof", {})
            break
        buffer += data
        if len(buffer) > 4 * 1024 * 1024:
            raise RuntimeError("Private ACP bound exceeded")
        while b"\n" in buffer:
            line, buffer = buffer.split(b"\n", 1)
            if not line.strip(): continue
            request = json.loads(line)
            record("request", request)
            method, params = request.get("method", ""), request.get("params", {})
            if method == "initialize":
                with open(os.path.join(root, "image-capability"), encoding="utf-8") as stream:
                    images = stream.read() == "yes"
                reply(request["id"], {"protocolVersion": 1,
                    "agentInfo": {"name": "gemini", "title": "Private Gemini ACP", "version": "0.55.1"},
                    "agentCapabilities": {"loadSession": True, "promptCapabilities": {"image": True} if images else {}}})
            elif method == "initialized" or method == "session/cancel":
                continue
            elif method == "session/new":
                reply(request["id"], {"sessionId": native,
                    "models": {"currentModelId": "gemini-private-image-model", "availableModels": [{"modelId": "gemini-private-image-model", "name": "Private model"}]},
                    "modes": {"currentModeId": "default", "availableModes": [{"id": "default", "name": "Default"}, {"id": "plan", "name": "Plan"}]}})
            elif method == "session/prompt":
                if params.get("sessionId") != native or prompt is not None:
                    raise RuntimeError("Wrong native owner or duplicate input")
                prompt = request["id"]
            elif method in ["session/set_mode", "session/set_model", "session/set_config_option"]:
                if params.get("sessionId") != native: raise RuntimeError("Foreign setup owner")
                reply(request["id"], {})
            else:
                raise RuntimeError("Unexpected native request: " + method)
    elif os.path.isfile(os.path.join(root, "request-fence")) and not fenced:
        # Written only after the actual C++ writer Flush. Drain readable input before acknowledging.
        if buffer: raise RuntimeError("Incomplete native frame at fence")
        fenced = True
        record("input-fence", {})
    if not mode_emitted and os.path.isfile(os.path.join(root, "emit-mode")):
        with open(os.path.join(root, "emit-mode"), encoding="utf-8") as stream:
            if stream.read() != "plan": raise RuntimeError("Only the advertised private Plan mode may be emitted")
        notification = {"jsonrpc": "2.0", "method": "session/update", "params": {"sessionId": native,
            "update": {"sessionUpdate": "current_mode_update", "currentModeId": "plan"}}}
        print(json.dumps(notification, separators=(",", ":")), flush=True)
        record("mode.notification.flushed", notification)
        mode_emitted = True
    if prompt is not None and os.path.isfile(os.path.join(root, "finish-prompt")):
        reply(prompt, {"stopReason": "end_turn"})
        prompt = None
)PY"));
				fs::permissions(executable, fs::perms::owner_all);
				private_path = std::make_unique<uam_test::ScopedEnvVar>("PATH", temp.root.string());
				// Isolate every native Gemini settings lookup; never inspect the user's provider home.
				private_native_home = std::make_unique<uam_test::ScopedEnvVar>("GEMINI_CLI_HOME", (temp.root / "gemini-home").string());
				private_system_settings = std::make_unique<uam_test::ScopedEnvVar>("GEMINI_CLI_SYSTEM_SETTINGS_PATH", (temp.root / "system-settings.json").string());
				private_system_defaults = std::make_unique<uam_test::ScopedEnvVar>("GEMINI_CLI_SYSTEM_DEFAULTS_PATH", (temp.root / "system-defaults.json").string());
				if (warm)
				{
					UAM_ASSERT(ChatDomainService().CreateBranchFromMessage(app, source_id, 2, std::string("Describe this edited Gemini original.")));
					branch_id = ChatDomainService().SelectedChatId(app);
					auto reloaded = ChatRepository::LoadLocalChat(app.data_root, branch_id);
					UAM_ASSERT(reloaded && reloaded->messages.size() == 3 && reloaded->native_session_id.empty());
					*ChatDomainService().FindChatById(app, branch_id) = *reloaded;
					branch_key = Branch().messages[2].attachments.at(0).native_asset_key;
					UAM_ASSERT(uam::native_assets::IsKeyForOwner(branch_id, branch_key));
					UAM_ASSERT(!uam::native_assets::IsKeyForOwner(source_id, branch_key));
					const auto original = uam::native_assets::ReadOwned(app.data_root, branch_id, branch_key);
					UAM_ASSERT(original.ok && original.bytes == expected_original);
					UAM_ASSERT(Branch().messages[2].attachments[0].path.empty() && !fs::exists(skill_file));
					std::string error;
					UAM_ASSERT(uam::StartAcpModelDiscovery(app, branch_id, &error, false));
					session = uam::FindAcpSessionForChat(app, branch_id);
					UAM_ASSERT(session && error.empty());
					const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
					do
					{
						Tick();
					} while ((!session->session_ready || !session->pending_request_methods.empty() || app.pending_chat_save_at_by_chat_id.contains(branch_id)) && std::chrono::steady_clock::now() < deadline);
					UAM_ASSERT(session->running && session->initialized && session->session_ready && !session->processing);
					UAM_ASSERT(session->pending_request_methods.empty() && session->last_error.empty());
					UAM_ASSERT_EQ(session->prompt_image_supported, advertise_images);
					UAM_ASSERT(!session->session_id.empty() && session->session_id == Branch().native_session_id);
					UAM_ASSERT(Prompts().empty());
					AssertPreservedSource();
				}
			}
			catch (...)
			{
				session = uam::FindAcpSessionForChat(app, branch_id);
				Close();
				throw;
			}
		}
		~GeminiOwnedImagePeer()
		{
			Close();
		}
		void Close()
		{
			if (!session && !branch_id.empty())
				session = uam::FindAcpSessionForChat(app, branch_id);
			if (session)
				(void)uam::StopAcpSession(app, branch_id);
			// Public exit teardown joins owned input/launch/stop workers before environment unwind.
			uam::FastStopAcpSessionsForExit(app);
		}
		ChatSession& Branch()
		{
			return *ChatDomainService().FindChatById(app, branch_id);
		}
		void Tick()
		{
			(void)uam::PollAllAcpSessions(app);
			(void)uam::FlushPendingChatSaves(app);
			session = branch_id.empty() ? nullptr : uam::FindAcpSessionForChat(app, branch_id);
			std::this_thread::yield();
		}
		nlohmann::json Trace() const
		{
			return nlohmann::json::parse(uam_test::ReadFile(temp.root / "trace.json"));
		}
		std::vector<nlohmann::json> Prompts() const
		{
			std::vector<nlohmann::json> result;
			for (const auto& item : Trace())
				if (item["kind"] == "request" && item["value"].value("method", "") == "session/prompt")
					result.push_back(item["value"]);
			return result;
		}
		void FenceNoInput()
		{
			UAM_ASSERT(session->stdin_writer && session->stdin_writer->Flush(std::chrono::seconds(1)));
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "request-fence", "drain already-written native bytes"));
			bool fenced = false;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			do
			{
				for (const auto& item : Trace())
					if (item["kind"] == "input-fence")
						fenced = true;
				std::this_thread::yield();
			} while (!fenced && std::chrono::steady_clock::now() < deadline);
			UAM_ASSERT(fenced && Prompts().empty());
		}
		void AssertPreservedSource()
		{
			UAM_ASSERT_EQ(uam_test::ReadFile(AppPaths::UamChatFilePath(app.data_root, source_id)), source_primary);
			AssertSource();
			UAM_ASSERT(!fs::exists(skill_file));
		}
	};
} // namespace

UAM_TEST(GeminiSavedBranchRetryDeliversOwnedPngThroughNegotiatedAcp)
{
	GeminiOwnedImagePeer peer;
	std::string error;
	// Baseline should refuse here: Gemini inherits SupportsSavedImageRetry == false.
	if (!uam::RetryLastAcpPrompt(peer.app, peer.branch_id, &error))
	{
		if (error != "Recorded image Retry requires valid local originals and a supported provider. Remove unsupported sources or attach files again.")
			throw uam_test::TestFailure("Gemini saved-image Retry failed at a different prerequisite: " + error);
		throw uam_test::TestFailure("Gemini saved-image Retry unsupported after actual ACP image negotiation and branch-owned PNG reload: " + error);
	}
	UAM_ASSERT(error.empty());
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while (peer.Prompts().empty() && std::chrono::steady_clock::now() < deadline)
		peer.Tick();
	const auto prompts = peer.Prompts();
	UAM_ASSERT_EQ(prompts.size(), std::size_t{1});
	const auto& request = prompts.front();
	UAM_ASSERT_EQ(request["params"]["sessionId"], nlohmann::json(peer.session->session_id));
	UAM_ASSERT_EQ(request["id"], nlohmann::json(peer.session->prompt_request_id));
	std::string text, decoded;
	std::vector<nlohmann::json> images;
	for (const auto& part : request["params"]["prompt"])
	{
		if (part.value("type", "") == "image")
			images.push_back(part);
		else
		{
			UAM_ASSERT_EQ(part.value("type", ""), std::string("text"));
			text += part.value("text", "");
		}
	}
	UAM_ASSERT_EQ(images.size(), std::size_t{1});
	UAM_ASSERT_EQ(images[0]["mimeType"], nlohmann::json("image/png"));
	UAM_ASSERT(uam::base64::Decode(images[0]["data"].get<std::string>(), decoded));
	UAM_ASSERT_EQ(decoded, peer.expected_original);
	UAM_ASSERT(!images[0].contains("uri") && !images[0].contains("path"));
	UAM_ASSERT(text.find("Inspect the recorded original.") != std::string::npos);
	UAM_ASSERT(text.find("Preserve this answer and its source attachment.") != std::string::npos);
	UAM_ASSERT(text.find("Describe this edited Gemini original.") != std::string::npos);
	// Normal skill delivery publishes captured bytes and emits an immutable reference.
	// Decode only this actual native reference, then establish fixture-local ownership before reading it.
	const std::string reference_prefix = "Attached skill: read ";
	const std::string reference_suffix = " before using it; if read is denied or unavailable, do not use this skill.";
	const auto reference_start = text.find(reference_prefix);
	UAM_ASSERT(reference_start != std::string::npos);
	const auto path_start = reference_start + reference_prefix.size();
	const auto path_end = text.find(reference_suffix, path_start);
	UAM_ASSERT(path_end != std::string::npos);
	const auto encoded_path = nlohmann::json::parse(text.substr(path_start, path_end - path_start));
	UAM_ASSERT(encoded_path.is_string());
	const auto resource = fs::path(encoded_path.get<std::string>());
	const auto workspace = fs::path(peer.Branch().workspace_directory);
	const auto resource_root = workspace / ".UAM" / "attached-skills";
	UAM_ASSERT(resource.is_absolute() && !fs::is_symlink(fs::symlink_status(workspace)));
	const auto relative = resource.lexically_relative(resource_root);
	UAM_ASSERT(!relative.empty() && !relative.is_absolute());
	std::size_t components = 0;
	for (const auto& part : relative)
	{
		UAM_ASSERT(part != "." && part != "..");
		++components;
	}
	UAM_ASSERT_EQ(components, std::size_t{2}); // one immutable generation and one resource filename
	UAM_ASSERT_EQ(resource.extension(), fs::path(".md"));
	fs::path checked = workspace;
	for (const auto& part : resource.lexically_relative(workspace))
	{
		checked /= part;
		UAM_ASSERT(!fs::is_symlink(fs::symlink_status(checked)));
	}
	UAM_ASSERT(fs::is_regular_file(resource) && fs::file_size(resource) == peer.skill_snapshot.size());
	UAM_ASSERT_EQ(uam_test::ReadFile(resource), peer.skill_snapshot);
	UAM_ASSERT(request.dump().find(peer.branch_key) == std::string::npos && request.dump().find(peer.source_key) == std::string::npos);
	UAM_ASSERT(uam::io::WriteTextFile(peer.temp.root / "finish-prompt", "ACK actual captured image prompt"));
	const auto finish = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while (peer.session->processing && std::chrono::steady_clock::now() < finish)
		peer.Tick();
	UAM_ASSERT(!peer.session->processing);
	UAM_ASSERT_EQ(peer.Prompts().size(), std::size_t{1});
	const auto saved = ChatRepository::LoadLocalChat(peer.app.data_root, peer.branch_id);
	UAM_ASSERT(saved && saved->messages[2].attachments[0].native_asset_key == peer.branch_key);
	UAM_ASSERT_EQ(saved->messages[2].markdown_store_prompt_blocks, std::vector<std::string>{peer.skill_snapshot});
	peer.AssertPreservedSource();
}

UAM_TEST(GeminiSavedOwnedRetryRefusesMissingNegotiatedImageCapability)
{
	GeminiOwnedImagePeer peer(false);
	const auto before = uam_test::ReadFile(AppPaths::UamChatFilePath(peer.app.data_root, peer.branch_id));
	std::string error;
	const bool accepted = uam::RetryLastAcpPrompt(peer.app, peer.branch_id, &error);
	if (accepted || error.empty())
		throw uam_test::TestFailure("Known unsupported Gemini image Retry admission: accepted=" + std::to_string(accepted) + "; error=" + error + "; processing=" + std::to_string(peer.session->processing) + "; lifecycle=" + peer.session->lifecycle_state);
	UAM_ASSERT(!peer.session->processing && !peer.session->pending_prompt_delivery && !peer.session->saved_image_retry);
	peer.FenceNoInput();
	UAM_ASSERT_EQ(uam_test::ReadFile(AppPaths::UamChatFilePath(peer.app.data_root, peer.branch_id)), before);
	peer.AssertPreservedSource();
}

UAM_TEST(GeminiSavedOwnedRetryRefusesForeignOwnerOriginalWithoutTextFallback)
{
	GeminiOwnedImagePeer peer;
	const auto before = uam_test::ReadFile(AppPaths::UamChatFilePath(peer.app.data_root, peer.branch_id));
	// It is a genuine existing PNG, but the source owner's key cannot authorize input for its branch.
	peer.Branch().messages[2].attachments[0].native_asset_key = peer.source_key;
	std::string error;
	UAM_ASSERT(!uam::RetryLastAcpPrompt(peer.app, peer.branch_id, &error) && !error.empty());
	UAM_ASSERT(!peer.session->processing && !peer.session->pending_prompt_delivery && !peer.session->saved_image_retry);
	peer.FenceNoInput();
	UAM_ASSERT_EQ(uam_test::ReadFile(AppPaths::UamChatFilePath(peer.app.data_root, peer.branch_id)), before);
	peer.AssertPreservedSource();
}
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && defined(__APPLE__)
UAM_TEST(GeminiColdEditedBranchRegenerationNegotiatesAndDeliversOwnedOriginal)
{
	GeminiOwnedImagePeer peer(true, false);
	UAM_ASSERT(peer.app.acp_sessions.empty() && peer.branch_id.empty() && peer.session == nullptr);
	std::string error, warning;
	// Public create + regenerate creates a fresh branch and captures eligibility BEFORE native setup.
	if (!uam::BranchFromMessageAndRetry(peer.app, peer.source_id, 2, std::string("Describe this edited Gemini original."), &peer.branch_id, &error, std::string("gemini-cold-original"), &warning, std::nullopt, nullptr, nullptr, nullptr, std::nullopt, false, true))
		throw uam_test::TestFailure("GeminiColdEditedBranchRegenerationNegotiatesAndDeliversOwnedOriginal public branch/regenerate prerequisite failed: " + error + "; warning=" + warning + "; retainedBranch=" + peer.branch_id);
	UAM_ASSERT(error.empty() && warning.empty() && !peer.branch_id.empty());
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	do
	{
		peer.Tick();
	} while ((!peer.session || !peer.session->initialized || !peer.session->session_ready || peer.session->session_id.empty()) && std::chrono::steady_clock::now() < deadline);
	UAM_ASSERT(peer.session && peer.session->initialized && peer.session->session_ready && peer.session->prompt_image_supported);
	UAM_ASSERT(peer.session->session_id == peer.Branch().native_session_id && !peer.session->session_id.empty());
	UAM_ASSERT(peer.session->session_id != ChatDomainService().FindChatById(peer.app, peer.source_id)->native_session_id);
	std::size_t creates = 0, image_negotiations = 0;
	for (const auto& item : peer.Trace())
	{
		if (item["kind"] == "request" && item["value"].value("method", "") == "session/new")
			++creates;
		if (item["kind"] == "response" && item["value"]["result"].contains("agentCapabilities"))
		{
			UAM_ASSERT_EQ(item["value"]["result"]["agentCapabilities"]["promptCapabilities"]["image"], nlohmann::json(true));
			++image_negotiations;
		}
	}
	UAM_ASSERT_EQ(creates, std::size_t{1});
	UAM_ASSERT_EQ(image_negotiations, std::size_t{1});
	peer.branch_key = peer.Branch().messages[2].attachments.at(0).native_asset_key;
	UAM_ASSERT(uam::native_assets::IsKeyForOwner(peer.branch_id, peer.branch_key));
	const auto delivery_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while (peer.Prompts().empty() && std::chrono::steady_clock::now() < delivery_deadline)
		peer.Tick();
	const auto prompts = peer.Prompts();
	UAM_ASSERT_EQ(prompts.size(), std::size_t{1});
	const auto& request = prompts.front();
	UAM_ASSERT_EQ(request["params"]["sessionId"], nlohmann::json(peer.session->session_id));
	UAM_ASSERT_EQ(request["id"], nlohmann::json(peer.session->prompt_request_id));
	std::string text, decoded;
	std::vector<nlohmann::json> images;
	for (const auto& part : request["params"]["prompt"])
	{
		if (part.value("type", "") == "image")
			images.push_back(part);
		else
		{
			UAM_ASSERT_EQ(part.value("type", ""), std::string("text"));
			text += part.value("text", "");
		}
	}
	UAM_ASSERT_EQ(images.size(), std::size_t{1});
	UAM_ASSERT_EQ(images[0]["mimeType"], nlohmann::json("image/png"));
	UAM_ASSERT(uam::base64::Decode(images[0]["data"].get<std::string>(), decoded));
	UAM_ASSERT_EQ(decoded, peer.expected_original);
	UAM_ASSERT(!images[0].contains("uri") && !images[0].contains("path"));
	UAM_ASSERT(text.find("Inspect the recorded original.") != std::string::npos);
	UAM_ASSERT(text.find("Preserve this answer and its source attachment.") != std::string::npos);
	UAM_ASSERT(text.find("Describe this edited Gemini original.") != std::string::npos);
	// Validate the actual skill reference before any fixture read, as the independent warm test does.
	const std::string reference_prefix = "Attached skill: read ";
	const std::string reference_suffix = " before using it; if read is denied or unavailable, do not use this skill.";
	const auto reference_start = text.find(reference_prefix);
	UAM_ASSERT(reference_start != std::string::npos);
	const auto path_start = reference_start + reference_prefix.size();
	const auto path_end = text.find(reference_suffix, path_start);
	UAM_ASSERT(path_end != std::string::npos);
	const auto encoded_path = nlohmann::json::parse(text.substr(path_start, path_end - path_start));
	UAM_ASSERT(encoded_path.is_string());
	const auto resource = fs::path(encoded_path.get<std::string>());
	const auto workspace = fs::path(peer.Branch().workspace_directory);
	const auto resource_root = workspace / ".UAM" / "attached-skills";
	UAM_ASSERT(resource.is_absolute() && !fs::is_symlink(fs::symlink_status(workspace)));
	const auto relative = resource.lexically_relative(resource_root);
	UAM_ASSERT(!relative.empty() && !relative.is_absolute());
	std::size_t components = 0;
	for (const auto& part : relative)
	{
		UAM_ASSERT(part != "." && part != "..");
		++components;
	}
	UAM_ASSERT_EQ(components, std::size_t{2});
	UAM_ASSERT_EQ(resource.extension(), fs::path(".md"));
	fs::path checked = workspace;
	for (const auto& part : resource.lexically_relative(workspace))
	{
		checked /= part;
		UAM_ASSERT(!fs::is_symlink(fs::symlink_status(checked)));
	}
	UAM_ASSERT(fs::is_regular_file(resource) && fs::file_size(resource) == peer.skill_snapshot.size());
	UAM_ASSERT_EQ(uam_test::ReadFile(resource), peer.skill_snapshot);
	UAM_ASSERT(request.dump().find(peer.branch_key) == std::string::npos && request.dump().find(peer.source_key) == std::string::npos);
	UAM_ASSERT(uam::io::WriteTextFile(peer.temp.root / "finish-prompt", "ACK actual cold image request"));
	const auto finish = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while (peer.session->processing && std::chrono::steady_clock::now() < finish)
		peer.Tick();
	UAM_ASSERT(!peer.session->processing);
	UAM_ASSERT_EQ(peer.Prompts().size(), std::size_t{1});
	const auto saved = ChatRepository::LoadLocalChat(peer.app.data_root, peer.branch_id);
	UAM_ASSERT(saved && saved->messages.size() >= 3 && saved->native_session_id == peer.session->session_id);
	UAM_ASSERT_EQ(saved->messages[2].attachments[0].native_asset_key, peer.branch_key);
	UAM_ASSERT_EQ(saved->messages[2].markdown_store_prompt_blocks, std::vector<std::string>{peer.skill_snapshot});
	peer.AssertPreservedSource();
}

UAM_TEST(GeminiColdEditedBranchMissingImageNegotiationRefusesInputWorkerAndWire)
{
	GeminiOwnedImagePeer peer(false, false);
	UAM_ASSERT(peer.app.acp_sessions.empty() && peer.branch_id.empty() && peer.session == nullptr);
	std::string error, warning;
	// Capturing cold protocol eligibility must allow real negotiation, not fabricate image support.
	if (!uam::BranchFromMessageAndRetry(peer.app, peer.source_id, 2, std::string("Describe this edited Gemini original."), &peer.branch_id, &error, std::string("gemini-cold-no-image"), &warning, std::nullopt, nullptr, nullptr, nullptr, std::nullopt, false, true))
		throw uam_test::TestFailure("GeminiColdEditedBranchMissingImageNegotiationRefusesInputWorkerAndWire public branch/regenerate prerequisite failed: " + error + "; warning=" + warning + "; retainedBranch=" + peer.branch_id);
	UAM_ASSERT(error.empty() && warning.empty() && !peer.branch_id.empty());
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	do
	{
		// These are actual common input-worker records; no private reader/worker hook is installed.
		UAM_ASSERT(peer.app.claude_input_tasks.empty());
		peer.Tick();
		if (peer.session)
			UAM_ASSERT(!peer.session->claude_input_preparation);
	} while ((!peer.session || !peer.session->initialized || !peer.session->session_ready || peer.session->session_id.empty() || peer.session->processing || peer.session->last_error.empty()) && std::chrono::steady_clock::now() < deadline);
	UAM_ASSERT(peer.session && peer.session->initialized && peer.session->session_ready);
	UAM_ASSERT(!peer.session->prompt_image_supported && !peer.session->processing && !peer.session->last_error.empty());
	UAM_ASSERT(!peer.session->session_id.empty() && peer.Branch().native_session_id == peer.session->session_id);
	std::size_t creates = 0, missing_negotiations = 0;
	for (const auto& item : peer.Trace())
	{
		if (item["kind"] == "request" && item["value"].value("method", "") == "session/new")
			++creates;
		if (item["kind"] == "response" && item["value"]["result"].contains("agentCapabilities"))
		{
			UAM_ASSERT(!item["value"]["result"]["agentCapabilities"]["promptCapabilities"].contains("image"));
			++missing_negotiations;
		}
	}
	UAM_ASSERT_EQ(creates, std::size_t{1});
	UAM_ASSERT_EQ(missing_negotiations, std::size_t{1});
	UAM_ASSERT(peer.app.claude_input_tasks.empty() && !peer.session->claude_input_preparation && !peer.session->pending_prompt_delivery);
	peer.FenceNoInput();
	const auto saved = ChatRepository::LoadLocalChat(peer.app.data_root, peer.branch_id);
	UAM_ASSERT(saved && saved->messages.size() >= 3 && saved->parent_chat_id == peer.source_id);
	const auto& attachment = saved->messages[2].attachments.at(0);
	UAM_ASSERT(uam::native_assets::IsKeyForOwner(peer.branch_id, attachment.native_asset_key));
	const auto retained = uam::native_assets::ReadOwned(peer.app.data_root, peer.branch_id, attachment.native_asset_key);
	UAM_ASSERT(retained.ok && retained.bytes == peer.expected_original);
	UAM_ASSERT_EQ(saved->messages[2].markdown_store_prompt_blocks, std::vector<std::string>{peer.skill_snapshot});
	peer.AssertPreservedSource();
}
#endif

#if UAM_ENABLE_RUNTIME_GEMINI_CLI && defined(__APPLE__)
UAM_TEST(GeminiActualNativeModeDriftRevokesFinishedImageAndDurablePacket)
{
	for (const bool durable : {false, true})
	{
		GeminiOwnedImagePeer peer;
		UAM_ASSERT_EQ(peer.session->current_mode_id, std::string("default"));
		std::string error;
		if (!uam::RetryLastAcpPrompt(peer.app, peer.branch_id, &error))
			throw uam_test::TestFailure("Gemini mode-drift image admission prerequisite: " + error);
		UAM_ASSERT(error.empty());
		const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		while (!peer.session->claude_input_preparation && std::chrono::steady_clock::now() < start_deadline)
		{
			peer.Tick();
			if (!peer.Prompts().empty())
				throw uam_test::TestFailure("Gemini mode-drift prerequisite: input escaped before a real image worker could be retained.");
		}
		if (!peer.session->claude_input_preparation)
			throw uam_test::TestFailure("Gemini mode-drift prerequisite: no actual bounded image worker started.");
		const auto state = peer.session->claude_input_preparation->state;
		// Stop owner/save polling. Only the actual existing image worker may advance now.
		const auto finish_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		while (!state->finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < finish_deadline)
			std::this_thread::yield();
		UAM_ASSERT(state->finished.load(std::memory_order_acquire) && state->error.empty());
		UAM_ASSERT(state->message.is_object() && state->message["message"]["content"].is_array());
		std::size_t prepared_images = 0;
		for (const auto& part : state->message["message"]["content"])
		{
			if (part.value("type", "") != "image")
				continue;
			++prepared_images;
			std::string decoded;
			UAM_ASSERT(uam::base64::Decode(part["source"]["data"].get<std::string>(), decoded));
			UAM_ASSERT_EQ(decoded, peer.expected_original);
		}
		UAM_ASSERT_EQ(prepared_images, std::size_t{1});
		UAM_ASSERT(!peer.session->pending_prompt_delivery && peer.Prompts().empty() && state->sealed_message.is_null());
		auto completions = std::make_shared<std::atomic<int>>(0);
		if (durable)
		{
			if (!peer.session->defer_prompt_save)
				throw uam_test::TestFailure("Gemini mode-drift durable prerequisite: ordinary deferred admission was not retained.");
			const auto packet_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
			while (!peer.session->pending_prompt_delivery && std::chrono::steady_clock::now() < packet_deadline)
			{
				// Stop immediately when the real packet exists; never manufacture a packet or save receipt.
				(void)uam::PollAllAcpSessions(peer.app);
				if (!peer.Prompts().empty())
					throw uam_test::TestFailure("Gemini mode-drift durable prerequisite: the native packet escaped before capture.");
				if (!peer.session->pending_prompt_delivery)
					(void)uam::FlushPendingChatSaves(peer.app);
				std::this_thread::yield();
			}
			if (!peer.session->pending_prompt_delivery || state->sealed_message.is_null())
				throw uam_test::TestFailure("Gemini mode-drift durable prerequisite: actual sealed pending delivery was not observed.");
			UAM_ASSERT(peer.session->claude_input_preparation && peer.session->claude_input_preparation->state == state);
			UAM_ASSERT_EQ(state->sealed_method, std::string("session/prompt"));
			UAM_ASSERT_EQ(state->sealed_message["params"]["sessionId"], nlohmann::json(peer.session->session_id));
			UAM_ASSERT(state->sealed_request_id > 0 && peer.session->prompt_request_id == 0);
			UAM_ASSERT(uam::DeferChatSaveResult(peer.app, peer.branch_id, [completions](bool) { ++*completions; }));
		}
		const auto task_count = peer.app.claude_input_tasks.size();
		UAM_ASSERT_EQ(peer.session->current_mode_id, std::string("default"));
		UAM_ASSERT(peer.session->stdout_buffer.empty());
		UAM_ASSERT(uam::io::WriteTextFile(peer.temp.root / "emit-mode", "plan"));
		nlohmann::json witness;
		const auto event_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		do
		{
			for (const auto& item : peer.Trace())
				if (item["kind"] == "mode.notification.flushed")
					witness = item["value"];
			std::this_thread::yield(); // No Poll/Flush can consume the worker or pending native packet.
		} while (witness.is_null() && std::chrono::steady_clock::now() < event_deadline);
		if (witness.is_null())
			throw uam_test::TestFailure("Gemini mode-drift prerequisite: native peer did not flush its actual mode event.");
		UAM_ASSERT_EQ(witness["params"]["sessionId"], nlohmann::json(peer.session->session_id));
		UAM_ASSERT_EQ(witness["params"]["update"]["sessionUpdate"], nlohmann::json("current_mode_update"));
		UAM_ASSERT_EQ(witness["params"]["update"]["currentModeId"], nlohmann::json("plan"));
		// Consume the real pipe frame through the public decoder, without the owner poll's
		// automatic delivery step or its stdout time budget. No native fields/event are fabricated.
		std::string native_frame;
		const auto read_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (native_frame.find('\n') == std::string::npos && std::chrono::steady_clock::now() < read_deadline)
		{
			char bytes[4096];
			const auto count = PlatformServicesFactory::Instance().process_service.ReadStdioProcessStdout(*peer.session, bytes, sizeof(bytes));
			if (count > 0)
				native_frame.append(bytes, static_cast<std::size_t>(count));
			UAM_ASSERT(native_frame.size() <= 32768);
			std::this_thread::yield();
		}
		UAM_ASSERT(!native_frame.empty() && native_frame.back() == '\n' && native_frame.find('\n') == native_frame.size() - 1);
		UAM_ASSERT_EQ(nlohmann::json::parse(native_frame), witness);
		// Ordinary ACP framing removes exactly this newline before the same production decoder.
		UAM_ASSERT(uam::ProcessAcpLineForTests(peer.app, *peer.session, peer.Branch(), native_frame.substr(0, native_frame.size() - 1)));
		UAM_ASSERT_EQ(peer.session->current_mode_id, std::string("plan"));
		UAM_ASSERT(peer.Prompts().empty());
		const auto revoke_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		do
		{
			peer.Tick();
		} while ((peer.session->saved_image_retry || peer.session->claude_input_preparation || peer.session->pending_prompt_delivery || peer.app.pending_chat_save_at_by_chat_id.contains(peer.branch_id)) && std::chrono::steady_clock::now() < revoke_deadline);
		UAM_ASSERT(!peer.session->saved_image_retry && !peer.session->claude_input_preparation && !peer.session->pending_prompt_delivery);
		UAM_ASSERT(!peer.session->processing && !peer.session->last_error.empty() && state->cancel.stop_requested());
		UAM_ASSERT_EQ(peer.session->current_mode_id, std::string("plan"));
		UAM_ASSERT(peer.app.claude_input_tasks.size() <= task_count);
		if (durable)
			UAM_ASSERT_EQ(completions->load(), 1);
		peer.FenceNoInput();
		const auto saved = ChatRepository::LoadLocalChat(peer.app.data_root, peer.branch_id);
		UAM_ASSERT(saved && saved->messages.size() >= 3 && saved->messages[2].attachments[0].native_asset_key == peer.branch_key);
		UAM_ASSERT_EQ(saved->messages[2].markdown_store_prompt_blocks, std::vector<std::string>{peer.skill_snapshot});
		peer.AssertPreservedSource();
	}
}
#endif


#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && UAM_ENABLE_SSH && defined(__APPLE__)
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/chat/message_attachment_sources.h"
#include "remote/runner_proxy.h"
#include "remote/runner_protocol.h"
#include <sys/wait.h>
#include <signal.h>
#include <cerrno>

namespace
{
	struct RemoteClaudeOrdinaryImageCleanup
	{
		uam::AppState& app;
		std::string chat_id;
		fs::path release;
		bool stopped = false;
		pid_t proxy_pid = -1;

		void Tick()
		{
			(void)uam::PollAllAcpSessions(app);
			(void)uam::FlushPendingChatSaves(app);
			ProviderCliCompatibilityService().Poll(app);
		}
		bool Quiet() const
		{
			const auto* session = uam::FindAcpSessionForChat(app, chat_id);
			const auto* chat = ChatDomainService().FindChatById(app, chat_id);
			const bool confirmed = proxy_pid <= 0 || (session && chat && !session->remote_stop_unconfirmed &&
			    (session->stop_outcome == uam::platform::ProcessStopOutcome::Graceful || session->stop_outcome == uam::platform::ProcessStopOutcome::Forced) &&
			    !chat->remote_stop_cleanup_pending && !chat->remote_process_exists);
			return confirmed && !app.provider_context_cleanup_worker && app.memory_extraction_tasks.empty() &&
			       app.permission_review_tasks.empty() && app.turn_checkpoint_tasks.empty() && app.pending_acp_remote_stops.empty() && app.acp_process_stop_tasks.empty() &&
			       app.claude_input_tasks.empty() && app.local_acp_launch_tasks.empty() && app.agent_input_preparation_tasks.empty() &&
			       app.remote_context_tasks.empty() && app.cli_context_preparation_tasks.empty() && app.memory_recall_tasks.empty() &&
			       !app.runtime_cli_version_check_task.running && !app.runtime_cli_version_check_task.worker &&
			       app.runtime_cli_version_check_queue.empty() && app.runtime_cli_install_tasks.empty() &&
			       app.pending_chat_save_tasks.empty() && app.pending_chat_save_at_by_chat_id.empty() &&
			       (!session || (!session->running && !session->remote_stop_pending && !session->remote_stop_unconfirmed && !session->local_stop_pending &&
			           !session->claude_input_preparation && !session->pending_local_launch && !session->pending_agent_input &&
			           !session->pending_prompt_delivery && !session->pending_local_session_receipt && !session->attached_skill_preparation));
		}
		void StopAndDrain()
		{
			if (stopped) return;
			// Allow the real peer to finish if an assertion failed while its result
			// was deliberately withheld. This does not fabricate runtime completion.
			(void)uam::io::WriteTextFile(release, "Release the actual private peer result");
			if (const auto* session = uam::FindAcpSessionForChat(app, chat_id)) proxy_pid = session->child_pid;
			(void)uam::StopAcpSession(app, chat_id); // Original owned proxy/control token, never a new bridge-direct Stop.
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
			while (!Quiet() && std::chrono::steady_clock::now() < deadline)
			{
				Tick();
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			if (!Quiet())
			{
				std::cerr << "FATAL_PRIVATE_REMOTE_CLAUDE_DRAIN: owned runtime/workers did not settle; private environment will not unwind" << std::endl;
				std::abort();
			}
			if (proxy_pid > 0)
			{
				// WNOWAIT cannot accidentally reap a missed child and manufacture
				// successful runtime cleanup. Refuse environment teardown on doubt.
				siginfo_t info{}; errno = 0;
				if (waitid(P_PID, static_cast<id_t>(proxy_pid), &info, WEXITED | WNOHANG | WNOWAIT) != -1 || errno != ECHILD)
				{
					std::cerr << "FATAL_PRIVATE_REMOTE_CLAUDE_PROXY_NOT_REAPED" << std::endl;
					std::abort();
				}
			}
			stopped = true;
		}
		~RemoteClaudeOrdinaryImageCleanup() noexcept
		{
			try { StopAndDrain(); }
			catch (...) { std::cerr << "FATAL_PRIVATE_REMOTE_CLAUDE_CLEANUP" << std::endl; std::abort(); }
		}
	};
}

UAM_TEST(ClaudeRemoteImageSendDeliversExactScopedBytesThroughActualRunner)
{
	using Json = nlohmann::json;
	uam_test::TempDir temp("uam-remote-claude-image-positive");
	const auto bin = temp.root / "bin", profile = temp.root / "private-home";
	const auto workspace = fs::weakly_canonical(temp.root) / "remote-workspace"; // Match actual child cwd through macOS temp aliases.
	fs::create_directories(bin); fs::create_directories(workspace); fs::create_directories(profile / "tmp");
	const auto runner = PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath().parent_path() / "uam-runner";
	UAM_ASSERT(fs::is_regular_file(runner) && fs::equivalent(runner, uam::remote::PackagedRunnerPath()));
	const auto runner_manifest = Json::parse(uam_test::ReadFile(fs::path(runner.string() + ".manifest.json")));
	const auto runner_version = uam::strings::Trim(uam_test::ReadFile(fs::path(runner.string() + ".version")));
	UAM_ASSERT_EQ(runner_version, std::string(uam::constants::kAppVersion).substr(1));
	UAM_ASSERT_EQ(runner_manifest.at("protocolVersion"), Json(uam::remote::kRunnerProtocolVersion));
	const auto fingerprint = runner_manifest.at("sourceFingerprint").get<std::string>();
	UAM_ASSERT(fingerprint.size() == 64 && uam_test::ReadFile(runner).find(fingerprint) != std::string::npos);
	const auto ssh = bin / "ssh", shell = bin / "sh", claude = bin / "claude";
	UAM_ASSERT(uam::io::WriteTextFile(ssh,
	    "#!/bin/sh\ncase \" $* \" in *' private-claude-image-host '*) ;; *) exit 92;; esac\nprintf '%s\\n' \"$*\" >>\"$UAM_TEST_REMOTE_IMAGE_ROOT/ssh-invocations\"\nexec \"$UAM_TEST_RUNNER\" bridge-direct\n"));
	UAM_ASSERT(uam::io::WriteTextFile(shell,
	    "#!/bin/sh\nif [ \"$1\" = '-lc' ]; then shift; exec /bin/sh -c \"$@\"; fi\nexec /bin/sh \"$@\"\n"));
	UAM_ASSERT(uam::io::WriteTextFile(claude, R"REMOTECLAUDE(#!/usr/bin/python3
import json, os, pathlib, signal, sys, time
root = pathlib.Path(os.environ['UAM_TEST_REMOTE_IMAGE_ROOT'])
config = json.loads((root / 'peer-config.json').read_text())
def store(name, value):
    temporary = root / (name + '.tmp')
    with temporary.open('w') as out:
        json.dump(value, out)
        out.flush()
        os.fsync(out.fileno())
    os.replace(temporary, root / name)
def emit(value):
    print(json.dumps(value), flush=True)
if sys.argv[1:] == ['--version']:
    print('2.1.31 (Claude Code)', flush=True)
    sys.exit(0)
def terminate(signum, frame):
    store('peer-exit.json', dict(pid=os.getpid(), signal=signum))
    sys.exit(0)
signal.signal(signal.SIGTERM, terminate)
signal.signal(signal.SIGINT, terminate)
args = sys.argv[1:]
assert '-p' in args and '--resume' not in args
assert args[args.index('--input-format') + 1] == 'stream-json'
assert args[args.index('--output-format') + 1] == 'stream-json'
assert args[args.index('--model') + 1] == config['model']
store('peer-launch.json', dict(pid=os.getpid(), argv=args, cwd=os.getcwd(), executable=sys.argv[0]))
users = 0
for line in sys.stdin:
    message = json.loads(line)
    if message.get('type') == 'control_request':
        request = message['request']
        assert request['subtype'] == 'initialize', request
        assert isinstance(message['request_id'], str) and message['request_id']
        emit(dict(type='control_response', response=dict(subtype='success', request_id=message['request_id'],
            response=dict(commands=[], models=[dict(value=config['model'], displayName='Private image model')]))))
        emit(dict(type='system', subtype='init', session_id=config['native'], model=config['model'],
            permissionMode=args[args.index('--permission-mode') + 1], cwd=os.getcwd()))
        continue
    assert message.get('type') == 'user', message
    users += 1
    assert users == 1
    store('peer-input.json', dict(pid=os.getpid(), raw=line, envelope=message))
    while not (root / 'release-result').exists():
        time.sleep(0.005)
    emit(dict(type='assistant', uuid='dddddddd-dddd-4ddd-8ddd-dddddddddddd', session_id=config['native'],
        parent_tool_use_id=None, message=dict(role='assistant', model=config['model'],
            content=[dict(type='text', text=config['answer'])])))
    emit(dict(type='result', subtype='success', is_error=False, uuid='eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee',
        session_id=config['native'], model=config['model'], result=config['answer'], parent_tool_use_id=None))
store('peer-exit.json', dict(pid=os.getpid(), eof=True))
)REMOTECLAUDE"));
	for (const auto& file : {ssh, shell, claude}) fs::permissions(file, fs::perms::owner_all);
	// All process-owning values below die before these private environment scopes.
	std::vector<std::unique_ptr<uam_test::ScopedEnvVar>> environment;
	const auto env = [&](const std::string& key, const std::string& value)
	{ environment.push_back(std::make_unique<uam_test::ScopedEnvVar>(key, value)); };
	env("PATH", bin.string() + ":/usr/bin:/bin"); env("HOME", profile.string()); env("USERPROFILE", profile.string());
	env("CLAUDE_CONFIG_DIR", (profile / "claude").string()); env("UAM_DATA_DIR", (temp.root / "controller").string());
	env("TMPDIR", (profile / "tmp").string()); env("UAM_TEST_RUNNER", runner.string()); env("UAM_TEST_REMOTE_IMAGE_ROOT", temp.root.string());
	const std::string model = "private-claude-image-model", native = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
	const std::string task = "Describe only this current remote PNG.", answer = "Actual private remote Claude image received.";
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "peer-config.json", Json{{"model", model}, {"native", native}, {"answer", answer}}.dump()));
	std::string image_bytes, old_bytes;
	UAM_ASSERT(uam::base64::Decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==", image_bytes));
	UAM_ASSERT(uam::base64::Decode("R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7", old_bytes));
	UAM_ASSERT(!image_bytes.empty() && old_bytes != image_bytes);
	UAM_ASSERT(uam::io::WriteBinaryFile(workspace / "current.png", image_bytes));
	UAM_ASSERT(uam::io::WriteBinaryFile(workspace / "old.gif", old_bytes));

	uam::AppState app;
	app.data_root = temp.root / "controller"; app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.settings.central_provider_configuration.enabled = app.settings.central_provider_configuration.uam_control_enabled = false;
	app.settings.git_enabled = app.settings.git_auto_worktree = false;
	ExecutionHost host; host.id = "private-remote-claude"; host.label = "Private image host"; host.transport = "ssh";
	host.ssh_alias = "private-claude-image-host"; host.platform = "macos"; host.runner_status = "ready";
	host.runner_version = runner_version; host.runner_protocol_version = uam::remote::kRunnerProtocolVersion;
	const ExecutionHost requested_host = host;
	app.settings.execution_hosts = {host}; uam::execution_hosts::Normalize(app.settings.execution_hosts);
	const auto* selected_host = uam::execution_hosts::Find(app.settings.execution_hosts, requested_host.id);
	UAM_ASSERT(selected_host && selected_host->id == requested_host.id && selected_host->id != uam::execution_hosts::kLocalHostId);
	UAM_ASSERT(selected_host->transport == "ssh" && selected_host->ssh_alias == "private-claude-image-host");
	UAM_ASSERT(uam::execution_hosts::SameConnection(*selected_host, requested_host));
	host = *selected_host;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kClaudeCli, false);
	chat.execution_host_id = host.id; chat.workspace_directory = workspace.string(); chat.model_id = model;
	chat.memory_enabled = chat.uam_control_enabled = false; chat.approval_mode = "default"; chat.command_safety_tier = "safe";
	MessageAttachment old; old.id = "old-image"; old.name = "Retain old GIF"; old.kind = "image"; old.mime_type = "image/gif";
	old.path = "old.gif"; // Ordinary workspace paths have no native-origin metadata.
	UAM_ASSERT(old.source == AttachmentSource::LegacyPath && old.origin_execution_host_id.empty() && uam::attachment_sources::IsPersistable(old, chat.id));
	chat.messages = {{MessageRole::User, "Retain this prior remote context."}, {MessageRole::Assistant, "Retain the earlier answer."}};
	chat.messages.front().attachments = {old};
	UAM_ASSERT(chat.native_session_id.empty() && !chat.remote_process_exists && chat.remote_process_control_token.empty());
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	const auto original = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
	UAM_ASSERT(original && original->messages == chat.messages);
	app.chats.push_back(*original);
	ChatSession unrelated = ChatDomainService().CreateNewChat("", uam::provider_ids::kClaudeCli, false);
	unrelated.workspace_directory = (temp.root / "unrelated-workspace").string(); fs::create_directories(fs::path(unrelated.workspace_directory));
	unrelated.messages = {{MessageRole::User, "Retain unrelated saved history."}};
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, unrelated)); app.chats.push_back(unrelated);
	const auto unrelated_path = AppPaths::UamChatFilePath(app.data_root, unrelated.id);
	const auto unrelated_primary = uam_test::ReadFile(unrelated_path);
	ChatDomainService().SelectChatById(app, chat.id);
	app.composer_text = "Retain the unrelated live draft.";
	const auto prefix = original->messages;
	RemoteClaudeOrdinaryImageCleanup cleanup{app, chat.id, temp.root / "release-result"};
	MessageAttachment current; current.id = "current-image"; current.name = "Current remote PNG"; current.kind = "image";
	current.mime_type = "image/png"; current.path = "current.png";
	UAM_ASSERT(current.source == AttachmentSource::LegacyPath && current.origin_execution_host_id.empty() && uam::attachment_sources::IsPersistable(current, chat.id));
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(app, chat.id, task, {}, {current}, false, &error) && error.empty());
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	bool receipt = false;
	bool saw_pending_request = false, saw_empty_pending_method = false, saw_nonpositive_pending_id = false;
	bool saw_image_worker = false, saw_image_worker_finished = false, saw_image_worker_error = false, saw_prepared_message = false;
	const auto observe_admission = [&]()
	{
		if (const auto* owner = ChatDomainService().FindChatById(app, chat.id))
			for (const auto& pending : owner->remote_pending_requests)
			{
				saw_pending_request = true;
				saw_empty_pending_method = saw_empty_pending_method || pending.method.empty();
				saw_nonpositive_pending_id = saw_nonpositive_pending_id || pending.request_id <= 0;
			}
		if (const auto* runtime = uam::FindAcpSessionForChat(app, chat.id); runtime && runtime->claude_input_preparation)
		{
			const auto& state = runtime->claude_input_preparation->state;
			saw_image_worker = true;
			if (state && state->finished.load(std::memory_order_acquire))
			{
				saw_image_worker_finished = true;
				saw_image_worker_error = saw_image_worker_error || !state->error.empty();
				saw_prepared_message = saw_prepared_message || (!state->message.is_null() && !state->message.empty());
			}
		}
	};
	while (std::chrono::steady_clock::now() < deadline)
	{
		observe_admission();
		cleanup.Tick();
		observe_admission();
		const auto* session = uam::FindAcpSessionForChat(app, chat.id);
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
		if (fs::is_regular_file(temp.root / "peer-input.json") && session && session->session_ready && session->processing &&
		    session->session_id == native && saved && saved->native_session_id == native && saved->remote_turn_reconnect_pending &&
		    saved->remote_prompt_delivery_id.empty() && saved->remote_prompt_delivery_payload.empty() && !session->pending_prompt_delivery &&
		    app.pending_chat_save_at_by_chat_id.empty() && app.pending_chat_save_tasks.empty()) { receipt = true; break; }
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	if (!receipt)
	{
		const auto* runtime = uam::FindAcpSessionForChat(app, chat.id);
		const auto* owner = ChatDomainService().FindChatById(app, chat.id);
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
		// Fixed labels/booleans/counts only: never log tokens, native IDs, payloads,
		// peer envelopes, image bytes, paths, prompt prose or unknown error text.
		const auto error_class = [](const std::string& value)
		{
			if (value.empty()) return "empty";
			if (value == "Remote prompt was not sent because its owned durable receipt was not confirmed.") return "owned_durable_receipt_not_confirmed";
			if (value == "Prompt was not sent because chat history could not be saved.") return "chat_history_save_failed";
			return "other_nonempty";
		};
		std::cerr << "PRIVATE_REMOTE_CLAUDE_RECEIPT_DIAGNOSTIC " << Json{
		    {"session_present", runtime != nullptr}, {"session_running", runtime && runtime->running},
		    {"session_initialized", runtime && runtime->initialized}, {"session_ready", runtime && runtime->session_ready},
		    {"session_processing", runtime && runtime->processing}, {"session_native_id_present", runtime && !runtime->session_id.empty()},
		    {"session_last_error", runtime ? error_class(runtime->last_error) : "missing_session"},
		    {"pending_prompt_delivery", runtime && static_cast<bool>(runtime->pending_prompt_delivery)},
		    {"pending_agent_input", runtime && static_cast<bool>(runtime->pending_agent_input)},
		    {"pending_local_launch", runtime && static_cast<bool>(runtime->pending_local_launch)},
		    {"owner_present", owner != nullptr}, {"owner_rows", owner ? owner->messages.size() : 0},
		    {"owner_queue_held", owner && owner->acp_queue_held}, {"owner_pending_requests", owner ? owner->remote_pending_requests.size() : 0},
		    {"owner_delivery_id_present", owner && !owner->remote_prompt_delivery_id.empty()},
		    {"owner_delivery_payload_present", owner && !owner->remote_prompt_delivery_payload.empty()},
		    {"saved_present", static_cast<bool>(saved)}, {"saved_rows", saved ? saved->messages.size() : 0},
		    {"saved_native_id_present", saved && !saved->native_session_id.empty()},
		    {"saved_remote_reconnect_pending", saved && saved->remote_turn_reconnect_pending},
		    {"saved_delivery_id_present", saved && !saved->remote_prompt_delivery_id.empty()},
		    {"saved_delivery_payload_present", saved && !saved->remote_prompt_delivery_payload.empty()},
		    {"peer_input_exists", fs::is_regular_file(temp.root / "peer-input.json")},
		    {"saw_pending_request", saw_pending_request}, {"saw_empty_pending_method", saw_empty_pending_method},
		    {"saw_nonpositive_pending_id", saw_nonpositive_pending_id},
		    {"saw_image_worker", saw_image_worker}, {"saw_image_worker_finished", saw_image_worker_finished},
		    {"saw_image_worker_error", saw_image_worker_error}, {"saw_prepared_message", saw_prepared_message},
		    {"claude_input_tasks", app.claude_input_tasks.size()}, {"remote_context_tasks", app.remote_context_tasks.size()},
		    {"pending_save_tasks", app.pending_chat_save_tasks.size()}, {"scheduled_saves", app.pending_chat_save_at_by_chat_id.size()},
		    {"pending_remote_stops", app.pending_acp_remote_stops.size()}}.dump() << std::endl;
	}
	UAM_ASSERT(receipt); // Result is still withheld: clearing the real pre-write intent requires actual protocol3 input ACK.
	auto* session = uam::FindAcpSessionForChat(app, chat.id);
	UAM_ASSERT(session && session->running && session->initialized && session->session_ready && session->processing && session->last_error.empty());
	UAM_ASSERT_EQ(session->protocol_kind, std::string("claude-code-stream-json"));
	UAM_ASSERT_EQ(session->process_execution_host_id, host.id); UAM_ASSERT_EQ(session->remote_runner_protocol_version, 3);
	UAM_ASSERT_EQ(session->current_model_id, model); UAM_ASSERT_EQ(session->session_id, native);
	// Claude's ordinary launch does not promise a compatibility cache entry.
	// Request a real exact-host probe and consume it within the existing Send deadline.
	std::string version_error;
	UAM_ASSERT(ProviderCliCompatibilityService().StartProviderVersionCheck(app,
	    uam::provider_ids::kClaudeCli, true, host.id, &version_error) && version_error.empty());
	const auto version_key = CliProviderVersionStateKey(uam::provider_ids::kClaudeCli, host.id);
	bool version_checked = false;
	while (std::chrono::steady_clock::now() < deadline)
	{
		cleanup.Tick();
		const auto found = app.runtime_cli_versions_by_provider_id.find(version_key);
		if (found != app.runtime_cli_versions_by_provider_id.end() && found->second.checked &&
		    !app.runtime_cli_version_check_task.running && !app.runtime_cli_version_check_task.worker)
		{
			version_checked = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT(version_checked);
	const auto version_found = app.runtime_cli_versions_by_provider_id.find(version_key);
	UAM_ASSERT(version_found != app.runtime_cli_versions_by_provider_id.end() && version_found->second.checked);
	const auto version = app.runtime_cli_versions_by_provider_id.at(CliProviderVersionStateKey(uam::provider_ids::kClaudeCli, host.id));
	UAM_ASSERT(version.checked && version.supported && version.check_error.empty() && uam::execution_hosts::SameConnection(version.execution_host, host));
	UAM_ASSERT_EQ(version.installed_version, std::string("2.1.31"));
	const auto launch = Json::parse(uam_test::ReadFile(temp.root / "peer-launch.json"));
	UAM_ASSERT_EQ(launch.at("cwd"), Json(workspace.string())); UAM_ASSERT_EQ(launch.at("executable"), Json(claude.string()));
	const auto input = Json::parse(uam_test::ReadFile(temp.root / "peer-input.json"));
	UAM_ASSERT_EQ(input.at("pid"), launch.at("pid"));
	const auto envelope = input.at("envelope");
	UAM_ASSERT_EQ(envelope.at("type"), Json("user")); UAM_ASSERT(!envelope.contains("session_id"));
	UAM_ASSERT(input.at("raw").get<std::string>().size() <= 2 * 1024 * 1024);
	std::string prompt_text; std::vector<Json> images;
	for (const auto& part : envelope.at("message").at("content"))
	{
		if (part.value("type", "") == "text") prompt_text += part.value("text", "");
		if (part.value("type", "") == "image") images.push_back(part);
	}
	UAM_ASSERT(prompt_text.find(task) != std::string::npos); UAM_ASSERT_EQ(images.size(), std::size_t{1});
	UAM_ASSERT_EQ(images.front().at("source").at("type"), Json("base64"));
	UAM_ASSERT_EQ(images.front().at("source").at("media_type"), Json("image/png"));
	std::string decoded; UAM_ASSERT(uam::base64::Decode(images.front()["source"]["data"].get<std::string>(), decoded));
	UAM_ASSERT_EQ(decoded, image_bytes); UAM_ASSERT(decoded != old_bytes);
	const auto delivered = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
	UAM_ASSERT(delivered && delivered->messages.size() >= prefix.size() + 1);
	UAM_ASSERT_EQ(session->turn_user_message_index, static_cast<int>(prefix.size()));
	const auto& delivered_user = delivered->messages[static_cast<std::size_t>(session->turn_user_message_index)];
	UAM_ASSERT(delivered_user.role == MessageRole::User && !delivered_user.acp_prompt_not_sent);
	UAM_ASSERT_EQ(delivered->messages[0], prefix[0]); UAM_ASSERT_EQ(delivered->messages[1], prefix[1]);
	UAM_ASSERT_EQ(delivered_user.content, task);
	UAM_ASSERT_EQ(delivered_user.attachments.size(), std::size_t{1});
	UAM_ASSERT_EQ(delivered_user.attachments.front().name, current.name); // Ordinary native wire has no filename label.
	UAM_ASSERT(delivered_user.attachments.front().source == AttachmentSource::LegacyPath && delivered_user.attachments.front().origin_execution_host_id.empty());
	UAM_ASSERT_EQ(delivered_user.attachments.front().path, current.path);
	UAM_ASSERT_EQ(delivered->execution_host_id, host.id); // Ordinary path resolution is scoped by the actual saved owner/session host.
	UAM_ASSERT(delivered->remote_process_exists && !delivered->remote_process_control_token.empty());
	UAM_ASSERT_EQ(session->remote_output_delivery_token, delivered->remote_process_control_token);
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "release-result", "Finish the actual peer turn"));
	bool finished = false;
	while (std::chrono::steady_clock::now() < deadline)
	{
		cleanup.Tick();
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
		if (!session->processing && session->session_ready && saved && saved->messages.size() == prefix.size() + 2 &&
		    saved->messages.back().content == answer && app.pending_chat_save_at_by_chat_id.empty() && app.pending_chat_save_tasks.empty()) { finished = true; break; }
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT(finished && session->last_error.empty());
	cleanup.StopAndDrain(); // Must occur before reading final saved state and before private environment teardown.
	UAM_ASSERT(cleanup.proxy_pid > 0 && !session->remote_stop_pending && !session->remote_stop_unconfirmed && !session->local_stop_pending);
	UAM_ASSERT(session->stop_outcome == uam::platform::ProcessStopOutcome::Graceful || session->stop_outcome == uam::platform::ProcessStopOutcome::Forced);
	UAM_ASSERT(!session->running && session->child_pid == -1 && session->stdin_write_fd == -1 && session->stdout_read_fd == -1 && session->stderr_read_fd == -1);
	int proxy_status = 0; errno = 0;
	UAM_ASSERT(waitpid(cleanup.proxy_pid, &proxy_status, WNOHANG) == -1 && errno == ECHILD); // Actual original direct-child proxy was reaped.
	const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
	const auto* live = ChatDomainService().FindChatById(app, chat.id);
	UAM_ASSERT(saved && live && saved->messages == live->messages && saved->native_session_id == native);
	UAM_ASSERT(!saved->remote_stop_cleanup_pending && !live->remote_stop_cleanup_pending && !saved->remote_process_exists && !live->remote_process_exists);
	UAM_ASSERT_EQ(saved->execution_host_id, host.id); UAM_ASSERT_EQ(saved->workspace_directory, workspace.string()); UAM_ASSERT_EQ(saved->model_id, model);
	UAM_ASSERT_EQ(saved->messages.front(), prefix.front()); UAM_ASSERT_EQ(saved->messages.back().content, answer);
	UAM_ASSERT_EQ(uam_test::ReadFile(workspace / "current.png"), image_bytes); UAM_ASSERT_EQ(uam_test::ReadFile(workspace / "old.gif"), old_bytes);
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), chat.id);
	UAM_ASSERT_EQ(app.composer_text, std::string("Retain the unrelated live draft."));
	UAM_ASSERT_EQ(uam_test::ReadFile(unrelated_path), unrelated_primary);
	UAM_ASSERT(uam_test::ReadFile(temp.root / "ssh-invocations").find("private-claude-image-host") != std::string::npos);
	UAM_ASSERT(cleanup.Quiet());
}
#endif


#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && UAM_ENABLE_SSH && defined(__APPLE__)
enum class RemoteOwnedRetryControl
{
	Healthy,
	HealthyIndexed,
	BenignMetadata,
	MissingOriginal,
	EncodedLimit,
	HostConnection,
	ProviderProfile,
	SavedNative,
	PendingRow,
	PendingIncarnation,
	MixedLegacy,
	MixedOpaque
};

static void CheckClaudeRemoteSavedOwnedRetry(RemoteOwnedRetryControl control)
{
	using Json = nlohmann::json;
	uam_test::TempDir temp("uam-remote-claude-owned-retry");
	const auto bin = temp.root / "bin", profile = temp.root / "private-home";
	const auto workspace = fs::weakly_canonical(temp.root) / "remote-workspace"; // Match actual child cwd through macOS temp aliases.
	fs::create_directories(bin); fs::create_directories(workspace); fs::create_directories(profile / "tmp");
	const auto runner = PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath().parent_path() / "uam-runner";
	UAM_ASSERT(fs::is_regular_file(runner) && fs::equivalent(runner, uam::remote::PackagedRunnerPath()));
	const auto runner_manifest = Json::parse(uam_test::ReadFile(fs::path(runner.string() + ".manifest.json")));
	const auto runner_version = uam::strings::Trim(uam_test::ReadFile(fs::path(runner.string() + ".version")));
	UAM_ASSERT_EQ(runner_version, std::string(uam::constants::kAppVersion).substr(1));
	UAM_ASSERT_EQ(runner_manifest.at("protocolVersion"), Json(uam::remote::kRunnerProtocolVersion));
	const auto fingerprint = runner_manifest.at("sourceFingerprint").get<std::string>();
	UAM_ASSERT(fingerprint.size() == 64 && uam_test::ReadFile(runner).find(fingerprint) != std::string::npos);
	const auto ssh = bin / "ssh", shell = bin / "sh", claude = bin / "claude";
	UAM_ASSERT(uam::io::WriteTextFile(ssh,
	    "#!/bin/sh\ncase \" $* \" in *' private-claude-image-host '*) ;; *) exit 92;; esac\nprintf '%s\\n' \"$*\" >>\"$UAM_TEST_REMOTE_IMAGE_ROOT/ssh-invocations\"\nexec \"$UAM_TEST_RUNNER\" bridge-direct\n"));
	UAM_ASSERT(uam::io::WriteTextFile(shell,
	    "#!/bin/sh\nif [ \"$1\" = '-lc' ]; then shift; exec /bin/sh -c \"$@\"; fi\nexec /bin/sh \"$@\"\n"));
	UAM_ASSERT(uam::io::WriteTextFile(claude, R"REMOTECLAUDE(#!/usr/bin/python3
import json, os, pathlib, signal, sys, time
root = pathlib.Path(os.environ['UAM_TEST_REMOTE_IMAGE_ROOT'])
config = json.loads((root / 'peer-config.json').read_text())
def store(name, value):
    temporary = root / (name + '.tmp')
    with temporary.open('w') as out:
        json.dump(value, out)
        out.flush()
        os.fsync(out.fileno())
    os.replace(temporary, root / name)
def emit(value):
    print(json.dumps(value), flush=True)
if sys.argv[1:] == ['--version']:
    print('2.1.31 (Claude Code)', flush=True)
    sys.exit(0)
def terminate(signum, frame):
    store('peer-exit.json', dict(pid=os.getpid(), signal=signum))
    sys.exit(0)
signal.signal(signal.SIGTERM, terminate)
signal.signal(signal.SIGINT, terminate)
args = sys.argv[1:]
assert '-p' in args and '--resume' not in args
assert args[args.index('--input-format') + 1] == 'stream-json'
assert args[args.index('--output-format') + 1] == 'stream-json'
assert args[args.index('--model') + 1] == config['model']
store('peer-launch.json', dict(pid=os.getpid(), argv=args, cwd=os.getcwd(), executable=sys.argv[0]))
users = 0
for line in sys.stdin:
    with (root / 'peer-stdin.jsonl').open('a') as trace:
        trace.write(line)
        trace.flush()
        os.fsync(trace.fileno())
    message = json.loads(line)
    if message.get('type') == 'control_request':
        request = message['request']
        assert request['subtype'] == 'initialize', request
        assert isinstance(message['request_id'], str) and message['request_id']
        emit(dict(type='control_response', response=dict(subtype='success', request_id=message['request_id'],
            response=dict(commands=[], models=[dict(value=config['model'], displayName='Private image model')]))))
        emit(dict(type='system', subtype='init', session_id=config['native'], model=config['model'],
            permissionMode=args[args.index('--permission-mode') + 1], cwd=os.getcwd()))
        continue
    if message.get('type') != 'user':
        store('peer-unexpected-input.json', dict(pid=os.getpid(), raw=line, envelope=message))
    assert message.get('type') == 'user', message
    users += 1
    if users > 2:
        store('peer-extra-input.json', dict(pid=os.getpid(), raw=line, envelope=message))
    assert users in (1, 2)
    store('peer-input.json' if users == 1 else 'peer-retry-input.json', dict(pid=os.getpid(), raw=line, envelope=message))
    while not (root / ('release-result' if users == 1 else 'release-retry-result')).exists():
        time.sleep(0.005)
    emit(dict(type='assistant', uuid='dddddddd-dddd-4ddd-8ddd-dddddddddddd' if users == 1 else 'ffffffff-ffff-4fff-8fff-ffffffffffff', session_id=config['native'],
        parent_tool_use_id=None, message=dict(role='assistant', model=config['model'],
            content=[dict(type='text', text=config['answer'] if users == 1 else config['retry_answer'])])))
    emit(dict(type='result', subtype='success', is_error=False, uuid='eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee' if users == 1 else '11111111-1111-4111-8111-111111111111',
        session_id=config['native'], model=config['model'], result=config['answer'] if users == 1 else config['retry_answer'], parent_tool_use_id=None))
store('peer-exit.json', dict(pid=os.getpid(), eof=True))
)REMOTECLAUDE"));
	for (const auto& file : {ssh, shell, claude}) fs::permissions(file, fs::perms::owner_all);
	// All process-owning values below die before these private environment scopes.
	std::vector<std::unique_ptr<uam_test::ScopedEnvVar>> environment;
	const auto env = [&](const std::string& key, const std::string& value)
	{ environment.push_back(std::make_unique<uam_test::ScopedEnvVar>(key, value)); };
	env("PATH", bin.string() + ":/usr/bin:/bin"); env("HOME", profile.string()); env("USERPROFILE", profile.string());
	env("CLAUDE_CONFIG_DIR", (profile / "claude").string()); env("UAM_DATA_DIR", (temp.root / "controller").string());
	env("TMPDIR", (profile / "tmp").string()); env("UAM_TEST_RUNNER", runner.string()); env("UAM_TEST_REMOTE_IMAGE_ROOT", temp.root.string());
	const std::string model = "private-claude-image-model", native = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
	const std::string task = "Describe only this current remote PNG.", answer = "Actual private remote Claude image received.";
	const std::string retry_task = control == RemoteOwnedRetryControl::EncodedLimit
	    ? std::string(1024 * 1024, '\\') : "Retry this saved retained original only.";
	const std::string retry_answer = "Actual retained remote original received.";
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "peer-config.json", Json{{"model", model}, {"native", native}, {"answer", answer}, {"retry_answer", retry_answer}}.dump()));
	std::string image_bytes, old_bytes;
	UAM_ASSERT(uam::base64::Decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==", image_bytes));
	UAM_ASSERT(uam::base64::Decode("R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7", old_bytes));
	UAM_ASSERT(!image_bytes.empty() && old_bytes != image_bytes);
	UAM_ASSERT(uam::io::WriteBinaryFile(workspace / "current.png", image_bytes));
	UAM_ASSERT(uam::io::WriteBinaryFile(workspace / "old.gif", old_bytes));

	uam::AppState app;
	app.data_root = temp.root / "controller"; app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.settings.central_provider_configuration.enabled = app.settings.central_provider_configuration.uam_control_enabled = false;
	app.settings.git_enabled = app.settings.git_auto_worktree = false;
	ExecutionHost host; host.id = "private-remote-claude"; host.label = "Private image host"; host.transport = "ssh";
	host.ssh_alias = "private-claude-image-host"; host.platform = "macos"; host.runner_status = "ready";
	host.runner_version = runner_version; host.runner_protocol_version = uam::remote::kRunnerProtocolVersion;
	const ExecutionHost requested_host = host;
	app.settings.execution_hosts = {host}; uam::execution_hosts::Normalize(app.settings.execution_hosts);
	const auto* selected_host = uam::execution_hosts::Find(app.settings.execution_hosts, requested_host.id);
	UAM_ASSERT(selected_host && selected_host->id == requested_host.id && selected_host->id != uam::execution_hosts::kLocalHostId);
	UAM_ASSERT(selected_host->transport == "ssh" && selected_host->ssh_alias == "private-claude-image-host");
	UAM_ASSERT(uam::execution_hosts::SameConnection(*selected_host, requested_host));
	host = *selected_host;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kClaudeCli, false);
	chat.execution_host_id = host.id; chat.workspace_directory = workspace.string(); chat.model_id = model;
	chat.memory_enabled = chat.uam_control_enabled = false; chat.approval_mode = "default"; chat.command_safety_tier = "safe";
	MessageAttachment old; old.id = "old-image"; old.name = "Retain old GIF"; old.kind = "image"; old.mime_type = "image/gif";
	old.path = "old.gif"; // Ordinary workspace paths have no native-origin metadata.
	UAM_ASSERT(old.source == AttachmentSource::LegacyPath && old.origin_execution_host_id.empty() && uam::attachment_sources::IsPersistable(old, chat.id));
	chat.messages = {{MessageRole::User, "Retain this prior remote context."}, {MessageRole::Assistant, "Retain the earlier answer."}};
	chat.messages.front().attachments = {old};
	UAM_ASSERT(chat.native_session_id.empty() && !chat.remote_process_exists && chat.remote_process_control_token.empty());
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	const auto original = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
	UAM_ASSERT(original && original->messages == chat.messages);
	app.chats.push_back(*original);
	ChatSession unrelated = ChatDomainService().CreateNewChat("", uam::provider_ids::kClaudeCli, false);
	unrelated.workspace_directory = (temp.root / "unrelated-workspace").string(); fs::create_directories(fs::path(unrelated.workspace_directory));
	unrelated.messages = {{MessageRole::User, "Retain unrelated saved history."}};
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, unrelated)); app.chats.push_back(unrelated);
	const auto unrelated_path = AppPaths::UamChatFilePath(app.data_root, unrelated.id);
	const auto unrelated_primary = uam_test::ReadFile(unrelated_path);
	ChatDomainService().SelectChatById(app, chat.id);
	app.composer_text = "Retain the unrelated live draft.";
	const auto prefix = original->messages;
	RemoteClaudeOrdinaryImageCleanup cleanup{app, chat.id, temp.root / "release-result"};
	MessageAttachment current; current.id = "current-image"; current.name = "Current remote PNG"; current.kind = "image";
	current.mime_type = "image/png"; current.path = "current.png";
	UAM_ASSERT(current.source == AttachmentSource::LegacyPath && current.origin_execution_host_id.empty() && uam::attachment_sources::IsPersistable(current, chat.id));
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(app, chat.id, task, {}, {current}, false, &error) && error.empty());
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	bool receipt = false;
	bool saw_pending_request = false, saw_empty_pending_method = false, saw_nonpositive_pending_id = false;
	bool saw_image_worker = false, saw_image_worker_finished = false, saw_image_worker_error = false, saw_prepared_message = false;
	const auto observe_admission = [&]()
	{
		if (const auto* owner = ChatDomainService().FindChatById(app, chat.id))
			for (const auto& pending : owner->remote_pending_requests)
			{
				saw_pending_request = true;
				saw_empty_pending_method = saw_empty_pending_method || pending.method.empty();
				saw_nonpositive_pending_id = saw_nonpositive_pending_id || pending.request_id <= 0;
			}
		if (const auto* runtime = uam::FindAcpSessionForChat(app, chat.id); runtime && runtime->claude_input_preparation)
		{
			const auto& state = runtime->claude_input_preparation->state;
			saw_image_worker = true;
			if (state && state->finished.load(std::memory_order_acquire))
			{
				saw_image_worker_finished = true;
				saw_image_worker_error = saw_image_worker_error || !state->error.empty();
				saw_prepared_message = saw_prepared_message || (!state->message.is_null() && !state->message.empty());
			}
		}
	};
	while (std::chrono::steady_clock::now() < deadline)
	{
		observe_admission();
		cleanup.Tick();
		observe_admission();
		const auto* session = uam::FindAcpSessionForChat(app, chat.id);
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
		if (fs::is_regular_file(temp.root / "peer-input.json") && session && session->session_ready && session->processing &&
		    session->session_id == native && saved && saved->native_session_id == native && saved->remote_turn_reconnect_pending &&
		    saved->remote_prompt_delivery_id.empty() && saved->remote_prompt_delivery_payload.empty() && !session->pending_prompt_delivery &&
		    app.pending_chat_save_at_by_chat_id.empty() && app.pending_chat_save_tasks.empty()) { receipt = true; break; }
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	if (!receipt)
	{
		const auto* runtime = uam::FindAcpSessionForChat(app, chat.id);
		const auto* owner = ChatDomainService().FindChatById(app, chat.id);
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
		// Fixed labels/booleans/counts only: never log tokens, native IDs, payloads,
		// peer envelopes, image bytes, paths, prompt prose or unknown error text.
		const auto error_class = [](const std::string& value)
		{
			if (value.empty()) return "empty";
			if (value == "Remote prompt was not sent because its owned durable receipt was not confirmed.") return "owned_durable_receipt_not_confirmed";
			if (value == "Prompt was not sent because chat history could not be saved.") return "chat_history_save_failed";
			return "other_nonempty";
		};
		std::cerr << "PRIVATE_REMOTE_CLAUDE_RECEIPT_DIAGNOSTIC " << Json{
		    {"session_present", runtime != nullptr}, {"session_running", runtime && runtime->running},
		    {"session_initialized", runtime && runtime->initialized}, {"session_ready", runtime && runtime->session_ready},
		    {"session_processing", runtime && runtime->processing}, {"session_native_id_present", runtime && !runtime->session_id.empty()},
		    {"session_last_error", runtime ? error_class(runtime->last_error) : "missing_session"},
		    {"pending_prompt_delivery", runtime && static_cast<bool>(runtime->pending_prompt_delivery)},
		    {"pending_agent_input", runtime && static_cast<bool>(runtime->pending_agent_input)},
		    {"pending_local_launch", runtime && static_cast<bool>(runtime->pending_local_launch)},
		    {"owner_present", owner != nullptr}, {"owner_rows", owner ? owner->messages.size() : 0},
		    {"owner_queue_held", owner && owner->acp_queue_held}, {"owner_pending_requests", owner ? owner->remote_pending_requests.size() : 0},
		    {"owner_delivery_id_present", owner && !owner->remote_prompt_delivery_id.empty()},
		    {"owner_delivery_payload_present", owner && !owner->remote_prompt_delivery_payload.empty()},
		    {"saved_present", static_cast<bool>(saved)}, {"saved_rows", saved ? saved->messages.size() : 0},
		    {"saved_native_id_present", saved && !saved->native_session_id.empty()},
		    {"saved_remote_reconnect_pending", saved && saved->remote_turn_reconnect_pending},
		    {"saved_delivery_id_present", saved && !saved->remote_prompt_delivery_id.empty()},
		    {"saved_delivery_payload_present", saved && !saved->remote_prompt_delivery_payload.empty()},
		    {"peer_input_exists", fs::is_regular_file(temp.root / "peer-input.json")},
		    {"saw_pending_request", saw_pending_request}, {"saw_empty_pending_method", saw_empty_pending_method},
		    {"saw_nonpositive_pending_id", saw_nonpositive_pending_id},
		    {"saw_image_worker", saw_image_worker}, {"saw_image_worker_finished", saw_image_worker_finished},
		    {"saw_image_worker_error", saw_image_worker_error}, {"saw_prepared_message", saw_prepared_message},
		    {"claude_input_tasks", app.claude_input_tasks.size()}, {"remote_context_tasks", app.remote_context_tasks.size()},
		    {"pending_save_tasks", app.pending_chat_save_tasks.size()}, {"scheduled_saves", app.pending_chat_save_at_by_chat_id.size()},
		    {"pending_remote_stops", app.pending_acp_remote_stops.size()}}.dump() << std::endl;
	}
	UAM_ASSERT(receipt); // Result is still withheld: clearing the real pre-write intent requires actual protocol3 input ACK.
	auto* session = uam::FindAcpSessionForChat(app, chat.id);
	UAM_ASSERT(session && session->running && session->initialized && session->session_ready && session->processing && session->last_error.empty());
	UAM_ASSERT_EQ(session->protocol_kind, std::string("claude-code-stream-json"));
	UAM_ASSERT_EQ(session->process_execution_host_id, host.id); UAM_ASSERT_EQ(session->remote_runner_protocol_version, 3);
	UAM_ASSERT_EQ(session->current_model_id, model); UAM_ASSERT_EQ(session->session_id, native);
	// Claude's ordinary launch does not promise a compatibility cache entry.
	// Request a real exact-host probe and consume it within the existing Send deadline.
	std::string version_error;
	UAM_ASSERT(ProviderCliCompatibilityService().StartProviderVersionCheck(app,
	    uam::provider_ids::kClaudeCli, true, host.id, &version_error) && version_error.empty());
	const auto version_key = CliProviderVersionStateKey(uam::provider_ids::kClaudeCli, host.id);
	bool version_checked = false;
	while (std::chrono::steady_clock::now() < deadline)
	{
		cleanup.Tick();
		const auto found = app.runtime_cli_versions_by_provider_id.find(version_key);
		if (found != app.runtime_cli_versions_by_provider_id.end() && found->second.checked &&
		    !app.runtime_cli_version_check_task.running && !app.runtime_cli_version_check_task.worker)
		{
			version_checked = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT(version_checked);
	const auto version_found = app.runtime_cli_versions_by_provider_id.find(version_key);
	UAM_ASSERT(version_found != app.runtime_cli_versions_by_provider_id.end() && version_found->second.checked);
	const auto version = app.runtime_cli_versions_by_provider_id.at(CliProviderVersionStateKey(uam::provider_ids::kClaudeCli, host.id));
	UAM_ASSERT(version.checked && version.supported && version.check_error.empty() && uam::execution_hosts::SameConnection(version.execution_host, host));
	UAM_ASSERT_EQ(version.installed_version, std::string("2.1.31"));
	const auto launch = Json::parse(uam_test::ReadFile(temp.root / "peer-launch.json"));
	UAM_ASSERT_EQ(launch.at("cwd"), Json(workspace.string())); UAM_ASSERT_EQ(launch.at("executable"), Json(claude.string()));
	const auto input = Json::parse(uam_test::ReadFile(temp.root / "peer-input.json"));
	UAM_ASSERT_EQ(input.at("pid"), launch.at("pid"));
	const auto envelope = input.at("envelope");
	UAM_ASSERT_EQ(envelope.at("type"), Json("user")); UAM_ASSERT(!envelope.contains("session_id"));
	UAM_ASSERT(input.at("raw").get<std::string>().size() <= 2 * 1024 * 1024);
	std::string prompt_text; std::vector<Json> images;
	for (const auto& part : envelope.at("message").at("content"))
	{
		if (part.value("type", "") == "text") prompt_text += part.value("text", "");
		if (part.value("type", "") == "image") images.push_back(part);
	}
	UAM_ASSERT(prompt_text.find(task) != std::string::npos); UAM_ASSERT_EQ(images.size(), std::size_t{1});
	UAM_ASSERT_EQ(images.front().at("source").at("type"), Json("base64"));
	UAM_ASSERT_EQ(images.front().at("source").at("media_type"), Json("image/png"));
	std::string decoded; UAM_ASSERT(uam::base64::Decode(images.front()["source"]["data"].get<std::string>(), decoded));
	UAM_ASSERT_EQ(decoded, image_bytes); UAM_ASSERT(decoded != old_bytes);
	const auto delivered = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
	UAM_ASSERT(delivered && delivered->messages.size() >= prefix.size() + 1);
	UAM_ASSERT_EQ(session->turn_user_message_index, static_cast<int>(prefix.size()));
	const auto& delivered_user = delivered->messages[static_cast<std::size_t>(session->turn_user_message_index)];
	UAM_ASSERT(delivered_user.role == MessageRole::User && !delivered_user.acp_prompt_not_sent);
	UAM_ASSERT_EQ(delivered->messages[0], prefix[0]); UAM_ASSERT_EQ(delivered->messages[1], prefix[1]);
	UAM_ASSERT_EQ(delivered_user.content, task);
	UAM_ASSERT_EQ(delivered_user.attachments.size(), std::size_t{1});
	UAM_ASSERT_EQ(delivered_user.attachments.front().name, current.name); // Ordinary native wire has no filename label.
	UAM_ASSERT(delivered_user.attachments.front().source == AttachmentSource::LegacyPath && delivered_user.attachments.front().origin_execution_host_id.empty());
	UAM_ASSERT_EQ(delivered_user.attachments.front().path, current.path);
	UAM_ASSERT_EQ(delivered->execution_host_id, host.id); // Ordinary path resolution is scoped by the actual saved owner/session host.
	UAM_ASSERT(delivered->remote_process_exists && !delivered->remote_process_control_token.empty());
	UAM_ASSERT_EQ(session->remote_output_delivery_token, delivered->remote_process_control_token);
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "release-result", "Finish the actual peer turn"));
	bool finished = false;
	while (std::chrono::steady_clock::now() < deadline)
	{
		cleanup.Tick();
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
		if (!session->processing && session->session_ready && saved && saved->messages.size() == prefix.size() + 2 &&
		    saved->messages.back().content == answer && app.pending_chat_save_at_by_chat_id.empty() && app.pending_chat_save_tasks.empty()) { finished = true; break; }
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT(finished && session->last_error.empty());
	// The ordinary positive above is a mandatory prerequisite. This next public
	// asset transaction is a typed DATA fixture, not a Claude native export or
	// permission/process/session proof. All execution authority came from Send.
	auto* retry_live = ChatDomainService().FindChatById(app, chat.id);
	UAM_ASSERT(retry_live && session->running && session->session_ready && !session->processing);
	UAM_ASSERT(!session->pending_prompt_delivery && session->queued_prompt.empty() && session->prompt_request_id == 0);
	UAM_ASSERT(retry_live->remote_process_exists && !retry_live->remote_process_control_token.empty());
	UAM_ASSERT_EQ(retry_live->native_session_id, native);
	UAM_ASSERT_EQ(session->session_id, native);
	UAM_ASSERT_EQ(session->remote_output_delivery_token, retry_live->remote_process_control_token);
	UAM_ASSERT_EQ(session->process_execution_host_id, host.id);
	const auto retry_process = session->child_pid;
	const auto retry_control = session->remote_output_delivery_token;
	const auto retry_incarnation = retry_live->metadata_incarnation;
	const auto retry_birth = retry_live->created_at;
	const auto before_retry_messages = retry_live->messages;
	const auto revision = ChatRepository::ChatStorageRevision(app.data_root, chat.id);
	UAM_ASSERT(revision && *revision != "missing");
	ChatSession candidate = *retry_live;
	Message retry_row;
	retry_row.role = MessageRole::User;
	retry_row.content = retry_task;
	retry_row.created_at = uam::time::TimestampNow();
	retry_row.acp_prompt_not_sent = true;
	const auto captured_skill_file = temp.root / "captured-owned-retry-skill.md";
	const std::string captured_skill_block = "CAPTURED OWNED RETRY SKILL\nUse the retained instructions exactly.\n";
	const std::string changed_skill_disk = "CHANGED DISK CONTENT MUST NOT REPLACE CAPTURED CONTEXT\n";
	if (control == RemoteOwnedRetryControl::BenignMetadata)
	{
		UAM_ASSERT(uam::io::WriteTextFile(captured_skill_file, captured_skill_block));
		retry_row.markdown_store_files = {captured_skill_file.string()};
		retry_row.markdown_store_prompt_blocks = {uam_test::ReadFile(captured_skill_file)};
		UAM_ASSERT_EQ(retry_row.markdown_store_prompt_blocks, std::vector<std::string>{captured_skill_block});
	}
	MessageAttachment retained;
	retained.id = "fixture-retained-original"; retained.name = "Retained exact GIF"; retained.kind = "image";
	retained.source = AttachmentSource::NativeInlinePending;
	retained.native_part_key = "v1:data-fixture/saved-user/image/0";
	retained.native_source_field = "data-fixture/user/attachments/0";
	retained.origin_execution_host_id = host.id;
	std::string media_error;
	retained.native_inline_payload = uam::native_assets::MakeInlinePayload(uam::base64::Encode(old_bytes), "image/gif",
	    std::make_shared<uam::native_assets::PayloadBudget>(), media_error);
	UAM_ASSERT(retained.native_inline_payload && media_error.empty());
	retained.mime_type = retained.native_inline_payload->MimeType();
	retained.size_bytes = retained.native_inline_payload->DecodedSize();
	retained.content_digest = retained.native_inline_payload->Digest();
	retry_row.attachments = {retained};
	candidate.messages.push_back(std::move(retry_row));
	const auto retry_index = static_cast<int>(candidate.messages.size()) - 1;
	// Prepare/Publish/ACK and disposal finish on this actual worker before live
	// adoption. The worker does not manufacture any runtime authority.
	auto media_worker = std::async(std::launch::async, [root = app.data_root, candidate = std::move(candidate), revision = *revision]() mutable
	{
		std::string failure;
		auto prepared = uam::PreparedNativeMediaChatSave::Prepare(root, std::move(candidate), revision, failure);
		if (!prepared || !prepared->Publish(failure) || !prepared->AcknowledgeOriginals(failure))
			return std::make_pair(std::optional<ChatSession>{}, failure);
		return std::make_pair(prepared->TakeCommittedChat(), failure);
	});
	auto committed = media_worker.get();
	UAM_ASSERT(committed.first && committed.second.empty());
	UAM_ASSERT(committed.first->metadata_incarnation == retry_incarnation && committed.first->created_at == retry_birth);
	*retry_live = std::move(*committed.first);
	const auto retry_saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
	UAM_ASSERT(retry_saved && retry_saved->messages == retry_live->messages);
	UAM_ASSERT_EQ(retry_saved->native_session_id, native);
	UAM_ASSERT_EQ(retry_saved->execution_host_id, host.id);
	UAM_ASSERT_EQ(retry_saved->workspace_directory, workspace.string());
	UAM_ASSERT_EQ(retry_saved->model_id, model);
	UAM_ASSERT_EQ(retry_saved->messages.size(), before_retry_messages.size() + 1);
	UAM_ASSERT(std::equal(before_retry_messages.begin(), before_retry_messages.end(), retry_saved->messages.begin()));
	const auto& saved_retry = retry_saved->messages.back();
	if (control == RemoteOwnedRetryControl::BenignMetadata)
	{
		UAM_ASSERT_EQ(saved_retry.markdown_store_files, std::vector<std::string>{captured_skill_file.string()});
		UAM_ASSERT_EQ(saved_retry.markdown_store_prompt_blocks, std::vector<std::string>{captured_skill_block});
		UAM_ASSERT_EQ(retry_live->messages.back().markdown_store_files, saved_retry.markdown_store_files);
		UAM_ASSERT_EQ(retry_live->messages.back().markdown_store_prompt_blocks, saved_retry.markdown_store_prompt_blocks);
		// Change the actual private file after public Prepare/Publish/ACK and reload.
		UAM_ASSERT(uam::io::WriteTextFile(captured_skill_file, changed_skill_disk));
		UAM_ASSERT(uam_test::ReadFile(captured_skill_file) != captured_skill_block);
	}
	UAM_ASSERT(saved_retry.role == MessageRole::User && saved_retry.acp_prompt_not_sent && !saved_retry.acp_prompt_discarded);
	UAM_ASSERT_EQ(saved_retry.content, retry_task);
	UAM_ASSERT_EQ(saved_retry.attachments.size(), std::size_t{1});
	const auto owned = saved_retry.attachments.front();
	UAM_ASSERT(owned.source == AttachmentSource::OwnedNativeAsset && owned.path.empty() && !owned.native_inline_payload);
	UAM_ASSERT_EQ(owned.origin_execution_host_id, host.id);
	UAM_ASSERT(uam::native_assets::IsKeyForOwner(chat.id, owned.native_asset_key));
	UAM_ASSERT(!uam::native_assets::IsKeyForOwner(native, owned.native_asset_key));
	const auto original_bytes = uam::native_assets::ReadOwned(app.data_root, chat.id, owned.native_asset_key);
	UAM_ASSERT(original_bytes.ok && original_bytes.bytes == old_bytes && original_bytes.bytes != image_bytes);
	UAM_ASSERT_EQ(uam::RecoverableUnsentAcpMessageIndices(app, *retry_live), std::vector<int>{retry_index});
	UAM_ASSERT_EQ(session->child_pid, retry_process);
	UAM_ASSERT_EQ(session->remote_output_delivery_token, retry_control);
	UAM_ASSERT(session->running && session->session_ready && !session->processing && !session->saved_image_retry);
	const auto retained_primary = uam_test::ReadFile(AppPaths::UamChatFilePath(app.data_root, chat.id));
	const auto prior_input = uam_test::ReadFile(temp.root / "peer-input.json");
	const auto stdin_before_retry = uam_test::ReadFile(temp.root / "peer-stdin.jsonl");
	UAM_ASSERT(!fs::exists(temp.root / "peer-retry-input.json"));
	std::vector<std::string> asset_parts;
	{
		std::istringstream key(owned.native_asset_key);
		for (std::string part; std::getline(key, part, ':');)
		{
			asset_parts.push_back(part);
		}
	}
	UAM_ASSERT(asset_parts.size() == 5 && asset_parts.front() == "na1");
	const auto original_path = app.data_root / std::string(uam::native_assets::kStoreDirectory) /
	    asset_parts[1] / asset_parts[2] / (asset_parts[3] + ".bin");
	UAM_ASSERT_EQ(uam_test::ReadFile(original_path), original_bytes.bytes);
	if (control == RemoteOwnedRetryControl::MissingOriginal)
	{
		UAM_ASSERT(fs::remove(original_path));
		UAM_ASSERT(!uam::native_assets::ReadOwned(app.data_root, chat.id, owned.native_asset_key).ok);
	}
	if (control == RemoteOwnedRetryControl::EncodedLimit)
	{
		UAM_ASSERT(retry_task.size() < 2 * 1024 * 1024 - 16 * 1024);
		UAM_ASSERT(Json(retry_task).dump().size() > 2 * 1024 * 1024);
	}
	std::string retry_error;
	// Require the exact genuine recoverable row; indexed Retry must not inherit
	// eligibility from a lower RetryLast call or a truncated transcript.
	UAM_ASSERT_EQ(uam::RecoverableUnsentAcpMessageIndices(app, *retry_live), std::vector<int>{retry_index});
	const bool retry_accepted = control == RemoteOwnedRetryControl::Healthy
	    ? uam::RetryLastAcpPrompt(app, chat.id, &retry_error)
	    : uam::RetryFailedAcpMessage(app, chat.id, retry_index, &retry_error);
	if (!retry_accepted)
	{
		// Baseline is meaningful only after every real ordinary/owned-store/row
		// prerequisite above. Qualify the current specific local-only refusal.
		if (control == RemoteOwnedRetryControl::Healthy)
		{
			UAM_ASSERT_EQ(retry_error, std::string("Recorded image Retry requires valid local originals and a supported provider. Remove unsupported sources or attach files again."));
		}
		else
		{
			UAM_ASSERT(!retry_error.empty());
		}
		UAM_ASSERT_EQ(uam_test::ReadFile(AppPaths::UamChatFilePath(app.data_root, chat.id)), retained_primary);
		UAM_ASSERT_EQ(retry_live->messages, retry_saved->messages);
		UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "peer-input.json"), prior_input);
		UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "peer-stdin.jsonl"), stdin_before_retry);
		UAM_ASSERT(!fs::exists(temp.root / "peer-retry-input.json"));
		UAM_ASSERT(!session->saved_image_retry && !session->pending_prompt_delivery);
		UAM_ASSERT_EQ(session->child_pid, retry_process);
		UAM_ASSERT_EQ(session->remote_output_delivery_token, retry_control);
	}
	UAM_ASSERT(retry_accepted && retry_error.empty()); // Intended future product oracle; never classify prerequisite failures as this RED.
	const bool worker_control = control == RemoteOwnedRetryControl::MissingOriginal || control == RemoteOwnedRetryControl::EncodedLimit ||
	    control == RemoteOwnedRetryControl::HostConnection || control == RemoteOwnedRetryControl::ProviderProfile || control == RemoteOwnedRetryControl::SavedNative ||
	    control == RemoteOwnedRetryControl::PendingRow || control == RemoteOwnedRetryControl::PendingIncarnation || control == RemoteOwnedRetryControl::BenignMetadata || control == RemoteOwnedRetryControl::HealthyIndexed;
	if (worker_control)
	{
		const auto refusal_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (!session->claude_input_preparation && session->processing && std::chrono::steady_clock::now() < refusal_deadline)
		{
			(void)uam::FlushPendingChatSaves(app);
			(void)uam::acp_detail::SendQueuedPromptIfReady(app, *session, *retry_live);
			std::this_thread::yield();
		}
		UAM_ASSERT(session->claude_input_preparation && session->saved_image_retry);
		const auto state = session->claude_input_preparation->state;
		// Observe the genuine worker only; no owner polling can consume its result here.
		while (!state->finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < refusal_deadline)
		{
			std::this_thread::yield();
		}
		UAM_ASSERT(state->finished.load(std::memory_order_acquire));
		UAM_ASSERT(!session->pending_prompt_delivery && state->sealed_message.is_null());
		UAM_ASSERT(!fs::exists(temp.root / "peer-retry-input.json"));
		if (control == RemoteOwnedRetryControl::MissingOriginal || control == RemoteOwnedRetryControl::EncodedLimit)
		{
			UAM_ASSERT(!state->error.empty() && (state->message.is_null() || state->message.empty()));
			if (control == RemoteOwnedRetryControl::EncodedLimit)
			{
				UAM_ASSERT(state->error.find("SSH input size limit") != std::string::npos);
			}
		}
		else
		{
			UAM_ASSERT(state->error.empty() && state->message.is_object());
			UAM_ASSERT(state->message.contains("message") && state->message["message"]["content"].is_array());
			std::size_t images = 0;
			for (const auto& part : state->message["message"]["content"])
			{
				if (part.value("type", "") != "image")
				{
					continue;
				}
				++images;
				std::string decoded;
				UAM_ASSERT(uam::base64::Decode(part["source"]["data"].get<std::string>(), decoded));
				UAM_ASSERT_EQ(decoded, original_bytes.bytes);
			}
			UAM_ASSERT_EQ(images, std::size_t{1});
		}
		if (control == RemoteOwnedRetryControl::BenignMetadata)
		{
			// Require actual successful skill publication, not inline fallback, before
			// the image worker completed and before benign settings mutate.
			UAM_ASSERT(!session->queued_skill_spans.empty());
			const auto skill_state = session->attached_skill_preparation;
			UAM_ASSERT(skill_state && skill_state->finished.load(std::memory_order_acquire));
			UAM_ASSERT(skill_state->skills_prepared && skill_state->error.empty() && !skill_state->cancel.stop_requested());
			UAM_ASSERT_EQ(skill_state->skill_spans.size(), std::size_t{1});
			UAM_ASSERT_EQ(skill_state->skill_spans, session->queued_skill_spans);
			const auto& span = skill_state->skill_spans.front();
			UAM_ASSERT(span.length == captured_skill_block.size() && span.offset <= skill_state->prompt.size());
			UAM_ASSERT(span.length <= skill_state->prompt.size() - span.offset && !span.digest.empty());
			UAM_ASSERT_EQ(skill_state->prompt.substr(span.offset, span.length), captured_skill_block);
			UAM_ASSERT(skill_state->prompt.find(changed_skill_disk) == std::string::npos);
			UAM_ASSERT_EQ(state->identity.prompt, skill_state->wire_prompt);
			UAM_ASSERT(skill_state->wire_prompt.find("Attached skill: read ") != std::string::npos);
			// Benign controller display/health metadata is not connection or profile
			// authority. Mutate it only after the real successful worker completes.
			const auto found = std::ranges::find(app.settings.execution_hosts, host.id, &ExecutionHost::id);
			UAM_ASSERT(found != app.settings.execution_hosts.end());
			UAM_ASSERT(uam::execution_hosts::SameConnection(*found, host));
			found->label = "Renamed private selected host";
			found->last_seen_at = "2026-10-18 19:00:00";
			app.settings.ui_theme = "light";
			UAM_ASSERT_EQ(retry_live->execution_host_id, found->id);
			UAM_ASSERT(uam::execution_hosts::SameConnection(*found, host));
			UAM_ASSERT_EQ(retry_live->native_session_id, native);
			UAM_ASSERT_EQ(session->child_pid, retry_process);
			UAM_ASSERT_EQ(session->remote_output_delivery_token, retry_control);
		}
		else if (control != RemoteOwnedRetryControl::HealthyIndexed)
		{
			auto completions = std::make_shared<std::atomic<int>>(0);
			const bool pending_control = control == RemoteOwnedRetryControl::PendingRow || control == RemoteOwnedRetryControl::PendingIncarnation;
			if (pending_control)
			{
				UAM_ASSERT(uam::acp_detail::SendQueuedPromptIfReady(app, *session, *retry_live));
				UAM_ASSERT(session->pending_prompt_delivery && !state->sealed_message.is_null());
				UAM_ASSERT(state->sealed_method.empty() && state->sealed_request_id > 0);
				UAM_ASSERT(session->prompt_request_id == 0 && !fs::exists(temp.root / "peer-retry-input.json"));
				UAM_ASSERT(uam::DeferChatSaveResult(app, chat.id, [completions](bool) { ++*completions; }));
			}
			const auto configured_host = [&]() -> ExecutionHost&
			{
				const auto found = std::ranges::find(app.settings.execution_hosts, host.id, &ExecutionHost::id);
				UAM_ASSERT(found != app.settings.execution_hosts.end());
				return *found;
			};
			const auto saved_profile = *ProviderProfileStore::FindById(app.provider_profiles, uam::provider_ids::kClaudeCli);
			// These mutations revoke captured state; they never grant process/native/Retry authority.
			if (control == RemoteOwnedRetryControl::HostConnection)
			{
				configured_host().ssh_alias += "-different-connection";
			}
			if (control == RemoteOwnedRetryControl::ProviderProfile)
			{
				ProviderProfileStore::FindById(app.provider_profiles, uam::provider_ids::kClaudeCli)->interactive_command += " --different-profile";
			}
			if (control == RemoteOwnedRetryControl::SavedNative)
			{
				retry_live->native_session_id = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
				UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *retry_live));
			}
			if (control == RemoteOwnedRetryControl::PendingRow)
			{
				retry_live->messages[static_cast<std::size_t>(retry_index)].content += " Preserve this newer authored intent.";
				UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *retry_live));
			}
			if (control == RemoteOwnedRetryControl::PendingIncarnation)
			{
				retry_live->metadata_incarnation = std::make_shared<const std::uint8_t>(0);
			}
			const auto expected_messages = retry_live->messages;
			const auto expected_native = retry_live->native_session_id;
			const auto expected_incarnation = retry_live->metadata_incarnation;
			const auto expected_alias = uam::execution_hosts::Find(app.settings.execution_hosts, host.id)->ssh_alias;
			const auto expected_command = ProviderProfileStore::FindById(app.provider_profiles, uam::provider_ids::kClaudeCli)->interactive_command;
			do
			{
				cleanup.Tick();
			} while ((session->saved_image_retry || session->claude_input_preparation || session->pending_prompt_delivery ||
			    session->processing || !app.pending_chat_save_tasks.empty() || !app.pending_chat_save_at_by_chat_id.empty()) &&
			    std::chrono::steady_clock::now() < refusal_deadline);
			UAM_ASSERT(!session->processing && !session->last_error.empty());
			UAM_ASSERT(!session->saved_image_retry && !session->claude_input_preparation && !session->pending_prompt_delivery);
			UAM_ASSERT(state->cancel.stop_requested());
			UAM_ASSERT(app.pending_chat_save_tasks.empty() && app.pending_chat_save_at_by_chat_id.empty());
			if (pending_control)
			{
				UAM_ASSERT_EQ(completions->load(), 1);
			}
			UAM_ASSERT_EQ(retry_live->metadata_incarnation, expected_incarnation);
			UAM_ASSERT_EQ(retry_live->native_session_id, expected_native);
			UAM_ASSERT_EQ(uam::execution_hosts::Find(app.settings.execution_hosts, host.id)->ssh_alias, expected_alias);
			UAM_ASSERT_EQ(ProviderProfileStore::FindById(app.provider_profiles, uam::provider_ids::kClaudeCli)->interactive_command, expected_command);
			const auto refused = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
			UAM_ASSERT(refused && refused->messages.size() == expected_messages.size());
			UAM_ASSERT_EQ(refused->native_session_id, expected_native);
			for (std::size_t index = 0; index < expected_messages.size(); ++index)
			{
				for (const auto* row : std::array<const Message*, 2>{&refused->messages[index], &retry_live->messages[index]})
				{
					UAM_ASSERT_EQ(row->role, expected_messages[index].role);
					UAM_ASSERT_EQ(row->content, expected_messages[index].content);
					UAM_ASSERT_EQ(row->created_at, expected_messages[index].created_at);
					UAM_ASSERT_EQ(row->attachments, expected_messages[index].attachments);
				}
			}
			UAM_ASSERT_EQ(session->child_pid, retry_process);
			UAM_ASSERT_EQ(session->remote_output_delivery_token, retry_control);
			UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "peer-input.json"), prior_input);
			UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "peer-stdin.jsonl"), stdin_before_retry);
			UAM_ASSERT(!fs::exists(temp.root / "peer-retry-input.json") && !fs::exists(temp.root / "peer-extra-input.json") && !fs::exists(temp.root / "peer-unexpected-input.json"));
			UAM_ASSERT_EQ(uam_test::ReadFile(unrelated_path), unrelated_primary);
			UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), chat.id);
			UAM_ASSERT_EQ(app.composer_text, std::string("Retain the unrelated live draft."));
			if (control == RemoteOwnedRetryControl::MissingOriginal)
			{
				UAM_ASSERT(uam::io::WriteBinaryFile(original_path, original_bytes.bytes));
			}
			UAM_ASSERT_EQ(uam::native_assets::ReadOwned(app.data_root, chat.id, owned.native_asset_key).bytes, original_bytes.bytes);
			// Restore only private drift configuration after all preservation/refusal
			// assertions, so cleanup can stop the original actual connection safely.
			configured_host() = host;
			*ProviderProfileStore::FindById(app.provider_profiles, uam::provider_ids::kClaudeCli) = saved_profile;
			retry_live->native_session_id = native;
			cleanup.StopAndDrain();
			UAM_ASSERT(cleanup.Quiet());
			UAM_ASSERT(!fs::exists(temp.root / "peer-retry-input.json") && !fs::exists(temp.root / "peer-extra-input.json") && !fs::exists(temp.root / "peer-unexpected-input.json"));
			UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "peer-input.json"), prior_input);
			UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "peer-stdin.jsonl"), stdin_before_retry);
			return;
		}
	}
	const auto retry_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	bool retry_receipt = false;
	while (std::chrono::steady_clock::now() < retry_deadline)
	{
		cleanup.Tick();
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
		if (fs::is_regular_file(temp.root / "peer-retry-input.json") && session->processing && saved &&
		    saved->remote_turn_reconnect_pending && saved->remote_prompt_delivery_id.empty() && saved->remote_prompt_delivery_payload.empty() &&
		    !session->pending_prompt_delivery && app.pending_chat_save_tasks.empty() && app.pending_chat_save_at_by_chat_id.empty())
		{ retry_receipt = true; break; }
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT(retry_receipt);
	UAM_ASSERT_EQ(session->session_id, native);
	UAM_ASSERT_EQ(session->child_pid, retry_process);
	UAM_ASSERT_EQ(session->remote_output_delivery_token, retry_control);
	const auto retry_input = Json::parse(uam_test::ReadFile(temp.root / "peer-retry-input.json"));
	UAM_ASSERT_EQ(retry_input.at("pid"), input.at("pid"));
	UAM_ASSERT(retry_input.at("raw").get<std::string>().size() <= 2 * 1024 * 1024);
	std::string retry_text;
	std::vector<Json> retry_images;
	for (const auto& part : retry_input.at("envelope").at("message").at("content"))
	{
		if (part.value("type", "") == "text") retry_text += part.value("text", "");
		if (part.value("type", "") == "image") retry_images.push_back(part);
	}
	UAM_ASSERT(retry_text.find(retry_task) != std::string::npos);
	if (control == RemoteOwnedRetryControl::BenignMetadata)
	{
		// Inspect the actual wire reference, then read the actual helper-installed
		// private resource. This proves delivery, not native backend consumption.
		const std::string reference_prefix = "Attached skill: read ";
		const std::string reference_suffix = " before using it; if read is denied or unavailable, do not use this skill.";
		const auto start = retry_text.find(reference_prefix);
		UAM_ASSERT(start != std::string::npos);
		const auto path_start = start + reference_prefix.size();
		const auto path_end = retry_text.find(reference_suffix, path_start);
		UAM_ASSERT(path_end != std::string::npos);
		const auto encoded_path = Json::parse(retry_text.substr(path_start, path_end - path_start));
		UAM_ASSERT(encoded_path.is_string());
		const auto resource = fs::path(encoded_path.get<std::string>());
		const auto relative = resource.lexically_relative(workspace / ".UAM" / "provider-setup");
		UAM_ASSERT(resource.is_absolute() && !relative.empty() && !relative.is_absolute());
		std::size_t components = 0;
		for (const auto& part : relative)
		{
			UAM_ASSERT(part != "." && part != "..");
			++components;
		}
		UAM_ASSERT_EQ(components, std::size_t{2});
		UAM_ASSERT_EQ(resource.extension(), fs::path(".md"));
		std::string resource_bytes, resource_error;
		{
			uam::remote::RunnerClient client(PlatformServicesFactory::Instance().process_service,
			    uam::remote::SshBridgeArgv(host.ssh_alias, host.platform, host.runner_version, host.runner_directory, host.runner_protocol_version),
			    host.runner_version, host.runner_protocol_version);
			UAM_ASSERT(client.ReadWorkspaceAttachment(workspace.string(), resource.lexically_relative(workspace).generic_string(),
			    resource_bytes, captured_skill_block.size(), &resource_error));
		}
		UAM_ASSERT(resource_error.empty());
		UAM_ASSERT_EQ(resource_bytes, captured_skill_block);
		UAM_ASSERT(resource_bytes != changed_skill_disk);
		UAM_ASSERT(retry_text.find(captured_skill_block) == std::string::npos);
		UAM_ASSERT(retry_text.find(changed_skill_disk) == std::string::npos);
		UAM_ASSERT_EQ(uam_test::ReadFile(captured_skill_file), changed_skill_disk);
	}
	UAM_ASSERT_EQ(retry_images.size(), std::size_t{1});
	UAM_ASSERT_EQ(retry_images.front().at("source").at("media_type"), Json("image/gif"));
	std::string retried_bytes;
	UAM_ASSERT(uam::base64::Decode(retry_images.front().at("source").at("data").get<std::string>(), retried_bytes));
	UAM_ASSERT_EQ(retried_bytes, original_bytes.bytes);
	UAM_ASSERT(retried_bytes != image_bytes);
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "release-retry-result", "Finish the actual retained Retry"));
	bool retry_finished = false;
	while (std::chrono::steady_clock::now() < retry_deadline)
	{
		cleanup.Tick();
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
		if (!session->processing && saved && saved->messages.size() == before_retry_messages.size() + 2 && saved->messages.back().content == retry_answer &&
		    app.pending_chat_save_tasks.empty() && app.pending_chat_save_at_by_chat_id.empty()) { retry_finished = true; break; }
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT(retry_finished && session->last_error.empty());
	const auto after_retry = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
	UAM_ASSERT(after_retry && after_retry->native_session_id == native && after_retry->messages[static_cast<std::size_t>(retry_index)].attachments == saved_retry.attachments);
	UAM_ASSERT(!after_retry->messages[static_cast<std::size_t>(retry_index)].acp_prompt_not_sent);
	UAM_ASSERT(std::equal(before_retry_messages.begin(), before_retry_messages.end(), after_retry->messages.begin()));
	UAM_ASSERT_EQ(uam::native_assets::ReadOwned(app.data_root, chat.id, owned.native_asset_key).bytes, original_bytes.bytes);

	if (control == RemoteOwnedRetryControl::BenignMetadata)
	{
		const auto* benign_host = uam::execution_hosts::Find(app.settings.execution_hosts, host.id);
		UAM_ASSERT(benign_host && uam::execution_hosts::SameConnection(*benign_host, host));
		UAM_ASSERT_EQ(benign_host->label, std::string("Renamed private selected host"));
		UAM_ASSERT_EQ(app.settings.ui_theme, std::string("light"));
		for (const auto* row : std::array<const Message*, 2>{&after_retry->messages[static_cast<std::size_t>(retry_index)], &retry_live->messages[static_cast<std::size_t>(retry_index)]})
		{
			UAM_ASSERT_EQ(row->markdown_store_files, std::vector<std::string>{captured_skill_file.string()});
			UAM_ASSERT_EQ(row->markdown_store_prompt_blocks, std::vector<std::string>{captured_skill_block});
		}
		UAM_ASSERT_EQ(uam_test::ReadFile(captured_skill_file), changed_skill_disk);
	}

	if (control == RemoteOwnedRetryControl::MixedLegacy || control == RemoteOwnedRetryControl::MixedOpaque)
	{
		// A COMPLETE healthy public owned Retry above qualifies this route before
		// invalid-source refusal, so baseline unsupported cannot count as GREEN.
		auto* mixed_live = ChatDomainService().FindChatById(app, chat.id);
		UAM_ASSERT(mixed_live && session->running && session->session_ready && !session->processing);
		Message mixed_row = saved_retry;
		mixed_row.created_at = uam::time::TimestampNow();
		mixed_row.content = "Refuse this complete mixed-source saved Retry.";
		mixed_row.acp_prompt_not_sent = true;
		if (control == RemoteOwnedRetryControl::MixedLegacy)
		{
			mixed_row.attachments.push_back(current);
		}
		else
		{
			// Opaque DATA stays independent of the Gemini-only helper above.
			MessageAttachment opaque;
			opaque.id = "retained-opaque-part";
			opaque.name = "Opaque provider reference";
			opaque.source = AttachmentSource::ProviderUri;
			opaque.source_uri = "https://assets.invalid/opaque-branch-reference.pdf";
			opaque.native_part_key = "v1:branch-source/assistant/file-part";
			opaque.native_source_field = "content/1/fileData";
			opaque.kind = "image";
			opaque.mime_type = "image/png";
			opaque.origin_execution_host_id = host.id;
			mixed_row.attachments.push_back(opaque);
		}
		mixed_live->messages.push_back(mixed_row);
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *mixed_live));
		const auto mixed_saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
		UAM_ASSERT(mixed_saved && mixed_saved->messages.back().attachments == mixed_row.attachments);
		const auto mixed_primary = uam_test::ReadFile(AppPaths::UamChatFilePath(app.data_root, chat.id));
		const auto second_input = uam_test::ReadFile(temp.root / "peer-retry-input.json");
		const auto mixed_stdin = uam_test::ReadFile(temp.root / "peer-stdin.jsonl");
		const auto mixed_index = static_cast<int>(mixed_live->messages.size()) - 1;
		UAM_ASSERT_EQ(uam::RecoverableUnsentAcpMessageIndices(app, *mixed_live), std::vector<int>{mixed_index});
		std::string mixed_error;
		UAM_ASSERT(!uam::RetryFailedAcpMessage(app, chat.id, mixed_index, &mixed_error) && !mixed_error.empty());
		UAM_ASSERT(!session->processing && !session->saved_image_retry && !session->claude_input_preparation && !session->pending_prompt_delivery);
		UAM_ASSERT_EQ(mixed_live->messages, mixed_saved->messages);
		UAM_ASSERT_EQ(uam_test::ReadFile(AppPaths::UamChatFilePath(app.data_root, chat.id)), mixed_primary);
		UAM_ASSERT_EQ(session->child_pid, retry_process);
		UAM_ASSERT_EQ(session->remote_output_delivery_token, retry_control);
		UAM_ASSERT_EQ(uam::native_assets::ReadOwned(app.data_root, chat.id, owned.native_asset_key).bytes, original_bytes.bytes);
		UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "peer-input.json"), prior_input);
		UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "peer-retry-input.json"), second_input);
		UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "peer-stdin.jsonl"), mixed_stdin);
		UAM_ASSERT(!fs::exists(temp.root / "peer-extra-input.json") && !fs::exists(temp.root / "peer-unexpected-input.json"));
		UAM_ASSERT_EQ(uam_test::ReadFile(unrelated_path), unrelated_primary);
		UAM_ASSERT_EQ(app.composer_text, std::string("Retain the unrelated live draft."));
		cleanup.StopAndDrain();
		UAM_ASSERT(cleanup.Quiet());
		UAM_ASSERT(!fs::exists(temp.root / "peer-extra-input.json") && !fs::exists(temp.root / "peer-unexpected-input.json"));
		UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "peer-retry-input.json"), second_input);
		UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "peer-stdin.jsonl"), mixed_stdin);
		return;
	}

	cleanup.StopAndDrain(); // Must occur before reading final saved state and before private environment teardown.
	UAM_ASSERT(cleanup.proxy_pid > 0 && !session->remote_stop_pending && !session->remote_stop_unconfirmed && !session->local_stop_pending);
	UAM_ASSERT(session->stop_outcome == uam::platform::ProcessStopOutcome::Graceful || session->stop_outcome == uam::platform::ProcessStopOutcome::Forced);
	UAM_ASSERT(!session->running && session->child_pid == -1 && session->stdin_write_fd == -1 && session->stdout_read_fd == -1 && session->stderr_read_fd == -1);
	int proxy_status = 0; errno = 0;
	UAM_ASSERT(waitpid(cleanup.proxy_pid, &proxy_status, WNOHANG) == -1 && errno == ECHILD); // Actual original direct-child proxy was reaped.
	const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
	const auto* live = ChatDomainService().FindChatById(app, chat.id);
	UAM_ASSERT(saved && live && saved->messages == live->messages && saved->native_session_id == native);
	UAM_ASSERT(!saved->remote_stop_cleanup_pending && !live->remote_stop_cleanup_pending && !saved->remote_process_exists && !live->remote_process_exists);
	UAM_ASSERT_EQ(saved->execution_host_id, host.id); UAM_ASSERT_EQ(saved->workspace_directory, workspace.string()); UAM_ASSERT_EQ(saved->model_id, model);
	UAM_ASSERT_EQ(saved->messages.front(), prefix.front()); UAM_ASSERT_EQ(saved->messages.back().content, retry_answer);
	UAM_ASSERT_EQ(uam_test::ReadFile(workspace / "current.png"), image_bytes); UAM_ASSERT_EQ(uam_test::ReadFile(workspace / "old.gif"), old_bytes);
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), chat.id);
	UAM_ASSERT_EQ(app.composer_text, std::string("Retain the unrelated live draft."));
	UAM_ASSERT_EQ(uam_test::ReadFile(unrelated_path), unrelated_primary);
	UAM_ASSERT(uam_test::ReadFile(temp.root / "ssh-invocations").find("private-claude-image-host") != std::string::npos);
	UAM_ASSERT(cleanup.Quiet());
}

UAM_TEST(ClaudeRemoteSavedOwnedRetryDeliversExactOriginalThroughActualRunner)
{
	CheckClaudeRemoteSavedOwnedRetry(RemoteOwnedRetryControl::Healthy);
}

UAM_TEST(ClaudeRemoteSavedOwnedRetryFailedMessageDeliversExactOriginalThroughActualRunner)
{
	CheckClaudeRemoteSavedOwnedRetry(RemoteOwnedRetryControl::HealthyIndexed);
}

UAM_TEST(ClaudeRemoteSavedOwnedRetryFailedMessagePreservesBenignHostAndThemeChanges)
{
	CheckClaudeRemoteSavedOwnedRetry(RemoteOwnedRetryControl::BenignMetadata);
}

UAM_TEST(ClaudeRemoteSavedOwnedRetryRejectsMissingOriginalAndEncodedOverLimit)
{
	for (const auto control : {RemoteOwnedRetryControl::MissingOriginal, RemoteOwnedRetryControl::EncodedLimit})
	{
		CheckClaudeRemoteSavedOwnedRetry(control);
	}
}

UAM_TEST(ClaudeRemoteSavedOwnedRetryRejectsCapturedHostProfileAndNativeDrift)
{
	for (const auto control : {RemoteOwnedRetryControl::HostConnection, RemoteOwnedRetryControl::ProviderProfile, RemoteOwnedRetryControl::SavedNative})
	{
		CheckClaudeRemoteSavedOwnedRetry(control);
	}
}

UAM_TEST(ClaudeRemoteSavedOwnedRetryRejectsRowAndIncarnationAtDurablePacket)
{
	for (const auto control : {RemoteOwnedRetryControl::PendingRow, RemoteOwnedRetryControl::PendingIncarnation})
	{
		CheckClaudeRemoteSavedOwnedRetry(control);
	}
}

UAM_TEST(ClaudeRemoteSavedOwnedRetryRefusesMixedSourcesAfterHealthyOwnedRetry)
{
	for (const auto control : {RemoteOwnedRetryControl::MixedLegacy, RemoteOwnedRetryControl::MixedOpaque})
	{
		CheckClaudeRemoteSavedOwnedRetry(control);
	}
}
#endif

#if UAM_ENABLE_RUNTIME_CODEX_CLI && UAM_ENABLE_SSH && defined(__APPLE__)
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/chat/message_attachment_sources.h"
#include "remote/runner_proxy.h"
#include "remote/runner_protocol.h"
#include <sys/wait.h>
#include <signal.h>
#include <cerrno>
#include <tuple>

namespace
{
	struct RemoteCodexOwnedImageCleanup
	{
		uam::AppState& app;
		std::string chat_id;
		fs::path release;
		bool stopped = false;
		pid_t proxy_pid = -1;

		void Tick()
		{
			(void)uam::PollAllAcpSessions(app);
			(void)uam::FlushPendingChatSaves(app);
			ProviderCliCompatibilityService().Poll(app);
		}
		bool Quiet() const
		{
			const auto* session = uam::FindAcpSessionForChat(app, chat_id);
			const auto* chat = ChatDomainService().FindChatById(app, chat_id);
			const bool confirmed = proxy_pid <= 0 || (session && chat && !session->remote_stop_unconfirmed &&
			    (session->stop_outcome == uam::platform::ProcessStopOutcome::Graceful || session->stop_outcome == uam::platform::ProcessStopOutcome::Forced) &&
			    !chat->remote_stop_cleanup_pending && !chat->remote_process_exists);
			return confirmed && !app.provider_context_cleanup_worker && app.memory_extraction_tasks.empty() &&
			       app.permission_review_tasks.empty() && app.turn_checkpoint_tasks.empty() && app.pending_acp_remote_stops.empty() && app.acp_process_stop_tasks.empty() &&
			       app.claude_input_tasks.empty() && app.local_acp_launch_tasks.empty() && app.agent_input_preparation_tasks.empty() &&
			       app.remote_context_tasks.empty() && app.cli_context_preparation_tasks.empty() && app.memory_recall_tasks.empty() &&
			       !app.runtime_cli_version_check_task.running && !app.runtime_cli_version_check_task.worker &&
			       app.runtime_cli_version_check_queue.empty() && app.runtime_cli_install_tasks.empty() &&
			       app.pending_chat_save_tasks.empty() && app.pending_chat_save_at_by_chat_id.empty() &&
			       (!session || (!session->running && !session->remote_stop_pending && !session->remote_stop_unconfirmed && !session->local_stop_pending &&
			           !session->claude_input_preparation && !session->pending_local_launch && !session->pending_agent_input &&
			           !session->pending_prompt_delivery && !session->pending_local_session_receipt && !session->attached_skill_preparation));
		}
		void StopAndDrain()
		{
			if (stopped) return;
			// Allow the real peer to finish if an assertion failed while its result
			// was deliberately withheld. This does not fabricate runtime completion.
			(void)uam::io::WriteTextFile(release, "Release the actual private peer result");
			if (const auto* session = uam::FindAcpSessionForChat(app, chat_id)) proxy_pid = session->child_pid;
			(void)uam::StopAcpSession(app, chat_id); // Original owned proxy/control token, never a new bridge-direct Stop.
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
			while (!Quiet() && std::chrono::steady_clock::now() < deadline)
			{
				Tick();
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			if (!Quiet())
			{
				const auto* owner = uam::FindAcpSessionForChat(app, chat_id);
				const auto* saved = ChatDomainService().FindChatById(app, chat_id);
				std::cerr << "PRIVATE_REMOTE_DRAIN_DIAGNOSTIC " << nlohmann::json{
				    {"session_present",owner != nullptr},{"chat_present",saved != nullptr},{"captured_proxy",proxy_pid > 0},
				    {"runtime",owner ? nlohmann::json{{"running",owner->running},{"remote_stop_pending",owner->remote_stop_pending},
				        {"remote_stop_unconfirmed",owner->remote_stop_unconfirmed},{"local_stop_pending",owner->local_stop_pending},
				        {"stop_confirmed_outcome",owner->stop_outcome == uam::platform::ProcessStopOutcome::Graceful || owner->stop_outcome == uam::platform::ProcessStopOutcome::Forced},
				        {"image_preparation",static_cast<bool>(owner->claude_input_preparation)},{"local_launch",static_cast<bool>(owner->pending_local_launch)},
				        {"agent_input",static_cast<bool>(owner->pending_agent_input)},{"prompt_delivery",static_cast<bool>(owner->pending_prompt_delivery)},
				        {"local_receipt",static_cast<bool>(owner->pending_local_session_receipt)},{"skills",static_cast<bool>(owner->attached_skill_preparation)},
				        {"output_ack",!owner->pending_remote_output_ack_line.empty()},{"source_exit_ack",!owner->pending_remote_source_exit_ack_line.empty()}} : nlohmann::json::object()},
				    {"chat",saved ? nlohmann::json{{"stop_cleanup",saved->remote_stop_cleanup_pending},{"remote_process",saved->remote_process_exists}} : nlohmann::json::object()},
				    {"context_cleanup",static_cast<bool>(app.provider_context_cleanup_worker)},{"memory_tasks",app.memory_extraction_tasks.size()},
				    {"permission_tasks",app.permission_review_tasks.size()},{"checkpoint_tasks",app.turn_checkpoint_tasks.size()},
				    {"remote_stops",app.pending_acp_remote_stops.size()},{"process_stops",app.acp_process_stop_tasks.size()},
				    {"image_tasks",app.claude_input_tasks.size()},{"local_launch_tasks",app.local_acp_launch_tasks.size()},
				    {"agent_input_tasks",app.agent_input_preparation_tasks.size()},{"remote_context_tasks",app.remote_context_tasks.size()},
				    {"cli_context_tasks",app.cli_context_preparation_tasks.size()},{"memory_recall_tasks",app.memory_recall_tasks.size()},
				    {"version_running",app.runtime_cli_version_check_task.running},{"version_worker",static_cast<bool>(app.runtime_cli_version_check_task.worker)},
				    {"version_queue",app.runtime_cli_version_check_queue.size()},{"install_tasks",app.runtime_cli_install_tasks.size()},
				    {"save_tasks",app.pending_chat_save_tasks.size()},{"scheduled_saves",app.pending_chat_save_at_by_chat_id.size()}}.dump() << std::endl;
				std::cerr << "FATAL_PRIVATE_REMOTE_CODEX_DRAIN: owned runtime/workers did not settle; private environment will not unwind" << std::endl;
				std::abort();
			}
			if (proxy_pid > 0)
			{
				// WNOWAIT cannot accidentally reap a missed child and manufacture
				// successful runtime cleanup. Refuse environment teardown on doubt.
				siginfo_t info{}; errno = 0;
				if (waitid(P_PID, static_cast<id_t>(proxy_pid), &info, WEXITED | WNOHANG | WNOWAIT) != -1 || errno != ECHILD)
				{
					std::cerr << "FATAL_PRIVATE_REMOTE_CODEX_PROXY_NOT_REAPED" << std::endl;
					std::abort();
				}
			}
			stopped = true;
		}
		~RemoteCodexOwnedImageCleanup() noexcept
		{
			try { StopAndDrain(); }
			catch (...) { std::cerr << "FATAL_PRIVATE_REMOTE_CODEX_CLEANUP" << std::endl; std::abort(); }
		}
	};
}

enum class RemoteCodexOwnedRetryControl { Healthy, MissingOriginal, MixedLegacy, WorkerModel, PendingNative };

static void CheckRemoteCodexOwnedRetry(RemoteCodexOwnedRetryControl control)
{
	using Json = nlohmann::json;
	uam_test::TempDir temp("uam-private-remote-codex-owned-retry");
	const auto bin = temp.root / "bin", home = temp.root / "private-home";
	const auto workspace = fs::weakly_canonical(temp.root) / "workspace";
	fs::create_directories(bin); fs::create_directories(home / "tmp"); fs::create_directories(workspace);
	const auto runner = PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath().parent_path() / "uam-runner";
	UAM_ASSERT(fs::is_regular_file(runner) && fs::equivalent(runner, uam::remote::PackagedRunnerPath()));
	const auto manifest = Json::parse(uam_test::ReadFile(fs::path(runner.string() + ".manifest.json")));
	const auto runner_version = uam::strings::Trim(uam_test::ReadFile(fs::path(runner.string() + ".version")));
	UAM_ASSERT_EQ(runner_version, std::string(uam::constants::kAppVersion).substr(1));
	UAM_ASSERT_EQ(manifest.at("protocolVersion"), Json(uam::remote::kRunnerProtocolVersion));
	const auto fingerprint = manifest.at("sourceFingerprint").get<std::string>();
	UAM_ASSERT(fingerprint.size() == 64 && uam_test::ReadFile(runner).find(fingerprint) != std::string::npos);
	const auto ssh = bin / "ssh", shell = bin / "sh", codex = bin / "codex";
	UAM_ASSERT(uam::io::WriteTextFile(ssh, "#!/bin/sh\ncase \" $* \" in *' private-codex-owned-host '*) ;; *) exit 92;; esac\nprintf '%s\\n' \"$*\" >>\"$UAM_TEST_REMOTE_CODEX_ROOT/ssh-invocations\"\nexec \"$UAM_TEST_RUNNER\" bridge-direct\n"));
	UAM_ASSERT(uam::io::WriteTextFile(shell, "#!/bin/sh\nif [ \"$1\" = '-lc' ]; then shift; exec /bin/sh -c \"$@\"; fi\nexec /bin/sh \"$@\"\n"));
	UAM_ASSERT(uam::io::WriteTextFile(codex, R"REMOTE_CODEX(#!/usr/bin/python3 -I
import json, os, pathlib, signal, sys, time
root = pathlib.Path(os.environ['UAM_TEST_REMOTE_CODEX_ROOT'])
config = json.loads((root / 'config.json').read_text())
def store(name, value):
    temporary = root / (name + '.tmp')
    with temporary.open('w') as stream:
        json.dump(value, stream, separators=(',', ':'))
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, root / name)
def emit(value):
    print(json.dumps(value, separators=(',', ':')), flush=True)
def event(method, params):
    emit(dict(jsonrpc='2.0', method=method, params=params))
def reply(request, result):
    emit(dict(jsonrpc='2.0', id=request['id'], result=result))
if sys.argv[1:] == ['--version']:
    print('codex-cli 0.162.1', flush=True)
    sys.exit(0)
assert sys.argv[1:] == ['--disable', 'computer_use', 'app-server', '--listen', 'stdio://'], sys.argv[1:]
def terminate(signum, frame):
    store('exit.json', dict(pid=os.getpid(), signal=signum))
    sys.exit(0)
signal.signal(signal.SIGTERM, terminate)
signal.signal(signal.SIGINT, terminate)
store('launch.json', dict(pid=os.getpid(), argv=sys.argv[1:], cwd=os.getcwd(), executable=sys.argv[0]))
turns = 0
for raw in sys.stdin:
    # Record every actual stdin line before parsing or assertions, including forbidden extra turns.
    with (root / 'stdin.jsonl').open('a') as stream:
        stream.write(raw)
        stream.flush()
        os.fsync(stream.fileno())
    request = json.loads(raw)
    method = request.get('method', '')
    if method == 'initialized':
        continue
    if method == 'initialize':
        reply(request, dict(userAgent='codex-cli/0.162.1'))
    elif method == 'model/list':
        reply(request, dict(data=[dict(id=config['model'], displayName='Private Codex model', isDefault=True,
            defaultReasoningEffort='low', supportedReasoningEfforts=[dict(reasoningEffort='low'), dict(reasoningEffort='high')], additionalSpeedTiers=['fast'])],
            currentModelId=config['model'], nextCursor=None))
    elif method == 'account/rateLimits/read':
        reply(request, dict(rateLimits={}))
    elif method == 'thread/start':
        assert request['params']['model'] == config['model']
        reply(request, dict(thread=dict(id=config['native']), model=config['model'], approvalsReviewer='user'))
    elif method == 'turn/start':
        turns += 1
        store('input-%d.json' % turns, dict(pid=os.getpid(), raw=raw, envelope=request))
        assert turns in (1, 2), 'Unauthorized extra native turn'
        assert request['params']['threadId'] == config['native']
        while not (root / ('release-%d' % turns)).exists():
            time.sleep(0.005)
        turn = dict(id='private-turn-%d' % turns, status='inProgress')
        reply(request, dict(turn=turn))
        event('turn/started', dict(threadId=config['native'], turn=turn))
        event('item/agentMessage/delta', dict(threadId=config['native'], turnId=turn['id'], itemId='answer-%d' % turns,
            delta=config['answer'] if turns == 1 else config['retry_answer']))
        event('turn/completed', dict(threadId=config['native'], turn=dict(id=turn['id'], status='completed', error=None)))
    elif method == 'turn/interrupt':
        reply(request, {})
    else:
        store('unexpected.json', request)
        raise RuntimeError('Unexpected private Codex method: ' + method)
store('exit.json', dict(pid=os.getpid(), eof=True))
)REMOTE_CODEX"));
	for (const auto& executable : {ssh, shell, codex}) fs::permissions(executable, fs::perms::owner_all);
	std::vector<std::unique_ptr<uam_test::ScopedEnvVar>> environment;
	const auto env = [&](const std::string& key, const std::string& value)
	{ environment.push_back(std::make_unique<uam_test::ScopedEnvVar>(key, value)); };
	env("PATH", bin.string() + ":/usr/bin:/bin"); env("HOME", home.string()); env("USERPROFILE", home.string());
	env("CODEX_HOME", (home / "codex").string()); env("TMPDIR", (home / "tmp").string());
	env("UAM_DATA_DIR", (temp.root / "controller").string()); env("UAM_TEST_RUNNER", runner.string());
	env("UAM_TEST_REMOTE_CODEX_ROOT", temp.root.string());
	const std::string native = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", model = "private-codex-owned-model";
	const std::string task = "Ordinary private remote text.", answer = "Actual private Codex ordinary turn.", retry_answer = "Actual private Codex retained image turn.";
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "config.json", Json{{"native", native}, {"model", model}, {"answer", answer}, {"retry_answer", retry_answer}}.dump()));
	std::string png, gif;
	UAM_ASSERT(uam::base64::Decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==", png));
	UAM_ASSERT(uam::base64::Decode("R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7", gif));
	UAM_ASSERT(!png.empty() && !gif.empty() && png != gif);
	uam::AppState app; app.data_root = temp.root / "controller";
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.settings.central_provider_configuration.enabled = app.settings.central_provider_configuration.uam_control_enabled = false;
	app.settings.git_enabled = app.settings.git_auto_worktree = app.settings.svn_enabled = app.settings.computer_use_globally_enabled = false;
	ExecutionHost host; host.id = "private-remote-codex-owned"; host.label = "Private Codex host";
	host.transport = "ssh"; host.ssh_alias = "private-codex-owned-host"; host.platform = "macos";
	host.runner_status = "ready"; host.runner_version = runner_version; host.runner_protocol_version = uam::remote::kRunnerProtocolVersion;
	app.settings.execution_hosts = {host}; uam::execution_hosts::Normalize(app.settings.execution_hosts);
	const auto* selected = uam::execution_hosts::Find(app.settings.execution_hosts, host.id);
	UAM_ASSERT(selected && uam::execution_hosts::SameConnection(*selected, host)); host = *selected;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli, false);
	chat.execution_host_id = host.id; chat.workspace_directory = workspace.string(); chat.model_id = model;
	chat.reasoning_effort = "high"; chat.service_tier = "fast"; chat.service_tier_explicit = true;
	chat.approval_mode = "default"; chat.command_safety_tier = "safe";
	chat.memory_enabled = chat.uam_control_enabled = chat.computer_use_enabled = false;
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	const auto initial = ChatRepository::LoadLocalChat(app.data_root, chat.id, true); UAM_ASSERT(initial); app.chats.push_back(*initial);
	ChatSession unrelated = ChatDomainService().CreateNewChat("", uam::provider_ids::kCodexCli, false);
	unrelated.workspace_directory = workspace.string(); unrelated.messages = {{MessageRole::User, "Unrelated saved row."}};
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, unrelated)); app.chats.push_back(unrelated);
	const auto unrelated_path = AppPaths::UamChatFilePath(app.data_root, unrelated.id);
	const auto unrelated_bytes = uam_test::ReadFile(unrelated_path);
	ChatDomainService().SelectChatById(app, chat.id); app.composer_text = "Keep unrelated draft.";
	RemoteCodexOwnedImageCleanup cleanup{app, chat.id, temp.root / "release-1"};
	const auto wait = [&](const auto& predicate)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (std::chrono::steady_clock::now() < deadline)
		{
			cleanup.Tick();
			if (predicate()) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return false;
	};
	const auto live = [&]() -> ChatSession& { auto* value = ChatDomainService().FindChatById(app, chat.id); UAM_ASSERT(value); return *value; };
	const auto runtime = [&]() -> uam::AcpSessionState& { auto* value = uam::FindAcpSessionForChat(app, chat.id); UAM_ASSERT(value); return *value; };
	const auto count_turns = [&]()
	{
		std::size_t count = 0; std::istringstream stream(uam_test::ReadFile(temp.root / "stdin.jsonl"));
		for (std::string line; std::getline(stream, line);)
		{ const auto value = Json::parse(line); if (value.value("method", "") == "turn/start") ++count; }
		return count;
	};
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(app, chat.id, task, {}, {}, false, &error) && error.empty());
	UAM_ASSERT(wait([&]() { return fs::is_regular_file(temp.root / "input-1.json") && runtime().running && runtime().initialized && runtime().session_ready && runtime().processing &&
	    live().native_session_id == native && live().remote_turn_reconnect_pending && live().remote_prompt_delivery_id.empty() && live().remote_prompt_delivery_payload.empty() &&
	    !runtime().pending_prompt_delivery && app.pending_chat_save_at_by_chat_id.empty() && app.pending_chat_save_tasks.empty(); }));
	UAM_ASSERT_EQ(runtime().protocol_kind, std::string("codex-app-server"));
	UAM_ASSERT_EQ(runtime().session_id, native); UAM_ASSERT_EQ(runtime().codex_thread_id, native); UAM_ASSERT_EQ(runtime().current_model_id, model);
	UAM_ASSERT_EQ(runtime().process_execution_host_id, host.id); UAM_ASSERT_EQ(runtime().remote_runner_protocol_version, 3);
	UAM_ASSERT(!runtime().remote_output_delivery_token.empty() && runtime().remote_output_delivery_token == live().remote_process_control_token);
	const auto launch = Json::parse(uam_test::ReadFile(temp.root / "launch.json"));
	const auto ordinary = Json::parse(uam_test::ReadFile(temp.root / "input-1.json"));
	UAM_ASSERT_EQ(launch.at("pid"), ordinary.at("pid")); UAM_ASSERT_EQ(launch.at("cwd"), Json(workspace.string()));
	UAM_ASSERT_EQ(launch.at("executable"), Json(codex.string())); UAM_ASSERT_EQ(count_turns(), std::size_t{1});
	UAM_ASSERT_EQ(ordinary.at("envelope").at("params").at("input").size(), std::size_t{1});
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "release-1", "Complete genuine ordinary turn"));
	UAM_ASSERT(wait([&]() { const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true); return !runtime().processing && runtime().session_ready && runtime().pending_request_methods.empty() &&
	    saved && saved->messages.size() == 2 && saved->messages.back().content == answer && saved->native_session_id == native && !saved->remote_turn_reconnect_pending &&
	    saved->remote_pending_requests.empty() && live().remote_pending_requests.empty() &&
	    app.pending_chat_save_at_by_chat_id.empty() && app.pending_chat_save_tasks.empty(); }));
	UAM_ASSERT(runtime().last_error.empty());
	const auto process = runtime().child_pid; const auto token = runtime().remote_output_delivery_token;
	const auto publish = [&](const std::string& content)
	{
		const auto revision = ChatRepository::ChatStorageRevision(app.data_root, chat.id); UAM_ASSERT(revision && *revision != "missing");
		ChatSession candidate = live(); Message row; row.role = MessageRole::User; row.content = content;
		row.created_at = uam::time::TimestampNow(); row.acp_prompt_not_sent = true;
		for (const auto& [name, bytes, mime] : std::array<std::tuple<std::string, std::string, std::string>, 2>{{{"First exact PNG", png, "image/png"}, {"Second exact GIF", gif, "image/gif"}}})
		{
			MessageAttachment image; image.id = name; image.name = name; image.kind = "image"; image.source = AttachmentSource::NativeInlinePending;
			image.origin_execution_host_id = host.id; image.native_part_key = "v1:data-fixture/" + name; image.native_source_field = "data-fixture/user/image/" + name;
			std::string failure; image.native_inline_payload = uam::native_assets::MakeInlinePayload(uam::base64::Encode(bytes), mime, std::make_shared<uam::native_assets::PayloadBudget>(), failure);
			UAM_ASSERT(image.native_inline_payload && failure.empty()); image.mime_type = image.native_inline_payload->MimeType(); image.size_bytes = image.native_inline_payload->DecodedSize(); image.content_digest = image.native_inline_payload->Digest(); row.attachments.push_back(std::move(image));
		}
		candidate.messages.push_back(std::move(row)); const auto before = live().messages;
		auto worker = std::async(std::launch::async, [root = app.data_root, candidate = std::move(candidate), revision = *revision]() mutable
		{
			std::string failure; auto prepared = uam::PreparedNativeMediaChatSave::Prepare(root, std::move(candidate), revision, failure);
			if (!prepared || !prepared->Publish(failure) || !prepared->AcknowledgeOriginals(failure)) return std::make_pair(std::optional<ChatSession>{}, failure);
			return std::make_pair(prepared->TakeCommittedChat(), failure);
		});
		const auto result = worker.get(); UAM_ASSERT(result.first && result.second.empty()); live() = *result.first;
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true); UAM_ASSERT(saved && saved->messages == live().messages);
		UAM_ASSERT_EQ(saved->messages.size(), before.size() + 1); UAM_ASSERT(std::equal(before.begin(), before.end(), saved->messages.begin()));
		UAM_ASSERT_EQ(saved->native_session_id, native); UAM_ASSERT_EQ(saved->execution_host_id, host.id); UAM_ASSERT_EQ(saved->model_id, model);
		UAM_ASSERT_EQ(saved->messages.back().attachments.size(), std::size_t{2});
		for (std::size_t index = 0; index < 2; ++index)
		{
			const auto& image = saved->messages.back().attachments[index]; UAM_ASSERT(image.source == AttachmentSource::OwnedNativeAsset && image.path.empty() && !image.native_inline_payload);
			UAM_ASSERT(uam::native_assets::IsKeyForOwner(chat.id, image.native_asset_key) && !uam::native_assets::IsKeyForOwner(native, image.native_asset_key));
			const auto original = uam::native_assets::ReadOwned(app.data_root, chat.id, image.native_asset_key); UAM_ASSERT(original.ok && original.bytes == (index == 0 ? png : gif));
		}
		const int index = static_cast<int>(live().messages.size() - 1);
		const auto recoverable = uam::RecoverableUnsentAcpMessageIndices(app, live());
		if (recoverable != std::vector<int>{index})
		{
			// Fixed labels, booleans, bounded counts and row indices only. Never print
			// IDs, tokens, paths, payloads, prompts or arbitrary native error strings.
			const auto& owner = live(); const auto& session = runtime();
			const auto known = uam::KnownUnsentAcpMessageIndices(app, owner);
			const auto bounded_indices = [](const std::vector<int>& values)
			{ return std::vector<int>(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(values.size(), 16))); };
			const auto& row = owner.messages[static_cast<std::size_t>(index)];
			const auto disk = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
			std::cerr << "PRIVATE_REMOTE_CODEX_RETRY_ELIGIBILITY_DIAGNOSTIC " << Json{
			    {"expected_index", index}, {"recoverable_count", recoverable.size()}, {"recoverable_indices", bounded_indices(recoverable)},
			    {"known_count", known.size()}, {"known_indices", bounded_indices(known)},
			    {"chat", {
			        {"rows", owner.messages.size()}, {"messages_loaded", owner.messages_loaded}, {"imported_read_only", owner.imported_read_only},
			        {"queue_storage_error", owner.acp_queue_storage_error}, {"queue_held", owner.acp_queue_held},
			        {"queued_count", owner.acp_queued_prompts.size()}, {"dispatched_count", owner.acp_dispatched_queued_prompt_count},
			        {"remote_turn_reconnect_pending", owner.remote_turn_reconnect_pending}, {"remote_restart_pending", owner.remote_restart_pending},
			        {"remote_stop_cleanup_pending", owner.remote_stop_cleanup_pending}, {"remote_source_exit_pending", owner.remote_source_exit_pending},
			        {"remote_active_turn_present", !owner.remote_active_turn_id.empty()}, {"delivery_session_present", !owner.remote_prompt_delivery_session_id.empty()},
			        {"delivery_id_present", !owner.remote_prompt_delivery_id.empty()}, {"delivery_payload_present", !owner.remote_prompt_delivery_payload.empty()},
			        {"pending_request_count", owner.remote_pending_requests.size()}, {"interaction_response_count", owner.remote_interaction_responses.size()},
			        {"native_present", !owner.native_session_id.empty()}, {"native_matches_peer", owner.native_session_id == native},
			        {"remote_process_exists", owner.remote_process_exists}, {"process_control_present", !owner.remote_process_control_token.empty()},
			        {"process_control_matches_runtime", owner.remote_process_control_token == session.remote_output_delivery_token},
			        {"remote_user_index", owner.remote_turn_user_message_index}, {"side_cleanup_requested", owner.side_cleanup_requested},
			        {"stop_in_progress", uam::AcpStopInProgress(app, owner.id)}, {"worktree_operation", app.worktree_operation_chat_ids.contains(owner.id)}}},
			    {"runtime", {
			        {"running", session.running}, {"initialized", session.initialized}, {"ready", session.session_ready}, {"processing", session.processing},
			        {"provider_matches_chat", session.provider_id == owner.provider_id}, {"session_present", !session.session_id.empty()},
			        {"session_matches_chat_native", session.session_id == owner.native_session_id}, {"codex_thread_matches_session", session.codex_thread_id == session.session_id},
			        {"queued_prompt_present", !session.queued_prompt.empty()}, {"queued_count", session.queued_user_prompts.size()}, {"queue_held", session.queue_held},
			        {"waiting_for_input", uam::AcpSessionIsWaitingForInput(session)}, {"pending_runtime_request", uam::AcpSessionHasPendingRuntimeRequest(session)},
			        {"background_task_count", ProviderRuntimeRegistry::ResolveById(session.provider_id).AcpBackgroundTaskCount(session)},
			        {"pending_cancel", uam::AcpSessionHasPendingCancel(session)}, {"prompt_request_nonzero", session.prompt_request_id != 0},
			        {"pending_request_count", session.pending_request_methods.size()}, {"pending_steer_count", session.pending_steer_requests.size()},
			        {"reconnect_pending", session.reconnect_pending}, {"local_stop_pending", session.local_stop_pending}, {"remote_stop_pending", session.remote_stop_pending},
			        {"remote_stop_unconfirmed", session.remote_stop_unconfirmed}, {"recovering_remote_turn", session.recovering_remote_turn}, {"recovering_remote_process", session.recovering_remote_process},
			        {"pending_compaction", static_cast<bool>(session.pending_compaction)}, {"pending_prompt_delivery", static_cast<bool>(session.pending_prompt_delivery)},
			        {"pending_local_launch", static_cast<bool>(session.pending_local_launch)}, {"pending_local_session_receipt", static_cast<bool>(session.pending_local_session_receipt)},
			        {"pending_agent_input", static_cast<bool>(session.pending_agent_input)}, {"pending_queued_history", static_cast<bool>(session.pending_queued_history)},
			        {"model_discovery_only", session.model_discovery_only}, {"goal_internal_session", session.goal_internal_session}, {"goal_review_scheduled", session.goal_review_scheduled},
			        {"managed_run_present", !session.managed_agent_run_id.empty()}, {"checkpoint_commit_pending", session.turn_checkpoint_commit_pending},
			        {"checkpoint_preflight_pending", session.turn_checkpoint_preflight_pending}, {"turn_first_index", session.turn_first_user_message_index},
			        {"turn_last_index", session.turn_user_message_index}, {"last_error_present", !session.last_error.empty()},
			        {"save_task_count", app.pending_chat_save_tasks.size()}, {"scheduled_save_count", app.pending_chat_save_at_by_chat_id.size()}}},
			    {"row", {
			        {"user", row.role == MessageRole::User}, {"unsent", row.acp_prompt_not_sent}, {"discarded", row.acp_prompt_discarded},
			        {"created_at_present", !row.created_at.empty()}, {"provider_empty_or_matching", row.provider.empty() || row.provider == owner.provider_id},
			        {"tool_count", row.tool_calls.size()}, {"thought_count", row.thoughts.size()}, {"plan_count", row.plan_entries.size()},
			        {"plan_summary_present", !row.plan_summary.empty()}, {"block_count", row.blocks.size()}, {"attachment_count", row.attachments.size()}}},
			    {"saved", {
			        {"present", static_cast<bool>(disk)}, {"rows", disk ? disk->messages.size() : 0},
			        {"native_matches_live", disk && disk->native_session_id == owner.native_session_id}, {"native_matches_peer", disk && disk->native_session_id == native},
			        {"remote_process_exists", disk && disk->remote_process_exists}, {"control_matches_live", disk && disk->remote_process_control_token == owner.remote_process_control_token},
			        {"remote_reconnect_pending", disk && disk->remote_turn_reconnect_pending}, {"remote_active_turn_present", disk && !disk->remote_active_turn_id.empty()},
			        {"delivery_session_present", disk && !disk->remote_prompt_delivery_session_id.empty()}, {"delivery_id_present", disk && !disk->remote_prompt_delivery_id.empty()},
			        {"delivery_payload_present", disk && !disk->remote_prompt_delivery_payload.empty()}, {"pending_request_count", disk ? disk->remote_pending_requests.size() : 0}}}
			}.dump() << std::endl;
		}
		UAM_ASSERT_EQ(uam::RecoverableUnsentAcpMessageIndices(app, live()), std::vector<int>{index}); return index;
	};
	const int index = publish("Retry both retained images in order.");
	const auto authored = live().messages[static_cast<std::size_t>(index)];
	const auto primary = uam_test::ReadFile(AppPaths::UamChatFilePath(app.data_root, chat.id));
	const auto stdin_before = uam_test::ReadFile(temp.root / "stdin.jsonl");
	std::string retry_error; const bool accepted = uam::RetryFailedAcpMessage(app, chat.id, index, &retry_error);
	if (!accepted)
	{
		// Intended pre-opt-in RED is qualified only after every real Send/ACK/native/store/eligibility prerequisite.
		UAM_ASSERT_EQ(retry_error, std::string("Saved image Retry needs this chat's existing ready SSH session. Reopen it or attach the files again."));
		UAM_ASSERT_EQ(live().messages[static_cast<std::size_t>(index)], authored);
		UAM_ASSERT_EQ(uam_test::ReadFile(AppPaths::UamChatFilePath(app.data_root, chat.id)), primary);
		UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "stdin.jsonl"), stdin_before); UAM_ASSERT_EQ(count_turns(), std::size_t{1});
		cleanup.StopAndDrain(); UAM_ASSERT(cleanup.Quiet());
	}
	UAM_ASSERT(accepted && retry_error.empty());
	UAM_ASSERT(wait([&]() { return fs::is_regular_file(temp.root / "input-2.json") && runtime().processing && !runtime().pending_prompt_delivery &&
	    live().remote_prompt_delivery_id.empty() && live().remote_prompt_delivery_payload.empty() && app.pending_chat_save_at_by_chat_id.empty() && app.pending_chat_save_tasks.empty(); }));
	const auto retried = Json::parse(uam_test::ReadFile(temp.root / "input-2.json"));
	UAM_ASSERT_EQ(retried.at("pid"), launch.at("pid")); UAM_ASSERT_EQ(runtime().child_pid, process); UAM_ASSERT_EQ(runtime().remote_output_delivery_token, token);
	const auto& request = retried.at("envelope"); UAM_ASSERT_EQ(request.at("method"), Json("turn/start"));
	auto before_params = ordinary.at("envelope").at("params"), after_params = request.at("params");
	before_params.erase("input"); after_params.erase("input"); UAM_ASSERT_EQ(after_params, before_params); // Exact native policy/model/effort/tier/sandbox fields.
	UAM_ASSERT_EQ(request.at("params").at("input").size(), std::size_t{3});
	const auto& input = request.at("params").at("input"); UAM_ASSERT_EQ(input[0].at("type"), Json("text"));
	const auto text = input[0].at("text").get<std::string>(); UAM_ASSERT(text.find(authored.content) != std::string::npos);
	const auto first_name = text.find(authored.attachments[0].name), second_name = text.find(authored.attachments[1].name);
	UAM_ASSERT(first_name != std::string::npos && second_name != std::string::npos && first_name < second_name);
	for (std::size_t image = 0; image < 2; ++image)
	{
		const auto& part = input[image + 1]; const auto prefix = std::string("data:") + (image == 0 ? "image/png" : "image/gif") + ";base64,";
		UAM_ASSERT_EQ(part.at("type"), Json("image")); UAM_ASSERT(!part.contains("path")); const auto url = part.at("url").get<std::string>(); UAM_ASSERT(url.starts_with(prefix));
		std::string decoded; UAM_ASSERT(uam::base64::Decode(url.substr(prefix.size()), decoded)); UAM_ASSERT_EQ(decoded, image == 0 ? png : gif);
		UAM_ASSERT(request.dump().find(authored.attachments[image].native_asset_key) == std::string::npos);
	}
	UAM_ASSERT(request.dump().find("localImage") == std::string::npos && request.dump().find("file://") == std::string::npos);
	UAM_ASSERT(retried.at("raw").get<std::string>().size() <= 2 * 1024 * 1024); UAM_ASSERT_EQ(count_turns(), std::size_t{2});
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "release-2", "Complete actual retained image turn"));
	UAM_ASSERT(wait([&]() { const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true); return !runtime().processing && runtime().session_ready && runtime().pending_request_methods.empty() && saved &&
	    saved->messages.size() == 4 && saved->messages.back().content == retry_answer && !saved->messages[static_cast<std::size_t>(index)].acp_prompt_not_sent &&
	    !saved->remote_turn_reconnect_pending && saved->remote_pending_requests.empty() && live().remote_pending_requests.empty() && app.pending_chat_save_at_by_chat_id.empty() && app.pending_chat_save_tasks.empty(); }));
	UAM_ASSERT(!runtime().saved_image_retry && !runtime().claude_input_preparation && !runtime().pending_prompt_delivery && runtime().last_error.empty());
	if (control != RemoteCodexOwnedRetryControl::Healthy)
	{
		const int bad_index = publish("Refuse this complete third input if authority or sources change.");
		const auto bad_original = live().messages[static_cast<std::size_t>(bad_index)];
		fs::path missing_original_path;
		if (control == RemoteCodexOwnedRetryControl::MissingOriginal)
		{
			std::vector<std::string> parts; std::istringstream key(bad_original.attachments[0].native_asset_key);
			for (std::string part; std::getline(key, part, ':');) parts.push_back(part);
			UAM_ASSERT(parts.size() == 5 && parts[0] == "na1");
			missing_original_path = app.data_root / std::string(uam::native_assets::kStoreDirectory) / parts[1] / parts[2] / (parts[3] + ".bin");
			UAM_ASSERT(fs::remove(missing_original_path));
		}
		if (control == RemoteCodexOwnedRetryControl::MixedLegacy)
		{
			MessageAttachment legacy; legacy.id = "forbidden-remote-path"; legacy.name = "Forbidden legacy image"; legacy.kind = "image"; legacy.mime_type = "image/png"; legacy.path = "real-image.png";
			UAM_ASSERT(uam::io::WriteBinaryFile(workspace / "real-image.png", png)); live().messages[static_cast<std::size_t>(bad_index)].attachments.push_back(legacy);
			UAM_ASSERT(ChatRepository::SaveChat(app.data_root, live()));
		}
		const auto bad_authored = live().messages[static_cast<std::size_t>(bad_index)]; const auto no_third = uam_test::ReadFile(temp.root / "stdin.jsonl");
		const auto expected_messages = live().messages;
		const auto refusal_primary = uam_test::ReadFile(AppPaths::UamChatFilePath(app.data_root, chat.id));
		const auto expected_process_exists = live().remote_process_exists;
		const auto expected_process_control = live().remote_process_control_token;
		const auto saved_native = live().native_session_id; const auto current_model = runtime().current_model_id;
		const auto save_completions = std::make_shared<std::atomic<int>>(0);
		std::string refusal; const bool captured = uam::RetryFailedAcpMessage(app, chat.id, bad_index, &refusal);
		if (control == RemoteCodexOwnedRetryControl::MixedLegacy) UAM_ASSERT(!captured && !refusal.empty());
		else
		{
			UAM_ASSERT(captured && refusal.empty());
			if (control == RemoteCodexOwnedRetryControl::MissingOriginal || control == RemoteCodexOwnedRetryControl::WorkerModel || control == RemoteCodexOwnedRetryControl::PendingNative)
			{
				const auto worker_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
				while (!runtime().claude_input_preparation && runtime().processing && std::chrono::steady_clock::now() < worker_deadline)
				{ (void)uam::FlushPendingChatSaves(app); (void)uam::acp_detail::SendQueuedPromptIfReady(app, runtime(), live()); std::this_thread::yield(); }
				UAM_ASSERT(runtime().claude_input_preparation && runtime().saved_image_retry);
				const auto state = runtime().claude_input_preparation->state;
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
				while (!state->finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
				UAM_ASSERT(state->finished.load(std::memory_order_acquire));
				if (control == RemoteCodexOwnedRetryControl::MissingOriginal) UAM_ASSERT(!state->error.empty() && state->message.is_null());
				else
				{
					UAM_ASSERT(state->error.empty() && state->message.is_object());
					const auto& parts = state->message.at("message").at("content"); UAM_ASSERT_EQ(parts.size(), std::size_t{3});
					for (std::size_t image = 0; image < 2; ++image)
					{ std::string decoded; UAM_ASSERT(uam::base64::Decode(parts[image + 1].at("source").at("data").get<std::string>(), decoded)); UAM_ASSERT_EQ(decoded, image == 0 ? png : gif); }
				}
				UAM_ASSERT(state->sealed_message.is_null() && !runtime().pending_prompt_delivery && count_turns() == 2);
				if (control == RemoteCodexOwnedRetryControl::WorkerModel) runtime().current_model_id = "negative-drift-only-model";
				else if (control == RemoteCodexOwnedRetryControl::PendingNative)
				{
					// SSH durable admission is gated independently of the local-only defer_prompt_save flag.
					UAM_ASSERT(uam::acp_detail::CanDeferRemotePromptAdmission(app, runtime(), live()));
					UAM_ASSERT(!app.pending_chat_save_at_by_chat_id.contains(chat.id) && !app.pending_chat_save_tasks.contains(chat.id));
					const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
					while (!runtime().pending_prompt_delivery && std::chrono::steady_clock::now() < deadline)
					{ (void)uam::PollAllAcpSessions(app); if (!runtime().pending_prompt_delivery) (void)uam::FlushPendingChatSaves(app); std::this_thread::yield(); }
					UAM_ASSERT(runtime().pending_prompt_delivery && !state->sealed_message.is_null() && count_turns() == 2);
					UAM_ASSERT_EQ(state->sealed_method, std::string("turn/start")); UAM_ASSERT_EQ(state->sealed_message.at("params").at("threadId"), Json(native));
					UAM_ASSERT(state->sealed_request_id > 0 && runtime().prompt_request_id == 0);
					// Public state joins the actual sealed remote packet to its unconsumed durable save.
					UAM_ASSERT(runtime().saved_image_retry && runtime().saved_image_retry->remote_scope);
					const auto& scope = *runtime().saved_image_retry->remote_scope;
					const auto* packet_host = uam::execution_hosts::Find(app.settings.execution_hosts, live().execution_host_id);
					UAM_ASSERT(packet_host && uam::execution_hosts::SameConnection(*packet_host, scope.host) && uam::execution_hosts::SameConnection(*packet_host, host));
					UAM_ASSERT(scope.proxy_protocol == 3 && runtime().remote_runner_protocol_version == 3 && packet_host->runner_protocol_version == 3);
					UAM_ASSERT_EQ(scope.native_session_id, native); UAM_ASSERT_EQ(scope.runtime_session_id, native);
					UAM_ASSERT_EQ(scope.process_id, uam::acp_detail::AcpProcessHandleLabel(runtime()));
					UAM_ASSERT_EQ(runtime().child_pid, process); UAM_ASSERT_EQ(scope.proxy_token, token);
					UAM_ASSERT_EQ(runtime().remote_output_delivery_token, token); UAM_ASSERT_EQ(live().remote_process_control_token, token);
					UAM_ASSERT_EQ(live().native_session_id, native); UAM_ASSERT_EQ(runtime().session_id, native);
					UAM_ASSERT(live().remote_turn_reconnect_pending && live().remote_prompt_delivery_session_id == "acp-" + chat.id && !live().remote_prompt_delivery_id.empty());
					UAM_ASSERT_EQ(live().remote_prompt_delivery_payload, uam::acp_detail::SerializeStructuredMessage(runtime(), state->sealed_message));
					UAM_ASSERT_EQ(live().remote_pending_requests.size(), std::size_t{1});
					const auto& pending_request = live().remote_pending_requests.front();
					UAM_ASSERT(pending_request.request_id == state->sealed_request_id && pending_request.method == state->sealed_method &&
					    pending_request.turn_serial == runtime().turn_serial && !pending_request.response_consumed);
					UAM_ASSERT(app.pending_chat_save_at_by_chat_id.contains(chat.id));
					const auto pending_save = app.pending_chat_save_tasks.find(chat.id);
					UAM_ASSERT(pending_save != app.pending_chat_save_tasks.end() && pending_save->second);
					UAM_ASSERT(uam::DeferChatSaveResult(app, chat.id, [save_completions](bool) { ++*save_completions; }));
					UAM_ASSERT_EQ(save_completions->load(), 0); // No owner poll/Flush after final iteration staged its actual packet.
					// This mutates only the live chat field initially; no explicit SaveChat is used to inject durable drift.
					live().native_session_id.clear(); // Negative drift of the saved actual parsed thread, never positive authority.
				}
			}
			UAM_ASSERT(wait([&]() { return !runtime().saved_image_retry && !runtime().claude_input_preparation && !runtime().pending_prompt_delivery && !runtime().processing && app.pending_chat_save_at_by_chat_id.empty() && app.pending_chat_save_tasks.empty(); }));
			UAM_ASSERT(!runtime().last_error.empty());
			if (control == RemoteCodexOwnedRetryControl::PendingNative) UAM_ASSERT_EQ(save_completions->load(), 1);
		}
		// Settle and inspect the real saved refusal before restoring any drift field.
		UAM_ASSERT(app.pending_chat_save_at_by_chat_id.empty() && app.pending_chat_save_tasks.empty());
		const auto refused = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
		UAM_ASSERT(refused && refused->messages.size() == expected_messages.size());
		UAM_ASSERT_EQ(live().messages.size(), expected_messages.size());
		const auto expected_native = control == RemoteCodexOwnedRetryControl::PendingNative ? std::string{} : saved_native;
		UAM_ASSERT_EQ(refused->native_session_id, expected_native);
		UAM_ASSERT_EQ(live().native_session_id, expected_native);
		for (std::size_t row_index = 0; row_index < expected_messages.size(); ++row_index)
		{
			for (const auto* row : std::array<const Message*, 2>{&refused->messages[row_index], &live().messages[row_index]})
			{
				UAM_ASSERT_EQ(row->role, expected_messages[row_index].role);
				UAM_ASSERT_EQ(row->content, expected_messages[row_index].content);
				UAM_ASSERT_EQ(row->created_at, expected_messages[row_index].created_at);
				UAM_ASSERT_EQ(row->attachments, expected_messages[row_index].attachments);
			}
		}
		UAM_ASSERT_EQ(refused->execution_host_id, host.id); UAM_ASSERT_EQ(live().execution_host_id, host.id);
		UAM_ASSERT_EQ(refused->provider_id, std::string(uam::provider_ids::kCodexCli));
		UAM_ASSERT_EQ(runtime().child_pid, process); UAM_ASSERT_EQ(runtime().remote_output_delivery_token, token);
		UAM_ASSERT_EQ(runtime().session_id, native); UAM_ASSERT_EQ(runtime().codex_thread_id, native);
		UAM_ASSERT_EQ(refused->remote_process_exists, expected_process_exists); UAM_ASSERT_EQ(live().remote_process_exists, expected_process_exists);
		UAM_ASSERT_EQ(refused->remote_process_control_token, expected_process_control); UAM_ASSERT_EQ(live().remote_process_control_token, expected_process_control);
		if (control == RemoteCodexOwnedRetryControl::MixedLegacy)
		{
			UAM_ASSERT_EQ(uam_test::ReadFile(AppPaths::UamChatFilePath(app.data_root, chat.id)), refusal_primary);
			UAM_ASSERT_EQ(refused->messages, expected_messages); UAM_ASSERT_EQ(live().messages, expected_messages);
		}
		UAM_ASSERT_EQ(live().messages[static_cast<std::size_t>(bad_index)].content, bad_authored.content);
		UAM_ASSERT_EQ(live().messages[static_cast<std::size_t>(bad_index)].attachments, bad_authored.attachments);
		UAM_ASSERT_EQ(live().messages[static_cast<std::size_t>(bad_index)].created_at, bad_authored.created_at);
		UAM_ASSERT_EQ(count_turns(), std::size_t{2}); UAM_ASSERT(!fs::exists(temp.root / "input-3.json")); UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "stdin.jsonl"), no_third);
		if (!missing_original_path.empty()) UAM_ASSERT(uam::io::WriteBinaryFile(missing_original_path, png));
			live().native_session_id = saved_native; runtime().current_model_id = current_model;
		(void)uam::PollAllAcpSessions(app); (void)uam::FlushPendingChatSaves(app); UAM_ASSERT_EQ(count_turns(), std::size_t{2});
	}
	for (std::size_t image = 0; image < authored.attachments.size(); ++image)
	{ const auto original = uam::native_assets::ReadOwned(app.data_root, chat.id, authored.attachments[image].native_asset_key); UAM_ASSERT(original.ok && original.bytes == (image == 0 ? png : gif)); }
	UAM_ASSERT_EQ(live().messages[static_cast<std::size_t>(index)].content, authored.content);
	UAM_ASSERT_EQ(live().messages[static_cast<std::size_t>(index)].attachments, authored.attachments);
	UAM_ASSERT_EQ(app.composer_text, std::string("Keep unrelated draft.")); UAM_ASSERT_EQ(uam_test::ReadFile(unrelated_path), unrelated_bytes);
	const auto stdin_before_stop = uam_test::ReadFile(temp.root / "stdin.jsonl"); cleanup.StopAndDrain(); UAM_ASSERT(cleanup.Quiet());
	UAM_ASSERT_EQ(count_turns(), std::size_t{2}); UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "stdin.jsonl"), stdin_before_stop);
}

UAM_TEST(CodexRemoteSavedOwnedRetryDeliversDataUrlsThroughActualRunner)
{
	CheckRemoteCodexOwnedRetry(RemoteCodexOwnedRetryControl::Healthy);
}
UAM_TEST(CodexRemoteSavedOwnedRetryRejectsMissingAndMixedCompleteSources)
{
	for (const auto control : {RemoteCodexOwnedRetryControl::MissingOriginal, RemoteCodexOwnedRetryControl::MixedLegacy}) CheckRemoteCodexOwnedRetry(control);
}
UAM_TEST(CodexRemoteSavedOwnedRetryRevokesWorkerAndDurableNativeDrift)
{
	for (const auto control : {RemoteCodexOwnedRetryControl::WorkerModel, RemoteCodexOwnedRetryControl::PendingNative}) CheckRemoteCodexOwnedRetry(control);
}
#endif


#if UAM_ENABLE_RUNTIME_CLAUDE_CLI && UAM_ENABLE_SSH && defined(__APPLE__)
static void CheckClaudeStoppedRemoteContinuation(bool owned_retry)
{
	using Json = nlohmann::json;
	uam_test::TempDir temp("uam-remote-claude-cold-prerequisite");
	const auto bin = temp.root / "bin", profile = temp.root / "private-home";
	const auto workspace = fs::weakly_canonical(temp.root) / "remote-workspace"; // Match actual child cwd through macOS temp aliases.
	fs::create_directories(bin); fs::create_directories(workspace); fs::create_directories(profile / "tmp");
	const auto runner = PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath().parent_path() / "uam-runner";
	UAM_ASSERT(fs::is_regular_file(runner) && fs::equivalent(runner, uam::remote::PackagedRunnerPath()));
	const auto runner_manifest = Json::parse(uam_test::ReadFile(fs::path(runner.string() + ".manifest.json")));
	const auto runner_version = uam::strings::Trim(uam_test::ReadFile(fs::path(runner.string() + ".version")));
	UAM_ASSERT_EQ(runner_version, std::string(uam::constants::kAppVersion).substr(1));
	UAM_ASSERT_EQ(runner_manifest.at("protocolVersion"), Json(uam::remote::kRunnerProtocolVersion));
	const auto fingerprint = runner_manifest.at("sourceFingerprint").get<std::string>();
	UAM_ASSERT(fingerprint.size() == 64 && uam_test::ReadFile(runner).find(fingerprint) != std::string::npos);
	const auto ssh = bin / "ssh", shell = bin / "sh", claude = bin / "claude";
	UAM_ASSERT(uam::io::WriteTextFile(ssh,
	    "#!/bin/sh\ncase \" $* \" in *' private-claude-image-host '*) ;; *) exit 92;; esac\nprintf '%s\\n' \"$*\" >>\"$UAM_TEST_REMOTE_IMAGE_ROOT/ssh-invocations\"\nexec \"$UAM_TEST_RUNNER\" bridge-direct\n"));
	UAM_ASSERT(uam::io::WriteTextFile(shell,
	    "#!/bin/sh\nif [ \"$1\" = '-lc' ]; then shift; exec /bin/sh -c \"$@\"; fi\nexec /bin/sh \"$@\"\n"));
	UAM_ASSERT(uam::io::WriteTextFile(claude, R"REMOTECLAUDE(#!/usr/bin/python3
import json, os, pathlib, signal, sys, time
root = pathlib.Path(os.environ['UAM_TEST_REMOTE_IMAGE_ROOT'])
config = json.loads((root / 'peer-config.json').read_text())
def store(name, value):
    temporary = root / (name + '.tmp')
    with temporary.open('w') as out:
        json.dump(value, out)
        out.flush()
        os.fsync(out.fileno())
    os.replace(temporary, root / name)
def emit(value):
    print(json.dumps(value), flush=True)
if sys.argv[1:] == ['--version']:
    print('2.1.31 (Claude Code)', flush=True)
    sys.exit(0)
def terminate(signum, frame):
    store('peer-resume-exit.json' if '--resume' in sys.argv else 'peer-exit.json', dict(pid=os.getpid(), signal=signum))
    sys.exit(0)
signal.signal(signal.SIGTERM, terminate)
signal.signal(signal.SIGINT, terminate)
args = sys.argv[1:]
assert '-p' in args
resumed = '--resume' in args
if resumed:
    actual_prior = json.loads((root / 'native-established.json').read_text())
    assert args[args.index('--resume') + 1] == actual_prior['session_id']
else:
    assert not (root / 'native-established.json').exists()
assert args[args.index('--input-format') + 1] == 'stream-json'
assert args[args.index('--output-format') + 1] == 'stream-json'
assert args[args.index('--model') + 1] == config['model']
store('peer-resume-launch.json' if resumed else 'peer-launch.json', dict(pid=os.getpid(), argv=args, cwd=os.getcwd(), executable=sys.argv[0]))
users = 0
for line in sys.stdin:
    with (root / ('peer-resume-stdin.jsonl' if resumed else 'peer-stdin.jsonl')).open('a') as trace:
        trace.write(line)
        trace.flush()
        os.fsync(trace.fileno())
    message = json.loads(line)
    if message.get('type') == 'control_request':
        request = message['request']
        assert request['subtype'] == 'initialize', request
        assert isinstance(message['request_id'], str) and message['request_id']
        emit(dict(type='control_response', response=dict(subtype='success', request_id=message['request_id'],
            response=dict(commands=[], models=[dict(value=config['model'], displayName='Private image model')]))))
        init = dict(type='system', subtype='init', session_id=config['native'], model=config['model'],
            permissionMode=args[args.index('--permission-mode') + 1], cwd=os.getcwd(), slash_commands=['compact'] if resumed else [])
        store('peer-resume-native-init.json' if resumed else 'native-established.json', init)
        emit(init)
        continue
    assert message.get('type') == 'user', message
    users += 1
    assert users == 1
    store('peer-resume-input.json' if resumed else 'peer-input.json', dict(pid=os.getpid(), raw=line, envelope=message))
    while not (root / ('release-resume-result' if resumed else 'release-result')).exists():
        time.sleep(0.005)
    emit(dict(type='assistant', uuid='ffffffff-ffff-4fff-8fff-ffffffffffff' if resumed else 'dddddddd-dddd-4ddd-8ddd-dddddddddddd', session_id=config['native'],
        parent_tool_use_id=None, message=dict(role='assistant', model=config['model'],
            content=[dict(type='text', text=config['resume_answer'] if resumed else config['answer'])])))
    emit(dict(type='result', subtype='success', is_error=False, uuid='11111111-1111-4111-8111-111111111111' if resumed else 'eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee',
        session_id=config['native'], model=config['model'], result=config['resume_answer'] if resumed else config['answer'], parent_tool_use_id=None))
store('peer-resume-exit.json' if resumed else 'peer-exit.json', dict(pid=os.getpid(), eof=True))
)REMOTECLAUDE"));
	for (const auto& file : {ssh, shell, claude}) fs::permissions(file, fs::perms::owner_all);
	// All process-owning values below die before these private environment scopes.
	std::vector<std::unique_ptr<uam_test::ScopedEnvVar>> environment;
	const auto env = [&](const std::string& key, const std::string& value)
	{ environment.push_back(std::make_unique<uam_test::ScopedEnvVar>(key, value)); };
	env("PATH", bin.string() + ":/usr/bin:/bin"); env("HOME", profile.string()); env("USERPROFILE", profile.string());
	env("CLAUDE_CONFIG_DIR", (profile / "claude").string()); env("UAM_DATA_DIR", (temp.root / "controller").string());
	env("TMPDIR", (profile / "tmp").string()); env("UAM_TEST_RUNNER", runner.string()); env("UAM_TEST_REMOTE_IMAGE_ROOT", temp.root.string());
	const std::string model = "private-claude-image-model", native = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
	const std::string task = "Describe only this current remote PNG.", answer = "Actual private remote Claude image received.";
	const std::string resume_task = owned_retry ? "Retry the saved original after the confirmed stop." : "Continue the actual stopped native session with text only.";
	const std::string resume_answer = "Actual newly resumed private native response.";
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "peer-config.json", Json{{"model", model}, {"native", native}, {"answer", answer}, {"resume_answer", resume_answer}}.dump()));
	std::string image_bytes, old_bytes;
	UAM_ASSERT(uam::base64::Decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==", image_bytes));
	UAM_ASSERT(uam::base64::Decode("R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7", old_bytes));
	UAM_ASSERT(!image_bytes.empty() && old_bytes != image_bytes);
	UAM_ASSERT(uam::io::WriteBinaryFile(workspace / "current.png", image_bytes));
	UAM_ASSERT(uam::io::WriteBinaryFile(workspace / "old.gif", old_bytes));

	uam::AppState app;
	app.data_root = temp.root / "controller"; app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.settings.central_provider_configuration.enabled = app.settings.central_provider_configuration.uam_control_enabled = false;
	app.settings.git_enabled = app.settings.git_auto_worktree = false;
	ExecutionHost host; host.id = "private-remote-claude"; host.label = "Private image host"; host.transport = "ssh";
	host.ssh_alias = "private-claude-image-host"; host.platform = "macos"; host.runner_status = "ready";
	host.runner_version = runner_version; host.runner_protocol_version = uam::remote::kRunnerProtocolVersion;
	const ExecutionHost requested_host = host;
	app.settings.execution_hosts = {host}; uam::execution_hosts::Normalize(app.settings.execution_hosts);
	const auto* selected_host = uam::execution_hosts::Find(app.settings.execution_hosts, requested_host.id);
	UAM_ASSERT(selected_host && selected_host->id == requested_host.id && selected_host->id != uam::execution_hosts::kLocalHostId);
	UAM_ASSERT(selected_host->transport == "ssh" && selected_host->ssh_alias == "private-claude-image-host");
	UAM_ASSERT(uam::execution_hosts::SameConnection(*selected_host, requested_host));
	host = *selected_host;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kClaudeCli, false);
	chat.execution_host_id = host.id; chat.workspace_directory = workspace.string(); chat.model_id = model;
	chat.memory_enabled = chat.uam_control_enabled = false; chat.approval_mode = "default"; chat.command_safety_tier = "safe";
	MessageAttachment old; old.id = "old-image"; old.name = "Retain old GIF"; old.kind = "image"; old.mime_type = "image/gif";
	old.path = "old.gif"; // Ordinary workspace paths have no native-origin metadata.
	UAM_ASSERT(old.source == AttachmentSource::LegacyPath && old.origin_execution_host_id.empty() && uam::attachment_sources::IsPersistable(old, chat.id));
	chat.messages = {{MessageRole::User, "Retain this prior remote context."}, {MessageRole::Assistant, "Retain the earlier answer."}};
	chat.messages.front().attachments = {old};
	UAM_ASSERT(chat.native_session_id.empty() && !chat.remote_process_exists && chat.remote_process_control_token.empty());
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	const auto original = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
	UAM_ASSERT(original && original->messages == chat.messages);
	app.chats.push_back(*original);
	ChatSession unrelated = ChatDomainService().CreateNewChat("", uam::provider_ids::kClaudeCli, false);
	unrelated.workspace_directory = (temp.root / "unrelated-workspace").string(); fs::create_directories(fs::path(unrelated.workspace_directory));
	unrelated.messages = {{MessageRole::User, "Retain unrelated saved history."}};
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, unrelated)); app.chats.push_back(unrelated);
	const auto unrelated_path = AppPaths::UamChatFilePath(app.data_root, unrelated.id);
	const auto unrelated_primary = uam_test::ReadFile(unrelated_path);
	ChatDomainService().SelectChatById(app, chat.id);
	app.composer_text = "Retain the unrelated live draft.";
	const auto prefix = original->messages;
	RemoteClaudeOrdinaryImageCleanup cleanup{app, chat.id, temp.root / "release-result"};
	MessageAttachment current; current.id = "current-image"; current.name = "Current remote PNG"; current.kind = "image";
	current.mime_type = "image/png"; current.path = "current.png";
	UAM_ASSERT(current.source == AttachmentSource::LegacyPath && current.origin_execution_host_id.empty() && uam::attachment_sources::IsPersistable(current, chat.id));
	std::string error;
	UAM_ASSERT(uam::SendAcpPrompt(app, chat.id, task, {}, {current}, false, &error) && error.empty());
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	bool receipt = false;
	bool saw_pending_request = false, saw_empty_pending_method = false, saw_nonpositive_pending_id = false;
	bool saw_image_worker = false, saw_image_worker_finished = false, saw_image_worker_error = false, saw_prepared_message = false;
	const auto observe_admission = [&]()
	{
		if (const auto* owner = ChatDomainService().FindChatById(app, chat.id))
			for (const auto& pending : owner->remote_pending_requests)
			{
				saw_pending_request = true;
				saw_empty_pending_method = saw_empty_pending_method || pending.method.empty();
				saw_nonpositive_pending_id = saw_nonpositive_pending_id || pending.request_id <= 0;
			}
		if (const auto* runtime = uam::FindAcpSessionForChat(app, chat.id); runtime && runtime->claude_input_preparation)
		{
			const auto& state = runtime->claude_input_preparation->state;
			saw_image_worker = true;
			if (state && state->finished.load(std::memory_order_acquire))
			{
				saw_image_worker_finished = true;
				saw_image_worker_error = saw_image_worker_error || !state->error.empty();
				saw_prepared_message = saw_prepared_message || (!state->message.is_null() && !state->message.empty());
			}
		}
	};
	while (std::chrono::steady_clock::now() < deadline)
	{
		observe_admission();
		cleanup.Tick();
		observe_admission();
		const auto* session = uam::FindAcpSessionForChat(app, chat.id);
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
		if (fs::is_regular_file(temp.root / "peer-input.json") && session && session->session_ready && session->processing &&
		    session->session_id == native && saved && saved->native_session_id == native && saved->remote_turn_reconnect_pending &&
		    saved->remote_prompt_delivery_id.empty() && saved->remote_prompt_delivery_payload.empty() && !session->pending_prompt_delivery &&
		    app.pending_chat_save_at_by_chat_id.empty() && app.pending_chat_save_tasks.empty()) { receipt = true; break; }
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	if (!receipt)
	{
		const auto* runtime = uam::FindAcpSessionForChat(app, chat.id);
		const auto* owner = ChatDomainService().FindChatById(app, chat.id);
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
		// Fixed labels/booleans/counts only: never log tokens, native IDs, payloads,
		// peer envelopes, image bytes, paths, prompt prose or unknown error text.
		const auto error_class = [](const std::string& value)
		{
			if (value.empty()) return "empty";
			if (value == "Remote prompt was not sent because its owned durable receipt was not confirmed.") return "owned_durable_receipt_not_confirmed";
			if (value == "Prompt was not sent because chat history could not be saved.") return "chat_history_save_failed";
			return "other_nonempty";
		};
		std::cerr << "PRIVATE_REMOTE_CLAUDE_RECEIPT_DIAGNOSTIC " << Json{
		    {"session_present", runtime != nullptr}, {"session_running", runtime && runtime->running},
		    {"session_initialized", runtime && runtime->initialized}, {"session_ready", runtime && runtime->session_ready},
		    {"session_processing", runtime && runtime->processing}, {"session_native_id_present", runtime && !runtime->session_id.empty()},
		    {"session_last_error", runtime ? error_class(runtime->last_error) : "missing_session"},
		    {"pending_prompt_delivery", runtime && static_cast<bool>(runtime->pending_prompt_delivery)},
		    {"pending_agent_input", runtime && static_cast<bool>(runtime->pending_agent_input)},
		    {"pending_local_launch", runtime && static_cast<bool>(runtime->pending_local_launch)},
		    {"owner_present", owner != nullptr}, {"owner_rows", owner ? owner->messages.size() : 0},
		    {"owner_queue_held", owner && owner->acp_queue_held}, {"owner_pending_requests", owner ? owner->remote_pending_requests.size() : 0},
		    {"owner_delivery_id_present", owner && !owner->remote_prompt_delivery_id.empty()},
		    {"owner_delivery_payload_present", owner && !owner->remote_prompt_delivery_payload.empty()},
		    {"saved_present", static_cast<bool>(saved)}, {"saved_rows", saved ? saved->messages.size() : 0},
		    {"saved_native_id_present", saved && !saved->native_session_id.empty()},
		    {"saved_remote_reconnect_pending", saved && saved->remote_turn_reconnect_pending},
		    {"saved_delivery_id_present", saved && !saved->remote_prompt_delivery_id.empty()},
		    {"saved_delivery_payload_present", saved && !saved->remote_prompt_delivery_payload.empty()},
		    {"peer_input_exists", fs::is_regular_file(temp.root / "peer-input.json")},
		    {"saw_pending_request", saw_pending_request}, {"saw_empty_pending_method", saw_empty_pending_method},
		    {"saw_nonpositive_pending_id", saw_nonpositive_pending_id},
		    {"saw_image_worker", saw_image_worker}, {"saw_image_worker_finished", saw_image_worker_finished},
		    {"saw_image_worker_error", saw_image_worker_error}, {"saw_prepared_message", saw_prepared_message},
		    {"claude_input_tasks", app.claude_input_tasks.size()}, {"remote_context_tasks", app.remote_context_tasks.size()},
		    {"pending_save_tasks", app.pending_chat_save_tasks.size()}, {"scheduled_saves", app.pending_chat_save_at_by_chat_id.size()},
		    {"pending_remote_stops", app.pending_acp_remote_stops.size()}}.dump() << std::endl;
	}
	UAM_ASSERT(receipt); // Result is still withheld: clearing the real pre-write intent requires actual protocol3 input ACK.
	auto* session = uam::FindAcpSessionForChat(app, chat.id);
	UAM_ASSERT(session && session->running && session->initialized && session->session_ready && session->processing && session->last_error.empty());
	UAM_ASSERT_EQ(session->protocol_kind, std::string("claude-code-stream-json"));
	UAM_ASSERT_EQ(session->process_execution_host_id, host.id); UAM_ASSERT_EQ(session->remote_runner_protocol_version, 3);
	UAM_ASSERT_EQ(session->current_model_id, model); UAM_ASSERT_EQ(session->session_id, native);
	// Claude's ordinary launch does not promise a compatibility cache entry.
	// Request a real exact-host probe and consume it within the existing Send deadline.
	std::string version_error;
	UAM_ASSERT(ProviderCliCompatibilityService().StartProviderVersionCheck(app,
	    uam::provider_ids::kClaudeCli, true, host.id, &version_error) && version_error.empty());
	const auto version_key = CliProviderVersionStateKey(uam::provider_ids::kClaudeCli, host.id);
	bool version_checked = false;
	while (std::chrono::steady_clock::now() < deadline)
	{
		cleanup.Tick();
		const auto found = app.runtime_cli_versions_by_provider_id.find(version_key);
		if (found != app.runtime_cli_versions_by_provider_id.end() && found->second.checked &&
		    !app.runtime_cli_version_check_task.running && !app.runtime_cli_version_check_task.worker)
		{
			version_checked = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT(version_checked);
	const auto version_found = app.runtime_cli_versions_by_provider_id.find(version_key);
	UAM_ASSERT(version_found != app.runtime_cli_versions_by_provider_id.end() && version_found->second.checked);
	const auto version = app.runtime_cli_versions_by_provider_id.at(CliProviderVersionStateKey(uam::provider_ids::kClaudeCli, host.id));
	UAM_ASSERT(version.checked && version.supported && version.check_error.empty() && uam::execution_hosts::SameConnection(version.execution_host, host));
	UAM_ASSERT_EQ(version.installed_version, std::string("2.1.31"));
	const auto launch = Json::parse(uam_test::ReadFile(temp.root / "peer-launch.json"));
	UAM_ASSERT_EQ(launch.at("cwd"), Json(workspace.string())); UAM_ASSERT_EQ(launch.at("executable"), Json(claude.string()));
	const auto input = Json::parse(uam_test::ReadFile(temp.root / "peer-input.json"));
	UAM_ASSERT_EQ(input.at("pid"), launch.at("pid"));
	const auto envelope = input.at("envelope");
	UAM_ASSERT_EQ(envelope.at("type"), Json("user")); UAM_ASSERT(!envelope.contains("session_id"));
	UAM_ASSERT(input.at("raw").get<std::string>().size() <= 2 * 1024 * 1024);
	std::string prompt_text; std::vector<Json> images;
	for (const auto& part : envelope.at("message").at("content"))
	{
		if (part.value("type", "") == "text") prompt_text += part.value("text", "");
		if (part.value("type", "") == "image") images.push_back(part);
	}
	UAM_ASSERT(prompt_text.find(task) != std::string::npos); UAM_ASSERT_EQ(images.size(), std::size_t{1});
	UAM_ASSERT_EQ(images.front().at("source").at("type"), Json("base64"));
	UAM_ASSERT_EQ(images.front().at("source").at("media_type"), Json("image/png"));
	std::string decoded; UAM_ASSERT(uam::base64::Decode(images.front()["source"]["data"].get<std::string>(), decoded));
	UAM_ASSERT_EQ(decoded, image_bytes); UAM_ASSERT(decoded != old_bytes);
	const auto delivered = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
	UAM_ASSERT(delivered && delivered->messages.size() >= prefix.size() + 1);
	UAM_ASSERT_EQ(session->turn_user_message_index, static_cast<int>(prefix.size()));
	const auto& delivered_user = delivered->messages[static_cast<std::size_t>(session->turn_user_message_index)];
	UAM_ASSERT(delivered_user.role == MessageRole::User && !delivered_user.acp_prompt_not_sent);
	UAM_ASSERT_EQ(delivered->messages[0], prefix[0]); UAM_ASSERT_EQ(delivered->messages[1], prefix[1]);
	UAM_ASSERT_EQ(delivered_user.content, task);
	UAM_ASSERT_EQ(delivered_user.attachments.size(), std::size_t{1});
	UAM_ASSERT_EQ(delivered_user.attachments.front().name, current.name); // Ordinary native wire has no filename label.
	UAM_ASSERT(delivered_user.attachments.front().source == AttachmentSource::LegacyPath && delivered_user.attachments.front().origin_execution_host_id.empty());
	UAM_ASSERT_EQ(delivered_user.attachments.front().path, current.path);
	UAM_ASSERT_EQ(delivered->execution_host_id, host.id); // Ordinary path resolution is scoped by the actual saved owner/session host.
	UAM_ASSERT(delivered->remote_process_exists && !delivered->remote_process_control_token.empty());
	UAM_ASSERT_EQ(session->remote_output_delivery_token, delivered->remote_process_control_token);
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "release-result", "Finish the actual peer turn"));
	bool finished = false;
	while (std::chrono::steady_clock::now() < deadline)
	{
		cleanup.Tick();
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
		if (!session->processing && session->session_ready && saved && saved->messages.size() == prefix.size() + 2 &&
		    saved->messages.back().content == answer && app.pending_chat_save_at_by_chat_id.empty() && app.pending_chat_save_tasks.empty()) { finished = true; break; }
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT(finished && session->last_error.empty());

	// Initial ordinary Send is real execution authority; optional original setup
	// below is public typed DATA publication, not a native export/permission.
	auto* owner = ChatDomainService().FindChatById(app, chat.id);
	UAM_ASSERT(owner && session->running && session->session_ready && !session->processing);
	UAM_ASSERT(!session->saved_image_retry && !session->claude_manual_compaction_supported);
	const auto established = Json::parse(uam_test::ReadFile(temp.root / "native-established.json"));
	UAM_ASSERT_EQ(established.at("session_id"), Json(owner->native_session_id));
	const std::string expected_native = owner->native_session_id;
	UAM_ASSERT(!expected_native.empty() && session->session_id == expected_native);
	int retry_index = -1;
	MessageAttachment retained;
	if (owned_retry)
	{
		const auto revision = ChatRepository::ChatStorageRevision(app.data_root, chat.id);
		UAM_ASSERT(revision && *revision != "missing");
		ChatSession candidate = *owner;
		Message row; row.role = MessageRole::User; row.content = resume_task;
		row.created_at = uam::time::TimestampNow(); row.acp_prompt_not_sent = true;
		MessageAttachment pending; pending.id = "cold-retained-original"; pending.name = "Retained cold GIF"; pending.kind = "image";
		pending.source = AttachmentSource::NativeInlinePending;
		pending.native_part_key = "v1:data-fixture/cold-user/image/0";
		pending.native_source_field = "data-fixture/user/attachments/0";
		pending.origin_execution_host_id = host.id;
		std::string media_error;
		pending.native_inline_payload = uam::native_assets::MakeInlinePayload(uam::base64::Encode(old_bytes), "image/gif",
		    std::make_shared<uam::native_assets::PayloadBudget>(), media_error);
		UAM_ASSERT(pending.native_inline_payload && media_error.empty());
		pending.mime_type = pending.native_inline_payload->MimeType();
		pending.size_bytes = pending.native_inline_payload->DecodedSize(); pending.content_digest = pending.native_inline_payload->Digest();
		row.attachments = {pending}; candidate.messages.push_back(std::move(row));
		const auto incarnation = owner->metadata_incarnation;
		auto worker = std::async(std::launch::async, [root = app.data_root, candidate = std::move(candidate), revision = *revision]() mutable
		{
			std::string failure;
			auto prepared = uam::PreparedNativeMediaChatSave::Prepare(root, std::move(candidate), revision, failure);
			if (!prepared || !prepared->Publish(failure) || !prepared->AcknowledgeOriginals(failure))
				return std::make_pair(std::optional<ChatSession>{}, failure);
			return std::make_pair(prepared->TakeCommittedChat(), failure);
		});
		auto result = worker.get(); UAM_ASSERT(result.first && result.second.empty());
		UAM_ASSERT(result.first->metadata_incarnation == incarnation && result.first->native_session_id == expected_native);
		*owner = std::move(*result.first);
		const auto stored = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
		UAM_ASSERT(stored && stored->messages == owner->messages && stored->native_session_id == expected_native);
		retry_index = static_cast<int>(stored->messages.size()) - 1;
		UAM_ASSERT(stored->messages.back().role == MessageRole::User && stored->messages.back().acp_prompt_not_sent);
		UAM_ASSERT_EQ(stored->messages.back().content, resume_task);
		UAM_ASSERT_EQ(stored->messages.back().attachments.size(), std::size_t{1});
		retained = stored->messages.back().attachments.front();
		UAM_ASSERT(retained.source == AttachmentSource::OwnedNativeAsset && retained.path.empty() && !retained.native_inline_payload);
		UAM_ASSERT_EQ(retained.origin_execution_host_id, host.id);
		UAM_ASSERT(uam::native_assets::IsKeyForOwner(chat.id, retained.native_asset_key));
		UAM_ASSERT(!uam::native_assets::IsKeyForOwner(expected_native, retained.native_asset_key));
		const auto bytes = uam::native_assets::ReadOwned(app.data_root, chat.id, retained.native_asset_key);
		UAM_ASSERT(bytes.ok && bytes.bytes == old_bytes);
	}
	const auto before_stop_messages = owner->messages;
	cleanup.StopAndDrain();
	UAM_ASSERT(cleanup.Quiet() && cleanup.proxy_pid > 0);
	UAM_ASSERT(!session->running && session->child_pid == -1 && !session->remote_stop_pending && !session->remote_stop_unconfirmed);
	int first_status = 0; errno = 0;
	UAM_ASSERT(waitpid(cleanup.proxy_pid, &first_status, WNOHANG) == -1 && errno == ECHILD);
	const auto stopped = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
	UAM_ASSERT(stopped && stopped->messages == before_stop_messages && stopped->messages == owner->messages);
	UAM_ASSERT_EQ(stopped->native_session_id, expected_native);
	UAM_ASSERT(!stopped->remote_process_exists && !stopped->remote_turn_reconnect_pending && !stopped->remote_stop_cleanup_pending);
	UAM_ASSERT(!session->saved_image_retry && !session->pending_prompt_delivery && app.pending_chat_save_tasks.empty());
	UAM_ASSERT(!fs::exists(temp.root / "peer-resume-launch.json") && !fs::exists(temp.root / "peer-resume-input.json"));
	const auto before_resume_messages = stopped->messages;
	const auto stopped_primary = uam_test::ReadFile(AppPaths::UamChatFilePath(app.data_root, chat.id));
	const auto first_input = uam_test::ReadFile(temp.root / "peer-input.json");
	const auto first_stdin = uam_test::ReadFile(temp.root / "peer-stdin.jsonl");
	if (owned_retry)
	{
		UAM_ASSERT_EQ(uam::RecoverableUnsentAcpMessageIndices(app, *owner), std::vector<int>{retry_index});
		const auto bytes = uam::native_assets::ReadOwned(app.data_root, chat.id, retained.native_asset_key);
		UAM_ASSERT(bytes.ok && bytes.bytes == old_bytes);
	}
	// A separate actual cleanup owns any new process; the first one is already
	// confirmed reaped, and neither guard clears/reconstructs runtime authority.
	RemoteClaudeOrdinaryImageCleanup resumed_cleanup{app, chat.id, temp.root / "release-resume-result"};
	std::string resume_error;
	const bool resumed = owned_retry
	    ? uam::RetryFailedAcpMessage(app, chat.id, retry_index, &resume_error)
	    : uam::SendAcpPrompt(app, chat.id, resume_task, &resume_error);
	if (!resumed && owned_retry)
	{
		// Honest intended baseline: classify only after independent cold TEXT
		// control passes and every stopped-owner/original/index prerequisite above.
		UAM_ASSERT_EQ(resume_error, std::string("Saved image Retry needs this chat's existing ready SSH session. Reopen it or attach the files again."));
		UAM_ASSERT_EQ(uam_test::ReadFile(AppPaths::UamChatFilePath(app.data_root, chat.id)), stopped_primary);
		UAM_ASSERT_EQ(owner->messages, before_resume_messages);
		UAM_ASSERT(!session->saved_image_retry && !session->pending_prompt_delivery && !session->running);
		UAM_ASSERT(!fs::exists(temp.root / "peer-resume-launch.json") && !fs::exists(temp.root / "peer-resume-input.json"));
		UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "peer-input.json"), first_input);
		UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "peer-stdin.jsonl"), first_stdin);
	}
	UAM_ASSERT(resumed && resume_error.empty());
	const auto resume_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	bool fresh_receipt = false;
	while (std::chrono::steady_clock::now() < resume_deadline)
	{
		resumed_cleanup.Tick();
		const auto stored = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
		if (fs::is_regular_file(temp.root / "peer-resume-input.json") && session->running && session->initialized && session->session_ready &&
		    session->processing && session->claude_manual_compaction_supported && stored && stored->native_session_id == expected_native &&
		    stored->remote_process_exists && stored->remote_turn_reconnect_pending && !stored->remote_process_control_token.empty() &&
		    stored->remote_prompt_delivery_id.empty() && stored->remote_prompt_delivery_payload.empty() && !session->pending_prompt_delivery &&
		    app.pending_chat_save_tasks.empty() && app.pending_chat_save_at_by_chat_id.empty())
		{ fresh_receipt = true; break; }
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT(fresh_receipt && session->last_error.empty());
	const auto resumed_launch = Json::parse(uam_test::ReadFile(temp.root / "peer-resume-launch.json"));
	const auto resumed_args = resumed_launch.at("argv").get<std::vector<std::string>>();
	const auto resume_arg = std::ranges::find(resumed_args, "--resume");
	UAM_ASSERT(resume_arg != resumed_args.end() && std::next(resume_arg) != resumed_args.end());
	UAM_ASSERT_EQ(*std::next(resume_arg), expected_native);
	UAM_ASSERT(resumed_launch.at("pid") != launch.at("pid"));
	UAM_ASSERT_EQ(resumed_launch.at("cwd"), Json(workspace.string()));
	const auto init = Json::parse(uam_test::ReadFile(temp.root / "peer-resume-native-init.json"));
	UAM_ASSERT_EQ(init.at("session_id"), Json(expected_native));
	UAM_ASSERT_EQ(init.at("slash_commands"), Json::array({"compact"}));
	// This native flag was false on the first process and becomes true only
	// through the genuine decoded system/init frame, not saved-ID copying.
	UAM_ASSERT(session->claude_manual_compaction_supported);
	UAM_ASSERT_EQ(session->session_id, expected_native);
	UAM_ASSERT_EQ(session->process_execution_host_id, host.id);
	UAM_ASSERT_EQ(session->remote_runner_protocol_version, uam::remote::kRunnerProtocolVersion);
	const auto current_saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
	UAM_ASSERT(current_saved && current_saved->remote_process_control_token == session->remote_output_delivery_token);
	const auto resumed_input = Json::parse(uam_test::ReadFile(temp.root / "peer-resume-input.json"));
	UAM_ASSERT_EQ(resumed_input.at("pid"), resumed_launch.at("pid"));
	const auto& resumed_envelope = resumed_input.at("envelope");
	UAM_ASSERT_EQ(resumed_envelope.at("type"), Json("user"));
	UAM_ASSERT(resumed_input.at("raw").get<std::string>().size() <= 2 * 1024 * 1024);
	std::string resumed_text; std::vector<Json> resumed_images;
	for (const auto& part : resumed_envelope.at("message").at("content"))
	{
		if (part.value("type", "") == "text") resumed_text += part.value("text", "");
		if (part.value("type", "") == "image") resumed_images.push_back(part);
	}
	UAM_ASSERT(resumed_text.find(resume_task) != std::string::npos);
	UAM_ASSERT_EQ(resumed_images.size(), owned_retry ? std::size_t{1} : std::size_t{0});
	if (owned_retry)
	{
		UAM_ASSERT_EQ(resumed_images.front().at("source").at("media_type"), Json("image/gif"));
		std::string bytes; UAM_ASSERT(uam::base64::Decode(resumed_images.front().at("source").at("data").get<std::string>(), bytes));
		UAM_ASSERT_EQ(bytes, old_bytes); UAM_ASSERT(bytes != image_bytes);
	}
	const auto current_user = owned_retry ? retry_index : static_cast<int>(before_resume_messages.size());
	UAM_ASSERT_EQ(session->turn_user_message_index, current_user);
	UAM_ASSERT(current_saved->messages.size() > static_cast<std::size_t>(current_user));
	UAM_ASSERT_EQ(current_saved->messages[static_cast<std::size_t>(current_user)].content, resume_task);
	UAM_ASSERT(!current_saved->messages[static_cast<std::size_t>(current_user)].acp_prompt_not_sent);
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "release-resume-result", "Finish actual resumed turn"));
	const auto expected_count = before_resume_messages.size() + (owned_retry ? 1 : 2);
	bool completed = false;
	while (std::chrono::steady_clock::now() < resume_deadline)
	{
		resumed_cleanup.Tick();
		const auto stored = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
		if (!session->processing && stored && stored->messages.size() == expected_count && stored->messages.back().content == resume_answer &&
		    app.pending_chat_save_tasks.empty() && app.pending_chat_save_at_by_chat_id.empty()) { completed = true; break; }
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT(completed && session->last_error.empty());
	resumed_cleanup.StopAndDrain();
	UAM_ASSERT(resumed_cleanup.Quiet() && resumed_cleanup.proxy_pid > 0);
	UAM_ASSERT(resumed_cleanup.proxy_pid != cleanup.proxy_pid && !session->running && session->child_pid == -1);
	errno = 0; int final_status = 0;
	UAM_ASSERT(waitpid(resumed_cleanup.proxy_pid, &final_status, WNOHANG) == -1 && errno == ECHILD);
	const auto final = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
	UAM_ASSERT(final && final->messages == owner->messages && final->native_session_id == expected_native && final->messages.size() == expected_count);
	UAM_ASSERT(!final->remote_process_exists && !final->remote_stop_cleanup_pending);
	UAM_ASSERT_EQ(final->messages.back().content, resume_answer);
	const auto prefix_count = owned_retry ? before_resume_messages.size() - 1 : before_resume_messages.size();
	UAM_ASSERT(std::equal(before_resume_messages.begin(), before_resume_messages.begin() + static_cast<std::ptrdiff_t>(prefix_count), final->messages.begin()));
	if (owned_retry)
	{
		UAM_ASSERT_EQ(final->messages[static_cast<std::size_t>(retry_index)].attachments, std::vector<MessageAttachment>{retained});
		const auto bytes = uam::native_assets::ReadOwned(app.data_root, chat.id, retained.native_asset_key);
		UAM_ASSERT(bytes.ok && bytes.bytes == old_bytes);
	}
	UAM_ASSERT_EQ(uam_test::ReadFile(workspace / "current.png"), image_bytes);
	UAM_ASSERT_EQ(uam_test::ReadFile(workspace / "old.gif"), old_bytes);
	UAM_ASSERT_EQ(uam_test::ReadFile(unrelated_path), unrelated_primary);
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), chat.id);
	UAM_ASSERT_EQ(app.composer_text, std::string("Retain the unrelated live draft."));
}

UAM_TEST(ClaudeRemoteStoppedTextResumeBindsExactSavedSessionThroughActualRunner)
{
	CheckClaudeStoppedRemoteContinuation(false);
}

UAM_TEST(ClaudeRemoteStoppedOwnedRetryResumesExactSavedSessionThroughActualRunner)
{
	CheckClaudeStoppedRemoteContinuation(true);
}
#endif


#if UAM_ENABLE_RUNTIME_GEMINI_CLI && UAM_ENABLE_SSH && UAM_ENABLE_RUNTIME_CLAUDE_CLI && defined(__APPLE__)
enum class RemoteGeminiOwnedRetryControl { Healthy, MixedLegacy, MissingOriginal, WorkerMode, WorkerModel, PendingNative, WrongHost, NegotiatedFalse };

static void CheckGeminiRemoteSavedOwnedIndexedRetry(RemoteGeminiOwnedRetryControl control)
{
	using Json = nlohmann::json;
	uam_test::TempDir temp("uam-remote-gemini-owned-retry");
	const auto bin = temp.root / "bin", profile = temp.root / "private-home";
	const auto workspace = fs::weakly_canonical(temp.root) / "remote-workspace";
	fs::create_directories(bin); fs::create_directories(workspace); fs::create_directories(profile / "tmp");
	const auto runner = PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath().parent_path() / "uam-runner";
	UAM_ASSERT(fs::is_regular_file(runner) && fs::equivalent(runner, uam::remote::PackagedRunnerPath()));
	const auto runner_manifest = Json::parse(uam_test::ReadFile(fs::path(runner.string() + ".manifest.json")));
	const auto runner_version = uam::strings::Trim(uam_test::ReadFile(fs::path(runner.string() + ".version")));
	UAM_ASSERT_EQ(runner_version, std::string(uam::constants::kAppVersion).substr(1));
	UAM_ASSERT_EQ(runner_manifest.at("protocolVersion"), Json(uam::remote::kRunnerProtocolVersion));
	const auto fingerprint = runner_manifest.at("sourceFingerprint").get<std::string>();
	UAM_ASSERT(fingerprint.size() == 64 && uam_test::ReadFile(runner).find(fingerprint) != std::string::npos);
	const auto ssh = bin / "ssh", shell = bin / "sh", gemini = bin / "gemini";
	UAM_ASSERT(uam::io::WriteTextFile(ssh,
	    "#!/bin/sh\ncase \" $* \" in *' private-gemini-image-host '*) ;; *) exit 92;; esac\nprintf '%s\\n' \"$*\" >>\"$UAM_TEST_REMOTE_GEMINI_ROOT/ssh-invocations\"\nexec \"$UAM_TEST_RUNNER\" bridge-direct\n"));
	UAM_ASSERT(uam::io::WriteTextFile(shell,
	    "#!/bin/sh\nif [ \"$1\" = '-lc' ]; then shift; exec /bin/sh -c \"$@\"; fi\nexec /bin/sh \"$@\"\n"));
	UAM_ASSERT(uam::io::WriteTextFile(gemini, R"REMOTEGEMINI(#!/usr/bin/python3
import json, os, pathlib, signal, sys, time, uuid
root = pathlib.Path(os.environ['UAM_TEST_REMOTE_GEMINI_ROOT'])
config = json.loads((root / 'peer-config.json').read_text())
def store(name, value):
    temporary = root / (name + '.tmp')
    with temporary.open('w') as out:
        json.dump(value, out)
        out.flush()
        os.fsync(out.fileno())
    os.replace(temporary, root / name)
def emit(value):
    print(json.dumps(value), flush=True)
def reply(identifier, result, filename=None):
    value = dict(jsonrpc='2.0', id=identifier, result=result)
    if filename:
        store(filename, value)
    emit(value)
if sys.argv[1:] == ['--version']:
    print('0.55.1', flush=True)
    sys.exit(0)
def terminate(signum, frame):
    store('peer-exit.json', dict(pid=os.getpid(), signal=signum))
    sys.exit(0)
signal.signal(signal.SIGTERM, terminate)
signal.signal(signal.SIGINT, terminate)
args = sys.argv[1:]
assert '--acp' in args
assert args[args.index('--model') + 1] == config['model']
store('peer-launch.json', dict(pid=os.getpid(), argv=args, cwd=os.getcwd(), executable=sys.argv[0]))
native = str(uuid.uuid4())
initialized, created, prompts = False, False, 0
for line in sys.stdin:
    assert len(line.encode('utf-8')) <= 2 * 1024 * 1024
    # The journal is durable BEFORE JSON parse, including unwanted text-only input.
    with (root / 'peer-stdin.jsonl').open('a') as trace:
        trace.write(line)
        trace.flush()
        os.fsync(trace.fileno())
    request = json.loads(line)
    assert request.get('jsonrpc') == '2.0'
    method, params = request.get('method'), request.get('params', {})
    if method == 'initialize':
        assert not initialized
        initialized = True
        reply(request['id'], dict(protocolVersion=1,
            agentInfo=dict(name='gemini', title='Private remote Gemini ACP', version='0.55.1'),
            agentCapabilities=dict(loadSession=True, promptCapabilities=dict(image=config['image']))), 'peer-initialize-response.json')
    elif method == 'initialized':
        assert initialized
    elif method == 'session/new':
        assert initialized and not created and params['cwd'] == os.getcwd()
        created = True
        reply(request['id'], dict(sessionId=native,
            models=dict(currentModelId=config['model'], availableModels=[dict(modelId=config['model'], name='Private model')]),
            modes=dict(currentModeId='default', availableModes=[dict(id='default', name='Default'), dict(id='plan', name='Plan')])), 'peer-session-response.json')
    elif method in ['session/set_mode', 'session/set_model', 'session/set_config_option']:
        assert created and params.get('sessionId') == native
        if method == 'session/set_mode': assert params.get('modeId') == 'default'
        if method == 'session/set_model': assert params.get('modelId') == config['model']
        reply(request['id'], {})
    elif method == 'session/cancel':
        assert created and params.get('sessionId') == native
    elif method == 'session/prompt':
        assert created and params.get('sessionId') == native
        prompts += 1
        assert prompts <= 2
        store('peer-input-' + str(prompts) + '.json', dict(pid=os.getpid(), raw=line, request=request))
        while not (root / ('release-result-' + str(prompts))).exists() and not (root / 'release-all-results').exists():
            time.sleep(0.005)
        answer = config['answer'] if prompts == 1 else config['retry_answer']
        emit(dict(jsonrpc='2.0', method='session/update', params=dict(sessionId=native,
            update=dict(sessionUpdate='agent_message_chunk', content=dict(type='text', text=answer)))))
        reply(request['id'], dict(stopReason='end_turn'))
    else:
        raise RuntimeError('Unexpected private ACP request: ' + str(method))
store('peer-exit.json', dict(pid=os.getpid(), eof=True))
)REMOTEGEMINI"));
	for (const auto& file : {ssh, shell, gemini}) fs::permissions(file, fs::perms::owner_all);
	// Reuse the existing provider-neutral Stop/drain guard despite its historical name.
	// All process-owning values die before private environment/root scopes unwind.
	std::vector<std::unique_ptr<uam_test::ScopedEnvVar>> environment;
	const auto env = [&](const std::string& key, const std::string& value)
	{ environment.push_back(std::make_unique<uam_test::ScopedEnvVar>(key, value)); };
	env("PATH", bin.string() + ":/usr/bin:/bin"); env("HOME", profile.string()); env("USERPROFILE", profile.string());
	env("GEMINI_CLI_HOME", profile.string());
	env("GEMINI_CLI_SYSTEM_SETTINGS_PATH", (profile / "system-settings.json").string());
	env("GEMINI_CLI_SYSTEM_DEFAULTS_PATH", (profile / "system-defaults.json").string());
	env("UAM_DATA_DIR", (temp.root / "controller").string()); env("TMPDIR", (profile / "tmp").string());
	env("UAM_TEST_RUNNER", runner.string()); env("UAM_TEST_REMOTE_GEMINI_ROOT", temp.root.string());
	const std::string model = "private-gemini-image-model";
	const std::string task = "Describe only this current remote PNG.", answer = "Actual private remote Gemini PNG received.";
	const std::string retry_task = "Retry only the retained remote original.", retry_answer = "Actual private remote Gemini owned GIF received.";
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "peer-config.json", Json{{"model", model}, {"answer", answer}, {"retry_answer", retry_answer}}.dump()));
	const auto peer_configuration = Json{{"model", model}, {"answer", answer}, {"retry_answer", retry_answer}, {"image", control != RemoteGeminiOwnedRetryControl::NegotiatedFalse}};
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "peer-config.json", peer_configuration.dump()));
	std::string png, gif;
	UAM_ASSERT(uam::base64::Decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==", png));
	UAM_ASSERT(uam::base64::Decode("R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7", gif));
	UAM_ASSERT(!png.empty() && png != gif);
	UAM_ASSERT(uam::io::WriteBinaryFile(workspace / "current.png", png));
	UAM_ASSERT(uam::io::WriteBinaryFile(workspace / "old.gif", gif));
	uam::AppState app;
	app.data_root = temp.root / "controller"; app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.settings.central_provider_configuration.enabled = app.settings.central_provider_configuration.uam_control_enabled = false;
	app.settings.mcp_servers.clear(); app.settings.host_scoped_settings.clear();
	app.settings.git_enabled = app.settings.git_auto_worktree = app.settings.svn_enabled = false;
	app.settings.computer_use_globally_enabled = false;
	ExecutionHost requested; requested.id = "private-remote-gemini"; requested.label = "Private Gemini host"; requested.transport = "ssh";
	requested.ssh_alias = "private-gemini-image-host"; requested.platform = "macos"; requested.runner_status = "ready";
	requested.runner_version = runner_version; requested.runner_protocol_version = uam::remote::kRunnerProtocolVersion;
	app.settings.execution_hosts = {requested}; uam::execution_hosts::Normalize(app.settings.execution_hosts);
	const auto* selected = uam::execution_hosts::Find(app.settings.execution_hosts, requested.id);
	UAM_ASSERT(selected && selected->id != uam::execution_hosts::kLocalHostId && selected->transport == "ssh");
	UAM_ASSERT(uam::execution_hosts::SameConnection(*selected, requested));
	const ExecutionHost host = *selected;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kGeminiCli, false);
	chat.execution_host_id = host.id; chat.workspace_directory = workspace.string(); chat.model_id = model;
	chat.memory_enabled = chat.uam_control_enabled = chat.computer_use_enabled = false;
	chat.reasoning_effort.clear(); chat.service_tier.clear(); chat.service_tier_explicit = false;
	chat.approval_mode = "default"; chat.command_safety_tier = "off";
	MessageAttachment old; old.id = "old-image"; old.name = "Earlier GIF"; old.kind = "image"; old.mime_type = "image/gif"; old.path = "old.gif";
	UAM_ASSERT(old.source == AttachmentSource::LegacyPath && old.origin_execution_host_id.empty() && uam::attachment_sources::IsPersistable(old, chat.id));
	chat.messages = {{MessageRole::User, "Preserve earlier remote context."}, {MessageRole::Assistant, "Preserve earlier answer."}};
	chat.messages.front().attachments = {old};
	UAM_ASSERT(chat.native_session_id.empty() && !chat.remote_process_exists && chat.remote_process_control_token.empty());
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, chat));
	const auto original = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
	UAM_ASSERT(original && original->messages == chat.messages); app.chats.push_back(*original);
	ChatSession unrelated = ChatDomainService().CreateNewChat("", uam::provider_ids::kGeminiCli, false);
	unrelated.workspace_directory = (temp.root / "unrelated-workspace").string(); fs::create_directories(fs::path(unrelated.workspace_directory));
	unrelated.messages = {{MessageRole::User, "Keep unrelated history."}};
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root, unrelated)); app.chats.push_back(unrelated);
	const auto unrelated_path = AppPaths::UamChatFilePath(app.data_root, unrelated.id);
	const auto unrelated_primary = uam_test::ReadFile(unrelated_path);
	ChatDomainService().SelectChatById(app, chat.id); app.composer_text = "Keep the unrelated live draft.";
	RemoteClaudeOrdinaryImageCleanup cleanup{app, chat.id, temp.root / "release-all-results"};
	const auto requests = [&]()
	{
		std::vector<Json> result;
		if (!fs::is_regular_file(temp.root / "peer-stdin.jsonl")) return result;
		std::istringstream lines(uam_test::ReadFile(temp.root / "peer-stdin.jsonl")); std::string line;
		while (std::getline(lines, line))
		{
			if (line.empty()) continue;
			const auto value = Json::parse(line);
			if (value.value("method", "") == "session/prompt") result.push_back(value);
		}
		return result;
	};
	const auto assert_image = [&](const Json& request, const std::string& native, const std::string& bytes, const std::string& mime)
	{
		UAM_ASSERT_EQ(request.at("method"), Json("session/prompt"));
		UAM_ASSERT_EQ(request.at("params").at("sessionId"), Json(native));
		UAM_ASSERT(request.at("id").is_number_integer() && request.at("id").get<int>() > 0);
		std::vector<Json> images;
		for (const auto& part : request.at("params").at("prompt")) if (part.value("type", "") == "image") images.push_back(part);
		UAM_ASSERT_EQ(images.size(), std::size_t{1});
		UAM_ASSERT_EQ(images.front().at("mimeType"), Json(mime));
		UAM_ASSERT(!images.front().contains("path") && !images.front().contains("uri"));
		std::string decoded; UAM_ASSERT(uam::base64::Decode(images.front().at("data").get<std::string>(), decoded));
		UAM_ASSERT_EQ(decoded, bytes);
	};
	MessageAttachment current; current.id = "current-image"; current.name = "Current remote PNG";
	current.kind = "image"; current.mime_type = "image/png"; current.path = "current.png";
	std::string error;
	if (control == RemoteGeminiOwnedRetryControl::NegotiatedFalse)
	{
		UAM_ASSERT(uam::SendAcpPrompt(app, chat.id, task, {}, {}, false, &error) && error.empty());
	}
	else
	{
	UAM_ASSERT(uam::SendAcpPrompt(app, chat.id, task, {}, {current}, false, &error) && error.empty());
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	bool first_receipt = false;
	while (std::chrono::steady_clock::now() < deadline)
	{
		cleanup.Tick();
		const auto* runtime = uam::FindAcpSessionForChat(app, chat.id);
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
		if (fs::is_regular_file(temp.root / "peer-input-1.json") && runtime && runtime->running && runtime->session_ready && runtime->processing &&
		    saved && !saved->native_session_id.empty() && saved->native_session_id == runtime->session_id && saved->remote_turn_reconnect_pending &&
		    saved->remote_prompt_delivery_id.empty() && saved->remote_prompt_delivery_payload.empty() && !runtime->pending_prompt_delivery &&
		    app.pending_chat_save_at_by_chat_id.empty() && app.pending_chat_save_tasks.empty()) { first_receipt = true; break; }
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT(first_receipt); // Peer result withheld: cleared protocol3 intent requires actual input ACK.
	auto* session = uam::FindAcpSessionForChat(app, chat.id);
	auto* owner = ChatDomainService().FindChatById(app, chat.id);
	if (control == RemoteGeminiOwnedRetryControl::NegotiatedFalse)
	{
		UAM_ASSERT(session && owner && session->initialized && !session->prompt_image_supported && !owner->remote_prompt_image_supported);
	}
	else
	{
	UAM_ASSERT(session && owner && session->initialized && session->prompt_image_supported && owner->remote_prompt_image_supported);
	}
	UAM_ASSERT_EQ(session->protocol_kind, std::string("gemini-acp"));
	UAM_ASSERT_EQ(session->agent_version, std::string("0.55.1"));
	UAM_ASSERT_EQ(session->process_execution_host_id, host.id); UAM_ASSERT_EQ(session->remote_runner_protocol_version, 3);
	UAM_ASSERT_EQ(session->current_model_id, model); UAM_ASSERT_EQ(session->current_mode_id, std::string("default"));
	const auto negotiated = Json::parse(uam_test::ReadFile(temp.root / "peer-initialize-response.json"));
	if (control == RemoteGeminiOwnedRetryControl::NegotiatedFalse)
	{
		UAM_ASSERT(negotiated.at("result").at("agentCapabilities").at("promptCapabilities").at("image") == false);
	}
	else
	{
	UAM_ASSERT(negotiated.at("result").at("agentCapabilities").at("promptCapabilities").at("image") == true);
	}
	const auto native_setup = Json::parse(uam_test::ReadFile(temp.root / "peer-session-response.json"));
	const std::string native = native_setup.at("result").at("sessionId").get<std::string>();
	UAM_ASSERT(!native.empty() && session->session_id == native && owner->native_session_id == native);
	UAM_ASSERT_EQ(native_setup.at("result").at("models").at("currentModelId"), Json(model));
	const auto launch = Json::parse(uam_test::ReadFile(temp.root / "peer-launch.json"));
	UAM_ASSERT_EQ(launch.at("cwd"), Json(workspace.string())); UAM_ASSERT_EQ(launch.at("executable"), Json(gemini.string()));
	const auto first_input = Json::parse(uam_test::ReadFile(temp.root / "peer-input-1.json"));
	UAM_ASSERT_EQ(first_input.at("pid"), launch.at("pid"));
	if (control == RemoteGeminiOwnedRetryControl::NegotiatedFalse)
	{
		UAM_ASSERT_EQ(first_input.at("request").at("params").at("sessionId"), Json(native));
		for (const auto& part : first_input.at("request").at("params").at("prompt")) UAM_ASSERT_EQ(part.at("type"), Json("text"));
	}
	else
	{
	assert_image(first_input.at("request"), native, png, "image/png");
	}
	std::string first_text;
	for (const auto& part : first_input.at("request").at("params").at("prompt")) if (part.value("type", "") == "text") first_text += part.value("text", "");
	UAM_ASSERT(first_text.find(task) != std::string::npos);
	const auto first_saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
	UAM_ASSERT(first_saved && first_saved->execution_host_id == host.id && first_saved->native_session_id == native);
	UAM_ASSERT_EQ(first_saved->remote_process_control_token, session->remote_output_delivery_token);
	UAM_ASSERT_EQ(session->turn_user_message_index, static_cast<int>(original->messages.size()));
	const auto& first_user = first_saved->messages.at(original->messages.size());
	UAM_ASSERT(first_user.role == MessageRole::User && !first_user.acp_prompt_not_sent);
	if (control == RemoteGeminiOwnedRetryControl::NegotiatedFalse)
	{
		UAM_ASSERT_EQ(first_user.content, task); UAM_ASSERT(first_user.attachments.empty());
	}
	else
	{
	UAM_ASSERT_EQ(first_user.content, task); UAM_ASSERT_EQ(first_user.attachments, std::vector<MessageAttachment>{current});
	}
	UAM_ASSERT_EQ(requests(), std::vector<Json>{first_input.at("request")});
	UAM_ASSERT_EQ(first_input.at("request").at("id"), Json(session->prompt_request_id));
	UAM_ASSERT_EQ(session->pending_request_methods.at(session->prompt_request_id), std::string("session/prompt"));
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "release-result-1", "Release actual first ACP result"));
	bool first_finished = false;
	while (std::chrono::steady_clock::now() < deadline)
	{
		cleanup.Tick();
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
		if (!session->processing && session->session_ready && saved && saved->messages.size() == original->messages.size() + 2 &&
		    saved->messages.back().content == answer && app.pending_chat_save_at_by_chat_id.empty() && app.pending_chat_save_tasks.empty() &&
		    saved->remote_pending_requests.empty() && owner->remote_pending_requests.empty()) { first_finished = true; break; }
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT(first_finished && session->last_error.empty() && session->pending_request_methods.empty());
	UAM_ASSERT(std::equal(original->messages.begin(), original->messages.end(), owner->messages.begin()));
	const auto original_proxy = session->child_pid;
	const auto original_process = uam::acp_detail::AcpProcessHandleLabel(*session);
	const std::string original_token = session->remote_output_delivery_token;
	UAM_ASSERT(original_proxy > 0 && !original_process.empty() && !original_token.empty() && owner->remote_process_control_token == original_token);
	// Public typed DATA publication supplies an original; it does not mint any native execution evidence.
	const auto revision = ChatRepository::ChatStorageRevision(app.data_root, chat.id); UAM_ASSERT(revision && *revision != "missing");
	const auto published_prefix = owner->messages;
	ChatSession candidate = *owner;
	Message row; row.role = MessageRole::User; row.content = retry_task; row.created_at = uam::time::TimestampNow(); row.acp_prompt_not_sent = true;
	MessageAttachment pending; pending.id = "remote-retained-gif"; pending.name = "Retained remote GIF"; pending.kind = "image";
	pending.source = AttachmentSource::NativeInlinePending; pending.native_part_key = "v1:data-fixture/remote-gemini/user/image/0";
	pending.native_source_field = "data-fixture/user/attachments/0"; pending.origin_execution_host_id = host.id;
	std::string media_error;
	pending.native_inline_payload = uam::native_assets::MakeInlinePayload(uam::base64::Encode(gif), "image/gif", std::make_shared<uam::native_assets::PayloadBudget>(), media_error);
	UAM_ASSERT(pending.native_inline_payload && media_error.empty());
	pending.mime_type = pending.native_inline_payload->MimeType(); pending.size_bytes = pending.native_inline_payload->DecodedSize(); pending.content_digest = pending.native_inline_payload->Digest();
	row.attachments = {pending}; candidate.messages.push_back(std::move(row));
	const auto incarnation = owner->metadata_incarnation;
	auto worker = std::async(std::launch::async, [root = app.data_root, candidate = std::move(candidate), revision = *revision]() mutable
	{
		std::string failure;
		auto prepared = uam::PreparedNativeMediaChatSave::Prepare(root, std::move(candidate), revision, failure);
		if (!prepared || !prepared->Publish(failure) || !prepared->AcknowledgeOriginals(failure)) return std::make_pair(std::optional<ChatSession>{}, failure);
		return std::make_pair(prepared->TakeCommittedChat(), failure);
	});
	auto result = worker.get(); UAM_ASSERT(result.first && result.second.empty() && result.first->metadata_incarnation == incarnation);
	*owner = std::move(*result.first);
	const auto stored = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
	UAM_ASSERT(stored && stored->messages == owner->messages && stored->native_session_id == native);
	UAM_ASSERT_EQ(stored->messages.size(), published_prefix.size() + 1);
	UAM_ASSERT(std::equal(published_prefix.begin(), published_prefix.end(), stored->messages.begin()));
	const int index = static_cast<int>(stored->messages.size()) - 1;
	UAM_ASSERT(stored->messages.back().role == MessageRole::User && stored->messages.back().acp_prompt_not_sent && stored->messages.back().content == retry_task && stored->messages.back().attachments.size() == 1);
	const auto retained = stored->messages.back().attachments.front();
	UAM_ASSERT(retained.source == AttachmentSource::OwnedNativeAsset && retained.path.empty() && !retained.native_inline_payload);
	UAM_ASSERT_EQ(retained.origin_execution_host_id, host.id);
	UAM_ASSERT(uam::native_assets::IsKeyForOwner(chat.id, retained.native_asset_key) && !uam::native_assets::IsKeyForOwner(native, retained.native_asset_key));
	const auto bytes = uam::native_assets::ReadOwned(app.data_root, chat.id, retained.native_asset_key); UAM_ASSERT(bytes.ok && bytes.bytes == gif);
	// Failure-only scalar observation: retain the exact eligibility assertion and all runtime authority.
	const auto actual_known_retry_indices = uam::KnownUnsentAcpMessageIndices(app, *owner);
	const auto actual_recoverable_retry_indices = uam::RecoverableUnsentAcpMessageIndices(app, *owner);
	if (actual_recoverable_retry_indices != std::vector<int>{index})
	{
		const auto chat_gates = [](const ChatSession& value)
		{
			std::size_t consumed = 0;
			for (const auto& request : value.remote_pending_requests) if (request.response_consumed) ++consumed;
			return Json{
			    {"loaded", value.messages_loaded}, {"imported", value.imported_read_only}, {"queue_error", value.acp_queue_storage_error},
			    {"turn_reconnect", value.remote_turn_reconnect_pending}, {"restart", value.remote_restart_pending},
			    {"stop_cleanup", value.remote_stop_cleanup_pending}, {"source_exit", value.remote_source_exit_pending},
			    {"active_turn", !value.remote_active_turn_id.empty()}, {"delivery_session", !value.remote_prompt_delivery_session_id.empty()},
			    {"delivery_id", !value.remote_prompt_delivery_id.empty()}, {"delivery_payload", !value.remote_prompt_delivery_payload.empty()},
			    {"pending_requests", value.remote_pending_requests.size()}, {"consumed_requests", consumed},
			    {"interaction_responses", value.remote_interaction_responses.size()}, {"side_cleanup", value.side_cleanup_requested},
			    {"queued", value.acp_queued_prompts.size()}, {"dispatched", value.acp_dispatched_queued_prompt_count},
			    {"turn_user_index", value.remote_turn_user_message_index}, {"rows", value.messages.size()}};
		};
		const auto diagnostic_saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
		const Message& diagnostic_row = owner->messages.at(static_cast<std::size_t>(index));
		const Json runtime_gates{
		    {"running", session->running}, {"initialized", session->initialized}, {"ready", session->session_ready},
		    {"native_matches", session->session_id == owner->native_session_id}, {"provider_matches", session->provider_id == owner->provider_id},
		    {"processing", session->processing}, {"compaction", static_cast<bool>(session->pending_compaction)},
		    {"waiting_permission", session->waiting_for_permission}, {"waiting_input", session->waiting_for_user_input},
		    {"queued_prompt", !session->queued_prompt.empty()}, {"queued_users", session->queued_user_prompts.size()},
		    {"background", ProviderRuntimeRegistry::ResolveById(session->provider_id).AcpBackgroundTaskCount(*session)},
		    {"cancel", session->cancel_requested || session->cancel_request_id != 0}, {"reconnect", session->reconnect_pending},
		    {"local_stop", session->local_stop_pending}, {"remote_stop", session->remote_stop_pending}, {"unconfirmed_stop", session->remote_stop_unconfirmed},
		    {"prompt_delivery", static_cast<bool>(session->pending_prompt_delivery)}, {"launch", static_cast<bool>(session->pending_local_launch)},
		    {"local_receipt", static_cast<bool>(session->pending_local_session_receipt)}, {"agent_input", static_cast<bool>(session->pending_agent_input)},
		    {"queued_history", static_cast<bool>(session->pending_queued_history)}, {"recover_turn", session->recovering_remote_turn},
		    {"recover_process", session->recovering_remote_process}, {"model_discovery", session->model_discovery_only},
		    {"goal_review", session->goal_review_scheduled}, {"checkpoint", session->turn_checkpoint_commit_pending},
		    {"managed_run", !session->managed_agent_run_id.empty()}, {"steers", session->pending_steer_requests.size()},
		    {"request_methods", session->pending_request_methods.size()}, {"prompt_request", session->prompt_request_id != 0},
		    {"setup_request", session->initialize_request_id != 0 || session->session_setup_request_id != 0 || session->startup_model_request_id != 0},
		    {"config_request", session->reasoning_change_request_id != 0 || session->config_option_change_request_id != 0 || session->mode_change_request_id != 0 || session->model_change_request_id != 0 || session->awaiting_model_config_options},
		    {"turn_first_index", session->turn_first_user_message_index}, {"turn_user_index", session->turn_user_message_index},
		    {"output_ack", !session->pending_remote_output_ack_line.empty()}, {"input_receipt", !session->pending_remote_input_receipt_id.empty()},
		    {"exit_ack", !session->pending_remote_source_exit_ack_line.empty()}, {"stdout_poll", session->stdout_poll_pending}, {"stderr_poll", session->stderr_poll_pending}};
		std::cerr << "PRIVATE_REMOTE_GEMINI_RETRY_ELIGIBILITY " << Json{
		    {"known", actual_known_retry_indices}, {"recoverable", actual_recoverable_retry_indices}, {"expected", index},
		    {"live", chat_gates(*owner)}, {"saved_exists", static_cast<bool>(diagnostic_saved)},
		    {"saved", diagnostic_saved ? chat_gates(*diagnostic_saved) : Json(nullptr)}, {"runtime", runtime_gates},
		    {"stop_in_progress", uam::AcpStopInProgress(app, chat.id)}, {"worktree_operation", app.worktree_operation_chat_ids.contains(chat.id)},
		    {"pending_save", app.pending_chat_save_tasks.contains(chat.id)}, {"scheduled_save", app.pending_chat_save_at_by_chat_id.contains(chat.id)},
		    {"row_user", diagnostic_row.role == MessageRole::User}, {"row_unsent", diagnostic_row.acp_prompt_not_sent}, {"row_discarded", diagnostic_row.acp_prompt_discarded},
		    {"row_provider_matches", diagnostic_row.provider.empty() || diagnostic_row.provider == owner->provider_id},
		    {"row_tools", diagnostic_row.tool_calls.size()}, {"row_thoughts", diagnostic_row.thoughts.size()},
		    {"row_plan", diagnostic_row.plan_entries.size()}, {"row_plan_summary", !diagnostic_row.plan_summary.empty()}, {"row_blocks", diagnostic_row.blocks.size()}}
		    .dump() << std::endl;
	}
	UAM_ASSERT_EQ(uam::RecoverableUnsentAcpMessageIndices(app, *owner), std::vector<int>{index});
	if (control == RemoteGeminiOwnedRetryControl::NegotiatedFalse)
	{
		UAM_ASSERT(session->running && session->session_ready && !session->processing && !session->prompt_image_supported);
	}
	else
	{
	UAM_ASSERT(session->running && session->session_ready && !session->processing && session->prompt_image_supported);
	}
	const auto primary = AppPaths::UamChatFilePath(app.data_root, chat.id), summary = AppPaths::UamChatSummaryFilePath(app.data_root, chat.id);
	const auto before_primary = uam_test::ReadFile(primary), before_summary = uam_test::ReadFile(summary);
	const auto before_messages = owner->messages;
	const auto before_stdin = uam_test::ReadFile(temp.root / "peer-stdin.jsonl");
	if (control == RemoteGeminiOwnedRetryControl::NegotiatedFalse)
	{
		const auto image_task_count = app.claude_input_tasks.size();
		std::string unsupported;
		UAM_ASSERT(!uam::RetryFailedAcpMessage(app, chat.id, index, &unsupported));
		UAM_ASSERT_EQ(unsupported, std::string("Saved image Retry needs this chat's existing ready SSH session. Reopen it or attach the files again."));
		UAM_ASSERT_EQ(uam_test::ReadFile(primary), before_primary); UAM_ASSERT_EQ(uam_test::ReadFile(summary), before_summary);
		UAM_ASSERT_EQ(owner->messages, before_messages); UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "peer-stdin.jsonl"), before_stdin);
		UAM_ASSERT(!session->saved_image_retry && !session->claude_input_preparation && !session->pending_prompt_delivery && !session->processing);
		UAM_ASSERT_EQ(app.claude_input_tasks.size(), image_task_count);
		UAM_ASSERT(!session->prompt_image_supported && !owner->remote_prompt_image_supported);
		UAM_ASSERT_EQ(session->child_pid, original_proxy); UAM_ASSERT_EQ(uam::acp_detail::AcpProcessHandleLabel(*session), original_process);
		UAM_ASSERT_EQ(session->remote_output_delivery_token, original_token); UAM_ASSERT_EQ(session->session_id, native);
		UAM_ASSERT_EQ(requests(), std::vector<Json>{first_input.at("request")}); UAM_ASSERT(!fs::exists(temp.root / "peer-input-2.json"));
		const auto retained_original = uam::native_assets::ReadOwned(app.data_root, chat.id, retained.native_asset_key);
		UAM_ASSERT(retained_original.ok && retained_original.bytes == gif);
		cleanup.StopAndDrain(); UAM_ASSERT(cleanup.Quiet() && cleanup.proxy_pid == original_proxy);
		int status = 0; errno = 0; UAM_ASSERT(waitpid(original_proxy, &status, WNOHANG) == -1 && errno == ECHILD);
		const auto final = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
		UAM_ASSERT(final && final->messages == before_messages && !final->remote_process_exists && !final->remote_stop_cleanup_pending);
		UAM_ASSERT_EQ(final->execution_host_id, host.id); UAM_ASSERT_EQ(final->native_session_id, native);
		UAM_ASSERT_EQ(final->workspace_directory, workspace.string()); UAM_ASSERT_EQ(final->model_id, model);
		UAM_ASSERT_EQ(final->approval_mode, std::string("default")); UAM_ASSERT_EQ(final->command_safety_tier, std::string("off"));
		UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "peer-stdin.jsonl"), before_stdin);
		UAM_ASSERT_EQ(uam_test::ReadFile(unrelated_path), unrelated_primary); UAM_ASSERT_EQ(app.composer_text, std::string("Keep the unrelated live draft."));
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), chat.id);
		return;
	}
	std::string retry_error;
	const bool accepted = uam::RetryFailedAcpMessage(app, chat.id, index, &retry_error);
	if (!accepted)
	{
		UAM_ASSERT_EQ(retry_error, std::string("Saved image Retry needs this chat's existing ready SSH session. Reopen it or attach the files again."));
		UAM_ASSERT_EQ(uam_test::ReadFile(primary), before_primary); UAM_ASSERT_EQ(uam_test::ReadFile(summary), before_summary);
		UAM_ASSERT_EQ(owner->messages, before_messages); UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "peer-stdin.jsonl"), before_stdin);
		UAM_ASSERT(!session->saved_image_retry && !session->claude_input_preparation && !session->pending_prompt_delivery && !session->processing);
		UAM_ASSERT_EQ(requests().size(), std::size_t{1}); UAM_ASSERT(!fs::exists(temp.root / "peer-input-2.json"));
		UAM_ASSERT_EQ(session->child_pid, original_proxy); UAM_ASSERT_EQ(session->remote_output_delivery_token, original_token);
	}
	UAM_ASSERT(accepted && retry_error.empty()); // Prospective intended RED ONLY after the real remote/owned/index/refusal prerequisites above.
	const auto retry_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	bool retry_receipt = false;
	while (std::chrono::steady_clock::now() < retry_deadline)
	{
		cleanup.Tick();
		const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
		if (fs::is_regular_file(temp.root / "peer-input-2.json") && session->processing && saved && saved->native_session_id == native &&
		    saved->remote_turn_reconnect_pending && saved->remote_prompt_delivery_id.empty() && saved->remote_prompt_delivery_payload.empty() &&
		    !session->pending_prompt_delivery && app.pending_chat_save_tasks.empty() && app.pending_chat_save_at_by_chat_id.empty()) { retry_receipt = true; break; }
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT(retry_receipt && session->last_error.empty() && session->prompt_image_supported);
	UAM_ASSERT_EQ(session->session_id, native); UAM_ASSERT_EQ(session->child_pid, original_proxy);
	UAM_ASSERT_EQ(uam::acp_detail::AcpProcessHandleLabel(*session), original_process); UAM_ASSERT_EQ(session->remote_output_delivery_token, original_token);
	UAM_ASSERT_EQ(session->current_model_id, model); UAM_ASSERT_EQ(session->current_mode_id, std::string("default"));
	const auto second_input = Json::parse(uam_test::ReadFile(temp.root / "peer-input-2.json"));
	UAM_ASSERT_EQ(second_input.at("pid"), launch.at("pid")); assert_image(second_input.at("request"), native, gif, "image/gif");
	UAM_ASSERT_EQ(requests(), (std::vector<Json>{first_input.at("request"), second_input.at("request")}));
	UAM_ASSERT_EQ(second_input.at("request").at("id"), Json(session->prompt_request_id));
	std::string retry_text;
	for (const auto& part : second_input.at("request").at("params").at("prompt")) if (part.value("type", "") == "text") retry_text += part.value("text", "");
	UAM_ASSERT(retry_text.find(retry_task) != std::string::npos);
	UAM_ASSERT(retry_text.ends_with("\nImage 1 (native image-block order): " + Json(retained.name).dump()));
	const auto delivered = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
	UAM_ASSERT(delivered && delivered->messages.size() == before_messages.size() && session->turn_user_message_index == index);
	UAM_ASSERT_EQ(delivered->messages[static_cast<std::size_t>(index)].attachments, std::vector<MessageAttachment>{retained});
	UAM_ASSERT(!delivered->messages[static_cast<std::size_t>(index)].acp_prompt_not_sent);
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "release-result-2", "Release actual retry ACP result"));
	bool retry_finished = false;
	while (std::chrono::steady_clock::now() < retry_deadline)
	{
		cleanup.Tick(); const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
		if (!session->processing && saved && saved->messages.size() == before_messages.size() + 1 && saved->messages.back().content == retry_answer &&
		    app.pending_chat_save_tasks.empty() && app.pending_chat_save_at_by_chat_id.empty()) { retry_finished = true; break; }
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT(retry_finished && session->last_error.empty());
	if (control != RemoteGeminiOwnedRetryControl::Healthy)
	{
		// A complete healthy indexed owned-image delivery above qualifies every refusal arm.
		// Finish actual RPC tombstone retirement within the SAME existing retry deadline.
		bool retired = false;
		while (std::chrono::steady_clock::now() < retry_deadline)
		{
			cleanup.Tick(); const auto saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
			if (saved && saved->remote_pending_requests.empty() && owner->remote_pending_requests.empty() &&
			    !session->processing && session->pending_request_methods.empty() && app.pending_chat_save_tasks.empty() &&
			    app.pending_chat_save_at_by_chat_id.empty()) { retired = true; break; }
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		UAM_ASSERT(retired && session->last_error.empty() && session->session_ready);
		UAM_ASSERT_EQ(owner->messages.size(), before_messages.size() + 1);
		UAM_ASSERT_EQ(owner->messages.back().content, retry_answer);
		UAM_ASSERT(!owner->messages[static_cast<std::size_t>(index)].acp_prompt_not_sent);
		const auto healthy_messages = owner->messages;
		Message bad_row; bad_row.role = MessageRole::User; bad_row.content = "Refuse the complete third retained-image input on changed evidence.";
		bad_row.created_at = uam::time::TimestampNow(); bad_row.acp_prompt_not_sent = true; bad_row.attachments = {retained};
		if (control == RemoteGeminiOwnedRetryControl::MixedLegacy) bad_row.attachments.push_back(current);
		if (control == RemoteGeminiOwnedRetryControl::WrongHost)
		{
			bad_row.attachments.front().origin_execution_host_id = "private-foreign-gemini-host";
			UAM_ASSERT(bad_row.attachments.front().origin_execution_host_id != host.id);
			const auto same_original = uam::native_assets::ReadOwned(app.data_root, chat.id, bad_row.attachments.front().native_asset_key);
			UAM_ASSERT(same_original.ok && same_original.bytes == gif);
		}
		owner->messages.push_back(bad_row);
		UAM_ASSERT(ChatRepository::SaveChat(app.data_root, *owner));
		const auto bad_saved = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
		UAM_ASSERT(bad_saved && bad_saved->messages == owner->messages && bad_saved->messages.back().attachments == bad_row.attachments);
		const int bad_index = static_cast<int>(owner->messages.size()) - 1;
		UAM_ASSERT_EQ(uam::RecoverableUnsentAcpMessageIndices(app, *owner), std::vector<int>{bad_index});
		const auto expected_messages = owner->messages;
		const auto bad_primary = uam_test::ReadFile(primary), bad_summary = uam_test::ReadFile(summary);
		const auto no_third_input = uam_test::ReadFile(temp.root / "peer-stdin.jsonl");
		const auto expected_process_exists = owner->remote_process_exists;
		const auto expected_process_control = owner->remote_process_control_token;
		fs::path missing_path;
		if (control == RemoteGeminiOwnedRetryControl::MissingOriginal)
		{
			std::vector<std::string> parts; std::istringstream key(retained.native_asset_key);
			for (std::string part; std::getline(key, part, ':');) parts.push_back(part);
			UAM_ASSERT(parts.size() == 5 && parts[0] == "na1");
			missing_path = app.data_root / std::string(uam::native_assets::kStoreDirectory) / parts[1] / parts[2] / (parts[3] + ".bin");
			UAM_ASSERT(fs::remove(missing_path));
			UAM_ASSERT(!uam::native_assets::ReadOwned(app.data_root, chat.id, retained.native_asset_key).ok);
		}
		const auto control_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		std::string refusal;
		const bool bad_accepted = uam::RetryFailedAcpMessage(app, chat.id, bad_index, &refusal);
		const auto save_completions = std::make_shared<std::atomic<int>>(0);
		if (control == RemoteGeminiOwnedRetryControl::MixedLegacy || control == RemoteGeminiOwnedRetryControl::WrongHost)
		{
			UAM_ASSERT(!bad_accepted && !refusal.empty());
			UAM_ASSERT_EQ(uam_test::ReadFile(primary), bad_primary); UAM_ASSERT_EQ(uam_test::ReadFile(summary), bad_summary);
			UAM_ASSERT_EQ(owner->messages, expected_messages);
			UAM_ASSERT(!session->saved_image_retry && !session->claude_input_preparation && !session->pending_prompt_delivery && !session->processing);
		}
		else
		{
			UAM_ASSERT(bad_accepted && refusal.empty());
			while (!session->claude_input_preparation && session->processing && std::chrono::steady_clock::now() < control_deadline)
			{
				(void)uam::FlushPendingChatSaves(app);
				(void)uam::acp_detail::SendQueuedPromptIfReady(app, *session, *owner);
				std::this_thread::yield();
			}
			UAM_ASSERT(session->claude_input_preparation && session->saved_image_retry && session->saved_image_retry->remote_scope);
			const auto state = session->claude_input_preparation->state;
			while (!state->finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < control_deadline) std::this_thread::yield();
			UAM_ASSERT(state->finished.load(std::memory_order_acquire));
			UAM_ASSERT(state->sealed_message.is_null() && !session->pending_prompt_delivery && requests().size() == 2);
			if (control == RemoteGeminiOwnedRetryControl::MissingOriginal)
				UAM_ASSERT(!state->error.empty() && state->message.is_null());
			else
			{
				UAM_ASSERT(state->error.empty() && state->message.is_object());
				const auto& prepared = state->message.at("message").at("content");
				UAM_ASSERT_EQ(prepared.size(), std::size_t{2}); UAM_ASSERT_EQ(prepared[1].at("type"), Json("image"));
				UAM_ASSERT_EQ(prepared[1].at("source").at("media_type"), Json("image/gif"));
				std::string prepared_original; UAM_ASSERT(uam::base64::Decode(prepared[1].at("source").at("data").get<std::string>(), prepared_original));
				UAM_ASSERT_EQ(prepared_original, gif);
				// Only revoke observed evidence AFTER a genuine successful worker; never create native authority.
				if (control == RemoteGeminiOwnedRetryControl::WorkerMode) session->current_mode_id = "negative-private-mode-drift";
				if (control == RemoteGeminiOwnedRetryControl::WorkerModel) session->current_model_id = "negative-private-model-drift";
				if (control == RemoteGeminiOwnedRetryControl::PendingNative)
				{
					UAM_ASSERT(!app.pending_chat_save_at_by_chat_id.contains(chat.id) && !app.pending_chat_save_tasks.contains(chat.id));
					UAM_ASSERT(uam::acp_detail::SendQueuedPromptIfReady(app, *session, *owner));
					UAM_ASSERT(session->pending_prompt_delivery && !state->sealed_message.is_null());
					UAM_ASSERT_EQ(state->sealed_method, std::string("session/prompt"));
					UAM_ASSERT(state->sealed_request_id > 0 && session->prompt_request_id == 0);
					assert_image(state->sealed_message, native, gif, "image/gif");
					// Public state witnesses the real remote packet and its still-unsettled durable save.
					const auto& scope = *session->saved_image_retry->remote_scope;
					const auto* packet_host = uam::execution_hosts::Find(app.settings.execution_hosts, owner->execution_host_id);
					UAM_ASSERT(packet_host && uam::execution_hosts::SameConnection(*packet_host, scope.host));
					UAM_ASSERT(scope.proxy_protocol == 3 && session->remote_runner_protocol_version == 3 && packet_host->runner_protocol_version == 3);
					UAM_ASSERT_EQ(scope.proxy_token, original_token); UAM_ASSERT_EQ(owner->remote_process_control_token, original_token);
					UAM_ASSERT_EQ(session->remote_output_delivery_token, original_token);
					UAM_ASSERT_EQ(owner->native_session_id, native); UAM_ASSERT_EQ(session->session_id, native);
					UAM_ASSERT(owner->remote_turn_reconnect_pending && owner->remote_prompt_delivery_session_id == "acp-" + chat.id && !owner->remote_prompt_delivery_id.empty());
					UAM_ASSERT_EQ(owner->remote_prompt_delivery_payload, uam::acp_detail::SerializeStructuredMessage(*session, state->sealed_message));
					UAM_ASSERT_EQ(owner->remote_pending_requests.size(), std::size_t{1});
					const auto& pending_request = owner->remote_pending_requests.front();
					UAM_ASSERT(pending_request.request_id == state->sealed_request_id && pending_request.method == state->sealed_method &&
					    pending_request.turn_serial == session->turn_serial && !pending_request.response_consumed);
					UAM_ASSERT(app.pending_chat_save_at_by_chat_id.contains(chat.id));
					const auto pending_save = app.pending_chat_save_tasks.find(chat.id);
					UAM_ASSERT(pending_save != app.pending_chat_save_tasks.end() && pending_save->second);
					UAM_ASSERT(uam::DeferChatSaveResult(app, chat.id, [save_completions](bool) { ++*save_completions; }));
					UAM_ASSERT_EQ(save_completions->load(), 0); // No owner polling/Flush occurred since staging the actual packet.
					owner->native_session_id.clear(); // Negative drift only, after sealing and before the actual durable callback.
				}
			}
			while ((session->processing || session->saved_image_retry || session->claude_input_preparation || session->pending_prompt_delivery ||
			    !app.pending_chat_save_tasks.empty() || !app.pending_chat_save_at_by_chat_id.empty()) && std::chrono::steady_clock::now() < control_deadline)
			{ cleanup.Tick(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
			UAM_ASSERT(!session->processing && !session->last_error.empty());
			UAM_ASSERT(!session->saved_image_retry && !session->claude_input_preparation && !session->pending_prompt_delivery);
			if (control == RemoteGeminiOwnedRetryControl::PendingNative) UAM_ASSERT_EQ(save_completions->load(), 1);
		}
		UAM_ASSERT(app.pending_chat_save_tasks.empty() && app.pending_chat_save_at_by_chat_id.empty());
		const auto refused = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
		UAM_ASSERT(refused && refused->messages.size() == expected_messages.size() && owner->messages.size() == expected_messages.size());
		UAM_ASSERT(std::equal(healthy_messages.begin(), healthy_messages.end(), refused->messages.begin()));
		UAM_ASSERT(std::equal(healthy_messages.begin(), healthy_messages.end(), owner->messages.begin()));
		for (const auto* actual : std::array<const Message*, 2>{&refused->messages.back(), &owner->messages.back()})
		{
			UAM_ASSERT_EQ(actual->role, bad_row.role); UAM_ASSERT_EQ(actual->content, bad_row.content);
			UAM_ASSERT_EQ(actual->created_at, bad_row.created_at); UAM_ASSERT_EQ(actual->attachments, bad_row.attachments);
			UAM_ASSERT(actual->acp_prompt_not_sent && !actual->acp_prompt_discarded);
		}
		const auto expected_native = control == RemoteGeminiOwnedRetryControl::PendingNative ? std::string{} : native;
		UAM_ASSERT_EQ(refused->native_session_id, expected_native); UAM_ASSERT_EQ(owner->native_session_id, expected_native);
		UAM_ASSERT_EQ(refused->execution_host_id, host.id); UAM_ASSERT_EQ(refused->model_id, model);
		UAM_ASSERT_EQ(refused->approval_mode, std::string("default")); UAM_ASSERT_EQ(refused->command_safety_tier, std::string("off"));
		UAM_ASSERT_EQ(refused->remote_process_exists, expected_process_exists); UAM_ASSERT_EQ(refused->remote_process_control_token, expected_process_control);
		UAM_ASSERT_EQ(session->session_id, native); UAM_ASSERT_EQ(session->child_pid, original_proxy);
		UAM_ASSERT_EQ(uam::acp_detail::AcpProcessHandleLabel(*session), original_process); UAM_ASSERT_EQ(session->remote_output_delivery_token, original_token);
		UAM_ASSERT_EQ(requests(), (std::vector<Json>{first_input.at("request"), second_input.at("request")}));
		UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "peer-stdin.jsonl"), no_third_input); UAM_ASSERT(!fs::exists(temp.root / "peer-input-3.json"));
		UAM_ASSERT_EQ(uam_test::ReadFile(workspace / "current.png"), png); UAM_ASSERT_EQ(uam_test::ReadFile(workspace / "old.gif"), gif);
		UAM_ASSERT_EQ(uam_test::ReadFile(unrelated_path), unrelated_primary); UAM_ASSERT_EQ(app.composer_text, std::string("Keep the unrelated live draft."));
		UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), chat.id);
		// Restore private negative-only drift and deliberately removed bytes only after every refusal oracle.
		if (!missing_path.empty()) UAM_ASSERT(uam::io::WriteBinaryFile(missing_path, gif));
		UAM_ASSERT_EQ(uam::native_assets::ReadOwned(app.data_root, chat.id, retained.native_asset_key).bytes, gif);
		owner->native_session_id = native; session->current_model_id = model; session->current_mode_id = "default";
		cleanup.StopAndDrain(); UAM_ASSERT(cleanup.Quiet() && cleanup.proxy_pid == original_proxy);
		int status = 0; errno = 0; UAM_ASSERT(waitpid(original_proxy, &status, WNOHANG) == -1 && errno == ECHILD);
		UAM_ASSERT_EQ(requests().size(), std::size_t{2}); UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "peer-stdin.jsonl"), no_third_input);
		return;
	}
	cleanup.StopAndDrain(); UAM_ASSERT(cleanup.Quiet() && cleanup.proxy_pid == original_proxy);
	int status = 0; errno = 0; UAM_ASSERT(waitpid(original_proxy, &status, WNOHANG) == -1 && errno == ECHILD);
	const auto final = ChatRepository::LoadLocalChat(app.data_root, chat.id, true);
	UAM_ASSERT(final && final->messages == owner->messages && final->native_session_id == native && final->messages.size() == before_messages.size() + 1);
	UAM_ASSERT(!final->remote_process_exists && !final->remote_stop_cleanup_pending);
	UAM_ASSERT_EQ(final->execution_host_id, host.id); UAM_ASSERT_EQ(final->workspace_directory, workspace.string());
	UAM_ASSERT_EQ(final->model_id, model); UAM_ASSERT_EQ(final->approval_mode, std::string("default"));
	UAM_ASSERT_EQ(final->command_safety_tier, std::string("off"));
	UAM_ASSERT(std::equal(before_messages.begin(), before_messages.begin() + index, final->messages.begin()));
	UAM_ASSERT_EQ(final->messages[static_cast<std::size_t>(index)].attachments, std::vector<MessageAttachment>{retained});
	const auto preserved = uam::native_assets::ReadOwned(app.data_root, chat.id, retained.native_asset_key); UAM_ASSERT(preserved.ok && preserved.bytes == gif);
	UAM_ASSERT_EQ(uam_test::ReadFile(workspace / "current.png"), png); UAM_ASSERT_EQ(uam_test::ReadFile(workspace / "old.gif"), gif);
	UAM_ASSERT_EQ(uam_test::ReadFile(unrelated_path), unrelated_primary);
	UAM_ASSERT_EQ(ChatDomainService().SelectedChatId(app), chat.id); UAM_ASSERT_EQ(app.composer_text, std::string("Keep the unrelated live draft."));
	UAM_ASSERT(uam_test::ReadFile(temp.root / "ssh-invocations").find("private-gemini-image-host") != std::string::npos);
}

UAM_TEST(GeminiRemoteSavedOwnedIndexedRetryDeliversExactOriginalThroughActualRunner)
{
	CheckGeminiRemoteSavedOwnedIndexedRetry(RemoteGeminiOwnedRetryControl::Healthy);
}

UAM_TEST(GeminiRemoteSavedOwnedRetryRefusesMissingAndMixedCompleteSources)
{
	for (const auto control : {RemoteGeminiOwnedRetryControl::MissingOriginal, RemoteGeminiOwnedRetryControl::MixedLegacy})
		CheckGeminiRemoteSavedOwnedIndexedRetry(control);
}

UAM_TEST(GeminiRemoteSavedOwnedRetryRevokesWorkerModeModelAndDurableNativeDrift)
{
	for (const auto control : {RemoteGeminiOwnedRetryControl::WorkerMode, RemoteGeminiOwnedRetryControl::WorkerModel, RemoteGeminiOwnedRetryControl::PendingNative})
		CheckGeminiRemoteSavedOwnedIndexedRetry(control);
}

UAM_TEST(GeminiRemoteSavedOwnedRetryRejectsNegotiatedFalseAfterHealthyText)
{
	CheckGeminiRemoteSavedOwnedIndexedRetry(RemoteGeminiOwnedRetryControl::NegotiatedFalse);
}

UAM_TEST(GeminiRemoteSavedOwnedRetryRefusesForeignHostAfterHealthyOwnedDelivery)
{
	CheckGeminiRemoteSavedOwnedIndexedRetry(RemoteGeminiOwnedRetryControl::WrongHost);
}
#endif

#include "common/paths/workspace_root.h"

#if UAM_ENABLE_RUNTIME_COPILOT_CLI && UAM_ENABLE_RUNTIME_CODEX_CLI && UAM_ENABLE_SSH && defined(__APPLE__)
static void CheckCopilotRemoteSavedOwnedIndexedRetry(bool folder_only, bool changed_folder)
{
	using Json = nlohmann::json;
	uam_test::TempDir temp("uam-private-remote-copilot-owned");
	const auto bin = temp.root / "bin", home = temp.root / "private-home";
	const auto workspace = fs::weakly_canonical(temp.root) / "workspace";
	fs::create_directories(bin); fs::create_directories(home / "tmp"); fs::create_directories(workspace);
	const auto runner = PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath().parent_path() / "uam-runner";
	UAM_ASSERT(fs::is_regular_file(runner) && fs::equivalent(runner, uam::remote::PackagedRunnerPath()));
	const auto runner_manifest = Json::parse(uam_test::ReadFile(fs::path(runner.string() + ".manifest.json")));
	const auto runner_version = uam::strings::Trim(uam_test::ReadFile(fs::path(runner.string() + ".version")));
	UAM_ASSERT_EQ(runner_version, std::string(uam::constants::kAppVersion).substr(1));
	UAM_ASSERT_EQ(runner_manifest.at("protocolVersion"), Json(uam::remote::kRunnerProtocolVersion));
	const auto fingerprint = runner_manifest.at("sourceFingerprint").get<std::string>();
	UAM_ASSERT(fingerprint.size() == 64 && uam_test::ReadFile(runner).find(fingerprint) != std::string::npos);
	const auto ssh = bin / "ssh", shell = bin / "sh", copilot = bin / "copilot";
	UAM_ASSERT(uam::io::WriteTextFile(ssh, "#!/bin/sh\ncase \" $* \" in *' private-copilot-owned-host '*) ;; *) exit 92;; esac\nprintf '%s\\n' \"$*\" >>\"$UAM_TEST_REMOTE_COPILOT_ROOT/ssh-invocations\"\nexec \"$UAM_TEST_RUNNER\" bridge-direct\n"));
	UAM_ASSERT(uam::io::WriteTextFile(shell, "#!/bin/sh\nif [ \"$1\" = '-lc' ]; then shift; exec /bin/sh -c \"$@\"; fi\nexec /bin/sh \"$@\"\n"));
	UAM_ASSERT(uam::io::WriteTextFile(copilot, R"COPILOT_REMOTE(#!/usr/bin/python3 -I
import base64, datetime, json, os, pathlib, signal, sys, uuid
root = pathlib.Path(os.environ['UAM_TEST_REMOTE_COPILOT_ROOT'])
config = json.loads((root / 'config.json').read_text())
def store(name, value):
    temporary = root / (name + '.tmp')
    with temporary.open('w') as stream:
        json.dump(value, stream, separators=(',', ':'))
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, root / name)
if sys.argv[1:] == ['--version']:
    store('version.json', dict(pid=os.getpid(), argv=sys.argv[1:]))
    print('GitHub Copilot CLI 1.0.98', flush=True)
    sys.exit(0)
args = sys.argv[1:]
assert args[:3] == ['--headless', '--stdio', '--no-auto-update'], args
tail = args[3:]
if tail:
    assert len(tail) == 5 and tail[0] == '--plugin-dir' and tail[2] == '--agent' and tail[4] == '--excluded-tools=task,read_agent,list_agents,write_agent', tail
    plugin = pathlib.Path(tail[1]).resolve()
    assert plugin.is_relative_to(root.resolve()) and tail[3].startswith('uam-') and '/' not in tail[3]
    assert (plugin / 'plugin.json').is_file() and (plugin / 'agents' / (tail[3] + '.agent.md')).is_file()
def terminate(signum, frame):
    store('exit.json', dict(pid=os.getpid(), signal=signum))
    sys.exit(0)
signal.signal(signal.SIGTERM, terminate)
signal.signal(signal.SIGINT, terminate)
emitted_bytes = 0
def emit(value):
    global emitted_bytes
    body = json.dumps(value, separators=(',', ':')).encode()
    frame = ('Content-Length: %d\r\n\r\n' % len(body)).encode() + body
    assert emitted_bytes + len(frame) <= 16*1024*1024
    sys.stdout.buffer.write(frame)
    sys.stdout.buffer.flush()
    with (root / 'stdout-raw.bin').open('ab') as stream:
        stream.write(frame); stream.flush(); os.fsync(stream.fileno())
    emitted_bytes += len(frame)
def reply(request, result): emit(dict(jsonrpc='2.0', id=request['id'], result=result))
model = dict(id=config['model'], name='Private explicit vision model', capabilities=dict(supports=dict(vision=True),
    limits=dict(vision=dict(supported_media_types=['image/png','image/jpeg','image/gif'], max_prompt_images=4, max_prompt_image_size=2097152))))
native = None
turns = 0
folder_queries = set()
store('launch.json', dict(pid=os.getpid(), argv=args, cwd=os.getcwd(), executable=sys.argv[0]))
raw_bytes = 0
def journal_native(data):
    global raw_bytes
    # Preserve even malformed input BEFORE parsing; bounded per private native process.
    retained = data[:max(0, 16*1024*1024 - raw_bytes)]
    with (root / 'stdin-raw.bin').open('ab') as stream:
        stream.write(retained); stream.flush(); os.fsync(stream.fileno())
    raw_bytes += len(retained)
    assert len(retained) == len(data), 'Private raw input journal limit exceeded'
while True:
    lengths = []
    header_bytes = 0
    while True:
        line = sys.stdin.buffer.readline(8193)
        journal_native(line)
        header_bytes += len(line)
        assert header_bytes <= 8192, 'Private SDK header limit exceeded'
        if not line: sys.exit(0)
        if line == b'\r\n': break
        if line.lower().startswith(b'content-length:'): lengths.append(int(line.split(b':',1)[1]))
    assert len(lengths) == 1 and 0 < lengths[0] <= 16*1024*1024
    body = sys.stdin.buffer.read(lengths[0])
    journal_native(body)
    assert len(body) == lengths[0]
    request = json.loads(body)
    with (root / 'stdin.jsonl').open('a') as stream:
        stream.write(json.dumps(request, separators=(',', ':')) + '\n'); stream.flush(); os.fsync(stream.fileno())
    method = request.get('method', '')
    if method in ('status.get','models.list','session.model.getCurrent') and (root / 'hold-folder-proof').exists():
        folder_queries.add(method)
        store('held-folder-proof.json', dict(pid=os.getpid(), request=request))
        while not (root / 'release-folder-proof').exists():
            import time
            time.sleep(0.005)
    if method == 'ping': reply(request, dict(protocolVersion=3))
    elif method == 'models.list': reply(request, dict(models=[model]))
    elif method == 'status.get': reply(request, dict(version='1.0.98', protocolVersion=3))
    elif method == 'session.model.list': reply(request, dict(list=[model]))
    elif method == 'session.model.getCurrent':
        assert request['params']['sessionId'] == native
        reply(request, dict(modelId=config['model']))
    elif method == 'session.create':
        assert native is None and request['params']['model'] == config['model']
        assert pathlib.Path(request['params']['workingDirectory']).resolve() == pathlib.Path(config['workspace']).resolve()
        native = str(uuid.uuid4())
        store('setup.json', dict(request=request, native=native))
        reply(request, dict(sessionId=native))
    elif method == 'session.send':
        turns += 1; assert turns in (1,2), 'Unauthorized extra native input'
        params = request['params']; assert params['sessionId'] == native and params['mode'] == 'enqueue' and params['agentMode'] == 'interactive'
        assert len(params['attachments']) == 1
        part = params['attachments'][0]
        if turns == 1:
            assert part['type'] == 'file' and part['displayName'] == 'Current remote PNG'
            path = pathlib.Path(part['path']).resolve()
            assert path == pathlib.Path(config['workspace']).resolve() / 'current.png'
            image = path.read_bytes()
        else:
            assert part['type'] == 'blob' and part['mimeType'] == 'image/gif' and part['displayName'] == 'Retained remote GIF'
            assert 'path' not in part and 'url' not in part
            image = base64.b64decode(part['data'], validate=True)
        store('input-%d.json' % turns, dict(pid=os.getpid(), request=request, image=base64.b64encode(image).decode()))
        while not (root / ('release-%d' % turns)).exists():
            import time
            time.sleep(0.005)
        reply(request, dict(messageId=str(uuid.uuid4())))
        for kind, data in [('assistant.message',dict(messageId=str(uuid.uuid4()),content=config['answer'] if turns == 1 else config['retry_answer'])),('session.idle',{})]:
            event = dict(id=str(uuid.uuid4()),parentId=None,timestamp=datetime.datetime.now(datetime.timezone.utc).isoformat(),type=kind,data=data)
            emit(dict(jsonrpc='2.0',method='session.event',params=dict(sessionId=native,event=event)))
        store('completion-%d.json' % turns, dict(pid=os.getpid(), input_id=request['id'], emitted_bytes=emitted_bytes))
    elif method == 'session.abort': reply(request, {})
    elif method == 'session.destroy': reply(request, {})
    else:
        store('unexpected.json', dict(method=method)); raise RuntimeError('Unexpected private SDK method: '+method)
    if len(folder_queries) == 3: store('folder-queries-drained.json', dict(pid=os.getpid(), methods=sorted(folder_queries), emitted_bytes=emitted_bytes))
)COPILOT_REMOTE"));
	for (const auto& executable : {ssh, shell, copilot}) fs::permissions(executable, fs::perms::owner_all);
	std::vector<std::unique_ptr<uam_test::ScopedEnvVar>> environment;
	const auto env = [&](const std::string& key, const std::string& value) { environment.push_back(std::make_unique<uam_test::ScopedEnvVar>(key, value)); };
	env("PATH", bin.string() + ":/usr/bin:/bin"); env("HOME", home.string()); env("USERPROFILE", home.string());
	env("COPILOT_HOME", (home / "copilot").string()); env("XDG_CONFIG_HOME", (home / ".config").string()); env("TMPDIR", (home / "tmp").string());
	env("UAM_DATA_DIR", (temp.root / "controller").string()); env("UAM_TEST_RUNNER", runner.string()); env("UAM_TEST_REMOTE_COPILOT_ROOT", temp.root.string());
	const std::string model = "private-copilot-explicit-vision-model", answer = "Actual private Copilot ordinary image turn.", retry_answer = "Actual private Copilot retained blob turn.";
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "config.json", Json{{"model",model},{"workspace",workspace.string()},{"answer",answer},{"retry_answer",retry_answer}}.dump()));
	std::string png, gif;
	UAM_ASSERT(uam::base64::Decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==", png));
	UAM_ASSERT(uam::base64::Decode("R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7", gif));
	UAM_ASSERT(!png.empty() && !gif.empty() && png != gif && uam::io::WriteBinaryFile(workspace / "current.png", png));
	uam::AppState app; app.data_root = temp.root / "controller"; app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.settings.central_provider_configuration.enabled = app.settings.central_provider_configuration.uam_control_enabled = false;
	app.settings.git_enabled = app.settings.git_auto_worktree = app.settings.svn_enabled = app.settings.computer_use_globally_enabled = false;
	ExecutionHost host; host.id = "private-remote-copilot-owned"; host.label = "Private Copilot host"; host.transport = "ssh";
	host.ssh_alias = "private-copilot-owned-host"; host.platform = "macos"; host.runner_status = "ready"; host.runner_version = runner_version; host.runner_protocol_version = uam::remote::kRunnerProtocolVersion;
	app.settings.execution_hosts = {host}; uam::execution_hosts::Normalize(app.settings.execution_hosts);
	const auto* selected = uam::execution_hosts::Find(app.settings.execution_hosts, host.id);
	UAM_ASSERT(selected && selected->id != "local" && uam::execution_hosts::SameConnection(*selected,host)); host = *selected;
	ChatSession chat = ChatDomainService().CreateNewChat("", uam::provider_ids::kCopilotCli, false);
	chat.execution_host_id = host.id;
	if (folder_only)
	{
		ChatFolder folder; folder.id = ChatDomainService().NewFolderId(); folder.title = "Private same-host workspace"; folder.directory = workspace.string(); folder.execution_host_id = host.id;
		UAM_ASSERT(!folder.id.empty()); app.folders.push_back(folder); chat.folder_id = folder.id;
		UAM_ASSERT(chat.workspace_directory.empty() && uam::paths::ResolveWorkspaceRootPath(app,chat) == workspace);
	}
	else chat.workspace_directory = workspace.string();
	chat.model_id = model; chat.approval_mode = "default"; chat.command_safety_tier = "off";
	chat.memory_enabled = chat.uam_control_enabled = chat.computer_use_enabled = false;
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root,chat)); const auto initial = ChatRepository::LoadLocalChat(app.data_root,chat.id,true); UAM_ASSERT(initial); app.chats.push_back(*initial);
	ChatSession unrelated = ChatDomainService().CreateNewChat("",uam::provider_ids::kCopilotCli,false); unrelated.workspace_directory = workspace.string(); unrelated.messages = {{MessageRole::User,"Keep unrelated history."}};
	UAM_ASSERT(ChatRepository::SaveChat(app.data_root,unrelated)); app.chats.push_back(unrelated);
	const auto unrelated_path = AppPaths::UamChatFilePath(app.data_root,unrelated.id);
	const auto unrelated_bytes = uam_test::ReadFile(unrelated_path);
	ChatDomainService().SelectChatById(app,chat.id); app.composer_text = "Keep the unrelated live draft.";
	// Reuse the existing protocol3 original-proxy Stop/reap and complete worker drain helper.
	RemoteCodexOwnedImageCleanup cleanup{app,chat.id,temp.root / "release-1"};
	try
	{
	const auto live = [&]() -> ChatSession& { auto* value = ChatDomainService().FindChatById(app,chat.id); UAM_ASSERT(value); return *value; };
	const auto runtime = [&]() -> uam::AcpSessionState& { auto* value = uam::FindAcpSessionForChat(app,chat.id); UAM_ASSERT(value); return *value; };
	const auto wait = [&](const auto& predicate)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (std::chrono::steady_clock::now() < deadline) { cleanup.Tick(); if (predicate()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
		return false;
	};
	const auto sends = [&]()
	{
		std::vector<Json> result; std::istringstream stream(uam_test::ReadFile(temp.root / "stdin.jsonl"));
		for (std::string line; std::getline(stream,line);) { const auto value = Json::parse(line); if (value.value("method","") == "session.send") result.push_back(value); }
		return result;
	};
	MessageAttachment current; current.id = "ordinary-image"; current.name = "Current remote PNG"; current.kind = "image"; current.mime_type = "image/png"; current.path = "current.png";
	if (folder_only) current.path = (workspace / "current.png").string(); // Valid absolute native host file; no fabricated own workspace field.
	std::string error; UAM_ASSERT(uam::SendAcpPrompt(app,chat.id,"Read this ordinary remote image.",{}, {current},false,&error) && error.empty());
	UAM_ASSERT(wait([&]() { return fs::is_regular_file(temp.root / "input-1.json") && runtime().running && runtime().initialized && runtime().session_ready && runtime().processing &&
	    !live().native_session_id.empty() && live().native_session_id == runtime().session_id && live().remote_turn_reconnect_pending && live().remote_prompt_delivery_id.empty() &&
	    live().remote_prompt_delivery_payload.empty() && !runtime().pending_prompt_delivery && app.pending_chat_save_at_by_chat_id.empty() && app.pending_chat_save_tasks.empty(); }));
	const std::string native = runtime().session_id, token = runtime().remote_output_delivery_token;
	const auto process = runtime().child_pid;
	const auto launch = Json::parse(uam_test::ReadFile(temp.root / "launch.json")), setup = Json::parse(uam_test::ReadFile(temp.root / "setup.json")), ordinary = Json::parse(uam_test::ReadFile(temp.root / "input-1.json"));
	UAM_ASSERT_EQ(setup.at("native"),Json(native)); UAM_ASSERT_EQ(setup.at("request").at("params").at("model"),Json(model));
	UAM_ASSERT_EQ(launch.at("pid"),ordinary.at("pid")); UAM_ASSERT_EQ(launch.at("cwd"),Json(workspace.string())); UAM_ASSERT_EQ(launch.at("executable"),Json(copilot.string()));
	UAM_ASSERT_EQ(runtime().protocol_kind,std::string("copilot-sdk")); UAM_ASSERT_EQ(runtime().process_execution_host_id,host.id); UAM_ASSERT_EQ(runtime().remote_runner_protocol_version,3);
	UAM_ASSERT(process > 0 && !token.empty() && token == live().remote_process_control_token && runtime().current_model_id == model);
	std::string actual; UAM_ASSERT(uam::base64::Decode(ordinary.at("image").get<std::string>(),actual)); UAM_ASSERT_EQ(actual,png);
	UAM_ASSERT_EQ(sends(),std::vector<Json>{ordinary.at("request")}); UAM_ASSERT_EQ(ordinary.at("request").at("id"),Json(runtime().prompt_request_id));
	UAM_ASSERT_EQ(runtime().pending_request_methods.at(runtime().prompt_request_id),std::string("session.send"));
	const auto active_saved = ChatRepository::LoadLocalChat(app.data_root,chat.id,true);
	UAM_ASSERT(active_saved && !active_saved->remote_active_turn_id.empty() && !live().remote_active_turn_id.empty());
	// Ordinary remote Copilot launch does not promise a CLI-version cache entry.
	// Request the real exact-host probe; consume it within the existing turn-settlement wait.
	std::string version_error;
	UAM_ASSERT(ProviderCliCompatibilityService().StartProviderVersionCheck(app,
	    uam::provider_ids::kCopilotCli, true, host.id, &version_error) && version_error.empty());
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "release-1","Complete actual native ordinary turn"));
	UAM_ASSERT(wait([&]() { const auto saved = ChatRepository::LoadLocalChat(app.data_root,chat.id,true);
	    const auto checked_version = app.runtime_cli_versions_by_provider_id.find(CliProviderVersionStateKey(uam::provider_ids::kCopilotCli,host.id));
	    return fs::is_regular_file(temp.root / "completion-1.json") && checked_version != app.runtime_cli_versions_by_provider_id.end() && checked_version->second.checked && saved && saved->messages.size() == 2 && saved->messages.back().content == answer &&
	    !runtime().processing && runtime().session_ready && runtime().pending_request_methods.empty() && !saved->remote_turn_reconnect_pending && saved->remote_pending_requests.empty() && live().remote_pending_requests.empty() &&
	    app.pending_chat_save_at_by_chat_id.empty() && app.pending_chat_save_tasks.empty() && !app.runtime_cli_version_check_task.running && !app.runtime_cli_version_check_task.worker; }));
	const auto version = app.runtime_cli_versions_by_provider_id.find(CliProviderVersionStateKey(uam::provider_ids::kCopilotCli,host.id));
	UAM_ASSERT(version != app.runtime_cli_versions_by_provider_id.end() && version->second.checked && version->second.supported && version->second.check_error.empty() && uam::execution_hosts::SameConnection(version->second.execution_host,host));
	UAM_ASSERT_EQ(version->second.installed_version,std::string("1.0.98")); UAM_ASSERT(fs::is_regular_file(temp.root / "version.json")); UAM_ASSERT(runtime().last_error.empty());
	const auto first_saved = ChatRepository::LoadLocalChat(app.data_root,chat.id,true);
	UAM_ASSERT(first_saved && first_saved->native_session_id == native && first_saved->execution_host_id == host.id && first_saved->provider_id == uam::provider_ids::kCopilotCli);
	UAM_ASSERT_EQ(first_saved->messages.front().attachments,std::vector<MessageAttachment>{current}); UAM_ASSERT_EQ(first_saved->remote_process_control_token,token);
	// Actual native output frames prove acceptance precedes message and terminal idle.
	const auto completion = Json::parse(uam_test::ReadFile(temp.root / "completion-1.json"));
	UAM_ASSERT_EQ(completion.at("pid"),launch.at("pid")); UAM_ASSERT_EQ(completion.at("input_id"),ordinary.at("request").at("id"));
	const auto emitted = uam_test::ReadFile(temp.root / "stdout-raw.bin");
	UAM_ASSERT(completion.at("emitted_bytes").is_number_unsigned());
	const auto emitted_prefix_bytes = completion.at("emitted_bytes").get<std::uint64_t>();
	UAM_ASSERT(emitted_prefix_bytes > 0 && emitted_prefix_bytes <= 16*1024*1024 && emitted_prefix_bytes <= emitted.size());
	// Later model replies may append after idle; only the marker's fsynced prefix is immutable.
	std::string_view frames(emitted.data(),static_cast<std::size_t>(emitted_prefix_bytes));
	std::size_t position = 0, acceptance = std::string::npos, assistant = std::string::npos, idle = std::string::npos;
	while (!frames.empty())
	{
		const auto frame = uam::structured_framing::Read(frames); UAM_ASSERT_EQ(frame.status,uam::structured_framing::Status::Complete);
		const auto value = Json::parse(frames.substr(frame.body_offset,frame.body_bytes));
		if (value.contains("id") && value.at("id") == ordinary.at("request").at("id"))
		{
			UAM_ASSERT(acceptance == std::string::npos && value.at("result").at("messageId").is_string() && !value.at("result").at("messageId").get<std::string>().empty()); acceptance = position;
		}
		if (value.value("method","") == "session.event")
		{
			UAM_ASSERT_EQ(value.at("params").at("sessionId"),Json(native)); const auto& event = value.at("params").at("event");
			if (event.at("type") == "assistant.message") { UAM_ASSERT(assistant == std::string::npos); UAM_ASSERT_EQ(event.at("data").at("content"),Json(answer)); assistant = position; }
			if (event.at("type") == "session.idle") { UAM_ASSERT(idle == std::string::npos && event.at("data").is_object()); idle = position; }
		}
		frames.remove_prefix(frame.consumed_bytes); ++position;
	}
	UAM_ASSERT(acceptance != std::string::npos && assistant != std::string::npos && idle != std::string::npos && acceptance < assistant && assistant < idle);
	UAM_ASSERT(!runtime().processing && runtime().last_settled_turn_serial == runtime().turn_serial && runtime().last_turn_outcome == "completed");
	// Intended completed-turn identity regression: no fixture clears durable authority.
	UAM_ASSERT(first_saved->remote_active_turn_id.empty() && live().remote_active_turn_id.empty());
	// Typed DATA publication creates the retained original, not a Copilot exporter or execution receipt.
	const auto revision = ChatRepository::ChatStorageRevision(app.data_root,chat.id); UAM_ASSERT(revision && *revision != "missing");
	const auto prefix = live().messages; const auto incarnation = live().metadata_incarnation; ChatSession candidate = live();
	Message row; row.role = MessageRole::User; row.content = "Retry this retained original."; row.created_at = uam::time::TimestampNow(); row.acp_prompt_not_sent = true;
	MessageAttachment pending; pending.id = "retained-gif"; pending.name = "Retained remote GIF"; pending.kind = "image"; pending.source = AttachmentSource::NativeInlinePending;
	pending.native_part_key = "v1:data-fixture/remote-copilot/user/image/0"; pending.native_source_field = "data-fixture/user/attachments/0"; pending.origin_execution_host_id = host.id;
	std::string media_error; pending.native_inline_payload = uam::native_assets::MakeInlinePayload(uam::base64::Encode(gif),"image/gif",std::make_shared<uam::native_assets::PayloadBudget>(),media_error);
	UAM_ASSERT(pending.native_inline_payload && media_error.empty()); pending.mime_type = pending.native_inline_payload->MimeType(); pending.size_bytes = pending.native_inline_payload->DecodedSize(); pending.content_digest = pending.native_inline_payload->Digest();
	row.attachments = {pending}; candidate.messages.push_back(std::move(row));
	auto worker = std::async(std::launch::async,[root = app.data_root,candidate = std::move(candidate),revision = *revision,folder_only]() mutable
	{
		if (folder_only)
		{
			// DATA store fixture: the media-chat parser requires an explicit directory,
			// so publish through the public asset batch contract without filling the chat's folder-only workspace.
			std::string failure;
			if (ChatRepository::ChatStorageRevision(root,candidate.id) != revision) return std::make_pair(std::optional<ChatSession>{},std::string("Fixture primary changed before original publication"));
			auto& image = candidate.messages.back().attachments.front();
			auto batch = uam::native_assets::PreparedBatch::Prepare(root,candidate.id,{image.native_inline_payload},failure);
			if (!batch || batch->Assets().size() != 1) return std::make_pair(std::optional<ChatSession>{},failure);
			const auto reference = batch->Assets().front(); image.source = AttachmentSource::OwnedNativeAsset; image.native_inline_payload.reset();
			image.native_asset_key = reference.key; image.mime_type = reference.mime_type; image.content_digest = reference.digest; image.size_bytes = reference.size_bytes;
			if (!batch->MarkPublicationAttempted(failure) || !ChatRepository::SaveChat(root,candidate) || !batch->MarkPublished(failure))
				return std::make_pair(std::optional<ChatSession>{},failure.empty() ? std::string("Fixture original publication was not acknowledged") : failure);
			return std::make_pair(std::optional<ChatSession>{std::move(candidate)},failure);
		}
		std::string failure; auto prepared = uam::PreparedNativeMediaChatSave::Prepare(root,std::move(candidate),revision,failure);
		if (!prepared || !prepared->Publish(failure) || !prepared->AcknowledgeOriginals(failure)) return std::make_pair(std::optional<ChatSession>{},failure);
		return std::make_pair(prepared->TakeCommittedChat(),failure);
	});
	auto published = worker.get(); UAM_ASSERT(published.first && published.second.empty() && published.first->metadata_incarnation == incarnation); live() = std::move(*published.first);
	const auto stored = ChatRepository::LoadLocalChat(app.data_root,chat.id,true); UAM_ASSERT(stored && stored->messages == live().messages && stored->native_session_id == native);
	UAM_ASSERT_EQ(stored->messages.size(),prefix.size()+1); UAM_ASSERT(std::equal(prefix.begin(),prefix.end(),stored->messages.begin()));
	const int index = static_cast<int>(stored->messages.size())-1; const auto authored = stored->messages.back();
	UAM_ASSERT(authored.role == MessageRole::User && authored.acp_prompt_not_sent && authored.attachments.size() == 1 && authored.attachments.front().source == AttachmentSource::OwnedNativeAsset);
	if (folder_only)
	{
		UAM_ASSERT(live().workspace_directory.empty() && stored->workspace_directory.empty());
		UAM_ASSERT_EQ(live().folder_id,chat.folder_id); UAM_ASSERT_EQ(stored->folder_id,chat.folder_id);
		const auto* folder = ChatDomainService().FindFolderById(app,chat.folder_id);
		UAM_ASSERT(folder && folder->execution_host_id == host.id && folder->directory == workspace.string());
	}
	const auto original = uam::native_assets::ReadOwned(app.data_root,chat.id,authored.attachments.front().native_asset_key); UAM_ASSERT(original.ok && original.bytes == gif && original.bytes != png);
	const auto actual_recoverable = uam::RecoverableUnsentAcpMessageIndices(app,live());
	if (actual_recoverable != std::vector<int>{index})
	{
		// Failure-only fixed labels, booleans, counts and row indices. Never print
		// native IDs, process tokens, paths, commands, image bytes or authored prose.
		const auto& owner = live(); const auto& session = runtime();
		const auto known = uam::KnownUnsentAcpMessageIndices(app,owner);
		const auto bounded = [](const std::vector<int>& values)
		{ return std::vector<int>(values.begin(),values.begin()+static_cast<std::ptrdiff_t>(std::min<std::size_t>(values.size(),16))); };
		const auto chat_gates = [](const ChatSession& value)
		{
			return Json{{"loaded",value.messages_loaded},{"imported",value.imported_read_only},{"queue_error",value.acp_queue_storage_error},
			    {"queued",value.acp_queued_prompts.size()},{"dispatched",value.acp_dispatched_queued_prompt_count},
			    {"turn_reconnect",value.remote_turn_reconnect_pending},{"restart",value.remote_restart_pending},
			    {"stop_cleanup",value.remote_stop_cleanup_pending},{"source_exit",value.remote_source_exit_pending},
			    {"active_turn",!value.remote_active_turn_id.empty()},{"delivery_session",!value.remote_prompt_delivery_session_id.empty()},
			    {"delivery_id",!value.remote_prompt_delivery_id.empty()},{"delivery_payload",!value.remote_prompt_delivery_payload.empty()},
			    {"pending_requests",value.remote_pending_requests.size()},{"interaction_responses",value.remote_interaction_responses.size()},
			    {"side_cleanup",value.side_cleanup_requested},{"remote_user_index",value.remote_turn_user_message_index},
			    {"rows",value.messages.size()},{"native_present",!value.native_session_id.empty()}};
		};
		const auto disk = ChatRepository::LoadLocalChat(app.data_root,chat.id,true);
		const auto& row = owner.messages.at(static_cast<std::size_t>(index));
		std::cerr << "PRIVATE_REMOTE_COPILOT_RETRY_ELIGIBILITY_DIAGNOSTIC " << Json{
		    {"expected_index",index},{"known_count",known.size()},{"known_indices",bounded(known)},
		    {"recoverable_count",actual_recoverable.size()},{"recoverable_indices",bounded(actual_recoverable)},
		    {"chat",chat_gates(owner)},{"saved_present",static_cast<bool>(disk)},{"saved",disk ? chat_gates(*disk) : Json::object()},
		    {"row",{{"user",row.role == MessageRole::User},{"not_sent",row.acp_prompt_not_sent},{"discarded",row.acp_prompt_discarded},
		        {"provider_empty",row.provider.empty()},{"provider_matches",row.provider == owner.provider_id},
		        {"tools",row.tool_calls.size()},{"thoughts",row.thoughts.size()},{"plan_entries",row.plan_entries.size()},
		        {"plan_summary",!row.plan_summary.empty()},{"blocks",row.blocks.size()},{"timestamp_present",!row.created_at.empty()}}},
		    {"runtime",{{"running",session.running},{"initialized",session.initialized},{"ready",session.session_ready},
		        {"native_matches",session.session_id == owner.native_session_id},{"provider_matches",session.provider_id == owner.provider_id},
		        {"processing",session.processing},{"compaction",static_cast<bool>(session.pending_compaction)},
		        {"waiting_permission",session.waiting_for_permission},{"waiting_input",session.waiting_for_user_input},
		        {"queued_prompt",!session.queued_prompt.empty()},{"queued_users",session.queued_user_prompts.size()},
		        {"background",ProviderRuntimeRegistry::ResolveById(session.provider_id).AcpBackgroundTaskCount(session)},
		        {"cancel",session.cancel_requested || session.cancel_request_id != 0},{"reconnect",session.reconnect_pending},
		        {"local_stop",session.local_stop_pending},{"remote_stop",session.remote_stop_pending},{"unconfirmed_stop",session.remote_stop_unconfirmed},
		        {"prompt_delivery",static_cast<bool>(session.pending_prompt_delivery)},{"launch",static_cast<bool>(session.pending_local_launch)},
		        {"local_receipt",static_cast<bool>(session.pending_local_session_receipt)},{"agent_input",static_cast<bool>(session.pending_agent_input)},
		        {"queued_history",static_cast<bool>(session.pending_queued_history)},{"recover_turn",session.recovering_remote_turn},
		        {"recover_process",session.recovering_remote_process},{"model_discovery",session.model_discovery_only},
		        {"goal_review",session.goal_review_scheduled},{"checkpoint",session.turn_checkpoint_commit_pending},
		        {"managed_run",!session.managed_agent_run_id.empty()},{"steers",session.pending_steer_requests.size()},
		        {"request_methods",session.pending_request_methods.size()},{"prompt_request",session.prompt_request_id != 0},
		        {"setup_request",session.initialize_request_id != 0 || session.session_setup_request_id != 0 || session.startup_model_request_id != 0},
		        {"pending_runtime_request",uam::AcpSessionHasPendingRuntimeRequest(session)},
		        {"reasoning_change_request",session.reasoning_change_request_id != 0},
		        {"config_option_change_request",session.config_option_change_request_id != 0},
		        {"mode_change_request",session.mode_change_request_id != 0},
		        {"model_change_request",session.model_change_request_id != 0},
		        {"awaiting_model_config_options",session.awaiting_model_config_options},
		        {"turn_first_user_index",session.turn_first_user_message_index},{"turn_user_index",session.turn_user_message_index}}},
		    {"stop_in_progress",uam::AcpStopInProgress(app,owner.id)},{"worktree_operation",app.worktree_operation_chat_ids.contains(owner.id)},
		    {"scheduled_saves",app.pending_chat_save_at_by_chat_id.size()},{"save_tasks",app.pending_chat_save_tasks.size()}}.dump() << std::endl;
	}
	UAM_ASSERT_EQ(uam::RecoverableUnsentAcpMessageIndices(app,live()),std::vector<int>{index});
	UAM_ASSERT_EQ(runtime().child_pid,process); UAM_ASSERT_EQ(runtime().remote_output_delivery_token,token); UAM_ASSERT_EQ(runtime().session_id,native);
	const auto primary = uam_test::ReadFile(AppPaths::UamChatFilePath(app.data_root,chat.id)), stdin_before = uam_test::ReadFile(temp.root / "stdin.jsonl");
	const auto raw_before = uam_test::ReadFile(temp.root / "stdin-raw.bin"); UAM_ASSERT(!raw_before.empty());
	std::string retry_error; const bool accepted = uam::RetryFailedAcpMessage(app,chat.id,index,&retry_error);
	if (!accepted)
	{
		// Only this specific Capture refusal AFTER real ordinary/native/store/eligible-row prerequisites is intended RED.
		UAM_ASSERT_EQ(retry_error,std::string("Saved image Retry needs this chat's existing ready SSH session. Reopen it or attach the files again."));
		UAM_ASSERT_EQ(live().messages[static_cast<std::size_t>(index)],authored); UAM_ASSERT_EQ(uam_test::ReadFile(AppPaths::UamChatFilePath(app.data_root,chat.id)),primary);
		UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "stdin.jsonl"),stdin_before); UAM_ASSERT_EQ(sends().size(),std::size_t{1});
		UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "stdin-raw.bin"),raw_before);
		UAM_ASSERT_EQ(uam::native_assets::ReadOwned(app.data_root,chat.id,authored.attachments.front().native_asset_key).bytes,gif);
		UAM_ASSERT_EQ(app.composer_text,std::string("Keep the unrelated live draft.")); UAM_ASSERT_EQ(uam_test::ReadFile(unrelated_path),unrelated_bytes);
		cleanup.StopAndDrain(); UAM_ASSERT(cleanup.Quiet());
		UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "stdin-raw.bin"),raw_before);
	}
	UAM_ASSERT(accepted && retry_error.empty());
	UAM_ASSERT(wait([&]() { return fs::is_regular_file(temp.root / "input-2.json") && runtime().processing && !runtime().pending_prompt_delivery && live().remote_prompt_delivery_id.empty() && live().remote_prompt_delivery_payload.empty() && app.pending_chat_save_at_by_chat_id.empty() && app.pending_chat_save_tasks.empty(); }));
	const auto retried = Json::parse(uam_test::ReadFile(temp.root / "input-2.json")); UAM_ASSERT_EQ(retried.at("pid"),launch.at("pid"));
	UAM_ASSERT(uam::base64::Decode(retried.at("image").get<std::string>(),actual)); UAM_ASSERT_EQ(actual,gif);
	const auto& request = retried.at("request"); UAM_ASSERT_EQ(request.at("params").at("sessionId"),Json(native)); UAM_ASSERT_EQ(request.at("params").at("attachments").size(),std::size_t{1});
	UAM_ASSERT_EQ(request.at("params").at("attachments")[0].at("type"),Json("blob")); UAM_ASSERT_EQ(request.at("params").at("attachments")[0].at("displayName"),Json(authored.attachments[0].name));
	UAM_ASSERT(request.dump().find(authored.attachments[0].native_asset_key) == std::string::npos && request.dump().find("file://") == std::string::npos);
	UAM_ASSERT(request.at("params").at("prompt").get<std::string>().find(authored.attachments[0].name) != std::string::npos);
	std::set<std::string> fresh_queries; std::istringstream retry_traffic(uam_test::ReadFile(temp.root / "stdin.jsonl").substr(stdin_before.size()));
	for (std::string line; std::getline(retry_traffic,line);) fresh_queries.insert(Json::parse(line).value("method",""));
	for (const auto* query : {"status.get","models.list","session.model.getCurrent"}) UAM_ASSERT(fresh_queries.contains(query));
	UAM_ASSERT_EQ(sends().size(),std::size_t{2}); UAM_ASSERT_EQ(request.at("id"),Json(runtime().prompt_request_id)); UAM_ASSERT_EQ(runtime().pending_request_methods.at(runtime().prompt_request_id),std::string("session.send"));
	UAM_ASSERT_EQ(runtime().child_pid,process); UAM_ASSERT_EQ(runtime().remote_output_delivery_token,token); UAM_ASSERT_EQ(runtime().session_id,native); UAM_ASSERT_EQ(runtime().current_model_id,model);
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "release-2","Complete actual native retained turn"));
	UAM_ASSERT(wait([&]() { const auto saved = ChatRepository::LoadLocalChat(app.data_root,chat.id,true); return saved && saved->messages.size() == 4 && saved->messages.back().content == retry_answer && !saved->messages[static_cast<std::size_t>(index)].acp_prompt_not_sent &&
	    saved->messages[static_cast<std::size_t>(index)].attachments == authored.attachments && !runtime().processing && runtime().session_ready && runtime().pending_request_methods.empty() && saved->remote_pending_requests.empty() && live().remote_pending_requests.empty() &&
	    !saved->remote_turn_reconnect_pending && app.pending_chat_save_at_by_chat_id.empty() && app.pending_chat_save_tasks.empty(); }));
	UAM_ASSERT(runtime().last_error.empty() && !runtime().saved_image_retry && !runtime().claude_input_preparation && !runtime().pending_prompt_delivery);
	UAM_ASSERT_EQ(uam::native_assets::ReadOwned(app.data_root,chat.id,authored.attachments[0].native_asset_key).bytes,gif);
	UAM_ASSERT_EQ(uam_test::ReadFile(workspace / "current.png"),png); UAM_ASSERT_EQ(uam_test::ReadFile(unrelated_path),unrelated_bytes); UAM_ASSERT_EQ(app.composer_text,std::string("Keep the unrelated live draft."));
	if (changed_folder)
	{
		UAM_ASSERT(folder_only);
		Message next = authored; next.content = "Refuse delivery after the actual folder workspace changes."; next.created_at = uam::time::TimestampNow(); next.acp_prompt_not_sent = true;
		live().messages.push_back(next); UAM_ASSERT(ChatRepository::SaveChat(app.data_root,live()));
		const int next_index = static_cast<int>(live().messages.size())-1; const auto expected_messages = live().messages;
		UAM_ASSERT_EQ(uam::RecoverableUnsentAcpMessageIndices(app,live()),std::vector<int>{next_index});
		UAM_ASSERT(uam::io::WriteTextFile(temp.root / "hold-folder-proof","Hold actual fresh native support replies"));
		const auto raw_before_control = uam_test::ReadFile(temp.root / "stdin-raw.bin");
		std::string refusal; UAM_ASSERT(uam::RetryFailedAcpMessage(app,chat.id,next_index,&refusal) && refusal.empty());
		UAM_ASSERT(wait([&]() { return fs::is_regular_file(temp.root / "held-folder-proof.json"); }));
		const auto held = Json::parse(uam_test::ReadFile(temp.root / "held-folder-proof.json")); UAM_ASSERT_EQ(held.at("pid"),launch.at("pid"));
		UAM_ASSERT(runtime().saved_image_retry && !runtime().claude_input_preparation && !runtime().pending_prompt_delivery && sends().size() == 2);
		const auto& evidence = runtime().copilot_native_state.at("savedImageRetry");
		UAM_ASSERT(evidence.is_object() && evidence.at("phase") == "querying" && evidence.at("requests").size() == 3);
		const auto changed_workspace = fs::weakly_canonical(temp.root) / "changed-workspace"; fs::create_directories(changed_workspace);
		auto* folder = ChatDomainService().FindFolderById(app,chat.folder_id); UAM_ASSERT(folder && folder->execution_host_id == host.id); folder->directory = changed_workspace.string();
		UAM_ASSERT(live().workspace_directory.empty() && uam::paths::ResolveWorkspaceRootPath(app,live()) == changed_workspace);
		UAM_ASSERT(uam::io::WriteTextFile(temp.root / "release-folder-proof","Release actual stale native replies"));
		UAM_ASSERT(wait([&]()
		{
			if (!fs::is_regular_file(temp.root / "folder-queries-drained.json")) return false;
			const auto drained = Json::parse(uam_test::ReadFile(temp.root / "folder-queries-drained.json"));
			UAM_ASSERT(drained.at("emitted_bytes").is_number_unsigned());
			const auto emitted_prefix = drained.at("emitted_bytes").get<std::uint64_t>();
			UAM_ASSERT(emitted_prefix > 0 && emitted_prefix <= 16*1024*1024);
			const auto saved = ChatRepository::LoadLocalChat(app.data_root,chat.id,true);
			// Real cursor publication and ACK delivery, not absence before the final marker arrives.
			return saved && live().remote_delivered_stdout_cursor >= emitted_prefix && saved->remote_delivered_stdout_cursor >= emitted_prefix &&
			    runtime().pending_remote_output_ack_line.empty() && runtime().pending_remote_source_exit_ack_line.empty() &&
			    live().remote_pending_requests.empty() && saved->remote_pending_requests.empty() &&
			    !runtime().processing && !runtime().saved_image_retry && !runtime().claude_input_preparation && !runtime().pending_prompt_delivery &&
			    runtime().pending_request_methods.empty() && app.pending_chat_save_at_by_chat_id.empty() && app.pending_chat_save_tasks.empty();
		}));
		UAM_ASSERT(!runtime().last_error.empty() && sends().size() == 2 && !fs::exists(temp.root / "input-3.json"));
		const auto retained = ChatRepository::LoadLocalChat(app.data_root,chat.id,true); UAM_ASSERT(retained && retained->messages.size() == expected_messages.size());
		for (std::size_t i=0;i<expected_messages.size();++i)
			for (const auto* actual_row : std::array<const Message*,2>{&retained->messages[i],&live().messages[i]})
			{
				UAM_ASSERT_EQ(actual_row->role,expected_messages[i].role); UAM_ASSERT_EQ(actual_row->content,expected_messages[i].content);
				UAM_ASSERT_EQ(actual_row->created_at,expected_messages[i].created_at); UAM_ASSERT_EQ(actual_row->attachments,expected_messages[i].attachments);
			}
		UAM_ASSERT(retained->messages.back().acp_prompt_not_sent && live().messages.back().acp_prompt_not_sent && retained->workspace_directory.empty());
		UAM_ASSERT_EQ(retained->native_session_id,native); UAM_ASSERT_EQ(runtime().session_id,native); UAM_ASSERT_EQ(runtime().child_pid,process); UAM_ASSERT_EQ(runtime().remote_output_delivery_token,token);
		UAM_ASSERT_EQ(folder->directory,changed_workspace.string()); UAM_ASSERT_EQ(uam::native_assets::ReadOwned(app.data_root,chat.id,authored.attachments[0].native_asset_key).bytes,gif);
		const auto raw = uam_test::ReadFile(temp.root / "stdin-raw.bin"); UAM_ASSERT(raw.starts_with(raw_before_control)); std::string_view suffix(raw.data()+raw_before_control.size(),raw.size()-raw_before_control.size());
		std::multiset<std::string> observed;
		while (!suffix.empty())
		{
			const auto frame = uam::structured_framing::Read(suffix); UAM_ASSERT_EQ(frame.status,uam::structured_framing::Status::Complete);
			const auto query = Json::parse(suffix.substr(frame.body_offset,frame.body_bytes)); observed.insert(query.value("method","")); suffix.remove_prefix(frame.consumed_bytes);
		}
		UAM_ASSERT_EQ(observed,(std::multiset<std::string>{"status.get","models.list","session.model.getCurrent"}));
		UAM_ASSERT_EQ(uam_test::ReadFile(unrelated_path),unrelated_bytes); UAM_ASSERT_EQ(app.composer_text,std::string("Keep the unrelated live draft."));
	}
	const auto raw_before_stop = uam_test::ReadFile(temp.root / "stdin-raw.bin");
	const auto stdin_before_stop = uam_test::ReadFile(temp.root / "stdin.jsonl"); cleanup.StopAndDrain(); UAM_ASSERT(cleanup.Quiet());
	UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "stdin-raw.bin"),raw_before_stop);
	UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "stdin.jsonl"),stdin_before_stop); UAM_ASSERT_EQ(sends().size(),std::size_t{2});
	}
	catch (const uam_test::TestFailure& failure)
	{
		// Harness failures contain source file/line/expression, never compared values.
		// Print before cleanup's destructor can abort and hide the original assertion.
		std::cerr << "PRIVATE_REMOTE_COPILOT_ORIGINAL_TEST_FAILURE " << std::string_view(failure.what()).substr(0,4096) << std::endl;
		throw;
	}
	catch (const std::exception&)
	{
		// Arbitrary exception text can contain payloads; report only the fixed category.
		std::cerr << "PRIVATE_REMOTE_COPILOT_ORIGINAL_NON_ASSERTION_EXCEPTION" << std::endl;
		throw;
	}
}
UAM_TEST(CopilotRemoteSavedOwnedIndexedRetryDeliversBlobThroughActualRunner)
{
	CheckCopilotRemoteSavedOwnedIndexedRetry(false,false);
}
UAM_TEST(CopilotRemoteFolderOnlyOwnedRetryUsesSameHostWorkspace)
{
	CheckCopilotRemoteSavedOwnedIndexedRetry(true,false);
}
UAM_TEST(CopilotRemoteFolderWorkspaceDriftRefusesOwnedRetryAfterNativeQueries)
{
	CheckCopilotRemoteSavedOwnedIndexedRetry(true,true);
}
#endif
