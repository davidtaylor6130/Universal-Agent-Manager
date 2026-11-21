#include "common/provider/opencode_v2_session.h"
#include "common/platform/platform_services.h"
#include "common/platform/platform_state_fields.h"
#include "common/paths/path_utils.h"
#include "common/utils/base64.h"
#include "common/utils/env_utils.h"
#include "common/utils/io_utils.h"
#include "common/runtime/cli_version_parser.h"
#include <array>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>
#include <stdexcept>
#include <utility>

namespace uam::opencode_v2
{
namespace
{
using Clock = std::chrono::steady_clock;
constexpr std::size_t kMaxPacket = 16 * 1024 * 1024;
constexpr std::string_view kPermissionProbeAction = "uam.permission.probe";
struct Process
{
	platform::StdioProcessPlatformFields fields;
	IPlatformProcessService& service;
	explicit Process(IPlatformProcessService& process_service) : service(process_service) {}
	~Process() { service.StopStdioProcess(fields, true); service.CloseStdioProcessHandles(fields); }
};
std::string Capture(const std::vector<std::string>& argv, IPlatformProcessService& service, int seconds = 30)
{
	Process process(service);
	// Separate stdout/stderr like StreamV1: the merged-input launch failed every 2.1.4 export over 64 KiB.
	if (!process.service.StartStdioProcess(process.fields, {}, argv, nullptr)) throw std::runtime_error("OpenCode command could not start.");
	process.service.CloseStdioProcessInput(process.fields);
	std::array<char, 8192> bytes{}, diagnostics{};
	std::string output;
	int exit = -1;
	const auto deadline = Clock::now() + std::chrono::seconds(seconds);
	while (Clock::now() < deadline)
	{
		const auto count = process.service.ReadStdioProcessStdout(process.fields, bytes.data(), bytes.size());
		process.service.ReadStdioProcessStderr(process.fields, diagnostics.data(), diagnostics.size());
		if (count > 0)
		{
			if (output.size() + static_cast<std::size_t>(count) > 64 * 1024 * 1024) throw std::runtime_error("OpenCode history exceeds the bounded import size.");
			output.append(bytes.data(), static_cast<std::size_t>(count)); continue;
		}
		if (count < 0 && count != -2) break;
		if (process.service.PollStdioProcessExited(process.fields, &exit) && count == 0)
		{
			if (exit != 0) throw std::runtime_error("OpenCode command failed; native history was not replaced.");
			return output;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	throw std::runtime_error("OpenCode command did not finish within its bounded deadline.");
}
int StreamV1(const std::vector<std::string>& argv, IPlatformProcessService& service)
{
	// Preserve V1's bounded worker/SAX import path without buffering its complete
	// export in this dispatcher or mixing native diagnostics into JSON stdout.
	Process process(service);
	if (!process.service.StartStdioProcess(process.fields, {}, argv, nullptr)) throw std::runtime_error("OpenCode V1 history command could not start.");
	process.service.CloseStdioProcessInput(process.fields);
	std::array<char, 8192> bytes{}, diagnostics{};
	int exit = -1;
	const auto deadline = Clock::now() + std::chrono::seconds(30);
	while (Clock::now() < deadline)
	{
		const auto count = process.service.ReadStdioProcessStdout(process.fields, bytes.data(), bytes.size());
		process.service.ReadStdioProcessStderr(process.fields, diagnostics.data(), diagnostics.size());
		if (count > 0)
		{
			if (std::fwrite(bytes.data(), 1, static_cast<std::size_t>(count), stdout) != static_cast<std::size_t>(count)) return 1;
			continue;
		}
		if (count < 0 && count != -2) return 1;
		if (process.service.PollStdioProcessExited(process.fields, &exit) && count == 0) return std::fflush(stdout) == 0 && exit == 0 ? 0 : 1;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	throw std::runtime_error("OpenCode V1 history command timed out; the import was not published.");
}
void Output(const Json& value)
{
	const std::string line = value.dump(-1, ' ', true, Json::error_handler_t::replace) + '\n';
	if (std::fwrite(line.data(), 1, line.size(), stdout) != line.size() || std::fflush(stdout) != 0) throw std::runtime_error("UAM V2 output lease closed.");
}
std::string Quote(std::string_view value)
{
	std::string result = "\"";
	for (char c : value)
	{
		if (c == '\r' || c == '\n' || c == '\0') throw std::runtime_error("Invalid native HTTP configuration.");
		if (c == '\\' || c == '"') result += '\\';
		result += c;
	}
	return result + '"';
}
std::string Curl()
{
#if defined(_WIN32)
	const auto system = env::GetNonEmptyString("SystemRoot").value_or("");
	if (system.empty()) throw std::runtime_error("The system HTTP client is unavailable.");
	return paths::Utf8PathString(paths::PathFromUtf8(system) / "System32" / "curl.exe");
#else
	return "/usr/bin/curl";
#endif
}
// The newer-version bridge knows the initialize request identity before it runs
// checks that can fail. Report failure through the same owned startup response.
Json ReadInitialize(IPlatformProcessService& process, std::string& input)
{
	const auto deadline = Clock::now() + std::chrono::seconds(15);
	std::array<char, 8192> bytes{};
	while (Clock::now() < deadline)
	{
		const auto read = process.ReadStandardInput(bytes.data(), bytes.size());
		if (read.status == platform::StandardInputReadStatus::EndOfStream || read.status == platform::StandardInputReadStatus::Error) break;
		if (read.status == platform::StandardInputReadStatus::Data) input.append(bytes.data(), read.size);
		const auto end = input.find('\n');
		if (input.size() > kMaxPacket) break;
		if (end != std::string::npos)
		{
			const auto request = Json::parse(input.substr(0, end), nullptr, false);
			if (request.is_object() && request.contains("id") && request.at("id").is_number_integer() && request.value("method", Json{}) == "initialize")
				return request.at("id");
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	throw std::runtime_error("OpenCode V2 did not receive an owned initialize request. Input remains held.");
}

class Service
{
public:
	Process server, events;
	std::string url, password, nonce, captured_directory, version;
	std::filesystem::path directory, policy_path, proof_path, probe_path;
	Clock::time_point startup_deadline = Clock::time_point::max();
	bool connected = false;
	explicit Service(IPlatformProcessService& process_service) : server(process_service), events(process_service)
	{
		nonce = server.service.GenerateUuid(); password = server.service.GenerateUuid() + server.service.GenerateUuid();
		directory = std::filesystem::temp_directory_path() / ("uam-opencode-v2-" + nonce);
		if (!std::filesystem::create_directory(directory)) throw std::runtime_error("Could not allocate private OpenCode V2 configuration.");
		std::filesystem::permissions(directory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
		policy_path = directory / "policy.json"; proof_path = directory / "proof.json"; probe_path = directory / "probe.json";
	}
	~Service()
	{
		// Close lease, stop/reap both owned children before removing executable policy.
		server.service.CloseStdioProcessInput(server.fields);
		server.service.StopStdioProcess(server.fields, true);
		events.service.StopStdioProcess(events.fields, true);
		std::error_code ignored; std::filesystem::remove_all(directory, ignored);
	}
	void WritePolicy(const std::string& id, const std::string& cwd, bool read_only)
	{
		if (!io::AtomicWriteFile(policy_path, Json{{"nonce", nonce}, {"active", !id.empty()}, {"sessionID", id}, {"directory", cwd}, {"readOnly", read_only}}.dump())) throw std::runtime_error("Could not publish the captured OpenCode V2 policy.");
		captured_directory = cwd;
	}
	void DrainNativeLogs()
	{
		std::array<char, 8192> discarded{};
		server.service.ReadStdioProcessStdout(server.fields, discarded.data(), discarded.size());
		server.service.ReadStdioProcessStderr(server.fields, discarded.data(), discarded.size());
	}
	std::string Config(const Json& body = nullptr) const
	{
		std::string config = "header = " + Quote("Authorization: Basic " + base64::Encode("opencode:" + password)) + "\n";
		if (!body.is_null()) config += "data = " + Quote(body.dump()) + "\n";
		return config;
	}
	Json Query(const std::string& method, const std::string& route, const Json& body)
	{
		if (Clock::now() >= startup_deadline) throw std::runtime_error("OpenCode V2 exceeded the startup verification deadline.");
		if (!route.starts_with("/api/") || route.find_first_of("\r\n") != std::string::npos) throw std::runtime_error("Invalid OpenCode V2 native route.");
		Process process(server.service);
		std::string config = Config(body);
		// A queued DELETE followed by an owned-delivery interrupt must leave
		// room to drain SSE within the shared five-second cancellation budget.
		const bool cancel_control = (method == "DELETE" && route.find("/inbox/") != std::string::npos) || (method == "POST" && route.ends_with("/interrupt"));
		const std::vector<std::string> argv{Curl(), "-q", "--silent", "--globoff", "--noproxy", "*", "--proto", "=http", "--connect-timeout", cancel_control ? "1" : "2", "--max-time", cancel_control ? "1" : "15", "--max-filesize", "16777216", "--request", method, "--header", "Content-Type: application/json", "--config", "-", "--write-out", "\n%{http_code}", url + route};
		if (!process.service.StartStdioProcessWithInput(process.fields, {}, argv, config, nullptr)) throw std::runtime_error("OpenCode V2 HTTP request could not start.");
		std::fill(config.begin(), config.end(), '\0');
		std::string output; std::array<char, 8192> bytes{}; int exit = -1;
		const auto deadline = std::min(startup_deadline, Clock::now() + std::chrono::seconds(cancel_control ? 2 : 16));
		while (Clock::now() < deadline)
		{
			DrainNativeLogs();
			const auto count = process.service.ReadStdioProcessStdout(process.fields, bytes.data(), bytes.size());
			if (count > 0)
			{
				if (output.size() + static_cast<std::size_t>(count) > kMaxPacket) break;
				output.append(bytes.data(), static_cast<std::size_t>(count)); continue;
			}
			if (count < 0 && count != -2) break;
			if (process.service.PollStdioProcessExited(process.fields, &exit) && count == 0)
			{
				const auto marker = output.rfind('\n');
				if (exit != 0 || marker == std::string::npos || (output.substr(marker + 1) != "200" && output.substr(marker + 1) != "201" && output.substr(marker + 1) != "202" && output.substr(marker + 1) != "204")) break;
				if (marker == 0) return Json::object();
				Json parsed = Json::parse(output.substr(0, marker), nullptr, false);
				if (parsed.is_discarded()) break;
				return parsed;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		throw std::runtime_error("The owned OpenCode V2 service did not confirm the native request; input is not automatically replayed.");
	}
	Json ReadProof(const std::filesystem::path& path) const
	{
		std::ifstream file(path);
		std::array<char, 16385> bytes{};
		file.read(bytes.data(), bytes.size());
		const auto size = static_cast<std::size_t>(file.gcount());
		return size < bytes.size() ? Json::parse(std::string_view(bytes.data(), size), nullptr, false) : Json{};
	}
	void Proof(const std::string& cwd)
	{
		const auto deadline = std::min(startup_deadline, Clock::now() + std::chrono::seconds(10));
		while (Clock::now() < deadline)
		{
			DrainNativeLogs();
			const auto proof = ReadProof(proof_path);
			if (proof.is_object() && proof.value("nonce", Json{}) == nonce && proof.value("directory", Json{}) == cwd) return;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		throw std::runtime_error("OpenCode V2 did not load UAM's process-local permission guard. Input remains held.");
	}
	void VerifyDenied(const std::string& id, const std::string& cwd)
	{
		// The permission API evaluates a synthetic resource with no tool source,
		// model request or tool call. Even a failed guard cannot execute a tool.
		const std::string route = "/api/session/" + Encode(id) + "/permission";
		Json result;
		try
		{
			result = Query("POST", route, {{"action", kPermissionProbeAction}, {"resources", Json::array({nonce})}, {"save", Json::array()}});
		}
		catch (...)
		{
			throw std::runtime_error("OpenCode V2 permission evaluation endpoint did not confirm the deny probe. Input remains held.");
		}
		const auto data = result.is_object() ? result.value("data", Json{}) : Json{};
		const auto proof = ReadProof(probe_path);
		if (!data.is_object() || data.value("effect", Json{}) != "deny" ||
		    !proof.is_object() || proof != Json{{"nonce", nonce}, {"directory", cwd}, {"sessionID", id}})
			throw std::runtime_error("OpenCode V2 did not prove that UAM's permission hook denied the probe. Input remains held.");
	}
	void Start(const std::vector<std::string>& command, const std::string& expected_version)
	{
		WritePolicy({}, {}, true);
		const auto plugin = directory / "permission-plugin";
		std::filesystem::create_directory(plugin);
		if (!io::AtomicWriteFile(plugin / "package.json", R"({"name":"uam-v2-permission-boundary","type":"module","main":"server.js"})") || !io::AtomicWriteFile(plugin / "server.js", PermissionPluginSource())) throw std::runtime_error("Could not prepare the OpenCode V2 permission guard.");
		Json config = Json::object();
		if (const auto existing = env::GetNonEmptyString("OPENCODE_CONFIG_CONTENT")) config = Json::parse(*existing, nullptr, false, true);
		if (!config.is_object()) throw std::runtime_error("OpenCode inline configuration is invalid.");
		if (!config.contains("plugins")) config["plugins"] = Json::array();
		if (!config["plugins"].is_array()) throw std::runtime_error("OpenCode V2 plugins must be a list.");
		Json options{{"policyPath", paths::Utf8PathString(policy_path)}, {"proofPath", paths::Utf8PathString(proof_path)}, {"nonce", nonce}};
		if (!expected_version.empty())
		{
			options["probePath"] = paths::Utf8PathString(probe_path);
			options["probeAction"] = kPermissionProbeAction;
		}
		config["plugins"].push_back({{"package", paths::Utf8PathString(plugin)}, {"options", std::move(options)}});
		const auto argv = ServeArgv(command);
		if (!server.service.StartStdioProcess(server.fields, {}, argv, nullptr, {{"OPENCODE_PASSWORD", password}, {"OPENCODE_CONFIG_CONTENT", config.dump()}})) throw std::runtime_error("The private OpenCode V2 service could not start.");
		std::array<char, 8192> bytes{}; std::string ready;
		const auto deadline = std::min(startup_deadline, Clock::now() + std::chrono::seconds(30));
		while (Clock::now() < deadline)
		{
			const auto count = server.service.ReadStdioProcessStdout(server.fields, bytes.data(), bytes.size());
			// Drain logs without publishing native output or credentials to Chat.
			std::array<char, 8192> discarded{}; server.service.ReadStdioProcessStderr(server.fields, discarded.data(), discarded.size());
			if (count > 0) ready.append(bytes.data(), static_cast<std::size_t>(count));
			if (ready.size() > 16384) break;
			const auto newline = ready.find('\n');
			if (newline != std::string::npos)
			{
				const auto observation = Json::parse(ready.substr(0, newline), nullptr, false);
				if (!observation.is_object()) break;
				url = observation.value("url", "");
				constexpr std::string_view prefix = "http://127.0.0.1:";
				if (!url.starts_with(prefix)) break;
				std::string port_text = url.substr(prefix.size()); if (port_text.ends_with('/')) port_text.pop_back();
				unsigned int port = 0; const auto parsed = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
				if (parsed.ec != std::errc{} || parsed.ptr != port_text.data() + port_text.size() || port == 0 || port > 65535) break;
				url = std::string(prefix) + std::to_string(port);
				// The running service, not the PATH check, is the protocol witness.
				Json info;
				try { info = Query("GET", "/api/info", nullptr); }
				catch (...)
				{
					if (expected_version.empty()) throw;
					throw std::runtime_error("OpenCode V2 did not answer the authenticated protocol check (/api/info). Input remains held.");
				}
				version = info.contains("version") && info.at("version").is_string() ? info.at("version").get<std::string>() : std::string{};
				if (!expected_version.empty() && version != expected_version)
					throw std::runtime_error("The running OpenCode V2 protocol version differs from the checked CLI version. Input remains held.");
				if (expected_version.empty() && !IsVerifiedV2(version)) throw std::runtime_error("The running OpenCode service is not a verified V2 protocol version. " + UnverifiedV2Message(version, "native API adapter"));
				std::string config = Config();
				if (!events.service.StartStdioProcessWithInput(events.fields, {}, {Curl(), "-q", "--silent", "--fail", "--no-buffer", "--noproxy", "*", "--proto", "=http", "--connect-timeout", "2", "--config", "-", url + "/api/event"}, config, nullptr))
				{
					if (!expected_version.empty()) throw std::runtime_error("OpenCode V2 event subscription could not start. Input remains held.");
					break;
				}
				std::fill(config.begin(), config.end(), '\0');
				return;
			}
			if (server.service.PollStdioProcessExited(server.fields)) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		throw std::runtime_error("OpenCode V2 did not publish a valid private loopback service lease.");
	}
};

int Bridge(const std::vector<std::string>& command, IPlatformProcessService& process_service, const std::optional<std::string>& worker_model = {}, const std::string& expected_version = {})
{
	std::string worker_prompt, worker_text;
	if (worker_model)
	{
		const auto deadline = Clock::now() + std::chrono::seconds(15);
		std::array<char, 8192> bytes{};
		while (true)
		{
			const auto read = process_service.ReadFiniteStandardInput(bytes.data(), bytes.size(), kMaxPacket);
			if (read.status == platform::StandardInputReadStatus::EndOfStream) break;
			if (read.status == platform::StandardInputReadStatus::Error || Clock::now() > deadline) throw std::runtime_error("OpenCode V2 text-worker input did not close.");
			if (read.status == platform::StandardInputReadStatus::Data) worker_prompt.append(bytes.data(), read.size);
			if (worker_prompt.size() > kMaxPacket) throw std::runtime_error("OpenCode V2 text-worker input exceeds its bounded size.");
			if (read.status != platform::StandardInputReadStatus::Data) std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		if (worker_prompt.empty()) throw std::runtime_error("OpenCode V2 text-worker input is empty.");
	}
	bool worker_setup = false, worker_done = false, worker_failed = false;
	std::vector<Json> worker_replies;
	std::string input;
	Json initialize_id;
	const bool runtime_verification = !expected_version.empty();
	if (runtime_verification) initialize_id = ReadInitialize(process_service, input);
	const auto report_initialize_failure = [&](std::string_view reason)
	{
		Output({{"jsonrpc", "2.0"}, {"id", initialize_id}, {"error", {{"code", kRuntimeVerificationError}, {"message", reason}}}});
	};
	Service service(process_service);
	if (runtime_verification)
	{
		service.startup_deadline = Clock::now() + std::chrono::seconds(90);
		auto probe = command;
		probe.push_back("--version");
		try
		{
			if (cli_versions::ExtractCliReportedVersion(Capture(probe, process_service, 10), true) != expected_version)
				throw std::runtime_error("Unexpected CLI version");
		}
		catch (...)
		{
			report_initialize_failure("OpenCode V2 could not confirm the checked release with an exact, unsuffixed CLI version. Input remains held.");
			return 1;
		}
	}
	try { service.Start(command, expected_version); }
	catch (const std::exception& error)
	{
		if (!runtime_verification) throw;
		report_initialize_failure(error.what());
		return 1;
	}
	VerifyPermission verify;
	if (runtime_verification) verify = [&](const auto& id, const auto& cwd)
	{
		service.Proof(cwd);
		service.VerifyDenied(id, cwd);
	};
	Session session([&](const auto& method, const auto& route, const auto& body)
	{
		if ((method == "POST" && (route == "/api/session" || route.ends_with("/prompt"))) || (method == "PATCH" && route.starts_with("/api/session/")))
		{
			if (!service.connected) throw std::runtime_error("OpenCode V2 event subscription is not ready.");
			if (service.captured_directory.empty()) throw std::runtime_error("OpenCode V2 has no captured workspace for its permission proof.");
			service.Proof(service.captured_directory);
		}
		try { return service.Query(method, route, body); }
		catch (...)
		{
			if (!runtime_verification || service.startup_deadline == Clock::time_point::max()) throw;
			if (route.starts_with("/api/location?")) throw std::runtime_error("OpenCode V2 workspace lookup failed. Input remains held.");
			if (route.starts_with("/api/plugin?")) throw std::runtime_error("OpenCode V2 permission plugin lookup failed. Input remains held.");
			if (method == "PATCH") throw std::runtime_error("OpenCode V2 permission rules could not be replaced. Input remains held.");
			if (route.starts_with("/api/session")) throw std::runtime_error("OpenCode V2 could not confirm the native session or read back its workspace and permission rules. Input remains held.");
			if (route.starts_with("/api/model")) throw std::runtime_error("OpenCode V2 could not read the startup model catalogue. Input remains held.");
			throw std::runtime_error("OpenCode V2 could not confirm the captured MCP configuration. Input remains held.");
		}
	}, [&](const auto& id, const auto& cwd, bool read_only)
	{
		// Text-only workers keep this policy inactive: every native permission
		// evaluates to deny, including pre-existing saved/project allowances.
		service.WritePolicy(worker_model ? std::string{} : id, cwd, read_only);
	}, [&](const Json& value)
	{
		if (!worker_model) { Output(value); return; }
		if (value.contains("error")) { worker_failed = true; return; }
		const auto method = value.value("method", "");
		if (method == "session/update")
		{
			const auto& update = value.at("params").at("update");
			if (update.value("sessionUpdate", "") == "agent_message_chunk") worker_text += update.at("content").at("text").get<std::string>();
			if (worker_text.size() > 4 * 1024 * 1024) throw std::runtime_error("OpenCode V2 text-worker output exceeds its bounded size.");
		}
		else if (method == "session/request_permission") worker_replies.push_back({{"id", value.at("id")}, {"result", {{"outcome", {{"outcome", "cancelled"}}}}}});
		else if (method == "uam/opencode/question") worker_replies.push_back({{"id", value.at("id")}, {"result", {{"answers", Json::object()}}}});
		else if (value.value("id", Json{}) == 4 && value.contains("result"))
		{
			worker_done = true;
			worker_failed = value.at("result").value("stopReason", "") != "end_turn";
		}
	}, service.version, std::move(verify));
	std::string sse, frame;
	std::array<char, 8192> bytes{};
	const auto initial_deadline = Clock::now() + std::chrono::seconds(15);
	try
	{
		while (true)
		{
			session.CheckDeadlines();
			if (session.Broken()) return 1;
			const auto count = service.events.service.ReadStdioProcessStdout(service.events.fields, bytes.data(), bytes.size());
			if (count == 0 || (count < 0 && count != -2))
			{
				if (!initialize_id.is_null()) throw std::runtime_error("OpenCode V2 event subscription closed before the protocol check completed. Input remains held.");
				session.Disconnected();
				return 1;
			}
			if (count > 0) sse.append(bytes.data(), static_cast<std::size_t>(count));
			if (sse.size() + frame.size() > kMaxPacket) throw std::runtime_error("OpenCode V2 event exceeds the bounded packet size.");
			while (sse.find('\n') != std::string::npos)
			{
				const auto end = sse.find('\n'); std::string line = sse.substr(0, end); sse.erase(0, end + 1);
				if (line.ends_with('\r')) line.pop_back();
				if (line.starts_with("data:")) { if (!frame.empty()) frame += '\n'; frame += line.substr(line.size() > 5 && line[5] == ' ' ? 6 : 5); }
				if (line.empty() && !frame.empty())
				{
					const auto event = Json::parse(frame, nullptr, false); frame.clear();
					if (!event.is_object()) throw std::runtime_error("Invalid OpenCode V2 event frame.");
					if (event.value("type", "") == "location.shutdown")
					{
						if (!initialize_id.is_null()) throw std::runtime_error("OpenCode V2 reloaded its native policy during startup.");
						session.Disconnected("OpenCode V2 reloaded its native policy. This process lease must be restarted explicitly."); return 1;
					}
					if (event.value("type", "") == "server.connected") service.connected = true;
					session.Event(event);
					if (session.Broken()) return 1;
				}
			}
			if (service.events.service.PollStdioProcessExited(service.events.fields) || service.server.service.PollStdioProcessExited(service.server.fields))
			{
				if (!initialize_id.is_null()) throw std::runtime_error("OpenCode V2 service or event subscription exited during the protocol check. Input remains held.");
				session.Disconnected();
				return 1;
			}
			// Native log drainage stays bounded and private.
			service.DrainNativeLogs();
			if (!service.connected && Clock::now() > std::min(initial_deadline, service.startup_deadline)) throw std::runtime_error("OpenCode V2 event subscription did not become ready.");
			if (worker_model)
			{
				if (worker_failed) return 1;
				if (worker_done) return std::fwrite(worker_text.data(), 1, worker_text.size(), stdout) == worker_text.size() && std::fflush(stdout) == 0 ? 0 : 1;
				if (service.connected && !worker_setup && count == -2 && sse.empty() && frame.empty())
				{
					worker_setup = true;
					const Json meta{{"uamPolicy", {{"readOnly", true}}}};
					session.Handle({{"id", 2}, {"method", "session/new"}, {"params", {{"cwd", paths::Utf8PathString(std::filesystem::current_path())}, {"_meta", meta}}}});
					if (!worker_failed && !worker_model->empty()) session.Handle({{"id", 3}, {"method", "session/set_model"}, {"params", {{"sessionId", session.NativeId()}, {"modelId", *worker_model}}}});
					if (!worker_failed) session.Handle({{"id", 4}, {"method", "session/prompt"}, {"params", {{"sessionId", session.NativeId()}, {"prompt", Json::array({{{"type", "text"}, {"text", worker_prompt}}})}, {"_meta", meta}}}});
				}
				for (const auto& reply : std::exchange(worker_replies, {})) session.Handle(reply);
				if (worker_failed || session.Broken()) return 1;
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
				continue;
			}
			// Drain startup/previous-turn event backlog before accepting another
			// command. During an active turn, cancellation must remain responsive.
			if (service.connected && (session.HasTurn() || (count == -2 && sse.empty() && frame.empty())))
			{
				if (initialize_id.is_null())
				{
					const auto read = service.server.service.ReadStandardInput(bytes.data(), bytes.size());
					if (read.status == platform::StandardInputReadStatus::EndOfStream) return 0;
					if (read.status == platform::StandardInputReadStatus::Error) return 1;
					if (read.status == platform::StandardInputReadStatus::Data) input.append(bytes.data(), read.size);
				}
				if (input.size() > kMaxPacket) throw std::runtime_error("UAM V2 input exceeds the bounded packet size.");
				while (input.find('\n') != std::string::npos)
				{
					const auto end = input.find('\n'); auto message = Json::parse(input.substr(0, end), nullptr, false); input.erase(0, end + 1);
					if (!message.is_object()) throw std::runtime_error("Invalid UAM V2 input frame.");
					session.Handle(message);
					initialize_id = nullptr;
					if (session.Broken()) return 1;
					if (session.Ready()) service.startup_deadline = Clock::time_point::max();
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}
	catch (const std::exception&)
	{
		if (!initialize_id.is_null())
		{
			report_initialize_failure("OpenCode V2 event subscription did not confirm the startup protocol. Input remains held.");
			return 1;
		}
		session.Disconnected("OpenCode V2 returned an unverified event or transport response. Input is held and is never replayed automatically.");
		return 1;
	}
}

int History(const std::string& operation, std::vector<std::string> command, const std::string& id, IPlatformProcessService& service)
{
	auto probe = command; probe.push_back("--version");
	const auto parsed_version = cli_versions::ExtractCliReportedVersion(Capture(probe, service, 10));
	if (!parsed_version) throw std::runtime_error("OpenCode history returned no usable CLI version.");
	const std::string version = *parsed_version;
	const bool v2 = IsCheckedHistoryV2(version);
	command = HistoryArgv(version, std::move(command), operation, id);
#if !defined(_WIN32)
	// 2.1.4 also exits before a large piped export drains (1.8 MiB arrived truncated).
	if (operation == "export") command = FileBackedStdoutArgv(std::move(command));
#endif
	if (!v2) return StreamV1(command, service);
	Json result = Json::parse(Capture(command, service), nullptr, false);
	if (result.is_discarded()) throw std::runtime_error("OpenCode history returned invalid JSON.");
	if (v2 && operation == "export") result = NormalizeExport(result);
	if (operation == "list")
	{
		if (!result.is_array() || result.size() >= 100001) throw std::runtime_error("OpenCode history list is incomplete or exceeds its bounded limit.");
		NormalizeHistoryList(result);
	}
	Output(result); return 0;
}
}

bool IsCommand(const std::vector<std::string>& arguments)
{
	return arguments.size() >= 2 && (arguments[1] == "--uam-opencode-v2-bridge" || arguments[1] == "--uam-opencode-history" || arguments[1] == "--uam-opencode-v2-text-worker");
}
std::optional<int> RunCommand(const std::vector<std::string>& arguments, IPlatformProcessService& service)
{
	if (!IsCommand(arguments)) return std::nullopt;
	try
	{
		const auto separator = std::find(arguments.begin() + 2, arguments.end(), "--");
		if (separator == arguments.end() || std::next(separator) == arguments.end()) throw std::runtime_error("Missing OpenCode executable.");
		std::vector<std::string> command(std::next(separator), arguments.end());
		if (arguments[1] == "--uam-opencode-v2-bridge")
		{
			if (separator == arguments.begin() + 2) return Bridge(command, service);
			if (separator == arguments.begin() + 3 && NeedsLocalChatRuntimeVerification(arguments[2]))
				return Bridge(command, service, {}, arguments[2]);
			throw std::runtime_error("Invalid OpenCode V2 bridge compatibility version.");
		}
		if (arguments[1] == "--uam-opencode-v2-text-worker")
		{
			if (separator != arguments.begin() + 3) throw std::runtime_error("Invalid OpenCode V2 text-worker model.");
			return Bridge(command, service, arguments.at(2));
		}
		return History(arguments.at(2), command, separator - arguments.begin() > 3 ? arguments[3] : "", service);
	}
	catch (const std::exception& error)
	{
		// Do not include raw native logs, inline config or authentication material.
		std::fprintf(stderr, "UAM OpenCode adapter: %s\n", error.what());
		return 1;
	}
}
}
