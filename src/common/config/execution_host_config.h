#pragma once

#include "common/models/app_models.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace uam::execution_hosts
{
	inline constexpr std::string_view kLocalHostId = "local";

	inline ExecutionHost LocalHost()
	{
		return {};
	}

	inline bool IsPortableId(std::string_view value)
	{
		return !value.empty() && value.size() <= 64 &&
		       std::ranges::all_of(value, [](unsigned char c) {
			       return std::isalnum(c) != 0 || c == '-' || c == '_';
		       });
	}

	inline bool IsSafeSshAlias(std::string_view value)
	{
		return !value.empty() && value.size() <= 255 && value.front() != '-' &&
		       std::ranges::all_of(value, [](unsigned char c) {
			       return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.';
		       });
	}

	inline std::string Bounded(std::string_view value, std::size_t maximum)
	{
		std::string result = uam::strings::Trim(value);
		if (result.size() > maximum) result.resize(maximum);
		std::erase_if(result, [](unsigned char c) { return c < 0x20 || c == 0x7f; });
		return result;
	}

	inline bool NormalizeRemote(ExecutionHost& host, std::string* error = nullptr)
	{
		host.id = Bounded(host.id, 64);
		host.label = Bounded(host.label, 128);
		host.ssh_alias = Bounded(host.ssh_alias, 255);
		if (!IsPortableId(host.id) || host.id == kLocalHostId)
		{
			if (error != nullptr) *error = "Remote host ids must be unique portable identifiers other than 'local'.";
			return false;
		}
		if (!IsSafeSshAlias(host.ssh_alias))
		{
			if (error != nullptr) *error = "SSH hosts must use one exact alias from ~/.ssh/config.";
			return false;
		}
		if (host.label.empty()) host.label = host.ssh_alias;
		host.transport = "ssh";
		if (host.runner_status != "uninstalled" && host.runner_status != "installing" &&
		    host.runner_status != "ready" && host.runner_status != "offline" &&
		    host.runner_status != "error")
			host.runner_status = "uninstalled";
		host.runner_version = Bounded(host.runner_version, 64);
		host.platform = Bounded(host.platform, 64);
		host.architecture = Bounded(host.architecture, 64);
		host.last_seen_at = Bounded(host.last_seen_at, 64);
		return true;
	}

	inline void Normalize(std::vector<ExecutionHost>& hosts)
	{
		std::vector<ExecutionHost> normalized{LocalHost()};
		std::unordered_set<std::string> ids{std::string(kLocalHostId)};
		for (ExecutionHost host : hosts)
		{
			if (!NormalizeRemote(host) || !ids.insert(host.id).second) continue;
			normalized.push_back(std::move(host));
		}
		hosts = std::move(normalized);
	}

	inline const ExecutionHost* Find(const std::vector<ExecutionHost>& hosts, std::string_view id)
	{
		static const ExecutionHost local = LocalHost();
		if (id.empty() || id == kLocalHostId)
		{
			const auto configured = std::ranges::find(hosts, kLocalHostId, &ExecutionHost::id);
			return configured == hosts.end() ? &local : &*configured;
		}
		const auto found = std::ranges::find_if(hosts, [id](const ExecutionHost& host) {
			return host.id == id;
		});
		return found == hosts.end() ? nullptr : &*found;
	}

	inline nlohmann::json Serialize(std::vector<ExecutionHost> hosts)
	{
		Normalize(hosts);
		nlohmann::json result = nlohmann::json::array();
		for (const ExecutionHost& host : hosts)
		{
			result.push_back({
			    {"id", host.id}, {"label", host.label}, {"transport", host.transport},
			    {"sshAlias", host.ssh_alias}, {"runnerStatus", host.runner_status},
			    {"runnerVersion", host.runner_version}, {"platform", host.platform},
			    {"architecture", host.architecture}, {"lastSeenAt", host.last_seen_at},
			});
		}
		return result;
	}

	inline std::vector<ExecutionHost> Parse(const nlohmann::json& value)
	{
		std::vector<ExecutionHost> hosts;
		if (value.is_array())
		{
			for (const nlohmann::json& entry : value)
			{
				if (!entry.is_object()) continue;
				hosts.push_back({
				    entry.value("id", ""), entry.value("label", ""), entry.value("transport", "ssh"),
				    entry.value("sshAlias", ""), entry.value("runnerStatus", "uninstalled"),
				    entry.value("runnerVersion", ""), entry.value("platform", ""),
				    entry.value("architecture", ""), entry.value("lastSeenAt", ""),
				});
			}
		}
		Normalize(hosts);
		return hosts;
	}
} // namespace uam::execution_hosts
