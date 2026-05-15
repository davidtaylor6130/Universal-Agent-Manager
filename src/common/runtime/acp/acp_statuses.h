#pragma once

#include "common/utils/string_utils.h"

#include <string>
#include <string_view>

namespace uam::acp_statuses
{
	inline constexpr const char* kPending = "pending";
	inline constexpr const char* kRunning = "running";
	inline constexpr const char* kInProgress = "in_progress";
	inline constexpr const char* kCompleted = "completed";
	inline constexpr const char* kFailed = "failed";
	inline constexpr const char* kAutoApproved = "auto_approved";

	inline bool IsFailedStatus(std::string_view status)
	{
		return uam::strings::TrimAsciiView(status) == kFailed;
	}

	inline bool IsFailedStatus(const char* status)
	{
		return IsFailedStatus(uam::strings::ViewOrEmpty(status));
	}

	inline std::string ExistingOrPending(std::string_view status)
	{
		return uam::strings::TrimOrFallback(status, kPending);
	}
} // namespace uam::acp_statuses
