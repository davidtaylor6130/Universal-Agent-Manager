#include "common/provider/custom_acp/custom_acp_config.h"

#include "common/paths/path_utils.h"
#include "common/utils/env_utils.h"
#include "common/utils/hash_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <optional>
#include <set>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace uam::custom_acp
{
	namespace
	{
		constexpr int kSavedVersion = 1;

		bool HasLineBreakOrNul(std::string_view value)
		{
			return value.find_first_of(std::string_view("\r\n\0", 3)) != std::string_view::npos;
		}

		bool HasControlCharacter(std::string_view value)
		{
			return std::ranges::any_of(value, [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; });
		}

		/// Invisible or reordering characters (zero-width, bidirectional controls, no-break space) would make the
		/// shown command differ from what runs, so they are refused rather than displayed.
		bool HasMisleadingCharacter(std::string_view value)
		{
			for (std::size_t index = 0; index + 1 < value.size(); ++index)
			{
				const unsigned char first = static_cast<unsigned char>(value[index]);
				const unsigned char second = static_cast<unsigned char>(value[index + 1]);
				if (first == 0xC2 && second == 0xA0) return true; // U+00A0
				if (index + 2 >= value.size()) continue;
				const unsigned char third = static_cast<unsigned char>(value[index + 2]);
				if (first == 0xE2 && second == 0x80 && ((third >= 0x8B && third <= 0x8F) || (third >= 0xAA && third <= 0xAE) || third == 0xAF)) return true; // U+200B-200F, U+202A-202F
				if (first == 0xE2 && second == 0x81 && (third == 0xA0 || (third >= 0xA6 && third <= 0xA9))) return true; // U+2060, U+2066-2069
				if (first == 0xEF && second == 0xBB && third == 0xBF) return true; // U+FEFF
			}
			return false;
		}

		bool DisplaySafe(std::string_view value)
		{
			return !value.empty() && std::ranges::all_of(value, [](char c)
			{
				const unsigned char byte = static_cast<unsigned char>(c);
				return std::isalnum(byte) || std::string_view("_@%+=:,./-\\").find(c) != std::string_view::npos;
			});
		}

		std::string QuoteForDisplay(std::string_view value)
		{
			if (DisplaySafe(value)) return std::string(value);
			std::string quoted = "\"";
			for (const char c : value)
			{
				if (c == '"' || c == '\\' || c == '$' || c == '`') quoted.push_back('\\');
				quoted.push_back(c);
			}
			quoted.push_back('"');
			return quoted;
		}

		std::string LowerAscii(std::string_view value)
		{
			std::string lowered(value);
			std::ranges::transform(lowered, lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return lowered;
		}

		bool IsBatchScriptExtension(std::string_view extension)
		{
			const std::string lowered = LowerAscii(extension);
			return lowered == ".cmd" || lowered == ".bat";
		}

		/// Windows starts only these directly; anything else would need a shell or another program.
		bool IsWindowsProgramExtension(std::string_view extension)
		{
			const std::string lowered = LowerAscii(extension);
			return lowered == ".exe" || lowered == ".com";
		}

		bool IsRegularFile(const std::filesystem::path& path)
		{
			std::error_code error;
			return std::filesystem::is_regular_file(path, error) && !error;
		}

		bool IsExecutableFile(const std::filesystem::path& path, bool windows)
		{
			if (!IsRegularFile(path)) return false;
#if defined(_WIN32)
			(void)windows;
			return true;
#else
			// Windows rules are also exercised on other systems by tests; there, any existing file counts.
			return windows || ::access(path.c_str(), X_OK) == 0;
#endif
		}

		bool HasDirectorySeparator(std::string_view command, bool windows)
		{
			return command.find('/') != std::string_view::npos || (windows && (command.find('\\') != std::string_view::npos || command.find(':') != std::string_view::npos));
		}

		std::string NotFoundError(std::string_view command)
		{
			std::string error = "Custom ACP agent command not found: " + std::string(command) + ".";
			if (command.find(' ') != std::string_view::npos)
				return error + " Put each argument on its own line under Arguments, or use the program's full path.";
			return error + " Use the program's full path in Settings.";
		}

		std::string BatchScriptError(std::string_view command)
		{
			return "Custom ACP agent command " + std::string(command) + " is a batch script, which UAM will not run through a shell. Use the program it starts instead.";
		}

		std::mutex& ActiveMutex()
		{
			static std::mutex mutex;
			return mutex;
		}

		AgentConfig& ActiveStorage()
		{
			static AgentConfig config;
			return config;
		}

		std::string& ActiveLoadErrorStorage()
		{
			static std::string error;
			return error;
		}
	} // namespace

	std::string DisplayName(const AgentConfig& config)
	{
		const std::string trimmed = uam::strings::Trim(config.display_name);
		return trimmed.empty() ? std::string(kDefaultDisplayName) : trimmed;
	}

	bool IsConfigured(const AgentConfig& config)
	{
		return !uam::strings::Trim(config.command).empty();
	}

	AgentConfig Normalized(AgentConfig config)
	{
		config.display_name = uam::strings::Trim(config.display_name);
		config.command = uam::strings::Trim(config.command);
		for (EnvironmentVariable& variable : config.environment) variable.name = uam::strings::Trim(variable.name);
		return config;
	}

	std::string ValidationError(const AgentConfig& config)
	{
		if (config.display_name.size() > kMaxDisplayNameBytes) return "The custom ACP agent name is too long.";
		if (HasControlCharacter(config.display_name)) return "The custom ACP agent name cannot contain line breaks or control characters.";
		if (config.command.size() > kMaxCommandBytes) return "The custom ACP agent command is too long.";
		if (HasControlCharacter(config.command)) return "The custom ACP agent command cannot contain line breaks or control characters.";
		if (HasMisleadingCharacter(config.command)) return "The custom ACP agent command cannot contain invisible or direction-changing characters.";
		if (config.arguments.size() > kMaxArgumentCount) return "A custom ACP agent can have at most 64 arguments.";
		for (const std::string& argument : config.arguments)
		{
			if (argument.size() > kMaxArgumentBytes) return "A custom ACP agent argument is too long.";
			if (HasLineBreakOrNul(argument)) return "Each custom ACP agent argument must be on its own line.";
			if (HasControlCharacter(argument)) return "Custom ACP agent arguments cannot contain tabs or control characters.";
			if (HasMisleadingCharacter(argument)) return "Custom ACP agent arguments cannot contain invisible or direction-changing characters.";
		}
		if (config.environment.size() > kMaxEnvironmentCount) return "A custom ACP agent can set at most 64 environment variables.";
		std::set<std::string> names;
		for (const EnvironmentVariable& variable : config.environment)
		{
			if (variable.name.size() > kMaxEnvironmentNameBytes || !uam::env::IsVariableName(variable.name))
				return "Environment variable names must start with a letter or underscore and use only letters, digits and underscores.";
			// Windows names are case-insensitive, so refuse duplicates that only differ in case everywhere.
			if (!names.insert(LowerAscii(variable.name)).second) return "Environment variable " + variable.name + " is set more than once.";
			if (variable.value.size() > kMaxEnvironmentValueBytes) return "The value of environment variable " + variable.name + " is too long.";
			if (HasLineBreakOrNul(variable.value)) return "The value of environment variable " + variable.name + " cannot contain line breaks.";
		}
		if ((!config.approved_fingerprint.empty() && (config.approved_fingerprint.size() != 16 ||
		    config.approved_fingerprint.find_first_not_of("0123456789abcdef") != std::string::npos)) ||
		    config.approved_fingerprint.empty() != config.approved_executable.empty() || HasControlCharacter(config.approved_executable))
			return "The saved custom ACP agent approval is not valid.";
		return "";
	}

	std::vector<std::string> LaunchArgv(const AgentConfig& config)
	{
		std::vector<std::string> argv;
		argv.reserve(config.arguments.size() + 1);
		argv.push_back(config.command);
		argv.insert(argv.end(), config.arguments.begin(), config.arguments.end());
		return argv;
	}

	std::vector<std::pair<std::string, std::string>> LaunchEnvironment(const AgentConfig& config)
	{
		std::vector<std::pair<std::string, std::string>> environment;
		environment.reserve(config.environment.size());
		for (const EnvironmentVariable& variable : config.environment) environment.emplace_back(variable.name, variable.value);
		return environment;
	}

	std::string DisplayCommand(const AgentConfig& config)
	{
		std::string display;
		for (const std::string& part : LaunchArgv(config))
		{
			if (!display.empty()) display.push_back(' ');
			display += QuoteForDisplay(part);
		}
		return display;
	}

	std::string LaunchFingerprint(const AgentConfig& config)
	{
		std::uint64_t hash = uam::hashing::kFnv1a64OffsetBasis;
		const auto add = [&hash](std::string_view value)
		{
			uam::hashing::UpdateFnv1a64WithSeparator(hash, std::to_string(value.size()));
			uam::hashing::UpdateFnv1a64WithSeparator(hash, value);
		};
		add("uam-custom-acp-launch-v1");
		add(config.command);
		add(std::to_string(config.arguments.size()));
		for (const std::string& argument : config.arguments) add(argument);
		add(std::to_string(config.environment.size()));
		for (const EnvironmentVariable& variable : config.environment)
		{
			add(variable.name);
			add(variable.value);
		}
		return uam::hashing::Hex64Padded(hash);
	}

	bool IsApproved(const AgentConfig& config)
	{
		return IsConfigured(config) && !config.approved_fingerprint.empty() && config.approved_fingerprint == LaunchFingerprint(config);
	}

	AgentConfig WithApprovalCarriedOver(AgentConfig next, const AgentConfig& previous)
	{
		const bool keep = IsApproved(previous) && LaunchFingerprint(previous) == LaunchFingerprint(next);
		next.approved_fingerprint = keep ? previous.approved_fingerprint : std::string{};
		next.approved_executable = keep ? previous.approved_executable : std::string{};
		return next;
	}

	nlohmann::json ToJson(const AgentConfig& config)
	{
		nlohmann::json environment = nlohmann::json::array();
		for (const EnvironmentVariable& variable : config.environment) environment.push_back({{"name", variable.name}, {"value", variable.value}});
		return {
		    {"version", kSavedVersion},
		    {"displayName", config.display_name},
		    {"command", config.command},
		    {"arguments", config.arguments},
		    {"environment", std::move(environment)},
		    {"approvedFingerprint", config.approved_fingerprint},
		    {"approvedExecutable", config.approved_executable},
		};
	}

	bool FromJson(const nlohmann::json& value, AgentConfig& out, std::string* error)
	{
		const auto fail = [error](std::string message)
		{
			if (error != nullptr) *error = std::move(message);
			return false;
		};
		const auto string_field = [&value](const char* name, std::string& target)
		{
			const auto found = value.find(name);
			if (found == value.end()) return true;
			if (!found->is_string()) return false;
			target = found->get<std::string>();
			return true;
		};
		if (!value.is_object() || !value.contains("version") || !value["version"].is_number_integer() || value["version"].get<int>() != kSavedVersion)
			return fail("The custom ACP agent settings file is not in a format this version of UAM can read.");
		AgentConfig parsed;
		if (!string_field("displayName", parsed.display_name) || !string_field("command", parsed.command) ||
		    !string_field("approvedFingerprint", parsed.approved_fingerprint) || !string_field("approvedExecutable", parsed.approved_executable))
			return fail("The custom ACP agent settings file is damaged.");
		if (const auto arguments = value.find("arguments"); arguments != value.end())
		{
			if (!arguments->is_array()) return fail("The custom ACP agent settings file is damaged.");
			for (const nlohmann::json& argument : *arguments)
			{
				if (!argument.is_string()) return fail("The custom ACP agent settings file is damaged.");
				parsed.arguments.push_back(argument.get<std::string>());
			}
		}
		if (const auto environment = value.find("environment"); environment != value.end())
		{
			if (!environment->is_array()) return fail("The custom ACP agent settings file is damaged.");
			for (const nlohmann::json& entry : *environment)
			{
				if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string() || !entry.contains("value") || !entry["value"].is_string())
					return fail("The custom ACP agent settings file is damaged.");
				parsed.environment.push_back({entry["name"].get<std::string>(), entry["value"].get<std::string>()});
			}
		}
		parsed = Normalized(std::move(parsed));
		if (const std::string invalid = ValidationError(parsed); !invalid.empty()) return fail(invalid);
		out = std::move(parsed);
		return true;
	}

	std::filesystem::path ConfigPath(const std::filesystem::path& data_root)
	{
		return data_root / std::filesystem::path(std::string(kConfigFileName));
	}

	bool Load(const std::filesystem::path& data_root, AgentConfig& out, std::string* error)
	{
		const std::filesystem::path path = ConfigPath(data_root);
		std::error_code exists_error;
		if (!std::filesystem::exists(path, exists_error))
		{
			if (exists_error)
			{
				if (error != nullptr) *error = "The custom ACP agent settings could not be read.";
				return false;
			}
			out = {};
			return true;
		}
		std::string text;
		if (!uam::io::TryReadTextFile(path, text))
		{
			if (error != nullptr) *error = "The custom ACP agent settings could not be read.";
			return false;
		}
		const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
		if (parsed.is_discarded())
		{
			if (error != nullptr) *error = "The custom ACP agent settings file is damaged.";
			return false;
		}
		return FromJson(parsed, out, error);
	}

	bool Save(const std::filesystem::path& data_root, const AgentConfig& config, std::string* error)
	{
		if (const std::string invalid = ValidationError(config); !invalid.empty())
		{
			if (error != nullptr) *error = invalid;
			return false;
		}
		std::error_code create_error;
		std::filesystem::create_directories(data_root, create_error);
		if (create_error || !uam::io::AtomicWriteFile(ConfigPath(data_root), ToJson(config).dump(2) + "\n"))
		{
			if (error != nullptr) *error = "The custom ACP agent settings could not be saved.";
			return false;
		}
		return true;
	}

	ExecutableSearch DefaultExecutableSearch()
	{
		ExecutableSearch search;
#if defined(_WIN32)
		search.windows = true;
		constexpr char kSeparator = ';';
#else
		constexpr char kSeparator = ':';
#endif
#if defined(_WIN32)
		// The Win32 block, which the platform refreshes from the registry before launches, not the CRT's ANSI copy.
		std::optional<std::string> path;
		if (const DWORD length = ::GetEnvironmentVariableW(L"PATH", nullptr, 0); length > 0)
		{
			std::wstring wide(length, L'\0');
			const DWORD written = ::GetEnvironmentVariableW(L"PATH", wide.data(), length);
			if (written > 0 && written < length)
			{
				wide.resize(written);
				path = uam::paths::Utf8PathString(std::filesystem::path(wide));
			}
		}
#else
		const std::optional<std::string> path = uam::env::GetNonEmptyString("PATH");
#endif
		if (path)
		{
			std::size_t start = 0;
			while (start <= path->size())
			{
				const std::size_t end = path->find(kSeparator, start);
				const std::string entry = path->substr(start, end == std::string::npos ? std::string::npos : end - start);
				if (!entry.empty()) search.directories.push_back(uam::paths::PathFromUtf8(entry));
				if (end == std::string::npos) break;
				start = end + 1;
			}
		}
		return search;
	}

	std::filesystem::path ResolveExecutable(std::string_view command, const ExecutableSearch& search, std::string& error)
	{
		error.clear();
		if (command.empty())
		{
			error = "Set a command for the custom ACP agent in Settings.";
			return {};
		}
		const std::filesystem::path given = uam::paths::PathFromUtf8(command);
		const std::string extension = uam::paths::Utf8PathString(given.extension());
		if (search.windows && IsBatchScriptExtension(extension))
		{
			error = BatchScriptError(command);
			return {};
		}
		if (search.windows && given.has_extension() && !IsWindowsProgramExtension(extension))
		{
			error = "Custom ACP agent command " + std::string(command) + " is not a program UAM can start directly. Use an .exe file.";
			return {};
		}
		if (HasDirectorySeparator(command, search.windows))
		{
			if (!given.is_absolute())
			{
				error = "Custom ACP agent command " + std::string(command) + " must be a program name or a full path.";
				return {};
			}
			// Windows would look for name.exe, name.cmd and so on next to an extensionless path and run a .cmd through
			// cmd.exe, so a full path must name the program file itself.
			if (search.windows && !IsWindowsProgramExtension(extension))
			{
				error = "Custom ACP agent command " + std::string(command) + " is not a program UAM can start directly. Use the full path of its .exe file.";
				return {};
			}
			if (!IsRegularFile(given))
			{
				error = NotFoundError(command);
				return {};
			}
			if (!IsExecutableFile(given, search.windows))
			{
				error = "Custom ACP agent command is not executable: " + std::string(command) + ".";
				return {};
			}
			return given;
		}
		std::vector<std::string> names{std::string(command)};
		if (search.windows && !given.has_extension()) names = {std::string(command) + ".exe", std::string(command) + ".com"};
		bool found_batch_script = false;
		bool found_non_executable = false;
		for (const std::filesystem::path& directory : search.directories)
		{
			if (directory.empty() || !directory.is_absolute()) continue; // A relative PATH entry would depend on the chat's folder.
			for (const std::string& name : names)
			{
				const std::filesystem::path candidate = directory / uam::paths::PathFromUtf8(name);
				if (IsExecutableFile(candidate, search.windows)) return candidate;
				found_non_executable = found_non_executable || IsRegularFile(candidate);
			}
			if (search.windows && !given.has_extension())
				for (const char* script : {".cmd", ".bat"})
					found_batch_script = found_batch_script || IsRegularFile(directory / uam::paths::PathFromUtf8(std::string(command) + script));
		}
		if (found_batch_script) error = BatchScriptError(command);
		else if (found_non_executable) error = "Custom ACP agent command is not executable: " + std::string(command) + ".";
		else error = NotFoundError(command);
		return {};
	}

	std::string PrepareLaunch(const AgentConfig& config, const ExecutableSearch& search, bool remote, std::vector<std::string>& argv,
	    std::vector<std::pair<std::string, std::string>>& environment)
	{
		if (remote) return "Custom ACP agents run only on this computer for now. Choose this computer for this chat.";
		if (!IsConfigured(config)) return "Set up the custom ACP agent in Settings before starting a chat with it.";
		if (const std::string invalid = ValidationError(config); !invalid.empty()) return invalid;
		if (!IsApproved(config)) return "Review and allow the custom ACP agent's command in Settings before its first launch.";
		std::string error;
		const std::filesystem::path executable = ResolveExecutable(config.command, search, error);
		if (executable.empty()) return error;
		if (uam::paths::Utf8PathString(executable) != config.approved_executable)
			return "The custom ACP agent command now starts a different program (" + uam::paths::Utf8PathString(executable) + "). Review and allow it again in Settings.";
		argv = LaunchArgv(config);
		argv.front() = uam::paths::Utf8PathString(executable);
		const std::vector<std::pair<std::string, std::string>> configured = LaunchEnvironment(config);
		environment.insert(environment.end(), configured.begin(), configured.end());
		return "";
	}

	AgentConfig ActiveConfig()
	{
		const std::lock_guard lock(ActiveMutex());
		return ActiveStorage();
	}

	void SetActiveConfig(AgentConfig config)
	{
		const std::lock_guard lock(ActiveMutex());
		ActiveStorage() = std::move(config);
	}
	std::string ActiveLoadError()
	{
		const std::lock_guard lock(ActiveMutex());
		return ActiveLoadErrorStorage();
	}

	void SetActiveLoadError(std::string error)
	{
		const std::lock_guard lock(ActiveMutex());
		ActiveLoadErrorStorage() = std::move(error);
	}
} // namespace uam::custom_acp
