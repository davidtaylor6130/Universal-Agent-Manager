#pragma once

#include "common/utils/string_utils.h"

#include <string>
#include <string_view>

namespace uam::acp_tool_kinds
{
	inline constexpr const char* kOther = "other";

	inline std::string ExistingOrOther(std::string_view kind)
	{
		return uam::strings::TrimOrFallback(kind, kOther);
	}
} // namespace uam::acp_tool_kinds
