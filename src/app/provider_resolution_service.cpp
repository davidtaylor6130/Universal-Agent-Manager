#include "provider_resolution_service.h"

#include "common/provider/provider_ids.h"
#include "common/provider/provider_runtime.h"
#include "common/provider/runtime/provider_build_config.h"
#include "common/utils/string_utils.h"

#include <string>
#include <string_view>

namespace
{
	std::string CanonicalProviderId(std::string_view provider_id)
	{
		return uam::provider_ids::CanonicalCliProviderLookupId(provider_id);
	}

	const MemoryWorkerBinding* FindWorkerBindingByProviderId(const AppSettings& settings, std::string_view provider_id)
	{
		const auto found = settings.memory_worker_bindings.find(std::string(provider_id));
		return found == settings.memory_worker_bindings.end() ? nullptr : &found->second;
	}

	const MemoryWorkerBinding* FindWorkerBindingByCanonicalProviderId(const AppSettings& settings, std::string_view provider_id)
	{
		const std::string lookup_id = CanonicalProviderId(provider_id);
		if (lookup_id.empty())
		{
			return nullptr;
		}

		for (const auto& [binding_provider_id, binding] : settings.memory_worker_bindings)
		{
			if (CanonicalProviderId(binding_provider_id) == lookup_id)
			{
				return &binding;
			}
		}

		return nullptr;
	}

	const MemoryWorkerBinding* WorkerBindingForProviderId(const AppSettings& settings, std::string_view provider_id)
	{
		std::string_view preferred = uam::strings::TrimAsciiView(provider_id);
		if (preferred.empty())
		{
			return nullptr;
		}

		if (const MemoryWorkerBinding* binding = FindWorkerBindingByProviderId(settings, preferred); binding != nullptr)
		{
			return binding;
		}

		const std::string lookup_id = CanonicalProviderId(preferred);
		if (lookup_id != preferred)
		{
			if (const MemoryWorkerBinding* binding = FindWorkerBindingByProviderId(settings, lookup_id); binding != nullptr)
			{
				return binding;
			}
		}

		return FindWorkerBindingByCanonicalProviderId(settings, preferred);
	}

	template <typename Predicate>
	bool ProviderForChatMatches(const uam::AppState& app, const ChatSession& chat, Predicate predicate)
	{
		const ProviderProfile* profile = ProviderResolutionService().ProviderForChat(app, chat);
		return profile != nullptr && predicate(*profile);
	}
} // namespace

ProviderProfile* ProviderResolutionService::ActiveProvider(uam::AppState& app) const
{
	const std::string active_provider_id = CanonicalProviderId(app.settings.active_provider_id);
	ProviderProfile* found = ProviderProfileStore::FindById(app.provider_profiles, active_provider_id);

	if (found != nullptr)
	{
		app.settings.active_provider_id = active_provider_id;
		return found;
	}

	// PR-7: profiles are build-defined and reset on startup (application.cpp), so this only
	// matters as a safety net should `provider_profiles` ever be left empty.
	ProviderProfileStore::EnsureDefaultProfile(app.provider_profiles);
	app.settings.active_provider_id = provider_build_config::FirstEnabledProviderId();
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

	static const ProviderProfile fallback = []()
	{
		ProviderProfile profile;
		profile.id = provider_build_config::FirstEnabledProviderId();
		profile.title = profile.id;
		return profile;
	}();
	return fallback;
}

const ProviderProfile* ProviderResolutionService::ProviderForChat(const uam::AppState& app, const ChatSession& chat) const
{
	std::string_view preferred = uam::strings::TrimAsciiView(chat.provider_id);

	if (preferred.empty())
	{
		return ActiveProvider(app);
	}

	return ProviderProfileStore::FindById(app.provider_profiles, preferred);
}

const ProviderProfile& ProviderResolutionService::ProviderForChatOrDefault(const uam::AppState& app, const ChatSession& chat) const
{
	if (const ProviderProfile* profile = ProviderForChat(app, chat); profile != nullptr)
	{
		return *profile;
	}

	return ActiveProviderOrDefault(app);
}

ProviderResolutionService::WorkerProviderSelection ProviderResolutionService::WorkerProviderSelectionForChat(const uam::AppState& app, const ChatSession& chat) const
{
	const MemoryWorkerBinding* binding = WorkerBindingForProviderId(app.settings, chat.provider_id);
	std::string_view worker_provider_id = binding != nullptr ? uam::strings::TrimAsciiView(binding->worker_provider_id) : std::string_view{};
	if (!worker_provider_id.empty())
	{
		if (const ProviderProfile* profile = ProviderProfileStore::FindById(app.provider_profiles, worker_provider_id); profile != nullptr)
		{
			return {profile, uam::strings::Trim(binding->worker_model_id)};
		}
	}

	return {ProviderForChat(app, chat), ""};
}

const ProviderProfile* ProviderResolutionService::WorkerProviderForChat(const uam::AppState& app, const ChatSession& chat) const
{
	return WorkerProviderSelectionForChat(app, chat).provider;
}

std::string ProviderResolutionService::WorkerModelForChat(const uam::AppState& app, const ChatSession& chat) const
{
	return WorkerProviderSelectionForChat(app, chat).model_id;
}

bool ProviderResolutionService::ChatProviderIsAvailable(const uam::AppState& app, const ChatSession& chat) const
{
	return ProviderForChatMatches(app, chat, [](const ProviderProfile& profile) { return ProviderRuntime::IsRuntimeEnabled(profile); });
}

std::string ProviderResolutionService::ChatProviderUnavailableReason(const uam::AppState& app, const ChatSession& chat) const
{
	if (ChatProviderIsAvailable(app, chat))
	{
		return "";
	}

	const std::string preferred = CanonicalProviderId(chat.provider_id);
	if (preferred.empty())
	{
		return "No supported provider is available for this chat in the current build.";
	}

	return "Provider '" + preferred + "' is not supported in this build.";
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
	return ProviderForChatMatches(app, chat, [](const ProviderProfile& profile) { return ProviderRuntime::UsesNativeOverlayHistory(profile); });
}

bool ProviderResolutionService::ChatUsesInternalEngine(const uam::AppState& app, const ChatSession& chat) const
{
	return ProviderForChatMatches(app, chat, [](const ProviderProfile& profile) { return ProviderRuntime::UsesInternalEngine(profile); });
}

bool ProviderResolutionService::ChatUsesCliOutput(const uam::AppState& app, const ChatSession& chat) const
{
	return ProviderForChatMatches(app, chat, [](const ProviderProfile& profile) { return ProviderRuntime::UsesCliOutput(profile); });
}
