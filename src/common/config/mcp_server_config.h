#pragma once

#include "common/models/app_models.h"
#include "common/config/execution_host_config.h"
#include "common/paths/path_utils.h"
#include "common/utils/env_utils.h"
#include "common/utils/string_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uam::mcp_server_config
{
	inline std::string WorkspaceKey(std::string_view value)
	{
		std::string portable = uam::strings::Trim(value);
		if (uam::execution_hosts::IsAbsoluteRemotePath("windows", portable))
		{
			std::ranges::replace(portable, '\\', '/');
			portable = uam::paths::PortablePathString(uam::paths::PathFromUtf8(portable).lexically_normal());
			while (portable.size() > 3 && portable.back() == '/') portable.pop_back();
			return uam::strings::ToLowerAscii(portable);
		}
		const std::filesystem::path path = uam::paths::PathFromUtf8(portable);
		if (path.empty()) return {};
		std::string key = uam::paths::NormalizedPortablePathString(uam::paths::NormalizeExistingOrAbsolutePath(path));
#if defined(_WIN32)
		key = uam::strings::ToLowerAscii(key);
#endif
		return key;
	}

	inline bool IsLoopbackUrl(std::string_view value)
	{
		const std::string url = uam::strings::TrimAndLowerAscii(value);
		if (!url.starts_with("http://") && !url.starts_with("https://")) return false;
		if (url.find('@') != std::string::npos || url.find_first_of(" \t\r\n") != std::string::npos) return false;
		const std::size_t authority_at = url.starts_with("https://") ? 8 : 7;
		const std::size_t authority_end = url.find_first_of("/?#", authority_at);
		const std::string authority = url.substr(authority_at, authority_end - authority_at);
		return authority == "localhost" || authority.starts_with("localhost:") ||
		       authority == "127.0.0.1" || authority.starts_with("127.0.0.1:") ||
		       authority == "[::1]" || authority.starts_with("[::1]:");
	}

	inline bool IsHeaderName(std::string_view value)
	{
		if (value.empty()) return false;
		for (const unsigned char c : value)
		{
			if (!(std::isalnum(c) || std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) != std::string_view::npos)) return false;
		}
		return true;
	}

	inline nlohmann::json SerializeSecretReferences(const std::vector<McpSecretReference>& references)
	{
		nlohmann::json result = nlohmann::json::array();
		for (const McpSecretReference& reference : references)
		{
			result.push_back({{"name", reference.name}, {"environmentVariable", reference.environment_variable}});
		}
		return result;
	}

	inline nlohmann::json Serialize(const std::vector<McpServerConfiguration>& servers)
	{
		nlohmann::json result = nlohmann::json::array();
		for (const McpServerConfiguration& server : servers)
		{
			result.push_back({
			    {"executionHostId", server.execution_host_id}, {"id", server.id}, {"name", server.name}, {"workspaceDirectory", server.workspace_directory},
			    {"transport", server.transport}, {"command", server.command}, {"args", server.args},
			    {"url", server.url}, {"environment", SerializeSecretReferences(server.environment)},
			    {"headers", SerializeSecretReferences(server.headers)}, {"enabled", server.enabled},
			});
		}
		return result;
	}

	inline std::vector<McpSecretReference> ParseSecretReferences(const nlohmann::json& value)
	{
		std::vector<McpSecretReference> result;
		if (!value.is_array()) return {{}};
		for (const nlohmann::json& entry : value)
		{
			if (!entry.is_object())
			{
				result.push_back({});
				continue;
			}
			const std::string name = entry.contains("name") && entry["name"].is_string() ? entry["name"].get<std::string>() : "";
			const std::string environment_variable = entry.contains("environmentVariable") && entry["environmentVariable"].is_string()
			                                             ? entry["environmentVariable"].get<std::string>() : "";
			result.push_back({uam::strings::Trim(name), uam::strings::Trim(environment_variable)});
		}
		return result;
	}

	inline std::string StringField(const nlohmann::json& value, const char* key, std::string fallback = {})
	{
		return value.contains(key) && value[key].is_string() ? value[key].get<std::string>() : std::move(fallback);
	}

	inline std::vector<McpServerConfiguration> Parse(const nlohmann::json& value)
	{
		std::vector<McpServerConfiguration> result;
		if (!value.is_array()) return result;
		for (const nlohmann::json& entry : value)
		{
			if (!entry.is_object())
			{
				result.push_back({});
				continue;
			}
			McpServerConfiguration server;
			server.execution_host_id = uam::strings::Trim(StringField(entry, "executionHostId"));
			server.id = uam::strings::Trim(StringField(entry, "id"));
			server.name = uam::strings::Trim(StringField(entry, "name"));
			server.workspace_directory = uam::strings::Trim(StringField(entry, "workspaceDirectory"));
			server.transport = uam::strings::TrimAndLowerAscii(StringField(entry, "transport", "stdio"));
			server.command = uam::strings::Trim(StringField(entry, "command"));
			server.url = uam::strings::Trim(StringField(entry, "url"));
			server.enabled = !entry.contains("enabled") || !entry["enabled"].is_boolean() || entry["enabled"].get<bool>();
			bool valid_shape = !entry.contains("enabled") || entry["enabled"].is_boolean();
			for (const char* key : {"executionHostId", "id", "name", "workspaceDirectory", "transport", "command", "url"})
			{
				if (entry.contains(key) && !entry[key].is_string()) valid_shape = false;
			}
			if (entry.contains("args") && entry["args"].is_array())
			{
				for (const nlohmann::json& arg : entry["args"])
				{
					if (arg.is_string()) server.args.push_back(arg.get<std::string>());
					else valid_shape = false;
				}
			}
			else if (entry.contains("args")) valid_shape = false;
			if (entry.contains("environment")) server.environment = ParseSecretReferences(entry["environment"]);
			if (entry.contains("headers")) server.headers = ParseSecretReferences(entry["headers"]);
			if (!valid_shape) server.id.clear();
			result.push_back(std::move(server));
		}
		return result;
	}

	inline bool NormalizeAndValidate(std::vector<McpServerConfiguration>& servers, std::string* error_out = nullptr)
	{
		std::set<std::string> ids;
		for (McpServerConfiguration& server : servers)
		{
			server.execution_host_id = uam::strings::Trim(server.execution_host_id);
			server.id = uam::strings::Trim(server.id);
			server.name = uam::strings::Trim(server.name);
			const std::filesystem::path workspace_path = uam::paths::PathFromUtf8(uam::strings::Trim(server.workspace_directory));
			if (!workspace_path.empty() && !workspace_path.is_absolute() && !uam::execution_hosts::IsAbsoluteRemotePath("windows", server.workspace_directory))
			{
				if (error_out) *error_out = "Every MCP server needs a unique id, a name, and an absolute workspace directory or an empty global scope.";
				return false;
			}
			server.workspace_directory = server.execution_host_id.empty() || server.execution_host_id == "local" ? WorkspaceKey(server.workspace_directory) : uam::strings::Trim(server.workspace_directory);
			server.transport = uam::strings::TrimAndLowerAscii(server.transport);
			server.command = uam::strings::Trim(server.command);
			server.url = uam::strings::Trim(server.url);
			if (server.id.empty() || server.name.empty() || !ids.insert(server.id).second)
			{
				if (error_out) *error_out = "Every MCP server needs a unique id, a name, and an absolute workspace directory or an empty global scope.";
				return false;
			}
			if (server.name == "uam-control" || server.name == "uam-computer")
			{
				if (error_out) *error_out = "MCP server name '" + server.name + "' is reserved by UAM.";
				return false;
			}
			if (server.transport != "stdio" && server.transport != "http" && server.transport != "sse")
			{
				if (error_out) *error_out = "MCP server '" + server.name + "' has an unsupported transport. Use stdio, http, or sse.";
				return false;
			}
			if (server.transport == "stdio")
			{
				if (server.command.empty() || (!uam::paths::PathFromUtf8(server.command).is_absolute() && !uam::execution_hosts::IsAbsoluteRemotePath("windows", server.command)))
				{
					if (error_out) *error_out = "MCP server '" + server.name + "' needs an absolute executable path.";
					return false;
				}
			}
			else if (!IsLoopbackUrl(server.url))
			{
				if (error_out) *error_out = "MCP server '" + server.name + "' must use a localhost HTTP(S) URL.";
				return false;
			}
			for (McpSecretReference& reference : server.environment)
			{
				reference.name = uam::strings::Trim(reference.name);
				reference.environment_variable = uam::strings::Trim(reference.environment_variable);
				if (!uam::env::IsVariableName(reference.name) || !uam::env::IsVariableName(reference.environment_variable))
				{
					if (error_out) *error_out = "MCP server '" + server.name + "' has an invalid environment-variable reference.";
					return false;
				}
			}
			for (McpSecretReference& reference : server.headers)
			{
				reference.name = uam::strings::Trim(reference.name);
				reference.environment_variable = uam::strings::Trim(reference.environment_variable);
				if (!IsHeaderName(reference.name) || !uam::env::IsVariableName(reference.environment_variable))
				{
					if (error_out) *error_out = "MCP server '" + server.name + "' has an invalid header environment-variable reference.";
					return false;
				}
			}
			if (server.transport == "stdio" && !server.headers.empty())
			{
				if (error_out) *error_out = "MCP stdio server '" + server.name + "' cannot use HTTP headers.";
				return false;
			}
			if (server.transport != "stdio" && (!server.command.empty() || !server.args.empty() || !server.environment.empty()))
			{
				if (error_out) *error_out = "MCP HTTP server '" + server.name + "' cannot use stdio command fields.";
				return false;
			}
		}
		return true;
	}

	/// <summary>Workspace and host overrides replace global entries by server name without duplicate connections.</summary>
	inline std::vector<McpServerConfiguration> SelectForWorkspace(const std::vector<McpServerConfiguration>& configured, std::string_view workspace_directory, std::string_view host_id = "local")
	{
		std::map<std::string, McpServerConfiguration> selected;
		for (const McpServerConfiguration& server : configured)
		{
			if (!server.enabled || (!server.execution_host_id.empty() && server.execution_host_id != host_id) || (!server.workspace_directory.empty() && WorkspaceKey(server.workspace_directory) != WorkspaceKey(workspace_directory))) continue;
			const auto previous = selected.find(server.name);
			const int specificity = (server.workspace_directory.empty() ? 0 : 2) + (server.execution_host_id.empty() ? 0 : 1);
			if (previous != selected.end())
			{
				const int previous_specificity = (previous->second.workspace_directory.empty() ? 0 : 2) + (previous->second.execution_host_id.empty() ? 0 : 1);
				if (specificity < previous_specificity) continue;
			}
			selected[server.name] = server;
		}
		std::vector<McpServerConfiguration> result;
		for (const auto& [name, server] : selected) result.push_back(server);
		return result;
	}

	inline nlohmann::json ResolveForWorkspace(const std::vector<McpServerConfiguration>& configured,
	                                          std::string_view workspace_directory,
	                                          bool http_supported, bool sse_supported,
	                                          std::string* error_out = nullptr, std::string_view execution_host_id = "local")
	{
		const std::string workspace = WorkspaceKey(workspace_directory);
		nlohmann::json result = nlohmann::json::array();
		for (const McpServerConfiguration& server : SelectForWorkspace(configured, workspace_directory, execution_host_id))
		{
			if (!server.enabled || (!server.execution_host_id.empty() && server.execution_host_id != execution_host_id) || (!server.workspace_directory.empty() && WorkspaceKey(server.workspace_directory) != workspace)) continue;
			if ((server.transport == "http" && !http_supported) || (server.transport == "sse" && !sse_supported))
			{
				if (error_out) *error_out = "The provider does not support the " + server.transport + " transport required by MCP server '" + server.name + "'.";
				return nullptr;
			}
			nlohmann::json resolved;
			resolved["name"] = server.name;
			if (server.transport == "stdio")
			{
				if (!uam::paths::IsRegularFileNoThrow(uam::paths::PathFromUtf8(server.command)))
				{
					if (error_out) *error_out = "MCP server executable is missing: " + server.command;
					return nullptr;
				}
				resolved["command"] = server.command;
				resolved["args"] = server.args;
				resolved["env"] = nlohmann::json::array();
				for (const McpSecretReference& reference : server.environment)
				{
					const std::optional<std::string> value = uam::env::GetNonEmptyString(reference.environment_variable.c_str());
					if (!value)
					{
						if (error_out) *error_out = "MCP server '" + server.name + "' needs environment variable " + reference.environment_variable + ".";
						return nullptr;
					}
					resolved["env"].push_back({{"name", reference.name}, {"value", *value}});
				}
			}
			else
			{
				resolved["type"] = server.transport;
				resolved["url"] = server.url;
				resolved["headers"] = nlohmann::json::array();
				for (const McpSecretReference& reference : server.headers)
				{
					const std::optional<std::string> value = uam::env::GetNonEmptyString(reference.environment_variable.c_str());
					if (!value)
					{
						if (error_out) *error_out = "MCP server '" + server.name + "' needs environment variable " + reference.environment_variable + ".";
						return nullptr;
					}
					resolved["headers"].push_back({{"name", reference.name}, {"value", *value}});
				}
			}
			result.push_back(std::move(resolved));
		}
		return result;
	}

	inline bool HasEnabledServerForWorkspace(const std::vector<McpServerConfiguration>& configured, std::string_view workspace_directory)
	{
		const std::string workspace = WorkspaceKey(workspace_directory);
		return std::ranges::any_of(configured, [&](const McpServerConfiguration& server) {
			return server.enabled && (server.workspace_directory.empty() || WorkspaceKey(server.workspace_directory) == workspace);
		});
	}
}
