#pragma once

#include "common/utils/string_utils.h"

#include <array>
#include <string_view>

namespace uam::sensitive
{
	inline constexpr auto kSensitiveMarkers = std::to_array<std::string_view>({
	    "api_key",
	    "api-key",
	    "apikey",
	    "access_token",
	    "authorization:",
	    "authorization=",
	    "password",
	    "private_key",
	    "private-key",
	    "refresh_token",
	    "secret",
	    "token=",
	    "token:",
	    "bearer ",
	    "-----begin ",
	});

	inline bool LooksSensitiveText(std::string_view text)
	{
		return uam::strings::ContainsAnyCaseInsensitive(text, kSensitiveMarkers);
	}
} // namespace uam::sensitive
