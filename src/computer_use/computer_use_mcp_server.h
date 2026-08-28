#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <vector>

namespace uam::computer_use
{
	bool IsMcpServerInvocation(const std::vector<std::string>& arguments);
	int RunMcpServer(const std::vector<std::string>& arguments);
	nlohmann::json ToolDefinitionsForTests();
	nlohmann::json ActionAppliedFailureForTests(std::string message, std::string frame_id);
	nlohmann::json ObservationSuccessForTests(std::string frame_id);
	nlohmann::json WaitSuccessForTests(std::string frame_id);
} // namespace uam::computer_use
