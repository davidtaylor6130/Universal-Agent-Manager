#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// The one user-configured Custom ACP agent (#523): a best-effort provider that UAM starts from a command, an
/// argument list and environment pairs saved in Settings. The configuration is read only from UAM's own data folder,
/// never from a repository or chat, and the agent is launched with an argument array, never through a shell. A
/// changed launch must be allowed again in Settings, where the exact command is shown, before it can start.
namespace uam::custom_acp
{
	inline constexpr std::string_view kDefaultDisplayName = "Custom ACP agent";
	inline constexpr std::string_view kConfigFileName = "custom-acp-agent.json";
	inline constexpr std::size_t kMaxDisplayNameBytes = 80;
	inline constexpr std::size_t kMaxCommandBytes = 4096;
	inline constexpr std::size_t kMaxArgumentCount = 64;
	inline constexpr std::size_t kMaxArgumentBytes = 4096;
	inline constexpr std::size_t kMaxEnvironmentCount = 64;
	inline constexpr std::size_t kMaxEnvironmentNameBytes = 128;
	inline constexpr std::size_t kMaxEnvironmentValueBytes = 8192;

	struct EnvironmentVariable
	{
		std::string name;
		std::string value;
		bool operator==(const EnvironmentVariable&) const = default;
	};

	struct AgentConfig
	{
		std::string display_name;
		std::string command;
		std::vector<std::string> arguments;
		std::vector<EnvironmentVariable> environment;
		/// The launch fingerprint the user allowed in Settings after seeing the exact command; empty until then.
		std::string approved_fingerprint;
		bool operator==(const AgentConfig&) const = default;
	};

	/// <summary>The trimmed display name, or the default name when none is set.</summary>
	std::string DisplayName(const AgentConfig& config);
	/// <summary>True when a command is set.</summary>
	bool IsConfigured(const AgentConfig& config);
	/// <summary>One plain line describing why the configuration cannot be saved, or empty when it can.</summary>
	std::string ValidationError(const AgentConfig& config);
	/// <summary>Trims the name and command; arguments and environment values are kept exactly as typed.</summary>
	AgentConfig Normalized(AgentConfig config);

	/// <summary>The argument array UAM launches: the command followed by each argument, one element each.</summary>
	std::vector<std::string> LaunchArgv(const AgentConfig& config);
	/// <summary>The environment pairs UAM adds to the agent process.</summary>
	std::vector<std::pair<std::string, std::string>> LaunchEnvironment(const AgentConfig& config);
	/// <summary>The exact command for people to read before the first launch. Quoting here is for display only; the
	/// agent is never started from this text.</summary>
	std::string DisplayCommand(const AgentConfig& config);
	/// <summary>Identifies the command, arguments and environment, so any change needs a new approval.</summary>
	std::string LaunchFingerprint(const AgentConfig& config);
	/// <summary>True when the current launch is the one the user allowed.</summary>
	bool IsApproved(const AgentConfig& config);
	/// <summary>Keeps an earlier approval only when the launch it covered is unchanged.</summary>
	AgentConfig WithApprovalCarriedOver(AgentConfig next, const AgentConfig& previous);

	nlohmann::json ToJson(const AgentConfig& config);
	/// <summary>Parses the saved form; returns false with a one-line error for a malformed or invalid file.</summary>
	bool FromJson(const nlohmann::json& value, AgentConfig& out, std::string* error);

	std::filesystem::path ConfigPath(const std::filesystem::path& data_root);
	/// <summary>Reads the saved configuration. A missing file is an empty, unconfigured agent.</summary>
	bool Load(const std::filesystem::path& data_root, AgentConfig& out, std::string* error);
	/// <summary>Atomically replaces the saved configuration.</summary>
	bool Save(const std::filesystem::path& data_root, const AgentConfig& config, std::string* error);

	struct ExecutableSearch
	{
		std::vector<std::filesystem::path> directories;
		bool windows = false;
	};

	/// <summary>PATH from this process's environment, plus the usual package manager folders on macOS.</summary>
	ExecutableSearch DefaultExecutableSearch();
	/// <summary>Resolves the command to an executable file. On failure returns an empty path and one plain line.
	/// Windows batch scripts (.cmd, .bat) are refused, because starting them runs a shell.</summary>
	std::filesystem::path ResolveExecutable(std::string_view command, const ExecutableSearch& search, std::string& error);
	/// <summary>Checks a launch before the process starts and fills the argument array with the resolved executable.
	/// Returns one plain line when the agent cannot start.</summary>
	std::string PrepareLaunch(const AgentConfig& config, const ExecutableSearch& search, bool remote, std::vector<std::string>& argv,
	    std::vector<std::pair<std::string, std::string>>& environment);

	/// <summary>The configuration the app last loaded or saved, shared with the provider runtime.</summary>
	AgentConfig ActiveConfig();
	void SetActiveConfig(AgentConfig config);
} // namespace uam::custom_acp
