#pragma once

#include "common/utils/string_utils.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace uam::acp_json_rpc
{
	inline constexpr const char* kVersion = "2.0";

	inline nlohmann::json Request(int id, std::string_view method, nlohmann::json params)
	{
		return {
		    {"jsonrpc", kVersion},
		    {"id", id},
		    {"method", std::string(method)},
		    {"params", std::move(params)},
		};
	}

	inline nlohmann::json Request(int id, const char* method, nlohmann::json params)
	{
		return Request(id, uam::strings::ViewOrEmpty(method), std::move(params));
	}

	inline nlohmann::json Notification(std::string_view method, nlohmann::json params = nlohmann::json::object())
	{
		nlohmann::json message = {
		    {"jsonrpc", kVersion},
		    {"method", std::string(method)},
		};
		if (!params.is_null())
		{
			message["params"] = std::move(params);
		}
		return message;
	}

	inline nlohmann::json Notification(const char* method, nlohmann::json params = nlohmann::json::object())
	{
		return Notification(uam::strings::ViewOrEmpty(method), std::move(params));
	}

	inline nlohmann::json SuccessResponse(nlohmann::json id, nlohmann::json result)
	{
		return {
		    {"jsonrpc", kVersion},
		    {"id", std::move(id)},
		    {"result", std::move(result)},
		};
	}

	inline nlohmann::json ErrorResponse(nlohmann::json id, int code, std::string_view message)
	{
		return {
		    {"jsonrpc", kVersion},
		    {"id", std::move(id)},
		    {"error",
		     {
		         {"code", code},
		         {"message", std::string(message)},
		     }},
		};
	}
} // namespace uam::acp_json_rpc
