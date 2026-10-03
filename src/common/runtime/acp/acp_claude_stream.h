#pragma once

#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"

#include <array>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>

namespace uam::acp_claude_stream
{
	/// <summary>Claude's SDK control envelopes are distinct from JSON-RPC ACP messages.</summary>
	inline nlohmann::json ControlRequest(int id, nlohmann::json request)
	{
		return {{"type", "control_request"}, {"request_id", std::to_string(id)}, {"request", std::move(request)}};
	}

	inline nlohmann::json ControlResponse(const nlohmann::json& id, nlohmann::json response)
	{
		return {{"type", "control_response"}, {"response", {{"subtype", "success"}, {"request_id", id}, {"response", std::move(response)}}}};
	}
	inline constexpr const char* kMessageTypeSystem = "system";
	inline constexpr const char* kMessageTypeAssistant = "assistant";
	inline constexpr const char* kMessageTypeUser = "user";
	inline constexpr const char* kMessageTypeResult = "result";

	inline constexpr const char* kSubtypeInit = "init";
	inline constexpr const char* kSubtypeErrorDuringExecution = "error_during_execution";
	inline constexpr const char* kSubtypeErrorMaxTurns = "error_max_turns";

	inline constexpr const char* kContentThinking = "thinking";
	inline constexpr const char* kContentToolUse = "tool_use";
	inline constexpr const char* kContentToolResult = "tool_result";

	inline constexpr auto kResultErrorSubtypes = std::to_array<std::string_view>({
	    kSubtypeErrorDuringExecution,
	    kSubtypeErrorMaxTurns,
	});

	inline bool IsResultErrorSubtype(std::string_view subtype)
	{
		return uam::strings::StartsWith(subtype, "error_");
	}

	inline bool IsResultErrorSubtype(const char* subtype)
	{
		return IsResultErrorSubtype(uam::strings::ViewOrEmpty(subtype));
	}
} // namespace uam::acp_claude_stream
