#pragma once

#include "common/state/app_state.h"

class ProviderResolutionService
{
  public:
	struct WorkerProviderSelection
	{
		const ProviderProfile* provider = nullptr;
		std::string model_id;
	};

	ProviderProfile* ActiveProvider(uam::AppState& app) const;
	const ProviderProfile* ActiveProvider(const uam::AppState& app) const;
	const ProviderProfile& ActiveProviderOrDefault(const uam::AppState& app) const;
	const ProviderProfile* ProviderForChat(const uam::AppState& app, const ChatSession& chat) const;
	const ProviderProfile& ProviderForChatOrDefault(const uam::AppState& app, const ChatSession& chat) const;
	WorkerProviderSelection WorkerProviderSelectionForChat(const uam::AppState& app, const ChatSession& chat) const;
	const ProviderProfile* WorkerProviderForChat(const uam::AppState& app, const ChatSession& chat) const;
	std::string WorkerModelForChat(const uam::AppState& app, const ChatSession& chat) const;
	bool ChatProviderIsAvailable(const uam::AppState& app, const ChatSession& chat) const;
	std::string ChatProviderUnavailableReason(const uam::AppState& app, const ChatSession& chat) const;
	bool ActiveProviderUsesNativeOverlayHistory(const uam::AppState& app) const;
	bool ActiveProviderUsesInternalEngine(const uam::AppState& app) const;
	bool ChatUsesNativeOverlayHistory(const uam::AppState& app, const ChatSession& chat) const;
	bool ChatUsesInternalEngine(const uam::AppState& app, const ChatSession& chat) const;
	bool ChatUsesCliOutput(const uam::AppState& app, const ChatSession& chat) const;
};
