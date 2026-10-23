#pragma once

#include "common/config/provider_chat_defaults.h"
#include "common/config/settings_normalization.h"
#include "common/provider/provider_ids.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>
#include <tuple>

namespace uam::companion
{
	inline constexpr std::string_view kWritableSettingsActions[] = {
	    "setProviderChatDefaults", "setMemorySettings", "setUamAgentPreferences",
	    "setStorageSettings", "setVcsSettings", "setProviderMcpConfiguration", "setProviderAvailability"};

	inline bool IsSettingsAction(std::string_view action)
	{
		return action == "getProviderMcpStatus" ||
		       std::find(std::begin(kWritableSettingsActions), std::end(kWritableSettingsActions), action) != std::end(kWritableSettingsActions);
	}

	inline nlohmann::json WritableSettingsActions()
	{
		nlohmann::json result = nlohmann::json::array();
		for (const std::string_view action : kWritableSettingsActions) result.push_back(action);
		return result;
	}

	namespace settings_protocol
	{
		inline nlohmann::json Pick(const nlohmann::json& source, std::initializer_list<std::string_view> keys)
		{
			nlohmann::json result = nlohmann::json::object();
			if (!source.is_object()) return result;
			for (const auto key : keys)
				if (source.contains(std::string(key))) result[std::string(key)] = source[std::string(key)];
			return result;
		}

		inline bool Keys(const nlohmann::json& object, std::initializer_list<std::string_view> allowed)
		{
			if (!object.is_object()) return false;
			for (const auto& [key, value] : object.items())
				if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) return false;
			return true;
		}

		inline bool Text(const nlohmann::json& value, std::size_t maximum = 256, bool allow_empty = false)
		{
			if (!value.is_string()) return false;
			const auto& text = value.get_ref<const std::string&>();
			return text.size() <= maximum && (allow_empty || !uam::strings::TrimAsciiView(text).empty()) &&
			       std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; });
		}

		inline bool Integer(const nlohmann::json& value, int minimum, int maximum)
		{
			return value.is_number_integer() && value >= minimum && value <= maximum;
		}

		inline bool Choice(const nlohmann::json& value, std::initializer_list<std::string_view> choices)
		{
			return value.is_string() && std::find(choices.begin(), choices.end(), value.get_ref<const std::string&>()) != choices.end();
		}

		inline bool Provider(const nlohmann::json& value)
		{
			return value.is_string() && uam::provider_ids::IsKnownCliProviderId(value.get_ref<const std::string&>());
		}

		inline bool Model(const nlohmann::json& value)
		{
			return value.is_string() && uam::provider_chat_defaults::IsAllowedModelId(value.get_ref<const std::string&>());
		}

		inline bool MemoryPair(const nlohmann::json& value, const char* enabled, const char* level, bool allow_default)
		{
			if (value.contains(enabled) && !value[enabled].is_boolean()) return false;
			if (value.contains(level) && !Choice(value[level], {"off", "readOnly", "strict", "balanced", "open"}) &&
			    !(allow_default && value[level] == "default")) return false;
			return !value.contains(enabled) || !value.contains(level) || value[enabled] == (value[level] != "off");
		}

		inline bool Defaults(const nlohmann::json& value)
		{
			if (!Keys(value, {"modelId", "reviewerModelId", "approvalMode", "commandSafetyTier", "memoryEnabled", "memoryLevel", "reasoningEffort", "serviceTier", "smallModelMode", "featurePreference", "hiddenModelIds", "hiddenProviderIds"}) || value.empty()) return false;
			for (const char* key : {"modelId", "reviewerModelId"}) if (value.contains(key) && !Model(value[key])) return false;
			if ((value.contains("approvalMode") && !Choice(value["approvalMode"], {"default", "acceptEdits", "plan"})) ||
			    (value.contains("commandSafetyTier") && !Choice(value["commandSafetyTier"], {"off", "acceptEdits", "aiReview", "yolo"})) ||
			    (value.contains("reasoningEffort") && (!value["reasoningEffort"].is_string() || (!value["reasoningEffort"].get_ref<const std::string&>().empty() && !uam::codex::IsReasoningEffort(value["reasoningEffort"].get_ref<const std::string&>())))) ||
			    (value.contains("serviceTier") && !Choice(value["serviceTier"], {"", "fast", "flex"})) ||
			    (value.contains("smallModelMode") && !value["smallModelMode"].is_boolean()) ||
			    (value.contains("featurePreference") && !Choice(value["featurePreference"], {"uam", "provider"})) ||
			    !MemoryPair(value, "memoryEnabled", "memoryLevel", true)) return false;
			for (const char* key : {"hiddenModelIds", "hiddenProviderIds"})
			{
				if (!value.contains(key)) continue;
				if (!value[key].is_array() || value[key].size() > 4096) return false;
				for (const auto& id : value[key]) if (!Text(id, 1024)) return false;
			}
			return true;
		}

		inline nlohmann::json DefaultsProjection(const nlohmann::json& source)
		{
			nlohmann::json result = nlohmann::json::object();
			if (source.is_object()) for (const auto& [id, defaults] : source.items())
				if (uam::provider_ids::IsKnownCliProviderId(id) && defaults.is_object())
					result[id] = Pick(defaults, {"modelId", "approvalMode", "commandSafetyTier", "memoryEnabled", "memoryLevel", "reasoningEffort", "serviceTier", "smallModelMode", "reviewerModelId", "featurePreference", "hiddenModelIds", "hiddenProviderIds"});
			return result;
		}

		inline nlohmann::json WorkerProjection(const nlohmann::json& source)
		{
			nlohmann::json result = nlohmann::json::object();
			if (source.is_object()) for (const auto& [id, worker] : source.items())
				if (uam::provider_ids::IsKnownCliProviderId(id) && worker.is_object()) result[id] = Pick(worker, {"workerProviderId", "workerModelId"});
			return result;
		}

		inline void CompleteMemoryPair(nlohmann::json& patch, const char* enabled, const char* level, const nlohmann::json& current, const char* fallback_level)
		{
			if (patch.contains(level)) patch[enabled] = patch[level] != "off";
			else if (patch.contains(enabled))
			{
				const std::string previous_level = current.value(level, std::string(fallback_level));
				patch[level] = patch[enabled].get<bool>() ? (previous_level == "off" ? fallback_level : previous_level) : "off";
			}
		}
	}

	inline nlohmann::json SettingsProjection(const nlohmann::json& settings)
	{
		using settings_protocol::Pick;
		nlohmann::json result = Pick(settings, {"theme", "showProviderIconsInSidebar", "showWorktreePathInSidebar", "activeProviderId", "defaultNewChatProviderId", "memoryEnabledDefault", "memoryLevelDefault", "memoryIdleDelaySeconds", "memoryRecallBudgetBytes", "goalMaxLoopIterations", "acpTurnOutputLimitMiB", "acpSetupInactivityTimeoutSeconds", "permissionReviewerProviderId", "permissionReviewerModelId", "favoriteUamAgentIds", "uamAgentCycleShortcut", "hostScopedSettings", "worktreeCleanupEnabled", "worktreeRetentionDays", "backupLogRetentionDays", "httpCacheClearIntervalDays"});
		result["providerChatDefaults"] = settings_protocol::DefaultsProjection(settings.value("providerChatDefaults", nlohmann::json::object()));
		result["memoryWorkerBindings"] = settings_protocol::WorkerProjection(settings.value("memoryWorkerBindings", nlohmann::json::object()));
		result["providerAvailability"] = nlohmann::json::array();
		if (settings.contains("providerAvailability") && settings["providerAvailability"].is_array())
			for (const auto& item : settings["providerAvailability"]) result["providerAvailability"].push_back(Pick(item, {"providerId", "executionHostId", "enabled"}));
		result["executionHosts"] = nlohmann::json::array();
		if (settings.contains("executionHosts") && settings["executionHosts"].is_array())
			for (const auto& host : settings["executionHosts"]) result["executionHosts"].push_back(Pick(host, {"id", "label", "transport", "runnerStatus", "platform", "architecture", "lastSeenAt", "customIcon", "customIconAsset"}));
		if (settings.contains("hostScopedSettingsValues") && settings["hostScopedSettingsValues"].is_object())
		{
			result["hostScopedSettingsValues"] = nlohmann::json::object();
			for (const auto& [id, scoped] : settings["hostScopedSettingsValues"].items())
			{
				result["hostScopedSettingsValues"][id] = Pick(scoped, {"defaultNewChatProviderId"});
				result["hostScopedSettingsValues"][id]["providerChatDefaults"] = settings_protocol::DefaultsProjection(scoped.value("providerChatDefaults", nlohmann::json::object()));
			}
		}
		return result;
	}

	struct SettingsPolicyResult
	{
		int status = 200;
		std::string error;
		bool Ok() const { return status == 200; }
	};

	inline SettingsPolicyResult ValidateSettingsRequest(std::string_view action, const nlohmann::json& payload)
	{
		using namespace settings_protocol;
		if (!payload.is_object() || payload.empty()) return {400, "Supply at least one supported settings field."};
		if (action == "setProviderChatDefaults")
		{
			if (!Keys(payload, {"scopeExecutionHostId", "defaultProviderId", "defaults"})) return {403, "Only provider chat defaults can be changed here."};
			if ((payload.contains("scopeExecutionHostId") && !Text(payload["scopeExecutionHostId"], 256, true)) ||
			    (payload.contains("defaultProviderId") && !Provider(payload["defaultProviderId"])) ||
			    (!payload.contains("defaultProviderId") && !payload.contains("defaults"))) return {400, "Select valid provider defaults and settings scope."};
			if (payload.contains("defaults"))
			{
				if (!payload["defaults"].is_object() || payload["defaults"].empty() || payload["defaults"].size() > 6) return {400, "Supply defaults for supported providers."};
				for (const auto& [id, defaults] : payload["defaults"].items())
					if (!uam::provider_ids::IsKnownCliProviderId(id) || !Defaults(defaults)) return {400, "Invalid provider chat default fields."};
			}
		}
		else if (action == "setMemorySettings")
		{
			if (!Keys(payload, {"enabledDefault", "levelDefault", "idleDelaySeconds", "recallBudgetBytes", "workerBindings", "permissionReviewerProviderId", "permissionReviewerModelId", "goalMaxLoopIterations", "acpTurnOutputLimitMiB", "acpSetupInactivityTimeoutSeconds"})) return {403, "Only memory, reviewer and runtime defaults can be changed here."};
			if (!MemoryPair(payload, "enabledDefault", "levelDefault", false)) return {400, "Invalid memory default selection."};
			for (const auto& bound : {std::tuple{"idleDelaySeconds", uam::settings::kMinMemoryIdleDelaySeconds, uam::settings::kMaxMemoryIdleDelaySeconds},
			                          std::tuple{"recallBudgetBytes", uam::settings::kMinMemoryRecallBudgetBytes, uam::settings::kMaxMemoryRecallBudgetBytes},
			                          std::tuple{"goalMaxLoopIterations", uam::settings::kMinGoalMaxLoopIterations, uam::settings::kMaxGoalMaxLoopIterations},
			                          std::tuple{"acpTurnOutputLimitMiB", uam::settings::kMinAcpTurnOutputLimitMiB, uam::settings::kMaxAcpTurnOutputLimitMiB},
			                          std::tuple{"acpSetupInactivityTimeoutSeconds", uam::settings::kMinAcpSetupInactivityTimeoutSeconds, uam::settings::kMaxAcpSetupInactivityTimeoutSeconds}})
				if (payload.contains(std::get<0>(bound)) && !Integer(payload[std::get<0>(bound)], std::get<1>(bound), std::get<2>(bound))) return {400, "Settings integer exceeds its supported range."};
			if ((payload.contains("permissionReviewerProviderId") && payload["permissionReviewerProviderId"] != "" && !Provider(payload["permissionReviewerProviderId"])) ||
			    (payload.contains("permissionReviewerModelId") && !Model(payload["permissionReviewerModelId"]))) return {400, "Invalid reviewer provider or model."};
			if (payload.contains("workerBindings"))
			{
				if (!payload["workerBindings"].is_object() || payload["workerBindings"].empty() || payload["workerBindings"].size() > 6) return {400, "Invalid memory worker bindings."};
				for (const auto& [id, worker] : payload["workerBindings"].items())
					if (!uam::provider_ids::IsKnownCliProviderId(id) || !Keys(worker, {"workerProviderId", "workerModelId"}) || worker.empty() ||
					    (worker.contains("workerProviderId") && !Provider(worker["workerProviderId"])) || (worker.contains("workerModelId") && !Model(worker["workerModelId"]))) return {400, "Invalid memory worker binding."};
			}
		}
		else if (action == "setUamAgentPreferences")
		{
			if (!Keys(payload, {"favoriteUamAgentIds", "uamAgentCycleShortcut"})) return {403, "Only UAM agent preferences can be changed here."};
			if (payload.contains("favoriteUamAgentIds"))
			{
				if (!payload["favoriteUamAgentIds"].is_array() || payload["favoriteUamAgentIds"].size() > uam::settings::kMaxFavoriteUamAgents) return {400, "Invalid favorite UAM agents."};
				for (const auto& id : payload["favoriteUamAgentIds"])
					if (!id.is_string() || !uam::settings::IsUamAgentId(id.get_ref<const std::string&>())) return {400, "Invalid favorite UAM agent ID."};
			}
			if (payload.contains("uamAgentCycleShortcut") && (!payload["uamAgentCycleShortcut"].is_string() || !uam::settings::IsUamAgentCycleShortcut(payload["uamAgentCycleShortcut"].get_ref<const std::string&>()))) return {400, "Unsupported UAM agent cycle shortcut."};
		}
		else if (action == "setStorageSettings")
		{
			if (!Keys(payload, {"backupLogRetentionDays", "httpCacheClearIntervalDays"})) return {403, "Only log and HTTP cache retention can be changed here."};
			for (const auto& [key, value] : payload.items()) if (!Integer(value, 0, 3650)) return {400, "Storage retention must be an integer from 0 to 3650 days."};
		}
		else if (action == "setVcsSettings")
		{
			if (!Keys(payload, {"worktreeCleanupEnabled", "worktreeRetentionDays"})) return {403, "Only worktree retention can be changed here."};
			if ((payload.contains("worktreeCleanupEnabled") && !payload["worktreeCleanupEnabled"].is_boolean()) ||
			    (payload.contains("worktreeRetentionDays") && !Integer(payload["worktreeRetentionDays"], 1, 3650))) return {400, "Invalid worktree retention policy."};
		}
		else if (action == "setProviderAvailability")
		{
			if (!Keys(payload, {"executionHostId", "providerId", "enabled"})) return {403, "Only the selected provider availability switch can be changed."};
			if (!payload.contains("executionHostId") || !Text(payload["executionHostId"]) || !payload.contains("providerId") || !Provider(payload["providerId"]) || !payload.contains("enabled") || !payload["enabled"].is_boolean()) return {400, "Select a provider, execution host and enabled boolean."};
		}
		else if (action == "getProviderMcpStatus" || action == "setProviderMcpConfiguration")
		{
			if (action == "getProviderMcpStatus" && !Keys(payload, {"executionHostId"})) return {403, "Companion MCP status accepts only an execution host ID."};
			if (action == "setProviderMcpConfiguration" && !Keys(payload, {"executionHostId", "providerId", "serverId", "enabled"})) return {403, "Only an existing per-provider MCP enabled switch can be changed."};
			if (!payload.contains("executionHostId") || !Text(payload["executionHostId"])) return {400, "Select an explicit execution host."};
			if (action == "setProviderMcpConfiguration" && (!payload.contains("providerId") || !Provider(payload["providerId"]) || !payload.contains("serverId") || !Text(payload["serverId"]) || !payload.contains("enabled") || !payload["enabled"].is_boolean())) return {400, "Select a provider, existing server and enabled boolean."};
		}
		else return {403, "Settings action unavailable to the companion."};
		return {};
	}

	/// Called immediately before synchronous handler dispatch on the UI thread, with unprojected current state.
	inline SettingsPolicyResult PrepareSettingsDispatch(std::string_view action, nlohmann::json& payload, const nlohmann::json& state)
	{
		const auto validation = ValidateSettingsRequest(action, payload);
		if (!validation.Ok()) return validation;
		if (!state.contains("settings") || !state["settings"].is_object()) return {503, "Settings are unavailable."};
		const auto& settings = state["settings"];
		if (payload.contains("executionHostId") || payload.contains("scopeExecutionHostId"))
		{
			const bool scope = payload.contains("scopeExecutionHostId");
			const std::string host = uam::strings::Trim(payload[scope ? "scopeExecutionHostId" : "executionHostId"].get<std::string>());
			payload[scope ? "scopeExecutionHostId" : "executionHostId"] = host;
			if (!(scope && (host.empty() || host == "*")))
			{
				bool found = false;
				if (settings.contains("executionHosts") && settings["executionHosts"].is_array())
					for (const auto& candidate : settings["executionHosts"])
						if (candidate.value("id", std::string{}) == host && (!scope || candidate.value("transport", std::string{}) == "ssh")) found = true;
				if (!found) return {404, "Execution host or settings scope not found."};
			}
		}
		if (action == "setUamAgentPreferences")
		{
			for (const char* key : {"favoriteUamAgentIds", "uamAgentCycleShortcut"})
			{
				if (payload.contains(key)) continue;
				if (!settings.contains(key)) return {503, "UAM agent preferences are unavailable."};
				payload[key] = settings[key];
			}
		}
		else if (action == "setVcsSettings")
		{
			for (const char* key : {"gitEnabled", "svnEnabled"})
			{
				if (!settings.contains(key) || !settings[key].is_boolean()) return {503, "Worktree settings are unavailable."};
				payload[key] = settings[key];
			}
		}
		else if (action == "setMemorySettings")
		{
			const nlohmann::json current = {{"enabledDefault", settings.value("memoryEnabledDefault", true)}, {"levelDefault", settings.value("memoryLevelDefault", std::string("strict"))}};
			settings_protocol::CompleteMemoryPair(payload, "enabledDefault", "levelDefault", current, "strict");
			if (payload.contains("workerBindings")) for (auto& [id, worker] : payload["workerBindings"].items())
			{
				const auto bindings = settings.value("memoryWorkerBindings", nlohmann::json::object());
				const auto previous = bindings.value(id, nlohmann::json::object());
				if (!worker.contains("workerProviderId"))
				{
					if (!previous.contains("workerProviderId")) return {400, "Select a memory worker provider before its model."};
					worker["workerProviderId"] = previous["workerProviderId"];
				}
				if (!worker.contains("workerModelId")) worker["workerModelId"] = previous.value("workerProviderId", std::string{}) == worker["workerProviderId"].get_ref<const std::string&>() ? previous.value("workerModelId", std::string{}) : "";
			}
		}
		else if (action == "setProviderChatDefaults" && payload.contains("defaults"))
		{
			const auto* current = &settings;
			const std::string scope = payload.value("scopeExecutionHostId", std::string{});
			if (!scope.empty() && scope != "*" && settings.contains("hostScopedSettingsValues") && settings["hostScopedSettingsValues"].contains(scope)) current = &settings["hostScopedSettingsValues"][scope];
			for (auto& [id, defaults] : payload["defaults"].items())
			{
				const auto by_provider = current->value("providerChatDefaults", nlohmann::json::object());
				settings_protocol::CompleteMemoryPair(defaults, "memoryEnabled", "memoryLevel", by_provider.value(id, nlohmann::json::object()), "default");
			}
		}
		return {};
	}

	inline nlohmann::json MobileMcpStatus(const nlohmann::json& source)
	{
		using settings_protocol::Pick;
		nlohmann::json result = Pick(source, {"executionHostId", "computerUseBuiltInAvailable", "repairMode", "connectedProofAvailable", "handshakeProofAvailable"});
		result["providers"] = nlohmann::json::array();
		if (!source.contains("providers") || !source["providers"].is_array()) return result;
		for (const auto& entry : source["providers"])
		{
			if (!entry.is_object() || !entry.contains("providerId") || !settings_protocol::Provider(entry["providerId"])) continue;
			nlohmann::json provider = Pick(entry, {"providerId", "uamControlEnabled", "sharedSetupEnabled", "activeSessionCount", "configuredSessionCount", "connectedSessionCount", "additionalServerCount", "providerSessionFailed"});
			const std::string status = entry.value("status", std::string{});
			provider["status"] = settings_protocol::Choice(status, {"disabled", "missing", "configured-unverified", "failed", "handshake-confirmed", "connected"}) ? status : "unknown";
			provider["detail"] = status == "connected" ? "The running provider MCP connection has a current heartbeat." :
			                     status == "handshake-confirmed" ? "Handshake confirmed; waiting for a current heartbeat." :
			                     status == "disabled" ? "MCP is disabled for this provider and environment." :
			                     status == "configured-unverified" ? "Configuration is available; a live connection has not been confirmed." :
			                     status == "missing" ? "Connection is unavailable; check the provider on the main device." :
			                     "Connection status is unavailable; check the main device.";
			provider["mcpServers"] = nlohmann::json::array();
			if (entry.contains("mcpServers") && entry["mcpServers"].is_array())
				for (const auto& server : entry["mcpServers"]) provider["mcpServers"].push_back(Pick(server, {"id", "name", "enabled"}));
			result["providers"].push_back(std::move(provider));
		}
		return result;
	}
}
