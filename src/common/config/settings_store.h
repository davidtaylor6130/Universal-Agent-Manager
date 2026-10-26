#pragma once

#include "common/models/app_models.h"

#include <nlohmann/json_fwd.hpp>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace uam::io
{
	enum class AtomicWriteStage;
}

enum class SettingsMemoryWriteOutcome
{
	RejectedBeforeReplace,
	Committed,
	UnknownAfterReplacePermission,
};

// Only the fields covered by the memory-settings action; never a full settings export.
struct SettingsMemoryCanonicalValues
{
	bool memory_enabled_default = false;
	std::string memory_level_default;
	int memory_idle_delay_seconds = 0;
	int memory_recall_budget_bytes = 0;
	int goal_max_loop_iterations = 0;
	int acp_setup_inactivity_timeout_seconds = 0;
	int acp_turn_output_limit_mib = 0;
	std::map<std::string, MemoryWorkerBinding> memory_worker_bindings;
	std::string permission_reviewer_provider_id;
	std::string permission_reviewer_model_id;
};

// canonical is authoritative only when outcome is Committed and canonical_values_ready.
struct SettingsMemoryWriteReceipt
{
	SettingsMemoryWriteOutcome outcome = SettingsMemoryWriteOutcome::RejectedBeforeReplace;
	bool before_replace_permitted = false;
	bool canonical_values_ready = false;
	bool atomic_write_returned = false;
	bool write_success = false;
	bool primary_committed = false;
	bool backup_degraded = false;
	bool exception_observed = false;
	SettingsMemoryCanonicalValues canonical;
};

// Scalar publication facts only; no canonical settings snapshot or extra payload copies.
// Facts survive exceptions; SaveObserved rethrows the original exception.
struct SettingsWriteObservation
{
	SettingsMemoryWriteOutcome outcome = SettingsMemoryWriteOutcome::RejectedBeforeReplace;
	bool before_replace_permitted = false;
	bool atomic_write_returned = false;
	bool write_success = false;
	bool primary_committed = false;
	bool backup_degraded = false;
	bool exception_observed = false;
};

struct SettingsLoadResult
{
	bool loaded = false;
	bool recovered_from_backup = false;
	bool unrecovered_error = false;
	std::string warning;
};

/// <summary>
/// Reads and writes persisted application settings.
/// </summary>
class SettingsStore
{
  public:
	static nlohmann::json SerializeExecutionRoutingSettings(const ExecutionRoutingSettings& settings);
	static bool ParseExecutionRoutingSettings(const nlohmann::json& value, ExecutionRoutingSettings& settings, std::string& error);
	/// <summary>Saves settings to disk.</summary>
	static bool Save(const std::filesystem::path& settings_file, const AppSettings& settings);
	/// <summary>Opt-in memory-write facts; a false bool alone does not prove non-commit.</summary>
	static bool Save(const std::filesystem::path& settings_file, const AppSettings& settings,
	    SettingsMemoryWriteReceipt* memory_receipt,
	    const std::function<bool(uam::io::AtomicWriteStage)>& stage_gate = {});

	/// <summary>Legacy bool/throw behavior with scalar write facts; never a canonical-value receipt.</summary>
	static bool SaveObserved(const std::filesystem::path& settings_file, const AppSettings& settings,
	    SettingsWriteObservation& observation,
	    const std::function<bool(uam::io::AtomicWriteStage)>& stage_gate = {});

	/// <summary>Loads settings from disk.</summary>
	static SettingsLoadResult Load(const std::filesystem::path& settings_file, AppSettings& settings);
	/// <summary>Same parser/normalization, primary only; missing/invalid primary leaves settings unchanged.</summary>
	static SettingsLoadResult LoadPrimary(const std::filesystem::path& settings_file, AppSettings& settings);
	/// Relocates the one UAM-owned root-bound settings field without rewriting unrelated or unknown lines.
	/// Other source-root references refuse migration instead of guessing at provider/workspace semantics.
	static bool RelocateDataFolderReferences(std::string_view original, const std::filesystem::path& source, const std::filesystem::path& destination, std::string& relocated, std::string& error);

  private:
	static bool SaveInternal(const std::filesystem::path& settings_file, const AppSettings& settings,
	    SettingsMemoryWriteReceipt* memory_receipt, SettingsWriteObservation* observation,
	    const std::function<bool(uam::io::AtomicWriteStage)>& stage_gate);
	static SettingsLoadResult LoadInternal(const std::filesystem::path& settings_file,
	    AppSettings& settings, bool allow_backup);
};
