#pragma once

#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"

#include <array>
#include <string>
#include <string_view>

namespace uam::acp_claude_stream
{
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
		return uam::ranges::Contains(kResultErrorSubtypes, subtype);
	}

	inline bool IsResultErrorSubtype(const char* subtype)
	{
		return IsResultErrorSubtype(uam::strings::ViewOrEmpty(subtype));
	}
} // namespace uam::acp_claude_stream
