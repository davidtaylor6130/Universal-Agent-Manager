#pragma once

#include "common/state/app_state.h"

#include <nlohmann/json_fwd.hpp>

namespace uam
{
	/// <summary>
	/// About once a second: recounts each computer's in-use and waiting chats, and keeps a background load reading
	/// for the computers that need one (live or waiting work, automatic routing to that computer, or settings that
	/// show load). Never waits for a reading. Returns true when a figure the interface shows changed.
	/// Observation only: admission does not read these figures.
	/// </summary>
	bool PollHostLoad(AppState& app);
	/// <summary>Keeps every configured computer sampled for the next 30 seconds; settings call it while they show load.</summary>
	void RequestHostLoad(AppState& app);
	/// <summary>Per-computer load, in-use, waiting and limit figures for the interface, keyed by computer ID.</summary>
	nlohmann::json HostLoadForFrontend(const AppState& app);
	/// <summary>The per-computer reasons behind the chat's current routing wait; empty when they belong to an earlier wait.</summary>
	nlohmann::json RoutingWaitHostsForFrontend(const AppState& app, const std::string& chat_id);
}
