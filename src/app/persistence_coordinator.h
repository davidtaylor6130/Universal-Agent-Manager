#ifndef UAM_APP_PERSISTENCE_COORDINATOR_H
#define UAM_APP_PERSISTENCE_COORDINATOR_H


#include "common/state/app_state.h"

#include <filesystem>
#include <string>
#include <vector>

class PersistenceCoordinator
{
  public:
	std::string ExecuteCommandCaptureOutput(const std::string& command) const;

	std::filesystem::path SettingsFilePath(const uam::AppState& app) const;
	std::filesystem::path ChatsRootPath(const uam::AppState& app) const;
	std::filesystem::path ChatPath(const uam::AppState& app, const ChatSession& chat) const;
	std::filesystem::path DefaultDataRootPath() const;
	std::filesystem::path TempFallbackDataRootPath() const;
	bool EnsureDataRootLayout(const std::filesystem::path& data_root, std::string* error_out) const;

	void SaveFolders(const uam::AppState& app) const;
	void SaveProviders(const uam::AppState& app) const;
	std::filesystem::path ProviderProfileFilePath(const uam::AppState& app) const;
	std::filesystem::path FrontendActionFilePath(const uam::AppState& app) const;
	void LoadFrontendActions(uam::AppState& app) const;
	std::vector<ChatSession> LoadChats(const uam::AppState& app) const;
};

PersistenceCoordinator& GetPersistenceCoordinator();

#endif // UAM_APP_PERSISTENCE_COORDINATOR_H
