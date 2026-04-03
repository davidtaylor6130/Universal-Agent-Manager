#ifndef UAM_APP_PROVIDER_RESOLUTION_SERVICE_H
#define UAM_APP_PROVIDER_RESOLUTION_SERVICE_H


#include "common/state/app_state.h"

class ProviderResolutionService
{
  public:
	ProviderProfile* ActiveProvider(uam::AppState& app) const;
	const ProviderProfile* ActiveProvider(const uam::AppState& app) const;
	const ProviderProfile& ActiveProviderOrDefault(const uam::AppState& app) const;
	const ProviderProfile* ProviderForChat(const uam::AppState& app, const ChatSession& chat) const;
	const ProviderProfile& ProviderForChatOrDefault(const uam::AppState& app, const ChatSession& chat) const;
	bool ActiveProviderUsesNativeOverlayHistory(const uam::AppState& app) const;
	bool ActiveProviderUsesInternalEngine(const uam::AppState& app) const;
	bool ChatUsesNativeOverlayHistory(const uam::AppState& app, const ChatSession& chat) const;
	bool ChatUsesInternalEngine(const uam::AppState& app, const ChatSession& chat) const;
	bool ChatUsesCliOutput(const uam::AppState& app, const ChatSession& chat) const;
};

ProviderResolutionService& GetProviderResolutionService();

#endif // UAM_APP_PROVIDER_RESOLUTION_SERVICE_H
