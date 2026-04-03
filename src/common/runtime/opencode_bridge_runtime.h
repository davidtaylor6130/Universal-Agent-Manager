#pragma once

#include "common/state/app_state.h"

#include <filesystem>
#include <string>

class LocalBridgeRuntimeService
{
  public:
	bool EnsureRunning(uam::AppState& app,
	                   const std::filesystem::path& model_folder,
	                   const std::string& requested_model,
	                   std::string* error_out = nullptr) const;

	void Stop(uam::AppState& app, bool keep_token = true) const;
};
