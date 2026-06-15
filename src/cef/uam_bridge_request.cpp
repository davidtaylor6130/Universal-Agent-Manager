#include "cef/uam_bridge_request.h"

#include "common/utils/nlohmann_json_utils.h"

#include <string_view>
#include <utility>

namespace uam::cef
{

	namespace
	{
		constexpr std::string_view kActionField = "action";
		constexpr std::string_view kPayloadField = "payload";
		constexpr int kBadRequestStatus = 400;
		constexpr int kOkStatus = 200;
		constexpr std::string_view kMissingActionError = "Bridge request action must be a non-empty string.";

		BridgeRequestParseResult Failure(int status, std::string error)
		{
			BridgeRequestParseResult result;
			result.ok = false;
			result.status = status;
			result.error = std::move(error);
			return result;
		}
	} // namespace

	BridgeRequestParseResult ParseBridgeRequest(std::string_view raw_request)
	{
		nlohmann::json root;
		try
		{
			root = nlohmann::json::parse(raw_request.begin(), raw_request.end());
		}
		catch (const nlohmann::json::parse_error&)
		{
			return Failure(kBadRequestStatus, "Invalid JSON request");
		}

		if (!root.is_object())
		{
			return Failure(kBadRequestStatus, "Bridge request must be a JSON object.");
		}

		const std::string action{uam::nlohmann_json::TrimmedStringViewOrEmpty(root, kActionField)};
		if (action.empty())
		{
			return Failure(kBadRequestStatus, std::string(kMissingActionError));
		}

		nlohmann::json payload = nlohmann::json::object();
		const nlohmann::json* payload_value = uam::nlohmann_json::FindField(root, kPayloadField);
		if (payload_value != nullptr && !payload_value->is_null())
		{
			if (!payload_value->is_object())
			{
				return Failure(kBadRequestStatus, "Bridge request payload must be a JSON object.");
			}

			payload = *payload_value;
		}

		BridgeRequestParseResult result;
		result.ok = true;
		result.status = kOkStatus;
		result.request.action = action;
		result.request.payload = std::move(payload);
		return result;
	}

} // namespace uam::cef
