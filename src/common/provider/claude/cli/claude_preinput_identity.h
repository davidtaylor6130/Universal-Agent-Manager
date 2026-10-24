#pragma once

#include "common/provider/claude_preinput_observation.h"

#include <nlohmann/json.hpp>
#include <functional>

namespace uam::claude_preinput
{
	/// Process-free fixture sources only. Witnesses from this seam are explicitly synthetic
	/// and the owning production binding consumer rejects them.
	struct InjectedTransportForTests
	{
		std::function<std::string()> process;
		std::function<std::shared_ptr<const void>()> writer;
		std::function<bool(const nlohmann::json&)> write_control;
	};
	bool ArmWithInjectedTransportForTests(const AppState&, AcpSessionState&, const ChatSession&,
	    std::shared_ptr<const InjectedTransportForTests>);
	// Adapter-only seams, before ordinary initialize/setup/message projection.
	bool Consume(const AppState& app, AcpSessionState& session, const ChatSession& chat, const nlohmann::json& message);
	bool BeforeSharedDispatch(const AppState& app, AcpSessionState& session, const ChatSession& chat, const nlohmann::json* decoded_frame) noexcept;
}
