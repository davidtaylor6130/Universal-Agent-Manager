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

	/// <summary>Reads the version a CLI printed for --version. Wrappers such as bun or node can print their own
	/// version first, so only the last non-empty line counts, after removing ANSI colour codes; that line must hold
	/// exactly one x.y.z, and anything else is treated as unknown.</summary>
	inline std::optional<std::string> ExtractCliReportedVersion(std::string_view text)
	{
		std::string plain;
		plain.reserve(text.size());
		for (std::size_t index = 0; index < text.size(); ++index)
		{
			if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == '[')
			{
				// CSI sequence: parameters and intermediates, ended by a byte in @..~.
				index += 2;
				while (index < text.size() && !(text[index] >= '@' && text[index] <= '~')) ++index;
				continue;
			}
			plain.push_back(text[index]);
		}
		std::string_view line;
		for (std::string_view rest = plain; !rest.empty();)
		{
			const std::size_t end = rest.find('\n');
			std::string_view candidate = rest.substr(0, end);
			while (!candidate.empty() && (candidate.back() == '\r' || candidate.back() == ' ' || candidate.back() == '\t')) candidate.remove_suffix(1);
			if (candidate.find_first_not_of(" \t") != std::string_view::npos) line = candidate;
			if (end == std::string_view::npos) break;
			rest.remove_prefix(end + 1);
		}
		const std::optional<std::string> version = ExtractSemverVersion(line);
		if (!version) return std::nullopt;
		// A second x.y.z on the same line ("opencode 2.1.4 (bun 1.1.38)") leaves the CLI's own version ambiguous.
		const std::size_t after = line.find(*version) + version->size();
		return ExtractSemverVersion(line.substr(after)) ? std::nullopt : version;
	}
}
