#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace uam::cli_versions
{
	// The version manager and native command dispatch must accept the same
	// decorated CLI output (for example, "opencode v1.19.4 (Homebrew)").
	inline std::optional<std::string> ExtractSemverVersion(std::string_view text)
	{
		const auto digit = [](char c) { return c >= '0' && c <= '9'; };
		for (std::size_t start = 0; start < text.size(); ++start)
		{
			if (!digit(text[start])) continue;
			std::size_t end = start;
			bool valid = true;
			for (int component = 0; component < 3; ++component)
			{
				const auto first = end;
				while (end < text.size() && digit(text[end])) ++end;
				if (end == first) { valid = false; break; }
				if (component < 2)
				{
					if (end == text.size() || text[end] != '.') { valid = false; break; }
					++end;
				}
			}
			if (valid) return std::string(text.substr(start, end - start));
		}
		return std::nullopt;
	}
}
