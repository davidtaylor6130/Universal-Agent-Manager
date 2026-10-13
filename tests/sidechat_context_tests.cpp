#include "test_harness.h"
#include "app/chat_lifecycle_service.h"
#include "common/paths/app_paths.h"
#include "common/chat/chat_message_page_index.h"
#include "common/memory/memory_policy.h"
#include "common/utils/io_utils.h"

#include <chrono>
#include <future>
#include <thread>

using namespace uam_test;

namespace
{
	ChatSession PersistDetachedSideChatParent(const TempDir& temp)
	{
		ChatSession parent;
		parent.id = "side-context-parent";
		parent.provider_id = "codex-cli";
		parent.native_session_id = "thread-parent";
		parent.workspace_directory = temp.root.string();
		parent.model_id = "parent-model";
		parent.created_at = "2026-10-13T00:00:00.000Z";
		parent.updated_at = parent.created_at;
		parent.workspace_isolation_kind = "worktree";
		for (int index = 0; index < 100; ++index)
			parent.messages.push_back({MessageRole::User, "Context " + std::to_string(index) + " " + std::string(1024, 'x'), "message-" + std::to_string(index)});
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, parent));
		const std::optional<ChatSession> metadata = ChatRepository::LoadLocalChat(temp.root, parent.id, false);
		UAM_ASSERT(metadata.has_value());
		UAM_ASSERT(!metadata->messages_loaded);
		return *metadata;
	}
}

UAM_TEST(TemporarySideChatReferenceClipsRecentTextAndPreservesUtf8)
{
	std::vector<Message> messages;
	messages.push_back({MessageRole::User, "Very old context"});
	for (int index = 0; index < 48; ++index) messages.push_back({MessageRole::Assistant, ""});
	std::string large;
	for (int index = 0; index < 40000; ++index) large += "🙂";
	large += "recent-tail";
	messages.push_back({MessageRole::Assistant, large});
	const std::string context = uam::BuildTemporarySideChatContext(messages);
	UAM_ASSERT(context.size() <= 32768);
	UAM_ASSERT(context.find("recent-tail") != std::string::npos);
	UAM_ASSERT(context.find("Very old context") == std::string::npos);
	UAM_ASSERT(context.find("[Earlier text omitted]") != std::string::npos);
	UAM_ASSERT(!nlohmann::json(context).dump().empty());
}

UAM_TEST(TemporarySideChatDetachedPreparationKeepsParentUnhydrated)
{
	TempDir temp("side-context-detached");
	uam::AppState app;
	app.data_root = temp.root;
	app.chats.push_back(PersistDetachedSideChatParent(temp));
	const std::string digest = app.chats.front().persisted_messages_digest;
	const std::optional<uam::TemporarySideChatContext> prepared = uam::PrepareTemporarySideChatContext(temp.root, app.chats.front());
	UAM_ASSERT(prepared.has_value());
	UAM_ASSERT(prepared->page.chat.messages.empty());
	UAM_ASSERT(prepared->reference.size() <= 32768);
	UAM_ASSERT(prepared->reference.find("Context 99 ") != std::string::npos);
	UAM_ASSERT(prepared->reference.find("Context 0 ") == std::string::npos);
	std::string id;
	UAM_ASSERT(uam::CreateTemporarySideChat(app, app.chats.front().id, &id, &*prepared));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(2));
	UAM_ASSERT(!app.chats.front().messages_loaded);
	UAM_ASSERT(app.chats.front().messages.empty());
	UAM_ASSERT_EQ(app.chats.front().persisted_messages_digest, digest);
	UAM_ASSERT_EQ(app.chats.back().temporary_parent_chat_id, app.chats.front().id);
	UAM_ASSERT_EQ(app.chats.back().model_id, std::string("parent-model"));
	UAM_ASSERT(app.chats.back().native_session_id.empty());
	UAM_ASSERT(app.chats.back().workspace_isolation_kind.empty());
	UAM_ASSERT(!app.chats.back().memory_enabled);
	UAM_ASSERT_EQ(app.chats.back().provider_handoff_context, prepared->reference);
}

UAM_TEST(TemporarySideChatPreparedContextRejectsChangedParentScope)
{
	TempDir temp("side-context-scope");
	const ChatSession parent = PersistDetachedSideChatParent(temp);
	const std::optional<uam::TemporarySideChatContext> prepared = uam::PrepareTemporarySideChatContext(temp.root, parent);
	UAM_ASSERT(prepared.has_value());
	for (int change = 0; change < 5; ++change)
	{
		uam::AppState app;
		app.data_root = temp.root;
		app.chats.push_back(parent);
		if (change == 0) app.chats.front().provider_id = "gemini-cli";
		if (change == 1) app.chats.front().execution_host_id = "other-host";
		if (change == 2) app.chats.front().workspace_directory += "/changed";
		if (change == 3) app.chats.front().folder_id = "changed-folder";
		if (change == 4) app.chats.front().native_session_id = "changed-thread";
		std::string id;
		UAM_ASSERT(!uam::CreateTemporarySideChat(app, parent.id, &id, &*prepared));
		UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
		UAM_ASSERT(id.empty());
	}
}

UAM_TEST(TemporarySideChatPreparedContextRejectsChangedPrimaryRevision)
{
	TempDir temp("side-context-revision");
	uam::AppState app;
	app.data_root = temp.root;
	app.chats.push_back(PersistDetachedSideChatParent(temp));
	const std::optional<uam::TemporarySideChatContext> prepared = uam::PrepareTemporarySideChatContext(temp.root, app.chats.front());
	UAM_ASSERT(prepared.has_value());
	const std::optional<ChatSession> original = ChatRepository::LoadLocalChat(temp.root, app.chats.front().id);
	UAM_ASSERT(original.has_value());
	ChatSession changed = *original;
	changed.messages.back().content = "New context with same message count";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, changed));
	std::string id;
	UAM_ASSERT(!uam::CreateTemporarySideChat(app, app.chats.front().id, &id, &*prepared));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(id.empty());
}

UAM_TEST(TemporarySideChatDirectCreationCannotHydrateDetachedHistory)
{
	TempDir temp("side-context-direct");
	uam::AppState app;
	app.data_root = temp.root;
	app.chats.push_back(PersistDetachedSideChatParent(temp));
	std::string id;
	UAM_ASSERT(!uam::CreateTemporarySideChat(app, app.chats.front().id, &id));
	UAM_ASSERT(!app.chats.front().messages_loaded);
	UAM_ASSERT(app.chats.front().messages.empty());
	UAM_ASSERT(id.empty());
}

UAM_TEST(TemporarySideChatDetachedPreparationHonorsCancellation)
{
	TempDir temp("side-context-cancelled");
	const ChatSession parent = PersistDetachedSideChatParent(temp);
	UAM_ASSERT(!uam::PrepareTemporarySideChatContext(temp.root, parent, []() { return true; }).has_value());
	UAM_ASSERT(!parent.messages_loaded);
	UAM_ASSERT(parent.messages.empty());
}

UAM_TEST(TemporarySideChatCreationDoesNotWaitForUnrelatedPublication)
{
	TempDir temp("side-context-lock-timing");
	ChatSession blocker;
	blocker.id = "side-context-unrelated-publication";
	const std::shared_ptr<PreparedChatSave> publication = ChatRepository::PrepareChatSave(temp.root, blocker);
	UAM_ASSERT(publication != nullptr);
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession parent;
	parent.id = "side-context-timing-parent";
	parent.provider_id = "codex-cli";
	parent.messages_loaded = true;
	app.chats.push_back(parent);
	const std::optional<ChatSession> candidate = uam::BuildTemporarySideChatCandidate(app, parent.id);
	UAM_ASSERT(candidate.has_value());
	std::string revision;
	UAM_ASSERT(ChatRepository::SaveChatIfAbsent(temp.root, *candidate, &revision));
	std::promise<void> locked;
	std::future<void> ready = locked.get_future();
	std::jthread holder([&]()
	{
		(void)ChatRepository::PublishPreparedChatSave(publication, true, nullptr, [&]()
		{
			locked.set_value();
			std::this_thread::sleep_for(std::chrono::milliseconds(350));
			return true;
		});
	});
	ready.wait();
	const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	const std::optional<ChatSession> next = uam::BuildTemporarySideChatCandidate(app, parent.id);
	const bool created = uam::AdoptTemporarySideChat(app, *candidate, temp.root, revision, parent.metadata_incarnation);
	const std::chrono::steady_clock::duration elapsed = std::chrono::steady_clock::now() - start;
	holder.join();
	UAM_ASSERT(next.has_value());
	UAM_ASSERT(created);
	UAM_ASSERT(elapsed < std::chrono::milliseconds(100));

}

UAM_TEST(TemporarySideChatStaleCleanupCannotDeleteReplacement)
{
	TempDir temp("side-context-guarded-cleanup");
	ChatSession child;
	child.id = "side-context-owned-child";
	child.temporary_parent_chat_id = "side-context-owner-parent";
	std::string original;
	UAM_ASSERT(ChatRepository::SaveChatIfAbsent(temp.root, child, &original));
	UAM_ASSERT(!ChatRepository::DeleteTemporaryChatIfRevision(temp.root, child.id, "foreign-parent", original));
	child.title = "Replacement retained";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, child));
	UAM_ASSERT(!ChatRepository::DeleteTemporaryChatIfRevision(temp.root, child.id, child.temporary_parent_chat_id, original));
	const std::optional<ChatSession> replacement = ChatRepository::LoadLocalChat(temp.root, child.id, false);
	UAM_ASSERT(replacement && replacement->title == "Replacement retained");
	UAM_ASSERT(ChatRepository::DeleteChatStorageFiles(temp.root, child.id).Failed() == false);
	std::string current;
	UAM_ASSERT(ChatRepository::SaveChatIfAbsent(temp.root, child, &current));
	UAM_ASSERT(ChatRepository::DeleteTemporaryChatIfRevision(temp.root, child.id, child.temporary_parent_chat_id, current));
	UAM_ASSERT(!ChatRepository::LoadLocalChat(temp.root, child.id, false));
}

UAM_TEST(TemporarySideChatSaveReceiptRejectsSameTimestampReplacement)
{
	TempDir temp("side-context-exact-receipt");
	ChatSession child;
	child.id = "side-context-replacement-child";
	child.title = "Original child";
	child.temporary_parent_chat_id = "side-context-owner-parent";
	std::string receipt;
	UAM_ASSERT(ChatRepository::SaveChatIfAbsent(temp.root, child, &receipt));
	UAM_ASSERT(ChatRepository::MatchesTemporaryChatSaveReceipt(temp.root, child.id, receipt));
	const std::filesystem::path primary = AppPaths::UamChatFilePath(temp.root, child.id);
	const std::filesystem::file_time_type saved_time = std::filesystem::last_write_time(primary);
	std::string foreign = receipt.substr(receipt.find('\n') + 1);
	const std::size_t title = foreign.find(child.title);
	UAM_ASSERT(title != std::string::npos);
	foreign.replace(title, child.title.size(), "Replaced child");
	UAM_ASSERT(uam::io::WriteTextFile(primary, foreign));
	std::filesystem::last_write_time(primary, saved_time);
	UAM_ASSERT(!ChatRepository::MatchesTemporaryChatSaveReceipt(temp.root, child.id, receipt));
	UAM_ASSERT(!ChatRepository::DeleteTemporaryChatIfRevision(temp.root, child.id, child.temporary_parent_chat_id, receipt));
	std::string retained;
	UAM_ASSERT(uam::io::TryReadTextFile(primary, retained, foreign.size()));
	UAM_ASSERT(retained == foreign);
}

UAM_TEST(TemporarySideChatAdoptionRaceCannotOverwriteForeignOwner)
{
	TempDir temp("side-context-adoption-race");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession parent;
	parent.id = "side-context-race-parent";
	parent.provider_id = "codex-cli";
	app.chats.push_back(parent);
	const std::optional<ChatSession> candidate = uam::BuildTemporarySideChatCandidate(app, parent.id);
	UAM_ASSERT(candidate.has_value());
	std::string receipt;
	UAM_ASSERT(ChatRepository::SaveChatIfAbsent(temp.root, *candidate, &receipt));
	UAM_ASSERT(ChatRepository::MatchesTemporaryChatSaveReceipt(temp.root, candidate->id, receipt));
	const std::shared_ptr<PreparedChatSave> before_replacement = ChatRepository::PrepareChatSave(temp.root, *candidate);
	const std::shared_ptr<PreparedChatSave> opened_before_replacement = ChatRepository::PrepareLastOpenedAt(temp.root, *candidate);
	UAM_ASSERT(before_replacement && opened_before_replacement);
	// Replacement occurs after worker verification and before GUI adoption.
	const std::filesystem::path primary = AppPaths::UamChatFilePath(temp.root, candidate->id);
	const std::filesystem::file_time_type saved_time = std::filesystem::last_write_time(primary);
	std::string foreign = receipt.substr(receipt.find('\n') + 1);
	const std::size_t token = foreign.find(candidate->temporary_storage_owner_token);
	UAM_ASSERT(token != std::string::npos);
	foreign.replace(token, candidate->temporary_storage_owner_token.size(), "00000000-0000-0000-0000-000000000000");
	UAM_ASSERT(uam::io::WriteTextFile(primary, foreign));
	std::filesystem::last_write_time(primary, saved_time);
	UAM_ASSERT(uam::AdoptTemporarySideChat(app, *candidate, temp.root, receipt, parent.metadata_incarnation));
	ChatSession& adopted = app.chats.back();
	adopted.title = "Must not overwrite replacement";
	UAM_ASSERT(!ChatRepository::PublishPreparedChatSave(before_replacement));
	UAM_ASSERT(!ChatRepository::PublishPreparedChatSave(opened_before_replacement));
	bool full_save_required = false;
	UAM_ASSERT(!ChatRepository::SaveQueuedPromptOutbox(temp.root, adopted, &full_save_required));
	UAM_ASSERT(!ChatRepository::PrepareLastOpenedAt(temp.root, adopted));
	UAM_ASSERT(!ChatRepository::SaveLastOpenedAt(temp.root, adopted));
	UAM_ASSERT(!ChatRepository::SaveChat(temp.root, adopted));
	UAM_ASSERT(!ChatRepository::PrepareChatSave(temp.root, adopted));
	UAM_ASSERT(!ChatRepository::DeleteTemporaryChatIfRevision(temp.root, adopted.id, parent.id, receipt));
	std::string retained;
	UAM_ASSERT(uam::io::TryReadTextFile(primary, retained, foreign.size()));
	UAM_ASSERT(retained == foreign);
}

UAM_TEST(TemporarySideChatStorageTokenSurvivesReloadAndPreparedSave)
{
	TempDir temp("side-context-token-reload");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession parent;
	parent.id = "side-context-token-parent";
	parent.provider_id = "codex-cli";
	app.chats.push_back(parent);
	const std::optional<ChatSession> candidate = uam::BuildTemporarySideChatCandidate(app, parent.id);
	UAM_ASSERT(candidate.has_value());
	std::string receipt;
	UAM_ASSERT(ChatRepository::SaveChatIfAbsent(temp.root, *candidate, &receipt));
	const std::optional<ChatSession> summary = ChatRepository::LoadLocalChat(temp.root, candidate->id, false);
	const std::optional<ChatSession> full = ChatRepository::LoadLocalChat(temp.root, candidate->id, true);
	UAM_ASSERT(summary && full);
	UAM_ASSERT(summary->temporary_storage_owner_token == candidate->temporary_storage_owner_token);
	UAM_ASSERT(full->temporary_storage_owner_token == candidate->temporary_storage_owner_token);
	ChatSession updated = *full;
	updated.messages.push_back({MessageRole::User, "New side context"});
	const std::shared_ptr<PreparedChatSave> prepared = ChatRepository::PrepareChatSave(temp.root, updated);
	UAM_ASSERT(prepared != nullptr);
	UAM_ASSERT(ChatRepository::PublishPreparedChatSave(prepared));
	const std::optional<ChatSession> saved = ChatRepository::LoadLocalChat(temp.root, candidate->id, true);
	UAM_ASSERT(saved && saved->messages.size() == 1);
	UAM_ASSERT(saved->temporary_storage_owner_token == candidate->temporary_storage_owner_token);
	const std::shared_ptr<PreparedChatSave> stale = ChatRepository::PrepareChatSave(temp.root, *saved);
	UAM_ASSERT(stale != nullptr);
	UAM_ASSERT(!ChatRepository::DeleteChatStorageFiles(temp.root, candidate->id).Failed());
	UAM_ASSERT(!ChatRepository::PublishPreparedChatSave(stale));
	UAM_ASSERT(!ChatRepository::SaveChat(temp.root, *saved));
}

UAM_TEST(TemporarySideChatAdoptionRejectsRecreatedParentMetadata)
{
	TempDir temp("side-context-parent-incarnation");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession parent;
	parent.id = "side-context-recreated-parent";
	parent.created_at = "2026-10-13T00:00:00.000Z";
	parent.updated_at = parent.created_at;
	parent.provider_id = "codex-cli";
	app.chats.push_back(parent);
	const std::optional<ChatSession> candidate = uam::BuildTemporarySideChatCandidate(app, parent.id);
	UAM_ASSERT(candidate.has_value());
	std::string receipt;
	UAM_ASSERT(ChatRepository::SaveChatIfAbsent(temp.root, *candidate, &receipt));
	ChatSession recreated;
	recreated.id = parent.id;
	recreated.created_at = parent.created_at;
	recreated.updated_at = parent.updated_at;
	recreated.provider_id = parent.provider_id;
	app.chats.front() = recreated;
	UAM_ASSERT(!uam::AdoptTemporarySideChat(app, *candidate, temp.root, receipt, parent.metadata_incarnation));
	UAM_ASSERT(app.chats.size() == 1);
	UAM_ASSERT(ChatRepository::DeleteTemporaryChatIfRevision(temp.root, candidate->id, parent.id, receipt));
}

UAM_TEST(TemporarySideChatLargeOwnershipChecksRefuseMissingOrCorruptIndex)
{
	TempDir temp("side-context-missing-index");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession parent;
	parent.id = "side-context-large-parent";
	parent.provider_id = "codex-cli";
	app.chats.push_back(parent);
	std::optional<ChatSession> child = uam::BuildTemporarySideChatCandidate(app, parent.id);
	UAM_ASSERT(child.has_value());
	UAM_ASSERT(ChatRepository::SaveChatIfAbsent(temp.root, *child));
	child->messages.push_back({MessageRole::User, std::string(5 * 1024 * 1024, 'x')});
	const std::shared_ptr<PreparedChatSave> initial = ChatRepository::PrepareChatSave(temp.root, *child);
	UAM_ASSERT(initial && ChatRepository::PublishPreparedChatSave(initial));
	const std::shared_ptr<PreparedChatSave> pending = ChatRepository::PrepareChatSave(temp.root, *child);
	UAM_ASSERT(pending != nullptr);
	const std::filesystem::path primary = AppPaths::UamChatFilePath(temp.root, child->id);
	const std::filesystem::path index = uam::chat_pages::IndexPath(primary);
	std::string original, index_bytes;
	UAM_ASSERT(uam::io::TryReadTextFile(primary, original, 8 * 1024 * 1024));
	UAM_ASSERT(uam::io::TryReadTextFile(index, index_bytes, 1024 * 1024));
	for (int defect = 0; defect < 2; ++defect)
	{
		if (defect == 0) UAM_ASSERT(std::filesystem::remove(index));
		else UAM_ASSERT(uam::io::WriteTextFile(index, "corrupt child index"));
		UAM_ASSERT(!ChatRepository::SaveChat(temp.root, *child));
		UAM_ASSERT(!ChatRepository::PrepareChatSave(temp.root, *child));
		UAM_ASSERT(!ChatRepository::PublishPreparedChatSave(pending));
		bool full_save_required = false;
		UAM_ASSERT(!ChatRepository::SaveQueuedPromptOutbox(temp.root, *child, &full_save_required));
		UAM_ASSERT(!ChatRepository::PrepareLastOpenedAt(temp.root, *child));
		UAM_ASSERT(!ChatRepository::SaveLastOpenedAt(temp.root, *child));
		std::string retained;
		UAM_ASSERT(uam::io::TryReadTextFile(primary, retained, original.size()));
		UAM_ASSERT(retained == original);
	}
	UAM_ASSERT(uam::io::WriteTextFile(index, index_bytes));
	UAM_ASSERT(ChatRepository::PrepareChatSave(temp.root, *child) != nullptr);
}

UAM_TEST(TemporarySideChatMemoryStaysOffUnderEnabledDefaults)
{
	TempDir temp("side-context-memory-off");
	uam::AppState app;
	app.data_root = temp.root;
	app.settings.memory_enabled_default = true;
	app.settings.memory_level_default = uam::memory_levels::kOpen;
	ChatSession parent;
	parent.id = "side-context-memory-parent";
	parent.provider_id = "codex-cli";
	parent.memory_level = uam::memory_levels::kDefault;
	app.chats.push_back(parent);
	for (int provider_override = 0; provider_override < 2; ++provider_override)
	{
		if (provider_override != 0)
		{
			app.settings.provider_chat_defaults[parent.provider_id].memory_enabled = true;
			app.settings.provider_chat_defaults[parent.provider_id].memory_level = uam::memory_levels::kStrict;
		}
		UAM_ASSERT(uam::memory_policy::Resolve(app.settings, parent) != uam::memory_levels::kOff);
		const std::optional<ChatSession> candidate = uam::BuildTemporarySideChatCandidate(app, parent.id);
		UAM_ASSERT(candidate.has_value());
		UAM_ASSERT(!candidate->memory_enabled);
		UAM_ASSERT(candidate->memory_level == uam::memory_levels::kOff);
		UAM_ASSERT(uam::memory_policy::Resolve(app.settings, *candidate) == uam::memory_levels::kOff);
		UAM_ASSERT(ChatRepository::SaveChatIfAbsent(temp.root, *candidate));
		const std::optional<ChatSession> reloaded = ChatRepository::LoadLocalChat(temp.root, candidate->id, false);
		UAM_ASSERT(reloaded.has_value());
		UAM_ASSERT(uam::memory_policy::Resolve(app.settings, *reloaded) == uam::memory_levels::kOff);
	}
}

UAM_TEST(TemporarySideChatDeletionStagesOwnedSnapshotAndKeepsSurvivor)
{
	for (int mode = 0; mode < 4; ++mode)
	{
		TempDir temp("side-context-deletion-snapshot-" + std::to_string(mode));
		uam::AppState app;
		app.data_root = temp.root;
		app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
		ChatSession parent;
		parent.id = "side-context-deletion-parent";
		parent.provider_id = "codex-cli";
		parent.workspace_directory = temp.root.string();
		parent.messages.push_back({MessageRole::User, "Surviving main-chat content"});
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, parent));
		app.chats.push_back(parent);
		std::optional<ChatSession> child = uam::BuildTemporarySideChatCandidate(app, parent.id);
		UAM_ASSERT(child.has_value());
		child->messages.push_back({MessageRole::User, "Owned rollback content"});
		UAM_ASSERT(ChatRepository::SaveChatIfAbsent(temp.root, *child));
		const std::string child_id = child->id;
		const std::string token = child->temporary_storage_owner_token;
		const std::filesystem::path parent_primary = AppPaths::UamChatFilePath(temp.root, parent.id);
		const std::filesystem::path child_primary = AppPaths::UamChatFilePath(temp.root, child_id);
		std::string parent_bytes, child_bytes;
		UAM_ASSERT(uam::io::TryReadTextFile(parent_primary, parent_bytes));
		UAM_ASSERT(uam::io::TryReadTextFile(child_primary, child_bytes));
		if (mode % 2 != 0)
		{
			child = ChatRepository::LoadLocalChat(temp.root, child_id, false);
			UAM_ASSERT(child && !child->messages_loaded);
		}
		app.chats.push_back(*child);
		if (mode >= 2)
		{
			ChatSession collision = *child;
			collision.title = "Conflicting rollback generation";
			app.chats.push_back(collision);
		}
		uam::WorkspaceDeletionTask task;
		UAM_ASSERT(uam::PrepareChatDeletion(app, {child_id}, task));
		const bool staged = uam::StageWorkspaceDeletion(task);
		const std::optional<ChatSession> rollback = ChatRepository::LoadLocalChat(temp.root / ".deletion-transaction", child_id, true);
		UAM_ASSERT(rollback.has_value());
		UAM_ASSERT(rollback->temporary_storage_owner_token == token);
		UAM_ASSERT(rollback->messages.size() == 1);
		UAM_ASSERT(rollback->messages.front().content == "Owned rollback content");
		if (mode >= 2)
		{
			UAM_ASSERT(!staged);
			UAM_ASSERT(rollback->title != "Conflicting rollback generation");
			std::string retained_child;
			UAM_ASSERT(uam::io::TryReadTextFile(child_primary, retained_child));
			UAM_ASSERT(retained_child == child_bytes);
			UAM_ASSERT(ChatDomainService().FindChatById(app, child_id) != nullptr);
		}
		else
		{
			UAM_ASSERT(staged);
			UAM_ASSERT(uam::RecoverPendingDeletionTransaction(app));
			UAM_ASSERT(!ChatRepository::LoadLocalChat(temp.root, child_id));
		}
		std::string retained_parent;
		UAM_ASSERT(uam::io::TryReadTextFile(parent_primary, retained_parent));
		UAM_ASSERT(retained_parent == parent_bytes);
	}
}
