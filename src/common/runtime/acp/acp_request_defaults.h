#pragma once

#include "common/constants/app_constants.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace uam::acp_request_defaults
{
	inline constexpr const char* kClientName = "universal-agent-manager";
	inline constexpr const char* kClientVersion = "1.0.1";
	inline constexpr const char* kCodexApprovalPolicy = "on-request";
	inline constexpr const char* kCodexSandbox = "workspace-write";

	inline nlohmann::json ClientInfo()
	{
		return {
		    {"name", kClientName},
		    {"title", uam::constants::kAppDisplayName},
		    {"version", kClientVersion},
		};
	}

	inline nlohmann::json CodexThreadStartParams(std::string_view cwd)
	{
		return {
		    {"cwd", std::string(cwd)}, {"approvalPolicy", kCodexApprovalPolicy}, {"sandbox", kCodexSandbox}, {"serviceName", kClientName}, {"experimentalRawEvents", false}, {"persistExtendedHistory", true},
		};
	}

	inline nlohmann::json CodexThreadResumeParams(std::string_view thread_id, std::string_view cwd)
	{
		return {
		    {"threadId", std::string(thread_id)}, {"cwd", std::string(cwd)}, {"approvalPolicy", kCodexApprovalPolicy}, {"sandbox", kCodexSandbox}, {"persistExtendedHistory", true},
		};
	}
} // namespace uam::acp_request_defaults
