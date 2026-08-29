#include "remote/runner_protocol.h"
#include "remote/runner_proxy.h"
#include "remote/runner_state.h"

#if defined(__APPLE__)
#include "common/platform/platform_application_macos.h"
#endif
#if defined(__APPLE__) || defined(__linux__)
#include "remote/runner_service_posix.h"
#elif defined(_WIN32)
#include "remote/runner_service_windows.h"
#endif

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>

#ifndef UAM_REMOTE_RUNNER_VERSION
#define UAM_REMOTE_RUNNER_VERSION "development"
#endif

int main(int argc, char** argv)
{
#if defined(__APPLE__)
	if (const std::optional<int> watchdog_result =
	        uam::platform::RunMacParentDeathWatchdogIfRequested(argc, argv);
	    watchdog_result.has_value())
		return *watchdog_result;
#endif
	if (argc == 2 && std::string(argv[1]) == "--version")
	{
		std::cout << UAM_REMOTE_RUNNER_VERSION << '\n';
		return 0;
	}
	if (argc == 2 && std::string(argv[1]) == "--protocol-version")
	{
		std::cout << uam::remote::kRunnerProtocolVersion << '\n';
		return 0;
	}
	if (argc == 8 && std::string(argv[1]) == "proxy" &&
	    std::string(argv[2]) == "--alias" && std::string(argv[4]) == "--platform" &&
	    std::string(argv[6]) == "--version")
		return uam::remote::RunProcessProxy(argv[3], argv[5], argv[7]);
	if (argc == 10 && std::string(argv[1]) == "proxy" &&
	    std::string(argv[2]) == "--alias" && std::string(argv[4]) == "--platform" &&
	    std::string(argv[6]) == "--version" && std::string(argv[8]) == "--directory")
		return uam::remote::RunProcessProxy(argv[3], argv[5], argv[7], argv[9]);
	if (argc == 3 && std::string(argv[1]) == "terminal")
		return uam::remote::RunTerminalProcess(argv[2]);
#if defined(__APPLE__) || defined(__linux__)
	if (argc == 6 && std::string(argv[1]) == "mcp" &&
	    std::string(argv[2]) == "--channel" && std::string(argv[4]) == "--socket")
		return uam::remote::RunRemoteMcpShim(argv[3], argv[5]);
	if (argc == 4 && std::string(argv[2]) == "--socket")
	{
		if (std::string(argv[1]) == "serve")
			return uam::remote::RunRunnerService(argv[3], UAM_REMOTE_RUNNER_VERSION);
		if (std::string(argv[1]) == "start")
			return uam::remote::StartRunnerService(argv[3], UAM_REMOTE_RUNNER_VERSION);
		if (std::string(argv[1]) == "stop")
			return uam::remote::StopRunnerService(argv[3]);
		if (std::string(argv[1]) == "bridge")
			return uam::remote::RunRunnerBridge(argv[3]);
	}
#elif defined(_WIN32)
	if (argc == 4 && std::string(argv[1]) == "mcp" &&
	    std::string(argv[2]) == "--channel")
		return uam::remote::RunRemoteMcpShim(argv[3]);
	if (argc == 2)
	{
		if (std::string(argv[1]) == "serve")
			return uam::remote::RunRunnerService(UAM_REMOTE_RUNNER_VERSION);
		if (std::string(argv[1]) == "start")
			return uam::remote::StartRunnerService(UAM_REMOTE_RUNNER_VERSION);
		if (std::string(argv[1]) == "stop") return uam::remote::StopRunnerService();
		if (std::string(argv[1]) == "bridge") return uam::remote::RunRunnerBridge();
	}
#endif
	if (argc != 2 || std::string(argv[1]) != "bridge-direct")
	{
		std::cerr << "Usage: uam-runner start|serve|stop|bridge --socket PATH | proxy --alias SSH_ALIAS --platform OS --version VERSION [--directory HOME_RELATIVE_PATH] | terminal SPEC | mcp --channel ID --socket PATH | --version | --protocol-version\n";
		return 2;
	}

	uam::remote::RunnerState state;
	for (;;)
	{
		nlohmann::json request;
		std::string error;
		const uam::remote::FrameReadResult result =
		    uam::remote::ReadFrame(std::cin, request, &error);
		if (result == uam::remote::FrameReadResult::EndOfStream) return 0;
		if (result == uam::remote::FrameReadResult::Error)
		{
			std::cerr << error << '\n';
			return 2;
		}
		if (!uam::remote::WriteFrame(
		        std::cout,
		        uam::remote::HandleRunnerRequest(request, UAM_REMOTE_RUNNER_VERSION, &state),
		        &error))
		{
			std::cerr << error << '\n';
			return 2;
		}
	}
}
