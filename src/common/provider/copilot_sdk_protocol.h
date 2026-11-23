#pragma once

#include "common/utils/nlohmann_json_utils.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace uam::copilot
{

/// Copilot SDK server protocols whose wire messages, permission and approval routing UAM has verified.
/// Every Copilot SDK reader (chat, model refresh, saved-image Retry, managed continuation and the
/// account reader) admits a running CLI only through this list. Add a protocol only with fixture
/// coverage of its wire messages; permission and approval routing must stay as strict as protocol 3.
inline constexpr std::array<std::int64_t, 1> kVerifiedSdkProtocols{3};

/// The protocolVersion a ping, connect or status.get result reports. Missing, non-integer and negative values are unreported.
inline std::optional<std::int64_t> ReportedSdkProtocol(const nlohmann::json& result)
{
	const nlohmann::json* value = uam::nlohmann_json::FindField(result, "protocolVersion");
	if (value == nullptr || !value->is_number_integer())
		return std::nullopt;
	if (value->is_number_unsigned())
	{
		const std::uint64_t unsigned_value = value->get<std::uint64_t>();
		if (unsigned_value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
			return std::nullopt;
		return static_cast<std::int64_t>(unsigned_value);
	}
	const std::int64_t signed_value = value->get<std::int64_t>();
	return signed_value >= 0 ? std::optional<std::int64_t>(signed_value) : std::nullopt;
}

inline bool IsVerifiedSdkProtocol(std::optional<std::int64_t> protocol)
{
	return protocol && std::find(kVerifiedSdkProtocols.begin(), kVerifiedSdkProtocols.end(), *protocol) != kVerifiedSdkProtocols.end();
}

inline bool IsVerifiedSdkProtocolResult(const nlohmann::json& result)
{
	return IsVerifiedSdkProtocol(ReportedSdkProtocol(result));
}

/// "3", "3 and 4", "3, 4 and 5".
inline std::string VerifiedSdkProtocolsText()
{
	std::string text;
	for (std::size_t index = 0; index < kVerifiedSdkProtocols.size(); ++index)
	{
		if (index > 0)
			text += index + 1 == kVerifiedSdkProtocols.size() ? " and " : ", ";
		text += std::to_string(kVerifiedSdkProtocols[index]);
	}
	return text;
}

/// One plain line naming what the running CLI reported. It never suggests updating the CLI:
/// a newer CLI is usually the cause, and only a UAM change can admit a new protocol.
inline std::string SdkProtocolRefusal(std::optional<std::int64_t> protocol)
{
	const std::string supported = "UAM supports " + VerifiedSdkProtocolsText() + ".";
	return protocol ? "This Copilot CLI uses protocol " + std::to_string(*protocol) + "; " + supported
	                : "This Copilot CLI did not report a protocol; " + supported;
}

inline std::string SdkProtocolRefusal(const nlohmann::json& result)
{
	return SdkProtocolRefusal(ReportedSdkProtocol(result));
}

}
