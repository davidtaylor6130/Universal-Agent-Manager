#pragma once

#include "common/state/app_state.h"

#include <nlohmann/json_fwd.hpp>

namespace uam::compute_pools
{
	/// <summary>True while this session's turn is held at a compute pool gate.</summary>
	bool IsWaiting(const AcpSessionState& session, double now_seconds);

	/// <summary>True when the current turn is held at a gate, so its prompt has provably not been written to the provider.</summary>
	bool HeldBeforeDelivery(const AcpSessionState& session);

	/// <summary>Per-pool running and waiting counts for the settings page.</summary>
	nlohmann::json UsageForFrontend(const AppState& app);
}

namespace uam::acp_detail
{
	/// <summary>Decides whether a ready turn may be delivered. Returns false to hold it; status_changed reports a visible wait change.</summary>
	bool AdmitTurnToComputePools(AppState& app, AcpSessionState& session, const ChatSession& chat, bool& status_changed);
}
