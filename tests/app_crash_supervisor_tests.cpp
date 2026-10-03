#include "common/platform/app_crash_supervisor.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace
{
	void Crash()
	{
#if defined(_WIN32)
		SetErrorMode(SEM_NOGPFAULTERRORBOX);
		RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#else
		raise(SIGABRT);
#endif
	}
}

int main(int argc, char** argv)
{
	if (argc < 2) return 2;
	const std::string mode = argv[1];
	if (mode == "bypass")
	{
		for (const std::string& service : {std::string("--type=renderer"), std::string("--uam-control-stdio")})
			if (uam::platform::RunAppCrashSupervisor({argv[0], service, "--uam-supervised-gui"}).has_value()) return 1;
		return 0;
	}
	const std::vector<std::string> launch_arguments(argv, argv + argc);
	const bool child = std::find(launch_arguments.begin(), launch_arguments.end(), "--uam-supervised-gui") != launch_arguments.end();
	if (child) (void)uam::platform::RunAppCrashSupervisor(launch_arguments);
	if (child)
	{
		const char* count_file = std::getenv("UAM_SUPERVISOR_TEST_COUNT_FILE");
		if (count_file == nullptr) return 2;
		int count = 0;
		{ std::ifstream input(count_file); input >> count; }
		{ std::ofstream output(count_file); output << count + 1; }
		if (mode == "hold")
		{
#if defined(_WIN32)
			const unsigned long child_pid = GetCurrentProcessId();
#else
			const int child_pid = getpid();
#endif
			{ std::ofstream output(std::filesystem::path(count_file).parent_path() / "child.pid"); output << child_pid; }
			std::this_thread::sleep_for(std::chrono::seconds(60));
		}
		if (mode == "loop" || (mode == "once" && count == 0)) Crash();
		if (mode == "term")
		{
#if defined(_WIN32)
			TerminateProcess(GetCurrentProcess(), 0xC000013AUL);
#else
			raise(SIGTERM);
#endif
		}
		return mode == "nonzero" ? 7 : 0;
	}

	const char* requested_root = std::getenv("UAM_SUPERVISOR_TEST_ROOT");
	const std::filesystem::path directory = requested_root != nullptr ? std::filesystem::path(requested_root) : std::filesystem::temp_directory_path() /
	    ("uam-supervisor-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(directory);
#if defined(_WIN32)
	_putenv_s("UAM_DATA_DIR", directory.string().c_str());
#else
	setenv("UAM_DATA_DIR", directory.string().c_str(), 1);
#endif

	const std::filesystem::path count_path = directory / "count";

#if defined(_WIN32)
	_putenv_s("UAM_SUPERVISOR_TEST_COUNT_FILE", count_path.string().c_str());
#else
	setenv("UAM_SUPERVISOR_TEST_COUNT_FILE", count_path.string().c_str(), 1);
#endif
	const std::vector<std::string> arguments = {std::filesystem::absolute(argv[0]).string(), mode};
	const std::optional<int> result = uam::platform::RunAppCrashSupervisor(arguments);
	const std::filesystem::path actual_count_path = count_path;
	int count = 0;
	{ std::ifstream input(actual_count_path); input >> count; }
	const int expected_count = mode == "once" ? 2 : mode == "loop" ? 4 : 1;
#if defined(_WIN32)
	const int expected_exit = mode == "loop" ? static_cast<int>(EXCEPTION_ACCESS_VIOLATION) : mode == "term" ? static_cast<int>(0xC000013AUL) : mode == "nonzero" ? 7 : 0;
#else
	const int expected_exit = mode == "loop" ? 128 + SIGABRT : mode == "term" ? 128 + SIGTERM : mode == "nonzero" ? 7 : 0;
#endif
	std::filesystem::remove_all(directory);
	if (!result.has_value() || *result != expected_exit || count != expected_count)
	{
		std::cerr << mode << ": launches=" << count << " expected=" << expected_count << '\n';
		return 1;
	}
	std::cout << "PASS " << mode << ": launches=" << count << '\n';
	return 0;
}
