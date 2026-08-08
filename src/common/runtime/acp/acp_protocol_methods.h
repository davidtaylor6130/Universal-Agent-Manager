#pragma once

#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"

#include <array>
#include <string>
#include <string_view>

namespace uam::acp_methods
{
	inline constexpr const char* kInitialize = "initialize";
	inline constexpr const char* kInitialized = "initialized";
	inline constexpr const char* kModelList = "model/list";
	inline constexpr const char* kSessionNew = "session/new";
	inline constexpr const char* kSessionLoad = "session/load";
	inline constexpr const char* kSessionPrompt = "session/prompt";
	inline constexpr const char* kSessionCancel = "session/cancel";
	inline constexpr const char* kSessionUpdate = "session/update";
	inline constexpr const char* kSessionRequestPermission = "session/request_permission";
	inline constexpr const char* kSessionSetConfigOption = "session/set_config_option";
	inline constexpr const char* kSessionSetMode = "session/set_mode";
	inline constexpr const char* kSessionSetModel = "session/set_model";
	inline constexpr const char* kThreadStart = "thread/start";
	inline constexpr const char* kThreadResume = "thread/resume";
	inline constexpr const char* kTurnStart = "turn/start";
	inline constexpr const char* kTurnInterrupt = "turn/interrupt";
	inline constexpr const char* kTurnStarted = "turn/started";
	inline constexpr const char* kTurnCompleted = "turn/completed";
	inline constexpr const char* kTurnPlanUpdated = "turn/plan/updated";
	inline constexpr const char* kThreadTokenUsageUpdated = "thread/tokenUsage/updated";
	inline constexpr const char* kAccountRateLimitsRead = "account/rateLimits/read";
	inline constexpr const char* kAccountRateLimitsUpdated = "account/rateLimits/updated";
	inline constexpr const char* kItemStarted = "item/started";
	inline constexpr const char* kItemCompleted = "item/completed";
	inline constexpr const char* kItemAgentMessageDelta = "item/agentMessage/delta";
	inline constexpr const char* kItemReasoningTextDelta = "item/reasoning/textDelta";
	inline constexpr const char* kItemReasoningSummaryTextDelta = "item/reasoning/summaryTextDelta";
	inline constexpr const char* kItemReasoningSummaryPartAdded = "item/reasoning/summaryPartAdded";
	inline constexpr const char* kItemPlanDelta = "item/plan/delta";
	inline constexpr const char* kItemCommandExecutionOutputDelta = "item/commandExecution/outputDelta";
	inline constexpr const char* kCommandExecOutputDelta = "command/exec/outputDelta";
	inline constexpr const char* kItemFileChangeOutputDelta = "item/fileChange/outputDelta";
	inline constexpr const char* kItemCommandExecutionRequestApproval = "item/commandExecution/requestApproval";
	inline constexpr const char* kItemFileChangeRequestApproval = "item/fileChange/requestApproval";
	inline constexpr const char* kItemPermissionsRequestApproval = "item/permissions/requestApproval";
	inline constexpr const char* kItemToolRequestUserInput = "item/tool/requestUserInput";
	inline constexpr const char* kError = "error";

	inline constexpr auto kLifecycleResultMethods = std::to_array<std::string_view>({
	    kInitialize,
	    kThreadStart,
	    kThreadResume,
	    kTurnStart,
	    kSessionNew,
	    kSessionLoad,
	});

	inline constexpr auto kCodexThreadSetupMethods = std::to_array<std::string_view>({
	    kThreadStart,
	    kThreadResume,
	});

	inline constexpr auto kSessionModeOrModelUpdateMethods = std::to_array<std::string_view>({
	    kSessionSetConfigOption,
	    kSessionSetMode,
	    kSessionSetModel,
	});

	inline constexpr auto kCodexItemLifecycleMethods = std::to_array<std::string_view>({
	    kItemStarted,
	    kItemCompleted,
	});

	inline constexpr auto kCodexToolOutputDeltaMethods = std::to_array<std::string_view>({
	    kItemCommandExecutionOutputDelta,
	    kCommandExecOutputDelta,
	    kItemFileChangeOutputDelta,
	});

	inline constexpr auto kIgnoredCodexAppServerMethods = std::to_array<std::string_view>({
	    "thread/started",
	    "thread/status/changed",
	    "serverRequest/resolved",
	    "thread/name/updated",
	    "configWarning",
	    "deprecationNotice",
	});

	inline bool IsLifecycleResultMethod(std::string_view method)
	{
		return uam::ranges::Contains(kLifecycleResultMethods, method);
	}

	inline bool IsLifecycleResultMethod(const char* method)
	{
		return IsLifecycleResultMethod(uam::strings::ViewOrEmpty(method));
	}

	inline bool IsCodexThreadSetupMethod(std::string_view method)
	{
		return uam::ranges::Contains(kCodexThreadSetupMethods, method);
	}

	inline bool IsCodexThreadSetupMethod(const char* method)
	{
		return IsCodexThreadSetupMethod(uam::strings::ViewOrEmpty(method));
	}

	inline bool IsSessionModeOrModelUpdateMethod(std::string_view method)
	{
		return uam::ranges::Contains(kSessionModeOrModelUpdateMethods, method);
	}

	inline bool IsSessionModeOrModelUpdateMethod(const char* method)
	{
		return IsSessionModeOrModelUpdateMethod(uam::strings::ViewOrEmpty(method));
	}

	inline bool IsCodexItemLifecycleMethod(std::string_view method)
	{
		return uam::ranges::Contains(kCodexItemLifecycleMethods, method);
	}

	inline bool IsCodexItemLifecycleMethod(const char* method)
	{
		return IsCodexItemLifecycleMethod(uam::strings::ViewOrEmpty(method));
	}

	inline bool IsCodexToolOutputDeltaMethod(std::string_view method)
	{
		return uam::ranges::Contains(kCodexToolOutputDeltaMethods, method);
	}

	inline bool IsCodexToolOutputDeltaMethod(const char* method)
	{
		return IsCodexToolOutputDeltaMethod(uam::strings::ViewOrEmpty(method));
	}

	inline bool IsCodexFileChangeOutputDeltaMethod(std::string_view method)
	{
		return method == kItemFileChangeOutputDelta;
	}

	inline bool IsCodexFileChangeOutputDeltaMethod(const char* method)
	{
		return IsCodexFileChangeOutputDeltaMethod(uam::strings::ViewOrEmpty(method));
	}

	inline bool IsIgnoredCodexAppServerMethod(std::string_view method)
	{
		return uam::ranges::Contains(kIgnoredCodexAppServerMethods, method);
	}

	inline bool IsIgnoredCodexAppServerMethod(const char* method)
	{
		return IsIgnoredCodexAppServerMethod(uam::strings::ViewOrEmpty(method));
	}
} // namespace uam::acp_methods
