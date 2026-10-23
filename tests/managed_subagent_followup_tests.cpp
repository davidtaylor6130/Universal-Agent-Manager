#include "test_harness.h"
#include "app/managed_subagent_followup_service.h"

#include <nlohmann/json.hpp>

#include <algorithm>

using namespace uam_test;

namespace
{
	constexpr const char* kFollowupRunId = "11111111-1111-4111-8111-111111111111";
	constexpr const char* kFollowupParentRunId = "22222222-2222-4222-8222-222222222222";
	constexpr const char* kFollowupRootBirth = "33333333-3333-4333-8333-333333333333";
	constexpr const char* kFollowupTranscriptBirth = "44444444-4444-4444-8444-444444444444";
	constexpr const char* kFollowupSecondTurn = "55555555-5555-4555-8555-555555555555";
	constexpr const char* kFollowupClientRequest = "66666666-6666-4666-8666-666666666666";
	constexpr const char* kFollowupNativeSession = "77777777-7777-4777-8777-777777777777";

	fs::path FollowupRunPath(const fs::path& data_root, std::string_view run_id = kFollowupRunId)
	{
		return data_root / "agent-runs" / (std::string(run_id) + ".json");
	}

	AgentRun FollowupLegacyRun()
	{
		AgentRun run;
		run.id = kFollowupRunId;
		run.root_chat_id = "managed-followup-root";
		run.parent_run_id = kFollowupParentRunId;
		run.transcript_chat_id = "managed-followup-transcript";
		run.agent_id = "reviewer";
		run.definition_hash = "fixture-definition-identity";
		run.definition_snapshot = "---\nversion: 1\n---\nKeep the saved restrictive policy.";
		run.definition_instructions = "Keep the saved restrictive policy.";
		run.provider_id = "codex-cli";
		run.model_id = "fixture-model-exact";
		run.execution_capability = "uam-prompt-injected";
		run.task = "Original managed task.";
		run.effective_workspace_access = "read";
		run.status = "completed";
		run.depth = 2;
		run.expected_turn_serial = 1;
		run.started_at_epoch_ms = 1000;
		run.finished_at_epoch_ms = 1100;
		run.deadline_at_epoch_ms = 1801000;
		run.created_at = "2026-10-17 10:00:00";
		run.started_at = run.created_at;
		run.finished_at = "2026-10-17 10:00:01";
		run.updated_at = run.finished_at;
		run.result_delivery_id = run.id;
		run.deliver_result_to_root_chat = true;
		run.root_result_delivered = true;
		run.root_result_delivery_attempts = 1;
		return run;
	}

	AgentRun FollowupLegacyParentRun()
	{
		AgentRun parent = FollowupLegacyRun();
		parent.id = kFollowupParentRunId;
		parent.parent_run_id.clear();
		parent.transcript_chat_id = "managed-followup-parent-transcript";
		parent.agent_id = "delegator";
		parent.task = "Original parent task.";
		parent.depth = 1;
		parent.result_delivery_id = parent.id;
		return parent;
	}

	nlohmann::json FollowupV2Fixture(const fs::path& data_root)
	{
		// Preserve the actual depth-two ancestry required by the public loader.
		UAM_ASSERT(uam::AgentRunLedger::Save(data_root, FollowupLegacyParentRun()));
		// Seed through the existing public writer; no future C++ declaration is needed for RED.
		UAM_ASSERT(uam::AgentRunLedger::Save(data_root, FollowupLegacyRun()));
		nlohmann::json value = nlohmann::json::parse(ReadFile(FollowupRunPath(data_root)));
		const nlohmann::json principal = {
		    {"kind", "user-root"}, {"rootChatId", value.at("rootChatId")},
		    {"rootBirthId", kFollowupRootBirth}, {"parentRunId", ""},
		    {"parentTranscriptBirthId", ""}};
		nlohmann::json first = {
		    {"turnId", kFollowupRunId}, {"turnNumber", 1}, {"clientRequestId", ""},
		    {"afterTurnId", ""}, {"principal", principal}, {"text", ""},
		    {"continuity", "native"}, {"recoveryDecision", "none"}, {"retryOfTurnId", ""},
		    {"status", "completed"}, {"deliveryId", kFollowupRunId},
		    {"dispatchAttempted", true}, {"expectedTurnSerial", 1},
		    {"firstUserMessageIndex", 0}, {"userMessageCreatedAt", "2026-10-17 10:00:00"},
		    {"startedAtEpochMs", 1000}, {"finishedAtEpochMs", 1100},
		    {"resultRequired", true}, {"resultDelivered", true},
		    {"resultDeliveryAttempts", 1}, {"auditPending", false}, {"diagnosticCode", ""}};
		nlohmann::json second = first;
		second["turnId"] = kFollowupSecondTurn;
		second["turnNumber"] = 2;
		second["clientRequestId"] = kFollowupClientRequest;
		second["afterTurnId"] = kFollowupRunId;
		second["text"] = "  Follow up with exact authored bytes.\nKeep \\\"quoted\\\" text and \xe2\x80\x94 Unicode.  ";
		second["status"] = "queued";
		second["deliveryId"] = kFollowupSecondTurn;
		second["dispatchAttempted"] = false;
		second["expectedTurnSerial"] = 0;
		second["firstUserMessageIndex"] = -1;
		second["userMessageCreatedAt"] = "";
		second["startedAtEpochMs"] = 0;
		second["finishedAtEpochMs"] = 0;
		second["resultDelivered"] = false;
		second["resultDeliveryAttempts"] = 0;
		// These are inert codec fixtures, never runtime/provider attestation or launch authority.
		const nlohmann::json execution = {
		    {"connectionConfigurationJson", nlohmann::json::array({"local", "local", "", "linux", "x64", "", 0, "", "", nullptr}).dump()},
		    {"providerConfigurationJson", nlohmann::json{{"fixture", "exact-saved-configuration"}}.dump()},
		    {"executionHostId", "local"}, {"workspaceDirectory", "/fixture/workspace"},
		    {"workspaceWorktreeDirectory", ""}, {"workspaceSourceDirectory", "/fixture/workspace"},
		    {"routingMode", "explicit"}, {"routingSourceFolderId", ""}, {"routingSourceDirectory", ""},
		    {"effectiveModelId", "fixture-model-exact"}, {"modelIdentitySource", "exact-requested"},
		    {"reasoningEffort", "high"}, {"serviceTier", ""}, {"serviceTierExplicit", false},
		    {"approvalMode", "plan"}, {"commandSafetyTier", "off"}, {"nativeSessionId", kFollowupNativeSession}};
		value["version"] = 2;
		value["status"] = "queued";
		value["expectedTurnSerial"] = 0;
		value["startedAtEpochMs"] = value["finishedAtEpochMs"] = value["deadlineAtEpochMs"] = 0;
		value["startedAt"] = value["finishedAt"] = "";
		value["updatedAt"] = "2026-10-17 10:00:02";
		value["managedContinuation"] = {
		    {"owner", {{"rootBirthId", kFollowupRootBirth}, {"transcriptBirthId", kFollowupTranscriptBirth}, {"delegationGrant", nullptr}}},
		    {"execution", execution}, {"currentTurnId", kFollowupSecondTurn},
		    {"currentTurnNumber", 2}, {"stateRevision", 2},
		    {"turns", nlohmann::json::array({first, second})}};
		return value;
	}
}

UAM_TEST(ManagedFollowupLedgerV2RoundTripsTwoTurnsThroughPublicSave)
{
	TempDir temp("uam-managed-followup-v2-roundtrip");
	const nlohmann::json expected = FollowupV2Fixture(temp.root);
	const fs::path primary = FollowupRunPath(temp.root);
	const fs::path parent_primary = FollowupRunPath(temp.root, kFollowupParentRunId);
	const std::string parent_bytes = ReadFile(parent_primary);
	const std::string original = expected.dump(2);
	UAM_ASSERT(uam::io::WriteTextFile(primary, original));
	const uam::AgentRunLoadResult loaded = uam::AgentRunLedger::LoadAllReadOnly(temp.root);
	// Current v1-only parser reaches this runtime RED without missing future declarations.
	UAM_ASSERT_EQ(loaded.runs.size(), std::size_t{2});
	UAM_ASSERT(loaded.errors.empty());
	UAM_ASSERT_EQ(ReadFile(primary), original);
	UAM_ASSERT_EQ(ReadFile(parent_primary), parent_bytes);
	const auto target = std::ranges::find(loaded.runs, std::string(kFollowupRunId), &AgentRun::id);
	UAM_ASSERT(target != loaded.runs.end());
	const AgentRun& run = *target;
	UAM_ASSERT_EQ(run.id, std::string(kFollowupRunId));
	UAM_ASSERT_EQ(run.root_chat_id, expected.at("rootChatId").get<std::string>());
	UAM_ASSERT_EQ(run.parent_run_id, expected.at("parentRunId").get<std::string>());
	UAM_ASSERT_EQ(run.transcript_chat_id, expected.at("transcriptChatId").get<std::string>());
	UAM_ASSERT_EQ(run.depth, 2);
	UAM_ASSERT_EQ(run.definition_snapshot, expected.at("definitionSnapshot").get<std::string>());
	UAM_ASSERT_EQ(run.provider_id, std::string("codex-cli"));
	UAM_ASSERT_EQ(run.model_id, std::string("fixture-model-exact"));
	UAM_ASSERT_EQ(run.result_delivery_id, run.id);
	UAM_ASSERT(uam::AgentRunLedger::Save(temp.root, run));
	const nlohmann::json saved = nlohmann::json::parse(ReadFile(primary));
	UAM_ASSERT_EQ(saved.at("version"), nlohmann::json(2));
	// Exact whole-section equality covers both receipts, raw text, births, native ID and per-turn results.
	UAM_ASSERT_EQ(saved.at("managedContinuation"), expected.at("managedContinuation"));
	UAM_ASSERT_EQ(saved.at("managedContinuation").at("execution").at("nativeSessionId"), nlohmann::json(kFollowupNativeSession));
	UAM_ASSERT_EQ(saved, expected);
	const uam::AgentRunLoadResult again = uam::AgentRunLedger::LoadAllReadOnly(temp.root);
	UAM_ASSERT_EQ(again.runs.size(), std::size_t{2});
	UAM_ASSERT(again.errors.empty());
	const auto reloaded_target = std::ranges::find(again.runs, std::string(kFollowupRunId), &AgentRun::id);
	UAM_ASSERT(reloaded_target != again.runs.end());
	UAM_ASSERT(uam::AgentRunLedger::Save(temp.root, *reloaded_target));
	UAM_ASSERT_EQ(nlohmann::json::parse(ReadFile(primary)), expected);
	UAM_ASSERT_EQ(ReadFile(parent_primary), parent_bytes);
}

UAM_TEST(ManagedFollowupLedgerMalformedV2NeverFallsBackDuringLiveRead)
{
	TempDir temp("uam-managed-followup-v2-malformed");
	const nlohmann::json valid = FollowupV2Fixture(temp.root);
	const fs::path primary = FollowupRunPath(temp.root);
	const fs::path parent_primary = FollowupRunPath(temp.root, kFollowupParentRunId);
	const std::string parent_bytes = ReadFile(parent_primary);
	// Keep a valid v1 recovery candidate: the live reader must not adopt or publish it.
	UAM_ASSERT(uam::AgentRunLedger::Save(temp.root, FollowupLegacyRun()));
	const fs::path backup = uam::io::MakeBackupPath(primary);
	const std::string backup_before = ReadFile(backup);
	UAM_ASSERT_EQ(nlohmann::json::parse(backup_before).at("version"), nlohmann::json(1));
	std::vector<nlohmann::json> malformed;
	malformed.push_back(valid);
	malformed.back().erase("managedContinuation");
	for (const auto& section : {nlohmann::json(nullptr), nlohmann::json(false), nlohmann::json("invalid"), nlohmann::json::array(), nlohmann::json::object()})
	{
		malformed.push_back(valid);
		malformed.back()["managedContinuation"] = section;
	}
	malformed.push_back(valid);
	malformed.back()["managedContinuation"]["owner"]["rootBirthId"] = "invalid-birth";
	malformed.push_back(valid);
	malformed.back()["managedContinuation"]["turns"][1]["afterTurnId"] = kFollowupSecondTurn;
	malformed.push_back(valid);
	malformed.back()["managedContinuation"]["turns"][1]["clientRequestId"] = false;
	malformed.push_back(valid);
	malformed.back()["managedContinuation"]["turns"][1]["continuity"] = "invented-mode";
	malformed.push_back(valid);
	malformed.back()["managedContinuation"]["currentTurnNumber"] = 1;
	malformed.push_back(valid);
	malformed.back()["managedContinuation"]["execution"]["nativeSessionId"] = nullptr;
	for (const auto& value : malformed)
	{
		const std::string bytes = value.dump(2);
		UAM_ASSERT(uam::io::WriteTextFile(primary, bytes));
		const uam::AgentRunLoadResult loaded = uam::AgentRunLedger::LoadAllReadOnly(temp.root);
		UAM_ASSERT_EQ(loaded.runs.size(), std::size_t{1});
		UAM_ASSERT(std::ranges::find(loaded.runs, std::string(kFollowupRunId), &AgentRun::id) == loaded.runs.end());
		UAM_ASSERT_EQ(loaded.runs.front().id, std::string(kFollowupParentRunId));
		UAM_ASSERT_EQ(loaded.runs.front().root_chat_id, valid.at("rootChatId").get<std::string>());
		UAM_ASSERT(loaded.runs.front().parent_run_id.empty());
		UAM_ASSERT_EQ(loaded.runs.front().depth, 1);
		UAM_ASSERT(!loaded.errors.empty());
		UAM_ASSERT_EQ(ReadFile(primary), bytes);
		UAM_ASSERT_EQ(ReadFile(backup), backup_before);
		UAM_ASSERT_EQ(ReadFile(parent_primary), parent_bytes);
	}
}

UAM_TEST(ManagedFollowupLedgerLegacyV1RoundTripDoesNotMintContinuation)
{
	TempDir temp("uam-managed-followup-v1-unchanged");
	const AgentRun original = FollowupLegacyRun();
	UAM_ASSERT(uam::AgentRunLedger::Save(temp.root, FollowupLegacyParentRun()));
	UAM_ASSERT(uam::AgentRunLedger::Save(temp.root, original));
	const fs::path primary = FollowupRunPath(temp.root);
	const fs::path parent_primary = FollowupRunPath(temp.root, kFollowupParentRunId);
	const std::string parent_bytes = ReadFile(parent_primary);
	const nlohmann::json expected_parent = nlohmann::json::parse(parent_bytes);
	const std::string bytes = ReadFile(primary);
	const nlohmann::json expected = nlohmann::json::parse(bytes);
	UAM_ASSERT_EQ(expected.at("version"), nlohmann::json(1));
	UAM_ASSERT(!expected.contains("managedContinuation"));
	const uam::AgentRunLoadResult loaded = uam::AgentRunLedger::LoadAllReadOnly(temp.root);
	UAM_ASSERT_EQ(loaded.runs.size(), std::size_t{2});
	UAM_ASSERT(loaded.errors.empty());
	UAM_ASSERT_EQ(ReadFile(primary), bytes);
	UAM_ASSERT_EQ(ReadFile(parent_primary), parent_bytes);
	const auto target = std::ranges::find(loaded.runs, original.id, &AgentRun::id);
	UAM_ASSERT(target != loaded.runs.end());
	UAM_ASSERT(std::ranges::find(loaded.runs, std::string(kFollowupParentRunId), &AgentRun::id) != loaded.runs.end());
	UAM_ASSERT_EQ(target->id, original.id);
	UAM_ASSERT_EQ(target->root_chat_id, original.root_chat_id);
	UAM_ASSERT_EQ(target->parent_run_id, original.parent_run_id);
	UAM_ASSERT_EQ(target->transcript_chat_id, original.transcript_chat_id);
	UAM_ASSERT_EQ(target->depth, 2);
	for (const AgentRun& run : loaded.runs)
		UAM_ASSERT(uam::AgentRunLedger::Save(temp.root, run));
	UAM_ASSERT_EQ(nlohmann::json::parse(ReadFile(primary)), expected);
	UAM_ASSERT(!nlohmann::json::parse(ReadFile(primary)).contains("managedContinuation"));
	UAM_ASSERT_EQ(nlohmann::json::parse(ReadFile(parent_primary)), expected_parent);
	UAM_ASSERT_EQ(expected_parent.at("version"), nlohmann::json(1));
	UAM_ASSERT(!expected_parent.contains("managedContinuation"));
}


UAM_TEST(ManagedFollowupStartupCannotAdoptOrdinaryBackupOverInvalidManagedPrimary)
{
	TempDir temp("uam-managed-followup-parse-failure-backup");
	ChatSession legacy;
	legacy.id = "managed-followup-recovery-guard";
	legacy.title = "Managed follow-up recovery guard";
	legacy.provider_id = "codex-cli";
	legacy.native_session_id = kFollowupNativeSession;
	legacy.created_at = legacy.updated_at = "2026-10-17 10:00:00";
	uam::AcpQueuedUserPromptState ordinary;
	ordinary.id = kFollowupSecondTurn;
	ordinary.text = "Ordinary backup input must not replace unsupported managed input.";
	legacy.acp_queued_prompts.push_back(ordinary);
	// Create the genuine legacy recovery candidate through the existing public writer.
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, legacy));
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, legacy));
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, legacy.id);
	const fs::path backup = uam::io::MakeBackupPath(primary);
	const std::string backup_bytes = ReadFile(backup);
	const auto backup_value = nlohmann::json::parse(backup_bytes);
	UAM_ASSERT_EQ(backup_value.at("acp_queued_prompts").size(), std::size_t{1});
	UAM_ASSERT(!backup_value.at("acp_queued_prompts")[0].contains("managed_turn"));
	UAM_ASSERT(!backup_value.at("acp_queue_held").get<bool>());

	auto primary_value = nlohmann::json::parse(ReadFile(primary));
	primary_value["acp_queued_prompts"][0]["managed_turn"] = {
	    {"run_id", kFollowupRunId}, {"turn_id", kFollowupSecondTurn}, {"turn_number", 2},
	    {"expected_native_session_id", kFollowupNativeSession}, {"continuity", "native"}};
	// Preserve valid structured JSON and a complete managed marker, but force the real
	// parser's unrelated non-array-messages failure before it can return a ChatSession.
	primary_value["messages"] = nlohmann::json::object();
	const std::string primary_bytes = primary_value.dump(2);
	UAM_ASSERT(uam::io::WriteTextFile(primary, primary_bytes));
	std::string warning;
	const auto loaded = ChatRepository::LoadLocalChats(temp.root, &warning);
	UAM_ASSERT_EQ(ReadFile(primary), primary_bytes);
	UAM_ASSERT_EQ(ReadFile(backup), backup_bytes);
	// An unavailable record may be omitted or returned held, never as ordinary runnable input.
	UAM_ASSERT(std::ranges::none_of(loaded, [&](const ChatSession& chat)
	{
		return chat.id == legacy.id && !chat.imported_read_only && !chat.acp_queue_held &&
		    !chat.acp_queue_storage_error && !chat.acp_queued_prompts.empty();
	}));
}

UAM_TEST(ManagedFollowupEncodedLimitRejectsBeforeAnyLedgerWrite)
{
	TempDir temp("uam-managed-followup-encoded-limit");
	const AgentRun original = FollowupLegacyParentRun();
	UAM_ASSERT(uam::AgentRunLedger::Save(temp.root, original));
	UAM_ASSERT(uam::AgentRunLedger::Save(temp.root, original));
	const fs::path primary = FollowupRunPath(temp.root, kFollowupParentRunId), backup = uam::io::MakeBackupPath(primary);
	const std::string bytes = ReadFile(primary), backup_bytes = ReadFile(backup);
	AgentRun oversized = original;
	// Within the existing 256 KiB field limit; JSON escaping alone exceeds the 512 KiB file limit.
	oversized.definition_snapshot.assign(256U * 1024U, '\n');
	std::string error;
	UAM_ASSERT(!uam::AgentRunLedger::Save(temp.root, oversized, &error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT_EQ(ReadFile(primary), bytes);
	UAM_ASSERT_EQ(ReadFile(backup), backup_bytes);
	const fs::path absent = temp.root / "must-not-be-created";
	UAM_ASSERT(!uam::AgentRunLedger::Save(absent, oversized, &error));
	UAM_ASSERT(!fs::exists(absent));
}

UAM_TEST(ManagedFollowupConsumedJournalCannotBlockCanonicalClearedQueue)
{
	TempDir temp("uam-managed-followup-consumed-journal");
	ChatSession chat;
	chat.id = "managed-followup-consumed-journal";
	chat.title = "Consumed queue journal authority";
	chat.provider_id = "codex-cli";
	chat.created_at = chat.updated_at = "2026-10-17 10:00:00";
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	uam::AcpQueuedUserPromptState ordinary;
	ordinary.id = kFollowupSecondTurn;
	ordinary.text = "This obsolete journal row has already been consumed.";
	chat.acp_queued_prompts.push_back(ordinary);
	UAM_ASSERT(ChatRepository::SaveQueuedPromptOutbox(temp.root, chat));
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id);
	const fs::path outbox = temp.root / "queue-outboxes" / (chat.id + ".json");
	auto stale_journal = nlohmann::json::parse(ReadFile(outbox));
	const std::string consumed_revision = chat.acp_queue_storage_revision;
	UAM_ASSERT(!consumed_revision.empty());
	UAM_ASSERT_EQ(stale_journal.at("revision"), nlohmann::json(consumed_revision));
	chat.acp_queued_prompts.clear();
	// Establish the consumed revision using actual ordinary APIs before adding raw
	// future identity: the current decoder cannot re-encode unknown journal fields.
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const std::string canonical_bytes = ReadFile(primary);
	const auto canonical = nlohmann::json::parse(canonical_bytes);
	UAM_ASSERT_EQ(canonical.at("acp_queue_storage_revision"), nlohmann::json(consumed_revision));
	UAM_ASSERT(canonical.at("acp_queued_prompts").empty());
	stale_journal["acp_queued_prompts"][0]["managed_turn"] = {
	    {"run_id", kFollowupRunId}, {"turn_id", kFollowupSecondTurn}, {"turn_number", 2},
	    {"expected_native_session_id", kFollowupNativeSession}, {"continuity", "native"}};
	const std::string stale_bytes = stale_journal.dump(2);
	UAM_ASSERT(uam::io::WriteTextFile(outbox, stale_bytes));
	auto loaded = ChatRepository::LoadLocalChatBounded(temp.root, chat.id, 1024U * 1024U);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT_EQ(ReadFile(primary), canonical_bytes);
	UAM_ASSERT_EQ(ReadFile(outbox), stale_bytes);
	UAM_ASSERT_EQ(loaded->acp_queue_storage_revision, consumed_revision);
	UAM_ASSERT(loaded->acp_queued_prompts.empty());
	UAM_ASSERT(!loaded->acp_queue_held && !loaded->acp_queue_storage_error);
	const auto prepared = ChatRepository::PrepareChatSave(temp.root, *loaded);
	UAM_ASSERT(prepared != nullptr);
	UAM_ASSERT_EQ(ReadFile(primary), canonical_bytes);
	UAM_ASSERT_EQ(ReadFile(outbox), stale_bytes);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, *loaded));
	UAM_ASSERT(nlohmann::json::parse(ReadFile(primary)).at("acp_queued_prompts").empty());
	UAM_ASSERT_EQ(ReadFile(outbox), stale_bytes);
}


UAM_TEST(ManagedFollowupStartupLeavesV2InertAndPreservesLegacyParent)
{
	for (const std::string status : {"queued", "running", "completed"})
	{
		TempDir temp("uam-managed-followup-startup-inert");
		auto value = FollowupV2Fixture(temp.root);
		value["status"] = status;
		value["managedContinuation"]["turns"][1]["status"] = status;
		value["managedContinuation"]["turns"][1]["dispatchAttempted"] = status != "queued";
		const fs::path primary = FollowupRunPath(temp.root);
		const fs::path parent = FollowupRunPath(temp.root, kFollowupParentRunId);
		const std::string bytes = value.dump(2), parent_bytes = ReadFile(parent);
		UAM_ASSERT(uam::io::WriteTextFile(primary, bytes));
		const auto inspected = uam::AgentRunLedger::LoadAllReadOnly(temp.root);
		UAM_ASSERT_EQ(inspected.runs.size(), std::size_t{2});
		UAM_ASSERT(inspected.errors.empty());
		const auto startup = uam::AgentRunLedger::LoadAll(temp.root);
		UAM_ASSERT_EQ(startup.runs.size(), std::size_t{1});
		UAM_ASSERT_EQ(startup.runs.front().id, std::string(kFollowupParentRunId));
		UAM_ASSERT(std::ranges::any_of(startup.errors, [](const std::string& error)
		    { return error.find("inert") != std::string::npos; }));
		UAM_ASSERT_EQ(ReadFile(primary), bytes);
		UAM_ASSERT_EQ(ReadFile(parent), parent_bytes);
		UAM_ASSERT(!fs::exists(uam::io::MakeBackupPath(primary)));
	}
}

UAM_TEST(ManagedFollowupMalformedStartupNeverAdoptsItsLegacyBackup)
{
	TempDir temp("uam-managed-followup-startup-malformed");
	auto value = FollowupV2Fixture(temp.root);
	UAM_ASSERT(uam::AgentRunLedger::Save(temp.root, FollowupLegacyRun()));
	const fs::path primary = FollowupRunPath(temp.root), backup = uam::io::MakeBackupPath(primary);
	const std::string backup_bytes = ReadFile(backup);
	value["managedContinuation"] = false;
	const std::string bytes = value.dump(2);
	UAM_ASSERT(uam::io::WriteTextFile(primary, bytes));
	const auto startup = uam::AgentRunLedger::LoadAll(temp.root);
	UAM_ASSERT_EQ(startup.runs.size(), std::size_t{1});
	UAM_ASSERT_EQ(startup.runs.front().id, std::string(kFollowupParentRunId));
	UAM_ASSERT(!startup.errors.empty());
	UAM_ASSERT_EQ(ReadFile(primary), bytes);
	UAM_ASSERT_EQ(ReadFile(backup), backup_bytes);
}

UAM_TEST(ManagedFollowupOversizedStartupNeverAdoptsItsLegacyBackup)
{
	TempDir temp("uam-managed-followup-startup-oversized");
	const auto value = FollowupV2Fixture(temp.root);
	UAM_ASSERT(uam::AgentRunLedger::Save(temp.root, FollowupLegacyRun()));
	const fs::path primary = FollowupRunPath(temp.root), backup = uam::io::MakeBackupPath(primary);
	const fs::path parent = FollowupRunPath(temp.root, kFollowupParentRunId);
	const std::string backup_bytes = ReadFile(backup), parent_bytes = ReadFile(parent);
	UAM_ASSERT_EQ(nlohmann::json::parse(backup_bytes).at("version"), nlohmann::json(1));
	std::string bytes = value.dump(2);
	constexpr std::size_t maximum_run_bytes = 512U * 1024U;
	UAM_ASSERT(bytes.size() < maximum_run_bytes);
	// Legal trailing JSON whitespace crosses the read limit without obscuring the v2 marker.
	bytes.resize(maximum_run_bytes + 1U, ' ');
	UAM_ASSERT_EQ(nlohmann::json::parse(bytes), value);
	UAM_ASSERT(uam::io::WriteTextFile(primary, bytes));
	const auto startup = uam::AgentRunLedger::LoadAll(temp.root);
	UAM_ASSERT_EQ(ReadFile(primary), bytes);
	UAM_ASSERT_EQ(ReadFile(backup), backup_bytes);
	UAM_ASSERT_EQ(ReadFile(parent), parent_bytes);
	UAM_ASSERT(std::ranges::none_of(startup.runs, [](const AgentRun& run) { return run.id == kFollowupRunId; }));
	UAM_ASSERT_EQ(startup.runs.size(), std::size_t{1});
	UAM_ASSERT_EQ(startup.runs.front().id, std::string(kFollowupParentRunId));
	UAM_ASSERT(!startup.errors.empty());
}

UAM_TEST(ManagedFollowupStartupStillRecoversOrdinaryLegacyCorruption)
{
	TempDir temp("uam-managed-followup-startup-legacy-recovery");
	UAM_ASSERT(uam::AgentRunLedger::Save(temp.root, FollowupLegacyParentRun()));
	UAM_ASSERT(uam::AgentRunLedger::Save(temp.root, FollowupLegacyRun()));
	UAM_ASSERT(uam::AgentRunLedger::Save(temp.root, FollowupLegacyRun()));
	const fs::path primary = FollowupRunPath(temp.root), backup = uam::io::MakeBackupPath(primary);
	const std::string backup_bytes = ReadFile(backup);
	UAM_ASSERT_EQ(nlohmann::json::parse(backup_bytes).at("version"), nlohmann::json(1));
	UAM_ASSERT(uam::io::WriteTextFile(primary, "unparseable legacy corruption"));
	const auto startup = uam::AgentRunLedger::LoadAll(temp.root);
	UAM_ASSERT(startup.errors.empty());
	UAM_ASSERT_EQ(startup.runs.size(), std::size_t{2});
	const auto recovered = std::ranges::find(startup.runs, std::string(kFollowupRunId), &AgentRun::id);
	UAM_ASSERT(recovered != startup.runs.end());
	UAM_ASSERT(!recovered->managed_continuation);
	UAM_ASSERT_EQ(ReadFile(primary), backup_bytes);
	UAM_ASSERT_EQ(ReadFile(backup), backup_bytes);
}

UAM_TEST(ManagedFollowupStartupNeverPublishesV2BackupOrUnboundLegacyDescendant)
{
	TempDir temp("uam-managed-followup-startup-ancestry");
	auto value = FollowupV2Fixture(temp.root);
	const fs::path primary = FollowupRunPath(temp.root), backup = uam::io::MakeBackupPath(primary);
	const std::string v2_bytes = value.dump(2), damaged = "unparseable legacy corruption";
	UAM_ASSERT(uam::io::WriteTextFile(backup, v2_bytes));
	UAM_ASSERT(uam::io::WriteTextFile(primary, damaged));
	const auto no_backup_adoption = uam::AgentRunLedger::LoadAll(temp.root);
	UAM_ASSERT_EQ(no_backup_adoption.runs.size(), std::size_t{1});
	UAM_ASSERT_EQ(no_backup_adoption.runs.front().id, std::string(kFollowupParentRunId));
	UAM_ASSERT_EQ(ReadFile(primary), damaged);
	UAM_ASSERT_EQ(ReadFile(backup), v2_bytes);
	// The v2 ancestor is excluded before closure, so its v1 child cannot become a live orphan.
	UAM_ASSERT(uam::AgentRunLedger::Save(temp.root, FollowupLegacyRun()));
	value["id"] = kFollowupParentRunId;
	value["parentRunId"] = "";
	value["transcriptChatId"] = "managed-followup-parent-transcript";
	value["depth"] = 1;
	value["resultDeliveryId"] = kFollowupParentRunId;
	value["managedContinuation"]["turns"][0]["turnId"] = kFollowupParentRunId;
	value["managedContinuation"]["turns"][0]["deliveryId"] = kFollowupParentRunId;
	value["managedContinuation"]["turns"][1]["afterTurnId"] = kFollowupParentRunId;
	const fs::path parent = FollowupRunPath(temp.root, kFollowupParentRunId);
	const std::string parent_bytes = value.dump(2), child_bytes = ReadFile(primary);
	UAM_ASSERT(uam::io::WriteTextFile(parent, parent_bytes));
	const auto inspected = uam::AgentRunLedger::LoadAllReadOnly(temp.root);
	UAM_ASSERT_EQ(inspected.runs.size(), std::size_t{2});
	UAM_ASSERT(inspected.errors.empty());
	const auto startup = uam::AgentRunLedger::LoadAll(temp.root);
	UAM_ASSERT(startup.runs.empty());
	UAM_ASSERT(!startup.errors.empty());
	UAM_ASSERT_EQ(ReadFile(parent), parent_bytes);
	UAM_ASSERT_EQ(ReadFile(primary), child_bytes);
}

UAM_TEST(ManagedFollowupLegacyInterruptionRefusesV2WithoutMutation)
{
	TempDir temp("uam-managed-followup-refused-interruption");
	const auto expected = FollowupV2Fixture(temp.root);
	const fs::path primary = FollowupRunPath(temp.root);
	const std::string bytes = expected.dump(2);
	UAM_ASSERT(uam::io::WriteTextFile(primary, bytes));
	auto inspected = uam::AgentRunLedger::LoadAllReadOnly(temp.root);
	UAM_ASSERT_EQ(inspected.runs.size(), std::size_t{2});
	const auto target = std::ranges::find(inspected.runs, std::string(kFollowupRunId), &AgentRun::id);
	UAM_ASSERT(target != inspected.runs.end());
	std::vector<AgentRun> only_v2{*target};
	std::vector<std::string> errors;
	UAM_ASSERT(!uam::AgentRunLedger::MarkNonterminalInterrupted(temp.root, &only_v2, "manager_restarted", &errors));
	UAM_ASSERT(!errors.empty());
	UAM_ASSERT_EQ(only_v2.front().status, target->status);
	UAM_ASSERT(only_v2.front().managed_continuation == target->managed_continuation);
	UAM_ASSERT_EQ(ReadFile(primary), bytes);
	UAM_ASSERT(!fs::exists(uam::io::MakeBackupPath(primary)));
	const fs::path blocked = temp.root / "not-a-directory";
	UAM_ASSERT(uam::io::WriteTextFile(blocked, "preserve blocker"));
	UAM_ASSERT(!uam::AgentRunLedger::Save(blocked, only_v2.front()));
	UAM_ASSERT_EQ(ReadFile(blocked), std::string("preserve blocker"));
	UAM_ASSERT_EQ(ReadFile(primary), bytes);
}

namespace
{
	ChatSession ManagedFollowupCodecChat()
	{
		ChatSession chat;
		chat.id = "managed-followup-codec-chat";
		chat.provider_id = "codex-cli";
		chat.native_session_id = kFollowupNativeSession;
		chat.created_at = chat.updated_at = "2026-10-17 10:00:00";
		chat.managed_followup_incarnation = kFollowupTranscriptBirth;
		uam::AcpQueuedUserPromptState prompt;
		prompt.id = kFollowupSecondTurn;
		prompt.text = "Preserve this inert managed input.";
		prompt.managed_turn = uam::ManagedRunPromptIdentity{
		    kFollowupRunId, kFollowupSecondTurn, 2, kFollowupNativeSession, uam::ManagedRunContinuity::Native};
		chat.acp_queued_prompts.push_back(prompt);
		return chat;
	}

	void AssertManagedInputCannotSave(const fs::path& root, const ChatSession& loaded)
	{
		UAM_ASSERT(loaded.managed_followup_input_unavailable && loaded.acp_queue_held && loaded.acp_queue_storage_error);
		const std::string fingerprint = ChatRepository::ChatMetadataFingerprint(root, loaded);
		UAM_ASSERT(!fingerprint.empty());
		UAM_ASSERT(nlohmann::json::parse(fingerprint).at("validationManagedFollowupInputUnavailable").get<bool>());
		UAM_ASSERT(!ChatRepository::SaveChat(root, loaded));
		UAM_ASSERT(!ChatRepository::PrepareChatSave(root, loaded));
		ChatSession copy = loaded;
		UAM_ASSERT(!ChatRepository::SaveQueuedPromptOutbox(root, copy));
	}
}

UAM_TEST(ManagedFollowupQueueCodecHoldsValidAndMalformedPrimaryBytes)
{
	TempDir temp("uam-managed-followup-queue-codec");
	const ChatSession chat = ManagedFollowupCodecChat();
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id), backup = uam::io::MakeBackupPath(primary);
	const std::string valid_bytes = ReadFile(primary), backup_bytes = ReadFile(backup);
	const auto archived = ChatRepository::ParseArchivalQueuedPrompts(valid_bytes, chat.id);
	UAM_ASSERT(archived && archived->size() == 1);
	UAM_ASSERT(archived->front().managed_turn == chat.acp_queued_prompts.front().managed_turn);
	auto loaded = ChatRepository::LoadLocalChatBounded(temp.root, chat.id, 1024U * 1024U);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT_EQ(loaded->managed_followup_incarnation, std::string(kFollowupTranscriptBirth));
	UAM_ASSERT_EQ(loaded->acp_queued_prompts.size(), std::size_t{1});
	UAM_ASSERT(loaded->acp_queued_prompts.front().managed_turn == chat.acp_queued_prompts.front().managed_turn);
	AssertManagedInputCannotSave(temp.root, *loaded);
	UAM_ASSERT_EQ(ReadFile(primary), valid_bytes);
	UAM_ASSERT_EQ(ReadFile(backup), backup_bytes);
	auto summary = ChatRepository::LoadLocalChat(temp.root, chat.id, false);
	UAM_ASSERT(summary.has_value());
	UAM_ASSERT(summary->managed_followup_input_unavailable);
	UAM_ASSERT(ChatRepository::HydrateChatMessages(temp.root, *summary));
	AssertManagedInputCannotSave(temp.root, *summary);
	for (const auto& malformed : {nlohmann::json(nullptr), nlohmann::json(false), nlohmann::json{{"turn_id", "invalid"}}})
	{
		auto value = nlohmann::json::parse(valid_bytes);
		value["acp_queued_prompts"][0]["managed_turn"] = malformed;
		const std::string bytes = value.dump(2);
		UAM_ASSERT(uam::io::WriteTextFile(primary, bytes));
		loaded = ChatRepository::LoadLocalChatBounded(temp.root, chat.id, 1024U * 1024U);
		UAM_ASSERT(loaded.has_value());
		AssertManagedInputCannotSave(temp.root, *loaded);
		UAM_ASSERT_EQ(ReadFile(primary), bytes);
		UAM_ASSERT_EQ(ReadFile(backup), backup_bytes);
	}
}

UAM_TEST(ManagedFollowupQueueOutboxOverlayCannotClearItsInertBlocker)
{
	TempDir temp("uam-managed-followup-queue-outbox");
	ChatSession chat = ManagedFollowupCodecChat();
	const auto prompts = chat.acp_queued_prompts;
	chat.acp_queued_prompts.clear();
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	chat.acp_queued_prompts = prompts;
	UAM_ASSERT(ChatRepository::SaveQueuedPromptOutbox(temp.root, chat));
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id), backup = uam::io::MakeBackupPath(primary);
	const fs::path outbox = temp.root / "queue-outboxes" / (chat.id + ".json");
	const std::string bytes = ReadFile(primary), backup_bytes = ReadFile(backup), outbox_bytes = ReadFile(outbox);
	auto loaded = ChatRepository::LoadLocalChatBounded(temp.root, chat.id, 1024U * 1024U);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT_EQ(loaded->acp_queued_prompts.size(), std::size_t{1});
	AssertManagedInputCannotSave(temp.root, *loaded);
	UAM_ASSERT_EQ(ReadFile(primary), bytes);
	UAM_ASSERT_EQ(ReadFile(backup), backup_bytes);
	UAM_ASSERT_EQ(ReadFile(outbox), outbox_bytes);
}

UAM_TEST(ManagedFollowupServiceRemainsUnavailableWithoutMutatingLoadedData)
{
	TempDir temp("uam-managed-followup-disabled-service");
	const auto expected = FollowupV2Fixture(temp.root);
	const fs::path primary = FollowupRunPath(temp.root);
	const std::string bytes = expected.dump(2);
	UAM_ASSERT(uam::io::WriteTextFile(primary, bytes));
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession root;
	root.id = "managed-followup-root";
	root.managed_followup_incarnation = kFollowupRootBirth;
	app.chats.push_back(root);
	app.agent_runs = uam::AgentRunLedger::LoadAllReadOnly(temp.root).runs;
	UAM_ASSERT_EQ(app.agent_runs.size(), std::size_t{2});
	const auto state = uam::ManagedSubagentFollowupService::GetState(app, root.id, kFollowupRunId);
	// Inert depth-two codec data is not a genuine same-child native continuation witness.
	UAM_ASSERT(!state.can_send && state.code == uam::ManagedFollowupCode::NativeResumeUnavailable);
	const uam::ManagedFollowupRequest request{.after_turn_id = kFollowupSecondTurn, .client_request_id = kFollowupClientRequest, .text = "Do not admit this."};
	const auto result = uam::ManagedSubagentFollowupService::Submit(app, root.id, kFollowupRunId, request);
	UAM_ASSERT(!result.accepted && !result.replayed && !result.acceptance && result.code == uam::ManagedFollowupCode::NotImplemented);
	UAM_ASSERT(uam::ManagedSubagentFollowupService::GetState(app, root.id, kFollowupParentRunId).code == uam::ManagedFollowupCode::LegacyIdentityUnverified);
	const auto foreign = uam::ManagedSubagentFollowupService::GetState(app, "unrelated-root", kFollowupRunId);
	UAM_ASSERT(!foreign.can_send && foreign.code == uam::ManagedFollowupCode::ScopeChanged && foreign.current_turn_id.empty());
	app.chats.front().managed_followup_incarnation = kFollowupTranscriptBirth;
	const auto replaced = uam::ManagedSubagentFollowupService::GetState(app, root.id, kFollowupRunId);
	UAM_ASSERT(!replaced.can_send && replaced.code == uam::ManagedFollowupCode::ScopeChanged && replaced.current_turn_id.empty());
	UAM_ASSERT_EQ(app.agent_runs.size(), std::size_t{2});
	UAM_ASSERT_EQ(app.chats.size(), std::size_t{1});
	UAM_ASSERT(app.queued_agent_run_ids.empty() && app.acp_sessions.empty());
	UAM_ASSERT_EQ(ReadFile(primary), bytes);
}


UAM_TEST(ManagedFollowupSummaryFallbackPreservesManagedPrimaryAndRecoversLegacyCorruption)
{
	for (const bool invalid_summary : {false, true})
	{
		for (const bool managed : {false, true})
		{
			TempDir temp("uam-managed-followup-summary-fallback");
			ChatSession legacy;
			legacy.id = "managed-followup-summary-recovery";
			legacy.title = "Summary fallback recovery authority";
			legacy.provider_id = "codex-cli";
			legacy.created_at = legacy.updated_at = "2026-10-17 10:00:00";
			uam::AcpQueuedUserPromptState ordinary;
			ordinary.id = kFollowupSecondTurn;
			ordinary.text = "A legacy recovery fixture mentions managed_turn as ordinary text.";
			legacy.acp_queued_prompts.push_back(ordinary);
			Message authored;
			authored.role = MessageRole::User;
			authored.content = "Preserve genuine legacy backup content.";
			authored.created_at = legacy.created_at;
			legacy.messages.push_back(authored);
			UAM_ASSERT(ChatRepository::SaveChat(temp.root, legacy));
			UAM_ASSERT(ChatRepository::SaveChat(temp.root, legacy));
			const fs::path primary = AppPaths::UamChatFilePath(temp.root, legacy.id);
			const fs::path backup = uam::io::MakeBackupPath(primary);
			const fs::path summary = AppPaths::UamChatSummaryFilePath(temp.root, legacy.id);
			const std::string backup_bytes = ReadFile(backup);
			auto value = nlohmann::json::parse(ReadFile(primary));
			value["acp_queued_prompts"][0]["managed_turn"] = {
			    {"run_id", kFollowupRunId}, {"turn_id", kFollowupSecondTurn}, {"turn_number", 2},
			    {"expected_native_session_id", kFollowupNativeSession}, {"continuity", "native"}};
			value["messages"] = nlohmann::json::object();
			// Unparseable legacy corruption containing a mere marker fragment must still recover.
			const std::string bytes = managed ? value.dump(2) : "broken legacy JSON: \"managed_turn\": null";
			UAM_ASSERT(uam::io::WriteTextFile(primary, bytes));
			if (invalid_summary) UAM_ASSERT(uam::io::WriteTextFile(summary, "{}"));
			else UAM_ASSERT(fs::remove(summary));
			std::string warning;
			const auto loaded = ChatRepository::LoadLocalChatSummaries(temp.root, &warning);
			if (managed)
			{
				UAM_ASSERT(loaded.empty());
				UAM_ASSERT(!warning.empty());
				UAM_ASSERT_EQ(ReadFile(primary), bytes);
				UAM_ASSERT_EQ(ReadFile(backup), backup_bytes);
			}
			else
			{
				UAM_ASSERT_EQ(loaded.size(), std::size_t{1});
				UAM_ASSERT_EQ(loaded.front().id, legacy.id);
				UAM_ASSERT(!loaded.front().managed_followup_input_unavailable);
				UAM_ASSERT(!loaded.front().acp_queue_storage_error);
				UAM_ASSERT_EQ(loaded.front().acp_queued_prompts.size(), std::size_t{1});
				UAM_ASSERT_EQ(loaded.front().acp_queued_prompts.front().text, ordinary.text);
				const auto recovered = nlohmann::json::parse(ReadFile(primary));
				UAM_ASSERT(recovered.at("messages").is_array());
				UAM_ASSERT_EQ(recovered.at("messages").size(), std::size_t{1});
				UAM_ASSERT_EQ(recovered.at("messages")[0].at("content").get<std::string>(), authored.content);
				UAM_ASSERT(!recovered.at("acp_queued_prompts")[0].contains("managed_turn"));
			}
		}
	}
}

UAM_TEST(ManagedFollowupConsumedTypedJournalCannotBlockCurrentOrReplacementBirth)
{
	for (const bool replace_birth : {false, true})
	{
		TempDir temp("uam-managed-followup-consumed-typed-journal");
		ChatSession chat = ManagedFollowupCodecChat();
		const auto prompts = chat.acp_queued_prompts;
		chat.acp_queued_prompts.clear();
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
		chat.acp_queued_prompts = prompts;
		UAM_ASSERT(ChatRepository::SaveQueuedPromptOutbox(temp.root, chat));
		const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id);
		const fs::path outbox = temp.root / "queue-outboxes" / (chat.id + ".json");
		const std::string journal_bytes = ReadFile(outbox);
		const auto journal = nlohmann::json::parse(journal_bytes);
		const std::string consumed_revision = chat.acp_queue_storage_revision;
		UAM_ASSERT_EQ(journal.at("revision"), nlohmann::json(consumed_revision));
		chat.acp_queued_prompts.clear();
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
		if (replace_birth)
		{
			// Reader fixture: install a different complete primary after consuming
			// the journal. Ordinary saves cannot replace an already bound birth.
			auto replacement = nlohmann::json::parse(ReadFile(primary));
			replacement["managed_followup_incarnation"] = kFollowupRootBirth;
			UAM_ASSERT(uam::io::WriteTextFile(primary, replacement.dump(2)));
			chat.managed_followup_incarnation = kFollowupRootBirth;
		}
		const std::string canonical_bytes = ReadFile(primary);
		const auto canonical = nlohmann::json::parse(canonical_bytes);
		UAM_ASSERT(canonical.at("acp_queued_prompts").empty());
		UAM_ASSERT_EQ(canonical.at("acp_queue_storage_revision"), nlohmann::json(consumed_revision));
		if (replace_birth)
			UAM_ASSERT(journal.at("owner").get<std::string>() != ChatRepository::QueueStorageOwner(chat));
		auto loaded = ChatRepository::LoadLocalChatBounded(temp.root, chat.id, 1024U * 1024U);
		UAM_ASSERT(loaded.has_value());
		UAM_ASSERT(!loaded->managed_followup_input_unavailable && !loaded->acp_queue_storage_error && !loaded->acp_queue_held);
		UAM_ASSERT(loaded->acp_queued_prompts.empty());
		UAM_ASSERT_EQ(loaded->managed_followup_incarnation, chat.managed_followup_incarnation);
		UAM_ASSERT_EQ(ReadFile(primary), canonical_bytes);
		UAM_ASSERT_EQ(ReadFile(outbox), journal_bytes);
		const auto prepared = ChatRepository::PrepareChatSave(temp.root, *loaded);
		UAM_ASSERT(prepared != nullptr);
		UAM_ASSERT_EQ(ReadFile(primary), canonical_bytes);
		UAM_ASSERT(ChatRepository::SaveChat(temp.root, *loaded));
		UAM_ASSERT_EQ(ReadFile(outbox), journal_bytes);
		UAM_ASSERT(nlohmann::json::parse(ReadFile(primary)).at("acp_queued_prompts").empty());
	}
}

UAM_TEST(ManagedFollowupOwnedMalformedJournalRetainsUnavailableMarker)
{
	TempDir temp("uam-managed-followup-owned-malformed-journal");
	ChatSession chat = ManagedFollowupCodecChat();
	const auto prompts = chat.acp_queued_prompts;
	chat.acp_queued_prompts.clear();
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	chat.acp_queued_prompts = prompts;
	UAM_ASSERT(ChatRepository::SaveQueuedPromptOutbox(temp.root, chat));
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id), backup = uam::io::MakeBackupPath(primary);
	const fs::path outbox = temp.root / "queue-outboxes" / (chat.id + ".json");
	const std::string primary_bytes = ReadFile(primary), backup_bytes = ReadFile(backup);
	auto journal = nlohmann::json::parse(ReadFile(outbox));
	journal["acp_queued_prompts"][0]["managed_turn"] = nullptr;
	const std::string journal_bytes = journal.dump(2);
	UAM_ASSERT(uam::io::WriteTextFile(outbox, journal_bytes));
	auto loaded = ChatRepository::LoadLocalChatBounded(temp.root, chat.id, 1024U * 1024U);
	UAM_ASSERT(loaded.has_value());
	AssertManagedInputCannotSave(temp.root, *loaded);
	UAM_ASSERT_EQ(ReadFile(primary), primary_bytes);
	UAM_ASSERT_EQ(ReadFile(backup), backup_bytes);
	UAM_ASSERT_EQ(ReadFile(outbox), journal_bytes);
}


UAM_TEST(ManagedFollowupForeignNewerJournalDoesNotImportManagedAuthority)
{
	TempDir temp("uam-managed-followup-foreign-journal");
	ChatSession chat = ManagedFollowupCodecChat();
	const auto prompts = chat.acp_queued_prompts;
	chat.acp_queued_prompts.clear();
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, chat));
	chat.acp_queued_prompts = prompts;
	UAM_ASSERT(ChatRepository::SaveQueuedPromptOutbox(temp.root, chat));
	const fs::path primary = AppPaths::UamChatFilePath(temp.root, chat.id);
	const fs::path outbox = temp.root / "queue-outboxes" / (chat.id + ".json");
	const std::string primary_bytes = ReadFile(primary);
	auto journal = nlohmann::json::parse(ReadFile(outbox));
	ChatSession foreign = chat;
	foreign.managed_followup_incarnation = kFollowupRootBirth;
	journal["owner"] = ChatRepository::QueueStorageOwner(foreign);
	const std::string journal_bytes = journal.dump(2);
	UAM_ASSERT(uam::io::WriteTextFile(outbox, journal_bytes));
	auto loaded = ChatRepository::LoadLocalChatBounded(temp.root, chat.id, 1024U * 1024U);
	UAM_ASSERT(loaded.has_value());
	// Preserve the existing mismatched-journal hold without importing permanent authority.
	UAM_ASSERT(loaded->acp_queue_held && loaded->acp_queue_storage_error);
	UAM_ASSERT(!loaded->managed_followup_input_unavailable);
	UAM_ASSERT(loaded->acp_queued_prompts.empty());
	UAM_ASSERT_EQ(ReadFile(primary), primary_bytes);
	UAM_ASSERT_EQ(ReadFile(outbox), journal_bytes);
	UAM_ASSERT(fs::remove(outbox));
	loaded = ChatRepository::LoadLocalChatBounded(temp.root, chat.id, 1024U * 1024U);
	UAM_ASSERT(loaded.has_value());
	UAM_ASSERT(!loaded->managed_followup_input_unavailable && !loaded->acp_queue_storage_error && !loaded->acp_queue_held);
	UAM_ASSERT(ChatRepository::PrepareChatSave(temp.root, *loaded) != nullptr);
	UAM_ASSERT(ChatRepository::SaveChat(temp.root, *loaded));
}


namespace
{
	AgentRun ReadValidatedManagedWriterTarget(const fs::path& root, bool managed)
	{
		const auto loaded = uam::AgentRunLedger::LoadAllReadOnly(root);
		UAM_ASSERT(loaded.errors.empty());
		UAM_ASSERT_EQ(loaded.runs.size(), std::size_t{2});
		const auto target = std::ranges::find(loaded.runs, std::string(kFollowupRunId), &AgentRun::id);
		UAM_ASSERT(target != loaded.runs.end());
		UAM_ASSERT_EQ(target->root_chat_id, std::string("managed-followup-root"));
		UAM_ASSERT_EQ(target->parent_run_id, std::string(kFollowupParentRunId));
		UAM_ASSERT_EQ(target->transcript_chat_id, std::string("managed-followup-transcript"));
		UAM_ASSERT_EQ(target->depth, 2);
		UAM_ASSERT_EQ(target->managed_continuation.has_value(), managed);
		return *target;
	}

	AgentRun ValidatedManagedWriterPredecessor(const fs::path& private_root)
	{
		const auto value = FollowupV2Fixture(private_root);
		UAM_ASSERT(uam::io::WriteTextFile(FollowupRunPath(private_root), value.dump(2)));
		AgentRun predecessor = ReadValidatedManagedWriterTarget(private_root, true);
		UAM_ASSERT_EQ(predecessor.managed_continuation->state_revision, std::uint64_t{2});
		UAM_ASSERT_EQ(predecessor.managed_continuation->turns.size(), std::size_t{2});
		UAM_ASSERT_EQ(predecessor.status, std::string("queued"));
		return predecessor;
	}

	AgentRun CompletedManagedWriterSuccessor(const AgentRun& predecessor)
	{
		AgentRun newer = predecessor;
		auto& continuation = *newer.managed_continuation;
		++continuation.state_revision;
		auto& current = continuation.turns.back();
		current.status = uam::ManagedRunTurnStatus::Completed;
		current.dispatch_attempted = true;
		current.expected_turn_serial = 2;
		current.first_user_message_index = 1;
		current.user_message_created_at = "2026-10-17 10:00:02";
		current.started_at_epoch_ms = 2000;
		current.finished_at_epoch_ms = 2100;
		current.result_delivered = true;
		current.result_delivery_attempts = 1;
		newer.status = "completed";
		newer.expected_turn_serial = current.expected_turn_serial;
		newer.started_at_epoch_ms = current.started_at_epoch_ms;
		newer.finished_at_epoch_ms = current.finished_at_epoch_ms;
		newer.deadline_at_epoch_ms = 1802000;
		newer.started_at = "2026-10-17 10:00:02";
		newer.finished_at = newer.updated_at = "2026-10-17 10:00:03";
		return newer;
	}

	void SaveInitialManagedWriterPrimary(const fs::path& root, const AgentRun& newer,
	    const std::string& validated_backup_bytes)
	{
		UAM_ASSERT(uam::AgentRunLedger::Save(root, FollowupLegacyParentRun()));
		const fs::path primary = FollowupRunPath(root), backup = uam::io::MakeBackupPath(primary);
		UAM_ASSERT(!fs::exists(primary) && !fs::exists(backup));
		// This is an ordinary initial Save into an absent fixture path, not conditional creation.
		UAM_ASSERT(uam::AgentRunLedger::Save(root, newer));
		const AgentRun saved = ReadValidatedManagedWriterTarget(root, true);
		UAM_ASSERT(saved.managed_continuation == newer.managed_continuation);
		UAM_ASSERT_EQ(saved.status, newer.status);
		UAM_ASSERT_EQ(saved.expected_turn_serial, newer.expected_turn_serial);
		UAM_ASSERT_EQ(saved.finished_at_epoch_ms, newer.finished_at_epoch_ms);
		UAM_ASSERT_EQ(nlohmann::json::parse(ReadFile(primary)).at("version"), nlohmann::json(2));
		// Give rejection a nonempty, independently validated recovery witness to preserve.
		UAM_ASSERT(uam::io::WriteTextFile(backup, validated_backup_bytes));
	}
}

UAM_TEST(ManagedFollowupUncheckedSaveCannotRewindNewerV2Primary)
{
	TempDir private_predecessor("uam-managed-ledger-private-predecessor");
	const AgentRun older = ValidatedManagedWriterPredecessor(private_predecessor.root);
	const AgentRun newer = CompletedManagedWriterSuccessor(older);
	UAM_ASSERT_EQ(newer.id, older.id);
	UAM_ASSERT_EQ(newer.root_chat_id, older.root_chat_id);
	UAM_ASSERT_EQ(newer.parent_run_id, older.parent_run_id);
	UAM_ASSERT_EQ(newer.transcript_chat_id, older.transcript_chat_id);
	UAM_ASSERT_EQ(newer.depth, older.depth);
	UAM_ASSERT(newer.managed_continuation->owner == older.managed_continuation->owner);
	UAM_ASSERT(newer.managed_continuation->execution == older.managed_continuation->execution);
	UAM_ASSERT_EQ(newer.managed_continuation->current_turn_id, older.managed_continuation->current_turn_id);
	UAM_ASSERT_EQ(newer.managed_continuation->state_revision, older.managed_continuation->state_revision + 1);
	TempDir target("uam-managed-ledger-stale-unchecked-save");
	SaveInitialManagedWriterPrimary(target.root, newer, ReadFile(FollowupRunPath(private_predecessor.root)));
	const fs::path primary = FollowupRunPath(target.root), backup = uam::io::MakeBackupPath(primary);
	const fs::path parent = FollowupRunPath(target.root, kFollowupParentRunId);
	const std::string primary_bytes = ReadFile(primary), backup_bytes = ReadFile(backup), parent_bytes = ReadFile(parent);
	UAM_ASSERT_EQ(nlohmann::json::parse(backup_bytes).at("managedContinuation").at("stateRevision"), nlohmann::json(2));
	std::string error;
	UAM_ASSERT(!uam::AgentRunLedger::Save(target.root, older, &error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT_EQ(ReadFile(primary), primary_bytes);
	UAM_ASSERT_EQ(ReadFile(backup), backup_bytes);
	UAM_ASSERT_EQ(ReadFile(parent), parent_bytes);
	const AgentRun still_newer = ReadValidatedManagedWriterTarget(target.root, true);
	UAM_ASSERT(still_newer.managed_continuation == newer.managed_continuation);
}

UAM_TEST(ManagedFollowupUncheckedLegacySaveCannotDowngradeV2Primary)
{
	TempDir private_managed("uam-managed-ledger-private-managed");
	const AgentRun managed = CompletedManagedWriterSuccessor(ValidatedManagedWriterPredecessor(private_managed.root));
	TempDir private_legacy("uam-managed-ledger-private-legacy");
	UAM_ASSERT(uam::AgentRunLedger::Save(private_legacy.root, FollowupLegacyParentRun()));
	UAM_ASSERT(uam::AgentRunLedger::Save(private_legacy.root, FollowupLegacyRun()));
	const AgentRun legacy = ReadValidatedManagedWriterTarget(private_legacy.root, false);
	UAM_ASSERT_EQ(legacy.id, managed.id);
	UAM_ASSERT_EQ(legacy.root_chat_id, managed.root_chat_id);
	UAM_ASSERT_EQ(legacy.parent_run_id, managed.parent_run_id);
	UAM_ASSERT_EQ(legacy.transcript_chat_id, managed.transcript_chat_id);
	UAM_ASSERT_EQ(legacy.depth, managed.depth);
	UAM_ASSERT_EQ(legacy.provider_id, managed.provider_id);
	UAM_ASSERT_EQ(legacy.model_id, managed.model_id);
	TempDir target("uam-managed-ledger-unchecked-downgrade");
	SaveInitialManagedWriterPrimary(target.root, managed, ReadFile(FollowupRunPath(private_legacy.root)));
	const fs::path primary = FollowupRunPath(target.root), backup = uam::io::MakeBackupPath(primary);
	const std::string primary_bytes = ReadFile(primary), backup_bytes = ReadFile(backup);
	UAM_ASSERT_EQ(nlohmann::json::parse(backup_bytes).at("version"), nlohmann::json(1));
	std::string error;
	UAM_ASSERT(!uam::AgentRunLedger::Save(target.root, legacy, &error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT_EQ(ReadFile(primary), primary_bytes);
	UAM_ASSERT_EQ(ReadFile(backup), backup_bytes);
	UAM_ASSERT(ReadValidatedManagedWriterTarget(target.root, true).managed_continuation == managed.managed_continuation);
}

UAM_TEST(ManagedFollowupUncheckedLegacySaveCannotReplaceRecognizableInvalidV2Primary)
{
	// Genuine legacy corruption remains recoverable; mere marker text is not a structured v2 record.
	for (const bool structured_corruption : {false, true})
	{
		TempDir legacy_root("uam-managed-ledger-unmarked-recovery-control");
		UAM_ASSERT(uam::AgentRunLedger::Save(legacy_root.root, FollowupLegacyParentRun()));
		UAM_ASSERT(uam::AgentRunLedger::Save(legacy_root.root, FollowupLegacyRun()));
		UAM_ASSERT(uam::AgentRunLedger::Save(legacy_root.root, FollowupLegacyRun()));
		const fs::path primary = FollowupRunPath(legacy_root.root), backup = uam::io::MakeBackupPath(primary);
		const std::string backup_bytes = ReadFile(backup);
		auto bad_legacy = nlohmann::json::parse(ReadFile(primary));
		bad_legacy["definitionSnapshot"] = false;
		const std::string damaged = structured_corruption ? bad_legacy.dump(2) : "broken legacy JSON mentions managedContinuation";
		UAM_ASSERT(uam::io::WriteTextFile(primary, damaged));
		const auto invalid = uam::AgentRunLedger::LoadAllReadOnly(legacy_root.root);
		UAM_ASSERT_EQ(invalid.runs.size(), std::size_t{1});
		UAM_ASSERT(!invalid.errors.empty());
		const auto recovered = uam::AgentRunLedger::LoadAll(legacy_root.root);
		UAM_ASSERT_EQ(recovered.runs.size(), std::size_t{2});
		UAM_ASSERT(recovered.errors.empty());
		UAM_ASSERT_EQ(ReadFile(primary), backup_bytes);
		UAM_ASSERT_EQ(ReadFile(backup), backup_bytes);
		const AgentRun legacy = ReadValidatedManagedWriterTarget(legacy_root.root, false);
		UAM_ASSERT(uam::AgentRunLedger::Save(legacy_root.root, legacy));
	}
	TempDir private_managed("uam-managed-ledger-private-invalid-managed");
	const AgentRun managed = CompletedManagedWriterSuccessor(ValidatedManagedWriterPredecessor(private_managed.root));
	TempDir private_legacy("uam-managed-ledger-private-invalid-legacy");
	UAM_ASSERT(uam::AgentRunLedger::Save(private_legacy.root, FollowupLegacyParentRun()));
	UAM_ASSERT(uam::AgentRunLedger::Save(private_legacy.root, FollowupLegacyRun()));
	const AgentRun legacy = ReadValidatedManagedWriterTarget(private_legacy.root, false);
	UAM_ASSERT_EQ(legacy.id, managed.id);
	UAM_ASSERT_EQ(legacy.root_chat_id, managed.root_chat_id);
	UAM_ASSERT_EQ(legacy.transcript_chat_id, managed.transcript_chat_id);
	TempDir target("uam-managed-ledger-recognizable-invalid-primary");
	SaveInitialManagedWriterPrimary(target.root, managed, ReadFile(FollowupRunPath(private_legacy.root)));
	const fs::path primary = FollowupRunPath(target.root), backup = uam::io::MakeBackupPath(primary);
	const std::string backup_bytes = ReadFile(backup);
	auto invalid_managed = nlohmann::json::parse(ReadFile(primary));
	// Break an unrelated required top-level field while retaining complete identity and v2 evidence.
	invalid_managed["definitionSnapshot"] = false;
	UAM_ASSERT_EQ(invalid_managed.at("version"), nlohmann::json(2));
	UAM_ASSERT(invalid_managed.at("managedContinuation").is_object());
	UAM_ASSERT_EQ(invalid_managed.at("id"), nlohmann::json(legacy.id));
	const std::string damaged = invalid_managed.dump(2);
	UAM_ASSERT(uam::io::WriteTextFile(primary, damaged));
	for (const bool startup : {false, true})
	{
		const auto invalid = startup ? uam::AgentRunLedger::LoadAll(target.root) : uam::AgentRunLedger::LoadAllReadOnly(target.root);
		UAM_ASSERT_EQ(invalid.runs.size(), std::size_t{1});
		UAM_ASSERT_EQ(invalid.runs.front().id, std::string(kFollowupParentRunId));
		UAM_ASSERT(!invalid.errors.empty());
		UAM_ASSERT_EQ(ReadFile(primary), damaged);
		UAM_ASSERT_EQ(ReadFile(backup), backup_bytes);
	}
	std::string error;
	UAM_ASSERT(!uam::AgentRunLedger::Save(target.root, legacy, &error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT_EQ(ReadFile(primary), damaged);
	UAM_ASSERT_EQ(ReadFile(backup), backup_bytes);
}


#include <cerrno>
#include <chrono>
#include <thread>
#if !defined(_WIN32)
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

UAM_TEST(ManagedFollowupUncheckedSaveRejectsNonregularPrimaryBeforeReading)
{
	TempDir temp("uam-managed-ledger-nonregular-primary");
	const AgentRun legacy = FollowupLegacyParentRun();
	UAM_ASSERT(uam::AgentRunLedger::Save(temp.root, legacy));
	UAM_ASSERT(uam::AgentRunLedger::Save(temp.root, legacy));
	const fs::path primary = FollowupRunPath(temp.root, legacy.id);
	const fs::path backup = uam::io::MakeBackupPath(primary);
	const std::string backup_bytes = ReadFile(backup);
	const auto valid = uam::AgentRunLedger::LoadAllReadOnly(temp.root);
	UAM_ASSERT(valid.errors.empty() && valid.runs.size() == 1);
	UAM_ASSERT_EQ(valid.runs.front().id, legacy.id);
	UAM_ASSERT(!valid.runs.front().managed_continuation);
	UAM_ASSERT(fs::remove(primary));
#if !defined(_WIN32)
	UAM_ASSERT_EQ(::mkfifo(primary.c_str(), 0600), 0);
	UAM_ASSERT(fs::is_fifo(primary));
	const pid_t child = ::fork();
	UAM_ASSERT(child >= 0);
	if (child == 0)
	{
		// Private worker only; it has no provider, application state or process launcher.
		try
		{
			std::string error;
			const bool saved = uam::AgentRunLedger::Save(temp.root, legacy, &error);
			::_exit(!saved && !error.empty() ? 0 : 2);
		}
		catch (...)
		{
			::_exit(3);
		}
	}
	struct PrivateWorkerCleanup
	{
		pid_t child;
		int writer = -1;
		void CloseWriter()
		{
			if (writer >= 0) { (void)::close(writer); writer = -1; }
		}
		void StopAndReap()
		{
			CloseWriter();
			if (child <= 0) return;
			(void)::kill(child, SIGKILL);
			while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
			child = -1;
		}
		~PrivateWorkerCleanup() { StopAndReap(); }
	} cleanup{child};
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	int status = 0;
	int probe_error = 0;
	bool completed_normally = false;
	bool paired_before_release = false;
	while (std::chrono::steady_clock::now() < deadline)
	{
		const pid_t observed = ::waitpid(child, &status, WNOHANG);
		if (observed == child)
		{
			completed_normally = true;
			cleanup.child = -1;
			break;
		}
		if (observed < 0 && errno != EINTR)
		{
			probe_error = errno;
			if (errno == ECHILD) cleanup.child = -1;
			break;
		}
		cleanup.writer = ::open(primary.c_str(), O_WRONLY | O_NONBLOCK);
		if (cleanup.writer >= 0)
		{
			// Only the Save worker can be this FIFO's reader. No bytes have been supplied.
			paired_before_release = true;
			break;
		}
		if (errno != ENXIO && errno != EINTR) { probe_error = errno; break; }
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	bool released = !paired_before_release;
	if (paired_before_release)
	{
		// Release a buggy reader with actual structured managed evidence, then EOF.
		// Its eventual false result must not hide that it depended on FIFO input.
		constexpr char marker[] = "{\"version\":2,\"managedContinuation\":null}";
		// A failed child must not turn a release-write EPIPE into native-harness termination.
		sigset_t blocked, previous;
		sigemptyset(&blocked);
		sigaddset(&blocked, SIGPIPE);
		const int mask_error = ::pthread_sigmask(SIG_BLOCK, &blocked, &previous);
		if (mask_error != 0) probe_error = mask_error;
		else
		{
			ssize_t written;
			do { written = ::write(cleanup.writer, marker, sizeof(marker) - 1); } while (written < 0 && errno == EINTR);
			released = written == static_cast<ssize_t>(sizeof(marker) - 1);
			if (written < 0 && errno == EPIPE)
			{
				sigset_t pending;
				if (::sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1)
				{
					int consumed = 0;
					(void)::sigwait(&blocked, &consumed);
				}
			}
			const int restore_error = ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
			if (restore_error != 0) probe_error = restore_error;
		}
		cleanup.CloseWriter();
	}
	while (!completed_normally && probe_error == 0 && std::chrono::steady_clock::now() < deadline)
	{
		const pid_t observed = ::waitpid(child, &status, WNOHANG);
		if (observed == child)
		{
			completed_normally = true;
			cleanup.child = -1;
			break;
		}
		if (observed < 0 && errno != EINTR)
		{
			probe_error = errno;
			if (errno == ECHILD) cleanup.child = -1;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	// All potentially failing test assertions run after the private worker is joined/reaped.
	cleanup.StopAndReap();
	UAM_ASSERT_EQ(probe_error, 0);
	UAM_ASSERT(released);
	UAM_ASSERT(completed_normally);
	UAM_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	UAM_ASSERT(fs::is_fifo(primary));
	UAM_ASSERT_EQ(ReadFile(backup), backup_bytes);
	// Intended RED: the current inspection opened a FIFO before it rejected the Save.
	UAM_ASSERT(!paired_before_release);
#else
	// Windows still executes nonregular rejection and byte-preservation controls.
	UAM_ASSERT(fs::create_directory(primary));
	std::string error;
	UAM_ASSERT(!uam::AgentRunLedger::Save(temp.root, legacy, &error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT(fs::is_directory(primary));
	UAM_ASSERT_EQ(ReadFile(backup), backup_bytes);
#endif
}


UAM_TEST(ManagedFollowupUncheckedSaveBoundsRegularPrimaryInspection)
{
	for (int mode = 0; mode < 3; ++mode)
	{
		TempDir temp("uam-managed-ledger-regular-inspection");
		const AgentRun legacy = FollowupLegacyParentRun();
		UAM_ASSERT(uam::AgentRunLedger::Save(temp.root, legacy));
		UAM_ASSERT(uam::AgentRunLedger::Save(temp.root, legacy));
		const fs::path primary = FollowupRunPath(temp.root, legacy.id);
		const fs::path backup = uam::io::MakeBackupPath(primary);
		const std::string backup_bytes = ReadFile(backup);
		const std::string source = mode == 0 ? std::string{} :
		    mode == 1 ? std::string("{ordinary legacy corruption") :
		    std::string(512U * 1024U + 1U, 'x');
		UAM_ASSERT(uam::io::WriteTextFile(primary, source));
		std::string error;
		const bool saved = uam::AgentRunLedger::Save(temp.root, legacy, &error);
		if (mode == 2)
		{
			UAM_ASSERT(!saved && !error.empty());
			UAM_ASSERT_EQ(ReadFile(primary), source);
			UAM_ASSERT_EQ(ReadFile(backup), backup_bytes);
		}
		else
		{
			UAM_ASSERT(saved);
			const auto loaded = uam::AgentRunLedger::LoadAllReadOnly(temp.root);
			UAM_ASSERT(loaded.errors.empty() && loaded.runs.size() == 1);
			UAM_ASSERT_EQ(loaded.runs.front().id, legacy.id);
			UAM_ASSERT(!loaded.runs.front().managed_continuation);
		}
	}
}


namespace
{
	std::string ConditionalLedgerCandidateBytes(const AgentRun& value)
	{
		TempDir validation("uam-conditional-ledger-candidate-validation");
		UAM_ASSERT(uam::AgentRunLedger::Save(validation.root, FollowupLegacyParentRun()));
		UAM_ASSERT(uam::AgentRunLedger::Save(validation.root, value));
		const AgentRun loaded = ReadValidatedManagedWriterTarget(validation.root, true);
		UAM_ASSERT(loaded.managed_continuation == value.managed_continuation);
		UAM_ASSERT_EQ(loaded.status, value.status);
		return ReadFile(FollowupRunPath(validation.root));
	}

	void InstallConditionalLedgerExpected(const fs::path& root, const AgentRun& expected)
	{
		TempDir backup("uam-conditional-ledger-backup-validation");
		UAM_ASSERT(uam::AgentRunLedger::Save(backup.root, FollowupLegacyParentRun()));
		UAM_ASSERT(uam::AgentRunLedger::Save(backup.root, FollowupLegacyRun()));
		UAM_ASSERT(!ReadValidatedManagedWriterTarget(backup.root, false).managed_continuation);
		SaveInitialManagedWriterPrimary(root, expected, ReadFile(FollowupRunPath(backup.root)));
	}

	std::vector<fs::path> ConditionalLedgerStagingFiles(const fs::path& root)
	{
		std::vector<fs::path> files;
		const std::string primary_prefix = std::string(kFollowupRunId) + ".json" + uam::io::kTempWritePathSuffix;
		const std::string backup_prefix = std::string(kFollowupRunId) + ".json.bak" + uam::io::kTempWritePathSuffix;
		for (const auto& entry : fs::directory_iterator(root / "agent-runs"))
		{
			const auto name = entry.path().filename().string();
			if (entry.is_regular_file() && (name.starts_with(primary_prefix) || name.starts_with(backup_prefix)))
				files.push_back(entry.path());
		}
		std::ranges::sort(files);
		return files;
	}

	AgentRun ConditionalLedgerAppend(const AgentRun& expected)
	{
		AgentRun candidate = expected;
		auto& continuation = *candidate.managed_continuation;
		uam::ManagedRunTurnReceipt next;
		next.turn_id = "88888888-8888-4888-8888-888888888888";
		next.turn_number = continuation.current_turn_number + 1;
		next.client_request_id = "99999999-9999-4999-8999-999999999999";
		next.after_turn_id = continuation.current_turn_id;
		next.principal = continuation.turns.back().principal;
		next.text = "  Preserve exact new authored bytes.\n  ";
		next.delivery_id = next.turn_id;
		next.result_required = true;
		continuation.turns.push_back(next);
		continuation.current_turn_id = next.turn_id;
		continuation.current_turn_number = next.turn_number;
		++continuation.state_revision;
		candidate.status = "queued";
		candidate.expected_turn_serial = 0;
		candidate.started_at_epoch_ms = candidate.finished_at_epoch_ms = candidate.deadline_at_epoch_ms = 0;
		candidate.started_at.clear();
		candidate.finished_at.clear();
		candidate.result_excerpt.clear();
		candidate.diagnostic_code.clear();
		candidate.diagnostic.clear();
		candidate.updated_at = "2026-10-17 10:00:04";
		return candidate;
	}
}

UAM_TEST(ManagedFollowupConditionalSavePublishesExactCurrentTurnAndIsSingleUse)
{
	TempDir validation("uam-conditional-ledger-current-validation"), target("uam-conditional-ledger-current");
	const AgentRun expected = ValidatedManagedWriterPredecessor(validation.root);
	const AgentRun candidate = CompletedManagedWriterSuccessor(expected);
	const std::string candidate_bytes = ConditionalLedgerCandidateBytes(candidate);
	InstallConditionalLedgerExpected(target.root, expected);
	const fs::path primary = FollowupRunPath(target.root), backup = uam::io::MakeBackupPath(primary);
	const std::string primary_bytes = ReadFile(primary), backup_bytes = ReadFile(backup);
	std::string error;
	const auto prepared = uam::AgentRunLedger::PrepareSave(target.root, expected, candidate, &error);
	UAM_ASSERT(prepared != nullptr);
	UAM_ASSERT_EQ(ReadFile(primary), primary_bytes);
	UAM_ASSERT_EQ(ReadFile(backup), backup_bytes);
	const auto published = uam::AgentRunLedger::PublishPreparedSave(prepared, [] { return true; });
	UAM_ASSERT(published.state == uam::AgentRunPublishState::Committed);
	UAM_ASSERT_EQ(ReadFile(primary), candidate_bytes);
	UAM_ASSERT_EQ(ReadFile(backup), primary_bytes);
	UAM_ASSERT(ReadValidatedManagedWriterTarget(target.root, true).managed_continuation == candidate.managed_continuation);
	const auto reused = uam::AgentRunLedger::PublishPreparedSave(prepared, [] { return true; });
	UAM_ASSERT(reused.state == uam::AgentRunPublishState::Rejected);
	UAM_ASSERT_EQ(ReadFile(primary), candidate_bytes);
	UAM_ASSERT_EQ(ReadFile(backup), primary_bytes);
}

UAM_TEST(ManagedFollowupConditionalSaveAcceptsAppendAndInitialBindingShapes)
{
	for (const bool initial_binding : {false, true})
	{
		TempDir validation("uam-conditional-ledger-shape-validation"), target("uam-conditional-ledger-shape");
		const AgentRun queued = ValidatedManagedWriterPredecessor(validation.root);
		AgentRun expected = CompletedManagedWriterSuccessor(queued);
		AgentRun candidate = ConditionalLedgerAppend(expected);
		if (initial_binding)
		{
			expected = FollowupLegacyRun();
			expected.managed_continuation = queued.managed_continuation;
			auto& initial = *expected.managed_continuation;
			initial.turns.resize(1);
			initial.current_turn_id = expected.id;
			initial.current_turn_number = 1;
			initial.state_revision = 1;
			initial.execution.reset();
			candidate = expected;
			candidate.managed_continuation->state_revision = 2;
			// Storage-shape fixture only: this does not attest a provider or grant execution authority.
			candidate.managed_continuation->execution = queued.managed_continuation->execution;
		}
		const std::string candidate_bytes = ConditionalLedgerCandidateBytes(candidate);
		InstallConditionalLedgerExpected(target.root, expected);
		const fs::path primary = FollowupRunPath(target.root), backup = uam::io::MakeBackupPath(primary);
		const std::string primary_bytes = ReadFile(primary), backup_bytes = ReadFile(backup);
		const auto prepared = uam::AgentRunLedger::PrepareSave(target.root, expected, candidate);
		UAM_ASSERT(prepared != nullptr);
		UAM_ASSERT_EQ(ReadFile(primary), primary_bytes);
		UAM_ASSERT_EQ(ReadFile(backup), backup_bytes);
		UAM_ASSERT(uam::AgentRunLedger::PublishPreparedSave(prepared, [] { return true; }).state == uam::AgentRunPublishState::Committed);
		UAM_ASSERT_EQ(ReadFile(primary), candidate_bytes);
		UAM_ASSERT_EQ(ReadFile(backup), primary_bytes);
	}
}

UAM_TEST(ManagedFollowupConditionalSaveTwoPreparationsHaveOneWinner)
{
	TempDir validation("uam-conditional-ledger-two-validation"), target("uam-conditional-ledger-two");
	const AgentRun expected = ValidatedManagedWriterPredecessor(validation.root);
	const AgentRun candidate_a = CompletedManagedWriterSuccessor(expected);
	AgentRun candidate_b = candidate_a;
	candidate_b.status = "failed";
	candidate_b.diagnostic_code = candidate_b.managed_continuation->turns.back().diagnostic_code = "fixture_failed";
	candidate_b.managed_continuation->turns.back().status = uam::ManagedRunTurnStatus::Failed;
	const std::string a_bytes = ConditionalLedgerCandidateBytes(candidate_a);
	(void)ConditionalLedgerCandidateBytes(candidate_b);
	InstallConditionalLedgerExpected(target.root, expected);
	const fs::path primary = FollowupRunPath(target.root), backup = uam::io::MakeBackupPath(primary);
	const std::string old_primary = ReadFile(primary);
	const auto a = uam::AgentRunLedger::PrepareSave(target.root, expected, candidate_a);
	const auto b = uam::AgentRunLedger::PrepareSave(target.root, expected, candidate_b);
	UAM_ASSERT(a != nullptr && b != nullptr);
	UAM_ASSERT(uam::AgentRunLedger::PublishPreparedSave(a, [] { return true; }).state == uam::AgentRunPublishState::Committed);
	UAM_ASSERT(uam::AgentRunLedger::PublishPreparedSave(b, [] { return true; }).state == uam::AgentRunPublishState::Rejected);
	UAM_ASSERT_EQ(ReadFile(primary), a_bytes);
	UAM_ASSERT_EQ(ReadFile(backup), old_primary);
	UAM_ASSERT(!uam::AgentRunLedger::PrepareSave(target.root, expected, candidate_b));
	AgentRun audit = candidate_a;
	++audit.managed_continuation->state_revision;
	audit.managed_continuation->turns.back().audit_pending = true;
	(void)ConditionalLedgerCandidateBytes(audit);
	const auto interfered = uam::AgentRunLedger::PrepareSave(target.root, candidate_a, audit);
	UAM_ASSERT(interfered != nullptr);
	const std::string whitespace = a_bytes + "\n";
	UAM_ASSERT(uam::io::WriteTextFile(primary, whitespace));
	UAM_ASSERT(uam::AgentRunLedger::PublishPreparedSave(interfered, [] { return true; }).state == uam::AgentRunPublishState::Rejected);
	UAM_ASSERT_EQ(ReadFile(primary), whitespace);
	UAM_ASSERT_EQ(ReadFile(backup), old_primary);
	// Whitespace is harmless at fresh preparation, but changing exact bytes after preparation is interference.
	auto fresh = uam::AgentRunLedger::PrepareSave(target.root, candidate_a, audit);
	UAM_ASSERT(fresh != nullptr);
	fresh.reset();
	UAM_ASSERT_EQ(ReadFile(primary), whitespace);
}

UAM_TEST(ManagedFollowupConditionalSaveAbandonmentAndOwnerLossPreserveFiles)
{
	TempDir validation("uam-conditional-ledger-owner-validation"), target("uam-conditional-ledger-owner");
	const AgentRun expected = ValidatedManagedWriterPredecessor(validation.root);
	const AgentRun candidate = CompletedManagedWriterSuccessor(expected);
	(void)ConditionalLedgerCandidateBytes(candidate);
	InstallConditionalLedgerExpected(target.root, expected);
	const fs::path primary = FollowupRunPath(target.root), backup = uam::io::MakeBackupPath(primary);
	const std::string primary_bytes = ReadFile(primary), backup_bytes = ReadFile(backup);
	const auto before = ConditionalLedgerStagingFiles(target.root);
	auto abandoned = uam::AgentRunLedger::PrepareSave(target.root, expected, candidate);
	UAM_ASSERT(abandoned != nullptr);
	UAM_ASSERT(ConditionalLedgerStagingFiles(target.root).size() > before.size());
	abandoned.reset();
	UAM_ASSERT(ConditionalLedgerStagingFiles(target.root) == before);
	auto rejected = uam::AgentRunLedger::PrepareSave(target.root, expected, candidate);
	UAM_ASSERT(rejected != nullptr);
	int owner_checks = 0;
	UAM_ASSERT(uam::AgentRunLedger::PublishPreparedSave(rejected, [&] { ++owner_checks; return false; }).state == uam::AgentRunPublishState::Rejected);
	UAM_ASSERT(owner_checks > 0);
	rejected.reset();
	UAM_ASSERT(ConditionalLedgerStagingFiles(target.root) == before);
	UAM_ASSERT_EQ(ReadFile(primary), primary_bytes);
	UAM_ASSERT_EQ(ReadFile(backup), backup_bytes);
}

UAM_TEST(ManagedFollowupConditionalSaveFaultStagesPreserveDetailedOutcome)
{
	for (const auto fault : {uam::io::AtomicWriteStage::BeforeReplace,
	         uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync, uam::io::AtomicWriteStage::BeforeBackupCleanup})
	{
		TempDir validation("uam-conditional-ledger-fault-validation"), target("uam-conditional-ledger-fault");
		const AgentRun expected = ValidatedManagedWriterPredecessor(validation.root);
		const AgentRun candidate = CompletedManagedWriterSuccessor(expected);
		const std::string candidate_bytes = ConditionalLedgerCandidateBytes(candidate);
		InstallConditionalLedgerExpected(target.root, expected);
		const fs::path primary = FollowupRunPath(target.root), backup = uam::io::MakeBackupPath(primary);
		const std::string primary_bytes = ReadFile(primary), backup_bytes = ReadFile(backup);
		auto prepared = uam::AgentRunLedger::PrepareSave(target.root, expected, candidate);
		UAM_ASSERT(prepared != nullptr);
		bool reached = false;
		const auto result = uam::AgentRunLedger::PublishPreparedSave(prepared, [] { return true; }, [&](uam::io::AtomicWriteStage stage)
		{
			if (stage == fault) { reached = true; return false; }
			return true;
		});
		UAM_ASSERT(reached);
		prepared.reset();
		if (fault == uam::io::AtomicWriteStage::BeforeReplace)
		{
			UAM_ASSERT(result.state == uam::AgentRunPublishState::Rejected);
			UAM_ASSERT_EQ(ReadFile(primary), primary_bytes);
			UAM_ASSERT_EQ(ReadFile(backup), backup_bytes);
			UAM_ASSERT(ConditionalLedgerStagingFiles(target.root).empty());
		}
		else
		{
			UAM_ASSERT_EQ(ReadFile(primary), candidate_bytes);
			UAM_ASSERT_EQ(ReadFile(backup), backup_bytes);
			if (fault == uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync)
			{
				UAM_ASSERT(result.state == uam::AgentRunPublishState::Uncertain);
				const auto evidence = ConditionalLedgerStagingFiles(target.root);
				UAM_ASSERT(std::ranges::any_of(evidence, [&](const fs::path& path) { return ReadFile(path) == primary_bytes; }));
			}
			else UAM_ASSERT(result.state == uam::AgentRunPublishState::Committed);
		}
	}
}

UAM_TEST(ManagedFollowupConditionalSaveRejectsInvalidOrChangedTransitionsWithoutWrites)
{
	TempDir validation("uam-conditional-ledger-reject-validation"), target("uam-conditional-ledger-reject");
	const AgentRun queued = ValidatedManagedWriterPredecessor(validation.root);
	const AgentRun expected = CompletedManagedWriterSuccessor(queued);
	AgentRun update = expected;
	++update.managed_continuation->state_revision;
	update.managed_continuation->turns.back().audit_pending = true;
	(void)ConditionalLedgerCandidateBytes(update);
	InstallConditionalLedgerExpected(target.root, expected);
	const fs::path primary = FollowupRunPath(target.root), backup = uam::io::MakeBackupPath(primary);
	const std::string primary_bytes = ReadFile(primary), backup_bytes = ReadFile(backup);
	std::vector<AgentRun> changed(6, update);
	++changed[0].managed_continuation->state_revision;
	changed[1].effective_workspace_access = "write";
	changed[2].managed_continuation->turns.back().text += " changed authored bytes";
	changed[3].managed_continuation->turns.front().diagnostic_code = "changed_history";
	changed[4].managed_continuation->execution->native_session_id = "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee";
	changed[5].managed_continuation->turns.back().dispatch_attempted = false;
	for (const auto& candidate : changed)
	{
		// Each is individually codec-valid; rejection must concern conditional transition rules.
		(void)ConditionalLedgerCandidateBytes(candidate);
		std::string error;
		UAM_ASSERT(!uam::AgentRunLedger::PrepareSave(target.root, expected, candidate, &error));
		UAM_ASSERT(!error.empty());
	}
	AgentRun malformed = update;
	malformed.managed_continuation->current_turn_number = 0;
	UAM_ASSERT(!uam::AgentRunLedger::PrepareSave(target.root, expected, malformed));
	UAM_ASSERT(!uam::AgentRunLedger::PrepareSave(target.root, malformed, update));
	UAM_ASSERT(!uam::AgentRunLedger::PrepareSave(target.root, FollowupLegacyRun(), FollowupLegacyRun()));
	UAM_ASSERT(!uam::AgentRunLedger::PrepareSave(target.root, queued, expected));
	UAM_ASSERT_EQ(ReadFile(primary), primary_bytes);
	UAM_ASSERT_EQ(ReadFile(backup), backup_bytes);
	UAM_ASSERT(ConditionalLedgerStagingFiles(target.root).empty());
	int callbacks = 0;
	const auto null_result = uam::AgentRunLedger::PublishPreparedSave({}, [&] { ++callbacks; return true; },
	    [&](uam::io::AtomicWriteStage) { ++callbacks; return true; });
	UAM_ASSERT(null_result.state == uam::AgentRunPublishState::Rejected);
	UAM_ASSERT(!null_result.error.empty());
	UAM_ASSERT_EQ(callbacks, 0);
	UAM_ASSERT_EQ(ReadFile(primary), primary_bytes);
	UAM_ASSERT_EQ(ReadFile(backup), backup_bytes);
	auto unknown = nlohmann::json::parse(primary_bytes);
	unknown["unmodeled_owner_extension"] = {{"preserve", "opaque future data"}};
	const std::string unknown_bytes = unknown.dump(2);
	UAM_ASSERT(uam::io::WriteTextFile(primary, unknown_bytes));
	UAM_ASSERT(ReadValidatedManagedWriterTarget(target.root, true).managed_continuation == expected.managed_continuation);
	UAM_ASSERT(!uam::AgentRunLedger::PrepareSave(target.root, expected, update));
	UAM_ASSERT_EQ(ReadFile(primary), unknown_bytes);
	UAM_ASSERT_EQ(ReadFile(backup), backup_bytes);
	UAM_ASSERT(ConditionalLedgerStagingFiles(target.root).empty());
}


namespace
{
	AgentRun InitialManagedLedgerCandidate()
	{
		AgentRun candidate;
		candidate.id = kFollowupRunId;
		candidate.root_chat_id = "initial-ledger-root";
		candidate.transcript_chat_id = "initial-ledger-transcript";
		candidate.agent_id = "reviewer";
		candidate.definition_hash = "initial-definition-identity";
		candidate.definition_snapshot = "---\nversion: 1\n---\nKeep the original restrictive policy.";
		candidate.definition_instructions = "Keep the original restrictive policy.";
		candidate.provider_id = "codex-cli";
		candidate.model_id = "fixture-model-exact";
		candidate.execution_capability = "uam-prompt-injected";
		candidate.task = "  Initial authored task with exact bytes.\n  ";
		candidate.effective_workspace_access = "read";
		candidate.status = "queued";
		candidate.depth = 1;
		candidate.created_at = candidate.updated_at = "2026-10-17 10:00:00";
		candidate.result_delivery_id = candidate.id;
		candidate.deliver_result_to_root_chat = true;
		uam::ManagedRunContinuation initial;
		initial.owner.root_birth_id = kFollowupRootBirth;
		initial.owner.transcript_birth_id = kFollowupTranscriptBirth;
		initial.current_turn_id = candidate.id;
		initial.current_turn_number = 1;
		initial.state_revision = 1;
		uam::ManagedRunTurnReceipt receipt;
		receipt.turn_id = receipt.delivery_id = candidate.id;
		receipt.turn_number = 1;
		receipt.principal.kind = uam::ManagedRunPrincipalKind::UserRoot;
		receipt.principal.root_chat_id = candidate.root_chat_id;
		receipt.principal.root_birth_id = initial.owner.root_birth_id;
		receipt.result_required = candidate.deliver_result_to_root_chat;
		initial.turns.push_back(receipt);
		candidate.managed_continuation = std::move(initial);
		return candidate;
	}

	std::string ValidateInitialManagedLedgerBytes(const AgentRun& candidate)
	{
		TempDir validation("uam-initial-ledger-codec-validation");
		// Ordinary inert Save in an independent root validates the existing codec, not creation authority.
		UAM_ASSERT(uam::AgentRunLedger::Save(validation.root, candidate));
		const auto loaded = uam::AgentRunLedger::LoadAllReadOnly(validation.root);
		UAM_ASSERT(loaded.errors.empty());
		UAM_ASSERT_EQ(loaded.runs.size(), std::size_t{1});
		const AgentRun& run = loaded.runs.front();
		UAM_ASSERT_EQ(run.id, candidate.id);
		UAM_ASSERT_EQ(run.root_chat_id, candidate.root_chat_id);
		UAM_ASSERT_EQ(run.transcript_chat_id, candidate.transcript_chat_id);
		UAM_ASSERT(run.parent_run_id.empty());
		UAM_ASSERT_EQ(run.depth, 1);
		UAM_ASSERT_EQ(run.task, candidate.task);
		UAM_ASSERT_EQ(run.status, candidate.status);
		UAM_ASSERT(run.managed_continuation == candidate.managed_continuation);
		return ReadFile(FollowupRunPath(validation.root, candidate.id));
	}

	std::vector<fs::path> InitialManagedLedgerStaging(const fs::path& root)
	{
		if (!fs::exists(root / "agent-runs")) return {};
		std::vector<fs::path> files;
		const std::string prefix = std::string(kFollowupRunId) + ".json";
		for (const auto& entry : fs::directory_iterator(root / "agent-runs"))
		{
			const std::string name = entry.path().filename().string();
			if (fs::is_regular_file(entry.symlink_status()) &&
			    (name.starts_with(prefix + ".tmp.") || name.starts_with(prefix + ".bak.tmp."))) files.push_back(entry.path());
		}
		std::ranges::sort(files);
		return files;
	}
}

UAM_TEST(ManagedFollowupInitialSavePublishesExactBytesOnceAndCleansUnusedStages)
{
	const AgentRun candidate = InitialManagedLedgerCandidate();
	const std::string encoded = ValidateInitialManagedLedgerBytes(candidate);
	const auto shape = nlohmann::json::parse(encoded);
	UAM_ASSERT_EQ(shape.at("version"), nlohmann::json(2));
	UAM_ASSERT_EQ(shape.at("managedContinuation").at("turns").size(), std::size_t{1});
	UAM_ASSERT(shape.at("managedContinuation").at("execution").is_null());
	UAM_ASSERT(candidate.managed_continuation->owner.root_birth_id != candidate.managed_continuation->owner.transcript_birth_id);
	TempDir target("uam-initial-ledger-publish"), cleanup("uam-initial-ledger-cleanup");
	const fs::path primary = FollowupRunPath(target.root), backup = uam::io::MakeBackupPath(primary);
	UAM_ASSERT(!fs::exists(target.root / "agent-runs"));
	std::string error;
	auto prepared = uam::AgentRunLedger::PrepareInitialSave(target.root, candidate, &error);
	UAM_ASSERT(prepared != nullptr);
	UAM_ASSERT(!fs::exists(primary) && !fs::exists(backup));
	UAM_ASSERT(!InitialManagedLedgerStaging(target.root).empty());
	int owner_checks = 0;
	const auto published = uam::AgentRunLedger::PublishPreparedSave(prepared, [&] { ++owner_checks; return true; });
	UAM_ASSERT(published.state == uam::AgentRunPublishState::Committed);
	UAM_ASSERT(owner_checks > 0);
	UAM_ASSERT_EQ(ReadFile(primary), encoded);
	UAM_ASSERT(!fs::exists(backup));
	UAM_ASSERT(uam::AgentRunLedger::PublishPreparedSave(prepared, [] { return true; }).state == uam::AgentRunPublishState::Rejected);
	prepared.reset();
	UAM_ASSERT(InitialManagedLedgerStaging(target.root).empty());
	const auto loaded = uam::AgentRunLedger::LoadAllReadOnly(target.root);
	UAM_ASSERT(loaded.errors.empty());
	UAM_ASSERT_EQ(loaded.runs.size(), std::size_t{1});
	UAM_ASSERT(loaded.runs.front().managed_continuation == candidate.managed_continuation);
	UAM_ASSERT_EQ(ReadFile(primary), encoded);

	const fs::path unused_primary = FollowupRunPath(cleanup.root), unused_backup = uam::io::MakeBackupPath(unused_primary);
	auto abandoned = uam::AgentRunLedger::PrepareInitialSave(cleanup.root, candidate);
	UAM_ASSERT(abandoned != nullptr);
	UAM_ASSERT(!InitialManagedLedgerStaging(cleanup.root).empty());
	abandoned.reset();
	UAM_ASSERT(InitialManagedLedgerStaging(cleanup.root).empty());
	auto rejected = uam::AgentRunLedger::PrepareInitialSave(cleanup.root, candidate);
	UAM_ASSERT(rejected != nullptr);
	owner_checks = 0;
	UAM_ASSERT(uam::AgentRunLedger::PublishPreparedSave(rejected, [&] { ++owner_checks; return false; }).state == uam::AgentRunPublishState::Rejected);
	UAM_ASSERT(owner_checks > 0);
	rejected.reset();
	UAM_ASSERT(InitialManagedLedgerStaging(cleanup.root).empty());
	UAM_ASSERT(!fs::exists(unused_primary) && !fs::exists(unused_backup));
}

UAM_TEST(ManagedFollowupInitialSaveHasOneWinnerAndRejectsInterveningBackup)
{
	const AgentRun candidate = InitialManagedLedgerCandidate();
	const std::string encoded = ValidateInitialManagedLedgerBytes(candidate);
	AgentRun competing = candidate;
	competing.task = "A distinct independently valid creation candidate.";
	competing.deliver_result_to_root_chat = false;
	competing.managed_continuation->turns.front().result_required = false;
	(void)ValidateInitialManagedLedgerBytes(competing);
	TempDir target("uam-initial-ledger-two-prepares"), interference("uam-initial-ledger-backup-interference");
	const fs::path primary = FollowupRunPath(target.root), backup = uam::io::MakeBackupPath(primary);
	auto first = uam::AgentRunLedger::PrepareInitialSave(target.root, candidate);
	auto second = uam::AgentRunLedger::PrepareInitialSave(target.root, competing);
	UAM_ASSERT(first != nullptr && second != nullptr);
	UAM_ASSERT(!fs::exists(primary) && !fs::exists(backup));
	UAM_ASSERT(uam::AgentRunLedger::PublishPreparedSave(first, [] { return true; }).state == uam::AgentRunPublishState::Committed);
	UAM_ASSERT(uam::AgentRunLedger::PublishPreparedSave(second, [] { return true; }).state == uam::AgentRunPublishState::Rejected);
	first.reset();
	second.reset();
	UAM_ASSERT_EQ(ReadFile(primary), encoded);
	UAM_ASSERT(!fs::exists(backup));
	UAM_ASSERT(InitialManagedLedgerStaging(target.root).empty());

	const fs::path absent_primary = FollowupRunPath(interference.root), occupied_backup = uam::io::MakeBackupPath(absent_primary);
	auto staged = uam::AgentRunLedger::PrepareInitialSave(interference.root, candidate);
	UAM_ASSERT(staged != nullptr);
	UAM_ASSERT(!fs::exists(absent_primary) && !fs::exists(occupied_backup));
	UAM_ASSERT(uam::io::WriteTextFile(occupied_backup, encoded));
	UAM_ASSERT(uam::AgentRunLedger::PublishPreparedSave(staged, [] { return true; }).state == uam::AgentRunPublishState::Rejected);
	staged.reset();
	UAM_ASSERT(!fs::exists(absent_primary));
	UAM_ASSERT_EQ(ReadFile(occupied_backup), encoded);
	UAM_ASSERT(InitialManagedLedgerStaging(interference.root).empty());
}

UAM_TEST(ManagedFollowupInitialSaveFaultStagesRetainOnlyActualInitialEvidence)
{
	const AgentRun candidate = InitialManagedLedgerCandidate();
	const std::string encoded = ValidateInitialManagedLedgerBytes(candidate);
	for (const auto fault : {uam::io::AtomicWriteStage::BeforeReplace,
	         uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync, uam::io::AtomicWriteStage::BeforeBackupCleanup})
	{
		TempDir target("uam-initial-ledger-fault");
		const fs::path primary = FollowupRunPath(target.root), backup = uam::io::MakeBackupPath(primary);
		auto prepared = uam::AgentRunLedger::PrepareInitialSave(target.root, candidate);
		UAM_ASSERT(prepared != nullptr);
		UAM_ASSERT(!fs::exists(primary) && !fs::exists(backup));
		bool reached = false;
		const auto result = uam::AgentRunLedger::PublishPreparedSave(prepared, [] { return true; }, [&](uam::io::AtomicWriteStage stage)
		{
			if (stage == fault) { reached = true; return false; }
			return true;
		});
		UAM_ASSERT(reached);
		prepared.reset();
		UAM_ASSERT(!fs::exists(backup));
		if (fault == uam::io::AtomicWriteStage::BeforeReplace)
		{
			UAM_ASSERT(result.state == uam::AgentRunPublishState::Rejected);
			UAM_ASSERT(!fs::exists(primary));
			UAM_ASSERT(InitialManagedLedgerStaging(target.root).empty());
		}
		else
		{
			UAM_ASSERT_EQ(ReadFile(primary), encoded);
			UAM_ASSERT(result.state == (fault == uam::io::AtomicWriteStage::AfterReplaceBeforeDirectorySync
			    ? uam::AgentRunPublishState::Uncertain : uam::AgentRunPublishState::Committed));
			// No old-primary temp is expected: initial creation began with no primary.
		}
	}
}

UAM_TEST(ManagedFollowupInitialSaveRejectsOccupiedPathsAndNoninitialShapes)
{
	const AgentRun candidate = InitialManagedLedgerCandidate();
	(void)ValidateInitialManagedLedgerBytes(candidate);
	TempDir occupied("uam-initial-ledger-occupied"), invalid("uam-initial-ledger-invalid");
	AgentRun legacy = candidate;
	legacy.managed_continuation.reset();
	UAM_ASSERT(uam::AgentRunLedger::Save(occupied.root, legacy));
	UAM_ASSERT(uam::AgentRunLedger::Save(occupied.root, legacy));
	const fs::path primary = FollowupRunPath(occupied.root), backup = uam::io::MakeBackupPath(primary);
	const std::string legacy_primary = ReadFile(primary), legacy_backup = ReadFile(backup);
	UAM_ASSERT_EQ(nlohmann::json::parse(legacy_primary).at("version"), nlohmann::json(1));
	UAM_ASSERT(!uam::AgentRunLedger::PrepareInitialSave(occupied.root, candidate));
	UAM_ASSERT_EQ(ReadFile(primary), legacy_primary);
	UAM_ASSERT_EQ(ReadFile(backup), legacy_backup);
	UAM_ASSERT(fs::remove(primary));
	UAM_ASSERT(!uam::AgentRunLedger::PrepareInitialSave(occupied.root, candidate));
	UAM_ASSERT(!fs::exists(primary));
	UAM_ASSERT_EQ(ReadFile(backup), legacy_backup);
	const std::string malformed_backup = "Retain non-JSON recovery evidence.\n";
	UAM_ASSERT(uam::io::WriteTextFile(backup, malformed_backup));
	UAM_ASSERT(!uam::AgentRunLedger::PrepareInitialSave(occupied.root, candidate));
	UAM_ASSERT_EQ(ReadFile(backup), malformed_backup);
	UAM_ASSERT(fs::remove(backup));
	UAM_ASSERT(fs::create_directory(backup));
	UAM_ASSERT(uam::io::WriteTextFile(backup / "sentinel", "Keep directory evidence."));
	UAM_ASSERT(!uam::AgentRunLedger::PrepareInitialSave(occupied.root, candidate));
	UAM_ASSERT(fs::is_directory(backup));
	UAM_ASSERT_EQ(ReadFile(backup / "sentinel"), std::string("Keep directory evidence."));
	UAM_ASSERT(!fs::exists(primary));
	UAM_ASSERT(InitialManagedLedgerStaging(occupied.root).empty());
#if !defined(_WIN32)
	// A dangling link is occupied evidence even though exists() follows it and returns false.
	TempDir dangling("uam-initial-ledger-dangling-backup");
	fs::create_directories(dangling.root / "agent-runs");
	const fs::path linked_backup = uam::io::MakeBackupPath(FollowupRunPath(dangling.root));
	const fs::path missing_target = dangling.root / "missing-backup-target";
	fs::create_symlink(missing_target, linked_backup);
	UAM_ASSERT(fs::is_symlink(fs::symlink_status(linked_backup)) && !fs::exists(linked_backup));
	UAM_ASSERT(!uam::AgentRunLedger::PrepareInitialSave(dangling.root, candidate));
	UAM_ASSERT_EQ(fs::read_symlink(linked_backup), missing_target);
	UAM_ASSERT(!fs::exists(FollowupRunPath(dangling.root)));
	UAM_ASSERT(InitialManagedLedgerStaging(dangling.root).empty());
#endif

	std::vector<AgentRun> noninitial(5, candidate);
	noninitial[0].managed_continuation->state_revision = 2;
	uam::ManagedRunExecutionBinding execution;
	execution.connection_configuration_json = execution.provider_configuration_json = "{}";
	execution.execution_host_id = "local";
	execution.routing_mode = "explicit";
	execution.command_safety_tier = "off";
	noninitial[1].managed_continuation->execution = execution;
	noninitial[2].managed_continuation->owner.transcript_birth_id = kFollowupRootBirth;
	noninitial[3].status = "running";
	noninitial[3].managed_continuation->turns.front().status = uam::ManagedRunTurnStatus::Running;
	noninitial[3].managed_continuation->turns.front().dispatch_attempted = true;
	auto& checkpoint = noninitial[4];
	checkpoint.expected_turn_serial = 1;
	checkpoint.started_at_epoch_ms = checkpoint.deadline_at_epoch_ms = 1000;
	checkpoint.started_at = "2026-10-17 10:00:01";
	checkpoint.root_result_delivery_attempts = 1;
	checkpoint.result_excerpt = "No output belongs to an unstarted initial turn.";
	checkpoint.diagnostic_code = "not_initial";
	auto& receipt = checkpoint.managed_continuation->turns.front();
	receipt.dispatch_attempted = true;
	receipt.expected_turn_serial = 1;
	receipt.first_user_message_index = 0;
	receipt.user_message_created_at = checkpoint.started_at;
	receipt.started_at_epoch_ms = 1000;
	receipt.result_delivery_attempts = 1;
	receipt.audit_pending = true;
	receipt.diagnostic_code = checkpoint.diagnostic_code;
	for (const auto& value : noninitial)
	{
		// Each is codec-valid in a separate root; the new boundary must reject its initial-creation shape.
		(void)ValidateInitialManagedLedgerBytes(value);
		std::string error;
		UAM_ASSERT(!uam::AgentRunLedger::PrepareInitialSave(invalid.root, value, &error));
		UAM_ASSERT(!error.empty());
	}
	UAM_ASSERT(!uam::AgentRunLedger::PrepareInitialSave(invalid.root, legacy));
	AgentRun malformed = candidate;
	malformed.managed_continuation->current_turn_number = 0;
	UAM_ASSERT(!uam::AgentRunLedger::PrepareInitialSave(invalid.root, malformed));
	UAM_ASSERT(!fs::exists(FollowupRunPath(invalid.root)));
	UAM_ASSERT(!fs::exists(uam::io::MakeBackupPath(FollowupRunPath(invalid.root))));
	UAM_ASSERT(InitialManagedLedgerStaging(invalid.root).empty());
	int callbacks = 0;
	const auto absent = uam::AgentRunLedger::PublishPreparedSave({}, [&] { ++callbacks; return true; },
	    [&](uam::io::AtomicWriteStage) { ++callbacks; return true; });
	UAM_ASSERT(absent.state == uam::AgentRunPublishState::Rejected);
	UAM_ASSERT(!absent.error.empty());
	UAM_ASSERT_EQ(callbacks, 0);
}


UAM_TEST(ManagedFollowupInitialSaveRejectsIndependentCheckpointsAndCommitsFalseDeliveryContract)
{
	const AgentRun initial = InitialManagedLedgerCandidate();
	(void)ValidateInitialManagedLedgerBytes(initial);
	TempDir invalid("uam-initial-ledger-independent-checkpoints"), accepted("uam-initial-ledger-false-delivery");
	std::vector<AgentRun> checkpoints(10, initial);
	checkpoints[0].managed_continuation->turns.front().audit_pending = true;
	checkpoints[1].result_excerpt = "An unstarted initial turn has no output.";
	checkpoints[2].diagnostic_code = checkpoints[2].managed_continuation->turns.front().diagnostic_code = "not_initial";
	checkpoints[3].diagnostic = "An unstarted initial turn has no diagnostic.";
	checkpoints[4].deadline_at_epoch_ms = 1000;
	checkpoints[5].started_at = "2026-10-17 10:00:01";
	checkpoints[6].finished_at = "2026-10-17 10:00:02";
	checkpoints[7].root_result_delivered = checkpoints[7].managed_continuation->turns.front().result_delivered = true;
	checkpoints[8].root_result_delivery_attempts = checkpoints[8].managed_continuation->turns.front().result_delivery_attempts = 1;
	// Generic codec encoding normalizes this empty scalar; initial creation must require explicit identity.
	checkpoints[9].result_delivery_id.clear();
	for (const auto& value : checkpoints)
	{
		// Each changes one independent checkpoint, with only required receipt/scalar mirrors.
		(void)ValidateInitialManagedLedgerBytes(value);
		std::string error;
		UAM_ASSERT(!uam::AgentRunLedger::PrepareInitialSave(invalid.root, value, &error));
		UAM_ASSERT(!error.empty());
		UAM_ASSERT(!fs::exists(FollowupRunPath(invalid.root)));
		UAM_ASSERT(!fs::exists(uam::io::MakeBackupPath(FollowupRunPath(invalid.root))));
		UAM_ASSERT(InitialManagedLedgerStaging(invalid.root).empty());
	}
	UAM_ASSERT(!fs::exists(invalid.root / "agent-runs"));

	AgentRun candidate = initial;
	candidate.deliver_result_to_root_chat = false;
	candidate.managed_continuation->turns.front().result_required = false;
	const std::string encoded = ValidateInitialManagedLedgerBytes(candidate);
	const fs::path primary = FollowupRunPath(accepted.root), backup = uam::io::MakeBackupPath(primary);
	auto prepared = uam::AgentRunLedger::PrepareInitialSave(accepted.root, candidate);
	UAM_ASSERT(prepared != nullptr);
	UAM_ASSERT(!fs::exists(primary) && !fs::exists(backup));
	const auto result = uam::AgentRunLedger::PublishPreparedSave(prepared, [] { return true; });
	UAM_ASSERT(result.state == uam::AgentRunPublishState::Committed);
	prepared.reset();
	UAM_ASSERT_EQ(ReadFile(primary), encoded);
	UAM_ASSERT(!fs::exists(backup));
	UAM_ASSERT(InitialManagedLedgerStaging(accepted.root).empty());
	const auto loaded = uam::AgentRunLedger::LoadAllReadOnly(accepted.root);
	UAM_ASSERT(loaded.errors.empty());
	UAM_ASSERT_EQ(loaded.runs.size(), std::size_t{1});
	UAM_ASSERT(!loaded.runs.front().deliver_result_to_root_chat);
	UAM_ASSERT(!loaded.runs.front().managed_continuation->turns.front().result_required);
}

UAM_TEST(ManagedFollowupInitialSaveRejectsInterveningNonregularBackupWithoutDeletingEvidence)
{
	const AgentRun candidate = InitialManagedLedgerCandidate();
	(void)ValidateInitialManagedLedgerBytes(candidate);
	for (const bool dangling : {false, true})
	{
#if defined(_WIN32)
		if (dangling) continue; // Creating a symlink can require elevated Windows privileges.
#endif
		TempDir target("uam-initial-ledger-nonregular-interference");
		const fs::path primary = FollowupRunPath(target.root), backup = uam::io::MakeBackupPath(primary);
		auto prepared = uam::AgentRunLedger::PrepareInitialSave(target.root, candidate);
		UAM_ASSERT(prepared != nullptr);
		UAM_ASSERT(!fs::exists(primary) && !fs::exists(backup));
		const fs::path missing_target = target.root / "missing-backup-target";
		const std::string sentinel = "Preserve the intervening directory and its exact bytes.\n";
		if (dangling)
		{
			fs::create_symlink(missing_target, backup);
			UAM_ASSERT(fs::is_symlink(fs::symlink_status(backup)) && !fs::exists(backup));
		}
		else
		{
			UAM_ASSERT(fs::create_directory(backup));
			UAM_ASSERT(uam::io::WriteTextFile(backup / "sentinel", sentinel));
		}
		UAM_ASSERT(uam::AgentRunLedger::PublishPreparedSave(prepared, [] { return true; }).state == uam::AgentRunPublishState::Rejected);
		prepared.reset();
		UAM_ASSERT(!fs::exists(primary));
		UAM_ASSERT(InitialManagedLedgerStaging(target.root).empty());
		if (dangling)
		{
			UAM_ASSERT(fs::is_symlink(fs::symlink_status(backup)));
			UAM_ASSERT_EQ(fs::read_symlink(backup), missing_target);
			UAM_ASSERT(!fs::exists(missing_target));
		}
		else
		{
			UAM_ASSERT(fs::is_directory(backup));
			UAM_ASSERT_EQ(ReadFile(backup / "sentinel"), sentinel);
		}
	}
}
