#include "test_harness.h"
#include "app/local_chat_bundle_service.h"
#include "cef/state_serializer.h"
#include "common/provider/native_agent_history.h"

using namespace uam_test;

namespace
{
	struct NativeLineageFixture
	{
		TempDir temp{"uam-native-child-lineage"};
		ChatSession parent;
		ChatSession child;
		std::string native_child_id;

		explicit NativeLineageFixture(const std::string& provider = "codex-cli", const std::string& host = "local")
		{
			parent.id = "actual-uam-parent";
			parent.provider_id = provider;
			parent.native_session_id = "550e8400-e29b-41d4-a716-446655440000";
			parent.execution_host_id = host;
			parent.workspace_directory = host == "local" ? temp.root.string() : "/workspace/owned";
			parent.created_at = parent.updated_at = "2026-10-16T10:00:00Z";
			child.id = "actual-uam-child";
			child.provider_id = provider;
			child.execution_host_id = host;
			child.workspace_directory = parent.workspace_directory;
			child.created_at = child.updated_at = "2026-10-16T10:01:00Z";
			child.branch_root_chat_id = "actual-branch-root";
			if (provider == "codex-cli") native_child_id = "660e8400-e29b-41d4-a716-446655440001";
			else if (provider == "opencode-cli") native_child_id = "ses_actual_child";
			else
			{
				native_child_id = uam::native_agents::BuildHistoryId(provider, parent.native_session_id, "actual-native-agent", host);
				child.id = native_child_id;
				child.imported_read_only = true;
				// Existing native journal correlation remains native metadata, not a UAM parent ID.
				child.parent_chat_id = parent.native_session_id;
			}
			if (!child.imported_read_only) child.native_session_id = native_child_id;
			Message owner{MessageRole::Assistant, "Delegated work"};
			ToolCall tool;
			tool.id = "actual-native-tool";
			tool.name = "native-delegation";
			tool.is_sub_agent = true;
			tool.sub_agent_id = native_child_id;
			tool.sub_agent_children_authoritative = provider == "codex-cli" || provider == "gemini-cli";
			if (tool.sub_agent_children_authoritative) tool.sub_agent_children.push_back({native_child_id, "Child", "completed"});
			owner.tool_calls.push_back(std::move(tool));
			parent.messages.push_back(std::move(owner));
			child.messages.push_back({MessageRole::Assistant, "Actual child transcript"});
		}

		bool Apply() { return ChatRepository::ApplyNativeChildDisplayLineage(parent, child, native_child_id); }
		ToolCall& Tool() { return parent.messages.front().tool_calls.front(); }
		void AssertReceipt(const ChatSession& chat) const
		{
			UAM_ASSERT_EQ(chat.subagent_parent_chat_id, parent.id);
			UAM_ASSERT_EQ(chat.subagent_parent_created_at, parent.created_at);
			UAM_ASSERT(chat.subagent_parent_chat_id != parent.native_session_id);
		}
	};
}

UAM_TEST(NativeChildLineagePersistsFiveProviderKinds)
{
	for (const std::string provider : {"codex-cli", "opencode-cli", "gemini-cli", "claude-cli", "copilot-cli"})
	{
		NativeLineageFixture fixture(provider);
		const auto native_id = fixture.child.native_session_id;
		const auto branch_parent = fixture.child.parent_chat_id;
		const auto branch_root = fixture.child.branch_root_chat_id;
		UAM_ASSERT(fixture.Apply());
		UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, fixture.child));
		const auto raw = nlohmann::json::parse(ReadFile(AppPaths::UamChatFilePath(fixture.temp.root, fixture.child.id)));
		UAM_ASSERT_EQ(raw.at("subagent_parent_chat_id"), nlohmann::json(fixture.parent.id));
		UAM_ASSERT_EQ(raw.at("subagent_parent_created_at"), nlohmann::json(fixture.parent.created_at));
		for (const bool messages : {false, true})
		{
			const auto loaded = ChatRepository::LoadLocalChat(fixture.temp.root, fixture.child.id, messages);
			UAM_ASSERT(loaded);
			fixture.AssertReceipt(*loaded);
			UAM_ASSERT_EQ(loaded->native_session_id, native_id);
			UAM_ASSERT_EQ(loaded->parent_chat_id, branch_parent);
			UAM_ASSERT_EQ(loaded->branch_root_chat_id, branch_root);
			UAM_ASSERT_EQ(loaded->imported_read_only, fixture.child.imported_read_only);
		}
		const auto summaries = ChatRepository::LoadLocalChatSummariesReadOnly(fixture.temp.root);
		UAM_ASSERT_EQ(summaries.size(), std::size_t{1});
		fixture.AssertReceipt(summaries.front());
		const auto bounded = ChatRepository::LoadLocalChatSummaryReadOnly(fixture.temp.root, fixture.child.id);
		UAM_ASSERT(bounded);
		fixture.AssertReceipt(*bounded);
	}
}

UAM_TEST(NativeChildLineageRequiresActualToolEvidence)
{
	for (const std::string provider : {"codex-cli", "opencode-cli", "gemini-cli", "claude-cli", "copilot-cli"})
	{
		NativeLineageFixture fixture(provider);
		fixture.Tool().sub_agent_children.clear();
		fixture.Tool().sub_agent_id.clear();
		fixture.Tool().args_json = nlohmann::json{{"sessionId", fixture.native_child_id}}.dump();
		fixture.Tool().result_text = fixture.native_child_id;
		fixture.Tool().sub_agent_title = fixture.native_child_id;
		fixture.child.title = fixture.native_child_id;
		UAM_ASSERT(!fixture.Apply());
		UAM_ASSERT(fixture.child.subagent_parent_chat_id.empty());
		UAM_ASSERT(fixture.child.subagent_parent_created_at.empty());
	}
}

UAM_TEST(NativeChildLineageCodexRequiresNormalizedReceiver)
{
	NativeLineageFixture fixture;
	fixture.Tool().sub_agent_children.clear();
	UAM_ASSERT(!fixture.Apply()); // A rejected authoritative list cannot use the still-matching scalar.
	fixture.Tool().sub_agent_children_authoritative = false;
	UAM_ASSERT(!fixture.Apply());
	fixture.Tool().sub_agent_children_authoritative = true;
	fixture.Tool().sub_agent_children = {{"invalid-thread-id", {}, {}}};
	UAM_ASSERT(!fixture.Apply());
	fixture.Tool().sub_agent_children = {{"urn:uuid:" + fixture.native_child_id, {}, {}}};
	UAM_ASSERT(fixture.Apply());
	fixture.AssertReceipt(fixture.child);
}

UAM_TEST(NativeChildLineageRejectsProviderHostWorkspaceAndSelfMismatch)
{
	for (int mismatch = 0; mismatch < 10; ++mismatch)
	{
		NativeLineageFixture fixture;
		if (mismatch == 0) fixture.child.provider_id = "opencode-cli";
		if (mismatch == 1) fixture.child.execution_host_id = "other-host";
		if (mismatch == 2) fixture.child.workspace_directory = (fixture.temp.root / "other-workspace").string();
		if (mismatch == 3) fixture.child.id = fixture.parent.id;
		if (mismatch == 4) fixture.child.native_session_id = fixture.parent.native_session_id;
		if (mismatch == 5) fixture.parent.created_at.clear();
		if (mismatch == 6) fixture.parent.workspace_directory.clear();
		if (mismatch == 7) fixture.parent.messages_loaded = false;
		if (mismatch == 8) fixture.parent.imported_read_only = true;
		if (mismatch == 9) fixture.parent.messages.front().role = MessageRole::User;
		UAM_ASSERT(!fixture.Apply());
		UAM_ASSERT(fixture.child.subagent_parent_chat_id.empty());
	}
}

UAM_TEST(NativeChildLineagePassiveReferenceRequiresExactNativeParent)
{
	for (const std::string provider : {"gemini-cli", "claude-cli", "copilot-cli"})
	for (int mismatch = 0; mismatch < 5; ++mismatch)
	{
		NativeLineageFixture fixture(provider, "owned-remote");
		if (mismatch == 0) fixture.parent.native_session_id = "different-native-parent";
		if (mismatch == 1) fixture.child.parent_chat_id = fixture.parent.id;
		if (mismatch == 2) fixture.child.native_session_id = fixture.native_child_id;
		if (mismatch == 3) fixture.child.imported_read_only = false;
		if (mismatch == 4) fixture.Tool().is_sub_agent = false;
		UAM_ASSERT(!fixture.Apply());
		UAM_ASSERT(fixture.child.subagent_parent_chat_id.empty());
	}
}

UAM_TEST(NativeChildLineageRemoteReferencesRemainPassive)
{
	for (const std::string provider : {"gemini-cli", "claude-cli", "copilot-cli"})
	{
		NativeLineageFixture fixture(provider, "owned-remote");
		UAM_ASSERT(fixture.Apply());
		UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, fixture.child));
		const auto loaded = ChatRepository::LoadLocalChat(fixture.temp.root, fixture.child.id);
		UAM_ASSERT(loaded);
		fixture.AssertReceipt(*loaded);
		UAM_ASSERT(loaded->native_session_id.empty());
		UAM_ASSERT(loaded->imported_read_only);
		UAM_ASSERT_EQ(loaded->execution_host_id, std::string("owned-remote"));
		uam::AppState app;
		app.data_root = fixture.temp.root;
		app.chats = {fixture.parent, *loaded};
		ChatDomainService().SelectChatById(app, loaded->id);
		for (const bool summary : {false, true})
		{
			const auto state = uam::StateSerializer::Serialize(app, summary);
			bool found = false;
			for (const auto& row : state.at("chats"))
			{
				if (row.at("id") != fixture.child.id) continue;
				found = true;
				UAM_ASSERT_EQ(row.at("workspaceDirectory"), nlohmann::json(fixture.parent.workspace_directory));
				UAM_ASSERT_EQ(row.at("subagentParentCreatedAt"), nlohmann::json(fixture.parent.created_at));
			}
			UAM_ASSERT(found);
		}
		UAM_ASSERT(uam::paths::ResolveWorkspaceRootPath(app, *loaded).empty());
	}
}

UAM_TEST(NativeChildLineageDoesNotReparentOrCrossParentBirth)
{
	NativeLineageFixture fixture;
	UAM_ASSERT(fixture.Apply());
	const auto receipt = fixture.child;
	UAM_ASSERT(fixture.Apply()); // Idempotent reopening of the same relationship.
	fixture.parent.created_at = "2026-10-16T11:00:00Z";
	UAM_ASSERT(!fixture.Apply());
	UAM_ASSERT_EQ(fixture.child.subagent_parent_created_at, receipt.subagent_parent_created_at);
	fixture.parent.created_at = receipt.subagent_parent_created_at;
	fixture.parent.id = "different-uam-parent";
	UAM_ASSERT(!fixture.Apply());
	UAM_ASSERT_EQ(fixture.child.subagent_parent_chat_id, receipt.subagent_parent_chat_id);
	fixture.child.subagent_parent_created_at.clear();
	UAM_ASSERT(!fixture.Apply()); // An incomplete prior receipt is not permission to transfer it.
}

UAM_TEST(NativeChildLineageHydrationCarriesLiveSummaryReceipt)
{
	NativeLineageFixture fixture;
	UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, fixture.child));
	auto summary = ChatRepository::LoadLocalChat(fixture.temp.root, fixture.child.id, false);
	UAM_ASSERT(summary && !summary->messages_loaded);
	summary->subagent_parent_chat_id = fixture.parent.id;
	summary->subagent_parent_created_at = fixture.parent.created_at;
	UAM_ASSERT(ChatRepository::HydrateChatMessages(fixture.temp.root, *summary));
	fixture.AssertReceipt(*summary);
	UAM_ASSERT_EQ(summary->messages, fixture.child.messages);
	// Detached transcript adoption must retain newer live metadata too.
	auto detached = ChatRepository::LoadLocalChat(fixture.temp.root, fixture.child.id, false);
	UAM_ASSERT(detached);
	ChatSession live = *detached;
	live.subagent_parent_chat_id = fixture.parent.id;
	live.subagent_parent_created_at = fixture.parent.created_at;
	UAM_ASSERT(ChatRepository::HydrateChatMessages(fixture.temp.root, *detached));
	UAM_ASSERT(ChatRepository::AdoptHydratedMessagesIfUnchanged(live, std::move(*detached), live.persisted_message_count, live.persisted_messages_digest));
	fixture.AssertReceipt(live);
}

UAM_TEST(NativeChildLineageUnloadedSaveAndPageKeepReceipt)
{
	NativeLineageFixture fixture("claude-cli");
	UAM_ASSERT(fixture.Apply());
	UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, fixture.child));
	auto summary = ChatRepository::LoadLocalChat(fixture.temp.root, fixture.child.id, false);
	UAM_ASSERT(summary && !summary->messages_loaded);
	summary->title = "Renamed child";
	UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, *summary));
	const auto loaded = ChatRepository::LoadLocalChat(fixture.temp.root, fixture.child.id);
	UAM_ASSERT(loaded);
	fixture.AssertReceipt(*loaded);
	UAM_ASSERT_EQ(loaded->messages, fixture.child.messages);
	const auto page = ChatRepository::ReadMessagePage(fixture.temp.root, *summary, 10);
	UAM_ASSERT(page);
	fixture.AssertReceipt(page->chat);
	UAM_ASSERT_EQ(page->chat.messages, fixture.child.messages);
	summary->last_opened_at = "2026-10-16T12:00:00Z";
	UAM_ASSERT(ChatRepository::SaveLastOpenedAt(fixture.temp.root, *summary));
	const auto reopened = ChatRepository::LoadLocalChat(fixture.temp.root, fixture.child.id, false);
	UAM_ASSERT(reopened);
	fixture.AssertReceipt(*reopened);
}

UAM_TEST(NativeChildLineageNativeOverlayKeepsSavedReceipt)
{
	// Requires root's coordinated two-field OverlayLocalChatState integration;
	// this worker does not edit the frozen runtime orchestration source.
	for (const std::string provider : {"codex-cli", "opencode-cli", "gemini-cli", "claude-cli", "copilot-cli"})
	{
		NativeLineageFixture fixture(provider);
		UAM_ASSERT(fixture.Apply());
		UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, fixture.child));
		const auto saved = ChatRepository::LoadLocalChat(fixture.temp.root, fixture.child.id);
		UAM_ASSERT(saved);
		ChatSession native = fixture.child;
		native.subagent_parent_chat_id.clear();
		native.subagent_parent_created_at.clear();
		ChatHistorySyncService().OverlayPreparedLocalHistory(*saved, native);
		fixture.AssertReceipt(native);
		UAM_ASSERT_EQ(native.native_session_id, fixture.child.native_session_id);
		UAM_ASSERT_EQ(native.imported_read_only, fixture.child.imported_read_only);
	}
}

UAM_TEST(NativeChildLineageFrontendAndStorageFingerprintsTrackBothFields)
{
	NativeLineageFixture fixture;
	const auto original = ChatRepository::ChatMetadataFingerprint(fixture.temp.root, fixture.child);
	const auto queue_owner = ChatRepository::QueueStorageOwner(fixture.child);
	const auto admission = ChatRepository::QueueAdmissionFingerprint(fixture.child);
	UAM_ASSERT(fixture.Apply());
	UAM_ASSERT(original != ChatRepository::ChatMetadataFingerprint(fixture.temp.root, fixture.child));
	UAM_ASSERT_EQ(queue_owner, ChatRepository::QueueStorageOwner(fixture.child));
	UAM_ASSERT_EQ(admission, ChatRepository::QueueAdmissionFingerprint(fixture.child));
	uam::AppState app;
	app.data_root = fixture.temp.root;
	app.chats = {fixture.parent, fixture.child};
	ChatDomainService().SelectChatById(app, fixture.child.id);
	for (const bool summary : {false, true})
	{
		const auto state = uam::StateSerializer::Serialize(app, summary);
		bool found = false;
		for (const auto& chat : state.at("chats"))
		{
			if (chat.at("id") != fixture.child.id) continue;
			found = true;
			UAM_ASSERT_EQ(chat.at("subagentParentChatId"), nlohmann::json(fixture.parent.id));
			UAM_ASSERT_EQ(chat.at("subagentParentCreatedAt"), nlohmann::json(fixture.parent.created_at));
		}
		UAM_ASSERT(found);
	}
	const auto initial = uam::StateSerializer::SerializeFingerprint(app);
	const auto stored = ChatRepository::ChatMetadataFingerprint(fixture.temp.root, app.chats.back());
	app.chats.back().subagent_parent_created_at += "-changed";
	UAM_ASSERT(initial != uam::StateSerializer::SerializeFingerprint(app));
	UAM_ASSERT(stored != ChatRepository::ChatMetadataFingerprint(fixture.temp.root, app.chats.back()));
	const auto changed_birth = uam::StateSerializer::SerializeFingerprint(app);
	const auto changed_birth_storage = ChatRepository::ChatMetadataFingerprint(fixture.temp.root, app.chats.back());
	app.chats.back().subagent_parent_chat_id = "other-parent";
	UAM_ASSERT(changed_birth != uam::StateSerializer::SerializeFingerprint(app));
	UAM_ASSERT(changed_birth_storage != ChatRepository::ChatMetadataFingerprint(fixture.temp.root, app.chats.back()));
	const auto full = uam::StateSerializer::SerializeSession(fixture.child);
	UAM_ASSERT_EQ(full.at("subagentParentChatId"), nlohmann::json(fixture.parent.id));
	UAM_ASSERT_EQ(full.at("subagentParentCreatedAt"), nlohmann::json(fixture.parent.created_at));
}

UAM_TEST(NativeChildLineageRejectedPublicationDoesNotChangeSavedReceipt)
{
	NativeLineageFixture fixture;
	UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, fixture.child));
	const auto revision = ChatRepository::ChatStorageRevision(fixture.temp.root, fixture.child.id);
	UAM_ASSERT(revision);
	UAM_ASSERT(fixture.Apply());
	const auto prepared = ChatRepository::PrepareChatSave(fixture.temp.root, fixture.child, *revision);
	UAM_ASSERT(prepared);
	ChatSession concurrent = fixture.child;
	concurrent.subagent_parent_chat_id.clear();
	concurrent.subagent_parent_created_at.clear();
	concurrent.title = "Newer saved metadata";
	UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, concurrent));
	UAM_ASSERT(!ChatRepository::PublishPreparedChatSave(prepared, true, &fixture.child));
	const auto loaded = ChatRepository::LoadLocalChat(fixture.temp.root, fixture.child.id);
	UAM_ASSERT(loaded);
	UAM_ASSERT(loaded->subagent_parent_chat_id.empty() && loaded->subagent_parent_created_at.empty());
	UAM_ASSERT_EQ(loaded->title, concurrent.title);
	UAM_ASSERT_EQ(loaded->messages, concurrent.messages);
}

UAM_TEST(NativeChildLineageLegacyDocumentsDoNotInferParent)
{
	NativeLineageFixture fixture;
	UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, fixture.child));
	const auto path = AppPaths::UamChatFilePath(fixture.temp.root, fixture.child.id);
	auto raw = nlohmann::json::parse(ReadFile(path));
	raw.erase("subagent_parent_chat_id");
	raw.erase("subagent_parent_created_at");
	raw["parent_chat_id"] = fixture.parent.id;
	UAM_ASSERT(uam::io::WriteTextFile(path, raw.dump(2)));
	const auto loaded = ChatRepository::LoadLocalChat(fixture.temp.root, fixture.child.id, true, nullptr, false);
	UAM_ASSERT(loaded);
	UAM_ASSERT(loaded->subagent_parent_chat_id.empty() && loaded->subagent_parent_created_at.empty());
	UAM_ASSERT_EQ(loaded->parent_chat_id, fixture.parent.id);
}

UAM_TEST(NativeChildLineageBundleImportDetachesSourceParentOnCollision)
{
	NativeLineageFixture fixture("opencode-cli");
	UAM_ASSERT(fixture.Apply());
	const auto source = fixture.temp.root / "source";
	const auto target = fixture.temp.root / "target";
	const auto bundle = fixture.temp.root / "bundle";
	UAM_ASSERT(ChatRepository::SaveChat(source, fixture.parent));
	UAM_ASSERT(ChatRepository::SaveChat(source, fixture.child));
	UAM_ASSERT(ChatRepository::SaveChat(target, fixture.parent));
	UAM_ASSERT(ChatRepository::SaveChat(target, fixture.child));
	const auto before = ReadFile(AppPaths::UamChatFilePath(source, fixture.child.id));
	UAM_ASSERT(uam::LocalChatBundleService::Export(source, bundle).ok);
	UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(source, fixture.child.id)), before);
	const auto imported = uam::LocalChatBundleService::Import(target, bundle);
	UAM_ASSERT(imported.ok && imported.renamed_count == 2);
	for (const auto& item : imported.items)
	{
		const auto copy = ChatRepository::LoadLocalChat(target, item.imported_id);
		UAM_ASSERT(copy && copy->imported_read_only && copy->native_session_id.empty());
		UAM_ASSERT(copy->subagent_parent_chat_id.empty() && copy->subagent_parent_created_at.empty());
	}
	const auto original = ChatRepository::LoadLocalChat(target, fixture.child.id);
	UAM_ASSERT(original);
	fixture.AssertReceipt(*original);
}

UAM_TEST(NativeChildLineageMessageBranchStartsWithoutReceipt)
{
	NativeLineageFixture fixture;
	UAM_ASSERT(fixture.Apply());
	fixture.child.workspace_isolation_kind = "none";
	fixture.child.messages = {{MessageRole::User, "Branch this user turn"}, {MessageRole::Assistant, "Saved answer"}};
	uam::AppState app;
	app.data_root = fixture.temp.root;
	app.chats.push_back(fixture.child);
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	UAM_ASSERT(ChatDomainService().CreateBranchFromMessage(app, fixture.child.id, 0));
	const ChatSession* branch = ChatDomainService().SelectedChat(app);
	UAM_ASSERT(branch && branch->id != fixture.child.id);
	UAM_ASSERT_EQ(branch->parent_chat_id, fixture.child.id);
	UAM_ASSERT(branch->subagent_parent_chat_id.empty() && branch->subagent_parent_created_at.empty());
	const auto source = ChatDomainService().FindChatById(app, fixture.child.id);
	UAM_ASSERT(source);
	fixture.AssertReceipt(*source);
}

// NCL-R1-01 regression-only append: preserve the original fifteen-case prefix.
namespace
{
	void AssertNativeParentBirthInCef(const fs::path& data_root, const ChatSession& parent, const ChatSession& child, const std::string& expected_birth)
	{
		uam::AppState app;
		app.data_root = data_root;
		app.chats = {parent, child};
		ChatDomainService().SelectChatById(app, parent.id);
		for (const bool summary : {false, true})
		{
			const auto state = uam::StateSerializer::Serialize(app, summary);
			const auto& rows = state.at("chats");
			const auto parent_row = std::ranges::find_if(rows, [&](const nlohmann::json& row) { return row.at("id") == parent.id; });
			const auto child_row = std::ranges::find_if(rows, [&](const nlohmann::json& row) { return row.at("id") == child.id; });
			UAM_ASSERT(parent_row != rows.end() && child_row != rows.end());
			UAM_ASSERT_EQ(parent_row->at("createdAt"), nlohmann::json(expected_birth));
			UAM_ASSERT_EQ(child_row->at("subagentParentChatId"), parent_row->at("id"));
			UAM_ASSERT_EQ(child_row->at("subagentParentCreatedAt"), parent_row->at("createdAt"));
			UAM_ASSERT_EQ(child_row->at("providerId"), parent_row->at("providerId"));
			UAM_ASSERT_EQ(child_row->at("executionHostId"), parent_row->at("executionHostId"));
			UAM_ASSERT_EQ(child_row->at("workspaceDirectory"), parent_row->at("workspaceDirectory"));
		}
		const auto fingerprint = uam::StateSerializer::SerializeFingerprint(app);
		const auto& rows = fingerprint.at("chats");
		const auto parent_row = std::ranges::find_if(rows, [&](const nlohmann::json& row) { return row.at("id") == parent.id; });
		const auto child_row = std::ranges::find_if(rows, [&](const nlohmann::json& row) { return row.at("id") == child.id; });
		UAM_ASSERT(parent_row != rows.end() && child_row != rows.end());
		UAM_ASSERT_EQ(parent_row->at("createdAt"), nlohmann::json(expected_birth));
		UAM_ASSERT_EQ(child_row->at("subagentParentCreatedAt"), parent_row->at("createdAt"));
	}
}

UAM_TEST(NativeChildLineageParentPreparedRefreshPreservesUamBirth)
{
	NativeLineageFixture fixture;
	fixture.parent.messages.front().created_at = fixture.parent.created_at;
	UAM_ASSERT(fixture.Apply());
	UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, fixture.parent));
	UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, fixture.child));
	const auto child_bytes = ReadFile(AppPaths::UamChatFilePath(fixture.temp.root, fixture.child.id));
	const auto saved_parent = ChatRepository::LoadLocalChat(fixture.temp.root, fixture.parent.id);
	UAM_ASSERT(saved_parent);
	ChatSession native = fixture.parent;
	native.id = fixture.parent.native_session_id;
	native.created_at = "2026-10-16T10:00:05Z"; // Provider birth B differs from this UAM record's birth A.
	native.updated_at = "2026-10-16T10:05:00Z";
	native.messages.push_back({MessageRole::Assistant, "Newer native parent answer", native.updated_at});
	UAM_ASSERT(!native.created_at.empty() && native.created_at != fixture.parent.created_at);
	UAM_ASSERT(native.updated_at > saved_parent->updated_at && native.messages.size() > saved_parent->messages.size());
	UAM_ASSERT_EQ(native.native_session_id, saved_parent->native_session_id);
	UAM_ASSERT_EQ(native.provider_id, saved_parent->provider_id);
	UAM_ASSERT_EQ(native.execution_host_id, saved_parent->execution_host_id);
	UAM_ASSERT_EQ(native.workspace_directory, saved_parent->workspace_directory);
	ChatHistorySyncService().OverlayPreparedLocalHistory(*saved_parent, native);
	UAM_ASSERT_EQ(native.id, fixture.parent.id);
	UAM_ASSERT_EQ(native.messages.size(), std::size_t{2});
	UAM_ASSERT_EQ(native.messages.back().content, std::string("Newer native parent answer"));
	UAM_ASSERT(ChatRepository::SaveChat(fixture.temp.root, native));
	const auto reloaded_parent = ChatRepository::LoadLocalChat(fixture.temp.root, fixture.parent.id);
	auto reloaded_child = ChatRepository::LoadLocalChat(fixture.temp.root, fixture.child.id);
	UAM_ASSERT(reloaded_parent && reloaded_child);
	UAM_ASSERT_EQ(reloaded_parent->messages.back().content, std::string("Newer native parent answer"));
	UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(fixture.temp.root, fixture.child.id)), child_bytes);
	fixture.AssertReceipt(*reloaded_child);
	// Intended first product failure before root's fix: the persisted parent is B.
	UAM_ASSERT_EQ(reloaded_parent->created_at, fixture.parent.created_at);
	UAM_ASSERT_EQ(reloaded_parent->id, reloaded_child->subagent_parent_chat_id);
	UAM_ASSERT_EQ(reloaded_parent->created_at, reloaded_child->subagent_parent_created_at);
	AssertNativeParentBirthInCef(fixture.temp.root, *reloaded_parent, *reloaded_child, fixture.parent.created_at);
	UAM_ASSERT(ChatRepository::ApplyNativeChildDisplayLineage(*reloaded_parent, *reloaded_child, fixture.native_child_id));
	fixture.AssertReceipt(*reloaded_child);
}

UAM_TEST(NativeChildLineageParentNativeImportPreservesUamBirth)
{
	NativeLineageFixture fixture("gemini-cli");
	const fs::path data_root = fixture.temp.root / "data";
	const fs::path native_directory = fixture.temp.root / "explicit-native-source";
	const std::string child_agent = "660e8400-e29b-41d4-a716-446655440001";
	fixture.native_child_id = uam::native_agents::BuildHistoryId("gemini-cli", fixture.parent.native_session_id, child_agent, "local");
	fixture.child.id = fixture.native_child_id;
	fixture.child.branch_root_chat_id = fixture.child.id;
	fixture.parent.messages.front().created_at = fixture.parent.created_at;
	fixture.Tool().name = "delegate_to_agent";
	fixture.Tool().status = "success";
	fixture.Tool().sub_agent_id = fixture.native_child_id;
	fixture.Tool().sub_agent_children = {{fixture.native_child_id, "Child", "completed"}};
	ChatFolder folder{"lineage-workspace", "Lineage workspace", fixture.parent.workspace_directory, false};
	fixture.parent.folder_id = fixture.child.folder_id = folder.id;
	UAM_ASSERT(fixture.Apply());
	UAM_ASSERT(ChatRepository::SaveChat(data_root, fixture.parent));
	UAM_ASSERT(ChatRepository::SaveChat(data_root, fixture.child));
	const auto child_bytes = ReadFile(AppPaths::UamChatFilePath(data_root, fixture.child.id));
	const std::string provider_birth = "2026-10-16T10:00:05Z";
	const std::string native_update = "2026-10-16T10:05:00Z";
	const nlohmann::json transcript = {
	    {"sessionId", fixture.parent.native_session_id}, {"startTime", provider_birth}, {"lastUpdated", native_update},
	    {"messages", nlohmann::json::array({
	        {{"type", "gemini"}, {"timestamp", provider_birth}, {"content", "Delegated work"},
	            {"toolCalls", nlohmann::json::array({{{"id", "actual-native-tool"}, {"name", "delegate_to_agent"},
	                {"status", "success"}, {"agentId", child_agent}, {"resultDisplay", {{"agentName", "Child"}, {"state", "completed"}}}}})}},
	        {{"type", "gemini"}, {"timestamp", native_update}, {"content", "Imported newer native parent answer"}}
	    })}
	};
	UAM_ASSERT(uam::io::WriteTextFile(native_directory / "native-parent.json", transcript.dump(2)));
	// Use the real native JSON loader through its public application boundary;
	// bind Gemini's workspace-less document to this explicit caller's known scope.
	ChatHistorySyncService history;
	auto loaded = history.LoadNativeSessionChats(native_directory, ProviderProfileStore::DefaultGeminiProfile());
	UAM_ASSERT_EQ(loaded.size(), std::size_t{1});
	ChatSession& incoming = loaded.front();
	UAM_ASSERT_EQ(incoming.created_at, provider_birth);
	UAM_ASSERT_EQ(incoming.id, fixture.parent.native_session_id);
	UAM_ASSERT_EQ(incoming.native_session_id, fixture.parent.native_session_id);
	UAM_ASSERT(incoming.created_at != fixture.parent.created_at && incoming.updated_at > fixture.parent.updated_at);
	UAM_ASSERT_EQ(incoming.messages.size(), std::size_t{2});
	UAM_ASSERT_EQ(incoming.messages.front().tool_calls.size(), std::size_t{1});
	UAM_ASSERT(incoming.messages.front().tool_calls.front().is_sub_agent);
	UAM_ASSERT_EQ(incoming.messages.front().tool_calls.front().sub_agent_id, fixture.native_child_id);
	incoming.workspace_directory = folder.directory;
	incoming.folder_id = folder.id;
	UAM_ASSERT_EQ(incoming.provider_id, fixture.parent.provider_id);
	UAM_ASSERT_EQ(incoming.execution_host_id, fixture.parent.execution_host_id);
	UAM_ASSERT_EQ(incoming.workspace_directory, fixture.parent.workspace_directory);
	const auto persisted_parent = ChatRepository::LoadLocalChat(data_root, fixture.parent.id);
	const auto persisted_child = ChatRepository::LoadLocalChat(data_root, fixture.child.id);
	UAM_ASSERT(persisted_parent && persisted_child);
	uam::AppState app;
	app.data_root = data_root;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	app.folders.push_back(folder);
	app.chats = {*persisted_parent, *persisted_child};
	ChatDomainService().SelectChatById(app, fixture.parent.id);
	UAM_ASSERT(app.acp_sessions.empty() && app.cli_terminals.empty());
	UAM_ASSERT(!uam::strings::StartsWith(fixture.parent.id, "chat-")); // This saved catalogue record is eligible for native reconciliation.
	ChatHistorySyncService::LocalHistoryDiscovery discovery;
	discovery.chats = std::move(loaded);
	const auto imported = history.ImportDiscoveredProviderChatsForFolder(app, folder, std::move(discovery));
	UAM_ASSERT(imported.success);
	UAM_ASSERT_EQ(imported.imported_count, 1);
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{2});
	const ChatSession* live_parent = ChatDomainService().FindChatById(app, fixture.parent.id);
	const ChatSession* live_child = ChatDomainService().FindChatById(app, fixture.child.id);
	UAM_ASSERT(live_parent && live_child);
	UAM_ASSERT_EQ(live_parent->native_session_id, fixture.parent.native_session_id);
	UAM_ASSERT_EQ(live_parent->messages.size(), std::size_t{2});
	UAM_ASSERT_EQ(live_parent->messages.back().content, std::string("Imported newer native parent answer"));
	const auto reloaded_parent = ChatRepository::LoadLocalChat(data_root, fixture.parent.id);
	auto reloaded_child = ChatRepository::LoadLocalChat(data_root, fixture.child.id);
	UAM_ASSERT(reloaded_parent && reloaded_child);
	UAM_ASSERT_EQ(reloaded_parent->messages.back().content, std::string("Imported newer native parent answer"));
	UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatFilePath(data_root, fixture.child.id)), child_bytes);
	fixture.AssertReceipt(*live_child);
	fixture.AssertReceipt(*reloaded_child);
	// Intended first product failure before root's fix: the imported saved parent is B.
	UAM_ASSERT_EQ(reloaded_parent->created_at, fixture.parent.created_at);
	UAM_ASSERT_EQ(live_parent->created_at, fixture.parent.created_at);
	UAM_ASSERT_EQ(reloaded_parent->id, fixture.parent.id);
	UAM_ASSERT_EQ(reloaded_parent->id, reloaded_child->subagent_parent_chat_id);
	UAM_ASSERT_EQ(reloaded_parent->created_at, reloaded_child->subagent_parent_created_at);
	AssertNativeParentBirthInCef(data_root, *reloaded_parent, *reloaded_child, fixture.parent.created_at);
	UAM_ASSERT(ChatRepository::ApplyNativeChildDisplayLineage(*reloaded_parent, *reloaded_child, fixture.native_child_id));
	fixture.AssertReceipt(*reloaded_child);
}
