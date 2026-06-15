#include "provider_profile_migration_service.h"

#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_build_config.h"
#include "common/state/app_state.h"

#include <string>
#include <string_view>

std::string ProviderProfileMigrationService::MapLegacyRuntimeId(std::string_view provider_id, bool use_native_history_provider_for_blank_id) const
{
	const std::string mapped_provider_id = uam::provider_ids::CanonicalCliProviderLookupId(provider_id);
	if (mapped_provider_id.empty())
	{
		return use_native_history_provider_for_blank_id ? provider_build_config::NativeHistoryProviderIdOrFirst() : mapped_provider_id;
	}

	if (uam::provider_ids::IsKnownCliProviderId(mapped_provider_id))
	{
		return provider_build_config::EnabledCliProviderIdOrFirst(mapped_provider_id);
	}

	return mapped_provider_id;
}

bool ProviderProfileMigrationService::MigrateActiveProviderIdToFixedModes(uam::AppState& app) const
{
	std::string mapped_provider_id = MapLegacyRuntimeId(app.settings.active_provider_id, true);
	if (!uam::provider_ids::IsKnownCliProviderId(mapped_provider_id))
	{
		mapped_provider_id = provider_build_config::FirstEnabledProviderId();
	}

	if (mapped_provider_id == app.settings.active_provider_id)
	{
		return false;
	}

	app.settings.active_provider_id = mapped_provider_id;
	return true;
}
