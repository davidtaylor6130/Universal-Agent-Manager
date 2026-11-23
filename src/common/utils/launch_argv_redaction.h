#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace uam::launch_argv
{
	inline constexpr std::string_view kRedacted = "[private configuration]";

	/// <summary>Whether a provider launch argument carries private configuration that diagnostics and error text must omit.</summary>
	/// <remarks>Codex -c overrides carry instructions and header variable maps; MCP env values reach Codex through its
	/// environment (env_vars lists names only), and ".env=" stays covered in case a table value is ever passed. Claude
	/// --settings can carry auth keys (env, apiKeyHelper) copied from the user's settings.</remarks>
	inline bool IsPrivate(const std::vector<std::string>& argv, std::size_t index)
	{
		const std::string& argument = argv[index];
		return argument.starts_with("developer_instructions=") || argument.find(".env.") != std::string::npos ||
		       argument.find(".env=") != std::string::npos || argument.find("http_headers") != std::string::npos ||
		       argument.starts_with("--settings=") || (index > 0 && argv[index - 1] == "--settings");
	}

	/// <summary>A copy of argv with each private argument replaced by a fixed marker.</summary>
	inline std::vector<std::string> Redacted(const std::vector<std::string>& argv)
	{
		std::vector<std::string> redacted = argv;
		for (std::size_t index = 0; index < argv.size(); ++index)
			if (IsPrivate(argv, index)) redacted[index] = std::string(kRedacted);
		return redacted;
	}
}
