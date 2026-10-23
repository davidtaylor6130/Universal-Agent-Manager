#pragma once

#include "common/state/app_state.h"

#include <filesystem>
#include <functional>
#include <string>

namespace uam::io
{
	enum class AtomicWriteStage;
}

struct SettingsMemoryWriteReceipt;

class PersistenceCoordinator
{
  public:
	bool EnsureDataRootLayout(const std::filesystem::path& data_root, std::string* error_out) const;
	static constexpr std::uint64_t kMaxSettingsPublicationRevision = 9007199254740991ULL;
	bool EnsureSettingsPublicationScope(uam::AppState& app) const;
	bool AdvanceSettingsPublication(uam::AppState& app) const;
	// Call before forming a memory mutation candidate; never substitutes backup contents.
	bool ResolveMemorySettingsUncertainty(uam::AppState& app) const;
	bool SaveSettings(uam::AppState& app) const;
	bool SaveSettings(uam::AppState& app, SettingsMemoryWriteReceipt* memory_receipt,
	    const std::function<bool(uam::io::AtomicWriteStage)>& stage_gate = {}) const;
	bool LoadSettings(uam::AppState& app) const;
	void LoadFrontendActions(uam::AppState& app) const;
};
