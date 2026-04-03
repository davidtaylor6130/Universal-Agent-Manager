#include "persistence_coordinator.h"

#include "app/application_core_helpers.h"
#include "app/chat_domain_service.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_local_service.h"

#include "common/app_paths.h"
#include "common/chat_folder_store.h"
#include "common/frontend_actions.h"
#include "common/chat_repository.h"
#include "common/platform/platform_services.h"
#include "common/provider_profile.h"
#include "common/settings_store.h"
#include "common/ui/modals/modal_window_state.h"
#include "common/ui/theme/theme_choice.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace fs = std::filesystem;
using uam::AppState;

std::string PersistenceCoordinator::ExecuteCommandCaptureOutput(const std::string& command) const
{
	const std::string full_command = command + " 2>&1";
	const IPlatformProcessService& process_service = PlatformServicesFactory::Instance().process_service;
	std::string output;
	int raw_status = -1;
	std::string capture_error;

	if (!process_service.CaptureCommandOutput(full_command, &output, &raw_status, &capture_error))
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

	const int exit_code = process_service.NormalizeCapturedCommandExitCode(raw_status);

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

void PersistenceCoordinator::SaveSettings(AppState& app) const
{
	app.settings.ui_theme = NormalizeThemeChoice(app.settings.ui_theme);
	app.settings.runtime_backend = ProviderResolutionService().ActiveProviderUsesInternalEngine(app) ? "ollama-engine" : "provider-cli";
#if UAM_ENABLE_ENGINE_RAG
	app.settings.vector_db_backend = (app.settings.vector_db_backend == "none") ? "none" : "ollama-engine";
#else
	app.settings.vector_db_backend = "none";
	app.settings.selected_vector_model_id.clear();
#endif
	app.settings.selected_model_id = Trim(app.settings.selected_model_id);
	app.settings.models_folder_directory = Trim(app.settings.models_folder_directory);
	app.settings.selected_vector_model_id = Trim(app.settings.selected_vector_model_id);
	app.settings.vector_database_name_override = NormalizeVectorDatabaseName(app.settings.vector_database_name_override);
	app.settings.cli_idle_timeout_seconds = std::clamp(app.settings.cli_idle_timeout_seconds, 30, 3600);
	app.settings.rag_top_k = std::clamp(app.settings.rag_top_k, 1, 20);
	app.settings.rag_max_snippet_chars = std::clamp(app.settings.rag_max_snippet_chars, 120, 4000);
	app.settings.rag_max_file_bytes = std::clamp(app.settings.rag_max_file_bytes, 16 * 1024, 20 * 1024 * 1024);
	app.settings.rag_scan_max_tokens = std::clamp(app.settings.rag_scan_max_tokens, 0, 32768);
	ClampWindowSettings(app.settings);
	SyncRagServiceConfig(app);

	if (app.opencode_bridge.running)
	{
		fs::path desired_model_folder = NormalizeAbsolutePath(ResolveRagModelFolder(app));

		if (desired_model_folder.empty())
		{
			desired_model_folder = ResolveRagModelFolder(app);
		}

		const std::string desired_model = Trim(app.settings.selected_model_id);

		if (app.opencode_bridge.model_folder != desired_model_folder.string() || app.opencode_bridge.requested_model != desired_model)
		{
			std::string bridge_error;

			if (!RuntimeLocalService().RestartLocalBridgeIfModelChanged(app, &bridge_error) && !bridge_error.empty())
			{
				app.status_line = bridge_error;
			}
		}
	}

	ChatDomainService().RefreshRememberedSelection(app);
	SettingsStore::Save(AppPaths::SettingsFilePath(app.data_root), app.settings, app.center_view_mode);
}

void PersistenceCoordinator::LoadSettings(AppState& app) const
{
	SettingsStore::Load(AppPaths::SettingsFilePath(app.data_root), app.settings, app.center_view_mode);
#if UAM_ENABLE_ENGINE_RAG
	app.settings.vector_db_backend = (app.settings.vector_db_backend == "none") ? "none" : "ollama-engine";
#else
	app.settings.vector_db_backend = "none";
	app.settings.selected_vector_model_id.clear();
#endif
	app.settings.selected_model_id = Trim(app.settings.selected_model_id);
	app.settings.models_folder_directory = Trim(app.settings.models_folder_directory);
	app.settings.selected_vector_model_id = Trim(app.settings.selected_vector_model_id);
	app.settings.vector_database_name_override = NormalizeVectorDatabaseName(app.settings.vector_database_name_override);
	app.settings.cli_idle_timeout_seconds = std::clamp(app.settings.cli_idle_timeout_seconds, 30, 3600);

	if (Trim(app.settings.prompt_profile_root_path).empty())
	{
		app.settings.prompt_profile_root_path = AppPaths::DefaultGeminiUniversalRootPath().string();
	}

	SyncRagServiceConfig(app);
	app.rag_manual_query_max = std::clamp(app.settings.rag_top_k, 1, 20);
	app.rag_manual_query_min = 1;
}

void PersistenceCoordinator::LoadFrontendActions(AppState& app) const
{
	std::string error;
	const fs::path l_actionMapPath = app.data_root / "frontend_actions.txt";

	if (!uam::LoadFrontendActionMap(l_actionMapPath, app.frontend_actions, &error))
	{
		app.frontend_actions = uam::DefaultFrontendActionMap();

		if (!uam::SaveFrontendActionMap(l_actionMapPath, app.frontend_actions, &error) && !error.empty())
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
