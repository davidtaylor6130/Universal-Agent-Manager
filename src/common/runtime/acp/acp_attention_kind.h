#pragma once

#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"

#include <array>
#include <string>
#include <string_view>

namespace uam
{
	inline constexpr auto kAcpAttentionKinds = std::to_array<std::string_view>({
	    "question", "plan", "memory", "permission", "command", "file", "error", "generic",
	});

	inline bool IsAcpAttentionKind(std::string_view kind)
	{
		return uam::ranges::Contains(kAcpAttentionKinds, kind);
	}

	inline std::string NormalizeAcpAttentionKind(std::string_view kind, std::string_view fallback)
	{
		const std::string_view normalized = uam::strings::TrimAsciiView(kind);
		return IsAcpAttentionKind(normalized) ? std::string(normalized) : std::string(fallback);
	}
} // namespace uam
