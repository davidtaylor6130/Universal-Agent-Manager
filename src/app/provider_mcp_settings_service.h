#pragma once

#include "common/state/app_state.h"
#include "common/config/mcp_server_config.h"
#include "common/config/central_provider_configuration.h"
#include "common/config/build_features.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_setup.h"
#include "common/utils/time_utils.h"
#include <sstream>
#include <stdexcept>
#include <ranges>
#if !defined(_WIN32)
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif

namespace uam::provider_mcp_settings
{
	/// Resolve without executing a command. Imported arguments remain unchanged for review.
	inline std::string ResolveCommand(const std::string& command, const std::vector<std::filesystem::path>& search)
	{
		const std::filesystem::path path = paths::PathFromUtf8(command);
		const auto usable = [](const std::filesystem::path& candidate)
		{
			if (!paths::IsRegularFileNoThrow(candidate)) return false;
#if !defined(_WIN32)
			return access(candidate.c_str(), X_OK) == 0;
#else
			const std::string extension = strings::ToLowerAscii(candidate.extension().string());
			return extension == ".exe" || extension == ".cmd" || extension == ".bat" || extension == ".com";
#endif
		};
		if (path.is_absolute()) return usable(path) ? paths::Utf8PathString(path) : std::string{};
		if (path.has_parent_path() || command.empty()) return {};
		for (const std::filesystem::path& directory : search)
		{
			if (!directory.is_absolute()) continue;
			if (usable(directory / path)) return paths::Utf8PathString(directory / path);
#if defined(_WIN32)
			for (const char* extension : {".exe", ".cmd", ".bat", ".com"})
				if (usable(directory / (command + extension))) return paths::Utf8PathString(directory / (command + extension));
#endif
		}
		return {};
	}

	inline std::vector<std::filesystem::path> CommandSearchPaths()
	{
		std::vector<std::filesystem::path> result;
		std::stringstream directories(env::GetNonEmptyString("PATH").value_or(""));
		std::string directory;
#if defined(_WIN32)
		constexpr char separator = ';';
#else
		constexpr char separator = ':';
#endif
		while (std::getline(directories, directory, separator))
			if (!directory.empty()) result.push_back(paths::PathFromUtf8(directory));
#if defined(__APPLE__)
		result.emplace_back("/opt/homebrew/bin"); result.emplace_back("/usr/local/bin");
#endif
		return result;
	}

	/// Preview conversion only. Unsupported entries remain visible as warnings, never silently dropped on save.
	inline nlohmann::json Preview(const nlohmann::json& config, const std::string& host, const std::string& workspace,
	    const std::vector<std::filesystem::path>& search)
	{
		nlohmann::json result{{"executionHostId", host}, {"sources", nlohmann::json::array()},
		    {"servers", nlohmann::json::array()}, {"warnings", nlohmann::json::array()}, {"requiresReview", true}};
		if (!config.is_object() || !config.contains("mcp") || !config["mcp"].is_object())
			throw std::runtime_error("OpenCode configuration must contain an mcp object.");
		if (config["mcp"].size() > 128) throw std::runtime_error("Import at most 128 MCP definitions at a time.");
		for (const auto& [name, entry] : config["mcp"].items())
		{
			try
			{
				if (!entry.is_object() || !entry.contains("type") || !entry["type"].is_string()) throw std::runtime_error("missing local or remote type");
				McpServerConfiguration server;
				server.id = "opencode-" + provider_setup::Digest(host + "\n" + workspace + "\n" + name);
				server.execution_host_id = host; server.workspace_directory = workspace; server.name = name;
				if (entry.contains("enabled") && !entry["enabled"].is_boolean()) throw std::runtime_error("enabled must be boolean");
				server.enabled = entry.value("enabled", true);
				const std::string type = entry["type"].get<std::string>();
				const char* references = nullptr;
				if (type == "local")
				{
					if (!entry.contains("command") || !entry["command"].is_array() || entry["command"].empty() || entry["command"].size() > 256) throw std::runtime_error("command must be a bounded nonempty string array");
					const std::vector<std::string> command = entry["command"].get<std::vector<std::string>>();
					server.command = ResolveCommand(command.front(), search);
					if (server.command.empty()) throw std::runtime_error("executable unavailable; use its full path on the selected host");
					server.args.assign(command.begin() + 1, command.end());
					for (const std::string& argument : server.args)
						if (argument.find("{env:") != std::string::npos || argument.find("{file:") != std::string::npos) throw std::runtime_error("command substitutions need manual configuration");
					server.transport = "stdio"; references = "environment";
				}
				else if (type == "remote")
				{
					server.transport = "http"; server.url = entry.at("url").get<std::string>(); references = "headers";
				}
				else throw std::runtime_error("unsupported type");
				if (entry.contains("timeout") || entry.contains("oauth")) throw std::runtime_error("timeout or OAuth settings need manual configuration; their semantics cannot be preserved by this import");
				if (entry.contains(references))
				{
					if (!entry[references].is_object()) throw std::runtime_error("environment and headers must be objects");
					for (const auto& [key, value] : entry[references].items())
					{
						if (!value.is_string()) throw std::runtime_error("environment and header values must be strings");
						const std::string text = value.get<std::string>();
						McpSecretReference reference; reference.name = key;
						if (text.starts_with("{env:") && text.ends_with('}') && text.size() > 6) reference.environment_variable = text.substr(5, text.size() - 6);
						else if (text.find("{env:") != std::string::npos || text.find("{file:") != std::string::npos) throw std::runtime_error("embedded substitutions need manual configuration");
						else reference.value = text;
						(type == "local" ? server.environment : server.headers).push_back(std::move(reference));
					}
				}
				if (server.url.find("{env:") != std::string::npos || server.url.find("{file:") != std::string::npos) throw std::runtime_error("URL substitutions need manual configuration");
				std::vector<McpServerConfiguration> normalized{server}; std::string error;
				if (!mcp_server_config::NormalizeAndValidate(normalized, &error)) throw std::runtime_error(error);
				result["servers"].push_back(mcp_server_config::Serialize(normalized).front());
			}
			catch (const std::exception& error)
			{
				// Errors describe the shape or missing executable; never echo secret values.
				result["warnings"].push_back(name + ": " + error.what());
			}
		}
		return result;
	}

	/// Read at most 2 MiB from a regular open handle; a final symlink or FIFO cannot redirect/block the worker.
	inline bool ReadConfigFile(const std::filesystem::path& path, std::string& bytes)
	{
		bytes.clear();
		if (path.empty() || path.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos) return false;
		constexpr std::size_t cap = provider_setup::kMaxFileBytes;
#if defined(_WIN32)
		std::filesystem::path native; std::string error;
		if (!io::NativeFilePath(path, native, error)) return false;
		HANDLE handle = CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
		    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (handle == INVALID_HANDLE_VALUE) return false;
		BY_HANDLE_FILE_INFORMATION info{};
		bool ok = GetFileInformationByHandle(handle, &info) &&
		    (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0 &&
		    info.nFileSizeHigh == 0 && info.nFileSizeLow > 0 && info.nFileSizeLow <= cap;
		if (ok)
		{
			bytes.resize(info.nFileSizeLow); DWORD count = 0;
			ok = ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) && count == bytes.size();
			char extra = 0; DWORD extra_count = 0;
			ok = ok && ReadFile(handle, &extra, 1, &extra_count, nullptr) && extra_count == 0;
		}
		CloseHandle(handle);
#else
		const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
		if (fd < 0) return false;
		struct stat info{};
		bool ok = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0 && static_cast<std::uintmax_t>(info.st_size) <= cap;
		if (ok)
		{
			bytes.resize(static_cast<std::size_t>(info.st_size)); std::size_t offset = 0;
			while (offset < bytes.size())
			{
				const ssize_t count = read(fd, bytes.data() + offset, bytes.size() - offset);
				if (count <= 0) { ok = false; break; }
				offset += static_cast<std::size_t>(count);
			}
			char extra = 0; ok = ok && read(fd, &extra, 1) == 0;
		}
		close(fd);
#endif
		if (!ok) bytes.clear();
		return ok;
	}

	/// Remove JSONC comments and trailing commas without interpreting quoted values.
	inline std::string JsoncText(std::string text)
	{
		bool quoted = false;
		bool escaped = false;
		for (std::size_t index = 0; index < text.size(); ++index)
		{
			const char ch = text[index];
			if (quoted)
			{
				if (escaped) escaped = false;
				else if (ch == '\\') escaped = true;
				else if (ch == '"') quoted = false;
				continue;
			}
			if (ch == '"') { quoted = true; continue; }
			if (ch != '/' || index + 1 >= text.size()) continue;
			if (text[index + 1] == '/')
			{
				while (index < text.size() && text[index] != '\n' && text[index] != '\r') text[index++] = ' ';
				if (index == text.size()) break;
			}
			else if (text[index + 1] == '*')
			{
				text[index++] = ' '; text[index++] = ' ';
				while (index + 1 < text.size() && !(text[index] == '*' && text[index + 1] == '/'))
				{
					if (text[index] != '\n' && text[index] != '\r') text[index] = ' ';
					++index;
				}
				if (index + 1 >= text.size()) throw std::runtime_error("The OpenCode JSONC block comment is not closed.");
				text[index] = ' '; text[++index] = ' ';
			}
		}
		quoted = false; escaped = false;
		char previous = 0;
		for (std::size_t index = 0; index < text.size(); ++index)
		{
			const char ch = text[index];
			if (quoted)
			{
				if (escaped) escaped = false;
				else if (ch == '\\') escaped = true;
				else if (ch == '"') quoted = false;
				previous = ch; continue;
			}
			if (ch == '"') { quoted = true; previous = ch; continue; }
			if (ch == ',')
			{
				std::size_t next = index + 1;
				while (next < text.size() && (text[next] == ' ' || text[next] == '\t' || text[next] == '\r' || text[next] == '\n')) ++next;
				if (next < text.size() && (text[next] == '}' || text[next] == ']') && previous != 0 && previous != ',' && previous != ':' && previous != '{' && previous != '[')
					text[index] = ' ';
			}
			if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') previous = ch;
		}
		return text;
	}

	/// Bounded global configuration import. Later OpenCode layers override matching server names.
	inline nlohmann::json ReadImport(std::string source, const std::string& workspace, const std::string& inline_config,
	    const std::string& override_path, const std::filesystem::path& config_root, const std::vector<std::filesystem::path>& search)
	{
		bool fallback = false;
		if (source.empty())
		{
			if (config_root.is_absolute() && paths::IsDirectoryNoThrow(config_root / "opencode")) source = paths::Utf8PathString(config_root / "opencode");
			else { source = override_path; fallback = true; }
		}
		nlohmann::json combined = nlohmann::json::object();
		nlohmann::json sources = nlohmann::json::array();
		std::size_t total_bytes = 0;
		const auto merge = [&](std::string bytes, const std::string& name)
		{
			if (bytes.size() > provider_setup::kMaxFileBytes || total_bytes > provider_setup::kMaxFileBytes - bytes.size()) throw std::runtime_error("OpenCode configuration exceeds the 2 MiB import limit.");
			total_bytes += bytes.size();
			const nlohmann::json config = nlohmann::json::parse(JsoncText(std::move(bytes)), nullptr, false);
			if (!config.is_object()) throw std::runtime_error("The selected OpenCode configuration is not valid JSON or JSONC. Check its quotes, commas and comments.");
			if (config.contains("mcp"))
			{
				if (!config["mcp"].is_object()) throw std::runtime_error("OpenCode mcp settings must be an object.");
				if (!combined.contains("mcp")) combined["mcp"] = nlohmann::json::object();
				for (const auto& [id, settings] : config["mcp"].items())
				{
					if (combined["mcp"].contains(id) && combined["mcp"][id].is_object() && settings.is_object()) combined["mcp"][id].merge_patch(settings);
					else combined["mcp"][id] = settings;
				}
			}
			sources.push_back(name);
		};
		if (source.empty() && !inline_config.empty()) merge(inline_config, "OPENCODE_CONFIG_CONTENT");
		else
		{
			const std::filesystem::path path = paths::PathFromUtf8(source);
			if (source.empty() || !path.is_absolute()) throw std::runtime_error("Select your global OpenCode configuration directory or file by its full path.");
			const auto read = [&](const std::filesystem::path& file)
			{
				std::string bytes;
				if (!ReadConfigFile(file, bytes)) throw std::runtime_error("Could not read the selected OpenCode configuration file.");
				merge(std::move(bytes), paths::Utf8PathString(file));
			};
			if (paths::IsDirectoryNoThrow(path))
			{
				for (const char* name : {"config.json", "opencode.json", "opencode.jsonc"})
					if (paths::PathExistsNoThrow(path / name)) read(path / name);
				if (sources.empty()) throw std::runtime_error("The selected directory contains no opencode.jsonc, opencode.json or config.json. Select the global OpenCode configuration directory or its file.");
			}
			else read(path);
		}
		nlohmann::json result = Preview(combined, "local", workspace, search);
		result["sources"] = std::move(sources);
		if (fallback) result["warnings"].push_back("No global OpenCode configuration directory was found. This preview uses the explicitly listed environment configuration instead.");
		return result;
	}

	/// Current capability handshake observations only. Configuration alone is never a connection proof.
	inline nlohmann::json Status(const AppState& app, const std::string& host, const std::string& workspace, const std::string& chat_id = {})
	{
		nlohmann::json providers = nlohmann::json::array();
		const int64_t now = time::SystemEpochMillisecondsNow();
		const ChatSession* selected = nullptr;
		if (!chat_id.empty())
		{
			const auto found = std::ranges::find(app.chats, chat_id, &ChatSession::id);
			if (found == app.chats.end() || found->execution_host_id != host) throw std::runtime_error("Chat no longer belongs to the selected execution host.");
			selected = &*found;
		}
		const std::vector<McpServerConfiguration> servers = mcp_server_config::SelectForWorkspace(app.settings.mcp_servers, workspace, host);
		for (const char* provider : provider_ids::kAllCliProviderIds)
		{
			if (selected != nullptr && selected->provider_id != provider) continue;
			const bool enabled = central_configuration::McpEnabled(app.settings.central_provider_configuration, provider, host, "uam-mcp", selected != nullptr ? selected->uam_control_enabled : app.settings.central_provider_configuration.uam_control_enabled);
			std::size_t active = 0, configured = 0, connected = 0, live = 0;
			bool provider_failed = false;
			const auto observe = [&](const AcpSessionState& session, bool running)
			{
				const auto chat = std::ranges::find(app.chats, session.chat_id, &ChatSession::id);
				if (session.model_discovery_only || session.provider_id != provider || (!session.process_execution_host_id.empty() && session.process_execution_host_id != host) || chat == app.chats.end() || chat->execution_host_id != host ||
				    (selected != nullptr && session.chat_id != chat_id)) return;
				if (!running) { provider_failed = provider_failed || !session.last_error.empty(); return; }
				++active;
				const auto capability = std::ranges::find(app.uam_control_capabilities, session.uam_control_capability_id, &UamControlCapability::id);
				if (capability == app.uam_control_capabilities.end() || capability->session_chat_id != session.chat_id || capability->provider_id != provider ||
				    (!chat->uam_control_enabled && !capability->session_control_override_at_launch) || capability->expires_at_epoch_ms <= now || !paths::IsDirectoryNoThrow(capability->directory) ||
				    paths::IsLinkOrReparsePointNoThrow(capability->directory)) return;
				++configured;
				if (capability->mcp_initialized && capability->mcp_initialized_at_epoch_ms > 0)
				{
					++connected;
					if (capability->mcp_heartbeat_at_epoch_ms > 0 && capability->mcp_heartbeat_at_epoch_ms <= now && now - capability->mcp_heartbeat_at_epoch_ms < 20000) ++live;
				}
			};
			for (const std::unique_ptr<AcpSessionState>& session : app.acp_sessions) if (session != nullptr) observe(*session, session->running);
			for (const std::unique_ptr<CliTerminalState>& terminal : app.cli_terminals)
				if (terminal != nullptr && terminal->uam_control_session != nullptr) observe(*terminal->uam_control_session, terminal->running);
			const std::string state = live > 0 ? "connected" : connected > 0 ? "handshake-confirmed" : configured > 0 ? "configured-unverified" : !enabled || (selected != nullptr && selected->uam_control_explicitly_disabled) ? "disabled" : "missing";
			const std::string detail = state == "disabled" ? "UAM tools are disabled for this scope." : state == "connected" ? "The running provider MCP connection has a current heartbeat." : state == "handshake-confirmed" ? "UAM MCP handshake confirmed for this running session. Waiting for a current connection heartbeat." :
			    state == "configured-unverified" ? "UAM MCP is installed for a running session. No handshake has been confirmed." : provider_failed ? "The provider session failed. Check its chat error; an MCP failure has not been established." : "UAM MCP is installed when the next enabled session starts.";
			nlohmann::json server_settings = nlohmann::json::array();
			for (const McpServerConfiguration& server : app.settings.mcp_servers)
				if (server.execution_host_id.empty() || server.execution_host_id == host)
					server_settings.push_back({{"id", server.id}, {"name", server.name}, {"enabled", central_configuration::McpEnabled(app.settings.central_provider_configuration, provider, host, server.id, server.enabled)}});
			providers.push_back({{"providerId", provider}, {"uamControlEnabled", enabled}, {"sharedSetupEnabled", central_configuration::McpEnabled(app.settings.central_provider_configuration, provider, host, "shared-setup", app.settings.central_provider_configuration.enabled)}, {"mcpServers", server_settings}, {"status", state}, {"detail", detail}, {"activeSessionCount", active},
			    {"configuredSessionCount", configured}, {"connectedSessionCount", connected}, {"additionalServerCount", servers.size()}, {"providerSessionFailed", provider_failed}});
		}
		return {{"executionHostId", host}, {"computerUseBuiltInAvailable", UAM_ENABLE_COMPUTER_USE && host == "local"}, {"providers", providers}, {"repairMode", "next-session-launch"}, {"connectedProofAvailable", true}, {"handshakeProofAvailable", true}};
	}
}
