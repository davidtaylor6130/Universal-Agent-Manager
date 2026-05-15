#pragma once

#include "common/state/app_state.h"

#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

bool PollPendingRuntimeCall(uam::AppState& app);

class ChatHistorySyncService
{
  public:
	struct ImportResult
	{
		int imported_count = 0;
		int total_count = 0;
	};
	ImportResult ImportAllNativeChatsToLocal(uam::AppState& app, bool delete_native_after_import, const std::string& target_chat_id = "") const;
	ImportResult ImportAllNativeChatsByDiscovery(uam::AppState& app, bool delete_native_after_import, const std::string& target_chat_id = "") const;
	void RefreshChatHistory(uam::AppState& app) const;
	bool SaveChatWithStatus(uam::AppState& app, const ChatSession& chat, const std::string& success, const std::string& failure) const;
	bool RenameChat(uam::AppState& app, ChatSession& chat, const std::string& requested_title) const;
	std::vector<ChatSession> LoadNativeSessionChats(const std::filesystem::path& chats_dir, const ProviderProfile& provider, std::stop_token stop_token = {}) const;
	std::optional<std::filesystem::path> ResolveNativeHistoryChatsDirForWorkspace(const std::filesystem::path& workspace_root) const;
	std::filesystem::path ResolveNativeHistoryChatsDirForChat(const uam::AppState& app, const ChatSession& chat) const;
	void ReconcileUnresolvedDraftLinksByDiscovery(uam::AppState& app) const;
	void LoadSidebarChats(uam::AppState& app) const;
	void LoadSidebarChatsByDiscovery(uam::AppState& app) const;
	void RefreshNativeSessionDirectory(uam::AppState& app) const;
	bool StartAsyncNativeChatLoad(uam::AppState& app, const ProviderProfile& provider, const std::filesystem::path& chats_dir) const;
	bool TryConsumeAsyncNativeChatLoad(uam::AppState& app, std::vector<ChatSession>& chats_out, std::string& error_out) const;
	std::vector<std::string> SessionIdsFromChats(const std::vector<ChatSession>& chats) const;
	std::optional<std::filesystem::path> FindNativeSessionFilePath(const std::filesystem::path& chats_dir, const std::string& session_id) const;
	bool DeleteNativeSessionFileForChat(const uam::AppState& app, const ChatSession& chat, std::error_code* error_out = nullptr) const;
	bool DeleteNativeWorkspaceHistoryForFolder(const uam::AppState& app, const ChatFolder& folder, std::error_code* error_out = nullptr) const;
	std::string ResolveResumeSessionIdForChat(const uam::AppState& app, const ChatSession& chat) const;
	bool PersistLocalDraftNativeSessionLink(const uam::AppState& app, ChatSession& local_chat, const std::string& native_session_id) const;
	void ApplyLocalOverrides(uam::AppState& app, std::vector<ChatSession>& native_chats) const;
	bool TruncateNativeSessionFromDisplayedMessage(const uam::AppState& app, const ChatSession& chat, int displayed_message_index, std::string* error_out) const;
	bool MoveChatToFolder(uam::AppState& app, ChatSession& chat, const std::string& new_folder_id) const;
	bool ExportChatToNative(const uam::AppState& app, const ChatSession& chat) const;
};
