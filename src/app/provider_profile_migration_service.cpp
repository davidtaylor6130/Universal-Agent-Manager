#include "provider_profile_migration_service.h"

#include "app/application_core_helpers.h"
#include "common/provider/runtime/provider_build_config.h"

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
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
		return kRuntimeIdGeminiCli;
#else
		return provider_build_config::FirstEnabledProviderId();
#endif
	}

	if (lowered == kRuntimeIdGeminiCli)
	{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
		return kRuntimeIdGeminiCli;
#else
		return provider_build_config::FirstEnabledProviderId();
#endif
	}

	if (lowered == "codex" || lowered == kRuntimeIdCodexCli)
	{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
		return kRuntimeIdCodexCli;
#else
		return provider_build_config::FirstEnabledProviderId();
#endif
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
	const std::string lowered = ToLowerAscii(Trim(profile.id));
	return
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	    lowered == kRuntimeIdGeminiCli ||
#endif
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	    lowered == kRuntimeIdCodexCli ||
#endif
	    false;
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

	const std::string lowered = ToLowerAscii(Trim(app.settings.active_provider_id));
	if (
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
	    lowered == kRuntimeIdGeminiCli ||
#endif
#if UAM_ENABLE_RUNTIME_CODEX_CLI
	    lowered == kRuntimeIdCodexCli ||
#endif
	    false)
	{
		return false;
	}

	if (lowered == "gemini")
	{
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
		app.settings.active_provider_id = kRuntimeIdGeminiCli;
#else
		app.settings.active_provider_id = provider_build_config::FirstEnabledProviderId();
#endif
		return true;
	}

	if (lowered == "codex")
	{
#if UAM_ENABLE_RUNTIME_CODEX_CLI
		app.settings.active_provider_id = kRuntimeIdCodexCli;
#else
		app.settings.active_provider_id = provider_build_config::FirstEnabledProviderId();
#endif
		return true;
	}

	app.settings.active_provider_id = provider_build_config::FirstEnabledProviderId();
	return true;
}

bool ProviderProfileMigrationService::MigrateChatProviderBindingsToFixedModes(uam::AppState& app) const
{
	(void)app;
	return false;
}
