#pragma once

#include <nlohmann/json.hpp>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>
#include <functional>
#include <utility>

class IPlatformProcessService;

namespace uam::opencode_native
{
	struct Endpoint
	{
		int port = 0;
		std::string token;
	};
	/// Only these typed routes can reach an owned OpenCode loopback server.
	bool Query(IPlatformProcessService& service, const Endpoint& endpoint, const std::string& workspace,
	    const nlohmann::json& operation, nlohmann::json& result, std::string& error, std::stop_token cancel = {});
	bool ValidEndpoint(const Endpoint& endpoint);
	bool ValidNativeId(const std::string& id);
	bool IsNativeExecutable(const std::string& executable, const std::string& platform);
	/// Custom wrappers and explicit network policy retain basic ACP without native flags or a companion gate.
	bool SupportsCompanionLaunch(const std::vector<std::string>& argv, const std::string& platform);
	using PortProbe = std::function<bool(int)>;
	/// Target-host bind probe; an occupied observer port removes only UAM's canonical optional TUI suffix/auth.
	bool RemoveConflictingTerminalObserver(std::vector<std::string>& argv, std::vector<std::pair<std::string, std::string>>& environment,
	    const std::string& platform, PortProbe probe = {});
	/// Pure request construction, keeping credentials out of argv, files and diagnostic output.
	bool BuildRequest(const Endpoint& endpoint, const std::string& workspace, const nlohmann::json& operation,
	    std::string& url, std::string& method, std::string& config, std::string& error);
}
