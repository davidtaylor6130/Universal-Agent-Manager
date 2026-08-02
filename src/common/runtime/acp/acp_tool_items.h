#pragma once

#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"

#include <array>
#include <string>
#include <string_view>

namespace uam::acp_tool_items
{
	inline constexpr const char* kCommandExecution = "commandExecution";
	inline constexpr const char* kFileChange = "fileChange";
	inline constexpr const char* kMcpToolCall = "mcpToolCall";
	inline constexpr const char* kDynamicToolCall = "dynamicToolCall";
	inline constexpr const char* kCollabAgentToolCall = "collabAgentToolCall";
	inline constexpr const char* kAgentMessage = "agentMessage";
	inline constexpr const char* kReasoning = "reasoning";
	inline constexpr const char* kPlan = "plan";
	inline constexpr const char* kUserInput = "userInput";

	inline constexpr auto kCodexToolItemTypes = std::to_array<std::string_view>({
	    kCommandExecution,
	    kFileChange,
	    kMcpToolCall,
	    kDynamicToolCall,
	    kCollabAgentToolCall,
	});

	inline constexpr auto kWholeItemContentTypes = std::to_array<std::string_view>({
	    kFileChange,
	    kMcpToolCall,
	    kDynamicToolCall,
	    kCollabAgentToolCall,
	});

	inline bool IsCodexToolItemType(std::string_view type)
	{
		return uam::ranges::Contains(kCodexToolItemTypes, uam::strings::TrimAsciiView(type));
	}

	inline bool IsCodexToolItemType(const char* type)
	{
		return IsCodexToolItemType(uam::strings::ViewOrEmpty(type));
	}

	inline bool UsesWholeItemAsContent(std::string_view type)
	{
		return uam::ranges::Contains(kWholeItemContentTypes, uam::strings::TrimAsciiView(type));
	}

	inline bool UsesWholeItemAsContent(const char* type)
	{
		return UsesWholeItemAsContent(uam::strings::ViewOrEmpty(type));
	}
} // namespace uam::acp_tool_items
