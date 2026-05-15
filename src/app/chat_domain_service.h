#pragma once

#include "common/state/app_state.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class ChatDomainService
{
  public:
	struct MessageAnalytics
	{
		std::string provider;
		int64_t input_tokens = 0;
		int64_t output_chars = 0;
		int64_t time_to_first_token_ms = 0;
		int64_t processing_time_ms = 0;
		bool interrupted = false;
	};

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
	std::string ChatTitleOrFallback(const ChatSession& chat) const;

	int FindChatIndexById(const uam::AppState& app, const std::string& chat_id) const;
	ChatSession* FindChatById(uam::AppState& app, const std::string& chat_id) const;
	const ChatSession* FindChatById(const uam::AppState& app, const std::string& chat_id) const;
	ChatSession* SelectedChat(uam::AppState& app) const;
	const ChatSession* SelectedChat(const uam::AppState& app) const;
	std::string SelectedChatId(const uam::AppState& app) const;
	std::string SelectedChatProviderId(const uam::AppState& app) const;
	void SetSelectedChatIndexOrNearest(uam::AppState& app, int preferred_index) const;
	void SelectRememberedOrFirstChat(uam::AppState& app) const;
	void SortChatsByRecent(std::vector<ChatSession>& chats) const;
	bool ShouldReplaceChatForDuplicateId(const ChatSession& candidate, const ChatSession& existing) const;
	std::vector<ChatSession> DeduplicateChatsById(std::vector<ChatSession> chats) const;
	void RefreshRememberedSelection(uam::AppState& app) const;
	void SelectChatById(uam::AppState& app, const std::string& chat_id) const;
	ChatSession CreateNewChat(const std::string& folder_id, const std::string& provider_id) const;
	bool CreateBranchFromMessage(uam::AppState& app, const std::string& source_chat_id, int message_index) const;
	void AddMessage(ChatSession& chat, MessageRole role, const std::string& text) const;
	void AddMessageWithAnalytics(ChatSession& chat, MessageRole role, const std::string& text, const MessageAnalytics& analytics) const;
};
