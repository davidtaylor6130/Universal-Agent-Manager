#pragma once

#include "common/config/mcp_server_config.h"
#include "common/provider/provider_ids.h"
#include "common/provider/gemini_options.h"
#include "common/provider/provider_native_context.h"
#include "common/paths/path_utils.h"
#include "common/platform/native_user_home.h"
#include "common/utils/hash_utils.h"
#include "common/utils/io_utils.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <cctype>
#include <map>
#include <optional>
#include <set>

namespace uam::provider_setup
{
	inline constexpr std::size_t kMaxFiles = 4096;
	inline constexpr std::size_t kMaxFileBytes = 2 * 1024 * 1024;
	inline constexpr std::size_t kMaxBundleBytes = 32 * 1024 * 1024;
	using Environment = std::vector<std::pair<std::string, std::string>>;

	inline bool SafeRelativePath(const std::string& name)
	{
		if (name.empty() || name.size() > 1024 || name.find_first_of("\\:\0", 0, 3) != std::string::npos) return false;
		const std::filesystem::path path = uam::paths::PathFromUtf8(name);
		if (path.is_absolute()) return false;
		for (const std::filesystem::path& part : path)
			if (part == "." || part == ".." || part.empty()) return false;
		return name != "owner.json";
	}

	/// <summary>Check every existing path component before reading or writing managed resources.</summary>
	inline bool NoSymlinks(const std::filesystem::path& path)
	{
		std::filesystem::path current;
		for (const std::filesystem::path& part : path)
		{
			current /= part;
			std::error_code error;
			const std::filesystem::file_status status = std::filesystem::symlink_status(current, error);
			if (error && error != std::errc::no_such_file_or_directory) return false;
			if (std::filesystem::is_symlink(status) || uam::paths::IsLinkOrReparsePointNoThrow(current))
			{
#if defined(__APPLE__)
				// macOS exposes these system directories through fixed root aliases.
				if ((current == "/var" || current == "/tmp" || current == "/etc") && std::filesystem::read_symlink(current, error) == std::filesystem::path("private") / current.filename()) continue;
#endif
				return false;
			}
		}
		return true;
	}

	inline bool ReadFile(const std::filesystem::path& path, std::string& bytes, std::string& error)
	{
		std::error_code ec;
		if (!NoSymlinks(path) || !std::filesystem::is_regular_file(path, ec) || ec || std::filesystem::file_size(path, ec) > kMaxFileBytes || ec)
		{ error = "A central resource is missing, too large or a symbolic link: " + uam::paths::Utf8PathString(path); return false; }
		std::ifstream input(path, std::ios::binary);
		// Bound the read itself as well as the initial stat, including files growing during launch.
		bytes.clear();
		std::array<char, 16384> chunk;
		while (input && bytes.size() <= kMaxFileBytes)
		{
			input.read(chunk.data(), static_cast<std::streamsize>(std::min(chunk.size(), kMaxFileBytes + 1 - bytes.size())));
			bytes.append(chunk.data(), static_cast<std::size_t>(input.gcount()));
		}
		if (input.bad() || !input.eof() || bytes.size() > kMaxFileBytes) { error = "Could not read central resource: " + uam::paths::Utf8PathString(path); return false; }
		return true;
	}

	/// <summary>Publish complete files atomically; immutable bundle files accept only an identical existing value.</summary>
	inline bool WriteRuntimeFile(const std::filesystem::path& target, std::string_view bytes, std::string& error, bool replace_existing = true)
	{
		if (!NoSymlinks(target)) { error = "Central runtime path contains a symbolic link."; return false; }
		std::filesystem::path temporary;
		if (!uam::provider_native_context::WritePrivateSettings(target.parent_path(), bytes, temporary))
		{ error = "Could not stage central runtime configuration."; return false; }
		std::error_code ec;
#if defined(_WIN32)
		if (!MoveFileExW(temporary.c_str(), target.c_str(), (replace_existing ? MOVEFILE_REPLACE_EXISTING : 0) | MOVEFILE_WRITE_THROUGH))
			ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
#else
		if (replace_existing) std::filesystem::rename(temporary, target, ec);
		else if (::link(temporary.c_str(), target.c_str()) != 0) ec = std::error_code(errno, std::generic_category());
#endif
		if (ec)
		{
			// Windows may deny replacement while a concurrent reader holds the identical published file.
			std::string published;
			std::string read_error;
			const bool already_published = ReadFile(target, published, read_error) && published == bytes;
			const std::string publication_error = ec.message();
			std::filesystem::remove(temporary, ec);
			if (already_published) return true;
			error = "Could not publish central runtime configuration: " + publication_error;
			return false;
		}
		if (!replace_existing) std::filesystem::remove(temporary, ec);
		return true;
	}

	inline std::string Digest(std::string_view bytes) { return uam::hashing::Hex64Padded(uam::hashing::Fnv1a64(bytes)); }

	inline bool ValidateManifest(const nlohmann::json& manifest, std::string& error)
	{
		if (!manifest.is_object() || !manifest.contains("format") || !manifest["format"].is_number_integer() || manifest["format"] != 1 || !manifest.contains("files") || !manifest["files"].is_object() || manifest["files"].size() > kMaxFiles)
		{ error = "Central resource manifest is invalid."; return false; }
		if (manifest.contains("mcpOnly") && !manifest["mcpOnly"].is_boolean()) { error = "MCP-only metadata must be boolean."; return false; }
		if (manifest.contains("openCodeCodeMode") && !manifest["openCodeCodeMode"].is_boolean()) { error = "OpenCode discovery metadata must be boolean."; return false; }
		if (manifest.contains("openCodeUamControlDeadline") && !manifest["openCodeUamControlDeadline"].is_boolean()) { error = "OpenCode UAM control deadline metadata must be boolean."; return false; }
		if (manifest.contains("claudeTerminalFastMode") && !manifest["claudeTerminalFastMode"].is_boolean()) { error = "Claude terminal fast-mode metadata must be boolean."; return false; }
		if (manifest.contains("geminiThinking"))
		{
			const auto& thinking = manifest["geminiThinking"];
			if (!thinking.is_object() || thinking.size() != 3 || !thinking.contains("model") || !thinking["model"].is_string() ||
			    !thinking.contains("effort") || !thinking["effort"].is_string() || !thinking.contains("cliVersion") || !thinking["cliVersion"].is_string() ||
			    !uam::gemini::SupportsReasoningEffort(thinking["model"].get<std::string>(), thinking["effort"].get<std::string>(), thinking["cliVersion"].get<std::string>()))
			{ error = "Gemini thinking metadata requires a verified concrete model, CLI version and supported named level."; return false; }
		}
		if (manifest.dump().size() > 512 * 1024) { error = "Central resource manifest exceeds 512 KiB."; return false; }
		std::size_t total = 0;
		for (const auto& [name, entry] : manifest["files"].items())
		{
			if (!SafeRelativePath(name) || !entry.is_object() || !entry.contains("size") || !entry["size"].is_number_unsigned() || !entry.contains("digest") || !entry["digest"].is_string())
			{ error = "Central resource manifest contains an invalid file."; return false; }
			const std::uint64_t size = entry["size"].get<std::uint64_t>();
			const std::string digest = entry["digest"].get<std::string>();
			if (entry.contains("executable") && !entry["executable"].is_boolean()) { error = "Central executable metadata is invalid."; return false; }
			if (size > kMaxFileBytes || digest.size() != 16 || digest.find_first_not_of("0123456789abcdef") != std::string::npos)
			{ error = "Central resource file is too large or has an invalid checksum."; return false; }
			total += size;
		}
		if ((manifest.contains("hostInstructionFile") && !manifest["hostInstructionFile"].is_string()) || (manifest.contains("mcpServers") && !manifest["mcpServers"].is_array()))
		{ error = "Central host instructions or MCP configuration are invalid."; return false; }
		if (total > kMaxBundleBytes) { error = "Central resources exceed 32 MiB."; return false; }
		return true;
	}

	/// <summary>Immutable owned directories allow interrupted SSH uploads to resume without overwriting user files.</summary>
	inline bool Prepare(const std::filesystem::path& directory, const nlohmann::json& manifest, std::vector<std::string>& missing, std::string& error)
	{
		if (!ValidateManifest(manifest, error)) return false;
		if (!directory.is_absolute() || !NoSymlinks(directory) || directory.filename() != Digest(manifest.dump()))
		{ error = "Central resource directory does not match its manifest."; return false; }
		std::error_code ec;
		std::filesystem::create_directories(directory, ec);
		if (ec) { error = "Could not create central resource directory."; return false; }
		const std::filesystem::path owner_path = directory / "owner.json";
		const std::string serialized = manifest.dump();
		if (!std::filesystem::exists(owner_path, ec))
		{
			// A stopped initializer may leave an empty directory or private staging files.
			bool unowned_entries = false;
			for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory, ec))
			{
				const std::string name = uam::paths::Utf8PathString(entry.path().filename());
				if (name != "owner.json" && !(name.starts_with("context-settings-") && name.ends_with(".json"))) unowned_entries = true;
			}
			if (ec) { error = "Could not inspect central resource ownership."; return false; }
			// Another process may have published ownership and resources during inspection.
			if (!std::filesystem::exists(owner_path, ec))
			{
				if (ec || unowned_entries) { error = "Central resource directory belongs to different data."; return false; }
				std::filesystem::permissions(directory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec);
				if (ec) { error = "Could not secure central resource directory: " + ec.message(); return false; }
				if (!WriteRuntimeFile(owner_path, serialized, error, false)) return false;
			}
		}
		std::string owner;
		if (!ReadFile(owner_path, owner, error) || owner != serialized)
		{ error = "Central resource directory belongs to different data."; return false; }

		missing.clear();
		for (const auto& [name, entry] : manifest["files"].items())
		{
			const std::filesystem::path path = directory / uam::paths::PathFromUtf8(name);
			if (!NoSymlinks(path)) { error = "Central resource path contains a symbolic link."; return false; }
			if (!std::filesystem::exists(path, ec)) { missing.push_back(name); continue; }
			std::string bytes;
			if (!ReadFile(path, bytes, error) || bytes.size() != entry["size"].get<std::size_t>() || Digest(bytes) != entry["digest"].get<std::string>())
			{ error = "Installed central resource differs from its manifest: " + name; return false; }
		}
		return true;
	}

	inline std::string EnvironmentValue(const Environment& environment, const std::string& name)
	{
		for (Environment::const_reverse_iterator entry = environment.rbegin(); entry != environment.rend(); ++entry)
			if (entry->first == name) return entry->second;
		return uam::env::GetNonEmptyString(name.c_str()).value_or("");
	}

	inline void SetEnvironment(Environment& environment, const std::string& name, const std::string& value)
	{
		std::erase_if(environment, [&](const std::pair<std::string, std::string>& entry) { return entry.first == name; });
		environment.emplace_back(name, value);
	}

	inline std::string ReplaceRoot(std::string text, const std::string& root)
	{
		std::size_t position = 0;
		while ((position = text.find("__UAM_RESOURCE_ROOT__", position)) != std::string::npos)
		{
			text.replace(position, std::string_view("__UAM_RESOURCE_ROOT__").size(), root);
			position += root.size();
		}
		return text;
	}

	/// <summary>Resolve native-owned configuration paths, bounding bytes while permitting ordinary dotfile/mount links.</summary>
	inline bool ReadNativeSettingsFile(const std::filesystem::path& path, std::string& bytes, std::string& error, bool workspace_file = false)
	{
		std::error_code ec;
		if (workspace_file && (std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec)) || uam::paths::IsLinkOrReparsePointNoThrow(path)))
		{ error = "Workspace settings file cannot be a symbolic link or reparse point."; return false; }
		ec.clear();
		const auto resolved = std::filesystem::canonical(path, ec);
		if (ec) { error = "Native settings file is missing or unreadable."; return false; }
		return ReadFile(resolved, bytes, error);
	}

	inline bool ReadOptionalGeminiSettings(const std::filesystem::path& path, nlohmann::json& settings, std::string& error, bool workspace_file = false)
	{
		settings = nlohmann::json::object();
		std::error_code ec;
		if (workspace_file && !path.empty() && (std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec)) || uam::paths::IsLinkOrReparsePointNoThrow(path)))
		{ error = "Workspace settings file cannot be a symbolic link or reparse point."; return false; }
		ec.clear();
		if (path.empty() || !std::filesystem::exists(path, ec))
		{
			if (ec) { error = "Could not inspect Gemini settings."; return false; }
			return true;
		}
		std::string bytes;
		if (!ReadNativeSettingsFile(path, bytes, error, workspace_file)) return false;
		settings = nlohmann::json::parse(bytes, nullptr, false, true);
		if (!settings.is_object()) { error = "Existing Gemini settings must contain a JSON object."; return false; }
		return true;
	}

	inline bool SameNativeDirectory(const std::filesystem::path& first, const std::filesystem::path& second)
	{
		if (first.empty() || second.empty()) return false;
		std::error_code ec;
		return first == second || std::filesystem::equivalent(first, second, ec);
	}

	/// <summary>Preserve native customOverrides array precedence without promoting untrusted workspace settings.</summary>
	inline bool ApplyGeminiThinking(nlohmann::json& system, const nlohmann::json& thinking,
	    const std::filesystem::path& system_path, const std::filesystem::path& workspace, Environment& environment, std::string& error)
	{
		std::string defaults_path = EnvironmentValue(environment, "GEMINI_CLI_SYSTEM_DEFAULTS_PATH");
		if (defaults_path.empty()) defaults_path = uam::paths::Utf8PathString(system_path.parent_path() / "system-defaults.json");
		if (uam::paths::PathFromUtf8(defaults_path).is_relative()) defaults_path = uam::paths::Utf8PathString(workspace / uam::paths::PathFromUtf8(defaults_path));
		const std::string custom_home = EnvironmentValue(environment, "GEMINI_CLI_HOME");
		auto home = custom_home.empty()
		    ? uam::platform::ResolveNativeUserHomePath(EnvironmentValue(environment, "HOME"), EnvironmentValue(environment, "USERPROFILE")).value_or(std::filesystem::path{})
		    : uam::paths::PathFromUtf8(custom_home);
		if (!home.empty() && home.is_relative()) home = workspace / home;
		nlohmann::json defaults, user, project;
		if (!ReadOptionalGeminiSettings(uam::paths::PathFromUtf8(defaults_path), defaults, error) ||
		    !ReadOptionalGeminiSettings(home.empty() ? std::filesystem::path{} : home / ".gemini/settings.json", user, error) ||
		    !ReadOptionalGeminiSettings(SameNativeDirectory(workspace, home) ? std::filesystem::path{} : workspace / ".gemini/settings.json", project, error, true)) return false;
		bool trust_enabled = true;
		for (const auto* source : {&defaults, &user, &system})
		{
			if (source->contains("security") && (*source)["security"].is_object() && (*source)["security"].contains("folderTrust") &&
			    (*source)["security"]["folderTrust"].is_object() && (*source)["security"]["folderTrust"].contains("enabled"))
			{
				const auto& enabled = (*source)["security"]["folderTrust"]["enabled"];
				if (!enabled.is_boolean()) { error = "Gemini folder trust configuration is invalid."; return false; }
				trust_enabled = enabled.get<bool>();
			}
		}
		const bool forced_untrusted = thinking.value("cliVersion", "") == "0.64.0" &&
		    (EnvironmentValue(environment, "GEMINI_RESTRICTED_MODE") == "true" || EnvironmentValue(environment, "GEMINI_CLI_TRUST_WORKSPACE") == "false");
		bool trusted = !forced_untrusted && (!trust_enabled || EnvironmentValue(environment, "GEMINI_CLI_TRUST_WORKSPACE") == "true");
		bool trust_known = trusted || forced_untrusted;
		const bool project_overrides = project.contains("modelConfigs") && project["modelConfigs"].is_object() && project["modelConfigs"].contains("customOverrides");
		const bool system_overrides = system.contains("modelConfigs") && system["modelConfigs"].is_object() && system["modelConfigs"].contains("customOverrides");
		if (!trust_known && !EnvironmentValue(environment, "GEMINI_CLI_IDE_WORKSPACE_PATH").empty() &&
		    project.contains("modelConfigs") && project["modelConfigs"].is_object() && project["modelConfigs"].contains("customOverrides") &&
		    !(system.contains("modelConfigs") && system["modelConfigs"].is_object() && system["modelConfigs"].contains("customOverrides")))
		{ error = "Gemini IDE-owned workspace trust is unavailable for a thinking snapshot; use Default or native host settings."; return false; }
		if (!trust_known)
		{
			nlohmann::json folders;
			std::filesystem::path trust_path = uam::paths::PathFromUtf8(EnvironmentValue(environment, "GEMINI_CLI_TRUSTED_FOLDERS_PATH"));
			if (trust_path.empty())
			{
				if (home.empty()) { error = "The executing host's native home is unavailable for Gemini trust."; return false; }
				trust_path = thinking.value("cliVersion", "") == "0.64.0" && EnvironmentValue(environment, "SANDBOX") == "sandbox-exec"
				    ? home / ".cache/.gemini/trustedFolders.json" : home / ".gemini/trustedFolders.json";
			}
			if (trust_path.is_relative()) trust_path = workspace / trust_path;
			if (!ReadOptionalGeminiSettings(trust_path, folders, error)) return false;
			std::size_t longest = 0;
			std::map<std::string, std::string> normalized_rules;
			std::error_code ec;
			auto location = std::filesystem::weakly_canonical(workspace, ec);
			if (ec) { error = "Could not resolve Gemini workspace trust."; return false; }
			for (const auto& [name, level] : folders.items())
			{
				if (!level.is_string() || (level != "TRUST_FOLDER" && level != "TRUST_PARENT" && level != "DO_NOT_TRUST"))
				{ error = "Gemini trusted-folder configuration contains an invalid trust level."; return false; }
				auto rule = uam::paths::PathFromUtf8(name);
				if (rule.is_relative()) rule = workspace / rule;
				rule = rule.lexically_normal();
				if (rule.filename().empty() && rule.has_relative_path()) rule = rule.parent_path();
				std::string rule_key = uam::paths::Utf8PathString(rule);
#if defined(_WIN32) || defined(__APPLE__)
				// Node folds Unicode too. Do not guess case-dependent trust when that cannot be reproduced.
				const auto non_ascii = [](const std::string& value) { return std::any_of(value.begin(), value.end(), [](unsigned char ch) { return ch >= 128; }); };
				if ((non_ascii(rule_key) || non_ascii(uam::paths::Utf8PathString(location))) && project_overrides && !system_overrides)
				{ error = "Gemini workspace override trust needs explicit native trust when Unicode path case cannot be verified."; return false; }
				rule_key = uam::strings::ToLowerAscii(rule_key);
				rule = uam::paths::PathFromUtf8(rule_key);
#endif
				const std::string trust_level = level.get<std::string>();
				if (const auto prior = normalized_rules.find(rule_key); prior != normalized_rules.end() && prior->second != trust_level)
				{ error = "Gemini trusted-folder aliases have conflicting rules; resolve them before snapshotting workspace overrides."; return false; }
				normalized_rules[rule_key] = trust_level;
				auto effective = std::filesystem::weakly_canonical(level == "TRUST_PARENT" ? rule.parent_path() : rule, ec);
				if (ec) { error = "Could not resolve Gemini trusted-folder path."; return false; }
#if defined(_WIN32) || defined(__APPLE__)
				location = uam::paths::PathFromUtf8(uam::strings::ToLowerAscii(uam::paths::Utf8PathString(location)));
				effective = uam::paths::PathFromUtf8(uam::strings::ToLowerAscii(uam::paths::Utf8PathString(effective)));
#endif
				const std::string relative = uam::paths::Utf8PathString(location.lexically_relative(effective));
				if (relative.empty() || relative == ".." || relative.starts_with("../") || relative.starts_with("..\\") || (trust_known && rule_key.size() <= longest)) continue;
				longest = rule_key.size();
				trust_known = true;
				trusted = level == "TRUST_FOLDER" || level == "TRUST_PARENT";
			}
		}
		if (!trust_known && project_overrides && !system_overrides)
		{ error = "Gemini workspace trust is undecided; choose native trust before selecting a thinking level, or select Default."; return false; }

		nlohmann::json overrides = nlohmann::json::array();
		for (const auto* source : {&defaults, &user, trusted ? &project : nullptr, &system})
		{
			if (source == nullptr) continue;
			if (!source->contains("modelConfigs")) continue;
			const auto& models = (*source)["modelConfigs"];
			if (!models.is_object()) { error = "Gemini modelConfigs must be an object."; return false; }
			if (models.contains("modelIdResolutions") && !models["modelIdResolutions"].is_object())
			{ error = "Gemini modelIdResolutions must be an object."; return false; }
			if (!models.contains("customOverrides")) continue;
			if (!models["customOverrides"].is_array()) { error = "Gemini custom model overrides must be an array."; return false; }
			overrides = models["customOverrides"];
		}
		// Only the selected concrete chat model is affected; helper models and fallback routes retain native settings.
		std::string level = thinking["effort"].get<std::string>();
		std::transform(level.begin(), level.end(), level.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
		overrides.push_back({{"match", {{"model", thinking["model"]}, {"isChatModel", true}, {"overrideScope", "core"}}},
		    {"modelConfig", {{"model", thinking["model"]}, {"generateContentConfig", {{"thinkingConfig", {{"thinkingLevel", level}}}}}}}});
		// Conditional retry overrides cannot silently restore an expert-file thinking level.
		for (const bool retry : {false, true})
		{
			auto conditional = overrides.back();
			conditional["match"]["isRetry"] = retry;
			overrides.push_back(std::move(conditional));
		}
		system["modelConfigs"]["customOverrides"] = std::move(overrides);
		// Concrete choices retain their identity instead of native preview/GA alias substitution.
		system["modelConfigs"]["modelIdResolutions"][thinking["model"].get<std::string>()] = {{"default", thinking["model"]}, {"contexts", nlohmann::json::array()}};
		// Changing the system-settings path must not relocate native system defaults.
		SetEnvironment(environment, "GEMINI_CLI_SYSTEM_DEFAULTS_PATH", defaults_path);
		return true;
	}

	/// <summary>Resolve expert settings only on the executing host and preserve every key except the explicit speed override.</summary>
	inline bool ApplyClaudeTerminalFastMode(const std::filesystem::path& runtime, const std::filesystem::path& workspace,
	    std::vector<std::string>& argv, bool fast, std::string& error)
	{
		std::optional<std::size_t> last;
		std::size_t count = 0;
		for (std::size_t index = 1; index < argv.size() && argv[index] != "--"; ++index)
		{
			// Structured Chat applies its negotiated, acknowledged native setting instead.
			if (argv[index] == "--input-format" || argv[index].starts_with("--input-format=")) return true;
			if (argv[index] == "--settings")
			{
				if (index + 1 == argv.size() || argv[index + 1] == "--" || argv[index + 1].starts_with("--"))
				{ error = "Claude --settings requires a JSON object or an executing-host filepath."; return false; }
				last = index; count = 2; ++index;
			}
			else if (argv[index].starts_with("--settings=")) { last = index; count = 1; }
		}
		nlohmann::json settings = nlohmann::json::object();
		if (last)
		{
			const std::string source = count == 2 ? argv[*last + 1] : argv[*last].substr(std::string_view("--settings=").size());
			if (source.empty()) { error = "Claude --settings cannot be empty."; return false; }
			std::string bytes = source;
			const std::size_t first = source.find_first_not_of(" \t\r\n");
			if (first == std::string::npos || source[first] != '{')
			{
				std::filesystem::path path = uam::paths::PathFromUtf8(source);
				if (path.is_relative()) path = workspace / path;
				if (!ReadNativeSettingsFile(path, bytes, error)) { error = "Claude --settings file is unreadable or larger than 2 MiB. " + error; return false; }
			}
			if (bytes.size() > kMaxFileBytes) { error = "Claude --settings exceeds 2 MiB."; return false; }
			settings = nlohmann::json::parse(bytes, nullptr, false);
			if (!settings.is_object()) { error = "Claude --settings must contain a valid JSON object."; return false; }
		}
		settings["fastMode"] = fast;
		const std::string snapshot = settings.dump();
		if (snapshot.size() > kMaxFileBytes) { error = "Merged Claude --settings exceeds 2 MiB."; return false; }
		const std::filesystem::path target = runtime / ("claude-settings-" + Digest(snapshot) + ".json");
		if (!WriteRuntimeFile(target, snapshot, error, false)) return false;
		const std::string path = uam::paths::Utf8PathString(target);
		if (last)
		{
			if (count == 2) argv[*last + 1] = path;
			else argv[*last] = "--settings=" + path;
		}
		else
		{
			const auto positional = std::find(argv.begin() + std::min(std::size_t{1}, argv.size()), argv.end(), "--");
			argv.insert(positional, {"--settings", path});
		}
		return true;
	}

	/// <summary>Apply session-only settings on the executing host; secrets never travel from controller to SSH.</summary>
	inline bool Apply(const std::string& provider, const std::filesystem::path& directory,
	                  const std::filesystem::path& workspace, std::vector<std::string>& argv,
	                  Environment& environment, std::string& error, const nlohmann::json& session_servers = nlohmann::json::array(),
	                  std::string* protocol_instructions = nullptr)
	{
		if (protocol_instructions) protocol_instructions->clear();
		if (argv.empty() || (provider != uam::provider_ids::kCodexCli && provider != uam::provider_ids::kClaudeCli && provider != uam::provider_ids::kGeminiCli && provider != uam::provider_ids::kOpenCodeCli && provider != uam::provider_ids::kCopilotCli && provider != uam::provider_ids::kAntigravityCli))
		{ error = "Central configuration does not support this provider."; return false; }
		if (!session_servers.is_array()) { error = "Session MCP configuration is invalid."; return false; }
		std::string owner;
		if (!ReadFile(directory / "owner.json", owner, error)) return false;
		const nlohmann::json manifest = nlohmann::json::parse(owner, nullptr, false);
		std::vector<std::string> missing;
		if (!Prepare(directory, manifest, missing, error) || !missing.empty())
		{ if (error.empty()) error = "Central resource installation is incomplete."; return false; }
		for (const auto& [name, entry] : manifest["files"].items())
		{
			std::error_code permissions_error;
			std::filesystem::perms permissions = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
			if (entry.value("executable", false)) permissions |= std::filesystem::perms::owner_exec;
			std::filesystem::permissions(directory / uam::paths::PathFromUtf8(name), permissions, std::filesystem::perm_options::replace, permissions_error);
			if (permissions_error) { error = "Could not set central resource permissions."; return false; }
		}
		const bool mcp_only = manifest.value("mcpOnly", false);
		std::string instructions;
		if (!mcp_only && !ReadFile(directory / "AGENTS.md", instructions, error)) return false;
		const std::string root = uam::paths::Utf8PathString(directory);
		instructions = ReplaceRoot(instructions, root);
		std::error_code workspace_error;
		// Codex loads workspace AGENTS.md natively; copying it into -c duplicates context and can exceed Windows argv limits.
		if (!mcp_only && provider != uam::provider_ids::kCodexCli && std::filesystem::exists(workspace / "AGENTS.md", workspace_error))
		{
			std::string workspace_instructions;
			if (!ReadFile(workspace / "AGENTS.md", workspace_instructions, error)) return false;
			instructions += "\n\nWorkspace AGENTS.md:\n" + workspace_instructions;
		}
		else if (workspace_error) { error = "Workspace instructions are unavailable."; return false; }
		if (manifest.contains("hostInstructionFile") && !manifest["hostInstructionFile"].is_string()) { error = "Host instruction path is invalid."; return false; }
		const std::string host_file = manifest.value("hostInstructionFile", std::string{});
		if (!host_file.empty())
		{
			std::string host_instructions;
			if (!ReadFile(uam::paths::PathFromUtf8(host_file), host_instructions, error)) return false;
			instructions += "\n\n" + host_instructions;
		}
		// Generated files remain private and are never written to the workspace or provider home.
		const std::filesystem::path runtime = directory / "runtime" / provider / Digest(session_servers.dump() + nlohmann::json(argv).dump() + nlohmann::json(environment).dump());
		if (!NoSymlinks(runtime)) { error = "Central runtime path contains a symbolic link."; return false; }
		std::error_code ec;
		std::filesystem::create_directories(runtime, ec);
		const std::filesystem::path instruction_path = runtime / "AGENTS.md";
		const std::filesystem::path mcp_path = runtime / "mcp.json";
		for (const std::filesystem::path& path : {instruction_path, mcp_path, runtime / "settings.json"})
			if (!NoSymlinks(path)) { error = "Central runtime file contains a symbolic link."; return false; }
		if (ec || !WriteRuntimeFile(instruction_path, instructions, error)) { error = "Could not install central instructions."; return false; }
		std::vector<McpServerConfiguration> configured = uam::mcp_server_config::Parse(manifest.value("mcpServers", nlohmann::json::array()));
		if (!uam::mcp_server_config::NormalizeAndValidate(configured, &error)) return false;
		nlohmann::json servers = uam::mcp_server_config::ResolveForWorkspace(configured, uam::paths::Utf8PathString(workspace), true, true, &error);
		if (!servers.is_array()) return false;
		for (const nlohmann::json& server : session_servers) servers.push_back(server);
		nlohmann::json native_servers = nlohmann::json::object();
		for (const nlohmann::json& server : servers)
		{
			nlohmann::json native = server;
			const std::string name = native["name"].get<std::string>();
			native.erase("name");
			for (const char* field : {"env", "headers"})
			{
				if (!native.contains(field)) continue;
				nlohmann::json entries = nlohmann::json::object();
				for (const nlohmann::json& entry : native[field]) entries[entry["name"].get<std::string>()] = entry["value"];
				native[field] = std::move(entries);
			}
			native_servers[name] = std::move(native);
		}
		if (!WriteRuntimeFile(mcp_path, nlohmann::json{{"mcpServers", native_servers}}.dump(), error)) { error = "Could not install central MCP configuration."; return false; }
		const std::string instruction_file = uam::paths::Utf8PathString(instruction_path);
		if (provider == uam::provider_ids::kAntigravityCli)
		{
			// Native --add-dir discovers this private directory without changing cwd or user configuration.
			for (nlohmann::json& server : native_servers)
			{
				if (server.contains("url"))
				{
					server["serverUrl"] = server["url"];
					server.erase("url");
				}
				server.erase("type");
			}
			const std::filesystem::path isolated_config = runtime / ".agents" / "mcp_config.json";
			if (!NoSymlinks(isolated_config)) { error = "Central runtime path contains a symbolic link."; return false; }
			std::filesystem::create_directories(isolated_config.parent_path(), ec);
			if (ec) { error = "Could not create private Antigravity configuration directory."; return false; }
			std::filesystem::permissions(isolated_config.parent_path(), std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec);
			if (ec) { error = "Could not secure private Antigravity configuration directory."; return false; }
			if (!WriteRuntimeFile(isolated_config, nlohmann::json{{"mcpServers", native_servers}}.dump(), error)) return false;
			argv.insert(argv.end(), {"--add-dir", uam::paths::Utf8PathString(runtime)});
		}
		else if (provider == uam::provider_ids::kClaudeCli)
		{
			if (manifest.contains("claudeTerminalFastMode") &&
			    !ApplyClaudeTerminalFastMode(runtime, workspace, argv, manifest["claudeTerminalFastMode"].get<bool>(), error)) return false;
			for (std::size_t index = 1; index + 1 < argv.size();)
			{
				if (mcp_only || (argv[index] != "--append-system-prompt-file" && argv[index] != "--append-system-prompt")) { ++index; continue; }
				std::string previous = argv[index + 1];
				if (argv[index] == "--append-system-prompt-file" && !ReadFile(uam::paths::PathFromUtf8(previous), previous, error)) return false;
				instructions += "\n\n" + previous;
				argv.erase(argv.begin() + index, argv.begin() + index + 2);
			}
			if (!WriteRuntimeFile(instruction_path, instructions, error)) return false;
			if (!mcp_only) argv.insert(argv.end(), {"--append-system-prompt-file", instruction_file});
			for (std::size_t index = 1; index + 1 < argv.size();)
			{
				if (argv[index] != "--mcp-config") { ++index; continue; }
				const std::string source = argv[index + 1];
				std::string content = source;
				if (!source.starts_with("{"))
					if (!ReadFile(uam::paths::PathFromUtf8(source), content, error)) return false;
				const nlohmann::json previous = nlohmann::json::parse(content, nullptr, false);
				if (!previous.is_object() || !previous.contains("mcpServers") || !previous["mcpServers"].is_object())
				{ error = "Existing Claude MCP configuration is invalid."; return false; }
				for (const auto& [name, server] : previous["mcpServers"].items())
					if (!native_servers.contains(name)) native_servers[name] = server;
				argv.erase(argv.begin() + index, argv.begin() + index + 2);
			}
			if (!native_servers.empty())
			{
				if (!WriteRuntimeFile(mcp_path, nlohmann::json{{"mcpServers", native_servers}}.dump(), error)) return false;
				argv.insert(argv.end(), {"--mcp-config", uam::paths::Utf8PathString(mcp_path)});
			}
		}
		else if (provider == uam::provider_ids::kCodexCli)
		{
			#if defined(_WIN32)
			constexpr std::size_t max_instruction_bytes = 12 * 1024;
#else
			constexpr std::size_t max_instruction_bytes = 64 * 1024;
#endif
			const bool structured_setup = protocol_instructions != nullptr && std::ranges::find(argv, "app-server") != argv.end();
			if (!mcp_only && structured_setup) *protocol_instructions = instructions;
			else if (!mcp_only)
			{
				if (instructions.size() > max_instruction_bytes) { error = "Codex central instructions exceed the OS launch limit. Move reference material into skills."; return false; }
				argv.insert(argv.begin() + 1, {"-c", "developer_instructions=" + nlohmann::json(instructions).dump()});
			}
			for (const auto& [name, server] : native_servers.items())
			{
				const std::string prefix = "mcp_servers." + nlohmann::json(name).dump() + ".";
				for (const auto& [key, value] : server.items())
				{
					if (key == "type")
					{
						if (value == "sse") { error = "Codex does not support SSE MCP servers. Use HTTP or stdio."; return false; }
						continue;
					}
					if (key == "env")
					{
						std::vector<std::string> names;
						for (const auto& [variable, text] : value.items())
						{
							SetEnvironment(environment, variable, text.get<std::string>());
							names.push_back(variable);
						}
						argv.insert(argv.begin() + 1, {"-c", prefix + "env_vars=" + nlohmann::json(names).dump()});
						continue;
					}
					if (key == "headers")
					{
						for (const auto& [header, text] : value.items())
						{
							const std::string variable = "UAM_CENTRAL_MCP_HEADER_" + Digest(name + header);
							SetEnvironment(environment, variable, text.get<std::string>());
							argv.insert(argv.begin() + 1, {"-c", prefix + "env_http_headers." + nlohmann::json(header).dump() + "=" + nlohmann::json(variable).dump()});
						}
						continue;
					}
					if (value.is_object())
					{
						for (const auto& [entry, text] : value.items()) argv.insert(argv.begin() + 1, {"-c", prefix + key + "." + nlohmann::json(entry).dump() + "=" + text.dump()});
					}
					else argv.insert(argv.begin() + 1, {"-c", prefix + key + "=" + value.dump()});
				}
			}
		}
		else if (provider == uam::provider_ids::kCopilotCli)
		{
			const std::string existing = EnvironmentValue(environment, "COPILOT_CUSTOM_INSTRUCTIONS_DIRS");
			if (!mcp_only) SetEnvironment(environment, "COPILOT_CUSTOM_INSTRUCTIONS_DIRS", uam::paths::Utf8PathString(runtime) + (existing.empty() ? "" : "," + existing));
			if (!servers.empty()) argv.insert(argv.end(), {"--additional-mcp-config", "@" + uam::paths::Utf8PathString(mcp_path)});
		}
		else if (provider == uam::provider_ids::kOpenCodeCli)
		{
			// Resolve defaults on the executing host, preserving even an explicitly empty override.
			constexpr const char* code_mode = "OPENCODE_EXPERIMENTAL_CODE_MODE";
			if (manifest.value("openCodeCodeMode", false) && std::getenv(code_mode) == nullptr &&
			    std::ranges::none_of(environment, [code_mode](const std::pair<std::string, std::string>& entry)
			    {
#if defined(_WIN32)
				return uam::strings::EqualsIgnoreCase(entry.first, code_mode);
#else
				return entry.first == code_mode;
#endif
			    }))
				SetEnvironment(environment, code_mode, "true");
			const std::string existing = EnvironmentValue(environment, "OPENCODE_CONFIG_CONTENT");
			nlohmann::json config = existing.empty() ? nlohmann::json::object() : nlohmann::json::parse(existing, nullptr, false, true);
			if (!config.is_object()) { error = "Existing OpenCode inline configuration is invalid."; return false; }
			if (!config.contains("instructions")) config["instructions"] = nlohmann::json::array();
			if (!config["instructions"].is_array()) { error = "OpenCode instructions must be a list."; return false; }
			if (!mcp_only) config["instructions"].push_back(instruction_file);
			if (config.contains("mcp") && !config["mcp"].is_object()) { error = "OpenCode MCP settings must be an object."; return false; }
			const bool uam_control_deadline = manifest.value("openCodeUamControlDeadline", false);
			const bool inline_uam_control = config.contains("mcp") && config["mcp"].contains("uam-control");
			if (uam_control_deadline && inline_uam_control && native_servers.contains("uam-control"))
			{ error = "Existing OpenCode inline UAM control settings conflict with this launch's owned server."; return false; }
			for (const auto& [name, server] : native_servers.items())
			{
				nlohmann::json native;
				if (server.contains("command"))
				{
					std::vector<std::string> command{server["command"].get<std::string>()};
					const std::vector<std::string> args = server["args"].get<std::vector<std::string>>();
					command.insert(command.end(), args.begin(), args.end());
					native = {{"type", "local"}, {"command", command}, {"environment", server["env"]}, {"enabled", true}};
				}
				else native = {{"type", "remote"}, {"url", server["url"]}, {"headers", server["headers"]}, {"enabled", true}};
				config["mcp"][name] = std::move(native);
			}
			if (uam_control_deadline && !inline_uam_control)
			{
				// ACP supplies its real server later. Checked OpenCode 1.19.3 skips disabled static
				// entries, then uses their per-server timeout for the dynamically registered server.
				if (!native_servers.contains("uam-control"))
					config["mcp"]["uam-control"] = {{"type", "local"}, {"command", nlohmann::json::array()}, {"enabled", false}};
				config["mcp"]["uam-control"]["timeout"] = 120000;
			}
			SetEnvironment(environment, "OPENCODE_CONFIG_CONTENT", config.dump());
		}
		else if (provider == uam::provider_ids::kGeminiCli)
		{
			nlohmann::json config = nlohmann::json::object();
			std::string existing = EnvironmentValue(environment, "GEMINI_CLI_SYSTEM_SETTINGS_PATH");
			if (existing.empty())
			{
#if defined(_WIN32)
				existing = "C:\\ProgramData\\gemini-cli\\settings.json";
#elif defined(__APPLE__)
				existing = "/Library/Application Support/GeminiCli/settings.json";
#else
				existing = "/etc/gemini-cli/settings.json";
#endif
			}
			if (uam::paths::PathFromUtf8(existing).is_relative()) existing = uam::paths::Utf8PathString(workspace / uam::paths::PathFromUtf8(existing));
			if (!ReadOptionalGeminiSettings(uam::paths::PathFromUtf8(existing), config, error)) return false;
			if (manifest.contains("geminiThinking"))
			{
				const std::string executable = uam::strings::ToLowerAscii(uam::paths::Utf8PathString(uam::paths::PathFromUtf8(argv.front()).filename()));
				if (executable != "gemini" && executable != "gemini.cmd" && executable != "gemini.exe" && executable != "gemini.ps1")
				{ error = "Gemini thinking requires the direct native Gemini CLI command; a shell or custom wrapper can change host trust after snapshot preparation."; return false; }
				if (!ApplyGeminiThinking(config, manifest["geminiThinking"], uam::paths::PathFromUtf8(existing), workspace, environment, error)) return false;
			}
			if ((config.contains("context") && !config["context"].is_object()) || (config.contains("mcpServers") && !config["mcpServers"].is_object()))
			{ error = "Gemini context and MCP settings must be objects."; return false; }
			if (!mcp_only) config["context"]["loadMemoryFromIncludeDirectories"] = true;
			if (!mcp_only)
			{
				// Add AGENTS.md without replacing the user's configured memory filenames.
				nlohmann::json names = config.value("context", nlohmann::json::object()).value("fileName", nlohmann::json("GEMINI.md"));
				std::vector<std::filesystem::path> memory_sources;
				const std::string custom_home = EnvironmentValue(environment, "GEMINI_CLI_HOME");
				auto home = custom_home.empty()
				    ? uam::platform::ResolveNativeUserHomePath(EnvironmentValue(environment, "HOME"), EnvironmentValue(environment, "USERPROFILE")).value_or(std::filesystem::path{})
				    : uam::paths::PathFromUtf8(custom_home);
				if (!home.empty() && home.is_relative()) home = workspace / home;
				if (!home.empty()) memory_sources.push_back(home / ".gemini/settings.json");
				if (!SameNativeDirectory(workspace, home)) memory_sources.push_back(workspace / ".gemini/settings.json");
				for (const std::filesystem::path& source : memory_sources)
				{
					std::error_code source_error;
					if (!std::filesystem::exists(source, source_error)) continue;
					std::string bytes;
					if (!ReadNativeSettingsFile(source, bytes, error, source == workspace / ".gemini/settings.json")) return false;
					const nlohmann::json source_settings = nlohmann::json::parse(bytes, nullptr, false, true);
					if (!source_settings.is_object()) { error = "Existing Gemini memory settings are invalid."; return false; }
					if (source_settings.contains("context") && source_settings["context"].is_object() && source_settings["context"].contains("fileName")) names = source_settings["context"]["fileName"];
				}
				if (config.contains("context") && config["context"].is_object() && config["context"].contains("fileName")) names = config["context"]["fileName"];
				if (names.is_string()) names = nlohmann::json::array({names});
				if (!names.is_array()) { error = "Gemini context filenames are invalid."; return false; }
				if (!mcp_only && std::find(names.begin(), names.end(), nlohmann::json("AGENTS.md")) == names.end()) names.push_back("AGENTS.md");
				config["context"]["fileName"] = names;
			}
			for (const auto& [name, server] : native_servers.items())
			{
				nlohmann::json native = server;
				if (native.contains("type")) { native.erase("type"); if (server["type"] == "http") { native["httpUrl"] = native["url"]; native.erase("url"); } }
				config["mcpServers"][name] = std::move(native);
			}
			const std::string snapshot = config.dump();
			if (snapshot.size() > kMaxFileBytes) { error = "Merged Gemini settings exceed 2 MiB."; return false; }
			const std::filesystem::path settings = runtime / ("gemini-settings-" + Digest(snapshot) + ".json");
			if (!WriteRuntimeFile(settings, snapshot, error, false)) { error = "Could not install immutable Gemini configuration. " + error; return false; }
			SetEnvironment(environment, "GEMINI_CLI_SYSTEM_SETTINGS_PATH", uam::paths::Utf8PathString(settings));
			if (!mcp_only) argv.insert(argv.end(), {"--include-directories", uam::paths::Utf8PathString(runtime)});
		}
		else { error = "Central configuration does not support this provider."; return false; }
		return true;
	}
}
