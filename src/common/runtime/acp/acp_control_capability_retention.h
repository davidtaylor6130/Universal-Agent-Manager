#pragma once

#include <cstdint>

namespace uam
{
	struct AppState;
	struct UamControlCapability;
}

namespace uam::acp_detail
{
	/// Keeps only an owned pending launch record alive; this never grants callable MCP authority.
	bool ShouldRetainPendingLocalAcpControlCapability(const AppState& app,
	    const UamControlCapability& capability, std::int64_t now_epoch_ms, double now_seconds);
}
