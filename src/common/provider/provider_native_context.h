#pragma once

#include "common/provider/provider_ids.h"
#include "common/platform/platform_services.h"
#include "common/platform/platform_state_fields.h"
#include "common/utils/env_utils.h"
#include "common/utils/uuid.h"
#include "common/paths/path_utils.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <stop_token>
#include <cerrno>
#include <limits>
#include <filesystem>
#include <fstream>
#include <random>
#include <fcntl.h>
#if defined(_WIN32)
#include <io.h>
#include <sys/stat.h>
#include <process.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <signal.h>
#endif
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace uam::provider_native_context
{
	/// <summary>Creates a private settings file exclusively so an existing link cannot be followed.</summary>
	inline bool WritePrivateSettings(const std::filesystem::path& directory, std::string_view bytes,
	    std::filesystem::path& target)
	{
		std::random_device random;
		for (int attempt = 0; attempt < 8; ++attempt)
		{
			target = directory / ("context-settings-" + std::to_string(random()) + ".json");
#if defined(_WIN32)
			const int descriptor = _wopen(target.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
			const int descriptor = ::open(target.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
#endif
			if (descriptor < 0)
			{
				continue;
			}
			std::size_t written = 0;
			while (written < bytes.size())
			{
#if defined(_WIN32)
				const int count = _write(descriptor, bytes.data() + written, static_cast<unsigned int>(std::min<std::size_t>(bytes.size() - written, 16384)));
#else
				const ssize_t count = ::write(descriptor, bytes.data() + written, bytes.size() - written);
#endif
				if (count <= 0)
				{
					break;
				}
				written += static_cast<std::size_t>(count);
			}
#if defined(_WIN32)
			const bool closed = _close(descriptor) == 0;
#else
			const bool closed = ::close(descriptor) == 0;
#endif
			if (written == bytes.size() && closed)
			{
				return true;
			}
			std::error_code ignored;
			std::filesystem::remove(target, ignored);
			return false;
		}
		return false;
	}


	/// <summary>Resolves the configured storage/workspace root, then checks only the app-owned descendants.</summary>
	inline bool OwnedPathHasNoLinks(const std::filesystem::path& directory, std::string& error)
	{
		std::filesystem::path root = directory.parent_path();
		if (root.filename() == "runtime-context")
		{
			root = root.parent_path();
		}
		else if (root.filename() == "context" && root.parent_path().filename() == ".UAM")
		{
			root = root.parent_path().parent_path();
		}
		std::error_code ec;
		std::filesystem::path current = std::filesystem::weakly_canonical(root, ec);
		if (ec)
		{
			return false;
		}
		for (const std::filesystem::path& component : directory.lexically_relative(root))
		{
			if (component == ".." || component == ".")
			{
				return false;
			}
			current /= component;
			const std::filesystem::file_status status = std::filesystem::symlink_status(current, ec);
			if (ec == std::errc::no_such_file_or_directory)
			{
				ec.clear();
				continue;
			}
			if (ec || std::filesystem::is_symlink(status))
			{
				error = "Provider context requires owned storage without symbolic links.";
				return false;
			}
		}
		return true;
	}

	/// <summary>Deletes listed owned files after exit; remote receipts make retries safe across host changes.</summary>
	inline bool RemoveOwnedContext(const std::filesystem::path& directory, bool check_process, std::string& error)
	{
		std::error_code ec;
		if (!std::filesystem::exists(directory, ec))
		{
			if (check_process)
			{
				error = "Provider context cleanup could not confirm the original remote storage.";
			}
			return !check_process && !ec;
		}
		if (!OwnedPathHasNoLinks(directory, error))
		{
			return false;
		}
		const std::filesystem::path marker = directory / "owner.json";
		const std::filesystem::file_status marker_status = std::filesystem::symlink_status(marker, ec);
		if (ec == std::errc::no_such_file_or_directory)
		{
			return !check_process;
		}
		if (std::filesystem::is_symlink(marker_status) || ec)
		{
			error = "Provider context ownership could not be confirmed.";
			return false;
		}
		std::ifstream input(marker);
		const nlohmann::json owner = nlohmann::json::parse(input, nullptr, false);
		input.close();
		if (!owner.is_object() || (!owner.contains("owner") || !owner["owner"].is_string() || owner["owner"].get<std::string>() != uam::paths::Utf8PathString(directory.filename())) || !owner.contains("files") || !owner["files"].is_array())
		{
			error = "Provider context ownership could not be confirmed.";
			return false;
		}
		if (owner.contains("runnerPid") && (!owner["runnerPid"].is_number_integer() || owner["runnerPid"] < 0 || owner["runnerPid"] > std::numeric_limits<int>::max()))
		{
			return false;
		}
		const int process_id = owner.value("runnerPid", 0);
		if (check_process && process_id > 0)
		{
#if defined(_WIN32)
			HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(process_id));
			const bool running = process != nullptr ? WaitForSingleObject(process, 0) == WAIT_TIMEOUT : GetLastError() != ERROR_INVALID_PARAMETER;
			if (process != nullptr)
			{
				CloseHandle(process);
			}
#else
			const bool running = ::kill(process_id, 0) == 0 || errno != ESRCH;
#endif
			if (running)
			{
				error = "Provider context cleanup is waiting for its terminal process to exit.";
				return false;
			}
		}
		std::vector<std::filesystem::path> files;
		for (const nlohmann::json& name : owner["files"])
		{
			if (!name.is_string())
			{
				return false;
			}
			const std::filesystem::path relative = uam::paths::PathFromUtf8(name.get<std::string>());
			if (relative.empty() || relative.is_absolute())
			{
				return false;
			}
			std::filesystem::path current = directory;
			for (const std::filesystem::path& component : relative)
			{
				if (component == ".." || component == ".")
				{
					return false;
				}
				current /= component;
				const std::filesystem::file_status status = std::filesystem::symlink_status(current, ec);
				if (ec == std::errc::no_such_file_or_directory)
				{
					ec.clear();
					continue;
				}
				if (ec || std::filesystem::is_symlink(status))
				{
					return false;
				}
			}
			files.push_back(directory / relative);
		}
		for (const std::filesystem::path& file : files)
		{
			(void)std::filesystem::remove(file, ec);
			if (ec)
			{
				return false;
			}
		}
		if (check_process)
		{
			// Keep a receipt on the original machine so retry never mistakes another
			// machine's absent directory for successful cleanup.
			const nlohmann::json receipt{{"owner", owner["owner"]}, {"files", nlohmann::json::array()}, {"runnerPid", 0}};
			std::filesystem::path temporary;
			if (!WritePrivateSettings(directory, receipt.dump(), temporary))
			{
				return false;
			}
#if defined(_WIN32)
			std::filesystem::remove(marker, ec);
#endif
			std::filesystem::rename(temporary, marker, ec);
			return !ec;
		}
		(void)std::filesystem::remove(marker, ec);
		if (ec)
		{
			return false;
		}
		// Only empty directories are removed. Unrelated files are preserved.
		(void)std::filesystem::remove(directory / ".github/instructions", ec);
		ec.clear();
		(void)std::filesystem::remove(directory / ".github", ec);
		ec.clear();
		(void)std::filesystem::remove(directory, ec);
		return !ec || ec == std::errc::directory_not_empty;
	}

	/// <summary>Prepares private storage while refusing to replace context still consumed by a live CLI.</summary>
	inline bool PrepareOwnedContext(const std::filesystem::path& directory, std::string& error)
	{
		std::error_code ec;
		if (!OwnedPathHasNoLinks(directory, error))
		{
			return false;
		}
		if (std::filesystem::exists(directory, ec))
		{
			if (std::filesystem::exists(directory / "owner.json", ec))
			{
				if (!RemoveOwnedContext(directory, true, error))
				{
					return false;
				}
				std::filesystem::remove(directory / "owner.json", ec);
				if (ec)
				{
					return false;
				}
			}
			else if (!std::filesystem::is_empty(directory, ec) || ec)
			{
				error = "Provider context directory contains unowned files.";
				return false;
			}
		}
		std::filesystem::create_directories(directory, ec);
		if (ec)
		{
			return false;
		}
		std::filesystem::permissions(directory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec);
		if (ec)
		{
			return false;
		}
		const nlohmann::json owner{{"owner", uam::paths::Utf8PathString(directory.filename())},
		    {"files", {"conversation.md", "GEMINI.md", ".github/instructions/handoff.instructions.md"}}};
		std::filesystem::path temporary;
		if (!WritePrivateSettings(directory, owner.dump(), temporary))
		{
			return false;
		}
		std::filesystem::rename(temporary, directory / "owner.json", ec);
		return !ec;
	}

	/// <summary>Uses the native profile-aware prompt renderer to isolate custom instructions without model calls.</summary>
	inline bool ReadCodexProfileInstructions(IPlatformProcessService& service, const std::vector<std::string>& arguments,
	    const std::filesystem::path& workspace, const std::vector<std::pair<std::string, std::string>>& environment,
	    std::string& instructions, std::string& error, std::stop_token stop_token)
	{
		const std::function<bool(bool, std::vector<std::string>&)> render = [&](bool clear, std::vector<std::string>& texts)
		{
			std::vector<std::string> command = arguments;
			command.insert(command.end(), {"debug", "prompt-input"});
			if (clear) command.insert(command.end(), {"-c", "developer_instructions=\"\""});
			uam::platform::StdioProcessPlatformFields process;
			if (!service.StartStdioProcess(process, workspace, command, &error, environment)) return false;
			struct Guard
			{
				IPlatformProcessService& service;
				uam::platform::StdioProcessPlatformFields& process;
				~Guard()
				{
					service.StopStdioProcess(process, true);
					service.CloseStdioProcessHandles(process);
				}
			} guard{service, process};
			std::string output;
			std::array<char, 16384> bytes{};
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			bool exited = false;
			while (!stop_token.stop_requested() && std::chrono::steady_clock::now() < deadline)
			{
				const std::ptrdiff_t read = service.ReadStdioProcessStdout(process, bytes.data(), bytes.size(), nullptr);
				if (read > 0) output.append(bytes.data(), static_cast<std::size_t>(read));
				(void)service.ReadStdioProcessStderr(process, bytes.data(), bytes.size(), nullptr);
				if (output.size() > 1024 * 1024) return false;
				if (read <= 0 && service.PollStdioProcessExited(process, nullptr)) { exited = true; break; }
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			if (!exited || stop_token.stop_requested()) return false;
			const nlohmann::json rendered = nlohmann::json::parse(output, nullptr, false);
			if (!rendered.is_array()) return false;
			for (const nlohmann::json& message : rendered)
			{
				if (!message.is_object() || message.value("role", "") != "developer") continue;
				if (!message.contains("content") || !message["content"].is_array()) return false;
				for (const nlohmann::json& content : message["content"])
				{
					if (!content.is_object() || !content.contains("text") || !content["text"].is_string()) return false;
					texts.push_back(content["text"].get<std::string>());
				}
			}
			return true;
		};
		std::vector<std::string> configured;
		std::vector<std::string> cleared;
		if (!render(false, configured) || !render(true, cleared))
		{
			error = "Codex profile instructions could not be read for context handoff.";
			return false;
		}
		std::size_t index = 0;
		std::vector<std::string> extra;
		for (const std::string& text : configured)
		{
			if (index < cleared.size() && text == cleared[index]) ++index;
			else extra.push_back(text);
		}
		if (index != cleared.size() || extra.size() > 1)
		{
			error = "Codex profile instructions could not be separated from native policy safely.";
			return false;
		}
		instructions = extra.empty() ? std::string{} : extra.front();
		return true;
	}

	/// <summary>Imports actual prior context into a native thread without submitting a model turn.</summary>
	inline bool ImportCodexContext(IPlatformProcessService& service, std::vector<std::string>& argv,
	    const std::filesystem::path& workspace, const std::vector<std::pair<std::string, std::string>>& environment,
	    const std::filesystem::path& context_file, std::string& error, std::stop_token stop_token = {})
	{
		if (argv.empty()) return false;
		std::error_code size_error;
		const std::uintmax_t context_size = std::filesystem::file_size(context_file, size_error);
		if (size_error || context_size == 0 || context_size > 4 * 1024 * 1024)
		{ error = "Codex prior context is unavailable or exceeds the 4 MiB import limit."; return false; }
		std::ifstream input(context_file, std::ios::binary);
		std::string context((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		if (!input || context.empty() || context.size() > 4 * 1024 * 1024)
		{ error = "Codex prior context is unavailable or exceeds the 4 MiB import limit."; return false; }
		std::vector<std::string> probe = argv;
		const std::vector<std::string>::iterator resume = std::find(probe.begin() + 1, probe.end(), "resume");
		if (resume != probe.end())
		{
			probe.erase(resume);
			const std::vector<std::string>::iterator identity = std::find_if(probe.begin() + 1, probe.end(), [](const std::string& argument) { return uam::uuid::IsCanonicalUuid(argument); });
			if (identity != probe.end()) probe.erase(identity);
		}
		std::erase(probe, "--no-alt-screen");
		nlohmann::json start{{"cwd", uam::paths::Utf8PathString(workspace)}, {"persistExtendedHistory", true}};
		const bool profile_selected = std::ranges::any_of(probe, [](const std::string& argument)
		{
			return argument == "-p" || argument == "--profile" || argument.starts_with("--profile=");
		});
		if (profile_selected)
		{
			std::string instructions;
			if (!ReadCodexProfileInstructions(service, probe, workspace, environment, instructions, error, stop_token)) return false;
			start["developerInstructions"] = instructions;
		}
		for (std::size_t index = 1; index < probe.size(); ++index)
		{
			if ((probe[index] == "-p" || probe[index] == "--profile" || probe[index] == "-m" || probe[index] == "--model") && index + 1 < probe.size())
			{
				if (probe[index] == "-m" || probe[index] == "--model") start["model"] = probe[index + 1];
				probe.erase(probe.begin() + index, probe.begin() + index + 2);
				--index;
			}
			else if (probe[index].starts_with("--profile="))
			{
				probe.erase(probe.begin() + index);
				--index;
			}
		}
		// Import starts no model turn; these TUI flags have no app-server equivalent.
		for (const std::string_view flag : {"--full-auto", "--no-daemon", "--worktree", "--dangerously-bypass-hook-trust"})
			std::erase(probe, std::string(flag));
		probe.push_back("app-server");
		uam::platform::StdioProcessPlatformFields process;
		if (!service.StartStdioProcess(process, workspace, probe, &error, environment)) return false;
		struct Guard
		{
			IPlatformProcessService& service;
			uam::platform::StdioProcessPlatformFields& process;
			~Guard()
			{
				service.StopStdioProcess(process, true);
				service.CloseStdioProcessHandles(process);
			}
		} guard{service, process};
		std::string buffered;
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		const std::function<bool(int, std::string_view, const nlohmann::json&, nlohmann::json&)> request =
		    [&](int id, std::string_view method, const nlohmann::json& params, nlohmann::json& result)
		    {
			    const std::string line = nlohmann::json{{"id", id}, {"method", method}, {"params", params}}.dump() + "\n";
			    if (!service.WriteToStdioProcess(process, line.data(), line.size(), nullptr)) return false;
			    std::array<char, 16384> bytes{};
			    while (!stop_token.stop_requested() && std::chrono::steady_clock::now() < deadline)
			    {
				    const std::ptrdiff_t read = service.ReadStdioProcessStdout(process, bytes.data(), bytes.size(), nullptr);
				    if (read > 0) buffered.append(bytes.data(), static_cast<std::size_t>(read));
				    if (buffered.size() > 1024 * 1024) return false;
				    // Drain diagnostics without copying provider settings into app logs.
				    (void)service.ReadStdioProcessStderr(process, bytes.data(), bytes.size(), nullptr);
				    std::size_t end = buffered.find('\n');
				    while (end != std::string::npos)
				    {
					    const nlohmann::json response = nlohmann::json::parse(buffered.substr(0, end), nullptr, false);
					    buffered.erase(0, end + 1);
					    if (response.is_object() && response.contains("id") && response["id"] == id)
					    {
						    if (!response.contains("result")) return false;
						    result = response["result"];
						    return true;
					    }
					    end = buffered.find('\n');
				    }
				    if (service.PollStdioProcessExited(process, nullptr)) return false;
				    std::this_thread::sleep_for(std::chrono::milliseconds(10));
			    }
			    return false;
		    };
		nlohmann::json result;
		if (!request(1, "initialize", {{"clientInfo", {{"name", "uam-context-import"}, {"version", "1"}}}, {"capabilities", {{"experimentalApi", true}}}}, result))
		{ error = "Codex native context import could not initialize. Update Codex and retry."; return false; }
		const std::string initialized = "{\"method\":\"initialized\"}\n";
		if (!service.WriteToStdioProcess(process, initialized.data(), initialized.size(), nullptr) ||
		    !request(2, "thread/start", start, result) || !result.is_object() ||
		    !result.contains("thread") || !result["thread"].is_object() ||
		    !result["thread"].contains("id") || !result["thread"]["id"].is_string() ||
		    !uam::uuid::IsCanonicalUuid(result["thread"]["id"].get<std::string>()))
		{ error = "Codex did not create an owned thread for prior context."; return false; }
		const std::string identity = result["thread"]["id"].get<std::string>();
		const nlohmann::json item{{"type", "message"}, {"role", "assistant"},
		    {"content", nlohmann::json::array({{{"type", "output_text"}, {"text", context}}})}};
		if (!request(3, "thread/inject_items", {{"threadId", identity}, {"items", nlohmann::json::array({item})}}, result))
		{ error = "Codex could not import prior context. Update Codex and retry."; return false; }
		const std::vector<std::string>::iterator old_resume = std::find(argv.begin() + 1, argv.end(), "resume");
		if (old_resume != argv.end())
		{
			argv.erase(old_resume);
			const std::vector<std::string>::iterator old_identity = std::find_if(argv.begin() + 1, argv.end(), [](const std::string& argument) { return uam::uuid::IsCanonicalUuid(argument); });
			if (old_identity != argv.end()) argv.erase(old_identity);
		}
		argv.insert(argv.begin() + 1, {"resume", identity});
		return true;
	}

	/// <summary>Adds only provider-native context inputs, preserving the executing host's existing settings.</summary>
	inline bool ConfigureEnvironment(std::string_view provider_id, const std::filesystem::path& directory,
	    std::vector<std::pair<std::string, std::string>>& environment, std::string& error,
	    const std::filesystem::path& workspace = {}, bool remote = false,
	    IPlatformProcessService* process_service = nullptr, std::vector<std::string>* argv = nullptr, std::stop_token stop_token = {})
	{
		const std::string path = uam::paths::Utf8PathString(directory);
		std::ifstream manifest_input(directory / "owner.json");
		nlohmann::json manifest = nlohmann::json::parse(manifest_input, nullptr, false);
		manifest_input.close();
		if (!manifest.is_object() || (!manifest.contains("owner") || !manifest["owner"].is_string() || manifest["owner"].get<std::string>() != uam::paths::Utf8PathString(directory.filename())) || !manifest.contains("files") || !manifest["files"].is_array())
		{
			error = "Provider context ownership could not be confirmed.";
			return false;
		}
		if (remote)
		{
#if defined(_WIN32)
			manifest["runnerPid"] = _getpid();
#else
			manifest["runnerPid"] = static_cast<int>(::getpid());
#endif
		}
		if (provider_id == uam::provider_ids::kCodexCli)
		{
			if (process_service == nullptr || argv == nullptr || !ImportCodexContext(*process_service, *argv, workspace, environment, directory / "conversation.md", error, stop_token)) return false;
		}
		else if (provider_id == uam::provider_ids::kOpenCodeCli)
		{
			nlohmann::json config = nlohmann::json::object();
			if (const std::optional<std::string> existing = uam::env::GetNonEmptyString("OPENCODE_CONFIG_CONTENT"))
			{
				config = nlohmann::json::parse(*existing, nullptr, false, true);
				if (!config.is_object())
				{
					error = "OpenCode inline configuration is invalid.";
					return false;
				}
			}
			if (!config.contains("instructions"))
			{
				config["instructions"] = nlohmann::json::array();
			}
			if (!config["instructions"].is_array())
			{
				error = "OpenCode instructions must be a list of files.";
				return false;
			}
			const std::string context = uam::paths::Utf8PathString(directory / "conversation.md");
			if (std::find(config["instructions"].begin(), config["instructions"].end(), nlohmann::json(context)) == config["instructions"].end())
			{
				config["instructions"].push_back(context);
			}
			environment.emplace_back("OPENCODE_CONFIG_CONTENT", config.dump());
		}
		else if (provider_id == uam::provider_ids::kCopilotCli)
		{
			std::string directories = path;
			if (const std::optional<std::string> existing = uam::env::GetNonEmptyString("COPILOT_CUSTOM_INSTRUCTIONS_DIRS"))
			{
				directories += "," + *existing;
			}
			environment.emplace_back("COPILOT_CUSTOM_INSTRUCTIONS_DIRS", directories);
		}
		else if (provider_id == uam::provider_ids::kGeminiCli)
		{
			std::filesystem::path original;
			if (const std::optional<std::string> configured = uam::env::GetNonEmptyString("GEMINI_CLI_SYSTEM_SETTINGS_PATH"))
			{
				original = uam::paths::PathFromUtf8(*configured);
			}
			else
			{
#if defined(_WIN32)
				original = "C:\\ProgramData\\gemini-cli\\settings.json";
#elif defined(__APPLE__)
				original = "/Library/Application Support/GeminiCli/settings.json";
#else
				original = "/etc/gemini-cli/settings.json";
#endif
			}
			nlohmann::json settings = nlohmann::json::object();
			std::error_code ec;
			if (std::filesystem::exists(original, ec))
			{
				std::ifstream source(original, std::ios::binary);
				std::ostringstream bytes;
				bytes << source.rdbuf();
				settings = nlohmann::json::parse(bytes.str(), nullptr, false, true);
				if (!source || !settings.is_object())
				{
					error = "Gemini system settings could not be preserved for context handoff.";
					return false;
				}
			}
			else if (ec)
			{
				error = "Gemini system settings are unavailable.";
				return false;
			}
			// The provider may use custom memory filenames. Stage the snapshot under those
			// names too, rather than changing the user's context.fileName preference.
			std::vector<std::filesystem::path> sources{original};
			if (const std::optional<std::string> defaults = uam::env::GetNonEmptyString("GEMINI_CLI_SYSTEM_DEFAULTS_PATH"))
				sources.push_back(uam::paths::PathFromUtf8(*defaults));
			else
			{
				sources.push_back(original.parent_path() / "system-defaults.json");
			}
			std::optional<std::string> home = uam::env::GetNonEmptyString("GEMINI_CLI_HOME");
			if (!home)
			{
				home = uam::env::GetNonEmptyString("HOME");
			}
#if defined(_WIN32)
			if (!home)
			{
				home = uam::env::GetNonEmptyString("USERPROFILE");
			}
#endif
			if (home)
			{
				sources.push_back(uam::paths::PathFromUtf8(*home) / ".gemini/settings.json");
			}
			if (!workspace.empty())
			{
				sources.push_back(workspace / ".gemini/settings.json");
			}
			for (const std::filesystem::path& source_path : sources)
			{
				if (!std::filesystem::exists(source_path, ec))
				{
					if (ec)
					{
						error = "Gemini memory settings are unavailable.";
						return false;
					}
					continue;
				}
				std::ifstream input(source_path, std::ios::binary);
				std::ostringstream contents;
				contents << input.rdbuf();
				const nlohmann::json configured = nlohmann::json::parse(contents.str(), nullptr, false, true);
				if (!input || !configured.is_object())
				{
					error = "Gemini memory settings could not be preserved.";
					return false;
				}
				if (!configured.contains("context") || !configured["context"].is_object() || !configured["context"].contains("fileName"))
				{
					continue;
				}
				const nlohmann::json& names = configured["context"]["fileName"];
				const nlohmann::json filenames = names.is_string() ? nlohmann::json::array({names}) : names;
				if (!filenames.is_array())
				{
					error = "Gemini memory filenames are invalid.";
					return false;
				}
				for (const nlohmann::json& name : filenames)
				{
					if (!name.is_string())
					{
						error = "Gemini memory filenames are invalid.";
						return false;
					}
					const std::string filename = name.get<std::string>();
					if (filename.empty() || filename == "." || filename == ".." || filename.find_first_of("/\\") != std::string::npos)
					{
						error = "Gemini memory filenames must be simple file names for context handoff.";
						return false;
					}
					if (filename == "GEMINI.md" || filename == "conversation.md")
					{
						continue;
					}
					std::ifstream context(directory / "conversation.md", std::ios::binary);
					std::ostringstream snapshot;
					snapshot << context.rdbuf();
					std::filesystem::path temporary;
					if (!context || !WritePrivateSettings(directory, snapshot.str(), temporary))
					{
						error = "Gemini memory context could not be saved.";
						return false;
					}
#if defined(_WIN32)
					std::filesystem::remove(directory / filename, ec);
#endif
					std::filesystem::rename(temporary, directory / filename, ec);
					if (ec)
					{
						error = "Gemini memory context could not be saved.";
						return false;
					}
					manifest["files"].push_back(filename);
				}
			}
			if (settings.contains("context") && !settings["context"].is_object())
			{
				error = "Gemini context settings are invalid.";
				return false;
			}
			settings["context"]["loadMemoryFromIncludeDirectories"] = true;
			std::filesystem::path target;
			if (!WritePrivateSettings(directory, settings.dump(), target))
			{
				error = "Gemini context settings could not be saved privately.";
				return false;
			}
			const std::filesystem::path settings_target = directory / "context-settings.json";
#if defined(_WIN32)
			std::filesystem::remove(settings_target, ec);
#endif
			std::filesystem::rename(target, settings_target, ec);
			if (ec)
			{
				return false;
			}
			target = settings_target;
			manifest["files"].push_back("context-settings.json");
			environment.emplace_back("GEMINI_CLI_SYSTEM_SETTINGS_PATH", uam::paths::Utf8PathString(target));
		}
		std::filesystem::path temporary;
		if (!WritePrivateSettings(directory, manifest.dump(), temporary))
		{
			return false;
		}
		std::error_code save_error;
#if defined(_WIN32)
		std::filesystem::remove(directory / "owner.json", save_error);
#endif
		std::filesystem::rename(temporary, directory / "owner.json", save_error);
		return !save_error;
	}
}
