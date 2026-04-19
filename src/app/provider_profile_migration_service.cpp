#include "provider_profile_migration_service.h"

#include "app/application_core_helpers.h"

namespace
{
	constexpr const char* kRuntimeIdGeminiCli = "gemini-cli";
	constexpr const char* kRuntimeIdCodexCli = "codex-cli";
} // namespace

bool ProviderProfileMigrationService::IsNativeHistoryProviderId(const std::string& provider_id) const
{
	const std::string lowered = ToLowerAscii(Trim(provider_id));
	return lowered == kRuntimeIdGeminiCli || lowered == "gemini";
}

std::string ProviderProfileMigrationService::MapLegacyRuntimeId(const std::string& provider_id, const bool prefer_cli_for_native_history) const
{
	(void)prefer_cli_for_native_history;
	const std::string trimmed = Trim(provider_id);
	const std::string lowered = ToLowerAscii(trimmed);

	if (lowered == "gemini")
	{
		return kRuntimeIdGeminiCli;
	}

	if (lowered == kRuntimeIdGeminiCli)
	{
		return kRuntimeIdGeminiCli;
	}

	if (lowered == "codex" || lowered == kRuntimeIdCodexCli)
	{
		return kRuntimeIdCodexCli;
	}

	return trimmed;
}

std::string ProviderProfileMigrationService::DefaultRuntimeIdForLegacyViewHint(const uam::AppState& app) const
{
	(void)app.center_view_mode;
	return kRuntimeIdGeminiCli;
}

bool ProviderProfileMigrationService::ShouldShowProviderProfileInUi(const ProviderProfile& profile) const
{
	const std::string lowered = ToLowerAscii(Trim(profile.id));
	return lowered == kRuntimeIdGeminiCli || lowered == kRuntimeIdCodexCli;
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
		app.settings.active_provider_id = kRuntimeIdGeminiCli;
		return true;
	}

	const std::string lowered = ToLowerAscii(Trim(app.settings.active_provider_id));
	if (lowered == kRuntimeIdGeminiCli || lowered == kRuntimeIdCodexCli)
	{
		return false;
	}

	if (lowered == "gemini")
	{
		app.settings.active_provider_id = kRuntimeIdGeminiCli;
		return true;
	}

	if (lowered == "codex")
	{
		app.settings.active_provider_id = kRuntimeIdCodexCli;
		return true;
	}

	app.settings.active_provider_id = kRuntimeIdGeminiCli;
	return true;
}

bool ProviderProfileMigrationService::MigrateChatProviderBindingsToFixedModes(uam::AppState& app) const
{
	(void)app;
	return false;
}
