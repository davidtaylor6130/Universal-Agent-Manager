#pragma once

#include "common/state/app_state.h"

#include <string>
#include <string_view>

namespace uam
{
	enum class ChatProviderSwitchResult
	{
		Changed,
		Unchanged,
		UnsupportedProvider,
		ChatNotFound,
		ActiveRuntime,
		SaveFailed,
	};

	ChatProviderSwitchResult SwitchChatProvider(AppState& app, std::string_view chat_id, std::string_view provider_id);
}

bool RemoveChatById(uam::AppState& app, const std::string& chat_id);
bool DeleteFolderById(uam::AppState& app, const std::string& folder_id);
bool CreateFolder(uam::AppState& app, const std::string& title, const std::string& directory, std::string* created_folder_id = nullptr);
bool RenameFolderById(uam::AppState& app, const std::string& folder_id, const std::string& title, const std::string& directory);
std::string ResolveRequestedNewChatFolderId(uam::AppState& app, const std::string& requested_folder_id = std::string());
