#include "test_harness.h"
#include "common/chat/chat_message_page_index.h"

#include <type_traits>

using namespace uam_test;

static_assert(!std::is_convertible_v<std::shared_ptr<PreparedChatPublication>, std::shared_ptr<PreparedChatSave>>);
static_assert(!std::is_convertible_v<std::shared_ptr<PreparedChatSave>, std::shared_ptr<PreparedChatPublication>>);

namespace
{
	ChatSession PublicationChat()
	{
		ChatSession chat;
		chat.id = "conditional-publication-fixture";
		chat.provider_id = "codex-cli";
		chat.execution_host_id = "local";
		chat.workspace_directory = "/tmp/uam-publication-storage-fixture";
		chat.title = "Stored original";
		chat.created_at = "2026-10-17T10:00:00.000Z";
		chat.updated_at = "2026-10-17T10:00:01.000Z";
		chat.last_opened_at = "2026-10-17T10:00:02.000Z";
		chat.attention_revision = "owned-attention-overlay";
		chat.snoozed_until_ms = 1800000000123;
		Message message;
		message.role = MessageRole::User;
		message.content = "Exact authored bytes: {} [] \\\"quoted\\\" Unicode ☃\n";
		message.created_at = chat.created_at;
		message.provider = chat.provider_id;
		message.model_id = "inert-model";
		chat.messages.push_back(message);
		return chat;
	}

	ChatSession PublicationCandidate(ChatSession chat)
	{
		chat.title = "Updated metadata and complete history";
		chat.updated_at = "2026-10-17T10:00:03.000Z";
		Message answer;
		answer.role = MessageRole::Assistant;
		answer.content = "Complete stored answer, with trailing spaces.  ";
		answer.created_at = chat.updated_at;
		answer.provider = chat.provider_id;
		answer.model_id = "inert-model";
		chat.messages.push_back(answer);
		return chat;
	}

	std::string PublicationRevision(const fs::path& root, const ChatSession& chat)
	{
		const auto revision = ChatRepository::ChatStorageRevision(root, chat.id);
		UAM_ASSERT(revision && *revision != "missing" && *revision != "unreadable");
		return *revision;
	}

	std::string PublicationBytes(const fs::path& path)
	{
		UAM_ASSERT(fs::is_regular_file(path) && !fs::is_symlink(path));
		return ReadFile(path);
	}

	std::map<fs::path, std::string> PublicationFiles(const fs::path& root)
	{
		std::map<fs::path, std::string> files;
		for (const auto& entry : fs::recursive_directory_iterator(root))
			if (entry.is_regular_file() && !entry.is_symlink())
				files.emplace(entry.path().lexically_relative(root), ReadFile(entry.path()));
		return files;
	}

	void LegacyPreparedPublication(const fs::path& root, ChatSession& chat)
	{
		const auto revision = ChatRepository::ChatStorageRevision(root, chat.id);
		UAM_ASSERT(revision);
		auto prepared = ChatRepository::PrepareChatSave(root, chat, *revision);
		UAM_ASSERT(prepared && !prepared->primary_temp.empty());
		UAM_ASSERT(ChatRepository::PublishPreparedChatSave(prepared, true, &chat));
		prepared.reset();
		const auto loaded = ChatRepository::LoadLocalChatBounded(root, chat.id, 4 * 1024 * 1024);
		UAM_ASSERT(loaded && loaded->id == chat.id);
		UAM_ASSERT_EQ(loaded->messages, chat.messages);
	}

	std::string IndependentlyPreparedBytes(ChatSession chat)
	{
		TempDir expected("uam-publication-independent-encoding");
		LegacyPreparedPublication(expected.root, chat);
		return PublicationBytes(AppPaths::UamChatFilePath(expected.root, chat.id));
	}

	void SeedPublication(const fs::path& root, ChatSession& chat)
	{
		ChatSession older = chat;
		older.title = "Actual predecessor backup";
		LegacyPreparedPublication(root, older);
		LegacyPreparedPublication(root, chat);
		const fs::path primary = AppPaths::UamChatFilePath(root, chat.id);
		UAM_ASSERT(fs::is_regular_file(uam::io::MakeBackupPath(primary)));
		UAM_ASSERT(PublicationBytes(primary) != PublicationBytes(uam::io::MakeBackupPath(primary)));
	}

	bool HasPrivateEvidence(const ChatPublicationResult& result, const fs::path& primary, std::string_view bytes)
	{
		for (const fs::path& path : result.evidence_paths)
			if (path != primary && path != uam::io::MakeBackupPath(primary) && fs::is_regular_file(path) &&
			    !fs::is_symlink(path) && ReadFile(path) == bytes) return true;
		return false;
	}

	void SeedUnloadedBackup(const fs::path& root, ChatSession& unloaded)
	{
		ChatSession chat = PublicationChat();
		UAM_ASSERT(ChatRepository::SaveChat(root, chat));
		chat.title = "Current saved metadata";
		UAM_ASSERT(ChatRepository::SaveChat(root, chat));
		const auto summary = ChatRepository::LoadLocalChat(root, chat.id, false);
		UAM_ASSERT(summary && !summary->messages_loaded && summary->persisted_message_count == 1);
		unloaded = *summary;
		UAM_ASSERT(unloaded.messages.empty());
		const auto backup = nlohmann::json::parse(PublicationBytes(uam::io::MakeBackupPath(AppPaths::UamChatFilePath(root, chat.id))));
		UAM_ASSERT_EQ(backup.at("messages").size(), std::size_t{1});
		UAM_ASSERT_EQ(backup.at("messages").front().at("content").get<std::string>(), chat.messages.front().content);
	}
}

UAM_TEST(ChatPublicationCreateAbsentCommitsExactPreparedBytesOnce)
{
	TempDir temp("uam-publication-create");
	ChatSession candidate = PublicationChat();
	const std::string expected = IndependentlyPreparedBytes(candidate);
	UAM_ASSERT(candidate.managed_followup_incarnation.empty());
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, candidate.id);
	const fs::path backup = uam::io::MakeBackupPath(primary);
	UAM_ASSERT(!fs::exists(primary) && !fs::exists(backup));
	std::string error;
	auto prepared = ChatRepository::PrepareChatPublication(temp.root, candidate, ChatPublicationMode::CreateAbsent, {}, &error);
	UAM_ASSERT(prepared); // Intended operation RED after independent public codec/readability prerequisites.
	UAM_ASSERT(error.empty() && !fs::exists(primary) && !fs::exists(backup));
	const auto committed = ChatRepository::PublishPreparedChatPublication(prepared, [] { return true; }, &candidate);
	UAM_ASSERT_EQ(committed.state, ChatPublicationState::Committed);
	UAM_ASSERT(!committed.backup_degraded && !committed.secondary_degraded);
	UAM_ASSERT_EQ(PublicationBytes(primary), expected);
	UAM_ASSERT(!fs::exists(backup));
	const auto before_retry = PublicationFiles(temp.root);
	UAM_ASSERT_EQ(ChatRepository::PublishPreparedChatPublication(prepared, [] { return true; }).state, ChatPublicationState::Rejected);
	prepared.reset();
	UAM_ASSERT_EQ(PublicationFiles(temp.root), before_retry);
	const auto loaded = ChatRepository::LoadLocalChatBounded(temp.root, candidate.id, 4 * 1024 * 1024);
	UAM_ASSERT(loaded && loaded->managed_followup_incarnation.empty());
	UAM_ASSERT_EQ(loaded->messages, candidate.messages);
}

UAM_TEST(ChatPublicationUpdateExistingCommitsExactPreparedBytesAndBackup)
{
	TempDir temp("uam-publication-update");
	ChatSession chat = PublicationChat();
	SeedPublication(temp.root, chat);
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id);
	const std::string old_primary = PublicationBytes(primary), old_backup = PublicationBytes(uam::io::MakeBackupPath(primary));
	ChatSession candidate = PublicationCandidate(chat);
	const std::string expected = IndependentlyPreparedBytes(candidate);
	std::string error;
	auto prepared = ChatRepository::PrepareChatPublication(temp.root, candidate, ChatPublicationMode::UpdateExisting, PublicationRevision(temp.root, chat), &error);
	UAM_ASSERT(prepared);
	UAM_ASSERT(error.empty());
	UAM_ASSERT_EQ(PublicationBytes(primary), old_primary);
	UAM_ASSERT_EQ(PublicationBytes(uam::io::MakeBackupPath(primary)), old_backup);
	const auto result = ChatRepository::PublishPreparedChatPublication(prepared, [] { return true; }, &candidate);
	UAM_ASSERT_EQ(result.state, ChatPublicationState::Committed);
	UAM_ASSERT(!result.backup_degraded && !result.secondary_degraded);
	UAM_ASSERT_EQ(PublicationBytes(primary), expected);
	UAM_ASSERT_EQ(PublicationBytes(uam::io::MakeBackupPath(primary)), old_primary);
	prepared.reset();
	const auto loaded = ChatRepository::LoadLocalChatBounded(temp.root, candidate.id, 4 * 1024 * 1024);
	UAM_ASSERT(loaded);
	UAM_ASSERT_EQ(loaded->messages, candidate.messages);
	UAM_ASSERT_EQ(loaded->attention_revision, chat.attention_revision);
	UAM_ASSERT_EQ(loaded->snoozed_until_ms, chat.snoozed_until_ms);
}

UAM_TEST(ChatPublicationCreateInterferenceOwnerLossAndAbandonmentRemainInert)
{
	for (int variant = 0; variant < 4; ++variant)
	{
		TempDir temp("uam-publication-inert-rejection");
		ChatSession chat = PublicationChat();
		UAM_ASSERT(!IndependentlyPreparedBytes(chat).empty());
		const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id), backup = uam::io::MakeBackupPath(primary);
		const auto baseline = PublicationFiles(temp.root);
		auto prepared = ChatRepository::PrepareChatPublication(temp.root, chat, ChatPublicationMode::CreateAbsent, {});
		UAM_ASSERT(prepared);
		UAM_ASSERT(!fs::exists(primary) && !fs::exists(backup));
		if (variant == 0)
		{
			prepared.reset();
			UAM_ASSERT_EQ(PublicationFiles(temp.root), baseline);
			continue;
		}
		if (variant == 1) UAM_ASSERT(uam::io::WriteTextFile(backup, "intervening backup evidence"));
		std::shared_ptr<PreparedChatPublication> competing;
		if (variant == 3)
		{
			competing = ChatRepository::PrepareChatPublication(temp.root, chat, ChatPublicationMode::CreateAbsent, {});
			UAM_ASSERT(competing);
			UAM_ASSERT_EQ(ChatRepository::PublishPreparedChatPublication(competing, [] { return true; }).state, ChatPublicationState::Committed);
		}
		const std::string primary_before = fs::exists(primary) ? PublicationBytes(primary) : std::string{};
		const std::string backup_before = fs::exists(backup) ? PublicationBytes(backup) : std::string{};
		const auto refused = ChatRepository::PublishPreparedChatPublication(prepared, [variant] { return variant != 2; });
		UAM_ASSERT_EQ(refused.state, ChatPublicationState::Rejected);
		prepared.reset(); competing.reset();
		UAM_ASSERT_EQ(fs::exists(primary), variant == 3);
		UAM_ASSERT_EQ(fs::exists(backup), variant == 1);
		if (fs::exists(primary)) UAM_ASSERT_EQ(PublicationBytes(primary), primary_before);
		if (fs::exists(backup)) UAM_ASSERT_EQ(PublicationBytes(backup), backup_before);
	}
}

UAM_TEST(ChatPublicationPrimaryFaultRetainsExchangeEvidenceAcrossResetAndJanitor)
{
	for (const auto fault : {uam::io::AtomicWriteStage::BeforeReplace, uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync, uam::io::AtomicWriteStage::BeforeBackupCleanup})
	{
		TempDir temp("uam-publication-primary-fault");
		ChatSession chat = PublicationChat();
		SeedPublication(temp.root, chat);
		const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id), backup = uam::io::MakeBackupPath(primary);
		fs::last_write_time(primary, fs::file_time_type::clock::now() - std::chrono::hours(25));
		const std::string old = PublicationBytes(primary), old_backup = PublicationBytes(backup);
		ChatSession candidate = PublicationCandidate(chat);
		const std::string expected = IndependentlyPreparedBytes(candidate);
		auto prepared = ChatRepository::PrepareChatPublication(temp.root, candidate, ChatPublicationMode::UpdateExisting, PublicationRevision(temp.root, chat));
		UAM_ASSERT(prepared);
		bool fault_seen = false;
		const auto result = ChatRepository::PublishPreparedChatPublication(prepared, [] { return true; }, nullptr,
		    [&](ChatPublicationFile file, uam::io::AtomicWriteStage stage)
		    {
			    if (file == ChatPublicationFile::Primary && stage == fault) { fault_seen = true; return false; }
			    return true;
		    });
		UAM_ASSERT(fault_seen);
		prepared.reset();
		UAM_ASSERT_EQ(PublicationBytes(backup), old_backup);
		if (fault == uam::io::AtomicWriteStage::BeforeReplace)
		{
			UAM_ASSERT_EQ(result.state, ChatPublicationState::Rejected);
			UAM_ASSERT_EQ(PublicationBytes(primary), old);
		}
		else
		{
			UAM_ASSERT_EQ(result.state, fault == uam::io::AtomicWriteStage::BeforeBackupCleanup ? ChatPublicationState::Committed : ChatPublicationState::Uncertain);
			if (result.state == ChatPublicationState::Committed) UAM_ASSERT(result.backup_degraded);
			UAM_ASSERT_EQ(PublicationBytes(primary), expected);
			UAM_ASSERT(HasPrivateEvidence(result, primary, old));
			uam::io::CleanupAbandonedAtomicTemps(primary);
			uam::io::CleanupAbandonedAtomicTemps(backup);
			UAM_ASSERT(HasPrivateEvidence(result, primary, old));
			UAM_ASSERT_EQ(PublicationBytes(primary), expected);
		}
	}
}

UAM_TEST(ChatPublicationSecondaryFaultReportsCommittedPrimaryAndRetainsPredecessor)
{
	for (const auto file : {ChatPublicationFile::MessageIndex, ChatPublicationFile::Summary})
	{
		for (const auto fault : {uam::io::AtomicWriteStage::BeforeReplace, uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync})
		{
			TempDir temp("uam-publication-secondary-fault");
			ChatSession chat = PublicationChat();
			SeedPublication(temp.root, chat);
			const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id);
			const fs::path target = file == ChatPublicationFile::Summary ? AppPaths::UamChatSummaryFilePath(temp.root, chat.id) : uam::chat_pages::IndexPath(primary);
			fs::last_write_time(target, fs::file_time_type::clock::now() - std::chrono::hours(25));
			const std::string old_secondary = PublicationBytes(target), old_primary = PublicationBytes(primary);
			if (file == ChatPublicationFile::Summary)
				UAM_ASSERT_EQ(nlohmann::json::parse(old_secondary).at("attention_revision").get<std::string>(), chat.attention_revision);
			ChatSession candidate = PublicationCandidate(chat);
			const std::string expected = IndependentlyPreparedBytes(candidate);
			auto prepared = ChatRepository::PrepareChatPublication(temp.root, candidate, ChatPublicationMode::UpdateExisting, PublicationRevision(temp.root, chat));
			UAM_ASSERT(prepared);
			bool fault_seen = false;
			const auto result = ChatRepository::PublishPreparedChatPublication(prepared, [] { return true; }, &candidate,
			    [&](ChatPublicationFile role, uam::io::AtomicWriteStage stage)
			    {
				    if (role == file && stage == fault) { fault_seen = true; return false; }
				    return true;
			    });
			UAM_ASSERT(fault_seen);
			UAM_ASSERT_EQ(result.state, ChatPublicationState::Committed);
			UAM_ASSERT(result.secondary_degraded);
			prepared.reset();
			UAM_ASSERT_EQ(PublicationBytes(primary), expected);
			UAM_ASSERT_EQ(PublicationBytes(uam::io::MakeBackupPath(primary)), old_primary);
			if (fault == uam::io::AtomicWriteStage::BeforeReplace) UAM_ASSERT_EQ(PublicationBytes(target), old_secondary);
			else
			{
				UAM_ASSERT(HasPrivateEvidence(result, target, old_secondary));
				uam::io::CleanupAbandonedAtomicTemps(target);
				uam::io::CleanupAbandonedAtomicTemps(uam::io::MakeBackupPath(target));
				UAM_ASSERT(HasPrivateEvidence(result, target, old_secondary));
			}
		}
	}
}

UAM_TEST(ChatPublicationUnloadedCompleteSourceUsesOnlyPrivateMissingIndexPreparation)
{
	TempDir temp("uam-publication-unloaded-private-index");
	ChatSession metadata = PublicationChat();
	std::vector<Message> expected = PublicationCandidate(metadata).messages;
	metadata.messages.clear();
	auto imported = ChatRepository::PrepareImportedChatSave(temp.root, metadata,
	    [&](const std::function<bool(const Message&)>& emit, ChatSession&)
	    {
		    for (const Message& message : expected) if (!emit(message)) return false;
		    return true;
	    });
	UAM_ASSERT(imported && ChatRepository::PublishPreparedChatSave(imported, true, &metadata));
	imported.reset();
	UAM_ASSERT(!metadata.messages_loaded && metadata.messages.empty());
	UAM_ASSERT_EQ(metadata.persisted_message_count, expected.size());
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, metadata.id), index_path = uam::chat_pages::IndexPath(primary);
	const auto source = ChatRepository::LoadLocalChatBounded(temp.root, metadata.id, 4 * 1024 * 1024);
	UAM_ASSERT(source && source->messages == expected);
	UAM_ASSERT(uam::chat_pages::ReadIndex(index_path, PublicationRevision(temp.root, metadata)));
	UAM_ASSERT(fs::remove(index_path));
	const std::string old_primary = PublicationBytes(primary), old_summary = PublicationBytes(AppPaths::UamChatSummaryFilePath(temp.root, metadata.id));
	metadata.pinned = true;
	auto prepared = ChatRepository::PrepareChatPublication(temp.root, metadata, ChatPublicationMode::UpdateExisting, PublicationRevision(temp.root, metadata));
	UAM_ASSERT(prepared);
	UAM_ASSERT(!fs::exists(index_path));
	UAM_ASSERT_EQ(PublicationBytes(primary), old_primary);
	UAM_ASSERT_EQ(PublicationBytes(AppPaths::UamChatSummaryFilePath(temp.root, metadata.id)), old_summary);
	UAM_ASSERT_EQ(ChatRepository::PublishPreparedChatPublication(prepared, [] { return true; }, &metadata).state, ChatPublicationState::Committed);
	prepared.reset();
	const auto complete = ChatRepository::LoadLocalChatBounded(temp.root, metadata.id, 4 * 1024 * 1024);
	UAM_ASSERT(complete && complete->pinned);
	UAM_ASSERT_EQ(complete->messages, expected);
	const auto index = uam::chat_pages::ReadIndex(index_path, PublicationRevision(temp.root, metadata));
	UAM_ASSERT(index && index->message_count == expected.size() && index->attachment_owner == metadata.id);
}

UAM_TEST(ChatPublicationRejectsUnusableUnloadedPrimaryWithoutLegacyBackupAdoption)
{
	// Genuine legacy fallback remains supported; explicit UpdateExisting must not inherit it.
	TempDir legacy("uam-publication-legacy-backup-control");
	ChatSession legacy_unloaded;
	SeedUnloadedBackup(legacy.root, legacy_unloaded);
	const fs::path legacy_primary = AppPaths::UamChatFilePath(legacy.root, legacy_unloaded.id);
	UAM_ASSERT(uam::io::WriteTextFile(legacy_primary, "{"));
	legacy_unloaded.pinned = true;
	UAM_ASSERT(ChatRepository::SaveChat(legacy.root, legacy_unloaded));
	const auto restored = ChatRepository::LoadLocalChatBounded(legacy.root, legacy_unloaded.id, 4 * 1024 * 1024);
	UAM_ASSERT(restored && restored->pinned && restored->messages.size() == 1);
	UAM_ASSERT_EQ(restored->messages.front().content, PublicationChat().messages.front().content);

	TempDir temp("uam-publication-unusable-existing-primary");
	ChatSession unloaded;
	SeedUnloadedBackup(temp.root, unloaded);
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, unloaded.id);
	UAM_ASSERT(uam::io::WriteTextFile(primary, "{"));
	UAM_ASSERT(fs::is_regular_file(primary));
	const std::string revision = PublicationRevision(temp.root, unloaded);
	const auto baseline = PublicationFiles(temp.root);
	unloaded.pinned = true;
	std::string error;
	UAM_ASSERT(!ChatRepository::PrepareChatPublication(temp.root, unloaded, ChatPublicationMode::UpdateExisting, revision, &error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT_EQ(PublicationFiles(temp.root), baseline);
	int callbacks = 0;
	const auto refused = ChatRepository::PublishPreparedChatPublication({}, [&] { ++callbacks; return true; }, &unloaded,
	    [&](ChatPublicationFile, uam::io::AtomicWriteStage) { ++callbacks; return true; });
	UAM_ASSERT_EQ(refused.state, ChatPublicationState::Rejected);
	UAM_ASSERT_EQ(callbacks, 0);
	UAM_ASSERT_EQ(PublicationFiles(temp.root), baseline);
}

UAM_TEST(ChatPublicationPrivateIndexSpoolValidOperationUsesRealSource)
{
	TempDir temp("uam-publication-private-spool-valid");
	ChatSession chat = PublicationChat();
	LegacyPreparedPublication(temp.root, chat);
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id);
	const std::string revision = PublicationRevision(temp.root, chat);
	const auto source_index = uam::chat_pages::ReadIndex(uam::chat_pages::IndexPath(primary), revision);
	UAM_ASSERT(source_index && source_index->message_count == 1);
	std::vector<uam::chat_pages::Span> spans;
	UAM_ASSERT(uam::chat_pages::ReadMessageSpans(uam::chat_pages::IndexPath(primary), *source_index, 0, 1, spans));
	UAM_ASSERT_EQ(spans.size(), std::size_t{1});
	const fs::path records = temp.root / ".uam-publication-11111111-1111-4111-8111-111111111111.records";
	const fs::path staged_index = temp.root / "private-index";
	UAM_ASSERT(!fs::exists(records));
	{
		uam::chat_pages::Builder builder(primary, chat.id, records);
		UAM_ASSERT(builder.Add(spans.front(), chat.messages.front())); // Actual operation RED, not missing constructor.
		UAM_ASSERT(builder.Finish(staged_index, revision, source_index->source_size, source_index->array_begin, source_index->array_end));
		const auto index = uam::chat_pages::ReadIndex(staged_index, revision);
		UAM_ASSERT(index && index->message_count == 1 && index->attachment_owner == chat.id);
	}
	UAM_ASSERT(!fs::exists(records));
	UAM_ASSERT(fs::is_regular_file(staged_index));
}

UAM_TEST(ChatPublicationPrivateIndexSpoolCollisionNeverAdoptsOrDeletesExistingBytes)
{
	TempDir temp("uam-publication-private-spool-collision");
	ChatSession chat = PublicationChat();
	LegacyPreparedPublication(temp.root, chat);
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id);
	const std::string revision = PublicationRevision(temp.root, chat);
	const auto source_index = uam::chat_pages::ReadIndex(uam::chat_pages::IndexPath(primary), revision);
	UAM_ASSERT(source_index && source_index->message_count == 1);
	std::vector<uam::chat_pages::Span> spans;
	UAM_ASSERT(uam::chat_pages::ReadMessageSpans(uam::chat_pages::IndexPath(primary), *source_index, 0, 1, spans));
	UAM_ASSERT_EQ(spans.size(), std::size_t{1});
	const fs::path records = temp.root / ".uam-publication-22222222-2222-4222-8222-222222222222.records";
	const fs::path staged_index = temp.root / "must-not-be-created-index";
	UAM_ASSERT(uam::io::WriteTextFile(records, "preexisting private spool collision evidence"));
	const auto baseline = PublicationFiles(temp.root);
	{
		uam::chat_pages::Builder builder(primary, chat.id, records);
		UAM_ASSERT(!builder.Add(spans.front(), chat.messages.front()));
		UAM_ASSERT(!builder.CopyRecords(uam::chat_pages::IndexPath(primary), *source_index, source_index->array_begin));
		UAM_ASSERT(!builder.Finish(staged_index, revision, source_index->source_size, source_index->array_begin, source_index->array_end));
	}
	UAM_ASSERT(!fs::exists(staged_index));
	UAM_ASSERT_EQ(PublicationFiles(temp.root), baseline);
}

UAM_TEST(ChatPublicationPrivateCommitHelperCommitsAndRetainsUncertainEvidence)
{
	for (const bool interrupt_after_replace : {false, true})
	{
		TempDir temp("uam-publication-private-commit-helper");
		const fs::path primary = temp.root / "primary", candidate = temp.root / ".uam-publication-candidate", evidence = temp.root / ".uam-publication-predecessor";
		UAM_ASSERT(uam::io::WriteTextFile(primary, "old complete primary"));
		fs::last_write_time(primary, fs::file_time_type::clock::now() - std::chrono::hours(25));
		uam::io::AtomicWriteResult staged;
		UAM_ASSERT(uam::io::WriteAndSyncAtomicTemp(candidate, "new complete candidate", {}, staged));
		UAM_ASSERT_EQ(PublicationBytes(primary), std::string("old complete primary"));
		UAM_ASSERT_EQ(PublicationBytes(candidate), std::string("new complete candidate"));
		UAM_ASSERT(!fs::exists(evidence));
		bool before_replace = false, after_replace = false;
		const auto result = uam::io::CommitSyncedAtomicTemp(primary, candidate, false,
		    [&](uam::io::AtomicWriteStage stage)
		    {
			    if (stage == uam::io::AtomicWriteStage::BeforeReplace) before_replace = true;
			    if (stage == uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync) { after_replace = true; return !interrupt_after_replace; }
			    return true;
		    }, true, true, evidence);
		UAM_ASSERT(before_replace); // Intended helper RED after actual exclusive staging prerequisites.
		UAM_ASSERT(after_replace);
		UAM_ASSERT_EQ(PublicationBytes(primary), std::string("new complete candidate"));
		if (interrupt_after_replace)
		{
			UAM_ASSERT(!result.success && result.interrupted);
			const fs::path old =
#if defined(_WIN32)
			    evidence;
#else
			    candidate;
#endif
			UAM_ASSERT_EQ(PublicationBytes(old), std::string("old complete primary"));
			uam::io::CleanupAbandonedAtomicTemps(primary);
			uam::io::CleanupAbandonedAtomicTemps(uam::io::MakeBackupPath(primary));
			UAM_ASSERT_EQ(PublicationBytes(old), std::string("old complete primary"));
		}
		else
		{
			UAM_ASSERT(result.success && result.primary_committed && !result.backup_degraded);
			UAM_ASSERT(!fs::exists(candidate) && !fs::exists(evidence));
			UAM_ASSERT(!fs::exists(uam::io::MakeBackupPath(primary)));
		}
	}
}

UAM_TEST(ChatPublicationPrivateCommitHelperRejectsPreexistingEvidenceWithoutCallbacks)
{
	TempDir temp("uam-publication-private-evidence-collision");
	const fs::path primary = temp.root / "primary", candidate = temp.root / ".uam-publication-candidate", evidence = temp.root / ".uam-publication-predecessor";
	UAM_ASSERT(uam::io::WriteTextFile(primary, "old"));
	uam::io::AtomicWriteResult staged;
	UAM_ASSERT(uam::io::WriteAndSyncAtomicTemp(candidate, "candidate", {}, staged));
	UAM_ASSERT(uam::io::WriteTextFile(evidence, "unowned collision"));
	const auto baseline = PublicationFiles(temp.root);
	int callbacks = 0;
	const auto result = uam::io::CommitSyncedAtomicTemp(primary, candidate, false,
	    [&](uam::io::AtomicWriteStage) { ++callbacks; return true; }, true, true, evidence);
	UAM_ASSERT(!result.success && !result.error.empty());
	UAM_ASSERT_EQ(callbacks, 0);
	UAM_ASSERT_EQ(PublicationFiles(temp.root), baseline);
}

#if defined(_WIN32)
UAM_TEST(ChatPublicationWindowsNativeSharingDeniedReplaceRetainsOwnedCandidate)
{
	TempDir temp("uam-publication-windows-native-replace-failure");
	const fs::path primary = temp.root / "primary", candidate = temp.root / ".uam-publication-candidate", evidence = temp.root / ".uam-publication-predecessor";
	UAM_ASSERT(uam::io::WriteTextFile(primary, "old native primary"));
	uam::io::AtomicWriteResult staged;
	UAM_ASSERT(uam::io::WriteAndSyncAtomicTemp(candidate, "new native candidate", {}, staged));
	const HANDLE raw = CreateFileW(primary.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
	    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	UAM_ASSERT(raw != INVALID_HANDLE_VALUE);
	const std::unique_ptr<void, decltype(&CloseHandle)> held(raw, &CloseHandle);
	// Real native sharing failure is a prerequisite, distinct from an injected after-success fault.
	UAM_ASSERT(!ReplaceFileW(primary.c_str(), candidate.c_str(), evidence.c_str(), 0, nullptr, nullptr));
	UAM_ASSERT_EQ(GetLastError(), DWORD{ERROR_SHARING_VIOLATION});
	UAM_ASSERT_EQ(PublicationBytes(primary), std::string("old native primary"));
	UAM_ASSERT_EQ(PublicationBytes(candidate), std::string("new native candidate"));
	UAM_ASSERT(!fs::exists(evidence));

	// Preserve six-argument failure/cleanup behavior using a separate legacy candidate.
	const fs::path legacy_candidate = temp.root / "legacy-candidate";
	uam::io::AtomicWriteResult legacy_staged;
	UAM_ASSERT(uam::io::WriteAndSyncAtomicTemp(legacy_candidate, "legacy candidate", {}, legacy_staged));
	const auto legacy = uam::io::CommitSyncedAtomicTemp(primary, legacy_candidate, true, {}, true, true);
	UAM_ASSERT(!legacy.success && !legacy.interrupted);
	UAM_ASSERT(!fs::exists(legacy_candidate));
	bool permission_seen = false;
	const auto result = uam::io::CommitSyncedAtomicTemp(primary, candidate, true,
	    [&](uam::io::AtomicWriteStage stage) { if (stage == uam::io::AtomicWriteStage::BeforeReplace) permission_seen = true; return true; },
	    true, true, evidence);
	UAM_ASSERT(permission_seen);
	UAM_ASSERT(!result.success && !result.interrupted && !result.error.empty());
	UAM_ASSERT_EQ(result.stage, uam::io::AtomicWriteStage::BeforeReplace);
	UAM_ASSERT_EQ(PublicationBytes(primary), std::string("old native primary"));
	UAM_ASSERT_EQ(PublicationBytes(candidate), std::string("new native candidate"));
	UAM_ASSERT(!fs::exists(evidence));
}
#endif

UAM_TEST(ChatPublicationUpdateRejectsSameRevisionPrimaryContentInterference)
{
	TempDir temp("uam-publication-same-revision-incumbent");
	ChatSession chat = PublicationChat();
	SeedPublication(temp.root, chat);
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id);
	const fs::path backup = uam::io::MakeBackupPath(primary);
	const std::string revision = PublicationRevision(temp.root, chat);
	const std::string original = PublicationBytes(primary), old_backup = PublicationBytes(backup);
	const auto modified = fs::last_write_time(primary);
	ChatSession candidate = PublicationCandidate(chat);
	auto prepared = ChatRepository::PrepareChatPublication(temp.root, candidate, ChatPublicationMode::UpdateExisting, revision);
	UAM_ASSERT(prepared);
	std::string interference = original;
	const std::size_t title = interference.find("Stored original");
	UAM_ASSERT(title != std::string::npos);
	interference[title] = 'F';
	UAM_ASSERT(uam::io::WriteTextFile(primary, interference));
	fs::last_write_time(primary, modified);
	UAM_ASSERT_EQ(PublicationRevision(temp.root, chat), revision);
	UAM_ASSERT(interference != original && interference.size() == original.size());
	UAM_ASSERT_EQ(ChatRepository::PublishPreparedChatPublication(prepared, [] { return true; }).state, ChatPublicationState::Rejected);
	prepared.reset();
	UAM_ASSERT_EQ(PublicationBytes(primary), interference);
	UAM_ASSERT_EQ(PublicationBytes(backup), old_backup);
}

UAM_TEST(ChatPublicationRejectsChangedOwnedStageWithoutRemovingForeignBytes)
{
	TempDir temp("uam-publication-same-metadata-stage");
	ChatSession candidate = PublicationChat();
	const std::string expected = IndependentlyPreparedBytes(candidate);
	const auto before = PublicationFiles(temp.root);
	auto prepared = ChatRepository::PrepareChatPublication(temp.root, candidate, ChatPublicationMode::CreateAbsent, {});
	UAM_ASSERT(prepared);
	fs::path staged_primary;
	for (const auto& [relative, bytes] : PublicationFiles(temp.root))
	{
		if (!before.contains(relative) && bytes == expected)
		{
			UAM_ASSERT(staged_primary.empty());
			staged_primary = temp.root / relative;
		}
	}
	UAM_ASSERT(!staged_primary.empty());
	const auto modified = fs::last_write_time(staged_primary);
	std::string interference = expected;
	const std::size_t title = interference.find("Stored original");
	UAM_ASSERT(title != std::string::npos);
	interference[title] = 'F';
	UAM_ASSERT(uam::io::WriteTextFile(staged_primary, interference));
	fs::last_write_time(staged_primary, modified);
	UAM_ASSERT_EQ(fs::file_size(staged_primary), expected.size());
	UAM_ASSERT_EQ(fs::last_write_time(staged_primary), modified);
	UAM_ASSERT_EQ(ChatRepository::PublishPreparedChatPublication(prepared, [] { return true; }).state, ChatPublicationState::Rejected);
	prepared.reset();
	UAM_ASSERT_EQ(PublicationBytes(staged_primary), interference);
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, candidate.id);
	UAM_ASSERT(!fs::exists(primary) && !fs::exists(uam::io::MakeBackupPath(primary)));
}

UAM_TEST(ChatPublicationLargeUnloadedUpdatePreservesCompleteHistoryPast256MiB)
{
	TempDir temp("uam-publication-large-unloaded-source");
	std::error_code error;
	const auto space = fs::space(temp.root, error);
	UAM_ASSERT(!error && space.available >= 900ULL * 1024 * 1024);
	ChatSession metadata = PublicationChat();
	metadata.messages.clear();
	const auto message_at = [](std::size_t ordinal)
	{
		Message message = PublicationChat().messages.front();
		message.content = "Complete source row " + std::to_string(ordinal) + " " + std::string(1024 * 1024, 'x');
		return message;
	};
	auto imported = ChatRepository::PrepareImportedChatSave(temp.root, metadata,
	    [&](const std::function<bool(const Message&)>& emit, ChatSession&)
	    {
		    for (std::size_t ordinal = 0; ordinal < 260; ++ordinal)
			    if (!emit(message_at(ordinal))) return false;
		    return true;
	    });
	UAM_ASSERT(imported && ChatRepository::PublishPreparedChatSave(imported, true, &metadata));
	imported.reset();
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, metadata.id);
	UAM_ASSERT(fs::file_size(primary) > 256ULL * 1024 * 1024);
	UAM_ASSERT(!metadata.messages_loaded && metadata.messages.empty() && metadata.persisted_message_count == 260);
	for (const std::size_t before : {std::size_t{1}, std::size_t{130}, std::size_t{260}})
	{
		const auto page = ChatRepository::ReadMessagePage(temp.root, metadata, 1, before);
		UAM_ASSERT(page && page->chat.messages.size() == 1);
		UAM_ASSERT_EQ(page->chat.messages.front(), message_at(before - 1));
	}
	metadata.title = "Complete large history without hydration";
	metadata.pinned = true;
	auto prepared = ChatRepository::PrepareChatPublication(temp.root, metadata, ChatPublicationMode::UpdateExisting, PublicationRevision(temp.root, metadata));
	UAM_ASSERT(prepared);
	UAM_ASSERT_EQ(ChatRepository::PublishPreparedChatPublication(prepared, [] { return true; }, &metadata).state, ChatPublicationState::Committed);
	prepared.reset();
	UAM_ASSERT(!metadata.messages_loaded && metadata.messages.empty());
	UAM_ASSERT(fs::file_size(primary) > 256ULL * 1024 * 1024);
	const auto summary = ChatRepository::LoadLocalChat(temp.root, metadata.id, false);
	UAM_ASSERT(summary && summary->pinned && summary->title == metadata.title && summary->persisted_message_count == 260);
	const auto index = uam::chat_pages::ReadIndex(uam::chat_pages::IndexPath(primary), PublicationRevision(temp.root, *summary));
	UAM_ASSERT(index && index->attachment_owner == metadata.id && index->message_count == 260);
	for (const std::size_t before : {std::size_t{1}, std::size_t{130}, std::size_t{260}})
	{
		const auto page = ChatRepository::ReadMessagePage(temp.root, *summary, 1, before);
		UAM_ASSERT(page && page->chat.messages.size() == 1);
		UAM_ASSERT_EQ(page->chat.messages.front(), message_at(before - 1));
	}
	const auto saved_summary = nlohmann::json::parse(PublicationBytes(AppPaths::UamChatSummaryFilePath(temp.root, metadata.id)));
	UAM_ASSERT_EQ(saved_summary.at("summary_source_size").get<std::uintmax_t>(), fs::file_size(primary));
}

UAM_TEST(ChatPublicationPrivateSpoolRetiresOnlyItsOwnUnchangedFile)
{
	TempDir temp("uam-publication-changed-private-spool");
	ChatSession chat = PublicationChat();
	LegacyPreparedPublication(temp.root, chat);
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id);
	const std::string revision = PublicationRevision(temp.root, chat);
	const auto source = uam::chat_pages::ReadIndex(uam::chat_pages::IndexPath(primary), revision);
	UAM_ASSERT(source && source->message_count == 1);
	std::vector<uam::chat_pages::Span> spans;
	UAM_ASSERT(uam::chat_pages::ReadMessageSpans(uam::chat_pages::IndexPath(primary), *source, 0, 1, spans));
	UAM_ASSERT_EQ(spans.size(), std::size_t{1});
	const fs::path records = temp.root / ".uam-publication-33333333-3333-4333-8333-333333333333.records";
	const fs::path staged_index = temp.root / "owned-stage-index";
	const std::string foreign = "Replacement private spool evidence must survive builder destruction";
	{
		uam::chat_pages::Builder builder(primary, chat.id, records);
		UAM_ASSERT(builder.Add(spans.front(), chat.messages.front()));
		UAM_ASSERT(builder.Finish(staged_index, revision, source->source_size, source->array_begin, source->array_end));
		UAM_ASSERT(uam::chat_pages::ReadIndex(staged_index, revision));
		// Finish may already have retired its own spool; neither that nor a later
		// destructor authorizes removing a replacement writer's different bytes.
		UAM_ASSERT(uam::io::AtomicWriteFileDetailed(records, foreign).success);
		UAM_ASSERT_EQ(PublicationBytes(records), foreign);
	}
	UAM_ASSERT_EQ(PublicationBytes(records), foreign);
}

UAM_TEST(ChatPublicationAcceptsTrailingSeparatorRootForCreateAndUpdate)
{
	TempDir legacy("uam-publication-trailing-root-legacy-control");
	ChatSession control = PublicationChat();
	LegacyPreparedPublication(legacy.root / "", control);
	for (const auto mode : {ChatPublicationMode::CreateAbsent, ChatPublicationMode::UpdateExisting})
	{
		TempDir temp("uam-publication-trailing-root");
		const fs::path spelling = temp.root / "";
		ChatSession candidate = PublicationChat();
		std::string revision;
		if (mode == ChatPublicationMode::UpdateExisting)
		{
			SeedPublication(spelling, candidate);
			revision = PublicationRevision(spelling, candidate);
			candidate = PublicationCandidate(candidate);
		}
		const std::string expected = IndependentlyPreparedBytes(candidate);
		auto prepared = ChatRepository::PrepareChatPublication(spelling, candidate, mode, revision);
		UAM_ASSERT(prepared);
		UAM_ASSERT_EQ(ChatRepository::PublishPreparedChatPublication(prepared, [] { return true; }, &candidate).state, ChatPublicationState::Committed);
		prepared.reset();
		UAM_ASSERT_EQ(PublicationBytes(AppPaths::UamChatFilePath(temp.root, candidate.id)), expected);
	}
}

UAM_TEST(ChatPublicationUnfinishedPrivateSpoolRejectsReplacementAndPreservesBytes)
{
	TempDir temp("uam-publication-unfinished-private-spool");
	ChatSession chat = PublicationChat();
	LegacyPreparedPublication(temp.root, chat);
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id);
	const std::string revision = PublicationRevision(temp.root, chat);
	const auto index = uam::chat_pages::ReadIndex(uam::chat_pages::IndexPath(primary), revision);
	UAM_ASSERT(index && index->message_count == 1);
	std::vector<uam::chat_pages::Span> spans;
	UAM_ASSERT(uam::chat_pages::ReadMessageSpans(uam::chat_pages::IndexPath(primary), *index, 0, 1, spans));
	UAM_ASSERT_EQ(spans.size(), std::size_t{1});
	const fs::path records = temp.root / ".uam-publication-44444444-4444-4444-8444-444444444444.records";
	const std::string foreign = "Unfinished spool replacement remains another writer's evidence";
	{
		uam::chat_pages::Builder builder(primary, chat.id, records);
		UAM_ASSERT(builder.Add(spans.front(), chat.messages.front()));
		UAM_ASSERT(uam::io::AtomicWriteFileDetailed(records, foreign).success);
		UAM_ASSERT_EQ(PublicationBytes(records), foreign);
		UAM_ASSERT(!builder.Add(spans.front(), chat.messages.front()));
	}
	UAM_ASSERT_EQ(PublicationBytes(records), foreign);
}
