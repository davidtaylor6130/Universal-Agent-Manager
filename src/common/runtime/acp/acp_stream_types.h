#pragma once

#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"

#include <array>
#include <string>
#include <string_view>

namespace uam::acp_stream_types
{
	inline constexpr const char* kSessionUpdateUserMessageChunk = "user_message_chunk";
	inline constexpr const char* kSessionUpdateAgentThoughtChunk = "agent_thought_chunk";
	inline constexpr const char* kSessionUpdateAgentMessageChunk = "agent_message_chunk";
	inline constexpr const char* kSessionUpdateToolCall = "tool_call";
	inline constexpr const char* kSessionUpdateToolCallUpdate = "tool_call_update";
	inline constexpr const char* kSessionUpdatePlan = "plan";
	inline constexpr const char* kSessionUpdateCurrentMode = "current_mode_update";
	inline constexpr const char* kSessionUpdateConfigOptions = "config_option_update";

	inline constexpr const char* kSessionUpdateAvailableCommands = "available_commands_update";

	inline constexpr const char* kTurnEventAssistantText = "assistant_text";
	inline constexpr const char* kTurnEventThought = "thought";
	inline constexpr const char* kTurnEventToolCall = "tool_call";
	inline constexpr const char* kTurnEventPermissionRequest = "permission_request";
	inline constexpr const char* kTurnEventUserInputRequest = "user_input_request";
	inline constexpr const char* kTurnEventPlan = "plan";

	inline constexpr auto kToolSessionUpdateTypes = std::to_array<std::string_view>({
	    kSessionUpdateToolCall,
	    kSessionUpdateToolCallUpdate,
	});

	inline constexpr auto kTextTurnEventTypes = std::to_array<std::string_view>({
	    kTurnEventAssistantText,
	    kTurnEventThought,
	});

	inline bool IsToolSessionUpdateType(std::string_view type)
	{
		return uam::ranges::Contains(kToolSessionUpdateTypes, uam::strings::TrimAsciiView(type));
	}

	inline bool IsToolSessionUpdateType(const char* type)
	{
		return IsToolSessionUpdateType(uam::strings::ViewOrEmpty(type));
	}

	inline bool SessionUpdateTypesCompatible(std::string_view expected, std::string_view incoming)
	{
		expected = uam::strings::TrimAsciiView(expected);
		incoming = uam::strings::TrimAsciiView(incoming);
		return expected == incoming || (IsToolSessionUpdateType(expected) && IsToolSessionUpdateType(incoming));
	}

	inline bool SessionUpdateTypesCompatible(const char* expected, const char* incoming)
	{
		return SessionUpdateTypesCompatible(uam::strings::ViewOrEmpty(expected), uam::strings::ViewOrEmpty(incoming));
	}

	inline bool IsTextTurnEventType(std::string_view type)
	{
		return uam::ranges::Contains(kTextTurnEventTypes, uam::strings::TrimAsciiView(type));
	}

	inline bool IsTextTurnEventType(const char* type)
	{
		return IsTextTurnEventType(uam::strings::ViewOrEmpty(type));
	}
} // namespace uam::acp_stream_types
