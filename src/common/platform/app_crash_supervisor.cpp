#include "common/platform/app_crash_supervisor.h"
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string_view>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#else
#include <cerrno>
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#if defined(__APPLE__)
#include <libproc.h>
#endif
#endif

namespace uam::platform
{
	namespace
	{
		std::atomic_bool supervised_gui{false};
		std::atomic_bool restart_requested{false};
		std::atomic_bool data_folder_requested{false};
		std::atomic_bool data_folder_shutdown_confirmed{false};
		std::atomic_bool data_folder_restart_confirmed{false};

		bool ParentRunsThisExecutable(std::uint32_t parent_pid)
		{
			std::error_code error;
#if defined(_WIN32)
			const HANDLE parent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent_pid);
			if (!parent) return false;
			std::array<wchar_t, 32768> source{}, own{};
			DWORD source_size = static_cast<DWORD>(source.size());
			const bool queried = QueryFullProcessImageNameW(parent, 0, source.data(), &source_size) && GetModuleFileNameW(nullptr, own.data(), static_cast<DWORD>(own.size()));
			CloseHandle(parent);
			return queried && std::filesystem::equivalent(source.data(), own.data(), error) && !error;
#elif defined(__APPLE__)
			std::array<char, PROC_PIDPATHINFO_MAXSIZE> source{}, own{};
			return proc_pidpath(parent_pid, source.data(), static_cast<std::uint32_t>(source.size())) > 0 &&
			    proc_pidpath(getpid(), own.data(), static_cast<std::uint32_t>(own.size())) > 0 &&
			    std::filesystem::equivalent(source.data(), own.data(), error) && !error;
#else
			return std::filesystem::equivalent(std::filesystem::path("/proc") / std::to_string(parent_pid) / "exe", "/proc/self/exe", error) && !error;
#endif
		}

		void LogRecovery(const int result, const int attempt, const bool exhausted)
		{
			std::filesystem::path directory;
			if (const char* data = std::getenv("UAM_DATA_DIR"); data != nullptr && *data != '\0') directory = std::filesystem::path(data) / "logs";
#if defined(_WIN32)
			else if (const char* local = std::getenv("LOCALAPPDATA")) directory = std::filesystem::path(local) / "Universal Agent Manager" / "logs";
#else
			else if (const char* home = std::getenv("HOME")) directory = std::filesystem::path(home) / "Library" / "Logs" / "Universal Agent Manager";
#endif
			if (directory.empty()) return;
			std::error_code error;
			std::filesystem::create_directories(directory, error);
			if (error) return;
			std::ofstream log(directory / "crash-recovery.log", std::ios::app);
			log << "exit=" << result << " restart=" << attempt << " action="
			    << (exhausted ? "restart_limit_reached" : "restarting") << '\n';
		}

		/// Bind shutdown to this supervisor's identity, including uncatchable parent termination.
		void WatchSupervisor(const std::vector<std::string>& arguments)
		{
			constexpr std::string_view prefix = "--uam-supervisor-pid=";
			std::uint32_t parent_pid = 0;
			bool has_parent_pid = false;
			for (const std::string& argument : arguments)
			{
				if (!argument.starts_with(prefix)) continue;
				if (has_parent_pid) std::_Exit(1);
				has_parent_pid = true;
				const std::string_view value(argument.data() + prefix.size(), argument.size() - prefix.size());
				const std::from_chars_result parsed = std::from_chars(value.data(), value.data() + value.size(), parent_pid);
				if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) std::_Exit(1);
			}
			if (parent_pid == 0 || parent_pid > static_cast<std::uint32_t>((std::numeric_limits<int>::max)())) std::_Exit(1);
			const bool folder_handoff = std::find(arguments.begin(), arguments.end(), "--uam-data-folder-normal-exit") != arguments.end();
			// A copied private flag and an arbitrary parent PID cannot vouch for a stopped profile.
			if (folder_handoff && !ParentRunsThisExecutable(parent_pid)) std::_Exit(1);
#if defined(_WIN32)
			// The private marker alone must not enable restart in an unrelated process.
			HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
			if (snapshot == INVALID_HANDLE_VALUE) std::_Exit(1);
			PROCESSENTRY32W entry{};
			entry.dwSize = sizeof(entry);
			bool parent_matches = false;
			if (Process32FirstW(snapshot, &entry))
			{
				do
				{
					if (entry.th32ProcessID != GetCurrentProcessId()) continue;
					parent_matches = entry.th32ParentProcessID == parent_pid;
					break;
				} while (Process32NextW(snapshot, &entry));
			}
			CloseHandle(snapshot);
			if (!parent_matches) std::_Exit(1);
			// The handle identifies the original process even if its PID is later reused.
			HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
			if (parent == nullptr) std::_Exit(0);
			std::thread([parent]()
			{
				(void)WaitForSingleObject(parent, INFINITE);
				CloseHandle(parent);
				std::_Exit(0);
			}).detach();
#else
			if (getppid() != static_cast<pid_t>(parent_pid)) std::_Exit(1);
			std::thread([parent_pid]()
			{
				while (getppid() == static_cast<pid_t>(parent_pid))
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
				std::_Exit(0);
			}).detach();
#endif
			supervised_gui.store(true, std::memory_order_release);
			data_folder_restart_confirmed.store(folder_handoff);
		}
	}

	bool RequestApplicationRestart(bool data_folder_transition)
	{
		if (!supervised_gui.load(std::memory_order_acquire)) return false;
		bool expected = false;
		if (!restart_requested.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
			return false;
		data_folder_requested.store(data_folder_transition);
		return true;
	}
	bool DataFolderRestartRequested()
	{
		return data_folder_requested.load();
	}
	bool ConfirmedDataFolderRestart()
	{
		return data_folder_restart_confirmed.load();
	}
	void ConfirmDataFolderShutdown(bool confirmed)
	{
		data_folder_shutdown_confirmed.store(confirmed);
	}
	int RequestedRestartExitCode()
	{
		return !DataFolderRestartRequested() ? kApplicationRestartExitCode : data_folder_shutdown_confirmed.load() ? kDataFolderRestartExitCode : 1;
	}

	bool ApplicationRestartRequested()
	{
		return restart_requested.load(std::memory_order_acquire);
	}

	std::optional<int> RunAppCrashSupervisor(const std::vector<std::string>& arguments)
	{
		constexpr const char* child_argument = "--uam-supervised-gui";
		if (arguments.empty()) return std::nullopt;
		for (const std::string& argument : arguments)
		{
			if ((argument.starts_with("--type=") || argument == "--type") || argument == "--uam-control-stdio") return std::nullopt;
		}
		if (std::find(arguments.begin(), arguments.end(), child_argument) != arguments.end())
		{
			WatchSupervisor(arguments);
			return std::nullopt;
		}
		// The stopped handoff marker is minted only after this supervisor observed full normal child exit.
		if (std::find(arguments.begin(), arguments.end(), "--uam-data-folder-normal-exit") != arguments.end())
			return 1;
		bool stopped_handoff = false;
		int restart_count = 0;
		for (;;)
		{
			const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
			bool crashed = false;
			int result = 1;
#if defined(_WIN32)
			// Preserve the original Unicode command line and Windows quoting verbatim.
			std::wstring command_line = GetCommandLineW();
			command_line += L" --uam-supervised-gui --uam-supervisor-pid=" + std::to_wstring(GetCurrentProcessId());
			if (stopped_handoff)
				command_line += L" --uam-data-folder-normal-exit";
			STARTUPINFOW startup{};
			startup.cb = sizeof(startup);
			PROCESS_INFORMATION process{};
			if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
				return 1;
			CloseHandle(process.hThread);
			const DWORD wait_result = WaitForSingleObject(process.hProcess, INFINITE);
			DWORD exit_code = 1;
			const BOOL got_exit = GetExitCodeProcess(process.hProcess, &exit_code);
			CloseHandle(process.hProcess);
			if (wait_result != WAIT_OBJECT_0 || !got_exit) return 1;
			result = static_cast<int>(exit_code);
			// NT exception statuses. Explicit termination and ordinary exit codes do not restart.
			crashed = exit_code >= 0x80000000UL && exit_code != 0xC000013AUL;
#else
			std::vector<std::string> child_arguments = arguments;
			child_arguments.emplace_back(child_argument);
			child_arguments.push_back("--uam-supervisor-pid=" + std::to_string(getpid()));
			if (stopped_handoff)
				child_arguments.emplace_back("--uam-data-folder-normal-exit");
			std::vector<char*> argv;
			for (std::string& argument : child_arguments) argv.push_back(argument.data());
			argv.push_back(nullptr);
			pid_t child_pid = 0;
			if (posix_spawnp(&child_pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) return 1;
			int status = 0;
			pid_t waited = 0;
			do { waited = waitpid(child_pid, &status, 0); } while (waited < 0 && errno == EINTR);
			if (waited < 0) return 1;
			if (WIFEXITED(status)) result = WEXITSTATUS(status);
			else if (WIFSIGNALED(status))
			{
				const int signal = WTERMSIG(status);
				result = 128 + signal;
				crashed = signal == SIGABRT || signal == SIGSEGV || signal == SIGBUS ||
				          signal == SIGILL || signal == SIGFPE || signal == SIGTRAP;
			}
#endif
			// The wait above observes full process exit, including normal GUI teardown.
			// Explicit requests neither consume nor reset the fatal-crash retry budget.
			stopped_handoff = !crashed && result == kDataFolderRestartExitCode;
			if (!crashed && (result == kApplicationRestartExitCode || result == kDataFolderRestartExitCode))
				continue;
			if (!crashed) return result;
			if (std::chrono::steady_clock::now() - started >= std::chrono::minutes(1)) restart_count = 0;
			if (++restart_count > 3)
			{
				LogRecovery(result, restart_count - 1, true);
				return result;
			}
			LogRecovery(result, restart_count, false);
			std::this_thread::sleep_for(std::chrono::seconds(restart_count));
		}
	}
}
