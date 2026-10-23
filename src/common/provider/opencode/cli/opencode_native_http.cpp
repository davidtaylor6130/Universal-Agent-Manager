#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "common/provider/opencode_native_http.h"
#include "common/platform/platform_services.h"
#include "common/platform/platform_state_fields.h"
#include "common/paths/path_utils.h"
#include "common/utils/base64.h"
#include "common/utils/env_utils.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <charconv>
#include <thread>

namespace uam::opencode_native
{
	namespace
	{
		bool PortAvailable(int port)
		{
#if defined(_WIN32)
			WSADATA startup{};
			if (WSAStartup(MAKEWORD(2, 2), &startup) != 0) return false;
			const SOCKET listener = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
			if (listener == INVALID_SOCKET) { WSACleanup(); return false; }
			const BOOL exclusive = TRUE;
			(void)setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#else
			const int listener = socket(AF_INET, SOCK_STREAM, 0);
			if (listener < 0) return false;
			if (fcntl(listener, F_SETFD, FD_CLOEXEC) < 0) { close(listener); return false; }
#endif
			sockaddr_in address{};
			address.sin_family = AF_INET;
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			address.sin_port = htons(static_cast<unsigned short>(port));
			const bool available = bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
#if defined(_WIN32)
			closesocket(listener); WSACleanup();
#else
			close(listener);
#endif
			return available;
		}
		std::string Encode(std::string_view text)
		{
			std::string result;
			constexpr char digits[] = "0123456789ABCDEF";
			for (const unsigned char c : text)
			{
				if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') result += static_cast<char>(c);
				else { result += '%'; result += digits[c >> 4]; result += digits[c & 15]; }
			}
			return result;
		}
		std::string ConfigString(std::string_view value)
		{
			std::string escaped = "\"";
			for (const char c : value) { if (c == '\\' || c == '"') escaped += '\\'; escaped += c; }
			return escaped + "\"";
		}
	}

	bool ValidNativeId(const std::string& id)
	{
		return !id.empty() && id.size() <= 512 && std::ranges::all_of(id, [](unsigned char c)
		{ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; });
	}
	bool ValidEndpoint(const Endpoint& endpoint)
	{
		return endpoint.port >= 1024 && endpoint.port <= 65535 && !endpoint.token.empty() && endpoint.token.size() <= 256 &&
		    std::ranges::all_of(endpoint.token, [](unsigned char c) { return c >= 33 && c <= 126; });
	}
	bool IsNativeExecutable(const std::string& command, const std::string& platform)
	{
		std::string executable = command;
		const bool windows = platform == "windows" || platform == "Windows";
		if (windows)
		{
			std::ranges::replace(executable, '\\', '/');
			std::ranges::transform(executable, executable.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		}
		const std::string filename = std::filesystem::path(executable).filename().string();
		return filename == "opencode" || (windows && (filename == "opencode.exe" || filename == "opencode.cmd"));
	}
	bool SupportsCompanionLaunch(const std::vector<std::string>& argv, const std::string& platform)
	{
		if (argv.empty() || !IsNativeExecutable(argv.front(), platform) || std::find(argv.begin() + 1, argv.end(), "acp") == argv.end()) return false;
		return !std::ranges::any_of(argv, [](const std::string& value)
		{
			return value == "attach" || value == "--mini" || value.starts_with("--mini=") || value == "--port" || value.starts_with("--port=") ||
			    value == "--hostname" || value.starts_with("--hostname=") || value == "--mdns" || value.starts_with("--mdns=") ||
			    value == "--no-mdns" || value == "--cors" || value.starts_with("--cors=");
		});
	}

	bool RemoveConflictingTerminalObserver(std::vector<std::string>& argv, std::vector<std::pair<std::string, std::string>>& environment,
	    const std::string& platform, PortProbe probe)
	{
		if (argv.size() < 6 || !IsNativeExecutable(argv.front(), platform)) return false;
		const std::size_t suffix = argv.size() - 5;
		if (argv[suffix] != "--hostname" || argv[suffix + 1] != "127.0.0.1" || argv[suffix + 2] != "--port" || argv.back() != "--no-mdns") return false;
		int port = 0;
		const std::string& text = argv[suffix + 3];
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), port);
		if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return false;
		std::size_t username = environment.size(), password = environment.size();
		for (std::size_t index = environment.size(); index-- > 0;)
		{
			if (username == environment.size() && environment[index].first == "OPENCODE_SERVER_USERNAME") username = index;
			if (password == environment.size() && environment[index].first == "OPENCODE_SERVER_PASSWORD") password = index;
		}
		if (username == environment.size() || password == environment.size() || environment[username].second != "opencode" || !ValidEndpoint({port, environment[password].second})) return false;
		if ((probe ? probe : PortProbe(PortAvailable))(port)) return false;
		argv.resize(suffix);
		environment.erase(environment.begin() + static_cast<std::ptrdiff_t>(std::max(username, password)));
		environment.erase(environment.begin() + static_cast<std::ptrdiff_t>(std::min(username, password)));
		return true;
	}

	bool BuildRequest(const Endpoint& endpoint, const std::string& workspace, const nlohmann::json& operation,
	    std::string& url, std::string& method, std::string& config, std::string& error)
	try
	{
		url.clear(); method = "GET"; config.clear();
		if (!ValidEndpoint(endpoint) || workspace.empty() || workspace.size() > 4096 || workspace.find_first_of("\r\n\0", 0, 3) != std::string::npos || !operation.is_object())
		{ error = "The native OpenCode companion has invalid process authority."; return false; }
		const std::string kind = operation.value("operation", "");
		const std::string session = operation.value("nativeSessionId", "");
		std::string route;
		nlohmann::json payload;
		if (kind == "health") route = "/global/health";
		else if (kind == "configProviders") route = "/config/providers";
		else if (kind == "permissions") route = "/permission";
		else if (kind == "status") route = "/session/status";
		else if ((kind == "session" || kind == "messages" || kind == "enableTask" || kind == "disableTask") && ValidNativeId(session))
		{
			route = "/session/" + session;
			if (kind == "messages") route += "/message";
			if (kind == "enableTask" || kind == "disableTask")
			{
				if (kind == "enableTask" && operation.value("readOnly", false))
				{ error = "Native OpenCode delegation remains disabled for read-only turns."; return false; }
				method = "PATCH";
				payload = {{"permission", nlohmann::json::array({{{"permission", "task"}, {"pattern", "*"}, {"action", kind == "disableTask" ? "deny" : "ask"}}})}};
			}
		}
		else if (kind == "reply" && ValidNativeId(operation.value("requestId", "")))
		{
			const std::string reply = operation.value("reply", "");
			if (reply != "once" && reply != "reject")
			{ error = "Native child permissions require an explicit one-time decision."; return false; }
			route = "/permission/" + operation.value("requestId", "") + "/reply";
			method = "POST"; payload = {{"reply", reply}};
		}
		else { error = "The native OpenCode operation is unsupported."; return false; }
		url = "http://127.0.0.1:" + std::to_string(endpoint.port) + route;
		if (kind != "health") url += "?directory=" + Encode(workspace);
		if (kind == "messages") url += "&limit=2";
		config = "header = " + ConfigString("Authorization: Basic " + base64::Encode("opencode:" + endpoint.token)) + "\n";
		if (!payload.is_null()) config += "data = " + ConfigString(payload.dump()) + "\n";
		return true;
	}
	catch (const nlohmann::json::exception&)
	{ error = "The native OpenCode operation has invalid fields."; return false; }

	bool Query(IPlatformProcessService& service, const Endpoint& endpoint, const std::string& workspace,
	    const nlohmann::json& operation, nlohmann::json& result, std::string& error, std::stop_token cancel)
	{
		result = nullptr;
		std::string url, method, config;
		if (!BuildRequest(endpoint, workspace, operation, url, method, config, error)) return false;
#if defined(_WIN32)
		const std::string system = env::GetNonEmptyString("SystemRoot").value_or("");
		if (system.empty()) { error = "The system HTTP client is unavailable."; return false; }
		const std::string executable = paths::Utf8PathString(paths::PathFromUtf8(system) / "System32" / "curl.exe");
#else
		const std::string executable = "/usr/bin/curl";
#endif
		const std::vector<std::string> argv{executable, "-q", "--silent", "--noproxy", "*", "--proto", "=http", "--connect-timeout", "1", "--max-time", "2", "--max-filesize", "262144", "--request", method,
		    "--header", "Content-Type: application/json", "--config", "-", "--write-out", "\n%{http_code}", url};
		platform::StdioProcessPlatformFields process;
		if (cancel.stop_requested() || !service.StartStdioProcessWithInput(process, {}, argv, config, nullptr))
		{ std::fill(config.begin(), config.end(), '\0'); error = "The native OpenCode HTTP client could not start."; return false; }
		std::fill(config.begin(), config.end(), '\0');
		struct Guard
		{
			IPlatformProcessService& service; platform::StdioProcessPlatformFields& process;
			~Guard() { service.StopStdioProcess(process, true); service.CloseStdioProcessHandles(process); }
		} guard{service, process};
		std::string output;
		std::array<char, 8192> buffer{};
		int exit = -1;
		bool exited = false;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (!cancel.stop_requested() && std::chrono::steady_clock::now() < deadline)
		{
			const auto read = service.ReadStdioProcessStdout(process, buffer.data(), buffer.size(), nullptr);
			// -2 means the owned process has no bytes available yet, not a failed read.
			if (read < 0 && read != -2) break;
			if (read > 0)
			{
				if (output.size() + static_cast<std::size_t>(read) > 262160) break;
				output.append(buffer.data(), static_cast<std::size_t>(read));
				continue;
			}
			exited = service.PollStdioProcessExited(process, &exit);
			// Process exit can race with the empty read; drain to actual EOF before closing pipes.
			if (exited && read == 0) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		const auto marker = output.rfind('\n');
		if (!exited || exit != 0 || marker == std::string::npos || output.substr(marker + 1) != "200")
		{ error = cancel.stop_requested() ? "Native OpenCode control was cancelled." : "The owned native OpenCode server did not confirm the request."; return false; }
		result = nlohmann::json::parse(output.substr(0, marker), nullptr, false);
		if (result.is_discarded()) { result = nullptr; error = "The native OpenCode server returned invalid JSON."; return false; }
		return true;
	}
}
