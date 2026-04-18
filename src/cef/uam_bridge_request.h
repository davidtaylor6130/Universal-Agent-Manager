#pragma once

#include <nlohmann/json.hpp>

#include <string>

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

	BridgeRequestParseResult ParseBridgeRequest(const std::string& raw_request);

} // namespace uam::cef
