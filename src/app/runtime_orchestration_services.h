#ifndef UAM_APP_RUNTIME_ORCHESTRATION_SERVICES_H
#define UAM_APP_RUNTIME_ORCHESTRATION_SERVICES_H


#include "common/state/app_state.h"
#include "common/platform/sdl_includes.h"

#include <string>

class TerminalSessionManager
{
  public:
	bool ForwardEscapeToSelectedTerminal(uam::AppState& p_app, const SDL_Event& p_event) const;
	void MarkSelectedTerminalForLaunch(uam::AppState& p_app) const;
	void StopAllTerminals(uam::AppState& p_app, bool p_clearIdentity = true) const;
	void FastStopTerminalsForExit(uam::AppState& p_app) const;
};

class TerminalPollingService
{
  public:
	void PollAllTerminals(uam::AppState& p_app) const;
};

class PendingRuntimeCallService
{
  public:
	void Poll(uam::AppState& p_app) const;
	bool HasPendingCallForChat(const uam::AppState& p_app, const std::string& p_chatId) const;
	bool HasAnyPendingCall(const uam::AppState& p_app) const;
	const PendingRuntimeCall* FirstPendingCallForChat(const uam::AppState& p_app, const std::string& p_chatId) const;
};

class ProviderRequestService
{
  public:
	void StartSelectedChatRequest(uam::AppState& p_app) const;
	bool QueuePromptForChat(uam::AppState& p_app,
	                        ChatSession& p_chat,
	                        const std::string& p_prompt,
	                        bool p_templateControlMessage = false) const;
};

class ChatHistorySyncService
{
  public:
	void RefreshChatHistory(uam::AppState& p_app) const;
};

#endif // UAM_APP_RUNTIME_ORCHESTRATION_SERVICES_H
