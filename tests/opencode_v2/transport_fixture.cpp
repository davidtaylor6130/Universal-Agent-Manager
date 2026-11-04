#include "common/provider/opencode_v2_session.h"
#include "common/platform/platform_services.h"
#include "common/platform/platform_state_fields.h"
#include "common/utils/base64.h"
#include "common/provider/provider_text_worker.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#if defined(_WIN32)
#include <io.h>
#define FIXTURE_DUP _dup
#define FIXTURE_DUP2 _dup2
#define FIXTURE_CLOSE _close
#define FIXTURE_FILENO _fileno
#else
#include <unistd.h>
#define FIXTURE_DUP dup
#define FIXTURE_DUP2 dup2
#define FIXTURE_CLOSE close
#define FIXTURE_FILENO fileno
#endif

namespace
{
using namespace uam::opencode_v2;
using Fields = uam::platform::StdioProcessPlatformFields;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

struct OutputCapture
{
	FILE* file = std::tmpfile();
	int original = -1;
	OutputCapture()
	{
		std::cout.flush(); std::fflush(stdout);
		Require(file != nullptr, "Could not allocate private fixture output");
		original = FIXTURE_DUP(FIXTURE_FILENO(stdout));
		Require(original >= 0 && FIXTURE_DUP2(FIXTURE_FILENO(file), FIXTURE_FILENO(stdout)) >= 0, "Could not capture fixture stdout");
	}
	~OutputCapture()
	{
		std::fflush(stdout);
		if (original >= 0) { FIXTURE_DUP2(original, FIXTURE_FILENO(stdout)); FIXTURE_CLOSE(original); }
		if (file) std::fclose(file);
	}
	std::string Read()
	{
		std::fflush(stdout); std::fseek(file, 0, SEEK_END);
		const auto size = std::ftell(file); Require(size >= 0 && size <= 4 * 1024 * 1024, "Unexpected fixture output size");
		std::string result(static_cast<std::size_t>(size), '\0'); std::rewind(file);
		Require(std::fread(result.data(), 1, result.size(), file) == result.size(), "Could not read fixture output");
		return result;
	}
};
struct InlineConfiguration
{
	std::optional<std::string> original;
	InlineConfiguration()
	{
		if (const char* value = std::getenv("OPENCODE_CONFIG_CONTENT")) original = value;
#if defined(_WIN32)
		_putenv_s("OPENCODE_CONFIG_CONTENT", "{}");
#else
		setenv("OPENCODE_CONFIG_CONTENT", "{}", 1);
#endif
	}
	~InlineConfiguration()
	{
#if defined(_WIN32)
		_putenv_s("OPENCODE_CONFIG_CONTENT", original ? original->c_str() : "");
#else
		if (original) setenv("OPENCODE_CONFIG_CONTENT", original->c_str(), 1); else unsetenv("OPENCODE_CONFIG_CONTENT");
#endif
	}
};

// Every process operation is simulated. No shell, CLI, socket, account, native
// profile, provider model or global installation can be reached by this driver.
class SimulatedProcesses final : public IPlatformProcessService
{
public:
	struct Process { std::string kind, output; std::size_t offset = 0; bool stopped = false; };
	mutable std::map<const Fields*, Process> processes;
	mutable const Fields* stream = nullptr;
	mutable const Fields* server = nullptr;
	std::string version = "2.1.2";
	bool publish_proof = true;
	bool text_worker = false;
	bool malformed_sse = false;
	bool early_cancel = false, failed_delete = false, lost_interrupt = false;
	std::string version_output;
	std::string cwd = std::filesystem::current_path().generic_string();
	mutable Json info, plugin_options;
	mutable std::string password, private_directory;
	mutable int server_starts = 0, server_stops = 0, stream_stops = 0, lease_closes = 0, creates = 0, prompts = 0, replies = 0, interrupts = 0;
	mutable std::uint64_t uuid = 0, event = 0;
	mutable std::vector<Json> requests;
	mutable std::vector<std::vector<std::string>> commands;
	mutable std::vector<std::string> input;
	mutable std::size_t input_index = 0;
	std::string history;

	SimulatedProcesses()
	{
		info = {{"id", "ses_transport"}, {"agent", "build"}, {"location", {{"directory", cwd}}}, {"model", {{"providerID", "mock"}, {"id", "demo"}}}};
		const auto send = [&](Json message) { input.push_back(message.dump() + '\n'); };
		send({{"id", 1}, {"method", "initialize"}});
		send({{"id", 2}, {"method", "session/new"}, {"params", {{"cwd", cwd}, {"_meta", {{"uamPolicy", {{"readOnly", false}}}}}}}});
		send({{"id", 3}, {"method", "session/set_model"}, {"params", {{"sessionId", "ses_transport"}, {"modelId", "mock/demo/high"}}}});
		send({{"id", 4}, {"method", "session/prompt"}, {"params", {{"sessionId", "ses_transport"}, {"prompt", Json::array({{{"type", "text"}, {"text", "first fixture input"}}})}, {"_meta", {{"uamPolicy", {{"readOnly", false}}}}}}}});
		send({{"id", "v2:per_transport"}, {"result", {{"outcome", {{"outcome", "selected"}, {"optionId", "once"}}}}}});
		send({{"id", 5}, {"method", "session/prompt"}, {"params", {{"sessionId", "ses_transport"}, {"prompt", Json::array({{{"type", "text"}, {"text", "cancelled fixture input"}}})}, {"_meta", {{"uamPolicy", {{"readOnly", false}}}}}}}});
		send({{"method", "session/cancel"}, {"params", {{"sessionId", "ses_transport"}}}});
	}
	void Event(const std::string& type, Json data) const
	{
		Require(stream != nullptr, "Native event stream was not allocated");
		if (type != "server.connected") data["sessionID"] = "ses_transport";
		processes.at(stream).output += "data: " + Json{{"id", "evt_" + std::to_string(++event)}, {"type", type}, {"data", data}}.dump() + "\n\n";
	}
	Json Native(const std::string& method, const std::string& route, const Json& body) const
	{
		requests.push_back({{"method", method}, {"route", route}, {"body", body}});
		if (route == "/api/info") return {{"version", version}};
		if (route.starts_with("/api/location?")) return {{"directory", cwd}};
		if (route.starts_with("/api/plugin?"))
		{
			if (publish_proof)
			{
				std::ofstream proof(plugin_options.at("proofPath").get<std::string>());
				proof << Json{{"nonce", plugin_options.at("nonce")}, {"directory", cwd}}.dump();
			}
			return {{"data", Json::array()}};
		}
		const Json model{{"providerID", "mock"}, {"id", "demo"}, {"name", "Mock"}, {"enabled", true}, {"limit", {{"context", 200000}, {"output", 32000}}}, {"variants", Json::array({{{"id", "high"}}})}};
		if (route.starts_with("/api/model/default?")) return {{"data", model}};
		if (route.starts_with("/api/model?")) return {{"data", Json::array({model})}};
		if (method == "POST" && route == "/api/session")
		{
			++creates; info["agent"] = body.at("agent"); info["permissions"] = body.at("permissions"); return {{"data", info}};
		}
		if (route == "/api/session/ses_transport")
		{
			if (method == "PATCH") { info["permissions"] = body.at("permissions"); return nullptr; }
			return {{"data", info}};
		}
		if (route.ends_with("/inbox")) return {{"data", Json::array()}};
		if (route.ends_with("/model")) { info["model"] = body.at("model"); return nullptr; }
		if (route.ends_with("/prompt"))
		{
			if (text_worker)
			{
				std::ifstream policy(plugin_options.at("policyPath").get<std::string>());
				Json captured; policy >> captured;
				Require(captured.at("active") == false, "Text worker acquired tool permission authority");
				Require(body.at("text") == "Text-only reviewer fixture", "Text-worker stdin changed");
			}
			++prompts;
			if (malformed_sse)
			{
				processes.at(stream).output += "data: {malformed\n\n";
				return {{"data", {{"id", "msg_input_" + std::to_string(prompts)}, {"sessionID", "ses_transport"}}}};
			}
			// Delivery happened natively, but its SSE fact is still in flight
			// when UAM sends cancel. DELETE cannot establish that fact.
			if (prompts == 2 && early_cancel) return {{"data", {{"id", "msg_input_2"}, {"sessionID", "ses_transport"}}}};
			Event("session.inbox.delivered", {{"inboxID", "msg_input_" + std::to_string(prompts)}});
			Event("session.text.delta", {{"delta", prompts == 1 ? "hello" : "partial"}});
			if (prompts == 1)
			{
				Event("session.tool.input.started", {{"assistantMessageID", "msg_assistant"}, {"id", "read"}, {"name", "shell"}});
				Event("session.tool.called", {{"assistantMessageID", "msg_assistant"}, {"id", "read"}, {"input", {{"command", "git status"}}}});
				Event("permission.asked", {{"id", "per_transport"}, {"action", "shell"}, {"resources", Json::array({"git status"})}, {"source", {{"type", "tool"}, {"messageID", "msg_assistant"}, {"id", "read"}}}});
			}
			return {{"data", {{"id", "msg_input_" + std::to_string(prompts)}, {"sessionID", "ses_transport"}}}};
		}
		if (route.ends_with("/permission/per_transport/reply"))
		{
			Require(body.at("decision") == (text_worker ? "reject" : "once"), "Transport wrote an unadmitted native grant"); ++replies;
			Event("session.tool.success", {{"assistantMessageID", "msg_assistant"}, {"id", "read"}, {"content", Json::array({{{"type", "text"}, {"text", "clean"}}})}});
			Event("session.step.ended", {{"assistantMessageID", "msg_assistant"}, {"tokens", {{"input", 10}, {"output", 2}, {"reasoning", 1}, {"cache", {{"read", 3}, {"write", 0}}}}}});
			Event("session.execution.succeeded", Json::object()); return nullptr;
		}
		if (route.ends_with("/interrupt")) { ++interrupts; Event("session.execution.interrupted", Json::object()); return {{"interrupted", true}}; }
		if (method == "DELETE" && route.ends_with("/inbox/msg_input_2") && early_cancel)
		{
			Event("session.inbox.delivered", {{"inboxID", "msg_input_2"}});
			return nullptr;
		}
		throw std::runtime_error("Unexpected simulated native route");
	}
	bool StartStdioProcess(Fields& fields, const std::filesystem::path&, const std::vector<std::string>& argv, std::string*, const std::vector<std::pair<std::string, std::string>>& env) const override
	{
		commands.push_back(argv);
		if (argv.back() == "--version") { processes[&fields] = {"capture", version_output.empty() ? version + '\n' : version_output}; return true; }
		if (std::find(argv.begin(), argv.end(), "serve") == argv.end()) { processes[&fields] = {"history", history}; return true; }
		Require(argv == ServeArgv({"opencode"}), "Native serve argv changed");
		server = &fields; ++server_starts;
		Json config;
		for (const auto& [name, value] : env)
			if (name == "OPENCODE_PASSWORD") password = value;
			else if (name == "OPENCODE_CONFIG_CONTENT") config = Json::parse(value);
		Require(!password.empty(), "Fresh native credential missing");
		plugin_options = config.at("plugins").back().at("options");
		const auto plugin = config.at("plugins").back().at("package").get<std::string>();
		private_directory = std::filesystem::path(plugin).parent_path().string();
		std::ifstream source(std::filesystem::path(plugin) / "server.js");
		const std::string actual((std::istreambuf_iterator<char>(source)), {});
		Require(actual == PermissionPluginSource(), "Executable permission guard changed in transport");
		processes[&fields] = {"server", "{\"url\":\"http://127.0.0.1:49152\"}\n"}; return true;
	}
	bool StartStdioProcessWithInput(Fields& fields, const std::filesystem::path&, const std::vector<std::string>& argv, std::string_view input_bytes, std::string*, const std::vector<std::pair<std::string, std::string>>&) const override
	{
		commands.push_back(argv);
		if (argv.front() == "opencode")
		{
			processes[&fields] = {"capture", argv.back() == "--version" ? (version_output.empty() ? version + '\n' : version_output) : history}; return true;
		}
		Require(argv.back().starts_with("http://127.0.0.1:49152/api/"), "HTTP escaped its private loopback endpoint");
		for (const auto& arg : argv) Require(arg.find(password) == std::string::npos, "Credential leaked into argv");
		Require(input_bytes.find(uam::base64::Encode("opencode:" + password)) != std::string::npos, "Authentication was not piped privately");
		if (argv.back().ends_with("/api/event")) { stream = &fields; processes[&fields] = {"stream", {}}; Event("server.connected", Json::object()); return true; }
		const auto flag = std::find(argv.begin(), argv.end(), "--request"); Require(flag != argv.end() && std::next(flag) != argv.end(), "HTTP method missing");
		Json body;
		std::istringstream lines{std::string(input_bytes)}; std::string line;
		while (std::getline(lines, line)) if (line.starts_with("data = ")) body = Json::parse(Json::parse(line.substr(7)).get<std::string>());
		const auto route = argv.back().substr(std::string("http://127.0.0.1:49152").size());
		const auto result = Native(*std::next(flag), route, body);
		if (route.find("/inbox/") != std::string::npos || route.ends_with("/interrupt"))
		{
			const auto maximum = std::find(argv.begin(), argv.end(), "--max-time");
			Require(maximum != argv.end() && std::next(maximum) != argv.end() && *std::next(maximum) == "1", "Cancel HTTP call could consume the whole shared cancel budget");
		}
		const bool failed = (failed_delete && route.find("/inbox/") != std::string::npos) || (lost_interrupt && route.ends_with("/interrupt"));
		processes[&fields] = {"http", failed ? "{}\n500" : result.is_null() ? "\n204" : result.dump() + "\n200"}; return true;
	}
	mutable int finite_input_reads = 0;
	uam::platform::StandardInputReadResult ReadFiniteStandardInput(char* bytes, std::size_t size,
	    std::size_t maximum_input_bytes) const override
	{
		Require(text_worker && maximum_input_bytes == 16 * 1024 * 1024, "Finite input used outside bounded worker contract");
		++finite_input_reads;
		return ReadFixtureInput(bytes, size);
	}
	uam::platform::StandardInputReadResult ReadStandardInput(char* bytes, std::size_t size) const override
	{
		Require(!text_worker, "Worker used pipe-only stdin instead of its finite preloaded input contract");
		return ReadFixtureInput(bytes, size);
	}
	uam::platform::StandardInputReadResult ReadFixtureInput(char* bytes, std::size_t size) const
	{
		using Status = uam::platform::StandardInputReadStatus;
		if (!publish_proof && input_index >= 2) return {Status::EndOfStream, 0};
		if (input_index == input.size())
		{
			if (early_cancel && stream && processes.at(stream).offset < processes.at(stream).output.size()) return {Status::Pending, 0};
			return {Status::EndOfStream, 0};
		}
		const auto& next = input.at(input_index++); Require(next.size() <= size, "Fixture input exceeds capacity");
		std::memcpy(bytes, next.data(), next.size()); return {Status::Data, next.size()};
	}
	std::ptrdiff_t ReadStdioProcessStdout(Fields& fields, char* bytes, std::size_t size, std::string*) const override
	{
		auto& state = processes.at(&fields); const auto count = std::min(size, state.output.size() - state.offset);
		if (count) { std::memcpy(bytes, state.output.data() + state.offset, count); state.offset += count; return static_cast<std::ptrdiff_t>(count); }
		return state.kind == "stream" || state.kind == "server" ? -2 : 0;
	}
	std::ptrdiff_t ReadStdioProcessStderr(Fields&, char*, std::size_t, std::string*) const override { return -2; }
	bool PollStdioProcessExited(Fields& fields, int* exit) const override
	{
		const auto& state = processes.at(&fields); if (exit) *exit = 0;
		return state.stopped || ((state.kind == "http" || state.kind == "capture" || state.kind == "history") && state.offset == state.output.size());
	}
	void StopStdioProcess(Fields& fields, bool) const override
	{
		const auto found = processes.find(&fields); if (found == processes.end() || found->second.stopped) return;
		found->second.stopped = true;
		if (found->second.kind == "server") ++server_stops;
		if (found->second.kind == "stream") ++stream_stops;
	}
	void CloseStdioProcessHandles(Fields& fields) const override { processes.erase(&fields); }
	void CloseStdioProcessInput(Fields& fields) const override { if (&fields == server) ++lease_closes; }
	void TerminateStdioProcess(Fields& fields, bool fast) const override { StopStdioProcess(fields, fast); }
	bool WriteToStdioProcess(Fields&, const char*, std::size_t, std::string*) const override { throw std::runtime_error("Unexpected simulated pipe write"); }
	std::string GenerateUuid() const override { return "fixture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++uuid); }
	bool SupportsDetachedProcesses() const override { return false; }
	bool PopulateLocalTime(std::time_t, std::tm*) const override { return false; }
	std::string BuildShellCommandWithWorkingDirectory(const std::filesystem::path&, const std::string&) const override { throw std::runtime_error("Shell invocation forbidden in fixture"); }
	bool CaptureCommandOutput(const std::string&, std::string*, int*, std::string*) const override { throw std::runtime_error("Shell capture forbidden in fixture"); }
	int NormalizeCapturedCommandExitCode(int status) const override { return status; }
	ProcessExecutionResult ExecuteCommand(const std::string&, int, std::stop_token) const override { throw std::runtime_error("Command execution forbidden in fixture"); }
	std::filesystem::path ResolveCurrentExecutablePath() const override { return {}; }
	std::unique_ptr<uam::platform::DataRootLock> TryAcquireDataRootLock(const std::filesystem::path&, std::string*) const override { return {}; }
	uintmax_t NativeGeminiSessionMaxFileBytes() const override { return 0; }
	std::size_t NativeGeminiSessionMaxMessages() const override { return 0; }
	bool LaunchShellAt(const std::filesystem::path&, std::string*) const override { throw std::runtime_error("Terminal launch forbidden in fixture"); }
	void SetKeepSystemAwake(bool) const override {}
	bool EmbeddedBrowserUsesMockKeychain() const override { return true; }
};
}

void RunOpenCodeV2TransportFixtures()
{
	InlineConfiguration configuration;
	{
		SimulatedProcesses native; std::string output;
		{
			OutputCapture capture;
			Require(RunCommand({"uam-fixture", "--uam-opencode-v2-bridge", "--", "opencode"}, native) == std::optional<int>{0}, "Simulated bridge failed");
			output = capture.Read();
		}
		Require(native.creates == 1 && native.prompts == 2 && native.replies == 1 && native.interrupts == 1, "Native lifecycle operations differ");
		Require(native.server_starts == 1 && native.server_stops == 1 && native.stream_stops == 1 && native.lease_closes == 1, "Owned process/stream lease was not retired");
		Require(!std::filesystem::exists(native.private_directory), "Private executable policy was not retired");
		std::istringstream lines(output); std::string line; bool completed = false, cancelled = false, approval = false;
		while (std::getline(lines, line))
		{
			const auto value = Json::parse(line);
			Require(!value.contains("error"), "Unexpected bridge protocol error");
			if (value.value("method", "") == "session/request_permission") approval = value.at("params").at("toolCall").at("toolCallId") == ToolKey("msg_assistant", "read");
			if (value.value("id", Json{}) == 4) completed = value.at("result").at("stopReason") == "end_turn" && value.at("result").at("usage").at("totalTokens") == 16;
			if (value.value("id", Json{}) == 5) cancelled = value.at("result").at("stopReason") == "cancelled";
		}
		Require(completed && cancelled && approval, "Bridge did not preserve streamed turn/approval/cancel identity");
	}
	{
		SimulatedProcesses native; native.version = "2.2.0";
		OutputCapture capture;
		Require(RunCommand({"uam-fixture", "--uam-opencode-v2-bridge", "--", "opencode"}, native) == std::optional<int>{1}, "Unverified running service was accepted");
		Require(native.creates == 0 && native.prompts == 0 && native.server_stops == 1, "Version refusal leaked a session/process");
	}
	{
		SimulatedProcesses native; native.publish_proof = false;
		OutputCapture capture;
		Require(RunCommand({"uam-fixture", "--uam-opencode-v2-bridge", "--", "opencode"}, native) == std::optional<int>{0}, "Missing proof did not return a structured setup failure");
		Require(native.creates == 0 && native.prompts == 0 && capture.Read().find("permission guard") != std::string::npos, "Missing hook proof admitted input");
	}
	{
		SimulatedProcesses native; native.version = "1.19.4"; native.history = "{\"info\":{\"id\":\"ses_v1\"},\"messages\":[]}\n";
		native.version_output = "opencode v1.19.4 (Homebrew)\n";
		OutputCapture capture;
		Require(RunCommand({"uam-fixture", "--uam-opencode-history", "export", "ses_v1", "--", "opencode"}, native) == std::optional<int>{0}, "V1 history dispatcher failed");
		Require(capture.Read() == native.history, "V1 history bytes were changed");
#if defined(_WIN32)
		Require(native.commands.back() == std::vector<std::string>({"opencode", "export", "ses_v1", "--pure"}), "V1 export argv changed");
#else
		// POSIX V1 export writes to a private file first: OpenCode 1.19.4 can exit before a piped stdout drains.
		Require(native.commands.back() == FileBackedStdoutArgv({"opencode", "export", "ses_v1", "--pure"}), "V1 export bypassed file-backed stdout");
#endif
	}
	{
		SimulatedProcesses native;
		native.history = Json::array({{{"id", "ses_v2"}, {"directory", native.cwd}, {"title", "Fixture"}}}).dump();
		OutputCapture capture;
		Require(RunCommand({"uam-fixture", "--uam-opencode-history", "list", "--", "opencode"}, native) == std::optional<int>{0}, "V2 history list failed");
		const auto listed = Json::parse(capture.Read());
		Require(listed.at(0).at("opencode_v2_session_id") == "ses_v2", "V2 imported session lost its exact-ID downgrade refusal hint");
		Require(native.commands.back() == HistoryArgv("2.1.2", {"opencode"}, "list", {}), "V2 list used legacy argv");
	}
	for (const std::string version : {"2.1.2", "2.1.4"})
	{
		SimulatedProcesses native; native.version = version;
		native.history = Json{{"info", {{"id", "ses_v2"}, {"location", {{"directory", native.cwd}}}}}, {"messages", Json::array({{{"id", "msg_user"}, {"type", "user"}, {"text", "Retained input"}}})}}.dump();
		OutputCapture capture;
		Require(RunCommand({"uam-fixture", "--uam-opencode-history", "export", "ses_v2", "--", "opencode"}, native) == std::optional<int>{0}, "V2 history export failed");
		const auto exported = Json::parse(capture.Read());
		Require(exported.at("info").at("directory") == native.cwd && exported.at("messages").at(0).at("parts").at(0).at("text") == "Retained input", "V2 export failed the internal history contract");
#if defined(_WIN32)
		Require(native.commands.back() == HistoryArgv(version, {"opencode"}, "export", "ses_v2"), "V2 export used legacy argv");
#else
		Require(native.commands.back() == FileBackedStdoutArgv(HistoryArgv(version, {"opencode"}, "export", "ses_v2")), "V2 export bypassed file-backed stdout");
#endif
	}
	{
		SimulatedProcesses native; native.text_worker = true; native.input = {"Text-only reviewer fixture"};
		OutputCapture capture;
		Require(RunCommand({"uam-fixture", "--uam-opencode-v2-text-worker", "mock/demo/high", "--", "opencode"}, native) == std::optional<int>{0}, "V2 text worker failed");
		Require(capture.Read() == "hello", "Text worker leaked protocol, tool, reasoning or usage output");
		Require(native.finite_input_reads == 2, "Worker did not read finite data followed by EOF before native startup");
		Require(native.creates == 1 && native.prompts == 1 && native.replies == 1 && native.interrupts == 0, "Text worker reused a native session or admitted extra input");
		Require(native.info.at("model").at("variant") == "high", "Text worker lost selected model/effort");
		Require(native.server_stops == 1 && native.stream_stops == 1 && !std::filesystem::exists(native.private_directory), "Text-worker lease/policy survived completion");
	}
	{
		SimulatedProcesses native;
		native.cwd += "/synthetic-alias"; native.info["location"]["directory"] = native.cwd;
		auto setup = Json::parse(native.input.at(1)); setup["params"]["cwd"] = native.cwd; native.input[1] = setup.dump() + '\n';
		OutputCapture capture;
		Require(RunCommand({"uam-fixture", "--uam-opencode-v2-bridge", "--", "opencode"}, native) == std::optional<int>{0}, "Captured directory differing from process cwd failed proof");
		Require(native.prompts == 2 && capture.Read().find("permission guard") == std::string::npos, "Prompt/PATCH proof borrowed process current_path");
	}
	{
		SimulatedProcesses native; native.malformed_sse = true; native.input.resize(4);
		OutputCapture capture;
		Require(RunCommand({"uam-fixture", "--uam-opencode-v2-bridge", "--", "opencode"}, native) == std::optional<int>{1}, "Malformed SSE retained a writable bridge");
		const auto output = capture.Read();
		Require(output.find("never replayed") != std::string::npos && output.find("\"id\":4") != std::string::npos && native.prompts == 1, "Malformed SSE exited without the owned held/no-replay error");
		Require(native.server_stops == 1 && native.stream_stops == 1, "Malformed-event bridge leaked an owned process");
	}
	for (const bool failed_delete : {false, true})
	{
		SimulatedProcesses native; native.early_cancel = true; native.failed_delete = failed_delete; native.lost_interrupt = failed_delete;
		OutputCapture capture;
		Require(RunCommand({"uam-fixture", "--uam-opencode-v2-bridge", "--", "opencode"}, native) == std::optional<int>{0}, "Delivered/delete race lost the bridge process");
		const auto output = capture.Read();
		Require(native.prompts == 2 && native.interrupts == 1 && output.find("\"stopReason\":\"cancelled\"") != std::string::npos && output.find("\"error\"") == std::string::npos, "HTTP cancel outcomes replaced the exact owned terminal event");
	}
	{
		SimulatedProcesses ids;
		const auto root = std::filesystem::temp_directory_path() / ids.GenerateUuid();
		std::filesystem::create_directory(root);
		struct Cleanup { std::filesystem::path root; ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(root, ignored); } } cleanup{root};
		ProviderProfile profile; profile.id = "opencode-cli";
		std::vector<std::string> native{"uam-fixture", "--uam-opencode-v2-text-worker", "mock/demo", "--", "opencode"};
		Require(uam::provider_workers::ApplyWorkerIsolationPolicy(profile, root, native, true), "V2 native isolation was rejected");
		Require(!std::filesystem::exists(root / "opencode.json") && std::find(native.begin(), native.end(), "--pure") == native.end(), "V2 worker retained incompatible V1 isolation");
		std::vector<std::string> legacy{"opencode", "run", "--", "text"};
		Require(uam::provider_workers::ApplyWorkerIsolationPolicy(profile, root, legacy), "V1 worker isolation changed");
		Require(std::filesystem::exists(root / "opencode.json") && legacy.at(2) == "--pure", "V1 deny configuration was removed");
	}
}
