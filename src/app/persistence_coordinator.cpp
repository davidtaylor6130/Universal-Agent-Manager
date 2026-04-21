#include "persistence_coordinator.h"

#include "app/application_core_helpers.h"
#include "app/chat_domain_service.h"

#include "common/paths/app_paths.h"
#include "common/chat/chat_folder_store.h"
#include "common/config/frontend_actions.h"
#include "common/config/settings_store.h"
#include "common/platform/platform_services.h"
#include "common/provider/runtime/provider_build_config.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sstream>

namespace fs = std::filesystem;
using uam::AppState;

namespace
{
	std::string NormalizeThemeChoice(std::string value)
	{
		value = ToLowerAscii(Trim(value));
		if (value == "light")
		{
			return "light";
		}
		if (value == "system")
		{
			return "system";
		}
		return "dark";
	}

	void ClampWindowSettings(AppSettings& settings)
	{
		settings.ui_scale_multiplier = std::clamp(settings.ui_scale_multiplier, 0.85f, 1.75f);
		settings.sidebar_width = std::clamp(settings.sidebar_width, 220.0f, 600.0f);
		settings.window_width = std::clamp(settings.window_width, 960, 8192);
		settings.window_height = std::clamp(settings.window_height, 620, 8192);
	}
}

std::string PersistenceCoordinator::ExecuteCommandCaptureOutput(const std::string& command) const
{
	const IPlatformProcessService& process_service = PlatformServicesFactory::Instance().process_service;
	const ProcessExecutionResult result = process_service.ExecuteCommand(command);

	if (!result.error.empty() && result.output.empty())
	{
		std::ostringstream message;
		message << "Failed to launch provider CLI command";

		if (errno != 0)
		{
			message << " (" << std::strerror(errno) << ")";
		}

		message << ".";
		return message.str();
	}

	std::string output = result.output;
	const int exit_code = result.exit_code;

	if (output.empty())
	{
		output = "(Provider CLI returned no output.)";
	}

	if (exit_code != 0)
	{
		output += "\n\n[Provider CLI exited with code " + std::to_string(exit_code) + "]";
	}

	return output;
}

fs::path PersistenceCoordinator::TempFallbackDataRootPath() const
{
	std::error_code ec;
	const fs::path temp = fs::temp_directory_path(ec);

	if (!ec)
	{
		return temp / "universal_agent_manager_data";
	}

	return fs::path("data");
}

bool PersistenceCoordinator::EnsureDataRootLayout(const fs::path& data_root, std::string* error_out) const
{
	std::error_code ec;
	fs::create_directories(data_root, ec);

	if (ec)
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to create data root '" + data_root.string() + "': " + ec.message();
		}

		return false;
	}

	fs::create_directories(data_root / "chats", ec);

	if (ec)
	{
		if (error_out != nullptr)
		{
			*error_out = "Failed to create chats dir '" + (data_root / "chats").string() + "': " + ec.message();
		}

		return false;
	}

	return true;
}

bool PersistenceCoordinator::SaveSettings(AppState& app) const
{
	if (Trim(app.settings.active_provider_id).empty())
	{
		app.settings.active_provider_id = provider_build_config::FirstEnabledProviderId();
	}
	app.settings.runtime_backend = "provider-cli";
	app.settings.provider_command_template = app.settings.provider_command_template.empty()
		? "gemini {resume} {flags} {prompt}"
		: app.settings.provider_command_template;
	app.settings.gemini_command_template = app.settings.provider_command_template;
	app.settings.gemini_yolo_mode = app.settings.provider_yolo_mode;
	app.settings.gemini_extra_flags = Trim(app.settings.provider_extra_flags);
	app.settings.provider_extra_flags = app.settings.gemini_extra_flags;
	app.settings.ui_theme = NormalizeThemeChoice(app.settings.ui_theme);
	app.settings.cli_idle_timeout_seconds = std::clamp(app.settings.cli_idle_timeout_seconds, 30, 3600);
	ClampWindowSettings(app.settings);

	ChatDomainService().RefreshRememberedSelection(app);
	if (!SettingsStore::Save(AppPaths::SettingsFilePath(app.data_root), app.settings, app.center_view_mode))
	{
		app.status_line = "Failed to persist settings.";
		return false;
	}

	return true;
}

void PersistenceCoordinator::LoadSettings(AppState& app) const
{
	SettingsStore::Load(AppPaths::SettingsFilePath(app.data_root), app.settings, app.center_view_mode);
	if (Trim(app.settings.active_provider_id).empty())
	{
		app.settings.active_provider_id = provider_build_config::FirstEnabledProviderId();
	}
	app.settings.runtime_backend = "provider-cli";
	app.settings.provider_extra_flags = Trim(app.settings.provider_extra_flags);
	app.settings.gemini_command_template = app.settings.provider_command_template;
	app.settings.gemini_yolo_mode = app.settings.provider_yolo_mode;
	app.settings.gemini_extra_flags = app.settings.provider_extra_flags;
	app.settings.ui_theme = NormalizeThemeChoice(app.settings.ui_theme);
	app.settings.cli_idle_timeout_seconds = std::clamp(app.settings.cli_idle_timeout_seconds, 30, 3600);
	ClampWindowSettings(app.settings);
}

void PersistenceCoordinator::LoadFrontendActions(AppState& app) const
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
