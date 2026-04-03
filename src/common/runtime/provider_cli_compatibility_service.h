#ifndef UAM_COMMON_RUNTIME_PROVIDER_CLI_COMPATIBILITY_SERVICE_H
#define UAM_COMMON_RUNTIME_PROVIDER_CLI_COMPATIBILITY_SERVICE_H

#include <string>

namespace uam
{
	struct AppState;
}

class ProviderCliCompatibilityService
{
  public:
	void StartVersionCheck(uam::AppState& app, bool force) const;
	void StartPinToSupported(uam::AppState& app) const;
	void Poll(uam::AppState& app) const;
};

#endif // UAM_COMMON_RUNTIME_PROVIDER_CLI_COMPATIBILITY_SERVICE_H
