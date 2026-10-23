#include "cef/uam_bridge_request.h"

#include "common/utils/nlohmann_json_utils.h"

#include <algorithm>
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

		if (action == "sendAcpPrompt")
		{
			// Only the current envelope can supply this optional correlation hint.
			payload.erase("_uamOriginRequestId");
			const nlohmann::json* request_id = uam::nlohmann_json::FindField(root, "requestId");
			if (request_id != nullptr && request_id->is_string())
			{
				const std::string& value = request_id->get_ref<const std::string&>();
				if (!value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](unsigned char byte) { return byte >= 0x20 && byte <= 0x7e; }))
					payload["_uamOriginRequestId"] = value;
			}
		}

		BridgeRequestParseResult result;
		result.ok = true;
		result.status = kOkStatus;
		result.request.action = action;
		result.request.payload = std::move(payload);
		return result;
	}

	bool IsCompanionChatReviewRequest(const BridgeRequest& request)
	{
		if (request.action != "getVcsCommitStatus" && request.action != "getVcsFileDiff") return false;
		const nlohmann::json& payload = request.payload;
		if (!payload.is_object() || !payload.contains("chatId") || !payload["chatId"].is_string() || payload["chatId"].get_ref<const std::string&>().empty() || !payload.contains("comparisonRef") || payload["comparisonRef"] != "chat" || payload.value("vcsType", nlohmann::json("git")) != "git") return false;
		if (request.action == "getVcsCommitStatus") return true;
		if (!payload.contains("path") || !payload["path"].is_string()) return false;
		const std::string& path = payload["path"].get_ref<const std::string&>();
		if (path.empty() || path.front() == '/' || path.front() == '\\' || path.find(':') != std::string::npos || path.find('\0') != std::string::npos) return false;
		std::size_t start = 0;
		while (start < path.size())
		{
			const std::size_t end = path.find_first_of("/\\", start);
			if (path.substr(start, end == std::string::npos ? end : end - start) == "..") return false;
			if (end == std::string::npos) break;
			start = end + 1;
		}
		return true;
	}

} // namespace uam::cef
