#include "test_harness.h"

#include <stdexcept>

namespace
{
	using Stage = uam::io::AtomicWriteStage;
	using Outcome = SettingsMemoryWriteOutcome;

	AppSettings ReloadPrimary(const std::filesystem::path& path)
	{
		AppSettings loaded;
		const auto result = SettingsStore::Load(path, loaded);
		UAM_ASSERT(result.loaded && !result.recovered_from_backup && !result.unrecovered_error);
		return loaded;
	}

	void SameCoveredValues(const SettingsMemoryCanonicalValues& canonical, const AppSettings& settings)
	{
		UAM_ASSERT_EQ(canonical.memory_enabled_default, settings.memory_enabled_default);
		UAM_ASSERT_EQ(canonical.memory_level_default, settings.memory_level_default);
		UAM_ASSERT_EQ(canonical.memory_idle_delay_seconds, settings.memory_idle_delay_seconds);
		UAM_ASSERT_EQ(canonical.memory_recall_budget_bytes, settings.memory_recall_budget_bytes);
		UAM_ASSERT_EQ(canonical.goal_max_loop_iterations, settings.goal_max_loop_iterations);
		UAM_ASSERT_EQ(canonical.acp_setup_inactivity_timeout_seconds, settings.acp_setup_inactivity_timeout_seconds);
		UAM_ASSERT_EQ(canonical.acp_turn_output_limit_mib, settings.acp_turn_output_limit_mib);
		UAM_ASSERT_EQ(canonical.memory_worker_bindings.size(), settings.memory_worker_bindings.size());
		for (const auto& [provider, binding] : settings.memory_worker_bindings)
		{
			const auto found = canonical.memory_worker_bindings.find(provider);
			UAM_ASSERT(found != canonical.memory_worker_bindings.end());
			UAM_ASSERT_EQ(found->second.worker_provider_id, binding.worker_provider_id);
			UAM_ASSERT_EQ(found->second.worker_model_id, binding.worker_model_id);
		}
		UAM_ASSERT_EQ(canonical.permission_reviewer_provider_id, settings.permission_reviewer_provider_id);
		UAM_ASSERT_EQ(canonical.permission_reviewer_model_id, settings.permission_reviewer_model_id);
	}

	AppSettings UnnormalizedMemorySettings()
	{
		AppSettings settings;
		settings.memory_enabled_default = true;
		settings.memory_level_default = "readOnly";
		settings.memory_idle_delay_seconds = -100;
		settings.memory_recall_budget_bytes = 999999;
		settings.goal_max_loop_iterations = -100;
		settings.acp_setup_inactivity_timeout_seconds = 999999;
		settings.acp_turn_output_limit_mib = 999999;
		settings.memory_worker_bindings.clear();
		settings.memory_worker_bindings["codex"] = MemoryWorkerBinding{"claude", "  private-worker-model  "};
		settings.permission_reviewer_provider_id = "claude";
		settings.permission_reviewer_model_id = "  private-review-model  ";
		return settings;
	}

	void CheckStageFailure(Stage target, bool throws, Outcome expected, bool replacement_visible)
	{
		uam_test::TempDir temp("uam-memory-write-stage");
		const auto path = temp.root / "settings.txt";
		AppSettings previous;
		previous.memory_idle_delay_seconds = 60;
		UAM_ASSERT(SettingsStore::Save(path, previous));
		const auto before = uam_test::ReadFile(path);
		AppSettings next = previous;
		next.memory_idle_delay_seconds = 177;
		SettingsMemoryWriteReceipt receipt;
		int visits = 0;
		const bool saved = SettingsStore::Save(path, next, &receipt, [&](Stage stage)
		{
			if (stage != target) return true;
			++visits;
			if (throws) throw std::runtime_error("private atomic stage fault");
			return false;
		});
		UAM_ASSERT_EQ(visits, 1);
		UAM_ASSERT(!saved && !receipt.write_success);
		UAM_ASSERT_EQ(receipt.outcome, expected);
		UAM_ASSERT_EQ(receipt.before_replace_permitted, replacement_visible);
		UAM_ASSERT(receipt.canonical_values_ready);
		UAM_ASSERT_EQ(receipt.canonical.memory_idle_delay_seconds, 177);
		UAM_ASSERT_EQ(receipt.exception_observed, throws);
		UAM_ASSERT_EQ(receipt.atomic_write_returned, !throws);
		UAM_ASSERT(!receipt.backup_degraded);
		if (replacement_visible)
		{
			// Independent current-primary observation is not a durability/commit receipt.
			UAM_ASSERT(uam_test::ReadFile(path) != before);
			UAM_ASSERT_EQ(ReloadPrimary(path).memory_idle_delay_seconds, 177);
		}
		else
		{
			UAM_ASSERT(!receipt.primary_committed);
			UAM_ASSERT_EQ(uam_test::ReadFile(path), before);
			UAM_ASSERT_EQ(ReloadPrimary(path).memory_idle_delay_seconds, 60);
		}
	}
}

UAM_TEST(MemoryWriteReceiptCommittedCanonicalMatchesReloadWithoutMutatingInput)
{
	uam_test::TempDir temp("uam-memory-write-canonical");
	const auto path = temp.root / "settings.txt";
	const AppSettings input = UnnormalizedMemorySettings();
	// Legacy serialization is an additional whole-input mutation witness.
	UAM_ASSERT(SettingsStore::Save(temp.root / "input-before.txt", input));
	SettingsMemoryWriteReceipt receipt;
	UAM_ASSERT(SettingsStore::Save(path, input, &receipt));
	UAM_ASSERT_EQ(receipt.outcome, Outcome::Committed);
	UAM_ASSERT(receipt.before_replace_permitted && receipt.canonical_values_ready && receipt.atomic_write_returned);
	UAM_ASSERT(receipt.write_success && receipt.primary_committed && !receipt.backup_degraded && !receipt.exception_observed);
	const auto loaded = ReloadPrimary(path);
	SameCoveredValues(receipt.canonical, loaded);
	UAM_ASSERT(loaded.memory_enabled_default && loaded.memory_level_default == "readOnly");
	UAM_ASSERT(loaded.memory_idle_delay_seconds != input.memory_idle_delay_seconds);
	UAM_ASSERT(loaded.memory_recall_budget_bytes != input.memory_recall_budget_bytes);
	UAM_ASSERT(loaded.goal_max_loop_iterations != input.goal_max_loop_iterations);
	UAM_ASSERT(loaded.acp_setup_inactivity_timeout_seconds != input.acp_setup_inactivity_timeout_seconds);
	UAM_ASSERT(loaded.acp_turn_output_limit_mib != input.acp_turn_output_limit_mib);
	UAM_ASSERT_EQ(loaded.permission_reviewer_provider_id, std::string(uam::provider_ids::kClaudeCli));
	UAM_ASSERT_EQ(loaded.permission_reviewer_model_id, std::string("private-review-model"));
	const auto worker = loaded.memory_worker_bindings.find(uam::provider_ids::kCodexCli);
	UAM_ASSERT(worker != loaded.memory_worker_bindings.end());
	UAM_ASSERT_EQ(worker->second.worker_provider_id, std::string(uam::provider_ids::kClaudeCli));
	UAM_ASSERT_EQ(worker->second.worker_model_id, std::string("private-worker-model"));
	SameCoveredValues(SettingsMemoryCanonicalValues{input.memory_enabled_default, input.memory_level_default,
	    input.memory_idle_delay_seconds, input.memory_recall_budget_bytes, input.goal_max_loop_iterations,
	    input.acp_setup_inactivity_timeout_seconds, input.acp_turn_output_limit_mib, input.memory_worker_bindings,
	    input.permission_reviewer_provider_id, input.permission_reviewer_model_id}, UnnormalizedMemorySettings());
	UAM_ASSERT(SettingsStore::Save(temp.root / "input-after.txt", input));
	UAM_ASSERT_EQ(uam_test::ReadFile(temp.root / "input-before.txt"), uam_test::ReadFile(temp.root / "input-after.txt"));
}

UAM_TEST(MemoryWriteReceiptBeforeReplaceRejectPreservesPrimary)
{
	CheckStageFailure(Stage::BeforeReplace, false, Outcome::RejectedBeforeReplace, false);
}

UAM_TEST(MemoryWriteReceiptBeforeReplaceThrowPreservesPrimary)
{
	CheckStageFailure(Stage::BeforeReplace, true, Outcome::RejectedBeforeReplace, false);
}

UAM_TEST(MemoryWriteReceiptAfterReplaceRejectRemainsUnknown)
{
	CheckStageFailure(Stage::AfterReplaceBeforeDirectorySync, false, Outcome::UnknownAfterReplacePermission, true);
}

UAM_TEST(MemoryWriteReceiptAfterReplaceThrowRemainsUnknown)
{
	CheckStageFailure(Stage::AfterReplaceBeforeDirectorySync, true, Outcome::UnknownAfterReplacePermission, true);
}

UAM_TEST(MemoryWriteReceiptBackupCleanupRejectCommitsButThrowRemainsUnknown)
{
	for (const bool throws : {false, true})
	{
		uam_test::TempDir temp("uam-memory-write-backup");
		const auto path = temp.root / "settings.txt";
		AppSettings settings;
		UAM_ASSERT(SettingsStore::Save(path, settings));
		settings.memory_idle_delay_seconds = 188;
		SettingsMemoryWriteReceipt receipt;
		int visits = 0;
		const bool saved = SettingsStore::Save(path, settings, &receipt, [&](Stage stage)
		{
			if (stage != Stage::BeforeBackupCleanup) return true;
			++visits;
			if (throws) throw std::runtime_error("private backup cleanup fault");
			return false;
		});
		UAM_ASSERT_EQ(visits, 1);
		UAM_ASSERT(receipt.before_replace_permitted && receipt.canonical_values_ready);
		UAM_ASSERT_EQ(receipt.exception_observed, throws);
		UAM_ASSERT_EQ(receipt.atomic_write_returned, !throws);
		UAM_ASSERT_EQ(ReloadPrimary(path).memory_idle_delay_seconds, 188);
		if (throws)
		{
			UAM_ASSERT(!saved && !receipt.write_success && !receipt.primary_committed && !receipt.backup_degraded);
			UAM_ASSERT_EQ(receipt.outcome, Outcome::UnknownAfterReplacePermission);
		}
		else
		{
			UAM_ASSERT(saved && receipt.write_success && receipt.primary_committed && receipt.backup_degraded);
			UAM_ASSERT_EQ(receipt.outcome, Outcome::Committed);
			SameCoveredValues(receipt.canonical, ReloadPrimary(path));
		}
	}
}

UAM_TEST(MemoryWriteReceiptReuseClearsEarlierCommitFacts)
{
	uam_test::TempDir temp("uam-memory-write-reuse");
	const auto path = temp.root / "settings.txt";
	AppSettings settings;
	settings.memory_idle_delay_seconds = 111;
	SettingsMemoryWriteReceipt receipt;
	UAM_ASSERT(SettingsStore::Save(path, settings, &receipt));
	UAM_ASSERT_EQ(receipt.outcome, Outcome::Committed);
	const auto committed = uam_test::ReadFile(path);
	settings.memory_idle_delay_seconds = 122;
	UAM_ASSERT(!SettingsStore::Save(path, settings, &receipt, [](Stage stage) { return stage != Stage::BeforeReplace; }));
	UAM_ASSERT_EQ(receipt.outcome, Outcome::RejectedBeforeReplace);
	UAM_ASSERT(!receipt.before_replace_permitted && !receipt.write_success && !receipt.primary_committed && !receipt.backup_degraded && !receipt.exception_observed);
	UAM_ASSERT(receipt.atomic_write_returned && receipt.canonical_values_ready);
	UAM_ASSERT_EQ(receipt.canonical.memory_idle_delay_seconds, 122);
	UAM_ASSERT_EQ(uam_test::ReadFile(path), committed);
	settings.memory_idle_delay_seconds = 133;
	UAM_ASSERT(!SettingsStore::Save(path, settings, &receipt, [](Stage stage)
	{
		if (stage == Stage::AfterReplaceBeforeDirectorySync) throw std::runtime_error("private reuse fault");
		return true;
	}));
	UAM_ASSERT_EQ(receipt.outcome, Outcome::UnknownAfterReplacePermission);
	UAM_ASSERT(receipt.before_replace_permitted && receipt.canonical_values_ready && receipt.exception_observed);
	UAM_ASSERT(!receipt.atomic_write_returned && !receipt.write_success && !receipt.primary_committed && !receipt.backup_degraded);
	UAM_ASSERT_EQ(receipt.canonical.memory_idle_delay_seconds, 133);
	UAM_ASSERT_EQ(ReloadPrimary(path).memory_idle_delay_seconds, 133);
}

UAM_TEST(MemoryWriteReceiptNullAndLegacyWritersPreserveBytesAndSuccess)
{
	uam_test::TempDir temp("uam-memory-write-legacy");
	const auto legacy = temp.root / "legacy.txt";
	const auto null_receipt = temp.root / "null.txt";
	const auto observed = temp.root / "observed.txt";
	AppSettings old;
	for (const auto& path : {legacy, null_receipt, observed}) UAM_ASSERT(SettingsStore::Save(path, old));
	const AppSettings input = UnnormalizedMemorySettings();
	UAM_ASSERT(SettingsStore::Save(legacy, input));
	UAM_ASSERT(SettingsStore::Save(null_receipt, input, nullptr));
	SettingsMemoryWriteReceipt receipt;
	UAM_ASSERT(SettingsStore::Save(observed, input, &receipt));
	UAM_ASSERT_EQ(receipt.outcome, Outcome::Committed);
	UAM_ASSERT_EQ(uam_test::ReadFile(legacy), uam_test::ReadFile(null_receipt));
	UAM_ASSERT_EQ(uam_test::ReadFile(legacy), uam_test::ReadFile(observed));
	UAM_ASSERT_EQ(uam_test::ReadFile(uam::io::MakeBackupPath(legacy)), uam_test::ReadFile(uam::io::MakeBackupPath(null_receipt)));
	UAM_ASSERT_EQ(uam_test::ReadFile(uam::io::MakeBackupPath(legacy)), uam_test::ReadFile(uam::io::MakeBackupPath(observed)));
	SameCoveredValues(receipt.canonical, ReloadPrimary(observed));
}

UAM_TEST(MemoryWriteReceiptLegacyDisabledPairCanonicalizesOff)
{
	uam_test::TempDir temp("uam-memory-write-disabled-pair");
	const auto path = temp.root / "settings.txt";
	AppSettings input = UnnormalizedMemorySettings();
	input.memory_enabled_default = false;
	SettingsMemoryWriteReceipt receipt;
	UAM_ASSERT(SettingsStore::Save(path, input, &receipt));
	UAM_ASSERT_EQ(receipt.outcome, Outcome::Committed);
	const auto loaded = ReloadPrimary(path);
	SameCoveredValues(receipt.canonical, loaded);
	UAM_ASSERT(!loaded.memory_enabled_default);
	UAM_ASSERT_EQ(loaded.memory_level_default, std::string("off"));
	UAM_ASSERT(!input.memory_enabled_default);
	UAM_ASSERT_EQ(input.memory_level_default, std::string("readOnly"));
}


UAM_TEST(SettingsObservedSaveMatchesLegacyPrimaryAndBackupBytes)
{
	uam_test::TempDir temp("uam-settings-observed-success");
	const auto legacy = temp.root / "legacy.txt";
	const auto observed = temp.root / "observed.txt";
	AppSettings before;
	before.memory_idle_delay_seconds = 61;
	UAM_ASSERT(SettingsStore::Save(legacy, before));
	UAM_ASSERT(SettingsStore::Save(observed, before));
	const AppSettings input = UnnormalizedMemorySettings();
	SettingsWriteObservation facts;
	UAM_ASSERT(SettingsStore::Save(legacy, input));
	UAM_ASSERT(SettingsStore::SaveObserved(observed, input, facts));
	UAM_ASSERT(facts.outcome == Outcome::Committed);
	UAM_ASSERT(facts.atomic_write_returned && facts.before_replace_permitted && facts.write_success && facts.primary_committed);
	UAM_ASSERT(!facts.exception_observed && !facts.backup_degraded);
	UAM_ASSERT_EQ(uam_test::ReadFile(legacy), uam_test::ReadFile(observed));
	UAM_ASSERT_EQ(uam_test::ReadFile(uam::io::MakeBackupPath(legacy)), uam_test::ReadFile(uam::io::MakeBackupPath(observed)));
	UAM_ASSERT_EQ(input.memory_idle_delay_seconds, -100);
	UAM_ASSERT_EQ(input.permission_reviewer_model_id, std::string("  private-review-model  "));
}

UAM_TEST(SettingsObservedSaveBeforeReplaceRejectionPreservesPrimary)
{
	uam_test::TempDir temp("uam-settings-observed-reject");
	const auto path = temp.root / "settings.txt";
	AppSettings settings;
	UAM_ASSERT(SettingsStore::Save(path, settings));
	const auto before = uam_test::ReadFile(path);
	settings.memory_idle_delay_seconds = 177;
	SettingsWriteObservation facts;
	int visits = 0;
	UAM_ASSERT(!SettingsStore::SaveObserved(path, settings, facts, [&](Stage stage)
	{
		if (stage != Stage::BeforeReplace) return true;
		++visits;
		return false;
	}));
	UAM_ASSERT_EQ(visits, 1);
	UAM_ASSERT(facts.outcome == Outcome::RejectedBeforeReplace);
	UAM_ASSERT(facts.atomic_write_returned && !facts.before_replace_permitted && !facts.write_success && !facts.primary_committed && !facts.exception_observed);
	UAM_ASSERT_EQ(uam_test::ReadFile(path), before);
}

UAM_TEST(SettingsObservedSaveAfterReplaceRejectionIsUnknown)
{
	uam_test::TempDir temp("uam-settings-observed-unknown");
	const auto path = temp.root / "settings.txt";
	AppSettings settings;
	UAM_ASSERT(SettingsStore::Save(path, settings));
	settings.memory_idle_delay_seconds = 178;
	SettingsWriteObservation facts;
	int visits = 0;
	UAM_ASSERT(!SettingsStore::SaveObserved(path, settings, facts, [&](Stage stage)
	{
		if (stage != Stage::AfterReplaceBeforeDirectorySync) return true;
		++visits;
		return false;
	}));
	UAM_ASSERT_EQ(visits, 1);
	UAM_ASSERT(facts.outcome == Outcome::UnknownAfterReplacePermission);
	UAM_ASSERT(facts.atomic_write_returned && facts.before_replace_permitted && !facts.write_success && !facts.exception_observed);
	AppSettings loaded;
	const auto result = SettingsStore::LoadPrimary(path, loaded);
	UAM_ASSERT(result.loaded && !result.recovered_from_backup);
	UAM_ASSERT_EQ(loaded.memory_idle_delay_seconds, 178);
	// Seeing replacement bytes is not a committed/durable receipt on either platform.
}

UAM_TEST(SettingsObservedSaveRethrowsOriginalStageExceptionWithFacts)
{
	struct PrivateStageFailure { const void* identity; };
	for (const Stage target : {Stage::BeforeReplace, Stage::AfterReplaceBeforeDirectorySync})
	{
		uam_test::TempDir temp("uam-settings-observed-throw");
		const auto path = temp.root / "settings.txt";
		AppSettings settings;
		UAM_ASSERT(SettingsStore::Save(path, settings));
		const auto before = uam_test::ReadFile(path);
		settings.memory_idle_delay_seconds = 179;
		SettingsWriteObservation facts;
		int identity = 0;
		int visits = 0;
		bool caught = false;
		try
		{
			(void)SettingsStore::SaveObserved(path, settings, facts, [&](Stage stage)
			{
				if (stage == target) { ++visits; throw PrivateStageFailure{&identity}; }
				return true;
			});
		}
		catch (const PrivateStageFailure& error) { caught = true; UAM_ASSERT(error.identity == &identity); }
		UAM_ASSERT(caught && visits == 1 && facts.exception_observed && !facts.atomic_write_returned);
		const bool replaced = target == Stage::AfterReplaceBeforeDirectorySync;
		UAM_ASSERT_EQ(facts.before_replace_permitted, replaced);
		UAM_ASSERT(facts.outcome == (replaced ? Outcome::UnknownAfterReplacePermission : Outcome::RejectedBeforeReplace));
		UAM_ASSERT(!facts.write_success && !facts.primary_committed && !facts.backup_degraded);
		if (!replaced) UAM_ASSERT_EQ(uam_test::ReadFile(path), before);
		else
		{
			AppSettings loaded;
			UAM_ASSERT(SettingsStore::LoadPrimary(path, loaded).loaded);
			UAM_ASSERT_EQ(loaded.memory_idle_delay_seconds, 179);
		}
	}
}

UAM_TEST(SettingsObservedSaveReuseClearsEarlierCommitAndExceptionFacts)
{
	uam_test::TempDir temp("uam-settings-observed-reuse");
	const auto path = temp.root / "settings.txt";
	AppSettings settings;
	SettingsWriteObservation facts;
	UAM_ASSERT(SettingsStore::SaveObserved(path, settings, facts));
	UAM_ASSERT(facts.outcome == Outcome::Committed && facts.primary_committed);
	UAM_ASSERT(!SettingsStore::SaveObserved(path, settings, facts, [](Stage stage) { return stage != Stage::BeforeReplace; }));
	UAM_ASSERT(facts.outcome == Outcome::RejectedBeforeReplace && facts.atomic_write_returned);
	UAM_ASSERT(!facts.primary_committed && !facts.before_replace_permitted && !facts.exception_observed && !facts.write_success);
}

UAM_TEST(SettingsPrimaryLoadNeverSubstitutesValidBackupForInvalidOrMissingPrimary)
{
	for (const bool missing : {false, true})
	{
		uam_test::TempDir temp("uam-settings-primary-only");
		const auto path = temp.root / "settings.txt";
		AppSettings settings;
		settings.memory_idle_delay_seconds = 111;
		UAM_ASSERT(SettingsStore::Save(path, settings));
		settings.memory_idle_delay_seconds = 122;
		UAM_ASSERT(SettingsStore::Save(path, settings));
		const auto backup = uam::io::MakeBackupPath(path);
		const auto backup_before = uam_test::ReadFile(backup);
		if (missing) UAM_ASSERT(std::filesystem::remove(path));
		else UAM_ASSERT(uam::io::WriteTextFile(path, "not-a-complete-settings-file\n"));
		AppSettings primary_only = UnnormalizedMemorySettings();
		const auto result = SettingsStore::LoadPrimary(path, primary_only);
		UAM_ASSERT(!result.loaded && !result.recovered_from_backup);
		UAM_ASSERT_EQ(result.unrecovered_error, !missing);
		UAM_ASSERT_EQ(primary_only.memory_idle_delay_seconds, -100);
		UAM_ASSERT_EQ(primary_only.memory_level_default, std::string("readOnly"));
		UAM_ASSERT_EQ(primary_only.permission_reviewer_model_id, std::string("  private-review-model  "));
		UAM_ASSERT_EQ(uam_test::ReadFile(backup), backup_before);
		AppSettings legacy;
		const auto recovered = SettingsStore::Load(path, legacy);
		UAM_ASSERT(recovered.loaded && recovered.recovered_from_backup && !recovered.unrecovered_error);
		UAM_ASSERT_EQ(legacy.memory_idle_delay_seconds, 111);
		UAM_ASSERT_EQ(uam_test::ReadFile(backup), backup_before);
		UAM_ASSERT_EQ(std::filesystem::exists(path), !missing);
		if (!missing) UAM_ASSERT_EQ(uam_test::ReadFile(path), std::string("not-a-complete-settings-file\n"));
	}
}

UAM_TEST(SettingsPrimaryLoadUsesExistingParserAndNormalization)
{
	uam_test::TempDir temp("uam-settings-primary-normalization");
	const auto path = temp.root / "settings.txt";
	const AppSettings input = UnnormalizedMemorySettings();
	UAM_ASSERT(SettingsStore::Save(path, input));
	// Start with a complete valid file but put an out-of-range value on disk,
	// independently of Save's normalizer. Both readers must normalize that input.
	std::string raw = uam_test::ReadFile(path);
	const std::string key = "memory_idle_delay_seconds=";
	const auto at = raw.find(key);
	UAM_ASSERT(at != std::string::npos);
	const auto end = raw.find('\n', at);
	UAM_ASSERT(end != std::string::npos);
	raw.replace(at + key.size(), end - at - key.size(), "-100");
	UAM_ASSERT(uam::io::WriteTextFile(path, raw));
	AppSettings primary;
	const auto loaded = SettingsStore::LoadPrimary(path, primary);
	UAM_ASSERT(loaded.loaded && !loaded.recovered_from_backup && !loaded.unrecovered_error);
	const AppSettings legacy = ReloadPrimary(path);
	SameCoveredValues(SettingsMemoryCanonicalValues{primary.memory_enabled_default, primary.memory_level_default,
	    primary.memory_idle_delay_seconds, primary.memory_recall_budget_bytes, primary.goal_max_loop_iterations,
	    primary.acp_setup_inactivity_timeout_seconds, primary.acp_turn_output_limit_mib, primary.memory_worker_bindings,
	    primary.permission_reviewer_provider_id, primary.permission_reviewer_model_id}, legacy);
	UAM_ASSERT(primary.memory_idle_delay_seconds != input.memory_idle_delay_seconds);
	UAM_ASSERT_EQ(primary.permission_reviewer_provider_id, std::string(uam::provider_ids::kClaudeCli));
	UAM_ASSERT_EQ(primary.permission_reviewer_model_id, std::string("private-review-model"));
}
