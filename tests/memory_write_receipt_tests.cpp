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
