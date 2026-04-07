#ifndef UAM_APP_RUNTIME_LOCAL_SERVICE_H
#define UAM_APP_RUNTIME_LOCAL_SERVICE_H


#include "common/state/app_state.h"

#include <string>

class RuntimeLocalService
{
  public:
	bool ProviderUsesLocalBridgeRuntime(const ProviderProfile& provider) const;
	bool EnsureSelectedLocalRuntimeModelForProvider(uam::AppState& app) const;
	bool EnsureLocalRuntimeModelLoaded(uam::AppState& app, std::string* error_out = nullptr) const;
	bool RestartLocalBridgeIfModelChanged(uam::AppState& app, std::string* error_out = nullptr) const;
	void StopLocalBridge(uam::AppState& app) const;
};

#endif // UAM_APP_RUNTIME_LOCAL_SERVICE_H
