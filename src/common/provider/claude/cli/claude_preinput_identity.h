#pragma once

#include "common/provider/claude_preinput_observation.h"

#include <nlohmann/json.hpp>

namespace uam::claude_preinput
{
	// Adapter-only seams, before ordinary initialize/setup/message projection.
	bool Consume(const AppState& app, AcpSessionState& session, const ChatSession& chat, const nlohmann::json& message);
	bool BeforeSharedDispatch(const AppState& app, AcpSessionState& session, const ChatSession& chat, const nlohmann::json* decoded_frame) noexcept;
}
