#pragma once

#include "common/models/app_models.h"
#include "common/provider/provider_runtime.h"
#include "common/provider/opencode_native_http.h"
#include <functional>
#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>

namespace uam
{
	struct CliTerminalState;
	struct OpenCodeTerminalActivityObserver
	{
		struct State;
		std::shared_ptr<State> state;
	};
	namespace opencode_terminal
	{
		using Query = std::function<bool(const opencode_native::Endpoint&, const std::optional<ExecutionHost>&,
		    const std::string&, const nlohmann::json&, nlohmann::json&, std::string&, std::stop_token)>;
		using ClockNow = std::function<std::chrono::steady_clock::time_point()>;
		/// Optional observation never replaces the CLI or blocks a custom terminal launch.
		void PrepareLaunch(CliTerminalState& terminal, const ChatSession& chat, const ExecutionHost& host,
		    std::vector<std::string>& argv, std::vector<std::pair<std::string, std::string>>& environment, Query query = {}, ClockNow clock = {});
		bool MatchesHost(const CliTerminalState& terminal, const ExecutionHost& host);
		ProviderTerminalActivity Poll(CliTerminalState& terminal, std::string_view native_session,
		    const std::filesystem::path& workspace, bool ambiguous);
		void Checkpoint(CliTerminalState& terminal);
	}
}
