#pragma once

#include "common/state/app_state.h"

#include <filesystem>
#include <string>

class PersistenceCoordinator
{
  public:
	bool EnsureDataRootLayout(const std::filesystem::path& data_root, std::string* error_out) const;
	bool SaveSettings(uam::AppState& app) const;
	bool LoadSettings(uam::AppState& app) const;
	void LoadFrontendActions(uam::AppState& app) const;
};
