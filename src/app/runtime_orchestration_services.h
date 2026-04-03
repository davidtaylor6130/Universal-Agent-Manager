#ifndef UAM_APP_RUNTIME_ORCHESTRATION_SERVICES_H
#define UAM_APP_RUNTIME_ORCHESTRATION_SERVICES_H


#include "common/state/app_state.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

enum class TemplatePreflightOutcome
{
	ReadyWithTemplate,
	ReadyWithoutTemplate,
	BlockingError
};

class ProviderRequestService
{
  public:
	void StartSelectedChatRequest(uam::AppState& p_app) const;
	bool QueuePromptForChat(uam::AppState& p_app,
	                        ChatSession& p_chat,
	                        const std::string& p_prompt,
	                        bool p_templateControlMessage = false) const;
	TemplatePreflightOutcome PreflightWorkspaceTemplateForChat(uam::AppState& p_app,
	                                                          const ProviderProfile& p_provider,
	                                                          const ChatSession& p_chat,
	                                                          std::string* p_bootstrapPromptOut = nullptr,
	                                                          std::string* p_statusOut = nullptr) const;
};

class ChatHistorySyncService
{
  public:
	void RefreshChatHistory(uam::AppState& p_app) const;
	void SaveChatWithStatus(uam::AppState& p_app,
	                        const ChatSession& p_chat,
	                        const std::string& p_success,
	                        const std::string& p_failure) const;
	std::vector<ChatSession> LoadNativeSessionChats(const std::filesystem::path& p_chatsDir,
	                                                const ProviderProfile& p_provider) const;
	void RefreshNativeSessionDirectory(uam::AppState& p_app) const;
	bool StartAsyncNativeChatLoad(uam::AppState& p_app) const;
	bool TryConsumeAsyncNativeChatLoad(uam::AppState& p_app,
	                                   std::vector<ChatSession>& p_chatsOut,
	                                   std::string& p_errorOut) const;
	std::vector<std::string> SessionIdsFromChats(const std::vector<ChatSession>& p_chats) const;
	std::optional<std::filesystem::path> FindNativeSessionFilePath(const std::filesystem::path& p_chatsDir,
	                                                               const std::string& p_sessionId) const;
	bool PersistLocalDraftNativeSessionLink(const uam::AppState& p_app,
	                                        ChatSession& p_localChat,
	                                        const std::string& p_nativeSessionId) const;
	std::string ResolveResumeSessionIdForChat(const uam::AppState& p_app, const ChatSession& p_chat) const;
	void ApplyLocalOverrides(uam::AppState& p_app, std::vector<ChatSession>& p_nativeChats) const;
	bool TruncateNativeSessionFromDisplayedMessage(const uam::AppState& p_app,
	                                               const ChatSession& p_chat,
	                                               int p_displayedMessageIndex,
	                                               std::string* p_errorOut) const;
};

#endif // UAM_APP_RUNTIME_ORCHESTRATION_SERVICES_H
