#pragma once

#include <nlohmann/json.hpp>
#include <charconv>
#include <optional>
#include <string>

namespace uam::companion
{
	struct ConnectionEnvironment
	{
		std::optional<std::string> token_file, port, origin;
	};
	struct ConnectionConfiguration
	{
		bool valid = true, configured = false, enabled = false;
		std::string token_file, origin, proxy_executable, proxy_config;
		int port = 58948;
		std::string Url() const
		{
			return !valid || !configured ? std::string{} : origin.empty() ? "http://127.0.0.1:" + std::to_string(port) + "/companion" : origin + "/companion";
		}
	};

	/// Resolve once for both status publication and runtime startup. Environment token
	/// configuration is independent; port/origin overrides also apply to saved configuration.
	inline ConnectionConfiguration ResolveConnectionConfiguration(const std::optional<std::string>& persisted, const ConnectionEnvironment& environment)
	{
		ConnectionConfiguration result;
		std::string port = "58948";
		if (environment.token_file)
		{
			result.token_file = *environment.token_file;
			result.configured = !result.token_file.empty();
			result.enabled = result.configured;
		}
		else if (persisted)
		{
			const auto config = nlohmann::json::parse(*persisted, nullptr, false);
			if (!config.is_object()) { result.valid = false; return result; }
			if (config.contains("enabled") && config["enabled"].is_boolean()) result.enabled = config["enabled"].get<bool>();
			else result.valid = false;
			if (config.contains("token_file") && config["token_file"].is_string())
			{
				result.token_file = config["token_file"].get<std::string>();
				result.configured = !result.token_file.empty();
			}
			else result.valid = false;
			for (const auto& [key, target] : {std::pair{"origin", &result.origin}, {"proxy_executable", &result.proxy_executable}, {"proxy_config", &result.proxy_config}})
			{
				if (!config.contains(key)) continue;
				if (!config[key].is_string()) result.valid = false;
				else *target = config[key].get<std::string>();
			}
			if (config.contains("port") && !environment.port)
			{
				if (config["port"].is_string()) port = config["port"].get<std::string>();
				else if (config["port"].is_number_integer()) port = config["port"].dump();
				else result.valid = false;
			}
		}
		if (environment.port) port = *environment.port;
		if (environment.origin) result.origin = *environment.origin;
		const auto parsed = std::from_chars(port.data(), port.data() + port.size(), result.port);
		if (parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() || result.port < 1025 || result.port > 65535 || (result.enabled && !result.configured)) result.valid = false;
		return result;
	}
}
