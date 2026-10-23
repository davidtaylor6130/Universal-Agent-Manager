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
#include "common/platform/platform_services.h"

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

bool PersistenceCoordinator::EnsureSettingsPublicationScope(uam::AppState& app) const
{
	if (!app.settings_publication_scope.empty()) return true;
	std::string scope = PlatformServicesFactory::Instance().process_service.GenerateUuid();
	if (scope.empty())
	{
		app.status_line = "Could not initialize settings publication scope.";
		return false;
	}
	app.settings_publication_scope.swap(scope);
	return true;
}

bool PersistenceCoordinator::AdvanceSettingsPublication(uam::AppState& app) const
{
	if (app.settings_publication_revision >= kMaxSettingsPublicationRevision)
	{
		app.status_line = "Settings publication revision is exhausted. Restart the app before changing settings.";
		return false;
	}
	if (!EnsureSettingsPublicationScope(app)) return false;
	++app.settings_publication_revision;
	return true;
}

bool PersistenceCoordinator::ResolveMemorySettingsUncertainty(uam::AppState& app) const
{
	if (!app.memory_settings_uncertain) return true;
	try
	{
		AppSettings primary;
		const SettingsLoadResult result = SettingsStore::LoadPrimary(AppPaths::SettingsFilePath(app.data_root), primary);
		if (!result.loaded || result.recovered_from_backup || result.unrecovered_error)
		{
			app.status_line = "Memory settings require a validated primary settings file before another settings save.";
			return false;
		}
		// No throwing copies after validation: only these ten covered fields are reconciled.
		app.settings.memory_enabled_default = primary.memory_enabled_default;
		app.settings.memory_level_default.swap(primary.memory_level_default);
		app.settings.memory_idle_delay_seconds = primary.memory_idle_delay_seconds;
		app.settings.memory_recall_budget_bytes = primary.memory_recall_budget_bytes;
		app.settings.goal_max_loop_iterations = primary.goal_max_loop_iterations;
		app.settings.acp_setup_inactivity_timeout_seconds = primary.acp_setup_inactivity_timeout_seconds;
		app.settings.acp_turn_output_limit_mib = primary.acp_turn_output_limit_mib;
		app.settings.memory_worker_bindings.swap(primary.memory_worker_bindings);
		app.settings.permission_reviewer_provider_id.swap(primary.permission_reviewer_provider_id);
		app.settings.permission_reviewer_model_id.swap(primary.permission_reviewer_model_id);
		app.memory_settings_uncertain = false;
		return true;
	}
	catch (...)
	{
		app.status_line = "Could not validate the primary memory settings before another settings save.";
		return false;
	}
}

bool PersistenceCoordinator::SaveSettings(uam::AppState& app) const
{
	return SaveSettings(app, nullptr);
}

bool PersistenceCoordinator::SaveSettings(uam::AppState& app, SettingsMemoryWriteReceipt* memory_receipt,
    const std::function<bool(uam::io::AtomicWriteStage)>& stage_gate) const
{
	if (memory_receipt != nullptr) *memory_receipt = SettingsMemoryWriteReceipt{};
	// A legacy caller may restore its whole pre-call settings snapshot after failure.
	// Preserve an existing recovery obligation even if primary readback succeeds first.
	const bool generic_recovery_obligation = memory_receipt == nullptr && app.memory_settings_uncertain;
	// Generic observation contains scalar facts only, not a canonical settings payload.
	SettingsWriteObservation observation;
	const auto observe_uncertainty = [&]()
	{
		const auto outcome = memory_receipt != nullptr ? memory_receipt->outcome : observation.outcome;
		if (outcome == SettingsMemoryWriteOutcome::UnknownAfterReplacePermission)
			app.memory_settings_uncertain = true;
	};
	const auto save_settings = [&]() -> bool
	{
		if (!AdvanceSettingsPublication(app)) return false;
		if (!ResolveMemorySettingsUncertainty(app)) return false;
		NormalizeProviderCliSettings(app.settings);
		ChatDomainService().RefreshRememberedSelection(app);
		const auto path = AppPaths::SettingsFilePath(app.data_root);
		const bool saved = memory_receipt != nullptr
		    ? SettingsStore::Save(path, app.settings, memory_receipt, stage_gate)
		    : SettingsStore::SaveObserved(path, app.settings, observation, stage_gate);
		observe_uncertainty();
		if (!saved)
		{
			app.status_line = "Failed to persist settings.";
			return false;
		}
		if (app.provider_model_catalog != nullptr) app.provider_model_catalog->SetExecutionHostContext(app.settings.execution_hosts);
		return true;
	};
	try
	{
		const bool saved = save_settings();
		if (!saved && generic_recovery_obligation) app.memory_settings_uncertain = true;
		return saved;
	}
	catch (...)
	{
		observe_uncertainty();
		if (memory_receipt == nullptr)
		{
			// Legacy callers receive no canonical payload and may restore speculative settings.
			// Even a known store commit cannot authorize that later rollback to overwrite primary memory.
			// Recovery can precede normalization/selection failures before observation begins.
			if (generic_recovery_obligation || observation.before_replace_permitted) app.memory_settings_uncertain = true;
			throw; // Preserve the original exception.
		}
		memory_receipt->exception_observed = true;
		// A later catalogue error cannot erase a completed producer commit or its canonical values.
		if (memory_receipt->outcome != SettingsMemoryWriteOutcome::Committed)
		{
			memory_receipt->outcome = memory_receipt->before_replace_permitted
			    ? SettingsMemoryWriteOutcome::UnknownAfterReplacePermission
			    : SettingsMemoryWriteOutcome::RejectedBeforeReplace;
		}
		observe_uncertainty();
		return false;
	}
}

bool PersistenceCoordinator::LoadSettings(uam::AppState& app) const
{
	if (!AdvanceSettingsPublication(app)) return false;
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
	// Preserve legacy backup-loading semantics, but backup/defaults cannot retire an Unknown primary.
	if (result.loaded && !result.recovered_from_backup) app.memory_settings_uncertain = false;
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
