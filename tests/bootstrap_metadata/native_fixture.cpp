// Compiles the production bootstrap, replacing only the platform boundary. This
// executable never starts a process; generated scripts are exported for separate
// disposable-filesystem controls. No app, SSH client or real runner is linked.
#include "remote/runner_bootstrap.h"
#include "windows_command_assertions.h"
#include "remote/runner_protocol.h"
#include "remote/runner_client.h"
#include "app/memory_library_service.h"
#include "common/platform/platform_services.h"
#include "common/platform/platform_state_fields.h"
#include "common/utils/base64.h"
#include "common/utils/io_utils.h"
#include "common/config/execution_host_config.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>

namespace fs = std::filesystem;
using Process = uam::platform::StdioProcessPlatformFields;
using json = nlohmann::json;

namespace
{
	void Require(bool condition, const std::string& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	std::string DecodeCommand(std::string command)
	{
		const std::string utf8 = "FromBase64String('";
		if (const auto start = command.find(utf8); start != std::string::npos)
		{
			const auto begin = start + utf8.size();
			Require(uam::base64::Decode(command.substr(begin, command.find('\'', begin) - begin), command), "Invalid UTF-8 wrapper");
		}
		else if (const auto start = command.find("-EncodedCommand "); start != std::string::npos)
		{
			std::string utf16;
			Require(uam::base64::Decode(command.substr(start + 16), utf16), "Invalid UTF-16 wrapper");
			command.clear();
			for (std::size_t index = 0; index < utf16.size(); index += 2) command += utf16[index];
		}
		return command;
	}

	// The unused interfaces reject calls rather than constructing native UI/path
	// services. Only StartStdioProcess/reads/writes below are part of the test.
	class FixtureServices final : public IPlatformProcessService, public IPlatformTerminalRuntime,
	    public IPlatformFileDialogService, public IPlatformPathService, public IPlatformDictationService
	{
		struct State { std::string output; int status = 0; bool guard = false; bool exited = true; std::string diagnostic; bool client = false; };
	  public:
		mutable std::map<Process*, State> processes;
		mutable std::map<std::string, std::string> commands;
		mutable std::vector<std::string> phases;
		std::string platform = "linux";
		mutable std::string fail_phase;
		mutable bool held = false;
		mutable int starts = 0;
		mutable int releases = 0;
		mutable bool refuse_rollback = false;
		mutable bool lose_guard = false;
		mutable bool recovered_prior = false;
		mutable bool stale_rollback_reply = false;
		mutable bool disconnect_activation_reply = false;
		mutable bool read_error = false;
		mutable bool authorized = false;

		void Reset(std::string target)
		{
			Require(!held, "A preceding native fixture leaked its lease");
			processes.clear(); commands.clear(); phases.clear(); fail_phase.clear();
			platform = std::move(target); starts = releases = 0;
			refuse_rollback = lose_guard = recovered_prior = stale_rollback_reply = disconnect_activation_reply = read_error = authorized = false;
		}
		State Command(std::string command) const
		{
			command = DecodeCommand(std::move(command));
			std::string phase;
			std::string output;
			if (command.find("$a=[IO.File]::Open((Join-Path $root 'install.authorize-") != std::string::npos) { phase = "authorize"; authorized = true; }
			else if (command.find("journal=") != std::string::npos || command.find("$journal=Join-Path") != std::string::npos) { phase = "recover"; output = recovered_prior ? "R\n" : ""; }
			else if (command.find("New-UamMetadata $installed $metadata") != std::string::npos) phase = "prepare";
			else if (command.find("$file=$installed;") != std::string::npos) phase = "publish";
			else if (command.find("Set-UamRunnerFile $file $installed $backup") != std::string::npos || command.find("if mv -f") != std::string::npos) phase = "activate";
			else if (command.find("Undo-UamTransaction $installed $backup $marker $metadata") != std::string::npos || command.find("uam_restore_metadata; fi; rm -f") != std::string::npos) phase = "rollback";
			else if (command.find("UAM_INSTALL_COMMITTED:") != std::string::npos)
			{
				phase = "commit";
				output = "UAM_INSTALL_COMMITTED:metadata\n";
			}
			else if (command.find("UAM_HELPER_RETIRED:") != std::string::npos) phase = "retire";
			else if (command.find("install.release-") != std::string::npos) phase = "release";
			else if (command.ends_with(" --version")) { phase = "version"; output = "4.5.7\n"; }
			else if (command.ends_with(" --protocol-version")) { phase = "protocol"; output = std::to_string(uam::remote::kRunnerProtocolVersion) + "\n"; }
			else if (command.find("mkdir -p") != std::string::npos || command.find("New-Item -ItemType Directory") != std::string::npos) phase = "directory";
			else if (command.find("socket=\"$HOME/") != std::string::npos) phase = "endpoint";
			else throw std::runtime_error("Unrecognized generated command: " + command.substr(0, 150));
			Require(held, "Mutation or verification occurred outside the native install lease");
			if (phase != "authorize" && phase != "release") Require(authorized, "Recovery/mutation preceded durable authorisation");
			phases.push_back(phase); commands[phase] = command;
			if (phase == "rollback" && refuse_rollback) return {"Injected rollback refusal", 255};
			if (phase == "release")
			{
				held = false; ++releases;
				for (auto& [key, state] : processes) if (state.guard) state.exited = true;
			}
			if (phase == fail_phase)
			{
				fail_phase.clear();
				if (lose_guard)
				{
					held = false;
					for (auto& [key, state] : processes) if (state.guard) state.exited = true;
				}
				return {"Injected lost acknowledgement or refusal", 255};
			}
			if (phase == "commit" && fail_phase == "commit-cleanup")
			{
				fail_phase.clear();
				return {output + "Injected cleanup diagnostic\n", 255};
			}
			return {output};
		}
		bool StartStdioProcess(Process& process, const fs::path&, const std::vector<std::string>& argv,
		    std::string*, const std::vector<std::pair<std::string, std::string>>&) const override
		{
			++starts;
			Require(!argv.empty() && (argv[0] == "ssh" || argv[0] == "scp"), "Unexpected platform launch");
			State state;
			const std::string command = DecodeCommand(argv.back());
			if (argv[0] == "scp") { Require(held, "Copy occurred without lease"); phases.push_back("copy"); }
			else if (command == "uname -s && uname -m") state = platform == "linux" ? State{"Linux\nx86_64\n"} : State{"", 1};
			else if (command == "'Windows'; $env:PROCESSOR_ARCHITECTURE") state.output = "Windows\nAMD64\n";
			else if (command.find(" bridge --socket ") != std::string::npos) { state.client = true; state.exited = false; phases.push_back("ordinary-bridge"); }
			else if (const auto ready = command.find("UAM_INSTALL_LOCK_READY:"); ready != std::string::npos)
			{
				if (held) state = {"UAM_INSTALL_BUSY\n", 73, false, true, "The remote helper is busy with another installation."};
				else
				{
					held = true; commands["guard"] = command; phases.push_back("guard");
					const auto begin = ready + 23;
					const auto end = command.find_first_of("'\"", begin);
					state = {"UAM_INSTALL_LOCK_READY:" + command.substr(begin, end - begin) + (platform == "windows" ? "\r\n" : "\n"), 0, true, false};
					
				}
			}
			else state = Command(command);
			processes[&process] = std::move(state);
			return true;
		}
		bool WriteToStdioProcess(Process& process, const char* bytes, std::size_t length, std::string*) const override
		{
			if (processes.at(&process).client)
			{
				Require(length > 4, "Truncated client frame");
				const auto request = json::parse(std::string(bytes + 4, length - 4));
				Require(request.value("type", "") == "hello", "Unexpected ordinary client request");
				const std::string response = json{{"id",request["id"]},{"ok",true},{"nonce",request["nonce"]},
				    {"protocolVersion",request["protocolVersion"]},{"runnerVersion","4.5.7"},
				    {"capabilities",{{"processExecution",true},{"fileCopy",true},{"computerUse",false}}}}.dump();
				std::string frame; for (int shift : {24,16,8,0}) frame += static_cast<char>((response.size() >> shift) & 255);
				processes.at(&process).output += frame + response;
				return true;
			}
			const std::string request(bytes, length - 1);
			if (request == "UAM_INSTALL_AUTHORIZE:metadata") { authorized = true; phases.push_back("authorize"); return true; }
			const auto delimiter = request.find(':');
			Require(delimiter != std::string::npos, "Untagged guarded request");
			std::string command;
			Require(uam::base64::Decode(request.substr(delimiter + 1), command), "Invalid guarded request");
			const State reply = Command(command);
			const auto id = request.substr(0, delimiter);
			auto& output = processes.at(&process).output;
			if (stale_rollback_reply && phases.back() == "rollback")
				output += "UAM_INSTALL_RESULT:" + std::to_string(std::stoull(id) - 1) + ":0:" + uam::base64::Encode("delayed activation success") + "\n";
			output += "UAM_INSTALL_RESULT:" + id + ":" + std::to_string(reply.status) + ":" + uam::base64::Encode(reply.output) + "\n";
			if (disconnect_activation_reply && phases.back() == "activate") read_error = true;
			return true;
		}
		std::ptrdiff_t ReadStdioProcessStdout(Process& process, char* bytes, std::size_t length, std::string*) const override
		{
			if (read_error) { read_error = false; return -1; }
			auto& output = processes.at(&process).output;
			const auto count = std::min(length, output.size());
			std::memcpy(bytes, output.data(), count); output.erase(0, count);
			return count ? static_cast<std::ptrdiff_t>(count) : -2;
		}
		bool PollStdioProcessExited(Process& process, int* status) const override
		{
			const auto& state = processes.at(&process);
			if (status) *status = state.status;
			return state.exited;
		}
		void CloseStdioProcessInput(Process& process) const override
		{
			auto& state = processes.at(&process);
			if (state.client) state.exited = true;
			if (state.guard && !state.exited) { held = false; state.exited = true; ++releases; }
		}
		void CloseStdioProcessHandles(Process&) const override {}
		void TerminateStdioProcess(Process& p, bool) const override { CloseStdioProcessInput(p); }
		void StopStdioProcess(Process& p, bool) const override { CloseStdioProcessInput(p); }
		std::ptrdiff_t ReadStdioProcessStderr(Process& process, char* bytes, std::size_t length, std::string*) const override
		{
			auto& output = processes.at(&process).diagnostic;
			const auto count = std::min(length, output.size());
			std::memcpy(bytes, output.data(), count); output.erase(0, count);
			return count ? static_cast<std::ptrdiff_t>(count) : -2;
		}
		bool StartStdioProcessWithInput(Process&, const fs::path&, const std::vector<std::string>&, std::string_view, std::string*, const std::vector<std::pair<std::string, std::string>>&) const override { throw std::runtime_error("Unexpected stdin process"); }
		bool SupportsDetachedProcesses() const override { return false; }
		bool PopulateLocalTime(std::time_t, std::tm*) const override { return false; }
		std::string BuildShellCommandWithWorkingDirectory(const fs::path&, const std::string&) const override { return {}; }
		bool CaptureCommandOutput(const std::string&, std::string*, int*, std::string*) const override { return false; }
		int NormalizeCapturedCommandExitCode(int status) const override { return status; }
		ProcessExecutionResult ExecuteCommand(const std::string&, int, std::stop_token) const override { throw std::runtime_error("Unexpected command"); }
		fs::path ResolveCurrentExecutablePath() const override { return {}; }
		std::unique_ptr<uam::platform::DataRootLock> TryAcquireDataRootLock(const fs::path&, std::string*) const override { return {}; }
		uintmax_t NativeGeminiSessionMaxFileBytes() const override { return 0; }
		std::size_t NativeGeminiSessionMaxMessages() const override { return 0; }
		std::string GenerateUuid() const override { return {}; }
		bool LaunchShellAt(const fs::path&, std::string*) const override { throw std::runtime_error("Unexpected shell"); }
		void SetKeepSystemAwake(bool) const override {}
		bool EmbeddedBrowserUsesMockKeychain() const override { return false; }
		bool IsAvailable() const override { return false; }
		bool StartCliTerminalProcess(uam::CliTerminalState&, const fs::path&, const std::vector<std::string>&, std::string*, const std::vector<std::pair<std::string, std::string>>&) const override { throw std::runtime_error("Unexpected terminal"); }
		void CloseCliTerminalHandles(uam::CliTerminalState&) const override {}
		bool WriteToCliTerminal(uam::CliTerminalState&, const char*, std::size_t) const override { return false; }
		void StopCliTerminalProcess(uam::CliTerminalState&, bool) const override {}
		void ResizeCliTerminal(uam::CliTerminalState&) const override {}
		std::ptrdiff_t ReadCliTerminalOutput(uam::CliTerminalState&, char*, std::size_t) const override { return -1; }
		bool HasReadableTerminalOutputHandle(const uam::CliTerminalState&) const override { return false; }
		bool PollCliTerminalProcessExited(uam::CliTerminalState&) const override { return true; }
		bool SupportsAsyncNativeGeminiHistoryRefresh() const override { return false; }
		bool SupportsNativeDialogs() const override { return false; }
		bool BrowsePath(PlatformPathBrowseTarget, const fs::path&, std::string*, std::string*) const override { return false; }
		bool OpenFolderInFileManager(const fs::path&, std::string*, const fs::path&) const override { return false; }
		bool OpenFolderInEditorPreset(const fs::path&, const std::string&, std::string*) const override { return false; }
		bool RevealPathInFileManager(const fs::path&, std::string*) const override { return false; }
		bool OpenFileInTextEditor(const fs::path&, std::string*) const override { return false; }
		fs::path DefaultDataRootPath() const override { throw std::runtime_error("Unexpected data root"); }
		std::optional<fs::path> ResolveUserHomePath() const override { throw std::runtime_error("Unexpected home lookup"); }
		fs::path ExpandLeadingTildePath(const std::string&) const override { return {}; }
		bool CanProbeDirectoryWithoutPrompt(const fs::path&) const override { return false; }
		bool EnsureHiddenDirectory(const fs::path&, std::error_code*) const override { return false; }
		bool Start(const DictationOptions&, std::string*) override { return false; }
		void Stop(std::string_view) override {}
		bool IsRunning() const override { return false; }
		std::vector<DictationEvent> PollEvents() override { return {}; }
	};
	FixtureServices service;

	uam::remote::BootstrapResult ExecuteFixtureBootstrap(const uam::remote::BootstrapPlan& plan, std::stop_token stop = {})
	{
		auto acquired = uam::remote::AcquireBootstrapPlan(plan, stop);
		if (!acquired.ok) return acquired;
		const auto identity = uam::remote::BootstrapInstallIdentity(plan, acquired);
		return uam::remote::ExecuteBootstrapPlan(plan, std::move(acquired), plan.nonce, identity, stop);
	}

	struct Artifact
	{
		fs::path directory = fs::temp_directory_path() / ("uam-bootstrap-native-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		Artifact()
		{
			fs::create_directories(directory);
			Require(uam::io::WriteTextFile(directory / "uam-runner", "inert artifact: never executed"), "Artifact write");
			Manifest(json{{"sourceFingerprint", std::string(64, 'b')}, {"protocolVersion", uam::remote::kRunnerProtocolVersion}}.dump());
		}
		~Artifact() { std::error_code error; fs::remove_all(directory, error); }
		void Manifest(std::string bytes) const { Require(uam::io::WriteTextFile(directory / "uam-runner.manifest.json", bytes), "Manifest write"); }
		uam::remote::BootstrapPlan Plan(std::string platform) const
		{
			uam::remote::BootstrapPlan plan;
			std::string error;
			Require(uam::remote::BuildBootstrapPlan("fixture", "4.5.7", "metadata", {{platform, "x86_64", directory / "uam-runner", std::string(64, 'a')}}, plan, &error, "r"), error);
			return plan;
		}
	};

	json Capture(std::string platform)
	{
		Artifact artifact;
		service.Reset(platform);
		const auto plan = artifact.Plan(platform);
		const auto result = ExecuteFixtureBootstrap(plan);
		Require(result.ok, result.error);
		Require(uam::remote::FinalizeBootstrapPlan(plan, result, false), "Cannot capture rollback");
		auto commands = service.commands;
		service.Reset(platform);
		const auto committed = ExecuteFixtureBootstrap(plan);
		Require(committed.ok, committed.error);
		Require(uam::remote::FinalizeBootstrapPlan(plan, committed, true), "Cannot capture commit");
		commands["commit"] = service.commands.at("commit");
		if (platform == "windows") commands["retire"] = service.commands.at("retire");
		return commands;
	}

	void ManifestValidation()
	{
		Artifact artifact;
		Require(artifact.Plan("linux").artifacts.front().source_fingerprint == std::string(64, 'b'), "Valid packaged fingerprint was not captured");
		for (const std::string bytes : {std::string("{"), std::string("[]"), std::string(4097, ' '),
		    json{{"sourceFingerprint", std::string(64, 'B')}, {"protocolVersion", 3}}.dump(),
		    json{{"sourceFingerprint", std::string(64, 'b')}, {"protocolVersion", 0}}.dump(),
		    json{{"sourceFingerprint", std::string(64, 'b')}, {"protocolVersion", "3"}}.dump()})
		{
			artifact.Manifest(bytes);
			bool refused = false;
			try { (void)artifact.Plan("linux"); } catch (const std::exception&) { refused = true; }
			Require(refused, "Malformed manifest accepted");
		}
		fs::remove(artifact.directory / "uam-runner.manifest.json");
		bool refused = false;
		try { (void)artifact.Plan("windows"); } catch (const std::exception&) { refused = true; }
		Require(refused, "Missing manifest accepted");
		Require(service.starts == 0, "Manifest validation started a process");
	}

	void NativeTransactionSequencing()
	{
		for (const auto platform : {"linux", "windows"})
		{
			const auto captured = Capture(platform);
			Require(captured.contains("rollback") && captured.contains("commit") && captured.contains("activate"), "Missing transaction route");
			const auto index = [](const std::string& phase) { return std::ranges::find(service.phases, phase) - service.phases.begin(); };
			Require(index("guard") < index("copy") && index("copy") < index("activate") && index("activate") < index("commit"), "Native lease/activation order");
			if (platform == std::string("windows")) Require(index("commit") < index("retire") && index("retire") < index("release"), "Native retirement must follow the commit inside the lease");
			if (platform == std::string("windows")) Require(index("prepare") < index("activate") && index("activate") < index("publish") && index("publish") < index("version"), "Native publication order");
			Require(!service.held && service.releases == 1, "Native finalization did not release lease exactly once");
		}
	}

	void NativeFailureRollback()
	{
		Artifact artifact;
		for (const auto platform : {"linux", "windows"})
		{
			for (const std::string phase : {"activate", "version", "protocol", "prepare", "publish"})
			{
				if (platform == std::string("linux") && (phase == "prepare" || phase == "publish")) continue;
				service.Reset(platform); service.fail_phase = phase;
				const auto result = ExecuteFixtureBootstrap(artifact.Plan(platform));
				Require(!result.ok, "Failed mutation acknowledgement accepted");
				Require(service.commands.contains("rollback"), "Native failed phase skipped transactional rollback: " + phase);
				Require(!service.held, "Native failed phase leaked lease");
			}
		}
	}

	void NativeCancellation()
	{
		Artifact artifact; service.Reset("linux"); std::stop_source stop; stop.request_stop();
		const auto result = ExecuteFixtureBootstrap(artifact.Plan("linux"), stop.get_token());
		Require(!result.ok && result.error.find("canceled") != std::string::npos, "Cancellation was not retained");
		Require(service.starts == 0, "Canceled bootstrap reached process service");
	}

	void NativeLeaseCompetition()
	{
		Artifact artifact; service.Reset("linux");
		auto first_plan = artifact.Plan("linux");
		std::string error;
		auto first = uam::remote::AcquireBootstrapInstallLease(first_plan, "linux", error);
		Require(first != nullptr && service.held, "First installation lease was not held");
		auto second_plan = first_plan; second_plan.ssh_alias = "another-alias"; second_plan.nonce = "another-owner";
		auto second = uam::remote::AcquireBootstrapInstallLease(second_plan, "linux", error);
		Require(second == nullptr, "Competing installation was admitted");
		Require(error.find("busy") != std::string::npos, "Busy lock diagnostic was not retained: " + error);
		Require(service.held && service.releases == 0, "Failed competitor released another owner's lease");
		first.reset();
		Require(!service.held && service.releases == 1, "Owned abandoned lease did not release");
		second = uam::remote::AcquireBootstrapInstallLease(second_plan, "linux", error);
		Require(second != nullptr, "Installation could not retry after owned release");
		second.reset();
	}

	void WindowsReplacementPlan()
	{
		uam::remote::BootstrapPlan plan;
		plan.ssh_alias = "fixture"; plan.runner_directory = std::string(240, 'd'); plan.version = std::string(64, 'v'); plan.nonce = std::string(64, 'n');
		const auto argv = uam::remote::BuildBootstrapLockArgv(plan, "windows");
		Require(!argv.empty() && argv.back().size() < 8000, "Windows lock exceeds command limit");
		uam_test::bootstrap_command_checks::WindowsLock(plan, argv);
		const std::string script = DecodeCommand(argv.back());
		Require(script.find("[IO.File]::Replace($src,$dst,$save)") != std::string::npos, "Loaded-image replacement route lost");
		Require(script.find("Copy-Item -LiteralPath $old -Destination $restore") != std::string::npos, "Rollback no longer stages unexecuted bytes");
		Require(script.find("Copy-Item -LiteralPath $old -Destination $bin") == std::string::npos, "Rollback writes a loaded image directly");
		Require(script.find("Restore-UamMetadata $bin $meta") < script.find("if ($restart)"), "Recovery restarts before restoring metadata");
	}

	void WindowsArtifactCoreAssertions()
	{
		Artifact artifact; service.Reset("windows");
		auto plan = artifact.Plan("windows");
		plan.previous_platform = "windows"; plan.previous_version = "4.4.0";
		plan.previous_runner_directory = "old-tools/uam"; plan.previous_protocol_version = uam::remote::kRunnerProtocolVersion;
		const auto result = ExecuteFixtureBootstrap(plan);
		Require(result.ok, result.error);
		Require(uam::remote::FinalizeBootstrapPlan(plan, result, true), "Cannot finalise Windows assertion fixture");
		std::string commands;
		for (const auto& phase : service.phases)
			if (service.commands.contains(phase)) commands += service.commands.at(phase) + "\n";
		uam_test::bootstrap_command_checks::WindowsArtifact(plan, commands);
	}

	void NativeCommitOutcomes()
	{
		Artifact artifact;
		for (const auto platform : {"linux", "windows"})
		{
			for (const std::string failure : {"", "commit", "commit-cleanup"})
			{
				service.Reset(platform);
				auto plan = artifact.Plan(platform);
				auto result = ExecuteFixtureBootstrap(plan);
				Require(result.ok, "Commit fixture setup failed");
				service.fail_phase = failure;
				std::string error;
				bool committed = false;
				const bool finalized = uam::remote::FinalizeBootstrapPlan(plan, result, true, &error, {}, &committed);
				Require(finalized == failure.empty(), "Cleanup failure was hidden");
				Require(committed == (failure != "commit"), "Unknown commit was accepted or confirmed commit lost");
				result.install_lease.reset();
				Require(!service.held, "Commit outcome retained the native lease");
			}
			const auto starts = service.starts;
			uam::remote::BootstrapResult orphan;
			orphan.platform = platform;
			std::string error;
			bool committed = true;
			Require(!uam::remote::FinalizeBootstrapPlan(artifact.Plan(platform), orphan, true, &error, {}, &committed), "Commit reacquired a lease after recovery");
			Require(!committed && service.starts == starts, "Missing commit lease started recovery or acknowledged another tuple");
		}
	}

	void NativeDurableFence()
	{
		Artifact artifact;
		for (const std::string platform : {"linux", "windows"})
		{
			service.Reset(platform); service.recovered_prior = true;
			const auto plan = artifact.Plan(platform);
			auto acquired = uam::remote::AcquireBootstrapPlan(plan);
			Require(acquired.ok && service.phases == std::vector<std::string>{"guard"}, "Acquisition recovered before durable acknowledgement");
			auto rejected = uam::remote::ExecuteBootstrapPlan(plan, acquired, "wrong-owner", "wrong-identity");
			Require(!rejected.ok && service.phases.size() == 1, "Unbound persisted fence authorised recovery");
			rejected.install_lease.reset(); acquired.install_lease.reset();
			Require(!service.commands.contains("recover"), "Controller/callback loss recovered without fence");
			for (const std::string failure : {"recover", "activate", "publish", "version"})
			{
				if (platform == "linux" && failure == "publish") continue;
				service.Reset(platform); auto pending = uam::remote::AcquireBootstrapPlan(plan);
				ExecutionHost host; host.id="fixture"; host.transport="ssh"; host.ssh_alias="fixture"; host.runner_status="ready";
				uam::execution_hosts::BeginInstallFence(host, plan.nonce, uam::remote::BootstrapInstallIdentity(plan, pending));
				const auto durable = uam::execution_hosts::Serialize({host});
				const auto settings_path=artifact.directory/"settings.json";
				Require(uam::io::WriteTextFile(settings_path,durable.dump()), "Fixture durable fence save failed");
				service.fail_phase=failure; service.refuse_rollback=true;
				auto result = uam::remote::ExecuteBootstrapPlan(plan, std::move(pending), host.runner_install_nonce, host.runner_install_identity);
				// Lose completion: only the pre-recovery settings record survives.
				result.install_lease.reset(); const auto reloaded=uam::execution_hosts::Parse(json::parse(uam::io::ReadTextFile(settings_path))).back();
				Require(reloaded.runner_install_unresolved && reloaded.runner_install_nonce==plan.nonce && reloaded.runner_install_identity==host.runner_install_identity && !uam::execution_hosts::CanConnectRunner(reloaded), "Lost completion/settings reload admitted an unresolved tuple");
			}
		}
	}

	void NativeReplyIdentity()
	{
		Artifact artifact; const auto plan=artifact.Plan("linux");
		service.Reset("linux"); service.fail_phase="activate"; service.refuse_rollback=true; service.stale_rollback_reply=true;
		auto result=ExecuteFixtureBootstrap(plan);
		Require(!result.rollback_confirmed && !result.PreviousInstallationConfirmed(), "Delayed activation success falsely confirmed refused rollback");
		Require(result.error.find("uncorrelated")!=std::string::npos, "Stale reply was not rejected by operation identity"); result.install_lease.reset();
		service.Reset("linux"); service.disconnect_activation_reply=true; service.refuse_rollback=true;
		result=ExecuteFixtureBootstrap(plan);
		Require(!result.rollback_confirmed && !result.PreviousInstallationConfirmed() && !service.commands.contains("rollback"), "Uncertain stream was reused for rollback"); result.install_lease.reset();
	}

	void NativeHostClientAdmission()
	{
		service.Reset("linux"); uam::remote::RunnerClient raw(service, {"inert", "bridge"}); ExecutionHost host; host.id="fixture"; host.transport="ssh"; host.ssh_alias="fixture";
		host.platform="linux"; host.runner_version="4.5.7"; host.runner_protocol_version=uam::remote::kRunnerProtocolVersion;
		uam::execution_hosts::BeginInstallFence(host,"pending-owner","pending-identity");
		host=uam::execution_hosts::Parse(uam::execution_hosts::Serialize({host})).back();
		uam::remote::RunnerClient client(service,host); std::string error;
		uam::remote::DirectoryListing listing; std::vector<MemoryLibraryStore::Entry> entries; MemoryLibraryStore::Entry entry; MemoryLibraryStore::Draft draft;
		Require(!client.ListDirectories("/workspace",listing,&error), "Workspace validation bypassed installation fence");
		Require(!client.ListMemoryEntries("/workspace",entries,&error) && !client.CreateMemoryEntry("/workspace",draft,entry,&error) && !client.DeleteMemoryEntry("/workspace","entry",&error), "Memory CRUD bypassed installation fence");
		Require(!uam::remote::ValidateRemoteWorkspaceDirectory(service,host,"/workspace",&error), "Actual workspace validation worker bypassed fence");
		MemoryLibraryService::Scope scope; scope.scope_type="folder"; scope.folder_id="fixture"; scope.remote_host=host; scope.remote_workspace="/workspace";
		error.clear(); Require(MemoryLibraryService::ListEntries(scope,&error).empty() && !error.empty(), "Memory service listing bypassed fence");
		Require(!MemoryLibraryService::CreateEntry(scope,draft,&entry,&error) && !MemoryLibraryService::DeleteEntry(scope,"entry",&error), "Memory service mutations bypassed fence");
		Require(service.starts==0 && !error.empty(), "Fenced saved-host caller launched a bridge or request");
		host.runner_install_unresolved=false; host.runner_install_nonce.clear(); host.runner_install_identity.clear(); host.runner_status="error";
		Require(uam::remote::CanAttemptRunnerConnection(host), "Ordinary transient-error reconnect became unavailable");
		uam::remote::RunnerClient retry(service,host); Require(retry.Connect(&error) && service.starts==1, "Ordinary transient error could not complete a fresh injected handshake");
	}

	void NativeRecoveryAdmission()
	{
		Artifact artifact;
		for (const auto platform : {"linux", "windows"})
		{
			const auto pre_mutation = platform == std::string("windows") ? "prepare" : "endpoint";
			service.Reset(platform); service.fail_phase = pre_mutation;
			auto result = ExecuteFixtureBootstrap(artifact.Plan(platform));
			Require(!result.ok && result.PreviousInstallationConfirmed(), "Premutation refusal lost confirmed prior state");
			result.install_lease.reset();
			for (const auto failure : {"rollback-refused", "guard-lost", "restored"})
			{
				service.Reset(platform); service.fail_phase = "activate";
				service.refuse_rollback = failure == std::string("rollback-refused");
				service.lose_guard = failure == std::string("guard-lost");
				result = ExecuteFixtureBootstrap(artifact.Plan(platform));
				Require(!result.ok && result.activation_attempted, "Failed activation did not retain structured mutation state");
				Require(result.PreviousInstallationConfirmed() == (failure == std::string("restored")), "Unknown rollback restored prior Ready authority");
				ExecutionHost prior;
				if (!result.PreviousInstallationConfirmed()) uam::execution_hosts::RequireInstallReconciliation(prior);
				Require((prior.runner_status == "ready") == (failure == std::string("restored")), "Actual install failure transition published unverified Ready");
				result.install_lease.reset();
			}
			service.Reset(platform); service.recovered_prior = true; service.fail_phase = pre_mutation;
			result = ExecuteFixtureBootstrap(artifact.Plan(platform));
			Require(result.recovery_required && !result.PreviousInstallationConfirmed(), "Acquisition recovery was mistaken for intact prior state");
			result.install_lease.reset();
			service.Reset(platform);
			result = ExecuteFixtureBootstrap(artifact.Plan(platform));
			Require(result.ok && !result.PreviousInstallationConfirmed(), "Activation without commit/rollback remained confirmed");
			service.refuse_rollback = true; std::string error;
			result.rollback_confirmed = uam::remote::FinalizeBootstrapPlan(artifact.Plan(platform), result, false, &error);
			Require(!result.PreviousInstallationConfirmed(), "Handshake rollback refusal admitted prior Ready");
			result.install_lease.reset();
		}
		ExecutionHost host;
		host.id = "fixture"; host.transport = "ssh"; host.ssh_alias = "fixture"; host.runner_version = "4.5.7";
		host.runner_status = "error";
		Require(uam::execution_hosts::CanConnectRunner(host) && uam::execution_hosts::CanBrowseRemoteDirectories(host), "Ordinary transient error reconnect was removed");
		uam::execution_hosts::RequireInstallReconciliation(host);
		const auto saved = uam::execution_hosts::Serialize({host});
		const auto reloaded = uam::execution_hosts::Parse(saved);
		Require(reloaded.size() == 2 && reloaded[1].runner_install_unresolved, "Settings reload lost the install fence");
		host = reloaded[1]; auto observed = host;
		Require(!uam::execution_hosts::CanConnectRunner(host) && !uam::execution_hosts::CanBrowseRemoteDirectories(host), "Reloaded unresolved installation admitted new connections");
		Require(!uam::execution_hosts::ApplyHealthObservation(host, observed, true, "new-health"), "Compatible handshake cleared unresolved installation");
		Require(host.runner_status == "error" && host.runner_install_unresolved, "Health promotion overwrote authority fence");
		uam::execution_hosts::ApplyInstallCommit(host, false);
		Require(!uam::execution_hosts::CanConnectRunner(host), "Unknown commit cleared durable launch admission");
		uam::execution_hosts::ApplyInstallCommit(host, true);
		Require(uam::execution_hosts::CanConnectRunner(host) && uam::execution_hosts::ApplyHealthObservation(host, host, true, "reconciled"), "Settled installation could not reconnect");
	}
}

PlatformServices& PlatformServicesFactory::Instance()
{
	static PlatformServices services{service, service, service, service, service};
	return services;
}

int main(int argc, char** argv)
{
	const std::map<std::string, std::function<void()>> cases{
	    {"ManifestValidation", ManifestValidation}, {"NativeTransactionSequencing", NativeTransactionSequencing},
	    {"NativeFailureRollback", NativeFailureRollback}, {"NativeCancellation", NativeCancellation},
	    {"NativeLeaseCompetition", NativeLeaseCompetition}, {"WindowsReplacementPlan", WindowsReplacementPlan},
	    {"WindowsBootstrapLockFitsOpenSshCommandLimit", WindowsReplacementPlan},
	    {"RemoteRunnerBootstrapSelectsAndHardensTheWindowsArtifact", WindowsArtifactCoreAssertions},
	    {"NativeCommitOutcomes", NativeCommitOutcomes}, {"NativeRecoveryAdmission", NativeRecoveryAdmission},
	    {"NativeDurableFence", NativeDurableFence}, {"NativeReplyIdentity", NativeReplyIdentity}, {"NativeHostClientAdmission", NativeHostClientAdmission}};
	try
	{
		if (argc == 2 && std::string(argv[1]) == "--list-tests") { for (const auto& [name, fn] : cases) std::cout << name << '\n'; return 0; }
		if (argc == 2 && std::string(argv[1]) == "--dump-scripts") { std::cout << json{{"windows", Capture("windows")}, {"linux", Capture("linux")}, {"protocol", uam::remote::kRunnerProtocolVersion}}.dump(2) << '\n'; return 0; }
		Require(argc == 2 && cases.contains(argv[1]), "Use one exact case or --list-tests/--dump-scripts");
		cases.at(argv[1])(); std::cout << "PASS " << argv[1] << '\n'; return 0;
	}
	catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
