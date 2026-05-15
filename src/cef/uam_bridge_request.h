#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace uam::cef
{

	struct BridgeRequest
	{
		std::string action;
		nlohmann::json payload = nlohmann::json::object();
	};

	struct BridgeRequestParseResult
	{
		bool ok = false;
		int status = 400;
		std::string error;
		BridgeRequest request;
	};

	BridgeRequestParseResult ParseBridgeRequest(std::string_view raw_request);

} // namespace uam::cef
