#pragma once

#include <string>

namespace uam
{
	struct AppState;
}

class ProviderCliCompatibilityService
{
  public:
	std::string BuildVersionCheckCommand() const;
	std::string BuildPinCommand() const;
	void StartVersionCheck(uam::AppState& app, bool force) const;
	void StartPinToSupported(uam::AppState& app) const;
	void Poll(uam::AppState& app) const;
};

ProviderCliCompatibilityService& GetProviderCliCompatibilityService();
