#include "test_harness.h"
#include "common/chat/chat_message_page_index.h"
#include "common/platform/platform_incremental_sha256.h"

#include <array>
#include <future>
#include "common/platform/published_file_change_witness.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_cold_retry_witness.h"
#include <type_traits>

using namespace uam_test;

static_assert(!std::is_convertible_v<std::shared_ptr<PreparedChatPublication>, std::shared_ptr<PreparedChatSave>>);

namespace
{
	constexpr const char* kBirthB = "33333333-3333-4333-8333-333333333333";
	constexpr const char* kBirthC = "44444444-4444-4444-8444-444444444444";

	ChatSession BirthChat()
	{
		ChatSession chat;
		chat.id = "canonical-birth-storage-fixture";
		chat.provider_id = "codex-cli";
		chat.execution_host_id = "local";
		chat.workspace_directory = "/tmp/uam-canonical-birth-storage-fixture";
		chat.title = "Unbound original";
		chat.created_at = "2026-10-17T10:00:00.000Z";
		chat.updated_at = "2026-10-17T10:00:01.000Z";
		chat.last_opened_at = "2026-10-17T10:00:02.000Z";
		Message message;
		message.role = MessageRole::User;
		message.content = "Authored storage bytes, with trailing spaces.  ";
		message.provider = chat.provider_id;
		message.model_id = "inert-model";
		message.created_at = chat.created_at;
		chat.messages.push_back(std::move(message));
		return chat;
	}

	std::string BirthBytes(const fs::path& path)
	{
		UAM_ASSERT(fs::is_regular_file(path) && !fs::is_symlink(path));
		return ReadFile(path);
	}

	std::string BirthRevision(const fs::path& root, const ChatSession& chat)
	{
		const auto revision = ChatRepository::ChatStorageRevision(root, chat.id);
		UAM_ASSERT(revision && *revision != "missing" && *revision != "unreadable");
		return *revision;
	}

	void BirthReadable(const fs::path& root, const ChatSession& expected)
	{
		const auto loaded = ChatRepository::LoadLocalChatBounded(root, expected.id, 4 * 1024 * 1024);
		UAM_ASSERT(loaded && loaded->id == expected.id);
		UAM_ASSERT_EQ(loaded->managed_followup_incarnation, expected.managed_followup_incarnation);
		UAM_ASSERT_EQ(loaded->messages, expected.messages);
	}

	void BirthCandidateCodecControl(const ChatSession& candidate)
	{
		TempDir independent("uam-birth-independent-candidate-codec");
		UAM_ASSERT(ChatRepository::SaveChat(independent.root, candidate));
		BirthReadable(independent.root, candidate);
	}

	struct BoundStorage
	{
		ChatSession before_ack;
		ChatSession bound;
		std::string primary;
		std::string backup;
	};

	BoundStorage BirthSeedBound(const fs::path& root)
	{
		BoundStorage result;
		result.before_ack = BirthChat();
		UAM_ASSERT(ChatRepository::SaveChat(root, result.before_ack));
		BirthReadable(root, result.before_ack);
		result.bound = result.before_ack;
		// Supplied fixture data proves codec/storage behavior, never owned birth creation.
		result.bound.managed_followup_incarnation = kBirthB;
		result.bound.title = "Bound storage value";
		UAM_ASSERT(ChatRepository::SaveChat(root, result.bound));
		BirthReadable(root, result.bound);
		result.bound.title = "Same-birth ordinary update";
		UAM_ASSERT(ChatRepository::SaveChat(root, result.bound));
		BirthReadable(root, result.bound);
		UAM_ASSERT(result.before_ack.managed_followup_incarnation.empty());
		UAM_ASSERT(result.before_ack.metadata_incarnation == result.bound.metadata_incarnation);
		const fs::path primary = AppPaths::UamChatFilePath(root, result.bound.id);
		result.primary = BirthBytes(primary);
		result.backup = BirthBytes(uam::io::MakeBackupPath(primary));
		UAM_ASSERT_EQ(nlohmann::json::parse(result.primary).at("managed_followup_incarnation").get<std::string>(), std::string(kBirthB));
		UAM_ASSERT_EQ(nlohmann::json::parse(result.backup).at("managed_followup_incarnation").get<std::string>(), std::string(kBirthB));
		UAM_ASSERT(result.primary != result.backup);
		return result;
	}

	void BirthFilesUnchanged(const fs::path& root, const BoundStorage& before)
	{
		const fs::path primary = AppPaths::UamChatFilePath(root, before.bound.id);
		UAM_ASSERT_EQ(BirthBytes(primary), before.primary);
		UAM_ASSERT_EQ(BirthBytes(uam::io::MakeBackupPath(primary)), before.backup);
	}

	void BirthStaleFingerprintControl(const fs::path& root, const BoundStorage& stored, const ChatSession& stale)
	{
		UAM_ASSERT(!ChatRepository::ChatMetadataFingerprint(root, stale).empty());
		BirthFilesUnchanged(root, stored);
	}

	struct BirthFileWitness
	{
		std::uintmax_t size = 0;
		fs::file_time_type modified;
		std::string digest;
		bool operator==(const BirthFileWitness&) const = default;
	};

	BirthFileWitness BirthWitness(const fs::path& path)
	{
		UAM_ASSERT(fs::is_regular_file(path) && !fs::is_symlink(path));
		BirthFileWitness result{fs::file_size(path), fs::last_write_time(path), {}};
		std::ifstream input;
		UAM_ASSERT(uam::io::OpenReadFile(path, input));
		uam::platform::IncrementalSha256 hash;
		std::array<char, 65536> buffer{};
		std::uintmax_t count = 0;
		while (input.read(buffer.data(), buffer.size()) || input.gcount() != 0)
		{
			count += static_cast<std::uintmax_t>(input.gcount());
			UAM_ASSERT(hash.Add(std::string_view(buffer.data(), static_cast<std::size_t>(input.gcount()))));
		}
		UAM_ASSERT(input.eof() && !input.bad() && count == result.size);
		result.digest = hash.Finish();
		UAM_ASSERT(!result.digest.empty());
		UAM_ASSERT_EQ(fs::file_size(path), result.size);
		UAM_ASSERT_EQ(fs::last_write_time(path), result.modified);
		return result;
	}

	std::string BirthFileEnd(const fs::path& path, bool tail)
	{
		UAM_ASSERT(fs::file_size(path) >= 65536);
		std::ifstream input;
		UAM_ASSERT(uam::io::OpenReadFile(path, input));
		if (tail) input.seekg(-static_cast<std::streamoff>(65536), std::ios::end);
		std::string bytes(65536, '\0');
		UAM_ASSERT(input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())));
		return bytes;
	}
}

UAM_TEST(ChatBirthPreservationOrdinaryBoundAndUnboundStorageControls)
{
	TempDir legacy("uam-birth-empty-legacy-controls");
	ChatSession unbound = BirthChat();
	UAM_ASSERT(ChatRepository::SaveChat(legacy.root, unbound));
	unbound.title = "Ordinary empty-birth update";
	UAM_ASSERT(ChatRepository::SaveChat(legacy.root, unbound));
	auto old_prepared = ChatRepository::PrepareChatSave(legacy.root, unbound, BirthRevision(legacy.root, unbound));
	UAM_ASSERT(old_prepared && ChatRepository::PublishPreparedChatSave(old_prepared, true, &unbound));
	old_prepared.reset();
	unbound.title = "Detailed empty-birth update";
	auto empty_prepared = ChatRepository::PrepareChatPublication(legacy.root, unbound, ChatPublicationMode::UpdateExisting, BirthRevision(legacy.root, unbound));
	UAM_ASSERT(empty_prepared);
	UAM_ASSERT_EQ(ChatRepository::PublishPreparedChatPublication(empty_prepared, [] { return true; }, &unbound).state, ChatPublicationState::Committed);
	empty_prepared.reset();
	UAM_ASSERT(unbound.managed_followup_incarnation.empty());
	BirthReadable(legacy.root, unbound);

	TempDir bound("uam-birth-bound-positive-controls");
	BoundStorage stored = BirthSeedBound(bound.root);
	stored.bound.title = "Same-B legacy prepared update";
	auto prepared = ChatRepository::PrepareChatSave(bound.root, stored.bound, BirthRevision(bound.root, stored.bound));
	UAM_ASSERT(prepared && ChatRepository::PublishPreparedChatSave(prepared, true, &stored.bound));
	prepared.reset();
	BirthReadable(bound.root, stored.bound);
	stored.bound.title = "Same-B detailed prepared update";
	auto detailed = ChatRepository::PrepareChatPublication(bound.root, stored.bound, ChatPublicationMode::UpdateExisting, BirthRevision(bound.root, stored.bound));
	UAM_ASSERT(detailed);
	UAM_ASSERT_EQ(ChatRepository::PublishPreparedChatPublication(detailed, [] { return true; }, &stored.bound).state, ChatPublicationState::Committed);
	detailed.reset();
	BirthReadable(bound.root, stored.bound);
	const auto primary = AppPaths::UamChatFilePath(bound.root, stored.bound.id);
	const std::string bytes = BirthBytes(primary), backup = BirthBytes(uam::io::MakeBackupPath(primary));
	UAM_ASSERT(!ChatRepository::ChatMetadataFingerprint(bound.root, stored.before_ack).empty());
	UAM_ASSERT_EQ(BirthBytes(primary), bytes);
	UAM_ASSERT_EQ(BirthBytes(uam::io::MakeBackupPath(primary)), backup);
}

UAM_TEST(ChatBirthPreservationUnmarkedLegacyCorruptionStillRecoversBackup)
{
	TempDir temp("uam-birth-unmarked-legacy-recovery-control");
	ChatSession chat = BirthChat();
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const ChatSession known_backup = chat;
	chat.title = "Later unbound primary";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id);
	const auto backup = nlohmann::json::parse(BirthBytes(uam::io::MakeBackupPath(primary)));
	UAM_ASSERT(!backup.contains("managed_followup_incarnation"));
	UAM_ASSERT_EQ(backup.at("messages").front().at("content").get<std::string>(), known_backup.messages.front().content);
	// The existing supported unmarked legacy corruption case; no historical B is inferred.
	UAM_ASSERT(uam::io::WriteTextFile(primary, "{"));
	const auto loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.size(), std::size_t{1});
	UAM_ASSERT_EQ(loaded.front().id, known_backup.id);
	UAM_ASSERT_EQ(loaded.front().title, known_backup.title);
	UAM_ASSERT(loaded.front().managed_followup_incarnation.empty());
	UAM_ASSERT_EQ(loaded.front().messages, known_backup.messages);
	BirthReadable(temp.root, known_backup);
}

UAM_TEST(ChatBirthPreservationDirectSaveRejectsBlankBirth)
{
	TempDir temp("uam-birth-direct-blank");
	const BoundStorage stored = BirthSeedBound(temp.root);
	ChatSession stale = stored.before_ack;
	stale.title = "Unsaved blank-birth metadata";
	BirthCandidateCodecControl(stale);
	BirthStaleFingerprintControl(temp.root, stored, stale);
	UAM_ASSERT(!ChatRepository::SaveChat(temp.root, stale)); // Intended behavioral RED.
	BirthFilesUnchanged(temp.root, stored);
}

UAM_TEST(ChatBirthPreservationDirectSaveRejectsDifferentBirth)
{
	TempDir temp("uam-birth-direct-different");
	const BoundStorage stored = BirthSeedBound(temp.root);
	ChatSession stale = stored.bound;
	stale.managed_followup_incarnation = kBirthC;
	stale.title = "Unsaved different-birth metadata";
	BirthCandidateCodecControl(stale);
	BirthStaleFingerprintControl(temp.root, stored, stale);
	UAM_ASSERT(!ChatRepository::SaveChat(temp.root, stale)); // Separate intended behavioral RED.
	BirthFilesUnchanged(temp.root, stored);
}

UAM_TEST(ChatBirthPreservationLegacyPreparedRejectsFreshRevisionBlankBirth)
{
	TempDir temp("uam-birth-legacy-fresh-revision");
	const BoundStorage stored = BirthSeedBound(temp.root);
	ChatSession stale = stored.before_ack;
	stale.title = "Blank snapshot captured before live ACK";
	BirthCandidateCodecControl(stale);
	BirthStaleFingerprintControl(temp.root, stored, stale);
	const std::string current_revision = BirthRevision(temp.root, stored.bound);
	auto prepared = ChatRepository::PrepareChatSave(temp.root, stale, current_revision);
	bool rejected = !prepared; // A future preparation-time refusal is valid.
	if (prepared)
	{
		UAM_ASSERT_EQ(prepared->expected_storage_revision, current_revision);
		BirthFilesUnchanged(temp.root, stored);
		rejected = !ChatRepository::PublishPreparedChatSave(prepared, true, &stale, [] { return true; });
	}
	UAM_ASSERT(rejected); // Intended RED even though the revision was captured AFTER B.
	prepared.reset();
	BirthFilesUnchanged(temp.root, stored);
}

UAM_TEST(ChatBirthPreservationDetailedRejectsFreshRevisionBlankBirth)
{
	TempDir temp("uam-birth-detailed-fresh-revision");
	const BoundStorage stored = BirthSeedBound(temp.root);
	ChatSession stale = stored.before_ack;
	stale.title = "Blank snapshot with latest canonical revision";
	BirthCandidateCodecControl(stale);
	BirthStaleFingerprintControl(temp.root, stored, stale);
	const std::string current_revision = BirthRevision(temp.root, stored.bound);
	auto prepared = ChatRepository::PrepareChatPublication(temp.root, stale, ChatPublicationMode::UpdateExisting, current_revision);
	bool rejected = !prepared;
	if (prepared)
	{
		BirthFilesUnchanged(temp.root, stored);
		rejected = ChatRepository::PublishPreparedChatPublication(prepared, [] { return true; }, &stale).state == ChatPublicationState::Rejected;
	}
	UAM_ASSERT(rejected); // Intended RED; Uncertain is not a known rejection.
	prepared.reset();
	BirthFilesUnchanged(temp.root, stored);
}

UAM_TEST(ChatBirthPreservationLegacyPublisherChecksActualCandidateStage)
{
	TempDir temp("uam-birth-legacy-actual-stage");
	const BoundStorage stored = BirthSeedBound(temp.root);
	ChatSession candidate = stored.bound;
	candidate.title = "Valid same-B staged metadata";
	auto prepared = ChatRepository::PrepareChatSave(temp.root, candidate, BirthRevision(temp.root, stored.bound));
	UAM_ASSERT(prepared && !prepared->primary_temp.empty());
	auto actual = nlohmann::json::parse(BirthBytes(prepared->primary_temp));
	UAM_ASSERT_EQ(actual.at("managed_followup_incarnation").get<std::string>(), std::string(kBirthB));
	actual.erase("managed_followup_incarnation");
	UAM_ASSERT(uam::io::WriteTextFile(prepared->primary_temp, actual.dump(2)));
	UAM_ASSERT(!nlohmann::json::parse(BirthBytes(prepared->primary_temp)).contains("managed_followup_incarnation"));
	UAM_ASSERT_EQ(candidate.managed_followup_incarnation, std::string(kBirthB));
	BirthFilesUnchanged(temp.root, stored);
	UAM_ASSERT(!ChatRepository::PublishPreparedChatSave(prepared, true, &candidate, [] { return true; }));
	prepared.reset();
	BirthFilesUnchanged(temp.root, stored);
}

UAM_TEST(ChatBirthPreservationLatePrimaryBirthSurvivesMissingIndexAndBlankSummary)
{
	TempDir positive("uam-birth-large-small-positive-control");
	(void)BirthSeedBound(positive.root);
	TempDir temp("uam-birth-large-late-primary");
	std::error_code error;
	const auto space = fs::space(temp.root, error);
	UAM_ASSERT(!error && space.available >= 900ULL * 1024 * 1024);
	ChatSession metadata = BirthChat();
	metadata.messages.clear();
	metadata.managed_followup_incarnation = kBirthB;
	const auto message_at = [](std::size_t ordinal)
	{
		Message message = BirthChat().messages.front();
		message.content = "Canonical late-birth row " + std::to_string(ordinal) + " " + std::string(1024 * 1024, 'x');
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
	const fs::path backup = uam::io::MakeBackupPath(primary);
	const fs::path index = uam::chat_pages::IndexPath(primary);
	const fs::path summary = AppPaths::UamChatSummaryFilePath(temp.root, metadata.id);
	UAM_ASSERT(fs::file_size(primary) > 256ULL * 1024 * 1024);
	UAM_ASSERT(!fs::exists(backup));
	UAM_ASSERT(!metadata.messages_loaded && metadata.messages.empty() && metadata.persisted_message_count == 260);
	const std::string prefix = BirthFileEnd(primary, false), tail = BirthFileEnd(primary, true);
	UAM_ASSERT(prefix.starts_with("{\"messages\":["));
	UAM_ASSERT(prefix.find("\"managed_followup_incarnation\"") == std::string::npos);
	UAM_ASSERT(tail.find("\"managed_followup_incarnation\"") != std::string::npos && tail.find(kBirthB) != std::string::npos);
	for (const std::size_t before : {std::size_t{1}, std::size_t{130}, std::size_t{260}})
	{
		const auto page = ChatRepository::ReadMessagePage(temp.root, metadata, 1, before);
		UAM_ASSERT(page && page->chat.messages.size() == 1);
		UAM_ASSERT_EQ(page->chat.managed_followup_incarnation, std::string(kBirthB));
		UAM_ASSERT_EQ(page->chat.messages.front(), message_at(before - 1));
	}
	UAM_ASSERT(fs::remove(index, error) && !error);
	// A derived cache cannot establish birth authority. Leave canonical B untouched.
	auto derived = nlohmann::json::parse(BirthBytes(summary));
	UAM_ASSERT_EQ(derived.at("managed_followup_incarnation").get<std::string>(), std::string(kBirthB));
	derived.erase("managed_followup_incarnation");
	UAM_ASSERT(uam::io::WriteTextFile(summary, derived.dump(2)));
	const std::string summary_bytes = BirthBytes(summary);
	UAM_ASSERT(!nlohmann::json::parse(summary_bytes).contains("managed_followup_incarnation"));
	const BirthFileWitness witness = BirthWitness(primary);
	ChatSession stale = metadata;
	stale.managed_followup_incarnation.clear();
	stale.title = "Blank metadata cannot replace a late canonical birth";
	UAM_ASSERT(!ChatRepository::ChatMetadataFingerprint(temp.root, stale).empty());
	UAM_ASSERT_EQ(BirthWitness(primary), witness);
	const std::string fresh_revision = BirthRevision(temp.root, metadata);
	auto prepared = ChatRepository::PrepareChatPublication(temp.root, stale, ChatPublicationMode::UpdateExisting, fresh_revision);
	bool rejected = !prepared;
	if (prepared)
	{
		UAM_ASSERT_EQ(BirthWitness(primary), witness);
		UAM_ASSERT(!fs::exists(index) && !fs::exists(backup));
		rejected = ChatRepository::PublishPreparedChatPublication(prepared, [] { return true; }, &stale).state == ChatPublicationState::Rejected;
	}
	UAM_ASSERT(rejected); // Intended RED after real >256MiB/index/page/late-field prerequisites.
	prepared.reset();
	UAM_ASSERT_EQ(BirthWitness(primary), witness);
	UAM_ASSERT(!fs::exists(index) && !fs::exists(backup));
	UAM_ASSERT_EQ(BirthBytes(summary), summary_bytes);
}

namespace
{
	std::string BirthR59Document(std::string_view seed, std::string_view birth_fields)
	{
		auto value = nlohmann::json::parse(seed);
		value.erase("managed_followup_incarnation");
		std::string bytes = value.dump(2);
		UAM_ASSERT(!bytes.empty() && bytes.back() == '}');
		bytes.pop_back();
		bytes += "," + std::string(birth_fields) + "}";
		UAM_ASSERT(nlohmann::json::accept(bytes));
		UAM_ASSERT_EQ(nlohmann::json::parse(bytes).at("id").get<std::string>(), BirthChat().id);
		return bytes;
	}

	std::vector<std::string> BirthR59MalformedFields()
	{
		return {
		    R"("managed_followup_incarnation":37)",
		    R"("managed_followup_incarnation":null)",
		    R"("managed_followup_incarnation":true)",
		    R"("managed_followup_incarnation":[])",
		    R"("managed_followup_incarnation":{})",
		    R"("managed_followup_incarnation":"not-a-canonical-birth")",
		    R"("managed_followup_incarnation":"33333333-3333-4333-8333-333333333333","managed_followup_incarnation":"33333333-3333-4333-8333-333333333333")",
		    R"("managed_followup_incarnation":"33333333-3333-4333-8333-333333333333","managed_followup_incarnation":"44444444-4444-4444-8444-444444444444")",
		    R"("managed_followup_incarnation":"33333333-3333-4333-8333-333333333333","managed_followup_\u0069ncarnation":"44444444-4444-4444-8444-444444444444")",
		    R"("managed_followup_incarnation":"","managed_followup_incarnation":"33333333-3333-4333-8333-333333333333")",
		    R"("managed_followup_incarnation":"33333333-3333-4333-8333-333333333333","managed_followup_incarnation":"")"
		};
	}

	void BirthR59FingerprintIndependentOfPrimary(const fs::path& root, const ChatSession& candidate)
	{
		TempDir absent("uam-birth-recognition-fingerprint-no-storage");
		const auto expected = ChatRepository::ChatMetadataFingerprint(absent.root, candidate);
		UAM_ASSERT(!expected.empty() && fs::is_empty(absent.root));
		UAM_ASSERT_EQ(ChatRepository::ChatMetadataFingerprint(root, candidate), expected);
		UAM_ASSERT(fs::is_empty(absent.root));
	}

	// No fallback load is used to classify the incumbent; capture its actual raw bytes.
	void BirthR59SetPrimary(const fs::path& root, BoundStorage& stored, const std::string& bytes)
	{
		const auto primary = AppPaths::UamChatFilePath(root, stored.bound.id);
		UAM_ASSERT(uam::io::WriteTextFile(primary, bytes));
		stored.primary = BirthBytes(primary);
		UAM_ASSERT_EQ(stored.primary, bytes);
		BirthFilesUnchanged(root, stored);
	}

	void BirthR59RejectReplacement(const fs::path& root, const BoundStorage& stored, ChatSession candidate, int writer)
	{
		BirthCandidateCodecControl(candidate);
		BirthR59FingerprintIndependentOfPrimary(root, candidate);
		BirthFilesUnchanged(root, stored);
		if (writer == 0)
		{
			UAM_ASSERT(!ChatRepository::SaveChat(root, candidate));
		}
		else
		{
			const std::string revision = BirthRevision(root, stored.bound); // Fresh AFTER raw primary installation.
			if (writer == 1)
			{
				auto prepared = ChatRepository::PrepareChatSave(root, candidate, revision);
				if (prepared)
				{
					UAM_ASSERT_EQ(prepared->expected_storage_revision, revision);
					BirthFilesUnchanged(root, stored);
					UAM_ASSERT(!ChatRepository::PublishPreparedChatSave(prepared, true, &candidate, [] { return true; }));
				}
			}
			else
			{
				auto prepared = ChatRepository::PrepareChatPublication(root, candidate, ChatPublicationMode::UpdateExisting, revision);
				if (prepared)
				{
					BirthFilesUnchanged(root, stored);
					UAM_ASSERT_EQ(ChatRepository::PublishPreparedChatPublication(prepared, [] { return true; }, &candidate).state, ChatPublicationState::Rejected);
				}
			}
		}
		BirthFilesUnchanged(root, stored);
	}
}

UAM_TEST(ChatBirthRecognitionExactEmptyAndNestedLookalikesRemainUnbound)
{
	TempDir temp("uam-birth-recognition-exact-empty");
	ChatSession candidate = BirthChat();
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, candidate));
	const auto primary = AppPaths::UamChatFilePath(temp.root, candidate.id);
	auto root = nlohmann::json::parse(BirthBytes(primary));
	root["managed_followup_incarnation"] = "";
	root["unrelated_storage_fixture"] = {{"managed_followup_incarnation", 37}};
	UAM_ASSERT(uam::io::WriteTextFile(primary, root.dump(2)));
	BirthReadable(temp.root, candidate); // Exact empty is codec-valid, unlike malformed-present.
	const std::string empty_bytes = BirthBytes(primary);
	BirthR59FingerprintIndependentOfPrimary(temp.root, candidate);
	UAM_ASSERT_EQ(BirthBytes(primary), empty_bytes);
	candidate.title = "Ordinary replacement of exact-empty unbound primary";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, candidate));
	BirthReadable(temp.root, candidate);
	UAM_ASSERT(!nlohmann::json::parse(BirthBytes(primary)).contains("managed_followup_incarnation"));

	// A nested field with the same spelling is not a top-level binding.
	root = nlohmann::json::parse(BirthBytes(primary));
	root["unrelated_storage_fixture"] = {{"managed_followup_incarnation", 37}};
	UAM_ASSERT(uam::io::WriteTextFile(primary, root.dump(2)));
	auto prepared = ChatRepository::PrepareChatSave(temp.root, candidate, BirthRevision(temp.root, candidate));
	UAM_ASSERT(prepared && ChatRepository::PublishPreparedChatSave(prepared, true, &candidate));
	prepared.reset();
	BirthReadable(temp.root, candidate);
}

UAM_TEST(ChatBirthRecognitionUnmarkedInvalidMessagesKeepsLegacyBackupRecovery)
{
	TempDir temp("uam-birth-recognition-unmarked-body-recovery");
	ChatSession chat = BirthChat();
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const ChatSession known_backup = chat;
	chat.title = "Later ordinary unbound primary";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const auto primary = AppPaths::UamChatFilePath(temp.root, chat.id);
	auto raw = nlohmann::json::parse(BirthBytes(primary));
	UAM_ASSERT(!raw.contains("managed_followup_incarnation"));
	const auto backup = nlohmann::json::parse(BirthBytes(uam::io::MakeBackupPath(primary)));
	UAM_ASSERT(!backup.contains("managed_followup_incarnation"));
	UAM_ASSERT_EQ(backup.at("messages").front().at("content").get<std::string>(), known_backup.messages.front().content);
	raw["messages"] = {{"unexpected", true}};
	UAM_ASSERT(uam::io::WriteTextFile(primary, raw.dump(2)));
	UAM_ASSERT(!ChatRepository::LoadLocalChatBounded(temp.root, chat.id, 4 * 1024 * 1024));
	const auto loaded = ChatRepository::LoadLocalChats(temp.root);
	UAM_ASSERT_EQ(loaded.size(), std::size_t{1});
	UAM_ASSERT_EQ(loaded.front().id, known_backup.id);
	UAM_ASSERT_EQ(loaded.front().title, known_backup.title);
	UAM_ASSERT(loaded.front().managed_followup_incarnation.empty());
	UAM_ASSERT_EQ(loaded.front().messages, known_backup.messages);
	BirthReadable(temp.root, known_backup);
}

UAM_TEST(ChatBirthRecognitionTypedPrimarySurvivesUnrelatedInvalidMessages)
{
	for (const int writer : {0, 1, 2})
	{
		for (const bool different_birth : {false, true})
		{
			TempDir temp("uam-birth-recognition-typed-invalid-body");
			BoundStorage stored = BirthSeedBound(temp.root);
			auto raw = nlohmann::json::parse(stored.primary);
			UAM_ASSERT_EQ(raw.at("managed_followup_incarnation").get<std::string>(), std::string(kBirthB));
			raw["messages"] = {{"unexpected", true}};
			BirthR59SetPrimary(temp.root, stored, raw.dump(2));
			UAM_ASSERT(!ChatRepository::LoadLocalChatBounded(temp.root, stored.bound.id, 4 * 1024 * 1024));
			BirthFilesUnchanged(temp.root, stored); // B exists despite whole-chat semantic failure.
			ChatSession candidate = stored.before_ack;
			if (different_birth) candidate.managed_followup_incarnation = kBirthC;
			candidate.title = "Full save cannot erase typed B through an unrelated invalid body";
			BirthR59RejectReplacement(temp.root, stored, candidate, writer);
		}
	}
}

UAM_TEST(ChatBirthRecognitionMalformedPresentPrimaryRefusesFullReplacement)
{
	for (const auto& fields : BirthR59MalformedFields())
	{
		for (const int writer : {0, 1, 2})
		{
			TempDir temp("uam-birth-recognition-malformed-primary");
			BoundStorage stored = BirthSeedBound(temp.root);
			BirthR59SetPrimary(temp.root, stored, BirthR59Document(stored.primary, fields));
			ChatSession candidate = stored.before_ack;
			candidate.title = "Malformed present is not unbound legacy permission";
			BirthR59RejectReplacement(temp.root, stored, candidate, writer);
		}
	}
}

UAM_TEST(ChatBirthRecognitionLegacyPublisherRejectsMalformedActualStage)
{
	for (const auto& fields : BirthR59MalformedFields())
	{
		TempDir temp("uam-birth-recognition-malformed-stage");
		const BoundStorage stored = BirthSeedBound(temp.root);
		ChatSession candidate = stored.bound;
		candidate.title = "Same-B candidate before actual stage mutation";
		UAM_ASSERT_EQ(candidate.managed_followup_incarnation, std::string(kBirthB));
		const auto primary = AppPaths::UamChatFilePath(temp.root, candidate.id);
		const std::string before = BirthBytes(primary), backup = BirthBytes(uam::io::MakeBackupPath(primary));
		auto prepared = ChatRepository::PrepareChatSave(temp.root, candidate, BirthRevision(temp.root, candidate));
		UAM_ASSERT(prepared && !prepared->primary_temp.empty());
		const std::string actual = BirthR59Document(BirthBytes(prepared->primary_temp), fields);
		UAM_ASSERT(uam::io::WriteTextFile(prepared->primary_temp, actual));
		UAM_ASSERT_EQ(BirthBytes(prepared->primary_temp), actual);
		UAM_ASSERT_EQ(candidate.managed_followup_incarnation, std::string(kBirthB));
		BirthR59FingerprintIndependentOfPrimary(temp.root, candidate);
		UAM_ASSERT_EQ(BirthBytes(primary), before);
		UAM_ASSERT_EQ(BirthBytes(uam::io::MakeBackupPath(primary)), backup);
		UAM_ASSERT(!ChatRepository::PublishPreparedChatSave(prepared, true, &candidate, [] { return true; }));
		prepared.reset();
		UAM_ASSERT_EQ(BirthBytes(primary), before);
		UAM_ASSERT_EQ(BirthBytes(uam::io::MakeBackupPath(primary)), backup);
	}
}

UAM_TEST(ChatBirthRecognitionSummaryOnlyPublicationPreservesCanonicalPrimary)
{
	TempDir temp("uam-birth-recognition-summary-only");
	const BoundStorage stored = BirthSeedBound(temp.root);
	ChatSession stale = stored.before_ack;
	stale.last_opened_at = "2026-10-17T10:01:00.000Z";
	auto prepared = ChatRepository::PrepareLastOpenedAt(temp.root, stale);
	UAM_ASSERT(prepared && prepared->primary_temp.empty() && !prepared->summary_temp.empty());
	BirthFilesUnchanged(temp.root, stored);
	UAM_ASSERT(ChatRepository::PublishPreparedChatSave(prepared, true, &stale, [] { return true; }));
	prepared.reset();
	BirthFilesUnchanged(temp.root, stored);
	const auto summary = nlohmann::json::parse(BirthBytes(AppPaths::UamChatSummaryFilePath(temp.root, stale.id)));
	UAM_ASSERT_EQ(summary.at("managed_followup_incarnation").get<std::string>(), std::string(kBirthB));
	UAM_ASSERT_EQ(summary.at("last_opened_at").get<std::string>(), stale.last_opened_at);
}

UAM_TEST(ChatBirthRecognitionUnsafePrimaryRefusesAndFingerprintRemainsUsable)
{
	TempDir temp("uam-birth-recognition-nonregular");
	ChatSession candidate = BirthChat();
	BirthCandidateCodecControl(candidate);
	const auto primary = AppPaths::UamChatFilePath(temp.root, candidate.id);
	fs::create_directories(primary);
	const auto sentinel = primary / "foreign-evidence";
	UAM_ASSERT(uam::io::WriteTextFile(sentinel, "retain this nonregular primary evidence"));
	BirthR59FingerprintIndependentOfPrimary(temp.root, candidate);
	UAM_ASSERT(!ChatRepository::SaveChat(temp.root, candidate));
	UAM_ASSERT(fs::is_directory(primary));
	UAM_ASSERT_EQ(BirthBytes(sentinel), std::string("retain this nonregular primary evidence"));
	UAM_ASSERT(!fs::exists(uam::io::MakeBackupPath(primary)));

	// A regular-file ancestor makes storage stat/open fail without permission assumptions.
	TempDir blocked("uam-birth-recognition-uninspectable-ancestor");
	const fs::path blocker = blocked.root / "not-a-data-directory";
	UAM_ASSERT(uam::io::WriteTextFile(blocker, "foreign ancestor"));
	BirthR59FingerprintIndependentOfPrimary(blocker, candidate);
	UAM_ASSERT(!ChatRepository::SaveChat(blocker, candidate));
	UAM_ASSERT_EQ(BirthBytes(blocker), std::string("foreign ancestor"));
#if !defined(_WIN32)
	// No Windows symlink privilege is assumed. These are file evidence, never provider processes.
	for (const bool dangling : {true, false})
	{
		TempDir linked("uam-birth-recognition-link-evidence");
		const auto path = AppPaths::UamChatFilePath(linked.root, candidate.id);
		fs::create_directories(path.parent_path());
		const fs::path target = dangling ? linked.root / "missing-target" : path;
		fs::create_symlink(target, path);
		UAM_ASSERT(fs::is_symlink(fs::symlink_status(path)));
		UAM_ASSERT_EQ(fs::read_symlink(path), target);
		std::error_code error;
		(void)fs::status(path, error);
		if (dangling) UAM_ASSERT(!fs::exists(fs::status(path, error)));
		else UAM_ASSERT(error); // A loop/stat error never establishes writable absence.
		BirthR59FingerprintIndependentOfPrimary(linked.root, candidate);
		UAM_ASSERT(!ChatRepository::SaveChat(linked.root, candidate));
		UAM_ASSERT(fs::is_symlink(fs::symlink_status(path)));
		UAM_ASSERT_EQ(fs::read_symlink(path), target);
		UAM_ASSERT(!fs::exists(uam::io::MakeBackupPath(path)));
	}
#endif
}


namespace uam
{
	// Existing pure observer of the actual preparation future; never creates or harvests one.
	bool PendingChatSavePreparationForTests(const AppState& app, const std::string& chat_id, bool& ready);
}

UAM_TEST(ChatBindingOwnPublicationRejectsAtomicSameSizeTimePrimaryReplacement)
{
	TempDir temp("uam-binding-physical-publication");
	ChatSession chat = BirthChat();
	SavedChatPrimaryWitness own;
	UAM_ASSERT(ChatRepository::SaveLoadedChatWithPrimaryWitness(temp.root, chat, own));
	UAM_ASSERT(ChatRepository::MatchesSavedChatPrimaryWitness(temp.root, chat.id, own));
	chat.native_session_id = kBirthB; // Typed storage DATA, not a native execution receipt.
	auto worker = std::async(std::launch::async, [root = temp.root, chat, own]()
	{
		auto prepared = ChatRepository::PrepareLoadedChatBindingSave(root, chat, own);
		if (!prepared || !ChatRepository::PublishPreparedChatSave(prepared)) return std::shared_ptr<PreparedChatSave>{};
		return prepared;
	});
	const auto published = worker.get(); // The real worker has returned BEFORE the current-owner check.
	UAM_ASSERT(published && ChatRepository::BindingPublicationStillCurrent(published));
	UAM_ASSERT(published->binding_emitted_primary);
	UAM_ASSERT(ChatRepository::MatchesSavedChatPrimaryWitness(temp.root, chat.id, *published->binding_emitted_primary));
	const auto path = AppPaths::UamChatFilePath(temp.root, chat.id);
	const auto own_primary = ReadFile(path);
	const auto own_revision = ChatRepository::ChatStorageRevision(temp.root, chat.id);
	const auto timestamp = fs::last_write_time(path);
	const auto summary_path = AppPaths::UamChatSummaryFilePath(temp.root, chat.id);
	const auto summary = ReadFile(summary_path);
	const auto index_path = uam::chat_pages::IndexPath(path);
	const auto index = ReadFile(index_path);
	std::string replacement = own_primary;
	const std::string previous = "Authored storage bytes, with trailing spaces.  ";
	const auto at = replacement.find(previous);
	UAM_ASSERT(at != std::string::npos);
	replacement.replace(at, previous.size(), "Replaced storage bytes, with trailing spaces.  ");
	UAM_ASSERT(replacement != own_primary && replacement.size() == own_primary.size());
	UAM_ASSERT(uam::io::WriteTextFile(path, replacement)); // Actual atomic replacement, not an injected receipt.
	fs::last_write_time(path, timestamp);
	UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(temp.root, chat.id), own_revision); // Old weak callback passes.
	UAM_ASSERT(!ChatRepository::BindingPublicationStillCurrent(published)); // Actual physical-object guard used by callback.
	UAM_ASSERT(!ChatRepository::MatchesSavedChatPrimaryWitness(temp.root, chat.id, *published->binding_emitted_primary));
	UAM_ASSERT_EQ(ReadFile(path), replacement);
	UAM_ASSERT_EQ(ReadFile(summary_path), summary);
	UAM_ASSERT_EQ(ReadFile(index_path), index);
}

UAM_TEST(ChatBindingPendingPreparationPreservesNewerOrdinarySaveSchedule)
{
	TempDir temp("uam-binding-newer-normal-save");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat = BirthChat();
	chat.native_session_id = kBirthB;
	SavedChatPrimaryWitness own;
	UAM_ASSERT(ChatRepository::SaveLoadedChatWithPrimaryWitness(temp.root, chat, own));
	app.chats.push_back(chat);
	app.composer_text = "Preserve the existing live draft.";
	const auto initial_messages = app.chats.front().messages;
	int binding_callbacks = 0, ordinary_callbacks = 0;
	bool ordinary_saved = false;
	uam::acp_detail::DurableAdmissionState binding_state = uam::acp_detail::DurableAdmissionState::Pending;
	struct Drain
	{
		uam::AppState& app;
		~Drain()
		{
			const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while ((!app.pending_chat_save_tasks.empty() || !app.pending_chat_save_at_by_chat_id.empty()) && std::chrono::steady_clock::now() < end)
			{
				(void)uam::FlushPendingChatSaves(app);
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			if (!app.pending_chat_save_tasks.empty() || !app.pending_chat_save_at_by_chat_id.empty())
			{
				std::cerr << "FATAL_PRIVATE_BINDING_SAVE_DRAIN: actual save work did not settle" << std::endl;
				std::abort(); // Keep callback/App/root lifetime intact on failed cleanup.
			}
		}
	} drain{app};
	const auto owned = [&]()
	{
		return app.data_root == temp.root && app.chats.size() == 1 && app.chats.front().id == chat.id &&
		    app.chats.front().native_session_id == kBirthB && app.chats.front().messages == initial_messages;
	};
	UAM_ASSERT(uam::acp_detail::BeginDurableBindingAdmission(app, app.chats.front(), own, owned,
	    [&](uam::acp_detail::DurableBindingAdmissionReceipt receipt)
	    { ++binding_callbacks; binding_state = receipt.state; }) == uam::acp_detail::DurableAdmissionState::Pending);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	bool actual_preparation = false;
	while (std::chrono::steady_clock::now() < deadline)
	{
		(void)uam::FlushPendingChatSaves(app);
		bool ready = false;
		if (uam::PendingChatSavePreparationForTests(app, chat.id, ready)) { actual_preparation = true; break; }
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT(actual_preparation && binding_callbacks == 0); // Real future exists, never harvested/fabricated here.
	ChatDomainService().AddMessage(app.chats.front(), MessageRole::User, "Genuine newer authored work must be saved.");
	app.chats.front().title = "Newer ordinary metadata";
	const auto newer = app.chats.front().messages;
	uam::acp_detail::ScheduleChatSave(app, app.chats.front(), 0.0);
	UAM_ASSERT(uam::DeferChatSaveResult(app, chat.id, [&](bool saved) { ++ordinary_callbacks; ordinary_saved = saved; }));
	while (std::chrono::steady_clock::now() < deadline &&
	    (ordinary_callbacks == 0 || binding_callbacks == 0 || !app.pending_chat_save_tasks.empty() || !app.pending_chat_save_at_by_chat_id.empty()))
	{
		(void)uam::FlushPendingChatSaves(app);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT_EQ(binding_callbacks, 1);
	UAM_ASSERT(binding_state == uam::acp_detail::DurableAdmissionState::Failed);
	UAM_ASSERT_EQ(ordinary_callbacks, 1);
	UAM_ASSERT(ordinary_saved && app.pending_chat_save_tasks.empty() && app.pending_chat_save_at_by_chat_id.empty());
	const auto saved = ChatRepository::LoadLocalChat(temp.root, chat.id, true);
	UAM_ASSERT(saved && saved->messages == newer && app.chats.front().messages == newer);
	UAM_ASSERT_EQ(saved->title, std::string("Newer ordinary metadata"));
	UAM_ASSERT_EQ(saved->native_session_id, std::string(kBirthB));
	UAM_ASSERT_EQ(app.composer_text, std::string("Preserve the existing live draft."));
}


#if defined(__APPLE__) || defined(__linux__)
UAM_TEST(ChatBindingHeldObjectHashRefusesReplacedFifoAndSymlinkWithoutBlocking)
{
	TempDir temp("uam-binding-held-nofollow");
	ChatSession chat = BirthChat();
	SavedChatPrimaryWitness own;
	UAM_ASSERT(ChatRepository::SaveLoadedChatWithPrimaryWitness(temp.root, chat, own));
	const auto path = AppPaths::UamChatFilePath(temp.root, chat.id);
	const auto original_bytes = ReadFile(path);
	const auto physical = uam::platform::PublishedFileChangeWitness::Capture(path);
	UAM_ASSERT(physical);
	auto positive = std::async(std::launch::async, [physical, own] { return physical->MatchesContent(own.byte_count, own.sha256); });
	UAM_ASSERT(positive.get()); // Same actual opened regular object, real worker hash.
	const auto retained = temp.root / "retained-primary.json";
	fs::rename(path, retained);
	UAM_ASSERT(mkfifo(path.c_str(), 0600) == 0);
	auto fifo = std::async(std::launch::async, [physical, own] { return physical->MatchesContent(own.byte_count, own.sha256); });
	if (fifo.wait_for(std::chrono::seconds(30)) != std::future_status::ready)
	{
		std::cerr << "FATAL_PRIVATE_BINDING_FIFO: actual worker blocked on substituted leaf" << std::endl;
		std::abort(); // Never tear down its real private root underneath an unfinished read.
	}
	UAM_ASSERT(!fifo.get());
	UAM_ASSERT(!uam::platform::PublishedFileChangeWitness::Capture(path));
	UAM_ASSERT(fs::remove(path));
	fs::create_symlink(retained, path);
	auto linked = std::async(std::launch::async, [physical, own] { return physical->MatchesContent(own.byte_count, own.sha256); });
	UAM_ASSERT(!linked.get());
	UAM_ASSERT(!uam::platform::PublishedFileChangeWitness::Capture(path));
	UAM_ASSERT_EQ(ReadFile(retained), original_bytes);
	UAM_ASSERT(fs::is_symlink(fs::symlink_status(path)));
	UAM_ASSERT_EQ(fs::read_symlink(path), retained);
}
#endif


namespace uam
{
	// Pure production observer analogous to the preparation observer; never harvests/creates a future.
	bool PendingChatSavePublicationForTests(const AppState& app, const std::string& chat_id, bool& ready);
}

namespace
{
	void CheckBindingAdmissionActualPublicationCallback(bool replace_primary)
	{
		TempDir temp("uam-binding-endpoint-callback");
		uam::AppState app;
		app.data_root = temp.root;
		ChatSession chat = BirthChat();
		chat.native_session_id = kBirthB; // Typed repository DATA, no native execution or image authority.
		SavedChatPrimaryWitness launch;
		UAM_ASSERT(ChatRepository::SaveLoadedChatWithPrimaryWitness(temp.root, chat, launch));
		app.chats.push_back(chat);
		app.composer_text = "Preserve this actual live draft.";
		const auto authored = chat.messages;
		const std::string candidate_title = "Actual binding publication candidate";
		app.chats.front().title = candidate_title; // Distinct real persisted marker proves this publication happened.
		ChatSession neighbor = BirthChat(); neighbor.id += "-neighbor"; neighbor.title = "Ordinary neighbor";
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, neighbor));
		auto normal = std::async(std::launch::async, [root = temp.root, neighbor]()
		{
			auto prepared = ChatRepository::PrepareChatSave(root, neighbor);
			return prepared && ChatRepository::PublishPreparedChatSave(prepared);
		});
		UAM_ASSERT(normal.get()); // Existing ordinary Save/Prepare/Publish path, no binding opt-in.
		const auto neighbor_path = AppPaths::UamChatFilePath(temp.root, neighbor.id);
		const auto neighbor_primary = ReadFile(neighbor_path);
		const auto neighbor_summary = ReadFile(AppPaths::UamChatSummaryFilePath(temp.root, neighbor.id));
		const auto neighbor_index = ReadFile(uam::chat_pages::IndexPath(neighbor_path));
		int callbacks = 0;
		std::optional<uam::acp_detail::DurableBindingAdmissionReceipt> receipt;
		const auto owned = [&]()
		{
			return app.data_root == temp.root && app.chats.size() == 1 && app.chats.front().id == chat.id &&
			    app.chats.front().native_session_id == kBirthB && app.chats.front().messages == authored &&
			    app.chats.front().title == candidate_title;
		};
		struct Drain
		{
			uam::AppState& app;
			~Drain()
			{
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
				while ((!app.pending_chat_save_tasks.empty() || !app.pending_chat_save_at_by_chat_id.empty()) &&
				    std::chrono::steady_clock::now() < deadline)
				{
					(void)uam::FlushPendingChatSaves(app);
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				}
				if (!app.pending_chat_save_tasks.empty() || !app.pending_chat_save_at_by_chat_id.empty())
				{
					std::cerr << "FATAL_PRIVATE_BINDING_ENDPOINT_DRAIN: actual save work did not settle" << std::endl;
					std::abort(); // Keep callback/App/root lifetime intact on uncertain actual completion.
				}
			}
		} drain{app};
		UAM_ASSERT(uam::acp_detail::BeginDurableBindingAdmission(app, app.chats.front(), launch, owned,
		    [&](uam::acp_detail::DurableBindingAdmissionReceipt result)
		    { ++callbacks; receipt = std::move(result); }) == uam::acp_detail::DurableAdmissionState::Pending);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		bool publication_exists = false, publication_ready = false;
		while (std::chrono::steady_clock::now() < deadline)
		{
			(void)uam::FlushPendingChatSaves(app);
			if (uam::PendingChatSavePublicationForTests(app, chat.id, publication_ready))
			{
				publication_exists = true;
				break; // STOP owner flushing before the actual future can be consumed.
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		UAM_ASSERT(publication_exists && callbacks == 0);
		while (!publication_ready && std::chrono::steady_clock::now() < deadline)
		{
			UAM_ASSERT(uam::PendingChatSavePublicationForTests(app, chat.id, publication_ready));
			if (!publication_ready) std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		UAM_ASSERT(publication_ready && callbacks == 0); // Actual worker completed; real GUI result remains unconsumed.
		const auto path = AppPaths::UamChatFilePath(temp.root, chat.id);
		const auto published = ChatRepository::LoadLocalChat(temp.root, chat.id, true, nullptr, false);
		UAM_ASSERT(published && published->title == candidate_title && published->messages == authored);
		UAM_ASSERT_EQ(published->native_session_id, std::string(kBirthB)); // Distinct marker rules out failed Publish + old file.
		const auto own_primary = ReadFile(path);
		const auto own_revision = ChatRepository::ChatStorageRevision(temp.root, chat.id);
		UAM_ASSERT(own_revision && *own_revision != "missing");
		uam::platform::IncrementalSha256 own_hash;
		UAM_ASSERT(own_hash.Add(own_primary));
		const auto own_sha256 = own_hash.Finish();
		UAM_ASSERT_EQ(own_sha256.size(), std::size_t{64});
		const auto summary_path = AppPaths::UamChatSummaryFilePath(temp.root, chat.id);
		const auto index_path = uam::chat_pages::IndexPath(path);
		const auto summary = ReadFile(summary_path), index = ReadFile(index_path);
		std::string expected_primary = own_primary;
		if (replace_primary)
		{
			const auto timestamp = fs::last_write_time(path);
			const std::string old = "Authored storage bytes, with trailing spaces.  ";
			const auto at = expected_primary.find(old);
			UAM_ASSERT(at != std::string::npos);
			expected_primary.replace(at, old.size(), "Replaced storage bytes, with trailing spaces.  ");
			UAM_ASSERT(expected_primary != own_primary && expected_primary.size() == own_primary.size());
			UAM_ASSERT(uam::io::WriteTextFile(path, expected_primary)); // Real external-style atomic canonical replacement.
			fs::last_write_time(path, timestamp);
			UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(temp.root, chat.id), own_revision); // Exact weak-stat collision.
		}
		while (std::chrono::steady_clock::now() < deadline &&
		    (callbacks == 0 || !app.pending_chat_save_tasks.empty() || !app.pending_chat_save_at_by_chat_id.empty()))
		{
			(void)uam::FlushPendingChatSaves(app); // ONLY now consume the genuine completed publication.
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		UAM_ASSERT_EQ(callbacks, 1);
		UAM_ASSERT(receipt && app.pending_chat_save_tasks.empty() && app.pending_chat_save_at_by_chat_id.empty());
		if (replace_primary)
		{
			UAM_ASSERT(receipt->state == uam::acp_detail::DurableAdmissionState::Failed);
			UAM_ASSERT(receipt->physical_primary && !receipt->physical_primary->Current());
		}
		else
		{
			UAM_ASSERT(receipt->state == uam::acp_detail::DurableAdmissionState::Committed);
			UAM_ASSERT_EQ(receipt->primary_revision, *own_revision);
			UAM_ASSERT_EQ(receipt->primary_sha256, own_sha256);
			UAM_ASSERT_EQ(receipt->primary_byte_count, static_cast<std::uint64_t>(own_primary.size()));
			UAM_ASSERT(receipt->physical_primary && receipt->physical_primary->Current());
		}
		UAM_ASSERT_EQ(ReadFile(path), expected_primary);
		UAM_ASSERT_EQ(ReadFile(summary_path), summary);
		UAM_ASSERT_EQ(ReadFile(index_path), index);
		UAM_ASSERT_EQ(app.chats.front().messages, authored);
		UAM_ASSERT_EQ(app.chats.front().title, candidate_title);
		UAM_ASSERT_EQ(app.composer_text, std::string("Preserve this actual live draft."));
		UAM_ASSERT_EQ(ReadFile(neighbor_path), neighbor_primary);
		UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatSummaryFilePath(temp.root, neighbor.id)), neighbor_summary);
		UAM_ASSERT_EQ(ReadFile(uam::chat_pages::IndexPath(neighbor_path)), neighbor_index);
		(void)uam::FlushPendingChatSaves(app); // A real extra owner poll must not restage/rollback the refused old binding.
		UAM_ASSERT_EQ(callbacks, 1);
		UAM_ASSERT(app.pending_chat_save_tasks.empty() && app.pending_chat_save_at_by_chat_id.empty());
		UAM_ASSERT_EQ(ReadFile(path), expected_primary);
	}
}

UAM_TEST(ChatBindingAdmissionReturnsActualOwnPublicationReceiptOnce)
{
	CheckBindingAdmissionActualPublicationCallback(false);
}

UAM_TEST(ChatBindingAdmissionRejectsPostWorkerSameSizeTimeReplacementBeforeCallback)
{
	CheckBindingAdmissionActualPublicationCallback(true);
}


UAM_TEST(SavedChatHeldReadReturnsExactCanonicalBytesWithinBound)
{
	TempDir temp("uam-held-canonical-read");
	ChatSession chat = BirthChat();
	chat.messages.front().content += std::string(131073, 'x') + "\nUnicode canonical bytes: \xE2\x98\x83";
	SavedChatPrimaryWitness own;
	UAM_ASSERT(ChatRepository::SaveLoadedChatWithPrimaryWitness(temp.root, chat, own));
	const auto path = AppPaths::UamChatFilePath(temp.root, chat.id);
	const auto actual = ReadFile(path);
	UAM_ASSERT(actual.size() == own.byte_count && actual.size() > 2 * 65536);
	const auto physical = uam::platform::PublishedFileChangeWitness::Capture(path);
	UAM_ASSERT(physical);
	UAM_ASSERT(!physical->ReadBounded(0));
	UAM_ASSERT(!physical->ReadBounded(own.byte_count - 1));
	auto read = std::async(std::launch::async, [physical, own] { return physical->ReadBounded(own.byte_count); });
	const auto bytes = read.get();
	UAM_ASSERT(bytes && *bytes == actual);
	uam::platform::IncrementalSha256 hash;
	UAM_ASSERT(hash.Add(*bytes));
	UAM_ASSERT_EQ(hash.Finish(), own.sha256);
	UAM_ASSERT(physical->Current());
	UAM_ASSERT_EQ(ReadFile(path), actual);
}

UAM_TEST(SavedChatHeldReadRejectsCancellationAndReplacedPrimary)
{
	TempDir temp("uam-held-canonical-stale");
	ChatSession chat = BirthChat();
	SavedChatPrimaryWitness own;
	UAM_ASSERT(ChatRepository::SaveLoadedChatWithPrimaryWitness(temp.root, chat, own));
	const auto path = AppPaths::UamChatFilePath(temp.root, chat.id);
	const auto original = ReadFile(path);
	const auto physical = uam::platform::PublishedFileChangeWitness::Capture(path);
	UAM_ASSERT(physical);
	std::stop_source cancelled;
	cancelled.request_stop();
	UAM_ASSERT(!physical->ReadBounded(own.byte_count, cancelled.get_token()));
	UAM_ASSERT(physical->Current() && ReadFile(path) == original);
	const auto modified = fs::last_write_time(path);
	std::string foreign(original.size(), 'f');
	UAM_ASSERT(uam::io::WriteTextFile(path, foreign));
	fs::last_write_time(path, modified);
	UAM_ASSERT(!physical->ReadBounded(own.byte_count));
	UAM_ASSERT_EQ(ReadFile(path), foreign);
	UAM_ASSERT_EQ(fs::last_write_time(path), modified);
}

#if !defined(_WIN32)
UAM_TEST(SavedChatHeldReadRefusesFifoAndSymlinkSubstitutionWithoutReopen)
{
	TempDir temp("uam-held-canonical-nofollow");
	ChatSession chat = BirthChat();
	SavedChatPrimaryWitness own;
	UAM_ASSERT(ChatRepository::SaveLoadedChatWithPrimaryWitness(temp.root, chat, own));
	const auto path = AppPaths::UamChatFilePath(temp.root, chat.id);
	const auto original = ReadFile(path);
	const auto physical = uam::platform::PublishedFileChangeWitness::Capture(path);
	UAM_ASSERT(physical);
	const auto retained = temp.root / "retained-primary.json";
	fs::rename(path, retained);
	UAM_ASSERT(mkfifo(path.c_str(), 0600) == 0);
	auto fifo = std::async(std::launch::async, [physical, own] { return physical->ReadBounded(own.byte_count); });
	if (fifo.wait_for(std::chrono::seconds(30)) != std::future_status::ready)
	{
		std::cerr << "FATAL_PRIVATE_HELD_READ_FIFO: worker blocked on substituted canonical leaf" << std::endl;
		std::abort(); // Do not remove a private root while an unexpected blocked worker owns it.
	}
	UAM_ASSERT(!fifo.get());
	UAM_ASSERT(!uam::platform::PublishedFileChangeWitness::Capture(path));
	UAM_ASSERT(fs::remove(path));
	fs::create_symlink(retained, path);
	UAM_ASSERT(!physical->ReadBounded(own.byte_count));
	UAM_ASSERT(!uam::platform::PublishedFileChangeWitness::Capture(path));
	UAM_ASSERT(fs::is_symlink(fs::symlink_status(path)) && fs::read_symlink(path) == retained);
	UAM_ASSERT_EQ(ReadFile(retained), original);
}
#endif

UAM_TEST(StoppedCanonicalReadRetainsExactHeldPrimaryAndSummaryWithoutPublication)
{
	TempDir temp("uam-stopped-canonical-data");
	ChatSession chat = BirthChat();
	SavedChatPrimaryWitness own;
	UAM_ASSERT(ChatRepository::SaveLoadedChatWithPrimaryWitness(temp.root, chat, own));
	const auto primary = AppPaths::UamChatFilePath(temp.root, chat.id);
	const auto summary = AppPaths::UamChatSummaryFilePath(temp.root, chat.id);
	const auto bytes = ReadFile(primary), summary_bytes = ReadFile(summary);
	std::string error;
	const auto observed = ChatRepository::ReadStoppedChatCanonical(temp.root, chat.id, 4 * 1024 * 1024, &error);
	UAM_ASSERT(observed && observed->Current());
	UAM_ASSERT_EQ(observed->PrimaryJson(), bytes);
	UAM_ASSERT(observed->SummaryJson() && *observed->SummaryJson() == summary_bytes);
	const auto loaded = ChatRepository::LoadLocalChatBounded(temp.root, chat.id, 4 * 1024 * 1024);
	UAM_ASSERT(loaded && observed->Graph().messages == loaded->messages && observed->Graph().id == chat.id);
	UAM_ASSERT_EQ(ReadFile(primary), bytes); UAM_ASSERT_EQ(ReadFile(summary), summary_bytes);
	std::stop_source cancelled; cancelled.request_stop();
	UAM_ASSERT(!ChatRepository::ReadStoppedChatCanonical(temp.root, chat.id, bytes.size(), &error, cancelled.get_token()));
	UAM_ASSERT(!ChatRepository::ReadStoppedChatCanonical(temp.root, chat.id, bytes.size() - 1, &error));
	UAM_ASSERT(observed->Current());
}

UAM_TEST(StoppedCanonicalReadRefusesPresentJournalAndEffectiveSummaryOverlay)
{
	for (const bool queue : {false, true})
	{
		TempDir temp("uam-stopped-canonical-refusal");
		ChatSession chat = BirthChat();
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
		const auto primary = AppPaths::UamChatFilePath(temp.root, chat.id);
		const auto summary = AppPaths::UamChatSummaryFilePath(temp.root, chat.id);
		const auto bytes = ReadFile(primary);
		fs::path changed = summary;
		if (queue)
		{
			changed = temp.root / "queue-outboxes" / (chat.id + ".json");
			fs::create_directories(changed.parent_path());
			UAM_ASSERT(uam::io::WriteTextFile(changed, "PRESENT-QUEUE-MUST-NOT-BE-PARSED"));
		}
		else
		{
			auto document = ParseJson(ReadFile(summary)); UAM_ASSERT(document);
			uam::json::SetString(*document, "title", "Newer durable summary title");
			UAM_ASSERT(uam::io::WriteTextFile(summary, SerializeJson(*document)));
		}
		const auto changed_bytes = ReadFile(changed);
		std::string error;
		UAM_ASSERT(!ChatRepository::ReadStoppedChatCanonical(temp.root, chat.id, 4 * 1024 * 1024, &error));
		UAM_ASSERT(!error.empty());
		UAM_ASSERT_EQ(ReadFile(primary), bytes); UAM_ASSERT_EQ(ReadFile(changed), changed_bytes);
	}
}

UAM_TEST(StoppedCanonicalReadKeepsUnknownRawDataButNeverPublishesOrPromotesIt)
{
	TempDir temp("uam-stopped-canonical-unknown");
	ChatSession chat = BirthChat(); UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const auto primary = AppPaths::UamChatFilePath(temp.root, chat.id);
	fs::remove(AppPaths::UamChatSummaryFilePath(temp.root, chat.id)); // Explicit absent-summary data-read arm.
	auto bytes = ReadFile(primary); const auto end = bytes.rfind('}'); UAM_ASSERT(end != std::string::npos);
	bytes.insert(end, ",\"futureOpaquePolicy\":{\"value\":9007199254740993,\"nested\":[true,null,\"retain exactly\"]}");
	UAM_ASSERT(uam::io::WriteTextFile(primary, bytes));
	std::string error;
	const auto observed = ChatRepository::ReadStoppedChatCanonical(temp.root, chat.id, 4 * 1024 * 1024, &error);
	UAM_ASSERT(observed && observed->Current());
	UAM_ASSERT_EQ(observed->PrimaryJson(), bytes);
	UAM_ASSERT(!observed->SummaryJson());
	// This observation deliberately offers no publication/permission method or conversion.
	static_assert(!std::is_convertible_v<StoppedChatCanonicalRead, SavedChatPrimaryWitness>);
	static_assert(!std::is_convertible_v<std::shared_ptr<const StoppedChatCanonicalRead>, std::shared_ptr<PreparedChatSave>>);
	const auto replacement = primary.string() + ".private-replacement";
	UAM_ASSERT(uam::io::WriteTextFile(replacement, bytes));
	fs::remove(primary); fs::rename(replacement, primary);
	UAM_ASSERT(!observed->Current()); UAM_ASSERT_EQ(observed->PrimaryJson(), bytes);
	UAM_ASSERT_EQ(ReadFile(primary), bytes);
}

#if !defined(_WIN32)
UAM_TEST(StoppedCanonicalReadRefusesFifoJournalWithoutOpeningIt)
{
	TempDir temp("uam-stopped-canonical-fifo");
	ChatSession chat = BirthChat(); UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const auto journal = temp.root / "queue-outboxes" / (chat.id + ".json");
	fs::create_directories(journal.parent_path()); UAM_ASSERT(::mkfifo(journal.c_str(), 0600) == 0);
	const auto primary = AppPaths::UamChatFilePath(temp.root, chat.id); const auto bytes = ReadFile(primary);
	auto refusal = std::async(std::launch::async, [&]() { return ChatRepository::ReadStoppedChatCanonical(temp.root, chat.id, 4 * 1024 * 1024); });
	UAM_ASSERT(refusal.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
	UAM_ASSERT(!refusal.get()); UAM_ASSERT_EQ(ReadFile(primary), bytes);
}
#endif

namespace
{
	ChatSession StoppedLaunchStorageChat()
	{
		ChatSession chat = BirthChat();
		chat.branch_root_chat_id = chat.id;
		chat.execution_host_id = "private-stopped-host";
		chat.native_session_id = kBirthB; // Typed stored DATA, never a launched/native authority witness.
		return chat;
	}

	void SeedStoppedLaunchStorage(const fs::path& root, ChatSession chat, bool stream)
	{
		if (!stream) UAM_ASSERT(ChatRepository::SaveChat(root, chat));
		else
		{
			auto prepared = ChatRepository::PrepareChatPublication(root, chat, ChatPublicationMode::CreateAbsent, {});
			UAM_ASSERT(prepared);
			UAM_ASSERT_EQ(ChatRepository::PublishPreparedChatPublication(prepared, [] { return true; }, &chat).state, ChatPublicationState::Committed);
		}
	}
}

UAM_TEST(StoppedLaunchStoragePublishesOnlyMatchedRawBaselineAndTypedToken)
{
	for (const bool stream : {false, true})
	{
		TempDir temp("uam-stopped-launch-publish");
		const ChatSession original = StoppedLaunchStorageChat();
		SeedStoppedLaunchStorage(temp.root, original, stream);
		const auto primary = AppPaths::UamChatFilePath(temp.root, original.id);
		const auto summary = AppPaths::UamChatSummaryFilePath(temp.root, original.id);
		const auto before = ReadFile(primary), before_summary = ReadFile(summary);
		std::string error;
		const auto observed = ChatRepository::ReadStoppedChatCanonical(temp.root, original.id, 4 * 1024 * 1024, &error);
		UAM_ASSERT(observed && observed->Current()); // queue-outboxes directory is genuinely absent.
		const ChatSession selected = observed->Graph();
		const StoppedChatLaunchDelta delta{"actual-private-initial-token"};
		auto prepared = ChatRepository::PrepareStoppedChatLaunchStorage(observed, selected, delta, &error);
		UAM_ASSERT(prepared && error.empty());
		UAM_ASSERT_EQ(ReadFile(primary), before); UAM_ASSERT_EQ(ReadFile(summary), before_summary);
		const auto published = ChatRepository::PublishStoppedChatLaunchStorage(prepared, [] { return true; });
		UAM_ASSERT_EQ(published.publication.state, ChatPublicationState::Committed);
		UAM_ASSERT(!published.publication.backup_degraded && !published.publication.secondary_degraded);
		UAM_ASSERT(published.receipt && published.receipt->Current());
		UAM_ASSERT(!published.receipt->InitialTransportLaunch()); // A token-only storage delta is not a transport launch origin.
		const auto bytes = ReadFile(primary);
		uam::platform::IncrementalSha256 hash; UAM_ASSERT(hash.Add(bytes));
		UAM_ASSERT_EQ(published.receipt->Primary().byte_count, static_cast<std::uint64_t>(bytes.size()));
		UAM_ASSERT_EQ(published.receipt->Primary().sha256, hash.Finish());
		UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(temp.root, original.id), std::optional<std::string>(published.receipt->Primary().revision));
		const auto durable = ChatRepository::LoadLocalChatBounded(temp.root, original.id, 4 * 1024 * 1024);
		UAM_ASSERT(durable && durable->remote_process_control_token == delta.process_control_token);
		UAM_ASSERT_EQ(durable->messages, selected.messages);
		UAM_ASSERT_EQ(durable->native_session_id, selected.native_session_id);
		UAM_ASSERT_EQ(durable->model_id, selected.model_id);
		UAM_ASSERT_EQ(durable->approval_mode, selected.approval_mode);
		UAM_ASSERT(!durable->remote_process_exists && durable->acp_queued_prompts.empty());
		UAM_ASSERT_EQ(published.receipt->Graph().remote_process_control_token, delta.process_control_token);
		const auto replay = ChatRepository::PublishStoppedChatLaunchStorage(prepared, [] { return true; });
		UAM_ASSERT_EQ(replay.publication.state, ChatPublicationState::Rejected); UAM_ASSERT(!replay.receipt);
		prepared.reset(); // Creator-origin teardown must not change a healthy own receipt's physical stamp.
		UAM_ASSERT(published.receipt->Current());
		const auto next = ChatRepository::ReadStoppedChatCanonical(temp.root, original.id, 4 * 1024 * 1024, &error);
		UAM_ASSERT(next);
		UAM_ASSERT(!ChatRepository::PrepareStoppedChatLaunchStorage(next, next->Graph(), delta, &error)); // Explicit no-op, never a new own receipt.
		UAM_ASSERT_EQ(ReadFile(primary), bytes);
		static_assert(!std::is_convertible_v<StoppedChatCanonicalRead, StoppedChatLaunchStorageReceipt>);
		static_assert(!std::is_default_constructible_v<StoppedChatLaunchStorageReceipt>);
	}
}

UAM_TEST(StoppedLaunchStorageRefusesUnknownRawAndCompleteSelectedGraphDrift)
{
	for (const std::string variant : {"unknown-primary", "unknown-summary", "duplicate-primary", "numeric-primary", "model", "row", "policy"})
	{
		TempDir temp("uam-stopped-launch-full-baseline");
		ChatSession original = StoppedLaunchStorageChat(); SeedStoppedLaunchStorage(temp.root, original, false);
		const auto primary = AppPaths::UamChatFilePath(temp.root, original.id), summary = AppPaths::UamChatSummaryFilePath(temp.root, original.id);
		if (variant.starts_with("unknown"))
		{
			const auto target = variant == "unknown-primary" ? primary : summary;
			auto raw = ReadFile(target); const auto end = raw.rfind('}'); UAM_ASSERT(end != std::string::npos);
			raw.insert(end, ",\"futureOpaquePolicy\":{\"number\":9007199254740993,\"retain\":true}");
			UAM_ASSERT(uam::io::WriteTextFile(target, raw));
			if (variant == "unknown-primary") fs::remove(summary); // Explicit absent-summary read, not an invalid source-size shortcut.
		}
		if (variant == "duplicate-primary" || variant == "numeric-primary")
		{
			auto raw = ReadFile(primary);
			if (variant == "duplicate-primary")
			{
				const auto end = raw.rfind('}'); UAM_ASSERT(end != std::string::npos);
				raw.insert(end, ",\"model_id\":\"\""); // Decode may accept; raw full-baseline equality must refuse.
			}
			else
			{
				const auto key = raw.find("\"remote_turn_serial\""); UAM_ASSERT(key != std::string::npos);
				const auto colon = raw.find(':', key), zero = raw.find_first_not_of(" \r\n\t", colon + 1);
				UAM_ASSERT(colon != std::string::npos && zero != std::string::npos && raw[zero] == '0');
				raw.replace(zero, 1, "0e0"); // Same numeric value, unsupported original spelling remains untouched.
			}
			UAM_ASSERT(uam::io::WriteTextFile(primary, raw)); fs::remove(summary);
		}
		std::string error;
		const auto observed = ChatRepository::ReadStoppedChatCanonical(temp.root, original.id, 4 * 1024 * 1024, &error);
		UAM_ASSERT(observed);
		ChatSession selected = observed->Graph();
		if (variant == "model") selected.model_id = "foreign-live-model";
		if (variant == "row") selected.messages.front().content += " foreign bytes";
		if (variant == "policy") selected.command_safety_tier = "yolo";
		const auto before = ReadFile(primary);
		const auto before_summary = fs::exists(summary) ? std::optional<std::string>(ReadFile(summary)) : std::nullopt;
		UAM_ASSERT(!ChatRepository::PrepareStoppedChatLaunchStorage(observed, selected, {"new-private-token"}, &error));
		UAM_ASSERT(!error.empty()); UAM_ASSERT_EQ(ReadFile(primary), before);
		UAM_ASSERT_EQ(fs::exists(summary), static_cast<bool>(before_summary));
		if (before_summary) UAM_ASSERT_EQ(ReadFile(summary), *before_summary);
	}
}

UAM_TEST(StoppedLaunchStorageRefusesHeldPredecessorCompanionBirthAndOwnerChanges)
{
	for (const std::string variant : {"primary", "summary", "index", "queue", "birth", "owner", "stage"})
	{
		TempDir temp("uam-stopped-launch-final-guards");
		const ChatSession original = StoppedLaunchStorageChat(); SeedStoppedLaunchStorage(temp.root, original, false);
		const auto primary = AppPaths::UamChatFilePath(temp.root, original.id), summary = AppPaths::UamChatSummaryFilePath(temp.root, original.id);
		const auto observed = ChatRepository::ReadStoppedChatCanonical(temp.root, original.id, 4 * 1024 * 1024);
		UAM_ASSERT(observed);
		auto prepared = ChatRepository::PrepareStoppedChatLaunchStorage(observed, observed->Graph(), {"new-private-token"});
		UAM_ASSERT(prepared);
		fs::path changed;
		if (variant == "primary" || variant == "summary") changed = variant == "primary" ? primary : summary;
		if (variant == "index") changed = uam::chat_pages::IndexPath(primary);
		if (variant == "queue") { changed = temp.root / "queue-outboxes" / (original.id + ".json"); fs::create_directories(changed.parent_path()); }
		if (variant == "stage")
			for (const auto& entry : fs::directory_iterator(primary.parent_path()))
				if (entry.path().filename().string().ends_with("-primary")) { UAM_ASSERT(changed.empty()); changed = entry.path(); }
		if (!changed.empty())
		{
			const auto existing = fs::exists(changed) ? std::optional<std::string>(ReadFile(changed)) : std::nullopt;
			const auto modified = existing ? std::optional<fs::file_time_type>(fs::last_write_time(changed)) : std::nullopt;
			const auto foreign = existing ? (variant == "stage" ? *existing : std::string(existing->size(), 'f')) : "foreign companion";
			UAM_ASSERT(uam::io::WriteTextFile(changed, foreign));
			if (modified) fs::last_write_time(changed, *modified);
		}
		if (variant == "birth") { ChatSession bound = observed->Graph(); bound.managed_followup_incarnation = kBirthC; UAM_ASSERT(ChatRepository::SaveChat(temp.root, bound)); }
		const auto before = ReadFile(primary), before_summary = ReadFile(summary);
		const auto changed_bytes = changed.empty() ? std::optional<std::string>{} : std::optional<std::string>(ReadFile(changed));
		const auto result = ChatRepository::PublishStoppedChatLaunchStorage(prepared, [variant] { return variant != "owner"; });
		UAM_ASSERT_EQ(result.publication.state, ChatPublicationState::Rejected); UAM_ASSERT(!result.receipt);
		UAM_ASSERT_EQ(ReadFile(primary), before); UAM_ASSERT_EQ(ReadFile(summary), before_summary);
		if (changed_bytes) UAM_ASSERT_EQ(ReadFile(changed), *changed_bytes);
	}
}

UAM_TEST(StoppedLaunchStoragePreservesActualUncertainAndDegradedFactsWithoutReceipt)
{
	for (const bool secondary : {false, true})
	{
		TempDir temp("uam-stopped-launch-outcome");
		const ChatSession original = StoppedLaunchStorageChat(); SeedStoppedLaunchStorage(temp.root, original, false);
		const auto observed = ChatRepository::ReadStoppedChatCanonical(temp.root, original.id, 4 * 1024 * 1024); UAM_ASSERT(observed);
		auto prepared = ChatRepository::PrepareStoppedChatLaunchStorage(observed, observed->Graph(), {"new-private-token"}); UAM_ASSERT(prepared);
		bool reached = false;
		const auto result = ChatRepository::PublishStoppedChatLaunchStorage(prepared, [] { return true; },
		    [&](ChatPublicationFile role, uam::io::AtomicWriteStage point)
		    {
			    const bool fault = secondary ? role == ChatPublicationFile::Summary && point == uam::io::AtomicWriteStage::BeforeReplace :
			        role == ChatPublicationFile::Primary && point == uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync;
			    if (fault) { reached = true; return false; }
			    return true;
		    });
		UAM_ASSERT(reached && !result.receipt);
		UAM_ASSERT_EQ(result.publication.state, secondary ? ChatPublicationState::Committed : ChatPublicationState::Uncertain);
		if (secondary) UAM_ASSERT(result.publication.secondary_degraded);
		const auto again = ChatRepository::PublishStoppedChatLaunchStorage(prepared, [] { return true; });
		UAM_ASSERT_EQ(again.publication.state, ChatPublicationState::Rejected); UAM_ASSERT(!again.receipt);
	}
}

#if !defined(_WIN32)
UAM_TEST(StoppedLaunchStorageRefusesLateFifoJournalWithoutOpeningIt)
{
	TempDir temp("uam-stopped-launch-late-fifo");
	const ChatSession original = StoppedLaunchStorageChat(); SeedStoppedLaunchStorage(temp.root, original, false);
	const auto observed = ChatRepository::ReadStoppedChatCanonical(temp.root, original.id, 4 * 1024 * 1024); UAM_ASSERT(observed);
	auto prepared = ChatRepository::PrepareStoppedChatLaunchStorage(observed, observed->Graph(), {"new-private-token"}); UAM_ASSERT(prepared);
	const auto path = AppPaths::UamChatFilePath(temp.root, original.id), journal = temp.root / "queue-outboxes" / (original.id + ".json");
	const auto bytes = ReadFile(path); fs::create_directories(journal.parent_path()); UAM_ASSERT(::mkfifo(journal.c_str(), 0600) == 0);
	auto future = std::async(std::launch::async, [&] { return ChatRepository::PublishStoppedChatLaunchStorage(prepared, [] { return true; }); });
	if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready) { std::cerr << "FATAL_PRIVATE_STOPPED_FIFO" << std::endl; std::abort(); }
	const auto result = future.get(); UAM_ASSERT_EQ(result.publication.state, ChatPublicationState::Rejected); UAM_ASSERT(!result.receipt);
	UAM_ASSERT_EQ(ReadFile(path), bytes); UAM_ASSERT(fs::is_fifo(fs::symlink_status(journal)));
}
#endif

UAM_TEST(StoppedLaunchRetainedObjectIdentitySurvivesRenameButRejectsIdenticalReplacement)
{
	TempDir temp("uam-stopped-launch-physical-identity");
	const auto original = temp.root / "emitted-stage", canonical = temp.root / "canonical";
	const std::string bytes = "actual retained regular object";
	UAM_ASSERT(uam::io::WriteTextFile(original, bytes));
	const auto held = uam::platform::PublishedFileChangeWitness::Capture(original); UAM_ASSERT(held && held->Current());
	fs::rename(original, canonical);
	const auto published = uam::platform::PublishedFileChangeWitness::Capture(canonical); UAM_ASSERT(published && published->Current());
	UAM_ASSERT(!held->Current()); // Identity alone is deliberately not old-path Current or a content grant.
	UAM_ASSERT(held->SameRetainedObject(*published));
	uam::platform::IncrementalSha256 hash; UAM_ASSERT(hash.Add(bytes)); const auto digest = hash.Finish();
	UAM_ASSERT(published->MatchesContent(bytes.size(), digest));
	const auto modified = fs::last_write_time(canonical);
	UAM_ASSERT(uam::io::WriteTextFile(canonical, bytes)); fs::last_write_time(canonical, modified);
	const auto foreign = uam::platform::PublishedFileChangeWitness::Capture(canonical); UAM_ASSERT(foreign && foreign->Current());
	UAM_ASSERT(foreign->MatchesContent(bytes.size(), digest));
	UAM_ASSERT(!held->SameRetainedObject(*foreign)); UAM_ASSERT(!published->Current());
}

UAM_TEST(AtomicTempActualWriterOriginRejectsSameByteForeignAfterCloseBeforeObservation)
{
	for (const bool substitute : {false, true})
	{
		TempDir temp("uam-actual-writer-origin");
		const auto path = temp.root / "private-created-stage";
		const std::string bytes = "actual successfully emitted bytes";
		std::shared_ptr<const uam::platform::AtomicTempWriterOrigin> origin;
		uam::io::AtomicWriteResult write;
		bool after_close = false;
		UAM_ASSERT(uam::io::WriteAndSyncAtomicTempWithOrigin(path,
		    [&](const std::function<bool(std::string_view)>& append) { return append(bytes); },
		    [&](uam::io::AtomicWriteStage point)
		    {
			    if (point == uam::io::AtomicWriteStage::AfterFileSync)
			    {
				    after_close = true; // Real helper checkpoint follows sync, creator identity retention and writer close.
				    if (substitute)
				    {
					    const auto modified = fs::last_write_time(path);
					    UAM_ASSERT(uam::io::WriteTextFile(path, bytes));
					    fs::last_write_time(path, modified); // Same bytes/size/time must not adopt the foreign object.
				    }
			    }
			    return true;
		    }, write, origin));
		UAM_ASSERT(after_close && origin);
		const auto physical = uam::platform::PublishedFileChangeWitness::Capture(path); UAM_ASSERT(physical && physical->Current());
		uam::platform::IncrementalSha256 hash; UAM_ASSERT(hash.Add(bytes));
		UAM_ASSERT(physical->MatchesContent(bytes.size(), hash.Finish()));
		UAM_ASSERT_EQ(origin->Matches(*physical), !substitute);
		if (!substitute)
		{
			const auto canonical = temp.root / "canonical"; fs::rename(path, canonical);
			const auto renamed = uam::platform::PublishedFileChangeWitness::Capture(canonical); UAM_ASSERT(renamed && renamed->Current());
			UAM_ASSERT(origin->Matches(*renamed)); // Retained creator-derived identity remains alive across publication rename.
			UAM_ASSERT(!physical->Current());
			origin.reset();
			UAM_ASSERT(renamed->Current()); // Closing attribute-only origin cannot finalize pending writer timestamps.
		}
		const auto failed_path = temp.root / "interrupted-stage";
		uam::io::AtomicWriteResult failed;
		UAM_ASSERT(!uam::io::WriteAndSyncAtomicTempWithOrigin(failed_path,
		    [&](const std::function<bool(std::string_view)>& append) { return append(bytes); },
		    [](uam::io::AtomicWriteStage point) { return point != uam::io::AtomicWriteStage::AfterFileSync; }, failed, origin));
		UAM_ASSERT(!origin); // No old receipt on interrupted/failed/no-completion output.
		static_assert(!std::is_default_constructible_v<uam::platform::AtomicTempWriterOrigin>);
		static_assert(!std::is_copy_constructible_v<uam::platform::AtomicTempWriterOrigin>);
	}
}

UAM_TEST(StoppedLaunchStorageRefusesSameByteForeignAtActualWriterClose)
{
	TempDir temp("uam-stopped-launch-writer-origin");
	const ChatSession original = StoppedLaunchStorageChat(); SeedStoppedLaunchStorage(temp.root, original, false);
	const auto primary = AppPaths::UamChatFilePath(temp.root, original.id), summary = AppPaths::UamChatSummaryFilePath(temp.root, original.id);
	const auto before = ReadFile(primary), before_summary = ReadFile(summary);
	const auto observed = ChatRepository::ReadStoppedChatCanonical(temp.root, original.id, 4 * 1024 * 1024); UAM_ASSERT(observed);
	fs::path foreign_stage;
	std::string foreign_bytes, error;
	const auto prepared = ChatRepository::PrepareStoppedChatLaunchStorage(observed, observed->Graph(), {"new-private-token"}, &error,
	    [&](ChatPublicationFile role, uam::io::AtomicWriteStage point)
	    {
		    if (foreign_stage.empty() && role == ChatPublicationFile::Primary && point == uam::io::AtomicWriteStage::AfterFileSync)
		    {
			    for (const auto& entry : fs::directory_iterator(primary.parent_path()))
				    if (entry.path().filename().string().ends_with("-primary")) { UAM_ASSERT(foreign_stage.empty()); foreign_stage = entry.path(); }
			    UAM_ASSERT(!foreign_stage.empty());
			    foreign_bytes = ReadFile(foreign_stage);
			    const auto modified = fs::last_write_time(foreign_stage);
			    UAM_ASSERT(uam::io::WriteTextFile(foreign_stage, foreign_bytes));
			    fs::last_write_time(foreign_stage, modified);
		    }
		    return true;
	    });
	UAM_ASSERT(!foreign_stage.empty()); // Actual synced writer checkpoint reached; not a fabricated origin or stage.
	UAM_ASSERT(!prepared && !error.empty()); // Unsafe post-close Capture accepts this exact substitution.
	UAM_ASSERT_EQ(ReadFile(primary), before); UAM_ASSERT_EQ(ReadFile(summary), before_summary);
	UAM_ASSERT(fs::exists(foreign_stage)); UAM_ASSERT_EQ(ReadFile(foreign_stage), foreign_bytes); // Unknown object retained, never adopted/removed.
}


namespace
{
	StoppedChatLaunchDelta StoppedTransportDelta(std::string protocol = "private-structured-json")
	{
		StoppedChatLaunchDelta delta{"private-new-transport-token"};
		delta.transport_launch = StoppedChatTransportLaunchMarker{std::move(protocol)};
		return delta;
	}

	void AssertStoppedTransportPrimary(const std::string& before, const std::string& after,
	                                  const StoppedChatLaunchDelta& delta)
	{
		auto expected = nlohmann::json::parse(before);
		expected["remote_process_control_token"] = delta.process_control_token;
		if (delta.transport_launch)
		{
			expected["remote_process_exists"] = true;
			expected["remote_structured_protocol"] = delta.transport_launch->structured_protocol;
			expected["remote_prompt_image_supported"] = false;
		}
		UAM_ASSERT_EQ(nlohmann::json::parse(after), expected); // Every other persisted key and nested record unchanged.
	}
}

UAM_TEST(StoppedLaunchTransportMarkerPublishesExactFieldsInlineAndStream)
{
	for (const bool stream : {false, true})
	{
		TempDir temp("uam-stopped-transport-publish");
		const ChatSession original = StoppedLaunchStorageChat(); SeedStoppedLaunchStorage(temp.root, original, stream);
		const auto primary = AppPaths::UamChatFilePath(temp.root, original.id), summary = AppPaths::UamChatSummaryFilePath(temp.root, original.id);
		const auto before = ReadFile(primary), before_summary = ReadFile(summary);
		std::string error;
		const auto observed = ChatRepository::ReadStoppedChatCanonical(temp.root, original.id, 4 * 1024 * 1024, &error);
		UAM_ASSERT(observed && observed->Current() && error.empty());
		const ChatSession selected = observed->Graph();
		const auto delta = StoppedTransportDelta(stream ? "private-stream-json" : std::string(64, 'p'));
		auto prepared = ChatRepository::PrepareStoppedChatLaunchStorage(observed, selected, delta, &error);
		UAM_ASSERT(prepared && error.empty());
		UAM_ASSERT_EQ(ReadFile(primary), before); UAM_ASSERT_EQ(ReadFile(summary), before_summary);
		const auto result = ChatRepository::PublishStoppedChatLaunchStorage(prepared, [] { return true; });
		UAM_ASSERT_EQ(result.publication.state, ChatPublicationState::Committed);
		UAM_ASSERT(!result.publication.backup_degraded && !result.publication.secondary_degraded && result.receipt && result.receipt->Current());
		AssertStoppedTransportPrimary(before, ReadFile(primary), delta);
		const auto stored = ChatRepository::LoadLocalChatBounded(temp.root, original.id, 4 * 1024 * 1024);
		UAM_ASSERT(stored && stored->remote_process_exists && !stored->remote_prompt_image_supported);
		UAM_ASSERT_EQ(stored->remote_structured_protocol, delta.transport_launch->structured_protocol);
		UAM_ASSERT_EQ(stored->messages, selected.messages); UAM_ASSERT_EQ(stored->native_session_id, selected.native_session_id);
		UAM_ASSERT_EQ(stored->model_id, selected.model_id); UAM_ASSERT_EQ(stored->approval_mode, selected.approval_mode);
		const auto metadata = nlohmann::json::parse(ReadFile(summary));
		UAM_ASSERT(metadata.at("remote_process_exists").get<bool>() && !metadata.at("remote_prompt_image_supported").get<bool>());
		UAM_ASSERT_EQ(metadata.at("remote_structured_protocol"), nlohmann::json(delta.transport_launch->structured_protocol));
		UAM_ASSERT_EQ(result.receipt->Graph().remote_structured_protocol, stored->remote_structured_protocol);
		UAM_ASSERT(result.receipt->Graph().remote_process_exists && !result.receipt->Graph().remote_prompt_image_supported);
		UAM_ASSERT(!ChatRepository::ReadStoppedChatCanonical(temp.root, original.id, 4 * 1024 * 1024)); // Marker is conservative DATA, not stopped/readiness proof.
		prepared.reset(); UAM_ASSERT(result.receipt->Current());
	}
}

UAM_TEST(StoppedLaunchTransportMarkerRefusesMalformedAndForeignGraphs)
{
	for (const std::string variant : {"empty", "overlong", "nul", "space", "local", "selected-policy", "not-stopped"})
	{
		TempDir temp("uam-stopped-transport-refusal");
		ChatSession original = StoppedLaunchStorageChat();
		if (variant == "local") original.execution_host_id = "local";
		if (variant == "not-stopped") original.remote_process_exists = true;
		SeedStoppedLaunchStorage(temp.root, original, false);
		const auto primary = AppPaths::UamChatFilePath(temp.root, original.id), summary = AppPaths::UamChatSummaryFilePath(temp.root, original.id);
		const auto before = ReadFile(primary), before_summary = ReadFile(summary);
		std::string error;
		const auto observed = ChatRepository::ReadStoppedChatCanonical(temp.root, original.id, 4 * 1024 * 1024, &error);
		if (variant == "not-stopped") UAM_ASSERT(!observed && !error.empty());
		else
		{
			UAM_ASSERT(observed && observed->Current()); ChatSession selected = observed->Graph();
			auto delta = StoppedTransportDelta();
			if (variant == "empty") delta.transport_launch->structured_protocol.clear();
			if (variant == "overlong") delta.transport_launch->structured_protocol = std::string(65, 'p');
			if (variant == "nul") delta.transport_launch->structured_protocol = std::string("native\0json", 11);
			if (variant == "space") delta.transport_launch->structured_protocol = "native json";
			if (variant == "selected-policy") selected.command_safety_tier = "yolo";
			UAM_ASSERT(!ChatRepository::PrepareStoppedChatLaunchStorage(observed, selected, delta, &error));
			UAM_ASSERT(!error.empty());
		}
		UAM_ASSERT_EQ(ReadFile(primary), before); UAM_ASSERT_EQ(ReadFile(summary), before_summary);
	}
}

UAM_TEST(StoppedLaunchTransportMarkerPreservesForeignAndUncertainFacts)
{
	for (const std::string variant : {"foreign-primary", "owner", "uncertain", "secondary"})
	{
		TempDir temp("uam-stopped-transport-publication");
		const ChatSession original = StoppedLaunchStorageChat(); SeedStoppedLaunchStorage(temp.root, original, false);
		const auto primary = AppPaths::UamChatFilePath(temp.root, original.id), summary = AppPaths::UamChatSummaryFilePath(temp.root, original.id);
		const auto before = ReadFile(primary), before_summary = ReadFile(summary);
		const auto observed = ChatRepository::ReadStoppedChatCanonical(temp.root, original.id, 4 * 1024 * 1024); UAM_ASSERT(observed);
		const auto delta = StoppedTransportDelta();
		auto prepared = ChatRepository::PrepareStoppedChatLaunchStorage(observed, observed->Graph(), delta); UAM_ASSERT(prepared);
		if (variant == "foreign-primary")
		{
			const auto time = fs::last_write_time(primary); UAM_ASSERT(uam::io::WriteTextFile(primary, before)); fs::last_write_time(primary, time);
		}
		bool fault_reached = false;
		const auto result = ChatRepository::PublishStoppedChatLaunchStorage(prepared, [&] { return variant != "owner"; },
		    [&](ChatPublicationFile role, uam::io::AtomicWriteStage point)
		    {
			    const bool fault = variant == "uncertain" ? role == ChatPublicationFile::Primary && point == uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync :
			        variant == "secondary" && role == ChatPublicationFile::Summary && point == uam::io::AtomicWriteStage::BeforeReplace;
			    if (fault) { fault_reached = true; return false; } return true;
		    });
		UAM_ASSERT(!result.receipt);
		if (variant == "foreign-primary" || variant == "owner")
		{
			UAM_ASSERT_EQ(result.publication.state, ChatPublicationState::Rejected);
			UAM_ASSERT_EQ(ReadFile(primary), before); UAM_ASSERT_EQ(ReadFile(summary), before_summary);
		}
		else
		{
			UAM_ASSERT(fault_reached);
			UAM_ASSERT_EQ(result.publication.state, variant == "secondary" ? ChatPublicationState::Committed : ChatPublicationState::Uncertain);
			if (variant == "secondary") UAM_ASSERT(result.publication.secondary_degraded);
			AssertStoppedTransportPrimary(before, ReadFile(primary), delta); // Known marker write retained, not rolled back or promoted to receipt.
		}
		UAM_ASSERT_EQ(ChatRepository::PublishStoppedChatLaunchStorage(prepared, [] { return true; }).publication.state, ChatPublicationState::Rejected);
	}
}

UAM_TEST(StoppedLaunchTokenOnlyDeltaRemainsTransportNeutral)
{
	TempDir temp("uam-stopped-token-compatibility");
	const ChatSession original = StoppedLaunchStorageChat(); SeedStoppedLaunchStorage(temp.root, original, false);
	const auto primary = AppPaths::UamChatFilePath(temp.root, original.id);
	const auto before = ReadFile(primary);
	const auto observed = ChatRepository::ReadStoppedChatCanonical(temp.root, original.id, 4 * 1024 * 1024); UAM_ASSERT(observed);
	const StoppedChatLaunchDelta delta{"private-token-only-unchanged"}; UAM_ASSERT(!delta.transport_launch);
	auto prepared = ChatRepository::PrepareStoppedChatLaunchStorage(observed, observed->Graph(), delta); UAM_ASSERT(prepared);
	const auto result = ChatRepository::PublishStoppedChatLaunchStorage(prepared, [] { return true; });
	UAM_ASSERT_EQ(result.publication.state, ChatPublicationState::Committed); UAM_ASSERT(result.receipt && result.receipt->Current());
	AssertStoppedTransportPrimary(before, ReadFile(primary), delta);
	const auto stored = ChatRepository::LoadLocalChatBounded(temp.root, original.id, 4 * 1024 * 1024);
	UAM_ASSERT(stored && !stored->remote_process_exists && !stored->remote_prompt_image_supported && stored->remote_structured_protocol.empty());
}

namespace
{
	std::shared_ptr<const StoppedChatLaunchStorageReceipt> OwnContinuationLaunch(const fs::path& root, bool stream)
	{
		const ChatSession original = StoppedLaunchStorageChat(); SeedStoppedLaunchStorage(root, original, stream);
		const auto stopped = ChatRepository::ReadStoppedChatCanonical(root, original.id, 4 * 1024 * 1024); UAM_ASSERT(stopped);
		auto prepared = ChatRepository::PrepareStoppedChatLaunchStorage(stopped, stopped->Graph(), StoppedTransportDelta()); UAM_ASSERT(prepared);
		const auto launched = ChatRepository::PublishStoppedChatLaunchStorage(prepared, [] { return true; });
		UAM_ASSERT_EQ(launched.publication.state, ChatPublicationState::Committed);
		UAM_ASSERT(launched.receipt && launched.receipt->Current());
		return launched.receipt; // Private storage DATA, no provider/process/input fixture or claimed native authority.
	}

	void ReplaceOwnContinuationFile(const fs::path& path)
	{
		const auto bytes = ReadFile(path); const auto time = fs::last_write_time(path);
		UAM_ASSERT(uam::io::WriteTextFile(path, bytes)); fs::last_write_time(path, time);
		UAM_ASSERT_EQ(ReadFile(path), bytes); UAM_ASSERT_EQ(fs::last_write_time(path), time);
	}

	uam::AcpQueuedUserPromptState OwnContinuationHead(const ChatSession& chat)
	{
		uam::AcpQueuedUserPromptState head;
		head.id = "private-owned-existing-row-head";
		head.text = chat.messages.front().content;
		head.append_user_message = false;
		head.direct_admission = true;
		head.prepared_for_delivery = true;
		head.existing_user_message_index = 0;
		head.existing_user_message_created_at = chat.messages.front().created_at;
		return head;
	}
}

UAM_TEST(OwnChatPublicationRotatesSameGraphHeadAndPacketReceipts)
{
	for (const bool stream : {false, true})
	{
		TempDir temp("uam-own-continuation-chain");
		auto own = OwnContinuationLaunch(temp.root, stream);
		const auto primary = AppPaths::UamChatFilePath(temp.root, own->Graph().id);
		const auto summary = AppPaths::UamChatSummaryFilePath(temp.root, own->Graph().id);
		const auto original = ReadFile(primary), original_summary = ReadFile(summary);
		const auto initial = own;
		std::string error;
		auto binding = ChatRepository::PrepareOwnChatPublication(own, own->Graph(), &error);
		UAM_ASSERT(binding && error.empty() && own->Current());
		UAM_ASSERT_EQ(ReadFile(primary), original);
		const auto bound = ChatRepository::PublishStoppedChatLaunchStorage(binding, [] { return true; });
		UAM_ASSERT_EQ(bound.publication.state, ChatPublicationState::Committed);
		UAM_ASSERT(bound.receipt && bound.receipt->Current() && !initial->Current());
		UAM_ASSERT_EQ(ReadFile(primary), original); UAM_ASSERT_EQ(ReadFile(summary), original_summary);
		UAM_ASSERT_EQ(bound.receipt->Primary().sha256, initial->Primary().sha256); // Same bytes, genuinely new own physical publication.
		UAM_ASSERT(initial->InitialTransportLaunch() && !bound.receipt->InitialTransportLaunch());
		UAM_ASSERT(bound.receipt->SameLaunchPublicationChain(*initial));
		binding.reset(); UAM_ASSERT(bound.receipt->Current()); own = bound.receipt;
		ChatSession candidate = own->Graph();
		candidate.acp_queued_prompts = {OwnContinuationHead(candidate)};
		candidate.acp_dispatched_queued_prompt_count = 1;
		candidate.remote_active_turn_id = "private-storage-turn";
		candidate.remote_turn_serial = 1;
		candidate.remote_turn_user_message_index = 0;
		auto head = ChatRepository::PrepareOwnChatPublication(own, candidate, &error); UAM_ASSERT(head && error.empty());
		const auto headed = ChatRepository::PublishStoppedChatLaunchStorage(head, [] { return true; });
		UAM_ASSERT_EQ(headed.publication.state, ChatPublicationState::Committed);
		UAM_ASSERT(headed.receipt && headed.receipt->Current() && !own->Current()); own = headed.receipt;
		const auto saved_head = ChatRepository::LoadLocalChatBounded(temp.root, candidate.id, 4 * 1024 * 1024);
		UAM_ASSERT(saved_head && saved_head->messages == candidate.messages && saved_head->acp_queued_prompts == candidate.acp_queued_prompts);
		UAM_ASSERT_EQ(saved_head->acp_dispatched_queued_prompt_count, std::size_t(1));
		UAM_ASSERT(!fs::exists(temp.root / "queue-outboxes" / (candidate.id + ".json")));
		candidate = own->Graph();
		candidate.remote_prompt_delivery_session_id = "private-storage-delivery-session";
		candidate.remote_prompt_delivery_id = "private-storage-delivery-id";
		candidate.remote_prompt_delivery_payload = "{\"privateStoragePacket\":\"exact bytes, not native authority\"}";
		auto packet = ChatRepository::PrepareOwnChatPublication(own, candidate, &error); UAM_ASSERT(packet && error.empty());
		const auto committed = ChatRepository::PublishStoppedChatLaunchStorage(packet, [] { return true; });
		UAM_ASSERT_EQ(committed.publication.state, ChatPublicationState::Committed);
		UAM_ASSERT(committed.receipt && committed.receipt->Current() && !own->Current());
		UAM_ASSERT(committed.receipt->SameLaunchPublicationChain(*initial));
		const auto stored = ChatRepository::LoadLocalChatBounded(temp.root, candidate.id, 4 * 1024 * 1024);
		UAM_ASSERT(stored && stored->messages == candidate.messages && stored->acp_queued_prompts == candidate.acp_queued_prompts);
		UAM_ASSERT_EQ(stored->remote_prompt_delivery_payload, candidate.remote_prompt_delivery_payload);
		UAM_ASSERT_EQ(stored->remote_prompt_delivery_id, candidate.remote_prompt_delivery_id);
		UAM_ASSERT_EQ(stored->native_session_id, initial->Graph().native_session_id);
		UAM_ASSERT_EQ(stored->model_id, initial->Graph().model_id); UAM_ASSERT_EQ(stored->approval_mode, initial->Graph().approval_mode);
		UAM_ASSERT_EQ(ChatRepository::PublishStoppedChatLaunchStorage(packet, [] { return true; }).publication.state, ChatPublicationState::Rejected);
		packet.reset(); head.reset(); UAM_ASSERT(committed.receipt->Current());
		const auto after = ReadFile(primary);
		ReplaceOwnContinuationFile(summary); // A foreign same-byte companion AFTER commit is not a next own predecessor.
		UAM_ASSERT(!committed.receipt->Current());
		UAM_ASSERT(!ChatRepository::PrepareOwnChatPublication(committed.receipt, committed.receipt->Graph(), &error));
		UAM_ASSERT_EQ(ReadFile(primary), after);
	}
}

UAM_TEST(OwnChatPublicationRefusesForeignPrimaryCompanionsAndJournal)
{
	for (const bool after_worker : {false, true})
	for (const std::string role : {"primary", "summary", "index", "journal"})
	{
		TempDir temp("uam-own-continuation-foreign");
		const auto own = OwnContinuationLaunch(temp.root, true); // Genuine streamed index prerequisite.
		const auto primary = AppPaths::UamChatFilePath(temp.root, own->Graph().id);
		const auto summary = AppPaths::UamChatSummaryFilePath(temp.root, own->Graph().id);
		const auto index = uam::chat_pages::IndexPath(primary);
		UAM_ASSERT(fs::is_regular_file(index));
		const auto before = ReadFile(primary), summary_before = ReadFile(summary), index_before = ReadFile(index);
		std::shared_ptr<PreparedChatPublication> prepared;
		if (after_worker)
		{
			auto worker = std::async(std::launch::async, [&] { return ChatRepository::PrepareOwnChatPublication(own, own->Graph()); });
			if (worker.wait_for(std::chrono::seconds(30)) != std::future_status::ready)
			{
				std::cerr << "FATAL_PRIVATE_OWN_CONTINUATION: actual preparation did not settle" << std::endl; std::abort();
			}
			prepared = worker.get(); UAM_ASSERT(prepared && own->Current());
		}
		const auto changed = role == "primary" ? primary : role == "summary" ? summary : role == "index" ? index :
		    temp.root / "queue-outboxes" / (own->Graph().id + ".json");
		if (role == "journal")
		{
			fs::create_directories(changed.parent_path());
			UAM_ASSERT(uam::io::WriteTextFile(changed, "EXTERNAL QUEUE IS NOT OUR PRIMARY SINGLETON"));
		}
		else ReplaceOwnContinuationFile(changed);
		const auto foreign = ReadFile(changed); UAM_ASSERT(!own->Current());
		if (after_worker)
		{
			const auto rejected = ChatRepository::PublishStoppedChatLaunchStorage(prepared, [] { return true; });
			UAM_ASSERT_EQ(rejected.publication.state, ChatPublicationState::Rejected); UAM_ASSERT(!rejected.receipt);
		}
		else UAM_ASSERT(!ChatRepository::PrepareOwnChatPublication(own, own->Graph()));
		UAM_ASSERT_EQ(ReadFile(changed), foreign); UAM_ASSERT_EQ(ReadFile(primary), before);
		UAM_ASSERT_EQ(ReadFile(summary), summary_before); UAM_ASSERT_EQ(ReadFile(index), index_before);
	}
}

UAM_TEST(OwnChatPublicationRejectsForeignScopeAndLossyCandidate)
{
	for (const std::string variant : {"null", "host", "native", "birth", "policy", "unloaded", "two-heads", "lossy-head", "lossy-process", "owner"})
	{
		TempDir temp("uam-own-continuation-scope"); const auto own = OwnContinuationLaunch(temp.root, false);
		const auto primary = AppPaths::UamChatFilePath(temp.root, own->Graph().id), summary = AppPaths::UamChatSummaryFilePath(temp.root, own->Graph().id);
		const auto before = ReadFile(primary), summary_before = ReadFile(summary);
		ChatSession candidate = own->Graph();
		if (variant == "host") candidate.execution_host_id = "foreign-host";
		if (variant == "native") candidate.native_session_id = kBirthC;
		if (variant == "birth") candidate.managed_followup_incarnation = kBirthC;
		if (variant == "policy") candidate.command_safety_tier = "yolo";
		if (variant == "unloaded") candidate.messages_loaded = false;
		if (variant == "lossy-process") candidate.remote_process_exists = false;
		if (variant == "two-heads") candidate.acp_queued_prompts = {OwnContinuationHead(candidate), OwnContinuationHead(candidate)};
		if (variant == "lossy-head") { auto head = OwnContinuationHead(candidate); head.id.clear(); candidate.acp_queued_prompts = {head}; }
		std::string error;
		auto prepared = ChatRepository::PrepareOwnChatPublication(variant == "null" ? nullptr : own, candidate, &error);
		if (variant == "owner")
		{
			UAM_ASSERT(prepared && error.empty());
			const auto rejected = ChatRepository::PublishStoppedChatLaunchStorage(prepared, [] { return false; });
			UAM_ASSERT_EQ(rejected.publication.state, ChatPublicationState::Rejected); UAM_ASSERT(!rejected.receipt);
		}
		else UAM_ASSERT(!prepared && !error.empty());
		UAM_ASSERT_EQ(ReadFile(primary), before); UAM_ASSERT_EQ(ReadFile(summary), summary_before); UAM_ASSERT(own->Current());
	}
}

UAM_TEST(OwnChatPublicationRetainsUncertainAndDegradedCommitFactsWithoutReceipt)
{
	for (const std::string variant : {"uncertain", "summary", "index", "late-owner"})
	{
		TempDir temp("uam-own-continuation-facts"); const auto own = OwnContinuationLaunch(temp.root, true);
		const auto primary = AppPaths::UamChatFilePath(temp.root, own->Graph().id);
		ChatSession candidate = own->Graph(); candidate.remote_prompt_delivery_id = "known-new-storage-marker";
		auto prepared = ChatRepository::PrepareOwnChatPublication(own, candidate); UAM_ASSERT(prepared);
		bool reached = false; int owner_checks = 0;
		const auto result = ChatRepository::PublishStoppedChatLaunchStorage(prepared,
		    [&] { ++owner_checks; return variant != "late-owner" || owner_checks == 1; },
		    [&](ChatPublicationFile role, uam::io::AtomicWriteStage point)
		    {
			    const bool fault = variant == "uncertain" ? role == ChatPublicationFile::Primary && point == uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync :
			        ((variant == "summary" && role == ChatPublicationFile::Summary) || (variant == "index" && role == ChatPublicationFile::MessageIndex)) && point == uam::io::AtomicWriteStage::BeforeReplace;
			    if (fault) { reached = true; return false; } return true;
		    });
		UAM_ASSERT(!result.receipt && !own->Current());
		if (variant == "late-owner") { UAM_ASSERT(owner_checks >= 2); UAM_ASSERT_EQ(result.publication.state, ChatPublicationState::Committed); }
		else
		{
			UAM_ASSERT(reached);
			UAM_ASSERT_EQ(result.publication.state, variant == "uncertain" ? ChatPublicationState::Uncertain : ChatPublicationState::Committed);
			if (variant != "uncertain") UAM_ASSERT(result.publication.secondary_degraded);
		}
		UAM_ASSERT_EQ(nlohmann::json::parse(ReadFile(primary)).at("remote_prompt_delivery_id"), nlohmann::json(candidate.remote_prompt_delivery_id));
		const auto facts = ReadFile(primary);
		const auto replay = ChatRepository::PublishStoppedChatLaunchStorage(prepared, [] { return true; });
		UAM_ASSERT_EQ(replay.publication.state, ChatPublicationState::Rejected); UAM_ASSERT(!replay.receipt); UAM_ASSERT_EQ(ReadFile(primary), facts);
	}
}

UAM_TEST(OwnChatPublicationReceiptSeparatesIdenticalGraphsInDifferentRoots)
{
	TempDir first("uam-own-continuation-root-first"), second("uam-own-continuation-root-second");
	const auto local = OwnContinuationLaunch(first.root, false), foreign = OwnContinuationLaunch(second.root, false);
	UAM_ASSERT(local->Current() && foreign->Current());
	UAM_ASSERT_EQ(local->Graph().id, foreign->Graph().id); UAM_ASSERT_EQ(local->Graph().messages, foreign->Graph().messages);
	UAM_ASSERT_EQ(local->Primary().sha256, foreign->Primary().sha256);
	const auto first_path = AppPaths::UamChatFilePath(first.root, local->Graph().id);
	const auto second_path = AppPaths::UamChatFilePath(second.root, foreign->Graph().id);
	const auto first_bytes = ReadFile(first_path), second_bytes = ReadFile(second_path);
	UAM_ASSERT(local->MatchesDataRoot(first.root) && foreign->MatchesDataRoot(second.root));
	UAM_ASSERT(!local->MatchesDataRoot(second.root) && !foreign->MatchesDataRoot(first.root));
	UAM_ASSERT(local->MatchesDataRoot(first.root / "")); // Only the existing terminal-empty spelling rule.
	UAM_ASSERT(!local->MatchesDataRoot(first.root / ".") && !local->MatchesDataRoot({}));
	UAM_ASSERT(local->Current() && foreign->Current());
	UAM_ASSERT_EQ(ReadFile(first_path), first_bytes); UAM_ASSERT_EQ(ReadFile(second_path), second_bytes);
	// Actual C admission must use this association BEFORE calling PrepareOwn, plus its own current/runtime owner checks.
}


namespace uam
{
	// Observes only the real repository continuation worker; no fabricated completion/readiness.
	bool PendingOwnedChatPublicationForTests(const AppState& app, const std::string& chat_id, bool& ready);
}

namespace
{
	struct OwnedPublicationTestDrain
	{
		uam::AppState& app;
		~OwnedPublicationTestDrain()
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while ((!app.pending_chat_save_tasks.empty() || !app.pending_chat_save_at_by_chat_id.empty()) && std::chrono::steady_clock::now() < deadline)
			{
				(void)uam::FlushPendingChatSaves(app);
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			if (!app.pending_chat_save_tasks.empty() || !app.pending_chat_save_at_by_chat_id.empty())
			{
				std::cerr << "FATAL_PRIVATE_OWNED_PUBLICATION_DRAIN: actual save work did not settle" << std::endl;
				std::abort(); // Keep callbacks/App/private root alive beneath unfinished actual work.
			}
		}
	};

	void WaitOwnedPublicationActualWorkerReady(uam::AppState& app, const std::string& id)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		bool exists = false, ready = false;
		while (!exists && std::chrono::steady_clock::now() < deadline)
		{
			(void)uam::FlushPendingChatSaves(app);
			exists = uam::PendingOwnedChatPublicationForTests(app, id, ready);
			if (!exists) std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		UAM_ASSERT(exists);
		// Stop owner flushing after the actual future exists; its result remains unconsumed.
		while (!ready && std::chrono::steady_clock::now() < deadline)
		{
			UAM_ASSERT(uam::PendingOwnedChatPublicationForTests(app, id, ready));
			if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		UAM_ASSERT(ready);
	}

	void SettleOwnedPublicationActualWork(uam::AppState& app)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while ((!app.pending_chat_save_tasks.empty() || !app.pending_chat_save_at_by_chat_id.empty()) && std::chrono::steady_clock::now() < deadline)
		{
			(void)uam::FlushPendingChatSaves(app);
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		UAM_ASSERT(app.pending_chat_save_tasks.empty() && app.pending_chat_save_at_by_chat_id.empty());
	}
}

UAM_TEST(OwnChatAdmissionReturnsActualPublicationOnceAndRejectsForeignAfterWorker)
{
	for (const bool replace : {false, true})
	{
		TempDir temp("uam-owned-admission-callback");
		const auto predecessor = OwnContinuationLaunch(temp.root, true);
		uam::AppState app; app.data_root = temp.root; app.chats.push_back(predecessor->Graph());
		app.composer_text = "A separate draft remains unchanged.";
		ChatSession& live = app.chats.front(); live.title = "Actual owned full binding candidate";
		const auto authored = live.messages;
		int callbacks = 0;
		std::optional<StoppedChatLaunchPublicationResult> result;
		OwnedPublicationTestDrain drain{app};
		const auto owner = [&] { return app.data_root == temp.root && app.chats.front().messages == authored && app.chats.front().title == "Actual owned full binding candidate"; };
		UAM_ASSERT(uam::acp_detail::BeginOwnedChatPublicationAdmission(app, live, predecessor, owner,
		    [&](StoppedChatLaunchPublicationResult actual) { ++callbacks; result = std::move(actual); }) == uam::acp_detail::DurableAdmissionState::Pending);
		WaitOwnedPublicationActualWorkerReady(app, live.id);
		UAM_ASSERT_EQ(callbacks, 0);
		const auto primary = AppPaths::UamChatFilePath(temp.root, live.id), summary = AppPaths::UamChatSummaryFilePath(temp.root, live.id);
		const auto index = uam::chat_pages::IndexPath(primary);
		const auto saved = ChatRepository::LoadLocalChatBounded(temp.root, live.id, 4 * 1024 * 1024);
		UAM_ASSERT(saved && saved->title == live.title && saved->messages == authored && saved->remote_process_control_token == live.remote_process_control_token);
		UAM_ASSERT_EQ(saved->native_session_id, live.native_session_id);
		UAM_ASSERT(!predecessor->Current()); // Genuine new physical publication, not a read/no-op.
		const auto bytes = ReadFile(primary), summary_bytes = ReadFile(summary), index_bytes = ReadFile(index);
		const auto revision = ChatRepository::ChatStorageRevision(temp.root, live.id);
		const auto own_object = uam::platform::PublishedFileChangeWitness::Capture(primary); UAM_ASSERT(own_object && own_object->Current());
		if (replace)
		{
			ReplaceOwnContinuationFile(primary); // Actual same-byte atomic foreign object, with old mtime restored.
			UAM_ASSERT(!own_object->Current());
			UAM_ASSERT_EQ(ChatRepository::ChatStorageRevision(temp.root, live.id), revision);
		}
		SettleOwnedPublicationActualWork(app);
		UAM_ASSERT_EQ(callbacks, 1); UAM_ASSERT(result);
		UAM_ASSERT_EQ(result->publication.state, ChatPublicationState::Committed); // Facts survive callback refusal.
		UAM_ASSERT(!result->publication.backup_degraded && !result->publication.secondary_degraded);
		if (replace) UAM_ASSERT(!result->receipt);
		else
		{
			UAM_ASSERT(result->receipt && result->receipt->Current() && result->receipt->MatchesDataRoot(temp.root));
			UAM_ASSERT_EQ(result->receipt->Graph().messages, authored);
			UAM_ASSERT_EQ(result->receipt->Graph().title, live.title);
		}
		const auto final_object = uam::platform::PublishedFileChangeWitness::Capture(primary); UAM_ASSERT(final_object && final_object->Current());
		UAM_ASSERT_EQ(ReadFile(primary), bytes); UAM_ASSERT_EQ(ReadFile(summary), summary_bytes); UAM_ASSERT_EQ(ReadFile(index), index_bytes);
		UAM_ASSERT_EQ(live.messages, authored); UAM_ASSERT_EQ(app.composer_text, std::string("A separate draft remains unchanged."));
		(void)uam::FlushPendingChatSaves(app); // No implicit restage/adoption of the foreign object.
		UAM_ASSERT_EQ(callbacks, 1); UAM_ASSERT(final_object->Current());
		UAM_ASSERT(app.pending_chat_save_tasks.empty() && app.pending_chat_save_at_by_chat_id.empty());
	}
}

UAM_TEST(OwnChatAdmissionRetiresForNewerOrdinarySaveWithoutLosingItsSchedule)
{
	TempDir temp("uam-owned-admission-newer-normal");
	const auto predecessor = OwnContinuationLaunch(temp.root, false);
	uam::AppState app; app.data_root = temp.root; app.chats.push_back(predecessor->Graph());
	app.composer_text = "Preserve newer-save draft.";
	app.chats.front().title = "Owned binding candidate before newer work";
	int own_callbacks = 0, normal_callbacks = 0; bool normal_saved = false;
	std::optional<StoppedChatLaunchPublicationResult> result;
	OwnedPublicationTestDrain drain{app};
	UAM_ASSERT(uam::acp_detail::BeginOwnedChatPublicationAdmission(app, app.chats.front(), predecessor, [] { return true; },
	    [&](StoppedChatLaunchPublicationResult actual) { ++own_callbacks; result = std::move(actual); }) == uam::acp_detail::DurableAdmissionState::Pending);
	WaitOwnedPublicationActualWorkerReady(app, app.chats.front().id);
	UAM_ASSERT_EQ(own_callbacks, 0);
	ChatDomainService().AddMessage(app.chats.front(), MessageRole::User, "New independently authored work must survive.");
	app.chats.front().title = "Newer ordinary save owner";
	const auto newer = app.chats.front().messages;
	uam::acp_detail::ScheduleChatSave(app, app.chats.front(), 0.0);
	UAM_ASSERT(uam::DeferChatSaveResult(app, app.chats.front().id, [&](bool saved) { ++normal_callbacks; normal_saved = saved; }));
	SettleOwnedPublicationActualWork(app);
	UAM_ASSERT_EQ(own_callbacks, 1); UAM_ASSERT(result && !result->receipt);
	UAM_ASSERT_EQ(result->publication.state, ChatPublicationState::Committed); // Real first worker completed before the new schedule.
	UAM_ASSERT_EQ(normal_callbacks, 1); UAM_ASSERT(normal_saved);
	const auto saved = ChatRepository::LoadLocalChatBounded(temp.root, app.chats.front().id, 4 * 1024 * 1024);
	UAM_ASSERT(saved && saved->title == "Newer ordinary save owner" && saved->messages == newer);
	UAM_ASSERT_EQ(app.chats.front().messages, newer); UAM_ASSERT_EQ(app.composer_text, std::string("Preserve newer-save draft."));
	(void)uam::FlushPendingChatSaves(app); UAM_ASSERT_EQ(own_callbacks, 1); UAM_ASSERT_EQ(normal_callbacks, 1);
}

UAM_TEST(OwnChatAdmissionRejectsForeignRootReceiptBeforeScheduling)
{
	TempDir local("uam-owned-admission-local-root"), foreign("uam-owned-admission-foreign-root");
	const auto own = OwnContinuationLaunch(local.root, false), other = OwnContinuationLaunch(foreign.root, false);
	UAM_ASSERT(own->Current() && other->Current()); UAM_ASSERT_EQ(own->Graph().id, other->Graph().id);
	UAM_ASSERT_EQ(own->Graph().messages, other->Graph().messages);
	uam::AppState app; app.data_root = local.root; app.chats.push_back(own->Graph());
	const auto local_primary = AppPaths::UamChatFilePath(local.root, own->Graph().id), other_primary = AppPaths::UamChatFilePath(foreign.root, other->Graph().id);
	const auto local_bytes = ReadFile(local_primary), other_bytes = ReadFile(other_primary);
	const auto local_summary = ReadFile(AppPaths::UamChatSummaryFilePath(local.root, own->Graph().id));
	const auto other_summary = ReadFile(AppPaths::UamChatSummaryFilePath(foreign.root, other->Graph().id));
	int callbacks = 0;
	UAM_ASSERT(uam::acp_detail::BeginOwnedChatPublicationAdmission(app, app.chats.front(), other, [] { return true; },
	    [&](StoppedChatLaunchPublicationResult) { ++callbacks; }) == uam::acp_detail::DurableAdmissionState::Failed);
	UAM_ASSERT_EQ(callbacks, 0); UAM_ASSERT(app.pending_chat_save_tasks.empty() && app.pending_chat_save_at_by_chat_id.empty());
	UAM_ASSERT(own->Current() && other->Current());
	UAM_ASSERT_EQ(ReadFile(local_primary), local_bytes); UAM_ASSERT_EQ(ReadFile(other_primary), other_bytes);
	UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatSummaryFilePath(local.root, own->Graph().id)), local_summary);
	UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatSummaryFilePath(foreign.root, other->Graph().id)), other_summary);
}

UAM_TEST(OwnChatAdmissionObservesRealResultAfterOrdinarySynchronousSaveRemovesDebounce)
{
	TempDir temp("uam-owned-admission-sync-save");
	const auto predecessor = OwnContinuationLaunch(temp.root, false);
	uam::AppState app; app.data_root = temp.root; app.chats.push_back(predecessor->Graph());
	app.chats.front().title = "Owned candidate genuinely published first";
	int callbacks = 0, unrelated_callbacks = 0;
	std::optional<StoppedChatLaunchPublicationResult> result;
	OwnedPublicationTestDrain drain{app};
	UAM_ASSERT(uam::acp_detail::BeginOwnedChatPublicationAdmission(app, app.chats.front(), predecessor, [] { return true; },
	    [&](StoppedChatLaunchPublicationResult actual) { ++callbacks; result = std::move(actual); }) == uam::acp_detail::DurableAdmissionState::Pending);
	UAM_ASSERT(!uam::DeferChatSaveResult(app, app.chats.front().id, [&](bool) { ++unrelated_callbacks; })); // No stealing an exclusive receipt.
	WaitOwnedPublicationActualWorkerReady(app, app.chats.front().id);
	UAM_ASSERT_EQ(callbacks, 0);
	const auto first = ChatRepository::LoadLocalChatBounded(temp.root, app.chats.front().id, 4 * 1024 * 1024);
	UAM_ASSERT(first && first->title == "Owned candidate genuinely published first");
	app.chats.front().title = "Actual synchronous ordinary save";
	UAM_ASSERT(uam::acp_detail::SaveChatQuietly(app, app.chats.front()));
	UAM_ASSERT(!app.pending_chat_save_at_by_chat_id.contains(app.chats.front().id));
	bool ready = false; UAM_ASSERT(uam::PendingOwnedChatPublicationForTests(app, app.chats.front().id, ready) && ready);
	const auto primary = AppPaths::UamChatFilePath(temp.root, app.chats.front().id);
	const auto actual = ReadFile(primary), summary = ReadFile(AppPaths::UamChatSummaryFilePath(temp.root, app.chats.front().id));
	const auto current = uam::platform::PublishedFileChangeWitness::Capture(primary); UAM_ASSERT(current && current->Current());
	SettleOwnedPublicationActualWork(app); // Poll the actual former worker even though the ordinary save erased its debounce.
	UAM_ASSERT_EQ(callbacks, 1); UAM_ASSERT_EQ(unrelated_callbacks, 0);
	UAM_ASSERT(result && !result->receipt && result->publication.state == ChatPublicationState::Committed);
	const auto saved = ChatRepository::LoadLocalChatBounded(temp.root, app.chats.front().id, 4 * 1024 * 1024);
	UAM_ASSERT(saved && saved->title == "Actual synchronous ordinary save" && saved->messages == app.chats.front().messages);
	UAM_ASSERT_EQ(ReadFile(primary), actual); UAM_ASSERT_EQ(ReadFile(AppPaths::UamChatSummaryFilePath(temp.root, app.chats.front().id)), summary);
	(void)uam::FlushPendingChatSaves(app); UAM_ASSERT_EQ(callbacks, 1); UAM_ASSERT(current->Current());
}

UAM_TEST(OwnChatAdmissionRejectsChangedOwnerRowPolicyRootAndForceWithoutFallback)
{
	for (const std::string variant : {"owner", "row", "policy", "root", "force"})
	{
		TempDir temp("uam-owned-admission-revoked"), other("uam-owned-admission-new-root");
		const auto predecessor = OwnContinuationLaunch(temp.root, false);
		uam::AppState app; app.data_root = temp.root; app.chats.push_back(predecessor->Graph());
		const auto primary = AppPaths::UamChatFilePath(temp.root, predecessor->Graph().id), summary = AppPaths::UamChatSummaryFilePath(temp.root, predecessor->Graph().id);
		const auto bytes = ReadFile(primary), summary_bytes = ReadFile(summary);
		bool owned = true; int callbacks = 0; std::optional<StoppedChatLaunchPublicationResult> result;
		OwnedPublicationTestDrain drain{app};
		UAM_ASSERT(uam::acp_detail::BeginOwnedChatPublicationAdmission(app, app.chats.front(), predecessor, [&] { return owned; },
		    [&](StoppedChatLaunchPublicationResult actual) { ++callbacks; result = std::move(actual); }) == uam::acp_detail::DurableAdmissionState::Pending);
		if (variant == "owner") owned = false;
		if (variant == "row") app.chats.front().messages.front().content = "Changed exact row.";
		if (variant == "policy") app.chats.front().approval_mode = "changed-private-policy";
		if (variant == "root") app.data_root = other.root;
		if (variant == "force") (void)uam::FlushPendingChatSaves(app, true); // Must cancel, never publish via force fallback.
		SettleOwnedPublicationActualWork(app);
		UAM_ASSERT_EQ(callbacks, 1); UAM_ASSERT(result && !result->receipt);
		UAM_ASSERT_EQ(result->publication.state, ChatPublicationState::Rejected); // No worker was started.
		UAM_ASSERT(predecessor->Current()); UAM_ASSERT_EQ(ReadFile(primary), bytes); UAM_ASSERT_EQ(ReadFile(summary), summary_bytes);
		UAM_ASSERT(!fs::exists(AppPaths::UamChatFilePath(other.root, predecessor->Graph().id)));
		(void)uam::FlushPendingChatSaves(app); UAM_ASSERT_EQ(callbacks, 1); UAM_ASSERT(predecessor->Current());
	}
}

namespace
{
	// Injected lifetime/identity facts only: no PID, native process, byte writer, CLI or runner is created.
	struct ProcessFreeColdWitness
	{
		int runtime = 0;
		std::shared_ptr<const char> epoch = std::make_shared<const char>(0);
		std::shared_ptr<const char> writer = std::make_shared<const char>(0);
		std::shared_ptr<const char> observer = std::make_shared<const char>(0);
		bool owner_current = true, native_current = true, initial_current = true, handed_off = false;
		int writes = 0;
		uam::cold_retry::Sequence sequence{&runtime, [&] { return owner_current; }, [&] { return initial_current; }};
		uam::cold_retry::Identity Identity() const
		{
			return {&runtime, "injected-original-generation", epoch, writer, observer};
		}
		uam::cold_retry::Launch Launch() const
		{
			return {"/injected/working-directory", {"injected-command", "--resume", "injected-native"}, {{"INJECTED_KEY", "private launch value"}}};
		}
		void Observe()
		{
			auto launched = Identity(); launched.writer.reset(); launched.observer.reset();
			UAM_ASSERT(sequence.BeginProcessAttempt(Launch()));
			UAM_ASSERT(sequence.ProcessStarted(launched, Launch()));
			UAM_ASSERT(sequence.InitializeWritten(Identity()));
			UAM_ASSERT(sequence.NativeObserved(Identity(), [&] { return native_current; }));
		}
		void Ready()
		{
			Observe();
			UAM_ASSERT(sequence.BeginBinding(Identity()));
			initial_current = false; // The original file was genuinely replaced by our binding publication.
			UAM_ASSERT(sequence.BindingPublished(Identity(), [] { return true; }));
			UAM_ASSERT(sequence.Handoff(Identity(), [&] { handed_off = true; return true; }));
		}
	};
}

UAM_TEST(ColdWitnessSequenceRefusesCopiedIdentityReplacementStopAndExpiredCreators)
{
	for (const std::string fault : {"runtime", "same-process-new-epoch", "writer", "observer", "expired-observer", "stop"})
	{
		ProcessFreeColdWitness fixture; fixture.Ready();
		const auto original_epoch = fixture.epoch, original_writer = fixture.writer, original_observer = fixture.observer;
		auto identity = fixture.Identity();
		int foreign_runtime = 0;
		if (fault == "runtime") identity.runtime = &foreign_runtime;
		if (fault == "same-process-new-epoch") fixture.epoch = std::make_shared<const char>(0);
		if (fault == "writer") fixture.writer = std::make_shared<const char>(0);
		if (fault == "observer") fixture.observer = std::make_shared<const char>(0);
		if (fault == "expired-observer") identity.observer.reset();
		if (fault == "stop") fixture.sequence.Refuse(); // The same transition used by actual Stop/reset hooks.
		if (fault != "runtime" && fault != "expired-observer") identity = fixture.Identity();
		UAM_ASSERT(!fixture.sequence.Check(identity));
		fixture.epoch = original_epoch; fixture.writer = original_writer; fixture.observer = original_observer;
		UAM_ASSERT(!fixture.sequence.Check(fixture.Identity()));
		UAM_ASSERT(!fixture.sequence.WriteOnce(fixture.Identity(), "wire", "delivery", 1, 2, [&] { ++fixture.writes; return true; }));
		UAM_ASSERT_EQ(fixture.writes, 0);
	}
	ProcessFreeColdWitness expired; expired.Observe();
	const std::weak_ptr<const void> lifetime = expired.observer;
	expired.observer.reset();
	UAM_ASSERT(lifetime.expired()); // The transition fence retains no creator handles.
	UAM_ASSERT(!expired.sequence.Check(expired.Identity()));
}

UAM_TEST(ColdWitnessSequenceRequiresStartupHandoffAndExactFinalPublicationBeforeOneWrite)
{
	for (const std::string fault : {"none", "no-binding", "handoff", "no-packet", "foreign-publication", "post-final-foreign", "native-drift", "payload", "delivery", "turn", "request", "sink-failure", "sink-throw"})
	{
		ProcessFreeColdWitness fixture; fixture.Observe();
		bool final_current = true;
		if (fault == "no-binding")
		{
			UAM_ASSERT(!fixture.sequence.Handoff(fixture.Identity(), [] { return true; }));
		}
		else
		{
			UAM_ASSERT(fixture.sequence.BeginBinding(fixture.Identity()));
			fixture.initial_current = false;
			UAM_ASSERT(fixture.sequence.BindingPublished(fixture.Identity(), [] { return true; }));
			const bool handed_off = fixture.sequence.Handoff(fixture.Identity(), [&] { return fault != "handoff"; });
			UAM_ASSERT_EQ(handed_off, fault != "handoff");
			if (handed_off)
			{
				UAM_ASSERT(fixture.sequence.BeginPacket(fixture.Identity(), "exact-wire", "exact-delivery", 7, 19));
				if (fault != "no-packet")
				{
					const bool published = fixture.sequence.PacketPublished(fixture.Identity(), [&] { return final_current && fault != "foreign-publication"; });
					UAM_ASSERT_EQ(published, fault != "foreign-publication");
				}
			}
		}
		if (fault == "native-drift") fixture.native_current = false;
		if (fault == "post-final-foreign") final_current = false;
		const bool written = fixture.sequence.WriteOnce(fixture.Identity(), fault == "payload" ? "changed-wire" : "exact-wire",
		    fault == "delivery" ? "foreign-delivery" : "exact-delivery", fault == "turn" ? 8 : 7, fault == "request" ? 20 : 19, [&]
		    {
			    ++fixture.writes;
			    if (fault == "sink-throw") throw 1;
			    return fault != "sink-failure";
		    });
		UAM_ASSERT_EQ(written, fault == "none");
		const bool attempted = fault == "none" || fault == "sink-failure" || fault == "sink-throw";
		UAM_ASSERT_EQ(fixture.writes, attempted ? 1 : 0);
		UAM_ASSERT_EQ(fixture.sequence.InputAttempted(), attempted);
		UAM_ASSERT(!fixture.sequence.WriteOnce(fixture.Identity(), "exact-wire", "exact-delivery", 7, 19, [&] { ++fixture.writes; return true; }));
		UAM_ASSERT_EQ(fixture.writes, attempted ? 1 : 0);
	}
}

UAM_TEST(ColdWitnessSequenceSealsActualLaunchWithoutReplacementOrMetadataRebinding)
{
	for (const std::string fault : {"no-origin", "command", "environment", "workspace", "nul", "oversize", "second-attempt"})
	{
		ProcessFreeColdWitness fixture;
		auto launch = fixture.Launch();
		if (fault == "nul") launch.argv.front().push_back('\0');
		if (fault == "oversize") launch.environment.front().second.assign(8 * 1024 * 1024, 'x');
		if (fault != "no-origin")
		{
			const bool captured = fixture.sequence.BeginProcessAttempt(launch);
			UAM_ASSERT_EQ(captured, fault != "nul" && fault != "oversize");
		}
		if (fault == "command") launch.argv.front() = "replacement-command";
		if (fault == "environment") launch.environment.front().second = "changed-launch-environment";
		if (fault == "workspace") launch.working_directory = "/foreign-workspace";
		if (fault == "second-attempt") UAM_ASSERT(fixture.sequence.ProcessStarted(fixture.Identity(), launch));
		UAM_ASSERT(!fixture.sequence.ProcessStarted(fixture.Identity(), launch));
		UAM_ASSERT(!fixture.sequence.BeginProcessAttempt(fixture.Launch()));
		UAM_ASSERT_EQ(fixture.writes, 0);
	}
}

UAM_TEST(ColdWitnessSequenceUsesActualBindingAndPacketPublicationCallbacksWithoutProcesses)
{
	for (const std::string fault : {"none", "foreign-primary", "stop", "replacement", "newer-save"})
	{
		TempDir temp("uam-cold-witness-publication-no-process");
		auto receipt = OwnContinuationLaunch(temp.root, true);
		uam::AppState app; app.data_root = temp.root; app.chats.push_back(receipt->Graph());
		OwnedPublicationTestDrain drain{app};
		ProcessFreeColdWitness fixture;
		uam::cold_retry::Sequence sequence(&fixture.runtime, [&] { return fixture.owner_current; }, [receipt] { return receipt->Current(); });
		auto launched = fixture.Identity(); launched.writer.reset(); launched.observer.reset();
		UAM_ASSERT(sequence.BeginProcessAttempt(fixture.Launch()));
		UAM_ASSERT(sequence.ProcessStarted(launched, fixture.Launch()));
		UAM_ASSERT(sequence.InitializeWritten(fixture.Identity()));
		UAM_ASSERT(sequence.NativeObserved(fixture.Identity(), [&] { return fixture.native_current; }));
		UAM_ASSERT(sequence.BeginBinding(fixture.Identity()));
		int binding_callbacks = 0, packet_callbacks = 0;
		UAM_ASSERT(uam::acp_detail::BeginOwnedChatPublicationAdmission(app, app.chats.front(), receipt,
		    [&] { return sequence.Check(fixture.Identity()); }, [&](StoppedChatLaunchPublicationResult result)
		    {
			    ++binding_callbacks;
			    UAM_ASSERT(result.receipt && result.publication.state == ChatPublicationState::Committed);
			    UAM_ASSERT(sequence.BindingPublished(fixture.Identity(), [next = result.receipt] { return next->Current(); }));
			    receipt = result.receipt;
			    UAM_ASSERT(sequence.Handoff(fixture.Identity(), [&] { fixture.handed_off = true; return true; }));
		    }) == uam::acp_detail::DurableAdmissionState::Pending);
		WaitOwnedPublicationActualWorkerReady(app, receipt->Graph().id);
		UAM_ASSERT_EQ(binding_callbacks, 0); UAM_ASSERT(!fixture.handed_off);
		UAM_ASSERT(!receipt->Current());
		SettleOwnedPublicationActualWork(app);
		UAM_ASSERT_EQ(binding_callbacks, 1); UAM_ASSERT(fixture.handed_off && receipt->Current());
		auto& candidate = app.chats.front();
		candidate.remote_active_turn_id = "injected-cold-turn";
		candidate.remote_turn_serial = 7;
		candidate.remote_turn_user_message_index = 0;
		candidate.remote_turn_reconnect_pending = true;
		candidate.remote_prompt_delivery_session_id = "acp-" + candidate.id;
		candidate.remote_prompt_delivery_id = "injected-cold-delivery";
		candidate.remote_prompt_delivery_payload = "{\"type\":\"user\",\"privateFixture\":true}\n";
		UAM_ASSERT(sequence.BeginPacket(fixture.Identity(), candidate.remote_prompt_delivery_payload, candidate.remote_prompt_delivery_id, 7, 19));
		std::optional<ChatPublicationState> packet_fact;
		UAM_ASSERT(uam::acp_detail::BeginOwnedChatPublicationAdmission(app, candidate, receipt,
		    [&] { return sequence.Check(fixture.Identity()); }, [&](StoppedChatLaunchPublicationResult result)
		    {
			    ++packet_callbacks; packet_fact = result.publication.state;
			    if (!result.receipt) { sequence.Refuse(); return; }
			    UAM_ASSERT(sequence.PacketPublished(fixture.Identity(), [next = result.receipt] { return next->Current(); }));
			    receipt = result.receipt;
		    }) == uam::acp_detail::DurableAdmissionState::Pending);
		WaitOwnedPublicationActualWorkerReady(app, candidate.id);
		UAM_ASSERT_EQ(packet_callbacks, 0);
		const auto primary = AppPaths::UamChatFilePath(temp.root, candidate.id);
		const auto packet_bytes = ReadFile(primary);
		const auto foreign_stage = primary.parent_path() / "foreign-cold-stage.tmp";
		UAM_ASSERT(uam::io::WriteTextFile(foreign_stage, "foreign creator bytes"));
		if (fault == "foreign-primary") ReplaceOwnContinuationFile(primary);
		if (fault == "stop") sequence.Refuse();
		if (fault == "replacement") fixture.epoch = std::make_shared<const char>(0);
		if (fault == "newer-save")
		{
			candidate.title = "Newer independently scheduled save";
			uam::acp_detail::ScheduleChatSave(app, candidate, 0.0);
		}
		SettleOwnedPublicationActualWork(app);
		UAM_ASSERT_EQ(packet_callbacks, 1);
		UAM_ASSERT(packet_fact && *packet_fact == ChatPublicationState::Committed); // Commit facts survive callback refusal.
		const bool written = sequence.WriteOnce(fixture.Identity(), candidate.remote_prompt_delivery_payload,
		    candidate.remote_prompt_delivery_id, 7, 19, [&] { ++fixture.writes; return true; });
		UAM_ASSERT_EQ(written, fault == "none"); UAM_ASSERT_EQ(fixture.writes, fault == "none" ? 1 : 0);
		UAM_ASSERT_EQ(ReadFile(foreign_stage), std::string("foreign creator bytes"));
		if (fault != "newer-save") UAM_ASSERT_EQ(ReadFile(primary), packet_bytes);
		else
		{
			const auto saved = ChatRepository::LoadLocalChatBounded(temp.root, candidate.id, 4 * 1024 * 1024);
			UAM_ASSERT(saved && saved->title == candidate.title && saved->messages == candidate.messages);
		}
	}
}

UAM_TEST(ColdWitnessPublicationCancellationPreservesOriginalAndNewerSaveOwners)
{
	static_assert(!std::is_default_constructible_v<uam::acp_detail::OwnedChatPublicationCancellation>);
	for (const bool successor : {false, true})
	{
		TempDir temp("uam-cold-witness-exact-cancellation");
		const auto predecessor = OwnContinuationLaunch(temp.root, false);
		uam::AppState app; app.data_root = temp.root; app.chats.push_back(predecessor->Graph());
		OwnedPublicationTestDrain drain{app};
		const auto primary = AppPaths::UamChatFilePath(temp.root, predecessor->Graph().id);
		const auto original = ReadFile(primary);
		int own_callbacks = 0, newer_callbacks = 0;
		bool newer_saved = false;
		std::optional<ChatPublicationState> cancelled_fact;
		bool cancelled_receipt = true;
		std::shared_ptr<const uam::acp_detail::OwnedChatPublicationCancellation> cancellation;
		UAM_ASSERT(uam::acp_detail::BeginOwnedChatPublicationAdmission(app, app.chats.front(), predecessor, [] { return true; },
		    [&](StoppedChatLaunchPublicationResult result)
		    {
			    ++own_callbacks;
			    cancelled_fact = result.publication.state;
			    cancelled_receipt = static_cast<bool>(result.receipt);
		    }, &cancellation) == uam::acp_detail::DurableAdmissionState::Pending);
		UAM_ASSERT(cancellation);
		cancellation->Cancel(); // Before polling: no worker can race the refusal oracle.
		if (successor)
		{
			app.chats.front().title = "Newer ordinary owner survives old cancellation";
			uam::acp_detail::ScheduleChatSave(app, app.chats.front(), 0.0);
			UAM_ASSERT(uam::DeferChatSaveResult(app, app.chats.front().id, [&](bool saved) { ++newer_callbacks; newer_saved = saved; }));
		}
		cancellation->Cancel(); // This exact old handle cannot revoke the new ordinary schedule.
		SettleOwnedPublicationActualWork(app);
		UAM_ASSERT_EQ(own_callbacks, 1);
		UAM_ASSERT(cancelled_fact && *cancelled_fact == ChatPublicationState::Rejected && !cancelled_receipt);
		if (!successor)
		{
			UAM_ASSERT(predecessor->Current()); UAM_ASSERT_EQ(ReadFile(primary), original);
			UAM_ASSERT_EQ(newer_callbacks, 0);
		}
		else
		{
			UAM_ASSERT_EQ(newer_callbacks, 1); UAM_ASSERT(newer_saved);
			const auto saved = ChatRepository::LoadLocalChatBounded(temp.root, app.chats.front().id, 4 * 1024 * 1024);
			UAM_ASSERT(saved && saved->title == app.chats.front().title && saved->messages == app.chats.front().messages);
		}
	}
}
