#ifndef UAM_APP_PERSISTENCE_COORDINATOR_H
#define UAM_APP_PERSISTENCE_COORDINATOR_H


#include "common/state/app_state.h"

#include <filesystem>
#include <string>
#include <vector>

class PersistenceCoordinator
{
  public:
	std::string ExecuteCommandCaptureOutput(const std::string& command) const;
	std::filesystem::path TempFallbackDataRootPath() const;
	bool EnsureDataRootLayout(const std::filesystem::path& data_root, std::string* error_out) const;
	void LoadFrontendActions(uam::AppState& app) const;
};

#endif // UAM_APP_PERSISTENCE_COORDINATOR_H
