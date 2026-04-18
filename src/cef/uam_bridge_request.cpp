#include "cef/uam_bridge_request.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace uam::cef
{

	namespace
	{
		bool IsBlank(const std::string& value)
		{
			return std::all_of(value.begin(), value.end(), [](const unsigned char ch) { return std::isspace(ch) != 0; });
		}

		BridgeRequestParseResult Failure(const int status, std::string error)
		{
			BridgeRequestParseResult result;
			result.ok = false;
			result.status = status;
			result.error = std::move(error);
			return result;
		}
	} // namespace

	BridgeRequestParseResult ParseBridgeRequest(const std::string& raw_request)
	{
		nlohmann::json root;
		try
		{
			root = nlohmann::json::parse(raw_request);
		}
		catch (const nlohmann::json::parse_error&)
		{
			return Failure(400, "Invalid JSON request");
		}

		if (!root.is_object())
		{
			return Failure(400, "Bridge request must be a JSON object.");
		}

		const auto action_it = root.find("action");
		if (action_it == root.end() || !action_it->is_string())
		{
			return Failure(400, "Bridge request action must be a non-empty string.");
		}

		const std::string action = action_it->get<std::string>();
		if (action.empty() || IsBlank(action))
		{
			return Failure(400, "Bridge request action must be a non-empty string.");
		}

		nlohmann::json payload = nlohmann::json::object();
		const auto payload_it = root.find("payload");
		if (payload_it != root.end() && !payload_it->is_null())
		{
			if (!payload_it->is_object())
			{
				return Failure(400, "Bridge request payload must be a JSON object.");
			}

			payload = *payload_it;
		}

		BridgeRequestParseResult result;
		result.ok = true;
		result.status = 200;
		result.request.action = action;
		result.request.payload = std::move(payload);
		return result;
	}

} // namespace uam::cef
