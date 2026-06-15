#include "persistence_coordinator.h"

#include "app/chat_domain_service.h"

#include "common/paths/app_paths.h"
#include "common/paths/path_utils.h"
#include "common/chat/chat_folder_store.h"
#include "common/config/frontend_actions.h"
#include "common/config/settings_normalization.h"
#include "common/config/settings_store.h"
#include "common/platform/platform_services.h"
#include "common/provider/runtime/provider_build_config.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <optional>
#include <string_view>

namespace fs = std::filesystem;

namespace
{
	constexpr const char* kDefaultDataDirectoryName = "data";
	constexpr const char* kFallbackDataRootDirectoryName = "universal_agent_manager_data";
	constexpr const char* kChatsDirectoryName = "chats";
	constexpr const char* kProviderCliNoOutputMessage = "(Provider CLI returned no output.)";
	constexpr const char* kProviderCliExitCodePrefix = "\n\n[Provider CLI exited with code ";

	void SetError(std::string* error_out, std::string_view message)
	{
		if (error_out != nullptr)
		{
			error_out->assign(message);
		}
	}

	void NormalizeProviderCliSettings(AppSettings& settings)
	{
		settings.active_provider_id = provider_build_config::EnabledCliProviderIdOrFirst(settings.active_provider_id);
		settings.runtime_backend = "provider-cli";
		settings.gemini_yolo_mode = settings.provider_yolo_mode;
		settings.gemini_extra_flags = uam::strings::Trim(settings.provider_extra_flags);
		settings.provider_extra_flags = settings.gemini_extra_flags;
		settings.ui_theme = uam::settings::NormalizeThemeId(settings.ui_theme);
		settings.cli_idle_timeout_seconds = std::clamp(settings.cli_idle_timeout_seconds, uam::settings::kMinCliIdleTimeoutSeconds, uam::settings::kMaxCliIdleTimeoutSeconds);
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

		SetError(error_out, "Failed to create " + label + " '" + path.string() + "': " + error.message());
		return false;
	}

	std::string ProviderLaunchFailureMessage()
	{
		std::string message = "Failed to launch provider CLI command";
		if (errno != 0)
		{
			message += " (";
			message += std::strerror(errno);
			message += ")";
		}
		message += ".";
		return message;
	}
} // namespace

std::string PersistenceCoordinator::ExecuteCommandCaptureOutput(const std::string& command) const
{
	const IPlatformProcessService& process_service = PlatformServicesFactory::Instance().process_service;
	const ProcessExecutionResult result = process_service.ExecuteCommand(command);

	if (!result.error.empty() && result.output.empty())
	{
		return ProviderLaunchFailureMessage();
	}

	std::string output = result.output;
	const int exit_code = result.exit_code;

	if (output.empty())
	{
		output = kProviderCliNoOutputMessage;
	}

	if (exit_code != 0)
	{
		output += kProviderCliExitCodePrefix + std::to_string(exit_code) + "]";
	}

	return output;
}

fs::path PersistenceCoordinator::TempFallbackDataRootPath() const
{
	if (const std::optional<fs::path> temp = uam::paths::TempDirectoryPathNoThrow())
	{
		return *temp / kFallbackDataRootDirectoryName;
	}

	return fs::path(kDefaultDataDirectoryName);
}

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

	return true;
}

bool PersistenceCoordinator::SaveSettings(uam::AppState& app) const
{
	NormalizeProviderCliSettings(app.settings);
	ChatDomainService().RefreshRememberedSelection(app);
	if (!SettingsStore::Save(AppPaths::SettingsFilePath(app.data_root), app.settings, app.center_view_mode))
	{
		app.status_line = "Failed to persist settings.";
		return false;
	}

	return true;
}

void PersistenceCoordinator::LoadSettings(uam::AppState& app) const
{
	SettingsStore::Load(AppPaths::SettingsFilePath(app.data_root), app.settings, app.center_view_mode);
	NormalizeProviderCliSettings(app.settings);
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
