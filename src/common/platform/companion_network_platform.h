#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace uam::platform
{
	// Literal IPv4 only: no DNS, credentials, application requests or certificate bypass.
	bool ProbeCompanionTcpListener(const std::string& ipv4, int port, std::chrono::milliseconds timeout);

	/// True when `pid` is a descendant of `ancestor` through `parents` (pid -> parent pid), unless the chain passes
	/// through `excluded` first. Cycles and unknown parents end the walk.
	inline bool DescendsFrom(std::int64_t pid, std::int64_t ancestor, std::int64_t excluded, const std::unordered_map<std::int64_t, std::int64_t>& parents)
	{
		for (std::size_t steps = 0; steps <= parents.size(); ++steps)
		{
			if (pid == excluded) return false;
			const auto parent = parents.find(pid);
			if (parent == parents.end() || parent->second == pid || parent->second <= 0) return false;
			pid = parent->second;
			if (pid == ancestor) return true;
		}
		return false;
	}

	/// Whether the loopback TCP client on `client_port`, connected to this process's `server_port`, belongs to a
	/// process this one launched (directly or indirectly), other than `excluded_pid` and its descendants.
	/// Nullopt when the owner cannot be established (unsupported system or lookup failure).
	/// ponytail: processes that detach (reparented to init/launchd) or run outside UAM's process tree are not caught.
	std::optional<bool> LoopbackClientDescendsFromThisProcess(int client_port, int server_port, std::int64_t excluded_pid);
}
