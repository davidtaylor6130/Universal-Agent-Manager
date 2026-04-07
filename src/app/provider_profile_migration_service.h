#ifndef UAM_APP_PROVIDER_PROFILE_MIGRATION_SERVICE_H
#define UAM_APP_PROVIDER_PROFILE_MIGRATION_SERVICE_H


#include "common/state/app_state.h"

#include <string>

class ProviderProfileMigrationService
{
  public:
	bool IsNativeHistoryProviderId(const std::string& provider_id) const;
	std::string MapLegacyRuntimeId(const std::string& provider_id, bool prefer_cli_for_native_history) const;
	std::string DefaultRuntimeIdForLegacyViewHint(const uam::AppState& app) const;
	bool ShouldShowProviderProfileInUi(const ProviderProfile& profile) const;
	bool MigrateProviderProfilesToFixedModeIds(uam::AppState& app) const;
	bool MigrateActiveProviderIdToFixedModes(uam::AppState& app) const;
	bool MigrateChatProviderBindingsToFixedModes(uam::AppState& app) const;
};

#endif // UAM_APP_PROVIDER_PROFILE_MIGRATION_SERVICE_H
