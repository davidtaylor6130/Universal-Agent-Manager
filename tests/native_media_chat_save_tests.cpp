#include "test_harness.h"
#include "common/chat/native_media_chat_save.h"
#include "common/chat/native_attachment_assets.h"

#if defined(__APPLE__) || defined(_WIN32)
namespace
{
	const std::string kPreparedPng = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==";
	ChatSession MediaOwner(const std::filesystem::path& root)
	{
		ChatSession chat; chat.id = "admitted-UAM-media-owner"; chat.native_session_id = "separate-native-session";
		chat.provider_id = "gemini-cli"; chat.execution_host_id = "local"; chat.workspace_directory = root.string();
		chat.created_at = "2026-10-16T12:00:00Z"; chat.updated_at = chat.created_at;
		chat.messages.push_back(Message{MessageRole::User, "Original saved record"});
		UAM_ASSERT(ChatRepository::SaveChat(root, chat));
		return chat;
	}
	MessageAttachment PendingOriginal()
	{
		MessageAttachment a; a.id = "native-part"; a.name = "Recorded image"; a.kind = "image";
		a.source = AttachmentSource::NativeInlinePending; a.native_part_key = "v1:provider/session/message/part";
		a.native_tool_call_id = "recorded-tool"; a.native_source_field = "toolCalls/recorded-tool/result/0";
		a.origin_execution_host_id = "local"; std::string error;
		a.native_inline_payload = uam::native_assets::MakeInlinePayload(std::string(kPreparedPng), "image/png",
		    std::make_shared<uam::native_assets::PayloadBudget>(), error);
		UAM_ASSERT(a.native_inline_payload && error.empty());
		a.mime_type = a.native_inline_payload->MimeType(); a.content_digest = a.native_inline_payload->Digest();
		a.size_bytes = a.native_inline_payload->DecodedSize();
		return a;
	}
	std::string SavedRevision(const std::filesystem::path& root, const ChatSession& chat)
	{
		const auto revision = ChatRepository::ChatStorageRevision(root, chat.id);
		UAM_ASSERT(revision); return *revision;
	}
}

UAM_TEST(NativeMediaPreparedSavePublishesOriginalsOnlyWithActualPrimaryReceipt)
{
	uam_test::TempDir temp("uam-native-media-prepared-publish"); auto chat = MediaOwner(temp.root);
	const auto primary = AppPaths::UamChatFilePath(temp.root, chat.id); const auto before = uam_test::ReadFile(primary);
	const auto expected = PendingOriginal(); chat.messages.front().attachments.push_back(expected);
	std::string error;
	auto save = uam::PreparedNativeMediaChatSave::Prepare(temp.root, chat, SavedRevision(temp.root, chat), error);
	UAM_ASSERT(save && error.empty()); UAM_ASSERT_EQ(uam_test::ReadFile(primary), before);
	UAM_ASSERT(!save->PrimaryAcknowledged()); UAM_ASSERT(!save->AcknowledgeOriginals(error));
	UAM_ASSERT(!save->TakeCommittedChat());
	const auto& staged = save->Candidate().messages.front().attachments.front();
	UAM_ASSERT_EQ(staged.source, AttachmentSource::OwnedNativeAsset); UAM_ASSERT(!staged.native_inline_payload);
	UAM_ASSERT_EQ(staged.native_part_key, expected.native_part_key); UAM_ASSERT_EQ(staged.native_tool_call_id, expected.native_tool_call_id);
	UAM_ASSERT(staged.path.empty()); UAM_ASSERT(uam::native_assets::IsKeyForOwner(chat.id, staged.native_asset_key));
	UAM_ASSERT(!uam::native_assets::IsKeyForOwner(chat.native_session_id, staged.native_asset_key));
	UAM_ASSERT(save->Publish(error, [] { return true; })); UAM_ASSERT(save->PrimaryAcknowledged());
	UAM_ASSERT(save->AcknowledgeOriginals(error));
	auto committed = save->TakeCommittedChat(); UAM_ASSERT(committed); UAM_ASSERT(!save->TakeCommittedChat());
	UAM_ASSERT(!save->Publish(error));
	const auto loaded = ChatRepository::LoadLocalChat(temp.root, chat.id); UAM_ASSERT(loaded);
	const auto& a = loaded->messages.front().attachments.front();
	UAM_ASSERT_EQ(a, committed->messages.front().attachments.front());
	const auto original = uam::native_assets::ReadOwned(temp.root, chat.id, a.native_asset_key);
	std::string bytes; UAM_ASSERT(uam::base64::Decode(kPreparedPng, bytes));
	UAM_ASSERT(original.ok); UAM_ASSERT_EQ(original.bytes, bytes);
}

UAM_TEST(NativeMediaPreparedSaveReusesVerifiedOriginalAcrossRefresh)
{
	uam_test::TempDir temp("uam-native-media-reuse"); auto chat = MediaOwner(temp.root);
	chat.messages.front().attachments.push_back(PendingOriginal()); std::string error;
	auto first = uam::PreparedNativeMediaChatSave::Prepare(temp.root, chat, SavedRevision(temp.root, chat), error);
	UAM_ASSERT(first && first->Publish(error)); UAM_ASSERT(first->AcknowledgeOriginals(error));
	const auto key = first->Candidate().messages.front().attachments.front().native_asset_key;
	chat.messages.front().attachments.front().native_part_key = "v1:new-association-same-original";
	auto second = uam::PreparedNativeMediaChatSave::Prepare(temp.root, chat, SavedRevision(temp.root, chat), error);
	UAM_ASSERT(second); const auto& retained = second->Candidate().messages.front().attachments.front();
	UAM_ASSERT_EQ(retained.native_asset_key, key);
	UAM_ASSERT_EQ(retained.native_part_key, std::string("v1:new-association-same-original"));
	UAM_ASSERT(second->Publish(error)); UAM_ASSERT(second->AcknowledgeOriginals(error));
}

UAM_TEST(NativeMediaPreparedSaveRejectsStaleRevisionBeforeCreatingOriginals)
{
	uam_test::TempDir temp("uam-native-media-stale-prepare"); auto chat = MediaOwner(temp.root);
	const auto old = SavedRevision(temp.root, chat); auto changed = chat; changed.title = "New saved metadata";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, changed)); chat.messages.front().attachments.push_back(PendingOriginal());
	std::string error; UAM_ASSERT(!uam::PreparedNativeMediaChatSave::Prepare(temp.root, chat, old, error));
	UAM_ASSERT(!error.empty()); UAM_ASSERT(!std::filesystem::exists(temp.root / std::string(uam::native_assets::kStoreDirectory)));
}

UAM_TEST(NativeMediaPreparedSaveRetainsUnacknowledgedOriginalsOnRejectedPublication)
{
	uam_test::TempDir temp("uam-native-media-rejected-publish"); auto chat = MediaOwner(temp.root);
	const auto primary = AppPaths::UamChatFilePath(temp.root, chat.id); const auto before = uam_test::ReadFile(primary);
	chat.messages.front().attachments.push_back(PendingOriginal()); std::string error;
	auto save = uam::PreparedNativeMediaChatSave::Prepare(temp.root, chat, SavedRevision(temp.root, chat), error);
	UAM_ASSERT(save); const auto key = save->Candidate().messages.front().attachments.front().native_asset_key;
	UAM_ASSERT(!save->Publish(error, [] { return false; })); UAM_ASSERT(!save->PrimaryAcknowledged());
	UAM_ASSERT(!save->AcknowledgeOriginals(error)); UAM_ASSERT(!save->Publish(error));
	save.reset(); UAM_ASSERT_EQ(uam_test::ReadFile(primary), before);
	UAM_ASSERT(uam::native_assets::ReadOwned(temp.root, chat.id, key).ok);
}

UAM_TEST(NativeMediaPreparedSaveRejectsInconsistentPendingBeforeFilesystemMutation)
{
	for (int variant = 0; variant < 3; ++variant)
	{
		uam_test::TempDir temp("uam-native-media-invalid-pending"); auto chat = MediaOwner(temp.root);
		auto pending = PendingOriginal();
		if (variant == 0) pending.content_digest = std::string(64, '0');
		if (variant == 1) pending.origin_execution_host_id = "another-execution-host";
		if (variant == 2) pending.path = "/not-native-source-authority.png";
		chat.messages.front().attachments.push_back(std::move(pending)); std::string error;
		UAM_ASSERT(!uam::PreparedNativeMediaChatSave::Prepare(temp.root, chat, SavedRevision(temp.root, chat), error));
		UAM_ASSERT(!error.empty()); UAM_ASSERT(!std::filesystem::exists(temp.root / std::string(uam::native_assets::kStoreDirectory)));
	}
}

UAM_TEST(NativeMediaPreparedSaveCancellationDoesNotAcknowledgeOrCreateOriginals)
{
	uam_test::TempDir temp("uam-native-media-cancelled"); auto chat = MediaOwner(temp.root);
	chat.messages.front().attachments.push_back(PendingOriginal()); std::stop_source stopped; stopped.request_stop();
	std::string error;
	UAM_ASSERT(!uam::PreparedNativeMediaChatSave::Prepare(temp.root, chat, SavedRevision(temp.root, chat), error, stopped.get_token()));
	UAM_ASSERT(!error.empty()); UAM_ASSERT(!std::filesystem::exists(temp.root / std::string(uam::native_assets::kStoreDirectory)));
}

UAM_TEST(NativeMediaPreparedSaveRevalidatesStorageBeforePublication)
{
	uam_test::TempDir temp("uam-native-media-stale-publish"); auto chat = MediaOwner(temp.root);
	chat.messages.front().attachments.push_back(PendingOriginal()); std::string error;
	auto save = uam::PreparedNativeMediaChatSave::Prepare(temp.root, chat, SavedRevision(temp.root, chat), error); UAM_ASSERT(save);
	auto changed = *ChatRepository::LoadLocalChat(temp.root, chat.id); changed.title = "Changed during preparation";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, changed));
	const auto before = uam_test::ReadFile(AppPaths::UamChatFilePath(temp.root, chat.id));
	UAM_ASSERT(!save->Publish(error)); UAM_ASSERT(!save->AcknowledgeOriginals(error));
	UAM_ASSERT_EQ(uam_test::ReadFile(AppPaths::UamChatFilePath(temp.root, chat.id)), before);
}
#endif


#if defined(__APPLE__) || defined(_WIN32)
#include "common/provider/native_agent_history.h"
#include <map>

namespace
{
	using EmptyNativeIdFileTree = std::map<std::string, std::optional<std::string>>;
	EmptyNativeIdFileTree SnapshotEmptyNativeIdFileTree(const std::filesystem::path& root)
	{
		EmptyNativeIdFileTree snapshot;
		for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
		{
			UAM_ASSERT(!entry.is_symlink()); // Only this small private fixture tree.
			const auto relative = entry.path().lexically_relative(root).generic_string();
			if (entry.is_directory()) snapshot.emplace(relative, std::nullopt);
			else
			{
				UAM_ASSERT(entry.is_regular_file());
				snapshot.emplace(relative, uam_test::ReadFile(entry.path()));
			}
		}
		return snapshot;
	}
}

UAM_TEST(NativeMediaGenericPrepareRejectsEmptyNativeIdEvenForFabricatedPassiveReference)
{
	for (const bool fabricated_passive : {false, true})
	{
		uam_test::TempDir temp("uam-native-media-empty-id-boundary");
		auto chat = MediaOwner(temp.root);
		const auto original_primary = AppPaths::UamChatFilePath(temp.root, chat.id);
		const auto original_bytes = uam_test::ReadFile(original_primary);
		chat.native_session_id.clear();
		if (fabricated_passive)
		{
			const std::string parent = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
			chat.id = uam::native_agents::BuildHistoryId("gemini-cli", parent,
			    "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "local");
			UAM_ASSERT(!chat.id.empty());
			chat.imported_read_only = true;
			chat.parent_chat_id = parent;
			chat.branch_root_chat_id = chat.id;
			// A caller can save a passive-shaped record, but that is NOT a genuine
			// scoped native read and cannot substitute for the private admission.
			UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
		}
		const auto primary = AppPaths::UamChatFilePath(temp.root, chat.id);
		const auto before = uam_test::ReadFile(primary);
		const auto revision = SavedRevision(temp.root, chat);
		UAM_ASSERT(!before.empty() && !revision.empty() && revision != "missing");
		const auto pending = PendingOriginal();
		chat.messages.front().attachments.push_back(pending);
		UAM_ASSERT(chat.native_session_id.empty() && chat.messages_loaded);
		UAM_ASSERT(chat.messages.front().attachments.front().native_inline_payload);
		const auto files = SnapshotEmptyNativeIdFileTree(temp.root);
		std::string error;
		auto prepared = uam::PreparedNativeMediaChatSave::Prepare(temp.root, chat, revision, error);
		UAM_ASSERT(!prepared && !error.empty());
		UAM_ASSERT_EQ(SnapshotEmptyNativeIdFileTree(temp.root), files);
		UAM_ASSERT(!std::filesystem::exists(temp.root / std::string(uam::native_assets::kStoreDirectory)));
		UAM_ASSERT_EQ(uam_test::ReadFile(primary), before);
		UAM_ASSERT_EQ(SavedRevision(temp.root, chat), revision);
		UAM_ASSERT_EQ(uam_test::ReadFile(original_primary), original_bytes);
		UAM_ASSERT(chat.messages.front().attachments.front() == pending);
		UAM_ASSERT(chat.messages.front().attachments.front().native_inline_payload);
	}
}
#endif
