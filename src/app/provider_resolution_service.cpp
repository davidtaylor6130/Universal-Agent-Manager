#include "provider_resolution_service.h"

#include "app/application_core_helpers.h"

#include "common/provider/provider_runtime.h"

ProviderProfile* ProviderResolutionService::ActiveProvider(uam::AppState& app) const
{
	ProviderProfile* found = ProviderProfileStore::FindById(app.provider_profiles, app.settings.active_provider_id);

	if (found != nullptr)
	{
		return found;
	}

	ProviderProfileStore::EnsureDefaultProfile(app.provider_profiles);
	app.settings.active_provider_id = "gemini-structured";
	return ProviderProfileStore::FindById(app.provider_profiles, app.settings.active_provider_id);
}

const ProviderProfile* ProviderResolutionService::ActiveProvider(const uam::AppState& app) const
{
	return ProviderProfileStore::FindById(app.provider_profiles, app.settings.active_provider_id);
}

const ProviderProfile& ProviderResolutionService::ActiveProviderOrDefault(const uam::AppState& app) const
{
	const ProviderProfile* profile = ActiveProvider(app);

	if (profile != nullptr)
	{
		return *profile;
	}

	static const ProviderProfile fallback = ProviderProfileStore::DefaultGeminiProfile();
	return fallback;
}

const ProviderProfile* ProviderResolutionService::ProviderForChat(const uam::AppState& app, const ChatSession& chat) const
{
	const std::string preferred = Trim(chat.provider_id);

	if (!preferred.empty())
	{
		if (const ProviderProfile* profile = ProviderProfileStore::FindById(app.provider_profiles, preferred); profile != nullptr)
		{
			return profile;
		}
	}

	return ActiveProvider(app);
}

const ProviderProfile& ProviderResolutionService::ProviderForChatOrDefault(const uam::AppState& app, const ChatSession& chat) const
{
	if (const ProviderProfile* profile = ProviderForChat(app, chat); profile != nullptr)
	{
		return *profile;
	}

	return ActiveProviderOrDefault(app);
}

bool ProviderResolutionService::ActiveProviderUsesNativeOverlayHistory(const uam::AppState& app) const
{
	const ProviderProfile* profile = ActiveProvider(app);
	return profile != nullptr && ProviderRuntime::UsesNativeOverlayHistory(*profile);
}

bool ProviderResolutionService::ActiveProviderUsesInternalEngine(const uam::AppState& app) const
{
	const ProviderProfile* profile = ActiveProvider(app);
	return profile != nullptr && ProviderRuntime::UsesInternalEngine(*profile);
}

bool ProviderResolutionService::ChatUsesNativeOverlayHistory(const uam::AppState& app, const ChatSession& chat) const
{
	const ProviderProfile* profile = ProviderForChat(app, chat);
	return profile != nullptr && ProviderRuntime::UsesNativeOverlayHistory(*profile);
}

bool ProviderResolutionService::ChatUsesInternalEngine(const uam::AppState& app, const ChatSession& chat) const
{
	const ProviderProfile* profile = ProviderForChat(app, chat);
	return profile != nullptr && ProviderRuntime::UsesInternalEngine(*profile);
}

bool ProviderResolutionService::ChatUsesCliOutput(const uam::AppState& app, const ChatSession& chat) const
{
	const ProviderProfile* profile = ProviderForChat(app, chat);
	return profile != nullptr && ProviderRuntime::UsesCliOutput(*profile);
}

ProviderResolutionService& GetProviderResolutionService()
{
	static ProviderResolutionService service;
	return service;
}
