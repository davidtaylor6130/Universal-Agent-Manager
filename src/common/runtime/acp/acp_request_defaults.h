#pragma once

#include "common/constants/app_constants.h"

#include <nlohmann/json.hpp>

#include <string>

namespace uam::acp_request_defaults
{
	inline constexpr const char* kClientName = "universal-agent-manager";
	inline const std::string kClientVersion = std::string(uam::constants::kAppVersion).substr(1);

	inline nlohmann::json ClientInfo()
	{
		return {
		    {"name", kClientName},
		    {"title", uam::constants::kAppDisplayName},
		    {"version", kClientVersion},
		};
	}
} // namespace uam::acp_request_defaults
