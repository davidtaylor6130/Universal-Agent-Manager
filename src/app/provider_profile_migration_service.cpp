#include "provider_profile_migration_service.h"

#include "app/application_core_helpers.h"

namespace
{
	constexpr const char* kRuntimeIdCli = "gemini-cli";
} // namespace

bool ProviderProfileMigrationService::IsNativeHistoryProviderId(const std::string& provider_id) const
{
	const std::string lowered = ToLowerAscii(Trim(provider_id));
	return lowered == kRuntimeIdCli || lowered == "gemini";
}

std::string ProviderProfileMigrationService::MapLegacyRuntimeId(const std::string& provider_id, const bool prefer_cli_for_native_history) const
{
	(void)prefer_cli_for_native_history;
	const std::string trimmed = Trim(provider_id);
	const std::string lowered = ToLowerAscii(trimmed);

	if (lowered == "gemini")
	{
		return kRuntimeIdCli;
	}

	if (lowered == kRuntimeIdCli)
	{
		return kRuntimeIdCli;
	}

	return trimmed;
}

std::string ProviderProfileMigrationService::DefaultRuntimeIdForLegacyViewHint(const uam::AppState& app) const
{
	(void)app.center_view_mode;
	return kRuntimeIdCli;
}

bool ProviderProfileMigrationService::ShouldShowProviderProfileInUi(const ProviderProfile& profile) const
{
	return Trim(profile.id) == kRuntimeIdCli;
}

bool ProviderProfileMigrationService::MigrateProviderProfilesToFixedModeIds(uam::AppState& app) const
{
	(void)app;
	return false;
}

bool ProviderProfileMigrationService::MigrateActiveProviderIdToFixedModes(uam::AppState& app) const
{
	if (Trim(app.settings.active_provider_id).empty())
	{
		app.settings.active_provider_id = kRuntimeIdCli;
		return true;
	}

	if (ToLowerAscii(Trim(app.settings.active_provider_id)) != kRuntimeIdCli)
	{
		app.settings.active_provider_id = kRuntimeIdCli;
		return true;
	}

	return false;
}

bool ProviderProfileMigrationService::MigrateChatProviderBindingsToFixedModes(uam::AppState& app) const
{
	(void)app;
	return false;
}
