#pragma once

#include "common/config/execution_host_config.h"
#include "common/config/provider_chat_defaults.h"
#include "common/utils/nlohmann_json_utils.h"

namespace uam::host_scoped_settings
{
	/// <summary>Validate an explicit save scope before any settings mutation.</summary>
	inline bool ParseScope(const AppSettings& settings, const nlohmann::json& payload, std::string& host_id, std::string& error)
	{
		host_id.clear();
		if (!payload.contains("scopeExecutionHostId")) return true;
		if (!payload["scopeExecutionHostId"].is_string())
		{
			error = "Settings scope must be a computer ID.";
			return false;
		}
		host_id = uam::strings::Trim(payload["scopeExecutionHostId"].get<std::string>());
		if (host_id.empty() || host_id == "*") { host_id.clear(); return true; }
		const ExecutionHost* host = execution_hosts::Find(settings.execution_hosts, host_id);
		if (host == nullptr || host->transport != "ssh")
		{
			error = "Settings scope must select a configured SSH computer.";
			return false;
		}
		return true;
	}

	inline const std::vector<McpServerConfiguration>& McpServers(const AppSettings& settings, std::string_view host_id)
	{
		const std::map<std::string, HostScopedSettings>::const_iterator found = settings.host_scoped_settings.find(std::string(host_id));
		return found != settings.host_scoped_settings.end() && found->second.mcp_servers ? *found->second.mcp_servers : settings.mcp_servers;
	}

	inline std::vector<McpServerConfiguration> VisibleMcpServers(const AppSettings& settings, std::string_view host_id)
	{
		std::vector<McpServerConfiguration> servers = McpServers(settings, host_id);
		std::erase_if(servers, [&](const McpServerConfiguration& server) { return !server.execution_host_id.empty() && server.execution_host_id != host_id; });
		return servers;
	}

	inline std::string DefaultProvider(const AppSettings& settings, std::string_view host_id)
	{
		const std::map<std::string, HostScopedSettings>::const_iterator found = settings.host_scoped_settings.find(std::string(host_id));
		return found != settings.host_scoped_settings.end() && found->second.default_new_chat_provider_id ? *found->second.default_new_chat_provider_id : settings.default_new_chat_provider_id;
	}
	/// <summary>Resolve only creation-time inherited fields; later explicit edits win.</summary>
	inline void ApplyInheritedRoutingDefaults(const AppSettings& settings, ChatSession& chat)
	{
		const ProviderChatDefaults defaults = provider_chat_defaults::ForProvider(settings, chat.provider_id, chat.execution_host_id);
		for (const std::string& field : chat.routing_inherited_default_fields)
		{
			if (field == "modelId" && chat.model_id.empty()) chat.model_id = defaults.model_id;
			else if (field == "reviewerModelId" && chat.reviewer_model_id.empty()) chat.reviewer_model_id = defaults.reviewer_model_id;
			else if (field == "reasoningEffort" && chat.reasoning_effort.empty()) chat.reasoning_effort = defaults.reasoning_effort;
			else if (field == "serviceTier" && chat.service_tier.empty() && !chat.service_tier_explicit && provider_ids::IsCliProviderAliasOf(chat.provider_id, provider_ids::kCodexCli))
			{ chat.service_tier = defaults.service_tier; chat.service_tier_explicit = !defaults.service_tier.empty(); }
		}
	}

}
