#pragma once

#include "computer_use/computer_use_platform.h"
#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uam::computer_use
{
	bool ApplicationNamedInTask(std::string_view prompt, std::string_view application);
	bool IsMcpServerInvocation(const std::vector<std::string>& arguments);
	int RunMcpServer(const std::vector<std::string>& arguments);
	nlohmann::json ToolDefinitionsForTests();
	nlohmann::json ActionAppliedFailureForTests(std::string message, std::string frame_id);
	nlohmann::json StaleFrameFailureForTests(std::string message, std::string frame_id);
	nlohmann::json ObservationSuccessForTests(std::string frame_id, bool elements_truncated = false);
	nlohmann::json WaitSuccessForTests(std::string frame_id);
	bool ElementReferenceIsCurrentForTests(const Capture& reference, const Capture& current, int element_id);
	std::optional<std::string> ConvertPointerCoordinates(Action& action, const Capture& capture,
	                                                    const nlohmann::json& arguments);
} // namespace uam::computer_use
