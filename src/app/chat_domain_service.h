#ifndef UAM_APP_CHAT_DOMAIN_SERVICE_H
#define UAM_APP_CHAT_DOMAIN_SERVICE_H


#include "common/state/app_state.h"

#include <cstddef>
#include <string>
#include <vector>

class ChatDomainService
{
  public:
	std::string NewFolderId() const;
	int FindFolderIndexById(const uam::AppState& app, const std::string& folder_id) const;
	ChatFolder* FindFolderById(uam::AppState& app, const std::string& folder_id) const;
	const ChatFolder* FindFolderById(const uam::AppState& app, const std::string& folder_id) const;
	void EnsureDefaultFolder(uam::AppState& app) const;
	void EnsureNewChatFolderSelection(uam::AppState& app) const;
	void NormalizeChatFolderAssignments(uam::AppState& app) const;
	std::string FolderForNewChat(const uam::AppState& app) const;
	int CountChatsInFolder(const uam::AppState& app, const std::string& folder_id) const;
	std::string FolderTitleOrFallback(const ChatFolder& folder) const;

	int FindChatIndexById(const uam::AppState& app, const std::string& chat_id) const;
	ChatSession* SelectedChat(uam::AppState& app) const;
	const ChatSession* SelectedChat(const uam::AppState& app) const;
	void SortChatsByRecent(std::vector<ChatSession>& chats) const;
	bool ShouldReplaceChatForDuplicateId(const ChatSession& candidate, const ChatSession& existing) const;
	std::vector<ChatSession> DeduplicateChatsById(std::vector<ChatSession> chats) const;
	void RefreshRememberedSelection(uam::AppState& app) const;
	void SelectChatById(uam::AppState& app, const std::string& chat_id) const;
	ChatSession CreateNewChat(const std::string& folder_id, const std::string& provider_id) const;
	bool CreateBranchFromMessage(uam::AppState& app, const std::string& source_chat_id, int message_index) const;
	void ConsumePendingBranchRequest(uam::AppState& app) const;
	void AddMessage(ChatSession& chat, MessageRole role, const std::string& text) const;
};

#endif // UAM_APP_CHAT_DOMAIN_SERVICE_H
