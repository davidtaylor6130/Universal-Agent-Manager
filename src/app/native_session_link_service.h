#ifndef UAM_APP_NATIVE_SESSION_LINK_SERVICE_H
#define UAM_APP_NATIVE_SESSION_LINK_SERVICE_H


#include "common/state/app_state.h"

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

class NativeSessionLinkService
{
  public:
	bool IsLocalDraftChatId(const std::string& chat_id) const;
	std::optional<std::string> InferNativeSessionIdForLocalDraft(const ChatSession& local_chat,
	                                                            const std::vector<ChatSession>& native_chats) const;
	std::vector<std::string> CollectNewSessionIds(const std::vector<ChatSession>& loaded_chats,
	                                              const std::vector<std::string>& existing_ids) const;
	std::string PickFirstUnblockedSessionId(const std::vector<std::string>& candidate_ids,
	                                        const std::unordered_set<std::string>& blocked_ids) const;
	bool SessionIdExistsInLoadedChats(const std::vector<ChatSession>& loaded_chats,
	                                 const std::string& session_id) const;
};

#endif // UAM_APP_NATIVE_SESSION_LINK_SERVICE_H
