// Focused tests for the opt-in Custom ACP agent (#523): its Settings configuration, launch checks and a real
// stdio session with the scripted ACP agent in tests/fixtures/custom_acp_agent.py. Built without CEF or any
// provider, so it also runs where the desktop app cannot be built.
//
// Usage: uam_custom_acp_agent_tests <python3> <tests/fixtures/custom_acp_agent.py>
#include "common/paths/path_utils.h"
#include "common/provider/custom_acp_config.h"
#include "common/runtime/acp/acp_json_rpc.h"
#include "common/runtime/acp/acp_permissions.h"
#include "common/runtime/acp/acp_protocol_methods.h"
#include "common/runtime/acp/acp_request_defaults.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace
{
	using uam::custom_acp::AgentConfig;

	void Check(bool condition, const std::string& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	void CheckOneLine(const std::string& message, const std::string& context)
	{
		Check(!message.empty(), context + ": expected an error message");
		Check(message.find('\n') == std::string::npos && message.find('\r') == std::string::npos, context + ": error must be one line: " + message);
	}

	struct TempDirectory
	{
		std::filesystem::path path;
		TempDirectory()
		{
			const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			path = std::filesystem::temp_directory_path() / ("uam-custom-acp-" + std::to_string(stamp));
			std::filesystem::create_directories(path);
		}
		~TempDirectory()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path, ignored);
		}
	};

	void WriteFile(const std::filesystem::path& path, const std::string& text, bool executable)
	{
		std::ofstream(path, std::ios::binary) << text;
#if !defined(_WIN32)
		::chmod(path.c_str(), executable ? 0755 : 0644);
#else
		(void)executable;
#endif
	}

	AgentConfig SampleConfig()
	{
		AgentConfig config;
		config.display_name = "  My agent  ";
		config.command = "  my-agent  ";
		config.arguments = {"--acp", "two words", "semi;colon $HOME `id`"};
		config.environment = {{"AGENT_TOKEN", "secret value"}, {"_DEBUG", "1"}};
		return config;
	}

	void ValidationCases()
	{
		AgentConfig config = uam::custom_acp::Normalized(SampleConfig());
		Check(config.display_name == "My agent" && config.command == "my-agent", "name and command are trimmed");
		Check(config.arguments[1] == "two words", "arguments are kept exactly as typed");
		Check(uam::custom_acp::ValidationError(config).empty(), "a normal configuration is valid");
		Check(uam::custom_acp::DisplayName(AgentConfig{}) == "Custom ACP agent", "an unnamed agent has the default name");
		Check(!uam::custom_acp::IsConfigured(AgentConfig{}), "an empty command is not configured");

		const auto invalid = [&](const std::function<void(AgentConfig&)>& change, const std::string& context)
		{
			AgentConfig changed = config;
			change(changed);
			CheckOneLine(uam::custom_acp::ValidationError(changed), context);
		};
		invalid([](AgentConfig& c) { c.environment.push_back({"1BAD", "x"}); }, "environment names start with a letter or underscore");
		invalid([](AgentConfig& c) { c.environment.push_back({"agent_token", "x"}); }, "environment names that differ only in case are duplicates");
		invalid([](AgentConfig& c) { c.environment[0].value = "a\nb"; }, "environment values are one line");
		invalid([](AgentConfig& c) { c.arguments.push_back("a\nb"); }, "each argument is one line");
		invalid([](AgentConfig& c) { c.arguments.push_back(std::string("a\0b", 3)); }, "arguments cannot hold NUL");
		invalid([](AgentConfig& c) { c.display_name = std::string(321, 'n'); }, "names are short");
		invalid([](AgentConfig& c) { c.arguments.push_back("a\tb"); }, "arguments cannot hold tabs or control characters");
		invalid([](AgentConfig& c) { c.arguments.push_back("safe\xE2\x80\xAEtxt.exe"); }, "arguments cannot hold direction-changing characters");
		invalid([](AgentConfig& c) { c.arguments.push_back("two\xC2\xA0words"); }, "arguments cannot hold a no-break space that looks like a separator");
		invalid([](AgentConfig& c) { c.command = "agent\xE2\x80\x8B"; }, "commands cannot hold zero-width characters");
		invalid([](AgentConfig& c) { c.arguments.push_back("a\xE2\x80\xA8" "b"); }, "arguments cannot hold a line separator");
		invalid([](AgentConfig& c) { c.arguments.push_back("a\xC2\x85" "b"); }, "arguments cannot hold C1 controls");
		invalid([](AgentConfig& c) { c.approved_fingerprint = "0123456789abcdef"; }, "an approval names the program it allowed");
		invalid([](AgentConfig& c) { c.command = "agent\n--flag"; }, "commands are one line");
		invalid([](AgentConfig& c) { c.arguments.assign(65, "x"); }, "argument count is bounded");

		AgentConfig parsed;
		std::string error;
		Check(uam::custom_acp::FromJson(uam::custom_acp::ToJson(config), parsed, &error) && parsed == config, "the saved form round-trips");
		nlohmann::json wrong_version = uam::custom_acp::ToJson(config);
		wrong_version["version"] = 2;
		Check(!uam::custom_acp::FromJson(wrong_version, parsed, &error), "an unknown saved version is refused");
		CheckOneLine(error, "unknown version");
		nlohmann::json wrong_type = uam::custom_acp::ToJson(config);
		wrong_type["arguments"] = "--acp";
		Check(!uam::custom_acp::FromJson(wrong_type, parsed, &error), "arguments must be a list");
		CheckOneLine(error, "argument list type");
		nlohmann::json half_approval = uam::custom_acp::ToJson(config);
		half_approval["approvedFingerprint"] = uam::custom_acp::LaunchFingerprint(config);
		half_approval.erase("approvedExecutable");
		Check(uam::custom_acp::FromJson(half_approval, parsed, &error) && parsed.command == config.command && parsed.approved_fingerprint.empty(),
		    "a half-saved approval is dropped without losing the agent");
	}

	void ApprovalCases()
	{
		const AgentConfig config = uam::custom_acp::Normalized(SampleConfig());
		const std::string fingerprint = uam::custom_acp::LaunchFingerprint(config);
		Check(fingerprint.size() == 16, "fingerprints are 16 hex digits");
		const auto differs = [&](const std::function<void(AgentConfig&)>& change, const std::string& context)
		{
			AgentConfig changed = config;
			change(changed);
			Check(uam::custom_acp::LaunchFingerprint(changed) != fingerprint, context);
		};
		differs([](AgentConfig& c) { c.command = "other-agent"; }, "a new command needs a new approval");
		differs([](AgentConfig& c) { c.arguments.push_back("--yolo"); }, "a new argument needs a new approval");
		differs([](AgentConfig& c) { c.arguments = {"--acp two words", "semi;colon $HOME `id`"}; }, "argument boundaries are part of the approval");
		differs([](AgentConfig& c) { c.environment[0].value = "other"; }, "a new environment value needs a new approval");
		differs([](AgentConfig& c) { c.environment[1].name = "_TRACE"; }, "a new environment name needs a new approval");
		AgentConfig renamed = config;
		renamed.display_name = "Renamed";
		Check(uam::custom_acp::LaunchFingerprint(renamed) == fingerprint, "renaming does not change what launches");

		AgentConfig approved = config;
		Check(!uam::custom_acp::IsApproved(approved), "a new configuration is not allowed yet");
		approved.approved_fingerprint = fingerprint;
		approved.approved_executable = "/usr/bin/my-agent";
		Check(uam::custom_acp::IsApproved(approved), "the shown launch is allowed");
		Check(uam::custom_acp::IsApproved(uam::custom_acp::WithApprovalCarriedOver(renamed, approved)), "renaming keeps the approval");
		AgentConfig changed = config;
		changed.arguments.push_back("--extra");
		Check(!uam::custom_acp::IsApproved(uam::custom_acp::WithApprovalCarriedOver(changed, approved)), "changing the launch drops the approval");
		Check(uam::custom_acp::WithApprovalCarriedOver(changed, approved).approved_fingerprint.empty() &&
		    uam::custom_acp::WithApprovalCarriedOver(changed, approved).approved_executable.empty(), "a dropped approval is cleared");
		Check(uam::custom_acp::WithApprovalCarriedOver(renamed, approved).approved_executable == "/usr/bin/my-agent", "a kept approval keeps its program");

		Check(uam::custom_acp::DisplayCommand(config) == "my-agent --acp \"two words\" \"semi;colon \\$HOME \\`id\\`\"",
		    "the displayed command shows each argument's boundaries: " + uam::custom_acp::DisplayCommand(config));
	}

	void StorageCases()
	{
		TempDirectory root;
		AgentConfig loaded = SampleConfig();
		std::string error;
		Check(uam::custom_acp::Load(root.path, loaded, &error) && loaded == AgentConfig{}, "a missing file is an unconfigured agent");
		AgentConfig config = uam::custom_acp::Normalized(SampleConfig());
		config.approved_fingerprint = uam::custom_acp::LaunchFingerprint(config);
		config.approved_executable = "/usr/local/bin/my-agent";
		Check(uam::custom_acp::Save(root.path, config, &error), "the configuration saves: " + error);
		Check(uam::custom_acp::Load(root.path, loaded, &error) && loaded == config && uam::custom_acp::IsApproved(loaded), "the saved configuration and approval load back");
		WriteFile(uam::custom_acp::ConfigPath(root.path), "{not json", false);
		Check(!uam::custom_acp::Load(root.path, loaded, &error), "a damaged file is reported");
		CheckOneLine(error, "damaged file");
		AgentConfig invalid = config;
		invalid.environment.push_back({"BAD NAME", "x"});
		Check(!uam::custom_acp::Save(root.path, invalid, &error), "an invalid configuration is not saved");
		CheckOneLine(error, "invalid save");
	}

	void ResolutionCases()
	{
		TempDirectory root;
		const std::filesystem::path bin = root.path / "bin";
		std::filesystem::create_directories(bin);
		WriteFile(bin / "agent", "#!/bin/sh\nexit 0\n", true);
		WriteFile(bin / "plain", "not a program\n", false);
		uam::custom_acp::ExecutableSearch search{{std::filesystem::path("relative/bin"), bin}, false};
		std::string error;
#if !defined(_WIN32)
		Check(uam::custom_acp::ResolveExecutable("agent", search, error) == bin / "agent", "a program name is found on the search path: " + error);
		Check(uam::custom_acp::ResolveExecutable(uam::paths::Utf8PathString(bin / "agent"), search, error) == bin / "agent", "a full path is used as given");
		Check(uam::custom_acp::ResolveExecutable("plain", search, error).empty(), "a file that is not executable is refused");
		CheckOneLine(error, "not executable");
		Check(error.find("not executable") != std::string::npos, "the error says the file is not executable: " + error);
#endif
		Check(uam::custom_acp::ResolveExecutable("missing-agent", search, error).empty(), "an unknown program is refused");
		CheckOneLine(error, "not found");
		Check(error.rfind("Custom ACP agent command not found: missing-agent.", 0) == 0, "the not-found error names the command: " + error);
		Check(uam::custom_acp::ResolveExecutable("npx some-agent --acp", search, error).empty(), "a command line typed into the command field is refused");
		Check(error.find("Arguments") != std::string::npos, "the error points at the Arguments list: " + error);
		Check(uam::custom_acp::ResolveExecutable("bin/agent", search, error).empty(), "a relative path is refused, since it would depend on the chat folder");
		CheckOneLine(error, "relative path");
		Check(uam::custom_acp::ResolveExecutable("", search, error).empty(), "an empty command is refused");
		CheckOneLine(error, "empty command");

		// Windows rules: batch scripts would run through cmd.exe, so they are refused rather than wrapped.
		const std::filesystem::path win = root.path / "win";
		std::filesystem::create_directories(win);
		WriteFile(win / "shim.cmd", "@echo off\n", false);
		WriteFile(win / "tool.exe", "MZ", false);
		uam::custom_acp::ExecutableSearch windows{{win}, true};
		Check(uam::custom_acp::ResolveExecutable("shim", windows, error).empty(), "a name that only resolves to a batch script is refused");
		Check(error.find("batch script") != std::string::npos, "the error explains batch scripts: " + error);
		Check(uam::custom_acp::ResolveExecutable("shim.cmd", windows, error).empty(), "an explicit batch script is refused");
		CheckOneLine(error, "explicit batch script");
		Check(uam::custom_acp::ResolveExecutable("tool", windows, error) == win / "tool.exe", "a program name finds its .exe: " + error);
		Check(uam::custom_acp::ResolveExecutable("agent.ps1", windows, error).empty(), "a script that needs another program is refused");
		CheckOneLine(error, "powershell script");
		// An extensionless full path (an npm shim) would make Windows try name.cmd next to it, so it is refused.
		WriteFile(win / "npm-agent", "#!/bin/sh\n", false);
		WriteFile(win / "npm-agent.cmd", "@echo off\n", false);
		Check(uam::custom_acp::ResolveExecutable(uam::paths::Utf8PathString(win / "npm-agent"), windows, error).empty(), "an extensionless full path is refused on Windows");
		Check(error.find(".exe") != std::string::npos, "the error asks for the .exe file: " + error);
		Check(uam::custom_acp::ResolveExecutable(uam::paths::Utf8PathString(win / "tool.exe"), windows, error) == win / "tool.exe", "a full .exe path is used as given");
	}

	void LaunchCheckCases(const std::filesystem::path& python)
	{
		uam::custom_acp::ExecutableSearch search{{python.parent_path()}, false};
		AgentConfig config;
		std::vector<std::string> argv;
		std::vector<std::pair<std::string, std::string>> environment;
		CheckOneLine(uam::custom_acp::PrepareLaunch(config, search, false, argv, environment), "unconfigured launch");
		config.command = uam::paths::Utf8PathString(python);
		config.arguments = {"-I", "fixture.py"};
		config.environment = {{"UAM_FIXTURE_ECHO", "x"}};
		const std::string unapproved = uam::custom_acp::PrepareLaunch(config, search, false, argv, environment);
		CheckOneLine(unapproved, "unapproved launch");
		Check(unapproved.find("allow") != std::string::npos && argv.empty(), "an unapproved command never starts: " + unapproved);
		config.approved_fingerprint = uam::custom_acp::LaunchFingerprint(config);
		config.approved_executable = uam::paths::Utf8PathString(python);
		const std::string remote = uam::custom_acp::PrepareLaunch(config, search, true, argv, environment);
		CheckOneLine(remote, "remote launch");
		Check(argv.empty(), "SSH launches are refused in this slice");
		Check(uam::custom_acp::PrepareLaunch(config, search, false, argv, environment).empty(), "an approved local launch is prepared");
		Check(argv == std::vector<std::string>{uam::paths::Utf8PathString(python), "-I", "fixture.py"}, "the launch is the configured argument array");
		Check(environment.size() == 1 && environment[0].first == "UAM_FIXTURE_ECHO", "configured environment pairs are added");

		// The approval covers the program the command resolved to: another file on the search path needs a new one.
		AgentConfig moved = config;
		moved.command = "agent-copy";
		moved.approved_fingerprint = uam::custom_acp::LaunchFingerprint(moved);
		TempDirectory first, second;
		WriteFile(first.path / "agent-copy", "#!/bin/sh\n", true);
		WriteFile(second.path / "agent-copy", "#!/bin/sh\n", true);
		moved.approved_executable = uam::paths::Utf8PathString(second.path / "agent-copy");
		argv.clear();
		const std::string shadowed = uam::custom_acp::PrepareLaunch(moved, {{first.path, second.path}, false}, false, argv, environment);
		CheckOneLine(shadowed, "shadowed program");
		Check(shadowed.find("different program") != std::string::npos && argv.empty(), "a different program on the search path needs a new approval: " + shadowed);

		AgentConfig bad = config;
		TempDirectory missing_root;
		const std::string missing_path = uam::paths::Utf8PathString(missing_root.path / "uam-custom-acp-agent");
		bad.command = missing_path;
		bad.approved_fingerprint = uam::custom_acp::LaunchFingerprint(bad);
		bad.approved_executable = missing_path;
		argv.clear();
		const std::string missing = uam::custom_acp::PrepareLaunch(bad, search, false, argv, environment);
		CheckOneLine(missing, "bad command");
		Check(missing.rfind("Custom ACP agent command not found: ", 0) == 0 && missing.find("uam-custom-acp-agent") != std::string::npos && argv.empty(),
		    "a bad command gives one clear launch error: " + missing);
	}

#if !defined(_WIN32)
	/// A minimal stdio client: the fixture is a real process started from an argument array with posix_spawn (no shell).
	class StdioAgent
	{
	  public:
		StdioAgent(const std::vector<std::string>& argv, const std::vector<std::pair<std::string, std::string>>& overrides)
		{
			int in[2], out[2], err[2];
			Check(::pipe(in) == 0 && ::pipe(out) == 0 && ::pipe(err) == 0, "pipes");
			posix_spawn_file_actions_t actions;
			posix_spawn_file_actions_init(&actions);
			posix_spawn_file_actions_adddup2(&actions, in[0], 0);
			posix_spawn_file_actions_adddup2(&actions, out[1], 1);
			posix_spawn_file_actions_adddup2(&actions, err[1], 2);
			for (int fd : {in[0], in[1], out[0], out[1], err[0], err[1]}) posix_spawn_file_actions_addclose(&actions, fd);
			std::vector<std::string> environment;
			for (char** entry = environ; *entry != nullptr; ++entry) environment.emplace_back(*entry);
			for (const auto& [name, value] : overrides) environment.push_back(name + "=" + value);
			std::vector<char*> env_ptrs;
			for (std::string& entry : environment) env_ptrs.push_back(entry.data());
			env_ptrs.push_back(nullptr);
			std::vector<std::string> args = argv;
			std::vector<char*> arg_ptrs;
			for (std::string& arg : args) arg_ptrs.push_back(arg.data());
			arg_ptrs.push_back(nullptr);
			const int spawned = ::posix_spawn(&m_pid, args.front().c_str(), &actions, nullptr, arg_ptrs.data(), env_ptrs.data());
			posix_spawn_file_actions_destroy(&actions);
			::close(in[0]);
			::close(out[1]);
			::close(err[1]);
			m_stdin = in[1];
			m_stdout = out[0];
			m_stderr = err[0];
			Check(spawned == 0, "posix_spawn failed: " + std::to_string(spawned));
		}

		~StdioAgent()
		{
			if (m_stdin >= 0) ::close(m_stdin);
			if (m_pid > 0 && !m_reaped)
			{
				::kill(m_pid, SIGTERM);
				int status = 0;
				::waitpid(m_pid, &status, 0);
			}
			::close(m_stdout);
			::close(m_stderr);
		}

		void Send(const nlohmann::json& message)
		{
			const std::string line = message.dump() + "\n";
			Check(::write(m_stdin, line.data(), line.size()) == static_cast<ssize_t>(line.size()), "write to agent");
		}

		/// The next JSON-RPC message, or null at end of output.
		nlohmann::json Next()
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
			for (;;)
			{
				if (const std::size_t end = m_buffer.find('\n'); end != std::string::npos)
				{
					const std::string line = m_buffer.substr(0, end);
					m_buffer.erase(0, end + 1);
					const nlohmann::json parsed = nlohmann::json::parse(line, nullptr, false);
					Check(!parsed.is_discarded(), "agent wrote a line that is not JSON: " + line);
					return parsed;
				}
				const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
				Check(left > 0, "timed out waiting for the agent");
				pollfd poll_fd{m_stdout, POLLIN, 0};
				const int ready = ::poll(&poll_fd, 1, static_cast<int>(left));
				if (ready < 0 && errno == EINTR) continue;
				Check(ready > 0, "timed out waiting for the agent");
				char chunk[4096];
				const ssize_t read = ::read(m_stdout, chunk, sizeof(chunk));
				if (read <= 0) return nullptr;
				m_buffer.append(chunk, static_cast<std::size_t>(read));
			}
		}

		std::string Stderr()
		{
			std::string text;
			char chunk[1024];
			ssize_t read = 0;
			while ((read = ::read(m_stderr, chunk, sizeof(chunk))) > 0) text.append(chunk, static_cast<std::size_t>(read));
			return text;
		}

		int Wait()
		{
			int status = 0;
			::waitpid(m_pid, &status, 0);
			m_reaped = true;
			return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
		}

	  private:
		pid_t m_pid = -1;
		bool m_reaped = false;
		int m_stdin = -1;
		int m_stdout = -1;
		int m_stderr = -1;
		std::string m_buffer;
	};

	nlohmann::json Result(StdioAgent& agent, int id)
	{
		const nlohmann::json message = agent.Next();
		Check(message.is_object() && message.value("id", -1) == id && message.contains("result"), "expected the result of request " + std::to_string(id) + ", got " + message.dump());
		return message["result"];
	}

	nlohmann::json Update(StdioAgent& agent, const std::string& session_id)
	{
		const nlohmann::json message = agent.Next();
		Check(message.is_object() && message.value("method", "") == uam::acp_methods::kSessionUpdate, "expected session/update, got " + message.dump());
		Check(message["params"].value("sessionId", "") == session_id, "updates name the session");
		return message["params"]["update"];
	}

	nlohmann::json Prompt(int id, const std::string& session_id, const std::string& text)
	{
		return uam::acp_json_rpc::Request(id, uam::acp_methods::kSessionPrompt,
		    {{"sessionId", session_id}, {"prompt", nlohmann::json::array({{{"type", "text"}, {"text", text}}})}});
	}

	nlohmann::json PermissionAnswer(const nlohmann::json& request, const std::string& option_id)
	{
		return uam::acp_json_rpc::SuccessResponse(request["id"],
		    {{uam::acp_permissions::kOutcomeField, {{uam::acp_permissions::kOutcomeField, uam::acp_permissions::kSelectedOutcome}, {uam::acp_permissions::kOptionIdField, option_id}}}});
	}

	void PermissionRoundTrip(StdioAgent& agent, int id, const std::string& session_id, const std::string& option_id, const std::string& expected_text, const std::string& expected_status)
	{
		agent.Send(Prompt(id, session_id, "permission"));
		const nlohmann::json tool_call = Update(agent, session_id);
		Check(tool_call.value("sessionUpdate", "") == "tool_call" && tool_call.value("status", "") == "pending", "the tool call is reported before asking");
		const nlohmann::json request = agent.Next();
		Check(request.value("method", "") == uam::acp_methods::kSessionRequestPermission && request.contains("id"), "the agent asks for permission");
		Check(request["params"]["options"].size() == 2 && request["params"]["toolCall"].value("toolCallId", "") == tool_call.value("toolCallId", ""), "the request names the tool call and its options");
		agent.Send(PermissionAnswer(request, option_id));
		const nlohmann::json tool_update = Update(agent, session_id);
		Check(tool_update.value("sessionUpdate", "") == "tool_call_update" && tool_update.value("status", "") == expected_status, "the tool call reports the decision");
		const nlohmann::json chunk = Update(agent, session_id);
		Check(chunk["content"].value("text", "") == expected_text, "the agent reports the outcome: " + chunk.dump());
		Check(Result(agent, id).value("stopReason", "") == "end_turn", "the permission turn ends");
	}

	void StdioSessionCase(const std::filesystem::path& python, const std::filesystem::path& fixture)
	{
		TempDirectory workspace;
		AgentConfig config;
		config.display_name = "Scripted fixture";
		config.command = python.filename().string(); // Found on the search path, then launched by full path.
		const std::string tricky_name = "Fixture; echo pwned $HOME `id` \"quoted\"";
		config.arguments = {"-I", uam::paths::Utf8PathString(fixture), "--name", tricky_name, "--models"};
		config.environment = {{"UAM_FIXTURE_ECHO", "value with spaces; $NOT_EXPANDED"}};
		config.approved_fingerprint = uam::custom_acp::LaunchFingerprint(config);
		config.approved_executable = uam::paths::Utf8PathString(python.parent_path() / python.filename());
		std::vector<std::string> argv;
		std::vector<std::pair<std::string, std::string>> environment;
		const std::string error = uam::custom_acp::PrepareLaunch(config, {{python.parent_path()}, false}, false, argv, environment);
		Check(error.empty(), "the fixture launch is prepared: " + error);
		Check(std::filesystem::path(argv.front()).is_absolute(), "the launch uses the resolved program");
		StdioAgent agent(argv, environment);

		// initialize, as UAM's shared ACP defaults send it.
		agent.Send(uam::acp_json_rpc::Request(1, uam::acp_methods::kInitialize,
		    {{"protocolVersion", 1}, {"clientCapabilities", {{"session", {{"compaction", nlohmann::json::object()}}}}}, {"clientInfo", uam::acp_request_defaults::ClientInfo()}}));
		const nlohmann::json initialized = Result(agent, 1);
		Check(initialized.value("protocolVersion", 0) == 1, "the agent answers initialize");
		Check(initialized["agentInfo"].value("title", "") == tricky_name, "arguments reach the agent unchanged, with no shell: " + initialized["agentInfo"].dump());
		Check(initialized["agentCapabilities"]["promptCapabilities"].value("image", true) == false, "unreported features stay off");

		agent.Send(uam::acp_json_rpc::Request(2, uam::acp_methods::kSessionNew, {{"cwd", uam::paths::Utf8PathString(workspace.path)}, {"mcpServers", nlohmann::json::array()}}));
		const nlohmann::json session = Result(agent, 2);
		const std::string session_id = session.value("sessionId", "");
		Check(!session_id.empty(), "session/new returns a session");
		Check(session["models"]["availableModels"].size() == 2 && session["modes"]["availableModes"].size() == 2, "reported models and modes arrive with the session");

		// A streamed prompt.
		agent.Send(Prompt(3, session_id, "hello"));
		std::string streamed;
		for (int index = 0; index < 3; ++index)
		{
			const nlohmann::json update = Update(agent, session_id);
			Check(update.value("sessionUpdate", "") == "agent_message_chunk", "text streams as message chunks");
			streamed += update["content"].value("text", "");
		}
		Check(streamed == "Hello from the scripted agent.", "the streamed text arrives in order: " + streamed);
		Check(Result(agent, 3).value("stopReason", "") == "end_turn", "the streamed turn ends");

		// Exact argument and environment delivery.
		agent.Send(Prompt(4, session_id, "argv"));
		const nlohmann::json echoed = nlohmann::json::parse(Update(agent, session_id)["content"].value("text", ""));
		Check(echoed["argv"] == nlohmann::json(std::vector<std::string>{"--name", tricky_name, "--models"}), "every argument arrives as one element: " + echoed.dump());
		Check(echoed["echo"] == "value with spaces; $NOT_EXPANDED", "environment values arrive unexpanded");
		Check(Result(agent, 4).value("stopReason", "") == "end_turn", "the argv turn ends");

		// Permission round-trips, allowed and rejected.
		PermissionRoundTrip(agent, 5, session_id, "allow", "Permission granted.", "completed");
		PermissionRoundTrip(agent, 6, session_id, "reject", "Permission denied.", "failed");

		// Cancel.
		agent.Send(Prompt(7, session_id, "wait"));
		Check(Update(agent, session_id)["content"].value("text", "") == "Working", "the long turn starts");
		agent.Send(uam::acp_json_rpc::Notification(uam::acp_methods::kSessionCancel, {{"sessionId", session_id}}));
		Check(Result(agent, 7).value("stopReason", "") == "cancelled", "session/cancel ends the turn as cancelled");
	}

	void EarlyExitCase(const std::filesystem::path& python, const std::filesystem::path& fixture)
	{
		StdioAgent agent({uam::paths::Utf8PathString(python), "-I", uam::paths::Utf8PathString(fixture), "--exit-code", "3"}, {});
		Check(agent.Next().is_null(), "an agent that exits early closes its output");
		Check(agent.Wait() == 3, "its exit code is observable");
		Check(agent.Stderr().find("exiting before the handshake") != std::string::npos, "its error output is observable");
	}
#endif
} // namespace

int main(int argc, char** argv)
{
	if (argc != 3)
	{
		std::cerr << "usage: uam_custom_acp_agent_tests <python3> <custom_acp_agent.py>\n";
		return 2;
	}
	const std::filesystem::path python = std::filesystem::absolute(uam::paths::PathFromUtf8(argv[1]));
	const std::filesystem::path fixture = std::filesystem::absolute(uam::paths::PathFromUtf8(argv[2]));
	try
	{
		ValidationCases();
		ApprovalCases();
		StorageCases();
		ResolutionCases();
		LaunchCheckCases(python);
#if !defined(_WIN32)
		StdioSessionCase(python, fixture);
		EarlyExitCase(python, fixture);
		std::cout << "Custom ACP agent tests passed, including the stdio session with the scripted agent.\n";
#else
		(void)fixture;
		std::cout << "Custom ACP agent configuration tests passed; the stdio session case runs on macOS and Linux only.\n";
#endif
	}
	catch (const std::exception& error)
	{
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
	return 0;
}
