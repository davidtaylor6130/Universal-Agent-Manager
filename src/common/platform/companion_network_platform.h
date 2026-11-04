#pragma once
#include <chrono>
#include <string>

namespace uam::platform
{
	// Literal IPv4 only: no DNS, credentials, application requests or certificate bypass.
	bool ProbeCompanionTcpListener(const std::string& ipv4, int port, std::chrono::milliseconds timeout);
}
