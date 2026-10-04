#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>

namespace uam
{
	/// <summary>Reads retained terminal bytes after an absolute cursor; overflow requires a fresh attachment.</summary>
	inline std::optional<std::string_view> ReadTerminalOutputAfter(std::string_view retained, std::uint64_t total, std::uint64_t cursor)
	{
		const std::uint64_t start = total - std::min<std::uint64_t>(total, retained.size());
		if (cursor < start || cursor > total) return std::nullopt;
		return retained.substr(static_cast<std::size_t>(cursor - start));
	}
}
