#pragma once

#include "common/models/app_models.h"
#include "common/config/execution_host_config.h"
#include "common/provider/provider_ids.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/string_utils.h"
#include <map>
#include <nlohmann/json.hpp>

namespace uam::provider_availability
{
	inline std::string HostId(std::string_view value)
	{
		return strings::NonEmptyOrFallback(strings::Trim(value), "local");
	}

	/// <summary>Missing overrides enable the provider; host identities never share availability implicitly.</summary>
	inline bool IsEnabled(const AppSettings& settings, std::string_view provider, std::string_view host = "local")
	{
		const std::string id = provider_ids::CanonicalCliProviderLookupId(provider);
		const std::string host_id = HostId(host);
		bool enabled = true;
		for (const ProviderAvailabilityOverride& entry : settings.provider_availability)
			if (provider_ids::CanonicalCliProviderLookupId(entry.provider_id) == id && HostId(entry.execution_host_id) == host_id) enabled = entry.enabled;
		return enabled;
	}

	inline void Normalize(std::vector<ProviderAvailabilityOverride>& entries)
	{
		std::map<std::pair<std::string, std::string>, bool> unique;
		for (const ProviderAvailabilityOverride& entry : entries)
		{
			const std::string id = provider_ids::CanonicalCliProviderLookupId(entry.provider_id);
			const std::string host = HostId(entry.execution_host_id);
			if ((provider_ids::IsKnownCliProviderId(id) || provider_ids::IsRetainedExcludedProviderId(id)) && execution_hosts::IsPortableId(host)) unique[{id, host}] = entry.enabled;
		}
		entries.clear();
		for (const std::pair<const std::pair<std::string, std::string>, bool>& entry : unique)
			if (!entry.second) entries.push_back({entry.first.first, entry.first.second, false});
	}

	inline nlohmann::json Serialize(std::vector<ProviderAvailabilityOverride> entries)
	{
		Normalize(entries);
		nlohmann::json result = nlohmann::json::array();
		for (const ProviderAvailabilityOverride& entry : entries)
			result.push_back({{"providerId", entry.provider_id}, {"executionHostId", entry.execution_host_id}, {"enabled", entry.enabled}});
		return result;
	}

	/// <summary>Rejects malformed persisted policy instead of silently enabling a disabled provider. A build that excludes
	/// providers keeps their entries as inert data (IsEnabled is only asked about included providers).</summary>
	inline bool Parse(const nlohmann::json& value, std::vector<ProviderAvailabilityOverride>& result)
	{
		if (!value.is_array() || value.size() > 4096) return false;
		std::vector<ProviderAvailabilityOverride> parsed;
		for (const nlohmann::json& entry : value)
		{
			if (!entry.is_object() || !entry.contains("providerId") || !entry["providerId"].is_string() ||
			    !entry.contains("executionHostId") || !entry["executionHostId"].is_string()) return false;
			const std::optional<bool> enabled = nlohmann_json::BoolFieldStrict(entry, "enabled");
			const std::string provider = provider_ids::CanonicalCliProviderLookupId(entry["providerId"].get<std::string>());
			const std::string host = HostId(entry["executionHostId"].get<std::string>());
			if (!enabled || !(provider_ids::IsKnownCliProviderId(provider) || provider_ids::IsRetainedExcludedProviderId(provider)) || !execution_hosts::IsPortableId(host)) return false;
			parsed.push_back({provider, host, *enabled});
		}
		Normalize(parsed);
		result = std::move(parsed);
		return true;
	}

	inline bool Set(AppSettings& settings, std::string_view provider, std::string_view host, bool enabled)
	{
		const std::string id = provider_ids::CanonicalCliProviderLookupId(provider);
		const std::string host_id = HostId(host);
		if (!provider_ids::IsKnownCliProviderId(id) || !execution_hosts::IsPortableId(host_id)) return false;
		settings.provider_availability.push_back({id, host_id, enabled});
		Normalize(settings.provider_availability);
		return true;
	}

	inline std::string DisabledReason()
	{
		return "This provider is disabled on the selected host. Enable it in Settings to continue.";
	}
}
