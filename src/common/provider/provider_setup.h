#pragma once

#include "common/config/mcp_server_config.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_native_context.h"
#include "common/paths/path_utils.h"
#include "common/utils/hash_utils.h"
#include "common/utils/io_utils.h"
#include <fstream>
#include <map>
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
		bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
		if (!input || bytes.size() > kMaxFileBytes) { error = "Could not read central resource: " + uam::paths::Utf8PathString(path); return false; }
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

	/// <summary>Apply session-only settings on the executing host; secrets never travel from controller to SSH.</summary>
	inline bool Apply(const std::string& provider, const std::filesystem::path& directory,
	                  const std::filesystem::path& workspace, std::vector<std::string>& argv,
	                  Environment& environment, std::string& error, const nlohmann::json& session_servers = nlohmann::json::array())
	{
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
		if (!mcp_only && std::filesystem::exists(workspace / "AGENTS.md", workspace_error))
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
			if (instructions.size() > max_instruction_bytes) { error = "Codex central instructions exceed the OS launch limit. Move reference material into skills."; return false; }
			if (!mcp_only) argv.insert(argv.begin() + 1, {"-c", "developer_instructions=" + nlohmann::json(instructions).dump()});
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
			const std::string existing = EnvironmentValue(environment, "OPENCODE_CONFIG_CONTENT");
			nlohmann::json config = existing.empty() ? nlohmann::json::object() : nlohmann::json::parse(existing, nullptr, false, true);
			if (!config.is_object()) { error = "Existing OpenCode inline configuration is invalid."; return false; }
			if (!config.contains("instructions")) config["instructions"] = nlohmann::json::array();
			if (!config["instructions"].is_array()) { error = "OpenCode instructions must be a list."; return false; }
			if (!mcp_only) config["instructions"].push_back(instruction_file);
			if (config.contains("mcp") && !config["mcp"].is_object()) { error = "OpenCode MCP settings must be an object."; return false; }
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
				std::error_code source_error;
				if (!std::filesystem::exists(uam::paths::PathFromUtf8(existing), source_error)) existing.clear();
			}
			if (!existing.empty())
			{
				std::string bytes;
				if (!ReadFile(uam::paths::PathFromUtf8(existing), bytes, error)) return false;
				config = nlohmann::json::parse(bytes, nullptr, false, true);
				if (!config.is_object()) { error = "Existing Gemini system settings are invalid."; return false; }
			}
			if ((config.contains("context") && !config["context"].is_object()) || (config.contains("mcpServers") && !config["mcpServers"].is_object()))
			{ error = "Gemini context and MCP settings must be objects."; return false; }
			if (!mcp_only) config["context"]["loadMemoryFromIncludeDirectories"] = true;
			if (!mcp_only)
			{
				// Add AGENTS.md without replacing the user's configured memory filenames.
				nlohmann::json names = config.value("context", nlohmann::json::object()).value("fileName", nlohmann::json("GEMINI.md"));
				std::vector<std::filesystem::path> memory_sources;
				const std::string home = EnvironmentValue(environment, "GEMINI_CLI_HOME").empty() ? EnvironmentValue(environment, "HOME") : EnvironmentValue(environment, "GEMINI_CLI_HOME");
				if (!home.empty()) memory_sources.push_back(uam::paths::PathFromUtf8(home) / ".gemini/settings.json");
				memory_sources.push_back(workspace / ".gemini/settings.json");
				for (const std::filesystem::path& source : memory_sources)
				{
					std::error_code source_error;
					if (!std::filesystem::exists(source, source_error)) continue;
					std::string bytes;
					if (!ReadFile(source, bytes, error)) return false;
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
			const std::filesystem::path settings = runtime / "settings.json";
			if (!WriteRuntimeFile(settings, config.dump(), error)) { error = "Could not install Gemini configuration."; return false; }
			SetEnvironment(environment, "GEMINI_CLI_SYSTEM_SETTINGS_PATH", uam::paths::Utf8PathString(settings));
			if (!mcp_only) argv.insert(argv.end(), {"--include-directories", uam::paths::Utf8PathString(runtime)});
		}
		else { error = "Central configuration does not support this provider."; return false; }
		return true;
	}
}
