#pragma once

#include "common/state/app_state.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace uam
{
	class UamControlService
	{
	  public:
		static bool IsStdioServerInvocation(const std::vector<std::string>& arguments);
		static bool SupportsStructuredProtocol(std::string_view protocol);
		static int RunStdioServerFromEnvironment();
		static bool Initialize(AppState& app, std::string* error_out = nullptr);
		static void Shutdown(AppState& app);
		static bool AppendSessionMcpServer(AppState& app, AcpSessionState& session,
		                                   const ChatSession& chat, std::string_view setup_method,
		                                   nlohmann::json& request, std::string* error_out = nullptr);
		static void RevokeForSession(AppState& app, AcpSessionState& session);
		static bool ProcessPendingRequests(AppState& app);

		// Pure entry point retained for focused authority and restart tests.
		static nlohmann::json HandleRequestForTests(AppState& app, const std::string& capability_id,
		                                            const nlohmann::json& request,
		                                            int64_t now_epoch_ms);
		static bool ValidStdioToolCallForTests(const nlohmann::json& request);
	};
} // namespace uam
