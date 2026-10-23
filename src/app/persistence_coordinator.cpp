#include "persistence_coordinator.h"
#include "app/provider_model_catalog_service.h"

#include "app/chat_domain_service.h"

#include "common/paths/app_paths.h"
#include "common/paths/path_utils.h"
#include "common/config/frontend_actions.h"
#include "common/config/settings_normalization.h"
#include "common/config/settings_store.h"
#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_build_config.h"
#include "common/utils/string_utils.h"

namespace fs = std::filesystem;

namespace
{
	constexpr const char* kChatsDirectoryName = "chats";
	constexpr const char* kThemesDirectoryName = "themes";
	constexpr const char* kAgentsDirectoryName = "agents";
	constexpr const char* kAgentRunsDirectoryName = "agent-runs";
	void NormalizeProviderCliSettings(AppSettings& settings)
	{
		settings.active_provider_id = uam::strings::NonEmptyOrFallback(
		    uam::provider_ids::NormalizeCliProviderAlias(settings.active_provider_id),
		    provider_build_config::FirstEnabledProviderId());
		settings.provider_extra_flags = uam::strings::Trim(settings.provider_extra_flags);
		settings.ui_theme = uam::settings::NormalizeThemeId(settings.ui_theme);
		uam::settings::ClampRuntimeTimeoutSettings(settings);
		uam::settings::ClampWindowSettings(settings);
	}

	bool EnsureDirectory(const fs::path& path, const std::string& label, std::string* error_out)
	{
		std::error_code error;
		uam::paths::CreateDirectoriesNoThrow(path, &error);
		if (!error)
		{
			return true;
		}

		if (error_out != nullptr)
		{
			*error_out = "Failed to create " + label + " '" + path.string() + "': " + error.message();
		}
		return false;
	}

} // namespace

bool PersistenceCoordinator::EnsureDataRootLayout(const fs::path& data_root, std::string* error_out) const
{
	if (!EnsureDirectory(data_root, "data root", error_out))
	{
		return false;
	}

	if (!EnsureDirectory(data_root / kChatsDirectoryName, "chats dir", error_out))
	{
		return false;
	}

	if (!EnsureDirectory(data_root / kThemesDirectoryName, "themes dir", error_out))
	{
		return false;
	}
	if (!EnsureDirectory(data_root / kAgentsDirectoryName, "agents dir", error_out) ||
	    !EnsureDirectory(data_root / kAgentRunsDirectoryName, "agent runs dir", error_out))
	{
		return false;
	}

	return true;
}

bool PersistenceCoordinator::SaveSettings(uam::AppState& app) const
{
	return SaveSettings(app, nullptr);
}

bool PersistenceCoordinator::SaveSettings(uam::AppState& app, SettingsMemoryWriteReceipt* memory_receipt,
    const std::function<bool(uam::io::AtomicWriteStage)>& stage_gate) const
{
	if (memory_receipt != nullptr) *memory_receipt = SettingsMemoryWriteReceipt{};
	const auto save_settings = [&]() -> bool
	{
		NormalizeProviderCliSettings(app.settings);
		ChatDomainService().RefreshRememberedSelection(app);
		if (!SettingsStore::Save(AppPaths::SettingsFilePath(app.data_root), app.settings, memory_receipt, stage_gate))
		{
			app.status_line = "Failed to persist settings.";
			return false;
		}

		if (app.provider_model_catalog != nullptr) app.provider_model_catalog->SetExecutionHostContext(app.settings.execution_hosts);
		return true;
	};
	if (memory_receipt == nullptr) return save_settings();
	try
	{
		return save_settings();
	}
	catch (...)
	{
		// SettingsStore captures commit facts before catalogue work can throw.
		memory_receipt->exception_observed = true;
		if (memory_receipt->outcome != SettingsMemoryWriteOutcome::Committed)
		{
			memory_receipt->outcome = memory_receipt->before_replace_permitted
			    ? SettingsMemoryWriteOutcome::UnknownAfterReplacePermission
			    : SettingsMemoryWriteOutcome::RejectedBeforeReplace;
		}
		return false;
	}
}

bool PersistenceCoordinator::LoadSettings(uam::AppState& app) const
{
	const SettingsLoadResult result = SettingsStore::Load(AppPaths::SettingsFilePath(app.data_root), app.settings);
	if (result.unrecovered_error)
	{
		app.status_line = result.warning;
		return false;
	}
	if (result.recovered_from_backup)
	{
		app.status_line = result.warning;
	}
	NormalizeProviderCliSettings(app.settings);
	if (app.provider_model_catalog != nullptr) app.provider_model_catalog->SetExecutionHostContext(app.settings.execution_hosts);
	return true;
}

void PersistenceCoordinator::LoadFrontendActions(uam::AppState& app) const
{
	std::string error;
	const fs::path action_map_path = app.data_root / "frontend_actions.txt";

	if (!uam::LoadFrontendActionMap(action_map_path, app.frontend_actions, &error))
	{
		app.frontend_actions = uam::DefaultFrontendActionMap();

		if (!uam::SaveFrontendActionMap(action_map_path, app.frontend_actions, &error) && !error.empty())
		{
			app.status_line = "Frontend action map reset, but saving failed: " + error;
		}
		else if (!error.empty())
		{
			app.status_line = "Frontend action map was invalid and has been reset.";
		}

		return;
	}

	uam::NormalizeFrontendActionMap(app.frontend_actions);
}
