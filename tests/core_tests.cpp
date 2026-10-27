#include "common/companion/protocol.h"
#include "common/provider/copilot_native_history.h"
#include "core/chat_import_utils.h"
#include "cef/uam_bridge_request.h"
#include "common/platform/platform_speech_output.h"
#include "test_harness.h"
#include "common/platform/file_explorer_application.h"
#include "common/platform/powershell_selection.h"
#include "remote/remote_workspace_terminal.h"
#include "common/config/custom_icon.h"
#include "common/config/companion_network.h"
#include "app/chat_lifecycle_service.h"
#include "app/chat_search_service.h"
#include "app/local_chat_bundle_service.h"
#include "app/persistence_coordinator.h"
#include "app/transcript_retention_service.h"
#include "app/startup_native_import_service.h"
#include "app/runtime_orchestration_internal.h"
#include "app/uam_control_service.h"
#include "cef/state_serializer.h"
#include "cef/uam_cef_client.h"
#include "cef/uam_query_handler_internal.h"
#include "cef/uam_query_handler_async.h"
#include "common/config/mcp_server_config.h"
#include "common/chat/chat_repository.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_goal_loop.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/time_utils.h"
#include "common/utils/uuid.h"

#include <cstdio>
#include <fstream>
#include <future>
#include <type_traits>

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#include <versionhelpers.h>
#endif

#if defined(__APPLE__)
#include "common/platform/platform_application_macos.h"
#endif

using namespace uam_test;

#if defined(_WIN32)
namespace
{
	constexpr const char* kWindowsGrandchildFixture = "--uam-windows-grandchild-fixture";
	constexpr const char* kWindowsProviderFixture = "--uam-windows-provider-fixture";
	constexpr const char* kWindowsManagerFixture = "--uam-windows-manager-fixture";
	constexpr const char* kWindowsHandleProbeFixture = "--uam-windows-handle-probe-fixture";
	constexpr const char* kWindowsOutputFloodFixture = "--uam-windows-output-flood-fixture";
	constexpr const char* kWindowsFdStateFixture = "--uam-windows-fd-state-fixture";
	constexpr const wchar_t* kWindowsPrivateFdStateName = L"c28fc6f98a2c44abbbd89d6a3037d0d9_POSIX_FD_STATE";

	std::filesystem::path WindowsFixtureExecutablePath()
	{
		return PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath();
	}

	std::wstring QuoteWindowsFixtureArgument(const std::wstring& value)
	{
		return L"\"" + value + L"\"";
	}

	std::vector<wchar_t> BuildWindowsFixtureCommandLine(const std::vector<std::wstring>& argv)
	{
		std::wstring command;
		for (const std::wstring& arg : argv)
		{
			if (!command.empty())
			{
				command.push_back(L' ');
			}
			command += QuoteWindowsFixtureArgument(arg);
		}
		std::vector<wchar_t> result(command.begin(), command.end());
		result.push_back(L'\0');
		return result;
	}

	bool StartRawWindowsFixture(const std::vector<std::wstring>& argv, PROCESS_INFORMATION& process)
	{
		std::vector<wchar_t> command_line = BuildWindowsFixtureCommandLine(argv);
		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		return CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE;
	}

	std::vector<std::string> WindowsProviderFixtureArgv(
	    const std::filesystem::path& ready_path,
	    const std::filesystem::path& provider_pid_path,
	    const std::filesystem::path& descendant_pid_path,
	    const std::filesystem::path& marker_path,
	    bool crash)
	{
		return {
		    uam::paths::Utf8PathString(WindowsFixtureExecutablePath()),
		    kWindowsProviderFixture,
		    uam::paths::Utf8PathString(ready_path),
		    uam::paths::Utf8PathString(provider_pid_path),
		    uam::paths::Utf8PathString(descendant_pid_path),
		    uam::paths::Utf8PathString(marker_path),
		    crash ? "crash" : "wait"};
	}

	std::string WindowsFixtureShellCommand(const std::vector<std::string>& argv)
	{
		std::string command;
		for (const std::string& arg : argv)
		{
			if (!command.empty())
			{
				command.push_back(' ');
			}
			command += ShellQuoteForTest(arg);
		}
		return command;
	}

	bool WaitForWindowsFixtureReady(const std::filesystem::path& ready_path, std::chrono::milliseconds timeout = std::chrono::seconds(5))
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (uam::paths::PathExistsNoThrow(ready_path))
			{
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return false;
	}

	DWORD ReadWindowsFixturePid(const std::filesystem::path& path)
	{
		try
		{
			return static_cast<DWORD>(std::stoul(uam::strings::Trim(uam::io::ReadTextFile(path))));
		}
		catch (...)
		{
			return 0;
		}
	}

	class ScopedWindowsTestHandle
	{
	  public:
		ScopedWindowsTestHandle() = default;
		explicit ScopedWindowsTestHandle(HANDLE handle) : m_handle(handle) {}
		~ScopedWindowsTestHandle()
		{
			if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE)
			{
				CloseHandle(m_handle);
			}
		}
		ScopedWindowsTestHandle(const ScopedWindowsTestHandle&) = delete;
		ScopedWindowsTestHandle& operator=(const ScopedWindowsTestHandle&) = delete;
		ScopedWindowsTestHandle(ScopedWindowsTestHandle&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
		ScopedWindowsTestHandle& operator=(ScopedWindowsTestHandle&& other) noexcept
		{
			if (this != &other)
			{
				if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE)
				{
					CloseHandle(m_handle);
				}
				m_handle = std::exchange(other.m_handle, nullptr);
			}
			return *this;
		}
		HANDLE Get() const { return m_handle; }
		bool IsOpen() const { return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE; }

	  private:
		HANDLE m_handle = nullptr;
	};

	class ScopedRawWindowsProcess
	{
	  public:
		explicit ScopedRawWindowsProcess(HANDLE process) : m_process(process) {}
		~ScopedRawWindowsProcess()
		{
			if (m_process == nullptr || m_process == INVALID_HANDLE_VALUE)
			{
				return;
			}
			if (WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT)
			{
				(void)TerminateProcess(m_process, 99);
				(void)WaitForSingleObject(m_process, 3000);
			}
			CloseHandle(m_process);
		}
		ScopedRawWindowsProcess(const ScopedRawWindowsProcess&) = delete;
		ScopedRawWindowsProcess& operator=(const ScopedRawWindowsProcess&) = delete;
		HANDLE Get() const { return m_process; }

	  private:
		HANDLE m_process = nullptr;
	};

	struct WindowsObservedFixtureTree
	{
		ScopedWindowsTestHandle provider;
		ScopedWindowsTestHandle descendant;
	};

	WindowsObservedFixtureTree ObserveWindowsFixtureTree(
	    const std::filesystem::path& ready_path,
	    const std::filesystem::path& provider_pid_path,
	    const std::filesystem::path& descendant_pid_path,
	    bool provider_must_still_be_running = true)
	{
		UAM_ASSERT(WaitForWindowsFixtureReady(ready_path));
		const DWORD provider_pid = ReadWindowsFixturePid(provider_pid_path);
		const DWORD descendant_pid = ReadWindowsFixturePid(descendant_pid_path);
		UAM_ASSERT(provider_pid != 0);
		UAM_ASSERT(descendant_pid != 0);
		WindowsObservedFixtureTree tree;
		tree.provider = ScopedWindowsTestHandle(OpenProcess(SYNCHRONIZE, FALSE, provider_pid));
		tree.descendant = ScopedWindowsTestHandle(OpenProcess(SYNCHRONIZE, FALSE, descendant_pid));
		if (provider_must_still_be_running)
		{
			UAM_ASSERT(tree.provider.IsOpen());
		}
		UAM_ASSERT(tree.descendant.IsOpen());
		return tree;
	}

	void AssertWindowsFixtureTreeExited(const WindowsObservedFixtureTree& tree)
	{
		if (tree.provider.IsOpen())
		{
			UAM_ASSERT_EQ(WaitForSingleObject(tree.provider.Get(), 3000), static_cast<DWORD>(WAIT_OBJECT_0));
		}
		UAM_ASSERT(tree.descendant.IsOpen());
		UAM_ASSERT_EQ(WaitForSingleObject(tree.descendant.Get(), 3000), static_cast<DWORD>(WAIT_OBJECT_0));
	}

	std::optional<int> RunWindowsContainmentFixtureMode(int argc, char** argv)
	{
		if (argc >= 2 && std::string_view(argv[1]) == kWindowsGrandchildFixture)
		{
			if (argc != 3)
			{
				return 2;
			}
			Sleep(10000);
			(void)uam::io::WriteTextFile(uam::paths::PathFromUtf8(argv[2]), "descendant survived");
			Sleep(20000);
			return 0;
		}

		if (argc >= 2 && std::string_view(argv[1]) == kWindowsProviderFixture)
		{
			if (argc != 7)
			{
				return 2;
			}
			const std::filesystem::path executable = WindowsFixtureExecutablePath();
			PROCESS_INFORMATION descendant{};
			if (executable.empty() || !StartRawWindowsFixture(
			                              {executable.wstring(), std::filesystem::path(kWindowsGrandchildFixture).wstring(), uam::paths::PathFromUtf8(argv[5]).wstring()},
			                              descendant))
			{
				return 3;
			}
			CloseHandle(descendant.hThread);
			const bool wrote = uam::io::WriteTextFile(uam::paths::PathFromUtf8(argv[3]), std::to_string(GetCurrentProcessId())) &&
			                   uam::io::WriteTextFile(uam::paths::PathFromUtf8(argv[4]), std::to_string(descendant.dwProcessId)) &&
			                   uam::io::WriteTextFile(uam::paths::PathFromUtf8(argv[2]), "ready");
			if (!wrote)
			{
				(void)TerminateProcess(descendant.hProcess, 4);
				(void)WaitForSingleObject(descendant.hProcess, 3000);
				CloseHandle(descendant.hProcess);
				return 4;
			}
			CloseHandle(descendant.hProcess);
			if (std::string_view(argv[6]) == "crash")
			{
				Sleep(250);
				return 23;
			}
			Sleep(30000);
			return 0;
		}

		if (argc >= 2 && std::string_view(argv[1]) == kWindowsManagerFixture)
		{
			if (argc != 7)
			{
				return 2;
			}
			const std::vector<std::string> provider_argv = WindowsProviderFixtureArgv(
			    uam::paths::PathFromUtf8(argv[3]),
			    uam::paths::PathFromUtf8(argv[4]),
			    uam::paths::PathFromUtf8(argv[5]),
			    uam::paths::PathFromUtf8(argv[6]), false);
			const std::string_view transport = argv[2];
			std::string error;
			if (transport == "captured")
			{
				(void)PlatformServicesFactory::Instance().process_service.ExecuteCommand(WindowsFixtureShellCommand(provider_argv));
				return 31;
			}
			if (transport == "stdio")
			{
				uam::platform::StdioProcessPlatformFields process;
				if (!PlatformServicesFactory::Instance().process_service.StartStdioProcess(process, {}, provider_argv, &error))
				{
					return 32;
				}
				Sleep(30000);
				return 0;
			}
			if (transport == "terminal")
			{
				uam::CliTerminalState terminal;
				terminal.rows = uam::kCliTerminalDefaultRows;
				terminal.cols = uam::kCliTerminalDefaultCols;
				if (!PlatformServicesFactory::Instance().terminal_runtime.StartCliTerminalProcess(terminal, {}, provider_argv, &error))
				{
					return 33;
				}
				Sleep(30000);
				return 0;
			}
			return 34;
		}

		if (argc >= 2 && std::string_view(argv[1]) == kWindowsHandleProbeFixture)
		{
			if (argc != 4)
			{
				return 2;
			}
			const HANDLE candidate = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::stoull(argv[2])));
			const bool inherited = SetEvent(candidate) != FALSE;
			for (int line = 0; line < 2000; ++line)
			{
				std::cout << argv[3] << (inherited ? ":inherited\n" : ":isolated\n");
			}
			return inherited ? 41 : 0;
		}

		if (argc >= 2 && std::string_view(argv[1]) == kWindowsFdStateFixture)
		{
			if (argc != 3) return 2;
			wchar_t stale_state[64]{};
			SetLastError(ERROR_SUCCESS);
			if (GetEnvironmentVariableW(kWindowsPrivateFdStateName, stale_state, 64) != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND) return 51;
			char ordinary[64]{};
			if (GetEnvironmentVariableA("UAM_TEST_FD_STATE_ORDINARY", ordinary, 64) == 0 || std::string(ordinary) != argv[2]) return 52;
			static constexpr char marker[] = "fd-state-isolated\n";
			DWORD written = 0;
			return WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), marker, sizeof(marker) - 1, &written, nullptr) && written == sizeof(marker) - 1 ? 0 : 53;
		}

		if (argc >= 2 && std::string_view(argv[1]) == kWindowsOutputFloodFixture)
		{
			if (argc != 3)
			{
				return 2;
			}
			const HANDLE output = std::string_view(argv[2]) == "stderr" ? GetStdHandle(STD_ERROR_HANDLE) : GetStdHandle(STD_OUTPUT_HANDLE);
			std::array<char, 64 * 1024> bytes{};
			bytes.fill('x');
			for (std::size_t remaining = uam::platform::kCapturedCommandMaxOutputBytes + bytes.size(); remaining > 0;)
			{
				DWORD written = 0;
				const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(remaining, bytes.size()));
				if (!WriteFile(output, bytes.data(), requested, &written, nullptr) || written == 0)
				{
					return 0;
				}
				remaining -= written;
			}
			return 0;
		}

		return std::nullopt;
	}
} // namespace
#endif
UAM_TEST(AsyncByteWriterPreservesOrderAndBoundsQueue)
{
	std::mutex captured_mutex;
	std::string captured;
	uam::platform::AsyncByteWriter writer(
	    [&captured_mutex, &captured](const char* bytes, std::size_t len, std::string&) -> std::ptrdiff_t
	    {
		    std::lock_guard lock(captured_mutex);
		    captured.append(bytes, len);
		    return static_cast<std::ptrdiff_t>(len);
	    },
	    [] {});

	UAM_ASSERT(writer.Enqueue("first", 5));
	UAM_ASSERT(writer.Enqueue("-second", 7));
	UAM_ASSERT(writer.Flush(std::chrono::seconds(1)));
	UAM_ASSERT_EQ(writer.PendingBytes(), static_cast<std::size_t>(0));
	std::lock_guard lock(captured_mutex);
	UAM_ASSERT_EQ(captured, std::string("first-second"));
}

UAM_TEST(AsyncByteWriterReportsQueueLimitAndNoProgress)
{
	uam::platform::AsyncByteWriter writer(
	    [](const char*, std::size_t, std::string&) -> std::ptrdiff_t { return 0; },
	    [] {},
	    std::chrono::milliseconds(25));
	const std::string payload(uam::platform::kAsyncInputMaxQueuedBytes, 'x');
	UAM_ASSERT(writer.Enqueue(payload.data(), payload.size()));
	std::string error;
	UAM_ASSERT(!writer.Enqueue("x", 1, &error));
	UAM_ASSERT(uam::strings::Contains(error, "4 MiB"));

	uam::platform::AsyncByteWriter stalled_writer(
	    [](const char*, std::size_t, std::string&) -> std::ptrdiff_t { return 0; },
	    [] {},
	    std::chrono::milliseconds(25));
	UAM_ASSERT(stalled_writer.Enqueue("blocked", 7));
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	error.clear();
	UAM_ASSERT(stalled_writer.FailureOrStall(&error));
	UAM_ASSERT(uam::strings::Contains(error, "no progress"));
}

UAM_TEST(PlatformProviderInputQueuesDoNotBlockOnNonReadingChildren)
{
	TempDir temp("uam-async-provider-input");
	auto& process_service = PlatformServicesFactory::Instance().process_service;
	uam::platform::StdioProcessPlatformFields process;
	std::string error;
#if defined(_WIN32)
	const std::vector<std::string> argv = {"cmd.exe", "/C", "ping -n 31 127.0.0.1 >NUL"};
#else
	const std::vector<std::string> argv = {"/bin/sh", "-c", "sleep 30"};
#endif
	UAM_ASSERT(process_service.StartStdioProcess(process, temp.root, argv, &error));
	const std::string payload(2 * 1024 * 1024, 'x');
	const auto started_at = std::chrono::steady_clock::now();
	UAM_ASSERT(process_service.WriteToStdioProcess(process, payload.data(), payload.size(), &error));
	UAM_ASSERT(std::chrono::steady_clock::now() - started_at < std::chrono::milliseconds(250));
	UAM_ASSERT(process.stdin_writer != nullptr);
	const auto close_started_at = std::chrono::steady_clock::now();
	process_service.CloseStdioProcessInput(process);
	UAM_ASSERT(std::chrono::steady_clock::now() - close_started_at < std::chrono::seconds(2));
	process_service.StopStdioProcess(process, true);

	uam::CliTerminalState terminal;
	auto& terminal_runtime = PlatformServicesFactory::Instance().terminal_runtime;
	UAM_ASSERT(terminal_runtime.StartCliTerminalProcess(terminal, temp.root, argv, &error));
	const auto terminal_started_at = std::chrono::steady_clock::now();
	UAM_ASSERT(terminal_runtime.WriteToCliTerminal(terminal, payload.data(), payload.size()));
	UAM_ASSERT(std::chrono::steady_clock::now() - terminal_started_at < std::chrono::milliseconds(250));
	UAM_ASSERT(terminal.input_writer != nullptr);
	terminal_runtime.StopCliTerminalProcess(terminal, true);
}

UAM_TEST(StdioProcessEnvironmentOverridesAreChildScoped)
{
	TempDir temp("uam-stdio-environment");
	auto& process_service = PlatformServicesFactory::Instance().process_service;
	uam::platform::StdioProcessPlatformFields process;
	std::string error;
#if defined(_WIN32)
	const std::vector<std::string> argv = {"cmd.exe", "/C", "<nul set /p =%UAM_TEST_CHILD_ENV%"};
#else
	const std::vector<std::string> argv = {"/bin/sh", "-c", "printf %s \"$UAM_TEST_CHILD_ENV\""};
#endif
	UAM_ASSERT(process_service.StartStdioProcess(process, temp.root, argv, &error, {{"UAM_TEST_CHILD_ENV", "child-only"}}));
	process_service.CloseStdioProcessInput(process);

	std::string output;
	char buffer[128];
	bool exited = false;
	for (int attempt = 0; attempt < 200; ++attempt)
	{
		const std::ptrdiff_t read = process_service.ReadStdioProcessStdout(process, buffer, sizeof(buffer), &error);
		if (read > 0)
		{
			output.append(buffer, static_cast<std::size_t>(read));
		}
		exited = process_service.PollStdioProcessExited(process);
		if (exited && read <= 0)
		{
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	process_service.CloseStdioProcessHandles(process);
	UAM_ASSERT(exited);
	UAM_ASSERT_EQ(output, std::string("child-only"));
	UAM_ASSERT(std::getenv("UAM_TEST_CHILD_ENV") == nullptr || std::string(std::getenv("UAM_TEST_CHILD_ENV")) != "child-only");

	uam::platform::StdioProcessPlatformFields invalid_process;
	error.clear();
	UAM_ASSERT(!process_service.StartStdioProcess(invalid_process, temp.root, argv, &error, {{"INVALID=NAME", "value"}}));
	UAM_ASSERT(!error.empty());
}

#if defined(_WIN32)
namespace
{
	void AssertWindowsFdStateParentUnchanged()
	{
		wchar_t value[64]{};
		UAM_ASSERT_EQ(GetEnvironmentVariableW(kWindowsPrivateFdStateName, value, 64), static_cast<DWORD>(12));
		UAM_ASSERT_EQ(std::wstring(value), std::wstring(L"AAAAAAICAgA="));
		UAM_ASSERT_EQ(GetEnvironmentVariableW(L"UAM_TEST_FD_STATE_ORDINARY", value, 64), static_cast<DWORD>(9));
		UAM_ASSERT_EQ(std::wstring(value), std::wstring(L"inherited"));
	}
}

UAM_TEST(WindowsReplacedStdioDiscardsInheritedOpenSshFdState)
{
	TempDir temp("uam-windows-fd-state-stdio");
	ScopedWideWindowsEnvVar poison(kWindowsPrivateFdStateName, L"AAAAAAICAgA=");
	ScopedWideWindowsEnvVar ordinary(L"UAM_TEST_FD_STATE_ORDINARY", L"inherited");
	UAM_ASSERT(poison.applied && ordinary.applied);
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	for (bool override_ordinary : {false, true})
	{
		const std::string expected = override_ordinary ? "overridden" : "inherited";
		const std::vector<std::pair<std::string, std::string>> overrides = override_ordinary ?
		    std::vector<std::pair<std::string, std::string>>{{"UAM_TEST_FD_STATE_ORDINARY", expected}} : std::vector<std::pair<std::string, std::string>>{};
		uam::platform::StdioProcessPlatformFields process;
		struct StopOwnedProcess
		{
			IPlatformProcessService& service;
			uam::platform::StdioProcessPlatformFields& process;
			~StopOwnedProcess() { service.StopStdioProcess(process, true); }
		} cleanup{service, process};
		std::string error;
		UAM_ASSERT(service.StartStdioProcess(process, temp.root, {uam::paths::Utf8PathString(WindowsFixtureExecutablePath()), kWindowsFdStateFixture, expected}, &error, overrides));
		service.CloseStdioProcessInput(process);
		std::string output;
		std::array<char, 128> buffer{};
		bool exited = false;
		int exit_code = -1;
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < deadline)
		{
			const std::ptrdiff_t bytes = service.ReadStdioProcessStdout(process, buffer.data(), buffer.size(), &error);
			if (bytes > 0) output.append(buffer.data(), static_cast<std::size_t>(bytes));
			exited = service.PollStdioProcessExited(process, &exit_code);
			if (exited && bytes <= 0) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		AssertWindowsFdStateParentUnchanged();
		UAM_ASSERT(exited);
		UAM_ASSERT_EQ(exit_code, 0);
		UAM_ASSERT_EQ(output, std::string("fd-state-isolated\n"));
	}
}

UAM_TEST(WindowsCapturedCommandDiscardsInheritedOpenSshFdState)
{
	ScopedWideWindowsEnvVar poison(kWindowsPrivateFdStateName, L"AAAAAAICAgA=");
	ScopedWideWindowsEnvVar ordinary(L"UAM_TEST_FD_STATE_ORDINARY", L"inherited");
	UAM_ASSERT(poison.applied && ordinary.applied);
	const ProcessExecutionResult result = PlatformServicesFactory::Instance().process_service.ExecuteCommand(
	    WindowsFixtureShellCommand({uam::paths::Utf8PathString(WindowsFixtureExecutablePath()), kWindowsFdStateFixture, "inherited"}), 5000);
	AssertWindowsFdStateParentUnchanged();
	UAM_ASSERT(result.ok);
	UAM_ASSERT_EQ(result.exit_code, 0);
	UAM_ASSERT_EQ(result.output, std::string("fd-state-isolated\n"));
}

UAM_TEST(WindowsConPtyDiscardsInheritedOpenSshFdState)
{
	TempDir temp("uam-windows-fd-state-conpty");
	ScopedWideWindowsEnvVar poison(kWindowsPrivateFdStateName, L"AAAAAAICAgA=");
	ScopedWideWindowsEnvVar ordinary(L"UAM_TEST_FD_STATE_ORDINARY", L"inherited");
	UAM_ASSERT(poison.applied && ordinary.applied);
	IPlatformTerminalRuntime& runtime = PlatformServicesFactory::Instance().terminal_runtime;
	for (bool override_ordinary : {false, true})
	{
		const std::string expected = override_ordinary ? "overridden" : "inherited";
		const std::vector<std::pair<std::string, std::string>> overrides = override_ordinary ?
		    std::vector<std::pair<std::string, std::string>>{{"UAM_TEST_FD_STATE_ORDINARY", expected}} : std::vector<std::pair<std::string, std::string>>{};
		uam::CliTerminalState terminal;
		terminal.rows = uam::kCliTerminalDefaultRows;
		terminal.cols = uam::kCliTerminalDefaultCols;
		struct StopOwnedTerminal
		{
			IPlatformTerminalRuntime& runtime;
			uam::CliTerminalState& terminal;
			~StopOwnedTerminal() { runtime.StopCliTerminalProcess(terminal, true); }
		} cleanup{runtime, terminal};
		std::string error;
		UAM_ASSERT(runtime.StartCliTerminalProcess(terminal, temp.root, {uam::paths::Utf8PathString(WindowsFixtureExecutablePath()), kWindowsFdStateFixture, expected}, &error, overrides));
		std::string output;
		std::array<char, 128> buffer{};
		bool exited = false;
		const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < deadline)
		{
			const std::ptrdiff_t bytes = runtime.ReadCliTerminalOutput(terminal, buffer.data(), buffer.size());
			if (bytes > 0) output.append(buffer.data(), static_cast<std::size_t>(bytes));
			exited = runtime.PollCliTerminalProcessExited(terminal);
			if (exited && bytes <= 0) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		AssertWindowsFdStateParentUnchanged();
		UAM_ASSERT(exited);
		UAM_ASSERT(output.find("fd-state-isolated") != std::string::npos);
	}
}
#endif

UAM_TEST(CapturedCommandOutputIsBounded)
{
#if defined(_WIN32)
	const std::vector<std::string> commands = {
	    WindowsFixtureShellCommand({uam::paths::Utf8PathString(WindowsFixtureExecutablePath()), kWindowsOutputFloodFixture, "stdout"}),
	    WindowsFixtureShellCommand({uam::paths::Utf8PathString(WindowsFixtureExecutablePath()), kWindowsOutputFloodFixture, "stderr"})};
#else
	const std::vector<std::string> commands = {"yes bounded-output", "yes bounded-error >&2"};
#endif
	for (const std::string& command : commands)
	{
		const auto started_at = std::chrono::steady_clock::now();
		const ProcessExecutionResult result = PlatformServicesFactory::Instance().process_service.ExecuteCommand(command, 10000);
		const auto elapsed = std::chrono::steady_clock::now() - started_at;

		UAM_ASSERT(result.output_truncated);
		UAM_ASSERT(!result.ok);
		UAM_ASSERT_EQ(result.exit_code, -1);
		UAM_ASSERT_EQ(result.output.size(), uam::platform::kCapturedCommandMaxOutputBytes);
		UAM_ASSERT_EQ(result.error, std::string(uam::platform::kCapturedCommandOutputLimitError));
		UAM_ASSERT(elapsed < std::chrono::seconds(5));
	}
}

UAM_TEST(ConcurrentCapturedCommandsKeepPipeOutputIsolated)
{
#if defined(_WIN32)
	SECURITY_ATTRIBUTES security{};
	security.nLength = sizeof(security);
	security.bInheritHandle = TRUE;
	ScopedWindowsTestHandle unlisted_inheritable_handle(CreateEventW(&security, TRUE, FALSE, nullptr));
	UAM_ASSERT(unlisted_inheritable_handle.IsOpen());
	const std::string handle_value = std::to_string(reinterpret_cast<std::uintptr_t>(unlisted_inheritable_handle.Get()));
	const std::string executable = uam::paths::Utf8PathString(WindowsFixtureExecutablePath());
	const std::string first_command = WindowsFixtureShellCommand({executable, kWindowsHandleProbeFixture, handle_value, "alpha-only"});
	const std::string second_command = WindowsFixtureShellCommand({executable, kWindowsHandleProbeFixture, handle_value, "beta-only"});
#else
	const std::string first_command = "yes alpha-only | head -n 2000";
	const std::string second_command = "yes beta-only | head -n 2000";
#endif
	// Repeat fast exits to exercise the race between the pipe peek and process-exit check.
	for (int iteration = 0; iteration < 16; ++iteration)
	{
		ProcessExecutionResult first;
		ProcessExecutionResult second;
		std::jthread first_thread([&] { first = PlatformServicesFactory::Instance().process_service.ExecuteCommand(first_command, 5000); });
		std::jthread second_thread([&] { second = PlatformServicesFactory::Instance().process_service.ExecuteCommand(second_command, 5000); });
		first_thread.join();
		second_thread.join();
		UAM_ASSERT(first.ok);
		UAM_ASSERT(second.ok);
		UAM_ASSERT(first.output.find("alpha-only") != std::string::npos);
		UAM_ASSERT(first.output.find("beta-only") == std::string::npos);
		UAM_ASSERT(second.output.find("beta-only") != std::string::npos);
		UAM_ASSERT(second.output.find("alpha-only") == std::string::npos);
		UAM_ASSERT_EQ(std::ranges::count(first.output, '\n'), 2000);
		UAM_ASSERT_EQ(std::ranges::count(second.output, '\n'), 2000);
#if defined(_WIN32)
		UAM_ASSERT_EQ(WaitForSingleObject(unlisted_inheritable_handle.Get(), 0), static_cast<DWORD>(WAIT_TIMEOUT));
		UAM_ASSERT(first.output.find(":isolated") != std::string::npos);
		UAM_ASSERT(first.output.find(":inherited") == std::string::npos);
		UAM_ASSERT(second.output.find(":isolated") != std::string::npos);
		UAM_ASSERT(second.output.find(":inherited") == std::string::npos);
#endif
	}
}

#if defined(_WIN32)
UAM_TEST(WindowsFastSpawnerRepeatedStopsKillWholeStdioAndConPtyTrees)
{
	TempDir temp("uam-windows-repeated-tree-stop");
	auto& process_service = PlatformServicesFactory::Instance().process_service;
	auto& terminal_runtime = PlatformServicesFactory::Instance().terminal_runtime;
	for (int iteration = 0; iteration < 16; ++iteration)
	{
		const std::string prefix = "stdio-" + std::to_string(iteration);
		const fs::path ready = temp.root / (prefix + ".ready");
		const fs::path provider_pid = temp.root / (prefix + ".provider.pid");
		const fs::path descendant_pid = temp.root / (prefix + ".descendant.pid");
		const fs::path marker = temp.root / (prefix + ".survived");
		uam::platform::StdioProcessPlatformFields process;
		std::string error;
		UAM_ASSERT(process_service.StartStdioProcess(process, temp.root, WindowsProviderFixtureArgv(ready, provider_pid, descendant_pid, marker, false), &error));
		const WindowsObservedFixtureTree tree = ObserveWindowsFixtureTree(ready, provider_pid, descendant_pid);
		process_service.StopStdioProcess(process, true);
		AssertWindowsFixtureTreeExited(tree);
		UAM_ASSERT(!fs::exists(marker));
	}

	for (int iteration = 0; iteration < 16; ++iteration)
	{
		const std::string prefix = "terminal-" + std::to_string(iteration);
		const fs::path ready = temp.root / (prefix + ".ready");
		const fs::path provider_pid = temp.root / (prefix + ".provider.pid");
		const fs::path descendant_pid = temp.root / (prefix + ".descendant.pid");
		const fs::path marker = temp.root / (prefix + ".survived");
		uam::CliTerminalState terminal;
		terminal.rows = uam::kCliTerminalDefaultRows;
		terminal.cols = uam::kCliTerminalDefaultCols;
		std::string error;
		UAM_ASSERT(terminal_runtime.StartCliTerminalProcess(terminal, temp.root, WindowsProviderFixtureArgv(ready, provider_pid, descendant_pid, marker, false), &error));
		const WindowsObservedFixtureTree tree = ObserveWindowsFixtureTree(ready, provider_pid, descendant_pid);
		terminal_runtime.StopCliTerminalProcess(terminal, true);
		AssertWindowsFixtureTreeExited(tree);
		UAM_ASSERT(!fs::exists(marker));
	}
}

UAM_TEST(WindowsCapturedTimeoutAndCancellationKillWholeTrees)
{
	TempDir temp("uam-windows-captured-tree-stop");
	auto& process_service = PlatformServicesFactory::Instance().process_service;

	const fs::path timeout_ready = temp.root / "timeout.ready";
	const fs::path timeout_provider_pid = temp.root / "timeout.provider.pid";
	const fs::path timeout_descendant_pid = temp.root / "timeout.descendant.pid";
	const fs::path timeout_marker = temp.root / "timeout.survived";
	ProcessExecutionResult timeout_result;
	std::jthread timeout_thread([&]
	{
		timeout_result = process_service.ExecuteCommand(
		    WindowsFixtureShellCommand(WindowsProviderFixtureArgv(timeout_ready, timeout_provider_pid, timeout_descendant_pid, timeout_marker, false)), 2000);
	});
	const WindowsObservedFixtureTree timeout_tree = ObserveWindowsFixtureTree(timeout_ready, timeout_provider_pid, timeout_descendant_pid);
	timeout_thread.join();
	UAM_ASSERT(timeout_result.timed_out);
	AssertWindowsFixtureTreeExited(timeout_tree);
	UAM_ASSERT(!fs::exists(timeout_marker));

	const fs::path cancel_ready = temp.root / "cancel.ready";
	const fs::path cancel_provider_pid = temp.root / "cancel.provider.pid";
	const fs::path cancel_descendant_pid = temp.root / "cancel.descendant.pid";
	const fs::path cancel_marker = temp.root / "cancel.survived";
	std::stop_source stop_source;
	ProcessExecutionResult cancel_result;
	std::jthread cancel_thread([&]
	{
		cancel_result = process_service.ExecuteCommand(
		    WindowsFixtureShellCommand(WindowsProviderFixtureArgv(cancel_ready, cancel_provider_pid, cancel_descendant_pid, cancel_marker, false)), 5000, stop_source.get_token());
	});
	const WindowsObservedFixtureTree cancel_tree = ObserveWindowsFixtureTree(cancel_ready, cancel_provider_pid, cancel_descendant_pid);
	stop_source.request_stop();
	cancel_thread.join();
	UAM_ASSERT(cancel_result.canceled);
	AssertWindowsFixtureTreeExited(cancel_tree);
	UAM_ASSERT(!fs::exists(cancel_marker));
}

UAM_TEST(WindowsProviderCrashCleanupKillsCapturedStdioAndConPtyDescendants)
{
	TempDir temp("uam-windows-provider-crash-tree");
	auto& process_service = PlatformServicesFactory::Instance().process_service;
	auto& terminal_runtime = PlatformServicesFactory::Instance().terminal_runtime;

	const fs::path captured_ready = temp.root / "captured.ready";
	const fs::path captured_provider_pid = temp.root / "captured.provider.pid";
	const fs::path captured_descendant_pid = temp.root / "captured.descendant.pid";
	const fs::path captured_marker = temp.root / "captured.survived";
	ProcessExecutionResult captured_result;
	std::jthread captured_thread([&]
	{
		captured_result = process_service.ExecuteCommand(
		    WindowsFixtureShellCommand(WindowsProviderFixtureArgv(captured_ready, captured_provider_pid, captured_descendant_pid, captured_marker, true)), 5000);
	});
	const WindowsObservedFixtureTree captured_tree = ObserveWindowsFixtureTree(captured_ready, captured_provider_pid, captured_descendant_pid);
	captured_thread.join();
	UAM_ASSERT(!captured_result.timed_out);
	AssertWindowsFixtureTreeExited(captured_tree);

	const fs::path stdio_ready = temp.root / "stdio.ready";
	const fs::path stdio_provider_pid = temp.root / "stdio.provider.pid";
	const fs::path stdio_descendant_pid = temp.root / "stdio.descendant.pid";
	const fs::path stdio_marker = temp.root / "stdio.survived";
	uam::platform::StdioProcessPlatformFields process;
	std::string error;
	UAM_ASSERT(process_service.StartStdioProcess(process, temp.root, WindowsProviderFixtureArgv(stdio_ready, stdio_provider_pid, stdio_descendant_pid, stdio_marker, true), &error));
	const WindowsObservedFixtureTree stdio_tree = ObserveWindowsFixtureTree(stdio_ready, stdio_provider_pid, stdio_descendant_pid);
	bool stdio_exited = false;
	for (int attempt = 0; attempt < 600 && !stdio_exited; ++attempt)
	{
		stdio_exited = process_service.PollStdioProcessExited(process);
		if (!stdio_exited) std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT(stdio_exited);
	process_service.CloseStdioProcessHandles(process);
	AssertWindowsFixtureTreeExited(stdio_tree);

	const fs::path terminal_ready = temp.root / "terminal.ready";
	const fs::path terminal_provider_pid = temp.root / "terminal.provider.pid";
	const fs::path terminal_descendant_pid = temp.root / "terminal.descendant.pid";
	const fs::path terminal_marker = temp.root / "terminal.survived";
	uam::CliTerminalState terminal;
	terminal.rows = uam::kCliTerminalDefaultRows;
	terminal.cols = uam::kCliTerminalDefaultCols;
	UAM_ASSERT(terminal_runtime.StartCliTerminalProcess(terminal, temp.root, WindowsProviderFixtureArgv(terminal_ready, terminal_provider_pid, terminal_descendant_pid, terminal_marker, true), &error));
	const WindowsObservedFixtureTree terminal_tree = ObserveWindowsFixtureTree(terminal_ready, terminal_provider_pid, terminal_descendant_pid);
	bool terminal_exited = false;
	std::array<char, 4096> terminal_output{};
	for (int attempt = 0; attempt < 600 && !terminal_exited; ++attempt)
	{
		UAM_ASSERT(terminal_runtime.ReadCliTerminalOutput(terminal, terminal_output.data(), terminal_output.size()) != -1);
		terminal_exited = terminal_runtime.PollCliTerminalProcessExited(terminal);
		if (!terminal_exited) std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	UAM_ASSERT(terminal_exited);
	terminal_runtime.CloseCliTerminalHandles(terminal);
	AssertWindowsFixtureTreeExited(terminal_tree);
	UAM_ASSERT(!fs::exists(captured_marker));
	UAM_ASSERT(!fs::exists(stdio_marker));
	UAM_ASSERT(!fs::exists(terminal_marker));
}

UAM_TEST(WindowsManagerDeathKillOnCloseKillsCapturedStdioAndConPtyTrees)
{
	TempDir temp("uam-windows-manager-death-tree");
	const std::filesystem::path executable = WindowsFixtureExecutablePath();
	UAM_ASSERT(!executable.empty());
	for (const std::string_view transport : {std::string_view("captured"), std::string_view("stdio"), std::string_view("terminal")})
	{
		const std::string prefix(transport);
		const fs::path ready = temp.root / (prefix + ".ready");
		const fs::path provider_pid = temp.root / (prefix + ".provider.pid");
		const fs::path descendant_pid = temp.root / (prefix + ".descendant.pid");
		const fs::path marker = temp.root / (prefix + ".survived");
		PROCESS_INFORMATION manager{};
		UAM_ASSERT(StartRawWindowsFixture(
		    {executable.wstring(),
		     std::filesystem::path(kWindowsManagerFixture).wstring(),
		     std::filesystem::path(prefix).wstring(),
		     ready.wstring(), provider_pid.wstring(), descendant_pid.wstring(), marker.wstring()},
		    manager));
		CloseHandle(manager.hThread);
		ScopedRawWindowsProcess manager_process(manager.hProcess);
		const WindowsObservedFixtureTree tree = ObserveWindowsFixtureTree(ready, provider_pid, descendant_pid);
		UAM_ASSERT(TerminateProcess(manager_process.Get(), 91) != FALSE);
		UAM_ASSERT_EQ(WaitForSingleObject(manager_process.Get(), 3000), static_cast<DWORD>(WAIT_OBJECT_0));
		AssertWindowsFixtureTreeExited(tree);
		UAM_ASSERT(!fs::exists(marker));
	}
}

UAM_TEST(WindowsFastApplicationShutdownKillsStructuredAndTerminalTrees)
{
	TempDir temp("uam-windows-app-shutdown-tree");
	uam::AppState app;
	app.data_root = temp.root;

	const fs::path stdio_ready = temp.root / "stdio.ready";
	const fs::path stdio_provider_pid = temp.root / "stdio.provider.pid";
	const fs::path stdio_descendant_pid = temp.root / "stdio.descendant.pid";
	const fs::path stdio_marker = temp.root / "stdio.survived";
	auto session = std::make_unique<uam::AcpSessionState>();
	std::string error;
	UAM_ASSERT(PlatformServicesFactory::Instance().process_service.StartStdioProcess(
	    *session, temp.root, WindowsProviderFixtureArgv(stdio_ready, stdio_provider_pid, stdio_descendant_pid, stdio_marker, false), &error));
	const WindowsObservedFixtureTree stdio_tree = ObserveWindowsFixtureTree(stdio_ready, stdio_provider_pid, stdio_descendant_pid);
	app.acp_sessions.push_back(std::move(session));

	const fs::path terminal_ready = temp.root / "terminal.ready";
	const fs::path terminal_provider_pid = temp.root / "terminal.provider.pid";
	const fs::path terminal_descendant_pid = temp.root / "terminal.descendant.pid";
	const fs::path terminal_marker = temp.root / "terminal.survived";
	auto terminal = std::make_unique<uam::CliTerminalState>();
	terminal->rows = uam::kCliTerminalDefaultRows;
	terminal->cols = uam::kCliTerminalDefaultCols;
	UAM_ASSERT(PlatformServicesFactory::Instance().terminal_runtime.StartCliTerminalProcess(
	    *terminal, temp.root, WindowsProviderFixtureArgv(terminal_ready, terminal_provider_pid, terminal_descendant_pid, terminal_marker, false), &error));
	terminal->running = true;
	const WindowsObservedFixtureTree terminal_tree = ObserveWindowsFixtureTree(terminal_ready, terminal_provider_pid, terminal_descendant_pid);
	app.cli_terminals.push_back(std::move(terminal));

	uam::FastStopAcpSessionsForExit(app);
	uam::FastStopCliTerminalsForExit(app);
	AssertWindowsFixtureTreeExited(stdio_tree);
	AssertWindowsFixtureTreeExited(terminal_tree);
	UAM_ASSERT(!fs::exists(stdio_marker));
	UAM_ASSERT(!fs::exists(terminal_marker));
}
#endif

#if !defined(_WIN32)
#if defined(__APPLE__)
UAM_TEST(MacCapturedCommandsUseTerminalFallbackPath)
{
	ScopedEnvVar scoped_path("PATH", "");
	const ProcessExecutionResult result =
	    PlatformServicesFactory::Instance().process_service.ExecuteCommand("command -v sh");
	UAM_ASSERT(result.ok);
	UAM_ASSERT(result.output.find("/bin/sh") != std::string::npos);
}

enum class MacParentDeathFixture
{
	Captured,
	Stdio,
	Terminal,
};

void AssertMacParentDeathKillsOwnedTree(MacParentDeathFixture fixture, const std::string& name)
{
	TempDir temp("uam-mac-parent-death-" + name);
	const fs::path provider_pid_path = temp.root / "provider.pid";
	const fs::path descendant_pid_path = temp.root / "descendant.pid";
	const std::string command = "echo $$ > " + ShellQuoteForTest(provider_pid_path.string()) +
	                            "; trap '' HUP TERM; (trap '' HUP TERM; while :; do sleep 1; done) & descendant=$!; echo $descendant > " +
	                            ShellQuoteForTest(descendant_pid_path.string()) + "; wait";

	const pid_t manager_pid = fork();
	UAM_ASSERT(manager_pid >= 0);
	if (manager_pid == 0)
	{
		std::string error;
		if (fixture == MacParentDeathFixture::Captured)
		{
			(void)PlatformServicesFactory::Instance().process_service.ExecuteCommand(command);
		}
		else if (fixture == MacParentDeathFixture::Stdio)
		{
			uam::platform::StdioProcessPlatformFields process;
			if (!PlatformServicesFactory::Instance().process_service.StartStdioProcess(process, temp.root, {"/bin/sh", "-c", command}, &error))
			{
				_exit(11);
			}
			pause();
		}
		else
		{
			uam::CliTerminalState terminal;
			if (!PlatformServicesFactory::Instance().terminal_runtime.StartCliTerminalProcess(terminal, temp.root, {"/bin/sh", "-c", command}, &error))
			{
				_exit(12);
			}
			pause();
		}
		_exit(13);
	}

	bool ready = false;
	bool manager_reaped = false;
	std::string provider_pid_text;
	std::string descendant_pid_text;
	for (int attempt = 0; attempt < 400; ++attempt)
	{
		if (fs::exists(provider_pid_path) && fs::exists(descendant_pid_path))
		{
			provider_pid_text = uam::io::ReadTextFile(provider_pid_path);
			descendant_pid_text = uam::io::ReadTextFile(descendant_pid_path);
			if (!provider_pid_text.empty() && !descendant_pid_text.empty())
			{
				ready = true;
				break;
			}
		}
		int manager_status = 0;
		if (waitpid(manager_pid, &manager_status, WNOHANG) == manager_pid)
		{
			manager_reaped = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}

	pid_t provider_pid = -1;
	pid_t descendant_pid = -1;
	if (ready)
	{
		provider_pid = static_cast<pid_t>(std::stoll(provider_pid_text));
		descendant_pid = static_cast<pid_t>(std::stoll(descendant_pid_text));
	}
	if (!manager_reaped)
	{
		(void)kill(manager_pid, SIGKILL);
		int manager_status = 0;
		while (waitpid(manager_pid, &manager_status, 0) < 0 && errno == EINTR)
		{
		}
	}
	UAM_ASSERT(ready);

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	bool provider_gone = false;
	bool descendant_gone = false;
	while (std::chrono::steady_clock::now() < deadline)
	{
		errno = 0;
		provider_gone = kill(provider_pid, 0) != 0 && errno == ESRCH;
		errno = 0;
		descendant_gone = kill(descendant_pid, 0) != 0 && errno == ESRCH;
		if (provider_gone && descendant_gone)
		{
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(provider_gone);
	UAM_ASSERT(descendant_gone);

	const pid_t replacement_manager = fork();
	UAM_ASSERT(replacement_manager >= 0);
	if (replacement_manager == 0)
	{
		const ProcessExecutionResult replacement = PlatformServicesFactory::Instance().process_service.ExecuteCommand("exit 0", 1000);
		_exit(replacement.ok ? 0 : 21);
	}
	int replacement_status = 0;
	while (waitpid(replacement_manager, &replacement_status, 0) < 0 && errno == EINTR)
	{
	}
	UAM_ASSERT(WIFEXITED(replacement_status));
	UAM_ASSERT_EQ(WEXITSTATUS(replacement_status), 0);
}

UAM_TEST(MacCapturedCommandParentDeathKillsOwnedTree)
{
	AssertMacParentDeathKillsOwnedTree(MacParentDeathFixture::Captured, "captured");
}

UAM_TEST(MacStdioParentDeathKillsOwnedTree)
{
	AssertMacParentDeathKillsOwnedTree(MacParentDeathFixture::Stdio, "stdio");
}

UAM_TEST(MacTerminalParentDeathKillsOwnedTree)
{
	AssertMacParentDeathKillsOwnedTree(MacParentDeathFixture::Terminal, "terminal");
}

UAM_TEST(MacParentDeathWatchdogDoesNotRetainDataRootLock)
{
	TempDir temp("uam-mac-watchdog-data-lock");
	const fs::path data_root = temp.root / "data";
	const fs::path provider_pid_path = temp.root / "provider.pid";
	const fs::path ready_path = temp.root / "ready";
	const pid_t manager_pid = fork();
	UAM_ASSERT(manager_pid >= 0);
	if (manager_pid == 0)
	{
		std::string error;
		auto lock = PlatformServicesFactory::Instance().process_service.TryAcquireDataRootLock(data_root, &error);
		if (lock == nullptr)
		{
			_exit(31);
		}
		uam::platform::StdioProcessPlatformFields process;
		const std::string command = "echo $$ > " + ShellQuoteForTest(provider_pid_path.string()) + "; trap '' HUP TERM; while :; do sleep 1; done";
		if (!PlatformServicesFactory::Instance().process_service.StartStdioProcess(process, temp.root, {"/bin/sh", "-c", command}, &error))
		{
			_exit(32);
		}
		lock.reset();
		(void)uam::io::WriteTextFile(ready_path, "ready");
		pause();
		_exit(33);
	}

	std::string provider_pid_text;
	for (int attempt = 0; attempt < 400; ++attempt)
	{
		if (fs::exists(ready_path) && fs::exists(provider_pid_path))
		{
			provider_pid_text = uam::io::ReadTextFile(provider_pid_path);
			if (!provider_pid_text.empty())
			{
				break;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	const bool ready = fs::exists(ready_path) && !provider_pid_text.empty();
	const pid_t provider_pid = ready ? static_cast<pid_t>(std::stoll(provider_pid_text)) : -1;
	std::string lock_error;
	auto replacement_lock = PlatformServicesFactory::Instance().process_service.TryAcquireDataRootLock(data_root, &lock_error);
	const bool lock_acquired = replacement_lock != nullptr;
	replacement_lock.reset();
	(void)kill(manager_pid, SIGKILL);
	int manager_status = 0;
	while (waitpid(manager_pid, &manager_status, 0) < 0 && errno == EINTR)
	{
	}
	bool provider_gone = false;
	for (int attempt = 0; ready && attempt < 300; ++attempt)
	{
		errno = 0;
		provider_gone = kill(provider_pid, 0) != 0 && errno == ESRCH;
		if (provider_gone)
		{
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(ready);
	UAM_ASSERT(lock_acquired);
	UAM_ASSERT(provider_gone);
}
#endif

UAM_TEST(MacCapturedCommandTimeoutStopsDescendantProcesses)
{
	TempDir temp("uam-mac-command-tree-timeout");
	const fs::path marker = temp.root / "descendant-ran.txt";
	const std::string command = "(sleep 0.3; touch " + ShellQuoteForTest(marker.string()) + ") & wait";

	const ProcessExecutionResult result = PlatformServicesFactory::Instance().process_service.ExecuteCommand(command, 50);
	UAM_ASSERT(result.timed_out);
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	UAM_ASSERT(!fs::exists(marker));
}

UAM_TEST(MacCapturedCommandTimeoutEscalatesPastIgnoredSigterm)
{
	const auto started_at = std::chrono::steady_clock::now();
	const ProcessExecutionResult result = PlatformServicesFactory::Instance().process_service.ExecuteCommand("trap '' TERM; while :; do sleep 1; done", 50);
	const auto elapsed = std::chrono::steady_clock::now() - started_at;

	UAM_ASSERT(result.timed_out);
	UAM_ASSERT(!result.ok);
	UAM_ASSERT(elapsed < std::chrono::seconds(4));
}

UAM_TEST(MacCapturedCommandCancellationEscalatesPastIgnoredSigterm)
{
	std::stop_source stop_source;
	std::jthread canceler(
	    [&stop_source]
	    {
		    std::this_thread::sleep_for(std::chrono::milliseconds(50));
		    stop_source.request_stop();
	    });
	const auto started_at = std::chrono::steady_clock::now();
	const ProcessExecutionResult result = PlatformServicesFactory::Instance().process_service.ExecuteCommand("trap '' TERM; while :; do sleep 1; done", -1, stop_source.get_token());
	const auto elapsed = std::chrono::steady_clock::now() - started_at;

	UAM_ASSERT(result.canceled);
	UAM_ASSERT(!result.ok);
	UAM_ASSERT(elapsed < std::chrono::seconds(4));
}

UAM_TEST(MacStoppingStdioProcessStopsDescendantProcesses)
{
	TempDir temp("uam-mac-stdio-tree-stop");
	const fs::path marker = temp.root / "descendant-ran.txt";
	uam::platform::StdioProcessPlatformFields process;
	std::string error;
	auto& process_service = PlatformServicesFactory::Instance().process_service;
	UAM_ASSERT(process_service.StartStdioProcess(process, temp.root, {"/bin/sh", "-c", "(sleep 0.3; touch " + ShellQuoteForTest(marker.string()) + ") & wait"}, &error));

	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	process_service.StopStdioProcess(process, true);
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	UAM_ASSERT(!fs::exists(marker));
}

UAM_TEST(MacWritingToClosedStdioPipeDoesNotTerminateCaller)
{
	TempDir temp("uam-mac-closed-stdio");
	uam::platform::StdioProcessPlatformFields process;
	std::string error;
	auto& process_service = PlatformServicesFactory::Instance().process_service;
	UAM_ASSERT(process_service.StartStdioProcess(process, temp.root, {"/bin/sh", "-c", "exit 0"}, &error));

	int exit_code = -1;
	for (int attempt = 0; attempt < 100 && !process_service.PollStdioProcessExited(process, &exit_code); ++attempt)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT_EQ(exit_code, 0);

	const pid_t tester = fork();
	if (tester == 0)
	{
		std::signal(SIGPIPE, SIG_DFL);
		std::string write_error;
		const bool wrote = process_service.WriteToStdioProcess(process, "x", 1, &write_error);
		bool failed = false;
		for (int attempt = 0; attempt < 100 && !failed; ++attempt)
		{
			failed = process.stdin_writer != nullptr && process.stdin_writer->FailureOrStall(&write_error);
			if (!failed)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
		}
		process.stdin_writer.reset();
		_exit(wrote && failed && !write_error.empty() ? 0 : 1);
	}
	UAM_ASSERT(tester > 0);

	int status = 0;
	UAM_ASSERT_EQ(waitpid(tester, &status, 0), tester);
	process_service.CloseStdioProcessHandles(process);
	UAM_ASSERT(WIFEXITED(status));
	UAM_ASSERT_EQ(WEXITSTATUS(status), 0);
}
#endif

UAM_TEST(ProviderWorkerDirectExecutionHonorsTimeoutAndCancellation)
{
	TempDir temp("uam-provider-worker-direct-stop");
	uam::ProviderWorkerInvocation invocation;
	invocation.direct_process = true;
#if defined(_WIN32)
	const fs::path shim_path = temp.root / "uam-slow-worker.cmd";
	UAM_ASSERT(uam::io::WriteTextFile(shim_path, "@echo off\r\necho started\r\n:noise\r\necho noisy-output\r\ngoto noise\r\n"));
	invocation.argv = {uam::paths::Utf8PathString(shim_path)};
	const std::string input_prefix = "uam-provider-worker-stdin-" + std::to_string(GetCurrentProcessId()) + "-";
	const auto count_worker_input_files = [&]()
	{
		std::error_code error;
		const fs::path temp_directory = fs::temp_directory_path(error);
		std::size_t count = 0;
		for (fs::directory_iterator it(temp_directory, error), end; !error && it != end; it.increment(error))
		{
			if (it->path().filename().string().rfind(input_prefix, 0) == 0)
			{
				++count;
			}
		}
		return count;
	};
	const std::size_t input_files_before = count_worker_input_files();
#else
	invocation.argv = {"/bin/sh", "-c", "printf 'started\\n'; sleep 5"};
#endif
	invocation.standard_input.assign(1024 * 1024, 'p');
	invocation.command_preview = "uam-slow-worker";

	const ProcessExecutionResult timed_out = uam::ExecuteProviderWorkerInvocation(invocation, temp.root, 250);
	UAM_ASSERT(timed_out.timed_out);
	UAM_ASSERT(!timed_out.canceled);
	UAM_ASSERT_EQ(timed_out.exit_code, -1);
	UAM_ASSERT_EQ(timed_out.error, std::string("Command timed out."));
	UAM_ASSERT(timed_out.output.find("started") != std::string::npos);

	std::stop_source stop_source;
	stop_source.request_stop();
	const ProcessExecutionResult canceled = uam::ExecuteProviderWorkerInvocation(invocation, temp.root, -1, stop_source.get_token());
	UAM_ASSERT(canceled.canceled);
	UAM_ASSERT(!canceled.timed_out);
	UAM_ASSERT_EQ(canceled.exit_code, -1);
	UAM_ASSERT_EQ(canceled.error, std::string("Command canceled."));
#if defined(_WIN32)
	uam::ProviderWorkerInvocation missing_worker = invocation;
	missing_worker.argv = {"uam-provider-worker-command-that-does-not-exist-7f4938d1"};
	const ProcessExecutionResult launch_failed = uam::ExecuteProviderWorkerInvocation(missing_worker, temp.root, 250);
	UAM_ASSERT(!launch_failed.ok);
	UAM_ASSERT(!launch_failed.error.empty());
	UAM_ASSERT_EQ(count_worker_input_files(), input_files_before);
#endif
}

#if defined(_WIN32)
UAM_TEST(WindowsProviderWorkerDirectLaunchesPathCmdShimWithLongStdin)
{
	TempDir temp("uam-win-cmd-shim");
	const fs::path shim_path = temp.root / "uam-fake-cli.cmd";
	UAM_ASSERT(uam::io::WriteTextFile(shim_path, "@echo off\r\necho shim:%~1:%~2\r\necho stderr-marker 1>&2\r\necho stdin-begin\r\nmore\r\necho stdin-end\r\n"));

	const char* existing_path = std::getenv("PATH");
	const std::string combined_path = temp.root.string() + (existing_path == nullptr ? "" : (";" + std::string(existing_path)));
	ScopedEnvVar scoped_path("PATH", combined_path);

	uam::ProviderWorkerInvocation invocation;
	invocation.direct_process = true;
	invocation.argv = {"uam-fake-cli", "hello world", "tail"};
	invocation.standard_input = std::string(12000, 'z');
	invocation.command_preview = "uam-fake-cli <prompt via stdin>";

	const ProcessExecutionResult result = uam::ExecuteProviderWorkerInvocation(invocation, temp.root, 5000);
	UAM_ASSERT(result.ok);
	UAM_ASSERT_EQ(result.exit_code, 0);
	UAM_ASSERT(result.output.find("shim:hello world:tail") != std::string::npos);
	UAM_ASSERT(result.output.find("stderr-marker") != std::string::npos);
	UAM_ASSERT(result.output.find("stdin-begin") != std::string::npos);
	UAM_ASSERT(result.output.find("stdin-end") != std::string::npos);
	UAM_ASSERT(std::ranges::count(result.output, 'z') >= static_cast<std::ptrdiff_t>(12000));
	const std::size_t stdout_position = result.output.find("shim:hello world:tail");
	const std::size_t stderr_position = result.output.find("stderr-marker");
	const std::size_t stdin_position = result.output.find("stdin-begin");
	UAM_ASSERT(stdout_position < stderr_position);
	UAM_ASSERT(stderr_position < stdin_position);
}

UAM_TEST(WindowsTerminalLaunchesCopilotCmdShimFromUnicodePathWithSessionId)
{
	TempDir temp("uam-win-copilot-terminal-shim");
	const fs::path unicode_bin = temp.root / uam::paths::PathFromUtf8("copilot-\xE2\x98\x83-bin");
	fs::create_directories(unicode_bin);
	const fs::path shim_path = unicode_bin / "copilot.cmd";
	UAM_ASSERT(uam::io::WriteTextFile(shim_path, "@echo off\r\nset /p \"UAM_INPUT=\"\r\necho copilot:%~1:%~2:%UAM_INPUT%\r\n"));

	const DWORD existing_path_size = GetEnvironmentVariableW(L"PATH", nullptr, 0);
	std::wstring existing_path(static_cast<std::size_t>(existing_path_size), L'\0');
	if (existing_path_size > 0)
	{
		const DWORD written = GetEnvironmentVariableW(L"PATH", existing_path.data(), existing_path_size);
		UAM_ASSERT(written > 0 && written < existing_path_size);
		existing_path.resize(static_cast<std::size_t>(written));
	}
	const std::wstring combined_path = unicode_bin.wstring() + (existing_path.empty() ? std::wstring() : L";" + existing_path);
	ScopedWideWindowsEnvVar scoped_path(L"PATH", combined_path);
	UAM_ASSERT(scoped_path.applied);

	uam::CliTerminalState terminal;
	terminal.rows = uam::kCliTerminalDefaultRows;
	terminal.cols = uam::kCliTerminalDefaultCols;
	const std::string session_id = "7b7f1032-0e41-4af2-9cd9-222222222222";
	std::string error;
	auto& terminal_runtime = PlatformServicesFactory::Instance().terminal_runtime;
	UAM_ASSERT(terminal_runtime.StartCliTerminalProcess(terminal, temp.root, {"copilot", "--session-id", session_id}, &error));
	UAM_ASSERT(error.empty());
	static constexpr char kInput[] = "hello\r\n";
	UAM_ASSERT(terminal_runtime.WriteToCliTerminal(terminal, kInput, sizeof(kInput) - 1));

	std::string output;
	std::array<char, 512> buffer{};
	bool exited = false;
	for (int attempt = 0; attempt < 500; ++attempt)
	{
		const std::ptrdiff_t bytes = terminal_runtime.ReadCliTerminalOutput(terminal, buffer.data(), buffer.size());
		UAM_ASSERT(bytes != -1);
		if (bytes > 0)
		{
			output.append(buffer.data(), static_cast<std::size_t>(bytes));
		}
		if (terminal_runtime.PollCliTerminalProcessExited(terminal))
		{
			exited = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	if (exited)
	{
		terminal_runtime.CloseCliTerminalHandles(terminal);
	}
	else
	{
		terminal_runtime.StopCliTerminalProcess(terminal, true);
	}
	UAM_ASSERT(exited);
	UAM_ASSERT(output.find("copilot:--session-id:" + session_id + ":hello") != std::string::npos);
}

UAM_TEST(WindowsCopilotVersionProbeHandlesUnicodeWinGetShimPath)
{
	TempDir temp("uam-win-copilot-version-shim");
	const fs::path shim_bin = temp.root / uam::paths::PathFromUtf8("Jos\xC3\xA9") / "Microsoft" / "WinGet" / "Links";
	fs::create_directories(shim_bin);
	UAM_ASSERT(uam::io::WriteTextFile(shim_bin / "copilot.cmd", "@echo off\r\necho GitHub Copilot CLI 1.0.75\r\n"));
	// The version probe checks pwsh availability but executes only the Copilot shim.
	UAM_ASSERT(uam::io::WriteTextFile(shim_bin / "pwsh.cmd", "@echo off\r\nexit /b 93\r\n"));

	const DWORD existing_path_size = GetEnvironmentVariableW(L"PATH", nullptr, 0);
	std::wstring existing_path(static_cast<std::size_t>(existing_path_size), L'\0');
	if (existing_path_size > 0)
	{
		const DWORD written = GetEnvironmentVariableW(L"PATH", existing_path.data(), existing_path_size);
		UAM_ASSERT(written > 0 && written < existing_path_size);
		existing_path.resize(static_cast<std::size_t>(written));
	}
	const std::wstring combined_path = shim_bin.wstring() + (existing_path.empty() ? std::wstring() : L";" + existing_path);
	ScopedWideWindowsEnvVar scoped_path(L"PATH", combined_path);
	UAM_ASSERT(scoped_path.applied);

	uam::AppState app;
	app.provider_profiles = ProviderProfileStore::BuiltInProfiles();
	ProviderCliCompatibilityService service;
	std::string check_error;
	UAM_ASSERT(service.StartProviderVersionCheck(app, uam::provider_ids::kCopilotCli, true, "local", &check_error));
	UAM_ASSERT(check_error.empty());
	// The asynchronous PowerShell/npm/CIM probe has a 30-second production budget.
	const std::chrono::steady_clock::time_point check_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(35);
	while (!app.runtime_cli_versions_by_provider_id[uam::provider_ids::kCopilotCli].checked &&
	       std::chrono::steady_clock::now() < check_deadline)
	{
		service.Poll(app);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	const uam::CliProviderVersionState& state = app.runtime_cli_versions_by_provider_id.at(uam::provider_ids::kCopilotCli);
	UAM_ASSERT(state.checked);
	UAM_ASSERT(state.supported);
	UAM_ASSERT_EQ(state.installed_version, std::string("1.0.75"));
	UAM_ASSERT_EQ(state.install_method, std::string("winget"));
}

UAM_TEST(WindowsLaunchesAppendRegistryPathMissingFromStaleProcessPath)
{
	// A helper started before a CLI install keeps its old PATH; System32 stands in for the newly added registry entry.
	TempDir temp("uam-win-stale-path");
	const auto expect_registry_path_child = [&](const ProcessExecutionResult& result)
	{
		UAM_ASSERT(result.ok);
		UAM_ASSERT_EQ(result.exit_code, 0);
		UAM_ASSERT(uam::strings::ContainsCaseInsensitive(result.output, "\\cmd.exe"));
		std::wstring path(32767, L'\0');
		path.resize(GetEnvironmentVariableW(L"PATH", path.data(), static_cast<DWORD>(path.size())));
		UAM_ASSERT(path.starts_with(temp.root.wstring() + L";"));
	};
	{
		ScopedEnvVar stale_path("PATH", temp.root.string());
		expect_registry_path_child(PlatformServicesFactory::Instance().process_service.ExecuteCommand("where.exe cmd", 10000));
	}
	{
		ScopedEnvVar stale_path("PATH", temp.root.string());
		uam::ProviderWorkerInvocation invocation;
		invocation.direct_process = true;
		invocation.argv = {"where.exe", "cmd"};
		invocation.command_preview = "where.exe cmd";
		expect_registry_path_child(uam::ExecuteProviderWorkerInvocation(invocation, temp.root, 10000));
	}
}

#endif

#if defined(__APPLE__)
UAM_TEST(MacTerminalRejectsInvalidWorkingDirectory)
{
	TempDir temp("uam-mac-terminal-workdir");
	const fs::path file_path = temp.root / "not-a-directory";
	UAM_ASSERT(uam::io::WriteTextFile(file_path, "not a directory"));

	uam::CliTerminalState terminal;
	terminal.rows = uam::kCliTerminalDefaultRows;
	terminal.cols = uam::kCliTerminalDefaultCols;
	std::string error;
	const bool started = PlatformServicesFactory::Instance().terminal_runtime.StartCliTerminalProcess(terminal, file_path, {"/bin/echo", "hello"}, &error);
	if (started)
	{
		PlatformServicesFactory::Instance().terminal_runtime.StopCliTerminalProcess(terminal, true);
		PlatformServicesFactory::Instance().terminal_runtime.CloseCliTerminalHandles(terminal);
	}
	UAM_ASSERT(!started);
	UAM_ASSERT(!error.empty());
}

UAM_TEST(MacTerminalFastStopTerminatesProcessGroupChildren)
{
	TempDir temp("uam-mac-terminal-process-group");
	uam::CliTerminalState terminal;
	terminal.rows = uam::kCliTerminalDefaultRows;
	terminal.cols = uam::kCliTerminalDefaultCols;
	std::string error;
	const bool started = PlatformServicesFactory::Instance().terminal_runtime.StartCliTerminalProcess(terminal, temp.root, {"/bin/sh", "-c", "sleep 30"}, &error);
	UAM_ASSERT(started);
	UAM_ASSERT(terminal.child_pid > 0);
	const pid_t process_group_id = terminal.child_pid;
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	PlatformServicesFactory::Instance().terminal_runtime.StopCliTerminalProcess(terminal, true);
	PlatformServicesFactory::Instance().terminal_runtime.CloseCliTerminalHandles(terminal);
	UAM_ASSERT_EQ(terminal.child_pid, static_cast<pid_t>(-1));

	bool process_group_gone = false;
	for (int attempt = 0; attempt < 20; ++attempt)
	{
		errno = 0;
		if (kill(-process_group_id, 0) != 0 && errno == ESRCH)
		{
			process_group_gone = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
	}

	UAM_ASSERT(process_group_gone);
}
#endif

std::optional<int> RunRemoteMcpLostAckFixture(int argc, char** argv);
std::optional<int> RunRemoteOpenCodeCreateFixture(int argc, char** argv);
std::optional<int> RunRemoteAttachmentFixture(int argc, char** argv);
std::optional<int> RunNativeRemoteHistoryReleaseFixture(int argc, char** argv);
std::optional<int> RunRemoteEmptyArgumentsFixture(int argc, char** argv);
std::optional<int> RunRemoteOldStartupEchoFixture(int argc, char** argv);
std::optional<int> RunRemoteWorktreeReceiptFixture(int argc, char** argv);
std::optional<int> RunRemoteCodexSetupFixture(int argc, char** argv);
std::optional<int> RunEmptyNativeImportFixture(int argc, char** argv);
std::optional<int> RunOpenCodeSessionCreateFixtureIfRequested(int argc, char* argv[]);

#if defined(__APPLE__)
std::optional<int> RunMacTerminalSignalFixtureIfRequested(int argc, char* argv[]);
#endif

int main(int argc, char** argv)
{
#if defined(_WIN32)
	if ((argc == 2 || argc == 3) && std::string_view(argv[1]) == "--uam-test-stdio-cat")
	{
		// Paired with uam_test::StdioCatArgv: copy stdin unchanged until EOF.
		const HANDLE output = argc == 3
		    ? CreateFileW(std::filesystem::path(argv[2]).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)
		    : GetStdHandle(STD_OUTPUT_HANDLE);
		if (output == INVALID_HANDLE_VALUE) return 1;
		char buffer[8192];
		DWORD count = 0, written = 0;
		while (::ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer), &count, nullptr) && count > 0)
			if (!::WriteFile(output, buffer, count, &written, nullptr) || written != count) return 1;
		return 0;
	}
#endif
	// Every ordinary test starts outside the developer's native Antigravity store.
	// Individual fixtures may override these only with their own synthetic roots.
	uam_test::ScopedNativeHistoryHome native_history_home;
	if (const std::optional<int> result = RunRemoteEmptyArgumentsFixture(argc, argv)) return *result;
	if (const std::optional<int> result = RunRemoteOldStartupEchoFixture(argc, argv)) return *result;
	if (const std::optional<int> result = RunRemoteWorktreeReceiptFixture(argc, argv)) return *result;
	if (const std::optional<int> result = RunRemoteCodexSetupFixture(argc, argv)) return *result;
	if (const std::optional<int> result = RunEmptyNativeImportFixture(argc, argv)) return *result;
	if (const std::optional<int> result = RunRemoteOpenCodeCreateFixture(argc, argv)) return *result;
	if (const std::optional<int> result = RunRemoteAttachmentFixture(argc, argv)) return *result;
	if (const std::optional<int> result = RunNativeRemoteHistoryReleaseFixture(argc, argv)) return *result;
	if (const std::optional<int> result = RunOpenCodeSessionCreateFixtureIfRequested(argc, argv)) return *result;
	if (const std::optional<int> result = RunRemoteMcpLostAckFixture(argc, argv)) return *result;
#if !defined(_WIN32)
	if ((argc == 3 || argc == 4 || (argc == 5 && (std::string_view(argv[4]) == "--without-central-config" || std::string_view(argv[4]).starts_with("--reject-config-start=")))) && std::string_view(argv[1]) == "--uam-test-runner-result")
	{
		const std::string argument = argv[2];
		const nlohmann::json fixture_result = nlohmann::json::parse(argument.starts_with('@') ? uam_test::ReadFile(uam::paths::PathFromUtf8(argument.substr(1))) : argument, nullptr, false);
		if (fixture_result.is_discarded()) return 2;
		const bool private_chunked = fixture_result.is_object() && fixture_result.value("privateChunkedOutput", false);
		std::string chunked_output;
		if (private_chunked && !uam::base64::Decode(fixture_result.value("stdoutBase64", ""), chunked_output)) return 2;
		std::size_t chunked_offset = 0;
		uam::remote::RunnerState state;
		nlohmann::json request;
		while (uam::remote::ReadFrame(std::cin, request) == uam::remote::FrameReadResult::Ok)
		{
			if (argc >= 4)
			{
				std::ofstream log(argv[3], std::ios::app);
				log << request.value("type", "") << ' ' << request.value("sessionId", "") << std::endl;
				if (request.value("type", "") == "process.poll" && !(argc == 5 && std::string_view(argv[4]).starts_with("--reject-config-start=")))
				{
					std::this_thread::sleep_for(std::chrono::seconds(5));
					return 0;
				}
			}
			nlohmann::json response = request.value("type", "") == "hello"
			    ? uam::remote::HandleRunnerRequest(request, "test", &state)
			    : nlohmann::json{{"id", request["id"]}, {"ok", true},
			                     {"result", fixture_result}};
			if (private_chunked && request.value("type", "") != "hello")
			{
				response["result"] = nlohmann::json::object();
				if (request.value("type", "") == "process.poll")
				{
					const std::string_view chunk = std::string_view(chunked_output).substr(chunked_offset, 65536);
					chunked_offset += chunk.size();
					response["result"] = {{"running", chunked_offset < chunked_output.size()}, {"exitCode", fixture_result.value("exitCode", 0)},
					    {"stdoutBase64", uam::base64::Encode(chunk)}, {"stderrBase64", ""}, {"stdoutCursor", chunked_offset}, {"stderrCursor", std::uint64_t{0}}};
				}
			}
			if (argc == 5 && std::string_view(argv[4]) == "--without-central-config" && request.value("type", "") == "hello")
				response["capabilities"]["centralProviderConfiguration"] = false;
			if (argc == 5 && std::string_view(argv[4]).starts_with("--reject-config-start=") && request.value("type", "") == "process.start")
				response = {{"id", request["id"]}, {"ok", false}, {"error", {{"code", std::string(argv[4]).substr(std::string_view("--reject-config-start=").size())}, {"message", "Central resource folder does not belong to this workspace."}}}};
			if (argc == 5 && std::string_view(argv[4]).starts_with("--reject-config-start=") && request.value("type", "") == "process.poll")
			{
				const std::string outcome = fixture_result.value("pollOutcome", "missing");
				if (outcome == "disconnect") return 0;
				response = outcome == "existing"
				    ? nlohmann::json{{"id", request["id"]}, {"ok", true}, {"result", {{"running", true}}}}
				    : nlohmann::json{{"id", request["id"]}, {"ok", false}, {"error", {{"code", outcome == "foreign" ? "unauthorized" : "session_not_found"}, {"message", "Fixture process ownership result."}}}};
			}
			if (!uam::remote::WriteFrame(std::cout, response)) return 1;
			std::cout.flush();
		}
		return 0;
	}
#endif
	if (argc == 2 && std::string_view(argv[1]) == "--uam-test-control-mcp")
		return uam::UamControlService::RunStdioServerFromEnvironment();
#if defined(__APPLE__)
	if (const std::optional<int> fixture_exit = RunMacTerminalSignalFixtureIfRequested(argc, argv); fixture_exit.has_value())
	{
		return *fixture_exit;
	}
	if (const std::optional<int> terminal_exit = uam::platform::RunMacTerminalChildIfRequested(argc, argv); terminal_exit.has_value())
	{
		return *terminal_exit;
	}
	if (const std::optional<int> watchdog_result = uam::platform::RunMacParentDeathWatchdogIfRequested(argc, argv); watchdog_result.has_value())
	{
		return *watchdog_result;
	}
#endif
#if defined(_WIN32)
	if (const std::optional<int> fixture_result = RunWindowsContainmentFixtureMode(argc, argv); fixture_result.has_value())
	{
		return *fixture_result;
	}
#endif
	// Fixture repositories must check out the bytes they commit. Git for Windows ships a system
	// core.autocrlf=true, which turns committed "x\n" into "x\r\n" and adds LF/CRLF warnings to
	// captured output; product-owned repositories already pin this per command.
	const ScopedEnvVar git_line_endings("GIT_CONFIG_PARAMETERS", "'core.autocrlf=false'");
	int failures = 0;
	if (argc == 2 && std::string_view(argv[1]) == "--list-tests")
	{
		for (const TestCase& test : Registry()) std::cout << test.name << '\n';
		return 0;
	}
	int executed = 0;
	const bool report_timing = uam::env::GetNonEmptyString("UAM_TEST_TIMINGS").has_value();
	const std::string test_filter =
	    uam::env::GetNonEmptyString("UAM_TEST_FILTER").value_or("");
	for (const TestCase& test : Registry())
	{
		bool selected = argc <= 1 &&
		                (test_filter.empty() || test.name.find(test_filter) != std::string::npos);
		for (int index = 1; !selected && index < argc; ++index) selected = test.name == argv[index];
		if (!selected)
		{
			continue;
		}
		++executed;
		const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
		try
		{
			test.fn();
			std::cout << "[PASS] " << test.name << '\n';
		}
		catch (const std::exception& ex)
		{
			++failures;
			std::cerr << "[FAIL] " << test.name << ": " << ex.what() << '\n';
		}
		if (report_timing)
		{
			const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
			std::cerr << "[TIME] " << test.name << ": " << milliseconds << " ms\n";
		}
	}
	if (executed == 0)
	{
		std::cerr << "No tests matched the requested selection.\n";
		return 2;
	}

	if (failures != 0)
	{
		std::cerr << failures << " test(s) failed.\n";
		return 1;
	}

	return 0;
}
