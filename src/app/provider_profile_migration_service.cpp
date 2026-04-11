#include "provider_profile_migration_service.h"

#include "app/application_core_helpers.h"

#include "common/provider/runtime/provider_build_config.h"

namespace
{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	constexpr const char* kRuntimeIdCli = "gemini-cli";
#endif
} // namespace

bool ProviderProfileMigrationService::IsNativeHistoryProviderId(const std::string& provider_id) const
{
	const std::string lowered = ToLowerAscii(Trim(provider_id));
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	return lowered == kRuntimeIdCli || lowered == "gemini";
#else
	(void)provider_id;
	return false;
#endif
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
	return provider_build_config::FirstEnabledProviderId();
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
		app.settings.active_provider_id = provider_build_config::FirstEnabledProviderId();
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
