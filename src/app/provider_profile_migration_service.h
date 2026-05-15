#pragma once

#include <string>
#include <string_view>

namespace uam
{
	struct AppState;
}

class ProviderProfileMigrationService
{
  public:
	std::string MapLegacyRuntimeId(std::string_view provider_id, bool use_native_history_provider_for_blank_id) const;
	bool MigrateActiveProviderIdToFixedModes(uam::AppState& app) const;
};
