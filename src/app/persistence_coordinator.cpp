#include "persistence_coordinator.h"

#include "app/application_core_helpers.h"

#include "common/app_paths.h"
#include "common/chat_folder_store.h"
#include "common/frontend_actions.h"
#include "common/local_chat_store.h"
#include "common/platform/platform_services.h"
#include "common/provider_profile.h"

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

fs::path PersistenceCoordinator::SettingsFilePath(const AppState& app) const
{
	return AppPaths::SettingsFilePath(app.data_root);
}

fs::path PersistenceCoordinator::ChatsRootPath(const AppState& app) const
{
	return AppPaths::ChatsRootPath(app.data_root);
}

fs::path PersistenceCoordinator::ChatPath(const AppState& app, const ChatSession& chat) const
{
	return AppPaths::ChatPath(app.data_root, chat.id);
}

fs::path PersistenceCoordinator::DefaultDataRootPath() const
{
	return AppPaths::DefaultDataRootPath();
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

void PersistenceCoordinator::SaveFolders(const AppState& app) const
{
	ChatFolderStore::Save(app.data_root, app.folders);
}

void PersistenceCoordinator::SaveProviders(const AppState& app) const
{
	ProviderProfileStore::Save(app.data_root, app.provider_profiles);
}

fs::path PersistenceCoordinator::ProviderProfileFilePath(const AppState& app) const
{
	return app.data_root / "providers.txt";
}

fs::path PersistenceCoordinator::FrontendActionFilePath(const AppState& app) const
{
	return app.data_root / "frontend_actions.txt";
}

void PersistenceCoordinator::LoadFrontendActions(AppState& app) const
{
	std::string error;

	if (!uam::LoadFrontendActionMap(FrontendActionFilePath(app), app.frontend_actions, &error))
	{
		app.frontend_actions = uam::DefaultFrontendActionMap();

		if (!uam::SaveFrontendActionMap(FrontendActionFilePath(app), app.frontend_actions, &error) && !error.empty())
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

std::vector<ChatSession> PersistenceCoordinator::LoadChats(const AppState& app) const
{
	return LocalChatStore::Load(app.data_root);
}

PersistenceCoordinator& GetPersistenceCoordinator()
{
	static PersistenceCoordinator coordinator;
	return coordinator;
}
