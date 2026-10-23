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

UAM_TEST(LocalChatBundleExportOmitsPrivateProviderCompanionAuthority)
{
	TempDir temp("uam-bundle-provider-authority");
	ChatSession chat;
	chat.id = "native-companion";
	chat.provider_id = "opencode-cli";
	chat.execution_host_id = "remote-host";
	chat.remote_process_exists = true;
	chat.remote_process_control_token = "private-runner-control-token";
	chat.remote_opencode_native_port = 41234;
	chat.remote_opencode_native_token = "private-native-companion-token";
	chat.messages.push_back(Message{MessageRole::User, "Retain the conversation"});
	const auto source = temp.root / "source";
	UAM_ASSERT(ChatRepository::SaveChat(source, chat));
	const auto before = ReadFile(AppPaths::UamChatFilePath(source, chat.id));
	UAM_ASSERT(uam::LocalChatBundleService::Export(source, temp.root / "bundle").ok);
	const auto exported = ReadFile(AppPaths::UamChatFilePath(temp.root / "bundle", chat.id));
	UAM_ASSERT(exported.find(chat.remote_process_control_token) == std::string::npos);
	UAM_ASSERT(exported.find(chat.remote_opencode_native_token) == std::string::npos);
	UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(source, chat.id)), before);
	UAM_ASSERT(uam::LocalChatBundleService::Import(temp.root / "target", temp.root / "bundle").ok);
	const auto loaded = ChatRepository::LoadLocalChat(temp.root / "target", chat.id);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT_EQ(loaded->messages, chat.messages);
	UAM_ASSERT(loaded->remote_opencode_native_token.empty() && loaded->remote_process_control_token.empty());
	UAM_ASSERT(!loaded->remote_process_exists);
}

UAM_TEST(LocalChatBundleExportRedactsRawAuthorityAfterPassiveNormalization)
{
	TempDir temp("uam-bundle-stale-native-authority");
	for (const bool read_only : {false, true})
	for (const bool receipt_only : {false, true})
	{
		ChatSession chat;
		chat.id = std::string(read_only ? "passive-native-token" : "local-native-token") + (receipt_only ? "-receipt-only" : "");
		chat.imported_read_only = read_only;
		chat.messages.push_back(Message{MessageRole::Assistant, "Retained history"});
		const auto source = temp.root / "source";
		UAM_ASSERT(ChatRepository::SaveChat(source, chat));
		const auto file = AppPaths::UamChatFilePath(source, chat.id);
		auto raw = nlohmann::json::parse(ReadFile(file));
		raw["remote_opencode_native_token"] = receipt_only ? "" : "stale-private-native-token";
		raw["remote_opencode_native_port"] = 41234;
		raw["remote_process_control_token"] = receipt_only ? "" : "stale-private-runner-token";
		raw["remote_manual_compaction_boundary_json"] = nlohmann::json{{"kind", "claude_manual_compaction_terminal"}, {"processControlToken", "stale-private-maintenance-token"}}.dump();
		UAM_ASSERT(uam::io::WriteTextFile(file, raw.dump()));
		const auto loaded = ChatRepository::LoadLocalChat(source, chat.id);
		UAM_ASSERT(loaded.has_value() && loaded->remote_opencode_native_token.empty());
		const auto before = ReadFile(file);
		const auto bundle = temp.root / ("bundle-" + chat.id);
		UAM_ASSERT(uam::LocalChatBundleService::Export(source, bundle).ok);
		const auto exported = ReadFile(AppPaths::UamChatFilePath(bundle, chat.id));
		UAM_ASSERT(exported.find("stale-private-native-token") == std::string::npos);
		UAM_ASSERT(exported.find("stale-private-runner-token") == std::string::npos);
		UAM_ASSERT(exported.find("stale-private-maintenance-token") == std::string::npos);
		UAM_ASSERT(nlohmann::json::parse(exported).value("remote_manual_compaction_boundary_json", "").empty());
		UAM_ASSERT_EQ(ReadFile(file), before);
	}
}

// Native originals are portable bytes, not merely owner-bound JSON references.
// Keep these public Export/Import contracts independent of bundle filenames/version.
#include "common/chat/native_attachment_assets.h"
#include "common/chat/native_media_chat_save.h"

#if defined(__APPLE__) || defined(_WIN32)
namespace
{
	namespace bundle_assets = uam::native_assets;

	std::string BundlePngBytes()
	{
		std::string bytes;
		UAM_ASSERT(uam::base64::Decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==", bytes));
		return bytes;
	}

	MessageAttachment BundlePendingOriginal(const std::string& bytes, const std::string& mime, const std::string& part)
	{
		MessageAttachment a;
		a.id = "attachment-" + part;
		a.name = "Original " + part;
		a.kind = mime == "image/png" ? "image" : "file";
		a.source = AttachmentSource::NativeInlinePending;
		a.origin_execution_host_id = "local";
		a.native_part_key = "v1:native-session/message/" + part;
		a.native_tool_call_id = "recorded-tool-" + part;
		a.native_source_field = "toolCalls/" + a.native_tool_call_id + "/result/0";
		std::string error;
		a.native_inline_payload = bundle_assets::MakeInlinePayload(uam::base64::Encode(bytes), mime,
		    std::make_shared<bundle_assets::PayloadBudget>(), error);
		UAM_ASSERT(a.native_inline_payload && error.empty());
		a.mime_type = a.native_inline_payload->MimeType();
		a.content_digest = a.native_inline_payload->Digest();
		a.size_bytes = a.native_inline_payload->DecodedSize();
		return a;
	}

	ChatSession SaveBundleNativeOriginals(const fs::path& root, bool destination_originals = false)
	{
		fs::create_directories(root);
		ChatSession chat;
		chat.id = "portable-native-originals";
		chat.title = destination_originals ? "Existing destination history" : "Portable native history";
		chat.provider_id = "gemini-cli";
		chat.native_session_id = "provider-session-is-not-the-UAM-owner";
		chat.execution_host_id = "local";
		chat.workspace_directory = root.string();
		chat.created_at = "2026-10-16T12:00:00Z";
		chat.updated_at = chat.created_at;
		chat.messages.push_back(Message{MessageRole::Assistant, "Retained tool originals"});
		UAM_ASSERT(ChatRepository::SaveChat(root, chat));
		chat.messages.front().attachments.push_back(BundlePendingOriginal(
		    destination_originals ? std::string("existing destination original") : BundlePngBytes(),
		    destination_originals ? "application/x-destination-original" : "image/png", "message-part"));
		chat.messages.front().attachments.push_back(BundlePendingOriginal(
		    destination_originals ? std::string("existing queued original") : std::string("\0Q\0R\xff", 5),
		    "application/x-opaque-original", "queue-only-part"));
		const auto revision = ChatRepository::ChatStorageRevision(root, chat.id);
		UAM_ASSERT(revision);
		std::string error;
		auto prepared = uam::PreparedNativeMediaChatSave::Prepare(root, chat, *revision, error);
		UAM_ASSERT(prepared && error.empty());
		UAM_ASSERT(prepared->Publish(error));
		UAM_ASSERT(prepared->PrimaryAcknowledged());
		UAM_ASSERT(prepared->AcknowledgeOriginals(error));
		auto committed = prepared->TakeCommittedChat();
		UAM_ASSERT(committed);
		chat = std::move(*committed);
		// Persistence supports already-owned queued originals. Pending input admission
		// and queue delivery algorithms are outside this bundle contract.
		uam::AcpQueuedUserPromptState prompt;
		prompt.id = "archived-queued-input";
		prompt.text = "Retain the queue-only original without dispatching it";
		prompt.attachments.push_back(chat.messages.front().attachments.back());
		chat.messages.front().attachments.pop_back();
		chat.acp_queued_prompts.push_back(std::move(prompt));
		UAM_ASSERT(ChatRepository::SaveChat(root, chat));
		const auto loaded = ChatRepository::LoadLocalChat(root, chat.id);
		UAM_ASSERT(loaded && loaded->acp_queued_prompts.size() == 1);
		UAM_ASSERT_EQ(loaded->acp_queued_prompts.front().attachments, chat.acp_queued_prompts.front().attachments);
		return *loaded;
	}

	std::map<fs::path, std::string> BundleFileSnapshot(const fs::path& root)
	{
		std::map<fs::path, std::string> files;
		if (fs::exists(root))
			for (const auto& entry : fs::recursive_directory_iterator(root))
				if (fs::is_regular_file(entry.symlink_status()))
					files.emplace(entry.path().lexically_relative(root), ReadFile(entry.path()));
		return files;
	}

	fs::path FindBundleOriginalFile(const fs::path& root, const std::string& bytes)
	{
		std::vector<fs::path> matches;
		for (const auto& entry : fs::recursive_directory_iterator(root))
			if (fs::is_regular_file(entry.symlink_status()) && entry.file_size() == bytes.size() && ReadFile(entry.path()) == bytes)
				matches.push_back(entry.path());
		// The transfer must carry an actual opaque original file; no layout/name is
		// prescribed, and this fixture never grants production path-copy authority.
		UAM_ASSERT_EQ(matches.size(), std::size_t{1});
		return matches.front();
	}

	void AssertBundleOriginal(const fs::path& root, const std::string& owner,
	    const MessageAttachment& expected, const MessageAttachment& actual, const std::string& bytes)
	{
		UAM_ASSERT_EQ(actual.source, AttachmentSource::OwnedNativeAsset);
		UAM_ASSERT(bundle_assets::IsKeyForOwner(owner, actual.native_asset_key));
		UAM_ASSERT(!actual.native_inline_payload);
		auto normalized = actual;
		normalized.native_asset_key = expected.native_asset_key;
		UAM_ASSERT_EQ(normalized, expected); // Includes MIME, digest and every native source association.
		const auto original = bundle_assets::ReadOwned(root, owner, actual.native_asset_key);
		UAM_ASSERT(original.ok && original.error.empty());
		UAM_ASSERT_EQ(original.bytes, bytes);
	}

	void AssertBundleNativeImport(const fs::path& source_root, const fs::path& target, const ChatSession& source, const std::string& imported_id)
	{
		const auto loaded = ChatRepository::LoadLocalChat(target, imported_id);
		UAM_ASSERT(loaded && loaded->messages.size() == 1 && loaded->messages.front().attachments.size() == 1);
		UAM_ASSERT(loaded->imported_read_only);
		UAM_ASSERT(loaded->native_session_id.empty() && loaded->workspace_directory.empty());
		UAM_ASSERT(!loaded->uam_control_enabled && loaded->agent_run_id.empty());
		UAM_ASSERT(loaded->acp_queued_prompts.empty() && loaded->acp_dispatched_queued_prompt_count == 0);
		const auto& expected = source.messages.front().attachments.front();
		const auto& actual = loaded->messages.front().attachments.front();
		AssertBundleOriginal(target, imported_id, expected, actual, BundlePngBytes());
		auto normalized_messages = loaded->messages;
		normalized_messages.front().attachments.front().native_asset_key = expected.native_asset_key;
		UAM_ASSERT_EQ(normalized_messages, source.messages);
		// Read the archival association from canonical storage, since passive loads
		// intentionally suppress runnable queued input rather than resuming it.
		const auto raw = nlohmann::json::parse(ReadFile(AppPaths::UamChatFilePath(target, imported_id)));
		UAM_ASSERT_EQ(raw.at("acp_queued_prompts").size(), std::size_t{1});
		const auto& queued = raw.at("acp_queued_prompts").at(0);
		UAM_ASSERT_EQ(queued.at("id").get<std::string>(), source.acp_queued_prompts.front().id);
		UAM_ASSERT_EQ(queued.at("text").get<std::string>(), source.acp_queued_prompts.front().text);
		UAM_ASSERT_EQ(queued.at("attachments").size(), std::size_t{1});
		const auto& archived = queued.at("attachments").at(0);
		const auto& queue_expected = source.acp_queued_prompts.front().attachments.front();
		auto archive_expected = nlohmann::json::parse(ReadFile(AppPaths::UamChatFilePath(source_root, source.id)))
		    .at("acp_queued_prompts").at(0).at("attachments").at(0);
		archive_expected["nativeAssetKey"] = archived.at("nativeAssetKey");
		UAM_ASSERT_EQ(archived, archive_expected);
		auto queue_actual = queue_expected;
		queue_actual.native_asset_key = archived.at("nativeAssetKey").get<std::string>();
		AssertBundleOriginal(target, imported_id, queue_expected, queue_actual, std::string("\0Q\0R\xff", 5));
		if (imported_id != source.id)
		{
			UAM_ASSERT(!bundle_assets::IsKeyForOwner(source.id, actual.native_asset_key));
			UAM_ASSERT(!bundle_assets::IsKeyForOwner(source.id, queue_actual.native_asset_key));
			UAM_ASSERT(!bundle_assets::ReadOwned(target, source.id, actual.native_asset_key).ok);
			UAM_ASSERT(!bundle_assets::ReadOwned(target, source.id, queue_actual.native_asset_key).ok);
		}
	}
}

UAM_TEST(LocalChatBundleNativeOriginalsRoundTripIntoFreshRoot)
{
	TempDir temp("uam-bundle-native-original-roundtrip");
	const auto source = temp.root / "source", bundle = temp.root / "bundle", target = temp.root / "target";
	const auto chat = SaveBundleNativeOriginals(source);
	const auto before = BundleFileSnapshot(source);
	const auto exported = uam::LocalChatBundleService::Export(source, bundle);
	UAM_ASSERT(exported.ok && exported.errors.empty() && !exported.degraded);
	UAM_ASSERT_EQ(exported.exported_count, std::size_t{1});
	const auto imported = uam::LocalChatBundleService::Import(target, bundle);
	UAM_ASSERT(imported.ok && imported.errors.empty() && !imported.degraded);
	UAM_ASSERT_EQ(imported.imported_count, std::size_t{1});
	UAM_ASSERT_EQ(imported.renamed_count, std::size_t{0});
	UAM_ASSERT_EQ(imported.items.size(), std::size_t{1});
	UAM_ASSERT(imported.items.front().imported);
	UAM_ASSERT_EQ(imported.items.front().imported_id, chat.id);
	AssertBundleNativeImport(source, target, chat, imported.items.front().imported_id);
	UAM_ASSERT_EQ(BundleFileSnapshot(source), before);
}

UAM_TEST(LocalChatBundleNativeOriginalsRebindToExactCollisionId)
{
	TempDir temp("uam-bundle-native-original-collision");
	const auto source = temp.root / "source", bundle = temp.root / "bundle", target = temp.root / "target";
	const auto chat = SaveBundleNativeOriginals(source);
	const auto existing = SaveBundleNativeOriginals(target, true);
	auto reserved = existing;
	reserved.id += "--imported";
	reserved.messages.front().attachments.clear();
	reserved.acp_queued_prompts.clear();
	UAM_ASSERT(ChatRepository::SaveChat(target, reserved));
	const auto before_source = BundleFileSnapshot(source), before_target = BundleFileSnapshot(target);
	UAM_ASSERT(uam::LocalChatBundleService::Export(source, bundle).ok);
	const auto imported = uam::LocalChatBundleService::Import(target, bundle);
	UAM_ASSERT(imported.ok && imported.errors.empty() && !imported.degraded);
	UAM_ASSERT_EQ(imported.imported_count, std::size_t{1});
	UAM_ASSERT_EQ(imported.renamed_count, std::size_t{1});
	UAM_ASSERT_EQ(imported.items.size(), std::size_t{1});
	UAM_ASSERT(imported.items.front().imported && imported.items.front().renamed);
	UAM_ASSERT_EQ(imported.items.front().source_id, chat.id);
	UAM_ASSERT_EQ(imported.items.front().imported_id, chat.id + "--imported-2");
	AssertBundleNativeImport(source, target, chat, imported.items.front().imported_id);
	for (const auto& [file, bytes] : before_target) UAM_ASSERT_EQ(ReadFile(target / file), bytes);
	const auto& old = existing.messages.front().attachments.front();
	AssertBundleOriginal(target, existing.id, old, old, "existing destination original");
	const auto& old_queue = existing.acp_queued_prompts.front().attachments.front();
	AssertBundleOriginal(target, existing.id, old_queue, old_queue, "existing queued original");
	UAM_ASSERT_EQ(BundleFileSnapshot(source), before_source);
}

UAM_TEST(LocalChatBundleNativeOriginalExportRejectsMissingOrCorruptOwnedBytes)
{
	for (const bool missing : {false, true})
	{
		TempDir temp("uam-bundle-native-original-invalid-source");
		const auto source = temp.root / "source", bundle = temp.root / "bundle";
		const auto chat = SaveBundleNativeOriginals(source);
		const auto original = FindBundleOriginalFile(source, std::string("\0Q\0R\xff", 5));
		if (missing) UAM_ASSERT(fs::remove(original));
		else UAM_ASSERT(uam::io::WriteTextFile(original, std::string("\0Q\0R\xfe", 5))); // Same size, wrong digest.
		const auto before = BundleFileSnapshot(source);
		const auto exported = uam::LocalChatBundleService::Export(source, bundle);
		UAM_ASSERT(!exported.ok && !exported.errors.empty());
		UAM_ASSERT_EQ(exported.exported_count, std::size_t{0});
		UAM_ASSERT(!fs::exists(bundle / "manifest.json"));
		UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(bundle, chat.id)));
		UAM_ASSERT_EQ(BundleFileSnapshot(source), before);
	}
}

UAM_TEST(LocalChatBundleNativeOriginalImportRejectsMissingOrCorruptBeforePublication)
{
	for (const bool missing : {false, true})
	{
		TempDir temp("uam-bundle-native-original-invalid-bundle");
		const auto source = temp.root / "source", bundle = temp.root / "bundle", target = temp.root / "target";
		const auto chat = SaveBundleNativeOriginals(source);
		auto earlier = chat;
		earlier.id = "aaa-valid-transcript-before-invalid-original";
		earlier.messages.front().attachments.clear();
		earlier.acp_queued_prompts.clear();
		UAM_ASSERT(ChatRepository::SaveChat(source, earlier));
		const auto existing = SaveBundleNativeOriginals(target, true);
		UAM_ASSERT(uam::LocalChatBundleService::Export(source, bundle).ok);
		const auto original = FindBundleOriginalFile(bundle, std::string("\0Q\0R\xff", 5));
		if (missing) UAM_ASSERT(fs::remove(original));
		else UAM_ASSERT(uam::io::WriteTextFile(original, std::string("\0Q\0R\xfe", 5)));
		const auto before_source = BundleFileSnapshot(source), before_target = BundleFileSnapshot(target);
		const auto imported = uam::LocalChatBundleService::Import(target, bundle);
		UAM_ASSERT(!imported.ok && !imported.errors.empty());
		UAM_ASSERT_EQ(imported.imported_count, std::size_t{0});
		for (const auto& item : imported.items) UAM_ASSERT(!item.imported);
		UAM_ASSERT_EQ(BundleFileSnapshot(target), before_target);
		UAM_ASSERT_EQ(BundleFileSnapshot(source), before_source);
		const auto& old = existing.messages.front().attachments.front();
		AssertBundleOriginal(target, existing.id, old, old, "existing destination original");
	}
}

UAM_TEST(LocalChatBundleOpaqueNativeUrisRoundTripWithoutAcquiringAuthority)
{
	TempDir temp("uam-bundle-opaque-native-uri");
	const auto source = temp.root / "source", bundle = temp.root / "bundle", target = temp.root / "target";
	ChatSession chat;
	chat.id = "portable-opaque-uri";
	chat.provider_id = "gemini-cli";
	chat.native_session_id = "source-native-session";
	chat.workspace_directory = source.string();
	chat.messages.push_back(Message{MessageRole::Assistant, "Recorded provider reference"});
	for (const std::string uri : {"file:///never-authorized/provider-document.pdf", "provider://opaque/session/part?reference=only", "https://provider.invalid/original.pdf"})
	{
		MessageAttachment a;
		a.id = "uri-" + std::to_string(chat.messages.front().attachments.size());
		a.name = "Opaque original reference";
		a.kind = "file";
		a.mime_type = "application/pdf";
		a.source = AttachmentSource::ProviderUri;
		a.source_uri = uri;
		a.origin_execution_host_id = "recorded-remote-origin";
		a.native_part_key = "v1:native-session/message/" + a.id;
		a.native_tool_call_id = "recorded-uri-tool";
		a.native_source_field = "toolCalls/recorded-uri-tool/result/0";
		chat.messages.front().attachments.push_back(std::move(a));
	}
	UAM_ASSERT(ChatRepository::SaveChat(source, chat));
	const auto before = BundleFileSnapshot(source);
	const auto exported = uam::LocalChatBundleService::Export(source, bundle);
	UAM_ASSERT(exported.ok && exported.errors.empty());
	const auto imported = uam::LocalChatBundleService::Import(target, bundle);
	UAM_ASSERT(imported.ok && imported.errors.empty());
	UAM_ASSERT_EQ(imported.imported_count, std::size_t{1});
	const auto loaded = ChatRepository::LoadLocalChat(target, chat.id);
	UAM_ASSERT(loaded && loaded->imported_read_only);
	UAM_ASSERT_EQ(loaded->messages, chat.messages);
	UAM_ASSERT(loaded->native_session_id.empty() && loaded->workspace_directory.empty());
	UAM_ASSERT(!loaded->uam_control_enabled && loaded->acp_queued_prompts.empty());
	UAM_ASSERT(!fs::exists(target / std::string(bundle_assets::kStoreDirectory)));
	UAM_ASSERT_EQ(BundleFileSnapshot(source), before);
}
#endif

#if defined(__APPLE__) || defined(_WIN32)
UAM_TEST(LocalChatBundlePassiveNativeOriginalsSurviveReexportAndReimport)
{
	TempDir temp("uam-bundle-passive-native-reexport");
	const auto source = temp.root / "source", first = temp.root / "first", second = temp.root / "second";
	const auto first_bundle = temp.root / "first-bundle", second_bundle = temp.root / "second-bundle";
	const auto chat = SaveBundleNativeOriginals(source);
	const auto source_before = BundleFileSnapshot(source);
	UAM_ASSERT(uam::LocalChatBundleService::Export(source, first_bundle).ok);
	const auto initial = uam::LocalChatBundleService::Import(first, first_bundle);
	UAM_ASSERT(initial.ok && initial.imported_count == 1 && initial.items.size() == 1);
	AssertBundleNativeImport(source, first, chat, initial.items.front().imported_id);
	const auto passive_before = BundleFileSnapshot(first);
	const auto exported = uam::LocalChatBundleService::Export(first, second_bundle);
	UAM_ASSERT(exported.ok && exported.exported_count == 1 && exported.errors.empty());
	const auto imported = uam::LocalChatBundleService::Import(second, second_bundle);
	UAM_ASSERT(imported.ok && imported.imported_count == 1 && imported.items.size() == 1 && imported.errors.empty());
	AssertBundleNativeImport(source, second, chat, imported.items.front().imported_id);
	UAM_ASSERT_EQ(BundleFileSnapshot(first), passive_before);
	UAM_ASSERT_EQ(BundleFileSnapshot(source), source_before);
}
#endif

#if defined(__APPLE__) || defined(_WIN32)
UAM_TEST(LocalChatBundlePassiveNativeArchiveSurvivesLoadedAndUnloadedMetadataSaves)
{
	for (const bool include_messages : {true, false})
	{
		TempDir temp("uam-bundle-passive-native-metadata-save");
		const auto source = temp.root / "source", first = temp.root / "first", second = temp.root / "second";
		const auto first_bundle = temp.root / "first-bundle", second_bundle = temp.root / "second-bundle";
		const auto chat = SaveBundleNativeOriginals(source);
		const auto source_before = BundleFileSnapshot(source);
		UAM_ASSERT(uam::LocalChatBundleService::Export(source, first_bundle).ok);
		const auto initial = uam::LocalChatBundleService::Import(first, first_bundle);
		UAM_ASSERT(initial.ok && initial.imported_count == 1 && initial.items.size() == 1);
		const auto id = initial.items.front().imported_id;
		AssertBundleNativeImport(source, first, chat, id);
		const auto primary = AppPaths::UamChatFilePath(first, id);
		const auto archive_before = nlohmann::json::parse(ReadFile(primary)).at("acp_queued_prompts");
		UAM_ASSERT_EQ(archive_before.size(), std::size_t{1});
		auto passive = ChatRepository::LoadLocalChat(first, id, include_messages);
		UAM_ASSERT(passive && passive->imported_read_only && passive->acp_queued_prompts.empty());
		UAM_ASSERT_EQ(passive->messages_loaded, include_messages);
		passive->title = "Renamed passive transcript";
		passive->pinned = true;
		// Public generic metadata persistence, as used by rename/pin consumers.
		UAM_ASSERT(ChatRepository::SaveChat(first, *passive));
		const auto saved = nlohmann::json::parse(ReadFile(primary));
		UAM_ASSERT_EQ(saved.at("acp_queued_prompts"), archive_before);
		for (const bool load_messages : {false, true})
		{
			const auto reloaded = ChatRepository::LoadLocalChat(first, id, load_messages);
			UAM_ASSERT(reloaded && reloaded->imported_read_only && reloaded->pinned);
			UAM_ASSERT_EQ(reloaded->title, std::string("Renamed passive transcript"));
			UAM_ASSERT(reloaded->acp_queued_prompts.empty() && reloaded->acp_dispatched_queued_prompt_count == 0);
		}
		AssertBundleNativeImport(source, first, chat, id);
		const auto first_before = BundleFileSnapshot(first);
		const auto exported = uam::LocalChatBundleService::Export(first, second_bundle);
		UAM_ASSERT(exported.ok && exported.exported_count == 1 && exported.errors.empty());
		const auto imported = uam::LocalChatBundleService::Import(second, second_bundle);
		UAM_ASSERT(imported.ok && imported.imported_count == 1 && imported.items.size() == 1 && imported.errors.empty());
		AssertBundleNativeImport(source, second, chat, imported.items.front().imported_id);
		UAM_ASSERT_EQ(BundleFileSnapshot(first), first_before);
		UAM_ASSERT_EQ(BundleFileSnapshot(source), source_before);
	}
}

UAM_TEST(LocalChatBundleEqualOriginalGenerationsPreserveDistinctAssociationsAcrossTwoHops)
{
	TempDir temp("uam-bundle-equal-native-generations");
	const auto source = temp.root / "source", first = temp.root / "first", second = temp.root / "second";
	const auto first_bundle = temp.root / "first-bundle", second_bundle = temp.root / "second-bundle";
	fs::create_directories(source);
	ChatSession chat;
	chat.id = "equal-generation-originals";
	chat.provider_id = "gemini-cli";
	chat.title = "Equal originals with different provenance";
	chat.created_at = "2026-10-16T12:00:00Z";
	chat.updated_at = chat.created_at;
	chat.messages.push_back(Message{MessageRole::Assistant, "Recorded message original"});
	UAM_ASSERT(ChatRepository::SaveChat(source, chat));
	auto message_part = BundlePendingOriginal(BundlePngBytes(), "image/png", "equal-message-part");
	auto queue_part = BundlePendingOriginal(BundlePngBytes(), "image/png", "equal-queue-part");
	std::string error;
	auto message_batch = bundle_assets::PreparedBatch::Prepare(source, chat.id, {message_part.native_inline_payload}, error);
	UAM_ASSERT(message_batch && error.empty());
	auto queue_batch = bundle_assets::PreparedBatch::Prepare(source, chat.id, {queue_part.native_inline_payload}, error);
	UAM_ASSERT(queue_batch && error.empty());
	UAM_ASSERT(message_batch->Receipt().generation != queue_batch->Receipt().generation);
	const auto make_owned = [](MessageAttachment& part, const bundle_assets::AssetReference& reference)
	{
		part.source = AttachmentSource::OwnedNativeAsset;
		part.native_asset_key = reference.key;
		part.native_inline_payload.reset();
	};
	make_owned(message_part, message_batch->Assets().front());
	make_owned(queue_part, queue_batch->Assets().front());
	UAM_ASSERT(message_part.native_asset_key != queue_part.native_asset_key);
	UAM_ASSERT_EQ(message_part.mime_type, queue_part.mime_type);
	UAM_ASSERT_EQ(message_part.content_digest, queue_part.content_digest);
	UAM_ASSERT(message_part.native_part_key != queue_part.native_part_key);
	UAM_ASSERT(message_part.native_tool_call_id != queue_part.native_tool_call_id);
	chat.messages.front().attachments.push_back(message_part);
	uam::AcpQueuedUserPromptState prompt;
	prompt.id = "equal-original-archived-queue";
	prompt.text = "Separate queued association to the same original bytes";
	prompt.attachments.push_back(queue_part);
	chat.acp_queued_prompts.push_back(prompt);
	UAM_ASSERT(message_batch->MarkPublicationAttempted(error));
	UAM_ASSERT(queue_batch->MarkPublicationAttempted(error));
	UAM_ASSERT(ChatRepository::SaveChat(source, chat)); // Actual primary ACK for both generations.
	UAM_ASSERT(message_batch->MarkPublished(error));
	UAM_ASSERT(queue_batch->MarkPublished(error));
	AssertBundleOriginal(source, chat.id, message_part, message_part, BundlePngBytes());
	AssertBundleOriginal(source, chat.id, queue_part, queue_part, BundlePngBytes());
	const auto source_before = BundleFileSnapshot(source);
	ChatSession sentinel;
	sentinel.id = "preserved-first-destination";
	sentinel.messages.push_back(Message{MessageRole::User, "Keep this destination record"});
	UAM_ASSERT(ChatRepository::SaveChat(first, sentinel));
	sentinel.id = "preserved-second-destination";
	UAM_ASSERT(ChatRepository::SaveChat(second, sentinel));
	const auto first_before = BundleFileSnapshot(first), second_before = BundleFileSnapshot(second);
	const auto assert_import = [&](const fs::path& root, const std::string& id)
	{
		const auto loaded = ChatRepository::LoadLocalChat(root, id);
		UAM_ASSERT(loaded && loaded->imported_read_only && loaded->acp_queued_prompts.empty());
		UAM_ASSERT(loaded->native_session_id.empty() && loaded->workspace_directory.empty());
		UAM_ASSERT_EQ(loaded->messages.size(), std::size_t{1});
		UAM_ASSERT_EQ(loaded->messages.front().attachments.size(), std::size_t{1});
		const auto& actual_message = loaded->messages.front().attachments.front();
		AssertBundleOriginal(root, id, message_part, actual_message, BundlePngBytes());
		auto normalized_messages = loaded->messages;
		normalized_messages.front().attachments.front().native_asset_key = message_part.native_asset_key;
		UAM_ASSERT_EQ(normalized_messages, chat.messages);
		const auto archive = ChatRepository::ParseArchivalQueuedPrompts(ReadFile(AppPaths::UamChatFilePath(root, id)), id);
		UAM_ASSERT(archive && archive->size() == 1 && archive->front().attachments.size() == 1);
		UAM_ASSERT_EQ(archive->front().id, prompt.id);
		UAM_ASSERT_EQ(archive->front().text, prompt.text);
		const auto& actual_queue = archive->front().attachments.front();
		AssertBundleOriginal(root, id, queue_part, actual_queue, BundlePngBytes());
		UAM_ASSERT(actual_message.native_part_key != actual_queue.native_part_key);
		UAM_ASSERT(actual_message.native_tool_call_id != actual_queue.native_tool_call_id);
	};
	const auto exported = uam::LocalChatBundleService::Export(source, first_bundle);
	UAM_ASSERT(exported.ok && exported.exported_count == 1 && exported.errors.empty());
	const auto initial = uam::LocalChatBundleService::Import(first, first_bundle);
	UAM_ASSERT(initial.ok && initial.imported_count == 1 && initial.items.size() == 1 && initial.errors.empty());
	const auto first_id = initial.items.front().imported_id;
	assert_import(first, first_id);
	for (const auto& [file, bytes] : first_before) UAM_ASSERT_EQ(ReadFile(first / file), bytes);
	const auto passive_before = BundleFileSnapshot(first);
	const auto reexported = uam::LocalChatBundleService::Export(first, second_bundle);
	UAM_ASSERT(reexported.ok && reexported.exported_count == 2 && reexported.errors.empty());
	const auto imported = uam::LocalChatBundleService::Import(second, second_bundle);
	UAM_ASSERT(imported.ok && imported.imported_count == 2 && imported.items.size() == 2 && imported.errors.empty());
	const auto item = std::find_if(imported.items.begin(), imported.items.end(), [&](const auto& candidate)
	    { return candidate.source_id == first_id; });
	UAM_ASSERT(item != imported.items.end() && item->imported);
	assert_import(second, item->imported_id);
	for (const auto& [file, bytes] : second_before) UAM_ASSERT_EQ(ReadFile(second / file), bytes);
	UAM_ASSERT_EQ(BundleFileSnapshot(first), passive_before);
	UAM_ASSERT_EQ(BundleFileSnapshot(source), source_before);
}
#endif

#include "common/chat/chat_message_page_index.h"

#if defined(__APPLE__) || defined(_WIN32)
UAM_TEST(LocalChatBundlePassiveArchiveUnknownAndStaleSavesPreservePrimary)
{
	TempDir temp("uam-bundle-passive-archive-authority");
	const auto source = temp.root / "source", bundle = temp.root / "bundle", target = temp.root / "target";
	const auto original = SaveBundleNativeOriginals(source);
	UAM_ASSERT(uam::LocalChatBundleService::Export(source, bundle).ok);
	UAM_ASSERT(uam::LocalChatBundleService::Import(target, bundle).ok);
	const auto primary = AppPaths::UamChatFilePath(target, original.id);
	const auto archive_before = nlohmann::json::parse(ReadFile(primary)).at("acp_queued_prompts");
	auto unknown = ChatRepository::LoadLocalChat(target, original.id);
	UAM_ASSERT(unknown && unknown->passive_queued_prompt_archive);
	unknown->passive_queued_prompt_archive.reset();
	unknown->title = "Unknown archive must preserve the authoritative primary";
	UAM_ASSERT(ChatRepository::SaveChat(target, *unknown));
	UAM_ASSERT_EQ(nlohmann::json::parse(ReadFile(primary)).at("acp_queued_prompts"), archive_before);
	auto stale = ChatRepository::LoadLocalChat(target, original.id);
	UAM_ASSERT(stale && stale->acp_queued_prompts.empty());
	auto changed = nlohmann::json::parse(ReadFile(primary));
	changed["acp_queued_prompts"][0]["text"] = "Different stale archive content";
	stale->passive_queued_prompt_archive = ChatRepository::ParsePassiveQueuedPromptArchive(changed.dump(), stale->id);
	UAM_ASSERT(stale->passive_queued_prompt_archive);
	const auto before_rejection = ReadFile(primary);
	UAM_ASSERT(!ChatRepository::SaveChat(target, *stale));
	UAM_ASSERT(!ChatRepository::PrepareChatSave(target, *stale));
	UAM_ASSERT_EQ(ReadFile(primary), before_rejection);
	auto current = ChatRepository::LoadLocalChat(target, original.id);
	UAM_ASSERT(current);
	auto prepared = ChatRepository::PrepareChatSave(target, *current);
	UAM_ASSERT(prepared);
	current->title = "A newer primary defeats previously prepared metadata";
	UAM_ASSERT(ChatRepository::SaveChat(target, *current));
	const auto newer = ReadFile(primary);
	UAM_ASSERT(!ChatRepository::PublishPreparedChatSave(prepared));
	UAM_ASSERT_EQ(ReadFile(primary), newer);
	AssertBundleNativeImport(source, target, original, original.id);
}

UAM_TEST(LocalChatBundlePassiveArchivePagingHydrationAndFingerprints)
{
	TempDir temp("uam-bundle-passive-archive-projections");
	const auto source = temp.root / "source", bundle = temp.root / "bundle", target = temp.root / "target";
	const auto original = SaveBundleNativeOriginals(source);
	UAM_ASSERT(uam::LocalChatBundleService::Export(source, bundle).ok);
	UAM_ASSERT(uam::LocalChatBundleService::Import(target, bundle).ok);
	const auto summary = ChatRepository::LoadLocalChat(target, original.id, false);
	UAM_ASSERT(summary && summary->passive_queued_prompt_archive && summary->acp_queued_prompts.empty());
	auto copied = *summary;
	UAM_ASSERT_EQ(copied.passive_queued_prompt_archive.get(), summary->passive_queued_prompt_archive.get());
	copied.passive_queued_prompt_archive = ChatRepository::ParsePassiveQueuedPromptArchive(
	    ReadFile(AppPaths::UamChatFilePath(target, original.id)), original.id);
	UAM_ASSERT(copied.passive_queued_prompt_archive.get() != summary->passive_queued_prompt_archive.get());
	UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(target, copied), ChatRepository::ChatMetadataFingerprint(target, *summary));
	const auto page = ChatRepository::ReadMessagePage(target, *summary, 1);
	UAM_ASSERT(page && page->chat.passive_queued_prompt_archive && page->chat.acp_queued_prompts.empty());
	UAM_ASSERT(ChatRepository::MatchesMessagePage(*summary, *page));
	UAM_ASSERT_EQ(page->chat.passive_queued_prompt_archive->canonical_prompts_json, summary->passive_queued_prompt_archive->canonical_prompts_json);
	auto known_empty = *summary;
	known_empty.passive_queued_prompt_archive = ChatRepository::ParsePassiveQueuedPromptArchive(nlohmann::json{{"id", original.id}}.dump(), original.id);
	UAM_ASSERT(known_empty.passive_queued_prompt_archive);
	UAM_ASSERT(ChatRepository::ChatMetadataFingerprint(target, known_empty) != ChatRepository::ChatMetadataFingerprint(target, *summary));
	UAM_ASSERT_EQ(ChatRepository::QueueAdmissionFingerprint(known_empty), ChatRepository::QueueAdmissionFingerprint(*summary));
	UAM_ASSERT(!ChatRepository::MatchesMessagePage(known_empty, *page));
	UAM_ASSERT(!ChatRepository::HydrateChatMessages(target, known_empty));
	auto unknown = known_empty;
	unknown.passive_queued_prompt_archive.reset();
	UAM_ASSERT(ChatRepository::ChatMetadataFingerprint(target, unknown) != ChatRepository::ChatMetadataFingerprint(target, known_empty));
	UAM_ASSERT(ChatRepository::HydrateChatMessages(target, unknown));
	UAM_ASSERT(unknown.passive_queued_prompt_archive && unknown.acp_queued_prompts.empty());
	UAM_ASSERT_EQ(unknown.passive_queued_prompt_archive->canonical_prompts_json, summary->passive_queued_prompt_archive->canonical_prompts_json);
	auto adopted = *summary;
	adopted.passive_queued_prompt_archive.reset();
	auto full = ChatRepository::LoadLocalChat(target, original.id);
	UAM_ASSERT(full && ChatRepository::AdoptHydratedMessagesIfUnchanged(adopted, std::move(*full), summary->persisted_message_count, summary->persisted_messages_digest));
	UAM_ASSERT(adopted.passive_queued_prompt_archive && adopted.acp_queued_prompts.empty());
	auto active = adopted;
	active.imported_read_only = false;
	const auto primary = AppPaths::UamChatFilePath(target, original.id);
	const auto before = ReadFile(primary);
	UAM_ASSERT(!ChatRepository::SaveChat(target, active));
	UAM_ASSERT_EQ(ReadFile(primary), before);
}

UAM_TEST(LocalChatBundlePassiveArchiveCodecPreservesRawMetadataAndBounds)
{
	TempDir temp("uam-bundle-passive-archive-codec");
	const auto source = temp.root / "source", bundle = temp.root / "bundle", target = temp.root / "target";
	const auto original = SaveBundleNativeOriginals(source);
	auto canonical = nlohmann::json::parse(ReadFile(AppPaths::UamChatFilePath(source, original.id)));
	canonical["acp_queued_prompts"][0]["inert_provider_metadata"] = {{"opaque", true}, {"references", {"retain", "without dispatch"}}};
	const auto archive = ChatRepository::ParsePassiveQueuedPromptArchive(canonical.dump(), original.id);
	UAM_ASSERT(archive && nlohmann::json::parse(archive->canonical_prompts_json) == canonical.at("acp_queued_prompts"));
	UAM_ASSERT(!ChatRepository::ParsePassiveQueuedPromptArchive(canonical.dump(), "foreign-owner"));
	for (int variant = 0; variant < 6; ++variant)
	{
		auto invalid = canonical;
		auto& queue = invalid["acp_queued_prompts"];
		if (variant == 0) for (int index = 1; index < 33; ++index) queue.push_back(queue[0]);
		if (variant == 1) for (int index = 1; index < 65; ++index) queue[0]["attachments"].push_back(queue[0]["attachments"][0]);
		if (variant == 2) queue[0]["text"] = std::string(2U * 1024U * 1024U + 1U, 't');
		if (variant == 3) queue[0]["attachments"][0]["nativePartKey"] = std::string(16385, 'p');
		if (variant == 4) queue[0]["attachments"][0]["inert_padding"] = std::string(1024U * 1024U, 'm');
		if (variant == 5) queue[0]["inert_padding"] = std::string(16U * 1024U * 1024U, 'a');
		UAM_ASSERT(!ChatRepository::ParsePassiveQueuedPromptArchive(invalid.dump(), original.id));
	}
	UAM_ASSERT(uam::LocalChatBundleService::Export(source, bundle).ok);
	const auto bundled_primary = AppPaths::UamChatFilePath(bundle, original.id);
	auto bundled = nlohmann::json::parse(ReadFile(bundled_primary));
	bundled["acp_queued_prompts"][0]["inert_provider_metadata"] = canonical["acp_queued_prompts"][0]["inert_provider_metadata"];
	const auto text = bundled.dump(2);
	UAM_ASSERT(uam::io::WriteTextFile(bundled_primary, text));
	auto manifest = nlohmann::json::parse(ReadFile(bundle / "manifest.json"));
	manifest["chats"][0]["byteSize"] = text.size();
	UAM_ASSERT(uam::io::WriteTextFile(bundle / "manifest.json", manifest.dump(2)));
	UAM_ASSERT(uam::LocalChatBundleService::Import(target, bundle).ok);
	const auto primary = AppPaths::UamChatFilePath(target, original.id);
	const auto raw_before = nlohmann::json::parse(ReadFile(primary)).at("acp_queued_prompts");
	auto passive = ChatRepository::LoadLocalChat(target, original.id);
	UAM_ASSERT(passive && passive->acp_queued_prompts.empty());
	passive->title = "Preserve inert raw provider metadata";
	UAM_ASSERT(ChatRepository::SaveChat(target, *passive));
	UAM_ASSERT_EQ(nlohmann::json::parse(ReadFile(primary)).at("acp_queued_prompts"), raw_before);
	UAM_ASSERT_EQ(raw_before[0]["inert_provider_metadata"], canonical["acp_queued_prompts"][0]["inert_provider_metadata"]);
}

UAM_TEST(LocalChatBundleLegacyPassiveArchiveUsesExplicitWorkerMigration)
{
	for (const bool no_messages : {false, true})
	for (const bool include_messages : {false, true})
	{
		TempDir temp("uam-bundle-legacy-passive-archive-migration");
		const auto source = temp.root / "source", bundle = temp.root / "bundle", target = temp.root / "target";
		const auto original = SaveBundleNativeOriginals(source);
		UAM_ASSERT(uam::LocalChatBundleService::Export(source, bundle).ok);
		UAM_ASSERT(uam::LocalChatBundleService::Import(target, bundle).ok);
		const auto primary = AppPaths::UamChatFilePath(target, original.id);
		auto legacy = nlohmann::json::parse(ReadFile(primary));
		if (no_messages)
		{
			legacy.erase("messages"); // Previous loaded empty saves omitted the array.
			legacy["persisted_message_count"] = 0;
			legacy.erase("persisted_messages_digest");
		}
		UAM_ASSERT(uam::io::WriteTextFile(primary, legacy.dump(2)));
		UAM_ASSERT(fs::remove(uam::chat_pages::IndexPath(primary)));
		std::error_code error;
		fs::remove(AppPaths::UamChatSummaryFilePath(target, original.id), error);
		auto passive = ChatRepository::LoadLocalChat(target, original.id, include_messages);
		UAM_ASSERT(passive && passive->passive_queued_prompt_archive && passive->acp_queued_prompts.empty());
		passive->title = "Rename after explicit worker archive migration";
		passive->pinned = true;
		const auto before = ReadFile(primary);
		UAM_ASSERT(!ChatRepository::SaveChat(target, *passive)); // No implicit GUI transcript scan.
		UAM_ASSERT_EQ(ReadFile(primary), before);
		const auto revision = ChatRepository::ChatStorageRevision(target, original.id);
		UAM_ASSERT(revision);
		auto prepared = ChatRepository::PrepareChatSave(target, *passive, *revision); // Existing worker entry point.
		UAM_ASSERT(prepared && ReadFile(primary) == before);
		UAM_ASSERT(ChatRepository::PublishPreparedChatSave(prepared));
		UAM_ASSERT(fs::is_regular_file(uam::chat_pages::IndexPath(primary)));
		passive = ChatRepository::LoadLocalChat(target, original.id, include_messages);
		UAM_ASSERT(passive && passive->acp_queued_prompts.empty());
		passive->title = "Ordinary metadata save now retains the migrated archive";
		UAM_ASSERT(ChatRepository::SaveChat(target, *passive));
		UAM_ASSERT_EQ(nlohmann::json::parse(ReadFile(primary)).at("acp_queued_prompts"), legacy.at("acp_queued_prompts"));
		const auto archive = ChatRepository::ParseArchivalQueuedPrompts(ReadFile(primary), original.id);
		UAM_ASSERT(archive && archive->size() == 1 && archive->front().attachments.size() == 1);
		const auto bytes = bundle_assets::ReadOwned(target, original.id, archive->front().attachments.front().native_asset_key);
		UAM_ASSERT(bytes.ok && bytes.bytes == std::string("\0Q\0R\xff", 5));
	}
}

UAM_TEST(LocalChatBundleNativeInventoryStillRejectsDuplicateDeclarations)
{
	TempDir temp("uam-bundle-native-duplicate-inventory");
	const auto source = temp.root / "source", bundle = temp.root / "bundle", target = temp.root / "target";
	const auto original = SaveBundleNativeOriginals(source);
	UAM_ASSERT(uam::LocalChatBundleService::Export(source, bundle).ok);
	auto manifest = nlohmann::json::parse(ReadFile(bundle / "manifest.json"));
	UAM_ASSERT(!manifest["chats"][0]["originals"].empty());
	manifest["chats"][0]["originals"].push_back(manifest["chats"][0]["originals"][0]);
	UAM_ASSERT(uam::io::WriteTextFile(bundle / "manifest.json", manifest.dump(2)));
	const auto imported = uam::LocalChatBundleService::Import(target, bundle);
	UAM_ASSERT(!imported.ok && imported.imported_count == 0 && !imported.errors.empty());
	UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(target, original.id)));
}
#endif

#if defined(__APPLE__) || defined(_WIN32)
UAM_TEST(LocalChatBundlePassiveArchiveNumericValuesRemainDistinctInFactoriesAndFingerprints)
{
	TempDir temp("uam-bundle-archive-numeric-factory");
	for (const std::uint64_t sequence : {std::uint64_t{1000000000000001ULL}, std::uint64_t{9007199254740993ULL}})
	{
		nlohmann::json document = {
		    {"id", "numeric-archive-owner"},
		    {"acp_queued_prompts", nlohmann::json::array({{
		        {"id", "numeric-archival-input"}, {"text", "Inert numeric metadata"},
		        {"inert_provider_metadata", {{"sequence", sequence}}}
		    }})}
		};
		const auto wire = nlohmann::json::parse(document.dump());
		const auto& wire_value = wire.at("acp_queued_prompts").at(0).at("inert_provider_metadata").at("sequence");
		UAM_ASSERT(wire_value.is_number_integer());
		UAM_ASSERT_EQ(wire_value.get<std::uint64_t>(), sequence);
		const auto exact = ChatRepository::ParsePassiveQueuedPromptArchive(document.dump(), "numeric-archive-owner");
		UAM_ASSERT(exact);
		document["acp_queued_prompts"][0]["inert_provider_metadata"]["sequence"] = sequence - std::uint64_t{1};
		const auto adjacent = ChatRepository::ParsePassiveQueuedPromptArchive(document.dump(), "numeric-archive-owner");
		UAM_ASSERT(adjacent);
		UAM_ASSERT(exact->canonical_prompts_json != adjacent->canonical_prompts_json);
		const auto captured = nlohmann::json::parse(exact->canonical_prompts_json);
		const auto& captured_value = captured.at(0).at("inert_provider_metadata").at("sequence");
		UAM_ASSERT(captured_value.is_number_integer());
		UAM_ASSERT_EQ(captured_value.get<std::uint64_t>(), sequence);
		const auto other = nlohmann::json::parse(adjacent->canonical_prompts_json);
		UAM_ASSERT_EQ(other.at(0).at("inert_provider_metadata").at("sequence").get<std::uint64_t>(), sequence - std::uint64_t{1});
		ChatSession first;
		first.id = "numeric-archive-owner";
		first.imported_read_only = true;
		first.passive_queued_prompt_archive = exact;
		auto second = first;
		second.passive_queued_prompt_archive = adjacent;
		const auto first_fingerprint = ChatRepository::ChatMetadataFingerprint(temp.root, first);
		const auto second_fingerprint = ChatRepository::ChatMetadataFingerprint(temp.root, second);
		UAM_ASSERT(!first_fingerprint.empty() && !second_fingerprint.empty());
		UAM_ASSERT(first_fingerprint != second_fingerprint);
		UAM_ASSERT_EQ(ChatRepository::QueueAdmissionFingerprint(first), ChatRepository::QueueAdmissionFingerprint(second));
	}
}

UAM_TEST(LocalChatBundlePassiveArchiveNumericMetadataSurvivesSaveAndTwoHops)
{
	for (const std::uint64_t sequence : {std::uint64_t{1000000000000001ULL}, std::uint64_t{9007199254740993ULL}})
	{
		TempDir temp("uam-bundle-archive-numeric-two-hop");
		const auto source = temp.root / "source", first = temp.root / "first", second = temp.root / "second";
		const auto first_bundle = temp.root / "first-bundle", second_bundle = temp.root / "second-bundle";
		const auto original = SaveBundleNativeOriginals(source);
		const auto source_before = BundleFileSnapshot(source);
		UAM_ASSERT(uam::LocalChatBundleService::Export(source, first_bundle).ok);
		const auto bundled_primary = AppPaths::UamChatFilePath(first_bundle, original.id);
		auto bundled = nlohmann::json::parse(ReadFile(bundled_primary));
		bundled["acp_queued_prompts"][0]["inert_provider_metadata"] = {{"sequence", sequence}};
		const auto text = bundled.dump(2); // Lossless integer fixture, never a double literal.
		UAM_ASSERT_EQ(nlohmann::json::parse(text).at("acp_queued_prompts").at(0).at("inert_provider_metadata").at("sequence").get<std::uint64_t>(), sequence);
		UAM_ASSERT(uam::io::WriteTextFile(bundled_primary, text));
		auto manifest = nlohmann::json::parse(ReadFile(first_bundle / "manifest.json"));
		manifest["chats"][0]["byteSize"] = text.size();
		UAM_ASSERT(uam::io::WriteTextFile(first_bundle / "manifest.json", manifest.dump(2)));
		const auto assert_numeric_archive = [&](const fs::path& root, const std::string& id)
		{
			for (const auto& path : {AppPaths::UamChatFilePath(root, id), AppPaths::UamChatSummaryFilePath(root, id)})
			{
				const auto raw = nlohmann::json::parse(ReadFile(path));
				const auto& value = raw.at("acp_queued_prompts").at(0).at("inert_provider_metadata").at("sequence");
				UAM_ASSERT(value.is_number_integer());
				UAM_ASSERT_EQ(value.get<std::uint64_t>(), sequence);
			}
			for (const bool include_messages : {false, true})
			{
				const auto passive = ChatRepository::LoadLocalChat(root, id, include_messages);
				UAM_ASSERT(passive && passive->imported_read_only && passive->passive_queued_prompt_archive);
				UAM_ASSERT(passive->acp_queued_prompts.empty() && !passive->acp_queue_held && passive->acp_dispatched_queued_prompt_count == 0);
				const auto captured = nlohmann::json::parse(passive->passive_queued_prompt_archive->canonical_prompts_json);
				UAM_ASSERT(captured.at(0).at("inert_provider_metadata").at("sequence").is_number_integer());
				UAM_ASSERT_EQ(captured.at(0).at("inert_provider_metadata").at("sequence").get<std::uint64_t>(), sequence);
			}
			AssertBundleNativeImport(source, root, original, id);
		};
		const auto initial = uam::LocalChatBundleService::Import(first, first_bundle);
		UAM_ASSERT(initial.ok && initial.imported_count == 1 && initial.items.size() == 1 && initial.errors.empty());
		const auto id = initial.items.front().imported_id;
		assert_numeric_archive(first, id);
		auto selected = ChatRepository::LoadLocalChat(first, id, false);
		UAM_ASSERT(selected);
		selected->last_opened_at = "2026-10-17T12:10:00Z";
		UAM_ASSERT(ChatRepository::SaveLastOpenedAt(first, *selected));
		assert_numeric_archive(first, id);
		selected->last_opened_at = "2026-10-17T12:11:00Z";
		const auto prepared_selection = ChatRepository::PrepareLastOpenedAt(first, *selected);
		UAM_ASSERT(prepared_selection && ChatRepository::PublishPreparedChatSave(prepared_selection));
		assert_numeric_archive(first, id);
		const auto primary = AppPaths::UamChatFilePath(first, id);
		auto stale_document = nlohmann::json::parse(ReadFile(primary));
		stale_document["acp_queued_prompts"][0]["inert_provider_metadata"]["sequence"] = sequence - std::uint64_t{1};
		auto stale = ChatRepository::LoadLocalChat(first, id);
		UAM_ASSERT(stale);
		stale->passive_queued_prompt_archive = ChatRepository::ParsePassiveQueuedPromptArchive(stale_document.dump(), id);
		UAM_ASSERT(stale->passive_queued_prompt_archive);
		const auto before_stale_save = ReadFile(primary);
		UAM_ASSERT(!ChatRepository::SaveChat(first, *stale));
		UAM_ASSERT_EQ(ReadFile(primary), before_stale_save);
		for (const bool include_messages : {true, false})
		{
			auto passive = ChatRepository::LoadLocalChat(first, id, include_messages);
			UAM_ASSERT(passive && passive->acp_queued_prompts.empty());
			passive->title = include_messages ? "Numeric archive loaded rename" : "Numeric archive metadata rename";
			passive->pinned = true;
			UAM_ASSERT(ChatRepository::SaveChat(first, *passive));
			assert_numeric_archive(first, id);
		}
		const auto passive_before = BundleFileSnapshot(first);
		const auto exported = uam::LocalChatBundleService::Export(first, second_bundle);
		UAM_ASSERT(exported.ok && exported.exported_count == 1 && exported.errors.empty());
		const auto second_wire = nlohmann::json::parse(ReadFile(AppPaths::UamChatFilePath(second_bundle, id)));
		UAM_ASSERT_EQ(second_wire.at("acp_queued_prompts").at(0).at("inert_provider_metadata").at("sequence").get<std::uint64_t>(), sequence);
		const auto imported = uam::LocalChatBundleService::Import(second, second_bundle);
		UAM_ASSERT(imported.ok && imported.imported_count == 1 && imported.items.size() == 1 && imported.errors.empty());
		assert_numeric_archive(second, imported.items.front().imported_id);
		UAM_ASSERT_EQ(BundleFileSnapshot(first), passive_before);
		UAM_ASSERT_EQ(BundleFileSnapshot(source), source_before);
	}
}

UAM_TEST(LocalChatBundleIndexedLegacyQueueIdsConvertToPassiveWithoutArchiveRewrite)
{
	for (const bool duplicate_ids : {false, true})
	{
		TempDir temp("uam-bundle-indexed-legacy-id-conversion");
		const auto root = temp.root / "active", bundle = temp.root / "bundle", target = temp.root / "target";
		ChatSession active;
		active.id = "indexed-legacy-queue-conversion";
		active.provider_id = "codex-cli";
		active.created_at = "2026-10-17T12:00:00Z";
		active.updated_at = active.created_at;
		active.messages.push_back(Message{MessageRole::User, "Retain indexed active history"});
		uam::AcpQueuedUserPromptState prompt;
		prompt.id = duplicate_ids ? "repeated-legacy-id" : "";
		prompt.text = "Raw legacy queue identity must remain archival authority";
		active.acp_queued_prompts.push_back(prompt);
		if (duplicate_ids)
		{
			prompt.text = "Distinct second row with the same raw legacy identity";
			active.acp_queued_prompts.push_back(prompt);
		}
		active.acp_queue_held = true;
		active.acp_dispatched_queued_prompt_count = 1;
		UAM_ASSERT(ChatRepository::SaveChat(root, active));
		const auto prepared = ChatRepository::PrepareChatSave(root, active);
		UAM_ASSERT(prepared && ChatRepository::PublishPreparedChatSave(prepared));
		const auto primary = AppPaths::UamChatFilePath(root, active.id);
		const auto raw = nlohmann::json::parse(ReadFile(primary));
		const auto raw_archive = raw.at("acp_queued_prompts");
		UAM_ASSERT_EQ(raw_archive.at(0).at("id").get<std::string>(), active.acp_queued_prompts.front().id);
		if (duplicate_ids) UAM_ASSERT_EQ(raw_archive.at(0).at("id"), raw_archive.at(1).at("id"));
		auto loaded = ChatRepository::LoadLocalChat(root, active.id);
		UAM_ASSERT(loaded && !loaded->imported_read_only && !loaded->passive_queued_prompt_archive);
		UAM_ASSERT_EQ(loaded->acp_queued_prompts.size(), active.acp_queued_prompts.size());
		UAM_ASSERT(loaded->acp_queue_held && loaded->acp_dispatched_queued_prompt_count == 1);
		const auto normalized_row = duplicate_ids ? std::size_t{1} : std::size_t{0};
		UAM_ASSERT(loaded->acp_queued_prompts.at(normalized_row).id != raw_archive.at(normalized_row).at("id").get<std::string>());
		const auto revision = ChatRepository::ChatStorageRevision(root, active.id);
		UAM_ASSERT(revision);
		const auto index = uam::chat_pages::ReadIndex(uam::chat_pages::IndexPath(primary), *revision);
		UAM_ASSERT(index && index->attachment_owner == active.id && index->message_count == 1);
		loaded->imported_read_only = true;
		UAM_ASSERT(ChatRepository::SaveChat(root, *loaded)); // Must pass with a CURRENT index, independent of migration.
		UAM_ASSERT_EQ(nlohmann::json::parse(ReadFile(primary)).at("acp_queued_prompts"), raw_archive);
		for (const bool include_messages : {false, true})
		{
			const auto passive = ChatRepository::LoadLocalChat(root, active.id, include_messages);
			UAM_ASSERT(passive && passive->imported_read_only && passive->passive_queued_prompt_archive);
			UAM_ASSERT(passive->acp_queued_prompts.empty() && !passive->acp_queue_held && passive->acp_dispatched_queued_prompt_count == 0);
			UAM_ASSERT_EQ(nlohmann::json::parse(passive->passive_queued_prompt_archive->canonical_prompts_json), raw_archive);
		}
		UAM_ASSERT(uam::LocalChatBundleService::Export(root, bundle).ok);
		UAM_ASSERT(uam::LocalChatBundleService::Import(target, bundle).ok);
		UAM_ASSERT_EQ(nlohmann::json::parse(ReadFile(AppPaths::UamChatFilePath(target, active.id))).at("acp_queued_prompts"), raw_archive);
	}
}
#endif

#if defined(__APPLE__) || defined(_WIN32)
UAM_TEST(LocalChatBundlePassiveArchiveStreamedImportRetainsMessagesAndNumericMetadata)
{
	TempDir temp("uam-bundle-passive-streamed-archive");
	ChatSession metadata;
	metadata.id = "passive-streamed-numeric-archive";
	metadata.provider_id = "codex-cli";
	metadata.imported_read_only = true;
	const std::uint64_t sequence = 9007199254740993ULL;
	const nlohmann::json canonical = {
	    {"id", metadata.id},
	    {"acp_queued_prompts", nlohmann::json::array({{
	        {"id", "inert-streamed-input"}, {"text", "Remain an archive"},
	        {"inert_provider_metadata", {{"sequence", sequence}}}
	    }})}
	};
	metadata.passive_queued_prompt_archive = ChatRepository::ParsePassiveQueuedPromptArchive(canonical.dump(), metadata.id);
	UAM_ASSERT(metadata.passive_queued_prompt_archive);
	const std::vector<Message> messages = {
	    Message{MessageRole::User, "Keep the first streamed message"},
	    Message{MessageRole::Assistant, "Keep the second streamed message"}
	};
	const auto prepared = ChatRepository::PrepareImportedChatSave(temp.root, metadata,
	    [&](const std::function<bool(const Message&)>& emit, ChatSession&)
	    {
	        for (const auto& message : messages) if (!emit(message)) return false;
	        return true;
	    });
	UAM_ASSERT(prepared && ChatRepository::PublishPreparedChatSave(prepared));
	const auto loaded = ChatRepository::LoadLocalChat(temp.root, metadata.id);
	UAM_ASSERT(loaded && loaded->messages_loaded && loaded->imported_read_only);
	UAM_ASSERT_EQ(loaded->messages.size(), messages.size());
	for (std::size_t index = 0; index < messages.size(); ++index)
	{
		UAM_ASSERT_EQ(loaded->messages[index].role, messages[index].role);
		UAM_ASSERT_EQ(loaded->messages[index].content, messages[index].content);
	}
	UAM_ASSERT(loaded->acp_queued_prompts.empty() && !loaded->acp_queue_held && loaded->acp_dispatched_queued_prompt_count == 0);
	UAM_ASSERT(loaded->passive_queued_prompt_archive);
	UAM_ASSERT_EQ(nlohmann::json::parse(loaded->passive_queued_prompt_archive->canonical_prompts_json), canonical.at("acp_queued_prompts"));
	for (const auto& path : {AppPaths::UamChatFilePath(temp.root, metadata.id), AppPaths::UamChatSummaryFilePath(temp.root, metadata.id)})
	{
		const auto document = nlohmann::json::parse(ReadFile(path));
		const auto& value = document.at("acp_queued_prompts").at(0).at("inert_provider_metadata").at("sequence");
		UAM_ASSERT(value.is_number_integer());
		UAM_ASSERT_EQ(value.get<std::uint64_t>(), sequence);
	}
	const auto revision = ChatRepository::ChatStorageRevision(temp.root, metadata.id);
	UAM_ASSERT(revision);
	const auto index = uam::chat_pages::ReadIndex(uam::chat_pages::IndexPath(AppPaths::UamChatFilePath(temp.root, metadata.id)), *revision);
	UAM_ASSERT(index && index->message_count == messages.size() && index->attachment_owner == metadata.id);
}
#endif

#if defined(__APPLE__) || defined(_WIN32)
UAM_TEST(LocalChatBundleEmptyActiveQueueConvertsFromOriginalAndLoadedSnapshots)
{
	for (const bool use_loaded : {false, true})
	for (const bool missing_queue_field : {false, true})
	{
		TempDir temp("uam-bundle-empty-active-conversion");
		ChatSession active;
		active.id = "empty-active-conversion";
		active.provider_id = "codex-cli";
		active.messages.push_back(Message{MessageRole::User, "Retain active history without queued inputs"});
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, active));
		const auto primary = AppPaths::UamChatFilePath(temp.root, active.id);
		UAM_ASSERT(!fs::exists(uam::chat_pages::IndexPath(primary)));
		if (missing_queue_field)
		{
			auto legacy = nlohmann::json::parse(ReadFile(primary));
			legacy.erase("acp_queued_prompts");
			UAM_ASSERT(uam::io::WriteTextFile(primary, legacy.dump(2)));
			std::error_code error;
			fs::remove(AppPaths::UamChatSummaryFilePath(temp.root, active.id), error);
		}
		auto candidate = use_loaded ? ChatRepository::LoadLocalChat(temp.root, active.id) : std::optional<ChatSession>(active);
		UAM_ASSERT(candidate && !candidate->imported_read_only && candidate->acp_queued_prompts.empty() && !candidate->passive_queued_prompt_archive);
		candidate->imported_read_only = true;
		candidate->title = "Convert an empty active queue";
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, *candidate));
		const auto converted = nlohmann::json::parse(ReadFile(primary));
		UAM_ASSERT(converted.at("imported_read_only").get<bool>());
		UAM_ASSERT(converted.at("acp_queued_prompts").is_array() && converted.at("acp_queued_prompts").empty());
		for (const bool include_messages : {false, true})
		{
			const auto passive = ChatRepository::LoadLocalChat(temp.root, active.id, include_messages);
			UAM_ASSERT(passive && passive->imported_read_only && passive->passive_queued_prompt_archive);
			UAM_ASSERT(passive->acp_queued_prompts.empty() && !passive->acp_queue_held && passive->acp_dispatched_queued_prompt_count == 0);
			UAM_ASSERT(nlohmann::json::parse(passive->passive_queued_prompt_archive->canonical_prompts_json).empty());
			if (include_messages)
			{
				UAM_ASSERT_EQ(passive->messages.size(), std::size_t{1});
				UAM_ASSERT_EQ(passive->messages.front().content, active.messages.front().content);
			}
		}
	}
}

UAM_TEST(LocalChatBundleUnknownPassiveArchiveAndUnprovenPolicyFailClosed)
{
	TempDir temp("uam-bundle-passive-policy-probe-guard");
	const auto source = temp.root / "source", bundle = temp.root / "bundle", target = temp.root / "target";
	const auto original = SaveBundleNativeOriginals(source);
	UAM_ASSERT(uam::LocalChatBundleService::Export(source, bundle).ok);
	UAM_ASSERT(uam::LocalChatBundleService::Import(target, bundle).ok);
	const auto primary = AppPaths::UamChatFilePath(target, original.id);
	auto legacy = nlohmann::json::parse(ReadFile(primary));
	UAM_ASSERT(!legacy.at("acp_queued_prompts").empty());
	UAM_ASSERT(uam::io::WriteTextFile(primary, legacy.dump(2)));
	UAM_ASSERT(fs::remove(uam::chat_pages::IndexPath(primary)));
	std::error_code error;
	fs::remove(AppPaths::UamChatSummaryFilePath(target, original.id), error);
	auto unknown = ChatRepository::LoadLocalChat(target, original.id);
	UAM_ASSERT(unknown && unknown->imported_read_only && unknown->passive_queued_prompt_archive && unknown->acp_queued_prompts.empty());
	unknown->passive_queued_prompt_archive.reset();
	unknown->title = "Unknown is not an empty active queue";
	unknown->pinned = true;
	const auto before = BundleFileSnapshot(target);
	UAM_ASSERT(!ChatRepository::SaveChat(target, *unknown));
	UAM_ASSERT_EQ(BundleFileSnapshot(target), before);
	AssertBundleNativeImport(source, target, original, original.id);

	// An empty caller snapshot cannot erase a nonempty raw active archive.
	ChatSession active_archive;
	active_archive.id = "active-archive-with-empty-candidate";
	uam::AcpQueuedUserPromptState retained;
	retained.id = "primary-only-queued-input";
	retained.text = "Preserve raw archive despite an empty caller queue";
	active_archive.acp_queued_prompts.push_back(retained);
	const auto active_root = temp.root / "active-archive";
	UAM_ASSERT(ChatRepository::SaveChat(active_root, active_archive));
	const auto active_primary = AppPaths::UamChatFilePath(active_root, active_archive.id);
	const auto retained_archive = nlohmann::json::parse(ReadFile(active_primary)).at("acp_queued_prompts");
	active_archive.acp_queued_prompts.clear();
	active_archive.imported_read_only = true;
	UAM_ASSERT(ChatRepository::SaveChat(active_root, active_archive));
	UAM_ASSERT_EQ(nlohmann::json::parse(ReadFile(active_primary)).at("acp_queued_prompts"), retained_archive);
	const auto active_passive = ChatRepository::LoadLocalChat(active_root, active_archive.id);
	UAM_ASSERT(active_passive && active_passive->acp_queued_prompts.empty() && active_passive->passive_queued_prompt_archive);
	UAM_ASSERT_EQ(nlohmann::json::parse(active_passive->passive_queued_prompt_archive->canonical_prompts_json), retained_archive);

	// Policy after the fixed probe ceiling is unknown, not active admission.
	const auto capped_root = temp.root / "capped";
	ChatSession capped;
	capped.id = "unproven-active-policy";
	UAM_ASSERT(ChatRepository::SaveChat(capped_root, capped));
	const auto capped_primary = AppPaths::UamChatFilePath(capped_root, capped.id);
	auto delayed_policy = nlohmann::json::parse(ReadFile(capped_primary));
	delayed_policy["a_inert_padding"] = std::string(64U * 1024U, 'p');
	UAM_ASSERT(uam::io::WriteTextFile(capped_primary, delayed_policy.dump()));
	const auto capped_before = ReadFile(capped_primary);
	capped.imported_read_only = true;
	UAM_ASSERT(!ChatRepository::SaveChat(capped_root, capped));
	UAM_ASSERT_EQ(ReadFile(capped_primary), capped_before);
}

UAM_TEST(LocalChatBundleNonemptyActiveQueueKeepsExistingConversionReadCeiling)
{
	TempDir temp("uam-bundle-nonempty-active-policy-ceiling");
	ChatSession active;
	active.id = "nonempty-active-conversion-ceiling";
	uam::AcpQueuedUserPromptState prompt;
	prompt.id = "large-authored-queue";
	prompt.text = std::string(128U * 1024U, 'q');
	active.acp_queued_prompts.push_back(prompt);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, active));
	const auto primary = AppPaths::UamChatFilePath(temp.root, active.id);
	UAM_ASSERT(!fs::exists(uam::chat_pages::IndexPath(primary)));
	const auto archive = nlohmann::json::parse(ReadFile(primary)).at("acp_queued_prompts");
	active.imported_read_only = true;
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, active));
	UAM_ASSERT_EQ(nlohmann::json::parse(ReadFile(primary)).at("acp_queued_prompts"), archive);
	const auto passive = ChatRepository::LoadLocalChat(temp.root, active.id);
	UAM_ASSERT(passive && passive->imported_read_only && passive->passive_queued_prompt_archive);
	UAM_ASSERT(passive->acp_queued_prompts.empty());
	UAM_ASSERT_EQ(nlohmann::json::parse(passive->passive_queued_prompt_archive->canonical_prompts_json), archive);
}
#endif
