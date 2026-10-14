#include "remote/runner_startup_observation.h"
#include "test_harness.h"
#include "remote/runner_client.h"

using namespace uam_test;

#if defined(__APPLE__) || defined(__linux__)
UAM_TEST(RemoteStartupObservationReadsOwnedRegistration)
{
	TempDir temp("uam-startup-observation");
	const fs::path home = fs::canonical(temp.root);
	const fs::path executable = home / "runner/new/uam-runner";
	const fs::path socket = home / "runner/endpoint.sock";
	const fs::path unit = home / ".config/systemd/user" / (uam::remote::RunnerStartupIdentity(executable) + ".service");
	const fs::path manager = home / "systemctl";
	UAM_ASSERT(uam::io::WriteTextFile(manager,
	    "#!/bin/sh\ncase \"$2\" in\nis-enabled) printf '%s\\n' \"$UAM_OBSERVATION_STATE\"; exit \"$UAM_OBSERVATION_EXIT\";;\n"
	    "show) printf '%s\\n' \"$UAM_OBSERVATION_FRAGMENT\";;\n*) exit 99;;\nesac\n"));
	fs::permissions(manager, fs::perms::owner_all);
	ScopedEnvVar path("PATH", home.string() + ":/usr/bin:/bin");
	ScopedEnvVar fragment("UAM_OBSERVATION_FRAGMENT", unit.string());
	std::string error;
	const auto observe = [&]()
	{
		return uam::remote::ObserveLinuxRunnerStartup(PlatformServicesFactory::Instance().process_service,
		    home, executable, socket, error);
	};
	{
		ScopedEnvVar state("UAM_OBSERVATION_STATE", "not-found");
		ScopedEnvVar exit_code_env("UAM_OBSERVATION_EXIT", "4");
		UAM_ASSERT_EQ(observe(), "disabled");
		UAM_ASSERT(error.empty());
	}
	const std::string contents = uam::remote::LinuxRunnerStartupUnit(executable, socket);
	UAM_ASSERT(uam::io::WriteTextFile(unit, contents));
	{
		ScopedEnvVar state("UAM_OBSERVATION_STATE", "enabled");
		ScopedEnvVar exit_code_env("UAM_OBSERVATION_EXIT", "0");
		UAM_ASSERT_EQ(observe(), "enabled");
		{
			ScopedEnvVar wrong("UAM_OBSERVATION_FRAGMENT", (home / "other.service").string());
			UAM_ASSERT(observe().empty());
			UAM_ASSERT(!error.empty());
		}
		UAM_ASSERT_EQ(uam::io::ReadTextFile(unit), contents);
	}
	{
		ScopedEnvVar state("UAM_OBSERVATION_STATE", "disabled");
		ScopedEnvVar exit_code_env("UAM_OBSERVATION_EXIT", "1");
		UAM_ASSERT_EQ(observe(), "disabled");
	}
	{
		ScopedEnvVar state("UAM_OBSERVATION_STATE", "enabled-runtime");
		ScopedEnvVar exit_code_env("UAM_OBSERVATION_EXIT", "0");
		UAM_ASSERT(observe().empty());
	}
	{
		ScopedEnvVar state("UAM_OBSERVATION_STATE", "disabled");
		ScopedEnvVar exit_code_env("UAM_OBSERVATION_EXIT", "99");
		UAM_ASSERT(observe().empty());
	}
	{
		ScopedEnvVar state("UAM_OBSERVATION_STATE", "Failed to connect to bus");
		ScopedEnvVar exit_code_env("UAM_OBSERVATION_EXIT", "1");
		UAM_ASSERT(observe().empty());
		UAM_ASSERT(!error.empty());
	}
	UAM_ASSERT(uam::io::WriteTextFile(unit, "Unowned entry"));
	UAM_ASSERT(observe().empty());
	UAM_ASSERT_EQ(uam::io::ReadTextFile(unit), "Unowned entry");
	fs::remove(unit);
	fs::create_symlink(manager, unit);
	UAM_ASSERT(observe().empty());
}
#endif

std::optional<int> RunRemoteOldStartupEchoFixture(int argc, char** argv)
{
	if (argc != 2 || std::string_view(argv[1]) != "--uam-test-old-startup-echo") return std::nullopt;
	nlohmann::json request;
	while (uam::remote::ReadFrame(std::cin, request) == uam::remote::FrameReadResult::Ok)
	{
		nlohmann::json response;
		if (request.value("type", "") == "hello")
		{
			response = {{"id", request["id"]}, {"ok", true}, {"nonce", request["nonce"]},
			    {"protocolVersion", request["protocolVersion"]}, {"runnerVersion", "development"},
			    {"capabilities", {{"processExecution", true}, {"fileCopy", true}, {"computerUse", false}, {"runnerStartup", true}}}};
		}
		else
		{
			const bool enabled = request.value("enabled", false);
			response = {{"id", request["id"]}, {"ok", true},
			    {"result", {{"enabled", enabled}, {"status", enabled ? "enabled" : "disabled"}}}};
		}
		if (!uam::remote::WriteFrame(std::cout, response)) return 1;
		std::cout.flush();
	}
	return 0;
}

UAM_TEST(RemoteOldStartupEchoCannotConfirmAppliedState)
{
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	uam::remote::RunnerClient client(service,
	    {uam::paths::Utf8PathString(service.ResolveCurrentExecutablePath()), "--uam-test-old-startup-echo"}, "development");
	std::string error;
	UAM_ASSERT(client.Connect(&error));
	UAM_ASSERT(!client.ConfigureStartup(true, &error));
	UAM_ASSERT(!error.empty());
	std::string observed = "disabled";
	UAM_ASSERT(!client.ObserveStartup(observed, &error));
	UAM_ASSERT(observed.empty());
}

namespace
{
	class StartupObservationProcessFixture final : public IPlatformProcessService
	{
	  public:
		ProcessExecutionResult registration;
		ProcessExecutionResult fragment;
		bool SupportsDetachedProcesses() const override
		{
			return {};
		}
		bool PopulateLocalTime(std::time_t timestamp, std::tm* tm_out) const override
		{
			return {};
		}
		std::string BuildShellCommandWithWorkingDirectory(const std::filesystem::path& working_directory, const std::string& command) const override
		{
			return {};
		}
		bool CaptureCommandOutput(const std::string& command, std::string* output_out, int* raw_status_out, std::string* error_out) const override
		{
			return {};
		}
		int NormalizeCapturedCommandExitCode(int raw_status) const override
		{
			return {};
		}
		ProcessExecutionResult ExecuteCommand(const std::string& command, int timeout_ms, std::stop_token stop_token) const override
		{
			return command.find("is-enabled") != std::string::npos ? registration : fragment;
		}
		bool StartStdioProcess(uam::platform::StdioProcessPlatformFields& process, const std::filesystem::path& working_directory, const std::vector<std::string>& argv, std::string* error_out, const std::vector<std::pair<std::string, std::string>>& environment_overrides) const override
		{
			return {};
		}
		bool StartStdioProcessWithInput(uam::platform::StdioProcessPlatformFields& process, const std::filesystem::path& working_directory, const std::vector<std::string>& argv, std::string_view standard_input, std::string* error_out, const std::vector<std::pair<std::string, std::string>>& environment_overrides) const override
		{
			return {};
		}
		void CloseStdioProcessHandles(uam::platform::StdioProcessPlatformFields& process) const override
		{
		}
		bool WriteToStdioProcess(uam::platform::StdioProcessPlatformFields& process, const char* bytes, std::size_t len, std::string* error_out) const override
		{
			return {};
		}
		void CloseStdioProcessInput(uam::platform::StdioProcessPlatformFields& process) const override
		{
		}
		void TerminateStdioProcess(uam::platform::StdioProcessPlatformFields& process, bool fast_exit) const override
		{
		}
		void StopStdioProcess(uam::platform::StdioProcessPlatformFields& process, bool fast_exit) const override
		{
		}
		std::ptrdiff_t ReadStdioProcessStdout(uam::platform::StdioProcessPlatformFields& process, char* buffer, std::size_t buffer_size, std::string* error_out) const override
		{
			return {};
		}
		std::ptrdiff_t ReadStdioProcessStderr(uam::platform::StdioProcessPlatformFields& process, char* buffer, std::size_t buffer_size, std::string* error_out) const override
		{
			return {};
		}
		bool PollStdioProcessExited(uam::platform::StdioProcessPlatformFields& process, int* exit_code_out) const override
		{
			return {};
		}
		std::filesystem::path ResolveCurrentExecutablePath() const override
		{
			return {};
		}
		std::unique_ptr<uam::platform::DataRootLock> TryAcquireDataRootLock(const std::filesystem::path& data_root, std::string* error_out) const override
		{
			return {};
		}
		uintmax_t NativeGeminiSessionMaxFileBytes() const override
		{
			return {};
		}
		std::size_t NativeGeminiSessionMaxMessages() const override
		{
			return {};
		}
		std::string GenerateUuid() const override
		{
			return {};
		}
		bool LaunchShellAt(const std::filesystem::path& working_directory, std::string* error_out) const override
		{
			return {};
		}
		void SetKeepSystemAwake(bool keep_awake) const override
		{
		}
		bool EmbeddedBrowserUsesMockKeychain() const override
		{
			return {};
		}
	};
}

UAM_TEST(RemoteStartupObservationRejectsIncompleteProcessResults)
{
	TempDir temp("uam-startup-result-guard");
	const fs::path home = fs::canonical(temp.root);
	const fs::path executable = home / "runner/new/uam-runner";
	const fs::path socket = home / "runner/endpoint.sock";
	const fs::path unit = home / ".config/systemd/user" / (uam::remote::RunnerStartupIdentity(executable) + ".service");
	UAM_ASSERT(uam::io::WriteTextFile(unit, uam::remote::LinuxRunnerStartupUnit(executable, socket)));
	StartupObservationProcessFixture service;
	std::string error;
	const auto observe = [&]()
	{
		return uam::remote::ObserveLinuxRunnerStartup(service, home, executable, socket, error);
	};
	for (int phase = 0; phase != 2; ++phase)
	{
		for (int failure = 0; failure != 4; ++failure)
		{
			service.registration = ProcessExecutionResult{true, false, false, false, 0, "enabled\n", {}};
			service.fragment = ProcessExecutionResult{true, false, false, false, 0, unit.string() + "\n", {}};
			ProcessExecutionResult& result = phase == 0 ? service.registration : service.fragment;
			if (failure == 0) result.timed_out = true;
			if (failure == 1) result.canceled = true;
			if (failure == 2) result.output_truncated = true;
			if (failure == 3) result.error = "Capture failed.";
			UAM_ASSERT(observe().empty());
			UAM_ASSERT(!error.empty());
		}
	}
	service.registration = ProcessExecutionResult{false, false, false, false, 1, "disabled\n", {}};
	UAM_ASSERT_EQ(observe(), "disabled");
	fs::remove(unit);
	service.registration = ProcessExecutionResult{false, false, false, false, 4, "not-found\n", {}};
	UAM_ASSERT_EQ(observe(), "disabled");
}
